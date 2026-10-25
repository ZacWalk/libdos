/* A first program for libdos cc: proves the compiler, the DOS services and the
 * emulator all agree with each other. */

#include "dos.h"

int add(int a, int b)
{
	return a + b;
}

int factorial(int n)
{
	if (n <= 1) return 1;
	return n * factorial(n - 1);
}

int main(void)
{
	int i;
	char buffer[16];

	putln("Hello from C, compiled by libdos cc.");

	puts("2 + 3 = ");
	putu(add(2, 3));
	puts("\n");

	puts("7! = ");
	putu(factorial(7));
	puts("\n");

	puts("counting:");
	for (i = 0; i < 5; i = i + 1)
	{
		puts(" ");
		putu(i * i);
	}
	puts("\n");

	strcpy(buffer, "strings");
	strcat(buffer, " too");
	putln(buffer);

	puts("a big number: ");
	putu32(0x4240, 0x000f); /* 1000000 */
	puts("\n");

	return 0;
}
