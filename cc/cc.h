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
#include <vector>

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
};

// Each define is either NAME or NAME=VALUE, as a command line spells it.
cc_result cc_compile(const char* text, const char* name, cc_sources& sources,
                     const std::vector<std::string>& defines = {});
