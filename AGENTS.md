# ovrwin Repository Guidance

ovrwin is a C++20 DOS program emulator: a platform-independent static library
(`src/`), a Windows host implementation (`host/`), a Win32 app (`app/`) and a
command-line driver that is both the test suite and the debugger (`cli/`).
Alongside it are a C compiler for 16-bit DOS (`cc/`) and a DOS shell written in
C and built with it (`shell/`).

Read [docs/design.md](docs/design.md) before changing anything in `src/` - it
covers the callback opcode, the mapping-not-emulating boundary, the plane
interleave and the invariants that hold the design together.
[docs/todo.md](docs/todo.md) is the gap list; prefer picking work from there
over inventing new work. See [README.md](README.md) for what the emulator
covers at a glance.

## Project Boundaries

- `src/` is platform independent. No `windows.h`, no `wchar_t` in new API, no
  file-system access, no threads.
- The library takes bytes and returns bytes. Reading files is the host's job,
  through `host_services` in `src/host.h`.
- `host/`, `app/` and `cli/` own everything Windows: the window, GDI, waveOut,
  the file system and the console.
- `cc/` is platform independent too, and opens no files: a caller supplies
  source text through `cc_sources`, the same way a host supplies the drive.
- `shell/` is C for the emulated machine, not for the host. It is compiled by
  `cc/`, so it can only use the subset in [docs/todo.md](docs/todo.md).
- When the app needs a new capability, expose a platform-neutral operation from
  `machine.h` rather than reaching into `machine_state`.

## Coding Guidelines

- Tab-indented, C++20, existing naming (`snake_case` types and functions).
- Keep it compact. Add an abstraction only when it improves the library API or
  removes real duplication.
- `src/rom-font.cpp` is generated data. Never hand-edit it; run
  `tools/make-font.ps1`.
- Never edit anything under `intermediate/` or `bin/`.
- Debug uses the static debug runtime (`/MTd`), Release the static runtime
  (`/MT`). Changing this breaks the project-reference link.

## Build And Validation

`dd.ps1` builds and runs in one step and finds MSBuild via `vswhere`, so no
developer prompt is needed. It defaults to Debug x64 (`run` defaults to
Release):

```powershell
.\dd.ps1 test               # build + run the test suite
.\dd.ps1 run                # build + launch the app
.\dd.ps1 bench              # build, compile the benchmark for this machine, run it
.\dd.ps1 bench -Exe         # the same, as a standalone bin\bench.exe
.\dd.ps1 cli --info GAME.EXE
.\dd.ps1 cli --cc .\shell\shell.c
.\dd.ps1 test -Release      # Release
.\dd.ps1 test -Win32        # 32-bit
```

Direct MSBuild after any source or project change:

```powershell
msbuild ovrwin.sln /m /p:Configuration=Debug /p:Platform=x64
.\bin\ovrwincli64d.exe --test    # returns non-zero on failure
```

Gotcha: at the **solution** level the 32-bit platform is named `x86`
(`/p:Platform=x86`); `Platform=Win32` only works on an individual `.vcxproj`.

Outputs go to `bin/` as `<name><64|32><r|d>` (`64`/`32` = x64/Win32, `d`/`r` =
Debug/Release), e.g. `ovrwin64d.lib`, `ovrwinapp64r.exe`, `ovrwincli64d.exe`.

## Debugging A Program That Does Not Work

This is most of the work, and the CLI exists for it. In order:

1. `--trace` prints every DOS and BIOS call with its arguments and the address
   it came from. Nearly every failure is visible here as a service returning
   something the program did not expect.
2. `--trace-tail n` prints the last `n` instructions executed with registers.
   Use it when the program gives up without asking for anything.
3. `--bmp file` writes the final frame. The text-screen dump (`--screen`) is
   meaningless in a graphics mode; the bitmap is not.
4. `--state` prints registers, the memory map, open files and a disassembly
   window.

A worked example, from the commit history: Elite reported "Packed file is
corrupt". `--trace-tail` showed its EXEPACK unpacker decoding a valid block and
then reading garbage. Comparing the source pointer before and after a
`rep movsb` showed it had moved 1741 bytes instead of 2228 - a timer interrupt
had arrived mid-block and the emulator had *abandoned* the REP instead of
rewinding to the prefix and resuming it.

## What The Tests Pin

`cli/main.cpp` is the contract. Before "fixing" an emulation detail, check
whether a test already encodes the current behaviour:

- CPU: arithmetic and its flags, `MUL`/`IDIV`, shifts, string instructions,
  `CALL`/`RET`, `LES`, and the 80186 additions.
- Interrupts: INT 10h through the vector table, and a program hooking a vector
  and seeing its own handler run.
- Timer: channel 0 raising IRQ 0 at the programmed rate, and the BIOS tick
  count advancing through the INT 8 stub.
- Video: mode 13h linear memory and DAC output, the planar set/reset and bit
  mask path, and text-mode plane writes.
- DOS: MCB allocate/free/reuse/resize, path resolution including `..`, both
  loaders including relocation, and a program running to completion with its
  output on screen.
- Sound: the AdLib detection sequence, and both the OPL and the speaker
  producing output.
- The compiler: a C program using recursion, arrays, structures, `switch` and
  the preprocessor is compiled and then **run in the emulator**, and its output
  checked. The same is done for 32-bit arithmetic (multiply, both divides,
  remainder, shifts and comparisons), for function pointers and `goto`, and for
  variadic functions. A second pair of programs pins `EXEC`.
- The disassembler, which the debugger depends on.

A green build proves nothing on its own - run `.\dd.ps1 test` and check for
`FAIL` lines. Smoke-test app changes by launching it; the title bar reports the
program, the video mode and the pause state.

## Known Gaps

The gap list lives in [docs/todo.md](docs/todo.md). Keep it current.
