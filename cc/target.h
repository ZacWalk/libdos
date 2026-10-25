#ifndef LIBDOS_CC_TARGET_H
#define LIBDOS_CC_TARGET_H

/* libdos by Zac Walker
 *
 * What the front end has to know about the machine it is compiling for, and
 * nothing else. Everything here is a question the parser genuinely has to ask -
 * how big is an int, how far can a call reach - as opposed to a decision about
 * which instruction to use, which belongs to the back end.
 *
 * The three targets are one instruction set at three widths, which is what
 * makes a shared front end worth having: 8088 and 386 differ in whether 32-bit
 * arithmetic is native and in how many registers there are, not in what a C
 * program means.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cc_machine
{
	cc_machine_i8088, /* 16-bit, four general registers, 32-bit arithmetic through helpers */
	cc_machine_i386, /* 32-bit, eight general registers, scaled index addressing */
	cc_machine_x64 /* 64-bit, sixteen general registers */
} cc_machine;

/* Which instructions the back end may use, which is a separate question from
 * the width it compiles for: a 16-bit DOS program built for a 386 is still
 * 16-bit, it just has SETcc, MOVZX and a near conditional branch available. */
typedef enum cc_cpu
{
	cc_cpu_i8086,
	cc_cpu_i80186,
	cc_cpu_i80386
} cc_cpu;

typedef struct cc_target
{
	cc_machine machine;
	cc_cpu cpu;

	int int_size;
	int long_size;
	int pointer_size;

	/* The widest operation the machine does without a runtime helper. */
	int native_width;

	/* General registers the code generator may use. */
	int registers;

	/* Where a parameter starts, above the saved frame pointer and the return
	 * address. Both are pointer sized, so this is not the same on every machine. */
	int frame_base;

	/* Where the image loads, and how much room to leave above it. A .COM is
	 * one segment with the stack on top of the data. */
	int code_origin;
	int stack_size;
	int address_limit;

	/* Whether the program starts by talking to DOS. Code called by a host has
	 * no stack to set up and nothing to exit to. */
	int hosted;
} cc_target;

/* C has no default member initialisers, so the defaults live here. Every
 * target starts from the 8088 and then says how it differs. */
static void cc_target_init(cc_target* t)
{
	t->machine = cc_machine_i8088;
	t->cpu = cc_cpu_i8086;
	t->int_size = 2;
	t->long_size = 4;
	t->pointer_size = 2;
	t->native_width = 2;
	t->registers = 8;
	t->frame_base = 4;
	t->code_origin = 0x100;
	t->stack_size = 2048;
	t->address_limit = 0xfff0;
	t->hosted = 0;
}

#ifdef __cplusplus
}
#endif

#endif

