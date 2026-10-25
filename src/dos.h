#pragma once

// libdos by Zac Walker
//
// The DOS personality: the MZ/COM loader, the PSP and MCB memory chain, and
// the INT 21h service layer. This is the "Wine" part of the emulator - there
// is no DOS kernel in memory, only a faithful implementation of the interface
// programs were written against.

#include <cstdint>

#include "cpu.h"

struct machine_state;

constexpr int dos_max_files = 64; // system file table entries, shared by every process
constexpr int dos_max_searches = 16;
constexpr int dos_max_nesting = 8;
constexpr int dos_handles = 20; // the handle table a PSP carries at offset 18h

// What a system file table entry refers to.
constexpr uint8_t dos_device_none = 0; // a file on the drive
constexpr uint8_t dos_device_con = 1;
constexpr uint8_t dos_device_aux = 2;
constexpr uint8_t dos_device_prn = 3;
constexpr uint8_t dos_device_nul = 4;

// One entry of the system file table. A process reaches it through the handle
// table in its PSP, so a duplicated or an inherited handle shares the entry -
// and with it the file position - exactly as on DOS.
struct dos_file
{
	int references = 0; // handles pointing here, in every process; 0 = free
	bool permanent = false; // the standard devices are never released
	int host_handle = -1;
	uint8_t device = dos_device_none;
	uint8_t mode = 0; // the open mode; bit 7 set means a child does not inherit it
	char path[128] = {};
};

struct dos_search
{
	bool used = false;
	int host_search = -1;
	uint32_t stamp = 0; // also kept in the DTA, so a search that was closed is not mistaken for its successor
	uint32_t last_used = 0; // when the program last stepped it: the longest idle makes way for a new one
};

// What a parent process needs back when the program it started through
// INT 21h AH=4Bh terminates.
struct dos_exec_frame
{
	cpu_state cpu;
	uint16_t psp = 0;
	uint32_t dta = 0;
};

// What an embedder decides about the DOS personality. dos_reset keeps it.
struct dos_config
{
	// What INT 21h AH=30h reports.
	uint8_t version_major = 5;
	uint8_t version_minor = 0;

	// The environment of a program the host starts, as NAME=VALUE strings each
	// ended by a NUL. Empty means the built-in default.
	char environment[4096] = {};
	int environment_length = 0;
};

struct dos_state
{
	uint16_t psp_segment = 0;
	uint16_t environment_segment = 0;
	uint16_t first_mcb = 0;
	uint16_t load_segment = 0;
	uint32_t dta = 0; // segment:offset, packed seg << 16 | offset
	uint8_t current_drive = 2; // 0 = A:
	char cwd[3][64] = {}; // per-drive current directory, "\GAMES\WL" with no trailing backslash
	uint8_t return_code = 0;
	uint8_t return_type = 0;
	bool terminated = false;
	bool ctrl_break = false;
	uint8_t verify = 0;
	uint8_t switch_char = '/';

	// The INT 21h AH=0Ah read in progress, as DS:DX, which spans one call per key.
	bool line_input = false;
	uint32_t line_buffer = 0;

	// The scancode an extended key still owes the next AH=01h/06h/07h/08h.
	bool extended_pending = false;
	uint8_t extended_scancode = 0;

	dos_file files[dos_max_files];
	dos_search searches[dos_max_searches];
	uint32_t search_stamp = 0; // counts find-firsts and find-nexts, so stamps never repeat

	char program_path[128] = {};
	uint32_t entry_cs_ip = 0;
	uint32_t entry_ss_sp = 0;
	uint16_t image_segment = 0;
	uint32_t image_bytes = 0;

	dos_exec_frame exec_stack[dos_max_nesting];
	int exec_depth = 0;

	dos_config config;
};

void dos_reset(machine_state& s);

// Loads a DOS executable image (MZ or COM) and leaves the CPU at its entry
// point. `program_path` is the DOS path the program will see in its PSP
// environment; `command_line` is everything after the program name.
bool dos_load_program(machine_state& s, const uint8_t* image, uint32_t size, const char* program_path,
                      const char* command_line, char* error, int error_size);

// Services INT 20h, 21h, 27h, 28h and 2Fh. Returns false if the vector is not
// ours, so the caller can fall through to the default handler.
bool dos_interrupt(machine_state& s, uint8_t vector);

// Path handling, shared with the host: turns whatever the program passed into
// a normalised "C:\DIR\NAME.EXT" with no relative components.
void dos_resolve_path(machine_state& s, const char* input, char* out, int out_size);

uint16_t dos_alloc(machine_state& s, uint16_t paragraphs, uint16_t& largest);
bool dos_free(machine_state& s, uint16_t segment);
bool dos_resize(machine_state& s, uint16_t segment, uint16_t paragraphs, uint16_t& largest);

// Handle I/O as INT 21h AH=3Fh and AH=40h do it, through the current
// process's handle table, so a redirected handle reaches its file. Both return
// the count transferred, or -1. A console read returns 0 when the input is not
// ready yet; the caller decides whether to wait.
int dos_read_handle(machine_state& s, uint16_t handle, uint8_t* data, int length);
int dos_write_handle(machine_state& s, uint16_t handle, const uint8_t* data, int length);

// The system file table entry a handle of the current process refers to, or -1.
int dos_handle_file(machine_state& s, uint16_t handle);

// Opens (or creates, or opens for appending) a file - or a device such as NUL
// - as a system file table entry with no handle yet. -1 if it cannot be.
int dos_open_file(machine_state& s, const char* path, int access, bool create, bool append,
                  uint8_t attributes = 0);

// Points a handle of the current process at a system file table entry and
// returns the one it pointed at before, or -1; `file` -1 closes the handle.
// Redirection is this, done before a command and undone after it.
int dos_point_handle(machine_state& s, uint16_t handle, int file);

// Drops one reference to an entry, closing the file when none are left.
void dos_release_file(machine_state& s, int file);

// Starts `path` as a child of the program that is in INT 21h AH=4Bh, exactly
// as that call does: the CPU moves to the child, and the parent carries on
// after its INT when the child ends. `environment` is the block to copy for
// the child, 0 for the parent's own. A child's handles 0 and 1 can be pointed
// elsewhere (-1 inherits them). On failure returns false with the DOS error.
bool dos_start_child(machine_state& s, const char* path, const char* tail, uint16_t environment, int input,
                     int output, uint8_t& error);

// The value of `name` in an environment block (0 for the current process's),
// copied into `out`; false if it is not set.
bool dos_environment_value(machine_state& s, uint16_t environment, const char* name, char* out, int out_size);
