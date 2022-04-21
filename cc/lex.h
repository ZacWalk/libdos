#pragma once

// ovrwin by Zac Walker
//
// Tokeniser and preprocessor. The whole translation unit is turned into one
// token vector with includes inlined and macros expanded, which the parser
// then walks; that costs a little memory and saves a great deal of plumbing.

#include <string>
#include <vector>

class cc_sources;

enum class tok
{
	end, identifier, number, string, character,

	kw_int, kw_char, kw_void, kw_unsigned, kw_signed, kw_short, kw_long,
	kw_if, kw_else, kw_while, kw_for, kw_do, kw_return, kw_break, kw_continue,
	kw_switch, kw_case, kw_default, kw_struct, kw_union, kw_enum, kw_typedef,
	kw_sizeof, kw_static, kw_extern, kw_const, kw_goto, kw_qualifier,

	l_paren, r_paren, l_brace, r_brace, l_bracket, r_bracket,
	semicolon, comma, dot, arrow, question, colon,
	plus, minus, star, slash, percent, amp, pipe, caret, tilde, bang, assign,
	less, greater, inc, dec, shl, shr, le, ge, eq, ne, and_and, or_or,
	plus_assign, minus_assign, star_assign, slash_assign, percent_assign,
	amp_assign, pipe_assign, caret_assign, shl_assign, shr_assign, ellipsis,
};

struct cc_token
{
	tok kind = tok::end;
	std::string text; // identifier name, or the contents of a string literal
	long value = 0; // numeric or character constant
	double real = 0; // the value when is_real, which only a constant may be
	bool is_real = false;
	bool wide = false; // the literal carries an L suffix or does not fit in 16 bits
	bool unsigned_literal = false; // the literal carries a U suffix
	int line = 0;
	int file = 0; // index into the file-name table
};

struct cc_token_stream
{
	std::vector<cc_token> tokens;
	std::vector<std::string> files;
	std::string error;
	std::string error_file;
	int error_line = 0;
};

bool cc_tokenise(const char* text, const char* name, cc_sources& sources, cc_token_stream& out,
                 const std::vector<std::string>& defines = {});
