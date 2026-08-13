# Making cc compile itself

The compiler is 6,800 lines of C++ across four files. This is the plan for
turning it into C that it can read, and then for feeding it its own source.

Two separate goals hide inside "self-hosting", and they should be kept apart or
neither gets finished:

1. **The source becomes C** - a bump allocator, interned names, sorted arrays,
   no exceptions, no templates. Worth doing on its own: it is smaller, faster
   and easier to reason about, and it is the only version of the compiler that
   could ever be its own input.
2. **The compiler compiles that source** - which is a list of missing language
   features and a width problem, not a rewrite.

Goal 1 changes every file and breaks nothing. Goal 2 changes the back end and
breaks everything at once. Do them in that order, with the test suite green at
every commit in between.

## Which self-host

Three ways this can end. They differ enormously in cost.

**A. Hosted x64, in process.** `cc` compiles its own source into an x64 image,
the CLI `code_alloc`s it and calls it, and *that* compiles `bench.c` into the
same `.COM` bytes the MSVC-built compiler produced. No linker, no executable
format, no C runtime: the driver hands the compiled compiler an arena and an
array of `(name, text)` pairs and gets back an image. `--ccnative` already does
the calling half.

This is the cheapest honest proof, and it is the recommended target. What it
needs is in "The width problem" below.

**B. Hosted x64, a real `cc.exe`.** Everything in A plus a PE/COFF writer, an
import table, and a decision about the C runtime. Nothing here is hard, but
none of it teaches anything the project does not already know. Do it only if
the compiler should escape the CLI.

**C. 16-bit DOS, inside the emulator.** The trophy, and much the furthest away.
The compiler holds the whole translation unit as tokens and seeks backwards
through them - that is its main trick, not an incidental choice - so compiling
itself means roughly 60,000 tokens resident. At 16 bytes a token that is a
megabyte, in a compiler that today emits a `.COM` with 64 KiB for code, data and
stack together. Route C therefore needs the memory model work in
[docs/todo.md](todo.md) first (far pointers or a 386 flat target), and needs it
for the compiler's *own* code as well as for Doom. Keep it on the list; do not
start with it.

A 32-bit flat variant of A is worth remembering: the same width work aimed at
`cc_machine::i386` would run in the Win32 build of the CLI, and 386 protected
mode is where the emulator is going anyway. It is the same job either way; x64
is picked because the back end exists.

## The C dialect

The source has to be readable by a compiler with no templates, no overloading
and no exceptions, so the port fixes a dialect up front and sticks to it. Every
rule below is a thing `cc` cannot do, not a matter of taste:

- No classes: plain structs and free functions taking a first parameter.
- No references: pointers.
- No overloading, no default arguments, no constructors, no destructors.
- No `enum class` - plain enums with prefixed names, as `type_kind` already is.
- No lambdas: the 34 `[&]` captures become file-static functions taking the
  generator pointer. Most are two lines and can be inlined at the call site.
- No `std::initializer_list`: `bytes({0x8c, 0xc8})` becomes a call per byte or a
  static array plus a length.
- No exceptions (below).
- Declarations at the top of a block, C89 style, until it is confirmed the
  parser accepts them anywhere.
- One translation unit: `cc/all.c` includes the other files, because there is no
  linker. That also means file-scope `static` is not private - two files may not
  define the same helper name. The MSVC build compiles `all.c` too, so the two
  paths cannot drift.

Kept, because `cc` has them: `enum`, `typedef`, `struct`, `union`, function
pointers, varargs, `long`, aggregate initialisers, function-like macros.

## Memory: one arena, a few growable lists

The user-facing shape is the one asked for.

```c
typedef struct arena
{
	char* base;
	size_t used;
	size_t size;
} arena;

void* arena_alloc(arena* a, size_t bytes);   // 8-aligned, never freed
```

Everything with a stable address and a fixed size goes here: source text,
interned names, decoded string literals, parameter lists, macro bodies.

A bump allocator cannot grow the last block once anything follows it, so the
handful of genuinely growing collections use a separate `realloc`-backed list.
One type, used by every one of them, with a macro standing in for the template:

```c
typedef struct list
{
	void* items;
	int count;
	int capacity;
} list;

void* list_grow(list* l, int item_size);  // returns the new slot
#define list_push(l, type) ((type*)list_grow(&(l), (int)sizeof(type)))
#define list_at(l, type, i) (((type*)(l).items)[i])
```

That covers the 122 `std::vector`s. Note that `list_grow` returns a pointer, so
nothing here needs a struct return.

The speculation machinery (`emission_mark`, `tell`/`rewind`) still works: it
already records counts, not pointers, and truncating a list is setting `count`.
The one thing to watch is a *saved pointer* into a list across a `list_push` -
in C++ `std::vector` had the same hazard, but the compiler currently leans on
`symbols_[i]` copies to dodge it. Every such spot must be re-read as an index.

For the preprocessor's temporary token vectors (`vector<vector<cc_token>>` for
macro arguments) a second scratch arena that is reset per expansion is simpler
than a list of lists. Decide when porting `lex`.

## Names: interning, and text that never moves

Source files are read once, into the arena, and normalised on the way in:

- UTF-16LE/BE with a BOM converted to UTF-8; a UTF-8 BOM stripped.
- CRLF and lone CR to LF.
- **Truncate at a trailing 0x1A.** Old DOS sources carry it and MSVC treats it
  as end of file, so a file that ends in one must behave the same in both
  compilers or the two builds disagree silently.
- A NUL appended, so the scanner needs no length check.

Every file stays resident for the whole compile, so a token can point into it.
Identifiers do not point into it - they intern:

```c
typedef int name_id;               // 0 means none
name_id intern(const char* text, int length);
const char* name_text(name_id id);
```

A hash table of buckets in the arena, each entry the offset of a NUL-terminated
copy. After this, an identifier comparison is `a == b`, which is what removes
the 70 `std::string`s *and* most of the 8 `std::map`s:

- `strings_`, `labels_`, `macros_` are keyed by `name_id`. Either a sorted
  array of `(name_id, value)` with a binary search and an insert, or - better
  where the key space is dense - a side array indexed by the id.
- `uses`, `rejected`, `starts` in the register allocator are keyed by an `int`
  already; a sorted array is a drop-in.
- `edges` (`map<int, vector<int>>`) becomes a flat list of `(from, to)` sorted
  once, which is also how it is consumed.

One sorted-array helper, `int lower_bound(const int* keys, int count, int key)`,
serves all of them. Insertion is a memmove; these tables are small.

`cc_token` loses `std::string text` and gains `name_id name` plus, for a string
literal, an `(offset, length)` into an arena buffer of decoded bytes. That takes
the token from ~56 bytes to 20 and is most of the compile-time win.

`cc_result` loses its strings too: a fixed `char message[256]`, a `name_id file`,
and the function map as an arena array of `{name_id name; int offset;}`.

`cc_sources` is a virtual class; in C it becomes what `host_services` is to the
library - a struct of a function pointer and a `void* user`. For route A the
driver can go further and pre-load every source into the arena, so the compiled
compiler makes no callbacks at all and needs no runtime whatsoever.

## Errors without exceptions

There are five `throw`s and one `catch`. `setjmp` is not in `cc/include`, and
adding it is real work on a target with no stack unwinding, so the port uses a
flag instead:

- `generator.failed` plus the message, line and file already carried by
  `compile_error`.
- `fail()` records the first failure and sets the flag. It does not return
  anything, so every caller has to be able to carry on.
- After failure `peek()` reports `tok::end` and `accept()`/`expect()` are no-ops.
  Every parse loop in the file already terminates on `tok::end`, so the parser
  unwinds by itself; only the loops that are not driven by the token stream need
  an explicit `if (g->failed) break`.
- Nothing may index an array with a value derived from a failed parse. Auditing
  that is the real cost of this change, and it is worth doing carefully: it is
  the difference between a bad error message and a crash.

## Order of work

Every step keeps `.\dd.ps1 test` green. The trick that makes this possible: C
compiles in the existing project. Add a `.c` file with
`<CompileAs>CompileAsC</CompileAs>` - the project passes `/TP` to everything
otherwise - wrap its header in `extern "C"`, and MSVC builds it as C while
`cli/main.cpp` stays C++.

The second trick is what makes the enums affordable. Renaming `enum class tok`
to a C enum would touch 577 `tok::x` sites in the parser. Instead the C header
declares `cc_tok` with `tok_x` enumerators and, for C++ only, a `namespace tok`
holding `const cc_tok end = tok_end;` and the rest. Every one of those 577 sites
keeps compiling untouched, a const enum variable is still a valid `case` label,
and the only edits needed are the places `tok` was used as a *type* - a
namespace and a type cannot share a name. The shim goes when the file that needs
it becomes C.

Where a shim will not do - `op::mov` had to become `op_mov` because `op` is also
a type the parser names - the rename is done by a throwaway Python script and
checked by the fingerprint. That is what the emitter's 490 call sites took: one
script, one build, no diff in the output.

0. **Pin the current behaviour.** `tools/cc-fingerprint.ps1` compiles a corpus -
   bench.c at three settings, the shell, hello - and records the size and hash
   of each `.COM`, plus what `--ccbench` costs to run. `-Save` re-records it.
   The port is not meant to change a byte of any of them. *(done)*
1. **Infrastructure.** `cc/support.{h,c}`: the arena, `cc_list`, the sorted
   `cc_map`, interned `cc_names`, and the source normaliser. Unit-tested from
   `cli/main.cpp`. *(done)*
2. **`target.h`** - already almost C. Dropped `enum class` and the unused static
   factory; the defaults moved into `cc_target_init`. *(done)*
3. **`sources.h`** - the virtual `cc_sources` became a struct holding a `load`
   function pointer, which a host embeds as its first member. *(done)*
4. **`lex.h`/`lex.c`** - the tokeniser and preprocessor. A token now carries an
   interned `name` for comparison and a `text` pointer for printing, and a
   string literal carries its length because it may contain a NUL. The
   file-name table is gone: a token's `file` is an interned pointer. *(done)*
5. **`emit.h`/`emit.c`/`emit-x64.c`** - the instruction list and both encoders.
   The operand factories became `operand_in(int r)` and the ~140 shape helpers
   `emit_*(emitter* e, ...)`; the five vectors became `cc_list`s reached through
   `EMIT_CODE`/`EMIT_LABEL`/`EMIT_IMAGE`. The 490 call sites in the parser were
   rewritten by a one-shot Python script rather than by hand. *(done)*
6. **`gen.cpp`** (4,400 lines), the bulk. `class generator` becomes
   `struct generator` and every method a function taking `generator* g`. The
   translation is machine-made - `tools/port-gen.py` turns `generator::foo` into
   `gen_foo(generator* g, ...)`, gives every member and call the `g` it needs,
   and converts C++ references into pointers in the signature, in the body and
   at every call site. `cc/gen.c` is not in the build until it compiles, so the
   suite stays green the whole way through; the two are swapped in one commit
   at the end. *(in progress)*

   What a script cannot do is decide, so the constructs that need decisions -
   range-for loops, lambdas, local `const T&` bindings, std::vector, std::string,
   std::map and throw - are removed from `gen.cpp` **first**, where each change
   is still valid C++ and the fingerprint says whether it changed anything.
7. **`cc.h`** - the public header. `cc_result` owns an arena now, and the image,
   the message and the function map all point into it. *(done)*
8. **`build-cc.ps1`** (below), building the same sources outside MSBuild.

At the end of step 8 nothing self-hosts yet, the compiler is C, the tests still
pass, and the fingerprint is unchanged.

## What `cc` still cannot compile

The feature list for goal 2, in the order it is needed:

- **Struct assignment, struct parameters, struct returns.** Unavoidable:
  `operand` and `instruction` are passed and returned by value everywhere, and
  writing that style out of the source would disfigure it. Caller copies
  arguments; a return larger than the accumulator pair goes through a hidden
  pointer argument. This is the single biggest item and it belongs in the
  compiler regardless - `docs/todo.md` has listed it since the beginning.
- **The width problem.** Today the front end makes `int` 2 bytes and pointers 2
  bytes on every target, and the x64 back end emits 66h-prefixed 16-bit
  operations so the flags match the interpreter's. A compiler that addresses a
  megabyte of arena cannot do that. What is needed:
  - `type_info` sizes taken from `cc_target` rather than assumed - `int_size`,
    `pointer_size` and `native_width` exist already and are not honoured.
  - The generator's hard-coded width 2 in `emit(op, 2, ...)` replaced by the
    target's width. This is a large but shallow edit.
  - 32-bit arithmetic on x64 stops going through the DX:AX helpers and becomes
    one instruction; the helpers stay for the 8086.
  - The x64 encoder grows REX.W and 32/64-bit forms. `instruction.fixup` already
    tells `resolve()` where the address went, so this does not disturb layout.
  - Pointer arithmetic scales by the element size at pointer width, and casts
    between a pointer and an integer keep the width.
- **A runtime with no operating system.** `memcpy`, `memset`, `memmove`,
  `strlen`, `strcmp`, `memcmp` and a small `snprintf`, all in C in `cc/lib`,
  compiled into the image. Route A needs nothing else: no `malloc` (the arena is
  handed in), no file I/O (the sources are handed in), no `exit`.
  `realloc` for the growable lists is the one exception - either pass a
  `void* (*grow)(void*, size_t)` in with the arena, or give the lists a
  chunked arena-backed growth strategy and drop `realloc` entirely. The second
  is more in keeping and removes the last host dependency.
- Smaller ones, each to be confirmed against the ported source rather than
  guessed at: declarations after statements, `sizeof` on an expression with a
  call (there is a known stale-fixup bug), string literal concatenation, `#` and
  `##` (the preprocessor has neither - the port must not use them), aggregate
  initialisers nested two deep, and `switch` on an enum wider than 16 bits.

## The bootstrap and how it is checked

```powershell
.\build-cc.ps1                 # cl /TC over cc\all.c -> bin\cc-msvc.exe
.\build-cc.ps1 -Bootstrap      # cc-msvc compiles cc\all.c -> stage 2
.\build-cc.ps1 -Fixpoint       # stage 2 compiles cc\all.c -> stage 3
```

The proof is `stage2 == stage3`, byte for byte. Stage 1 is a different compiler
(MSVC) so its output will differ; stage 2 and stage 3 are the same compiler
compiling the same source and must agree exactly. If they do not, the compiler
miscompiles itself and the difference in the image says where.

A weaker check that arrives much earlier, and is worth wiring into
`ovrwincli --test` as soon as route A runs at all: compile `bench.c` with the
MSVC-built compiler and with the stage-2 compiler and compare the two `.COM`
images. That exercises the whole front end and every back end path without
needing the compiler to be able to read its own source yet.

`build-cc.ps1` stays deliberately small - find `cl` through `vswhere` the way
`dd.ps1` does, one compile of `cc/all.c`, one of a driver, `/W4 /WX /std:c17`,
no MSBuild. The emulator keeps its solution; only the compiler gets a script.

## What to be suspicious of

- **The fingerprint is the whole safety net for goal 1.** Any commit that
  changes it has changed behaviour, whatever the tests say.
- Saved pointers into a `list` across a push. C++ hid some of these behind
  index-based access; C will not.
- Interning changes iteration order anywhere a `std::map<std::string, ...>` was
  walked. If the output depends on that order - the string table and the
  function map are candidates - the fingerprint will catch it, and the fix is
  to sort explicitly rather than to keep the map.
- The register allocator and `fold_address_copies` are pointer-heavy passes over
  the instruction list and are where a port bug will hide. They already have the
  most specific failure mode in the project: every compiled program breaks.
- Route A's compiled compiler runs with **no guard pages and no unwind data**,
  so a fault kills the CLI outright (see `docs/design.md` and the SEH note in
  the repository notes). Keep it behind its own command so a broken stage 2 can
  be bisected without taking the test suite with it.
