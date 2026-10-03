# ovrwin Repository Guidance

ovrwin is a C++20 DOS program emulator: a platform-independent static library
(`src/`), a Windows host implementation (`host/`), an app built on the shared
[platform-h](https://github.com/ZacWalk/platform-h) layer (`app/`) and a
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
- `app/` talks to the outside world through `pf::` only - window, menu, input,
  audio, dialogs and file paths. It names no operating system, and nothing
  Win32-shaped may go into platform-h to make it easier; see that repo's rules.
- `host/` and `cli/` own what is left of Windows: the file system and the
  console. `host/host-win32.cpp` is shared by the app and the CLI.
- `cc/` is platform independent too, and opens no files: a caller supplies
  source text through `cc_sources`, the same way a host supplies the drive.
- `shell/` is C for the emulated machine, not for the host. It is compiled by
  `cc/`, so it can only use the subset in [docs/todo.md](docs/todo.md).
- When the app needs a new capability, expose a platform-neutral operation from
  `machine.h` rather than reaching into `machine_state`.

### Why `host/` is still Win32

Considered and deliberately deferred, so nobody re-opens it by accident.
Everything in `host-win32.cpp` but one call maps onto `<filesystem>`,
`<fstream>` and `<chrono>`. The exception is `code_alloc` / `code_commit`,
which wants pages that are writable and then executable and has no standard
form - and routing that through `pf::` would drag the platform layer, and the
`WinMain` that forces `app_init` stubs on a console program, into the CLI. If
it is ever done, the shape is standard C++ plus a small OS-specific file, not
`pf::`.

Two behaviours would change and both want tests written first: `FindFirstFileA`
is doing the DOS wildcard matching and the 8.3 short name today - and gets the
name wrong on a volume with short-name creation disabled, truncating to 12
characters without honouring the dot - and `std::filesystem` has no hidden,
system or archive attribute.

## Coding Guidelines

- Tab-indented, C++20, existing naming (`snake_case` types and functions).
- Keep it compact. Add an abstraction only when it improves the library API or
  removes real duplication.
- `src/rom-font.cpp` is generated data. Never hand-edit it; run
  `tools/make-font.ps1`.
- Never edit anything under `build/` or `exe/`.
- Debug and Release both link the static CRT (`/MTd`, `/MT`), set once in
  `CMakeLists.txt` before the platform layer is pulled in so it is built the
  same way. Changing it breaks the link.

## Build And Validation

Build with the vendored dd driver in PowerShell 7.4+. It sets up the MSVC
environment itself, so no developer prompt is needed. `build` and `test` cover
Debug and Release unless one is named:

```powershell
.\dd.ps1 test --label ovrwin        # build + the CLI suite and the app's offscreen check
.\dd.ps1 build release              # or debug
.\dd.ps1 launch                     # build Release + start the app (for the human)
.\dd.ps1 run cli -- --info GAME.EXE
.\dd.ps1 run cli -- --cc .\shell\shell.c
.\dd.ps1 bench --yes                # compile the benchmark for this machine and run it
.\dd.ps1 clean --dry-run
```

Use `--label ovrwin`: an unfiltered `.\dd.ps1 test` also opens the app's window
for a second as a smoke check. After a build the suite runs directly too:

```powershell
.\exe\ovrwincli.exe --test    # returns non-zero on failure
```

Build trees are per preset under `build/`; binaries land in `exe/` as
`ovrwin.exe` and `ovrwincli.exe`, with a `-d` suffix for Debug.

The app builds against a sibling `../platform-h` checkout when one exists and
against the pinned revision in `CMakeLists.txt` otherwise - so a green local
build says nothing about the pin. After changing platform-h, push it and re-pin
here.

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

A green build proves nothing on its own - run `.\dd.ps1 test --label ovrwin`.
It runs the CLI suite and then the app's offscreen self-check, which pins the
picture scaling and that the framebuffer reaches the screen, in both
configurations. Do not launch the GUI to verify a change; `ovrwin.exe -test` is
the headless form and `dd.ps1 launch` is for the human.

## Known Gaps

The gap list lives in [docs/todo.md](docs/todo.md). Keep it current.

## dd build system

This repo uses the vendored dd build system. dd has two modes.

**CLI mode** is the default; each verb runs once and exits:

```pwsh
.\dd.ps1 test                  # build both configs and run the suite
.\dd.ps1 build debug
.\dd.ps1 doctor --json
.\dd.ps1 commands --json       # list this project's own commands
```

**MCP mode** — `.\dd.ps1 mcp` turns the process into a stdio JSON-RPC server for an
MCP client, adapting typed requests onto CLI mode. It owns stdout for protocol
messages, so it prints no result envelope and rejects `--json`. Register it with
`.\dd.ps1 ide --mcp`.

Project settings live in `dd.psd1`; dependency pins live in
`cmake/dd-dependencies.json` with `dependencies.owner = 'dd'`. Vendored dd file
hashes are recorded in `docs/dd-upstream.json`.

### Project commands

`dd bench` compiles a C source with ovrwin's own compiler and runs it (ported from
the pre-dd driver). Build the CLI first with `dd build`.

```pwsh
.\dd.ps1 bench --dry-run                 # default source cc/bench/bench.c, run natively
.\dd.ps1 bench --exe true --yes          # emit an executable into exe/ then run it
.\dd.ps1 bench --source cc/bench/other.c --yes
```
