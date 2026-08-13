#ifndef OVRWIN_CC_EMIT_H
#define OVRWIN_CC_EMIT_H

/* ovrwin by Zac Walker
 *
 * The intermediate structure the back ends share, and the encoder that turns it
 * into 8086 machine code.
 *
 * The generator no longer produces bytes. It produces a list of instructions in
 * the x86 family's own vocabulary - an opcode, a width, and up to two operands,
 * with the flags implicit exactly as the hardware has them. That is deliberately
 * not a target-neutral intermediate representation: 8088, 386 and x64 are one
 * instruction set at three widths, so parameterising over width and register
 * count buys retargeting without paying for an abstraction that would have to be
 * undone again at the bottom. It is also why the same structure could carry
 * translated guest code: the flags an 8086 sets and the flags an x64 sets are
 * the same flags, which a neutral IR would have to model and then discard.
 *
 * Nothing in the list refers to a byte offset. Positions are decided once, in
 * emit_lay_out(), which is what lets a branch take its shortest form, an
 * unreachable function be left out, and the whole thing be cut into segments
 * later.
 */

#include <stdint.h>

#include "support.h"
#include "target.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum condition
{
	cond_equal, cond_not_equal,
	cond_less, cond_less_equal, cond_greater, cond_greater_equal, /* signed */
	cond_below, cond_below_equal, cond_above, cond_above_equal /* unsigned */
} condition;

condition opposite(condition c);

/* The architecture's own numbering, so a back end can use it as the reg field. */
enum reg_name
{
	reg_a = 0, reg_c = 1, reg_d = 2, reg_b = 3,
	reg_sp = 4, reg_bp = 5, reg_si = 6, reg_di = 7,
	reg_ah = 4, /* only as a byte register */
	reg_none = 0xff
};

typedef enum op
{
	op_nop,
	op_mov,
	op_lea,
	op_movsx, op_movzx, /* 386: a narrow source widened into the destination */
	op_add, op_adc, op_sub, op_sbb, op_and_, op_or_, op_xor_, op_cmp, op_test,
	op_inc, op_dec, op_neg, op_not_,
	op_mul, op_imul, op_div, op_idiv, /* against the accumulator, as the machine has them */
	op_shl, op_shr, op_sar, op_rcl, op_rcr,
	op_push, op_pop, op_xchg,
	op_cbw, op_cwd, /* widen the accumulator in place */
	op_setcc, /* dest becomes 0 or 1, which an 8086 needs a branch for */
	op_jmp, op_jcc, op_call, op_call_at, op_ret, op_interrupt,
	op_literal /* a run of bytes, for the hand-written runtime helpers */
} op;

typedef enum operand_kind
{
	operand_kind_none,
	operand_kind_reg,
	operand_kind_immediate,
	operand_kind_memory /* [base + index + displacement] */
} operand_kind;

/* What an operand's value has to become once everything has moved. */
typedef enum reference_kind
{
	reference_kind_none,
	reference_kind_data, /* the address of an object in the data section */
	reference_kind_code, /* the address of a label */
	reference_kind_deferred /* a value settled after generation, such as a frame size */
} reference_kind;

typedef struct operand
{
	operand_kind kind;
	uint8_t reg; /* the register, or the base of a memory operand */
	uint8_t index;
	reference_kind reference;
	int32_t value; /* the displacement, or the immediate */
	int32_t target; /* the label, data offset or deferred slot */
} operand;

operand operand_none(void);
operand operand_in(int r);
operand operand_constant(int32_t v);
operand operand_at(int base, int32_t displacement, int index);
operand operand_absolute(int32_t address);

typedef struct instruction
{
	op code;
	uint8_t width;
	condition test;
	int32_t label; /* for a branch or a call, where it goes */
	operand dest;
	operand source;

	int32_t literal_at; /* the run of bytes, for op_literal */
	int32_t literal_length;

	/* Settled by emit_lay_out(). */
	int32_t offset;
	int32_t length;
	int32_t fixup; /* where in the instruction an address goes, from the start */
} instruction;

/* An instruction range that is not to be emitted. */
typedef struct code_range
{
	int first;
	int last;
} code_range;

typedef struct emitter
{
	cc_list code; /* instruction */
	cc_list literals; /* uint8_t */
	cc_list labels; /* int, the instruction a label is bound to, or -1 */
	cc_list deferred; /* int32_t */
	cc_list image; /* uint8_t, the encoding once emit_lay_out() has run */
	cc_list dropped; /* code_range */

	/* What the back end may emit. Set once, before anything is generated. */
	cc_cpu cpu;
	cc_machine machine;

	/* A run of raw bytes coalesces into one instruction, but never across a
	 * label: a label names an instruction, so the run has to start again. */
	int break_run;
} emitter;

void emit_init(emitter* e);
void emit_free(emitter* e);

#define EMIT_CODE(e, index) (((instruction*)(e).code.items)[index])
#define EMIT_LABEL(e, index) (((int*)(e).labels.items)[index])
#define EMIT_IMAGE(e, index) (((uint8_t*)(e).image.items)[index])

instruction* emit_op(emitter* e, op code, int width, operand dest, operand source);

/* The operand of the instruction just emitted holds something that is only
 * settled once everything has moved. */
void emit_refers_to_data(emitter* e, int32_t offset);
void emit_refers_to_code(emitter* e, int label);
int emit_refers_to_deferred(emitter* e); /* returns the slot, for emit_set_deferred */

int emit_label(emitter* e);
int emit_here(emitter* e);
void emit_bind(emitter* e, int label);
int emit_is_bound(const emitter* e, int label);
int emit_offset_of(const emitter* e, int label);

int emit_defer(emitter* e, int32_t value);
void emit_set_deferred(emitter* e, int slot, int32_t value);

typedef struct emitter_position
{
	int code;
	int literals;
	int labels;
	int deferred;
} emitter_position;

emitter_position emit_tell(const emitter* e);
void emit_rewind(emitter* e, emitter_position p);

/* Drops the instructions between two labels, which is how an uncalled function
 * stops taking up room. */
void emit_drop(emitter* e, int first, int last);

/* Chooses an encoding for everything, leaves out what was dropped, and settles
 * every offset. */
void emit_lay_out(emitter* e, int code_base);

/* Writes the addresses that could only be known once the data section was
 * placed. Zero if a label was never bound. */
int emit_resolve(emitter* e, int code_base, int data_base);

void emit_byte(emitter* e, int value);

int emit_encode(const emitter* e, instruction* in, int position, cc_list* into);
int emit_encode_x64(const emitter* e, instruction* in, int position, cc_list* into);

static int emit_at(const emitter* e) { return e->code.count; }
static int emit_code_size(const emitter* e) { return e->image.count; }

/* ---- the shapes the generator asks for ----
 *
 * Every one of these is the same handful of instructions with a different
 * operand, which is the point: the hundred and forty names the byte emitter
 * needed collapse to about thirty opcodes once an operand is a value rather
 * than part of the mnemonic.
 */

static operand emit_frame(int32_t displacement) { return operand_at(reg_bp, displacement, reg_none); }
static operand emit_pointee(int32_t displacement) { return operand_at(reg_b, displacement, reg_none); }

static void emit_plain(emitter* e, op code, int width)
{
	emit_op(e, code, width, operand_none(), operand_none());
}

static void emit_one(emitter* e, op code, int width, operand dest)
{
	emit_op(e, code, width, dest, operand_none());
}

static void emit_mov_ax_imm(emitter* e, int v) { emit_op(e, op_mov, 2, operand_in(reg_a), operand_constant(v)); }
static void emit_mov_cx_imm(emitter* e, int v) { emit_op(e, op_mov, 2, operand_in(reg_c), operand_constant(v)); }
static void emit_mov_bx_imm(emitter* e, int v) { emit_op(e, op_mov, 2, operand_in(reg_b), operand_constant(v)); }
static void emit_mov_dx_imm(emitter* e, int v) { emit_op(e, op_mov, 2, operand_in(reg_d), operand_constant(v)); }
static void emit_mov_sp_imm(emitter* e, int v) { emit_op(e, op_mov, 2, operand_in(reg_sp), operand_constant(v)); }
static void emit_mov_cl_imm(emitter* e, int v) { emit_op(e, op_mov, 1, operand_in(reg_c), operand_constant(v)); }

static void emit_load_ax_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_in(reg_a), operand_absolute(a)); }
static void emit_store_ax_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_absolute(a), operand_in(reg_a)); }
static void emit_load_al_absolute(emitter* e, int a) { emit_op(e, op_mov, 1, operand_in(reg_a), operand_absolute(a)); }
static void emit_store_al_absolute(emitter* e, int a) { emit_op(e, op_mov, 1, operand_absolute(a), operand_in(reg_a)); }
static void emit_load_dx_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_in(reg_d), operand_absolute(a)); }
static void emit_store_dx_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_absolute(a), operand_in(reg_d)); }
static void emit_load_cx_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_in(reg_c), operand_absolute(a)); }
static void emit_load_bx_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_in(reg_b), operand_absolute(a)); }
static void emit_mov_bx_absolute(emitter* e, int a) { emit_op(e, op_mov, 2, operand_in(reg_b), operand_absolute(a)); }

static void emit_load_ax_frame(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_a), emit_frame(d)); }
static void emit_store_ax_frame(emitter* e, int d) { emit_op(e, op_mov, 2, emit_frame(d), operand_in(reg_a)); }
static void emit_load_al_frame(emitter* e, int d) { emit_op(e, op_mov, 1, operand_in(reg_a), emit_frame(d)); }
static void emit_store_al_frame(emitter* e, int d) { emit_op(e, op_mov, 1, emit_frame(d), operand_in(reg_a)); }
static void emit_load_dx_frame(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_d), emit_frame(d)); }
static void emit_store_dx_frame(emitter* e, int d) { emit_op(e, op_mov, 2, emit_frame(d), operand_in(reg_d)); }
static void emit_load_cx_frame(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_c), emit_frame(d)); }
static void emit_load_bx_frame(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_b), emit_frame(d)); }
static void emit_mov_bx_frame(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_b), emit_frame(d)); }
static void emit_lea_ax_frame(emitter* e, int d) { emit_op(e, op_lea, 2, operand_in(reg_a), emit_frame(d)); }
static void emit_lea_cx_frame(emitter* e, int d) { emit_op(e, op_lea, 2, operand_in(reg_c), emit_frame(d)); }

static void emit_load_ax_indirect(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_a), emit_pointee(d)); }
static void emit_store_ax_indirect(emitter* e, int d) { emit_op(e, op_mov, 2, emit_pointee(d), operand_in(reg_a)); }
static void emit_load_al_indirect(emitter* e, int d) { emit_op(e, op_mov, 1, operand_in(reg_a), emit_pointee(d)); }
static void emit_store_al_indirect(emitter* e, int d) { emit_op(e, op_mov, 1, emit_pointee(d), operand_in(reg_a)); }
static void emit_load_dx_indirect_high(emitter* e, int d) { emit_op(e, op_mov, 2, operand_in(reg_d), emit_pointee(d + 2)); }
static void emit_store_dx_indirect_high(emitter* e, int d) { emit_op(e, op_mov, 2, emit_pointee(d + 2), operand_in(reg_d)); }
static void emit_lea_ax_indirect(emitter* e, int d) { emit_op(e, op_lea, 2, operand_in(reg_a), emit_pointee(d)); }

static void emit_push_ax(emitter* e) { emit_one(e, op_push, 2, operand_in(reg_a)); }
static void emit_pop_ax(emitter* e) { emit_one(e, op_pop, 2, operand_in(reg_a)); }
static void emit_pop_bx(emitter* e) { emit_one(e, op_pop, 2, operand_in(reg_b)); }
static void emit_pop_cx(emitter* e) { emit_one(e, op_pop, 2, operand_in(reg_c)); }
static void emit_push_dx(emitter* e) { emit_one(e, op_push, 2, operand_in(reg_d)); }
static void emit_pop_dx(emitter* e) { emit_one(e, op_pop, 2, operand_in(reg_d)); }
static void emit_push_bp(emitter* e) { emit_one(e, op_push, 2, operand_in(reg_bp)); }
static void emit_pop_bp(emitter* e) { emit_one(e, op_pop, 2, operand_in(reg_bp)); }

static void emit_move(emitter* e, int to, int from) { emit_op(e, op_mov, 2, operand_in(to), operand_in(from)); }
static void emit_mov_cx_ax(emitter* e) { emit_move(e, reg_c, reg_a); }
static void emit_mov_bx_ax(emitter* e) { emit_move(e, reg_b, reg_a); }
static void emit_mov_ax_cx(emitter* e) { emit_move(e, reg_a, reg_c); }
static void emit_mov_ax_bx(emitter* e) { emit_move(e, reg_a, reg_b); }
static void emit_mov_dx_ax(emitter* e) { emit_move(e, reg_d, reg_a); }
static void emit_mov_ax_dx(emitter* e) { emit_move(e, reg_a, reg_d); }
static void emit_mov_cx_dx(emitter* e) { emit_move(e, reg_c, reg_d); }
static void emit_mov_bp_sp(emitter* e) { emit_move(e, reg_bp, reg_sp); }
static void emit_mov_sp_bp(emitter* e) { emit_move(e, reg_sp, reg_bp); }
static void emit_xchg_ax_cx(emitter* e) { emit_op(e, op_xchg, 2, operand_in(reg_a), operand_in(reg_c)); }

static void emit_mov_ax_cs(emitter* e)
{
	emit_byte(e, 0x8c);
	emit_byte(e, 0xc8);
}

static void emit_arith(emitter* e, op kind, int to, int from)
{
	emit_op(e, kind, 2, operand_in(to), operand_in(from));
}

static void emit_arith_imm(emitter* e, op kind, int to, int v)
{
	emit_op(e, kind, 2, operand_in(to), operand_constant(v));
}

static void emit_add_ax_cx(emitter* e) { emit_arith(e, op_add, reg_a, reg_c); }
static void emit_sub_ax_cx(emitter* e) { emit_arith(e, op_sub, reg_a, reg_c); }
static void emit_and_ax_cx(emitter* e) { emit_arith(e, op_and_, reg_a, reg_c); }
static void emit_or_ax_cx(emitter* e) { emit_arith(e, op_or_, reg_a, reg_c); }
static void emit_xor_ax_cx(emitter* e) { emit_arith(e, op_xor_, reg_a, reg_c); }
static void emit_cmp_ax_cx(emitter* e) { emit_arith(e, op_cmp, reg_a, reg_c); }
static void emit_cmp_cx_ax(emitter* e) { emit_arith(e, op_cmp, reg_c, reg_a); }
static void emit_add_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_add, reg_a, v); }
static void emit_sub_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_sub, reg_a, v); }
static void emit_and_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_and_, reg_a, v); }
static void emit_or_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_or_, reg_a, v); }
static void emit_xor_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_xor_, reg_a, v); }
static void emit_cmp_ax_imm(emitter* e, int v) { emit_arith_imm(e, op_cmp, reg_a, v); }
static void emit_add_sp_imm(emitter* e, int v) { emit_arith_imm(e, op_add, reg_sp, v); }
static void emit_sub_sp_imm(emitter* e, int v) { emit_arith_imm(e, op_sub, reg_sp, v); }
static void emit_inc_ax(emitter* e) { emit_one(e, op_inc, 2, operand_in(reg_a)); }
static void emit_dec_ax(emitter* e) { emit_one(e, op_dec, 2, operand_in(reg_a)); }

static void emit_imul_cx(emitter* e) { emit_one(e, op_imul, 2, operand_in(reg_c)); }
static void emit_mul_cx(emitter* e) { emit_one(e, op_mul, 2, operand_in(reg_c)); }

static void emit_idiv_cx(emitter* e)
{
	emit_plain(e, op_cwd, 2);
	emit_one(e, op_idiv, 2, operand_in(reg_c));
}

static void emit_div_cx(emitter* e)
{
	emit_op(e, op_xor_, 2, operand_in(reg_d), operand_in(reg_d));
	emit_one(e, op_div, 2, operand_in(reg_c));
}

/* A shift by -1 means "by CL", which is the only variable count an 8086 has. */
static void emit_shift(emitter* e, op kind, int r, int by)
{
	emit_op(e, kind, 2, operand_in(r), operand_constant(by));
}

static void emit_shl_ax_cl(emitter* e) { emit_shift(e, op_shl, reg_a, -1); }
static void emit_sar_ax_cl(emitter* e) { emit_shift(e, op_sar, reg_a, -1); }
static void emit_shr_ax_cl(emitter* e) { emit_shift(e, op_shr, reg_a, -1); }
static void emit_shl_ax_one(emitter* e) { emit_shift(e, op_shl, reg_a, 1); }
static void emit_shr_ax_one(emitter* e) { emit_shift(e, op_shr, reg_a, 1); }
static void emit_sar_ax_one(emitter* e) { emit_shift(e, op_sar, reg_a, 1); }
static void emit_shl_dx_one(emitter* e) { emit_shift(e, op_shl, reg_d, 1); }
static void emit_shl_dx_cl(emitter* e) { emit_shift(e, op_shl, reg_d, -1); }

static void emit_neg_ax(emitter* e) { emit_one(e, op_neg, 2, operand_in(reg_a)); }
static void emit_not_ax(emitter* e) { emit_one(e, op_not_, 2, operand_in(reg_a)); }
static void emit_cbw(emitter* e) { emit_plain(e, op_cbw, 2); }
static void emit_cwd(emitter* e) { emit_plain(e, op_cwd, 2); }
static void emit_zero_ah(emitter* e) { emit_op(e, op_xor_, 1, operand_in(reg_ah), operand_in(reg_ah)); }
static void emit_test_ax_ax(emitter* e) { emit_op(e, op_test, 2, operand_in(reg_a), operand_in(reg_a)); }
static void emit_xor_ax_ax(emitter* e) { emit_op(e, op_xor_, 2, operand_in(reg_a), operand_in(reg_a)); }
static void emit_xor_cx_cx(emitter* e) { emit_op(e, op_xor_, 2, operand_in(reg_c), operand_in(reg_c)); }

static void emit_add_ax_bx(emitter* e) { emit_arith(e, op_add, reg_a, reg_b); }
static void emit_adc_dx_cx(emitter* e) { emit_arith(e, op_adc, reg_d, reg_c); }
static void emit_sub_ax_bx(emitter* e) { emit_arith(e, op_sub, reg_a, reg_b); }
static void emit_sbb_dx_cx(emitter* e) { emit_arith(e, op_sbb, reg_d, reg_c); }
static void emit_and_ax_bx(emitter* e) { emit_arith(e, op_and_, reg_a, reg_b); }
static void emit_and_dx_cx(emitter* e) { emit_arith(e, op_and_, reg_d, reg_c); }
static void emit_or_ax_bx(emitter* e) { emit_arith(e, op_or_, reg_a, reg_b); }
static void emit_or_dx_cx(emitter* e) { emit_arith(e, op_or_, reg_d, reg_c); }
static void emit_xor_ax_bx(emitter* e) { emit_arith(e, op_xor_, reg_a, reg_b); }
static void emit_xor_dx_cx(emitter* e) { emit_arith(e, op_xor_, reg_d, reg_c); }
static void emit_or_ax_dx(emitter* e) { emit_arith(e, op_or_, reg_a, reg_d); }
static void emit_xor_dx_dx(emitter* e) { emit_arith(e, op_xor_, reg_d, reg_d); }
static void emit_not_dx(emitter* e) { emit_one(e, op_not_, 2, operand_in(reg_d)); }
static void emit_adc_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_adc, reg_d, v); }
static void emit_sbb_dx_zero(emitter* e) { emit_arith_imm(e, op_sbb, reg_d, 0); }
static void emit_add_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_add, reg_d, v); }
static void emit_sub_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_sub, reg_d, v); }
static void emit_sbb_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_sbb, reg_d, v); }
static void emit_adc_dx_zero(emitter* e) { emit_arith_imm(e, op_adc, reg_d, 0); }
static void emit_and_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_and_, reg_d, v); }
static void emit_or_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_or_, reg_d, v); }
static void emit_xor_dx_imm(emitter* e, int v) { emit_arith_imm(e, op_xor_, reg_d, v); }

static void emit_shl_pair_one(emitter* e)
{
	emit_shift(e, op_shl, reg_a, 1);
	emit_shift(e, op_rcl, reg_d, 1);
}

static void emit_shr_pair_one(emitter* e)
{
	emit_shift(e, op_shr, reg_d, 1);
	emit_shift(e, op_rcr, reg_a, 1);
}

static void emit_sar_pair_one(emitter* e)
{
	emit_shift(e, op_sar, reg_d, 1);
	emit_shift(e, op_rcr, reg_a, 1);
}

/* Negating a pair is not two negates: the low word borrows into the high. */
static void emit_negate_dx_ax(emitter* e)
{
	emit_one(e, op_neg, 2, operand_in(reg_d));
	emit_one(e, op_neg, 2, operand_in(reg_a));
	emit_sbb_dx_zero(e);
}

static void emit_set_from_condition(emitter* e, condition c)
{
	emit_op(e, op_setcc, 2, operand_in(reg_a), operand_none())->test = c;
}

static void emit_jump(emitter* e, int label)
{
	emit_op(e, op_jmp, 2, operand_none(), operand_none())->label = label;
}

static void emit_jump_if(emitter* e, condition c, int label)
{
	instruction* in = emit_op(e, op_jcc, 2, operand_none(), operand_none());
	in->test = c;
	in->label = label;
}

static void emit_jump_unless(emitter* e, condition c, int label) { emit_jump_if(e, opposite(c), label); }

static void emit_call(emitter* e, int label)
{
	emit_op(e, op_call, 2, operand_none(), operand_none())->label = label;
}
static void emit_call_frame(emitter* e, int d) { emit_one(e, op_call_at, 2, emit_frame(d)); }
static void emit_ret(emitter* e) { emit_plain(e, op_ret, 2); }
static void emit_interrupt(emitter* e, int number) { emit_one(e, op_interrupt, 2, operand_constant(number)); }

#ifdef __cplusplus
}
#endif

#endif
