# libdos Repository Guidance

libdos is a C++20 DOS program emulator: a platform-independent static library
(`src/`), a Windows host implementation (`host/`), dui - the windowed app built
on the shared [platform-h](https://github.com/ZacWalk/platform-h) layer
(`app/`) - and demu, the command-line runner that is also the test suite and
the debugger (`cli/`).
Alongside it are a C compiler for 16-bit DOS (`cc/`), a DOS shell written in
C and built with it (`shell/`), and dimg (`dimg/`), which makes and reads
bootable DOS disk images with the library in `disk/`.

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
  through `host_services` in `src/host.h` - and in boot mode through the
  `block_device`s in `src/block-device.h` a host attaches as drives.
- `app/` (dui) talks to the outside world through `pf::` only - window, menu, input,
  audio, dialogs and file paths. It names no operating system, and nothing
  Win32-shaped may go into platform-h to make it easier; see that repo's rules.
- `host/` and `cli/` (demu) own what is left of Windows: the file system and the
  console. `host/host-win32.cpp` is shared by dui and demu; demu's program
  runner (`cli/runner.cpp`) adds the terminal, the fixed date and LIB's
  stand-in (`cli/lib-omf.c`) on top of it.
- `cc/` is platform independent too, and opens no files: a caller supplies
  source text through `cc_sources`, the same way a host supplies the drive.
- `disk/` is platform independent as well and opens no files either: a caller
  hands in the boot code and the files' bytes, and a `disk_sink` or
  `disk_source` for the sectors. `dimg/` owns the files, the sparse image file
  and the command line.
- `shell/` is C for the emulated machine, not for the host. It is compiled by
  `cc/`, so it can only use the subset in [docs/todo.md](docs/todo.md).
- When dui or demu needs a new capability, expose a platform-neutral operation from
  `machine.h` rather than reaching into `machine_state`.

### Why `host/` is still Win32

Considered and deliberately deferred, so nobody re-opens it by accident.
Everything in `host-win32.cpp` but one call maps onto `<filesystem>`,
`<fstream>` and `<chrono>`. The exception is `code_alloc` / `code_commit`,
which wants pages that are writable and then executable and has no standard
form - and routing that through `pf::` would drag the platform layer, and the
`WinMain` that forces `app_init` stubs on a console program, into demu. If
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
environment itself, so no developer prompt is needed. `build` covers Debug and
Release unless one is named; `test` always builds and runs both:

```powershell
.\dd.ps1 test --label libdos        # build + the demu suite and dui's offscreen check
.\dd.ps1 build release              # or debug
.\dd.ps1 launch                     # build Release + start dui (for the human)
.\dd.ps1 run cli '--' --info GAME.EXE
.\dd.ps1 run cli '--' --cc .\shell\shell.c
.\dd.ps1 bench --yes                # compile the benchmark for this machine and run it
.\dd.ps1 clean --dry-run
```

The `'--'` is quoted because PowerShell removes a bare `--` from a script's
arguments, and dd then rejects demu's own options as its own.

Use `--label libdos`: an unfiltered `.\dd.ps1 test` also opens dui's window
for a second as a smoke check. After a build the suite runs directly too:

```powershell
.\exe\demu.exe --test    # returns non-zero on failure
```

Build trees are per preset under `build/`; binaries land in `exe/` as
`dui.exe`, `demu.exe` and `dimg.exe`, with a `-d` suffix for Debug. Embedded in another
CMake project, libdos builds only its libraries unless `LIBDOS_BUILD_TOOLS` or
`LIBDOS_BUILD_APP` is on, and only `LIBDOS_BUILD_APP` fetches platform-h.

The [dd](https://github.com/ZacWalk/dd) build driver is vendored unmodified as
`dd.ps1` and `.dd/`, with file hashes recorded in
[docs/dd-upstream.json](docs/dd-upstream.json). Project settings live in
`dd.psd1`; platform-h, the one dependency, is pinned by the `GIT_TAG` in
`CMakeLists.txt`, not in `cmake/dd-dependencies.json`.
`.\dd.ps1 commands --json` lists this project's own commands. dd's
[two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes) —
CLI, and MCP for an MCP client — and its
[commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) are
documented in its own repository.

`dd bench` compiles a C source with libdos's own compiler and runs it (ported
from the pre-dd driver). Build demu first with `dd build`:

```pwsh
.\dd.ps1 bench --dry-run                 # default source cc/bench/bench.c, run natively
.\dd.ps1 bench --exe true --yes          # emit an executable into exe/ then run it
.\dd.ps1 bench --source cc/bench/other.c --yes
```

dui builds against a sibling `../platform-h` checkout when one exists and
against the pinned revision in `CMakeLists.txt` otherwise - so a green local
build says nothing about the pin. After changing platform-h, push it and re-pin
here.

A tag `vX.Y.Z` on `main` releases: `.github/workflows/release.yml` builds and
tests Release and publishes `libdos-vX.Y.Z-win-x64.zip` (demu, dimg and dui)
with its SHA-256. dos4z pins both the version and that hash in `make-dos.ps1`,
so moving it to a new release means changing the two together there.

## Debugging A Program That Does Not Work

This is most of the work, and demu exists for it. In order:

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
- DOS: MCB allocate/free/reuse/resize, path resolution including `..` and a
  current directory below the root, both loaders including relocation,
  buffered line input (AH=0Ah), and a program running to completion with its
  output on screen.
- The keyboard: what the BIOS buffers for a key with Shift, Ctrl or Alt held,
  and that the modifiers and locks alone put nothing there.
- The shell: a program run from the prompt gets its command line, leaves its
  output on screen and returns; one that will not load is reported once.
- Sound: the AdLib detection sequence, and both the OPL and the speaker
  producing output.
- The compiler: a C program using recursion, arrays, structures, `switch` and
  the preprocessor is compiled and then **run in the emulator**, and its output
  checked. The same is done for 32-bit arithmetic (multiply, both divides,
  remainder, shifts and comparisons), for function pointers and `goto`, for
  variadic functions and for the type and width of constants. A second pair of
  programs pins `EXEC`. What it refuses is pinned too - structure assignment,
  parameters and returns, assigning to an array, a second definition of a
  function, an unterminated comment or `#if` - with the line an error at the
  end of the file is reported on.
- The disassembler, which the debugger depends on.
- DOS for the build tools: PSP:0002 covering the whole program block, the
  handle tables (a duplicated or forced handle sharing its position, files
  closed when a program ends), devices by name, names cut to 8.3, a child's
  own copy of the environment with its path, AH=47h, AH=29h, and searches a
  program abandons; COMMAND.COM /C's built-in commands, redirection, PATH
  search, exit codes and a host's own program; and the processor models.
- Disk images: a floppy's parameter block, FATs and root directory order
  (IO.SYS, MSDOS.SYS, then the label); a hard disk's cylinder-aligned
  partitions, EBR chain, partition types and CHS addresses past cylinder 1023;
  and the reader listing and reading back files, a subdirectory's included.
- Boot mode: a boot sector reading by CHS and by LBA, the LBA calls switched
  off, a floppy booting without 55AAh, INT 13h AH=08h for both kinds of drive,
  the IBM PC ROM with nothing mapped above INT 1Fh, a restart through
  FFFF:0000, and nothing to boot.

The suite cannot see the biggest consumer: demu builds MS-DOS 4.00 in
[dos4z](https://github.com/ZacWalk/dos4z) with the original 1988 toolchain,
and a change to `src/dos.cpp`, `src/command.cpp` or `src/cpu.cpp` should still
produce every one of that build's binaries byte for byte. dos4z's
`make-dos.ps1 -LibdosPath ..\libdos` builds with this checkout's demu, and its
disk images with this checkout's dimg; its `test-boot.ps1 -LibdosPath ..\libdos`
boots the result with `demu --boot` - floppies, hard disks by CHS and by LBA,
FDISK and FORMAT - and is the gate for `src/bios.cpp` and `cli/boot.cpp`.

A green build proves nothing on its own - run `.\dd.ps1 test --label libdos`.
It runs two CTest entries in each configuration: `demu_suite`, the demu
suite, and `dui_selftest`, dui's offscreen self-check, which pins the
picture scaling, that the framebuffer reaches the screen, that a repaint draws
the picture rather than leaving the window black, that a key held when the
window loses the focus is let go, that the right and middle mouse buttons are
let go, and that a close message ends the session.
There is no separate unit-test binary; a new check of
`src/`, `cc/` or `disk/` goes in the demu suite. Do not launch the GUI to verify a change;
`dui.exe -test` is the headless form and `dd.ps1 launch` is for the human.

## Known Gaps

The gap list lives in [docs/todo.md](docs/todo.md). Keep it current.
