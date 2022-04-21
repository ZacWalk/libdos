#pragma once

// ovrwin by Zac Walker
//
// PC BIOS services (INT 10h video, 11h/12h equipment, 13h disk, 15h system,
// 16h keyboard, 1Ah clock) plus the INT 33h mouse driver, and the input state
// they read. The BIOS data area lives in emulated memory at segment 0x40 so
// programs that poke it directly - and most of them do - see the truth.

#include <cstdint>

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

struct bios_state
{
	uint16_t equipment = 0x0021;
	uint16_t memory_kb = 640;
	uint32_t ticks = 0;
	mouse_state mouse;
};

void bios_reset(machine_state& s);

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
