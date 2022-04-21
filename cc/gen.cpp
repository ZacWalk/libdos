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

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	constexpr int com_origin = 0x100;
	constexpr int stack_size = 2048;

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
		int address = -1; // code offset, for functions
		long value = 0; // enumeration constant
		int scope = 0;
		std::vector<int> parameters; // declared parameter types, so a call can convert
		std::vector<int> pending_calls;
	};

	enum value_kind { val_const, val_reg, val_local, val_global, val_deref };

	struct value
	{
		value_kind kind = val_const;
		int type = 0;
		long constant = 0;
		double real = 0; // only meaningful while the value is still a constant
		int offset = 0;
	};

	struct data_fixup
	{
		int position;
		int offset;
	};

	// The address of a function is only known once its body has been generated,
	// and a forward reference may sit in either section.
	struct function_fixup
	{
		int position;
		int function;
		bool in_data;
	};

	struct switch_context
	{
		int temporary = 0;
		std::vector<std::pair<long, int>> cases;
		int default_position = -1;
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

	class generator
	{
	public:
		explicit generator(cc_token_stream& tokens) : tokens_(tokens)
		{
		}

		void run();
		std::vector<uint8_t> finish(int& code_size, int& data_size);

	private:
		cc_token_stream& tokens_;
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
		std::vector<std::vector<int>> break_lists_;
		std::vector<std::vector<int>> continue_lists_;
		std::vector<switch_context> switches_;
		size_t at_ = 0;
		int scope_ = 0;
		int frame_ = 0;
		int frame_patch_ = -1;
		int stack_patch_ = -1;
		int paragraph_patch_ = -1;
		int return_type_ = 0;
		mutable size_t discarded_functions_ = 0;
		mutable size_t discarded_helpers_[helper_count] = {};
		std::vector<int> helper_calls_[helper_count];
		int helper_address_[helper_count] = {};

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
			info.size = 2;
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

		void function_reference(int position, int function, bool in_data = false)
		{
			function_fixups_.push_back({position, function, in_data});
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

		void data_reference(int position, int offset) { data_fixups_.push_back({position, offset}); }

		void mark(size_t& code_mark, size_t& fixup_mark) const
		{
			code_mark = e_.code.size();
			fixup_mark = data_fixups_.size();
			discarded_functions_ = function_fixups_.size();
			for (int i = 0; i < helper_count; ++i) discarded_helpers_[i] = helper_calls_[i].size();
		}

		void discard(size_t code_mark, size_t fixup_mark)
		{
			e_.code.resize(code_mark);
			data_fixups_.resize(fixup_mark);
			function_fixups_.resize(discarded_functions_);
			// A call site inside discarded code would otherwise be patched later,
			// writing over whatever took its place.
			for (int i = 0; i < helper_count; ++i) helper_calls_[i].resize(discarded_helpers_[i]);
		}

		// ---- code generation ----
		void load(value& v);
		void load_address(value& v);
		void store(const value& target);
		void load_indirect(int type);
		void store_indirect(int type);
		void convert_to_boolean(value& v);
		void apply_operator(tok op, bool unsigned_operation);
		void apply_operator_wide(tok op, bool unsigned_operation);
		void widen(int from_type);
		void load_converted(value& v, int to_type);
		void call_helper(helper which);
		void emit_helpers();

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

		value expression();
		value assignment_expression();
		value conditional_expression();
		value binary_expression(int precedence);
		value unary_expression();
		value postfix_expression();
		value primary_expression();
		value call(int function_index);
		value indirect_call(value& target);
		int push_arguments(const std::vector<int>& parameters);

		void emit_runtime();
		void emit_interrupt_call(int number, int cf, int bx, int cx, int dx, bool with_si);
		void emit_startup();
	};

	// -----------------------------------------------------------------------
	// Code generation
	// -----------------------------------------------------------------------

	void generator::load_indirect(int type_index)
	{
		e_.mov_bx_ax();
		if (type_size(type_index) == 1)
		{
			e_.load_al_indirect();
			if (is_unsigned(type_index)) e_.zero_ah();
			else e_.cbw();
		}
		else
		{
			e_.load_ax_indirect();
			if (is_wide(type_index)) e_.load_dx_indirect_high();
		}
	}

	void generator::store_indirect(int type_index)
	{
		if (type_size(type_index) == 1)
		{
			e_.store_al_indirect();
			return;
		}
		e_.store_ax_indirect();
		if (is_wide(type_index)) e_.store_dx_indirect_high();
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
				data_reference(e_.at() - 2, v.offset);
			}
			else if (type_size(v.type) == 1)
			{
				e_.load_al_absolute(0);
				data_reference(e_.at() - 2, v.offset);
				if (is_unsigned(v.type)) e_.zero_ah();
				else e_.cbw();
			}
			else
			{
				e_.load_ax_absolute(0);
				data_reference(e_.at() - 2, v.offset);
				if (wide)
				{
					e_.load_dx_absolute(0);
					data_reference(e_.at() - 2, v.offset + 2);
				}
			}
			break;

		default: // val_deref: the address is already in AX
			if (!is_array(v.type) && !is_record(v.type)) load_indirect(v.type);
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
			data_reference(e_.at() - 2, v.offset);
			break;
		case val_deref:
			break; // the address is already in AX
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
				data_reference(e_.at() - 2, target.offset);
			}
			else
			{
				e_.store_ax_absolute(0);
				data_reference(e_.at() - 2, target.offset);
				if (wide)
				{
					e_.store_dx_absolute(0);
					data_reference(e_.at() - 2, target.offset + 2);
				}
			}
			break;
		case val_deref:
			e_.pop_bx();
			store_indirect(target.type);
			break;
		default:
			fail("cannot assign to this expression");
		}
	}

	void generator::convert_to_boolean(value& v)
	{
		const bool wide = is_wide(v.type);
		load(v);
		if (wide) e_.or_ax_dx(); // zero only when both halves are zero
		else e_.test_ax_ax();
	}

	void generator::call_helper(helper which)
	{
		helper_calls_[which].push_back(e_.call());
	}

	// AX holds the left operand and CX the right.
	void generator::apply_operator(tok op, bool unsigned_operation)
	{
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
			e_.byte(0x89);
			e_.byte(0xd0); // mov ax,dx
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

		condition c;
		switch (op)
		{
		case tok::eq: c = condition::equal; break;
		case tok::ne: c = condition::not_equal; break;
		case tok::less: c = unsigned_operation ? condition::below : condition::less; break;
		case tok::le: c = unsigned_operation ? condition::below_equal : condition::less_equal; break;
		case tok::greater: c = unsigned_operation ? condition::above : condition::greater; break;
		case tok::ge: c = unsigned_operation ? condition::above_equal : condition::greater_equal; break;
		default: fail("unsupported operator");
		}
		e_.cmp_ax_cx();
		e_.set_from_condition(c);
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
		// operands were.
		condition c;
		switch (op)
		{
		case tok::eq: c = condition::equal; break;
		case tok::ne: c = condition::not_equal; break;
		case tok::less: c = condition::less; break;
		case tok::le: c = condition::less_equal; break;
		case tok::greater: c = condition::greater; break;
		case tok::ge: c = condition::greater_equal; break;
		default: fail("unsupported operator");
		}
		call_helper(unsigned_operation ? h_ulcmp : h_lcmp);
		e_.test_ax_ax();
		e_.set_from_condition(c);
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
		const int pushed = push_arguments(symbols_[size_t(function_index)].parameters);

		const int position = e_.call();
		if (symbols_[size_t(function_index)].address >= 0)
			e_.patch_word(position, symbols_[size_t(function_index)].address - (position + 2));
		else
			symbols_[size_t(function_index)].pending_calls.push_back(position);

		if (pushed != 0) e_.add_sp_imm(pushed);

		value result;
		result.kind = val_reg;
		result.type = symbols_[size_t(function_index)].type;
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
			}

			switch (symbols_[size_t(index)].kind)
			{
			case sym_constant:
				v.kind = val_const;
				v.type = t_int_;
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
				function_reference(e_.at() - 2, index);
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
				load(v);
				e_.push_ax();
				value index = expression();
				expect(tok::r_bracket, "expected ]");
				load(index);
				const int size = type_size(element);
				if (size != 1)
				{
					e_.mov_cx_imm(size);
					e_.imul_cx();
				}
				e_.mov_cx_ax();
				e_.pop_ax();
				e_.add_ax_cx();
				v.kind = val_deref;
				v.type = element;
				continue;
			}

			if (kind() == tok::dot || kind() == tok::arrow)
			{
				const bool through_pointer = kind() == tok::arrow;
				advance();
				if (kind() != tok::identifier) fail("expected a member name");
				const std::string member = peek().text;
				advance();

				int record_type;
				if (through_pointer)
				{
					if (!is_scalar_pointer(v.type)) fail("-> needs a pointer");
					record_type = target_of(v.type);
					load(v);
				}
				else
				{
					record_type = v.type;
					load_address(v);
				}
				if (!is_record(record_type)) fail("not a structure or union");

				const record_info& info = records_[size_t(type(record_type).record)];
				const record_member* found = nullptr;
				for (const record_member& m : info.members) if (m.name == member) found = &m;
				if (found == nullptr) fail(("no such member: " + member).c_str());

				if (found->offset != 0) e_.add_ax_imm(found->offset);
				v.kind = val_deref;
				v.type = found->type;
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
					if (target.kind == val_deref) store_indirect(target.type);
					else store(target);
					e_.pop_ax();
					e_.pop_dx();
					v.kind = val_reg;
					continue;
				}

				if (v.kind == val_deref)
				{
					e_.push_ax(); // the address
					load_indirect(v.type);
					e_.push_ax(); // the value the expression produces
					if (increment) e_.add_ax_imm(step);
					else e_.sub_ax_imm(step);
					e_.pop_cx(); // the old value
					e_.pop_bx(); // the address
					store_indirect(v.type);
					e_.mov_ax_cx();
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
			load(v);
			v.kind = val_deref;
			v.type = target;
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
			const bool wide = is_wide(v.type);
			load(v);
			if (wide) e_.or_ax_dx();
			else
			{
				e_.mov_cx_imm(0);
				e_.cmp_ax_cx();
			}
			e_.set_from_condition(condition::equal);
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
				if (target.kind == val_deref) store_indirect(target.type);
				else store(target);
				v.kind = val_reg;
				return v;
			}
			if (v.kind == val_deref)
			{
				e_.push_ax();
				const int target_type = v.type;
				load_indirect(target_type);
				if (increment) e_.add_ax_imm(step);
				else e_.sub_ax_imm(step);
				e_.pop_bx();
				store_indirect(target_type);
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
				size_t code_mark, fixup_mark;
				mark(code_mark, fixup_mark);
				const value v = unary_expression();
				measured = v.type;
				discard(code_mark, fixup_mark);
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
				convert_to_boolean(left);
				const int shortcut = op == tok::and_and ? e_.jump_if_zero() : e_.jump_if_not_zero();
				value right = binary_expression(level);
				convert_to_boolean(right);
				e_.set_from_condition(condition::not_equal);
				const int done = e_.jump();
				e_.patch_jump(shortcut);
				e_.mov_ax_imm(op == tok::and_and ? 0 : 1);
				e_.patch_jump(done);
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
			size_t code_mark, fixup_mark;
			mark(code_mark, fixup_mark);

			const bool left_pointer = is_scalar_pointer(left.type);
			const int element = left_pointer ? type_size(target_of(left.type)) : 1;
			const int left_type = left_pointer ? pointer_to(target_of(left.type)) : left.type;
			const bool left_wide = !left_pointer && is_wide(left.type);

			// A real has no register to be loaded into, so the speculative load
			// has to be skipped; if the right side then turns out not to be
			// constant, the fold below cannot happen and that is the error.
			const bool left_real = left_constant && is_real(left.type);
			if (!left_real)
			{
				load(left);
				if (left_wide) e_.push_dx();
				e_.push_ax();
			}
			value right = binary_expression(level);

			if (left_real && (right.kind != val_const || is_scalar_pointer(right.type)))
				fail("floating point is only available in a constant expression");

			if (left_constant && right.kind == val_const && !is_scalar_pointer(right.type))
			{
				discard(code_mark, fixup_mark);

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
			load(right);

			// A pointer is 16-bit, so an index that happens to be long is
			// simply truncated rather than dragging the whole operation wide.
			const bool wide = !left_pointer && !right_pointer && (left_wide || is_wide(right.type));
			const bool unsigned_operation = is_unsigned(left.type) || is_unsigned(right.type);

			if (left_pointer && !right_pointer && element != 1 && (op == tok::plus || op == tok::minus))
			{
				e_.mov_cx_imm(element);
				e_.imul_cx();
			}

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

			value result;
			result.kind = val_reg;
			result.type = t_int_;
			if (left_pointer && right_pointer && op == tok::minus)
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
			else if (wide && precedence_of(op) != 7 && precedence_of(op) != 6)
			{
				// A comparison is an int; every other 32-bit operator keeps the
				// width, and a shift keeps the type of what was shifted.
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
			size_t code_mark = 0, fixup_mark = 0;
			mark(code_mark, fixup_mark);
			value then_value = expression();
			if (!taken) discard(code_mark, fixup_mark);
			expect(tok::colon, "expected : in a conditional expression");
			mark(code_mark, fixup_mark);
			value else_value = conditional_expression();
			if (taken) discard(code_mark, fixup_mark);
			value chosen = taken ? then_value : else_value;
			if (chosen.kind != val_const) load(chosen);
			return chosen;
		}

		convert_to_boolean(v);
		const int otherwise = e_.jump_if_zero();
		// The middle arm is a full expression, so a comma belongs to it rather
		// than ending the argument or declarator the conditional sits in.
		value then_value = expression();
		load(then_value);
		const int done = e_.jump();
		expect(tok::colon, "expected : in a conditional expression");
		e_.patch_jump(otherwise);
		value else_value = conditional_expression();
		load_converted(else_value, then_value.type);
		e_.patch_jump(done);

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
			if (target.kind == val_deref) e_.push_ax();
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

		if (target.kind == val_deref) e_.push_ax();
		value current = target;
		load(current);
		if (wide) e_.push_dx();
		e_.push_ax();
		value source = assignment_expression();
		load(source);
		if (pointer_target && element != 1 && (binary == tok::plus || binary == tok::minus))
		{
			e_.mov_cx_imm(element);
			e_.imul_cx();
		}
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
		size_t code_mark, fixup_mark;
		mark(code_mark, fixup_mark);
		const value v = conditional_expression();
		discard(code_mark, fixup_mark);
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
				size_t code_mark, fixup_mark;
				mark(code_mark, fixup_mark);
				const value subscript = conditional_expression();
				discard(code_mark, fixup_mark);
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
			size_t code_mark, fixup_mark;
			mark(code_mark, fixup_mark);
			const value step = binary_expression(precedence_of(tok::plus));
			discard(code_mark, fixup_mark);
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
				data_fixups_.push_back({-1 - at, int(symbols_[size_t(symbol)].offset + offset)});
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
			function_reference(at, index, true);
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
			data_fixups_.push_back({-1 - offset, text_offset}); // negative marks a data-to-data fixup
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

	void generator::function_body(int function_index, const std::vector<int>& parameters,
	                              const std::vector<std::string>& parameter_names)
	{
		symbols_[size_t(function_index)].address = e_.at();
		for (const int position : symbols_[size_t(function_index)].pending_calls)
			e_.patch_word(position, e_.at() - (position + 2));
		symbols_[size_t(function_index)].pending_calls.clear();
		return_type_ = symbols_[size_t(function_index)].type;

		e_.push_bp();
		e_.mov_bp_sp();
		e_.sub_sp_imm(0);
		frame_patch_ = e_.at() - 2;
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
			if (found == labels_.end()) fail(("undefined label: " + entry.first).c_str());
			e_.patch_word(entry.second, found->second - (entry.second + 2));
		}
		pending_gotos_.clear();

		e_.patch_word(frame_patch_, frame_);
		emit_return();
		leave_scope(--scope_);
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
				expression();
				expect(tok::semicolon, "expected ;");
			}
		}

		const int condition_start = e_.at();
		int exit_jump = -1;
		if (!accept(tok::semicolon))
		{
			value v = expression();
			convert_to_boolean(v);
			exit_jump = e_.jump_if_zero();
			expect(tok::semicolon, "expected ;");
		}

		// The step runs after the body but is written before it, so the layout
		// is: condition, jump to body, step, jump to condition, body.
		const int to_body = e_.jump();
		const int step_start = e_.at();
		if (kind() != tok::r_paren) expression();
		expect(tok::r_paren, "expected ) after for");
		e_.jump_back(condition_start);
		e_.patch_jump(to_body);

		break_lists_.emplace_back();
		continue_lists_.emplace_back();
		statement();
		e_.jump_back(step_start);

		if (exit_jump >= 0) e_.patch_jump(exit_jump);
		for (const int position : break_lists_.back()) e_.patch_jump(position);
		break_lists_.pop_back();
		for (const int position : continue_lists_.back()) e_.patch_word(position, step_start - (position + 2));
		continue_lists_.pop_back();
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
		const int to_dispatch = e_.jump();

		switches_.push_back(context);
		break_lists_.emplace_back();
		statement();
		const int to_end = e_.jump();

		e_.patch_jump(to_dispatch);
		const switch_context& done = switches_.back();
		for (const auto& entry : done.cases)
		{
			e_.load_ax_frame(done.temporary);
			e_.mov_cx_imm(int(entry.first));
			e_.cmp_ax_cx();
			e_.byte(0x75); // jne over the jump
			e_.byte(0x03);
			e_.jump_back(entry.second);
		}
		if (done.default_position >= 0) e_.jump_back(done.default_position);
		switches_.pop_back();

		e_.patch_jump(to_end);
		for (const int position : break_lists_.back()) e_.patch_jump(position);
		break_lists_.pop_back();
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
			convert_to_boolean(v);
			expect(tok::r_paren, "expected ) after the condition");
			const int otherwise = e_.jump_if_zero();
			statement();
			if (accept(tok::kw_else))
			{
				const int done = e_.jump();
				e_.patch_jump(otherwise);
				statement();
				e_.patch_jump(done);
			}
			else
			{
				e_.patch_jump(otherwise);
			}
			return;
		}

		case tok::kw_while:
		{
			advance();
			const int start = e_.at();
			expect(tok::l_paren, "expected ( after while");
			value v = expression();
			convert_to_boolean(v);
			expect(tok::r_paren, "expected ) after the condition");
			const int exit_jump = e_.jump_if_zero();
			break_lists_.emplace_back();
			continue_lists_.emplace_back();
			statement();
			e_.jump_back(start);
			e_.patch_jump(exit_jump);
			for (const int position : break_lists_.back()) e_.patch_jump(position);
			break_lists_.pop_back();
			for (const int position : continue_lists_.back()) e_.patch_word(position, start - (position + 2));
			continue_lists_.pop_back();
			return;
		}

		case tok::kw_do:
		{
			advance();
			const int start = e_.at();
			break_lists_.emplace_back();
			continue_lists_.emplace_back();
			statement();
			const int condition_start = e_.at();
			if (!accept(tok::kw_while)) fail("expected while after do");
			expect(tok::l_paren, "expected ( after while");
			value v = expression();
			convert_to_boolean(v);
			expect(tok::r_paren, "expected )");
			expect(tok::semicolon, "expected ;");
			const int again = e_.jump_if_not_zero();
			e_.patch_word(again, start - (again + 2));
			for (const int position : break_lists_.back()) e_.patch_jump(position);
			break_lists_.pop_back();
			for (const int position : continue_lists_.back())
				e_.patch_word(position, condition_start - (position + 2));
			continue_lists_.pop_back();
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
			switches_.back().cases.emplace_back(label, e_.at());
			return;
		}

		case tok::kw_default:
			advance();
			if (switches_.empty()) fail("default outside a switch");
			expect(tok::colon, "expected : after default");
			switches_.back().default_position = e_.at();
			return;

		case tok::kw_break:
			advance();
			expect(tok::semicolon, "expected ;");
			if (break_lists_.empty()) fail("break outside a loop or switch");
			break_lists_.back().push_back(e_.jump());
			return;

		case tok::kw_continue:
			advance();
			expect(tok::semicolon, "expected ;");
			if (continue_lists_.empty()) fail("continue outside a loop");
			continue_lists_.back().push_back(e_.jump());
			return;

		case tok::kw_goto:
		{
			advance();
			if (kind() != tok::identifier) fail("expected a label after goto");
			const std::string label = peek().text;
			advance();
			expect(tok::semicolon, "expected ; after goto");
			pending_gotos_.emplace_back(label, e_.jump());
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
			emit_return();
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
			labels_[peek().text] = e_.at();
			advance();
			advance();
			if (kind() != tok::r_brace) statement();
			return;
		}

		expression();
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
		stack_patch_ = e_.at() - 2;
		e_.mov_bx_imm(0);
		paragraph_patch_ = e_.at() - 2;
		e_.byte(0xb4);
		e_.byte(0x4a); // mov ah,4Ah
		e_.interrupt(0x21);

		const int call_position = e_.call();
		const int main_index = define("main", sym_function, t_int_);
		symbols_[size_t(main_index)].pending_calls.push_back(call_position);

		e_.byte(0xb4);
		e_.byte(0x4c); // mov ah,4Ch
		e_.interrupt(0x21);
	}

	// The thunk a C program calls to reach a BIOS or DOS service: registers in,
	// AX out, and whatever DOS left in the other registers readable afterwards.
	void generator::emit_interrupt_call(int number, int cf, int bx, int cx, int dx, bool with_si)
	{
		e_.push_bp();
		e_.mov_bp_sp();
		if (with_si) e_.byte(0x56); // push si
		e_.load_ax_frame(4);
		e_.byte(0x8b);
		e_.byte(0x9e);
		e_.word(6); // mov bx,[bp+6]
		e_.byte(0x8b);
		e_.byte(0x8e);
		e_.word(8); // mov cx,[bp+8]
		e_.byte(0x8b);
		e_.byte(0x96);
		e_.word(10); // mov dx,[bp+10]
		if (with_si)
		{
			e_.byte(0x8b);
			e_.byte(0xb6);
			e_.word(12); // mov si,[bp+12]
		}
		e_.interrupt(number);

		e_.byte(0x89);
		e_.byte(0x1e);
		data_reference(e_.at(), symbols_[size_t(bx)].offset);
		e_.word(0); // mov [__bx],bx
		e_.byte(0x89);
		e_.byte(0x0e);
		data_reference(e_.at(), symbols_[size_t(cx)].offset);
		e_.word(0); // mov [__cx],cx
		e_.byte(0x89);
		e_.byte(0x16);
		data_reference(e_.at(), symbols_[size_t(dx)].offset);
		e_.word(0); // mov [__dx],dx
		e_.byte(0xc7);
		e_.byte(0x06);
		data_reference(e_.at(), symbols_[size_t(cf)].offset);
		e_.word(0);
		e_.word(0); // mov word [__cf],0
		e_.byte(0x73);
		e_.byte(0x06); // jnc over the next instruction
		e_.byte(0xc7);
		e_.byte(0x06);
		data_reference(e_.at(), symbols_[size_t(cf)].offset);
		e_.word(0);
		e_.word(1); // mov word [__cf],1
		if (with_si) e_.byte(0x5e); // pop si
		e_.pop_bp();
		e_.ret();
	}

	// The subroutines the 32-bit operators call, written out after the program
	// so that a program using no long pays nothing for them. Each takes DX:AX
	// and CX:BX and returns DX:AX.
	void generator::emit_helpers()
	{
		bool used[helper_count] = {};
		for (int i = 0; i < helper_count; ++i) used[i] = !helper_calls_[i].empty();
		if (used[h_ldiv] || used[h_uldiv] || used[h_lmod] || used[h_ulmod]) used[h_uldivmod] = true;

		const auto bytes = [&](std::initializer_list<int> list) { for (const int b : list) e_.byte(b); };
		const auto branch = [&](int opcode)
		{
			e_.byte(opcode);
			e_.byte(0);
			return e_.at() - 1;
		};
		const auto here = [&](int position) { e_.code[size_t(position)] = uint8_t(e_.at() - (position + 1)); };
		const auto again = [&](int opcode, int target) { e_.byte(opcode); e_.byte(uint8_t(target - (e_.at() + 1))); };
		const auto negate = [&] { e_.negate_dx_ax(); };

		if (used[h_uldivmod] || used[h_uldiv])
		{
			// Restoring division a bit at a time: SI:DI accumulates the
			// remainder while the quotient replaces the dividend in DX:AX.
			helper_address_[h_uldivmod] = e_.at();
			helper_address_[h_uldiv] = e_.at();
			bytes({0x56, 0x57, 0x55}); // push si / push di / push bp
			bytes({0x31, 0xf6, 0x31, 0xff}); // xor si,si / xor di,di
			bytes({0xbd, 0x20, 0x00}); // mov bp,32
			const int loop = e_.at();
			bytes({0xd1, 0xe0, 0xd1, 0xd2, 0xd1, 0xd7, 0xd1, 0xd6}); // shl ax,1 / rcl dx,1 / rcl di,1 / rcl si,1
			bytes({0x39, 0xce}); // cmp si,cx
			const int skip_low = branch(0x72); // jb
			const int to_subtract = branch(0x77); // ja
			bytes({0x39, 0xdf}); // cmp di,bx
			const int skip = branch(0x72); // jb
			here(to_subtract);
			bytes({0x29, 0xdf, 0x19, 0xce}); // sub di,bx / sbb si,cx
			bytes({0x0c, 0x01}); // or al,1
			here(skip_low);
			here(skip);
			e_.byte(0x4d); // dec bp
			again(0x75, loop);
			bytes({0x89, 0xf1, 0x89, 0xfb}); // mov cx,si / mov bx,di
			bytes({0x5d, 0x5f, 0x5e}); // pop bp / pop di / pop si
			e_.ret();
		}

		const auto call_divide = [&]
		{
			const int position = e_.call();
			e_.patch_word(position, helper_address_[h_uldivmod] - (position + 2));
		};

		if (used[h_lmul])
		{
			// The high word of a 32-bit product needs only the low halves of
			// the two cross terms, so three 16-bit multiplies are enough.
			helper_address_[h_lmul] = e_.at();
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
			helper_address_[h_ulmod] = e_.at();
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			e_.ret();
		}

		if (used[h_ldiv])
		{
			helper_address_[h_ldiv] = e_.at();
			e_.byte(0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(0x79);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(0x79);
			bytes({0x83, 0xf6, 0x01}); // xor si,1
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(0x74);
			negate();
			here(done);
			e_.byte(0x5e); // pop si
			e_.ret();
		}

		if (used[h_lmod])
		{
			// The sign of a remainder follows the dividend, not the divisor.
			helper_address_[h_lmod] = e_.at();
			e_.byte(0x56); // push si
			bytes({0x31, 0xf6}); // xor si,si
			bytes({0x85, 0xd2}); // test dx,dx
			int positive = branch(0x79);
			bytes({0xbe, 0x01, 0x00}); // mov si,1
			negate();
			here(positive);
			bytes({0x85, 0xc9}); // test cx,cx
			positive = branch(0x79);
			bytes({0xf7, 0xd9, 0xf7, 0xdb, 0x83, 0xd9, 0x00}); // neg cx / neg bx / sbb cx,0
			here(positive);
			call_divide();
			bytes({0x89, 0xd8, 0x89, 0xca}); // mov ax,bx / mov dx,cx
			bytes({0x85, 0xf6}); // test si,si
			const int done = branch(0x74);
			negate();
			here(done);
			e_.byte(0x5e); // pop si
			e_.ret();
		}

		const int shifts[3] = {h_lshl, h_lshr, h_lsar};
		for (const int which : shifts)
		{
			if (!used[which]) continue;
			helper_address_[which] = e_.at();
			bytes({0x88, 0xd9}); // mov cl,bl
			bytes({0x80, 0xe1, 0x1f}); // and cl,31
			const int done = branch(0x74);
			const int loop = e_.at();
			if (which == h_lshl) bytes({0xd1, 0xe0, 0xd1, 0xd2}); // shl ax,1 / rcl dx,1
			else if (which == h_lshr) bytes({0xd1, 0xea, 0xd1, 0xd8}); // shr dx,1 / rcr ax,1
			else bytes({0xd1, 0xfa, 0xd1, 0xd8}); // sar dx,1 / rcr ax,1
			bytes({0xfe, 0xc9}); // dec cl
			again(0x75, loop);
			here(done);
			e_.ret();
		}

		for (const int which : {int(h_lcmp), int(h_ulcmp)})
		{
			if (!used[which]) continue;
			// Reduces the pair to -1, 0 or 1 so the caller can use the ordinary
			// signed conditions whatever the operand signedness was.
			helper_address_[which] = e_.at();
			bytes({0x39, 0xca}); // cmp dx,cx
			const int low = which == h_lcmp ? 0x7c : 0x72;
			const int high = which == h_lcmp ? 0x7f : 0x77;
			const int less_high = branch(low);
			const int greater_high = branch(high);
			bytes({0x39, 0xd8}); // cmp ax,bx
			const int less_low = branch(0x72);
			const int greater_low = branch(0x77);
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

		for (int i = 0; i < helper_count; ++i)
			for (const int position : helper_calls_[i])
				e_.patch_word(position, helper_address_[i] - (position + 2));
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

		const int int21 = define("__int21", sym_function, t_int_);
		symbols_[size_t(int21)].address = e_.at();
		emit_interrupt_call(0x21, cf, bx, cx, dx, false);

		const int int10 = define("__int10", sym_function, t_int_);
		symbols_[size_t(int10)].address = e_.at();
		emit_interrupt_call(0x10, cf, bx, cx, dx, false);

		// The same call with SI set, for the handful of services that take a
		// buffer there rather than in DX.
		const int int21si = define("__int21si", sym_function, t_int_);
		symbols_[size_t(int21si)].address = e_.at();
		emit_interrupt_call(0x21, cf, bx, cx, dx, true);

		// The segment everything lives in, which a program needs when it hands
		// DOS a far pointer.
		const int seg = define("__seg", sym_function, t_uint_);
		symbols_[size_t(seg)].address = e_.at();
		e_.mov_ax_cs();
		e_.ret();
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
		info.size = 2;
		t_int_ = add_type(info);
		info.kind = ty_uint;
		info.size = 2;
		t_uint_ = add_type(info);
		info.kind = ty_long;
		info.size = 4;
		t_long_ = add_type(info);
		info.kind = ty_ulong;
		info.size = 4;
		t_ulong_ = add_type(info);
		info.kind = ty_real;
		info.size = 4;
		t_real_ = add_type(info);

		emit_startup();
		emit_runtime();

		while (kind() != tok::end) declaration();
		emit_helpers();
	}

	std::vector<uint8_t> generator::finish(int& code_size, int& data_size)
	{
		for (const symbol& entry : symbols_)
		{
			if (entry.kind != sym_function || entry.pending_calls.empty()) continue;
			throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
		}

		// Data follows the code, so its address is only known now.
		while (e_.code.size() % 2 != 0) e_.byte(0x90);
		const int data_base = com_origin + int(e_.code.size());

		const auto put_word = [&](std::vector<uint8_t>& into, int at, int value)
		{
			into[size_t(at)] = uint8_t(value & 0xff);
			into[size_t(at) + 1] = uint8_t((value >> 8) & 0xff);
		};

		for (const function_fixup& fixup : function_fixups_)
		{
			const symbol& entry = symbols_[size_t(fixup.function)];
			if (entry.address < 0) throw compile_error{("undefined function: " + entry.name).c_str(), 0, 0};
			const int address = com_origin + entry.address;
			if (fixup.in_data) put_word(data_, fixup.position, address);
			else e_.patch_word(fixup.position, address);
		}

		for (const data_fixup& fixup : data_fixups_)
		{
			if (fixup.position >= 0)
			{
				e_.patch_word(fixup.position, data_base + fixup.offset);
			}
			else
			{
				// A pointer held in the data section itself.
				const int at = -1 - fixup.position;
				const int address = data_base + fixup.offset;
				data_[size_t(at)] = uint8_t(address & 0xff);
				data_[size_t(at) + 1] = uint8_t((address >> 8) & 0xff);
			}
		}

		code_size = int(e_.code.size());
		data_size = int(data_.size());

		const int image_end = data_base + data_size;
		const int stack_top = (image_end + stack_size + 1) & ~1;
		if (stack_top > 0xfff0) throw compile_error{"the program is too large for one segment", 0, 0};
		e_.patch_word(stack_patch_, stack_top);
		e_.patch_word(paragraph_patch_, (stack_top + 15) / 16);

		std::vector<uint8_t> image = e_.code;
		image.insert(image.end(), data_.begin(), data_.end());
		return image;
	}
}

cc_result cc_compile(const char* text, const char* name, cc_sources& sources,
                     const std::vector<std::string>& defines)
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

	generator pass(stream);
	try
	{
		pass.run();
		result.image = pass.finish(result.code_size, result.data_size);
		result.ok = true;
	}
	catch (const compile_error& error)
	{
		result.message = error.message;
		result.line = error.line;
		result.file = error.file >= 0 && error.file < int(stream.files.size())
			              ? stream.files[size_t(error.file)]
			              : std::string(name);
	}
	return result;
}
