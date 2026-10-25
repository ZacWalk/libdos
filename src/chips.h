#pragma once

// libdos by Zac Walker
//
// The support chips a DOS program can see: the 8253 timer, the 8259 interrupt
// controllers and the 8042 keyboard controller. Only the behaviour programs
// actually depend on is modelled - counting, interrupt priority and the status
// bits that timing loops poll.

#include <cstdint>

struct machine_state;

constexpr uint32_t pit_frequency = 1193182;

struct pit_channel
{
	uint16_t reload = 0xffff;
	uint16_t latched = 0;
	uint8_t mode = 3;
	uint8_t access = 3; // 1 = lo, 2 = hi, 3 = lo/hi
	bool write_high = false; // next lo/hi byte is the high one
	bool read_high = false;
	bool latch_pending = false;
	bool bcd = false;
	bool gate = true;
	uint64_t start_tick = 0;
	uint32_t counter = 0x10000; // a zero reload means a full 65536 ticks
};

struct pit_state
{
	pit_channel channel[3];
	uint64_t ticks = 0; // in 1.193182 MHz units
	uint64_t remainder = 0; // cycles not yet converted to ticks

	// Cycles the CPU has run that the counters have not been moved on by, and
	// how many can pass before channel 0 next reaches zero. The counters are
	// only worked out then, or when a program looks at the timer.
	uint64_t pending = 0;
	uint64_t due = 0;
};

struct pic_chip
{
	uint8_t irr = 0, imr = 0xff, isr = 0;
	uint8_t base = 8;
	int init_step = 0;
	bool auto_eoi = false;
	bool read_isr = false;
};

struct pic_state
{
	pic_chip master;
	pic_chip slave;
};

struct keyboard_controller
{
	uint8_t output = 0;
	uint8_t status = 0x14;
	uint8_t command = 0;
	bool command_pending = false;
	uint8_t port_61 = 0;
	uint8_t queue[32] = {};
	int queue_count = 0;
};

void pit_reset(pit_state& p);
void pit_step(machine_state& s, uint64_t cycles);

// Brings the counters up to date with the cycles run since they were last
// moved on, and works out when channel 0 is next due. The emulation loop adds
// each instruction's cycles to `pending` and calls this once they reach `due`,
// which raises IRQ 0 on exactly the instruction stepping every time would.
void pit_sync(machine_state& s);

uint8_t pit_read(machine_state& s, uint16_t port);
void pit_write(machine_state& s, uint16_t port, uint8_t value);

void pic_reset(pic_state& p);
void pic_raise(machine_state& s, int irq);
void pic_lower(machine_state& s, int irq);
int pic_pending_vector(machine_state& s); // -1 when nothing is ready
void pic_acknowledge(machine_state& s, int vector);
uint8_t pic_read(machine_state& s, uint16_t port);
void pic_write(machine_state& s, uint16_t port, uint8_t value);

void kbd_push_scancode(machine_state& s, uint8_t code);
uint8_t kbd_read(machine_state& s, uint16_t port);
void kbd_write(machine_state& s, uint16_t port, uint8_t value);
