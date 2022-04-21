// ovrwin by Zac Walker
//
// 8086/80186 interpreter. One call to cpu_step executes one instruction, or
// one repetition of a string instruction, so an interrupt can land in the
// middle of a REP exactly as it does on hardware.
//
// The core decode is derived from Andrew Jenner's reenigne 8086 emulator by
// way of the demu DOS tool, reworked into machine_state and extended with the
// 80186 instruction set that late-80s DOS games assume.

#include "machine.h"

#include <cstdio>
#include <cstring>

namespace
{
	const uint8_t parity_table[256] = {
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,
		4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
	};

	// One decoded instruction. Everything that has to survive between
	// repetitions of a string instruction lives in cpu_state instead.
	struct x86
	{
		machine_state& s;
		cpu_state& c;

		uint16_t address = 0;
		bool use_memory = false;
		uint8_t mod_rm = 0;
		int segment = seg_ds;
		bool word_size = false;
		bool source_is_rm = false;
		uint32_t data = 0;
		uint32_t source = 0;
		uint32_t destination = 0;
		uint16_t residue = 0;
		int alu_operation = 0;
		uint16_t saved_ip = 0;
		uint16_t saved_cs = 0;
		int cycles = 4;

		x86(machine_state& state) : s(state), c(state.cpu)
		{
		}

		void undefined(const char* what)
		{
			if (s.error[0] == 0)
				snprintf(s.error, sizeof(s.error), "%s at %04X:%04X", what, c.cs(), c.ip);
		}

		uint32_t physical(uint16_t offset, int seg)
		{
			int sr = seg;
			if (sr < 0)
			{
				sr = segment;
				if (c.segment_override >= 0) sr = c.segment_override;
			}
			return ((uint32_t(c.regs[reg_seg + sr]) << 4) + offset) & 0xfffff;
		}

		uint8_t read_byte(uint16_t offset, int seg) { return mem_read8(s, physical(offset, seg)); }

		uint16_t read_word(uint16_t offset, int seg)
		{
			return uint16_t(mem_read8(s, physical(offset, seg)) |
				(mem_read8(s, physical(uint16_t(offset + 1), seg)) << 8));
		}

		void write_byte(uint8_t value, uint16_t offset, int seg) { mem_write8(s, physical(offset, seg), value); }

		void write_word(uint16_t value, uint16_t offset, int seg)
		{
			mem_write8(s, physical(offset, seg), uint8_t(value));
			mem_write8(s, physical(uint16_t(offset + 1), seg), uint8_t(value >> 8));
		}

		uint16_t read_wb(uint16_t offset, int seg) { return word_size ? read_word(offset, seg) : read_byte(offset, seg); }

		void write_wb(uint16_t value, uint16_t offset, int seg)
		{
			if (word_size) write_word(value, offset, seg);
			else write_byte(uint8_t(value), offset, seg);
		}

		uint8_t fetch_byte()
		{
			const uint8_t b = read_byte(c.ip, seg_cs);
			++c.ip;
			return b;
		}

		uint16_t fetch_word()
		{
			uint16_t w = fetch_byte();
			w = uint16_t(w | (fetch_byte() << 8));
			return w;
		}

		uint16_t fetch(bool ws) { return ws ? fetch_word() : fetch_byte(); }
		static uint16_t sign_extend(uint8_t v) { return uint16_t(v + (v < 0x80 ? 0 : 0xff00)); }
		int mod_rm_reg() const { return (mod_rm >> 3) & 7; }

		void jump(uint16_t target) { c.ip = target; }

		void jump_short(uint8_t offset, bool taken)
		{
			if (taken)
			{
				jump(uint16_t(c.ip + sign_extend(offset)));
				cycles += 12;
			}
		}

		bool cf() const { return (c.flags & flag_cf) != 0; }
		bool pf() const { return (c.flags & flag_pf) != 0; }
		bool af() const { return (c.flags & flag_af) != 0; }
		bool zf() const { return (c.flags & flag_zf) != 0; }
		bool sf() const { return (c.flags & flag_sf) != 0; }
		bool df() const { return (c.flags & flag_df) != 0; }
		bool of() const { return (c.flags & flag_of) != 0; }

		void set_cf(bool v) { c.set_flag(flag_cf, v); }
		void set_af(bool v) { c.set_flag(flag_af, v); }
		void set_of(bool v) { c.set_flag(flag_of, v); }
		void set_if(bool v) { c.set_flag(flag_if, v); }
		void set_df(bool v) { c.set_flag(flag_df, v); }
		void set_ca() { set_cf(true); set_af(true); }
		void clear_ca() { set_cf(false); set_af(false); }
		void clear_cao() { clear_ca(); set_of(false); }

		void set_pf() { c.flags = uint16_t((c.flags & ~flag_pf) | parity_table[data & 0xff]); }
		void set_zf() { c.set_flag(flag_zf, (data & (word_size ? 0xffff : 0xff)) == 0); }
		void set_sf() { c.set_flag(flag_sf, (data & (word_size ? 0x8000 : 0x80)) != 0); }
		void set_pzs() { set_pf(); set_zf(); set_sf(); }

		void bitwise(uint16_t value)
		{
			data = value;
			clear_cao();
			set_pzs();
		}

		void test_op(uint16_t d, uint16_t sv)
		{
			destination = d;
			source = sv;
			bitwise(uint16_t(destination & source));
		}

		void do_af() { set_af(((data ^ source ^ destination) & 0x10) != 0); }
		void do_cf() { set_cf((data & (word_size ? 0x10000 : 0x100)) != 0); }
		void set_capzs() { set_pzs(); do_af(); do_cf(); }

		void set_of_add()
		{
			const uint32_t t = (data ^ source) & (data ^ destination);
			set_of((t & (word_size ? 0x8000 : 0x80)) != 0);
		}

		void set_of_sub()
		{
			const uint32_t t = (destination ^ source) & (data ^ destination);
			set_of((t & (word_size ? 0x8000 : 0x80)) != 0);
		}

		void set_of_rotate() { set_of(((data ^ destination) & (word_size ? 0x8000 : 0x80)) != 0); }

		void add()
		{
			data = destination + source;
			set_capzs();
			set_of_add();
		}

		void sub()
		{
			data = destination - source;
			set_capzs();
			set_of_sub();
		}

		void do_alu()
		{
			switch (alu_operation)
			{
			case 0: add();
				break;
			case 1: bitwise(uint16_t(destination | source));
				break;
			case 2: source += cf() ? 1 : 0;
				add();
				break;
			case 3: source += cf() ? 1 : 0;
				sub();
				break;
			case 4: test_op(uint16_t(destination), uint16_t(source));
				break;
			case 5: sub();
				break;
			case 6: bitwise(uint16_t(destination ^ source));
				break;
			default: sub();
				break; // CMP
			}
		}

		int string_increment() const
		{
			const int r = word_size ? 2 : 1;
			return df() ? -r : r;
		}

		uint16_t lods()
		{
			address = c.si();
			c.set_si(uint16_t(c.si() + string_increment()));
			segment = seg_ds;
			return read_wb(address, -1);
		}

		uint16_t lods_di()
		{
			address = c.di();
			c.set_di(uint16_t(c.di() + string_increment()));
			return read_wb(address, seg_es);
		}

		void stos(uint16_t value)
		{
			address = c.di();
			c.set_di(uint16_t(c.di() + string_increment()));
			write_wb(value, address, seg_es);
		}

		void do_rep(bool compare)
		{
			if (c.rep == 0 || c.cx() == 0) return;
			c.set_cx(uint16_t(c.cx() - 1));
			c.repeating = c.cx() != 0 && (!compare || zf() != (c.rep == 1));
			cycles += 9;
		}

		void push(uint16_t value)
		{
			c.set_sp(uint16_t(c.sp() - 2));
			write_word(value, c.sp(), seg_ss);
		}

		uint16_t pop()
		{
			const uint16_t r = read_word(c.sp(), seg_ss);
			c.set_sp(uint16_t(c.sp() + 2));
			return r;
		}

		uint16_t* mod_rm_rw() { return &c.regs[mod_rm_reg()]; }

		uint16_t get_reg() const { return word_size ? c.regs[(mod_rm >> 3) & 7] : c.reg8((mod_rm >> 3) & 7); }

		void set_reg(uint16_t value)
		{
			if (word_size) c.regs[(mod_rm >> 3) & 7] = value;
			else c.set_reg8((mod_rm >> 3) & 7, uint8_t(value));
		}

		uint16_t get_accum() const { return word_size ? c.ax() : c.al(); }

		void set_accum()
		{
			if (word_size) c.set_ax(uint16_t(data));
			else c.set_al(uint8_t(data));
		}

		uint16_t effective_address()
		{
			mod_rm = fetch_byte();
			use_memory = true;
			switch (mod_rm & 7)
			{
			case 0: segment = seg_ds; address = uint16_t(c.bx() + c.si()); break;
			case 1: segment = seg_ds; address = uint16_t(c.bx() + c.di()); break;
			case 2: segment = seg_ss; address = uint16_t(c.bp() + c.si()); break;
			case 3: segment = seg_ss; address = uint16_t(c.bp() + c.di()); break;
			case 4: segment = seg_ds; address = c.si(); break;
			case 5: segment = seg_ds; address = c.di(); break;
			case 6: segment = seg_ss; address = c.bp(); break;
			default: segment = seg_ds; address = c.bx(); break;
			}
			switch (mod_rm & 0xc0)
			{
			case 0x00:
				if ((mod_rm & 0xc7) == 6)
				{
					segment = seg_ds;
					address = fetch_word();
				}
				cycles += 6;
				break;
			case 0x40:
				address = uint16_t(address + sign_extend(fetch_byte()));
				cycles += 8;
				break;
			case 0x80:
				address = uint16_t(address + fetch_word());
				cycles += 8;
				break;
			default:
				use_memory = false;
				address = mod_rm & 7;
				break;
			}
			return address;
		}

		uint16_t read_ea_again()
		{
			if (!use_memory) return word_size ? c.regs[address] : c.reg8(address);
			return read_wb(address, -1);
		}

		uint16_t read_ea()
		{
			effective_address();
			return read_ea_again();
		}

		void write_ea_again(uint16_t value)
		{
			if (!use_memory)
			{
				if (word_size) c.regs[address] = value;
				else c.set_reg8(address, uint8_t(value));
			}
			else
			{
				write_wb(value, address, -1);
			}
		}

		void write_ea(uint16_t value)
		{
			effective_address();
			write_ea_again(value);
		}

		void far_load()
		{
			if (!use_memory)
			{
				undefined("far pointer needs memory operand");
				return;
			}
			saved_ip = read_word(address, -1);
			saved_cs = read_word(uint16_t(address + 2), -1);
		}

		void far_jump()
		{
			c.set_cs(saved_cs);
			jump(saved_ip);
		}

		void far_call()
		{
			push(c.cs());
			push(c.ip);
			far_jump();
		}

		void call_near(uint16_t target)
		{
			push(c.ip);
			jump(target);
		}

		uint16_t inc_dec(bool decrement)
		{
			source = 1;
			if (!decrement)
			{
				data = destination + source;
				set_of_add();
			}
			else
			{
				data = destination - source;
				set_of_sub();
			}
			do_af();
			set_pzs();
			return uint16_t(data);
		}

		void divide_overflow()
		{
			cpu_interrupt(s, int_divide_error);
			data = source = 1;
		}

		void divide()
		{
			bool negative = false;
			bool dividend_negative = false;
			if (mod_rm_reg() == 7)
			{
				if ((destination & 0x80000000) != 0)
				{
					destination = uint32_t(-int32_t(destination));
					negative = !negative;
					dividend_negative = true;
				}
				if ((source & 0x8000) != 0)
				{
					source = uint32_t(-int32_t(source)) & 0xffff;
					negative = !negative;
				}
			}
			if (source == 0)
			{
				divide_overflow();
				return;
			}
			data = destination / source;
			uint32_t product = data * source;
			if (product > destination)
			{
				--data;
				product -= source;
			}
			residue = uint16_t(destination - product);
			if (negative) data = uint32_t(-int32_t(data));
			if (dividend_negative) residue = uint16_t(-int16_t(residue));
		}

		void rotate_shift()
		{
			while (source != 0)
			{
				destination = data;
				switch (mod_rm_reg())
				{
				case 0: // ROL
					data <<= 1;
					do_cf();
					data |= cf() ? 1 : 0;
					set_of_rotate();
					break;
				case 1: // ROR
					set_cf((data & 1) != 0);
					data >>= 1;
					if (cf()) data |= word_size ? 0x8000 : 0x80;
					set_of_rotate();
					break;
				case 2: // RCL
					data = (data << 1) | (cf() ? 1 : 0);
					do_cf();
					set_of_rotate();
					break;
				case 3: // RCR
					data >>= 1;
					if (cf()) data |= word_size ? 0x8000 : 0x80;
					set_cf((destination & 1) != 0);
					set_of_rotate();
					break;
				case 4:
				case 6: // SHL / SAL
					data <<= 1;
					do_cf();
					set_of_rotate();
					set_pzs();
					break;
				case 5: // SHR
					set_cf((data & 1) != 0);
					data >>= 1;
					set_of_rotate();
					set_af(true);
					set_pzs();
					break;
				default: // SAR
					set_cf((data & 1) != 0);
					data >>= 1;
					data |= destination & (word_size ? 0x8000 : 0x80);
					set_of_rotate();
					set_af(true);
					set_pzs();
					break;
				}
				--source;
				++cycles;
			}
		}

		void imul_immediate(uint16_t immediate)
		{
			const int32_t a = int16_t(data);
			const int32_t b = int16_t(immediate);
			const int32_t r = a * b;
			set_reg(uint16_t(r));
			const bool overflow = r != int16_t(r);
			set_cf(overflow);
			set_of(overflow);
			cycles += 20;
		}

		void execute();
	};

	void push16(machine_state& s, uint16_t value)
	{
		cpu_state& c = s.cpu;
		c.set_sp(uint16_t(c.sp() - 2));
		mem_write16(s, ((uint32_t(c.ss()) << 4) + c.sp()) & 0xfffff, value);
	}

	void x86::execute()
	{
		if (!c.repeating)
		{
			if (!c.prefix)
			{
				c.segment_override = -1;
				c.rep = 0;
				c.instruction_ip = c.ip;
			}
			c.prefix = false;
			c.opcode = fetch_byte();
		}

		const uint8_t opcode = c.opcode;
		word_size = (opcode & 1) != 0;
		source_is_rm = (opcode & 2) != 0;
		const int operation = (opcode >> 3) & 7;
		bool taken = false;

		switch (opcode)
		{
		case 0x00: case 0x01: case 0x02: case 0x03:
		case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x10: case 0x11: case 0x12: case 0x13:
		case 0x18: case 0x19: case 0x1a: case 0x1b:
		case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x28: case 0x29: case 0x2a: case 0x2b:
		case 0x30: case 0x31: case 0x32: case 0x33:
		case 0x38: case 0x39: case 0x3a: case 0x3b: // alu rm,r
			data = read_ea();
			if (!source_is_rm)
			{
				destination = data;
				source = get_reg();
			}
			else
			{
				destination = get_reg();
				source = data;
			}
			alu_operation = operation;
			do_alu();
			if (alu_operation != 7)
			{
				if (!source_is_rm) write_ea_again(uint16_t(data));
				else set_reg(uint16_t(data));
			}
			break;

		case 0x04: case 0x05: case 0x0c: case 0x0d:
		case 0x14: case 0x15: case 0x1c: case 0x1d:
		case 0x24: case 0x25: case 0x2c: case 0x2d:
		case 0x34: case 0x35: case 0x3c: case 0x3d: // alu accum,imm
			destination = get_accum();
			source = fetch(word_size);
			alu_operation = operation;
			do_alu();
			if (alu_operation != 7) set_accum();
			break;

		case 0x06: case 0x0e: case 0x16: case 0x1e: // PUSH segreg
			push(c.regs[reg_seg + operation]);
			break;

		case 0x07: case 0x17: case 0x1f: // POP segreg
			c.regs[reg_seg + operation] = pop();
			break;

		case 0x26: case 0x2e: case 0x36: case 0x3e: // segment override
			c.segment_override = operation - 4;
			c.prefix = true;
			cycles = 2;
			break;

		case 0x27: // DAA
		case 0x2f: // DAS
			if (af() || (c.al() & 0x0f) > 9)
			{
				data = uint32_t(c.al() + (opcode == 0x27 ? 6 : -6));
				c.set_al(uint8_t(data));
				set_af(true);
				if ((data & 0x100) != 0) set_cf(true);
			}
			set_cf(cf() || c.al() > 0x9f);
			if (cf()) c.set_al(uint8_t(c.al() + (opcode == 0x27 ? 0x60 : -0x60)));
			word_size = false;
			data = c.al();
			set_pzs();
			break;

		case 0x37: // AAA
		case 0x3f: // AAS
			if (af() || (c.al() & 0xf) > 9)
			{
				c.set_al(uint8_t(c.al() + (opcode == 0x37 ? 6 : -6)));
				c.set_ah(uint8_t(c.ah() + (opcode == 0x37 ? 1 : -1)));
				set_ca();
			}
			else
			{
				clear_ca();
			}
			c.set_al(uint8_t(c.al() & 0x0f));
			break;

		case 0x40: case 0x41: case 0x42: case 0x43:
		case 0x44: case 0x45: case 0x46: case 0x47:
		case 0x48: case 0x49: case 0x4a: case 0x4b:
		case 0x4c: case 0x4d: case 0x4e: case 0x4f: // INC/DEC rw
			destination = c.regs[opcode & 7];
			word_size = true;
			c.regs[opcode & 7] = inc_dec((opcode & 8) != 0);
			cycles = 3;
			break;

		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57: // PUSH rw
			push(c.regs[opcode & 7]);
			cycles = 11;
			break;

		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f: // POP rw
			c.regs[opcode & 7] = pop();
			cycles = 8;
			break;

		case 0x60: // PUSHA (80186)
		{
			const uint16_t old_sp = c.sp();
			for (int i = 0; i < 8; ++i) push(i == reg_sp ? old_sp : c.regs[i]);
			cycles = 36;
			break;
		}

		case 0x61: // POPA (80186)
			for (int i = 7; i >= 0; --i)
			{
				const uint16_t v = pop();
				if (i != reg_sp) c.regs[i] = v;
			}
			cycles = 51;
			break;

		case 0x62: // BOUND (80186)
		{
			word_size = true;
			effective_address();
			const uint16_t index = get_reg();
			const int16_t lower = int16_t(read_word(address, -1));
			const int16_t upper = int16_t(read_word(uint16_t(address + 2), -1));
			if (int16_t(index) < lower || int16_t(index) > upper) cpu_interrupt(s, int_bound);
			break;
		}

		case 0x68: // PUSH imm16 (80186)
			push(fetch_word());
			break;

		case 0x69: // IMUL r,rm,imm16 (80186)
			word_size = true;
			data = read_ea();
			imul_immediate(fetch_word());
			break;

		case 0x6a: // PUSH imm8 (80186)
			push(sign_extend(fetch_byte()));
			break;

		case 0x6b: // IMUL r,rm,imm8 (80186)
			word_size = true;
			data = read_ea();
			imul_immediate(sign_extend(fetch_byte()));
			break;

		case 0x6c: case 0x6d: // INSB/INSW (80186)
			if (c.rep == 0 || c.cx() != 0)
			{
				const uint16_t v = word_size
					? uint16_t(io_read8(s, c.dx()) | (io_read8(s, uint16_t(c.dx() + 1)) << 8))
					: io_read8(s, c.dx());
				stos(v);
			}
			do_rep(false);
			break;

		case 0x6e: case 0x6f: // OUTSB/OUTSW (80186)
			if (c.rep == 0 || c.cx() != 0)
			{
				const uint16_t v = lods();
				io_write8(s, c.dx(), uint8_t(v));
				if (word_size) io_write8(s, uint16_t(c.dx() + 1), uint8_t(v >> 8));
			}
			do_rep(false);
			break;

		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b:
		case 0x7c: case 0x7d: case 0x7e: case 0x7f: // Jcc
			switch (opcode & 0x0e)
			{
			case 0x00: taken = of(); break;
			case 0x02: taken = cf(); break;
			case 0x04: taken = zf(); break;
			case 0x06: taken = cf() || zf(); break;
			case 0x08: taken = sf(); break;
			case 0x0a: taken = pf(); break;
			case 0x0c: taken = sf() != of(); break;
			default: taken = sf() != of() || zf(); break;
			}
			jump_short(fetch_byte(), taken == ((opcode & 1) == 0));
			break;

		case 0x80: case 0x81: case 0x82: case 0x83: // alu rm,imm
			destination = read_ea();
			data = fetch(opcode == 0x81);
			source = opcode == 0x83 ? sign_extend(uint8_t(data)) : data;
			alu_operation = mod_rm_reg();
			do_alu();
			if (alu_operation != 7) write_ea_again(uint16_t(data));
			break;

		case 0x84: case 0x85: // TEST rm,r
			data = read_ea();
			test_op(uint16_t(data), get_reg());
			break;

		case 0x86: case 0x87: // XCHG rm,r
			data = read_ea();
			write_ea_again(get_reg());
			set_reg(uint16_t(data));
			break;

		case 0x88: case 0x89: // MOV rm,r
			effective_address();
			write_ea_again(get_reg());
			break;

		case 0x8a: case 0x8b: // MOV r,rm
			set_reg(read_ea());
			break;

		case 0x8c: // MOV rm,segreg
			effective_address();
			word_size = true;
			write_ea_again(c.regs[reg_seg + mod_rm_reg()]);
			break;

		case 0x8d: // LEA
			address = effective_address();
			word_size = true;
			if (!use_memory) undefined("LEA needs a memory operand");
			set_reg(address);
			break;

		case 0x8e: // MOV segreg,rm
			word_size = true;
			data = read_ea();
			c.regs[reg_seg + mod_rm_reg()] = uint16_t(data);
			break;

		case 0x8f: // POP rm
			word_size = true;
			write_ea(pop());
			break;

		case 0x90: case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96: case 0x97: // XCHG AX,rw
			data = c.ax();
			c.set_ax(c.regs[opcode & 7]);
			c.regs[opcode & 7] = uint16_t(data);
			cycles = 3;
			break;

		case 0x98: // CBW
			c.set_ax(sign_extend(c.al()));
			break;

		case 0x99: // CWD
			c.set_dx((c.ax() & 0x8000) == 0 ? 0x0000 : 0xffff);
			break;

		case 0x9a: // CALL far
			saved_ip = fetch_word();
			saved_cs = fetch_word();
			far_call();
			cycles = 28;
			break;

		case 0x9b: // WAIT
			break;

		case 0x9c: // PUSHF
			push(uint16_t((c.flags & 0x0fd7) | 0xf000));
			break;

		case 0x9d: // POPF
			c.flags = uint16_t(pop() | 2);
			break;

		case 0x9e: // SAHF
			c.flags = uint16_t((c.flags & 0xff02) | c.ah());
			break;

		case 0x9f: // LAHF
			c.set_ah(uint8_t(c.flags & 0xd7));
			break;

		case 0xa0: case 0xa1: // MOV accum,[imm]
			segment = seg_ds;
			data = read_wb(fetch_word(), -1);
			set_accum();
			break;

		case 0xa2: case 0xa3: // MOV [imm],accum
			segment = seg_ds;
			write_wb(get_accum(), fetch_word(), -1);
			break;

		case 0xa4: case 0xa5: // MOVS
			if (c.rep == 0 || c.cx() != 0) stos(lods());
			do_rep(false);
			cycles = 18;
			break;

		case 0xa6: case 0xa7: // CMPS
			if (c.rep == 0 || c.cx() != 0)
			{
				destination = lods();
				source = lods_di();
				sub();
			}
			do_rep(true);
			cycles = 22;
			break;

		case 0xa8: case 0xa9: // TEST accum,imm
			data = fetch(word_size);
			test_op(get_accum(), uint16_t(data));
			break;

		case 0xaa: case 0xab: // STOS
			if (c.rep == 0 || c.cx() != 0) stos(get_accum());
			do_rep(false);
			cycles = 11;
			break;

		case 0xac: case 0xad: // LODS
			if (c.rep == 0 || c.cx() != 0)
			{
				data = lods();
				set_accum();
			}
			do_rep(false);
			cycles = 12;
			break;

		case 0xae: case 0xaf: // SCAS
			if (c.rep == 0 || c.cx() != 0)
			{
				destination = get_accum();
				source = lods_di();
				sub();
			}
			do_rep(true);
			cycles = 15;
			break;

		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7: // MOV rb,imm
			c.set_reg8(opcode & 7, fetch_byte());
			break;

		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf: // MOV rw,imm
			c.regs[opcode & 7] = fetch_word();
			break;

		case 0xc0: case 0xc1: // shift rm,imm8 (80186)
			word_size = (opcode & 1) != 0;
			data = read_ea();
			source = fetch_byte() & 0x1f;
			rotate_shift();
			write_ea_again(uint16_t(data));
			break;

		case 0xc2: case 0xc3: case 0xca: case 0xcb: // RET
			saved_ip = pop();
			saved_cs = (opcode & 8) == 0 ? c.cs() : pop();
			if (!word_size) c.set_sp(uint16_t(c.sp() + fetch_word()));
			far_jump();
			cycles = 20;
			break;

		case 0xc4: case 0xc5: // LES / LDS
			word_size = true;
			effective_address();
			far_load();
			*mod_rm_rw() = saved_ip;
			c.regs[reg_seg + ((opcode & 1) == 0 ? seg_es : seg_ds)] = saved_cs;
			break;

		case 0xc6: case 0xc7: // MOV rm,imm
			effective_address();
			write_ea_again(fetch(word_size));
			break;

		case 0xc8: // ENTER (80186)
		{
			const uint16_t frame_size = fetch_word();
			const uint8_t level = fetch_byte() & 0x1f;
			push(c.bp());
			const uint16_t frame = c.sp();
			for (int i = 1; i < level; ++i)
			{
				c.set_bp(uint16_t(c.bp() - 2));
				push(read_word(c.bp(), seg_ss));
			}
			if (level > 0) push(frame);
			c.set_bp(frame);
			c.set_sp(uint16_t(c.sp() - frame_size));
			cycles = 15;
			break;
		}

		case 0xc9: // LEAVE (80186)
			c.set_sp(c.bp());
			c.set_bp(pop());
			cycles = 8;
			break;

		case 0xcc: // INT 3
			cpu_interrupt(s, int_breakpoint);
			cycles = 52;
			break;

		case 0xcd: // INT imm8
			cpu_interrupt(s, fetch_byte());
			cycles = 51;
			break;

		case 0xce: // INTO
			if (of()) cpu_interrupt(s, int_overflow);
			break;

		case 0xcf: // IRET
			jump(pop());
			c.set_cs(pop());
			c.flags = uint16_t(pop() | 2);
			cycles = 24;
			break;

		case 0xd0: case 0xd1: case 0xd2: case 0xd3: // shift rm,1 / rm,CL
			data = read_ea();
			source = (opcode & 2) == 0 ? 1 : (c.cl() & 0x1f);
			rotate_shift();
			write_ea_again(uint16_t(data));
			break;

		case 0xd4: // AAM
			data = fetch_byte();
			if (data == 0)
			{
				divide_overflow();
				break;
			}
			c.set_ah(uint8_t(c.al() / uint8_t(data)));
			c.set_al(uint8_t(c.al() % uint8_t(data)));
			word_size = true;
			data = c.ax();
			set_pzs();
			break;

		case 0xd5: // AAD
			data = fetch_byte();
			c.set_al(uint8_t(c.al() + c.ah() * uint8_t(data)));
			c.set_ah(0);
			word_size = false;
			data = c.al();
			set_pzs();
			break;

		case 0xd6: // SALC
			c.set_al(cf() ? 0xff : 0x00);
			break;

		case 0xd7: // XLATB
			segment = seg_ds;
			c.set_al(read_byte(uint16_t(c.bx() + c.al()), -1));
			break;

		case 0xd8: case 0xd9: case 0xda: case 0xdb:
		case 0xdc: case 0xdd: case 0xde: case 0xdf: // x87 escape
			// No coprocessor: consume the operand bytes and carry on. Programs
			// that need one check for it first and pick a software path.
			effective_address();
			break;

		case 0xe0: case 0xe1: case 0xe2: // LOOPcc
			c.set_cx(uint16_t(c.cx() - 1));
			taken = c.cx() != 0;
			if (opcode == 0xe0 && zf()) taken = false;
			if (opcode == 0xe1 && !zf()) taken = false;
			jump_short(fetch_byte(), taken);
			break;

		case 0xe3: // JCXZ
			jump_short(fetch_byte(), c.cx() == 0);
			break;

		case 0xe4: case 0xe5: // IN imm8
		{
			const uint16_t port = fetch_byte();
			data = word_size ? uint16_t(io_read8(s, port) | (io_read8(s, uint16_t(port + 1)) << 8)) : io_read8(s, port);
			set_accum();
			cycles = 10;
			break;
		}

		case 0xe6: case 0xe7: // OUT imm8
		{
			const uint16_t port = fetch_byte();
			io_write8(s, port, uint8_t(get_accum()));
			if (word_size) io_write8(s, uint16_t(port + 1), uint8_t(get_accum() >> 8));
			cycles = 10;
			break;
		}

		case 0xe8: // CALL near
			data = fetch_word();
			call_near(uint16_t(c.ip + data));
			cycles = 19;
			break;

		case 0xe9: // JMP near
			data = fetch_word();
			jump(uint16_t(c.ip + data));
			cycles = 15;
			break;

		case 0xea: // JMP far
			saved_ip = fetch_word();
			saved_cs = fetch_word();
			far_jump();
			cycles = 15;
			break;

		case 0xeb: // JMP short
			jump_short(fetch_byte(), true);
			break;

		case 0xec: case 0xed: // IN DX
			data = word_size
				? uint16_t(io_read8(s, c.dx()) | (io_read8(s, uint16_t(c.dx() + 1)) << 8))
				: io_read8(s, c.dx());
			set_accum();
			cycles = 8;
			break;

		case 0xee: case 0xef: // OUT DX
			io_write8(s, c.dx(), uint8_t(get_accum()));
			if (word_size) io_write8(s, uint16_t(c.dx() + 1), uint8_t(get_accum() >> 8));
			cycles = 8;
			break;

		case 0xf0: // LOCK
			c.prefix = true;
			cycles = 2;
			break;

		case 0xf2: // REPNZ
		case 0xf3: // REPZ
			c.rep = opcode == 0xf2 ? 1 : 2;
			c.prefix = true;
			cycles = 2;
			break;

		case 0xf4: // HLT
			c.halted = true;
			cycles = 2;
			break;

		case 0xf5: // CMC
			c.flags ^= flag_cf;
			break;

		case 0xf6: case 0xf7: // group 3
			data = read_ea();
			switch (mod_rm_reg())
			{
			case 0: case 1: // TEST rm,imm
				test_op(uint16_t(data), fetch(word_size));
				break;
			case 2: // NOT
				write_ea_again(uint16_t(~data));
				break;
			case 3: // NEG
				source = data;
				destination = 0;
				sub();
				write_ea_again(uint16_t(data));
				break;
			case 4: case 5: // MUL / IMUL
				source = data;
				destination = get_accum();
				data = destination;
				set_sf();
				set_pf();
				data *= source;
				c.set_ax(uint16_t(data));
				if (!word_size)
				{
					if (mod_rm_reg() == 4)
					{
						set_cf(c.ah() != 0);
					}
					else
					{
						if ((source & 0x80) != 0) c.set_ah(uint8_t(c.ah() - uint8_t(destination)));
						if ((destination & 0x80) != 0) c.set_ah(uint8_t(c.ah() - uint8_t(source)));
						set_cf(c.ah() == ((c.al() & 0x80) == 0 ? 0 : 0xff));
					}
				}
				else
				{
					c.set_dx(uint16_t(data >> 16));
					if (mod_rm_reg() == 4)
					{
						data |= c.dx();
						set_cf(c.dx() != 0);
					}
					else
					{
						if ((source & 0x8000) != 0) c.set_dx(uint16_t(c.dx() - destination));
						if ((destination & 0x8000) != 0) c.set_dx(uint16_t(c.dx() - source));
						data |= c.dx();
						set_cf(c.dx() == ((c.ax() & 0x8000) == 0 ? 0 : 0xffff));
					}
				}
				set_zf();
				set_of(cf());
				cycles += 70;
				break;
			default: // DIV / IDIV
				source = data;
				if (source == 0)
				{
					divide_overflow();
					break;
				}
				if (!word_size)
				{
					destination = c.ax();
					if (mod_rm_reg() == 6)
					{
						divide();
						if (data > 0xff) divide_overflow();
					}
					else
					{
						if ((destination & 0x8000) != 0) destination |= 0xffff0000;
						source = sign_extend(uint8_t(source));
						divide();
						if (data > 0x7f && data < 0xffffff80) divide_overflow();
					}
					c.set_ah(uint8_t(residue));
					c.set_al(uint8_t(data));
				}
				else
				{
					destination = (uint32_t(c.dx()) << 16) + c.ax();
					divide();
					if (mod_rm_reg() == 6)
					{
						if (data > 0xffff) divide_overflow();
					}
					else
					{
						if (data > 0x7fff && data < 0xffff8000) divide_overflow();
					}
					c.set_dx(residue);
					c.set_ax(uint16_t(data));
				}
				cycles += 80;
				break;
			}
			break;

		case 0xf8: case 0xf9: // CLC / STC
			set_cf(word_size);
			break;

		case 0xfa: case 0xfb: // CLI / STI
			set_if(word_size);
			break;

		case 0xfc: case 0xfd: // CLD / STD
			set_df(word_size);
			break;

		case 0xfe: case 0xff: // group 4/5
			effective_address();
			switch (mod_rm_reg())
			{
			case 0: case 1: // INC / DEC rm
				destination = read_ea_again();
				write_ea_again(inc_dec(mod_rm_reg() != 0));
				break;
			case 2: // CALL rm
				call_near(read_ea_again());
				cycles = 21;
				break;
			case 3: // CALL far [rm]
				far_load();
				far_call();
				cycles = 37;
				break;
			case 4: // JMP rm
				jump(read_ea_again());
				cycles = 18;
				break;
			case 5: // JMP far [rm]
				far_load();
				far_jump();
				cycles = 24;
				break;
			case 6: // PUSH rm
				push(read_ea_again());
				cycles = 16;
				break;
			default:
				undefined("invalid group 5 operation");
				break;
			}
			break;

		case 0x0f: // 80286 two-byte escape; only the harmless bits are modelled
		{
			const uint8_t second = fetch_byte();
			if (second == 0x01)
			{
				effective_address();
				if (mod_rm_reg() == 4) write_ea_again(0); // SMSW: real mode, no PE
			}
			else
			{
				undefined("unsupported 0F opcode");
			}
			break;
		}

		default:
			if (opcode == callback_opcode)
			{
				machine_callback(s, fetch_byte());
				cycles = 2;
			}
			else
			{
				undefined("invalid opcode");
			}
			break;
		}
	}
}

void cpu_reset(cpu_state& c)
{
	c = cpu_state();
}

void cpu_interrupt(machine_state& s, uint8_t vector)
{
	cpu_state& c = s.cpu;
	push16(s, c.flags);
	push16(s, c.cs());
	push16(s, c.ip);
	c.set_flag(flag_if | flag_tf, false);
	c.set_cs(mem_read16(s, uint32_t(vector) * 4 + 2));
	c.ip = mem_read16(s, uint32_t(vector) * 4);
	c.halted = false;
	c.repeating = false;
	c.prefix = false;
}

void cpu_set_iret_flag(machine_state& s, uint16_t mask, bool on)
{
	// Inside one of our stubs the stack still holds IP, CS then FLAGS, so the
	// flags an IRET will restore are four bytes above SP.
	const cpu_state& c = s.cpu;
	const uint32_t address = ((uint32_t(c.ss()) << 4) + uint16_t(c.sp() + 4)) & 0xfffff;
	const uint16_t flags = mem_read16(s, address);
	mem_write16(s, address, uint16_t(on ? (flags | mask) : (flags & ~mask)));
}

int cpu_step(machine_state& s)
{
	cpu_state& c = s.cpu;

	// Hardware interrupts are taken between instructions, never inside a
	// prefix sequence, but they may interrupt a REP.
	if (!c.prefix && c.if_())
	{
		const int vector = pic_pending_vector(s);
		if (vector >= 0)
		{
			pic_acknowledge(s, vector);
			if (c.repeating)
			{
				// A REP is resumed, not abandoned: the return address points at
				// the prefix, so the instruction restarts with whatever CX is
				// left. Getting this wrong silently truncates block moves.
				c.ip = c.instruction_ip;
				c.repeating = false;
			}
			cpu_interrupt(s, uint8_t(vector));
			return 61;
		}
	}

	if (c.halted) return 8;

	x86 x(s);
	x.execute();
	c.cycles += x.cycles;
	return x.cycles;
}
