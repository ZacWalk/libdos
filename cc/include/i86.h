#ifndef __LIBDOS_I86_H__
#define __LIBDOS_I86_H__

// libdos by Zac Walker
//
// The 8086-specific bits a 16-bit DOS source expects. The far-pointer macros
// are honest only while the compiler emits the tiny model, where every segment
// register already holds the program's own segment.

#include <stdint.h>

unsigned int __seg(void);

#define FP_SEG(p) (__seg())
#define FP_OFF(p) ((unsigned int)(p))
#define MK_FP(s, o) ((void *)(o))

#ifndef __LIBDOS_PORTS__
#define __LIBDOS_PORTS__
int inp(int port);
int outp(int port, int value);
unsigned int inpw(int port);
unsigned int outpw(int port, unsigned int value);
#endif

void enable(void);
void disable(void);
void delay(unsigned int milliseconds);

#endif
