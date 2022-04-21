// ovrwin by Zac Walker
//
// The C library for programs built by cc/. There is no linker yet, so a
// program includes this once rather than linking against it.
//
// Everything that talks to the outside world goes through __int21, the thunk
// the code generator emits. DOS clobbers __bx, __cx, __dx and __cf on every
// call, so a result has to be copied out before the next one.

#ifndef __OVRWIN_LIBC_C__
#define __OVRWIN_LIBC_C__

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __int21(int ax, int bx, int cx, int dx);
int __int21si(int ax, int bx, int cx, int dx, int si);
extern int __cf;
extern int __bx;
extern int __dx;

// ---------------------------------------------------------------- memory

void *memcpy(void *to, const void *from, size_t count)
{
	char *out = (char *)to;
	const char *in = (const char *)from;
	size_t i;
	for (i = 0; i < count; i++) out[i] = in[i];
	return to;
}

// Overlapping copies have to run backwards when the destination is higher.
void *memmove(void *to, const void *from, size_t count)
{
	char *out = (char *)to;
	const char *in = (const char *)from;
	size_t i;
	if (out > in)
	{
		for (i = count; i > 0; i--) out[i - 1] = in[i - 1];
		return to;
	}
	for (i = 0; i < count; i++) out[i] = in[i];
	return to;
}

void *memset(void *to, int value, size_t count)
{
	char *out = (char *)to;
	size_t i;
	for (i = 0; i < count; i++) out[i] = (char)value;
	return to;
}

int memcmp(const void *a, const void *b, size_t count)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;
	size_t i;
	for (i = 0; i < count; i++)
		if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
	return 0;
}

// ---------------------------------------------------------------- strings

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

char *strcpy(char *to, const char *from)
{
	size_t i = 0;
	while (from[i]) { to[i] = from[i]; i++; }
	to[i] = 0;
	return to;
}

char *strncpy(char *to, const char *from, size_t count)
{
	size_t i = 0;
	while (i < count && from[i]) { to[i] = from[i]; i++; }
	while (i < count) { to[i] = 0; i++; }
	return to;
}

char *strcat(char *to, const char *from)
{
	strcpy(to + strlen(to), from);
	return to;
}

int strcmp(const char *a, const char *b)
{
	size_t i = 0;
	while (a[i] && a[i] == b[i]) i++;
	if (a[i] == b[i]) return 0;
	return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
}

int strncmp(const char *a, const char *b, size_t count)
{
	size_t i = 0;
	while (i < count)
	{
		if (a[i] != b[i]) return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
		if (!a[i]) return 0;
		i++;
	}
	return 0;
}

static int __lower(int c)
{
	if (c >= 'A' && c <= 'Z') return c + 32;
	return c;
}

int strcasecmp(const char *a, const char *b)
{
	size_t i = 0;
	while (a[i] && __lower(a[i]) == __lower(b[i])) i++;
	if (__lower(a[i]) == __lower(b[i])) return 0;
	return __lower(a[i]) < __lower(b[i]) ? -1 : 1;
}

int strncasecmp(const char *a, const char *b, size_t count)
{
	size_t i = 0;
	while (i < count)
	{
		if (__lower(a[i]) != __lower(b[i])) return __lower(a[i]) < __lower(b[i]) ? -1 : 1;
		if (!a[i]) return 0;
		i++;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	size_t i = 0;
	for (;;)
	{
		if (s[i] == (char)c) return (char *)(s + i);
		if (!s[i]) return (char *)0;
		i++;
	}
}

char *strrchr(const char *s, int c)
{
	char *found = (char *)0;
	size_t i = 0;
	for (;;)
	{
		if (s[i] == (char)c) found = (char *)(s + i);
		if (!s[i]) return found;
		i++;
	}
}

// ---------------------------------------------------------------- output
//
// Formatting writes through one function so printf, sprintf and snprintf can
// share it: __into is where the next character goes, or null for the console.

static char *__into;
static size_t __room;
static int __written;

static void __emit(int c)
{
	__written++;
	if (__into == (char *)0)
	{
		if (c == '\n') __int21(0x0200, 0, 0, '\r');
		__int21(0x0200, 0, 0, c);
		return;
	}
	if (__room > 1) { *__into = (char)c; __into++; __room--; }
}

int putchar(int c)
{
	__into = (char *)0;
	__emit(c);
	return c;
}

int puts(const char *s)
{
	size_t i = 0;
	__into = (char *)0;
	while (s[i]) { __emit(s[i]); i++; }
	__emit('\n');
	return 0;
}

static void __emit_text(const char *s)
{
	size_t i = 0;
	while (s[i]) { __emit(s[i]); i++; }
}

// One routine prints every integer: the caller widens to 32 bits and says
// whether the value was signed and what base to use.
static void __emit_number(long value, int base, int is_signed)
{
	char digits[12];
	unsigned long v;
	int at = 0;

	if (is_signed && value < 0)
	{
		__emit('-');
		v = (unsigned long)(-value);
	}
	else
	{
		v = (unsigned long)value;
	}

	if (v == 0) { __emit('0'); return; }
	while (v != 0)
	{
		int digit = (int)(v % (unsigned long)base);
		digits[at] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		v = v / (unsigned long)base;
		at++;
	}
	while (at > 0) { at--; __emit(digits[at]); }
}

static int __format(const char *format, va_list ap)
{
	size_t i = 0;
	__written = 0;
	while (format[i])
	{
		int is_long = 0;
		if (format[i] != '%') { __emit(format[i]); i++; continue; }

		i++;
		// Width and flags are accepted and ignored; nothing here depends on
		// them lining up, and honouring them is not worth the code.
		while (format[i] == '-' || format[i] == '+' || format[i] == ' ' || format[i] == '0') i++;
		while (format[i] >= '0' && format[i] <= '9') i++;
		if (format[i] == '.') { i++; while (format[i] >= '0' && format[i] <= '9') i++; }
		while (format[i] == 'l' || format[i] == 'h') { if (format[i] == 'l') is_long = 1; i++; }

		if (format[i] == 'd' || format[i] == 'i')
		{
			if (is_long) __emit_number(va_arg(ap, long), 10, 1);
			else __emit_number((long)va_arg(ap, int), 10, 1);
		}
		else if (format[i] == 'u')
		{
			if (is_long) __emit_number(va_arg(ap, long), 10, 0);
			else __emit_number((long)(unsigned int)va_arg(ap, int), 10, 0);
		}
		else if (format[i] == 'x' || format[i] == 'X')
		{
			if (is_long) __emit_number(va_arg(ap, long), 16, 0);
			else __emit_number((long)(unsigned int)va_arg(ap, int), 16, 0);
		}
		else if (format[i] == 'c')
		{
			__emit(va_arg(ap, int));
		}
		else if (format[i] == 's')
		{
			__emit_text(va_arg(ap, char *));
		}
		else if (format[i] == '%')
		{
			__emit('%');
		}
		else if (format[i] == 0)
		{
			break;
		}
		i++;
	}
	if (__into != (char *)0 && __room > 0) *__into = 0;
	return __written;
}

int printf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	__into = (char *)0;
	return __format(format, ap);
}

int sprintf(char *into, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	__into = into;
	__room = 0x7fff;
	return __format(format, ap);
}

int snprintf(char *into, size_t limit, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	__into = into;
	__room = limit;
	return __format(format, ap);
}

// ---------------------------------------------------------------- files

FILE *fopen(const char *name, const char *mode)
{
	int handle;
	int access = 0;
	if (mode[0] == 'w') access = 1;
	if (mode[0] == 'w')
		handle = __int21(0x3c00, 0, 0, (int)name); // create, truncating
	else
		handle = __int21(0x3d00 + access, 0, 0, (int)name);
	if (__cf != 0) return (FILE *)0;
	// A handle of zero is stdin, so it can stand in for the null a caller tests.
	return (FILE *)(handle + 1);
}

int fclose(FILE *file)
{
	if (file == (FILE *)0) return EOF;
	__int21(0x3e00, (int)file - 1, 0, 0);
	return 0;
}

size_t fread(void *into, size_t size, size_t count, FILE *file)
{
	size_t total = size * count;
	int read;
	if (file == (FILE *)0 || total == 0) return 0;
	read = __int21(0x3f00, (int)file - 1, (int)total, (int)into);
	if (__cf != 0) return 0;
	return size == 0 ? 0 : (size_t)read / size;
}

size_t fwrite(const void *from, size_t size, size_t count, FILE *file)
{
	size_t total = size * count;
	int wrote;
	if (file == (FILE *)0 || total == 0) return 0;
	wrote = __int21(0x4000, (int)file - 1, (int)total, (int)from);
	if (__cf != 0) return 0;
	return size == 0 ? 0 : (size_t)wrote / size;
}

int fseek(FILE *file, long offset, int whence)
{
	if (file == (FILE *)0) return -1;
	// CX:DX carries the offset, so the halves go in separately.
	__int21(0x4200 + whence, (int)file - 1, (int)(offset >> 16), (int)(offset & 0xffff));
	return __cf != 0 ? -1 : 0;
}

long ftell(FILE *file)
{
	long high;
	long low;
	if (file == (FILE *)0) return -1;
	low = (long)(unsigned int)__int21(0x4201, (int)file - 1, 0, 0);
	if (__cf != 0) return -1;
	high = (long)(unsigned int)__dx;
	return (high << 16) | low;
}

// ---------------------------------------------------------------- general
//
// A bump allocator over one static block. Nothing built with this compiler
// frees in a pattern that would repay a free list, and the zone allocator a
// game brings does its own management anyway.

#ifndef HEAP_SIZE
#define HEAP_SIZE 4096
#endif

static char __heap[HEAP_SIZE];
static size_t __heap_used;

void *malloc(size_t size)
{
	char *block;
	size = (size + 1) & ~(size_t)1;
	if (__heap_used + size > HEAP_SIZE) return (void *)0;
	block = __heap + __heap_used;
	__heap_used += size;
	return (void *)block;
}

void *calloc(size_t count, size_t size)
{
	void *block = malloc(count * size);
	if (block != (void *)0) memset(block, 0, count * size);
	return block;
}

void free(void *block)
{
	(void)block;
}

void exit(int code)
{
	__int21(0x4c00 + (code & 0xff), 0, 0, 0);
}

void abort(void)
{
	exit(3);
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

int atoi(const char *s) { return (int)atol(s); }

long atol(const char *s)
{
	long value = 0;
	int negative = 0;
	size_t i = 0;
	while (s[i] == ' ' || s[i] == '\t') i++;
	if (s[i] == '-') { negative = 1; i++; }
	else if (s[i] == '+') i++;
	while (s[i] >= '0' && s[i] <= '9')
	{
		value = value * 10 + (s[i] - '0');
		i++;
	}
	return negative ? -value : value;
}

static long __seed = 1;

int rand(void)
{
	__seed = __seed * 1103515245 + 12345;
	return (int)((__seed >> 16) & 0x7fff);
}

void srand(unsigned int seed)
{
	__seed = (long)(unsigned long)seed;
}

char *getenv(const char *name)
{
	(void)name;
	return (char *)0;
}

#endif
