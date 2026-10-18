# libdos gaps

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

- No 32-bit operands or addresses and no protected mode, so a DOS extender
  (DOS/4GW, CWSDPMI) will not run. That rules out the Ultima IV upgrade and
  anything else built with DJGPP. This, not emulator speed, is what stops a
  more advanced game running: `--bench` puts the interpreter at ~35 M guest
  instructions a second, six times what a 386 at 33 MHz managed. The x64 JIT
  design and those measurements are in [jit.md](jit.md).

  The 386's two-byte opcodes are in, in their 16-bit forms, so a 16-bit program
  built for a 386 runs. What is missing is the `66`/`67` prefixes, the 32-bit
  register file with SIB addressing, `PUSHAD`/`POPAD`, `CDQ`/`CWDE`, `BSWAP`
  and the 32-bit string instructions.
- No x87. The escape opcodes consume their operands and do nothing, which is
  right for a program that checks for a coprocessor first and wrong for one
  that assumes it.
- Cycle counts are per-opcode-class approximations, not measured 8086 timings.
  Anything that calibrates a delay loop against them will be off.

## DOS

- FCB file functions (0Fh-24h) are absent. Only the parse-filename call is
  there. Pre-3.0 programs will need them.
- `EXEC` AL=01h (load without running, for a debugger) is refused.
- No Ctrl+C or Ctrl+Break. DOS never raises INT 23h and the BIOS never calls
  INT 1Bh, so a ^C typed at a program reading the console arrives as
  character 3 - into an AH=0Ah line as well - and nothing stops a program that
  never asks for input; Machine > Reset is the only way out. dui maps
  neither the Pause key nor Ctrl+Break.
- The root of the drive lists `.` and `..`, which a real root directory does
  not have: the host's `FindFirstFileA` returns them for the mounted folder.
- No network redirector, no SHARE, no file locking.
- Extended error information (AH=59h) always reports success.
- AH=57h cannot set a file's date and time: the host interface has no call for
  it, so the request succeeds and changes nothing.
- COMMAND.COM /C refuses pipes and batch files, and FOR can only run built-in
  commands: starting a program for one item would leave the rest unrun.

## demu as a runner

- It runs the dos4z build of MS-DOS 4.00 about a third slower than the demu it
  replaced (231 s against 169 s for a clean build), all of it in the
  interpreter: per instruction, the decode, the effective address and the
  flags. The timer and interrupt checks were the first 25% and are gone; the
  rest wants the core reworked or the JIT in [jit.md](jit.md).
- The old demu could check every memory access against a shadow map of what
  had been written and allocated. That caught its own loader's mistakes more
  than any program's, and doing it here would put a test on every access, so
  it was not carried over; `--trace-tail` and the suite cover that ground.

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
- The generated code is no longer naive, but it is still a long way from a real
  8086 compiler's. `cc/bench/bench.c` is the yardstick - the shapes a game's
  inner loops are made of, with a checksum in the exit code - and
  `demu --ccbench cc\bench\bench.c` reports its size and what it costs to
  run. What is there now, statement by statement: an operand that generates no
  code of its own is placed rather than pushed, so a constant folds into the
  instruction; a comparison stays in the flags instead of becoming a 0 or a 1
  that is then tested; a dereference remembers that its address is still in a
  pointer variable and uses `[bx+displacement]`; a constant index or a member
  offset folds into the address; a statement whose result nobody wants is
  applied straight to memory; an operator with a register/memory form reads its
  second operand where it lies rather than through CX; and a loop puts its
  condition at the bottom.

  And across the whole program, from the instruction list described below: a
  branch that reaches gets the two-byte form, a branch to the next instruction
  goes altogether, a function nothing can reach is left out, and a body short
  enough is copied to its call site instead of called - with any argument that
  is a constant standing in for the parameter, which is what makes the copy
  smaller as well as faster. `--inline <n>`, `--no-inline` and `--keep-unused`
  control it; `--map` says where the code went.

  And per function, over its finished instructions: the two frame slots it
  touches most move into SI and DI, weighting a use inside a loop far above one
  on a straight line - a loop being nothing more, here, than a branch back to
  something already generated. A pointer that lands in a register then addresses
  memory itself, which needs to know that BX is dead, which is a backward
  liveness pass over the same list. None of this could have been decided while
  parsing: which slot is worth a register is only known once the last statement
  has been read.

  What is not:

  - Only two registers, and only whole-function live ranges. A value that is
    hot in one loop and dead elsewhere holds its register for the whole body,
    and a third candidate gets nothing. Splitting a range at a basic block
    boundary is the next step, and the liveness pass is already there.
  - Nothing is hoisted out of a loop, so an invariant operand is reloaded every
    iteration once the two registers are spoken for.
  - The address of a global folds into an `add` rather than into the
    displacement of the access that follows it, costing an instruction at every
    subscript of a global array.
  - A 32-bit operand pair lives in DX:AX, with CX:BX for the second operand,
    and anything harder than an add or a constant shift calls one of the
    helpers emitted after the program.
  - `switch` is a chain of compares, not a table.
  - Only a constant argument stands in for a parameter when a body is copied.
    A local would too, but only if nothing the body calls could change it, and
    the generator cannot yet say that.
- `sizeof` of an expression containing a call to a not-yet-defined function
  will leave a stale fixup behind.
- A cast to an abstract function-pointer type, `(int (*)(int))p`, is a syntax
  error; the same type through a `typedef` works.
- A call is not checked against the prototype's parameter count, so `f(1)` for
  `int f(int, int)` compiles and the callee reads whatever is on the stack.
- Constant folding does not wrap to 16 bits: `1 << 15` folds to 32768, not
  -32768, so `(1 << 15) == -32768` is false. Signed overflow is undefined in C,
  but every 16-bit compiler wraps and programs rely on it.
- A `switch` truncates its controlling expression to 16 bits.
- Structures are packed to the byte, matching the `-zp1` a 16-bit DOS build
  normally uses. There is no way to ask for alignment.
- The preprocessor has no `#` or `##` operator, and `#pragma` is ignored.
- The C library in `cc/lib/libc.c` is a starting point, not a complete one:
  `malloc` is a bump allocator that never reuses, `getenv` always fails,
  `printf` accepts width and precision but ignores them, and `fopen` in append
  mode moves to the end once, when the file is opened, rather than before every
  write. The list of what a real program still asks for is under the memory
  model below.
- The compiler is C++ and so cannot be its own input. Rewriting it in C and
  then feeding it to itself is planned in [docs/selfhost.md](selfhost.md). What
  stands in the way, in the compiler itself, is struct assignment, struct
  parameters and struct returns, and a front end that honours the target's int
  and pointer widths instead of assuming two bytes.
- A program compiled for this machine runs in a 64 KiB block the host hands it,
  so it is the tiny model natively as well: `demu --ccnative cc\bench\bench.c`
  compiles the benchmark for x64, runs it in process and gets the same checksum
  and exit code the emulated build gives - about 195x faster, 0.8 ms against
  157 ms. `cc\bench\roundtrip.c` does the same for the file services. What is
  missing there is the rest of DOS: no find-first, no keyboard, no EXEC, so the
  shell does not run this way yet.
- `demu --ccexe <file.c>` writes the same image out as a Windows
  executable: one section holding the block, a startup that hands the program
  its own base, and a service routine that turns INT 21h into `WriteFile` and
  `ExitProcess`. It is not relocatable - there is no relocation table, so the
  loader has to honour the image base - and its DOS is smaller still than the
  in-process one: only 02h, 09h and 4Ch, so a program that opens a file runs but
  gets a failure back.

## Running a large program: the memory model

The compiler emits a `.COM`: one segment, code and data and stack together,
64 KiB in total. That is the single largest limit in the project and the reason
a real game cannot be built here yet.

Doom8088, the worked example in `inspiration/Doom8088`, now **compiles all the
way through** as a single translation unit. Every one of the 51 files its mode
13h build needs parses and generates code; the only thing it stops on is the
segment limit. Reproducing it:

```powershell
# one file that includes the other 51, then the library it expects
demu --cc doomall.c -I cc\include -DC_ONLY -DFLAT_SPAN -DVIEWWINDOWWIDTH=120
```

What that says today:

- **132 KiB of code and 74 KiB of data.** The earlier guess here was 500-600 KiB
  of code; the measurement is a quarter of that. Three code segments, not nine.
- The data does **not** fit in one 64 KiB segment either, so the medium model
  Doom8088's own build uses is not quite enough: either the large tables have
  to move to the code segment (they are `const`) or data needs far pointers too.
- Twenty-eight library and runtime functions are still missing: `inp`, `outp`,
  `int86`, `_dos_getvect`, `_dos_setvect`, `_dos_allocmem`, `_disable`,
  `_enable`, `_chain_intr`, `exit`, `abs`, `labs`, `fopen`, `fread`, `fseek`,
  `ftell`, `printf`, `vprintf`, `sprintf`, `memcpy`, `memset`, `memcmp`,
  `memchr`, `strcpy`, `strncpy`, `strlen`, `stricmp`, `toupper`. Most are
  already in `cc/lib/libc.c`; the DOS ones are thin wrappers over `__int21`.
- It is the `C_ONLY` configuration, so the three hand-written assembly files
  (`i_vv13ma.asm`, `m_fixed.asm`, `z_xms.asm`) are not needed.

**There is no linker to write.** `cc/` already does whole-program compilation:
one token vector, one symbol table, every fixup resolved in `finish()`. Symbol
resolution across objects is a linker's actual job and it is already done. Nor
would working in offsets help, because the code already works in offsets. The
limit is narrower than that: a near `CALL` displacement is 16 bits and `CS`
never changes. What is missing is a memory model, not a linker.

The bookkeeping a memory model needs is now there. `emitter` holds an
**instruction list** rather than bytes: each entry names an operation, a width,
and operands that may be a register, an immediate, or a memory form, and an
operand that means an address carries a reference instead of a number. Only
`emit.c` knows an opcode, so `lay_out()` can choose an encoding for each
branch, drop what the program cannot reach, move everything that is left, and
recompute every label. That is the smallest thing that deserves to be called an
intermediate representation, and it is what the three pieces below all rest on.
`--map` prints what came out of it: for Doom8088, 583 functions, so dividing
them into three segments is arithmetic.

There are three back ends over that one list, which is the claim it was built to
make:

- **8086**, in `emit.c`. The default, and what a `.COM` needs.
- **386**, the same file with more to say: `SETcc` instead of a branch over a
  clear, `MOVZX`/`MOVSX` instead of a load and a fix-up, and a conditional
  branch that reaches in one instruction. Still a 16-bit `.COM` - `--cpu 386`
  is the switch, and every compiler test runs at both levels. Worth 5% of the
  instructions on the benchmark.
- **x64**, in `emit-x64.c`, a second file of the same shape: a different
  mod/rm table, different prefixes, a branch that reaches further, and agreeing
  with the first on nothing but the opcode bytes. `--ccnative` compiles a
  program for the machine the compiler is running on, puts it in memory the
  host made executable, and calls it. The test suite compiles one program both
  ways and checks the host and the emulator agree.

What the x64 back end cannot do yet is a **pointer**: the front end still makes
an `int` and a pointer two bytes wide, so an address does not survive in a
register. Locals reach through RBP and globals RIP-relative, which is enough
for arithmetic, control flow and calls, and not enough for arrays, pointers or
`long`. Making the front end parametric over width is the work that unblocks
it, and it is the same work that would let the 386 back end compile 32-bit
code.

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
  construction. The layout record already knows every function's span and every
  call site, so laying functions into 64 KiB segments is arithmetic plus
  padding, and each segment word becomes an MZ relocation entry. Function
  pointers widen to four bytes with it.

Overlays, or loading pieces on demand, are the obvious alternative and are
worse here. Overlays pay off when a program has phases - load the menu, discard
it, load the game. Doom's call graph is densely interconnected: the renderer,
the tables and the zone allocator call each other constantly, so it would
thrash. It also needs an overlay manager and a thunk on every cross-overlay
call. More machinery than far calls, for a worse result.

One translation unit is viable, and now demonstrated. The file-scope name
collisions the whole-program build was expected to hit did not materialise in
the mode 13h set.

The emulator side is in better shape than the compiler side: mode 13h, the
timer, the keyboard and the PC speaker are all there. The gaps that would bite
are unchained mode Y (listed under Video) and XMS, which `z_xms.asm` uses for
the WAD cache - though `C_ONLY` avoids it.


## Shell

There are two: the native prompt in `src/shell.cpp` that dui comes up with,
and `shell/shell.c`, a DOS program built by `cc/`. The native one shares
COMMAND.COM's built-in commands with COMMAND.COM /C (`src/command.cpp`), so it
has COPY, wildcards for DEL, REN, ATTRIB and the rest, and `CD..` and `CD\`
written without a space. Neither has:

- Batch files, so a game's `RUN.BAT` has to be read and typed by hand.
- Redirection or piping at the prompt; COMMAND.COM /C has redirection.
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
  tell the cursor pad from the numeric keypad. For the same reason the
  emulated NumLock starts off, so the keypad's digits arrive as cursor keys
  whatever the host's NumLock says.
- Alt, Alt+key and F10 never reach a program in dui: platform-h does not
  forward `WM_SYSKEYDOWN`/`WM_SYSKEYUP`, so Windows uses them for the menu bar
  instead. The BIOS translates Alt combinations for when they do arrive.
- No joystick. Port 201h reports nothing attached.

## Host and app

- dui has no settings file: window size, sound and aspect are not
  remembered between runs.
- No save states.
- dui draws the whole frame every iteration whether or not anything
  changed; `vga_state::dirty` is maintained but not used.

## Tests

The suite in `cli/main.cpp` covers the CPU, the interrupt path, the timer, the
video modes and write logic, DOS memory and paths, both loaders, a full program
run, sound, the compiler and the disassembler. It does not yet pin:

- The planar read modes and the colour-compare path.
- The MCB chain layout a program would walk.
- CGA mode rendering.
- Anything about `--trace` or the debug capture output, or what `--info` says
  about a file - which once called an MZ file too short to load a COM file.
- The C library in `cc/lib/libc.c`. Every compiler test is self-contained, so
  the library is only exercised by hand; it needs a test that compiles a
  program including it and checks the output.
- The preprocessor directly. `#if`/`#elif` arithmetic, macro arguments that are
  themselves macros, and a function-like macro used inside another macro's body
  have all been wrong at some point and are only covered indirectly.
