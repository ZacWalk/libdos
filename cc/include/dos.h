#ifndef __LIBDOS_DOS_H__
#define __LIBDOS_DOS_H__

// libdos by Zac Walker
//
// The register-block interface to the BIOS and DOS that 16-bit sources use.

#include <stdint.h>

struct WORDREGS
{
	unsigned int ax, bx, cx, dx, si, di, cflag;
};

struct BYTEREGS
{
	unsigned char al, ah, bl, bh, cl, ch, dl, dh;
};

union REGS
{
	struct WORDREGS w; // Watcom spells it w, the older libraries spell it x
	struct WORDREGS x;
	struct BYTEREGS h;
};

struct SREGS
{
	unsigned int es, cs, ss, ds;
};

int int86(int number, union REGS *in, union REGS *out);
int int86x(int number, union REGS *in, union REGS *out, struct SREGS *segments);

// An interrupt vector, as the segment and offset DOS reports it.
struct dos_vector
{
	unsigned int offset;
	unsigned int segment;
};

struct dos_vector _dos_getvect(int vector);
void _dos_setvect(int vector, struct dos_vector handler);

int _dos_allocmem(unsigned int paragraphs, unsigned int *segment);
int _dos_freemem(unsigned int segment);

#endif
