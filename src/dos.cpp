// ovrwin by Zac Walker
//
// The DOS personality. There is no DOS kernel in memory: the MCB chain, the
// PSP and the INT 21h entry points are built by the emulator so that programs
// see exactly the interface they were written against, while file access is
// handed to the host.

#include "machine.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	// Programs load above the first 64 KiB. Some EXEPACK-compressed images
	// compute a segment below their own load address while unpacking and
	// report "Packed file is corrupt" if that calculation goes negative.
	constexpr uint16_t first_mcb_segment = 0x1000;
	constexpr uint16_t memory_top_segment = 0x9fff;

	constexpr uint8_t error_file_not_found = 0x02;
	constexpr uint8_t error_path_not_found = 0x03;
	constexpr uint8_t error_too_many_files = 0x04;
	constexpr uint8_t error_access_denied = 0x05;
	constexpr uint8_t error_invalid_handle = 0x06;
	constexpr uint8_t error_out_of_memory = 0x08;
	constexpr uint8_t error_invalid_function = 0x01;
	constexpr uint8_t error_no_more_files = 0x12;

	void set_carry(machine_state& s, bool on) { cpu_set_iret_flag(s, flag_cf, on); }

	void fail(machine_state& s, uint8_t code)
	{
		s.cpu.set_ax(code);
		set_carry(s, true);
	}

	void succeed(machine_state& s) { set_carry(s, false); }

	char upper(char c) { return char(std::toupper(static_cast<unsigned char>(c))); }

	uint16_t mcb_size(machine_state& s, uint16_t mcb) { return read16(s, mcb, 3); }
	uint16_t mcb_owner(machine_state& s, uint16_t mcb) { return read16(s, mcb, 1); }
	uint8_t mcb_type(machine_state& s, uint16_t mcb) { return read8(s, mcb, 0); }

	void write_mcb(machine_state& s, uint16_t mcb, uint8_t type, uint16_t owner, uint16_t size)
	{
		write8(s, mcb, 0, type);
		write16(s, mcb, 1, owner);
		write16(s, mcb, 3, size);
		for (int i = 8; i < 16; ++i) write8(s, mcb, uint16_t(i), 0);
	}

	void merge_free_blocks(machine_state& s)
	{
		uint16_t mcb = s.dos.first_mcb;
		while (mcb_type(s, mcb) == 'M')
		{
			const uint16_t next = uint16_t(mcb + mcb_size(s, mcb) + 1);
			if (mcb_owner(s, mcb) == 0 && mcb_owner(s, next) == 0)
			{
				const uint16_t combined = uint16_t(mcb_size(s, mcb) + mcb_size(s, next) + 1);
				write8(s, mcb, 0, mcb_type(s, next));
				write16(s, mcb, 3, combined);
				continue;
			}
			mcb = next;
		}
	}
}

uint16_t dos_alloc(machine_state& s, uint16_t paragraphs, uint16_t& largest)
{
	merge_free_blocks(s);
	largest = 0;
	uint16_t mcb = s.dos.first_mcb;
	for (;;)
	{
		const uint8_t type = mcb_type(s, mcb);
		const uint16_t size = mcb_size(s, mcb);
		if (mcb_owner(s, mcb) == 0)
		{
			if (size > largest) largest = size;
			if (size >= paragraphs)
			{
				if (size > paragraphs + 1)
				{
					// Split, leaving the remainder as a free block.
					const uint16_t remainder = uint16_t(mcb + paragraphs + 1);
					write_mcb(s, remainder, type, 0, uint16_t(size - paragraphs - 1));
					write8(s, mcb, 0, 'M');
					write16(s, mcb, 3, paragraphs);
				}
				write16(s, mcb, 1, s.dos.psp_segment != 0 ? s.dos.psp_segment : 0x0008);
				return uint16_t(mcb + 1);
			}
		}
		if (type != 'M') break;
		mcb = uint16_t(mcb + size + 1);
	}
	return 0;
}

bool dos_free(machine_state& s, uint16_t segment)
{
	if (segment == 0) return false;
	const uint16_t mcb = uint16_t(segment - 1);
	const uint8_t type = mcb_type(s, mcb);
	if (type != 'M' && type != 'Z') return false;
	write16(s, mcb, 1, 0);
	merge_free_blocks(s);
	return true;
}

bool dos_resize(machine_state& s, uint16_t segment, uint16_t paragraphs, uint16_t& largest)
{
	const uint16_t mcb = uint16_t(segment - 1);
	const uint8_t type = mcb_type(s, mcb);
	if (type != 'M' && type != 'Z') return false;

	const uint16_t size = mcb_size(s, mcb);
	largest = size;
	if (paragraphs == size) return true;

	if (paragraphs < size)
	{
		const uint16_t remainder = uint16_t(mcb + paragraphs + 1);
		write_mcb(s, remainder, type, 0, uint16_t(size - paragraphs - 1));
		write8(s, mcb, 0, 'M');
		write16(s, mcb, 3, paragraphs);
		merge_free_blocks(s);
		return true;
	}

	// Growing only works into the free block immediately above.
	if (type != 'M') return false;
	const uint16_t next = uint16_t(mcb + size + 1);
	if (mcb_owner(s, next) != 0) return false;
	const uint16_t available = uint16_t(size + mcb_size(s, next) + 1);
	largest = available;
	if (available < paragraphs) return false;
	write8(s, mcb, 0, mcb_type(s, next));
	write16(s, mcb, 3, available);
	if (available > paragraphs + 1)
	{
		const uint16_t remainder = uint16_t(mcb + paragraphs + 1);
		write_mcb(s, remainder, mcb_type(s, mcb), 0, uint16_t(available - paragraphs - 1));
		write8(s, mcb, 0, 'M');
		write16(s, mcb, 3, paragraphs);
	}
	return true;
}

void dos_resolve_path(machine_state& s, const char* input, char* out, int out_size)
{
	char drive = char('A' + s.dos.current_drive);
	const char* rest = input;
	if (input[0] != 0 && input[1] == ':')
	{
		drive = upper(input[0]);
		rest = input + 2;
	}

	char work[256] = {};
	int length = 0;
	if (rest[0] != '\\' && rest[0] != '/')
	{
		const int index = (drive - 'A') < 3 ? (drive - 'A') : 2;
		const char* cwd = s.dos.cwd[index];
		for (int i = 0; cwd[i] != 0 && length < 200; ++i) work[length++] = cwd[i];
		// The current directory has no trailing backslash, so the relative
		// part needs one; "\GAMES" and "WL.EXE" are "\GAMES\WL.EXE".
		work[length++] = '\\';
	}

	for (int i = 0; rest[i] != 0 && length < 200; ++i)
	{
		char c = rest[i] == '/' ? '\\' : rest[i];
		if (c == '\\' && (length == 0 || work[length - 1] == '\\')) continue;
		work[length++] = upper(c);
	}
	work[length] = 0;

	// Collapse "." and ".." components.
	char parts[16][16] = {};
	int count = 0;
	int start = 0;
	for (int i = 0; i <= length; ++i)
	{
		if (work[i] != '\\' && work[i] != 0) continue;
		const int piece = i - start;
		if (piece > 0 && piece < 16)
		{
			char name[16] = {};
			std::memcpy(name, work + start, size_t(piece));
			if (std::strcmp(name, ".") == 0)
			{
				// nothing
			}
			else if (std::strcmp(name, "..") == 0)
			{
				if (count > 0) --count;
			}
			else if (count < 16)
			{
				std::memcpy(parts[count++], name, sizeof(name));
			}
		}
		start = i + 1;
	}

	int written = snprintf(out, size_t(out_size), "%c:", drive);
	for (int i = 0; i < count && written < out_size - 1; ++i)
		written += snprintf(out + written, size_t(out_size - written), "\\%s", parts[i]);
	if (written < out_size - 1 && count == 0) snprintf(out + written, size_t(out_size - written), "\\");
}

namespace
{
	int allocate_file_slot(machine_state& s)
	{
		for (int i = 5; i < dos_max_files; ++i) if (!s.dos.files[i].used) return i;
		return -1;
	}

	void set_up_standard_handles(machine_state& s)
	{
		for (int i = 0; i < dos_max_files; ++i) s.dos.files[i] = dos_file();
		for (int i = 0; i < 3; ++i)
		{
			s.dos.files[i].used = true;
			s.dos.files[i].device = 1;
			s.dos.files[i].info = uint8_t(i == 0 ? 0x81 : 0x82);
		}
		s.dos.files[3].used = true;
		s.dos.files[3].device = 2;
		s.dos.files[4].used = true;
		s.dos.files[4].device = 3;
	}

	void console_write(machine_state& s, uint8_t ch)
	{
		vga_write_char(s, s.vga.page, ch, 0x07, s.vga.render_mode != vga_render_mode::text);
		if (ch == '\n') vga_write_char(s, s.vga.page, '\r', 0x07, false);
	}

	// The console read paths block. Rather than stalling the machine we rewind
	// the caller so the INT instruction runs again on the next step.
	bool console_read(machine_state& s, uint16_t& key, bool wait)
	{
		if (bios_pop_key(s, key)) return true;
		if (wait) bios_retry_interrupt(s);
		return false;
	}

	// What DOS's character functions hand a program: a key with no character,
	// such as F1 or an arrow, comes back as 0 and then as its scancode on the
	// next call, which is how a program reads one.
	bool console_char(machine_state& s, uint8_t& ch, bool wait)
	{
		dos_state& d = s.dos;
		if (d.extended_pending)
		{
			d.extended_pending = false;
			ch = d.extended_scancode;
			return true;
		}
		uint16_t key = 0;
		if (!console_read(s, key, wait)) return false;
		ch = uint8_t(key & 0xff);
		if (ch == 0)
		{
			d.extended_pending = true;
			d.extended_scancode = uint8_t(key >> 8);
		}
		return true;
	}

	uint32_t dta_address(machine_state& s)
	{
		return linear_address(uint16_t(s.dos.dta >> 16), uint16_t(s.dos.dta));
	}

	void store_find_result(machine_state& s, const host_file_info& info, int search)
	{
		const uint32_t dta = dta_address(s);
		mem_write16(s, dta + 0, uint16_t(search));
		mem_write8(s, dta + 21, info.attributes);
		mem_write16(s, dta + 22, info.dos_time);
		mem_write16(s, dta + 24, info.dos_date);
		mem_write16(s, dta + 26, uint16_t(info.size));
		mem_write16(s, dta + 28, uint16_t(info.size >> 16));
		for (int i = 0; i < 13; ++i) mem_write8(s, dta + 30 + uint32_t(i), uint8_t(info.name[i]));
	}

	void build_environment(machine_state& s, uint16_t segment, const char* program_path)
	{
		static const char* const variables[] = {
			"COMSPEC=C:\\COMMAND.COM",
			"PATH=C:\\",
			"TEMP=C:\\",
			"PROMPT=$p$g",
		};

		uint16_t offset = 0;
		for (const char* variable : variables)
		{
			for (int i = 0; variable[i] != 0; ++i) write8(s, segment, offset++, uint8_t(variable[i]));
			write8(s, segment, offset++, 0);
		}
		write8(s, segment, offset++, 0); // end of environment
		write16(s, segment, offset, 1); // one string follows
		offset = uint16_t(offset + 2);
		for (int i = 0; program_path[i] != 0; ++i) write8(s, segment, offset++, uint8_t(program_path[i]));
		write8(s, segment, offset++, 0);
	}

	void build_psp(machine_state& s, uint16_t psp, uint16_t environment, uint16_t top, const char* command_line)
	{
		for (int i = 0; i < 256; ++i) write8(s, psp, uint16_t(i), 0);

		write8(s, psp, 0x00, 0xcd); // INT 20h
		write8(s, psp, 0x01, 0x20);
		write16(s, psp, 0x02, top);
		write8(s, psp, 0x05, 0xcd); // far dispatcher: INT 21h, RETF
		write8(s, psp, 0x06, 0x21);
		write8(s, psp, 0x07, 0xcb);
		write16(s, psp, 0x0a, 0x0000); // terminate address
		write16(s, psp, 0x0c, callback_segment);
		write16(s, psp, 0x16, psp); // parent PSP
		for (int i = 0; i < 20; ++i) write8(s, psp, uint16_t(0x18 + i), 0xff);
		write8(s, psp, 0x18, 0x01);
		write8(s, psp, 0x19, 0x01);
		write8(s, psp, 0x1a, 0x01);
		write8(s, psp, 0x1b, 0x00);
		write8(s, psp, 0x1c, 0x02);
		write16(s, psp, 0x2c, environment);
		write16(s, psp, 0x32, 20);
		write16(s, psp, 0x34, 0x0018);
		write16(s, psp, 0x36, psp);
		write8(s, psp, 0x50, 0xcd);
		write8(s, psp, 0x51, 0x21);
		write8(s, psp, 0x52, 0xcb);
		write8(s, psp, 0x5c, 0x00);
		write8(s, psp, 0x6c, 0x00);

		int length = 0;
		while (command_line[length] != 0 && length < 126) ++length;
		write8(s, psp, 0x80, uint8_t(length));
		for (int i = 0; i < length; ++i) write8(s, psp, uint16_t(0x81 + i), uint8_t(command_line[i]));
		write8(s, psp, uint16_t(0x81 + length), 0x0d);
	}
}

void dos_reset(machine_state& s)
{	char saved_cwd[3][64];
	std::memcpy(saved_cwd, s.dos.cwd, sizeof(saved_cwd));
	s.dos = dos_state();
	std::memcpy(s.dos.cwd, saved_cwd, sizeof(saved_cwd));

	s.dos.first_mcb = first_mcb_segment;
	write_mcb(s, first_mcb_segment, 'Z', 0, uint16_t(memory_top_segment - first_mcb_segment - 1));
	set_up_standard_handles(s);
	s.dos.dta = (uint32_t(0) << 16) | 0x80;
}

// Loads a program image into a fresh block of memory and points the CPU at
// its entry. Used both for the first program and for INT 21h AH=4Bh.
static bool load_image(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                       const char* command_line, uint16_t inherited_environment, char* error, int error_size);

bool dos_load_program(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                      const char* command_line, char* error, int error_size)
{
	dos_reset(s);
	return load_image(s, image, size, program_path, command_line, 0, error, error_size);
}

static bool load_image(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                       const char* command_line, uint16_t inherited_environment, char* error, int error_size)
{
	const auto report = [&](const char* message)
	{
		if (error != nullptr) snprintf(error, size_t(error_size), "%s", message);
		return false;
	};

	if (size < 4) return report("File is too small to be a DOS program");

	std::snprintf(s.dos.program_path, sizeof(s.dos.program_path), "%s", program_path);

	// The environment gets its own block, as DOS does, so a program can free
	// it and can walk back to it through the PSP. A child inherits its
	// parent's rather than getting a copy.
	uint16_t largest = 0;
	uint16_t environment = inherited_environment;
	const bool owns_environment = inherited_environment == 0;
	if (owns_environment)
	{
		environment = dos_alloc(s, 0x40, largest);
		if (environment == 0) return report("Out of memory building the environment");
		build_environment(s, environment, program_path);
	}
	s.dos.environment_segment = environment;

	const bool is_exe = image[0] == 'M' && image[1] == 'Z';

	if (is_exe)
	{
		if (size < 0x20) return report("Truncated MZ header");
		const auto word = [&](uint32_t offset) { return uint16_t(image[offset] | (image[offset + 1] << 8)); };
		const uint16_t bytes_in_last_block = word(0x02);
		const uint16_t blocks = word(0x04);
		const uint16_t relocations = word(0x06);
		const uint16_t header_paragraphs = word(0x08);
		const uint16_t min_alloc = word(0x0a);
		const uint16_t max_alloc = word(0x0c);
		const uint16_t initial_ss = word(0x0e);
		const uint16_t initial_sp = word(0x10);
		const uint16_t initial_ip = word(0x14);
		const uint16_t initial_cs = word(0x16);
		const uint16_t relocation_table = word(0x18);

		uint32_t image_length = uint32_t(blocks) * 512;
		if (bytes_in_last_block != 0) image_length = image_length - 512 + bytes_in_last_block;
		if (image_length > size) image_length = size;
		const uint32_t header_length = uint32_t(header_paragraphs) * 16;
		if (header_length > image_length) return report("Corrupt MZ header");
		const uint32_t body = image_length - header_length;

		uint16_t paragraphs = uint16_t(0x10 + ((body + 15) / 16) + min_alloc);
		uint16_t block = dos_alloc(s, paragraphs, largest);
		if (block == 0 && largest > 0x10 + (body + 15) / 16)
		{
			paragraphs = largest;
			block = dos_alloc(s, paragraphs, largest);
		}
		if (block == 0) return report("Not enough memory to load the program");
		if (max_alloc >= largest && largest > paragraphs) dos_resize(s, block, largest, largest);

		const uint16_t psp = block;
		const uint16_t load = uint16_t(psp + 0x10);
		s.dos.psp_segment = psp;
		s.dos.image_segment = load;
		s.dos.image_bytes = body;

		for (uint32_t i = 0; i < body; ++i)
			mem_write8(s, linear_address(load, 0) + i, image[header_length + i]);

		for (uint16_t i = 0; i < relocations; ++i)
		{
			const uint32_t entry = relocation_table + uint32_t(i) * 4;
			if (entry + 4 > size) break;
			const uint16_t offset = uint16_t(image[entry] | (image[entry + 1] << 8));
			const uint16_t segment = uint16_t(image[entry + 2] | (image[entry + 3] << 8));
			const uint32_t address = linear_address(uint16_t(load + segment), offset);
			mem_write16(s, address, uint16_t(mem_read16(s, address) + load));
		}

		build_psp(s, psp, environment, uint16_t(psp + paragraphs), command_line);
		if (owns_environment) write16(s, environment - 1, 1, psp);
		write16(s, psp - 1, 1, psp);

		cpu_state& c = s.cpu;
		c.set_cs(uint16_t(load + initial_cs));
		c.ip = initial_ip;
		c.set_ss(uint16_t(load + initial_ss));
		c.set_sp(initial_sp);
		c.set_ds(psp);
		c.set_es(psp);
		s.dos.entry_cs_ip = (uint32_t(c.cs()) << 16) | c.ip;
		s.dos.entry_ss_sp = (uint32_t(c.ss()) << 16) | c.sp();
	}
	else
	{
		if (size > 0xff00) return report("COM file is larger than a single segment");

		uint16_t paragraphs = 0x1000;
		uint16_t block = dos_alloc(s, paragraphs, largest);
		if (block == 0)
		{
			paragraphs = largest;
			block = dos_alloc(s, paragraphs, largest);
		}
		if (block == 0) return report("Not enough memory to load the program");
		if (largest > paragraphs) dos_resize(s, block, largest, largest);

		const uint16_t psp = block;
		s.dos.psp_segment = psp;
		s.dos.image_segment = psp;
		s.dos.image_bytes = size;

		for (uint32_t i = 0; i < size; ++i)
			mem_write8(s, linear_address(psp, 0x100) + i, image[i]);

		build_psp(s, psp, environment, uint16_t(psp + paragraphs), command_line);
		if (owns_environment) write16(s, environment - 1, 1, psp);
		write16(s, psp - 1, 1, psp);

		cpu_state& c = s.cpu;
		c.set_cs(psp);
		c.ip = 0x100;
		c.set_ss(psp);
		c.set_sp(0xfffe);
		c.set_ds(psp);
		c.set_es(psp);
		write16(s, psp, 0xfffe, 0x0000); // a RET goes to the INT 20h at PSP:0
		s.dos.entry_cs_ip = (uint32_t(c.cs()) << 16) | c.ip;
		s.dos.entry_ss_sp = (uint32_t(c.ss()) << 16) | c.sp();
	}

	s.dos.dta = (uint32_t(s.dos.psp_segment) << 16) | 0x80;
	s.cpu.set_ax(0);
	s.cpu.set_bx(0);
	s.cpu.set_cx(0xff);
	s.cpu.set_dx(s.dos.psp_segment);
	s.cpu.set_si(s.cpu.ip);
	s.cpu.set_di(s.cpu.sp());
	s.cpu.set_bp(0x091c);
	s.cpu.flags = 0x0202;
	return true;
}

namespace
{
	// Releases every block a process owns, which is what happens when it ends.
	void free_owned_memory(machine_state& s, uint16_t owner)
	{
		uint16_t mcb = s.dos.first_mcb;
		for (int guard = 0; guard < 512; ++guard)
		{
			const uint8_t type = mcb_type(s, mcb);
			if (mcb_owner(s, mcb) == owner) write16(s, mcb, 1, 0);
			if (type != 'M') break;
			mcb = uint16_t(mcb + mcb_size(s, mcb) + 1);
		}
		merge_free_blocks(s);
	}

	void terminate(machine_state& s, uint8_t code, uint8_t type)
	{
		s.dos.return_code = code;
		s.dos.return_type = type;

		if (s.dos.exec_depth > 0)
		{
			// A child process ending hands control back to its parent rather
			// than to the host.
			const dos_exec_frame& frame = s.dos.exec_stack[--s.dos.exec_depth];
			free_owned_memory(s, s.dos.psp_segment);
			s.cpu = frame.cpu;
			s.dos.psp_segment = frame.psp;
			s.dos.dta = frame.dta;
			s.cpu.set_flag(flag_cf, false);
			return;
		}

		s.dos.terminated = true;
		s.exited = true;
		s.exit_code = code;
	}

	// INT 21h AH=4Bh. The parent's state is put aside, the child is loaded
	// into free memory, and the CPU is left at the child's entry point.
	bool exec_program(machine_state& s)
	{
		cpu_state& c = s.cpu;
		if (c.al() != 0 || s.host == nullptr) return false;
		if (s.dos.exec_depth >= dos_max_nesting) return false;

		char path[192] = {};
		char raw[160] = {};
		read_string(s, c.ds(), c.dx(), raw, sizeof(raw));
		if (raw[0] == 0) return false;
		dos_resolve_path(s, raw, path, sizeof(path));

		host_file_info info;
		if (!s.host->file_info(path, info) || info.directory || info.size == 0) return false;
		const int handle = s.host->file_open(path, host_open_read);
		if (handle < 0) return false;

		std::vector<uint8_t> image(info.size, 0);
		uint32_t total = 0;
		while (total < info.size)
		{
			const int read = s.host->file_read(handle, image.data() + total, int(info.size - total));
			if (read <= 0) break;
			total += uint32_t(read);
		}
		s.host->file_close(handle);
		if (total == 0) return false;
		image.resize(total);

		// The command tail comes from the parameter block at ES:BX.
		const uint16_t block_segment = c.es();
		const uint16_t block = c.bx();
		const uint16_t environment = read16(s, block_segment, block);
		const uint16_t tail_offset = read16(s, block_segment, uint16_t(block + 2));
		const uint16_t tail_segment = read16(s, block_segment, uint16_t(block + 4));
		const uint8_t length = read8(s, tail_segment, tail_offset);
		char tail[128] = {};
		for (int i = 0; i < length && i < int(sizeof(tail)) - 1; ++i)
			tail[i] = char(read8(s, tail_segment, uint16_t(tail_offset + 1 + i)));

		// Remember where the parent has to carry on. The IRET frame the INT
		// pushed is unwound here, so the parent resumes after its own call.
		dos_exec_frame& frame = s.dos.exec_stack[s.dos.exec_depth];
		frame.cpu = c;
		frame.psp = s.dos.psp_segment;
		frame.dta = s.dos.dta;
		const uint32_t stack = linear_address(c.ss(), c.sp());
		frame.cpu.ip = mem_read16(s, stack);
		frame.cpu.set_cs(mem_read16(s, stack + 2));
		frame.cpu.flags = uint16_t(mem_read16(s, stack + 4) & ~flag_cf);
		frame.cpu.set_sp(uint16_t(c.sp() + 6));

		const uint16_t parent_psp = s.dos.psp_segment;
		char error[128] = {};
		if (!load_image(s, image.data(), uint32_t(image.size()), path, tail,
		                environment != 0 ? environment : s.dos.environment_segment, error, sizeof(error)))
		{
			s.cpu = frame.cpu;
			s.dos.psp_segment = parent_psp;
			return false;
		}

		write16(s, s.dos.psp_segment, 0x16, parent_psp);
		++s.dos.exec_depth;
		return true;
	}

	bool read_dos_path(machine_state& s, uint16_t segment, uint16_t offset, char* out, int out_size)
	{
		char raw[192] = {};
		read_string(s, segment, offset, raw, sizeof(raw));
		if (raw[0] == 0) return false;
		dos_resolve_path(s, raw, out, out_size);
		return true;
	}

	void file_functions(machine_state& s)
	{
		cpu_state& c = s.cpu;
		dos_state& d = s.dos;
		char path[192] = {};

		switch (c.ah())
		{
		case 0x3c: // create
		case 0x5b: // create new
		{
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)))
			{
				fail(s, error_file_not_found);
				return;
			}
			if (c.ah() == 0x5b && s.host != nullptr)
			{
				host_file_info info;
				if (s.host->file_info(path, info))
				{
					fail(s, error_access_denied);
					return;
				}
			}
			const int slot = allocate_file_slot(s);
			if (slot < 0)
			{
				fail(s, error_too_many_files);
				return;
			}
			const int handle = s.host != nullptr ? s.host->file_create(path, uint8_t(c.cx() & 0xff)) : -1;
			if (handle < 0)
			{
				fail(s, error_access_denied);
				return;
			}
			d.files[slot].used = true;
			d.files[slot].host_handle = handle;
			d.files[slot].device = 0;
			snprintf(d.files[slot].path, sizeof(d.files[slot].path), "%s", path);
			c.set_ax(uint16_t(slot));
			succeed(s);
			return;
		}

		case 0x3d: // open
		{
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)))
			{
				fail(s, error_file_not_found);
				return;
			}
			const int slot = allocate_file_slot(s);
			if (slot < 0)
			{
				fail(s, error_too_many_files);
				return;
			}
			const int handle = s.host != nullptr ? s.host->file_open(path, c.al() & 3) : -1;
			if (handle < 0)
			{
				fail(s, error_file_not_found);
				return;
			}
			d.files[slot].used = true;
			d.files[slot].host_handle = handle;
			d.files[slot].device = 0;
			snprintf(d.files[slot].path, sizeof(d.files[slot].path), "%s", path);
			c.set_ax(uint16_t(slot));
			succeed(s);
			return;
		}

		case 0x3e: // close
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (d.files[handle].device == 0 && s.host != nullptr) s.host->file_close(d.files[handle].host_handle);
			if (handle >= 5) d.files[handle] = dos_file();
			succeed(s);
			return;
		}

		case 0x3f: // read
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (d.files[handle].device == 1)
			{
				uint16_t key = 0;
				if (!console_read(s, key, true)) return;
				const uint8_t ch = uint8_t(key & 0xff);
				write8(s, c.ds(), c.dx(), ch == 13 ? uint8_t('\r') : ch);
				console_write(s, ch);
				c.set_ax(1);
				succeed(s);
				return;
			}
			static uint8_t buffer[16384];
			uint16_t total = 0;
			while (total < c.cx())
			{
				const int want = int(c.cx() - total) > int(sizeof(buffer)) ? int(sizeof(buffer)) : int(c.cx() - total);
				const int got = s.host != nullptr ? s.host->file_read(d.files[handle].host_handle, buffer, want) : -1;
				if (got < 0)
				{
					fail(s, error_access_denied);
					return;
				}
				for (int i = 0; i < got; ++i)
					mem_write8(s, linear_address(c.ds(), uint16_t(c.dx() + total + i)), buffer[i]);
				total = uint16_t(total + got);
				if (got < want) break;
			}
			c.set_ax(total);
			succeed(s);
			return;
		}

		case 0x40: // write
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (d.files[handle].device != 0)
			{
				for (uint16_t i = 0; i < c.cx(); ++i)
					console_write(s, read8(s, c.ds(), uint16_t(c.dx() + i)));
				c.set_ax(c.cx());
				succeed(s);
				return;
			}
			static uint8_t buffer[16384];
			uint16_t total = 0;
			while (total < c.cx())
			{
				const int want = int(c.cx() - total) > int(sizeof(buffer)) ? int(sizeof(buffer)) : int(c.cx() - total);
				for (int i = 0; i < want; ++i)
					buffer[i] = read8(s, c.ds(), uint16_t(c.dx() + total + i));
				const int put = s.host != nullptr
					                ? s.host->file_write(d.files[handle].host_handle, buffer, want)
					                : -1;
				if (put < 0)
				{
					fail(s, error_access_denied);
					return;
				}
				total = uint16_t(total + put);
				if (put < want) break;
			}
			c.set_ax(total);
			succeed(s);
			return;
		}

		case 0x41: // delete
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)) || s.host == nullptr ||
				!s.host->file_delete(path))
			{
				fail(s, error_file_not_found);
				return;
			}
			succeed(s);
			return;

		case 0x42: // seek
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used || d.files[handle].device != 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			const int32_t offset = int32_t((uint32_t(c.cx()) << 16) | c.dx());
			const int32_t position = s.host != nullptr
				                         ? s.host->file_seek(d.files[handle].host_handle, offset, c.al())
				                         : -1;
			if (position < 0)
			{
				fail(s, error_invalid_function);
				return;
			}
			c.set_ax(uint16_t(position));
			c.set_dx(uint16_t(uint32_t(position) >> 16));
			succeed(s);
			return;
		}

		case 0x43: // get/set attributes
		{
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)) || s.host == nullptr)
			{
				fail(s, error_file_not_found);
				return;
			}
			if (c.al() == 0)
			{
				host_file_info info;
				if (!s.host->file_info(path, info))
				{
					fail(s, error_file_not_found);
					return;
				}
				c.set_cx(info.attributes);
				succeed(s);
				return;
			}
			if (!s.host->file_set_attributes(path, uint8_t(c.cx() & 0xff)))
			{
				fail(s, error_access_denied);
				return;
			}
			succeed(s);
			return;
		}

		case 0x45: // duplicate handle
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return;
			}
			const int slot = allocate_file_slot(s);
			if (slot < 0)
			{
				fail(s, error_too_many_files);
				return;
			}
			d.files[slot] = d.files[handle];
			if (d.files[handle].device == 0 && s.host != nullptr)
			{
				const int copy = s.host->file_open(d.files[handle].path, host_open_read_write);
				if (copy < 0)
				{
					fail(s, error_access_denied);
					return;
				}
				d.files[slot].host_handle = copy;
			}
			c.set_ax(uint16_t(slot));
			succeed(s);
			return;
		}

		case 0x46: // force duplicate
			succeed(s);
			return;

		case 0x47: // get current directory
		{
			const int drive = c.dl() == 0 ? d.current_drive : (c.dl() - 1);
			const char* cwd = d.cwd[drive < 3 && drive >= 0 ? drive : 2];
			uint16_t offset = c.si();
			for (int i = 0; cwd[i] != 0; ++i) write8(s, c.ds(), offset++, uint8_t(cwd[i]));
			write8(s, c.ds(), offset, 0);
			c.set_ax(0x0100);
			succeed(s);
			return;
		}

		case 0x4e: // find first
		{
			char pattern[192] = {};
			if (!read_dos_path(s, c.ds(), c.dx(), pattern, sizeof(pattern)) || s.host == nullptr)
			{
				fail(s, error_file_not_found);
				return;
			}
			host_file_info info;
			const int search = s.host->find_first(pattern, uint8_t(c.cx() & 0xff), info);
			if (search < 0)
			{
				fail(s, error_file_not_found);
				return;
			}
			store_find_result(s, info, search);
			succeed(s);
			return;
		}

		case 0x4f: // find next
		{
			const uint32_t dta = dta_address(s);
			const int search = int16_t(mem_read16(s, dta));
			host_file_info info;
			if (s.host == nullptr || search < 0 || !s.host->find_next(search, info))
			{
				fail(s, error_no_more_files);
				return;
			}
			store_find_result(s, info, search);
			succeed(s);
			return;
		}

		case 0x56: // rename
		{
			char to[192] = {};
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)) ||
				!read_dos_path(s, c.es(), c.di(), to, sizeof(to)) ||
				s.host == nullptr || !s.host->file_rename(path, to))
			{
				fail(s, error_file_not_found);
				return;
			}
			succeed(s);
			return;
		}

		case 0x57: // get/set file date and time
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (c.al() == 0)
			{
				host_file_info info;
				if (s.host != nullptr && s.host->file_info(d.files[handle].path, info))
				{
					c.set_cx(info.dos_time);
					c.set_dx(info.dos_date);
				}
				else
				{
					c.set_cx(0);
					c.set_dx(0);
				}
			}
			succeed(s);
			return;
		}

		default:
			fail(s, error_invalid_function);
			return;
		}
	}
}

bool dos_interrupt(machine_state& s, uint8_t vector)
{
	cpu_state& c = s.cpu;
	dos_state& d = s.dos;

	if (vector == 0x20)
	{
		terminate(s, 0, 0);
		return true;
	}
	if (vector == 0x27)
	{
		terminate(s, 0, 3);
		return true;
	}
	if (vector == 0x28) return true; // DOS idle
	if (vector == 0x2a)
	{
		c.set_ah(0); // no network
		return true;
	}
	if (vector == 0x2f)
	{
		c.set_al(0); // no multiplex handler installed
		return true;
	}
	if (vector != 0x21) return false;

	switch (c.ah())
	{
	case 0x00:
		terminate(s, 0, 0);
		return true;

	case 0x01: case 0x07: case 0x08:
	{
		const bool second_half = d.extended_pending;
		uint8_t ch = 0;
		if (!console_char(s, ch, true)) return true;
		c.set_al(ch);
		// Neither half of an extended key is a character to echo.
		if (c.ah() == 0x01 && ch != 0 && !second_half) console_write(s, ch);
		return true;
	}

	case 0x02:
		console_write(s, c.dl());
		c.set_al(c.dl());
		return true;

	case 0x03:
		c.set_al(0);
		return true;

	case 0x04: case 0x05:
		return true;

	case 0x06:
		if (c.dl() == 0xff)
		{
			uint8_t ch = 0;
			if (!console_char(s, ch, false))
			{
				cpu_set_iret_flag(s, flag_zf, true);
				c.set_al(0);
				return true;
			}
			cpu_set_iret_flag(s, flag_zf, false);
			c.set_al(ch);
			return true;
		}
		console_write(s, c.dl());
		c.set_al(c.dl());
		return true;

	case 0x09:
	{
		char text[1024] = {};
		read_string(s, c.ds(), c.dx(), text, sizeof(text), '$');
		for (int i = 0; text[i] != 0; ++i) console_write(s, uint8_t(text[i]));
		c.set_al('$');
		return true;
	}

	case 0x0a:
	{
		// Buffered input: gather a line, one key per call, so the machine
		// keeps running while the user types. The count typed so far lives in
		// the buffer's second byte, which on entry to a fresh read is only the
		// template DOS offers for editing, so a new read starts it at zero.
		const uint16_t buffer = c.dx();
		const uint32_t where = (uint32_t(c.ds()) << 16) | buffer;
		if (!d.line_input || d.line_buffer != where)
		{
			d.line_input = true;
			d.line_buffer = where;
			write8(s, c.ds(), uint16_t(buffer + 1), 0);
		}
		const uint8_t capacity = read8(s, c.ds(), buffer);
		uint8_t length = read8(s, c.ds(), uint16_t(buffer + 1));
		uint16_t key = 0;
		if (!console_read(s, key, true)) return true;

		const uint8_t ch = uint8_t(key & 0xff);
		if (ch == 13)
		{
			// Only the carriage return is echoed, as DOS does: the cursor
			// goes back to the start of the line and the caller moves on.
			write8(s, c.ds(), uint16_t(buffer + 2 + length), 13);
			write8(s, c.ds(), uint16_t(buffer + 1), length);
			console_write(s, '\r');
			d.line_input = false;
			return true;
		}
		if (ch == 8)
		{
			if (length > 0)
			{
				--length;
				console_write(s, 8);
				console_write(s, ' ');
				console_write(s, 8);
			}
		}
		else if (ch != 0 && length + 1 < capacity)
		{
			write8(s, c.ds(), uint16_t(buffer + 2 + length), ch);
			++length;
			console_write(s, ch);
		}
		write8(s, c.ds(), uint16_t(buffer + 1), length);
		bios_retry_interrupt(s);
		return true;
	}

	case 0x0b:
	{
		uint16_t key = 0;
		c.set_al(d.extended_pending || bios_peek_key(s, key) ? 0xff : 0x00);
		return true;
	}

	case 0x0c:
		// Flush the buffer, then run the requested input function.
		{
			uint16_t key = 0;
			while (bios_pop_key(s, key)) {}
			d.extended_pending = false;
			if (c.al() == 0x01 || c.al() == 0x06 || c.al() == 0x07 || c.al() == 0x08 || c.al() == 0x0a)
			{
				c.set_ah(c.al());
				return dos_interrupt(s, 0x21);
			}
			c.set_al(0);
			return true;
		}

	case 0x0d:
		return true;

	case 0x0e:
		d.current_drive = uint8_t(c.dl() < 3 ? c.dl() : 2);
		c.set_al(3); // three drives
		return true;

	case 0x19:
		c.set_al(d.current_drive);
		return true;

	case 0x1a:
		d.dta = (uint32_t(c.ds()) << 16) | c.dx();
		return true;

	case 0x1b: case 0x1c:
		c.set_al(0xff);
		c.set_cx(512);
		c.set_dx(0x1000);
		c.set_ds(callback_segment);
		c.set_bx(0);
		return true;

	case 0x25:
		mem_write16(s, uint32_t(c.al()) * 4, c.dx());
		mem_write16(s, uint32_t(c.al()) * 4 + 2, c.ds());
		return true;

	case 0x29:
	{
		// Parse a filename into an FCB. Only the pieces programs actually use
		// are filled in: drive, name and extension.
		char raw[64] = {};
		read_string(s, c.ds(), c.si(), raw, sizeof(raw));
		const uint16_t fcb = c.di();
		write8(s, c.es(), fcb, 0);
		for (int i = 0; i < 11; ++i) write8(s, c.es(), uint16_t(fcb + 1 + i), ' ');
		int index = 0;
		int position = 0;
		while (raw[index] != 0 && raw[index] != '.' && position < 8)
			write8(s, c.es(), uint16_t(fcb + 1 + position++), uint8_t(upper(raw[index++])));
		while (raw[index] != 0 && raw[index] != '.') ++index;
		if (raw[index] == '.')
		{
			++index;
			position = 0;
			while (raw[index] != 0 && position < 3)
				write8(s, c.es(), uint16_t(fcb + 9 + position++), uint8_t(upper(raw[index++])));
		}
		c.set_al(0);
		c.set_si(uint16_t(c.si() + index));
		return true;
	}

	case 0x2a: case 0x2c:
	{
		int year = 1990, month = 1, day = 1, weekday = 1, hour = 0, minute = 0, second = 0, hundredths = 0;
		if (s.host != nullptr) s.host->clock(year, month, day, weekday, hour, minute, second, hundredths);
		if (c.ah() == 0x2a)
		{
			c.set_cx(uint16_t(year));
			c.set_dh(uint8_t(month));
			c.set_dl(uint8_t(day));
			c.set_al(uint8_t(weekday));
		}
		else
		{
			c.set_ch(uint8_t(hour));
			c.set_cl(uint8_t(minute));
			c.set_dh(uint8_t(second));
			c.set_dl(uint8_t(hundredths));
		}
		return true;
	}

	case 0x2b: case 0x2d:
		c.set_al(0);
		return true;

	case 0x2e:
		d.verify = c.al();
		return true;

	case 0x2f:
		c.set_es(uint16_t(d.dta >> 16));
		c.set_bx(uint16_t(d.dta));
		return true;

	case 0x30:
		c.set_ax(0x0005); // DOS 5.00
		c.set_bx(0);
		c.set_cx(0);
		return true;

	case 0x31:
		terminate(s, c.al(), 3);
		return true;

	case 0x33:
		if (c.al() == 0) c.set_dl(uint8_t(d.ctrl_break ? 1 : 0));
		else if (c.al() == 1) d.ctrl_break = c.dl() != 0;
		else if (c.al() == 5) c.set_dl(3); // boot drive C:
		return true;

	case 0x34:
		c.set_es(callback_segment);
		c.set_bx(0x0100); // InDOS flag, always zero
		return true;

	case 0x35:
		c.set_bx(mem_read16(s, uint32_t(c.al()) * 4));
		c.set_es(mem_read16(s, uint32_t(c.al()) * 4 + 2));
		return true;

	case 0x36:
	{
		const uint32_t free = s.host != nullptr ? s.host->free_space() : 0x1000000;
		const uint32_t clusters = free / (512 * 8);
		c.set_ax(8); // sectors per cluster
		c.set_cx(512); // bytes per sector
		c.set_bx(uint16_t(clusters > 0xfff0 ? 0xfff0 : clusters));
		c.set_dx(0xfff0);
		return true;
	}

	case 0x37:
		if (c.al() == 0) c.set_dl(uint8_t(d.switch_char));
		else if (c.al() == 1) d.switch_char = c.dl();
		return true;

	case 0x38:
		// Country information: US defaults.
		for (int i = 0; i < 34; ++i) write8(s, c.ds(), uint16_t(c.dx() + i), 0);
		write8(s, c.ds(), uint16_t(c.dx() + 7), '$');
		write8(s, c.ds(), uint16_t(c.dx() + 9), ',');
		write8(s, c.ds(), uint16_t(c.dx() + 11), '.');
		write8(s, c.ds(), uint16_t(c.dx() + 13), '-');
		write8(s, c.ds(), uint16_t(c.dx() + 15), ':');
		c.set_bx(1);
		succeed(s);
		return true;

	case 0x39: case 0x3a: case 0x3b:
	{
		char path[192] = {};
		if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path)) || s.host == nullptr)
		{
			fail(s, error_path_not_found);
			return true;
		}
		bool ok = false;
		if (c.ah() == 0x39) ok = s.host->make_directory(path);
		else if (c.ah() == 0x3a) ok = s.host->remove_directory(path);
		else if (s.host->directory_exists(path))
		{
			const int drive = (path[0] - 'A') < 3 ? (path[0] - 'A') : 2;
			snprintf(d.cwd[drive], sizeof(d.cwd[drive]), "%s", path + 2);
			ok = true;
		}
		if (!ok)
		{
			fail(s, error_path_not_found);
			return true;
		}
		succeed(s);
		return true;
	}

	case 0x3c: case 0x3d: case 0x3e: case 0x3f: case 0x40: case 0x41: case 0x42:
	case 0x43: case 0x45: case 0x46: case 0x47: case 0x4e: case 0x4f: case 0x56:
	case 0x57: case 0x5b:
		file_functions(s);
		return true;

	case 0x44: // IOCTL
		switch (c.al())
		{
		case 0x00:
		{
			const uint16_t handle = c.bx();
			if (handle >= dos_max_files || !d.files[handle].used)
			{
				fail(s, error_invalid_handle);
				return true;
			}
			c.set_dx(d.files[handle].device != 0 ? uint16_t(d.files[handle].info) : 0x0000);
			succeed(s);
			return true;
		}
		case 0x01:
			succeed(s);
			return true;
		case 0x08:
			c.set_ax(1); // fixed disk
			succeed(s);
			return true;
		default:
			fail(s, error_invalid_function);
			return true;
		}

	case 0x48:
	{
		uint16_t largest = 0;
		const uint16_t block = dos_alloc(s, c.bx(), largest);
		if (block == 0)
		{
			c.set_bx(largest);
			fail(s, error_out_of_memory);
			return true;
		}
		c.set_ax(block);
		succeed(s);
		return true;
	}

	case 0x49:
		if (!dos_free(s, c.es()))
		{
			fail(s, error_invalid_function);
			return true;
		}
		succeed(s);
		return true;

	case 0x4a:
	{
		uint16_t largest = 0;
		if (!dos_resize(s, c.es(), c.bx(), largest))
		{
			c.set_bx(largest);
			fail(s, error_out_of_memory);
			return true;
		}
		succeed(s);
		return true;
	}

	case 0x4b:
		if (!exec_program(s))
		{
			fail(s, error_file_not_found);
			return true;
		}
		// The CPU is now at the child's entry point, so there is nothing to
		// return to; the IRET frame was unwound into the saved parent state.
		return true;

	case 0x4c:
		terminate(s, c.al(), 0);
		return true;

	case 0x4d:
		c.set_al(d.return_code);
		c.set_ah(d.return_type);
		return true;

	case 0x50:
		d.psp_segment = c.bx();
		return true;

	case 0x51: case 0x62:
		c.set_bx(d.psp_segment);
		return true;

	case 0x52:
		// List of lists: point at a zeroed area so a program that walks it
		// finds nothing rather than garbage.
		c.set_es(callback_segment);
		c.set_bx(0x0200);
		return true;

	case 0x54:
		c.set_al(d.verify);
		return true;

	case 0x58:
		if (c.al() == 0) c.set_ax(0);
		succeed(s);
		return true;

	case 0x59:
		c.set_ax(0);
		c.set_bh(0);
		c.set_bl(0);
		c.set_ch(0);
		return true;

	case 0x5a:
	{
		// Create a temporary file: append a name to the path the caller gave.
		char path[192] = {};
		char raw[160] = {};
		read_string(s, c.ds(), c.dx(), raw, sizeof(raw));
		int length = int(std::strlen(raw));
		snprintf(raw + length, sizeof(raw) - size_t(length), "OVR%05u.TMP", unsigned(s.cycles % 100000));
		dos_resolve_path(s, raw, path, sizeof(path));
		for (int i = 0; raw[i] != 0; ++i) write8(s, c.ds(), uint16_t(c.dx() + i), uint8_t(raw[i]));
		write8(s, c.ds(), uint16_t(c.dx() + std::strlen(raw)), 0);
		const int slot = allocate_file_slot(s);
		const int handle = s.host != nullptr && slot >= 0 ? s.host->file_create(path, 0) : -1;
		if (handle < 0)
		{
			fail(s, error_access_denied);
			return true;
		}
		d.files[slot].used = true;
		d.files[slot].host_handle = handle;
		snprintf(d.files[slot].path, sizeof(d.files[slot].path), "%s", path);
		c.set_ax(uint16_t(slot));
		succeed(s);
		return true;
	}

	case 0x63:
		c.set_al(0);
		succeed(s);
		return true;

	case 0x65:
		c.set_ax(0);
		succeed(s);
		return true;

	case 0x67:
		succeed(s);
		return true;

	case 0x68: case 0x6a:
		succeed(s);
		return true;

	default:
		fail(s, error_invalid_function);
		return true;
	}
}
