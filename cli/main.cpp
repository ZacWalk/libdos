// libdos by Zac Walker
//
// Command line driver: runs the behavioural test suite, and inspects,
// disassembles and headlessly runs DOS executables.

#include "cli.h"

#include "../src/machine.h"
#include "../src/debug.h"
#include "../host/host-win32.h"
#include "../cc/cc.h"
#include "../cc/support.h"
#include "../disk/disk-image.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
	int checks = 0;
	int failures = 0;

	LARGE_INTEGER now()
	{
		LARGE_INTEGER at;
		QueryPerformanceCounter(&at);
		return at;
	}

	double milliseconds_since(LARGE_INTEGER started)
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return double(now().QuadPart - started.QuadPart) * 1000.0 / double(frequency.QuadPart);
	}

	void check(bool condition, const char* name)
	{
		++checks;
		if (!condition)
		{
			++failures;
			printf("FAIL  %s\n", name);
		}
	}

	void check_equal(uint32_t actual, uint32_t expected, const char* name)
	{
		++checks;
		if (actual != expected)
		{
			++failures;
			printf("FAIL  %s: got %04X, expected %04X\n", name, actual, expected);
		}
	}

	bool read_file(const char* path, uint8_t*& data, uint32_t& size)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || file == nullptr) return false;
		fseek(file, 0, SEEK_END);
		const long length = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (length <= 0)
		{
			fclose(file);
			return false;
		}
		data = new uint8_t[size_t(length)];
		size = uint32_t(fread(data, 1, size_t(length), file));
		fclose(file);
		return size > 0;
	}

	void split_path(const char* path, char* directory, int directory_size, char* name, int name_size)
	{
		const char* last = path;
		for (const char* p = path; *p != 0; ++p) if (*p == '\\' || *p == '/') last = p + 1;
		const int length = int(last - path);
		if (length == 0) snprintf(directory, size_t(directory_size), ".");
		else snprintf(directory, size_t(directory_size), "%.*s", length - 1 > 0 ? length - 1 : 1, path);
		snprintf(name, size_t(name_size), "%s", last);
	}

	// Writes the current frame as a 24-bit BMP, which is the only practical way
	// to check a graphics mode from a script.
	bool write_bmp(const char* path, const uint32_t* pixels, int width, int height)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || file == nullptr) return false;

		const int row_bytes = (width * 3 + 3) & ~3;
		const uint32_t image_size = uint32_t(row_bytes * height);
		const uint32_t file_size = 54 + image_size;
		uint8_t header[54] = {};
		header[0] = 'B';
		header[1] = 'M';
		memcpy(header + 2, &file_size, 4);
		const uint32_t offset = 54;
		memcpy(header + 10, &offset, 4);
		const uint32_t info_size = 40;
		memcpy(header + 14, &info_size, 4);
		memcpy(header + 18, &width, 4);
		memcpy(header + 22, &height, 4);
		header[26] = 1;
		header[28] = 24;
		memcpy(header + 34, &image_size, 4);
		fwrite(header, 1, sizeof(header), file);

		uint8_t* row = new uint8_t[size_t(row_bytes)]();
		for (int y = height - 1; y >= 0; --y)
		{
			for (int x = 0; x < width; ++x)
			{
				const uint32_t pixel = pixels[y * width + x];
				row[x * 3 + 0] = uint8_t(pixel);
				row[x * 3 + 1] = uint8_t(pixel >> 8);
				row[x * 3 + 2] = uint8_t(pixel >> 16);
			}
			fwrite(row, 1, size_t(row_bytes), file);
		}
		delete[] row;
		fclose(file);
		return true;
	}

	bool write_wav(const char* path, const int16_t* samples, uint32_t count)	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || file == nullptr) return false;

		const uint32_t data_size = count * 2;
		const uint32_t file_size = 36 + data_size;
		const uint32_t format_size = 16;
		const uint16_t pcm = 1, channels = 1, bits = 16, align = 2;
		const uint32_t rate = sound_sample_rate;
		const uint32_t byte_rate = rate * align;
		fwrite("RIFF", 1, 4, file);
		fwrite(&file_size, 4, 1, file);
		fwrite("WAVEfmt ", 1, 8, file);
		fwrite(&format_size, 4, 1, file);
		fwrite(&pcm, 2, 1, file);
		fwrite(&channels, 2, 1, file);
		fwrite(&rate, 4, 1, file);
		fwrite(&byte_rate, 4, 1, file);
		fwrite(&align, 2, 1, file);
		fwrite(&bits, 2, 1, file);
		fwrite("data", 1, 4, file);
		fwrite(&data_size, 4, 1, file);
		fwrite(samples, 2, count, file);
		fclose(file);
		return true;
	}

	// Types one character on the emulated keyboard. Going through machine_key
	// rather than the BIOS buffer means a program reading raw scancodes sees
	// what it expects.
	machine_key key_from_char(char c)
	{
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
		const char* const rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
		const machine_key first[3] = {machine_key::q, machine_key::a, machine_key::z};
		for (int row = 0; row < 3; ++row)
		{
			const char* at = strchr(rows[row], c);
			if (at != nullptr) return machine_key(int(first[row]) + int(at - rows[row]));
		}
		static const char digit_row[] = "1234567890";
		const char* digits = strchr(digit_row, c);
		if (digits != nullptr && c != 0) return machine_key(int(machine_key::num1) + int(digits - digit_row));
		switch (c)
		{
		case ' ': return machine_key::space;
		case '\n': case '\r': return machine_key::enter;
		case 27: return machine_key::escape;
		default: return machine_key::none;
		}
	}

	// Loads a block of machine code at 2000:0000 and points the CPU at it.
	void load_code(machine& m, const uint8_t* code, uint32_t length)
	{
		machine_state& s = m.state();
		for (uint32_t i = 0; i < length; ++i) s.memory[0x20000 + i] = code[i];
		s.cpu.set_cs(0x2000);
		s.cpu.ip = 0;
		s.cpu.set_ds(0x2000);
		s.cpu.set_es(0x2000);
		s.cpu.set_ss(0x3000);
		s.cpu.set_sp(0xfffe);
		s.cpu.flags = 0x0202;
		s.cpu.halted = false;
	}

	void step(machine& m, int count)
	{
		for (int i = 0; i < count; ++i) cpu_step(m.state());
	}

	void test_cpu()
	{
		machine m;

		{
			// mov ax,1234h / mov bx,1111h / add ax,bx
			const uint8_t code[] = {0xb8, 0x34, 0x12, 0xbb, 0x11, 0x11, 0x01, 0xd8};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0x2345, "add ax,bx");
		}

		{
			// mov al,0FFh / add al,1  -> zero, carry, aux carry
			const uint8_t code[] = {0xb0, 0xff, 0x04, 0x01};
			load_code(m, code, sizeof(code));
			step(m, 2);
			const cpu_state& c = m.state().cpu;
			check_equal(c.al(), 0x00, "add al,1 result");
			check(c.cf() && c.zf() && (c.flags & flag_af) != 0, "add al,1 flags");
		}

		{
			// mov ax,8000h / sub ax,1 -> overflow set, sign clear
			const uint8_t code[] = {0xb8, 0x00, 0x80, 0x2d, 0x01, 0x00};
			load_code(m, code, sizeof(code));
			step(m, 2);
			const cpu_state& c = m.state().cpu;
			check_equal(c.ax(), 0x7fff, "sub ax,1 result");
			check(c.of() && !c.sf(), "sub ax,1 overflow");
		}

		{
			// mov ax,0FFFFh / mov bx,2 / mul bx -> dx:ax = 0001:FFFE
			const uint8_t code[] = {0xb8, 0xff, 0xff, 0xbb, 0x02, 0x00, 0xf7, 0xe3};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0xfffe, "mul low word");
			check_equal(m.state().cpu.dx(), 0x0001, "mul high word");
		}

		{
			// mov ax,000Ah / cwd / mov bx,3 / idiv bx -> ax = 3, dx = 1
			const uint8_t code[] = {0xb8, 0x0a, 0x00, 0x99, 0xbb, 0x03, 0x00, 0xf7, 0xfb};
			load_code(m, code, sizeof(code));
			step(m, 4);
			check_equal(m.state().cpu.ax(), 3, "idiv quotient");
			check_equal(m.state().cpu.dx(), 1, "idiv remainder");
		}

		{
			// mov cx,4 / mov al,55h / rep stosb, then check memory
			const uint8_t code[] = {0xb9, 0x04, 0x00, 0xb0, 0x55, 0xbf, 0x00, 0x10, 0xfc, 0xf3, 0xaa};
			load_code(m, code, sizeof(code));
			step(m, 9);
			machine_state& s = m.state();
			check_equal(s.memory[0x21000], 0x55, "rep stosb first byte");
			check_equal(s.memory[0x21003], 0x55, "rep stosb last byte");
			check_equal(s.memory[0x21004], 0x00, "rep stosb stopped");
			check_equal(s.cpu.cx(), 0, "rep stosb count");
		}

		{
			// pusha / popa round trip (80186)
			const uint8_t code[] = {0xb8, 0x11, 0x11, 0x60, 0xb8, 0x22, 0x22, 0x61};
			load_code(m, code, sizeof(code));
			step(m, 4);
			check_equal(m.state().cpu.ax(), 0x1111, "pusha/popa restores ax");
		}

		{
			// mov ax,4 / shl ax,3 (80186 immediate form)
			const uint8_t code[] = {0xb8, 0x04, 0x00, 0xc1, 0xe0, 0x03};
			load_code(m, code, sizeof(code));
			step(m, 2);
			check_equal(m.state().cpu.ax(), 32, "shl ax,imm8");
		}

		{
			// mov ax,0FFFFh / sar ax,1 keeps the sign
			const uint8_t code[] = {0xb8, 0xff, 0xff, 0xd1, 0xf8};
			load_code(m, code, sizeof(code));
			step(m, 2);
			check_equal(m.state().cpu.ax(), 0xffff, "sar sign extension");
		}

		{
			// call/ret through the stack
			const uint8_t code[] = {0xe8, 0x03, 0x00, 0xb8, 0x01, 0x00, 0xb8, 0x02, 0x00, 0xc3};
			load_code(m, code, sizeof(code));
			step(m, 3); // call, mov ax,2, ret
			check_equal(m.state().cpu.ip, 3, "ret returns to the caller");
			check_equal(m.state().cpu.ax(), 2, "call reached the target");
		}

		{
			// les bx,[si] loads both halves
			machine_state& s = m.state();
			const uint8_t code[] = {0xbe, 0x00, 0x20, 0xc4, 0x1c};
			load_code(m, code, sizeof(code));
			s.memory[0x22000] = 0x34;
			s.memory[0x22001] = 0x12;
			s.memory[0x22002] = 0x00;
			s.memory[0x22003] = 0xb8;
			step(m, 2);
			check_equal(s.cpu.bx(), 0x1234, "les offset");
			check_equal(s.cpu.es(), 0xb800, "les segment");
		}
	}

	// The 80386 instructions a 16-bit program built with -3 actually contains.
	void test_cpu_386()
	{
		machine m;

		{
			// xor ax,ax / je near +3 / mov ax,2222h / mov ax,1111h
			const uint8_t code[] = {
				0x31, 0xc0, 0x0f, 0x84, 0x03, 0x00, 0xb8, 0x22, 0x22, 0xb8, 0x11, 0x11
			};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0x1111, "near jcc taken");
		}

		{
			// mov ax,5 / cmp ax,5 / sete bl / setne bh
			const uint8_t code[] = {
				0xb8, 0x05, 0x00, 0x3d, 0x05, 0x00, 0x0f, 0x94, 0xc3, 0x0f, 0x95, 0xc7
			};
			load_code(m, code, sizeof(code));
			step(m, 4);
			check_equal(m.state().cpu.bl(), 1, "sete");
			check_equal(m.state().cpu.bh(), 0, "setne");
		}

		{
			// mov bl,0FFh / movzx ax,bl / movsx cx,bl
			const uint8_t code[] = {0xb3, 0xff, 0x0f, 0xb6, 0xc3, 0x0f, 0xbe, 0xcb};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0x00ff, "movzx zero extends");
			check_equal(m.state().cpu.cx(), 0xffff, "movsx sign extends");
		}

		{
			// mov ax,0 / bts ax,5 / bt ax,5
			const uint8_t code[] = {
				0xb8, 0x00, 0x00, 0x0f, 0xba, 0xe8, 0x05, 0x0f, 0xba, 0xe0, 0x05
			};
			load_code(m, code, sizeof(code));
			step(m, 2);
			check_equal(m.state().cpu.ax(), 0x0020, "bts sets the bit");
			check(!m.state().cpu.cf(), "bts reports the old bit");
			step(m, 1);
			check(m.state().cpu.cf(), "bt reads the bit back");
		}

		{
			// mov ax,0100h / bsf bx,ax / bsr cx,ax
			const uint8_t code[] = {0xb8, 0x00, 0x01, 0x0f, 0xbc, 0xd8, 0x0f, 0xbd, 0xc8};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.bx(), 8, "bsf finds the low bit");
			check_equal(m.state().cpu.cx(), 8, "bsr finds the high bit");
		}

		{
			// mov ax,1234h / mov bx,5678h / shld ax,bx,4
			const uint8_t code[] = {
				0xb8, 0x34, 0x12, 0xbb, 0x78, 0x56, 0x0f, 0xa4, 0xd8, 0x04
			};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0x2345, "shld");
		}

		{
			// mov ax,1234h / mov bx,5678h / shrd ax,bx,4
			const uint8_t code[] = {
				0xb8, 0x34, 0x12, 0xbb, 0x78, 0x56, 0x0f, 0xac, 0xd8, 0x04
			};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0x8123, "shrd");
		}

		{
			// mov ax,5 / mov bx,-3 / imul ax,bx
			const uint8_t code[] = {0xb8, 0x05, 0x00, 0xbb, 0xfd, 0xff, 0x0f, 0xaf, 0xc3};
			load_code(m, code, sizeof(code));
			step(m, 3);
			check_equal(m.state().cpu.ax(), 0xfff1, "imul r16,rm16");
		}

		{
			// mov si,2000h / lfs bx,[si], then read through the fs override
			machine_state& s = m.state();
			const uint8_t code[] = {0xbe, 0x00, 0x20, 0x0f, 0xb4, 0x1c, 0x64, 0xa0, 0x10, 0x00};
			load_code(m, code, sizeof(code));
			s.memory[0x22000] = 0x34;
			s.memory[0x22001] = 0x12;
			s.memory[0x22002] = 0x00;
			s.memory[0x22003] = 0x30;
			s.memory[0x30010] = 0x5a;
			step(m, 4); // the fs prefix is a step of its own
			check_equal(s.cpu.bx(), 0x1234, "lfs offset");
			check_equal(s.cpu.fs(), 0x3000, "lfs segment");
			check_equal(s.cpu.al(), 0x5a, "fs segment override");
		}

		{
			// mov ax,4321h / mov fs,ax / push fs / pop gs
			const uint8_t code[] = {
				0xb8, 0x21, 0x43, 0x8e, 0xe0, 0x0f, 0xa0, 0x0f, 0xa9
			};
			load_code(m, code, sizeof(code));
			step(m, 4);
			check_equal(m.state().cpu.fs(), 0x4321, "mov fs,ax");
			check_equal(m.state().cpu.gs(), 0x4321, "push fs / pop gs");
		}
	}

	void test_interrupts()
	{
		machine m;
		machine_state& s = m.state();

		// int 10h ah=0Fh reports the mode the BIOS set at reset.
		const uint8_t code[] = {0xb4, 0x0f, 0xcd, 0x10};
		load_code(m, code, sizeof(code));
		for (int i = 0; i < 8; ++i) cpu_step(s);
		check_equal(s.cpu.al(), 3, "int 10h reports mode 3");
		check_equal(s.cpu.ah(), 80, "int 10h reports 80 columns");

		// A program can take a vector over and see its own handler run.
		mem_write16(s, 0x21 * 4, 0x0000);
		mem_write16(s, 0x21 * 4 + 2, 0x4000);
		s.memory[0x40000] = 0xb8; // mov ax,0BEEFh
		s.memory[0x40001] = 0xef;
		s.memory[0x40002] = 0xbe;
		s.memory[0x40003] = 0xcf; // iret
		const uint8_t hook[] = {0xcd, 0x21};
		load_code(m, hook, sizeof(hook));
		for (int i = 0; i < 3; ++i) cpu_step(s);
		check_equal(s.cpu.ax(), 0xbeef, "a hooked vector runs the program's handler");
	}

	void test_timer()
	{
		machine m;
		machine_state& s = m.state();

		// Program channel 0 for a fast tick and check IRQ 0 arrives.
		io_write8(s, 0x43, 0x36);
		io_write8(s, 0x40, 0xe8);
		io_write8(s, 0x40, 0x03); // 1000 ticks
		pit_step(s, s.cpu_hz / 1000);
		check((s.pic.master.irr & 0x01) != 0, "timer raises IRQ 0");

		const uint32_t before = mem_read16(s, 0x46c);
		s.cpu.flags |= flag_if;
		for (int i = 0; i < 64; ++i) cpu_step(s);
		check(mem_read16(s, 0x46c) != before, "the BIOS tick count advances");
	}

	void test_video()
	{
		machine m;
		machine_state& s = m.state();

		vga_set_mode(s, 0x13);
		check_equal(uint32_t(s.vga.width), 320, "mode 13h width");
		check_equal(uint32_t(s.vga.height), 200, "mode 13h height");
		mem_write8(s, 0xa0000 + 100, 42);
		check_equal(mem_read8(s, 0xa0000 + 100), 42, "mode 13h memory is linear");
		s.vga.dac[42][0] = 63;
		s.vga.dac[42][1] = 0;
		s.vga.dac[42][2] = 0;
		vga_render(s);
		check_equal(s.vga.framebuffer[100], 0xff0000, "mode 13h renders through the DAC");

		vga_set_mode(s, 0x12);
		check_equal(uint32_t(s.vga.width), 640, "mode 12h width");
		// Write a pixel through the planar path: map mask 0x0F, bit mask 0x80.
		io_write8(s, 0x3c4, 0x02);
		io_write8(s, 0x3c5, 0x0f);
		io_write8(s, 0x3ce, 0x08);
		io_write8(s, 0x3cf, 0x80);
		io_write8(s, 0x3ce, 0x00);
		io_write8(s, 0x3cf, 0x0f);
		io_write8(s, 0x3ce, 0x01);
		io_write8(s, 0x3cf, 0x0f);
		mem_read8(s, 0xa0000); // load the latches
		mem_write8(s, 0xa0000, 0xff);
		check_equal(vga_get_pixel(s, 0, 0), 15, "planar set/reset writes all four planes");
		check_equal(vga_get_pixel(s, 1, 0), 0, "the bit mask limits the write to one pixel");

		vga_set_mode(s, 0x03);
		vga_write_char(s, 0, 'A', 0x07, true);
		check_equal(s.vga.mem[0], 'A', "text mode writes plane 0");
		check_equal(s.vga.mem[1], 0x07, "text mode writes plane 1");
		check_equal(s.vga.cursor_x[0], 1, "the cursor advances");
	}

	void test_dos_memory()
	{
		machine m;
		machine_state& s = m.state();

		uint16_t largest = 0;
		const uint16_t first = dos_alloc(s, 0x100, largest);
		const uint16_t second = dos_alloc(s, 0x100, largest);
		check(first != 0 && second != 0, "two blocks allocate");
		check(second > first + 0x100, "blocks do not overlap");
		check(dos_free(s, first), "a block frees");
		const uint16_t again = dos_alloc(s, 0x80, largest);
		check_equal(again, first, "a freed block is reused");
		check(dos_resize(s, second, 0x40, largest), "a block shrinks");

		char resolved[128] = {};
		dos_resolve_path(s, "GAME.EXE", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\GAME.EXE") == 0, "a bare name resolves against the root");
		dos_resolve_path(s, "sub\\..\\data\\file.dat", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\DATA\\FILE.DAT") == 0, "\"..\" collapses");
		dos_resolve_path(s, "A:\\X\\Y.Z", resolved, sizeof(resolved));
		check(strcmp(resolved, "A:\\X\\Y.Z") == 0, "an explicit drive is kept");

		// What CD and INT 21h AH=3Bh leave behind once a program is not at the root.
		snprintf(s.dos.cwd[2], sizeof(s.dos.cwd[2]), "%s", "\\GAMES");
		dos_resolve_path(s, "wl.exe", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\GAMES\\WL.EXE") == 0, "a bare name resolves against the current directory");
		dos_resolve_path(s, "*.*", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\GAMES\\*.*") == 0, "a wildcard resolves against the current directory");
		dos_resolve_path(s, "..", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\") == 0, "\"..\" leaves the current directory");
		dos_resolve_path(s, "\\X.DAT", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\X.DAT") == 0, "a rooted path ignores the current directory");
		snprintf(s.dos.cwd[2], sizeof(s.dos.cwd[2]), "%s", "\\");
		dos_resolve_path(s, "GAME.EXE", resolved, sizeof(resolved));
		check(strcmp(resolved, "C:\\GAME.EXE") == 0, "CD \\ leaves the root as the current directory");
		s.dos.cwd[2][0] = 0;
	}

	void test_loader()
	{
		machine m;

		// A minimal MZ with one relocation, so a correct load can be seen in
		// the segment value the program reads back.
		uint8_t image[64] = {};
		const auto set_word = [&](int offset, uint16_t value)
		{
			image[offset] = uint8_t(value);
			image[offset + 1] = uint8_t(value >> 8);
		};
		image[0] = 'M';
		image[1] = 'Z';
		set_word(0x02, 39); // bytes in last block
		set_word(0x04, 1); // blocks
		set_word(0x06, 1); // relocations
		set_word(0x08, 2); // header paragraphs
		set_word(0x0a, 0x10); // minimum extra paragraphs
		set_word(0x0c, 0xffff);
		set_word(0x0e, 0x0000); // ss
		set_word(0x10, 0x0100); // sp
		set_word(0x14, 0x0000); // ip
		set_word(0x16, 0x0000); // cs
		set_word(0x18, 0x001c); // relocation table
		set_word(0x1c, 0x0001); // relocation offset
		set_word(0x1e, 0x0000); // relocation segment
		image[0x20] = 0xb8; // mov ax,0000 (relocated)
		image[0x23] = 0xb4; // mov ah,4Ch
		image[0x24] = 0x4c;
		image[0x25] = 0xcd; // int 21h
		image[0x26] = 0x21;

		check(m.load_program(image, 39, "C:\\TEST.EXE", ""), "the MZ image loads");
		machine_state& s = m.state();
		const uint16_t load_segment = s.dos.image_segment;
		check_equal(s.cpu.cs(), load_segment, "CS points at the loaded image");
		check_equal(s.cpu.ds(), s.dos.psp_segment, "DS points at the PSP");
		check_equal(read8(s, s.dos.psp_segment, 0), 0xcd, "the PSP starts with INT 20h");
		cpu_step(s);
		check_equal(s.cpu.ax(), load_segment, "the relocation was applied");

		// The COM path: load at PSP:0100 with all four segments equal.
		const uint8_t com[] = {0xb8, 0x21, 0x43, 0xcd, 0x20};
		check(m.load_program(com, sizeof(com), "C:\\TEST.COM", ""), "the COM image loads");
		check_equal(m.state().cpu.ip, 0x100, "a COM starts at 0100");
		check_equal(m.state().cpu.cs(), m.state().cpu.ds(), "a COM has CS = DS");
	}

	void test_program_run()
	{
		machine m;

		// mov ah,9 / mov dx,010Ch / int 21h / mov ax,4C00h / int 21h / "HELLO$"
		const uint8_t com[] = {
			0xb4, 0x09, 0xba, 0x0c, 0x01, 0xcd, 0x21, 0xb8, 0x00, 0x4c, 0xcd, 0x21,
			'H', 'E', 'L', 'L', 'O', '$'
		};
		check(m.load_program(com, sizeof(com), "C:\\HELLO.COM", ""), "the test program loads");
		m.exec(200000);
		check(m.exited(), "the program terminated");
		check_equal(m.exit_code(), 0, "the exit code is zero");

		char screen[4096] = {};
		m.text_screen(screen, sizeof(screen));
		check(strstr(screen, "HELLO") != nullptr, "INT 21h AH=09 reached the screen");
	}

	// INT 21h AH=0Ah spans one call per key. A second read into the same buffer
	// starts empty rather than after the first, and Enter echoes only the
	// carriage return, as DOS does.
	void test_line_input()
	{
		// Reads a line into the buffer at 0123h twice, then writes the second with AH=40h.
		std::vector<uint8_t> com = {
			0xba, 0x23, 0x01, 0xb4, 0x0a, 0xcd, 0x21, 0xba, 0x23, 0x01, 0xb4, 0x0a, 0xcd, 0x21,
			0xb4, 0x40, 0xbb, 0x01, 0x00, 0x8a, 0x0e, 0x24, 0x01, 0xb5, 0x00, 0xba, 0x25, 0x01,
			0xcd, 0x21, 0xb8, 0x00, 0x4c, 0xcd, 0x21,
		};
		com.push_back(20); // capacity; the count and the text follow
		com.resize(com.size() + 22, 0);

		machine m;
		check(m.load_program(com.data(), uint32_t(com.size()), "C:\\READ.COM", ""), "the line input program loads");
		const machine_key typed[] = {
			machine_key::a, machine_key::b, machine_key::c, machine_key::enter,
			machine_key::x, machine_key::y, machine_key::enter,
		};
		for (const machine_key key : typed)
		{
			m.key_down(key);
			m.key_up(key);
			for (int frame = 0; frame < 2; ++frame) m.exec(m.cycles_per_frame());
		}
		for (int frame = 0; frame < 10 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		check(m.exited(), "both lines were read");

		char screen[4096] = {};
		m.text_screen(screen, sizeof(screen));
		check(strncmp(screen, "xyc", 3) == 0, "Enter echoes a carriage return and no line feed");
		check(strstr(screen, "abcxy") == nullptr, "a second read into the same buffer starts empty");
	}

	// DOS's character input hands over an extended key in two calls: 0, then
	// the scancode. A C program's getch() reads arrows and F-keys that way.
	void test_console_keys()
	{
		// Three reads with AH=08h, stored at 0200h, 0201h and 0202h.
		const uint8_t com[] = {
			0xb4, 0x08, 0xcd, 0x21, 0xa2, 0x00, 0x02,
			0xb4, 0x08, 0xcd, 0x21, 0xa2, 0x01, 0x02,
			0xb4, 0x08, 0xcd, 0x21, 0xa2, 0x02, 0x02,
			0xb8, 0x00, 0x4c, 0xcd, 0x21,
		};
		machine m;
		check(m.load_program(com, sizeof(com), "C:\\GETCH.COM", ""), "the getch program loads");
		const uint16_t psp = m.state().dos.psp_segment;
		for (const machine_key key : {machine_key::f1, machine_key::a})
		{
			m.key_down(key);
			m.key_up(key);
			for (int frame = 0; frame < 2; ++frame) m.exec(m.cycles_per_frame());
		}
		for (int frame = 0; frame < 4 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		check(m.exited(), "two keys answer three reads");
		check_equal(read8(m.state(), psp, 0x200), 0x00, "an extended key reads as 0 first");
		check_equal(read8(m.state(), psp, 0x201), 0x3b, "then as its scancode");
		check_equal(read8(m.state(), psp, 0x202), 'a', "and the next key follows it");
	}

	void test_sound()
	{
		machine m;
		machine_state& s = m.state();
		m.set_audio_enabled(true);

		// The AdLib detection sequence: start timer 1 and read the status.
		io_write8(s, 0x388, 0x04);
		io_write8(s, 0x389, 0x60);
		io_write8(s, 0x388, 0x04);
		io_write8(s, 0x389, 0x21);
		check_equal(io_read8(s, 0x388), 0xc0, "an AdLib answers the detection sequence");

		// A note through channel 0 has to produce something audible.
		const uint8_t writes[][2] = {
			{0x20, 0x01}, {0x23, 0x01}, {0x40, 0x10}, {0x43, 0x00},
			{0x60, 0xf0}, {0x63, 0xf0}, {0x80, 0x77}, {0x83, 0x77},
			{0xa0, 0x98}, {0xb0, 0x31}, {0xc0, 0x01},
		};
		for (const auto& write : writes)
		{
			io_write8(s, 0x388, write[0]);
			io_write8(s, 0x389, write[1]);
		}
		s.cycles += 100000;

		int16_t samples[735] = {};
		m.generate_audio(samples, 735);
		int peak = 0;
		for (const int16_t sample : samples) peak = sample > peak ? sample : peak;
		check(peak > 200, "the OPL produces output after a key-on");

		// The speaker gate has to be audible too.
		io_write8(s, 0x43, 0xb6);
		io_write8(s, 0x42, 0x00);
		io_write8(s, 0x42, 0x10);
		io_write8(s, 0x61, 0x03);
		s.cycles += 100000;
		m.generate_audio(samples, 735);
		peak = 0;
		for (const int16_t sample : samples) peak = sample > peak ? sample : peak;
		check(peak > 200, "the PC speaker produces output when gated on");

		// Audio is the host's setting: starting a program must not silence it.
		machine_reset(s);
		check(s.sound.enabled, "a reset keeps audio enabled");
		io_write8(s, 0x43, 0xb6);
		io_write8(s, 0x42, 0x00);
		io_write8(s, 0x42, 0x10);
		io_write8(s, 0x61, 0x03);
		s.cycles += 100000;
		m.generate_audio(samples, 735);
		peak = 0;
		for (const int16_t sample : samples) peak = sample > peak ? sample : peak;
		check(peak > 200, "the speaker still sounds after a reset");
	}

	void test_shell()
	{
		machine m;
		m.start_shell();
		check(m.shell_active(), "the shell comes up");

		const auto type = [&](const char* text)
		{
			for (const char* p = text; *p != 0; ++p) bios_push_key(m.state(), 0, uint8_t(*p));
			m.exec(m.cycles_per_frame());
		};

		char screen[8192] = {};
		type("ver\r");
		m.text_screen(screen, sizeof(screen));
		check(strstr(screen, "DOS 5.00") != nullptr, "a built-in command runs");

		type("nosuchthing\r");
		m.text_screen(screen, sizeof(screen));
		check(strstr(screen, "Bad command") != nullptr, "an unknown command is reported");
		check(strstr(screen, "C:\\>") != nullptr, "the prompt comes back");

		// Backspace has to edit the line rather than reach the command.
		type("verx\b\r");
		m.text_screen(screen, sizeof(screen));
		int versions = 0;
		for (const char* p = screen; (p = strstr(p, "DOS 5.00")) != nullptr; ++p) ++versions;
		check(versions == 2, "backspace edits the line before it is run");
	}

	// A host that serves one file from memory, so the EXEC path can be tested
	// without anything on disk.
	class memory_host final : public host_services
	{
	public:
		void add(const char* path, const std::vector<uint8_t>& contents)
		{
			name_ = path;
			contents_ = contents;
		}

		int file_open(const char* path, int) override
		{
			if (name_ != path) return -1;
			position_ = 0;
			return 7;
		}

		int file_create(const char*, uint8_t) override { return -1; }
		void file_close(int) override { }

		int file_read(int, uint8_t* buffer, int length) override
		{
			int copied = 0;
			while (copied < length && position_ < contents_.size()) buffer[copied++] = contents_[position_++];
			return copied;
		}

		int file_write(int, const uint8_t*, int) override { return -1; }
		int32_t file_seek(int, int32_t, int) override { return -1; }
		bool file_delete(const char*) override { return false; }
		bool file_rename(const char*, const char*) override { return false; }

		bool file_info(const char* path, host_file_info& info) override
		{
			if (name_ != path) return false;
			info.size = uint32_t(contents_.size());
			return true;
		}

		bool file_set_attributes(const char*, uint8_t) override { return false; }
		int find_first(const char*, uint8_t, host_file_info&) override { return -1; }
		bool find_next(int, host_file_info&) override { return false; }
		void find_close(int) override { }
		bool make_directory(const char*) override { return false; }
		bool remove_directory(const char*) override { return false; }
		bool directory_exists(const char*) override { return false; }
		uint32_t free_space() override { return 0x100000; }

		void clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
		           int& hundredths) override
		{
			year = 1990;
			month = 1;
			day = 1;
			weekday = 1;
			hour = 12;
			minute = 0;
			second = 0;
			hundredths = 0;
		}

	private:
		std::string name_;
		std::vector<uint8_t> contents_;
		size_t position_ = 0;
	};

	// A program started from the prompt, and one that will not load.
	void test_shell_programs()
	{
		const auto run_from_prompt = [](memory_host& host, const char* typed, char* screen, int screen_size)
		{
			machine m;
			m.set_host(&host);
			m.start_shell();
			for (const char* p = typed; *p != 0; ++p) bios_push_key(m.state(), 0, uint8_t(*p));
			for (int frame = 0; frame < 30; ++frame) m.exec(m.cycles_per_frame());
			m.text_screen(screen, screen_size);
			return m.shell_active() && !m.state().shell.program_running;
		};

		char screen[8192] = {};
		{
			// mov ah,9 / mov dx,010Ch / int 21h / mov ax,4C05h / int 21h / "HELLO$"
			memory_host host;
			host.add("C:\\HELLO.COM", {0xb4, 0x09, 0xba, 0x0c, 0x01, 0xcd, 0x21, 0xb8, 0x05, 0x4c, 0xcd, 0x21,
			                           'H', 'E', 'L', 'L', 'O', '$'});
			check(run_from_prompt(host, "hello\r", screen, sizeof(screen)), "a program run from the prompt returns to it");
			check(strstr(screen, "HELLO") != nullptr, "a text program's output is still on screen when it ends");
			check(strstr(screen, "exit code 5") != nullptr, "the exit code is reported");
		}
		{
			memory_host host;
			host.add("C:\\TINY.COM", {0x90});
			check(run_from_prompt(host, "tiny\r", screen, sizeof(screen)), "a program that will not load leaves the prompt");
			check(strstr(screen, "too small") != nullptr, "the reason it would not load is shown");
			check(strstr(screen, "C:\\>C:\\>") == nullptr, "the prompt is shown once");
			check(strstr(screen, "Type HELP") == nullptr, "the banner is not shown again");
		}
		{
			// Writes its own command tail to stdout: mov ah,40h / mov bx,1 /
			// mov cl,[0080h] / mov ch,0 / mov dx,0081h / int 21h / mov ax,4C00h / int 21h
			memory_host host;
			host.add("C:\\ARGS.COM", {0xb4, 0x40, 0xbb, 0x01, 0x00, 0x8a, 0x0e, 0x80, 0x00, 0xb5, 0x00, 0xba, 0x81,
			                          0x00, 0xcd, 0x21, 0xb8, 0x00, 0x4c, 0xcd, 0x21});
			run_from_prompt(host, "args one two\r", screen, sizeof(screen));
			check(strstr(screen, "one two") != nullptr, "a program run from the prompt gets its command line");
		}
	}

	// Sources held in memory, for compiler tests that include nothing.
	struct no_sources
	{
		cc_sources base;
	};

	int load_nothing(cc_sources*, const char*, const void**, size_t*) { return 0; }

	no_sources make_no_sources()
	{
		no_sources sources = {};
		sources.base.load = load_nothing;
		return sources;
	}

	// Every compiler test runs at each CPU level, so the 386 encodings are
	// covered by the same answers rather than by a separate set of checks.
	cc_cpu compiling_for = cc_cpu_i8086;

	cc_options default_options()
	{
		cc_options options;
		cc_options_init(&options);
		return options;
	}

	bool compile_text(const char* source, std::vector<uint8_t>& image, const char* name,
	                  cc_options options = default_options())
	{
		options.cpu = compiling_for;
		no_sources sources = make_no_sources();
		cc_result result = cc_compile(source, name, &sources.base, nullptr, 0, &options);
		if (!result.ok) printf("      %s(%d): %s\n", name, result.line, result.message);
		image.assign(result.image, result.image + result.image_size);
		const bool ok = result.ok != 0;
		cc_result_free(&result);
		return ok;
	}

	std::string run_image(const std::vector<uint8_t>& image, host_services* host)
	{
		machine m;
		if (host != nullptr) m.set_host(host);
		if (!m.load_program(image.data(), uint32_t(image.size()), "C:\\TEST.COM", "")) return "";
		for (int frame = 0; frame < 400 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		char screen[8192] = {};
		m.text_screen(screen, sizeof(screen));
		return screen;
	}

	// The memory model the C compiler is being rewritten onto: an arena, a
	// growable list, a sorted table and interned names. Everything the ported
	// front end will stand on, so it is worth pinning on its own.
	void test_support()
	{
		cc_arena arena;
		cc_arena_init(&arena, 1024); // small, so the block chain is exercised

		{
			char* first = (char*)cc_arena_alloc(&arena, 3);
			char* second = (char*)cc_arena_alloc(&arena, 1);
			char* large = (char*)cc_arena_alloc(&arena, 40000);
			check(first != nullptr && second != nullptr && large != nullptr, "the arena hands out memory");
			check((reinterpret_cast<uintptr_t>(second) & 7) == 0, "every allocation is aligned");
			check(second >= first + 3, "allocations do not overlap");
			check(first[0] == 0 && large[39999] == 0, "an allocation arrives zeroed");
			memset(large, 0x5a, 40000); // a block of its own, so this must not tread on anything
			check(first[0] == 0, "an oversized allocation gets its own block");

			const char* text = cc_arena_copy(&arena, "abcdef", 3);
			check(text != nullptr && strcmp(text, "abc") == 0, "a copy is terminated at the length given");
		}

		{
			cc_list list;
			cc_list_init(&list, int(sizeof(int)));
			for (int i = 0; i < 100; ++i) *(int*)cc_list_add(&list) = i * 3;
			check(list.count == 100 && CC_AT(list, int, 99) == 297, "a list grows and keeps its contents");

			cc_list_set_count(&list, 10);
			check(list.count == 10 && CC_AT(list, int, 9) == 27, "rewinding a list keeps what is left");
			cc_list_set_count(&list, 12);
			check(CC_AT(list, int, 11) == 0, "extending a list zeroes what it adds");
			cc_list_free(&list);
		}

		{
			cc_map map;
			cc_map_init(&map);
			const int keys[] = {40, 10, 30, 20, 10};
			for (int i = 0; i < 5; ++i) cc_map_set(&map, keys[i], keys[i] * 2);
			check(cc_map_count(&map) == 4, "setting a key twice replaces it");
			check(cc_map_get(&map, 30, -1) == 60, "a key can be found");
			check(cc_map_get(&map, 35, -1) == -1, "a missing key gives the default");
			check(cc_map_pair(&map, 0)->key == 10 && cc_map_pair(&map, 3)->key == 40,
			      "a map is walked in key order whatever order it was filled in");
			cc_map_free(&map);
		}

		{
			cc_names names;
			cc_names_init(&names, &arena);
			const int a = cc_intern(&names, "counter", -1);
			const int b = cc_intern(&names, "counter_x", 7);
			const int c = cc_intern(&names, "counters", -1);
			check(a != 0 && a == b, "the same name interns to the same id");
			check(c != a, "a longer name is a different name");
			check(strcmp(cc_name_text(&names, a), "counter") == 0, "an id gives its text back");
			check(strcmp(cc_name_text(&names, 0), "") == 0, "zero is no name at all");

			// Enough to force the table to grow more than once, all still distinct.
			char name[32] = {};
			for (int i = 0; i < 1000; ++i)
			{
				snprintf(name, sizeof(name), "name_%d", i);
				cc_intern(&names, name, -1);
			}
			check(cc_names_count(&names) == 1002, "growing the table keeps every name exactly once");
			check(cc_intern(&names, "name_500", -1) == cc_intern(&names, "name_500", -1),
			      "a name interned before the table grew is still found");
			cc_names_free(&names);
		}

		{
			size_t length = 0;
			const char* text = cc_normalise_source(&arena, "a\r\nb\rc\n", 7, &length);
			check(text != nullptr && strcmp(text, "a\nb\nc\n") == 0 && length == 6,
			      "line endings become one thing");

			text = cc_normalise_source(&arena, "\xef\xbb\xbf" "int x;", 9, &length);
			check(strcmp(text, "int x;") == 0, "a UTF-8 byte order mark is dropped");

			text = cc_normalise_source(&arena, "code\x1a" "junk", 9, &length);
			check(strcmp(text, "code") == 0 && length == 4, "the text stops at Ctrl+Z");

			const char utf16[] = "\xff\xfe" "i\0n\0t\0 \0=\0";
			text = cc_normalise_source(&arena, utf16, 12, &length);
			check(strcmp(text, "int =") == 0, "UTF-16 becomes UTF-8");

			const char pair[] = "\xff\xfe" "\x3d\xd8\x00\xde";
			text = cc_normalise_source(&arena, pair, 6, &length);
			check(length == 4 && (unsigned char)text[0] == 0xf0, "a surrogate pair becomes four bytes");
		}

		cc_arena_free(&arena);
		check(arena.block == nullptr, "the arena gives everything back at once");
	}

	void test_compiler()
	{
		std::vector<uint8_t> image;

		// Expressions, control flow, recursion, pointers and the preprocessor.
		const char* arithmetic =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"#define OUT(c) __int21(0x0200, 0, 0, (c))\n"
			"void putch(int c) { OUT(c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putu(unsigned v) {\n"
			"  char d[8]; int at;\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v) { d[at] = (char)('0' + v % 10); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
			"struct point { int x; int y; };\n"
			"struct point origin;\n"
			"int total(int *values, int count) {\n"
			"  int sum; int i; sum = 0;\n"
			"  for (i = 0; i < count; i = i + 1) sum = sum + values[i];\n"
			"  return sum;\n"
			"}\n"
			"int main(void) {\n"
			"  int numbers[4]; int i;\n"
			"  puts(\"A\"); putu(fib(10));\n"
			"  puts(\"B\"); putu(1000 / 8 + 3 * 4);\n"
			"  origin.x = 20; origin.y = 22;\n"
			"  puts(\"C\"); putu(origin.x + origin.y);\n"
			"  for (i = 0; i < 4; i = i + 1) numbers[i] = (i + 1) * 10;\n"
			"  puts(\"D\"); putu(total(numbers, 4));\n"
			"  puts(\"E\");\n"
			"  switch (numbers[1]) { case 10: putu(1); break; case 20: putu(2); break; default: putu(9); }\n"
			"#ifdef __DOS__\n"
			"  puts(\"F\");\n"
			"#endif\n"
			"  return 0;\n"
			"}\n";

		check(compile_text(arithmetic, image, "arithmetic.c"), "the compiler accepts the test program");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("A55") != std::string::npos, "recursion and unsigned printing work");
			check(screen.find("B137") != std::string::npos, "operator precedence is right");
			check(screen.find("C42") != std::string::npos, "structure members work");
			check(screen.find("D100") != std::string::npos, "arrays and pointer parameters work");
			check(screen.find("E2") != std::string::npos, "switch picks the right case");
			check(screen.find("F") != std::string::npos, "the preprocessor defines __DOS__");
		}

		// 32-bit arithmetic, which an 8086 has no instruction for: everything
		// harder than an add goes through a helper the compiler emits.
		const char* wide =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"void putch(int c) { __int21(0x0200, 0, 0, c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putl(long v) {\n"
			"  char d[12]; int at;\n"
			"  if (v < 0) { putch('-'); v = -v; }\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v != 0) { d[at] = (char)('0' + (int)(v % 10)); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"long total;\n"
			"int main(void) {\n"
			"  long a; long b; unsigned long u; int i;\n"
			"  a = 100000; b = 7;\n"
			"  puts(\"A\"); putl(a * b);\n"
			"  puts(\"B\"); putl(a / b);\n"
			"  puts(\"C\"); putl(a % b);\n"
			"  puts(\"D\"); putl(-a / b);\n"
			"  puts(\"E\"); putl(1L << 20);\n"
			"  puts(\"F\"); putl(a > 99999 ? 1 : 0);\n"
			"  u = 2000000; u = u * 2000;\n"
			"  puts(\"G\"); putl((long)(u / 1000));\n"
			"  total = 0;\n"
			"  for (i = 0; i < 5; i = i + 1) total = total + 1000000;\n"
			"  puts(\"H\"); putl(total);\n"
			"  puts(\"I\"); putl((long)sizeof(long));\n"
			"  return 0;\n"
			"}\n";

		check(compile_text(wide, image, "wide.c"), "the compiler accepts 32-bit arithmetic");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("A700000") != std::string::npos, "a 32-bit multiply is right");
			check(screen.find("B14285") != std::string::npos, "a signed 32-bit divide is right");
			check(screen.find("C5") != std::string::npos, "a 32-bit remainder is right");
			check(screen.find("D-14285") != std::string::npos, "a negative divide truncates towards zero");
			check(screen.find("E1048576") != std::string::npos, "a 32-bit shift is right");
			check(screen.find("F1") != std::string::npos, "a 32-bit comparison is right");
			check(screen.find("G4000000") != std::string::npos, "an unsigned 32-bit divide is right");
			check(screen.find("H5000000") != std::string::npos, "a 32-bit global accumulates");
			check(screen.find("I4") != std::string::npos, "long is four bytes");
		}

		// The shapes the code generator has special cases for: an operand that
		// can be placed rather than pushed, a comparison left in the flags, a
		// dereference whose address stays in memory, a statement applied
		// straight to a variable, and a loop with its condition at the bottom.
		const char* placement =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"void putch(int c) { __int21(0x0200, 0, 0, c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putu(unsigned v) {\n"
			"  char d[8]; int at;\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v) { d[at] = (char)('0' + v % 10); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"int failed; int counter; int numbers[8]; long total;\n"
			"struct point { int x; int y; long z; };\n"
			"struct point points[4];\n"
			"void expect(int ok, int id) { if (!ok) { failed = 1; puts(\"X\"); putu(id); } }\n"
			"int bump(void) { counter = counter + 1; return counter; }\n"
			"int main(void) {\n"
			"  int i; int j; long a; int *p; struct point *q; unsigned u; char c;\n"
			"  failed = 0;\n"
			"  i = 5; i++; ++i; i--; expect(i == 6, 1);\n"
			"  i += 10; i -= 6; i = i + 3; expect(i == 13, 2);\n"
			"  i &= 12; i |= 1; i ^= 5; expect(i == 8, 3);\n"
			"  j = 4; i = i + j; expect(i == 12, 4);\n"
			"  i = 5; j = i++; expect(j == 5 && i == 6, 5);\n"
			"  j = ++i; expect(j == 7 && i == 7, 6);\n"
			"  total = 100000; total += 100000; a = 7; total = total + a;\n"
			"  expect(total == 200007, 7);\n"
			"  numbers[0] = 11; numbers[1] = 22; numbers[2] = 33;\n"
			"  p = &numbers[0]; p++; expect(*p == 22, 8);\n"
			"  p += 1; expect(*p == 33, 9); p = p - 2; expect(*p == 11, 10);\n"
			"  points[1].x = 4; points[1].y = 5; points[1].z = 400000;\n"
			"  q = &points[1];\n"
			"  expect(q->x == 4 && q->y == 5 && q->z == 400000, 11);\n"
			"  q->y = q->y + 2; q->z += 1;\n"
			"  expect(points[1].y == 7 && points[1].z == 400001, 12);\n"
			"  i = 3; j = 4;\n"
			"  expect((i < j) == 1 && (i > j) == 0 && !(i > j), 13);\n"
			"  expect((i < j) + (j < i) == 1, 14);\n"
			"  u = 40000; expect(u > 100 && !(u < 100), 15);\n"
			"  counter = 0; i = bump() + counter; expect(i == 1 || i == 2, 16);\n"
			"  counter = 0; i = counter + bump(); expect(i == 1 || i == 2, 17);\n"
			"  a = 0x12345678;\n"
			"  expect((a >> 16) == 0x1234 && (a >> 8) == 0x123456, 18);\n"
			"  expect((a << 4) == 0x23456780 && (a & 0xffff) == 0x5678, 19);\n"
			"  a = -70000;\n"
			"  expect((a >> 16) == -2 && (a >> 1) == -35000, 20);\n"
			"  expect(a * 4 == -280000 && a / 8 == -8750, 21);\n"
			"  i = 100; u = 100;\n"
			"  expect(i * 8 == 800 && i / 4 == 25, 22);\n"
			"  expect(u / 8 == 12 && u % 8 == 4, 23);\n"
			"  expect((i << 3) == 800 && (i >> 2) == 25, 24);\n"
			"  for (i = 0; i < 8; i++) numbers[i] = i * i;\n"
			"  j = 0; for (i = 0; i < 8; i++) j += numbers[i];\n"
			"  expect(j == 140, 25);\n"
			"  i = 0; j = 0; while (i < 8) { j = j + numbers[i]; i++; }\n"
			"  expect(j == 140, 26);\n"
			"  j = 0; i = 0; do { j = j + 1; i++; } while (i < 4);\n"
			"  expect(j == 4, 27);\n"
			"  j = 0;\n"
			"  for (i = 0; i < 20; i++) { if (i == 3) continue; if (i == 6) break; j = j + i; }\n"
			"  expect(j == 12, 28);\n"
			"  c = -3; expect(c < 0 && c + 5 == 2, 29);\n"
			"  numbers[3] = 7; p = &numbers[3];\n"
			"  (*p)++; expect(numbers[3] == 8, 30);\n"
			"  ++(*p); expect(numbers[3] == 9, 31);\n"
			"  *p += 5; expect(numbers[3] == 14, 32);\n"
			"  *p = *p * 2; expect(numbers[3] == 28, 33);\n"
			"  q = &points[2]; q->z = 70000; q->z >>= 1;\n"
			"  expect(points[2].z == 35000, 34);\n"
			"  if (!failed) puts(\"OK\");\n"
			"  return 0;\n"
			"}\n";

		check(compile_text(placement, image, "placement.c"), "the compiler accepts the placement program");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("OK") != std::string::npos, "the generated code is right where it is clever");
		}

		// A body copied to its call site has to mean exactly what calling it
		// would have: the same answers, whatever the size limit is set to.
		const char* whole_program =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"void putch(int c) { __int21(0x0200, 0, 0, c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putu(unsigned v) {\n"
			"  char d[8]; int at;\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v) { d[at] = (char)('0' + v % 10); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"int failed; int counter; int numbers[4];\n"
			"void expect(int ok, int id) { if (!ok) { failed = 1; puts(\"X\"); putu(id); } }\n"
			"int scale(int v, int f) { return (v * f) >> 2; }\n"
			"int clamp(int v, int low, int high) {\n"
			"  if (v < low) return low;\n"
			"  if (v > high) return high;\n"
			"  return v;\n"
			"}\n"
			"int consume(int n) { n = n + 1; n = n * 2; return n; }\n"
			"long widen(int v) { return (long)v * 100000; }\n"
			"int bump(void) { counter = counter + 1; return counter; }\n"
			"int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
			"int never_called(int a) { return a * 12345; }\n"
			"void fill(int *p, int v) { p[0] = v; p[1] = v + 1; }\n"
			"int main(void) {\n"
			"  int i; long l;\n"
			"  failed = 0; counter = 0;\n"
			"  expect(scale(100, 3) == 75, 1);\n"
			"  i = 7; expect(scale(i, 4) == 7, 2);\n"
			"  expect(clamp(5, 0, 10) == 5 && clamp(-1, 0, 10) == 0 && clamp(99, 0, 10) == 10, 3);\n"
			"  i = 4; expect(consume(i) == 10 && i == 4, 4);\n"
			"  l = widen(3); expect(l == 300000, 5);\n"
			"  expect(bump() == 1 && bump() == 2 && counter == 2, 6);\n"
			"  expect(fib(10) == 55, 7);\n"
			"  fill(numbers, 9); expect(numbers[0] == 9 && numbers[1] == 10, 8);\n"
			"  i = 0;\n"
			"  while (i < 3) { i = i + scale(4, 1); }\n"
			"  expect(i == 3, 9);\n"
			"  if (!failed) puts(\"OK\");\n"
			"  return 0;\n"
			"}\n";

		for (const int limit : {0, 24, 200})
		{
			cc_options options = default_options();
			options.inline_limit = limit;
			check(compile_text(whole_program, image, "inline.c", options),
			      "the compiler accepts the whole-program test");
			if (image.empty()) continue;
			const std::string screen = run_image(image, nullptr);
			check(screen.find("OK") != std::string::npos,
			      limit == 0
				      ? "calling and copying a body agree, with no copying"
				      : "calling and copying a body agree, with copying");
		}

		// A function nothing can reach costs nothing, and one that was copied
		// everywhere stops being reached.
		cc_options keep = default_options();
		keep.drop_unreachable = false;
		std::vector<uint8_t> lean, fat;
		check(compile_text(whole_program, lean, "inline.c"), "the lean build compiles");
		check(compile_text(whole_program, fat, "inline.c", keep), "the kept build compiles");
		check(!lean.empty() && !fat.empty() && lean.size() < fat.size(),
		      "an unreachable function is left out");

		// Function pointers, which reach a callee through a frame slot rather
		// than a relative call, and goto.
		const char* indirect =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"void putch(int c) { __int21(0x0200, 0, 0, c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putu(unsigned v) {\n"
			"  char d[8]; int at;\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v) { d[at] = (char)('0' + v % 10); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"int twice(int n) { return n * 2; }\n"
			"int square(int n) { return n * n; }\n"
			"typedef int (*op_t)(int);\n"
			"struct entry { int base; int (*run)(int); };\n"
			"struct entry list[2];\n"
			"op_t start[2] = { twice, square };\n"
			"op_t table[2];\n"
			"int apply(op_t f, int n) { return f(n); }\n"
			"int main(void) {\n"
			"  op_t f; int i; int sum;\n"
			"  f = twice;\n"
			"  puts(\"A\"); putu(f(21));\n"
			"  f = square;\n"
			"  puts(\"B\"); putu((*f)(7));\n"
			"  puts(\"C\"); putu(apply(twice, 100));\n"
			"  table[0] = twice; table[1] = square;\n"
			"  sum = 0;\n"
			"  for (i = 0; i < 2; i = i + 1) sum = sum + table[i](5);\n"
			"  puts(\"D\"); putu(sum);\n"
			"  list[0].base = 3; list[0].run = twice;\n"
			"  list[1].base = 4; list[1].run = square;\n"
			"  sum = 0;\n"
			"  for (i = 0; i < 2; i = i + 1) sum = sum + list[i].run(list[i].base);\n"
			"  puts(\"E\"); putu(sum);\n"
			"  puts(\"F\"); putu(start[1](6));\n"
			"  i = 0;\n"
			"again:\n"
			"  i = i + 1;\n"
			"  if (i < 4) goto again;\n"
			"  puts(\"G\"); putu(i);\n"
			"  return 0;\n"
			"}\n";

		check(compile_text(indirect, image, "indirect.c"), "the compiler accepts function pointers");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("A42") != std::string::npos, "a call through a pointer variable works");
			check(screen.find("B49") != std::string::npos, "(*f)(x) and f(x) mean the same thing");
			check(screen.find("C200") != std::string::npos, "a function passed as an argument works");
			check(screen.find("D35") != std::string::npos, "a call through an array element works");
			check(screen.find("E22") != std::string::npos, "a call through a structure member works");
			check(screen.find("F36") != std::string::npos, "a global initialised with a function works");
			check(screen.find("G4") != std::string::npos, "goto jumps to a label");
		}

		// Variadic arguments, which are found by address above the last named
		// parameter rather than being passed in any declared slot.
		const char* variadic =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"typedef char *va_list;\n"
			"#define va_start(ap, last) ((ap) = __args(last))\n"
			"#define va_arg(ap, type) (*(type *)(((ap) += sizeof(type)) - sizeof(type)))\n"
			"void putch(int c) { __int21(0x0200, 0, 0, c); }\n"
			"void puts(char *s) { while (*s) { putch(*s); s = s + 1; } }\n"
			"void putl(long v) {\n"
			"  char d[12]; int at;\n"
			"  if (v < 0) { putch('-'); v = -v; }\n"
			"  if (v == 0) { putch('0'); return; }\n"
			"  at = 0;\n"
			"  while (v != 0) { d[at] = (char)('0' + (int)(v % 10)); v = v / 10; at = at + 1; }\n"
			"  while (at > 0) { at = at - 1; putch(d[at]); }\n"
			"}\n"
			"int total(int count, ...) {\n"
			"  va_list ap; int i; int sum;\n"
			"  va_start(ap, count);\n"
			"  sum = 0;\n"
			"  for (i = 0; i < count; i = i + 1) sum = sum + va_arg(ap, int);\n"
			"  return sum;\n"
			"}\n"
			"void show(char *label, ...) {\n"
			"  va_list ap;\n"
			"  va_start(ap, label);\n"
			"  puts(label);\n"
			"  putl(va_arg(ap, long));\n"
			"  putch(va_arg(ap, int));\n"
			"}\n"
			"int main(void) {\n"
			"  puts(\"A\"); putl((long)total(4, 10, 20, 30, 40));\n"
			"  puts(\"B\"); putl((long)total(1, 7));\n"
			"  show(\"C\", 100000L * 7, 'z');\n"
			"  return 0;\n"
			"}\n";

		check(compile_text(variadic, image, "variadic.c"), "the compiler accepts variadic functions");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("A100") != std::string::npos, "variadic arguments are summed in order");
			check(screen.find("B7") != std::string::npos, "a single variadic argument works");
			check(screen.find("C700000z") != std::string::npos, "a 32-bit variadic argument takes two slots");
		}

		// Loading a child through INT 21h AH=4Bh and coming back.
		const char* child =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"int main(void) { __int21(0x0200, 0, 0, 'X'); return 0; }\n";
		std::vector<uint8_t> child_image;
		check(compile_text(child, child_image, "child.c"), "the child program compiles");

		const char* parent =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"unsigned __seg(void);\n"
			"extern int __cf;\n"
			"char name[16];\n"
			"unsigned block[7];\n"
			"char tail[4];\n"
			"int main(void) {\n"
			"  name[0] = 'C'; name[1] = ':'; name[2] = 92; name[3] = 'K';\n"
			"  name[4] = 'I'; name[5] = 'D'; name[6] = '.'; name[7] = 'C';\n"
			"  name[8] = 'O'; name[9] = 'M'; name[10] = 0;\n"
			"  tail[0] = 0; tail[1] = 13;\n"
			"  block[0] = 0; block[1] = (unsigned)tail; block[2] = __seg();\n"
			"  block[3] = 0x5c; block[4] = __seg(); block[5] = 0x6c; block[6] = __seg();\n"
			"  __int21(0x0200, 0, 0, 'P');\n"
			"  __int21(0x4b00, (int)block, 0, (int)name);\n"
			"  __int21(0x0200, 0, 0, __cf == 0 ? 'Q' : '!');\n"
			"  return 0;\n"
			"}\n";
		check(compile_text(parent, image, "parent.c"), "the parent program compiles");

		if (!image.empty() && !child_image.empty())
		{
			memory_host host;
			host.add("C:\\KID.COM", child_image);
			const std::string screen = run_image(image, &host);
			check(screen.find("PXQ") != std::string::npos, "EXEC runs a child and returns to the parent");
		}

		// The type of a constant decides how much of it travels: a decimal
		// literal with a U suffix that fits in 16 bits is an unsigned int, and
		// a cast narrows a constant the way the register would hold it.
		const char* constants =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"void say(int ok) { __int21(0x0200, 0, 0, ok ? 'Y' : 'N'); }\n"
			"int main(void) {\n"
			"  __int21(0x0200, 0, 0, '[');\n"
			"  say(sizeof(65535u) == 2); say(sizeof(40000u) == 2); say(sizeof(40000) == 4);\n"
			"  say(((unsigned)0x8000 >> 15) == 1); say((unsigned char)0x1ff == 255); say((char)0x80 < 0);\n"
			"  __int21(0x0200, 0, 0, ']');\n"
			"  return 0;\n"
			"}\n";
		check(compile_text(constants, image, "constants.c"), "the constant typing program compiles");
		if (!image.empty())
		{
			const std::string screen = run_image(image, nullptr);
			check(screen.find("[YYYYYY]") != std::string::npos, "constants keep the type and width C gives them");
		}
	}

	// What the compiler says about a program it refuses, and on which line;
	// empty if it compiled after all.
	std::string compile_error(const char* source, int* line = nullptr)
	{
		cc_options options = default_options();
		no_sources sources = make_no_sources();
		cc_result result = cc_compile(source, "refused.c", &sources.base, nullptr, 0, &options);
		std::string message = result.ok ? "" : (result.message != nullptr ? result.message : "?");
		if (line != nullptr) *line = result.line;
		cc_result_free(&result);
		return message;
	}

	// Each of these used to compile without a word, into something that did
	// not do what the source says, or to fail with a message about something
	// else entirely.
	void test_compiler_refusals()
	{
		const auto refused = [](const char* source, const char* says)
		{
			return compile_error(source).find(says) != std::string::npos;
		};

		check(refused("struct p { int x; };\n"
		              "int main(void) { struct p a; struct p b; a.x = 1; b = a; return b.x; }\n",
		              "structure assignment is not supported"), "structure assignment is refused");
		check(refused("int a[3];\nint main(void) { a = 5; return 0; }\n", "cannot assign to an array"),
		      "assigning to an array is refused");
		check(refused("int main(void) { return 3; }\nint main(void) { return 4; }\n", "redefinition of function"),
		      "a second definition of a function is refused");
		check(refused("struct p { int x; };\nstruct p f(void) { struct p r; r.x = 1; return r; }\n"
		              "int main(void) { return 0; }\n", "returning a structure"),
		      "returning a structure is refused");
		check(refused("struct p { int x; };\nint f(struct p v) { return v.x; }\nint main(void) { return 0; }\n",
		              "passing a structure by value"), "a structure parameter is refused");

		int line = 0;
		const std::string comment = compile_error("int main(void) { return 0; }\n/* never closed\nint x;\n", &line);
		check(comment.find("unterminated comment") != std::string::npos && line == 2,
		      "an unterminated comment is reported where it opens");
		check(refused("#if 1\nint main(void) { return 0; }\n", "unterminated #if"),
		      "an #if with no #endif is refused");
		const std::string body = compile_error("int main(void)\n{\n\tif (1) {\n\t\treturn 0;\n\t}\n", &line);
		check(!body.empty() && line == 5, "an error at the end of the file is reported on its last line");
	}

	// What a hosted image gets instead of DOS. The services are the ones a
	// compiled program actually asks for, and each is the host's own, so a
	// native run reads and writes the same files the emulated one does.
	struct native_registers
	{
		uint16_t ax, bx, cx, dx, si;
	};

	// The generated code calls back with nothing but the block, and there is one
	// program running at a time, so the host it belongs to is kept here.
	host_services* native_host = nullptr;
	jmp_buf native_finished;
	int native_exit_code = 0;

	// A guest path is relative to the drive the host mounted, and the host wants
	// it the way DOS spells it.
	void native_path(const char* text, char* into, size_t size)
	{
		if (text[0] != 0 && text[1] == ':') snprintf(into, size, "%s", text);
		else snprintf(into, size, "C:\\%s", text[0] == '\\' ? text + 1 : text);
		for (char* at = into; *at != 0; ++at)
			if (*at >= 'a' && *at <= 'z') *at = char(*at - 'a' + 'A');
	}

	void native_service(void* block, int number)
	{
		uint8_t* memory = static_cast<uint8_t*>(block);
		native_registers* r = reinterpret_cast<native_registers*>(memory + CC_NATIVE_REGISTERS);
		uint8_t* carry = memory + CC_NATIVE_CARRY;
		char path[MAX_PATH] = {};
		int result = 0;

		*carry = 0;
		if (number != 0x21) return;

		switch ((r->ax >> 8) & 0xff)
		{
		case 0x02:
			putchar(r->dx & 0xff);
			r->ax = uint16_t((r->ax & 0xff00) | (r->dx & 0xff));
			break;

		case 0x09:
			for (int at = r->dx; at < 0x10000 && memory[at] != '$'; ++at) putchar(memory[at]);
			break;

		case 0x3c: // create
			native_path(reinterpret_cast<const char*>(memory + r->dx), path, sizeof(path));
			result = native_host != nullptr ? native_host->file_create(path, uint8_t(r->cx)) : -1;
			if (result < 0) *carry = 1;
			r->ax = uint16_t(result < 0 ? 3 : result);
			break;

		case 0x3d: // open
			native_path(reinterpret_cast<const char*>(memory + r->dx), path, sizeof(path));
			result = native_host != nullptr ? native_host->file_open(path, r->ax & 0xff) : -1;
			if (result < 0) *carry = 1;
			r->ax = uint16_t(result < 0 ? 2 : result);
			break;

		case 0x3e: // close
			if (native_host != nullptr) native_host->file_close(r->bx);
			break;

		case 0x3f: // read
			result = native_host != nullptr
				         ? native_host->file_read(r->bx, memory + r->dx, r->cx)
				         : -1;
			if (result < 0) *carry = 1;
			r->ax = uint16_t(result < 0 ? 5 : result);
			break;

		case 0x40: // write
			result = native_host != nullptr
				         ? native_host->file_write(r->bx, memory + r->dx, r->cx)
				         : -1;
			if (result < 0) *carry = 1;
			r->ax = uint16_t(result < 0 ? 5 : result);
			break;

		case 0x41: // delete
			native_path(reinterpret_cast<const char*>(memory + r->dx), path, sizeof(path));
			if (native_host == nullptr || !native_host->file_delete(path)) *carry = 1;
			break;

		case 0x4c: // terminate
			native_exit_code = r->ax & 0xff;
			longjmp(native_finished, 1);

		default:
			*carry = 1;
			r->ax = 1;
			break;
		}
	}

	// Compiles for the machine this is running on, puts the result somewhere it
	// can be executed, and calls it. The same source through the same front end
	// and a different back end has to give the same answer as the emulator did,
	// which is the only claim a second back end is really making.
	//
	// The block is the whole of the program's world: code, data and stack, the
	// 64 KiB a .COM gets, with its address handed to the entry point so that a
	// guest pointer can stay the 16-bit offset the front end thinks it is.
	bool run_native(const std::vector<uint8_t>& image, host_services& host, int& answer)
	{
#if !defined(_M_X64) && !defined(__x86_64__)
		(void)image;
		(void)host;
		(void)answer;
		return false; // the back end emits x64, and this is not one
#else
		const uint32_t block_size = 0x10000;
		if (image.size() > block_size) return false;

		void* block = host.code_alloc(block_size);
		if (block == nullptr) return false;
		memset(block, 0, block_size);
		memcpy(block, image.data(), image.size());

		using service_entry = void (*)(void*, int);
		const service_entry service = native_service;
		memcpy(static_cast<uint8_t*>(block) + CC_NATIVE_SERVICE, &service, sizeof(service));
		host.code_commit(block, block_size);

		native_host = &host;
		native_exit_code = 0;
		fflush(stdout); // the generated code writes straight to the console

		// A program that exits in the middle comes back here instead of
		// returning; the generated code has no frames worth unwinding.
		if (setjmp(native_finished) == 0)
		{
			using entry_point = int (*)(void*);
			const entry_point run = reinterpret_cast<entry_point>(block);
			answer = run(block) & 0xffff;
		}
		else answer = native_exit_code;

		native_host = nullptr;
		host.code_free(block, block_size);
		return true;
#endif
	}

	void test_native_backend()
	{
		// A pointer is a 16-bit offset into the block the host hands the entry
		// point, exactly as it is under DOS, so arrays, structures and the
		// address of a local all mean the same thing on both machines.
		const char* source =
			"int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
			"int counter;\n"
			"int scale(int v, int f) { return (v * f) >> 2; }\n"
			"struct point { int x; int y; };\n"
			"struct point grid[4];\n"
			"int total(int *values, int count) {\n"
			"  int sum; int i; sum = 0;\n"
			"  for (i = 0; i < count; i = i + 1) sum = sum + values[i];\n"
			"  return sum;\n"
			"}\n"
			"int length(char *s) { int n; n = 0; while (*s) { n = n + 1; s = s + 1; } return n; }\n"
			"int main(void) {\n"
			"  int i; int sum; int local[4]; char *text;\n"
			"  sum = 0;\n"
			"  for (i = 0; i < 10; i = i + 1) sum = sum + scale(i, 6);\n"
			"  counter = 0;\n"
			"  while (counter < 5) counter = counter + 1;\n"
			"  switch (counter) { case 4: return 1; case 5: break; default: return 2; }\n"
			"  for (i = 0; i < 4; i = i + 1) { local[i] = i * 2; grid[i].x = i; grid[i].y = i + 1; }\n"
			"  sum = sum + total(local, 4);\n"
			"  for (i = 0; i < 4; i = i + 1) sum = sum + grid[i].x * grid[i].y;\n"
			"  text = \"abcdef\";\n"
			"  sum = sum + length(text);\n"
			"  return sum + fib(12) + counter;\n"
			"}\n";

		// What the interpreter says it means, through the 8086 back end.
		std::vector<uint8_t> guest;
		cc_options as_dos = default_options();
		as_dos.cpu = cc_cpu_i8086;
		{
			no_sources sources = make_no_sources();
			cc_result result = cc_compile(source, "native.c", &sources.base, nullptr, 0, &as_dos);
			check(result.ok != 0, "the differential program compiles for the 8086");
			guest.assign(result.image, result.image + result.image_size);
			cc_result_free(&result);
		}
		if (guest.empty()) return;

		machine m;
		int expected = -1;
		if (m.load_program(guest.data(), uint32_t(guest.size()), "C:\\NATIVE.COM", ""))
		{
			for (int frame = 0; frame < 400 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
			if (m.exited()) expected = int(m.exit_code());
		}
		check(expected == 252, "the emulator agrees on what the program means");

		cc_options as_host = default_options();
		as_host.machine = cc_machine_x64;
		no_sources sources = make_no_sources();
		cc_result native = cc_compile(source, "native.c", &sources.base, nullptr, 0, &as_host);
		check(native.ok != 0, "the same program compiles for the host");
		if (!native.ok)
		{
			cc_result_free(&native);
			return;
		}

		const std::vector<uint8_t> image(native.image, native.image + native.image_size);
		cc_result_free(&native);

		win32_host host;
		int answer = -1;
		if (!run_native(image, host, answer)) return; // not a host that can run it
		check_equal(uint32_t(answer), uint32_t(expected),
		            "the host back end computes what the emulator computed");
	}

	// A key travels from the host through the keyboard controller, IRQ 1 and
	// the BIOS before anything can read it. Pushing straight into the BIOS
	// buffer, as test_shell does, skips all of that.
	void test_keyboard()
	{
		{
			machine m;
			m.start_shell();

			const machine_key typed[] = {machine_key::v, machine_key::e, machine_key::r, machine_key::enter};
			for (const machine_key key : typed)
			{
				m.key_down(key);
				m.key_up(key);
			}
			for (int frame = 0; frame < 4; ++frame) m.exec(m.cycles_per_frame());

			char screen[8192] = {};
			m.text_screen(screen, sizeof(screen));
			check(strstr(screen, "DOS 5.00") != nullptr, "typing at the prompt reaches the shell");
		}

		{
			// mov ah,0 / int 16h / mov dl,al / mov ah,2 / int 21h / exit
			const uint8_t com[] = {
				0xb4, 0x00, 0xcd, 0x16, 0x88, 0xc2, 0xb4, 0x02, 0xcd, 0x21,
				0xb8, 0x00, 0x4c, 0xcd, 0x21
			};
			machine m;
			check(m.load_program(com, sizeof(com), "C:\\KEY.COM", ""), "the key test program loads");

			// It waits for a key, so it should still be running.
			m.exec(m.cycles_per_frame());
			check(!m.exited(), "INT 16h waits for a key rather than returning one");

			m.key_down(machine_key::a);
			m.key_up(machine_key::a);
			for (int frame = 0; frame < 4 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());

			check(m.exited(), "the program continued once a key arrived");
			char screen[8192] = {};
			m.text_screen(screen, sizeof(screen));
			check(strchr(screen, 'a') != nullptr, "a running program reads the key through INT 9 and INT 16h");
		}

		{
			// What the BIOS buffers for a key pressed with modifiers held: the
			// single word a program reads back, 0 for none, FFFFh for several.
			machine m;
			machine_state& s = m.state();
			const auto press = [&](const std::vector<machine_key>& held, machine_key key)
			{
				for (const machine_key k : held) m.key_down(k);
				m.key_down(key);
				m.key_up(key);
				for (const machine_key k : held) m.key_up(k);
				while (s.kbd.queue_count > 0) bios_keyboard_interrupt(s);
				uint16_t word = 0;
				uint32_t found = 0;
				int count = 0;
				while (bios_pop_key(s, word))
				{
					found = word;
					++count;
				}
				return count > 1 ? 0xffffu : found;
			};

			const machine_key shift = machine_key::left_shift;
			const machine_key ctrl = machine_key::left_ctrl;
			const machine_key alt = machine_key::left_alt;
			check_equal(press({}, machine_key::left_shift), 0, "Shift alone puts nothing in the buffer");
			check_equal(press({shift}, machine_key::a), 0x1e41, "Shift+A is one key, not two");
			check_equal(press({ctrl}, machine_key::c), 0x2e03, "Ctrl+C");
			check_equal(press({ctrl}, machine_key::enter), 0x1c0a, "Ctrl+Enter is a line feed");
			check_equal(press({alt}, machine_key::x), 0x2d00, "Alt+X has no character");
			check_equal(press({alt}, machine_key::num1), 0x7800, "Alt+1");
			check_equal(press({}, machine_key::f1), 0x3b00, "F1");
			check_equal(press({shift}, machine_key::f1), 0x5400, "Shift+F1");
			check_equal(press({ctrl}, machine_key::f1), 0x5e00, "Ctrl+F1");
			check_equal(press({alt}, machine_key::f10), 0x7100, "Alt+F10");
			check_equal(press({}, machine_key::f12), 0x8600, "F12");
			check_equal(press({ctrl}, machine_key::left), 0x7300, "Ctrl+Left");
			check_equal(press({ctrl}, machine_key::page_down), 0x7600, "Ctrl+PgDn");
			check_equal(press({}, machine_key::keypad_plus), 0x4e2b, "keypad + without NumLock");
			check_equal(press({}, machine_key::keypad7), 0x4700, "keypad 7 without NumLock is Home");
			check_equal(press({shift}, machine_key::left), 0x4b00, "Shift+Left is still an arrow");
			check_equal(press({}, machine_key::caps_lock), 0, "Caps Lock puts nothing in the buffer");
			check_equal(press({}, machine_key::q), 0x1051, "Caps Lock makes letters capitals");
		}

		// --keys types digits through the same table; the debug build once typed F-keys for them.
		check_equal(uint32_t(key_from_char('1')), uint32_t(machine_key::num1), "--keys types 1");
		check_equal(uint32_t(key_from_char('0')), uint32_t(machine_key::num0), "--keys types 0");
	}

	void test_disassembler()
	{
		machine m;
		machine_state& s = m.state();
		const uint8_t code[] = {0xb8, 0x34, 0x12, 0xcd, 0x21, 0x8b, 0x46, 0xfe, 0xf3, 0xa4};
		for (uint32_t i = 0; i < sizeof(code); ++i) s.memory[0x20000 + i] = code[i];

		disassembly line = disassemble(s, 0x2000, 0);
		check(strcmp(line.text, "mov ax,1234h") == 0, "disassembles mov r16,imm16");
		check_equal(line.length, 3, "mov r16,imm16 is three bytes");

		line = disassemble(s, 0x2000, 3);
		check(strcmp(line.text, "int 21h") == 0, "disassembles int");

		line = disassemble(s, 0x2000, 5);
		check(strcmp(line.text, "mov ax,[bp-02h]") == 0, "disassembles a signed displacement");

		line = disassemble(s, 0x2000, 8);
		check(strcmp(line.text, "rep movsb") == 0, "disassembles a repeated string op");

		const uint8_t wide[] = {
			0x0f, 0x84, 0x03, 0x00, // je near
			0x0f, 0x94, 0xc3, // sete bl
			0x0f, 0xb6, 0xc3, // movzx ax,bl
			0x0f, 0xba, 0xe8, 0x05, // bts ax,5
			0x64, 0xa0, 0x10, 0x00, // mov al,fs:[0010h]
		};
		for (uint32_t i = 0; i < sizeof(wide); ++i) s.memory[0x21000 + i] = wide[i];

		line = disassemble(s, 0x2100, 0);
		check(strcmp(line.text, "je 0007h") == 0, "disassembles a near jcc");
		check_equal(line.length, 4, "near jcc is four bytes");

		line = disassemble(s, 0x2100, 4);
		check(strcmp(line.text, "sete bl") == 0, "disassembles setcc");

		line = disassemble(s, 0x2100, 7);
		check(strcmp(line.text, "movzx ax,bl") == 0, "disassembles movzx");

		line = disassemble(s, 0x2100, 10);
		check(strcmp(line.text, "bts ax,05h") == 0, "disassembles bts");

		line = disassemble(s, 0x2100, 14);
		check(strcmp(line.text, "mov al,fs:[0010h]") == 0, "disassembles an fs override");
	}

	// A whole drive in memory, for the tests of handles, redirection and
	// COMMAND.COM /C: files and directories, wildcard searches, a console the
	// test can read back, and programs the host stands in for.
	class ram_host final : public host_services
	{
	public:
		std::map<std::string, std::string> files; // "C:\DIR\NAME.EXT" -> contents
		std::set<std::string> directories = {"C:\\"};
		bool takes_console = false;
		bool console_is_terminal = true;
		std::string console;
		std::map<std::string, int> natives; // name -> exit code
		std::string native_tail;

		int file_open(const char* path, int) override { return files.count(path) != 0 ? open(path) : -1; }

		int file_create(const char* path, uint8_t) override
		{
			files[path].clear();
			return open(path);
		}

		void file_close(int handle) override
		{
			if (valid(handle)) open_[handle].used = false;
		}

		int file_read(int handle, uint8_t* buffer, int length) override
		{
			if (!valid(handle)) return -1;
			const std::string& data = files[open_[handle].path];
			int copied = 0;
			while (copied < length && open_[handle].position < data.size())
				buffer[copied++] = uint8_t(data[open_[handle].position++]);
			return copied;
		}

		int file_write(int handle, const uint8_t* buffer, int length) override
		{
			if (!valid(handle)) return -1;
			std::string& data = files[open_[handle].path];
			for (int i = 0; i < length; ++i)
			{
				size_t& at = open_[handle].position;
				if (at < data.size()) data[at] = char(buffer[i]);
				else data += char(buffer[i]);
				++at;
			}
			return length;
		}

		int32_t file_seek(int handle, int32_t offset, int origin) override
		{
			if (!valid(handle)) return -1;
			const int32_t base = origin == 1 ? int32_t(open_[handle].position)
			                                 : origin == 2 ? int32_t(files[open_[handle].path].size()) : 0;
			open_[handle].position = size_t(base + offset);
			return int32_t(open_[handle].position);
		}

		bool file_truncate(int handle) override
		{
			if (!valid(handle)) return false;
			files[open_[handle].path].resize(open_[handle].position);
			return true;
		}

		bool file_delete(const char* path) override { return files.erase(path) != 0; }

		bool file_rename(const char* from, const char* to) override
		{
			if (files.count(from) == 0 || files.count(to) != 0) return false;
			files[to] = files[from];
			files.erase(from);
			return true;
		}

		bool file_info(const char* path, host_file_info& info) override
		{
			const bool file = files.count(path) != 0;
			if (!file && directories.count(path) == 0) return false;
			fill(path, file, info);
			return true;
		}

		bool file_set_attributes(const char* path, uint8_t) override { return files.count(path) != 0; }

		int find_first(const char* pattern, uint8_t, host_file_info& info) override
		{
			const std::string text = pattern;
			const size_t slash = text.rfind('\\');
			const std::string directory = text.substr(0, slash + 1);
			const std::string mask = text.substr(slash + 1);
			for (int i = 0; i < 32; ++i)
			{
				if (searches_[i].used) continue;
				searches_[i] = search();
				for (const auto& [path, contents] : files)
				{
					(void)contents;
					if (path.compare(0, directory.size(), directory) != 0) continue;
					const std::string name = path.substr(directory.size());
					if (name.find('\\') == std::string::npos && matches(mask, name)) searches_[i].names.push_back(path);
				}
				if (searches_[i].names.empty()) return -1;
				searches_[i].used = true;
				fill(searches_[i].names[0], true, info);
				searches_[i].next = 1;
				return i;
			}
			return -1;
		}

		bool find_next(int search, host_file_info& info) override
		{
			if (search < 0 || search >= 32 || !searches_[search].used) return false;
			if (searches_[search].next >= searches_[search].names.size())
			{
				searches_[search].used = false;
				return false;
			}
			fill(searches_[search].names[searches_[search].next++], true, info);
			return true;
		}

		void find_close(int search) override
		{
			if (search >= 0 && search < 32) searches_[search].used = false;
		}

		int open_searches() const
		{
			int count = 0;
			for (const search& s : searches_) count += s.used ? 1 : 0;
			return count;
		}

		bool make_directory(const char* path) override { return directories.insert(path).second; }
		bool remove_directory(const char* path) override { return directories.erase(path) != 0; }
		bool directory_exists(const char* path) override { return directories.count(path) != 0; }
		uint32_t free_space() override { return 0x100000; }

		void clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
		           int& hundredths) override
		{
			year = 1988;
			month = 6;
			day = 15;
			weekday = 3;
			hour = 12;
			minute = second = hundredths = 0;
		}

		bool console_write(int, const uint8_t* data, int length) override
		{
			if (!takes_console) return false;
			console.append(reinterpret_cast<const char*>(data), size_t(length));
			return true;
		}

		int console_read(uint8_t*, int) override { return takes_console ? 0 : -1; }
		bool console_is_device(int) override { return console_is_terminal; }

		int run_native(const char* name, const char* tail, const char*) override
		{
			const auto found = natives.find(name);
			if (found == natives.end()) return -1;
			native_tail = tail;
			return found->second;
		}

		std::string video; // what INT 10h wrote, in order
		void video_text(uint8_t character) override { video.push_back(char(character)); }

	private:
		struct open_file
		{
			std::string path;
			size_t position = 0;
			bool used = false;
		};

		struct search
		{
			std::vector<std::string> names;
			size_t next = 0;
			bool used = false;
		};

		open_file open_[32];
		search searches_[32];

		bool valid(int handle) const { return handle >= 0 && handle < 32 && open_[handle].used; }

		int open(const char* path)
		{
			for (int i = 0; i < 32; ++i)
			{
				if (open_[i].used) continue;
				open_[i] = open_file{path, 0, true};
				return i;
			}
			return -1;
		}

		void fill(const std::string& path, bool file, host_file_info& info)
		{
			info = host_file_info();
			snprintf(info.name, sizeof(info.name), "%s", path.substr(path.rfind('\\') + 1).c_str());
			info.size = file ? uint32_t(files[path].size()) : 0;
			info.directory = !file;
			info.attributes = file ? 0x20 : 0x10;
		}

		// DOS wildcards: "*.*" is every name, and otherwise * and ? as usual.
		static bool matches(const std::string& mask, const std::string& name)
		{
			if (mask == "*.*") return true;
			size_t m = 0;
			size_t n = 0;
			size_t star = std::string::npos;
			size_t resume = 0;
			while (n < name.size())
			{
				if (m < mask.size() && (mask[m] == '?' || mask[m] == name[n]))
				{
					++m;
					++n;
				}
				else if (m < mask.size() && mask[m] == '*')
				{
					star = m++;
					resume = n;
				}
				else if (star != std::string::npos)
				{
					m = star + 1;
					n = ++resume;
				}
				else return false;
			}
			while (m < mask.size() && mask[m] == '*') ++m;
			return m == mask.size();
		}
	};

	// Runs a compiled program as C:\TEST.COM on a host until it ends.
	machine* run_on(machine& m, const std::vector<uint8_t>& image, host_services& host)
	{
		m.set_host(&host);
		if (!m.load_program(image.data(), uint32_t(image.size()), "C:\\TEST.COM", "")) return nullptr;
		for (int frame = 0; frame < 2000 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		return &m;
	}

	// The handle table in each PSP, the system file table behind it, devices
	// by name, names cut to 8.3, and the top of memory a C runtime sizes its
	// heap from.
	void test_dos_handles()
	{
		{
			// A COM program and an EXE both get all the memory there is, and
			// PSP:0002 has to say so.
			machine m;
			const uint8_t com[] = {0xcd, 0x20};
			m.load_program(com, sizeof(com), "C:\\T.COM", "");
			machine_state& s = m.state();
			uint16_t psp = s.dos.psp_segment;
			check_equal(read16(s, psp, 2), uint16_t(psp + read16(s, uint16_t(psp - 1), 3)),
			            "PSP:0002 is the end of a COM program's block");

			uint8_t exe[0x22] = {'M', 'Z', 0x22, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00, 0xff, 0xff};
			exe[0x20] = 0xcd;
			exe[0x21] = 0x20;
			m.load_program(exe, sizeof(exe), "C:\\T.EXE", "");
			psp = s.dos.psp_segment;
			check_equal(read16(s, psp, 2), uint16_t(psp + read16(s, uint16_t(psp - 1), 3)),
			            "PSP:0002 is the end of an EXE program's block");
			check(read16(s, psp, 2) > 0x9000, "an EXE asking for everything gets everything");
		}

		const char* handles =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"int main(void) {\n"
			"  int h = __int21(0x3c00, 0, 0, (int)\"OUT.TXT\");\n"
			"  int d;\n"
			"  __int21(0x4000, h, 2, (int)\"AB\");\n"
			"  d = __int21(0x4500, h, 0, 0);\n"
			"  __int21(0x4000, d, 1, (int)\"C\");\n"
			"  __int21(0x3e00, d, 0, 0);\n"
			"  __int21(0x4600, h, 1, 0);\n"
			"  __int21(0x0900, 0, 0, (int)\"D$\");\n"
			"  __int21(0x3e00, h, 0, 0);\n"
			"  h = __int21(0x3c00, 0, 0, (int)\"LONGFILENAME.TEXT\");\n"
			"  __int21(0x3e00, h, 0, 0);\n"
			"  h = __int21(0x3d01, 0, 0, (int)\"NUL\");\n"
			"  __int21(0x4000, h, 5, (int)\"lost!\");\n"
			"  __int21(0x3e00, h, 0, 0);\n"
			"  return 0;\n"
			"}\n";
		std::vector<uint8_t> image;
		check(compile_text(handles, image, "handles.c"), "the handles program compiles");
		if (!image.empty())
		{
			ram_host host;
			machine m;
			run_on(m, image, host);
			check(host.files["C:\\OUT.TXT"] == "ABCD",
			      "a duplicated handle and a forced one share the file and its position");
			check(host.files.count("C:\\LONGFILE.TEX") != 0, "a created name is cut to 8.3");
			check(host.files.count("C:\\NUL") == 0, "NUL is the device, in any directory");
			int open = 0;
			for (const dos_file& file : m.state().dos.files) open += file.references > 0 && !file.permanent ? 1 : 0;
			check(open == 0, "a program's files are all closed when it ends");
		}
	}

	// COMMAND.COM /C, carried out natively: built-in commands, redirection, a
	// program found along PATH and its exit code, a host's own program, and
	// the console a host takes over.
	void test_command_line()
	{
		const char* kid =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"int main(void) {\n"
			"  unsigned char *tail = (unsigned char *)0x80;\n"
			"  __int21(0x4000, 1, tail[0], 0x81);\n"
			"  return 3;\n"
			"}\n";
		const char* parent =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"unsigned __seg(void);\n"
			"extern int __cf;\n"
			"char tail[130];\n"
			"unsigned block[7];\n"
			"int run(char *line) {\n"
			"  int n = 0;\n"
			"  while (line[n] != 0) { tail[n + 1] = line[n]; ++n; }\n"
			"  tail[0] = n; tail[n + 1] = 13;\n"
			"  block[0] = 0; block[1] = (unsigned)tail; block[2] = __seg();\n"
			"  block[3] = 0x5c; block[4] = __seg(); block[5] = 0x6c; block[6] = __seg();\n"
			"  __int21(0x4b00, (int)block, 0, (int)\"C:\\\\COMMAND.COM\");\n"
			"  if (__cf) return 9;\n"
			"  return __int21(0x4d00, 0, 0, 0) & 0xff;\n"
			"}\n"
			"int main(void) {\n"
			"  int code;\n"
			"  run(\" /C echo hello > HELLO.TXT\");\n"
			"  run(\" /C copy /b A.TXT+B.TXT C.TXT\");\n"
			"  run(\" /C ren C.TXT D.TXT\");\n"
			"  run(\" /C del A.TXT\");\n"
			"  run(\" /C if exist D.TXT echo yes> E.TXT\");\n"
			"  run(\" /C if not exist A.TXT type B.TXT > F.TXT\");\n"
			"  code = run(\" /C kid one two>KID.TXT\");\n"
			"  __int21(0x0200, 0, 0, '0' + code);\n"
			"  code = run(\" /C tool x\");\n"
			"  __int21(0x0200, 0, 0, '0' + code);\n"
			"  code = run(\" /C nosuch\");\n"
			"  __int21(0x0200, 0, 0, '0' + code);\n"
			"  return 0;\n"
			"}\n";

		std::vector<uint8_t> kid_image;
		std::vector<uint8_t> image;
		check(compile_text(kid, kid_image, "kid.c"), "the child program compiles");
		check(compile_text(parent, image, "command.c"), "the COMMAND.COM /C program compiles");
		if (kid_image.empty() || image.empty()) return;

		ram_host host;
		host.files["C:\\A.TXT"] = "aa";
		host.files["C:\\B.TXT"] = "bb";
		host.directories.insert("C:\\BIN");
		host.files["C:\\BIN\\KID.COM"].assign(kid_image.begin(), kid_image.end());
		host.natives["TOOL"] = 7;
		host.takes_console = true;

		machine m;
		const char* environment[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\BIN"};
		m.set_environment(environment, 2);
		run_on(m, image, host);
		check(host.files["C:\\HELLO.TXT"] == "hello\r\n", "ECHO with its output redirected to a file");
		check(host.files["C:\\D.TXT"] == "aabb", "COPY /B joins files, and REN renames the result");
		check(host.files.count("C:\\A.TXT") == 0 && host.files.count("C:\\C.TXT") == 0, "DEL deletes");
		check(host.files["C:\\E.TXT"] == "yes\r\n", "IF EXIST runs the rest of the line");
		check(host.files["C:\\F.TXT"] == "bb", "IF NOT EXIST, and TYPE redirected");
		check(host.files["C:\\KID.TXT"] == " one two", "a program found along PATH gets its tail and its redirection");
		check(host.console == "        1 File(s) copied\r\n37Bad command or file name\r\n1",
		      "the exit codes of a program, a host's program and a bad command reach the caller");
		check(host.native_tail == " x", "a host's own program gets the command tail");
		check(host.console.find("Bad command or file name") != std::string::npos, "a bad command is reported");

		// Without a host that has taken it, the console is the screen again.
		machine screen;
		const char* hello =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"int main(void) { __int21(0x0900, 0, 0, (int)\"on the screen$\"); return 0; }\n";
		check(compile_text(hello, image, "screen.c"), "the screen program compiles");
		ram_host plain;
		run_on(screen, image, plain);
		char text[4096] = {};
		screen.text_screen(text, sizeof(text));
		check(strstr(text, "on the screen") != nullptr, "the console stays on the screen by default");
	}

	// What a child is given: its own copy of the environment with its path at
	// the end, AH=47h's directory as DOS returns it, AH=29h, and searches that
	// are abandoned without running the host out of them.
	void test_dos_services()
	{
		const char* waits = "int main(void) { for (;;) {} return 0; }\n";
		const char* parent =
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"unsigned __seg(void);\n"
			"unsigned block[7];\n"
			"char tail[2];\n"
			"int main(void) {\n"
			"  tail[0] = 0; tail[1] = 13;\n"
			"  block[0] = 0; block[1] = (unsigned)tail; block[2] = __seg();\n"
			"  block[3] = 0x5c; block[4] = __seg(); block[5] = 0x6c; block[6] = __seg();\n"
			"  __int21(0x4b00, (int)block, 0, (int)\"C:\\\\SUB\\\\WAITS.COM\");\n"
			"  return 0;\n"
			"}\n";
		std::vector<uint8_t> waits_image;
		std::vector<uint8_t> image;
		check(compile_text(waits, waits_image, "waits.c"), "the waiting child compiles");
		check(compile_text(parent, image, "envparent.c"), "the environment parent compiles");
		if (!waits_image.empty() && !image.empty())
		{
			ram_host host;
			host.directories.insert("C:\\SUB");
			host.files["C:\\SUB\\WAITS.COM"].assign(waits_image.begin(), waits_image.end());
			machine m;
			const char* environment[] = {"COMSPEC=C:\\COMMAND.COM", "TOOL=1"};
			m.set_environment(environment, 2);
			m.set_host(&host);
			m.load_program(image.data(), uint32_t(image.size()), "C:\\TEST.COM", "");
			for (int frame = 0; frame < 20; ++frame) m.exec(m.cycles_per_frame());

			machine_state& s = m.state();
			const uint16_t child = s.dos.psp_segment;
			const uint16_t child_environment = read16(s, child, 0x2c);
			const uint16_t parent_environment = read16(s, read16(s, child, 0x16), 0x2c);
			std::string strings;
			uint16_t offset = 0;
			while (offset < 400 && !(read8(s, child_environment, offset) == 0 && strings.size() > 0 && strings.back() == '\n'))
			{
				const uint8_t ch = read8(s, child_environment, offset++);
				strings += ch == 0 ? '\n' : char(ch);
			}
			char path[64] = {};
			read_string(s, child_environment, uint16_t(offset + 3), path, sizeof(path));
			check(child_environment != parent_environment, "a child gets its own copy of the environment");
			check(strings.find("TOOL=1") != std::string::npos, "the copy has the parent's variables");
			check(strcmp(path, "C:\\SUB\\WAITS.COM") == 0, "and the child's own path after them");
		}

		// AH=47h: no drive and no leading backslash.
		{
			ram_host host;
			host.directories.insert("C:\\SUB");
			machine m;
			m.set_host(&host);
			const uint8_t com[] = {0xcd, 0x20};
			m.load_program(com, sizeof(com), "C:\\T.COM", "");
			check(m.set_current_directory("C:\\SUB"), "the current directory can be set");
			machine_state& s = m.state();
			s.cpu.set_ah(0x47);
			s.cpu.set_dl(0);
			s.cpu.set_ds(0x2000);
			s.cpu.set_si(0);
			// The service writes its answer and the status into an IRET frame.
			s.cpu.set_ss(0x3000);
			s.cpu.set_sp(0x100);
			dos_interrupt(s, 0x21);
			char text[64] = {};
			read_string(s, 0x2000, 0, text, sizeof(text));
			check(strcmp(text, "SUB") == 0, "AH=47h gives the directory without a leading backslash");
		}

		// AH=29h: the drive, the name and the extension, padded with spaces.
		{
			machine m;
			const uint8_t com[] = {0xcd, 0x20};
			m.load_program(com, sizeof(com), "C:\\T.COM", "");
			machine_state& s = m.state();
			const char* name = " a:file.t*";
			for (int i = 0; name[i] != 0 || i == 0; ++i) write8(s, 0x2000, uint16_t(i), uint8_t(name[i]));
			write8(s, 0x2000, uint16_t(std::strlen(name)), 0);
			s.cpu.set_ax(0x2901);
			s.cpu.set_ds(0x2000);
			s.cpu.set_si(0);
			s.cpu.set_es(0x2000);
			s.cpu.set_di(0x100);
			dos_interrupt(s, 0x21);
			char fcb[13] = {};
			for (int i = 0; i < 12; ++i) fcb[i] = char(read8(s, 0x2000, uint16_t(0x100 + i)));
			check(fcb[0] == 1 && std::memcmp(fcb + 1, "FILE    T??", 11) == 0, "AH=29h parses a drive, a name and a wildcard");
			check_equal(s.cpu.al(), 1, "AH=29h reports the wildcard");
		}

		// A program that only asks whether files exist never finishes its
		// searches; the host still does not run out of them.
		{
			const char* probe =
				"int __int21(int ax, int bx, int cx, int dx);\n"
				"extern int __cf;\n"
				"int main(void) {\n"
				"  int i;\n"
				"  int found = 0;\n"
				"  for (i = 0; i < 40; ++i) { __int21(0x4e00, 0, 0, (int)\"*.TXT\"); if (!__cf) ++found; }\n"
				"  for (i = 0; i < 40; ++i) { __int21(0x4e00, 0, 0, (int)\"ONE.TXT\"); if (!__cf) ++found; }\n"
				"  __int21(0x0200, 0, 0, found == 80 ? 'Y' : 'N');\n"
				"  return 0;\n"
				"}\n";
			check(compile_text(probe, image, "probe.c"), "the search program compiles");
			ram_host host;
			host.files["C:\\ONE.TXT"] = "1";
			host.files["C:\\TWO.TXT"] = "2";
			host.takes_console = true;
			machine m;
			run_on(m, image, host);
			check(host.console == "Y", "abandoned searches do not run the host out of searches");
			check(host.open_searches() <= dos_max_searches, "no more searches are left open than DOS keeps");
		}
	}

	// The processor models: the 8086 ones stop at an instruction an 8088
	// does not have, and behave like an 8086 where it differs.
	void test_cpu_models()
	{
		{
			machine m;
			m.set_cpu_model(cpu_model::i8086_strict);
			const uint8_t pusha[] = {0x60};
			load_code(m, pusha, sizeof(pusha));
			step(m, 1);
			check(m.faulted() && strstr(m.error(), "60h") != nullptr, "an 8086 stops at PUSHA");
		}
		{
			machine m;
			m.set_cpu_model(cpu_model::i8086_strict);
			const uint8_t escape[] = {0xd9, 0xe8}; // fld1
			load_code(m, escape, sizeof(escape));
			step(m, 1);
			check(m.faulted(), "the strict model stops at a coprocessor instruction");
		}
		{
			machine m;
			m.set_cpu_model(cpu_model::i8086);
			const uint8_t escape[] = {0xd9, 0xe8, 0x90};
			load_code(m, escape, sizeof(escape));
			step(m, 2);
			check(!m.faulted(), "a plain 8086 executes an escape as a no-op");
		}
		for (const cpu_model model : {cpu_model::i8086, cpu_model::i386})
		{
			machine m;
			m.set_cpu_model(model);
			// push sp / pop ax / mov cl,21h / mov bx,1 / shl bx,cl
			const uint8_t code[] = {0x54, 0x58, 0xb1, 0x21, 0xbb, 0x01, 0x00, 0xd3, 0xe3};
			load_code(m, code, sizeof(code));
			step(m, 5);
			const cpu_state& c = m.state().cpu;
			const bool is_8086 = model == cpu_model::i8086;
			check_equal(c.ax(), is_8086 ? 0xfffc : 0xfffe, is_8086 ? "an 8086 pushes SP after the decrement"
			                                                     : "a 386 pushes SP as it was");
			check_equal(c.bx(), is_8086 ? 0 : 2, is_8086 ? "an 8086 shifts by all of CL" : "a 386 shifts by CL mod 32");
			check(!m.faulted(), "neither stops");
		}
	}

	// An image in memory that holds only the sectors written; the rest read
	// as zeros, so a test disk can be gigabytes.
	class sector_map final : public disk_sink, public disk_source
	{
	public:
		bool size(uint64_t bytes) override
		{
			length = bytes;
			sectors.clear();
			return true;
		}

		bool write(uint64_t offset, const uint8_t* data, size_t count) override
		{
			if (offset + count > length) return false;
			for (size_t done = 0; done < count;)
			{
				const uint64_t at = offset + done;
				const size_t within = size_t(at % disk_sector_size);
				const size_t part = std::min<size_t>(count - done, disk_sector_size - within);
				std::vector<uint8_t>& block = sectors[at / disk_sector_size];
				block.resize(disk_sector_size);
				memcpy(block.data() + within, data + done, part);
				done += part;
			}
			return true;
		}

		bool read(uint64_t offset, uint8_t* data, size_t count) override
		{
			if (offset + count > length) return false;
			for (size_t done = 0; done < count;)
			{
				const uint64_t at = offset + done;
				const size_t within = size_t(at % disk_sector_size);
				const size_t part = std::min<size_t>(count - done, disk_sector_size - within);
				const auto found = sectors.find(at / disk_sector_size);
				if (found == sectors.end())
					memset(data + done, 0, part);
				else
					memcpy(data + done, found->second.data() + within, part);
				done += part;
			}
			return true;
		}

		std::vector<uint8_t> sector(uint64_t number)
		{
			std::vector<uint8_t> data(disk_sector_size);
			read(number * disk_sector_size, data.data(), data.size());
			return data;
		}

	private:
		uint64_t length = 0;
		std::map<uint64_t, std::vector<uint8_t>> sectors;
	};

	uint32_t le32(const uint8_t* p)
	{
		return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
	}

	disk_file test_file(const char* name, size_t size, bool system)
	{
		disk_file file;
		file.name = name;
		file.system = system;
		for (size_t i = 0; i < size; ++i) file.data.push_back(uint8_t(i * 7 + size));
		return file;
	}

	// dimg's images: a floppy and hard disks laid out as DOS 4.0's FORMAT and
	// FDISK lay them out, and the reader that finds the files again.
	void test_disk_images()
	{
		uint8_t boot[disk_sector_size] = {0xeb, 0x3c, 0x90, 'M', 'S', 'D', 'O', 'S', '4', '.', '0'};
		boot[510] = 0x55;
		boot[511] = 0xaa;
		uint8_t master[disk_sector_size] = {0xfa, 0x33, 0xc0};
		master[510] = 0x55;
		master[511] = 0xaa;
		const std::vector<disk_file> files = {
			test_file("bin\\io.sys", 3000, true), test_file("msdos.sys", 1500, true),
			test_file("command.com", 25000, false), test_file("empty.txt", 0, false),
			test_file("readme.long", 600, false),
		};
		disk_stamp stamp;
		stamp.date = 8 << 9 | 6 << 5 | 15; // 1988-06-15
		stamp.time = 12 << 11 | 34 << 5 | 28;
		stamp.serial = 0x12345678;

		{
			uint8_t name[11];
			disk_name_83("dir\\verylongname.text", name);
			check(memcmp(name, "VERYLONGTEX", 11) == 0, "an 8.3 name is cut to eight and three, upper case");
			disk_name_83("a.b.c", name);
			check(memcmp(name, "A.B     C  ", 11) == 0, "an 8.3 name's extension follows the last dot");
		}

		// A 360 KB floppy: IO.SYS and MSDOS.SYS first, then the label.
		std::vector<uint8_t> floppy;
		disk_report report;
		std::string error;
		check(disk_build_floppy(360, boot, files, "test disk", stamp, floppy, report, error), "a 360 KB floppy is built");
		check_equal(uint32_t(floppy.size()), 368640, "a 360 KB floppy is 720 sectors");
		check(floppy[13] == 2 && floppy[17] == 112 && floppy[21] == 0xfd && floppy[22] == 2 && floppy[24] == 9,
		      "a 360 KB floppy's parameter block");
		check(memcmp(&floppy[43], "TEST DISK  ", 11) == 0 && memcmp(&floppy[54], "FAT12   ", 8) == 0,
		      "the boot sector names the label and FAT12");
		check_equal(le32(&floppy[39]), 0x12345678, "a floppy's serial number is the stamp's");
		const uint8_t* root = &floppy[5 * disk_sector_size];
		check(memcmp(root, "IO      SYS", 11) == 0 && root[11] == 0x07 && root[26] == 2, "IO.SYS is first, a system file at cluster 2");
		check(memcmp(root + 32, "MSDOS   SYS", 11) == 0 && root[43] == 0x07 && root[58] == 5, "MSDOS.SYS follows it");
		check(memcmp(root + 64, "TEST DISK  ", 11) == 0 && root[75] == 0x08, "the label is third");
		check_equal(uint32_t(root[86] | root[87] << 8), stamp.time, "the label is dated");
		check(memcmp(root + 128, "EMPTY   TXT", 11) == 0 && root[154] == 0 && le32(root + 156) == 0,
		      "an empty file has an entry and no cluster");
		const uint8_t* fat = &floppy[disk_sector_size];
		check(fat[0] == 0xfd && fat[1] == 0xff && fat[2] == 0xff && fat[3] == 0x03 && (fat[6] | (fat[7] & 0x0f) << 8) == 0xfff,
		      "the FAT holds the media byte and IO.SYS's chain");
		check(memcmp(fat, &floppy[3 * disk_sector_size], 2 * disk_sector_size) == 0, "both FATs match");
		check_equal(report.volumes[0].free_clusters, 354 - 31, "the floppy's free clusters");

		// Read it back, with a subdirectory added by hand: SUB at cluster 33
		// holding INNER.TXT at 34 and a deleted entry.
		const auto cluster_at = [](uint32_t cluster) { return size_t(12 + (cluster - 2) * 2) * disk_sector_size; };
		const auto set_fat12 = [&](uint32_t cluster, uint32_t value) {
			uint8_t* p = &floppy[disk_sector_size + cluster + cluster / 2];
			if (cluster & 1)
			{
				p[0] = uint8_t((p[0] & 0x0f) | (value & 0x0f) << 4);
				p[1] = uint8_t(value >> 4);
			}
			else
			{
				p[0] = uint8_t(value);
				p[1] = uint8_t((p[1] & 0xf0) | (value >> 8 & 0x0f));
			}
		};
		uint8_t* sub = &floppy[5 * disk_sector_size + 6 * 32];
		memcpy(sub, "SUB        ", 11);
		sub[11] = 0x10;
		sub[26] = 33;
		uint8_t* listing = &floppy[cluster_at(33)];
		memcpy(listing, ".          ", 11);
		listing[11] = 0x10;
		listing[26] = 33;
		memcpy(listing + 32, "..         ", 11);
		listing[43] = 0x10;
		memcpy(listing + 64, "\xe5" "DELETEDTXT", 11);
		memcpy(listing + 96, "INNER   TXT", 11);
		listing[96 + 11] = 0x20;
		listing[96 + 26] = 34;
		listing[96 + 28] = 5;
		memcpy(&floppy[cluster_at(34)], "hello", 5);
		set_fat12(33, 0xfff);
		set_fat12(34, 0xfff);

		sector_map image;
		image.size(floppy.size());
		image.write(0, floppy.data(), floppy.size());
		std::vector<disk_volume> volumes;
		check(disk_volumes(image, volumes, error) && volumes.size() == 1, "a floppy is one volume");
		check(volumes[0].formatted && !volumes[0].fat16 && volumes[0].type == 0 && volumes[0].label == "TEST DISK" &&
		          volumes[0].clusters == 354 && volumes[0].free_clusters == 354 - 33,
		      "the floppy's volume, label and free space are found");
		std::vector<disk_entry> entries;
		check(disk_list(image, volumes[0], entries, error), "the floppy is listed");
		std::string paths;
		for (const disk_entry& e : entries) paths += e.path + (e.attributes & 0x10 ? "/ " : " ");
		check(paths == "\\IO.SYS \\MSDOS.SYS \\COMMAND.COM \\EMPTY.TXT \\README.LON \\SUB/ \\SUB\\INNER.TXT ",
		      "the listing names every file and directory, and no label or deleted entry");
		check(entries[2].size == 25000 && entries[2].date == stamp.date && entries[2].time == stamp.time,
		      "an entry's size and date are listed");
		std::vector<uint8_t> data;
		check(disk_read_file(image, volumes[0], "\\command.com", data, error) && data == files[2].data,
		      "a file is read back whole");
		check(disk_read_file(image, volumes[0], "sub/inner.txt", data, error) && data.size() == 5 &&
		          memcmp(data.data(), "hello", 5) == 0,
		      "a file in a subdirectory is read back");
		check(disk_read_file(image, volumes[0], "EMPTY.TXT", data, error) && data.empty(), "an empty file reads as nothing");
		check(!disk_read_file(image, volumes[0], "\\deleted.txt", data, error), "a deleted file is not found");
		check(!disk_read_file(image, volumes[0], "\\sub", data, error) && error.find("directory") != std::string::npos,
		      "a directory is not read as a file");

		std::vector<disk_file> too_many;
		for (int i = 0; i < 120; ++i) too_many.push_back(test_file(("f" + std::to_string(i)).c_str(), 1, false));
		check(!disk_build_floppy(360, boot, too_many, "", stamp, floppy, report, error) &&
		          error.find("root directory") != std::string::npos,
		      "a 360 KB floppy's root directory holds 112 entries");
		check(!disk_build_floppy(360, boot, {test_file("big", 400000, false)}, "", stamp, floppy, report, error) &&
		          error.find("full") != std::string::npos,
		      "a file bigger than the floppy is refused");

		// A 20 MB disk with 5 MB partitions: C: and three logical drives, all
		// FAT12, on 16 heads and 63 sectors a track.
		disk_layout layout;
		layout.megabytes = 20;
		layout.partition_megabytes = 5;
		check(disk_build_hard_disk(layout, master, boot, files, "dos4z", stamp, image, report, error),
		      "a 20 MB hard disk is built");
		check(report.cylinders == 40 && report.heads == 16 && report.sectors_per_track == 63 && report.sectors == 40320,
		      "the disk is whole cylinders");
		check(report.volumes.size() == 4 && report.volumes[0].start == 63 && report.volumes[0].sectors == 10017 &&
		          report.volumes[0].type == 0x01 && report.volumes[0].sectors_per_cluster == 8,
		      "C: is a cylinder-aligned FAT12 partition");
		std::vector<uint8_t> mbr = image.sector(0);
		check(mbr[0] == 0xfa && mbr[510] == 0x55, "the master boot record keeps its code");
		check(mbr[0x1be] == 0x80 && mbr[0x1bf] == 1 && mbr[0x1c0] == 1 && mbr[0x1c1] == 0 && mbr[0x1c2] == 0x01 &&
		          le32(&mbr[0x1c6]) == 63 && le32(&mbr[0x1ca]) == 10017,
		      "C: is active, at head 1 of cylinder 0");
		check(mbr[0x1d2] == 0x05 && le32(&mbr[0x1d6]) == 10080 && le32(&mbr[0x1da]) == 30240,
		      "the extended partition holds the rest of the disk");
		std::vector<uint8_t> ebr = image.sector(10080);
		check(ebr[0x1c2] == 0x01 && le32(&ebr[0x1c6]) == 63 && le32(&ebr[0x1ca]) == 10017 && ebr[0x1d2] == 0x05 &&
		          le32(&ebr[0x1d6]) == 10080 && le32(&ebr[0x1da]) == 10080,
		      "D:'s EBR places D: and links the next EBR, relative to the extended partition");
		check(image.sector(30240)[0x1d2] == 0, "the last EBR links nothing");
		std::vector<uint8_t> c_boot = image.sector(63);
		check(le32(&c_boot[28]) == 63 && c_boot[36] == 0x80 && le32(&c_boot[39]) == 0x12345678 + 63 &&
		          c_boot[21] == 0xf8 && memcmp(&c_boot[43], "DOS4Z      FAT12   ", 19) == 0,
		      "C:'s boot sector counts the sectors before it and is drive 80h");

		check(disk_volumes(image, volumes, error) && volumes.size() == 4, "the reader finds all four volumes");
		check(volumes[0].label == "DOS4Z" && volumes[1].label == "DRIVE D" && volumes[3].label == "DRIVE F" &&
		          volumes[3].start == 30303 && volumes[3].type == 0x01,
		      "the logical drives follow the EBR chain in order");
		check(disk_read_file(image, volumes[0], "\\IO.SYS", data, error) && data == files[0].data, "C: holds IO.SYS");
		check(disk_list(image, volumes[2], entries, error) && entries.empty(), "a logical drive is empty");

		// FAT16 partition types: 04h up to 32 MB, 06h beyond, 0Eh and 0Fh with LBA.
		layout.megabytes = 40;
		layout.partition_megabytes = 20;
		layout.lba_types = true;
		check(disk_build_hard_disk(layout, master, boot, {}, "", stamp, image, report, error) &&
		          report.volumes.size() == 3 && report.volumes[0].type == 0x0e && report.volumes[1].type == 0x0e &&
		          report.volumes[2].type == 0x01 && report.volumes[0].sectors_per_cluster == 4 && report.volumes[0].fat16,
		      "LBA types: FAT16 partitions are 0Eh, a small one is still 01h");
		check(image.sector(0)[0x1d2] == 0x0f, "the extended partition is 0Fh with LBA");
		check(disk_volumes(image, volumes, error) && volumes.size() == 3 && volumes[0].fat16 && !volumes[2].fat16,
		      "the reader tells FAT16 from FAT12 by the cluster count");
		layout.lba_types = false;
		check(disk_build_hard_disk(layout, master, boot, {}, "", stamp, image, report, error) &&
		          report.volumes[0].type == 0x04,
		      "a FAT16 partition under 32 MB is 04h");
		layout.megabytes = 64;
		layout.partition_megabytes = 64;
		check(disk_build_hard_disk(layout, master, boot, {}, "", stamp, image, report, error) &&
		          report.volumes.size() == 1 && report.volumes[0].type == 0x06 && report.volumes[0].sectors == 130977,
		      "a partition over 32 MB is 06h");

		// 8.5 GB: 255 heads, and partitions past cylinder 1023 get the
		// largest CHS address.
		layout.megabytes = 8500;
		layout.partition_megabytes = 2047;
		layout.lba_types = true;
		check(disk_build_hard_disk(layout, master, boot, {}, "", stamp, image, report, error) &&
		          report.heads == 255 && report.cylinders == 1083 && report.volumes.size() == 5 &&
		          report.volumes[0].sectors_per_cluster == 64 && report.volumes[4].sectors_per_cluster == 16,
		      "an 8.5 GB disk is five volumes, the first with 32 KB clusters");
		std::vector<uint8_t> last = image.sector(report.volumes[4].start - 63);
		check(last[0x1bf] == 0xfe && last[0x1c0] == 0xff && last[0x1c1] == 0xff,
		      "a partition beyond cylinder 1023 has the largest CHS address");
		check(disk_volumes(image, volumes, error) && volumes.size() == 5 && volumes[4].label == "DRIVE G",
		      "the reader follows the chain past cylinder 1023");
		layout.megabytes = 15;
		layout.heads = 255;
		check(!disk_build_hard_disk(layout, master, boot, {}, "", stamp, image, report, error),
		      "a disk under two cylinders is refused");
	}

	// A drive in memory for boot mode.
	class ram_drive final : public block_device
	{
	public:
		std::vector<uint8_t> bytes;

		explicit ram_drive(size_t sectors) : bytes(sectors * 512)
		{
		}

		uint64_t sectors() override
		{
			return bytes.size() / 512;
		}

		bool read(uint64_t lba, uint8_t* data, uint32_t count) override
		{
			if ((lba + count) * 512 > bytes.size()) return false;
			memcpy(data, &bytes[size_t(lba) * 512], size_t(count) * 512);
			return true;
		}

		bool write(uint64_t lba, const uint8_t* data, uint32_t count) override
		{
			if ((lba + count) * 512 > bytes.size()) return false;
			memcpy(&bytes[size_t(lba) * 512], data, size_t(count) * 512);
			return true;
		}
	};

	// Boot mode: the machine loads a boot sector and runs it on the BIOS, with
	// no DOS of its own.
	void test_boot_mode()
	{
		// Prints 'B', reads sector 2 by CHS and prints its first byte, asks
		// for the LBA calls and prints BH (AAh if they are there), reads LBA 3
		// through the packet at 7C80h and prints its first byte, then halts.
		const uint8_t program[] = {
			0x31, 0xc0, 0x8e, 0xd8, 0x8e, 0xc0,             // xor ax,ax / mov ds,ax / mov es,ax
			0xb8, 0x42, 0x0e, 0xcd, 0x10,                   // mov ax,0E42h / int 10h
			0xb8, 0x01, 0x02, 0xbb, 0x00, 0x06,             // mov ax,0201h / mov bx,0600h
			0xb9, 0x02, 0x00, 0xb6, 0x00, 0xcd, 0x13,       // mov cx,2 / mov dh,0 / int 13h
			0xa0, 0x00, 0x06, 0xb4, 0x0e, 0xcd, 0x10,       // mov al,[0600h] / mov ah,0Eh / int 10h
			0xb4, 0x41, 0xbb, 0xaa, 0x55, 0xcd, 0x13,       // mov ah,41h / mov bx,55AAh / int 13h
			0x88, 0xf8, 0xb4, 0x0e, 0xcd, 0x10,             // mov al,bh / mov ah,0Eh / int 10h
			0xbe, 0x80, 0x7c, 0xb4, 0x42, 0xcd, 0x13,       // mov si,7C80h / mov ah,42h / int 13h
			0xa0, 0x00, 0x08, 0xb4, 0x0e, 0xcd, 0x10,       // mov al,[0800h] / mov ah,0Eh / int 10h
			0xf4, 0xeb, 0xfd,                               // hlt / jmp $-1
		};
		const uint8_t packet[16] = {0x10, 0, 1, 0, 0x00, 0x08, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0};
		const auto make_disk = [&](ram_drive& disk) {
			memcpy(disk.bytes.data(), program, sizeof(program));
			memcpy(&disk.bytes[0x80], packet, sizeof(packet));
			disk.bytes[510] = 0x55;
			disk.bytes[511] = 0xaa;
			disk.bytes[512] = 'C';
			disk.bytes[3 * 512] = 'D';
		};
		const auto run = [](machine& m, ram_host& host, size_t characters) {
			for (int i = 0; i < 400 && host.video.size() < characters && !m.faulted(); ++i) m.exec(1000);
		};

		{
			ram_host host;
			ram_drive disk(20 * 16 * 63);
			make_disk(disk);
			machine m;
			m.set_host(&host);
			m.set_cpu_model(cpu_model::i8086);
			check(m.attach_disk(0x80, &disk, {20, 16, 63}, true), "a hard disk is attached");
			check(m.power_on(0x80), "the machine boots from it");
			run(m, host, 4);
			check(host.video == "BC\xaa" "D", "the boot sector reads by CHS, finds the LBA calls and reads by LBA");
			machine_state& s = m.state();
			check(read16(s, 0, 0x21 * 4) == 0 && read16(s, 0, 0x21 * 4 + 2) == 0 && read16(s, 0, 0x33 * 4 + 2) == 0,
			      "booted, no DOS and no mouse driver are mapped in");
			check(s.memory[0xffffe] == 0xff && memcmp(&s.memory[0xffff5], "10/27/82", 8) == 0,
			      "the ROM is an IBM PC's");
			check(read16(s, 0, 0x1e * 4 + 2) == 0xf000 && s.memory[0xfefc7 + 4] == 9,
			      "INT 1Eh points at the diskette parameters");
			check(read8(s, bda_segment, 0x75) == 1 && (read16(s, bda_segment, 0x10) & 0xc1) == 0x01,
			      "the BIOS counts one hard disk and one floppy drive");

			// mov ah,08h / mov dl,80h / int 13h / hlt
			const uint8_t parameters[] = {0xb4, 0x08, 0xb2, 0x80, 0xcd, 0x13, 0xf4};
			load_code(m, parameters, sizeof(parameters));
			s.pic.master.irr = 0; // no timer tick in the middle of the steps
			step(m, 8);
			check(s.cpu.cx() == 0x133f && s.cpu.dx() == 0x0f01 && !s.cpu.cf(),
			      "AH=08h gives the last cylinder, sector, head and the number of disks");

			// jmp far FFFF:0000, which is how FDISK restarts the machine
			const uint8_t restart[] = {0xea, 0x00, 0x00, 0xff, 0xff};
			load_code(m, restart, sizeof(restart));
			s.pic.master.irr = 0;
			step(m, 2);
			check(s.cpu.cs() == 0 && s.cpu.ip == 0x7c00 && s.memory[0x7c00] == 0x31,
			      "a jump to FFFF:0000 starts the machine again from the disk");
		}
		{
			ram_host host;
			ram_drive disk(20 * 16 * 63);
			make_disk(disk);
			machine m;
			m.set_host(&host);
			m.attach_disk(0x80, &disk, {20, 16, 63}, false);
			m.power_on(0x80);
			run(m, host, 4);
			check(host.video == std::string("BCU\0", 4), "with the LBA calls switched off, AH=41h and AH=42h fail");
		}
		{
			ram_host host;
			ram_drive floppy(2880);
			make_disk(floppy);
			floppy.bytes[510] = floppy.bytes[511] = 0;
			machine m;
			m.set_host(&host);
			m.attach_disk(0x00, &floppy, {80, 2, 18}, true);
			check(m.power_on(0x00), "a floppy boots without the 55AAh signature");
			run(m, host, 4);
			check(host.video == std::string("BCU\0", 4), "a floppy has CHS only");

			// mov ah,08h / mov dl,00h / int 13h / hlt
			const uint8_t parameters[] = {0xb4, 0x08, 0xb2, 0x00, 0xcd, 0x13, 0xf4};
			load_code(m, parameters, sizeof(parameters));
			m.state().pic.master.irr = 0;
			step(m, 8);
			const cpu_state& c = m.state().cpu;
			check(c.bl() == 4 && c.cx() == 0x4f12 && c.dx() == 0x0101 && c.es() == 0xf000 && c.di() == 0xefc7,
			      "AH=08h reports a 1.44 MB drive and its parameter table");
		}
		{
			machine m;
			check(!m.power_on(0x80) && strstr(m.error(), "nothing to boot") != nullptr,
			      "with no disk there is nothing to boot");
		}

		// cc's INT 13h thunk, as DISKINFO uses it. Mapped, there is no disk:
		// a reset works and anything else is a bad command, with the carry set.
		const char* disk_program =
			"int __int13(int ax, int bx, int cx, int dx);\n"
			"int __int21(int ax, int bx, int cx, int dx);\n"
			"extern int __cf;\n"
			"int main(void) {\n"
			"  int ax;\n"
			"  __int13(0x0000, 0, 0, 0x80);\n"
			"  __int21(0x0200, 0, 0, __cf == 0 ? 'R' : '!');\n"
			"  ax = __int13(0x0800, 0, 0, 0x80);\n"
			"  __int21(0x0200, 0, 0, __cf != 0 && (ax >> 8) == 1 ? 'F' : '!');\n"
			"  return 0;\n"
			"}\n";
		std::vector<uint8_t> image;
		check(compile_text(disk_program, image, "disk.c"), "a program calling __int13 compiles");
		if (!image.empty())
			check(run_image(image, nullptr).find("RF") != std::string::npos, "__int13 reaches INT 13h and reports the carry");
	}

	int run_tests()
	{
		test_cpu();
		test_cpu_386();
		test_interrupts();
		test_timer();
		test_video();
		test_dos_memory();
		test_loader();
		test_program_run();
		test_line_input();
		test_console_keys();
		test_sound();
		test_shell();
		test_shell_programs();
		test_support();
		for (const cc_cpu cpu : {cc_cpu_i8086, cc_cpu_i80386})
		{
			compiling_for = cpu;
			test_compiler();
		}
		compiling_for = cc_cpu_i8086;
		test_compiler_refusals();
		test_native_backend();
		test_keyboard();
		test_disassembler();
		test_dos_handles();
		test_command_line();
		test_dos_services();
		test_cpu_models();
		test_disk_images();
		test_boot_mode();

		printf("%d checks, %d failures\n", checks, failures);
		return failures == 0 ? 0 : 1;
	}

	// Supplies #include files to the compiler from the directory the source
	// being compiled lives in, then from any directory given with -I.
	struct directory_sources
	{
		cc_sources base; // first, so the callback can cast its way back here
		std::vector<std::string> directories;
		std::vector<uint8_t> contents; // the file being handed over, alive until the next one
	};

	int load_from_directory(cc_sources* self, const char* name, const void** bytes, size_t* size)
	{
		directory_sources* sources = reinterpret_cast<directory_sources*>(self);
		for (const std::string& directory : sources->directories)
		{
			char path[MAX_PATH] = {};
			snprintf(path, sizeof(path), "%s\\%s", directory.c_str(), name);
			uint8_t* data = nullptr;
			uint32_t length = 0;
			if (!read_file(path, data, length)) continue;
			sources->contents.assign(data, data + length);
			delete[] data;
			*bytes = sources->contents.data();
			*size = sources->contents.size();
			return 1;
		}
		return 0;
	}

	// Compiles a C file and returns the .COM image, reporting any error.
	bool compile_file(const char* path, const std::vector<std::string>& includes,
	                  const std::vector<std::string>& defines, std::vector<uint8_t>& image, bool quiet,
	                  const cc_options& options = default_options(), bool map = false)
	{
		uint8_t* data = nullptr;
		uint32_t size = 0;
		if (!read_file(path, data, size))
		{
			printf("Cannot read %s\n", path);
			return false;
		}
		const std::string text(reinterpret_cast<const char*>(data), size);
		delete[] data;

		char directory[MAX_PATH] = {};
		char name[MAX_PATH] = {};
		split_path(path, directory, MAX_PATH, name, MAX_PATH);

		directory_sources sources;
		sources.base.load = load_from_directory;
		sources.directories.emplace_back(directory);
		for (const std::string& entry : includes) sources.directories.push_back(entry);

		std::vector<const char*> define_text;
		for (const std::string& entry : defines) define_text.push_back(entry.c_str());

		cc_result result = cc_compile(text.c_str(), name, &sources.base,
		                              define_text.empty() ? nullptr : define_text.data(),
		                              int(define_text.size()), &options);
		if (map && result.function_count > 0)
		{
			std::vector<std::pair<std::string, int>> sizes;
			for (int i = 0; i < result.function_count; ++i)
				sizes.emplace_back(result.functions[i].name, result.functions[i].size);
			std::sort(sizes.begin(), sizes.end(),
			          [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b)
			          {
				          return a.second > b.second;
			          });
			int total = 0;
			for (const auto& entry : sizes) total += entry.second;
			printf("%d functions, %d bytes of them\n", int(sizes.size()), total);
			for (const auto& entry : sizes) printf("  %6d  %s\n", entry.second, entry.first.c_str());
		}
		if (!result.ok)
		{
			printf("%s(%d): %s\n", result.file != nullptr ? result.file : name, result.line,
			       result.message != nullptr ? result.message : "");
			cc_result_free(&result);
			return false;
		}
		if (!quiet)
			printf("%s: %d bytes (%d code, %d data)\n", name, result.image_size,
			       result.code_size, result.data_size);
		image.assign(result.image, result.image + result.image_size);
		cc_result_free(&result);
		return true;
	}

	int compile_command(const char* path, const char* output, const std::vector<std::string>& includes,
	                    const std::vector<std::string>& defines, const cc_options& options, bool map)
	{
		std::vector<uint8_t> image;
		if (!compile_file(path, includes, defines, image, false, options, map)) return 1;

		char target[MAX_PATH] = {};
		if (output != nullptr) snprintf(target, sizeof(target), "%s", output);
		else
		{
			snprintf(target, sizeof(target), "%s", path);
			char* dot = strrchr(target, '.');
			if (dot != nullptr) snprintf(dot, sizeof(target) - size_t(dot - target), ".COM");
		}

		FILE* file = nullptr;
		if (fopen_s(&file, target, "wb") != 0 || file == nullptr)
		{
			printf("Cannot write %s\n", target);
			return 1;
		}
		fwrite(image.data(), 1, image.size(), file);
		fclose(file);
		printf("Wrote %s\n", target);
		return 0;
	}

}

int show_info(const char* path)
{
	uint8_t* data = nullptr;
	uint32_t size = 0;
	if (!read_file(path, data, size))
	{
		printf("Cannot read %s\n", path);
		return 1;
	}

	printf("%s, %u bytes\n", path, size);
	// The loader takes any file that starts with MZ for an executable, so
	// one too short to have a header is described as a broken one, not as
	// a COM file it will never be run as.
	if (size >= 2 && size < 0x20 && data[0] == 'M' && data[1] == 'Z')
	{
		printf("  format             MZ executable with a truncated header; it will not load\n");
	}
	else if (size >= 0x20 && data[0] == 'M' && data[1] == 'Z')
	{
		const auto word = [&](uint32_t offset) { return uint16_t(data[offset] | (data[offset + 1] << 8)); };
		const uint32_t image_length = uint32_t(word(0x04)) * 512 -
			(word(0x02) != 0 ? (512 - word(0x02)) : 0);
		printf("  format             MZ executable\n");
		printf("  image size         %u bytes (%u header, %u body)\n", image_length,
		       unsigned(word(0x08)) * 16, image_length - unsigned(word(0x08)) * 16);
		printf("  relocations        %u at %04Xh\n", word(0x06), word(0x18));
		printf("  extra paragraphs   min %u, max %u\n", word(0x0a), word(0x0c));
		printf("  entry point        CS:IP %04X:%04X\n", word(0x16), word(0x14));
		printf("  initial stack      SS:SP %04X:%04X\n", word(0x0e), word(0x10));
		printf("  memory needed      %u KiB\n",
		       (image_length - unsigned(word(0x08)) * 16 + unsigned(word(0x0a)) * 16 + 1023) / 1024);
		if (word(0x18) >= 0x40 && size >= 0x40)
		{
			const uint32_t extended = uint32_t(data[0x3c] | (data[0x3d] << 8) |
				(data[0x3e] << 16) | (data[0x3f] << 24));
			if (extended + 2 < size && data[extended] == 'N' && data[extended + 1] == 'E')
				printf("  note               contains a Windows NE image\n");
		}
	}
	else
	{
		printf("  format             COM (loaded at PSP:0100)\n");
	}
	delete[] data;
	return 0;
}

namespace
{
	int disassemble_file(const char* path, int count)
	{
		uint8_t* data = nullptr;
		uint32_t size = 0;
		if (!read_file(path, data, size))
		{
			printf("Cannot read %s\n", path);
			return 1;
		}

		machine m;
		char directory[MAX_PATH] = {};
		char name[MAX_PATH] = {};
		split_path(path, directory, MAX_PATH, name, MAX_PATH);
		char dos_path[160] = {};
		snprintf(dos_path, sizeof(dos_path), "C:\\%s", name);
		if (!m.load_program(data, size, dos_path, ""))
		{
			printf("%s\n", m.error());
			delete[] data;
			return 1;
		}

		machine_state& s = m.state();
		uint16_t offset = s.cpu.ip;
		for (int i = 0; i < count; ++i)
		{
			const disassembly line = disassemble(s, s.cpu.cs(), offset);
			printf("%04X:%04X  %-14s %s\n", s.cpu.cs(), offset, line.bytes, line.text);
			offset = uint16_t(offset + line.length);
		}
		delete[] data;
		return 0;
	}

	// Mounts the folder a program lives in and loads it, which is what every
	// command that actually runs one needs.
	bool mount_and_load(const char* path, win32_host& host, machine& m, const char* command_line,
	                    char* dos_path, int dos_path_size)
	{
		uint8_t* data = nullptr;
		uint32_t size = 0;
		if (!read_file(path, data, size))
		{
			printf("Cannot read %s\n", path);
			return false;
		}

		char directory[MAX_PATH] = {};
		char name[MAX_PATH] = {};
		split_path(path, directory, MAX_PATH, name, MAX_PATH);
		if (!host.set_root(directory))
		{
			printf("Cannot mount %s\n", directory);
			delete[] data;
			return false;
		}

		m.set_host(&host);
		snprintf(dos_path, size_t(dos_path_size), "C:\\%s", name);
		const bool loaded = m.load_program(data, size, dos_path, command_line);
		if (!loaded) printf("%s\n", m.error());
		delete[] data;
		return loaded;
	}

	// What the compiler's output costs the guest: the size of the image, and the
	// work the program does to run to completion. Both have to come down before
	// a program the size of a game is worth attempting, and neither is visible
	// in the host-side --bench numbers.
	int cc_bench_command(const char* path, const std::vector<std::string>& includes,
	                     const std::vector<std::string>& defines, const cc_options& options)
	{
		std::vector<uint8_t> image;
		if (!compile_file(path, includes, defines, image, false, options)) return 1;

		// The program has to exist on a drive the emulator can mount, and it is
		// not something to leave behind next to the source.
		char directory[MAX_PATH] = {};
		if (GetTempPathA(MAX_PATH, directory) == 0) return 1;
		char target[MAX_PATH] = {};
		snprintf(target, sizeof(target), "%sCCBENCH.COM", directory);
		FILE* file = nullptr;
		if (fopen_s(&file, target, "wb") != 0 || file == nullptr)
		{
			printf("Cannot write %s\n", target);
			return 1;
		}
		fwrite(image.data(), 1, image.size(), file);
		fclose(file);

		win32_host host;
		machine m;
		char dos_path[160] = {};
		if (!mount_and_load(target, host, m, "", dos_path, sizeof(dos_path))) return 1;
		m.set_render_enabled(false);

		machine_state& s = m.state();
		const LARGE_INTEGER started = now();
		for (int frame = 0; frame < 36000 && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		const double elapsed = milliseconds_since(started);
		if (!m.exited())
		{
			printf("The benchmark did not finish\n");
			return 1;
		}
		printf("exit code %u, %llu instructions, %llu cycles\n", s.exit_code,
		       static_cast<unsigned long long>(s.cpu.instructions),
		       static_cast<unsigned long long>(s.cycles));
		printf("emulated in %.3f ms\n", elapsed);
		if (s.error[0] != 0) printf("first problem: %s\n", s.error);
		return 0;
	}

	// ---- a Windows executable around a compiled block ----
	//
	// The x64 back end produces the same one-segment image the DOS back end
	// does, so an .exe is that block plus two pieces of glue: a startup that
	// hands the program the address of its own block, and a service routine
	// that turns the INT 21h calls into Windows ones. Nothing is relocated, so
	// the loader has to honour the image base - which is what lets every
	// address in here be written now rather than at load time.
	struct pe_writer
	{
		static const uint64_t image_base = 0x400000;
		static const uint32_t section_rva = 0x1000;
		static const uint32_t block_size = 0x10000; // code, data and stack

		std::vector<uint8_t> body; // the one section, from section_rva onwards
		std::vector<std::pair<uint32_t, uint32_t>> calls; // where a call is, and the IAT slot it wants

		uint32_t at() const { return uint32_t(body.size()); }
		uint32_t rva() const { return section_rva + at(); }
		void u8(int v) { body.push_back(uint8_t(v)); }
		void u16(int v) { u8(v & 0xff); u8((v >> 8) & 0xff); }
		void u32(uint32_t v) { u16(int(v & 0xffff)); u16(int((v >> 16) & 0xffff)); }
		void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
		void run(std::initializer_list<int> bytes) { for (const int b : bytes) u8(b); }
		void align(uint32_t to) { while (at() % to != 0) u8(0); }

		// call [rip + slot], which is how an import is reached.
		void call_import(uint32_t slot)
		{
			run({0xff, 0x15});
			calls.emplace_back(at(), slot);
			u32(0);
		}

		void patch(uint32_t position, uint32_t value)
		{
			body[position] = uint8_t(value & 0xff);
			body[position + 1] = uint8_t((value >> 8) & 0xff);
			body[position + 2] = uint8_t((value >> 16) & 0xff);
			body[position + 3] = uint8_t((value >> 24) & 0xff);
		}
	};

	// The service routine, called the same way the in-process one is: the block
	// in RCX and the interrupt number in EDX. Only the volatile registers are
	// touched, because every other one is holding something of the guest's.
	void write_service(pe_writer& pe, uint32_t std_handle, uint32_t write_file, uint32_t exit_process)
	{
		std::vector<uint32_t> to_end;

		pe.run({0x48, 0x83, 0xec, 0x48}); // sub rsp, 48h - shadow space and three locals
		pe.run({0x48, 0x89, 0x4c, 0x24, 0x30}); // mov [rsp+30h], rcx - the block
		pe.run({0xc6, 0x81}); // mov byte [rcx + carry], 0
		pe.u32(CC_NATIVE_CARRY);
		pe.u8(0);

		pe.run({0x83, 0xfa, 0x21}); // cmp edx, 21h
		pe.run({0x75});
		const uint32_t not_dos = pe.at();
		pe.u8(0);

		pe.run({0x0f, 0xb7, 0x81}); // movzx eax, word [rcx + registers]
		pe.u32(CC_NATIVE_REGISTERS);
		pe.run({0xc1, 0xe8, 0x08}); // shr eax, 8 - the service is in AH

		pe.run({0x83, 0xf8, 0x02, 0x74}); // cmp eax, 2 / je
		const uint32_t to_character = pe.at();
		pe.u8(0);
		pe.run({0x83, 0xf8, 0x09, 0x74}); // cmp eax, 9 / je
		const uint32_t to_string = pe.at();
		pe.u8(0);
		pe.run({0x83, 0xf8, 0x4c, 0x74}); // cmp eax, 4Ch / je
		const uint32_t to_exit = pe.at();
		pe.u8(0);
		pe.run({0xeb});
		to_end.push_back(pe.at());
		pe.u8(0);

		// One character: the byte is DL, which is already in the block.
		const uint32_t character = pe.at();
		pe.body[to_character] = uint8_t(character - to_character - 1);
		pe.run({0xb9, 0xf5, 0xff, 0xff, 0xff}); // mov ecx, -11 - the standard output
		pe.call_import(std_handle);
		pe.run({0x48, 0x89, 0xc1}); // mov rcx, rax
		pe.run({0x48, 0x8b, 0x54, 0x24, 0x30}); // mov rdx, [rsp+30h]
		pe.run({0x48, 0x81, 0xc2}); // add rdx, <the low byte of DX>
		pe.u32(CC_NATIVE_REGISTERS + 6);
		pe.run({0x41, 0xb8, 0x01, 0x00, 0x00, 0x00}); // mov r8d, 1
		pe.run({0x4c, 0x8d, 0x4c, 0x24, 0x28}); // lea r9, [rsp+28h] - bytes written
		pe.run({0x48, 0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00}); // mov qword [rsp+20h], 0
		pe.call_import(write_file);
		pe.run({0xeb});
		to_end.push_back(pe.at());
		pe.u8(0);

		// A $-terminated string, a character at a time: the cursor lives on the
		// stack because every register that would survive a call is the
		// guest's.
		const uint32_t string = pe.at();
		pe.body[to_string] = uint8_t(string - to_string - 1);
		pe.run({0x4c, 0x8b, 0x54, 0x24, 0x30}); // mov r10, [rsp+30h]
		pe.run({0x41, 0x0f, 0xb7, 0x82}); // movzx eax, word [r10 + dx]
		pe.u32(CC_NATIVE_REGISTERS + 6);
		pe.run({0x49, 0x01, 0xc2}); // add r10, rax
		pe.run({0x4c, 0x89, 0x54, 0x24, 0x38}); // mov [rsp+38h], r10

		const uint32_t loop = pe.at();
		pe.run({0x4c, 0x8b, 0x54, 0x24, 0x38}); // mov r10, [rsp+38h]
		pe.run({0x41, 0x80, 0x3a, 0x24}); // cmp byte [r10], '$'
		pe.run({0x74});
		to_end.push_back(pe.at());
		pe.u8(0);
		pe.run({0xb9, 0xf5, 0xff, 0xff, 0xff}); // mov ecx, -11
		pe.call_import(std_handle);
		pe.run({0x48, 0x89, 0xc1}); // mov rcx, rax
		pe.run({0x48, 0x8b, 0x54, 0x24, 0x38}); // mov rdx, [rsp+38h]
		pe.run({0x41, 0xb8, 0x01, 0x00, 0x00, 0x00}); // mov r8d, 1
		pe.run({0x4c, 0x8d, 0x4c, 0x24, 0x28}); // lea r9, [rsp+28h]
		pe.run({0x48, 0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00}); // mov qword [rsp+20h], 0
		pe.call_import(write_file);
		pe.run({0x48, 0xff, 0x44, 0x24, 0x38}); // inc qword [rsp+38h]
		pe.run({0xeb});
		pe.u8(int(int8_t(int(loop) - int(pe.at()) - 1)));

		// Terminate, which does not come back.
		const uint32_t finish = pe.at();
		pe.body[to_exit] = uint8_t(finish - to_exit - 1);
		pe.run({0x4c, 0x8b, 0x54, 0x24, 0x30}); // mov r10, [rsp+30h]
		pe.run({0x41, 0x0f, 0xb7, 0x8a}); // movzx ecx, word [r10 + ax]
		pe.u32(CC_NATIVE_REGISTERS);
		pe.run({0x81, 0xe1, 0xff, 0x00, 0x00, 0x00}); // and ecx, 255
		pe.call_import(exit_process);

		const uint32_t end = pe.at();
		pe.body[not_dos] = uint8_t(end - not_dos - 1);
		for (const uint32_t branch : to_end) pe.body[branch] = uint8_t(end - branch - 1);
		pe.run({0x48, 0x83, 0xc4, 0x48}); // add rsp, 48h
		pe.run({0xc3}); // ret
	}

	bool write_executable(const std::vector<uint8_t>& image, const char* path)
	{
		if (image.size() > pe_writer::block_size) return false;

		pe_writer pe;
		pe.body.assign(image.begin(), image.end());
		pe.body.resize(pe_writer::block_size, 0);

		// The startup the loader enters: the block is the image itself, and the
		// program's answer becomes the process's.
		const uint32_t entry_rva = pe.rva();
		pe.run({0x48, 0x83, 0xec, 0x28}); // sub rsp, 28h - alignment and shadow space
		pe.run({0x48, 0xb9}); // mov rcx, <the block>
		pe.u64(pe_writer::image_base + pe_writer::section_rva);
		pe.run({0x48, 0xb8}); // mov rax, <the program's own entry>
		pe.u64(pe_writer::image_base + pe_writer::section_rva);
		pe.run({0xff, 0xd0}); // call rax
		pe.run({0x0f, 0xb7, 0xc8}); // movzx ecx, ax
		const uint32_t exit_call = pe.at();
		pe.run({0xff, 0x15, 0x00, 0x00, 0x00, 0x00});
		pe.run({0xcc}); // not reached

		// The imports, laid out before the service routine so their slots are
		// known while it is being written.
		pe.align(8);
		const uint32_t iat_rva = pe.rva();
		const uint32_t iat_at = pe.at();
		for (int i = 0; i < 4; ++i) pe.u64(0); // three imports and the terminator

		const uint32_t std_handle = iat_rva;
		const uint32_t write_file = iat_rva + 8;
		const uint32_t exit_process = iat_rva + 16;

		const uint32_t service_rva = pe.rva();
		write_service(pe, std_handle, write_file, exit_process);
		pe.calls.emplace_back(exit_call + 2, exit_process);

		// The import directory, its lookup table and the names.
		pe.align(8);
		const uint32_t lookup_rva = pe.rva();
		const uint32_t lookup_at = pe.at();
		for (int i = 0; i < 4; ++i) pe.u64(0);

		pe.align(4);
		const uint32_t descriptors_rva = pe.rva();
		const uint32_t descriptors_at = pe.at();
		for (int i = 0; i < 10; ++i) pe.u32(0); // one descriptor and the terminator

		const char* names[3] = {"GetStdHandle", "WriteFile", "ExitProcess"};
		uint32_t name_rva[3] = {};
		for (int i = 0; i < 3; ++i)
		{
			pe.align(2);
			name_rva[i] = pe.rva();
			pe.u16(0); // the hint
			for (const char* c = names[i]; *c != 0; ++c) pe.u8(*c);
			pe.u8(0);
		}
		const uint32_t library_rva = pe.rva();
		for (const char* c = "KERNEL32.dll"; *c != 0; ++c) pe.u8(*c);
		pe.u8(0);
		pe.align(16);

		for (int i = 0; i < 3; ++i)
		{
			pe.patch(iat_at + i * 8, name_rva[i]);
			pe.patch(lookup_at + i * 8, name_rva[i]);
		}
		pe.patch(descriptors_at, lookup_rva); // OriginalFirstThunk
		pe.patch(descriptors_at + 12, library_rva); // Name
		pe.patch(descriptors_at + 16, iat_rva); // FirstThunk

		// The program reaches the host the same way it does in process: through
		// a function pointer at the top of its own block.
		{
			const uint64_t service = pe_writer::image_base + service_rva;
			memcpy(&pe.body[CC_NATIVE_SERVICE], &service, sizeof(service));
		}

		// Every call into the import table is relative to the instruction after
		// it, and both ends are inside the image.
		for (const auto& call : pe.calls)
			pe.patch(call.first, call.second - (pe_writer::section_rva + call.first + 4));

		const uint32_t file_alignment = 0x200;
		const uint32_t headers = 0x200;
		const uint32_t raw_size = (uint32_t(pe.body.size()) + file_alignment - 1) / file_alignment *
			file_alignment;
		const uint32_t image_size = (pe_writer::section_rva + uint32_t(pe.body.size()) + 0xfff) /
			0x1000 * 0x1000;

		std::vector<uint8_t> file(headers, 0);
		const auto put16 = [&](uint32_t at, int v)
		{
			file[at] = uint8_t(v & 0xff);
			file[at + 1] = uint8_t((v >> 8) & 0xff);
		};
		const auto put32 = [&](uint32_t at, uint32_t v)
		{
			put16(at, int(v & 0xffff));
			put16(at + 2, int((v >> 16) & 0xffff));
		};
		const auto put64 = [&](uint32_t at, uint64_t v)
		{
			put32(at, uint32_t(v));
			put32(at + 4, uint32_t(v >> 32));
		};

		file[0] = 'M';
		file[1] = 'Z';
		put32(0x3c, 0x40); // where the real header is

		uint32_t p = 0x40;
		file[p] = 'P';
		file[p + 1] = 'E';
		p += 4;
		put16(p, 0x8664); // machine
		put16(p + 2, 1); // one section
		put16(p + 16, 0xf0); // the optional header's size
		put16(p + 18, 0x0023); // executable, relocations stripped, large address aware
		p += 20;

		const uint32_t optional = p;
		put16(optional, 0x20b); // PE32+
		put32(optional + 4, raw_size); // size of code
		put32(optional + 16, entry_rva);
		put32(optional + 20, pe_writer::section_rva); // base of code
		put64(optional + 24, pe_writer::image_base);
		put32(optional + 32, 0x1000); // section alignment
		put32(optional + 36, file_alignment);
		put16(optional + 40, 6); // the oldest Windows this asks for
		put16(optional + 48, 6);
		put32(optional + 56, image_size);
		put32(optional + 60, headers);
		put16(optional + 68, 3); // a console program
		put64(optional + 72, 0x100000); // stack reserve
		put64(optional + 80, 0x1000); // stack commit
		put64(optional + 88, 0x100000); // heap reserve
		put64(optional + 96, 0x1000); // heap commit
		put32(optional + 108, 16); // how many directories follow

		const uint32_t directories = optional + 112;
		put32(directories + 8, descriptors_rva); // the import directory
		put32(directories + 12, 40);
		put32(directories + 96, iat_rva); // and the table it fills in
		put32(directories + 100, 32);

		const uint32_t section = optional + 0xf0;
		memcpy(&file[section], ".text\0\0", 8);
		put32(section + 8, uint32_t(pe.body.size())); // virtual size
		put32(section + 12, pe_writer::section_rva);
		put32(section + 16, raw_size);
		put32(section + 20, headers); // where it is in the file
		put32(section + 36, 0xe0000060); // code and data, read, write, execute

		file.insert(file.end(), pe.body.begin(), pe.body.end());
		file.resize(headers + raw_size, 0);

		FILE* out = nullptr;
		if (fopen_s(&out, path, "wb") != 0 || out == nullptr) return false;
		fwrite(file.data(), 1, file.size(), out);
		fclose(out);
		return true;
	}

	// Compiles for the machine this is running on and calls the result. The
	// same source through the same front end and a different back end has to
	// give the same answer the emulator gave, which is the only claim a second
	// back end is really making.
	int cc_native_command(const char* path, const std::vector<std::string>& includes,
	                      const std::vector<std::string>& defines, bool dump)
	{
		cc_options options = default_options();
		options.machine = cc_machine_x64;
		std::vector<uint8_t> image;
		if (!compile_file(path, includes, defines, image, false, options)) return 1;

		if (dump)
		{
			for (size_t i = 0; i < image.size(); ++i)
				printf("%02X%s", image[i], (i + 1) % 16 == 0 ? "\n" : " ");
			printf("\n");
		}
		fflush(stdout); // a fault in generated code takes the process, buffer and all

		// The program's drive is the directory its source came from, which is
		// where the emulated run would have put it too.
		char directory[MAX_PATH] = {};
		char name[MAX_PATH] = {};
		split_path(path, directory, MAX_PATH, name, MAX_PATH);

		win32_host host;
		host.set_root(directory);

		int answer = 0;
		const LARGE_INTEGER started = now();
		if (!run_native(image, host, answer))
		{
			printf("This build cannot run what it just compiled\n");
			return 1;
		}
		printf("returned %d in %.3f ms\n", answer, milliseconds_since(started));
		return 0;
	}

	// The same image, written out as something Windows will load itself.
	int cc_exe_command(const char* path, const char* output, const std::vector<std::string>& includes,
	                   const std::vector<std::string>& defines)
	{
		cc_options options = default_options();
		options.machine = cc_machine_x64;
		std::vector<uint8_t> image;
		if (!compile_file(path, includes, defines, image, false, options)) return 1;

		char target[MAX_PATH] = {};
		if (output != nullptr) snprintf(target, sizeof(target), "%s", output);
		else
		{
			snprintf(target, sizeof(target), "%s", path);
			char* dot = strrchr(target, '.');
			if (dot != nullptr) snprintf(dot, sizeof(target) - size_t(dot - target), ".exe");
		}

		if (!write_executable(image, target))
		{
			printf("Cannot write %s\n", target);
			return 1;
		}
		printf("Wrote %s\n", target);
		return 0;
	}

	int run_program(const char* path, int frames, bool show_screen, bool trace, bool show_state,
	                const char* command_line, int trace_tail, const char* bmp_path, const char* wav_path,
	                const char* keys, int key_delay)
	{
		const DWORD attributes = GetFileAttributesA(path);
		const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

		win32_host host;
		host.set_verbose(trace);
		machine m;

		if (is_directory)
		{
			if (!host.set_root(path))
			{
				printf("Cannot mount %s\n", path);
				return 1;
			}
			m.set_host(&host);
			m.state().trace = trace;

			// A folder mounts the drive and drops you at the shell prompt;
			// --args is typed in for you.
			printf("Mounted %s, at the shell prompt\n", host.root());
			m.start_shell();
			for (const char* p = command_line; *p != 0;)
			{
				for (int i = 0; i < 8 && *p != 0; ++i) bios_push_key(m.state(), 0, uint8_t(*p++));
				m.exec(m.cycles_per_frame());
			}
			if (command_line[0] != 0) bios_push_key(m.state(), 0x1c, 13);
		}
		else
		{
			char dos_path[160] = {};
			if (!mount_and_load(path, host, m, command_line, dos_path, sizeof(dos_path))) return 1;
			m.state().trace = trace;
			printf("Running %s from %s for %d frames\n", dos_path, host.root(), frames);
		}

		if (trace_tail > 0)
		{
			// Instruction-level run, keeping the most recent instructions so a
			// program that fails can be read backwards from where it stopped.
			struct trace_entry
			{
				uint16_t cs, ip, ax, bx, cx, dx, si, di;
				char text[64];
			};
			const int capacity = trace_tail;
			trace_entry* ring = new trace_entry[size_t(capacity)];
			int64_t count = 0;
			machine_state& s = m.state();
			const uint64_t budget = uint64_t(frames) * m.cycles_per_frame();
			uint64_t spent = 0;
			while (spent < budget && !m.exited())
			{
				trace_entry& entry = ring[count % capacity];
				entry.cs = s.cpu.cs();
				entry.ip = s.cpu.ip;
				entry.ax = s.cpu.ax();
				entry.bx = s.cpu.bx();
				entry.cx = s.cpu.cx();
				entry.dx = s.cpu.dx();
				entry.si = s.cpu.si();
				entry.di = s.cpu.di();
				const disassembly line = disassemble(s, entry.cs, entry.ip);
				memcpy(entry.text, line.text, sizeof(entry.text));
				++count;

				const int cost = cpu_step(s);
				s.cycles += uint64_t(cost);
				spent += uint64_t(cost);
				s.pit.pending += uint64_t(cost);
				if (s.pit.pending >= s.pit.due) pit_sync(s);
			}
			const int64_t first = count > capacity ? count - capacity : 0;
			printf("--- last %lld instructions of %lld ---\n",
			       static_cast<long long>(count - first), static_cast<long long>(count));
			for (int64_t i = first; i < count; ++i)
			{
				const trace_entry& entry = ring[i % capacity];
				printf("%04X:%04X  %-26s AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X\n",
				       entry.cs, entry.ip, entry.text, entry.ax, entry.bx, entry.cx, entry.dx,
				       entry.si, entry.di);
			}
			delete[] ring;
		}
		else
		{
			constexpr int samples_per_frame = sound_sample_rate / 60;
			std::vector<int16_t> audio;
			if (wav_path != nullptr) m.set_audio_enabled(true);
			int typed = 0;
			machine_key held = machine_key::none;
			for (int frame = 0; frame < frames && !m.exited(); ++frame)
			{
				if (held != machine_key::none)
				{
					m.key_up(held);
					held = machine_key::none;
				}
				else if (keys != nullptr && keys[typed] != 0 && frame % key_delay == 0)
				{
					held = key_from_char(keys[typed++]);
					if (held != machine_key::none) m.key_down(held);
				}
				m.exec(m.cycles_per_frame());
				if (wav_path == nullptr) continue;
				audio.resize(audio.size() + samples_per_frame);
				m.generate_audio(audio.data() + audio.size() - samples_per_frame, samples_per_frame);
			}
			if (wav_path != nullptr)
			{
				int16_t peak = 0;
				for (const int16_t sample : audio)
					if (sample > peak || -sample > peak) peak = int16_t(sample < 0 ? -sample : sample);
				if (write_wav(wav_path, audio.data(), uint32_t(audio.size())))
					printf("Wrote %s (%.1f s, peak %d of 32767)\n", wav_path,
					       double(audio.size()) / sound_sample_rate, peak);
				else
					printf("Cannot write %s\n", wav_path);
			}
		}

		machine_state& s = m.state();
		if (m.exited()) printf("Program exited with code %u\n", s.exit_code);
		else printf("Still running after %d frames\n", frames);
		if (s.error[0] != 0) printf("First problem: %s\n", s.error);

		if (show_state)
		{
			debug_state state{};
			debug_capture(s, state);
			char text[2048] = {};
			debug_format(state, text, sizeof(text));
			printf("%s", text);
		}

		if (show_screen)
		{
			char screen[8192] = {};
			m.text_screen(screen, sizeof(screen));
			printf("--- video mode %02Xh, %dx%d ---\n%s", s.vga.mode, s.vga.width, s.vga.height, screen);
		}

		if (bmp_path != nullptr)
		{
			vga_render(s);
			if (write_bmp(bmp_path, m.framebuffer(), m.width(), m.height()))
				printf("Wrote %s (%dx%d)\n", bmp_path, m.width(), m.height());
			else
				printf("Cannot write %s\n", bmp_path);
		}
		return 0;
	}

	// What the interpreter actually costs, and therefore what cpu_hz a 60 Hz
	// frame could afford. Phase 0 of docs/jit.md: nothing about a JIT is worth
	// starting until these numbers say where the time goes.
	struct bench_pass
	{
		double seconds = 0;
		uint64_t instructions = 0;
		uint64_t cycles = 0;
		int frames = 0;
		bool exited = false;
	};

	bench_pass bench_measure(machine& m, double seconds, bool render)
	{
		m.set_render_enabled(render);
		machine_state& s = m.state();
		const uint64_t instructions = s.cpu.instructions;
		const uint64_t cycles = s.cycles;

		LARGE_INTEGER frequency{}, start{}, now{};
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);

		bench_pass pass;
		do
		{
			m.exec(m.cycles_per_frame());
			++pass.frames;
			QueryPerformanceCounter(&now);
			pass.seconds = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
		}
		while (pass.seconds < seconds && !m.exited());

		pass.instructions = s.cpu.instructions - instructions;
		pass.cycles = s.cycles - cycles;
		pass.exited = m.exited();
		return pass;
	}

	void bench_report(const char* label, const bench_pass& pass, uint32_t cpu_hz)
	{
		const double rate = pass.seconds > 0 ? double(pass.cycles) / pass.seconds : 0;
		printf("  %-12s %10.1f M %11.1f %10.1fx %14.1f\n",
		       label,
		       pass.seconds > 0 ? double(pass.instructions) / pass.seconds / 1e6 : 0,
		       rate / 1e6,
		       cpu_hz > 0 ? rate / double(cpu_hz) : 0,
		       pass.instructions > 0 ? double(pass.cycles) / double(pass.instructions) : 0);
	}

	int run_bench(const char* path, double seconds, int warmup, uint32_t cpu_hz, const char* command_line)
	{
		win32_host host;
		machine m;
		char dos_path[160] = {};
		if (!mount_and_load(path, host, m, command_line, dos_path, sizeof(dos_path))) return 1;
		if (cpu_hz > 0) m.state().cpu_hz = cpu_hz;

		printf("Benchmarking %s from %s\n", dos_path, host.root());
		printf("  warmup %d frames, %.1f s per pass, cpu_hz %u\n\n", warmup, seconds, m.state().cpu_hz);
		for (int frame = 0; frame < warmup && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
		if (m.exited())
		{
			printf("Program exited during warmup with code %u\n", m.state().exit_code);
			return 1;
		}

		printf("  pass         instructions/s   guest MHz   real-time   cycles/instr\n");
		const bench_pass full = bench_measure(m, seconds, true);
		bench_report("full", full, m.state().cpu_hz);
		bench_pass bare;
		if (!full.exited)
		{
			bare = bench_measure(m, seconds, false);
			bench_report("no render", bare, m.state().cpu_hz);
		}
		m.set_render_enabled(true);

		const double full_rate = full.seconds > 0 ? double(full.cycles) / full.seconds : 0;
		const double bare_rate = bare.seconds > 0 ? double(bare.cycles) / bare.seconds : 0;
		printf("\n");
		if (bare_rate > 0)
			printf("  vga_render is %.0f%% of the frame\n", (1.0 - full_rate / bare_rate) * 100.0);
		printf("  sustains 60 fps at cpu_hz = %.1f MHz (%.1fx the configured %.1f MHz)\n",
		       full_rate / 1e6, full_rate / double(m.state().cpu_hz), double(m.state().cpu_hz) / 1e6);

		machine_state& s = m.state();
		printf("  video mode %02Xh %dx%d, %d frames measured%s\n",
		       s.vga.mode, s.vga.width, s.vga.height, full.frames,
		       full.exited ? ", program exited during the pass" : "");
		if (s.error[0] != 0) printf("  first problem: %s\n", s.error);
		return 0;
	}

	void usage()
	{
		printf(
			"demu - DOS program runner, debugger and test suite (libdos)\n\n"
			"  demu --test                       run the behavioural test suite\n"
			"  demu --cc <file.c> [-o out.com] [-I dir] [-D name=value]\n"
			"                                         compile C to a DOS program\n"
			"  demu --ccbench <file.c> [-I dir] [-D name=value]\n"
			"                                         compile, run, and report size and work\n"
			"  demu --ccnative <file.c> [--dump] compile for this machine and run it\n"
			"  demu --ccexe <file.c> [-o out.exe] [-I dir] [-D name=value]\n"
			"                                         compile to a Windows executable\n"
			"  demu --info <file>                describe a DOS executable\n"
			"  demu --disasm <file> [count]      disassemble from the entry point\n"
			"  demu --run <file> [options]       run headlessly\n"
			"  demu --run <folder> --args \"dir\"  mount a folder and use the shell\n"
			"  demu --bench <file> [options]     measure interpreter throughput\n\n"
			"Run options:\n"
			"  --frames <n>    how many 60 Hz frames to run (default 600)\n"
			"  --screen        print the text screen afterwards\n"
			"  --state         print registers, memory and a disassembly window\n"
			"  --trace         report DOS and BIOS calls as they happen\n"
			"  --trace-tail n  print the last n instructions executed\n"
			"  --bmp <file>    write the final frame as a bitmap\n"
			"  --wav <file>    write the audio the program produced\n"
			"  --keys <text>   type these keys as it runs, one every --key-delay frames\n"
			"  --key-delay <n> frames between typed keys (default 60)\n"
			"  --args <text>   command line to hand to the program\n\n"
			"Bench options:\n"
			"  --seconds <n>   wall clock per pass (default 3)\n"
			"  --warmup <n>    frames to run before measuring (default 300)\n"
			"  --cpu-hz <n>    emulated clock to measure at (default 8000000)\n\n"
			"Compiler options:\n"
			"  --inline <n>    how big a body may be, in tokens, to be copied to\n"
			"                  its call site rather than called (default 24)\n"
			"  --no-inline     never copy a body to its call site\n"
			"  --keep-unused   keep the functions nothing can reach\n"
			"  --cpu <n>       8086 or 386; a 386 still produces a 16-bit .COM\n"
			"  --map           list what each function cost, largest first\n\n");
		runner_usage();
		boot_usage();
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		usage();
		return 1;
	}

	const char* command = argv[1];

	// Anything but one of the -- commands is a program to run, the way the
	// original demu ran the build tools: demu [options] program [arguments].
	if (command[0] != '-' || command[1] != '-') return run_dos_program(argc - 1, argv + 1);

	if (strcmp(command, "--test") == 0) return run_tests();
	if (strcmp(command, "--cc") == 0 && argc > 2)
	{
		const char* output = nullptr;
		std::vector<std::string> includes;
		std::vector<std::string> defines;
		cc_options options = default_options();
		bool map = false;
		for (int i = 3; i < argc; ++i)
		{
			if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
			else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) includes.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) defines.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "--inline") == 0 && i + 1 < argc) options.inline_limit = atoi(argv[++i]);
			else if (strcmp(argv[i], "--no-inline") == 0) options.inline_limit = 0;
			else if (strcmp(argv[i], "--keep-unused") == 0) options.drop_unreachable = false;
			else if (strcmp(argv[i], "--map") == 0) map = true;
			else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
				options.cpu = atoi(argv[++i]) >= 386 ? cc_cpu_i80386 : cc_cpu_i8086;
			else if (strncmp(argv[i], "-D", 2) == 0) defines.emplace_back(argv[i] + 2);
			else if (strncmp(argv[i], "-I", 2) == 0) includes.emplace_back(argv[i] + 2);
		}
		return compile_command(argv[2], output, includes, defines, options, map);
	}
	if (strcmp(command, "--ccbench") == 0 && argc > 2)
	{
		std::vector<std::string> includes;
		std::vector<std::string> defines;
		cc_options options = default_options();
		for (int i = 3; i < argc; ++i)
		{
			if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) includes.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) defines.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "--inline") == 0 && i + 1 < argc) options.inline_limit = atoi(argv[++i]);
			else if (strcmp(argv[i], "--no-inline") == 0) options.inline_limit = 0;
			else if (strcmp(argv[i], "--keep-unused") == 0) options.drop_unreachable = false;
			else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
				options.cpu = atoi(argv[++i]) >= 386 ? cc_cpu_i80386 : cc_cpu_i8086;
			else if (strncmp(argv[i], "-D", 2) == 0) defines.emplace_back(argv[i] + 2);
			else if (strncmp(argv[i], "-I", 2) == 0) includes.emplace_back(argv[i] + 2);
		}
		return cc_bench_command(argv[2], includes, defines, options);
	}
	if (strcmp(command, "--ccnative") == 0 && argc > 2)
	{
		std::vector<std::string> includes;
		std::vector<std::string> defines;
		bool dump = false;
		for (int i = 3; i < argc; ++i)
		{
			if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) includes.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) defines.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "--dump") == 0) dump = true;
		}
		return cc_native_command(argv[2], includes, defines, dump);
	}
	if (strcmp(command, "--ccexe") == 0 && argc > 2)
	{
		const char* output = nullptr;
		std::vector<std::string> includes;
		std::vector<std::string> defines;
		for (int i = 3; i < argc; ++i)
		{
			if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
			else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) includes.emplace_back(argv[++i]);
			else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) defines.emplace_back(argv[++i]);
			else if (strncmp(argv[i], "-D", 2) == 0) defines.emplace_back(argv[i] + 2);
			else if (strncmp(argv[i], "-I", 2) == 0) includes.emplace_back(argv[i] + 2);
		}
		return cc_exe_command(argv[2], output, includes, defines);
	}
	if (strcmp(command, "--info") == 0 && argc > 2) return show_info(argv[2]);
	if (strcmp(command, "--disasm") == 0 && argc > 2)
		return disassemble_file(argv[2], argc > 3 ? atoi(argv[3]) : 32);
	if (strcmp(command, "--boot") == 0) return run_boot(argc - 2, argv + 2);
	if (strcmp(command, "--bench") == 0 && argc > 2)
	{
		double seconds = 3.0;
		int warmup = 300;
		uint32_t bench_hz = 0;
		const char* bench_args = "";
		for (int i = 3; i < argc; ++i)
		{
			if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
			else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) warmup = atoi(argv[++i]);
			else if (strcmp(argv[i], "--cpu-hz") == 0 && i + 1 < argc) bench_hz = uint32_t(atoi(argv[++i]));
			else if (strcmp(argv[i], "--args") == 0 && i + 1 < argc) bench_args = argv[++i];
		}
		return run_bench(argv[2], seconds, warmup, bench_hz, bench_args);
	}

	const char* path = nullptr;
	if (strcmp(command, "--run") == 0 && argc > 2) path = argv[2];
	if (path == nullptr)
	{
		usage();
		return 1;
	}

	int frames = 600;
	bool screen = false;
	bool trace = false;
	bool state = false;
	int trace_tail = 0;
	const char* command_line = "";
	const char* bmp_path = nullptr;
	const char* wav_path = nullptr;
	const char* keys = nullptr;
	int key_delay = 60;
	for (int i = 2; i < argc; ++i)
	{
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = atoi(argv[++i]);
		else if (strcmp(argv[i], "--screen") == 0) screen = true;
		else if (strcmp(argv[i], "--trace") == 0) trace = true;
		else if (strcmp(argv[i], "--state") == 0) state = true;
		else if (strcmp(argv[i], "--trace-tail") == 0 && i + 1 < argc) trace_tail = atoi(argv[++i]);
		else if (strcmp(argv[i], "--bmp") == 0 && i + 1 < argc) bmp_path = argv[++i];
		else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wav_path = argv[++i];
		else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) keys = argv[++i];
		else if (strcmp(argv[i], "--key-delay") == 0 && i + 1 < argc) key_delay = atoi(argv[++i]);
		else if (strcmp(argv[i], "--args") == 0 && i + 1 < argc) command_line = argv[++i];
	}
	if (key_delay < 2) key_delay = 2;
	return run_program(path, frames, screen, trace, state, command_line, trace_tail, bmp_path, wav_path,
	                   keys, key_delay);
}
