#ifndef __OVRWIN_CONIO_H__
#define __OVRWIN_CONIO_H__

// ovrwin by Zac Walker
//
// The console and port routines a 16-bit DOS source expects. The port ones are
// also declared by i86.h; the guards let either header be included first.

int getch(void);
int kbhit(void);
int putch(int c);
void clrscr(void);

#ifndef __OVRWIN_PORTS__
#define __OVRWIN_PORTS__
int inp(int port);
int outp(int port, int value);
unsigned int inpw(int port);
unsigned int outpw(int port, unsigned int value);
#endif

#endif
