#pragma once

// libdos by Zac Walker
//
// A minimal command interpreter. It is not a DOS program - there is nowhere to
// load one from before a drive is mounted - but it uses the same DOS services
// a program would, so what it lists is exactly what a program will see.

#include <cstdint>

struct machine_state;

struct shell_state
{
	bool active = false; // the shell owns the screen and keyboard
	bool program_running = false;
	char line[128] = {};
	int length = 0;
	char program[64] = {}; // name of the program that is running, for the host
	// A command's question - DEL's "Are you sure" - waits for the next line
	// typed; a yes runs `asked` again with `answered` set.
	bool asking = false;
	bool answered = false;
	char asked[128] = {};
};

// Clears the screen, prints the banner and shows a prompt.
void shell_start(machine_state& s);

// Consumes whatever is in the keyboard buffer. Called once per frame while the
// shell is idle.
void shell_tick(machine_state& s);

// Loads and starts a program, as typing its name would. A program that is found
// but will not load is reported on the screen, and the shell stays at its
// prompt without showing it again; that is left to the caller.
bool shell_run(machine_state& s, const char* dos_path, const char* command_line);

// Called when the program the shell started has terminated.
void shell_program_finished(machine_state& s);

// Shows the prompt for the current directory.
void shell_prompt(machine_state& s);
