// ovrwin by Zac Walker
//
// AdLib (YM3812 / OPL2) synthesis and the PC speaker. Register writes are
// timestamped with the machine's cycle counter and replayed against the sample
// clock, so a host can run a whole frame of emulation and only then ask for
// its audio without smearing note changes to frame boundaries.

#include "machine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr double two_pi = 6.283185307179586;
	constexpr double opl_native_rate = 49716.0;

	// Frequency multiplier per operator, in halves.
	const double multiplier_table[16] = {
		0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15
	};

	// Key scale level attenuation in dB per octave above the base.
	const double ksl_table[4] = {0.0, 1.5, 3.0, 6.0};

	// ops[i] holds the operator whose registers live at this offset from the
	// start of each OPL register block.
	const int operator_slot[opl_operators] = {
		0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21
	};

	int slot_to_operator(int slot)
	{
		for (int i = 0; i < opl_operators; ++i) if (operator_slot[i] == slot) return i;
		return -1;
	}

	int modulator_of(int channel) { return (channel / 3) * 6 + (channel % 3); }

	double waveform(int shape, double phase)
	{
		const double x = std::fmod(phase, 1.0) * two_pi;
		const double s = std::sin(x);
		switch (shape & 3)
		{
		case 1: return s > 0 ? s : 0.0;
		case 2: return std::fabs(s);
		case 3: return std::fmod(phase, 0.5) < 0.25 ? std::fabs(s) : 0.0;
		default: return s;
		}
	}

	double amplitude_from_db(double db)
	{
		if (db >= 96.0) return 0.0;
		return std::pow(10.0, -db / 20.0);
	}

	// Approximate the OPL envelope generator: the published rate tables are
	// close to a halving of the time for every four steps of the effective
	// rate, which is what these two helpers reproduce.
	double decay_step_per_sample(int rate)
	{
		if (rate <= 0) return 0.0;
		if (rate >= 63) return 96.0;
		const double milliseconds = std::pow(2.0, (60.0 - rate) / 4.0) * 1.2;
		return 96.0 / (milliseconds * sound_sample_rate / 1000.0);
	}

	double attack_step_per_sample(int rate)
	{
		if (rate <= 0) return 0.0;
		if (rate >= 60) return 96.0;
		const double milliseconds = std::pow(2.0, (60.0 - rate) / 4.0) * 0.35;
		return 96.0 / (milliseconds * sound_sample_rate / 1000.0);
	}

	struct opl_operator_settings
	{
		int multiplier = 0;
		int ksl = 0;
		int total_level = 0;
		int attack = 0, decay = 0, sustain = 0, release = 0;
		bool sustaining = false;
		bool ksr = false;
		int wave = 0;
	};

	opl_operator_settings read_settings(const opl_state& o, int index)
	{
		const int slot = operator_slot[index];
		opl_operator_settings settings;
		const uint8_t am = o.regs[0x20 + slot];
		settings.multiplier = am & 0x0f;
		settings.ksr = (am & 0x10) != 0;
		settings.sustaining = (am & 0x20) != 0;
		const uint8_t ksl_tl = o.regs[0x40 + slot];
		settings.ksl = (ksl_tl >> 6) & 3;
		settings.total_level = ksl_tl & 0x3f;
		const uint8_t ad = o.regs[0x60 + slot];
		settings.attack = (ad >> 4) & 0x0f;
		settings.decay = ad & 0x0f;
		const uint8_t sr = o.regs[0x80 + slot];
		settings.sustain = (sr >> 4) & 0x0f;
		settings.release = sr & 0x0f;
		settings.wave = (o.regs[0x01] & 0x20) != 0 ? (o.regs[0xe0 + slot] & 3) : 0;
		return settings;
	}

	void opl_key(opl_state& o, int channel, bool on)
	{
		const int modulator = modulator_of(channel);
		for (int i = 0; i < 2; ++i)
		{
			opl_operator& op = o.ops[modulator + i * 3];
			if (on && !op.key_on)
			{
				op.state = 1;
				op.phase = 0;
			}
			else if (!on && op.key_on)
			{
				op.state = 4;
			}
			op.key_on = on;
		}
	}

	void opl_write(opl_state& o, uint8_t reg, uint8_t value)
	{
		o.regs[reg] = value;
		if (reg != 0) o.enabled = true;
		if (reg >= 0xb0 && reg <= 0xb8) opl_key(o, reg - 0xb0, (value & 0x20) != 0);
	}

	double operator_output(opl_state& o, int index, int channel, double modulation, double samples_per_step)
	{
		opl_operator& op = o.ops[index];
		const opl_operator_settings settings = read_settings(o, index);

		const uint8_t low = o.regs[0xa0 + channel];
		const uint8_t high = o.regs[0xb0 + channel];
		const int fnum = ((high & 3) << 8) | low;
		const int block = (high >> 2) & 7;

		const int key_scale = settings.ksr ? (block * 2 + (fnum >> 9)) : (block / 2);

		switch (op.state)
		{
		case 1:
		{
			const double step = attack_step_per_sample(std::min(63, 4 * settings.attack + key_scale)) *
				samples_per_step;
			if (settings.attack == 0) op.envelope = 96.0;
			else op.envelope -= step;
			if (op.envelope <= 0.0)
			{
				op.envelope = 0.0;
				op.state = 2;
			}
			break;
		}
		case 2:
		{
			const double step = decay_step_per_sample(std::min(63, 4 * settings.decay + key_scale)) *
				samples_per_step;
			op.envelope += step;
			const double sustain_db = settings.sustain == 15 ? 96.0 : settings.sustain * 3.0;
			if (op.envelope >= sustain_db)
			{
				op.envelope = sustain_db;
				op.state = 3;
			}
			break;
		}
		case 3:
			if (!settings.sustaining)
			{
				op.envelope += decay_step_per_sample(std::min(63, 4 * settings.release + key_scale)) *
					samples_per_step;
				if (op.envelope >= 96.0)
				{
					op.envelope = 96.0;
					op.state = 0;
				}
			}
			break;
		case 4:
			op.envelope += decay_step_per_sample(std::min(63, 4 * settings.release + key_scale)) * samples_per_step;
			if (op.envelope >= 96.0)
			{
				op.envelope = 96.0;
				op.state = 0;
			}
			break;
		default:
			return 0.0;
		}

		const double frequency = double(fnum) * opl_native_rate / std::pow(2.0, 20.0 - block);
		const double increment = frequency * multiplier_table[settings.multiplier] / sound_sample_rate;
		op.phase += increment * samples_per_step;
		if (op.phase > 1e6) op.phase = std::fmod(op.phase, 1.0);

		const double ksl_db = ksl_table[settings.ksl] * std::max(0, block - 3);
		const double attenuation = op.envelope + settings.total_level * 0.75 + ksl_db;
		const double value = waveform(settings.wave, op.phase + modulation) * amplitude_from_db(attenuation);
		op.previous = op.output;
		op.output = value;
		return value;
	}

	double opl_sample(opl_state& o, double samples_per_step)
	{
		if (!o.enabled) return 0.0;

		double mix = 0.0;
		for (int channel = 0; channel < opl_channels; ++channel)
		{
			const int modulator = modulator_of(channel);
			const int carrier = modulator + 3;
			if (o.ops[modulator].state == 0 && o.ops[carrier].state == 0) continue;

			const uint8_t connection = o.regs[0xc0 + channel];
			const int feedback = (connection >> 1) & 7;
			const double feedback_amount = feedback == 0
				                               ? 0.0
				                               : (o.ops[modulator].output + o.ops[modulator].previous) * 0.5 *
				                               std::pow(2.0, feedback - 1) / 8.0;

			const double modulator_value = operator_output(o, modulator, channel, feedback_amount, samples_per_step);
			if ((connection & 1) != 0)
			{
				mix += modulator_value;
				mix += operator_output(o, carrier, channel, 0.0, samples_per_step);
			}
			else
			{
				mix += operator_output(o, carrier, channel, modulator_value * 0.5, samples_per_step);
			}
		}
		return mix / 9.0;
	}
}

void sound_reset(sound_state& s)
{
	s = sound_state();
}

void sound_queue_write(machine_state& s, uint16_t port, uint16_t value)
{
	sound_state& sound = s.sound;

	// The AdLib detection sequence writes the timer control register and reads
	// the status back at once, so that path cannot wait for the mixer.
	if (port == 0x388) sound.opl.live_index = uint8_t(value);
	if (port == 0x389 && sound.opl.live_index == 0x04)
	{
		if ((value & 0x80) != 0) sound.opl.live_timer = 0;
		else sound.opl.live_timer = uint8_t(value & 0x03);
	}

	if (sound.queue_count >= sound_queue_size)
	{
		// The queue only overflows when a host stops asking for audio; drop
		// the oldest event rather than the newest so state stays current.
		std::memmove(sound.queue, sound.queue + 1, sizeof(sound_event) * (sound_queue_size - 1));
		--sound.queue_count;
	}
	sound_event& event = sound.queue[sound.queue_count++];
	event.cycle = s.cycles;
	event.port = port;
	event.value = value;

	if (!sound.enabled)
	{
		// With audio off the queue is never drained, so apply writes directly
		// to keep register reads consistent.
		if (port == 0x389) opl_write(sound.opl, sound.opl.index, uint8_t(value));
		else if (port == 0x388) sound.opl.index = uint8_t(value);
		sound.queue_count = 0;
	}
}

uint8_t opl_read_status(machine_state& s)
{
	// Programs detect an AdLib by starting timer 1 and watching the two top
	// status bits come up.
	return s.sound.opl.live_timer != 0 ? 0xc0 : 0x00;
}

void sound_render(machine_state& s, int16_t* out, int frames)
{
	sound_state& sound = s.sound;
	if (frames <= 0) return;

	const uint64_t span = s.cycles > sound.last_cycle ? s.cycles - sound.last_cycle : 0;
	const uint64_t start = sound.last_cycle;
	const double cycles_per_sample = span > 0 ? double(span) / frames : 0.0;
	const double samples_per_step = 1.0;

	int event = 0;
	double speaker_phase = sound.speaker.phase;
	uint16_t speaker_reload = uint16_t(s.pit.channel[2].reload);

	for (int i = 0; i < frames; ++i)
	{
		const uint64_t now = start + uint64_t(cycles_per_sample * i);
		while (event < sound.queue_count && sound.queue[event].cycle <= now)
		{
			const sound_event& e = sound.queue[event];
			switch (e.port)
			{
			case 0x388: sound.opl.index = uint8_t(e.value);
				break;
			case 0x389: opl_write(sound.opl, sound.opl.index, uint8_t(e.value));
				break;
			case 0x42: speaker_reload = e.value;
				break;
			case 0x61:
				sound.speaker.timer_gate = (e.value & 1) != 0;
				sound.speaker.data_bit = (e.value & 2) != 0;
				break;
			default: break;
			}
			++event;
		}

		double sample = opl_sample(sound.opl, samples_per_step) * 0.85;

		if (sound.speaker.timer_gate && sound.speaker.data_bit)
		{
			const double frequency = speaker_reload == 0
				                         ? double(pit_frequency) / 65536.0
				                         : double(pit_frequency) / speaker_reload;
			speaker_phase += frequency / sound_sample_rate;
			if (speaker_phase >= 1.0) speaker_phase -= std::floor(speaker_phase);
			sample += (speaker_phase < 0.5 ? 0.18 : -0.18);
		}
		else if (sound.speaker.data_bit)
		{
			// Direct speaker toggling, as used for sampled sound effects.
			sample += 0.18;
		}

		sample *= sound.volume / 100.0;
		if (sample > 1.0) sample = 1.0;
		if (sample < -1.0) sample = -1.0;
		out[i] = int16_t(sample * 30000.0);
	}

	sound.speaker.phase = speaker_phase;

	// Anything left in the queue happened after the span we just rendered.
	if (event > 0)
	{
		const int remaining = sound.queue_count - event;
		if (remaining > 0) std::memmove(sound.queue, sound.queue + event, sizeof(sound_event) * remaining);
		sound.queue_count = remaining;
	}
	sound.last_cycle = s.cycles;
}
