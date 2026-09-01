#pragma once

// Optional CUDA Driver API backend.
//
// It deliberately uses the stable driver ABI and embedded PTX, so the normal
// Sura executable still builds without CUDA headers, nvcc, cudart, or cuBLAS.
// The NVIDIA display/compute driver is loaded only when a CUDA operation is
// requested. It supports typed f32/f16/bf16 tensor storage for matmul plus a
// bounded f32 resident training subset, with unsupported combinations explicit.

#include <cstdint>
#include <cstddef>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define SURA_CUDA_CALL WINAPI
#else
#include <dlfcn.h>
#define SURA_CUDA_CALL
#endif

namespace SuraStd {

class SuraCudaDriver {
public:
    // A process-local allocation token. It is deliberately not a CUdeviceptr:
    // callers cannot forge pointer arithmetic or use a handle after free.
    using DeviceHandle = uint64_t;
    static constexpr DeviceHandle INVALID_DEVICE_HANDLE = 0;

    // Matmul compute is distinct from physical TensorStorage. Outputs and
    // accumulated gradients remain f32; A/B may be f32 or matching packed
    // f16/bf16 storage, with cuBLAS or embedded PTX providing f32 accumulation.
    enum class MatmulCompute : uint32_t {
        FLOAT32 = 0,
        FLOAT16 = 1,
        BFLOAT16 = 2
    };

    enum class TensorStorage : uint32_t {
        FLOAT32 = 0,
        FLOAT16 = 1,
        BFLOAT16 = 2,
        UINT32 = 3
    };

    struct StatsSnapshot {
        size_t allocated_bytes = 0;
        size_t peak_allocated_bytes = 0;
        uint64_t allocation_calls = 0;
        uint64_t free_calls = 0;
        uint64_t h2d_bytes = 0;
        uint64_t d2h_bytes = 0;
        // Small synchronization/status words are tracked separately from
        // tensor payload traffic so zero-copy compute assertions stay useful.
        uint64_t control_d2h_bytes = 0;
        uint64_t d2d_bytes = 0;
        uint64_t kernel_launches = 0;
        uint64_t matmul_launches = 0;
        // Exactly one of these counters is incremented for every successful
        // matmul dispatch.  `matmul_launches` remains their sum so existing
        // callers keep the same high-level meaning.
        uint64_t cublas_matmul_launches = 0;
        uint64_t reference_matmul_launches = 0;
        uint64_t float32_matmul_launches = 0;
        uint64_t float16_matmul_launches = 0;
        uint64_t bfloat16_matmul_launches = 0;
        uint64_t cublas_fast_matmul_launches = 0;
        uint64_t mixed_matmul_fallback_launches = 0;
        uint64_t typed_storage_matmul_launches = 0;
        uint64_t storage_conversion_launches = 0;
        uint64_t elementwise_launches = 0;
        uint64_t relu_launches = 0;
        uint64_t gelu_launches = 0;
        uint64_t layer_norm_launches = 0;
        uint64_t embedding_launches = 0;
        uint64_t cross_entropy_launches = 0;
        uint64_t attention_launches = 0;
        uint64_t reference_attention_launches = 0;
        uint64_t parallel_attention_launches = 0;
        uint64_t warp_attention_launches = 0;
        uint64_t fused_attention_launches = 0;
        uint64_t fast_attention_forward_launches = 0;
        uint64_t transpose_launches = 0;
        uint64_t reduction_launches = 0;
        uint64_t optimizer_launches = 0;
        bool cublas_available = false;
        bool cublas_gemm_ex_available = false;
        bool cublas_disabled = false;
        bool counter_overflow = false;
    };

private:
    using CUresult = int;
    using CUdevice = int;
    using CUcontext = void*;
    using CUmodule = void*;
    using CUfunction = void*;
    using CUdeviceptr = uint64_t;
    static constexpr CUresult CUDA_SUCCESS = 0;

#ifdef _WIN32
    HMODULE library_ = nullptr;
    HMODULE cublas_library_ = nullptr;
#else
    void* library_ = nullptr;
    void* cublas_library_ = nullptr;
#endif

    // cuBLAS is optional at both build and run time.  These declarations use
    // only its stable C ABI, so the portable Sura binary does not require the
    // CUDA toolkit headers, import libraries, or DLLs to be installed.
    using CublasStatus = int;
    using CublasHandle = void*;
    using CublasCreate = CublasStatus (SURA_CUDA_CALL*)(CublasHandle*);
    using CublasDestroy = CublasStatus (SURA_CUDA_CALL*)(CublasHandle);
    using CublasSgemm = CublasStatus (SURA_CUDA_CALL*)(
        CublasHandle, int, int, int, int, int,
        const float*, const float*, int, const float*, int,
        const float*, float*, int);
    using CublasGemmEx = CublasStatus (SURA_CUDA_CALL*)(
        CublasHandle, int, int, int, int, int,
        const void*, const void*, int, int,
        const void*, int, int, const void*, void*, int, int, int, int);
    using CublasGetStatusString = const char* (SURA_CUDA_CALL*)(CublasStatus);
    static constexpr CublasStatus CUBLAS_STATUS_SUCCESS = 0;
    static constexpr int CUBLAS_OP_N = 0;
    static constexpr int CUBLAS_OP_T = 1;
    static constexpr CublasStatus CUBLAS_STATUS_ARCH_MISMATCH = 8;
    static constexpr CublasStatus CUBLAS_STATUS_NOT_SUPPORTED = 15;
    static constexpr int CUDA_R_32F = 0;
    static constexpr int CUDA_R_16F = 2;
    static constexpr int CUDA_R_16BF = 14;
    static constexpr int CUBLAS_COMPUTE_32F = 68;
    static constexpr int CUBLAS_COMPUTE_32F_FAST_16F = 74;
    static constexpr int CUBLAS_COMPUTE_32F_FAST_16BF = 75;
    static constexpr int CUBLAS_GEMM_DEFAULT = -1;

    CublasCreate cublasCreate_ = nullptr;
    CublasDestroy cublasDestroy_ = nullptr;
    CublasSgemm cublasSgemm_ = nullptr;
    CublasGemmEx cublasGemmEx_ = nullptr;
    CublasGetStatusString cublasGetStatusString_ = nullptr;
    CublasHandle cublas_handle_ = nullptr;
    std::string cublas_library_name_;
    std::string cublas_error_;
    bool cublas_disabled_ = false;

    using CuInit = CUresult (SURA_CUDA_CALL*)(unsigned int);
    using CuDeviceGetCount = CUresult (SURA_CUDA_CALL*)(int*);
    using CuDeviceGet = CUresult (SURA_CUDA_CALL*)(CUdevice*, int);
    using CuDeviceGetName = CUresult (SURA_CUDA_CALL*)(char*, int, CUdevice);
    using CuDeviceComputeCapability = CUresult (SURA_CUDA_CALL*)(int*, int*, CUdevice);
    using CuDeviceTotalMem = CUresult (SURA_CUDA_CALL*)(size_t*, CUdevice);
    using CuCtxCreate = CUresult (SURA_CUDA_CALL*)(CUcontext*, unsigned int, CUdevice);
    using CuCtxDestroy = CUresult (SURA_CUDA_CALL*)(CUcontext);
    using CuModuleLoadData = CUresult (SURA_CUDA_CALL*)(CUmodule*, const void*);
    using CuModuleUnload = CUresult (SURA_CUDA_CALL*)(CUmodule);
    using CuModuleGetFunction = CUresult (SURA_CUDA_CALL*)(CUfunction*, CUmodule, const char*);
    using CuMemAlloc = CUresult (SURA_CUDA_CALL*)(CUdeviceptr*, size_t);
    using CuMemFree = CUresult (SURA_CUDA_CALL*)(CUdeviceptr);
    using CuMemcpyHtoD = CUresult (SURA_CUDA_CALL*)(CUdeviceptr, const void*, size_t);
    using CuMemcpyDtoH = CUresult (SURA_CUDA_CALL*)(void*, CUdeviceptr, size_t);
    using CuMemcpyDtoD = CUresult (SURA_CUDA_CALL*)(CUdeviceptr, CUdeviceptr, size_t);
    using CuCtxSetCurrent = CUresult (SURA_CUDA_CALL*)(CUcontext);
    using CuLaunchKernel = CUresult (SURA_CUDA_CALL*)(
        CUfunction, unsigned int, unsigned int, unsigned int,
        unsigned int, unsigned int, unsigned int, unsigned int,
        void*, void**, void**);
    using CuCtxSynchronize = CUresult (SURA_CUDA_CALL*)();
    using CuGetErrorName = CUresult (SURA_CUDA_CALL*)(CUresult, const char**);
    using CuGetErrorString = CUresult (SURA_CUDA_CALL*)(CUresult, const char**);

    CuInit cuInit_ = nullptr;
    CuDeviceGetCount cuDeviceGetCount_ = nullptr;
    CuDeviceGet cuDeviceGet_ = nullptr;
    CuDeviceGetName cuDeviceGetName_ = nullptr;
    CuDeviceComputeCapability cuDeviceComputeCapability_ = nullptr;
    CuDeviceTotalMem cuDeviceTotalMem_ = nullptr;
    CuCtxCreate cuCtxCreate_ = nullptr;
    CuCtxDestroy cuCtxDestroy_ = nullptr;
    CuModuleLoadData cuModuleLoadData_ = nullptr;
    CuModuleUnload cuModuleUnload_ = nullptr;
    CuModuleGetFunction cuModuleGetFunction_ = nullptr;
    CuMemAlloc cuMemAlloc_ = nullptr;
    CuMemFree cuMemFree_ = nullptr;
    CuMemcpyHtoD cuMemcpyHtoD_ = nullptr;
    CuMemcpyDtoH cuMemcpyDtoH_ = nullptr;
    CuMemcpyDtoD cuMemcpyDtoD_ = nullptr;
    CuCtxSetCurrent cuCtxSetCurrent_ = nullptr;
    CuLaunchKernel cuLaunchKernel_ = nullptr;
    CuCtxSynchronize cuCtxSynchronize_ = nullptr;
    CuGetErrorName cuGetErrorName_ = nullptr;
    CuGetErrorString cuGetErrorString_ = nullptr;

    CUdevice device_ = 0;
    int device_index_ = 0;
    CUcontext context_ = nullptr;
    CUmodule module_ = nullptr;
    CUfunction matmul_f32_ = nullptr;
    CUfunction matmul_ex_f32_ = nullptr;
    CUfunction matmul_typed_f32_ = nullptr;
    CUfunction unpack_u16_f32_ = nullptr;
    CUfunction pack_f32_u16_ = nullptr;
    CUfunction binary_f32_ = nullptr;
    CUfunction fill_f32_ = nullptr;
    CUfunction scale_f32_ = nullptr;
    CUfunction affine_f32_ = nullptr;
    CUfunction finite_status_f32_ = nullptr;
    CUfunction adam_f32_ = nullptr;
    CUfunction bias_add_f32_ = nullptr;
    CUfunction bias_gradient_f32_ = nullptr;
    CUfunction relu_f32_ = nullptr;
    CUfunction relu_backward_f32_ = nullptr;
    CUfunction gelu_f32_ = nullptr;
    CUfunction gelu_backward_f32_ = nullptr;
    CUfunction layer_norm_f32_ = nullptr;
    CUfunction layer_norm_backward_f32_ = nullptr;
    CUfunction layer_norm_parameter_backward_f32_ = nullptr;
    CUfunction embedding_f32_ = nullptr;
    CUfunction embedding_backward_f32_ = nullptr;
    CUfunction cross_entropy_ids_stats_f32_ = nullptr;
    CUfunction cross_entropy_ids_loss_f32_ = nullptr;
    CUfunction cross_entropy_ids_backward_f32_ = nullptr;
    CUfunction causal_attention_f32_ = nullptr;
    CUfunction causal_attention_warp_f32_ = nullptr;
    CUfunction causal_attention_warp_fast_f32_ = nullptr;
    CUfunction causal_attention_fused_query_backward_f32_ = nullptr;
    CUfunction causal_attention_fused_key_value_backward_f32_ = nullptr;
    CUfunction causal_attention_backward_f32_ = nullptr;
    CUfunction causal_attention_probabilities_f32_ = nullptr;
    CUfunction causal_attention_value_backward_f32_ = nullptr;
    CUfunction causal_attention_score_backward_f32_ = nullptr;
    CUfunction causal_attention_query_backward_f32_ = nullptr;
    CUfunction causal_attention_key_backward_f32_ = nullptr;
    CUfunction transpose_f32_ = nullptr;
    CUfunction sum_f32_ = nullptr;
    int device_count_ = 0;
    int compute_major_ = 0;
    int compute_minor_ = 0;
    size_t total_memory_ = 0;
    std::string device_name_;
    std::string error_;
    bool initialized_ = false;
    bool attempted_ = false;
    struct DeviceAllocation {
        CUdeviceptr pointer = 0;
        size_t elements = 0;
        TensorStorage storage = TensorStorage::FLOAT32;
    };
    std::unordered_map<DeviceHandle, DeviceAllocation> allocations_;
    DeviceHandle next_handle_ = 1;
    StatsSnapshot stats_;
    mutable std::mutex mutex_;

    static constexpr const char* kPtx = R"ptx(
.version 6.0
.target sm_50
.address_size 64

.visible .entry sura_matmul_f32(
    .param .u64 param_a,
    .param .u64 param_b,
    .param .u64 param_c,
    .param .u32 param_m,
    .param .u32 param_n,
    .param .u32 param_k
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<16>;
    .reg .f32 %f<5>;

    ld.param.u64 %rd1, [param_a];
    ld.param.u64 %rd2, [param_b];
    ld.param.u64 %rd3, [param_c];
    ld.param.u32 %r1, [param_m];
    ld.param.u32 %r2, [param_n];
    ld.param.u32 %r3, [param_k];

    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    mov.u32 %r8, %ctaid.y;
    mov.u32 %r9, %ntid.y;
    mov.u32 %r10, %tid.y;
    mad.lo.s32 %r11, %r8, %r9, %r10;

    setp.ge.u32 %p1, %r7, %r2;
    setp.ge.u32 %p2, %r11, %r1;
    or.pred %p3, %p1, %p2;
    @%p3 bra DONE;

    mov.f32 %f1, 0f00000000;
    mov.u32 %r12, 0;

LOOP:
    setp.ge.u32 %p1, %r12, %r3;
    @%p1 bra STORE;

    mad.lo.u32 %r13, %r11, %r3, %r12;
    mul.wide.u32 %rd4, %r13, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f2, [%rd5];

    mad.lo.u32 %r14, %r12, %r2, %r7;
    mul.wide.u32 %rd6, %r14, 4;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.f32 %f3, [%rd7];

    fma.rn.f32 %f1, %f2, %f3, %f1;
    add.u32 %r12, %r12, 1;
    bra LOOP;

STORE:
    mad.lo.u32 %r15, %r11, %r2, %r7;
    mul.wide.u32 %rd8, %r15, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f1;

DONE:
    ret;
}

// Row-major C[M,N] = op(A)[M,K] * op(B)[K,N].  When transposed,
// A is stored as [K,M] and B as [N,K].  This is intentionally a simple,
// auditable reference kernel; a future cuBLAS path can replace it without
// changing the opaque allocation API.
.visible .entry sura_matmul_ex_f32(
    .param .u64 param_a,
    .param .u64 param_b,
    .param .u64 param_c,
    .param .u32 param_m,
    .param .u32 param_n,
    .param .u32 param_k,
    .param .u32 param_trans_a,
    .param .u32 param_trans_b,
    .param .u32 param_compute
)
{
    .reg .pred %p<9>;
    .reg .b16 %h<3>;
    .reg .b32 %r<28>;
    .reg .b64 %rd<16>;
    .reg .f32 %f<5>;

    ld.param.u64 %rd1, [param_a];
    ld.param.u64 %rd2, [param_b];
    ld.param.u64 %rd3, [param_c];
    ld.param.u32 %r1, [param_m];
    ld.param.u32 %r2, [param_n];
    ld.param.u32 %r3, [param_k];
    ld.param.u32 %r4, [param_trans_a];
    ld.param.u32 %r5, [param_trans_b];
    ld.param.u32 %r18, [param_compute];

    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    mad.lo.s32 %r9, %r6, %r7, %r8;
    mov.u32 %r10, %ctaid.y;
    mov.u32 %r11, %ntid.y;
    mov.u32 %r12, %tid.y;
    mad.lo.s32 %r13, %r10, %r11, %r12;

    setp.ge.u32 %p1, %r9, %r2;
    setp.ge.u32 %p2, %r13, %r1;
    or.pred %p3, %p1, %p2;
    @%p3 bra MMEX_DONE;

    mov.f32 %f1, 0f00000000;
    mov.u32 %r14, 0;

MMEX_LOOP:
    setp.ge.u32 %p1, %r14, %r3;
    @%p1 bra MMEX_STORE;

    setp.eq.u32 %p4, %r4, 0;
    @%p4 bra MMEX_A_NORMAL;
    mad.lo.u32 %r15, %r14, %r1, %r13;
    bra MMEX_A_READY;
MMEX_A_NORMAL:
    mad.lo.u32 %r15, %r13, %r3, %r14;
MMEX_A_READY:
    mul.wide.u32 %rd4, %r15, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f2, [%rd5];

    setp.eq.u32 %p5, %r5, 0;
    @%p5 bra MMEX_B_NORMAL;
    mad.lo.u32 %r16, %r9, %r3, %r14;
    bra MMEX_B_READY;
MMEX_B_NORMAL:
    mad.lo.u32 %r16, %r14, %r2, %r9;
MMEX_B_READY:
    mul.wide.u32 %rd6, %r16, 4;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.f32 %f3, [%rd7];

    setp.eq.u32 %p6, %r18, 0;
    @%p6 bra MMEX_COMPUTE_READY;
    setp.eq.u32 %p7, %r18, 1;
    @%p7 bra MMEX_QUANTIZE_F16;
    // Round finite f32 operands to bfloat16 (nearest-even), retain them in
    // f32 registers, then use the same f32 accumulator contract as GEMMEx.
    mov.b32 %r19, %f2;
    shr.u32 %r20, %r19, 16;
    and.b32 %r20, %r20, 1;
    add.u32 %r19, %r19, 32767;
    add.u32 %r19, %r19, %r20;
    and.b32 %r19, %r19, 0xffff0000;
    mov.b32 %f2, %r19;
    mov.b32 %r21, %f3;
    shr.u32 %r22, %r21, 16;
    and.b32 %r22, %r22, 1;
    add.u32 %r21, %r21, 32767;
    add.u32 %r21, %r21, %r22;
    and.b32 %r21, %r21, 0xffff0000;
    mov.b32 %f3, %r21;
    bra MMEX_COMPUTE_READY;
MMEX_QUANTIZE_F16:
    cvt.rn.f16.f32 %h1, %f2;
    cvt.rn.f16.f32 %h2, %f3;
    cvt.f32.f16 %f2, %h1;
    cvt.f32.f16 %f3, %h2;
MMEX_COMPUTE_READY:
    fma.rn.f32 %f1, %f2, %f3, %f1;
    add.u32 %r14, %r14, 1;
    bra MMEX_LOOP;

MMEX_STORE:
    mad.lo.u32 %r17, %r13, %r2, %r9;
    mul.wide.u32 %rd8, %r17, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f1;
MMEX_DONE:
    ret;
}

// Row-major typed-storage GEMM. Inputs may independently be f32 (0), f16 (1),
// or bf16 (2); accumulation and output storage are always f32. This is the
// deterministic fallback for native 2-byte storage and mixed-storage backward.
.visible .entry sura_matmul_typed_f32(
    .param .u64 param_a,
    .param .u64 param_b,
    .param .u64 param_c,
    .param .u32 param_m,
    .param .u32 param_n,
    .param .u32 param_k,
    .param .u32 param_trans_a,
    .param .u32 param_trans_b,
    .param .u32 param_storage_a,
    .param .u32 param_storage_b
)
{
    .reg .pred %p<10>;
    .reg .b16 %h<4>;
    .reg .b32 %r<36>;
    .reg .b64 %rd<20>;
    .reg .f32 %f<6>;

    ld.param.u64 %rd1, [param_a];
    ld.param.u64 %rd2, [param_b];
    ld.param.u64 %rd3, [param_c];
    ld.param.u32 %r1, [param_m];
    ld.param.u32 %r2, [param_n];
    ld.param.u32 %r3, [param_k];
    ld.param.u32 %r4, [param_trans_a];
    ld.param.u32 %r5, [param_trans_b];
    ld.param.u32 %r18, [param_storage_a];
    ld.param.u32 %r19, [param_storage_b];

    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    mad.lo.s32 %r9, %r6, %r7, %r8;
    mov.u32 %r10, %ctaid.y;
    mov.u32 %r11, %ntid.y;
    mov.u32 %r12, %tid.y;
    mad.lo.s32 %r13, %r10, %r11, %r12;
    setp.ge.u32 %p1, %r9, %r2;
    setp.ge.u32 %p2, %r13, %r1;
    or.pred %p3, %p1, %p2;
    @%p3 bra MT_DONE;

    mov.f32 %f1, 0f00000000;
    mov.u32 %r14, 0;
MT_LOOP:
    setp.ge.u32 %p1, %r14, %r3;
    @%p1 bra MT_STORE;

    setp.eq.u32 %p4, %r4, 0;
    @%p4 bra MT_A_NORMAL;
    mad.lo.u32 %r15, %r14, %r1, %r13;
    bra MT_A_INDEX_READY;
MT_A_NORMAL:
    mad.lo.u32 %r15, %r13, %r3, %r14;
MT_A_INDEX_READY:
    setp.eq.u32 %p5, %r18, 0;
    @%p5 bra MT_A_F32;
    mul.wide.u32 %rd4, %r15, 2;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.u16 %h1, [%rd5];
    setp.eq.u32 %p6, %r18, 1;
    @%p6 bra MT_A_F16;
    cvt.u32.u16 %r20, %h1;
    shl.b32 %r20, %r20, 16;
    mov.b32 %f2, %r20;
    bra MT_A_READY;
MT_A_F16:
    cvt.f32.f16 %f2, %h1;
    bra MT_A_READY;
MT_A_F32:
    mul.wide.u32 %rd4, %r15, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f2, [%rd5];
MT_A_READY:

    setp.eq.u32 %p7, %r5, 0;
    @%p7 bra MT_B_NORMAL;
    mad.lo.u32 %r16, %r9, %r3, %r14;
    bra MT_B_INDEX_READY;
MT_B_NORMAL:
    mad.lo.u32 %r16, %r14, %r2, %r9;
MT_B_INDEX_READY:
    setp.eq.u32 %p8, %r19, 0;
    @%p8 bra MT_B_F32;
    mul.wide.u32 %rd6, %r16, 2;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.u16 %h2, [%rd7];
    setp.eq.u32 %p9, %r19, 1;
    @%p9 bra MT_B_F16;
    cvt.u32.u16 %r21, %h2;
    shl.b32 %r21, %r21, 16;
    mov.b32 %f3, %r21;
    bra MT_B_READY;
MT_B_F16:
    cvt.f32.f16 %f3, %h2;
    bra MT_B_READY;
MT_B_F32:
    mul.wide.u32 %rd6, %r16, 4;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.f32 %f3, [%rd7];
MT_B_READY:
    fma.rn.f32 %f1, %f2, %f3, %f1;
    add.u32 %r14, %r14, 1;
    bra MT_LOOP;

MT_STORE:
    mad.lo.u32 %r17, %r13, %r2, %r9;
    mul.wide.u32 %rd8, %r17, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f1;
MT_DONE:
    ret;
}

.visible .entry sura_unpack_u16_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .u32 param_count,
    .param .u32 param_storage
)
{
    .reg .pred %p<3>;
    .reg .b16 %h<2>;
    .reg .b32 %r<12>;
    .reg .b64 %rd<8>;
    .reg .f32 %f<2>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_storage];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra UNPACK_DONE;
    mul.wide.u32 %rd3, %r6, 2;
    mul.wide.u32 %rd4, %r6, 4;
    add.s64 %rd5, %rd1, %rd3;
    add.s64 %rd6, %rd2, %rd4;
    ld.global.u16 %h1, [%rd5];
    setp.eq.u32 %p2, %r2, 1;
    @%p2 bra UNPACK_F16;
    cvt.u32.u16 %r7, %h1;
    shl.b32 %r7, %r7, 16;
    mov.b32 %f1, %r7;
    bra UNPACK_STORE;
UNPACK_F16:
    cvt.f32.f16 %f1, %h1;
UNPACK_STORE:
    st.global.f32 [%rd6], %f1;
UNPACK_DONE:
    ret;
}

.visible .entry sura_pack_f32_u16(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .u64 param_status,
    .param .u32 param_count,
    .param .u32 param_storage,
    .param .u32 param_status_bit,
    .param .f32 param_max_finite
)
{
    .reg .pred %p<6>;
    .reg .b16 %h<2>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<10>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.u64 %rd3, [param_status];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_storage];
    ld.param.u32 %r3, [param_status_bit];
    ld.param.f32 %f1, [param_max_finite];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r1;
    @%p1 bra PACK_DONE;
    mul.wide.u32 %rd4, %r7, 4;
    mul.wide.u32 %rd5, %r7, 2;
    add.s64 %rd6, %rd1, %rd4;
    add.s64 %rd7, %rd2, %rd5;
    ld.global.f32 %f2, [%rd6];
    testp.finite.f32 %p2, %f2;
    abs.f32 %f3, %f2;
    setp.le.f32 %p3, %f3, %f1;
    and.pred %p4, %p2, %p3;
    @!%p4 atom.global.or.b32 %r8, [%rd3], %r3;
    setp.eq.u32 %p5, %r2, 1;
    @%p5 bra PACK_F16;
    mov.b32 %r9, %f2;
    shr.u32 %r10, %r9, 16;
    and.b32 %r10, %r10, 1;
    add.u32 %r9, %r9, 32767;
    add.u32 %r9, %r9, %r10;
    shr.u32 %r9, %r9, 16;
    cvt.u16.u32 %h1, %r9;
    st.global.u16 [%rd7], %h1;
    bra PACK_DONE;
PACK_F16:
    cvt.rn.f16.f32 %h1, %f2;
    st.global.u16 [%rd7], %h1;
PACK_DONE:
    ret;
}

// op: 0 add, 1 subtract, 2 multiply, 3 divide.
.visible .entry sura_binary_f32(
    .param .u64 param_a,
    .param .u64 param_b,
    .param .u64 param_out,
    .param .u32 param_count,
    .param .u32 param_op
)
{
    .reg .pred %p<5>;
    .reg .b32 %r<8>;
    .reg .b64 %rd<10>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_a];
    ld.param.u64 %rd2, [param_b];
    ld.param.u64 %rd3, [param_out];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_op];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra BIN_DONE;
    mul.wide.u32 %rd4, %r6, 4;
    add.s64 %rd5, %rd1, %rd4;
    add.s64 %rd6, %rd2, %rd4;
    add.s64 %rd7, %rd3, %rd4;
    ld.global.f32 %f1, [%rd5];
    ld.global.f32 %f2, [%rd6];
    setp.eq.u32 %p2, %r2, 0;
    @%p2 bra BIN_ADD;
    setp.eq.u32 %p3, %r2, 1;
    @%p3 bra BIN_SUB;
    setp.eq.u32 %p4, %r2, 2;
    @%p4 bra BIN_MUL;
    div.rn.f32 %f3, %f1, %f2;
    bra BIN_STORE;
BIN_MUL:
    mul.rn.f32 %f3, %f1, %f2;
    bra BIN_STORE;
BIN_ADD:
    add.rn.f32 %f3, %f1, %f2;
    bra BIN_STORE;
BIN_SUB:
    sub.rn.f32 %f3, %f1, %f2;
BIN_STORE:
    st.global.f32 [%rd7], %f3;
BIN_DONE:
    ret;
}

.visible .entry sura_fill_f32(
    .param .u64 param_out,
    .param .f32 param_value,
    .param .u32 param_count
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<5>;
    .reg .f32 %f<2>;
    ld.param.u64 %rd1, [param_out];
    ld.param.f32 %f1, [param_value];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra FILL_DONE;
    mul.wide.u32 %rd2, %r5, 4;
    add.s64 %rd3, %rd1, %rd2;
    st.global.f32 [%rd3], %f1;
FILL_DONE:
    ret;
}

.visible .entry sura_scale_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .f32 param_scale,
    .param .u32 param_count
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra SCALE_DONE;
    mul.wide.u32 %rd3, %r5, 4;
    add.s64 %rd4, %rd1, %rd3;
    add.s64 %rd5, %rd2, %rd3;
    ld.global.f32 %f2, [%rd4];
    mul.rn.f32 %f3, %f2, %f1;
    st.global.f32 [%rd5], %f3;
SCALE_DONE:
    ret;
}

.visible .entry sura_affine_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .f32 param_scale,
    .param .f32 param_bias,
    .param .u32 param_count
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<5>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.f32 %f1, [param_scale];
    ld.param.f32 %f2, [param_bias];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra AFFINE_DONE;
    mul.wide.u32 %rd3, %r5, 4;
    add.s64 %rd4, %rd1, %rd3;
    add.s64 %rd5, %rd2, %rd3;
    ld.global.f32 %f3, [%rd4];
    fma.rn.f32 %f4, %f3, %f1, %f2;
    st.global.f32 [%rd5], %f4;
AFFINE_DONE:
    ret;
}

// ORs `status_bit` into one shared 32-bit status word if any candidate is
// non-finite. Optimizers can validate every parameter/state candidate before
// committing any of them, with one four-byte host observation per step.
.visible .entry sura_finite_status_f32(
    .param .u64 param_in,
    .param .u64 param_status,
    .param .u32 param_count,
    .param .u32 param_status_bit
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<9>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<2>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_status];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_status_bit];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra FINITE_STATUS_DONE;
    mul.wide.u32 %rd3, %r6, 4;
    add.s64 %rd4, %rd1, %rd3;
    ld.global.f32 %f1, [%rd4];
    testp.finite.f32 %p2, %f1;
    @!%p2 atom.global.or.b32 %r7, [%rd2], %r2;
FINITE_STATUS_DONE:
    ret;
}

// Fused transactional Adam candidate. Old parameter/moments are read-only;
// candidates are written separately and any non-finite state sets status bits.
.visible .entry sura_adam_f32(
    .param .u64 param_old_parameter,
    .param .u64 param_gradient,
    .param .u64 param_old_m,
    .param .u64 param_old_v,
    .param .u64 param_new_parameter,
    .param .u64 param_new_m,
    .param .u64 param_new_v,
    .param .u64 param_status,
    .param .u32 param_count,
    .param .f32 param_learning_rate,
    .param .f32 param_one_minus_beta1,
    .param .f32 param_one_minus_beta2,
    .param .f32 param_correction1,
    .param .f32 param_correction2,
    .param .f32 param_epsilon,
    .param .f32 param_weight_decay
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<10>;
    .reg .b64 %rd<20>;
    .reg .f32 %f<28>;
    ld.param.u64 %rd1, [param_old_parameter];
    ld.param.u64 %rd2, [param_gradient];
    ld.param.u64 %rd3, [param_old_m];
    ld.param.u64 %rd4, [param_old_v];
    ld.param.u64 %rd5, [param_new_parameter];
    ld.param.u64 %rd6, [param_new_m];
    ld.param.u64 %rd7, [param_new_v];
    ld.param.u64 %rd8, [param_status];
    ld.param.u32 %r1, [param_count];
    ld.param.f32 %f1, [param_learning_rate];
    ld.param.f32 %f4, [param_one_minus_beta1];
    ld.param.f32 %f5, [param_one_minus_beta2];
    ld.param.f32 %f6, [param_correction1];
    ld.param.f32 %f7, [param_correction2];
    ld.param.f32 %f8, [param_epsilon];
    ld.param.f32 %f9, [param_weight_decay];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra ADAM_DONE;
    mul.wide.u32 %rd9, %r5, 4;
    add.s64 %rd10, %rd1, %rd9;
    add.s64 %rd11, %rd2, %rd9;
    add.s64 %rd12, %rd3, %rd9;
    add.s64 %rd13, %rd4, %rd9;
    add.s64 %rd14, %rd5, %rd9;
    add.s64 %rd15, %rd6, %rd9;
    add.s64 %rd16, %rd7, %rd9;
    ld.global.f32 %f10, [%rd10];
    ld.global.f32 %f11, [%rd11];
    ld.global.f32 %f12, [%rd12];
    ld.global.f32 %f13, [%rd13];
    testp.finite.f32 %p2, %f10;
    testp.finite.f32 %p3, %f11;
    testp.finite.f32 %p4, %f12;
    testp.finite.f32 %p5, %f13;
    and.pred %p6, %p2, %p3;
    and.pred %p6, %p6, %p4;
    @!%p6 atom.global.or.b32 %r6, [%rd8], 1;
    mov.f32 %f27, 0f00000000;
    setp.ge.f32 %p7, %f13, %f27;
    and.pred %p8, %p5, %p7;
    @!%p8 atom.global.or.b32 %r7, [%rd8], 2;
    fma.rn.f32 %f14, %f10, %f9, %f11;
    sub.rn.f32 %f15, %f14, %f12;
    fma.rn.f32 %f16, %f15, %f4, %f12;
    mul.rn.f32 %f17, %f14, %f14;
    sub.rn.f32 %f18, %f17, %f13;
    fma.rn.f32 %f19, %f18, %f5, %f13;
    testp.finite.f32 %p9, %f16;
    testp.finite.f32 %p10, %f19;
    setp.ge.f32 %p7, %f19, %f27;
    and.pred %p10, %p10, %p7;
    and.pred %p9, %p9, %p10;
    @!%p9 atom.global.or.b32 %r8, [%rd8], 4;
    div.rn.f32 %f20, %f16, %f6;
    div.rn.f32 %f21, %f19, %f7;
    sqrt.rn.f32 %f22, %f21;
    add.rn.f32 %f23, %f22, %f8;
    div.rn.f32 %f24, %f20, %f23;
    neg.f32 %f25, %f1;
    fma.rn.f32 %f26, %f25, %f24, %f10;
    testp.finite.f32 %p11, %f26;
    @!%p11 atom.global.or.b32 %r9, [%rd8], 8;
    st.global.f32 [%rd14], %f26;
    st.global.f32 [%rd15], %f16;
    st.global.f32 [%rd16], %f19;
ADAM_DONE:
    ret;
}

// Adds a contiguous [cols] bias to every row of a row-major [rows,cols]
// matrix. Input and output may alias.
.visible .entry sura_bias_add_f32(
    .param .u64 param_in,
    .param .u64 param_bias,
    .param .u64 param_out,
    .param .u32 param_count,
    .param .u32 param_cols
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<9>;
    .reg .b64 %rd<10>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_bias];
    ld.param.u64 %rd3, [param_out];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_cols];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra BIAS_DONE;
    rem.u32 %r7, %r6, %r2;
    mul.wide.u32 %rd4, %r6, 4;
    mul.wide.u32 %rd5, %r7, 4;
    add.s64 %rd6, %rd1, %rd4;
    add.s64 %rd7, %rd2, %rd5;
    add.s64 %rd8, %rd3, %rd4;
    ld.global.f32 %f1, [%rd6];
    ld.global.f32 %f2, [%rd7];
    add.rn.f32 %f3, %f1, %f2;
    st.global.f32 [%rd8], %f3;
BIAS_DONE:
    ret;
}

// Reduces a contiguous [rows,cols] upstream gradient along rows for a
// broadcast [cols] bias.  One thread owns each output column, giving a
// deterministic row order without atomics.
.visible .entry sura_bias_gradient_f32(
    .param .u64 param_grad_out,
    .param .u64 param_grad_bias,
    .param .u32 param_rows,
    .param .u32 param_cols
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<9>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<3>;
    ld.param.u64 %rd1, [param_grad_out];
    ld.param.u64 %rd2, [param_grad_bias];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_cols];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r2;
    @%p1 bra BIAS_GRAD_DONE;
    mov.u32 %r7, 0;
    mov.f32 %f1, 0f00000000;
BIAS_GRAD_LOOP:
    setp.ge.u32 %p2, %r7, %r1;
    @%p2 bra BIAS_GRAD_STORE;
    mul.lo.u32 %r8, %r7, %r2;
    add.u32 %r8, %r8, %r6;
    mul.wide.u32 %rd3, %r8, 4;
    add.s64 %rd4, %rd1, %rd3;
    ld.global.f32 %f2, [%rd4];
    add.rn.f32 %f1, %f1, %f2;
    add.u32 %r7, %r7, 1;
    bra BIAS_GRAD_LOOP;
BIAS_GRAD_STORE:
    mul.wide.u32 %rd5, %r6, 4;
    add.s64 %rd6, %rd2, %rd5;
    st.global.f32 [%rd6], %f1;
BIAS_GRAD_DONE:
    ret;
}

.visible .entry sura_relu_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .u32 param_count
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra RELU_DONE;
    mul.wide.u32 %rd3, %r5, 4;
    add.s64 %rd4, %rd1, %rd3;
    add.s64 %rd5, %rd2, %rd3;
    ld.global.f32 %f1, [%rd4];
    mov.f32 %f2, 0f00000000;
    setp.gt.f32 %p2, %f1, %f2;
    selp.f32 %f3, %f1, %f2, %p2;
    st.global.f32 [%rd5], %f3;
RELU_DONE:
    ret;
}

.visible .entry sura_relu_backward_f32(
    .param .u64 param_in,
    .param .u64 param_grad_out,
    .param .u64 param_grad_in,
    .param .u32 param_count
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<10>;
    .reg .f32 %f<5>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_grad_out];
    ld.param.u64 %rd3, [param_grad_in];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra RELUB_DONE;
    mul.wide.u32 %rd4, %r5, 4;
    add.s64 %rd5, %rd1, %rd4;
    add.s64 %rd6, %rd2, %rd4;
    add.s64 %rd7, %rd3, %rd4;
    ld.global.f32 %f1, [%rd5];
    ld.global.f32 %f2, [%rd6];
    mov.f32 %f3, 0f00000000;
    setp.gt.f32 %p2, %f1, %f3;
    selp.f32 %f4, %f2, %f3, %p2;
    st.global.f32 [%rd7], %f4;
RELUB_DONE:
    ret;
}

// Exact-form GELU semantics, x * Phi(x), using a cancellation-safe erfc
// approximation. This is not the common tanh approximation. The polynomial
// is the Numerical Recipes erfc form evaluated in f32; exp is implemented
// through PTX ex2.approx because this backend intentionally has no libdevice.
.visible .entry sura_gelu_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .u32 param_count
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<18>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra GELU_DONE;
    mul.wide.u32 %rd3, %r5, 4;
    add.s64 %rd4, %rd1, %rd3;
    add.s64 %rd5, %rd2, %rd3;
    ld.global.f32 %f1, [%rd4];

    abs.f32 %f2, %f1;
    mul.rn.f32 %f2, %f2, 0f3F3504F3;
    mul.rn.f32 %f3, %f2, 0f3F000000;
    add.rn.f32 %f3, %f3, 0f3F800000;
    mov.f32 %f14, 0f3F800000;
    div.rn.f32 %f3, %f14, %f3;

    mov.f32 %f4, 0f3E2EF945;
    fma.rn.f32 %f4, %f4, %f3, 0fBF527892;
    fma.rn.f32 %f4, %f4, %f3, 0f3FBE87B0;
    fma.rn.f32 %f4, %f4, %f3, 0fBF914E5D;
    fma.rn.f32 %f4, %f4, %f3, 0f3E8EC7CC;
    fma.rn.f32 %f4, %f4, %f3, 0fBE3EC24C;
    fma.rn.f32 %f4, %f4, %f3, 0f3DC636C9;
    fma.rn.f32 %f4, %f4, %f3, 0f3EBF88FB;
    fma.rn.f32 %f4, %f4, %f3, 0f3F8000C7;

    neg.f32 %f5, %f2;
    fma.rn.f32 %f6, %f5, %f2, 0fBFA1FC4E;
    fma.rn.f32 %f6, %f3, %f4, %f6;
    mul.rn.f32 %f6, %f6, 0f3FB8AA3B;
    ex2.approx.f32 %f7, %f6;
    mul.rn.f32 %f8, %f3, %f7;
    mul.rn.f32 %f9, %f8, 0f3F000000;

    setp.lt.f32 %p2, %f1, 0f00000000;
    mul.rn.f32 %f10, %f1, %f9;
    sub.rn.f32 %f11, 0f3F800000, %f9;
    mul.rn.f32 %f12, %f1, %f11;
    selp.f32 %f13, %f10, %f12, %p2;
    st.global.f32 [%rd5], %f13;
GELU_DONE:
    ret;
}

.visible .entry sura_gelu_backward_f32(
    .param .u64 param_in,
    .param .u64 param_grad_out,
    .param .u64 param_grad_in,
    .param .u32 param_count
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<7>;
    .reg .b64 %rd<10>;
    .reg .f32 %f<22>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_grad_out];
    ld.param.u64 %rd3, [param_grad_in];
    ld.param.u32 %r1, [param_count];
    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4;
    setp.ge.u32 %p1, %r5, %r1;
    @%p1 bra GELUB_DONE;
    mul.wide.u32 %rd4, %r5, 4;
    add.s64 %rd5, %rd1, %rd4;
    add.s64 %rd6, %rd2, %rd4;
    add.s64 %rd7, %rd3, %rd4;
    ld.global.f32 %f1, [%rd5];
    ld.global.f32 %f2, [%rd6];

    abs.f32 %f3, %f1;
    mul.rn.f32 %f3, %f3, 0f3F3504F3;
    mul.rn.f32 %f4, %f3, 0f3F000000;
    add.rn.f32 %f4, %f4, 0f3F800000;
    mov.f32 %f17, 0f3F800000;
    div.rn.f32 %f4, %f17, %f4;

    mov.f32 %f5, 0f3E2EF945;
    fma.rn.f32 %f5, %f5, %f4, 0fBF527892;
    fma.rn.f32 %f5, %f5, %f4, 0f3FBE87B0;
    fma.rn.f32 %f5, %f5, %f4, 0fBF914E5D;
    fma.rn.f32 %f5, %f5, %f4, 0f3E8EC7CC;
    fma.rn.f32 %f5, %f5, %f4, 0fBE3EC24C;
    fma.rn.f32 %f5, %f5, %f4, 0f3DC636C9;
    fma.rn.f32 %f5, %f5, %f4, 0f3EBF88FB;
    fma.rn.f32 %f5, %f5, %f4, 0f3F8000C7;

    neg.f32 %f6, %f3;
    fma.rn.f32 %f7, %f6, %f3, 0fBFA1FC4E;
    fma.rn.f32 %f7, %f4, %f5, %f7;
    mul.rn.f32 %f7, %f7, 0f3FB8AA3B;
    ex2.approx.f32 %f8, %f7;
    mul.rn.f32 %f9, %f4, %f8;
    mul.rn.f32 %f10, %f9, 0f3F000000;

    mul.rn.f32 %f11, %f1, %f1;
    mul.rn.f32 %f11, %f11, 0fBF000000;
    mul.rn.f32 %f11, %f11, 0f3FB8AA3B;
    ex2.approx.f32 %f12, %f11;
    mul.rn.f32 %f12, %f12, 0f3ECC422A;
    sub.rn.f32 %f13, 0f3F800000, %f10;
    setp.lt.f32 %p2, %f1, 0f00000000;
    selp.f32 %f14, %f10, %f13, %p2;
    fma.rn.f32 %f15, %f1, %f12, %f14;
    mul.rn.f32 %f16, %f2, %f15;
    st.global.f32 [%rd7], %f16;
GELUB_DONE:
    ret;
}

// Correctness-first final-axis LayerNorm. One CUDA thread owns one complete
// row, so row statistics are deterministic. Values are scaled by max(abs(x),
// sqrt(epsilon)) before computing moments to avoid squaring large f32 inputs.
.visible .entry sura_layer_norm_f32_scaled_reference(
    .param .u64 param_in,
    .param .u64 param_weight,
    .param .u64 param_bias,
    .param .u64 param_out,
    .param .u64 param_saved_mean,
    .param .u64 param_saved_rstd,
    .param .u32 param_rows,
    .param .u32 param_features,
    .param .f32 param_epsilon,
    .param .u32 param_has_weight,
    .param .u32 param_has_bias,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<7>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<12>;
    .reg .f32 %f<24>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_weight];
    ld.param.u64 %rd3, [param_bias];
    ld.param.u64 %rd4, [param_out];
    ld.param.u64 %rd5, [param_saved_mean];
    ld.param.u64 %rd6, [param_saved_rstd];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_features];
    ld.param.f32 %f22, [param_epsilon];
    ld.param.u32 %r3, [param_has_weight];
    ld.param.u32 %r4, [param_has_bias];
    ld.param.u32 %r5, [param_save_stats];
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    mad.lo.s32 %r9, %r6, %r7, %r8;
    setp.ge.u32 %p1, %r9, %r1;
    @%p1 bra LN_DONE;
    mul.lo.u32 %r10, %r9, %r2;

    mov.f32 %f1, 0f00000000;
    mov.u32 %r11, 0;
LN_MAX_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LN_MAX_READY;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    abs.f32 %f3, %f2;
    max.f32 %f1, %f1, %f3;
    add.u32 %r11, %r11, 1;
    bra LN_MAX_LOOP;
LN_MAX_READY:
    sqrt.rn.f32 %f4, %f22;
    max.f32 %f5, %f1, %f4;

    mov.f32 %f6, 0f00000000;
    mov.u32 %r11, 0;
    mov.u32 %r13, 0;
LN_MEAN_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LN_MEAN_READY;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    div.rn.f32 %f7, %f2, %f5;
    add.u32 %r13, %r13, 1;
    cvt.rn.f32.u32 %f8, %r13;
    sub.rn.f32 %f9, %f7, %f6;
    div.rn.f32 %f9, %f9, %f8;
    add.rn.f32 %f6, %f6, %f9;
    add.u32 %r11, %r11, 1;
    bra LN_MEAN_LOOP;
LN_MEAN_READY:
    cvt.rn.f32.u32 %f12, %r2;
    mov.f32 %f10, 0f00000000;
    mov.u32 %r11, 0;
LN_VAR_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LN_VAR_READY;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    div.rn.f32 %f7, %f2, %f5;
    sub.rn.f32 %f11, %f7, %f6;
    fma.rn.f32 %f10, %f11, %f11, %f10;
    add.u32 %r11, %r11, 1;
    bra LN_VAR_LOOP;
LN_VAR_READY:
    div.rn.f32 %f13, %f10, %f12;
    mul.rn.f32 %f14, %f6, %f5;
    setp.gt.f32 %p2, %f13, 0f00000000;
    @!%p2 bra LN_CONSTANT_ROW;
    div.rn.f32 %f15, %f22, %f5;
    div.rn.f32 %f15, %f15, %f5;
    add.rn.f32 %f15, %f13, %f15;
    sqrt.rn.f32 %f15, %f15;
    mov.f32 %f23, 0f3F800000;
    div.rn.f32 %f16, %f23, %f15;
    div.rn.f32 %f17, %f16, %f5;
    bra LN_STATS_READY;
LN_CONSTANT_ROW:
    // Preserve an exactly constant row's mean bit-for-bit so backward cannot
    // manufacture a tiny normalized value through divide/multiply rounding.
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f14, [%rd8];
    sqrt.rn.f32 %f15, %f22;
    mov.f32 %f23, 0f3F800000;
    div.rn.f32 %f17, %f23, %f15;
    mov.f32 %f16, 0f00000000;
LN_STATS_READY:
    setp.ne.u32 %p3, %r5, 0;
    @!%p3 bra LN_SKIP_SAVE;
    mul.wide.u32 %rd9, %r9, 4;
    add.s64 %rd10, %rd5, %rd9;
    add.s64 %rd11, %rd6, %rd9;
    st.global.f32 [%rd10], %f14;
    st.global.f32 [%rd11], %f17;
LN_SKIP_SAVE:
    setp.ne.u32 %p4, %r3, 0;
    setp.ne.u32 %p5, %r4, 0;
    mov.u32 %r11, 0;
LN_OUTPUT_LOOP:
    setp.ge.u32 %p3, %r11, %r2;
    @%p3 bra LN_DONE;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    @!%p2 bra LN_OUTPUT_ZERO;
    div.rn.f32 %f7, %f2, %f5;
    sub.rn.f32 %f18, %f7, %f6;
    mul.rn.f32 %f18, %f18, %f16;
    bra LN_OUTPUT_NORMALIZED;
LN_OUTPUT_ZERO:
    mov.f32 %f18, 0f00000000;
LN_OUTPUT_NORMALIZED:
    mov.f32 %f20, %f18;
    @!%p4 bra LN_OUTPUT_NO_WEIGHT;
    mul.wide.u32 %rd9, %r11, 4;
    add.s64 %rd10, %rd2, %rd9;
    ld.global.f32 %f19, [%rd10];
    mul.rn.f32 %f20, %f20, %f19;
LN_OUTPUT_NO_WEIGHT:
    @!%p5 bra LN_OUTPUT_NO_BIAS;
    mul.wide.u32 %rd9, %r11, 4;
    add.s64 %rd10, %rd3, %rd9;
    ld.global.f32 %f21, [%rd10];
    add.rn.f32 %f20, %f20, %f21;
LN_OUTPUT_NO_BIAS:
    add.s64 %rd8, %rd4, %rd7;
    st.global.f32 [%rd8], %f20;
    add.u32 %r11, %r11, 1;
    bra LN_OUTPUT_LOOP;
LN_DONE:
    ret;
}

// Public LayerNorm forward reference kernel. Accumulation is f64 even though
// public storage remains f32. Every row is centered on its first (exact f32)
// value before summation, so a large common offset cannot erase small but
// representable differences. Consumer GPUs execute this slowly; a future
// warp-Welford kernel can replace it behind the same ABI.
.visible .entry sura_layer_norm_f32(
    .param .u64 param_in,
    .param .u64 param_weight,
    .param .u64 param_bias,
    .param .u64 param_out,
    .param .u64 param_saved_mean,
    .param .u64 param_saved_rstd,
    .param .u32 param_rows,
    .param .u32 param_features,
    .param .f32 param_epsilon,
    .param .u32 param_has_weight,
    .param .u32 param_has_bias,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<6>;
    .reg .b32 %r<15>;
    .reg .b64 %rd<12>;
    .reg .f32 %f<8>;
    .reg .f64 %fd<20>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_weight];
    ld.param.u64 %rd3, [param_bias];
    ld.param.u64 %rd4, [param_out];
    ld.param.u64 %rd5, [param_saved_mean];
    ld.param.u64 %rd6, [param_saved_rstd];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_features];
    ld.param.f32 %f1, [param_epsilon];
    ld.param.u32 %r3, [param_has_weight];
    ld.param.u32 %r4, [param_has_bias];
    ld.param.u32 %r5, [param_save_stats];
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    mad.lo.s32 %r9, %r6, %r7, %r8;
    setp.ge.u32 %p1, %r9, %r1;
    @%p1 bra LNF64_DONE;
    mul.lo.u32 %r10, %r9, %r2;

    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd19, %f2;

    mov.f64 %fd1, 0d0000000000000000;
    mov.f64 %fd2, 0d0000000000000000;
    mov.u32 %r11, 0;
LNF64_SUM_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LNF64_SUM_READY;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd3, %f2;
    sub.rn.f64 %fd3, %fd3, %fd19;
    sub.rn.f64 %fd4, %fd3, %fd2;
    add.rn.f64 %fd5, %fd1, %fd4;
    sub.rn.f64 %fd6, %fd5, %fd1;
    sub.rn.f64 %fd2, %fd6, %fd4;
    mov.f64 %fd1, %fd5;
    add.u32 %r11, %r11, 1;
    bra LNF64_SUM_LOOP;
LNF64_SUM_READY:
    cvt.rn.f64.u32 %fd7, %r2;
    div.rn.f64 %fd8, %fd1, %fd7;

    mov.f64 %fd9, 0d0000000000000000;
    mov.f64 %fd10, 0d0000000000000000;
    mov.u32 %r11, 0;
LNF64_VAR_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LNF64_VAR_READY;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd3, %f2;
    sub.rn.f64 %fd11, %fd3, %fd19;
    sub.rn.f64 %fd11, %fd11, %fd8;
    mul.rn.f64 %fd11, %fd11, %fd11;
    sub.rn.f64 %fd4, %fd11, %fd10;
    add.rn.f64 %fd5, %fd9, %fd4;
    sub.rn.f64 %fd6, %fd5, %fd9;
    sub.rn.f64 %fd10, %fd6, %fd4;
    mov.f64 %fd9, %fd5;
    add.u32 %r11, %r11, 1;
    bra LNF64_VAR_LOOP;
LNF64_VAR_READY:
    div.rn.f64 %fd12, %fd9, %fd7;
    cvt.f64.f32 %fd13, %f1;
    add.rn.f64 %fd12, %fd12, %fd13;
    sqrt.rn.f64 %fd12, %fd12;
    mov.f64 %fd14, 0d3FF0000000000000;
    div.rn.f64 %fd15, %fd14, %fd12;

    setp.ne.u32 %p3, %r5, 0;
    @!%p3 bra LNF64_SKIP_SAVE;
    add.rn.f64 %fd3, %fd19, %fd8;
    cvt.rn.f32.f64 %f3, %fd3;
    cvt.rn.f32.f64 %f4, %fd15;
    mul.wide.u32 %rd9, %r9, 4;
    add.s64 %rd10, %rd5, %rd9;
    add.s64 %rd11, %rd6, %rd9;
    st.global.f32 [%rd10], %f3;
    st.global.f32 [%rd11], %f4;
LNF64_SKIP_SAVE:
    setp.ne.u32 %p4, %r3, 0;
    setp.ne.u32 %p5, %r4, 0;
    mov.u32 %r11, 0;
LNF64_OUTPUT_LOOP:
    setp.ge.u32 %p2, %r11, %r2;
    @%p2 bra LNF64_DONE;
    add.u32 %r12, %r10, %r11;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd3, %f2;
    sub.rn.f64 %fd16, %fd3, %fd19;
    sub.rn.f64 %fd16, %fd16, %fd8;
    mul.rn.f64 %fd16, %fd16, %fd15;
    @!%p4 bra LNF64_NO_WEIGHT;
    mul.wide.u32 %rd9, %r11, 4;
    add.s64 %rd10, %rd2, %rd9;
    ld.global.f32 %f5, [%rd10];
    cvt.f64.f32 %fd17, %f5;
    mul.rn.f64 %fd16, %fd16, %fd17;
LNF64_NO_WEIGHT:
    @!%p5 bra LNF64_NO_BIAS;
    mul.wide.u32 %rd9, %r11, 4;
    add.s64 %rd10, %rd3, %rd9;
    ld.global.f32 %f6, [%rd10];
    cvt.f64.f32 %fd18, %f6;
    add.rn.f64 %fd16, %fd16, %fd18;
LNF64_NO_BIAS:
    cvt.rn.f32.f64 %f7, %fd16;
    add.s64 %rd8, %rd4, %rd7;
    st.global.f32 [%rd8], %f7;
    add.u32 %r11, %r11, 1;
    bra LNF64_OUTPUT_LOOP;
LNF64_DONE:
    ret;
}

.visible .entry sura_layer_norm_backward_f32(
    .param .u64 param_in,
    .param .u64 param_weight,
    .param .u64 param_grad_out,
    .param .u64 param_saved_mean,
    .param .u64 param_saved_rstd,
    .param .u64 param_grad_in,
    .param .u32 param_rows,
    .param .u32 param_features,
    .param .f32 param_epsilon,
    .param .u32 param_has_weight
)
{
    .reg .pred %p<5>;
    .reg .b32 %r<17>;
    .reg .b64 %rd<13>;
    .reg .f32 %f<10>;
    .reg .f64 %fd<24>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_weight];
    ld.param.u64 %rd3, [param_grad_out];
    ld.param.u64 %rd4, [param_saved_mean];
    ld.param.u64 %rd5, [param_saved_rstd];
    ld.param.u64 %rd6, [param_grad_in];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_features];
    ld.param.f32 %f1, [param_epsilon];
    ld.param.u32 %r3, [param_has_weight];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r1;
    @%p1 bra LNB_DONE;
    mul.lo.u32 %r8, %r7, %r2;

    // Forward saves rounded f32 diagnostics for graph state, but an f64 mean
    // need not be representable as f32 (for example, 16777217). Recompute the
    // exact row statistics from the version-checked input before differentiating.
    mul.wide.u32 %rd7, %r8, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd1, %f2;
    mov.f64 %fd2, 0d0000000000000000;
    mov.f64 %fd3, 0d0000000000000000;
    mov.u32 %r9, 0;
LNB_STAT_SUM_LOOP:
    setp.ge.u32 %p3, %r9, %r2;
    @%p3 bra LNB_STAT_SUM_READY;
    add.u32 %r10, %r8, %r9;
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd4, %f2;
    sub.rn.f64 %fd4, %fd4, %fd1;
    sub.rn.f64 %fd5, %fd4, %fd3;
    add.rn.f64 %fd6, %fd2, %fd5;
    sub.rn.f64 %fd7, %fd6, %fd2;
    sub.rn.f64 %fd3, %fd7, %fd5;
    mov.f64 %fd2, %fd6;
    add.u32 %r9, %r9, 1;
    bra LNB_STAT_SUM_LOOP;
LNB_STAT_SUM_READY:
    cvt.rn.f64.u32 %fd8, %r2;
    div.rn.f64 %fd9, %fd2, %fd8;
    mov.f64 %fd10, 0d0000000000000000;
    mov.f64 %fd11, 0d0000000000000000;
    mov.u32 %r9, 0;
LNB_STAT_VAR_LOOP:
    setp.ge.u32 %p3, %r9, %r2;
    @%p3 bra LNB_STAT_VAR_READY;
    add.u32 %r10, %r8, %r9;
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    cvt.f64.f32 %fd12, %f2;
    sub.rn.f64 %fd12, %fd12, %fd1;
    sub.rn.f64 %fd12, %fd12, %fd9;
    mul.rn.f64 %fd12, %fd12, %fd12;
    sub.rn.f64 %fd5, %fd12, %fd11;
    add.rn.f64 %fd6, %fd10, %fd5;
    sub.rn.f64 %fd7, %fd6, %fd10;
    sub.rn.f64 %fd11, %fd7, %fd5;
    mov.f64 %fd10, %fd6;
    add.u32 %r9, %r9, 1;
    bra LNB_STAT_VAR_LOOP;
LNB_STAT_VAR_READY:
    div.rn.f64 %fd13, %fd10, %fd8;
    cvt.f64.f32 %fd14, %f1;
    add.rn.f64 %fd13, %fd13, %fd14;
    sqrt.rn.f64 %fd13, %fd13;
    mov.f64 %fd16, 0d3FF0000000000000;
    div.rn.f64 %fd15, %fd16, %fd13;

    setp.ne.u32 %p2, %r3, 0;
    mov.f64 %fd17, 0d0000000000000000;
    mov.f64 %fd18, 0d0000000000000000;
    mov.u32 %r9, 0;
LNB_SUM_LOOP:
    setp.ge.u32 %p3, %r9, %r2;
    @%p3 bra LNB_SUM_READY;
    add.u32 %r10, %r8, %r9;
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    add.s64 %rd9, %rd3, %rd7;
    ld.global.f32 %f2, [%rd8];
    ld.global.f32 %f3, [%rd9];
    cvt.f64.f32 %fd19, %f3;
    @!%p2 bra LNB_G_READY;
    mul.wide.u32 %rd10, %r9, 4;
    add.s64 %rd11, %rd2, %rd10;
    ld.global.f32 %f4, [%rd11];
    cvt.f64.f32 %fd4, %f4;
    mul.rn.f64 %fd19, %fd19, %fd4;
LNB_G_READY:
    cvt.f64.f32 %fd20, %f2;
    sub.rn.f64 %fd20, %fd20, %fd1;
    sub.rn.f64 %fd20, %fd20, %fd9;
    mul.rn.f64 %fd20, %fd20, %fd15;
    add.rn.f64 %fd17, %fd17, %fd19;
    fma.rn.f64 %fd18, %fd19, %fd20, %fd18;
    add.u32 %r9, %r9, 1;
    bra LNB_SUM_LOOP;
LNB_SUM_READY:
    div.rn.f64 %fd21, %fd17, %fd8;
    div.rn.f64 %fd22, %fd18, %fd8;
    mov.u32 %r9, 0;
LNB_OUTPUT_LOOP:
    setp.ge.u32 %p3, %r9, %r2;
    @%p3 bra LNB_DONE;
    add.u32 %r10, %r8, %r9;
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd8, %rd1, %rd7;
    add.s64 %rd9, %rd3, %rd7;
    ld.global.f32 %f2, [%rd8];
    ld.global.f32 %f3, [%rd9];
    cvt.f64.f32 %fd19, %f3;
    @!%p2 bra LNB_OUTPUT_G_READY;
    mul.wide.u32 %rd10, %r9, 4;
    add.s64 %rd11, %rd2, %rd10;
    ld.global.f32 %f4, [%rd11];
    cvt.f64.f32 %fd4, %f4;
    mul.rn.f64 %fd19, %fd19, %fd4;
LNB_OUTPUT_G_READY:
    cvt.f64.f32 %fd20, %f2;
    sub.rn.f64 %fd20, %fd20, %fd1;
    sub.rn.f64 %fd20, %fd20, %fd9;
    mul.rn.f64 %fd20, %fd20, %fd15;
    sub.rn.f64 %fd23, %fd19, %fd21;
    neg.f64 %fd12, %fd22;
    fma.rn.f64 %fd23, %fd20, %fd12, %fd23;
    mul.rn.f64 %fd23, %fd23, %fd15;
    cvt.rn.f32.f64 %f5, %fd23;
    add.s64 %rd12, %rd6, %rd7;
    st.global.f32 [%rd12], %f5;
    add.u32 %r9, %r9, 1;
    bra LNB_OUTPUT_LOOP;
LNB_DONE:
    ret;
}

.visible .entry sura_layer_norm_parameter_backward_f32(
    .param .u64 param_in,
    .param .u64 param_grad_out,
    .param .u64 param_saved_mean,
    .param .u64 param_saved_rstd,
    .param .u64 param_grad_weight,
    .param .u64 param_grad_bias,
    .param .u32 param_rows,
    .param .u32 param_features,
    .param .f32 param_epsilon,
    .param .u32 param_write_weight,
    .param .u32 param_write_bias
)
{
    .reg .pred %p<6>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<7>;
    .reg .f64 %fd<21>;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_grad_out];
    ld.param.u64 %rd3, [param_saved_mean];
    ld.param.u64 %rd4, [param_saved_rstd];
    ld.param.u64 %rd5, [param_grad_weight];
    ld.param.u64 %rd6, [param_grad_bias];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_features];
    ld.param.f32 %f1, [param_epsilon];
    ld.param.u32 %r3, [param_write_weight];
    ld.param.u32 %r4, [param_write_bias];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r2;
    @%p1 bra LNP_DONE;
    setp.ne.u32 %p3, %r3, 0;
    mov.f64 %fd1, 0d0000000000000000;
    mov.f64 %fd2, 0d0000000000000000;
    mov.u32 %r9, 0;
LNP_ROW_LOOP:
    setp.ge.u32 %p2, %r9, %r1;
    @%p2 bra LNP_STORE;
    mad.lo.u32 %r10, %r9, %r2, %r8;
    mul.wide.u32 %rd7, %r10, 4;
    add.s64 %rd9, %rd2, %rd7;
    ld.global.f32 %f2, [%rd9];
    cvt.f64.f32 %fd3, %f2;
    add.rn.f64 %fd2, %fd2, %fd3;
    @!%p3 bra LNP_ROW_NEXT;

    // Each feature thread recomputes a row's anchor-centered f64 statistics.
    // This is deliberately correctness-first: the rounded saved f32 mean is
    // diagnostic graph state and cannot represent every exact f32-row mean.
    mul.lo.u32 %r11, %r9, %r2;
    mul.wide.u32 %rd10, %r11, 4;
    add.s64 %rd11, %rd1, %rd10;
    ld.global.f32 %f3, [%rd11];
    cvt.f64.f32 %fd4, %f3;
    mov.f64 %fd5, 0d0000000000000000;
    mov.f64 %fd6, 0d0000000000000000;
    mov.u32 %r12, 0;
LNP_STAT_SUM_LOOP:
    setp.ge.u32 %p4, %r12, %r2;
    @%p4 bra LNP_STAT_SUM_READY;
    add.u32 %r13, %r11, %r12;
    mul.wide.u32 %rd10, %r13, 4;
    add.s64 %rd11, %rd1, %rd10;
    ld.global.f32 %f3, [%rd11];
    cvt.f64.f32 %fd7, %f3;
    sub.rn.f64 %fd7, %fd7, %fd4;
    sub.rn.f64 %fd8, %fd7, %fd6;
    add.rn.f64 %fd9, %fd5, %fd8;
    sub.rn.f64 %fd10, %fd9, %fd5;
    sub.rn.f64 %fd6, %fd10, %fd8;
    mov.f64 %fd5, %fd9;
    add.u32 %r12, %r12, 1;
    bra LNP_STAT_SUM_LOOP;
LNP_STAT_SUM_READY:
    cvt.rn.f64.u32 %fd11, %r2;
    div.rn.f64 %fd12, %fd5, %fd11;
    mov.f64 %fd13, 0d0000000000000000;
    mov.f64 %fd14, 0d0000000000000000;
    mov.u32 %r12, 0;
LNP_STAT_VAR_LOOP:
    setp.ge.u32 %p4, %r12, %r2;
    @%p4 bra LNP_STAT_VAR_READY;
    add.u32 %r13, %r11, %r12;
    mul.wide.u32 %rd10, %r13, 4;
    add.s64 %rd11, %rd1, %rd10;
    ld.global.f32 %f3, [%rd11];
    cvt.f64.f32 %fd15, %f3;
    sub.rn.f64 %fd15, %fd15, %fd4;
    sub.rn.f64 %fd15, %fd15, %fd12;
    mul.rn.f64 %fd15, %fd15, %fd15;
    sub.rn.f64 %fd8, %fd15, %fd14;
    add.rn.f64 %fd9, %fd13, %fd8;
    sub.rn.f64 %fd10, %fd9, %fd13;
    sub.rn.f64 %fd14, %fd10, %fd8;
    mov.f64 %fd13, %fd9;
    add.u32 %r12, %r12, 1;
    bra LNP_STAT_VAR_LOOP;
LNP_STAT_VAR_READY:
    div.rn.f64 %fd16, %fd13, %fd11;
    cvt.f64.f32 %fd17, %f1;
    add.rn.f64 %fd16, %fd16, %fd17;
    sqrt.rn.f64 %fd16, %fd16;
    mov.f64 %fd18, 0d3FF0000000000000;
    div.rn.f64 %fd19, %fd18, %fd16;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f3, [%rd8];
    cvt.f64.f32 %fd20, %f3;
    sub.rn.f64 %fd20, %fd20, %fd4;
    sub.rn.f64 %fd20, %fd20, %fd12;
    mul.rn.f64 %fd20, %fd20, %fd19;
    fma.rn.f64 %fd1, %fd3, %fd20, %fd1;
LNP_ROW_NEXT:
    add.u32 %r9, %r9, 1;
    bra LNP_ROW_LOOP;
LNP_STORE:
    cvt.rn.f32.f64 %f4, %fd1;
    cvt.rn.f32.f64 %f5, %fd2;
    mul.wide.u32 %rd7, %r8, 4;
    @!%p3 bra LNP_SKIP_WEIGHT;
    add.s64 %rd8, %rd5, %rd7;
    st.global.f32 [%rd8], %f4;
LNP_SKIP_WEIGHT:
    setp.ne.u32 %p5, %r4, 0;
    @!%p5 bra LNP_DONE;
    add.s64 %rd9, %rd6, %rd7;
    st.global.f32 [%rd9], %f5;
LNP_DONE:
    ret;
}

.visible .entry sura_embedding_f32_float_reference(
    .param .u64 param_weight,
    .param .u64 param_ids,
    .param .u64 param_out,
    .param .u32 param_count,
    .param .u32 param_dimensions
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<13>;
    .reg .b64 %rd<12>;
    .reg .f32 %f<3>;
    ld.param.u64 %rd1, [param_weight];
    ld.param.u64 %rd2, [param_ids];
    ld.param.u64 %rd3, [param_out];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_dimensions];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra EMB_DONE;
    div.u32 %r7, %r6, %r2;
    rem.u32 %r8, %r6, %r2;
    mul.wide.u32 %rd4, %r7, 4;
    add.s64 %rd5, %rd2, %rd4;
    ld.global.f32 %f1, [%rd5];
    cvt.rzi.u32.f32 %r9, %f1;
    mad.lo.u32 %r10, %r9, %r2, %r8;
    mul.wide.u32 %rd6, %r10, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f2, [%rd7];
    mul.wide.u32 %rd8, %r6, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f2;
EMB_DONE:
    ret;
}

.visible .entry sura_embedding_backward_f32_float_reference(
    .param .u64 param_ids,
    .param .u64 param_grad_out,
    .param .u64 param_grad_weight,
    .param .u32 param_count,
    .param .u32 param_dimensions
)
{
    .reg .pred %p<2>;
    .reg .b32 %r<13>;
    .reg .b64 %rd<12>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_ids];
    ld.param.u64 %rd2, [param_grad_out];
    ld.param.u64 %rd3, [param_grad_weight];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_dimensions];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra EMBB_DONE;
    div.u32 %r7, %r6, %r2;
    rem.u32 %r8, %r6, %r2;
    mul.wide.u32 %rd4, %r7, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f1, [%rd5];
    cvt.rzi.u32.f32 %r9, %f1;
    mad.lo.u32 %r10, %r9, %r2, %r8;
    mul.wide.u32 %rd6, %r10, 4;
    add.s64 %rd7, %rd3, %rd6;
    mul.wide.u32 %rd8, %r6, 4;
    add.s64 %rd9, %rd2, %rd8;
    ld.global.f32 %f2, [%rd9];
    atom.global.add.f32 %f3, [%rd7], %f2;
EMBB_DONE:
    ret;
}

// Public embedding kernels use host-validated packed uint32 token ids. The
// backward kernel assigns one thread per embedding dimension, so repeated ids
// accumulate in token order without atomics or races.
.visible .entry sura_embedding_f32(
    .param .u64 param_weight,
    .param .u64 param_ids,
    .param .u64 param_out,
    .param .u32 param_count,
    .param .u32 param_vocabulary,
    .param .u32 param_dimensions
)
{
    .reg .pred %p<3>;
    .reg .b32 %r<14>;
    .reg .b64 %rd<12>;
    .reg .f32 %f<3>;
    ld.param.u64 %rd1, [param_weight];
    ld.param.u64 %rd2, [param_ids];
    ld.param.u64 %rd3, [param_out];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_vocabulary];
    ld.param.u32 %r3, [param_dimensions];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r1;
    @%p1 bra EMBU_DONE;
    div.u32 %r8, %r7, %r3;
    rem.u32 %r9, %r7, %r3;
    mul.wide.u32 %rd4, %r8, 4;
    add.s64 %rd5, %rd2, %rd4;
    ld.global.u32 %r10, [%rd5];
    mul.wide.u32 %rd8, %r7, 4;
    add.s64 %rd9, %rd3, %rd8;
    setp.ge.u32 %p2, %r10, %r2;
    @%p2 bra EMBU_INVALID;
    mad.lo.u32 %r11, %r10, %r3, %r9;
    mul.wide.u32 %rd6, %r11, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f1, [%rd7];
    st.global.f32 [%rd9], %f1;
    bra EMBU_DONE;
EMBU_INVALID:
    mov.f32 %f2, 0f00000000;
    st.global.f32 [%rd9], %f2;
EMBU_DONE:
    ret;
}

.visible .entry sura_embedding_backward_f32(
    .param .u64 param_ids,
    .param .u64 param_grad_out,
    .param .u64 param_grad_weight,
    .param .u32 param_vocabulary,
    .param .u32 param_tokens,
    .param .u32 param_dimensions
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<4>;
    ld.param.u64 %rd1, [param_ids];
    ld.param.u64 %rd2, [param_grad_out];
    ld.param.u64 %rd3, [param_grad_weight];
    ld.param.u32 %r1, [param_vocabulary];
    ld.param.u32 %r2, [param_tokens];
    ld.param.u32 %r3, [param_dimensions];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r3;
    @%p1 bra EMBUB_DONE;

    mov.u32 %r8, 0;
    mov.f32 %f1, 0f00000000;
EMBUB_ZERO_LOOP:
    setp.ge.u32 %p2, %r8, %r1;
    @%p2 bra EMBUB_TOKEN_START;
    mad.lo.u32 %r9, %r8, %r3, %r7;
    mul.wide.u32 %rd4, %r9, 4;
    add.s64 %rd5, %rd3, %rd4;
    st.global.f32 [%rd5], %f1;
    add.u32 %r8, %r8, 1;
    bra EMBUB_ZERO_LOOP;

EMBUB_TOKEN_START:
    mov.u32 %r8, 0;
EMBUB_TOKEN_LOOP:
    setp.ge.u32 %p2, %r8, %r2;
    @%p2 bra EMBUB_DONE;
    mul.wide.u32 %rd4, %r8, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.u32 %r10, [%rd5];
    setp.ge.u32 %p3, %r10, %r1;
    @%p3 bra EMBUB_NEXT_TOKEN;
    mad.lo.u32 %r11, %r10, %r3, %r7;
    mad.lo.u32 %r12, %r8, %r3, %r7;
    mul.wide.u32 %rd6, %r11, 4;
    mul.wide.u32 %rd7, %r12, 4;
    add.s64 %rd8, %rd3, %rd6;
    add.s64 %rd9, %rd2, %rd7;
    ld.global.f32 %f2, [%rd8];
    ld.global.f32 %f3, [%rd9];
    add.rn.f32 %f2, %f2, %f3;
    st.global.f32 [%rd8], %f2;
EMBUB_NEXT_TOKEN:
    add.u32 %r8, %r8, 1;
    bra EMBUB_TOKEN_LOOP;
EMBUB_DONE:
    ret;
}

// Sparse-ID mean cross entropy. The forward reference kernel is deliberately
// single-threaded for deterministic stable log-sum-exp; backward parallelizes
// over logits using saved row maxima/inverse sums.
.visible .entry sura_cross_entropy_ids_f32_serial_reference(
    .param .u64 param_logits,
    .param .u64 param_ids,
    .param .u64 param_loss,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u32 param_rows,
    .param .u32 param_classes,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<6>;
    .reg .b32 %r<18>;
    .reg .b64 %rd<16>;
    .reg .f32 %f<14>;
    .reg .f64 %fd<8>;
    mov.u32 %r1, %ctaid.x;
    setp.ne.u32 %p1, %r1, 0;
    @%p1 bra CEI_DONE;
    mov.u32 %r2, %tid.x;
    setp.ne.u32 %p2, %r2, 0;
    @%p2 bra CEI_DONE;
    ld.param.u64 %rd1, [param_logits];
    ld.param.u64 %rd2, [param_ids];
    ld.param.u64 %rd3, [param_loss];
    ld.param.u64 %rd4, [param_saved_max];
    ld.param.u64 %rd5, [param_saved_inv_sum];
    ld.param.u32 %r3, [param_rows];
    ld.param.u32 %r4, [param_classes];
    ld.param.u32 %r5, [param_save_stats];
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r6, 0;
CEI_ROW_LOOP:
    setp.ge.u32 %p3, %r6, %r3;
    @%p3 bra CEI_STORE;
    mul.lo.u32 %r7, %r6, %r4;
    mul.wide.u32 %rd6, %r7, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f1, [%rd7];
    mov.u32 %r8, 1;
CEI_MAX_LOOP:
    setp.ge.u32 %p4, %r8, %r4;
    @%p4 bra CEI_MAX_READY;
    add.u32 %r9, %r7, %r8;
    mul.wide.u32 %rd8, %r9, 4;
    add.s64 %rd9, %rd1, %rd8;
    ld.global.f32 %f2, [%rd9];
    max.f32 %f1, %f1, %f2;
    add.u32 %r8, %r8, 1;
    bra CEI_MAX_LOOP;
CEI_MAX_READY:
    mov.f32 %f3, 0f00000000;
    mov.u32 %r8, 0;
CEI_EXP_LOOP:
    setp.ge.u32 %p4, %r8, %r4;
    @%p4 bra CEI_EXP_READY;
    add.u32 %r9, %r7, %r8;
    mul.wide.u32 %rd8, %r9, 4;
    add.s64 %rd9, %rd1, %rd8;
    ld.global.f32 %f2, [%rd9];
    sub.rn.f32 %f4, %f2, %f1;
    mul.rn.f32 %f4, %f4, 0f3FB8AA3B;
    ex2.approx.f32 %f5, %f4;
    add.rn.f32 %f3, %f3, %f5;
    add.u32 %r8, %r8, 1;
    bra CEI_EXP_LOOP;
CEI_EXP_READY:
    mov.f32 %f6, 0f3F800000;
    div.rn.f32 %f6, %f6, %f3;
    lg2.approx.f32 %f7, %f3;
    mul.rn.f32 %f7, %f7, 0f3F317218;
    mul.wide.u32 %rd8, %r6, 4;
    add.s64 %rd9, %rd2, %rd8;
    ld.global.u32 %r10, [%rd9];
    sub.u32 %r11, %r4, 1;
    min.u32 %r10, %r10, %r11;
    add.u32 %r12, %r7, %r10;
    mul.wide.u32 %rd10, %r12, 4;
    add.s64 %rd11, %rd1, %rd10;
    ld.global.f32 %f8, [%rd11];
    cvt.f64.f32 %fd2, %f1;
    cvt.f64.f32 %fd3, %f8;
    sub.rn.f64 %fd4, %fd2, %fd3;
    cvt.f64.f32 %fd5, %f7;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.rn.f64 %fd1, %fd1, %fd4;
    setp.ne.u32 %p5, %r5, 0;
    @!%p5 bra CEI_NEXT_ROW;
    add.s64 %rd12, %rd4, %rd8;
    add.s64 %rd13, %rd5, %rd8;
    st.global.f32 [%rd12], %f1;
    st.global.f32 [%rd13], %f6;
CEI_NEXT_ROW:
    add.u32 %r6, %r6, 1;
    bra CEI_ROW_LOOP;
CEI_STORE:
    cvt.rn.f64.u32 %fd6, %r3;
    div.rn.f64 %fd1, %fd1, %fd6;
    cvt.rn.f32.f64 %f9, %fd1;
    st.global.f32 [%rd3], %f9;
CEI_DONE:
    ret;
}

.visible .entry sura_cross_entropy_ids_stats_f32(
    .param .u64 param_logits,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u32 param_rows,
    .param .u32 param_classes
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<15>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<8>;
    .reg .f64 %fd<5>;
    ld.param.u64 %rd1, [param_logits];
    ld.param.u64 %rd2, [param_saved_max];
    ld.param.u64 %rd3, [param_saved_inv_sum];
    ld.param.u32 %r1, [param_rows];
    ld.param.u32 %r2, [param_classes];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.s32 %r6, %r3, %r4, %r5;
    setp.ge.u32 %p1, %r6, %r1;
    @%p1 bra CEIS_DONE;
    mul.lo.u32 %r7, %r6, %r2;
    mul.wide.u32 %rd4, %r7, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f1, [%rd5];
    mov.u32 %r8, 1;
CEIS_MAX_LOOP:
    setp.ge.u32 %p2, %r8, %r2;
    @%p2 bra CEIS_MAX_READY;
    add.u32 %r9, %r7, %r8;
    mul.wide.u32 %rd6, %r9, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f2, [%rd7];
    max.f32 %f1, %f1, %f2;
    add.u32 %r8, %r8, 1;
    bra CEIS_MAX_LOOP;
CEIS_MAX_READY:
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r8, 0;
CEIS_EXP_LOOP:
    setp.ge.u32 %p2, %r8, %r2;
    @%p2 bra CEIS_EXP_READY;
    add.u32 %r9, %r7, %r8;
    mul.wide.u32 %rd6, %r9, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f2, [%rd7];
    sub.rn.f32 %f3, %f2, %f1;
    mul.rn.f32 %f3, %f3, 0f3FB8AA3B;
    ex2.approx.f32 %f4, %f3;
    cvt.f64.f32 %fd2, %f4;
    add.rn.f64 %fd1, %fd1, %fd2;
    add.u32 %r8, %r8, 1;
    bra CEIS_EXP_LOOP;
CEIS_EXP_READY:
    mov.f64 %fd3, 0d3FF0000000000000;
    div.rn.f64 %fd3, %fd3, %fd1;
    cvt.rn.f32.f64 %f5, %fd3;
    mul.wide.u32 %rd8, %r6, 4;
    add.s64 %rd9, %rd2, %rd8;
    add.s64 %rd10, %rd3, %rd8;
    st.global.f32 [%rd9], %f1;
    st.global.f32 [%rd10], %f5;
CEIS_DONE:
    ret;
}

.visible .entry sura_cross_entropy_ids_loss_f32(
    .param .u64 param_logits,
    .param .u64 param_ids,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u64 param_loss,
    .param .u32 param_rows,
    .param .u32 param_classes
)
{
    .reg .pred %p<5>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<16>;
    .reg .f32 %f<10>;
    .reg .f64 %fd<8>;
    mov.u32 %r1, %ctaid.x;
    setp.ne.u32 %p1, %r1, 0;
    @%p1 bra CEIL_DONE;
    mov.u32 %r2, %tid.x;
    setp.ne.u32 %p2, %r2, 0;
    @%p2 bra CEIL_DONE;
    ld.param.u64 %rd1, [param_logits];
    ld.param.u64 %rd2, [param_ids];
    ld.param.u64 %rd3, [param_saved_max];
    ld.param.u64 %rd4, [param_saved_inv_sum];
    ld.param.u64 %rd5, [param_loss];
    ld.param.u32 %r3, [param_rows];
    ld.param.u32 %r4, [param_classes];
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r5, 0;
CEIL_ROW_LOOP:
    setp.ge.u32 %p3, %r5, %r3;
    @%p3 bra CEIL_STORE;
    mul.wide.u32 %rd6, %r5, 4;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.u32 %r6, [%rd7];
    setp.ge.u32 %p4, %r6, %r4;
    @%p4 bra CEIL_INVALID;
    add.s64 %rd8, %rd3, %rd6;
    add.s64 %rd9, %rd4, %rd6;
    ld.global.f32 %f1, [%rd8];
    ld.global.f32 %f2, [%rd9];
    mad.lo.u32 %r7, %r5, %r4, %r6;
    mul.wide.u32 %rd10, %r7, 4;
    add.s64 %rd11, %rd1, %rd10;
    ld.global.f32 %f3, [%rd11];
    lg2.approx.f32 %f4, %f2;
    mul.rn.f32 %f4, %f4, 0f3F317218;
    neg.f32 %f4, %f4;
    cvt.f64.f32 %fd2, %f1;
    cvt.f64.f32 %fd3, %f3;
    sub.rn.f64 %fd4, %fd2, %fd3;
    cvt.f64.f32 %fd5, %f4;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.rn.f64 %fd1, %fd1, %fd4;
    add.u32 %r5, %r5, 1;
    bra CEIL_ROW_LOOP;
CEIL_INVALID:
    mov.f32 %f5, 0f7FC00000;
    st.global.f32 [%rd5], %f5;
    bra CEIL_DONE;
CEIL_STORE:
    cvt.rn.f64.u32 %fd6, %r3;
    div.rn.f64 %fd1, %fd1, %fd6;
    cvt.rn.f32.f64 %f5, %fd1;
    st.global.f32 [%rd5], %f5;
CEIL_DONE:
    ret;
}

.visible .entry sura_cross_entropy_ids_backward_f32(
    .param .u64 param_logits,
    .param .u64 param_ids,
    .param .u64 param_grad_loss,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u64 param_grad_logits,
    .param .u32 param_count,
    .param .u32 param_rows,
    .param .u32 param_classes
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<16>;
    .reg .b64 %rd<16>;
    .reg .f32 %f<12>;
    .reg .f64 %fd<5>;
    ld.param.u64 %rd1, [param_logits];
    ld.param.u64 %rd2, [param_ids];
    ld.param.u64 %rd3, [param_grad_loss];
    ld.param.u64 %rd4, [param_saved_max];
    ld.param.u64 %rd5, [param_saved_inv_sum];
    ld.param.u64 %rd6, [param_grad_logits];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_rows];
    ld.param.u32 %r3, [param_classes];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r1;
    @%p1 bra CEIB_DONE;
    div.u32 %r8, %r7, %r3;
    rem.u32 %r9, %r7, %r3;
    mul.wide.u32 %rd7, %r7, 4;
    add.s64 %rd8, %rd1, %rd7;
    ld.global.f32 %f1, [%rd8];
    mul.wide.u32 %rd9, %r8, 4;
    add.s64 %rd10, %rd4, %rd9;
    add.s64 %rd11, %rd5, %rd9;
    ld.global.f32 %f2, [%rd10];
    ld.global.f32 %f3, [%rd11];
    sub.rn.f32 %f4, %f1, %f2;
    mul.rn.f32 %f4, %f4, 0f3FB8AA3B;
    ex2.approx.f32 %f5, %f4;
    mul.rn.f32 %f5, %f5, %f3;
    add.s64 %rd12, %rd2, %rd9;
    ld.global.u32 %r10, [%rd12];
    setp.ge.u32 %p3, %r10, %r3;
    @%p3 bra CEIB_INVALID;
    setp.eq.u32 %p2, %r9, %r10;
    selp.f32 %f6, 0f3F800000, 0f00000000, %p2;
    sub.rn.f32 %f7, %f5, %f6;
    ld.global.f32 %f8, [%rd3];
    cvt.f64.f32 %fd1, %f8;
    cvt.rn.f64.u32 %fd2, %r2;
    div.rn.f64 %fd3, %fd1, %fd2;
    cvt.rn.f32.f64 %f8, %fd3;
    mul.rn.f32 %f7, %f7, %f8;
    add.s64 %rd13, %rd6, %rd7;
    st.global.f32 [%rd13], %f7;
    bra CEIB_DONE;
CEIB_INVALID:
    mov.f32 %f10, 0f7FC00000;
    add.s64 %rd13, %rd6, %rd7;
    st.global.f32 [%rd13], %f10;
CEIB_DONE:
    ret;
}

// Generic contiguous rank-2..8 axis swap. Shape and input-stride metadata are
// passed by value in the launch parameter block, so a pure device transpose
// performs no metadata H2D transfer and needs no auxiliary allocation.
.visible .entry sura_transpose_f32(
    .param .u64 param_input,
    .param .u64 param_output,
    .param .u32 param_count,
    .param .u32 param_first,
    .param .u32 param_second,
    .param .u32 param_od0,
    .param .u32 param_od1,
    .param .u32 param_od2,
    .param .u32 param_od3,
    .param .u32 param_od4,
    .param .u32 param_od5,
    .param .u32 param_od6,
    .param .u32 param_od7,
    .param .u32 param_is0,
    .param .u32 param_is1,
    .param .u32 param_is2,
    .param .u32 param_is3,
    .param .u32 param_is4,
    .param .u32 param_is5,
    .param .u32 param_is6,
    .param .u32 param_is7
)
{
    .reg .pred %p<5>;
    .reg .b32 %r<20>;
    .reg .b64 %rd<8>;
    .reg .f32 %f<2>;
    ld.param.u64 %rd1, [param_input];
    ld.param.u64 %rd2, [param_output];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_first];
    ld.param.u32 %r3, [param_second];
    mov.u32 %r4, %ctaid.x;
    mov.u32 %r5, %ntid.x;
    mov.u32 %r6, %tid.x;
    mad.lo.s32 %r7, %r4, %r5, %r6;
    setp.ge.u32 %p1, %r7, %r1;
    @%p1 bra TRANSPOSE_DONE;
    mov.u32 %r9, %r7;
    mov.u32 %r10, 0;
    mov.u32 %r11, 0;
    mov.u32 %r12, 0;
    mov.u32 %r14, 0;
    mov.u32 %r15, 0;

    ld.param.u32 %r13, [param_od7];
    ld.param.u32 %r16, [param_is7];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 7;
    setp.eq.u32 %p3, %r3, 7;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od6];
    ld.param.u32 %r16, [param_is6];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 6;
    setp.eq.u32 %p3, %r3, 6;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od5];
    ld.param.u32 %r16, [param_is5];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 5;
    setp.eq.u32 %p3, %r3, 5;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od4];
    ld.param.u32 %r16, [param_is4];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 4;
    setp.eq.u32 %p3, %r3, 4;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od3];
    ld.param.u32 %r16, [param_is3];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 3;
    setp.eq.u32 %p3, %r3, 3;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od2];
    ld.param.u32 %r16, [param_is2];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 2;
    setp.eq.u32 %p3, %r3, 2;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od1];
    ld.param.u32 %r16, [param_is1];
    rem.u32 %r17, %r9, %r13;
    div.u32 %r9, %r9, %r13;
    setp.eq.u32 %p2, %r2, 1;
    setp.eq.u32 %p3, %r3, 1;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    ld.param.u32 %r13, [param_od0];
    ld.param.u32 %r16, [param_is0];
    rem.u32 %r17, %r9, %r13;
    setp.eq.u32 %p2, %r2, 0;
    setp.eq.u32 %p3, %r3, 0;
    @%p2 mov.u32 %r10, %r17;
    @%p3 mov.u32 %r11, %r17;
    @%p2 mov.u32 %r14, %r16;
    @%p3 mov.u32 %r15, %r16;
    or.pred %p4, %p2, %p3;
    @!%p4 mad.lo.u32 %r12, %r17, %r16, %r12;

    mad.lo.u32 %r12, %r11, %r14, %r12;
    mad.lo.u32 %r12, %r10, %r15, %r12;
    mul.wide.u32 %rd3, %r12, 4;
    mul.wide.u32 %rd4, %r7, 4;
    add.s64 %rd5, %rd1, %rd3;
    add.s64 %rd6, %rd2, %rd4;
    ld.global.f32 %f1, [%rd5];
    st.global.f32 [%rd6], %f1;
TRANSPOSE_DONE:
    ret;
}

// Low-memory correctness reference for causal attention. One thread owns an
// output row, applies the stable online-softmax recurrence, and writes only
// the row output plus two f32 statistics. It deliberately materializes no
// [sequence, sequence] score or probability matrix.
.visible .entry sura_causal_attention_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<32>;
    .reg .b64 %rd<32>;
    .reg .f32 %f<32>;
    .reg .f64 %fd<12>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_saved_max];
    ld.param.u64 %rd6, [param_saved_inv_sum];
    ld.param.u32 %r1, [param_total_rows];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r5, [param_save_stats];
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    mad.lo.s32 %r9, %r6, %r7, %r8;
    setp.ge.u32 %p1, %r9, %r1;
    @%p1 bra CA_FWD_DONE;
    div.u32 %r10, %r9, %r2;
    rem.u32 %r11, %r9, %r2;
    mul.lo.u32 %r12, %r2, %r3;
    mul.lo.u32 %r13, %r2, %r4;
    mul.lo.u32 %r14, %r10, %r12;
    mad.lo.u32 %r15, %r11, %r3, %r14;
    mul.lo.u32 %r16, %r10, %r13;
    mad.lo.u32 %r17, %r11, %r4, %r16;
    mov.u32 %r23, 0;
CA_FWD_ZERO_LOOP:
    setp.ge.u32 %p2, %r23, %r4;
    @%p2 bra CA_FWD_ZERO_READY;
    add.u32 %r24, %r17, %r23;
    mul.wide.u32 %rd7, %r24, 4;
    add.s64 %rd8, %rd4, %rd7;
    st.global.f32 [%rd8], 0f00000000;
    add.u32 %r23, %r23, 1;
    bra CA_FWD_ZERO_LOOP;
CA_FWD_ZERO_READY:
    mov.f64 %fd9, 0dFFF0000000000000;
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r18, 0;
CA_FWD_COL_LOOP:
    setp.gt.u32 %p3, %r18, %r11;
    @%p3 bra CA_FWD_NORMALIZE;
    mad.lo.u32 %r19, %r18, %r3, %r14;
    mov.f64 %fd2, 0d0000000000000000;
    mov.u32 %r20, 0;
CA_FWD_DOT_LOOP:
    setp.ge.u32 %p6, %r20, %r3;
    @%p6 bra CA_FWD_DOT_READY;
    add.u32 %r21, %r15, %r20;
    add.u32 %r22, %r19, %r20;
    mul.wide.u32 %rd9, %r21, 4;
    mul.wide.u32 %rd10, %r22, 4;
    add.s64 %rd11, %rd1, %rd9;
    add.s64 %rd12, %rd2, %rd10;
    ld.global.f32 %f7, [%rd11];
    ld.global.f32 %f8, [%rd12];
    cvt.f64.f32 %fd3, %f7;
    cvt.f64.f32 %fd4, %f8;
    mul.rn.f64 %fd3, %fd3, %fd4;
    add.rn.f64 %fd2, %fd2, %fd3;
    add.u32 %r20, %r20, 1;
    bra CA_FWD_DOT_LOOP;
CA_FWD_DOT_READY:
    cvt.f64.f32 %fd5, %f1;
    mul.rn.f64 %fd2, %fd2, %fd5;
    abs.f64 %fd10, %fd2;
    setp.le.f64 %p7, %fd10, 0d7FEFFFFFFFFFFFFF;
    @!%p7 bra CA_FWD_INVALID;
    max.f64 %fd10, %fd9, %fd2;
    setp.eq.u32 %p4, %r18, 0;
    @%p4 bra CA_FWD_FIRST_COL;
    sub.rn.f64 %fd11, %fd9, %fd10;
    cvt.rn.f32.f64 %f5, %fd11;
    mul.rn.f32 %f5, %f5, 0f3FB8AA3B;
    ex2.approx.f32 %f5, %f5;
    bra CA_FWD_PREVIOUS_READY;
CA_FWD_FIRST_COL:
    mov.f32 %f5, 0f00000000;
CA_FWD_PREVIOUS_READY:
    sub.rn.f64 %fd11, %fd2, %fd10;
    cvt.rn.f32.f64 %f6, %fd11;
    mul.rn.f32 %f6, %f6, 0f3FB8AA3B;
    ex2.approx.f32 %f6, %f6;
    cvt.f64.f32 %fd6, %f5;
    cvt.f64.f32 %fd7, %f6;
    mul.rn.f64 %fd3, %fd1, %fd6;
    add.rn.f64 %fd4, %fd3, %fd7;
    div.rn.f64 %fd3, %fd3, %fd4;
    div.rn.f64 %fd7, %fd7, %fd4;
    mad.lo.u32 %r25, %r18, %r4, %r16;
    mov.u32 %r23, 0;
CA_FWD_VALUE_LOOP:
    setp.ge.u32 %p8, %r23, %r4;
    @%p8 bra CA_FWD_VALUE_READY;
    add.u32 %r24, %r17, %r23;
    add.u32 %r26, %r25, %r23;
    mul.wide.u32 %rd13, %r24, 4;
    mul.wide.u32 %rd14, %r26, 4;
    add.s64 %rd15, %rd4, %rd13;
    add.s64 %rd16, %rd3, %rd14;
    ld.global.f32 %f10, [%rd15];
    ld.global.f32 %f11, [%rd16];
    cvt.f64.f32 %fd5, %f10;
    cvt.f64.f32 %fd6, %f11;
    mul.rn.f64 %fd5, %fd5, %fd3;
    mul.rn.f64 %fd6, %fd6, %fd7;
    add.rn.f64 %fd5, %fd5, %fd6;
    cvt.rn.f32.f64 %f12, %fd5;
    st.global.f32 [%rd15], %f12;
    add.u32 %r23, %r23, 1;
    bra CA_FWD_VALUE_LOOP;
CA_FWD_VALUE_READY:
    mov.f64 %fd1, %fd4;
    mov.f64 %fd9, %fd10;
    add.u32 %r18, %r18, 1;
    bra CA_FWD_COL_LOOP;
CA_FWD_NORMALIZE:
    mov.f64 %fd8, 0d3FF0000000000000;
    div.rn.f64 %fd8, %fd8, %fd1;
    cvt.rn.f32.f64 %f14, %fd8;
CA_FWD_SAVE:
    setp.eq.u32 %p5, %r5, 0;
    @%p5 bra CA_FWD_DONE;
    mul.wide.u32 %rd19, %r9, 4;
    add.s64 %rd20, %rd5, %rd19;
    add.s64 %rd21, %rd6, %rd19;
    cvt.rn.f32.f64 %f3, %fd9;
    st.global.f32 [%rd20], %f3;
    st.global.f32 [%rd21], %f14;
    bra CA_FWD_DONE;
CA_FWD_INVALID:
    mov.f32 %f16, 0f7FC00000;
    mov.u32 %r23, 0;
CA_FWD_INVALID_LOOP:
    setp.ge.u32 %p10, %r23, %r4;
    @%p10 bra CA_FWD_INVALID_SAVE;
    add.u32 %r24, %r17, %r23;
    mul.wide.u32 %rd22, %r24, 4;
    add.s64 %rd23, %rd4, %rd22;
    st.global.f32 [%rd23], %f16;
    add.u32 %r23, %r23, 1;
    bra CA_FWD_INVALID_LOOP;
CA_FWD_INVALID_SAVE:
    setp.eq.u32 %p11, %r5, 0;
    @%p11 bra CA_FWD_DONE;
    mul.wide.u32 %rd24, %r9, 4;
    add.s64 %rd25, %rd5, %rd24;
    add.s64 %rd26, %rd6, %rd24;
    st.global.f32 [%rd25], %f16;
    st.global.f32 [%rd26], %f16;
CA_FWD_DONE:
    ret;
}

// Legacy warp-parallel f32 online causal attention. One warp owns one output
// row and updates disjoint value dimensions with a fixed reduction tree. The
// forward stores no score matrix; this kernel pairs with the packed backward
// fallback when the fused recomputation path is disabled.
.visible .entry sura_causal_attention_warp_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<48>;
    .reg .b64 %rd<28>;
    .reg .f32 %f<32>;
    .reg .f64 %fd<12>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_saved_max];
    ld.param.u64 %rd6, [param_saved_inv_sum];
    ld.param.u32 %r1, [param_total_rows];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r5, [param_save_stats];
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    shr.u32 %r9, %r7, 5;
    shr.u32 %r10, %r8, 5;
    mad.lo.u32 %r11, %r6, %r9, %r10;
    and.b32 %r12, %r8, 31;
    setp.ge.u32 %p1, %r11, %r1;
    @%p1 bra CAW_DONE;
    div.u32 %r13, %r11, %r2;
    rem.u32 %r14, %r11, %r2;
    mul.lo.u32 %r15, %r2, %r3;
    mul.lo.u32 %r16, %r13, %r15;
    mad.lo.u32 %r17, %r14, %r3, %r16;
    mul.lo.u32 %r18, %r2, %r4;
    mul.lo.u32 %r19, %r13, %r18;
    mad.lo.u32 %r20, %r14, %r4, %r19;
    mov.f32 %f2, 0f00000000;
    mov.u32 %r21, %r12;
CAW_ZERO_LOOP:
    setp.ge.u32 %p2, %r21, %r4;
    @%p2 bra CAW_ZERO_READY;
    add.u32 %r22, %r20, %r21;
    mul.wide.u32 %rd7, %r22, 4;
    add.s64 %rd8, %rd4, %rd7;
    st.global.f32 [%rd8], %f2;
    add.u32 %r21, %r21, 32;
    bra CAW_ZERO_LOOP;
CAW_ZERO_READY:
    mov.f32 %f19, 0fFF800000;
    mov.f32 %f20, 0f00000000;
    mov.u32 %r23, 0;
CAW_COL_LOOP:
    setp.gt.u32 %p3, %r23, %r14;
    @%p3 bra CAW_SAVE;
    mad.lo.u32 %r24, %r23, %r3, %r16;
    mov.f32 %f17, 0f00000000;
    mov.u32 %r25, %r12;
CAW_DOT_LOOP:
    setp.ge.u32 %p4, %r25, %r3;
    @%p4 bra CAW_DOT_REDUCE;
    add.u32 %r26, %r17, %r25;
    add.u32 %r27, %r24, %r25;
    mul.wide.u32 %rd9, %r26, 4;
    mul.wide.u32 %rd10, %r27, 4;
    add.s64 %rd11, %rd1, %rd9;
    add.s64 %rd12, %rd2, %rd10;
    ld.global.f32 %f7, [%rd11];
    ld.global.f32 %f8, [%rd12];
    mul.rn.f32 %f18, %f7, %f8;
    add.rn.f32 %f17, %f17, %f18;
    add.u32 %r25, %r25, 32;
    bra CAW_DOT_LOOP;
CAW_DOT_REDUCE:
    mov.b32 %r30, %f17;
    shfl.sync.down.b32 %r32, %r30, 16, 31, 0xffffffff;
    mov.b32 %f18, %r32;
    add.rn.f32 %f17, %f17, %f18;
    mov.b32 %r30, %f17;
    shfl.sync.down.b32 %r32, %r30, 8, 31, 0xffffffff;
    mov.b32 %f18, %r32;
    add.rn.f32 %f17, %f17, %f18;
    mov.b32 %r30, %f17;
    shfl.sync.down.b32 %r32, %r30, 4, 31, 0xffffffff;
    mov.b32 %f18, %r32;
    add.rn.f32 %f17, %f17, %f18;
    mov.b32 %r30, %f17;
    shfl.sync.down.b32 %r32, %r30, 2, 31, 0xffffffff;
    mov.b32 %f18, %r32;
    add.rn.f32 %f17, %f17, %f18;
    mov.b32 %r30, %f17;
    shfl.sync.down.b32 %r32, %r30, 1, 31, 0xffffffff;
    mov.b32 %f18, %r32;
    add.rn.f32 %f17, %f17, %f18;
    mov.f32 %f24, 0f00000000;
    mov.f32 %f25, 0f00000000;
    mov.u32 %r36, 1;
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAW_LANE0_READY;
    mul.rn.f32 %f17, %f17, %f1;
    abs.f32 %f26, %f17;
    setp.le.f32 %p6, %f26, 0f7F7FFFFF;
    @!%p6 bra CAW_LANE0_INVALID;
    max.f32 %f21, %f19, %f17;
    setp.eq.u32 %p7, %r23, 0;
    @%p7 bra CAW_FIRST_COL;
    sub.rn.f32 %f22, %f19, %f21;
    mul.rn.f32 %f6, %f22, 0f3FB8AA3B;
    ex2.approx.f32 %f6, %f6;
    bra CAW_PREVIOUS_READY;
CAW_FIRST_COL:
    mov.f32 %f6, 0f00000000;
CAW_PREVIOUS_READY:
    sub.rn.f32 %f22, %f17, %f21;
    mul.rn.f32 %f9, %f22, 0f3FB8AA3B;
    ex2.approx.f32 %f9, %f9;
    mul.rn.f32 %f22, %f20, %f6;
    add.rn.f32 %f23, %f22, %f9;
    div.rn.f32 %f24, %f22, %f23;
    div.rn.f32 %f25, %f9, %f23;
    mov.f32 %f20, %f23;
    mov.f32 %f19, %f21;
    bra CAW_LANE0_READY;
CAW_LANE0_INVALID:
    mov.u32 %r36, 0;
CAW_LANE0_READY:
    shfl.sync.idx.b32 %r37, %r36, 0, 31, 0xffffffff;
    setp.eq.u32 %p8, %r37, 0;
    @%p8 bra CAW_INVALID;
    mov.b32 %r38, %f24;
    shfl.sync.idx.b32 %r40, %r38, 0, 31, 0xffffffff;
    mov.b32 %f24, %r40;
    mov.b32 %r38, %f25;
    shfl.sync.idx.b32 %r40, %r38, 0, 31, 0xffffffff;
    mov.b32 %f25, %r40;
    mad.lo.u32 %r42, %r23, %r4, %r19;
    mov.u32 %r21, %r12;
CAW_VALUE_LOOP:
    setp.ge.u32 %p9, %r21, %r4;
    @%p9 bra CAW_NEXT_COL;
    add.u32 %r22, %r20, %r21;
    add.u32 %r43, %r42, %r21;
    mul.wide.u32 %rd13, %r22, 4;
    mul.wide.u32 %rd14, %r43, 4;
    add.s64 %rd15, %rd4, %rd13;
    add.s64 %rd16, %rd3, %rd14;
    ld.global.f32 %f10, [%rd15];
    ld.global.f32 %f11, [%rd16];
    mul.rn.f32 %f18, %f10, %f24;
    mul.rn.f32 %f22, %f11, %f25;
    add.rn.f32 %f18, %f18, %f22;
    mov.f32 %f12, %f18;
    st.global.f32 [%rd15], %f12;
    add.u32 %r21, %r21, 32;
    bra CAW_VALUE_LOOP;
CAW_NEXT_COL:
    add.u32 %r23, %r23, 1;
    bra CAW_COL_LOOP;
CAW_SAVE:
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAW_DONE;
    setp.eq.u32 %p6, %r5, 0;
    @%p6 bra CAW_DONE;
    mov.f32 %f23, 0f3F800000;
    div.rn.f32 %f23, %f23, %f20;
    mov.f32 %f13, %f19;
    mov.f32 %f14, %f23;
    mul.wide.u32 %rd17, %r11, 4;
    add.s64 %rd18, %rd5, %rd17;
    add.s64 %rd19, %rd6, %rd17;
    st.global.f32 [%rd18], %f13;
    st.global.f32 [%rd19], %f14;
    bra CAW_DONE;
CAW_INVALID:
    mov.f32 %f16, 0f7FC00000;
    mov.u32 %r21, %r12;
CAW_INVALID_LOOP:
    setp.ge.u32 %p10, %r21, %r4;
    @%p10 bra CAW_INVALID_SAVE;
    add.u32 %r22, %r20, %r21;
    mul.wide.u32 %rd20, %r22, 4;
    add.s64 %rd21, %rd4, %rd20;
    st.global.f32 [%rd21], %f16;
    add.u32 %r21, %r21, 32;
    bra CAW_INVALID_LOOP;
CAW_INVALID_SAVE:
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAW_DONE;
    setp.eq.u32 %p6, %r5, 0;
    @%p6 bra CAW_DONE;
    mul.wide.u32 %rd22, %r11, 4;
    add.s64 %rd23, %rd5, %rd22;
    add.s64 %rd24, %rd6, %rd22;
    st.global.f32 [%rd23], %f16;
    st.global.f32 [%rd24], %f16;
CAW_DONE:
    ret;
}

// Fast warp-parallel online causal attention. This is the production f32
// path: one warp owns one output row, no score/probability matrix is stored,
// and lane reductions use the same fixed tree on every run. Explicit strict
// precision uses the serial f64 reference kernel; disabling the fused path
// selects the legacy f32 warp kernel above plus packed backward kernels.
.visible .entry sura_causal_attention_warp_fast_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_save_stats
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<52>;
    .reg .b64 %rd<28>;
    .reg .f32 %f<24>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_saved_max];
    ld.param.u64 %rd6, [param_saved_inv_sum];
    ld.param.u32 %r1, [param_total_rows];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r5, [param_save_stats];
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r8, %tid.x;
    shr.u32 %r9, %r7, 5;
    shr.u32 %r10, %r8, 5;
    mad.lo.u32 %r11, %r6, %r9, %r10;
    and.b32 %r12, %r8, 31;
    setp.ge.u32 %p1, %r11, %r1;
    @%p1 bra CAF_DONE;
    div.u32 %r13, %r11, %r2;
    rem.u32 %r14, %r11, %r2;
    mul.lo.u32 %r15, %r2, %r3;
    mul.lo.u32 %r16, %r13, %r15;
    mad.lo.u32 %r17, %r14, %r3, %r16;
    mul.lo.u32 %r18, %r2, %r4;
    mul.lo.u32 %r19, %r13, %r18;
    mad.lo.u32 %r20, %r14, %r4, %r19;
    mov.f32 %f2, 0f00000000;
    mov.u32 %r21, %r12;
CAF_ZERO_LOOP:
    setp.ge.u32 %p2, %r21, %r4;
    @%p2 bra CAF_ZERO_READY;
    add.u32 %r22, %r20, %r21;
    mul.wide.u32 %rd7, %r22, 4;
    add.s64 %rd8, %rd4, %rd7;
    st.global.f32 [%rd8], %f2;
    add.u32 %r21, %r21, 32;
    bra CAF_ZERO_LOOP;
CAF_ZERO_READY:
    mov.f32 %f3, 0fFF800000;
    mov.f32 %f4, 0f00000000;
    mov.u32 %r23, 0;
CAF_COL_LOOP:
    setp.gt.u32 %p3, %r23, %r14;
    @%p3 bra CAF_SAVE;
    mad.lo.u32 %r24, %r23, %r3, %r16;
    mov.f32 %f5, 0f00000000;
    mov.u32 %r25, %r12;
CAF_DOT_LOOP:
    setp.ge.u32 %p4, %r25, %r3;
    @%p4 bra CAF_DOT_REDUCE;
    add.u32 %r26, %r17, %r25;
    add.u32 %r27, %r24, %r25;
    mul.wide.u32 %rd9, %r26, 4;
    mul.wide.u32 %rd10, %r27, 4;
    add.s64 %rd11, %rd1, %rd9;
    add.s64 %rd12, %rd2, %rd10;
    ld.global.f32 %f6, [%rd11];
    ld.global.f32 %f7, [%rd12];
    fma.rn.f32 %f5, %f6, %f7, %f5;
    add.u32 %r25, %r25, 32;
    bra CAF_DOT_LOOP;
CAF_DOT_REDUCE:
    mov.b32 %r30, %f5;
    shfl.sync.down.b32 %r31, %r30, 16, 31, 0xffffffff;
    mov.b32 %f8, %r31;
    add.rn.f32 %f5, %f5, %f8;
    mov.b32 %r30, %f5;
    shfl.sync.down.b32 %r31, %r30, 8, 31, 0xffffffff;
    mov.b32 %f8, %r31;
    add.rn.f32 %f5, %f5, %f8;
    mov.b32 %r30, %f5;
    shfl.sync.down.b32 %r31, %r30, 4, 31, 0xffffffff;
    mov.b32 %f8, %r31;
    add.rn.f32 %f5, %f5, %f8;
    mov.b32 %r30, %f5;
    shfl.sync.down.b32 %r31, %r30, 2, 31, 0xffffffff;
    mov.b32 %f8, %r31;
    add.rn.f32 %f5, %f5, %f8;
    mov.b32 %r30, %f5;
    shfl.sync.down.b32 %r31, %r30, 1, 31, 0xffffffff;
    mov.b32 %f8, %r31;
    add.rn.f32 %f5, %f5, %f8;
    mov.f32 %f15, 0f00000000;
    mov.f32 %f16, 0f00000000;
    mov.u32 %r36, 1;
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAF_LANE0_READY;
    mul.rn.f32 %f5, %f5, %f1;
    abs.f32 %f9, %f5;
    setp.le.f32 %p6, %f9, 0f7F7FFFFF;
    @!%p6 bra CAF_LANE0_INVALID;
    max.f32 %f10, %f3, %f5;
    setp.eq.u32 %p7, %r23, 0;
    @%p7 bra CAF_FIRST_COL;
    sub.rn.f32 %f11, %f3, %f10;
    mul.rn.f32 %f11, %f11, 0f3FB8AA3B;
    ex2.approx.f32 %f11, %f11;
    bra CAF_PREVIOUS_READY;
CAF_FIRST_COL:
    mov.f32 %f11, 0f00000000;
CAF_PREVIOUS_READY:
    sub.rn.f32 %f12, %f5, %f10;
    mul.rn.f32 %f12, %f12, 0f3FB8AA3B;
    ex2.approx.f32 %f12, %f12;
    mul.rn.f32 %f13, %f4, %f11;
    add.rn.f32 %f14, %f13, %f12;
    div.rn.f32 %f15, %f13, %f14;
    div.rn.f32 %f16, %f12, %f14;
    mov.f32 %f4, %f14;
    mov.f32 %f3, %f10;
    bra CAF_LANE0_READY;
CAF_LANE0_INVALID:
    mov.u32 %r36, 0;
CAF_LANE0_READY:
    shfl.sync.idx.b32 %r37, %r36, 0, 31, 0xffffffff;
    setp.eq.u32 %p8, %r37, 0;
    @%p8 bra CAF_INVALID;
    mov.b32 %r38, %f15;
    shfl.sync.idx.b32 %r40, %r38, 0, 31, 0xffffffff;
    mov.b32 %f15, %r40;
    mov.b32 %r39, %f16;
    shfl.sync.idx.b32 %r41, %r39, 0, 31, 0xffffffff;
    mov.b32 %f16, %r41;
    mad.lo.u32 %r42, %r23, %r4, %r19;
    mov.u32 %r21, %r12;
CAF_VALUE_LOOP:
    setp.ge.u32 %p9, %r21, %r4;
    @%p9 bra CAF_NEXT_COL;
    add.u32 %r22, %r20, %r21;
    add.u32 %r43, %r42, %r21;
    mul.wide.u32 %rd13, %r22, 4;
    mul.wide.u32 %rd14, %r43, 4;
    add.s64 %rd15, %rd4, %rd13;
    add.s64 %rd16, %rd3, %rd14;
    ld.global.f32 %f17, [%rd15];
    ld.global.f32 %f18, [%rd16];
    mul.rn.f32 %f17, %f17, %f15;
    fma.rn.f32 %f19, %f18, %f16, %f17;
    st.global.f32 [%rd15], %f19;
    add.u32 %r21, %r21, 32;
    bra CAF_VALUE_LOOP;
CAF_NEXT_COL:
    add.u32 %r23, %r23, 1;
    bra CAF_COL_LOOP;
CAF_SAVE:
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAF_DONE;
    setp.eq.u32 %p6, %r5, 0;
    @%p6 bra CAF_DONE;
    div.rn.f32 %f20, 0f3F800000, %f4;
    mul.wide.u32 %rd17, %r11, 4;
    add.s64 %rd18, %rd5, %rd17;
    add.s64 %rd19, %rd6, %rd17;
    st.global.f32 [%rd18], %f3;
    st.global.f32 [%rd19], %f20;
    bra CAF_DONE;
CAF_INVALID:
    mov.f32 %f21, 0f7FC00000;
    mov.u32 %r21, %r12;
CAF_INVALID_LOOP:
    setp.ge.u32 %p10, %r21, %r4;
    @%p10 bra CAF_INVALID_SAVE;
    add.u32 %r22, %r20, %r21;
    mul.wide.u32 %rd20, %r22, 4;
    add.s64 %rd21, %rd4, %rd20;
    st.global.f32 [%rd21], %f21;
    add.u32 %r21, %r21, 32;
    bra CAF_INVALID_LOOP;
CAF_INVALID_SAVE:
    setp.ne.u32 %p5, %r12, 0;
    @%p5 bra CAF_DONE;
    setp.eq.u32 %p6, %r5, 0;
    @%p6 bra CAF_DONE;
    mul.wide.u32 %rd22, %r11, 4;
    add.s64 %rd23, %rd5, %rd22;
    add.s64 %rd24, %rd6, %rd22;
    st.global.f32 [%rd23], %f21;
    st.global.f32 [%rd24], %f21;
CAF_DONE:
    ret;
}

// FlashAttention-style deterministic backward for dQ. A warp owns one
// (causal row, 32-feature tile), recomputes probabilities from the saved
// forward max/inverse-sum, and writes each dQ element exactly once. There is
// no O(T^2) probability workspace and no atomic reduction.
.visible .entry sura_causal_attention_fused_query_backward_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_gradient_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u64 param_gradient_query,
    .param .u32 param_work_warps,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_dimension_tiles
)
{
    .reg .pred %p<12>;
    .reg .b32 %r<64>;
    .reg .b64 %rd<32>;
    .reg .f32 %f<20>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_gradient_output];
    ld.param.u64 %rd6, [param_saved_max];
    ld.param.u64 %rd7, [param_saved_inv_sum];
    ld.param.u64 %rd8, [param_gradient_query];
    ld.param.u32 %r1, [param_work_warps];
    ld.param.u32 %r2, [param_total_rows];
    ld.param.u32 %r3, [param_sequence];
    ld.param.u32 %r4, [param_dimensions];
    ld.param.u32 %r5, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r6, [param_dimension_tiles];
    mov.u32 %r7, %ctaid.x;
    mov.u32 %r8, %ntid.x;
    mov.u32 %r9, %tid.x;
    shr.u32 %r10, %r8, 5;
    shr.u32 %r11, %r9, 5;
    mad.lo.u32 %r12, %r7, %r10, %r11;
    and.b32 %r13, %r9, 31;
    setp.ge.u32 %p1, %r12, %r1;
    @%p1 bra CAFQ_DONE;
    div.u32 %r14, %r12, %r6;
    rem.u32 %r15, %r12, %r6;
    setp.ge.u32 %p2, %r14, %r2;
    @%p2 bra CAFQ_DONE;
    div.u32 %r16, %r14, %r3;
    rem.u32 %r17, %r14, %r3;
    mul.lo.u32 %r18, %r3, %r4;
    mul.lo.u32 %r19, %r16, %r18;
    mad.lo.u32 %r20, %r17, %r4, %r19;
    mul.lo.u32 %r21, %r3, %r5;
    mul.lo.u32 %r22, %r16, %r21;
    mad.lo.u32 %r23, %r17, %r5, %r22;
    shl.b32 %r24, %r15, 5;
    add.u32 %r24, %r24, %r13;

    // D_i = dot(dO_i, O_i), used by the softmax Jacobian identity.
    mov.f32 %f2, 0f00000000;
    mov.u32 %r25, %r13;
CAFQ_SOFT_DOT_LOOP:
    setp.ge.u32 %p3, %r25, %r5;
    @%p3 bra CAFQ_SOFT_DOT_REDUCE;
    add.u32 %r26, %r23, %r25;
    mul.wide.u32 %rd9, %r26, 4;
    add.s64 %rd10, %rd4, %rd9;
    add.s64 %rd11, %rd5, %rd9;
    ld.global.f32 %f3, [%rd10];
    ld.global.f32 %f4, [%rd11];
    fma.rn.f32 %f2, %f3, %f4, %f2;
    add.u32 %r25, %r25, 32;
    bra CAFQ_SOFT_DOT_LOOP;
CAFQ_SOFT_DOT_REDUCE:
    mov.b32 %r40, %f2;
    shfl.sync.down.b32 %r41, %r40, 16, 31, 0xffffffff;
    mov.b32 %f5, %r41;
    add.rn.f32 %f2, %f2, %f5;
    mov.b32 %r40, %f2;
    shfl.sync.down.b32 %r41, %r40, 8, 31, 0xffffffff;
    mov.b32 %f5, %r41;
    add.rn.f32 %f2, %f2, %f5;
    mov.b32 %r40, %f2;
    shfl.sync.down.b32 %r41, %r40, 4, 31, 0xffffffff;
    mov.b32 %f5, %r41;
    add.rn.f32 %f2, %f2, %f5;
    mov.b32 %r40, %f2;
    shfl.sync.down.b32 %r41, %r40, 2, 31, 0xffffffff;
    mov.b32 %f5, %r41;
    add.rn.f32 %f2, %f2, %f5;
    mov.b32 %r40, %f2;
    shfl.sync.down.b32 %r41, %r40, 1, 31, 0xffffffff;
    mov.b32 %f5, %r41;
    add.rn.f32 %f2, %f2, %f5;
    mov.b32 %r40, %f2;
    shfl.sync.idx.b32 %r41, %r40, 0, 31, 0xffffffff;
    mov.b32 %f2, %r41;

    // Saved row statistics are read once and broadcast within the warp.
    mov.f32 %f6, 0f00000000;
    mov.f32 %f7, 0f00000000;
    setp.ne.u32 %p4, %r13, 0;
    @%p4 bra CAFQ_STATS_BROADCAST;
    mul.wide.u32 %rd12, %r14, 4;
    add.s64 %rd13, %rd6, %rd12;
    add.s64 %rd14, %rd7, %rd12;
    ld.global.f32 %f6, [%rd13];
    ld.global.f32 %f7, [%rd14];
CAFQ_STATS_BROADCAST:
    mov.b32 %r42, %f6;
    shfl.sync.idx.b32 %r43, %r42, 0, 31, 0xffffffff;
    mov.b32 %f6, %r43;
    mov.b32 %r42, %f7;
    shfl.sync.idx.b32 %r43, %r42, 0, 31, 0xffffffff;
    mov.b32 %f7, %r43;
    mov.f32 %f8, 0f00000000;
    mov.u32 %r27, 0;
CAFQ_COL_LOOP:
    setp.gt.u32 %p5, %r27, %r17;
    @%p5 bra CAFQ_STORE;
    mad.lo.u32 %r28, %r27, %r4, %r19;
    mad.lo.u32 %r29, %r27, %r5, %r22;
    mov.f32 %f9, 0f00000000;
    mov.f32 %f10, 0f00000000;
    mov.u32 %r30, %r13;
CAFQ_PAIR_DOT_LOOP:
    setp.ge.u32 %p6, %r30, %r4;
    @%p6 bra CAFQ_VALUE_DOT_LOOP_START;
    add.u32 %r31, %r20, %r30;
    add.u32 %r32, %r28, %r30;
    mul.wide.u32 %rd15, %r31, 4;
    mul.wide.u32 %rd16, %r32, 4;
    add.s64 %rd17, %rd1, %rd15;
    add.s64 %rd18, %rd2, %rd16;
    ld.global.f32 %f11, [%rd17];
    ld.global.f32 %f12, [%rd18];
    fma.rn.f32 %f9, %f11, %f12, %f9;
    add.u32 %r30, %r30, 32;
    bra CAFQ_PAIR_DOT_LOOP;
CAFQ_VALUE_DOT_LOOP_START:
    mov.u32 %r30, %r13;
CAFQ_VALUE_DOT_LOOP:
    setp.ge.u32 %p7, %r30, %r5;
    @%p7 bra CAFQ_PAIR_REDUCE;
    add.u32 %r31, %r23, %r30;
    add.u32 %r32, %r29, %r30;
    mul.wide.u32 %rd19, %r31, 4;
    mul.wide.u32 %rd20, %r32, 4;
    add.s64 %rd21, %rd5, %rd19;
    add.s64 %rd22, %rd3, %rd20;
    ld.global.f32 %f11, [%rd21];
    ld.global.f32 %f12, [%rd22];
    fma.rn.f32 %f10, %f11, %f12, %f10;
    add.u32 %r30, %r30, 32;
    bra CAFQ_VALUE_DOT_LOOP;
CAFQ_PAIR_REDUCE:
    mov.b32 %r44, %f9;
    mov.b32 %r45, %f10;
    shfl.sync.down.b32 %r46, %r44, 16, 31, 0xffffffff;
    shfl.sync.down.b32 %r47, %r45, 16, 31, 0xffffffff;
    mov.b32 %f13, %r46;
    mov.b32 %f14, %r47;
    add.rn.f32 %f9, %f9, %f13;
    add.rn.f32 %f10, %f10, %f14;
    mov.b32 %r44, %f9;
    mov.b32 %r45, %f10;
    shfl.sync.down.b32 %r46, %r44, 8, 31, 0xffffffff;
    shfl.sync.down.b32 %r47, %r45, 8, 31, 0xffffffff;
    mov.b32 %f13, %r46;
    mov.b32 %f14, %r47;
    add.rn.f32 %f9, %f9, %f13;
    add.rn.f32 %f10, %f10, %f14;
    mov.b32 %r44, %f9;
    mov.b32 %r45, %f10;
    shfl.sync.down.b32 %r46, %r44, 4, 31, 0xffffffff;
    shfl.sync.down.b32 %r47, %r45, 4, 31, 0xffffffff;
    mov.b32 %f13, %r46;
    mov.b32 %f14, %r47;
    add.rn.f32 %f9, %f9, %f13;
    add.rn.f32 %f10, %f10, %f14;
    mov.b32 %r44, %f9;
    mov.b32 %r45, %f10;
    shfl.sync.down.b32 %r46, %r44, 2, 31, 0xffffffff;
    shfl.sync.down.b32 %r47, %r45, 2, 31, 0xffffffff;
    mov.b32 %f13, %r46;
    mov.b32 %f14, %r47;
    add.rn.f32 %f9, %f9, %f13;
    add.rn.f32 %f10, %f10, %f14;
    mov.b32 %r44, %f9;
    mov.b32 %r45, %f10;
    shfl.sync.down.b32 %r46, %r44, 1, 31, 0xffffffff;
    shfl.sync.down.b32 %r47, %r45, 1, 31, 0xffffffff;
    mov.b32 %f13, %r46;
    mov.b32 %f14, %r47;
    add.rn.f32 %f9, %f9, %f13;
    add.rn.f32 %f10, %f10, %f14;
    mov.f32 %f16, 0f00000000;
    setp.ne.u32 %p8, %r13, 0;
    @%p8 bra CAFQ_DS_BROADCAST;
    mul.rn.f32 %f9, %f9, %f1;
    sub.rn.f32 %f15, %f9, %f6;
    mul.rn.f32 %f15, %f15, 0f3FB8AA3B;
    ex2.approx.f32 %f15, %f15;
    mul.rn.f32 %f15, %f15, %f7;
    sub.rn.f32 %f16, %f10, %f2;
    mul.rn.f32 %f16, %f16, %f15;
    mul.rn.f32 %f16, %f16, %f1;
CAFQ_DS_BROADCAST:
    mov.b32 %r48, %f16;
    shfl.sync.idx.b32 %r49, %r48, 0, 31, 0xffffffff;
    mov.b32 %f16, %r49;
    setp.ge.u32 %p9, %r24, %r4;
    @%p9 bra CAFQ_NEXT_COL;
    add.u32 %r33, %r28, %r24;
    mul.wide.u32 %rd23, %r33, 4;
    add.s64 %rd24, %rd2, %rd23;
    ld.global.f32 %f17, [%rd24];
    fma.rn.f32 %f8, %f16, %f17, %f8;
CAFQ_NEXT_COL:
    add.u32 %r27, %r27, 1;
    bra CAFQ_COL_LOOP;
CAFQ_STORE:
    setp.ge.u32 %p10, %r24, %r4;
    @%p10 bra CAFQ_DONE;
    add.u32 %r34, %r20, %r24;
    mul.wide.u32 %rd25, %r34, 4;
    add.s64 %rd26, %rd8, %rd25;
    st.global.f32 [%rd26], %f8;
CAFQ_DONE:
    ret;
}

// Fused dK+dV counterpart. One warp owns one key row and one 32-feature
// tile, loops over all causal query rows, and writes race-free gradients.
.visible .entry sura_causal_attention_fused_key_value_backward_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_gradient_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u64 param_gradient_key,
    .param .u64 param_gradient_value,
    .param .u32 param_work_warps,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_feature_tiles,
    .param .u32 param_need_key,
    .param .u32 param_need_value
)
{
    .reg .pred %p<16>;
    .reg .b32 %r<72>;
    .reg .b64 %rd<36>;
    .reg .f32 %f<24>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_gradient_output];
    ld.param.u64 %rd6, [param_saved_max];
    ld.param.u64 %rd7, [param_saved_inv_sum];
    ld.param.u64 %rd8, [param_gradient_key];
    ld.param.u64 %rd9, [param_gradient_value];
    ld.param.u32 %r1, [param_work_warps];
    ld.param.u32 %r2, [param_total_rows];
    ld.param.u32 %r3, [param_sequence];
    ld.param.u32 %r4, [param_dimensions];
    ld.param.u32 %r5, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r6, [param_feature_tiles];
    ld.param.u32 %r7, [param_need_key];
    ld.param.u32 %r8, [param_need_value];
    mov.u32 %r9, %ctaid.x;
    mov.u32 %r10, %ntid.x;
    mov.u32 %r11, %tid.x;
    shr.u32 %r12, %r10, 5;
    shr.u32 %r13, %r11, 5;
    mad.lo.u32 %r14, %r9, %r12, %r13;
    and.b32 %r15, %r11, 31;
    setp.ge.u32 %p1, %r14, %r1;
    @%p1 bra CAFKV_DONE;
    div.u32 %r16, %r14, %r6;
    rem.u32 %r17, %r14, %r6;
    setp.ge.u32 %p2, %r16, %r2;
    @%p2 bra CAFKV_DONE;
    div.u32 %r18, %r16, %r3;
    rem.u32 %r19, %r16, %r3;
    mul.lo.u32 %r20, %r3, %r4;
    mul.lo.u32 %r21, %r18, %r20;
    mad.lo.u32 %r22, %r19, %r4, %r21;
    mul.lo.u32 %r23, %r3, %r5;
    mul.lo.u32 %r24, %r18, %r23;
    mad.lo.u32 %r25, %r19, %r5, %r24;
    shl.b32 %r26, %r17, 5;
    add.u32 %r26, %r26, %r15;
    mov.f32 %f2, 0f00000000;
    mov.f32 %f3, 0f00000000;
    mov.u32 %r27, %r19;
CAFKV_ROW_LOOP:
    setp.ge.u32 %p3, %r27, %r3;
    @%p3 bra CAFKV_STORE;
    mad.lo.u32 %r28, %r27, %r4, %r21;
    mad.lo.u32 %r29, %r27, %r5, %r24;
    mad.lo.u32 %r30, %r18, %r3, %r27;
    mov.f32 %f4, 0f00000000;
    mov.u32 %r31, %r15;
CAFKV_SCORE_LOOP:
    setp.ge.u32 %p4, %r31, %r4;
    @%p4 bra CAFKV_SCORE_REDUCE;
    add.u32 %r32, %r28, %r31;
    add.u32 %r33, %r22, %r31;
    mul.wide.u32 %rd10, %r32, 4;
    mul.wide.u32 %rd11, %r33, 4;
    add.s64 %rd12, %rd1, %rd10;
    add.s64 %rd13, %rd2, %rd11;
    ld.global.f32 %f5, [%rd12];
    ld.global.f32 %f6, [%rd13];
    fma.rn.f32 %f4, %f5, %f6, %f4;
    add.u32 %r31, %r31, 32;
    bra CAFKV_SCORE_LOOP;
CAFKV_SCORE_REDUCE:
    mov.b32 %r48, %f4;
    shfl.sync.down.b32 %r49, %r48, 16, 31, 0xffffffff;
    mov.b32 %f7, %r49;
    add.rn.f32 %f4, %f4, %f7;
    mov.b32 %r48, %f4;
    shfl.sync.down.b32 %r49, %r48, 8, 31, 0xffffffff;
    mov.b32 %f7, %r49;
    add.rn.f32 %f4, %f4, %f7;
    mov.b32 %r48, %f4;
    shfl.sync.down.b32 %r49, %r48, 4, 31, 0xffffffff;
    mov.b32 %f7, %r49;
    add.rn.f32 %f4, %f4, %f7;
    mov.b32 %r48, %f4;
    shfl.sync.down.b32 %r49, %r48, 2, 31, 0xffffffff;
    mov.b32 %f7, %r49;
    add.rn.f32 %f4, %f4, %f7;
    mov.b32 %r48, %f4;
    shfl.sync.down.b32 %r49, %r48, 1, 31, 0xffffffff;
    mov.b32 %f7, %r49;
    add.rn.f32 %f4, %f4, %f7;
    mov.f32 %f10, 0f00000000;
    setp.ne.u32 %p5, %r15, 0;
    @%p5 bra CAFKV_PROB_BROADCAST;
    mul.wide.u32 %rd14, %r30, 4;
    add.s64 %rd15, %rd6, %rd14;
    add.s64 %rd16, %rd7, %rd14;
    ld.global.f32 %f8, [%rd15];
    ld.global.f32 %f9, [%rd16];
    mul.rn.f32 %f4, %f4, %f1;
    sub.rn.f32 %f10, %f4, %f8;
    mul.rn.f32 %f10, %f10, 0f3FB8AA3B;
    ex2.approx.f32 %f10, %f10;
    mul.rn.f32 %f10, %f10, %f9;
CAFKV_PROB_BROADCAST:
    mov.b32 %r50, %f10;
    shfl.sync.idx.b32 %r51, %r50, 0, 31, 0xffffffff;
    mov.b32 %f10, %r51;

    // dK needs dp and D; dV alone skips both reductions.
    setp.eq.u32 %p6, %r7, 0;
    @%p6 bra CAFKV_ACCUMULATE;
    mov.f32 %f11, 0f00000000;
    mov.f32 %f12, 0f00000000;
    mov.u32 %r31, %r15;
CAFKV_VALUE_DOTS_LOOP:
    setp.ge.u32 %p7, %r31, %r5;
    @%p7 bra CAFKV_VALUE_DOTS_REDUCE;
    add.u32 %r32, %r29, %r31;
    add.u32 %r33, %r25, %r31;
    mul.wide.u32 %rd17, %r32, 4;
    mul.wide.u32 %rd18, %r33, 4;
    add.s64 %rd19, %rd5, %rd17;
    add.s64 %rd20, %rd4, %rd17;
    add.s64 %rd21, %rd3, %rd18;
    ld.global.f32 %f13, [%rd19];
    ld.global.f32 %f14, [%rd20];
    ld.global.f32 %f15, [%rd21];
    fma.rn.f32 %f11, %f13, %f14, %f11;
    fma.rn.f32 %f12, %f13, %f15, %f12;
    add.u32 %r31, %r31, 32;
    bra CAFKV_VALUE_DOTS_LOOP;
CAFKV_VALUE_DOTS_REDUCE:
    mov.b32 %r52, %f11;
    mov.b32 %r53, %f12;
    shfl.sync.down.b32 %r54, %r52, 16, 31, 0xffffffff;
    shfl.sync.down.b32 %r55, %r53, 16, 31, 0xffffffff;
    mov.b32 %f16, %r54;
    mov.b32 %f17, %r55;
    add.rn.f32 %f11, %f11, %f16;
    add.rn.f32 %f12, %f12, %f17;
    mov.b32 %r52, %f11;
    mov.b32 %r53, %f12;
    shfl.sync.down.b32 %r54, %r52, 8, 31, 0xffffffff;
    shfl.sync.down.b32 %r55, %r53, 8, 31, 0xffffffff;
    mov.b32 %f16, %r54;
    mov.b32 %f17, %r55;
    add.rn.f32 %f11, %f11, %f16;
    add.rn.f32 %f12, %f12, %f17;
    mov.b32 %r52, %f11;
    mov.b32 %r53, %f12;
    shfl.sync.down.b32 %r54, %r52, 4, 31, 0xffffffff;
    shfl.sync.down.b32 %r55, %r53, 4, 31, 0xffffffff;
    mov.b32 %f16, %r54;
    mov.b32 %f17, %r55;
    add.rn.f32 %f11, %f11, %f16;
    add.rn.f32 %f12, %f12, %f17;
    mov.b32 %r52, %f11;
    mov.b32 %r53, %f12;
    shfl.sync.down.b32 %r54, %r52, 2, 31, 0xffffffff;
    shfl.sync.down.b32 %r55, %r53, 2, 31, 0xffffffff;
    mov.b32 %f16, %r54;
    mov.b32 %f17, %r55;
    add.rn.f32 %f11, %f11, %f16;
    add.rn.f32 %f12, %f12, %f17;
    mov.b32 %r52, %f11;
    mov.b32 %r53, %f12;
    shfl.sync.down.b32 %r54, %r52, 1, 31, 0xffffffff;
    shfl.sync.down.b32 %r55, %r53, 1, 31, 0xffffffff;
    mov.b32 %f16, %r54;
    mov.b32 %f17, %r55;
    add.rn.f32 %f11, %f11, %f16;
    add.rn.f32 %f12, %f12, %f17;
    mov.f32 %f18, 0f00000000;
    setp.ne.u32 %p8, %r15, 0;
    @%p8 bra CAFKV_DS_BROADCAST;
    sub.rn.f32 %f18, %f12, %f11;
    mul.rn.f32 %f18, %f18, %f10;
    mul.rn.f32 %f18, %f18, %f1;
CAFKV_DS_BROADCAST:
    mov.b32 %r56, %f18;
    shfl.sync.idx.b32 %r57, %r56, 0, 31, 0xffffffff;
    mov.b32 %f18, %r57;
CAFKV_ACCUMULATE:
    setp.eq.u32 %p9, %r7, 0;
    @%p9 bra CAFKV_ACCUMULATE_VALUE;
    setp.ge.u32 %p10, %r26, %r4;
    @%p10 bra CAFKV_ACCUMULATE_VALUE;
    add.u32 %r34, %r28, %r26;
    mul.wide.u32 %rd22, %r34, 4;
    add.s64 %rd23, %rd1, %rd22;
    ld.global.f32 %f19, [%rd23];
    fma.rn.f32 %f2, %f18, %f19, %f2;
CAFKV_ACCUMULATE_VALUE:
    setp.eq.u32 %p11, %r8, 0;
    @%p11 bra CAFKV_NEXT_ROW;
    setp.ge.u32 %p12, %r26, %r5;
    @%p12 bra CAFKV_NEXT_ROW;
    add.u32 %r35, %r29, %r26;
    mul.wide.u32 %rd24, %r35, 4;
    add.s64 %rd25, %rd5, %rd24;
    ld.global.f32 %f20, [%rd25];
    fma.rn.f32 %f3, %f10, %f20, %f3;
CAFKV_NEXT_ROW:
    add.u32 %r27, %r27, 1;
    bra CAFKV_ROW_LOOP;
CAFKV_STORE:
    setp.eq.u32 %p13, %r7, 0;
    @%p13 bra CAFKV_STORE_VALUE;
    setp.ge.u32 %p14, %r26, %r4;
    @%p14 bra CAFKV_STORE_VALUE;
    add.u32 %r36, %r22, %r26;
    mul.wide.u32 %rd26, %r36, 4;
    add.s64 %rd27, %rd8, %rd26;
    st.global.f32 [%rd27], %f2;
CAFKV_STORE_VALUE:
    setp.eq.u32 %p13, %r8, 0;
    @%p13 bra CAFKV_DONE;
    setp.ge.u32 %p14, %r26, %r5;
    @%p14 bra CAFKV_DONE;
    add.u32 %r37, %r25, %r26;
    mul.wide.u32 %rd28, %r37, 4;
    add.s64 %rd29, %rd9, %rd28;
    st.global.f32 [%rd29], %f3;
CAFKV_DONE:
    ret;
}

// Deterministic low-memory backward. One thread owns one flattened prefix
// batch/head, so q/k/v reductions need neither atomics nor host transfers.
// Probabilities are recomputed from saved row statistics.
.visible .entry sura_causal_attention_backward_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_gradient_output,
    .param .u64 param_saved_max,
    .param .u64 param_saved_inv_sum,
    .param .u64 param_gradient_query,
    .param .u64 param_gradient_key,
    .param .u64 param_gradient_value,
    .param .u32 param_batches,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_need_query,
    .param .u32 param_need_key,
    .param .u32 param_need_value
)
{
    .reg .pred %p<15>;
    .reg .b32 %r<40>;
    .reg .b64 %rd<40>;
    .reg .f32 %f<24>;
    .reg .f64 %fd<20>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_value];
    ld.param.u64 %rd4, [param_output];
    ld.param.u64 %rd5, [param_gradient_output];
    ld.param.u64 %rd6, [param_saved_max];
    ld.param.u64 %rd7, [param_saved_inv_sum];
    ld.param.u64 %rd8, [param_gradient_query];
    ld.param.u64 %rd9, [param_gradient_key];
    ld.param.u64 %rd10, [param_gradient_value];
    ld.param.u32 %r1, [param_batches];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r30, [param_need_query];
    ld.param.u32 %r31, [param_need_key];
    ld.param.u32 %r32, [param_need_value];
    mov.f32 %f20, 0f00000000;
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r1;
    @%p1 bra CA_BWD_DONE;
    mul.lo.u32 %r9, %r2, %r3;
    mul.lo.u32 %r10, %r2, %r4;
    mul.lo.u32 %r11, %r8, %r9;
    mul.lo.u32 %r12, %r8, %r10;
    mul.lo.u32 %r13, %r8, %r2;
    mov.u32 %r14, 0;
CA_BWD_ZERO_QK_LOOP:
    setp.ge.u32 %p2, %r14, %r9;
    @%p2 bra CA_BWD_ZERO_V_START;
    add.u32 %r24, %r11, %r14;
    mul.wide.u32 %rd11, %r24, 4;
    add.s64 %rd12, %rd8, %rd11;
    add.s64 %rd13, %rd9, %rd11;
    setp.ne.u32 %p12, %r30, 0;
    setp.ne.u32 %p13, %r31, 0;
    @%p12 st.global.f32 [%rd12], %f20;
    @%p13 st.global.f32 [%rd13], %f20;
    add.u32 %r14, %r14, 1;
    bra CA_BWD_ZERO_QK_LOOP;
CA_BWD_ZERO_V_START:
    mov.u32 %r14, 0;
CA_BWD_ZERO_V_LOOP:
    setp.ge.u32 %p3, %r14, %r10;
    @%p3 bra CA_BWD_ROW_START;
    add.u32 %r24, %r12, %r14;
    mul.wide.u32 %rd14, %r24, 4;
    add.s64 %rd15, %rd10, %rd14;
    setp.ne.u32 %p12, %r32, 0;
    @%p12 st.global.f32 [%rd15], %f20;
    add.u32 %r14, %r14, 1;
    bra CA_BWD_ZERO_V_LOOP;
CA_BWD_ROW_START:
    mov.u32 %r15, 0;
CA_BWD_ROW_LOOP:
    setp.ge.u32 %p4, %r15, %r2;
    @%p4 bra CA_BWD_DONE;
    mad.lo.u32 %r16, %r15, %r3, %r11;
    mad.lo.u32 %r17, %r15, %r4, %r12;
    add.u32 %r18, %r13, %r15;
    mul.wide.u32 %rd16, %r18, 4;
    add.s64 %rd17, %rd6, %rd16;
    add.s64 %rd18, %rd7, %rd16;
    ld.global.f32 %f2, [%rd17];
    ld.global.f32 %f3, [%rd18];
    // Recompute row statistics in f64. Saved f32 diagnostics are retained for
    // observability/lifetime checks, but are intentionally not the numerical
    // source of truth when absolute scores exceed float32 range.
    mov.f64 %fd18, 0dFFF0000000000000;
    mov.u32 %r20, 0;
CA_BWD_MAX_COL_LOOP:
    setp.gt.u32 %p11, %r20, %r15;
    @%p11 bra CA_BWD_SUM_START;
    mad.lo.u32 %r21, %r20, %r3, %r11;
    mov.f64 %fd4, 0d0000000000000000;
    mov.u32 %r23, 0;
CA_BWD_MAX_DOT_LOOP:
    setp.ge.u32 %p7, %r23, %r3;
    @%p7 bra CA_BWD_MAX_DOT_READY;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r21, %r23;
    mul.wide.u32 %rd22, %r24, 4;
    mul.wide.u32 %rd23, %r25, 4;
    add.s64 %rd24, %rd1, %rd22;
    add.s64 %rd25, %rd2, %rd23;
    ld.global.f32 %f6, [%rd24];
    ld.global.f32 %f7, [%rd25];
    cvt.f64.f32 %fd5, %f6;
    cvt.f64.f32 %fd6, %f7;
    mul.rn.f64 %fd5, %fd5, %fd6;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.u32 %r23, %r23, 1;
    bra CA_BWD_MAX_DOT_LOOP;
CA_BWD_MAX_DOT_READY:
    cvt.f64.f32 %fd7, %f1;
    mul.rn.f64 %fd4, %fd4, %fd7;
    max.f64 %fd18, %fd18, %fd4;
    add.u32 %r20, %r20, 1;
    bra CA_BWD_MAX_COL_LOOP;
CA_BWD_SUM_START:
    mov.f64 %fd19, 0d0000000000000000;
    mov.u32 %r20, 0;
CA_BWD_SUM_COL_LOOP:
    setp.gt.u32 %p11, %r20, %r15;
    @%p11 bra CA_BWD_SUM_READY;
    mad.lo.u32 %r21, %r20, %r3, %r11;
    mov.f64 %fd4, 0d0000000000000000;
    mov.u32 %r23, 0;
CA_BWD_SUM_DOT_LOOP:
    setp.ge.u32 %p7, %r23, %r3;
    @%p7 bra CA_BWD_SUM_DOT_READY;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r21, %r23;
    mul.wide.u32 %rd22, %r24, 4;
    mul.wide.u32 %rd23, %r25, 4;
    add.s64 %rd24, %rd1, %rd22;
    add.s64 %rd25, %rd2, %rd23;
    ld.global.f32 %f6, [%rd24];
    ld.global.f32 %f7, [%rd25];
    cvt.f64.f32 %fd5, %f6;
    cvt.f64.f32 %fd6, %f7;
    mul.rn.f64 %fd5, %fd5, %fd6;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.u32 %r23, %r23, 1;
    bra CA_BWD_SUM_DOT_LOOP;
CA_BWD_SUM_DOT_READY:
    cvt.f64.f32 %fd7, %f1;
    mul.rn.f64 %fd4, %fd4, %fd7;
    sub.rn.f64 %fd5, %fd4, %fd18;
    cvt.rn.f32.f64 %f9, %fd5;
    mul.rn.f32 %f9, %f9, 0f3FB8AA3B;
    ex2.approx.f32 %f10, %f9;
    cvt.f64.f32 %fd12, %f10;
    add.rn.f64 %fd19, %fd19, %fd12;
    add.u32 %r20, %r20, 1;
    bra CA_BWD_SUM_COL_LOOP;
CA_BWD_SUM_READY:
    mov.f64 %fd12, 0d3FF0000000000000;
    div.rn.f64 %fd19, %fd12, %fd19;
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r19, 0;
CA_BWD_SOFTMAX_DOT_LOOP:
    setp.ge.u32 %p5, %r19, %r4;
    @%p5 bra CA_BWD_COL_START;
    add.u32 %r25, %r17, %r19;
    mul.wide.u32 %rd19, %r25, 4;
    add.s64 %rd20, %rd5, %rd19;
    add.s64 %rd21, %rd4, %rd19;
    ld.global.f32 %f4, [%rd20];
    ld.global.f32 %f5, [%rd21];
    cvt.f64.f32 %fd2, %f4;
    cvt.f64.f32 %fd3, %f5;
    mul.rn.f64 %fd2, %fd2, %fd3;
    add.rn.f64 %fd1, %fd1, %fd2;
    add.u32 %r19, %r19, 1;
    bra CA_BWD_SOFTMAX_DOT_LOOP;
CA_BWD_COL_START:
    mov.u32 %r20, 0;
CA_BWD_COL_LOOP:
    setp.gt.u32 %p6, %r20, %r15;
    @%p6 bra CA_BWD_NEXT_ROW;
    mad.lo.u32 %r21, %r20, %r3, %r11;
    mad.lo.u32 %r22, %r20, %r4, %r12;
    mov.f64 %fd4, 0d0000000000000000;
    mov.u32 %r23, 0;
CA_BWD_SCORE_LOOP:
    setp.ge.u32 %p7, %r23, %r3;
    @%p7 bra CA_BWD_SCORE_READY;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r21, %r23;
    mul.wide.u32 %rd22, %r24, 4;
    mul.wide.u32 %rd23, %r25, 4;
    add.s64 %rd24, %rd1, %rd22;
    add.s64 %rd25, %rd2, %rd23;
    ld.global.f32 %f6, [%rd24];
    ld.global.f32 %f7, [%rd25];
    cvt.f64.f32 %fd5, %f6;
    cvt.f64.f32 %fd6, %f7;
    mul.rn.f64 %fd5, %fd5, %fd6;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.u32 %r23, %r23, 1;
    bra CA_BWD_SCORE_LOOP;
CA_BWD_SCORE_READY:
    cvt.f64.f32 %fd7, %f1;
    mul.rn.f64 %fd4, %fd4, %fd7;
    sub.rn.f64 %fd5, %fd4, %fd18;
    cvt.rn.f32.f64 %f9, %fd5;
    mul.rn.f32 %f9, %f9, 0f3FB8AA3B;
    ex2.approx.f32 %f10, %f9;
    cvt.f64.f32 %fd12, %f10;
    mul.rn.f64 %fd12, %fd12, %fd19;
    cvt.rn.f32.f64 %f10, %fd12;
    or.b32 %r33, %r30, %r31;
    setp.eq.u32 %p14, %r33, 0;
    @%p14 bra CA_BWD_V_LOOP_START;
    mov.f64 %fd8, 0d0000000000000000;
    mov.u32 %r19, 0;
CA_BWD_DP_LOOP:
    setp.ge.u32 %p8, %r19, %r4;
    @%p8 bra CA_BWD_DP_READY;
    add.u32 %r25, %r17, %r19;
    add.u32 %r26, %r22, %r19;
    mul.wide.u32 %rd26, %r25, 4;
    mul.wide.u32 %rd27, %r26, 4;
    add.s64 %rd28, %rd5, %rd26;
    add.s64 %rd29, %rd3, %rd27;
    ld.global.f32 %f11, [%rd28];
    ld.global.f32 %f12, [%rd29];
    cvt.f64.f32 %fd9, %f11;
    cvt.f64.f32 %fd10, %f12;
    mul.rn.f64 %fd9, %fd9, %fd10;
    add.rn.f64 %fd8, %fd8, %fd9;
    add.u32 %r19, %r19, 1;
    bra CA_BWD_DP_LOOP;
CA_BWD_DP_READY:
    sub.rn.f64 %fd11, %fd8, %fd1;
    cvt.f64.f32 %fd12, %f10;
    mul.rn.f64 %fd11, %fd11, %fd12;
    mul.rn.f64 %fd13, %fd11, %fd7;
    mov.u32 %r23, 0;
CA_BWD_QK_LOOP:
    setp.ge.u32 %p9, %r23, %r3;
    @%p9 bra CA_BWD_V_LOOP_START;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r21, %r23;
    mul.wide.u32 %rd30, %r24, 4;
    mul.wide.u32 %rd31, %r25, 4;
    add.s64 %rd32, %rd1, %rd30;
    add.s64 %rd33, %rd2, %rd31;
    ld.global.f32 %f13, [%rd32];
    ld.global.f32 %f14, [%rd33];
    setp.eq.u32 %p12, %r30, 0;
    @%p12 bra CA_BWD_SKIP_Q;
    add.s64 %rd34, %rd8, %rd30;
    ld.global.f32 %f15, [%rd34];
    cvt.f64.f32 %fd14, %f14;
    mul.rn.f64 %fd14, %fd13, %fd14;
    cvt.f64.f32 %fd15, %f15;
    add.rn.f64 %fd15, %fd15, %fd14;
    cvt.rn.f32.f64 %f15, %fd15;
    st.global.f32 [%rd34], %f15;
CA_BWD_SKIP_Q:
    setp.eq.u32 %p13, %r31, 0;
    @%p13 bra CA_BWD_SKIP_K;
    add.s64 %rd35, %rd9, %rd31;
    ld.global.f32 %f16, [%rd35];
    cvt.f64.f32 %fd16, %f13;
    mul.rn.f64 %fd16, %fd13, %fd16;
    cvt.f64.f32 %fd17, %f16;
    add.rn.f64 %fd17, %fd17, %fd16;
    cvt.rn.f32.f64 %f16, %fd17;
    st.global.f32 [%rd35], %f16;
CA_BWD_SKIP_K:
    add.u32 %r23, %r23, 1;
    bra CA_BWD_QK_LOOP;
CA_BWD_V_LOOP_START:
    setp.eq.u32 %p12, %r32, 0;
    @%p12 bra CA_BWD_NEXT_COL;
    mov.u32 %r19, 0;
CA_BWD_V_LOOP:
    setp.ge.u32 %p10, %r19, %r4;
    @%p10 bra CA_BWD_NEXT_COL;
    add.u32 %r25, %r17, %r19;
    add.u32 %r26, %r22, %r19;
    mul.wide.u32 %rd36, %r25, 4;
    mul.wide.u32 %rd37, %r26, 4;
    add.s64 %rd38, %rd5, %rd36;
    add.s64 %rd39, %rd10, %rd37;
    ld.global.f32 %f17, [%rd38];
    ld.global.f32 %f18, [%rd39];
    mul.rn.f32 %f19, %f10, %f17;
    add.rn.f32 %f18, %f18, %f19;
    st.global.f32 [%rd39], %f18;
    add.u32 %r19, %r19, 1;
    bra CA_BWD_V_LOOP;
CA_BWD_NEXT_COL:
    add.u32 %r20, %r20, 1;
    bra CA_BWD_COL_LOOP;
CA_BWD_NEXT_ROW:
    add.u32 %r15, %r15, 1;
    bra CA_BWD_ROW_LOOP;
CA_BWD_DONE:
    ret;
}

// Parallel deterministic attention backward workspace builder. One warp owns
// one causal row and uses the exact same f64 lane tree as the warp forward for
// every q*k score. Lane zero writes normalized probabilities into a packed
// triangular [batch, T*(T+1)/2] buffer; rows never race one another.
.visible .entry sura_causal_attention_probabilities_f32(
    .param .u64 param_query,
    .param .u64 param_key,
    .param .u64 param_probabilities,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .f32 param_scale,
    .param .u32 param_pairs_per_batch
)
{
    .reg .pred %p<10>;
    .reg .b32 %r<40>;
    .reg .b64 %rd<18>;
    .reg .f32 %f<8>;
    .reg .f64 %fd<12>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_key];
    ld.param.u64 %rd3, [param_probabilities];
    ld.param.u32 %r1, [param_total_rows];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r4, [param_pairs_per_batch];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    shr.u32 %r8, %r6, 5;
    shr.u32 %r9, %r7, 5;
    mad.lo.u32 %r10, %r5, %r8, %r9;
    and.b32 %r11, %r7, 31;
    setp.ge.u32 %p1, %r10, %r1;
    @%p1 bra CAP_DONE;
    div.u32 %r12, %r10, %r2;
    rem.u32 %r13, %r10, %r2;
    mul.lo.u32 %r14, %r2, %r3;
    mul.lo.u32 %r15, %r12, %r14;
    mad.lo.u32 %r16, %r13, %r3, %r15;
    mul.lo.u32 %r17, %r12, %r4;
    and.b32 %r18, %r13, 1;
    setp.eq.u32 %p2, %r18, 0;
    @%p2 bra CAP_ROW_EVEN;
    add.u32 %r19, %r13, 1;
    shr.u32 %r19, %r19, 1;
    mul.lo.u32 %r20, %r13, %r19;
    bra CAP_ROW_START_READY;
CAP_ROW_EVEN:
    shr.u32 %r19, %r13, 1;
    add.u32 %r26, %r13, 1;
    mul.lo.u32 %r20, %r19, %r26;
CAP_ROW_START_READY:
    mov.f64 %fd1, 0dFFF0000000000000;
    mov.f64 %fd8, 0d0000000000000000;
    mov.u32 %r21, 0;
CAP_MAX_COL_LOOP:
    setp.gt.u32 %p3, %r21, %r13;
    @%p3 bra CAP_WEIGHT_START;
    mad.lo.u32 %r22, %r21, %r3, %r15;
    mov.f64 %fd2, 0d0000000000000000;
    mov.u32 %r23, %r11;
CAP_MAX_DOT_LOOP:
    setp.ge.u32 %p4, %r23, %r3;
    @%p4 bra CAP_MAX_DOT_REDUCE;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r22, %r23;
    mul.wide.u32 %rd4, %r24, 4;
    mul.wide.u32 %rd5, %r25, 4;
    add.s64 %rd6, %rd1, %rd4;
    add.s64 %rd7, %rd2, %rd5;
    ld.global.f32 %f2, [%rd6];
    ld.global.f32 %f3, [%rd7];
    cvt.f64.f32 %fd3, %f2;
    cvt.f64.f32 %fd4, %f3;
    mul.rn.f64 %fd3, %fd3, %fd4;
    add.rn.f64 %fd2, %fd2, %fd3;
    add.u32 %r23, %r23, 32;
    bra CAP_MAX_DOT_LOOP;
CAP_MAX_DOT_REDUCE:
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 16, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 16, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 8, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 8, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 4, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 4, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 2, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 2, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 1, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 1, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.u32 %r32, 1;
    setp.ne.u32 %p5, %r11, 0;
    @%p5 bra CAP_MAX_LANE0_READY;
    cvt.f64.f32 %fd5, %f1;
    mul.rn.f64 %fd2, %fd2, %fd5;
    abs.f64 %fd9, %fd2;
    setp.le.f64 %p6, %fd9, 0d7FEFFFFFFFFFFFFF;
    @!%p6 bra CAP_MAX_INVALID_SCORE;
    max.f64 %fd10, %fd1, %fd2;
    setp.eq.u32 %p7, %r21, 0;
    @%p7 bra CAP_MAX_FIRST_COL;
    sub.rn.f64 %fd11, %fd1, %fd10;
    cvt.rn.f32.f64 %f4, %fd11;
    mul.rn.f32 %f4, %f4, 0f3FB8AA3B;
    ex2.approx.f32 %f4, %f4;
    bra CAP_MAX_PREVIOUS_READY;
CAP_MAX_FIRST_COL:
    mov.f32 %f4, 0f00000000;
CAP_MAX_PREVIOUS_READY:
    sub.rn.f64 %fd11, %fd2, %fd10;
    cvt.rn.f32.f64 %f5, %fd11;
    mul.rn.f32 %f5, %f5, 0f3FB8AA3B;
    ex2.approx.f32 %f5, %f5;
    cvt.f64.f32 %fd6, %f4;
    cvt.f64.f32 %fd7, %f5;
    mul.rn.f64 %fd6, %fd8, %fd6;
    add.rn.f64 %fd8, %fd6, %fd7;
    mov.f64 %fd1, %fd10;
    bra CAP_MAX_LANE0_READY;
CAP_MAX_INVALID_SCORE:
    mov.u32 %r32, 0;
CAP_MAX_LANE0_READY:
    shfl.sync.idx.b32 %r33, %r32, 0, 31, 0xffffffff;
    setp.eq.u32 %p8, %r33, 0;
    @%p8 bra CAP_INVALID;
    add.u32 %r21, %r21, 1;
    bra CAP_MAX_COL_LOOP;
CAP_WEIGHT_START:
    mov.u32 %r21, 0;
CAP_WEIGHT_COL_LOOP:
    setp.gt.u32 %p3, %r21, %r13;
    @%p3 bra CAP_DONE;
    mad.lo.u32 %r22, %r21, %r3, %r15;
    mov.f64 %fd2, 0d0000000000000000;
    mov.u32 %r23, %r11;
CAP_WEIGHT_DOT_LOOP:
    setp.ge.u32 %p4, %r23, %r3;
    @%p4 bra CAP_WEIGHT_DOT_REDUCE;
    add.u32 %r24, %r16, %r23;
    add.u32 %r25, %r22, %r23;
    mul.wide.u32 %rd8, %r24, 4;
    mul.wide.u32 %rd9, %r25, 4;
    add.s64 %rd10, %rd1, %rd8;
    add.s64 %rd11, %rd2, %rd9;
    ld.global.f32 %f2, [%rd10];
    ld.global.f32 %f3, [%rd11];
    cvt.f64.f32 %fd3, %f2;
    cvt.f64.f32 %fd4, %f3;
    mul.rn.f64 %fd3, %fd3, %fd4;
    add.rn.f64 %fd2, %fd2, %fd3;
    add.u32 %r23, %r23, 32;
    bra CAP_WEIGHT_DOT_LOOP;
CAP_WEIGHT_DOT_REDUCE:
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 16, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 16, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 8, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 8, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 4, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 4, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 2, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 2, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    mov.b64 {%r28, %r29}, %fd2;
    shfl.sync.down.b32 %r30, %r28, 1, 31, 0xffffffff;
    shfl.sync.down.b32 %r31, %r29, 1, 31, 0xffffffff;
    mov.b64 %fd4, {%r30, %r31};
    add.rn.f64 %fd2, %fd2, %fd4;
    setp.ne.u32 %p5, %r11, 0;
    @%p5 bra CAP_WEIGHT_NEXT;
    cvt.f64.f32 %fd5, %f1;
    mul.rn.f64 %fd2, %fd2, %fd5;
    sub.rn.f64 %fd6, %fd2, %fd1;
    cvt.rn.f32.f64 %f4, %fd6;
    mul.rn.f32 %f4, %f4, 0f3FB8AA3B;
    ex2.approx.f32 %f4, %f4;
    cvt.f64.f32 %fd7, %f4;
    div.rn.f64 %fd7, %fd7, %fd8;
    cvt.rn.f32.f64 %f6, %fd7;
    add.u32 %r34, %r17, %r20;
    add.u32 %r34, %r34, %r21;
    mul.wide.u32 %rd12, %r34, 4;
    add.s64 %rd13, %rd3, %rd12;
    st.global.f32 [%rd13], %f6;
CAP_WEIGHT_NEXT:
    add.u32 %r21, %r21, 1;
    bra CAP_WEIGHT_COL_LOOP;
CAP_INVALID:
    setp.ne.u32 %p9, %r11, 0;
    @%p9 bra CAP_DONE;
    mov.f32 %f7, 0f7FC00000;
    mov.u32 %r21, 0;
CAP_INVALID_LOOP:
    setp.gt.u32 %p3, %r21, %r13;
    @%p3 bra CAP_DONE;
    add.u32 %r34, %r17, %r20;
    add.u32 %r34, %r34, %r21;
    mul.wide.u32 %rd14, %r34, 4;
    add.s64 %rd15, %rd3, %rd14;
    st.global.f32 [%rd15], %f7;
    add.u32 %r21, %r21, 1;
    bra CAP_INVALID_LOOP;
CAP_DONE:
    ret;
}

.visible .entry sura_causal_attention_value_backward_f32(
    .param .u64 param_probabilities,
    .param .u64 param_gradient_output,
    .param .u64 param_gradient_value,
    .param .u32 param_count,
    .param .u32 param_sequence,
    .param .u32 param_value_dimensions,
    .param .u32 param_pairs_per_batch
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<24>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<4>;
    .reg .f64 %fd<5>;
    ld.param.u64 %rd1, [param_probabilities];
    ld.param.u64 %rd2, [param_gradient_output];
    ld.param.u64 %rd3, [param_gradient_value];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_value_dimensions];
    ld.param.u32 %r4, [param_pairs_per_batch];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r1;
    @%p1 bra CAV_DONE;
    rem.u32 %r9, %r8, %r3;
    div.u32 %r10, %r8, %r3;
    rem.u32 %r11, %r10, %r2;
    div.u32 %r12, %r10, %r2;
    mul.lo.u32 %r13, %r12, %r4;
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r14, %r11;
CAV_ROW_LOOP:
    setp.ge.u32 %p2, %r14, %r2;
    @%p2 bra CAV_STORE;
    and.b32 %r15, %r14, 1;
    setp.eq.u32 %p3, %r15, 0;
    @%p3 bra CAV_ROW_EVEN;
    add.u32 %r16, %r14, 1;
    shr.u32 %r16, %r16, 1;
    mul.lo.u32 %r17, %r14, %r16;
    bra CAV_ROW_READY;
CAV_ROW_EVEN:
    shr.u32 %r16, %r14, 1;
    add.u32 %r18, %r14, 1;
    mul.lo.u32 %r17, %r16, %r18;
CAV_ROW_READY:
    add.u32 %r19, %r13, %r17;
    add.u32 %r19, %r19, %r11;
    mul.wide.u32 %rd4, %r19, 4;
    add.s64 %rd5, %rd1, %rd4;
    ld.global.f32 %f1, [%rd5];
    mad.lo.u32 %r20, %r12, %r2, %r14;
    mad.lo.u32 %r20, %r20, %r3, %r9;
    mul.wide.u32 %rd6, %r20, 4;
    add.s64 %rd7, %rd2, %rd6;
    ld.global.f32 %f2, [%rd7];
    cvt.f64.f32 %fd2, %f1;
    cvt.f64.f32 %fd3, %f2;
    mul.rn.f64 %fd2, %fd2, %fd3;
    add.rn.f64 %fd1, %fd1, %fd2;
    add.u32 %r14, %r14, 1;
    bra CAV_ROW_LOOP;
CAV_STORE:
    cvt.rn.f32.f64 %f3, %fd1;
    mul.wide.u32 %rd8, %r8, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f3;
CAV_DONE:
    ret;
}

.visible .entry sura_causal_attention_score_backward_f32(
    .param .u64 param_value,
    .param .u64 param_output,
    .param .u64 param_gradient_output,
    .param .u64 param_probabilities,
    .param .u64 param_score_gradient,
    .param .u32 param_total_rows,
    .param .u32 param_sequence,
    .param .u32 param_value_dimensions,
    .param .f32 param_scale,
    .param .u32 param_pairs_per_batch
)
{
    .reg .pred %p<5>;
    .reg .b32 %r<28>;
    .reg .b64 %rd<20>;
    .reg .f32 %f<8>;
    .reg .f64 %fd<12>;
    ld.param.u64 %rd1, [param_value];
    ld.param.u64 %rd2, [param_output];
    ld.param.u64 %rd3, [param_gradient_output];
    ld.param.u64 %rd4, [param_probabilities];
    ld.param.u64 %rd5, [param_score_gradient];
    ld.param.u32 %r1, [param_total_rows];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_value_dimensions];
    ld.param.f32 %f1, [param_scale];
    ld.param.u32 %r4, [param_pairs_per_batch];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r1;
    @%p1 bra CADS_DONE;
    div.u32 %r9, %r8, %r2;
    rem.u32 %r10, %r8, %r2;
    mul.lo.u32 %r11, %r2, %r3;
    mul.lo.u32 %r12, %r9, %r11;
    mad.lo.u32 %r13, %r10, %r3, %r12;
    mul.lo.u32 %r14, %r9, %r4;
    and.b32 %r15, %r10, 1;
    setp.eq.u32 %p2, %r15, 0;
    @%p2 bra CADS_ROW_EVEN;
    add.u32 %r16, %r10, 1;
    shr.u32 %r16, %r16, 1;
    mul.lo.u32 %r17, %r10, %r16;
    bra CADS_ROW_READY;
CADS_ROW_EVEN:
    shr.u32 %r16, %r10, 1;
    add.u32 %r18, %r10, 1;
    mul.lo.u32 %r17, %r16, %r18;
CADS_ROW_READY:
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r21, 0;
CADS_SOFT_COL_LOOP:
    setp.gt.u32 %p4, %r21, %r10;
    @%p4 bra CADS_COL_START;
    mad.lo.u32 %r22, %r21, %r3, %r12;
    mov.f64 %fd4, 0d0000000000000000;
    mov.u32 %r19, 0;
CADS_SOFT_DP_LOOP:
    setp.ge.u32 %p3, %r19, %r3;
    @%p3 bra CADS_SOFT_DP_READY;
    add.u32 %r20, %r13, %r19;
    add.u32 %r23, %r22, %r19;
    mul.wide.u32 %rd6, %r20, 4;
    mul.wide.u32 %rd7, %r23, 4;
    add.s64 %rd8, %rd3, %rd6;
    add.s64 %rd9, %rd1, %rd7;
    ld.global.f32 %f2, [%rd8];
    ld.global.f32 %f3, [%rd9];
    cvt.f64.f32 %fd2, %f2;
    cvt.f64.f32 %fd3, %f3;
    mul.rn.f64 %fd2, %fd2, %fd3;
    add.rn.f64 %fd4, %fd4, %fd2;
    add.u32 %r19, %r19, 1;
    bra CADS_SOFT_DP_LOOP;
CADS_SOFT_DP_READY:
    add.u32 %r24, %r14, %r17;
    add.u32 %r24, %r24, %r21;
    mul.wide.u32 %rd10, %r24, 4;
    add.s64 %rd11, %rd4, %rd10;
    ld.global.f32 %f6, [%rd11];
    cvt.f64.f32 %fd5, %f6;
    mul.rn.f64 %fd5, %fd5, %fd4;
    add.rn.f64 %fd1, %fd1, %fd5;
    add.u32 %r21, %r21, 1;
    bra CADS_SOFT_COL_LOOP;
CADS_COL_START:
    mov.u32 %r21, 0;
CADS_COL_LOOP:
    setp.gt.u32 %p4, %r21, %r10;
    @%p4 bra CADS_DONE;
    mad.lo.u32 %r22, %r21, %r3, %r12;
    mov.f64 %fd4, 0d0000000000000000;
    mov.u32 %r19, 0;
CADS_DP_LOOP:
    setp.ge.u32 %p3, %r19, %r3;
    @%p3 bra CADS_DP_READY;
    add.u32 %r20, %r13, %r19;
    add.u32 %r23, %r22, %r19;
    mul.wide.u32 %rd9, %r20, 4;
    mul.wide.u32 %rd10, %r23, 4;
    add.s64 %rd11, %rd3, %rd9;
    add.s64 %rd12, %rd1, %rd10;
    ld.global.f32 %f4, [%rd11];
    ld.global.f32 %f5, [%rd12];
    cvt.f64.f32 %fd5, %f4;
    cvt.f64.f32 %fd6, %f5;
    mul.rn.f64 %fd5, %fd5, %fd6;
    add.rn.f64 %fd4, %fd4, %fd5;
    add.u32 %r19, %r19, 1;
    bra CADS_DP_LOOP;
CADS_DP_READY:
    add.u32 %r24, %r14, %r17;
    add.u32 %r24, %r24, %r21;
    mul.wide.u32 %rd13, %r24, 4;
    add.s64 %rd14, %rd4, %rd13;
    add.s64 %rd15, %rd5, %rd13;
    ld.global.f32 %f6, [%rd14];
    sub.rn.f64 %fd7, %fd4, %fd1;
    cvt.f64.f32 %fd8, %f6;
    mul.rn.f64 %fd7, %fd7, %fd8;
    cvt.f64.f32 %fd9, %f1;
    mul.rn.f64 %fd7, %fd7, %fd9;
    cvt.rn.f32.f64 %f7, %fd7;
    st.global.f32 [%rd15], %f7;
    add.u32 %r21, %r21, 1;
    bra CADS_COL_LOOP;
CADS_DONE:
    ret;
}

.visible .entry sura_causal_attention_query_backward_f32(
    .param .u64 param_key,
    .param .u64 param_score_gradient,
    .param .u64 param_gradient_query,
    .param .u32 param_count,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_pairs_per_batch
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<24>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<4>;
    .reg .f64 %fd<5>;
    ld.param.u64 %rd1, [param_key];
    ld.param.u64 %rd2, [param_score_gradient];
    ld.param.u64 %rd3, [param_gradient_query];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_pairs_per_batch];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r1;
    @%p1 bra CAQ_DONE;
    rem.u32 %r9, %r8, %r3;
    div.u32 %r10, %r8, %r3;
    rem.u32 %r11, %r10, %r2;
    div.u32 %r12, %r10, %r2;
    mul.lo.u32 %r13, %r12, %r4;
    mul.lo.u32 %r14, %r2, %r3;
    mul.lo.u32 %r15, %r12, %r14;
    and.b32 %r16, %r11, 1;
    setp.eq.u32 %p2, %r16, 0;
    @%p2 bra CAQ_ROW_EVEN;
    add.u32 %r17, %r11, 1;
    shr.u32 %r17, %r17, 1;
    mul.lo.u32 %r18, %r11, %r17;
    bra CAQ_ROW_READY;
CAQ_ROW_EVEN:
    shr.u32 %r17, %r11, 1;
    add.u32 %r19, %r11, 1;
    mul.lo.u32 %r18, %r17, %r19;
CAQ_ROW_READY:
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r20, 0;
CAQ_COL_LOOP:
    setp.gt.u32 %p3, %r20, %r11;
    @%p3 bra CAQ_STORE;
    add.u32 %r21, %r13, %r18;
    add.u32 %r21, %r21, %r20;
    mul.wide.u32 %rd4, %r21, 4;
    add.s64 %rd5, %rd2, %rd4;
    ld.global.f32 %f1, [%rd5];
    mad.lo.u32 %r22, %r20, %r3, %r15;
    add.u32 %r22, %r22, %r9;
    mul.wide.u32 %rd6, %r22, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f2, [%rd7];
    cvt.f64.f32 %fd2, %f1;
    cvt.f64.f32 %fd3, %f2;
    mul.rn.f64 %fd2, %fd2, %fd3;
    add.rn.f64 %fd1, %fd1, %fd2;
    add.u32 %r20, %r20, 1;
    bra CAQ_COL_LOOP;
CAQ_STORE:
    cvt.rn.f32.f64 %f3, %fd1;
    mul.wide.u32 %rd8, %r8, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f3;
CAQ_DONE:
    ret;
}

.visible .entry sura_causal_attention_key_backward_f32(
    .param .u64 param_query,
    .param .u64 param_score_gradient,
    .param .u64 param_gradient_key,
    .param .u32 param_count,
    .param .u32 param_sequence,
    .param .u32 param_dimensions,
    .param .u32 param_pairs_per_batch
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<24>;
    .reg .b64 %rd<14>;
    .reg .f32 %f<4>;
    .reg .f64 %fd<5>;
    ld.param.u64 %rd1, [param_query];
    ld.param.u64 %rd2, [param_score_gradient];
    ld.param.u64 %rd3, [param_gradient_key];
    ld.param.u32 %r1, [param_count];
    ld.param.u32 %r2, [param_sequence];
    ld.param.u32 %r3, [param_dimensions];
    ld.param.u32 %r4, [param_pairs_per_batch];
    mov.u32 %r5, %ctaid.x;
    mov.u32 %r6, %ntid.x;
    mov.u32 %r7, %tid.x;
    mad.lo.s32 %r8, %r5, %r6, %r7;
    setp.ge.u32 %p1, %r8, %r1;
    @%p1 bra CAK_DONE;
    rem.u32 %r9, %r8, %r3;
    div.u32 %r10, %r8, %r3;
    rem.u32 %r11, %r10, %r2;
    div.u32 %r12, %r10, %r2;
    mul.lo.u32 %r13, %r12, %r4;
    mul.lo.u32 %r14, %r2, %r3;
    mul.lo.u32 %r15, %r12, %r14;
    mov.f64 %fd1, 0d0000000000000000;
    mov.u32 %r16, %r11;
CAK_ROW_LOOP:
    setp.ge.u32 %p2, %r16, %r2;
    @%p2 bra CAK_STORE;
    and.b32 %r17, %r16, 1;
    setp.eq.u32 %p3, %r17, 0;
    @%p3 bra CAK_ROW_EVEN;
    add.u32 %r18, %r16, 1;
    shr.u32 %r18, %r18, 1;
    mul.lo.u32 %r19, %r16, %r18;
    bra CAK_ROW_READY;
CAK_ROW_EVEN:
    shr.u32 %r18, %r16, 1;
    add.u32 %r20, %r16, 1;
    mul.lo.u32 %r19, %r18, %r20;
CAK_ROW_READY:
    add.u32 %r21, %r13, %r19;
    add.u32 %r21, %r21, %r11;
    mul.wide.u32 %rd4, %r21, 4;
    add.s64 %rd5, %rd2, %rd4;
    ld.global.f32 %f1, [%rd5];
    mad.lo.u32 %r22, %r16, %r3, %r15;
    add.u32 %r22, %r22, %r9;
    mul.wide.u32 %rd6, %r22, 4;
    add.s64 %rd7, %rd1, %rd6;
    ld.global.f32 %f2, [%rd7];
    cvt.f64.f32 %fd2, %f1;
    cvt.f64.f32 %fd3, %f2;
    mul.rn.f64 %fd2, %fd2, %fd3;
    add.rn.f64 %fd1, %fd1, %fd2;
    add.u32 %r16, %r16, 1;
    bra CAK_ROW_LOOP;
CAK_STORE:
    cvt.rn.f32.f64 %f3, %fd1;
    mul.wide.u32 %rd8, %r8, 4;
    add.s64 %rd9, %rd3, %rd8;
    st.global.f32 [%rd9], %f3;
CAK_DONE:
    ret;
}

// Correctness-first reduction.  It intentionally uses one GPU thread so the
// result has a stable order; a parallel two-stage reduction is still needed
// for production throughput.
.visible .entry sura_sum_f32(
    .param .u64 param_in,
    .param .u64 param_out,
    .param .u32 param_count
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<6>;
    .reg .b64 %rd<7>;
    .reg .f32 %f<3>;
    mov.u32 %r1, %ctaid.x;
    setp.ne.u32 %p1, %r1, 0;
    @%p1 bra SUM_DONE;
    mov.u32 %r2, %tid.x;
    setp.ne.u32 %p2, %r2, 0;
    @%p2 bra SUM_DONE;
    ld.param.u64 %rd1, [param_in];
    ld.param.u64 %rd2, [param_out];
    ld.param.u32 %r3, [param_count];
    mov.u32 %r4, 0;
    mov.f32 %f1, 0f00000000;
SUM_LOOP:
    setp.ge.u32 %p3, %r4, %r3;
    @%p3 bra SUM_STORE;
    mul.wide.u32 %rd3, %r4, 4;
    add.s64 %rd4, %rd1, %rd3;
    ld.global.f32 %f2, [%rd4];
    add.rn.f32 %f1, %f1, %f2;
    add.u32 %r4, %r4, 1;
    bra SUM_LOOP;
SUM_STORE:
    st.global.f32 [%rd2], %f1;
SUM_DONE:
    ret;
}
)ptx";

    std::string cublas_status_text(CublasStatus status) const {
        if (cublasGetStatusString_) {
            const char* text = cublasGetStatusString_(status);
            if (text && *text) return text;
        }
        switch (status) {
            case 0: return "CUBLAS_STATUS_SUCCESS";
            case 1: return "CUBLAS_STATUS_NOT_INITIALIZED";
            case 3: return "CUBLAS_STATUS_ALLOC_FAILED";
            case 7: return "CUBLAS_STATUS_INVALID_VALUE";
            case 8: return "CUBLAS_STATUS_ARCH_MISMATCH";
            case 11: return "CUBLAS_STATUS_MAPPING_ERROR";
            case 13: return "CUBLAS_STATUS_EXECUTION_FAILED";
            case 14: return "CUBLAS_STATUS_INTERNAL_ERROR";
            case 15: return "CUBLAS_STATUS_NOT_SUPPORTED";
            case 16: return "CUBLAS_STATUS_LICENSE_ERROR";
            default: return "cuBLAS status " + std::to_string(status);
        }
    }

    void close_cublas_locked() {
        if (cublas_handle_ && cublasDestroy_) cublasDestroy_(cublas_handle_);
        cublas_handle_ = nullptr;
        cublasCreate_ = nullptr;
        cublasDestroy_ = nullptr;
        cublasSgemm_ = nullptr;
        cublasGemmEx_ = nullptr;
        cublasGetStatusString_ = nullptr;
#ifdef _WIN32
        if (cublas_library_) FreeLibrary(cublas_library_);
#else
        if (cublas_library_) dlclose(cublas_library_);
#endif
        cublas_library_ = nullptr;
        cublas_library_name_.clear();
    }

    template<typename Function>
    bool load_cublas_symbol_locked(Function& destination, const char* name,
                                   bool required = true) {
#ifdef _WIN32
        destination = reinterpret_cast<Function>(
            GetProcAddress(cublas_library_, name));
#else
        destination = reinterpret_cast<Function>(dlsym(cublas_library_, name));
#endif
        if (destination || !required) return true;
        cublas_error_ = std::string("cuBLAS symbol is unavailable: ") + name;
        return false;
    }

    bool bind_open_cublas_locked(const std::string& display_name) {
        if (!load_cublas_symbol_locked(cublasCreate_, "cublasCreate_v2")
            || !load_cublas_symbol_locked(cublasDestroy_, "cublasDestroy_v2")
            || !load_cublas_symbol_locked(cublasSgemm_, "cublasSgemm_v2")) {
            close_cublas_locked();
            return false;
        }
        load_cublas_symbol_locked(cublasGetStatusString_,
                                  "cublasGetStatusString", false);
        load_cublas_symbol_locked(cublasGemmEx_, "cublasGemmEx", false);
        CublasStatus status = cublasCreate_(&cublas_handle_);
        if (status != CUBLAS_STATUS_SUCCESS || !cublas_handle_) {
            cublas_error_ = "cublasCreate_v2 failed: "
                + cublas_status_text(status);
            close_cublas_locked();
            return false;
        }
        cublas_library_name_ = display_name;
        cublas_error_.clear();
        return true;
    }

#ifdef _WIN32
    static std::wstring windows_environment_value(const wchar_t* name) {
        DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return {};
        std::wstring value(required, L'\0');
        DWORD written = GetEnvironmentVariableW(name, &value[0], required);
        if (written == 0 || written >= required) return {};
        value.resize(written);
        return value;
    }

    static std::string utf8_from_wide(const std::wstring& value) {
        if (value.empty()) return {};
        int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           (int)value.size(), nullptr, 0,
                                           nullptr, nullptr);
        if (required <= 0) return "<unprintable path>";
        std::string result((size_t)required, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(),
                            &result[0], required, nullptr, nullptr);
        return result;
    }

    static std::wstring windows_full_path(const std::wstring& path) {
        DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (required == 0) return path;
        std::wstring result(required, L'\0');
        DWORD written = GetFullPathNameW(path.c_str(), required,
                                         &result[0], nullptr);
        if (written == 0 || written >= required) return path;
        result.resize(written);
        return result;
    }

    static std::wstring windows_join_path(const std::wstring& directory,
                                          const wchar_t* leaf) {
        if (directory.empty()) return std::wstring(leaf);
        wchar_t end = directory.back();
        return directory + (end == L'\\' || end == L'/' ? L"" : L"\\")
            + leaf;
    }

    static bool windows_regular_file(const std::wstring& path) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool open_cublas_full_path_locked(const std::wstring& path) {
        std::wstring full = windows_full_path(path);
        cublas_library_ = LoadLibraryExW(full.c_str(), nullptr,
                                         LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!cublas_library_) {
            cublas_error_ = "could not load cuBLAS library '"
                + utf8_from_wide(full) + "' (Windows error "
                + std::to_string((unsigned long)GetLastError()) + ")";
            return false;
        }
        return bind_open_cublas_locked(utf8_from_wide(full));
    }

    bool open_cublas_default_search_locked(const wchar_t* name) {
        cublas_library_ = LoadLibraryExW(name, nullptr,
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!cublas_library_) return false;
        return bind_open_cublas_locked(utf8_from_wide(name));
    }
#else
    bool open_cublas_locked(const char* name) {
        dlerror();
        cublas_library_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!cublas_library_) {
            const char* detail = dlerror();
            cublas_error_ = std::string("could not load cuBLAS library '")
                + name + "'" + (detail ? std::string(": ") + detail : "");
            return false;
        }
        return bind_open_cublas_locked(name);
    }
#endif

    // Optional initialization never changes the primary CUDA error.  Failure
    // means that matmul stays on Sura's resident PTX kernel, not on the CPU.
    void initialize_optional_cublas_locked() {
        cublas_disabled_ = false;
        cublas_error_.clear();
        if (const char* disabled = std::getenv("SURA_CUBLAS_DISABLE")) {
            if (std::strcmp(disabled, "1") == 0) {
                cublas_disabled_ = true;
                cublas_error_ = "cuBLAS disabled by SURA_CUBLAS_DISABLE=1";
                return;
            }
        }
#ifdef _WIN32
        const wchar_t* names[] = {
            L"cublas64_13.dll", L"cublas64_12.dll", L"cublas64_11.dll"
        };
        std::wstring explicit_path =
            windows_environment_value(L"SURA_CUBLAS_LIBRARY");
        if (!explicit_path.empty()) {
            open_cublas_full_path_locked(explicit_path);
            return;
        }

        // Secure loader search covers the executable directory, System32,
        // and directories registered with AddDllDirectory.
        for (const wchar_t* name : names) {
            if (open_cublas_default_search_locked(name)) return;
        }

        std::vector<std::wstring> directories;
        std::wstring cuda_path = windows_environment_value(L"CUDA_PATH");
        if (!cuda_path.empty()) directories.push_back(
            windows_join_path(cuda_path, L"bin"));

        // Search PATH explicitly and load the resulting full path.  This
        // avoids adding the current working directory to the DLL search.
        std::wstring path = windows_environment_value(L"PATH");
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find(L';', start);
            if (end == std::wstring::npos) end = path.size();
            std::wstring directory = path.substr(start, end - start);
            while (!directory.empty()
                   && (directory.front() == L' ' || directory.front() == L'\"')) {
                directory.erase(directory.begin());
            }
            while (!directory.empty()
                   && (directory.back() == L' ' || directory.back() == L'\"')) {
                directory.pop_back();
            }
            if (!directory.empty()) directories.push_back(directory);
            if (end == path.size()) break;
            start = end + 1;
        }
        for (const std::wstring& directory : directories) {
            for (const wchar_t* name : names) {
                std::wstring candidate = windows_join_path(directory, name);
                if (windows_regular_file(candidate)
                    && open_cublas_full_path_locked(candidate)) return;
            }
        }
        cublas_error_ = "cuBLAS was not found in the executable directory, "
            "CUDA_PATH, or PATH; set SURA_CUBLAS_LIBRARY to an explicit DLL";
#else
        if (const char* explicit_path = std::getenv("SURA_CUBLAS_LIBRARY")) {
            if (*explicit_path) {
                open_cublas_locked(explicit_path);
                return;
            }
        }
        const char* names[] = {
            "libcublas.so.13", "libcublas.so.12", "libcublas.so.11",
            "libcublas.so"
        };
        for (const char* name : names) {
            if (open_cublas_locked(name)) return;
        }
        cublas_error_ = "cuBLAS was not found by the dynamic loader; set "
            "SURA_CUBLAS_LIBRARY to an explicit shared library";
#endif
    }

    template<typename Function>
    bool load_symbol(Function& destination, const char* name) {
#ifdef _WIN32
        destination = reinterpret_cast<Function>(GetProcAddress(library_, name));
#else
        destination = reinterpret_cast<Function>(dlsym(library_, name));
#endif
        if (destination) return true;
        error_ = std::string("CUDA driver symbol is unavailable: ") + name;
        return false;
    }

    std::string result_text(CUresult result) const {
        const char* name = nullptr;
        const char* description = nullptr;
        if (cuGetErrorName_) cuGetErrorName_(result, &name);
        if (cuGetErrorString_) cuGetErrorString_(result, &description);
        std::string text = name ? name : ("CUDA error " + std::to_string(result));
        if (description && *description) text += std::string(": ") + description;
        return text;
    }

    bool require(CUresult result, const char* operation) {
        if (result == CUDA_SUCCESS) return true;
        error_ = std::string(operation) + " failed: " + result_text(result);
        return false;
    }

    enum class KernelKind {
        MATMUL, ELEMENTWISE, RELU, GELU, LAYER_NORM, EMBEDDING, CROSS_ENTROPY,
        ATTENTION_REFERENCE, ATTENTION_PARALLEL, ATTENTION_WARP,
        ATTENTION_FUSED,
        TRANSPOSE, REDUCTION, OPTIMIZER, CONVERSION
    };

    void add_counter_locked(uint64_t& counter, uint64_t amount) {
        if (amount > std::numeric_limits<uint64_t>::max() - counter) {
            counter = std::numeric_limits<uint64_t>::max();
            stats_.counter_overflow = true;
        } else {
            counter += amount;
        }
    }

    void record_allocation_locked(size_t bytes) {
        if (bytes > std::numeric_limits<size_t>::max() - stats_.allocated_bytes) {
            stats_.allocated_bytes = std::numeric_limits<size_t>::max();
            stats_.counter_overflow = true;
        } else {
            stats_.allocated_bytes += bytes;
        }
        if (stats_.allocated_bytes > stats_.peak_allocated_bytes) {
            stats_.peak_allocated_bytes = stats_.allocated_bytes;
        }
        add_counter_locked(stats_.allocation_calls, 1);
    }

    void record_free_locked(size_t bytes) {
        if (bytes > stats_.allocated_bytes) {
            stats_.allocated_bytes = 0;
            stats_.counter_overflow = true;
        } else {
            stats_.allocated_bytes -= bytes;
        }
        add_counter_locked(stats_.free_calls, 1);
    }

    void record_kernel_locked(KernelKind kind) {
        add_counter_locked(stats_.kernel_launches, 1);
        switch (kind) {
            case KernelKind::MATMUL:
                add_counter_locked(stats_.matmul_launches, 1); break;
            case KernelKind::ELEMENTWISE:
                add_counter_locked(stats_.elementwise_launches, 1); break;
            case KernelKind::RELU:
                add_counter_locked(stats_.relu_launches, 1); break;
            case KernelKind::GELU:
                add_counter_locked(stats_.gelu_launches, 1); break;
            case KernelKind::LAYER_NORM:
                add_counter_locked(stats_.layer_norm_launches, 1); break;
            case KernelKind::EMBEDDING:
                add_counter_locked(stats_.embedding_launches, 1); break;
            case KernelKind::CROSS_ENTROPY:
                add_counter_locked(stats_.cross_entropy_launches, 1); break;
            case KernelKind::ATTENTION_REFERENCE:
                add_counter_locked(stats_.attention_launches, 1);
                add_counter_locked(stats_.reference_attention_launches, 1); break;
            case KernelKind::ATTENTION_PARALLEL:
                add_counter_locked(stats_.attention_launches, 1);
                add_counter_locked(stats_.parallel_attention_launches, 1); break;
            case KernelKind::ATTENTION_WARP:
                add_counter_locked(stats_.attention_launches, 1);
                add_counter_locked(stats_.warp_attention_launches, 1); break;
            case KernelKind::ATTENTION_FUSED:
                add_counter_locked(stats_.attention_launches, 1);
                add_counter_locked(stats_.fused_attention_launches, 1); break;
            case KernelKind::TRANSPOSE:
                add_counter_locked(stats_.transpose_launches, 1); break;
            case KernelKind::REDUCTION:
                add_counter_locked(stats_.reduction_launches, 1); break;
            case KernelKind::OPTIMIZER:
                add_counter_locked(stats_.optimizer_launches, 1); break;
            case KernelKind::CONVERSION:
                add_counter_locked(stats_.storage_conversion_launches, 1); break;
        }
    }

    void record_matmul_locked(bool used_cublas, MatmulCompute compute,
                              bool fast_cublas = false,
                              bool mixed_fallback = false,
                              bool typed_storage = false) {
        record_kernel_locked(KernelKind::MATMUL);
        add_counter_locked(used_cublas ? stats_.cublas_matmul_launches
                                       : stats_.reference_matmul_launches,
                           1);
        switch (compute) {
            case MatmulCompute::FLOAT32:
                add_counter_locked(stats_.float32_matmul_launches, 1); break;
            case MatmulCompute::FLOAT16:
                add_counter_locked(stats_.float16_matmul_launches, 1); break;
            case MatmulCompute::BFLOAT16:
                add_counter_locked(stats_.bfloat16_matmul_launches, 1); break;
        }
        if (fast_cublas) add_counter_locked(stats_.cublas_fast_matmul_launches, 1);
        if (mixed_fallback) {
            add_counter_locked(stats_.mixed_matmul_fallback_launches, 1);
        }
        if (typed_storage) {
            add_counter_locked(stats_.typed_storage_matmul_launches, 1);
        }
    }

    bool prepare_locked() {
        if (!initialize_locked()) return false;
        error_.clear();
        return require(cuCtxSetCurrent_(context_), "cuCtxSetCurrent");
    }

    static size_t storage_element_size(TensorStorage storage) {
        switch (storage) {
            case TensorStorage::FLOAT32:
            case TensorStorage::UINT32:
                return sizeof(uint32_t);
            case TensorStorage::FLOAT16:
            case TensorStorage::BFLOAT16:
                return sizeof(uint16_t);
        }
        return 0;
    }

    bool kernel_count_locked(size_t count, uint32_t& out, const char* operation) {
        if (count == 0) {
            error_ = std::string(operation) + " requires at least one element";
            return false;
        }
        if (count > (size_t)std::numeric_limits<uint32_t>::max()) {
            error_ = std::string(operation) + " exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        out = (uint32_t)count;
        return true;
    }

    bool matrix_sizes_locked(uint32_t rows, uint32_t cols, uint32_t inner,
                             size_t& left_elements, size_t& right_elements,
                             size_t& output_elements, const char* operation) {
        if (rows == 0 || cols == 0 || inner == 0) {
            error_ = std::string(operation) + " requires non-zero matrix dimensions";
            return false;
        }
        const uint64_t left = (uint64_t)rows * inner;
        const uint64_t right = (uint64_t)inner * cols;
        const uint64_t output = (uint64_t)rows * cols;
        const uint64_t limit = std::numeric_limits<uint32_t>::max();
        if (left > limit || right > limit || output > limit) {
            error_ = std::string(operation)
                + " exceeds the uint32 row-major CUDA kernel index limit";
            return false;
        }
        left_elements = (size_t)left;
        right_elements = (size_t)right;
        output_elements = (size_t)output;
        return true;
    }

    bool resolve_storage_range_locked(DeviceHandle handle, size_t offset, size_t count,
                                      TensorStorage expected_storage,
                                      CUdeviceptr& pointer, const char* operation) {
        auto found = allocations_.find(handle);
        if (handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = std::string(operation) + " received an invalid or freed CUDA handle";
            return false;
        }
        if (count == 0) {
            error_ = std::string(operation) + " requires a non-zero transfer/operation size";
            return false;
        }
        if (offset > found->second.elements
            || count > found->second.elements - offset) {
            error_ = std::string(operation) + " exceeds the CUDA allocation bounds";
            return false;
        }
        if (found->second.storage != expected_storage) {
            error_ = std::string(operation) + " received a CUDA allocation with the wrong storage type";
            return false;
        }
        const size_t element_bytes = storage_element_size(found->second.storage);
        if (element_bytes == 0) {
            error_ = std::string(operation) + " found an invalid CUDA storage type";
            return false;
        }
        // Typed allocators cap elements at uint32 max, so this byte offset
        // cannot overflow either size_t or the 64-bit CUDA device address.
        pointer = found->second.pointer + (CUdeviceptr)(offset * element_bytes);
        return true;
    }


    bool resolve_range_locked(DeviceHandle handle, size_t offset, size_t count,
                              CUdeviceptr& pointer, const char* operation) {
        return resolve_storage_range_locked(handle, offset, count,
                                            TensorStorage::FLOAT32,
                                            pointer, operation);
    }

    bool allocate_storage_locked(size_t elements, TensorStorage storage,
                                 DeviceHandle& out_handle,
                                 const char* operation) {
        if (!prepare_locked()) return false;
        if (out_handle != INVALID_DEVICE_HANDLE) {
            error_ = std::string(operation)
                + " refuses to overwrite a non-zero CUDA handle";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(elements, ignored_count, operation)) return false;
        const size_t element_bytes = storage_element_size(storage);
        if (element_bytes == 0) {
            error_ = std::string(operation) + " received an invalid CUDA storage type";
            return false;
        }
        if (elements > std::numeric_limits<size_t>::max() / element_bytes) {
            error_ = std::string(operation) + " byte size overflow";
            return false;
        }
        const size_t bytes = elements * element_bytes;
        if (total_memory_ != 0
            && (stats_.allocated_bytes > total_memory_
                || bytes > total_memory_ - stats_.allocated_bytes)) {
            error_ = std::string(operation)
                + " exceeds the CUDA device total-memory preflight limit";
            return false;
        }
        CUdeviceptr pointer = 0;
        if (!require(cuMemAlloc_(&pointer, bytes), "cuMemAlloc(typed storage)")) {
            return false;
        }
        if (next_handle_ == INVALID_DEVICE_HANDLE) {
            cuMemFree_(pointer);
            error_ = "CUDA opaque handle space is exhausted";
            return false;
        }
        const DeviceHandle handle = next_handle_++;
        try {
            allocations_.emplace(handle, DeviceAllocation{pointer, elements, storage});
        } catch (...) {
            cuMemFree_(pointer);
            error_ = "host allocation failed while recording a CUDA buffer";
            return false;
        }
        record_allocation_locked(bytes);
        out_handle = handle;
        return true;
    }

    bool launch_1d_locked(CUfunction function, void** arguments,
                          uint32_t count, const char* operation,
                          KernelKind kind = KernelKind::ELEMENTWISE) {
        constexpr unsigned block = 256;
        const unsigned grid = count / block + (count % block != 0 ? 1u : 0u);
        if (!require(cuLaunchKernel_(function, grid, 1, 1, block, 1, 1,
                                    0, nullptr, arguments, nullptr), operation)) {
            return false;
        }
        record_kernel_locked(kind);
        return true;
    }

    bool launch_warp_rows_locked(
            CUfunction function, void** arguments, uint32_t rows,
            const char* operation,
            KernelKind kind = KernelKind::ATTENTION_WARP) {
        constexpr unsigned block = 128;
        constexpr unsigned warps_per_block = block / 32;
        const unsigned grid = rows / warps_per_block
            + (rows % warps_per_block != 0 ? 1u : 0u);
        if (!require(cuLaunchKernel_(function, grid, 1, 1, block, 1, 1,
                                    0, nullptr, arguments, nullptr), operation)) {
            return false;
        }
        record_kernel_locked(kind);
        return true;
    }

    bool launch_matmul_ex_locked(CUdeviceptr left, CUdeviceptr right,
                                 CUdeviceptr output, uint32_t rows,
                                 uint32_t cols, uint32_t inner,
                                 bool transpose_left, bool transpose_right,
                                 MatmulCompute compute,
                                 const char* operation) {
        // Sura tensors are row-major while cuBLAS's classic GEMM interface is
        // column-major.  Computing C^T = op(B)^T * op(A)^T preserves the
        // existing row-major bytes without staging or a transpose kernel.
        // The transpose flags carry over after swapping the operands because
        // a row-major [r,c] buffer is its [c,r] column-major transpose.
        if (cublas_handle_ && rows <= (uint32_t)std::numeric_limits<int>::max()
            && cols <= (uint32_t)std::numeric_limits<int>::max()
            && inner <= (uint32_t)std::numeric_limits<int>::max()) {
            const int m = (int)cols;
            const int n = (int)rows;
            const int k = (int)inner;
            const int trans_a = transpose_right ? CUBLAS_OP_T : CUBLAS_OP_N;
            const int trans_b = transpose_left ? CUBLAS_OP_T : CUBLAS_OP_N;
            const int lda = transpose_right ? k : m;
            const int ldb = transpose_left ? n : k;
            const int ldc = m;
            const float alpha = 1.0f;
            const float beta = 0.0f;
            const void* a = reinterpret_cast<const void*>((uintptr_t)right);
            const void* b = reinterpret_cast<const void*>((uintptr_t)left);
            void* c = reinterpret_cast<void*>((uintptr_t)output);
            if (compute == MatmulCompute::FLOAT32) {
                CublasStatus status = cublasSgemm_(
                    cublas_handle_, trans_a, trans_b, m, n, k,
                    &alpha, static_cast<const float*>(a), lda,
                    static_cast<const float*>(b), ldb, &beta,
                    static_cast<float*>(c), ldc);
                if (status != CUBLAS_STATUS_SUCCESS) {
                    error_ = std::string(operation) + " via cublasSgemm_v2 failed: "
                        + cublas_status_text(status);
                    return false;
                }
                record_matmul_locked(true, compute);
                return true;
            }
            const bool tensor_core_arch = compute == MatmulCompute::FLOAT16
                ? compute_major_ >= 7 : compute_major_ >= 8;
            if (cublasGemmEx_ && tensor_core_arch) {
                const int compute_type = compute == MatmulCompute::FLOAT16
                    ? CUBLAS_COMPUTE_32F_FAST_16F
                    : CUBLAS_COMPUTE_32F_FAST_16BF;
                CublasStatus status = cublasGemmEx_(
                    cublas_handle_, trans_a, trans_b, m, n, k,
                    &alpha, a, CUDA_R_32F, lda,
                    b, CUDA_R_32F, ldb, &beta,
                    c, CUDA_R_32F, ldc, compute_type,
                    CUBLAS_GEMM_DEFAULT);
                if (status == CUBLAS_STATUS_SUCCESS) {
                    record_matmul_locked(true, compute, true);
                    return true;
                }
                if (status != CUBLAS_STATUS_NOT_SUPPORTED
                    && status != CUBLAS_STATUS_ARCH_MISMATCH) {
                    error_ = std::string(operation)
                        + " via cublasGemmEx fast mixed compute failed: "
                        + cublas_status_text(status);
                    return false;
                }
            }
        }

        uint32_t trans_left = transpose_left ? 1u : 0u;
        uint32_t trans_right = transpose_right ? 1u : 0u;
        uint32_t compute_mode = static_cast<uint32_t>(compute);
        void* arguments[] = {
            &left, &right, &output, &rows, &cols, &inner,
            &trans_left, &trans_right, &compute_mode
        };
        constexpr unsigned block_x = 16;
        constexpr unsigned block_y = 16;
        const unsigned grid_x = cols / block_x + (cols % block_x != 0 ? 1u : 0u);
        const unsigned grid_y = rows / block_y + (rows % block_y != 0 ? 1u : 0u);
        if (!require(cuLaunchKernel_(matmul_ex_f32_, grid_x, grid_y, 1,
                                    block_x, block_y, 1, 0, nullptr,
                                    arguments, nullptr), operation)) {
            return false;
        }
        record_matmul_locked(false, compute, false,
                             compute != MatmulCompute::FLOAT32);
        return true;
    }

    bool launch_matmul_typed_locked(CUdeviceptr left, TensorStorage left_storage,
                                    CUdeviceptr right, TensorStorage right_storage,
                                    CUdeviceptr output, uint32_t rows,
                                    uint32_t cols, uint32_t inner,
                                    bool transpose_left, bool transpose_right,
                                    MatmulCompute compute,
                                    const char* operation) {
        const bool typed_storage = left_storage != TensorStorage::FLOAT32
            || right_storage != TensorStorage::FLOAT32;
        if (!typed_storage) {
            return launch_matmul_ex_locked(left, right, output, rows, cols, inner,
                                           transpose_left, transpose_right,
                                           compute, operation);
        }
        const auto is_numeric_storage = [](TensorStorage storage) {
            return storage == TensorStorage::FLOAT32
                || storage == TensorStorage::FLOAT16
                || storage == TensorStorage::BFLOAT16;
        };
        if (!is_numeric_storage(left_storage)
            || !is_numeric_storage(right_storage)) {
            error_ = std::string(operation) + " requires floating-point tensor storage";
            return false;
        }
        if (left_storage != right_storage
            && left_storage != TensorStorage::FLOAT32
            && right_storage != TensorStorage::FLOAT32) {
            error_ = std::string(operation)
                + " does not mix float16 and bfloat16 storage in one matmul";
            return false;
        }
        if (compute != MatmulCompute::FLOAT32) {
            const TensorStorage required = compute == MatmulCompute::FLOAT16
                ? TensorStorage::FLOAT16 : TensorStorage::BFLOAT16;
            if (left_storage != required || right_storage != required) {
                error_ = std::string(operation)
                    + " low-precision compute requires both operands to use the matching storage type";
                return false;
            }
        }

        // A native cuBLAS typed GEMM is used when both operands share one
        // two-byte format. Mixed storage remains a deterministic PTX fallback.
        if (left_storage == right_storage
            && (left_storage == TensorStorage::FLOAT16
                || left_storage == TensorStorage::BFLOAT16)
            && cublas_handle_ && cublasGemmEx_
            && rows <= (uint32_t)std::numeric_limits<int>::max()
            && cols <= (uint32_t)std::numeric_limits<int>::max()
            && inner <= (uint32_t)std::numeric_limits<int>::max()) {
            const bool supported_arch = left_storage == TensorStorage::FLOAT16
                ? compute_major_ >= 7 : compute_major_ >= 8;
            if (supported_arch) {
                const int m = (int)cols;
                const int n = (int)rows;
                const int k = (int)inner;
                const int trans_a = transpose_right ? CUBLAS_OP_T : CUBLAS_OP_N;
                const int trans_b = transpose_left ? CUBLAS_OP_T : CUBLAS_OP_N;
                const int lda = transpose_right ? k : m;
                const int ldb = transpose_left ? n : k;
                const int ldc = m;
                const float alpha = 1.0f;
                const float beta = 0.0f;
                const void* a = reinterpret_cast<const void*>((uintptr_t)right);
                const void* b = reinterpret_cast<const void*>((uintptr_t)left);
                void* c = reinterpret_cast<void*>((uintptr_t)output);
                const int data_type = left_storage == TensorStorage::FLOAT16
                    ? CUDA_R_16F : CUDA_R_16BF;
                int compute_type = CUBLAS_COMPUTE_32F;
                if (compute == MatmulCompute::FLOAT16) {
                    compute_type = CUBLAS_COMPUTE_32F_FAST_16F;
                } else if (compute == MatmulCompute::BFLOAT16) {
                    compute_type = CUBLAS_COMPUTE_32F_FAST_16BF;
                }
                const CublasStatus status = cublasGemmEx_(
                    cublas_handle_, trans_a, trans_b, m, n, k,
                    &alpha, a, data_type, lda, b, data_type, ldb,
                    &beta, c, CUDA_R_32F, ldc, compute_type,
                    CUBLAS_GEMM_DEFAULT);
                if (status == CUBLAS_STATUS_SUCCESS) {
                    record_matmul_locked(true, compute,
                                         compute != MatmulCompute::FLOAT32,
                                         false, true);
                    return true;
                }
                if (status != CUBLAS_STATUS_NOT_SUPPORTED
                    && status != CUBLAS_STATUS_ARCH_MISMATCH) {
                    error_ = std::string(operation)
                        + " via cublasGemmEx typed storage failed: "
                        + cublas_status_text(status);
                    return false;
                }
            }
        }

        uint32_t trans_left = transpose_left ? 1u : 0u;
        uint32_t trans_right = transpose_right ? 1u : 0u;
        uint32_t left_storage_code = static_cast<uint32_t>(left_storage);
        uint32_t right_storage_code = static_cast<uint32_t>(right_storage);
        void* arguments[] = {
            &left, &right, &output, &rows, &cols, &inner,
            &trans_left, &trans_right, &left_storage_code, &right_storage_code
        };
        constexpr unsigned block_x = 16;
        constexpr unsigned block_y = 16;
        const unsigned grid_x = cols / block_x + (cols % block_x != 0 ? 1u : 0u);
        const unsigned grid_y = rows / block_y + (rows % block_y != 0 ? 1u : 0u);
        if (!require(cuLaunchKernel_(matmul_typed_f32_, grid_x, grid_y, 1,
                                    block_x, block_y, 1, 0, nullptr,
                                    arguments, nullptr), operation)) {
            return false;
        }
        record_matmul_locked(false, compute, false,
                             compute != MatmulCompute::FLOAT32, true);
        return true;
    }

    bool binary_handles_locked(DeviceHandle left_handle,
                               DeviceHandle right_handle,
                               DeviceHandle output_handle,
                               size_t count, uint32_t operation_code,
                               const char* operation) {
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, operation)) return false;
        CUdeviceptr left = 0, right = 0, output = 0;
        if (!resolve_range_locked(left_handle, 0, count, left, operation)
            || !resolve_range_locked(right_handle, 0, count, right, operation)
            || !resolve_range_locked(output_handle, 0, count, output, operation)) {
            return false;
        }
        void* arguments[] = {
            &left, &right, &output, &kernel_count, &operation_code
        };
        return launch_1d_locked(binary_f32_, arguments, kernel_count, operation);
    }

    bool initialize_locked() {
        if (attempted_) return initialized_;
        attempted_ = true;
        if (const char* disabled = std::getenv("SURA_CUDA_DISABLE")) {
            if (std::strcmp(disabled, "1") == 0) {
                error_ = "CUDA backend disabled by SURA_CUDA_DISABLE=1";
                return false;
            }
        }
#ifdef _WIN32
        library_ = LoadLibraryW(L"nvcuda.dll");
#else
        library_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library_) {
            error_ = "NVIDIA CUDA driver library was not found";
            return false;
        }
        if (!load_symbol(cuInit_, "cuInit")
            || !load_symbol(cuDeviceGetCount_, "cuDeviceGetCount")
            || !load_symbol(cuDeviceGet_, "cuDeviceGet")
            || !load_symbol(cuDeviceGetName_, "cuDeviceGetName")
            || !load_symbol(cuDeviceComputeCapability_, "cuDeviceComputeCapability")
            || !load_symbol(cuDeviceTotalMem_, "cuDeviceTotalMem_v2")
            || !load_symbol(cuCtxCreate_, "cuCtxCreate_v2")
            || !load_symbol(cuCtxDestroy_, "cuCtxDestroy_v2")
            || !load_symbol(cuModuleLoadData_, "cuModuleLoadData")
            || !load_symbol(cuModuleUnload_, "cuModuleUnload")
            || !load_symbol(cuModuleGetFunction_, "cuModuleGetFunction")
            || !load_symbol(cuMemAlloc_, "cuMemAlloc_v2")
            || !load_symbol(cuMemFree_, "cuMemFree_v2")
            || !load_symbol(cuMemcpyHtoD_, "cuMemcpyHtoD_v2")
            || !load_symbol(cuMemcpyDtoH_, "cuMemcpyDtoH_v2")
            || !load_symbol(cuMemcpyDtoD_, "cuMemcpyDtoD_v2")
            || !load_symbol(cuCtxSetCurrent_, "cuCtxSetCurrent")
            || !load_symbol(cuLaunchKernel_, "cuLaunchKernel")
            || !load_symbol(cuCtxSynchronize_, "cuCtxSynchronize")) {
            return false;
        }
        load_symbol(cuGetErrorName_, "cuGetErrorName");
        load_symbol(cuGetErrorString_, "cuGetErrorString");
        if (!require(cuInit_(0), "cuInit")) return false;
        if (!require(cuDeviceGetCount_(&device_count_), "cuDeviceGetCount")) return false;
        if (device_count_ <= 0) {
            error_ = "no CUDA device is available";
            return false;
        }
        if (const char* selected = std::getenv("SURA_CUDA_DEVICE")) {
            if (*selected) {
                char* end = nullptr;
                errno = 0;
                long parsed = std::strtol(selected, &end, 10);
                if (errno != 0 || end == selected || *end != '\0'
                    || parsed < 0 || parsed >= device_count_) {
                    error_ = "SURA_CUDA_DEVICE must be an available zero-based CUDA device index";
                    return false;
                }
                device_index_ = (int)parsed;
            }
        }
        if (!require(cuDeviceGet_(&device_, device_index_), "cuDeviceGet")) return false;
        char name[256]{};
        if (!require(cuDeviceGetName_(name, (int)sizeof(name), device_), "cuDeviceGetName")) {
            return false;
        }
        device_name_ = name;
        if (!require(cuDeviceComputeCapability_(&compute_major_, &compute_minor_, device_),
                     "cuDeviceComputeCapability")) return false;
        if (!require(cuDeviceTotalMem_(&total_memory_, device_), "cuDeviceTotalMem")) return false;
        if (!require(cuCtxCreate_(&context_, 0, device_), "cuCtxCreate")) return false;
        CUresult module_result = cuModuleLoadData_(&module_, kPtx);
        if (module_result != CUDA_SUCCESS) {
            error_ = "cuModuleLoadData failed: " + result_text(module_result);
            return false;
        }
        if (!require(cuModuleGetFunction_(&matmul_f32_, module_, "sura_matmul_f32"),
                     "cuModuleGetFunction")) return false;
        if (!require(cuModuleGetFunction_(&matmul_ex_f32_, module_, "sura_matmul_ex_f32"),
                      "cuModuleGetFunction(matmul_ex_f32)")
            || !require(cuModuleGetFunction_(&matmul_typed_f32_, module_,
                                              "sura_matmul_typed_f32"),
                        "cuModuleGetFunction(matmul_typed_f32)")
            || !require(cuModuleGetFunction_(&unpack_u16_f32_, module_,
                                              "sura_unpack_u16_f32"),
                        "cuModuleGetFunction(unpack_u16_f32)")
            || !require(cuModuleGetFunction_(&pack_f32_u16_, module_,
                                              "sura_pack_f32_u16"),
                        "cuModuleGetFunction(pack_f32_u16)")
            || !require(cuModuleGetFunction_(&binary_f32_, module_, "sura_binary_f32"),
                        "cuModuleGetFunction(binary_f32)")
            || !require(cuModuleGetFunction_(&fill_f32_, module_, "sura_fill_f32"),
                        "cuModuleGetFunction(fill_f32)")
            || !require(cuModuleGetFunction_(&scale_f32_, module_, "sura_scale_f32"),
                        "cuModuleGetFunction(scale_f32)")
            || !require(cuModuleGetFunction_(&affine_f32_, module_, "sura_affine_f32"),
                         "cuModuleGetFunction(affine_f32)")
            || !require(cuModuleGetFunction_(&finite_status_f32_, module_,
                                             "sura_finite_status_f32"),
                        "cuModuleGetFunction(finite_status_f32)")
            || !require(cuModuleGetFunction_(&adam_f32_, module_, "sura_adam_f32"),
                         "cuModuleGetFunction(adam_f32)")
            || !require(cuModuleGetFunction_(&bias_add_f32_, module_, "sura_bias_add_f32"),
                         "cuModuleGetFunction(bias_add_f32)")
            || !require(cuModuleGetFunction_(&bias_gradient_f32_, module_,
                                              "sura_bias_gradient_f32"),
                         "cuModuleGetFunction(bias_gradient_f32)")
            || !require(cuModuleGetFunction_(&relu_f32_, module_, "sura_relu_f32"),
                        "cuModuleGetFunction(relu_f32)")
            || !require(cuModuleGetFunction_(&relu_backward_f32_, module_,
                                             "sura_relu_backward_f32"),
                        "cuModuleGetFunction(relu_backward_f32)")
            || !require(cuModuleGetFunction_(&gelu_f32_, module_, "sura_gelu_f32"),
                        "cuModuleGetFunction(gelu_f32)")
            || !require(cuModuleGetFunction_(&gelu_backward_f32_, module_,
                                             "sura_gelu_backward_f32"),
                        "cuModuleGetFunction(gelu_backward_f32)")
            || !require(cuModuleGetFunction_(&layer_norm_f32_, module_,
                                             "sura_layer_norm_f32"),
                        "cuModuleGetFunction(layer_norm_f32)")
            || !require(cuModuleGetFunction_(&layer_norm_backward_f32_, module_,
                                             "sura_layer_norm_backward_f32"),
                        "cuModuleGetFunction(layer_norm_backward_f32)")
            || !require(cuModuleGetFunction_(&layer_norm_parameter_backward_f32_,
                                             module_,
                                             "sura_layer_norm_parameter_backward_f32"),
                        "cuModuleGetFunction(layer_norm_parameter_backward_f32)")
            || !require(cuModuleGetFunction_(&embedding_f32_, module_,
                                             "sura_embedding_f32"),
                        "cuModuleGetFunction(embedding_f32)")
            || !require(cuModuleGetFunction_(&embedding_backward_f32_, module_,
                                             "sura_embedding_backward_f32"),
                        "cuModuleGetFunction(embedding_backward_f32)")
            || !require(cuModuleGetFunction_(&cross_entropy_ids_stats_f32_, module_,
                                             "sura_cross_entropy_ids_stats_f32"),
                        "cuModuleGetFunction(cross_entropy_ids_stats_f32)")
            || !require(cuModuleGetFunction_(&cross_entropy_ids_loss_f32_, module_,
                                             "sura_cross_entropy_ids_loss_f32"),
                        "cuModuleGetFunction(cross_entropy_ids_loss_f32)")
            || !require(cuModuleGetFunction_(&cross_entropy_ids_backward_f32_, module_,
                                             "sura_cross_entropy_ids_backward_f32"),
                        "cuModuleGetFunction(cross_entropy_ids_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_f32_, module_,
                                             "sura_causal_attention_f32"),
                        "cuModuleGetFunction(causal_attention_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_warp_f32_, module_,
                                             "sura_causal_attention_warp_f32"),
                        "cuModuleGetFunction(causal_attention_warp_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_warp_fast_f32_, module_,
                                             "sura_causal_attention_warp_fast_f32"),
                        "cuModuleGetFunction(causal_attention_warp_fast_f32)")
            || !require(cuModuleGetFunction_(
                            &causal_attention_fused_query_backward_f32_, module_,
                            "sura_causal_attention_fused_query_backward_f32"),
                        "cuModuleGetFunction(causal_attention_fused_query_backward_f32)")
            || !require(cuModuleGetFunction_(
                            &causal_attention_fused_key_value_backward_f32_, module_,
                            "sura_causal_attention_fused_key_value_backward_f32"),
                        "cuModuleGetFunction(causal_attention_fused_key_value_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_backward_f32_, module_,
                                             "sura_causal_attention_backward_f32"),
                        "cuModuleGetFunction(causal_attention_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_probabilities_f32_, module_,
                                             "sura_causal_attention_probabilities_f32"),
                        "cuModuleGetFunction(causal_attention_probabilities_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_value_backward_f32_, module_,
                                             "sura_causal_attention_value_backward_f32"),
                        "cuModuleGetFunction(causal_attention_value_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_score_backward_f32_, module_,
                                             "sura_causal_attention_score_backward_f32"),
                        "cuModuleGetFunction(causal_attention_score_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_query_backward_f32_, module_,
                                             "sura_causal_attention_query_backward_f32"),
                        "cuModuleGetFunction(causal_attention_query_backward_f32)")
            || !require(cuModuleGetFunction_(&causal_attention_key_backward_f32_, module_,
                                             "sura_causal_attention_key_backward_f32"),
                        "cuModuleGetFunction(causal_attention_key_backward_f32)")
            || !require(cuModuleGetFunction_(&transpose_f32_, module_,
                                             "sura_transpose_f32"),
                        "cuModuleGetFunction(transpose_f32)")
            || !require(cuModuleGetFunction_(&sum_f32_, module_, "sura_sum_f32"),
                        "cuModuleGetFunction(sum_f32)")) {
            return false;
        }
        initialize_optional_cublas_locked();
        initialized_ = true;
        error_.clear();
        return true;
    }

public:
    ~SuraCudaDriver() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_ && cuCtxSetCurrent_) cuCtxSetCurrent_(context_);
        if (context_ && cuCtxSynchronize_) cuCtxSynchronize_();
        close_cublas_locked();
        if (cuMemFree_) {
            for (const auto& allocation : allocations_) {
                if (allocation.second.pointer) cuMemFree_(allocation.second.pointer);
            }
        }
        allocations_.clear();
        stats_.allocated_bytes = 0;
        if (module_ && cuModuleUnload_) cuModuleUnload_(module_);
        if (context_ && cuCtxDestroy_) cuCtxDestroy_(context_);
#ifdef _WIN32
        if (library_) FreeLibrary(library_);
#else
        if (library_) dlclose(library_);
#endif
    }

    static SuraCudaDriver& instance() {
        static SuraCudaDriver driver;
        return driver;
    }

    bool available() {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialize_locked();
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    std::string device_name() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_name_;
    }
    int device_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_count_;
    }
    int device_index() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_index_;
    }
    int compute_major() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compute_major_;
    }
    int compute_minor() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compute_minor_;
    }
    size_t total_memory() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_memory_;
    }
    size_t allocated_memory() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_.allocated_bytes;
    }
    StatsSnapshot stats_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        StatsSnapshot snapshot = stats_;
        snapshot.cublas_available = cublas_handle_ != nullptr;
        snapshot.cublas_gemm_ex_available = cublas_handle_ != nullptr
            && cublasGemmEx_ != nullptr;
        snapshot.cublas_disabled = cublas_disabled_;
        return snapshot;
    }
    void reset_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t current_allocated = stats_.allocated_bytes;
        stats_ = StatsSnapshot{};
        stats_.allocated_bytes = current_allocated;
        stats_.peak_allocated_bytes = current_allocated;
    }

    bool cublas_available() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialize_locked()) return false;
        return cublas_handle_ != nullptr;
    }

    std::string cublas_library_name() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialize_locked()) return {};
        return cublas_library_name_;
    }

    std::string cublas_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!attempted_) initialize_locked();
        return cublas_error_;
    }

    bool synchronize() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        return require(cuCtxSynchronize_(), "cuCtxSynchronize");
    }

    // Every allocation has an immutable storage kind. Public handles are
    // opaque so a two-byte tensor can never be reinterpreted by an f32 kernel.
    // Handles become invalid immediately after free_device succeeds.
    bool allocate_f32(size_t elements, DeviceHandle& out_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocate_storage_locked(elements, TensorStorage::FLOAT32,
                                       out_handle, "allocate_f32");
    }

    bool allocate_f16(size_t elements, DeviceHandle& out_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocate_storage_locked(elements, TensorStorage::FLOAT16,
                                       out_handle, "allocate_f16");
    }

    bool allocate_bfloat16(size_t elements, DeviceHandle& out_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocate_storage_locked(elements, TensorStorage::BFLOAT16,
                                       out_handle, "allocate_bfloat16");
    }

    bool allocate_u32(size_t elements, DeviceHandle& out_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocate_storage_locked(elements, TensorStorage::UINT32,
                                       out_handle, "allocate_u32");
    }

    bool free_device(DeviceHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        auto found = allocations_.find(handle);
        if (handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = "free_device received an invalid or already-freed CUDA handle";
            return false;
        }
        const size_t element_bytes = storage_element_size(found->second.storage);
        if (element_bytes == 0
            || found->second.elements > std::numeric_limits<size_t>::max() / element_bytes) {
            error_ = "free_device found invalid CUDA allocation metadata";
            return false;
        }
        const size_t bytes = found->second.elements * element_bytes;
        if (!require(cuMemFree_(found->second.pointer), "cuMemFree(free_device)")) {
            return false;
        }
        record_free_locked(bytes);
        allocations_.erase(found);
        handle = INVALID_DEVICE_HANDLE;
        return true;
    }

    bool allocation_elements(DeviceHandle handle, size_t& elements) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        auto found = allocations_.find(handle);
        if (handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = "allocation_elements received an invalid or freed CUDA handle";
            return false;
        }
        elements = found->second.elements;
        return true;
    }

    bool allocation_storage(DeviceHandle handle, TensorStorage& storage) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        auto found = allocations_.find(handle);
        if (handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = "allocation_storage received an invalid or freed CUDA handle";
            return false;
        }
        storage = found->second.storage;
        return true;
    }

    bool upload_f32(DeviceHandle destination, size_t destination_offset,
                    const float* source, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (!source) {
            error_ = "upload_f32 received a null host source";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "upload_f32")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_range_locked(destination, destination_offset, count,
                                  pointer, "upload_f32")) return false;
        const size_t bytes = count * sizeof(float);
        if (!require(cuMemcpyHtoD_(pointer, source, bytes),
                     "cuMemcpyHtoD(upload_f32)")) return false;
        add_counter_locked(stats_.h2d_bytes, (uint64_t)bytes);
        return true;
    }

    bool upload_f32(DeviceHandle destination, const std::vector<float>& source,
                    size_t destination_offset = 0) {
        return upload_f32(destination, destination_offset, source.data(), source.size());
    }

    bool upload_u32(DeviceHandle destination, size_t destination_offset,
                    const uint32_t* source, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (!source) {
            error_ = "upload_u32 received a null host source";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "upload_u32")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_storage_range_locked(destination, destination_offset, count,
                                          TensorStorage::UINT32, pointer,
                                          "upload_u32")) return false;
        const size_t bytes = count * sizeof(uint32_t);
        if (!require(cuMemcpyHtoD_(pointer, source, bytes),
                     "cuMemcpyHtoD(upload_u32)")) return false;
        add_counter_locked(stats_.h2d_bytes, (uint64_t)bytes);
        return true;
    }

    bool upload_u32(DeviceHandle destination, const std::vector<uint32_t>& source,
                    size_t destination_offset = 0) {
        return upload_u32(destination, destination_offset,
                          source.data(), source.size());
    }

    bool upload_u16(DeviceHandle destination, TensorStorage storage,
                    size_t destination_offset, const uint16_t* source,
                    size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (storage != TensorStorage::FLOAT16
            && storage != TensorStorage::BFLOAT16) {
            error_ = "upload_u16 requires float16 or bfloat16 storage";
            return false;
        }
        if (!source) {
            error_ = "upload_u16 received a null host source";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "upload_u16")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_storage_range_locked(destination, destination_offset, count,
                                          storage, pointer, "upload_u16")) {
            return false;
        }
        const size_t bytes = count * sizeof(uint16_t);
        if (!require(cuMemcpyHtoD_(pointer, source, bytes),
                     "cuMemcpyHtoD(upload_u16)")) return false;
        add_counter_locked(stats_.h2d_bytes, (uint64_t)bytes);
        return true;
    }

    bool upload_u16(DeviceHandle destination, TensorStorage storage,
                    const std::vector<uint16_t>& source,
                    size_t destination_offset = 0) {
        return upload_u16(destination, storage, destination_offset,
                          source.data(), source.size());
    }

    bool download_f32(DeviceHandle source, size_t source_offset,
                      float* destination, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (!destination) {
            error_ = "download_f32 received a null host destination";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "download_f32")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_range_locked(source, source_offset, count,
                                  pointer, "download_f32")) return false;
        const size_t bytes = count * sizeof(float);
        if (!require(cuMemcpyDtoH_(destination, pointer, bytes),
                     "cuMemcpyDtoH(download_f32)")) return false;
        add_counter_locked(stats_.d2h_bytes, (uint64_t)bytes);
        return true;
    }

    bool download_f32(DeviceHandle source, std::vector<float>& destination,
                      size_t count, size_t source_offset = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "download_f32")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_range_locked(source, source_offset, count,
                                  pointer, "download_f32")) return false;
        try {
            destination.assign(count, 0.0f);
        } catch (...) {
            error_ = "host allocation failed while preparing download_f32";
            return false;
        }
        const size_t bytes = count * sizeof(float);
        if (!require(cuMemcpyDtoH_(destination.data(), pointer, bytes),
                     "cuMemcpyDtoH(download_f32)")) return false;
        add_counter_locked(stats_.d2h_bytes, (uint64_t)bytes);
        return true;
    }

    bool read_status_u32(DeviceHandle source, uint32_t& status_bits) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_range_locked(source, 0, 1, pointer,
                                  "read_status_u32")) return false;
        uint32_t candidate = 0;
        if (!require(cuMemcpyDtoH_(&candidate, pointer, sizeof(candidate)),
                     "cuMemcpyDtoH(read_status_u32)")) return false;
        add_counter_locked(stats_.control_d2h_bytes, sizeof(candidate));
        status_bits = candidate;
        return true;
    }

    bool download_u16(DeviceHandle source, TensorStorage storage,
                      size_t source_offset, uint16_t* destination,
                      size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (storage != TensorStorage::FLOAT16
            && storage != TensorStorage::BFLOAT16) {
            error_ = "download_u16 requires float16 or bfloat16 storage";
            return false;
        }
        if (!destination) {
            error_ = "download_u16 received a null host destination";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "download_u16")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_storage_range_locked(source, source_offset, count,
                                          storage, pointer, "download_u16")) {
            return false;
        }
        const size_t bytes = count * sizeof(uint16_t);
        if (!require(cuMemcpyDtoH_(destination, pointer, bytes),
                     "cuMemcpyDtoH(download_u16)")) return false;
        add_counter_locked(stats_.d2h_bytes, (uint64_t)bytes);
        return true;
    }

    bool download_u16(DeviceHandle source, TensorStorage storage,
                      std::vector<uint16_t>& destination, size_t count,
                      size_t source_offset = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (storage != TensorStorage::FLOAT16
            && storage != TensorStorage::BFLOAT16) {
            error_ = "download_u16 requires float16 or bfloat16 storage";
            return false;
        }
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "download_u16")) return false;
        CUdeviceptr pointer = 0;
        if (!resolve_storage_range_locked(source, source_offset, count,
                                          storage, pointer, "download_u16")) {
            return false;
        }
        try {
            destination.assign(count, (uint16_t)0);
        } catch (...) {
            error_ = "host allocation failed while preparing download_u16";
            return false;
        }
        const size_t bytes = count * sizeof(uint16_t);
        if (!require(cuMemcpyDtoH_(destination.data(), pointer, bytes),
                     "cuMemcpyDtoH(download_u16)")) return false;
        add_counter_locked(stats_.d2h_bytes, (uint64_t)bytes);
        return true;
    }

    bool copy_f32(DeviceHandle source, size_t source_offset,
                  DeviceHandle destination, size_t destination_offset,
                  size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "copy_f32")) return false;
        CUdeviceptr source_pointer = 0, destination_pointer = 0;
        if (!resolve_range_locked(source, source_offset, count,
                                  source_pointer, "copy_f32(source)")
            || !resolve_range_locked(destination, destination_offset, count,
                                     destination_pointer, "copy_f32(destination)")) {
            return false;
        }
        if (source == destination && source_offset == destination_offset) return true;
        if (source == destination) {
            const size_t source_end = source_offset + count;
            const size_t destination_end = destination_offset + count;
            if (source_offset < destination_end && destination_offset < source_end) {
                error_ = "copy_f32 rejects overlapping ranges in the same CUDA allocation";
                return false;
            }
        }
        const size_t bytes = count * sizeof(float);
        if (!require(cuMemcpyDtoD_(destination_pointer, source_pointer, bytes),
                     "cuMemcpyDtoD(copy_f32)")) return false;
        add_counter_locked(stats_.d2d_bytes, (uint64_t)bytes);
        return true;
    }

    bool copy_storage(DeviceHandle source, size_t source_offset,
                      DeviceHandle destination, size_t destination_offset,
                      size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t ignored_count = 0;
        if (!kernel_count_locked(count, ignored_count, "copy_storage")) return false;
        auto source_found = allocations_.find(source);
        auto destination_found = allocations_.find(destination);
        if (source == INVALID_DEVICE_HANDLE || source_found == allocations_.end()
            || destination == INVALID_DEVICE_HANDLE
            || destination_found == allocations_.end()) {
            error_ = "copy_storage received an invalid or freed CUDA handle";
            return false;
        }
        const TensorStorage storage = source_found->second.storage;
        if (destination_found->second.storage != storage) {
            error_ = "copy_storage rejects different source and destination storage types";
            return false;
        }
        CUdeviceptr source_pointer = 0, destination_pointer = 0;
        if (!resolve_storage_range_locked(source, source_offset, count, storage,
                                          source_pointer, "copy_storage(source)")
            || !resolve_storage_range_locked(destination, destination_offset,
                                             count, storage, destination_pointer,
                                             "copy_storage(destination)")) {
            return false;
        }
        if (source == destination && source_offset == destination_offset) return true;
        if (source == destination) {
            const size_t source_end = source_offset + count;
            const size_t destination_end = destination_offset + count;
            if (source_offset < destination_end && destination_offset < source_end) {
                error_ = "copy_storage rejects overlapping ranges in the same CUDA allocation";
                return false;
            }
        }
        const size_t element_bytes = storage_element_size(storage);
        if (element_bytes == 0
            || count > std::numeric_limits<size_t>::max() / element_bytes) {
            error_ = "copy_storage found invalid CUDA allocation metadata";
            return false;
        }
        const size_t bytes = count * element_bytes;
        if (!require(cuMemcpyDtoD_(destination_pointer, source_pointer, bytes),
                     "cuMemcpyDtoD(copy_storage)")) return false;
        add_counter_locked(stats_.d2d_bytes, (uint64_t)bytes);
        return true;
    }

    bool unpack_u16_to_f32(DeviceHandle source_handle,
                           DeviceHandle output_handle, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count,
                                 "unpack_u16_to_f32")) return false;
        auto found = allocations_.find(source_handle);
        if (source_handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = "unpack_u16_to_f32 received an invalid or freed source handle";
            return false;
        }
        const TensorStorage storage = found->second.storage;
        if (storage != TensorStorage::FLOAT16
            && storage != TensorStorage::BFLOAT16) {
            error_ = "unpack_u16_to_f32 requires float16 or bfloat16 source storage";
            return false;
        }
        if (source_handle == output_handle) {
            error_ = "unpack_u16_to_f32 requires a distinct float32 output allocation";
            return false;
        }
        CUdeviceptr source = 0, output = 0;
        if (!resolve_storage_range_locked(source_handle, 0, count, storage,
                                          source, "unpack_u16_to_f32(source)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "unpack_u16_to_f32(output)")) {
            return false;
        }
        uint32_t storage_code = static_cast<uint32_t>(storage);
        void* arguments[] = {&source, &output, &kernel_count, &storage_code};
        return launch_1d_locked(unpack_u16_f32_, arguments, kernel_count,
                                "cuLaunchKernel(unpack_u16_to_f32)",
                                KernelKind::CONVERSION);
    }

    bool pack_f32_to_u16(DeviceHandle source_handle,
                         DeviceHandle output_handle,
                         DeviceHandle status_handle, uint32_t status_bit,
                         size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count,
                                 "pack_f32_to_u16")) return false;
        if (status_bit == 0) {
            error_ = "pack_f32_to_u16 requires a non-zero status bit";
            return false;
        }
        auto found = allocations_.find(output_handle);
        if (output_handle == INVALID_DEVICE_HANDLE || found == allocations_.end()) {
            error_ = "pack_f32_to_u16 received an invalid or freed output handle";
            return false;
        }
        const TensorStorage storage = found->second.storage;
        if (storage != TensorStorage::FLOAT16
            && storage != TensorStorage::BFLOAT16) {
            error_ = "pack_f32_to_u16 requires float16 or bfloat16 output storage";
            return false;
        }
        if (source_handle == output_handle || source_handle == status_handle
            || output_handle == status_handle) {
            error_ = "pack_f32_to_u16 requires pairwise-distinct logical buffers";
            return false;
        }
        CUdeviceptr source = 0, output = 0, status = 0;
        if (!resolve_range_locked(source_handle, 0, count, source,
                                  "pack_f32_to_u16(source)")
            || !resolve_storage_range_locked(output_handle, 0, count, storage,
                                              output, "pack_f32_to_u16(output)")
            || !resolve_range_locked(status_handle, 0, 1, status,
                                     "pack_f32_to_u16(status)")) {
            return false;
        }
        uint32_t storage_code = static_cast<uint32_t>(storage);
        float max_finite = storage == TensorStorage::FLOAT16
            ? 65504.0f : 0x1.fep+127f;
        void* arguments[] = {
            &source, &output, &status, &kernel_count, &storage_code,
            &status_bit, &max_finite
        };
        return launch_1d_locked(pack_f32_u16_, arguments, kernel_count,
                                "cuLaunchKernel(pack_f32_to_u16)",
                                KernelKind::CONVERSION);
    }

    // Kernel launches enqueue work in the driver's context. Call synchronize()
    // at an explicit graph boundary; synchronous download_f32 also orders prior
    // work and returns any copy-side CUDA failure.
    bool fill_f32(DeviceHandle output_handle, float value, size_t count,
                  size_t output_offset = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "fill_f32")) return false;
        CUdeviceptr output = 0;
        if (!resolve_range_locked(output_handle, output_offset, count,
                                  output, "fill_f32")) return false;
        void* arguments[] = {&output, &value, &kernel_count};
        return launch_1d_locked(fill_f32_, arguments, kernel_count,
                                "cuLaunchKernel(fill_f32)");
    }

    bool add_f32(DeviceHandle left, DeviceHandle right,
                 DeviceHandle output, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        return binary_handles_locked(left, right, output, count, 0,
                                     "cuLaunchKernel(add_f32)");
    }

    bool subtract_f32(DeviceHandle left, DeviceHandle right,
                      DeviceHandle output, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        return binary_handles_locked(left, right, output, count, 1,
                                     "cuLaunchKernel(subtract_f32)");
    }

    bool multiply_f32(DeviceHandle left, DeviceHandle right,
                      DeviceHandle output, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        return binary_handles_locked(left, right, output, count, 2,
                                     "cuLaunchKernel(multiply_f32)");
    }

    bool divide_f32(DeviceHandle left, DeviceHandle right,
                    DeviceHandle output, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        return binary_handles_locked(left, right, output, count, 3,
                                     "cuLaunchKernel(divide_f32)");
    }

    bool scale_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                   float scale, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "scale_f32")) return false;
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input, "scale_f32(input)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "scale_f32(output)")) return false;
        void* arguments[] = {&input, &output, &scale, &kernel_count};
        return launch_1d_locked(scale_f32_, arguments, kernel_count,
                                "cuLaunchKernel(scale_f32)");
    }

    bool affine_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                    float scale, float bias, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "affine_f32")) return false;
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input, "affine_f32(input)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "affine_f32(output)")) return false;
        void* arguments[] = {&input, &output, &scale, &bias, &kernel_count};
        return launch_1d_locked(affine_f32_, arguments, kernel_count,
                                "cuLaunchKernel(affine_f32)");
    }

    bool finite_status_f32(DeviceHandle input_handle,
                           DeviceHandle status_handle,
                           size_t count, uint32_t status_bit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (input_handle == status_handle) {
            error_ = "finite_status_f32 requires a distinct status allocation";
            return false;
        }
        if (status_bit == 0) {
            error_ = "finite_status_f32 requires a non-zero status bit";
            return false;
        }
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count,
                                 "finite_status_f32")) return false;
        CUdeviceptr input = 0, status = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "finite_status_f32(input)")
            || !resolve_range_locked(status_handle, 0, 1, status,
                                     "finite_status_f32(status)")) {
            return false;
        }
        void* arguments[] = {
            &input, &status, &kernel_count, &status_bit
        };
        return launch_1d_locked(finite_status_f32_, arguments, kernel_count,
                                "cuLaunchKernel(finite_status_f32)",
                                KernelKind::OPTIMIZER);
    }

    bool adam_f32(DeviceHandle old_parameter_handle,
                  DeviceHandle gradient_handle,
                  DeviceHandle old_first_moment_handle,
                  DeviceHandle old_second_moment_handle,
                  DeviceHandle new_parameter_handle,
                  DeviceHandle new_first_moment_handle,
                  DeviceHandle new_second_moment_handle,
                  DeviceHandle status_handle,
                  size_t count,
                  float learning_rate,
                  float one_minus_beta1,
                  float one_minus_beta2,
                  float correction1,
                  float correction2,
                  float epsilon,
                  float weight_decay) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (!std::isfinite(learning_rate) || learning_rate <= 0.0f
            || !std::isfinite(one_minus_beta1)
            || one_minus_beta1 <= 0.0f || one_minus_beta1 > 1.0f
            || !std::isfinite(one_minus_beta2)
            || one_minus_beta2 <= 0.0f || one_minus_beta2 > 1.0f
            || !std::isfinite(correction1) || correction1 <= 0.0f
            || !std::isfinite(correction2) || correction2 <= 0.0f
            || !std::isfinite(epsilon) || epsilon <= 0.0f
            || !std::isfinite(weight_decay) || weight_decay < 0.0f) {
            error_ = "adam_f32 received invalid hyperparameters";
            return false;
        }
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "adam_f32")) return false;
        const DeviceHandle inputs[] = {
            old_parameter_handle, gradient_handle,
            old_first_moment_handle, old_second_moment_handle
        };
        const DeviceHandle outputs[] = {
            new_parameter_handle, new_first_moment_handle,
            new_second_moment_handle, status_handle
        };
        for (DeviceHandle output : outputs) {
            for (DeviceHandle input : inputs) {
                if (output == input) {
                    error_ = "adam_f32 requires transactional non-aliasing outputs";
                    return false;
                }
            }
        }
        if (new_parameter_handle == new_first_moment_handle
            || new_parameter_handle == new_second_moment_handle
            || new_parameter_handle == status_handle
            || new_first_moment_handle == new_second_moment_handle
            || new_first_moment_handle == status_handle
            || new_second_moment_handle == status_handle) {
            error_ = "adam_f32 output allocations must be distinct";
            return false;
        }
        CUdeviceptr old_parameter = 0, gradient = 0;
        CUdeviceptr old_first_moment = 0, old_second_moment = 0;
        CUdeviceptr new_parameter = 0, new_first_moment = 0, new_second_moment = 0;
        CUdeviceptr status = 0;
        if (!resolve_range_locked(old_parameter_handle, 0, count, old_parameter,
                                  "adam_f32(old_parameter)")
            || !resolve_range_locked(gradient_handle, 0, count, gradient,
                                     "adam_f32(gradient)")
            || !resolve_range_locked(old_first_moment_handle, 0, count,
                                     old_first_moment, "adam_f32(old_first_moment)")
            || !resolve_range_locked(old_second_moment_handle, 0, count,
                                     old_second_moment, "adam_f32(old_second_moment)")
            || !resolve_range_locked(new_parameter_handle, 0, count,
                                     new_parameter, "adam_f32(new_parameter)")
            || !resolve_range_locked(new_first_moment_handle, 0, count,
                                     new_first_moment, "adam_f32(new_first_moment)")
            || !resolve_range_locked(new_second_moment_handle, 0, count,
                                     new_second_moment, "adam_f32(new_second_moment)")
            || !resolve_range_locked(status_handle, 0, 1, status,
                                     "adam_f32(status)")) {
            return false;
        }
        void* arguments[] = {
            &old_parameter, &gradient, &old_first_moment, &old_second_moment,
            &new_parameter, &new_first_moment, &new_second_moment, &status,
            &kernel_count, &learning_rate, &one_minus_beta1, &one_minus_beta2,
            &correction1, &correction2, &epsilon, &weight_decay
        };
        return launch_1d_locked(adam_f32_, arguments, kernel_count,
                                "cuLaunchKernel(adam_f32)",
                                KernelKind::OPTIMIZER);
    }

    bool negate_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                    size_t count) {
        return affine_f32(input_handle, output_handle, -1.0f, 0.0f, count);
    }

    bool add_scalar_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                        float scalar, size_t count) {
        return affine_f32(input_handle, output_handle, 1.0f, scalar, count);
    }

    bool bias_add_f32(DeviceHandle input_handle, DeviceHandle bias_handle,
                      DeviceHandle output_handle, uint32_t rows, uint32_t cols) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (rows == 0 || cols == 0) {
            error_ = "bias_add_f32 requires non-zero rows and columns";
            return false;
        }
        // In-place input/output is safe because each thread owns one element,
        // but the broadcast bias is shared by many threads and must remain
        // read-only for the complete launch.
        if (output_handle == bias_handle) {
            error_ = "bias_add_f32 does not permit output to alias the broadcast bias";
            return false;
        }
        const uint64_t wide_count = (uint64_t)rows * cols;
        if (wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "bias_add_f32 exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        const size_t count = (size_t)wide_count;
        uint32_t kernel_count = (uint32_t)wide_count;
        CUdeviceptr input = 0, bias = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "bias_add_f32(input)")
            || !resolve_range_locked(bias_handle, 0, cols, bias,
                                     "bias_add_f32(bias)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "bias_add_f32(output)")) return false;
        void* arguments[] = {&input, &bias, &output, &kernel_count, &cols};
        return launch_1d_locked(bias_add_f32_, arguments, kernel_count,
                                 "cuLaunchKernel(bias_add_f32)");
    }

    // Computes grad_bias[col] = sum_row grad_output[row,col] for the same
    // row-major shape accepted by bias_add_f32.
    bool bias_gradient_f32(DeviceHandle gradient_output_handle,
                           DeviceHandle gradient_bias_handle,
                           uint32_t rows, uint32_t cols) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (rows == 0 || cols == 0) {
            error_ = "bias_gradient_f32 requires non-zero rows and columns";
            return false;
        }
        if (gradient_output_handle == gradient_bias_handle) {
            error_ = "bias_gradient_f32 requires a distinct reduction output";
            return false;
        }
        const uint64_t wide_count = (uint64_t)rows * cols;
        if (wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "bias_gradient_f32 exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        CUdeviceptr gradient_output = 0, gradient_bias = 0;
        if (!resolve_range_locked(gradient_output_handle, 0, (size_t)wide_count,
                                  gradient_output, "bias_gradient_f32(gradient_output)")
            || !resolve_range_locked(gradient_bias_handle, 0, cols, gradient_bias,
                                     "bias_gradient_f32(gradient_bias)")) {
            return false;
        }
        void* arguments[] = {&gradient_output, &gradient_bias, &rows, &cols};
        return launch_1d_locked(bias_gradient_f32_, arguments, cols,
                                "cuLaunchKernel(bias_gradient_f32)",
                                KernelKind::REDUCTION);
    }

    bool relu_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                  size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "relu_f32")) return false;
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input, "relu_f32(input)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "relu_f32(output)")) return false;
        void* arguments[] = {&input, &output, &kernel_count};
        return launch_1d_locked(relu_f32_, arguments, kernel_count,
                                "cuLaunchKernel(relu_f32)", KernelKind::RELU);
    }

    bool relu_backward_f32(DeviceHandle input_handle,
                           DeviceHandle gradient_output_handle,
                           DeviceHandle gradient_input_handle,
                           size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "relu_backward_f32")) return false;
        CUdeviceptr input = 0, gradient_output = 0, gradient_input = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "relu_backward_f32(input)")
            || !resolve_range_locked(gradient_output_handle, 0, count,
                                     gradient_output,
                                     "relu_backward_f32(gradient_output)")
            || !resolve_range_locked(gradient_input_handle, 0, count,
                                     gradient_input,
                                     "relu_backward_f32(gradient_input)")) {
            return false;
        }
        void* arguments[] = {
            &input, &gradient_output, &gradient_input, &kernel_count
        };
        return launch_1d_locked(relu_backward_f32_, arguments, kernel_count,
                                "cuLaunchKernel(relu_backward_f32)",
                                KernelKind::RELU);
    }

    bool gelu_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                  size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "gelu_f32")) return false;
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input, "gelu_f32(input)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "gelu_f32(output)")) return false;
        void* arguments[] = {&input, &output, &kernel_count};
        return launch_1d_locked(gelu_f32_, arguments, kernel_count,
                                "cuLaunchKernel(gelu_f32)", KernelKind::GELU);
    }

    bool gelu_backward_f32(DeviceHandle input_handle,
                           DeviceHandle gradient_output_handle,
                           DeviceHandle gradient_input_handle,
                           size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "gelu_backward_f32")) return false;
        CUdeviceptr input = 0, gradient_output = 0, gradient_input = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "gelu_backward_f32(input)")
            || !resolve_range_locked(gradient_output_handle, 0, count,
                                     gradient_output,
                                     "gelu_backward_f32(gradient_output)")
            || !resolve_range_locked(gradient_input_handle, 0, count,
                                     gradient_input,
                                     "gelu_backward_f32(gradient_input)")) {
            return false;
        }
        void* arguments[] = {
            &input, &gradient_output, &gradient_input, &kernel_count
        };
        return launch_1d_locked(gelu_backward_f32_, arguments, kernel_count,
                                "cuLaunchKernel(gelu_backward_f32)",
                                KernelKind::GELU);
    }

    bool layer_norm_f32(DeviceHandle input_handle,
                        DeviceHandle weight_handle,
                        DeviceHandle bias_handle,
                        DeviceHandle output_handle,
                        DeviceHandle saved_mean_handle,
                        DeviceHandle saved_rstd_handle,
                        uint32_t rows, uint32_t features,
                        float epsilon, bool has_weight,
                        bool has_bias, bool save_stats) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (rows == 0 || features == 0
            || !std::isfinite(epsilon) || epsilon < 1e-12f || epsilon > 0.1f) {
            error_ = "layer_norm_f32 received invalid dimensions or epsilon";
            return false;
        }
        const uint64_t wide_count = (uint64_t)rows * features;
        if (wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "layer_norm_f32 exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        if (output_handle == input_handle
            || (has_weight && output_handle == weight_handle)
            || (has_bias && output_handle == bias_handle)) {
            error_ = "layer_norm_f32 requires a fresh output allocation";
            return false;
        }
        if (save_stats
            && (saved_mean_handle == saved_rstd_handle
                || saved_mean_handle == output_handle
                || saved_rstd_handle == output_handle
                || saved_mean_handle == input_handle
                || saved_rstd_handle == input_handle
                || (has_weight && (saved_mean_handle == weight_handle
                                   || saved_rstd_handle == weight_handle))
                || (has_bias && (saved_mean_handle == bias_handle
                                 || saved_rstd_handle == bias_handle)))) {
            error_ = "layer_norm_f32 saved statistics must use distinct allocations";
            return false;
        }
        CUdeviceptr input = 0, weight = 0, bias = 0, output = 0;
        CUdeviceptr saved_mean = 0, saved_rstd = 0;
        const size_t count = (size_t)wide_count;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "layer_norm_f32(input)")
            || !resolve_range_locked(output_handle, 0, count, output,
                                     "layer_norm_f32(output)")) return false;
        weight = input;
        bias = input;
        saved_mean = input;
        saved_rstd = input;
        if (has_weight
            && !resolve_range_locked(weight_handle, 0, features, weight,
                                     "layer_norm_f32(weight)")) return false;
        if (has_bias
            && !resolve_range_locked(bias_handle, 0, features, bias,
                                     "layer_norm_f32(bias)")) return false;
        if (save_stats
            && (!resolve_range_locked(saved_mean_handle, 0, rows, saved_mean,
                                      "layer_norm_f32(saved_mean)")
                || !resolve_range_locked(saved_rstd_handle, 0, rows, saved_rstd,
                                         "layer_norm_f32(saved_rstd)"))) return false;
        uint32_t weight_flag = has_weight ? 1u : 0u;
        uint32_t bias_flag = has_bias ? 1u : 0u;
        uint32_t save_flag = save_stats ? 1u : 0u;
        void* arguments[] = {
            &input, &weight, &bias, &output, &saved_mean, &saved_rstd,
            &rows, &features, &epsilon, &weight_flag, &bias_flag, &save_flag
        };
        return launch_1d_locked(layer_norm_f32_, arguments, rows,
                                "cuLaunchKernel(layer_norm_f32)",
                                KernelKind::LAYER_NORM);
    }

    bool layer_norm_backward_f32(DeviceHandle input_handle,
                                 DeviceHandle weight_handle,
                                 DeviceHandle gradient_output_handle,
                                 DeviceHandle saved_mean_handle,
                                 DeviceHandle saved_rstd_handle,
                                 DeviceHandle gradient_input_handle,
                                 uint32_t rows, uint32_t features,
                                 float epsilon, bool has_weight) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t wide_count = (uint64_t)rows * features;
        if (rows == 0 || features == 0
            || !std::isfinite(epsilon) || epsilon < 1e-12f || epsilon > 0.1f
            || wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "layer_norm_backward_f32 received invalid dimensions or epsilon";
            return false;
        }
        if (saved_mean_handle == saved_rstd_handle) {
            error_ = "layer_norm_backward_f32 requires distinct saved statistics";
            return false;
        }
        const DeviceHandle read_handles[] = {
            input_handle, gradient_output_handle,
            saved_mean_handle, saved_rstd_handle
        };
        for (DeviceHandle handle : read_handles) {
            if (gradient_input_handle == handle) {
                error_ = "layer_norm_backward_f32 requires a fresh gradient output";
                return false;
            }
        }
        if (has_weight && gradient_input_handle == weight_handle) {
            error_ = "layer_norm_backward_f32 gradient aliases weight";
            return false;
        }
        const size_t count = (size_t)wide_count;
        CUdeviceptr input = 0, weight = 0, gradient_output = 0;
        CUdeviceptr saved_mean = 0, saved_rstd = 0, gradient_input = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "layer_norm_backward_f32(input)")
            || !resolve_range_locked(gradient_output_handle, 0, count,
                                     gradient_output,
                                     "layer_norm_backward_f32(gradient_output)")
            || !resolve_range_locked(saved_mean_handle, 0, rows, saved_mean,
                                     "layer_norm_backward_f32(saved_mean)")
            || !resolve_range_locked(saved_rstd_handle, 0, rows, saved_rstd,
                                     "layer_norm_backward_f32(saved_rstd)")
            || !resolve_range_locked(gradient_input_handle, 0, count,
                                     gradient_input,
                                     "layer_norm_backward_f32(gradient_input)")) return false;
        weight = input;
        if (has_weight
            && !resolve_range_locked(weight_handle, 0, features, weight,
                                     "layer_norm_backward_f32(weight)")) return false;
        uint32_t weight_flag = has_weight ? 1u : 0u;
        void* arguments[] = {
            &input, &weight, &gradient_output, &saved_mean, &saved_rstd,
            &gradient_input, &rows, &features, &epsilon, &weight_flag
        };
        return launch_1d_locked(layer_norm_backward_f32_, arguments, rows,
                                "cuLaunchKernel(layer_norm_backward_f32)",
                                KernelKind::LAYER_NORM);
    }

    bool layer_norm_parameter_backward_f32(
            DeviceHandle input_handle,
            DeviceHandle gradient_output_handle,
            DeviceHandle saved_mean_handle,
            DeviceHandle saved_rstd_handle,
            DeviceHandle gradient_weight_handle,
            DeviceHandle gradient_bias_handle,
            uint32_t rows, uint32_t features,
            float epsilon, bool need_weight, bool need_bias) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t wide_count = (uint64_t)rows * features;
        if (rows == 0 || features == 0 || (!need_weight && !need_bias)
            || !std::isfinite(epsilon) || epsilon < 1e-12f || epsilon > 0.1f
            || wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "layer_norm_parameter_backward_f32 received invalid arguments";
            return false;
        }
        if (saved_mean_handle == saved_rstd_handle) {
            error_ = "layer_norm_parameter_backward_f32 requires distinct saved statistics";
            return false;
        }
        if (need_weight && need_bias
            && gradient_weight_handle == gradient_bias_handle) {
            error_ = "LayerNorm weight and bias gradients must be distinct";
            return false;
        }
        const DeviceHandle read_handles[] = {
            input_handle, gradient_output_handle,
            saved_mean_handle, saved_rstd_handle
        };
        for (DeviceHandle read : read_handles) {
            if ((need_weight && gradient_weight_handle == read)
                || (need_bias && gradient_bias_handle == read)) {
                error_ = "LayerNorm parameter gradient aliases a read-only input";
                return false;
            }
        }
        const size_t count = (size_t)wide_count;
        CUdeviceptr input = 0, gradient_output = 0;
        CUdeviceptr saved_mean = 0, saved_rstd = 0;
        CUdeviceptr gradient_weight = 0, gradient_bias = 0;
        if (!resolve_range_locked(input_handle, 0, count, input,
                                  "layer_norm_parameter_backward_f32(input)")
            || !resolve_range_locked(gradient_output_handle, 0, count,
                                     gradient_output,
                                     "layer_norm_parameter_backward_f32(gradient_output)")
            || !resolve_range_locked(saved_mean_handle, 0, rows, saved_mean,
                                     "layer_norm_parameter_backward_f32(saved_mean)")
            || !resolve_range_locked(saved_rstd_handle, 0, rows, saved_rstd,
                                     "layer_norm_parameter_backward_f32(saved_rstd)")) return false;
        gradient_weight = input;
        gradient_bias = input;
        if (need_weight
            && !resolve_range_locked(gradient_weight_handle, 0, features,
                                     gradient_weight,
                                     "layer_norm_parameter_backward_f32(gradient_weight)")) {
            return false;
        }
        if (need_bias
            && !resolve_range_locked(gradient_bias_handle, 0, features,
                                     gradient_bias,
                                     "layer_norm_parameter_backward_f32(gradient_bias)")) {
            return false;
        }
        uint32_t weight_flag = need_weight ? 1u : 0u;
        uint32_t bias_flag = need_bias ? 1u : 0u;
        void* arguments[] = {
            &input, &gradient_output, &saved_mean, &saved_rstd,
            &gradient_weight, &gradient_bias, &rows, &features,
            &epsilon, &weight_flag, &bias_flag
        };
        return launch_1d_locked(layer_norm_parameter_backward_f32_, arguments,
                                features,
                                "cuLaunchKernel(layer_norm_parameter_backward_f32)",
                                KernelKind::LAYER_NORM);
    }

    bool embedding_f32(DeviceHandle weight_handle,
                       DeviceHandle ids_handle,
                       DeviceHandle output_handle,
                       uint32_t vocabulary, uint32_t tokens,
                       uint32_t dimensions) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (vocabulary == 0 || tokens == 0 || dimensions == 0) {
            error_ = "embedding_f32 received invalid dimensions";
            return false;
        }
        const uint64_t weight_count = (uint64_t)vocabulary * dimensions;
        const uint64_t output_count = (uint64_t)tokens * dimensions;
        if (weight_count > std::numeric_limits<uint32_t>::max()
            || output_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "embedding_f32 exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        if (output_handle == weight_handle || output_handle == ids_handle) {
            error_ = "embedding_f32 requires a fresh output allocation";
            return false;
        }
        CUdeviceptr weight = 0, ids = 0, output = 0;
        if (!resolve_range_locked(weight_handle, 0, (size_t)weight_count,
                                  weight, "embedding_f32(weight)")
            || !resolve_storage_range_locked(ids_handle, 0, tokens,
                                             TensorStorage::UINT32, ids,
                                             "embedding_f32(ids)")
            || !resolve_range_locked(output_handle, 0, (size_t)output_count,
                                     output, "embedding_f32(output)")) return false;
        uint32_t count = (uint32_t)output_count;
        void* arguments[] = {
            &weight, &ids, &output, &count, &vocabulary, &dimensions
        };
        return launch_1d_locked(embedding_f32_, arguments, count,
                                "cuLaunchKernel(embedding_f32)",
                                KernelKind::EMBEDDING);
    }

    bool embedding_backward_f32(DeviceHandle ids_handle,
                                DeviceHandle gradient_output_handle,
                                DeviceHandle gradient_weight_handle,
                                uint32_t vocabulary, uint32_t tokens,
                                uint32_t dimensions) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        if (vocabulary == 0 || tokens == 0 || dimensions == 0) {
            error_ = "embedding_backward_f32 received invalid dimensions";
            return false;
        }
        const uint64_t weight_count = (uint64_t)vocabulary * dimensions;
        const uint64_t output_count = (uint64_t)tokens * dimensions;
        if (weight_count > std::numeric_limits<uint32_t>::max()
            || output_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "embedding_backward_f32 exceeds the uint32 CUDA kernel index limit";
            return false;
        }
        if (gradient_weight_handle == ids_handle
            || gradient_weight_handle == gradient_output_handle) {
            error_ = "embedding_backward_f32 requires a fresh gradient allocation";
            return false;
        }
        CUdeviceptr ids = 0, gradient_output = 0, gradient_weight = 0;
        if (!resolve_storage_range_locked(ids_handle, 0, tokens,
                                          TensorStorage::UINT32, ids,
                                          "embedding_backward_f32(ids)")
            || !resolve_range_locked(gradient_output_handle, 0,
                                     (size_t)output_count, gradient_output,
                                     "embedding_backward_f32(gradient_output)")
            || !resolve_range_locked(gradient_weight_handle, 0,
                                     (size_t)weight_count, gradient_weight,
                                     "embedding_backward_f32(gradient_weight)")) return false;
        void* arguments[] = {
            &ids, &gradient_output, &gradient_weight,
            &vocabulary, &tokens, &dimensions
        };
        return launch_1d_locked(embedding_backward_f32_, arguments, dimensions,
                                "cuLaunchKernel(embedding_backward_f32)",
                                KernelKind::EMBEDDING);
    }

    bool cross_entropy_ids_f32(DeviceHandle logits_handle,
                               DeviceHandle ids_handle,
                               DeviceHandle loss_handle,
                               DeviceHandle saved_max_handle,
                               DeviceHandle saved_inv_sum_handle,
                               uint32_t rows, uint32_t classes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t wide_count = (uint64_t)rows * classes;
        if (rows == 0 || classes < 2
            || wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "cross_entropy_ids_f32 received invalid dimensions";
            return false;
        }
        const DeviceHandle buffers[] = {
            logits_handle, ids_handle, loss_handle,
            saved_max_handle, saved_inv_sum_handle
        };
        for (size_t first = 0; first < 5; ++first) {
            for (size_t second = first + 1; second < 5; ++second) {
                if (buffers[first] == buffers[second]) {
                    error_ = "cross_entropy_ids_f32 requires pairwise-distinct logical buffers";
                    return false;
                }
            }
        }
        CUdeviceptr logits = 0, ids = 0, loss = 0;
        CUdeviceptr saved_max = 0, saved_inv_sum = 0;
        if (!resolve_range_locked(logits_handle, 0, (size_t)wide_count,
                                  logits, "cross_entropy_ids_f32(logits)")
            || !resolve_storage_range_locked(ids_handle, 0, rows,
                                             TensorStorage::UINT32, ids,
                                             "cross_entropy_ids_f32(ids)")
            || !resolve_range_locked(loss_handle, 0, 1,
                                     loss, "cross_entropy_ids_f32(loss)")) return false;
        if (!resolve_range_locked(saved_max_handle, 0, rows, saved_max,
                                  "cross_entropy_ids_f32(saved_max)")
            || !resolve_range_locked(saved_inv_sum_handle, 0, rows,
                                     saved_inv_sum,
                                     "cross_entropy_ids_f32(saved_inv_sum)")) {
            return false;
        }
        void* stats_arguments[] = {
            &logits, &saved_max, &saved_inv_sum, &rows, &classes
        };
        if (!launch_1d_locked(cross_entropy_ids_stats_f32_, stats_arguments, rows,
                              "cuLaunchKernel(cross_entropy_ids_stats_f32)",
                              KernelKind::CROSS_ENTROPY)) return false;
        void* loss_arguments[] = {
            &logits, &ids, &saved_max, &saved_inv_sum, &loss, &rows, &classes
        };
        return launch_1d_locked(cross_entropy_ids_loss_f32_, loss_arguments, 1,
                                "cuLaunchKernel(cross_entropy_ids_loss_f32)",
                                KernelKind::CROSS_ENTROPY);
    }

    bool cross_entropy_ids_backward_f32(
            DeviceHandle logits_handle,
            DeviceHandle ids_handle,
            DeviceHandle gradient_loss_handle,
            DeviceHandle saved_max_handle,
            DeviceHandle saved_inv_sum_handle,
            DeviceHandle gradient_logits_handle,
            uint32_t rows, uint32_t classes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t wide_count = (uint64_t)rows * classes;
        if (rows == 0 || classes < 2
            || wide_count > std::numeric_limits<uint32_t>::max()) {
            error_ = "cross_entropy_ids_backward_f32 received invalid dimensions";
            return false;
        }
        const DeviceHandle buffers[] = {
            logits_handle, ids_handle, gradient_loss_handle,
            saved_max_handle, saved_inv_sum_handle, gradient_logits_handle
        };
        for (size_t first = 0; first < 6; ++first) {
            for (size_t second = first + 1; second < 6; ++second) {
                if (buffers[first] == buffers[second]) {
                    error_ = "cross_entropy_ids_backward_f32 requires pairwise-distinct logical buffers";
                    return false;
                }
            }
        }
        CUdeviceptr logits = 0, ids = 0, gradient_loss = 0;
        CUdeviceptr saved_max = 0, saved_inv_sum = 0, gradient_logits = 0;
        if (!resolve_range_locked(logits_handle, 0, (size_t)wide_count,
                                  logits, "cross_entropy_ids_backward_f32(logits)")
            || !resolve_storage_range_locked(ids_handle, 0, rows,
                                             TensorStorage::UINT32, ids,
                                             "cross_entropy_ids_backward_f32(ids)")
            || !resolve_range_locked(gradient_loss_handle, 0, 1,
                                     gradient_loss,
                                     "cross_entropy_ids_backward_f32(gradient_loss)")
            || !resolve_range_locked(saved_max_handle, 0, rows,
                                     saved_max,
                                     "cross_entropy_ids_backward_f32(saved_max)")
            || !resolve_range_locked(saved_inv_sum_handle, 0, rows,
                                     saved_inv_sum,
                                     "cross_entropy_ids_backward_f32(saved_inv_sum)")
            || !resolve_range_locked(gradient_logits_handle, 0,
                                     (size_t)wide_count, gradient_logits,
                                     "cross_entropy_ids_backward_f32(gradient_logits)")) {
            return false;
        }
        uint32_t count = (uint32_t)wide_count;
        void* arguments[] = {
            &logits, &ids, &gradient_loss, &saved_max, &saved_inv_sum,
            &gradient_logits, &count, &rows, &classes
        };
        return launch_1d_locked(cross_entropy_ids_backward_f32_, arguments,
                                count,
                                "cuLaunchKernel(cross_entropy_ids_backward_f32)",
                                KernelKind::CROSS_ENTROPY);
    }

    bool causal_attention_f32(
            DeviceHandle query_handle,
            DeviceHandle key_handle,
            DeviceHandle value_handle,
            DeviceHandle output_handle,
            DeviceHandle saved_max_handle,
            DeviceHandle saved_inv_sum_handle,
            uint32_t batches, uint32_t sequence, uint32_t dimensions,
            uint32_t value_dimensions, float scale, bool save_stats,
            bool warp_parallel, bool fast_warp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t rows_wide = (uint64_t)batches * sequence;
        const bool query_size_safe = dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / dimensions;
        const bool value_size_safe = value_dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / value_dimensions;
        const uint64_t query_wide = query_size_safe
            ? rows_wide * dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t value_wide = value_size_safe
            ? rows_wide * value_dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t limit = std::numeric_limits<uint32_t>::max();
        if (batches == 0 || sequence == 0 || dimensions == 0
            || value_dimensions == 0 || !query_size_safe || !value_size_safe
            || rows_wide > limit
            || query_wide > limit || value_wide > limit
            || !std::isfinite(scale) || scale <= 0.0f) {
            error_ = "causal_attention_f32 received invalid dimensions or scale";
            return false;
        }
        if (output_handle == query_handle || output_handle == key_handle
            || output_handle == value_handle) {
            error_ = "causal_attention_f32 requires a fresh output allocation";
            return false;
        }
        if (save_stats) {
            const DeviceHandle all[] = {
                query_handle, key_handle, value_handle, output_handle
            };
            if (saved_max_handle == saved_inv_sum_handle) {
                error_ = "causal_attention_f32 requires distinct saved statistics";
                return false;
            }
            for (DeviceHandle handle : all) {
                if (saved_max_handle == handle || saved_inv_sum_handle == handle) {
                    error_ = "causal_attention_f32 saved statistics alias tensor storage";
                    return false;
                }
            }
        } else if (saved_max_handle != INVALID_DEVICE_HANDLE
                   || saved_inv_sum_handle != INVALID_DEVICE_HANDLE) {
            error_ = "causal_attention_f32 received unused saved statistics";
            return false;
        }
        CUdeviceptr query = 0, key = 0, value = 0, output = 0;
        CUdeviceptr saved_max = 0, saved_inv_sum = 0;
        if (!resolve_range_locked(query_handle, 0, (size_t)query_wide,
                                  query, "causal_attention_f32(query)")
            || !resolve_range_locked(key_handle, 0, (size_t)query_wide,
                                     key, "causal_attention_f32(key)")
            || !resolve_range_locked(value_handle, 0, (size_t)value_wide,
                                     value, "causal_attention_f32(value)")
            || !resolve_range_locked(output_handle, 0, (size_t)value_wide,
                                     output, "causal_attention_f32(output)")) {
            return false;
        }
        if (save_stats
            && (!resolve_range_locked(saved_max_handle, 0, (size_t)rows_wide,
                                      saved_max, "causal_attention_f32(saved_max)")
                || !resolve_range_locked(saved_inv_sum_handle, 0,
                                         (size_t)rows_wide, saved_inv_sum,
                                         "causal_attention_f32(saved_inv_sum)"))) {
            return false;
        }
        uint32_t total_rows = (uint32_t)rows_wide;
        uint32_t save = save_stats ? 1U : 0U;
        void* arguments[] = {
            &query, &key, &value, &output, &saved_max, &saved_inv_sum,
            &total_rows, &sequence, &dimensions, &value_dimensions,
            &scale, &save
        };
        if (warp_parallel) {
            CUfunction kernel = fast_warp ? causal_attention_warp_fast_f32_
                                          : causal_attention_warp_f32_;
            const char* operation = fast_warp
                ? "cuLaunchKernel(causal_attention_warp_fast_f32)"
                : "cuLaunchKernel(causal_attention_warp_f32)";
            const bool launched = launch_warp_rows_locked(
                kernel, arguments, total_rows, operation);
            if (launched && fast_warp) {
                add_counter_locked(stats_.fast_attention_forward_launches, 1);
            }
            return launched;
        }
        return launch_1d_locked(causal_attention_f32_, arguments, total_rows,
                                "cuLaunchKernel(causal_attention_f32)",
                                KernelKind::ATTENTION_REFERENCE);
    }

    bool transpose_f32(DeviceHandle input_handle, DeviceHandle output_handle,
                       size_t count, const std::vector<size_t>& input_shape,
                       uint32_t first, uint32_t second) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "transpose_f32")) return false;
        if (input_shape.size() < 2 || input_shape.size() > 8
            || first >= input_shape.size() || second >= input_shape.size()
            || first == second || input_handle == output_handle) {
            error_ = "transpose_f32 received invalid rank, axes, or aliased output";
            return false;
        }
        std::array<uint32_t, 8> output_dims{};
        std::array<uint32_t, 8> input_strides{};
        output_dims.fill(1U);
        input_strides.fill(1U);
        uint64_t product = 1;
        for (size_t reverse = 0; reverse < input_shape.size(); ++reverse) {
            const size_t axis = input_shape.size() - 1 - reverse;
            const size_t dimension = input_shape[axis];
            if (dimension == 0
                || dimension > (size_t)std::numeric_limits<uint32_t>::max()
                || product > (uint64_t)std::numeric_limits<uint32_t>::max()) {
                error_ = "transpose_f32 shape exceeds the uint32 index limit";
                return false;
            }
            input_strides[axis] = (uint32_t)product;
            output_dims[axis] = (uint32_t)dimension;
            product *= dimension;
        }
        if (product != count
            || product > (uint64_t)std::numeric_limits<uint32_t>::max()) {
            error_ = "transpose_f32 shape does not match the allocation";
            return false;
        }
        std::swap(output_dims[first], output_dims[second]);
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count,
                                  input, "transpose_f32(input)")
            || !resolve_range_locked(output_handle, 0, count,
                                     output, "transpose_f32(output)")) {
            return false;
        }
        void* arguments[] = {
            &input, &output, &kernel_count, &first, &second,
            &output_dims[0], &output_dims[1], &output_dims[2], &output_dims[3],
            &output_dims[4], &output_dims[5], &output_dims[6], &output_dims[7],
            &input_strides[0], &input_strides[1], &input_strides[2],
            &input_strides[3], &input_strides[4], &input_strides[5],
            &input_strides[6], &input_strides[7]
        };
        return launch_1d_locked(transpose_f32_, arguments, kernel_count,
                                "cuLaunchKernel(transpose_f32)",
                                KernelKind::TRANSPOSE);
    }

    bool causal_attention_backward_f32(
            DeviceHandle query_handle,
            DeviceHandle key_handle,
            DeviceHandle value_handle,
            DeviceHandle output_handle,
            DeviceHandle gradient_output_handle,
            DeviceHandle saved_max_handle,
            DeviceHandle saved_inv_sum_handle,
            DeviceHandle gradient_query_handle,
            DeviceHandle gradient_key_handle,
            DeviceHandle gradient_value_handle,
            uint32_t batches, uint32_t sequence, uint32_t dimensions,
            uint32_t value_dimensions, float scale,
            bool need_query, bool need_key, bool need_value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t rows_wide = (uint64_t)batches * sequence;
        const bool query_size_safe = dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / dimensions;
        const bool value_size_safe = value_dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / value_dimensions;
        const uint64_t query_wide = query_size_safe
            ? rows_wide * dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t value_wide = value_size_safe
            ? rows_wide * value_dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t limit = std::numeric_limits<uint32_t>::max();
        if (batches == 0 || sequence == 0 || dimensions == 0
            || value_dimensions == 0 || !query_size_safe || !value_size_safe
            || rows_wide > limit
            || query_wide > limit || value_wide > limit
            || (!need_query && !need_key && !need_value)
            || !std::isfinite(scale) || scale <= 0.0f) {
            error_ = "causal_attention_backward_f32 received invalid dimensions or scale";
            return false;
        }
        const DeviceHandle reads[] = {
            query_handle, key_handle, value_handle, output_handle,
            gradient_output_handle, saved_max_handle, saved_inv_sum_handle
        };
        const DeviceHandle writes[] = {
            gradient_query_handle, gradient_key_handle, gradient_value_handle
        };
        if (need_query != (gradient_query_handle != INVALID_DEVICE_HANDLE)
            || need_key != (gradient_key_handle != INVALID_DEVICE_HANDLE)
            || need_value != (gradient_value_handle != INVALID_DEVICE_HANDLE)) {
            error_ = "causal_attention_backward_f32 handle/gradient flags disagree";
            return false;
        }
        for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
            DeviceHandle write = writes[i];
            if (write == INVALID_DEVICE_HANDLE) continue;
            for (DeviceHandle read : reads) {
                if (write == read) {
                    error_ = "causal_attention_backward_f32 requires fresh gradient outputs";
                    return false;
                }
            }
            for (size_t j = i + 1; j < sizeof(writes) / sizeof(writes[0]); ++j) {
                if (writes[j] != INVALID_DEVICE_HANDLE && write == writes[j]) {
                    error_ = "causal_attention_backward_f32 gradient outputs alias";
                    return false;
                }
            }
        }
        CUdeviceptr query = 0, key = 0, value = 0, output = 0;
        CUdeviceptr gradient_output = 0, saved_max = 0, saved_inv_sum = 0;
        CUdeviceptr gradient_query = 0, gradient_key = 0, gradient_value = 0;
        if (!resolve_range_locked(query_handle, 0, (size_t)query_wide,
                                  query, "causal_attention_backward_f32(query)")
            || !resolve_range_locked(key_handle, 0, (size_t)query_wide,
                                     key, "causal_attention_backward_f32(key)")
            || !resolve_range_locked(value_handle, 0, (size_t)value_wide,
                                     value, "causal_attention_backward_f32(value)")
            || !resolve_range_locked(output_handle, 0, (size_t)value_wide,
                                     output, "causal_attention_backward_f32(output)")
            || !resolve_range_locked(gradient_output_handle, 0,
                                     (size_t)value_wide, gradient_output,
                                     "causal_attention_backward_f32(gradient_output)")
            || !resolve_range_locked(saved_max_handle, 0, (size_t)rows_wide,
                                     saved_max,
                                     "causal_attention_backward_f32(saved_max)")
            || !resolve_range_locked(saved_inv_sum_handle, 0,
                                     (size_t)rows_wide, saved_inv_sum,
                                     "causal_attention_backward_f32(saved_inv_sum)")) {
            return false;
        }
        if (need_query
            && !resolve_range_locked(gradient_query_handle, 0,
                                     (size_t)query_wide, gradient_query,
                                     "causal_attention_backward_f32(gradient_query)")) {
            return false;
        }
        if (need_key
            && !resolve_range_locked(gradient_key_handle, 0,
                                     (size_t)query_wide, gradient_key,
                                     "causal_attention_backward_f32(gradient_key)")) {
            return false;
        }
        if (need_value
            && !resolve_range_locked(gradient_value_handle, 0,
                                     (size_t)value_wide, gradient_value,
                                     "causal_attention_backward_f32(gradient_value)")) {
            return false;
        }
        uint32_t need_query_u32 = need_query ? 1U : 0U;
        uint32_t need_key_u32 = need_key ? 1U : 0U;
        uint32_t need_value_u32 = need_value ? 1U : 0U;
        void* arguments[] = {
            &query, &key, &value, &output, &gradient_output,
            &saved_max, &saved_inv_sum,
            &gradient_query, &gradient_key, &gradient_value,
            &batches, &sequence, &dimensions, &value_dimensions, &scale,
            &need_query_u32, &need_key_u32, &need_value_u32
        };
        return launch_1d_locked(causal_attention_backward_f32_, arguments,
                                batches,
                                "cuLaunchKernel(causal_attention_backward_f32)",
                                KernelKind::ATTENTION_REFERENCE);
    }

    bool causal_attention_parallel_backward_f32(
            DeviceHandle query_handle,
            DeviceHandle key_handle,
            DeviceHandle value_handle,
            DeviceHandle output_handle,
            DeviceHandle gradient_output_handle,
            DeviceHandle workspace_handle,
            DeviceHandle gradient_query_handle,
            DeviceHandle gradient_key_handle,
            DeviceHandle gradient_value_handle,
            uint32_t batches, uint32_t sequence, uint32_t dimensions,
            uint32_t value_dimensions, float scale,
            bool need_query, bool need_key, bool need_value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t rows_wide = (uint64_t)batches * sequence;
        const bool query_size_safe = dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / dimensions;
        const bool value_size_safe = value_dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / value_dimensions;
        const uint64_t query_wide = query_size_safe
            ? rows_wide * dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t value_wide = value_size_safe
            ? rows_wide * value_dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t pairs_per_batch_wide =
            (uint64_t)sequence * (sequence + 1ULL) / 2ULL;
        const bool pair_size_safe = pairs_per_batch_wide == 0
            || batches <= std::numeric_limits<uint64_t>::max()
                          / pairs_per_batch_wide;
        const uint64_t pairs_wide = pair_size_safe
            ? (uint64_t)batches * pairs_per_batch_wide
            : std::numeric_limits<uint64_t>::max();
        const uint64_t limit = std::numeric_limits<uint32_t>::max();
        if (batches == 0 || sequence == 0 || dimensions == 0
            || value_dimensions == 0 || !query_size_safe || !value_size_safe
            || !pair_size_safe || (!need_query && !need_key && !need_value)
            || rows_wide > limit || query_wide > limit || value_wide > limit
            || pairs_per_batch_wide > limit || pairs_wide > limit
            || !std::isfinite(scale) || scale <= 0.0f) {
            error_ = "causal_attention_parallel_backward_f32 received invalid arguments";
            return false;
        }
        if (workspace_handle == INVALID_DEVICE_HANDLE
            || (need_query != (gradient_query_handle != INVALID_DEVICE_HANDLE))
            || (need_key != (gradient_key_handle != INVALID_DEVICE_HANDLE))
            || (need_value != (gradient_value_handle != INVALID_DEVICE_HANDLE))) {
            error_ = "causal_attention_parallel_backward_f32 handle/gradient flags disagree";
            return false;
        }
        const DeviceHandle reads[] = {
            query_handle, key_handle, value_handle, output_handle,
            gradient_output_handle
        };
        const DeviceHandle writes[] = {
            workspace_handle, gradient_query_handle,
            gradient_key_handle, gradient_value_handle
        };
        for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
            if (writes[i] == INVALID_DEVICE_HANDLE) continue;
            for (DeviceHandle read : reads) {
                if (writes[i] == read) {
                    error_ = "causal_attention_parallel_backward_f32 requires fresh work/output buffers";
                    return false;
                }
            }
            for (size_t j = i + 1; j < sizeof(writes) / sizeof(writes[0]); ++j) {
                if (writes[j] != INVALID_DEVICE_HANDLE && writes[i] == writes[j]) {
                    error_ = "causal_attention_parallel_backward_f32 output buffers alias";
                    return false;
                }
            }
        }
        CUdeviceptr query = 0, key = 0, value = 0, output = 0;
        CUdeviceptr gradient_output = 0, workspace = 0;
        CUdeviceptr gradient_query = 0, gradient_key = 0, gradient_value = 0;
        if (!resolve_range_locked(query_handle, 0, (size_t)query_wide,
                                  query, "causal_attention_parallel(query)")
            || !resolve_range_locked(key_handle, 0, (size_t)query_wide,
                                     key, "causal_attention_parallel(key)")
            || !resolve_range_locked(value_handle, 0, (size_t)value_wide,
                                     value, "causal_attention_parallel(value)")
            || !resolve_range_locked(output_handle, 0, (size_t)value_wide,
                                     output, "causal_attention_parallel(output)")
            || !resolve_range_locked(gradient_output_handle, 0,
                                     (size_t)value_wide, gradient_output,
                                     "causal_attention_parallel(gradient_output)")
            || !resolve_range_locked(workspace_handle, 0,
                                     (size_t)pairs_wide, workspace,
                                     "causal_attention_parallel(workspace)")) {
            return false;
        }
        if (need_query
            && !resolve_range_locked(gradient_query_handle, 0,
                                     (size_t)query_wide, gradient_query,
                                     "causal_attention_parallel(gradient_query)")) {
            return false;
        }
        if (need_key
            && !resolve_range_locked(gradient_key_handle, 0,
                                     (size_t)query_wide, gradient_key,
                                     "causal_attention_parallel(gradient_key)")) {
            return false;
        }
        if (need_value
            && !resolve_range_locked(gradient_value_handle, 0,
                                     (size_t)value_wide, gradient_value,
                                     "causal_attention_parallel(gradient_value)")) {
            return false;
        }
        uint32_t total_rows = (uint32_t)rows_wide;
        uint32_t query_count = (uint32_t)query_wide;
        uint32_t value_count = (uint32_t)value_wide;
        uint32_t pairs_per_batch = (uint32_t)pairs_per_batch_wide;
        void* probability_arguments[] = {
            &query, &key, &workspace, &total_rows,
            &sequence, &dimensions, &scale, &pairs_per_batch
        };
        if (!launch_warp_rows_locked(
                causal_attention_probabilities_f32_, probability_arguments,
                total_rows,
                "cuLaunchKernel(causal_attention_probabilities_f32)",
                KernelKind::ATTENTION_PARALLEL)) return false;
        if (need_value) {
            void* value_arguments[] = {
                &workspace, &gradient_output, &gradient_value,
                &value_count, &sequence, &value_dimensions, &pairs_per_batch
            };
            if (!launch_1d_locked(
                    causal_attention_value_backward_f32_, value_arguments,
                    value_count,
                    "cuLaunchKernel(causal_attention_value_backward_f32)",
                    KernelKind::ATTENTION_PARALLEL)) return false;
        }
        if (need_query || need_key) {
            void* score_arguments[] = {
                &value, &output, &gradient_output,
                &workspace, &workspace, &total_rows,
                &sequence, &value_dimensions, &scale, &pairs_per_batch
            };
            if (!launch_1d_locked(
                    causal_attention_score_backward_f32_, score_arguments,
                    total_rows,
                    "cuLaunchKernel(causal_attention_score_backward_f32)",
                    KernelKind::ATTENTION_PARALLEL)) return false;
        }
        if (need_query) {
            void* query_arguments[] = {
                &key, &workspace, &gradient_query,
                &query_count, &sequence, &dimensions, &pairs_per_batch
            };
            if (!launch_1d_locked(
                    causal_attention_query_backward_f32_, query_arguments,
                    query_count,
                    "cuLaunchKernel(causal_attention_query_backward_f32)",
                    KernelKind::ATTENTION_PARALLEL)) return false;
        }
        if (need_key) {
            void* key_arguments[] = {
                &query, &workspace, &gradient_key,
                &query_count, &sequence, &dimensions, &pairs_per_batch
            };
            if (!launch_1d_locked(
                    causal_attention_key_backward_f32_, key_arguments,
                    query_count,
                    "cuLaunchKernel(causal_attention_key_backward_f32)",
                    KernelKind::ATTENTION_PARALLEL)) return false;
        }
        return true;
    }

    // Deterministic FlashAttention-style recomputation path. The forward
    // stores only one max/inverse-sum pair per row; backward owns every output
    // gradient element in exactly one warp and therefore needs neither an
    // O(T^2) workspace nor atomic updates.
    bool causal_attention_fused_backward_f32(
            DeviceHandle query_handle,
            DeviceHandle key_handle,
            DeviceHandle value_handle,
            DeviceHandle output_handle,
            DeviceHandle gradient_output_handle,
            DeviceHandle saved_max_handle,
            DeviceHandle saved_inv_sum_handle,
            DeviceHandle gradient_query_handle,
            DeviceHandle gradient_key_handle,
            DeviceHandle gradient_value_handle,
            uint32_t batches, uint32_t sequence, uint32_t dimensions,
            uint32_t value_dimensions, float scale,
            bool need_query, bool need_key, bool need_value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        const uint64_t rows_wide = (uint64_t)batches * sequence;
        const bool query_size_safe = dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / dimensions;
        const bool value_size_safe = value_dimensions == 0
            || rows_wide <= std::numeric_limits<uint64_t>::max() / value_dimensions;
        const uint64_t query_wide = query_size_safe
            ? rows_wide * dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t value_wide = value_size_safe
            ? rows_wide * value_dimensions : std::numeric_limits<uint64_t>::max();
        const uint64_t dimension_tiles_wide = ((uint64_t)dimensions + 31ULL) / 32ULL;
        const uint64_t value_tiles_wide = ((uint64_t)value_dimensions + 31ULL) / 32ULL;
        const uint64_t query_work_wide = rows_wide * dimension_tiles_wide;
        const uint64_t kv_tiles_wide = std::max(
            need_key ? dimension_tiles_wide : 0ULL,
            need_value ? value_tiles_wide : 0ULL);
        const uint64_t kv_work_wide = rows_wide * kv_tiles_wide;
        const uint64_t limit = std::numeric_limits<uint32_t>::max();
        if (batches == 0 || sequence == 0 || dimensions == 0
            || value_dimensions == 0 || !query_size_safe || !value_size_safe
            || (!need_query && !need_key && !need_value)
            || rows_wide > limit || query_wide > limit || value_wide > limit
            || dimension_tiles_wide == 0 || dimension_tiles_wide > limit
            || value_tiles_wide == 0 || value_tiles_wide > limit
            || (need_query && query_work_wide > limit)
            || ((need_key || need_value)
                && (kv_tiles_wide == 0 || kv_tiles_wide > limit
                    || kv_work_wide > limit))
            || !std::isfinite(scale) || scale <= 0.0f) {
            error_ = "causal_attention_fused_backward_f32 received invalid arguments";
            return false;
        }
        if (need_query != (gradient_query_handle != INVALID_DEVICE_HANDLE)
            || need_key != (gradient_key_handle != INVALID_DEVICE_HANDLE)
            || need_value != (gradient_value_handle != INVALID_DEVICE_HANDLE)) {
            error_ = "causal_attention_fused_backward_f32 handle/gradient flags disagree";
            return false;
        }
        const DeviceHandle reads[] = {
            query_handle, key_handle, value_handle, output_handle,
            gradient_output_handle, saved_max_handle, saved_inv_sum_handle
        };
        const DeviceHandle writes[] = {
            gradient_query_handle, gradient_key_handle, gradient_value_handle
        };
        for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
            if (writes[i] == INVALID_DEVICE_HANDLE) continue;
            for (DeviceHandle read : reads) {
                if (writes[i] == read) {
                    error_ = "causal_attention_fused_backward_f32 requires fresh gradient outputs";
                    return false;
                }
            }
            for (size_t j = i + 1; j < sizeof(writes) / sizeof(writes[0]); ++j) {
                if (writes[j] != INVALID_DEVICE_HANDLE && writes[i] == writes[j]) {
                    error_ = "causal_attention_fused_backward_f32 gradient outputs alias";
                    return false;
                }
            }
        }
        CUdeviceptr query = 0, key = 0, value = 0, output = 0;
        CUdeviceptr gradient_output = 0, saved_max = 0, saved_inv_sum = 0;
        CUdeviceptr gradient_query = 0, gradient_key = 0, gradient_value = 0;
        if (!resolve_range_locked(query_handle, 0, (size_t)query_wide,
                                  query, "causal_attention_fused(query)")
            || !resolve_range_locked(key_handle, 0, (size_t)query_wide,
                                     key, "causal_attention_fused(key)")
            || !resolve_range_locked(value_handle, 0, (size_t)value_wide,
                                     value, "causal_attention_fused(value)")
            || !resolve_range_locked(output_handle, 0, (size_t)value_wide,
                                     output, "causal_attention_fused(output)")
            || !resolve_range_locked(gradient_output_handle, 0,
                                     (size_t)value_wide, gradient_output,
                                     "causal_attention_fused(gradient_output)")
            || !resolve_range_locked(saved_max_handle, 0, (size_t)rows_wide,
                                     saved_max, "causal_attention_fused(saved_max)")
            || !resolve_range_locked(saved_inv_sum_handle, 0,
                                     (size_t)rows_wide, saved_inv_sum,
                                     "causal_attention_fused(saved_inv_sum)")) {
            return false;
        }
        if (need_query
            && !resolve_range_locked(gradient_query_handle, 0,
                                     (size_t)query_wide, gradient_query,
                                     "causal_attention_fused(gradient_query)")) {
            return false;
        }
        if (need_key
            && !resolve_range_locked(gradient_key_handle, 0,
                                     (size_t)query_wide, gradient_key,
                                     "causal_attention_fused(gradient_key)")) {
            return false;
        }
        if (need_value
            && !resolve_range_locked(gradient_value_handle, 0,
                                     (size_t)value_wide, gradient_value,
                                     "causal_attention_fused(gradient_value)")) {
            return false;
        }
        uint32_t total_rows = (uint32_t)rows_wide;
        if (need_query) {
            uint32_t dimension_tiles = (uint32_t)dimension_tiles_wide;
            uint32_t work_warps = (uint32_t)query_work_wide;
            void* arguments[] = {
                &query, &key, &value, &output, &gradient_output,
                &saved_max, &saved_inv_sum, &gradient_query,
                &work_warps, &total_rows, &sequence, &dimensions,
                &value_dimensions, &scale, &dimension_tiles
            };
            if (!launch_warp_rows_locked(
                    causal_attention_fused_query_backward_f32_, arguments,
                    work_warps,
                    "cuLaunchKernel(causal_attention_fused_query_backward_f32)",
                    KernelKind::ATTENTION_FUSED)) {
                return false;
            }
        }
        if (need_key || need_value) {
            uint32_t feature_tiles = (uint32_t)kv_tiles_wide;
            uint32_t work_warps = (uint32_t)kv_work_wide;
            uint32_t need_key_u32 = need_key ? 1U : 0U;
            uint32_t need_value_u32 = need_value ? 1U : 0U;
            void* arguments[] = {
                &query, &key, &value, &output, &gradient_output,
                &saved_max, &saved_inv_sum, &gradient_key, &gradient_value,
                &work_warps, &total_rows, &sequence, &dimensions,
                &value_dimensions, &scale, &feature_tiles,
                &need_key_u32, &need_value_u32
            };
            if (!launch_warp_rows_locked(
                    causal_attention_fused_key_value_backward_f32_, arguments,
                    work_warps,
                    "cuLaunchKernel(causal_attention_fused_key_value_backward_f32)",
                    KernelKind::ATTENTION_FUSED)) {
                return false;
            }
        }
        return true;
    }

    bool sum_f32(DeviceHandle input_handle, DeviceHandle scalar_output_handle,
                 size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        uint32_t kernel_count = 0;
        if (!kernel_count_locked(count, kernel_count, "sum_f32")) return false;
        CUdeviceptr input = 0, output = 0;
        if (!resolve_range_locked(input_handle, 0, count, input, "sum_f32(input)")
            || !resolve_range_locked(scalar_output_handle, 0, 1, output,
                                     "sum_f32(output)")) return false;
        void* arguments[] = {&input, &output, &kernel_count};
        if (!require(cuLaunchKernel_(sum_f32_, 1, 1, 1, 1, 1, 1,
                                    0, nullptr, arguments, nullptr),
                     "cuLaunchKernel(sum_f32)")) return false;
        record_kernel_locked(KernelKind::REDUCTION);
        return true;
    }

    // Supports the two transpose combinations required by matmul backward:
    // dA = dC * B^T and dB = A^T * dC. Stored matrices remain contiguous and
    // row-major; transposition happens only in the kernel's index calculation.
    bool matmul_device_f32(DeviceHandle left_handle,
                           DeviceHandle right_handle,
                           DeviceHandle output_handle,
                           uint32_t rows, uint32_t cols, uint32_t inner,
                           bool transpose_left = false,
                           bool transpose_right = false,
                           MatmulCompute compute = MatmulCompute::FLOAT32) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        size_t left_elements = 0, right_elements = 0, output_elements = 0;
        if (!matrix_sizes_locked(rows, cols, inner, left_elements,
                                 right_elements, output_elements,
                                 "matmul_device_f32")) return false;
        if (output_handle == left_handle || output_handle == right_handle) {
            error_ = "matmul_device_f32 does not permit an input/output alias";
            return false;
        }
        CUdeviceptr left = 0, right = 0, output = 0;
        if (!resolve_range_locked(left_handle, 0, left_elements, left,
                                  "matmul_device_f32(left)")
            || !resolve_range_locked(right_handle, 0, right_elements, right,
                                     "matmul_device_f32(right)")
            || !resolve_range_locked(output_handle, 0, output_elements, output,
                                     "matmul_device_f32(output)")) return false;
        return launch_matmul_ex_locked(left, right, output, rows, cols, inner,
                                       transpose_left, transpose_right, compute,
                                       "cuLaunchKernel(matmul_device_f32)");
    }

    // Typed inputs retain their native 2-byte storage while accumulation and
    // output remain float32. This keeps gradients numerically stable and lets
    // backward multiply an f32 upstream gradient by a low-precision operand.
    bool matmul_device_typed(DeviceHandle left_handle,
                             DeviceHandle right_handle,
                             DeviceHandle output_handle,
                             uint32_t rows, uint32_t cols, uint32_t inner,
                             bool transpose_left = false,
                             bool transpose_right = false,
                             MatmulCompute compute = MatmulCompute::FLOAT32) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        size_t left_elements = 0, right_elements = 0, output_elements = 0;
        if (!matrix_sizes_locked(rows, cols, inner, left_elements,
                                 right_elements, output_elements,
                                 "matmul_device_typed")) return false;
        if (output_handle == left_handle || output_handle == right_handle) {
            error_ = "matmul_device_typed does not permit an input/output alias";
            return false;
        }
        auto left_found = allocations_.find(left_handle);
        auto right_found = allocations_.find(right_handle);
        if (left_handle == INVALID_DEVICE_HANDLE || left_found == allocations_.end()
            || right_handle == INVALID_DEVICE_HANDLE
            || right_found == allocations_.end()) {
            error_ = "matmul_device_typed received an invalid or freed input handle";
            return false;
        }
        const TensorStorage left_storage = left_found->second.storage;
        const TensorStorage right_storage = right_found->second.storage;
        CUdeviceptr left = 0, right = 0, output = 0;
        if (!resolve_storage_range_locked(left_handle, 0, left_elements,
                                          left_storage, left,
                                          "matmul_device_typed(left)")
            || !resolve_storage_range_locked(right_handle, 0, right_elements,
                                             right_storage, right,
                                             "matmul_device_typed(right)")
            || !resolve_range_locked(output_handle, 0, output_elements, output,
                                     "matmul_device_typed(output)")) {
            return false;
        }
        return launch_matmul_typed_locked(left, left_storage, right, right_storage,
                                          output, rows, cols, inner,
                                          transpose_left, transpose_right,
                                          compute,
                                          "cuLaunchKernel(matmul_device_typed)");
    }

    bool matmul_f32(const std::vector<float>& left,
                    const std::vector<float>& right,
                    std::vector<float>& output,
                    uint32_t rows, uint32_t cols, uint32_t inner) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepare_locked()) return false;
        size_t left_elements = 0, right_elements = 0, output_elements = 0;
        if (!matrix_sizes_locked(rows, cols, inner, left_elements,
                                 right_elements, output_elements,
                                 "matmul_f32")) return false;
        if (left.size() != left_elements || right.size() != right_elements) {
            error_ = "CUDA matmul received inconsistent buffer sizes";
            return false;
        }
        try {
            output.assign(output_elements, 0.0f);
        } catch (...) {
            error_ = "host allocation failed while preparing CUDA matmul output";
            return false;
        }
        size_t left_bytes = left.size() * sizeof(float);
        size_t right_bytes = right.size() * sizeof(float);
        size_t output_bytes = output.size() * sizeof(float);
        CUdeviceptr device_left = 0, device_right = 0, device_output = 0;
        auto cleanup = [&](bool report_error) {
            bool ok = true;
            auto release = [&](CUdeviceptr& pointer, size_t bytes,
                               const char* operation) {
                if (!pointer) return;
                CUresult result = cuMemFree_(pointer);
                if (result == CUDA_SUCCESS) {
                    record_free_locked(bytes);
                    pointer = 0;
                } else if (report_error && ok) {
                    ok = require(result, operation);
                }
            };
            release(device_output, output_bytes, "cuMemFree(output)");
            release(device_right, right_bytes, "cuMemFree(right)");
            release(device_left, left_bytes, "cuMemFree(left)");
            return ok;
        };
        if (!require(cuMemAlloc_(&device_left, left_bytes), "cuMemAlloc(left)")) {
            cleanup(false); return false;
        }
        record_allocation_locked(left_bytes);
        if (!require(cuMemAlloc_(&device_right, right_bytes), "cuMemAlloc(right)")) {
            cleanup(false); return false;
        }
        record_allocation_locked(right_bytes);
        if (!require(cuMemAlloc_(&device_output, output_bytes), "cuMemAlloc(output)")) {
            cleanup(false); return false;
        }
        record_allocation_locked(output_bytes);
        if (!require(cuMemcpyHtoD_(device_left, left.data(), left_bytes),
                     "cuMemcpyHtoD(left)")) {
            cleanup(false); return false;
        }
        add_counter_locked(stats_.h2d_bytes, (uint64_t)left_bytes);
        if (!require(cuMemcpyHtoD_(device_right, right.data(), right_bytes),
                     "cuMemcpyHtoD(right)")) {
            cleanup(false); return false;
        }
        add_counter_locked(stats_.h2d_bytes, (uint64_t)right_bytes);
        if (!launch_matmul_ex_locked(device_left, device_right, device_output,
                                     rows, cols, inner, false, false,
                                     MatmulCompute::FLOAT32,
                                     "matmul_f32")) {
            cleanup(false); return false;
        }
        if (!require(cuCtxSynchronize_(), "cuCtxSynchronize(matmul_f32)")) {
            cleanup(false); return false;
        }
        if (!require(cuMemcpyDtoH_(output.data(), device_output, output_bytes),
                     "cuMemcpyDtoH(output)")) {
            cleanup(false); return false;
        }
        add_counter_locked(stats_.d2h_bytes, (uint64_t)output_bytes);
        return cleanup(true);
    }
};

} // namespace SuraStd

#undef SURA_CUDA_CALL
