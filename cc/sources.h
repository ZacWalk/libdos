#ifndef LIBDOS_CC_SOURCES_H
#define LIBDOS_CC_SOURCES_H

/* libdos by Zac Walker
 *
 * The compiler never opens a file. A caller supplies source text the same way
 * a host supplies the emulated drive, and keeps every file it hands over alive
 * until the compile finishes - the lexer normalises the bytes into its arena,
 * but a caller that reuses one buffer per include still works.
 *
 * A host embeds this as the first member of its own structure, so the callback
 * gets its state back by casting `self`.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_sources cc_sources;

struct cc_sources
{
	/* Non-zero when the file was found. */
	int (*load)(cc_sources* self, const char* name, const void** bytes, size_t* size);
};

#ifdef __cplusplus
}
#endif

#endif
