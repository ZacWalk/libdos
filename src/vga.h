#pragma once

// ovrwin by Zac Walker
//
// IBM VGA/EGA/CGA display adapter: 256 KiB of display memory with the planar
// write logic, the sequencer / CRTC / graphics / attribute register files, the
// 256-entry DAC, and a renderer that turns all of that into a 32-bit frame
// buffer. No host types appear here - the host presents the pixels.

#include <cstdint>

struct machine_state;

constexpr int vga_max_width = 720;
constexpr int vga_max_height = 480;
constexpr uint32_t vga_plane_size = 0x10000;

// How CPU addresses in the A0000-BFFFF window reach display memory.
enum class vga_mapping
{
	planar, // one address per plane, gated by the write mask and latches
	odd_even, // address bit 0 picks the plane (text and CGA modes)
	chain4, // address bits 0-1 pick the plane (mode 13h)
};

enum class vga_render_mode
{
	text,
	cga2, // 640x200 1bpp
	cga4, // 320x200 2bpp
	planar4, // EGA/VGA 16 colour
	linear8, // 256 colour
};

struct vga_state
{
	// Display memory held plane-interleaved: byte `offset` of plane `p` is at
	// mem[offset * 4 + p]. That makes mode 13h a plain linear array and keeps
	// the planar renderer's inner loop to one indexed read per plane.
	uint8_t mem[vga_plane_size * 4] = {};

	uint8_t seq[8] = {};
	uint8_t crtc[32] = {};
	uint8_t gfx[16] = {};
	uint8_t attr[32] = {};
	uint8_t seq_index = 0, crtc_index = 0, gfx_index = 0, attr_index = 0;
	bool attr_flipflop = false; // 3C0 alternates between index and data writes
	uint8_t misc_output = 0x67;
	uint8_t feature_control = 0;
	uint8_t cga_mode_control = 0; // port 3D8
	uint8_t cga_color_select = 0; // port 3D9

	uint8_t latch[4] = {};

	uint8_t dac[256][3] = {}; // 6-bit components, as written through 3C9
	uint8_t dac_write_index = 0, dac_read_index = 0;
	int dac_channel = 0;
	uint8_t dac_mask = 0xff;

	// Character generator. Seeded from the built-in ROM font; a program that
	// uploads its own font through INT 10h AX=1100 replaces entries here.
	uint8_t font[256 * 32] = {};

	uint8_t mode = 3; // BIOS mode number
	vga_mapping mapping = vga_mapping::odd_even;
	vga_render_mode render_mode = vga_render_mode::text;
	int width = 720, height = 400;
	int text_columns = 80, text_rows = 25;
	int char_height = 16;
	uint32_t display_start = 0;

	uint8_t page = 0;
	uint8_t cursor_x[8] = {}, cursor_y[8] = {};
	uint8_t cursor_start = 14, cursor_end = 15;
	bool cursor_visible = true;

	uint64_t frame = 0;
	uint32_t framebuffer[vga_max_width * vga_max_height] = {};
	bool render_enabled = true;
	bool dirty = true;
};

void vga_reset(vga_state& v);
void vga_set_mode(machine_state& s, uint8_t mode);

uint8_t vga_mem_read(machine_state& s, uint32_t addr);
void vga_mem_write(machine_state& s, uint32_t addr, uint8_t value);

uint8_t vga_port_read(machine_state& s, uint16_t port);
void vga_port_write(machine_state& s, uint16_t port, uint8_t value);

void vga_render(machine_state& s);

// Text-mode helpers shared with the BIOS INT 10h implementation.
void vga_write_char(machine_state& s, uint8_t page, uint8_t ch, uint8_t attr, bool use_attr);
void vga_scroll_up(machine_state& s, int lines, uint8_t attr, int left, int top, int right, int bottom);
void vga_set_pixel(machine_state& s, int x, int y, uint8_t colour);
uint8_t vga_get_pixel(machine_state& s, int x, int y);
uint32_t vga_dac_colour(const vga_state& v, uint8_t index);

extern const uint8_t rom_font_8x8[256 * 8];
