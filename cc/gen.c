/* ovrwin by Zac Walker
 *
 * Parser and code generator. One pass, no syntax tree: the parser emits 8086
 * code as it goes and carries a small `value` describing where the result of
 * the expression it just parsed lives. That is the shape TCC uses, and it is
 * what keeps a working C compiler down to a readable few thousand lines.
 *
 * Everything is computed in AX. A binary operator pushes its left operand,
 * evaluates the right into AX, then pops the left back - no register
 * allocation, and correct by construction.
 *
 * A name is an integer here, not a string: the lexer interned it, so a symbol
 * lookup is a comparison of two ints. Everything that grows is a cc_list, and
 * an error is a longjmp back to cc_compile - the parser is recursive descent,
 * so there is no other honest way to abandon a parse without checking a flag
 * after every call.
 */

#include "cc.h"
#include "emit.h"
#include "lex.h"
#include "support.h"
#include "target.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum type_kind
{
	ty_void, ty_char, ty_uchar, ty_int, ty_uint, ty_long, ty_ulong, ty_real,
	ty_pointer, ty_array, ty_record, ty_function
} type_kind;

typedef struct type_info
{
	type_kind kind;
	int size;
	int base; /* element or target type, or the result of a function */
	int count; /* array length */
	int record;
	int signature; /* index into the parameter-list table, for a function */
} type_info;

typedef struct record_member
{
	int name;
	int type;
	int offset;
} record_member;

typedef struct record_info
{
	int name;
	cc_list members; /* record_member */
	int size;
	int is_union;
} record_info;

typedef enum symbol_kind { sym_global, sym_local, sym_function, sym_constant, sym_typedef } symbol_kind;

typedef struct symbol
{
	int name;
	symbol_kind kind;
	int type;
	int offset; /* data offset for globals, frame offset for locals */
	int label; /* where the code is, for a function */
	int start; /* the same label, kept for the span the body occupies */
	int finish; /* the label just past the body */
	long value; /* enumeration constant */
	int scope;
	cc_list parameters; /* int, declared parameter types, so a call can convert */
	int defined; /* a body was generated, not just a declaration */
	int called; /* something other than a dropped function reached it */
	int body; /* the token the body starts at, for inlining */
	int frame; /* how much stack the body needs, for inlining */
	cc_list parameter_names; /* int */
} symbol;

typedef enum value_kind { val_const, val_reg, val_local, val_global, val_deref, val_flags } value_kind;

/* Where the address of a dereference is. Leaving it in memory until the value
 * is actually wanted is what keeps it off the stack. */
typedef enum address_source { addr_reg, addr_local, addr_global } address_source;

typedef struct value
{
	value_kind kind;
	int type;
	long constant;
	double real; /* only meaningful while the value is still a constant */
	int offset; /* the condition, for a value that is still only flags */
	address_source address;
	int bias; /* the constant part of a dereferenced address */
} value;

/* A pointer held in the data section, which does not move and so is still
 * written by offset rather than through the emitter's layout record. */
typedef struct data_fixup
{
	int position;
	int offset;
} data_fixup;

/* The address of a function, written into the data section. */
typedef struct function_fixup
{
	int position;
	int function;
} function_fixup;

typedef struct switch_case
{
	long value;
	int label;
} switch_case;

typedef struct switch_context
{
	int temporary;
	cc_list cases; /* switch_case */
	int default_label;
} switch_context;

/* A string literal already in the data section, kept so that the same text is
 * only stored once. It may contain a NUL, so it carries its length. */
typedef struct string_entry
{
	const char* text;
	int length;
	int offset;
} string_entry;

typedef struct pending_goto
{
	int name;
	int label;
} pending_goto;

/* The 32-bit operations an 8086 has no instruction for. Each is a short
 * subroutine taking DX:AX and CX:BX and returning DX:AX, emitted after the
 * program only if something called it. */
typedef enum helper
{
	h_lmul, h_uldivmod, h_ldiv, h_uldiv, h_lmod, h_ulmod,
	h_lshl, h_lshr, h_lsar, h_lcmp, h_ulcmp, helper_count
} helper;

/* What a token looks like in a diagnostic. Punctuation and keywords carry no
 * text of their own, and those are exactly the ones a parser stops on. */
static const char* spelling_of(cc_tok k)
{
	switch (k)
	{
	case tok_end: return "end of file";
	case tok_number: return "a number";
	case tok_string: return "a string";
	case tok_character: return "a character constant";
	case tok_kw_int: return "int";
	case tok_kw_char: return "char";
	case tok_kw_void: return "void";
	case tok_kw_unsigned: return "unsigned";
	case tok_kw_signed: return "signed";
	case tok_kw_short: return "short";
	case tok_kw_long: return "long";
	case tok_kw_if: return "if";
	case tok_kw_else: return "else";
	case tok_kw_while: return "while";
	case tok_kw_for: return "for";
	case tok_kw_do: return "do";
	case tok_kw_return: return "return";
	case tok_kw_break: return "break";
	case tok_kw_continue: return "continue";
	case tok_kw_switch: return "switch";
	case tok_kw_case: return "case";
	case tok_kw_default: return "default";
	case tok_kw_struct: return "struct";
	case tok_kw_union: return "union";
	case tok_kw_enum: return "enum";
	case tok_kw_typedef: return "typedef";
	case tok_kw_sizeof: return "sizeof";
	case tok_kw_static: return "static";
	case tok_kw_extern: return "extern";
	case tok_kw_const: return "const";
	case tok_kw_goto: return "goto";
	case tok_l_paren: return "(";
	case tok_r_paren: return ")";
	case tok_l_brace: return "{";
	case tok_r_brace: return "}";
	case tok_l_bracket: return "[";
	case tok_r_bracket: return "]";
	case tok_semicolon: return ";";
	case tok_comma: return ",";
	case tok_dot: return ".";
	case tok_arrow: return "->";
	case tok_question: return "?";
	case tok_colon: return ":";
	case tok_plus: return "+";
	case tok_minus: return "-";
	case tok_star: return "*";
	case tok_slash: return "/";
	case tok_percent: return "%";
	case tok_amp: return "&";
	case tok_pipe: return "|";
	case tok_caret: return "^";
	case tok_tilde: return "~";
	case tok_bang: return "!";
	case tok_assign: return "=";
	case tok_less: return "<";
	case tok_greater: return ">";
	case tok_inc: return "++";
	case tok_dec: return "--";
	case tok_shl: return "<<";
	case tok_shr: return ">>";
	case tok_le: return "<=";
	case tok_ge: return ">=";
	case tok_eq: return "==";
	case tok_ne: return "!=";
	case tok_and_and: return "&&";
	case tok_or_or: return "||";
	case tok_ellipsis: return "...";
	default: return "this token";
	}
}

static int precedence_of(cc_tok k)
{
	switch (k)
	{
	case tok_star: case tok_slash: case tok_percent: return 10;
	case tok_plus: case tok_minus: return 9;
	case tok_shl: case tok_shr: return 8;
	case tok_less: case tok_greater: case tok_le: case tok_ge: return 7;
	case tok_eq: case tok_ne: return 6;
	case tok_amp: return 5;
	case tok_caret: return 4;
	case tok_pipe: return 3;
	case tok_and_and: return 2;
	case tok_or_or: return 1;
	default: return 0;
	}
}

static int is_comparison(cc_tok k) { return precedence_of(k) == 6 || precedence_of(k) == 7; }

static condition condition_of(cc_tok op, int unsigned_operation)
{
	switch (op)
	{
	case tok_eq: return cond_equal;
	case tok_ne: return cond_not_equal;
	case tok_less: return unsigned_operation ? cond_below : cond_less;
	case tok_le: return unsigned_operation ? cond_below_equal : cond_less_equal;
	case tok_greater: return unsigned_operation ? cond_above : cond_greater;
	default: return unsigned_operation ? cond_above_equal : cond_greater_equal;
	}
}

typedef struct generator
{
	cc_token_stream* tokens_;
	cc_options options_;
	cc_target target_;
	emitter e_;
	cc_list data_; /* uint8_t */
	cc_list data_fixups_; /* data_fixup */
	cc_list types_; /* type_info */
	cc_list records_; /* record_info */
	cc_list signatures_; /* cc_list of int */
	cc_list symbols_; /* symbol */
	cc_list strings_; /* string_entry */
	cc_list function_fixups_; /* function_fixup */
	cc_map labels_; /* interned name to label */
	cc_list pending_gotos_; /* pending_goto */
	cc_list break_labels_; /* int */
	cc_list continue_labels_; /* int */
	cc_list switches_; /* switch_context */
	int at_;
	int scope_;
	int frame_;
	int frame_patch_;
	int prologue_slots_; /* reserved room for the register saves */
	int body_start_;
	cc_list epilogue_slots_; /* int */
	int stack_patch_;
	int paragraph_patch_;
	int return_type_;
	int hoisted_;
	int current_function_;
	cc_list inline_returns_; /* int, the label the body's returns jump to */
	cc_list inline_stack_; /* int, what is being expanded, so it cannot expand itself */
	int helper_label_[helper_count];

	int t_void_, t_char_, t_uchar_, t_int_, t_uint_, t_long_, t_ulong_, t_real_;

	/* Where an error goes. The parse is recursive descent and an error can be
	 * found arbitrarily deep in it, so this is the one place a longjmp earns
	 * its keep: the alternative is a check after every one of two hundred
	 * calls. */
	jmp_buf recover;
	char message[192];
	int error_line;
	const char* error_file;
} generator;

#define SYMBOL_AT(g, index) (((symbol*)(g)->symbols_.items)[index])
#define TYPE_AT(g, index) (((type_info*)(g)->types_.items)[index])
#define RECORD_AT(g, index) (((record_info*)(g)->records_.items)[index])
#define MEMBER_AT(record, index) (((record_member*)(record)->members.items)[index])
#define INT_AT(list, index) (((int*)(list).items)[index])
#define DATA_AT(g, index) (((uint8_t*)(g)->data_.items)[index])

static void gen_fail(generator* g, const char* message);

/* ---- tokens ---- */

static const cc_token* gen_peek(const generator* g, int ahead)
{
	const int index = g->at_ + ahead;
	return &CC_TOKEN(*g->tokens_, index < g->tokens_->tokens.count ? index : g->tokens_->tokens.count - 1);
}

static cc_tok gen_kind(const generator* g) { return gen_peek(g, 0)->kind; }

static void gen_advance(generator* g)
{
	if (g->at_ + 1 < g->tokens_->tokens.count) ++g->at_;
}

static int gen_accept(generator* g, cc_tok k)
{
	if (gen_kind(g) != k) return 0;
	gen_advance(g);
	return 1;
}

static void gen_expect(generator* g, cc_tok k, const char* message)
{
	if (!gen_accept(g, k)) gen_fail(g, message);
}

static void gen_fail(generator* g, const char* message)
{
	const cc_token* token = gen_peek(g, 0);
	const int spelt = token->text != NULL && token->text[0] != 0;

	/* Naming the token the parser stopped at is most of a diagnostic, and
	 * survives a line number that a macro expansion has moved. */
	if (spelt)
		snprintf(g->message, sizeof(g->message), "%s, at '%.*s'", message, token->length, token->text);
	else
		snprintf(g->message, sizeof(g->message), "%s, at %s", message, spelling_of(token->kind));

	g->error_line = token->line;
	g->error_file = token->file;
	longjmp(g->recover, 1);
}

/* The same, for a message that has to be built first. */
static void gen_fail_name(generator* g, const char* what, const char* name)
{
	char text[160];
	snprintf(text, sizeof(text), "%s%s", what, name);
	gen_fail(g, text);
}

/* ---- types ---- */

static int gen_add_type(generator* g, const type_info* info)
{
	*(type_info*)cc_list_add(&g->types_) = *info;
	return g->types_.count - 1;
}

static void type_info_init(type_info* info)
{
	memset(info, 0, sizeof(*info));
	info->kind = ty_int;
	info->size = 2;
	info->base = -1;
	info->record = -1;
	info->signature = -1;
}

static const type_info* gen_type(const generator* g, int index) { return &TYPE_AT(g, index); }
static int gen_type_size(const generator* g, int index) { return TYPE_AT(g, index).size; }
static int gen_is_pointer(const generator* g, int index) { return TYPE_AT(g, index).kind == ty_pointer; }
static int gen_is_array(const generator* g, int index) { return TYPE_AT(g, index).kind == ty_array; }
static int gen_is_record(const generator* g, int index) { return TYPE_AT(g, index).kind == ty_record; }

static int gen_is_scalar_pointer(const generator* g, int index)
{
	return gen_is_pointer(g, index) || gen_is_array(g, index);
}

static int gen_is_unsigned(const generator* g, int index)
{
	const type_kind k = TYPE_AT(g, index).kind;
	return k == ty_uint || k == ty_uchar || k == ty_ulong || k == ty_pointer;
}

/* A 32-bit arithmetic type, which lives in DX:AX rather than in AX. */
static int gen_is_wide(const generator* g, int index)
{
	const type_kind k = TYPE_AT(g, index).kind;
	return k == ty_long || k == ty_ulong;
}

/* There is no floating point at run time, so a real only ever exists as a
 * constant on the way to an integer. */
static int gen_is_real(const generator* g, int index) { return TYPE_AT(g, index).kind == ty_real; }

static int gen_target_of(const generator* g, int index)
{
	const type_info* info = &TYPE_AT(g, index);
	return info->base >= 0 ? info->base : g->t_int_;
}

static int gen_pointer_to(generator* g, int base)
{
	type_info info;
	int i;
	for (i = 0; i < g->types_.count; ++i)
		if (TYPE_AT(g, i).kind == ty_pointer && TYPE_AT(g, i).base == base) return i;
	type_info_init(&info);
	info.kind = ty_pointer;
	info.size = g->target_.pointer_size;
	info.base = base;
	return gen_add_type(g, &info);
}

static int gen_array_of(generator* g, int base, int count)
{
	type_info info;
	type_info_init(&info);
	info.kind = ty_array;
	info.base = base;
	info.count = count;
	info.size = count * gen_type_size(g, base);
	return gen_add_type(g, &info);
}

static int gen_function_type(generator* g, int result, const cc_list* parameters)
{
	type_info info;
	cc_list* copy = (cc_list*)cc_list_add(&g->signatures_);
	int i;

	cc_list_init(copy, (int)sizeof(int));
	for (i = 0; i < parameters->count; ++i) *(int*)cc_list_add(copy) = INT_AT(*parameters, i);

	type_info_init(&info);
	info.kind = ty_function;
	info.size = 1;
	info.base = result;
	info.signature = g->signatures_.count - 1;
	return gen_add_type(g, &info);
}

static int gen_is_function_type(const generator* g, int index)
{
	return TYPE_AT(g, index).kind == ty_function;
}

static int gen_is_function_pointer(const generator* g, int index)
{
	return gen_is_pointer(g, index) && gen_is_function_type(g, gen_target_of(g, index));
}

static const cc_list* gen_signature_of(const generator* g, int function)
{
	return (const cc_list*)cc_list_at(&g->signatures_, TYPE_AT(g, function).signature);
}

/* The label a function's code starts at, made as soon as the name is known so
 * that a call never has to wait for the definition. */
static int gen_label_of(generator* g, int function)
{
	symbol* entry = &SYMBOL_AT(g, function);
	if (entry->label < 0) entry->label = emit_label(&g->e_);
	return entry->label;
}

/* ---- symbols ---- */

static int gen_find(const generator* g, int name)
{
	int i;
	if (name == 0) return -1;
	for (i = g->symbols_.count; i > 0; --i)
		if (SYMBOL_AT(g, i - 1).name == name) return i - 1;
	return -1;
}

static int gen_define(generator* g, int name, symbol_kind kind, int type)
{
	symbol* entry = (symbol*)cc_list_add(&g->symbols_);
	entry->name = name;
	entry->kind = kind;
	entry->type = type;
	entry->scope = g->scope_;
	entry->label = -1;
	entry->start = -1;
	entry->finish = -1;
	entry->body = -1;
	cc_list_init(&entry->parameters, (int)sizeof(int));
	cc_list_init(&entry->parameter_names, (int)sizeof(int));
	return g->symbols_.count - 1;
}

static void gen_leave_scope(generator* g, int level)
{
	int i;
	while (g->symbols_.count > 0 && SYMBOL_AT(g, g->symbols_.count - 1).scope > level)
	{
		symbol* entry = &SYMBOL_AT(g, g->symbols_.count - 1);
		cc_list_free(&entry->parameters);
		cc_list_free(&entry->parameter_names);
		g->symbols_.count -= 1;
	}
	if (!g->hoisted_) return;

	/* A function declared implicitly inside a body belongs to file scope but was
	 * pushed after the locals around it, so the pop above stops at it and leaves
	 * them behind. What is left is retired in place rather than removed, because
	 * a fixup refers to a symbol by index. */
	for (i = 0; i < g->symbols_.count; ++i)
	{
		symbol* entry = &SYMBOL_AT(g, i);
		if (entry->scope <= level) continue;
		entry->scope = level;
		entry->name = 0;
	}
}

/* ---- data ---- */

static int gen_reserve_data(generator* g, int size, int alignment)
{
	int offset;
	int i;
	while (g->data_.count % alignment != 0) *(uint8_t*)cc_list_add(&g->data_) = 0;
	offset = g->data_.count;
	for (i = 0; i < (size < 1 ? 1 : size); ++i) *(uint8_t*)cc_list_add(&g->data_) = 0;
	return offset;
}

static int gen_add_string(generator* g, const char* text, int length)
{
	int offset;
	int i;
	for (i = 0; i < g->strings_.count; ++i)
	{
		const string_entry* entry = (const string_entry*)cc_list_at(&g->strings_, i);
		if (entry->length == length && memcmp(entry->text, text, (size_t)length) == 0)
			return entry->offset;
	}

	offset = g->data_.count;
	for (i = 0; i < length; ++i) *(uint8_t*)cc_list_add(&g->data_) = (uint8_t)text[i];
	*(uint8_t*)cc_list_add(&g->data_) = 0;
	{
		string_entry* entry = (string_entry*)cc_list_add(&g->strings_);
		entry->text = text;
		entry->length = length;
		entry->offset = offset;
	}
	return offset;
}

/* The address in the instruction just emitted is an object in the data section,
 * which the layout pass has not placed yet. */
static void gen_data_reference(generator* g, int offset) { emit_refers_to_data(&g->e_, offset); }

/* A pointer to a data object, held in the data section itself. Data does not
 * move, so this one is still an offset. */
static void gen_data_pointer(generator* g, int position, int offset)
{
	data_fixup* fixup = (data_fixup*)cc_list_add(&g->data_fixups_);
	fixup->position = position;
	fixup->offset = offset;
}

/* Everything a speculatively generated expression can have added to the output,
 * so that it can be taken back if the expression turns out to fold or to want
 * its operands somewhere else. */
typedef struct emission_mark
{
	emitter_position code;
	int data_fixups;
	int function_fixups;
} emission_mark;

static emission_mark gen_mark(const generator* g)
{
	emission_mark m;
	m.code = emit_tell(&g->e_);
	m.data_fixups = g->data_fixups_.count;
	m.function_fixups = g->function_fixups_.count;
	return m;
}

static void gen_discard(generator* g, emission_mark m)
{
	emit_rewind(&g->e_, m.code);
	cc_list_set_count(&g->data_fixups_, m.data_fixups);
	cc_list_set_count(&g->function_fixups_, m.function_fixups);
}

static int gen_emitted_since(const generator* g, emission_mark m)
{
	if (g->e_.code.count != m.code.code || g->e_.literals.count != m.code.literals) return 1;
	return g->data_fixups_.count != m.data_fixups || g->function_fixups_.count != m.function_fixups;
}

/* A dereference whose address has just been computed into AX. */
static void make_deref(value* v, int type)
{
	v->kind = val_deref;
	v->type = type;
	v->address = addr_reg;
	v->bias = 0;
}

static void value_init(value* v)
{
	memset(v, 0, sizeof(*v));
	v->kind = val_const;
	v->address = addr_reg;
}

/* An operand that generated no code of its own can be put wherever the operator
 * wants it, which is what lets one of the two skip the stack. */
static int gen_settled(const generator* g, const value* v)
{
	(void)g;
	return v->kind == val_const || v->kind == val_local || v->kind == val_global;
}

static int in_memory(const value* v) { return v->kind == val_local || v->kind == val_global; }

static address_source source_of(const value* v)
{
	return v->kind == val_local ? addr_local : addr_global;
}

static int gen_fits_cx(const generator* g, const value* v)
{
	if (gen_is_real(g, v->type)) return 0;
	if (v->kind == val_const) return !gen_is_wide(g, v->type);
	if (v->kind != val_local && v->kind != val_global) return 0;
	return gen_is_array(g, v->type) || gen_is_record(g, v->type) || gen_type_size(g, v->type) == 2;
}

static int gen_fits_cx_bx(const generator* g, const value* v)
{
	if (gen_is_real(g, v->type) || gen_is_array(g, v->type) || gen_is_record(g, v->type)) return 0;
	if (v->kind == val_const) return 1;
	if (v->kind != val_local && v->kind != val_global) return 0;
	/* A narrow operand only widens into CX:BX for free when it is unsigned; a
	 * signed one needs AX, which holds the other operand. */
	return gen_is_wide(g, v->type) ||
		(gen_type_size(g, v->type) == 2 && gen_is_unsigned(g, v->type));
}

/* The address of a global is a constant, even though what it is is only settled
 * at the end, so it can fold into the instruction. */
static int gen_add_address_to_ax(generator* g, const value* v)
{
	if (v->kind != val_global || !(gen_is_array(g, v->type) || gen_is_record(g, v->type))) return 0;
	emit_add_ax_imm(&g->e_, 0);
	gen_data_reference(g, v->offset);
	return 1;
}

/* True when the operand starting here is a single token that generates no code,
 * so the operator can parse it before deciding anything. */
static int gen_single_operand_ahead(const generator* g, int level)
{
	const cc_tok k = gen_peek(g, 0)->kind;
	if (k != tok_number && k != tok_character && k != tok_identifier) return 0;
	switch (gen_peek(g, 1)->kind)
	{
	case tok_l_paren: case tok_l_bracket: case tok_dot: case tok_arrow:
	case tok_inc: case tok_dec:
		return 0;
	default:
		return precedence_of(gen_peek(g, 1)->kind) <= level;
	}
}

static void gen_skip_qualifiers(generator* g)
{
	while (gen_kind(g) == tok_kw_const || gen_kind(g) == tok_kw_qualifier) gen_advance(g);
}

static int gen_helper_label(generator* g, int which)
{
	if (g->helper_label_[which] < 0) g->helper_label_[which] = emit_label(&g->e_);
	return g->helper_label_[which];
}

	// Code generation
	// Code generation
	// Code generation
	// Code generation
	// -----------------------------------------------------------------------

	// Puts the address of a dereference in BX. When it is still sitting in a
	// pointer variable this is one instruction and costs no stack slot.
static void gen_address_to_bx(generator* g, const value* v)
	{
		switch (v->address)
		{
		case addr_local:
			emit_mov_bx_frame(&g->e_, v->offset);
			break;
		case addr_global:
			emit_mov_bx_absolute(&g->e_, 0);
			gen_data_reference(g, v->offset);
			break;
		default:
			emit_mov_bx_ax(&g->e_);
			break;
		}
	}

static void gen_store_indirect(generator* g,int type_index, int bias)
	{
		if (gen_type_size(g, type_index) == 1)
		{
			emit_store_al_indirect(&g->e_, bias);
			return;
		}
		emit_store_ax_indirect(&g->e_, bias);
		if (gen_is_wide(g, type_index)) emit_store_dx_indirect_high(&g->e_, bias);
	}

	// Sign or zero extends AX into DX:AX, so a 16-bit value can take part in a
	// 32-bit operation.
static void gen_widen(generator* g,int from_type)
	{
		if (gen_is_unsigned(g, from_type)) emit_xor_dx_dx(&g->e_);
		else emit_cwd(&g->e_);
	}

	// A byte fetched into AX. A 386 does it in one instruction; an 8086 loads AL
	// and then fixes AH. The width says the byte, not the destination, so that
	// the register allocator can see the slot is not a whole word.
static void gen_load_byte_widened(generator* g, operand from, int is_signed, int data_offset)
	{
		if (g->target_.cpu >= cc_cpu_i80386)
		{
			emit_op(&g->e_, is_signed ? op_movsx : op_movzx, 1, operand_in(reg_a), from);
			if (data_offset >= 0) gen_data_reference(g, data_offset);
			return;
		}
		emit_op(&g->e_, op_mov, 1, operand_in(reg_a), from);
		if (data_offset >= 0) gen_data_reference(g, data_offset);
		if (is_signed) emit_cbw(&g->e_);
		else emit_zero_ah(&g->e_);
	}

static void gen_load(generator* g, value* v)
	{
		// There is no floating point at run time: a real has to have been cast
		// to an integer before anything asks for it in a register.
		if (gen_is_real(g, v->type)) gen_fail(g, "floating point is only available in a constant expression");
		const int wide = gen_is_wide(g, v->type);
		switch (v->kind)
		{
		case val_const:
			emit_mov_ax_imm(&g->e_, (int)(v->constant & 0xffff));
			if (wide) emit_mov_dx_imm(&g->e_, (int)((v->constant >> 16) & 0xffff));
			break;

		case val_reg:
			break;

		case val_flags:
			// The comparison is only in the flags, so it costs something to ask
			// for it as a number.
			emit_set_from_condition(&g->e_, (condition)(v->offset));
			break;

		case val_local:
			if (gen_is_array(g, v->type) || gen_is_record(g, v->type)) emit_lea_ax_frame(&g->e_, v->offset);
			else if (gen_type_size(g, v->type) == 1)
				gen_load_byte_widened(g, emit_frame(v->offset), !gen_is_unsigned(g, v->type));
			else
			{
				emit_load_ax_frame(&g->e_, v->offset);
				if (wide) emit_load_dx_frame(&g->e_, v->offset + 2);
			}
			break;

		case val_global:
			if (gen_is_array(g, v->type) || gen_is_record(g, v->type))
			{
				emit_mov_ax_imm(&g->e_, 0);
				gen_data_reference(g, v->offset);
			}
			else if (gen_type_size(g, v->type) == 1)
				gen_load_byte_widened(g, operand_absolute(0), !gen_is_unsigned(g, v->type), v->offset);
			else
			{
				emit_load_ax_absolute(&g->e_, 0);
				gen_data_reference(g, v->offset);
				if (wide)
				{
					emit_load_dx_absolute(&g->e_, 0);
					gen_data_reference(g, v->offset + 2);
				}
			}
			break;

		default: // val_deref
			if (gen_is_array(g, v->type) || gen_is_record(g, v->type))
			{
				// What is wanted is the address, not what is there.
				if (v->address == addr_local) emit_load_ax_frame(&g->e_, v->offset);
				else if (v->address == addr_global)
				{
					emit_load_ax_absolute(&g->e_, 0);
					gen_data_reference(g, v->offset);
				}
				if (v->bias != 0) emit_add_ax_imm(&g->e_, v->bias);
				break;
			}
			gen_address_to_bx(g, &v);
			if (gen_type_size(g, v->type) == 1)
				gen_load_byte_widened(g, emit_pointee(v->bias), !gen_is_unsigned(g, v->type));
			else
			{
				emit_load_ax_indirect(&g->e_, v->bias);
				if (wide) emit_load_dx_indirect_high(&g->e_, v->bias);
			}
			break;
		}

		// An array used as a value decays to a pointer to its first element.
		if (gen_is_array(g, v->type)) v->type = gen_pointer_to(g, gen_target_of(g, v->type));
		v->kind = val_reg;
	}

	// Loads a value and converts it to the type the context wants, which on a
	// 16-bit target only ever means widening to DX:AX.
static void gen_load_converted(generator* g, value* v, int to_type)
	{
		const int want_wide = gen_is_wide(g, to_type);
		if (want_wide && v->kind == val_const)
		{
			// A narrow constant has to be sign extended before it is split.
			if (!gen_is_wide(g, v->type) && !gen_is_unsigned(g, v->type)) v->constant = (long)(int16_t(v->constant));
			v->type = to_type;
			gen_load(g, &v);
			return;
		}
		const int was_wide = gen_is_wide(g, v->type);
		const int from = v->type;
		gen_load(g, &v);
		if (want_wide && !was_wide) gen_widen(g, from);
		v->type = to_type;
	}

static void gen_load_address(generator* g, value* v)
	{
		switch (v->kind)
		{
		case val_local:
			emit_lea_ax_frame(&g->e_, v->offset);
			break;
		case val_global:
			emit_mov_ax_imm(&g->e_, 0);
			gen_data_reference(g, v->offset);
			break;
		case val_deref:
			if (v->address == addr_local) emit_load_ax_frame(&g->e_, v->offset);
			else if (v->address == addr_global)
			{
				emit_load_ax_absolute(&g->e_, 0);
				gen_data_reference(g, v->offset);
			}
			if (v->bias != 0) emit_add_ax_imm(&g->e_, v->bias);
			break;
		default:
			gen_fail(g, "this expression has no address");
		}
		v->kind = val_reg;
		v->type = gen_pointer_to(g, v->type);
	}

	// Stores AX into the target. For a dereference the caller must have pushed
	// the target address first.
static void gen_store(generator* g, const value* target)
	{
		const int wide = gen_is_wide(g, target->type);
		switch (target->kind)
		{
		case val_local:
			if (gen_type_size(g, target->type) == 1) emit_store_al_frame(&g->e_, target->offset);
			else
			{
				emit_store_ax_frame(&g->e_, target->offset);
				if (wide) emit_store_dx_frame(&g->e_, target->offset + 2);
			}
			break;
		case val_global:
			if (gen_type_size(g, target->type) == 1)
			{
				emit_store_al_absolute(&g->e_, 0);
				gen_data_reference(g, target->offset);
			}
			else
			{
				emit_store_ax_absolute(&g->e_, 0);
				gen_data_reference(g, target->offset);
				if (wide)
				{
					emit_store_dx_absolute(&g->e_, 0);
					gen_data_reference(g, target->offset + 2);
				}
			}
			break;
		case val_deref:
			if (target->address == addr_reg) emit_pop_bx(&g->e_);
			else gen_address_to_bx(g, &target);
			if (gen_type_size(g, target->type) == 1)
			{
				emit_store_al_indirect(&g->e_, target->bias);
				break;
			}
			emit_store_ax_indirect(&g->e_, target->bias);
			if (wide) emit_store_dx_indirect_high(&g->e_, target->bias);
			break;
		default:
			gen_fail(g, "cannot assign to this expression");
		}
	}

	// Reduces a value to the flags a conditional jump can be taken from, and
	// says which condition means 1. A comparison is already there.
static condition gen_test_value(generator* g, value* v)
	{
		if (v->kind == val_flags) return (condition)(v->offset);
		const int wide = gen_is_wide(g, v->type);
		gen_load(g, &v);
		if (wide) emit_or_ax_dx(&g->e_); // zero only when both halves are zero
		else emit_test_ax_ax(&g->e_);
		return cond_not_equal;
	}

static void gen_call_helper(generator* g,helper which)
	{
		emit_call(&g->e_, gen_helper_label(g, which));
	}

	// The second operand of a narrow operator, put straight in CX. Only called
	// for a value that needs no code of its own, so AX is left alone.
static void gen_load_to_cx(generator* g, value* v)
	{
		switch (v->kind)
		{
		case val_const:
			emit_mov_cx_imm(&g->e_, (int)(v->constant & 0xffff));
			break;
		case val_local:
			if (gen_is_array(g, v->type) || gen_is_record(g, v->type)) emit_lea_cx_frame(&g->e_, v->offset);
			else emit_load_cx_frame(&g->e_, v->offset);
			break;
		default:
			if (gen_is_array(g, v->type) || gen_is_record(g, v->type)) emit_mov_cx_imm(&g->e_, 0);
			else emit_load_cx_absolute(&g->e_, 0);
			gen_data_reference(g, v->offset);
			break;
		}
		if (gen_is_array(g, v->type)) v->type = gen_pointer_to(g, gen_target_of(g, v->type));
		v->kind = val_reg;
	}

	// The same for a 32-bit operator, whose second operand lives in CX:BX.
static void gen_load_to_cx_bx(generator* g, value* v)
	{
		if (v->kind == val_const)
		{
			long constant = v->constant;
			if (!gen_is_wide(g, v->type) && !gen_is_unsigned(g, v->type)) constant = (long)(int16_t(constant));
			emit_mov_bx_imm(&g->e_, (int)(constant & 0xffff));
			emit_mov_cx_imm(&g->e_, (int)((constant >> 16) & 0xffff));
			v->kind = val_reg;
			return;
		}

		if (v->kind == val_local)
		{
			emit_load_bx_frame(&g->e_, v->offset);
			if (gen_is_wide(g, v->type)) emit_load_cx_frame(&g->e_, v->offset + 2);
			else emit_xor_cx_cx(&g->e_);
		}
		else
		{
			emit_load_bx_absolute(&g->e_, 0);
			gen_data_reference(g, v->offset);
			if (gen_is_wide(g, v->type))
			{
				emit_load_cx_absolute(&g->e_, 0);
				gen_data_reference(g, v->offset + 2);
			}
			else
			{
				emit_xor_cx_cx(&g->e_);
			}
		}
		v->kind = val_reg;
	}

	// A shift by a count known now. Up to three single-bit shifts beat loading
	// CL, and every one of them beats the multiply or divide it may stand for.
static void gen_shift_ax_constant(generator* g,cc_tok op, int count, int unsigned_operation)
	{
		if (count <= 0) return;
		if (count <= 3)
		{
			for (int i = 0; i < count; ++i)
			{
				if (op == tok_shl) emit_shl_ax_one(&g->e_);
				else if (unsigned_operation) emit_shr_ax_one(&g->e_);
				else emit_sar_ax_one(&g->e_);
			}
			return;
		}
		emit_mov_cl_imm(&g->e_, count);
		if (op == tok_shl) emit_shl_ax_cl(&g->e_);
		else if (unsigned_operation) emit_shr_ax_cl(&g->e_);
		else emit_sar_ax_cl(&g->e_);
	}

	// The same for the 32-bit pair. A shift of 16 or more is just a move
	// between the halves, which is what makes the fixed-point idiom cheap.
static void gen_shift_wide_constant(generator* g,cc_tok op, int count, int unsigned_operation)
	{
		if (count <= 0) return;
		if (count >= 16)
		{
			if (op == tok_shl)
			{
				emit_mov_dx_ax(&g->e_);
				emit_xor_ax_ax(&g->e_);
				const int rest = count - 16;
				if (rest > 0 && rest <= 3) for (int i = 0; i < rest; ++i) emit_shl_dx_one(&g->e_);
				else if (rest > 3)
				{
					emit_mov_cl_imm(&g->e_, rest);
					emit_shl_dx_cl(&g->e_);
				}
			}
			else
			{
				emit_mov_ax_dx(&g->e_);
				if (unsigned_operation) emit_xor_dx_dx(&g->e_);
				else emit_cwd(&g->e_);
				gen_shift_ax_constant(g, op, count - 16, unsigned_operation);
			}
			return;
		}

		if (count <= 4)
		{
			for (int i = 0; i < count; ++i)
			{
				if (op == tok_shl) emit_shl_pair_one(&g->e_);
				else if (unsigned_operation) emit_shr_pair_one(&g->e_);
				else emit_sar_pair_one(&g->e_);
			}
			return;
		}

		// Anything else is shorter as the loop in the runtime helper.
		emit_mov_bx_imm(&g->e_, count);
		gen_apply_operator_wide(g, op, unsigned_operation);
	}

	// Multiplies AX by an element size. A shift is both shorter and an order of
	// magnitude faster than the multiply on an 8086.
static void gen_scale_ax(generator* g,int size)
	{
		if (size == 1) return;
		for (int bit = 1; bit < 16; ++bit)
		{
			if (size != (1 << bit)) continue;
			gen_shift_ax_constant(g, tok_shl, bit, 1);
			return;
		}
		emit_mov_cx_imm(&g->e_, size);
		emit_imul_cx(&g->e_);
	}

	// DX:AX holds the left operand and the right one is a constant. Returns
	// 0 when the operator has no immediate form, having generated nothing.
static int gen_apply_operator_wide_constant(generator* g,cc_tok op, long constant, int unsigned_operation)
	{
		const int low = (int)(constant & 0xffff);
		const int high = (int)((constant >> 16) & 0xffff);

		switch (op)
		{
		case tok_plus:
			if (low == 0 && high == 0) return 1;
			if (low == 0) emit_add_dx_imm(&g->e_, high);
			else
			{
				emit_add_ax_imm(&g->e_, low);
				if (high == 0) emit_adc_dx_zero(&g->e_);
				else emit_adc_dx_imm(&g->e_, high);
			}
			return 1;

		case tok_minus:
			if (low == 0 && high == 0) return 1;
			if (low == 0) emit_sub_dx_imm(&g->e_, high);
			else
			{
				emit_sub_ax_imm(&g->e_, low);
				if (high == 0) emit_sbb_dx_zero(&g->e_);
				else emit_sbb_dx_imm(&g->e_, high);
			}
			return 1;

		case tok_amp:
			if (low == 0) emit_xor_ax_ax(&g->e_);
			else if (low != 0xffff) emit_and_ax_imm(&g->e_, low);
			if (high == 0) emit_xor_dx_dx(&g->e_);
			else if (high != 0xffff) emit_and_dx_imm(&g->e_, high);
			return 1;

		case tok_pipe:
			if (low != 0) emit_or_ax_imm(&g->e_, low);
			if (high != 0) emit_or_dx_imm(&g->e_, high);
			return 1;

		case tok_caret:
			if (low != 0) emit_xor_ax_imm(&g->e_, low);
			if (high != 0) emit_xor_dx_imm(&g->e_, high);
			return 1;

		case tok_star:
			if (constant == 1) return 1;
			for (int bit = 1; bit < 31; ++bit)
			{
				if (constant != (1L << bit)) continue;
				gen_shift_wide_constant(g, tok_shl, bit, 1);
				return 1;
			}
			return 0;

		case tok_shl:
		case tok_shr:
			gen_shift_wide_constant(g, op, (int)(constant & 31), unsigned_operation);
			return 1;

		default:
			return 0;
		}
	}

	// AX holds the left operand and the right one is a constant. Returns 0
	// when the operator has no immediate form, and nothing has been generated.
static int gen_apply_operator_constant(generator* g,cc_tok op, long constant, int unsigned_operation)
	{
		const int value16 = (int)(constant & 0xffff);
		const auto power_of_two = [](int v)
		{
			for (int bit = 1; bit < 16; ++bit)
				if (v == (1 << bit)) return bit;
			return 0;
		};

		switch (op)
		{
		case tok_plus:
			if (value16 == 0) return 1;
			if (value16 == 1) emit_inc_ax(&g->e_);
			else if (value16 == 0xffff) emit_dec_ax(&g->e_);
			else emit_add_ax_imm(&g->e_, value16);
			return 1;

		case tok_minus:
			if (value16 == 0) return 1;
			if (value16 == 1) emit_dec_ax(&g->e_);
			else if (value16 == 0xffff) emit_inc_ax(&g->e_);
			else emit_sub_ax_imm(&g->e_, value16);
			return 1;

		case tok_amp:
			if (value16 == 0xffff) return 1;
			if (value16 == 0) emit_xor_ax_ax(&g->e_);
			else emit_and_ax_imm(&g->e_, value16);
			return 1;

		case tok_pipe:
			if (value16 != 0) emit_or_ax_imm(&g->e_, value16);
			return 1;

		case tok_caret:
			if (value16 != 0) emit_xor_ax_imm(&g->e_, value16);
			return 1;

		case tok_star:
			if (value16 == 0) emit_xor_ax_ax(&g->e_);
			else if (value16 == 1) return 1;
			else if (value16 == 0xffff) emit_neg_ax(&g->e_);
			else if (const int bit = power_of_two(value16)) gen_shift_ax_constant(g, tok_shl, bit, 1);
			else return 0;
			return 1;

		case tok_slash:
			if (value16 == 1) return 1;
			if (!unsigned_operation) return 0;
			if (const int bit = power_of_two(value16)) gen_shift_ax_constant(g, tok_shr, bit, 1);
			else return 0;
			return 1;

		case tok_percent:
			if (!unsigned_operation) return 0;
			if (value16 == 1) emit_xor_ax_ax(&g->e_);
			else if (power_of_two(value16)) emit_and_ax_imm(&g->e_, value16 - 1);
			else return 0;
			return 1;

		case tok_shl:
		case tok_shr:
			gen_shift_ax_constant(g, op, value16 & 31, unsigned_operation);
			return 1;

		case tok_eq: case tok_ne: case tok_less: case tok_le:
		case tok_greater: case tok_ge:
			// A comparison against zero needs no operand at all. The result is
			// left in the flags for the caller to branch on.
			if (value16 == 0) emit_test_ax_ax(&g->e_);
			else emit_cmp_ax_imm(&g->e_, value16);
			return 1;

		default:
			return 0;
		}
	}

	// AX holds the left operand and CX the right. When reversed they are the
	// other way round, which costs an exchange for the operators that care.
static void gen_apply_operator(generator* g,cc_tok op, int unsigned_operation, int reversed)
	{
		const int commutative = op == tok_plus || op == tok_star || op == tok_amp ||
			op == tok_pipe || op == tok_caret;
		if (reversed && !is_comparison(op) && !commutative) emit_xchg_ax_cx(&g->e_);

		switch (op)
		{
		case tok_plus: emit_add_ax_cx(&g->e_); return;
		case tok_minus: emit_sub_ax_cx(&g->e_); return;
		case tok_star:
			if (unsigned_operation) emit_mul_cx(&g->e_);
			else emit_imul_cx(&g->e_);
			return;
		case tok_slash:
			if (unsigned_operation) emit_div_cx(&g->e_);
			else emit_idiv_cx(&g->e_);
			return;
		case tok_percent:
			if (unsigned_operation) emit_div_cx(&g->e_);
			else emit_idiv_cx(&g->e_);
			emit_mov_ax_dx(&g->e_);
			return;
		case tok_amp: emit_and_ax_cx(&g->e_); return;
		case tok_pipe: emit_or_ax_cx(&g->e_); return;
		case tok_caret: emit_xor_ax_cx(&g->e_); return;
		case tok_shl: emit_shl_ax_cl(&g->e_); return;
		case tok_shr:
			if (unsigned_operation) emit_shr_ax_cl(&g->e_);
			else emit_sar_ax_cl(&g->e_);
			return;
		default:
			break;
		}

		if (!is_comparison(op)) gen_fail(g, "unsupported operator");
		if (reversed) emit_cmp_cx_ax(&g->e_);
		else emit_cmp_ax_cx(&g->e_);
	}


	// DX:AX holds the left operand and CX:BX the right. Only the operations an
	// 8086 can do in a couple of instructions are inline; the rest go to a
	// helper, which is also where the register convention comes from.
static void gen_apply_operator_wide(generator* g,cc_tok op, int unsigned_operation)
	{
		switch (op)
		{
		case tok_plus: emit_add_ax_bx(&g->e_); emit_adc_dx_cx(&g->e_); return;
		case tok_minus: emit_sub_ax_bx(&g->e_); emit_sbb_dx_cx(&g->e_); return;
		case tok_amp: emit_and_ax_bx(&g->e_); emit_and_dx_cx(&g->e_); return;
		case tok_pipe: emit_or_ax_bx(&g->e_); emit_or_dx_cx(&g->e_); return;
		case tok_caret: emit_xor_ax_bx(&g->e_); emit_xor_dx_cx(&g->e_); return;
		case tok_star: gen_call_helper(g, h_lmul); return;
		case tok_slash: gen_call_helper(g, unsigned_operation ? h_uldiv : h_ldiv); return;
		case tok_percent: gen_call_helper(g, unsigned_operation ? h_ulmod : h_lmod); return;
		case tok_shl: gen_call_helper(g, h_lshl); return;
		case tok_shr: gen_call_helper(g, unsigned_operation ? h_lshr : h_lsar); return;
		default:
			break;
		}

		// A comparison goes through a helper that reduces the pair to -1, 0 or 1
		// in AX, after which the ordinary signed conditions apply whatever the
		// operands were. The result is left in the flags.
		if (!is_comparison(op)) gen_fail(g, "unsupported operator");
		gen_call_helper(g, unsigned_operation ? h_ulcmp : h_lcmp);
		emit_test_ax_ax(&g->e_);
	}

	// -----------------------------------------------------------------------
	// Expressions
	// -----------------------------------------------------------------------

	// Arguments have to reach the callee in cdecl order, which means generating
	// them right to left. Because the parser walks a token vector rather than a
	// stream, the argument spans can be located first and then generated
	// backwards. Returns how many bytes ended up on the stack.
static int gen_push_arguments(generator* g, const std::vector<int>* parameters)
	{
		gen_expect(g, tok_l_paren, "expected (");

		std::vector<size_t> starts;
		if (gen_kind(g) != tok_r_paren)
		{
			starts.push_back(g->at_);
			int depth = 0;
			for (size_t scan = g->at_;; ++scan)
			{
				const cc_tok k = CC_TOKEN(g->tokens_, (int)(scan)).kind;
				if (k == tok_end) gen_fail(g, "unterminated argument list");
				if (k == tok_l_paren || k == tok_l_bracket) ++depth;
				else if (k == tok_r_bracket) --depth;
				else if (k == tok_r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok_comma && depth == 0) starts.push_back(scan + 1);
			}
		}

		const size_t saved = g->at_;
		int pushed = 0;
		for (size_t i = starts.size(); i > 0; --i)
		{
			g->at_ = starts[i - 1];
			value argument = gen_assignment_expression(g);
			if (i - 1 < parameters->size()) gen_load_converted(g, &argument, parameters[i - 1]);
			else gen_load(g, &argument);
			if (gen_is_wide(g, argument.type))
			{
				emit_push_dx(&g->e_);
				emit_push_ax(&g->e_);
				pushed += 4;
			}
			else
			{
				emit_push_ax(&g->e_);
				pushed += 2;
			}
		}
		g->at_ = saved;

		int depth = 0;
		for (;;)
		{
			const cc_tok k = gen_kind(g);
			if (k == tok_end) gen_fail(g, "unterminated argument list");
			if (k == tok_r_paren && depth == 0) break;
			if (k == tok_l_paren || k == tok_l_bracket) ++depth;
			else if (k == tok_r_paren || k == tok_r_bracket) --depth;
			gen_advance(g);
		}
		gen_expect(g, tok_r_paren, "expected )");
		return pushed;
	}

static value gen_call(generator* g,int function_index)
	{
		if (gen_can_inline(g, function_index)) return gen_inline_call(g, function_index);

		const int pushed = gen_push_arguments(g, SYMBOL_AT(g, (int)(function_index)).parameters);

		emit_call(&g->e_, gen_label_of(g, function_index));
		SYMBOL_AT(g, (int)(function_index)).called = 1;

		if (pushed != 0) emit_add_sp_imm(&g->e_, pushed);

		value result;
		result.kind = val_reg;
		result.type = SYMBOL_AT(g, (int)(function_index)).type;
		return result;
	}

	// Whether a call is better off as a copy of the body. The body is still in
	// the token vector, so expanding it is a matter of parsing it again with
	// the parameters bound to frame slots - no tree to clone, and every
	// optimisation the generator already does applies to the copy.
static int gen_can_inline(generator* g,int function_index)
	{
		if (g->options_.inline_limit <= 0) return 0;
		const symbol& entry = SYMBOL_AT(g, (int)(function_index));
		if (!entry.defined || entry.body < 0) return 0;
		if (entry.parameters.size() != entry.parameter_names.size()) return 0;
		for (const std::string& name : entry.parameter_names)
			if (name.empty()) return 0;
		if (function_index == g->current_function_) return 0;
		if (g->inline_stack_.count >= 4) return 0;
		for (const int active : g->inline_stack_)
			if (active == function_index) return 0;

		// The argument list has to match, so that every parameter is given a
		// value and none is left over.
		size_t arguments = 0;
		{
			size_t scan = g->at_ + 1; // past the (
			int depth = 0;
			if (scan < (size_t)(g->tokens_.tokens.count) && CC_TOKEN(g->tokens_, (int)(scan)).kind != tok_r_paren)
				arguments = 1;
			for (; scan < (size_t)(g->tokens_.tokens.count); ++scan)
			{
				const cc_tok k = CC_TOKEN(g->tokens_, (int)(scan)).kind;
				if (k == tok_end) return 0;
				if (k == tok_l_paren || k == tok_l_bracket) ++depth;
				else if (k == tok_r_bracket) --depth;
				else if (k == tok_r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok_comma && depth == 0) ++arguments;
			}
		}
		if (arguments != entry.parameters.size()) return 0;

		// A body that is short, and that needs nothing the real frame would
		// have given it: no label to jump to, no storage that outlives the
		// call, and no reaching past the named parameters.
		int depth = 0;
		int length = 0;
		for (size_t scan = (size_t)(entry.body); scan < (size_t)(g->tokens_.tokens.count); ++scan)
		{
			const cc_token& token = CC_TOKEN(g->tokens_, (int)(scan));
			if (token.kind == tok_end) return 0;
			if (token.kind == tok_l_brace) ++depth;
			else if (token.kind == tok_r_brace)
			{
				if (--depth == 0) return 1;
			}
			else if (token.kind == tok_kw_goto || token.kind == tok_kw_static) return 0;
			else if (token.kind == tok_identifier && strcmp(token.text, "__args") == 0) return 0;
			if (++length > g->options_.inline_limit) return 0;
		}
		return 0;
	}

	// Whether the body ever writes to a name. A parameter it never writes to
	// can be the argument itself rather than a copy of it.
static int gen_assigned_in_body(generator* g,int body, const std::string* name)
	{
		int depth = 0;
		for (size_t scan = (size_t)(body); scan + 1 < (size_t)(g->tokens_.tokens.count); ++scan)
		{
			const cc_token& token = CC_TOKEN(g->tokens_, (int)(scan));
			if (token.kind == tok_l_brace) ++depth;
			else if (token.kind == tok_r_brace && --depth == 0) return 0;
			if (token.kind != tok_identifier || name != token.text) continue;

			const cc_tok before = scan > 0 ? CC_TOKEN(g->tokens_, (int)(scan) - 1).kind : tok_end;
			if (before == tok_amp || before == tok_inc || before == tok_dec) return 1;
			switch (CC_TOKEN(g->tokens_, (int)(scan) + 1).kind)
			{
			case tok_assign: case tok_plus_assign: case tok_minus_assign:
			case tok_star_assign: case tok_slash_assign: case tok_percent_assign:
			case tok_amp_assign: case tok_pipe_assign: case tok_caret_assign:
			case tok_shl_assign: case tok_shr_assign:
			case tok_inc: case tok_dec:
				return 1;
			default:
				break;
			}
		}
		return 1;
	}

	// The arguments of an expanded call go into frame slots of their own, which
	// is what the parameters are for the length of the body. An argument that
	// is a constant the body never writes to skips the slot and becomes the
	// parameter, which is what makes the copy smaller than the call.
static std::vector<value> gen_inline_arguments(generator* g,int function_index)
	{
		const std::vector<int> parameters = SYMBOL_AT(g, (int)(function_index)).parameters;
		const std::vector<std::string> names = SYMBOL_AT(g, (int)(function_index)).parameter_names;
		const int body = SYMBOL_AT(g, (int)(function_index)).body;

		gen_expect(g, tok_l_paren, "expected (");

		std::vector<size_t> starts;
		if (gen_kind(g) != tok_r_paren)
		{
			starts.push_back(g->at_);
			int depth = 0;
			for (size_t scan = g->at_;; ++scan)
			{
				const cc_tok k = CC_TOKEN(g->tokens_, (int)(scan)).kind;
				if (k == tok_end) gen_fail(g, "unterminated argument list");
				if (k == tok_l_paren || k == tok_l_bracket) ++depth;
				else if (k == tok_r_bracket) --depth;
				else if (k == tok_r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok_comma && depth == 0) starts.push_back(scan + 1);
			}
		}

		const size_t saved = g->at_;
		std::vector<value> bound;
		for (size_t i = 0; i < starts.size(); ++i)
		{
			g->at_ = starts[i];
			const int wanted = parameters[i];
			const emission_mark start = gen_mark(g);
			value argument = gen_assignment_expression(g);

			if (argument.kind == val_const && !gen_is_real(g, argument.type) && !gen_emitted_since(g, start) &&
				!gen_assigned_in_body(g, body, &names[i]))
			{
				gen_discard(g, start);
				if (!gen_is_wide(g, wanted) && !gen_is_real(g, wanted)) argument.constant = (long)(int16_t(argument.constant));
				else if (!gen_is_wide(g, argument.type) && !gen_is_unsigned(g, argument.type))
					argument.constant = (long)(int16_t(argument.constant));
				argument.type = wanted;
				bound.push_back(argument);
				continue;
			}

			gen_load_converted(g, &argument, wanted);
			g->frame_ += gen_is_wide(g, wanted) ? 4 : 2;
			value target;
			target.kind = val_local;
			target.type = wanted;
			target.offset = -g->frame_;
			gen_store(g, &target);
			bound.push_back(target);
		}
		g->at_ = saved;

		int depth = 0;
		for (;;)
		{
			const cc_tok k = gen_kind(g);
			if (k == tok_end) gen_fail(g, "unterminated argument list");
			if (k == tok_r_paren && depth == 0) break;
			if (k == tok_l_paren || k == tok_l_bracket) ++depth;
			else if (k == tok_r_paren || k == tok_r_bracket) --depth;
			gen_advance(g);
		}
		gen_expect(g, tok_r_paren, "expected )");
		return bound;
	}

static value gen_inline_call(generator* g,int function_index)
	{
		const std::vector<std::string> names = SYMBOL_AT(g, (int)(function_index)).parameter_names;
		const int body = SYMBOL_AT(g, (int)(function_index)).body;
		const int result_type = SYMBOL_AT(g, (int)(function_index)).type;

		const std::vector<value> bound = gen_inline_arguments(g, function_index);

		++g->scope_;
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (bound[i].kind == val_const)
			{
				const int local = gen_define(g, names[i], sym_constant, bound[i].type);
				SYMBOL_AT(g, (int)(local)).value = bound[i].constant;
				continue;
			}
			const int local = gen_define(g, names[i], sym_local, bound[i].type);
			SYMBOL_AT(g, (int)(local)).offset = bound[i].offset;
		}

		// The body is another function's, so everything that belongs to a
		// function has to be put aside for the length of it.
		const size_t saved_at = g->at_;
		const int saved_return = g->return_type_;
		std::map<std::string, int> saved_labels;
		std::vector<std::pair<std::string, int>> saved_gotos;
		std::vector<int> saved_breaks, saved_continues;
		saved_labels.swap(g->labels_);
		saved_gotos.swap(g->pending_gotos_);
		saved_breaks.swap(g->break_labels_);
		saved_continues.swap(g->continue_labels_);

		g->return_type_ = result_type;
		g->inline_returns_.push_back(emit_label(&g->e_));
		g->inline_stack_.push_back(function_index);

		g->at_ = (size_t)(body);
		gen_expect(g, tok_l_brace, "expected { to open the function body");
		while (!gen_accept(g, tok_r_brace))
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated function body");
			gen_statement(g);
		}

		emit_bind(&g->e_, g->inline_returns_.back());
		g->inline_returns_.pop_back();
		g->inline_stack_.pop_back();
		g->return_type_ = saved_return;
		g->labels_.swap(saved_labels);
		g->pending_gotos_.swap(saved_gotos);
		g->break_labels_.swap(saved_breaks);
		g->continue_labels_.swap(saved_continues);
		g->at_ = saved_at;
		gen_leave_scope(g, --g->scope_);

		value result;
		result.kind = val_reg;
		result.type = result_type;
		return result;
	}

	// A call through a pointer. The target goes to a frame slot of its own so
	// that arguments - which may contain further calls - cannot disturb it.
static value gen_indirect_call(generator* g, value* target)
	{
		const int function = gen_target_of(g, target->type);
		gen_load(g, &target);
		g->frame_ += 2;
		const int slot = -g->frame_;
		emit_store_ax_frame(&g->e_, slot);

		const int pushed = gen_push_arguments(g, gen_signature_of(g, function));
		emit_call_frame(&g->e_, slot);
		if (pushed != 0) emit_add_sp_imm(&g->e_, pushed);

		value result;
		result.kind = val_reg;
		result.type = (*gen_type(g, function)).base;
		return result;
	}

static value gen_primary_expression(generator* g)
	{
		value v;
		const cc_token token = (*gen_peek(g, 0));

		if (token.kind == tok_number || token.kind == tok_character)
		{
			gen_advance(g);
			v.kind = val_const;
			if (token.is_real)
			{
				v.type = g->t_real_;
				v.real = token.real;
				v.constant = (long)(token.real);
				return v;
			}
			if (token.wide) v.type = token.unsigned_literal ? g->t_ulong_ : g->t_long_;
			else if (token.unsigned_literal || token.value > 32767) v.type = g->t_uint_;
			else v.type = g->t_int_;
			v.constant = token.value;
			return v;
		}

		if (token.kind == tok_string)
		{
			gen_advance(g);
			v.kind = val_global;
			v.type = gen_array_of(g, g->t_char_, token.length + 1);
			v.offset = gen_add_string(g, std::string(token.text, (size_t)(token.length)));
			return v;
		}

		if (token.kind == tok_l_paren)
		{
			gen_advance(g);
			v = gen_expression(g);
			gen_expect(g, tok_r_paren, "expected )");
			return v;
		}

		if (token.kind == tok_identifier)
		{
			// __args(last) is where the arguments after the last named one begin.
			if (strcmp(token.text, "__args") == 0)
			{
				gen_advance(g);
				gen_expect(g, tok_l_paren, "expected ( after __args");
				if (gen_kind(g) != tok_identifier) gen_fail(g, "__args needs the last named parameter");
				const int index = gen_find(g, (*gen_peek(g, 0)).text);
				if (index < 0 || SYMBOL_AT(g, (int)(index)).kind != sym_local ||
					SYMBOL_AT(g, (int)(index)).offset < 0)
					gen_fail(g, "__args needs the last named parameter");
				const symbol& last = SYMBOL_AT(g, (int)(index));
				emit_lea_ax_frame(&g->e_, last.offset + (gen_is_wide(g, last.type) ? 4 : 2));
				gen_advance(g);
				gen_expect(g, tok_r_paren, "expected ) after __args");
				v.kind = val_reg;
				v.type = gen_pointer_to(g, g->t_char_);
				return v;
			}

			gen_advance(g);
			int index = gen_find(g, token.text);
			if (index < 0)
			{
				// An undeclared name is only allowed as a call, which is
				// assumed to return int as pre-standard C did.
				if (gen_kind(g) != tok_l_paren) gen_fail(g, ("undeclared identifier: " + std::string(token.text)).c_str());
				index = gen_define(g, token.text, sym_function, g->t_int_);
				SYMBOL_AT(g, (int)(index)).scope = 0;
				if (g->scope_ != 0) g->hoisted_ = 1;
			}

			switch (SYMBOL_AT(g, (int)(index)).kind)
			{
			case sym_constant:
				v.kind = val_const;
				v.type = SYMBOL_AT(g, (int)(index)).type;
				v.constant = SYMBOL_AT(g, (int)(index)).value;
				return v;
			case sym_local:
				v.kind = val_local;
				v.type = SYMBOL_AT(g, (int)(index)).type;
				v.offset = SYMBOL_AT(g, (int)(index)).offset;
				return v;
			case sym_global:
				v.kind = val_global;
				v.type = SYMBOL_AT(g, (int)(index)).type;
				v.offset = SYMBOL_AT(g, (int)(index)).offset;
				return v;
			case sym_function:
				if (gen_kind(g) == tok_l_paren) return gen_call(g, index);
				// Used without a call, a function name is its address.
				v.kind = val_reg;
				emit_mov_ax_imm(&g->e_, 0);
				emit_refers_to_code(&g->e_, gen_label_of(g, index));
				SYMBOL_AT(g, (int)(index)).called = 1;
				v.type = gen_pointer_to(g, gen_function_type(g, SYMBOL_AT(g, (int)(index)).type,
				                                  SYMBOL_AT(g, (int)(index)).parameters));
				return v;
			default:
				gen_fail(g, "a type name cannot be used here");
			}
		}

		gen_fail(g, "expected an expression");
	}

static value gen_postfix_expression(generator* g)
	{
		value v = gen_primary_expression(g);

		for (;;)
		{
			if (gen_kind(g) == tok_l_paren && gen_is_function_pointer(g, v.type))
			{
				v = gen_indirect_call(g, &v);
				continue;
			}

			if (gen_kind(g) == tok_l_bracket)
			{
				gen_advance(g);
				if (!gen_is_scalar_pointer(g, v.type)) gen_fail(g, "only a pointer or an array can be indexed");
				const int element = gen_target_of(g, v.type);
				const int size = gen_type_size(g, element);
				const int base_in_memory = gen_is_array(g, v.type) && gen_settled(g, &v);

				// An index that generates nothing can be placed after the base
				// address, and a constant one folds into the address itself.
				const emission_mark start = gen_mark(g);
				const size_t saved = g->at_;
				value index;
				int index_settled = 0;
				if ((*gen_peek(g, 1)).kind == tok_r_bracket &&
					(gen_kind(g) == tok_number || gen_kind(g) == tok_character || gen_kind(g) == tok_identifier))
				{
					index = gen_expression(g);
					index_settled = !gen_emitted_since(g, start) && gen_settled(g, &index);
					if (!index_settled)
					{
						gen_discard(g, start);
						g->at_ = saved;
					}
				}

				if (index_settled)
				{
					gen_expect(g, tok_r_bracket, "expected ]");
					if (index.kind == val_const)
					{
						const int step = (int)(index.constant) * size;
						if (base_in_memory)
						{
							// An element of an array that is itself in memory
							// is just another object in memory.
							v.offset += step;
							v.type = element;
							continue;
						}
						if (v.kind == val_deref && gen_is_array(g, v.type))
						{
							v.bias += step;
							v.type = element;
							continue;
						}
						if (in_memory( &v))
						{
							// The pointer stays where it is; the element it
							// reaches is a fixed distance from it.
							const address_source source = source_of( &v);
							v.kind = val_deref;
							v.address = source;
							v.bias = step;
							v.type = element;
							continue;
						}
						gen_load(g, &v);
						if (step != 0) emit_add_ax_imm(&g->e_, step);
					}
					else if (size == 1)
					{
						gen_load(g, &v);
						if (!gen_operate_from_memory(g, tok_plus, &index))
						{
							gen_load_to_cx(g, &index);
							emit_add_ax_cx(&g->e_);
						}
					}
					else
					{
						// Scaling the index needs AX, so the base waits in CX.
						value base = v;
						gen_load(g, &index);
						gen_scale_ax(g, size);
						if (gen_add_address_to_ax(g, &base))
						{
						}
						else if (gen_operate_from_memory(g, tok_plus, &base))
						{
						}
						else if (gen_fits_cx(g, &base))
						{
							gen_load_to_cx(g, &base);
							emit_add_ax_cx(&g->e_);
						}
						else
						{
							emit_mov_cx_ax(&g->e_);
							gen_load(g, &base);
							emit_xchg_ax_cx(&g->e_);
							emit_add_ax_cx(&g->e_);
						}
					}
					make_deref( &v, element);
					continue;
				}

				if (gen_fits_cx(g, &v))
				{
					value base = v;
					value subscript = gen_expression(g);
					gen_expect(g, tok_r_bracket, "expected ]");
					gen_load(g, &subscript);
					gen_scale_ax(g, size);
					if (!gen_add_address_to_ax(g, &base) && !gen_operate_from_memory(g, tok_plus, &base))
					{
						gen_load_to_cx(g, &base);
						emit_add_ax_cx(&g->e_);
					}
				}
				else
				{
					gen_load(g, &v);
					emit_push_ax(&g->e_);
					value subscript = gen_expression(g);
					gen_expect(g, tok_r_bracket, "expected ]");
					gen_load(g, &subscript);
					gen_scale_ax(g, size);
					emit_mov_cx_ax(&g->e_);
					emit_pop_ax(&g->e_);
					emit_add_ax_cx(&g->e_);
				}
				make_deref( &v, element);
				continue;
			}

			if (gen_kind(g) == tok_dot || gen_kind(g) == tok_arrow)
			{
				const int through_pointer = gen_kind(g) == tok_arrow;
				gen_advance(g);
				if (gen_kind(g) != tok_identifier) gen_fail(g, "expected a member name");
				const std::string member = (*gen_peek(g, 0)).text;
				gen_advance(g);

				const int record_type = through_pointer ? gen_target_of(g, v.type) : v.type;
				if (through_pointer && !gen_is_scalar_pointer(g, v.type)) gen_fail(g, "-> needs a pointer");
				if (!gen_is_record(g, record_type)) gen_fail(g, "not a structure or union");

				const record_info& info = RECORD_AT(g, (int)((*gen_type(g, record_type)).record));
				const record_member* found = NULL;
				for (const record_member& m : info.members) if (m.name == member) found = &m;
				if (found == NULL) gen_fail(g, ("no such member: " + member).c_str());

				// A member of a record that is itself in memory needs no
				// address computing: it is another object in memory, and one
				// reached through a pointer is a fixed distance from it.
				if (!through_pointer)
				{
					if (in_memory( &v))
					{
						v.offset += found->offset;
						v.type = found->type;
						continue;
					}
					if (v.kind == val_deref)
					{
						v.bias += found->offset;
						v.type = found->type;
						continue;
					}
					gen_load_address(g, &v);
				}
				else if (in_memory( &v) && !gen_is_array(g, v.type))
				{
					const address_source source = source_of( &v);
					v.kind = val_deref;
					v.address = source;
					v.bias = found->offset;
					v.type = found->type;
					continue;
				}
				else
				{
					gen_load(g, &v);
				}

				const int member_offset = found->offset;
				make_deref( &v, found->type);
				v.bias = member_offset;
				continue;
			}

			if (gen_kind(g) == tok_inc || gen_kind(g) == tok_dec)
			{
				const int increment = gen_kind(g) == tok_inc;
				gen_advance(g);
				const int step = gen_is_scalar_pointer(g, v.type) ? gen_type_size(g, gen_target_of(g, v.type)) : 1;
				const int wide = gen_is_wide(g, v.type);

				if (wide)
				{
					// BX survives the load and the store, so a dereference does
					// not need the address on the stack as the 16-bit path does.
					const value target = v;
					gen_load(g, &v);
					emit_push_dx(&g->e_);
					emit_push_ax(&g->e_);
					if (increment)
					{
						emit_add_ax_imm(&g->e_, step);
						emit_adc_dx_imm(&g->e_, 0);
					}
					else
					{
						emit_sub_ax_imm(&g->e_, step);
						emit_sbb_dx_zero(&g->e_);
					}
					if (target.kind == val_deref) gen_store_indirect(g, target.type, target.bias);
					else gen_store(g, &target);
					emit_pop_ax(&g->e_);
					emit_pop_dx(&g->e_);
					v.kind = val_reg;
					continue;
				}

				if (v.kind == val_deref)
				{
					// Loading through BX leaves the address there, so the store
					// that follows needs nothing on the stack.
					const value target = v;
					gen_load(g, &v);
					emit_push_ax(&g->e_); // the value the expression produces
					if (increment) emit_add_ax_imm(&g->e_, step);
					else emit_sub_ax_imm(&g->e_, step);
					gen_store_indirect(g, target.type, target.bias);
					emit_pop_ax(&g->e_);
				}
				else
				{
					const value target = v;
					gen_load(g, &v);
					emit_push_ax(&g->e_);
					if (increment) emit_add_ax_imm(&g->e_, step);
					else emit_sub_ax_imm(&g->e_, step);
					gen_store(g, &target);
					emit_pop_ax(&g->e_);
				}
				v.kind = val_reg;
				continue;
			}
			break;
		}
		return v;
	}

static value gen_unary_expression(generator* g)
	{
		const cc_tok k = gen_kind(g);

		if (k == tok_amp)
		{
			gen_advance(g);
			value v = gen_unary_expression(g);
			// A function name already is an address, so &f and f are the same.
			if (gen_is_function_pointer(g, v.type) && v.kind == val_reg) return v;
			gen_load_address(g, &v);
			return v;
		}
		if (k == tok_star)
		{
			gen_advance(g);
			value v = gen_unary_expression(g);
			// Dereferencing a function pointer leaves it a function pointer, so
			// that (*f)(x) and f(x) mean the same thing.
			if (gen_is_function_pointer(g, v.type)) return v;
			if (!gen_is_scalar_pointer(g, v.type)) gen_fail(g, "only a pointer can be dereferenced");
			const int target = gen_target_of(g, v.type);
			// A pointer that is still in memory need not be loaded at all: where
			// it points is remembered instead, and read when something asks.
			if (in_memory( &v) && !gen_is_array(g, v.type))
			{
				const address_source source = source_of( &v);
				v.kind = val_deref;
				v.address = source;
				v.bias = 0;
				v.type = target;
				return v;
			}
			gen_load(g, &v);
			make_deref( &v, target);
			return v;
		}
		if (k == tok_plus)
		{
			gen_advance(g);
			return gen_unary_expression(g);
		}
		if (k == tok_minus)
		{
			gen_advance(g);
			value v = gen_unary_expression(g);
			if (v.kind == val_const)
			{
				v.constant = -v.constant;
				v.real = -v.real;
				return v;
			}
			const int wide = gen_is_wide(g, v.type);
			gen_load(g, &v);
			if (wide) emit_negate_dx_ax(&g->e_);
			else emit_neg_ax(&g->e_);
			return v;
		}
		if (k == tok_tilde)
		{
			gen_advance(g);
			value v = gen_unary_expression(g);
			if (v.kind == val_const)
			{
				v.constant = ~v.constant;
				return v;
			}
			const int wide = gen_is_wide(g, v.type);
			gen_load(g, &v);
			emit_not_ax(&g->e_);
			if (wide) emit_not_dx(&g->e_);
			return v;
		}
		if (k == tok_bang)
		{
			gen_advance(g);
			value v = gen_unary_expression(g);
			if (v.kind == val_const)
			{
				v.constant = v.constant == 0 ? 1 : 0;
				return v;
			}
			if (v.kind == val_flags)
			{
				v.offset = (int)(opposite((condition)(v.offset)));
				return v;
			}
			const int wide = gen_is_wide(g, v.type);
			gen_load(g, &v);
			if (wide) emit_or_ax_dx(&g->e_);
			else emit_test_ax_ax(&g->e_);
			v.kind = val_flags;
			v.offset = (int)(cond_equal);
			v.type = g->t_int_;
			return v;
		}
		if (k == tok_inc || k == tok_dec)
		{
			const int increment = k == tok_inc;
			gen_advance(g);
			value v = gen_unary_expression(g);
			const int step = gen_is_scalar_pointer(g, v.type) ? gen_type_size(g, gen_target_of(g, v.type)) : 1;
			if (gen_is_wide(g, v.type))
			{
				const value target = v;
				gen_load(g, &v);
				if (increment)
				{
					emit_add_ax_imm(&g->e_, step);
					emit_adc_dx_imm(&g->e_, 0);
				}
				else
				{
					emit_sub_ax_imm(&g->e_, step);
					emit_sbb_dx_zero(&g->e_);
				}
				if (target.kind == val_deref) gen_store_indirect(g, target.type, target.bias);
				else gen_store(g, &target);
				v.kind = val_reg;
				return v;
			}
			if (v.kind == val_deref)
			{
				// Loading through BX leaves the address there, so the store
				// that follows needs nothing on the stack.
				const value target = v;
				gen_load(g, &v);
				if (increment) emit_add_ax_imm(&g->e_, step);
				else emit_sub_ax_imm(&g->e_, step);
				gen_store_indirect(g, target.type, target.bias);
				v.kind = val_reg;
				return v;
			}
			const value target = v;
			gen_load(g, &v);
			if (increment) emit_add_ax_imm(&g->e_, step);
			else emit_sub_ax_imm(&g->e_, step);
			gen_store(g, &target);
			v.kind = val_reg;
			return v;
		}
		if (k == tok_kw_sizeof)
		{
			gen_advance(g);
			int measured;
			if (gen_kind(g) == tok_l_paren && gen_is_type_start(g, 1))
			{
				gen_advance(g);
				measured = gen_parse_type_specifier(g);
				gen_skip_qualifiers(g);
				while (gen_accept(g, tok_star))
				{
					measured = gen_pointer_to(g, measured);
					gen_skip_qualifiers(g);
				}
				if (gen_accept(g, tok_l_bracket))
				{
					const long count = gen_constant_expression(g);
					gen_expect(g, tok_r_bracket, "expected ]");
					measured = gen_array_of(g, measured, (int)(count));
				}
				gen_expect(g, tok_r_paren, "expected )");
			}
			else
			{
				const emission_mark start = gen_mark(g);
				const value v = gen_unary_expression(g);
				measured = v.type;
				gen_discard(g, start);
			}
			value v;
			v.kind = val_const;
			v.type = g->t_uint_;
			v.constant = gen_type_size(g, measured);
			return v;
		}

		// A cast. On a 16-bit target it only changes how the value is read, save
		// for the widening or narrowing between 16 and 32 bits.
		if (k == tok_l_paren && gen_is_type_start(g, 1))
		{
			gen_advance(g);
			int cast = gen_parse_type_specifier(g);
			gen_skip_qualifiers(g);
			while (gen_accept(g, tok_star))
			{
				cast = gen_pointer_to(g, cast);
				gen_skip_qualifiers(g);
			}
			gen_expect(g, tok_r_paren, "expected )");
			value v = gen_unary_expression(g);
			if (v.kind == val_const)
			{
				// A real only exists at compile time, so a cast is where it
				// turns into the integer the rest of the compiler can use.
				if (gen_is_real(g, v.type) && !gen_is_real(g, cast)) v.constant = (long)(v.real);
				else if (gen_is_real(g, cast)) v.real = double(v.constant);
				if (!gen_is_wide(g, cast) && !gen_is_real(g, cast)) v.constant = (long)(int16_t(v.constant));
				v.type = cast;
				return v;
			}
			gen_load_converted(g, &v, cast);
			return v;
		}

		return gen_postfix_expression(g);
	}

static value gen_binary_expression(generator* g,int precedence)
	{
		value left = gen_unary_expression(g);

		for (;;)
		{
			const cc_tok op = gen_kind(g);
			const int level = precedence_of(op);
			if (level == 0 || level <= precedence) return left;
			gen_advance(g);

			if (op == tok_and_and || op == tok_or_or)
			{
				const int shortcut = emit_label(&g->e_);
				const int done = emit_label(&g->e_);
				const condition first = gen_test_value(g, &left);
				if (op == tok_and_and) emit_jump_unless(&g->e_, first, shortcut);
				else emit_jump_if(&g->e_, first, shortcut);
				value right = gen_binary_expression(g, level);
				emit_set_from_condition(&g->e_, gen_test_value(g, &right));
				emit_jump(&g->e_, done);
				emit_bind(&g->e_, shortcut);
				emit_mov_ax_imm(&g->e_, op == tok_and_and ? 0 : 1);
				emit_bind(&g->e_, done);
				left.kind = val_reg;
				left.type = g->t_int_;
				continue;
			}

			// Both operands may still turn out to be constant, but the right
			// one is only known once it has been parsed. The code generated on
			// the way there is thrown away, which is what lets an expression
			// like 16*FRACUNIT or sizeof(a)/sizeof(a[0]) reach a case label or
			// an array bound.
			const int left_constant = left.kind == val_const && !gen_is_scalar_pointer(g, left.type);
			const long left_constant_value = left.constant;
			const double left_constant_real = left.real;
			const int left_constant_type = left.type;
			const emission_mark start = gen_mark(g);

			const int left_pointer = gen_is_scalar_pointer(g, left.type);
			const int element = left_pointer ? gen_type_size(g, gen_target_of(g, left.type)) : 1;
			const int left_type = left_pointer ? gen_pointer_to(g, gen_target_of(g, left.type)) : left.type;
			const int left_wide = !left_pointer && gen_is_wide(g, left.type);

			// A real has no register to be loaded into, so the speculative load
			// has to be skipped; if the right side then turns out not to be
			// constant, the fold below cannot happen and that is the error.
			const int left_real = left_constant && gen_is_real(g, left.type);

			// Neither operand has to go through the stack if one of them can be
			// put wherever the operator wants it at the end. The right one is
			// parsed first when a glance at the tokens says it will generate
			// nothing; failing that, a left operand that is a constant or sits
			// in memory can wait in place while the right one is generated.
			value right;
			int right_settled = 0;
			if (!left_real && gen_single_operand_ahead(g, level))
			{
				const size_t saved = g->at_;
				right = gen_binary_expression(g, level);
				right_settled = !gen_emitted_since(g, start) && gen_settled(g, &right);
				if (!right_settled)
				{
					gen_discard(g, start);
					g->at_ = saved;
				}
			}

			const int keep_left = !left_real && !left_wide && gen_fits_cx(g, &left);
			int pushed = 0;
			if (!right_settled)
			{
				if (!left_real && !keep_left)
				{
					gen_load(g, &left);
					if (left_wide) emit_push_dx(&g->e_);
					emit_push_ax(&g->e_);
					pushed = 1;
				}
				right = gen_binary_expression(g, level);
			}

			if (left_real && (right.kind != val_const || gen_is_scalar_pointer(g, right.type)))
				gen_fail(g, "floating point is only available in a constant expression");

			if (left_constant && right.kind == val_const && !gen_is_scalar_pointer(g, right.type))
			{
				gen_discard(g, start);

				// A real folds in double and stays one until something casts
				// it, which is the only way it can reach the code generator.
				if (gen_is_real(g, left_constant_type) || gen_is_real(g, right.type))
				{
					const double x = gen_is_real(g, left_constant_type)
						                 ? left_constant_real
						                 : double(left_constant_value);
					const double y = gen_is_real(g, right.type) ? right.real : double(right.constant);
					int comparison = 1;
					long verdict = 0;
					double folded = 0;
					switch (op)
					{
					case tok_plus: folded = x + y; comparison = 0; break;
					case tok_minus: folded = x - y; comparison = 0; break;
					case tok_star: folded = x * y; comparison = 0; break;
					case tok_slash:
						folded = y != 0 ? x / y : 0;
						comparison = 0;
						break;
					case tok_eq: verdict = x == y; break;
					case tok_ne: verdict = x != y; break;
					case tok_less: verdict = x < y; break;
					case tok_le: verdict = x <= y; break;
					case tok_greater: verdict = x > y; break;
					case tok_ge: verdict = x >= y; break;
					default: gen_fail(g, "that operator needs whole numbers");
					}
					left.kind = val_const;
					if (comparison)
					{
						left.type = g->t_int_;
						left.constant = verdict;
						left.real = double(verdict);
					}
					else
					{
						left.type = g->t_real_;
						left.real = folded;
						left.constant = (long)(folded);
					}
					continue;
				}

				const long a = left_constant_value;
				const long b = right.constant;
				const int either_unsigned = gen_is_unsigned(g, left_constant_type) || gen_is_unsigned(g, right.type);
				const unsigned long ua = static_cast<unsigned long>(a);
				const unsigned long ub = static_cast<unsigned long>(b);
				long result = 0;
				switch (op)
				{
				case tok_plus: result = a + b; break;
				case tok_minus: result = a - b; break;
				case tok_star: result = a * b; break;
				case tok_slash:
					if (b == 0) result = 0;
					else result = either_unsigned ? (long)(ua / ub) : a / b;
					break;
				case tok_percent:
					if (b == 0) result = 0;
					else result = either_unsigned ? (long)(ua % ub) : a % b;
					break;
				case tok_amp: result = a & b; break;
				case tok_pipe: result = a | b; break;
				case tok_caret: result = a ^ b; break;
				case tok_shl: result = (long)(ua << (ub & 31)); break;
				case tok_shr: result = either_unsigned ? (long)(ua >> (ub & 31)) : a >> (b & 31); break;
				case tok_eq: result = a == b; break;
				case tok_ne: result = a != b; break;
				case tok_less: result = either_unsigned ? ua < ub : a < b; break;
				case tok_le: result = either_unsigned ? ua <= ub : a <= b; break;
				case tok_greater: result = either_unsigned ? ua > ub : a > b; break;
				default: result = either_unsigned ? ua >= ub : a >= b; break;
				}

				left.kind = val_const;
				left.constant = result;
				if (level == 6 || level == 7) left.type = g->t_int_;
				else if (gen_is_wide(g, left_constant_type) || gen_is_wide(g, right.type))
					left.type = either_unsigned ? g->t_ulong_ : g->t_long_;
				else if (result > 65535 || result < -32768) left.type = either_unsigned ? g->t_ulong_ : g->t_long_;
				else left.type = either_unsigned ? g->t_uint_ : g->t_int_;
				continue;
			}

			const int right_pointer = gen_is_scalar_pointer(g, right.type);

			// A pointer is 16-bit, so an index that happens to be long is
			// simply truncated rather than dragging the whole operation wide.
			const int wide = !left_pointer && !right_pointer && (left_wide || gen_is_wide(g, right.type));
			const int unsigned_operation = gen_is_unsigned(g, left.type) || gen_is_scalar_pointer(g, left.type) ||
				gen_is_unsigned(g, right.type) || gen_is_scalar_pointer(g, right.type);
			const int scaled = left_pointer && !right_pointer && element != 1 &&
				(op == tok_plus || op == tok_minus);

			// The right operand can be placed last, and a constant then folds
			// into the instruction itself.
			const int place_right = right_settled && !pushed &&
				(!scaled || right.kind == val_const) &&
				(wide ? gen_fits_cx_bx(g, &right) : gen_fits_cx(g, &right));

			if (place_right)
			{
				if (wide)
				{
					long constant = right.constant;
					if (!gen_is_wide(g, right.type) && !gen_is_unsigned(g, right.type))
						constant = (long)(int16_t(constant));

					// A shift of 16 or more throws the low half away, so it is
					// never loaded. This is the fixed-point idiom.
					const int high_only = left_wide && in_memory( &left) &&
						right.kind == val_const && op == tok_shr &&
						(constant & 31) >= 16;
					if (high_only)
					{
						if (left.kind == val_local) emit_load_ax_frame(&g->e_, left.offset + 2);
						else
						{
							emit_load_ax_absolute(&g->e_, 0);
							gen_data_reference(g, left.offset + 2);
						}
						if (unsigned_operation) emit_xor_dx_dx(&g->e_);
						else emit_cwd(&g->e_);
						gen_shift_ax_constant(g, tok_shr, (int)(constant & 31) - 16, unsigned_operation);
						left.kind = val_reg;
					}
					else
					{
						gen_load(g, &left);
						if (!left_wide) gen_widen(g, left.type);
						if (right.kind != val_const ||
							!gen_apply_operator_wide_constant(g, op, constant, unsigned_operation))
						{
							gen_load_to_cx_bx(g, &right);
							gen_apply_operator_wide(g, op, unsigned_operation);
						}
					}
				}
				else
				{
					gen_load(g, &left);
					if (right.kind == val_const)
					{
						if (scaled) right.constant *= element;
						if (!gen_apply_operator_constant(g, op, right.constant, unsigned_operation))
						{
							gen_load_to_cx(g, &right);
							gen_apply_operator(g, op, unsigned_operation);
						}
					}
					else if (op == tok_plus && gen_add_address_to_ax(g, &right))
					{
					}
					else if (!gen_operate_from_memory(g, op, &right))
					{
						gen_load_to_cx(g, &right);
						gen_apply_operator(g, op, unsigned_operation);
					}
				}
			}
			else if (keep_left)
			{
				// The left operand stayed where it was, so the right one is in
				// AX and the operands are the wrong way round.
				gen_load(g, &right);
				if (wide)
				{
					if (!gen_is_wide(g, right.type)) gen_widen(g, right.type);
					emit_mov_cx_dx(&g->e_);
					emit_mov_bx_ax(&g->e_);
					gen_load(g, &left);
					gen_widen(g, left.type); // a wide left would have been pushed
					gen_apply_operator_wide(g, op, unsigned_operation);
				}
				else
				{
					if (scaled) gen_scale_ax(g, element);
					gen_load_to_cx(g, &left);
					gen_apply_operator(g, op, unsigned_operation, 1);
				}
			}
			else
			{
				if (!pushed && !left_real)
				{
					gen_load(g, &left);
					if (left_wide) emit_push_dx(&g->e_);
					emit_push_ax(&g->e_);
				}
				gen_load(g, &right);
				if (scaled) gen_scale_ax(g, element);

				if (wide)
				{
					if (!gen_is_wide(g, right.type)) gen_widen(g, right.type);
					emit_mov_cx_dx(&g->e_);
					emit_mov_bx_ax(&g->e_);
					emit_pop_ax(&g->e_);
					if (left_wide) emit_pop_dx(&g->e_);
					else gen_widen(g, left.type);
					gen_apply_operator_wide(g, op, unsigned_operation);
				}
				else
				{
					emit_mov_cx_ax(&g->e_);
					emit_pop_ax(&g->e_);
					if (left_wide) emit_pop_dx(&g->e_); // the left operand was pushed as a pair
					gen_apply_operator(g, op, unsigned_operation);
				}
			}

			value result;
			result.kind = val_reg;
			result.type = g->t_int_;
			if (is_comparison(op))
			{
				// The comparison is only in the flags. Whoever wants it as a
				// number will ask, and a branch will not have to.
				result.kind = val_flags;
				result.offset = (int)(condition_of(op, wide ? 0 : unsigned_operation));
			}
			else if (left_pointer && right_pointer && op == tok_minus)
			{
				if (element != 1)
				{
					emit_mov_cx_imm(&g->e_, element);
					emit_idiv_cx(&g->e_);
				}
			}
			else if (left_pointer && (op == tok_plus || op == tok_minus))
			{
				result.type = left_type;
			}
			else if (wide)
			{
				// Every 32-bit operator keeps the width, and a shift keeps the
				// type of what was shifted.
				if (op == tok_shl || op == tok_shr) result.type = left.type;
				else result.type = unsigned_operation ? g->t_ulong_ : g->t_long_;
			}
			else if (precedence_of(op) >= 8)
			{
				result.type = left.type;
			}
			left = result;
		}
	}

static value gen_conditional_expression(generator* g)
	{
		value v = gen_binary_expression(g, 0);
		if (gen_kind(g) != tok_question) return v;
		gen_advance(g);

		// A condition that is already known picks a branch here, so the whole
		// expression can still be a constant. Both arms are parsed either way.
		if (v.kind == val_const)
		{
			const int taken = v.constant != 0;
			emission_mark start = gen_mark(g);
			value then_value = gen_expression(g);
			if (!taken) gen_discard(g, start);
			gen_expect(g, tok_colon, "expected : in a conditional expression");
			start = gen_mark(g);
			value else_value = gen_conditional_expression(g);
			if (taken) gen_discard(g, start);
			value chosen = taken ? then_value : else_value;
			if (chosen.kind != val_const) gen_load(g, &chosen);
			return chosen;
		}

		const int otherwise = emit_label(&g->e_);
		const int done = emit_label(&g->e_);
		emit_jump_unless(&g->e_, gen_test_value(g, &v), otherwise);
		// The middle arm is a full expression, so a comma belongs to it rather
		// than ending the argument or declarator the conditional sits in.
		value then_value = gen_expression(g);
		gen_load(g, &then_value);
		emit_jump(&g->e_, done);
		gen_expect(g, tok_colon, "expected : in a conditional expression");
		emit_bind(&g->e_, otherwise);
		value else_value = gen_conditional_expression(g);
		gen_load_converted(g, &else_value, then_value.type);
		emit_bind(&g->e_, done);

		value result;
		result.kind = val_reg;
		result.type = then_value.type;
		return result;
	}

static value gen_assignment_expression(generator* g)
	{
		value target = gen_conditional_expression(g);
		const cc_tok op = gen_kind(g);

		const int simple = op == tok_assign;
		const int compound = op == tok_plus_assign || op == tok_minus_assign || op == tok_star_assign ||
			op == tok_slash_assign || op == tok_percent_assign || op == tok_amp_assign ||
			op == tok_pipe_assign || op == tok_caret_assign || op == tok_shl_assign ||
			op == tok_shr_assign;
		if (!simple && !compound) return target;

		if (target.kind != val_local && target.kind != val_global && target.kind != val_deref)
			gen_fail(g, "cannot assign to this expression");
		gen_advance(g);

		if (simple)
		{
			if (target.kind == val_deref && target.address == addr_reg) emit_push_ax(&g->e_);
			value source = gen_assignment_expression(g);
			gen_load_converted(g, &source, target.type);
			gen_store(g, &target);
			value result;
			result.kind = val_reg;
			result.type = target.type;
			return result;
		}

		static const struct
		{
			cc_tok assign;
			cc_tok binary;
		} mapping[] = {
			{tok_plus_assign, tok_plus}, {tok_minus_assign, tok_minus},
			{tok_star_assign, tok_star}, {tok_slash_assign, tok_slash},
			{tok_percent_assign, tok_percent}, {tok_amp_assign, tok_amp},
			{tok_pipe_assign, tok_pipe}, {tok_caret_assign, tok_caret},
			{tok_shl_assign, tok_shl}, {tok_shr_assign, tok_shr},
		};
		cc_tok binary = tok_plus;
		for (const auto& entry : mapping) if (entry.assign == op) binary = entry.binary;

		const int pointer_target = gen_is_scalar_pointer(g, target.type);
		const int element = pointer_target ? gen_type_size(g, gen_target_of(g, target.type)) : 1;
		const int wide = !pointer_target && gen_is_wide(g, target.type);

		// The source can be placed after the target is in AX when it generates
		// nothing, which is what lets x += 1 be a single instruction.
		const emission_mark start = gen_mark(g);
		const size_t saved = g->at_;
		value source;
		int source_settled = 0;
		if (gen_single_operand_ahead(g, 0))
		{
			source = gen_assignment_expression(g);
			source_settled = !gen_emitted_since(g, start) && gen_settled(g, &source) &&
				(wide ? gen_fits_cx_bx(g, &source) : gen_fits_cx(g, &source)) &&
				(element == 1 || source.kind == val_const);
			if (!source_settled)
			{
				gen_discard(g, start);
				g->at_ = saved;
			}
		}

		if (target.kind == val_deref && target.address == addr_reg) emit_push_ax(&g->e_);
		value current = target;
		gen_load(g, &current);

		if (source_settled)
		{
			const int unsigned_operation = gen_is_unsigned(g, target.type) || gen_is_scalar_pointer(g, target.type) ||
				gen_is_unsigned(g, source.type);
			long constant = source.constant;
			if (source.kind == val_const)
			{
				if (!gen_is_wide(g, source.type) && !gen_is_unsigned(g, source.type))
					constant = (long)(int16_t(constant));
				if (pointer_target && element != 1 &&
					(binary == tok_plus || binary == tok_minus))
					constant *= element;
			}
			const int folded = source.kind == val_const &&
				(wide
					 ? gen_apply_operator_wide_constant(g, binary, constant, unsigned_operation)
					 : gen_apply_operator_constant(g, binary, constant, unsigned_operation));
			if (!folded)
			{
				if (source.kind == val_const) source.constant = constant;
				if (wide)
				{
					gen_load_to_cx_bx(g, &source);
					gen_apply_operator_wide(g, binary, unsigned_operation);
				}
				else
				{
					gen_load_to_cx(g, &source);
					gen_apply_operator(g, binary, unsigned_operation);
				}
			}
			gen_store(g, &target);

			value result;
			result.kind = val_reg;
			result.type = target.type;
			return result;
		}

		if (wide) emit_push_dx(&g->e_);
		emit_push_ax(&g->e_);
		source = gen_assignment_expression(g);
		gen_load(g, &source);
		if (pointer_target && element != 1 && (binary == tok_plus || binary == tok_minus))
			gen_scale_ax(g, element);
		if (wide)
		{
			if (!gen_is_wide(g, source.type)) gen_widen(g, source.type);
			emit_mov_cx_dx(&g->e_);
			emit_mov_bx_ax(&g->e_);
			emit_pop_ax(&g->e_);
			emit_pop_dx(&g->e_);
			gen_apply_operator_wide(g, binary, gen_is_unsigned(g, target.type) || gen_is_unsigned(g, source.type));
		}
		else
		{
			emit_mov_cx_ax(&g->e_);
			emit_pop_ax(&g->e_);
			gen_apply_operator(g, binary, gen_is_unsigned(g, target.type) || gen_is_unsigned(g, source.type));
		}
		gen_store(g, &target);

		value result;
		result.kind = val_reg;
		result.type = target.type;
		return result;
	}

static value gen_expression(generator* g)
	{
		value v = gen_assignment_expression(g);
		while (gen_kind(g) == tok_comma)
		{
			gen_advance(g);
			v = gen_assignment_expression(g);
		}
		return v;
	}

static long gen_constant_expression(generator* g)
	{
		const emission_mark start = gen_mark(g);
		const value v = gen_conditional_expression(g);
		gen_discard(g, start);
		if (v.kind != val_const) gen_fail(g, "expected a constant expression");
		return v.constant;
	}

	// -----------------------------------------------------------------------
	// Declarations
	// -----------------------------------------------------------------------

static int gen_is_type_start(generator* g,int ahead)
	{
		switch ((*gen_peek(g, ahead)).kind)
		{
		case tok_kw_void: case tok_kw_char: case tok_kw_int: case tok_kw_unsigned:
		case tok_kw_signed: case tok_kw_short: case tok_kw_long: case tok_kw_struct:
		case tok_kw_union: case tok_kw_enum: case tok_kw_const: case tok_kw_qualifier:
		case tok_kw_static: case tok_kw_extern: case tok_kw_typedef:
			return 1;
		case tok_identifier:
		{
			const int index = gen_find(g, (*gen_peek(g, ahead)).text);
			return index >= 0 && SYMBOL_AT(g, (int)(index)).kind == sym_typedef;
		}
		default:
			return 0;
		}
	}

static int gen_parse_record(generator* g,int is_union)
	{
		std::string name;
		if (gen_kind(g) == tok_identifier)
		{
			name = (*gen_peek(g, 0)).text;
			gen_advance(g);
		}

		// A named record that is already known and not being defined here.
		if (!name.empty() && gen_kind(g) != tok_l_brace)
		{
			for (size_t i = 0; i < g->records_.count; ++i)
			{
				if (RECORD_AT(g, i).name != name) continue;
				for (size_t t = 0; t < g->types_.count; ++t)
					if (TYPE_AT(g, t).kind == ty_record && TYPE_AT(g, t).record == (int)(i)) return (int)(t);
			}
		}

		// A tag declared earlier is completed here rather than shadowed. A
		// typedef made from the forward declaration has to end up seeing the
		// members, which is how nearly every C header introduces a structure
		// that refers to itself.
		int record_index = -1;
		int type_index = -1;
		if (!name.empty())
		{
			for (size_t i = 0; i < g->records_.count; ++i)
			{
				if (RECORD_AT(g, i).name != name || !RECORD_AT(g, i).members.empty()) continue;
				record_index = (int)(i);
				for (size_t t = 0; t < g->types_.count; ++t)
					if (TYPE_AT(g, t).kind == ty_record && TYPE_AT(g, t).record == record_index) type_index = (int)(t);
				break;
			}
		}

		if (record_index < 0)
		{
			g->records_.push_back(record_info());
			record_index = (int)(g->records_.count) - 1;
			RECORD_AT(g, (int)(record_index)).name = name;
			RECORD_AT(g, (int)(record_index)).is_union = is_union;

			type_info info;
			info.kind = ty_record;
			info.record = record_index;
			info.size = 0;
			type_index = gen_add_type(g, info);
		}

		if (!gen_accept(g, tok_l_brace)) return type_index;
		RECORD_AT(g, (int)(record_index)).is_union = is_union;

		int offset = 0;
		int largest = 0;
		while (!gen_accept(g, tok_r_brace))
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated structure");
			const int base = gen_parse_type_specifier(g);
			for (;;)
			{
				std::string member_name;
				int is_function = 0;
				std::vector<int> parameters;
				std::vector<std::string> parameter_names;
				const int member_type = gen_parse_declarator(g, base, &member_name, &is_function, &parameters, &parameter_names);
				if (member_name.empty()) gen_fail(g, "expected a member name");

				record_member member;
				member.name = member_name;
				member.type = member_type;
				member.offset = is_union ? 0 : offset;
				RECORD_AT(g, (int)(record_index)).members.push_back(member);

				const int size = gen_type_size(g, member_type);
				if (is_union)
				{
					if (size > largest) largest = size;
				}
				else
				{
					// Members are packed to the byte. An 8086 has no alignment
					// requirement, and a structure read straight off disk - a
					// WAD lump, a directory entry - has to match the file.
					offset += size;
				}
				if (!gen_accept(g, tok_comma)) break;
			}
			gen_expect(g, tok_semicolon, "expected ; after a member");
		}

		RECORD_AT(g, (int)(record_index)).size = is_union ? largest : offset;
		TYPE_AT(g, (int)(type_index)).size = RECORD_AT(g, (int)(record_index)).size;
		return type_index;
	}

static int gen_parse_enum(generator* g)
	{
		if (gen_kind(g) == tok_identifier) gen_advance(g);
		if (gen_accept(g, tok_l_brace))
		{
			long next = 0;
			while (!gen_accept(g, tok_r_brace))
			{
				if (gen_kind(g) != tok_identifier) gen_fail(g, "expected an enumeration name");
				const std::string name = (*gen_peek(g, 0)).text;
				gen_advance(g);
				if (gen_accept(g, tok_assign)) next = gen_constant_expression(g);
				const int index = gen_define(g, name, sym_constant, g->t_int_);
				SYMBOL_AT(g, (int)(index)).value = next;
				++next;
				if (!gen_accept(g, tok_comma) && gen_kind(g) != tok_r_brace) gen_fail(g, "expected , in an enumeration");
			}
		}
		return g->t_int_;
	}

static int gen_parse_type_specifier(generator* g)
	{
		int is_unsigned_type = 0;
		int seen_sign = 0;
		gen_skip_qualifiers(g);

		if (gen_kind(g) == tok_kw_unsigned)
		{
			gen_advance(g);
			is_unsigned_type = 1;
			seen_sign = 1;
		}
		else if (gen_kind(g) == tok_kw_signed)
		{
			gen_advance(g);
			seen_sign = 1;
		}

		gen_skip_qualifiers(g);

		switch (gen_kind(g))
		{
		case tok_kw_void:
			gen_advance(g);
			return g->t_void_;
		case tok_kw_char:
			gen_advance(g);
			return is_unsigned_type ? g->t_uchar_ : g->t_char_;
		case tok_kw_short:
			gen_advance(g);
			if (gen_kind(g) == tok_kw_int) gen_advance(g);
			return is_unsigned_type ? g->t_uint_ : g->t_int_;
		case tok_kw_long:
			gen_advance(g);
			while (gen_kind(g) == tok_kw_long) gen_advance(g); // long long is 32-bit here too
			if (gen_kind(g) == tok_kw_int) gen_advance(g);
			return is_unsigned_type ? g->t_ulong_ : g->t_long_;
		case tok_kw_int:
			gen_advance(g);
			return is_unsigned_type ? g->t_uint_ : g->t_int_;
		case tok_kw_struct:
			gen_advance(g);
			return gen_parse_record(g, 0);
		case tok_kw_union:
			gen_advance(g);
			return gen_parse_record(g, 1);
		case tok_kw_enum:
			gen_advance(g);
			return gen_parse_enum(g);
		case tok_identifier:
		{
			const int index = gen_find(g, (*gen_peek(g, 0)).text);
			if (index >= 0 && SYMBOL_AT(g, (int)(index)).kind == sym_typedef)
			{
				gen_advance(g);
				return SYMBOL_AT(g, (int)(index)).type;
			}
			break;
		}
		default:
			break;
		}

		if (seen_sign) return is_unsigned_type ? g->t_uint_ : g->t_int_;
		gen_fail(g, "expected a type");
	}

static int gen_parse_declarator(generator* g,int base, std::string* name, int* is_function, std::vector<int>* parameters, std::vector<std::string>* parameter_names)
	{
		gen_skip_qualifiers(g);
		while (gen_accept(g, tok_star))
		{
			base = gen_pointer_to(g, base);
			gen_skip_qualifiers(g);
		}

		// A parenthesised declarator, as in int (*f)(int). The group binds to
		// whatever the suffix after it builds, so skip it, build that, then
		// come back and apply the group to it.
		if (gen_kind(g) == tok_l_paren && ((*gen_peek(g, 1)).kind == tok_star || (*gen_peek(g, 1)).kind == tok_l_paren))
		{
			const size_t group = g->at_;
			int depth = 0;
			do
			{
				if (gen_kind(g) == tok_end) gen_fail(g, "unterminated declarator");
				if (gen_kind(g) == tok_l_paren) ++depth;
				else if (gen_kind(g) == tok_r_paren) --depth;
				gen_advance(g);
			}
			while (depth > 0);

			const int inner_base = gen_parse_declarator_suffix(g, base, &is_function, &parameters, &parameter_names);
			const size_t after = g->at_;

			g->at_ = group + 1;
			int nested = 0;
			std::vector<int> nested_parameters;
			std::vector<std::string> nested_names;
			const int declared = gen_parse_declarator(g, inner_base, &name, &nested, &nested_parameters, &nested_names);
			gen_expect(g, tok_r_paren, "expected ) in a declarator");
			g->at_ = after;
			is_function = 0; // the parentheses belonged to the pointer, not to a definition
			return declared;
		}

		if (gen_kind(g) == tok_identifier)
		{
			name = (*gen_peek(g, 0)).text;
			gen_advance(g);
		}

		return gen_parse_declarator_suffix(g, base, &is_function, &parameters, &parameter_names);
	}

static int gen_parse_declarator_suffix(generator* g,int base, int* is_function, std::vector<int>* parameters, std::vector<std::string>* parameter_names)
	{
		if (gen_kind(g) == tok_l_paren)
		{
			gen_advance(g);
			is_function = 1;
			if (gen_kind(g) == tok_kw_void && (*gen_peek(g, 1)).kind == tok_r_paren) gen_advance(g);
			while (gen_kind(g) != tok_r_paren)
			{
				if (gen_kind(g) == tok_end) gen_fail(g, "unterminated parameter list");
				// The extra arguments are found through __args, so the ellipsis
				// itself needs nothing beyond being allowed here.
				if (gen_accept(g, tok_ellipsis)) break;
				const int parameter_base = gen_parse_type_specifier(g);
				std::string parameter_name;
				int nested = 0;
				std::vector<int> nested_parameters;
				std::vector<std::string> nested_names;
				int parameter_type = gen_parse_declarator(g, parameter_base, &parameter_name, &nested, &nested_parameters, &nested_names);
				// A parameter declared as an array or a function is really a pointer.
				if (gen_is_array(g, parameter_type)) parameter_type = gen_pointer_to(g, gen_target_of(g, parameter_type));
				else if (gen_is_function_type(g, parameter_type)) parameter_type = gen_pointer_to(g, parameter_type);
				parameters->push_back(parameter_type);
				parameter_names->push_back(parameter_name);
				if (!gen_accept(g, tok_comma)) break;
			}
			gen_expect(g, tok_r_paren, "expected ) after the parameter list");
			return gen_function_type(g, base, parameters);
		}

		std::vector<int> dimensions;
		while (gen_accept(g, tok_l_bracket))
		{
			if (gen_kind(g) == tok_r_bracket) dimensions.push_back(0);
			else dimensions.push_back((int)(gen_constant_expression(g)));
			gen_expect(g, tok_r_bracket, "expected ]");
		}
		for (size_t i = dimensions.size(); i > 0; --i) base = gen_array_of(g, base, dimensions[i - 1]);
		return base;
	}

	// The length of an array declared without one, counted from the initialiser
	// that follows. Only the outermost dimension can be left open, so nothing
	// below the first level of braces is counted.
static int gen_initialiser_length(generator* g)
	{
		if ((*gen_peek(g, 1)).kind == tok_string) return (*gen_peek(g, 1)).length + 1;
		if ((*gen_peek(g, 1)).kind != tok_l_brace) return 1;

		int depth = 0;
		int count = 0;
		int pending = 0;
		for (size_t scan = g->at_ + 1; scan < (size_t)(g->tokens_.tokens.count); ++scan)
		{
			const cc_tok k = CC_TOKEN(g->tokens_, (int)(scan)).kind;
			if (k == tok_end) break;
			if (k == tok_l_brace || k == tok_l_paren || k == tok_l_bracket)
			{
				++depth;
				if (depth > 1) pending = 1;
				continue;
			}
			if (k == tok_r_brace || k == tok_r_paren || k == tok_r_bracket)
			{
				if (--depth == 0) break;
				continue;
			}
			if (depth == 1 && k == tok_comma)
			{
				if (pending) ++count;
				pending = 0;
				continue;
			}
			pending = 1;
		}
		if (pending) ++count;
		return count > 0 ? count : 1;
	}

	// A constant address: an object, optionally cast, indexed or offset by a
	// constant number of elements. It cannot go through the ordinary constant
	// folder because the address is only known once the data section is placed.
static int gen_address_operand(generator* g, int* symbol, int* pointee, long* offset)
	{
		if (gen_kind(g) == tok_l_paren && gen_is_type_start(g, 1))
		{
			gen_advance(g);
			int cast = gen_parse_type_specifier(g);
			gen_skip_qualifiers(g);
			while (gen_accept(g, tok_star))
			{
				cast = gen_pointer_to(g, cast);
				gen_skip_qualifiers(g);
			}
			if (!gen_accept(g, tok_r_paren)) return 0;
			if (!gen_address_operand(g, &symbol, &pointee, &offset)) return 0;
			// The cast is what a following +1 counts in.
			pointee = gen_is_scalar_pointer(g, cast) ? gen_target_of(g, cast) : cast;
			return 1;
		}
		if (gen_kind(g) == tok_l_paren)
		{
			gen_advance(g);
			if (!gen_address_operand(g, &symbol, &pointee, &offset)) return 0;
			if (!gen_address_offset(g, pointee, &offset)) return 0;
			return gen_accept(g, tok_r_paren);
		}

		const int address_of = gen_accept(g, tok_amp);
		if (gen_kind(g) != tok_identifier) return 0;
		const int index = gen_find(g, (*gen_peek(g, 0)).text);
		if (index < 0 || SYMBOL_AT(g, (int)(index)).kind != sym_global) return 0;
		gen_advance(g);
		symbol = index;

		int current = SYMBOL_AT(g, (int)(index)).type;
		for (;;)
		{
			if (gen_accept(g, tok_l_bracket))
			{
				if (!gen_is_scalar_pointer(g, current)) return 0;
				const int element = gen_target_of(g, current);
				const emission_mark start = gen_mark(g);
				const value subscript = gen_conditional_expression(g);
				gen_discard(g, start);
				if (subscript.kind != val_const || !gen_accept(g, tok_r_bracket)) return 0;
				offset += subscript.constant * gen_type_size(g, element);
				current = element;
				continue;
			}
			if (gen_kind(g) == tok_dot && (*gen_peek(g, 1)).kind == tok_identifier)
			{
				if (!gen_is_record(g, current)) return 0;
				const record_info& info = RECORD_AT(g, (int)((*gen_type(g, current)).record));
				const record_member* found = NULL;
				for (const record_member& m : info.members) if (m.name == (*gen_peek(g, 1)).text) found = &m;
				if (found == NULL) return 0;
				offset += found->offset;
				current = found->type;
				gen_advance(g);
				gen_advance(g);
				continue;
			}
			break;
		}

		// Only an array turns into an address on its own.
		if (!address_of && !gen_is_array(g, current)) return 0;
		pointee = gen_is_array(g, current) ? gen_target_of(g, current) : current;
		return 1;
	}

static int gen_address_offset(generator* g,int pointee, long* offset)
	{
		while (gen_kind(g) == tok_plus || gen_kind(g) == tok_minus)
		{
			const int add = gen_kind(g) == tok_plus;
			gen_advance(g);
			const emission_mark start = gen_mark(g);
			const value step = gen_binary_expression(g, precedence_of(tok_plus));
			gen_discard(g, start);
			if (step.kind != val_const) return 0;
			offset += (add ? 1 : -1) * step.constant * gen_type_size(g, pointee);
		}
		return 1;
	}

static int gen_address_constant(generator* g,int at)
	{
		const size_t saved = g->at_;
		int symbol = -1;
		int pointee = g->t_char_;
		long offset = 0;
		if (gen_address_operand(g, &symbol, &pointee, &offset) && gen_address_offset(g, pointee, &offset))
		{
			const cc_tok after = gen_kind(g);
			if (after == tok_comma || after == tok_r_brace || after == tok_semicolon)
			{
				gen_data_pointer(g, at, (int)(SYMBOL_AT(g, (int)(symbol)).offset + offset));
				return 1;
			}
		}
		g->at_ = saved;
		return 0;
	}

static void gen_global_initialiser(generator* g,int type_index, int offset)
	{
		const auto put = [&](int at, int width, long v)
		{
			if (at < 0 || (size_t)(at) + (size_t)(width) > g->data_.count) gen_fail(g, "the initialiser does not fit");
			for (int i = 0; i < width; ++i) DATA_AT(g, (int)(at) + (size_t)(i)) = (uint8_t)((v >> (8 * i)) & 0xff);
		};

		// A function name in an initialiser is its address, which is not known
		// until every body has been generated.
		const auto function_address = [&](int at) -> int
		{
			const int name = gen_kind(g) == tok_amp && (*gen_peek(g, 1)).kind == tok_identifier ? 1 : 0;
			if ((*gen_peek(g, name)).kind != tok_identifier) return 0;
			const int index = gen_find(g, (*gen_peek(g, name)).text);
			if (index < 0 || SYMBOL_AT(g, (int)(index)).kind != sym_function) return 0;
			if ((*gen_peek(g, name + 1)).kind == tok_l_paren) return 0;
			for (int i = 0; i <= name; ++i) gen_advance(g);
			g->function_fixups_.push_back({at, index});
			SYMBOL_AT(g, (int)(index)).called = 1;
			return 1;
		};

		// The address of another global, handled by address_constant so that a
		// cast, an index or a constant element offset all come out right.

		if (gen_kind(g) == tok_string && gen_is_array(g, type_index) && gen_type_size(g, gen_target_of(g, type_index)) == 1)
		{
			const std::string text((*gen_peek(g, 0)).text, (size_t)((*gen_peek(g, 0)).length));
			gen_advance(g);
			if (offset < 0 || (size_t)(offset) + text.size() > g->data_.count) gen_fail(g, "the initialiser does not fit");
			for (size_t i = 0; i < text.size() && (int)(i) < gen_type_size(g, type_index); ++i)
				g->data_[(size_t)(offset) + i] = (uint8_t)(text[i]);
			return;
		}

		// A braced list follows the shape of the type it initialises, so an
		// array of structures needs no special case beyond recursion. Data
		// starts out zero, which is what a short list should leave behind.
		if (gen_accept(g, tok_l_brace))
		{
			if (gen_is_array(g, type_index) || gen_is_record(g, type_index))
			{
				const int array = gen_is_array(g, type_index);
				const int element = array ? gen_target_of(g, type_index) : 0;
				const int stride = array ? gen_type_size(g, element) : 0;
				const int count = array ? (*gen_type(g, type_index)).count : 0;
				const int record = array ? -1 : (*gen_type(g, type_index)).record;
				int index = 0;
				while (!gen_accept(g, tok_r_brace))
				{
					if (gen_kind(g) == tok_end) gen_fail(g, "unterminated initialiser");
					if (array)
					{
						if (count > 0 && index >= count) gen_fail(g, "too many initialisers");
						gen_global_initialiser(g, element, offset + index * stride);
					}
					else
					{
						if (index >= (int)(RECORD_AT(g, (int)(record)).members.size()))
							gen_fail(g, "too many initialisers");
						const record_member member = RECORD_AT(g, (int)(record)).members[(size_t)(index)];
						gen_global_initialiser(g, member.type, offset + member.offset);
					}
					++index;
					if (!gen_accept(g, tok_comma) && gen_kind(g) != tok_r_brace) gen_fail(g, "expected , in an initialiser");
				}
				return;
			}

			// A braced scalar, which C allows and headers occasionally use.
			gen_global_initialiser(g, type_index, offset);
			gen_expect(g, tok_r_brace, "expected } in an initialiser");
			return;
		}

		// A string used to initialise a char pointer becomes its address, so
		// it needs a fixup once the data section is placed.
		if (gen_kind(g) == tok_string && gen_is_pointer(g, type_index))
		{
			const int text_offset = gen_add_string(g, std::string((*gen_peek(g, 0)).text, (size_t)((*gen_peek(g, 0)).length)));
			gen_advance(g);
			gen_data_pointer(g, offset, text_offset);
			return;
		}

		if (function_address(offset)) return;
		if (gen_address_constant(g, offset)) return;

		const long v = gen_constant_expression(g);
		put(offset, gen_type_size(g, type_index), v);
	}

static void gen_emit_return(generator* g)
	{
		// Room for the register restores, which are only known once the whole
		// body has been generated. A nop is no bytes at all if nothing needs it.
		g->epilogue_slots_.push_back(emit_at(&g->e_));
		emit_op(&g->e_, op_nop, 2, operand_none(), operand_none());
		emit_op(&g->e_, op_nop, 2, operand_none(), operand_none());
		emit_mov_sp_bp(&g->e_);
		emit_pop_bp(&g->e_);
		emit_ret(&g->e_);
	}

	// Where a variable lives, as an operand the back end can work on directly.
static operand gen_place_of(generator* g, const value* target, int extra)
	{
		if (target->kind == val_local) return emit_frame(target->offset + extra);
		return operand_absolute(0);
	}

	// An operation applied straight to a variable in memory.
static void gen_memory_operation(generator* g,op kind, int width, const value* target, operand source,
	                                 int extra)
	{
		emit_op(&g->e_, kind, width, gen_place_of(g, &target, extra), source);
		if (target->kind != val_local) gen_data_reference(g, target->offset + extra);
	}

	// The mirror of it: the second operand of an operator is read where it
	// lies rather than loaded into CX first. Only the operators with a
	// register/memory form qualify - a multiply, a divide and a shift all want
	// a particular register.
static int gen_operate_from_memory(generator* g,cc_tok what, const value* right)
	{
		if (right->kind != val_local && right->kind != val_global) return 0;
		if (gen_is_array(g, right->type) || gen_is_record(g, right->type)) return 0;

		op kind;
		switch (what)
		{
		case tok_plus: kind = op_add; break;
		case tok_minus: kind = op_sub; break;
		case tok_amp: kind = op_and_; break;
		case tok_pipe: kind = op_or_; break;
		case tok_caret: kind = op_xor_; break;
		default:
			if (!is_comparison(what)) return 0;
			kind = op_cmp;
			break;
		}

		emit_op(&g->e_, kind, 2, operand_in(reg_a), gen_place_of(g, &right));
		if (right->kind != val_local) gen_data_reference(g, right->offset);
		return 1;
	}

	// Register allocation, over a function's whole body. Deciding this in the
	// parsing pass is impossible - which slot is worth a register is only
	// known once the last statement has been read - so it is a pass over the
	// instruction list instead, which is what having one is for. The two
	// busiest frame slots move into SI and DI, the only registers nothing else
	// in the generated code claims.
static void gen_promote_locals(generator* g)
	{
		if (g->prologue_slots_ < 0) return;
		const int first = g->body_start_;
		const int last = emit_at(&g->e_);

		// A use inside a loop is worth many on a straight line, and from here
		// a loop is simply a branch back to somewhere already generated.
		std::vector<int> weight((size_t)(last - first), 1);
		for (int i = first; i < last; ++i)
		{
			const instruction& in = EMIT_CODE(g->e_, (int)(i));
			if ((in.code != op_jmp && in.code != op_jcc) || in.label < 0) continue;
			const int target = EMIT_LABEL(g->e_, (int)(in.label));
			if (target < first || target >= i) continue;
			for (int k = target; k <= i; ++k)
				if (weight[(size_t)(k - first)] < 512) weight[(size_t)(k - first)] *= 8;
		}

		std::map<int, int> uses;
		std::map<int, int> rejected;
		for (int i = first; i < last; ++i)
		{
			const instruction& in = EMIT_CODE(g->e_, (int)(i));
			// A run of hand-assembled bytes says nothing about what it touches.
			if (in.code == op_literal) return;
			for (const operand* o : {&in.dest, &in.source})
			{
				if (o->reg == reg_si || o->reg == reg_di || o->index == reg_si || o->index == reg_di)
					if (o->kind != operand_kind_immediate) return;
				if (o->kind != operand_kind_memory || o->reg != reg_bp || o->index != reg_none)
					continue;
				// An address that escapes reaches slots this pass cannot see,
				// so one of them anywhere in the body settles the question.
				if (in.code == op_lea) return;
				if (in.width != 2) rejected[o->value] = 1;
				else uses[o->value] += weight[(size_t)(i - first)];
			}
		}

		std::vector<std::pair<int, int>> ranked;
		for (const auto& entry : uses)
			if (!rejected[entry.first]) ranked.emplace_back(entry.second, entry.first);
		std::sort(ranked.begin(), ranked.end(), std::greater<std::pair<int, int>>());

		// A register costs one save, one restore per return and, for a
		// parameter, one load, so a slot has to be worth more than that.
		const int registers[2] = {reg_si, reg_di};
		int chosen[2] = {0, 0};
		int count = 0;
		for (const auto& entry : ranked)
		{
			if (count == 2 || entry.first < 4 + (int)(g->epilogue_slots_.count)) break;
			chosen[count++] = entry.second;
		}
		if (count == 0) return;

		for (int i = first; i < last; ++i)
		{
			instruction& in = EMIT_CODE(g->e_, (int)(i));
			for (operand* o : {&in.dest, &in.source})
			{
				if (o->kind != operand_kind_memory || o->reg != reg_bp) continue;
				for (int n = 0; n < count; ++n)
					if (o->value == chosen[n]) *o = operand_in(registers[n]);
			}
		}

		// The saved copies go below every local, so no offset already handed
		// out has to move.
		const int save[2] = {-(g->frame_ + 2), -(g->frame_ + 4)};
		g->frame_ += 2 * count;

		const auto put = [&](int at, const operand& dest, const operand& source)
		{
			instruction moved;
			moved.code = op_mov;
			moved.width = 2;
			moved.dest = dest;
			moved.source = source;
			EMIT_CODE(g->e_, (int)(at)) = moved;
		};

		for (int n = 0; n < count; ++n)
		{
			put(g->prologue_slots_ + n, emit_frame(save[n]), operand_in(registers[n]));
			// A parameter arrives on the stack, so it is fetched once.
			if (chosen[n] > 0)
				put(g->prologue_slots_ + 2 + n, operand_in(registers[n]), emit_frame(chosen[n]));
			for (const int at : g->epilogue_slots_)
				put(at + n, operand_in(registers[n]), emit_frame(save[n]));
		}

		gen_fold_address_copies(g, first, last);
	}

	// Where a register still matters, worked out backwards over a function.
	// The instruction list is already a control flow graph: a branch names a
	// label and a label names an instruction.
static std::vector<int> gen_live_registers(generator* g,int first, int last, int which)
	{
		const auto reads = [&](const instruction& in)
		{
			// A helper takes its second 32-bit operand in CX:BX and a DOS call
			// takes whatever the program put in the registers, so both count.
			if (in.code == op_call || in.code == op_call_at || in.code == op_interrupt)
				return 1;
			for (const operand* o : {&in.dest, &in.source})
			{
				if (o->kind == operand_kind_memory && (o->reg == which || o->index == which))
					return 1;
				if (o->kind != operand_kind_reg || o->reg != which) continue;
				if (o == &in.source) return 1;
				if (in.code != op_mov && in.code != op_lea && in.code != op_pop &&
					in.code != op_setcc)
					return 1;
			}
			return 0;
		};
		const auto writes = [&](const instruction& in)
		{
			return in.dest.kind == operand_kind_reg && in.dest.reg == which &&
				(in.code == op_mov || in.code == op_lea || in.code == op_pop ||
					in.code == op_setcc);
		};

		std::vector<int> live((size_t)(last - first) + 1, 0);
		for (int changed = 1; changed;)
		{
			changed = 0;
			for (int k = last - 1; k >= first; --k)
			{
				const instruction& in = EMIT_CODE(g->e_, (int)(k));
				int out = 0;
				if (in.code != op_ret)
				{
					if (in.code != op_jmp) out = live[(size_t)(k + 1 - first)];
					if ((in.code == op_jmp || in.code == op_jcc) && in.label >= 0)
					{
						const int target = EMIT_LABEL(g->e_, (int)(in.label));
						out = out || target < first || target >= last ||
							live[(size_t)(target - first)];
					}
				}
				const int now = reads(in) || (out && !writes(in));
				if (now == live[(size_t)(k - first)]) continue;
				live[(size_t)(k - first)] = now;
				changed = 1;
			}
		}
		return live;
	}

	// Promotion leaves a copy into BX in front of every access through a
	// pointer that now lives in a register, and SI and DI can address memory
	// themselves. Removing the copy needs to know that nothing wants BX
	// afterwards, which is a question about the instruction list rather than
	// about the parse - the reason for doing this here and not there.
static void gen_fold_address_copies(generator* g,int first, int last)
	{
		const std::vector<int> live = gen_live_registers(g, first, last, reg_b);
		std::vector<int> entered((size_t)(last - first) + 1, 0);
		for (int label = 0; label < g->e_.labels.count; ++label)
		{
			const int index = EMIT_LABEL(g->e_, label);
			if (index >= first && index < last) entered[(size_t)(index - first)] = 1;
		}

		for (int i = first; i + 1 < last; ++i)
		{
			instruction& copy = EMIT_CODE(g->e_, (int)(i));
			if (copy.code != op_mov || copy.width != 2) continue;
			if (copy.dest.kind != operand_kind_reg || copy.dest.reg != reg_b) continue;
			if (copy.source.kind != operand_kind_reg) continue;
			const int base = copy.source.reg;
			if (base != reg_si && base != reg_di) continue;

			std::vector<int> uses; // instruction index and side, packed
			int settled = 0, rejected = 0, base_gone = 0;
			for (int k = i + 1; k < last && !settled && !rejected; ++k)
			{
				const instruction& in = EMIT_CODE(g->e_, (int)(k));
				// Where control can arrive another way, or leave, the question
				// is not what the next instruction does but whether BX is
				// wanted at all from here on.
				if (entered[(size_t)(k - first)] || in.code == op_literal ||
					in.code == op_jmp || in.code == op_jcc || in.code == op_ret ||
					in.code == op_call || in.code == op_call_at || in.code == op_interrupt)
				{
					if (live[(size_t)(k - first)]) rejected = 1;
					settled = 1;
					break;
				}
				for (int side = 0; side < 2 && !rejected; ++side)
				{
					const operand& o = side == 0 ? in.dest : in.source;
					if (o.kind == operand_kind_reg && o.reg == reg_b)
					{
						if (side == 0 && in.code == op_mov) settled = 1;
						else rejected = 1;
					}
					else if (o.kind == operand_kind_memory && (o.reg == reg_b || o.index == reg_b))
					{
						// The copy is only worth removing if the register it
						// came from still holds the address at every use.
						if (o.index != reg_none || base_gone) rejected = 1;
						else uses.push_back(k * 2 + side);
					}
				}
				if (in.code == op_xchg) rejected = 1;
				if (in.dest.kind == operand_kind_reg && in.dest.reg == base &&
					in.code != op_cmp && in.code != op_test && in.code != op_push)
					base_gone = 1;
			}
			if (!settled || rejected || uses.empty()) continue;

			for (const int packed : uses)
			{
				instruction& in = EMIT_CODE(g->e_, (int)(packed / 2));
				operand& o = packed % 2 == 0 ? in.dest : in.source;
				o.reg = (uint8_t)(base);
			}
			copy.code = op_nop;
			copy.dest = {};
			copy.source = {};
		}
	}

	// A statement whose result nobody wants - i++, x += 8, p = p + 1 - is one
	// instruction against memory rather than a load, an operation and a store.
	// Recognised from the tokens, because that is the only place the fact that
	// the value is being thrown away is visible.
static int gen_in_place_statement(generator* g,cc_tok terminator)
	{
		const size_t start = g->at_;
		const auto token = [&](size_t ahead)
		{
			const size_t index = start + ahead;
			return index < (size_t)(g->tokens_.tokens.count) ? CC_TOKEN(g->tokens_, (int)(index)).kind : tok_end;
		};

		size_t name = start;
		size_t rest = 0; // where the source operand begins
		cc_tok op = tok_plus;
		int step_only = 0;
		int increment = 1;

		if ((token(0) == tok_inc || token(0) == tok_dec) && token(1) == tok_identifier &&
			token(2) == terminator)
		{
			increment = token(0) == tok_inc;
			name = start + 1;
			step_only = 1;
		}
		else if (token(0) == tok_identifier && (token(1) == tok_inc || token(1) == tok_dec) &&
			token(2) == terminator)
		{
			increment = token(1) == tok_inc;
			step_only = 1;
		}

		if (!step_only)
		{
			// NAME op= <one token> and NAME = NAME op <one token>
			static const struct
			{
				cc_tok assign;
				cc_tok binary;
			} compound[] = {
				{tok_plus_assign, tok_plus}, {tok_minus_assign, tok_minus},
				{tok_amp_assign, tok_amp}, {tok_pipe_assign, tok_pipe},
				{tok_caret_assign, tok_caret},
			};

			if (token(0) != tok_identifier) return 0;
			int found = 0;
			for (const auto& entry : compound)
			{
				if (token(1) != entry.assign) continue;
				op = entry.binary;
				rest = 2;
				found = 1;
			}
			if (!found)
			{
				if (token(1) != tok_assign || token(2) != tok_identifier) return 0;
				if (CC_TOKEN(g->tokens_, (int)(start)).name != CC_TOKEN(g->tokens_, (int)(start) + 2).name) return 0;
				const cc_tok binary = token(3);
				if (binary != tok_plus && binary != tok_minus && binary != tok_amp &&
					binary != tok_pipe && binary != tok_caret)
					return 0;
				op = binary;
				rest = 4;
			}
			if (token(rest + 1) != terminator) return 0;
			const cc_tok source_kind = token(rest);
			if (source_kind != tok_number && source_kind != tok_character &&
				source_kind != tok_identifier)
				return 0;
		}

		const int index = gen_find(g, CC_TOKEN(g->tokens_, (int)(name)).text);
		if (index < 0) return 0;
		const symbol& entry = SYMBOL_AT(g, (int)(index));
		if (entry.kind != sym_local && entry.kind != sym_global) return 0;
		if (gen_is_array(g, entry.type) || gen_is_record(g, entry.type) || gen_is_real(g, entry.type)) return 0;
		const int width = gen_type_size(g, entry.type);
		if (width != 1 && width != 2 && width != 4) return 0;

		value target;
		target.kind = entry.kind == sym_local ? val_local : val_global;
		target.type = entry.type;
		target.offset = entry.offset;

		long constant = 0;
		int constant_source = 1;
		value source;
		if (step_only)
		{
			constant = gen_is_scalar_pointer(g, entry.type) ? gen_type_size(g, gen_target_of(g, entry.type)) : 1;
			op = increment ? tok_plus : tok_minus;
		}
		else
		{
			// Reading the source must generate nothing, which the single-token
			// check above has already made near certain.
			const size_t saved = g->at_;
			g->at_ = start + rest;
			const emission_mark before = gen_mark(g);
			source = gen_unary_expression(g);
			const int clean = !gen_emitted_since(g, before) && gen_settled(g, &source);
			g->at_ = saved;
			if (!clean)
			{
				gen_discard(g, before);
				return 0;
			}
			constant_source = source.kind == val_const;
			if (constant_source)
			{
				constant = source.constant;
				if (!gen_is_wide(g, source.type) && !gen_is_unsigned(g, source.type))
					constant = (long)(int16_t(constant));
				if (gen_is_scalar_pointer(g, entry.type) && (op == tok_plus || op == tok_minus))
					constant *= gen_type_size(g, gen_target_of(g, entry.type));
			}
			else
			{
				// Only a source of the same width can be applied in place.
				if (!in_memory( &source) || gen_is_array(g, source.type) || gen_is_record(g, source.type)) return 0;
				if (gen_type_size(g, source.type) != width || width == 1) return 0;
			}
		}

		::op kind = op_add;
		switch (op)
		{
		case tok_plus: kind = op_add; break;
		case tok_minus: kind = op_sub; break;
		case tok_amp: kind = op_and_; break;
		case tok_pipe: kind = op_or_; break;
		default: kind = op_xor_; break;
		}
		const int wide = width == 4;

		// The carry between the halves only matters for + and -.
		::op high_kind = kind;
		if (op == tok_plus) high_kind = op_adc;
		else if (op == tok_minus) high_kind = op_sbb;

		if (constant_source)
		{
			const int low = (int)(constant & 0xffff);
			const int high = (int)((constant >> 16) & 0xffff);
			if (!wide && (op == tok_plus || op == tok_minus) && (low == 1 || low == 0xffff))
			{
				const int up = (op == tok_plus) == (low == 1);
				gen_memory_operation(g, up ? op_inc : op_dec, width, &target, {});
			}
			else
			{
				gen_memory_operation(g, kind, width, &target, operand_constant(low));
				if (wide) gen_memory_operation(g, high_kind, 2, &target, operand_constant(high), 2);
			}
		}
		else
		{
			gen_load(g, &source);
			gen_memory_operation(g, kind, wide ? 2 : width, &target, operand_in(reg_a));
			if (wide) gen_memory_operation(g, high_kind, 2, &target, operand_in(reg_d), 2);
		}

		g->at_ = start + (step_only ? 2 : rest + 1);
		return 1;
	}

static void gen_function_body(generator* g,int function_index, const std::vector<int>* parameters, const std::vector<std::string>* parameter_names)
	{
		emit_bind(&g->e_, gen_label_of(g, function_index));
		SYMBOL_AT(g, (int)(function_index)).defined = 1;
		SYMBOL_AT(g, (int)(function_index)).start = SYMBOL_AT(g, (int)(function_index)).label;
		SYMBOL_AT(g, (int)(function_index)).body = (int)(g->at_);
		SYMBOL_AT(g, (int)(function_index)).parameter_names = parameter_names;
		g->return_type_ = SYMBOL_AT(g, (int)(function_index)).type;
		g->current_function_ = function_index;

		emit_push_bp(&g->e_);
		emit_mov_bp_sp(&g->e_);
		emit_sub_sp_imm(&g->e_, 0);
		g->frame_patch_ = emit_refers_to_deferred(&g->e_);
		g->frame_ = 0;
		g->prologue_slots_ = emit_at(&g->e_);
		for (int i = 0; i < 4; ++i) emit_op(&g->e_, op_nop, 2, operand_none(), operand_none());
		g->epilogue_slots_.clear();
		g->body_start_ = emit_at(&g->e_);

		++g->scope_;
		int offset = g->target_.frame_base;
		for (size_t i = 0; i < parameters->size(); ++i)
		{
			const int width = gen_is_wide(g, parameters[i]) ? 4 : 2;
			if (!parameter_names[i].empty())
			{
				const int index = gen_define(g, parameter_names[i], sym_local, parameters[i]);
				SYMBOL_AT(g, (int)(index)).offset = offset;
			}
			offset += width;
		}

		gen_expect(g, tok_l_brace, "expected { to open the function body");
		g->labels_.clear();
		g->pending_gotos_.clear();
		while (!gen_accept(g, tok_r_brace))
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated function body");
			gen_statement(g);
		}

		for (const auto& entry : g->pending_gotos_)
		{
			const auto found = g->labels_.find(entry.first);
			if (found == g->labels_.end() || !emit_is_bound(&g->e_, found->second))
				gen_fail(g, ("undefined label: " + entry.first).c_str());
		}
		g->pending_gotos_.clear();

		gen_emit_return(g);
		gen_promote_locals(g);
		emit_set_deferred(&g->e_, g->frame_patch_, g->frame_);
		SYMBOL_AT(g, (int)(function_index)).frame = g->frame_;
		SYMBOL_AT(g, (int)(function_index)).finish = emit_here(&g->e_);
		gen_leave_scope(g, --g->scope_);
		g->hoisted_ = 0;
		g->current_function_ = -1;
	}

static void gen_declaration(generator* g)
	{
		int is_typedef = 0;
		int is_static = 0;
		while (gen_kind(g) == tok_kw_static || gen_kind(g) == tok_kw_extern || gen_kind(g) == tok_kw_typedef ||
			gen_kind(g) == tok_kw_qualifier)
		{
			if (gen_kind(g) == tok_kw_typedef) is_typedef = 1;
			if (gen_kind(g) == tok_kw_static) is_static = 1;
			gen_advance(g);
		}

		const int base = gen_parse_type_specifier(g);
		if (gen_accept(g, tok_semicolon)) return; // a bare struct or enum declaration

		for (;;)
		{
			std::string name;
			int is_function = 0;
			std::vector<int> parameters;
			std::vector<std::string> parameter_names;
			int declared = gen_parse_declarator(g, base, &name, &is_function, &parameters, &parameter_names);
			if (name.empty()) gen_fail(g, "expected a name");

			// An array declared without a length takes it from its initialiser,
			// which has to be measured before any storage is reserved.
			if (!is_function && !is_typedef && gen_is_array(g, declared) && (*gen_type(g, declared)).count == 0 &&
				gen_kind(g) == tok_assign)
				declared = gen_array_of(g, gen_target_of(g, declared), gen_initialiser_length(g));

			if (is_typedef)
			{
				gen_define(g, name, sym_typedef, declared);
			}
			else if (is_function)
			{
				int index = gen_find(g, name);
				if (index < 0 || SYMBOL_AT(g, (int)(index)).kind != sym_function)
					index = gen_define(g, name, sym_function, (*gen_type(g, declared)).base);
				SYMBOL_AT(g, (int)(index)).type = (*gen_type(g, declared)).base;
				SYMBOL_AT(g, (int)(index)).parameters = parameters;
				if (gen_kind(g) == tok_l_brace)
				{
					gen_function_body(g, index, &parameters, &parameter_names);
					return;
				}
			}
			else
			{
				// A static local is a global that happens to be spelled inside
				// a function: it keeps its storage between calls, so it lives
				// in the data section and takes the same initialisers.
				const int static_storage = g->scope_ == 0 || is_static;
				int index = gen_find(g, name);
				if (index < 0 || SYMBOL_AT(g, (int)(index)).scope != g->scope_)
				{
					index = gen_define(g, name, static_storage ? sym_global : sym_local, declared);
					if (static_storage)
					{
						SYMBOL_AT(g, (int)(index)).offset = gen_reserve_data(g, gen_type_size(g, declared), 2);
					}
					else
					{
						g->frame_ += gen_type_size(g, declared) + (gen_type_size(g, declared) & 1);
						SYMBOL_AT(g, (int)(index)).offset = -g->frame_;
					}
				}
				else if (static_storage && gen_type_size(g, declared) > gen_type_size(g, SYMBOL_AT(g, (int)(index)).type))
				{
					// A header declares an array without a length and the
					// definition here supplies one, so the byte reserved for
					// the declaration has to give way to the real thing.
					const int old_offset = SYMBOL_AT(g, (int)(index)).offset;
					const int fresh = gen_reserve_data(g, gen_type_size(g, declared), 2);
					for (data_fixup& fixup : g->data_fixups_)
						if (fixup.offset == old_offset) fixup.offset = fresh;
					SYMBOL_AT(g, (int)(index)).offset = fresh;
					SYMBOL_AT(g, (int)(index)).type = declared;
				}

				if (gen_accept(g, tok_assign))
				{
					if (static_storage)
					{
						gen_global_initialiser(g, declared, SYMBOL_AT(g, (int)(index)).offset);
					}
					else
					{
						value target;
						target.kind = val_local;
						target.type = declared;
						target.offset = SYMBOL_AT(g, (int)(index)).offset;
						value source = gen_assignment_expression(g);
						gen_load_converted(g, &source, declared);
						gen_store(g, &target);
					}
				}
			}

			if (!gen_accept(g, tok_comma)) break;
		}
		gen_expect(g, tok_semicolon, "expected ; after a declaration");
	}

static void gen_local_declaration(generator* g) { gen_declaration(g); }

	// -----------------------------------------------------------------------
	// Statements
	// -----------------------------------------------------------------------

static void gen_compound_statement(generator* g)
	{
		++g->scope_;
		while (!gen_accept(g, tok_r_brace))
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated block");
			gen_statement(g);
		}
		gen_leave_scope(g, --g->scope_);
	}

static void gen_for_statement(generator* g)
	{
		++g->scope_;
		gen_expect(g, tok_l_paren, "expected ( after for");
		if (!gen_accept(g, tok_semicolon))
		{
			if (gen_is_type_start(g)) gen_local_declaration(g);
			else
			{
				if (!gen_in_place_statement(g, tok_semicolon)) gen_expression(g);
				gen_expect(g, tok_semicolon, "expected ;");
			}
		}

		// The condition and the step both run after the body, so they are
		// generated after it: an iteration then costs one conditional jump and
		// no unconditional ones. Finding them is a matter of walking the token
		// vector, which is the same trick a call uses to reverse its arguments.
		const size_t condition_at = g->at_;
		int depth = 0;
		while (gen_kind(g) != tok_semicolon || depth != 0)
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated for");
			if (gen_kind(g) == tok_l_paren) ++depth;
			else if (gen_kind(g) == tok_r_paren) --depth;
			gen_advance(g);
		}
		const int has_condition = g->at_ != condition_at;
		gen_advance(g);

		const size_t step_at = g->at_;
		depth = 0;
		while (gen_kind(g) != tok_r_paren || depth != 0)
		{
			if (gen_kind(g) == tok_end) gen_fail(g, "unterminated for");
			if (gen_kind(g) == tok_l_paren) ++depth;
			else if (gen_kind(g) == tok_r_paren) --depth;
			gen_advance(g);
		}
		const int has_step = g->at_ != step_at;
		gen_expect(g, tok_r_paren, "expected ) after for");

		const int condition_label = emit_label(&g->e_);
		const int body_start = emit_label(&g->e_);
		const int step_label = emit_label(&g->e_);
		if (has_condition) emit_jump(&g->e_, condition_label);
		emit_bind(&g->e_, body_start);

		g->break_labels_.push_back(emit_label(&g->e_));
		g->continue_labels_.push_back(step_label);
		gen_statement(g);
		const size_t after = g->at_;

		emit_bind(&g->e_, step_label);
		if (has_step)
		{
			g->at_ = step_at;
			if (!gen_in_place_statement(g, tok_r_paren)) gen_expression(g);
			g->at_ = after;
		}

		if (has_condition)
		{
			emit_bind(&g->e_, condition_label);
			g->at_ = condition_at;
			value v = gen_expression(g);
			const condition taken = gen_test_value(g, &v);
			g->at_ = after;
			emit_jump_if(&g->e_, taken, body_start);
		}
		else
		{
			emit_jump(&g->e_, body_start);
		}

		emit_bind(&g->e_, g->break_labels_.back());
		g->break_labels_.pop_back();
		g->continue_labels_.pop_back();
		gen_leave_scope(g, --g->scope_);
	}

static void gen_switch_statement(generator* g)
	{
		gen_expect(g, tok_l_paren, "expected ( after switch");
		value v = gen_expression(g);
		gen_load(g, &v);
		gen_expect(g, tok_r_paren, "expected ) after the switch expression");

		g->frame_ += 2;
		switch_context context;
		context.temporary = -g->frame_;
		emit_store_ax_frame(&g->e_, context.temporary);
		const int to_dispatch = emit_label(&g->e_);
		emit_jump(&g->e_, to_dispatch);

		g->switches_.push_back(context);
		g->break_labels_.push_back(emit_label(&g->e_));
		gen_statement(g);
		emit_jump(&g->e_, g->break_labels_.back());

		emit_bind(&g->e_, to_dispatch);
		const switch_context& done = g->switches_.back();
		for (const auto& entry : done.cases)
		{
			emit_load_ax_frame(&g->e_, done.temporary);
			emit_cmp_ax_imm(&g->e_, (int)(entry.first));
			emit_jump_if(&g->e_, cond_equal, entry.second);
		}
		if (done.default_label >= 0) emit_jump(&g->e_, done.default_label);
		g->switches_.pop_back();

		emit_bind(&g->e_, g->break_labels_.back());
		g->break_labels_.pop_back();
	}

static void gen_statement(generator* g)
	{
		switch (gen_kind(g))
		{
		case tok_l_brace:
			gen_advance(g);
			gen_compound_statement(g);
			return;

		case tok_semicolon:
			gen_advance(g);
			return;

		case tok_kw_if:
		{
			gen_advance(g);
			gen_expect(g, tok_l_paren, "expected ( after if");
			value v = gen_expression(g);
			const condition taken = gen_test_value(g, &v);
			gen_expect(g, tok_r_paren, "expected ) after the condition");
			const int otherwise = emit_label(&g->e_);
			emit_jump_unless(&g->e_, taken, otherwise);
			gen_statement(g);
			if (gen_accept(g, tok_kw_else))
			{
				const int done = emit_label(&g->e_);
				emit_jump(&g->e_, done);
				emit_bind(&g->e_, otherwise);
				gen_statement(g);
				emit_bind(&g->e_, done);
			}
			else
			{
				emit_bind(&g->e_, otherwise);
			}
			return;
		}

		case tok_kw_while:
		{
			gen_advance(g);
			gen_expect(g, tok_l_paren, "expected ( after while");

			// The condition belongs at the bottom of the loop, so it is
			// generated after the body rather than before it.
			const size_t condition_at = g->at_;
			int depth = 0;
			while (gen_kind(g) != tok_r_paren || depth != 0)
			{
				if (gen_kind(g) == tok_end) gen_fail(g, "unterminated while");
				if (gen_kind(g) == tok_l_paren) ++depth;
				else if (gen_kind(g) == tok_r_paren) --depth;
				gen_advance(g);
			}
			gen_expect(g, tok_r_paren, "expected ) after the condition");

			const int condition_label = emit_label(&g->e_);
			const int start = emit_label(&g->e_);
			emit_jump(&g->e_, condition_label);
			emit_bind(&g->e_, start);
			g->break_labels_.push_back(emit_label(&g->e_));
			g->continue_labels_.push_back(condition_label);
			gen_statement(g);
			const size_t after = g->at_;

			emit_bind(&g->e_, condition_label);
			g->at_ = condition_at;
			value v = gen_expression(g);
			const condition taken = gen_test_value(g, &v);
			g->at_ = after;
			emit_jump_if(&g->e_, taken, start);

			emit_bind(&g->e_, g->break_labels_.back());
			g->break_labels_.pop_back();
			g->continue_labels_.pop_back();
			return;
		}

		case tok_kw_do:
		{
			gen_advance(g);
			const int start = emit_here(&g->e_);
			const int condition_label = emit_label(&g->e_);
			g->break_labels_.push_back(emit_label(&g->e_));
			g->continue_labels_.push_back(condition_label);
			gen_statement(g);
			emit_bind(&g->e_, condition_label);
			if (!gen_accept(g, tok_kw_while)) gen_fail(g, "expected while after do");
			gen_expect(g, tok_l_paren, "expected ( after while");
			value v = gen_expression(g);
			const condition taken = gen_test_value(g, &v);
			gen_expect(g, tok_r_paren, "expected )");
			gen_expect(g, tok_semicolon, "expected ;");
			emit_jump_if(&g->e_, taken, start);
			emit_bind(&g->e_, g->break_labels_.back());
			g->break_labels_.pop_back();
			g->continue_labels_.pop_back();
			return;
		}

		case tok_kw_for:
			gen_advance(g);
			gen_for_statement(g);
			return;

		case tok_kw_switch:
			gen_advance(g);
			gen_switch_statement(g);
			return;

		case tok_kw_case:
		{
			gen_advance(g);
			if ((g->switches_.count == 0)) gen_fail(g, "case outside a switch");
			const long label = gen_constant_expression(g);
			gen_expect(g, tok_colon, "expected : after case");
			g->switches_.back().cases.emplace_back(label, emit_here(&g->e_));
			return;
		}

		case tok_kw_default:
			gen_advance(g);
			if ((g->switches_.count == 0)) gen_fail(g, "default outside a switch");
			gen_expect(g, tok_colon, "expected : after default");
			g->switches_.back().default_label = emit_here(&g->e_);
			return;

		case tok_kw_break:
			gen_advance(g);
			gen_expect(g, tok_semicolon, "expected ;");
			if ((g->break_labels_.count == 0)) gen_fail(g, "break outside a loop or switch");
			emit_jump(&g->e_, g->break_labels_.back());
			return;

		case tok_kw_continue:
			gen_advance(g);
			gen_expect(g, tok_semicolon, "expected ;");
			if ((g->continue_labels_.count == 0)) gen_fail(g, "continue outside a loop");
			emit_jump(&g->e_, g->continue_labels_.back());
			return;

		case tok_kw_goto:
		{
			gen_advance(g);
			if (gen_kind(g) != tok_identifier) gen_fail(g, "expected a label after goto");
			const std::string name = (*gen_peek(g, 0)).text;
			gen_advance(g);
			gen_expect(g, tok_semicolon, "expected ; after goto");
			if (g->labels_.find(name) == g->labels_.end()) g->labels_[name] = emit_label(&g->e_);
			g->pending_gotos_.emplace_back(name, 0);
			emit_jump(&g->e_, g->labels_[name]);
			return;
		}

		case tok_kw_return:
		{
			gen_advance(g);
			if (gen_kind(g) != tok_semicolon)
			{
				value v = gen_expression(g);
				gen_load_converted(g, &v, g->return_type_);
			}
			gen_expect(g, tok_semicolon, "expected ; after return");
			// Inside a body that was copied to a call site there is no frame to
			// unwind: the value is already where the caller wants it.
			if (!(g->inline_returns_.count == 0)) emit_jump(&g->e_, g->inline_returns_.back());
			else gen_emit_return(g);
			return;
		}

		default:
			break;
		}

		if (gen_is_type_start(g))
		{
			gen_local_declaration(g);
			return;
		}

		if (gen_kind(g) == tok_identifier && (*gen_peek(g, 1)).kind == tok_colon)
		{
			const std::string name = (*gen_peek(g, 0)).text;
			if (g->labels_.find(name) == g->labels_.end()) g->labels_[name] = emit_label(&g->e_);
			emit_bind(&g->e_, g->labels_[name]);
			gen_advance(g);
			gen_advance(g);
			if (gen_kind(g) != tok_r_brace) gen_statement(g);
			return;
		}

		if (!gen_in_place_statement(g, tok_semicolon)) gen_expression(g);
		gen_expect(g, tok_semicolon, "expected ; after an expression");
	}

	// -----------------------------------------------------------------------
	// Runtime, startup and output
	// -----------------------------------------------------------------------

static void gen_emit_startup(generator* g)
	{
		const int main_index = gen_define(g, "main", sym_function, g->t_int_);

		// Code a host calls arrives with a stack already, and returns rather
		// than exits. RBX, RSI and RDI belong to the caller, and the generator
		// treats all three as scratch.
		if (g->target_.hosted)
		{
			emit_byte(&g->e_, 0x53); // push rbx
			emit_byte(&g->e_, 0x56); // push rsi
			emit_byte(&g->e_, 0x57); // push rdi
			emit_call(&g->e_, gen_label_of(g, main_index));
			SYMBOL_AT(g, (int)(main_index)).called = 1;
			emit_byte(&g->e_, 0x5f); // pop rdi
			emit_byte(&g->e_, 0x5e); // pop rsi
			emit_byte(&g->e_, 0x5b); // pop rbx
			emit_ret(&g->e_);
			return;
		}

		// mov sp, <top of our block>; shrink the block DOS gave us so that a
		// child process has somewhere to load, then call main and exit.
		emit_mov_sp_imm(&g->e_, 0);
		g->stack_patch_ = emit_refers_to_deferred(&g->e_);
		emit_mov_bx_imm(&g->e_, 0);
		g->paragraph_patch_ = emit_refers_to_deferred(&g->e_);
		emit_op(&g->e_, op_mov, 1, operand_in(reg_ah), operand_constant(0x4a));
		emit_interrupt(&g->e_, 0x21);

		emit_call(&g->e_, gen_label_of(g, main_index));
		SYMBOL_AT(g, (int)(main_index)).called = 1;

		emit_op(&g->e_, op_mov, 1, operand_in(reg_ah), operand_constant(0x4c));
		emit_interrupt(&g->e_, 0x21);
	}

	// The thunk a C program calls to reach a BIOS or DOS service: registers in,
	// AX out, and whatever DOS left in the other registers readable afterwards.
static void gen_emit_interrupt_call(generator* g,int number, int cf, int bx, int cx, int dx, int with_si)
	{
		const auto from_frame = [&](int r, int displacement)
		{
			emit_op(&g->e_, op_mov, 2, operand_in(r), emit_frame(displacement));
		};
		const auto to_global = [&](int symbol, int r)
		{
			emit_op(&g->e_, op_mov, 2, operand_absolute(0), operand_in(r));
			gen_data_reference(g, SYMBOL_AT(g, (int)(symbol)).offset);
		};
		const auto set_global = [&](int symbol, int value)
		{
			emit_op(&g->e_, op_mov, 2, operand_absolute(0), operand_constant(value));
			gen_data_reference(g, SYMBOL_AT(g, (int)(symbol)).offset);
		};

		emit_push_bp(&g->e_);
		emit_mov_bp_sp(&g->e_);
		if (with_si) emit_op(&g->e_, op_push, 2, operand_in(reg_si), operand_none());
		from_frame(reg_a, 4);
		from_frame(reg_b, 6);
		from_frame(reg_c, 8);
		from_frame(reg_d, 10);
		if (with_si) from_frame(reg_si, 12);
		emit_interrupt(&g->e_, number);

		to_global(bx, reg_b);
		to_global(cx, reg_c);
		to_global(dx, reg_d);
		set_global(cf, 0);
		const int carry_clear = emit_label(&g->e_);
		emit_jump_if(&g->e_, cond_above_equal, carry_clear);
		set_global(cf, 1);
		emit_bind(&g->e_, carry_clear);
		if (with_si) emit_op(&g->e_, op_pop, 2, operand_in(reg_si), operand_none());
		emit_pop_bp(&g->e_);
		emit_ret(&g->e_);
	}

	// The subroutines the 32-bit operators call, written out after the program
	// so that a program using no long pays nothing for them. Each takes DX:AX
	// and CX:BX and returns DX:AX.
static void gen_emit_helpers(generator* g)
	{
		// A helper is wanted if a call that survived generation still refers to
		// it, which the layout record answers directly.
		const auto referenced = [&](int which)
		{
			if (g->helper_label_[which] < 0) return 0;
			for (int i = 0; i < g->e_.code.count; ++i)
			{
				const instruction& in = EMIT_CODE(g->e_, i);
				if (in.code == op_call && in.label == g->helper_label_[which]) return 1;
			}
			return 0;
		};

		int used[helper_count] = {};
		for (int i = 0; i < helper_count; ++i) used[i] = referenced(i);
		if (used[h_ldiv] || used[h_uldiv] || used[h_lmod] || used[h_ulmod]) used[h_uldivmod] = 1;

		const auto bytes = [&](std::initializer_list<int> list) { for (const int b : list) emit_byte(&g->e_, b); };

		// The helpers were hand-assembled, but their branches go through the
		// layout record like everything else, so they shorten with the rest.
		const auto branch = [&](condition c)
		{
			const int target = emit_label(&g->e_);
			emit_jump_if(&g->e_, c, target);
			return target;
		};
		const auto here = [&](int target) { emit_bind(&g->e_, target); };
		const auto again = [&](condition c, int target) { emit_jump_if(&g->e_, c, target); };
		const auto negate = [&] { emit_negate_dx_ax(&g->e_); };

		if (used[h_uldivmod] || used[h_uldiv])
		{
			// Restoring division a bit at a time: SI:DI accumulates the
			// remainder while the quotient replaces the dividend in DX:AX.
			emit_bind(&g->e_, gen_helper_label(g, h_uldivmod));
			if (g->helper_label_[h_uldiv] >= 0) emit_bind(&g->e_, g->helper_label_[h_uldiv]);
			bytes({0x56, 0x57, 0x55}); // push si / push di / push bp
			bytes({0x31, 0xf6, 0x31, 0xff}); // xor si,si / xor di,di
			bytes({0xbd, 0x20, 0x00}); // mov bp,32
			const int loop = emit_here(&g->e_);
			bytes({0xd1, 0xe0, 0xd1, 0xd2, 0xd1, 0xd7, 0xd1, 0xd6}); // shl ax,1 / rcl dx,1 / rcl di,1 / rcl si,1
			bytes({0x39, 0xce}); // cmp si,cx
			const int skip_low = branch(cond_below);
			const int to_subtract = branch(cond_above);
			bytes({0x39, 0xdf}); // cmp di,bx
			const int skip = branch(cond_below);
			here(to_subtract);
			bytes({0x29, 0xdf, 0x19, 0xce}); // sub di,bx / sbb si,cx
			bytes({0x0c, 0x01}); // or al,1
			here(skip_low);
			here(skip);
			emit_byte(&g->e_, 0x4d); // dec bp
			again(cond_not_equal, loop);
			bytes({0x89, 0xf1, 0x89, 0xfb}); // mov cx,si / mov bx,di
			bytes({0x5d, 0x5f, 0x5e}); // pop bp / pop di / pop si
			emit_ret(&g->e_);
		}

		const auto call_divide = [&] { emit_call(&g->e_, gen_helper_label(g, h_uldivmod)); };

		if (used[h_lmul])
		{
			// The high word of a 32-bit product needs only the low halves of
			// the two cross terms, so three 16-bit multiplies are enough.
			emit_bind(&g->e_, gen_helper_label(g, h_lmul));
			bytes({0x56, 0x57}); // push si / push di
			bytes({0x89, 0xc6, 0x89, 0xd7}); // mov si,ax / mov di,dx
			bytes({0xf7, 0xe1}); // mul cx
			bytes({0x89, 0xc1}); // mov cx,ax
			bytes({0x89, 0xf8}); // mov ax,di
			bytes({0xf7, 0xe3}); // mul bx
			bytes({0x01, 0xc1}); // add cx,ax
			bytes({0x89, 0xf0}); // mov ax,si
			bytes({0xf7, 0xe3}); // mul bx
			bytes({0x01, 0xca}); // add dx,cx
			bytes({0x5f, 0x5e}); // pop di / pop si
			emit_ret(&g->e_);
		}

		if (used[h_ulmod])
		{
			emit_bind(&g->e_, gen_helper_label(g, h_ulmod));
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			emit_ret(&g->e_);
		}

		if (used[h_ldiv])
		{
			emit_bind(&g->e_, gen_helper_label(g, h_ldiv));
			emit_byte(&g->e_, 0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(cond_greater_equal);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(cond_greater_equal);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(cond_equal);
			negate();
			here(done);
			emit_byte(&g->e_, 0x5e); // pop si
			emit_ret(&g->e_);
		}

		if (used[h_lmod])
		{
			// The sign of a remainder follows the dividend, not the divisor.
			emit_bind(&g->e_, gen_helper_label(g, h_lmod));
			emit_byte(&g->e_, 0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(cond_greater_equal);
			bytes({0xbe, 0x01, 0x00}); // mov si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(cond_greater_equal);
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(cond_equal);
			negate();
			here(done);
			emit_byte(&g->e_, 0x5e); // pop si
			emit_ret(&g->e_);
		}

		const int shifts[3] = {h_lshl, h_lshr, h_lsar};
		for (const int which : shifts)
		{
			if (!used[which]) continue;
			emit_bind(&g->e_, gen_helper_label(g, which));
			bytes({0x88, 0xd9}); // mov cl,bl
			bytes({0x80, 0xe1, 0x1f}); // and cl,31
			const int done = branch(cond_equal);
			const int loop = emit_here(&g->e_);
			if (which == h_lshl) bytes({0xd1, 0xe0, 0xd1, 0xd2}); // shl ax,1 / rcl dx,1
			else if (which == h_lshr) bytes({0xd1, 0xea, 0xd1, 0xd8}); // shr dx,1 / rcr ax,1
			else bytes({0xd1, 0xfa, 0xd1, 0xd8}); // sar dx,1 / rcr ax,1
			bytes({0xfe, 0xc9}); // dec cl
			again(cond_not_equal, loop);
			here(done);
			emit_ret(&g->e_);
		}

		for (const int which : {(int)(h_lcmp), (int)(h_ulcmp)})
		{
			if (!used[which]) continue;
			// Reduces the pair to -1, 0 or 1 so the caller can use the ordinary
			// signed conditions whatever the operand signedness was.
			emit_bind(&g->e_, gen_helper_label(g, which));
			bytes({0x39, 0xca}); // cmp dx,cx
			const condition low = which == h_lcmp ? cond_less : cond_below;
			const condition high = which == h_lcmp ? cond_greater : cond_above;
			const int less_high = branch(low);
			const int greater_high = branch(high);
			bytes({0x39, 0xd8}); // cmp ax,bx
			const int less_low = branch(cond_below);
			const int greater_low = branch(cond_above);
			emit_xor_ax_ax(&g->e_);
			emit_ret(&g->e_);
			here(less_high);
			here(less_low);
			emit_mov_ax_imm(&g->e_, -1);
			emit_ret(&g->e_);
			here(greater_high);
			here(greater_low);
			emit_mov_ax_imm(&g->e_, 1);
			emit_ret(&g->e_);
		}
	}

static void gen_emit_runtime(generator* g)
	{
		// The registers an INT 21h call left behind, so C code can read the
		// ones the return value does not carry.
		const int cf = gen_define(g, "__cf", sym_global, g->t_int_);
		SYMBOL_AT(g, (int)(cf)).offset = gen_reserve_data(g, 2, 2);
		const int bx = gen_define(g, "__bx", sym_global, g->t_int_);
		SYMBOL_AT(g, (int)(bx)).offset = gen_reserve_data(g, 2, 2);
		const int cx = gen_define(g, "__cx", sym_global, g->t_int_);
		SYMBOL_AT(g, (int)(cx)).offset = gen_reserve_data(g, 2, 2);
		const int dx = gen_define(g, "__dx", sym_global, g->t_int_);
		SYMBOL_AT(g, (int)(dx)).offset = gen_reserve_data(g, 2, 2);

		// The thunks are generated before anything can call them, so each one
		// is a function whose body is already there.
		const auto thunk = [&](const char* name, int type)
		{
			const int index = gen_define(g, name, sym_function, type);
			SYMBOL_AT(g, (int)(index)).defined = 1;
			SYMBOL_AT(g, (int)(index)).start = emit_here(&g->e_);
			SYMBOL_AT(g, (int)(index)).label = SYMBOL_AT(g, (int)(index)).start;
			return index;
		};

		const int int21 = thunk("__int21", g->t_int_);
		gen_emit_interrupt_call(g, 0x21, cf, bx, cx, dx, 0);
		SYMBOL_AT(g, (int)(int21)).finish = emit_here(&g->e_);

		const int int10 = thunk("__int10", g->t_int_);
		gen_emit_interrupt_call(g, 0x10, cf, bx, cx, dx, 0);
		SYMBOL_AT(g, (int)(int10)).finish = emit_here(&g->e_);

		// The same call with SI set, for the handful of services that take a
		// buffer there rather than in DX.
		const int int21si = thunk("__int21si", g->t_int_);
		gen_emit_interrupt_call(g, 0x21, cf, bx, cx, dx, 1);
		SYMBOL_AT(g, (int)(int21si)).finish = emit_here(&g->e_);

		// The segment everything lives in, which a program needs when it hands
		// DOS a far pointer.
		const int seg = thunk("__seg", g->t_uint_);
		emit_mov_ax_cs(&g->e_);
		emit_ret(&g->e_);
		SYMBOL_AT(g, (int)(seg)).finish = emit_here(&g->e_);
	}

static void gen_run(generator* g)
	{
		g->types_.clear();
		type_info info;
		info.kind = ty_void;
		info.size = 1;
		g->t_void_ = gen_add_type(g, info);
		info.kind = ty_char;
		info.size = 1;
		g->t_char_ = gen_add_type(g, info);
		info.kind = ty_uchar;
		info.size = 1;
		g->t_uchar_ = gen_add_type(g, info);
		info.kind = ty_int;
		info.size = g->target_.int_size;
		g->t_int_ = gen_add_type(g, info);
		info.kind = ty_uint;
		info.size = g->target_.int_size;
		g->t_uint_ = gen_add_type(g, info);
		info.kind = ty_long;
		info.size = g->target_.long_size;
		g->t_long_ = gen_add_type(g, info);
		info.kind = ty_ulong;
		info.size = g->target_.long_size;
		g->t_ulong_ = gen_add_type(g, info);
		info.kind = ty_real;
		info.size = g->target_.long_size;
		g->t_real_ = gen_add_type(g, info);

		gen_emit_startup(g);
		gen_emit_runtime(g);

		while (gen_kind(g) != tok_end) gen_declaration(g);
		gen_emit_helpers(g);
	}

	// Which functions the program can actually get to. Compiling a whole
	// project as one translation unit brings in everything every file declares,
	// and most of it is never called; the layout record knows what refers to
	// what, so the rest can simply not be laid out.
static void gen_drop_unreachable(generator* g)
	{
		if (!g->options_.drop_unreachable) return;

		struct span
		{
			int from = 0;
			int to = 0;
			int symbol = 0;
		};

		std::vector<span> spans;
		std::map<int, int> starts; // the instruction a function begins at, to its symbol
		for (size_t i = 0; i < g->symbols_.count; ++i)
		{
			const symbol& entry = SYMBOL_AT(g, i);
			if (entry.kind != sym_function || !entry.defined) continue;
			if (entry.start < 0 || entry.finish < 0) continue;
			span s;
			s.from = EMIT_LABEL(g->e_, (int)(entry.start));
			s.to = EMIT_LABEL(g->e_, (int)(entry.finish));
			s.symbol = (int)(i);
			if (s.from < 0 || s.to <= s.from) continue;
			spans.push_back(s);
			starts[s.from] = (int)(i);
		}
		if (spans.empty()) return;
		std::sort(spans.begin(), spans.end(),
		          [](const span& a, const span& b) { return a.from < b.from; });

		// Everything one function refers to, plus what is referred to from
		// outside any of them: the startup code and the runtime helpers.
		std::map<int, std::vector<int>> edges;
		std::vector<int> roots;
		const auto reference = [&](int at, int label)
		{
			if (label < 0 || !emit_is_bound(&g->e_, label)) return;
			const auto target = starts.find(EMIT_LABEL(g->e_, (int)(label)));
			if (target == starts.end()) return;

			size_t low = 0, high = spans.size();
			while (low < high)
			{
				const size_t middle = (low + high) / 2;
				if (spans[middle].to <= at) low = middle + 1;
				else high = middle;
			}
			if (low < spans.size() && at >= spans[low].from)
				edges[spans[low].symbol].push_back(target->second);
			else
				roots.push_back(target->second);
		};

		for (int i = 0; i < g->e_.code.count; ++i)
		{
			const instruction& in = EMIT_CODE(g->e_, i);
			if (in.code == op_call) reference(i, in.label);
			for (const operand* o : {&in.dest, &in.source})
				if (o->reference == reference_kind_code) reference(i, o->target);
		}
		for (const function_fixup& fixup : g->function_fixups_) roots.push_back(fixup.function);

		std::vector<int> reached(g->symbols_.count, 0);
		std::vector<int> pending = roots;
		while (!pending.empty())
		{
			const int index = pending.back();
			pending.pop_back();
			if (reached[(size_t)(index)]) continue;
			reached[(size_t)(index)] = 1;
			const auto found = edges.find(index);
			if (found == edges.end()) continue;
			for (const int next : found->second) pending.push_back(next);
		}

		for (const span& s : spans)
			if (!reached[(size_t)(s.symbol)])
				emit_drop(&g->e_, SYMBOL_AT(g, (int)(s.symbol)).start, SYMBOL_AT(g, (int)(s.symbol)).finish);
	}

static void gen_finish(generator* g, cc_result* result)
	{
		for (const symbol& entry : g->symbols_)
		{
			if (entry.kind != sym_function || !entry.called || entry.defined) continue;
			throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
		}

		// Nothing has an address until now: the layout pass picks the shortest
		// form for every branch, drops what the program never reaches, and
		// moves what is left.
		gen_drop_unreachable(g);
		emit_lay_out(&g->e_, g->target_.code_origin);

		// Data follows the code, so its address is only known after that.
		int code_size = (int)(g->e_.image.count);
		if (code_size % 2 != 0) ++code_size;
		const int data_base = g->target_.code_origin + code_size;
		const int data_size = (int)(g->data_.count);
		result->code_size = code_size;
		result->data_size = data_size;

		const int image_end = data_base + data_size;
		const int stack_top = (image_end + g->target_.stack_size + 1) & ~1;
		if (stack_top > g->target_.address_limit)
		{
			char message[160] = {};
			snprintf(message, sizeof(message),
			         "the program is too large for one segment: %d bytes of code and %d of data",
			         code_size, data_size);
			throw compile_error{message, 0, 0};
		}
		if (g->stack_patch_ >= 0) emit_set_deferred(&g->e_, g->stack_patch_, stack_top);
		if (g->paragraph_patch_ >= 0) emit_set_deferred(&g->e_, g->paragraph_patch_, (stack_top + 15) / 16);
		if (!emit_resolve(&g->e_, g->target_.code_origin, data_base))
			throw compile_error{"a jump was never given somewhere to go", 0, 0};

		// The data section holds pointers of its own, and it does not move.
		const auto put_word = [&](int at, int value)
		{
			DATA_AT(g, (int)(at)) = (uint8_t)(value & 0xff);
			g->data_[(size_t)(at) + 1] = (uint8_t)((value >> 8) & 0xff);
		};

		for (const function_fixup& fixup : g->function_fixups_)
		{
			const symbol& entry = SYMBOL_AT(g, (int)(fixup.function));
			if (!entry.defined) throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
			put_word(fixup.position, g->target_.code_origin + emit_offset_of(&g->e_, entry.label));
		}

		for (const data_fixup& fixup : g->data_fixups_)
			put_word(fixup.position, data_base + fixup.offset);

		// Code first, padded to an even boundary, then the data section.
		uint8_t* image = (uint8_t*)cc_arena_alloc(&result->arena, (size_t)(code_size + data_size) + 1);
		memcpy(image, g->e_.image.items, (size_t)(g->e_.image.count));
		for (int i = g->e_.image.count; i < code_size; ++i) image[i] = 0x90;
		if (data_size > 0) memcpy(image + code_size, g->data_.data(), (size_t)(data_size));
		result->image = image;
		result->image_size = code_size + data_size;
	}

	// What survived, and how much room it takes. The spans come from the
	// layout record, so this is what actually ended up in the image, and it is
	// what a multi-segment build would have to divide up.
static void gen_report(generator* g, cc_result* result)
	{
		int count = 0;
		for (const symbol& entry : g->symbols_)
		{
			if (entry.kind != sym_function || !entry.defined) continue;
			if (entry.start < 0 || entry.finish < 0 || entry.name.empty()) continue;
			if (emit_offset_of(&g->e_, entry.finish) - emit_offset_of(&g->e_, entry.start) > 0) ++count;
		}

		cc_function* functions = (cc_function*)cc_arena_alloc(&result->arena,
		                                                      sizeof(cc_function) * (size_t)(count + 1));
		result->functions = functions;
		result->function_count = 0;

		for (const symbol& entry : g->symbols_)
		{
			if (entry.kind != sym_function || !entry.defined) continue;
			if (entry.start < 0 || entry.finish < 0 || entry.name.empty()) continue;
			const int size = emit_offset_of(&g->e_, entry.finish) - emit_offset_of(&g->e_, entry.start);
			if (size <= 0) continue;
			functions[result->function_count].name =
				cc_arena_copy(&result->arena, entry.name.c_str(), entry.name.size());
			functions[result->function_count].size = size;
			++result->function_count;
		}
	}
}

void cc_result_free(cc_result* result)
{
	cc_arena_free(&result->arena);
	memset(result, 0, sizeof(*result));
}

cc_result cc_compile(const char* text, const char* name, cc_sources* sources,
                     const char* const* defines, int define_count, const cc_options* options)
{
	cc_result result;
	cc_names names;
	cc_token_stream stream;
	cc_options settings;

	memset(&result, 0, sizeof(result));
	cc_arena_init(&result.arena, 0);
	cc_names_init(&names, &result.arena);
	cc_token_stream_init(&stream, &result.arena, &names);

	cc_options_init(&settings);
	if (options != NULL) settings = *options;

	// Everything the result points at lives in the result's own arena, so only
	// the tables that helped build it are given back here.
	const auto release = [&]
	{
		cc_token_stream_free(&stream);
		cc_names_free(&names);
	};

	if (!cc_tokenise(&stream, text, name, sources, defines, define_count))
	{
		result.message = cc_arena_copy(&result.arena, stream.error, strlen(stream.error));
		result.file = stream.error_file != NULL ? stream.error_file : name;
		result.line = stream.error_line;
		release();
		return result;
	}

	generator pass(stream, settings);
	try
	{
		pass.run();
		pass.finish(result);
		pass.report(result);
		result.ok = 1;
	}
	catch (const compile_error& error)
	{
		// The sizes are worth having even when the program turned out not to
		// fit, which is the moment somebody wants to know where it all went.
		pass.report(result);
		result.message = cc_arena_copy(&result.arena, error.message.c_str(), error.message.size());
		result.line = error.line;
		result.file = error.file != NULL ? error.file : name;
	}
	release();
	return result;
}

