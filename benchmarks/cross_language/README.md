# Cross-language benchmark

The same eight workloads written in Sura, C++, Rust, Go, C#, Java, JavaScript
and Python, so a claim about Sura's speed can be checked rather than believed.

```powershell
.\benchmarks\cross_language\run.ps1
```

The runner builds whatever compilers are installed, skips the languages that are
not, and prints one table. Nothing here is required by CI; it exists to be
audited.

**Read the platform section before quoting any number.** Sura's JIT is complete
on Windows x64 only; on Linux and ARM64 the native tier is a baseline that
refuses function calls, array indexing and field access, so most of this suite
runs interpreted there. The two tables differ a lot, and the Linux one is the
less flattering of the two.

## Why the checksums matter

Two programs that finish in different times are only comparable if they did the
same work. Every implementation therefore accumulates its results into a
checksum and prints it. If a compiler had optimized a benchmark away, or an
implementation computed something slightly different, the checksums would
diverge and the timings would be meaningless.

Two divergences are expected and harmless:

- **JavaScript and Sura** differ from the rest in the last few digits. Their
  numbers are IEEE doubles, so `seed * 1103515245` in the sort benchmark's
  pseudo-random generator exceeds 53 bits of precision and produces a different
  sequence than the 64-bit integer languages. The array being sorted differs;
  the amount of work does not.
- **Rust** differs in the fractional part because its checksum accumulator is an
  integer (`AtomicU64`), which truncates.

## Method

- Each program warms up, then runs each workload five times and reports the
  fastest run. The fastest run is the one least polluted by scheduling noise.
- Every repetition uses `n + repetition` as its size rather than a constant.
  Without this, Go collapsed five identical calls into one and reported a time
  no work could achieve.
- Results are consumed into a global sink so a compiler cannot delete the work.
  Rust additionally wraps inputs in `std::hint::black_box`, because it will
  otherwise evaluate `fib(30)` at compile time.
- Timings come from each language's own monotonic clock, around the workload
  only, excluding process startup.

## What the workloads are

| name | what it does |
| --- | --- |
| `fib` | `fib(30)` by naive recursion — function call overhead |
| `numeric` | 3,000,000 iterations of `acc += (i*3-1)/2` — scalar float math |
| `array` | build a 1,000,000-element list, then sum it |
| `string` | build 200,000 pieces and join them |
| `dict` | 200,000 hash-map insert-or-increment operations |
| `sort` | sort 300,000 pseudo-random numbers |
| `object` | create 500,000 two-field objects, mutate and read them |
| `matmul` | 256×256 matrix multiply, hand-written triple loop in every language |

## Results

Measured on an Intel i5-12400F (6 cores / 12 threads), Windows 11, in ms.
Lower is better. Rerun `run.ps1` to reproduce on your own machine; absolute
numbers will differ, the shape should not.

| language | fib | numeric | array | string | dict | sort | object | matmul |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| C++ -O2 | 0.9 | 1.4 | 2.1 | 2.9 | 16.0 | 16.5 | 0.3 | 5.7 |
| Rust -O | 1.7 | 1.4 | 2.3 | 1.2 | 30.5 | 6.3 | 0.3 | 10.4 |
| Go 1.26 | 3.2 | 1.0 | 2.0 | 1.0 | 13.9 | 34.1 | ~0 | 9.1 |
| C# .NET 10 | 3.5 | 1.4 | 2.2 | 1.0 | 14.4 | 39.9 | 1.5 | 8.3 |
| Java 25 | 3.1 | 1.4 | 10.7 | 2.3 | 4.5 | 17.4 | 0.3 | 2.2 |
| Node 24 | 7.6 | 1.7 | 8.4 | 2.6 | 17.0 | 73.2 | 0.4 | 10.8 |
| **Sura JIT** | 22.3 | 15.2 | 38.8 | 10.8 | 65.8 | 29.6 | 81.3 | 219.5 |
| Sura VM | 75.1 | 71.0 | 65.5 | 12.9 | 98.9 | 34.9 | 148.1 | 549.0 |
| Python 3.12 | 82.2 | 184.5 | 96.9 | 7.3 | 37.4 | 95.8 | 81.3 | 699.3 |

Go's `object` column is zero because its compiler removes the loop entirely;
varying the input per repetition did not stop it. Read that cell as "not
measured" rather than as a time.

## The same benchmark on Linux

The numbers above are from Windows x64, where the JIT is complete. On Linux and
on ARM64 the native tier is a **baseline** that compiles numeric code only. On
Linux x86-64 it handles loops, guarded global reads and native-to-native calls
between pure numeric functions (so recursive `fib` runs entirely as native
code), but it still refuses array indexing, strings, dictionaries and field
access. In this suite that means two of the ten functions get native code
instead of all ten, and the rest run interpreted. ARM64 does not have the
call support yet, so only the numeric loop reaches native code there.

Same machine, Ubuntu under WSL2, ms:

| language | fib | numeric | array | string | dict | sort | object | matmul |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| C++ -O2 | 0.8 | 1.4 | 0.8 | 1.4 | 13.4 | 15.7 | 0.3 | 4.8 |
| **Sura JIT** | 9.3 | 10.5 | 59.0 | 9.9 | 77.0 | 31.7 | 108.4 | 560.8 |
| Sura VM | 78.0 | 79.8 | 65.7 | 10.0 | 79.0 | 31.6 | 107.0 | 556.3 |
| Python 3.14 | 59.8 | 109.3 | 56.7 | 3.9 | 22.6 | 73.6 | 45.1 | 467.2 |

So the honest summary is platform-dependent, and it is worth stating plainly
rather than quoting the better platform:

- **On Windows x64** the JIT compiles everything in this suite and Sura runs
  about twice as fast as CPython by geometric mean.
- **On Linux x86-64** the numeric loop and the recursive `fib` reach native
  code. Overall Sura is about 1.25x faster than CPython by geometric mean —
  well ahead on calls and arithmetic, behind on strings, dictionaries and
  objects, which still run interpreted.
- **On ARM64** only the numeric loop reaches native code, and Sura lands within
  about 10% of CPython overall.

What holds on both platforms is where the native tier or a native library
actually applies:

| | Windows | Linux |
| --- | ---: | ---: |
| `fib(30)` vs CPython | 3.7x faster | 6.4x faster |
| numeric loop vs CPython | 12x faster | 10x faster |
| sort vs CPython | 3.2x faster | 2.3x faster |
| startup vs CPython | 2.3x faster | 3.2x faster |
| `autograd.matmul` 256x256 | 0.98 ms | 1.88 ms |

Closing the rest of the Linux gap means teaching the baseline tier the array,
string, dictionary and field opcodes it currently refuses, which is the largest
open item in the project.

### How to read this

On Windows, by geometric mean Sura's JIT is about twice as fast as CPython,
roughly six and a half times slower than Node, and fourteen times slower than
C++. On Linux, see the platform section above — the summary there is different
and less flattering.

Sura is competitive at sorting, because `array.sort` calls a native C++ sort
rather than interpreting a comparison per element.

Sura is worst at `object`. There is no escape analysis, so every object in a
loop is really allocated: measured separately, creating one instance costs about
145 ns, against roughly 0.6 ns per iteration for the compiled languages, which
do not allocate at all. Splitting that benchmark shows where the time goes —
allocating each iteration takes 77 ms, reusing one object takes 4.1 ms, and
using no object at all takes 2.5 ms. Ninety-five percent of it is allocation and
collection; the field access itself is already cheap.

### The matmul column is the naive loop, not the fast path

Every language runs a hand-written triple loop there, which is the fair
comparison for "what the language does on its own". It is not the fastest way to
multiply matrices in Sura. Through the built-in kernel:

```sura
use autograd
C is autograd.matmul(A, B)
```

the same 256×256 product takes **0.98 ms**, which is faster than every hand
written loop in the table including C++ at 5.7 ms and Java at 2.2 ms. At 512×512
it takes 5.2 ms against 10.0 ms for a cache-blocked C++ implementation compiled
with `-O3 -march=native`. The kernel is threaded and dispatches to an AVX2 path
at run time, because the shipped binary is built portable and cannot assume the
instruction set at compile time.

That comparison is only fair in one direction: the other languages would also
win with BLAS. It says Sura ships a good kernel, not that its code generation
beats a C++ compiler.

## Startup

Process startup, measured separately as the wall time of a hello-world program,
best of seven runs:

| C++ | Rust | Go | **Sura** | Python | Node | Java |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 5.2 | 6.4 | 7.4 | **9.9** | 23.0 | 41.6 | 62.2 |

Sura starts faster than every other scripting language here, which matters for
small tools that run often and exit.
