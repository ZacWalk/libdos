#ifndef OVRWIN_CC_LEX_H
#define OVRWIN_CC_LEX_H

/* ovrwin by Zac Walker
 *
 * Tokeniser and preprocessor. The whole translation unit is turned into one
 * token list with includes inlined and macros expanded, which the parser then
 * walks; that costs a little memory and saves a great deal of plumbing.
 *
 * A token carries no text of its own. An identifier is an interned name - an
 * integer, so comparing two of them is comparing two integers - and the text
 * beside it points at the one copy of that name. A string literal points into
 * the arena at its decoded bytes, which may contain a NUL, so it carries a
 * length as well.
 */

#include "sources.h"
#include "support.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cc_tok
{
	tok_end, tok_identifier, tok_number, tok_string, tok_character,

	tok_kw_int, tok_kw_char, tok_kw_void, tok_kw_unsigned, tok_kw_signed,
	tok_kw_short, tok_kw_long, tok_kw_if, tok_kw_else, tok_kw_while, tok_kw_for,
	tok_kw_do, tok_kw_return, tok_kw_break, tok_kw_continue, tok_kw_switch,
	tok_kw_case, tok_kw_default, tok_kw_struct, tok_kw_union, tok_kw_enum,
	tok_kw_typedef, tok_kw_sizeof, tok_kw_static, tok_kw_extern, tok_kw_const,
	tok_kw_goto, tok_kw_qualifier,

	tok_l_paren, tok_r_paren, tok_l_brace, tok_r_brace, tok_l_bracket, tok_r_bracket,
	tok_semicolon, tok_comma, tok_dot, tok_arrow, tok_question, tok_colon,
	tok_plus, tok_minus, tok_star, tok_slash, tok_percent, tok_amp, tok_pipe,
	tok_caret, tok_tilde, tok_bang, tok_assign, tok_less, tok_greater, tok_inc,
	tok_dec, tok_shl, tok_shr, tok_le, tok_ge, tok_eq, tok_ne, tok_and_and,
	tok_or_or, tok_plus_assign, tok_minus_assign, tok_star_assign,
	tok_slash_assign, tok_percent_assign, tok_amp_assign, tok_pipe_assign,
	tok_caret_assign, tok_shl_assign, tok_shr_assign, tok_ellipsis
} cc_tok;

typedef struct cc_token
{
	cc_tok kind;
	int name; /* the interned identifier, or zero */
	const char* text; /* that name's one copy, or a string literal's bytes */
	int length; /* the bytes in a string literal */
	long value; /* numeric or character constant */
	double real; /* the value when is_real, which only a constant may be */
	int is_real;
	int wide; /* the literal carries an L suffix or does not fit in 16 bits */
	int unsigned_literal; /* the literal carries a U suffix */
	int line;
	const char* file;
} cc_token;

typedef struct cc_token_stream
{
	cc_arena* arena;
	cc_names* names;
	cc_list tokens; /* cc_token */
	char error[192];
	const char* error_file;
	int error_line;
} cc_token_stream;

#define CC_TOKEN(stream, index) (((cc_token*)(stream).tokens.items)[index])

void cc_token_stream_init(cc_token_stream* s, cc_arena* arena, cc_names* names);
void cc_token_stream_free(cc_token_stream* s);

/* Non-zero when the whole translation unit was read. `text` is the top-level
 * source; every other file comes from `sources`. */
int cc_tokenise(cc_token_stream* out, const char* text, const char* name, cc_sources* sources,
                const char* const* defines, int define_count);

#ifdef __cplusplus
}

/* While the parser is still C++ it spells these tok::end rather than tok_end.
 * This block is what lets the lexer be C first, and goes when gen.cpp follows. */
namespace tok
{
	const cc_tok end = tok_end;
	const cc_tok identifier = tok_identifier;
	const cc_tok number = tok_number;
	const cc_tok string = tok_string;
	const cc_tok character = tok_character;
	const cc_tok kw_int = tok_kw_int;
	const cc_tok kw_char = tok_kw_char;
	const cc_tok kw_void = tok_kw_void;
	const cc_tok kw_unsigned = tok_kw_unsigned;
	const cc_tok kw_signed = tok_kw_signed;
	const cc_tok kw_short = tok_kw_short;
	const cc_tok kw_long = tok_kw_long;
	const cc_tok kw_if = tok_kw_if;
	const cc_tok kw_else = tok_kw_else;
	const cc_tok kw_while = tok_kw_while;
	const cc_tok kw_for = tok_kw_for;
	const cc_tok kw_do = tok_kw_do;
	const cc_tok kw_return = tok_kw_return;
	const cc_tok kw_break = tok_kw_break;
	const cc_tok kw_continue = tok_kw_continue;
	const cc_tok kw_switch = tok_kw_switch;
	const cc_tok kw_case = tok_kw_case;
	const cc_tok kw_default = tok_kw_default;
	const cc_tok kw_struct = tok_kw_struct;
	const cc_tok kw_union = tok_kw_union;
	const cc_tok kw_enum = tok_kw_enum;
	const cc_tok kw_typedef = tok_kw_typedef;
	const cc_tok kw_sizeof = tok_kw_sizeof;
	const cc_tok kw_static = tok_kw_static;
	const cc_tok kw_extern = tok_kw_extern;
	const cc_tok kw_const = tok_kw_const;
	const cc_tok kw_goto = tok_kw_goto;
	const cc_tok kw_qualifier = tok_kw_qualifier;
	const cc_tok l_paren = tok_l_paren;
	const cc_tok r_paren = tok_r_paren;
	const cc_tok l_brace = tok_l_brace;
	const cc_tok r_brace = tok_r_brace;
	const cc_tok l_bracket = tok_l_bracket;
	const cc_tok r_bracket = tok_r_bracket;
	const cc_tok semicolon = tok_semicolon;
	const cc_tok comma = tok_comma;
	const cc_tok dot = tok_dot;
	const cc_tok arrow = tok_arrow;
	const cc_tok question = tok_question;
	const cc_tok colon = tok_colon;
	const cc_tok plus = tok_plus;
	const cc_tok minus = tok_minus;
	const cc_tok star = tok_star;
	const cc_tok slash = tok_slash;
	const cc_tok percent = tok_percent;
	const cc_tok amp = tok_amp;
	const cc_tok pipe = tok_pipe;
	const cc_tok caret = tok_caret;
	const cc_tok tilde = tok_tilde;
	const cc_tok bang = tok_bang;
	const cc_tok assign = tok_assign;
	const cc_tok less = tok_less;
	const cc_tok greater = tok_greater;
	const cc_tok inc = tok_inc;
	const cc_tok dec = tok_dec;
	const cc_tok shl = tok_shl;
	const cc_tok shr = tok_shr;
	const cc_tok le = tok_le;
	const cc_tok ge = tok_ge;
	const cc_tok eq = tok_eq;
	const cc_tok ne = tok_ne;
	const cc_tok and_and = tok_and_and;
	const cc_tok or_or = tok_or_or;
	const cc_tok plus_assign = tok_plus_assign;
	const cc_tok minus_assign = tok_minus_assign;
	const cc_tok star_assign = tok_star_assign;
	const cc_tok slash_assign = tok_slash_assign;
	const cc_tok percent_assign = tok_percent_assign;
	const cc_tok amp_assign = tok_amp_assign;
	const cc_tok pipe_assign = tok_pipe_assign;
	const cc_tok caret_assign = tok_caret_assign;
	const cc_tok shl_assign = tok_shl_assign;
	const cc_tok shr_assign = tok_shr_assign;
	const cc_tok ellipsis = tok_ellipsis;
}
#endif

#endif
