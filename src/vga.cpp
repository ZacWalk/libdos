// ovrwin by Zac Walker
//
// VGA/EGA/CGA display adapter. Display memory is held plane-interleaved, the
// CPU-side write logic implements the graphics controller's four write modes,
// and vga_render turns whatever the current mode is into a 32-bit frame buffer
// at the mode's native resolution. The host scales; the library never does.

#include "machine.h"

#include <cmath>
#include <cstring>

namespace
{
	// The 16 EGA colours as 6-bit DAC values, in attribute order.
	const uint8_t ega_palette[16][3] = {
		{0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42},
		{42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
		{21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
		{63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
	};

	const uint8_t grey_ramp[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};

	void hsv_to_dac(double h, double s, double v, uint8_t* out)
	{
		const double c = v * s;
		const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
		const double m = v - c;
		double r = 0, g = 0, b = 0;
		if (h < 60) { r = c; g = x; }
		else if (h < 120) { r = x; g = c; }
		else if (h < 180) { g = c; b = x; }
		else if (h < 240) { g = x; b = c; }
		else if (h < 300) { r = x; b = c; }
		else { r = c; b = x; }
		out[0] = uint8_t((r + m) * 63.0 + 0.5);
		out[1] = uint8_t((g + m) * 63.0 + 0.5);
		out[2] = uint8_t((b + m) * 63.0 + 0.5);
	}

	// The palette the VGA BIOS programs for a 256-colour mode: the EGA set,
	// a 16-step grey ramp, then 216 colours as 3 brightness groups of 3
	// saturations of 24 hues.
	void default_dac(vga_state& v)
	{
		std::memset(v.dac, 0, sizeof(v.dac));
		for (int i = 0; i < 16; ++i)
		{
			v.dac[i][0] = ega_palette[i][0];
			v.dac[i][1] = ega_palette[i][1];
			v.dac[i][2] = ega_palette[i][2];
		}
		for (int i = 0; i < 16; ++i)
		{
			v.dac[16 + i][0] = grey_ramp[i];
			v.dac[16 + i][1] = grey_ramp[i];
			v.dac[16 + i][2] = grey_ramp[i];
		}
		const double values[3] = {1.0, 0.45, 0.26};
		const double saturations[3] = {1.0, 0.5, 0.25};
		int index = 32;
		for (int value = 0; value < 3; ++value)
			for (int saturation = 0; saturation < 3; ++saturation)
				for (int hue = 0; hue < 24; ++hue)
					hsv_to_dac(hue * 15.0, saturations[saturation], values[value], v.dac[index++]);
	}

	// A VGA in a 16-colour or text mode programs its DAC with the 64 EGA
	// colours, indexed by the rgbRGB value an attribute palette register holds.
	// Rendering an EGA mode straight out of the 256-colour palette instead is
	// what turns a game's artwork psychedelic.
	void ega_dac(vga_state& v)
	{
		std::memset(v.dac, 0, sizeof(v.dac));
		for (int i = 0; i < 64; ++i)
		{
			const int red = ((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21;
			const int green = ((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21;
			const int blue = ((i >> 0) & 1) * 42 + ((i >> 3) & 1) * 21;
			v.dac[i][0] = uint8_t(red);
			v.dac[i][1] = uint8_t(green);
			v.dac[i][2] = uint8_t(blue);
		}
	}

	void default_attribute_palette(vga_state& v)
	{
		// The EGA colour each of the 16 attributes selects; 0x14 is the entry
		// that makes attribute 6 brown rather than dark yellow.
		static const uint8_t standard[16] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
			0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
		};
		for (int i = 0; i < 16; ++i) v.attr[i] = standard[i];
		v.attr[0x10] = 0x0c; // graphics mode, blink off by default
		v.attr[0x11] = 0x00;
		v.attr[0x12] = 0x0f;
		v.attr[0x13] = 0x00;
		v.attr[0x14] = 0x00;
	}

	uint32_t window_base(const vga_state& v, uint32_t address)
	{
		// The graphics controller's memory-map field decides which window the
		// adapter answers; in practice programs use A000 for graphics and B800
		// for text and CGA, which is what these three cases cover.
		if (address >= 0xb8000) return 0xb8000;
		if (address >= 0xb0000) return 0xb0000;
		(void)v;
		return 0xa0000;
	}

	uint8_t rotate_right(uint8_t value, int count)
	{
		count &= 7;
		return uint8_t((value >> count) | (value << (8 - count)));
	}

	uint8_t apply_operation(uint8_t source, uint8_t latch, int operation)
	{
		switch (operation)
		{
		case 1: return uint8_t(source & latch);
		case 2: return uint8_t(source | latch);
		case 3: return uint8_t(source ^ latch);
		default: return source;
		}
	}
}

void vga_reset(vga_state& v)
{
	std::memset(v.mem, 0, sizeof(v.mem));
	// The character generator has a 32-byte stride so any font height fits.
	std::memset(v.font, 0, sizeof(v.font));
	for (int ch = 0; ch < 256; ++ch)
		for (int row = 0; row < 8; ++row)
			v.font[ch * 32 + row] = rom_font_8x8[ch * 8 + row];
	ega_dac(v);
	default_attribute_palette(v);
	v.seq[2] = 0x0f;
	v.seq[4] = 0x02;
	v.gfx[8] = 0xff;
	v.crtc[0x13] = 40;
	v.dac_mask = 0xff;
	v.misc_output = 0x67;
}

void vga_set_mode(machine_state& s, uint8_t mode)
{
	vga_state& v = s.vga;
	const uint8_t requested = mode;
	const bool keep_memory = (mode & 0x80) != 0;
	mode &= 0x7f;

	v.mode = mode;
	v.page = 0;
	std::memset(v.cursor_x, 0, sizeof(v.cursor_x));
	std::memset(v.cursor_y, 0, sizeof(v.cursor_y));
	v.display_start = 0;
	v.cursor_visible = true;
	v.cursor_start = 6;
	v.cursor_end = 7;
	default_attribute_palette(v);
	if (mode == 0x13) default_dac(v);
	else ega_dac(v);
	v.seq[2] = 0x0f;
	v.gfx[8] = 0xff;
	v.gfx[5] = 0;
	v.gfx[3] = 0;
	v.gfx[1] = 0;

	switch (mode)
	{
	case 0x00: case 0x01:
		v.render_mode = vga_render_mode::text;
		v.mapping = vga_mapping::odd_even;
		v.text_columns = 40;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 320;
		v.height = 200;
		break;
	case 0x02: case 0x03: case 0x07:
		v.render_mode = vga_render_mode::text;
		v.mapping = vga_mapping::odd_even;
		v.text_columns = 80;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 640;
		v.height = 200;
		break;
	case 0x04: case 0x05:
		v.render_mode = vga_render_mode::cga4;
		v.mapping = vga_mapping::odd_even;
		v.text_columns = 40;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 320;
		v.height = 200;
		break;
	case 0x06:
		v.render_mode = vga_render_mode::cga2;
		v.mapping = vga_mapping::odd_even;
		v.text_columns = 80;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 640;
		v.height = 200;
		break;
	case 0x0d:
		v.render_mode = vga_render_mode::planar4;
		v.mapping = vga_mapping::planar;
		v.text_columns = 40;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 320;
		v.height = 200;
		v.crtc[0x13] = 20;
		break;
	case 0x0e:
		v.render_mode = vga_render_mode::planar4;
		v.mapping = vga_mapping::planar;
		v.text_columns = 80;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 640;
		v.height = 200;
		v.crtc[0x13] = 40;
		break;
	case 0x0f: case 0x10:
		v.render_mode = vga_render_mode::planar4;
		v.mapping = vga_mapping::planar;
		v.text_columns = 80;
		v.text_rows = 25;
		v.char_height = 14;
		v.width = 640;
		v.height = 350;
		v.crtc[0x13] = 40;
		break;
	case 0x11: case 0x12:
		v.render_mode = vga_render_mode::planar4;
		v.mapping = vga_mapping::planar;
		v.text_columns = 80;
		v.text_rows = 30;
		v.char_height = 16;
		v.width = 640;
		v.height = 480;
		v.crtc[0x13] = 40;
		break;
	case 0x13:
		v.render_mode = vga_render_mode::linear8;
		v.mapping = vga_mapping::chain4;
		v.text_columns = 40;
		v.text_rows = 25;
		v.char_height = 8;
		v.width = 320;
		v.height = 200;
		v.crtc[0x13] = 40;
		v.seq[4] = 0x0e;
		break;
	default:
		// Unknown mode: leave the adapter as it was but remember the number,
		// so a program probing for an SVGA mode does not blank the screen.
		v.mode = mode;
		return;
	}

	if (v.render_mode == vga_render_mode::text)
	{
		v.crtc[0x13] = uint8_t(v.text_columns);
		v.attr[0x10] = 0x08; // blink enabled, text mode
	}

	if (!keep_memory) std::memset(v.mem, 0, sizeof(v.mem));

	if (v.render_mode == vga_render_mode::text && !keep_memory)
	{
		for (int i = 0; i < v.text_columns * v.text_rows * 8; ++i)
		{
			v.mem[i * 4 + 0] = ' ';
			v.mem[i * 4 + 1] = 0x07;
		}
	}

	// BIOS data area video fields, which plenty of programs read directly.
	write8(s, bda_segment, 0x49, v.mode);
	write16(s, bda_segment, 0x4a, uint16_t(v.text_columns));
	write16(s, bda_segment, 0x4c, uint16_t(v.text_columns * v.text_rows * 2));
	write16(s, bda_segment, 0x4e, 0);
	write8(s, bda_segment, 0x62, 0);
	write16(s, bda_segment, 0x63, uint16_t(v.mode == 7 ? 0x3b4 : 0x3d4));
	write8(s, bda_segment, 0x84, uint8_t(v.text_rows - 1));
	write16(s, bda_segment, 0x85, uint16_t(v.char_height));
	write8(s, bda_segment, 0x87, uint8_t(keep_memory ? 0x80 : 0x00));
	(void)requested;
	v.dirty = true;
}

uint8_t vga_mem_read(machine_state& s, uint32_t address)
{
	vga_state& v = s.vga;
	const uint32_t offset = address - window_base(v, address);

	if (v.mapping == vga_mapping::chain4) return v.mem[offset & (vga_plane_size * 4 - 1)];
	if (v.mapping == vga_mapping::odd_even)
		return v.mem[(((offset >> 1) & (vga_plane_size - 1)) * 4) + (offset & 1)];

	const uint32_t plane_offset = offset & (vga_plane_size - 1);
	for (int p = 0; p < 4; ++p) v.latch[p] = v.mem[plane_offset * 4 + p];

	if ((v.gfx[5] & 0x08) != 0)
	{
		// Read mode 1: every bit reports whether that pixel matches the
		// colour compare register across the enabled planes.
		uint8_t result = 0xff;
		for (int p = 0; p < 4; ++p)
		{
			if (((v.gfx[7] >> p) & 1) == 0) continue;
			const uint8_t want = ((v.gfx[2] >> p) & 1) ? 0xff : 0x00;
			result = uint8_t(result & ~(v.latch[p] ^ want));
		}
		return result;
	}
	return v.latch[v.gfx[4] & 3];
}

void vga_mem_write(machine_state& s, uint32_t address, uint8_t value)
{
	vga_state& v = s.vga;
	const uint32_t offset = address - window_base(v, address);
	v.dirty = true;

	if (v.mapping == vga_mapping::chain4)
	{
		const uint32_t index = offset & (vga_plane_size * 4 - 1);
		if (((v.seq[2] >> (index & 3)) & 1) != 0 || v.seq[2] == 0x0f) v.mem[index] = value;
		return;
	}

	if (v.mapping == vga_mapping::odd_even)
	{
		const uint32_t plane = offset & 1;
		const uint32_t plane_offset = (offset >> 1) & (vga_plane_size - 1);
		v.mem[plane_offset * 4 + plane] = value;
		return;
	}

	const uint32_t plane_offset = offset & (vga_plane_size - 1);
	const int write_mode = v.gfx[5] & 3;
	const uint8_t map_mask = uint8_t(v.seq[2] & 0x0f);
	const uint8_t bit_mask = v.gfx[8];
	const int operation = (v.gfx[3] >> 3) & 3;
	const uint8_t rotated = rotate_right(value, v.gfx[3] & 7);

	for (int p = 0; p < 4; ++p)
	{
		if (((map_mask >> p) & 1) == 0) continue;

		uint8_t result;
		switch (write_mode)
		{
		case 1:
			result = v.latch[p];
			break;
		case 2:
		{
			const uint8_t src = ((value >> p) & 1) ? 0xff : 0x00;
			const uint8_t combined = apply_operation(src, v.latch[p], operation);
			result = uint8_t((combined & bit_mask) | (v.latch[p] & ~bit_mask));
			break;
		}
		case 3:
		{
			const uint8_t src = ((v.gfx[0] >> p) & 1) ? 0xff : 0x00;
			const uint8_t mask = uint8_t(bit_mask & rotated);
			result = uint8_t((src & mask) | (v.latch[p] & ~mask));
			break;
		}
		default:
		{
			const uint8_t src = ((v.gfx[1] >> p) & 1)
				                    ? (((v.gfx[0] >> p) & 1) ? 0xff : 0x00)
				                    : rotated;
			const uint8_t combined = apply_operation(src, v.latch[p], operation);
			result = uint8_t((combined & bit_mask) | (v.latch[p] & ~bit_mask));
			break;
		}
		}
		v.mem[plane_offset * 4 + p] = result;
	}
}

uint32_t vga_dac_colour(const vga_state& v, uint8_t index)
{
	const uint8_t* c = v.dac[index & v.dac_mask];
	const uint32_t r = uint32_t(c[0] * 255 / 63);
	const uint32_t g = uint32_t(c[1] * 255 / 63);
	const uint32_t b = uint32_t(c[2] * 255 / 63);
	return (r << 16) | (g << 8) | b;
}

namespace
{
	uint32_t attribute_colour(const vga_state& v, uint8_t index)
	{
		uint8_t dac_index = uint8_t(v.attr[index & 0x0f] & 0x3f);
		if ((v.attr[0x10] & 0x80) != 0) // P4/P5 come from the colour select register
			dac_index = uint8_t((dac_index & 0x0f) | ((v.attr[0x14] & 0x0f) << 4));
		else
			dac_index = uint8_t(dac_index | ((v.attr[0x14] & 0x0c) << 4));
		return vga_dac_colour(v, dac_index);
	}

	void render_text(machine_state& s)
	{
		vga_state& v = s.vga;
		const int columns = v.text_columns;
		const int rows = v.text_rows;
		const int cell_width = v.width / columns;
		const int cell_height = v.height / rows;
		const int font_height = v.char_height;
		const bool blink_enabled = (v.attr[0x10] & 0x08) != 0;
		const bool blink_on = (v.frame / 16) % 2 == 0;

		for (int row = 0; row < rows; ++row)
		{
			for (int column = 0; column < columns; ++column)
			{
				const uint32_t cell = v.display_start + uint32_t(row * columns + column);
				const uint8_t ch = v.mem[(cell & (vga_plane_size - 1)) * 4 + 0];
				const uint8_t attribute = v.mem[(cell & (vga_plane_size - 1)) * 4 + 1];
				const bool blinking = blink_enabled && (attribute & 0x80) != 0;
				const uint32_t foreground = attribute_colour(v, uint8_t(attribute & 0x0f));
				const uint32_t background = attribute_colour(
					v, uint8_t(blink_enabled ? ((attribute >> 4) & 0x07) : ((attribute >> 4) & 0x0f)));

				for (int y = 0; y < cell_height; ++y)
				{
					const int source_row = font_height == cell_height ? y : y * font_height / cell_height;
					uint8_t bits = v.font[ch * 32 + (source_row & 31)];
					if (blinking && !blink_on) bits = 0;
					uint32_t* out = v.framebuffer + (row * cell_height + y) * v.width + column * cell_width;
					for (int x = 0; x < cell_width; ++x)
					{
						const int bit = cell_width == 8 ? x : x * 8 / cell_width;
						out[x] = ((bits << bit) & 0x80) ? foreground : background;
					}
				}
			}
		}

		if (v.cursor_visible && (v.frame / 8) % 2 == 0 && v.cursor_start <= v.cursor_end)
		{
			const int column = v.cursor_x[v.page];
			const int row = v.cursor_y[v.page];
			if (column < columns && row < rows)
			{
				// A program that assumes a 16-row cell still gets a cursor.
				int start = v.cursor_start;
				int end = v.cursor_end;
				if (start >= cell_height) start = cell_height - 2;
				if (end >= cell_height) end = cell_height - 1;

				const uint32_t cell = v.display_start + uint32_t(row * columns + column);
				const uint8_t attribute = v.mem[(cell & (vga_plane_size - 1)) * 4 + 1];
				const uint32_t colour = attribute_colour(v, uint8_t(attribute & 0x0f));
				for (int y = start; y <= end && y < cell_height; ++y)
				{
					uint32_t* out = v.framebuffer + (row * cell_height + y) * v.width + column * cell_width;
					for (int x = 0; x < cell_width; ++x) out[x] = colour;
				}
			}
		}
	}

	void render_planar(machine_state& s)
	{
		vga_state& v = s.vga;
		const int stride = (v.crtc[0x13] != 0 ? v.crtc[0x13] : uint8_t(v.width / 16)) * 2;
		for (int y = 0; y < v.height; ++y)
		{
			uint32_t* out = v.framebuffer + y * v.width;
			const uint32_t row_offset = v.display_start + uint32_t(y * stride);
			for (int x = 0; x < v.width; x += 8)
			{
				const uint32_t offset = (row_offset + uint32_t(x / 8)) & (vga_plane_size - 1);
				const uint8_t p0 = v.mem[offset * 4 + 0];
				const uint8_t p1 = v.mem[offset * 4 + 1];
				const uint8_t p2 = v.mem[offset * 4 + 2];
				const uint8_t p3 = v.mem[offset * 4 + 3];
				for (int bit = 0; bit < 8; ++bit)
				{
					const int shift = 7 - bit;
					const uint8_t pixel = uint8_t(((p0 >> shift) & 1) | (((p1 >> shift) & 1) << 1) |
						(((p2 >> shift) & 1) << 2) | (((p3 >> shift) & 1) << 3));
					out[x + bit] = attribute_colour(v, pixel);
				}
			}
		}
	}

	void render_linear8(machine_state& s)
	{
		vga_state& v = s.vga;
		const int stride = (v.crtc[0x13] != 0 ? v.crtc[0x13] : 40) * 8;
		const uint32_t start = v.display_start * 4;
		for (int y = 0; y < v.height; ++y)
		{
			uint32_t* out = v.framebuffer + y * v.width;
			const uint32_t row_offset = start + uint32_t(y * stride);
			for (int x = 0; x < v.width; ++x)
				out[x] = vga_dac_colour(v, v.mem[(row_offset + uint32_t(x)) & (vga_plane_size * 4 - 1)]);
		}
	}

	void render_cga(machine_state& s)
	{
		vga_state& v = s.vga;
		const bool four_colour = v.render_mode == vga_render_mode::cga4;
		const int bytes_per_row = four_colour ? 80 : 80;

		// CGA colour set: bit 5 of the mode control picks the palette, bit 4
		// of the colour select register picks the bright version.
		const bool bright = (v.cga_color_select & 0x10) != 0;
		const int palette_choice = (v.cga_color_select & 0x20) != 0 ? 1 : 0;
		uint8_t palette[4];
		palette[0] = uint8_t(v.cga_color_select & 0x0f);
		if (palette_choice == 0)
		{
			palette[1] = uint8_t(bright ? 11 : 3);
			palette[2] = uint8_t(bright ? 13 : 5);
			palette[3] = uint8_t(bright ? 15 : 7);
		}
		else
		{
			palette[1] = uint8_t(bright ? 10 : 2);
			palette[2] = uint8_t(bright ? 12 : 4);
			palette[3] = uint8_t(bright ? 14 : 6);
		}

		for (int y = 0; y < v.height; ++y)
		{
			uint32_t* out = v.framebuffer + y * v.width;
			const uint32_t base = uint32_t((y & 1) * 0x2000 + (y >> 1) * bytes_per_row);
			if (four_colour)
			{
				for (int x = 0; x < v.width; ++x)
				{
					const uint32_t address = base + uint32_t(x >> 2);
					const uint8_t byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
					const uint8_t pixel = uint8_t((byte >> (6 - 2 * (x & 3))) & 3);
					out[x] = attribute_colour(v, palette[pixel]);
				}
			}
			else
			{
				for (int x = 0; x < v.width; ++x)
				{
					const uint32_t address = base + uint32_t(x >> 3);
					const uint8_t byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
					const uint8_t pixel = uint8_t((byte >> (7 - (x & 7))) & 1);
					out[x] = attribute_colour(v, uint8_t(pixel ? 15 : 0));
				}
			}
		}
	}
}

void vga_render(machine_state& s)
{
	vga_state& v = s.vga;
	++v.frame;
	if (!v.render_enabled) return;
	if (v.width <= 0 || v.height <= 0 || v.width > vga_max_width || v.height > vga_max_height) return;

	switch (v.render_mode)
	{
	case vga_render_mode::text: render_text(s); break;
	case vga_render_mode::planar4: render_planar(s); break;
	case vga_render_mode::linear8: render_linear8(s); break;
	default: render_cga(s); break;
	}
	v.dirty = false;
}

uint8_t vga_port_read(machine_state& s, uint16_t port)
{
	vga_state& v = s.vga;
	switch (port)
	{
	case 0x3c0: return v.attr_index;
	case 0x3c1: return v.attr[v.attr_index & 0x1f];
	case 0x3c2: return 0x10; // input status 0: colour display attached
	case 0x3c4: return v.seq_index;
	case 0x3c5: return v.seq[v.seq_index & 7];
	case 0x3c6: return v.dac_mask;
	case 0x3c7: return 0;
	case 0x3c8: return v.dac_write_index;
	case 0x3c9:
	{
		const uint8_t value = v.dac[v.dac_read_index][v.dac_channel];
		if (++v.dac_channel == 3)
		{
			v.dac_channel = 0;
			++v.dac_read_index;
		}
		return value;
	}
	case 0x3cc: return v.misc_output;
	case 0x3ce: return v.gfx_index;
	case 0x3cf: return v.gfx[v.gfx_index & 0x0f];
	case 0x3b4: case 0x3d4: return v.crtc_index;
	case 0x3b5: case 0x3d5: return v.crtc[v.crtc_index & 0x1f];
	case 0x3d8: return v.cga_mode_control;
	case 0x3d9: return v.cga_color_select;
	case 0x3ba: case 0x3da:
	{
		// Input status 1. Programs poll bit 3 for vertical retrace and bit 0
		// for any retrace, so both have to move with the machine clock.
		v.attr_flipflop = false;
		const uint32_t frame_cycles = s.cpu_hz / 70;
		const uint32_t position = uint32_t(s.cycles % (frame_cycles == 0 ? 1 : frame_cycles));
		const bool vertical = position > frame_cycles - frame_cycles / 20;
		const bool horizontal = (position % 128) < 24;
		return uint8_t((vertical ? 0x08 : 0) | (vertical || horizontal ? 0x01 : 0));
	}
	default: return 0xff;
	}
}

void vga_port_write(machine_state& s, uint16_t port, uint8_t value)
{
	vga_state& v = s.vga;
	v.dirty = true;
	switch (port)
	{
	case 0x3c0:
		if (!v.attr_flipflop) v.attr_index = uint8_t(value & 0x1f);
		else v.attr[v.attr_index & 0x1f] = value;
		v.attr_flipflop = !v.attr_flipflop;
		break;
	case 0x3c2: v.misc_output = value;
		break;
	case 0x3c4: v.seq_index = uint8_t(value & 7);
		break;
	case 0x3c5:
		v.seq[v.seq_index & 7] = value;
		if (v.seq_index == 4)
		{
			// Sequencer memory mode: chain-4 for mode 13h, odd/even for text,
			// planar for everything else.
			if ((value & 0x08) != 0) v.mapping = vga_mapping::chain4;
			else if ((value & 0x04) == 0) v.mapping = vga_mapping::odd_even;
			else v.mapping = vga_mapping::planar;

			if (v.mapping == vga_mapping::planar && v.render_mode == vga_render_mode::linear8)
			{
				// Unchained 256-colour ("mode X"): still linear per plane.
				v.mapping = vga_mapping::planar;
			}
		}
		break;
	case 0x3c6: v.dac_mask = value;
		break;
	case 0x3c7:
		v.dac_read_index = value;
		v.dac_channel = 0;
		break;
	case 0x3c8:
		v.dac_write_index = value;
		v.dac_channel = 0;
		break;
	case 0x3c9:
		v.dac[v.dac_write_index][v.dac_channel] = uint8_t(value & 0x3f);
		if (++v.dac_channel == 3)
		{
			v.dac_channel = 0;
			++v.dac_write_index;
		}
		break;
	case 0x3ce: v.gfx_index = uint8_t(value & 0x0f);
		break;
	case 0x3cf: v.gfx[v.gfx_index & 0x0f] = value;
		break;
	case 0x3b4: case 0x3d4: v.crtc_index = uint8_t(value & 0x1f);
		break;
	case 0x3b5: case 0x3d5:
		v.crtc[v.crtc_index & 0x1f] = value;
		if (v.crtc_index == 0x0c || v.crtc_index == 0x0d)
			v.display_start = uint32_t((v.crtc[0x0c] << 8) | v.crtc[0x0d]);
		if (v.crtc_index == 0x0a) v.cursor_start = uint8_t(value & 0x1f);
		if (v.crtc_index == 0x0b) v.cursor_end = uint8_t(value & 0x1f);
		if (v.crtc_index == 0x0e || v.crtc_index == 0x0f)
		{
			const uint16_t position = uint16_t((v.crtc[0x0e] << 8) | v.crtc[0x0f]);
			if (v.text_columns > 0)
			{
				v.cursor_x[v.page] = uint8_t(position % v.text_columns);
				v.cursor_y[v.page] = uint8_t(position / v.text_columns);
			}
		}
		break;
	case 0x3d8: v.cga_mode_control = value;
		break;
	case 0x3d9: v.cga_color_select = value;
		break;
	default: break;
	}
}

void vga_set_pixel(machine_state& s, int x, int y, uint8_t colour)
{
	vga_state& v = s.vga;
	if (x < 0 || y < 0 || x >= v.width || y >= v.height) return;

	switch (v.render_mode)
	{
	case vga_render_mode::linear8:
		v.mem[(uint32_t(y) * uint32_t(v.width) + uint32_t(x)) & (vga_plane_size * 4 - 1)] = colour;
		break;
	case vga_render_mode::planar4:
	{
		const int stride = (v.crtc[0x13] != 0 ? v.crtc[0x13] : 40) * 2;
		const uint32_t offset = (uint32_t(y) * uint32_t(stride) + uint32_t(x / 8)) & (vga_plane_size - 1);
		const uint8_t mask = uint8_t(0x80 >> (x & 7));
		for (int p = 0; p < 4; ++p)
		{
			uint8_t& byte = v.mem[offset * 4 + p];
			byte = uint8_t(((colour >> p) & 1) ? (byte | mask) : (byte & ~mask));
		}
		break;
	}
	case vga_render_mode::cga4:
	{
		const uint32_t address = uint32_t((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2));
		uint8_t& byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
		const int shift = 6 - 2 * (x & 3);
		byte = uint8_t((byte & ~(3 << shift)) | ((colour & 3) << shift));
		break;
	}
	case vga_render_mode::cga2:
	{
		const uint32_t address = uint32_t((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3));
		uint8_t& byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
		const uint8_t mask = uint8_t(0x80 >> (x & 7));
		byte = uint8_t(colour ? (byte | mask) : (byte & ~mask));
		break;
	}
	default: break;
	}
	v.dirty = true;
}

uint8_t vga_get_pixel(machine_state& s, int x, int y)
{
	vga_state& v = s.vga;
	if (x < 0 || y < 0 || x >= v.width || y >= v.height) return 0;

	switch (v.render_mode)
	{
	case vga_render_mode::linear8:
		return v.mem[(uint32_t(y) * uint32_t(v.width) + uint32_t(x)) & (vga_plane_size * 4 - 1)];
	case vga_render_mode::planar4:
	{
		const int stride = (v.crtc[0x13] != 0 ? v.crtc[0x13] : 40) * 2;
		const uint32_t offset = (uint32_t(y) * uint32_t(stride) + uint32_t(x / 8)) & (vga_plane_size - 1);
		const uint8_t mask = uint8_t(0x80 >> (x & 7));
		uint8_t colour = 0;
		for (int p = 0; p < 4; ++p) if ((v.mem[offset * 4 + p] & mask) != 0) colour = uint8_t(colour | (1 << p));
		return colour;
	}
	case vga_render_mode::cga4:
	{
		const uint32_t address = uint32_t((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2));
		const uint8_t byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
		return uint8_t((byte >> (6 - 2 * (x & 3))) & 3);
	}
	case vga_render_mode::cga2:
	{
		const uint32_t address = uint32_t((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3));
		const uint8_t byte = v.mem[(((address >> 1) & (vga_plane_size - 1)) * 4) + (address & 1)];
		return uint8_t((byte >> (7 - (x & 7))) & 1);
	}
	default: return 0;
	}
}

void vga_scroll_up(machine_state& s, int lines, uint8_t attribute, int left, int top, int right, int bottom)
{
	vga_state& v = s.vga;
	if (right >= v.text_columns) right = v.text_columns - 1;
	if (bottom >= v.text_rows) bottom = v.text_rows - 1;
	if (left > right || top > bottom) return;

	const uint32_t page_base = v.display_start;
	const int columns = v.text_columns;

	if (lines <= 0 || lines > bottom - top)
	{
		for (int y = top; y <= bottom; ++y)
			for (int x = left; x <= right; ++x)
			{
				const uint32_t cell = (page_base + uint32_t(y * columns + x)) & (vga_plane_size - 1);
				v.mem[cell * 4 + 0] = ' ';
				v.mem[cell * 4 + 1] = attribute;
			}
		v.dirty = true;
		return;
	}

	for (int y = top; y <= bottom - lines; ++y)
		for (int x = left; x <= right; ++x)
		{
			const uint32_t to = (page_base + uint32_t(y * columns + x)) & (vga_plane_size - 1);
			const uint32_t from = (page_base + uint32_t((y + lines) * columns + x)) & (vga_plane_size - 1);
			v.mem[to * 4 + 0] = v.mem[from * 4 + 0];
			v.mem[to * 4 + 1] = v.mem[from * 4 + 1];
		}
	for (int y = bottom - lines + 1; y <= bottom; ++y)
		for (int x = left; x <= right; ++x)
		{
			const uint32_t cell = (page_base + uint32_t(y * columns + x)) & (vga_plane_size - 1);
			v.mem[cell * 4 + 0] = ' ';
			v.mem[cell * 4 + 1] = attribute;
		}
	v.dirty = true;
}

void vga_write_char(machine_state& s, uint8_t page, uint8_t ch, uint8_t attribute, bool use_attribute)
{
	vga_state& v = s.vga;
	if (page >= 8) page = 0;
	int x = v.cursor_x[page];
	int y = v.cursor_y[page];

	switch (ch)
	{
	case '\r':
		x = 0;
		break;
	case '\n':
		++y;
		break;
	case '\b':
		if (x > 0) --x;
		break;
	case 7:
		break;
	default:
		if (v.render_mode == vga_render_mode::text)
		{
			const uint32_t cell = (v.display_start + uint32_t(y * v.text_columns + x)) & (vga_plane_size - 1);
			v.mem[cell * 4 + 0] = ch;
			if (use_attribute) v.mem[cell * 4 + 1] = attribute;
		}
		else
		{
			// Graphics mode: draw the glyph from the character generator.
			const int height = v.char_height;
			for (int row = 0; row < 8; ++row)
			{
				const uint8_t bits = v.font[ch * 32 + row];
				for (int bit = 0; bit < 8; ++bit)
				{
					const bool on = ((bits << bit) & 0x80) != 0;
					if (on) vga_set_pixel(s, x * 8 + bit, y * height + row, uint8_t(attribute & 0x0f));
					else if ((attribute & 0x80) == 0) vga_set_pixel(s, x * 8 + bit, y * height + row, 0);
				}
			}
		}
		++x;
		break;
	}

	if (x >= v.text_columns)
	{
		x = 0;
		++y;
	}
	if (y >= v.text_rows)
	{
		y = v.text_rows - 1;
		vga_scroll_up(s, 1, use_attribute ? attribute : 0x07, 0, 0, v.text_columns - 1, v.text_rows - 1);
	}
	v.cursor_x[page] = uint8_t(x);
	v.cursor_y[page] = uint8_t(y);
	v.dirty = true;
}
