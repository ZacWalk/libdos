#ifndef LIBDOS_LIB_OMF_H
#define LIBDOS_LIB_OMF_H

/* libdos by Zac Walker
 *
 * The Microsoft OMF library manager demu runs in place of LIB.EXE. */

#ifdef __cplusplus
extern "C" {
#endif

/* Carries out a LIB command tail - "MAPPER.LIB -+A.OBJ;" or "@MAPPER.LBR" -
 * with paths relative to the host's current directory. Returns LIB's exit
 * code: 0 when the library was written. */
int omf_lib(const char *args);

/* Non-zero for a line on stderr about each library written. */
extern int omf_lib_verbose;

#ifdef __cplusplus
}
#endif

#endif
