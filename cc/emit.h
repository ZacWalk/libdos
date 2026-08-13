#pragma once

// ovrwin by Zac Walker
//
// The intermediate structure the back ends share, and the encoder that turns it
// into 8086 machine code.
//
// The generator no longer produces bytes. It produces a list of instructions in
// the x86 family's own vocabulary - an opcode, a width, and up to two operands,
// with the flags implicit exactly as the hardware has them. That is deliberately
// not a target-neutral intermediate representation: 8088, 386 and x64 are one
// instruction set at three widths, so parameterising over width and register
// count buys retargeting without paying for an abstraction that would have to be
// undone again at the bottom. It is also why the same structure could carry
// translated guest code: the flags an 8086 sets and the flags an x64 sets are
// the same flags, which a neutral IR would have to model and then discard.
//
// Nothing in the list refers to a byte offset. Positions are decided once, in
// lay_out(), which is what lets a branch take its shortest form, an unreachable
// function be left out, and the whole thing be cut into segments later.

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "target.h"

enum class condition
{
	equal, not_equal,
	less, less_equal, greater, greater_equal, // signed
	below, below_equal, above, above_equal, // unsigned
};

condition opposite(condition c);

// The architecture's own numbering, so a back end can use it as the reg field.
enum reg_name
{
	reg_a = 0, reg_c = 1, reg_d = 2, reg_b = 3,
	reg_sp = 4, reg_bp = 5, reg_si = 6, reg_di = 7,
	reg_ah = 4, // only as a byte register
	reg_none = 0xff,
};

enum class op : uint8_t
{
	nop,
	mov,
	lea,
	movsx, movzx, // 386: a narrow source widened into the destination
	add, adc, sub, sbb, and_, or_, xor_, cmp, test,
	inc, dec, neg, not_,
	mul, imul, div, idiv, // against the accumulator, as the machine has them
	shl, shr, sar, rcl, rcr,
	push, pop, xchg,
	cbw, cwd, // widen the accumulator in place
	setcc, // dest becomes 0 or 1, which an 8086 needs a branch for
	jmp, jcc, call, call_at, ret, interrupt,
	literal, // a run of bytes, for the hand-written runtime helpers
};

enum class operand_kind : uint8_t
{
	none,
	reg,
	immediate,
	memory, // [base + index + displacement]
};

// What an operand's value has to become once everything has moved.
enum class reference_kind : uint8_t
{
	none,
	data, // the address of an object in the data section
	code, // the address of a label
	deferred, // a value settled after generation, such as a frame size
};

struct operand
{
	operand_kind kind = operand_kind::none;
	uint8_t reg = reg_none; // the register, or the base of a memory operand
	uint8_t index = reg_none;
	reference_kind reference = reference_kind::none;
	int32_t value = 0; // the displacement, or the immediate
	int32_t target = -1; // the label, data offset or deferred slot

	static operand in(int r)
	{
		operand o;
		o.kind = operand_kind::reg;
		o.reg = uint8_t(r);
		return o;
	}

	static operand constant(int32_t v)
	{
		operand o;
		o.kind = operand_kind::immediate;
		o.value = v;
		return o;
	}

	static operand at(int base, int32_t displacement, int index = reg_none)
	{
		operand o;
		o.kind = operand_kind::memory;
		o.reg = uint8_t(base);
		o.index = uint8_t(index);
		o.value = displacement;
		return o;
	}

	static operand absolute(int32_t address)
	{
		operand o;
		o.kind = operand_kind::memory;
		o.value = address;
		return o;
	}
};

struct instruction
{
	op code = op::nop;
	uint8_t width = 2;
	condition test = condition::equal;
	int32_t label = -1; // for a branch or a call, where it goes
	operand dest;
	operand source;

	int32_t literal_at = 0; // the run of bytes, for op::literal
	int32_t literal_length = 0;

	// Settled by lay_out().
	int32_t offset = 0;
	int32_t length = 0;
};

struct emitter
{
	std::vector<instruction> code;
	std::vector<uint8_t> literals;
	std::vector<int> labels; // the instruction a label is bound to, or -1
	std::vector<int32_t> deferred;
	std::vector<uint8_t> image; // the encoding, once lay_out() has run

	// What the back end may emit. Set once, before anything is generated.
	cc_cpu cpu = cc_cpu::i8086;

	// A run of raw bytes coalesces into one instruction, but never across a
	// label: a label names an instruction, so the run has to start again.
	bool break_run = false;

	int at() const { return int(code.size()); }

	instruction& emit(op code, int width, const operand& dest = {}, const operand& source = {});

	// The operand of the instruction just emitted holds something that is only
	// settled once everything has moved.
	void refers_to_data(int32_t offset);
	void refers_to_code(int label);
	int refers_to_deferred(); // returns the slot, for set_deferred

	int label();
	int here();
	void bind(int label);
	bool is_bound(int label) const { return labels[size_t(label)] >= 0; }
	int offset_of(int label) const;

	int defer(int32_t value);
	void set_deferred(int slot, int32_t value);

	struct position
	{
		size_t code = 0;
		size_t literals = 0;
		size_t labels = 0;
		size_t deferred = 0;
	};

	position tell() const;
	void rewind(const position& p);

	// Drops the instructions between two labels, which is how an uncalled
	// function stops taking up room.
	void drop(int first, int last);

	// Chooses an encoding for everything, leaves out what was dropped, and
	// settles every offset.
	void lay_out(int code_base);

	// Writes the addresses that could only be known once the data section was
	// placed. False if a label was never bound.
	bool resolve(int code_base, int data_base);

	int code_size() const { return int(image.size()); }

	// ---- the shapes the generator asks for ----
	//
	// Every one of these is the same handful of instructions with a different
	// operand, which is the point: the hundred and forty names the byte emitter
	// needed collapse to about thirty opcodes once an operand is a value rather
	// than part of the mnemonic.

	static operand frame(int32_t displacement) { return operand::at(reg_bp, displacement); }
	static operand pointee(int32_t displacement) { return operand::at(reg_b, displacement); }

	void byte(int value);
	void bytes(std::initializer_list<int> run);

	void mov_ax_imm(int v) { emit(op::mov, 2, operand::in(reg_a), operand::constant(v)); }
	void mov_cx_imm(int v) { emit(op::mov, 2, operand::in(reg_c), operand::constant(v)); }
	void mov_bx_imm(int v) { emit(op::mov, 2, operand::in(reg_b), operand::constant(v)); }
	void mov_dx_imm(int v) { emit(op::mov, 2, operand::in(reg_d), operand::constant(v)); }
	void mov_sp_imm(int v) { emit(op::mov, 2, operand::in(reg_sp), operand::constant(v)); }
	void mov_cl_imm(int v) { emit(op::mov, 1, operand::in(reg_c), operand::constant(v)); }

	void load_ax_absolute(int a) { emit(op::mov, 2, operand::in(reg_a), operand::absolute(a)); }
	void store_ax_absolute(int a) { emit(op::mov, 2, operand::absolute(a), operand::in(reg_a)); }
	void load_al_absolute(int a) { emit(op::mov, 1, operand::in(reg_a), operand::absolute(a)); }
	void store_al_absolute(int a) { emit(op::mov, 1, operand::absolute(a), operand::in(reg_a)); }
	void load_dx_absolute(int a) { emit(op::mov, 2, operand::in(reg_d), operand::absolute(a)); }
	void store_dx_absolute(int a) { emit(op::mov, 2, operand::absolute(a), operand::in(reg_d)); }
	void load_cx_absolute(int a) { emit(op::mov, 2, operand::in(reg_c), operand::absolute(a)); }
	void load_bx_absolute(int a) { emit(op::mov, 2, operand::in(reg_b), operand::absolute(a)); }
	void mov_bx_absolute(int a) { emit(op::mov, 2, operand::in(reg_b), operand::absolute(a)); }

	void load_ax_frame(int d) { emit(op::mov, 2, operand::in(reg_a), frame(d)); }
	void store_ax_frame(int d) { emit(op::mov, 2, frame(d), operand::in(reg_a)); }
	void load_al_frame(int d) { emit(op::mov, 1, operand::in(reg_a), frame(d)); }
	void store_al_frame(int d) { emit(op::mov, 1, frame(d), operand::in(reg_a)); }
	void load_dx_frame(int d) { emit(op::mov, 2, operand::in(reg_d), frame(d)); }
	void store_dx_frame(int d) { emit(op::mov, 2, frame(d), operand::in(reg_d)); }
	void load_cx_frame(int d) { emit(op::mov, 2, operand::in(reg_c), frame(d)); }
	void load_bx_frame(int d) { emit(op::mov, 2, operand::in(reg_b), frame(d)); }
	void mov_bx_frame(int d) { emit(op::mov, 2, operand::in(reg_b), frame(d)); }
	void lea_ax_frame(int d) { emit(op::lea, 2, operand::in(reg_a), frame(d)); }
	void lea_cx_frame(int d) { emit(op::lea, 2, operand::in(reg_c), frame(d)); }

	void load_ax_indirect(int d = 0) { emit(op::mov, 2, operand::in(reg_a), pointee(d)); }
	void store_ax_indirect(int d = 0) { emit(op::mov, 2, pointee(d), operand::in(reg_a)); }
	void load_al_indirect(int d = 0) { emit(op::mov, 1, operand::in(reg_a), pointee(d)); }
	void store_al_indirect(int d = 0) { emit(op::mov, 1, pointee(d), operand::in(reg_a)); }
	void load_dx_indirect_high(int d = 0) { emit(op::mov, 2, operand::in(reg_d), pointee(d + 2)); }
	void store_dx_indirect_high(int d = 0) { emit(op::mov, 2, pointee(d + 2), operand::in(reg_d)); }
	void lea_ax_indirect(int d) { emit(op::lea, 2, operand::in(reg_a), pointee(d)); }

	void push_ax() { emit(op::push, 2, operand::in(reg_a)); }
	void pop_ax() { emit(op::pop, 2, operand::in(reg_a)); }
	void pop_bx() { emit(op::pop, 2, operand::in(reg_b)); }
	void pop_cx() { emit(op::pop, 2, operand::in(reg_c)); }
	void push_dx() { emit(op::push, 2, operand::in(reg_d)); }
	void pop_dx() { emit(op::pop, 2, operand::in(reg_d)); }
	void push_bp() { emit(op::push, 2, operand::in(reg_bp)); }
	void pop_bp() { emit(op::pop, 2, operand::in(reg_bp)); }

	void move(int to, int from) { emit(op::mov, 2, operand::in(to), operand::in(from)); }
	void mov_cx_ax() { move(reg_c, reg_a); }
	void mov_bx_ax() { move(reg_b, reg_a); }
	void mov_ax_cx() { move(reg_a, reg_c); }
	void mov_ax_bx() { move(reg_a, reg_b); }
	void mov_dx_ax() { move(reg_d, reg_a); }
	void mov_ax_dx() { move(reg_a, reg_d); }
	void mov_cx_dx() { move(reg_c, reg_d); }
	void mov_bp_sp() { move(reg_bp, reg_sp); }
	void mov_sp_bp() { move(reg_sp, reg_bp); }
	void xchg_ax_cx() { emit(op::xchg, 2, operand::in(reg_a), operand::in(reg_c)); }
	void mov_ax_cs() { bytes({0x8c, 0xc8}); }

	void arith(op kind, int to, int from) { emit(kind, 2, operand::in(to), operand::in(from)); }
	void arith_imm(op kind, int to, int v) { emit(kind, 2, operand::in(to), operand::constant(v)); }

	void add_ax_cx() { arith(op::add, reg_a, reg_c); }
	void sub_ax_cx() { arith(op::sub, reg_a, reg_c); }
	void and_ax_cx() { arith(op::and_, reg_a, reg_c); }
	void or_ax_cx() { arith(op::or_, reg_a, reg_c); }
	void xor_ax_cx() { arith(op::xor_, reg_a, reg_c); }
	void cmp_ax_cx() { arith(op::cmp, reg_a, reg_c); }
	void cmp_cx_ax() { arith(op::cmp, reg_c, reg_a); }
	void add_ax_imm(int v) { arith_imm(op::add, reg_a, v); }
	void sub_ax_imm(int v) { arith_imm(op::sub, reg_a, v); }
	void and_ax_imm(int v) { arith_imm(op::and_, reg_a, v); }
	void or_ax_imm(int v) { arith_imm(op::or_, reg_a, v); }
	void xor_ax_imm(int v) { arith_imm(op::xor_, reg_a, v); }
	void cmp_ax_imm(int v) { arith_imm(op::cmp, reg_a, v); }
	void add_sp_imm(int v) { arith_imm(op::add, reg_sp, v); }
	void sub_sp_imm(int v) { arith_imm(op::sub, reg_sp, v); }
	void inc_ax() { emit(op::inc, 2, operand::in(reg_a)); }
	void dec_ax() { emit(op::dec, 2, operand::in(reg_a)); }

	void imul_cx() { emit(op::imul, 2, operand::in(reg_c)); }
	void mul_cx() { emit(op::mul, 2, operand::in(reg_c)); }

	void idiv_cx()
	{
		emit(op::cwd, 2);
		emit(op::idiv, 2, operand::in(reg_c));
	}

	void div_cx()
	{
		emit(op::xor_, 2, operand::in(reg_d), operand::in(reg_d));
		emit(op::div, 2, operand::in(reg_c));
	}

	// A shift by -1 means "by CL", which is the only variable count an 8086 has.
	void shift(op kind, int r, int by) { emit(kind, 2, operand::in(r), operand::constant(by)); }
	void shl_ax_cl() { shift(op::shl, reg_a, -1); }
	void sar_ax_cl() { shift(op::sar, reg_a, -1); }
	void shr_ax_cl() { shift(op::shr, reg_a, -1); }
	void shl_ax_one() { shift(op::shl, reg_a, 1); }
	void shr_ax_one() { shift(op::shr, reg_a, 1); }
	void sar_ax_one() { shift(op::sar, reg_a, 1); }
	void shl_dx_one() { shift(op::shl, reg_d, 1); }
	void shl_dx_cl() { shift(op::shl, reg_d, -1); }

	void neg_ax() { emit(op::neg, 2, operand::in(reg_a)); }
	void not_ax() { emit(op::not_, 2, operand::in(reg_a)); }
	void cbw() { emit(op::cbw, 2); }
	void cwd() { emit(op::cwd, 2); }
	void zero_ah() { emit(op::xor_, 1, operand::in(reg_ah), operand::in(reg_ah)); }
	void test_ax_ax() { emit(op::test, 2, operand::in(reg_a), operand::in(reg_a)); }
	void xor_ax_ax() { emit(op::xor_, 2, operand::in(reg_a), operand::in(reg_a)); }
	void xor_cx_cx() { emit(op::xor_, 2, operand::in(reg_c), operand::in(reg_c)); }

	void add_ax_bx() { arith(op::add, reg_a, reg_b); }
	void adc_dx_cx() { arith(op::adc, reg_d, reg_c); }
	void sub_ax_bx() { arith(op::sub, reg_a, reg_b); }
	void sbb_dx_cx() { arith(op::sbb, reg_d, reg_c); }
	void and_ax_bx() { arith(op::and_, reg_a, reg_b); }
	void and_dx_cx() { arith(op::and_, reg_d, reg_c); }
	void or_ax_bx() { arith(op::or_, reg_a, reg_b); }
	void or_dx_cx() { arith(op::or_, reg_d, reg_c); }
	void xor_ax_bx() { arith(op::xor_, reg_a, reg_b); }
	void xor_dx_cx() { arith(op::xor_, reg_d, reg_c); }
	void or_ax_dx() { arith(op::or_, reg_a, reg_d); }
	void xor_dx_dx() { arith(op::xor_, reg_d, reg_d); }
	void not_dx() { emit(op::not_, 2, operand::in(reg_d)); }
	void adc_dx_imm(int v) { arith_imm(op::adc, reg_d, v); }
	void sbb_dx_zero() { arith_imm(op::sbb, reg_d, 0); }
	void add_dx_imm(int v) { arith_imm(op::add, reg_d, v); }
	void sub_dx_imm(int v) { arith_imm(op::sub, reg_d, v); }
	void sbb_dx_imm(int v) { arith_imm(op::sbb, reg_d, v); }
	void adc_dx_zero() { arith_imm(op::adc, reg_d, 0); }
	void and_dx_imm(int v) { arith_imm(op::and_, reg_d, v); }
	void or_dx_imm(int v) { arith_imm(op::or_, reg_d, v); }
	void xor_dx_imm(int v) { arith_imm(op::xor_, reg_d, v); }

	void shl_pair_one()
	{
		shift(op::shl, reg_a, 1);
		shift(op::rcl, reg_d, 1);
	}

	void shr_pair_one()
	{
		shift(op::shr, reg_d, 1);
		shift(op::rcr, reg_a, 1);
	}

	void sar_pair_one()
	{
		shift(op::sar, reg_d, 1);
		shift(op::rcr, reg_a, 1);
	}

	// Negating a pair is not two negates: the low word borrows into the high.
	void negate_dx_ax()
	{
		emit(op::neg, 2, operand::in(reg_d));
		emit(op::neg, 2, operand::in(reg_a));
		sbb_dx_zero();
	}

	void set_from_condition(condition c) { emit(op::setcc, 2, operand::in(reg_a)).test = c; }

	void jump(int label) { emit(op::jmp, 2).label = label; }

	void jump_if(condition c, int label)
	{
		instruction& in = emit(op::jcc, 2);
		in.test = c;
		in.label = label;
	}

	void jump_unless(condition c, int label) { jump_if(opposite(c), label); }
	void call(int label) { emit(op::call, 2).label = label; }
	void call_frame(int d) { emit(op::call_at, 2, frame(d)); }
	void ret() { emit(op::ret, 2); }
	void interrupt(int number) { emit(op::interrupt, 2, operand::constant(number)); }

private:
	std::vector<std::pair<int, int>> dropped_;

	int encode(const instruction& in, int position, std::vector<uint8_t>* into) const;
};
