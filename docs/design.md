# libdos design

How the emulator is put together and why. For what is *not* built yet, see
[todo.md](todo.md). For build and usage, see the [README](../README.md).

## Shape

Four pieces, with a hard boundary between the first and the rest:

| | |
|---|---|
| `src/` | The emulator. Platform independent: no Windows headers, no file system, no threads. Builds `libdos`. |
| `host/` | The Windows side of `host_services`: one directory presented as the emulated drive. |
| `app/` | dui, the platform-h host. Owns the window, picture, sound and menus through `pf::`. |
| `cli/` | demu, a console harness that drives the library headlessly and doubles as the debugger. |
| `cc/` | A C compiler targeting 16-bit DOS. Platform independent, and like `src/` it opens no files. |
| `shell/` | A DOS command interpreter written in C and built by `cc/`. |

`machine` is the embeddable façade; `machine_state` is the whole machine as
plain data. `machine` owns exactly one `machine_state` and is non-copyable.

The library never opens a file. `host_services` (in `src/host.h`) is the only
way out: a host implements it and decides what `C:\` means. That is what makes
the emulator embeddable and what stops a program reaching outside its folder.

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

The two 8259s model interrupt request, in-service and mask registers with real
priority. `cpu_step` takes a pending interrupt at an instruction boundary when
IF is set, and the vector goes through the IVT like any other - so if a program
hooked INT 8, its handler runs.

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

A process ends through `terminate`, which frees every memory block the child
owned - that is what the owner field in each MCB is for - and pops the parent
back. Only when there is no parent left does the machine actually stop.

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
so that a program it starts has somewhere to load. The runtime is three
interrupt thunks (`__int21`, `__int21si`, `__int10`) and `__seg`; everything
else is ordinary C. There is no linker, so a program that wants the C library
in `cc/lib/libc.c` includes it rather than linking against it, and the headers
in `cc/include` are declarations only.

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
2. `mem_read8`/`mem_write8` are the only path to memory, and `host_services`
   the only path to the outside world.
3. Nothing in the emulator blocks. A service that cannot answer yet rewinds the
   caller and returns.
4. State a program can observe lives in emulated memory, not in C++ fields.
5. `cli/main.cpp` is the contract. A green build proves nothing; run
   `.\dd.ps1 test --label libdos`.
