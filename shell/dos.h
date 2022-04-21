/* ovrwin by Zac Walker
 *
 * The DOS interface for programs built with ovrwincc, plus the handful of
 * library routines those programs need. There is no C library to link
 * against, so what is here is what there is.
 *
 * The compiler's runtime provides __int21, __int21si and __seg; everything
 * else is ordinary C compiled into the program.
 */

#ifndef DOS_H
#define DOS_H

/* Calls INT 21h with the given registers and returns AX. The registers DOS
 * left behind are readable afterwards, and __cf is the carry flag, which is
 * how DOS reports failure.
 *
 * These are overwritten by every call, so read what you need out of them
 * before calling anything else - including putch. */
int __int21(int ax, int bx, int cx, int dx);
int __int21si(int ax, int bx, int cx, int dx, int si);
int __int10(int ax, int bx, int cx, int dx);
unsigned __seg(void);
extern int __cf;
extern int __bx;
extern int __cx;
extern int __dx;

#define ATTR_DIRECTORY 0x10

/* ---- console ---- */

void putch(int c)
{
	__int21(0x0200, 0, 0, c);
}

void puts(char *s)
{
	while (*s != 0)
	{
		if (*s == '\n') putch('\r');
		putch(*s);
		s = s + 1;
	}
}

void putln(char *s)
{
	puts(s);
	puts("\n");
}

void putu(unsigned value)
{
	char digits[8];
	int at;

	if (value == 0)
	{
		putch('0');
		return;
	}
	at = 0;
	while (value != 0)
	{
		digits[at] = (char)('0' + (value % 10));
		value = value / 10;
		at = at + 1;
	}
	while (at > 0)
	{
		at = at - 1;
		putch(digits[at]);
	}
}

/* Prints a 32-bit value held as two 16-bit halves, which is how DOS reports a
 * file size. Long division a bit at a time keeps everything in 16 bits, since
 * this compiler has no 32-bit type. */
void putu32(unsigned low, unsigned high)
{
	char digits[12];
	unsigned remainder;
	unsigned quotient_high;
	unsigned quotient_low;
	int at;
	int bit;

	if (low == 0 && high == 0)
	{
		putch('0');
		return;
	}

	at = 0;
	while ((low != 0 || high != 0) && at < 11)
	{
		remainder = 0;
		quotient_high = 0;
		quotient_low = 0;

		bit = 16;
		while (bit > 0)
		{
			bit = bit - 1;
			remainder = (remainder << 1) | ((high >> bit) & 1);
			quotient_high = quotient_high << 1;
			if (remainder >= 10)
			{
				remainder = remainder - 10;
				quotient_high = quotient_high | 1;
			}
		}
		bit = 16;
		while (bit > 0)
		{
			bit = bit - 1;
			remainder = (remainder << 1) | ((low >> bit) & 1);
			quotient_low = quotient_low << 1;
			if (remainder >= 10)
			{
				remainder = remainder - 10;
				quotient_low = quotient_low | 1;
			}
		}

		digits[at] = (char)('0' + remainder);
		at = at + 1;
		low = quotient_low;
		high = quotient_high;
	}

	while (at > 0)
	{
		at = at - 1;
		putch(digits[at]);
	}
}

int getch(void)
{
	return __int21(0x0700, 0, 0, 0) & 255;
}

int key_waiting(void)
{
	return (__int21(0x0b00, 0, 0, 0) & 255) != 0;
}

/* ---- strings ---- */

int strlen(char *s)
{
	int n;
	n = 0;
	while (s[n] != 0) n = n + 1;
	return n;
}

void strcpy(char *to, char *from)
{
	while (*from != 0)
	{
		*to = *from;
		to = to + 1;
		from = from + 1;
	}
	*to = 0;
}

void strcat(char *to, char *from)
{
	while (*to != 0) to = to + 1;
	strcpy(to, from);
}

char upper(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - 32);
	return c;
}

/* Compares ignoring case, the way DOS matches command names. */
int match(char *a, char *b)
{
	while (*a != 0 && upper(*a) == upper(*b))
	{
		a = a + 1;
		b = b + 1;
	}
	return upper(*a) == upper(*b);
}

/* ---- files and directories ---- */

int dos_open(char *path, int mode)
{
	return __int21(0x3d00 | mode, 0, 0, (int)path);
}

int dos_close(int handle)
{
	return __int21(0x3e00, handle, 0, 0);
}

int dos_read(int handle, char *buffer, int length)
{
	return __int21(0x3f00, handle, length, (int)buffer);
}

int dos_write(int handle, char *buffer, int length)
{
	return __int21(0x4000, handle, length, (int)buffer);
}

int dos_delete(char *path)
{
	__int21(0x4100, 0, 0, (int)path);
	return __cf == 0;
}

int dos_chdir(char *path)
{
	__int21(0x3b00, 0, 0, (int)path);
	return __cf == 0;
}

int dos_mkdir(char *path)
{
	__int21(0x3900, 0, 0, (int)path);
	return __cf == 0;
}

int dos_rmdir(char *path)
{
	__int21(0x3a00, 0, 0, (int)path);
	return __cf == 0;
}

/* Writes the current directory of the default drive into `into`, without the
 * leading backslash that DOS leaves off. */
void dos_getcwd(char *into)
{
	__int21si(0x4700, 0, 0, 0, (int)into);
}

int dos_find_first(char *pattern, int attributes)
{
	__int21(0x4e00, 0, attributes, (int)pattern);
	return __cf == 0;
}

int dos_find_next(void)
{
	__int21(0x4f00, 0, 0, 0);
	return __cf == 0;
}

/* Runs another program and waits for it. Returns 1 if it started. */
int dos_exec(char *path, char *tail)
{
	unsigned parameters[7];

	parameters[0] = 0; /* inherit our environment */
	parameters[1] = (unsigned)tail;
	parameters[2] = __seg();
	parameters[3] = 0x5c;
	parameters[4] = __seg();
	parameters[5] = 0x6c;
	parameters[6] = __seg();
	__int21(0x4b00, (int)parameters, 0, (int)path);
	return __cf == 0;
}

int dos_exit_code(void)
{
	return __int21(0x4d00, 0, 0, 0) & 255;
}

/* The disk transfer area, where a directory search leaves its result. The
 * default is PSP:0080, and a .COM program's data segment is its PSP. */
#define DTA_ATTRIBUTES ((char *)0x95)
#define DTA_TIME       ((unsigned *)0x96)
#define DTA_DATE       ((unsigned *)0x98)
#define DTA_SIZE_LOW   ((unsigned *)0x9a)
#define DTA_SIZE_HIGH  ((unsigned *)0x9c)
#define DTA_NAME       ((char *)0x9e)

/* The command tail the program was started with, at PSP:0080. A directory
 * search overwrites it, so copy it before searching. */
#define TAIL_LENGTH    ((char *)0x80)
#define TAIL_TEXT      ((char *)0x81)

#endif
