// ovrwin by Zac Walker
//
// The 8086 back end: it turns the instruction list into bytes, and it is the
// only part of the compiler that knows an opcode from a hole in the ground.
// A second back end is another file of the same shape - the list it consumes
// says nothing about which encoding an operation has.

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

	// The eight arithmetic operations share one encoding, picked out by three
	// bits that are both the opcode's middle field and the /r extension.
	int arithmetic_index(op code)
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
		case op::cmp: return 7;
		default: return -1;
		}
	}

	int shift_index(op code)
	{
		switch (code)
		{
		case op::rcl: return 2;
		case op::rcr: return 3;
		case op::shl: return 4;
		case op::shr: return 5;
		case op::sar: return 7;
		default: return -1;
		}
	}

	bool is_byte(int32_t v) { return v >= -128 && v <= 127; }

	// A place to put bytes that can also just count them, so that measuring an
	// instruction and encoding it cannot disagree.
	struct sink
	{
		std::vector<uint8_t>* into;
		int count = 0;

		void put(int value)
		{
			if (into != nullptr) into->push_back(uint8_t(value));
			++count;
		}

		void word(int32_t value)
		{
			put(value & 0xff);
			put((value >> 8) & 0xff);
		}
	};

	// The 16-bit mod/rm forms. An 8086 addresses memory through four registers
	// and only in these combinations.
	bool memory_form(const operand& o, int& rm, int& mod)
	{
		const int base = o.reg;
		const int index = o.index;
		if (base == reg_none && index == reg_none)
		{
			rm = 6;
			mod = 0;
			return true;
		}
		if (index == reg_none)
		{
			switch (base)
			{
			case reg_si: rm = 4; break;
			case reg_di: rm = 5; break;
			case reg_bp: rm = 6; break;
			case reg_b: rm = 7; break;
			default: return false;
			}
		}
		else if (base == reg_b && index == reg_si) rm = 0;
		else if (base == reg_b && index == reg_di) rm = 1;
		else if (base == reg_bp && index == reg_si) rm = 2;
		else if (base == reg_bp && index == reg_di) rm = 3;
		else return false;

		if (o.value == 0 && rm != 6) mod = 0;
		else if (is_byte(o.value)) mod = 1;
		else mod = 2;
		return true;
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

// ---------------------------------------------------------------------------
// Building the list
// ---------------------------------------------------------------------------

instruction& emitter::emit(op code, int width, const operand& dest, const operand& source)
{
	instruction in;
	in.code = code;
	in.width = uint8_t(width);
	in.dest = dest;
	in.source = source;
	this->code.push_back(in);
	break_run = false;
	return this->code.back();
}

void emitter::byte(int value)
{
	if (!break_run && !code.empty() && code.back().code == op::literal &&
		code.back().literal_at + code.back().literal_length == int32_t(literals.size()))
	{
		literals.push_back(uint8_t(value));
		++code.back().literal_length;
		return;
	}
	instruction in;
	in.code = op::literal;
	in.literal_at = int32_t(literals.size());
	in.literal_length = 1;
	literals.push_back(uint8_t(value));
	code.push_back(in);
	break_run = false;
}

void emitter::bytes(std::initializer_list<int> run)
{
	for (const int value : run) byte(value);
}

namespace
{
	// The one operand of the instruction just emitted that can carry a
	// reference: an absolute address, or an immediate that stands for one.
	// An absolute address wins, because an instruction that has both is
	// storing a plain number at a named place.
	operand* referring_operand(instruction& in)
	{
		const auto absolute = [](const operand& o)
		{
			return o.kind == operand_kind::memory && o.reg == reg_none && o.index == reg_none;
		};
		if (absolute(in.source)) return &in.source;
		if (absolute(in.dest)) return &in.dest;
		if (in.source.kind == operand_kind::immediate) return &in.source;
		if (in.dest.kind == operand_kind::immediate) return &in.dest;
		return nullptr;
	}
}

void emitter::refers_to_data(int32_t offset)
{
	operand* o = referring_operand(code.back());
	o->reference = reference_kind::data;
	o->target = offset;
}

void emitter::refers_to_code(int label)
{
	operand* o = referring_operand(code.back());
	o->reference = reference_kind::code;
	o->target = label;
}

int emitter::refers_to_deferred()
{
	const int slot = defer(0);
	operand* o = referring_operand(code.back());
	o->reference = reference_kind::deferred;
	o->target = slot;
	return slot;
}

int emitter::label()
{
	labels.push_back(-1);
	return int(labels.size()) - 1;
}

void emitter::bind(int label) { labels[size_t(label)] = at(); break_run = true; }

int emitter::here()
{
	labels.push_back(at());
	break_run = true;
	return int(labels.size()) - 1;
}

int emitter::offset_of(int label) const
{
	const int index = labels[size_t(label)];
	if (index < 0) return -1;
	if (index >= int(code.size())) return int(image.size());
	return code[size_t(index)].offset;
}

int emitter::defer(int32_t value)
{
	deferred.push_back(value);
	return int(deferred.size()) - 1;
}

void emitter::set_deferred(int slot, int32_t value) { deferred[size_t(slot)] = value; }

emitter::position emitter::tell() const
{
	position p;
	p.code = code.size();
	p.literals = literals.size();
	p.labels = labels.size();
	p.deferred = deferred.size();
	return p;
}

void emitter::rewind(const position& p)
{
	code.resize(p.code);
	literals.resize(p.literals);
	labels.resize(p.labels);
	deferred.resize(p.deferred);
}

void emitter::drop(int first, int last) { dropped_.emplace_back(first, last); }

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Writes one instruction, or counts its bytes when `into` is null. `position`
// is where it starts, which only a relative branch needs.
int emitter::encode(const instruction& in, int position, std::vector<uint8_t>* into) const
{
	sink out{into};
	const int w = in.width == 1 ? 0 : 1;

	// A reference resolves to a number here; the ones that depend on the data
	// section are written again by resolve().
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

	// mod/rm, with the register or extension in the middle field.
	const auto modrm = [&](const operand& o, int field)
	{
		if (o.kind == operand_kind::reg)
		{
			out.put(0xc0 | (field << 3) | o.reg);
			return;
		}
		int rm = 0, mod = 0;
		memory_form(o, rm, mod);
		out.put((mod << 6) | (field << 3) | rm);
		if (mod == 0 && rm == 6) out.word(value_of(o));
		else if (mod == 1) out.put(o.value & 0xff);
		else if (mod == 2) out.word(o.value);
	};

	switch (in.code)
	{
	case op::nop:
		break;

	case op::literal:
		for (int i = 0; i < in.literal_length; ++i)
			out.put(literals[size_t(in.literal_at) + size_t(i)]);
		break;

	case op::mov:
		if (in.dest.kind == operand_kind::reg && in.source.kind == operand_kind::immediate)
		{
			out.put((w != 0 ? 0xb8 : 0xb0) + in.dest.reg);
			if (w != 0) out.word(value_of(in.source));
			else out.put(value_of(in.source) & 0xff);
			break;
		}
		// The accumulator reaches an absolute address in one byte less.
		if (in.dest.kind == operand_kind::reg && in.dest.reg == reg_a &&
			in.source.kind == operand_kind::memory && in.source.reg == reg_none &&
			in.source.index == reg_none)
		{
			out.put(0xa0 + w);
			out.word(value_of(in.source));
			break;
		}
		if (in.source.kind == operand_kind::reg && in.source.reg == reg_a &&
			in.dest.kind == operand_kind::memory && in.dest.reg == reg_none &&
			in.dest.index == reg_none)
		{
			out.put(0xa2 + w);
			out.word(value_of(in.dest));
			break;
		}
		if (in.source.kind == operand_kind::immediate)
		{
			out.put(0xc6 + w);
			modrm(in.dest, 0);
			if (w != 0) out.word(value_of(in.source));
			else out.put(value_of(in.source) & 0xff);
			break;
		}
		if (in.source.kind == operand_kind::reg)
		{
			out.put(0x88 + w);
			modrm(in.dest, in.source.reg);
			break;
		}
		out.put(0x8a + w);
		modrm(in.source, in.dest.reg);
		break;

	case op::lea:
		out.put(0x8d);
		modrm(in.source, in.dest.reg);
		break;

	case op::movsx:
	case op::movzx:
		out.put(0x0f);
		out.put(in.code == op::movzx ? 0xb6 : 0xbe); // the source is always a byte
		modrm(in.source, in.dest.reg);
		break;

	case op::add: case op::adc: case op::sub: case op::sbb:
	case op::and_: case op::or_: case op::xor_: case op::cmp:
	{
		const int index = arithmetic_index(in.code);
		if (in.source.kind == operand_kind::immediate)
		{
			const int32_t value = value_of(in.source);
			const bool plain = in.source.reference == reference_kind::none;
			// The accumulator has a short form, and so does an operand that
			// fits in a sign-extended byte.
			if (in.dest.kind == operand_kind::reg && in.dest.reg == reg_a && (w == 0 || !plain ||
				!is_byte(value)))
			{
				out.put((index << 3) | 0x04 | w);
				if (w != 0) out.word(value);
				else out.put(value & 0xff);
				break;
			}
			if (w != 0 && plain && is_byte(value))
			{
				out.put(0x83);
				modrm(in.dest, index);
				out.put(value & 0xff);
				break;
			}
			out.put(0x80 + w);
			modrm(in.dest, index);
			if (w != 0) out.word(value);
			else out.put(value & 0xff);
			break;
		}
		if (in.source.kind == operand_kind::reg)
		{
			out.put((index << 3) | w);
			modrm(in.dest, in.source.reg);
			break;
		}
		out.put((index << 3) | 0x02 | w);
		modrm(in.source, in.dest.reg);
		break;
	}

	case op::test:
		if (in.source.kind == operand_kind::immediate)
		{
			if (in.dest.kind == operand_kind::reg && in.dest.reg == reg_a)
			{
				out.put(0xa8 + w);
				if (w != 0) out.word(value_of(in.source));
				else out.put(value_of(in.source) & 0xff);
				break;
			}
			out.put(0xf6 + w);
			modrm(in.dest, 0);
			if (w != 0) out.word(value_of(in.source));
			else out.put(value_of(in.source) & 0xff);
			break;
		}
		out.put(0x84 + w);
		modrm(in.dest, in.source.reg);
		break;

	case op::inc:
	case op::dec:
	{
		const int field = in.code == op::inc ? 0 : 1;
		if (w != 0 && in.dest.kind == operand_kind::reg)
		{
			out.put((in.code == op::inc ? 0x40 : 0x48) + in.dest.reg);
			break;
		}
		out.put(0xfe + w);
		modrm(in.dest, field);
		break;
	}

	case op::neg:
	case op::not_:
	case op::mul:
	case op::imul:
	case op::div:
	case op::idiv:
	{
		int field = 0;
		switch (in.code)
		{
		case op::not_: field = 2; break;
		case op::neg: field = 3; break;
		case op::mul: field = 4; break;
		case op::imul: field = 5; break;
		case op::div: field = 6; break;
		default: field = 7; break;
		}
		out.put(0xf6 + w);
		modrm(in.dest, field);
		break;
	}

	case op::shl: case op::shr: case op::sar: case op::rcl: case op::rcr:
	{
		const int field = shift_index(in.code);
		const int32_t by = in.source.value;
		if (by < 0) out.put(0xd2 + w); // by CL
		else out.put(0xd0 + w); // by one
		modrm(in.dest, field);
		break;
	}

	case op::push:
	case op::pop:
		if (in.dest.kind == operand_kind::reg)
		{
			out.put((in.code == op::push ? 0x50 : 0x58) + in.dest.reg);
			break;
		}
		if (in.code == op::push)
		{
			out.put(0xff);
			modrm(in.dest, 6);
		}
		else
		{
			out.put(0x8f);
			modrm(in.dest, 0);
		}
		break;

	case op::xchg:
		if (in.dest.kind == operand_kind::reg && in.source.kind == operand_kind::reg &&
			(in.dest.reg == reg_a || in.source.reg == reg_a) && w != 0)
		{
			out.put(0x90 + (in.dest.reg == reg_a ? in.source.reg : in.dest.reg));
			break;
		}
		out.put(0x86 + w);
		modrm(in.dest, in.source.reg);
		break;

	case op::cbw: out.put(0x98); break;
	case op::cwd: out.put(0x99); break;
	case op::ret: out.put(0xc3); break;

	case op::interrupt:
		out.put(0xcd);
		out.put(in.dest.value & 0xff);
		break;

	case op::setcc:
		// A 386 sets the byte and clears the rest; an 8086 has to branch
		// over a clear, because it has no SETcc at all.
		if (cpu >= cc_cpu::i80386 && in.dest.reg < 4)
		{
			out.put(0x0f);
			out.put(codes_for(in.test).taken + 0x20);
			out.put(0xc0 | in.dest.reg);
			if (w != 0)
			{
				out.put(0x30); // xor high,high
				out.put(0xc0 | ((in.dest.reg + 4) << 3) | (in.dest.reg + 4));
			}
			break;
		}
		out.put((w != 0 ? 0xb8 : 0xb0) + in.dest.reg);
		if (w != 0) out.word(1);
		else out.put(1);
		out.put(codes_for(in.test).taken);
		out.put(0x02);
		out.put(0x30 | w);
		out.put(0xc0 | (in.dest.reg << 3) | in.dest.reg);
		break;

	case op::call:
		out.put(0xe8);
		out.word(0);
		break;

	case op::call_at:
		out.put(0xff);
		modrm(in.dest, 2);
		break;

	case op::jmp:
		if (in.length == 2)
		{
			out.put(0xeb);
			out.put(0);
			break;
		}
		out.put(0xe9);
		out.word(0);
		break;

	case op::jcc:
		if (in.length == 2)
		{
			out.put(codes_for(in.test).taken);
			out.put(0);
			break;
		}
		// A 386 branches that far in one instruction. An 8086 has no near
		// conditional branch, so it jumps over an unconditional one.
		if (cpu >= cc_cpu::i80386)
		{
			out.put(0x0f);
			out.put(codes_for(in.test).taken + 0x10);
			out.word(0);
			break;
		}
		out.put(codes_for(in.test).inverse);
		out.put(0x03);
		out.put(0xe9);
		out.word(0);
		break;
	}

	(void)position;
	return out.count;
}

// ---------------------------------------------------------------------------
// Laying it out
// ---------------------------------------------------------------------------

void emitter::lay_out(int code_base)
{
	(void)code_base;

	// What is being dropped, as instruction ranges.
	std::vector<bool> alive(code.size(), true);
	for (const auto& range : dropped_)
	{
		const int from = labels[size_t(range.first)];
		const int to = labels[size_t(range.second)];
		if (from < 0 || to <= from) continue;
		for (int i = from; i < to && i < int(code.size()); ++i) alive[size_t(i)] = false;
	}

	// Every branch starts at its longest, so the pass can only shorten.
	for (instruction& in : code)
	{
		if (in.code == op::jmp) in.length = 3;
		else if (in.code == op::jcc) in.length = cpu >= cc_cpu::i80386 ? 4 : 5;
		else in.length = 0;
	}

	const auto place = [&]
	{
		int offset = 0;
		for (size_t i = 0; i < code.size(); ++i)
		{
			code[i].offset = offset;
			if (!alive[i]) continue;
			if (code[i].code == op::jmp || code[i].code == op::jcc) offset += code[i].length;
			else offset += encode(code[i], offset, nullptr);
		}
		return offset;
	};

	// Shortening a branch only brings its target closer, so this settles.
	place();
	for (;;)
	{
		bool changed = false;
		for (size_t i = 0; i < code.size(); ++i)
		{
			instruction& in = code[i];
			if (!alive[i]) continue;
			if (in.code != op::jmp && in.code != op::jcc) continue;
			if (in.length == 0) continue;
			if (in.label < 0 || labels[size_t(in.label)] < 0) continue;

			const int index = labels[size_t(in.label)];
			const int target = index < int(code.size()) ? code[size_t(index)].offset : place();
			if (target == in.offset)
			{
				// A branch to the instruction after it does nothing at all,
				// which is what the last return of an expanded body becomes.
				in.length = 0;
				changed = true;
				continue;
			}
			if (in.length == 2) continue;
			const int displacement = target - (in.offset + 2);
			if (!is_byte(displacement)) continue;
			in.length = 2;
			changed = true;
		}
		place();
		if (!changed) break;
	}

	image.clear();
	for (size_t i = 0; i < code.size(); ++i)
	{
		if (!alive[i]) continue;
		code[i].offset = int32_t(image.size());
		const int length = encode(code[i], code[i].offset, &image);
		code[i].length = int32_t(length);
	}

	// A label bound to a dropped instruction lands on whatever took its place,
	// which is harmless because nothing that survived refers to it. Its length
	// goes to zero so that resolve() knows there is nothing there to write to.
	for (size_t i = code.size(); i > 0; --i)
	{
		if (alive[i - 1]) continue;
		code[i - 1].offset = i < code.size() ? code[i].offset : int32_t(image.size());
		code[i - 1].length = 0;
	}

	dropped_.clear();
}

bool emitter::resolve(int code_base, int data_base)
{
	const auto put_word = [&](int at, int value)
	{
		image[size_t(at)] = uint8_t(value & 0xff);
		image[size_t(at) + 1] = uint8_t((value >> 8) & 0xff);
	};

	for (const instruction& in : code)
	{
		// Nothing was written for it: either it was dropped, or it is a branch
		// that turned out to go to the instruction after it.
		if (in.length == 0) continue;

		if (in.code == op::jmp || in.code == op::jcc || in.code == op::call)
		{
			if (in.label < 0 || labels[size_t(in.label)] < 0) return false;
			const int index = labels[size_t(in.label)];
			const int target = index < int(code.size()) ? code[size_t(index)].offset : int(image.size());
			if (in.length == 2)
			{
				image[size_t(in.offset) + 1] = uint8_t(target - (in.offset + 2));
				continue;
			}
			if (in.code == op::jcc)
				put_word(in.offset + (cpu >= cc_cpu::i80386 ? 2 : 3), target - (in.offset + in.length));
			else put_word(in.offset + 1, target - (in.offset + in.length));
			continue;
		}

		// An operand that names an address only became a number once the data
		// section was placed, so it is written again here.
		for (const operand* o : {&in.dest, &in.source})
		{
			if (o->reference == reference_kind::none) continue;
			int value = 0;
			if (o->reference == reference_kind::deferred) value = deferred[size_t(o->target)];
			else if (o->reference == reference_kind::data) value = data_base + o->target;
			else
			{
				if (o->target < 0 || labels[size_t(o->target)] < 0) return false;
				const int index = labels[size_t(o->target)];
				value = code_base +
					(index < int(code.size()) ? code[size_t(index)].offset : int(image.size()));
			}
			// An address in a memory operand is the displacement, which follows
			// the opcode and the mod/rm byte - or the opcode alone in the
			// accumulator short form. An address used as an immediate is the
			// last word the instruction wrote.
			int at = in.offset + in.length - 2;
			if (o->kind == operand_kind::memory)
			{
				const bool accumulator = in.code == op::mov &&
					((in.dest.kind == operand_kind::reg && in.dest.reg == reg_a && o == &in.source) ||
						(in.source.kind == operand_kind::reg && in.source.reg == reg_a && o == &in.dest));
				at = in.offset + (accumulator ? 1 : 2);
			}
			put_word(at, value);
		}
	}
	return true;
}
