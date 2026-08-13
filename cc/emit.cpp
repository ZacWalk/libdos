// ovrwin by Zac Walker
//
// 8086 instruction emitter.

#include "emit.h"

#include <algorithm>

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

condition opposite(condition c)
{
	switch (c)
	{
	case condition::equal: return condition::not_equal;
	case condition::not_equal: return condition::equal;
	case condition::less: return condition::greater_equal;
	case condition::less_equal: return condition::greater;
	case condition::greater: return condition::less_equal;
	case condition::greater_equal: return condition::less;
	case condition::below: return condition::above_equal;
	case condition::below_equal: return condition::above;
	case condition::above: return condition::below_equal;
	default: return condition::below;
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

void emitter::mov_cl_imm(int value) { byte(0xb1); byte(value & 0xff); }
void emitter::load_cx_absolute(int address) { byte(0x8b); byte(0x0e); word(address); }
void emitter::load_bx_absolute(int address) { byte(0x8b); byte(0x1e); word(address); }
void emitter::load_cx_frame(int displacement) { byte(0x8b); byte(0x8e); word(displacement); }
void emitter::load_bx_frame(int displacement) { byte(0x8b); byte(0x9e); word(displacement); }
void emitter::lea_cx_frame(int displacement) { byte(0x8d); byte(0x8e); word(displacement); }
void emitter::xor_cx_cx() { byte(0x31); byte(0xc9); }
void emitter::xchg_ax_cx() { byte(0x91); }
void emitter::cmp_cx_ax() { byte(0x39); byte(0xc1); }

void emitter::load_ax_frame(int displacement) { byte(0x8b); byte(0x86); word(displacement); }
void emitter::store_ax_frame(int displacement) { byte(0x89); byte(0x86); word(displacement); }
void emitter::load_al_frame(int displacement) { byte(0x8a); byte(0x86); word(displacement); }
void emitter::store_al_frame(int displacement) { byte(0x88); byte(0x86); word(displacement); }
void emitter::load_dx_frame(int displacement) { byte(0x8b); byte(0x96); word(displacement); }
void emitter::store_dx_frame(int displacement) { byte(0x89); byte(0x96); word(displacement); }
void emitter::lea_ax_frame(int displacement) { byte(0x8d); byte(0x86); word(displacement); }

void emitter::memory(int opcode, int field, bool frame, int displacement)
{
	byte(opcode);
	byte((frame ? 0x86 : 0x06) | (field << 3));
	word(displacement);
}

// An instruction with a [bx+displacement] operand, in the shortest of the
// three encodings the displacement allows.
void emitter::indirect(int opcode, int field, int displacement)
{
	byte(opcode);
	if (displacement == 0)
	{
		byte(0x07 | (field << 3));
	}
	else if (displacement >= -128 && displacement <= 127)
	{
		byte(0x47 | (field << 3));
		byte(displacement & 0xff);
	}
	else
	{
		byte(0x87 | (field << 3));
		word(displacement);
	}
}

void emitter::load_ax_indirect(int displacement) { indirect(0x8b, 0, displacement); }
void emitter::store_ax_indirect(int displacement) { indirect(0x89, 0, displacement); }
void emitter::load_al_indirect(int displacement) { indirect(0x8a, 0, displacement); }
void emitter::store_al_indirect(int displacement) { indirect(0x88, 0, displacement); }
void emitter::load_dx_indirect_high(int displacement) { indirect(0x8b, 2, displacement + 2); }
void emitter::store_dx_indirect_high(int displacement) { indirect(0x89, 2, displacement + 2); }
void emitter::lea_ax_indirect(int displacement) { indirect(0x8d, 0, displacement); }
void emitter::mov_bx_frame(int displacement) { byte(0x8b); byte(0x9e); word(displacement); }
void emitter::mov_bx_absolute(int address) { byte(0x8b); byte(0x1e); word(address); }

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
void emitter::mov_ax_dx() { byte(0x89); byte(0xd0); }
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
void emitter::and_ax_imm(int value) { byte(0x25); word(value); }
void emitter::or_ax_imm(int value) { byte(0x0d); word(value); }
void emitter::xor_ax_imm(int value) { byte(0x35); word(value); }
void emitter::cmp_ax_imm(int value) { byte(0x3d); word(value); }
void emitter::inc_ax() { byte(0x40); }
void emitter::dec_ax() { byte(0x48); }
void emitter::add_sp_imm(int value) { byte(0x81); byte(0xc4); word(value); }
void emitter::sub_sp_imm(int value) { byte(0x81); byte(0xec); word(value); }

void emitter::imul_cx() { byte(0xf7); byte(0xe9); }
void emitter::mul_cx() { byte(0xf7); byte(0xe1); }
void emitter::idiv_cx() { byte(0x99); byte(0xf7); byte(0xf9); }
void emitter::div_cx() { byte(0x31); byte(0xd2); byte(0xf7); byte(0xf1); }
void emitter::shl_ax_cl() { byte(0xd3); byte(0xe0); }
void emitter::sar_ax_cl() { byte(0xd3); byte(0xf8); }
void emitter::shr_ax_cl() { byte(0xd3); byte(0xe8); }
void emitter::shl_ax_one() { byte(0xd1); byte(0xe0); }
void emitter::shr_ax_one() { byte(0xd1); byte(0xe8); }
void emitter::sar_ax_one() { byte(0xd1); byte(0xf8); }

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

void emitter::add_dx_imm(int value) { byte(0x81); byte(0xc2); word(value); }
void emitter::sub_dx_imm(int value) { byte(0x81); byte(0xea); word(value); }
void emitter::sbb_dx_imm(int value) { byte(0x81); byte(0xda); word(value); }
void emitter::adc_dx_zero() { byte(0x83); byte(0xd2); byte(0x00); }
void emitter::and_dx_imm(int value) { byte(0x81); byte(0xe2); word(value); }
void emitter::or_dx_imm(int value) { byte(0x81); byte(0xca); word(value); }
void emitter::xor_dx_imm(int value) { byte(0x81); byte(0xf2); word(value); }
void emitter::shl_dx_one() { byte(0xd1); byte(0xe2); }
void emitter::shl_dx_cl() { byte(0xd3); byte(0xe2); }
void emitter::shl_pair_one() { byte(0xd1); byte(0xe0); byte(0xd1); byte(0xd2); }
void emitter::shr_pair_one() { byte(0xd1); byte(0xea); byte(0xd1); byte(0xd8); }
void emitter::sar_pair_one() { byte(0xd1); byte(0xfa); byte(0xd1); byte(0xd8); }

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

// ---------------------------------------------------------------------------
// The layout record
// ---------------------------------------------------------------------------

int emitter::label()
{
	labels.push_back(-1);
	return int(labels.size()) - 1;
}

void emitter::bind(int label)
{
	labels[size_t(label)] = at();
}

int emitter::here()
{
	labels.push_back(at());
	return int(labels.size()) - 1;
}

namespace
{
	// Every fixup is emitted at its longest, so that the layout pass can only
	// ever make the program shorter.
	int longest_of(emitter::fixup_kind kind)
	{
		switch (kind)
		{
		case emitter::fixup_kind::jump: return 3; // E9 rel16
		case emitter::fixup_kind::branch: return 5; // jcc rel8 over E9 rel16
		case emitter::fixup_kind::call: return 3; // E8 rel16
		default: return 2;
		}
	}
}

void emitter::jump(int label)
{
	fixup f;
	f.position = at();
	f.kind = fixup_kind::jump;
	f.length = f.longest = longest_of(f.kind);
	f.label = label;
	fixups.push_back(f);
	for (int i = 0; i < f.length; ++i) byte(0x90);
}

void emitter::jump_if(condition c, int label)
{
	fixup f;
	f.position = at();
	f.kind = fixup_kind::branch;
	f.length = f.longest = longest_of(f.kind);
	f.label = label;
	f.test = c;
	fixups.push_back(f);
	for (int i = 0; i < f.length; ++i) byte(0x90);
}

void emitter::jump_unless(condition c, int label)
{
	jump_if(opposite(c), label);
}

void emitter::call(int label)
{
	fixup f;
	f.position = at();
	f.kind = fixup_kind::call;
	f.length = f.longest = longest_of(f.kind);
	f.label = label;
	fixups.push_back(f);
	for (int i = 0; i < f.length; ++i) byte(0x90);
}

void emitter::code_at(int position, int label)
{
	fixup f;
	f.position = position;
	f.kind = fixup_kind::code_word;
	f.length = f.longest = 2;
	f.label = label;
	fixups.push_back(f);
}

void emitter::data_at(int position, int offset)
{
	fixup f;
	f.position = position;
	f.kind = fixup_kind::data_word;
	f.length = f.longest = 2;
	f.value = offset;
	fixups.push_back(f);
}

int emitter::value_at(int position, int value)
{
	fixup f;
	f.position = position;
	f.kind = fixup_kind::value_word;
	f.length = f.longest = 2;
	f.value = value;
	fixups.push_back(f);
	return int(fixups.size()) - 1;
}

void emitter::set_value(int index, int value)
{
	fixups[size_t(index)].value = value;
}

emitter::position emitter::tell() const
{
	position p;
	p.code = code.size();
	p.fixups = fixups.size();
	p.labels = labels.size();
	return p;
}

void emitter::rewind(const position& p)
{
	code.resize(p.code);
	fixups.resize(p.fixups);
	labels.resize(p.labels);
}

void emitter::drop(int first, int last)
{
	dropped_.emplace_back(first, last);
}

void emitter::lay_out()
{
	// What is being removed, as byte ranges rather than labels, because the
	// labels inside are about to stop meaning anything.
	std::vector<std::pair<int, int>> gone;
	for (const auto& range : dropped_)
	{
		const int from = labels[size_t(range.first)];
		const int to = labels[size_t(range.second)];
		if (from >= 0 && to > from) gone.emplace_back(from, to);
	}
	std::sort(gone.begin(), gone.end());

	const auto inside_gone = [&](int position)
	{
		for (const auto& range : gone)
			if (position >= range.first && position < range.second) return true;
		return false;
	};

	// A dropped range swallows the fixups in it and becomes one edit of its
	// own, so that every edit is a disjoint span with a length to choose.
	struct edit
	{
		int position = 0;
		int longest = 0;
		int length = 0;
		int fixup = -1;
	};

	std::vector<edit> edits;
	for (size_t i = 0; i < fixups.size(); ++i)
	{
		if (inside_gone(fixups[i].position)) continue;
		edit e;
		e.position = fixups[i].position;
		e.longest = fixups[i].longest;
		e.length = fixups[i].longest;
		e.fixup = int(i);
		edits.push_back(e);
	}
	for (const auto& range : gone)
	{
		edit e;
		e.position = range.first;
		e.longest = range.second - range.first;
		e.length = 0;
		edits.push_back(e);
	}
	std::sort(edits.begin(), edits.end(),
	          [](const edit& a, const edit& b) { return a.position < b.position; });

	// Where an offset lands, given the lengths chosen so far. Everything an
	// edit ends at or before has moved down by what the edits before it saved.
	std::vector<int> saved_before(edits.size() + 1, 0);
	const auto recompute_savings = [&]
	{
		int total = 0;
		for (size_t i = 0; i < edits.size(); ++i)
		{
			saved_before[i] = total;
			total += edits[i].longest - edits[i].length;
		}
		saved_before[edits.size()] = total;
	};

	const auto moved = [&](int offset)
	{
		size_t low = 0, high = edits.size();
		while (low < high)
		{
			const size_t middle = (low + high) / 2;
			if (edits[middle].position + edits[middle].longest <= offset) low = middle + 1;
			else high = middle;
		}
		return offset - saved_before[low];
	};

	// Shrinking a branch only ever brings its target closer, so repeating
	// until nothing changes settles. A branch to the instruction after it goes
	// altogether, which is what the last return of an expanded body becomes.
	for (;;)
	{
		recompute_savings();
		bool changed = false;
		for (size_t i = 0; i < edits.size(); ++i)
		{
			edit& e = edits[i];
			if (e.fixup < 0) continue;
			const fixup& f = fixups[size_t(e.fixup)];
			if (f.kind != fixup_kind::jump && f.kind != fixup_kind::branch) continue;
			if (e.length == 0) continue;
			if (f.label < 0 || labels[size_t(f.label)] < 0) continue;

			const int start = e.position - saved_before[i];
			const int to = moved(labels[size_t(f.label)]);
			if (to == start)
			{
				e.length = 0;
				changed = true;
				continue;
			}
			if (e.length == 2) continue;
			const int displacement = to - (start + 2);
			if (displacement < -128 || displacement > 127) continue;
			e.length = 2;
			changed = true;
		}
		if (!changed) break;
	}
	recompute_savings();

	// Rewrite the code, moving what is left and writing the opcodes. The
	// operands wait for resolve(), which is when the data section is known.
	std::vector<uint8_t> output;
	output.reserve(code.size());
	int read = 0;
	for (size_t i = 0; i < edits.size(); ++i)
	{
		edit& e = edits[i];
		output.insert(output.end(), code.begin() + read, code.begin() + e.position);
		read = e.position + e.longest;
		const int start = int(output.size());
		if (e.fixup >= 0)
		{
			fixup& f = fixups[size_t(e.fixup)];
			switch (f.kind)
			{
			case fixup_kind::jump:
				output.push_back(uint8_t(e.length == 2 ? 0xeb : 0xe9));
				break;
			case fixup_kind::branch:
				if (e.length == 2)
				{
					output.push_back(uint8_t(codes_for(f.test).taken));
				}
				else
				{
					output.push_back(uint8_t(codes_for(f.test).inverse));
					output.push_back(0x03);
					output.push_back(0xe9);
				}
				break;
			case fixup_kind::call:
				output.push_back(0xe8);
				break;
			default:
				break;
			}
			while (int(output.size()) - start < e.length) output.push_back(0);
			f.position = start;
			f.length = e.length;
		}
	}
	output.insert(output.end(), code.begin() + read, code.end());

	for (int& offset : labels)
		if (offset >= 0) offset = moved(offset);

	// A fixup inside a dropped range no longer has anywhere to be. Everything
	// else already had its new position written above.
	std::vector<bool> laid(fixups.size(), false);
	for (const edit& e : edits)
		if (e.fixup >= 0) laid[size_t(e.fixup)] = true;
	std::vector<fixup> kept;
	kept.reserve(fixups.size());
	for (size_t i = 0; i < fixups.size(); ++i)
		if (laid[i]) kept.push_back(fixups[i]);
	fixups.swap(kept);

	code.swap(output);
	dropped_.clear();
}

bool emitter::resolve(int code_base, int data_base)
{
	for (const fixup& f : fixups)
	{
		switch (f.kind)
		{
		case fixup_kind::jump:
		case fixup_kind::branch:
		case fixup_kind::call:
		{
			if (f.label < 0 || labels[size_t(f.label)] < 0) return false;
			if (f.length == 0) break;
			const int target = labels[size_t(f.label)];
			if (f.kind == fixup_kind::branch && f.length == 5)
			{
				patch_word(f.position + 3, target - (f.position + f.length));
				break;
			}
			if (f.length == 2)
			{
				code[size_t(f.position) + 1] = uint8_t(target - (f.position + 2));
				break;
			}
			patch_word(f.position + 1, target - (f.position + f.length));
			break;
		}
		case fixup_kind::code_word:
			if (f.label < 0 || labels[size_t(f.label)] < 0) return false;
			patch_word(f.position, code_base + labels[size_t(f.label)]);
			break;
		case fixup_kind::data_word:
			patch_word(f.position, data_base + f.value);
			break;
		default:
			patch_word(f.position, f.value);
			break;
		}
	}
	return true;
}

void emitter::call_frame(int displacement) { byte(0xff); byte(0x96); word(displacement); }

void emitter::ret() { byte(0xc3); }
void emitter::interrupt(int number) { byte(0xcd); byte(number); }
