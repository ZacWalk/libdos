#pragma once

// ovrwin by Zac Walker
//
// Audio: a YM3812 (AdLib / OPL2) synthesiser and the PC speaker driven from
// timer channel 2. Register writes are timestamped with the machine's cycle
// counter and replayed when the host asks for samples, so a host can run a
// whole frame and only then collect its audio without losing sub-frame note
// changes.

#include <cstdint>

struct machine_state;

constexpr int sound_sample_rate = 44100;
constexpr int opl_channels = 9;
constexpr int opl_operators = 18;

struct opl_operator
{
	double phase = 0;
	double envelope = 96.0; // attenuation in dB, 96 = silent
	int state = 0; // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
	double output = 0;
	double previous = 0; // for feedback averaging
	bool key_on = false;
};

struct opl_state
{
	uint8_t regs[256] = {};
	uint8_t index = 0;
	opl_operator ops[opl_operators];
	bool enabled = false; // set once a program writes a non-zero register

	// The detection sequence has to answer straight away, so the register
	// index and timer control are tracked outside the deferred replay.
	uint8_t live_index = 0;
	uint8_t live_timer = 0;
};

struct speaker_state
{
	bool timer_gate = false;
	bool data_bit = false;
	double phase = 0;
	bool level = false;
};

struct sound_event
{
	uint64_t cycle = 0;
	uint16_t port = 0;
	uint16_t value = 0;
};

constexpr int sound_queue_size = 4096;

struct sound_state
{
	opl_state opl;
	speaker_state speaker;

	sound_event queue[sound_queue_size];
	int queue_count = 0;
	uint64_t last_cycle = 0;
	bool enabled = false;
	int volume = 100; // percent
};

void sound_reset(sound_state& s);

// Records a write to an audio-visible port (0x388/0x389 for the OPL, 0x61 and
// the timer for the speaker) so it can be replayed at the right sample.
void sound_queue_write(machine_state& s, uint16_t port, uint16_t value);

uint8_t opl_read_status(machine_state& s);

// Produces `frames` stereo-free 16-bit samples covering the span since the
// previous call, and drops the events it consumed.
void sound_render(machine_state& s, int16_t* out, int frames);
