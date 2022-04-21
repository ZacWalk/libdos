// ovrwin by Zac Walker
//
// Tokeniser and preprocessor.

#include "lex.h"
#include "cc.h"

#include <cstdlib>
#include <cstring>
#include <map>

namespace
{
	struct macro
	{
		bool function_like = false;
		std::vector<std::string> parameters;
		std::vector<cc_token> body;
		bool expanding = false; // guards against a macro expanding itself
	};

	struct keyword_entry
	{
		const char* name;
		tok kind;
	};

	const keyword_entry keywords[] = {
		{"int", tok::kw_int}, {"char", tok::kw_char}, {"void", tok::kw_void},
		{"unsigned", tok::kw_unsigned}, {"signed", tok::kw_signed}, {"short", tok::kw_short},
		{"long", tok::kw_long}, {"if", tok::kw_if}, {"else", tok::kw_else},
		{"while", tok::kw_while}, {"for", tok::kw_for}, {"do", tok::kw_do},
		{"return", tok::kw_return}, {"break", tok::kw_break}, {"continue", tok::kw_continue},
		{"switch", tok::kw_switch}, {"case", tok::kw_case}, {"default", tok::kw_default},
		{"struct", tok::kw_struct}, {"union", tok::kw_union}, {"enum", tok::kw_enum},
		{"typedef", tok::kw_typedef}, {"sizeof", tok::kw_sizeof}, {"static", tok::kw_static},
		{"extern", tok::kw_extern}, {"const", tok::kw_const}, {"goto", tok::kw_goto},
		// Qualifiers the parser accepts and ignores. The memory-model ones are
		// meaningless while the compiler emits only the tiny model.
		{"volatile", tok::kw_qualifier}, {"register", tok::kw_qualifier},
		{"inline", tok::kw_qualifier}, {"__inline", tok::kw_qualifier},
		{"_Noreturn", tok::kw_qualifier}, {"restrict", tok::kw_qualifier},
		{"__cdecl", tok::kw_qualifier}, {"__interrupt", tok::kw_qualifier},
		{"__far", tok::kw_qualifier}, {"__near", tok::kw_qualifier}, {"__huge", tok::kw_qualifier},
		{"_far", tok::kw_qualifier}, {"_near", tok::kw_qualifier}, {"_huge", tok::kw_qualifier},
	};

	bool is_identifier_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
	bool is_digit(char c) { return c >= '0' && c <= '9'; }
	bool is_identifier(char c) { return is_identifier_start(c) || is_digit(c); }

	// One source file being scanned. Includes push another of these.
	struct scanner
	{
		std::string text;
		size_t position = 0;
		int line = 1;
		int file = 0;
	};

	class preprocessor
	{
	public:
		preprocessor(cc_sources& sources, cc_token_stream& out) : sources_(sources), out_(out)
		{
		}

		bool run(const char* text, const char* name, const std::vector<std::string>& defines);

	private:
		cc_sources& sources_;
		cc_token_stream& out_;
		std::vector<scanner> stack_;
		std::map<std::string, macro> macros_;
		std::string error_;

		scanner& current() { return stack_.back(); }

		bool fail(const char* message)
		{
			if (out_.error.empty())
			{
				out_.error = message;
				if (!stack_.empty())
				{
					out_.error_file = out_.files[size_t(current().file)];
					out_.error_line = current().line;
				}
			}
			return false;
		}

		int file_index(const char* name)
		{
			for (size_t i = 0; i < out_.files.size(); ++i) if (out_.files[i] == name) return int(i);
			out_.files.push_back(name);
			return int(out_.files.size()) - 1;
		}

		void skip_space(bool& at_line_start);
		bool next_raw(cc_token& token, bool& at_line_start, bool& have);
		bool directive();
		bool include_file();
		bool define();
		bool conditional(const std::string& name);
		bool read_condition(std::vector<cc_token>& out);
		void expand_object_macros(std::vector<cc_token>& tokens, bool protect_defined);
		bool skip_conditional_block(bool stop_at_else);
		bool expand(const cc_token& name, std::vector<cc_token>& out);
		bool read_line_tokens(std::vector<cc_token>& out);
		long evaluate_condition(const std::vector<cc_token>& tokens, size_t& at, int precedence, bool& ok);

		std::vector<bool> conditions_; // one entry per open #if, true when taking the branch
	};

	void preprocessor::skip_space(bool& at_line_start)
	{
		scanner& s = current();
		for (;;)
		{
			if (s.position >= s.text.size()) return;
			const char c = s.text[s.position];
			if (c == '\n')
			{
				++s.line;
				++s.position;
				at_line_start = true;
				continue;
			}
			if (c == ' ' || c == '\t' || c == '\r')
			{
				++s.position;
				continue;
			}
			if (c == '\\')
			{
				// A line continuation, which on a file with DOS line endings
				// has a carriage return between the backslash and the newline.
				size_t after = s.position + 1;
				if (after < s.text.size() && s.text[after] == '\r') ++after;
				if (after < s.text.size() && s.text[after] == '\n')
				{
					s.position = after + 1;
					++s.line;
					continue;
				}
			}
			if (c == '/' && s.position + 1 < s.text.size())
			{
				if (s.text[s.position + 1] == '/')
				{
					while (s.position < s.text.size() && s.text[s.position] != '\n') ++s.position;
					continue;
				}
				if (s.text[s.position + 1] == '*')
				{
					s.position += 2;
					while (s.position + 1 < s.text.size() &&
						!(s.text[s.position] == '*' && s.text[s.position + 1] == '/'))
					{
						if (s.text[s.position] == '\n') ++s.line;
						++s.position;
					}
					s.position += 2;
					continue;
				}
			}
			return;
		}
	}

	// Reads one token from the source, without any macro expansion.
	bool preprocessor::next_raw(cc_token& token, bool& at_line_start, bool& have)
	{
		have = false;
		skip_space(at_line_start);
		scanner& s = current();
		if (s.position >= s.text.size()) return true;

		token = cc_token();
		token.line = s.line;
		token.file = s.file;
		const char c = s.text[s.position];

		if (is_identifier_start(c))
		{
			const size_t start = s.position;
			while (s.position < s.text.size() && is_identifier(s.text[s.position])) ++s.position;
			token.text = s.text.substr(start, s.position - start);
			token.kind = tok::identifier;
			for (const keyword_entry& entry : keywords)
				if (token.text == entry.name) token.kind = entry.kind;
			have = true;
			return true;
		}

		if (is_digit(c) || (c == '.' && s.position + 1 < s.text.size() && is_digit(s.text[s.position + 1])))
		{
			long value = 0;

			// A decimal run followed by a point or an exponent is a real, which
			// this compiler only ever evaluates at compile time.
			const bool decimal = c != '0' || s.position + 1 >= s.text.size() ||
				is_digit(s.text[s.position + 1]) || s.text[s.position + 1] == '.' ||
				s.text[s.position + 1] == 'e' || s.text[s.position + 1] == 'E';
			if (decimal || c == '.')
			{
				size_t scan = s.position;
				while (scan < s.text.size() && is_digit(s.text[scan])) ++scan;
				bool real = false;
				if (scan < s.text.size() && s.text[scan] == '.')
				{
					real = true;
					++scan;
					while (scan < s.text.size() && is_digit(s.text[scan])) ++scan;
				}
				if (scan < s.text.size() && (s.text[scan] == 'e' || s.text[scan] == 'E'))
				{
					size_t exponent = scan + 1;
					if (exponent < s.text.size() && (s.text[exponent] == '+' || s.text[exponent] == '-'))
						++exponent;
					if (exponent < s.text.size() && is_digit(s.text[exponent]))
					{
						real = true;
						scan = exponent;
						while (scan < s.text.size() && is_digit(s.text[scan])) ++scan;
					}
				}
				if (real)
				{
					token.real = strtod(s.text.c_str() + s.position, nullptr);
					token.is_real = true;
					s.position = scan;
					while (s.position < s.text.size() && (s.text[s.position] == 'f' ||
						s.text[s.position] == 'F' || s.text[s.position] == 'l' ||
						s.text[s.position] == 'L')) ++s.position;
					token.kind = tok::number;
					have = true;
					return true;
				}
			}

			if (c == '0' && s.position + 1 < s.text.size() &&
				(s.text[s.position + 1] == 'b' || s.text[s.position + 1] == 'B'))
			{
				s.position += 2;
				while (s.position < s.text.size() && (s.text[s.position] == '0' || s.text[s.position] == '1'))
					value = value * 2 + (s.text[s.position++] - '0');
			}
			else if (c == '0' && s.position + 1 < s.text.size() &&
				(s.text[s.position + 1] == 'x' || s.text[s.position + 1] == 'X'))
			{
				s.position += 2;
				while (s.position < s.text.size())
				{
					const char d = s.text[s.position];
					int digit;
					if (is_digit(d)) digit = d - '0';
					else if (d >= 'a' && d <= 'f') digit = d - 'a' + 10;
					else if (d >= 'A' && d <= 'F') digit = d - 'A' + 10;
					else break;
					value = value * 16 + digit;
					++s.position;
				}
			}
			else if (c == '0')
			{
				while (s.position < s.text.size() && s.text[s.position] >= '0' && s.text[s.position] <= '7')
					value = value * 8 + (s.text[s.position++] - '0');
			}
			else
			{
				while (s.position < s.text.size() && is_digit(s.text[s.position]))
					value = value * 10 + (s.text[s.position++] - '0');
			}
			while (s.position < s.text.size() && (s.text[s.position] == 'u' || s.text[s.position] == 'U' ||
				s.text[s.position] == 'l' || s.text[s.position] == 'L'))
			{
				const char suffix = s.text[s.position++];
				if (suffix == 'u' || suffix == 'U') token.unsigned_literal = true;
				else token.wide = true;
			}
			token.kind = tok::number;
			token.value = value;
			if (value > 65535 || value < -32768) token.wide = true;
			have = true;
			return true;
		}

		const auto escape = [&](char e) -> char
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
			case 'e': return char(27);
			default: return e;
			}
		};

		if (c == '"')
		{
			++s.position;
			std::string value;
			while (s.position < s.text.size() && s.text[s.position] != '"')
			{
				char d = s.text[s.position++];
				if (d == '\\' && s.position < s.text.size()) d = escape(s.text[s.position++]);
				value.push_back(d);
			}
			if (s.position >= s.text.size()) return fail("unterminated string");
			++s.position;
			token.kind = tok::string;
			token.text = value;
			have = true;
			return true;
		}

		if (c == '\'')
		{
			++s.position;
			long value = 0;
			while (s.position < s.text.size() && s.text[s.position] != '\'')
			{
				char d = s.text[s.position++];
				if (d == '\\' && s.position < s.text.size()) d = escape(s.text[s.position++]);
				value = d;
			}
			if (s.position >= s.text.size()) return fail("unterminated character constant");
			++s.position;
			token.kind = tok::character;
			token.value = value;
			have = true;
			return true;
		}

		struct punctuator
		{
			const char* text;
			tok kind;
		};
		static const punctuator punctuators[] = {
			{"...", tok::ellipsis},
			{"<<=", tok::shl_assign}, {">>=", tok::shr_assign},
			{"->", tok::arrow}, {"++", tok::inc}, {"--", tok::dec},
			{"<<", tok::shl}, {">>", tok::shr}, {"<=", tok::le}, {">=", tok::ge},
			{"==", tok::eq}, {"!=", tok::ne}, {"&&", tok::and_and}, {"||", tok::or_or},
			{"+=", tok::plus_assign}, {"-=", tok::minus_assign}, {"*=", tok::star_assign},
			{"/=", tok::slash_assign}, {"%=", tok::percent_assign}, {"&=", tok::amp_assign},
			{"|=", tok::pipe_assign}, {"^=", tok::caret_assign},
			{"(", tok::l_paren}, {")", tok::r_paren}, {"{", tok::l_brace}, {"}", tok::r_brace},
			{"[", tok::l_bracket}, {"]", tok::r_bracket}, {";", tok::semicolon}, {",", tok::comma},
			{".", tok::dot}, {"?", tok::question}, {":", tok::colon}, {"+", tok::plus},
			{"-", tok::minus}, {"*", tok::star}, {"/", tok::slash}, {"%", tok::percent},
			{"&", tok::amp}, {"|", tok::pipe}, {"^", tok::caret}, {"~", tok::tilde},
			{"!", tok::bang}, {"=", tok::assign}, {"<", tok::less}, {">", tok::greater},
		};

		for (const punctuator& p : punctuators)
		{
			const size_t length = std::strlen(p.text);
			if (s.text.compare(s.position, length, p.text) == 0)
			{
				s.position += length;
				token.kind = p.kind;
				have = true;
				return true;
			}
		}

		if (c == '#')
		{
			++s.position;
			token.kind = tok::identifier;
			token.text = "#";
			have = true;
			return true;
		}

		return fail("unexpected character");
	}

	bool preprocessor::read_line_tokens(std::vector<cc_token>& out)
	{
		for (;;)
		{
			scanner& s = current();
			const size_t before = s.position;
			cc_token token;
			bool at_line_start = false;
			bool have = false;
			if (!next_raw(token, at_line_start, have)) return false;
			if (!have) return true;
			if (at_line_start)
			{
				// The token belongs to the next line; leave it there.
				s.position = before;
				return true;
			}
			out.push_back(token);
		}
	}

	bool preprocessor::include_file()
	{
		scanner& s = current();
		while (s.position < s.text.size() && (s.text[s.position] == ' ' || s.text[s.position] == '\t'))
			++s.position;
		if (s.position >= s.text.size()) return fail("#include needs a file name");

		const char open = s.text[s.position];
		const char close = open == '<' ? '>' : '"';
		if (open != '<' && open != '"') return fail("#include needs a file name");
		++s.position;
		std::string name;
		while (s.position < s.text.size() && s.text[s.position] != close) name.push_back(s.text[s.position++]);
		if (s.position >= s.text.size()) return fail("unterminated #include");
		++s.position;

		std::string text;
		if (!sources_.load(name.c_str(), text)) return fail("cannot find the included file");
		if (stack_.size() > 16) return fail("#include nested too deeply");

		scanner included;
		included.text = text;
		included.file = file_index(name.c_str());
		stack_.push_back(included);
		return true;
	}

	bool preprocessor::define()
	{
		cc_token name;
		bool at_line_start = false;
		bool have = false;
		if (!next_raw(name, at_line_start, have)) return false;
		// A keyword is a legal macro name: a source that maps inline onto its
		// compiler's spelling of it is doing exactly that.
		if (!have || name.text.empty()) return fail("#define needs a name");

		macro entry;
		scanner& s = current();
		// A '(' with no space before it makes this a function-like macro.
		if (s.position < s.text.size() && s.text[s.position] == '(')
		{
			entry.function_like = true;
			++s.position;
			for (;;)
			{
				cc_token parameter;
				if (!next_raw(parameter, at_line_start, have)) return false;
				if (!have) return fail("unterminated macro parameter list");
				if (parameter.kind == tok::r_paren) break;
				if (parameter.kind == tok::comma) continue;
				if (parameter.kind != tok::identifier) return fail("bad macro parameter");
				entry.parameters.push_back(parameter.text);
			}
		}

		if (!read_line_tokens(entry.body)) return false;
		macros_[name.text] = entry;
		return true;
	}

	// Evaluates a #if expression. Only what the shell and its headers need is
	// supported: constants, defined(), the comparison and logical operators.
	long preprocessor::evaluate_condition(const std::vector<cc_token>& tokens, size_t& at, int precedence, bool& ok)
	{
		const auto value_of = [&](const cc_token& token) -> long
		{
			if (token.kind == tok::number || token.kind == tok::character) return token.value;
			return 0; // an undefined identifier is zero, as C says
		};

		long left = 0;
		if (at >= tokens.size())
		{
			ok = false;
			return 0;
		}

		const cc_token& first = tokens[at];
		if (first.kind == tok::bang)
		{
			++at;
			left = evaluate_condition(tokens, at, 12, ok) == 0 ? 1 : 0;
		}
		else if (first.kind == tok::minus)
		{
			++at;
			left = -evaluate_condition(tokens, at, 12, ok);
		}
		else if (first.kind == tok::l_paren)
		{
			++at;
			left = evaluate_condition(tokens, at, 0, ok);
			if (at < tokens.size() && tokens[at].kind == tok::r_paren) ++at;
			else ok = false;
		}
		else if (first.kind == tok::identifier && first.text == "defined")
		{
			++at;
			bool parenthesised = false;
			if (at < tokens.size() && tokens[at].kind == tok::l_paren)
			{
				parenthesised = true;
				++at;
			}
			if (at >= tokens.size())
			{
				ok = false;
				return 0;
			}
			left = macros_.count(tokens[at].text) != 0 ? 1 : 0;
			++at;
			if (parenthesised)
			{
				if (at < tokens.size() && tokens[at].kind == tok::r_paren) ++at;
				else ok = false;
			}
		}
		else
		{
			left = value_of(first);
			++at;
		}

		for (;;)
		{
			if (at >= tokens.size()) return left;
			const tok kind = tokens[at].kind;
			int level;
			switch (kind)
			{
			case tok::or_or: level = 1; break;
			case tok::and_and: level = 2; break;
			case tok::pipe: level = 3; break;
			case tok::caret: level = 4; break;
			case tok::amp: level = 5; break;
			case tok::eq: case tok::ne: level = 6; break;
			case tok::less: case tok::greater: case tok::le: case tok::ge: level = 7; break;
			case tok::plus: case tok::minus: level = 9; break;
			case tok::star: case tok::slash: case tok::percent: level = 10; break;
			default: return left;
			}
			if (level <= precedence) return left;
			++at;
			const long right = evaluate_condition(tokens, at, level, ok);
			switch (kind)
			{
			case tok::or_or: left = (left || right) ? 1 : 0; break;
			case tok::and_and: left = (left && right) ? 1 : 0; break;
			case tok::pipe: left |= right; break;
			case tok::caret: left ^= right; break;
			case tok::amp: left &= right; break;
			case tok::eq: left = left == right; break;
			case tok::ne: left = left != right; break;
			case tok::less: left = left < right; break;
			case tok::greater: left = left > right; break;
			case tok::le: left = left <= right; break;
			case tok::ge: left = left >= right; break;
			case tok::plus: left += right; break;
			case tok::minus: left -= right; break;
			case tok::star: left *= right; break;
			case tok::slash: left = right != 0 ? left / right : 0; break;
			default: left = right != 0 ? left % right : 0; break;
			}
		}
	}

	// Skips a conditional branch, stopping *before* the directive that ends it
	// so the normal handler deals with the #else, #elif or #endif.
	bool preprocessor::skip_conditional_block(bool stop_at_else)
	{
		int depth = 0;
		for (;;)
		{
			scanner& s = current();
			if (s.position >= s.text.size()) return fail("unterminated #if");

			const size_t before = s.position;
			cc_token token;
			bool at_line_start = false;
			bool have = false;
			if (!next_raw(token, at_line_start, have)) return false;
			if (!have) return fail("unterminated #if");
			if (token.kind != tok::identifier || token.text != "#") continue;

			cc_token name;
			if (!next_raw(name, at_line_start, have)) return false;
			if (!have) continue;

			if (name.text == "if" || name.text == "ifdef" || name.text == "ifndef")
			{
				++depth;
				continue;
			}
			if (name.text == "endif")
			{
				if (depth == 0)
				{
					s.position = before;
					return true;
				}
				--depth;
				continue;
			}
			if (depth == 0 && stop_at_else && (name.text == "else" || name.text == "elif"))
			{
				s.position = before;
				return true;
			}
		}
	}

	// Replaces macros throughout a token list. A function-like one is expanded
	// only when its argument list is already in the list, which is how one
	// written inside another macro's body arrives here.
	void preprocessor::expand_object_macros(std::vector<cc_token>& tokens, bool protect_defined)
	{
		for (int pass = 0; pass < 16; ++pass)
		{
			std::vector<cc_token> expanded;
			bool changed = false;
			for (size_t i = 0; i < tokens.size(); ++i)
			{
				// The operand of defined is a name, not a value: expanding it
				// would lose the question being asked.
				if (protect_defined && tokens[i].kind == tok::identifier && tokens[i].text == "defined")
				{
					const size_t span = i + 1 < tokens.size() && tokens[i + 1].kind == tok::l_paren ? 3 : 1;
					for (size_t j = 0; j <= span && i + j < tokens.size(); ++j) expanded.push_back(tokens[i + j]);
					i += span;
					continue;
				}
				const auto found = tokens[i].kind == tok::identifier
					                   ? macros_.find(tokens[i].text)
					                   : macros_.end();

				if (found != macros_.end() && found->second.function_like && !found->second.expanding &&
					i + 1 < tokens.size() && tokens[i + 1].kind == tok::l_paren)
				{
					std::vector<std::vector<cc_token>> arguments;
					arguments.emplace_back();
					size_t scan = i + 2;
					int depth = 0;
					bool closed = false;
					for (; scan < tokens.size(); ++scan)
					{
						const tok k = tokens[scan].kind;
						if (k == tok::r_paren && depth == 0)
						{
							closed = true;
							break;
						}
						if (k == tok::l_paren) ++depth;
						else if (k == tok::r_paren) --depth;
						else if (k == tok::comma && depth == 0)
						{
							arguments.emplace_back();
							continue;
						}
						arguments.back().push_back(tokens[scan]);
					}

					macro& entry = found->second;
					if (arguments.size() == 1 && arguments[0].empty() && entry.parameters.empty())
						arguments.clear();
					if (closed && arguments.size() == entry.parameters.size())
					{
						entry.expanding = true;
						for (std::vector<cc_token>& argument : arguments)
							expand_object_macros(argument, protect_defined);
						for (const cc_token& body : entry.body)
						{
							bool substituted = false;
							if (body.kind == tok::identifier)
							{
								for (size_t p = 0; p < entry.parameters.size(); ++p)
								{
									if (entry.parameters[p] != body.text) continue;
									for (cc_token argument : arguments[p])
									{
										argument.line = tokens[i].line;
										argument.file = tokens[i].file;
										expanded.push_back(argument);
									}
									substituted = true;
									break;
								}
							}
							if (!substituted)
							{
								cc_token copy = body;
								copy.line = tokens[i].line;
								copy.file = tokens[i].file;
								expanded.push_back(copy);
							}
						}
						entry.expanding = false;
						i = scan;
						changed = true;
						continue;
					}
				}

				if (found == macros_.end() || found->second.function_like || found->second.expanding)
				{
					expanded.push_back(tokens[i]);
					continue;
				}
				for (cc_token token : found->second.body)
				{
					token.line = tokens[i].line;
					token.file = tokens[i].file;
					expanded.push_back(token);
				}
				changed = true;
			}
			tokens.swap(expanded);
			if (!changed) return;
		}
	}

	// Reads the expression of a #if or #elif and replaces the macros in it.
	bool preprocessor::read_condition(std::vector<cc_token>& line)
	{
		if (!read_line_tokens(line)) return false;
		expand_object_macros(line, true);
		return true;
	}

	bool preprocessor::conditional(const std::string& name)
	{
		bool taken = false;
		if (name == "ifdef" || name == "ifndef")
		{
			cc_token identifier;
			bool at_line_start = false;
			bool have = false;
			if (!next_raw(identifier, at_line_start, have)) return false;
			if (!have) return fail("#ifdef needs a name");
			const bool defined = macros_.count(identifier.text) != 0;
			taken = name == "ifdef" ? defined : !defined;
		}
		else
		{
			std::vector<cc_token> line;
			if (!read_condition(line)) return false;
			size_t at = 0;
			bool ok = true;
			taken = evaluate_condition(line, at, 0, ok) != 0;
			if (!ok) return fail("bad #if expression");
		}

		conditions_.push_back(taken);
		if (taken) return true;
		if (!skip_conditional_block(true)) return false;
		return true;
	}

	bool preprocessor::directive()
	{
		cc_token name;
		bool at_line_start = false;
		bool have = false;
		if (!next_raw(name, at_line_start, have)) return false;
		if (!have) return true;

		if (name.text == "include") return include_file();
		if (name.text == "define") return define();
		if (name.text == "undef")
		{
			cc_token identifier;
			if (!next_raw(identifier, at_line_start, have)) return false;
			if (have) macros_.erase(identifier.text);
			return true;
		}
		if (name.text == "ifdef" || name.text == "ifndef" || name.text == "if") return conditional(name.text);
		if (name.text == "else")
		{
			if (conditions_.empty()) return fail("#else without #if");
			if (conditions_.back()) return skip_conditional_block(false);
			conditions_.back() = true;
			return true;
		}
		if (name.text == "elif")
		{
			if (conditions_.empty()) return fail("#elif without #if");
			if (conditions_.back()) return skip_conditional_block(false);
			std::vector<cc_token> line;
			if (!read_condition(line)) return false;
			size_t at = 0;
			bool ok = true;
			const bool taken = evaluate_condition(line, at, 0, ok) != 0;
			if (!ok) return fail("bad #elif expression");
			if (taken)
			{
				conditions_.back() = true;
				return true;
			}
			return skip_conditional_block(true);
		}
		if (name.text == "endif")
		{
			if (conditions_.empty()) return fail("#endif without #if");
			conditions_.pop_back();
			return true;
		}
		if (name.text == "error") return fail("#error");
		if (name.text == "pragma" || name.text == "line")
		{
			std::vector<cc_token> ignored;
			return read_line_tokens(ignored);
		}
		return fail("unknown preprocessor directive");
	}

	bool preprocessor::expand(const cc_token& name, std::vector<cc_token>& out)
	{
		const auto found = macros_.find(name.text);
		if (found == macros_.end() || found->second.expanding)
		{
			out.push_back(name);
			return true;
		}

		macro& entry = found->second;
		std::vector<std::vector<cc_token>> arguments;

		if (entry.function_like)
		{
			// Only expand when a '(' actually follows.
			scanner& s = current();
			const size_t saved = s.position;
			const int saved_line = s.line;
			bool at_line_start = false;
			bool have = false;
			cc_token token;
			if (!next_raw(token, at_line_start, have)) return false;
			if (!have || token.kind != tok::l_paren)
			{
				s.position = saved;
				s.line = saved_line;
				out.push_back(name);
				return true;
			}

			arguments.emplace_back();
			int depth = 0;
			for (;;)
			{
				if (!next_raw(token, at_line_start, have)) return false;
				if (!have) return fail("unterminated macro argument list");
				if (token.kind == tok::l_paren) ++depth;
				if (token.kind == tok::r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				if (token.kind == tok::comma && depth == 0)
				{
					arguments.emplace_back();
					continue;
				}
				arguments.back().push_back(token);
			}
			if (arguments.size() == 1 && arguments[0].empty() && entry.parameters.empty()) arguments.clear();
			if (arguments.size() != entry.parameters.size()) return fail("wrong number of macro arguments");
			// An argument is expanded before it is substituted, so a constant
			// passed to a macro is a number by the time the parser sees it.
			for (std::vector<cc_token>& argument : arguments) expand_object_macros(argument, false);
		}

		entry.expanding = true;
		const size_t start = out.size();
		for (const cc_token& body : entry.body)
		{
			bool substituted = false;
			if (body.kind == tok::identifier)
			{
				for (size_t i = 0; i < entry.parameters.size(); ++i)
				{
					if (entry.parameters[i] != body.text) continue;
					for (cc_token argument : arguments[i])
					{
						argument.line = name.line;
						argument.file = name.file;
						out.push_back(argument);
					}
					substituted = true;
					break;
				}
				if (!substituted && macros_.count(body.text) != 0)
				{
					// Nested object-like expansion; the guard flag stops recursion.
					cc_token nested = body;
					nested.line = name.line;
					nested.file = name.file;
					if (!expand(nested, out))
					{
						entry.expanding = false;
						return false;
					}
					substituted = true;
				}
			}
			if (!substituted)
			{
				cc_token copy = body;
				copy.line = name.line;
				copy.file = name.file;
				out.push_back(copy);
			}
		}
		// A function-like macro written inside this body brings its argument
		// list with it, so it can only be expanded once the body is laid out.
		std::vector<cc_token> tail(out.begin() + ptrdiff_t(start), out.end());
		expand_object_macros(tail, false);
		out.resize(start);
		out.insert(out.end(), tail.begin(), tail.end());
		entry.expanding = false;
		return true;
	}

	bool preprocessor::run(const char* text, const char* name, const std::vector<std::string>& defines)
	{
		scanner first;
		first.text = text;
		first.file = file_index(name);
		stack_.push_back(first);

		macros_["__OVRWIN__"] = macro();
		macros_["__DOS__"] = macro();
		macros_["_M_I86"] = macro(); // the identity a 16-bit real-mode source expects

		// A GCC attribute says nothing this compiler can act on, and dropping it
		// here saves the parser from having to know the syntax at all.
		macro attribute;
		attribute.function_like = true;
		attribute.parameters.emplace_back("ignored");
		macros_["__attribute__"] = attribute;

		// A command-line define is scanned as if it were the body of a #define.
		for (const std::string& entry : defines)
		{
			const size_t equals = entry.find('=');
			macro definition;
			if (equals != std::string::npos)
			{
				scanner body;
				body.text = entry.substr(equals + 1);
				body.file = first.file;
				stack_.push_back(body);
				if (!read_line_tokens(definition.body)) return false;
				stack_.pop_back();
			}
			macros_[equals == std::string::npos ? entry : entry.substr(0, equals)] = definition;
		}

		while (!stack_.empty())
		{
			if (current().position >= current().text.size())
			{
				stack_.pop_back();
				continue;
			}

			bool at_line_start = false;
			cc_token token;
			bool have = false;
			const size_t before = current().position;
			if (!next_raw(token, at_line_start, have)) return false;
			(void)before;
			if (!have) continue;

			if (token.kind == tok::identifier && token.text == "#")
			{
				if (!directive()) return false;
				continue;
			}

			if (token.kind == tok::identifier)
			{
				if (!expand(token, out_.tokens)) return false;
				continue;
			}
			out_.tokens.push_back(token);
		}

		cc_token end;
		end.kind = tok::end;
		out_.tokens.push_back(end);

		// Adjacent string literals are one string. Joining them here rather than
		// in the scanner also catches the ones a macro brought together.
		size_t kept = 0;
		for (size_t i = 0; i < out_.tokens.size(); ++i)
		{
			if (kept > 0 && out_.tokens[i].kind == tok::string && out_.tokens[kept - 1].kind == tok::string)
			{
				out_.tokens[kept - 1].text += out_.tokens[i].text;
				continue;
			}
			out_.tokens[kept++] = out_.tokens[i];
		}
		out_.tokens.resize(kept);
		return true;
	}
}

bool cc_tokenise(const char* text, const char* name, cc_sources& sources, cc_token_stream& out,
                 const std::vector<std::string>& defines)
{
	preprocessor pass(sources, out);
	return pass.run(text, name, defines);
}
