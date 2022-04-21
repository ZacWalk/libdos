// ovrwin by Zac Walker
//
// 8253 timer, 8259 interrupt controllers and the 8042 keyboard controller.
// The timer is the important one: DOS games reprogram channel 0 to get a
// music or animation tick, so its rate has to drive IRQ 0 rather than a
// hard-coded 18.2 Hz.

#include "machine.h"

#include <cstring>

void pit_reset(pit_state& p)
{
	p = pit_state();
}

namespace
{
	uint32_t channel_period(const pit_channel& c)
	{
		return c.reload == 0 ? 0x10000u : c.reload;
	}
}

void pit_step(machine_state& s, uint64_t cycles)
{
	pit_state& p = s.pit;
	p.remainder += cycles * pit_frequency;
	const uint64_t ticks = p.remainder / s.cpu_hz;
	if (ticks == 0) return;
	p.remainder -= ticks * s.cpu_hz;
	p.ticks += ticks;

	for (int i = 0; i < 3; ++i)
	{
		pit_channel& c = p.channel[i];
		if (i == 2 && !c.gate) continue;

		const uint32_t period = channel_period(c);
		uint64_t remaining = ticks;
		while (remaining > 0)
		{
			if (remaining < c.counter)
			{
				c.counter -= uint32_t(remaining);
				break;
			}
			remaining -= c.counter;
			c.counter = period;
			if (i == 0) pic_raise(s, 0);
			if (c.mode == 0 || c.mode == 1) break; // one-shot: fires once
		}
	}
}

uint8_t pit_read(machine_state& s, uint16_t port)
{
	pit_state& p = s.pit;
	const int index = port & 3;
	if (index > 2) return 0xff;
	pit_channel& c = p.channel[index];

	const uint16_t value = c.latch_pending ? c.latched : uint16_t(c.counter);
	uint8_t result;
	switch (c.access)
	{
	case 1: result = uint8_t(value);
		break;
	case 2: result = uint8_t(value >> 8);
		break;
	default:
		result = c.read_high ? uint8_t(value >> 8) : uint8_t(value);
		c.read_high = !c.read_high;
		if (!c.read_high) c.latch_pending = false;
		return result;
	}
	c.latch_pending = false;
	return result;
}

void pit_write(machine_state& s, uint16_t port, uint8_t value)
{
	pit_state& p = s.pit;
	const int index = port & 3;

	if (index == 3)
	{
		const int channel = (value >> 6) & 3;
		if (channel == 3) return; // read-back command, not modelled
		pit_channel& c = p.channel[channel];
		const int access = (value >> 4) & 3;
		if (access == 0)
		{
			c.latched = uint16_t(c.counter);
			c.latch_pending = true;
			c.read_high = false;
			return;
		}
		c.access = uint8_t(access);
		c.mode = uint8_t((value >> 1) & 7);
		c.bcd = (value & 1) != 0;
		c.write_high = false;
		c.read_high = false;
		c.latch_pending = false;
		return;
	}

	pit_channel& c = p.channel[index];
	switch (c.access)
	{
	case 1:
		c.reload = uint16_t((c.reload & 0xff00) | value);
		break;
	case 2:
		c.reload = uint16_t((c.reload & 0x00ff) | (value << 8));
		break;
	default:
		if (!c.write_high)
		{
			c.reload = uint16_t((c.reload & 0xff00) | value);
			c.write_high = true;
			return;
		}
		c.reload = uint16_t((c.reload & 0x00ff) | (value << 8));
		c.write_high = false;
		break;
	}
	c.counter = channel_period(c);
	if (index == 2) sound_queue_write(s, 0x42, c.reload);
}

void pic_reset(pic_state& p)
{
	p = pic_state();
	p.master.base = 0x08;
	p.slave.base = 0x70;
}

void pic_raise(machine_state& s, int irq)
{
	if (irq < 8) s.pic.master.irr = uint8_t(s.pic.master.irr | (1 << irq));
	else s.pic.slave.irr = uint8_t(s.pic.slave.irr | (1 << (irq - 8)));
}

void pic_lower(machine_state& s, int irq)
{
	if (irq < 8) s.pic.master.irr = uint8_t(s.pic.master.irr & ~(1 << irq));
	else s.pic.slave.irr = uint8_t(s.pic.slave.irr & ~(1 << (irq - 8)));
}

namespace
{
	int highest_pending(const pic_chip& chip)
	{
		const uint8_t ready = uint8_t(chip.irr & ~chip.imr);
		for (int i = 0; i < 8; ++i)
		{
			const uint8_t bit = uint8_t(1 << i);
			if ((chip.isr & bit) != 0) return -1; // in service blocks lower priority
			if ((ready & bit) != 0) return i;
		}
		return -1;
	}
}

int pic_pending_vector(machine_state& s)
{
	const int master = highest_pending(s.pic.master);
	if (master < 0) return -1;
	if (master != 2) return s.pic.master.base + master;

	const int slave = highest_pending(s.pic.slave);
	if (slave < 0) return -1;
	return s.pic.slave.base + slave;
}

void pic_acknowledge(machine_state& s, int vector)
{
	if (vector >= s.pic.slave.base && vector < s.pic.slave.base + 8)
	{
		const int irq = vector - s.pic.slave.base;
		s.pic.slave.irr = uint8_t(s.pic.slave.irr & ~(1 << irq));
		s.pic.slave.isr = uint8_t(s.pic.slave.isr | (1 << irq));
		s.pic.master.isr = uint8_t(s.pic.master.isr | 0x04);
		return;
	}
	const int irq = vector - s.pic.master.base;
	if (irq < 0 || irq > 7) return;
	s.pic.master.irr = uint8_t(s.pic.master.irr & ~(1 << irq));
	s.pic.master.isr = uint8_t(s.pic.master.isr | (1 << irq));
}

namespace
{
	void end_of_interrupt(pic_chip& chip, int specific)
	{
		if (specific >= 0)
		{
			chip.isr = uint8_t(chip.isr & ~(1 << specific));
			return;
		}
		for (int i = 0; i < 8; ++i)
		{
			const uint8_t bit = uint8_t(1 << i);
			if ((chip.isr & bit) != 0)
			{
				chip.isr = uint8_t(chip.isr & ~bit);
				return;
			}
		}
	}

	void write_chip(pic_chip& chip, uint16_t port, uint8_t value)
	{
		if ((port & 1) == 0)
		{
			if ((value & 0x10) != 0)
			{
				chip.init_step = 1;
				chip.imr = 0;
				chip.isr = 0;
				return;
			}
			if ((value & 0x08) != 0)
			{
				chip.read_isr = (value & 0x03) == 0x03;
				return;
			}
			if ((value & 0x20) != 0) end_of_interrupt(chip, (value & 0x40) != 0 ? (value & 7) : -1);
			return;
		}

		switch (chip.init_step)
		{
		case 1:
			chip.base = uint8_t(value & 0xf8);
			chip.init_step = 2;
			break;
		case 2:
			chip.init_step = 3;
			break;
		case 3:
			chip.auto_eoi = (value & 0x02) != 0;
			chip.init_step = 0;
			break;
		default:
			chip.imr = value;
			break;
		}
	}
}

uint8_t pic_read(machine_state& s, uint16_t port)
{
	pic_chip& chip = port < 0xa0 ? s.pic.master : s.pic.slave;
	if ((port & 1) != 0) return chip.imr;
	return chip.read_isr ? chip.isr : chip.irr;
}

void pic_write(machine_state& s, uint16_t port, uint8_t value)
{
	write_chip(port < 0xa0 ? s.pic.master : s.pic.slave, port, value);
}

void kbd_push_scancode(machine_state& s, uint8_t code)
{
	keyboard_controller& k = s.kbd;
	if (k.queue_count >= int(sizeof(k.queue))) return;
	k.queue[k.queue_count++] = code;
	k.status = uint8_t(k.status | 0x01);
	pic_raise(s, 1);
}

uint8_t kbd_read(machine_state& s, uint16_t port)
{
	keyboard_controller& k = s.kbd;
	if (port == 0x64) return k.status;

	if (k.queue_count > 0)
	{
		k.output = k.queue[0];
		std::memmove(k.queue, k.queue + 1, size_t(--k.queue_count));
		if (k.queue_count == 0)
		{
			// The controller drops IRQ 1 once its output buffer is empty.
			k.status = uint8_t(k.status & ~0x01);
			pic_lower(s, 1);
		}
		else
		{
			pic_raise(s, 1);
		}
	}
	return k.output;
}

void kbd_write(machine_state& s, uint16_t port, uint8_t value)
{
	keyboard_controller& k = s.kbd;
	if (port == 0x64)
	{
		k.command = value;
		k.command_pending = true;
		return;
	}
	if (k.command_pending)
	{
		k.command_pending = false;
		return;
	}
	// Commands the keyboard itself answers: acknowledge everything.
	if (value == 0xff || value == 0xf4 || value == 0xf5 || value == 0xed)
	{
		k.queue_count = 0;
		kbd_push_scancode(s, 0xfa);
	}
}
