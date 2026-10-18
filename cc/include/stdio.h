#ifndef __LIBDOS_STDIO_H__
#define __LIBDOS_STDIO_H__

#include <stddef.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef int FILE;

FILE *fopen(const char *name, const char *mode);
int fclose(FILE *file);
size_t fread(void *into, size_t size, size_t count, FILE *file);
size_t fwrite(const void *from, size_t size, size_t count, FILE *file);
int fseek(FILE *file, long offset, int whence);
long ftell(FILE *file);

int putchar(int c);
int puts(const char *s);
int printf(const char *format, ...);
int sprintf(char *into, const char *format, ...);
int snprintf(char *into, size_t limit, const char *format, ...);

#endif
