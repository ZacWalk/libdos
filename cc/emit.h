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

struct emitter
{
	std::vector<uint8_t> code;

	int at() const { return int(code.size()); }
	void byte(int value) { code.push_back(uint8_t(value)); }
	void word(int value);
	void patch_word(int position, int value);

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

	// [bp + displacement], used for locals and parameters.
	void load_ax_frame(int displacement);
	void store_ax_frame(int displacement);
	void load_al_frame(int displacement);
	void store_al_frame(int displacement);
	void load_dx_frame(int displacement);
	void store_dx_frame(int displacement);
	void lea_ax_frame(int displacement);

	// [bx], used once an address has been computed.
	void load_ax_indirect();
	void store_ax_indirect();
	void load_al_indirect();
	void store_al_indirect();
	void load_dx_indirect_high(); // mov dx,[bx+2]
	void store_dx_indirect_high();

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
	void add_sp_imm(int value);
	void sub_sp_imm(int value);

	void imul_cx();
	void mul_cx();
	void idiv_cx(); // quotient in AX, remainder in DX
	void div_cx();
	void shl_ax_cl();
	void sar_ax_cl();
	void shr_ax_cl();

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

	// Turns the flags from the last compare into 0 or 1 in AX.
	void set_from_condition(condition c);

	// Long forms: an 8086 has no near conditional jump, so a conditional
	// branch is a short jump over an unconditional one. Both return the
	// position of the 16-bit displacement, for patching.
	int jump();
	int jump_if_zero();
	int jump_if_not_zero();
	void patch_jump(int position);
	void jump_back(int target);

	int call();
	void call_frame(int displacement); // call [bp + displacement], for a function pointer
	void ret();
	void interrupt(int number);
};
