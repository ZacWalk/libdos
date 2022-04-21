// ovrwin by Zac Walker
//
// 8086 instruction emitter.

#include "emit.h"

namespace
{
	// Short conditional jump opcodes, and the opcode of the opposite test.
	struct condition_codes
	{
		int taken;
		int inverse;
	};

	condition_codes codes_for(condition c)
	{
		switch (c)
		{
		case condition::equal: return {0x74, 0x75};
		case condition::not_equal: return {0x75, 0x74};
		case condition::less: return {0x7c, 0x7d};
		case condition::less_equal: return {0x7e, 0x7f};
		case condition::greater: return {0x7f, 0x7e};
		case condition::greater_equal: return {0x7d, 0x7c};
		case condition::below: return {0x72, 0x73};
		case condition::below_equal: return {0x76, 0x77};
		case condition::above: return {0x77, 0x76};
		default: return {0x73, 0x72};
		}
	}
}

void emitter::word(int value)
{
	byte(value & 0xff);
	byte((value >> 8) & 0xff);
}

void emitter::patch_word(int position, int value)
{
	code[size_t(position)] = uint8_t(value & 0xff);
	code[size_t(position) + 1] = uint8_t((value >> 8) & 0xff);
}

void emitter::mov_ax_imm(int value) { byte(0xb8); word(value); }
void emitter::mov_cx_imm(int value) { byte(0xb9); word(value); }
void emitter::mov_bx_imm(int value) { byte(0xbb); word(value); }
void emitter::mov_dx_imm(int value) { byte(0xba); word(value); }
void emitter::mov_sp_imm(int value) { byte(0xbc); word(value); }

void emitter::load_ax_absolute(int address) { byte(0xa1); word(address); }
void emitter::store_ax_absolute(int address) { byte(0xa3); word(address); }
void emitter::load_al_absolute(int address) { byte(0xa0); word(address); }
void emitter::store_al_absolute(int address) { byte(0xa2); word(address); }
void emitter::load_dx_absolute(int address) { byte(0x8b); byte(0x16); word(address); }
void emitter::store_dx_absolute(int address) { byte(0x89); byte(0x16); word(address); }

void emitter::load_ax_frame(int displacement) { byte(0x8b); byte(0x86); word(displacement); }
void emitter::store_ax_frame(int displacement) { byte(0x89); byte(0x86); word(displacement); }
void emitter::load_al_frame(int displacement) { byte(0x8a); byte(0x86); word(displacement); }
void emitter::store_al_frame(int displacement) { byte(0x88); byte(0x86); word(displacement); }
void emitter::load_dx_frame(int displacement) { byte(0x8b); byte(0x96); word(displacement); }
void emitter::store_dx_frame(int displacement) { byte(0x89); byte(0x96); word(displacement); }
void emitter::lea_ax_frame(int displacement) { byte(0x8d); byte(0x86); word(displacement); }

void emitter::load_ax_indirect() { byte(0x8b); byte(0x07); }
void emitter::store_ax_indirect() { byte(0x89); byte(0x07); }
void emitter::load_al_indirect() { byte(0x8a); byte(0x07); }
void emitter::store_al_indirect() { byte(0x88); byte(0x07); }
void emitter::load_dx_indirect_high() { byte(0x8b); byte(0x57); byte(0x02); }
void emitter::store_dx_indirect_high() { byte(0x89); byte(0x57); byte(0x02); }

void emitter::push_ax() { byte(0x50); }
void emitter::pop_ax() { byte(0x58); }
void emitter::pop_bx() { byte(0x5b); }
void emitter::pop_cx() { byte(0x59); }
void emitter::push_dx() { byte(0x52); }
void emitter::pop_dx() { byte(0x5a); }
void emitter::push_bp() { byte(0x55); }
void emitter::pop_bp() { byte(0x5d); }

void emitter::mov_cx_ax() { byte(0x89); byte(0xc1); }
void emitter::mov_bx_ax() { byte(0x89); byte(0xc3); }
void emitter::mov_ax_cx() { byte(0x89); byte(0xc8); }
void emitter::mov_ax_bx() { byte(0x89); byte(0xd8); }
void emitter::mov_dx_ax() { byte(0x89); byte(0xc2); }
void emitter::mov_cx_dx() { byte(0x89); byte(0xd1); }
void emitter::mov_bp_sp() { byte(0x89); byte(0xe5); }
void emitter::mov_sp_bp() { byte(0x89); byte(0xec); }
void emitter::mov_ax_cs() { byte(0x8c); byte(0xc8); }

void emitter::add_ax_cx() { byte(0x01); byte(0xc8); }
void emitter::sub_ax_cx() { byte(0x29); byte(0xc8); }
void emitter::and_ax_cx() { byte(0x21); byte(0xc8); }
void emitter::or_ax_cx() { byte(0x09); byte(0xc8); }
void emitter::xor_ax_cx() { byte(0x31); byte(0xc8); }
void emitter::cmp_ax_cx() { byte(0x39); byte(0xc8); }
void emitter::add_ax_imm(int value) { byte(0x05); word(value); }
void emitter::sub_ax_imm(int value) { byte(0x2d); word(value); }
void emitter::add_sp_imm(int value) { byte(0x81); byte(0xc4); word(value); }
void emitter::sub_sp_imm(int value) { byte(0x81); byte(0xec); word(value); }

void emitter::imul_cx() { byte(0xf7); byte(0xe9); }
void emitter::mul_cx() { byte(0xf7); byte(0xe1); }
void emitter::idiv_cx() { byte(0x99); byte(0xf7); byte(0xf9); }
void emitter::div_cx() { byte(0x31); byte(0xd2); byte(0xf7); byte(0xf1); }
void emitter::shl_ax_cl() { byte(0xd3); byte(0xe0); }
void emitter::sar_ax_cl() { byte(0xd3); byte(0xf8); }
void emitter::shr_ax_cl() { byte(0xd3); byte(0xe8); }

void emitter::neg_ax() { byte(0xf7); byte(0xd8); }
void emitter::not_ax() { byte(0xf7); byte(0xd0); }
void emitter::cbw() { byte(0x98); }
void emitter::zero_ah() { byte(0x30); byte(0xe4); }
void emitter::test_ax_ax() { byte(0x85); byte(0xc0); }
void emitter::xor_ax_ax() { byte(0x31); byte(0xc0); }

void emitter::add_ax_bx() { byte(0x01); byte(0xd8); }
void emitter::adc_dx_cx() { byte(0x11); byte(0xca); }
void emitter::sub_ax_bx() { byte(0x29); byte(0xd8); }
void emitter::sbb_dx_cx() { byte(0x19); byte(0xca); }
void emitter::and_ax_bx() { byte(0x21); byte(0xd8); }
void emitter::and_dx_cx() { byte(0x21); byte(0xca); }
void emitter::or_ax_bx() { byte(0x09); byte(0xd8); }
void emitter::or_dx_cx() { byte(0x09); byte(0xca); }
void emitter::xor_ax_bx() { byte(0x31); byte(0xd8); }
void emitter::xor_dx_cx() { byte(0x31); byte(0xca); }
void emitter::or_ax_dx() { byte(0x09); byte(0xd0); }
void emitter::xor_dx_dx() { byte(0x31); byte(0xd2); }
void emitter::not_dx() { byte(0xf7); byte(0xd2); }
void emitter::cwd() { byte(0x99); }
void emitter::adc_dx_imm(int value) { byte(0x81); byte(0xd2); word(value); }
void emitter::sbb_dx_zero() { byte(0x83); byte(0xda); byte(0x00); }

// Negating a pair is not two negates: the low word borrows into the high one.
void emitter::negate_dx_ax()
{
	byte(0xf7);
	byte(0xda); // neg dx
	neg_ax();
	sbb_dx_zero();
}

void emitter::set_from_condition(condition c)
{
	const condition_codes codes = codes_for(c);
	mov_ax_imm(1);
	byte(codes.taken);
	byte(0x02); // skip the two bytes that clear AX
	xor_ax_ax();
}

int emitter::jump()
{
	byte(0xe9);
	const int position = at();
	word(0);
	return position;
}

int emitter::jump_if_zero()
{
	// jnz +3 over a near jump, because an 8086 conditional jump is short only.
	byte(0x75);
	byte(0x03);
	return jump();
}

int emitter::jump_if_not_zero()
{
	byte(0x74);
	byte(0x03);
	return jump();
}

void emitter::call_frame(int displacement) { byte(0xff); byte(0x96); word(displacement); }

void emitter::patch_jump(int position)
{
	patch_word(position, at() - (position + 2));
}

void emitter::jump_back(int target)
{
	byte(0xe9);
	const int position = at();
	word(target - (position + 2));
}

int emitter::call()
{
	byte(0xe8);
	const int position = at();
	word(0);
	return position;
}

void emitter::ret() { byte(0xc3); }
void emitter::interrupt(int number) { byte(0xcd); byte(number); }
