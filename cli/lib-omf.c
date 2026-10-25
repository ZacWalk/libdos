/* libdos by Zac Walker
 *
 * A Microsoft OMF library manager: creates .LIB files from .OBJ files and adds,
 * replaces and removes modules, as LIB.EXE does. demu runs it in place of
 * LIB.EXE, which is an OS/2 bound executable that cannot run emulated. Moved
 * here from the demu of dos4z, whose MS-DOS 4.00 build uses it.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#include <ctype.h>
#include <windows.h>

#include "lib-omf.h"

int omf_lib_verbose = 0;
int (*omf_lib_map)(const char *name, char *host_path, int size) = NULL;

/* What went wrong in this run: a file not read or written, or more modules
 * or symbols than the tables hold. Any of them makes LIB's exit code 1. */
static int failures = 0;

/* Opens a file by the name LIB was given, through the host's mapping when
 * there is one; a name it refuses is not opened. */
static FILE *libOpen(const char *name, const char *mode)
{
    char host[MAX_PATH];
    if (omf_lib_map == NULL) return fopen(name, mode);
    if (!omf_lib_map(name, host, (int)sizeof(host))) return NULL;
    return fopen(host, mode);
}

/* ── OMF constants ─────────────────────────────────────────────────── */
#define LIB_HEADER  0xF0
#define LIB_END     0xF1
#define THEADR      0x80
#define MODEND      0x8A
#define PUBDEF      0x90
#define PUBDEF32    0x91
#define PAGE_SIZE   512  /* standard MS LIB page size */
#define DICT_BUCKET 37

/* ── OMF record reading ──────────────────────────────────────────────── */

static int omfRecordType(const unsigned char *p) { return p[0]; }
static int omfRecordLen(const unsigned char *p) { return p[1] | (p[2] << 8); }
static int omfRecordTotal(const unsigned char *p) { return 3 + omfRecordLen(p); }

/* ── Symbol collection ───────────────────────────────────────────────── */

#define MAX_SYMBOLS 4096
#define MAX_SYM_LEN 256

struct libSymbol {
    char name[MAX_SYM_LEN];
    int  modulePage;  /* page number in library */
};

struct libModule {
    unsigned char *data;
    int size;
    int page;         /* page number in library where this module starts */
    char name[MAX_PATH];
};

#define MAX_MODULES 256
static struct libModule modules[MAX_MODULES];
static int moduleCount = 0;
static struct libSymbol symbols[MAX_SYMBOLS];
static int symbolCount = 0;
static int symbolsDropped = 0; /* public symbols beyond MAX_SYMBOLS */

/* Extract public symbols from a module's OMF records */
static void extractPublics(const unsigned char *data, int size, int modulePage)
{
    int offset = 0;
    while (offset + 3 <= size) {
        int type = omfRecordType(data + offset);
        int rlen = omfRecordLen(data + offset);
        int total = rlen + 3;
        if (offset + total > size) break;

        if (type == PUBDEF || type == PUBDEF32) {
            /* PUBDEF record: group_idx(1-2), seg_idx(1-2), [base_frame(2)], then names */
            const unsigned char *body = data + offset + 3;
            int blen = rlen - 1; /* minus checksum byte */
            int i = 0;
            /* group index */
            int grpIdx = body[i++];
            if (grpIdx >= 0x80) i++; /* 2-byte index */
            /* segment index */
            int segIdx = body[i++];
            if (segIdx >= 0x80) i++;
            /* if segment index is 0, there's a base frame word */
            if (segIdx == 0 || (segIdx >= 0x80 && ((segIdx & 0x7F) << 8) == 0))
                i += 2;
            /* now read name entries */
            while (i < blen) {
                int nameLen = body[i++];
                if (nameLen == 0 || i + nameLen > blen) break;
                if (symbolCount < MAX_SYMBOLS) {
                    int copyLen = nameLen < MAX_SYM_LEN - 1 ? nameLen : MAX_SYM_LEN - 1;
                    memcpy(symbols[symbolCount].name, body + i, copyLen);
                    symbols[symbolCount].name[copyLen] = '\0';
                    symbols[symbolCount].modulePage = modulePage;
                    symbolCount++;
                } else {
                    symbolsDropped++;
                }
                i += nameLen;
                /* public offset: 2 bytes (or 4 for PUBDEF32) */
                i += (type == PUBDEF32) ? 4 : 2;
                /* type index */
                if (i < blen) {
                    int ti = body[i++];
                    if (ti >= 0x80 && i < blen) i++;
                }
            }
        }

        if (type == MODEND || type == (MODEND + 1))
            break;
        offset += total;
    }
}

/* ── MS LIB dictionary hash ─────────────────────────────────────────── */

struct hashResult { int block; int blockd; int bucket; int bucketd; };

#define ROTL2(a) (unsigned short)(((a) << 2) | ((a) >> 14))
#define ROTR2(a) (unsigned short)(((a) << 14) | ((a) >> 2))

static void libHash(const char *name, int numBlocks, struct hashResult *h)
{
    int len = (int)strlen(name);
    const unsigned char *left  = (const unsigned char *)name;
    const unsigned char *right = left + len;
    unsigned short count   = (unsigned short)len;
    unsigned short block   = count | 0x20;
    unsigned short blockd  = 0;
    unsigned short bucket  = 0;
    unsigned short bucketd = count | 0x20;
    unsigned short c;

    for (;;) {
        --right;
        c = *right | 0x20;
        blockd  = c ^ ROTL2(blockd);
        bucket  = c ^ ROTR2(bucket);
        if (--count == 0) break;
        c = *left | 0x20;
        ++left;
        block   = c ^ ROTL2(block);
        bucketd = c ^ ROTR2(bucketd);
    }

    h->bucket  = (int)(bucket  % DICT_BUCKET);
    h->bucketd = (int)(bucketd % DICT_BUCKET);
    if (h->bucketd == 0) h->bucketd = 1;
    h->block  = (int)(block  % (unsigned)numBlocks);
    h->blockd = (int)(blockd % (unsigned)numBlocks);
    if (h->blockd == 0) h->blockd = 1;
}

static int writeDictionary(FILE *fp, int numBlocks)
{
    /* Allocate dictionary blocks */
    int dictSize = numBlocks * 512;
    unsigned char *dict = (unsigned char *)calloc(1, dictSize);
    if (!dict) return -1;

    /* Track free-space pointer per block (byte offset of next free spot).
       Data area starts at byte 38 (first even byte after 37-byte bucket table). */
    int *blockFill = (int *)calloc(numBlocks, sizeof(int));
    if (!blockFill) { free(dict); return -1; }
    for (int i = 0; i < numBlocks; i++)
        blockFill[i] = (DICT_BUCKET + 1) & ~1; /* = 38 */

    for (int s = 0; s < symbolCount; s++) {
        const char *name = symbols[s].name;
        int nameLen = (int)strlen(name);
        struct hashResult h;
        libHash(name, numBlocks, &h);
        int blk = h.block, bucket = h.bucket;

        /* Probe using delta stepping (MS LIB collision resolution) */
        int placed = 0;
        for (int bi = 0; bi < numBlocks; bi++) {
            unsigned char *block = dict + blk * 512;
            for (int bj = 0; bj < DICT_BUCKET; bj++) {
                if (block[bucket] == 0) {
                    /* entry = nameLen(1) + name(nameLen) + modulePage(2) */
                    int entrySize = 1 + nameLen + 2;
                    int fillPtr = blockFill[blk];
                    if (fillPtr + entrySize <= 512) {
                        block[bucket] = (unsigned char)(fillPtr / 2);
                        block[fillPtr] = (unsigned char)nameLen;
                        memcpy(block + fillPtr + 1, name, nameLen);
                        int page = symbols[s].modulePage;
                        block[fillPtr + 1 + nameLen] = (unsigned char)(page & 0xFF);
                        block[fillPtr + 1 + nameLen + 1] = (unsigned char)((page >> 8) & 0xFF);
                        /* Advance fill to next even offset */
                        blockFill[blk] = (fillPtr + entrySize + 1) & ~1;
                        placed = 1;
                    }
                    break; /* bucket was empty, so stop probing in this block */
                }
                bucket += h.bucketd;
                if (bucket >= DICT_BUCKET) bucket -= DICT_BUCKET;
            }
            if (placed) break;
            blk += h.blockd;
            if (blk >= numBlocks) blk -= numBlocks;
            bucket = h.bucket; /* reset bucket for new block */
        }
        if (!placed) {
            free(blockFill);
            free(dict);
            return -1; /* dictionary full, need more blocks */
        }
    }

    fwrite(dict, 1, dictSize, fp);
    free(blockFill);
    free(dict);
    return 0;
}

/* ── Read an OBJ file ────────────────────────────────────────────────── */

static int readObjFile(const char *path, unsigned char **outData, int *outSize)
{
    char fullPath[MAX_PATH];
    _snprintf(fullPath, sizeof(fullPath), "%s", path);
    fullPath[sizeof(fullPath) - 1] = '\0';

    /* Add .OBJ extension if not present */
    if (!strrchr(path, '.')) {
        _snprintf(fullPath, sizeof(fullPath), "%s.OBJ", path);
        fullPath[sizeof(fullPath) - 1] = '\0';
    }

    FILE *f = libOpen(fullPath, "rb");
    if (!f) {
        /* Try lowercase */
        _snprintf(fullPath, sizeof(fullPath), "%s.obj", path);
        fullPath[sizeof(fullPath) - 1] = '\0';
        f = libOpen(fullPath, "rb");
    }
    if (!f) {
        /* Try original path exactly */
        f = libOpen(path, "rb");
    }
    if (!f) {
        fprintf(stderr, "lib: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    *outData = (unsigned char *)malloc(sz);
    if (!*outData) { fclose(f); return -1; }
    *outSize = (int)fread(*outData, 1, sz, f);
    fclose(f);
    return 0;
}

/* ── Read existing library ───────────────────────────────────────────── */

/* A library that is not there yet is not an error: LIB creates it. */
static int readExistingLib(const char *path)
{
    FILE *f = libOpen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *data = (unsigned char *)malloc(fileSize);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, fileSize, f);
    fclose(f);

    if (fileSize < 3 || data[0] != LIB_HEADER) {
        free(data);
        return -1;
    }

    int pageSize = omfRecordLen(data) + 3;
    if (pageSize < 4) { free(data); return -1; }

    /* Walk modules */
    int offset = pageSize; /* first module starts after header */
    while (offset + 3 <= (int)fileSize) {
        if (data[offset] == LIB_END) break;
        if (data[offset] != THEADR) break;

        /* Find module end */
        int modStart = offset;
        int p = offset;
        while (p + 3 <= (int)fileSize) {
            int type = data[p];
            int total = omfRecordTotal(data + p);
            p += total;
            if (type == MODEND || type == (MODEND + 1)) break;
        }
        int modSize = p - modStart;

        if (moduleCount < MAX_MODULES) {
            modules[moduleCount].data = (unsigned char *)malloc(modSize);
            memcpy(modules[moduleCount].data, data + modStart, modSize);
            modules[moduleCount].size = modSize;
            /* Extract module name from THEADR */
            int nameLen = data[modStart + 3];
            if (nameLen > 0 && nameLen < MAX_PATH - 1) {
                memcpy(modules[moduleCount].name, data + modStart + 4, nameLen);
                modules[moduleCount].name[nameLen] = '\0';
            }
            moduleCount++;
        } else {
            fprintf(stderr, "lib: '%s' has more than %d modules\n", path, MAX_MODULES);
            failures++;
            break;
        }

        /* Advance to next page boundary */
        offset = ((p + pageSize - 1) / pageSize) * pageSize;
    }

    free(data);
    return 0;
}

/* ── Write library ───────────────────────────────────────────────────── */

static int writeLibrary(const char *path)
{
    FILE *fp = libOpen(path, "wb");
    if (!fp) {
        fprintf(stderr, "lib: cannot create '%s'\n", path);
        failures++;
        return -1;
    }

    /* Calculate page assignments */
    int pageSize = PAGE_SIZE;
    int currentPage = 1; /* page 0 = header */
    symbolCount = 0;
    symbolsDropped = 0;

    for (int m = 0; m < moduleCount; m++) {
        modules[m].page = currentPage;
        extractPublics(modules[m].data, modules[m].size, currentPage);
        int pagesNeeded = (modules[m].size + pageSize - 1) / pageSize;
        currentPage += pagesNeeded;
    }
    if (symbolsDropped) {
        fprintf(stderr, "lib: more than %d public symbols\n", MAX_SYMBOLS);
        failures++;
    }

    /* Calculate dictionary requirements */
    int numBlocks = 1;
    if (symbolCount > 0) {
        /* Estimate: each block holds ~10-15 symbols, be generous */
        numBlocks = (symbolCount / 8) + 1;
        /* Must be prime for better hashing */
        static const int primes[] = {1,2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,
                                     53,59,61,67,71,73,79,83,89,97,0};
        for (int i = 0; primes[i]; i++) {
            if (primes[i] >= numBlocks) { numBlocks = primes[i]; break; }
        }
    }

    long dictOffset = (long)currentPage * pageSize;
    int dictSizeBlocks = numBlocks;

    /* Write library header (0xF0) */
    unsigned char header[PAGE_SIZE];
    memset(header, 0, pageSize);
    header[0] = LIB_HEADER;
    int recLen = pageSize - 3;
    header[1] = (unsigned char)(recLen & 0xFF);
    header[2] = (unsigned char)((recLen >> 8) & 0xFF);
    /* Dictionary offset at byte 3 (4 bytes LE) */
    header[3] = (unsigned char)(dictOffset & 0xFF);
    header[4] = (unsigned char)((dictOffset >> 8) & 0xFF);
    header[5] = (unsigned char)((dictOffset >> 16) & 0xFF);
    header[6] = (unsigned char)((dictOffset >> 24) & 0xFF);
    /* Dictionary size in blocks at byte 7 (2 bytes LE) */
    header[7] = (unsigned char)(dictSizeBlocks & 0xFF);
    header[8] = (unsigned char)((dictSizeBlocks >> 8) & 0xFF);
    /* Flags at byte 9: 0 = case-sensitive */
    header[9] = 0;
    fwrite(header, 1, pageSize, fp);

    /* Write modules */
    for (int m = 0; m < moduleCount; m++) {
        long targetOffset = (long)modules[m].page * pageSize;
        long currentPos = ftell(fp);
        /* Pad to page boundary if needed */
        while (currentPos < targetOffset) {
            fputc(0, fp);
            currentPos++;
        }
        fwrite(modules[m].data, 1, modules[m].size, fp);
    }

    /* Write LIB_END record, then dictionary.  The dict offset in the header
       must point to the first byte of dictionary data (AFTER the end record). */
    {
        long currentPos = ftell(fp);
        /* LIB_END record fills the gap between the last module and the next
           page boundary, so the dictionary starts on a page boundary. */
        int padLen = (int)(dictOffset - currentPos - 3);
        if (padLen < 0) padLen = 0;
        unsigned char endRec[3];
        endRec[0] = LIB_END;
        endRec[1] = (unsigned char)(padLen & 0xFF);
        endRec[2] = (unsigned char)((padLen >> 8) & 0xFF);
        fwrite(endRec, 1, 3, fp);
        for (int i = 0; i < padLen; i++)
            fputc(0, fp);

        /* Dictionary immediately follows the end record.  Update the header
           to record the actual position. */
        dictOffset = ftell(fp);
        fseek(fp, 3, SEEK_SET);
        unsigned char doff[4];
        doff[0] = (unsigned char)(dictOffset & 0xFF);
        doff[1] = (unsigned char)((dictOffset >> 8) & 0xFF);
        doff[2] = (unsigned char)((dictOffset >> 16) & 0xFF);
        doff[3] = (unsigned char)((dictOffset >> 24) & 0xFF);
        fwrite(doff, 1, 4, fp);
        fseek(fp, dictOffset, SEEK_SET);
    }

    /* Try writing dictionary, retry with more blocks if needed */
    int written = 0;
    for (int attempt = 0; attempt < 5 && !written; attempt++) {
        long pos = ftell(fp);
        if (writeDictionary(fp, numBlocks) == 0) {
            written = 1;
            break;
        }
        /* Retry with more blocks */
        numBlocks = numBlocks * 2 + 1;
        /* Update header with new dict size */
        dictSizeBlocks = numBlocks;
        fseek(fp, 7, SEEK_SET);
        unsigned char dsb[2];
        dsb[0] = (unsigned char)(dictSizeBlocks & 0xFF);
        dsb[1] = (unsigned char)((dictSizeBlocks >> 8) & 0xFF);
        fwrite(dsb, 1, 2, fp);
        fseek(fp, pos, SEEK_SET);
    }

    if (fclose(fp) != 0 || !written) {
        fprintf(stderr, "lib: could not write '%s'\n", path);
        failures++;
        return -1;
    }

    if (omf_lib_verbose)
        fprintf(stderr, "  lib: wrote '%s' (%d modules, %d symbols)\n",
                path, moduleCount, symbolCount);
    return 0;
}

/* ── Find module by name ─────────────────────────────────────────────── */

static int findModule(const char *name)
{
    for (int i = 0; i < moduleCount; i++) {
        /* Compare module name (may have extension) */
        char baseName[MAX_PATH];
        const char *p = modules[i].name;
        const char *slash = strrchr(p, '\\');
        if (slash) p = slash + 1;
        const char *fsl = strrchr(p, '/');
        if (fsl) p = fsl + 1;
        _snprintf(baseName, sizeof(baseName), "%s", p);
        baseName[sizeof(baseName) - 1] = '\0';
        char *dot = strrchr(baseName, '.');
        if (dot) *dot = '\0';

        char searchName[MAX_PATH];
        _snprintf(searchName, sizeof(searchName), "%s", name);
        searchName[sizeof(searchName) - 1] = '\0';
        char *sdot = strrchr(searchName, '.');
        if (sdot) *sdot = '\0';

        if (_stricmp(baseName, searchName) == 0)
            return i;
    }
    return -1;
}

/* ── Remove module ───────────────────────────────────────────────────── */

static void removeModule(int idx)
{
    if (idx < 0 || idx >= moduleCount) return;
    free(modules[idx].data);
    for (int i = idx; i < moduleCount - 1; i++)
        modules[i] = modules[i + 1];
    moduleCount--;
}

/* ── Add module ──────────────────────────────────────────────────────── */

static void addModule(const char *name)
{
    if (moduleCount >= MAX_MODULES) {
        fprintf(stderr, "lib: more than %d modules\n", MAX_MODULES);
        failures++;
        return;
    }
    unsigned char *data;
    int size;
    if (readObjFile(name, &data, &size) != 0) {
        failures++;
        return;
    }
    modules[moduleCount].data = data;
    modules[moduleCount].size = size;
    _snprintf(modules[moduleCount].name, MAX_PATH, "%s", name);
    modules[moduleCount].name[MAX_PATH - 1] = '\0';
    moduleCount++;
}

/* ── Reset state ─────────────────────────────────────────────────────── */

static void resetLibState(void)
{
    for (int i = 0; i < moduleCount; i++)
        free(modules[i].data);
    moduleCount = 0;
    symbolCount = 0;
}

/* ── Parse and execute LIB command ───────────────────────────────────── */

int omf_lib(const char *args)
{
    resetLibState();
    failures = 0;

    const char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    /* Check for @response file */
    if (*p == '@') {
        p++;
        char rspPath[MAX_PATH];
        int ri = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && ri < MAX_PATH - 1)
            rspPath[ri++] = *p++;
        rspPath[ri] = '\0';

        FILE *rsp = libOpen(rspPath, "r");
        if (!rsp) {
            fprintf(stderr, "lib: cannot open response file '%s'\n", rspPath);
            return 1;
        }

        /* Read response file:
           Line 1: library name
           Line 2: y (confirm overwrite)
           Lines 3+: +moduleName& (& = continuation)
           Last line: listfile; */
        char libName[MAX_PATH] = "";
        char line[1024];
        int lineNum = 0;

        while (fgets(line, sizeof(line), rsp)) {
            /* Trim trailing whitespace */
            int len = (int)strlen(line);
            while (len > 0 && ((unsigned char)line[len-1] <= ' '))
                line[--len] = '\0';

            lineNum++;
            if (lineNum == 1) {
                _snprintf(libName, sizeof(libName), "%s", line);
                libName[sizeof(libName) - 1] = '\0';
                /* Add .lib if no extension */
                if (!strchr(libName, '.')) {
                    int ll = (int)strlen(libName);
                    if (ll + 4 < MAX_PATH) strcat(libName, ".lib");
                }
                continue;
            }
            if (lineNum == 2) {
                /* y/n confirmation — skip */
                continue;
            }

            /* Module operations or listing file */
            char *lp = line;
            while (*lp == ' ' || *lp == '\t') lp++;

            /* Check for end (;) or listing file */
            if (strchr(lp, ';')) {
                /* May have +operations before ; */
                char *semi = strchr(lp, ';');
                *semi = '\0';
            }

            /* Check for & continuation — remove it */
            len = (int)strlen(lp);
            if (len > 0 && lp[len - 1] == '&')
                lp[--len] = '\0';

            /* Process operations */
            char *op = lp;
            while (*op) {
                while (*op == ' ' || *op == '\t') op++;
                if (!*op) break;

                if (*op == '+') {
                    op++;
                    char modName[MAX_PATH];
                    int mi = 0;
                    while (*op && *op != '+' && *op != '-' && *op != ' ' &&
                           *op != '\t' && *op != ';' && *op != '&' && mi < MAX_PATH - 1)
                        modName[mi++] = *op++;
                    modName[mi] = '\0';
                    /* Trim trailing whitespace */
                    while (mi > 0 && (unsigned char)modName[mi-1] <= ' ')
                        modName[--mi] = '\0';

                    if (mi > 0)
                        addModule(modName);
                } else if (*op == '-') {
                    op++;
                    if (*op == '+') {
                        /* Replace */
                        op++;
                        char modName[MAX_PATH];
                        int mi = 0;
                        while (*op && *op != '+' && *op != '-' && *op != ' ' &&
                               *op != ';' && mi < MAX_PATH - 1)
                            modName[mi++] = *op++;
                        modName[mi] = '\0';
                        int idx = findModule(modName);
                        if (idx >= 0) removeModule(idx);
                        addModule(modName);
                    } else {
                        /* Delete */
                        char modName[MAX_PATH];
                        int mi = 0;
                        while (*op && *op != '+' && *op != '-' && *op != ' ' &&
                               *op != ';' && mi < MAX_PATH - 1)
                            modName[mi++] = *op++;
                        modName[mi] = '\0';
                        int idx = findModule(modName);
                        if (idx >= 0) removeModule(idx);
                    }
                } else {
                    /* Skip unexpected char */
                    op++;
                }
            }
        }
        fclose(rsp);

        if (libName[0]) {
            writeLibrary(libName);
        } else {
            fprintf(stderr, "lib: '%s' names no library\n", rspPath);
            failures++;
        }
        resetLibState();
        return failures ? 1 : 0;
    }

    /* Command-line mode: lib libname[+obj1+obj2...] [operations],listfile; */
    char libName[MAX_PATH] = "";
    int li = 0;

    /* Parse library name (may include +obj... for create mode) */
    while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' &&
           *p != '+' && *p != '-' && li < MAX_PATH - 1)
        libName[li++] = *p++;
    libName[li] = '\0';

    /* Add .lib extension if needed */
    if (!strchr(libName, '.')) {
        if (li + 4 < MAX_PATH) strcat(libName, ".lib");
    }

    /* Try to read existing library */
    readExistingLib(libName);

    /* Parse operations */
    while (*p && *p != ';') {
        if (*p == '+') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char modName[MAX_PATH];
            int mi = 0;
            while (*p && *p != '+' && *p != '-' && *p != ',' && *p != ';' &&
                   *p != ' ' && *p != '\t' && mi < MAX_PATH - 1)
                modName[mi++] = *p++;
            modName[mi] = '\0';
            while (mi > 0 && (unsigned char)modName[mi-1] <= ' ')
                modName[--mi] = '\0';

            if (mi > 0)
                addModule(modName);
        } else if (*p == '-') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '+') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                char modName[MAX_PATH];
                int mi = 0;
                while (*p && *p != '+' && *p != '-' && *p != ',' && *p != ';' &&
                       *p != ' ' && *p != '\t' && mi < MAX_PATH - 1)
                    modName[mi++] = *p++;
                modName[mi] = '\0';
                int idx = findModule(modName);
                if (idx >= 0) removeModule(idx);
                addModule(modName);
            } else {
                char modName[MAX_PATH];
                int mi = 0;
                while (*p && *p != '+' && *p != '-' && *p != ',' && *p != ';' &&
                       *p != ' ' && *p != '\t' && mi < MAX_PATH - 1)
                    modName[mi++] = *p++;
                modName[mi] = '\0';
                int idx = findModule(modName);
                if (idx >= 0) removeModule(idx);
            }
        } else if (*p == ' ' || *p == '\t') {
            p++;
            /* After spaces, could be more operations */
        } else if (*p == ',') {
            p++;
            /* Skip listing file name */
            while (*p && *p != ';' && *p != ',') p++;
        } else {
            p++;
        }
    }

    writeLibrary(libName);
    resetLibState();
    return failures ? 1 : 0;
}
