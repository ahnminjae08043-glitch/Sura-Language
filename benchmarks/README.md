# Sura AI benchmarks

Four complementary, reproducible benchmark paths are kept here:

- `ai_backend_benchmark.sura` measures the public CPU path and the compatibility
  CUDA call for one rank-2 float32 matrix multiplication. Its CUDA timing includes
  the API's normal transfers and synchronization.
- `ai_resident_cuda_benchmark.sura` measures an already-resident CUDA forward
  graph and a complete resident training step. Uploads happen before warmup and
  before CUDA counters are reset. The report records allocation, H2D, D2H, D2D,
  and kernel-launch counters both before and after the final scalar readback.
- `causal_attention_benchmark.sura` measures direct rank-4 float32 causal
  attention plus scalar-sum backward on CPU and resident CUDA. The companion
  consumes the same Safetensors inputs in PyTorch when it is installed.
- `cuda_mixed_compute_benchmark.sura` measures aligned resident GEMM forward
  and forward+backward for f32, f16-compute, and bf16-compute. It records the
  dtype/backend counters, queries actual gradient dtype/device/bytes through
  payload-free `autograd.grad_info`, and compares the same deterministic
  Safetensors input with a PyTorch companion while preserving the different
  storage semantics.

Run the existing compatibility benchmark:

```powershell
pwsh -File tools/sura_ai_benchmark.ps1
```

Run the resident forward/training benchmark:

```powershell
pwsh -File tools/sura_ai_resident_benchmark.ps1
```

Run the causal-attention benchmark:

```powershell
pwsh -File tools/sura_attention_benchmark.ps1 -BatchSize 1 -Heads 4 -SequenceLength 128 -HeadDim 64
```

Run the CUDA mixed-compute benchmark with explicit cuBLAS and PyTorch paths:

```powershell
pwsh -File tools/sura_mixed_compute_benchmark.ps1 -Engine .\SuraLanguage.exe -Python C:\path\to\python.exe -CublasLibrary C:\path\to\cublas64_12.dll
```

Select a Python environment explicitly when PyTorch is not installed in the
default `python`:

```powershell
pwsh -File tools/sura_ai_resident_benchmark.ps1 -Python C:\path\to\python.exe
```

All four runners generate JSON and Markdown files under `artifacts/`. If Python,
PyTorch, and Safetensors are installed, the same input Safetensors file is also
used by the PyTorch companion. Each timed GPU sample is explicitly synchronized,
and process startup, input generation, and initial host-to-device upload are not
timed.

The mixed-compute workload uses aligned 1024×1024 GEMMs. Sura f16/bf16 rows
keep f32 input, weight, output, gradient, and optimizer storage while requesting
low-precision operand compute; PyTorch rows use native 2-byte low-precision
storage/output. Consequently it is a transparent backend-throughput comparison,
not an identical-memory-semantics comparison. `cublas_fast_matmul_launches`
proves a successful Tensor-Core-eligible FAST request, not instruction-level
HMMA execution; that requires a profiler. `grad_info` reads metadata rather than
the gradient payload, so its CUDA f32 `storage_bytes` check does not introduce a
payload D2H transfer; CPU gradients reported by the same API are f64 regardless
of the CPU Tensor storage dtype.

The mixed runner samples `nvidia-smi` seven times at one-second intervals and
uses the median as its preflight gate while retaining every sample and the peak.
The default public limit is 5%. `-AllowBusyGpu` is available only for
counter/correctness diagnostics and marks `performance_valid: false`.

The canonical 2026-07-12 RTX 4060 run used an explicit 15% limit because the
Codex desktop renderer held a persistent baseline. It records samples
`13, 13, 12, 12, 13, 12, 11` (median 12%, peak 13%) rather than presenting the
machine as lab-idle. With 10 warmups and 50 measured samples, Sura/PyTorch
ratios were 2.125x/4.128x for f32 forward/forward+backward, 3.965x/7.943x for
f16, and 4.058x/8.211x for bf16. Sura was slower in every row. The output,
dInput, and dWeight fingerprint hard gate passed for all dtypes, as did the
same-device, transfer, storage, sample, and dispatch contracts. The engine hash
is `d2fccece42264704b941ae1d7a6c058cd18f09e797476431b9fcae09f27eb70e`.

The resident workload uses 256x256 float32 tensors, three warmup iterations, and
ten measured iterations. Forward is `relu(matmul(input, weight))`. One training
step is `zero_grad + matmul + relu + mse + backward + SGD`. These narrowly scoped
numbers are suitable for tracking the resident backend; they do not establish
full framework parity.

The attention workload excludes projections and head split/merge so it does
not masquerade as an end-to-end Transformer benchmark. Reports distinguish
reference, warp-forward, legacy packed-backward, fused-recomputation-backward,
and fast-forward counters while verifying zero payload transfer before
observation. For T>=8 the default float32 CUDA training path is one warp
online-softmax forward launch plus two deterministic fused recomputation
backward launches. It stores only per-row softmax statistics rather than an
O(T^2) score/probability workspace. Setting `SURA_CUDA_ATTENTION_FUSED=0`
selects the six-launch legacy fallback (one warp forward plus five packed
backward kernels); setting `SURA_CUDA_ATTENTION_PARALLEL=0` selects the
two-launch reference forward/backward path. Every report validates numerical
fingerprints, same-device execution, zero timed H2D/D2H payload transfer, and
the selected launch contract. This is not an end-to-end Transformer benchmark,
a shared-memory tiled/Tensor Core FlashAttention claim, or framework-wide
performance parity.
