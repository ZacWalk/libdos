# ovrwin

Runs DOS games on modern Windows. Point it at a folder with a DOS executable
and play.

ovrwin is not a PC emulator with DOS installed on it. It emulates the hardware
a game talks to - the CPU, the VGA, the timer, the AdLib - and *maps* the
software interfaces, INT 21h and the BIOS, onto the host the way Wine maps the
Win32 API onto Linux. There is no DOS kernel in memory and no disk image: the
folder you open is the emulated C: drive.

Repository: <https://github.com/ZacWalk/ovrwin>

## What is emulated

| Part | Coverage |
|------|----------|
| CPU | 8086 and 80186: every documented opcode and addressing mode, the segment wrap, BCD arithmetic, and string instructions that resume correctly after an interrupt |
| Memory | The full 1 MiB real-mode space with the MCB chain, PSP and environment block a program expects to walk |
| VGA / EGA / CGA | 256 KiB of display memory with the graphics controller's four write modes, latches, set/reset and bit mask; text modes, CGA 4/5/6, EGA 0Dh/0Eh/10h, VGA 12h and 13h; the 256-entry DAC and the EGA-compatible palette |
| Timer | 8253 channels 0 and 2. A game that reprograms channel 0 for a music tick gets IRQ 0 at that rate, not at 18.2 Hz |
| Interrupts | Two 8259s with real priority and masking, driving INT 8 and INT 9 through the vector table so a program can hook or chain them |
| Keyboard | 8042 controller with scancodes, the BIOS translation layer and the BIOS keyboard buffer, both reachable |
| Sound | AdLib / OPL2: nine channels, two operators each, four waveforms, envelopes, feedback and both connection modes. PC speaker driven from timer channel 2 |
| Mouse | INT 33h driver with position, buttons, ranges, motion counters and an on-screen pointer |
| DOS | INT 21h: console, handle-based file I/O, directory search, memory allocation, the DTA, interrupt vectors, dates and times, `EXEC` for child processes, and the MZ/COM loader with relocation |

It also comes with a C compiler that targets 16-bit DOS, and a shell written in
C and built with it - see [Building DOS programs](#building-dos-programs).

How it all fits together is in [docs/design.md](docs/design.md). What is missing
or approximate is in [docs/todo.md](docs/todo.md) - headline gaps are Sound
Blaster digital audio, `EXEC` for child processes, and the 386 instruction set
(so a DOS extender will not run).

## Layout

| Path | Contents |
|------|----------|
| `src/` | The emulator. Platform independent - no Windows headers, no file system. Builds `ovrwin.lib`. |
| `host/` | The Windows implementation of the host interface: one directory presented as the emulated drive. |
| `app/` | Win32 app: a window, a DIB blit, waveOut audio and the key mapping. |
| `cli/` | Command line driver: the test suite, plus tools to inspect, disassemble, trace and headlessly run a program. |
| `cc/` | A C compiler for 16-bit DOS. Platform independent, and it opens no files either. `cc/include` and `cc/lib` are the C library it compiles against. |
| `shell/` | `dos.h` and a DOS command interpreter written in C, built by `cc/`. |
| `tools/` | `make-font.ps1`, which regenerates the character generator in `src/rom-font.cpp`. |
| `docs/` | [design.md](docs/design.md) (architecture) and [todo.md](docs/todo.md) (gaps). |

## Build and run

```powershell
.\dd.ps1 test   # build + run the test suite
.\dd.ps1 run    # build + launch the app
.\dd.ps1 bench  # compile the benchmark for this machine and run it here
.\dd.ps1 cli --info .\GAME\GAME.EXE
```

Or with MSBuild directly:

```powershell
msbuild ovrwin.sln /m /p:Configuration=Release /p:Platform=x64
```

Binaries land in `bin/` as `<name><64|32><r|d>` - architecture then
configuration, e.g. `ovrwin64d.lib`, `ovrwinapp64r.exe`, `ovrwincli64d.exe`.
Intermediate artifacts go under `intermediate/`.

## The app

Drop a program or a folder onto the window, open one through **File**, or pass
one on the command line. Whichever you do, the folder becomes the emulated C:
drive and nothing outside it is reachable.

With no program running you get a command prompt, so you can look around before
starting anything:

```
C:\>dir
C:\>cd wasteland
C:\>wl
```

`DIR`, `CD`, `MD`, `RD`, `DEL`, `TYPE`, `CLS`, `VER`, `MEM`, `HELP` and `EXIT`
are built in; anything else is treated as the name of a program to run, with
`.COM` tried before `.EXE`. When the program ends you are back at the prompt.
The prompt is not a DOS program - it uses the same host services INT 21h does,
so what `DIR` lists is exactly what a program will find.

The title bar reports the program, the current video mode and whether the
machine is paused.

| | |
|---|---|
| Ctrl+O / Ctrl+R | Open, Reset back to the prompt |
| F9 | Pause |
| View menu | Sound on/off |

The picture is scaled by a whole number and centred, so a pixel is always a
solid block and never an uneven one. Each axis gets its own factor, bounded so
a pixel is at least square and at most twice as tall as it is wide - roughly
its shape on the display a 200-line mode was written for.

Emulation is paced by the sound card: each frame's audio is queued to waveOut,
and waiting for a free buffer is what keeps the machine running at real time.

## Building DOS programs

`cc/` is a small C compiler that emits a DOS `.COM`. It exists because the DOS
side of an emulator is much easier to write in C than in hand-assembled bytes,
and because a program compiled here and run there exercises the whole emulator
from one end to the other.

```powershell
.\bin\ovrwincli64d.exe --cc .\shell\hello.c        # writes hello.COM
.\bin\ovrwincli64d.exe --run .\shell\hello.COM --screen
```

A program that wants the C library includes it, because there is nothing to
link with:

```powershell
.\bin\ovrwincli64d.exe --cc prog.c -I cc\include -I cc\lib -D NDEBUG
```

It is one pass with no syntax tree, in the shape of Fabrice Bellard's Tiny C
Compiler - the parser emits 8086 code as it goes. None of TCC's code is used;
it is LGPL and this is not. It accepts most of C89: 32-bit `long`, function
pointers, `goto`, varargs, `static` locals and constant folding over both
integers and floating literals. What it does not have is run-time floating
point, struct assignment, and any memory model beyond the tiny one - a program
is a single `.COM` in a single 64 KiB segment, and there is no linker, so
`cc/lib/libc.c` is included rather than linked. The gaps are listed in
[docs/todo.md](docs/todo.md).

`shell/shell.c` is a DOS command interpreter built with it, following
COMMAND.COM's command set. It runs as a real DOS program inside the emulator
and launches others through INT 21h `EXEC`:

```powershell
.\bin\ovrwincli64d.exe --cc .\shell\shell.c
copy .\shell\shell.COM .\GAME\
.\bin\ovrwincli64d.exe --run .\GAME\SHELL.COM --args "dir" --screen
```

The prompt the app comes up with is a separate, native one: it has to work
before any drive is mounted, and it is there whether or not you have built
anything.

## The command line

`ovrwincli64d.exe --test` runs the behavioural test suite and returns non-zero
on failure. The same binary is the debugger:

```powershell
.\bin\ovrwincli64d.exe --info GAME.EXE                  # MZ header, memory needed
.\bin\ovrwincli64d.exe --cc program.c [-I dir] [-D name=value]  # compile C to a DOS .COM
.\bin\ovrwincli64d.exe --disasm GAME.EXE 40             # disassemble from the entry point
.\bin\ovrwincli64d.exe --run GAME.EXE --frames 400 --bmp shot.bmp
.\bin\ovrwincli64d.exe --run GAME.EXE --trace           # every DOS and BIOS call
.\bin\ovrwincli64d.exe --run GAME.EXE --trace-tail 200  # the last instructions executed
.\bin\ovrwincli64d.exe --run GAME.EXE --screen --state  # text screen, registers, memory
.\bin\ovrwincli64d.exe --run GAME.EXE --keys " \n" --key-delay 240 --bmp shot.bmp
.\bin\ovrwincli64d.exe --run GAME.EXE --wav sound.wav    # what came out of the speaker
.\bin\ovrwincli64d.exe --run .\GAME --args "dir" --screen  # mount a folder, use the shell
.\bin\ovrwincli64d.exe --bench GAME.EXE --seconds 3     # guest instructions a second
```

`--trace` and `--trace-tail` are how you find out why a program gave up: the
first shows which service it asked for and what it got back, the second shows
the instructions it ran on the way there. `--keys` types on the emulated
keyboard as the program runs, which is what gets a game past its title screen
without the app.

## Embedding

Add a project reference to `src/ovrwin.vcxproj`, include `src/machine.h`, and
link the matching `ovrwin<arch><cfg>.lib`. Implement `host_services` from
`src/host.h` to decide what the emulated drive is, then feed the machine an
image (`load_program`), step it with `exec(cycles)`, and read `framebuffer()`
and `generate_audio()`. `app/app.cpp` is a complete worked host.

Contributing, or working on this with an AI agent? See [AGENTS.md](AGENTS.md).
