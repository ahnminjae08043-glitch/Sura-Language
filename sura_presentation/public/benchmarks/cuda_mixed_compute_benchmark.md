# Sura CUDA Mixed-Compute Benchmark

- Status: `ok`
- Comparison mode: `required_pytorch`
- GPU: NVIDIA GeForce RTX 4060
- Performance shape: `1024 x 1024`
- Correctness shape: `16 x 16`
- Samples: 10 warmup, 50 measured per workload/dtype
- GPU preflight utilization: median 12%, peak 13% across seven one-second samples `13, 13, 12, 12, 13, 12, 11` (public median limit 15%; performance valid: True)
- Timing: synchronized eager forward/autograd wall-clock; not a pure GEMM-kernel timer
- Shared input SHA-256: `07b8097a0707f8b6a15bd9faac3209b3003dd7fe07f7131ef5ef6362c80b551a`
- NVIDIA_TF32_OVERRIDE: `0` (caller value restored after the run)
- Ratios: Sura/PyTorch; lower is better
- Validation: True
- PyTorch: 2.11.0+cu128, f32 policy ieee, precision highest

| dtype | workload | Sura storage/output | Sura median ms | Sura p95 ms | PyTorch storage/output | PyTorch median ms | PyTorch p95 ms | Sura/PyTorch |
|---|---|---|---:|---:|---|---:|---:|---:|
| float32 | forward | f32 / float32 | 0.6794 | 0.9223 | float32 / float32 | 0.3197 | 0.4468 | 2.125 |
| float32 | forward+backward | f32 / float32 | 3.683 | 4.1984 | float32 / float32 | 0.8921 | 1.3091 | 4.128 |
| float16 | forward | f32 / float32 | 0.4611 | 0.6358 | float16 / float16 | 0.1163 | 0.1851 | 3.965 |
| float16 | forward+backward | f32 / float32 | 3.2583 | 4.1174 | float16 / float16 | 0.4102 | 0.6647 | 7.943 |
| bfloat16 | forward | f32 / float32 | 0.4995 | 0.7143 | bfloat16 / bfloat16 | 0.1231 | 0.1725 | 4.058 |
| bfloat16 | forward+backward | f32 / float32 | 4.089 | 5.1637 | bfloat16 / bfloat16 | 0.498 | 1.7437 | 8.211 |

## Correctness hard gate

- float32: output, dInput, and dWeight sum/L1/L2/first/center/last compared; aggregate abs=0.01, sample abs=0.001, rel=0.0001
- float16: output, dInput, and dWeight sum/L1/L2/first/center/last compared; aggregate abs=0.5, sample abs=0.02, rel=0.005
- bfloat16: output, dInput, and dWeight sum/L1/L2/first/center/last compared; aggregate abs=5, sample abs=0.25, rel=0.025

## Dispatch validation

- float32: static FAST eligible=False; forward=cublas_sgemm (FAST/fallback=0/0); forward+backward=cublas_sgemm (FAST/fallback=0/0); correctness=cublas_sgemm (FAST/fallback=0/0)
- float16: static FAST eligible=True; forward=cublas_gemmex_fast (FAST/fallback=50/0); forward+backward=cublas_gemmex_fast (FAST/fallback=150/0); correctness=cublas_gemmex_fast (FAST/fallback=3/0)
- bfloat16: static FAST eligible=True; forward=cublas_gemmex_fast (FAST/fallback=50/0); forward+backward=cublas_gemmex_fast (FAST/fallback=150/0); correctness=cublas_gemmex_fast (FAST/fallback=3/0)

## Reproducibility

- Engine SHA-256: `d2fccece42264704b941ae1d7a6c058cd18f09e797476431b9fcae09f27eb70e`
- Sura benchmark SHA-256: `5b48cc7945cc5e53fd910a99b282d0592a0b7e9d6098223cb510146bc78ed92c`
- PyTorch helper SHA-256: `182cac0778835c6f68339c62e734116b1187d99b8d45051164268cfb73486208`
- PowerShell driver SHA-256: `f877f530a0085674a6424c81b4b27d0de6811a9d3ce9189a0602763d5451b528`
- cuBLAS DLL SHA-256: `9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99`
- Python executable SHA-256: `0b471133e110cfb53a061cad528ce8e517d7b9ac41a0a396c39ad795a487fc14`

## Interpretation

Sura's float16 and bfloat16 rows are **compute-only mixed precision**: inputs, weights, outputs, and gradients remain float32 (4 bytes/element). They do not show 2-byte VRAM or bandwidth savings. PyTorch's low-precision rows use native float16/bfloat16 storage and output when the required comparison mode is enabled, so low-precision storage semantics are not identical.

Every measured sample uses host wall-clock timing around synchronized eager framework work. It includes dispatch, allocation, graph construction/traversal, and the final synchronization. It is not an isolated CUDA-event GEMM-kernel measurement.

`NVIDIA_TF32_OVERRIDE=0` is applied to both child processes and PyTorch additionally requests highest/IEEE float32 policy. Sura uses `cublasSgemm_v2` with the cuBLAS handle's default math mode. This controls TF32 acceleration but is not instruction-level proof that the two libraries execute identical IEEE instruction sequences.

`cublas_fast_matmul_launches` proves a successful FAST compute request, not HMMA execution. A PTX fallback is legal when the architecture, symbol, or cuBLAS operation does not support FAST compute and is reported separately. Nsight Compute is required for instruction-level proof.
