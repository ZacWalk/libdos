#ifndef LIBDOS_LIB_OMF_H
#define LIBDOS_LIB_OMF_H

/* libdos by Zac Walker
 *
 * The Microsoft OMF library manager demu runs in place of LIB.EXE. */

#ifdef __cplusplus
extern "C" {
#endif

/* Carries out a LIB command tail - "MAPPER.LIB -+A.OBJ;" or "@MAPPER.LBR".
 * Returns LIB's exit code: 0 when the library was written and every file it
 * names was read, 1 otherwise. */
int omf_lib(const char *args);

/* Non-zero for a line on stderr about each library written. */
extern int omf_lib_verbose;

/* Turns a file name LIB was given into the host path to open, returning 0 for
 * one it may not reach. Null takes names as host paths, relative to the
 * host's current directory. */
extern int (*omf_lib_map)(const char *name, char *host_path, int size);

#ifdef __cplusplus
}
#endif

#endif
