// ovrwin by Zac Walker
//
// The machine: memory, the I/O port map, the interrupt stubs that make the
// emulator's DOS and BIOS reachable through the vector table, and the exec
// loop a host drives.

#include "machine.h"

#include <cstdio>
#include <cstring>

namespace
{
	// Every vector the emulator services points at a stub in the callback
	// page. A program can therefore chain to "the previous handler" and still
	// reach us, and can equally take a vector over completely.
	constexpr int stub_stride = 8;
	constexpr uint8_t callback_eoi = 0xff;

	// Host key to XT scancode. Index is machine_key.
	const uint8_t scancode_table[] = {
		0x00, 0x01, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
		0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
		0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x2b,
		0x3a, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x1c,
		0x2a, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
		0x1d, 0x38, 0x39, 0x38, 0x1d,
		0x52, 0x53, 0x47, 0x4f, 0x49, 0x51, 0x48, 0x50, 0x4b, 0x4d,
		0x45, 0x35, 0x37, 0x4a, 0x4e, 0x1c, 0x53,
		0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49,
		0x37, 0x46, 0x00,
	};

	uint8_t key_scancode(machine_key key)
	{
		const size_t index = size_t(key);
		if (index >= sizeof(scancode_table)) return 0;
		return scancode_table[index];
	}

	void draw_mouse_pointer(machine_state& s)
	{
		const mouse_state& m = s.bios.mouse;
		if (!m.present || m.visible < 0) return;
		vga_state& v = s.vga;
		if (v.width <= 0 || v.height <= 0) return;

		// A plain arrow, one bit per pixel, drawn over the finished frame.
		static const uint16_t arrow[16] = {
			0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00,
			0x7f80, 0x7c00, 0x6c00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
		};

		const int span_x = m.max_x - m.min_x + 1;
		const int span_y = m.max_y - m.min_y + 1;
		const int x0 = span_x > 0 ? (m.x - m.min_x) * v.width / span_x : 0;
		const int y0 = span_y > 0 ? (m.y - m.min_y) * v.height / span_y : 0;

		for (int y = 0; y < 16; ++y)
		{
			for (int x = 0; x < 9; ++x)
			{
				if ((arrow[y] & (0x8000 >> x)) == 0) continue;
				const int px = x0 + x;
				const int py = y0 + y;
				if (px < 0 || py < 0 || px >= v.width || py >= v.height) continue;
				uint32_t& pixel = v.framebuffer[py * v.width + px];
				pixel = pixel > 0x808080 ? 0x000000 : 0xffffff;
			}
		}
	}
}

uint32_t machine_stub_address(uint8_t vector)
{
	return (uint32_t(callback_segment) << 4) + callback_offset + uint32_t(vector) * stub_stride;
}

void machine_install_vector(machine_state& s, uint8_t vector)
{
	const uint16_t offset = uint16_t(callback_offset + uint32_t(vector) * stub_stride);
	uint8_t* stub = s.memory + machine_stub_address(vector);

	int at = 0;
	stub[at++] = callback_opcode;
	stub[at++] = vector;
	if (vector == 0x08)
	{
		// The BIOS timer handler chains to INT 1Ch before acknowledging.
		stub[at++] = 0xcd;
		stub[at++] = 0x1c;
		stub[at++] = callback_opcode;
		stub[at++] = callback_eoi;
	}
	stub[at++] = 0xcf; // IRET

	mem_write16(s, uint32_t(vector) * 4, offset);
	mem_write16(s, uint32_t(vector) * 4 + 2, callback_segment);
}

void machine_callback(machine_state& s, uint8_t index)
{
	if (index == callback_eoi)
	{
		pic_write(s, 0x20, 0x20);
		return;
	}

	// An interrupt-level trace is how you find out why a program gave up, so
	// it reports the call before it is serviced.
	if (s.trace && s.host != nullptr && index != 0x08 && index != 0x09)
	{
		const cpu_state& c = s.cpu;
		char text[160] = {};
		snprintf(text, sizeof(text), "INT %02Xh AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X from %04X:%04X",
		         index, c.ax(), c.bx(), c.cx(), c.dx(), c.ds(), c.es(),
		         mem_read16(s, linear_address(c.ss(), uint16_t(c.sp() + 2))),
		         mem_read16(s, linear_address(c.ss(), c.sp())));
		s.host->trace(text);
	}

	if (dos_interrupt(s, index)) return;
	if (bios_interrupt(s, index)) return;
	// Anything else falls through to the IRET in the stub.
}

int read_string(machine_state& s, uint16_t segment, uint16_t offset, char* out, int out_size, char terminator)
{
	int length = 0;
	while (length < out_size - 1)
	{
		const char c = char(read8(s, segment, uint16_t(offset + length)));
		if (c == terminator) break;
		out[length++] = c;
	}
	out[length] = 0;
	return length;
}

uint8_t io_read8(machine_state& s, uint16_t port)
{
	switch (port)
	{
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		return pic_read(s, port);
	case 0x40: case 0x41: case 0x42: case 0x43:
		return pit_read(s, port);
	case 0x60: case 0x64:
		return kbd_read(s, port);
	case 0x61:
		// Bit 4 is the refresh signal; timing loops watch it move.
		return uint8_t((s.kbd.port_61 & 0x0f) | ((s.cycles & 0x40) != 0 ? 0x10 : 0x00));
	case 0x201:
		return 0xf0; // no joystick attached
	case 0x388: case 0x389:
		return opl_read_status(s);
	case 0x92:
		return 0x02; // A20 always on
	default:
		if (port >= 0x3b0 && port <= 0x3df) return vga_port_read(s, port);
		return 0xff;
	}
}

void io_write8(machine_state& s, uint16_t port, uint8_t value)
{
	switch (port)
	{
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		pic_write(s, port, value);
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		pit_write(s, port, value);
		return;
	case 0x60: case 0x64:
		kbd_write(s, port, value);
		return;
	case 0x61:
		s.kbd.port_61 = value;
		s.pit.channel[2].gate = (value & 1) != 0;
		sound_queue_write(s, 0x61, value);
		return;
	case 0x388: case 0x389:
		sound_queue_write(s, port, value);
		return;
	default:
		if (port >= 0x3b0 && port <= 0x3df) vga_port_write(s, port, value);
		return;
	}
}

machine::machine() : state_(new machine_state())
{
	reset();
}

machine::~machine() = default;

void machine::set_host(host_services* host)
{
	state_->host = host;
}

void machine_reset(machine_state& s)
{
	host_services* host = s.host;
	const uint32_t cpu_hz = s.cpu_hz;
	const bool trace = s.trace;

	std::memset(s.memory, 0, sizeof(s.memory));
	cpu_reset(s.cpu);
	pit_reset(s.pit);
	pic_reset(s.pic);
	sound_reset(s.sound);
	s.kbd = keyboard_controller();
	s.cycles = 0;
	s.next_render = 0;
	s.exited = false;
	s.exit_code = 0;
	s.error[0] = 0;
	s.host = host;
	s.cpu_hz = cpu_hz;
	s.trace = trace;
	s.shell = shell_state();

	// A far jump to the BIOS entry point at the reset vector, so a program
	// that jumps to F000:FFF0 restarts rather than executing rubbish.
	s.memory[0xffff0] = 0xea;
	s.memory[0xffff1] = 0xf0;
	s.memory[0xffff2] = 0xff;
	s.memory[0xffff3] = 0x00;
	s.memory[0xffff4] = 0xf0;

	for (int vector = 0; vector < 0x80; ++vector) machine_install_vector(s, uint8_t(vector));

	bios_reset(s);
	vga_reset(s.vga);
	vga_set_mode(s, 3);
	dos_reset(s);

	// Unmask the timer and keyboard interrupts, as the BIOS leaves them.
	s.pic.master.imr = 0xfc;
	s.pic.slave.imr = 0xff;
	s.pit.channel[0].reload = 0; // 65536 ticks, the familiar 18.2 Hz
	s.pit.channel[0].counter = 0x10000;
	s.cpu.flags |= flag_if;
}

void machine::reset()
{
	machine_reset(*state_);
}

bool machine::load_program(const uint8_t* image, uint32_t size, const char* dos_path, const char* command_line)
{
	machine_state& s = *state_;
	reset();
	if (!dos_load_program(s, image, size, dos_path, command_line, s.error, sizeof(s.error)))
	{
		s.exited = true;
		return false;
	}
	return true;
}

uint32_t machine::cycles_per_frame() const
{
	return state_->cpu_hz / 60;
}

uint64_t machine::exec(uint64_t budget)
{
	machine_state& s = *state_;

	// With the shell at its prompt there is no program to run, so the CPU is
	// left alone and only the clock moves on, for the cursor blink. The
	// keyboard still has to be serviced, though: nothing is executing the
	// BIOS INT 9 handler that would normally translate a scancode.
	if (s.shell.active && !s.shell.program_running)
	{
		while (s.kbd.queue_count > 0) bios_keyboard_interrupt(s);
		shell_tick(s);
		s.cycles += budget;
		vga_render(s);
		draw_mouse_pointer(s);
		return budget;
	}

	uint64_t spent = 0;
	bool rendered = false;

	while (spent < budget && !s.exited)
	{
		const int cost = cpu_step(s);
		s.cycles += uint64_t(cost);
		spent += uint64_t(cost);
		pit_step(s, uint64_t(cost));

		// Sample the picture at the emulated vertical retrace: a program that
		// waits for retrace before drawing has finished by then, so the frame
		// the host sees is a whole one rather than a half-drawn one.
		if (!rendered && s.cycles >= s.next_render)
		{
			vga_render(s);
			rendered = true;
		}
	}

	if (s.exited && s.shell.active && s.shell.program_running)
	{
		shell_program_finished(s);
		rendered = false;
	}

	if (!rendered) vga_render(s);

	const uint64_t frame_cycles = s.cpu_hz / 70;
	const uint64_t retrace = frame_cycles - frame_cycles / 20;
	s.next_render = (s.cycles / frame_cycles) * frame_cycles + retrace;
	if (s.next_render <= s.cycles) s.next_render += frame_cycles;

	draw_mouse_pointer(s);
	return spent;
}

void machine::start_shell()
{
	machine_reset(*state_);
	shell_start(*state_);
}

bool machine::shell_active() const { return state_->shell.active; }

bool machine::run_program(const char* name, const char* command_line)
{
	return shell_run(*state_, name, command_line);
}

bool machine::exited() const { return state_->exited; }
uint8_t machine::exit_code() const { return state_->exit_code; }
const char* machine::error() const { return state_->error; }

const uint32_t* machine::framebuffer() const { return state_->vga.framebuffer; }
int machine::width() const { return state_->vga.width; }
int machine::height() const { return state_->vga.height; }

void machine::set_render_enabled(bool enabled) { state_->vga.render_enabled = enabled; }

void machine::set_audio_enabled(bool enabled)
{
	state_->sound.enabled = enabled;
	state_->sound.last_cycle = state_->cycles;
}

void machine::generate_audio(int16_t* out, int frames)
{
	sound_render(*state_, out, frames);
}

void machine::key_down(machine_key key)
{
	const uint8_t code = key_scancode(key);
	if (code != 0) kbd_push_scancode(*state_, code);
}

void machine::key_up(machine_key key)
{
	const uint8_t code = key_scancode(key);
	if (code != 0) kbd_push_scancode(*state_, uint8_t(code | 0x80));
}

void machine::mouse_move(int x, int y)
{
	mouse_state& m = state_->bios.mouse;
	const int span_x = m.max_x - m.min_x + 1;
	const int span_y = m.max_y - m.min_y + 1;
	const int width = state_->vga.width > 0 ? state_->vga.width : 1;
	const int height = state_->vga.height > 0 ? state_->vga.height : 1;
	const int nx = m.min_x + x * span_x / width;
	const int ny = m.min_y + y * span_y / height;
	m.delta_x += (nx - m.x) * m.granularity_x / 8;
	m.delta_y += (ny - m.y) * m.granularity_y / 8;
	m.x = nx < m.min_x ? m.min_x : (nx > m.max_x ? m.max_x : nx);
	m.y = ny < m.min_y ? m.min_y : (ny > m.max_y ? m.max_y : ny);
}

void machine::mouse_button(int button, bool down)
{
	mouse_state& m = state_->bios.mouse;
	if (button < 0 || button > 2) return;
	const int bit = 1 << button;
	if (down)
	{
		m.buttons |= bit;
		++m.press_count[button];
		m.press_x[button] = m.x;
		m.press_y[button] = m.y;
	}
	else
	{
		m.buttons &= ~bit;
		++m.release_count[button];
		m.release_x[button] = m.x;
		m.release_y[button] = m.y;
	}
}

int machine::text_screen(char* out, int out_size) const
{
	const vga_state& v = state_->vga;
	int written = 0;
	for (int row = 0; row < v.text_rows && written < out_size - 1; ++row)
	{
		int line_end = written;
		for (int column = 0; column < v.text_columns && written < out_size - 1; ++column)
		{
			const uint32_t cell = (v.display_start + uint32_t(row * v.text_columns + column)) & (vga_plane_size - 1);
			const uint8_t ch = v.mem[cell * 4];
			out[written++] = (ch >= 32 && ch < 127) ? char(ch) : (ch == 0 ? ' ' : '.');
			if (ch > 32) line_end = written;
		}
		written = line_end;
		if (written < out_size - 1) out[written++] = '\n';
	}
	out[written] = 0;
	return written;
}

machine_state& machine::state() { return *state_; }
const machine_state& machine::state() const { return *state_; }
