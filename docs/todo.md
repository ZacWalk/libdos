# ovrwin gaps

Ranked roughly by value per unit of work. When you close an item, delete it;
when a review turns one up, add it here rather than in AGENTS.md.

## Known-broken programs

- **Mario Brothers VGA** fails inside Borland's `initgraph` and then reports
  "BGI Error: Graphics not initialized". Detection succeeds - the driver picks
  VGA (driver 9, mode 2) from INT 10h AH=1Ah - and the failure is after that,
  before any mode set. No file is opened and no memory is allocated on the way
  there, so it is not a missing `.BGI`. Start with
  `--trace-tail` from the last INT 10h AH=1Ah call.

## CPU

- No 386 instructions and no protected mode, so a DOS extender (DOS/4GW,
  CWSDPMI) will not run. That rules out the Ultima IV upgrade and anything
  else built with DJGPP.
- No x87. The escape opcodes consume their operands and do nothing, which is
  right for a program that checks for a coprocessor first and wrong for one
  that assumes it.
- Cycle counts are per-opcode-class approximations, not measured 8086 timings.
  Anything that calibrates a delay loop against them will be off.

## DOS

- FCB file functions (0Fh-24h) are absent. Only the parse-filename call is
  there. Pre-3.0 programs will need them.
- `EXEC` loads and runs a child (AL=0) but not an overlay (AL=3), and it always
  gives the child the parent's environment rather than a copy.
- No network redirector, no SHARE, no file locking.
- Extended error information (AH=59h) always reports success.

## The C compiler

- No floating point at run time. A floating literal folds in `double` inside a
  constant expression and has to be cast to an integer before anything asks for
  it in a register, which is what `(int32_t)(1.02*FRACUNIT)` needs and no more.
- No struct assignment, struct parameters or struct returns.
- Only the tiny model: one 64 KiB segment for code, data and stack, so there is
  no far pointer and a compiled program cannot address the video memory
  directly. There is no linker either, so `#include` is the only way to combine
  sources - `cc/lib/libc.c` is included rather than linked.
- `static` gives a local static storage, but a file-scope `static` is not
  private: there is one namespace, so two files defining the same helper name
  collide when they are included into one translation unit.
- The generated code is naive - every operand goes through AX and the stack -
  and roughly three times the size of what a real 8086 compiler emits. A 32-bit
  operand pair lives in DX:AX, with CX:BX for the second operand, and anything
  harder than an add calls one of the helpers emitted after the program.
- `sizeof` of an expression containing a call to a not-yet-defined function
  will leave a stale fixup behind.
- A `switch` truncates its controlling expression to 16 bits.
- Structures are packed to the byte, matching the `-zp1` a 16-bit DOS build
  normally uses. There is no way to ask for alignment.
- The preprocessor has no `#` or `##` operator, and `#pragma` is ignored.
- The C library in `cc/lib/libc.c` is a starting point, not a complete one:
  `malloc` is a bump allocator that never reuses, `getenv` always fails, and
  `printf` accepts width and precision but ignores them.

## Running a large program: the memory model

The compiler emits a `.COM`: one segment, code and data and stack together,
64 KiB in total. That is the single largest limit in the project and the reason
a real game cannot be built here yet. Doom8088, the worked example in
`inspiration/Doom8088`, compiles 50 of the 51 files its mode 13h build needs
but cannot be linked or run.

**There is no linker to write.** `cc/` already does whole-program compilation:
one token vector, one symbol table, every fixup resolved in `finish()`. Symbol
resolution across objects is a linker's actual job and it is already done. Nor
would working in offsets help, because the code already works in offsets and
patches them at the end. The limit is narrower than that: a near `CALL`
displacement is 16 bits and `CS` never changes. What is missing is a memory
model, not a linker.

Three pieces, smallest first, each independently useful and testable:

- **Far data pointers.** A 4-byte pointer, `LES BX,[p]` and an `ES:` prefix on
  the access, and `__far` meaning something rather than being ignored. The
  smallest of the three and the one that pays off on its own: it is what lets
  any compiled program address video memory directly, and it is needed 1079
  times in Doom8088.
- **`.EXE` output with relocations.** A multi-segment program cannot be a
  `.COM`. Worth doing before the item below and while there is still only one
  code segment, because it de-risks the whole pipeline on a small program. The
  loader already reads MZ files and applies relocations, so this is writing a
  header and a relocation table, not new emulation.
- **Far calls and multi-segment code.** Emit every user call as a far call
  (`9A off:seg`) with `RETF`, uniformly, so single-pass emission never has to
  guess an instruction width - two bytes more per call, and correct by
  construction. `finish()` already knows every function's start offset and
  every call site, so laying functions into 64 KiB segments is arithmetic plus
  padding, and each segment word becomes an MZ relocation entry. Data stays in
  one segment, which is the medium model and what Doom8088's own build uses.
  Function pointers widen to four bytes with it.

Overlays, or loading pieces on demand, are the obvious alternative and are
worse here. Overlays pay off when a program has phases - load the menu, discard
it, load the game. Doom's call graph is densely interconnected: the renderer,
the tables and the zone allocator call each other constantly, so it would
thrash. It also needs an overlay manager and a thunk on every cross-overlay
call. More machinery than far calls, for a worse result.

Two things to weigh before starting:

- The generated code is roughly three times the size of a real 8086 compiler's.
  Doom8088 is about 200 KiB built with gcc-ia16 `-Os`, so here it would be
  500-600 KiB across nine or so code segments. It would very likely run. Whether
  it is playable is a different question, and if that is the goal then codegen
  quality, not the memory model, is the real project.
- One translation unit is viable. Compiling all 51 files together needs their
  file-scope names not to collide, and a check across the mode 13h set found
  only a handful that do. File-scope `static` being private would settle it.

The emulator side is in better shape than the compiler side: mode 13h, the
timer, the keyboard and the PC speaker are all there. The gaps that would bite
are unchained mode Y (listed under Video) and XMS, which `z_xms.asm` uses for
the WAD cache.


## Shell

There are two: the native prompt in `src/shell.cpp` that the app comes up with,
and `shell/shell.c`, a DOS program built by `cc/`. Neither has:

- Batch files, so a game's `RUN.BAT` has to be read and typed by hand.
- `COPY`, wildcards for `DEL`, redirection or piping.
- Command history or editing beyond backspace.
- `/W` or `/P` for `DIR`, so a long listing scrolls past.

The native one additionally cannot start a program and stay resident the way
the DOS one does through `EXEC`; it resets the machine instead.

## Video

- No unchained 256-colour "mode X". Sequencer writes that would select it fall
  back to the planar path, so a game that uses it renders wrongly rather than
  not at all.
- No VESA/SVGA. INT 10h AH=4Fh reports unsupported, which is the honest answer.
- Split screen (CRTC line compare) and pixel panning are ignored, so a game
  with a fixed status panel under a scrolling view will scroll the whole
  screen.
- Scroll-down (INT 10h AH=07h) clears the window instead of scrolling it.
- The retrace status on port 3DAh is derived from the cycle counter rather than
  a real raster position. Timing loops work; a program measuring the raster
  will not.
- Pixels are whole numbers of screen pixels, so the 4:3 shape a 320x200 mode
  had is only approximated: it is shown square, and a 200-line text mode at
  1:2. Matching the original exactly means giving up whole-number scaling.
- The character generator is 8x8 rather than the VGA's 8x16, and codes 128-159
  and 224-255 of code page 437 are blank.

## Sound

- No Sound Blaster: no DSP at port 220h, no 8237 DMA, so digital sound effects
  and speech are silent. This is the largest remaining audio gap.
- The OPL envelope rates are an analytic approximation of the published tables
  rather than the tables themselves, and vibrato, tremolo and the percussion
  mode are not implemented.
- The PC speaker models the square wave from timer channel 2 and direct
  toggling of port 61h bit 1, but not PWM sample playback.
- No MIDI, no MPU-401.

## Input

- Extended keys reach the BIOS buffer through the keypad scancodes rather than
  the `E0` prefixed set, so a program reading raw scancodes from INT 9 cannot
  tell the cursor pad from the numeric keypad.
- No joystick. Port 201h reports nothing attached.

## Host and app

- The app has no settings file: window size, sound and aspect are not
  remembered between runs.
- No save states.
- The app draws the whole frame every iteration whether or not anything
  changed; `vga_state::dirty` is maintained but not used.

## Tests

The suite in `cli/main.cpp` covers the CPU, the interrupt path, the timer, the
video modes and write logic, DOS memory and paths, both loaders, a full program
run, sound, the compiler and the disassembler. It does not yet pin:

- The planar read modes and the colour-compare path.
- The MCB chain layout a program would walk.
- CGA mode rendering.
- Anything about `--trace` or the debug capture output.
- The C library in `cc/lib/libc.c`. Every compiler test is self-contained, so
  the library is only exercised by hand; it needs a test that compiles a
  program including it and checks the output.
- The preprocessor directly. `#if`/`#elif` arithmetic, macro arguments that are
  themselves macros, and a function-like macro used inside another macro's body
  have all been wrong at some point and are only covered indirectly.
