#ifndef __OVRWIN_STDDEF_H__
#define __OVRWIN_STDDEF_H__

typedef unsigned int size_t;
typedef int ptrdiff_t;

#define NULL 0
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif
