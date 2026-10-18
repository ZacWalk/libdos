// libdos by Zac Walker
//
// PC BIOS services. Everything a DOS program can reach through INT 10h-1Ah,
// plus the INT 33h mouse driver. State that programs are known to read behind
// the BIOS's back - the keyboard buffer, the tick count, the video fields -
// lives in the BIOS data area in emulated memory rather than in C++ fields.

#include "machine.h"

#include <cstring>

namespace
{
	// Scancode set 1 to ASCII. Index is the make code; 0 means the key has no
	// character and INT 16h reports it as an extended key.
	const uint8_t ascii_plain[0x59] = {
		0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
		'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
		'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
		'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
		'2', '3', '0', '.', 0, 0, 0, 0, 0,
	};

	const uint8_t ascii_shifted[0x59] = {
		0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 0,
		'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
		'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
		'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
		'2', '3', '0', '.', 0, 0, 0, 0, 0,
	};

	uint16_t buffer_head(machine_state& s) { return read16(s, bda_segment, bda_keyboard_head); }
	uint16_t buffer_tail(machine_state& s) { return read16(s, bda_segment, bda_keyboard_tail); }

	uint16_t next_slot(uint16_t offset)
	{
		offset = uint16_t(offset + 2);
		if (offset >= bda_keyboard_buffer_end) offset = bda_keyboard_buffer;
		return offset;
	}

	void set_carry(machine_state& s, bool on) { cpu_set_iret_flag(s, flag_cf, on); }

	void set_zero(machine_state& s, bool on) { cpu_set_iret_flag(s, flag_zf, on); }
}

void bios_reset(machine_state& s)
{
	s.bios = bios_state();

	std::memset(s.memory + 0x400, 0, 0x100);
	write16(s, bda_segment, 0x00, 0x03f8); // COM1
	write16(s, bda_segment, 0x08, 0x0378); // LPT1
	write16(s, bda_segment, 0x10, s.bios.equipment);
	write16(s, bda_segment, 0x13, s.bios.memory_kb);
	write16(s, bda_segment, bda_keyboard_head, bda_keyboard_buffer);
	write16(s, bda_segment, bda_keyboard_tail, bda_keyboard_buffer);
	write16(s, bda_segment, 0x80, bda_keyboard_buffer);
	write16(s, bda_segment, 0x82, bda_keyboard_buffer_end);
	write8(s, bda_segment, 0x96, 0x10); // enhanced keyboard present
}

void bios_push_key(machine_state& s, uint8_t scancode, uint8_t ascii)
{
	const uint16_t tail = buffer_tail(s);
	const uint16_t next = next_slot(tail);
	if (next == buffer_head(s)) return; // buffer full, the BIOS just beeps
	write16(s, bda_segment, tail, uint16_t((scancode << 8) | ascii));
	write16(s, bda_segment, bda_keyboard_tail, next);
}

bool bios_peek_key(machine_state& s, uint16_t& key)
{
	const uint16_t head = buffer_head(s);
	if (head == buffer_tail(s)) return false;
	key = read16(s, bda_segment, head);
	return true;
}

bool bios_pop_key(machine_state& s, uint16_t& key)
{
	const uint16_t head = buffer_head(s);
	if (head == buffer_tail(s)) return false;
	key = read16(s, bda_segment, head);
	write16(s, bda_segment, bda_keyboard_head, next_slot(head));
	return true;
}

void bios_retry_interrupt(machine_state& s)
{
	// The stub was entered by an INT n instruction, so backing the return
	// address up by its two bytes makes the program ask again.
	const cpu_state& c = s.cpu;
	const uint32_t address = ((uint32_t(c.ss()) << 4) + c.sp()) & 0xfffff;
	mem_write16(s, address, uint16_t(mem_read16(s, address) - 2));
}

void bios_keyboard_interrupt(machine_state& s)
{
	const uint8_t code = kbd_read(s, 0x60);
	const bool release = (code & 0x80) != 0;
	const uint8_t make = uint8_t(code & 0x7f);
	uint8_t flags = read8(s, bda_segment, bda_keyboard_flags);

	switch (make)
	{
	case 0x2a: flags = uint8_t(release ? (flags & ~0x02) : (flags | 0x02));
		break;
	case 0x36: flags = uint8_t(release ? (flags & ~0x01) : (flags | 0x01));
		break;
	case 0x1d: flags = uint8_t(release ? (flags & ~0x04) : (flags | 0x04));
		break;
	case 0x38: flags = uint8_t(release ? (flags & ~0x08) : (flags | 0x08));
		break;
	case 0x3a: if (!release) flags = uint8_t(flags ^ 0x40);
		break;
	case 0x45: if (!release) flags = uint8_t(flags ^ 0x20);
		break;
	case 0x46: if (!release) flags = uint8_t(flags ^ 0x10);
		break;
	default: break;
	}
	write8(s, bda_segment, bda_keyboard_flags, flags);

	// Shift, Ctrl, Alt and the three locks only change the flags. A BIOS never
	// puts them in the buffer, where a program would read them as keys.
	switch (make)
	{
	case 0x1d: case 0x2a: case 0x36: case 0x38: case 0x3a: case 0x45: case 0x46: return;
	default: break;
	}
	if (release || make >= 0x59) return;

	const bool shift = (flags & 0x03) != 0;
	const bool control = (flags & 0x04) != 0;
	const bool alt = (flags & 0x08) != 0;
	const bool caps = (flags & 0x40) != 0;
	const bool num_lock = (flags & 0x20) != 0;

	uint8_t scancode = make;
	uint8_t ascii = shift ? ascii_shifted[make] : ascii_plain[make];
	// The cursor pad shares its scancodes with the numeric keypad; without
	// NumLock those keys are extended keys with no character, Shift or not, so
	// Shift+Left stays an arrow. The keypad's minus and plus have no second
	// meaning and always type.
	if (!num_lock && make >= 0x47 && make <= 0x53 && make != 0x4a && make != 0x4e) ascii = 0;
	if (caps && ascii >= 'a' && ascii <= 'z') ascii = uint8_t(ascii - 32);
	else if (caps && shift && ascii >= 'A' && ascii <= 'Z') ascii = uint8_t(ascii + 32);

	// With a modifier held, a function key, and the cursor pad under Ctrl,
	// report codes of their own rather than the key's scancode: that is how a
	// program tells Shift+F1 from F1, or Ctrl+Left from Left.
	if (make >= 0x3b && make <= 0x44)
	{
		const int index = make - 0x3b;
		scancode = uint8_t(alt ? 0x68 + index : control ? 0x5e + index : shift ? 0x54 + index : make);
		ascii = 0;
	}
	else if (make == 0x57 || make == 0x58)
	{
		scancode = uint8_t((alt ? 0x8b : control ? 0x89 : shift ? 0x87 : 0x85) + make - 0x57);
		ascii = 0;
	}
	else if (alt)
	{
		if (make >= 0x02 && make <= 0x0d) scancode = uint8_t(make + 0x76); // Alt+1 is 7800h
		ascii = 0;
	}
	else if (control)
	{
		static const uint8_t control_pad[13] = {
			0x77, 0x8d, 0x84, 0x8e, 0x73, 0x8f, 0x74, 0x90, 0x75, 0x91, 0x76, 0x92, 0x93,
		};
		if (make >= 0x47 && make <= 0x53)
		{
			scancode = control_pad[make - 0x47];
			ascii = 0;
		}
		else if (ascii >= 'a' && ascii <= 'z') ascii = uint8_t(ascii - 'a' + 1);
		else if (ascii >= 'A' && ascii <= 'Z') ascii = uint8_t(ascii - 'A' + 1);
		else
		{
			switch (make)
			{
			case 0x01: ascii = 0x1b; break; // Esc
			case 0x07: ascii = 0x1e; break; // 6
			case 0x0c: ascii = 0x1f; break; // -
			case 0x0e: ascii = 0x7f; break; // Backspace
			case 0x0f: scancode = 0x94; ascii = 0; break; // Tab
			case 0x1a: ascii = 0x1b; break; // [
			case 0x1b: ascii = 0x1d; break; // ]
			case 0x1c: ascii = 0x0a; break; // Enter
			case 0x2b: ascii = 0x1c; break; // backslash
			case 0x37: scancode = 0x96; ascii = 0; break; // keypad *
			case 0x39: ascii = 0x20; break; // Space
			default: ascii = 0; break;
			}
		}
	}
	bios_push_key(s, scancode, ascii);
}

namespace
{
	void video_interrupt(machine_state& s)
	{
		cpu_state& c = s.cpu;
		vga_state& v = s.vga;

		switch (c.ah())
		{
		case 0x00:
			vga_set_mode(s, c.al());
			break;

		case 0x01:
			v.cursor_start = uint8_t(c.ch() & 0x1f);
			v.cursor_end = uint8_t(c.cl() & 0x1f);
			v.cursor_visible = (c.ch() & 0x20) == 0;
			break;

		case 0x02:
		{
			const uint8_t page = c.bh() < 8 ? c.bh() : 0;
			v.cursor_x[page] = c.dl();
			v.cursor_y[page] = c.dh();
			break;
		}

		case 0x03:
		{
			const uint8_t page = c.bh() < 8 ? c.bh() : 0;
			c.set_ch(v.cursor_start);
			c.set_cl(v.cursor_end);
			c.set_dl(v.cursor_x[page]);
			c.set_dh(v.cursor_y[page]);
			break;
		}

		case 0x05:
			v.page = uint8_t(c.al() & 7);
			v.display_start = uint32_t(v.page * v.text_columns * v.text_rows);
			write16(s, bda_segment, 0x4e, uint16_t(v.page * v.text_columns * v.text_rows * 2));
			write8(s, bda_segment, 0x62, v.page);
			break;

		case 0x06:
			vga_scroll_up(s, c.al(), c.bh(), c.cl(), c.ch(), c.dl(), c.dh());
			break;

		case 0x07:
			// Scroll down: modelled as a clear when a whole window is asked
			// for, which is what programs use it for.
			vga_scroll_up(s, c.al() == 0 ? 0 : -1, c.bh(), c.cl(), c.ch(), c.dl(), c.dh());
			break;

		case 0x08:
		{
			const uint8_t page = c.bh() < 8 ? c.bh() : 0;
			const uint32_t cell = (v.display_start + uint32_t(v.cursor_y[page] * v.text_columns + v.cursor_x[page])) &
				(vga_plane_size - 1);
			c.set_al(v.mem[cell * 4 + 0]);
			c.set_ah(v.mem[cell * 4 + 1]);
			break;
		}

		case 0x09:
		case 0x0a:
		{
			const uint8_t page = c.bh() < 8 ? c.bh() : 0;
			const uint8_t x = v.cursor_x[page];
			const uint8_t y = v.cursor_y[page];
			for (uint16_t i = 0; i < (c.cx() == 0 ? 1 : c.cx()); ++i)
				vga_write_char(s, page, c.al(), c.bl(), c.ah() == 0x09);
			v.cursor_x[page] = x;
			v.cursor_y[page] = y;
			break;
		}

		case 0x0b:
			if (c.bh() == 0) v.attr[0] = uint8_t(c.bl() & 0x0f);
			else v.cga_color_select = uint8_t((v.cga_color_select & ~0x20) | ((c.bl() & 1) << 5));
			break;

		case 0x0c:
			vga_set_pixel(s, int(c.cx()), int(c.dx()), c.al());
			break;

		case 0x0d:
			c.set_al(vga_get_pixel(s, int(c.cx()), int(c.dx())));
			break;

		case 0x0e:
			vga_write_char(s, v.page, c.al(), c.bl(), v.render_mode != vga_render_mode::text);
			break;

		case 0x0f:
			c.set_al(v.mode);
			c.set_ah(uint8_t(v.text_columns));
			c.set_bh(v.page);
			break;

		case 0x10:
			switch (c.al())
			{
			case 0x00: v.attr[c.bl() & 0x1f] = c.bh();
				break;
			case 0x01: v.attr[0x11] = c.bh();
				break;
			case 0x02:
				for (int i = 0; i < 17; ++i) v.attr[i] = read8(s, c.es(), uint16_t(c.dx() + i));
				break;
			case 0x03: v.attr[0x10] = uint8_t((v.attr[0x10] & ~0x08) | ((c.bl() & 1) << 3));
				break;
			case 0x07: c.set_bh(v.attr[c.bl() & 0x1f]);
				break;
			case 0x08: c.set_bh(v.attr[0x11]);
				break;
			case 0x10:
				v.dac[c.bx() & 0xff][0] = uint8_t(c.dh() & 0x3f);
				v.dac[c.bx() & 0xff][1] = uint8_t(c.ch() & 0x3f);
				v.dac[c.bx() & 0xff][2] = uint8_t(c.cl() & 0x3f);
				v.dirty = true;
				break;
			case 0x12:
				for (uint16_t i = 0; i < c.cx(); ++i)
				{
					const uint16_t entry = uint16_t(c.bx() + i);
					if (entry > 255) break;
					for (int channel = 0; channel < 3; ++channel)
						v.dac[entry][channel] = uint8_t(read8(s, c.es(), uint16_t(c.dx() + i * 3 + channel)) & 0x3f);
				}
				v.dirty = true;
				break;
			case 0x15:
				c.set_dh(v.dac[c.bx() & 0xff][0]);
				c.set_ch(v.dac[c.bx() & 0xff][1]);
				c.set_cl(v.dac[c.bx() & 0xff][2]);
				break;
			case 0x17:
				for (uint16_t i = 0; i < c.cx(); ++i)
				{
					const uint16_t entry = uint16_t(c.bx() + i);
					if (entry > 255) break;
					for (int channel = 0; channel < 3; ++channel)
						write8(s, c.es(), uint16_t(c.dx() + i * 3 + channel), v.dac[entry][channel]);
				}
				break;
			default: break;
			}
			break;

		case 0x11:
			switch (c.al())
			{
			case 0x00: case 0x10: // load user font
				for (uint16_t i = 0; i < c.cx(); ++i)
				{
					const int ch = (c.dx() + i) & 0xff;
					for (int row = 0; row < c.bh() && row < 32; ++row)
						v.font[ch * 32 + row] = read8(s, c.es(), uint16_t(c.bp() + i * c.bh() + row));
				}
				break;
			case 0x01: case 0x11: case 0x04: case 0x14: // ROM 8x14 / 8x16
			case 0x02: case 0x12: // ROM 8x8
				for (int ch = 0; ch < 256; ++ch)
					for (int row = 0; row < 8; ++row)
						v.font[ch * 32 + row] = rom_font_8x8[ch * 8 + row];
				v.char_height = 8;
				break;
			case 0x30:
				c.set_es(callback_segment);
				c.set_bp(0);
				c.set_cx(uint16_t(v.char_height));
				c.set_dl(uint8_t(v.text_rows - 1));
				break;
			default: break;
			}
			break;

		case 0x12:
			switch (c.bl())
			{
			case 0x10:
				c.set_bh(0); // colour display
				c.set_bl(3); // 256 KiB of display memory
				c.set_cx(0x0009);
				break;
			case 0x30:
				c.set_al(0x12);
				break;
			case 0x31: case 0x32: case 0x33: case 0x34:
				c.set_al(0x12);
				break;
			default:
				c.set_al(0);
				break;
			}
			break;

		case 0x13:
		{
			const uint8_t page = c.bh() < 8 ? c.bh() : 0;
			const uint8_t saved_x = v.cursor_x[page];
			const uint8_t saved_y = v.cursor_y[page];
			v.cursor_x[page] = c.dl();
			v.cursor_y[page] = c.dh();
			uint8_t attribute = c.bl();
			for (uint16_t i = 0; i < c.cx(); ++i)
			{
				uint8_t ch;
				if ((c.al() & 2) != 0)
				{
					ch = read8(s, c.es(), uint16_t(c.bp() + i * 2));
					attribute = read8(s, c.es(), uint16_t(c.bp() + i * 2 + 1));
				}
				else
				{
					ch = read8(s, c.es(), uint16_t(c.bp() + i));
				}
				vga_write_char(s, page, ch, attribute, true);
			}
			if ((c.al() & 1) == 0)
			{
				v.cursor_x[page] = saved_x;
				v.cursor_y[page] = saved_y;
			}
			break;
		}

		case 0x1a:
			c.set_al(0x1a);
			c.set_bl(0x08); // VGA with colour display
			c.set_bh(0x00);
			break;

		case 0x1b:
			// Functionality/state information: report a VGA with the current
			// mode, which is all any caller looks at.
			write8(s, c.es(), uint16_t(c.di() + 0x04), v.mode);
			write16(s, c.es(), uint16_t(c.di() + 0x05), uint16_t(v.text_columns));
			write8(s, c.es(), uint16_t(c.di() + 0x22), uint8_t(v.text_rows));
			write16(s, c.es(), uint16_t(c.di() + 0x23), uint16_t(v.char_height));
			c.set_al(0x1b);
			break;

		case 0x4f: // VESA: not provided
			c.set_ax(0x0100);
			break;

		default:
			break;
		}
	}

	void mouse_interrupt(machine_state& s)
	{
		cpu_state& c = s.cpu;
		mouse_state& m = s.bios.mouse;

		switch (c.ax())
		{
		case 0x0000:
			m = mouse_state();
			m.max_x = s.vga.width == 320 ? 639 : s.vga.width - 1;
			m.max_y = s.vga.height - 1;
			m.x = m.max_x / 2;
			m.y = m.max_y / 2;
			c.set_ax(0xffff);
			c.set_bx(2);
			break;
		case 0x0001: if (m.visible < 0) ++m.visible;
			break;
		case 0x0002: --m.visible;
			break;
		case 0x0003:
			c.set_bx(uint16_t(m.buttons));
			c.set_cx(uint16_t(m.x));
			c.set_dx(uint16_t(m.y));
			break;
		case 0x0004:
			m.x = c.cx();
			m.y = c.dx();
			break;
		case 0x0005:
		{
			const int button = c.bx() & 3;
			c.set_ax(uint16_t(m.buttons));
			c.set_bx(uint16_t(m.press_count[button > 2 ? 0 : button]));
			c.set_cx(uint16_t(m.press_x[button > 2 ? 0 : button]));
			c.set_dx(uint16_t(m.press_y[button > 2 ? 0 : button]));
			m.press_count[button > 2 ? 0 : button] = 0;
			break;
		}
		case 0x0006:
		{
			const int button = c.bx() & 3;
			c.set_ax(uint16_t(m.buttons));
			c.set_bx(uint16_t(m.release_count[button > 2 ? 0 : button]));
			c.set_cx(uint16_t(m.release_x[button > 2 ? 0 : button]));
			c.set_dx(uint16_t(m.release_y[button > 2 ? 0 : button]));
			m.release_count[button > 2 ? 0 : button] = 0;
			break;
		}
		case 0x0007:
			m.min_x = int16_t(c.cx());
			m.max_x = int16_t(c.dx());
			break;
		case 0x0008:
			m.min_y = int16_t(c.cx());
			m.max_y = int16_t(c.dx());
			break;
		case 0x000b:
			c.set_cx(uint16_t(int16_t(m.delta_x)));
			c.set_dx(uint16_t(int16_t(m.delta_y)));
			m.delta_x = 0;
			m.delta_y = 0;
			break;
		case 0x000c:
			m.callback_mask = c.cx();
			m.callback = (uint32_t(c.es()) << 16) | c.dx();
			break;
		case 0x000f:
			m.granularity_x = c.cx() == 0 ? 8 : c.cx();
			m.granularity_y = c.dx() == 0 ? 16 : c.dx();
			break;
		case 0x0014:
			c.set_cx(m.callback_mask);
			c.set_dx(uint16_t(m.callback));
			c.set_es(uint16_t(m.callback >> 16));
			break;
		case 0x0021:
			c.set_ax(0xffff);
			c.set_bx(2);
			break;
		case 0x0024:
			c.set_ax(0x0800); // driver version 8.00
			c.set_bx(0x0400); // PS/2 style
			c.set_cx(0x0000);
			break;
		default:
			break;
		}
	}
}

bool bios_interrupt(machine_state& s, uint8_t vector)
{
	cpu_state& c = s.cpu;

	switch (vector)
	{
	case 0x08:
	{
		// The timer tick. The stub that calls us chains on to INT 1Ch and only
		// then acknowledges the interrupt, as the real BIOS handler does.
		const uint32_t ticks = mem_read16(s, 0x46c) | (uint32_t(mem_read16(s, 0x46e)) << 16);
		const uint32_t next = ticks + 1;
		mem_write16(s, 0x46c, uint16_t(next));
		mem_write16(s, 0x46e, uint16_t(next >> 16));
		s.bios.ticks = next;
		return true;
	}

	case 0x09:
		bios_keyboard_interrupt(s);
		pic_write(s, 0x20, 0x20);
		return true;

	case 0x10:
		video_interrupt(s);
		return true;

	case 0x11:
		c.set_ax(s.bios.equipment);
		return true;

	case 0x12:
		c.set_ax(s.bios.memory_kb);
		return true;

	case 0x13:
		// No disk hardware. Reset succeeds, everything else reports "invalid
		// command" so that copy-protection code fails cleanly.
		if (c.ah() == 0x00)
		{
			c.set_ah(0);
			set_carry(s, false);
		}
		else
		{
			c.set_ah(0x01);
			set_carry(s, true);
		}
		return true;

	case 0x15:
		switch (c.ah())
		{
		case 0x86: // wait, in microseconds
			set_carry(s, false);
			return true;
		case 0x88: // extended memory size
			c.set_ax(0);
			set_carry(s, false);
			return true;
		case 0xc0: // system configuration
			c.set_ah(0x86);
			set_carry(s, true);
			return true;
		default:
			c.set_ah(0x86);
			set_carry(s, true);
			return true;
		}

	case 0x16:
		switch (c.ah())
		{
		case 0x00: case 0x10:
		{
			uint16_t key = 0;
			if (!bios_pop_key(s, key))
			{
				bios_retry_interrupt(s);
				return true;
			}
			c.set_ax(key);
			return true;
		}
		case 0x01: case 0x11:
		{
			uint16_t key = 0;
			if (!bios_peek_key(s, key))
			{
				set_zero(s, true);
				c.set_ax(0);
				return true;
			}
			set_zero(s, false);
			c.set_ax(key);
			return true;
		}
		case 0x02: case 0x12:
			c.set_al(read8(s, bda_segment, bda_keyboard_flags));
			if (c.ah() == 0x12) c.set_ah(read8(s, bda_segment, 0x18));
			return true;
		case 0x05:
			bios_push_key(s, c.ch(), c.cl());
			c.set_al(0);
			return true;
		case 0x03:
			return true;
		default:
			return true;
		}

	case 0x1a:
		switch (c.ah())
		{
		case 0x00:
		{
			const uint32_t ticks = mem_read16(s, 0x46c) | (uint32_t(mem_read16(s, 0x46e)) << 16);
			c.set_cx(uint16_t(ticks >> 16));
			c.set_dx(uint16_t(ticks));
			c.set_al(read8(s, bda_segment, 0x70));
			write8(s, bda_segment, 0x70, 0);
			return true;
		}
		case 0x01:
			mem_write16(s, 0x46c, c.dx());
			mem_write16(s, 0x46e, c.cx());
			return true;
		case 0x02:
		case 0x04:
		{
			int year = 1990, month = 1, day = 1, weekday = 1, hour = 0, minute = 0, second = 0, hundredths = 0;
			if (s.host) s.host->clock(year, month, day, weekday, hour, minute, second, hundredths);
			const auto bcd = [](int value) { return uint8_t(((value / 10) << 4) | (value % 10)); };
			if (c.ah() == 0x02)
			{
				c.set_ch(bcd(hour));
				c.set_cl(bcd(minute));
				c.set_dh(bcd(second));
				c.set_dl(0);
			}
			else
			{
				c.set_ch(bcd(year / 100));
				c.set_cl(bcd(year % 100));
				c.set_dh(bcd(month));
				c.set_dl(bcd(day));
			}
			set_carry(s, false);
			return true;
		}
		default:
			set_carry(s, true);
			return true;
		}

	case 0x33:
		mouse_interrupt(s);
		return true;

	default:
		return false;
	}
}
