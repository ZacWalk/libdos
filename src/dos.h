#pragma once

// ovrwin by Zac Walker
//
// The DOS personality: the MZ/COM loader, the PSP and MCB memory chain, and
// the INT 21h service layer. This is the "Wine" part of the emulator - there
// is no DOS kernel in memory, only a faithful implementation of the interface
// programs were written against.

#include <cstdint>

#include "cpu.h"

struct machine_state;

constexpr int dos_max_files = 64;
constexpr int dos_max_searches = 16;
constexpr int dos_max_nesting = 8;

struct dos_file
{
	bool used = false;
	int host_handle = -1;
	uint8_t device = 0; // 0 = disk, 1 = CON, 2 = AUX, 3 = PRN
	uint8_t info = 0; // device info word low byte, reported by AX=4400h
	char path[128] = {};
};

struct dos_search
{
	bool used = false;
	int host_search = -1;
	char pattern[128] = {};
	uint8_t attributes = 0;
};

// What a parent process needs back when the program it started through
// INT 21h AH=4Bh terminates.
struct dos_exec_frame
{
	cpu_state cpu;
	uint16_t psp = 0;
	uint32_t dta = 0;
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

	char program_path[128] = {};
	uint32_t entry_cs_ip = 0;
	uint32_t entry_ss_sp = 0;
	uint16_t image_segment = 0;
	uint32_t image_bytes = 0;

	dos_exec_frame exec_stack[dos_max_nesting];
	int exec_depth = 0;
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
