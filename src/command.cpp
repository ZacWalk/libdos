// libdos by Zac Walker
//
// The commands COMMAND.COM has built in, and COMMAND.COM /C itself. There is
// no COMMAND.COM in memory: a program that starts one to carry out a command
// line - NMAKE does, for every line that is not a plain program - has the line
// carried out here, and a program the line names is started as the caller's
// own child. File work goes through the same host services INT 21h uses.

#include "machine.h"
#include "command.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

	bool equals(const char* a, const char* b)
	{
		for (int i = 0;; ++i)
		{
			if (upper(a[i]) != upper(b[i])) return false;
			if (a[i] == 0) return true;
		}
	}

	bool is_blank(char c) { return c == ' ' || c == '\t'; }

	const char* skip_blanks(const char* p)
	{
		while (is_blank(*p)) ++p;
		return p;
	}

	// The word at the start of `text`, up to a blank, and whether it was there.
	bool starts_with_word(const char* text, const char* word)
	{
		int i = 0;
		for (; word[i] != 0; ++i)
			if (upper(text[i]) != upper(word[i])) return false;
		return text[i] == 0 || is_blank(text[i]);
	}

	std::string trimmed(const char* text)
	{
		std::string out(skip_blanks(text));
		while (!out.empty() && static_cast<unsigned char>(out.back()) <= ' ') out.pop_back();
		return out;
	}

	// An argument list split at blanks, with quoted text kept together.
	std::vector<std::string> words(const char* text)
	{
		std::vector<std::string> out;
		const char* p = text;
		for (;;)
		{
			p = skip_blanks(p);
			if (*p == 0) break;
			std::string word;
			bool quoted = false;
			while (*p != 0 && (quoted || !is_blank(*p)))
			{
				if (*p == '"') quoted = !quoted;
				word += *p++;
			}
			out.push_back(word);
		}
		return out;
	}

	const char* const builtin_names[] = {
		"attrib", "break", "cd", "chdir", "cls", "copy", "ctty", "date", "del", "dir", "echo", "erase", "exit",
		"goto", "md", "mkdir", "path", "pause", "rd", "rem", "ren", "rename", "rmdir", "set", "shift", "time",
		"type", "ver", "verify", "vol",
	};

	bool is_builtin(const char* name)
	{
		for (const char* builtin : builtin_names)
			if (equals(name, builtin)) return true;
		return false;
	}

	struct printer
	{
		machine_state& s;
		const command_output& out;

		void raw(const char* text, int length, bool error = false) const
		{
			out.write(s, text, length, error, out.context);
		}

		void text(const char* text) const { raw(text, int(std::strlen(text))); }

		void line(const char* text = "") const
		{
			raw(text, int(std::strlen(text)));
			raw("\r\n", 2);
		}

		void error(const char* text) const
		{
			raw(text, int(std::strlen(text)), true);
			raw("\r\n", 2, true);
		}
	};

	bool has_wildcards(const char* path) { return std::strpbrk(path, "*?") != nullptr; }

	const char* name_of(const char* path)
	{
		const char* name = path;
		for (const char* p = path; *p != 0; ++p)
			if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
		return name;
	}

	// The directory part of a resolved path, with its trailing backslash.
	std::string directory_of(const char* path) { return std::string(path, size_t(name_of(path) - path)); }

	std::string resolve(machine_state& s, const std::string& path)
	{
		char out[192] = {};
		dos_resolve_path(s, path.c_str(), out, sizeof(out));
		return out;
	}

	bool is_directory(machine_state& s, const std::string& path)
	{
		return s.host != nullptr && s.host->directory_exists(path.c_str());
	}

	bool is_file(machine_state& s, const std::string& path)
	{
		host_file_info info;
		return s.host != nullptr && s.host->file_info(path.c_str(), info) && !info.directory;
	}

	// Every file a pattern matches, as full DOS paths.
	std::vector<std::string> matches(machine_state& s, const std::string& pattern)
	{
		std::vector<std::string> out;
		if (s.host == nullptr) return out;
		host_file_info info;
		const int search = s.host->find_first(pattern.c_str(), 0x06, info);
		if (search < 0) return out;
		const std::string directory = directory_of(pattern.c_str());
		do
		{
			if (!info.directory) out.push_back(directory + info.name);
		}
		while (s.host->find_next(search, info));
		s.host->find_close(search);
		return out;
	}

	std::string current_directory(machine_state& s, int drive)
	{
		const char* cwd = s.dos.cwd[drive >= 0 && drive < 3 ? drive : 2];
		char text[96] = {};
		snprintf(text, sizeof(text), "%c:%s", char('A' + drive), cwd[0] != 0 ? cwd : "\\");
		return text;
	}

	// Appends `from` to an open file. In text mode a source ends at its first
	// Ctrl+Z, as COPY /A reads it.
	bool append_file(machine_state& s, const std::string& from, int to, bool text)
	{
		const int handle = s.host->file_open(from.c_str(), host_open_read);
		if (handle < 0) return false;
		uint8_t buffer[32768];
		bool ok = true;
		for (;;)
		{
			int got = s.host->file_read(handle, buffer, int(sizeof(buffer)));
			if (got <= 0) break;
			bool end = false;
			if (text)
			{
				const void* marker = std::memchr(buffer, 0x1a, size_t(got));
				if (marker != nullptr)
				{
					got = int(static_cast<const uint8_t*>(marker) - buffer);
					end = true;
				}
			}
			if (got > 0 && s.host->file_write(to, buffer, got) != got)
			{
				ok = false;
				break;
			}
			if (end) break;
		}
		s.host->file_close(handle);
		return ok;
	}

	// COPY. A single file is copied as it is, keeping its date; several - joined
	// with + or matched by a wildcard into one file - are joined, as text unless
	// /B says otherwise, which is DOS's default for a join.
	int command_copy(machine_state& s, const char* argument, const printer& p)
	{
		if (s.host == nullptr) return 1;
		bool binary = false;
		bool ascii = false;
		std::string joined;
		for (const std::string& word : words(argument))
		{
			std::string name;
			for (size_t i = 0; i < word.size(); ++i)
			{
				if (word[i] == '/' && i + 1 < word.size())
				{
					const char option = upper(word[++i]);
					if (option == 'B') binary = true;
					if (option == 'A') ascii = true;
					continue;
				}
				if (word[i] != '"') name += word[i];
			}
			if (name.empty()) continue;
			if (!joined.empty() && joined.back() != '+' && name[0] != '+') joined += ' ';
			joined += name;
		}

		const std::vector<std::string> parts = words(joined.c_str());
		if (parts.empty() || parts.size() > 2)
		{
			p.error(parts.empty() ? "Required parameter missing" : "Too many parameters");
			return 1;
		}

		std::vector<std::string> sources;
		size_t start = 0;
		const std::string& list = parts[0];
		for (size_t plus = list.find('+'); start <= list.size(); plus = list.find('+', start))
		{
			const std::string piece = list.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
			if (!piece.empty())
			{
				const std::string path = resolve(s, piece);
				if (has_wildcards(path.c_str()))
					for (const std::string& match : matches(s, path)) sources.push_back(match);
				else sources.push_back(path);
			}
			if (plus == std::string::npos) break;
			start = plus + 1;
		}
		if (sources.empty())
		{
			p.error("File not found");
			return 1;
		}

		std::string destination = parts.size() == 2 ? resolve(s, parts[1]) : current_directory(s, s.dos.current_drive);
		const bool into_directory = is_directory(s, destination);
		const bool join = list.find('+') != std::string::npos || (!into_directory && sources.size() > 1);

		int copied = 0;
		if (join)
		{
			if (into_directory) destination += (destination.back() == '\\' ? "" : "\\") + std::string(name_of(sources[0].c_str()));
			const int to = s.host->file_create(destination.c_str(), 0);
			if (to < 0)
			{
				p.error("File creation error");
				return 1;
			}
			const bool text = ascii || !binary;
			for (const std::string& source : sources)
			{
				if (!append_file(s, source, to, text))
				{
					s.host->file_close(to);
					p.error("File not found");
					return 1;
				}
			}
			if (text)
			{
				const uint8_t end = 0x1a;
				s.host->file_write(to, &end, 1);
			}
			s.host->file_close(to);
			copied = 1;
		}
		else
		{
			for (const std::string& source : sources)
			{
				std::string target = destination;
				if (into_directory) target += (target.back() == '\\' ? "" : "\\") + std::string(name_of(source.c_str()));
				if (equals(target.c_str(), source.c_str()))
				{
					p.error("File cannot be copied onto itself");
					return 1;
				}
				if (!s.host->file_copy(source.c_str(), target.c_str()))
				{
					p.error(is_file(s, source) ? "File creation error" : "File not found");
					return 1;
				}
				++copied;
			}
		}
		char summary[48] = {};
		snprintf(summary, sizeof(summary), "%9d File(s) copied", copied);
		p.line(summary);
		return 0;
	}

	// DEL and ERASE. A file that is not there is reported, but not as a
	// failure, since COMMAND.COM's own commands leave the errorlevel alone.
	int command_del(machine_state& s, const char* argument, const printer& p)
	{
		std::string target;
		for (const std::string& word : words(argument))
			if (word[0] != '/' && target.empty()) target = word;
		if (target.empty())
		{
			p.error("Required parameter missing");
			return 1;
		}
		std::string path = resolve(s, target);
		if (is_directory(s, path)) path += (path.back() == '\\' ? "*.*" : "\\*.*");
		if (has_wildcards(path.c_str()))
		{
			for (const std::string& match : matches(s, path)) s.host->file_delete(match.c_str());
			return 0;
		}
		if (s.host == nullptr || !s.host->file_delete(path.c_str())) p.error("File not found");
		return 0;
	}

	int command_ren(machine_state& s, const char* argument, const printer& p)
	{
		const std::vector<std::string> names = words(argument);
		if (names.size() != 2)
		{
			p.error(names.size() < 2 ? "Required parameter missing" : "Too many parameters");
			return 1;
		}
		const std::string from = resolve(s, names[0]);
		// The new name is a name only: the file stays in its directory.
		std::string to = directory_of(from.c_str());
		for (const char ch : std::string(name_of(names[1].c_str()))) to += upper(ch);
		if (s.host == nullptr || has_wildcards(from.c_str()) || !s.host->file_rename(from.c_str(), to.c_str()))
		{
			p.error("Duplicate file name or File not found");
			return 1;
		}
		return 0;
	}

	int command_type(machine_state& s, const char* argument, const printer& p)
	{
		const std::string name = trimmed(argument);
		if (name.empty())
		{
			p.error("Required parameter missing");
			return 1;
		}
		const std::string path = resolve(s, name);
		const int handle = s.host != nullptr ? s.host->file_open(path.c_str(), host_open_read) : -1;
		if (handle < 0)
		{
			p.error("File not found");
			return 1;
		}
		char buffer[4096];
		for (;;)
		{
			int got = s.host->file_read(handle, reinterpret_cast<uint8_t*>(buffer), int(sizeof(buffer)));
			if (got <= 0) break;
			const void* end = std::memchr(buffer, 0x1a, size_t(got));
			if (end != nullptr) got = int(static_cast<const char*>(end) - buffer);
			p.raw(buffer, got);
			if (end != nullptr) break;
		}
		s.host->file_close(handle);
		return 0;
	}

	int command_echo(const char* argument, const printer& p)
	{
		const std::string text = trimmed(argument);
		if (text.empty())
		{
			p.line("ECHO is on");
			return 0;
		}
		if (equals(text.c_str(), "on") || equals(text.c_str(), "off")) return 0;
		// "echo." and its relatives print what follows the delimiter.
		if (std::strchr(".:;,/+[]", text[0]) != nullptr && argument[0] == text[0])
		{
			p.line(text.c_str() + 1);
			return 0;
		}
		p.line(text.c_str());
		return 0;
	}

	int command_cd(machine_state& s, const char* argument, const printer& p)
	{
		const std::string target = trimmed(argument);
		if (target.empty())
		{
			p.line(current_directory(s, s.dos.current_drive).c_str());
			return 0;
		}
		if (target.size() == 2 && target[1] == ':')
		{
			p.line(current_directory(s, upper(target[0]) - 'A').c_str());
			return 0;
		}
		const std::string path = resolve(s, target);
		if (!is_directory(s, path))
		{
			p.error("Invalid directory");
			return 1;
		}
		const int drive = path[0] - 'A';
		snprintf(s.dos.cwd[drive >= 0 && drive < 3 ? drive : 2], sizeof(s.dos.cwd[0]), "%s", path.size() > 3 ? path.c_str() + 2 : "");
		return 0;
	}

	int command_md(machine_state& s, const char* argument, const printer& p)
	{
		const std::string path = resolve(s, trimmed(argument));
		if (trimmed(argument).empty() || s.host == nullptr)
		{
			p.error("Required parameter missing");
			return 1;
		}
		if (is_directory(s, path)) return 0; // already there is as good as made
		if (!s.host->make_directory(path.c_str()))
		{
			p.error("Unable to create directory");
			return 1;
		}
		return 0;
	}

	int command_rd(machine_state& s, const char* argument, const printer& p)
	{
		const std::string path = resolve(s, trimmed(argument));
		if (trimmed(argument).empty() || s.host == nullptr || !s.host->remove_directory(path.c_str()))
		{
			p.error("Invalid path, not directory,");
			p.error("or directory not empty");
			return 1;
		}
		return 0;
	}

	// SET and PATH show the environment. Changing it would change only the
	// copy COMMAND.COM was given, which is gone when the command line ends, so
	// an assignment is accepted and has no effect - as with COMMAND.COM /C.
	int command_set(machine_state& s, const char* argument, const printer& p)
	{
		if (!trimmed(argument).empty()) return 0;
		const uint16_t environment = read16(s, s.dos.psp_segment, 0x2c);
		uint16_t offset = 0;
		while (environment != 0 && offset < 0x7f00)
		{
			char entry[512] = {};
			const int length = read_string(s, environment, offset, entry, sizeof(entry));
			if (length == 0) break;
			p.line(entry);
			offset = uint16_t(offset + length + 1);
		}
		return 0;
	}

	int command_path(machine_state& s, const char* argument, const printer& p)
	{
		if (!trimmed(argument).empty()) return 0;
		char value[512] = {};
		if (!dos_environment_value(s, 0, "PATH", value, sizeof(value)) || value[0] == 0)
		{
			p.line("No Path");
			return 0;
		}
		p.text("PATH=");
		p.line(value);
		return 0;
	}

	int command_attrib(machine_state& s, const char* argument, const printer& p)
	{
		uint8_t set = 0;
		uint8_t clear = 0;
		std::string target;
		for (const std::string& word : words(argument))
		{
			if ((word[0] == '+' || word[0] == '-') && word.size() == 2)
			{
				uint8_t bit = 0;
				switch (upper(word[1]))
				{
				case 'R': bit = 0x01; break;
				case 'H': bit = 0x02; break;
				case 'S': bit = 0x04; break;
				case 'A': bit = 0x20; break;
				default: break;
				}
				(word[0] == '+' ? set : clear) |= bit;
			}
			else if (word[0] != '/' && target.empty())
			{
				target = word;
			}
		}
		if (s.host == nullptr) return 1;

		const std::string pattern = resolve(s, target.empty() ? "*.*" : target);
		std::vector<std::string> files;
		if (has_wildcards(pattern.c_str())) files = matches(s, pattern);
		else if (is_file(s, pattern)) files.push_back(pattern);
		if (files.empty())
		{
			p.error("File not found");
			return 1;
		}

		for (const std::string& file : files)
		{
			host_file_info info;
			if (!s.host->file_info(file.c_str(), info)) continue;
			if (set == 0 && clear == 0)
			{
				char row[160] = {};
				snprintf(row, sizeof(row), "%c  %c%c%c     %s", (info.attributes & 0x20) ? 'A' : ' ',
				         (info.attributes & 0x04) ? 'S' : ' ', (info.attributes & 0x02) ? 'H' : ' ',
				         (info.attributes & 0x01) ? 'R' : ' ', file.c_str());
				p.line(row);
				continue;
			}
			const uint8_t attributes = uint8_t(((info.attributes & 0x27) & ~clear) | set);
			if (!s.host->file_set_attributes(file.c_str(), attributes))
			{
				p.error("Access denied");
				return 1;
			}
		}
		return 0;
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

	int command_dir(machine_state& s, const char* argument, const printer& p)
	{
		if (s.host == nullptr) return 1;

		std::string target;
		for (const std::string& word : words(argument))
			if (word[0] != '/' && target.empty()) target = word;
		std::string pattern = resolve(s, target.empty() ? "*.*" : target);
		if (is_directory(s, pattern)) pattern += (pattern.back() == '\\' ? "*.*" : "\\*.*");

		p.line();
		p.line((" Directory of " + pattern).c_str());
		p.line();

		host_file_info info;
		const int search = s.host->find_first(pattern.c_str(), 0x10, info);
		if (search < 0)
		{
			p.line("File not found");
			p.line();
			return 0;
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
			snprintf(row, sizeof(row), "%-8s %-3s %13s   %02u-%02u-%04u  %2u:%02u", name, extension, size,
			         unsigned((info.dos_date >> 5) & 0x0f), unsigned(info.dos_date & 0x1f),
			         unsigned(1980 + (info.dos_date >> 9)), unsigned((info.dos_time >> 11) & 0x1f),
			         unsigned((info.dos_time >> 5) & 0x3f));
			p.line(row);
		}
		while (s.host->find_next(search, info));
		s.host->find_close(search);

		char bytes[24] = {};
		format_size(total, bytes, sizeof(bytes));
		char summary[96] = {};
		snprintf(summary, sizeof(summary), "%9d file(s) %14s bytes", files, bytes);
		p.line(summary);
		char free_text[24] = {};
		format_size(s.host->free_space(), free_text, sizeof(free_text));
		snprintf(summary, sizeof(summary), "%9d dir(s)  %14s bytes free", directories, free_text);
		p.line(summary);
		p.line();
		return 0;
	}

	// BREAK and VERIFY: show the setting, or turn it on or off.
	int command_switch(const char* name, const char* argument, bool& setting, const printer& p)
	{
		const std::string value = trimmed(argument);
		if (value.empty())
		{
			char text[32] = {};
			snprintf(text, sizeof(text), "%s is %s", name, setting ? "on" : "off");
			p.line(text);
			return 0;
		}
		if (equals(value.c_str(), "on")) setting = true;
		else if (equals(value.c_str(), "off")) setting = false;
		else
		{
			p.error("Must specify ON or OFF");
			return 1;
		}
		return 0;
	}

	void clock_now(machine_state& s, int& year, int& month, int& day, int& weekday, int& hour, int& minute,
	               int& second, int& hundredths)
	{
		year = 1990;
		month = day = 1;
		weekday = 1;
		hour = minute = second = hundredths = 0;
		if (s.host != nullptr) s.host->clock(year, month, day, weekday, hour, minute, second, hundredths);
	}

	// Standard output, for a command line COMMAND.COM /C carries out.
	void handle_output(machine_state& s, const char* text, int length, bool error, void*)
	{
		dos_write_handle(s, uint16_t(error ? 2 : 1), reinterpret_cast<const uint8_t*>(text), length);
	}

	void handle_clear(machine_state& s, void*)
	{
		uint8_t probe = 0;
		const int file = dos_handle_file(s, 1);
		const bool screen = file >= 0 && s.dos.files[file].device == dos_device_con &&
			(s.host == nullptr || s.host->console_read(&probe, 0) < 0);
		if (screen) vga_set_mode(s, 3);
	}

	void set_return_code(machine_state& s, int code)
	{
		s.dos.return_code = uint8_t(code);
		s.dos.return_type = 0;
	}

	// Finds the program a command names, as COMMAND.COM does: as given if it
	// has a path, otherwise in the current directory and then along PATH, and
	// with .COM, .EXE and then .BAT tried when it has no extension.
	bool find_program(machine_state& s, const char* command, uint16_t environment, std::string& found)
	{
		if (s.host == nullptr) return false;
		const char* dot = std::strrchr(name_of(command), '.');
		if (dot != nullptr && !equals(dot, ".com") && !equals(dot, ".exe") && !equals(dot, ".bat")) return false;

		const auto try_name = [&](const std::string& base)
		{
			if (dot != nullptr)
			{
				const std::string path = resolve(s, base);
				if (!is_file(s, path)) return false;
				found = path;
				return true;
			}
			for (const char* extension : {".COM", ".EXE", ".BAT"})
			{
				const std::string path = resolve(s, base + extension);
				if (is_file(s, path))
				{
					found = path;
					return true;
				}
			}
			return false;
		};

		if (try_name(command)) return true;
		if (std::strpbrk(command, "\\/:") != nullptr) return false;

		char path[512] = {};
		if (!dos_environment_value(s, environment, "PATH", path, sizeof(path))) return false;
		for (const char* entry = path; *entry != 0;)
		{
			const char* end = std::strchr(entry, ';');
			const size_t length = end != nullptr ? size_t(end - entry) : std::strlen(entry);
			std::string directory = trimmed(std::string(entry, length).c_str());
			entry += length + (end != nullptr ? 1 : 0);
			if (directory.empty()) continue;
			if (directory.back() != '\\') directory += '\\';
			if (try_name(directory + command)) return true;
		}
		return false;
	}

	command_result run_line(machine_state& s, const char* line, uint16_t environment, uint8_t& error, int depth);

	// IF [NOT] ERRORLEVEL n, IF [NOT] EXIST file, IF [NOT] a==b - then the
	// rest of the line, if the condition holds.
	command_result run_if(machine_state& s, const char* argument, uint16_t environment, uint8_t& error, int depth)
	{
		const char* q = skip_blanks(argument);
		bool negate = false;
		if (starts_with_word(q, "not"))
		{
			negate = true;
			q = skip_blanks(q + 3);
		}

		bool holds = false;
		if (starts_with_word(q, "errorlevel"))
		{
			// A fresh COMMAND.COM /C has run nothing yet, so its errorlevel is 0.
			q = skip_blanks(q + 10);
			const int level = std::atoi(q);
			while (*q != 0 && !is_blank(*q)) ++q;
			holds = level <= 0;
		}
		else if (starts_with_word(q, "exist"))
		{
			q = skip_blanks(q + 5);
			std::string name;
			while (*q != 0 && !is_blank(*q)) name += *q++;
			const std::string path = resolve(s, name);
			holds = has_wildcards(path.c_str()) ? !matches(s, path).empty() : is_file(s, path);
		}
		else
		{
			const char* equal = std::strstr(q, "==");
			if (equal == nullptr)
			{
				error = 0;
				set_return_code(s, 1);
				return command_result::done;
			}
			const std::string left = trimmed(std::string(q, size_t(equal - q)).c_str());
			q = skip_blanks(equal + 2);
			std::string right;
			while (*q != 0 && !is_blank(*q)) right += *q++;
			holds = left == right;
		}
		if (negate) holds = !holds;
		if (!holds)
		{
			set_return_code(s, 0);
			return command_result::done;
		}
		return run_line(s, q, environment, error, depth + 1);
	}

	// FOR %v IN (set) DO command. Every item runs, but only built-in commands
	// can: a program started for one item would leave the rest unrun.
	command_result run_for(machine_state& s, const char* argument, uint16_t environment, uint8_t& error, int depth,
	                       const printer& p)
	{
		const char* q = skip_blanks(argument);
		std::string variable;
		while (*q != 0 && !is_blank(*q)) variable += *q++;
		q = skip_blanks(q);
		const char* open = std::strchr(q, '(');
		const char* close = open != nullptr ? std::strchr(open, ')') : nullptr;
		const char* action = close != nullptr ? std::strstr(close, " ") : nullptr;
		if (variable.size() < 2 || variable[0] != '%' || !starts_with_word(q, "in") || close == nullptr ||
		    action == nullptr || !starts_with_word(skip_blanks(action), "do"))
		{
			p.error("Syntax error");
			set_return_code(s, 1);
			return command_result::done;
		}
		const std::string command = skip_blanks(skip_blanks(action) + 2);

		std::vector<std::string> items;
		for (const std::string& item : words(std::string(open + 1, size_t(close - open - 1)).c_str()))
		{
			if (!has_wildcards(item.c_str()))
			{
				items.push_back(item);
				continue;
			}
			for (const std::string& match : matches(s, resolve(s, item))) items.push_back(name_of(match.c_str()));
		}

		for (const std::string& item : items)
		{
			std::string expanded = command;
			for (size_t at = expanded.find(variable); at != std::string::npos; at = expanded.find(variable, at + item.size()))
				expanded.replace(at, variable.size(), item);
			char name[64] = {};
			const char* rest = nullptr;
			command_split(expanded.c_str(), name, sizeof(name), &rest);
			if (!is_builtin(name))
			{
				p.error("FOR can run built-in commands only");
				set_return_code(s, 1);
				return command_result::done;
			}
			const command_result result = run_line(s, expanded.c_str(), environment, error, depth + 1);
			if (result != command_result::done) return result;
		}
		return command_result::done;
	}

	command_result run_line(machine_state& s, const char* line, uint16_t environment, uint8_t& error, int depth)
	{
		static const command_output standard = {handle_output, handle_clear, nullptr};
		const printer p{s, standard};
		if (depth > 8)
		{
			set_return_code(s, 1);
			return command_result::done;
		}

		// IF, FOR and CALL take the rest of the line - redirections included -
		// as a command of their own.
		char command[64] = {};
		const char* argument = nullptr;
		command_split(line, command, sizeof(command), &argument);
		if (equals(command, "if")) return run_if(s, argument, environment, error, depth);
		if (equals(command, "for")) return run_for(s, argument, environment, error, depth, p);
		if (equals(command, "call")) return run_line(s, argument, environment, error, depth + 1);

		// Take out the redirections; what is left is the command.
		std::string rest;
		std::string input_name;
		std::string output_name;
		bool append = false;
		bool quoted = false;
		for (const char* q = line; *q != 0;)
		{
			if (*q == '"') quoted = !quoted;
			if (quoted || (*q != '<' && *q != '>' && *q != '|'))
			{
				rest += *q++;
				continue;
			}
			if (*q == '|')
			{
				p.error("Pipes are not supported");
				set_return_code(s, 1);
				return command_result::done;
			}
			const bool input = *q == '<';
			if (!input && q[1] == '>')
			{
				append = true;
				++q;
			}
			q = skip_blanks(q + 1);
			std::string name;
			while (*q != 0 && !is_blank(*q) && *q != '<' && *q != '>' && *q != '|') name += *q++;
			(input ? input_name : output_name) = name;
		}

		command_split(rest.c_str(), command, sizeof(command), &argument);
		if (command[0] == 0)
		{
			set_return_code(s, 0);
			return command_result::done;
		}

		// A drive on its own selects it.
		if (command[1] == ':' && command[2] == 0)
		{
			const int drive = upper(command[0]) - 'A';
			if (drive >= 0 && drive < 3) s.dos.current_drive = uint8_t(drive);
			set_return_code(s, 0);
			return command_result::done;
		}

		int input = -1;
		int output = -1;
		const auto release = [&]
		{
			dos_release_file(s, input);
			dos_release_file(s, output);
		};
		if (!input_name.empty())
		{
			input = dos_open_file(s, resolve(s, input_name).c_str(), host_open_read, false, false);
			if (input < 0)
			{
				p.error("File not found");
				set_return_code(s, 1);
				return command_result::done;
			}
		}
		if (!output_name.empty())
		{
			output = dos_open_file(s, resolve(s, output_name).c_str(), host_open_write, !append, append);
			if (output < 0)
			{
				release();
				p.error("File creation error");
				set_return_code(s, 1);
				return command_result::done;
			}
		}

		if (is_builtin(command))
		{
			// The caller's own standard handles point at the redirections
			// while the command runs, and back again afterwards.
			const int saved_input = input >= 0 ? dos_point_handle(s, 0, input) : -1;
			const int saved_output = output >= 0 ? dos_point_handle(s, 1, output) : -1;
			int code = 0;
			command_builtin(s, command, argument, standard, code);
			if (input >= 0) dos_release_file(s, dos_point_handle(s, 0, saved_input));
			if (output >= 0) dos_release_file(s, dos_point_handle(s, 1, saved_output));
			set_return_code(s, code);
			return command_result::done;
		}

		const std::string tail = argument[0] != 0 ? std::string(" ") + argument : std::string();

		// A program the host stands in for, found by name alone.
		if (s.host != nullptr)
		{
			char name[16] = {};
			const char* base = name_of(command);
			for (int i = 0; base[i] != 0 && base[i] != '.' && i < 15; ++i) name[i] = upper(base[i]);
			const std::string directory = current_directory(s, s.dos.current_drive);
			const int code = s.host->run_native(name, tail.c_str(), directory.c_str());
			if (code >= 0)
			{
				release();
				set_return_code(s, code);
				return command_result::done;
			}
		}

		std::string path;
		if (!find_program(s, command, environment, path))
		{
			release();
			p.error("Bad command or file name");
			set_return_code(s, 1);
			return command_result::done;
		}
		const char* dot = std::strrchr(path.c_str(), '.');
		if (dot != nullptr && equals(dot, ".bat"))
		{
			release();
			p.error("Batch files are not supported");
			set_return_code(s, 1);
			return command_result::done;
		}

		// The redirections go to the child, whose standard handles take them
		// over; when it ends, closing its handles closes them.
		if (!dos_start_child(s, path.c_str(), tail.c_str(), environment, input, output, error))
		{
			release();
			p.error(error == 8 ? "Program too big to fit in memory" : "Bad command or file name");
			set_return_code(s, 1);
			return command_result::done;
		}
		return command_result::started;
	}
}

void command_split(const char* line, char* command, int command_size, const char** argument)
{
	const char* p = skip_blanks(line);
	int length = 0;
	while (*p != 0 && !is_blank(*p) && std::strchr("/;,=<>|+", *p) == nullptr && length < command_size - 1)
	{
		// A built-in name runs straight into its argument in "echo.", "cd\"
		// and "cd..", where a program's name would carry on.
		if (std::strchr(".\\:[]", *p) != nullptr && length > 1)
		{
			command[length] = 0;
			if (is_builtin(command)) break;
		}
		command[length++] = *p++;
	}
	command[length] = 0;
	*argument = skip_blanks(p);
}

bool command_builtin(machine_state& s, const char* name, const char* argument, const command_output& out,
                     int& exit_code)
{
	const printer p{s, out};
	exit_code = 0;
	if (equals(name, "copy")) exit_code = command_copy(s, argument, p);
	else if (equals(name, "del") || equals(name, "erase")) exit_code = command_del(s, argument, p);
	else if (equals(name, "ren") || equals(name, "rename")) exit_code = command_ren(s, argument, p);
	else if (equals(name, "type")) exit_code = command_type(s, argument, p);
	else if (equals(name, "echo")) exit_code = command_echo(argument, p);
	else if (equals(name, "cd") || equals(name, "chdir")) exit_code = command_cd(s, argument, p);
	else if (equals(name, "md") || equals(name, "mkdir")) exit_code = command_md(s, argument, p);
	else if (equals(name, "rd") || equals(name, "rmdir")) exit_code = command_rd(s, argument, p);
	else if (equals(name, "set")) exit_code = command_set(s, argument, p);
	else if (equals(name, "path")) exit_code = command_path(s, argument, p);
	else if (equals(name, "attrib")) exit_code = command_attrib(s, argument, p);
	else if (equals(name, "dir")) exit_code = command_dir(s, argument, p);
	else if (equals(name, "cls"))
	{
		if (out.clear != nullptr) out.clear(s, out.context);
	}
	else if (equals(name, "ver"))
	{
		char text[48] = {};
		snprintf(text, sizeof(text), "libdos - DOS %u.%02u compatible", s.dos.config.version_major,
		         s.dos.config.version_minor);
		p.line(text);
	}
	else if (equals(name, "vol"))
	{
		p.line(" Volume in drive C has no label");
	}
	else if (equals(name, "date") || equals(name, "time"))
	{
		static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
		int year, month, day, weekday, hour, minute, second, hundredths;
		clock_now(s, year, month, day, weekday, hour, minute, second, hundredths);
		char text[64] = {};
		if (equals(name, "date"))
			snprintf(text, sizeof(text), "Current date is %s %02d-%02d-%04d", days[weekday % 7], month, day, year);
		else
			snprintf(text, sizeof(text), "Current time is %2d:%02d:%02d.%02d", hour, minute, second, hundredths);
		p.line(text);
	}
	else if (equals(name, "break")) exit_code = command_switch("BREAK", argument, s.dos.ctrl_break, p);
	else if (equals(name, "verify"))
	{
		bool verify = s.dos.verify != 0;
		exit_code = command_switch("VERIFY", argument, verify, p);
		s.dos.verify = verify ? 1 : 0;
	}
	else if (equals(name, "pause"))
	{
		p.text("Strike a key when ready . . . ");
		uint8_t key = 0;
		const int file = dos_handle_file(s, 0);
		uint8_t probe = 0;
		const bool keyboard = file >= 0 && s.dos.files[file].device == dos_device_con &&
			(s.host == nullptr || s.host->console_read(&probe, 0) < 0);
		if (!keyboard) dos_read_handle(s, 0, &key, 1);
		p.line();
	}
	else if (equals(name, "rem") || equals(name, "ctty") || equals(name, "goto") || equals(name, "shift") ||
	         equals(name, "exit"))
	{
		// Nothing to do outside a batch file or an interactive COMMAND.COM.
	}
	else
	{
		return false;
	}
	return true;
}

command_result command_exec(machine_state& s, const char* tail, uint16_t environment, uint8_t& error)
{
	const char* p = skip_blanks(tail);
	if (p[0] != '/' || (p[1] != 'c' && p[1] != 'C')) return command_result::not_a_command;
	error = 0;
	return run_line(s, p + 2, environment, error, 0);
}
