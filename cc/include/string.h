#ifndef __OVRWIN_STRING_H__
#define __OVRWIN_STRING_H__

#include <stddef.h>

void *memcpy(void *to, const void *from, size_t count);
void *memmove(void *to, const void *from, size_t count);
void *memset(void *to, int value, size_t count);
int memcmp(const void *a, const void *b, size_t count);

char *strcpy(char *to, const char *from);
char *strncpy(char *to, const char *from, size_t count);
char *strcat(char *to, const char *from);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t count);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t count);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

// The far variants a 16-bit source expects. In the tiny model they are the
// same functions, because there is only one segment to be far from.
#define _fmemcpy memcpy
#define _fmemmove memmove
#define _fmemset memset
#define _fmemcmp memcmp
#define _fstrcpy strcpy
#define _fstrlen strlen

#endif
