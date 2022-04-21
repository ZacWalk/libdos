// ovrwin by Zac Walker
//
// Disassembler and debug capture. Everything here reads emulated memory
// directly rather than through the bus helpers, so looking at a machine can
// never change it.

#include "machine.h"
#include "debug.h"

#include <cstdio>
#include <cstring>

namespace
{
	const char* const reg16_names[8] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
	const char* const reg8_names[8] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
	const char* const segment_names[4] = {"es", "cs", "ss", "ds"};
	const char* const memory_forms[8] = {
		"bx+si", "bx+di", "bp+si", "bp+di", "si", "di", "bp", "bx"
	};
	const char* const alu_names[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
	const char* const shift_names[8] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "shl", "sar"};
	const char* const condition_names[16] = {
		"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"
	};

	struct decoder
	{
		const machine_state& s;
		uint16_t segment;
		uint16_t offset;
		uint16_t start;
		const char* segment_override = nullptr;

		uint8_t byte()
		{
			const uint32_t address = ((uint32_t(segment) << 4) + offset) & 0xfffff;
			++offset;
			return s.memory[address];
		}

		uint16_t word()
		{
			const uint16_t low = byte();
			return uint16_t(low | (byte() << 8));
		}

		void modrm(char* rm, int rm_size, char* reg, int reg_size, bool word_size)
		{
			const uint8_t m = byte();
			const int mode = (m >> 6) & 3;
			const int which = (m >> 3) & 7;
			const int base = m & 7;

			if (reg != nullptr)
				snprintf(reg, size_t(reg_size), "%s", word_size ? reg16_names[which] : reg8_names[which]);

			if (mode == 3)
			{
				snprintf(rm, size_t(rm_size), "%s", word_size ? reg16_names[base] : reg8_names[base]);
				return;
			}

			char displacement[16] = {};
			if (mode == 0 && base == 6)
			{
				snprintf(rm, size_t(rm_size), "%s[%04Xh]", segment_override ? segment_override : "", word());
				return;
			}
			if (mode == 1)
			{
				const int8_t d = int8_t(byte());
				snprintf(displacement, sizeof(displacement), "%c%02Xh", d < 0 ? '-' : '+', d < 0 ? -d : d);
			}
			else if (mode == 2)
			{
				snprintf(displacement, sizeof(displacement), "+%04Xh", word());
			}
			snprintf(rm, size_t(rm_size), "%s[%s%s]", segment_override ? segment_override : "",
			         memory_forms[base], displacement);
		}

		int modrm_op(uint16_t at) const
		{
			const uint32_t address = ((uint32_t(segment) << 4) + at) & 0xfffff;
			return (s.memory[address] >> 3) & 7;
		}
	};
}

disassembly disassemble(const machine_state& s, uint16_t segment, uint16_t offset)
{
	disassembly result;
	decoder d{s, segment, offset, offset};

	char rm[40] = {};
	char reg[8] = {};
	char text[64] = {};

	for (;;)
	{
		const uint8_t opcode = d.byte();
		const bool word_size = (opcode & 1) != 0;
		const bool direction = (opcode & 2) != 0;

		if (opcode == 0x26 || opcode == 0x2e || opcode == 0x36 || opcode == 0x3e)
		{
			static const char* const prefixes[4] = {"es:", "cs:", "ss:", "ds:"};
			d.segment_override = prefixes[(opcode >> 3) & 3];
			continue;
		}
		if (opcode == 0xf2 || opcode == 0xf3 || opcode == 0xf0)
		{
			const char* name = opcode == 0xf0 ? "lock " : (opcode == 0xf2 ? "repne " : "rep ");
			const disassembly rest = disassemble(s, segment, d.offset);
			snprintf(result.text, sizeof(result.text), "%s%s", name, rest.text);
			result.length = uint16_t(d.offset - offset + rest.length);
			for (int i = 0; i < result.length && i < 15; ++i)
				snprintf(result.bytes + i * 2, sizeof(result.bytes) - size_t(i) * 2, "%02X",
				         s.memory[(((uint32_t(segment) << 4) + offset + i) & 0xfffff)]);
			return result;
		}

		switch (opcode)
		{
		case 0x00: case 0x01: case 0x02: case 0x03:
		case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x10: case 0x11: case 0x12: case 0x13:
		case 0x18: case 0x19: case 0x1a: case 0x1b:
		case 0x20: case 0x21: case 0x22: case 0x23:
		case 0x28: case 0x29: case 0x2a: case 0x2b:
		case 0x30: case 0x31: case 0x32: case 0x33:
		case 0x38: case 0x39: case 0x3a: case 0x3b:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), word_size);
			snprintf(text, sizeof(text), "%s %s,%s", alu_names[(opcode >> 3) & 7],
			         direction ? reg : rm, direction ? rm : reg);
			break;

		case 0x04: case 0x05: case 0x0c: case 0x0d:
		case 0x14: case 0x15: case 0x1c: case 0x1d:
		case 0x24: case 0x25: case 0x2c: case 0x2d:
		case 0x34: case 0x35: case 0x3c: case 0x3d:
			snprintf(text, sizeof(text), "%s %s,%04Xh", alu_names[(opcode >> 3) & 7],
			         word_size ? "ax" : "al", word_size ? d.word() : d.byte());
			break;

		case 0x06: case 0x0e: case 0x16: case 0x1e:
			snprintf(text, sizeof(text), "push %s", segment_names[(opcode >> 3) & 3]);
			break;
		case 0x07: case 0x17: case 0x1f:
			snprintf(text, sizeof(text), "pop %s", segment_names[(opcode >> 3) & 3]);
			break;

		case 0x27: snprintf(text, sizeof(text), "daa"); break;
		case 0x2f: snprintf(text, sizeof(text), "das"); break;
		case 0x37: snprintf(text, sizeof(text), "aaa"); break;
		case 0x3f: snprintf(text, sizeof(text), "aas"); break;

		case 0x40: case 0x41: case 0x42: case 0x43:
		case 0x44: case 0x45: case 0x46: case 0x47:
			snprintf(text, sizeof(text), "inc %s", reg16_names[opcode & 7]);
			break;
		case 0x48: case 0x49: case 0x4a: case 0x4b:
		case 0x4c: case 0x4d: case 0x4e: case 0x4f:
			snprintf(text, sizeof(text), "dec %s", reg16_names[opcode & 7]);
			break;
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
			snprintf(text, sizeof(text), "push %s", reg16_names[opcode & 7]);
			break;
		case 0x58: case 0x59: case 0x5a: case 0x5b:
		case 0x5c: case 0x5d: case 0x5e: case 0x5f:
			snprintf(text, sizeof(text), "pop %s", reg16_names[opcode & 7]);
			break;

		case 0x60: snprintf(text, sizeof(text), "pusha"); break;
		case 0x61: snprintf(text, sizeof(text), "popa"); break;
		case 0x68: snprintf(text, sizeof(text), "push %04Xh", d.word()); break;
		case 0x6a: snprintf(text, sizeof(text), "push %02Xh", d.byte()); break;

		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b:
		case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		{
			const int8_t rel = int8_t(d.byte());
			snprintf(text, sizeof(text), "j%s %04Xh", condition_names[opcode & 0x0f], uint16_t(d.offset + rel));
			break;
		}

		case 0x80: case 0x81: case 0x82: case 0x83:
		{
			const int operation = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			const uint16_t immediate = opcode == 0x81 ? d.word() : d.byte();
			snprintf(text, sizeof(text), "%s %s,%04Xh", alu_names[operation], rm, immediate);
			break;
		}

		case 0x84: case 0x85:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), word_size);
			snprintf(text, sizeof(text), "test %s,%s", rm, reg);
			break;
		case 0x86: case 0x87:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), word_size);
			snprintf(text, sizeof(text), "xchg %s,%s", rm, reg);
			break;
		case 0x88: case 0x89: case 0x8a: case 0x8b:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), word_size);
			snprintf(text, sizeof(text), "mov %s,%s", direction ? reg : rm, direction ? rm : reg);
			break;
		case 0x8c: case 0x8e:
		{
			const int which = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, true);
			if (opcode == 0x8c) snprintf(text, sizeof(text), "mov %s,%s", rm, segment_names[which]);
			else snprintf(text, sizeof(text), "mov %s,%s", segment_names[which], rm);
			break;
		}
		case 0x8d:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), true);
			snprintf(text, sizeof(text), "lea %s,%s", reg, rm);
			break;
		case 0x8f:
			d.modrm(rm, sizeof(rm), nullptr, 0, true);
			snprintf(text, sizeof(text), "pop %s", rm);
			break;

		case 0x90: snprintf(text, sizeof(text), "nop"); break;
		case 0x91: case 0x92: case 0x93: case 0x94:
		case 0x95: case 0x96: case 0x97:
			snprintf(text, sizeof(text), "xchg ax,%s", reg16_names[opcode & 7]);
			break;
		case 0x98: snprintf(text, sizeof(text), "cbw"); break;
		case 0x99: snprintf(text, sizeof(text), "cwd"); break;
		case 0x9a:
		{
			const uint16_t target = d.word();
			snprintf(text, sizeof(text), "call %04X:%04X", d.word(), target);
			break;
		}
		case 0x9c: snprintf(text, sizeof(text), "pushf"); break;
		case 0x9d: snprintf(text, sizeof(text), "popf"); break;
		case 0x9e: snprintf(text, sizeof(text), "sahf"); break;
		case 0x9f: snprintf(text, sizeof(text), "lahf"); break;

		case 0xa0: case 0xa1:
			snprintf(text, sizeof(text), "mov %s,[%04Xh]", word_size ? "ax" : "al", d.word());
			break;
		case 0xa2: case 0xa3:
			snprintf(text, sizeof(text), "mov [%04Xh],%s", d.word(), word_size ? "ax" : "al");
			break;
		case 0xa4: case 0xa5: snprintf(text, sizeof(text), "movs%c", word_size ? 'w' : 'b'); break;
		case 0xa6: case 0xa7: snprintf(text, sizeof(text), "cmps%c", word_size ? 'w' : 'b'); break;
		case 0xa8: case 0xa9:
			snprintf(text, sizeof(text), "test %s,%04Xh", word_size ? "ax" : "al",
			         word_size ? d.word() : d.byte());
			break;
		case 0xaa: case 0xab: snprintf(text, sizeof(text), "stos%c", word_size ? 'w' : 'b'); break;
		case 0xac: case 0xad: snprintf(text, sizeof(text), "lods%c", word_size ? 'w' : 'b'); break;
		case 0xae: case 0xaf: snprintf(text, sizeof(text), "scas%c", word_size ? 'w' : 'b'); break;

		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			snprintf(text, sizeof(text), "mov %s,%02Xh", reg8_names[opcode & 7], d.byte());
			break;
		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			snprintf(text, sizeof(text), "mov %s,%04Xh", reg16_names[opcode & 7], d.word());
			break;

		case 0xc0: case 0xc1:
		{
			const int operation = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			snprintf(text, sizeof(text), "%s %s,%02Xh", shift_names[operation], rm, d.byte());
			break;
		}
		case 0xc2: snprintf(text, sizeof(text), "ret %04Xh", d.word()); break;
		case 0xc3: snprintf(text, sizeof(text), "ret"); break;
		case 0xc4: case 0xc5:
			d.modrm(rm, sizeof(rm), reg, sizeof(reg), true);
			snprintf(text, sizeof(text), "%s %s,%s", opcode == 0xc4 ? "les" : "lds", reg, rm);
			break;
		case 0xc6: case 0xc7:
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			snprintf(text, sizeof(text), "mov %s,%04Xh", rm, word_size ? d.word() : d.byte());
			break;
		case 0xc8: snprintf(text, sizeof(text), "enter %04Xh,%02Xh", d.word(), d.byte()); break;
		case 0xc9: snprintf(text, sizeof(text), "leave"); break;
		case 0xca: snprintf(text, sizeof(text), "retf %04Xh", d.word()); break;
		case 0xcb: snprintf(text, sizeof(text), "retf"); break;
		case 0xcc: snprintf(text, sizeof(text), "int 3"); break;
		case 0xcd: snprintf(text, sizeof(text), "int %02Xh", d.byte()); break;
		case 0xce: snprintf(text, sizeof(text), "into"); break;
		case 0xcf: snprintf(text, sizeof(text), "iret"); break;

		case 0xd0: case 0xd1: case 0xd2: case 0xd3:
		{
			const int operation = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			snprintf(text, sizeof(text), "%s %s,%s", shift_names[operation], rm,
			         (opcode & 2) == 0 ? "1" : "cl");
			break;
		}
		case 0xd4: snprintf(text, sizeof(text), "aam %02Xh", d.byte()); break;
		case 0xd5: snprintf(text, sizeof(text), "aad %02Xh", d.byte()); break;
		case 0xd7: snprintf(text, sizeof(text), "xlat"); break;

		case 0xe0: case 0xe1: case 0xe2: case 0xe3:
		{
			static const char* const names[4] = {"loopne", "loope", "loop", "jcxz"};
			const int8_t rel = int8_t(d.byte());
			snprintf(text, sizeof(text), "%s %04Xh", names[opcode & 3], uint16_t(d.offset + rel));
			break;
		}
		case 0xe4: case 0xe5:
			snprintf(text, sizeof(text), "in %s,%02Xh", word_size ? "ax" : "al", d.byte());
			break;
		case 0xe6: case 0xe7:
			snprintf(text, sizeof(text), "out %02Xh,%s", d.byte(), word_size ? "ax" : "al");
			break;
		case 0xe8:
		{
			const int16_t rel = int16_t(d.word());
			snprintf(text, sizeof(text), "call %04Xh", uint16_t(d.offset + rel));
			break;
		}
		case 0xe9:
		{
			const int16_t rel = int16_t(d.word());
			snprintf(text, sizeof(text), "jmp %04Xh", uint16_t(d.offset + rel));
			break;
		}
		case 0xea:
		{
			const uint16_t target = d.word();
			snprintf(text, sizeof(text), "jmp %04X:%04X", d.word(), target);
			break;
		}
		case 0xeb:
		{
			const int8_t rel = int8_t(d.byte());
			snprintf(text, sizeof(text), "jmp %04Xh", uint16_t(d.offset + rel));
			break;
		}
		case 0xec: case 0xed: snprintf(text, sizeof(text), "in %s,dx", word_size ? "ax" : "al"); break;
		case 0xee: case 0xef: snprintf(text, sizeof(text), "out dx,%s", word_size ? "ax" : "al"); break;
		case 0xf4: snprintf(text, sizeof(text), "hlt"); break;
		case 0xf5: snprintf(text, sizeof(text), "cmc"); break;

		case 0xf6: case 0xf7:
		{
			static const char* const names[8] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
			const int operation = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			if (operation < 2)
				snprintf(text, sizeof(text), "test %s,%04Xh", rm, word_size ? d.word() : d.byte());
			else
				snprintf(text, sizeof(text), "%s %s", names[operation], rm);
			break;
		}

		case 0xf8: snprintf(text, sizeof(text), "clc"); break;
		case 0xf9: snprintf(text, sizeof(text), "stc"); break;
		case 0xfa: snprintf(text, sizeof(text), "cli"); break;
		case 0xfb: snprintf(text, sizeof(text), "sti"); break;
		case 0xfc: snprintf(text, sizeof(text), "cld"); break;
		case 0xfd: snprintf(text, sizeof(text), "std"); break;

		case 0xfe: case 0xff:
		{
			static const char* const names[8] = {"inc", "dec", "call", "call far", "jmp", "jmp far", "push", "?"};
			const int operation = d.modrm_op(d.offset);
			d.modrm(rm, sizeof(rm), nullptr, 0, word_size);
			snprintf(text, sizeof(text), "%s %s", names[operation], rm);
			break;
		}

		default:
			snprintf(text, sizeof(text), "db %02Xh", opcode);
			break;
		}
		break;
	}

	result.length = uint16_t(d.offset - offset);
	if (result.length == 0) result.length = 1;
	snprintf(result.text, sizeof(result.text), "%s", text);
	for (int i = 0; i < result.length && i < 15; ++i)
		snprintf(result.bytes + i * 2, sizeof(result.bytes) - size_t(i) * 2, "%02X",
		         s.memory[(((uint32_t(segment) << 4) + offset + i) & 0xfffff)]);
	return result;
}

void debug_capture(const machine_state& s, debug_state& out)
{
	const cpu_state& c = s.cpu;
	out.ax = c.ax();
	out.bx = c.bx();
	out.cx = c.cx();
	out.dx = c.dx();
	out.si = c.si();
	out.di = c.di();
	out.bp = c.bp();
	out.sp = c.sp();
	out.cs = c.cs();
	out.ds = c.ds();
	out.es = c.es();
	out.ss = c.ss();
	out.ip = c.ip;
	out.flags = c.flags;
	out.cycles = s.cycles;

	out.video_mode = s.vga.mode;
	out.video_width = s.vga.width;
	out.video_height = s.vga.height;

	out.psp_segment = s.dos.psp_segment;
	out.dta = s.dos.dta;
	out.open_files = 0;
	for (const dos_file& file : s.dos.files) if (file.used && file.device == 0) ++out.open_files;

	out.free_paragraphs = 0;
	out.largest_block = 0;
	uint16_t mcb = s.dos.first_mcb;
	for (int guard = 0; guard < 256 && mcb != 0; ++guard)
	{
		const uint32_t base = uint32_t(mcb) << 4;
		if (base + 16 >= memory_size) break;
		const uint8_t type = s.memory[base];
		const uint16_t owner = uint16_t(s.memory[base + 1] | (s.memory[base + 2] << 8));
		const uint16_t size = uint16_t(s.memory[base + 3] | (s.memory[base + 4] << 8));
		if (owner == 0)
		{
			out.free_paragraphs = uint16_t(out.free_paragraphs + size);
			if (size > out.largest_block) out.largest_block = size;
		}
		if (type != 'M') break;
		mcb = uint16_t(mcb + size + 1);
	}

	out.timer_ticks = s.bios.ticks;
	out.timer_reload = s.pit.channel[0].reload;
	out.adlib_active = s.sound.opl.enabled;

	uint16_t offset = c.ip;
	for (auto& line : out.code)
	{
		line = disassemble(s, c.cs(), offset);
		offset = uint16_t(offset + line.length);
	}
}

int debug_format(const debug_state& state, char* out, int out_size)
{
	int written = snprintf(out, size_t(out_size),
	                       "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n"
	                       "DS=%04X ES=%04X SS=%04X CS=%04X IP=%04X FL=%04X [%c%c%c%c%c%c%c%c]\n"
	                       "cycles=%llu mode=%02X %dx%d psp=%04X dta=%04X:%04X free=%uK largest=%uK\n"
	                       "files=%d ticks=%u timer=%u adlib=%s\n",
	                       state.ax, state.bx, state.cx, state.dx, state.si, state.di, state.bp, state.sp,
	                       state.ds, state.es, state.ss, state.cs, state.ip, state.flags,
	                       (state.flags & flag_of) ? 'O' : '-', (state.flags & flag_df) ? 'D' : '-',
	                       (state.flags & flag_if) ? 'I' : '-', (state.flags & flag_sf) ? 'S' : '-',
	                       (state.flags & flag_zf) ? 'Z' : '-', (state.flags & flag_af) ? 'A' : '-',
	                       (state.flags & flag_pf) ? 'P' : '-', (state.flags & flag_cf) ? 'C' : '-',
	                       static_cast<unsigned long long>(state.cycles), state.video_mode,
	                       state.video_width, state.video_height, state.psp_segment,
	                       uint16_t(state.dta >> 16), uint16_t(state.dta),
	                       unsigned(state.free_paragraphs) / 64, unsigned(state.largest_block) / 64,
	                       state.open_files, state.timer_ticks, state.timer_reload,
	                       state.adlib_active ? "yes" : "no");

	uint16_t offset = state.ip;
	for (const disassembly& line : state.code)
	{
		if (written >= out_size - 1) break;
		written += snprintf(out + written, size_t(out_size - written), "%04X:%04X  %-12s %s\n",
		                    state.cs, offset, line.bytes, line.text);
		offset = uint16_t(offset + line.length);
	}
	return written;
}
