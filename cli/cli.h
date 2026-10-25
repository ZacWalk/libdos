#pragma once

// libdos by Zac Walker
//
// What demu's parts share: the -- commands in main.cpp, the program runner in
// runner.cpp and boot mode in boot.cpp.

// Describes a DOS executable: the MZ header, or that it is a COM file.
int show_info(const char* path);

// demu [options] program [arguments]: runs a DOS program to its end with its
// console on the host's standard streams, and returns its exit code.
int run_dos_program(int argc, char** argv);

// The runner's half of the usage text.
void runner_usage();

// demu --boot: boot mode, headless. Attaches disk images, powers on, and
// carries out --expect and --type steps against the console; returns 0 when
// they have all happened.
int run_boot(int argc, char** argv);

// Its half of the usage text.
void boot_usage();
