#pragma once

// ovrwin by Zac Walker
//
// 8086/80186 disassembler and machine introspection. Both are strictly
// side-effect free: nothing here reads through a port or acknowledges an
// interrupt, so a debugger can look at a running machine without changing it.

#include <cstdint>

struct machine_state;

struct disassembly
{
	uint16_t length = 1;
	char bytes[32] = {};
	char text[64] = {};
};

disassembly disassemble(const machine_state& s, uint16_t segment, uint16_t offset);

struct debug_state
{
	uint16_t ax, bx, cx, dx, si, di, bp, sp;
	uint16_t cs, ds, es, ss, ip, flags;
	uint64_t cycles;
	uint8_t video_mode;
	int video_width, video_height;
	uint16_t psp_segment;
	uint32_t dta;
	uint16_t free_paragraphs;
	uint16_t largest_block;
	int open_files;
	uint32_t timer_ticks;
	uint16_t timer_reload;
	bool adlib_active;
	disassembly code[8];
};

void debug_capture(const machine_state& s, debug_state& out);
int debug_format(const debug_state& state, char* out, int out_size);
