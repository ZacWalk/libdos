#pragma once

// libdos by Zac Walker
//
// The whole emulated PC as one plain-data struct, plus the small class a host
// embeds. `machine` owns exactly one `machine_state` and is non-copyable.
// Nothing in this header knows what a file, a window or a thread is.

#include <cstdint>
#include <memory>

#include "cpu.h"
#include "vga.h"
#include "dos.h"
#include "bios.h"
#include "chips.h"
#include "shell.h"
#include "sound.h"
#include "host.h"

// 1 MiB of real-mode address space plus the 64 KiB the A20-less wrap of a
// segment:offset pair at the top of memory can reach, so no access needs a
// bounds test in the hot path.
constexpr uint32_t memory_size = 0x110000;

// Where the emulator keeps the interrupt stubs it owns. Every vector it
// services points into this page, so a program can chain to the previous
// handler and still reach us.
constexpr uint16_t callback_segment = 0xf000;
constexpr uint16_t callback_offset = 0xe000;
constexpr uint8_t callback_opcode = 0xf1;

// The callback boot mode's reset vector makes: power the machine off and on.
constexpr uint8_t callback_power_on = 0xfe;

struct machine_state
{
	uint8_t memory[memory_size] = {};
	cpu_state cpu;
	vga_state vga;
	dos_state dos;
	bios_state bios;
	pit_state pit;
	pic_state pic;
	keyboard_controller kbd;
	sound_state sound;
	shell_state shell;

	host_services* host = nullptr;

	uint64_t cycles = 0;
	uint32_t cpu_hz = 8000000;
	cpu_model model = cpu_model::i386;
	bool boot_mode = false; // no DOS mapped in: whatever was booted owns INT 20h and up
	bool exited = false;
	uint8_t exit_code = 0;
	bool faulted = false; // stopped by the emulator, not by the program; `error` says why
	bool trace = false;
	char error[256] = {};
};

inline uint32_t linear_address(uint16_t segment, uint16_t offset)
{
	return (uint32_t(segment) << 4) + offset;
}

// The A0000-BFFFF window belongs to the display adapter; everything else is
// plain RAM. These two functions are the only path to memory.
inline uint8_t mem_read8(machine_state& s, uint32_t address)
{
	address &= 0xfffff;
	if (address >= 0xa0000 && address < 0xc0000) return vga_mem_read(s, address);
	return s.memory[address];
}

inline void mem_write8(machine_state& s, uint32_t address, uint8_t value)
{
	address &= 0xfffff;
	if (address >= 0xa0000 && address < 0xc0000)
	{
		vga_mem_write(s, address, value);
		return;
	}
	if (address >= 0xc0000) return; // ROM
	s.memory[address] = value;
}

inline uint16_t mem_read16(machine_state& s, uint32_t address)
{
	return uint16_t(mem_read8(s, address) | (mem_read8(s, address + 1) << 8));
}

inline void mem_write16(machine_state& s, uint32_t address, uint16_t value)
{
	mem_write8(s, address, uint8_t(value));
	mem_write8(s, address + 1, uint8_t(value >> 8));
}

inline uint8_t read8(machine_state& s, uint16_t segment, uint16_t offset)
{
	return mem_read8(s, linear_address(segment, offset));
}

inline uint16_t read16(machine_state& s, uint16_t segment, uint16_t offset)
{
	return mem_read16(s, linear_address(segment, offset));
}

inline void write8(machine_state& s, uint16_t segment, uint16_t offset, uint8_t value)
{
	mem_write8(s, linear_address(segment, offset), value);
}

inline void write16(machine_state& s, uint16_t segment, uint16_t offset, uint16_t value)
{
	mem_write16(s, linear_address(segment, offset), value);
}

uint8_t io_read8(machine_state& s, uint16_t port);
void io_write8(machine_state& s, uint16_t port, uint8_t value);

// Called by the CPU when it meets the callback opcode inside one of our stubs.
void machine_callback(machine_state& s, uint8_t index);

// Stops the machine because of something the program cannot be allowed to do
// or the emulator cannot do for it, keeping the first reason given.
void machine_fault(machine_state& s, const char* reason);

// Returns the machine to power-on state, keeping the host and the current
// directory. `machine::reset` is this plus nothing else.
void machine_reset(machine_state& s);

// Points vector `n` at the stub the emulator owns for it.
void machine_install_vector(machine_state& s, uint8_t vector);
uint32_t machine_stub_address(uint8_t vector);

// Copies a NUL or '$' terminated string out of emulated memory.
int read_string(machine_state& s, uint16_t segment, uint16_t offset, char* out, int out_size, char terminator = 0);

enum class machine_key : uint8_t
{
	none,
	escape, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
	backtick, num1, num2, num3, num4, num5, num6, num7, num8, num9, num0, minus, equals, backspace,
	tab, q, w, e, r, t, y, u, i, o, p, left_bracket, right_bracket, backslash,
	caps_lock, a, s, d, f, g, h, j, k, l, semicolon, quote, enter,
	left_shift, z, x, c, v, b, n, m, comma, period, slash, right_shift,
	left_ctrl, left_alt, space, right_alt, right_ctrl,
	insert, del, home, end, page_up, page_down, up, down, left, right,
	num_lock, keypad_slash, keypad_star, keypad_minus, keypad_plus, keypad_enter, keypad_period,
	keypad0, keypad1, keypad2, keypad3, keypad4, keypad5, keypad6, keypad7, keypad8, keypad9,
	print_screen, scroll_lock, pause,
};

class machine
{
public:
	machine();
	~machine();
	machine(const machine&) = delete;
	machine& operator=(const machine&) = delete;

	void set_host(host_services* host);
	void reset();

	// Settings that outlive reset(): which processor runs the program, what
	// INT 21h AH=30h reports, and the environment a program the host starts is
	// given, as NAME=VALUE strings (none restores the default).
	void set_cpu_model(cpu_model model);
	void set_dos_version(uint8_t major, uint8_t minor);
	bool set_environment(const char* const* variables, int count);

	// Makes a DOS path such as "C:\SRC" the current directory, if the host
	// has it.
	bool set_current_directory(const char* dos_path);

	// Loads an MZ or COM image and leaves the CPU at its entry point.
	bool load_program(const uint8_t* image, uint32_t size, const char* dos_path, const char* command_line);

	// Brings up the built-in command interpreter, which is where the user
	// lands when no program has been started.
	void start_shell();
	bool shell_active() const;

	// Boot mode: an IBM PC with no DOS mapped in, which loads a disk's boot
	// sector and runs whatever it finds - DOS's own IO.SYS and MSDOS.SYS, on
	// the emulated BIOS. Drives 00h and 01h are floppies, 80h and 81h hard
	// disks. The device stays the host's and must outlive the machine, or be
	// detached with a null one; `extensions` gives a hard disk INT 13h's LBA
	// calls, AH=41h-48h. Attach the drives before power_on.
	bool attach_disk(uint8_t drive, block_device* device, const disk_geometry& geometry, bool extensions);

	// Starts the machine in boot mode from `drive`, as its power switch would.
	// A program jumping to FFFF:0000 does the same. False if there is nothing
	// to boot there, with error() saying so.
	bool power_on(uint8_t drive);

	// Runs a program by name, exactly as typing it at the prompt would, so it
	// returns to the prompt when it ends.
	bool run_program(const char* name, const char* command_line);

	// Runs until the budget is spent or the program terminates; returns the
	// cycles actually consumed.
	uint64_t exec(uint64_t cycles);
	uint32_t cycles_per_frame() const;

	bool exited() const;
	uint8_t exit_code() const;
	bool faulted() const;
	const char* error() const;

	const uint32_t* framebuffer() const;
	int width() const;
	int height() const;
	void set_render_enabled(bool enabled);

	void set_audio_enabled(bool enabled);
	void generate_audio(int16_t* out, int frames);

	void key_down(machine_key key);
	void key_up(machine_key key);
	void mouse_move(int x, int y);
	void mouse_button(int button, bool down);

	// Captures the 80x25 text screen as ASCII, for tests and diagnostics.
	int text_screen(char* out, int out_size) const;

	machine_state& state();
	const machine_state& state() const;

private:
	std::unique_ptr<machine_state> state_;
};
