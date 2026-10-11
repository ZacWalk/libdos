// ovrwin by Zac Walker
//
// The built-in command interpreter. It writes to the emulated text screen and
// reads the emulated keyboard buffer, so it looks and behaves like a program
// running on the machine, and it reaches the drive through the same host
// services INT 21h uses - what DIR lists is what a program will find.

#include "machine.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	constexpr uint8_t shell_attribute = 0x07;

	void write_text(machine_state& s, const char* text)
	{
		for (int i = 0; text[i] != 0; ++i)
		{
			if (text[i] == '\n') vga_write_char(s, 0, '\r', shell_attribute, true);
			vga_write_char(s, 0, uint8_t(text[i]), shell_attribute, true);
		}
	}

	void write_line(machine_state& s, const char* text)
	{
		write_text(s, text);
		write_text(s, "\n");
	}

	void write_prompt(machine_state& s)
	{
		char prompt[80] = {};
		const char* cwd = s.dos.cwd[s.dos.current_drive < 3 ? s.dos.current_drive : 2];
		snprintf(prompt, sizeof(prompt), "%c:%s>", char('A' + s.dos.current_drive), cwd[0] != 0 ? cwd : "\\");
		write_text(s, prompt);
	}

	char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

	bool equals(const char* a, const char* b)
	{
		for (int i = 0;; ++i)
		{
			if (upper(a[i]) != upper(b[i])) return false;
			if (a[i] == 0) return true;
		}
	}

	// Splits the line into a command and the rest, both trimmed.
	void split_command(const char* line, char* command, int command_size, const char** argument)
	{
		while (*line == ' ' || *line == '\t') ++line;
		int length = 0;
		while (*line != 0 && *line != ' ' && *line != '\t' && length < command_size - 1)
			command[length++] = *line++;
		command[length] = 0;
		while (*line == ' ' || *line == '\t') ++line;
		*argument = line;
	}

	void format_size(uint32_t value, char* out, int out_size)
	{
		// Thousands separators, because a bare byte count of a game's data
		// file is unreadable.
		char digits[16] = {};
		snprintf(digits, sizeof(digits), "%u", value);
		const int length = int(std::strlen(digits));
		int written = 0;
		for (int i = 0; i < length && written < out_size - 1; ++i)
		{
			if (i > 0 && (length - i) % 3 == 0) out[written++] = ',';
			out[written++] = digits[i];
		}
		out[written] = 0;
	}

	void command_dir(machine_state& s, const char* argument)
	{
		if (s.host == nullptr) return;

		char pattern[192] = {};
		if (argument[0] == 0) dos_resolve_path(s, "*.*", pattern, sizeof(pattern));
		else
		{
			dos_resolve_path(s, argument, pattern, sizeof(pattern));
			if (s.host->directory_exists(pattern))
			{
				const int length = int(std::strlen(pattern));
				snprintf(pattern + length, sizeof(pattern) - size_t(length), "%s*.*",
				         pattern[length - 1] == '\\' ? "" : "\\");
			}
		}

		char header[96] = {};
		snprintf(header, sizeof(header), " Directory of %s", pattern);
		write_line(s, "");
		write_line(s, header);
		write_line(s, "");

		host_file_info info;
		int search = s.host->find_first(pattern, 0x10, info);
		if (search < 0)
		{
			write_line(s, "File not found");
			write_line(s, "");
			return;
		}

		int files = 0;
		int directories = 0;
		uint32_t total = 0;
		do
		{
			char name[16] = {};
			char extension[8] = {};
			// "." and ".." are names, not an empty name with an extension.
			const char* dot = std::strchr(info.name, '.');
			if (dot != nullptr && dot != info.name)
			{
				snprintf(name, sizeof(name), "%.*s", int(dot - info.name), info.name);
				snprintf(extension, sizeof(extension), "%s", dot + 1);
			}
			else
			{
				snprintf(name, sizeof(name), "%s", info.name);
			}

			char size[20] = {};
			if (info.directory)
			{
				snprintf(size, sizeof(size), "%s", "<DIR>");
				++directories;
			}
			else
			{
				format_size(info.size, size, sizeof(size));
				++files;
				total += info.size;
			}

			char row[80] = {};
			snprintf(row, sizeof(row), "%-8s %-3s %13s   %02u-%02u-%04u  %2u:%02u",
			         name, extension, size,
			         unsigned((info.dos_date >> 5) & 0x0f), unsigned(info.dos_date & 0x1f),
			         unsigned(1980 + (info.dos_date >> 9)),
			         unsigned((info.dos_time >> 11) & 0x1f), unsigned((info.dos_time >> 5) & 0x3f));
			write_line(s, row);
		}
		while (s.host->find_next(search, info));
		s.host->find_close(search);

		char bytes[24] = {};
		format_size(total, bytes, sizeof(bytes));
		char summary[96] = {};
		snprintf(summary, sizeof(summary), "%9d file(s) %14s bytes", files, bytes);
		write_line(s, summary);
		char free_text[24] = {};
		format_size(s.host->free_space(), free_text, sizeof(free_text));
		snprintf(summary, sizeof(summary), "%9d dir(s)  %14s bytes free", directories, free_text);
		write_line(s, summary);
		write_line(s, "");
	}

	void command_cd(machine_state& s, const char* argument)
	{
		if (argument[0] == 0)
		{
			char here[96] = {};
			const char* cwd = s.dos.cwd[s.dos.current_drive < 3 ? s.dos.current_drive : 2];
			snprintf(here, sizeof(here), "%c:%s", char('A' + s.dos.current_drive), cwd[0] != 0 ? cwd : "\\");
			write_line(s, here);
			return;
		}

		char path[192] = {};
		dos_resolve_path(s, argument, path, sizeof(path));
		if (s.host == nullptr || !s.host->directory_exists(path))
		{
			write_line(s, "Invalid directory");
			return;
		}
		const int drive = (path[0] - 'A') < 3 ? (path[0] - 'A') : 2;
		snprintf(s.dos.cwd[drive], sizeof(s.dos.cwd[drive]), "%s", path + 2);
	}

	void command_type(machine_state& s, const char* argument)
	{
		if (argument[0] == 0 || s.host == nullptr)
		{
			write_line(s, "Required parameter missing");
			return;
		}
		char path[192] = {};
		dos_resolve_path(s, argument, path, sizeof(path));
		const int handle = s.host->file_open(path, host_open_read);
		if (handle < 0)
		{
			write_line(s, "File not found");
			return;
		}
		uint8_t buffer[512];
		for (;;)
		{
			const int read = s.host->file_read(handle, buffer, int(sizeof(buffer)));
			if (read <= 0) break;
			for (int i = 0; i < read; ++i)
			{
				if (buffer[i] == 26) break; // end of file marker
				vga_write_char(s, 0, buffer[i], shell_attribute, true);
			}
		}
		s.host->file_close(handle);
		write_text(s, "\n");
	}

	void command_del(machine_state& s, const char* argument)
	{
		char path[192] = {};
		dos_resolve_path(s, argument, path, sizeof(path));
		if (s.host == nullptr || !s.host->file_delete(path)) write_line(s, "File not found");
	}

	void command_mem(machine_state& s)
	{
		uint16_t largest = 0;
		uint16_t free_paragraphs = 0;
		uint16_t mcb = s.dos.first_mcb;
		for (int guard = 0; guard < 256; ++guard)
		{
			const uint8_t type = read8(s, mcb, 0);
			const uint16_t owner = read16(s, mcb, 1);
			const uint16_t size = read16(s, mcb, 3);
			if (owner == 0)
			{
				free_paragraphs = uint16_t(free_paragraphs + size);
				if (size > largest) largest = size;
			}
			if (type != 'M') break;
			mcb = uint16_t(mcb + size + 1);
		}
		char text[96] = {};
		snprintf(text, sizeof(text), "%6u bytes total conventional memory", 640u * 1024);
		write_line(s, text);
		snprintf(text, sizeof(text), "%6u bytes free, largest block %u bytes",
		         unsigned(free_paragraphs) * 16, unsigned(largest) * 16);
		write_line(s, text);
	}

	void command_help(machine_state& s)
	{
		write_line(s, "");
		write_line(s, "  DIR [path]     list a directory        CLS    clear the screen");
		write_line(s, "  CD [path]      change directory        VER    version");
		write_line(s, "  MD / RD path   make/remove directory   MEM    free memory");
		write_line(s, "  DEL file       delete a file           EXIT   close ovrwin");
		write_line(s, "  TYPE file      show a file");
		write_line(s, "");
		write_line(s, "  Type the name of an .EXE or .COM file to run it.");
		write_line(s, "");
	}

	// Reads a whole file through the host. Returns false if it cannot be read.
	bool read_program(machine_state& s, const char* path, std::vector<uint8_t>& image)
	{
		if (s.host == nullptr) return false;
		host_file_info info;
		if (!s.host->file_info(path, info) || info.directory || info.size == 0) return false;

		const int handle = s.host->file_open(path, host_open_read);
		if (handle < 0) return false;

		image.resize(info.size);
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

	// Finds what the user meant: an explicit extension, or .COM then .EXE.
	bool resolve_program(machine_state& s, const char* name, char* out, int out_size)
	{
		if (s.host == nullptr) return false;

		char candidate[192] = {};
		host_file_info info;
		const char* dot = std::strrchr(name, '.');
		if (dot != nullptr && (equals(dot, ".exe") || equals(dot, ".com")))
		{
			dos_resolve_path(s, name, candidate, sizeof(candidate));
			if (!s.host->file_info(candidate, info) || info.directory) return false;
			snprintf(out, size_t(out_size), "%s", candidate);
			return true;
		}

		static const char* const extensions[] = {".COM", ".EXE"};
		for (const char* extension : extensions)
		{
			char with_extension[160] = {};
			snprintf(with_extension, sizeof(with_extension), "%s%s", name, extension);
			dos_resolve_path(s, with_extension, candidate, sizeof(candidate));
			if (s.host->file_info(candidate, info) && !info.directory)
			{
				snprintf(out, size_t(out_size), "%s", candidate);
				return true;
			}
		}
		return false;
	}

	void execute_line(machine_state& s, const char* line)
	{
		char command[64] = {};
		const char* argument = nullptr;
		split_command(line, command, sizeof(command), &argument);
		if (command[0] == 0) return;

		if (equals(command, "dir")) command_dir(s, argument);
		else if (equals(command, "cd") || equals(command, "chdir")) command_cd(s, argument);
		else if (equals(command, "type")) command_type(s, argument);
		else if (equals(command, "del") || equals(command, "erase")) command_del(s, argument);
		else if (equals(command, "mem")) command_mem(s);
		else if (equals(command, "help") || equals(command, "?")) command_help(s);
		else if (equals(command, "cls"))
		{
			vga_set_mode(s, 3);
		}
		else if (equals(command, "ver"))
		{
			write_line(s, "ovrwin - DOS 5.00 compatible");
		}
		else if (equals(command, "md") || equals(command, "mkdir"))
		{
			char path[192] = {};
			dos_resolve_path(s, argument, path, sizeof(path));
			if (s.host == nullptr || !s.host->make_directory(path)) write_line(s, "Unable to create directory");
		}
		else if (equals(command, "rd") || equals(command, "rmdir"))
		{
			char path[192] = {};
			dos_resolve_path(s, argument, path, sizeof(path));
			if (s.host == nullptr || !s.host->remove_directory(path)) write_line(s, "Unable to remove directory");
		}
		else if (equals(command, "exit"))
		{
			s.shell.active = false;
			s.exited = true;
		}
		else if (!shell_run(s, command, argument))
		{
			write_line(s, "Bad command or file name");
		}
	}
}

void shell_start(machine_state& s)
{
	s.shell.active = true;
	s.shell.program_running = false;
	s.shell.length = 0;
	s.shell.line[0] = 0;
	s.exited = false;

	vga_set_mode(s, 3);
	write_line(s, "ovrwin");
	write_line(s, "");
	write_line(s, "Type HELP for a list of commands, or the name of a program to run it.");
	write_line(s, "You can also drop a program or a folder onto the window.");
	write_line(s, "");
	write_prompt(s);
}

void shell_tick(machine_state& s)
{
	shell_state& shell = s.shell;
	if (!shell.active || shell.program_running) return;

	uint16_t key = 0;
	while (bios_pop_key(s, key))
	{
		const uint8_t ch = uint8_t(key & 0xff);
		if (ch == 13)
		{
			write_text(s, "\n");
			shell.line[shell.length] = 0;
			const char* line = shell.line;
			shell.length = 0;
			execute_line(s, line);
			if (shell.active && !shell.program_running) write_prompt(s);
			shell.line[0] = 0;
		}
		else if (ch == 8)
		{
			if (shell.length > 0)
			{
				--shell.length;
				write_text(s, "\b \b");
			}
		}
		else if (ch >= 32 && ch < 127 && shell.length < int(sizeof(shell.line)) - 1)
		{
			shell.line[shell.length++] = char(ch);
			vga_write_char(s, 0, ch, shell_attribute, true);
		}
	}
}

bool shell_run(machine_state& s, const char* name, const char* command_line)
{
	char path[192] = {};
	if (!resolve_program(s, name, path, sizeof(path))) return false;

	std::vector<uint8_t> image;
	if (!read_program(s, path, image)) return false;

	// Typed at the prompt, the command line is still in the shell's line
	// buffer, which the reset below clears.
	char tail[128] = {};
	snprintf(tail, sizeof(tail), "%s", command_line);

	// The program gets a machine of its own; only the current directory and
	// the shell's own state survive.
	const bool shell_was_active = s.shell.active;
	machine_reset(s);
	s.shell.active = shell_was_active;

	char error[128] = {};
	if (!dos_load_program(s, image.data(), uint32_t(image.size()), path, tail, error, sizeof(error)))
	{
		// The reset has cleared the screen; say why, and leave the prompt to
		// whoever asked, as for any other command.
		write_line(s, error);
		return true;
	}

	const char* leaf = std::strrchr(path, '\\');
	snprintf(s.shell.program, sizeof(s.shell.program), "%s", leaf != nullptr ? leaf + 1 : path);
	s.shell.program_running = true;
	s.exited = false;
	return true;
}

void shell_program_finished(machine_state& s)
{
	s.shell.program_running = false;
	s.shell.program[0] = 0;
	s.exited = false;
	s.shell.length = 0;

	// A program leaves the adapter in whatever mode it was using, and the
	// shell can only write to 80-column text on page 0. Anything else goes
	// back to mode 3; a text program's output stays where it can be read.
	vga_state& v = s.vga;
	if ((v.mode != 2 && v.mode != 3) || v.page != 0 || v.display_start != 0)
	{
		vga_set_mode(s, 3);
	}
	else
	{
		v.cursor_visible = true;
		v.cursor_start = 6;
		v.cursor_end = 7;
		if (v.cursor_x[0] != 0) write_text(s, "\n");
	}

	char text[80] = {};
	snprintf(text, sizeof(text), "Program ended with exit code %u.", unsigned(s.exit_code));
	write_line(s, text);
	write_line(s, "");
	write_prompt(s);
}

void shell_prompt(machine_state& s)
{
	write_prompt(s);
}
