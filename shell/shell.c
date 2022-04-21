/* ovrwin by Zac Walker
 *
 * A DOS command interpreter, compiled by ovrwincc and run as a real DOS
 * program inside the emulator. The command set follows COMMAND.COM: the
 * internal commands are the ones in its COMTAB, and an external command is
 * looked for as .COM then .EXE, exactly as DOS does.
 */

#include "dos.h"

char line[132];
char command[32];
char argument[104];
char path[132];
char tail[132];
char cwd[80];
int running;

/* ---- helpers ---- */

void pad_to(int column, int from)
{
	while (from < column)
	{
		putch(' ');
		from = from + 1;
	}
}

void show_prompt(void)
{
	cwd[0] = 0;
	dos_getcwd(cwd);
	puts("C:");
	if (cwd[0] == 0) puts("\\");
	else puts(cwd);
	puts(">");
}

int read_line(void)
{
	int length;

	line[0] = 127;
	line[1] = 0;
	__int21(0x0a00, 0, 0, (int)line);
	length = line[1] & 255;
	line[2 + length] = 0;
	puts("\n");
	return length;
}

void split_line(void)
{
	char *p;
	int n;

	p = line + 2;
	while (*p == ' ' || *p == 9) p = p + 1;

	n = 0;
	while (*p != 0 && *p != ' ' && *p != 9 && n < 31)
	{
		command[n] = *p;
		n = n + 1;
		p = p + 1;
	}
	command[n] = 0;

	while (*p == ' ' || *p == 9) p = p + 1;
	n = 0;
	while (*p != 0 && n < 103)
	{
		argument[n] = *p;
		n = n + 1;
		p = p + 1;
	}
	argument[n] = 0;
}

/* ---- internal commands ---- */

void command_dir(void)
{
	int files;
	int directories;
	int column;
	char *name;

	if (argument[0] == 0) strcpy(path, "*.*");
	else strcpy(path, argument);

	if (!dos_find_first(path, ATTR_DIRECTORY))
	{
		putln("File not found");
		return;
	}

	files = 0;
	directories = 0;
	putln("");
	while (1)
	{
		name = DTA_NAME;
		puts(name);
		column = strlen(name);
		pad_to(14, column);

		if ((*DTA_ATTRIBUTES & ATTR_DIRECTORY) != 0)
		{
			puts("<DIR>");
			directories = directories + 1;
		}
		else
		{
			putu32(*DTA_SIZE_LOW, *DTA_SIZE_HIGH);
			files = files + 1;
		}
		putln("");
		if (!dos_find_next()) break;
	}

	putln("");
	putu(files);
	puts(" file(s), ");
	putu(directories);
	putln(" dir(s)");
}

void command_type(void)
{
	int handle;
	int got;
	int at;
	char buffer[128];

	if (argument[0] == 0)
	{
		putln("Required parameter missing");
		return;
	}

	handle = dos_open(argument, 0);
	if (__cf != 0)
	{
		putln("File not found");
		return;
	}

	while (1)
	{
		got = dos_read(handle, buffer, 128);
		if (got <= 0) break;
		at = 0;
		while (at < got)
		{
			if (buffer[at] == 26) break;
			putch(buffer[at]);
			at = at + 1;
		}
		if (at < got) break;
	}
	dos_close(handle);
	putln("");
}

void command_mem(void)
{
	unsigned paragraphs;

	/* AH=48h with a request DOS cannot meet reports the largest free block,
	 * in paragraphs. __bx has to be taken before anything else calls DOS. */
	__int21(0x48ff, 0xffff, 0, 0);
	paragraphs = (unsigned)__bx;

	puts("largest free block: ");
	putu32(paragraphs << 4, paragraphs >> 12);
	putln(" bytes");
}

void command_help(void)
{
	putln("");
	putln("  DIR [pattern]   list files          CLS     clear the screen");
	putln("  CD [path]       change directory    VER     version");
	putln("  MD path         make a directory    MEM     free memory");
	putln("  RD path         remove a directory  ECHO t  print text");
	putln("  DEL file        delete a file       PAUSE   wait for a key");
	putln("  TYPE file       show a file         EXIT    leave the shell");
	putln("");
	putln("  Any other name is run as a program, .COM before .EXE.");
	putln("");
}

/* ---- external commands ---- */

int has_extension(char *name)
{
	while (*name != 0)
	{
		if (*name == '.') return 1;
		name = name + 1;
	}
	return 0;
}

int start(char *program)
{
	int length;

	length = strlen(argument);
	tail[0] = (char)length;
	strcpy(tail + 1, argument);
	tail[1 + length] = 13;

	dos_exec(program, tail);
	return __cf == 0;
}

int run_external(void)
{
	if (has_extension(command))
	{
		strcpy(path, command);
		return start(path);
	}

	strcpy(path, command);
	strcat(path, ".COM");
	if (start(path)) return 1;

	strcpy(path, command);
	strcat(path, ".EXE");
	if (start(path)) return 1;
	return 0;
}

/* ---- dispatch ---- */

void execute(void)
{
	if (command[0] == 0) return;

	if (match(command, "dir")) { command_dir(); return; }
	if (match(command, "type")) { command_type(); return; }
	if (match(command, "mem")) { command_mem(); return; }
	if (match(command, "help") || match(command, "?")) { command_help(); return; }
	if (match(command, "rem")) return;

	if (match(command, "cls"))
	{
		__int10(0x0003, 0, 0, 0);
		return;
	}
	if (match(command, "ver"))
	{
		putln("ovrwin shell, DOS 5.00 compatible");
		return;
	}
	if (match(command, "echo"))
	{
		putln(argument);
		return;
	}
	if (match(command, "pause"))
	{
		puts("Press any key to continue . . .");
		getch();
		putln("");
		return;
	}
	if (match(command, "exit"))
	{
		running = 0;
		return;
	}
	if (match(command, "cd") || match(command, "chdir"))
	{
		if (argument[0] == 0)
		{
			cwd[0] = 0;
			dos_getcwd(cwd);
			puts("C:");
			if (cwd[0] == 0) putln("\\");
			else putln(cwd);
			return;
		}
		if (!dos_chdir(argument)) putln("Invalid directory");
		return;
	}
	if (match(command, "md") || match(command, "mkdir"))
	{
		if (!dos_mkdir(argument)) putln("Unable to create directory");
		return;
	}
	if (match(command, "rd") || match(command, "rmdir"))
	{
		if (!dos_rmdir(argument)) putln("Unable to remove directory");
		return;
	}
	if (match(command, "del") || match(command, "erase"))
	{
		if (!dos_delete(argument)) putln("File not found");
		return;
	}

	if (!run_external()) putln("Bad command or file name");
}

int main(void)
{
	int length;
	int at;

	/* The command tail has to be taken before anything uses the disk transfer
	 * area, which shares the same address. */
	length = *TAIL_LENGTH & 255;
	at = 0;
	while (at < length && at < 103)
	{
		argument[at] = TAIL_TEXT[at];
		at = at + 1;
	}
	argument[at] = 0;

	putln("");
	putln("ovrwin shell - type HELP for a list of commands");
	putln("");

	if (argument[0] != 0)
	{
		strcpy(line + 2, argument);
		split_line();
		execute();
	}

	running = 1;
	while (running)
	{
		show_prompt();
		read_line();
		split_line();
		execute();
	}
	return 0;
}
