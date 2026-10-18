#pragma once

// libdos by Zac Walker
//
// The only way the emulator reaches the outside world. The library never opens
// a file, allocates a window or reads a clock; a host implements these calls
// and decides what the emulated C: drive actually is.

#include <cstdint>

struct host_file_info
{
	char name[13] = {}; // 8.3, upper case, no path
	uint32_t size = 0;
	uint16_t dos_date = 0; // packed: yyyyyyym mmmddddd, year from 1980
	uint16_t dos_time = 0; // packed: hhhhhmmm mmmsssss, seconds / 2
	uint8_t attributes = 0;
	bool directory = false;
};

// DOS open modes, as passed to INT 21h AH=3Dh.
constexpr int host_open_read = 0;
constexpr int host_open_write = 1;
constexpr int host_open_read_write = 2;

class host_services
{
public:
	virtual ~host_services() = default;

	// A path here is always a normalised DOS path such as "C:\GAME\DATA.BIN".
	// Mapping it onto real storage is the host's business.
	virtual int file_open(const char* path, int access) = 0;
	virtual int file_create(const char* path, uint8_t attributes) = 0;
	virtual void file_close(int handle) = 0;
	virtual int file_read(int handle, uint8_t* buffer, int length) = 0;
	virtual int file_write(int handle, const uint8_t* buffer, int length) = 0;
	virtual int32_t file_seek(int handle, int32_t offset, int origin) = 0;

	// Sets a file's length to its current position: DOS's AH=40h writing no
	// bytes. A host that cannot leaves the file as it is.
	virtual bool file_truncate(int handle)
	{
		(void)handle;
		return false;
	}

	virtual bool file_delete(const char* path) = 0;
	virtual bool file_rename(const char* from, const char* to) = 0;

	// Copies a file as DOS's COPY copies one, keeping its date. The default
	// goes through the calls above and so leaves the date as now.
	virtual bool file_copy(const char* from, const char* to)
	{
		const int source = file_open(from, host_open_read);
		if (source < 0) return false;
		const int target = file_create(to, 0);
		if (target < 0)
		{
			file_close(source);
			return false;
		}
		uint8_t buffer[4096];
		bool ok = true;
		for (;;)
		{
			const int got = file_read(source, buffer, int(sizeof(buffer)));
			if (got <= 0) break;
			if (file_write(target, buffer, got) != got)
			{
				ok = false;
				break;
			}
		}
		file_close(source);
		file_close(target);
		return ok;
	}
	virtual bool file_info(const char* path, host_file_info& info) = 0;
	virtual bool file_set_attributes(const char* path, uint8_t attributes) = 0;

	// Directory search. find_first returns a search id (>= 0) or -1; the id is
	// stored in the caller's DTA so nested searches stay independent. DOS
	// keeps up to dos_max_searches (16) of a program's searches open, so a
	// host has to manage at least that many at once.
	virtual int find_first(const char* pattern, uint8_t attributes, host_file_info& info) = 0;
	virtual bool find_next(int search, host_file_info& info) = 0;
	virtual void find_close(int search) = 0;

	virtual bool make_directory(const char* path) = 0;
	virtual bool remove_directory(const char* path) = 0;
	virtual bool directory_exists(const char* path) = 0;
	virtual uint32_t free_space() = 0;

	// Wall clock, split the way INT 21h AH=2Ah/2Ch want it.
	virtual void clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
	                   int& hundredths) = 0;

	// The DOS console. A host with a terminal of its own - demu - can take it
	// over; by default it stays on the emulated screen and keyboard.
	// console_write returns false to leave the bytes for the screen. `handle`
	// is the DOS handle the program wrote to, so 2 can go to an error stream.
	virtual bool console_write(int handle, const uint8_t* data, int length)
	{
		(void)handle;
		(void)data;
		(void)length;
		return false;
	}

	// Reads up to `length` bytes of console input: the count, 0 at the end of
	// the input, or -1 to leave input to the emulated keyboard. A read of no
	// bytes asks only whether the host has the console: 0 if it has, -1 if not.
	virtual int console_read(uint8_t* data, int length)
	{
		(void)data;
		(void)length;
		return -1;
	}

	// What AX=4400h reports for standard handle 0, 1 or 2: a character device
	// unless the host's own stream has been redirected to a file.
	virtual bool console_is_device(int handle)
	{
		(void)handle;
		return true;
	}

	// A program the host runs itself instead of loading it, for a tool that
	// cannot run emulated - LIB.EXE is an OS/2 bound executable. Checked on
	// every EXEC. `name` is the base name in upper case with no extension,
	// `tail` the command tail and `directory` the DOS current directory, such
	// as "C:\SRC". Returns the exit code, or -1 if the host has no such program.
	virtual int run_native(const char* name, const char* tail, const char* directory)
	{
		(void)name;
		(void)tail;
		(void)directory;
		return -1;
	}

	// Memory that may be written and then executed. Emitting machine code for
	// the host is byte manipulation and needs no platform header; getting a
	// page it can run from is the one part that does. Returning null means the
	// host cannot do it, and the caller falls back to interpreting.
	virtual void* code_alloc(uint32_t size)
	{
		(void)size;
		return nullptr;
	}

	virtual void code_free(void* code, uint32_t size)
	{
		(void)code;
		(void)size;
	}

	// Called once the bytes are written, so a host can flush whatever caches
	// the instruction fetch path keeps.
	virtual void code_commit(void* code, uint32_t size)
	{
		(void)code;
		(void)size;
	}

	// Diagnostics. The CLI prints these; the GUI drops them.
	virtual void trace(const char* text) { (void)text; }

	// Every character a program writes through INT 10h's teletype, character
	// and string calls, in order: the console as a stream, which a test can
	// wait on where the screen would have scrolled it away.
	virtual void video_text(uint8_t character) { (void)character; }
};
