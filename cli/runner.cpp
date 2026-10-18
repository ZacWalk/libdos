// libdos by Zac Walker
//
// demu as a program runner, the way the original demu of dos4z ran the 1988
// MS-DOS build tools:
//
//     demu [options] program [arguments]
//
// The program runs headlessly to its end, with its console on stdin, stdout
// and stderr and its exit code returned. A directory on the host is drive C:
// and nothing outside it can be reached; the environment variables a build
// tool reads are passed in with their paths moved onto that drive.

#include "cli.h"
#include "lib-omf.h"

#include "../host/host-win32.h"
#include "../src/machine.h"

#include <direct.h>
#include <fcntl.h>
#include <io.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// The folder host, with the host's terminal as the DOS console and the
	// tools that cannot run emulated standing in for themselves.
	class runner_host final : public win32_host
	{
	public:
		bool console_write(int handle, const uint8_t* data, int length) override
		{
			// Unbuffered, as the original demu wrote, so a log that merges the
			// two streams keeps them in order.
			fflush(stdout);
			_write(handle == 2 ? 2 : 1, data, unsigned(length));
			return true;
		}

		int console_read(uint8_t* data, int length) override
		{
			if (length <= 0) return 0;
			const int got = _read(0, data, unsigned(length));
			return got > 0 ? got : 0;
		}

		bool console_is_device(int handle) override { return _isatty(handle) != 0; }

		// A fixed date, so that a build comes out the same whenever it is run,
		// and the real time of day, which CL folds into the names of its
		// temporary files.
		void clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
		           int& hundredths) override
		{
			win32_host::clock(year, month, day, weekday, hour, minute, second, hundredths);
			year = 1988;
			month = 6;
			day = 15;
			weekday = 3;
		}

		// LIB.EXE is an OS/2 bound executable; the library manager in
		// lib-omf.c does its work, on host paths relative to the directory the
		// program was in.
		int run_native(const char* name, const char* tail, const char* directory) override
		{
			if (std::strcmp(name, "LIB") != 0) return -1;
			char host_directory[MAX_PATH] = {};
			char saved[MAX_PATH] = {};
			if (!to_windows_path(directory, host_directory, MAX_PATH) || _getcwd(saved, MAX_PATH) == nullptr ||
			    _chdir(host_directory) != 0)
				return 1;
			omf_lib_verbose = verbose() ? 1 : 0;
			const int code = omf_lib(tail);
			fflush(stdout);
			fflush(stderr);
			_chdir(saved);
			return code;
		}

		void trace(const char* text) override
		{
			if (verbose()) fprintf(stderr, "[demu] %s\n", text);
		}
	};

	// A path in the form a comparison can rely on: full, and with any 8.3
	// short names (C:\USERS\ZACWAL~1) expanded.
	std::string long_path(const char* path)
	{
		char full[MAX_PATH] = {};
		if (GetFullPathNameA(path, MAX_PATH, full, nullptr) == 0) return path;
		char expanded[MAX_PATH] = {};
		const DWORD length = GetLongPathNameA(full, expanded, MAX_PATH);
		return length > 0 && length < MAX_PATH ? expanded : full;
	}

	// A host path under the root, as the DOS path a program sees it by.
	bool dos_path_of(const std::string& root, const char* host_path, std::string& out)
	{
		const std::string full = long_path(host_path);
		if (_strnicmp(full.c_str(), root.c_str(), root.size()) != 0) return false;
		const char* rest = full.c_str() + root.size();
		// C:\SRC2 is not under C:\SRC.
		if (root.back() != '\\' && *rest != 0 && *rest != '\\') return false;
		while (*rest == '\\') ++rest;
		out = "C:\\";
		for (; *rest != 0; ++rest) out += char(std::toupper(static_cast<unsigned char>(*rest)));
		return true;
	}

	// Whether every component of a path is a name DOS can use: eight
	// characters, an optional extension of three, and no spaces.
	bool dos_can_name(const std::string& path)
	{
		size_t start = 3; // past "C:\"
		while (start < path.size())
		{
			size_t end = path.find('\\', start);
			if (end == std::string::npos) end = path.size();
			const std::string part = path.substr(start, end - start);
			const size_t dot = part.find('.');
			const size_t base = dot == std::string::npos ? part.size() : dot;
			const size_t extension = dot == std::string::npos ? 0 : part.size() - dot - 1;
			if (base == 0 || base > 8 || extension > 3 || part.find(' ') != std::string::npos ||
			    (dot != std::string::npos && part.find('.', dot + 1) != std::string::npos))
				return false;
			start = end + 1;
		}
		return true;
	}

	// A variable's value with its host paths moved onto drive C:. A list is
	// done entry by entry, and an entry outside the root, or one no DOS
	// program could use, is left out.
	std::string translate(const std::string& root, const char* value)
	{
		std::string out;
		for (const char* p = value;;)
		{
			const char* end = std::strchr(p, ';');
			const std::string entry(p, end != nullptr ? size_t(end - p) : std::strlen(p));
			const bool absolute = entry.size() >= 3 && std::isalpha(static_cast<unsigned char>(entry[0])) &&
				entry[1] == ':' && (entry[2] == '\\' || entry[2] == '/');
			std::string mapped = entry;
			if (absolute && (!dos_path_of(root, entry.c_str(), mapped) || !dos_can_name(mapped))) mapped.clear();
			if (!mapped.empty())
			{
				if (!out.empty()) out += ';';
				out += mapped;
			}
			if (end == nullptr) break;
			p = end + 1;
		}
		return out;
	}

	// The command tail, as the original demu built it: each argument after a
	// space, quoted if it has one in it.
	std::string command_tail(int argc, char** argv)
	{
		std::string tail;
		for (int i = 0; i < argc; ++i)
		{
			tail += ' ';
			const bool quote = std::strchr(argv[i], ' ') != nullptr;
			if (quote) tail += '"';
			for (const char* p = argv[i]; *p != 0; ++p)
			{
				if (*p == '"') tail += '\\';
				tail += *p;
			}
			if (quote) tail += '"';
		}
		return tail;
	}

	bool read_whole_file(const char* path, std::vector<uint8_t>& data)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || file == nullptr) return false;
		uint8_t buffer[65536];
		for (;;)
		{
			const size_t got = fread(buffer, 1, sizeof(buffer), file);
			data.insert(data.end(), buffer, buffer + got);
			if (got < sizeof(buffer)) break;
		}
		fclose(file);
		return !data.empty();
	}

	bool exists(const std::string& path)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}
}

void runner_usage()
{
	printf("Running a program:\n"
	       "  demu [options] <program> [arguments]\n"
	       "      runs a DOS program to its end with its console on this one, and\n"
	       "      returns its exit code\n\n"
	       "  -r <dir>        the host directory that is drive C: (default: the root of\n"
	       "                  the current drive); nothing outside it can be reached\n"
	       "  -e <name>[=<v>] pass one more environment variable; PATH, INCLUDE, LIB,\n"
	       "                  INIT, TMP, TEMP and COUNTRY are passed already\n"
	       "  -cpu <model>    8086-strict (default), 8086, 186 or 386\n"
	       "  -shell          report DOS 4.00 rather than 3.20, for COMMAND.COM\n"
	       "  -v              report DOS calls and file opens on stderr\n"
	       "  -d              describe the executable and exit\n");
}

int run_dos_program(int argc, char** argv)
{
	bool verbose = false;
	bool dump = false;
	bool shell = false;
	cpu_model model = cpu_model::i8086_strict;
	std::string root;
	std::vector<std::string> extra;

	int i = 0;
	for (; i < argc && argv[i][0] == '-'; ++i)
	{
		const char* option = argv[i];
		if (std::strcmp(option, "--") == 0)
		{
			++i;
			break;
		}
		if (std::strcmp(option, "-v") == 0) verbose = true;
		else if (std::strcmp(option, "-d") == 0) dump = true;
		else if (std::strcmp(option, "-shell") == 0) shell = true;
		else if (std::strcmp(option, "-r") == 0 && i + 1 < argc) root = argv[++i];
		else if (std::strcmp(option, "-e") == 0 && i + 1 < argc) extra.push_back(argv[++i]);
		else if (std::strcmp(option, "-cpu") == 0 && i + 1 < argc)
		{
			const char* name = argv[++i];
			if (std::strcmp(name, "8086-strict") == 0) model = cpu_model::i8086_strict;
			else if (std::strcmp(name, "8086") == 0 || std::strcmp(name, "8088") == 0) model = cpu_model::i8086;
			else if (std::strcmp(name, "186") == 0) model = cpu_model::i80186;
			else if (std::strcmp(name, "386") == 0) model = cpu_model::i386;
			else
			{
				fprintf(stderr, "demu: unknown processor %s\n", name);
				return 1;
			}
		}
		else
		{
			fprintf(stderr, "demu: unknown option %s\n\n", option);
			runner_usage();
			return 1;
		}
	}
	if (i >= argc)
	{
		runner_usage();
		return 1;
	}

	// The program, with .COM and then .EXE tried if it was named without one.
	std::string program = argv[i++];
	const size_t separator = program.find_last_of("\\/");
	if (program.find('.', separator == std::string::npos ? 0 : separator) == std::string::npos)
		program += exists(program + ".COM") ? ".COM" : ".EXE";
	if (dump) return show_info(program.c_str());

	char cwd[MAX_PATH] = {};
	GetCurrentDirectoryA(MAX_PATH, cwd);
	if (root.empty()) root = std::string(cwd, 3); // "C:\"

	runner_host host;
	if (!host.set_root(root.c_str()))
	{
		fprintf(stderr, "demu: cannot use %s as drive C:\n", root.c_str());
		return 1;
	}
	root = long_path(host.root());
	host.set_verbose(verbose);
	// Byte for byte both ways: a program's CR LF must not become CR CR LF.
	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	_setmode(2, _O_BINARY);

	std::string dos_program;
	std::string dos_cwd;
	if (!dos_path_of(root, program.c_str(), dos_program) || !dos_path_of(root, cwd, dos_cwd))
	{
		fprintf(stderr, "demu: %s and the current directory have to be inside %s\n", program.c_str(), root.c_str());
		return 1;
	}

	const std::string tail = command_tail(argc - i, argv + i);

	// LIB stands in for itself here too, so that "demu LIB.EXE ..." works.
	char base[16] = {};
	const char* file = dos_program.c_str() + dos_program.rfind('\\') + 1;
	for (int n = 0; file[n] != 0 && file[n] != '.' && n < 15; ++n) base[n] = file[n];
	const int native = host.run_native(base, tail.c_str(), dos_cwd.c_str());
	if (native >= 0) return native;

	std::vector<uint8_t> image;
	if (!read_whole_file(program.c_str(), image))
	{
		fprintf(stderr, "demu: cannot read %s\n", program.c_str());
		return 1;
	}

	// The environment a build tool reads, in the order the original demu
	// gave it, with every host path moved onto drive C:.
	std::vector<std::string> variables = {"COMSPEC=C:\\COMMAND.COM"};
	const auto add = [&](const std::string& assignment)
	{
		const size_t equals = assignment.find('=');
		std::string variable = assignment.substr(0, equals);
		for (char& ch : variable) ch = char(std::toupper(static_cast<unsigned char>(ch)));
		std::string value;
		if (equals != std::string::npos) value = assignment.substr(equals + 1);
		else
		{
			char host_value[4096] = {};
			if (GetEnvironmentVariableA(variable.c_str(), host_value, sizeof(host_value)) == 0) return;
			value = host_value;
		}
		value = translate(root, value.c_str());
		if (!value.empty()) variables.push_back(variable + "=" + value);
	};
	for (const char* variable : {"PATH", "INCLUDE", "LIB", "INIT", "TMP", "TEMP", "COUNTRY"}) add(variable);
	for (const std::string& assignment : extra) add(assignment);
	std::vector<const char*> strings;
	for (const std::string& variable : variables) strings.push_back(variable.c_str());

	machine m;
	m.set_host(&host);
	m.set_cpu_model(model);
	m.set_dos_version(shell ? 4 : 3, shell ? 0 : 20);
	if (!m.set_environment(strings.data(), int(strings.size())))
	{
		fprintf(stderr, "demu: the environment is too big\n");
		return 1;
	}
	if (!m.set_current_directory(dos_cwd.c_str()))
	{
		fprintf(stderr, "demu: cannot make %s the current directory\n", dos_cwd.c_str());
		return 1;
	}
	if (!m.load_program(image.data(), uint32_t(image.size()), dos_program.c_str(), tail.c_str()))
	{
		fprintf(stderr, "demu: %s: %s\n", program.c_str(), m.error());
		return 1;
	}
	m.set_render_enabled(false);
	m.state().trace = verbose;

	while (!m.exited()) m.exec(50000000);

	fflush(stdout);
	if (m.faulted())
	{
		fprintf(stderr, "demu: %s\n", m.error());
		return 1;
	}
	return m.exit_code();
}
