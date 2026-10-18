#pragma once

// libdos by Zac Walker
//
// COMMAND.COM, mapped rather than run: its built-in commands carried out
// natively. Two callers share them - the prompt dui comes up with, which
// writes to the emulated screen, and a program that EXECs COMMAND.COM /C,
// whose output goes through its own standard handles and so honours
// redirection.

#include <cstdint>

struct machine_state;

// Where a built-in command's output goes. `error` marks a message for the
// standard error handle; `clear` is CLS, null where there is no screen to clear.
// `confirm` puts a yes-or-no question - DEL's, before it empties a directory -
// and says whether the answer was yes.
struct command_output
{
	void (*write)(machine_state& s, const char* text, int length, bool error, void* context);
	void (*clear)(machine_state& s, void* context);
	bool (*confirm)(machine_state& s, const char* question, void* context);
	void* context;
};

// Splits a typed line into the command word and the rest. The word ends where
// DOS ends it, so "echo." is ECHO with "." and "cd\" is CD with "\".
void command_split(const char* line, char* command, int command_size, const char** argument);

// Runs a built-in command. Returns false if `name` is not one, so the caller
// can look for a program instead; `exit_code` is the errorlevel it leaves.
bool command_builtin(machine_state& s, const char* name, const char* argument, const command_output& out,
                     int& exit_code);

enum class command_result
{
	done, // a built-in or a native program ran, and the return code is set
	started, // a program was started as a child of the caller
	failed, // nothing could be done; `error` is the DOS error
	not_a_command, // no /C in the tail: the caller loads COMMAND.COM itself
};

// INT 21h AH=4Bh of COMMAND.COM: carries out the /C command line in `tail`
// for the program that made the call. `environment` is the block it passed
// for COMMAND.COM, 0 for its own.
command_result command_exec(machine_state& s, const char* tail, uint16_t environment, uint8_t& error);
