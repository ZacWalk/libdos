/* ovrwin by Zac Walker
 *
 * The x64 back end: the same instruction list as the 8086 one, encoded for the
 * machine this compiler is running on. It is deliberately a second file of the
 * same shape rather than a parameterisation of the first, because the two
 * differ in the things an encoder is made of - the mod/rm table, the prefixes,
 * how far a branch reaches - and agree on nothing but the opcode bytes.
 *
 * The list still says 16 bits, so this emits 16-bit operations: on x64 that is
 * a 66h prefix in front of the same opcodes, which keeps the arithmetic and the
 * flags identical to what the interpreter would have produced. The stack and
 * frame pointers are the exception - those are the host's real RSP and RBP, so
 * anything naming them is 64-bit.
 *
 * What this cannot do is a pointer: the front end still makes them 16 bits
 * wide, so an address that has to survive in a register does not fit. Globals
 * are reached RIP-relative and locals through RBP, which is enough for
 * arithmetic, control flow and calls, and not enough for arrays or pointers.
 */

#include "emit.h"

typedef struct x64_sink
{
	const emitter* e;
	instruction* in;
	cc_list* into;
	int count;
	int pointer_sized;
} x64_sink;

static int is_byte_value(int32_t v)
{
	return v >= -128 && v <= 127;
}

/* The three bits an arithmetic operation puts in the opcode and in /r. */
static int arithmetic_index_x64(op code)
{
	switch (code)
	{
	case op_add: return 0;
	case op_or_: return 1;
	case op_adc: return 2;
	case op_sbb: return 3;
	case op_and_: return 4;
	case op_sub: return 5;
	case op_xor_: return 6;
	default: return 7; /* cmp */
	}
}

static int shift_index_x64(op code)
{
	switch (code)
	{
	case op_rcl: return 2;
	case op_rcr: return 3;
	case op_shl: return 4;
	case op_shr: return 5;
	default: return 7; /* sar */
	}
}

static int taken_code(condition c)
{
	switch (c)
	{
	case cond_equal: return 0x74;
	case cond_not_equal: return 0x75;
	case cond_less: return 0x7c;
	case cond_less_equal: return 0x7e;
	case cond_greater: return 0x7f;
	case cond_greater_equal: return 0x7d;
	case cond_below: return 0x72;
	case cond_below_equal: return 0x76;
	case cond_above: return 0x77;
	default: return 0x73; /* above_equal */
	}
}

/* Whether an operand names the stack or the frame pointer as a value. Those are
 * the host's own registers and have to stay 64 bits wide; everything else in
 * the list is a 16-bit quantity. */
static int names_pointer(const operand* o)
{
	return o->kind == operand_kind_reg && (o->reg == reg_sp || o->reg == reg_bp);
}

static void x64_put(x64_sink* out, int value)
{
	if (out->into != NULL)
	{
		uint8_t* slot = (uint8_t*)cc_list_add(out->into);
		if (slot != NULL) *slot = (uint8_t)value;
	}
	++out->count;
}

static void x64_word(x64_sink* out, int32_t value)
{
	x64_put(out, value & 0xff);
	x64_put(out, (value >> 8) & 0xff);
}

static void x64_dword(x64_sink* out, int32_t value)
{
	x64_word(out, value);
	x64_put(out, (value >> 16) & 0xff);
	x64_put(out, (value >> 24) & 0xff);
}

static int32_t x64_value_of(const emitter* e, const operand* o)
{
	if (o->reference == reference_kind_deferred) return ((const int32_t*)e->deferred.items)[o->target];
	if (o->reference == reference_kind_code && o->target >= 0)
	{
		const int index = EMIT_LABEL(*e, o->target);
		if (index >= 0 && index < e->code.count) return EMIT_CODE(*e, index).offset;
	}
	return o->value;
}

/* 66h narrows the operation to 16 bits; REX.W widens it to 64. A byte operation
 * needs neither. */
static void x64_prefix(x64_sink* out)
{
	if (out->pointer_sized) x64_put(out, 0x48);
	else if (out->in->width != 1) x64_put(out, 0x66);
}

/* The 64-bit mod/rm forms. A displacement off RBP always needs one, and an
 * absolute address becomes a distance from the instruction after this one. */
static void x64_modrm(x64_sink* out, const operand* o, int field)
{
	int rm;

	if (o->kind == operand_kind_reg)
	{
		x64_put(out, 0xc0 | ((field & 7) << 3) | (o->reg & 7));
		return;
	}
	if (o->reg == reg_none)
	{
		x64_put(out, 0x00 | ((field & 7) << 3) | 5); /* RIP-relative */
		out->in->fixup = out->count;
		x64_dword(out, 0);
		return;
	}
	rm = o->reg & 7;
	if (o->value == 0 && rm != reg_bp)
	{
		x64_put(out, 0x00 | ((field & 7) << 3) | rm);
		return;
	}
	if (is_byte_value(o->value))
	{
		x64_put(out, 0x40 | ((field & 7) << 3) | rm);
		x64_put(out, o->value & 0xff);
		return;
	}
	x64_put(out, 0x80 | ((field & 7) << 3) | rm);
	x64_dword(out, o->value);
}

/* An immediate is as wide as the operation, except that a pointer-sized one is
 * still only four bytes. */
static void x64_immediate(x64_sink* out, int32_t value)
{
	if (out->in->width == 1 && !out->pointer_sized) x64_put(out, value & 0xff);
	else if (out->pointer_sized) x64_dword(out, value);
	else x64_word(out, value);
}

int emit_encode_x64(const emitter* e, instruction* in, int position, cc_list* into)
{
	x64_sink out;

	out.e = e;
	out.in = in;
	out.into = into;
	out.count = 0;
	out.pointer_sized = names_pointer(&in->dest) || names_pointer(&in->source);

	switch (in->code)
	{
	case op_nop:
		break;

	case op_literal:
	{
		/* Hand-assembled 8086 bytes mean nothing here, but the length has to be
		 * right or everything after them moves. */
		int i;
		for (i = 0; i < in->literal_length; ++i)
			x64_put(&out, ((const uint8_t*)e->literals.items)[in->literal_at + i]);
		break;
	}

	case op_mov:
		if (in->dest.kind == operand_kind_reg && in->source.kind == operand_kind_immediate)
		{
			if (out.pointer_sized)
			{
				x64_put(&out, 0x48);
				x64_put(&out, 0xc7);
				x64_put(&out, 0xc0 | (in->dest.reg & 7));
				in->fixup = out.count;
				x64_dword(&out, x64_value_of(e, &in->source));
				break;
			}
			if (in->width != 1) x64_put(&out, 0x66);
			x64_put(&out, (in->width != 1 ? 0xb8 : 0xb0) + (in->dest.reg & 7));
			in->fixup = out.count;
			x64_immediate(&out, x64_value_of(e, &in->source));
			break;
		}
		if (in->source.kind == operand_kind_immediate)
		{
			int32_t at;
			x64_prefix(&out);
			x64_put(&out, in->width == 1 ? 0xc6 : 0xc7);
			x64_modrm(&out, &in->dest, 0);
			at = out.count;
			x64_immediate(&out, x64_value_of(e, &in->source));
			if (in->source.reference != reference_kind_none) in->fixup = at;
			break;
		}
		x64_prefix(&out);
		if (in->source.kind == operand_kind_reg)
		{
			x64_put(&out, in->width == 1 ? 0x88 : 0x89);
			x64_modrm(&out, &in->dest, in->source.reg);
			break;
		}
		x64_put(&out, in->width == 1 ? 0x8a : 0x8b);
		x64_modrm(&out, &in->source, in->dest.reg);
		break;

	case op_lea:
		x64_prefix(&out);
		x64_put(&out, 0x8d);
		x64_modrm(&out, &in->source, in->dest.reg);
		break;

	case op_movsx:
	case op_movzx:
		if (in->width != 1) x64_put(&out, 0x66);
		x64_put(&out, 0x0f);
		x64_put(&out, in->code == op_movzx ? 0xb6 : 0xbe);
		x64_modrm(&out, &in->source, in->dest.reg);
		break;

	case op_add: case op_adc: case op_sub: case op_sbb:
	case op_and_: case op_or_: case op_xor_: case op_cmp:
	{
		const int index = arithmetic_index_x64(in->code);
		if (in->source.kind == operand_kind_immediate)
		{
			const int32_t value = x64_value_of(e, &in->source);
			const int plain = in->source.reference == reference_kind_none;
			int32_t at;
			x64_prefix(&out);
			if (in->width != 1 && plain && is_byte_value(value))
			{
				x64_put(&out, 0x83);
				x64_modrm(&out, &in->dest, index);
				x64_put(&out, value & 0xff);
				break;
			}
			x64_put(&out, in->width == 1 ? 0x80 : 0x81);
			x64_modrm(&out, &in->dest, index);
			at = out.count;
			x64_immediate(&out, value);
			if (!plain) in->fixup = at;
			break;
		}
		x64_prefix(&out);
		if (in->source.kind == operand_kind_reg)
		{
			x64_put(&out, (index << 3) | (in->width == 1 ? 0 : 1));
			x64_modrm(&out, &in->dest, in->source.reg);
			break;
		}
		x64_put(&out, (index << 3) | (in->width == 1 ? 2 : 3));
		x64_modrm(&out, &in->source, in->dest.reg);
		break;
	}

	case op_test:
		x64_prefix(&out);
		if (in->source.kind == operand_kind_immediate)
		{
			x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
			x64_modrm(&out, &in->dest, 0);
			x64_immediate(&out, x64_value_of(e, &in->source));
			break;
		}
		x64_put(&out, in->width == 1 ? 0x84 : 0x85);
		x64_modrm(&out, &in->dest, in->source.reg);
		break;

	case op_inc:
	case op_dec:
		/* x64 has no one-byte inc: those opcodes became the REX prefixes. */
		x64_prefix(&out);
		x64_put(&out, in->width == 1 ? 0xfe : 0xff);
		x64_modrm(&out, &in->dest, in->code == op_inc ? 0 : 1);
		break;

	case op_neg:
	case op_not_:
		x64_prefix(&out);
		x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
		x64_modrm(&out, &in->dest, in->code == op_neg ? 3 : 2);
		break;

	case op_mul:
	case op_imul:
	case op_div:
	case op_idiv:
	{
		const int field = in->code == op_mul ? 4 : in->code == op_imul ? 5 : in->code == op_div ? 6 : 7;
		x64_prefix(&out);
		x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
		x64_modrm(&out, &in->dest, field);
		break;
	}

	case op_shl: case op_shr: case op_sar: case op_rcl: case op_rcr:
	{
		const int field = shift_index_x64(in->code);
		x64_prefix(&out);
		/* A count of -1 means CL, the only variable count the machine has. */
		x64_put(&out, in->source.value < 0 ? (in->width == 1 ? 0xd2 : 0xd3)
		                                   : (in->width == 1 ? 0xd0 : 0xd1));
		x64_modrm(&out, &in->dest, field);
		break;
	}

	case op_push:
	case op_pop:
		/* A push is 64-bit whatever else happens, so only a 16-bit one needs
		 * saying. The frame pointer wants the whole register. */
		if (!out.pointer_sized) x64_put(&out, 0x66);
		if (in->dest.kind == operand_kind_reg)
		{
			x64_put(&out, (in->code == op_push ? 0x50 : 0x58) + (in->dest.reg & 7));
			break;
		}
		x64_put(&out, in->code == op_push ? 0xff : 0x8f);
		x64_modrm(&out, &in->dest, in->code == op_push ? 6 : 0);
		break;

	case op_xchg:
		x64_prefix(&out);
		x64_put(&out, in->width == 1 ? 0x86 : 0x87);
		x64_modrm(&out, &in->dest, in->source.reg);
		break;

	case op_cbw:
		x64_put(&out, 0x66);
		x64_put(&out, 0x98);
		break;

	case op_cwd:
		x64_put(&out, 0x66);
		x64_put(&out, 0x99);
		break;

	case op_setcc:
		x64_put(&out, 0x0f);
		x64_put(&out, taken_code(in->test) + 0x20);
		x64_put(&out, 0xc0 | (in->dest.reg & 7));
		if (in->width != 1)
		{
			x64_put(&out, 0x66);
			x64_put(&out, 0x0f);
			x64_put(&out, 0xb6);
			x64_put(&out, 0xc0 | ((in->dest.reg & 7) << 3) | (in->dest.reg & 7));
		}
		break;

	case op_call:
		x64_put(&out, 0xe8);
		in->fixup = out.count;
		x64_dword(&out, 0);
		break;

	case op_call_at:
		x64_put(&out, 0xff);
		x64_modrm(&out, &in->dest, 2);
		break;

	case op_ret:
		x64_put(&out, 0xc3);
		break;

	case op_interrupt:
		/* There is no DOS here. Reaching one is a bug, and this says so. */
		x64_put(&out, 0x0f);
		x64_put(&out, 0x0b);
		break;

	case op_jmp:
		if (in->length == 2)
		{
			x64_put(&out, 0xeb);
			in->fixup = out.count;
			x64_put(&out, 0);
			break;
		}
		x64_put(&out, 0xe9);
		in->fixup = out.count;
		x64_dword(&out, 0);
		break;

	case op_jcc:
		if (in->length == 2)
		{
			x64_put(&out, taken_code(in->test));
			in->fixup = out.count;
			x64_put(&out, 0);
			break;
		}
		x64_put(&out, 0x0f);
		x64_put(&out, taken_code(in->test) + 0x10);
		in->fixup = out.count;
		x64_dword(&out, 0);
		break;
	}

	(void)position;
	return out.count;
}
