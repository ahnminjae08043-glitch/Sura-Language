# Sura Language

**한국어**: [README.md](README.md)

Sura is a standalone programming language implemented in C++. It compiles
`.sura` sources to register bytecode, runs them on a register VM, and applies
its own partial native JIT on Windows x64, Linux x86-64, and little-endian
Windows/Linux/macOS ARM64.

> A fast, Python-easy scripting language for games, automation, and AI agents
> that embeds into native apps like Lua — with a small, inspectable runtime.

## 30 seconds of Sura

```sura
class Particle do
    x is 0.0
    y is 0.0
    vx is 0.0
    vy is 0.0

    func init(x, y, vx, vy) do
        self.x is x
        self.y is y
        self.vx is vx
        self.vy is vy
    end

    func step(dt) do
        self.vy is self.vy - 9.8 * dt
        self.x is self.x + self.vx * dt
        self.y is self.y + self.vy * dt
    end
end

p is new Particle(0.0, 100.0, 1.5, 0.0)

steps is 0
while p.y > 0 do
    p.step(0.016)
    steps is steps + 1
end

print("landed after {steps} steps at x = {p.x}")
```

```text
$ sura --jit particle.sura
landed after 282 steps at x = 6.768000000000005
[JIT] 0 function(s), 1 method(s) compiled to native x86-64 code
```

That `step` method is exactly the shape of loop the JIT turns into native
code. Identifiers are full Unicode — Korean variable, function, and class
names are first-class, and diagnostics are available in Korean (`--lang ko`).

## Performance

Repository-owned measurements (2026-08-15, Windows x64; reproduce with
`run_benchmarks.ps1` before citing — see the honesty note in
[bench_summary.md](bench_summary.md)):

| Metric | Value |
| --- | ---: |
| Median JIT speedup over the Sura VM | 2.96x |
| Median speedup over CPython (19 workloads) | 4.77x |
| Game physics Vec3 loop vs CPython | 170x |
| Game physics loop vs native C++ (same scope) | within 1.4–1.8x |

Every benchmark records engine SHA-256, raw samples, and a fair-scope flag.
The build is deterministic: two clean-clone builds produce byte-identical
binaries, so recorded hashes are claims a reviewer can check.

## What ships in the box

- **Language**: classes with inheritance, structs, enums, closures,
  `match`/`when`, exceptions, string interpolation, strict-by-default type
  checking with type hints.
- **Tooling**: `surapkg` (project scaffold, tests, lockfiles, signed
  packages, registry, CI reports), an LSP server, and a VS Code extension
  with one-click run.
- **Stdlib**: JSON/schema validation, HTTP client and mock servers, regex,
  datetime, crypto, SQLite-style document DB helpers, async tasks, streams —
  plus LLM request/tool-call builders, policy-gated tool execution with
  audit logs, and RAG helpers for AI-agent scripts.
- **Interop**: C FFI, versioned plugin ABI, native embedding scaffolds
  (`surapkg embed`), Python/Node bridges, and experimental JS/WASM targets.
- **Games**: built-in terminal grid/keyboard/mouse APIs and Windows drawing
  primitives — the repo includes playable RPG, Snake, and 3D shooter examples.

## Quick start

```powershell
# Build from source (MinGW-w64 g++, ~2.5 min)
.\build.bat portable

# Run a script
.\SuraLanguage.exe examples\starter\01_hello.sura
.\SuraLanguage.exe --jit bench_physics3d.sura

# Or scaffold a project
.\surapkg.exe new hello_sura
cd hello_sura
..\surapkg.exe run
```

Guides live under [Guide/](Guide/) (Korean; the API reference
[reference.html](reference.html) is generated from the actual engine).
Support tiers and the compatibility contract are in
[COMPATIBILITY.md](COMPATIBILITY.md); current engineering debt is tracked
honestly in [REMAINING_ISSUES.md](REMAINING_ISSUES.md).

## Status

Sura 1.11.x. The core language, VM, stdlib, and `surapkg` are stable-tier
with a patch-compatibility guarantee through at least 2027-07-16. The native
JIT is platform-limited; JS/WASM targets and the AI training stack
(autograd, CUDA, transformers) are experimental. The roadmap is
[ROADMAP.md](ROADMAP.md).

## License

[MIT](LICENSE).
