/* A round trip through the file system, so the native service path is doing
   what the emulator's DOS layer does. */

int __int21(int ax, int bx, int cx, int dx);
extern int __cf;

static void put(int c)
{
	__int21(0x0200, 0, 0, c);
}

static void put_text(const char *s)
{
	while (*s != 0)
	{
		put(*s);
		s = s + 1;
	}
}

static void put_unsigned(unsigned v)
{
	char digits[8];
	int n;

	if (v == 0)
	{
		put('0');
		return;
	}
	n = 0;
	while (v != 0)
	{
		digits[n] = (char)('0' + (int)(v % 10));
		n = n + 1;
		v = v / 10;
	}
	while (n != 0)
	{
		n = n - 1;
		put(digits[n]);
	}
}

static char name[] = "ROUND.TMP";
static char written[] = "the quick brown fox";
static char read_back[32];

int main(void)
{
	int handle;
	int count;
	int i;
	int sum;

	handle = __int21(0x3c00, 0, 0, (int)name);
	if (__cf) return 1;

	count = 0;
	while (written[count] != 0) count = count + 1;
	__int21(0x4000, handle, count, (int)written);
	if (__cf) return 2;
	__int21(0x3e00, handle, 0, 0);

	handle = __int21(0x3d00, 0, 0, (int)name);
	if (__cf) return 3;
	count = __int21(0x3f00, handle, 32, (int)read_back);
	if (__cf) return 4;
	__int21(0x3e00, handle, 0, 0);

	__int21(0x4100, 0, 0, (int)name);
	if (__cf) return 5;

	sum = 0;
	for (i = 0; i < count; i = i + 1) sum = sum * 3 + read_back[i];

	put_text("read ");
	put_unsigned((unsigned)count);
	put_text(" bytes, sum ");
	put_unsigned((unsigned)(sum & 0x7fff));
	put('\r');
	put('\n');
	return (sum & 0x7f) + 100;
}
