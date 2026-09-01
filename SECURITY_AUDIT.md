# Sura security review handoff

This document prepares Sura for an independent source review. It is not an audit report, certification, penetration-test result, or claim that the runtime is secure.
As of 2026-07-16, this repository does not contain a report from an independent external security audit.

## Review scope

The handoff targets Sura 1.11.1 and the 1.11 compatibility contract. The
machine-readable version and support sources are `version.json` and
`compatibility.json`.

The highest-priority code paths are:

- parsing and validation: `lexer.hpp`, `parser.hpp`, `typechecker.hpp`,
  `compiler.hpp`, and `bytecode_io.hpp`;
- execution and memory ownership: `value.hpp`, `gc.cpp`, `jit_vm.hpp`,
  `jit_native.hpp`, `jit_x64.hpp`, and the platform-specific JIT paths;
- untrusted model and data formats: `checkpoint.hpp`, `safetensors.hpp`,
  `onnx_weights.hpp`, `tokenizer.hpp`, and `dataset.hpp`;
- privileged host access: filesystem, process, HTTP, Python, FFI, plugin, CUDA,
  media, and registry paths in `stdlib.hpp`, `sura_ffi.cpp`, `media.hpp`,
  `cuda_backend.hpp`, and `surapkg.cpp`;
- concurrency and cancellation: structured async code in `stdlib.hpp` and the
  distributed-training paths in `distributed.hpp`.

The operating system, compiler, FFmpeg, CUDA driver/toolkit, Python, Node.js,
downloaded native libraries, and registry hosting environment are external
trust dependencies. The audit handoff records Sura's integration surfaces but
does not include the source of those projects.

## Trust boundaries that must remain explicit

- Sura is not an operating-system sandbox.
- FFI and plugins execute native code in the runtime process.
- Tool approval policies cover policy-aware tool calls, not every direct host
  API.
- JavaScript, WebAssembly, CUDA, media, distributed training, BPE, and bounded
  ONNX execution have the support tiers stated in `compatibility.json`.
- A passing regression, sanitizer, fuzz-style mutation, or soak test is not
  proof that a vulnerability is absent.

## Reproduction commands

From a clean checkout on Ubuntu with `g++`, `make`, and PowerShell installed:

```sh
make clean
make CXX=g++ CXXFLAGS="-std=c++17 -O2 -DNDEBUG -Wall"
pwsh -NoProfile -File ./run_stable_tests.ps1 -Engine ./SuraLanguage
pwsh -NoProfile -File ./tools/sura_untrusted_input_smoke.ps1 -Cxx g++
pwsh -NoProfile -File ./tools/sura_runtime_soak.ps1 -Engine ./SuraLanguage -DurationSeconds 60 -PerRunTimeoutSeconds 120 -JsonOut artifacts/runtime_soak_audit.json
```

Memory and undefined-behaviour checks:

```sh
make clean
make CXX=g++ CXXFLAGS="-std=c++17 -O1 -g -DNDEBUG -Wall -fno-omit-frame-pointer -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" engine
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:strict_string_checks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./SuraLanguage ./tests/71_onnx_execution.sura
pwsh -NoProfile -File ./tools/sura_gc_memory_safety_smoke.ps1 -Cxx g++ -Sanitize
pwsh -NoProfile -File ./tools/sura_ffi_safety_smoke.ps1 -Cxx g++ -Sanitize
pwsh -NoProfile -File ./tools/sura_untrusted_input_smoke.ps1 -Cxx g++ -Sanitize
```

The complete sanitizer and thread-sanitizer commands are authoritative in
`.github/workflows/cross-platform-smoke.yml`. The scheduled multi-platform soak
configuration is in `.github/workflows/runtime-soak.yml`.

## Audit handoff bundle

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_security_audit_bundle.ps1 -RepoRoot . -Engine .\SuraLanguage.exe
```

The command creates a source review directory, a ZIP archive, a JSON report,
and a Markdown summary under `artifacts`. The bundle includes source and test
hashes, the current compatibility contract, the exact reproduction commands,
and the current engine hash when an engine is supplied. It deliberately does
not include the engine binary, secrets, registry data, signing keys, or a
fabricated external-audit result.

Reviewers should verify every copied file against `manifest.json` before using
the bundle. Findings and crash inputs must follow the private reporting process
in `SECURITY.md`.
