# ovrwin JIT plan

A plan for translating 8086/80186 real-mode code to x64 at run time. Read
[design.md](design.md) first: everything below has to hold the invariants it
describes, in particular that `src/` is platform independent, that
`mem_read8`/`mem_write8` are the only path to memory, and that nothing in the
emulator blocks.

## The goal, stated as a number

Today `machine_state::cpu_hz` is 8 MHz and `app` calls
`exec(cycles_per_frame())` once per frame. The emulated machine is therefore
paced at an XT's speed whether or not the host could do better, and until
Phase 0 the interpreter had never been asked how much faster it could go.

The goal of this phase is not "add a JIT". It is:

> **Sustain 60 frames a second with `cpu_hz` at 25-33 MHz, on one core, with
> the existing test suite green under both engines.**

That is 386DX territory - enough for the games that are out of reach now. Every
phase below is judged against that number, and the first phase exists purely to
measure it.

## Phase 0: what the measurements say

`ovrwincli --bench <file> [--seconds n] [--warmup n] [--cpu-hz n]` runs a
program headlessly with no 60 Hz pacing and reports guest instructions/second,
guest cycles/second and the `cpu_hz` a 60 Hz frame could afford. It runs the
same workload twice, once with `vga_render` on and once with it off, so the
cost of the frame conversion falls out of the difference.

Release x64, warmup 600 frames, 3 s per pass:

| program | mode | instructions/s | no render | `vga_render` | sustains |
|---|---|---|---|---|---|
| Battle Chess | 13h | 20-24 M | 27-46 M | 28-47% | 250-310 MHz |
| Wasteland | 0Dh EGA | 19.5 M | 29.8 M | 34% | 193 MHz |
| Elite | 03h | 35.0 M | 43.3 M | 19% | 483 MHz |
| synthetic integer loop | 03h | 33.6 M | 40.5 M | 17% | 299 MHz |

The synthetic loop is a `cc/`-compiled program doing table lookups and integer
arithmetic with no I/O, and it is there to rule out the obvious objection: that
the games are idling at a title screen and the numbers are measuring a
five-instruction polling loop. They are not. The dispatch rate is the same
whatever the workload, which makes **~35 M guest instructions per second** the
number that matters.

The passes are noisy - Battle Chess moved between 47% and 28% render across
runs - because the workloads are not steady. Treat a single figure as
indicative and re-run before drawing a conclusion from a small difference.

### The verdict, which is not what this plan assumed

Against that ceiling, expressed in the only unit that does not depend on the
emulator's own cycle model:

| target | roughly | headroom |
|---|---|---|
| 8086 at 8 MHz | 0.4 MIPS | 85x |
| 286 at 12 MHz | 1.5 MIPS | 23x |
| 386 at 33 MHz | 6 MIPS | 6x |
| 486 at 66 MHz | 25 MIPS | 1.4x |
| Pentium 90 | 70 MIPS | 0.5x |

**Throughput is not what stops a more advanced game running here.** For
everything ovrwin can execute at all - 8086 and 80186 real mode - the
interpreter has one to two orders of magnitude of margin, and at the plan's own
target of 25-33 MHz it has six. What actually blocks the next tier of games is
capability: 386 instructions, protected mode and DPMI, mode X, VESA and Sound
Blaster. A DOS/4GW game does not run slowly today, it does not run at all.

The JIT only starts to earn its place at a fast 486 and above, and every game in
that class needs the 386 and protected-mode work first. So the order is the
opposite of what this document originally assumed:

1. 386 instructions and protected mode, interpreted.
2. DPMI, so a DOS extender loads.
3. Re-run `--bench` against a real 32-bit workload. 32-bit code executed by a
   16-bit-shaped interpreter is where the margin above will actually go.
4. Then, and only against that measurement, the JIT below.

The rest of this document stands as the design for step 4. It is worth keeping
current, because the decisions in it - block keying, invalidation, flag
handling - are easier to honour while the 386 work is being written than to
retrofit afterwards.

### The one thing worth fixing now

`vga_render` is 17-47% of the frame, and the spread is the interesting part.
It is a fixed cost of 60 full-screen conversions per emulated second, so its
share falls as `cpu_hz` rises: at `--cpu-hz 33000000` Battle Chess's render
cost drops into the measurement noise. It looks expensive at 8 MHz only
because an 8 MHz 8086 does so little in a frame - about 10 000 instructions -
that a 64 000-pixel palette conversion is comparable work.

So it is not urgent, and it is also nearly free to fix: `vga_state::dirty` is
already maintained and not read, and a mode 13h frame that has not changed does
not need converting at all.

## Why the JIT is easier here than it looks

The guest is x86 and the host is x86. That is worth more than any other single
fact in this document:

- **Arithmetic and flags translate one to one.** x64 has the same CF, PF, AF,
  ZF, SF and OF, with the same semantics, for 8- and 16-bit operands. An
  emitted `add word ptr [regs+bx], ax` sets all six exactly as an 8086 would.
  All the careful hand-computed flag logic in `cpu.cpp` -
  `set_of_add`, `do_af`, `parity_table` - disappears in translated code, and
  with it a whole class of emulation bug. Flags are normally the hardest and
  slowest part of a JIT; here they are free.
- **Real mode has no paging, no privilege and no fault to reconstruct.** The
  address space is 1 MiB of flat array plus a wrap at the top, already
  allocated as one block with the wrap region included.
- **The register file is small enough to leave in memory.** Eight 16-bit
  general registers and four segments, contiguous in `cpu_state::regs`. x64
  operates on 16-bit memory operands directly, so a first working translator
  needs no register allocator at all.

What is genuinely hard is everything around the translated code: self-modifying
code, cache invalidation, interrupt latency, and keeping cycle accounting
faithful enough that the timer tests still pass.

## Where the code lives

The platform boundary is the first design question and it has a clean answer.
Emitting x64 machine code is byte manipulation - it needs no Windows header.
Only two things are host business: getting a page that is both writable and
executable, and telling the CPU the code changed.

```
src/jit.h        engine-neutral interface: jit_create, jit_flush, jit_exec
src/jit-x64.cpp  the translator and the byte emitter. No windows.h.
host/            implements the two new host_services calls
```

Three additions to `host_services`, defaulted so existing hosts keep compiling:

```cpp
virtual void* code_alloc(uint32_t size) { (void)size; return nullptr; }
virtual void code_free(void* p, uint32_t size) { (void)p; (void)size; }
virtual void code_commit(void* p, uint32_t size) { (void)p; (void)size; }
```

`code_alloc` returning `nullptr` means "no JIT", and the machine falls back to
the interpreter with no further ceremony. `code_commit` is where a host that
uses W^X flips protection and flushes; on Windows with a plain RWX page it does
nothing. That keeps `src/` clean and keeps the fallback honest.

**The interpreter stays.** It is the reference implementation, the oracle for
differential testing, the only engine for `--trace` and `--trace-tail`, and the
engine for every non-x64 build, so the whole of `jit-x64.cpp` sits behind
`#if defined(_M_X64) || defined(__x86_64__)`. The dd presets build x64 only, so
no 32-bit build checks that guard today.

`machine_state` gets one opaque pointer, `jit_state* jit`, owned by `machine`
and explicitly not part of the observable machine: `machine_reset` flushes it,
and anything that copies or serialises a `machine_state` must treat it as
non-transferable.

## The execution model

`machine::exec` keeps its shape - budget in, cycles out, render at the emulated
retrace - and gains a dispatcher in place of the `cpu_step` loop:

```
while (spent < budget && !exited)
    if an interrupt is pending and IF is set -> take it, continue
    block = lookup(linear(cs, ip))          // hash on the linear address
    if (!block) block = translate(...)
    spent += block->run(state)              // returns cycles consumed
    pit_step(state, cycles)
```

Decisions this encodes:

- **Blocks are keyed by linear address**, not `CS:IP`. Real-mode code is
  routinely reached through more than one segment:offset pair, and keying on
  the pair translates the same bytes repeatedly and, worse, invalidates only
  one of the copies.
- **Interrupts are checked between blocks, not between instructions.** Blocks
  end at every control transfer, so a loop cannot run unbounded without
  reaching the dispatcher. This changes interrupt latency from "one
  instruction" to "one block", which is under 64 instructions and closer to
  real hardware's prefetch-queue behaviour than the current model is.
- **Cycles are accumulated statically.** Each block carries the summed cost of
  its instructions, computed at translation time from the same table the
  interpreter uses, plus a runtime addition for a taken branch and for REP
  iterations. `cpu.cycles` and `s.cycles` advance by that at block exit. The
  timer test in `cli/main.cpp` pins channel 0's rate and is the check on this.
- **`pit_step` runs once per block**, with the block's whole cost. It already
  carries a fractional remainder, so no time is lost.

## Block formation

A block starts at a linear address and ends at the first of:

- any control transfer: `JMP`, `Jcc`, `LOOP`, `CALL`, `RET`, `INT`, `IRET`,
  `JCXZ` - taken or not, the block ends at the instruction;
- the callback opcode `F1`, because a callback may change `CS:IP` arbitrarily
  (`EXEC` does exactly that) and must return to the dispatcher;
- `HLT`, `POPF`, `STI`, `CLI`, or any write to a segment register, if the block
  caches segment bases (see below);
- a page boundary, so an invalidation never has to consider a block that spans
  two pages;
- 64 instructions.

Ending at every branch means no block-internal loop exists, which is what makes
one interrupt check per block sufficient.

## Translation: three tiers, introduced in order

**Tier 0 - a block of calls.** The first translator emits, for every guest
instruction, a call to the existing interpreter for that one instruction, then
the epilogue. It is no faster than the interpreter and possibly slower, and it
is the most valuable single step in this plan: it exercises the code buffer,
the block cache, the hash, invalidation, dispatch, cycle accounting and
interrupt latency with **zero code-generation risk**. If the test suite is
green here, every later bug is in one opcode's codegen and nowhere else.

**Tier 1 - the arithmetic and move core, natively.** `MOV`, the eight ALU
operations, `INC`/`DEC`, `TEST`, `CMP`, `XCHG`, `PUSH`/`POP`, `LEA`, the
shifts and rotates, `Jcc`, `LOOP`, `CALL`/`RET` near, `NOP`. Anything not
covered still emits a call to the interpreter helper, so a mixed block is
normal and the tier can land opcode by opcode.

Guest registers stay in `cpu_state::regs`, addressed off a pinned host register
(`r14` = `&cpu_state`, `r15` = `&s.memory[0]`, `rbx` = `&machine_state`). A
guest `ADD BX, SI` becomes one instruction:

```
66 03 5E 06        add bx16, word ptr [r14 + 6]   ; with bx16 loaded from [r14+3*2]
```

Flags are the interesting part. Keep them in the **host** `EFLAGS` across
consecutive translated instructions and materialise them into `cpu.flags` only
when something reads them: a helper call, a block exit, `PUSHF`, `LAHF`, or an
instruction that consumes CF (`ADC`, `SBB`, `RCL`). Two rules make this safe:

- The guest flags word is `(arithmetic flags, from the host) | (control flags:
  IF, DF, TF, held in `cpu.flags` and never touched by arithmetic)`. Merge on
  materialisation, never assume the word is whole.
- Materialise with `pushfq` / `pop`, not `LAHF`+`SETO`. `LAHF` in 64-bit mode
  is an optional feature that early AMD64 and EM64T parts lack, and OF needs a
  second instruction anyway.

Note the length-changing-prefix stall: `66` with a 16-bit immediate is slow to
decode on Intel parts. Where a 16-bit immediate is hot, emit `mov r32, imm32`
and then a register-form 16-bit operation.

**Tier 2 - memory operands and string instructions inlined.** A guest memory
access becomes: compute the effective address with 16-bit arithmetic so it
wraps as an 8086's does, zero-extend it, add the segment base, then

```
cmp eax, 0xa0000
jae  slow_path            ; display adapter and ROM
mov  dx, word ptr [r15 + rax]
```

The branch predicts perfectly for the overwhelming majority of accesses, and
the slow path is a call to `vga_mem_read`/`vga_mem_write`, which stays the only
implementation of the planar logic. Segment bases (`seg << 4`) are computed at
block entry into host registers, which is why a write to a segment register
ends a block.

String instructions are where the frame actually goes, and they need care
because the current design deliberately executes **one repetition per step** so
an interrupt can land mid-`REP` - the bug that made Elite report "packed file
is corrupt" was abandoning a `REP` instead of rewinding to its prefix. The JIT
must keep that observable behaviour while not paying for it per byte. The
answer is a chunked helper:

```
iterations = min(cx, cycles_until_the_next_timer_event / cost_per_iteration)
```

Run that many in a tight native loop, update `CX`, `SI`, `DI`, add the cycles,
and return to the dispatcher. Nothing observable changes - the interrupt still
arrives at the right emulated time, with `CX` at the right value and `IP`
rewound to the prefix - and a 32 000-byte screen blit costs one helper call
instead of 32 000 dispatches. For a `REP MOVS` whose whole source and
destination range is plain RAM, the helper degenerates to a `memcpy`; for one
targeting `A000` in chain-4 mode it degenerates to a `memcpy` into the
interleaved plane array. That pair of special cases is probably worth more than
the rest of Tier 2 put together.

**Tier 3, only if the numbers still demand it.** Block linking (patch a block's
exit to jump straight into its successor's entry, bypassing the dispatcher and
the hash lookup) and per-block register pinning of the two or three hottest
guest registers. Both are real wins and both make debugging harder, so they
come last and only against a measurement.

## Self-modifying code, which is the part that will bite

DOS programs modify their own code constantly, and this emulator meets it
immediately: every packed executable - EXEPACK, LZEXE, PKLITE - writes its real
code into memory and jumps to it, and `EXEC` and overlay loading rewrite whole
regions.

The hope that static analysis could find these sites at translation time and
model them is worth stating precisely, because half of it is right and the
other half cannot be made sound. A store's target is a run-time value:
`mov [bx+si], al` is not decidable at translate time, and the usual escape - "a
stack write cannot be code" - is not available here, because in a `.COM` file
`CS`, `DS`, `ES` and `SS` are all the same segment. So the *mechanism* has to
stay dynamic. What analysis buys is the **policy**: not whether to watch for a
write, but what to do about the block when one arrives.

The watch itself is unavoidable and cheap:

- A **code bitmap**, one bit per 1 KiB of the 1 MiB address space (128 bytes
  total), marks pages a translated block was built from.
- `mem_write8` gains one test against that bitmap. It already does a range test
  on every write, so this is a predictable branch on a hot but already-branchy
  path. A marked page invalidates every block starting in it - hence blocks
  never spanning a page boundary.
- **Every path that writes guest memory must go through it.** Audit
  `machine.cpp`'s loader, `dos.cpp`'s file reads and `machine_install_vector`
  for direct `s.memory[...]` writes and `memcpy`, and route them through a
  single `mem_write_block` that invalidates. A write that bypasses the bitmap
  produces a program that runs stale code, and that failure looks like a random
  hang a long way from its cause.

On top of that, three behaviours to model, because they want opposite
treatment:

| | what it looks like | policy |
|---|---|---|
| never written | almost everything | translate normally; one predictable branch per guest store is the whole cost |
| written once, then stable | an unpacker stub, an overlay load, `EXEC` | throw the blocks away and retranslate. Bounded, happens at load, needs no cleverness |
| written repeatedly | a patched immediate in an inner loop, a hand-unrolled blitter that rewrites its own body | retranslating is a treadmill. Count invalidations per block and, past a threshold, mark that **address** volatile and interpret it from then on, permanently |

The third row is the part that is genuinely modelled rather than merely
detected, and it is hysteresis, not analysis: a counter and a threshold. It
degrades to the interpreter's speed for the handful of addresses that deserve
it, which is exactly right, because those addresses were never going to be
worth translating.

Analysis at translation time then earns its keep in two narrow, cheap places:

- A store with a **constant displacement** into a page the block was itself
  translated from - `mov [1234h], al` with `DS` equal to `CS` - is a self-patch
  site the moment it is decoded. Mark the block volatile there and then rather
  than discovering it through three flushes.
- A block that ends by **jumping into a region it has just written** with a
  string instruction is the unpacker shape. Do not translate the writing block
  at all: a copy loop that runs once costs nothing interpreted, and translating
  it only to discard it is pure loss.

Neither is required for correctness. Both are policy hints that the bitmap
would eventually reach anyway, which is the right shape for this kind of
optimisation - if the analysis is wrong, the machine is slow, never incorrect.

Two supporting pieces:

- `--jit-stats` reports blocks translated, blocks flushed, blocks marked
  volatile and bytes of code cache used. An unpacker flushing a few hundred
  blocks at load is correct behaviour; a game flushing thousands a frame is a
  bug in the granularity, and the counter is how you tell those apart in a
  second.
- The code cache is a fixed arena (start at 32 MiB). When it fills, flush all
  of it and start again. Per-block reclamation is not worth the bookkeeping.

All of this - the bitmap, the counters, the volatile policy and the statistics -
belongs in **Tier 0**, where blocks are still just sequences of calls into the
interpreter. That is the whole point of Tier 0: the invalidation model can be
made right, and shown to be right on real packed executables, before a single
byte of x64 is emitted.

## Proving it correct

`cli/main.cpp` is the contract, and a JIT is exactly the kind of change that
passes a test suite and still breaks a game. Four mechanisms, in increasing
order of how much they will find:

1. **The whole suite, twice.** `--test` runs every existing test under the
   interpreter and again under the JIT. Cheap, and it pins the parts of the
   contract that already exist.
2. **`--jit-verify`: lockstep differential execution.** Run each block on the
   JIT, then re-run the same instructions on the interpreter from a copy of the
   entry state, and compare the full `cpu_state` and every memory byte written.
   Stop at the first divergence and print the disassembly of the block. This is
   the mechanism that turns "Battle Chess draws garbage" into "opcode D3 /4
   sets AF wrong", and it should exist before Tier 1 does.
3. **An instruction fuzzer.** Generate a random encoding from the supported
   opcode set, random register file, random flags and a scratch memory window;
   execute one instruction under both engines; diff registers, all six
   arithmetic flags and the memory window. Seeded and reproducible, run for a
   fixed count in `--test` and unbounded in a soak. Flags are where JITs go
   wrong, and this is the only way to cover them properly.
4. **The sample programs, compared.** Run each of Elite, Wasteland, Ultima IV
   and Battle Chess for a fixed number of cycles under both engines and compare
   a hash of the framebuffer and of the whole of RAM. Any divergence at all is
   a bug, and this catches the ones that only appear after millions of
   instructions.

`--trace` and `--trace-tail` force the interpreter. Debugging a program is a
different job from running one fast, and the JIT should never be in the way of
it.

## Order of work

| | | |
|---|---|---|
| 0 | `--bench` and the measurements above | **done** - and they say the JIT is not next |
| - | 386 instructions, protected mode, DPMI, then `--bench` again | the actual blocker |
| 1 | `host_services` code memory, `jit.h`, arena, block cache, invalidation bitmap and its policy, dispatcher, Tier 0 | suite green, speed unchanged |
| 2 | `--jit-verify` and the fuzzer | divergence found in one block, not one game |
| 3 | Tier 1 codegen, opcode by opcode | 2-3x |
| 4 | Tier 2: inline memory, chunked and block-copy string ops | the big one for mode 13h |
| 5 | Raise `cpu_hz`, re-run `--bench`, decide whether Tier 3 is needed | 60 fps at a 486's throughput |

Each row is a working build with a green suite. If row 5's number is reached at
row 4, stop.

## What this does not do

- It does not add 386 instructions, protected mode or a DOS extender. Those are
  a separate and larger project, and doing them on top of a working JIT is far
  easier than doing both at once. Nothing here should assume 16 bits forever:
  keep the register file, the flag handling and the address computation in
  terms of an operand size rather than hard-coding `uint16_t` where it costs
  nothing to avoid.
- It does not change what a program can observe. Interrupt latency moves from
  one instruction to one block and cycle costs are charged per block; neither
  is visible to a program that is not measuring the emulator itself. Everything
  else - the callback page, the MCB chain, mid-`REP` interrupts, the retrace
  status - behaves exactly as it does now, and the tests are what say so.
