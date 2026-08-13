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
/* PORT: the rest of the file is appended here */
