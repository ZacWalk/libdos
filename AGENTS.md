# libdos Repository Guidance

libdos is a C++20 DOS program emulator and the tools built on it - demu, dui
and dimg - with a C compiler for 16-bit DOS and a DOS shell compiled by it.
What each directory holds, how to build and run it, and what is and is not
emulated are in the [README](README.md).

| Read | Before |
|---|---|
| [docs/design.md](docs/design.md) | changing anything in `src/`: the callback opcode, the mapping-not-emulating boundary, the plane interleave and the invariants that hold the design together |
| [docs/cc.md](docs/cc.md) | changing `cc/`, or writing C for `shell/` |
| [README.md](README.md) | building, running, debugging a program, and what demu, dui and dimg do |

## Project Boundaries

- `src/` is platform independent. No `windows.h`, no `wchar_t` in new API, no
  file-system access, no threads.
- The library takes bytes and returns bytes. Reading files is the host's job,
  through `host_services` in `src/host.h` - and in boot mode through the
  `block_device`s in `src/block-device.h` a host attaches as drives.
- `app/` (dui) gets its window, drawn menu, input, audio, dialogs and file
  paths from libdesktop and calls no operating system directly. Nothing
  Win32-shaped may go into libdesktop to make it easier; see that repo's rules.
- `host/`, `cli/` (demu) and `dimg/` own what is left of Windows: the file
  system and the console. `host/host-win32.cpp` is shared by dui and demu;
  demu's program runner adds the terminal, the fixed date and LIB's stand-in on
  top of it, and its boot mode the disk image files and the script of steps.
- `host/host-win32.h` exposes no Windows headers or handles. Its native file
  and search handles live in the implementation, so including it from dui
  does not pull the SDK across the GUI boundary. `host_header_boundary` checks
  this independently of the emulator suite.
- `cc/` is platform independent too, and opens no files: a caller supplies
  source text through `cc_sources`, the same way a host supplies the drive.
- `disk/` is platform independent as well and opens no files either: a caller
  hands in the boot code and the files' bytes, and a `disk_sink` or
  `disk_source` for the sectors. The files are `dimg/`'s.
- `shell/` is C for the emulated machine, not for the host. It is compiled by
  `cc/`, so it can only use [the C that cc accepts](docs/cc.md#the-language).
- When dui or demu needs a new capability, expose a platform-neutral operation
  from `machine.h` rather than reaching into `machine_state`.

### Why `host/` is Win32

Decided, so nobody re-opens it by accident. Everything in `host-win32.cpp` but
`code_alloc` / `code_commit` maps onto `<filesystem>`, `<fstream>` and
`<chrono>`; those two want pages that are writable and then executable, which
have no standard form. A port is standard C++ plus a small OS-specific file,
never a dependency on the desktop UI layer, which would pull Windows
entry-point details into demu. It changes two behaviours, so tests come
first: `FindFirstFileA` does the DOS wildcard matching and supplies the 8.3
short name - on a volume with short-name creation disabled it truncates the
long name to 12 characters without honouring the dot - and `std::filesystem`
has no hidden, system or archive attribute.

## Coding Guidelines

- C++20, existing naming (`snake_case` types and functions).
- `.clang-format` is the layout: tabs, Allman braces, 120 columns. It covers
  the C that cc compiles - `shell/`, `cc/lib`, `cc/include`, `cc/bench` - as
  well, since layout does not change what cc emits (`tools/cc-fingerprint.ps1`
  checks). Visual Studio installs clang-format in `VC\Tools\Llvm\x64\bin`;
  `.\dd.ps1 fmt` finds it only on PATH and formats `include`, `src` and
  `tests`, which here is `src` alone, so run `clang-format -i` on the other
  directories' files too. `.clang-format-ignore` keeps it off
  `src/rom-font.cpp`, `cc/gen.c` (whose end of prelude `tools/port-gen.py`
  finds by its indentation) and dd's templates, and a table laid out by hand -
  opcode rows, BIOS and keyboard data, a run of one-line dispatches - is fenced
  with `// clang-format off` and `// clang-format on`.
- Every target MSVC builds compiles without warnings at `/W4`, and
  `.clang-tidy` names the bug-finding and performance checks the code is kept
  clean against. A deliberate exception is a `NOLINT(check): reason` on its
  line or the line above.
- Keep it compact. Add an abstraction only when it improves the library API or
  removes real duplication.
- `src/rom-font.cpp` is generated data. Never hand-edit it; run
  `tools/make-font.ps1`.
- Never edit anything under `build/` or `exe/`.
- Debug and Release both link the static CRT (`/MTd`, `/MT`), set once in
  `CMakeLists.txt` before the platform layer is pulled in so it is built the
  same way. Changing it breaks the link.

## Build And Validation

The commands are in the README's [Build and run](README.md#build-and-run).
The [dd](https://github.com/ZacWalk/dd) build driver is vendored unmodified as
`dd.ps1` and `.dd/`, with file hashes recorded in
[docs/dd-upstream.json](docs/dd-upstream.json); never edit either. Project
settings live in `dd.psd1`; libdesktop, the one dependency, is pinned by the
`GIT_TAG` in `CMakeLists.txt`, not in `cmake/dd-dependencies.json`.
`.\dd.ps1 commands --json` lists this project's own commands. dd's
[two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes) —
CLI, and MCP for an MCP client — and its
[commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) are
documented in its own repository.

- A green build proves nothing on its own - run `.\dd.ps1 test --label
  libdos`. It runs the host-header boundary check, the demu behavioural suite
  and dui's offscreen self-check, in both configurations.
- Do not launch the GUI to verify a change - hence `--label libdos`, which
  leaves out the smoke check that opens dui's window. `dui.exe -test` is the
  headless form and `dd.ps1 launch` is for the human.
- There is no separate unit-test binary; a new check of `src/`, `cc/` or
  `disk/` goes in the demu suite.
- `tools/cc-fingerprint.ps1` says whether a change to `cc/` changed what the
  compiler emits; a difference is looked at and recorded again on purpose
  ([docs/cc.md](docs/cc.md#checking-it)).
- A sibling `../libdesktop` checkout, when there is one, replaces the revision
  pinned in `CMakeLists.txt`, so a green local build says nothing about the
  pin. After changing libdesktop, push it and re-pin here.
- A tag `vX.Y.Z` on `main` releases: `.github/workflows/release.yml` builds and
  tests both configurations and publishes `libdos-vX.Y.Z-win-x64.zip` (demu,
  dimg and dui, Release) with its SHA-256. dos4z pins both the version and that
  hash in `make-dos.ps1`, so moving it to a new release means changing the two
  together there.

## Debugging A Program That Does Not Work

This is most of the work, and demu exists for it. Work through the
[debugger's steps](README.md#the-debugger-and-the-test-suite) in order:
`--trace`, then `--trace-tail`, then `--bmp`, then `--state`.

## What The Tests Pin

`cli/main.cpp` is the contract. Before "fixing" an emulation detail, check
whether a test already encodes the current behaviour. Its `test_*` functions
cover the CPU and the processor models, interrupts and the timer, video memory
and modes, DOS memory, paths, handles and services, both loaders, COMMAND.COM
/C, the native prompt and programs run from it, the keyboard, sound, the
compiler - programs compiled and run in the emulator at both CPU levels, what
it refuses, and the x64 back end against the emulator - the disassembler the
debugger depends on, disk images and boot mode. dui's offscreen self-check is
`run_offscreen` in `app/main.cpp`. Nothing checks the planar read modes and
colour compare, CGA rendering, the MCB chain as a program walks it, the output
of `--trace`, `--info` and `--state`, the preprocessor on its own,
`cc/lib/libc.c`, or `shell/shell.c`, which only the compiler's fingerprint
compiles.

The suite cannot see the biggest consumer: demu builds MS-DOS 4.00 in
[dos4z](https://github.com/ZacWalk/dos4z) with the original 1988 toolchain,
and a change to `src/dos.cpp`, `src/command.cpp` or `src/cpu.cpp` should still
produce every one of that build's binaries byte for byte. dos4z's
`make-dos.ps1 -LibdosPath ..\libdos` builds with this checkout's demu, and its
disk images with this checkout's dimg; its `test-boot.ps1 -LibdosPath ..\libdos`
boots the result with `demu --boot` - floppies, hard disks by CHS and by LBA,
FDISK and FORMAT - and is the gate for `src/bios.cpp` and `cli/boot.cpp`.