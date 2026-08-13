// ovrwin by Zac Walker
//
// Parser and code generator. One pass, no syntax tree: the parser emits 8086
// code as it goes and carries a small `value` describing where the result of
// the expression it just parsed lives. That is the shape TCC uses, and it is
// what keeps a working C compiler down to a readable few thousand lines.
//
// Everything is computed in AX. A binary operator pushes its left operand,
// evaluates the right into AX, then pops the left back - no register
// allocation, and correct by construction.

#include "cc.h"
#include "emit.h"
#include "lex.h"
#include "target.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct compile_error
	{
		std::string message;
		int line;
		int file;
	};

	enum type_kind
	{
		ty_void, ty_char, ty_uchar, ty_int, ty_uint, ty_long, ty_ulong, ty_real,
		ty_pointer, ty_array, ty_record, ty_function
	};

	struct type_info
	{
		type_kind kind = ty_int;
		int size = 2;
		int base = -1; // element or target type, or the result of a function
		int count = 0; // array length
		int record = -1;
		int signature = -1; // index into the parameter-list table, for a function
	};

	struct record_member
	{
		std::string name;
		int type = 0;
		int offset = 0;
	};

	struct record_info
	{
		std::string name;
		std::vector<record_member> members;
		int size = 0;
		bool is_union = false;
	};

	enum symbol_kind { sym_global, sym_local, sym_function, sym_constant, sym_typedef };

	struct symbol
	{
		std::string name;
		symbol_kind kind = sym_global;
		int type = 0;
		int offset = 0; // data offset for globals, frame offset for locals
		int label = -1; // where the code is, for a function
		int start = -1; // the same label, kept for the span the body occupies
		int finish = -1; // the label just past the body
		long value = 0; // enumeration constant
		int scope = 0;
		std::vector<int> parameters; // declared parameter types, so a call can convert
		bool defined = false; // a body was generated, not just a declaration
		bool called = false; // something other than a dropped function reached it
		int body = -1; // the token the body starts at, for inlining
		int frame = 0; // how much stack the body needs, for inlining
		std::vector<std::string> parameter_names;
	};

	enum value_kind { val_const, val_reg, val_local, val_global, val_deref, val_flags };

	// Where the address of a dereference is. Leaving it in memory until the
	// value is actually wanted is what keeps it off the stack.
	enum address_source { addr_reg, addr_local, addr_global };

	struct value
	{
		value_kind kind = val_const;
		int type = 0;
		long constant = 0;
		double real = 0; // only meaningful while the value is still a constant
		int offset = 0; // the condition, for a value that is still only flags
		address_source address = addr_reg;
		int bias = 0; // the constant part of a dereferenced address
	};

	// A pointer held in the data section, which does not move and so is still
	// written by offset rather than through the emitter's layout record.
	struct data_fixup
	{
		int position;
		int offset;
	};

	// The address of a function, written into the data section.
	struct function_fixup
	{
		int position;
		int function;
	};

	struct switch_context
	{
		int temporary = 0;
		std::vector<std::pair<long, int>> cases; // value and the label of the case
		int default_label = -1;
	};

	// The 32-bit operations an 8086 has no instruction for. Each is a short
	// subroutine taking DX:AX and CX:BX and returning DX:AX, emitted after the
	// program only if something called it.
	enum helper
	{
		h_lmul, h_uldivmod, h_ldiv, h_uldiv, h_lmod, h_ulmod,
		h_lshl, h_lshr, h_lsar, h_lcmp, h_ulcmp, helper_count
	};

	// What a token looks like in a diagnostic. Punctuation and keywords carry
	// no text of their own, and those are exactly the ones a parser stops on.
	const char* spelling_of(tok k)
	{
		switch (k)
		{
		case tok::end: return "end of file";
		case tok::number: return "a number";
		case tok::string: return "a string";
		case tok::character: return "a character constant";
		case tok::kw_int: return "int";
		case tok::kw_char: return "char";
		case tok::kw_void: return "void";
		case tok::kw_unsigned: return "unsigned";
		case tok::kw_signed: return "signed";
		case tok::kw_short: return "short";
		case tok::kw_long: return "long";
		case tok::kw_if: return "if";
		case tok::kw_else: return "else";
		case tok::kw_while: return "while";
		case tok::kw_for: return "for";
		case tok::kw_do: return "do";
		case tok::kw_return: return "return";
		case tok::kw_break: return "break";
		case tok::kw_continue: return "continue";
		case tok::kw_switch: return "switch";
		case tok::kw_case: return "case";
		case tok::kw_default: return "default";
		case tok::kw_struct: return "struct";
		case tok::kw_union: return "union";
		case tok::kw_enum: return "enum";
		case tok::kw_typedef: return "typedef";
		case tok::kw_sizeof: return "sizeof";
		case tok::kw_static: return "static";
		case tok::kw_extern: return "extern";
		case tok::kw_const: return "const";
		case tok::kw_goto: return "goto";
		case tok::l_paren: return "(";
		case tok::r_paren: return ")";
		case tok::l_brace: return "{";
		case tok::r_brace: return "}";
		case tok::l_bracket: return "[";
		case tok::r_bracket: return "]";
		case tok::semicolon: return ";";
		case tok::comma: return ",";
		case tok::dot: return ".";
		case tok::arrow: return "->";
		case tok::question: return "?";
		case tok::colon: return ":";
		case tok::plus: return "+";
		case tok::minus: return "-";
		case tok::star: return "*";
		case tok::slash: return "/";
		case tok::percent: return "%";
		case tok::amp: return "&";
		case tok::pipe: return "|";
		case tok::caret: return "^";
		case tok::tilde: return "~";
		case tok::bang: return "!";
		case tok::assign: return "=";
		case tok::less: return "<";
		case tok::greater: return ">";
		case tok::inc: return "++";
		case tok::dec: return "--";
		case tok::shl: return "<<";
		case tok::shr: return ">>";
		case tok::le: return "<=";
		case tok::ge: return ">=";
		case tok::eq: return "==";
		case tok::ne: return "!=";
		case tok::and_and: return "&&";
		case tok::or_or: return "||";
		case tok::ellipsis: return "...";
		default: return "this token";
		}
	}

	int precedence_of(tok k)
	{
		switch (k)
		{
		case tok::star: case tok::slash: case tok::percent: return 10;
		case tok::plus: case tok::minus: return 9;
		case tok::shl: case tok::shr: return 8;
		case tok::less: case tok::greater: case tok::le: case tok::ge: return 7;
		case tok::eq: case tok::ne: return 6;
		case tok::amp: return 5;
		case tok::caret: return 4;
		case tok::pipe: return 3;
		case tok::and_and: return 2;
		case tok::or_or: return 1;
		default: return 0;
		}
	}

	bool is_comparison(tok k) { return precedence_of(k) == 6 || precedence_of(k) == 7; }

	condition condition_of(tok op, bool unsigned_operation)
	{
		switch (op)
		{
		case tok::eq: return condition::equal;
		case tok::ne: return condition::not_equal;
		case tok::less: return unsigned_operation ? condition::below : condition::less;
		case tok::le: return unsigned_operation ? condition::below_equal : condition::less_equal;
		case tok::greater: return unsigned_operation ? condition::above : condition::greater;
		default: return unsigned_operation ? condition::above_equal : condition::greater_equal;
		}
	}

	class generator
	{
	public:
		explicit generator(cc_token_stream& tokens, const cc_options& options)
			: tokens_(tokens), options_(options)
		{
		}

		void run();
		std::vector<uint8_t> finish(int& code_size, int& data_size);
		void report(std::vector<std::pair<std::string, int>>& functions) const;

	private:
		cc_token_stream& tokens_;
		cc_options options_;
		cc_target target_;
		emitter e_;
		std::vector<uint8_t> data_;
		std::vector<data_fixup> data_fixups_;
		std::vector<type_info> types_;
		std::vector<record_info> records_;
		std::vector<std::vector<int>> signatures_;
		std::vector<symbol> symbols_;
		std::map<std::string, int> strings_;
		std::vector<function_fixup> function_fixups_;
		std::map<std::string, int> labels_;
		std::vector<std::pair<std::string, int>> pending_gotos_;
		std::vector<int> break_labels_;
		std::vector<int> continue_labels_;
		std::vector<switch_context> switches_;
		size_t at_ = 0;
		int scope_ = 0;
		int frame_ = 0;
		int frame_patch_ = -1;
		int stack_patch_ = -1;
		int paragraph_patch_ = -1;
		int return_type_ = 0;
		bool hoisted_ = false;
		int current_function_ = -1;
		std::vector<int> inline_returns_; // the label the body's returns jump to
		std::vector<int> inline_stack_; // what is being expanded, so it cannot expand itself
		int helper_label_[helper_count] = {
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
		};

		int helper_label(int which)
		{
			if (helper_label_[which] < 0) helper_label_[which] = e_.label();
			return helper_label_[which];
		}

		int t_void_ = 0, t_char_ = 1, t_uchar_ = 2, t_int_ = 3, t_uint_ = 4;
		int t_long_ = 5, t_ulong_ = 6, t_real_ = 7;

		[[noreturn]] void fail(const char* message) const
		{
			const cc_token& token = tokens_.tokens[at_ < tokens_.tokens.size() ? at_ : tokens_.tokens.size() - 1];
			// Naming the token the parser stopped at is most of a diagnostic,
			// and survives a line number that a macro expansion has moved.
			std::string text = message;
			text += token.text.empty() ? std::string(", at ") + spelling_of(token.kind)
			                           : ", at '" + token.text + "'";
			throw compile_error{text, token.line, token.file};
		}

		// ---- tokens ----
		const cc_token& peek(int ahead = 0) const
		{
			const size_t index = at_ + size_t(ahead);
			return tokens_.tokens[index < tokens_.tokens.size() ? index : tokens_.tokens.size() - 1];
		}

		tok kind() const { return peek().kind; }
		void advance() { if (at_ + 1 < tokens_.tokens.size()) ++at_; }

		bool accept(tok k)
		{
			if (kind() != k) return false;
			advance();
			return true;
		}

		void expect(tok k, const char* message)
		{
			if (!accept(k)) fail(message);
		}

		// ---- types ----
		int add_type(const type_info& info)
		{
			types_.push_back(info);
			return int(types_.size()) - 1;
		}

		const type_info& type(int index) const { return types_[size_t(index)]; }
		int type_size(int index) const { return types_[size_t(index)].size; }
		bool is_pointer(int index) const { return types_[size_t(index)].kind == ty_pointer; }
		bool is_array(int index) const { return types_[size_t(index)].kind == ty_array; }
		bool is_record(int index) const { return types_[size_t(index)].kind == ty_record; }
		bool is_scalar_pointer(int index) const { return is_pointer(index) || is_array(index); }

		bool is_unsigned(int index) const
		{
			const type_kind k = types_[size_t(index)].kind;
			return k == ty_uint || k == ty_uchar || k == ty_ulong || k == ty_pointer;
		}

		// A 32-bit arithmetic type, which lives in DX:AX rather than in AX.
		bool is_wide(int index) const
		{
			const type_kind k = types_[size_t(index)].kind;
			return k == ty_long || k == ty_ulong;
		}

		// There is no floating point at run time, so a real only ever exists as
		// a constant on the way to an integer.
		bool is_real(int index) const { return types_[size_t(index)].kind == ty_real; }

		int target_of(int index) const
		{
			const type_info& info = types_[size_t(index)];
			return info.base >= 0 ? info.base : t_int_;
		}

		int pointer_to(int base)
		{
			for (size_t i = 0; i < types_.size(); ++i)
				if (types_[i].kind == ty_pointer && types_[i].base == base) return int(i);
			type_info info;
			info.kind = ty_pointer;
			info.size = target_.pointer_size;
			info.base = base;
			return add_type(info);
		}

		int array_of(int base, int count)
		{
			type_info info;
			info.kind = ty_array;
			info.base = base;
			info.count = count;
			info.size = count * type_size(base);
			return add_type(info);
		}

		int function_type(int result, const std::vector<int>& parameters)
		{
			signatures_.push_back(parameters);
			type_info info;
			info.kind = ty_function;
			info.size = 1;
			info.base = result;
			info.signature = int(signatures_.size()) - 1;
			return add_type(info);
		}

		bool is_function_type(int index) const { return types_[size_t(index)].kind == ty_function; }

		bool is_function_pointer(int index) const
		{
			return is_pointer(index) && is_function_type(target_of(index));
		}

		const std::vector<int>& signature_of(int function) const
		{
			return signatures_[size_t(types_[size_t(function)].signature)];
		}

		// The label a function's code starts at, made as soon as the name is
		// known so that a call never has to wait for the definition.
		int label_of(int function)
		{
			symbol& entry = symbols_[size_t(function)];
			if (entry.label < 0) entry.label = e_.label();
			return entry.label;
		}

		// ---- symbols ----
		int find(const std::string& name) const
		{
			for (size_t i = symbols_.size(); i > 0; --i)
				if (symbols_[i - 1].name == name) return int(i) - 1;
			return -1;
		}

		int define(const std::string& name, symbol_kind kind, int type)
		{
			symbol entry;
			entry.name = name;
			entry.kind = kind;
			entry.type = type;
			entry.scope = scope_;
			symbols_.push_back(entry);
			return int(symbols_.size()) - 1;
		}

		void leave_scope(int level)
		{
			while (!symbols_.empty() && symbols_.back().scope > level) symbols_.pop_back();
			if (!hoisted_) return;

			// A function declared implicitly inside a body belongs to file scope
			// but was pushed after the locals around it, so the pop above stops
			// at it and leaves them behind. What is left is retired in place
			// rather than removed, because a fixup refers to a symbol by index.
			for (symbol& entry : symbols_)
			{
				if (entry.scope <= level) continue;
				entry.scope = level;
				entry.name.clear();
			}
		}

		// ---- data ----
		int reserve_data(int size, int alignment = 2)
		{
			while (int(data_.size()) % alignment != 0) data_.push_back(0);
			const int offset = int(data_.size());
			data_.resize(data_.size() + size_t(size < 1 ? 1 : size), 0);
			return offset;
		}

		int add_string(const std::string& text)
		{
			const auto found = strings_.find(text);
			if (found != strings_.end()) return found->second;
			const int offset = int(data_.size());
			for (const char c : text) data_.push_back(uint8_t(c));
			data_.push_back(0);
			strings_[text] = offset;
			return offset;
		}

		// The address in the instruction just emitted is an object in the data
		// section, which the layout pass has not placed yet.
		void data_reference(int offset) { e_.refers_to_data(offset); }

		// A pointer to a data object, held in the data section itself. Data does
		// not move, so this one is still an offset.
		void data_pointer(int position, int offset) { data_fixups_.push_back({position, offset}); }

		// Everything a speculatively generated expression can have added to the
		// output, so that it can be taken back if the expression turns out to
		// fold or to want its operands somewhere else.
		struct emission_mark
		{
			emitter::position code;
			size_t data_fixups = 0;
			size_t function_fixups = 0;
		};

		emission_mark mark() const
		{
			emission_mark m;
			m.code = e_.tell();
			m.data_fixups = data_fixups_.size();
			m.function_fixups = function_fixups_.size();
			return m;
		}

		void discard(const emission_mark& m)
		{
			e_.rewind(m.code);
			data_fixups_.resize(m.data_fixups);
			function_fixups_.resize(m.function_fixups);
		}

		bool emitted_since(const emission_mark& m) const
		{
			if (e_.code.size() != m.code.code || e_.literals.size() != m.code.literals) return true;
			return data_fixups_.size() != m.data_fixups || function_fixups_.size() != m.function_fixups;
		}

		// ---- code generation ----
		void load(value& v);
		void load_address(value& v);
		void store(const value& target);
		void store_indirect(int type, int bias = 0);
		void address_to_bx(const value& v);

		// A dereference whose address has just been computed into AX.
		static void make_deref(value& v, int type)
		{
			v.kind = val_deref;
			v.type = type;
			v.address = addr_reg;
			v.bias = 0;
		}
		condition test_value(value& v);
		void apply_operator(tok op, bool unsigned_operation, bool reversed = false);
		void apply_operator_wide(tok op, bool unsigned_operation);
		bool apply_operator_constant(tok op, long constant, bool unsigned_operation);
		bool apply_operator_wide_constant(tok op, long constant, bool unsigned_operation);
		void shift_ax_constant(tok op, int count, bool unsigned_operation);
		void shift_wide_constant(tok op, int count, bool unsigned_operation);
		void scale_ax(int size);
		void widen(int from_type);
		void load_converted(value& v, int to_type);
		void call_helper(helper which);
		void emit_helpers();

		// An operand that generated no code of its own can be put wherever the
		// operator wants it, which is what lets one of the two skip the stack.
		bool settled(const value& v) const
		{
			return v.kind == val_const || v.kind == val_local || v.kind == val_global;
		}

		static bool in_memory(const value& v) { return v.kind == val_local || v.kind == val_global; }
		static address_source source_of(const value& v)
		{
			return v.kind == val_local ? addr_local : addr_global;
		}

		bool fits_cx(const value& v) const
		{
			if (is_real(v.type)) return false;
			if (v.kind == val_const) return !is_wide(v.type);
			if (v.kind != val_local && v.kind != val_global) return false;
			return is_array(v.type) || is_record(v.type) || type_size(v.type) == 2;
		}

		bool fits_cx_bx(const value& v) const
		{
			if (is_real(v.type) || is_array(v.type) || is_record(v.type)) return false;
			if (v.kind == val_const) return true;
			if (v.kind != val_local && v.kind != val_global) return false;
			// A narrow operand only widens into CX:BX for free when it is
			// unsigned; a signed one needs AX, which holds the other operand.
			return is_wide(v.type) || (type_size(v.type) == 2 && is_unsigned(v.type));
		}

		void load_to_cx(value& v);
		void load_to_cx_bx(value& v);

		// The address of a global is a constant, even though what it is is only
		// settled at the end, so it can fold into the instruction.
		bool add_address_to_ax(const value& v)
		{
			if (v.kind != val_global || !(is_array(v.type) || is_record(v.type))) return false;
			e_.add_ax_imm(0);
			data_reference(v.offset);
			return true;
		}

		// True when the operand starting here is a single token that generates
		// no code, so the operator can parse it before deciding anything.
		bool single_operand_ahead(int level) const
		{
			const tok k = peek().kind;
			if (k != tok::number && k != tok::character && k != tok::identifier) return false;
			switch (peek(1).kind)
			{
			case tok::l_paren: case tok::l_bracket: case tok::dot: case tok::arrow:
			case tok::inc: case tok::dec:
				return false;
			default:
				return precedence_of(peek(1).kind) <= level;
			}
		}

		// ---- parsing ----
		void skip_qualifiers()
		{
			while (kind() == tok::kw_const || kind() == tok::kw_qualifier) advance();
		}

		bool is_type_start(int ahead = 0) const;
		int parse_type_specifier();
		int parse_declarator(int base, std::string& name, bool& is_function, std::vector<int>& parameters,
		                     std::vector<std::string>& parameter_names);
		int parse_declarator_suffix(int base, bool& is_function, std::vector<int>& parameters,
		                            std::vector<std::string>& parameter_names);
		int parse_record(bool is_union);
		int parse_enum();
		long constant_expression();
		void declaration();
		void function_body(int function_index, const std::vector<int>& parameters,
		                   const std::vector<std::string>& parameter_names);
		void statement();
		void compound_statement();
		void local_declaration();
		void global_initialiser(int type, int offset);
		int initialiser_length() const;
		bool address_constant(int at);
		bool address_operand(int& symbol, int& pointee, long& offset);
		bool address_offset(int pointee, long& offset);
		void for_statement();
		void switch_statement();
		void emit_return();
		bool in_place_statement(tok terminator);
		operand place_of(const value& target, int extra = 0);
		void memory_operation(op kind, int width, const value& target, const operand& source,
		                      int extra = 0);

		value expression();
		value assignment_expression();
		value conditional_expression();
		value binary_expression(int precedence);
		value unary_expression();
		value postfix_expression();
		value primary_expression();
		value call(int function_index);
		value indirect_call(value& target);
		bool can_inline(int function_index) const;
		bool assigned_in_body(int body, const std::string& name) const;
		value inline_call(int function_index);
		std::vector<value> inline_arguments(int function_index);
		int push_arguments(const std::vector<int>& parameters);

		void emit_runtime();
		void emit_interrupt_call(int number, int cf, int bx, int cx, int dx, bool with_si);
		void emit_startup();
		void drop_unreachable();
	};

	// -----------------------------------------------------------------------
	// Code generation
	// -----------------------------------------------------------------------

	// Puts the address of a dereference in BX. When it is still sitting in a
	// pointer variable this is one instruction and costs no stack slot.
	void generator::address_to_bx(const value& v)
	{
		switch (v.address)
		{
		case addr_local:
			e_.mov_bx_frame(v.offset);
			break;
		case addr_global:
			e_.mov_bx_absolute(0);
			data_reference(v.offset);
			break;
		default:
			e_.mov_bx_ax();
			break;
		}
	}

	void generator::store_indirect(int type_index, int bias)
	{
		if (type_size(type_index) == 1)
		{
			e_.store_al_indirect(bias);
			return;
		}
		e_.store_ax_indirect(bias);
		if (is_wide(type_index)) e_.store_dx_indirect_high(bias);
	}

	// Sign or zero extends AX into DX:AX, so a 16-bit value can take part in a
	// 32-bit operation.
	void generator::widen(int from_type)
	{
		if (is_unsigned(from_type)) e_.xor_dx_dx();
		else e_.cwd();
	}

	void generator::load(value& v)
	{
		// There is no floating point at run time: a real has to have been cast
		// to an integer before anything asks for it in a register.
		if (is_real(v.type)) fail("floating point is only available in a constant expression");
		const bool wide = is_wide(v.type);
		switch (v.kind)
		{
		case val_const:
			e_.mov_ax_imm(int(v.constant & 0xffff));
			if (wide) e_.mov_dx_imm(int((v.constant >> 16) & 0xffff));
			break;

		case val_reg:
			break;

		case val_flags:
			// The comparison is only in the flags, so it costs something to ask
			// for it as a number.
			e_.set_from_condition(condition(v.offset));
			break;

		case val_local:
			if (is_array(v.type) || is_record(v.type)) e_.lea_ax_frame(v.offset);
			else if (type_size(v.type) == 1)
			{
				e_.load_al_frame(v.offset);
				if (is_unsigned(v.type)) e_.zero_ah();
				else e_.cbw();
			}
			else
			{
				e_.load_ax_frame(v.offset);
				if (wide) e_.load_dx_frame(v.offset + 2);
			}
			break;

		case val_global:
			if (is_array(v.type) || is_record(v.type))
			{
				e_.mov_ax_imm(0);
				data_reference(v.offset);
			}
			else if (type_size(v.type) == 1)
			{
				e_.load_al_absolute(0);
				data_reference(v.offset);
				if (is_unsigned(v.type)) e_.zero_ah();
				else e_.cbw();
			}
			else
			{
				e_.load_ax_absolute(0);
				data_reference(v.offset);
				if (wide)
				{
					e_.load_dx_absolute(0);
					data_reference(v.offset + 2);
				}
			}
			break;

		default: // val_deref
			if (is_array(v.type) || is_record(v.type))
			{
				// What is wanted is the address, not what is there.
				if (v.address == addr_local) e_.load_ax_frame(v.offset);
				else if (v.address == addr_global)
				{
					e_.load_ax_absolute(0);
					data_reference(v.offset);
				}
				if (v.bias != 0) e_.add_ax_imm(v.bias);
				break;
			}
			address_to_bx(v);
			if (type_size(v.type) == 1)
			{
				e_.load_al_indirect(v.bias);
				if (is_unsigned(v.type)) e_.zero_ah();
				else e_.cbw();
			}
			else
			{
				e_.load_ax_indirect(v.bias);
				if (wide) e_.load_dx_indirect_high(v.bias);
			}
			break;
		}

		// An array used as a value decays to a pointer to its first element.
		if (is_array(v.type)) v.type = pointer_to(target_of(v.type));
		v.kind = val_reg;
	}

	// Loads a value and converts it to the type the context wants, which on a
	// 16-bit target only ever means widening to DX:AX.
	void generator::load_converted(value& v, int to_type)
	{
		const bool want_wide = is_wide(to_type);
		if (want_wide && v.kind == val_const)
		{
			// A narrow constant has to be sign extended before it is split.
			if (!is_wide(v.type) && !is_unsigned(v.type)) v.constant = long(int16_t(v.constant));
			v.type = to_type;
			load(v);
			return;
		}
		const bool was_wide = is_wide(v.type);
		const int from = v.type;
		load(v);
		if (want_wide && !was_wide) widen(from);
		v.type = to_type;
	}

	void generator::load_address(value& v)
	{
		switch (v.kind)
		{
		case val_local:
			e_.lea_ax_frame(v.offset);
			break;
		case val_global:
			e_.mov_ax_imm(0);
			data_reference(v.offset);
			break;
		case val_deref:
			if (v.address == addr_local) e_.load_ax_frame(v.offset);
			else if (v.address == addr_global)
			{
				e_.load_ax_absolute(0);
				data_reference(v.offset);
			}
			if (v.bias != 0) e_.add_ax_imm(v.bias);
			break;
		default:
			fail("this expression has no address");
		}
		v.kind = val_reg;
		v.type = pointer_to(v.type);
	}

	// Stores AX into the target. For a dereference the caller must have pushed
	// the target address first.
	void generator::store(const value& target)
	{
		const bool wide = is_wide(target.type);
		switch (target.kind)
		{
		case val_local:
			if (type_size(target.type) == 1) e_.store_al_frame(target.offset);
			else
			{
				e_.store_ax_frame(target.offset);
				if (wide) e_.store_dx_frame(target.offset + 2);
			}
			break;
		case val_global:
			if (type_size(target.type) == 1)
			{
				e_.store_al_absolute(0);
				data_reference(target.offset);
			}
			else
			{
				e_.store_ax_absolute(0);
				data_reference(target.offset);
				if (wide)
				{
					e_.store_dx_absolute(0);
					data_reference(target.offset + 2);
				}
			}
			break;
		case val_deref:
			if (target.address == addr_reg) e_.pop_bx();
			else address_to_bx(target);
			if (type_size(target.type) == 1)
			{
				e_.store_al_indirect(target.bias);
				break;
			}
			e_.store_ax_indirect(target.bias);
			if (wide) e_.store_dx_indirect_high(target.bias);
			break;
		default:
			fail("cannot assign to this expression");
		}
	}

	// Reduces a value to the flags a conditional jump can be taken from, and
	// says which condition means true. A comparison is already there.
	condition generator::test_value(value& v)
	{
		if (v.kind == val_flags) return condition(v.offset);
		const bool wide = is_wide(v.type);
		load(v);
		if (wide) e_.or_ax_dx(); // zero only when both halves are zero
		else e_.test_ax_ax();
		return condition::not_equal;
	}

	void generator::call_helper(helper which)
	{
		e_.call(helper_label(which));
	}

	// The second operand of a narrow operator, put straight in CX. Only called
	// for a value that needs no code of its own, so AX is left alone.
	void generator::load_to_cx(value& v)
	{
		switch (v.kind)
		{
		case val_const:
			e_.mov_cx_imm(int(v.constant & 0xffff));
			break;
		case val_local:
			if (is_array(v.type) || is_record(v.type)) e_.lea_cx_frame(v.offset);
			else e_.load_cx_frame(v.offset);
			break;
		default:
			if (is_array(v.type) || is_record(v.type)) e_.mov_cx_imm(0);
			else e_.load_cx_absolute(0);
			data_reference(v.offset);
			break;
		}
		if (is_array(v.type)) v.type = pointer_to(target_of(v.type));
		v.kind = val_reg;
	}

	// The same for a 32-bit operator, whose second operand lives in CX:BX.
	void generator::load_to_cx_bx(value& v)
	{
		if (v.kind == val_const)
		{
			long constant = v.constant;
			if (!is_wide(v.type) && !is_unsigned(v.type)) constant = long(int16_t(constant));
			e_.mov_bx_imm(int(constant & 0xffff));
			e_.mov_cx_imm(int((constant >> 16) & 0xffff));
			v.kind = val_reg;
			return;
		}

		if (v.kind == val_local)
		{
			e_.load_bx_frame(v.offset);
			if (is_wide(v.type)) e_.load_cx_frame(v.offset + 2);
			else e_.xor_cx_cx();
		}
		else
		{
			e_.load_bx_absolute(0);
			data_reference(v.offset);
			if (is_wide(v.type))
			{
				e_.load_cx_absolute(0);
				data_reference(v.offset + 2);
			}
			else
			{
				e_.xor_cx_cx();
			}
		}
		v.kind = val_reg;
	}

	// A shift by a count known now. Up to three single-bit shifts beat loading
	// CL, and every one of them beats the multiply or divide it may stand for.
	void generator::shift_ax_constant(tok op, int count, bool unsigned_operation)
	{
		if (count <= 0) return;
		if (count <= 3)
		{
			for (int i = 0; i < count; ++i)
			{
				if (op == tok::shl) e_.shl_ax_one();
				else if (unsigned_operation) e_.shr_ax_one();
				else e_.sar_ax_one();
			}
			return;
		}
		e_.mov_cl_imm(count);
		if (op == tok::shl) e_.shl_ax_cl();
		else if (unsigned_operation) e_.shr_ax_cl();
		else e_.sar_ax_cl();
	}

	// The same for the 32-bit pair. A shift of 16 or more is just a move
	// between the halves, which is what makes the fixed-point idiom cheap.
	void generator::shift_wide_constant(tok op, int count, bool unsigned_operation)
	{
		if (count <= 0) return;
		if (count >= 16)
		{
			if (op == tok::shl)
			{
				e_.mov_dx_ax();
				e_.xor_ax_ax();
				const int rest = count - 16;
				if (rest > 0 && rest <= 3) for (int i = 0; i < rest; ++i) e_.shl_dx_one();
				else if (rest > 3)
				{
					e_.mov_cl_imm(rest);
					e_.shl_dx_cl();
				}
			}
			else
			{
				e_.mov_ax_dx();
				if (unsigned_operation) e_.xor_dx_dx();
				else e_.cwd();
				shift_ax_constant(op, count - 16, unsigned_operation);
			}
			return;
		}

		if (count <= 4)
		{
			for (int i = 0; i < count; ++i)
			{
				if (op == tok::shl) e_.shl_pair_one();
				else if (unsigned_operation) e_.shr_pair_one();
				else e_.sar_pair_one();
			}
			return;
		}

		// Anything else is shorter as the loop in the runtime helper.
		e_.mov_bx_imm(count);
		apply_operator_wide(op, unsigned_operation);
	}

	// Multiplies AX by an element size. A shift is both shorter and an order of
	// magnitude faster than the multiply on an 8086.
	void generator::scale_ax(int size)
	{
		if (size == 1) return;
		for (int bit = 1; bit < 16; ++bit)
		{
			if (size != (1 << bit)) continue;
			shift_ax_constant(tok::shl, bit, true);
			return;
		}
		e_.mov_cx_imm(size);
		e_.imul_cx();
	}

	// DX:AX holds the left operand and the right one is a constant. Returns
	// false when the operator has no immediate form, having generated nothing.
	bool generator::apply_operator_wide_constant(tok op, long constant, bool unsigned_operation)
	{
		const int low = int(constant & 0xffff);
		const int high = int((constant >> 16) & 0xffff);

		switch (op)
		{
		case tok::plus:
			if (low == 0 && high == 0) return true;
			if (low == 0) e_.add_dx_imm(high);
			else
			{
				e_.add_ax_imm(low);
				if (high == 0) e_.adc_dx_zero();
				else e_.adc_dx_imm(high);
			}
			return true;

		case tok::minus:
			if (low == 0 && high == 0) return true;
			if (low == 0) e_.sub_dx_imm(high);
			else
			{
				e_.sub_ax_imm(low);
				if (high == 0) e_.sbb_dx_zero();
				else e_.sbb_dx_imm(high);
			}
			return true;

		case tok::amp:
			if (low == 0) e_.xor_ax_ax();
			else if (low != 0xffff) e_.and_ax_imm(low);
			if (high == 0) e_.xor_dx_dx();
			else if (high != 0xffff) e_.and_dx_imm(high);
			return true;

		case tok::pipe:
			if (low != 0) e_.or_ax_imm(low);
			if (high != 0) e_.or_dx_imm(high);
			return true;

		case tok::caret:
			if (low != 0) e_.xor_ax_imm(low);
			if (high != 0) e_.xor_dx_imm(high);
			return true;

		case tok::star:
			if (constant == 1) return true;
			for (int bit = 1; bit < 31; ++bit)
			{
				if (constant != (1L << bit)) continue;
				shift_wide_constant(tok::shl, bit, true);
				return true;
			}
			return false;

		case tok::shl:
		case tok::shr:
			shift_wide_constant(op, int(constant & 31), unsigned_operation);
			return true;

		default:
			return false;
		}
	}

	// AX holds the left operand and the right one is a constant. Returns false
	// when the operator has no immediate form, and nothing has been generated.
	bool generator::apply_operator_constant(tok op, long constant, bool unsigned_operation)
	{
		const int value16 = int(constant & 0xffff);
		const auto power_of_two = [](int v)
		{
			for (int bit = 1; bit < 16; ++bit)
				if (v == (1 << bit)) return bit;
			return 0;
		};

		switch (op)
		{
		case tok::plus:
			if (value16 == 0) return true;
			if (value16 == 1) e_.inc_ax();
			else if (value16 == 0xffff) e_.dec_ax();
			else e_.add_ax_imm(value16);
			return true;

		case tok::minus:
			if (value16 == 0) return true;
			if (value16 == 1) e_.dec_ax();
			else if (value16 == 0xffff) e_.inc_ax();
			else e_.sub_ax_imm(value16);
			return true;

		case tok::amp:
			if (value16 == 0xffff) return true;
			if (value16 == 0) e_.xor_ax_ax();
			else e_.and_ax_imm(value16);
			return true;

		case tok::pipe:
			if (value16 != 0) e_.or_ax_imm(value16);
			return true;

		case tok::caret:
			if (value16 != 0) e_.xor_ax_imm(value16);
			return true;

		case tok::star:
			if (value16 == 0) e_.xor_ax_ax();
			else if (value16 == 1) return true;
			else if (value16 == 0xffff) e_.neg_ax();
			else if (const int bit = power_of_two(value16)) shift_ax_constant(tok::shl, bit, true);
			else return false;
			return true;

		case tok::slash:
			if (value16 == 1) return true;
			if (!unsigned_operation) return false;
			if (const int bit = power_of_two(value16)) shift_ax_constant(tok::shr, bit, true);
			else return false;
			return true;

		case tok::percent:
			if (!unsigned_operation) return false;
			if (value16 == 1) e_.xor_ax_ax();
			else if (power_of_two(value16)) e_.and_ax_imm(value16 - 1);
			else return false;
			return true;

		case tok::shl:
		case tok::shr:
			shift_ax_constant(op, value16 & 31, unsigned_operation);
			return true;

		case tok::eq: case tok::ne: case tok::less: case tok::le:
		case tok::greater: case tok::ge:
			// A comparison against zero needs no operand at all. The result is
			// left in the flags for the caller to branch on.
			if (value16 == 0) e_.test_ax_ax();
			else e_.cmp_ax_imm(value16);
			return true;

		default:
			return false;
		}
	}

	// AX holds the left operand and CX the right. When reversed they are the
	// other way round, which costs an exchange for the operators that care.
	void generator::apply_operator(tok op, bool unsigned_operation, bool reversed)
	{
		const bool commutative = op == tok::plus || op == tok::star || op == tok::amp ||
			op == tok::pipe || op == tok::caret;
		if (reversed && !is_comparison(op) && !commutative) e_.xchg_ax_cx();

		switch (op)
		{
		case tok::plus: e_.add_ax_cx(); return;
		case tok::minus: e_.sub_ax_cx(); return;
		case tok::star:
			if (unsigned_operation) e_.mul_cx();
			else e_.imul_cx();
			return;
		case tok::slash:
			if (unsigned_operation) e_.div_cx();
			else e_.idiv_cx();
			return;
		case tok::percent:
			if (unsigned_operation) e_.div_cx();
			else e_.idiv_cx();
			e_.mov_ax_dx();
			return;
		case tok::amp: e_.and_ax_cx(); return;
		case tok::pipe: e_.or_ax_cx(); return;
		case tok::caret: e_.xor_ax_cx(); return;
		case tok::shl: e_.shl_ax_cl(); return;
		case tok::shr:
			if (unsigned_operation) e_.shr_ax_cl();
			else e_.sar_ax_cl();
			return;
		default:
			break;
		}

		if (!is_comparison(op)) fail("unsupported operator");
		if (reversed) e_.cmp_cx_ax();
		else e_.cmp_ax_cx();
	}


	// DX:AX holds the left operand and CX:BX the right. Only the operations an
	// 8086 can do in a couple of instructions are inline; the rest go to a
	// helper, which is also where the register convention comes from.
	void generator::apply_operator_wide(tok op, bool unsigned_operation)
	{
		switch (op)
		{
		case tok::plus: e_.add_ax_bx(); e_.adc_dx_cx(); return;
		case tok::minus: e_.sub_ax_bx(); e_.sbb_dx_cx(); return;
		case tok::amp: e_.and_ax_bx(); e_.and_dx_cx(); return;
		case tok::pipe: e_.or_ax_bx(); e_.or_dx_cx(); return;
		case tok::caret: e_.xor_ax_bx(); e_.xor_dx_cx(); return;
		case tok::star: call_helper(h_lmul); return;
		case tok::slash: call_helper(unsigned_operation ? h_uldiv : h_ldiv); return;
		case tok::percent: call_helper(unsigned_operation ? h_ulmod : h_lmod); return;
		case tok::shl: call_helper(h_lshl); return;
		case tok::shr: call_helper(unsigned_operation ? h_lshr : h_lsar); return;
		default:
			break;
		}

		// A comparison goes through a helper that reduces the pair to -1, 0 or 1
		// in AX, after which the ordinary signed conditions apply whatever the
		// operands were. The result is left in the flags.
		if (!is_comparison(op)) fail("unsupported operator");
		call_helper(unsigned_operation ? h_ulcmp : h_lcmp);
		e_.test_ax_ax();
	}

	// -----------------------------------------------------------------------
	// Expressions
	// -----------------------------------------------------------------------

	// Arguments have to reach the callee in cdecl order, which means generating
	// them right to left. Because the parser walks a token vector rather than a
	// stream, the argument spans can be located first and then generated
	// backwards. Returns how many bytes ended up on the stack.
	int generator::push_arguments(const std::vector<int>& parameters)
	{
		expect(tok::l_paren, "expected (");

		std::vector<size_t> starts;
		if (kind() != tok::r_paren)
		{
			starts.push_back(at_);
			int depth = 0;
			for (size_t scan = at_;; ++scan)
			{
				const tok k = tokens_.tokens[scan].kind;
				if (k == tok::end) fail("unterminated argument list");
				if (k == tok::l_paren || k == tok::l_bracket) ++depth;
				else if (k == tok::r_bracket) --depth;
				else if (k == tok::r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok::comma && depth == 0) starts.push_back(scan + 1);
			}
		}

		const size_t saved = at_;
		int pushed = 0;
		for (size_t i = starts.size(); i > 0; --i)
		{
			at_ = starts[i - 1];
			value argument = assignment_expression();
			if (i - 1 < parameters.size()) load_converted(argument, parameters[i - 1]);
			else load(argument);
			if (is_wide(argument.type))
			{
				e_.push_dx();
				e_.push_ax();
				pushed += 4;
			}
			else
			{
				e_.push_ax();
				pushed += 2;
			}
		}
		at_ = saved;

		int depth = 0;
		for (;;)
		{
			const tok k = kind();
			if (k == tok::end) fail("unterminated argument list");
			if (k == tok::r_paren && depth == 0) break;
			if (k == tok::l_paren || k == tok::l_bracket) ++depth;
			else if (k == tok::r_paren || k == tok::r_bracket) --depth;
			advance();
		}
		expect(tok::r_paren, "expected )");
		return pushed;
	}

	value generator::call(int function_index)
	{
		if (can_inline(function_index)) return inline_call(function_index);

		const int pushed = push_arguments(symbols_[size_t(function_index)].parameters);

		e_.call(label_of(function_index));
		symbols_[size_t(function_index)].called = true;

		if (pushed != 0) e_.add_sp_imm(pushed);

		value result;
		result.kind = val_reg;
		result.type = symbols_[size_t(function_index)].type;
		return result;
	}

	// Whether a call is better off as a copy of the body. The body is still in
	// the token vector, so expanding it is a matter of parsing it again with
	// the parameters bound to frame slots - no tree to clone, and every
	// optimisation the generator already does applies to the copy.
	bool generator::can_inline(int function_index) const
	{
		if (options_.inline_limit <= 0) return false;
		const symbol& entry = symbols_[size_t(function_index)];
		if (!entry.defined || entry.body < 0) return false;
		if (entry.parameters.size() != entry.parameter_names.size()) return false;
		for (const std::string& name : entry.parameter_names)
			if (name.empty()) return false;
		if (function_index == current_function_) return false;
		if (inline_stack_.size() >= 4) return false;
		for (const int active : inline_stack_)
			if (active == function_index) return false;

		// The argument list has to match, so that every parameter is given a
		// value and none is left over.
		size_t arguments = 0;
		{
			size_t scan = at_ + 1; // past the (
			int depth = 0;
			if (scan < tokens_.tokens.size() && tokens_.tokens[scan].kind != tok::r_paren) arguments = 1;
			for (; scan < tokens_.tokens.size(); ++scan)
			{
				const tok k = tokens_.tokens[scan].kind;
				if (k == tok::end) return false;
				if (k == tok::l_paren || k == tok::l_bracket) ++depth;
				else if (k == tok::r_bracket) --depth;
				else if (k == tok::r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok::comma && depth == 0) ++arguments;
			}
		}
		if (arguments != entry.parameters.size()) return false;

		// A body that is short, and that needs nothing the real frame would
		// have given it: no label to jump to, no storage that outlives the
		// call, and no reaching past the named parameters.
		int depth = 0;
		int length = 0;
		for (size_t scan = size_t(entry.body); scan < tokens_.tokens.size(); ++scan)
		{
			const cc_token& token = tokens_.tokens[scan];
			if (token.kind == tok::end) return false;
			if (token.kind == tok::l_brace) ++depth;
			else if (token.kind == tok::r_brace)
			{
				if (--depth == 0) return true;
			}
			else if (token.kind == tok::kw_goto || token.kind == tok::kw_static) return false;
			else if (token.kind == tok::identifier && token.text == "__args") return false;
			if (++length > options_.inline_limit) return false;
		}
		return false;
	}

	// Whether the body ever writes to a name. A parameter it never writes to
	// can be the argument itself rather than a copy of it.
	bool generator::assigned_in_body(int body, const std::string& name) const
	{
		int depth = 0;
		for (size_t scan = size_t(body); scan + 1 < tokens_.tokens.size(); ++scan)
		{
			const cc_token& token = tokens_.tokens[scan];
			if (token.kind == tok::l_brace) ++depth;
			else if (token.kind == tok::r_brace && --depth == 0) return false;
			if (token.kind != tok::identifier || token.text != name) continue;

			const tok before = scan > 0 ? tokens_.tokens[scan - 1].kind : tok::end;
			if (before == tok::amp || before == tok::inc || before == tok::dec) return true;
			switch (tokens_.tokens[scan + 1].kind)
			{
			case tok::assign: case tok::plus_assign: case tok::minus_assign:
			case tok::star_assign: case tok::slash_assign: case tok::percent_assign:
			case tok::amp_assign: case tok::pipe_assign: case tok::caret_assign:
			case tok::shl_assign: case tok::shr_assign:
			case tok::inc: case tok::dec:
				return true;
			default:
				break;
			}
		}
		return true;
	}

	// The arguments of an expanded call go into frame slots of their own, which
	// is what the parameters are for the length of the body. An argument that
	// is a constant the body never writes to skips the slot and becomes the
	// parameter, which is what makes the copy smaller than the call.
	std::vector<value> generator::inline_arguments(int function_index)
	{
		const std::vector<int> parameters = symbols_[size_t(function_index)].parameters;
		const std::vector<std::string> names = symbols_[size_t(function_index)].parameter_names;
		const int body = symbols_[size_t(function_index)].body;

		expect(tok::l_paren, "expected (");

		std::vector<size_t> starts;
		if (kind() != tok::r_paren)
		{
			starts.push_back(at_);
			int depth = 0;
			for (size_t scan = at_;; ++scan)
			{
				const tok k = tokens_.tokens[scan].kind;
				if (k == tok::end) fail("unterminated argument list");
				if (k == tok::l_paren || k == tok::l_bracket) ++depth;
				else if (k == tok::r_bracket) --depth;
				else if (k == tok::r_paren)
				{
					if (depth == 0) break;
					--depth;
				}
				else if (k == tok::comma && depth == 0) starts.push_back(scan + 1);
			}
		}

		const size_t saved = at_;
		std::vector<value> bound;
		for (size_t i = 0; i < starts.size(); ++i)
		{
			at_ = starts[i];
			const int wanted = parameters[i];
			const emission_mark start = mark();
			value argument = assignment_expression();

			if (argument.kind == val_const && !is_real(argument.type) && !emitted_since(start) &&
				!assigned_in_body(body, names[i]))
			{
				discard(start);
				if (!is_wide(wanted) && !is_real(wanted)) argument.constant = long(int16_t(argument.constant));
				else if (!is_wide(argument.type) && !is_unsigned(argument.type))
					argument.constant = long(int16_t(argument.constant));
				argument.type = wanted;
				bound.push_back(argument);
				continue;
			}

			load_converted(argument, wanted);
			frame_ += is_wide(wanted) ? 4 : 2;
			value target;
			target.kind = val_local;
			target.type = wanted;
			target.offset = -frame_;
			store(target);
			bound.push_back(target);
		}
		at_ = saved;

		int depth = 0;
		for (;;)
		{
			const tok k = kind();
			if (k == tok::end) fail("unterminated argument list");
			if (k == tok::r_paren && depth == 0) break;
			if (k == tok::l_paren || k == tok::l_bracket) ++depth;
			else if (k == tok::r_paren || k == tok::r_bracket) --depth;
			advance();
		}
		expect(tok::r_paren, "expected )");
		return bound;
	}

	value generator::inline_call(int function_index)
	{
		const std::vector<std::string> names = symbols_[size_t(function_index)].parameter_names;
		const int body = symbols_[size_t(function_index)].body;
		const int result_type = symbols_[size_t(function_index)].type;

		const std::vector<value> bound = inline_arguments(function_index);

		++scope_;
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (bound[i].kind == val_const)
			{
				const int local = define(names[i], sym_constant, bound[i].type);
				symbols_[size_t(local)].value = bound[i].constant;
				continue;
			}
			const int local = define(names[i], sym_local, bound[i].type);
			symbols_[size_t(local)].offset = bound[i].offset;
		}

		// The body is another function's, so everything that belongs to a
		// function has to be put aside for the length of it.
		const size_t saved_at = at_;
		const int saved_return = return_type_;
		std::map<std::string, int> saved_labels;
		std::vector<std::pair<std::string, int>> saved_gotos;
		std::vector<int> saved_breaks, saved_continues;
		saved_labels.swap(labels_);
		saved_gotos.swap(pending_gotos_);
		saved_breaks.swap(break_labels_);
		saved_continues.swap(continue_labels_);

		return_type_ = result_type;
		inline_returns_.push_back(e_.label());
		inline_stack_.push_back(function_index);

		at_ = size_t(body);
		expect(tok::l_brace, "expected { to open the function body");
		while (!accept(tok::r_brace))
		{
			if (kind() == tok::end) fail("unterminated function body");
			statement();
		}

		e_.bind(inline_returns_.back());
		inline_returns_.pop_back();
		inline_stack_.pop_back();
		return_type_ = saved_return;
		labels_.swap(saved_labels);
		pending_gotos_.swap(saved_gotos);
		break_labels_.swap(saved_breaks);
		continue_labels_.swap(saved_continues);
		at_ = saved_at;
		leave_scope(--scope_);

		value result;
		result.kind = val_reg;
		result.type = result_type;
		return result;
	}

	// A call through a pointer. The target goes to a frame slot of its own so
	// that arguments - which may contain further calls - cannot disturb it.
	value generator::indirect_call(value& target)
	{
		const int function = target_of(target.type);
		load(target);
		frame_ += 2;
		const int slot = -frame_;
		e_.store_ax_frame(slot);

		const int pushed = push_arguments(signature_of(function));
		e_.call_frame(slot);
		if (pushed != 0) e_.add_sp_imm(pushed);

		value result;
		result.kind = val_reg;
		result.type = type(function).base;
		return result;
	}

	value generator::primary_expression()
	{
		value v;
		const cc_token token = peek();

		if (token.kind == tok::number || token.kind == tok::character)
		{
			advance();
			v.kind = val_const;
			if (token.is_real)
			{
				v.type = t_real_;
				v.real = token.real;
				v.constant = long(token.real);
				return v;
			}
			if (token.wide) v.type = token.unsigned_literal ? t_ulong_ : t_long_;
			else if (token.unsigned_literal || token.value > 32767) v.type = t_uint_;
			else v.type = t_int_;
			v.constant = token.value;
			return v;
		}

		if (token.kind == tok::string)
		{
			advance();
			v.kind = val_global;
			v.type = array_of(t_char_, int(token.text.size()) + 1);
			v.offset = add_string(token.text);
			return v;
		}

		if (token.kind == tok::l_paren)
		{
			advance();
			v = expression();
			expect(tok::r_paren, "expected )");
			return v;
		}

		if (token.kind == tok::identifier)
		{
			// __args(last) is where the arguments after the last named one begin.
			if (token.text == "__args")
			{
				advance();
				expect(tok::l_paren, "expected ( after __args");
				if (kind() != tok::identifier) fail("__args needs the last named parameter");
				const int index = find(peek().text);
				if (index < 0 || symbols_[size_t(index)].kind != sym_local ||
					symbols_[size_t(index)].offset < 0)
					fail("__args needs the last named parameter");
				const symbol& last = symbols_[size_t(index)];
				e_.lea_ax_frame(last.offset + (is_wide(last.type) ? 4 : 2));
				advance();
				expect(tok::r_paren, "expected ) after __args");
				v.kind = val_reg;
				v.type = pointer_to(t_char_);
				return v;
			}

			advance();
			int index = find(token.text);
			if (index < 0)
			{
				// An undeclared name is only allowed as a call, which is
				// assumed to return int as pre-standard C did.
				if (kind() != tok::l_paren) fail(("undeclared identifier: " + token.text).c_str());
				index = define(token.text, sym_function, t_int_);
				symbols_[size_t(index)].scope = 0;
				if (scope_ != 0) hoisted_ = true;
			}

			switch (symbols_[size_t(index)].kind)
			{
			case sym_constant:
				v.kind = val_const;
				v.type = symbols_[size_t(index)].type;
				v.constant = symbols_[size_t(index)].value;
				return v;
			case sym_local:
				v.kind = val_local;
				v.type = symbols_[size_t(index)].type;
				v.offset = symbols_[size_t(index)].offset;
				return v;
			case sym_global:
				v.kind = val_global;
				v.type = symbols_[size_t(index)].type;
				v.offset = symbols_[size_t(index)].offset;
				return v;
			case sym_function:
				if (kind() == tok::l_paren) return call(index);
				// Used without a call, a function name is its address.
				v.kind = val_reg;
				e_.mov_ax_imm(0);
				e_.refers_to_code(label_of(index));
				symbols_[size_t(index)].called = true;
				v.type = pointer_to(function_type(symbols_[size_t(index)].type,
				                                  symbols_[size_t(index)].parameters));
				return v;
			default:
				fail("a type name cannot be used here");
			}
		}

		fail("expected an expression");
	}

	value generator::postfix_expression()
	{
		value v = primary_expression();

		for (;;)
		{
			if (kind() == tok::l_paren && is_function_pointer(v.type))
			{
				v = indirect_call(v);
				continue;
			}

			if (kind() == tok::l_bracket)
			{
				advance();
				if (!is_scalar_pointer(v.type)) fail("only a pointer or an array can be indexed");
				const int element = target_of(v.type);
				const int size = type_size(element);
				const bool base_in_memory = is_array(v.type) && settled(v);

				// An index that generates nothing can be placed after the base
				// address, and a constant one folds into the address itself.
				const emission_mark start = mark();
				const size_t saved = at_;
				value index;
				bool index_settled = false;
				if (peek(1).kind == tok::r_bracket &&
					(kind() == tok::number || kind() == tok::character || kind() == tok::identifier))
				{
					index = expression();
					index_settled = !emitted_since(start) && settled(index);
					if (!index_settled)
					{
						discard(start);
						at_ = saved;
					}
				}

				if (index_settled)
				{
					expect(tok::r_bracket, "expected ]");
					if (index.kind == val_const)
					{
						const int step = int(index.constant) * size;
						if (base_in_memory)
						{
							// An element of an array that is itself in memory
							// is just another object in memory.
							v.offset += step;
							v.type = element;
							continue;
						}
						if (v.kind == val_deref && is_array(v.type))
						{
							v.bias += step;
							v.type = element;
							continue;
						}
						if (in_memory(v))
						{
							// The pointer stays where it is; the element it
							// reaches is a fixed distance from it.
							const address_source source = source_of(v);
							v.kind = val_deref;
							v.address = source;
							v.bias = step;
							v.type = element;
							continue;
						}
						load(v);
						if (step != 0) e_.add_ax_imm(step);
					}
					else if (size == 1)
					{
						load(v);
						load_to_cx(index);
						e_.add_ax_cx();
					}
					else
					{
						// Scaling the index needs AX, so the base waits in CX.
						value base = v;
						load(index);
						scale_ax(size);
						if (add_address_to_ax(base))
						{
						}
						else if (fits_cx(base))
						{
							load_to_cx(base);
							e_.add_ax_cx();
						}
						else
						{
							e_.mov_cx_ax();
							load(base);
							e_.xchg_ax_cx();
							e_.add_ax_cx();
						}
					}
					make_deref(v, element);
					continue;
				}

				if (fits_cx(v))
				{
					value base = v;
					value subscript = expression();
					expect(tok::r_bracket, "expected ]");
					load(subscript);
					scale_ax(size);
					if (!add_address_to_ax(base))
					{
						load_to_cx(base);
						e_.add_ax_cx();
					}
				}
				else
				{
					load(v);
					e_.push_ax();
					value subscript = expression();
					expect(tok::r_bracket, "expected ]");
					load(subscript);
					scale_ax(size);
					e_.mov_cx_ax();
					e_.pop_ax();
					e_.add_ax_cx();
				}
				make_deref(v, element);
				continue;
			}

			if (kind() == tok::dot || kind() == tok::arrow)
			{
				const bool through_pointer = kind() == tok::arrow;
				advance();
				if (kind() != tok::identifier) fail("expected a member name");
				const std::string member = peek().text;
				advance();

				const int record_type = through_pointer ? target_of(v.type) : v.type;
				if (through_pointer && !is_scalar_pointer(v.type)) fail("-> needs a pointer");
				if (!is_record(record_type)) fail("not a structure or union");

				const record_info& info = records_[size_t(type(record_type).record)];
				const record_member* found = nullptr;
				for (const record_member& m : info.members) if (m.name == member) found = &m;
				if (found == nullptr) fail(("no such member: " + member).c_str());

				// A member of a record that is itself in memory needs no
				// address computing: it is another object in memory, and one
				// reached through a pointer is a fixed distance from it.
				if (!through_pointer)
				{
					if (in_memory(v))
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
					load_address(v);
				}
				else if (in_memory(v) && !is_array(v.type))
				{
					const address_source source = source_of(v);
					v.kind = val_deref;
					v.address = source;
					v.bias = found->offset;
					v.type = found->type;
					continue;
				}
				else
				{
					load(v);
				}

				const int member_offset = found->offset;
				make_deref(v, found->type);
				v.bias = member_offset;
				continue;
			}

			if (kind() == tok::inc || kind() == tok::dec)
			{
				const bool increment = kind() == tok::inc;
				advance();
				const int step = is_scalar_pointer(v.type) ? type_size(target_of(v.type)) : 1;
				const bool wide = is_wide(v.type);

				if (wide)
				{
					// BX survives the load and the store, so a dereference does
					// not need the address on the stack as the 16-bit path does.
					const value target = v;
					load(v);
					e_.push_dx();
					e_.push_ax();
					if (increment)
					{
						e_.add_ax_imm(step);
						e_.adc_dx_imm(0);
					}
					else
					{
						e_.sub_ax_imm(step);
						e_.sbb_dx_zero();
					}
					if (target.kind == val_deref) store_indirect(target.type, target.bias);
					else store(target);
					e_.pop_ax();
					e_.pop_dx();
					v.kind = val_reg;
					continue;
				}

				if (v.kind == val_deref)
				{
					// Loading through BX leaves the address there, so the store
					// that follows needs nothing on the stack.
					const value target = v;
					load(v);
					e_.push_ax(); // the value the expression produces
					if (increment) e_.add_ax_imm(step);
					else e_.sub_ax_imm(step);
					store_indirect(target.type, target.bias);
					e_.pop_ax();
				}
				else
				{
					const value target = v;
					load(v);
					e_.push_ax();
					if (increment) e_.add_ax_imm(step);
					else e_.sub_ax_imm(step);
					store(target);
					e_.pop_ax();
				}
				v.kind = val_reg;
				continue;
			}
			break;
		}
		return v;
	}

	value generator::unary_expression()
	{
		const tok k = kind();

		if (k == tok::amp)
		{
			advance();
			value v = unary_expression();
			// A function name already is an address, so &f and f are the same.
			if (is_function_pointer(v.type) && v.kind == val_reg) return v;
			load_address(v);
			return v;
		}
		if (k == tok::star)
		{
			advance();
			value v = unary_expression();
			// Dereferencing a function pointer leaves it a function pointer, so
			// that (*f)(x) and f(x) mean the same thing.
			if (is_function_pointer(v.type)) return v;
			if (!is_scalar_pointer(v.type)) fail("only a pointer can be dereferenced");
			const int target = target_of(v.type);
			// A pointer that is still in memory need not be loaded at all: where
			// it points is remembered instead, and read when something asks.
			if (in_memory(v) && !is_array(v.type))
			{
				const address_source source = source_of(v);
				v.kind = val_deref;
				v.address = source;
				v.bias = 0;
				v.type = target;
				return v;
			}
			load(v);
			make_deref(v, target);
			return v;
		}
		if (k == tok::plus)
		{
			advance();
			return unary_expression();
		}
		if (k == tok::minus)
		{
			advance();
			value v = unary_expression();
			if (v.kind == val_const)
			{
				v.constant = -v.constant;
				v.real = -v.real;
				return v;
			}
			const bool wide = is_wide(v.type);
			load(v);
			if (wide) e_.negate_dx_ax();
			else e_.neg_ax();
			return v;
		}
		if (k == tok::tilde)
		{
			advance();
			value v = unary_expression();
			if (v.kind == val_const)
			{
				v.constant = ~v.constant;
				return v;
			}
			const bool wide = is_wide(v.type);
			load(v);
			e_.not_ax();
			if (wide) e_.not_dx();
			return v;
		}
		if (k == tok::bang)
		{
			advance();
			value v = unary_expression();
			if (v.kind == val_const)
			{
				v.constant = v.constant == 0 ? 1 : 0;
				return v;
			}
			if (v.kind == val_flags)
			{
				v.offset = int(opposite(condition(v.offset)));
				return v;
			}
			const bool wide = is_wide(v.type);
			load(v);
			if (wide) e_.or_ax_dx();
			else e_.test_ax_ax();
			v.kind = val_flags;
			v.offset = int(condition::equal);
			v.type = t_int_;
			return v;
		}
		if (k == tok::inc || k == tok::dec)
		{
			const bool increment = k == tok::inc;
			advance();
			value v = unary_expression();
			const int step = is_scalar_pointer(v.type) ? type_size(target_of(v.type)) : 1;
			if (is_wide(v.type))
			{
				const value target = v;
				load(v);
				if (increment)
				{
					e_.add_ax_imm(step);
					e_.adc_dx_imm(0);
				}
				else
				{
					e_.sub_ax_imm(step);
					e_.sbb_dx_zero();
				}
				if (target.kind == val_deref) store_indirect(target.type, target.bias);
				else store(target);
				v.kind = val_reg;
				return v;
			}
			if (v.kind == val_deref)
			{
				// Loading through BX leaves the address there, so the store
				// that follows needs nothing on the stack.
				const value target = v;
				load(v);
				if (increment) e_.add_ax_imm(step);
				else e_.sub_ax_imm(step);
				store_indirect(target.type, target.bias);
				v.kind = val_reg;
				return v;
			}
			const value target = v;
			load(v);
			if (increment) e_.add_ax_imm(step);
			else e_.sub_ax_imm(step);
			store(target);
			v.kind = val_reg;
			return v;
		}
		if (k == tok::kw_sizeof)
		{
			advance();
			int measured;
			if (kind() == tok::l_paren && is_type_start(1))
			{
				advance();
				measured = parse_type_specifier();
				skip_qualifiers();
				while (accept(tok::star))
				{
					measured = pointer_to(measured);
					skip_qualifiers();
				}
				if (accept(tok::l_bracket))
				{
					const long count = constant_expression();
					expect(tok::r_bracket, "expected ]");
					measured = array_of(measured, int(count));
				}
				expect(tok::r_paren, "expected )");
			}
			else
			{
				const emission_mark start = mark();
				const value v = unary_expression();
				measured = v.type;
				discard(start);
			}
			value v;
			v.kind = val_const;
			v.type = t_uint_;
			v.constant = type_size(measured);
			return v;
		}

		// A cast. On a 16-bit target it only changes how the value is read, save
		// for the widening or narrowing between 16 and 32 bits.
		if (k == tok::l_paren && is_type_start(1))
		{
			advance();
			int cast = parse_type_specifier();
			skip_qualifiers();
			while (accept(tok::star))
			{
				cast = pointer_to(cast);
				skip_qualifiers();
			}
			expect(tok::r_paren, "expected )");
			value v = unary_expression();
			if (v.kind == val_const)
			{
				// A real only exists at compile time, so a cast is where it
				// turns into the integer the rest of the compiler can use.
				if (is_real(v.type) && !is_real(cast)) v.constant = long(v.real);
				else if (is_real(cast)) v.real = double(v.constant);
				if (!is_wide(cast) && !is_real(cast)) v.constant = long(int16_t(v.constant));
				v.type = cast;
				return v;
			}
			load_converted(v, cast);
			return v;
		}

		return postfix_expression();
	}

	value generator::binary_expression(int precedence)
	{
		value left = unary_expression();

		for (;;)
		{
			const tok op = kind();
			const int level = precedence_of(op);
			if (level == 0 || level <= precedence) return left;
			advance();

			if (op == tok::and_and || op == tok::or_or)
			{
				const int shortcut = e_.label();
				const int done = e_.label();
				const condition first = test_value(left);
				if (op == tok::and_and) e_.jump_unless(first, shortcut);
				else e_.jump_if(first, shortcut);
				value right = binary_expression(level);
				e_.set_from_condition(test_value(right));
				e_.jump(done);
				e_.bind(shortcut);
				e_.mov_ax_imm(op == tok::and_and ? 0 : 1);
				e_.bind(done);
				left.kind = val_reg;
				left.type = t_int_;
				continue;
			}

			// Both operands may still turn out to be constant, but the right
			// one is only known once it has been parsed. The code generated on
			// the way there is thrown away, which is what lets an expression
			// like 16*FRACUNIT or sizeof(a)/sizeof(a[0]) reach a case label or
			// an array bound.
			const bool left_constant = left.kind == val_const && !is_scalar_pointer(left.type);
			const long left_constant_value = left.constant;
			const double left_constant_real = left.real;
			const int left_constant_type = left.type;
			const emission_mark start = mark();

			const bool left_pointer = is_scalar_pointer(left.type);
			const int element = left_pointer ? type_size(target_of(left.type)) : 1;
			const int left_type = left_pointer ? pointer_to(target_of(left.type)) : left.type;
			const bool left_wide = !left_pointer && is_wide(left.type);

			// A real has no register to be loaded into, so the speculative load
			// has to be skipped; if the right side then turns out not to be
			// constant, the fold below cannot happen and that is the error.
			const bool left_real = left_constant && is_real(left.type);

			// Neither operand has to go through the stack if one of them can be
			// put wherever the operator wants it at the end. The right one is
			// parsed first when a glance at the tokens says it will generate
			// nothing; failing that, a left operand that is a constant or sits
			// in memory can wait in place while the right one is generated.
			value right;
			bool right_settled = false;
			if (!left_real && single_operand_ahead(level))
			{
				const size_t saved = at_;
				right = binary_expression(level);
				right_settled = !emitted_since(start) && settled(right);
				if (!right_settled)
				{
					discard(start);
					at_ = saved;
				}
			}

			const bool keep_left = !left_real && !left_wide && fits_cx(left);
			bool pushed = false;
			if (!right_settled)
			{
				if (!left_real && !keep_left)
				{
					load(left);
					if (left_wide) e_.push_dx();
					e_.push_ax();
					pushed = true;
				}
				right = binary_expression(level);
			}

			if (left_real && (right.kind != val_const || is_scalar_pointer(right.type)))
				fail("floating point is only available in a constant expression");

			if (left_constant && right.kind == val_const && !is_scalar_pointer(right.type))
			{
				discard(start);

				// A real folds in double and stays one until something casts
				// it, which is the only way it can reach the code generator.
				if (is_real(left_constant_type) || is_real(right.type))
				{
					const double x = is_real(left_constant_type)
						                 ? left_constant_real
						                 : double(left_constant_value);
					const double y = is_real(right.type) ? right.real : double(right.constant);
					bool comparison = true;
					long verdict = 0;
					double folded = 0;
					switch (op)
					{
					case tok::plus: folded = x + y; comparison = false; break;
					case tok::minus: folded = x - y; comparison = false; break;
					case tok::star: folded = x * y; comparison = false; break;
					case tok::slash:
						folded = y != 0 ? x / y : 0;
						comparison = false;
						break;
					case tok::eq: verdict = x == y; break;
					case tok::ne: verdict = x != y; break;
					case tok::less: verdict = x < y; break;
					case tok::le: verdict = x <= y; break;
					case tok::greater: verdict = x > y; break;
					case tok::ge: verdict = x >= y; break;
					default: fail("that operator needs whole numbers");
					}
					left.kind = val_const;
					if (comparison)
					{
						left.type = t_int_;
						left.constant = verdict;
						left.real = double(verdict);
					}
					else
					{
						left.type = t_real_;
						left.real = folded;
						left.constant = long(folded);
					}
					continue;
				}

				const long a = left_constant_value;
				const long b = right.constant;
				const bool either_unsigned = is_unsigned(left_constant_type) || is_unsigned(right.type);
				const unsigned long ua = static_cast<unsigned long>(a);
				const unsigned long ub = static_cast<unsigned long>(b);
				long result = 0;
				switch (op)
				{
				case tok::plus: result = a + b; break;
				case tok::minus: result = a - b; break;
				case tok::star: result = a * b; break;
				case tok::slash:
					if (b == 0) result = 0;
					else result = either_unsigned ? long(ua / ub) : a / b;
					break;
				case tok::percent:
					if (b == 0) result = 0;
					else result = either_unsigned ? long(ua % ub) : a % b;
					break;
				case tok::amp: result = a & b; break;
				case tok::pipe: result = a | b; break;
				case tok::caret: result = a ^ b; break;
				case tok::shl: result = long(ua << (ub & 31)); break;
				case tok::shr: result = either_unsigned ? long(ua >> (ub & 31)) : a >> (b & 31); break;
				case tok::eq: result = a == b; break;
				case tok::ne: result = a != b; break;
				case tok::less: result = either_unsigned ? ua < ub : a < b; break;
				case tok::le: result = either_unsigned ? ua <= ub : a <= b; break;
				case tok::greater: result = either_unsigned ? ua > ub : a > b; break;
				default: result = either_unsigned ? ua >= ub : a >= b; break;
				}

				left.kind = val_const;
				left.constant = result;
				if (level == 6 || level == 7) left.type = t_int_;
				else if (is_wide(left_constant_type) || is_wide(right.type))
					left.type = either_unsigned ? t_ulong_ : t_long_;
				else if (result > 65535 || result < -32768) left.type = either_unsigned ? t_ulong_ : t_long_;
				else left.type = either_unsigned ? t_uint_ : t_int_;
				continue;
			}

			const bool right_pointer = is_scalar_pointer(right.type);

			// A pointer is 16-bit, so an index that happens to be long is
			// simply truncated rather than dragging the whole operation wide.
			const bool wide = !left_pointer && !right_pointer && (left_wide || is_wide(right.type));
			const bool unsigned_operation = is_unsigned(left.type) || is_scalar_pointer(left.type) ||
				is_unsigned(right.type) || is_scalar_pointer(right.type);
			const bool scaled = left_pointer && !right_pointer && element != 1 &&
				(op == tok::plus || op == tok::minus);

			// The right operand can be placed last, and a constant then folds
			// into the instruction itself.
			const bool place_right = right_settled && !pushed &&
				(!scaled || right.kind == val_const) &&
				(wide ? fits_cx_bx(right) : fits_cx(right));

			if (place_right)
			{
				if (wide)
				{
					long constant = right.constant;
					if (!is_wide(right.type) && !is_unsigned(right.type))
						constant = long(int16_t(constant));

					// A shift of 16 or more throws the low half away, so it is
					// never loaded. This is the fixed-point idiom.
					const bool high_only = left_wide && in_memory(left) &&
						right.kind == val_const && op == tok::shr &&
						(constant & 31) >= 16;
					if (high_only)
					{
						if (left.kind == val_local) e_.load_ax_frame(left.offset + 2);
						else
						{
							e_.load_ax_absolute(0);
							data_reference(left.offset + 2);
						}
						if (unsigned_operation) e_.xor_dx_dx();
						else e_.cwd();
						shift_ax_constant(tok::shr, int(constant & 31) - 16, unsigned_operation);
						left.kind = val_reg;
					}
					else
					{
						load(left);
						if (!left_wide) widen(left.type);
						if (right.kind != val_const ||
							!apply_operator_wide_constant(op, constant, unsigned_operation))
						{
							load_to_cx_bx(right);
							apply_operator_wide(op, unsigned_operation);
						}
					}
				}
				else
				{
					load(left);
					if (right.kind == val_const)
					{
						if (scaled) right.constant *= element;
						if (!apply_operator_constant(op, right.constant, unsigned_operation))
						{
							load_to_cx(right);
							apply_operator(op, unsigned_operation);
						}
					}
					else if (op == tok::plus && add_address_to_ax(right))
					{
					}
					else
					{
						load_to_cx(right);
						apply_operator(op, unsigned_operation);
					}
				}
			}
			else if (keep_left)
			{
				// The left operand stayed where it was, so the right one is in
				// AX and the operands are the wrong way round.
				load(right);
				if (wide)
				{
					if (!is_wide(right.type)) widen(right.type);
					e_.mov_cx_dx();
					e_.mov_bx_ax();
					load(left);
					widen(left.type); // a wide left would have been pushed
					apply_operator_wide(op, unsigned_operation);
				}
				else
				{
					if (scaled) scale_ax(element);
					load_to_cx(left);
					apply_operator(op, unsigned_operation, true);
				}
			}
			else
			{
				if (!pushed && !left_real)
				{
					load(left);
					if (left_wide) e_.push_dx();
					e_.push_ax();
				}
				load(right);
				if (scaled) scale_ax(element);

				if (wide)
				{
					if (!is_wide(right.type)) widen(right.type);
					e_.mov_cx_dx();
					e_.mov_bx_ax();
					e_.pop_ax();
					if (left_wide) e_.pop_dx();
					else widen(left.type);
					apply_operator_wide(op, unsigned_operation);
				}
				else
				{
					e_.mov_cx_ax();
					e_.pop_ax();
					if (left_wide) e_.pop_dx(); // the left operand was pushed as a pair
					apply_operator(op, unsigned_operation);
				}
			}

			value result;
			result.kind = val_reg;
			result.type = t_int_;
			if (is_comparison(op))
			{
				// The comparison is only in the flags. Whoever wants it as a
				// number will ask, and a branch will not have to.
				result.kind = val_flags;
				result.offset = int(condition_of(op, wide ? false : unsigned_operation));
			}
			else if (left_pointer && right_pointer && op == tok::minus)
			{
				if (element != 1)
				{
					e_.mov_cx_imm(element);
					e_.idiv_cx();
				}
			}
			else if (left_pointer && (op == tok::plus || op == tok::minus))
			{
				result.type = left_type;
			}
			else if (wide)
			{
				// Every 32-bit operator keeps the width, and a shift keeps the
				// type of what was shifted.
				if (op == tok::shl || op == tok::shr) result.type = left.type;
				else result.type = unsigned_operation ? t_ulong_ : t_long_;
			}
			else if (precedence_of(op) >= 8)
			{
				result.type = left.type;
			}
			left = result;
		}
	}

	value generator::conditional_expression()
	{
		value v = binary_expression(0);
		if (kind() != tok::question) return v;
		advance();

		// A condition that is already known picks a branch here, so the whole
		// expression can still be a constant. Both arms are parsed either way.
		if (v.kind == val_const)
		{
			const bool taken = v.constant != 0;
			emission_mark start = mark();
			value then_value = expression();
			if (!taken) discard(start);
			expect(tok::colon, "expected : in a conditional expression");
			start = mark();
			value else_value = conditional_expression();
			if (taken) discard(start);
			value chosen = taken ? then_value : else_value;
			if (chosen.kind != val_const) load(chosen);
			return chosen;
		}

		const int otherwise = e_.label();
		const int done = e_.label();
		e_.jump_unless(test_value(v), otherwise);
		// The middle arm is a full expression, so a comma belongs to it rather
		// than ending the argument or declarator the conditional sits in.
		value then_value = expression();
		load(then_value);
		e_.jump(done);
		expect(tok::colon, "expected : in a conditional expression");
		e_.bind(otherwise);
		value else_value = conditional_expression();
		load_converted(else_value, then_value.type);
		e_.bind(done);

		value result;
		result.kind = val_reg;
		result.type = then_value.type;
		return result;
	}

	value generator::assignment_expression()
	{
		value target = conditional_expression();
		const tok op = kind();

		const bool simple = op == tok::assign;
		const bool compound = op == tok::plus_assign || op == tok::minus_assign || op == tok::star_assign ||
			op == tok::slash_assign || op == tok::percent_assign || op == tok::amp_assign ||
			op == tok::pipe_assign || op == tok::caret_assign || op == tok::shl_assign ||
			op == tok::shr_assign;
		if (!simple && !compound) return target;

		if (target.kind != val_local && target.kind != val_global && target.kind != val_deref)
			fail("cannot assign to this expression");
		advance();

		if (simple)
		{
			if (target.kind == val_deref && target.address == addr_reg) e_.push_ax();
			value source = assignment_expression();
			load_converted(source, target.type);
			store(target);
			value result;
			result.kind = val_reg;
			result.type = target.type;
			return result;
		}

		static const struct
		{
			tok assign;
			tok binary;
		} mapping[] = {
			{tok::plus_assign, tok::plus}, {tok::minus_assign, tok::minus},
			{tok::star_assign, tok::star}, {tok::slash_assign, tok::slash},
			{tok::percent_assign, tok::percent}, {tok::amp_assign, tok::amp},
			{tok::pipe_assign, tok::pipe}, {tok::caret_assign, tok::caret},
			{tok::shl_assign, tok::shl}, {tok::shr_assign, tok::shr},
		};
		tok binary = tok::plus;
		for (const auto& entry : mapping) if (entry.assign == op) binary = entry.binary;

		const bool pointer_target = is_scalar_pointer(target.type);
		const int element = pointer_target ? type_size(target_of(target.type)) : 1;
		const bool wide = !pointer_target && is_wide(target.type);

		// The source can be placed after the target is in AX when it generates
		// nothing, which is what lets x += 1 be a single instruction.
		const emission_mark start = mark();
		const size_t saved = at_;
		value source;
		bool source_settled = false;
		if (single_operand_ahead(0))
		{
			source = assignment_expression();
			source_settled = !emitted_since(start) && settled(source) &&
				(wide ? fits_cx_bx(source) : fits_cx(source)) &&
				(element == 1 || source.kind == val_const);
			if (!source_settled)
			{
				discard(start);
				at_ = saved;
			}
		}

		if (target.kind == val_deref && target.address == addr_reg) e_.push_ax();
		value current = target;
		load(current);

		if (source_settled)
		{
			const bool unsigned_operation = is_unsigned(target.type) || is_scalar_pointer(target.type) ||
				is_unsigned(source.type);
			long constant = source.constant;
			if (source.kind == val_const)
			{
				if (!is_wide(source.type) && !is_unsigned(source.type))
					constant = long(int16_t(constant));
				if (pointer_target && element != 1 &&
					(binary == tok::plus || binary == tok::minus))
					constant *= element;
			}
			const bool folded = source.kind == val_const &&
				(wide
					 ? apply_operator_wide_constant(binary, constant, unsigned_operation)
					 : apply_operator_constant(binary, constant, unsigned_operation));
			if (!folded)
			{
				if (source.kind == val_const) source.constant = constant;
				if (wide)
				{
					load_to_cx_bx(source);
					apply_operator_wide(binary, unsigned_operation);
				}
				else
				{
					load_to_cx(source);
					apply_operator(binary, unsigned_operation);
				}
			}
			store(target);

			value result;
			result.kind = val_reg;
			result.type = target.type;
			return result;
		}

		if (wide) e_.push_dx();
		e_.push_ax();
		source = assignment_expression();
		load(source);
		if (pointer_target && element != 1 && (binary == tok::plus || binary == tok::minus))
			scale_ax(element);
		if (wide)
		{
			if (!is_wide(source.type)) widen(source.type);
			e_.mov_cx_dx();
			e_.mov_bx_ax();
			e_.pop_ax();
			e_.pop_dx();
			apply_operator_wide(binary, is_unsigned(target.type) || is_unsigned(source.type));
		}
		else
		{
			e_.mov_cx_ax();
			e_.pop_ax();
			apply_operator(binary, is_unsigned(target.type) || is_unsigned(source.type));
		}
		store(target);

		value result;
		result.kind = val_reg;
		result.type = target.type;
		return result;
	}

	value generator::expression()
	{
		value v = assignment_expression();
		while (kind() == tok::comma)
		{
			advance();
			v = assignment_expression();
		}
		return v;
	}

	long generator::constant_expression()
	{
		const emission_mark start = mark();
		const value v = conditional_expression();
		discard(start);
		if (v.kind != val_const) fail("expected a constant expression");
		return v.constant;
	}

	// -----------------------------------------------------------------------
	// Declarations
	// -----------------------------------------------------------------------

	bool generator::is_type_start(int ahead) const
	{
		switch (peek(ahead).kind)
		{
		case tok::kw_void: case tok::kw_char: case tok::kw_int: case tok::kw_unsigned:
		case tok::kw_signed: case tok::kw_short: case tok::kw_long: case tok::kw_struct:
		case tok::kw_union: case tok::kw_enum: case tok::kw_const: case tok::kw_qualifier:
		case tok::kw_static: case tok::kw_extern: case tok::kw_typedef:
			return true;
		case tok::identifier:
		{
			const int index = find(peek(ahead).text);
			return index >= 0 && symbols_[size_t(index)].kind == sym_typedef;
		}
		default:
			return false;
		}
	}

	int generator::parse_record(bool is_union)
	{
		std::string name;
		if (kind() == tok::identifier)
		{
			name = peek().text;
			advance();
		}

		// A named record that is already known and not being defined here.
		if (!name.empty() && kind() != tok::l_brace)
		{
			for (size_t i = 0; i < records_.size(); ++i)
			{
				if (records_[i].name != name) continue;
				for (size_t t = 0; t < types_.size(); ++t)
					if (types_[t].kind == ty_record && types_[t].record == int(i)) return int(t);
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
			for (size_t i = 0; i < records_.size(); ++i)
			{
				if (records_[i].name != name || !records_[i].members.empty()) continue;
				record_index = int(i);
				for (size_t t = 0; t < types_.size(); ++t)
					if (types_[t].kind == ty_record && types_[t].record == record_index) type_index = int(t);
				break;
			}
		}

		if (record_index < 0)
		{
			records_.push_back(record_info());
			record_index = int(records_.size()) - 1;
			records_[size_t(record_index)].name = name;
			records_[size_t(record_index)].is_union = is_union;

			type_info info;
			info.kind = ty_record;
			info.record = record_index;
			info.size = 0;
			type_index = add_type(info);
		}

		if (!accept(tok::l_brace)) return type_index;
		records_[size_t(record_index)].is_union = is_union;

		int offset = 0;
		int largest = 0;
		while (!accept(tok::r_brace))
		{
			if (kind() == tok::end) fail("unterminated structure");
			const int base = parse_type_specifier();
			for (;;)
			{
				std::string member_name;
				bool is_function = false;
				std::vector<int> parameters;
				std::vector<std::string> parameter_names;
				const int member_type = parse_declarator(base, member_name, is_function, parameters,
				                                         parameter_names);
				if (member_name.empty()) fail("expected a member name");

				record_member member;
				member.name = member_name;
				member.type = member_type;
				member.offset = is_union ? 0 : offset;
				records_[size_t(record_index)].members.push_back(member);

				const int size = type_size(member_type);
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
				if (!accept(tok::comma)) break;
			}
			expect(tok::semicolon, "expected ; after a member");
		}

		records_[size_t(record_index)].size = is_union ? largest : offset;
		types_[size_t(type_index)].size = records_[size_t(record_index)].size;
		return type_index;
	}

	int generator::parse_enum()
	{
		if (kind() == tok::identifier) advance();
		if (accept(tok::l_brace))
		{
			long next = 0;
			while (!accept(tok::r_brace))
			{
				if (kind() != tok::identifier) fail("expected an enumeration name");
				const std::string name = peek().text;
				advance();
				if (accept(tok::assign)) next = constant_expression();
				const int index = define(name, sym_constant, t_int_);
				symbols_[size_t(index)].value = next;
				++next;
				if (!accept(tok::comma) && kind() != tok::r_brace) fail("expected , in an enumeration");
			}
		}
		return t_int_;
	}

	int generator::parse_type_specifier()
	{
		bool is_unsigned_type = false;
		bool seen_sign = false;
		skip_qualifiers();

		if (kind() == tok::kw_unsigned)
		{
			advance();
			is_unsigned_type = true;
			seen_sign = true;
		}
		else if (kind() == tok::kw_signed)
		{
			advance();
			seen_sign = true;
		}

		skip_qualifiers();

		switch (kind())
		{
		case tok::kw_void:
			advance();
			return t_void_;
		case tok::kw_char:
			advance();
			return is_unsigned_type ? t_uchar_ : t_char_;
		case tok::kw_short:
			advance();
			if (kind() == tok::kw_int) advance();
			return is_unsigned_type ? t_uint_ : t_int_;
		case tok::kw_long:
			advance();
			while (kind() == tok::kw_long) advance(); // long long is 32-bit here too
			if (kind() == tok::kw_int) advance();
			return is_unsigned_type ? t_ulong_ : t_long_;
		case tok::kw_int:
			advance();
			return is_unsigned_type ? t_uint_ : t_int_;
		case tok::kw_struct:
			advance();
			return parse_record(false);
		case tok::kw_union:
			advance();
			return parse_record(true);
		case tok::kw_enum:
			advance();
			return parse_enum();
		case tok::identifier:
		{
			const int index = find(peek().text);
			if (index >= 0 && symbols_[size_t(index)].kind == sym_typedef)
			{
				advance();
				return symbols_[size_t(index)].type;
			}
			break;
		}
		default:
			break;
		}

		if (seen_sign) return is_unsigned_type ? t_uint_ : t_int_;
		fail("expected a type");
	}

	int generator::parse_declarator(int base, std::string& name, bool& is_function,
	                                std::vector<int>& parameters, std::vector<std::string>& parameter_names)
	{
		skip_qualifiers();
		while (accept(tok::star))
		{
			base = pointer_to(base);
			skip_qualifiers();
		}

		// A parenthesised declarator, as in int (*f)(int). The group binds to
		// whatever the suffix after it builds, so skip it, build that, then
		// come back and apply the group to it.
		if (kind() == tok::l_paren && (peek(1).kind == tok::star || peek(1).kind == tok::l_paren))
		{
			const size_t group = at_;
			int depth = 0;
			do
			{
				if (kind() == tok::end) fail("unterminated declarator");
				if (kind() == tok::l_paren) ++depth;
				else if (kind() == tok::r_paren) --depth;
				advance();
			}
			while (depth > 0);

			const int inner_base = parse_declarator_suffix(base, is_function, parameters, parameter_names);
			const size_t after = at_;

			at_ = group + 1;
			bool nested = false;
			std::vector<int> nested_parameters;
			std::vector<std::string> nested_names;
			const int declared = parse_declarator(inner_base, name, nested, nested_parameters, nested_names);
			expect(tok::r_paren, "expected ) in a declarator");
			at_ = after;
			is_function = false; // the parentheses belonged to the pointer, not to a definition
			return declared;
		}

		if (kind() == tok::identifier)
		{
			name = peek().text;
			advance();
		}

		return parse_declarator_suffix(base, is_function, parameters, parameter_names);
	}

	int generator::parse_declarator_suffix(int base, bool& is_function, std::vector<int>& parameters,
	                                       std::vector<std::string>& parameter_names)
	{
		if (kind() == tok::l_paren)
		{
			advance();
			is_function = true;
			if (kind() == tok::kw_void && peek(1).kind == tok::r_paren) advance();
			while (kind() != tok::r_paren)
			{
				if (kind() == tok::end) fail("unterminated parameter list");
				// The extra arguments are found through __args, so the ellipsis
				// itself needs nothing beyond being allowed here.
				if (accept(tok::ellipsis)) break;
				const int parameter_base = parse_type_specifier();
				std::string parameter_name;
				bool nested = false;
				std::vector<int> nested_parameters;
				std::vector<std::string> nested_names;
				int parameter_type = parse_declarator(parameter_base, parameter_name, nested,
				                                      nested_parameters, nested_names);
				// A parameter declared as an array or a function is really a pointer.
				if (is_array(parameter_type)) parameter_type = pointer_to(target_of(parameter_type));
				else if (is_function_type(parameter_type)) parameter_type = pointer_to(parameter_type);
				parameters.push_back(parameter_type);
				parameter_names.push_back(parameter_name);
				if (!accept(tok::comma)) break;
			}
			expect(tok::r_paren, "expected ) after the parameter list");
			return function_type(base, parameters);
		}

		std::vector<int> dimensions;
		while (accept(tok::l_bracket))
		{
			if (kind() == tok::r_bracket) dimensions.push_back(0);
			else dimensions.push_back(int(constant_expression()));
			expect(tok::r_bracket, "expected ]");
		}
		for (size_t i = dimensions.size(); i > 0; --i) base = array_of(base, dimensions[i - 1]);
		return base;
	}

	// The length of an array declared without one, counted from the initialiser
	// that follows. Only the outermost dimension can be left open, so nothing
	// below the first level of braces is counted.
	int generator::initialiser_length() const
	{
		if (peek(1).kind == tok::string) return int(peek(1).text.size()) + 1;
		if (peek(1).kind != tok::l_brace) return 1;

		int depth = 0;
		int count = 0;
		bool pending = false;
		for (size_t scan = at_ + 1; scan < tokens_.tokens.size(); ++scan)
		{
			const tok k = tokens_.tokens[scan].kind;
			if (k == tok::end) break;
			if (k == tok::l_brace || k == tok::l_paren || k == tok::l_bracket)
			{
				++depth;
				if (depth > 1) pending = true;
				continue;
			}
			if (k == tok::r_brace || k == tok::r_paren || k == tok::r_bracket)
			{
				if (--depth == 0) break;
				continue;
			}
			if (depth == 1 && k == tok::comma)
			{
				if (pending) ++count;
				pending = false;
				continue;
			}
			pending = true;
		}
		if (pending) ++count;
		return count > 0 ? count : 1;
	}

	// A constant address: an object, optionally cast, indexed or offset by a
	// constant number of elements. It cannot go through the ordinary constant
	// folder because the address is only known once the data section is placed.
	bool generator::address_operand(int& symbol, int& pointee, long& offset)
	{
		if (kind() == tok::l_paren && is_type_start(1))
		{
			advance();
			int cast = parse_type_specifier();
			skip_qualifiers();
			while (accept(tok::star))
			{
				cast = pointer_to(cast);
				skip_qualifiers();
			}
			if (!accept(tok::r_paren)) return false;
			if (!address_operand(symbol, pointee, offset)) return false;
			// The cast is what a following +1 counts in.
			pointee = is_scalar_pointer(cast) ? target_of(cast) : cast;
			return true;
		}
		if (kind() == tok::l_paren)
		{
			advance();
			if (!address_operand(symbol, pointee, offset)) return false;
			if (!address_offset(pointee, offset)) return false;
			return accept(tok::r_paren);
		}

		const bool address_of = accept(tok::amp);
		if (kind() != tok::identifier) return false;
		const int index = find(peek().text);
		if (index < 0 || symbols_[size_t(index)].kind != sym_global) return false;
		advance();
		symbol = index;

		int current = symbols_[size_t(index)].type;
		for (;;)
		{
			if (accept(tok::l_bracket))
			{
				if (!is_scalar_pointer(current)) return false;
				const int element = target_of(current);
				const emission_mark start = mark();
				const value subscript = conditional_expression();
				discard(start);
				if (subscript.kind != val_const || !accept(tok::r_bracket)) return false;
				offset += subscript.constant * type_size(element);
				current = element;
				continue;
			}
			if (kind() == tok::dot && peek(1).kind == tok::identifier)
			{
				if (!is_record(current)) return false;
				const record_info& info = records_[size_t(type(current).record)];
				const record_member* found = nullptr;
				for (const record_member& m : info.members) if (m.name == peek(1).text) found = &m;
				if (found == nullptr) return false;
				offset += found->offset;
				current = found->type;
				advance();
				advance();
				continue;
			}
			break;
		}

		// Only an array turns into an address on its own.
		if (!address_of && !is_array(current)) return false;
		pointee = is_array(current) ? target_of(current) : current;
		return true;
	}

	bool generator::address_offset(int pointee, long& offset)
	{
		while (kind() == tok::plus || kind() == tok::minus)
		{
			const bool add = kind() == tok::plus;
			advance();
			const emission_mark start = mark();
			const value step = binary_expression(precedence_of(tok::plus));
			discard(start);
			if (step.kind != val_const) return false;
			offset += (add ? 1 : -1) * step.constant * type_size(pointee);
		}
		return true;
	}

	bool generator::address_constant(int at)
	{
		const size_t saved = at_;
		int symbol = -1;
		int pointee = t_char_;
		long offset = 0;
		if (address_operand(symbol, pointee, offset) && address_offset(pointee, offset))
		{
			const tok after = kind();
			if (after == tok::comma || after == tok::r_brace || after == tok::semicolon)
			{
				data_pointer(at, int(symbols_[size_t(symbol)].offset + offset));
				return true;
			}
		}
		at_ = saved;
		return false;
	}

	void generator::global_initialiser(int type_index, int offset)
	{
		const auto put = [&](int at, int width, long v)
		{
			if (at < 0 || size_t(at) + size_t(width) > data_.size()) fail("the initialiser does not fit");
			for (int i = 0; i < width; ++i) data_[size_t(at) + size_t(i)] = uint8_t((v >> (8 * i)) & 0xff);
		};

		// A function name in an initialiser is its address, which is not known
		// until every body has been generated.
		const auto function_address = [&](int at) -> bool
		{
			const int name = kind() == tok::amp && peek(1).kind == tok::identifier ? 1 : 0;
			if (peek(name).kind != tok::identifier) return false;
			const int index = find(peek(name).text);
			if (index < 0 || symbols_[size_t(index)].kind != sym_function) return false;
			if (peek(name + 1).kind == tok::l_paren) return false;
			for (int i = 0; i <= name; ++i) advance();
			function_fixups_.push_back({at, index});
			symbols_[size_t(index)].called = true;
			return true;
		};

		// The address of another global, handled by address_constant so that a
		// cast, an index or a constant element offset all come out right.

		if (kind() == tok::string && is_array(type_index) && type_size(target_of(type_index)) == 1)
		{
			const std::string text = peek().text;
			advance();
			if (offset < 0 || size_t(offset) + text.size() > data_.size()) fail("the initialiser does not fit");
			for (size_t i = 0; i < text.size() && int(i) < type_size(type_index); ++i)
				data_[size_t(offset) + i] = uint8_t(text[i]);
			return;
		}

		// A braced list follows the shape of the type it initialises, so an
		// array of structures needs no special case beyond recursion. Data
		// starts out zero, which is what a short list should leave behind.
		if (accept(tok::l_brace))
		{
			if (is_array(type_index) || is_record(type_index))
			{
				const bool array = is_array(type_index);
				const int element = array ? target_of(type_index) : 0;
				const int stride = array ? type_size(element) : 0;
				const int count = array ? type(type_index).count : 0;
				const int record = array ? -1 : type(type_index).record;
				int index = 0;
				while (!accept(tok::r_brace))
				{
					if (kind() == tok::end) fail("unterminated initialiser");
					if (array)
					{
						if (count > 0 && index >= count) fail("too many initialisers");
						global_initialiser(element, offset + index * stride);
					}
					else
					{
						if (index >= int(records_[size_t(record)].members.size()))
							fail("too many initialisers");
						const record_member member = records_[size_t(record)].members[size_t(index)];
						global_initialiser(member.type, offset + member.offset);
					}
					++index;
					if (!accept(tok::comma) && kind() != tok::r_brace) fail("expected , in an initialiser");
				}
				return;
			}

			// A braced scalar, which C allows and headers occasionally use.
			global_initialiser(type_index, offset);
			expect(tok::r_brace, "expected } in an initialiser");
			return;
		}

		// A string used to initialise a char pointer becomes its address, so
		// it needs a fixup once the data section is placed.
		if (kind() == tok::string && is_pointer(type_index))
		{
			const int text_offset = add_string(peek().text);
			advance();
			data_pointer(offset, text_offset);
			return;
		}

		if (function_address(offset)) return;
		if (address_constant(offset)) return;

		const long v = constant_expression();
		put(offset, type_size(type_index), v);
	}

	void generator::emit_return()
	{
		e_.mov_sp_bp();
		e_.pop_bp();
		e_.ret();
	}

	// Where a variable lives, as an operand the back end can work on directly.
	operand generator::place_of(const value& target, int extra)
	{
		if (target.kind == val_local) return emitter::frame(target.offset + extra);
		return operand::absolute(0);
	}

	// An operation applied straight to a variable in memory.
	void generator::memory_operation(op kind, int width, const value& target, const operand& source,
	                                 int extra)
	{
		e_.emit(kind, width, place_of(target, extra), source);
		if (target.kind != val_local) data_reference(target.offset + extra);
	}

	// A statement whose result nobody wants - i++, x += 8, p = p + 1 - is one
	// instruction against memory rather than a load, an operation and a store.
	// Recognised from the tokens, because that is the only place the fact that
	// the value is being thrown away is visible.
	bool generator::in_place_statement(tok terminator)
	{
		const size_t start = at_;
		const auto token = [&](size_t ahead)
		{
			const size_t index = start + ahead;
			return index < tokens_.tokens.size() ? tokens_.tokens[index].kind : tok::end;
		};

		size_t name = start;
		size_t rest = 0; // where the source operand begins
		tok op = tok::plus;
		bool step_only = false;
		bool increment = true;

		if ((token(0) == tok::inc || token(0) == tok::dec) && token(1) == tok::identifier &&
			token(2) == terminator)
		{
			increment = token(0) == tok::inc;
			name = start + 1;
			step_only = true;
		}
		else if (token(0) == tok::identifier && (token(1) == tok::inc || token(1) == tok::dec) &&
			token(2) == terminator)
		{
			increment = token(1) == tok::inc;
			step_only = true;
		}

		if (!step_only)
		{
			// NAME op= <one token> and NAME = NAME op <one token>
			static const struct
			{
				tok assign;
				tok binary;
			} compound[] = {
				{tok::plus_assign, tok::plus}, {tok::minus_assign, tok::minus},
				{tok::amp_assign, tok::amp}, {tok::pipe_assign, tok::pipe},
				{tok::caret_assign, tok::caret},
			};

			if (token(0) != tok::identifier) return false;
			bool found = false;
			for (const auto& entry : compound)
			{
				if (token(1) != entry.assign) continue;
				op = entry.binary;
				rest = 2;
				found = true;
			}
			if (!found)
			{
				if (token(1) != tok::assign || token(2) != tok::identifier) return false;
				if (tokens_.tokens[start].text != tokens_.tokens[start + 2].text) return false;
				const tok binary = token(3);
				if (binary != tok::plus && binary != tok::minus && binary != tok::amp &&
					binary != tok::pipe && binary != tok::caret)
					return false;
				op = binary;
				rest = 4;
			}
			if (token(rest + 1) != terminator) return false;
			const tok source_kind = token(rest);
			if (source_kind != tok::number && source_kind != tok::character &&
				source_kind != tok::identifier)
				return false;
		}

		const int index = find(tokens_.tokens[name].text);
		if (index < 0) return false;
		const symbol& entry = symbols_[size_t(index)];
		if (entry.kind != sym_local && entry.kind != sym_global) return false;
		if (is_array(entry.type) || is_record(entry.type) || is_real(entry.type)) return false;
		const int width = type_size(entry.type);
		if (width != 1 && width != 2 && width != 4) return false;

		value target;
		target.kind = entry.kind == sym_local ? val_local : val_global;
		target.type = entry.type;
		target.offset = entry.offset;

		long constant = 0;
		bool constant_source = true;
		value source;
		if (step_only)
		{
			constant = is_scalar_pointer(entry.type) ? type_size(target_of(entry.type)) : 1;
			op = increment ? tok::plus : tok::minus;
		}
		else
		{
			// Reading the source must generate nothing, which the single-token
			// check above has already made near certain.
			const size_t saved = at_;
			at_ = start + rest;
			const emission_mark before = mark();
			source = unary_expression();
			const bool clean = !emitted_since(before) && settled(source);
			at_ = saved;
			if (!clean)
			{
				discard(before);
				return false;
			}
			constant_source = source.kind == val_const;
			if (constant_source)
			{
				constant = source.constant;
				if (!is_wide(source.type) && !is_unsigned(source.type))
					constant = long(int16_t(constant));
				if (is_scalar_pointer(entry.type) && (op == tok::plus || op == tok::minus))
					constant *= type_size(target_of(entry.type));
			}
			else
			{
				// Only a source of the same width can be applied in place.
				if (!in_memory(source) || is_array(source.type) || is_record(source.type)) return false;
				if (type_size(source.type) != width || width == 1) return false;
			}
		}

		::op kind = ::op::add;
		switch (op)
		{
		case tok::plus: kind = ::op::add; break;
		case tok::minus: kind = ::op::sub; break;
		case tok::amp: kind = ::op::and_; break;
		case tok::pipe: kind = ::op::or_; break;
		default: kind = ::op::xor_; break;
		}
		const bool wide = width == 4;

		// The carry between the halves only matters for + and -.
		::op high_kind = kind;
		if (op == tok::plus) high_kind = ::op::adc;
		else if (op == tok::minus) high_kind = ::op::sbb;

		if (constant_source)
		{
			const int low = int(constant & 0xffff);
			const int high = int((constant >> 16) & 0xffff);
			if (!wide && (op == tok::plus || op == tok::minus) && (low == 1 || low == 0xffff))
			{
				const bool up = (op == tok::plus) == (low == 1);
				memory_operation(up ? ::op::inc : ::op::dec, width, target, {});
			}
			else
			{
				memory_operation(kind, width, target, operand::constant(low));
				if (wide) memory_operation(high_kind, 2, target, operand::constant(high), 2);
			}
		}
		else
		{
			load(source);
			memory_operation(kind, wide ? 2 : width, target, operand::in(reg_a));
			if (wide) memory_operation(high_kind, 2, target, operand::in(reg_d), 2);
		}

		at_ = start + (step_only ? 2 : rest + 1);
		return true;
	}

	void generator::function_body(int function_index, const std::vector<int>& parameters,
	                              const std::vector<std::string>& parameter_names)
	{
		e_.bind(label_of(function_index));
		symbols_[size_t(function_index)].defined = true;
		symbols_[size_t(function_index)].start = symbols_[size_t(function_index)].label;
		symbols_[size_t(function_index)].body = int(at_);
		symbols_[size_t(function_index)].parameter_names = parameter_names;
		return_type_ = symbols_[size_t(function_index)].type;
		current_function_ = function_index;

		e_.push_bp();
		e_.mov_bp_sp();
		e_.sub_sp_imm(0);
		frame_patch_ = e_.refers_to_deferred();
		frame_ = 0;

		++scope_;
		int offset = 4; // return address and the saved BP
		for (size_t i = 0; i < parameters.size(); ++i)
		{
			const int width = is_wide(parameters[i]) ? 4 : 2;
			if (!parameter_names[i].empty())
			{
				const int index = define(parameter_names[i], sym_local, parameters[i]);
				symbols_[size_t(index)].offset = offset;
			}
			offset += width;
		}

		expect(tok::l_brace, "expected { to open the function body");
		labels_.clear();
		pending_gotos_.clear();
		while (!accept(tok::r_brace))
		{
			if (kind() == tok::end) fail("unterminated function body");
			statement();
		}

		for (const auto& entry : pending_gotos_)
		{
			const auto found = labels_.find(entry.first);
			if (found == labels_.end() || !e_.is_bound(found->second))
				fail(("undefined label: " + entry.first).c_str());
		}
		pending_gotos_.clear();

		e_.set_deferred(frame_patch_, frame_);
		symbols_[size_t(function_index)].frame = frame_;
		emit_return();
		symbols_[size_t(function_index)].finish = e_.here();
		leave_scope(--scope_);
		hoisted_ = false;
		current_function_ = -1;
	}

	void generator::declaration()
	{
		bool is_typedef = false;
		bool is_static = false;
		while (kind() == tok::kw_static || kind() == tok::kw_extern || kind() == tok::kw_typedef ||
			kind() == tok::kw_qualifier)
		{
			if (kind() == tok::kw_typedef) is_typedef = true;
			if (kind() == tok::kw_static) is_static = true;
			advance();
		}

		const int base = parse_type_specifier();
		if (accept(tok::semicolon)) return; // a bare struct or enum declaration

		for (;;)
		{
			std::string name;
			bool is_function = false;
			std::vector<int> parameters;
			std::vector<std::string> parameter_names;
			int declared = parse_declarator(base, name, is_function, parameters, parameter_names);
			if (name.empty()) fail("expected a name");

			// An array declared without a length takes it from its initialiser,
			// which has to be measured before any storage is reserved.
			if (!is_function && !is_typedef && is_array(declared) && type(declared).count == 0 &&
				kind() == tok::assign)
				declared = array_of(target_of(declared), initialiser_length());

			if (is_typedef)
			{
				define(name, sym_typedef, declared);
			}
			else if (is_function)
			{
				int index = find(name);
				if (index < 0 || symbols_[size_t(index)].kind != sym_function)
					index = define(name, sym_function, type(declared).base);
				symbols_[size_t(index)].type = type(declared).base;
				symbols_[size_t(index)].parameters = parameters;
				if (kind() == tok::l_brace)
				{
					function_body(index, parameters, parameter_names);
					return;
				}
			}
			else
			{
				// A static local is a global that happens to be spelled inside
				// a function: it keeps its storage between calls, so it lives
				// in the data section and takes the same initialisers.
				const bool static_storage = scope_ == 0 || is_static;
				int index = find(name);
				if (index < 0 || symbols_[size_t(index)].scope != scope_)
				{
					index = define(name, static_storage ? sym_global : sym_local, declared);
					if (static_storage)
					{
						symbols_[size_t(index)].offset = reserve_data(type_size(declared));
					}
					else
					{
						frame_ += type_size(declared) + (type_size(declared) & 1);
						symbols_[size_t(index)].offset = -frame_;
					}
				}
				else if (static_storage && type_size(declared) > type_size(symbols_[size_t(index)].type))
				{
					// A header declares an array without a length and the
					// definition here supplies one, so the byte reserved for
					// the declaration has to give way to the real thing.
					const int old_offset = symbols_[size_t(index)].offset;
					const int fresh = reserve_data(type_size(declared));
					for (data_fixup& fixup : data_fixups_)
						if (fixup.offset == old_offset) fixup.offset = fresh;
					symbols_[size_t(index)].offset = fresh;
					symbols_[size_t(index)].type = declared;
				}

				if (accept(tok::assign))
				{
					if (static_storage)
					{
						global_initialiser(declared, symbols_[size_t(index)].offset);
					}
					else
					{
						value target;
						target.kind = val_local;
						target.type = declared;
						target.offset = symbols_[size_t(index)].offset;
						value source = assignment_expression();
						load_converted(source, declared);
						store(target);
					}
				}
			}

			if (!accept(tok::comma)) break;
		}
		expect(tok::semicolon, "expected ; after a declaration");
	}

	void generator::local_declaration() { declaration(); }

	// -----------------------------------------------------------------------
	// Statements
	// -----------------------------------------------------------------------

	void generator::compound_statement()
	{
		++scope_;
		while (!accept(tok::r_brace))
		{
			if (kind() == tok::end) fail("unterminated block");
			statement();
		}
		leave_scope(--scope_);
	}

	void generator::for_statement()
	{
		++scope_;
		expect(tok::l_paren, "expected ( after for");
		if (!accept(tok::semicolon))
		{
			if (is_type_start()) local_declaration();
			else
			{
				if (!in_place_statement(tok::semicolon)) expression();
				expect(tok::semicolon, "expected ;");
			}
		}

		// The condition and the step both run after the body, so they are
		// generated after it: an iteration then costs one conditional jump and
		// no unconditional ones. Finding them is a matter of walking the token
		// vector, which is the same trick a call uses to reverse its arguments.
		const size_t condition_at = at_;
		int depth = 0;
		while (kind() != tok::semicolon || depth != 0)
		{
			if (kind() == tok::end) fail("unterminated for");
			if (kind() == tok::l_paren) ++depth;
			else if (kind() == tok::r_paren) --depth;
			advance();
		}
		const bool has_condition = at_ != condition_at;
		advance();

		const size_t step_at = at_;
		depth = 0;
		while (kind() != tok::r_paren || depth != 0)
		{
			if (kind() == tok::end) fail("unterminated for");
			if (kind() == tok::l_paren) ++depth;
			else if (kind() == tok::r_paren) --depth;
			advance();
		}
		const bool has_step = at_ != step_at;
		expect(tok::r_paren, "expected ) after for");

		const int condition_label = e_.label();
		const int body_start = e_.label();
		const int step_label = e_.label();
		if (has_condition) e_.jump(condition_label);
		e_.bind(body_start);

		break_labels_.push_back(e_.label());
		continue_labels_.push_back(step_label);
		statement();
		const size_t after = at_;

		e_.bind(step_label);
		if (has_step)
		{
			at_ = step_at;
			if (!in_place_statement(tok::r_paren)) expression();
			at_ = after;
		}

		if (has_condition)
		{
			e_.bind(condition_label);
			at_ = condition_at;
			value v = expression();
			const condition taken = test_value(v);
			at_ = after;
			e_.jump_if(taken, body_start);
		}
		else
		{
			e_.jump(body_start);
		}

		e_.bind(break_labels_.back());
		break_labels_.pop_back();
		continue_labels_.pop_back();
		leave_scope(--scope_);
	}

	void generator::switch_statement()
	{
		expect(tok::l_paren, "expected ( after switch");
		value v = expression();
		load(v);
		expect(tok::r_paren, "expected ) after the switch expression");

		frame_ += 2;
		switch_context context;
		context.temporary = -frame_;
		e_.store_ax_frame(context.temporary);
		const int to_dispatch = e_.label();
		e_.jump(to_dispatch);

		switches_.push_back(context);
		break_labels_.push_back(e_.label());
		statement();
		e_.jump(break_labels_.back());

		e_.bind(to_dispatch);
		const switch_context& done = switches_.back();
		for (const auto& entry : done.cases)
		{
			e_.load_ax_frame(done.temporary);
			e_.cmp_ax_imm(int(entry.first));
			e_.jump_if(condition::equal, entry.second);
		}
		if (done.default_label >= 0) e_.jump(done.default_label);
		switches_.pop_back();

		e_.bind(break_labels_.back());
		break_labels_.pop_back();
	}

	void generator::statement()
	{
		switch (kind())
		{
		case tok::l_brace:
			advance();
			compound_statement();
			return;

		case tok::semicolon:
			advance();
			return;

		case tok::kw_if:
		{
			advance();
			expect(tok::l_paren, "expected ( after if");
			value v = expression();
			const condition taken = test_value(v);
			expect(tok::r_paren, "expected ) after the condition");
			const int otherwise = e_.label();
			e_.jump_unless(taken, otherwise);
			statement();
			if (accept(tok::kw_else))
			{
				const int done = e_.label();
				e_.jump(done);
				e_.bind(otherwise);
				statement();
				e_.bind(done);
			}
			else
			{
				e_.bind(otherwise);
			}
			return;
		}

		case tok::kw_while:
		{
			advance();
			expect(tok::l_paren, "expected ( after while");

			// The condition belongs at the bottom of the loop, so it is
			// generated after the body rather than before it.
			const size_t condition_at = at_;
			int depth = 0;
			while (kind() != tok::r_paren || depth != 0)
			{
				if (kind() == tok::end) fail("unterminated while");
				if (kind() == tok::l_paren) ++depth;
				else if (kind() == tok::r_paren) --depth;
				advance();
			}
			expect(tok::r_paren, "expected ) after the condition");

			const int condition_label = e_.label();
			const int start = e_.label();
			e_.jump(condition_label);
			e_.bind(start);
			break_labels_.push_back(e_.label());
			continue_labels_.push_back(condition_label);
			statement();
			const size_t after = at_;

			e_.bind(condition_label);
			at_ = condition_at;
			value v = expression();
			const condition taken = test_value(v);
			at_ = after;
			e_.jump_if(taken, start);

			e_.bind(break_labels_.back());
			break_labels_.pop_back();
			continue_labels_.pop_back();
			return;
		}

		case tok::kw_do:
		{
			advance();
			const int start = e_.here();
			const int condition_label = e_.label();
			break_labels_.push_back(e_.label());
			continue_labels_.push_back(condition_label);
			statement();
			e_.bind(condition_label);
			if (!accept(tok::kw_while)) fail("expected while after do");
			expect(tok::l_paren, "expected ( after while");
			value v = expression();
			const condition taken = test_value(v);
			expect(tok::r_paren, "expected )");
			expect(tok::semicolon, "expected ;");
			e_.jump_if(taken, start);
			e_.bind(break_labels_.back());
			break_labels_.pop_back();
			continue_labels_.pop_back();
			return;
		}

		case tok::kw_for:
			advance();
			for_statement();
			return;

		case tok::kw_switch:
			advance();
			switch_statement();
			return;

		case tok::kw_case:
		{
			advance();
			if (switches_.empty()) fail("case outside a switch");
			const long label = constant_expression();
			expect(tok::colon, "expected : after case");
			switches_.back().cases.emplace_back(label, e_.here());
			return;
		}

		case tok::kw_default:
			advance();
			if (switches_.empty()) fail("default outside a switch");
			expect(tok::colon, "expected : after default");
			switches_.back().default_label = e_.here();
			return;

		case tok::kw_break:
			advance();
			expect(tok::semicolon, "expected ;");
			if (break_labels_.empty()) fail("break outside a loop or switch");
			e_.jump(break_labels_.back());
			return;

		case tok::kw_continue:
			advance();
			expect(tok::semicolon, "expected ;");
			if (continue_labels_.empty()) fail("continue outside a loop");
			e_.jump(continue_labels_.back());
			return;

		case tok::kw_goto:
		{
			advance();
			if (kind() != tok::identifier) fail("expected a label after goto");
			const std::string name = peek().text;
			advance();
			expect(tok::semicolon, "expected ; after goto");
			if (labels_.find(name) == labels_.end()) labels_[name] = e_.label();
			pending_gotos_.emplace_back(name, 0);
			e_.jump(labels_[name]);
			return;
		}

		case tok::kw_return:
		{
			advance();
			if (kind() != tok::semicolon)
			{
				value v = expression();
				load_converted(v, return_type_);
			}
			expect(tok::semicolon, "expected ; after return");
			// Inside a body that was copied to a call site there is no frame to
			// unwind: the value is already where the caller wants it.
			if (!inline_returns_.empty()) e_.jump(inline_returns_.back());
			else emit_return();
			return;
		}

		default:
			break;
		}

		if (is_type_start())
		{
			local_declaration();
			return;
		}

		if (kind() == tok::identifier && peek(1).kind == tok::colon)
		{
			const std::string name = peek().text;
			if (labels_.find(name) == labels_.end()) labels_[name] = e_.label();
			e_.bind(labels_[name]);
			advance();
			advance();
			if (kind() != tok::r_brace) statement();
			return;
		}

		if (!in_place_statement(tok::semicolon)) expression();
		expect(tok::semicolon, "expected ; after an expression");
	}

	// -----------------------------------------------------------------------
	// Runtime, startup and output
	// -----------------------------------------------------------------------

	void generator::emit_startup()
	{
		// mov sp, <top of our block>; shrink the block DOS gave us so that a
		// child process has somewhere to load, then call main and exit.
		e_.mov_sp_imm(0);
		stack_patch_ = e_.refers_to_deferred();
		e_.mov_bx_imm(0);
		paragraph_patch_ = e_.refers_to_deferred();
		e_.emit(op::mov, 1, operand::in(reg_ah), operand::constant(0x4a));
		e_.interrupt(0x21);

		const int main_index = define("main", sym_function, t_int_);
		e_.call(label_of(main_index));
		symbols_[size_t(main_index)].called = true;

		e_.emit(op::mov, 1, operand::in(reg_ah), operand::constant(0x4c));
		e_.interrupt(0x21);
	}

	// The thunk a C program calls to reach a BIOS or DOS service: registers in,
	// AX out, and whatever DOS left in the other registers readable afterwards.
	void generator::emit_interrupt_call(int number, int cf, int bx, int cx, int dx, bool with_si)
	{
		const auto from_frame = [&](int r, int displacement)
		{
			e_.emit(op::mov, 2, operand::in(r), emitter::frame(displacement));
		};
		const auto to_global = [&](int symbol, int r)
		{
			e_.emit(op::mov, 2, operand::absolute(0), operand::in(r));
			data_reference(symbols_[size_t(symbol)].offset);
		};
		const auto set_global = [&](int symbol, int value)
		{
			e_.emit(op::mov, 2, operand::absolute(0), operand::constant(value));
			data_reference(symbols_[size_t(symbol)].offset);
		};

		e_.push_bp();
		e_.mov_bp_sp();
		if (with_si) e_.emit(op::push, 2, operand::in(reg_si));
		from_frame(reg_a, 4);
		from_frame(reg_b, 6);
		from_frame(reg_c, 8);
		from_frame(reg_d, 10);
		if (with_si) from_frame(reg_si, 12);
		e_.interrupt(number);

		to_global(bx, reg_b);
		to_global(cx, reg_c);
		to_global(dx, reg_d);
		set_global(cf, 0);
		const int carry_clear = e_.label();
		e_.jump_if(condition::above_equal, carry_clear);
		set_global(cf, 1);
		e_.bind(carry_clear);
		if (with_si) e_.emit(op::pop, 2, operand::in(reg_si));
		e_.pop_bp();
		e_.ret();
	}

	// The subroutines the 32-bit operators call, written out after the program
	// so that a program using no long pays nothing for them. Each takes DX:AX
	// and CX:BX and returns DX:AX.
	void generator::emit_helpers()
	{
		// A helper is wanted if a call that survived generation still refers to
		// it, which the layout record answers directly.
		const auto referenced = [&](int which)
		{
			if (helper_label_[which] < 0) return false;
			for (const instruction& in : e_.code)
				if (in.code == op::call && in.label == helper_label_[which]) return true;
			return false;
		};

		bool used[helper_count] = {};
		for (int i = 0; i < helper_count; ++i) used[i] = referenced(i);
		if (used[h_ldiv] || used[h_uldiv] || used[h_lmod] || used[h_ulmod]) used[h_uldivmod] = true;

		const auto bytes = [&](std::initializer_list<int> list) { for (const int b : list) e_.byte(b); };

		// The helpers were hand-assembled, but their branches go through the
		// layout record like everything else, so they shorten with the rest.
		const auto branch = [&](condition c)
		{
			const int target = e_.label();
			e_.jump_if(c, target);
			return target;
		};
		const auto here = [&](int target) { e_.bind(target); };
		const auto again = [&](condition c, int target) { e_.jump_if(c, target); };
		const auto negate = [&] { e_.negate_dx_ax(); };

		if (used[h_uldivmod] || used[h_uldiv])
		{
			// Restoring division a bit at a time: SI:DI accumulates the
			// remainder while the quotient replaces the dividend in DX:AX.
			e_.bind(helper_label(h_uldivmod));
			if (helper_label_[h_uldiv] >= 0) e_.bind(helper_label_[h_uldiv]);
			bytes({0x56, 0x57, 0x55}); // push si / push di / push bp
			bytes({0x31, 0xf6, 0x31, 0xff}); // xor si,si / xor di,di
			bytes({0xbd, 0x20, 0x00}); // mov bp,32
			const int loop = e_.here();
			bytes({0xd1, 0xe0, 0xd1, 0xd2, 0xd1, 0xd7, 0xd1, 0xd6}); // shl ax,1 / rcl dx,1 / rcl di,1 / rcl si,1
			bytes({0x39, 0xce}); // cmp si,cx
			const int skip_low = branch(condition::below);
			const int to_subtract = branch(condition::above);
			bytes({0x39, 0xdf}); // cmp di,bx
			const int skip = branch(condition::below);
			here(to_subtract);
			bytes({0x29, 0xdf, 0x19, 0xce}); // sub di,bx / sbb si,cx
			bytes({0x0c, 0x01}); // or al,1
			here(skip_low);
			here(skip);
			e_.byte(0x4d); // dec bp
			again(condition::not_equal, loop);
			bytes({0x89, 0xf1, 0x89, 0xfb}); // mov cx,si / mov bx,di
			bytes({0x5d, 0x5f, 0x5e}); // pop bp / pop di / pop si
			e_.ret();
		}

		const auto call_divide = [&] { e_.call(helper_label(h_uldivmod)); };

		if (used[h_lmul])
		{
			// The high word of a 32-bit product needs only the low halves of
			// the two cross terms, so three 16-bit multiplies are enough.
			e_.bind(helper_label(h_lmul));
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
			e_.ret();
		}

		if (used[h_ulmod])
		{
			e_.bind(helper_label(h_ulmod));
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			e_.ret();
		}

		if (used[h_ldiv])
		{
			e_.bind(helper_label(h_ldiv));
			e_.byte(0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(condition::greater_equal);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(condition::greater_equal);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(condition::equal);
			negate();
			here(done);
			e_.byte(0x5e); // pop si
			e_.ret();
		}

		if (used[h_lmod])
		{
			// The sign of a remainder follows the dividend, not the divisor.
			e_.bind(helper_label(h_lmod));
			e_.byte(0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(condition::greater_equal);
			bytes({0xbe, 0x01, 0x00}); // mov si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(condition::greater_equal);
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(condition::equal);
			negate();
			here(done);
			e_.byte(0x5e); // pop si
			e_.ret();
		}

		const int shifts[3] = {h_lshl, h_lshr, h_lsar};
		for (const int which : shifts)
		{
			if (!used[which]) continue;
			e_.bind(helper_label(which));
			bytes({0x88, 0xd9}); // mov cl,bl
			bytes({0x80, 0xe1, 0x1f}); // and cl,31
			const int done = branch(condition::equal);
			const int loop = e_.here();
			if (which == h_lshl) bytes({0xd1, 0xe0, 0xd1, 0xd2}); // shl ax,1 / rcl dx,1
			else if (which == h_lshr) bytes({0xd1, 0xea, 0xd1, 0xd8}); // shr dx,1 / rcr ax,1
			else bytes({0xd1, 0xfa, 0xd1, 0xd8}); // sar dx,1 / rcr ax,1
			bytes({0xfe, 0xc9}); // dec cl
			again(condition::not_equal, loop);
			here(done);
			e_.ret();
		}

		for (const int which : {int(h_lcmp), int(h_ulcmp)})
		{
			if (!used[which]) continue;
			// Reduces the pair to -1, 0 or 1 so the caller can use the ordinary
			// signed conditions whatever the operand signedness was.
			e_.bind(helper_label(which));
			bytes({0x39, 0xca}); // cmp dx,cx
			const condition low = which == h_lcmp ? condition::less : condition::below;
			const condition high = which == h_lcmp ? condition::greater : condition::above;
			const int less_high = branch(low);
			const int greater_high = branch(high);
			bytes({0x39, 0xd8}); // cmp ax,bx
			const int less_low = branch(condition::below);
			const int greater_low = branch(condition::above);
			e_.xor_ax_ax();
			e_.ret();
			here(less_high);
			here(less_low);
			e_.mov_ax_imm(-1);
			e_.ret();
			here(greater_high);
			here(greater_low);
			e_.mov_ax_imm(1);
			e_.ret();
		}
	}

	void generator::emit_runtime()
	{
		// The registers an INT 21h call left behind, so C code can read the
		// ones the return value does not carry.
		const int cf = define("__cf", sym_global, t_int_);
		symbols_[size_t(cf)].offset = reserve_data(2);
		const int bx = define("__bx", sym_global, t_int_);
		symbols_[size_t(bx)].offset = reserve_data(2);
		const int cx = define("__cx", sym_global, t_int_);
		symbols_[size_t(cx)].offset = reserve_data(2);
		const int dx = define("__dx", sym_global, t_int_);
		symbols_[size_t(dx)].offset = reserve_data(2);

		// The thunks are generated before anything can call them, so each one
		// is a function whose body is already there.
		const auto thunk = [&](const char* name, int type)
		{
			const int index = define(name, sym_function, type);
			symbols_[size_t(index)].defined = true;
			symbols_[size_t(index)].start = e_.here();
			symbols_[size_t(index)].label = symbols_[size_t(index)].start;
			return index;
		};

		const int int21 = thunk("__int21", t_int_);
		emit_interrupt_call(0x21, cf, bx, cx, dx, false);
		symbols_[size_t(int21)].finish = e_.here();

		const int int10 = thunk("__int10", t_int_);
		emit_interrupt_call(0x10, cf, bx, cx, dx, false);
		symbols_[size_t(int10)].finish = e_.here();

		// The same call with SI set, for the handful of services that take a
		// buffer there rather than in DX.
		const int int21si = thunk("__int21si", t_int_);
		emit_interrupt_call(0x21, cf, bx, cx, dx, true);
		symbols_[size_t(int21si)].finish = e_.here();

		// The segment everything lives in, which a program needs when it hands
		// DOS a far pointer.
		const int seg = thunk("__seg", t_uint_);
		e_.mov_ax_cs();
		e_.ret();
		symbols_[size_t(seg)].finish = e_.here();
	}

	void generator::run()
	{
		types_.clear();
		type_info info;
		info.kind = ty_void;
		info.size = 1;
		t_void_ = add_type(info);
		info.kind = ty_char;
		info.size = 1;
		t_char_ = add_type(info);
		info.kind = ty_uchar;
		info.size = 1;
		t_uchar_ = add_type(info);
		info.kind = ty_int;
		info.size = target_.int_size;
		t_int_ = add_type(info);
		info.kind = ty_uint;
		info.size = target_.int_size;
		t_uint_ = add_type(info);
		info.kind = ty_long;
		info.size = target_.long_size;
		t_long_ = add_type(info);
		info.kind = ty_ulong;
		info.size = target_.long_size;
		t_ulong_ = add_type(info);
		info.kind = ty_real;
		info.size = target_.long_size;
		t_real_ = add_type(info);

		emit_startup();
		emit_runtime();

		while (kind() != tok::end) declaration();
		emit_helpers();
	}

	// Which functions the program can actually get to. Compiling a whole
	// project as one translation unit brings in everything every file declares,
	// and most of it is never called; the layout record knows what refers to
	// what, so the rest can simply not be laid out.
	void generator::drop_unreachable()
	{
		if (!options_.drop_unreachable) return;

		struct span
		{
			int from = 0;
			int to = 0;
			int symbol = 0;
		};

		std::vector<span> spans;
		std::map<int, int> starts; // the instruction a function begins at, to its symbol
		for (size_t i = 0; i < symbols_.size(); ++i)
		{
			const symbol& entry = symbols_[i];
			if (entry.kind != sym_function || !entry.defined) continue;
			if (entry.start < 0 || entry.finish < 0) continue;
			span s;
			s.from = e_.labels[size_t(entry.start)];
			s.to = e_.labels[size_t(entry.finish)];
			s.symbol = int(i);
			if (s.from < 0 || s.to <= s.from) continue;
			spans.push_back(s);
			starts[s.from] = int(i);
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
			if (label < 0 || !e_.is_bound(label)) return;
			const auto target = starts.find(e_.labels[size_t(label)]);
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

		for (size_t i = 0; i < e_.code.size(); ++i)
		{
			const instruction& in = e_.code[i];
			if (in.code == op::call) reference(int(i), in.label);
			for (const operand* o : {&in.dest, &in.source})
				if (o->reference == reference_kind::code) reference(int(i), o->target);
		}
		for (const function_fixup& fixup : function_fixups_) roots.push_back(fixup.function);

		std::vector<bool> reached(symbols_.size(), false);
		std::vector<int> pending = roots;
		while (!pending.empty())
		{
			const int index = pending.back();
			pending.pop_back();
			if (reached[size_t(index)]) continue;
			reached[size_t(index)] = true;
			const auto found = edges.find(index);
			if (found == edges.end()) continue;
			for (const int next : found->second) pending.push_back(next);
		}

		for (const span& s : spans)
			if (!reached[size_t(s.symbol)])
				e_.drop(symbols_[size_t(s.symbol)].start, symbols_[size_t(s.symbol)].finish);
	}

	std::vector<uint8_t> generator::finish(int& code_size, int& data_size)
	{
		for (const symbol& entry : symbols_)
		{
			if (entry.kind != sym_function || !entry.called || entry.defined) continue;
			throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
		}

		// Nothing has an address until now: the layout pass picks the shortest
		// form for every branch, drops what the program never reaches, and
		// moves what is left.
		drop_unreachable();
		e_.lay_out(target_.code_origin);

		// Data follows the code, so its address is only known after that.
		code_size = int(e_.image.size());
		if (code_size % 2 != 0) ++code_size;
		const int data_base = target_.code_origin + code_size;
		data_size = int(data_.size());

		const int image_end = data_base + data_size;
		const int stack_top = (image_end + target_.stack_size + 1) & ~1;
		if (stack_top > target_.address_limit)
		{
			char message[160] = {};
			snprintf(message, sizeof(message),
			         "the program is too large for one segment: %d bytes of code and %d of data",
			         code_size, data_size);
			throw compile_error{message, 0, 0};
		}
		e_.set_deferred(stack_patch_, stack_top);
		e_.set_deferred(paragraph_patch_, (stack_top + 15) / 16);
		if (!e_.resolve(target_.code_origin, data_base))
			throw compile_error{"a jump was never given somewhere to go", 0, 0};

		// The data section holds pointers of its own, and it does not move.
		const auto put_word = [&](int at, int value)
		{
			data_[size_t(at)] = uint8_t(value & 0xff);
			data_[size_t(at) + 1] = uint8_t((value >> 8) & 0xff);
		};

		for (const function_fixup& fixup : function_fixups_)
		{
			const symbol& entry = symbols_[size_t(fixup.function)];
			if (!entry.defined) throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
			put_word(fixup.position, target_.code_origin + e_.offset_of(entry.label));
		}

		for (const data_fixup& fixup : data_fixups_)
			put_word(fixup.position, data_base + fixup.offset);

		std::vector<uint8_t> image = e_.image;
		while (image.size() % 2 != 0) image.push_back(0x90);
		image.insert(image.end(), data_.begin(), data_.end());
		return image;
	}

	// What survived, and how much room it takes. The spans come from the
	// layout record, so this is what actually ended up in the image, and it is
	// what a multi-segment build would have to divide up.
	void generator::report(std::vector<std::pair<std::string, int>>& functions) const
	{
		for (const symbol& entry : symbols_)
		{
			if (entry.kind != sym_function || !entry.defined) continue;
			if (entry.start < 0 || entry.finish < 0 || entry.name.empty()) continue;
			const int size = e_.offset_of(entry.finish) - e_.offset_of(entry.start);
			if (size > 0) functions.emplace_back(entry.name, size);
		}
	}
}

cc_result cc_compile(const char* text, const char* name, cc_sources& sources,
                     const std::vector<std::string>& defines, const cc_options& options)
{
	cc_result result;
	cc_token_stream stream;
	if (!cc_tokenise(text, name, sources, stream, defines))
	{
		result.message = stream.error;
		result.file = stream.error_file;
		result.line = stream.error_line;
		return result;
	}

	generator pass(stream, options);
	try
	{
		pass.run();
		result.image = pass.finish(result.code_size, result.data_size);
		pass.report(result.functions);
		result.ok = true;
	}
	catch (const compile_error& error)
	{
		// The sizes are worth having even when the program turned out not to
		// fit, which is the moment somebody wants to know where it all went.
		pass.report(result.functions);
		result.message = error.message;
		result.line = error.line;
		result.file = error.file >= 0 && error.file < int(stream.files.size())
			              ? stream.files[size_t(error.file)]
			              : std::string(name);
	}
	return result;
}
