#ifndef OVRWIN_CC_H
#define OVRWIN_CC_H

/* ovrwin by Zac Walker
 *
 * A small C compiler that targets 16-bit real mode and emits a DOS .COM. It
 * exists so the DOS side of this project can be written in C rather than in
 * hand-assembled bytes.
 *
 * The shape is borrowed from Fabrice Bellard's Tiny C Compiler: one pass, no
 * syntax tree, code generated as the parser goes, with a tiny value
 * abstraction standing in for a register allocator. None of TCC's code is used
 * here - it is LGPL and this is not - only the idea that a useful C compiler
 * does not need an intermediate representation.
 */

#include <stdint.h>

#include "sources.h"
#include "support.h"
#include "target.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the code went, once the layout pass has settled it. This is what a
 * multi-segment build has to divide up. */
typedef struct cc_function
{
	const char* name;
	int size;
} cc_function;

typedef struct cc_result
{
	int ok;

	const uint8_t* image; /* a DOS .COM image, loaded at offset 0100h */
	int image_size;

	const char* message;
	const char* file;
	int line;

	int code_size;
	int data_size;

	const cc_function* functions;
	int function_count;

	/* Everything above points into here, so a result is one thing to free. */
	cc_arena arena;
} cc_result;

void cc_result_free(cc_result* result);

/* What the compiler is allowed to do to the program as a whole. Every one of
 * these needs the whole program in memory at once, which is what compiling a
 * project as a single translation unit gives. */
typedef struct cc_options
{
	/* How big a body may be, in tokens, and still be copied to its call site
	 * rather than called. Zero turns it off. */
	int inline_limit;

	/* Leave out the functions nothing can reach. */
	int drop_unreachable;

	/* Which instructions the generated code may use. A 386 still produces a
	 * 16-bit .COM; it just has more to say it with. */
	cc_cpu cpu;

	/* What to compile for. x64 produces a block of host machine code to be
	 * called, not a DOS program. */
	cc_machine machine;
} cc_options;

static void cc_options_init(cc_options* options)
{
	options->inline_limit = 24;
	options->drop_unreachable = 1;
	options->cpu = cc_cpu_i8086;
	options->machine = cc_machine_i8088;
}

/* A hosted image asks the host for a service the way a DOS program asks the
 * operating system: it puts the registers somewhere agreed and calls. Both
 * places are at the top of the block, above the highest address the compiled
 * program itself can reach. */
#define CC_NATIVE_REGISTERS 0xffe0 /* ax, bx, cx, dx, si, as five words */
#define CC_NATIVE_CARRY 0xffea /* non-zero when the service failed */
#define CC_NATIVE_SERVICE 0xfff8 /* void (*)(void* block, int interrupt) */

/* Each define is either NAME or NAME=VALUE, as a command line spells it. */
cc_result cc_compile(const char* text, const char* name, cc_sources* sources,
                     const char* const* defines, int define_count, const cc_options* options);

#ifdef __cplusplus
}
#endif

#endif
