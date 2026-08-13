/* ovrwin by Zac Walker
 *
 * The 8086 back end: it turns the instruction list into bytes, and it is the
 * only part of the compiler that knows an opcode from a hole in the ground.
 * A second back end is another file of the same shape - the list it consumes
 * says nothing about which encoding an operation has.
 */

#include "emit.h"

#include <stdlib.h>
#include <string.h>

/* Short conditional jump opcodes, and the opcode of the opposite test. */
typedef struct condition_codes
{
	int taken;
	int inverse;
} condition_codes;

static condition_codes codes_for(condition c)
{
	condition_codes codes;
	switch (c)
	{
	case cond_equal: codes.taken = 0x74; codes.inverse = 0x75; break;
	case cond_not_equal: codes.taken = 0x75; codes.inverse = 0x74; break;
	case cond_less: codes.taken = 0x7c; codes.inverse = 0x7d; break;
	case cond_less_equal: codes.taken = 0x7e; codes.inverse = 0x7f; break;
	case cond_greater: codes.taken = 0x7f; codes.inverse = 0x7e; break;
	case cond_greater_equal: codes.taken = 0x7d; codes.inverse = 0x7c; break;
	case cond_below: codes.taken = 0x72; codes.inverse = 0x73; break;
	case cond_below_equal: codes.taken = 0x76; codes.inverse = 0x77; break;
	case cond_above: codes.taken = 0x77; codes.inverse = 0x76; break;
	default: codes.taken = 0x73; codes.inverse = 0x72; break;
	}
	return codes;
}

/* The eight arithmetic operations share one encoding, picked out by three bits
 * that are both the opcode's middle field and the /r extension. */
static int arithmetic_index(op code)
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
	case op_cmp: return 7;
	default: return -1;
	}
}

static int shift_index(op code)
{
	switch (code)
	{
	case op_rcl: return 2;
	case op_rcr: return 3;
	case op_shl: return 4;
	case op_shr: return 5;
	case op_sar: return 7;
	default: return -1;
	}
}

static int is_byte(int32_t v)
{
	return v >= -128 && v <= 127;
}

/* A place to put bytes that can also just count them, so that measuring an
 * instruction and encoding it cannot disagree. */
typedef struct sink
{
	const emitter* e;
	instruction* in;
	cc_list* into;
	int count;
} sink;

static void put(sink* out, int value)
{
	if (out->into != NULL)
	{
		uint8_t* slot = (uint8_t*)cc_list_add(out->into);
		if (slot != NULL) *slot = (uint8_t)value;
	}
	++out->count;
}

static void put_word(sink* out, int32_t value)
{
	put(out, value & 0xff);
	put(out, (value >> 8) & 0xff);
}

/* The 16-bit mod/rm forms. An 8086 addresses memory through four registers and
 * only in these combinations. */
static int memory_form(const operand* o, int* rm, int* mod)
{
	const int base = o->reg;
	const int index = o->index;
	if (base == reg_none && index == reg_none)
	{
		*rm = 6;
		*mod = 0;
		return 1;
	}
	if (index == reg_none)
	{
		switch (base)
		{
		case reg_si: *rm = 4; break;
		case reg_di: *rm = 5; break;
		case reg_bp: *rm = 6; break;
		case reg_b: *rm = 7; break;
		default: return 0;
		}
	}
	else if (base == reg_b && index == reg_si) *rm = 0;
	else if (base == reg_b && index == reg_di) *rm = 1;
	else if (base == reg_bp && index == reg_si) *rm = 2;
	else if (base == reg_bp && index == reg_di) *rm = 3;
	else return 0;

	if (o->value == 0 && *rm != 6) *mod = 0;
	else if (is_byte(o->value)) *mod = 1;
	else *mod = 2;
	return 1;
}

condition opposite(condition c)
{
	switch (c)
	{
	case cond_equal: return cond_not_equal;
	case cond_not_equal: return cond_equal;
	case cond_less: return cond_greater_equal;
	case cond_less_equal: return cond_greater;
	case cond_greater: return cond_less_equal;
	case cond_greater_equal: return cond_less;
	case cond_below: return cond_above_equal;
	case cond_below_equal: return cond_above;
	case cond_above: return cond_below_equal;
	default: return cond_below;
	}
}

/* ------------------------------------------------------------------------- */
/* Building the list                                                         */
/* ------------------------------------------------------------------------- */

operand operand_none(void)
{
	operand o;
	memset(&o, 0, sizeof(o));
	o.kind = operand_kind_none;
	o.reg = reg_none;
	o.index = reg_none;
	o.reference = reference_kind_none;
	return o;
}

operand operand_in(int r)
{
	operand o = operand_none();
	o.kind = operand_kind_reg;
	o.reg = (uint8_t)r;
	return o;
}

operand operand_constant(int32_t v)
{
	operand o = operand_none();
	o.kind = operand_kind_immediate;
	o.value = v;
	return o;
}

operand operand_at(int base, int32_t displacement, int index)
{
	operand o = operand_none();
	o.kind = operand_kind_memory;
	o.reg = (uint8_t)base;
	o.index = (uint8_t)index;
	o.value = displacement;
	return o;
}

operand operand_absolute(int32_t address)
{
	operand o = operand_none();
	o.kind = operand_kind_memory;
	o.value = address;
	return o;
}

void emit_init(emitter* e)
{
	memset(e, 0, sizeof(*e));
	cc_list_init(&e->code, (int)sizeof(instruction));
	cc_list_init(&e->literals, 1);
	cc_list_init(&e->labels, (int)sizeof(int));
	cc_list_init(&e->deferred, (int)sizeof(int32_t));
	cc_list_init(&e->image, 1);
	cc_list_init(&e->dropped, (int)sizeof(code_range));
	e->cpu = cc_cpu_i8086;
	e->machine = cc_machine_i8088;
}

void emit_free(emitter* e)
{
	cc_list_free(&e->code);
	cc_list_free(&e->literals);
	cc_list_free(&e->labels);
	cc_list_free(&e->deferred);
	cc_list_free(&e->image);
	cc_list_free(&e->dropped);
}

instruction* emit_op(emitter* e, op code, int width, operand dest, operand source)
{
	instruction* in = (instruction*)cc_list_add(&e->code);
	in->code = code;
	in->width = (uint8_t)width;
	in->test = cond_equal;
	in->label = -1;
	in->dest = dest;
	in->source = source;
	e->break_run = 0;
	return in;
}

void emit_byte(emitter* e, int value)
{
	instruction* in;
	uint8_t* slot;

	if (!e->break_run && e->code.count > 0)
	{
		instruction* last = &EMIT_CODE(*e, e->code.count - 1);
		if (last->code == op_literal && last->literal_at + last->literal_length == e->literals.count)
		{
			slot = (uint8_t*)cc_list_add(&e->literals);
			if (slot != NULL) *slot = (uint8_t)value;
			++last->literal_length;
			return;
		}
	}

	in = (instruction*)cc_list_add(&e->code);
	in->code = op_literal;
	in->test = cond_equal;
	in->label = -1;
	in->dest = operand_none();
	in->source = operand_none();
	in->width = 2;
	in->literal_at = e->literals.count;
	in->literal_length = 1;
	slot = (uint8_t*)cc_list_add(&e->literals);
	if (slot != NULL) *slot = (uint8_t)value;
	e->break_run = 0;
}

/* The one operand of the instruction just emitted that can carry a reference:
 * an absolute address, or an immediate that stands for one. An absolute address
 * wins, because an instruction that has both is storing a plain number at a
 * named place. */
static int is_absolute(const operand* o)
{
	return o->kind == operand_kind_memory && o->reg == reg_none && o->index == reg_none;
}

static operand* referring_operand(instruction* in)
{
	if (is_absolute(&in->source)) return &in->source;
	if (is_absolute(&in->dest)) return &in->dest;
	if (in->source.kind == operand_kind_immediate) return &in->source;
	if (in->dest.kind == operand_kind_immediate) return &in->dest;
	return NULL;
}

void emit_refers_to_data(emitter* e, int32_t offset)
{
	operand* o = referring_operand(&EMIT_CODE(*e, e->code.count - 1));
	o->reference = reference_kind_data;
	o->target = offset;
}

void emit_refers_to_code(emitter* e, int label)
{
	operand* o = referring_operand(&EMIT_CODE(*e, e->code.count - 1));
	o->reference = reference_kind_code;
	o->target = label;
}

int emit_refers_to_deferred(emitter* e)
{
	const int slot = emit_defer(e, 0);
	operand* o = referring_operand(&EMIT_CODE(*e, e->code.count - 1));
	o->reference = reference_kind_deferred;
	o->target = slot;
	return slot;
}

int emit_label(emitter* e)
{
	*(int*)cc_list_add(&e->labels) = -1;
	return e->labels.count - 1;
}

void emit_bind(emitter* e, int label)
{
	EMIT_LABEL(*e, label) = emit_at(e);
	e->break_run = 1;
}

int emit_here(emitter* e)
{
	*(int*)cc_list_add(&e->labels) = emit_at(e);
	e->break_run = 1;
	return e->labels.count - 1;
}

int emit_is_bound(const emitter* e, int label)
{
	return EMIT_LABEL(*e, label) >= 0;
}

int emit_offset_of(const emitter* e, int label)
{
	const int index = EMIT_LABEL(*e, label);
	if (index < 0) return -1;
	if (index >= e->code.count) return e->image.count;
	return EMIT_CODE(*e, index).offset;
}

int emit_defer(emitter* e, int32_t value)
{
	*(int32_t*)cc_list_add(&e->deferred) = value;
	return e->deferred.count - 1;
}

void emit_set_deferred(emitter* e, int slot, int32_t value)
{
	((int32_t*)e->deferred.items)[slot] = value;
}

emitter_position emit_tell(const emitter* e)
{
	emitter_position p;
	p.code = e->code.count;
	p.literals = e->literals.count;
	p.labels = e->labels.count;
	p.deferred = e->deferred.count;
	return p;
}

void emit_rewind(emitter* e, emitter_position p)
{
	cc_list_set_count(&e->code, p.code);
	cc_list_set_count(&e->literals, p.literals);
	cc_list_set_count(&e->labels, p.labels);
	cc_list_set_count(&e->deferred, p.deferred);
}

void emit_drop(emitter* e, int first, int last)
{
	code_range* range = (code_range*)cc_list_add(&e->dropped);
	range->first = first;
	range->last = last;
}

/* ------------------------------------------------------------------------- */
/* Encoding                                                                  */
/* ------------------------------------------------------------------------- */

/* A reference resolves to a number here; the ones that depend on the data
 * section are written again by emit_resolve(). */
static int32_t value_of(const emitter* e, const operand* o)
{
	if (o->reference == reference_kind_deferred) return ((const int32_t*)e->deferred.items)[o->target];
	if (o->reference == reference_kind_code && o->target >= 0)
	{
		const int index = EMIT_LABEL(*e, o->target);
		if (index >= 0 && index < e->code.count) return EMIT_CODE(*e, index).offset;
	}
	return o->value;
}

/* mod/rm, with the register or extension in the middle field. */
static void modrm(sink* out, const operand* o, int field)
{
	int rm = 0;
	int mod = 0;

	if (o->kind == operand_kind_reg)
	{
		put(out, 0xc0 | (field << 3) | o->reg);
		return;
	}
	memory_form(o, &rm, &mod);
	put(out, (mod << 6) | (field << 3) | rm);
	if (mod == 0 && rm == 6)
	{
		out->in->fixup = out->count;
		put_word(out, value_of(out->e, o));
	}
	else if (mod == 1) put(out, o->value & 0xff);
	else if (mod == 2) put_word(out, o->value);
}

static int encode_8086(const emitter* e, instruction* in, int position, cc_list* into)
{
	sink out;
	const int w = in->width == 1 ? 0 : 1;

	out.e = e;
	out.in = in;
	out.into = into;
	out.count = 0;

	switch (in->code)
	{
	case op_nop:
		break;

	case op_literal:
	{
		int i;
		for (i = 0; i < in->literal_length; ++i)
			put(&out, ((const uint8_t*)e->literals.items)[in->literal_at + i]);
		break;
	}

	case op_mov:
		if (in->dest.kind == operand_kind_reg && in->source.kind == operand_kind_immediate)
		{
			put(&out, (w != 0 ? 0xb8 : 0xb0) + in->dest.reg);
			if (w != 0) put_word(&out, value_of(e, &in->source));
			else put(&out, value_of(e, &in->source) & 0xff);
			break;
		}
		/* The accumulator reaches an absolute address in one byte less. */
		if (in->dest.kind == operand_kind_reg && in->dest.reg == reg_a &&
		    in->source.kind == operand_kind_memory && in->source.reg == reg_none &&
		    in->source.index == reg_none)
		{
			put(&out, 0xa0 + w);
			in->fixup = out.count;
			put_word(&out, value_of(e, &in->source));
			break;
		}
		if (in->source.kind == operand_kind_reg && in->source.reg == reg_a &&
		    in->dest.kind == operand_kind_memory && in->dest.reg == reg_none &&
		    in->dest.index == reg_none)
		{
			put(&out, 0xa2 + w);
			in->fixup = out.count;
			put_word(&out, value_of(e, &in->dest));
			break;
		}
		if (in->source.kind == operand_kind_immediate)
		{
			put(&out, 0xc6 + w);
			modrm(&out, &in->dest, 0);
			if (w != 0) put_word(&out, value_of(e, &in->source));
			else put(&out, value_of(e, &in->source) & 0xff);
			break;
		}
		if (in->source.kind == operand_kind_reg)
		{
			put(&out, 0x88 + w);
			modrm(&out, &in->dest, in->source.reg);
			break;
		}
		put(&out, 0x8a + w);
		modrm(&out, &in->source, in->dest.reg);
		break;

	case op_lea:
		put(&out, 0x8d);
		modrm(&out, &in->source, in->dest.reg);
		break;

	case op_movsx:
	case op_movzx:
		put(&out, 0x0f);
		put(&out, in->code == op_movzx ? 0xb6 : 0xbe); /* the source is always a byte */
		modrm(&out, &in->source, in->dest.reg);
		break;

	case op_add: case op_adc: case op_sub: case op_sbb:
	case op_and_: case op_or_: case op_xor_: case op_cmp:
	{
		const int index = arithmetic_index(in->code);
		if (in->source.kind == operand_kind_immediate)
		{
			const int32_t value = value_of(e, &in->source);
			const int plain = in->source.reference == reference_kind_none;
			/* The accumulator has a short form, and so does an operand that
			 * fits in a sign-extended byte. */
			if (in->dest.kind == operand_kind_reg && in->dest.reg == reg_a &&
			    (w == 0 || !plain || !is_byte(value)))
			{
				put(&out, (index << 3) | 0x04 | w);
				if (w != 0) put_word(&out, value);
				else put(&out, value & 0xff);
				break;
			}
			if (w != 0 && plain && is_byte(value))
			{
				put(&out, 0x83);
				modrm(&out, &in->dest, index);
				put(&out, value & 0xff);
				break;
			}
			put(&out, 0x80 + w);
			modrm(&out, &in->dest, index);
			if (w != 0) put_word(&out, value);
			else put(&out, value & 0xff);
			break;
		}
		if (in->source.kind == operand_kind_reg)
		{
			put(&out, (index << 3) | w);
			modrm(&out, &in->dest, in->source.reg);
			break;
		}
		put(&out, (index << 3) | 0x02 | w);
		modrm(&out, &in->source, in->dest.reg);
		break;
	}

	case op_test:
		if (in->source.kind == operand_kind_immediate)
		{
			if (in->dest.kind == operand_kind_reg && in->dest.reg == reg_a)
			{
				put(&out, 0xa8 + w);
				if (w != 0) put_word(&out, value_of(e, &in->source));
				else put(&out, value_of(e, &in->source) & 0xff);
				break;
			}
			put(&out, 0xf6 + w);
			modrm(&out, &in->dest, 0);
			if (w != 0) put_word(&out, value_of(e, &in->source));
			else put(&out, value_of(e, &in->source) & 0xff);
			break;
		}
		put(&out, 0x84 + w);
		modrm(&out, &in->dest, in->source.reg);
		break;

	case op_inc:
	case op_dec:
	{
		const int field = in->code == op_inc ? 0 : 1;
		if (w != 0 && in->dest.kind == operand_kind_reg)
		{
			put(&out, (in->code == op_inc ? 0x40 : 0x48) + in->dest.reg);
			break;
		}
		put(&out, 0xfe + w);
		modrm(&out, &in->dest, field);
		break;
	}

	case op_neg:
	case op_not_:
	case op_mul:
	case op_imul:
	case op_div:
	case op_idiv:
	{
		int field = 0;
		switch (in->code)
		{
		case op_not_: field = 2; break;
		case op_neg: field = 3; break;
		case op_mul: field = 4; break;
		case op_imul: field = 5; break;
		case op_div: field = 6; break;
		default: field = 7; break;
		}
		put(&out, 0xf6 + w);
		modrm(&out, &in->dest, field);
		break;
	}

	case op_shl: case op_shr: case op_sar: case op_rcl: case op_rcr:
	{
		const int field = shift_index(in->code);
		const int32_t by = in->source.value;
		if (by < 0) put(&out, 0xd2 + w); /* by CL */
		else put(&out, 0xd0 + w); /* by one */
		modrm(&out, &in->dest, field);
		break;
	}

	case op_push:
	case op_pop:
		if (in->dest.kind == operand_kind_reg)
		{
			put(&out, (in->code == op_push ? 0x50 : 0x58) + in->dest.reg);
			break;
		}
		if (in->code == op_push)
		{
			put(&out, 0xff);
			modrm(&out, &in->dest, 6);
		}
		else
		{
			put(&out, 0x8f);
			modrm(&out, &in->dest, 0);
		}
		break;

	case op_xchg:
		if (in->dest.kind == operand_kind_reg && in->source.kind == operand_kind_reg &&
		    (in->dest.reg == reg_a || in->source.reg == reg_a) && w != 0)
		{
			put(&out, 0x90 + (in->dest.reg == reg_a ? in->source.reg : in->dest.reg));
			break;
		}
		put(&out, 0x86 + w);
		modrm(&out, &in->dest, in->source.reg);
		break;

	case op_cbw: put(&out, 0x98); break;
	case op_cwd: put(&out, 0x99); break;
	case op_ret: put(&out, 0xc3); break;

	case op_interrupt:
		put(&out, 0xcd);
		put(&out, in->dest.value & 0xff);
		break;

	case op_setcc:
		/* A 386 sets the byte and clears the rest; an 8086 has to branch over a
		 * clear, because it has no SETcc at all. */
		if (e->cpu >= cc_cpu_i80386 && in->dest.reg < 4)
		{
			put(&out, 0x0f);
			put(&out, codes_for(in->test).taken + 0x20);
			put(&out, 0xc0 | in->dest.reg);
			if (w != 0)
			{
				put(&out, 0x30); /* xor high,high */
				put(&out, 0xc0 | ((in->dest.reg + 4) << 3) | (in->dest.reg + 4));
			}
			break;
		}
		put(&out, (w != 0 ? 0xb8 : 0xb0) + in->dest.reg);
		if (w != 0) put_word(&out, 1);
		else put(&out, 1);
		put(&out, codes_for(in->test).taken);
		put(&out, 0x02);
		put(&out, 0x30 | w);
		put(&out, 0xc0 | (in->dest.reg << 3) | in->dest.reg);
		break;

	case op_call:
		put(&out, 0xe8);
		in->fixup = out.count;
		put_word(&out, 0);
		break;

	case op_call_at:
		put(&out, 0xff);
		modrm(&out, &in->dest, 2);
		break;

	case op_jmp:
		if (in->length == 2)
		{
			put(&out, 0xeb);
			in->fixup = out.count;
			put(&out, 0);
			break;
		}
		put(&out, 0xe9);
		in->fixup = out.count;
		put_word(&out, 0);
		break;

	case op_jcc:
		if (in->length == 2)
		{
			put(&out, codes_for(in->test).taken);
			in->fixup = out.count;
			put(&out, 0);
			break;
		}
		/* A 386 branches that far in one instruction. An 8086 has no near
		 * conditional branch, so it jumps over an unconditional one. */
		if (e->cpu >= cc_cpu_i80386)
		{
			put(&out, 0x0f);
			put(&out, codes_for(in->test).taken + 0x10);
			in->fixup = out.count;
			put_word(&out, 0);
			break;
		}
		put(&out, codes_for(in->test).inverse);
		put(&out, 0x03);
		put(&out, 0xe9);
		in->fixup = out.count;
		put_word(&out, 0);
		break;
	}

	/* An immediate that stands for an address is the last word written; a
	 * displacement recorded its own position as it went. */
	if (in->dest.kind == operand_kind_immediate && in->dest.reference != reference_kind_none)
		in->fixup = out.count - 2;
	else if (in->source.kind == operand_kind_immediate && in->source.reference != reference_kind_none)
		in->fixup = out.count - 2;

	(void)position;
	return out.count;
}

/* Writes one instruction, or counts its bytes when `into` is null. `position`
 * is where it starts, which only a relative branch needs. */
int emit_encode(const emitter* e, instruction* in, int position, cc_list* into)
{
	in->fixup = 0;
	return e->machine == cc_machine_x64 ? emit_encode_x64(e, in, position, into)
	                                    : encode_8086(e, in, position, into);
}

/* How long a branch is before the layout pass has tried to shorten it. */
static int branch_length(const emitter* e, op code)
{
	if (e->machine == cc_machine_x64) return code == op_jmp ? 5 : 6;
	if (code == op_jmp) return 3;
	return e->cpu >= cc_cpu_i80386 ? 4 : 5;
}

/* ------------------------------------------------------------------------- */
/* Laying it out                                                             */
/* ------------------------------------------------------------------------- */

static int place_all(emitter* e, const char* alive)
{
	int offset = 0;
	int i;
	for (i = 0; i < e->code.count; ++i)
	{
		instruction* in = &EMIT_CODE(*e, i);
		in->offset = offset;
		if (!alive[i]) continue;
		if (in->code == op_jmp || in->code == op_jcc) offset += in->length;
		else offset += emit_encode(e, in, offset, NULL);
	}
	return offset;
}

void emit_lay_out(emitter* e, int code_base)
{
	char* alive;
	int i;

	(void)code_base;

	/* What is being dropped, as instruction ranges. */
	alive = (char*)malloc((size_t)(e->code.count > 0 ? e->code.count : 1));
	if (alive == NULL) return;
	memset(alive, 1, (size_t)(e->code.count > 0 ? e->code.count : 1));

	for (i = 0; i < e->dropped.count; ++i)
	{
		const code_range* range = (const code_range*)cc_list_at(&e->dropped, i);
		const int from = EMIT_LABEL(*e, range->first);
		const int to = EMIT_LABEL(*e, range->last);
		int at;
		if (from < 0 || to <= from) continue;
		for (at = from; at < to && at < e->code.count; ++at) alive[at] = 0;
	}

	/* Every branch starts at its longest, so the pass can only shorten. */
	for (i = 0; i < e->code.count; ++i)
	{
		instruction* in = &EMIT_CODE(*e, i);
		if (in->code == op_jmp || in->code == op_jcc) in->length = branch_length(e, in->code);
		else in->length = 0;
	}

	/* Shortening a branch only brings its target closer, so this settles. */
	place_all(e, alive);
	for (;;)
	{
		int changed = 0;
		for (i = 0; i < e->code.count; ++i)
		{
			instruction* in = &EMIT_CODE(*e, i);
			int index;
			int target;
			int displacement;

			if (!alive[i]) continue;
			if (in->code != op_jmp && in->code != op_jcc) continue;
			if (in->length == 0) continue;
			if (in->label < 0 || EMIT_LABEL(*e, in->label) < 0) continue;

			index = EMIT_LABEL(*e, in->label);
			target = index < e->code.count ? EMIT_CODE(*e, index).offset : place_all(e, alive);
			in = &EMIT_CODE(*e, i); /* place_all may have moved nothing, but be explicit */
			if (target == in->offset)
			{
				/* A branch to the instruction after it does nothing at all,
				 * which is what the last return of an expanded body becomes. */
				in->length = 0;
				changed = 1;
				continue;
			}
			if (in->length == 2) continue;
			displacement = target - (in->offset + 2);
			if (!is_byte(displacement)) continue;
			in->length = 2;
			changed = 1;
		}
		place_all(e, alive);
		if (!changed) break;
	}

	cc_list_set_count(&e->image, 0);
	for (i = 0; i < e->code.count; ++i)
	{
		instruction* in = &EMIT_CODE(*e, i);
		int length;
		if (!alive[i]) continue;
		in->offset = e->image.count;
		length = emit_encode(e, in, in->offset, &e->image);
		in->length = length;
	}

	/* A label bound to a dropped instruction lands on whatever took its place,
	 * which is harmless because nothing that survived refers to it. Its length
	 * goes to zero so that emit_resolve() knows there is nothing there to write
	 * to. */
	for (i = e->code.count; i > 0; --i)
	{
		if (alive[i - 1]) continue;
		EMIT_CODE(*e, i - 1).offset = i < e->code.count ? EMIT_CODE(*e, i).offset : e->image.count;
		EMIT_CODE(*e, i - 1).length = 0;
	}

	free(alive);
	cc_list_set_count(&e->dropped, 0);
}

static void image_word(emitter* e, int at, int value)
{
	EMIT_IMAGE(*e, at) = (uint8_t)(value & 0xff);
	EMIT_IMAGE(*e, at + 1) = (uint8_t)((value >> 8) & 0xff);
}

static void image_long(emitter* e, int at, int value)
{
	image_word(e, at, value);
	EMIT_IMAGE(*e, at + 2) = (uint8_t)((value >> 16) & 0xff);
	EMIT_IMAGE(*e, at + 3) = (uint8_t)((value >> 24) & 0xff);
}

int emit_resolve(emitter* e, int code_base, int data_base)
{
	const int relative = e->machine == cc_machine_x64;
	int i;

	for (i = 0; i < e->code.count; ++i)
	{
		instruction* in = &EMIT_CODE(*e, i);
		const operand* operands[2];
		int which;

		/* Nothing was written for it: either it was dropped, or it is a branch
		 * that turned out to go to the instruction after it. */
		if (in->length == 0) continue;

		if (in->code == op_jmp || in->code == op_jcc || in->code == op_call)
		{
			int index;
			int target;
			int displacement;

			if (in->label < 0 || EMIT_LABEL(*e, in->label) < 0) return 0;
			index = EMIT_LABEL(*e, in->label);
			target = index < e->code.count ? EMIT_CODE(*e, index).offset : e->image.count;
			displacement = target - (in->offset + in->length);
			if (in->length == 2) EMIT_IMAGE(*e, in->offset + in->fixup) = (uint8_t)displacement;
			else if (relative) image_long(e, in->offset + in->fixup, displacement);
			else image_word(e, in->offset + in->fixup, displacement);
			continue;
		}

		/* An operand that names an address only became a number once the data
		 * section was placed, so it is written again here. */
		operands[0] = &in->dest;
		operands[1] = &in->source;
		for (which = 0; which < 2; ++which)
		{
			const operand* o = operands[which];
			int value = 0;

			if (o->reference == reference_kind_none) continue;
			if (o->reference == reference_kind_deferred)
				value = ((const int32_t*)e->deferred.items)[o->target];
			else if (o->reference == reference_kind_data) value = data_base + o->target;
			else
			{
				int index;
				if (o->target < 0 || EMIT_LABEL(*e, o->target) < 0) return 0;
				index = EMIT_LABEL(*e, o->target);
				value = code_base + (index < e->code.count ? EMIT_CODE(*e, index).offset : e->image.count);
			}
			/* Where an address is reached from the instruction after it, what
			 * goes in the field is the distance rather than the address. */
			if (relative && o->kind == operand_kind_memory)
				value -= code_base + in->offset + in->length;
			if (o->reference == reference_kind_deferred && !relative)
				image_word(e, in->offset + in->fixup, value);
			else if (relative) image_long(e, in->offset + in->fixup, value);
			else image_word(e, in->offset + in->fixup, value);
		}
	}
	return 1;
}
