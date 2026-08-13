// ovrwin by Zac Walker
//
// The x64 back end: the same instruction list as the 8086 one, encoded for the
// machine this compiler is running on. It is deliberately a second file of the
// same shape rather than a parameterisation of the first, because the two
// differ in the things an encoder is made of - the mod/rm table, the prefixes,
// how far a branch reaches - and agree on nothing but the opcode bytes.
//
// The list still says 16 bits, so this emits 16-bit operations: on x64 that is
// a 66h prefix in front of the same opcodes, which keeps the arithmetic and the
// flags identical to what the interpreter would have produced. The stack and
// frame pointers are the exception - those are the host's real RSP and RBP, so
// anything naming them is 64-bit.
//
// What this cannot do is a pointer: the front end still makes them 16 bits
// wide, so an address that has to survive in a register does not fit. Globals
// are reached RIP-relative and locals through RBP, which is enough for
// arithmetic, control flow and calls, and not enough for arrays or pointers.

#include "emit.h"

namespace
{
	bool is_byte_value(int32_t v) { return v >= -128 && v <= 127; }

	// The three bits an arithmetic operation puts in the opcode and in /r.
	int arithmetic_index_x64(op code)
	{
		switch (code)
		{
		case op::add: return 0;
		case op::or_: return 1;
		case op::adc: return 2;
		case op::sbb: return 3;
		case op::and_: return 4;
		case op::sub: return 5;
		case op::xor_: return 6;
		default: return 7; // cmp
		}
	}

	int shift_index_x64(op code)
	{
		switch (code)
		{
		case op::rcl: return 2;
		case op::rcr: return 3;
		case op::shl: return 4;
		case op::shr: return 5;
		default: return 7; // sar
		}
	}

	int taken_code(condition c)
	{
		switch (c)
		{
		case condition::equal: return 0x74;
		case condition::not_equal: return 0x75;
		case condition::less: return 0x7c;
		case condition::less_equal: return 0x7e;
		case condition::greater: return 0x7f;
		case condition::greater_equal: return 0x7d;
		case condition::below: return 0x72;
		case condition::below_equal: return 0x76;
		case condition::above: return 0x77;
		default: return 0x73; // above_equal
		}
	}

	// Whether an operand names the stack or the frame pointer as a value. Those
	// are the host's own registers and have to stay 64 bits wide; everything
	// else in the list is a 16-bit quantity.
	bool names_pointer(const operand& o)
	{
		return o.kind == operand_kind::reg && (o.reg == reg_sp || o.reg == reg_bp);
	}
}

int emitter::encode_x64(instruction& in, int position, std::vector<uint8_t>* into) const
{
	int count = 0;
	const auto put = [&](int value)
	{
		if (into != nullptr) into->push_back(uint8_t(value));
		++count;
	};
	const auto word = [&](int32_t value)
	{
		put(value & 0xff);
		put((value >> 8) & 0xff);
	};
	const auto dword = [&](int32_t value)
	{
		word(value);
		put((value >> 16) & 0xff);
		put((value >> 24) & 0xff);
	};

	const auto value_of = [&](const operand& o) -> int32_t
	{
		if (o.reference == reference_kind::deferred) return deferred[size_t(o.target)];
		if (o.reference == reference_kind::code && o.target >= 0)
		{
			const int index = labels[size_t(o.target)];
			if (index >= 0 && index < int(code.size())) return code[size_t(index)].offset;
		}
		return o.value;
	};

	const bool pointer_sized = names_pointer(in.dest) || names_pointer(in.source);

	// 66h narrows the operation to 16 bits; REX.W widens it to 64. A byte
	// operation needs neither.
	const auto prefix = [&]
	{
		if (pointer_sized) put(0x48);
		else if (in.width != 1) put(0x66);
	};

	// The 64-bit mod/rm forms. A displacement off RBP always needs one, and an
	// absolute address becomes a distance from the instruction after this one.
	const auto modrm = [&](const operand& o, int field)
	{
		if (o.kind == operand_kind::reg)
		{
			put(0xc0 | ((field & 7) << 3) | (o.reg & 7));
			return;
		}
		if (o.reg == reg_none)
		{
			put(0x00 | ((field & 7) << 3) | 5); // RIP-relative
			in.fixup = count;
			dword(0);
			return;
		}
		const int rm = o.reg & 7;
		if (o.value == 0 && rm != reg_bp)
		{
			put(0x00 | ((field & 7) << 3) | rm);
			return;
		}
		if (is_byte_value(o.value))
		{
			put(0x40 | ((field & 7) << 3) | rm);
			put(o.value & 0xff);
			return;
		}
		put(0x80 | ((field & 7) << 3) | rm);
		dword(o.value);
	};

	// An immediate is as wide as the operation, except that a pointer-sized one
	// is still only four bytes.
	const auto immediate = [&](int32_t value)
	{
		if (in.width == 1 && !pointer_sized) put(value & 0xff);
		else if (pointer_sized) dword(value);
		else word(value);
	};

	switch (in.code)
	{
	case op::nop:
		break;

	case op::literal:
		// Hand-assembled 8086 bytes mean nothing here, but the length has to be
		// right or everything after them moves.
		for (int i = 0; i < in.literal_length; ++i) put(literals[size_t(in.literal_at) + size_t(i)]);
		break;

	case op::mov:
		if (in.dest.kind == operand_kind::reg && in.source.kind == operand_kind::immediate)
		{
			if (pointer_sized)
			{
				put(0x48);
				put(0xc7);
				put(0xc0 | (in.dest.reg & 7));
				in.fixup = count;
				dword(value_of(in.source));
				break;
			}
			if (in.width != 1) put(0x66);
			put((in.width != 1 ? 0xb8 : 0xb0) + (in.dest.reg & 7));
			in.fixup = count;
			immediate(value_of(in.source));
			break;
		}
		if (in.source.kind == operand_kind::immediate)
		{
			prefix();
			put(in.width == 1 ? 0xc6 : 0xc7);
			modrm(in.dest, 0);
			const int32_t at = count;
			immediate(value_of(in.source));
			if (in.source.reference != reference_kind::none) in.fixup = at;
			break;
		}
		prefix();
		if (in.source.kind == operand_kind::reg)
		{
			put(in.width == 1 ? 0x88 : 0x89);
			modrm(in.dest, in.source.reg);
			break;
		}
		put(in.width == 1 ? 0x8a : 0x8b);
		modrm(in.source, in.dest.reg);
		break;

	case op::lea:
		prefix();
		put(0x8d);
		modrm(in.source, in.dest.reg);
		break;

	case op::movsx:
	case op::movzx:
		if (in.width != 1) put(0x66);
		put(0x0f);
		put(in.code == op::movzx ? 0xb6 : 0xbe);
		modrm(in.source, in.dest.reg);
		break;

	case op::add: case op::adc: case op::sub: case op::sbb:
	case op::and_: case op::or_: case op::xor_: case op::cmp:
	{
		const int index = arithmetic_index_x64(in.code);
		if (in.source.kind == operand_kind::immediate)
		{
			const int32_t value = value_of(in.source);
			const bool plain = in.source.reference == reference_kind::none;
			prefix();
			if (in.width != 1 && plain && is_byte_value(value))
			{
				put(0x83);
				modrm(in.dest, index);
				put(value & 0xff);
				break;
			}
			put(in.width == 1 ? 0x80 : 0x81);
			modrm(in.dest, index);
			const int32_t at = count;
			immediate(value);
			if (!plain) in.fixup = at;
			break;
		}
		prefix();
		if (in.source.kind == operand_kind::reg)
		{
			put((index << 3) | (in.width == 1 ? 0 : 1));
			modrm(in.dest, in.source.reg);
			break;
		}
		put((index << 3) | (in.width == 1 ? 2 : 3));
		modrm(in.source, in.dest.reg);
		break;
	}

	case op::test:
		prefix();
		if (in.source.kind == operand_kind::immediate)
		{
			put(in.width == 1 ? 0xf6 : 0xf7);
			modrm(in.dest, 0);
			immediate(value_of(in.source));
			break;
		}
		put(in.width == 1 ? 0x84 : 0x85);
		modrm(in.dest, in.source.reg);
		break;

	case op::inc:
	case op::dec:
		// x64 has no one-byte inc: those opcodes became the REX prefixes.
		prefix();
		put(in.width == 1 ? 0xfe : 0xff);
		modrm(in.dest, in.code == op::inc ? 0 : 1);
		break;

	case op::neg:
	case op::not_:
		prefix();
		put(in.width == 1 ? 0xf6 : 0xf7);
		modrm(in.dest, in.code == op::neg ? 3 : 2);
		break;

	case op::mul:
	case op::imul:
	case op::div:
	case op::idiv:
	{
		const int field = in.code == op::mul ? 4 : in.code == op::imul ? 5 : in.code == op::div ? 6 : 7;
		prefix();
		put(in.width == 1 ? 0xf6 : 0xf7);
		modrm(in.dest, field);
		break;
	}

	case op::shl: case op::shr: case op::sar: case op::rcl: case op::rcr:
	{
		const int field = shift_index_x64(in.code);
		prefix();
		// A count of -1 means CL, the only variable count the machine has.
		put(in.source.value < 0 ? (in.width == 1 ? 0xd2 : 0xd3) : (in.width == 1 ? 0xd0 : 0xd1));
		modrm(in.dest, field);
		break;
	}

	case op::push:
	case op::pop:
		// A push is 64-bit whatever else happens, so only a 16-bit one needs
		// saying. The frame pointer wants the whole register.
		if (!pointer_sized) put(0x66);
		if (in.dest.kind == operand_kind::reg)
		{
			put((in.code == op::push ? 0x50 : 0x58) + (in.dest.reg & 7));
			break;
		}
		put(in.code == op::push ? 0xff : 0x8f);
		modrm(in.dest, in.code == op::push ? 6 : 0);
		break;

	case op::xchg:
		prefix();
		put(in.width == 1 ? 0x86 : 0x87);
		modrm(in.dest, in.source.reg);
		break;

	case op::cbw:
		put(0x66);
		put(0x98);
		break;

	case op::cwd:
		put(0x66);
		put(0x99);
		break;

	case op::setcc:
		put(0x0f);
		put(taken_code(in.test) + 0x20);
		put(0xc0 | (in.dest.reg & 7));
		if (in.width != 1)
		{
			put(0x66);
			put(0x0f);
			put(0xb6);
			put(0xc0 | ((in.dest.reg & 7) << 3) | (in.dest.reg & 7));
		}
		break;

	case op::call:
		put(0xe8);
		in.fixup = count;
		dword(0);
		break;

	case op::call_at:
		put(0xff);
		modrm(in.dest, 2);
		break;

	case op::ret:
		put(0xc3);
		break;

	case op::interrupt:
		// There is no DOS here. Reaching one is a bug, and this says so.
		put(0x0f);
		put(0x0b);
		break;

	case op::jmp:
		if (in.length == 2)
		{
			put(0xeb);
			in.fixup = count;
			put(0);
			break;
		}
		put(0xe9);
		in.fixup = count;
		dword(0);
		break;

	case op::jcc:
		if (in.length == 2)
		{
			put(taken_code(in.test));
			in.fixup = count;
			put(0);
			break;
		}
		put(0x0f);
		put(taken_code(in.test) + 0x10);
		in.fixup = count;
		dword(0);
		break;
	}

	(void)position;
	return count;
}
