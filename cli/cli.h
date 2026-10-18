#pragma once

// libdos by Zac Walker
//
// What demu's two halves share: the -- commands in main.cpp, and the program
// runner in runner.cpp.

// Describes a DOS executable: the MZ header, or that it is a COM file.
int show_info(const char* path);

// demu [options] program [arguments]: runs a DOS program to its end with its
// console on the host's standard streams, and returns its exit code.
int run_dos_program(int argc, char** argv);

// The runner's half of the usage text.
void runner_usage();
