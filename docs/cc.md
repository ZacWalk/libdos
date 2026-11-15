# cc, the C compiler

`cc/` compiles a C program into a DOS `.COM`, or into x64 code for the machine
it runs on. It is the `libdos_cc` library: platform independent, and it opens
no files - a caller hands it source text through `cc_sources`
(`cc/sources.h`) and gets the image back from `cc_compile` (`cc/cc.h`). demu
is its command line; the README shows
[how to run it](../README.md#building-dos-programs).

It is here because the DOS side of this project - the shell, test programs -
is far easier to write in C than in hand-assembled bytes, and because a
program compiled here and run in the emulator exercises the CPU, the loader
and the DOS layer against each other.

## The language

What `cc` accepts:

- **Types**: `char` (signed) and `unsigned char`; `short` and `int`, 16 bits;
  `long`, 32 bits, which `long long` also means; their `unsigned` forms;
  pointers, arrays of any dimension, `struct`, `union`, `enum`, `typedef` and
  function pointers. Structures are packed to the byte, as `-zp1` packs them,
  and there is no way to ask for alignment.
- **Declarations** anywhere in a block and in a `for` initialiser, `static`
  locals, `extern`, and initialisers for arrays and structures, nested. A call
  to a name never declared declares it, returning `int`.
- **Statements and operators**: all of C89's, `goto`, `switch`, `?:` and the
  comma operator included.
- **Variadic functions**: `...`, with `<stdarg.h>` built on the `__args`
  builtin.
- **Constants**: integers in decimal, octal, hex and binary (`0b`) with `U`
  and `L` suffixes - a decimal one too big for an `int` is a `long`; character
  constants; string literals, adjacent ones joined. The escapes are
  `\n \t \r \b \a \f \v \e \0`, and a backslash before any other character
  gives that character. A floating constant is allowed only in a constant
  expression, which folds in `double` and has to be cast to an integer before
  anything asks for it in a register: `(int32_t)(1.02*FRACUNIT)`.
- **Qualifiers**: `const`. `volatile`, `register`, `inline`, `restrict`,
  `_Noreturn`, `__cdecl`, `__interrupt` and the memory-model keywords
  (`__far`, `__near`, `__huge` and their one-underscore forms) are accepted and
  ignored, and `__attribute__(...)` expands to nothing.
- **The preprocessor**: `#include`, searched in the source's own directory and
  then each `-I` directory; object-like and function-like `#define`;
  `#undef`; `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`; `#error`.
  `#pragma` and `#line` are ignored. `#if` understands constants, `defined`,
  unary `!` and `-`, and the binary arithmetic, bitwise, comparison and logical
  operators - a shift, `~` or `?:` in one is an error or a wrong answer.
  `__LIBDOS__`, `__DOS__` and `_M_I86` are predefined, and `-D name[=value]`
  defines more.

What it refuses:

- `float` and `double`. There is no floating point at run time.
- Structure assignment, structure parameters and structure return values.
- Bit-fields, designated initialisers, compound literals and K&R-style
  definitions.
- The `#` and `##` operators, and variadic macros.
- A cast to a function-pointer type spelt out, `(int (*)(int))p`. The same
  type through a `typedef` works.

What it compiles without a word, and gets wrong:

- A `\x` escape, or an octal one other than `\0`: the backslash is dropped, so
  `"\x41"` is `x41`.
- A structure initialised from another, `struct p b = a;`, is not a copy.
- A call is not checked against its prototype's parameter count.
- Constant folding does not wrap to 16 bits: `1 << 15` folds to 32768, so
  `(1 << 15) == -32768` is false, where a 16-bit compiler wraps.
- `switch` compares only the low 16 bits of its controlling expression.
- A file-scope `static` is not private: a program is one translation unit
  with one namespace, so two included files defining the same name collide.

## A program

A program is the tiny model: code, data and stack share one 64 KiB segment,
loaded at 100h. There is no far pointer, so a program cannot address video
memory directly, and no linker, so `#include` is the only way to combine
sources. The startup shrinks the program's memory block with INT 21h AH=4Ah,
so that a program it starts has somewhere to load, then calls `main` and exits
with what it returned.

The runtime is a handful of builtins rather than a library:

| Builtin | What it does |
|---|---|
| `__int21(ax, bx, cx, dx)`, `__int21si(ax, bx, cx, dx, si)` | INT 21h, returning AX; `__cf`, `__bx`, `__cx` and `__dx` then hold what DOS left |
| `__int10(ax, bx, cx, dx)` | INT 10h |
| `__int13(ax, bx, cx, dx)`, `__int13si(ax, bx, cx, dx, si)` | INT 13h. They put DI back, since AH=08h returns a table in ES:DI and a function may keep a variable in DI |
| `__seg()` | the program's segment, for handing DOS a far pointer |
| `__args(last)` | the address of the arguments after `last` |

A builtin the program never calls is left out.

`cc/include` holds the headers, which are declarations only. `cc/lib/libc.c`
defines the memory and string functions, `putchar`, `puts`, `printf`,
`sprintf`, `snprintf`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`,
`malloc`, `calloc`, `free`, `exit`, `abort`, `atoi`, `atol`, `abs`, `labs`,
`rand`, `srand` and `getenv`; a program includes it once (`#include <libc.c>`,
compiled with `-I cc\include -I cc\lib`). In it, `malloc` is a bump allocator
over a static block of `HEAP_SIZE` bytes (4096 unless defined) and `free` does
nothing; `getenv` always fails; `printf` handles `%d %i %u %x %X %c %s %%` and
`l`, and skips flags, width and precision; and `fopen` in append mode moves to
the end once, when the file is opened, not before every write. What the
other headers declare - `<ctype.h>`, `<conio.h>`, `<dos.h>`, `<i86.h>`,
`<time.h>`, `<unistd.h>`, `<signal.h>` - has no definition.

## How it compiles

The shape is Fabrice Bellard's Tiny C Compiler's: one pass, no syntax tree,
code generated as the parser goes. None of TCC's code is used - it is LGPL and
this is not - only the idea that a useful C compiler needs no syntax tree.

The parser carries a small `value` describing where the result of the
expression it just parsed is: a constant, AX, a frame slot, a global, a
dereference, or a comparison still in the flags. `load` turns any of those into
a value in AX and `store` puts AX back. A binary operator pushes its left
operand, evaluates the right into AX, then pops the left back into AX with the
right in CX - bigger code than a real compiler's, and correct by construction.
A 32-bit value is the same one register wider: DX:AX, with CX:BX for the second
operand. Add, subtract and the bitwise operators are a pair of instructions;
multiply, both divides, both remainders, the shifts and the comparisons call
helpers emitted after the program, and only if something called them.

Four things are worth knowing:

- **Arguments are generated backwards.** cdecl pushes them right to left, and
  because the parser walks a token vector rather than a stream it can find the
  argument spans first and then generate them in reverse by seeking.
- **A loop's test comes after its body**, found the same way, so an iteration
  costs one conditional jump.
- **`switch` is dispatched after the body**: a forward jump to a chain of
  compares emitted once the case labels have been seen.
- **Constant folding needs the right operand before it can decide.** The
  parser generates the left operand speculatively, parses the right, and if
  both are constant discards the code and folds. That is what lets
  `16*FRACUNIT` or `sizeof(a)/sizeof(a[0])` reach an array bound or a case
  label.

The generator does not produce bytes but a list of instructions in the x86
family's own vocabulary - an operation, a width and up to two operands, where
an operand that means an address carries a reference instead of a number
(`cc/emit.h`). Only the back ends know an opcode. That leaves the whole program
to the passes after the parse:

- **Layout** (`emit_lay_out`) starts every branch at its longest and shortens
  it while its target is in reach - an 8086 has no near conditional branch, so
  a long one is a short branch over a near jump. A branch to the next
  instruction goes altogether, a function nothing can reach is left out
  (`--keep-unused` keeps it), and every label is placed again. `--map` lists
  what each function cost.
- **Inlining** copies a defined function to its call site when its body is
  at most `--inline <n>` tokens (24 by default; `--no-inline` turns it off)
  and uses no `goto`, `static` or `__args`. A constant argument stands in for
  a parameter the body never writes.
- **Registers**: per function, over its finished instructions, the two
  busiest frame slots move into SI and DI for the whole body, a use inside a
  loop - a branch back to code already generated - counting far above one on
  a straight line. A backward liveness pass then lets a pointer in a register
  address memory itself.

The parse itself avoids the stack where it can: an operand that generates no
code is placed rather than pushed, so a constant folds into the instruction;
a comparison stays in the flags; a constant index or member offset folds into
the address; a statement whose result nobody wants is applied straight to
memory. Nothing is hoisted out of a loop.

## Back ends

| | Source | Output |
|---|---|---|
| 8086 | `cc/emit.c` | A `.COM`. The default |
| 386 | `cc/emit.c`, `--cpu 386` | Still a 16-bit `.COM`, with `SETcc` for a branch over a clear, `MOVZX`/`MOVSX` for a load and a fix-up, and a near conditional branch |
| x64 | `cc/emit-x64.c` | Code for the machine the compiler runs on |

The x64 back end encodes the same list, so its code is still the tiny model:
the host gives it a 64 KiB block and R15 points at it, every memory reference
is `[r15 + offset]`, and a pointer stays the 16-bit offset the front end makes
it. SP and BP are R14 and R13, operations are 16 bits wide so the arithmetic
and the flags are the interpreter's, and the host's stack still carries the
return addresses. A DOS call writes the registers to an agreed place at the
top of the block and calls through a function pointer beside them
(`CC_NATIVE_*` in `cc/cc.h`).

- `demu --ccnative <file.c>` compiles for x64, runs the code in memory from
  `host_services::code_alloc` and reports what `main` returned. Its DOS is
  INT 21h AH=02h, 09h, 3Ch-41h and 4Ch, so the shell does not run this way.
  The code has no unwind data, so a fault in it ends demu.
- `demu --ccexe <file.c>` writes a Windows executable: one section holding the
  block, a startup that hands the program its base, and a service routine for
  AH=02h, 09h and 4Ch through `WriteFile` and `ExitProcess`. It has no
  relocation table, so it loads only at its image base.

## Checking it

The demu suite compiles programs and runs them in the emulator at both CPU
levels, checks what the compiler refuses, and compiles one program for both
the 8086 and x64 and checks the two agree. `cc/bench/bench.c` is the yardstick
for the code generated: the shapes a game's inner loops are made of, with a
checksum in the exit code; `demu --ccbench cc\bench\bench.c` reports its size
and what it costs to run. `tools/cc-fingerprint.ps1` records the size and hash
of what the compiler emits for a fixed corpus, and what the benchmark answers,
in `tools/cc-fingerprint.txt`; a difference is a change of output, to be looked
at and recorded again on purpose (its header shows how).

## Its own source

`cc/` is C apart from `gen.cpp`, the parser and code generator, which is C++.
`support.h` gives the C files an arena, a growable list, a sorted map and
interned names, and `cc_normalise_source` hands the scanner one encoding: UTF-16
with a byte order mark becomes UTF-8, a UTF-8 mark is dropped, CRLF and a lone
CR become LF, and the text stops at the first Ctrl+Z. `cc/gen.c` is a C version
of `gen.cpp` that the build does not use; `tools/port-gen.py` does the
mechanical part of that translation. `cc` cannot compile its own source.
