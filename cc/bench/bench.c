/* ovrwin by Zac Walker
 *
 * The workload cc/ is judged on: the shapes a game's inner loops are made of,
 * with a checksum at the end so a codegen change that breaks something shows up
 * as a wrong answer rather than a faster one.
 *
 *   ovrwincli --ccbench cc\bench\bench.c
 */

int __int21(int ax, int bx, int cx, int dx);

static void put(int c)
{
	__int21(0x0200, 0, 0, c);
}

static void put_unsigned(unsigned long v)
{
	char digits[12];
	int n = 0;
	if (v == 0)
	{
		put('0');
		return;
	}
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

static void put_text(const char *s)
{
	while (*s != 0)
	{
		put(*s);
		s = s + 1;
	}
}

/* ---- fixed point, as m_fixed.h has it ---- */

typedef long fixed_t;

#define FRACBITS 16
#define FRACUNIT (1L << FRACBITS)

static fixed_t FixedMul(fixed_t a, fixed_t b)
{
	return (a >> 8) * (b >> 8);
}

static fixed_t FixedDiv(fixed_t a, fixed_t b)
{
	if (b == 0) return 0;
	return (a << 8) / (b >> 8);
}

/* ---- a column renderer, the shape of r_draw.c ---- */

#define WIDTH 128
#define HEIGHT 64

static unsigned char screen[WIDTH * HEIGHT];
static unsigned char texture[256];
static unsigned char colourmap[256];

static void draw_column(int x, int top, int bottom, fixed_t start, fixed_t step)
{
	unsigned char *dest;
	fixed_t frac;
	int y;

	if (top < 0) top = 0;
	if (bottom > HEIGHT - 1) bottom = HEIGHT - 1;
	frac = start;
	dest = &screen[top * WIDTH + x];
	for (y = top; y <= bottom; y++)
	{
		*dest = colourmap[texture[(int)(frac >> FRACBITS) & 255]];
		dest = dest + WIDTH;
		frac = frac + step;
	}
}

static void draw_span(int y, int left, int right, unsigned char value)
{
	unsigned char *dest;
	int x;

	dest = &screen[y * WIDTH + left];
	for (x = left; x <= right; x++)
	{
		*dest = value;
		dest = dest + 1;
	}
}

/* ---- moving objects, the shape of p_mobj.c ---- */

typedef struct
{
	fixed_t x, y;
	fixed_t momx, momy;
	int health;
	int flags;
} thing_t;

#define THINGS 24

static thing_t things[THINGS];

static void move_thing(thing_t *t)
{
	t->x = t->x + t->momx;
	t->y = t->y + t->momy;
	if (t->x < 0 || t->x > (64L << FRACBITS))
	{
		t->momx = -t->momx;
		t->health = t->health - 1;
	}
	if (t->y < 0 || t->y > (64L << FRACBITS))
	{
		t->momy = -t->momy;
		t->health = t->health - 1;
	}
	if (t->health <= 0)
	{
		t->health = 100;
		t->flags = t->flags + 1;
	}
}

/* ---- the trigonometry tables, the shape of tables.c ---- */

#define ANGLES 256

static fixed_t sine[ANGLES];

static void build_tables(void)
{
	int i;
	fixed_t x;

	/* A cheap sine: a parabola over each half period, which is enough shape to
	   keep the multiplies honest. */
	for (i = 0; i < ANGLES; i++)
	{
		int half = i & 127;
		x = ((fixed_t)half * FRACUNIT) / 128;
		x = FixedMul(x, FRACUNIT - x) * 4;
		sine[i] = i < 128 ? x : -x;
	}
}

int main(void)
{
	int frame, i, x, y;
	unsigned long checksum;
	fixed_t angle;

	for (i = 0; i < 256; i++)
	{
		texture[i] = (unsigned char)(i * 3 + 7);
		colourmap[i] = (unsigned char)(255 - i);
	}
	build_tables();

	for (i = 0; i < THINGS; i++)
	{
		things[i].x = (fixed_t)(i * 3) << FRACBITS;
		things[i].y = (fixed_t)(i * 5) << FRACBITS;
		things[i].momx = sine[i * 7] >> 4;
		things[i].momy = sine[i * 11] >> 4;
		things[i].health = 20 + i;
		things[i].flags = 0;
	}

	for (frame = 0; frame < 24; frame++)
	{
		for (x = 0; x < WIDTH; x++)
		{
			angle = sine[(x + frame) & 255];
			{
				int height = 8 + (int)(FixedMul(angle, 24L * FRACUNIT) >> FRACBITS);
				int middle = HEIGHT / 2;
				fixed_t step = FixedDiv(64L * FRACUNIT, (fixed_t)(height + 2) << FRACBITS);
				draw_column(x, middle - height, middle + height, (fixed_t)frame << 12, step);
			}
		}

		for (y = 0; y < 4; y++) draw_span(y + frame % 8, 0, WIDTH - 1, (unsigned char)(y + frame));

		for (i = 0; i < THINGS; i++) move_thing(&things[i]);
	}

	checksum = 0;
	for (i = 0; i < WIDTH * HEIGHT; i++) checksum = checksum * 31 + screen[i];
	for (i = 0; i < THINGS; i++)
		checksum = checksum + (unsigned long)things[i].health * 7 + (unsigned long)things[i].flags;
	for (i = 0; i < ANGLES; i++) checksum = checksum ^ (unsigned long)sine[i];

	put_text("checksum ");
	put_unsigned(checksum);
	put('\r');
	put('\n');

	/* The exit code carries the checksum too, so a headless run can check it. */
	return (int)(checksum % 251);
}
