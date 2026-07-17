# Change Log

## 1.11.1

- Adds completion and highlighting for the bounded CPU `autograd.run_onnx` executor and its direct `autograd_run_onnx` alias.
- Adds completion, hover, snippets, and syntax highlighting for deterministic bounded `tokenizer.train_bpe`.
- Starts the engine-backed language server by default, with restart control and built-in provider fallback if the installed engine is unavailable.
- Derives the VSIX filename from package metadata so extension packaging cannot silently retain an older release number.
- Aligns the extension with the 1.11.1 runtime, website, installer, and release verification contract.

## 1.11.0

- Connected the extension to the engine-backed language server for diagnostics, navigation, rename, formatting, semantic tokens, and code actions; added restart control and basic-provider fallback when the server is unavailable.
- Added `Sura: Create Starter Project`, which creates a runnable package with source, tests, and VS Code settings through `surapkg new`.
- Added package Run and Test commands and a first-use walkthrough covering project creation, execution, testing, and debugging.
- Updated extension, runtime, website, and guide metadata for the 1.11.0 Starter Pack release.

## 1.10.0

- Rebuilt the VSIX against the 1.10.0 engine and documented the guarded
  strict counted-loop JIT path; `Sura: Run File (JIT)` automatically uses it
  when the runtime shape proof succeeds and falls back otherwise.
- Added isolated `async.sura` program tasks with bounded input, output, timeout, cancellation, and process-tree cleanup.
- Added JIT frame reuse with GC-visible roots, exception-safe VM frame cleanup, and no-replay native exception propagation.
- Added stricter bytecode frame validation and FFI run-bound value quarantine for closures, upvalues, and class instances.
- Updated runtime, package, editor, website, and reference metadata for the 1.10.0 API and verification records.

## 1.9.0

- Added strict type checking as the default execution and package-check mode, with explicit `--legacy-types` compatibility for source execution.
- Added bounded worker-pool async execution, cancellation, task status/error propagation, and structured scopes.
- Added JIT numeric guards, Win64 helper-call alignment fixes, exact record constructors, SIMD frame initialization, and lazy string-concatenation correctness fixes.
- Added GC shutdown, FFI persistent roots and serialized multi-context safety, ABI 1.1, and updated completion/hover metadata for the 1.9 runtime APIs.

## 1.8.3

- Added the default CUDA fused causal-attention path: one f32 warp online-softmax forward plus deterministic dQ and combined dK/dV recomputation kernels.
- Replaced the default packed O(T²) backward with deterministic fused recomputation; the legacy packed fallback remains available with `SURA_CUDA_ATTENTION_FUSED=0`, and a full q/k/v graph drops from six attention launches to three.
- Added the explicit `precision: "auto" | "fast" | "strict"` contract, fused/fast launch diagnostics, and arbitrary 32-lane tail coverage. `fast` requires the optimized CUDA path, while `strict` pins the f64 reference path.

## 1.8.2

- Added actual 2-byte CUDA float16/bfloat16 storage, device casts, typed cuBLAS/PTX matmul, and f32 master-weight SGD/Adam documentation.
- Added checkpoint v3 IntelliSense for CUDA master/moment/velocity state and exact `{optimizer: true, device: "cuda"}` leaf resume.
- Documented the verified memory boundary: low inference weights are smaller, while current low Adam steady state is 18N versus f32 16N.

## 1.8.1

- Added IntelliSense and highlighting for compute-only CUDA `autocast`, `backward_scaled`, transactional `unscale_gradients`, and payload-free `grad_info`; the native `autograd` surface now has 66 APIs.
- Added float16/bfloat16 CUDA matmul compute dispatch metadata and matmul-only explicit `compute_dtype` snippets while keeping public storage and gradients float32; `linear` follows the current autocast state.
- Documented `grad_info` fields for actual gradient dtype/device/bytes, scale, leaf, `requires_grad`, and basic optimizer readiness; CUDA gradients are float32 and CPU gradients are float64.
- Documented loss-scale validation, common-scale/empty-gradient unscale behavior, and already-unscaled rejection.

## 1.8.0

- Added optional dynamically loaded cuBLAS SGEMM acceleration with explicit reference-PTX fallback and backend counters.
- Added CUDA-resident forward/backward coverage for exact GELU, LayerNorm, embedding, and `cross_entropy_ids`.
- Added rank 2–8 arbitrary-axis CUDA transpose and verified split/merge multi-head training graphs.
- Added score-matrix-free warp-per-row causal-attention forward for `T >= 8` and deterministic packed probability/dScore backward with selective Q/K/V launches.
- Added sticky attention dispatch plans, a 64 MiB configurable packed-workspace limit, serial reference fallback, and exclusive reference/warp/packed launch counters.
- Added transactional CUDA SGD so non-finite multi-parameter updates are rejected without partial commits.
- Hardened dtype boundaries so float16/bfloat16 overflow is rejected before storage quantization or checkpoint materialization.

## 1.7.0

- Added true float32 CUDA-resident tensors with explicit `device` placement, `device()`/`to()`, and audited allocation/transfer/kernel counters.
- Added resident rank-2 matmul, standard column-bias linear layers, same-shape/scalar elementwise operations, ReLU, sum/mean, MSE composition, CUDA backward, and persistent device gradients.
- Added resident SGD and transactional CUDA Adam, plus lazily allocated host mirrors for CUDA intermediates.
- Added complete IntelliSense and syntax highlighting for all 62 native `autograd` APIs, including the four new device/residency APIs.

## 1.6.0

- Added IntelliSense and syntax highlighting for 58 native `autograd` APIs, including typed storage, the first CUDA matmul backend, Safetensors, ONNX weight interchange, and shared-filesystem gradient all-reduce.
- Added complete direct and module-member IntelliSense for the six `tokenizer` APIs and six streaming `dataset` APIs.
- Updated Tensor creation signatures and snippets for `float64`, `float32`, `float16`, and `bfloat16` storage.

## 1.5.0

- Added IntelliSense, signatures, and syntax highlighting for all 47 native `autograd` APIs.
- Added Transformer-oriented APIs for reshape, batched matmul, arbitrary-axis transpose, GELU, LayerNorm, embedding, causal attention, and sparse token-ID cross entropy.
- Added checkpoint save/load and runtime Tensor/attention limit discovery.

## 1.4.0

- Added complete IntelliSense for the native `autograd` module and all direct `autograd_*` built-ins.
- Added module-member completion, hover, signature, and snippet support for `nn`, its `ai` alias, and `autograd`.
- Added syntax highlighting for native neural-network and autograd functions.
- Improved signature-help parameter tracking for nested calls, arrays, dictionaries, and strings.
- Added debugger parsing for Tensor display values, including tensors nested inside dictionaries.
- Documented the native AI/autograd workflow and tightened extension packaging metadata.

## 1.3.2

- Added native `nn` and `ai` module discovery and direct neural-network built-in completions.
- Improved run/debug commands, project-symbol completion, and editor completion hygiene.
