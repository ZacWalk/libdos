#pragma once

// ovrwin by Zac Walker
//
// What the front end has to know about the machine it is compiling for, and
// nothing else. Everything here is a question the parser genuinely has to ask -
// how big is an int, how far can a call reach - as opposed to a decision about
// which instruction to use, which belongs to the back end.
//
// The three targets are one instruction set at three widths, which is what
// makes a shared front end worth having: 8088 and 386 differ in whether 32-bit
// arithmetic is native and in how many registers there are, not in what a C
// program means.

enum class cc_machine
{
	i8088, // 16-bit, four general registers, 32-bit arithmetic through helpers
	i386, // 32-bit, eight general registers, scaled index addressing
	x64, // 64-bit, sixteen general registers
};

struct cc_target
{
	cc_machine machine = cc_machine::i8088;

	int int_size = 2;
	int long_size = 4;
	int pointer_size = 2;

	// The widest operation the machine does without a runtime helper.
	int native_width = 2;

	// General registers the code generator may use.
	int registers = 8;

	// Where the image loads, and how much room to leave above it. A .COM is
	// one segment with the stack on top of the data.
	int code_origin = 0x100;
	int stack_size = 2048;
	int address_limit = 0xfff0;

	static cc_target for_machine(cc_machine m)
	{
		cc_target t;
		t.machine = m;
		switch (m)
		{
		case cc_machine::i386:
			t.int_size = 4;
			t.pointer_size = 4;
			t.native_width = 4;
			t.code_origin = 0;
			t.address_limit = 0x7fffffff;
			break;
		case cc_machine::x64:
			t.int_size = 4;
			t.pointer_size = 8;
			t.native_width = 8;
			t.registers = 16;
			t.code_origin = 0;
			t.address_limit = 0x7fffffff;
			break;
		default:
			break;
		}
		return t;
	}
};
