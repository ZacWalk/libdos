#pragma once

// libdos by Zac Walker
//
// PC BIOS services (INT 10h video, 11h/12h equipment, 13h disk, 15h system,
// 16h keyboard, 19h bootstrap, 1Ah clock) plus the INT 33h mouse driver, and
// the input state they read. The BIOS data area lives in emulated memory at
// segment 0x40 so programs that poke it directly - and most of them do - see
// the truth. The disks and the bootstrap serve boot mode only.

#include <cstdint>

#include "block-device.h"

struct machine_state;

constexpr uint16_t bda_segment = 0x0040;
constexpr uint16_t bda_keyboard_flags = 0x0017;
constexpr uint16_t bda_keyboard_head = 0x001a;
constexpr uint16_t bda_keyboard_tail = 0x001c;
constexpr uint16_t bda_keyboard_buffer = 0x001e;
constexpr uint16_t bda_keyboard_buffer_end = 0x003e;
constexpr uint16_t bda_timer_ticks = 0x006c;

struct mouse_state
{
	bool present = true;
	int visible = -1; // hide count, visible when >= 0
	int x = 0, y = 0; // in virtual mouse units (0-639, 0-199 scaled by mode)
	int min_x = 0, max_x = 639, min_y = 0, max_y = 199;
	int buttons = 0;
	int delta_x = 0, delta_y = 0; // accumulated since the last read of AH=0Bh
	int press_count[3] = {}, release_count[3] = {};
	int press_x[3] = {}, press_y[3] = {}, release_x[3] = {}, release_y[3] = {};
	uint16_t callback_mask = 0;
	uint32_t callback = 0; // segment:offset
	int granularity_x = 8, granularity_y = 8;
};

// A drive boot mode's INT 13h serves.
struct bios_drive
{
	block_device* device = nullptr;
	disk_geometry geometry;
	bool extensions = false; // the LBA calls, AH=41h-48h; hard disks only
	uint8_t status = 0; // the last call's, for AH=01h
};

// Floppies 00h and 01h, then hard disks 80h and 81h.
constexpr int bios_drive_count = 4;

struct bios_state
{
	uint16_t equipment = 0x0021;
	uint16_t memory_kb = 640;
	uint32_t ticks = 0;
	mouse_state mouse;

	// The host's, attached to the machine: a reset keeps them.
	bios_drive drives[bios_drive_count];
	uint8_t boot_drive = 0;
};

void bios_reset(machine_state& s);

// An IBM PC 5150's ROM for boot mode, on top of bios_reset: the model byte and
// date, the diskette parameter table, an equipment word that counts the
// floppies, and nothing above INT 1Fh - DOS brings its own.
void bios_reset_pc(machine_state& s);

// Index into bios_state::drives for a BIOS drive number, or -1.
int bios_drive_index(uint8_t drive);

// What INT 19h does: loads the boot drive's first sector to 0000:7C00 and
// jumps to it with DL holding the drive. False, with the machine stopped, if
// there is nothing there to boot.
bool bios_bootstrap(machine_state& s);

// Services the BIOS interrupt vectors. Returns false if the vector is not ours.
bool bios_interrupt(machine_state& s, uint8_t vector);

// Pushes a key into the BIOS keyboard buffer. `scancode` is the XT make code,
// `ascii` the translated character (0 for extended keys).
void bios_push_key(machine_state& s, uint8_t scancode, uint8_t ascii);
bool bios_peek_key(machine_state& s, uint16_t& key);
bool bios_pop_key(machine_state& s, uint16_t& key);

// Translates a scancode arriving from the keyboard controller and files it in
// the BIOS buffer; this is what the INT 9 handler does.
void bios_keyboard_interrupt(machine_state& s);

// Rewinds the return address of the interrupt currently being serviced so the
// INT instruction runs again. Used by the calls that block on input.
void bios_retry_interrupt(machine_state& s);
