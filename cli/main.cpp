// ovrwin by Zac Walker
//
// Command line driver: runs the behavioural test suite, and inspects,
// disassembles and headlessly runs DOS executables.

#include "../src/machine.h"
#include "../src/debug.h"
#include "../host/host-win32.h"
#include "../cc/cc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	int checks = 0;
	int failures = 0;

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

	// Sources held in memory, for compiler tests that include nothing.
	class no_sources final : public cc_sources
	{
	public:
		bool load(const char*, std::string&) override { return false; }
	};

	bool compile_text(const char* source, std::vector<uint8_t>& image, const char* name)
	{
		no_sources sources;
		const cc_result result = cc_compile(source, name, sources);
		if (!result.ok) printf("      %s(%d): %s\n", name, result.line, result.message.c_str());
		image = result.image;
		return result.ok;
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
	}

	int run_tests()
	{
		test_cpu();
		test_interrupts();
		test_timer();
		test_video();
		test_dos_memory();
		test_loader();
		test_program_run();
		test_sound();
		test_shell();
		test_compiler();
		test_keyboard();
		test_disassembler();

		printf("%d checks, %d failures\n", checks, failures);
		return failures == 0 ? 0 : 1;
	}

	// Supplies #include files to the compiler from the directory the source
	// being compiled lives in, then from any directory given with -I.
	class directory_sources final : public cc_sources
	{
	public:
		explicit directory_sources(const char* directory) { directories_.emplace_back(directory); }

		void add(const char* directory) { directories_.emplace_back(directory); }

		bool load(const char* name, std::string& text) override
		{
			for (const std::string& directory : directories_)
			{
				char path[MAX_PATH] = {};
				snprintf(path, sizeof(path), "%s\\%s", directory.c_str(), name);
				uint8_t* data = nullptr;
				uint32_t size = 0;
				if (!read_file(path, data, size)) continue;
				text.assign(reinterpret_cast<const char*>(data), size);
				delete[] data;
				return true;
			}
			return false;
		}

	private:
		std::vector<std::string> directories_;
	};

	// Compiles a C file and returns the .COM image, reporting any error.
	bool compile_file(const char* path, const std::vector<std::string>& includes,
	                  const std::vector<std::string>& defines, std::vector<uint8_t>& image, bool quiet)
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

		directory_sources sources(directory);
		for (const std::string& entry : includes) sources.add(entry.c_str());
		const cc_result result = cc_compile(text.c_str(), name, sources, defines);
		if (!result.ok)
		{
			printf("%s(%d): %s\n", result.file.empty() ? name : result.file.c_str(), result.line,
			       result.message.c_str());
			return false;
		}
		if (!quiet)
			printf("%s: %d bytes (%d code, %d data)\n", name, int(result.image.size()),
			       result.code_size, result.data_size);
		image = result.image;
		return true;
	}

	int compile_command(const char* path, const char* output, const std::vector<std::string>& includes,
	                    const std::vector<std::string>& defines)
	{
		std::vector<uint8_t> image;
		if (!compile_file(path, includes, defines, image, false)) return 1;

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
		if (size > 0x40 && data[0] == 'M' && data[1] == 'Z')
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
			if (word(0x18) >= 0x40 && size > 0x3e)
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

	int run_program(const char* path, int frames, bool show_screen, bool trace, bool show_state,
	                const char* command_line, int trace_tail, const char* bmp_path)
	{
		const DWORD attributes = GetFileAttributesA(path);
		const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

		uint8_t* data = nullptr;
		uint32_t size = 0;
		if (!is_directory && !read_file(path, data, size))
		{
			printf("Cannot read %s\n", path);
			return 1;
		}

		char directory[MAX_PATH] = {};
		char name[MAX_PATH] = {};
		if (is_directory) snprintf(directory, sizeof(directory), "%s", path);
		else split_path(path, directory, MAX_PATH, name, MAX_PATH);

		win32_host host;
		host.set_verbose(trace);
		if (!host.set_root(directory))
		{
			printf("Cannot mount %s\n", directory);
			delete[] data;
			return 1;
		}

		machine m;
		m.set_host(&host);
		m.state().trace = trace;

		if (is_directory)
		{
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
			snprintf(dos_path, sizeof(dos_path), "C:\\%s", name);
			if (!m.load_program(data, size, dos_path, command_line))
			{
				printf("%s\n", m.error());
				delete[] data;
				return 1;
			}
			printf("Running %s from %s for %d frames\n", dos_path, host.root(), frames);
		}
		delete[] data;

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
				pit_step(s, uint64_t(cost));
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
			for (int frame = 0; frame < frames && !m.exited(); ++frame) m.exec(m.cycles_per_frame());
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

	void usage()
	{
		printf(
			"ovrwin - DOS program emulator\n\n"
			"  ovrwincli --test                       run the behavioural test suite\n"
			"  ovrwincli --cc <file.c> [-o out.com] [-I dir] [-D name=value]\n"
			"                                         compile C to a DOS program\n"
			"  ovrwincli --info <file>                describe a DOS executable\n"
			"  ovrwincli --disasm <file> [count]      disassemble from the entry point\n"
			"  ovrwincli --run <file> [options]       run headlessly\n"
			"  ovrwincli --run <folder> --args \"dir\"  mount a folder and use the shell\n\n"
			"Run options:\n"
			"  --frames <n>    how many 60 Hz frames to run (default 600)\n"
			"  --screen        print the text screen afterwards\n"
			"  --state         print registers, memory and a disassembly window\n"
			"  --trace         report DOS and BIOS calls as they happen\n"
			"  --trace-tail n  print the last n instructions executed\n"
			"  --bmp <file>    write the final frame as a bitmap\n"
			"  --args <text>   command line to hand to the program\n");
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
	if (strcmp(command, "--test") == 0) return run_tests();
	if (strcmp(command, "--cc") == 0 && argc > 2)
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
		return compile_command(argv[2], output, includes, defines);
	}
	if (strcmp(command, "--info") == 0 && argc > 2) return show_info(argv[2]);
	if (strcmp(command, "--disasm") == 0 && argc > 2)
		return disassemble_file(argv[2], argc > 3 ? atoi(argv[3]) : 32);

	const char* path = nullptr;
	if (strcmp(command, "--run") == 0 && argc > 2) path = argv[2];
	else if (command[0] != '-') path = command;
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
	for (int i = 2; i < argc; ++i)
	{
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = atoi(argv[++i]);
		else if (strcmp(argv[i], "--screen") == 0) screen = true;
		else if (strcmp(argv[i], "--trace") == 0) trace = true;
		else if (strcmp(argv[i], "--state") == 0) state = true;
		else if (strcmp(argv[i], "--trace-tail") == 0 && i + 1 < argc) trace_tail = atoi(argv[++i]);
		else if (strcmp(argv[i], "--bmp") == 0 && i + 1 < argc) bmp_path = argv[++i];
		else if (strcmp(argv[i], "--args") == 0 && i + 1 < argc) command_line = argv[++i];
	}
	return run_program(path, frames, screen, trace, state, command_line, trace_tail, bmp_path);
}
