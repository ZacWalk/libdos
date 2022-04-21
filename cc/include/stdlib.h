#ifndef __OVRWIN_STDLIB_H__
#define __OVRWIN_STDLIB_H__

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void free(void *block);
void exit(int code);
void abort(void);

int atoi(const char *s);
long atol(const char *s);
int abs(int v);
long labs(long v);
int rand(void);
void srand(unsigned int seed);

char *getenv(const char *name);

#endif
