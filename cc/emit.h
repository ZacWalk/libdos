#pragma once

// ovrwin by Zac Walker
//
// 8086 instruction emitter. Only the instructions the code generator actually
// produces are here, which is a small fraction of the architecture: everything
// is computed in AX with CX as the second operand and the stack for the rest,
// so no register allocator is needed.

#include <cstdint>
#include <vector>

enum class condition
{
	equal, not_equal,
	less, less_equal, greater, greater_equal, // signed
	below, below_equal, above, above_equal, // unsigned
};

condition opposite(condition c);

struct emitter
{
	std::vector<uint8_t> code;

	int at() const { return int(code.size()); }
	void byte(int value) { code.push_back(uint8_t(value)); }
	void word(int value);
	void patch_word(int position, int value);
	void indirect(int opcode, int field, int displacement);

	// An instruction whose operand is a variable in memory: [bp+displacement]
	// for a local, [displacement] for a global. `field` is the reg or the
	// opcode extension, depending on the instruction.
	void memory(int opcode, int field, bool frame, int displacement);

	void mov_ax_imm(int value);
	void mov_cx_imm(int value);
	void mov_bx_imm(int value);
	void mov_dx_imm(int value);
	void mov_sp_imm(int value);

	// Absolute addressing, used for globals and string literals.
	void load_ax_absolute(int address);
	void store_ax_absolute(int address);
	void load_al_absolute(int address);
	void store_al_absolute(int address);
	void load_dx_absolute(int address);
	void store_dx_absolute(int address);

	// The second operand loaded straight into the register the operator wants,
	// which is what lets a simple operand skip the trip through AX and the stack.
	void mov_cl_imm(int value);
	void load_cx_absolute(int address);
	void load_bx_absolute(int address);
	void load_cx_frame(int displacement);
	void load_bx_frame(int displacement);
	void lea_cx_frame(int displacement);
	void xor_cx_cx();
	void xchg_ax_cx();
	void cmp_cx_ax();

	// [bp + displacement], used for locals and parameters.
	void load_ax_frame(int displacement);
	void store_ax_frame(int displacement);
	void load_al_frame(int displacement);
	void store_al_frame(int displacement);
	void load_dx_frame(int displacement);
	void store_dx_frame(int displacement);
	void lea_ax_frame(int displacement);

	// [bx], used once an address has been computed. The displacement is the
	// constant part of an address, which an 8086 addresses for nothing.
	void load_ax_indirect(int displacement = 0);
	void store_ax_indirect(int displacement = 0);
	void load_al_indirect(int displacement = 0);
	void store_al_indirect(int displacement = 0);
	void load_dx_indirect_high(int displacement = 0); // mov dx,[bx+2]
	void store_dx_indirect_high(int displacement = 0);
	void lea_ax_indirect(int displacement); // lea ax,[bx+displacement]
	void mov_bx_frame(int displacement);
	void mov_bx_absolute(int address);

	void push_ax();
	void pop_ax();
	void pop_bx();
	void pop_cx();
	void push_dx();
	void pop_dx();
	void push_bp();
	void pop_bp();

	void mov_cx_ax();
	void mov_bx_ax();
	void mov_ax_cx();
	void mov_ax_bx();
	void mov_dx_ax();
	void mov_ax_dx();
	void mov_cx_dx();
	void mov_bp_sp();
	void mov_sp_bp();
	void mov_ax_cs();

	void add_ax_cx();
	void sub_ax_cx();
	void and_ax_cx();
	void or_ax_cx();
	void xor_ax_cx();
	void cmp_ax_cx();
	void add_ax_imm(int value);
	void sub_ax_imm(int value);
	void and_ax_imm(int value);
	void or_ax_imm(int value);
	void xor_ax_imm(int value);
	void cmp_ax_imm(int value);
	void inc_ax();
	void dec_ax();
	void add_sp_imm(int value);
	void sub_sp_imm(int value);

	void imul_cx();
	void mul_cx();
	void idiv_cx(); // quotient in AX, remainder in DX
	void div_cx();
	void shl_ax_cl();
	void sar_ax_cl();
	void shr_ax_cl();
	void shl_ax_one();
	void shr_ax_one();
	void sar_ax_one();

	void neg_ax();
	void not_ax();
	void cbw();
	void zero_ah();
	void test_ax_ax();
	void xor_ax_ax();

	// A 32-bit value lives in DX:AX and its second operand in CX:BX, which is
	// what the runtime helpers in the code generator expect too.
	void add_ax_bx();
	void adc_dx_cx();
	void sub_ax_bx();
	void sbb_dx_cx();
	void and_ax_bx();
	void and_dx_cx();
	void or_ax_bx();
	void or_dx_cx();
	void xor_ax_bx();
	void xor_dx_cx();
	void or_ax_dx();
	void xor_dx_dx();
	void not_dx();
	void cwd();
	void adc_dx_imm(int value);
	void sbb_dx_zero();
	void negate_dx_ax();

	// A 32-bit operand pair against a constant, and the shifts of the pair the
	// runtime helper would otherwise loop for.
	void add_dx_imm(int value);
	void sub_dx_imm(int value);
	void sbb_dx_imm(int value);
	void adc_dx_zero();
	void and_dx_imm(int value);
	void or_dx_imm(int value);
	void xor_dx_imm(int value);
	void shl_dx_one();
	void shl_dx_cl();
	void shl_pair_one();
	void shr_pair_one();
	void sar_pair_one();

	// Turns the flags from the last compare into 0 or 1 in AX.
	void set_from_condition(condition c);

	// ---- the layout record ----
	//
	// A branch, a call and a field holding an address are recorded rather than
	// written as they are emitted. Nothing in the code refers to a byte offset
	// that has already been decided, so the whole program can be laid out once
	// at the end: a branch that reaches gets the two-byte form instead of a
	// short jump over a near one, a function nobody calls is dropped, and every
	// remaining offset is recomputed rather than patched in place.
	//
	// It is the smallest thing that deserves the name of an intermediate
	// representation, and it is the one a multi-segment build needs.

	enum class fixup_kind
	{
		jump, // E9 rel16, or EB rel8 when it reaches
		branch, // jcc rel8 over E9 rel16, or jcc rel8 when it reaches
		call, // E8 rel16
		code_word, // a word holding the address of a label
		data_word, // a word holding the address of an object in the data section
		value_word, // a word whose value is settled after generation
	};

	struct fixup
	{
		int position = 0;
		int length = 0; // what it occupies now
		int longest = 0; // what it occupied when it was emitted
		fixup_kind kind = fixup_kind::jump;
		int label = -1;
		int value = 0;
		condition test = condition::equal;
	};

	std::vector<int> labels; // the offset a label is bound to, or -1
	std::vector<fixup> fixups;

	int label(); // a place in the code, not yet known
	int here(); // the same, bound to where generation has got to
	void bind(int label);
	bool is_bound(int label) const { return labels[size_t(label)] >= 0; }
	int offset_of(int label) const { return labels[size_t(label)]; }

	void jump(int label);
	void jump_if(condition c, int label);
	void jump_unless(condition c, int label);
	void call(int label);

	// The word at `position`, already emitted as part of an instruction, holds
	// an address or a value that is only settled once everything has moved.
	void code_at(int position, int label);
	void data_at(int position, int offset);
	int value_at(int position, int value);
	void set_value(int index, int value);

	// Everything generation has produced, so that a speculative expression can
	// be taken back whole.
	struct position
	{
		size_t code = 0;
		size_t fixups = 0;
		size_t labels = 0;
	};

	position tell() const;
	void rewind(const position& p);

	// Drops the code between two labels, which is how an uncalled function
	// stops taking up room.
	void drop(int first, int last);

	// Picks the shortest encoding for every branch, removes what was dropped,
	// and moves everything that is left. Every label and fixup position is
	// recomputed; the operands themselves are left for resolve().
	void lay_out();

	// Writes the operands, once the layout and the data section are settled.
	// Returns false if a label was never bound.
	bool resolve(int code_base, int data_base);

	void call_frame(int displacement); // call [bp + displacement], for a function pointer
	void ret();
	void interrupt(int number);

private:
	std::vector<std::pair<int, int>> dropped_; // label pairs, as byte ranges
};
