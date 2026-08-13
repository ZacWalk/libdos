/* ovrwin by Zac Walker
 *
 * The x64 back end: the same instruction list as the 8086 one, encoded for the
 * machine this compiler is running on. It is deliberately a second file of the
 * same shape rather than a parameterisation of the first, because the two
 * differ in the things an encoder is made of - the mod/rm table, the prefixes,
 * how far a branch reaches - and agree on nothing but the opcode bytes.
 *
 * The list says 16 bits, so this emits 16-bit operations: on x64 that is a 66h
 * prefix in front of the same opcodes, which keeps the arithmetic and the flags
 * identical to what the interpreter would have produced.
 *
 * What makes a pointer work is the block. The front end compiles for the tiny
 * model - one 64 KiB segment holding code, data and stack - so the host hands
 * the generated code a 64 KiB block and R15 points at it. A guest pointer is
 * then a 16-bit offset into that block exactly as it is under DOS, and every
 * memory reference becomes [r15 + offset]. The two registers that hold offsets
 * rather than values, SP and BP, become R14 and R13 and are worked on 32 bits
 * at a time so that writing one clears the top of the register: an index has to
 * be a whole 64-bit quantity when the machine adds it to R15.
 *
 * The host's own stack still carries return addresses, so a call is a call.
 * Only the data stack - what push, pop and a frame pointer touch - lives in
 * the block.
 */

#include "cc.h"
#include "emit.h"

/* The register R15 holds the block, and the two guest registers that hold
 * offsets into it. */
#define X64_BLOCK 15
#define X64_SP 14
#define X64_BP 13

typedef struct x64_sink
{
	const emitter* e;
	instruction* in;
	cc_list* into;
	int count;
	int offset_sized; /* the operation is on SP or BP, so it is 32 bits wide */
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

/* A byte operation names AH with the same number SP has, and the front end
 * never puts an address in a byte register, so the width settles which one is
 * meant. */
static int is_offset_register(const operand* o, int width)
{
	return width != 1 && o->kind == operand_kind_reg && (o->reg == reg_sp || o->reg == reg_bp);
}

/* Where a guest register lives on the host, and whether that needs a REX bit. */
static int host_register(int reg, int width)
{
	if (width == 1) return reg;
	if (reg == reg_sp) return X64_SP;
	if (reg == reg_bp) return X64_BP;
	return reg;
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

/* 66h narrows an operation to 16 bits. An operation on SP or BP is left at 32,
 * which is also what zeroes the top of the register. */
static void x64_size(x64_sink* out, int width)
{
	if (!out->offset_sized && width != 1) x64_put(out, 0x66);
}

static void x64_rex(x64_sink* out, int w, int reg, int index, int base)
{
	const int rex = 0x40 | (w != 0 ? 8 : 0) | (reg >= 8 ? 4 : 0) | (index >= 8 ? 2 : 0) |
		(base >= 8 ? 1 : 0);
	if (rex != 0x40) x64_put(out, rex);
}

/* A guest address is R15 plus a 16-bit offset, which is a base and an index.
 * `lea` is the exception: it wants the offset itself, so it leaves R15 out. */
static void x64_modrm(x64_sink* out, const operand* o, int field, int width, int flat)
{
	const int reg_field = field & 7;

	if (o->kind == operand_kind_reg)
	{
		x64_put(out, 0xc0 | (reg_field << 3) | (host_register(o->reg, width) & 7));
		return;
	}

	if (flat)
	{
		/* [base + displacement], with no block: the value is an offset. */
		const int base = host_register(o->reg, 2) & 7;
		if (is_byte_value(o->value))
		{
			x64_put(out, 0x40 | (reg_field << 3) | base);
			if (base == 4) x64_put(out, 0x24); /* an index of none */
			x64_put(out, o->value & 0xff);
			return;
		}
		x64_put(out, 0x80 | (reg_field << 3) | base);
		if (base == 4) x64_put(out, 0x24);
		x64_dword(out, o->value);
		return;
	}

	if (o->reg == reg_none)
	{
		/* An absolute guest address: [r15 + disp32]. */
		x64_put(out, 0x80 | (reg_field << 3) | 4);
		x64_put(out, (4 << 3) | (X64_BLOCK & 7)); /* no index, base r15 */
		out->in->fixup = out->count;
		out->in->fixup_width = 4;
		x64_dword(out, x64_value_of(out->e, o));
		return;
	}

	/* [r15 + guest base + displacement]. */
	{
		const int index = host_register(o->reg, 2) & 7;
		if (o->value == 0)
		{
			x64_put(out, 0x00 | (reg_field << 3) | 4);
			x64_put(out, (index << 3) | (X64_BLOCK & 7));
			return;
		}
		if (is_byte_value(o->value))
		{
			x64_put(out, 0x40 | (reg_field << 3) | 4);
			x64_put(out, (index << 3) | (X64_BLOCK & 7));
			x64_put(out, o->value & 0xff);
			return;
		}
		x64_put(out, 0x80 | (reg_field << 3) | 4);
		x64_put(out, (index << 3) | (X64_BLOCK & 7));
		x64_dword(out, o->value);
	}
}

/* The REX an instruction needs, given which operand carries the register field
 * and which the mod/rm. */
static void x64_prefix(x64_sink* out, int width, const operand* rm, int reg, int flat)
{
	int index = 0;
	int base = 0;

	x64_size(out, width);

	if (rm->kind == operand_kind_reg) base = host_register(rm->reg, width);
	else if (flat) base = host_register(rm->reg, 2);
	else
	{
		base = X64_BLOCK;
		if (rm->reg != reg_none) index = host_register(rm->reg, 2);
	}
	x64_rex(out, 0, reg, index, base);
}

/* An immediate is as wide as the operation. */
static void x64_immediate(x64_sink* out, int width, int32_t value)
{
	if (out->offset_sized) x64_dword(out, value);
	else if (width == 1) x64_put(out, value & 0xff);
	else x64_word(out, value);
}

static int immediate_width(const x64_sink* out, int width)
{
	if (out->offset_sized) return 4;
	return width == 1 ? 1 : 2;
}

/* push and pop work on the block, not on the host's stack: sp is a guest
 * offset, so each becomes an explicit move. */
static void x64_adjust_sp(x64_sink* out, int by)
{
	x64_put(out, 0x41); /* REX.B */
	x64_put(out, 0x83);
	x64_put(out, by < 0 ? 0xee : 0xc6); /* sub/add r14d, imm8 */
	x64_put(out, (by < 0 ? -by : by) & 0xff);
}

/* [r15 + r14], the top of the guest stack. */
static void x64_stack_slot(x64_sink* out, int reg_field)
{
	x64_put(out, 0x00 | ((reg_field & 7) << 3) | 4);
	x64_put(out, ((X64_SP & 7) << 3) | (X64_BLOCK & 7));
}

int emit_encode_x64(const emitter* e, instruction* in, int position, cc_list* into)
{
	x64_sink out;

	out.e = e;
	out.in = in;
	out.into = into;
	out.count = 0;
	out.offset_sized = is_offset_register(&in->dest, in->width) ||
		is_offset_register(&in->source, in->width);

	switch (in->code)
	{
	case op_nop:
		break;

	case op_literal:
	{
		/* Bytes the generator assembled itself. The 8086 helpers mean nothing
		 * here, but a hosted program's own startup does. */
		int i;
		for (i = 0; i < in->literal_length; ++i)
			x64_put(&out, ((const uint8_t*)e->literals.items)[in->literal_at + i]);
		break;
	}

	case op_mov:
		if (in->dest.kind == operand_kind_reg && in->source.kind == operand_kind_immediate)
		{
			const int32_t value = x64_value_of(e, &in->source);
			if (out.offset_sized)
			{
				/* mov r14d/r13d, imm32 */
				x64_put(&out, 0x41);
				x64_put(&out, 0xc7);
				x64_put(&out, 0xc0 | (host_register(in->dest.reg, 2) & 7));
				in->fixup = out.count;
				in->fixup_width = 4;
				x64_dword(&out, value);
				break;
			}
			x64_size(&out, in->width);
			x64_rex(&out, 0, 0, 0, host_register(in->dest.reg, in->width));
			x64_put(&out, (in->width != 1 ? 0xb8 : 0xb0) + (host_register(in->dest.reg, in->width) & 7));
			in->fixup = out.count;
			in->fixup_width = immediate_width(&out, in->width);
			x64_immediate(&out, in->width, value);
			break;
		}
		if (in->source.kind == operand_kind_immediate)
		{
			int32_t at;
			x64_prefix(&out, in->width, &in->dest, 0, 0);
			x64_put(&out, in->width == 1 ? 0xc6 : 0xc7);
			x64_modrm(&out, &in->dest, 0, in->width, 0);
			at = out.count;
			x64_immediate(&out, in->width, x64_value_of(e, &in->source));
			if (in->source.reference != reference_kind_none)
			{
				in->fixup = at;
				in->fixup_width = immediate_width(&out, in->width);
			}
			break;
		}
		if (in->source.kind == operand_kind_reg)
		{
			x64_prefix(&out, in->width, &in->dest, host_register(in->source.reg, in->width), 0);
			x64_put(&out, in->width == 1 ? 0x88 : 0x89);
			x64_modrm(&out, &in->dest, host_register(in->source.reg, in->width), in->width, 0);
			break;
		}
		x64_prefix(&out, in->width, &in->source, host_register(in->dest.reg, in->width), 0);
		x64_put(&out, in->width == 1 ? 0x8a : 0x8b);
		x64_modrm(&out, &in->source, host_register(in->dest.reg, in->width), in->width, 0);
		break;

	case op_lea:
		/* The address of something in the block is a 16-bit offset, so this is
		 * the one place the block register is left out. */
		out.offset_sized = 1; /* a 32-bit lea, whose result is used 16 bits wide */
		x64_prefix(&out, 2, &in->source, host_register(in->dest.reg, 2), 1);
		x64_put(&out, 0x8d);
		x64_modrm(&out, &in->source, host_register(in->dest.reg, 2), 2, 1);
		break;

	case op_movsx:
	case op_movzx:
		x64_size(&out, in->width);
		x64_prefix(&out, in->width, &in->source, host_register(in->dest.reg, in->width), 0);
		x64_put(&out, 0x0f);
		x64_put(&out, in->code == op_movzx ? 0xb6 : 0xbe);
		x64_modrm(&out, &in->source, host_register(in->dest.reg, in->width), in->width, 0);
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
			x64_prefix(&out, in->width, &in->dest, 0, 0);
			if (in->width != 1 && plain && is_byte_value(value))
			{
				x64_put(&out, 0x83);
				x64_modrm(&out, &in->dest, index, in->width, 0);
				x64_put(&out, value & 0xff);
				break;
			}
			x64_put(&out, in->width == 1 ? 0x80 : 0x81);
			x64_modrm(&out, &in->dest, index, in->width, 0);
			at = out.count;
			x64_immediate(&out, in->width, value);
			if (!plain)
			{
				in->fixup = at;
				in->fixup_width = immediate_width(&out, in->width);
			}
			break;
		}
		if (in->source.kind == operand_kind_reg)
		{
			x64_prefix(&out, in->width, &in->dest, host_register(in->source.reg, in->width), 0);
			x64_put(&out, (index << 3) | (in->width == 1 ? 0 : 1));
			x64_modrm(&out, &in->dest, host_register(in->source.reg, in->width), in->width, 0);
			break;
		}
		x64_prefix(&out, in->width, &in->source, host_register(in->dest.reg, in->width), 0);
		x64_put(&out, (index << 3) | (in->width == 1 ? 2 : 3));
		x64_modrm(&out, &in->source, host_register(in->dest.reg, in->width), in->width, 0);
		break;
	}

	case op_test:
		if (in->source.kind == operand_kind_immediate)
		{
			x64_prefix(&out, in->width, &in->dest, 0, 0);
			x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
			x64_modrm(&out, &in->dest, 0, in->width, 0);
			x64_immediate(&out, in->width, x64_value_of(e, &in->source));
			break;
		}
		x64_prefix(&out, in->width, &in->dest, host_register(in->source.reg, in->width), 0);
		x64_put(&out, in->width == 1 ? 0x84 : 0x85);
		x64_modrm(&out, &in->dest, host_register(in->source.reg, in->width), in->width, 0);
		break;

	case op_inc:
	case op_dec:
		/* x64 has no one-byte inc: those opcodes became the REX prefixes. */
		x64_prefix(&out, in->width, &in->dest, 0, 0);
		x64_put(&out, in->width == 1 ? 0xfe : 0xff);
		x64_modrm(&out, &in->dest, in->code == op_inc ? 0 : 1, in->width, 0);
		break;

	case op_neg:
	case op_not_:
		x64_prefix(&out, in->width, &in->dest, 0, 0);
		x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
		x64_modrm(&out, &in->dest, in->code == op_neg ? 3 : 2, in->width, 0);
		break;

	case op_mul:
	case op_imul:
	case op_div:
	case op_idiv:
	{
		const int field = in->code == op_mul ? 4 : in->code == op_imul ? 5 : in->code == op_div ? 6 : 7;
		x64_prefix(&out, in->width, &in->dest, 0, 0);
		x64_put(&out, in->width == 1 ? 0xf6 : 0xf7);
		x64_modrm(&out, &in->dest, field, in->width, 0);
		break;
	}

	case op_shl: case op_shr: case op_sar: case op_rcl: case op_rcr:
	{
		const int field = shift_index_x64(in->code);
		x64_prefix(&out, in->width, &in->dest, 0, 0);
		/* A count of -1 means CL, the only variable count the machine has. */
		x64_put(&out, in->source.value < 0 ? (in->width == 1 ? 0xd2 : 0xd3)
		                                   : (in->width == 1 ? 0xd0 : 0xd1));
		x64_modrm(&out, &in->dest, field, in->width, 0);
		break;
	}

	case op_push:
	case op_pop:
	{
		/* The guest's stack is in the block, so this is an explicit move. */
		const int is_offset = is_offset_register(&in->dest, 2);
		const int reg = host_register(in->dest.reg, 2);

		if (in->code == op_push)
		{
			/* A slot on the guest stack is two bytes whatever the register is,
			 * so even the frame pointer is stored 16 bits at a time. */
			x64_adjust_sp(&out, -2);
			x64_put(&out, 0x66);
			x64_rex(&out, 0, reg, X64_SP, X64_BLOCK);
			x64_put(&out, 0x89);
			x64_stack_slot(&out, reg);
			break;
		}

		if (is_offset)
		{
			/* movzx r13d, word [r15+r14], so the whole register is the offset. */
			x64_rex(&out, 0, reg, X64_SP, X64_BLOCK);
			x64_put(&out, 0x0f);
			x64_put(&out, 0xb7);
			x64_stack_slot(&out, reg);
		}
		else
		{
			x64_put(&out, 0x66);
			x64_rex(&out, 0, reg, X64_SP, X64_BLOCK);
			x64_put(&out, 0x8b);
			x64_stack_slot(&out, reg);
		}
		x64_adjust_sp(&out, 2);
		break;
	}

	case op_xchg:
		x64_prefix(&out, in->width, &in->dest, host_register(in->source.reg, in->width), 0);
		x64_put(&out, in->width == 1 ? 0x86 : 0x87);
		x64_modrm(&out, &in->dest, host_register(in->source.reg, in->width), in->width, 0);
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
		in->fixup_width = 4;
		x64_dword(&out, 0);
		break;

	case op_call_at:
		/* The slot holds a guest offset, so it has to be added to the block
		 * before anything can be called through it. AX is scratch here: the
		 * arguments are already on the stack and the result comes back in it. */
		x64_put(&out, 0x66); /* mov ax, [ ... ] */
		x64_rex(&out, 0, 0, host_register(in->dest.reg, 2), X64_BLOCK);
		x64_put(&out, 0x8b);
		x64_modrm(&out, &in->dest, 0, 2, 0);
		x64_put(&out, 0x0f); /* movzx eax, ax */
		x64_put(&out, 0xb7);
		x64_put(&out, 0xc0);
		x64_put(&out, 0x49); /* lea rax, [r15 + rax] */
		x64_put(&out, 0x8d);
		x64_put(&out, 0x04);
		x64_put(&out, 0x07);
		x64_put(&out, 0xff); /* call rax */
		x64_put(&out, 0xd0);
		break;

	case op_ret:
		x64_put(&out, 0xc3);
		break;

	case op_interrupt:
	{
		/* The host's own service call. The guest's registers go where the host
		 * agreed to look for them, and RBP - which the guest never uses, since
		 * its frame pointer is R13 - carries the stack pointer across the
		 * alignment the host's calling convention insists on. */
		static const int saved[5] = {0x87, 0x9f, 0x8f, 0x97, 0xb7}; /* ax bx cx dx si */
		int i;

		for (i = 0; i < 5; ++i)
		{
			x64_put(&out, 0x66);
			x64_put(&out, 0x41);
			x64_put(&out, 0x89);
			x64_put(&out, saved[i]);
			x64_dword(&out, CC_NATIVE_REGISTERS + i * 2);
		}

		x64_put(&out, 0x55); /* push rbp */
		x64_put(&out, 0x48);
		x64_put(&out, 0x89);
		x64_put(&out, 0xe5); /* mov rbp, rsp */
		x64_put(&out, 0x48);
		x64_put(&out, 0x83);
		x64_put(&out, 0xe4);
		x64_put(&out, 0xf0); /* and rsp, -16 */
		x64_put(&out, 0x48);
		x64_put(&out, 0x83);
		x64_put(&out, 0xec);
		x64_put(&out, 0x20); /* sub rsp, 32 - the shadow space */

		x64_put(&out, 0x4c);
		x64_put(&out, 0x89);
		x64_put(&out, 0xf9); /* mov rcx, r15 */
		x64_put(&out, 0xba); /* mov edx, <interrupt> */
		x64_dword(&out, in->dest.value & 0xff);
		x64_put(&out, 0x41);
		x64_put(&out, 0xff);
		x64_put(&out, 0x97); /* call [r15 + service] */
		x64_dword(&out, CC_NATIVE_SERVICE);

		x64_put(&out, 0x48);
		x64_put(&out, 0x89);
		x64_put(&out, 0xec); /* mov rsp, rbp */
		x64_put(&out, 0x5d); /* pop rbp */

		/* RBX, RSI, RDI and R13 to R15 came back on their own. */
		for (i = 0; i < 4; ++i)
		{
			static const int reloaded[4] = {0x87, 0x8f, 0x97, 0xb7}; /* ax cx dx si */
			static const int at[4] = {0, 2, 3, 4};
			x64_put(&out, 0x66);
			x64_put(&out, 0x41);
			x64_put(&out, 0x8b);
			x64_put(&out, reloaded[i]);
			x64_dword(&out, CC_NATIVE_REGISTERS + at[i] * 2);
		}
		break;
	}

	case op_jmp:
		if (in->length == 2)
		{
			x64_put(&out, 0xeb);
			in->fixup = out.count;
			in->fixup_width = 1;
			x64_put(&out, 0);
			break;
		}
		x64_put(&out, 0xe9);
		in->fixup = out.count;
		in->fixup_width = 4;
		x64_dword(&out, 0);
		break;

	case op_jcc:
		if (in->length == 2)
		{
			x64_put(&out, taken_code(in->test));
			in->fixup = out.count;
			in->fixup_width = 1;
			x64_put(&out, 0);
			break;
		}
		x64_put(&out, 0x0f);
		x64_put(&out, taken_code(in->test) + 0x10);
		in->fixup = out.count;
		in->fixup_width = 4;
		x64_dword(&out, 0);
		break;
	}

	(void)position;
	return out.count;
}
