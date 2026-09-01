# Sura stabilization backlog

Last reviewed: 2026-07-25
Applies to: Sura 1.11.1

This file records verified engineering debt. It is not a feature wish list and
does not override `compatibility.json`, which is the machine-readable support
contract.

## What already exists

The following are implemented today and must not be described as missing:

- register bytecode compiler, versioned bytecode reader/writer, and register VM;
- native JIT subsets for the platforms listed in `compatibility.json`, with VM
  fallback outside the supported opcode subset;
- strict-by-default type checking;
- source imports/modules;
- functions, closures, classes, structs, enums, exceptions, and core collection
  values;
- `surapkg`, compatibility fixtures, C FFI, and a versioned plugin ABI;
- experimental JavaScript and WebAssembly translation targets.

Claims about completeness still need to respect the stable,
platform-limited, and experimental tiers in `compatibility.json`.

## P0: release provenance and clean reproduction

At the start of the 2026-07-25 review, the working tree contained 198 entries:
52 modified tracked paths and 146 untracked paths. Most of the volume was
freestanding OS/browser work. A binary built from that tree cannot be presented
as a clean release artifact.

A portable build of the current working tree succeeded, but it did not match the
checked-in local executable:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| current-source isolated build | 8,945,960 | `6cfd47406ee38b8c81bf4f04f5c774df82eabdac855c94d6d80a2ca33a8c8f60` |
| pre-existing `SuraLanguage.exe` | 8,945,418 | `bcf5713008b4578a5de4d28f0033303cbd037d1121af3f7b0f2cf7683dbefdc9` |

This is evidence that the old executable must not be used as proof for the
current source. It is not evidence of a compiler defect.

### Update 2026-08-15: stale binary replaced, and why hashes did not reproduce

`build.bat portable` was re-run, so the checked-in executables are now built
from the current working tree rather than the July artifact:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `SuraLanguage.exe` (current) | 8,925,149 | `3cb621f13dea8ae5abbf70077bba184ee24b57db96ac87c88f9c79caf1e83e5e` |
| `SuraLanguage.exe` (previous, stale July artifact) | 8,945,418 | `bcf5713008b4578a5de4d28f0033303cbd037d1121af3f7b0f2cf7683dbefdc9` |

Rebuilding the same source a second time then produced a **different SHA-256 at
an identical byte count**. `cmp -l` reduced the difference to exactly **4 bytes**,
and `objdump -p` identified them as the PE header `Time/Date` stamp. The
generated code was bit-identical; only the embedded build clock differed.

That means exit criterion 4 ("record SHA-256 for every artifact") was
unachievable as written: any third party rebuilding the tree would compute a
different hash no matter how clean their checkout was. `build.bat` and
`Makefile` now link with `-Wl,--no-insert-timestamp`, which pins that field to
the epoch and makes the recorded hash something a rebuild can actually confirm.

Verified after the change: `build.bat portable` was run twice, back to back,
against an unchanged tree. Both runs produced byte-identical engines. (The
hash below is the source state at the time of that experiment, not the current
artifact above; what it establishes is that repeat builds agree.)

```text
6eb197f314cd1f16251c864f4ec7179bd4f826ebe6865d7c15f59c15e034481e  build 1
6eb197f314cd1f16251c864f4ec7179bd4f826ebe6865d7c15f59c15e034481e  build 2
```

A third build after a comments-only edit reproduced the same hash again,
confirming the timestamp was the only nondeterministic input.

The engine build is therefore deterministic on this toolchain (g++ 15.2.0,
`-std=c++17 -O3 -DNDEBUG -Wall -static`), and a recorded SHA-256 is now a claim
a reviewer can check rather than one they have to take on trust.

Still open for a clean release: 153 untracked source files (`stdlib/freestanding`,
`examples`, `tools`, `os`) remain outside version control, so a fresh clone still
does not contain the sources these binaries were built from. That, not the
compiler, is the remaining blocker to criterion 2.

### Update 2026-08-31: working tree fully committed, clean clone verified

All 251 outstanding entries (98 modified, 153 untracked) were committed in ten
grouped, reviewable commits on `agent/sura-os-freestanding`: gitignore for
freestanding artifacts, lexer `\r` escape, mechanical exit-code gates, skip
accounting runners, reference/JS-target updates, release tooling, docs,
freestanding stdlib/OS work, QEMU gates, and third-party vendoring
(doomgeneric flattened from an accidental gitlink to real tracked files, with
provenance recorded in `third_party/doomgeneric/SURA_VENDOR.md`).

Verified from a fresh `git clone` of that commit:

- `build.bat portable` succeeded and produced `SuraLanguage.exe` with SHA-256
  `3cb621f1...`, byte-identical to the hash recorded in `bench_summary.md`
  on 2026-08-15 — the recorded provenance is now reproducible by a third party.
- `run_stable_tests.ps1` (JIT lane): 151 passed, 0 skipped, 0 failed.

Criterion 2 is met on this branch. Remaining for release: run the same
verification in CI (criterion 3) and publish the provenance manifest
(criteria 4–6).

## P1: WASM export ABI classification is context/order dependent

Verified 2026-09-01 while unblocking the cross-platform CI lane. The WASM
target has two return-kind systems: static kind inference (raw i32/string
handles) and value-preserving classification (tagged Value ABI). The
value-preserving verdict for a named function depends on which context asks
first (local lift hints are visible inside the function but not to callers,
and the result is cached), so identical source shapes can classify
differently — observed with `captured_inline_function_if_merge_ast` (cached
raw) versus `control_flow_function_alias_ast` (cached tagged).

The function-boundary unwrap added to `tools/sura_to_wasm.ps1` restores the
raw contract for direct promoted-lift and known-kind indirect returns, which
makes `sura_wasm_target_smoke` and `sura_wasm_exception_smoke` pass end to
end. Still failing, and pre-existing (verified against the unpatched
transpiler): `sura_wasm_function_dispatch_smoke` and
`sura_wasm_memory_safety_smoke` — their fixtures export functions the
classifier marks tagged while the JS harness expects raw numbers. The real
fix is to make return-ABI classification deterministic (context-free, no
query-order cache effects) and document the host-facing export contract;
test-side unwrapping cannot be written safely while the ABI can flip.

Release exit criteria:

1. Separate the OS/browser work into reviewable commits or a dedicated branch.
2. Start from a clean checkout of the intended release commit.
3. Build portable VM/package artifacts in CI.
4. Record commit, compiler version, flags, dependency versions, byte count, and
   SHA-256 for every artifact.
5. Run verification against those exact immutable artifacts.
6. Publish the provenance manifest with the release.

## P0: honest test outcomes

Runtime capability skips are not passes. Both PowerShell runners now recognize
an anchored output line such as:

```text
27_cuda_residency: SKIP (CUDA device unavailable)
```

`tools/sura_test.ps1` records `pass`, `skip`, and `fail` separately in JSON and
JUnit, including the skip reason. `run_stable_tests.ps1` prints the same three
counts. Use `-FailOnSkip` for a hardware certification lane:

```powershell
pwsh -File tools/sura_test.ps1 -Path tests -Engine .\SuraLanguage.exe -FailOnSkip
pwsh -File run_stable_tests.ps1 -Engine .\SuraLanguage.exe -FailOnSkip
```

A normal cross-platform lane may allow skips, but its result must be described
as "suite passed with N skips," never "all hardware paths verified."

### Recorded NVIDIA lane run (2026-08-15)

A `-FailOnSkip` lane was executed on a machine with a CUDA device present, so
the 30 `tests/*cuda*.sura` cases ran on hardware rather than skipping:

| Field | Value |
| --- | --- |
| Result | 151 passed, 0 skipped, 0 failed |
| Engine SHA-256 | `3cb621f13dea8ae5abbf70077bba184ee24b57db96ac87c88f9c79caf1e83e5e` |
| Engine bytes | 8,925,149 |
| GPU | NVIDIA GeForce RTX 4060 (8188 MiB) |
| Driver | 610.88 |
| Compiler | g++ (MSYS2) 15.2.0, `-std=c++17 -O3 -DNDEBUG -Wall -static` |
| Runner | `run_stable_tests.ps1 -FailOnSkip` |

The runner snapshots the engine and prints the snapshot hash, so this result is
bound to that exact artifact. This satisfies "skips are not passes" for this
run; it does not by itself satisfy the CI requirements below, because it was a
developer-machine run rather than a dedicated runner, and cuBLAS/CUDA toolkit
versions were not captured (`nvcc` is not on PATH on this host).

### Measured JIT coverage gap (2026-08-15)

`tools/sura_jit_differential.ps1` runs each program twice - once with `--jit`
and once on the register VM - and requires byte-identical output, using the VM
as the reference. Against `tests/` (141 programs):

| Outcome | Count |
| --- | ---: |
| Verified (native code ran, output matched the VM) | 106 |
| **DIVERGED (native output differs from the VM)** | **0** |
| No native compilation - proves nothing about codegen | 34 |
| Nondeterministic, excluded as an oracle | 1 |

Program counts are a weak proxy, though. The engine now reports a `[JIT-OPS]`
line naming the opcodes each run actually emitted, and the differential script
unions them, which turns the question into "which emitters are checked":

**All 49 opcodes reached by `emit_op` are exercised.** Six of them -
`INDEX_GET`, `INDEX_SET`, `NEW_INSTANCE`, `OP_IN`, `FOREACH_NEXT` and `DICT_KEYS` - were
added as new cases during this work, along with `DIV` and `USE_LIB` being
converted from unconditional bails; the rest were already there. The one
remaining name with a case but no coverage is `DEF_FUNC`,
whose case returns true without emitting anything: function definitions compile
to `MAKE_LAMBDA`, so the opcode does not reach the emitter in practice. There is
no machine code behind it to get wrong.

This started at 33 and moved for two different reasons, worth separating.
Genuinely new coverage came from `PRINT`/`PRINT_NO_NL`, which needed nothing
more than a hot callee that prints - every hot callee in the suite returned a
value instead (`76_jit_print_differential.sura`) - and from the bitwise family
in `74_jit_bitwise_differential.sura`. But `HALT`, `DEF_CLASS` and `STORE_GLOBAL`
were never actually missing: they are emitted by the top-level chunk, whose mask
the instrumentation was silently dropping (see below). Fixing that one blind
spot also took Verified from 25 to 50, because programs where only `main`
compiled had been filed under "no native compilation" the whole time.

#### `USE_LIB` was the second `DIV`, and is now fixed

`main` is compiled as a single function with no warm-up threshold, but on an
all-or-nothing basis: one rejected opcode disqualifies the entire top level.
`USE_LIB` returned false unconditionally, so **a single `use` statement anywhere
dropped the whole main body back to the interpreter** - the same shape of
problem `DIV` had inside functions, where one division disqualified its callee.
Since most real programs open with a `use`, this reached further than `DIV` did.

It was measured before being fixed, which is how the size of it became clear: a
three-line top level compiled and emitted
`LOAD_CONST LOAD_GLOBAL STORE_GLOBAL ADD PRINT HALT`, and adding `use math` and
nothing else took it to `(none)`.

`USE_LIB` now goes through `sura_jit_use_lib`, following the `DEF_CLASS` helper
shape. The care this one needed that `DIV` did not: `USE_LIB` is not pure - it
binds a stdlib module into a global slot - so `JitVM::jit_use_lib` mirrors the
interpreter's `USE_LIB` case exactly, including the order the module value is
built and bound. Otherwise a program's observable state would depend on whether
its top level happened to compile.

Measured with `tools/sura_ab_bench.ps1 -Jit` on `bench_use_toplevel.sura`, ten
interleaved rounds: **15.26 ms -> 2.63 ms, 10/10 rounds, 5.8x**. The top level
went from compiling 0 opcodes to 14.
`tests/78_jit_use_lib_differential.sura` uses the module after importing it, so
a binding that is performed differently - or skipped - fails an assertion rather
than passing silently.

#### Ranked worklist: what is keeping programs out of the JIT

`DIV` and `USE_LIB` were both found the same way - notice a program compiling
nothing, bisect until the responsible opcode falls out. That is now automatic.
The engine reports the opcode it gave up on:

```text
[JIT-BAIL] top level not compiled: NEW_INSTANCE at ip=22
```

and `sura_jit_differential.ps1` tallies them across a run. Because the top level
compiles all-or-nothing, each count below is **whole programs** that got no
native code, not instruction counts. Against `tests/` (136 programs):

The first run of this tally ranked `INDEX_GET` at **53** programs - more than
`DIV` and `USE_LIB` together - so it was given an emitter (below). The tally
after that change:

| Opcode | Now | After `INDEX_GET` | Originally |
| --- | ---: | ---: | ---: |
| `TRY_BEGIN` | 34 | 31 | 14 |
| `LOAD_UPVAL` | 3 | 3 | 3 |

Everything else on the original list has been given an emitter. The two that
remain are both structural rather than unwritten, and each has its own section
below: `TRY_BEGIN` needs a way to re-enter generated code at a bytecode offset,
and `LOAD_UPVAL` needs the closure the native calling convention does not pass.

None of these have a case in `emit_op` at all - they reach `default: return
false`.

**The "before" column is the point.** Fixing `INDEX_GET` did not convert 53
programs; it converted 35, and the rest moved down the list to whatever blocked
them next. `TRY_BEGIN` more than doubled without anyone touching it. That is the
caveat this table has to carry: a program reports only the *first* opcode that
stopped it, so these counts are a ranked worklist, not a forecast. Re-run the
tally after each emitter rather than projecting from it.

The other caveat is unchanged: this ranks *how many programs* an opcode blocks,
not how much time those programs spend in the blocked region. A program
disqualified by an opcode it executes once still appears here.

#### `OP_IN` and `FOREACH_NEXT`

`OP_IN` was ordinary: a value computation over array / dict / string, nil for
anything else, no control flow.

`FOREACH_NEXT` is worth recording because it looked like `TRY_BEGIN` and was
not. It also ends something by assigning `lip`, but that jump is **local to the
same body**, which the emitter's existing pending/patch machinery already
expresses - unlike a catch target, there is nothing to re-enter from outside.
So it splits cleanly: `JitVM::jit_foreach_next` advances the iterator and
returns 1 to continue or 0 to exit, and the generated code branches on that in
the same shape `JUMP_IF_FALSE` uses.

The distinction that matters across all of this: a jump *within* a compiled body
is fine, a jump *into* one from the interpreter is not.
`tests/82_jit_in_foreach_differential.sura` drives loops to their natural end -
including an empty collection, where the exit branch is taken without ever
producing a value, and nested loops, where an inner iterator must not disturb
the outer one.

#### Why `LOAD_UPVAL` is blocked on the native calling convention

`LOAD_UPVAL` reads through the *current frame's* closure:

```cpp
GCUpvalue* uv = fp->closure->upvalues[inst.operand];
R[a] = uv->location ? *(uv->location) : uv->closed;
```

A helper cannot do that, because generated code has no `fp`. The native entry
point is `fn(JitVM* vm, Value* R, const Value* consts)` - the closure a compiled
body was invoked through is simply not passed in, so there is nothing for the
helper to read upvalues from. `STORE_UPVAL` has the same shape and the same
gap.

This is a calling-convention limitation rather than a missing emitter, and
widening the ABI to carry the closure touches every call site and every
prologue. At 3 programs it is not worth that; it is recorded here so the next
person does not start by writing the helper and then discover there is no way
to reach the data.

#### Why `TRY_BEGIN` is not the next easy win

`TRY_BEGIN` now leads the list at 31 programs, and the opcode itself looks
trivial - the interpreter case sets three fields and falls through:

```cpp
fp->in_try        = true;
fp->catch_ip      = (size_t)inst.operand;
fp->catch_var_reg = a;
```

Emitting that as a helper call would take about ten minutes and **would be
wrong**. The state is trivial; what consumes it is not. Catching is entirely
interpreter-internal: `OP_THROW` walks `frame_pool` for the nearest frame with
`in_try`, pops the frames above it, restores `R`, and then resumes the dispatch
loop by assigning `lip = fp->catch_ip`.

Native code has no `lip` to assign. Three things would have to exist first:

1. **A runtime bytecode-ip to native-offset map.** `ip_to_native` exists, but
   only as a `NativeCompiler` member used to resolve jumps during compilation;
   `NativeFunc` does not retain it.
2. **A way to re-enter generated code at an arbitrary offset** with the register
   window in the right state.
3. **Landing pads in the generated code**, because a runtime error arrives as a
   C++ `JitThrow` propagating out of the native frame - it does not pass through
   `OP_THROW`'s frame walk at all.

Point 3 is the one that makes a naive implementation actively harmful rather
than merely incomplete. Set `in_try` from native code today and a `try` block
around, say, a division by zero would not catch: the helper throws, the C++
exception unwinds straight out of the compiled body, and the interpreter's
handler search never runs. The program would report an uncaught error where the
interpreter caught one - a behavioural difference, not a slowdown.

**The bail is a correctness guard, not an oversight.** It should stay until the
three items above are built.

`tests/84_jit_try_catch_semantics.sura` turns that decision into something
executable rather than a paragraph someone has to read first. It warms several
callables past the JIT threshold and then requires that errors raised *from
inside generated code* are still caught: division by zero and a type error out
of `sura_jit_checked_div`, an out-of-range index out of `sura_jit_index_get`,
an explicit `throw` crossing a function boundary, and control resuming after the
`try` rather than skipping the rest of the body. Emit `TRY_BEGIN` without the
re-entry machinery and those stop passing - which is the point, because the
failure being guarded against is silent.

`INDEX_SET` was taken next for exactly that reason, and it was the easy kind:
two branches, no result, no raising, and anything that is neither array nor dict
is silently ignored. It went through `sura_jit_index_set` in the `DOT_SET`
mould. The trap worth naming is that its register roles differ from
`INDEX_GET` - `a` is the container being written to rather than a destination -
so an emitter that copies `INDEX_GET`'s shape writes to the wrong place and
produces a silent wrong answer.
`tests/80_jit_index_set_differential.sura` pins both branches, the ignored
non-container case, and a read-modify-write loop that exercises `INDEX_GET` and
`INDEX_SET` against the same container.

`NEW_INSTANCE` looked at first like a second `TRY_BEGIN`: its interpreter case
pushes a frame and keeps interpreting inside the constructor. The distinction
that made it tractable is that its control transfer is a **call that returns**,
not a non-local jump - so the helper drives the constructor to completion with
`execute_frame()` and comes back, exactly as `dispatch_method_call_from_jit`
already does for ordinary method calls. Nothing has to be reconstructed.

Two interpreter orderings are load-bearing and `JitVM::jit_new_instance`
preserves both: the instance is written to its destination register *before* the
constructor runs (which is what makes the constructor's return value discarded
rather than overwriting it), and the constructor's frame registers are released
afterwards, so repeated construction in a loop does not grow the value stack.
`tests/81_jit_new_instance_differential.sura` pins the no-constructor,
returning-constructor and inherited-constructor cases, plus a 300-iteration
construction loop that would exhaust the stack rather than merely slow down if
the release were missed.

#### A blind spot in the coverage measurement itself

The top-level chunk is compiled into a local `NativeFunc` that is never stored
in `native_funcs`. Both the `[JIT] N function(s), M method(s)` tally and the
first version of the opcode mask therefore skipped it entirely, which is why
`HALT` looked unreachable and why a program where only `main` compiled was
being filed under "no native compilation". `JitVM::main_emitted_ops` now
captures it, and `sura_jit_differential.ps1` decides "did native code run" from
the emitted-opcode count rather than the function tally.

`DIV` used to be on that list, and measuring it is what showed the cost. It had
a case in `emit_op` that bailed unconditionally, so a single division anywhere
in a function disqualified the *whole function* from native compilation - every
other operation in it fell back to the register VM too. It now goes through a
guarded `sura_jit_checked_div` helper, the same shape `MOD` already used,
because dividing by zero has to raise `[E202]` rather than yield an infinity.

Measured with `tools/sura_ab_bench.ps1 -Jit` on `bench_division.sura`, ten
interleaved rounds: **21.01 ms -> 3.50 ms, 10/10 rounds, 6.0x**. The hot callee
went from compiling 0 opcodes to 8. `tests/72_jit_arithmetic_differential.sura`
pins both failure modes - division by zero and a non-numeric operand - because
those now raise from inside natively generated code and unwind through it.

Three tests were added to drive this deliberately, and they assert exact values
rather than only that the program ran:

- `72_jit_arithmetic_differential.sura` - arithmetic, all six comparisons,
  backward-branching loops, and `or`/`and` short-circuits (the only way to
  reach `JUMP_IF_TRUE`; plain `if/else` emits `JUMP_IF_FALSE` alone).
- `73_jit_object_differential.sura` - field reads and writes, instance
  creation, monomorphic method calls, closures.
- `74_jit_bitwise_differential.sura` - the bitwise family plus `LOGICAL_NOT`
  and `LOAD_NIL`. `BIT_NOT`, `BIT_XOR`, `LOGICAL_NOT` and `LOAD_NIL` had
  emitters that nothing in the suite reached, while their siblings
  `BIT_AND`/`BIT_OR`/`LSHIFT`/`RSHIFT` were covered incidentally - which is
  exactly the kind of gap a program count cannot show.

Two separate conclusions, and they should not be collapsed:

1. No codegen defect was found. Everything the JIT currently compiles produces
   exactly what the interpreter produces.
2. **106 of 141 programs now exercise a native emitter**, up from 22 when this
   measurement was first taken. The suite result "151 passed" is still quiet
   about JIT correctness for the remaining 34, so a green suite is not by itself
   evidence that `jit_native.hpp` is right. The emitter count above is the
   better measure.

This is the reason opcode coverage expansion is gated behind the differential
lane rather than started directly: widening the emitters without also raising
the Verified count means shipping machine code that nothing compares against.
Track both numbers together - Verified should rise as coverage grows, and
"No native compilation" should fall.

### Intermittent: `tests/async_sura_process_smoke.sura`

Across nine full suite runs on 2026-08-15 this test failed once and passed
eight times, with:

```text
[Runtime Error] async_sura(): path must name a regular file (line 6, col 1)
```

Run on its own immediately afterwards, it passed. So the suite result should be
read as "141 passed, with one test observed intermittent", not as a clean 141.

The message is also misleading, and that is a fixable defect rather than just
bad luck. `async_sura_regular_absolute_path` in `stdlib.hpp` does:

```cpp
std::error_code status_error;
if (!std::filesystem::is_regular_file(absolute, status_error)) {
    throw JitThrow{std::string(fn) + "(): path must name a regular file", line};
}
```

`status_error` is passed in and then never inspected. When the stat itself
fails transiently - a sharing violation, an antivirus or indexer holding the
file, a cloud-sync recall on a OneDrive-backed checkout - `is_regular_file`
returns false and the caller is told the path is not a regular file, which is
a different claim from "the filesystem would not tell me right now".

Fixed. All three sites that had this shape - the `file URL`, `program` and
`path` checks - now go through one helper, `async_require_regular_file`, which
separates "this is not a regular file" from "the filesystem would not answer":

```text
async_sura(): path must name a regular file          # absent, or a directory
async_sura(): cannot inspect path (Permission denied) # the query itself failed
```

One subtlety is worth recording, because the obvious implementation is wrong
here. libstdc++ on Windows reports a *missing* path through the `error_code`
rather than as a `not_found` status, so a plain "any error code means the query
failed" check sends the single most common case - the file simply is not there
- into the message meant for lock and permission failures. The helper therefore
treats `no_such_file_or_directory` and `not_a_directory` as definitive answers
and keeps the plain wording for them. `tests/75_async_path_error_messages.sura`
pins all three outcomes so this cannot regress.

Still open:

- The intermittent failure itself is not proven fixed. Better diagnostics mean
  the next occurrence will name the real cause instead of misdirecting; that is
  not the same as the flake being gone. Do not treat a single green suite run
  as proof this test is stable.
- `async_read_program_snapshot` has a weaker form of the same conflation: a
  failed `file_size` is reported as "program exceeds 64 MiB snapshot limit"
  (`stdlib.hpp`, the `size_error || size > ASYNC_SURA_MAX_PROGRAM_BYTES` check).
- Consider whether spawning helpers should resolve relative to the calling
  script rather than the process working directory. The test uses
  `path_join("tests", ...)`, so it only works when run from the repo root.

Remaining test work:

- promote this into a dedicated NVIDIA runner that uses `-FailOnSkip` in CI;
- publish CUDA/cuBLAS toolkit versions alongside the GPU and driver above;
- keep sanitizer, soak, target-translation, and OS/QEMU lanes separate from the
  fast language regression lane;
- define required versus optional lanes for a release in one machine-readable
  policy file.

## P0: security boundary

Sura is not an operating-system sandbox. Untrusted code can reach file, process,
network, FFI, and plugin surfaces with the host process's permissions. Native
plugins and FFI run inside the runtime process.

There is no independent external security audit in this repository as of the
review date. Existing sanitizer, malformed-input, compatibility, soak, and audit
bundle checks are regression evidence, not a security certification.

Before recommending untrusted-code execution:

1. define a minimal capability model and deny-by-default host surface;
2. isolate native code and untrusted workloads at the process/OS boundary;
3. threat-model bytecode, package, FFI, plugin, file, process, and network input;
4. commission an independent review and publish its scope and version;
5. add a vulnerability response process with tested private reporting.

See `SECURITY.md` and `SECURITY_AUDIT.md` for the current boundary.

## P1: core maintainability

Current concentration is high:

| File | Lines on 2026-07-25 | Primary concern |
| --- | ---: | --- |
| `stdlib.hpp` | 16,314 | host APIs, modules, async, AI, and registration share one review unit |
| `main.cpp` | 5,770 | CLI, compilation, execution, target handling, and diagnostics are coupled |

Do not perform a single mechanical mega-split. Preserve behaviour with small,
independently buildable moves in this order:

1. extract CLI argument parsing and command dispatch from `main.cpp`;
2. extract engine/session construction and file execution;
3. split standard-library registration from implementations;
4. group host-capability modules (`fs`, process, network, FFI/plugin) behind
   explicit interfaces;
5. split async/concurrency and AI/media/CUDA adapters into separate translation
   units;
6. add dependency-direction checks so modules cannot silently re-couple.

Each move must keep the stable VM suite, JIT suite, sanitizer lanes, and public
API snapshot green. Line-count reduction alone is not an acceptance criterion.

### Update 2026-08-15: where the build time actually goes

The maintainability concern above is usually felt as build time, so it was
measured rather than assumed (g++ 15.2.0, this tree):

| Target | Time |
| --- | ---: |
| `main.cpp` @ `-O3` | 89s |
| `main.cpp` @ `-O1` | 53s |
| `main.cpp` @ `-O0` | 40s (**fails to link**) |
| `surapkg.cpp` | 35s |
| `gc.cpp` + `platform.cpp` | 2s |
| `build.bat portable` end to end | ~145s |

Two things follow. The engine is effectively **one dominant translation unit**:
`gc.cpp` and `platform.cpp` together are 2 seconds, so nothing is gained by
parallelising them. And the optimiser is only about half the cost - dropping
from `-O3` to `-O1` saves 36s, but 53s remains, which is the parse and template
instantiation cost of `stdlib.hpp` and its neighbours. **No compiler flag gets
below roughly 50s; splitting the header into separately compilable units is the
only route past it.** That is the concrete argument for the split, and it is
worth about 50s per rebuild rather than the "builds are slow" impression.

`build.bat dev` was added for correctness-only rebuilds: `-O1`, engine only,
55s. `-O0` is not used despite being faster to compile, because this toolchain
then fails to link with undefined `__emutls_t` references for the
`thread_local` statics in `SuraStd`'s inline accessors - a detail worth
recording, since the obvious "just use -O0" produces a build that compiles and
then dies at the link step.

## P1: JavaScript target test cost

`test_js_target.sura` is a large cross-target fixture, not a fast native stable
test. The default JS smoke now performs full source translation, focused AST
translation, structural checks, and representative runtime parity, while
expensive whole-fixture execution and full-fixture AST translation are opt-in:

```powershell
# Fast CI smoke; target is under the default 20-second budget.
pwsh -File tools/sura_js_target_smoke.ps1

# Full generated runtime execution.
pwsh -File tools/sura_js_target_smoke.ps1 -ExtendedRuntime

# Full runtime plus local HTTP/static/route integration.
pwsh -File tools/sura_js_target_smoke.ps1 -NetworkIntegration
```

The large JS target fixture is no longer executed as a native language test by
`run_stable_tests.ps1`; the dedicated target smoke owns that coverage. Continue
splitting the fixture by language feature so failures identify a smaller area
and the extended lane can be parallelized safely.

## P1: scope control

The stable product is the language, VM, core APIs, package workflow, FFI, and
plugin ABI listed in `compatibility.json`. JIT/CUDA/media/distribution remain
platform-limited. JavaScript/WASM, Transformer/distributed training, bounded BPE
and ONNX, protected-release policy, registry operations, and the freestanding
OS/browser work must not silently expand the stable contract.

New experimental work should not block core stabilization. Promotion requires:

- a named owner and support platforms;
- positive, negative, fallback, timeout, and dependency-missing tests;
- public limitations and a compatibility impact statement;
- release CI evidence on the claimed platform;
- a maintenance and deprecation plan.

## P1: benchmark evidence

Repository benchmark reports are useful for regression tracking, but historical
self-measurements are not independent performance validation. Comparisons must
publish:

- exact source commit and binary hashes;
- compiler/interpreter/framework versions and flags;
- hardware, OS, power mode, driver, and background-load evidence;
- warmup, sample count, raw samples, statistic, and outlier policy;
- identical workload semantics and included/excluded setup costs;
- an independently runnable command or container.

Do not market aggregate "faster than Python" or best-case ratios as general
language performance. Treat existing numbers as repository-owned historical
results until independently reproduced.

### Update 2026-08-15: regenerated, and the mean was the problem

The report was regenerated because the previous one could not be checked at all:
it named `SuraFinal.exe`, a binary that no longer exists, with no hash. Every
figure in it was therefore unverifiable regardless of how it had been measured.
`bench_summary.md` now records the engine SHA-256 and byte count next to the
path, so a reader can confirm which artifact produced the numbers.

The regenerated figures also made a reporting defect obvious. The summary led
with means, and one benchmark dominates them:

| Statistic | Median | Mean |
| --- | ---: | ---: |
| JIT speedup | **2.83x** | 27.64x |
| Sura / Python | **4.67x** | 15.27x |

The mean JIT speedup is roughly ten times the median. It is not describing the
language; it is describing `bench_physics3d`, which hits a guarded
benchmark-specific fast path and lands at 414x while most of the suite sits
between 2x and 5x. The same applies to the Python column, where one 152x case
triples the mean.

`tools/sura_bench_dashboard.ps1` now computes and reports the median alongside
the mean in the summary table, the HTML dashboard and the JSON report. **Quote
the median when describing typical performance.** The mean is retained because
it is what previous reports used and dropping it would silently change the
trend series, not because it is the better statistic here.

## P2: ecosystem maturity

After P0/P1 stabilization:

- ~~publish a small, versioned standard-library reference generated from the
  stable API snapshot~~ — done 2026-08-15, see below;
- provide migration notes and deprecation tooling for every minor release;
- create a package conformance suite and signed registry metadata policy;
- ~~publish editor/LSP compatibility tests~~ — done 2026-08-15, see below;
- ~~add deterministic bug-report bundles containing versions, hashes, and
  minimal reproductions without secrets~~ — done 2026-08-15, see below.

### Update 2026-08-15: standard-library reference

`STDLIB_REFERENCE.md` is generated by `tools/sura_stdlib_reference.ps1` from
`tests/compat/<series>/stable-api.json`, with the snapshot path read out of
`compatibility.json` so the tool follows the contract instead of a hardcoded
series. 112 symbols across `array`, `dict`, `fs`, `json`, `string` and `test`,
each with its signature, plus the source path and SHA-256 so a reader can tell
whether the document is current.

`reference.html` was not the gap it looked like: it is a language guide - syntax,
install, compatibility tiers - and never listed the library functions at all.

The scope decision is the part worth keeping in mind. The engine's builtin table
holds roughly 664 names; the stable contract covers 112 of them. Documenting all
664 would imply a stability promise that does not exist, and documenting 112
without saying so would read as "this is the whole library". The generated
document therefore states explicitly that absence from it means *uncontracted*,
not *missing*. Closing that gap is a matter of promoting more of the surface
into the contract, which is a decision about what to commit to rather than a
documentation task.

### Update 2026-08-15: LSP compatibility test

`tools/sura_lsp_smoke.ps1` drives `SuraLanguage.exe --lsp` the way the VS Code
extension does - `initialize`, `initialized`, `didOpen`, then each request
method, then `shutdown`/`exit` - over real stdio with Content-Length framing.

Nothing else covered this. The `.sura` suite only ever runs programs, so the
entire editor path (framing, JSON-RPC envelopes, handshake order) had no test
despite `sura-vscode` launching the server through `vscode-languageclient` on
every session.

Current result: **12/12 requests answered, 12 capability fields advertised,
13 well-formed frames.** `hover`, `completion`, `definition`, `references`,
`signatureHelp`, `documentSymbol`, `formatting`, `codeAction`, `rename` and
`workspace/symbol` all respond.

Two deliberate choices in how it judges:

- It checks framing and JSON-RPC envelopes, not exact hover wording. The
  question is "can an editor talk to this", and pinning prose would make the
  test fight every improvement to the messages.
- A JSON-RPC `error` reply counts as a failure, not a pass. The server lists
  these methods in its own `initialize` capabilities, so erroring on one is a
  broken promise to the client rather than an acceptable "unsupported".

### Update 2026-08-15: the emitter benchmarks are now tracked

`bench_division.sura`, `bench_use_toplevel.sura` and `bench_index_get.sura`
were added alongside the `DIV`, `USE_LIB` and `INDEX_GET` emitters but were not
in the dashboard's benchmark list, so the 6.0x, 5.8x and 3.2x results had no
regression protection at all. Reverting an emitter would have cost a factor of
several with nothing to notice.

They measure something different from the rest of the suite. The others time a
workload; these three check that **one opcode still has an emitter**, because
each of them was blocking its entire enclosing body from native compilation. A
loss shows up as a 3-6x cliff rather than a few percent drift, which is exactly
the shape a regression gate can catch reliably.

Adding them tripped `sura_bench_dashboard_smoke.ps1`, which pins the benchmark
count at an exact number - correctly, since that assertion is what stops
benchmarks from silently disappearing. The pin is now 23.

### Update 2026-08-15: a gate that printed PASS and exited 2

`tools/sura_build_contract_smoke.ps1` ended with `Write-Host "...: PASS"` and no
`exit 0`, so it inherited whatever the last command returned. Its final check is
deliberately `build.bat <invalid mode>`, which exits 2 by design - so the gate
reported **PASS while exiting 2**. Anything reading the exit code, which is what
CI does, saw a failing gate whose output said it passed.

`sura_compatibility_gate.ps1` and `sura_bench_dashboard_smoke.ps1` had the same
missing `exit 0` and happened to end on a zero-returning command, so they worked
by luck. All three now state their exit code.

Worth generalising: **a gate that prints PASS must exit 0 explicitly.** Relying
on the last command's code makes the reported result and the machine-readable
result two different things, and only one of them is what CI acts on.

`tools/sura_gate_exit_audit.ps1` measures how widespread this is. At the start
of this work, of 169 gate and smoke scripts, **34 stated their exit code and 135
inherited it**. Most of the 135 are fine today only because their last command
happens to return zero, which is not a property anyone chose.

They were not mass-edited. A script that currently ends on a zero-returning
command is working, and changing 135 files without running them would trade a
latent problem for an unverified one.

Instead the backlog is being retired in verified batches: run each gate to
confirm it passes, add the `exit 0`, run it again to confirm it still passes and
now exits 0. Eleven batches are done. **The count is now 105 explicit, 64
inherited**, down from 34/135 - the majority of the suite now states its own
verdict.

Running them first is not optional, and the batches proved it. **Eighteen gates
were already printing PASS while exiting nonzero:**

| Gate | Was |
| --- | --- |
| `sura_build_contract_smoke.ps1` | PASS, exit 2 |
| `sura_pkg_version_smoke.ps1` | PASS, exit 1 |
| `sura_pkg_tree_smoke.ps1` | PASS, exit 1 |
| `sura_policy_smoke.ps1` | PASS, exit 1 |
| `sura_tool_policy_audit_smoke.ps1` | PASS, exit 1 |
| `sura_plugin_manifest_audit_smoke.ps1` | PASS, exit 1 |
| `sura_public_signature_smoke.ps1` | PASS, exit 1 |
| `sura_target_lowering_audit_smoke.ps1` | PASS, exit 1 |
| `sura_registry_verify_smoke.ps1` | PASS, exit 1 |
| `sura_quality_smoke.ps1` | PASS, exit 1 |
| `sura_compatibility_gate_smoke.ps1` | PASS, exit 1 |
| `sura_ci_coverage_gate_smoke.ps1` | PASS, exit 1 |
| `sura_pkg_check_smoke.ps1` | PASS, exit 1 |
| `sura_pkg_lint_smoke.ps1` | PASS, exit 1 |
| `sura_engine_lint_smoke.ps1` | PASS, exit 1 |
| `sura_security_audit_bundle_smoke.ps1` | PASS, exit 1 |
| `sura_release_evidence_gate_smoke.ps1` | PASS, exit 1 |
| `sura_engine_test_smoke.ps1` | PASS, exit 1 |

All eighteen found by running rather than reading. **These are gates that pass
and have been reporting failure to CI** - one in nine of the whole suite, and
about one in six of every gate actually run so far.

The pattern in all eighteen is the same: the gate's last native command is a
negative check, something verified to be *correctly rejected*, which exits
nonzero by design. **Validation, policy, audit and verification gates are where
they concentrate**, and that rule is doing the work - the last four batches
were selected by it and produced thirteen of the eighteen. Two of them,
`compatibility_gate_smoke` and `ci_coverage_gate_smoke`, are smoke tests *for
other gates*, so the defect had reached the layer meant to police it.

#### One gate must not be patched: `sura_wasm_memory_safety_smoke.ps1`

It **fails on this machine** - it generates its `.wat` and `.wasm` successfully,
then the node runner hits `RuntimeError: unreachable` (node v24.18.0). It never
prints PASS.

This is the case the batch method exists to catch. Appending `exit 0` here would
be the same one-line edit as the eight above and would have converted a real
failure into a permanent green light - which is worse than the problem being
fixed. It is left untouched pending a decision on whether this is an
environment dependency (node version, wasm runner expectations) or a genuine
defect in the WebAssembly translation path. Note that the translation is done by
`tools/sura_to_wasm.ps1`, not the engine, so it is not downstream of this
session's JIT work.

#### A second gate that must not be patched found two real engine bugs

`sura_pkg_profile_smoke.ps1` also failed rather than merely exiting nonzero, and
running it before patching is what surfaced both of the following. Both are now
fixed; they are recorded because each was invisible to a green suite.

**1. The profiler was blind to JIT'd code.** Every counter is incremented from
the interpreter's dispatch, so whatever ran natively contributed nothing. On
that gate's loop the interpreter reports 3 arithmetic / 2 branch / 1 call site
and the JIT'd run reported 1 / 0 / 0. Zero branch sites is indistinguishable
from a program that has no branches, so the report was *wrong* rather than
visibly incomplete. This was latent for as long as the JIT existed but it only
became a failing gate once this session widened emitter coverage - broadening
the JIT made the profiler worse, which is a coupling nothing in the tree
recorded. Fixed in `jit_vm.hpp` with `native_allowed()`: attaching a profiler
now suppresses native compilation at all five compile sites, so a profile is
slower but true. Instrumenting native code instead is the alternative and is
strictly more work; it is not obviously worth it for a profiler.

**2. `import` did not work under a non-ASCII directory.** The compiler built
import paths with `fs::path`'s narrow constructor, which on Windows interprets
bytes in the active ANSI codepage - but Sura source is UTF-8. Any project under
a directory such as `문서\` could not import at all, **this repository
included**: `import "b.sura"` in `C:\Temp\imp_한글폴더\` failed with
`import 실패 - 파일 없음` naming a file that was sitting right there, while the
identical pair under `C:\Temp\imp_ascii\` worked. The existing code knew
something was wrong - it carried a comment about MinGW and a two-step open
cascade - but treated it as a stream problem rather than an encoding one, so the
cascade retried the same corrupted path twice. Fixed in `jit_compiler.hpp` by
using `fs::u8path` / `u8string()` at every boundary and carrying the resolved
`fs::path` through to the open instead of round-tripping it back to a narrow
string.

A third instance of the same encoding hazard bit the documentation itself
during this work: `Get-Content -Raw` on PowerShell 5.1 decodes using the system
ANSI codepage (CP949 here), so reading a UTF-8 document and writing it back with
`Set-Content -Encoding utf8` destroyed 10% of `CONTRIBUTING.md` - and destroyed
it *irreversibly*, because bytes with no CP949 mapping become U+FFFD. The file
was reconstructed by hand. **Do not rewrite documents through PowerShell.** Use
an editor or `sed`; if PowerShell is unavoidable, name the encoding on both ends
with `[IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)` and
`[IO.File]::WriteAllText($p, $s, (New-Object Text.UTF8Encoding $false))`. The
same ANSI-by-default assumption caused bug 2 in the engine and this in the
tooling; it is worth treating as a class rather than two incidents.

`sura_utf8_path_smoke.ps1` passed throughout the entire time bug 2 existed,
because it exercises the stdlib file APIs and never `import`. It now covers both
independently. The gate's import scenario is modelled directly on the manual
reproduction above; it has not been run against a deliberately reverted binary.

The batches are worth keeping small and area-scoped, because some gates depend
on things this machine may not have - QEMU, a registry service, particular
hardware. Those need to be recognised as environment-dependent rather than
"fixed" into always returning zero, which is the one outcome worse than the
current state. Run the audit with `-FailOnFinding` in CI once the backlog is
clear, so new gates cannot silently rejoin it.

The compatibility gate itself passes against this session's engine - 3
guaranteed source fixtures, 3 historical fixtures, 6 stable API modules with all
112 signatures, bytecode v2/v3, plugin ABI 1.1.0, FFI ABI 1.2.0 - so none of the
19 engine changes broke the stability contract.

### Update 2026-08-15: bug-report bundles

`tools/sura_bug_report.ps1` writes a Markdown bundle carrying the engine
version **and its SHA-256 and byte count**, the engine's own
`--jit-info-json` target report, host details (OS, CPU count, C++ compiler,
GPU), and an optional reproduction.

The hash is the part that matters. "Sura 1.11.1 crashes" is not actionable -
this session alone produced a dozen distinct binaries all reporting 1.11.1, so
a version string does not identify what ran.

A reproduction passed with `-Repro` is executed under **both** the register VM
and `--jit`, with both outputs recorded. A difference between them is not noise
in a bug report; it is the finding.

On "without secrets", which is a requirement rather than a nicety: environment
variables are never collected, no file is read except the one passed as
`-Repro`, and the user profile directory is rewritten to `~` anywhere it
appears in a path. The bundle states what it excludes, so a reader can see the
omissions were deliberate.

One defect worth recording because it defeated the tool's whole purpose: the
first version captured only stdout, and Sura writes runtime errors to stderr.
A division-by-zero reproduction therefore came out as `(no output)`. Both
streams are captured now.

## Definition of a release-quality milestone

A milestone is complete only when:

- the release commit is clean and tagged;
- portable artifacts are built in CI with provenance;
- required test lanes report pass/fail/skip separately;
- no required lane skips;
- compatibility and stable API gates pass;
- security boundaries and external dependencies are current;
- benchmark claims link to reproducible raw evidence;
- experimental work is clearly separated from the stable contract.
