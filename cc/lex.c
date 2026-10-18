/* libdos by Zac Walker
 *
 * Tokeniser and preprocessor.
 *
 * Two things changed on the way from C++ to C, and both are visible
 * throughout: an identifier is an interned integer rather than a string, so a
 * macro table is a sorted array of integers and comparing names costs nothing;
 * and a token list is a cc_list, so the places that used to swap vectors now
 * hand a list's contents over and free what was there.
 */

#include "lex.h"

#include <stdlib.h>
#include <string.h>

typedef struct cc_macro
{
	int name;
	int function_like;
	int expanding; /* guards against a macro expanding itself */
	cc_list parameters; /* int, interned */
	cc_list body; /* cc_token */
} cc_macro;

/* One source file being scanned. Includes push another of these. */
typedef struct cc_scanner
{
	const char* text;
	size_t length;
	size_t position;
	int line;
	const char* file;
} cc_scanner;

typedef struct cc_preprocessor
{
	cc_sources* sources;
	cc_token_stream* out;
	cc_arena* arena;
	cc_names* names;
	cc_list stack; /* cc_scanner */
	cc_list macros; /* cc_macro */
	cc_map by_name; /* interned name to macro index, or -1 once undefined */
	cc_list conditions; /* int, one per open #if, non-zero when taking the branch */
	cc_map keywords; /* interned name to cc_tok */

	/* The words the preprocessor itself reacts to, interned once. */
	int n_hash, n_include, n_define, n_undef, n_if, n_ifdef, n_ifndef;
	int n_else, n_elif, n_endif, n_error, n_pragma, n_line, n_defined, n_args;
} cc_preprocessor;

typedef struct cc_keyword
{
	const char* name;
	cc_tok kind;
} cc_keyword;

static const cc_keyword cc_keywords[] = {
	{"int", tok_kw_int}, {"char", tok_kw_char}, {"void", tok_kw_void},
	{"unsigned", tok_kw_unsigned}, {"signed", tok_kw_signed}, {"short", tok_kw_short},
	{"long", tok_kw_long}, {"if", tok_kw_if}, {"else", tok_kw_else},
	{"while", tok_kw_while}, {"for", tok_kw_for}, {"do", tok_kw_do},
	{"return", tok_kw_return}, {"break", tok_kw_break}, {"continue", tok_kw_continue},
	{"switch", tok_kw_switch}, {"case", tok_kw_case}, {"default", tok_kw_default},
	{"struct", tok_kw_struct}, {"union", tok_kw_union}, {"enum", tok_kw_enum},
	{"typedef", tok_kw_typedef}, {"sizeof", tok_kw_sizeof}, {"static", tok_kw_static},
	{"extern", tok_kw_extern}, {"const", tok_kw_const}, {"goto", tok_kw_goto},
	/* Qualifiers the parser accepts and ignores. The memory-model ones are
	 * meaningless while the compiler emits only the tiny model. */
	{"volatile", tok_kw_qualifier}, {"register", tok_kw_qualifier},
	{"inline", tok_kw_qualifier}, {"__inline", tok_kw_qualifier},
	{"_Noreturn", tok_kw_qualifier}, {"restrict", tok_kw_qualifier},
	{"__cdecl", tok_kw_qualifier}, {"__interrupt", tok_kw_qualifier},
	{"__far", tok_kw_qualifier}, {"__near", tok_kw_qualifier}, {"__huge", tok_kw_qualifier},
	{"_far", tok_kw_qualifier}, {"_near", tok_kw_qualifier}, {"_huge", tok_kw_qualifier}
};

typedef struct cc_punctuator
{
	const char* text;
	cc_tok kind;
} cc_punctuator;

static const cc_punctuator cc_punctuators[] = {
	{"...", tok_ellipsis},
	{"<<=", tok_shl_assign}, {">>=", tok_shr_assign},
	{"->", tok_arrow}, {"++", tok_inc}, {"--", tok_dec},
	{"<<", tok_shl}, {">>", tok_shr}, {"<=", tok_le}, {">=", tok_ge},
	{"==", tok_eq}, {"!=", tok_ne}, {"&&", tok_and_and}, {"||", tok_or_or},
	{"+=", tok_plus_assign}, {"-=", tok_minus_assign}, {"*=", tok_star_assign},
	{"/=", tok_slash_assign}, {"%=", tok_percent_assign}, {"&=", tok_amp_assign},
	{"|=", tok_pipe_assign}, {"^=", tok_caret_assign},
	{"(", tok_l_paren}, {")", tok_r_paren}, {"{", tok_l_brace}, {"}", tok_r_brace},
	{"[", tok_l_bracket}, {"]", tok_r_bracket}, {";", tok_semicolon}, {",", tok_comma},
	{".", tok_dot}, {"?", tok_question}, {":", tok_colon}, {"+", tok_plus},
	{"-", tok_minus}, {"*", tok_star}, {"/", tok_slash}, {"%", tok_percent},
	{"&", tok_amp}, {"|", tok_pipe}, {"^", tok_caret}, {"~", tok_tilde},
	{"!", tok_bang}, {"=", tok_assign}, {"<", tok_less}, {">", tok_greater}
};

static int is_identifier_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int is_identifier(char c)
{
	return is_identifier_start(c) || is_digit(c);
}

/* ---- token lists ---- */

static void token_list_init(cc_list* l)
{
	cc_list_init(l, (int)sizeof(cc_token));
}

static void token_list_add(cc_list* l, const cc_token* token)
{
	cc_token* slot = (cc_token*)cc_list_add(l);
	if (slot != NULL) *slot = *token;
}

static cc_token* token_at(const cc_list* l, int index)
{
	return (cc_token*)cc_list_at(l, index);
}

/* ---- the pass ---- */

static cc_scanner* current(cc_preprocessor* p)
{
	return (cc_scanner*)cc_list_at(&p->stack, p->stack.count - 1);
}

static int fail(cc_preprocessor* p, const char* message)
{
	if (p->out->error[0] == 0)
	{
		size_t length = strlen(message);
		if (length >= sizeof(p->out->error)) length = sizeof(p->out->error) - 1;
		memcpy(p->out->error, message, length);
		p->out->error[length] = 0;
		if (p->stack.count > 0)
		{
			p->out->error_file = current(p)->file;
			p->out->error_line = current(p)->line;
		}
	}
	return 0;
}

static cc_macro* macro_find(cc_preprocessor* p, int name)
{
	int index = -1;
	if (name == 0) return NULL;
	if (!cc_map_find(&p->by_name, name, &index) || index < 0) return NULL;
	return (cc_macro*)cc_list_at(&p->macros, index);
}

/* A macro is never removed from the table, only marked absent: #undef is rare
 * and an entry costs a pointer. */
static cc_macro* macro_define(cc_preprocessor* p, int name)
{
	int index = -1;
	cc_macro* entry;

	if (cc_map_find(&p->by_name, name, &index) && index >= 0)
	{
		entry = (cc_macro*)cc_list_at(&p->macros, index);
		cc_list_free(&entry->parameters);
		cc_list_free(&entry->body);
	}
	else
	{
		entry = (cc_macro*)cc_list_add(&p->macros);
		if (entry == NULL) return NULL;
		index = p->macros.count - 1;
		cc_map_set(&p->by_name, name, index);
	}

	memset(entry, 0, sizeof(*entry));
	entry->name = name;
	cc_list_init(&entry->parameters, (int)sizeof(int));
	token_list_init(&entry->body);
	return entry;
}

/* Returns 0 only for a comment that never ends, which would otherwise take the
 * rest of the file with it and leave a misleading error behind. */
static int skip_space(cc_preprocessor* p, int* at_line_start)
{
	cc_scanner* s = current(p);
	for (;;)
	{
		char c;
		if (s->position >= s->length) return 1;
		c = s->text[s->position];
		if (c == '\n')
		{
			++s->line;
			++s->position;
			*at_line_start = 1;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r')
		{
			++s->position;
			continue;
		}
		if (c == '\\')
		{
			/* A line continuation, which on a file with DOS line endings has a
			 * carriage return between the backslash and the newline. */
			size_t after = s->position + 1;
			if (after < s->length && s->text[after] == '\r') ++after;
			if (after < s->length && s->text[after] == '\n')
			{
				s->position = after + 1;
				++s->line;
				continue;
			}
		}
		if (c == '/' && s->position + 1 < s->length)
		{
			if (s->text[s->position + 1] == '/')
			{
				while (s->position < s->length && s->text[s->position] != '\n') ++s->position;
				continue;
			}
			if (s->text[s->position + 1] == '*')
			{
				const int opened = s->line;
				s->position += 2;
				while (s->position + 1 < s->length &&
				       !(s->text[s->position] == '*' && s->text[s->position + 1] == '/'))
				{
					if (s->text[s->position] == '\n') ++s->line;
					++s->position;
				}
				if (s->position + 1 >= s->length)
				{
					s->line = opened;
					return fail(p, "unterminated comment");
				}
				s->position += 2;
				continue;
			}
		}
		return 1;
	}
}

static char escape_of(char e)
{
	switch (e)
	{
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case '0': return '\0';
	case 'b': return '\b';
	case 'a': return '\a';
	case 'f': return '\f';
	case 'v': return '\v';
	case 'e': return (char)27;
	default: return e;
	}
}

static int intern_token_name(cc_preprocessor* p, cc_token* token, const char* text, int length)
{
	token->name = cc_intern(p->names, text, length);
	token->text = cc_name_text(p->names, token->name);
	token->length = length;
	return token->name != 0;
}

/* Reads one token from the source, without any macro expansion. */
static int next_raw(cc_preprocessor* p, cc_token* token, int* at_line_start, int* have)
{
	cc_scanner* s;
	char c;
	size_t i;

	*have = 0;
	if (!skip_space(p, at_line_start)) return 0;
	s = current(p);
	if (s->position >= s->length) return 1;

	memset(token, 0, sizeof(*token));
	token->line = s->line;
	token->file = s->file;
	c = s->text[s->position];

	if (is_identifier_start(c))
	{
		const size_t start = s->position;
		int keyword = 0;
		while (s->position < s->length && is_identifier(s->text[s->position])) ++s->position;
		if (!intern_token_name(p, token, s->text + start, (int)(s->position - start)))
			return fail(p, "out of memory");
		token->kind = tok_identifier;
		if (cc_map_find(&p->keywords, token->name, &keyword)) token->kind = (cc_tok)keyword;
		*have = 1;
		return 1;
	}

	if (is_digit(c) || (c == '.' && s->position + 1 < s->length && is_digit(s->text[s->position + 1])))
	{
		long value = 0;
		int base_ten = 0;

		/* A decimal run followed by a point or an exponent is a real, which
		 * this compiler only ever evaluates at compile time. */
		const int decimal = c != '0' || s->position + 1 >= s->length ||
			is_digit(s->text[s->position + 1]) || s->text[s->position + 1] == '.' ||
			s->text[s->position + 1] == 'e' || s->text[s->position + 1] == 'E';
		if (decimal || c == '.')
		{
			size_t scan = s->position;
			int real = 0;
			while (scan < s->length && is_digit(s->text[scan])) ++scan;
			if (scan < s->length && s->text[scan] == '.')
			{
				real = 1;
				++scan;
				while (scan < s->length && is_digit(s->text[scan])) ++scan;
			}
			if (scan < s->length && (s->text[scan] == 'e' || s->text[scan] == 'E'))
			{
				size_t exponent = scan + 1;
				if (exponent < s->length && (s->text[exponent] == '+' || s->text[exponent] == '-'))
					++exponent;
				if (exponent < s->length && is_digit(s->text[exponent]))
				{
					real = 1;
					scan = exponent;
					while (scan < s->length && is_digit(s->text[scan])) ++scan;
				}
			}
			if (real)
			{
				token->real = strtod(s->text + s->position, NULL);
				token->is_real = 1;
				s->position = scan;
				while (s->position < s->length && (s->text[s->position] == 'f' ||
				       s->text[s->position] == 'F' || s->text[s->position] == 'l' ||
				       s->text[s->position] == 'L')) ++s->position;
				token->kind = tok_number;
				*have = 1;
				return 1;
			}
		}

		if (c == '0' && s->position + 1 < s->length &&
		    (s->text[s->position + 1] == 'b' || s->text[s->position + 1] == 'B'))
		{
			s->position += 2;
			while (s->position < s->length && (s->text[s->position] == '0' || s->text[s->position] == '1'))
				value = value * 2 + (s->text[s->position++] - '0');
		}
		else if (c == '0' && s->position + 1 < s->length &&
		         (s->text[s->position + 1] == 'x' || s->text[s->position + 1] == 'X'))
		{
			s->position += 2;
			for (;;)
			{
				char d;
				int digit;
				if (s->position >= s->length) break;
				d = s->text[s->position];
				if (is_digit(d)) digit = d - '0';
				else if (d >= 'a' && d <= 'f') digit = d - 'a' + 10;
				else if (d >= 'A' && d <= 'F') digit = d - 'A' + 10;
				else break;
				value = value * 16 + digit;
				++s->position;
			}
		}
		else if (c == '0')
		{
			while (s->position < s->length && s->text[s->position] >= '0' && s->text[s->position] <= '7')
				value = value * 8 + (s->text[s->position++] - '0');
		}
		else
		{
			base_ten = 1;
			while (s->position < s->length && is_digit(s->text[s->position]))
				value = value * 10 + (s->text[s->position++] - '0');
		}

		while (s->position < s->length && (s->text[s->position] == 'u' || s->text[s->position] == 'U' ||
		       s->text[s->position] == 'l' || s->text[s->position] == 'L'))
		{
			const char suffix = s->text[s->position++];
			if (suffix == 'u' || suffix == 'U') token->unsigned_literal = 1;
			else token->wide = 1;
		}
		token->kind = tok_number;
		token->value = value;
		if (value > 65535 || value < -32768) token->wide = 1;
		/* A decimal literal is never unsigned unless it says so, so one that
		 * does not fit in an int is a long, and one with a U suffix that fits
		 * in 16 bits is an unsigned int. A hex or octal one may be an
		 * unsigned int first, which is what makes 0xFFFF mean 65535. */
		else if (base_ten && value > 32767 && !token->unsigned_literal) token->wide = 1;
		*have = 1;
		return 1;
	}

	if (c == '"')
	{
		size_t scan = s->position + 1;
		char* value;
		int written = 0;

		while (scan < s->length && s->text[scan] != '"')
		{
			if (s->text[scan] == '\\' && scan + 1 < s->length) ++scan;
			++scan;
		}
		if (scan >= s->length) return fail(p, "unterminated string");

		value = (char*)cc_arena_alloc(p->arena, scan - s->position);
		if (value == NULL) return fail(p, "out of memory");

		++s->position;
		while (s->position < s->length && s->text[s->position] != '"')
		{
			char d = s->text[s->position++];
			if (d == '\\' && s->position < s->length) d = escape_of(s->text[s->position++]);
			value[written++] = d;
		}
		++s->position;
		value[written] = 0;
		token->kind = tok_string;
		token->text = value;
		token->length = written;
		*have = 1;
		return 1;
	}

	if (c == '\'')
	{
		long value = 0;
		++s->position;
		while (s->position < s->length && s->text[s->position] != '\'')
		{
			char d = s->text[s->position++];
			if (d == '\\' && s->position < s->length) d = escape_of(s->text[s->position++]);
			value = d;
		}
		if (s->position >= s->length) return fail(p, "unterminated character constant");
		++s->position;
		token->kind = tok_character;
		token->value = value;
		*have = 1;
		return 1;
	}

	for (i = 0; i < sizeof(cc_punctuators) / sizeof(cc_punctuators[0]); ++i)
	{
		const size_t length = strlen(cc_punctuators[i].text);
		if (s->position + length <= s->length &&
		    strncmp(s->text + s->position, cc_punctuators[i].text, length) == 0)
		{
			s->position += length;
			token->kind = cc_punctuators[i].kind;
			*have = 1;
			return 1;
		}
	}

	if (c == '#')
	{
		++s->position;
		token->kind = tok_identifier;
		token->name = p->n_hash;
		token->text = cc_name_text(p->names, p->n_hash);
		token->length = 1;
		*have = 1;
		return 1;
	}

	return fail(p, "unexpected character");
}

static int read_line_tokens(cc_preprocessor* p, cc_list* out)
{
	for (;;)
	{
		cc_scanner* s = current(p);
		const size_t before = s->position;
		cc_token token;
		int at_line_start = 0;
		int have = 0;
		if (!next_raw(p, &token, &at_line_start, &have)) return 0;
		if (!have) return 1;
		if (at_line_start)
		{
			/* The token belongs to the next line; leave it there. */
			current(p)->position = before;
			return 1;
		}
		token_list_add(out, &token);
	}
}

static int include_file(cc_preprocessor* p)
{
	cc_scanner* s = current(p);
	char open;
	char close;
	char name[260];
	int length = 0;
	const void* bytes = NULL;
	size_t size = 0;
	char* text;
	size_t text_length = 0;
	cc_scanner* included;
	const char* file;

	while (s->position < s->length && (s->text[s->position] == ' ' || s->text[s->position] == '\t'))
		++s->position;
	if (s->position >= s->length) return fail(p, "#include needs a file name");

	open = s->text[s->position];
	close = open == '<' ? '>' : '"';
	if (open != '<' && open != '"') return fail(p, "#include needs a file name");
	++s->position;
	while (s->position < s->length && s->text[s->position] != close)
	{
		if (length + 1 < (int)sizeof(name)) name[length++] = s->text[s->position];
		++s->position;
	}
	name[length] = 0;
	if (s->position >= s->length) return fail(p, "unterminated #include");
	++s->position;

	if (p->sources == NULL || p->sources->load == NULL ||
	    !p->sources->load(p->sources, name, &bytes, &size))
		return fail(p, "cannot find the included file");
	if (p->stack.count > 16) return fail(p, "#include nested too deeply");

	text = cc_normalise_source(p->arena, bytes, size, &text_length);
	if (text == NULL) return fail(p, "out of memory");
	file = cc_name_text(p->names, cc_intern(p->names, name, length));

	included = (cc_scanner*)cc_list_add(&p->stack);
	if (included == NULL) return fail(p, "out of memory");
	included->text = text;
	included->length = text_length;
	included->position = 0;
	included->line = 1;
	included->file = file;
	return 1;
}

static int define_directive(cc_preprocessor* p)
{
	cc_token name;
	int at_line_start = 0;
	int have = 0;
	cc_macro* entry;
	cc_scanner* s;
	int function_like = 0;
	cc_list parameters;

	if (!next_raw(p, &name, &at_line_start, &have)) return 0;
	/* A keyword is a legal macro name: a source that maps inline onto its
	 * compiler's spelling of it is doing exactly that. */
	if (!have || name.name == 0) return fail(p, "#define needs a name");

	cc_list_init(&parameters, (int)sizeof(int));
	s = current(p);

	/* A '(' with no space before it makes this a function-like macro. */
	if (s->position < s->length && s->text[s->position] == '(')
	{
		function_like = 1;
		++s->position;
		for (;;)
		{
			cc_token parameter;
			if (!next_raw(p, &parameter, &at_line_start, &have))
			{
				cc_list_free(&parameters);
				return 0;
			}
			if (!have)
			{
				cc_list_free(&parameters);
				return fail(p, "unterminated macro parameter list");
			}
			if (parameter.kind == tok_r_paren) break;
			if (parameter.kind == tok_comma) continue;
			if (parameter.kind != tok_identifier)
			{
				cc_list_free(&parameters);
				return fail(p, "bad macro parameter");
			}
			*(int*)cc_list_add(&parameters) = parameter.name;
		}
	}

	entry = macro_define(p, name.name);
	if (entry == NULL)
	{
		cc_list_free(&parameters);
		return fail(p, "out of memory");
	}
	cc_list_free(&entry->parameters);
	entry->parameters = parameters;
	entry->function_like = function_like;
	return read_line_tokens(p, &entry->body);
}

/* Evaluates a #if expression. Only what the shell and its headers need is
 * supported: constants, defined(), the comparison and logical operators. */
static long evaluate_condition(cc_preprocessor* p, const cc_list* tokens, int* at, int precedence, int* ok)
{
	long left = 0;
	const cc_token* first;

	if (*at >= tokens->count)
	{
		*ok = 0;
		return 0;
	}

	first = token_at(tokens, *at);
	if (first->kind == tok_bang)
	{
		++*at;
		left = evaluate_condition(p, tokens, at, 12, ok) == 0 ? 1 : 0;
	}
	else if (first->kind == tok_minus)
	{
		++*at;
		left = -evaluate_condition(p, tokens, at, 12, ok);
	}
	else if (first->kind == tok_l_paren)
	{
		++*at;
		left = evaluate_condition(p, tokens, at, 0, ok);
		if (*at < tokens->count && token_at(tokens, *at)->kind == tok_r_paren) ++*at;
		else *ok = 0;
	}
	else if (first->kind == tok_identifier && first->name == p->n_defined)
	{
		int parenthesised = 0;
		++*at;
		if (*at < tokens->count && token_at(tokens, *at)->kind == tok_l_paren)
		{
			parenthesised = 1;
			++*at;
		}
		if (*at >= tokens->count)
		{
			*ok = 0;
			return 0;
		}
		left = macro_find(p, token_at(tokens, *at)->name) != NULL ? 1 : 0;
		++*at;
		if (parenthesised)
		{
			if (*at < tokens->count && token_at(tokens, *at)->kind == tok_r_paren) ++*at;
			else *ok = 0;
		}
	}
	else
	{
		/* An undefined identifier is zero, as C says. */
		if (first->kind == tok_number || first->kind == tok_character) left = first->value;
		++*at;
	}

	for (;;)
	{
		cc_tok kind;
		int level;
		long right;

		if (*at >= tokens->count) return left;
		kind = token_at(tokens, *at)->kind;
		switch (kind)
		{
		case tok_or_or: level = 1; break;
		case tok_and_and: level = 2; break;
		case tok_pipe: level = 3; break;
		case tok_caret: level = 4; break;
		case tok_amp: level = 5; break;
		case tok_eq: case tok_ne: level = 6; break;
		case tok_less: case tok_greater: case tok_le: case tok_ge: level = 7; break;
		case tok_plus: case tok_minus: level = 9; break;
		case tok_star: case tok_slash: case tok_percent: level = 10; break;
		default: return left;
		}
		if (level <= precedence) return left;
		++*at;
		right = evaluate_condition(p, tokens, at, level, ok);
		switch (kind)
		{
		case tok_or_or: left = (left || right) ? 1 : 0; break;
		case tok_and_and: left = (left && right) ? 1 : 0; break;
		case tok_pipe: left |= right; break;
		case tok_caret: left ^= right; break;
		case tok_amp: left &= right; break;
		case tok_eq: left = left == right; break;
		case tok_ne: left = left != right; break;
		case tok_less: left = left < right; break;
		case tok_greater: left = left > right; break;
		case tok_le: left = left <= right; break;
		case tok_ge: left = left >= right; break;
		case tok_plus: left += right; break;
		case tok_minus: left -= right; break;
		case tok_star: left *= right; break;
		case tok_slash: left = right != 0 ? left / right : 0; break;
		default: left = right != 0 ? left % right : 0; break;
		}
	}
}

/* Skips a conditional branch, stopping *before* the directive that ends it so
 * the normal handler deals with the #else, #elif or #endif. */
static int skip_conditional_block(cc_preprocessor* p, int stop_at_else)
{
	int depth = 0;
	for (;;)
	{
		cc_scanner* s = current(p);
		size_t before;
		cc_token token;
		cc_token name;
		int at_line_start = 0;
		int have = 0;

		if (s->position >= s->length) return fail(p, "unterminated #if");
		before = s->position;
		if (!next_raw(p, &token, &at_line_start, &have)) return 0;
		if (!have) return fail(p, "unterminated #if");
		if (token.kind != tok_identifier || token.name != p->n_hash) continue;

		if (!next_raw(p, &name, &at_line_start, &have)) return 0;
		if (!have) continue;

		if (name.name == p->n_if || name.name == p->n_ifdef || name.name == p->n_ifndef)
		{
			++depth;
			continue;
		}
		if (name.name == p->n_endif)
		{
			if (depth == 0)
			{
				current(p)->position = before;
				return 1;
			}
			--depth;
			continue;
		}
		if (depth == 0 && stop_at_else && (name.name == p->n_else || name.name == p->n_elif))
		{
			current(p)->position = before;
			return 1;
		}
	}
}

static void expand_object_macros(cc_preprocessor* p, cc_list* tokens, int protect_defined);

/* Replaces macros throughout a token list. A function-like one is expanded only
 * when its argument list is already in the list, which is how one written
 * inside another macro's body arrives here. */
static void expand_object_macros(cc_preprocessor* p, cc_list* tokens, int protect_defined)
{
	int pass;
	for (pass = 0; pass < 16; ++pass)
	{
		cc_list expanded;
		int changed = 0;
		int i;

		token_list_init(&expanded);
		for (i = 0; i < tokens->count; ++i)
		{
			cc_token* token = token_at(tokens, i);
			cc_macro* found;
			int index;

			/* The operand of defined is a name, not a value: expanding it
			 * would lose the question being asked. */
			if (protect_defined && token->kind == tok_identifier && token->name == p->n_defined)
			{
				const int span = i + 1 < tokens->count &&
					token_at(tokens, i + 1)->kind == tok_l_paren ? 3 : 1;
				int j;
				for (j = 0; j <= span && i + j < tokens->count; ++j)
					token_list_add(&expanded, token_at(tokens, i + j));
				i += span;
				continue;
			}

			found = token->kind == tok_identifier ? macro_find(p, token->name) : NULL;

			if (found != NULL && found->function_like && !found->expanding &&
			    i + 1 < tokens->count && token_at(tokens, i + 1)->kind == tok_l_paren)
			{
				cc_list arguments; /* cc_list, one per argument */
				cc_list* argument;
				int scan = i + 2;
				int depth = 0;
				int closed = 0;
				int matched;

				cc_list_init(&arguments, (int)sizeof(cc_list));
				argument = (cc_list*)cc_list_add(&arguments);
				token_list_init(argument);

				for (; scan < tokens->count; ++scan)
				{
					const cc_tok k = token_at(tokens, scan)->kind;
					if (k == tok_r_paren && depth == 0)
					{
						closed = 1;
						break;
					}
					if (k == tok_l_paren) ++depth;
					else if (k == tok_r_paren) --depth;
					else if (k == tok_comma && depth == 0)
					{
						argument = (cc_list*)cc_list_add(&arguments);
						token_list_init(argument);
						continue;
					}
					argument = (cc_list*)cc_list_at(&arguments, arguments.count - 1);
					token_list_add(argument, token_at(tokens, scan));
				}

				if (arguments.count == 1 && ((cc_list*)cc_list_at(&arguments, 0))->count == 0 &&
				    found->parameters.count == 0)
				{
					cc_list_free((cc_list*)cc_list_at(&arguments, 0));
					arguments.count = 0;
				}

				matched = closed && arguments.count == found->parameters.count;
				if (matched)
				{
					int b;
					found->expanding = 1;
					for (index = 0; index < arguments.count; ++index)
						expand_object_macros(p, (cc_list*)cc_list_at(&arguments, index), protect_defined);

					for (b = 0; b < found->body.count; ++b)
					{
						cc_token body = *token_at(&found->body, b);
						int substituted = 0;
						if (body.kind == tok_identifier)
						{
							int q;
							for (q = 0; q < found->parameters.count; ++q)
							{
								cc_list* source;
								int a;
								if (CC_AT(found->parameters, int, q) != body.name) continue;
								source = (cc_list*)cc_list_at(&arguments, q);
								for (a = 0; a < source->count; ++a)
								{
									cc_token argument = *token_at(source, a);
									argument.line = token->line;
									argument.file = token->file;
									token_list_add(&expanded, &argument);
								}
								substituted = 1;
								break;
							}
						}
						if (!substituted)
						{
							body.line = token->line;
							body.file = token->file;
							token_list_add(&expanded, &body);
						}
						/* The body list may have moved if a nested expansion
						 * touched it, so it is re-read every time. */
						token = token_at(tokens, i);
					}
					found->expanding = 0;
					i = scan;
					changed = 1;
				}

				for (index = 0; index < arguments.count; ++index)
					cc_list_free((cc_list*)cc_list_at(&arguments, index));
				cc_list_free(&arguments);
				if (matched) continue;
			}

			if (found == NULL || found->function_like || found->expanding)
			{
				token_list_add(&expanded, token_at(tokens, i));
				continue;
			}
			for (index = 0; index < found->body.count; ++index)
			{
				cc_token body = *token_at(&found->body, index);
				body.line = token->line;
				body.file = token->file;
				token_list_add(&expanded, &body);
			}
			changed = 1;
		}

		cc_list_free(tokens);
		*tokens = expanded;
		if (!changed) return;
	}
}

/* Reads the expression of a #if or #elif and replaces the macros in it. */
static int read_condition(cc_preprocessor* p, cc_list* line)
{
	if (!read_line_tokens(p, line)) return 0;
	expand_object_macros(p, line, 1);
	return 1;
}

static int conditional(cc_preprocessor* p, int name)
{
	int taken = 0;
	if (name == p->n_ifdef || name == p->n_ifndef)
	{
		cc_token identifier;
		int at_line_start = 0;
		int have = 0;
		int defined;
		if (!next_raw(p, &identifier, &at_line_start, &have)) return 0;
		if (!have) return fail(p, "#ifdef needs a name");
		defined = macro_find(p, identifier.name) != NULL;
		taken = name == p->n_ifdef ? defined : !defined;
	}
	else
	{
		cc_list line;
		int at = 0;
		int ok = 1;
		token_list_init(&line);
		if (!read_condition(p, &line))
		{
			cc_list_free(&line);
			return 0;
		}
		taken = evaluate_condition(p, &line, &at, 0, &ok) != 0;
		cc_list_free(&line);
		if (!ok) return fail(p, "bad #if expression");
	}

	*(int*)cc_list_add(&p->conditions) = taken;
	if (taken) return 1;
	return skip_conditional_block(p, 1);
}

static int* condition_top(cc_preprocessor* p)
{
	return (int*)cc_list_at(&p->conditions, p->conditions.count - 1);
}

static int directive(cc_preprocessor* p)
{
	cc_token name;
	int at_line_start = 0;
	int have = 0;

	if (!next_raw(p, &name, &at_line_start, &have)) return 0;
	if (!have) return 1;

	if (name.name == p->n_include) return include_file(p);
	if (name.name == p->n_define) return define_directive(p);
	if (name.name == p->n_undef)
	{
		cc_token identifier;
		if (!next_raw(p, &identifier, &at_line_start, &have)) return 0;
		if (have && identifier.name != 0) cc_map_set(&p->by_name, identifier.name, -1);
		return 1;
	}
	if (name.name == p->n_ifdef || name.name == p->n_ifndef || name.name == p->n_if)
		return conditional(p, name.name);
	if (name.name == p->n_else)
	{
		if (p->conditions.count == 0) return fail(p, "#else without #if");
		if (*condition_top(p)) return skip_conditional_block(p, 0);
		*condition_top(p) = 1;
		return 1;
	}
	if (name.name == p->n_elif)
	{
		cc_list line;
		int at = 0;
		int ok = 1;
		int taken;

		if (p->conditions.count == 0) return fail(p, "#elif without #if");
		if (*condition_top(p)) return skip_conditional_block(p, 0);

		token_list_init(&line);
		if (!read_condition(p, &line))
		{
			cc_list_free(&line);
			return 0;
		}
		taken = evaluate_condition(p, &line, &at, 0, &ok) != 0;
		cc_list_free(&line);
		if (!ok) return fail(p, "bad #elif expression");
		if (taken)
		{
			*condition_top(p) = 1;
			return 1;
		}
		return skip_conditional_block(p, 1);
	}
	if (name.name == p->n_endif)
	{
		if (p->conditions.count == 0) return fail(p, "#endif without #if");
		p->conditions.count -= 1;
		return 1;
	}
	if (name.name == p->n_error) return fail(p, "#error");
	if (name.name == p->n_pragma || name.name == p->n_line)
	{
		cc_list ignored;
		int ok;
		token_list_init(&ignored);
		ok = read_line_tokens(p, &ignored);
		cc_list_free(&ignored);
		return ok;
	}
	return fail(p, "unknown preprocessor directive");
}

static int expand(cc_preprocessor* p, const cc_token* name, cc_list* out)
{
	cc_macro* entry = macro_find(p, name->name);
	cc_list arguments; /* cc_list, one per argument */
	int have_arguments = 0;
	int start;
	int b;
	int index;
	cc_list tail;

	if (entry == NULL || entry->expanding)
	{
		token_list_add(out, name);
		return 1;
	}

	cc_list_init(&arguments, (int)sizeof(cc_list));

	if (entry->function_like)
	{
		/* Only expand when a '(' actually follows. */
		cc_scanner* s = current(p);
		const size_t saved = s->position;
		const int saved_line = s->line;
		int at_line_start = 0;
		int have = 0;
		int depth = 0;
		cc_token token;
		cc_list* argument;

		if (!next_raw(p, &token, &at_line_start, &have)) return 0;
		if (!have || token.kind != tok_l_paren)
		{
			s = current(p);
			s->position = saved;
			s->line = saved_line;
			token_list_add(out, name);
			cc_list_free(&arguments);
			return 1;
		}

		argument = (cc_list*)cc_list_add(&arguments);
		token_list_init(argument);
		for (;;)
		{
			if (!next_raw(p, &token, &at_line_start, &have))
			{
				for (index = 0; index < arguments.count; ++index)
					cc_list_free((cc_list*)cc_list_at(&arguments, index));
				cc_list_free(&arguments);
				return 0;
			}
			if (!have)
			{
				for (index = 0; index < arguments.count; ++index)
					cc_list_free((cc_list*)cc_list_at(&arguments, index));
				cc_list_free(&arguments);
				return fail(p, "unterminated macro argument list");
			}
			if (token.kind == tok_l_paren) ++depth;
			if (token.kind == tok_r_paren)
			{
				if (depth == 0) break;
				--depth;
			}
			if (token.kind == tok_comma && depth == 0)
			{
				argument = (cc_list*)cc_list_add(&arguments);
				token_list_init(argument);
				continue;
			}
			argument = (cc_list*)cc_list_at(&arguments, arguments.count - 1);
			token_list_add(argument, &token);
		}

		if (arguments.count == 1 && ((cc_list*)cc_list_at(&arguments, 0))->count == 0 &&
		    entry->parameters.count == 0)
		{
			cc_list_free((cc_list*)cc_list_at(&arguments, 0));
			arguments.count = 0;
		}
		if (arguments.count != entry->parameters.count)
		{
			for (index = 0; index < arguments.count; ++index)
				cc_list_free((cc_list*)cc_list_at(&arguments, index));
			cc_list_free(&arguments);
			return fail(p, "wrong number of macro arguments");
		}
		/* An argument is expanded before it is substituted, so a constant
		 * passed to a macro is a number by the time the parser sees it. */
		for (index = 0; index < arguments.count; ++index)
			expand_object_macros(p, (cc_list*)cc_list_at(&arguments, index), 0);
		have_arguments = 1;
	}

	entry->expanding = 1;
	start = out->count;
	for (b = 0; b < entry->body.count; ++b)
	{
		cc_token body = *token_at(&entry->body, b);
		int substituted = 0;
		if (body.kind == tok_identifier)
		{
			int q;
			for (q = 0; q < entry->parameters.count; ++q)
			{
				cc_list* source;
				int a;
				if (CC_AT(entry->parameters, int, q) != body.name) continue;
				source = (cc_list*)cc_list_at(&arguments, q);
				for (a = 0; a < source->count; ++a)
				{
					cc_token argument = *token_at(source, a);
					argument.line = name->line;
					argument.file = name->file;
					token_list_add(out, &argument);
				}
				substituted = 1;
				break;
			}
			if (!substituted && macro_find(p, body.name) != NULL)
			{
				/* Nested object-like expansion; the guard flag stops recursion. */
				cc_token nested = body;
				nested.line = name->line;
				nested.file = name->file;
				if (!expand(p, &nested, out))
				{
					entry = macro_find(p, name->name);
					if (entry != NULL) entry->expanding = 0;
					for (index = 0; index < arguments.count; ++index)
						cc_list_free((cc_list*)cc_list_at(&arguments, index));
					cc_list_free(&arguments);
					return 0;
				}
				substituted = 1;
				entry = macro_find(p, name->name);
			}
		}
		if (!substituted)
		{
			body.line = name->line;
			body.file = name->file;
			token_list_add(out, &body);
		}
	}

	/* A function-like macro written inside this body brings its argument list
	 * with it, so it can only be expanded once the body is laid out. */
	token_list_init(&tail);
	for (index = start; index < out->count; ++index) token_list_add(&tail, token_at(out, index));
	expand_object_macros(p, &tail, 0);
	out->count = start;
	for (index = 0; index < tail.count; ++index) token_list_add(out, token_at(&tail, index));
	cc_list_free(&tail);

	entry->expanding = 0;
	if (have_arguments || arguments.count > 0)
	{
		for (index = 0; index < arguments.count; ++index)
			cc_list_free((cc_list*)cc_list_at(&arguments, index));
	}
	cc_list_free(&arguments);
	return 1;
}

static int intern_word(cc_preprocessor* p, const char* text)
{
	return cc_intern(p->names, text, -1);
}

static int run(cc_preprocessor* p, const char* text, const char* name,
               const char* const* defines, int define_count)
{
	cc_scanner* first;
	cc_macro* entry;
	char* normalised;
	size_t length = 0;
	int i;
	int kept;
	int end_line = 1;
	const char* end_file = NULL;

	normalised = cc_normalise_source(p->arena, text, strlen(text), &length);
	if (normalised == NULL) return fail(p, "out of memory");

	first = (cc_scanner*)cc_list_add(&p->stack);
	if (first == NULL) return fail(p, "out of memory");
	first->text = normalised;
	first->length = length;
	first->position = 0;
	first->line = 1;
	first->file = cc_name_text(p->names, intern_word(p, name));
	end_file = first->file;

	macro_define(p, intern_word(p, "__LIBDOS__"));
	macro_define(p, intern_word(p, "__DOS__"));
	macro_define(p, intern_word(p, "_M_I86")); /* the identity a 16-bit real-mode source expects */

	/* A GCC attribute says nothing this compiler can act on, and dropping it
	 * here saves the parser from having to know the syntax at all. */
	entry = macro_define(p, intern_word(p, "__attribute__"));
	if (entry == NULL) return fail(p, "out of memory");
	entry->function_like = 1;
	*(int*)cc_list_add(&entry->parameters) = intern_word(p, "ignored");

	/* A command-line define is scanned as if it were the body of a #define. */
	for (i = 0; i < define_count; ++i)
	{
		const char* option = defines[i];
		const char* equals = strchr(option, '=');
		const int name_length = equals != NULL ? (int)(equals - option) : (int)strlen(option);
		cc_list body;
		int ok = 1;

		token_list_init(&body);
		if (equals != NULL)
		{
			cc_scanner* value = (cc_scanner*)cc_list_add(&p->stack);
			if (value == NULL) return fail(p, "out of memory");
			value->text = equals + 1;
			value->length = strlen(equals + 1);
			value->position = 0;
			value->line = 1;
			value->file = ((cc_scanner*)cc_list_at(&p->stack, 0))->file;
			ok = read_line_tokens(p, &body);
			p->stack.count -= 1;
		}
		if (!ok)
		{
			cc_list_free(&body);
			return 0;
		}

		entry = macro_define(p, cc_intern(p->names, option, name_length));
		if (entry == NULL)
		{
			cc_list_free(&body);
			return fail(p, "out of memory");
		}
		cc_list_free(&entry->body);
		entry->body = body;
	}

	while (p->stack.count > 0)
	{
		cc_token token;
		int at_line_start = 0;
		int have = 0;

		if (current(p)->position >= current(p)->length)
		{
			/* The end of the input is the end of the main file: an error
			 * found there is reported on its last line, and a conditional
			 * still open there was never closed. */
			if (p->stack.count == 1)
			{
				cc_scanner* last = current(p);
				end_line = last->line;
				if (last->length > 0 && last->text[last->length - 1] == '\n' && end_line > 1) --end_line;
				end_file = last->file;
				if (p->conditions.count > 0)
				{
					last->line = end_line;
					return fail(p, "unterminated #if");
				}
			}
			p->stack.count -= 1;
			continue;
		}

		if (!next_raw(p, &token, &at_line_start, &have)) return 0;
		if (!have) continue;

		if (token.kind == tok_identifier && token.name == p->n_hash)
		{
			if (!directive(p)) return 0;
			continue;
		}

		if (token.kind == tok_identifier)
		{
			if (!expand(p, &token, &p->out->tokens)) return 0;
			continue;
		}
		token_list_add(&p->out->tokens, &token);
	}

	{
		cc_token end;
		memset(&end, 0, sizeof(end));
		end.kind = tok_end;
		end.line = end_line;
		end.file = end_file;
		token_list_add(&p->out->tokens, &end);
	}

	/* Adjacent string literals are one string. Joining them here rather than in
	 * the scanner also catches the ones a macro brought together. */
	kept = 0;
	for (i = 0; i < p->out->tokens.count; ++i)
	{
		cc_token* token = token_at(&p->out->tokens, i);
		cc_token* previous = kept > 0 ? token_at(&p->out->tokens, kept - 1) : NULL;
		if (previous != NULL && token->kind == tok_string && previous->kind == tok_string)
		{
			char* joined = (char*)cc_arena_alloc(p->arena, (size_t)previous->length +
			                                     (size_t)token->length + 1);
			if (joined == NULL) return fail(p, "out of memory");
			memcpy(joined, previous->text, (size_t)previous->length);
			memcpy(joined + previous->length, token->text, (size_t)token->length);
			joined[previous->length + token->length] = 0;
			previous->text = joined;
			previous->length += token->length;
			continue;
		}
		*token_at(&p->out->tokens, kept) = *token;
		++kept;
	}
	p->out->tokens.count = kept;
	return 1;
}

void cc_token_stream_init(cc_token_stream* s, cc_arena* arena, cc_names* names)
{
	memset(s, 0, sizeof(*s));
	s->arena = arena;
	s->names = names;
	token_list_init(&s->tokens);
	s->error_file = "";
}

void cc_token_stream_free(cc_token_stream* s)
{
	cc_list_free(&s->tokens);
}

int cc_tokenise(cc_token_stream* out, const char* text, const char* name, cc_sources* sources,
                const char* const* defines, int define_count)
{
	cc_preprocessor p;
	int ok;
	int i;

	memset(&p, 0, sizeof(p));
	p.sources = sources;
	p.out = out;
	p.arena = out->arena;
	p.names = out->names;
	cc_list_init(&p.stack, (int)sizeof(cc_scanner));
	cc_list_init(&p.macros, (int)sizeof(cc_macro));
	cc_map_init(&p.by_name);
	cc_list_init(&p.conditions, (int)sizeof(int));
	cc_map_init(&p.keywords);

	for (i = 0; i < (int)(sizeof(cc_keywords) / sizeof(cc_keywords[0])); ++i)
		cc_map_set(&p.keywords, intern_word(&p, cc_keywords[i].name), (int)cc_keywords[i].kind);

	p.n_hash = intern_word(&p, "#");
	p.n_include = intern_word(&p, "include");
	p.n_define = intern_word(&p, "define");
	p.n_undef = intern_word(&p, "undef");
	p.n_if = intern_word(&p, "if");
	p.n_ifdef = intern_word(&p, "ifdef");
	p.n_ifndef = intern_word(&p, "ifndef");
	p.n_else = intern_word(&p, "else");
	p.n_elif = intern_word(&p, "elif");
	p.n_endif = intern_word(&p, "endif");
	p.n_error = intern_word(&p, "error");
	p.n_pragma = intern_word(&p, "pragma");
	p.n_line = intern_word(&p, "line");
	p.n_defined = intern_word(&p, "defined");
	p.n_args = intern_word(&p, "__args");

	ok = run(&p, text, name, defines, define_count);

	for (i = 0; i < p.macros.count; ++i)
	{
		cc_macro* entry = (cc_macro*)cc_list_at(&p.macros, i);
		cc_list_free(&entry->parameters);
		cc_list_free(&entry->body);
	}
	cc_list_free(&p.macros);
	cc_map_free(&p.by_name);
	cc_list_free(&p.stack);
	cc_list_free(&p.conditions);
	cc_map_free(&p.keywords);
	return ok;
}
