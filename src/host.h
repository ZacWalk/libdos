#pragma once

// ovrwin by Zac Walker
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
	virtual bool file_delete(const char* path) = 0;
	virtual bool file_rename(const char* from, const char* to) = 0;
	virtual bool file_info(const char* path, host_file_info& info) = 0;
	virtual bool file_set_attributes(const char* path, uint8_t attributes) = 0;

	// Directory search. find_first returns a search id (>= 0) or -1; the id is
	// stored in the caller's DTA so nested searches stay independent.
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
};
