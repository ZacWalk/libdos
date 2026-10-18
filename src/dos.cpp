// libdos by Zac Walker
//
// The DOS personality. There is no DOS kernel in memory: the MCB chain, the
// PSP and the INT 21h entry points are built by the emulator so that programs
// see exactly the interface they were written against, while file access is
// handed to the host.

#include "machine.h"
#include "command.h"

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
	constexpr uint8_t error_bad_format = 0x0b;
	constexpr uint8_t error_invalid_function = 0x01;
	constexpr uint8_t error_no_more_files = 0x12;
	constexpr uint8_t error_file_exists = 0x50;

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

	// Collapse "." and ".." components. A component is never dropped for its
	// length: a name too long for DOS is cut to 8.3 before it gets here, and
	// losing one silently would turn C:\LONGNAME\FILE into C:\FILE.
	char parts[16][64] = {};
	int count = 0;
	int start = 0;
	for (int i = 0; i <= length; ++i)
	{
		if (work[i] != '\\' && work[i] != 0) continue;
		const int piece = i - start < 63 ? i - start : 63;
		if (piece > 0)
		{
			char name[64] = {};
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
	// The handle table of a process lives in its PSP, as on DOS: PSP:32h is
	// its size and PSP:34h a far pointer to it, normally PSP:18h. Each byte is
	// an index into the system file table, FFh for a closed handle.
	uint32_t handle_table(machine_state& s, uint16_t psp, int& size)
	{
		size = read16(s, psp, 0x32);
		if (size > 255) size = 255;
		return linear_address(read16(s, psp, 0x36), read16(s, psp, 0x34));
	}

	int allocate_file(machine_state& s)
	{
		for (int i = 0; i < dos_max_files; ++i)
			if (s.dos.files[i].references == 0 && !s.dos.files[i].permanent) return i;
		return -1;
	}

	// The lowest closed handle of the current process, or -1.
	int allocate_handle(machine_state& s)
	{
		int size = 0;
		const uint32_t table = handle_table(s, s.dos.psp_segment, size);
		for (int i = 0; i < size; ++i)
			if (mem_read8(s, table + uint32_t(i)) == 0xff) return i;
		return -1;
	}

	// The three devices DOS opens before any program runs, in the order the
	// first PSP's handle table (01 01 01 00 02) refers to them.
	void set_up_standard_handles(machine_state& s)
	{
		for (dos_file& file : s.dos.files) file = dos_file();
		static const uint8_t devices[] = {dos_device_aux, dos_device_con, dos_device_prn};
		static const char* const names[] = {"AUX", "CON", "PRN"};
		for (int i = 0; i < 3; ++i)
		{
			s.dos.files[i].device = devices[i];
			s.dos.files[i].permanent = true;
			snprintf(s.dos.files[i].path, sizeof(s.dos.files[i].path), "%s", names[i]);
		}
	}

	void close_handles(machine_state& s, uint16_t psp)
	{
		int size = 0;
		const uint32_t table = handle_table(s, psp, size);
		for (int i = 0; i < size; ++i)
		{
			const uint8_t file = mem_read8(s, table + uint32_t(i));
			if (file == 0xff) continue;
			mem_write8(s, table + uint32_t(i), 0xff);
			if (file < dos_max_files) dos_release_file(s, file);
		}
	}

	// A child gets every handle its parent has open except those opened with
	// bit 7 of the mode set. The entries are shared, so a child that moves a
	// file's position moves its parent's too.
	void inherit_handles(machine_state& s, uint16_t parent, uint16_t child)
	{
		int size = 0;
		const uint32_t from = handle_table(s, parent, size);
		for (int i = 0; i < dos_handles; ++i)
		{
			uint8_t file = i < size ? mem_read8(s, from + uint32_t(i)) : uint8_t(0xff);
			if (file != 0xff && (file >= dos_max_files || s.dos.files[file].references == 0 ||
			                     (s.dos.files[file].mode & 0x80) != 0))
				file = 0xff;
			if (file != 0xff) ++s.dos.files[file].references;
			write8(s, child, uint16_t(0x18 + i), file);
		}
		write16(s, child, 0x32, dos_handles);
		write16(s, child, 0x34, 0x0018);
		write16(s, child, 0x36, child);
	}

	// DOS reserves these names in every directory and with any extension.
	uint8_t device_named(const char* path)
	{
		const char* name = path;
		for (const char* p = path; *p != 0; ++p)
			if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
		char base[9] = {};
		for (int i = 0; i < 8 && name[i] != 0 && name[i] != '.'; ++i) base[i] = upper(name[i]);
		if (std::strcmp(base, "CON") == 0) return dos_device_con;
		if (std::strcmp(base, "NUL") == 0) return dos_device_nul;
		if (std::strcmp(base, "AUX") == 0 || (std::strncmp(base, "COM", 3) == 0 && base[3] >= '1' && base[3] <= '4' &&
		                                      base[4] == 0))
			return dos_device_aux;
		if (std::strcmp(base, "PRN") == 0 || (std::strncmp(base, "LPT", 3) == 0 && base[3] >= '1' && base[3] <= '3' &&
		                                      base[4] == 0))
			return dos_device_prn;
		return dos_device_none;
	}

	// DOS keeps eight characters of a name and three of its extension and
	// ignores the rest, so LINK's "ramdrive.map -map" is RAMDRIVE.MAP. Applied
	// to the last component of a path a program creates, opens or deletes.
	void cut_to_8_3(char* path)
	{
		char* name = path;
		for (char* p = path; *p != 0; ++p)
			if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
		if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return;
		char* dot = std::strchr(name, '.');
		const size_t base = dot != nullptr ? size_t(dot - name) : std::strlen(name);
		if (dot != nullptr && std::strlen(dot + 1) > 3) dot[4] = 0;
		if (base > 8) std::memmove(name + 8, name + base, std::strlen(name + base) + 1);
	}

	void screen_write(machine_state& s, uint8_t ch)
	{
		vga_write_char(s, s.vga.page, ch, 0x07, s.vga.render_mode != vga_render_mode::text);
		if (ch == '\n') vga_write_char(s, s.vga.page, '\r', 0x07, false);
	}

	// What reaches the console device: the host's terminal if it has taken the
	// console over, the emulated screen otherwise.
	void console_output(machine_state& s, uint16_t handle, const uint8_t* data, int length)
	{
		if (s.host != nullptr && s.host->console_write(handle, data, length)) return;
		for (int i = 0; i < length; ++i) screen_write(s, data[i]);
	}

	void console_write(machine_state& s, uint8_t ch)
	{
		dos_write_handle(s, 1, &ch, 1);
	}

	// Whether standard input is the console rather than a redirected file or
	// device, and the host has not taken the console over.
	bool keyboard_input(machine_state& s)
	{
		const int file = dos_handle_file(s, 0);
		if (file < 0 || s.dos.files[file].device != dos_device_con) return false;
		uint8_t probe = 0;
		return s.host == nullptr || s.host->console_read(&probe, 0) < 0;
	}

	// The console read paths block. Rather than stalling the machine we rewind
	// the caller so the INT instruction runs again on the next step.
	bool console_read(machine_state& s, uint16_t& key, bool wait)
	{
		if (!keyboard_input(s))
		{
			// Redirected input, or the host's: a byte at a time, and ^Z once
			// it runs out, which is what DOS hands a program at end of file.
			uint8_t ch = 0;
			key = dos_read_handle(s, 0, &ch, 1) == 1 ? ch : 0x1a;
			return true;
		}
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

	void store_find_result(machine_state& s, const host_file_info& info, int slot, uint32_t stamp)
	{
		// The reserved start of the DTA holds which search this is, as DOS
		// keeps its own search state there.
		const uint32_t dta = dta_address(s);
		mem_write16(s, dta + 0, uint16_t(slot));
		mem_write16(s, dta + 2, uint16_t(stamp));
		mem_write16(s, dta + 4, uint16_t(stamp >> 16));
		mem_write8(s, dta + 21, info.attributes);
		mem_write16(s, dta + 22, info.dos_time);
		mem_write16(s, dta + 24, info.dos_date);
		mem_write16(s, dta + 26, uint16_t(info.size));
		mem_write16(s, dta + 28, uint16_t(info.size >> 16));
		for (int i = 0; i < 13; ++i) mem_write8(s, dta + 30 + uint32_t(i), uint8_t(info.name[i]));
	}

	// A program never says it has finished with a directory search - one that
	// only wants to know whether a file exists asks for the first match and
	// moves on - so the host's searches would run out. A search for one name
	// is closed at once; of the rest, the oldest makes way for a new one.
	int open_search_slot(machine_state& s)
	{
		dos_state& d = s.dos;
		int oldest = 0;
		for (int i = 0; i < dos_max_searches; ++i)
		{
			if (!d.searches[i].used) return i;
			if (d.searches[i].stamp < d.searches[oldest].stamp) oldest = i;
		}
		if (s.host != nullptr) s.host->find_close(d.searches[oldest].host_search);
		d.searches[oldest] = dos_search();
		return oldest;
	}

	// Every program gets its own copy of an environment, as on DOS: the
	// NAME=VALUE strings, an empty one to end them, a count of 1 and the
	// program's own path - which is where a C runtime finds argv[0]. `source`
	// is the block to copy; with none, the embedder's configured environment
	// or the default is used. Returns the new block's segment, 0 if there is
	// no memory for it.
	uint16_t make_environment(machine_state& s, uint16_t source, const char* program_path)
	{
		static const char default_strings[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0TEMP=C:\\\0PROMPT=$p$g\0";
		std::vector<uint8_t> strings;
		if (source != 0)
		{
			for (uint32_t i = 0; i < 0x8000; ++i)
			{
				const uint8_t ch = read8(s, source, uint16_t(i));
				if (ch == 0 && (strings.empty() || strings.back() == 0)) break;
				strings.push_back(ch);
			}
		}
		else if (s.dos.config.environment_length > 0)
		{
			strings.assign(s.dos.config.environment, s.dos.config.environment + s.dos.config.environment_length);
		}
		else
		{
			strings.assign(default_strings, default_strings + sizeof(default_strings) - 1);
		}
		if (!strings.empty() && strings.back() != 0) strings.push_back(0);

		const size_t path_length = std::strlen(program_path);
		const size_t bytes = strings.size() + 1 + 2 + path_length + 1;
		uint16_t largest = 0;
		const uint16_t segment = dos_alloc(s, uint16_t((bytes + 15) / 16), largest);
		if (segment == 0) return 0;

		uint16_t offset = 0;
		for (const uint8_t ch : strings) write8(s, segment, offset++, ch);
		write8(s, segment, offset++, 0); // the empty string that ends them
		write16(s, segment, offset, 1); // one string follows
		offset = uint16_t(offset + 2);
		for (size_t i = 0; i < path_length; ++i) write8(s, segment, offset++, uint8_t(program_path[i]));
		write8(s, segment, offset, 0);
		return segment;
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
		// The terminate, Ctrl+C and critical error vectors as they are now, so
		// that whatever the program hooks is put back when it ends.
		for (int i = 0; i < 3; ++i)
		{
			write16(s, psp, uint16_t(0x0a + i * 4), mem_read16(s, uint32_t(0x22 + i) * 4));
			write16(s, psp, uint16_t(0x0c + i * 4), mem_read16(s, uint32_t(0x22 + i) * 4 + 2));
		}
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

	// Builds a loaded program's PSP and gives it its blocks and its handles.
	// PSP:0002 is the end of the program's block - all the memory the program
	// was given, which the C runtime sizes its data segment and heap from.
	void finish_psp(machine_state& s, uint16_t psp, uint16_t environment, uint16_t parent, const char* command_line)
	{
		build_psp(s, psp, environment, uint16_t(psp + mcb_size(s, uint16_t(psp - 1))), command_line);
		write16(s, uint16_t(environment - 1), 1, psp);
		write16(s, uint16_t(psp - 1), 1, psp);
		if (parent != 0)
		{
			inherit_handles(s, parent, psp);
		}
		else
		{
			// build_psp's table is 01 01 01 00 02: three handles on CON and
			// one each on AUX and PRN.
			s.dos.files[1].references += 3;
			s.dos.files[0].references += 1;
			s.dos.files[2].references += 1;
		}
	}
}

void dos_reset(machine_state& s)
{
	// Whatever the last program left open on the host is closed: DOS starts
	// again, the host carries on.
	if (s.host != nullptr)
	{
		for (const dos_search& search : s.dos.searches)
			if (search.used) s.host->find_close(search.host_search);
		for (const dos_file& file : s.dos.files)
			if (file.references > 0 && file.device == dos_device_none && file.host_handle >= 0)
				s.host->file_close(file.host_handle);
	}

	// The current directories and the embedder's settings outlive a reset;
	// everything else starts again.
	char saved_cwd[3][64];
	std::memcpy(saved_cwd, s.dos.cwd, sizeof(saved_cwd));
	const dos_config config = s.dos.config;
	s.dos = dos_state();
	std::memcpy(s.dos.cwd, saved_cwd, sizeof(saved_cwd));
	s.dos.config = config;

	s.dos.first_mcb = first_mcb_segment;
	write_mcb(s, first_mcb_segment, 'Z', 0, uint16_t(memory_top_segment - first_mcb_segment - 1));
	set_up_standard_handles(s);
	s.dos.dta = (uint32_t(0) << 16) | 0x80;
}

// Loads a program image into a fresh block of memory and points the CPU at
// its entry. Used both for the first program and for INT 21h AH=4Bh, where
// `parent` is the PSP the child inherits its handles from.
static bool load_image(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                       const char* command_line, uint16_t environment_source, uint16_t parent, char* error,
                       int error_size);

bool dos_load_program(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                      const char* command_line, char* error, int error_size)
{
	dos_reset(s);
	return load_image(s, image, size, program_path, command_line, 0, 0, error, error_size);
}

static bool load_image(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                       const char* command_line, uint16_t environment_source, uint16_t parent, char* error,
                       int error_size)
{
	uint16_t environment = 0;
	const auto report = [&](const char* message)
	{
		if (environment != 0) dos_free(s, environment);
		if (error != nullptr) snprintf(error, size_t(error_size), "%s", message);
		return false;
	};

	if (size < 4) return report("File is too small to be a DOS program");

	std::snprintf(s.dos.program_path, sizeof(s.dos.program_path), "%s", program_path);

	// The environment gets its own block, as DOS does, so a program can free
	// it and can walk back to it through the PSP.
	uint16_t largest = 0;
	environment = make_environment(s, environment_source, program_path);
	if (environment == 0) return report("Out of memory building the environment");
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

		finish_psp(s, psp, environment, parent, command_line);

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

		finish_psp(s, psp, environment, parent, command_line);

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
		dos_state& d = s.dos;
		d.return_code = code;
		d.return_type = type;

		// What a process hooked is put back as it was when it started, and
		// unless it stays resident its files are closed: a program that leaves
		// them open has DOS close them.
		const uint16_t psp = d.psp_segment;
		for (int i = 0; i < 3; ++i)
		{
			mem_write16(s, uint32_t(0x22 + i) * 4, read16(s, psp, uint16_t(0x0a + i * 4)));
			mem_write16(s, uint32_t(0x22 + i) * 4 + 2, read16(s, psp, uint16_t(0x0c + i * 4)));
		}
		if (type != 3) close_handles(s, psp);

		if (d.exec_depth > 0)
		{
			// A child process ending hands control back to its parent rather
			// than to the host.
			const dos_exec_frame& frame = d.exec_stack[--d.exec_depth];
			if (type != 3) free_owned_memory(s, psp);
			s.cpu = frame.cpu;
			d.psp_segment = frame.psp;
			d.dta = frame.dta;
			d.environment_segment = read16(s, frame.psp, 0x2c);
			s.cpu.set_flag(flag_cf, false);
			return;
		}

		d.terminated = true;
		s.exited = true;
		s.exit_code = code;
	}

	// Reads a whole program through the host. Returns false if it cannot.
	bool read_program_image(machine_state& s, const char* path, std::vector<uint8_t>& image)
	{
		if (s.host == nullptr) return false;
		host_file_info info;
		if (!s.host->file_info(path, info) || info.directory || info.size == 0) return false;
		const int handle = s.host->file_open(path, host_open_read);
		if (handle < 0) return false;

		image.assign(info.size, 0);
		uint32_t total = 0;
		while (total < info.size)
		{
			const int read = s.host->file_read(handle, image.data() + total, int(info.size - total));
			if (read <= 0) break;
			total += uint32_t(read);
		}
		s.host->file_close(handle);
		image.resize(total);
		return total > 0;
	}

	// The name a program is known by: "C:\TOOLS\LINK.EXE" is LINK.
	void base_name(const char* path, char* out, int out_size)
	{
		const char* name = path;
		for (const char* p = path; *p != 0; ++p)
			if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
		int length = 0;
		while (name[length] != 0 && name[length] != '.' && length < out_size - 1)
		{
			out[length] = upper(name[length]);
			++length;
		}
		out[length] = 0;
	}

	// INT 21h AH=4Bh AL=03h: load an overlay at a segment the caller owns,
	// relocated by the factor it gives, without making it a process.
	void load_overlay(machine_state& s, const char* path, uint16_t block_segment, uint16_t block)
	{
		std::vector<uint8_t> image;
		if (!read_program_image(s, path, image))
		{
			fail(s, error_file_not_found);
			return;
		}
		const uint16_t segment = read16(s, block_segment, block);
		const uint16_t factor = read16(s, block_segment, uint16_t(block + 2));
		const auto word = [&](uint32_t offset) { return uint16_t(image[offset] | (image[offset + 1] << 8)); };

		uint32_t header = 0;
		uint32_t body = uint32_t(image.size());
		const bool is_exe = image.size() >= 0x20 && image[0] == 'M' && image[1] == 'Z';
		if (is_exe)
		{
			uint32_t length = uint32_t(word(0x04)) * 512;
			if (word(0x02) != 0) length = length - 512 + word(0x02);
			if (length > image.size()) length = uint32_t(image.size());
			header = uint32_t(word(0x08)) * 16;
			if (header > length)
			{
				fail(s, 11); // invalid format
				return;
			}
			body = length - header;
		}

		for (uint32_t i = 0; i < body; ++i) mem_write8(s, linear_address(segment, 0) + i, image[header + i]);
		if (is_exe)
		{
			const uint32_t table = word(0x18);
			for (uint32_t i = 0; i < word(0x06); ++i)
			{
				const uint32_t entry = table + i * 4;
				if (entry + 4 > image.size()) break;
				const uint32_t address = linear_address(uint16_t(segment + word(entry + 2)), word(entry));
				mem_write16(s, address, uint16_t(mem_read16(s, address) + factor));
			}
		}
		succeed(s);
	}

	// INT 21h AH=4Bh.
	void exec(machine_state& s)
	{
		cpu_state& c = s.cpu;
		dos_state& d = s.dos;

		char raw[160] = {};
		read_string(s, c.ds(), c.dx(), raw, sizeof(raw));
		if (raw[0] == 0)
		{
			fail(s, error_file_not_found);
			return;
		}
		char path[192] = {};
		dos_resolve_path(s, raw, path, sizeof(path));
		const uint16_t block_segment = c.es();
		const uint16_t block = c.bx();

		if (c.al() == 3)
		{
			load_overlay(s, path, block_segment, block);
			return;
		}
		if (c.al() != 0)
		{
			fail(s, error_invalid_function);
			return;
		}

		// The command tail comes from the parameter block at ES:BX.
		const uint16_t environment = read16(s, block_segment, block);
		const uint16_t tail_offset = read16(s, block_segment, uint16_t(block + 2));
		const uint16_t tail_segment = read16(s, block_segment, uint16_t(block + 4));
		const uint8_t length = read8(s, tail_segment, tail_offset);
		char tail[128] = {};
		for (int i = 0; i < length && i < int(sizeof(tail)) - 1; ++i)
			tail[i] = char(read8(s, tail_segment, uint16_t(tail_offset + 1 + i)));

		char name[16] = {};
		base_name(path, name, sizeof(name));

		// COMMAND.COM /C is mapped, not run: the line is carried out by the
		// built-in interpreter, which starts any program it names as a child
		// of this caller.
		if (std::strcmp(name, "COMMAND") == 0)
		{
			uint8_t error = 0;
			switch (command_exec(s, tail, environment, error))
			{
			case command_result::done:
				succeed(s);
				return;
			case command_result::started:
				return;
			case command_result::failed:
				fail(s, error);
				return;
			case command_result::not_a_command:
				break; // no /C: the real COMMAND.COM, if there is one
			}
		}

		// A program the host stands in for.
		if (s.host != nullptr)
		{
			char directory[96] = {};
			const char* cwd = d.cwd[d.current_drive < 3 ? d.current_drive : 2];
			snprintf(directory, sizeof(directory), "%c:%s", char('A' + d.current_drive), cwd[0] != 0 ? cwd : "\\");
			const int code = s.host->run_native(name, tail, directory);
			if (code >= 0)
			{
				d.return_code = uint8_t(code);
				d.return_type = 0;
				succeed(s);
				return;
			}
		}

		uint8_t error = 0;
		if (!dos_start_child(s, path, tail, environment, -1, -1, error)) fail(s, error);
	}

	// Reads a path from the program, resolved. A program that names a file to
	// create, open or delete has the name cut to 8.3 first, as DOS cuts it.
	bool read_dos_path(machine_state& s, uint16_t segment, uint16_t offset, char* out, int out_size,
	                   bool file_name = false)
	{
		char raw[192] = {};
		read_string(s, segment, offset, raw, sizeof(raw));
		if (raw[0] == 0) return false;
		if (file_name) cut_to_8_3(raw);
		dos_resolve_path(s, raw, out, out_size);
		return true;
	}

	// Whether the host has taken the console over.
	bool host_console(machine_state& s)
	{
		uint8_t probe = 0;
		return s.host != nullptr && s.host->console_read(&probe, 0) >= 0;
	}

	// Gives a new handle of the current process the system file table entry
	// just opened for it, and returns the handle to the program.
	void return_new_handle(machine_state& s, int handle, int file)
	{
		dos_point_handle(s, uint16_t(handle), file);
		s.cpu.set_ax(uint16_t(handle));
		succeed(s);
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
		case 0x3d: // open
		{
			const bool create = c.ah() != 0x3d;
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path), true))
			{
				fail(s, error_file_not_found);
				return;
			}
			if (c.ah() == 0x5b && s.host != nullptr && device_named(path) == dos_device_none)
			{
				host_file_info info;
				if (s.host->file_info(path, info))
				{
					fail(s, error_file_exists);
					return;
				}
			}
			const int handle = allocate_handle(s);
			if (handle < 0)
			{
				fail(s, error_too_many_files);
				return;
			}
			const int file = create
				                 ? dos_open_file(s, path, host_open_read_write, true, false, uint8_t(c.cx() & 0x27))
				                 : dos_open_file(s, path, c.al() & 3, false, false);
			if (file < 0)
			{
				fail(s, create ? error_access_denied : error_file_not_found);
				return;
			}
			if (!create) d.files[file].mode = c.al();
			return_new_handle(s, handle, file);
			return;
		}

		case 0x3e: // close
		{
			if (dos_handle_file(s, c.bx()) < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			dos_release_file(s, dos_point_handle(s, c.bx(), -1));
			succeed(s);
			return;
		}

		case 0x3f: // read
		{
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (d.files[file].device == dos_device_con && !host_console(s))
			{
				// The emulated keyboard: a key at a time, echoed, and the call
				// repeated until one arrives.
				uint16_t key = 0;
				if (!bios_pop_key(s, key))
				{
					bios_retry_interrupt(s);
					return;
				}
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
				const int got = dos_read_handle(s, c.bx(), buffer, want);
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
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (c.cx() == 0)
			{
				// Writing nothing to a file sets its length to the position.
				if (d.files[file].device == dos_device_none && s.host != nullptr)
					s.host->file_truncate(d.files[file].host_handle);
				c.set_ax(0);
				succeed(s);
				return;
			}
			static uint8_t buffer[16384];
			uint16_t total = 0;
			while (total < c.cx())
			{
				const int want = int(c.cx() - total) > int(sizeof(buffer)) ? int(sizeof(buffer)) : int(c.cx() - total);
				for (int i = 0; i < want; ++i) buffer[i] = read8(s, c.ds(), uint16_t(c.dx() + total + i));
				const int put = dos_write_handle(s, c.bx(), buffer, want);
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
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path), true) || s.host == nullptr ||
			    !s.host->file_delete(path))
			{
				fail(s, error_file_not_found);
				return;
			}
			succeed(s);
			return;

		case 0x42: // seek
		{
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (d.files[file].device != dos_device_none)
			{
				c.set_ax(0);
				c.set_dx(0);
				succeed(s);
				return;
			}
			const int32_t offset = int32_t((uint32_t(c.cx()) << 16) | c.dx());
			const int32_t position = s.host != nullptr ? s.host->file_seek(d.files[file].host_handle, offset, c.al()) : -1;
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
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path), true) || s.host == nullptr)
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

		case 0x45: // duplicate handle: a second handle on the same entry
		{
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			const int handle = allocate_handle(s);
			if (handle < 0)
			{
				fail(s, error_too_many_files);
				return;
			}
			++d.files[file].references;
			return_new_handle(s, handle, file);
			return;
		}

		case 0x46: // force duplicate: CX becomes a second handle on BX's entry
		{
			const int file = dos_handle_file(s, c.bx());
			int size = 0;
			handle_table(s, d.psp_segment, size);
			if (file < 0 || c.cx() >= size)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (c.cx() != c.bx())
			{
				++d.files[file].references;
				dos_release_file(s, dos_point_handle(s, c.cx(), file));
			}
			succeed(s);
			return;
		}

		case 0x47: // get current directory, as DOS gives it: no drive and no leading backslash
		{
			const int drive = c.dl() == 0 ? d.current_drive : (c.dl() - 1);
			const char* cwd = d.cwd[drive < 3 && drive >= 0 ? drive : 2];
			while (*cwd == '\\') ++cwd;
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
			const int slot = open_search_slot(s);
			host_file_info info;
			const int search = s.host->find_first(pattern, uint8_t(c.cx() & 0xff), info);
			if (search < 0)
			{
				fail(s, error_file_not_found);
				return;
			}
			if (std::strpbrk(pattern, "*?") == nullptr)
			{
				s.host->find_close(search);
				store_find_result(s, info, 0xffff, 0);
			}
			else
			{
				d.searches[slot].used = true;
				d.searches[slot].host_search = search;
				d.searches[slot].stamp = ++d.search_stamp;
				store_find_result(s, info, slot, d.searches[slot].stamp);
			}
			succeed(s);
			return;
		}

		case 0x4f: // find next
		{
			const uint32_t dta = dta_address(s);
			const uint16_t slot = mem_read16(s, dta);
			const uint32_t stamp = mem_read16(s, dta + 2) | (uint32_t(mem_read16(s, dta + 4)) << 16);
			host_file_info info;
			if (s.host == nullptr || slot >= dos_max_searches || !d.searches[slot].used ||
			    d.searches[slot].stamp != stamp)
			{
				fail(s, error_no_more_files);
				return;
			}
			if (!s.host->find_next(d.searches[slot].host_search, info))
			{
				d.searches[slot] = dos_search(); // the host closes a search it has finished
				fail(s, error_no_more_files);
				return;
			}
			store_find_result(s, info, slot, stamp);
			succeed(s);
			return;
		}

		case 0x56: // rename
		{
			char to[192] = {};
			if (!read_dos_path(s, c.ds(), c.dx(), path, sizeof(path), true) ||
			    !read_dos_path(s, c.es(), c.di(), to, sizeof(to), true) ||
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
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return;
			}
			if (c.al() == 0)
			{
				host_file_info info;
				if (d.files[file].device == dos_device_none && s.host != nullptr &&
				    s.host->file_info(d.files[file].path, info))
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

int dos_handle_file(machine_state& s, uint16_t handle)
{
	int size = 0;
	const uint32_t table = handle_table(s, s.dos.psp_segment, size);
	if (handle >= size) return -1;
	const uint8_t file = mem_read8(s, table + handle);
	if (file >= dos_max_files) return -1;
	const dos_file& entry = s.dos.files[file];
	return entry.references > 0 || entry.permanent ? file : -1;
}

int dos_point_handle(machine_state& s, uint16_t handle, int file)
{
	int size = 0;
	const uint32_t table = handle_table(s, s.dos.psp_segment, size);
	if (handle >= size) return file;
	const uint8_t before = mem_read8(s, table + handle);
	mem_write8(s, table + handle, file < 0 ? uint8_t(0xff) : uint8_t(file));
	return before < dos_max_files ? before : -1;
}

void dos_release_file(machine_state& s, int file)
{
	if (file < 0 || file >= dos_max_files) return;
	dos_file& entry = s.dos.files[file];
	if (entry.references > 0) --entry.references;
	if (entry.references > 0 || entry.permanent) return;
	if (entry.device == dos_device_none && entry.host_handle >= 0 && s.host != nullptr)
		s.host->file_close(entry.host_handle);
	entry = dos_file();
}

int dos_open_file(machine_state& s, const char* path, int access, bool create, bool append, uint8_t attributes)
{
	const int file = allocate_file(s);
	if (file < 0) return -1;
	dos_file& entry = s.dos.files[file];
	entry = dos_file();
	const uint8_t device = device_named(path);
	if (device == dos_device_none)
	{
		if (s.host == nullptr) return -1;
		int handle = create ? s.host->file_create(path, attributes) : s.host->file_open(path, access);
		if (handle < 0 && append) handle = s.host->file_create(path, attributes);
		if (handle < 0) return -1;
		if (append) s.host->file_seek(handle, 0, 2);
		entry.host_handle = handle;
	}
	entry.device = device;
	entry.mode = uint8_t(access);
	entry.references = 1;
	snprintf(entry.path, sizeof(entry.path), "%s", path);
	return file;
}

int dos_read_handle(machine_state& s, uint16_t handle, uint8_t* data, int length)
{
	const int file = dos_handle_file(s, handle);
	if (file < 0) return -1;
	const dos_file& entry = s.dos.files[file];
	switch (entry.device)
	{
	case dos_device_none:
		return s.host != nullptr ? s.host->file_read(entry.host_handle, data, length) : -1;
	case dos_device_con:
	{
		const int got = s.host != nullptr ? s.host->console_read(data, length) : -1;
		return got < 0 ? 0 : got;
	}
	default:
		return 0; // NUL, AUX and PRN have nothing to read
	}
}

int dos_write_handle(machine_state& s, uint16_t handle, const uint8_t* data, int length)
{
	const int file = dos_handle_file(s, handle);
	if (file < 0) return -1;
	const dos_file& entry = s.dos.files[file];
	switch (entry.device)
	{
	case dos_device_none:
	{
		if (s.host == nullptr) return -1;
		int total = 0;
		while (total < length)
		{
			const int put = s.host->file_write(entry.host_handle, data + total, length - total);
			if (put < 0) return total > 0 ? total : -1;
			if (put == 0) break;
			total += put;
		}
		return total;
	}
	case dos_device_con:
		console_output(s, handle, data, length);
		return length;
	default:
		return length; // NUL, AUX and PRN take everything
	}
}

bool dos_start_child(machine_state& s, const char* path, const char* tail, uint16_t environment, int input,
                     int output, uint8_t& error)
{
	dos_state& d = s.dos;
	if (d.exec_depth >= dos_max_nesting)
	{
		error = error_out_of_memory;
		return false;
	}
	std::vector<uint8_t> image;
	if (!read_program_image(s, path, image))
	{
		error = error_file_not_found;
		return false;
	}

	// Remember where the parent has to carry on. The IRET frame the INT
	// pushed is unwound here, so the parent resumes after its own call. If
	// the child will not load, the CPU goes back to how the INT left it, so
	// the failure returns through that frame like any other.
	cpu_state& c = s.cpu;
	const cpu_state caller = c;
	dos_exec_frame& frame = d.exec_stack[d.exec_depth];
	frame.cpu = c;
	frame.psp = d.psp_segment;
	frame.dta = d.dta;
	const uint32_t stack = linear_address(c.ss(), c.sp());
	frame.cpu.ip = mem_read16(s, stack);
	frame.cpu.set_cs(mem_read16(s, stack + 2));
	frame.cpu.flags = uint16_t(mem_read16(s, stack + 4) & ~flag_cf);
	frame.cpu.set_sp(uint16_t(c.sp() + 6));

	const uint16_t parent = d.psp_segment;
	const uint16_t source = environment != 0 ? environment : read16(s, parent, 0x2c);
	char message[128] = {};
	if (!load_image(s, image.data(), uint32_t(image.size()), path, tail, source, parent, message, sizeof(message)))
	{
		s.cpu = caller;
		d.psp_segment = parent;
		d.dta = frame.dta;
		d.environment_segment = read16(s, parent, 0x2c);
		error = std::strstr(message, "memory") != nullptr ? error_out_of_memory : error_bad_format;
		return false;
	}

	write16(s, d.psp_segment, 0x16, parent);
	if (input >= 0) dos_release_file(s, dos_point_handle(s, 0, input));
	if (output >= 0) dos_release_file(s, dos_point_handle(s, 1, output));
	++d.exec_depth;
	return true;
}

bool dos_environment_value(machine_state& s, uint16_t environment, const char* name, char* out, int out_size)
{
	if (environment == 0) environment = read16(s, s.dos.psp_segment, 0x2c);
	if (environment == 0) return false;
	const size_t name_length = std::strlen(name);
	uint16_t offset = 0;
	while (offset < 0x7f00)
	{
		char entry[512] = {};
		const int length = read_string(s, environment, offset, entry, sizeof(entry));
		if (length == 0) break;
		bool match = size_t(length) > name_length && entry[name_length] == '=';
		for (size_t i = 0; match && i < name_length; ++i) match = upper(entry[i]) == upper(name[i]);
		if (match)
		{
			snprintf(out, size_t(out_size), "%s", entry + name_length + 1);
			return true;
		}
		offset = uint16_t(offset + length + 1);
	}
	return false;
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
		// AX=122Eh is how DOS 4's message retriever asks for the system's
		// message tables. There are none, so a request for one gets an empty
		// table and a program falls back on its own messages.
		if (c.ax() == 0x122e)
		{
			if ((c.dl() & 1) == 0)
			{
				c.set_es(callback_segment);
				c.set_di(0x0300);
			}
			return true;
		}
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
		const int length = read_string(s, c.ds(), c.dx(), text, sizeof(text), '$');
		dos_write_handle(s, 1, reinterpret_cast<const uint8_t*>(text), length);
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
		const bool typed = keyboard_input(s);
		if (!typed && ch == 10)
		{
			// The line feed after a carriage return in a redirected file.
			bios_retry_interrupt(s);
			return true;
		}
		// Redirected input also ends a line at the end of the file or when the
		// buffer is full, where the keyboard would wait or beep.
		if (ch == 13 || (!typed && (ch == 0x1a || length + 1 >= capacity)))
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
		if (!keyboard_input(s))
		{
			// A redirected file always has a character to read; a host's
			// terminal is never reported as having one, so a program polling
			// for Ctrl+C does not then block on it.
			const int file = dos_handle_file(s, 0);
			c.set_al(file >= 0 && d.files[file].device == dos_device_con ? 0x00 : 0xff);
			return true;
		}
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
		// Parse a filename into an FCB, as DOS does: AL bit 0 skips leading
		// separators, bits 1-3 keep the drive, name or extension the FCB
		// already holds unless the string gives one, and AL comes back 1 if
		// there were wildcards, FFh for a bad drive letter.
		const uint8_t flags = c.al();
		uint16_t source = c.si();
		const uint16_t fcb = c.di();
		uint8_t result = 0;
		const auto at = [&](int ahead) { return read8(s, c.ds(), uint16_t(source + ahead)); };
		const auto ends_name = [](uint8_t ch, bool all)
		{
			if (ch == 0 || ch == '.' || ch == ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == ';' || ch == ',' ||
			    ch == '=' || ch == '\t' || ch == '\r' || ch == '\n')
				return true;
			return all && (ch == '+' || ch == '<' || ch == '>' || ch == '|' || ch == '[' || ch == ']' || ch == '"');
		};
		const auto field = [&](int offset, int width)
		{
			for (int position = 0; position < width; ++position)
			{
				const uint8_t ch = at(0);
				if (ends_name(ch, true)) return;
				if (ch == '*')
				{
					for (; position < width; ++position) write8(s, c.es(), uint16_t(fcb + offset + position), '?');
					result = 1;
					++source;
					return;
				}
				if (ch == '?') result = 1;
				write8(s, c.es(), uint16_t(fcb + offset + position), uint8_t(upper(char(ch))));
				++source;
			}
		};

		if ((flags & 1) != 0)
			while (at(0) == ' ' || at(0) == '\t' || at(0) == ';' || at(0) == ',' || at(0) == '=') ++source;
		if ((flags & 2) == 0) write8(s, c.es(), fcb, 0);
		if ((flags & 4) == 0) for (int i = 1; i <= 8; ++i) write8(s, c.es(), uint16_t(fcb + i), ' ');
		if ((flags & 8) == 0) for (int i = 9; i <= 11; ++i) write8(s, c.es(), uint16_t(fcb + i), ' ');

		if (at(1) == ':')
		{
			const uint8_t drive = uint8_t(upper(char(at(0))));
			if (drive >= 'A' && drive <= 'Z')
			{
				write8(s, c.es(), fcb, uint8_t(drive - 'A' + 1));
				source = uint16_t(source + 2);
			}
			else
			{
				result = 0xff;
			}
		}
		field(1, 8);
		while (!ends_name(at(0), false)) ++source; // a name longer than eight
		if (at(0) == '.')
		{
			++source;
			field(9, 3);
		}
		c.set_al(result);
		c.set_si(source);
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
		c.set_al(d.config.version_major);
		c.set_ah(d.config.version_minor);
		c.set_bx(0);
		c.set_cx(0);
		return true;

	case 0x31:
	{
		// Stay resident: keep DX paragraphs of the program's block, its other
		// blocks and its open files.
		uint16_t largest = 0;
		dos_resize(s, d.psp_segment, c.dx() < 6 ? 6 : c.dx(), largest);
		terminate(s, c.al(), 3);
		return true;
	}

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
		case 0x00: // device information
		{
			const int file = dos_handle_file(s, c.bx());
			if (file < 0)
			{
				fail(s, error_invalid_handle);
				return true;
			}
			uint16_t info = 0x0002; // a file on drive C:
			switch (d.files[file].device)
			{
			case dos_device_con:
				// The console, unless the host's own stream behind a standard
				// handle is a file, which a C runtime treats differently.
				if (c.bx() > 2 || s.host == nullptr || s.host->console_is_device(c.bx())) info = 0x80d3;
				break;
			case dos_device_nul: info = 0x8084; break;
			case dos_device_aux: info = 0x80c0; break;
			case dos_device_prn: info = 0xa0c0; break;
			default: break;
			}
			c.set_dx(info);
			c.set_ax(info);
			succeed(s);
			return true;
		}
		case 0x01: // set device information
		case 0x0b: // sharing retry count
			succeed(s);
			return true;
		case 0x06: // input status
		case 0x07: // output status
			if (dos_handle_file(s, c.bx()) < 0)
			{
				fail(s, error_invalid_handle);
				return true;
			}
			c.set_al(0xff);
			succeed(s);
			return true;
		case 0x08: // removable media
			c.set_ax(1); // fixed disk
			succeed(s);
			return true;
		case 0x09: // drive is remote
		case 0x0a: // handle is remote
			c.set_dx(0);
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
		// On success the CPU is at the child's entry point, and the IRET frame
		// was unwound into the saved parent state; a failure, or a command
		// carried out without a child, returns through the frame as usual.
		exec(s);
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
		// Create a temporary file: append a name nothing else has to the path
		// the caller gave, and hand the whole path back.
		char raw[160] = {};
		read_string(s, c.ds(), c.dx(), raw, sizeof(raw));
		const int length = int(std::strlen(raw));
		const int handle = allocate_handle(s);
		if (handle < 0)
		{
			fail(s, error_too_many_files);
			return true;
		}
		for (unsigned attempt = 0; attempt < 100 && s.host != nullptr; ++attempt)
		{
			char path[192] = {};
			snprintf(raw + length, sizeof(raw) - size_t(length), "DOS%05u.TMP", unsigned((s.cycles + attempt) % 100000));
			dos_resolve_path(s, raw, path, sizeof(path));
			host_file_info info;
			if (s.host->file_info(path, info)) continue;
			const int file = dos_open_file(s, path, host_open_read_write, true, false, uint8_t(c.cx() & 0x27));
			if (file < 0) break;
			for (int i = 0; raw[i] != 0; ++i) write8(s, c.ds(), uint16_t(c.dx() + i), uint8_t(raw[i]));
			write8(s, c.ds(), uint16_t(c.dx() + std::strlen(raw)), 0);
			return_new_handle(s, handle, file);
			return true;
		}
		fail(s, error_access_denied);
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
	{
		// Set the handle count: a table bigger than the PSP's twenty moves to
		// a block of its own, which the process owns.
		int size = 0;
		const uint32_t table = handle_table(s, d.psp_segment, size);
		const int wanted = c.bx();
		if (wanted <= size)
		{
			succeed(s);
			return true;
		}
		if (wanted > 255)
		{
			fail(s, error_too_many_files);
			return true;
		}
		uint16_t largest = 0;
		const uint16_t block = dos_alloc(s, uint16_t((wanted + 15) / 16), largest);
		if (block == 0)
		{
			fail(s, error_out_of_memory);
			return true;
		}
		for (int i = 0; i < wanted; ++i)
			write8(s, block, uint16_t(i), i < size ? mem_read8(s, table + uint32_t(i)) : uint8_t(0xff));
		write16(s, d.psp_segment, 0x32, uint16_t(wanted));
		write16(s, d.psp_segment, 0x34, 0);
		write16(s, d.psp_segment, 0x36, block);
		succeed(s);
		return true;
	}

	case 0x68: case 0x6a:
		succeed(s);
		return true;

	default:
		fail(s, error_invalid_function);
		return true;
	}
}
