# libdos design

How the emulator is put together and why. For what is *not* built yet, see
[todo.md](todo.md). For build and usage, see the [README](../README.md).

## Shape

The pieces, with a hard boundary between `src/` and the rest:

| | |
|---|---|
| `src/` | The emulator. Platform independent: no Windows headers, no file system, no threads. Builds `libdos`. |
| `host/` | The Windows side of `host_services`: one directory presented as the emulated drive. |
| `app/` | dui, the libdesktop host. Owns the window, picture, sound and menus through `pf::`. |
| `cli/` | demu, a console harness that drives the library headlessly and doubles as the debugger; the runner that builds MS-DOS with its 1988 toolchain (`runner.cpp`, with LIB's stand-in in `lib-omf.c`); and boot mode's headless runner (`boot.cpp`). |
| `cc/` | A C compiler targeting 16-bit DOS. Platform independent, and like `src/` it opens no files. |
| `shell/` | A DOS command interpreter written in C and built by `cc/`. |
| `disk/` | Bootable DOS disk images and a reader for them. Platform independent, and it opens no files. |
| `dimg/` | dimg, the command line for `disk/`. |

`machine` is the embeddable façade; `machine_state` is the whole machine as
plain data. `machine` owns exactly one `machine_state` and is non-copyable.

The library never opens a file. `host_services` (in `src/host.h`) is the only
way out: a host implements it and decides what `C:\` means. That is what makes
the emulator embeddable and what stops a program reaching outside its folder.
Boot mode adds one more door of the same kind: the disks are `block_device`s
(`src/block-device.h`) that a host attaches, and where their sectors live is
again the host's business.

## Mapping, not emulating

There is no DOS in memory. `dos.cpp` implements the *interface* DOS presents -
INT 21h, the PSP, the MCB chain, the environment block - and hands file work to
the host. This is the same trade Wine makes: you get a program's behaviour
without a copy of the operating system, and you lose anything that depends on
DOS internals rather than its documented interface.

The line is drawn at what programs can observe:

- Things programs read directly are held in **emulated memory**, not C++
  fields: the MCB chain, the PSP, the BIOS data area, the keyboard buffer, the
  timer tick at `0040:006C`. A program that walks the MCB chain finds a real
  one.
- Things only reachable through a documented call are held in C++ (the file
  handle table, the search contexts).

### Boot mode

The one exception, for testing a DOS rather than running programs on one.
`power_on` sets `machine_state::boot_mode`, and a reset then builds an IBM PC
5150 instead: the BIOS's vectors up to INT 1Fh and nothing above them, the
ROM's model byte (`FF`) and date, the diskette parameter table at `F000:EFC7`,
and the reset vector at `F000:FFF0` holding a callback that powers the machine
off and on - which is where FDISK's restart, a far jump to `FFFF:0000`, lands.
`dos_reset` is not run and `machine_callback` never reaches `dos_interrupt`:
the DOS on the disk installs its own INT 21h, and a vector it has not set is
zero, as on the real machine.

The BIOS is libdos's own and stays high-level. INT 13h serves drives a host
attaches as `block_device`s - two floppies, as an XT has them, and two hard
disks as an XT-IDE card's BIOS presents them: CHS with up to 256 heads, plus
the LBA calls when the drive is given them, 127 sectors at most a call as EDD
and the XT-IDE Universal BIOS allow - and INT 19h reads the boot drive's first
sector to `0000:7C00` and jumps to it. Nothing is timed: a sector arrives the
moment it is asked for, so a whole FORMAT runs in a second. A host that wants
to watch the console has `video_text`, every character through INT 10h in
order, which is what demu's `--expect` reads. The power-on callback is only
answered in boot mode: a program under the mapped DOS that runs into it does
not get a machine without its DOS.

## The callback opcode

Every interrupt vector the emulator services points at a stub in a reserved
page at `F000:E000`. A stub is three bytes: the callback opcode `F1`, the
vector number, and `IRET`. When the CPU meets `F1` it calls
`machine_callback`, which dispatches to the DOS or BIOS layer, and then the
`IRET` returns to the program.

Trapping at the *vector* rather than at the `INT` instruction is the point:

- A program can read the vector with AH=35h, keep it, install its own handler,
  and chain to ours. That works because "ours" is a real address holding real
  instructions.
- A program can take a vector over completely and never reach us, which is
  exactly what should happen.
- The INT 8 stub is longer, because the BIOS timer handler is: it updates the
  tick count, chains to INT 1Ch with a real `CD 1C`, and only then
  acknowledges the interrupt.

A service that has to return a status in the flags - most of INT 21h - calls
`cpu_set_iret_flag`, which rewrites the flags word the `IRET` will pop.

A service that has to *block*, such as INT 16h AH=00h with no key waiting,
rewinds the return address by the two bytes of the `INT` instruction and
returns. The program asks again on its next instruction, and the machine keeps
running in the meantime. Nothing in the emulator ever waits.

## Execution model

Everything is driven from `cpu_step(machine_state&)`, which executes one
instruction and returns the cycles it cost. `machine::exec(budget)` loops until
the budget is spent, advancing the timer by exactly the cycles used, then
renders one frame.

`cpu_step` executes **one repetition** of a string instruction, so an interrupt
can land in the middle of a `REP` as it does on hardware. When that happens the
return address is rewound to the first byte of the instruction, prefixes
included, so the `IRET` resumes the block move with whatever `CX` is left.
Getting this wrong does not crash anything - it silently truncates block moves,
which is how it shows up as "packed file is corrupt" in a self-extracting
executable.

There is no scheduler and no event queue. Chips see time only at instruction
boundaries; in exchange the whole machine is one function call with no
allocation.

## Memory

`mem_read8` / `mem_write8` are the only path to memory. Both are a range test:
`A0000-BFFFF` belongs to the display adapter, `C0000` and above is ROM, and
everything else is the flat 1 MiB array. Addresses are masked to 20 bits, so a
segment:offset pair at the top of memory wraps the way an 8086 without an A20
gate does.

## CPU

`src/cpu.cpp` is a switch-per-opcode interpreter over `machine_state::cpu`,
derived from Andrew Jenner's reenigne 8086 core and extended with the 80186
instructions late-80s games assume: `PUSHA`/`POPA`, `PUSH imm`, `IMUL imm`,
`ENTER`/`LEAVE`, `INS`/`OUTS`, `BOUND`, and shifts by an immediate count (with
the 186's 5-bit count mask).

The 80386's two-byte opcodes are there too, in their 16-bit forms, because a
16-bit program built for a 386 is full of them: near `Jcc`, `SETcc`,
`MOVZX`/`MOVSX`, `BT`/`BTS`/`BTR`/`BTC`, `BSF`/`BSR`, `SHLD`/`SHRD`,
`IMUL r,rm`, `FS` and `GS` with their overrides, and `LSS`/`LFS`/`LGS`. What is
not there is the other half of a 386 - 32-bit operands and addresses, and
protected mode - so the register file is still 16 bits wide.

State that must survive between `cpu_step` calls - the opcode, the REP mode,
the segment override, the instruction's first byte - lives in `cpu_state`.
Everything else is a per-instruction `x86` struct built on the stack.

Cycle counts are approximate: a base cost per opcode class plus the effective
address and branch penalties. They are good enough to pace the timer and the
retrace status, and they are not claimed to be more than that.

### Processor models

`machine::set_cpu_model` picks what the interpreter is, and `machine_reset`
keeps it. The default, `i386`, is everything above. `i80186` drops the 386's
two-byte opcodes. The two 8086 models are for software that has to run on an
IBM PC: they **stop the machine** - `machine_fault`, with the opcode and its
CS:IP in `error()` - at any instruction an 8088 does not have, rather than
running it, because a program quietly relying on a newer chip is exactly what
testing on one is meant to catch. Where an 8086 behaves differently they behave
like one: `PUSH SP` pushes the decremented value, a shift by CL uses all of CL
rather than CL mod 32, and segment register fields 4-7 repeat ES-DS. They also
stop at an encoding that means nothing, where the default only notes it.
`i8086_strict` additionally refuses the instructions an XT with no coprocessor
has no business executing - x87 escapes, `WAIT`, `LOCK` and `HLT` - which is
what the original 1988 build tools are run under, as demu always ran them.

## Video

`vga_state` holds 256 KiB of display memory **plane-interleaved**: byte
`offset` of plane `p` is at `mem[offset * 4 + p]`. That one decision makes all
three addressing modes cheap:

- **chain 4** (mode 13h): the CPU address *is* the array index, so the
  256-colour modes are a plain linear array.
- **odd/even** (text and CGA): address bit 0 picks the plane.
- **planar** (EGA and VGA 16-colour): the address selects one offset across all
  four planes, gated by the map mask, the bit mask, the latches, set/reset and
  the ALU function. All four write modes and both read modes are implemented,
  because EGA games use every one of them.

`vga_render` turns whatever the mode is into a 32-bit frame buffer at the
mode's **native resolution**; the host scales. The library never scales, and
the host only ever scales by a whole number - a fractional factor has to drop
or double individual rows, which is exactly what makes an emulated screen look
uneven. The two axes get their own factor, bounded so a pixel is at least
square and at most twice as tall as it is wide; that fills the window for a
200-line mode without distorting one that is already 4:3.

The character generator is a 256 x 32-byte table seeded from `rom-font.cpp`
and replaceable through INT 10h AX=1100h, so a program that uploads its own
font gets it. The font itself is 8x8: codes 0-127 are the IBM PC BIOS glyphs
and the line-drawing and shading sets above 127 are generated from the edges
each character reaches, since geometry beats a traced bitmap. Text modes are
therefore 8x8 cells - 640x200 for 80x25 - rather than the VGA's 8x16, which
keeps every glyph pixel square once the host has scaled it.

`exec` calls it at the point in the budget where the emulated vertical retrace
falls, not at the end. A game that waits for retrace before drawing has
finished by then, so what the host presents is a whole frame rather than a
half-drawn one. The retrace period is the VGA's ~70 Hz while a host asks for
60 frames a second, so a budget can span two of them; `exec` samples at the
*last* retrace inside the budget, because taking the first leaves the picture
ageing by a varying fraction of a frame and that reads as judder. The host in
turn composes the picture and its letterbox into an off-screen DIB and presents
it with a single `BitBlt`, and marks itself per-monitor DPI aware so Windows
does not stretch the window as a bitmap. Both of those are the difference
between a steady picture and a flickering, soft one.

Colour goes attribute → attribute palette register → DAC. The DAC default
depends on the mode, and this matters: a VGA in a 16-colour or text mode loads
the 64 EGA colours indexed by the `rgbRGB` value in an attribute palette
register, while mode 13h loads the 256-colour ramp. Rendering an EGA game
through the 256-colour palette produces a picture that is recognisable but
completely the wrong colours.

## Timer and interrupts

`pit_step` converts CPU cycles into 1.193182 MHz ticks with a fractional
remainder, so no time is lost, and counts each channel down. Channel 0 raising
IRQ 0 at whatever rate the program programmed is the whole point: DOS games
reprogram it for music and animation, and hard-coding 18.2 Hz makes them run at
the wrong speed.

The counters are not moved on after every instruction, though: that cost a
quarter of the time a compiler took to run. `exec` adds each instruction's
cycles to `pit.pending` and calls `pit_sync` only when they reach `pit.due`,
the number of cycles until channel 0 next reaches zero. That raises IRQ 0 on
exactly the instruction stepping every time would, because stepping is
additive; and since the counters are otherwise stale, anything that looks at
them - a read or write of the timer's ports, or a change to channel 2's gate
through port 61h - calls `pit_sync` first.

The two 8259s model interrupt request, in-service and mask registers with real
priority. `cpu_step` takes a pending interrupt at an instruction boundary when
IF is set and a request is outstanding, and the vector goes through the IVT
like any other - so if a program hooked INT 8, its handler runs.

## Sound

`sound.cpp` is a YM3812 (OPL2): nine channels of two operators, four waveforms,
an envelope generator, feedback and both FM and additive connection, plus the
PC speaker driven from timer channel 2.

Register writes are **timestamped** with the machine's cycle counter and pushed
onto a queue. `sound_render` replays them against the sample clock. That is
what lets a host run a whole frame of emulation and only then ask for its
audio, without every note change snapping to a frame boundary.

The one exception is the AdLib detection sequence, which writes the timer
control register and reads the status back immediately; that path is tracked
outside the queue so it can answer at once.

Whether audio is on is the host's setting, not the machine's, so `machine_reset`
preserves it alongside the host pointer and the clock speed. It has to: the
shell resets the machine to start a program, and losing the flag there silences
everything the user actually runs.

## Command interpreter

`shell.cpp` is a small command prompt that owns the machine when no program is
running. It is deliberately **not** a DOS program: there is nowhere to load one
from before a drive is mounted, and a native one can start the first program
without an `EXEC` implementation. It writes through `vga_write_char` and reads
through `bios_pop_key`, so it behaves like a program on the machine, and it
reaches the drive through the same `host_services` calls INT 21h uses - what
`DIR` lists is exactly what a program will find.

While the shell is at its prompt `exec` does not step the CPU at all; there is
no program to run, so it only ticks the clock for the cursor blink. Starting a
program resets the machine and loads it, keeping only the current directory;
when the program terminates, `exec` sees `exited` and hands control back to the
prompt instead of ending the session.

Its built-in commands are not its own: `command.cpp` has them - COPY, DEL,
REN, TYPE, ECHO, CD, MD, RD, SET, PATH, ATTRIB, DIR, CLS, VER and the rest of
COMMAND.COM's - writing through a `command_output` the caller supplies. The
prompt's writes to the screen; the other caller's goes through a program's
standard handles, which is the next section. A command's question - DEL's
"Are you sure (Y/N)?" before it empties a directory - goes through it too:
COMMAND.COM /C reads the answer from standard input, and the prompt, which
cannot stop to wait, keeps the question open and takes the next line typed
as the answer.

## COMMAND.COM, mapped

A program that needs a shell - NMAKE does, for every command line that is not
a plain program - EXECs COMMAND.COM with `/C` and the line. There is no
COMMAND.COM to run, so `exec` hands the line to `command_exec`, which carries
it out for the caller: a built-in command runs at once, with the caller's own
handles 0 and 1 pointed at any `<`, `>` or `>>` file for as long as it takes;
a program is found the way COMMAND.COM finds one - in the current directory,
then along PATH from the environment the caller passed, `.COM` before `.EXE` -
and started as the caller's own child through `dos_start_child`, with the
redirections given to the child's handles instead. The return code of either
is what AH=4Dh then reports, and IF, FOR and CALL take the rest of their line
as a command of their own. Pipes and batch files are refused rather than
pretended.

A host can stand in for a program too. `host_services::run_native` is asked
first on every EXEC, by the program's name; demu answers for LIB, whose real
executable is an OS/2 bound one that cannot run here. Its stand-in takes each
file name the way DOS would - from the directory LIB was started in, on drive
C: - so it reaches nothing outside demu's root, and it exits 1, as LIB does,
when a file cannot be read or written or a table overflows.

## Files and handles

DOS's own shape, because programs depend on it: a **system file table** of
open files and devices (`dos_state::files`, each with a reference count), and
in every PSP a **handle table** - 20 bytes at PSP:18h, or a bigger one AH=67h
moves elsewhere - whose entries index it. A child inherits its parent's handles
(those not opened with the no-inherit bit), so redirection set up by a parent
reaches the child; AH=45h and AH=46h make a second handle on the same entry, so
the two share a position; and a process that ends has its handles closed for
it. The standard handles start as CON, AUX and PRN, and CON, NUL, AUX, PRN,
COMn and LPTn are devices by name in any directory, as DOS reserves them.

The console is where the host says it is. By default CON reads the emulated
keyboard and writes the screen; a host that overrides `console_write` and
`console_read` - demu - takes it over, and `console_is_device` decides what
AH=4400h reports, which is how a C runtime knows its output is a file. The
character functions (AH=01h-0Ch) go through handles 0 and 1, so they follow a
redirection too.

Names are cut to 8.3 where DOS cuts them - the last component of a file a
program creates, opens, renames or deletes - so LINK's `ramdrive.map -map` is
RAMDRIVE.MAP. A directory search's state lives in the program's DTA, with a
stamp that tells a search DOS has since closed from its successor: a program
that only wants to know whether a file exists never finishes a search, so one
for a single name is closed at once, and of the rest the one stepped longest
ago makes way - never one a program is still walking.

The input status calls - AH=0Bh, AH=06h with DL=FFh and AX=4406h - say what
DOS says: a redirected file has a character until its end, the emulated
keyboard when a key is waiting. A host that has the console answers for it
through `console_ready`, without waiting: demu says yes for a key pressed at
the terminal, or for bytes still to come when its own standard input is a
file or a pipe.

## Running another program

INT 21h AH=4Bh is where the mapping approach earns its keep and where it is
most delicate. The handler puts the parent's CPU state aside, loads the child
into free memory and leaves the CPU at the child's entry point.

Two details make it work:

- The parent is saved with the `INT`'s own return frame already unwound - the
  IP, CS and flags the `IRET` would have popped are read off the parent's stack
  and folded into the saved state, with CF cleared. When the child ends, the
  parent resumes at the instruction after its own call with DOS's success
  answer already in place.
- Changing CS:IP inside a callback means the `IRET` at the end of the stub is
  never reached, because the CPU fetches its next instruction from wherever the
  callback left it. That is what lets a service jump somewhere else entirely
  rather than return.

A process ends through `terminate`, which closes its handles, puts back the
INT 22h, 23h and 24h vectors its PSP saved when it started, frees every memory
block it owned - that is what the owner field in each MCB is for - and pops the
parent back. Only when there is no parent left does the machine actually stop.

A child gets its own copy of the environment - the block the parent passed, or
the parent's own - with a count of 1 and its full path after it, which is where
a C runtime finds `argv[0]`; and PSP:0002 is the end of the block it was given,
which is what a C runtime sizes its data segment and heap from. AL=03h loads an
overlay at a segment the caller owns, relocated by the factor it gives.

## The C compiler

`cc/` compiles C to a DOS `.COM`. It is here because the DOS-side programs this
project wants - a shell, test programs - are far easier to write in C than in
hand-assembled bytes, and because compiling a program here and running it there
exercises the CPU, the loader and the DOS layer against each other.

The shape is Tiny C Compiler's: one pass, no syntax tree, code emitted as the
parser goes. None of TCC's code is used - it is LGPL and this is not - only the
idea that a useful C compiler needs no intermediate representation.

The parser carries a small `value` describing where the result of the
expression it just parsed lives: a compile-time constant, AX, a frame slot, a
data address, or "the address is in AX". `load` turns any of those into a value
in AX and `store` puts AX back. There is no register allocator: a binary
operator pushes its left operand, evaluates the right into AX, then pops the
left back into AX with the right in CX. That is bigger code than a real
compiler emits and it is correct by construction.

A 32-bit value follows the same scheme one register wider: it lives in DX:AX
with CX:BX holding the second operand. Add, subtract and the bitwise operators
are a pair of instructions inline; multiply, both divides, both remainders, the
three shifts and the two comparisons call a helper. The helpers are emitted
*after* the program and only if something called them, so a program that never
uses a `long` pays nothing for them.

Five things are worth knowing:

- **Arguments are generated backwards.** cdecl needs them pushed right to left,
  and because the parser walks a token vector rather than a stream it can find
  the argument spans first and then generate them in reverse by seeking.
- **An 8086 has no near conditional jump.** Every conditional branch is a short
  jump over an unconditional near one, so nothing has to know in advance
  whether a target is within 127 bytes.
- **`switch` is dispatched after the body.** The switch jumps forward to a
  compare chain emitted once the case labels have been seen, which is the
  single-pass way to get a jump table's semantics without a second pass.
- **Constant folding needs the right operand before it can decide.** The parser
  marks its position, generates the left operand speculatively, parses the
  right, and if both turn out constant it discards the generated code and folds.
  That is what lets `16*FRACUNIT` or `sizeof(a)/sizeof(a[0])` reach an array
  bound or a case label.
- **Structures are packed to the byte.** An 8086 has no alignment requirement,
  and a structure read straight off disk has to match the file.

The emitted program starts by shrinking its own memory block through AH=4Ah,
so that a program it starts has somewhere to load. The runtime is five
interrupt thunks (`__int21`, `__int21si`, `__int10`, and `__int13` and
`__int13si` for the BIOS disk services, which put DI back, since AH=08h hands
back ES:DI and a function may keep a variable there) and `__seg`, and a thunk
the program never calls is not laid out; everything else is ordinary C. There
is no linker, so a program that wants the C library in `cc/lib/libc.c`
includes it rather than linking against it, and the headers in `cc/include`
are declarations only.

## Disk images

`disk/` makes the disks a real DOS boots from, which is a different job from
the emulator's: there, a folder is the drive; here, the drive is FAT on
sectors. It takes the boot code and the files' bytes and writes through a
`disk_sink`, which is told the image's size first and then gets only the
sectors that hold something - so dimg's sink is a sparse file and a 32 GB card
image costs the few megabytes written, and the suite's is a map of sectors.

The layout is DOS 4.0's own, so that FDISK, FORMAT, CHKDSK and SYS on the
machine find what they would have made. A hard disk has 63 sectors a track and
16 heads up to 504 MB, 255 above; a partition starts a track into its
cylinder - the first sector holds its partition table, FDISK's MBR for C: or an
EBR for a logical drive - and ends on a cylinder; the types are FDISK's (01h up
to 32680 sectors, 04h to 32 MB, 06h beyond, or 0Eh and 0Fh with LBA). Each
volume gets the cluster size IO.SYS's DiskTable2 gives one of its size, and a
FAT the size FORMAT gives it. That FAT is an entry or two short of the clusters
on some sizes - FORMAT's formula, like IO.SYS's, leaves out the two entries
before the first cluster - so a file is never put in a cluster with no entry;
DOS reads such a cluster's entry from the second FAT's media word and so never
uses it either. Files are never deleted from an
image being built, so each one is contiguous, which IO.SYS has to be. The date
and serial number come from the caller in a `disk_stamp`, so a build that fixes
them gets the same image every time.

The reader goes the other way and assumes nothing it made: it takes the
parameter block's word for the layout, finds the logical drives by walking the
EBR chain - only forward - letters them as DOS 4.0 does, tells FAT12 from FAT16
by the cluster count as DOS does, and gives up on a chain of clusters that
leaves the volume or loops, listing a directory several entries share once.

## Introspection

`debug_capture` fills a `debug_state` with the CPU, video, DOS and timer state
plus a disassembly window. It is strictly side-effect free: it reads emulated
memory directly rather than through the bus, so looking at a machine can never
acknowledge an interrupt out from under a program.

The disassembler in `debug.cpp` is shared by demu and dui, and demu's
`--trace-tail` uses it to keep a ring of the last instructions executed.

## Rules that keep this working

1. `src/` stays platform independent. No `windows.h`, no file access, no
   `wchar_t` in new API.
2. `mem_read8`/`mem_write8` are the only path to memory, and `host_services` -
   with, in boot mode, the `block_device`s a host attaches - the only path to
   the outside world.
3. Nothing in the emulator blocks. A service that cannot answer yet rewinds the
   caller and returns.
4. State a program can observe lives in emulated memory, not in C++ fields.
5. `cli/main.cpp` is the contract. A green build proves nothing; run
   `.\dd.ps1 test --label libdos`.
