#ifndef __LIBDOS_UNISTD_H__
#define __LIBDOS_UNISTD_H__

#include <stddef.h>

#define F_OK 0
#define R_OK 4

int open(const char *name, int flags);
int close(int handle);
int read(int handle, void *into, unsigned int count);
int write(int handle, const void *from, unsigned int count);
long lseek(int handle, long offset, int whence);
int unlink(const char *name);
int access(const char *name, int mode);

#endif
