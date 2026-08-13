#pragma once

// ovrwin by Zac Walker
//
// A small C compiler that targets 16-bit real mode and emits a DOS .COM. It
// exists so the DOS side of this project can be written in C rather than in
// hand-assembled bytes.
//
// The shape is borrowed from Fabrice Bellard's Tiny C Compiler: one pass, no
// syntax tree, code generated as the parser goes, with a tiny value
// abstraction standing in for a register allocator. None of TCC's code is used
// here - it is LGPL and this is not - only the idea that a useful C compiler
// does not need an intermediate representation.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "target.h"

// The compiler never opens a file. A caller supplies source text the same way
// a host supplies the emulated drive.
class cc_sources
{
public:
	virtual ~cc_sources() = default;

	// Returns the text of an included file, or false if it cannot be found.
	virtual bool load(const char* name, std::string& text) = 0;
};

struct cc_result
{
	bool ok = false;
	std::vector<uint8_t> image; // a DOS .COM image, loaded at offset 0100h
	std::string message;
	std::string file;
	int line = 0;
	int code_size = 0;
	int data_size = 0;

	// Where the code went, once the layout pass has settled it. This is what a
	// multi-segment build has to divide up.
	std::vector<std::pair<std::string, int>> functions;
};

// What the compiler is allowed to do to the program as a whole. Every one of
// these needs the whole program in memory at once, which is what compiling a
// project as a single translation unit gives.
struct cc_options
{
	// How big a body may be, in tokens, and still be copied to its call site
	// rather than called. Zero turns it off.
	int inline_limit = 24;

	// Leave out the functions nothing can reach.
	bool drop_unreachable = true;

	// Which instructions the generated code may use. A 386 still produces a
	// 16-bit .COM; it just has more to say it with.
	cc_cpu cpu = cc_cpu::i8086;

	// What to compile for. x64 produces a block of host machine code to be
	// called, not a DOS program.
	cc_machine machine = cc_machine::i8088;
};

// Each define is either NAME or NAME=VALUE, as a command line spells it.
cc_result cc_compile(const char* text, const char* name, cc_sources& sources,
                     const std::vector<std::string>& defines = {},
                     const cc_options& options = {});
