# Sura Security Policy

## Supported releases

The machine-readable support contract is `compatibility.json`. The current release is 1.11.1. The 1.11 series is active starting at 1.11.1 and its maintenance end is not scheduled before 2027-07-16. Releases older than 1.11.1 are outside the published source-compatibility and maintenance contract.

The compatibility contract does not make every feature stable. Native syntax, the register VM, the core values and control-flow model, the documented core APIs, the basic `surapkg` workflow, the C FFI, and the versioned plugin ABI are in the stable tier. JIT, CUDA, media, and Windows distribution paths are platform-limited. JavaScript/WASM targets, Transformer and distributed-training extensions, BPE and bounded ONNX execution, advanced protected-release policies, and external registry operations are experimental. See `COMPATIBILITY.md` and `compatibility.json` for the exact current lists.

## Reporting a vulnerability

Do not publish exploit details, secrets, crash inputs, or proof-of-concept code in a normal GitHub issue.

1. On the repository's GitHub **Security** page, use **Report a vulnerability** when private vulnerability reporting is available.
2. If that button is unavailable, open the **Private security contact request** issue template. Include only the affected Sura version and ask for a private contact channel. Do not include technical vulnerability details in that public issue.
3. In the private report, include the affected version and platform, required feature flags or environment variables, the smallest reproducible input, observed impact, and whether the issue is already public.

No response or repair deadline is promised by this repository. A report should not be described as accepted or fixed until the repository maintainer confirms that state.

## Runtime trust boundaries

- The Sura runtime is not an operating-system sandbox. A script can read or write files, make network requests, or start commands when it calls the corresponding APIs and the host process has permission.
- Tool-policy and approval APIs constrain calls that use those policy-aware tool paths. They do not turn direct filesystem, network, command, Python, FFI, or plugin APIs into a process sandbox.
- FFI and native plugins load native code into the runtime process. Hash checks, manifest allow-lists, time limits, and cooperative cancellation reduce specific risks but do not make an untrusted native library safe to load.
- Protected release packages provide tamper checks and practical source non-disclosure. They are not mathematical DRM, a secret vault, or protection against a capable party who controls the runtime and machine.
- The JavaScript and WebAssembly targets are experimental translations with documented partial compatibility. They must not be treated as security-equivalent replacements for the register VM.
- FFmpeg, CUDA, Python, Node.js, compilers, native libraries, and registry services are separate dependencies or processes. Their security and update state is not established by Sura's own test results.

## Current assurance evidence

Repository-owned verification currently includes:

- stable VM/JIT regression tests with per-process timeouts;
- parser and bytecode malformed-input regression harnesses;
- GC and FFI memory-safety harnesses;
- Linux ASan/UBSan runs and a structured-async TSan run;
- deterministic VM/JIT allocation, GC, closure, exception, async, BPE, and ONNX soak cycles, with a scheduled multi-platform workflow;
- compatibility, API snapshot, protected-package tamper/leak, package-signature, and tool-policy gates.
- a source-review handoff bundle with per-file SHA-256 and byte counts, exact reproduction commands, explicit trust boundaries, and an optional hash-only engine record.

These checks provide regression evidence only. As of 2026-07-16, this repository contains no report from an independent external security audit. Passing the checks does not prove the absence of memory-safety, logic, supply-chain, or sandbox-escape defects.

`SECURITY_AUDIT.md` and `tools/sura_security_audit_bundle.ps1` prepare review material for an external auditor. Creating or publishing that bundle is not an independent audit, certification, or vulnerability assessment.

## Disclosure and fixes

Security fixes should include a regression test when a safe automated reproduction is possible. Public disclosure should wait until affected supported releases have a fix or mitigation and the maintainer and reporter have agreed that publication is appropriate. Published advisories and release notes, when present, are the authoritative public record; this file does not claim that an unpublished report exists or has been resolved.
