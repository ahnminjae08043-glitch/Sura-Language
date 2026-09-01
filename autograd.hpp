#pragma once

// Included by stdlib.hpp after its shared argument/option helpers are defined.
// The implementation lives in its own header to keep the native Tensor and
// first-order autograd runtime auditable without growing the stdlib registry.
namespace SuraStd {

// Shared ownership gives CUDA storage an exact lifetime independent of the GC
// graph.  The opaque driver handle prevents raw CUdeviceptr access from
// escaping the backend.  Destruction is deliberately noexcept; a late driver
// free failure remains observable in backend stats/error state and the driver
// performs a final allocation sweep before destroying its context.
struct AgCudaAllocation {
    SuraCudaDriver::DeviceHandle handle = SuraCudaDriver::INVALID_DEVICE_HANDLE;
    const size_t elements;
    const int device_index;
    const SuraCudaDriver::TensorStorage storage;

    AgCudaAllocation(SuraCudaDriver::DeviceHandle value, size_t count, int device,
                     SuraCudaDriver::TensorStorage storage_kind)
        : handle(value), elements(count), device_index(device),
          storage(storage_kind) {}
    ~AgCudaAllocation() noexcept {
        if (handle != SuraCudaDriver::INVALID_DEVICE_HANDLE) {
            SuraCudaDriver::instance().free_device(handle);
        }
    }
    AgCudaAllocation(const AgCudaAllocation&) = delete;
    AgCudaAllocation& operator=(const AgCudaAllocation&) = delete;
};

static constexpr size_t AG_MAX_RANK = 8;
static constexpr size_t AG_DEFAULT_MAX_ELEMENTS = 10000000;
static constexpr size_t AG_HARD_MAX_ELEMENTS = 1000000000;
static constexpr size_t AG_MAX_GRAPH_NODES = 1000000;
static constexpr size_t AG_DEFAULT_MAX_EXTERNAL_BYTES = 512ULL * 1024ULL * 1024ULL;
static constexpr size_t AG_HARD_MAX_EXTERNAL_BYTES = 64ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr size_t AG_DEFAULT_MAX_ATTENTION_SCORES = 50000000;
static constexpr size_t AG_HARD_MAX_ATTENTION_SCORES = 5000000000ULL;
static constexpr size_t AG_DEFAULT_CUDA_ATTENTION_WORKSPACE_BYTES =
    64ULL * 1024ULL * 1024ULL;
static constexpr size_t AG_HARD_CUDA_ATTENTION_WORKSPACE_BYTES =
    4096ULL * 1024ULL * 1024ULL;
static constexpr size_t AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE = 8;

inline size_t ag_environment_limit(const char* name, size_t fallback, size_t maximum) {
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || !end || *end != '\0' || parsed == 0
        || parsed > (unsigned long long)maximum) {
        return fallback;
    }
    return (size_t)parsed;
}

inline size_t ag_max_elements() {
    static const size_t limit = ag_environment_limit(
        "SURA_TENSOR_MAX_ELEMENTS", AG_DEFAULT_MAX_ELEMENTS,
        AG_HARD_MAX_ELEMENTS);
    return limit;
}

inline size_t ag_max_external_bytes() {
    static const size_t limit_mb = ag_environment_limit(
        "SURA_TENSOR_MEMORY_LIMIT_MB",
        AG_DEFAULT_MAX_EXTERNAL_BYTES / (1024ULL * 1024ULL),
        AG_HARD_MAX_EXTERNAL_BYTES / (1024ULL * 1024ULL));
    return limit_mb * 1024ULL * 1024ULL;
}

inline size_t ag_max_attention_scores() {
    static const size_t limit = ag_environment_limit(
        "SURA_ATTENTION_MAX_SCORES", AG_DEFAULT_MAX_ATTENTION_SCORES,
        AG_HARD_MAX_ATTENTION_SCORES);
    return limit;
}

inline size_t ag_cuda_attention_workspace_limit() {
    static const size_t limit_mb = ag_environment_limit(
        "SURA_CUDA_ATTENTION_WORKSPACE_MB",
        AG_DEFAULT_CUDA_ATTENTION_WORKSPACE_BYTES / (1024ULL * 1024ULL),
        AG_HARD_CUDA_ATTENTION_WORKSPACE_BYTES / (1024ULL * 1024ULL));
    return limit_mb * 1024ULL * 1024ULL;
}

inline bool ag_cuda_attention_parallel_enabled() {
    const char* value = std::getenv("SURA_CUDA_ATTENTION_PARALLEL");
    if (!value || !*value) return true;
    const std::string text(value);
    return text != "0" && text != "false" && text != "off";
}

inline bool ag_cuda_attention_fused_enabled() {
    if (!ag_cuda_attention_parallel_enabled()) return false;
    const char* value = std::getenv("SURA_CUDA_ATTENTION_FUSED");
    if (!value || !*value) return true;
    const std::string text(value);
    return text != "0" && text != "false" && text != "off";
}

inline bool ag_cuda_attention_fused_plan(size_t sequence) {
    return sequence >= AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE
        && ag_cuda_attention_fused_enabled();
}

struct AgCudaAutocastState {
    bool enabled = false;
    TensorDType dtype = TensorDType::BFLOAT16;
};

inline AgCudaAutocastState& ag_cuda_autocast_storage() {
    static thread_local AgCudaAutocastState state;
    return state;
}

inline bool ag_cuda_attention_workspace_requirements(
        size_t batches, size_t sequence, uint64_t& total_pairs,
        uint64_t& workspace_bytes) {
    total_pairs = std::numeric_limits<uint64_t>::max();
    workspace_bytes = std::numeric_limits<uint64_t>::max();
    const uint64_t limit = std::numeric_limits<uint64_t>::max();
    const uint64_t sequence_wide = (uint64_t)sequence;
    if ((size_t)sequence_wide != sequence || sequence_wide == limit) return false;
    const uint64_t next_sequence = sequence_wide + 1ULL;
    if (sequence_wide != 0 && next_sequence > limit / sequence_wide) return false;
    const uint64_t pairs_per_batch = sequence_wide * next_sequence / 2ULL;
    if (pairs_per_batch != 0
        && (uint64_t)batches > limit / pairs_per_batch) return false;
    total_pairs = pairs_per_batch * (uint64_t)batches;
    if (total_pairs > limit / sizeof(float)) {
        total_pairs = std::numeric_limits<uint64_t>::max();
        return false;
    }
    workspace_bytes = total_pairs * sizeof(float);
    return true;
}

inline bool ag_cuda_attention_parallel_plan(
        size_t batches, size_t sequence, uint64_t& total_pairs,
        uint64_t& workspace_bytes) {
    const bool sizes_safe = ag_cuda_attention_workspace_requirements(
        batches, sequence, total_pairs, workspace_bytes);
    return sizes_safe
        && sequence >= AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE
        && ag_cuda_attention_parallel_enabled()
        && total_pairs <= (uint64_t)std::numeric_limits<uint32_t>::max()
        && workspace_bytes <= (uint64_t)ag_cuda_attention_workspace_limit();
}

inline size_t& ag_temporary_bytes_storage() {
    static thread_local size_t bytes = 0;
    return bytes;
}

using AgGcCallback = void(*)(void*);

inline AgGcCallback& ag_gc_callback_storage() {
    static thread_local AgGcCallback callback = nullptr;
    return callback;
}

inline void*& ag_gc_callback_context_storage() {
    static thread_local void* context = nullptr;
    return context;
}

class AgGcCallbackScope {
    AgGcCallback previous_callback_ = nullptr;
    void* previous_context_ = nullptr;
public:
    AgGcCallbackScope(AgGcCallback callback, void* context)
        : previous_callback_(ag_gc_callback_storage()),
          previous_context_(ag_gc_callback_context_storage()) {
        ag_gc_callback_storage() = callback;
        ag_gc_callback_context_storage() = context;
    }
    ~AgGcCallbackScope() {
        ag_gc_callback_storage() = previous_callback_;
        ag_gc_callback_context_storage() = previous_context_;
    }
    AgGcCallbackScope(const AgGcCallbackScope&) = delete;
    AgGcCallbackScope& operator=(const AgGcCallbackScope&) = delete;
};

inline bool ag_allocation_fits(size_t bytes) {
    size_t maximum = ag_max_external_bytes();
    size_t current = tensor_external_bytes_storage().load(std::memory_order_relaxed);
    size_t temporary = ag_temporary_bytes_storage();
    return current <= maximum && temporary <= maximum - current
        && bytes <= maximum - current - temporary;
}

inline void ag_preflight_bytes(size_t bytes, const char* name, int line) {
    if (!ag_allocation_fits(bytes) && ag_gc_callback_storage()) {
        ag_gc_callback_storage()(ag_gc_callback_context_storage());
    }
    if (!ag_allocation_fits(bytes)) {
        size_t maximum = ag_max_external_bytes();
        throw JitThrow{std::string(name) + "(): allocation would exceed the configured "
                       + std::to_string(maximum / (1024ULL * 1024ULL))
                       + " MiB tensor memory limit", line};
    }
}

class AgTemporaryBytes {
    size_t bytes_ = 0;
public:
    AgTemporaryBytes(size_t bytes, const char* name, int line) : bytes_(bytes) {
        ag_preflight_bytes(bytes_, name, line);
        ag_temporary_bytes_storage() += bytes_;
    }
    ~AgTemporaryBytes() { ag_temporary_bytes_storage() -= bytes_; }
    AgTemporaryBytes(const AgTemporaryBytes&) = delete;
    AgTemporaryBytes& operator=(const AgTemporaryBytes&) = delete;
};

inline bool ag_is_cuda(const GCTensor* tensor) {
    return tensor && tensor->cuda_data
        && tensor->cuda_data->handle != SuraCudaDriver::INVALID_DEVICE_HANDLE;
}

inline std::string ag_device_text(const GCTensor* tensor) {
    if (!ag_is_cuda(tensor)) return "cpu";
    return "cuda:" + std::to_string(tensor->cuda_data->device_index);
}

[[noreturn]] inline void ag_cuda_fail(const char* name, int line) {
    throw JitThrow{std::string(name) + "(): CUDA backend failed: "
                   + SuraCudaDriver::instance().error(), line};
}

inline SuraCudaDriver::TensorStorage ag_cuda_storage_for_dtype(
        const char* name, TensorDType dtype, int line) {
    switch (dtype) {
        case TensorDType::FLOAT32:
            return SuraCudaDriver::TensorStorage::FLOAT32;
        case TensorDType::FLOAT16:
            return SuraCudaDriver::TensorStorage::FLOAT16;
        case TensorDType::BFLOAT16:
            return SuraCudaDriver::TensorStorage::BFLOAT16;
        case TensorDType::FLOAT64:
            break;
    }
    throw JitThrow{std::string(name)
                   + "(): resident CUDA storage supports float32, float16, or bfloat16",
                   line};
}

inline TensorDType ag_cuda_dtype_for_storage(
        const char* name, SuraCudaDriver::TensorStorage storage, int line) {
    switch (storage) {
        case SuraCudaDriver::TensorStorage::FLOAT32:
            return TensorDType::FLOAT32;
        case SuraCudaDriver::TensorStorage::FLOAT16:
            return TensorDType::FLOAT16;
        case SuraCudaDriver::TensorStorage::BFLOAT16:
            return TensorDType::BFLOAT16;
        case SuraCudaDriver::TensorStorage::UINT32:
            break;
    }
    throw JitThrow{std::string(name)
                   + "(): UINT32 CUDA storage is internal index data, not a tensor dtype",
                   line};
}

inline std::shared_ptr<AgCudaAllocation> ag_cuda_allocate_kind(
        const char* name, size_t elements,
        SuraCudaDriver::TensorStorage storage, int line) {
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    SuraCudaDriver::DeviceHandle handle = SuraCudaDriver::INVALID_DEVICE_HANDLE;
    bool allocated = false;
    switch (storage) {
        case SuraCudaDriver::TensorStorage::FLOAT32:
            allocated = driver.allocate_f32(elements, handle);
            break;
        case SuraCudaDriver::TensorStorage::FLOAT16:
            allocated = driver.allocate_f16(elements, handle);
            break;
        case SuraCudaDriver::TensorStorage::BFLOAT16:
            allocated = driver.allocate_bfloat16(elements, handle);
            break;
        case SuraCudaDriver::TensorStorage::UINT32:
            allocated = driver.allocate_u32(elements, handle);
            break;
    }
    if (!allocated) ag_cuda_fail(name, line);
    try {
        return std::make_shared<AgCudaAllocation>(
            handle, elements, driver.device_index(), storage);
    } catch (...) {
        driver.free_device(handle);
        throw;
    }
}

inline std::shared_ptr<AgCudaAllocation> ag_cuda_allocate_storage(
        const char* name, size_t elements, TensorDType dtype, int line) {
    return ag_cuda_allocate_kind(
        name, elements, ag_cuda_storage_for_dtype(name, dtype, line), line);
}

// Gradients, workspaces, optimizer states, and existing elementwise kernels
// intentionally continue to use this f32 allocation helper.
inline std::shared_ptr<AgCudaAllocation> ag_cuda_allocate(
        const char* name, size_t elements, int line) {
    return ag_cuda_allocate_storage(
        name, elements, TensorDType::FLOAT32, line);
}

// Embedding and cross-entropy IDs are raw integer buffers.  Keeping them out
// of ag_cuda_allocate_storage prevents them from masquerading as f32 tensors.
inline std::shared_ptr<AgCudaAllocation> ag_cuda_allocate_u32(
        const char* name, size_t elements, int line) {
    return ag_cuda_allocate_kind(
        name, elements, SuraCudaDriver::TensorStorage::UINT32, line);
}

inline void ag_cuda_require_float32(const char* name,
                                    const GCTensor* tensor, int line) {
    if (tensor->data.dtype() != TensorDType::FLOAT32) {
        throw JitThrow{std::string(name)
                       + "(): this CUDA kernel requires float32 storage", line};
    }
}

inline void ag_cuda_require_tensor(const char* name, const GCTensor* tensor,
                                   int line);

inline void ag_cuda_upload(GCTensor* tensor, const char* name, int line) {
    const auto storage = ag_cuda_storage_for_dtype(
        name, tensor->data.dtype(), line);
    if (!tensor->data.host_readable()) {
        throw JitThrow{std::string(name)
                       + "(): cannot upload a stale host mirror", line};
    }
    if (ag_is_cuda(tensor)) {
        ag_cuda_require_tensor(name, tensor, line);
        return;
    }
    if (!SuraCudaDriver::instance().available()) ag_cuda_fail(name, line);
    const size_t bytes = tensor->data.byte_size();
    AgTemporaryBytes staging(bytes, name, line);
    auto allocation = ag_cuda_allocate_storage(
        name, tensor->data.size(), tensor->data.dtype(), line);
    bool uploaded = false;
    if (storage == SuraCudaDriver::TensorStorage::FLOAT32) {
        std::vector<float> packed(tensor->data.size());
        if (bytes != 0) {
            std::memcpy(packed.data(), tensor->data.packed_data(), bytes);
        }
        uploaded = SuraCudaDriver::instance().upload_f32(
            allocation->handle, packed);
    } else {
        std::vector<uint16_t> packed(tensor->data.size());
        if (bytes != 0) {
            std::memcpy(packed.data(), tensor->data.packed_data(), bytes);
        }
        uploaded = SuraCudaDriver::instance().upload_u16(
            allocation->handle, storage, packed);
    }
    if (!uploaded) {
        ag_cuda_fail(name, line);
    }
    tensor->cuda_data = std::move(allocation);
    tensor->coherence = TensorCoherence::SYNCHRONIZED;
}

// Host materialization is defined before the general CPU buffer helpers, so
// declare the accounting pair used by its lazy-allocation transaction here.
inline void ag_reserve_bytes(GCTensor* tensor, size_t bytes,
                             const char* name, int line);
inline void ag_release_bytes(GCTensor* tensor, size_t bytes);

inline void ag_cuda_materialize_host(GCTensor* tensor,
                                     const char* name, int line) {
    if (tensor->coherence == TensorCoherence::DEVICE_ONLY && !ag_is_cuda(tensor)) {
        throw JitThrow{std::string(name)
                       + "(): tensor lost its authoritative CUDA allocation", line};
    }
    if (tensor->coherence == TensorCoherence::SYNCHRONIZED) {
        const size_t expected_bytes = tensor->data.byte_size();
        if (!tensor->data.host_readable()
            || tensor->data.host_byte_size() != expected_bytes) {
            throw JitThrow{std::string(name) + "(): corrupted tensor coherence state", line};
        }
    }
    if (!ag_is_cuda(tensor)) return;
    ag_cuda_require_tensor(name, tensor, line);
    if (tensor->coherence != TensorCoherence::DEVICE_ONLY) return;
    const size_t count = tensor->data.size();
    const TensorDType dtype = tensor->data.dtype();
    const auto storage = tensor->cuda_data->storage;
    const size_t element_bytes = tensor_dtype_size(dtype);
    if (count > std::numeric_limits<size_t>::max() / element_bytes) {
        throw JitThrow{std::string(name) + "(): host materialization size overflows", line};
    }
    const size_t host_bytes = count * element_bytes;
    const size_t resident_host_bytes = tensor->data.host_byte_size();
    if (resident_host_bytes != 0 && resident_host_bytes != host_bytes) {
        throw JitThrow{std::string(name) + "(): corrupted CUDA host mirror size", line};
    }
    const bool added_host_reservation = resident_host_bytes == 0;
    if (added_host_reservation) {
        // Give the embedding VM its normal GC opportunity before atomically
        // claiming a new persistent host mirror.
        ag_preflight_bytes(host_bytes, name, line);
        ag_reserve_bytes(tensor, host_bytes, name, line);
    }
    try {
        // The download buffer is transient.  TensorBuffer::assign_packed builds
        // a second buffer transactionally; its future persistent reservation
        // is already counted, so an existing stale mirror is the only other
        // temporary allocation that must be charged here.
        const size_t temporary_buffers = resident_host_bytes ? 2 : 1;
        if (host_bytes > std::numeric_limits<size_t>::max() / temporary_buffers) {
            throw JitThrow{std::string(name) + "(): host materialization size overflows", line};
        }
        AgTemporaryBytes staging(host_bytes * temporary_buffers, name, line);
        TensorBuffer replacement;
        if (storage == SuraCudaDriver::TensorStorage::FLOAT32) {
            std::vector<float> packed;
            if (!SuraCudaDriver::instance().download_f32(
                    tensor->cuda_data->handle, packed, count)) {
                ag_cuda_fail(name, line);
            }
            replacement.assign_packed(packed.data(), count, dtype);
        } else {
            std::vector<uint16_t> packed;
            if (!SuraCudaDriver::instance().download_u16(
                    tensor->cuda_data->handle, storage, packed, count)) {
                ag_cuda_fail(name, line);
            }
            replacement.assign_packed(packed.data(), count, dtype);
        }
        for (size_t i = 0; i < count; ++i) {
            if (!std::isfinite(replacement[i])) {
                throw JitThrow{std::string(name)
                               + "(): CUDA tensor contains a non-finite value", line};
            }
        }
        tensor->data = std::move(replacement);
        tensor->coherence = TensorCoherence::SYNCHRONIZED;
    } catch (...) {
        if (added_host_reservation) ag_release_bytes(tensor, host_bytes);
        throw;
    }
}

inline void ag_cuda_mark_device_only(GCTensor* tensor) {
    tensor->coherence = TensorCoherence::DEVICE_ONLY;
    tensor->data.mark_host_stale();
}

inline bool ag_parse_device_request(const char* name, const std::string& device,
                                    int line) {
    if (device == "cpu") return false;
    if (device != "cuda" && device.rfind("cuda:", 0) != 0) {
        throw JitThrow{std::string(name)
                       + "(): device must be cpu, cuda, or cuda:<index>", line};
    }
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    if (!driver.available()) ag_cuda_fail(name, line);
    if (device.rfind("cuda:", 0) == 0) {
        const std::string index_text = device.substr(5);
        if (index_text.empty()
            || !std::all_of(index_text.begin(), index_text.end(),
                            [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
            throw JitThrow{std::string(name) + "(): invalid CUDA device index", line};
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(index_text.c_str(), &end, 10);
        if (!end || *end != '\0' || parsed > (unsigned long)std::numeric_limits<int>::max()
            || (int)parsed != driver.device_index()) {
            throw JitThrow{std::string(name)
                           + "(): requested CUDA device is not the process-selected device; "
                             "set SURA_CUDA_DEVICE before launch", line};
        }
    }
    return true;
}

inline bool ag_device_option_cuda(const char* name, const GCDict* options,
                                  int line) {
    if (!options) return false;
    auto found = options->elements.find("device");
    if (found == options->elements.end()) return false;
    if (!found->second.is_str()) {
        throw JitThrow{std::string(name) + "(): option device must be a string", line};
    }
    return ag_parse_device_request(name, found->second.as_str_ref(), line);
}

inline Value ag_place_created_tensor(Value value, bool cuda,
                                     const char* name, int line) {
    if (!cuda) return value;
    GCNativeRoot root(value.as_obj());
    ag_cuda_upload(value.as_tensor(), name, line);
    return value;
}

inline void ag_validate_options(const char* name, const GCDict* options,
                                const std::vector<std::string>& allowed, int line) {
    if (!options) return;
    for (const auto& entry : options->elements) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
            throw JitThrow{std::string(name) + "(): unknown option '" + entry.first + "'", line};
        }
    }
}

inline TensorDType ag_parse_dtype(const char* name, const std::string& text, int line) {
    if (text == "float64" || text == "f64") return TensorDType::FLOAT64;
    if (text == "float32" || text == "f32") return TensorDType::FLOAT32;
    if (text == "float16" || text == "f16" || text == "half") return TensorDType::FLOAT16;
    if (text == "bfloat16" || text == "bf16") return TensorDType::BFLOAT16;
    throw JitThrow{std::string(name) + "(): dtype must be float64, float32, float16, or bfloat16", line};
}

inline TensorDType ag_option_dtype(const char* name, const GCDict* options,
                                   TensorDType fallback, int line) {
    if (!options) return fallback;
    auto found = options->elements.find("dtype");
    if (found == options->elements.end()) return fallback;
    if (!found->second.is_str()) {
        throw JitThrow{std::string(name) + "(): option dtype must be a string", line};
    }
    return ag_parse_dtype(name, found->second.as_str_ref(), line);
}

inline bool ag_has_option(const GCDict* options, const char* key) {
    return options && options->elements.find(key) != options->elements.end();
}

inline TensorDType ag_promote_dtype(TensorDType left, TensorDType right) {
    if (left == right) return left;
    if (left == TensorDType::FLOAT64 || right == TensorDType::FLOAT64) {
        return TensorDType::FLOAT64;
    }
    if (left == TensorDType::FLOAT32 || right == TensorDType::FLOAT32) {
        return TensorDType::FLOAT32;
    }
    // Mixing IEEE half and bfloat16 needs a wider common exponent/mantissa.
    return TensorDType::FLOAT32;
}

inline TensorDType ag_promote_dtype(TensorDType first, TensorDType second,
                                    TensorDType third) {
    return ag_promote_dtype(ag_promote_dtype(first, second), third);
}

inline SuraCudaDriver::MatmulCompute ag_cuda_matmul_compute(
        TensorDType dtype, const char* name, int line) {
    switch (dtype) {
        case TensorDType::FLOAT32:
            return SuraCudaDriver::MatmulCompute::FLOAT32;
        case TensorDType::FLOAT16:
            return SuraCudaDriver::MatmulCompute::FLOAT16;
        case TensorDType::BFLOAT16:
            return SuraCudaDriver::MatmulCompute::BFLOAT16;
        case TensorDType::FLOAT64:
            break;
    }
    throw JitThrow{std::string(name)
                   + "(): CUDA matmul compute_dtype must be float32, float16, or bfloat16",
                   line};
}

inline void ag_validate_dtype_values(const char* name, const std::vector<double>& values,
                                     TensorDType dtype, int line) {
    double maximum = tensor_dtype_max_finite(dtype);
    for (double value : values) {
        if (!std::isfinite(value)) {
            throw JitThrow{std::string(name) + "(): tensor data must remain finite", line};
        }
        if (std::abs(value) > maximum) {
            throw JitThrow{std::string(name) + "(): value overflows "
                           + tensor_dtype_name(dtype), line};
        }
    }
}

inline std::string ag_shape_text(const std::vector<size_t>& shape) {
    std::string out = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(shape[i]);
    }
    return out + "]";
}

inline bool ag_same_shape(const std::vector<size_t>& left, const std::vector<size_t>& right) {
    return left == right;
}

inline size_t ag_numel(const char* name, const std::vector<size_t>& shape, int line) {
    if (shape.size() > AG_MAX_RANK) {
        throw JitThrow{std::string(name) + "(): tensor rank exceeds "
                       + std::to_string(AG_MAX_RANK), line};
    }
    size_t maximum = ag_max_elements();
    size_t count = 1;
    for (size_t dim : shape) {
        if (dim == 0) throw JitThrow{std::string(name) + "(): tensor dimensions must be positive", line};
        if (count > maximum / dim) {
            throw JitThrow{std::string(name) + "(): tensor exceeds the "
                           + std::to_string(maximum) + " element safety limit", line};
        }
        count *= dim;
    }
    return count;
}

inline void ag_reserve_bytes(GCTensor* tensor, size_t bytes, const char* name, int line) {
    if (!bytes) return;
    auto& total = tensor_external_bytes_storage();
    size_t maximum = ag_max_external_bytes();
    size_t temporary = ag_temporary_bytes_storage();
    size_t before = total.fetch_add(bytes, std::memory_order_relaxed);
    if (before > maximum || temporary > maximum - before
        || bytes > maximum - before - temporary) {
        total.fetch_sub(bytes, std::memory_order_relaxed);
        throw JitThrow{std::string(name) + "(): live tensor buffers exceed the configured "
                       + std::to_string(maximum / (1024ULL * 1024ULL))
                       + " MiB safety limit", line};
    }
    tensor->tracked_bytes += bytes;
}

inline void ag_release_bytes(GCTensor* tensor, size_t bytes) {
    if (!bytes) return;
    if (bytes > tensor->tracked_bytes) bytes = tensor->tracked_bytes;
    tensor->tracked_bytes -= bytes;
    tensor_external_bytes_storage().fetch_sub(bytes, std::memory_order_relaxed);
}

inline void ag_ensure_grad(GCTensor* tensor, const char* name, int line) {
    if (tensor->grad.size() == tensor->data.size()) return;
    if (!tensor->grad.empty()) {
        throw JitThrow{std::string(name) + "(): corrupted gradient buffer", line};
    }
    size_t new_bytes = tensor->data.size() * sizeof(double);
    ag_reserve_bytes(tensor, new_bytes, name, line);
    try {
        tensor->grad.assign(tensor->data.size(), 0.0);
    } catch (...) {
        std::vector<double>().swap(tensor->grad);
        ag_release_bytes(tensor, new_bytes);
        throw;
    }
}

inline void ag_clear_grad_buffer(GCTensor* tensor) {
    size_t bytes = tensor->grad.size() * sizeof(double);
    std::vector<double>().swap(tensor->grad);
    ag_release_bytes(tensor, bytes);
}

inline std::vector<size_t> ag_infer_shape_impl(const char* name, const Value& value,
                                               std::unordered_set<const GCArray*>& active,
                                               size_t depth, int line) {
    if (value.is_num()) {
        if (!std::isfinite(value.as_num())) {
            throw JitThrow{std::string(name) + "(): tensor data must contain finite numbers", line};
        }
        return {};
    }
    if (!value.is_arr()) {
        throw JitThrow{std::string(name) + "(): tensor data must be a number or rectangular numeric array", line};
    }
    if (depth >= AG_MAX_RANK) {
        throw JitThrow{std::string(name) + "(): tensor rank exceeds "
                       + std::to_string(AG_MAX_RANK), line};
    }
    auto* array = value.as_arr();
    if (array->elements.empty()) {
        throw JitThrow{std::string(name) + "(): empty tensor dimensions are not supported", line};
    }
    if (!active.insert(array).second) {
        throw JitThrow{std::string(name) + "(): cyclic arrays cannot be converted to tensors", line};
    }
    std::vector<size_t> child = ag_infer_shape_impl(name, array->elements[0], active, depth + 1, line);
    for (size_t i = 1; i < array->elements.size(); ++i) {
        std::vector<size_t> current = ag_infer_shape_impl(name, array->elements[i], active, depth + 1, line);
        if (current != child) {
            active.erase(array);
            throw JitThrow{std::string(name) + "(): ragged arrays cannot be converted to tensors", line};
        }
    }
    active.erase(array);
    std::vector<size_t> shape;
    shape.reserve(child.size() + 1);
    shape.push_back(array->elements.size());
    shape.insert(shape.end(), child.begin(), child.end());
    return shape;
}

inline void ag_flatten_data(const Value& value, std::vector<double>& out) {
    if (value.is_num()) {
        out.push_back(value.as_num());
        return;
    }
    for (const auto& item : value.as_arr()->elements) ag_flatten_data(item, out);
}

inline void ag_parse_data(const char* name, const Value& value,
                          std::vector<size_t>& shape, std::vector<double>& data, int line) {
    std::unordered_set<const GCArray*> active;
    shape = ag_infer_shape_impl(name, value, active, 0, line);
    size_t count = ag_numel(name, shape, line);
    ag_preflight_bytes(count * sizeof(double), name, line);
    data.reserve(count);
    ag_flatten_data(value, data);
    if (data.size() != count) throw JitThrow{std::string(name) + "(): invalid tensor data", line};
}

inline std::vector<size_t> ag_parse_shape(const char* name, const Value& value, int line) {
    auto* array = need_arr(name, value, 0, line);
    if (array->elements.size() > AG_MAX_RANK) {
        throw JitThrow{std::string(name) + "(): tensor rank exceeds "
                       + std::to_string(AG_MAX_RANK), line};
    }
    std::vector<size_t> shape;
    shape.reserve(array->elements.size());
    size_t maximum = ag_max_elements();
    for (size_t i = 0; i < array->elements.size(); ++i) {
        if (!array->elements[i].is_num() || !std::isfinite(array->elements[i].as_num())) {
            throw JitThrow{std::string(name) + "(): shape values must be positive integers", line};
        }
        double raw = array->elements[i].as_num();
        if (raw < 1.0 || raw != std::floor(raw) || raw > (double)maximum) {
            throw JitThrow{std::string(name) + "(): shape values must be positive integers no larger than "
                           + std::to_string(maximum), line};
        }
        shape.push_back((size_t)raw);
    }
    ag_numel(name, shape, line);
    return shape;
}

inline std::vector<size_t> ag_parse_reshape_shape(const char* name, const Value& value,
                                                  size_t input_count, int line) {
    auto* array = need_arr(name, value, 1, line);
    if (array->elements.size() > AG_MAX_RANK) {
        throw JitThrow{std::string(name) + "(): tensor rank exceeds "
                       + std::to_string(AG_MAX_RANK), line};
    }
    std::vector<size_t> shape(array->elements.size(), 0);
    size_t maximum = ag_max_elements();
    size_t known = 1;
    size_t inferred = array->elements.size();
    for (size_t i = 0; i < array->elements.size(); ++i) {
        if (!array->elements[i].is_num() || !std::isfinite(array->elements[i].as_num())) {
            throw JitThrow{std::string(name) + "(): shape values must be positive integers or one -1", line};
        }
        double raw = array->elements[i].as_num();
        if (raw == -1.0) {
            if (inferred != array->elements.size()) {
                throw JitThrow{std::string(name) + "(): shape may contain at most one -1", line};
            }
            inferred = i;
            continue;
        }
        if (raw < 1.0 || raw != std::floor(raw) || raw > (double)maximum) {
            throw JitThrow{std::string(name) + "(): shape values must be positive integers or one -1", line};
        }
        size_t dim = (size_t)raw;
        if (known > input_count / dim && input_count != 0) {
            throw JitThrow{std::string(name) + "(): requested shape does not match tensor size", line};
        }
        known *= dim;
        shape[i] = dim;
    }
    if (inferred != array->elements.size()) {
        if (known == 0 || input_count % known != 0 || input_count / known == 0) {
            throw JitThrow{std::string(name) + "(): cannot infer reshape dimension", line};
        }
        shape[inferred] = input_count / known;
    }
    if (ag_numel(name, shape, line) != input_count) {
        throw JitThrow{std::string(name) + "(): requested shape does not match tensor size", line};
    }
    return shape;
}

inline size_t ag_product(const std::vector<size_t>& shape) {
    size_t result = 1;
    for (size_t dim : shape) result *= dim;
    return result;
}

inline std::vector<size_t> ag_prefix_shape(const std::vector<size_t>& shape, size_t trailing) {
    return std::vector<size_t>(shape.begin(), shape.end() - (ptrdiff_t)trailing);
}

inline void ag_validate_attention_work(const char* name, size_t batches,
                                       size_t sequence, int line) {
    size_t left = sequence;
    size_t right = sequence + 1;
    if ((left & 1U) == 0) left /= 2;
    else right /= 2;
    size_t maximum = ag_max_attention_scores();
    if (left != 0 && right > maximum / left) {
        throw JitThrow{std::string(name) + "(): causal attention exceeds the configured "
                       + std::to_string(maximum) + " score safety limit", line};
    }
    size_t pairs = left * right;
    if (pairs != 0 && batches > maximum / pairs) {
        throw JitThrow{std::string(name) + "(): causal attention exceeds the configured "
                       + std::to_string(maximum) + " score safety limit", line};
    }
}

inline Value ag_make_tensor(const char* name, std::vector<double> data,
                            std::vector<size_t> shape, bool requires_grad,
                            TensorOp op, std::vector<GCTensor*> parents, int line,
                            TensorDType dtype = TensorDType::FLOAT64) {
    if (data.size() != ag_numel(name, shape, line)) {
        throw JitThrow{std::string(name) + "(): internal tensor shape mismatch", line};
    }
    ag_validate_dtype_values(name, data, dtype, line);
    size_t bytes = data.size() * tensor_dtype_size(dtype);
    ag_preflight_bytes(bytes, name, line);
    Value value = Value::make_tensor();
    GCTensor* tensor = value.as_tensor();
    ag_reserve_bytes(tensor, bytes, name, line);
    try {
        tensor->data.assign(std::move(data), dtype);
        tensor->shape = std::move(shape);
        tensor->requires_grad = requires_grad;
        if (requires_grad && op != TensorOp::LEAF) {
            tensor->op = op;
            tensor->parents = std::move(parents);
            tensor->parent_versions.reserve(tensor->parents.size());
            for (auto* parent : tensor->parents) tensor->parent_versions.push_back(parent->version);
        }
    } catch (...) {
        tensor->data.clear_and_release();
        std::vector<size_t>().swap(tensor->shape);
        std::vector<GCTensor*>().swap(tensor->parents);
        std::vector<uint64_t>().swap(tensor->parent_versions);
        ag_release_bytes(tensor, bytes);
        throw;
    }
    return value;
}

inline Value ag_make_cuda_tensor(const char* name,
                                 std::vector<size_t> shape,
                                 bool requires_grad,
                                 TensorOp op,
                                 std::vector<GCTensor*> parents,
                                 int line,
                                 TensorDType dtype = TensorDType::FLOAT32) {
    size_t count = ag_numel(name, shape, line);
    const size_t element_bytes = tensor_dtype_size(dtype);
    if (count > std::numeric_limits<size_t>::max() / element_bytes) {
        throw JitThrow{std::string(name) + "(): CUDA tensor size overflows", line};
    }
    // Validate the public dtype before publishing any GC-visible tensor state.
    (void)ag_cuda_storage_for_dtype(name, dtype, line);
    // Operation outputs are genuinely device-only: retain logical metadata,
    // but allocate no stale CPU mirror and perform no CPU initialization/H2D.
    Value value = Value::make_tensor();
    GCNativeRoot root(value.as_obj());
    GCTensor* tensor = value.as_tensor();
    tensor->data.assign_device_metadata(count, dtype);
    tensor->shape = std::move(shape);
    tensor->requires_grad = requires_grad;
    if (requires_grad && op != TensorOp::LEAF) {
        tensor->op = op;
        tensor->parents = std::move(parents);
        tensor->parent_versions.reserve(tensor->parents.size());
        for (auto* parent : tensor->parents) {
            tensor->parent_versions.push_back(parent->version);
        }
    }
    tensor->cuda_data = ag_cuda_allocate_storage(name, count, dtype, line);
    ag_cuda_mark_device_only(tensor);
    return value;
}

inline Value ag_clone_tensor_value(const char* name, const GCTensor* source,
                                   bool requires_grad, TensorDType dtype,
                                   int line) {
    if (ag_is_cuda(source)) {
        ag_cuda_require_tensor(name, source, line);
        if (dtype != source->data.dtype()) {
            throw JitThrow{std::string(name)
                           + "(): resident CUDA clone requires the source dtype; use cast() "
                             "for a storage conversion", line};
        }
        Value result = ag_make_cuda_tensor(name, source->shape, requires_grad,
                                            TensorOp::LEAF, {}, line, dtype);
        GCNativeRoot result_root(result.as_obj());
        if (!SuraCudaDriver::instance().copy_storage(
                source->cuda_data->handle, 0,
                result.as_tensor()->cuda_data->handle, 0,
                source->data.size())) {
            ag_cuda_fail(name, line);
        }
        return result;
    }
    if (!source->data.host_readable()) {
        throw JitThrow{std::string(name) + "(): source tensor host data is stale", line};
    }
    return ag_make_tensor(name, source->data.to_vector(), source->shape,
                          requires_grad, TensorOp::LEAF, {}, line, dtype);
}

inline void ag_cuda_require_tensor(const char* name, const GCTensor* tensor,
                                   int line) {
    if (!ag_is_cuda(tensor)) {
        throw JitThrow{std::string(name)
                       + "(): mixed CPU/CUDA operands are not supported; use to() explicitly",
                       line};
    }
    const auto expected_storage = ag_cuda_storage_for_dtype(
        name, tensor->data.dtype(), line);
    if (tensor->cuda_data->storage != expected_storage) {
        throw JitThrow{std::string(name)
                       + "(): corrupted CUDA allocation dtype metadata", line};
    }
    if (tensor->cuda_data->elements != tensor->data.size()) {
        throw JitThrow{std::string(name) + "(): corrupted CUDA allocation size", line};
    }
    const size_t expected_host_bytes = tensor->data.byte_size();
    const size_t actual_host_bytes = tensor->data.host_byte_size();
    if (tensor->coherence == TensorCoherence::HOST_ONLY
        || (tensor->coherence == TensorCoherence::SYNCHRONIZED
            && (!tensor->data.host_readable()
                || actual_host_bytes != expected_host_bytes))
        || (tensor->coherence == TensorCoherence::DEVICE_ONLY
            && (tensor->data.host_readable()
                || (actual_host_bytes != 0
                    && actual_host_bytes != expected_host_bytes)))) {
        throw JitThrow{std::string(name)
                       + "(): corrupted CUDA tensor coherence state", line};
    }
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    if (!driver.available()) ag_cuda_fail(name, line);
    if (tensor->cuda_data->device_index != driver.device_index()) {
        throw JitThrow{
            std::string(name)
                + "(): CUDA allocation belongs to a different active device",
            line};
    }
}

inline void ag_cuda_require_f32_allocation(
        const char* name, const GCTensor* tensor,
        const std::shared_ptr<AgCudaAllocation>& allocation,
        const char* description, int line) {
    if (!allocation
        || allocation->handle == SuraCudaDriver::INVALID_DEVICE_HANDLE
        || allocation->elements != tensor->data.size()
        || allocation->device_index != tensor->cuda_data->device_index
        || allocation->storage != SuraCudaDriver::TensorStorage::FLOAT32) {
        throw JitThrow{std::string(name) + "(): corrupted CUDA "
                       + description, line};
    }
}

inline void ag_cuda_require_f32_tensor(const char* name,
                                       const GCTensor* tensor, int line) {
    ag_cuda_require_tensor(name, tensor, line);
    ag_cuda_require_float32(name, tensor, line);
}

inline void ag_cuda_require_same_device(const char* name,
                                        const GCTensor* left,
                                        const GCTensor* right,
                                        int line) {
    ag_cuda_require_tensor(name, left, line);
    ag_cuda_require_tensor(name, right, line);
    if (left->cuda_data->device_index != right->cuda_data->device_index) {
        throw JitThrow{std::string(name) + "(): CUDA operands are on different devices", line};
    }
}

inline void ag_cuda_reject_unsupported(const char* name,
                                       std::initializer_list<GCTensor*> tensors,
                                       int line) {
    for (GCTensor* tensor : tensors) {
        if (ag_is_cuda(tensor)) {
            throw JitThrow{std::string(name)
                           + "(): this operation has no resident CUDA kernel yet; "
                             "use to(tensor, \"cpu\") explicitly", line};
        }
    }
}

inline void ag_cuda_copy_all(const char* name,
                             const AgCudaAllocation& source,
                             const AgCudaAllocation& destination,
                             int line) {
    if (source.elements != destination.elements) {
        throw JitThrow{std::string(name)
                       + "(): CUDA copy requires equal allocation sizes", line};
    }
    if (source.storage != destination.storage) {
        throw JitThrow{std::string(name)
                       + "(): CUDA copy requires identical storage types", line};
    }
    if (!SuraCudaDriver::instance().copy_storage(
            source.handle, 0, destination.handle, 0, source.elements)) {
        ag_cuda_fail(name, line);
    }
}

inline void ag_set_op_indices(GCTensor* tensor, std::vector<size_t> indices,
                              const char* name, int line) {
    if (indices.empty()) return;
    size_t bytes = indices.size() * sizeof(size_t);
    GCNativeRoot tensor_root(tensor);
    ag_preflight_bytes(bytes, name, line);
    ag_reserve_bytes(tensor, bytes, name, line);
    try {
        tensor->op_indices = std::move(indices);
    } catch (...) {
        std::vector<size_t>().swap(tensor->op_indices);
        ag_release_bytes(tensor, bytes);
        throw;
    }
}

inline void ag_clear_op_indices(GCTensor* tensor) {
    size_t bytes = tensor->op_indices.size() * sizeof(size_t);
    std::vector<size_t>().swap(tensor->op_indices);
    ag_release_bytes(tensor, bytes);
}

inline void ag_release_graph_node_state(GCTensor* tensor) {
    std::vector<GCTensor*>().swap(tensor->parents);
    std::vector<uint64_t>().swap(tensor->parent_versions);
    ag_clear_op_indices(tensor);
    tensor->cuda_layer_norm_mean.reset();
    tensor->cuda_layer_norm_rstd.reset();
    tensor->cuda_embedding_ids.reset();
    tensor->cuda_cross_entropy_ids.reset();
    tensor->cuda_cross_entropy_max.reset();
    tensor->cuda_cross_entropy_inv_sum.reset();
    tensor->cuda_attention_max.reset();
    tensor->cuda_attention_inv_sum.reset();
    tensor->cuda_attention_parallel_plan = false;
    tensor->cuda_attention_fused_plan = false;
    tensor->cuda_matmul_compute_dtype = TensorDType::FLOAT32;
    tensor->op_scalar = 0.0;
    tensor->graph_freed = true;
}

inline Value ag_tensor_from_value(const char* name, const Value& value,
                                  bool requires_grad, int line,
                                  TensorDType dtype = TensorDType::FLOAT64,
                                  bool preserve_tensor_dtype = true) {
    if (value.is_tensor()) {
        GCTensor* source = value.as_tensor();
        TensorDType output_dtype = preserve_tensor_dtype ? source->data.dtype() : dtype;
        if (ag_is_cuda(source)) {
            return ag_clone_tensor_value(name, source, requires_grad,
                                         output_dtype, line);
        }
        size_t bytes = source->data.size() * tensor_dtype_size(output_dtype);
        ag_preflight_bytes(bytes, name, line);
        Value out = Value::make_tensor();
        GCTensor* tensor = out.as_tensor();
        ag_reserve_bytes(tensor, bytes, name, line);
        try {
            tensor->data.assign(source->data.to_vector(), output_dtype);
            tensor->shape = source->shape;
            tensor->requires_grad = requires_grad;
        } catch (...) {
            tensor->data.clear_and_release();
            std::vector<size_t>().swap(tensor->shape);
            ag_release_bytes(tensor, bytes);
            throw;
        }
        return out;
    }
    std::vector<size_t> shape;
    std::vector<double> data;
    ag_parse_data(name, value, shape, data, line);
    return ag_make_tensor(name, std::move(data), std::move(shape), requires_grad,
                          TensorOp::LEAF, {}, line, dtype);
}

inline GCTensor* ag_need_tensor(const char* name, const Value& value, int index, int line) {
    if (!value.is_tensor()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(index + 1)
                       + " must be a tensor, got " + value_type_name(value), line};
    }
    return value.as_tensor();
}

inline Value ag_coerce_tensor(const char* name, const Value& value, int line,
                              TensorDType preferred = TensorDType::FLOAT64) {
    if (value.is_tensor()) return value;
    if (value.is_num() || value.is_arr()) {
        return ag_tensor_from_value(name, value, false, line, preferred, false);
    }
    throw JitThrow{std::string(name) + "(): operands must be tensors, numbers, or numeric arrays", line};
}

inline void ag_check_graph_operand(const char* name, const GCTensor* tensor, int line) {
    if (tensor->requires_grad && tensor->op != TensorOp::LEAF && tensor->graph_freed) {
        throw JitThrow{std::string(name) + "(): an input graph was freed; call detach() before reusing it", line};
    }
}

inline std::vector<size_t> ag_broadcast_shape(const char* name,
                                              const std::vector<size_t>& left,
                                              const std::vector<size_t>& right,
                                              int line) {
    size_t rank = std::max(left.size(), right.size());
    if (rank > AG_MAX_RANK) throw JitThrow{std::string(name) + "(): broadcast rank is too large", line};
    std::vector<size_t> out(rank, 1);
    for (size_t reverse = 0; reverse < rank; ++reverse) {
        size_t ldim = reverse < left.size() ? left[left.size() - 1 - reverse] : 1;
        size_t rdim = reverse < right.size() ? right[right.size() - 1 - reverse] : 1;
        if (ldim != rdim && ldim != 1 && rdim != 1) {
            throw JitThrow{std::string(name) + "(): shapes " + ag_shape_text(left)
                           + " and " + ag_shape_text(right) + " cannot broadcast", line};
        }
        out[rank - 1 - reverse] = std::max(ldim, rdim);
    }
    ag_numel(name, out, line);
    return out;
}

inline size_t ag_broadcast_index(size_t output_index,
                                 const std::vector<size_t>& output_shape,
                                 const std::vector<size_t>& input_shape) {
    size_t input_index = 0;
    size_t input_stride = 1;
    size_t remaining = output_index;
    for (size_t reverse = 0; reverse < output_shape.size(); ++reverse) {
        size_t output_axis = output_shape.size() - 1 - reverse;
        size_t coordinate = remaining % output_shape[output_axis];
        remaining /= output_shape[output_axis];
        if (reverse < input_shape.size()) {
            size_t input_axis = input_shape.size() - 1 - reverse;
            if (input_shape[input_axis] != 1) input_index += coordinate * input_stride;
            input_stride *= input_shape[input_axis];
        }
    }
    return input_index;
}

template<typename Data>
inline Value ag_nested_value(const Data& data, const std::vector<size_t>& shape,
                             size_t depth, size_t& offset) {
    if (depth == shape.size()) return Value(data[offset++]);
    Value out = Value::make_array();
    out.as_arr()->elements.reserve(shape[depth]);
    for (size_t i = 0; i < shape[depth]; ++i) {
        out.as_arr()->elements.push_back(ag_nested_value(data, shape, depth + 1, offset));
    }
    return out;
}

template<typename Data>
inline Value ag_data_value(const Data& data, const std::vector<size_t>& shape) {
    size_t offset = 0;
    return ag_nested_value(data, shape, 0, offset);
}

inline Value b_autograd_tensor(const Value* a, int n, int l) {
    need_args("autograd_tensor", n, 1, 2, l);
    bool requires_grad = false;
    TensorDType dtype = TensorDType::FLOAT64;
    bool cuda = false;
    if (n >= 2) {
        if (a[1].is_bool()) {
            requires_grad = a[1].as_bool();
        } else if (a[1].is_str()) {
            dtype = ag_parse_dtype("autograd_tensor", a[1].as_str_ref(), l);
        } else if (a[1].is_dict()) {
            GCDict* options = a[1].as_dict();
            ag_validate_options("autograd_tensor", options,
                                {"requires_grad", "dtype", "device"}, l);
            requires_grad = nn_option_bool("autograd_tensor", options, "requires_grad", false, l);
            dtype = ag_option_dtype("autograd_tensor", options, TensorDType::FLOAT64, l);
            cuda = ag_device_option_cuda("autograd_tensor", options, l);
            if (cuda && !ag_has_option(options, "dtype")) dtype = TensorDType::FLOAT32;
        } else {
            throw JitThrow{"autograd_tensor(): arg 2 must be a bool, dtype string, or options dict", l};
        }
    }
    Value result = ag_tensor_from_value("autograd_tensor", a[0], requires_grad, l, dtype,
                                        n < 2 || a[1].is_bool());
    return ag_place_created_tensor(result, cuda, "autograd_tensor", l);
}

inline Value b_autograd_parameter(const Value* a, int n, int l) {
    need_args("autograd_parameter", n, 1, 2, l);
    TensorDType dtype = TensorDType::FLOAT64;
    bool preserve = true;
    bool cuda = false;
    if (n >= 2) {
        preserve = false;
        if (a[1].is_str()) {
            dtype = ag_parse_dtype("autograd_parameter", a[1].as_str_ref(), l);
        } else if (a[1].is_dict()) {
            GCDict* options = a[1].as_dict();
            ag_validate_options("autograd_parameter", options, {"dtype", "device"}, l);
            dtype = ag_option_dtype("autograd_parameter", options, TensorDType::FLOAT64, l);
            cuda = ag_device_option_cuda("autograd_parameter", options, l);
            if (cuda && !ag_has_option(options, "dtype")) dtype = TensorDType::FLOAT32;
        } else {
            throw JitThrow{"autograd_parameter(): arg 2 must be a dtype string or options dict", l};
        }
    }
    Value result = ag_tensor_from_value("autograd_parameter", a[0], true, l, dtype, preserve);
    return ag_place_created_tensor(result, cuda, "autograd_parameter", l);
}

inline Value ag_filled_tensor(const char* name, const Value* a, int n, int l, double fill) {
    need_args(name, n, 1, 2, l);
    std::vector<size_t> shape = ag_parse_shape(name, a[0], l);
    bool requires_grad = false;
    TensorDType dtype = TensorDType::FLOAT64;
    bool cuda = false;
    if (n >= 2) {
        if (a[1].is_bool()) {
            requires_grad = a[1].as_bool();
        } else if (a[1].is_str()) {
            dtype = ag_parse_dtype(name, a[1].as_str_ref(), l);
        } else if (a[1].is_dict()) {
            GCDict* options = a[1].as_dict();
            ag_validate_options(name, options, {"requires_grad", "dtype", "device"}, l);
            requires_grad = nn_option_bool(name, options, "requires_grad", false, l);
            dtype = ag_option_dtype(name, options, TensorDType::FLOAT64, l);
            cuda = ag_device_option_cuda(name, options, l);
            if (cuda && !ag_has_option(options, "dtype")) dtype = TensorDType::FLOAT32;
        } else {
            throw JitThrow{std::string(name) + "(): arg 2 must be a bool, dtype string, or options dict", l};
        }
    }
    size_t count = ag_numel(name, shape, l);
    ag_preflight_bytes(count * sizeof(double), name, l);
    Value result = ag_make_tensor(name, std::vector<double>(count, fill),
                                  std::move(shape), requires_grad,
                                  TensorOp::LEAF, {}, l, dtype);
    return ag_place_created_tensor(result, cuda, name, l);
}

inline Value b_autograd_zeros(const Value* a, int n, int l) {
    return ag_filled_tensor("autograd_zeros", a, n, l, 0.0);
}

inline Value b_autograd_ones(const Value* a, int n, int l) {
    return ag_filled_tensor("autograd_ones", a, n, l, 1.0);
}

inline Value b_autograd_randn(const Value* a, int n, int l) {
    need_args("autograd_randn", n, 1, 2, l);
    std::vector<size_t> shape = ag_parse_shape("autograd_randn", a[0], l);
    GCDict* options = nn_options("autograd_randn", a, n, 1, l);
    ag_validate_options("autograd_randn", options,
                        {"mean", "std", "seed", "requires_grad", "dtype", "device"}, l);
    double mean = nn_option_number("autograd_randn", options, "mean", 0.0, -1e12, 1e12, l);
    double stddev = nn_option_number("autograd_randn", options, "std", 1.0, 0.0, 1e12, l);
    if (stddev <= 0.0) throw JitThrow{"autograd_randn(): option std must be positive", l};
    size_t seed = nn_option_integer("autograd_randn", options, "seed", 42, 0,
                                    (size_t)9007199254740991ULL, l);
    bool requires_grad = nn_option_bool("autograd_randn", options, "requires_grad", false, l);
    TensorDType dtype = ag_option_dtype("autograd_randn", options, TensorDType::FLOAT64, l);
    bool cuda = ag_device_option_cuda("autograd_randn", options, l);
    if (cuda && !ag_has_option(options, "dtype")) dtype = TensorDType::FLOAT32;
    size_t count = ag_numel("autograd_randn", shape, l);
    ag_preflight_bytes(count * sizeof(double), "autograd_randn", l);
    std::mt19937_64 generator((uint64_t)seed);
    std::normal_distribution<double> distribution(mean, stddev);
    std::vector<double> data(count);
    for (double& value : data) value = distribution(generator);
    Value result = ag_make_tensor("autograd_randn", std::move(data), std::move(shape),
                                  requires_grad, TensorOp::LEAF, {}, l, dtype);
    return ag_place_created_tensor(result, cuda, "autograd_randn", l);
}

inline Value b_autograd_data(const Value* a, int n, int l) {
    need_args("autograd_data", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_data", a[0], 0, l);
    ag_cuda_materialize_host(tensor, "autograd_data", l);
    return ag_data_value(tensor->data, tensor->shape);
}

inline Value b_autograd_grad(const Value* a, int n, int l) {
    need_args("autograd_grad", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_grad", a[0], 0, l);
    if (!std::isfinite(tensor->cuda_grad_scale)
        || tensor->cuda_grad_scale < 0.0f
        || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
        throw JitThrow{"autograd_grad(): corrupted CUDA gradient scale", l};
    }
    if (tensor->cuda_grad) {
        ag_cuda_require_tensor("autograd_grad", tensor, l);
        ag_cuda_require_f32_allocation(
            "autograd_grad", tensor, tensor->cuda_grad,
            "gradient state", l);
        size_t count = tensor->data.size();
        if (count > std::numeric_limits<size_t>::max()
                        / (sizeof(float) + sizeof(double))) {
            throw JitThrow{"autograd_grad(): CUDA gradient size overflows", l};
        }
        AgTemporaryBytes staging(count * (sizeof(float) + sizeof(double)),
                                 "autograd_grad", l);
        std::vector<float> packed;
        if (!SuraCudaDriver::instance().download_f32(
                tensor->cuda_grad->handle, packed, count)) {
            ag_cuda_fail("autograd_grad", l);
        }
        std::vector<double> values(count);
        for (size_t i = 0; i < count; ++i) {
            if (!std::isfinite(packed[i])) {
                throw JitThrow{"autograd_grad(): CUDA gradient is not finite", l};
            }
            values[i] = packed[i];
        }
        return ag_data_value(values, tensor->shape);
    }
    if (tensor->grad.empty()) return Value::nil();
    return ag_data_value(tensor->grad, tensor->shape);
}

inline Value b_autograd_grad_info(const Value* a, int n, int l) {
    need_args("autograd_grad_info", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_grad_info", a[0], 0, l);
    bool present = false;
    size_t elements = 0;
    size_t storage_bytes = 0;
    double scale = 0.0;
    const char* dtype = "float64";
    std::string device = "cpu";

    if (ag_is_cuda(tensor)) {
        ag_cuda_require_tensor("autograd_grad_info", tensor, l);
        if (!tensor->grad.empty()) {
            throw JitThrow{
                "autograd_grad_info(): CUDA tensor has a legacy host gradient",
                l};
        }
        if (!std::isfinite(tensor->cuda_grad_scale)
            || tensor->cuda_grad_scale < 0.0f
            || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
            throw JitThrow{
                "autograd_grad_info(): corrupted CUDA gradient scale", l};
        }
        if (tensor->cuda_grad) {
            ag_cuda_require_f32_allocation(
                "autograd_grad_info", tensor, tensor->cuda_grad,
                "gradient state", l);
            present = true;
            elements = tensor->cuda_grad->elements;
            storage_bytes = elements * sizeof(float);
        }
        scale = (double)tensor->cuda_grad_scale;
        dtype = "float32";
        device = ag_device_text(tensor);
    } else {
        if (tensor->cuda_grad || tensor->cuda_grad_scale != 0.0f) {
            throw JitThrow{
                "autograd_grad_info(): CPU tensor has corrupted CUDA gradient state",
                l};
        }
        if (!tensor->grad.empty()) {
            if (tensor->grad.size() != tensor->data.size()) {
                throw JitThrow{
                    "autograd_grad_info(): corrupted CPU gradient state", l};
            }
            present = true;
            elements = tensor->grad.size();
            storage_bytes = elements * sizeof(double);
            scale = 1.0;
        }
    }

    Value result = Value::make_dict();
    auto* fields = result.as_dict();
    fields->elements["present"] = Value(present);
    fields->elements["dtype"] = Value(std::string(dtype));
    fields->elements["device"] = Value(std::move(device));
    fields->elements["elements"] = Value((double)elements);
    fields->elements["storage_bytes"] = Value((double)storage_bytes);
    fields->elements["scale"] = Value(scale);
    fields->elements["scaled"] = Value(
        present && scale != 0.0 && scale != 1.0);
    fields->elements["leaf"] = Value(tensor->op == TensorOp::LEAF);
    fields->elements["requires_grad"] = Value(tensor->requires_grad);
    fields->elements["optimizer_ready"] = Value(
        tensor->op == TensorOp::LEAF && tensor->requires_grad
        && (!present || scale == 0.0 || scale == 1.0));
    return result;
}

inline Value b_autograd_dtype(const Value* a, int n, int l) {
    need_args("autograd_dtype", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_dtype", a[0], 0, l);
    return Value(std::string(tensor_dtype_name(tensor->data.dtype())));
}

inline Value b_autograd_device(const Value* a, int n, int l) {
    need_args("autograd_device", n, 1, 1, l);
    return Value(ag_device_text(ag_need_tensor("autograd_device", a[0], 0, l)));
}

inline Value b_autograd_to(const Value* a, int n, int l) {
    need_args("autograd_to", n, 2, 2, l);
    GCTensor* source = ag_need_tensor("autograd_to", a[0], 0, l);
    if (!a[1].is_str()) {
        throw JitThrow{"autograd_to(): arg 2 must be a device string", l};
    }
    bool target_cuda = ag_parse_device_request("autograd_to", a[1].as_str_ref(), l);
    if (target_cuda == ag_is_cuda(source)) return a[0];
    ag_check_graph_operand("autograd_to", source, l);
    ag_cuda_materialize_host(source, "autograd_to", l);
    std::vector<double> values = source->data.to_vector();
    Value result = ag_make_tensor(
        "autograd_to", std::move(values), source->shape, source->requires_grad,
        TensorOp::DEVICE_COPY, {source}, l, source->data.dtype());
    return ag_place_created_tensor(result, target_cuda, "autograd_to", l);
}

inline Value b_autograd_storage_bytes(const Value* a, int n, int l) {
    need_args("autograd_storage_bytes", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_storage_bytes", a[0], 0, l);
    return Value((double)tensor->data.byte_size());
}

inline Value b_autograd_cast(const Value* a, int n, int l) {
    need_args("autograd_cast", n, 2, 2, l);
    GCTensor* input = ag_need_tensor("autograd_cast", a[0], 0, l);
    if (!a[1].is_str()) throw JitThrow{"autograd_cast(): arg 2 must be a dtype string", l};
    ag_check_graph_operand("autograd_cast", input, l);
    TensorDType dtype = ag_parse_dtype("autograd_cast", a[1].as_str_ref(), l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_tensor("autograd_cast", input, l);
        if (dtype == input->data.dtype()) return a[0];
        if (dtype == TensorDType::FLOAT64
            || input->data.dtype() == TensorDType::FLOAT64) {
            throw JitThrow{
                "autograd_cast(): resident CUDA cast supports float32, float16, and bfloat16",
                l};
        }
        Value result = ag_make_cuda_tensor(
            "autograd_cast", input->shape, input->requires_grad,
            TensorOp::CAST, {input}, l, dtype);
        GCNativeRoot result_root(result.as_obj());
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        const size_t count = input->data.size();
        const bool input_low = input->data.dtype() == TensorDType::FLOAT16
            || input->data.dtype() == TensorDType::BFLOAT16;
        const bool output_low = dtype == TensorDType::FLOAT16
            || dtype == TensorDType::BFLOAT16;
        if (input_low && !output_low) {
            // The result already owns the exact f32 destination required by
            // the unpack kernel.  Writing into it directly avoids a second
            // f32 allocation and a redundant device-to-device copy.
            if (!driver.unpack_u16_to_f32(
                    input->cuda_data->handle,
                    result.as_tensor()->cuda_data->handle, count)) {
                ag_cuda_fail("autograd_cast", l);
            }
            return result;
        }
        std::shared_ptr<AgCudaAllocation> unpacked;
        SuraCudaDriver::DeviceHandle f32_source = input->cuda_data->handle;
        if (input_low) {
            unpacked = ag_cuda_allocate("autograd_cast", count, l);
            if (!driver.unpack_u16_to_f32(input->cuda_data->handle,
                                          unpacked->handle, count)) {
                ag_cuda_fail("autograd_cast", l);
            }
            f32_source = unpacked->handle;
        }
        if (!output_low) {
            throw JitThrow{"autograd_cast(): internal CUDA cast plan is invalid", l};
        }
        auto status = ag_cuda_allocate("autograd_cast", 1, l);
        if (!driver.fill_f32(status->handle, 0.0f, 1)
            || !driver.pack_f32_to_u16(
                f32_source, result.as_tensor()->cuda_data->handle,
                status->handle, 1u, count)) {
            ag_cuda_fail("autograd_cast", l);
        }
        uint32_t status_bits = 0;
        if (!driver.read_status_u32(status->handle, status_bits)) {
            ag_cuda_fail("autograd_cast", l);
        }
        if (status_bits != 0) {
            throw JitThrow{
                "autograd_cast(): value is non-finite or overflows the target CUDA dtype",
                l};
        }
        return result;
    }
    std::vector<double> data = input->data.to_vector();
    return ag_make_tensor("autograd_cast", std::move(data), input->shape,
                          input->requires_grad, TensorOp::CAST, {input}, l, dtype);
}

inline Value b_autograd_shape(const Value* a, int n, int l) {
    need_args("autograd_shape", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_shape", a[0], 0, l);
    Value out = Value::make_array();
    for (size_t dim : tensor->shape) out.as_arr()->elements.push_back(Value((double)dim));
    return out;
}

inline Value b_autograd_numel(const Value* a, int n, int l) {
    need_args("autograd_numel", n, 1, 1, l);
    return Value((double)ag_need_tensor("autograd_numel", a[0], 0, l)->data.size());
}

inline Value ag_cuda_autocast_report(const AgCudaAutocastState& state) {
    Value result = Value::make_dict();
    result.as_dict()->elements["enabled"] = Value(state.enabled);
    result.as_dict()->elements["dtype"] = Value(std::string(
        tensor_dtype_name(state.dtype)));
    return result;
}

inline Value b_autograd_autocast(const Value* a, int n, int l) {
    need_args("autograd_autocast", n, 0, 1, l);
    if (n == 0) return ag_cuda_autocast_report(ag_cuda_autocast_storage());
    const AgCudaAutocastState previous = ag_cuda_autocast_storage();
    AgCudaAutocastState next = previous;
    if (a[0].is_bool()) {
        next.enabled = a[0].as_bool();
    } else if (a[0].is_str()) {
        next.dtype = ag_parse_dtype(
            "autograd_autocast", a[0].as_str_ref(), l);
        next.enabled = true;
    } else if (a[0].is_dict()) {
        GCDict* options = a[0].as_dict();
        ag_validate_options("autograd_autocast", options,
                            {"enabled", "dtype"}, l);
        next.enabled = nn_option_bool(
            "autograd_autocast", options, "enabled", next.enabled, l);
        if (ag_has_option(options, "dtype")) {
            next.dtype = ag_parse_dtype(
                "autograd_autocast",
                nn_option_string("autograd_autocast", options, "dtype", "", l),
                l);
        }
    } else {
        throw JitThrow{
            "autograd_autocast(): argument must be a bool, dtype string, or options dict",
            l};
    }
    if (next.dtype != TensorDType::FLOAT16
        && next.dtype != TensorDType::BFLOAT16) {
        throw JitThrow{
            "autograd_autocast(): dtype must be float16 or bfloat16", l};
    }
    ag_cuda_autocast_storage() = next;
    return ag_cuda_autocast_report(previous);
}

inline Value b_autograd_limits(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_limits", n, 0, 0, l);
    Value result = Value::make_dict();
    auto* limits = result.as_dict();
    // Preserve the original CPU-default contract while exposing CUDA as
    // additional capability instead of changing the meaning of these keys.
    limits->elements["backend"] = Value(std::string("cpu"));
    limits->elements["dtype"] = Value(std::string("float64"));
    limits->elements["gradient_dtype"] = Value(std::string("float64"));
    limits->elements["cuda_backend"] = Value(std::string("cuda-driver-ptx+cublas-optional"));
    // Operation outputs and gradients remain f32, while tensor payloads may
    // now use genuine two-byte f16/bf16 device storage.
    limits->elements["cuda_dtype"] = Value(std::string("float32"));
    limits->elements["cuda_gradient_dtype"] = Value(std::string("float32"));
    limits->elements["cuda_optimizer_master_dtype"] = Value(std::string("float32"));
    limits->elements["cuda_typed_storage"] = Value(true);
    Value cuda_storage_dtypes = Value::make_array();
    for (const char* dtype : {"float32", "float16", "bfloat16"}) {
        cuda_storage_dtypes.as_arr()->elements.push_back(Value(std::string(dtype)));
    }
    limits->elements["cuda_storage_dtypes"] = cuda_storage_dtypes;
    limits->elements["cuda_resident"] = Value(true);
    Value supported_dtypes = Value::make_array();
    for (const char* dtype : {"float64", "float32", "float16", "bfloat16"}) {
        supported_dtypes.as_arr()->elements.push_back(Value(std::string(dtype)));
    }
    limits->elements["supported_dtypes"] = supported_dtypes;
    limits->elements["max_rank"] = Value((double)AG_MAX_RANK);
    limits->elements["max_elements"] = Value((double)ag_max_elements());
    limits->elements["max_graph_nodes"] = Value((double)AG_MAX_GRAPH_NODES);
    limits->elements["max_attention_scores"] = Value((double)ag_max_attention_scores());
    limits->elements["cuda_attention_workspace_limit_bytes"] = Value(
        (double)ag_cuda_attention_workspace_limit());
    limits->elements["cuda_attention_parallel"] = Value(
        ag_cuda_attention_parallel_enabled());
    limits->elements["cuda_attention_fused"] = Value(
        ag_cuda_attention_fused_enabled());
    limits->elements["cuda_attention_parallel_min_sequence"] = Value(
        (double)AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE);
    limits->elements["cuda_autocast_enabled"] = Value(
        ag_cuda_autocast_storage().enabled);
    limits->elements["cuda_autocast_dtype"] = Value(std::string(
        tensor_dtype_name(ag_cuda_autocast_storage().dtype)));
    limits->elements["memory_limit_bytes"] = Value((double)ag_max_external_bytes());
    limits->elements["memory_used_bytes"] = Value((double)tensor_external_bytes_storage().load(
        std::memory_order_relaxed));
    limits->elements["elements_env"] = Value(std::string("SURA_TENSOR_MAX_ELEMENTS"));
    limits->elements["memory_env"] = Value(std::string("SURA_TENSOR_MEMORY_LIMIT_MB"));
    limits->elements["attention_env"] = Value(std::string("SURA_ATTENTION_MAX_SCORES"));
    limits->elements["cuda_attention_workspace_env"] = Value(std::string(
        "SURA_CUDA_ATTENTION_WORKSPACE_MB"));
    limits->elements["cuda_attention_parallel_env"] = Value(std::string(
        "SURA_CUDA_ATTENTION_PARALLEL"));
    limits->elements["cuda_attention_fused_env"] = Value(std::string(
        "SURA_CUDA_ATTENTION_FUSED"));
    limits->elements["cuda_device_env"] = Value(std::string("SURA_CUDA_DEVICE"));
    limits->elements["cublas_library_env"] = Value(std::string("SURA_CUBLAS_LIBRARY"));
    limits->elements["cublas_disable_env"] = Value(std::string("SURA_CUBLAS_DISABLE"));
    limits->elements["cuda_available"] = Value(SuraCudaDriver::instance().available());
    return result;
}

inline Value b_autograd_cuda_available(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_cuda_available", n, 0, 0, l);
    return Value(SuraCudaDriver::instance().available());
}

inline Value b_autograd_cuda_info(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_cuda_info", n, 0, 0, l);
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    bool available = driver.available();
    const auto stats = driver.stats_snapshot();
    Value result = Value::make_dict();
    result.as_dict()->elements["available"] = Value(available);
    result.as_dict()->elements["backend"] = Value(std::string(
        stats.cublas_available ? "cuda-driver-cublas+ptx" : "cuda-driver-ptx"));
    result.as_dict()->elements["matmul_backend"] = Value(std::string(
        stats.cublas_available ? "cublas-sgemm" : "sura-reference-ptx"));
    result.as_dict()->elements["mixed_matmul_backend"] = Value(std::string(
        stats.cublas_gemm_ex_available
            ? "cublas-gemmex-fast-or-reference-ptx"
            : "sura-reference-ptx"));
    Value mixed_compute_dtypes = Value::make_array();
    mixed_compute_dtypes.as_arr()->elements.push_back(Value(std::string("float16")));
    mixed_compute_dtypes.as_arr()->elements.push_back(Value(std::string("bfloat16")));
    result.as_dict()->elements["mixed_matmul_compute_dtypes"] =
        mixed_compute_dtypes;
    result.as_dict()->elements["typed_storage"] = Value(true);
    Value storage_dtypes = Value::make_array();
    for (const char* dtype : {"float32", "float16", "bfloat16"}) {
        storage_dtypes.as_arr()->elements.push_back(Value(std::string(dtype)));
    }
    result.as_dict()->elements["storage_dtypes"] = storage_dtypes;
    result.as_dict()->elements["typed_matmul_output_dtype"] =
        Value(std::string("float32"));
    result.as_dict()->elements["gradient_dtype"] = Value(std::string("float32"));
    result.as_dict()->elements["optimizer_master_dtype"] =
        Value(std::string("float32"));
    result.as_dict()->elements["cublas_available"] = Value(stats.cublas_available);
    result.as_dict()->elements["cublas_gemm_ex_available"] = Value(
        stats.cublas_gemm_ex_available);
    result.as_dict()->elements["cublas_disabled"] = Value(stats.cublas_disabled);
    result.as_dict()->elements["cublas_library"] = Value(driver.cublas_library_name());
    result.as_dict()->elements["cublas_error"] = Value(driver.cublas_error());
    result.as_dict()->elements["kernel_coverage"] = Value(std::string(
        "resident:typed_f16_bf16_storage,typed_matmul,typed_matmul_backward,f32_master_sgd_adam,f32_elementwise,bias_add,relu,gelu,layer_norm,embedding,transpose,causal_attention,causal_attention_warp_forward,causal_attention_fused_backward,cross_entropy_ids,sum,bias_backward,relu_backward,gelu_backward,layer_norm_backward,embedding_backward,transpose_backward,causal_attention_backward,causal_attention_parallel_backward,cross_entropy_ids_backward"));
    if (available) {
        result.as_dict()->elements["device_count"] = Value((double)driver.device_count());
        result.as_dict()->elements["device_index"] = Value((double)driver.device_index());
        result.as_dict()->elements["device_name"] = Value(driver.device_name());
        result.as_dict()->elements["compute_capability"] = Value(
            std::to_string(driver.compute_major()) + "." + std::to_string(driver.compute_minor()));
        result.as_dict()->elements["total_memory_bytes"] = Value((double)driver.total_memory());
        result.as_dict()->elements["allocated_memory_bytes"] = Value((double)driver.allocated_memory());
    } else {
        result.as_dict()->elements["error"] = Value(driver.error());
    }
    return result;
}

inline Value b_autograd_cuda_stats(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_cuda_stats", n, 0, 0, l);
    const auto stats = SuraCudaDriver::instance().stats_snapshot();
    Value result = Value::make_dict();
    auto* values = result.as_dict();
    values->elements["allocated_bytes"] = Value((double)stats.allocated_bytes);
    values->elements["peak_allocated_bytes"] = Value((double)stats.peak_allocated_bytes);
    values->elements["allocation_calls"] = Value((double)stats.allocation_calls);
    values->elements["free_calls"] = Value((double)stats.free_calls);
    values->elements["h2d_bytes"] = Value((double)stats.h2d_bytes);
    values->elements["d2h_bytes"] = Value((double)stats.d2h_bytes);
    values->elements["control_d2h_bytes"] = Value((double)stats.control_d2h_bytes);
    values->elements["d2d_bytes"] = Value((double)stats.d2d_bytes);
    values->elements["kernel_launches"] = Value((double)stats.kernel_launches);
    values->elements["matmul_launches"] = Value((double)stats.matmul_launches);
    values->elements["cublas_matmul_launches"] = Value(
        (double)stats.cublas_matmul_launches);
    values->elements["reference_matmul_launches"] = Value(
        (double)stats.reference_matmul_launches);
    values->elements["float32_matmul_launches"] = Value(
        (double)stats.float32_matmul_launches);
    values->elements["float16_matmul_launches"] = Value(
        (double)stats.float16_matmul_launches);
    values->elements["bfloat16_matmul_launches"] = Value(
        (double)stats.bfloat16_matmul_launches);
    values->elements["cublas_fast_matmul_launches"] = Value(
        (double)stats.cublas_fast_matmul_launches);
    values->elements["mixed_matmul_fallback_launches"] = Value(
        (double)stats.mixed_matmul_fallback_launches);
    values->elements["typed_storage_matmul_launches"] = Value(
        (double)stats.typed_storage_matmul_launches);
    values->elements["storage_conversion_launches"] = Value(
        (double)stats.storage_conversion_launches);
    values->elements["elementwise_launches"] = Value((double)stats.elementwise_launches);
    values->elements["relu_launches"] = Value((double)stats.relu_launches);
    values->elements["gelu_launches"] = Value((double)stats.gelu_launches);
    values->elements["layer_norm_launches"] = Value((double)stats.layer_norm_launches);
    values->elements["embedding_launches"] = Value((double)stats.embedding_launches);
    values->elements["cross_entropy_launches"] = Value((double)stats.cross_entropy_launches);
    values->elements["attention_launches"] = Value((double)stats.attention_launches);
    values->elements["reference_attention_launches"] = Value(
        (double)stats.reference_attention_launches);
    values->elements["parallel_attention_launches"] = Value(
        (double)stats.parallel_attention_launches);
    values->elements["warp_attention_launches"] = Value(
        (double)stats.warp_attention_launches);
    values->elements["fused_attention_launches"] = Value(
        (double)stats.fused_attention_launches);
    values->elements["fast_attention_forward_launches"] = Value(
        (double)stats.fast_attention_forward_launches);
    values->elements["transpose_launches"] = Value((double)stats.transpose_launches);
    values->elements["reduction_launches"] = Value((double)stats.reduction_launches);
    values->elements["optimizer_launches"] = Value((double)stats.optimizer_launches);
    values->elements["cublas_available"] = Value(stats.cublas_available);
    values->elements["cublas_gemm_ex_available"] = Value(
        stats.cublas_gemm_ex_available);
    values->elements["cublas_disabled"] = Value(stats.cublas_disabled);
    values->elements["counter_overflow"] = Value(stats.counter_overflow);
    return result;
}

inline Value b_autograd_cuda_reset_stats(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_cuda_reset_stats", n, 0, 0, l);
    SuraCudaDriver::instance().reset_stats();
    return Value::nil();
}

inline Value b_autograd_cuda_synchronize(const Value* a, int n, int l) {
    (void)a;
    need_args("autograd_cuda_synchronize", n, 0, 0, l);
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    if (!driver.synchronize()) {
        throw JitThrow{"autograd_cuda_synchronize(): " + driver.error(), l};
    }
    return Value::nil();
}

inline Value b_autograd_requires_grad(const Value* a, int n, int l) {
    need_args("autograd_requires_grad", n, 1, 1, l);
    return Value(ag_need_tensor("autograd_requires_grad", a[0], 0, l)->requires_grad);
}

inline Value b_autograd_item(const Value* a, int n, int l) {
    need_args("autograd_item", n, 1, 1, l);
    GCTensor* tensor = ag_need_tensor("autograd_item", a[0], 0, l);
    if (tensor->data.size() != 1) throw JitThrow{"autograd_item(): tensor must contain exactly one value", l};
    ag_cuda_materialize_host(tensor, "autograd_item", l);
    return Value(tensor->data[0]);
}

inline Value b_autograd_detach(const Value* a, int n, int l) {
    need_args("autograd_detach", n, 1, 1, l);
    GCTensor* source = ag_need_tensor("autograd_detach", a[0], 0, l);
    if (ag_is_cuda(source)) {
        ag_cuda_require_tensor("autograd_detach", source, l);
        Value result = ag_make_cuda_tensor("autograd_detach", source->shape,
                                            false, TensorOp::LEAF, {}, l,
                                            source->data.dtype());
        GCNativeRoot result_root(result.as_obj());
        ag_cuda_copy_all("autograd_detach", *source->cuda_data,
                         *result.as_tensor()->cuda_data, l);
        return result;
    }
    ag_preflight_bytes(source->data.size() * sizeof(double), "autograd_detach", l);
    std::vector<double> data = source->data.to_vector();
    return ag_make_tensor("autograd_detach", std::move(data), source->shape,
                          false, TensorOp::LEAF, {}, l, source->data.dtype());
}

inline Value b_autograd_set_requires_grad(const Value* a, int n, int l) {
    need_args("autograd_set_requires_grad", n, 2, 2, l);
    GCTensor* tensor = ag_need_tensor("autograd_set_requires_grad", a[0], 0, l);
    if (!a[1].is_bool()) throw JitThrow{"autograd_set_requires_grad(): arg 2 must be a bool", l};
    if (tensor->op != TensorOp::LEAF) {
        throw JitThrow{"autograd_set_requires_grad(): only leaf tensors can change requires_grad", l};
    }
    bool next = a[1].as_bool();
    if (tensor->requires_grad != next) ++tensor->version;
    tensor->requires_grad = next;
    if (!tensor->requires_grad) {
        ag_clear_grad_buffer(tensor);
        tensor->cuda_grad.reset();
        tensor->cuda_grad_scale = 0.0f;
    }
    return a[0];
}

inline Value ag_binary_op(const char* name, const Value* a, int n, int l, TensorOp op) {
    need_args(name, n, 2, 2, l);
    TensorDType left_hint = a[1].is_tensor()
        ? a[1].as_tensor()->data.dtype() : TensorDType::FLOAT64;
    Value left_value = ag_coerce_tensor(name, a[0], l, left_hint);
    GCNativeRoot left_root(left_value.as_obj());
    Value right_value = ag_coerce_tensor(name, a[1], l,
                                         left_value.as_tensor()->data.dtype());
    GCNativeRoot right_root(right_value.as_obj());
    GCTensor* left = left_value.as_tensor();
    GCTensor* right = right_value.as_tensor();
    ag_check_graph_operand(name, left, l);
    ag_check_graph_operand(name, right, l);
    std::vector<size_t> shape = ag_broadcast_shape(name, left->shape, right->shape, l);
    size_t count = ag_numel(name, shape, l);

    const bool left_cuda = ag_is_cuda(left);
    const bool right_cuda = ag_is_cuda(right);
    if (left_cuda || right_cuda) {
        const bool raw_left_scalar = a[0].is_num() && !a[0].is_tensor();
        const bool raw_right_scalar = a[1].is_num() && !a[1].is_tensor();
        GCTensor* device_tensor = left_cuda ? left : right;
        const bool scalar_case = left_cuda != right_cuda
            && ((left_cuda && raw_right_scalar) || (right_cuda && raw_left_scalar));

        if (scalar_case) {
            GCTensor* scalar_tensor = left_cuda ? right : left;
            if (!scalar_tensor->shape.empty() || scalar_tensor->data.size() != 1) {
                throw JitThrow{std::string(name) + "(): CUDA scalar operand is malformed", l};
            }
            ag_cuda_require_f32_tensor(name, device_tensor, l);
            const double scalar_value = scalar_tensor->data[0];
            if (!std::isfinite(scalar_value)) {
                throw JitThrow{std::string(name) + "(): scalar must be finite", l};
            }
            if (op == TensorOp::DIV && !raw_left_scalar && scalar_value == 0.0) {
                throw JitThrow{std::string(name) + "(): division by zero", l};
            }
            if (op == TensorOp::DIV && raw_left_scalar) {
                throw JitThrow{std::string(name)
                               + "(): CUDA scalar/tensor division awaits a finite-check kernel", l};
            }
            bool requires_grad = device_tensor->requires_grad;
            Value result = ag_make_cuda_tensor(name, device_tensor->shape, requires_grad,
                                                op, {device_tensor}, l);
            GCNativeRoot result_root(result.as_obj());
            GCTensor* output = result.as_tensor();
            output->op_scalar = scalar_value;
            if (requires_grad) {
                ag_set_op_indices(output, {raw_left_scalar ? 1U : 0U}, name, l);
            }
            SuraCudaDriver& driver = SuraCudaDriver::instance();
            bool launched = false;
            const float scalar = (float)scalar_value;
            if (op == TensorOp::ADD) {
                launched = driver.add_scalar_f32(device_tensor->cuda_data->handle,
                                                 output->cuda_data->handle,
                                                 scalar, count);
            } else if (op == TensorOp::SUB) {
                launched = driver.affine_f32(device_tensor->cuda_data->handle,
                                             output->cuda_data->handle,
                                             raw_left_scalar ? -1.0f : 1.0f,
                                             raw_left_scalar ? scalar : -scalar,
                                             count);
            } else if (op == TensorOp::MUL) {
                launched = driver.scale_f32(device_tensor->cuda_data->handle,
                                            output->cuda_data->handle,
                                            scalar, count);
            } else if (!raw_left_scalar) {
                launched = driver.scale_f32(device_tensor->cuda_data->handle,
                                            output->cuda_data->handle,
                                            1.0f / scalar, count);
            } else {
                auto expanded = ag_cuda_allocate(name, count, l);
                launched = driver.fill_f32(expanded->handle, scalar, count)
                    && driver.divide_f32(expanded->handle,
                                         device_tensor->cuda_data->handle,
                                         output->cuda_data->handle, count);
            }
            if (!launched) ag_cuda_fail(name, l);
            return result;
        }

        ag_cuda_require_same_device(name, left, right, l);
        ag_cuda_require_float32(name, left, l);
        ag_cuda_require_float32(name, right, l);
        if (left->shape != right->shape) {
            const bool column_bias = op == TensorOp::ADD
                && left->shape == shape && left->shape.size() >= 2
                && right->shape.size() == 1
                && right->shape[0] == shape.back()
                && right->data.size() == shape.back()
                && shape.back() != 0
                && count / shape.back() <= (size_t)std::numeric_limits<uint32_t>::max()
                && shape.back() <= (size_t)std::numeric_limits<uint32_t>::max();
            if (column_bias) {
                const uint32_t rows = (uint32_t)(count / shape.back());
                const uint32_t cols = (uint32_t)shape.back();
                Value result = ag_make_cuda_tensor(name, std::move(shape),
                                                    left->requires_grad || right->requires_grad,
                                                    op, {left, right}, l);
                GCNativeRoot result_root(result.as_obj());
                if (!SuraCudaDriver::instance().bias_add_f32(
                        left->cuda_data->handle, right->cuda_data->handle,
                        result.as_tensor()->cuda_data->handle, rows, cols)) {
                    ag_cuda_fail(name, l);
                }
                return result;
            }
            throw JitThrow{std::string(name)
                           + "(): resident CUDA elementwise operations currently require "
                             "identical shapes, a [features] add bias, or a numeric scalar", l};
        }
        if (op == TensorOp::DIV) {
            throw JitThrow{std::string(name)
                           + "(): resident CUDA tensor division awaits a finite-check kernel; "
                             "division by a non-zero numeric scalar is supported", l};
        }
        Value result = ag_make_cuda_tensor(name, std::move(shape),
                                            left->requires_grad || right->requires_grad,
                                            op, {left, right}, l);
        GCNativeRoot result_root(result.as_obj());
        GCTensor* output = result.as_tensor();
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        bool launched = op == TensorOp::ADD
            ? driver.add_f32(left->cuda_data->handle, right->cuda_data->handle,
                             output->cuda_data->handle, count)
            : op == TensorOp::SUB
                ? driver.subtract_f32(left->cuda_data->handle, right->cuda_data->handle,
                                      output->cuda_data->handle, count)
                : op == TensorOp::MUL
                    ? driver.multiply_f32(left->cuda_data->handle, right->cuda_data->handle,
                                          output->cuda_data->handle, count)
                    : driver.divide_f32(left->cuda_data->handle, right->cuda_data->handle,
                                        output->cuda_data->handle, count);
        if (!launched) ag_cuda_fail(name, l);
        return result;
    }

    ag_preflight_bytes(count * sizeof(double), name, l);
    std::vector<double> data(count, 0.0);
    for (size_t i = 0; i < count; ++i) {
        double x = left->data[ag_broadcast_index(i, shape, left->shape)];
        double y = right->data[ag_broadcast_index(i, shape, right->shape)];
        if (op == TensorOp::ADD) data[i] = x + y;
        else if (op == TensorOp::SUB) data[i] = x - y;
        else if (op == TensorOp::MUL) data[i] = x * y;
        else {
            if (y == 0.0) throw JitThrow{std::string(name) + "(): division by zero", l};
            data[i] = x / y;
        }
        if (!std::isfinite(data[i])) throw JitThrow{std::string(name) + "(): result is not finite", l};
    }
    bool requires_grad = left->requires_grad || right->requires_grad;
    return ag_make_tensor(name, std::move(data), std::move(shape), requires_grad,
                          op, {left, right}, l,
                          ag_promote_dtype(left->data.dtype(), right->data.dtype()));
}

inline Value b_autograd_add(const Value* a, int n, int l) {
    return ag_binary_op("autograd_add", a, n, l, TensorOp::ADD);
}
inline Value b_autograd_sub(const Value* a, int n, int l) {
    return ag_binary_op("autograd_sub", a, n, l, TensorOp::SUB);
}
inline Value b_autograd_mul(const Value* a, int n, int l) {
    return ag_binary_op("autograd_mul", a, n, l, TensorOp::MUL);
}
inline Value b_autograd_div(const Value* a, int n, int l) {
    return ag_binary_op("autograd_div", a, n, l, TensorOp::DIV);
}

inline Value b_autograd_neg(const Value* a, int n, int l) {
    need_args("autograd_neg", n, 1, 1, l);
    GCTensor* input = ag_need_tensor("autograd_neg", a[0], 0, l);
    ag_check_graph_operand("autograd_neg", input, l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_f32_tensor("autograd_neg", input, l);
        Value result = ag_make_cuda_tensor("autograd_neg", input->shape,
                                            input->requires_grad, TensorOp::NEG,
                                            {input}, l);
        GCNativeRoot result_root(result.as_obj());
        if (!SuraCudaDriver::instance().negate_f32(
                input->cuda_data->handle, result.as_tensor()->cuda_data->handle,
                input->data.size())) {
            ag_cuda_fail("autograd_neg", l);
        }
        return result;
    }
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_neg", l);
    std::vector<double> data(input->data.size());
    for (size_t i = 0; i < data.size(); ++i) data[i] = -input->data[i];
    return ag_make_tensor("autograd_neg", std::move(data), input->shape,
                          input->requires_grad, TensorOp::NEG, {input}, l,
                          input->data.dtype());
}

inline Value b_autograd_reshape(const Value* a, int n, int l) {
    need_args("autograd_reshape", n, 2, 2, l);
    GCTensor* input = ag_need_tensor("autograd_reshape", a[0], 0, l);
    ag_check_graph_operand("autograd_reshape", input, l);
    std::vector<size_t> shape = ag_parse_reshape_shape(
        "autograd_reshape", a[1], input->data.size(), l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_tensor("autograd_reshape", input, l);
        Value result = ag_make_cuda_tensor("autograd_reshape", std::move(shape),
                                            input->requires_grad, TensorOp::RESHAPE,
                                            {input}, l, input->data.dtype());
        GCNativeRoot result_root(result.as_obj());
        ag_cuda_copy_all("autograd_reshape", *input->cuda_data,
                         *result.as_tensor()->cuda_data, l);
        return result;
    }
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_reshape", l);
    std::vector<double> data = input->data;
    return ag_make_tensor("autograd_reshape", std::move(data), std::move(shape),
                          input->requires_grad, TensorOp::RESHAPE, {input}, l,
                          input->data.dtype());
}

inline Value b_autograd_matmul(const Value* a, int n, int l) {
    need_args("autograd_matmul", n, 2, 3, l);
    GCTensor* left = ag_need_tensor("autograd_matmul", a[0], 0, l);
    GCTensor* right = ag_need_tensor("autograd_matmul", a[1], 1, l);
    ag_check_graph_operand("autograd_matmul", left, l);
    ag_check_graph_operand("autograd_matmul", right, l);
    if (left->shape.size() < 2 || right->shape.size() < 2) {
        throw JitThrow{"autograd_matmul(): both tensors must have rank 2 or greater", l};
    }
    size_t rows = left->shape[left->shape.size() - 2];
    size_t inner = left->shape.back();
    if (inner != right->shape[right->shape.size() - 2]) {
        throw JitThrow{"autograd_matmul(): inner dimensions do not match", l};
    }
    size_t cols = right->shape.back();
    std::vector<size_t> left_batch = ag_prefix_shape(left->shape, 2);
    std::vector<size_t> right_batch = ag_prefix_shape(right->shape, 2);
    std::vector<size_t> batch_shape = ag_broadcast_shape(
        "autograd_matmul", left_batch, right_batch, l);
    if (batch_shape.size() + 2 > AG_MAX_RANK) {
        throw JitThrow{"autograd_matmul(): result rank exceeds the tensor rank limit", l};
    }
    std::vector<size_t> shape = batch_shape;
    shape.push_back(rows);
    shape.push_back(cols);
    size_t output_count = ag_numel("autograd_matmul", shape, l);
    GCDict* options = nn_options("autograd_matmul", a, n, 2, l);
    ag_validate_options("autograd_matmul", options,
                        {"backend", "compute_dtype"}, l);
    const bool resident_cuda = ag_is_cuda(left) || ag_is_cuda(right);
    if (!resident_cuda) {
        ag_preflight_bytes(output_count * sizeof(double), "autograd_matmul", l);
    }
    std::string backend = nn_option_string("autograd_matmul", options, "backend",
                                           resident_cuda ? "cuda" : "cpu", l);
    if (backend != "cpu" && backend != "cuda") {
        throw JitThrow{"autograd_matmul(): option backend must be cpu or cuda", l};
    }
    const bool left_low_storage = left->data.dtype() == TensorDType::FLOAT16
        || left->data.dtype() == TensorDType::BFLOAT16;
    const bool right_low_storage = right->data.dtype() == TensorDType::FLOAT16
        || right->data.dtype() == TensorDType::BFLOAT16;
    if (resident_cuda && left_low_storage && right_low_storage
        && left->data.dtype() != right->data.dtype()) {
        throw JitThrow{
            "autograd_matmul(): float16 and bfloat16 CUDA storage cannot be mixed",
            l};
    }
    TensorDType default_compute = TensorDType::FLOAT32;
    if (resident_cuda && left_low_storage && right_low_storage
        && left->data.dtype() == right->data.dtype()) {
        default_compute = left->data.dtype();
    } else if (resident_cuda && !left_low_storage && !right_low_storage
               && ag_cuda_autocast_storage().enabled) {
        default_compute = ag_cuda_autocast_storage().dtype;
    }
    const std::string default_compute_dtype = tensor_dtype_name(default_compute);
    TensorDType compute_dtype = ag_parse_dtype(
        "autograd_matmul",
        nn_option_string("autograd_matmul", options, "compute_dtype",
                         default_compute_dtype, l),
        l);
    if (!resident_cuda && ag_has_option(options, "compute_dtype")) {
        throw JitThrow{
            "autograd_matmul(): compute_dtype is available only for resident CUDA tensors",
            l};
    }
    const auto cuda_compute = resident_cuda
        ? ag_cuda_matmul_compute(compute_dtype, "autograd_matmul", l)
        : SuraCudaDriver::MatmulCompute::FLOAT32;
    if (resident_cuda) {
        if (backend != "cuda") {
            throw JitThrow{"autograd_matmul(): a resident CUDA tensor cannot use the CPU backend; use to(cpu) first", l};
        }
        ag_cuda_require_same_device("autograd_matmul", left, right, l);
        if (right->shape.size() != 2) {
            throw JitThrow{
                "autograd_matmul(): resident CUDA supports a rank-2 shared right matrix; "
                "batched right operands are not implemented yet", l};
        }
        // A contiguous [..., rows, inner] left operand is the same physical
        // matrix as [prefix*rows, inner]. This opens Transformer projections
        // without allocating a reshape or implementing broadcasted BMM.
        const size_t flat_rows = left->data.size() / inner;
        if (flat_rows > (size_t)std::numeric_limits<uint32_t>::max()
            || cols > (size_t)std::numeric_limits<uint32_t>::max()
            || inner > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_matmul(): CUDA matrix dimensions are too large", l};
        }
        Value result = ag_make_cuda_tensor(
            "autograd_matmul", std::move(shape),
            left->requires_grad || right->requires_grad,
            TensorOp::MATMUL, {left, right}, l);
        GCNativeRoot result_root(result.as_obj());
        result.as_tensor()->cuda_matmul_compute_dtype = compute_dtype;
        if (!SuraCudaDriver::instance().matmul_device_typed(
                left->cuda_data->handle, right->cuda_data->handle,
                result.as_tensor()->cuda_data->handle,
                (uint32_t)flat_rows, (uint32_t)cols, (uint32_t)inner,
                false, false, cuda_compute)) {
            ag_cuda_fail("autograd_matmul", l);
        }
        return result;
    }
    if (backend == "cuda") {
        if (left->shape.size() != 2 || right->shape.size() != 2) {
            throw JitThrow{"autograd_matmul(): CUDA v1 supports rank-2 tensors only", l};
        }
        if (left->data.dtype() != TensorDType::FLOAT32
            || right->data.dtype() != TensorDType::FLOAT32) {
            throw JitThrow{"autograd_matmul(): CUDA v1 requires float32 tensors", l};
        }
        if (rows > (size_t)std::numeric_limits<uint32_t>::max()
            || cols > (size_t)std::numeric_limits<uint32_t>::max()
            || inner > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_matmul(): CUDA matrix dimensions are too large", l};
        }
        std::vector<float> left_values(left->data.size());
        std::vector<float> right_values(right->data.size());
        for (size_t i = 0; i < left_values.size(); ++i) left_values[i] = (float)left->data[i];
        for (size_t i = 0; i < right_values.size(); ++i) right_values[i] = (float)right->data[i];
        std::vector<float> output_values;
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        if (!driver.matmul_f32(left_values, right_values, output_values,
                               (uint32_t)rows, (uint32_t)cols, (uint32_t)inner)) {
            throw JitThrow{"autograd_matmul(): CUDA backend failed: " + driver.error(), l};
        }
        std::vector<double> cuda_data(output_values.size());
        for (size_t i = 0; i < output_values.size(); ++i) cuda_data[i] = output_values[i];
        return ag_make_tensor("autograd_matmul", std::move(cuda_data), std::move(shape),
                              left->requires_grad || right->requires_grad,
                              TensorOp::MATMUL, {left, right}, l, TensorDType::FLOAT32);
    }
    if (left->data.size() > std::numeric_limits<size_t>::max() - right->data.size()
        || left->data.size() + right->data.size()
               > std::numeric_limits<size_t>::max() - output_count) {
        throw JitThrow{"autograd_matmul(): CPU working buffer size overflows", l};
    }
    size_t staged_elements = left->data.size() + right->data.size() + output_count;
    if (staged_elements > std::numeric_limits<size_t>::max() / sizeof(double)) {
        throw JitThrow{"autograd_matmul(): CPU working buffer size overflows", l};
    }
    AgTemporaryBytes working_memory(staged_elements * sizeof(double),
                                    "autograd_matmul", l);
    // Decode packed dtype storage once. Reading TensorBuffer inside the cubic
    // loop repeats dtype dispatch and memcpy work for every multiply.
    std::vector<double> left_values = left->data.to_vector();
    std::vector<double> right_values = right->data.to_vector();
    std::vector<double> data(output_count, 0.0);
    size_t batches = ag_product(batch_shape);
    size_t left_matrix = rows * inner;
    size_t right_matrix = inner * cols;
    size_t output_matrix = rows * cols;
    for (size_t batch = 0; batch < batches; ++batch) {
        size_t left_base = ag_broadcast_index(batch, batch_shape, left_batch) * left_matrix;
        size_t right_base = ag_broadcast_index(batch, batch_shape, right_batch) * right_matrix;
        size_t output_base = batch * output_matrix;
        for (size_t row = 0; row < rows; ++row) {
            double* output_row = data.data() + output_base + row * cols;
            const double* left_row = left_values.data() + left_base + row * inner;
            for (size_t k = 0; k < inner; ++k) {
                double left_value = left_row[k];
                const double* right_row = right_values.data() + right_base + k * cols;
                for (size_t col = 0; col < cols; ++col) {
                    output_row[col] += left_value * right_row[col];
                }
            }
            for (size_t col = 0; col < cols; ++col) {
                if (!std::isfinite(output_row[col])) {
                    throw JitThrow{"autograd_matmul(): result is not finite", l};
                }
            }
        }
    }
    return ag_make_tensor("autograd_matmul", std::move(data), std::move(shape),
                          left->requires_grad || right->requires_grad,
                          TensorOp::MATMUL, {left, right}, l,
                          ag_promote_dtype(left->data.dtype(), right->data.dtype()));
}

inline size_t ag_axis(const char* name, const Value& value, size_t rank, int index, int line) {
    double raw = need_num(name, value, index, line);
    if (!std::isfinite(raw) || raw != std::floor(raw)) {
        throw JitThrow{std::string(name) + "(): axes must be integers", line};
    }
    if (raw < -(double)rank || raw >= (double)rank) {
        throw JitThrow{std::string(name) + "(): axis is out of range", line};
    }
    long long axis = (long long)raw;
    if (axis < 0) axis += (long long)rank;
    return (size_t)axis;
}

inline size_t ag_transposed_input_index(size_t output_index,
                                        const std::vector<size_t>& output_shape,
                                        const std::vector<size_t>& input_shape,
                                        size_t first, size_t second) {
    std::array<size_t, AG_MAX_RANK> coordinates{};
    size_t remaining = output_index;
    for (size_t reverse = 0; reverse < output_shape.size(); ++reverse) {
        size_t axis = output_shape.size() - 1 - reverse;
        coordinates[axis] = remaining % output_shape[axis];
        remaining /= output_shape[axis];
    }
    std::swap(coordinates[first], coordinates[second]);
    size_t input_index = 0;
    for (size_t axis = 0; axis < input_shape.size(); ++axis) {
        input_index = input_index * input_shape[axis] + coordinates[axis];
    }
    return input_index;
}

inline Value b_autograd_transpose(const Value* a, int n, int l) {
    need_args("autograd_transpose", n, 1, 3, l);
    GCTensor* input = ag_need_tensor("autograd_transpose", a[0], 0, l);
    ag_check_graph_operand("autograd_transpose", input, l);
    if (input->shape.size() < 2) {
        throw JitThrow{"autograd_transpose(): tensor must have rank 2 or greater", l};
    }
    if (n == 2) throw JitThrow{"autograd_transpose(): provide both axes or neither", l};
    size_t first = input->shape.size() - 2;
    size_t second = input->shape.size() - 1;
    if (n == 3) {
        first = ag_axis("autograd_transpose", a[1], input->shape.size(), 1, l);
        second = ag_axis("autograd_transpose", a[2], input->shape.size(), 2, l);
    }
    if (first == second) throw JitThrow{"autograd_transpose(): axes must be different", l};
    std::vector<size_t> shape = input->shape;
    std::swap(shape[first], shape[second]);
    if (ag_is_cuda(input)) {
        ag_cuda_require_f32_tensor("autograd_transpose", input, l);
        Value result = ag_make_cuda_tensor(
            "autograd_transpose", std::move(shape), input->requires_grad,
            TensorOp::TRANSPOSE, {input}, l);
        GCNativeRoot result_root(result.as_obj());
        if (result.as_tensor()->requires_grad) {
            ag_set_op_indices(result.as_tensor(), {first, second},
                              "autograd_transpose", l);
        }
        if (!SuraCudaDriver::instance().transpose_f32(
                input->cuda_data->handle,
                result.as_tensor()->cuda_data->handle,
                input->data.size(), input->shape,
                (uint32_t)first, (uint32_t)second)) {
            ag_cuda_fail("autograd_transpose", l);
        }
        return result;
    }
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_transpose", l);
    std::vector<double> data(input->data.size(), 0.0);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = input->data[ag_transposed_input_index(i, shape, input->shape, first, second)];
    }
    Value result = ag_make_tensor("autograd_transpose", std::move(data), std::move(shape),
                                  input->requires_grad, TensorOp::TRANSPOSE, {input}, l,
                                  input->data.dtype());
    if (result.as_tensor()->requires_grad) {
        ag_set_op_indices(result.as_tensor(), {first, second}, "autograd_transpose", l);
    }
    return result;
}

inline Value ag_unary_activation(const char* name, const Value* a, int n, int l, TensorOp op) {
    need_args(name, n, 1, 1, l);
    GCTensor* input = ag_need_tensor(name, a[0], 0, l);
    ag_check_graph_operand(name, input, l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_f32_tensor(name, input, l);
        if (op != TensorOp::RELU) {
            throw JitThrow{std::string(name)
                           + "(): resident CUDA currently supports relu only", l};
        }
        Value result = ag_make_cuda_tensor(name, input->shape,
                                            input->requires_grad, op, {input}, l);
        GCNativeRoot result_root(result.as_obj());
        if (!SuraCudaDriver::instance().relu_f32(
                input->cuda_data->handle, result.as_tensor()->cuda_data->handle,
                input->data.size())) {
            ag_cuda_fail(name, l);
        }
        return result;
    }
    ag_preflight_bytes(input->data.size() * sizeof(double), name, l);
    std::vector<double> data(input->data.size(), 0.0);
    for (size_t i = 0; i < data.size(); ++i) {
        double x = input->data[i];
        if (op == TensorOp::RELU) data[i] = std::max(0.0, x);
        else if (op == TensorOp::TANH) data[i] = std::tanh(x);
        else if (x >= 0.0) data[i] = 1.0 / (1.0 + std::exp(-x));
        else {
            double e = std::exp(x);
            data[i] = e / (1.0 + e);
        }
    }
    return ag_make_tensor(name, std::move(data), input->shape,
                          input->requires_grad, op, {input}, l,
                          input->data.dtype());
}

inline Value b_autograd_relu(const Value* a, int n, int l) {
    return ag_unary_activation("autograd_relu", a, n, l, TensorOp::RELU);
}
inline Value b_autograd_tanh(const Value* a, int n, int l) {
    return ag_unary_activation("autograd_tanh", a, n, l, TensorOp::TANH);
}
inline Value b_autograd_sigmoid(const Value* a, int n, int l) {
    return ag_unary_activation("autograd_sigmoid", a, n, l, TensorOp::SIGMOID);
}

inline Value b_autograd_gelu(const Value* a, int n, int l) {
    need_args("autograd_gelu", n, 1, 1, l);
    GCTensor* input = ag_need_tensor("autograd_gelu", a[0], 0, l);
    ag_check_graph_operand("autograd_gelu", input, l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_f32_tensor("autograd_gelu", input, l);
        Value result = ag_make_cuda_tensor(
            "autograd_gelu", input->shape, input->requires_grad,
            TensorOp::GELU, {input}, l);
        GCNativeRoot result_root(result.as_obj());
        if (!SuraCudaDriver::instance().gelu_f32(
                input->cuda_data->handle, result.as_tensor()->cuda_data->handle,
                input->data.size())) {
            ag_cuda_fail("autograd_gelu", l);
        }
        return result;
    }
    constexpr double inv_sqrt_two = 0.707106781186547524400844362104849039;
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_gelu", l);
    std::vector<double> data(input->data.size(), 0.0);
    for (size_t i = 0; i < data.size(); ++i) {
        double x = input->data[i];
        data[i] = 0.5 * x * (1.0 + std::erf(x * inv_sqrt_two));
        if (!std::isfinite(data[i])) throw JitThrow{"autograd_gelu(): result is not finite", l};
    }
    return ag_make_tensor("autograd_gelu", std::move(data), input->shape,
                          input->requires_grad, TensorOp::GELU, {input}, l,
                          input->data.dtype());
}

struct AgLayerNormStats {
    long double anchor_raw = 0.0L;
    long double mean_delta = 0.0L;
    long double denominator = 1.0L;
    long double inverse_std = 1.0L;
    long double scale = 1.0L;
    bool scaled = false;
};

inline AgLayerNormStats ag_layer_norm_stats(const GCTensor* input, size_t base,
                                             size_t features, double epsilon) {
    AgLayerNormStats stats;
    double maximum = 0.0;
    for (size_t col = 0; col < features; ++col) {
        maximum = std::max(maximum, std::abs(input->data[base + col]));
    }
    long double sqrt_epsilon = std::sqrt((long double)epsilon);
    stats.scaled = maximum >= sqrt_epsilon && maximum > 0.0;
    stats.scale = stats.scaled ? (long double)maximum : 1.0L;
    stats.anchor_raw = (long double)input->data[base];
    long double total = 0.0L;
    long double compensation = 0.0L;
    for (size_t col = 0; col < features; ++col) {
        long double delta = (long double)input->data[base + col]
                          - stats.anchor_raw;
        if (stats.scaled) delta /= stats.scale;
        long double corrected = delta - compensation;
        long double next = total + corrected;
        compensation = (next - total) - corrected;
        total = next;
    }
    stats.mean_delta = total / (long double)features;
    long double norm = 0.0L;
    for (size_t col = 0; col < features; ++col) {
        long double delta = (long double)input->data[base + col]
                          - stats.anchor_raw;
        if (stats.scaled) delta /= stats.scale;
        norm = std::hypot(norm, delta - stats.mean_delta);
    }
    long double rms = norm / std::sqrt((long double)features);
    if (stats.scaled) {
        stats.denominator = std::hypot(rms, sqrt_epsilon / stats.scale);
        stats.inverse_std = (1.0L / stats.scale) / stats.denominator;
    } else {
        stats.denominator = std::hypot(rms, sqrt_epsilon);
        stats.inverse_std = 1.0L / stats.denominator;
    }
    return stats;
}

inline double ag_layer_norm_normalized(const GCTensor* input, size_t index,
                                       const AgLayerNormStats& stats) {
    long double delta = (long double)input->data[index] - stats.anchor_raw;
    if (stats.scaled) delta /= stats.scale;
    return (double)((delta - stats.mean_delta) / stats.denominator);
}

inline Value b_autograd_layer_norm(const Value* a, int n, int l) {
    need_args("autograd_layer_norm", n, 1, 4, l);
    GCTensor* input = ag_need_tensor("autograd_layer_norm", a[0], 0, l);
    ag_check_graph_operand("autograd_layer_norm", input, l);
    if (input->shape.empty()) {
        throw JitThrow{"autograd_layer_norm(): input must have at least one dimension", l};
    }
    size_t features = input->shape.back();
    GCTensor* weight = nullptr;
    GCTensor* bias = nullptr;
    if (n >= 2 && !a[1].is_nil()) {
        weight = ag_need_tensor("autograd_layer_norm", a[1], 1, l);
        ag_check_graph_operand("autograd_layer_norm", weight, l);
        if (weight->shape != std::vector<size_t>{features}) {
            throw JitThrow{"autograd_layer_norm(): weight shape must match the last input dimension", l};
        }
    }
    if (n >= 3 && !a[2].is_nil()) {
        bias = ag_need_tensor("autograd_layer_norm", a[2], 2, l);
        ag_check_graph_operand("autograd_layer_norm", bias, l);
        if (bias->shape != std::vector<size_t>{features}) {
            throw JitThrow{"autograd_layer_norm(): bias shape must match the last input dimension", l};
        }
    }
    double epsilon = 0.00001;
    if (n >= 4) {
        epsilon = need_num("autograd_layer_norm", a[3], 3, l);
        if (!std::isfinite(epsilon) || epsilon < 1e-12 || epsilon > 0.1) {
            throw JitThrow{"autograd_layer_norm(): epsilon must be between 1e-12 and 0.1", l};
        }
    }
    size_t outer = input->data.size() / features;
    const bool resident_cuda = ag_is_cuda(input)
        || (weight && ag_is_cuda(weight)) || (bias && ag_is_cuda(bias));
    if (resident_cuda) {
        ag_cuda_require_f32_tensor("autograd_layer_norm", input, l);
        if (weight) {
            ag_cuda_require_same_device("autograd_layer_norm", input, weight, l);
            ag_cuda_require_float32("autograd_layer_norm", weight, l);
        }
        if (bias) {
            ag_cuda_require_same_device("autograd_layer_norm", input, bias, l);
            ag_cuda_require_float32("autograd_layer_norm", bias, l);
        }
        if (outer > (size_t)std::numeric_limits<uint32_t>::max()
            || features > (size_t)std::numeric_limits<uint32_t>::max()
            || input->data.size() > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_layer_norm(): CUDA dimensions are too large", l};
        }
        std::vector<GCTensor*> parents{input};
        if (weight) parents.push_back(weight);
        if (bias) parents.push_back(bias);
        bool requires_grad = input->requires_grad
                          || (weight && weight->requires_grad)
                          || (bias && bias->requires_grad);
        Value result = ag_make_cuda_tensor(
            "autograd_layer_norm", input->shape, requires_grad,
            TensorOp::LAYER_NORM, std::move(parents), l);
        GCNativeRoot result_root(result.as_obj());
        GCTensor* output = result.as_tensor();
        std::shared_ptr<AgCudaAllocation> saved_mean;
        std::shared_ptr<AgCudaAllocation> saved_rstd;
        if (requires_grad) {
            saved_mean = ag_cuda_allocate("autograd_layer_norm", outer, l);
            saved_rstd = ag_cuda_allocate("autograd_layer_norm", outer, l);
            ag_set_op_indices(output,
                              {weight ? 1ULL : 0ULL, bias ? 1ULL : 0ULL},
                              "autograd_layer_norm", l);
            output->op_scalar = epsilon;
        }
        if (!SuraCudaDriver::instance().layer_norm_f32(
                input->cuda_data->handle,
                weight ? weight->cuda_data->handle
                       : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                bias ? bias->cuda_data->handle
                     : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                output->cuda_data->handle,
                saved_mean ? saved_mean->handle
                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                saved_rstd ? saved_rstd->handle
                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                (uint32_t)outer, (uint32_t)features, (float)epsilon,
                weight != nullptr, bias != nullptr, requires_grad)) {
            ag_cuda_fail("autograd_layer_norm", l);
        }
        if (requires_grad) {
            output->cuda_layer_norm_mean = std::move(saved_mean);
            output->cuda_layer_norm_rstd = std::move(saved_rstd);
        }
        return result;
    }
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_layer_norm", l);
    std::vector<double> data(input->data.size(), 0.0);
    for (size_t row = 0; row < outer; ++row) {
        size_t base = row * features;
        AgLayerNormStats stats = ag_layer_norm_stats(input, base, features, epsilon);
        for (size_t col = 0; col < features; ++col) {
            double normalized = ag_layer_norm_normalized(input, base + col, stats);
            double value = normalized * (weight ? weight->data[col] : 1.0)
                         + (bias ? bias->data[col] : 0.0);
            if (!std::isfinite(value)) throw JitThrow{"autograd_layer_norm(): result is not finite", l};
            data[base + col] = value;
        }
    }
    std::vector<GCTensor*> parents{input};
    if (weight) parents.push_back(weight);
    if (bias) parents.push_back(bias);
    bool requires_grad = input->requires_grad
                      || (weight && weight->requires_grad)
                      || (bias && bias->requires_grad);
    TensorDType dtype = input->data.dtype();
    if (weight) dtype = ag_promote_dtype(dtype, weight->data.dtype());
    if (bias) dtype = ag_promote_dtype(dtype, bias->data.dtype());
    Value result = ag_make_tensor("autograd_layer_norm", std::move(data), input->shape,
                                  requires_grad, TensorOp::LAYER_NORM,
                                  std::move(parents), l, dtype);
    if (requires_grad) {
        ag_set_op_indices(result.as_tensor(),
                          {weight ? 1ULL : 0ULL, bias ? 1ULL : 0ULL},
                          "autograd_layer_norm", l);
        result.as_tensor()->op_scalar = epsilon;
    }
    return result;
}

inline void ag_collect_token_ids(const char* name, const Value& value,
                                 size_t vocabulary, std::vector<size_t>& ids,
                                 int line) {
    if (value.is_num()) {
        double raw = value.as_num();
        if (!std::isfinite(raw) || raw < 0.0 || raw != std::floor(raw)
            || raw >= (double)vocabulary) {
            throw JitThrow{std::string(name) + "(): token ids must be integers within the vocabulary", line};
        }
        ids.push_back((size_t)raw);
        return;
    }
    for (const Value& item : value.as_arr()->elements) {
        ag_collect_token_ids(name, item, vocabulary, ids, line);
    }
}

inline std::vector<size_t> ag_token_ids(const char* name, const Value& value,
                                        size_t vocabulary, std::vector<size_t>& shape,
                                        int line) {
    std::vector<double> owned;
    const std::vector<double>* numeric = nullptr;
    if (value.is_tensor()) {
        GCTensor* tokens = value.as_tensor();
        if (ag_is_cuda(tokens)) {
            throw JitThrow{std::string(name)
                           + "(): CUDA token tensors are not supported yet; "
                             "pass host-validated CPU ids, which are uploaded once as uint32",
                           line};
        }
        if (tokens->requires_grad) {
            throw JitThrow{std::string(name) + "(): token ids cannot require gradients", line};
        }
        shape = tokens->shape;
        owned = tokens->data.to_vector();
        numeric = &owned;
    } else if (value.is_num() || value.is_arr()) {
        std::unordered_set<const GCArray*> active;
        shape = ag_infer_shape_impl(name, value, active, 0, line);
    } else {
        throw JitThrow{std::string(name) + "(): token ids must be a tensor, number, or rectangular numeric array", line};
    }
    size_t count = ag_numel(name, shape, line);
    ag_preflight_bytes(count * sizeof(size_t), name, line);
    std::vector<size_t> ids;
    ids.reserve(count);
    if (numeric) {
        for (double raw : *numeric) {
            if (!std::isfinite(raw) || raw < 0.0 || raw != std::floor(raw)
                || raw >= (double)vocabulary) {
                throw JitThrow{std::string(name) + "(): token ids must be integers within the vocabulary", line};
            }
            ids.push_back((size_t)raw);
        }
    } else {
        ag_collect_token_ids(name, value, vocabulary, ids, line);
    }
    if (ids.size() != count) {
        throw JitThrow{std::string(name) + "(): token id shape is inconsistent", line};
    }
    return ids;
}

inline Value b_autograd_embedding(const Value* a, int n, int l) {
    need_args("autograd_embedding", n, 2, 2, l);
    GCTensor* weight = ag_need_tensor("autograd_embedding", a[1], 1, l);
    ag_check_graph_operand("autograd_embedding", weight, l);
    if (weight->shape.size() != 2) {
        throw JitThrow{"autograd_embedding(): weight must have shape [vocabulary, dimensions]", l};
    }
    std::vector<size_t> token_shape;
    std::vector<size_t> ids = ag_token_ids(
        "autograd_embedding", a[0], weight->shape[0], token_shape, l);
    if (token_shape.size() + 1 > AG_MAX_RANK) {
        throw JitThrow{"autograd_embedding(): output rank exceeds the tensor rank limit", l};
    }
    size_t dimensions = weight->shape[1];
    std::vector<size_t> shape = token_shape;
    shape.push_back(dimensions);
    size_t output_count = ag_numel("autograd_embedding", shape, l);
    if (ag_is_cuda(weight)) {
        ag_cuda_require_f32_tensor("autograd_embedding", weight, l);
        const size_t vocabulary = weight->shape[0];
        if (vocabulary > (size_t)std::numeric_limits<uint32_t>::max()
            || dimensions > (size_t)std::numeric_limits<uint32_t>::max()
            || ids.size() > (size_t)std::numeric_limits<uint32_t>::max()
            || output_count > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_embedding(): CUDA dimensions are too large", l};
        }
        AgTemporaryBytes id_staging(ids.size() * sizeof(uint32_t),
                                    "autograd_embedding", l);
        std::vector<uint32_t> packed_ids(ids.size());
        for (size_t index = 0; index < ids.size(); ++index) {
            packed_ids[index] = (uint32_t)ids[index];
        }
        auto device_ids = ag_cuda_allocate_u32("autograd_embedding", ids.size(), l);
        if (!SuraCudaDriver::instance().upload_u32(
                device_ids->handle, packed_ids)) {
            ag_cuda_fail("autograd_embedding", l);
        }
        Value result = ag_make_cuda_tensor(
            "autograd_embedding", std::move(shape), weight->requires_grad,
            TensorOp::EMBEDDING, {weight}, l);
        GCNativeRoot result_root(result.as_obj());
        if (!SuraCudaDriver::instance().embedding_f32(
                weight->cuda_data->handle, device_ids->handle,
                result.as_tensor()->cuda_data->handle,
                (uint32_t)vocabulary, (uint32_t)ids.size(),
                (uint32_t)dimensions)) {
            ag_cuda_fail("autograd_embedding", l);
        }
        result.as_tensor()->cuda_embedding_ids = std::move(device_ids);
        return result;
    }
    ag_preflight_bytes(output_count * sizeof(double) + ids.size() * sizeof(size_t),
                       "autograd_embedding", l);
    std::vector<double> data(output_count, 0.0);
    for (size_t token = 0; token < ids.size(); ++token) {
        size_t source = ids[token] * dimensions;
        for (size_t dimension = 0; dimension < dimensions; ++dimension) {
            data[token * dimensions + dimension] = weight->data[source + dimension];
        }
    }
    Value result = ag_make_tensor("autograd_embedding", std::move(data), std::move(shape),
                                  weight->requires_grad, TensorOp::EMBEDDING, {weight}, l,
                                  weight->data.dtype());
    if (weight->requires_grad) {
        ag_set_op_indices(result.as_tensor(), std::move(ids), "autograd_embedding", l);
    }
    return result;
}

inline Value b_autograd_causal_attention(const Value* a, int n, int l) {
    need_args("autograd_causal_attention", n, 3, 4, l);
    GCTensor* query = ag_need_tensor("autograd_causal_attention", a[0], 0, l);
    GCTensor* key = ag_need_tensor("autograd_causal_attention", a[1], 1, l);
    GCTensor* value = ag_need_tensor("autograd_causal_attention", a[2], 2, l);
    ag_check_graph_operand("autograd_causal_attention", query, l);
    ag_check_graph_operand("autograd_causal_attention", key, l);
    ag_check_graph_operand("autograd_causal_attention", value, l);
    const bool resident_cuda = ag_is_cuda(query) || ag_is_cuda(key) || ag_is_cuda(value);
    if (query->shape.size() < 2 || query->shape.size() != key->shape.size()
        || query->shape.size() != value->shape.size()) {
        throw JitThrow{"autograd_causal_attention(): q, k, and v must have the same rank of at least 2", l};
    }
    std::vector<size_t> query_prefix = ag_prefix_shape(query->shape, 2);
    std::vector<size_t> key_prefix = ag_prefix_shape(key->shape, 2);
    std::vector<size_t> value_prefix = ag_prefix_shape(value->shape, 2);
    size_t sequence = query->shape[query->shape.size() - 2];
    size_t dimensions = query->shape.back();
    size_t value_dimensions = value->shape.back();
    if (query_prefix != key_prefix || query_prefix != value_prefix
        || key->shape[key->shape.size() - 2] != sequence
        || value->shape[value->shape.size() - 2] != sequence
        || key->shape.back() != dimensions) {
        throw JitThrow{"autograd_causal_attention(): incompatible q, k, and v shapes", l};
    }
    GCDict* options = nn_options("autograd_causal_attention", a, n, 3, l);
    ag_validate_options("autograd_causal_attention", options,
                        {"scale", "precision"}, l);
    double default_scale = 1.0 / std::sqrt((double)dimensions);
    double scale = nn_option_number("autograd_causal_attention", options, "scale",
                                    default_scale, 1e-12, 1e12, l);
    const std::string precision = nn_option_string(
        "autograd_causal_attention", options, "precision", "auto", l);
    if (precision != "auto" && precision != "fast" && precision != "strict") {
        throw JitThrow{
            "autograd_causal_attention(): precision must be auto, fast, or strict", l};
    }
    const bool strict_precision = precision == "strict";
    const bool fast_precision = precision == "fast";
    std::vector<size_t> shape = query_prefix;
    shape.push_back(sequence);
    shape.push_back(value_dimensions);
    size_t batches = ag_product(query_prefix);
    ag_validate_attention_work("autograd_causal_attention", batches, sequence, l);
    size_t output_count = ag_numel("autograd_causal_attention", shape, l);
    bool requires_grad = query->requires_grad || key->requires_grad || value->requires_grad;
    if (resident_cuda) {
        ag_cuda_require_same_device("autograd_causal_attention", query, key, l);
        ag_cuda_require_same_device("autograd_causal_attention", query, value, l);
        ag_cuda_require_float32("autograd_causal_attention", query, l);
        ag_cuda_require_float32("autograd_causal_attention", key, l);
        ag_cuda_require_float32("autograd_causal_attention", value, l);
        const uint64_t total_rows = (uint64_t)batches * sequence;
        if (batches > (size_t)std::numeric_limits<uint32_t>::max()
            || sequence > (size_t)std::numeric_limits<uint32_t>::max()
            || dimensions > (size_t)std::numeric_limits<uint32_t>::max()
            || value_dimensions > (size_t)std::numeric_limits<uint32_t>::max()
            || total_rows > (uint64_t)std::numeric_limits<uint32_t>::max()
            || query->data.size() > (size_t)std::numeric_limits<uint32_t>::max()
            || value->data.size() > (size_t)std::numeric_limits<uint32_t>::max()
            || output_count > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_causal_attention(): CUDA dimensions are too large", l};
        }
        const float cuda_scale = (float)scale;
        if (!std::isfinite(cuda_scale) || cuda_scale <= 0.0f) {
            throw JitThrow{"autograd_causal_attention(): scale overflows CUDA float32", l};
        }
        uint64_t planned_pairs = 0;
        uint64_t planned_workspace_bytes = 0;
        const bool fused_available = ag_cuda_attention_fused_plan(sequence);
        if (fast_precision && !fused_available) {
            throw JitThrow{
                "autograd_causal_attention(): precision fast requires CUDA sequence >= 8 "
                "with SURA_CUDA_ATTENTION_PARALLEL and SURA_CUDA_ATTENTION_FUSED enabled",
                l};
        }
        const bool fast_warp = !strict_precision && fused_available;
        const bool fused_plan = requires_grad && fast_warp;
        const bool parallel_plan = requires_grad && !strict_precision
            && !fused_plan
            && ag_cuda_attention_parallel_plan(
                batches, sequence, planned_pairs, planned_workspace_bytes);
        const bool warp_forward = !strict_precision
            && (fast_warp || parallel_plan
            || (!requires_grad
                && sequence >= AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE
                && ag_cuda_attention_parallel_enabled()));
        Value result = ag_make_cuda_tensor(
            "autograd_causal_attention", std::move(shape), requires_grad,
            TensorOp::CAUSAL_ATTENTION, {query, key, value}, l);
        GCNativeRoot result_root(result.as_obj());
        GCTensor* output = result.as_tensor();
        std::shared_ptr<AgCudaAllocation> saved_max;
        std::shared_ptr<AgCudaAllocation> saved_inv_sum;
        try {
            if (requires_grad) {
                saved_max = ag_cuda_allocate(
                    "autograd_causal_attention", (size_t)total_rows, l);
                saved_inv_sum = ag_cuda_allocate(
                    "autograd_causal_attention", (size_t)total_rows, l);
                output->op_scalar = scale;
            }
            if (!SuraCudaDriver::instance().causal_attention_f32(
                    query->cuda_data->handle, key->cuda_data->handle,
                    value->cuda_data->handle, output->cuda_data->handle,
                    saved_max ? saved_max->handle
                              : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                    saved_inv_sum ? saved_inv_sum->handle
                                  : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                    (uint32_t)batches, (uint32_t)sequence,
                    (uint32_t)dimensions, (uint32_t)value_dimensions,
                    cuda_scale, requires_grad, warp_forward, fast_warp)) {
                ag_cuda_fail("autograd_causal_attention", l);
            }
            output->cuda_attention_parallel_plan = parallel_plan;
            output->cuda_attention_fused_plan = fused_plan;
            if (requires_grad) {
                output->cuda_attention_max = std::move(saved_max);
                output->cuda_attention_inv_sum = std::move(saved_inv_sum);
            }
        } catch (...) {
            // The output tensor has not escaped yet. Release its resident
            // allocation immediately so an allocation/launch failure does
            // not retain VRAM until the next tracing-GC cycle.
            output->cuda_attention_max.reset();
            output->cuda_attention_inv_sum.reset();
            output->cuda_attention_parallel_plan = false;
            output->cuda_attention_fused_plan = false;
            output->cuda_data.reset();
            throw;
        }
        return result;
    }
    if (fast_precision) {
        throw JitThrow{
            "autograd_causal_attention(): precision fast requires resident CUDA float32 tensors",
            l};
    }
    // Online softmax keeps only one output row instead of a sequence-sized
    // score/probability buffer. This is the same stable recurrence used by
    // memory-efficient attention kernels; the CPU implementation remains an
    // exact reference path and backward recomputes probabilities.
    size_t scratch_bytes = value_dimensions * sizeof(double);
    ag_preflight_bytes(output_count * sizeof(double) + scratch_bytes,
                       "autograd_causal_attention", l);
    std::vector<double> data(output_count, 0.0);
    size_t q_matrix = sequence * dimensions;
    size_t v_matrix = sequence * value_dimensions;
    AgTemporaryBytes scratch_guard(scratch_bytes, "autograd_causal_attention", l);
    std::vector<double> accumulator(value_dimensions, 0.0);
    for (size_t batch = 0; batch < batches; ++batch) {
        size_t q_base = batch * q_matrix;
        size_t v_base = batch * v_matrix;
        for (size_t row = 0; row < sequence; ++row) {
            double maximum = -std::numeric_limits<double>::infinity();
            double total = 0.0;
            std::fill(accumulator.begin(), accumulator.end(), 0.0);
            for (size_t col = 0; col <= row; ++col) {
                double score = 0.0;
                for (size_t d = 0; d < dimensions; ++d) {
                    score += query->data[q_base + row * dimensions + d]
                           * key->data[q_base + col * dimensions + d];
                }
                score *= scale;
                if (!std::isfinite(score)) {
                    throw JitThrow{"autograd_causal_attention(): attention score is not finite", l};
                }
                double next_maximum = std::max(maximum, score);
                double previous_scale = std::isfinite(maximum)
                    ? std::exp(maximum - next_maximum) : 0.0;
                double weight = std::exp(score - next_maximum);
                total = total * previous_scale + weight;
                for (size_t d = 0; d < value_dimensions; ++d) {
                    accumulator[d] = accumulator[d] * previous_scale
                                   + weight * value->data[v_base + col * value_dimensions + d];
                }
                maximum = next_maximum;
            }
            for (size_t d = 0; d < value_dimensions; ++d) {
                double output = accumulator[d] / total;
                if (!std::isfinite(output)) {
                    throw JitThrow{"autograd_causal_attention(): result is not finite", l};
                }
                data[v_base + row * value_dimensions + d] = output;
            }
        }
    }
    Value result = ag_make_tensor("autograd_causal_attention", std::move(data), std::move(shape),
                                  requires_grad, TensorOp::CAUSAL_ATTENTION,
                                  {query, key, value}, l,
                                  ag_promote_dtype(query->data.dtype(), key->data.dtype(),
                                                   value->data.dtype()));
    if (requires_grad) result.as_tensor()->op_scalar = scale;
    return result;
}

inline Value b_autograd_softmax(const Value* a, int n, int l) {
    need_args("autograd_softmax", n, 1, 1, l);
    GCTensor* input = ag_need_tensor("autograd_softmax", a[0], 0, l);
    ag_check_graph_operand("autograd_softmax", input, l);
    ag_cuda_reject_unsupported("autograd_softmax", {input}, l);
    if (input->shape.empty() || input->shape.back() < 2) {
        throw JitThrow{"autograd_softmax(): last dimension must contain at least 2 values", l};
    }
    size_t classes = input->shape.back();
    size_t outer = input->data.size() / classes;
    ag_preflight_bytes(input->data.size() * sizeof(double), "autograd_softmax", l);
    std::vector<double> data(input->data.size(), 0.0);
    for (size_t row = 0; row < outer; ++row) {
        size_t base = row * classes;
        double maximum = input->data[base];
        for (size_t col = 1; col < classes; ++col) maximum = std::max(maximum, input->data[base + col]);
        double total = 0.0;
        for (size_t col = 0; col < classes; ++col) {
            data[base + col] = std::exp(input->data[base + col] - maximum);
            total += data[base + col];
        }
        for (size_t col = 0; col < classes; ++col) data[base + col] /= total;
    }
    return ag_make_tensor("autograd_softmax", std::move(data), input->shape,
                          input->requires_grad, TensorOp::SOFTMAX, {input}, l,
                          input->data.dtype());
}

inline Value ag_reduce_all(const char* name, const Value* a, int n, int l, TensorOp op) {
    need_args(name, n, 1, 1, l);
    GCTensor* input = ag_need_tensor(name, a[0], 0, l);
    ag_check_graph_operand(name, input, l);
    if (ag_is_cuda(input)) {
        ag_cuda_require_f32_tensor(name, input, l);
        Value result = ag_make_cuda_tensor(name, {}, input->requires_grad,
                                            op, {input}, l);
        GCNativeRoot result_root(result.as_obj());
        GCTensor* output = result.as_tensor();
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        if (!driver.sum_f32(input->cuda_data->handle,
                            output->cuda_data->handle, input->data.size())) {
            ag_cuda_fail(name, l);
        }
        if (op == TensorOp::MEAN
            && !driver.scale_f32(output->cuda_data->handle,
                                 output->cuda_data->handle,
                                 1.0f / (float)input->data.size(), 1)) {
            ag_cuda_fail(name, l);
        }
        return result;
    }
    double total = 0.0;
    for (double value : input->data) {
        total += value;
        if (!std::isfinite(total)) throw JitThrow{std::string(name) + "(): result is not finite", l};
    }
    if (op == TensorOp::MEAN) total /= (double)input->data.size();
    return ag_make_tensor(name, {total}, {}, input->requires_grad, op, {input}, l,
                          input->data.dtype());
}

inline Value b_autograd_sum(const Value* a, int n, int l) {
    return ag_reduce_all("autograd_sum", a, n, l, TensorOp::SUM);
}
inline Value b_autograd_mean(const Value* a, int n, int l) {
    return ag_reduce_all("autograd_mean", a, n, l, TensorOp::MEAN);
}

inline Value ag_pair_loss(const char* name, const Value* a, int n, int l, TensorOp op) {
    need_args(name, n, 2, 2, l);
    Value prediction_value = ag_coerce_tensor(name, a[0], l);
    GCNativeRoot prediction_root(prediction_value.as_obj());
    Value target_value = ag_coerce_tensor(name, a[1], l,
                                          prediction_value.as_tensor()->data.dtype());
    GCNativeRoot target_root(target_value.as_obj());
    GCTensor* prediction = prediction_value.as_tensor();
    GCTensor* target = target_value.as_tensor();
    ag_check_graph_operand(name, prediction, l);
    ag_check_graph_operand(name, target, l);
    std::vector<size_t> shape = ag_broadcast_shape(name, prediction->shape, target->shape, l);
    size_t count = ag_numel(name, shape, l);
    if (ag_is_cuda(prediction) || ag_is_cuda(target)) {
        if (op != TensorOp::MSE) {
            throw JitThrow{std::string(name)
                           + "(): resident CUDA currently supports mse loss only", l};
        }
        ag_cuda_require_same_device(name, prediction, target, l);
        ag_cuda_require_float32(name, prediction, l);
        ag_cuda_require_float32(name, target, l);
        if (prediction->shape != target->shape) {
            throw JitThrow{std::string(name)
                           + "(): resident CUDA mse currently requires identical shapes", l};
        }
        Value difference_args[] = {prediction_value, target_value};
        Value difference = b_autograd_sub(difference_args, 2, l);
        GCNativeRoot difference_root(difference.as_obj());
        Value square_args[] = {difference, difference};
        Value square = b_autograd_mul(square_args, 2, l);
        GCNativeRoot square_root(square.as_obj());
        Value mean_args[] = {square};
        return b_autograd_mean(mean_args, 1, l);
    }
    double loss = 0.0;
    constexpr double epsilon = 1e-12;
    for (size_t i = 0; i < count; ++i) {
        double x = prediction->data[ag_broadcast_index(i, shape, prediction->shape)];
        double y = target->data[ag_broadcast_index(i, shape, target->shape)];
        if (op == TensorOp::MSE) {
            double difference = x - y;
            loss += difference * difference;
        } else if (op == TensorOp::BCE) {
            if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                throw JitThrow{std::string(name) + "(): probabilities and targets must be between 0 and 1", l};
            }
            double probability = epsilon + (1.0 - 2.0 * epsilon) * x;
            loss -= y * std::log(probability) + (1.0 - y) * std::log(1.0 - probability);
        } else {
            if (y < 0.0 || y > 1.0) {
                throw JitThrow{std::string(name) + "(): targets must be between 0 and 1", l};
            }
            loss += std::max(x, 0.0) - x * y + std::log1p(std::exp(-std::abs(x)));
        }
    }
    loss /= (double)count;
    if (!std::isfinite(loss)) throw JitThrow{std::string(name) + "(): loss is not finite", l};
    return ag_make_tensor(name, {loss}, {}, prediction->requires_grad || target->requires_grad,
                          op, {prediction, target}, l,
                          ag_promote_dtype(prediction->data.dtype(), target->data.dtype()));
}

inline Value b_autograd_mse(const Value* a, int n, int l) {
    return ag_pair_loss("autograd_mse", a, n, l, TensorOp::MSE);
}
inline Value b_autograd_bce(const Value* a, int n, int l) {
    return ag_pair_loss("autograd_bce", a, n, l, TensorOp::BCE);
}
inline Value b_autograd_bce_logits(const Value* a, int n, int l) {
    return ag_pair_loss("autograd_bce_logits", a, n, l, TensorOp::BCE_LOGITS);
}

inline Value b_autograd_cross_entropy(const Value* a, int n, int l) {
    need_args("autograd_cross_entropy", n, 2, 2, l);
    GCTensor* logits = ag_need_tensor("autograd_cross_entropy", a[0], 0, l);
    Value target_value = ag_coerce_tensor("autograd_cross_entropy", a[1], l,
                                          logits->data.dtype());
    GCNativeRoot target_root(target_value.as_obj());
    GCTensor* target = target_value.as_tensor();
    ag_check_graph_operand("autograd_cross_entropy", logits, l);
    ag_check_graph_operand("autograd_cross_entropy", target, l);
    ag_cuda_reject_unsupported("autograd_cross_entropy", {logits, target}, l);
    if (logits->shape != target->shape || logits->shape.empty() || logits->shape.back() < 2) {
        throw JitThrow{"autograd_cross_entropy(): logits and one-hot targets must have the same shape", l};
    }
    size_t classes = logits->shape.back();
    size_t outer = logits->data.size() / classes;
    double loss = 0.0;
    for (size_t row = 0; row < outer; ++row) {
        size_t base = row * classes;
        double maximum = logits->data[base];
        for (size_t col = 1; col < classes; ++col) maximum = std::max(maximum, logits->data[base + col]);
        double exp_total = 0.0;
        for (size_t col = 0; col < classes; ++col) exp_total += std::exp(logits->data[base + col] - maximum);
        double log_denom = maximum + std::log(exp_total);
        double target_total = 0.0;
        for (size_t col = 0; col < classes; ++col) {
            double expected = target->data[base + col];
            if (expected < 0.0 || expected > 1.0) {
                throw JitThrow{"autograd_cross_entropy(): targets must be probabilities", l};
            }
            target_total += expected;
            loss -= expected * (logits->data[base + col] - log_denom);
        }
        if (std::abs(target_total - 1.0) > 0.000001) {
            throw JitThrow{"autograd_cross_entropy(): each target row must sum to 1", l};
        }
    }
    loss /= (double)outer;
    if (!std::isfinite(loss)) {
        throw JitThrow{"autograd_cross_entropy(): loss is not finite", l};
    }
    return ag_make_tensor("autograd_cross_entropy", {loss}, {},
                          logits->requires_grad || target->requires_grad,
                          TensorOp::CROSS_ENTROPY, {logits, target}, l,
                          ag_promote_dtype(logits->data.dtype(), target->data.dtype()));
}

inline Value b_autograd_cross_entropy_ids(const Value* a, int n, int l) {
    need_args("autograd_cross_entropy_ids", n, 2, 2, l);
    GCTensor* logits = ag_need_tensor("autograd_cross_entropy_ids", a[0], 0, l);
    ag_check_graph_operand("autograd_cross_entropy_ids", logits, l);
    if (logits->shape.empty() || logits->shape.back() < 2) {
        throw JitThrow{"autograd_cross_entropy_ids(): logits must have a class dimension of size 2 or greater", l};
    }
    size_t classes = logits->shape.back();
    std::vector<size_t> target_shape;
    std::vector<size_t> ids = ag_token_ids(
        "autograd_cross_entropy_ids", a[1], classes, target_shape, l);
    std::vector<size_t> expected_shape = ag_prefix_shape(logits->shape, 1);
    if (target_shape != expected_shape) {
        throw JitThrow{"autograd_cross_entropy_ids(): target shape must equal logits shape without the class dimension", l};
    }
    size_t outer = logits->data.size() / classes;
    if (ids.size() != outer) {
        throw JitThrow{"autograd_cross_entropy_ids(): target count does not match logits", l};
    }
    if (ag_is_cuda(logits)) {
        ag_cuda_require_f32_tensor("autograd_cross_entropy_ids", logits, l);
        if (outer > (size_t)std::numeric_limits<uint32_t>::max()
            || classes > (size_t)std::numeric_limits<uint32_t>::max()
            || logits->data.size() > (size_t)std::numeric_limits<uint32_t>::max()) {
            throw JitThrow{"autograd_cross_entropy_ids(): CUDA dimensions are too large", l};
        }
        AgTemporaryBytes id_staging(ids.size() * sizeof(uint32_t),
                                    "autograd_cross_entropy_ids", l);
        std::vector<uint32_t> packed_ids(ids.size());
        for (size_t index = 0; index < ids.size(); ++index) {
            packed_ids[index] = (uint32_t)ids[index];
        }
        auto device_ids = ag_cuda_allocate_u32(
            "autograd_cross_entropy_ids", ids.size(), l);
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        if (!driver.upload_u32(device_ids->handle, packed_ids)) {
            ag_cuda_fail("autograd_cross_entropy_ids", l);
        }
        Value result = ag_make_cuda_tensor(
            "autograd_cross_entropy_ids", {}, logits->requires_grad,
            TensorOp::CROSS_ENTROPY_IDS, {logits}, l);
        GCNativeRoot result_root(result.as_obj());
        GCTensor* output = result.as_tensor();
        auto saved_max = ag_cuda_allocate(
            "autograd_cross_entropy_ids", outer, l);
        auto saved_inv_sum = ag_cuda_allocate(
            "autograd_cross_entropy_ids", outer, l);
        if (!driver.cross_entropy_ids_f32(
                logits->cuda_data->handle, device_ids->handle,
                output->cuda_data->handle,
                saved_max->handle, saved_inv_sum->handle,
                (uint32_t)outer, (uint32_t)classes)) {
            ag_cuda_fail("autograd_cross_entropy_ids", l);
        }
        if (logits->requires_grad) {
            output->cuda_cross_entropy_ids = std::move(device_ids);
            output->cuda_cross_entropy_max = std::move(saved_max);
            output->cuda_cross_entropy_inv_sum = std::move(saved_inv_sum);
        } else if (!driver.synchronize()) {
            // The two-stage forward consumes all three workspace buffers
            // asynchronously. Synchronize once before their local RAII owners
            // release them so a no-grad scalar retains only its own storage.
            ag_cuda_fail("autograd_cross_entropy_ids", l);
        }
        return result;
    }
    double loss = 0.0;
    for (size_t row = 0; row < outer; ++row) {
        size_t base = row * classes;
        double maximum = logits->data[base];
        for (size_t col = 1; col < classes; ++col) {
            maximum = std::max(maximum, logits->data[base + col]);
        }
        double total = 0.0;
        for (size_t col = 0; col < classes; ++col) {
            total += std::exp(logits->data[base + col] - maximum);
        }
        double log_denom = maximum + std::log(total);
        loss -= logits->data[base + ids[row]] - log_denom;
    }
    loss /= (double)outer;
    if (!std::isfinite(loss)) {
        throw JitThrow{"autograd_cross_entropy_ids(): loss is not finite", l};
    }
    Value result = ag_make_tensor("autograd_cross_entropy_ids", {loss}, {},
                                  logits->requires_grad, TensorOp::CROSS_ENTROPY_IDS,
                                  {logits}, l, logits->data.dtype());
    if (logits->requires_grad) {
        ag_set_op_indices(result.as_tensor(), std::move(ids),
                          "autograd_cross_entropy_ids", l);
    }
    return result;
}

inline Value b_autograd_linear(const Value* a, int n, int l) {
    need_args("autograd_linear", n, 2, 3, l);
    Value matmul_args[2] = {a[0], a[1]};
    Value output = b_autograd_matmul(matmul_args, 2, l);
    if (n < 3) return output;
    GCNativeRoot output_root(output.as_obj());
    Value add_args[2] = {output, a[2]};
    return b_autograd_add(add_args, 2, l);
}

inline void ag_validate_saved_versions(const char* name, const GCTensor* tensor, int line) {
    if (tensor->parents.size() != tensor->parent_versions.size()) {
        throw JitThrow{std::string(name) + "(): corrupted autograd graph", line};
    }
    for (size_t i = 0; i < tensor->parents.size(); ++i) {
        if (tensor->parents[i]->version != tensor->parent_versions[i]) {
            throw JitThrow{std::string(name)
                           + "(): a saved tensor was modified in-place after forward", line};
        }
    }
}

struct AgLeafGradientStage {
    std::unordered_map<GCTensor*, std::vector<double>> pending;
};

inline AgLeafGradientStage*& ag_leaf_gradient_stage() {
    static thread_local AgLeafGradientStage* stage = nullptr;
    return stage;
}

class AgLeafGradientScope {
    AgLeafGradientStage* previous_ = nullptr;
public:
    explicit AgLeafGradientScope(AgLeafGradientStage* stage)
        : previous_(ag_leaf_gradient_stage()) {
        ag_leaf_gradient_stage() = stage;
    }
    ~AgLeafGradientScope() { ag_leaf_gradient_stage() = previous_; }
    AgLeafGradientScope(const AgLeafGradientScope&) = delete;
    AgLeafGradientScope& operator=(const AgLeafGradientScope&) = delete;
};

inline void ag_accumulate(GCTensor* tensor, size_t index, double value,
                          const char* name, int line) {
    if (!tensor->requires_grad) return;
    if (!std::isfinite(value)) {
        throw JitThrow{std::string(name) + "(): gradient is not finite", line};
    }
    if (tensor->op == TensorOp::LEAF && ag_leaf_gradient_stage()) {
        auto& staged = ag_leaf_gradient_stage()->pending[tensor];
        if (staged.empty()) staged.assign(tensor->data.size(), 0.0);
        double next = staged[index] + value;
        if (!std::isfinite(next)) {
            throw JitThrow{std::string(name) + "(): gradient is not finite", line};
        }
        staged[index] = next;
        return;
    }
    ag_ensure_grad(tensor, name, line);
    double next = tensor->grad[index] + value;
    if (!std::isfinite(next)) {
        throw JitThrow{std::string(name) + "(): accumulated gradient is not finite", line};
    }
    tensor->grad[index] = next;
}

// Evaluate (left * right) / divisor without avoidable intermediate
// overflow/underflow. All mantissas remain in a small normalized range while
// exponents are combined separately, then rounded once to float64.
inline double ag_scaled_mul_div(double left, double right, double divisor) {
    if (left == 0.0 || right == 0.0) {
        bool negative = std::signbit(left) ^ std::signbit(right) ^ std::signbit(divisor);
        return std::copysign(0.0, negative ? -1.0 : 1.0);
    }
    int left_exp = 0, right_exp = 0, divisor_exp = 0;
    double left_mantissa = std::frexp(std::abs(left), &left_exp);
    double right_mantissa = std::frexp(std::abs(right), &right_exp);
    double divisor_mantissa = std::frexp(std::abs(divisor), &divisor_exp);
    double mantissa = (left_mantissa * right_mantissa) / divisor_mantissa;
    int normalize_exp = 0;
    mantissa = std::frexp(mantissa, &normalize_exp);
    int exponent = left_exp + right_exp - divisor_exp + normalize_exp;
    double magnitude = std::ldexp(mantissa, exponent);
    bool negative = std::signbit(left) ^ std::signbit(right) ^ std::signbit(divisor);
    return negative ? -magnitude : magnitude;
}

inline std::vector<GCTensor*> ag_topological_graph(const char* name, GCTensor* root, int line) {
    std::vector<GCTensor*> topo;
    std::vector<std::pair<GCTensor*, bool>> stack;
    std::unordered_set<GCTensor*> seen;
    stack.push_back({root, false});
    while (!stack.empty()) {
        auto [tensor, expanded] = stack.back();
        stack.pop_back();
        if (expanded) {
            topo.push_back(tensor);
            continue;
        }
        if (!seen.insert(tensor).second) continue;
        if (seen.size() > AG_MAX_GRAPH_NODES) {
            throw JitThrow{std::string(name) + "(): autograd graph exceeds the 1000000 node safety limit", line};
        }
        if (tensor->op != TensorOp::LEAF && tensor->graph_freed) {
            throw JitThrow{std::string(name)
                           + "(): graph was already freed; pass retain_graph=true on the first backward call", line};
        }
        stack.push_back({tensor, true});
        for (auto it = tensor->parents.rbegin(); it != tensor->parents.rend(); ++it) {
            if (*it) stack.push_back({*it, false});
        }
    }
    return topo;
}

inline void ag_backward_node(GCTensor* node, const char* name, int line) {
    if (node->op == TensorOp::LEAF || node->grad.empty()) return;
    ag_validate_saved_versions(name, node, line);
    GCTensor* left = node->parents.empty() ? nullptr : node->parents[0];
    GCTensor* right = node->parents.size() < 2 ? nullptr : node->parents[1];

    if (node->op == TensorOp::ADD || node->op == TensorOp::SUB
        || node->op == TensorOp::MUL || node->op == TensorOp::DIV) {
        for (size_t i = 0; i < node->data.size(); ++i) {
            size_t li = ag_broadcast_index(i, node->shape, left->shape);
            size_t ri = ag_broadcast_index(i, node->shape, right->shape);
            double upstream = node->grad[i];
            double x = left->data[li], y = right->data[ri];
            if (node->op == TensorOp::ADD) {
                ag_accumulate(left, li, upstream, name, line);
                ag_accumulate(right, ri, upstream, name, line);
            } else if (node->op == TensorOp::SUB) {
                ag_accumulate(left, li, upstream, name, line);
                ag_accumulate(right, ri, -upstream, name, line);
            } else if (node->op == TensorOp::MUL) {
                ag_accumulate(left, li, upstream * y, name, line);
                ag_accumulate(right, ri, upstream * x, name, line);
            } else {
                ag_accumulate(left, li, upstream / y, name, line);
                ag_accumulate(right, ri,
                              -ag_scaled_mul_div(upstream, node->data[i], y), name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::NEG) {
        for (size_t i = 0; i < node->grad.size(); ++i) ag_accumulate(left, i, -node->grad[i], name, line);
        return;
    }
    if (node->op == TensorOp::RESHAPE || node->op == TensorOp::CAST
        || node->op == TensorOp::DEVICE_COPY) {
        for (size_t i = 0; i < node->grad.size(); ++i) {
            ag_accumulate(left, i, node->grad[i], name, line);
        }
        return;
    }
    if (node->op == TensorOp::MATMUL) {
        size_t rows = left->shape[left->shape.size() - 2];
        size_t inner = left->shape.back();
        size_t cols = right->shape.back();
        std::vector<size_t> left_batch = ag_prefix_shape(left->shape, 2);
        std::vector<size_t> right_batch = ag_prefix_shape(right->shape, 2);
        std::vector<size_t> batch_shape = ag_prefix_shape(node->shape, 2);
        size_t batches = ag_product(batch_shape);
        size_t left_matrix = rows * inner;
        size_t right_matrix = inner * cols;
        size_t output_matrix = rows * cols;
        for (size_t batch = 0; batch < batches; ++batch) {
            size_t left_base = ag_broadcast_index(batch, batch_shape, left_batch) * left_matrix;
            size_t right_base = ag_broadcast_index(batch, batch_shape, right_batch) * right_matrix;
            size_t output_base = batch * output_matrix;
            for (size_t row = 0; row < rows; ++row) {
                for (size_t col = 0; col < cols; ++col) {
                    double upstream = node->grad[output_base + row * cols + col];
                    for (size_t k = 0; k < inner; ++k) {
                        ag_accumulate(left, left_base + row * inner + k,
                                      upstream * right->data[right_base + k * cols + col], name, line);
                        ag_accumulate(right, right_base + k * cols + col,
                                      upstream * left->data[left_base + row * inner + k], name, line);
                    }
                }
            }
        }
        return;
    }
    if (node->op == TensorOp::TRANSPOSE) {
        if (node->op_indices.size() != 2) {
            throw JitThrow{std::string(name) + "(): corrupted transpose metadata", line};
        }
        size_t first = node->op_indices[0], second = node->op_indices[1];
        for (size_t i = 0; i < node->grad.size(); ++i) {
            size_t input_index = ag_transposed_input_index(i, node->shape, left->shape,
                                                           first, second);
            ag_accumulate(left, input_index, node->grad[i], name, line);
        }
        return;
    }
    if (node->op == TensorOp::RELU || node->op == TensorOp::TANH
        || node->op == TensorOp::SIGMOID) {
        for (size_t i = 0; i < node->grad.size(); ++i) {
            double derivative = 1.0;
            if (node->op == TensorOp::RELU) derivative = left->data[i] > 0.0 ? 1.0 : 0.0;
            else if (node->op == TensorOp::TANH) derivative = 1.0 - node->data[i] * node->data[i];
            else derivative = node->data[i] * (1.0 - node->data[i]);
            ag_accumulate(left, i, node->grad[i] * derivative, name, line);
        }
        return;
    }
    if (node->op == TensorOp::GELU) {
        constexpr double inv_sqrt_two = 0.707106781186547524400844362104849039;
        constexpr double inv_sqrt_two_pi = 0.398942280401432677939946059934381868;
        for (size_t i = 0; i < node->grad.size(); ++i) {
            double x = left->data[i];
            double density_term = std::abs(x) < 40.0
                                ? x * std::exp(-0.5 * x * x) * inv_sqrt_two_pi
                                : 0.0;
            double derivative = 0.5 * (1.0 + std::erf(x * inv_sqrt_two))
                              + density_term;
            ag_accumulate(left, i, node->grad[i] * derivative, name, line);
        }
        return;
    }
    if (node->op == TensorOp::LAYER_NORM) {
        if (node->op_indices.size() != 2 || node->parents.empty()) {
            throw JitThrow{std::string(name) + "(): corrupted layer_norm metadata", line};
        }
        bool has_weight = node->op_indices[0] != 0;
        bool has_bias = node->op_indices[1] != 0;
        size_t parent_index = 1;
        GCTensor* weight = has_weight ? node->parents[parent_index++] : nullptr;
        GCTensor* bias = has_bias ? node->parents[parent_index++] : nullptr;
        if (parent_index != node->parents.size()) {
            throw JitThrow{std::string(name) + "(): corrupted layer_norm graph", line};
        }
        size_t features = left->shape.back();
        size_t outer = left->data.size() / features;
        for (size_t row = 0; row < outer; ++row) {
            size_t base = row * features;
            AgLayerNormStats stats = ag_layer_norm_stats(
                left, base, features, node->op_scalar);
            auto dxhat_at = [&](size_t col) -> long double {
                long double gamma = weight ? (long double)weight->data[col] : 1.0L;
                return (long double)node->grad[base + col] * gamma;
            };
            long double dxhat_anchor = dxhat_at(0);
            long double delta_total = 0.0L;
            long double delta_compensation = 0.0L;
            for (size_t col = 0; col < features; ++col) {
                double normalized = ag_layer_norm_normalized(left, base + col, stats);
                double upstream = node->grad[base + col];
                long double delta = dxhat_at(col) - dxhat_anchor;
                long double corrected = delta - delta_compensation;
                long double next = delta_total + corrected;
                delta_compensation = (next - delta_total) - corrected;
                delta_total = next;
                if (weight) ag_accumulate(weight, col, upstream * normalized, name, line);
                if (bias) ag_accumulate(bias, col, upstream, name, line);
            }
            long double mean_delta = delta_total / (long double)features;
            long double projection_total = 0.0L;
            long double projection_compensation = 0.0L;
            for (size_t col = 0; col < features; ++col) {
                double normalized = ag_layer_norm_normalized(left, base + col, stats);
                long double centered_dxhat = (dxhat_at(col) - dxhat_anchor) - mean_delta;
                long double term = centered_dxhat * (long double)normalized;
                long double corrected = term - projection_compensation;
                long double next = projection_total + corrected;
                projection_compensation = (next - projection_total) - corrected;
                projection_total = next;
            }
            long double projection_mean = projection_total / (long double)features;
            for (size_t col = 0; col < features; ++col) {
                double normalized = ag_layer_norm_normalized(left, base + col, stats);
                long double centered_dxhat = (dxhat_at(col) - dxhat_anchor) - mean_delta;
                long double raw_gradient = (long double)stats.inverse_std
                    * (centered_dxhat - (long double)normalized * projection_mean);
                double gradient = (double)raw_gradient;
                ag_accumulate(left, base + col, gradient, name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::EMBEDDING) {
        if (node->parents.size() != 1) {
            throw JitThrow{std::string(name) + "(): corrupted embedding graph", line};
        }
        size_t dimensions = left->shape[1];
        if (node->op_indices.size() * dimensions != node->grad.size()) {
            throw JitThrow{std::string(name) + "(): corrupted embedding metadata", line};
        }
        for (size_t token = 0; token < node->op_indices.size(); ++token) {
            size_t weight_base = node->op_indices[token] * dimensions;
            size_t output_base = token * dimensions;
            for (size_t d = 0; d < dimensions; ++d) {
                ag_accumulate(left, weight_base + d, node->grad[output_base + d], name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::CAUSAL_ATTENTION) {
        if (node->parents.size() != 3) {
            throw JitThrow{std::string(name) + "(): corrupted causal_attention graph", line};
        }
        GCTensor* query = node->parents[0];
        GCTensor* key = node->parents[1];
        GCTensor* value = node->parents[2];
        size_t sequence = query->shape[query->shape.size() - 2];
        size_t dimensions = query->shape.back();
        size_t value_dimensions = value->shape.back();
        std::vector<size_t> prefix = ag_prefix_shape(query->shape, 2);
        size_t batches = ag_product(prefix);
        size_t q_matrix = sequence * dimensions;
        size_t v_matrix = sequence * value_dimensions;
        AgTemporaryBytes attention_scratch(
            sequence * sizeof(double) * 2,
            "autograd_backward", line);
        std::vector<double> probabilities(sequence, 0.0);
        std::vector<double> probability_grad(sequence, 0.0);
        for (size_t batch = 0; batch < batches; ++batch) {
            size_t q_base = batch * q_matrix;
            size_t v_base = batch * v_matrix;
            for (size_t row = 0; row < sequence; ++row) {
                double maximum = -std::numeric_limits<double>::infinity();
                for (size_t col = 0; col <= row; ++col) {
                    double score = 0.0;
                    for (size_t d = 0; d < dimensions; ++d) {
                        score += query->data[q_base + row * dimensions + d]
                               * key->data[q_base + col * dimensions + d];
                    }
                    score *= node->op_scalar;
                    probabilities[col] = score;
                    maximum = std::max(maximum, score);
                }
                double total = 0.0;
                for (size_t col = 0; col <= row; ++col) {
                    probabilities[col] = std::exp(probabilities[col] - maximum);
                    total += probabilities[col];
                }
                for (size_t col = 0; col <= row; ++col) probabilities[col] /= total;
                double softmax_dot = 0.0;
                for (size_t col = 0; col <= row; ++col) {
                    double gradient = 0.0;
                    for (size_t d = 0; d < value_dimensions; ++d) {
                        double upstream = node->grad[v_base + row * value_dimensions + d];
                        gradient += upstream * value->data[v_base + col * value_dimensions + d];
                        ag_accumulate(value, v_base + col * value_dimensions + d,
                                      probabilities[col] * upstream, name, line);
                    }
                    probability_grad[col] = gradient;
                    softmax_dot += probabilities[col] * gradient;
                }
                for (size_t col = 0; col <= row; ++col) {
                    double score_grad = probabilities[col]
                                      * (probability_grad[col] - softmax_dot)
                                      * node->op_scalar;
                    for (size_t d = 0; d < dimensions; ++d) {
                        ag_accumulate(query, q_base + row * dimensions + d,
                                      score_grad * key->data[q_base + col * dimensions + d],
                                      name, line);
                        ag_accumulate(key, q_base + col * dimensions + d,
                                      score_grad * query->data[q_base + row * dimensions + d],
                                      name, line);
                    }
                }
            }
        }
        return;
    }
    if (node->op == TensorOp::SOFTMAX) {
        size_t classes = node->shape.back();
        size_t outer = node->data.size() / classes;
        for (size_t row = 0; row < outer; ++row) {
            size_t base = row * classes;
            double dot = 0.0;
            for (size_t col = 0; col < classes; ++col) dot += node->grad[base + col] * node->data[base + col];
            for (size_t col = 0; col < classes; ++col) {
                ag_accumulate(left, base + col,
                              node->data[base + col] * (node->grad[base + col] - dot), name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::SUM || node->op == TensorOp::MEAN) {
        double scale = node->grad[0];
        if (node->op == TensorOp::MEAN) scale /= (double)left->data.size();
        for (size_t i = 0; i < left->data.size(); ++i) ag_accumulate(left, i, scale, name, line);
        return;
    }
    if (node->op == TensorOp::MSE || node->op == TensorOp::BCE
        || node->op == TensorOp::BCE_LOGITS) {
        std::vector<size_t> shape = ag_broadcast_shape(name, left->shape, right->shape, line);
        double scale = node->grad[0] / (double)ag_numel(name, shape, line);
        constexpr double epsilon = 1e-12;
        for (size_t i = 0; i < ag_numel(name, shape, line); ++i) {
            size_t li = ag_broadcast_index(i, shape, left->shape);
            size_t ri = ag_broadcast_index(i, shape, right->shape);
            double x = left->data[li], y = right->data[ri];
            if (node->op == TensorOp::MSE) {
                ag_accumulate(left, li, scale * 2.0 * (x - y), name, line);
                ag_accumulate(right, ri, scale * 2.0 * (y - x), name, line);
            } else if (node->op == TensorOp::BCE) {
                double probability_scale = 1.0 - 2.0 * epsilon;
                double p = epsilon + probability_scale * x;
                ag_accumulate(left, li,
                              scale * probability_scale * (p - y) / (p * (1.0 - p)), name, line);
                ag_accumulate(right, ri, scale * (std::log(1.0 - p) - std::log(p)), name, line);
            } else {
                double probability = x >= 0.0 ? 1.0 / (1.0 + std::exp(-x))
                                               : std::exp(x) / (1.0 + std::exp(x));
                ag_accumulate(left, li, scale * (probability - y), name, line);
                ag_accumulate(right, ri, scale * -x, name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::CROSS_ENTROPY) {
        size_t classes = left->shape.back();
        size_t outer = left->data.size() / classes;
        double scale = node->grad[0] / (double)outer;
        for (size_t row = 0; row < outer; ++row) {
            size_t base = row * classes;
            double maximum = left->data[base];
            for (size_t col = 1; col < classes; ++col) maximum = std::max(maximum, left->data[base + col]);
            double total = 0.0;
            for (size_t col = 0; col < classes; ++col) total += std::exp(left->data[base + col] - maximum);
            double log_denom = maximum + std::log(total);
            double target_total = 0.0;
            for (size_t col = 0; col < classes; ++col) target_total += right->data[base + col];
            for (size_t col = 0; col < classes; ++col) {
                double probability = std::exp(left->data[base + col] - maximum) / total;
                ag_accumulate(left, base + col,
                              scale * (target_total * probability - right->data[base + col]), name, line);
                ag_accumulate(right, base + col,
                              scale * -(left->data[base + col] - log_denom), name, line);
            }
        }
        return;
    }
    if (node->op == TensorOp::CROSS_ENTROPY_IDS) {
        size_t classes = left->shape.back();
        size_t outer = left->data.size() / classes;
        if (node->op_indices.size() != outer) {
            throw JitThrow{std::string(name) + "(): corrupted cross_entropy_ids metadata", line};
        }
        double scale = node->grad[0] / (double)outer;
        for (size_t row = 0; row < outer; ++row) {
            size_t base = row * classes;
            double maximum = left->data[base];
            for (size_t col = 1; col < classes; ++col) {
                maximum = std::max(maximum, left->data[base + col]);
            }
            double total = 0.0;
            for (size_t col = 0; col < classes; ++col) {
                total += std::exp(left->data[base + col] - maximum);
            }
            for (size_t col = 0; col < classes; ++col) {
                double probability = std::exp(left->data[base + col] - maximum) / total;
                double expected = col == node->op_indices[row] ? 1.0 : 0.0;
                ag_accumulate(left, base + col,
                              scale * (probability - expected), name, line);
            }
        }
        return;
    }
}

using AgCudaGradientMap = std::unordered_map<
    GCTensor*, std::shared_ptr<AgCudaAllocation>>;

inline std::shared_ptr<AgCudaAllocation> ag_cuda_gradient_copy(
    const char* name, const std::shared_ptr<AgCudaAllocation>& source, int line) {
    auto result = ag_cuda_allocate(name, source->elements, line);
    ag_cuda_copy_all(name, *source, *result, line);
    return result;
}

inline void ag_cuda_accumulate_gradient(
    AgCudaGradientMap& gradients, GCTensor* tensor,
    const std::shared_ptr<AgCudaAllocation>& contribution,
    const char* name, int line) {
    if (!tensor->requires_grad) return;
    if (!contribution || contribution->elements != tensor->data.size()) {
        throw JitThrow{std::string(name) + "(): corrupted CUDA gradient contribution", line};
    }
    auto found = gradients.find(tensor);
    if (found == gradients.end()) {
        // Never share one upstream allocation between two parents: later
        // in-place accumulation for one branch must not mutate the other.
        gradients.emplace(tensor, ag_cuda_gradient_copy(name, contribution, line));
        return;
    }
    if (found->second->elements != contribution->elements
        || !SuraCudaDriver::instance().add_f32(
            found->second->handle, contribution->handle,
            found->second->handle, contribution->elements)) {
        ag_cuda_fail(name, line);
    }
}

inline Value ag_backward_cuda(const Value* a, int n, int l,
                              float declared_gradient_scale,
                              bool use_implicit_loss_scale) {
    const char* name = "autograd_backward";
    if (!std::isfinite(declared_gradient_scale)
        || declared_gradient_scale <= 0.0f) {
        throw JitThrow{"autograd_backward(): invalid internal CUDA gradient scale", l};
    }
    GCTensor* root = ag_need_tensor(name, a[0], 0, l);
    ag_cuda_require_tensor(name, root, l);
    if (!root->requires_grad) {
        throw JitThrow{"autograd_backward(): tensor does not require gradients", l};
    }

    bool retain_graph = false;
    if (n >= 3) {
        if (!a[2].is_bool()) {
            throw JitThrow{"autograd_backward(): retain_graph must be a bool", l};
        }
        retain_graph = a[2].as_bool();
    }

    std::vector<GCTensor*> topo = ag_topological_graph(name, root, l);
    for (GCTensor* tensor : topo) {
        if (!ag_is_cuda(tensor)) {
            throw JitThrow{
                "autograd_backward(): mixed CPU/CUDA graphs are not supported yet; "
                "create leaves directly on CUDA", l};
        }
        ag_cuda_require_tensor(name, tensor, l);
        if (tensor->cuda_data->device_index != root->cuda_data->device_index) {
            throw JitThrow{"autograd_backward(): graph spans multiple CUDA devices", l};
        }
        if (tensor->op != TensorOp::LEAF) ag_validate_saved_versions(name, tensor, l);
        if (tensor->op == TensorOp::LEAF) {
            if (!tensor->grad.empty()) {
                throw JitThrow{
                    "autograd_backward(): CUDA leaf has a legacy host gradient; call zero_grad() "
                    "after recreating the parameter on CUDA", l};
            }
            if (!std::isfinite(tensor->cuda_grad_scale)
                || tensor->cuda_grad_scale < 0.0f
                || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
                throw JitThrow{
                    "autograd_backward(): corrupted persistent CUDA gradient scale", l};
            }
            if (tensor->cuda_grad) {
                ag_cuda_require_f32_allocation(
                    "autograd_backward", tensor, tensor->cuda_grad,
                    "persistent gradient", l);
            }
            if (tensor->requires_grad && tensor->cuda_grad
                && tensor->cuda_grad_scale != 0.0f
                && tensor->cuda_grad_scale != declared_gradient_scale) {
                throw JitThrow{
                    "autograd_backward(): cannot accumulate CUDA gradients with different "
                    "loss scales; call unscale_gradients() or zero_grad() first", l};
            }
        }
    }

    SuraCudaDriver& driver = SuraCudaDriver::instance();
    AgCudaGradientMap gradients;
    // Device-only backward workspaces must outlive every queued consumer and
    // are released only after the final context synchronization below.
    std::vector<std::shared_ptr<AgCudaAllocation>> backward_temporaries;
    std::shared_ptr<AgCudaAllocation> root_gradient;
    bool root_seed_known = false;
    float root_seed = 0.0f;

    if (use_implicit_loss_scale) {
        if (root->data.size() != 1) {
            throw JitThrow{
                "autograd_backward_scaled(): loss must be a scalar tensor", l};
        }
        root_seed_known = true;
        root_seed = declared_gradient_scale;
        root_gradient = ag_cuda_allocate(name, 1, l);
        if (!driver.fill_f32(root_gradient->handle, root_seed, 1)) {
            ag_cuda_fail(name, l);
        }
    } else if (n >= 2 && !a[1].is_nil()) {
        Value gradient_value = ag_coerce_tensor(name, a[1], l, TensorDType::FLOAT32);
        GCNativeRoot gradient_root(gradient_value.as_obj());
        GCTensor* gradient = gradient_value.as_tensor();
        if (gradient->shape != root->shape) {
            throw JitThrow{"autograd_backward(): supplied gradient shape does not match output", l};
        }
        if (ag_is_cuda(gradient)) {
            ag_cuda_require_same_device(name, root, gradient, l);
            ag_cuda_require_float32(name, gradient, l);
            root_gradient = ag_cuda_gradient_copy(name, gradient->cuda_data, l);
        } else {
            if (!gradient->data.host_readable()) {
                throw JitThrow{"autograd_backward(): supplied CPU gradient is not host-readable", l};
            }
            std::vector<float> seed(gradient->data.size());
            for (size_t i = 0; i < seed.size(); ++i) {
                const double value = gradient->data[i];
                if (!std::isfinite(value)) {
                    throw JitThrow{"autograd_backward(): supplied gradient is not finite", l};
                }
                // Check in the source precision before conversion. Some
                // Windows libstdc++/optimizer combinations can classify an
                // excess-precision temporary after a float cast, while the
                // eventual stored f32 value is infinity.
                if (std::abs(value) > (double)std::numeric_limits<float>::max()) {
                    throw JitThrow{
                        "autograd_backward(): supplied gradient overflows CUDA float32", l};
                }
                const float converted = (float)value;
                if (!std::isfinite(converted)) {
                    throw JitThrow{
                        "autograd_backward(): supplied gradient overflows CUDA float32", l};
                }
                seed[i] = converted;
            }
            if (seed.size() == 1) {
                root_seed_known = true;
                root_seed = seed[0];
            }
            root_gradient = ag_cuda_allocate(name, seed.size(), l);
            const bool seeded = seed.size() == 1
                ? driver.fill_f32(root_gradient->handle, seed[0], 1)
                : driver.upload_f32(root_gradient->handle, seed);
            if (!seeded) ag_cuda_fail(name, l);
        }
    } else {
        if (root->data.size() != 1) {
            throw JitThrow{"autograd_backward(): non-scalar outputs require an explicit gradient", l};
        }
        root_seed_known = true;
        root_seed = 1.0f;
        root_gradient = ag_cuda_allocate(name, 1, l);
        if (!driver.fill_f32(root_gradient->handle, 1.0f, 1)) ag_cuda_fail(name, l);
    }
    gradients.emplace(root, root_gradient);

    for (auto iterator = topo.rbegin(); iterator != topo.rend(); ++iterator) {
        GCTensor* node = *iterator;
        auto gradient_entry = gradients.find(node);
        if (node->op == TensorOp::LEAF || gradient_entry == gradients.end()) continue;
        const auto& upstream = gradient_entry->second;
        GCTensor* left = node->parents.empty() ? nullptr : node->parents[0];
        GCTensor* right = node->parents.size() < 2 ? nullptr : node->parents[1];

        if (node->op == TensorOp::RESHAPE || node->op == TensorOp::CAST
            || node->op == TensorOp::DEVICE_COPY) {
            if (!left || left->data.size() != upstream->elements) {
                throw JitThrow{"autograd_backward(): corrupted CUDA identity graph", l};
            }
            ag_cuda_accumulate_gradient(gradients, left, upstream, name, l);
            continue;
        }
        if (node->op == TensorOp::TRANSPOSE) {
            if (!left || node->op_indices.size() != 2
                || left->shape.size() < 2
                || left->shape.size() != node->shape.size()
                || left->data.size() != upstream->elements) {
                throw JitThrow{"autograd_backward(): corrupted CUDA transpose graph", l};
            }
            const size_t first = node->op_indices[0];
            const size_t second = node->op_indices[1];
            std::vector<size_t> expected = left->shape;
            if (first >= expected.size() || second >= expected.size()
                || first == second) {
                throw JitThrow{"autograd_backward(): corrupted CUDA transpose axes", l};
            }
            std::swap(expected[first], expected[second]);
            if (expected != node->shape) {
                throw JitThrow{"autograd_backward(): corrupted CUDA transpose shape", l};
            }
            auto contribution = ag_cuda_allocate(name, left->data.size(), l);
            if (!driver.transpose_f32(
                    upstream->handle, contribution->handle,
                    upstream->elements, node->shape,
                    (uint32_t)first, (uint32_t)second)) {
                ag_cuda_fail(name, l);
            }
            ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            continue;
        }
        if (node->op == TensorOp::NEG) {
            auto contribution = ag_cuda_allocate(name, upstream->elements, l);
            if (!driver.negate_f32(upstream->handle, contribution->handle,
                                   upstream->elements)) ag_cuda_fail(name, l);
            ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            continue;
        }
        if (node->op == TensorOp::RELU) {
            if (!left || left->data.size() != upstream->elements) {
                throw JitThrow{"autograd_backward(): corrupted CUDA relu graph", l};
            }
            auto contribution = ag_cuda_allocate(name, upstream->elements, l);
            if (!driver.relu_backward_f32(left->cuda_data->handle, upstream->handle,
                                          contribution->handle, upstream->elements)) {
                ag_cuda_fail(name, l);
            }
            ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            continue;
        }
        if (node->op == TensorOp::GELU) {
            if (!left || left->data.size() != upstream->elements) {
                throw JitThrow{"autograd_backward(): corrupted CUDA gelu graph", l};
            }
            auto contribution = ag_cuda_allocate(name, upstream->elements, l);
            if (!driver.gelu_backward_f32(
                    left->cuda_data->handle, upstream->handle,
                    contribution->handle, upstream->elements)) {
                ag_cuda_fail(name, l);
            }
            ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            continue;
        }
        if (node->op == TensorOp::LAYER_NORM) {
            if (!left || node->op_indices.size() != 2
                || left->shape.empty() || node->shape != left->shape
                || upstream->elements != node->data.size()
                || left->data.size() != node->data.size()) {
                throw JitThrow{"autograd_backward(): corrupted CUDA layer_norm graph", l};
            }
            const bool has_weight = node->op_indices[0] != 0;
            const bool has_bias = node->op_indices[1] != 0;
            const size_t expected_parents = 1 + (has_weight ? 1 : 0)
                                               + (has_bias ? 1 : 0);
            if (node->parents.size() != expected_parents) {
                throw JitThrow{"autograd_backward(): corrupted CUDA layer_norm parents", l};
            }
            size_t parent_index = 1;
            GCTensor* norm_weight = has_weight ? node->parents[parent_index++] : nullptr;
            GCTensor* norm_bias = has_bias ? node->parents[parent_index++] : nullptr;
            const size_t features = left->shape.back();
            if (features == 0 || left->data.size() % features != 0
                || (norm_weight
                    && norm_weight->shape != std::vector<size_t>{features})
                || (norm_bias
                    && norm_bias->shape != std::vector<size_t>{features})
                || !std::isfinite(node->op_scalar)
                || node->op_scalar < 1e-12 || node->op_scalar > 0.1) {
                throw JitThrow{"autograd_backward(): corrupted CUDA layer_norm metadata", l};
            }
            const size_t rows = left->data.size() / features;
            if (rows > (size_t)std::numeric_limits<uint32_t>::max()
                || features > (size_t)std::numeric_limits<uint32_t>::max()
                || !node->cuda_layer_norm_mean || !node->cuda_layer_norm_rstd
                || node->cuda_layer_norm_mean->elements != rows
                || node->cuda_layer_norm_rstd->elements != rows
                || node->cuda_layer_norm_mean->device_index
                       != node->cuda_data->device_index
                || node->cuda_layer_norm_rstd->device_index
                       != node->cuda_data->device_index
                || node->cuda_layer_norm_mean->handle
                       == node->cuda_layer_norm_rstd->handle) {
                throw JitThrow{"autograd_backward(): corrupted CUDA layer_norm saved state", l};
            }
            if (left->requires_grad) {
                auto contribution = ag_cuda_allocate(name, left->data.size(), l);
                if (!driver.layer_norm_backward_f32(
                        left->cuda_data->handle,
                        norm_weight ? norm_weight->cuda_data->handle
                                    : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        upstream->handle,
                        node->cuda_layer_norm_mean->handle,
                        node->cuda_layer_norm_rstd->handle,
                        contribution->handle,
                        (uint32_t)rows, (uint32_t)features,
                        (float)node->op_scalar, norm_weight != nullptr)) {
                    ag_cuda_fail(name, l);
                }
                ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            }
            const bool need_weight = norm_weight && norm_weight->requires_grad;
            const bool need_bias = norm_bias && norm_bias->requires_grad;
            if (need_weight || need_bias) {
                std::shared_ptr<AgCudaAllocation> weight_contribution;
                std::shared_ptr<AgCudaAllocation> bias_contribution;
                if (need_weight) {
                    weight_contribution = ag_cuda_allocate(name, features, l);
                }
                if (need_bias) {
                    bias_contribution = ag_cuda_allocate(name, features, l);
                }
                if (!driver.layer_norm_parameter_backward_f32(
                        left->cuda_data->handle, upstream->handle,
                        node->cuda_layer_norm_mean->handle,
                        node->cuda_layer_norm_rstd->handle,
                        weight_contribution ? weight_contribution->handle
                                            : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        bias_contribution ? bias_contribution->handle
                                          : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        (uint32_t)rows, (uint32_t)features,
                        (float)node->op_scalar, need_weight, need_bias)) {
                    ag_cuda_fail(name, l);
                }
                if (need_weight) {
                    ag_cuda_accumulate_gradient(
                        gradients, norm_weight, weight_contribution, name, l);
                }
                if (need_bias) {
                    ag_cuda_accumulate_gradient(
                        gradients, norm_bias, bias_contribution, name, l);
                }
            }
            continue;
        }
        if (node->op == TensorOp::EMBEDDING) {
            if (!left || node->parents.size() != 1
                || left->shape.size() != 2 || node->shape.empty()
                || !node->op_indices.empty()
                || upstream->elements != node->data.size()) {
                throw JitThrow{"autograd_backward(): corrupted CUDA embedding graph", l};
            }
            const size_t vocabulary = left->shape[0];
            const size_t dimensions = left->shape[1];
            if (dimensions == 0 || node->shape.back() != dimensions
                || node->data.size() % dimensions != 0) {
                throw JitThrow{"autograd_backward(): corrupted CUDA embedding shape", l};
            }
            const size_t tokens = node->data.size() / dimensions;
            if (!node->cuda_embedding_ids
                || node->cuda_embedding_ids->elements != tokens
                || node->cuda_embedding_ids->device_index
                       != node->cuda_data->device_index
                || vocabulary > (size_t)std::numeric_limits<uint32_t>::max()
                || tokens > (size_t)std::numeric_limits<uint32_t>::max()
                || dimensions > (size_t)std::numeric_limits<uint32_t>::max()) {
                throw JitThrow{"autograd_backward(): corrupted CUDA embedding saved ids", l};
            }
            if (left->requires_grad) {
                auto contribution = ag_cuda_allocate(name, left->data.size(), l);
                if (!driver.embedding_backward_f32(
                        node->cuda_embedding_ids->handle, upstream->handle,
                        contribution->handle, (uint32_t)vocabulary,
                        (uint32_t)tokens, (uint32_t)dimensions)) {
                    ag_cuda_fail(name, l);
                }
                ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            }
            continue;
        }
        if (node->op == TensorOp::CAUSAL_ATTENTION) {
            if (!left || !right || node->parents.size() != 3
                || left->shape.size() < 2
                || left->shape.size() != right->shape.size()
                || left->shape.size() != node->parents[2]->shape.size()
                || upstream->elements != node->data.size()
                || !node->op_indices.empty()) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA causal_attention graph", l};
            }
            GCTensor* query = left;
            GCTensor* key = right;
            GCTensor* attention_value = node->parents[2];
            const size_t rank = query->shape.size();
            const size_t sequence = query->shape[rank - 2];
            const size_t dimensions = query->shape.back();
            const size_t value_dimensions = attention_value->shape.back();
            const std::vector<size_t> prefix = ag_prefix_shape(query->shape, 2);
            const size_t batches = ag_product(prefix);
            std::vector<size_t> expected_shape = prefix;
            expected_shape.push_back(sequence);
            expected_shape.push_back(value_dimensions);
            const uint64_t total_rows = (uint64_t)batches * sequence;
            if (ag_prefix_shape(key->shape, 2) != prefix
                || ag_prefix_shape(attention_value->shape, 2) != prefix
                || key->shape[rank - 2] != sequence
                || attention_value->shape[rank - 2] != sequence
                || key->shape.back() != dimensions
                || node->shape != expected_shape
                || key->data.size() != query->data.size()
                || node->data.size() != attention_value->data.size()
                || batches > (size_t)std::numeric_limits<uint32_t>::max()
                || sequence > (size_t)std::numeric_limits<uint32_t>::max()
                || dimensions > (size_t)std::numeric_limits<uint32_t>::max()
                || value_dimensions > (size_t)std::numeric_limits<uint32_t>::max()
                || total_rows > (uint64_t)std::numeric_limits<uint32_t>::max()
                || !std::isfinite(node->op_scalar)
                || node->op_scalar < 1e-12 || node->op_scalar > 1e12
                || !node->cuda_attention_max
                || !node->cuda_attention_inv_sum
                || node->cuda_attention_max->elements != (size_t)total_rows
                || node->cuda_attention_inv_sum->elements != (size_t)total_rows
                || node->cuda_attention_max->device_index
                       != node->cuda_data->device_index
                || node->cuda_attention_inv_sum->device_index
                       != node->cuda_data->device_index) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA causal_attention saved state", l};
            }
            const bool need_query = query->requires_grad;
            const bool need_key = key->requires_grad;
            const bool need_value = attention_value->requires_grad;
            uint64_t total_pairs = 0;
            uint64_t workspace_bytes = 0;
            const bool workspace_sizes_safe =
                ag_cuda_attention_workspace_requirements(
                    batches, sequence, total_pairs, workspace_bytes);
            const bool use_parallel = node->cuda_attention_parallel_plan;
            const bool use_fused = node->cuda_attention_fused_plan;
            if (use_parallel && use_fused) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA attention dispatch plan", l};
            }
            if (use_fused
                && sequence < AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA fused attention plan", l};
            }
            if (use_parallel
                && (!workspace_sizes_safe
                    || sequence < AG_CUDA_ATTENTION_PARALLEL_MIN_SEQUENCE
                    || total_pairs
                        > (uint64_t)std::numeric_limits<uint32_t>::max()
                    || workspace_bytes
                        > (uint64_t)AG_HARD_CUDA_ATTENTION_WORKSPACE_BYTES)) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA attention dispatch plan", l};
            }
            std::shared_ptr<AgCudaAllocation> query_contribution;
            std::shared_ptr<AgCudaAllocation> key_contribution;
            std::shared_ptr<AgCudaAllocation> value_contribution;
            if (need_query) {
                query_contribution = ag_cuda_allocate(name, query->data.size(), l);
            }
            if (need_key) {
                key_contribution = ag_cuda_allocate(name, key->data.size(), l);
            }
            if (need_value) {
                value_contribution = ag_cuda_allocate(
                    name, attention_value->data.size(), l);
            }
            if (use_fused) {
                if (!driver.causal_attention_fused_backward_f32(
                        query->cuda_data->handle, key->cuda_data->handle,
                        attention_value->cuda_data->handle,
                        node->cuda_data->handle, upstream->handle,
                        node->cuda_attention_max->handle,
                        node->cuda_attention_inv_sum->handle,
                        query_contribution ? query_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        key_contribution ? key_contribution->handle
                                         : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        value_contribution ? value_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        (uint32_t)batches, (uint32_t)sequence,
                        (uint32_t)dimensions, (uint32_t)value_dimensions,
                        (float)node->op_scalar,
                        need_query, need_key, need_value)) {
                    ag_cuda_fail(name, l);
                }
            } else if (use_parallel) {
                auto probabilities = ag_cuda_allocate(name, (size_t)total_pairs, l);
                backward_temporaries.push_back(probabilities);
                if (!driver.causal_attention_parallel_backward_f32(
                        query->cuda_data->handle, key->cuda_data->handle,
                        attention_value->cuda_data->handle,
                        node->cuda_data->handle, upstream->handle,
                        probabilities->handle,
                        query_contribution ? query_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        key_contribution ? key_contribution->handle
                                         : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        value_contribution ? value_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        (uint32_t)batches, (uint32_t)sequence,
                        (uint32_t)dimensions, (uint32_t)value_dimensions,
                        (float)node->op_scalar,
                        need_query, need_key, need_value)) {
                    ag_cuda_fail(name, l);
                }
            } else {
                if (!driver.causal_attention_backward_f32(
                        query->cuda_data->handle, key->cuda_data->handle,
                        attention_value->cuda_data->handle,
                        node->cuda_data->handle, upstream->handle,
                        node->cuda_attention_max->handle,
                        node->cuda_attention_inv_sum->handle,
                        query_contribution ? query_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        key_contribution ? key_contribution->handle
                                         : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        value_contribution ? value_contribution->handle
                                           : SuraCudaDriver::INVALID_DEVICE_HANDLE,
                        (uint32_t)batches, (uint32_t)sequence,
                        (uint32_t)dimensions, (uint32_t)value_dimensions,
                        (float)node->op_scalar,
                        need_query, need_key, need_value)) {
                    ag_cuda_fail(name, l);
                }
            }
            if (need_query) {
                ag_cuda_accumulate_gradient(
                    gradients, query, query_contribution, name, l);
            }
            if (need_key) {
                ag_cuda_accumulate_gradient(
                    gradients, key, key_contribution, name, l);
            }
            if (need_value) {
                ag_cuda_accumulate_gradient(
                    gradients, attention_value, value_contribution, name, l);
            }
            continue;
        }
        if (node->op == TensorOp::CROSS_ENTROPY_IDS) {
            if (!left || node->parents.size() != 1 || left->shape.empty()
                || left->shape.back() < 2 || node->data.size() != 1
                || upstream->elements != 1 || !node->op_indices.empty()) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA cross_entropy_ids graph", l};
            }
            const size_t classes = left->shape.back();
            if (left->data.size() % classes != 0) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA cross_entropy_ids shape", l};
            }
            const size_t rows = left->data.size() / classes;
            if (rows > (size_t)std::numeric_limits<uint32_t>::max()
                || classes > (size_t)std::numeric_limits<uint32_t>::max()
                || !node->cuda_cross_entropy_ids
                || !node->cuda_cross_entropy_max
                || !node->cuda_cross_entropy_inv_sum
                || node->cuda_cross_entropy_ids->elements != rows
                || node->cuda_cross_entropy_max->elements != rows
                || node->cuda_cross_entropy_inv_sum->elements != rows
                || node->cuda_cross_entropy_ids->device_index
                       != node->cuda_data->device_index
                || node->cuda_cross_entropy_max->device_index
                       != node->cuda_data->device_index
                || node->cuda_cross_entropy_inv_sum->device_index
                       != node->cuda_data->device_index) {
                throw JitThrow{
                    "autograd_backward(): corrupted CUDA cross_entropy_ids saved state", l};
            }
            if (left->requires_grad) {
                auto contribution = ag_cuda_allocate(name, left->data.size(), l);
                if (!driver.cross_entropy_ids_backward_f32(
                        left->cuda_data->handle,
                        node->cuda_cross_entropy_ids->handle,
                        upstream->handle,
                        node->cuda_cross_entropy_max->handle,
                        node->cuda_cross_entropy_inv_sum->handle,
                        contribution->handle,
                        (uint32_t)rows, (uint32_t)classes)) {
                    ag_cuda_fail(name, l);
                }
                ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            }
            continue;
        }
        if (node->op == TensorOp::SUM || node->op == TensorOp::MEAN) {
            if (node != root || !root_seed_known || !left) {
                throw JitThrow{
                    "autograd_backward(): CUDA sum/mean backward currently requires the "
                    "reduction to be the scalar loss root", l};
            }
            float scale = root_seed;
            if (node->op == TensorOp::MEAN) scale /= (float)left->data.size();
            auto contribution = ag_cuda_allocate(name, left->data.size(), l);
            if (!driver.fill_f32(contribution->handle, scale, left->data.size())) {
                ag_cuda_fail(name, l);
            }
            ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            continue;
        }
        if (node->op == TensorOp::MATMUL) {
            if (node->parents.size() != 2 || !left || !right
                || left->shape.size() < 2 || right->shape.size() != 2) {
                throw JitThrow{"autograd_backward(): corrupted CUDA matmul graph", l};
            }
            const size_t inner = left->shape.back();
            const size_t cols = right->shape[1];
            std::vector<size_t> expected_shape = left->shape;
            expected_shape.back() = cols;
            if (left->data.size() % inner != 0) {
                throw JitThrow{"autograd_backward(): corrupted CUDA matmul graph", l};
            }
            const size_t flat_rows = left->data.size() / inner;
            const bool typed_operand = left->data.dtype() == TensorDType::FLOAT16
                || left->data.dtype() == TensorDType::BFLOAT16
                || right->data.dtype() == TensorDType::FLOAT16
                || right->data.dtype() == TensorDType::BFLOAT16;
            // Persistent CUDA gradients are f32. A typed forward therefore
            // uses f32 compute for its mixed f32/low-storage backward GEMMs.
            const auto cuda_compute = typed_operand
                ? SuraCudaDriver::MatmulCompute::FLOAT32
                : ag_cuda_matmul_compute(
                    node->cuda_matmul_compute_dtype, "autograd_backward", l);
            if (right->shape[0] != inner || node->shape != expected_shape
                || right->data.size() != inner * cols
                || node->data.size() != flat_rows * cols
                || upstream->elements != node->data.size()
                || flat_rows > (size_t)std::numeric_limits<uint32_t>::max()
                || inner > (size_t)std::numeric_limits<uint32_t>::max()
                || cols > (size_t)std::numeric_limits<uint32_t>::max()) {
                throw JitThrow{"autograd_backward(): corrupted CUDA matmul graph", l};
            }
            if (left->requires_grad) {
                auto contribution = ag_cuda_allocate(name, left->data.size(), l);
                if (!driver.matmul_device_typed(
                        upstream->handle, right->cuda_data->handle,
                        contribution->handle, (uint32_t)flat_rows, (uint32_t)inner,
                        (uint32_t)cols, false, true, cuda_compute)) {
                    ag_cuda_fail(name, l);
                }
                ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
            }
            if (right->requires_grad) {
                auto contribution = ag_cuda_allocate(name, right->data.size(), l);
                if (!driver.matmul_device_typed(
                        left->cuda_data->handle, upstream->handle,
                        contribution->handle, (uint32_t)inner, (uint32_t)cols,
                        (uint32_t)flat_rows, true, false, cuda_compute)) {
                    ag_cuda_fail(name, l);
                }
                ag_cuda_accumulate_gradient(gradients, right, contribution, name, l);
            }
            continue;
        }
        if (node->op == TensorOp::ADD || node->op == TensorOp::SUB
            || node->op == TensorOp::MUL || node->op == TensorOp::DIV) {
            if (node->parents.size() == 1) {
                if (node->op_indices.size() != 1 || !left
                    || left->data.size() != upstream->elements) {
                    throw JitThrow{"autograd_backward(): corrupted CUDA scalar graph", l};
                }
                const bool scalar_on_left = node->op_indices[0] != 0;
                const float scalar = (float)node->op_scalar;
                std::shared_ptr<AgCudaAllocation> contribution;
                if (node->op == TensorOp::ADD
                    || (node->op == TensorOp::SUB && !scalar_on_left)) {
                    contribution = upstream;
                } else {
                    contribution = ag_cuda_allocate(name, upstream->elements, l);
                    if (node->op == TensorOp::SUB) {
                        if (!driver.negate_f32(upstream->handle, contribution->handle,
                                               upstream->elements)) ag_cuda_fail(name, l);
                    } else if (node->op == TensorOp::MUL) {
                        if (!driver.scale_f32(upstream->handle, contribution->handle,
                                              scalar, upstream->elements)) ag_cuda_fail(name, l);
                    } else if (!scalar_on_left) {
                        if (scalar == 0.0f
                            || !driver.scale_f32(upstream->handle, contribution->handle,
                                                 1.0f / scalar, upstream->elements)) {
                            ag_cuda_fail(name, l);
                        }
                    } else {
                        auto squared = ag_cuda_allocate(name, upstream->elements, l);
                        auto ratio = ag_cuda_allocate(name, upstream->elements, l);
                        if (!driver.multiply_f32(left->cuda_data->handle,
                                                 left->cuda_data->handle,
                                                 squared->handle, upstream->elements)
                            || !driver.divide_f32(upstream->handle, squared->handle,
                                                  ratio->handle, upstream->elements)
                            || !driver.scale_f32(ratio->handle, contribution->handle,
                                                 -scalar, upstream->elements)) {
                            ag_cuda_fail(name, l);
                        }
                    }
                }
                ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
                continue;
            }
            const bool column_bias = node->op == TensorOp::ADD
                && node->parents.size() == 2 && left && right
                && left->shape == node->shape && left->shape.size() >= 2
                && right->shape.size() == 1 && right->shape[0] == node->shape.back()
                && upstream->elements == node->data.size()
                && node->shape.back() != 0
                && node->data.size() / node->shape.back()
                       <= (size_t)std::numeric_limits<uint32_t>::max()
                && node->shape.back() <= (size_t)std::numeric_limits<uint32_t>::max();
            if (column_bias) {
                ag_cuda_accumulate_gradient(gradients, left, upstream, name, l);
                if (right->requires_grad) {
                    auto contribution = ag_cuda_allocate(name, right->data.size(), l);
                    if (!driver.bias_gradient_f32(
                            upstream->handle, contribution->handle,
                            (uint32_t)(node->data.size() / node->shape.back()),
                            (uint32_t)node->shape.back())) {
                        ag_cuda_fail(name, l);
                    }
                    ag_cuda_accumulate_gradient(gradients, right, contribution, name, l);
                }
                continue;
            }
            if (node->parents.size() != 2 || !left || !right
                || left->shape != node->shape || right->shape != node->shape) {
                throw JitThrow{
                    "autograd_backward(): CUDA binary backward requires identical shapes", l};
            }
            if (node->op == TensorOp::ADD) {
                ag_cuda_accumulate_gradient(gradients, left, upstream, name, l);
                ag_cuda_accumulate_gradient(gradients, right, upstream, name, l);
            } else if (node->op == TensorOp::SUB) {
                ag_cuda_accumulate_gradient(gradients, left, upstream, name, l);
                auto negative = ag_cuda_allocate(name, upstream->elements, l);
                if (!driver.negate_f32(upstream->handle, negative->handle,
                                       upstream->elements)) ag_cuda_fail(name, l);
                ag_cuda_accumulate_gradient(gradients, right, negative, name, l);
            } else if (node->op == TensorOp::MUL) {
                if (left->requires_grad) {
                    auto contribution = ag_cuda_allocate(name, upstream->elements, l);
                    if (!driver.multiply_f32(upstream->handle, right->cuda_data->handle,
                                             contribution->handle, upstream->elements)) {
                        ag_cuda_fail(name, l);
                    }
                    ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
                }
                if (right->requires_grad) {
                    auto contribution = ag_cuda_allocate(name, upstream->elements, l);
                    if (!driver.multiply_f32(upstream->handle, left->cuda_data->handle,
                                             contribution->handle, upstream->elements)) {
                        ag_cuda_fail(name, l);
                    }
                    ag_cuda_accumulate_gradient(gradients, right, contribution, name, l);
                }
            } else {
                if (left->requires_grad) {
                    auto contribution = ag_cuda_allocate(name, upstream->elements, l);
                    if (!driver.divide_f32(upstream->handle, right->cuda_data->handle,
                                           contribution->handle, upstream->elements)) {
                        ag_cuda_fail(name, l);
                    }
                    ag_cuda_accumulate_gradient(gradients, left, contribution, name, l);
                }
                if (right->requires_grad) {
                    auto squared = ag_cuda_allocate(name, upstream->elements, l);
                    auto ratio = ag_cuda_allocate(name, upstream->elements, l);
                    auto product = ag_cuda_allocate(name, upstream->elements, l);
                    auto negative = ag_cuda_allocate(name, upstream->elements, l);
                    if (!driver.multiply_f32(right->cuda_data->handle,
                                             right->cuda_data->handle,
                                             squared->handle, upstream->elements)
                        || !driver.divide_f32(left->cuda_data->handle, squared->handle,
                                              ratio->handle, upstream->elements)
                        || !driver.multiply_f32(upstream->handle, ratio->handle,
                                                product->handle, upstream->elements)
                        || !driver.negate_f32(product->handle, negative->handle,
                                              upstream->elements)) {
                        ag_cuda_fail(name, l);
                    }
                    ag_cuda_accumulate_gradient(gradients, right, negative, name, l);
                }
            }
            continue;
        }

        throw JitThrow{std::string(name) + "(): CUDA backward does not support this operation yet", l};
    }

    struct PendingLeafGradient {
        GCTensor* tensor = nullptr;
        std::shared_ptr<AgCudaAllocation> gradient;
    };
    std::vector<PendingLeafGradient> pending;
    for (GCTensor* tensor : topo) {
        if (tensor->op != TensorOp::LEAF || !tensor->requires_grad) continue;
        auto found = gradients.find(tensor);
        if (found == gradients.end()) continue;
        auto merged = ag_cuda_allocate(name, tensor->data.size(), l);
        if (tensor->cuda_grad) {
            ag_cuda_require_f32_allocation(
                name, tensor, tensor->cuda_grad,
                "persistent gradient", l);
            ag_cuda_copy_all(name, *tensor->cuda_grad, *merged, l);
            if (!driver.add_f32(merged->handle, found->second->handle,
                                merged->handle, tensor->data.size())) {
                ag_cuda_fail(name, l);
            }
        } else {
            ag_cuda_copy_all(name, *found->second, *merged, l);
        }
        pending.push_back({tensor, std::move(merged)});
    }
    if (!driver.synchronize()) ag_cuda_fail(name, l);
    for (auto& entry : pending) {
        entry.tensor->cuda_grad = std::move(entry.gradient);
        entry.tensor->cuda_grad_scale = declared_gradient_scale;
    }

    if (!retain_graph) {
        for (GCTensor* tensor : topo) {
            if (tensor->op == TensorOp::LEAF) continue;
            ag_release_graph_node_state(tensor);
        }
    }
    return Value::nil();
}

inline Value b_autograd_backward(const Value* a, int n, int l) {
    need_args("autograd_backward", n, 1, 3, l);
    GCTensor* root = ag_need_tensor("autograd_backward", a[0], 0, l);
    if (ag_is_cuda(root)) return ag_backward_cuda(a, n, l, 1.0f, false);
    if (!root->requires_grad) throw JitThrow{"autograd_backward(): tensor does not require gradients", l};
    std::vector<double> seed;
    if (n >= 2 && !a[1].is_nil()) {
        Value gradient_value = ag_coerce_tensor("autograd_backward", a[1], l);
        GCNativeRoot gradient_root(gradient_value.as_obj());
        GCTensor* gradient = gradient_value.as_tensor();
        if (gradient->shape != root->shape) {
            throw JitThrow{"autograd_backward(): supplied gradient shape does not match output", l};
        }
        ag_preflight_bytes(gradient->data.size() * sizeof(double),
                           "autograd_backward", l);
        seed = gradient->data;
    } else {
        if (root->data.size() != 1) {
            throw JitThrow{"autograd_backward(): non-scalar outputs require an explicit gradient", l};
        }
        seed.assign(1, 1.0);
    }
    AgTemporaryBytes seed_memory(seed.size() * sizeof(double), "autograd_backward", l);
    bool retain_graph = false;
    if (n >= 3) {
        if (!a[2].is_bool()) throw JitThrow{"autograd_backward(): retain_graph must be a bool", l};
        retain_graph = a[2].as_bool();
    }
    std::vector<GCTensor*> topo = ag_topological_graph("autograd_backward", root, l);
    for (GCTensor* tensor : topo) {
        if (ag_is_cuda(tensor)) {
            throw JitThrow{
                "autograd_backward(): mixed CPU/CUDA graphs are not supported yet; "
                "create leaves directly on CUDA", l};
        }
    }
    for (GCTensor* tensor : topo) {
        if (tensor->op != TensorOp::LEAF) {
            ag_validate_saved_versions("autograd_backward", tensor, l);
        }
    }
    size_t staged_bytes = 0;
    for (GCTensor* tensor : topo) {
        if (tensor->op != TensorOp::LEAF || !tensor->requires_grad) continue;
        size_t bytes = tensor->data.size() * sizeof(double);
        if (bytes > ag_max_external_bytes() - staged_bytes) {
            throw JitThrow{"autograd_backward(): staged leaf gradients exceed the tensor memory limit", l};
        }
        staged_bytes += bytes;
    }
    AgTemporaryBytes staged_memory(staged_bytes, "autograd_backward", l);
    size_t backward_buffers = 0;
    for (GCTensor* tensor : topo) {
        bool needs_buffer = tensor->op != TensorOp::LEAF
                         || (tensor->requires_grad && tensor->grad.empty());
        if (!needs_buffer) continue;
        size_t bytes = tensor->data.size() * sizeof(double);
        if (bytes > ag_max_external_bytes() - backward_buffers) {
            throw JitThrow{"autograd_backward(): gradient buffers exceed the tensor memory limit", l};
        }
        backward_buffers += bytes;
    }
    ag_preflight_bytes(backward_buffers, "autograd_backward", l);
    AgLeafGradientStage leaf_stage;
    AgLeafGradientScope leaf_scope(&leaf_stage);
    std::vector<GCTensor*> new_leaf_grad_buffers;
    try {
        for (GCTensor* tensor : topo) {
            if (tensor->op != TensorOp::LEAF) ag_clear_grad_buffer(tensor);
        }
        if (root->op == TensorOp::LEAF) {
            for (size_t i = 0; i < seed.size(); ++i) {
                ag_accumulate(root, i, seed[i], "autograd_backward", l);
            }
        } else {
            ag_ensure_grad(root, "autograd_backward", l);
            root->grad = seed;
        }
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            ag_backward_node(*it, "autograd_backward", l);
        }

        // Allocate and validate every destination before committing any leaf
        // value. A failed backward therefore leaves all prior leaf gradients
        // unchanged.
        new_leaf_grad_buffers.reserve(leaf_stage.pending.size());
        for (auto& entry : leaf_stage.pending) {
            GCTensor* tensor = entry.first;
            bool created = tensor->grad.empty();
            ag_ensure_grad(tensor, "autograd_backward", l);
            if (created) new_leaf_grad_buffers.push_back(tensor);
            for (size_t i = 0; i < entry.second.size(); ++i) {
                if (!std::isfinite(tensor->grad[i])
                    || !std::isfinite(tensor->grad[i] + entry.second[i])) {
                    throw JitThrow{"autograd_backward(): accumulated leaf gradient is not finite", l};
                }
            }
        }
        for (auto& entry : leaf_stage.pending) {
            GCTensor* tensor = entry.first;
            for (size_t i = 0; i < entry.second.size(); ++i) {
                tensor->grad[i] += entry.second[i];
            }
        }
    } catch (...) {
        for (GCTensor* tensor : topo) {
            if (tensor->op != TensorOp::LEAF) ag_clear_grad_buffer(tensor);
        }
        for (GCTensor* tensor : new_leaf_grad_buffers) ag_clear_grad_buffer(tensor);
        throw;
    }
    for (GCTensor* tensor : topo) {
        if (tensor->op != TensorOp::LEAF) ag_clear_grad_buffer(tensor);
    }
    if (!retain_graph) {
        for (GCTensor* tensor : topo) {
            if (tensor->op == TensorOp::LEAF) continue;
            ag_release_graph_node_state(tensor);
        }
    }
    return Value::nil();
}

inline Value b_autograd_backward_scaled(const Value* a, int n, int l) {
    need_args("autograd_backward_scaled", n, 2, 3, l);
    GCTensor* root = ag_need_tensor("autograd_backward_scaled", a[0], 0, l);
    if (!ag_is_cuda(root)) {
        throw JitThrow{
            "autograd_backward_scaled(): resident CUDA tensors are required", l};
    }
    if (root->data.size() != 1) {
        throw JitThrow{
            "autograd_backward_scaled(): loss must be a scalar tensor", l};
    }
    const double requested_scale = need_num(
        "autograd_backward_scaled", a[1], 1, l);
    if (!std::isfinite(requested_scale) || requested_scale <= 0.0
        || std::abs(requested_scale)
               > (double)std::numeric_limits<float>::max()) {
        throw JitThrow{
            "autograd_backward_scaled(): scale must be a positive finite CUDA float32",
            l};
    }
    const float scale = (float)requested_scale;
    if (!std::isfinite(scale) || scale <= 0.0f) {
        throw JitThrow{
            "autograd_backward_scaled(): scale must be a positive finite CUDA float32",
            l};
    }
    if (scale == 1.0f) {
        throw JitThrow{
            "autograd_backward_scaled(): scale 1 is already unscaled; use backward()",
            l};
    }
    const double reciprocal_wide = 1.0 / (double)scale;
    const float reciprocal = (float)reciprocal_wide;
    if (!std::isfinite(reciprocal_wide) || !std::isfinite(reciprocal)
        || reciprocal <= 0.0f) {
        throw JitThrow{
            "autograd_backward_scaled(): reciprocal scale must be representable "
            "as positive CUDA float32", l};
    }
    bool retain_graph = false;
    if (n >= 3) {
        if (!a[2].is_bool()) {
            throw JitThrow{
                "autograd_backward_scaled(): retain_graph must be a bool", l};
        }
        retain_graph = a[2].as_bool();
    }
    (void)retain_graph;
    try {
        return ag_backward_cuda(a, n, l, scale, true);
    } catch (JitThrow& error) {
        const std::string old_prefix = "autograd_backward():";
        if (error.message.rfind(old_prefix, 0) == 0) {
            error.message.replace(
                0, old_prefix.size(), "autograd_backward_scaled():");
        }
        throw;
    }
}

inline void ag_collect_parameters(const char* name, const Value& value,
                                  std::vector<GCTensor*>& out, int line) {
    if (value.is_tensor()) {
        out.push_back(value.as_tensor());
        return;
    }
    if (!value.is_arr()) throw JitThrow{std::string(name) + "(): parameters must be a tensor or array", line};
    for (const auto& item : value.as_arr()->elements) {
        if (!item.is_tensor()) throw JitThrow{std::string(name) + "(): parameter arrays must contain tensors", line};
        out.push_back(item.as_tensor());
    }
}

inline void ag_require_unique_parameters(const char* name,
                                         const std::vector<GCTensor*>& parameters,
                                         int line, bool require_nonempty = false) {
    if (require_nonempty && parameters.empty()) {
        throw JitThrow{std::string(name) + "(): parameter list must not be empty", line};
    }
    std::unordered_set<GCTensor*> seen;
    for (GCTensor* tensor : parameters) {
        if (!seen.insert(tensor).second) {
            throw JitThrow{std::string(name) + "(): parameter list contains a duplicate tensor", line};
        }
    }
}

inline Value b_autograd_zero_grad(const Value* a, int n, int l) {
    need_args("autograd_zero_grad", n, 1, 1, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_zero_grad", a[0], parameters, l);
    ag_require_unique_parameters("autograd_zero_grad", parameters, l);
    size_t required_bytes = 0;
    std::vector<GCTensor*> cpu_needs_allocation;
    cpu_needs_allocation.reserve(parameters.size());
    for (GCTensor* tensor : parameters) {
        if (!tensor->requires_grad) continue;
        if (tensor->op != TensorOp::LEAF) {
            throw JitThrow{
                "autograd_zero_grad(): parameters must be leaf tensors with "
                "requires_grad=true", l};
        }
        if (ag_is_cuda(tensor)) {
            ag_cuda_require_tensor("autograd_zero_grad", tensor, l);
            if (!std::isfinite(tensor->cuda_grad_scale)
                || tensor->cuda_grad_scale < 0.0f
                || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
                throw JitThrow{
                    "autograd_zero_grad(): corrupted CUDA gradient scale", l};
            }
            if (tensor->cuda_grad) {
                ag_cuda_require_f32_allocation(
                    "autograd_zero_grad", tensor, tensor->cuda_grad,
                    "gradient buffer", l);
            }
            continue;
        }
        if (!tensor->grad.empty() && tensor->grad.size() != tensor->data.size()) {
            throw JitThrow{"autograd_zero_grad(): corrupted gradient buffer", l};
        }
        if (!tensor->grad.empty()) continue;
        const size_t bytes = tensor->data.size() * sizeof(double);
        if (bytes > ag_max_external_bytes() - required_bytes) {
            throw JitThrow{
                "autograd_zero_grad(): gradient buffers exceed the tensor memory limit",
                l};
        }
        required_bytes += bytes;
        cpu_needs_allocation.push_back(tensor);
    }
    ag_preflight_bytes(required_bytes, "autograd_zero_grad", l);

    std::vector<std::pair<GCTensor*, std::shared_ptr<AgCudaAllocation>>> cuda_pending;
    cuda_pending.reserve(parameters.size());
    for (GCTensor* tensor : parameters) {
        if (!ag_is_cuda(tensor) || !tensor->requires_grad) continue;
        auto zero = ag_cuda_allocate("autograd_zero_grad", tensor->data.size(), l);
        if (!SuraCudaDriver::instance().fill_f32(
                zero->handle, 0.0f, tensor->data.size())) {
            ag_cuda_fail("autograd_zero_grad", l);
        }
        cuda_pending.push_back({tensor, std::move(zero)});
    }
    if (!cuda_pending.empty() && !SuraCudaDriver::instance().synchronize()) {
        ag_cuda_fail("autograd_zero_grad", l);
    }
    std::vector<GCTensor*> allocated;
    allocated.reserve(cpu_needs_allocation.size());
    try {
        for (GCTensor* tensor : cpu_needs_allocation) {
            ag_ensure_grad(tensor, "autograd_zero_grad", l);
            allocated.push_back(tensor);
        }
    } catch (...) {
        for (GCTensor* tensor : allocated) ag_clear_grad_buffer(tensor);
        throw;
    }
    // Every fallible allocation and device operation has completed. Commit
    // CUDA and CPU zeroing together so a mixed parameter list cannot be
    // partially modified by a later host allocation failure.
    for (auto& entry : cuda_pending) {
        entry.first->cuda_grad = std::move(entry.second);
        entry.first->cuda_grad_scale = 0.0f;
        if (!entry.first->grad.empty()) ag_clear_grad_buffer(entry.first);
    }
    for (GCTensor* tensor : parameters) {
        if (ag_is_cuda(tensor)) continue;
        if (tensor->requires_grad) std::fill(tensor->grad.begin(), tensor->grad.end(), 0.0);
    }
    return Value::nil();
}

inline void ag_validate_parameter(const char* name, GCTensor* tensor, int line) {
    if (tensor->op != TensorOp::LEAF || !tensor->requires_grad) {
        throw JitThrow{std::string(name) + "(): optimizers require leaf tensors with requires_grad=true", line};
    }
}

inline Value b_autograd_unscale_gradients(const Value* a, int n, int l) {
    need_args("autograd_unscale_gradients", n, 1, 2, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_unscale_gradients", a[0], parameters, l);
    ag_require_unique_parameters(
        "autograd_unscale_gradients", parameters, l, true);

    bool explicit_scale = n >= 2 && !a[1].is_nil();
    float expected_scale = 0.0f;
    if (explicit_scale) {
        const double requested = need_num(
            "autograd_unscale_gradients", a[1], 1, l);
        if (!std::isfinite(requested) || requested <= 0.0
            || std::abs(requested)
                   > (double)std::numeric_limits<float>::max()) {
            throw JitThrow{
                "autograd_unscale_gradients(): scale must be a positive finite CUDA float32",
                l};
        }
        expected_scale = (float)requested;
        if (!std::isfinite(expected_scale) || expected_scale <= 0.0f) {
            throw JitThrow{
                "autograd_unscale_gradients(): scale must be a positive finite CUDA float32",
                l};
        }
    }

    std::vector<GCTensor*> scaled_parameters;
    scaled_parameters.reserve(parameters.size());
    int device_index = -1;
    for (GCTensor* tensor : parameters) {
        if (tensor->op != TensorOp::LEAF || !tensor->requires_grad) {
            throw JitThrow{
                "autograd_unscale_gradients(): parameters must be leaf tensors with "
                "requires_grad=true", l};
        }
        if (!ag_is_cuda(tensor)) {
            throw JitThrow{
                "autograd_unscale_gradients(): resident CUDA parameters are required",
                l};
        }
        ag_cuda_require_tensor("autograd_unscale_gradients", tensor, l);
        if (device_index < 0) {
            device_index = tensor->cuda_data->device_index;
        } else if (tensor->cuda_data->device_index != device_index) {
            throw JitThrow{
                "autograd_unscale_gradients(): parameters span multiple CUDA devices",
                l};
        }
        if (!tensor->grad.empty()) {
            throw JitThrow{
                "autograd_unscale_gradients(): CUDA parameter has a legacy host gradient",
                l};
        }
        if (!std::isfinite(tensor->cuda_grad_scale)
            || tensor->cuda_grad_scale < 0.0f
            || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
            throw JitThrow{
                "autograd_unscale_gradients(): corrupted CUDA gradient scale", l};
        }
        if (!tensor->cuda_grad) continue;
        ag_cuda_require_f32_allocation(
            "autograd_unscale_gradients", tensor, tensor->cuda_grad,
            "gradient buffer", l);
        // A scale-zero buffer is the known-zero neutral state produced by
        // zero_grad(), so it needs neither division nor a finite-status pass.
        if (tensor->cuda_grad_scale == 0.0f) continue;
        if (explicit_scale) {
            if (tensor->cuda_grad_scale != expected_scale) {
                throw JitThrow{
                    "autograd_unscale_gradients(): supplied scale does not match "
                    "the persistent CUDA gradient scale", l};
            }
        } else if (expected_scale == 0.0f) {
            expected_scale = tensor->cuda_grad_scale;
        } else if (tensor->cuda_grad_scale != expected_scale) {
            throw JitThrow{
                "autograd_unscale_gradients(): parameters contain gradients with "
                "different loss scales", l};
        }
        scaled_parameters.push_back(tensor);
    }

    auto make_result = [](bool finite, bool committed, float scale,
                          size_t gradients, uint32_t status_bits) {
        Value result = Value::make_dict();
        auto* fields = result.as_dict();
        fields->elements["finite"] = Value(finite);
        fields->elements["found_inf"] = Value(!finite);
        fields->elements["scale"] = Value((double)scale);
        fields->elements["gradient_tensors"] = Value((double)gradients);
        fields->elements["committed"] = Value(committed);
        fields->elements["status"] = Value((double)status_bits);
        return result;
    };

    if (scaled_parameters.empty()) {
        return make_result(true, true, expected_scale, 0, 0);
    }
    if (expected_scale == 1.0f) {
        throw JitThrow{
            "autograd_unscale_gradients(): gradients are already unscaled", l};
    }
    const double inverse_wide = 1.0 / (double)expected_scale;
    const float inverse_scale = (float)inverse_wide;
    if (!std::isfinite(inverse_wide) || !std::isfinite(inverse_scale)
        || inverse_scale <= 0.0f) {
        throw JitThrow{
            "autograd_unscale_gradients(): reciprocal scale is not representable "
            "as positive CUDA float32", l};
    }

    struct PendingUnscaledGradient {
        GCTensor* tensor = nullptr;
        std::shared_ptr<AgCudaAllocation> gradient;
    };
    std::vector<PendingUnscaledGradient> pending;
    pending.reserve(scaled_parameters.size());
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    auto status = ag_cuda_allocate("autograd_unscale_gradients", 1, l);
    if (!driver.fill_f32(status->handle, 0.0f, 1)) {
        ag_cuda_fail("autograd_unscale_gradients", l);
    }
    for (GCTensor* tensor : scaled_parameters) {
        auto candidate = ag_cuda_allocate(
            "autograd_unscale_gradients", tensor->data.size(), l);
        if (!driver.scale_f32(
                tensor->cuda_grad->handle, candidate->handle,
                inverse_scale, tensor->data.size())
            || !driver.finite_status_f32(
                candidate->handle, status->handle,
                tensor->data.size(), 1u)) {
            ag_cuda_fail("autograd_unscale_gradients", l);
        }
        pending.push_back({tensor, std::move(candidate)});
    }
    if (!driver.synchronize()) {
        ag_cuda_fail("autograd_unscale_gradients", l);
    }
    float status_storage = 0.0f;
    if (!driver.download_f32(status->handle, 0, &status_storage, 1)) {
        ag_cuda_fail("autograd_unscale_gradients", l);
    }
    uint32_t status_bits = 0;
    std::memcpy(&status_bits, &status_storage, sizeof(status_bits));
    if (status_bits != 0) {
        // Keep every original scaled gradient and its metadata intact so the
        // caller can inspect, clear, or retry the complete failed step.
        return make_result(false, false, expected_scale,
                           pending.size(), status_bits);
    }
    // Allocate the observable result before the no-throw commit. Even host
    // allocation failure must leave every original scaled gradient intact.
    Value success = make_result(
        true, true, expected_scale, pending.size(), 0);
    for (auto& entry : pending) {
        entry.tensor->cuda_grad = std::move(entry.gradient);
        entry.tensor->cuda_grad_scale = 1.0f;
    }
    return success;
}

inline Value b_autograd_sgd(const Value* a, int n, int l) {
    need_args("autograd_sgd", n, 2, 3, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_sgd", a[0], parameters, l);
    ag_require_unique_parameters("autograd_sgd", parameters, l, true);
    double learning_rate = need_num("autograd_sgd", a[1], 1, l);
    if (!std::isfinite(learning_rate) || learning_rate <= 0.0) {
        throw JitThrow{"autograd_sgd(): learning_rate must be positive", l};
    }
    GCDict* options = nn_options("autograd_sgd", a, n, 2, l);
    ag_validate_options("autograd_sgd", options, {"momentum", "weight_decay"}, l);
    double momentum = nn_option_number("autograd_sgd", options, "momentum", 0.0, 0.0, 0.999999, l);
    double weight_decay = nn_option_number("autograd_sgd", options, "weight_decay", 0.0, 0.0, 100.0, l);

    const bool has_cuda_parameter = std::any_of(
        parameters.begin(), parameters.end(), [](GCTensor* tensor) {
            return ag_is_cuda(tensor);
        });
    if (has_cuda_parameter) {
        const float cuda_learning_rate = (float)learning_rate;
        const float cuda_momentum = (float)momentum;
        const float cuda_weight_decay = (float)weight_decay;
        if (!std::isfinite(cuda_learning_rate) || cuda_learning_rate <= 0.0f) {
            throw JitThrow{
                "autograd_sgd(): learning_rate is not representable as positive CUDA float32",
                l};
        }
        if (momentum > 0.0 && cuda_momentum == 0.0f) {
            throw JitThrow{
                "autograd_sgd(): momentum underflows CUDA float32", l};
        }
        if (weight_decay > 0.0 && cuda_weight_decay == 0.0f) {
            throw JitThrow{
                "autograd_sgd(): weight_decay underflows CUDA float32", l};
        }
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        const auto float32_storage = SuraCudaDriver::TensorStorage::FLOAT32;
        auto require_f32_state = [&](const std::shared_ptr<AgCudaAllocation>& state,
                                     const GCTensor* tensor,
                                     const char* description) {
            if (!state) return;
            if (state->handle == SuraCudaDriver::INVALID_DEVICE_HANDLE
                || state->elements != tensor->data.size()
                || state->device_index != tensor->cuda_data->device_index
                || state->storage != float32_storage) {
                throw JitThrow{std::string("autograd_sgd(): corrupted CUDA ")
                               + description, l};
            }
        };
        int cuda_device_index = -1;
        for (GCTensor* tensor : parameters) {
            ag_validate_parameter("autograd_sgd", tensor, l);
            if (!ag_is_cuda(tensor)) {
                throw JitThrow{
                    "autograd_sgd(): mixed CPU/CUDA parameter lists are not supported", l};
            }
            ag_cuda_require_tensor("autograd_sgd", tensor, l);
            if (cuda_device_index < 0) {
                cuda_device_index = tensor->cuda_data->device_index;
            } else if (tensor->cuda_data->device_index != cuda_device_index) {
                throw JitThrow{
                    "autograd_sgd(): parameters span multiple CUDA devices", l};
            }
            if (!tensor->grad.empty()) {
                throw JitThrow{
                    "autograd_sgd(): CUDA parameter has a legacy host gradient", l};
            }
            require_f32_state(tensor->cuda_grad, tensor, "gradient buffer");
            require_f32_state(tensor->cuda_sgd_velocity, tensor,
                              "momentum buffer");
            if (tensor->data.dtype() == TensorDType::FLOAT32) {
                if (tensor->cuda_master_data) {
                    throw JitThrow{
                        "autograd_sgd(): float32 CUDA parameters must not have "
                        "master-weight state", l};
                }
            } else {
                require_f32_state(tensor->cuda_master_data, tensor,
                                  "master-weight buffer");
            }
            if (!std::isfinite(tensor->cuda_grad_scale)
                || tensor->cuda_grad_scale < 0.0f
                || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
                throw JitThrow{
                    "autograd_sgd(): corrupted CUDA gradient scale", l};
            }
            if (tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f
                && tensor->cuda_grad_scale != 1.0f) {
                throw JitThrow{
                    "autograd_sgd(): scaled CUDA gradient must be passed through "
                    "unscale_gradients() before the optimizer step", l};
            }
            if (!tensor->sgd_velocity.empty()) {
                throw JitThrow{
                    "autograd_sgd(): CUDA parameter has CPU momentum state", l};
            }
        }

        struct PendingCudaSgd {
            GCTensor* tensor = nullptr;
            std::shared_ptr<AgCudaAllocation> parameter;
            std::shared_ptr<AgCudaAllocation> master;
            std::shared_ptr<AgCudaAllocation> velocity;
        };
        std::vector<PendingCudaSgd> pending;
        pending.reserve(parameters.size());
        const bool has_cuda_gradient = std::any_of(
            parameters.begin(), parameters.end(), [](GCTensor* tensor) {
                return (bool)tensor->cuda_grad;
            });
        std::shared_ptr<AgCudaAllocation> status;
        if (has_cuda_gradient) {
            status = ag_cuda_allocate("autograd_sgd", 1, l);
            if (!driver.fill_f32(status->handle, 0.0f, 1)) {
                ag_cuda_fail("autograd_sgd", l);
            }
        }
        for (GCTensor* tensor : parameters) {
            if (!tensor->cuda_grad) {
                // Parameters omitted from this step keep every optimizer state,
                // regardless of this invocation's momentum option.
                continue;
            }
            PendingCudaSgd next;
            next.tensor = tensor;
            const size_t count = tensor->data.size();

            // Float16/bfloat16 are visible weights only. Their optimizer math
            // stays in float32, with the persistent master initialized lazily
            // into this still-unpublished transaction.
            std::shared_ptr<AgCudaAllocation> current_parameter_f32;
            const bool low_precision =
                tensor->data.dtype() == TensorDType::FLOAT16
                || tensor->data.dtype() == TensorDType::BFLOAT16;
            if (low_precision) {
                if (tensor->cuda_master_data) {
                    current_parameter_f32 = tensor->cuda_master_data;
                } else {
                    current_parameter_f32 =
                        ag_cuda_allocate("autograd_sgd", count, l);
                    if (!driver.unpack_u16_to_f32(
                            tensor->cuda_data->handle,
                            current_parameter_f32->handle, count)) {
                        ag_cuda_fail("autograd_sgd", l);
                    }
                }
            } else {
                // Float32 parameters preserve the original one-allocation
                // behavior and never publish redundant master state.
                current_parameter_f32 = tensor->cuda_data;
            }

            auto effective_gradient = ag_cuda_allocate("autograd_sgd", count, l);
            if (weight_decay == 0.0) {
                ag_cuda_copy_all("autograd_sgd", *tensor->cuda_grad,
                                 *effective_gradient, l);
            } else {
                if (!driver.scale_f32(current_parameter_f32->handle,
                                      effective_gradient->handle,
                                      cuda_weight_decay, count)
                    || !driver.add_f32(tensor->cuda_grad->handle,
                                       effective_gradient->handle,
                                       effective_gradient->handle, count)) {
                    ag_cuda_fail("autograd_sgd", l);
                }
            }

            std::shared_ptr<AgCudaAllocation> update = effective_gradient;
            if (momentum > 0.0) {
                next.velocity = ag_cuda_allocate("autograd_sgd", count, l);
                if (tensor->cuda_sgd_velocity) {
                    if (!driver.scale_f32(tensor->cuda_sgd_velocity->handle,
                                          next.velocity->handle,
                                          cuda_momentum, count)
                        || !driver.add_f32(next.velocity->handle,
                                           effective_gradient->handle,
                                           next.velocity->handle, count)) {
                        ag_cuda_fail("autograd_sgd", l);
                    }
                } else {
                    ag_cuda_copy_all("autograd_sgd", *effective_gradient,
                                     *next.velocity, l);
                }
                update = next.velocity;
            }

            auto scaled_update = ag_cuda_allocate("autograd_sgd", count, l);
            auto candidate_master = ag_cuda_allocate("autograd_sgd", count, l);
            if (!driver.scale_f32(update->handle, scaled_update->handle,
                                  cuda_learning_rate, count)
                || !driver.subtract_f32(current_parameter_f32->handle,
                                        scaled_update->handle,
                                        candidate_master->handle, count)) {
                ag_cuda_fail("autograd_sgd", l);
            }
            if (!driver.finite_status_f32(
                    candidate_master->handle, status->handle, count, 1u)
                || (next.velocity && !driver.finite_status_f32(
                    next.velocity->handle, status->handle, count, 2u))) {
                ag_cuda_fail("autograd_sgd", l);
            }
            if (low_precision) {
                next.parameter = ag_cuda_allocate_storage(
                    "autograd_sgd", count, tensor->data.dtype(), l);
                if (!driver.pack_f32_to_u16(
                        candidate_master->handle, next.parameter->handle,
                        status->handle, 4u, count)) {
                    ag_cuda_fail("autograd_sgd", l);
                }
                next.master = std::move(candidate_master);
            } else {
                next.parameter = std::move(candidate_master);
            }
            pending.push_back(std::move(next));
        }
        if (has_cuda_gradient) {
            float status_storage = 0.0f;
            if (!driver.download_f32(status->handle, 0, &status_storage, 1)) {
                ag_cuda_fail("autograd_sgd", l);
            }
            uint32_t status_bits = 0;
            std::memcpy(&status_bits, &status_storage, sizeof(status_bits));
            if (status_bits != 0) {
                throw JitThrow{
                    "autograd_sgd(): non-finite CUDA update rejected transactionally (status "
                    + std::to_string(status_bits) + ")", l};
            }
        }
        // All potentially throwing work is complete before this no-throw
        // parameter-list commit.
        Value result = Value::nil();
        for (auto& next : pending) {
            next.tensor->cuda_data = std::move(next.parameter);
            next.tensor->cuda_master_data = std::move(next.master);
            next.tensor->cuda_sgd_velocity = std::move(next.velocity);
            ag_cuda_mark_device_only(next.tensor);
            ++next.tensor->version;
        }
        return result;
    }

    // Preflight every update before mutating any parameter. This makes a
    // rejected non-finite step transactional across the full parameter list.
    for (GCTensor* tensor : parameters) {
        ag_validate_parameter("autograd_sgd", tensor, l);
        if (!tensor->grad.empty() && tensor->grad.size() != tensor->data.size()) {
            throw JitThrow{"autograd_sgd(): corrupted gradient buffer", l};
        }
        if (momentum > 0.0 && !tensor->sgd_velocity.empty()
            && tensor->sgd_velocity.size() != tensor->data.size()) {
            throw JitThrow{"autograd_sgd(): corrupted momentum buffer", l};
        }
        for (size_t i = 0; i < tensor->grad.size(); ++i) {
            double gradient = tensor->grad[i] + weight_decay * tensor->data[i];
            if (!std::isfinite(gradient)) {
                throw JitThrow{"autograd_sgd(): gradient is not finite", l};
            }
            double velocity = gradient;
            if (momentum > 0.0) {
                double previous = tensor->sgd_velocity.empty() ? 0.0 : tensor->sgd_velocity[i];
                velocity = momentum * previous + gradient;
            }
            double next = tensor->data[i] - learning_rate * velocity;
            if (!std::isfinite(velocity) || !std::isfinite(next)) {
                throw JitThrow{"autograd_sgd(): parameter update is not finite", l};
            }
            if (std::abs(next) > tensor_dtype_max_finite(tensor->data.dtype())) {
                throw JitThrow{"autograd_sgd(): parameter update overflows storage dtype", l};
            }
        }
    }

    std::vector<GCTensor*> allocated_velocity;
    allocated_velocity.reserve(parameters.size());
    try {
        if (momentum > 0.0) {
            size_t required_bytes = 0;
            for (GCTensor* tensor : parameters) {
                if (tensor->grad.empty() || !tensor->sgd_velocity.empty()) continue;
                size_t bytes = tensor->data.size() * sizeof(double);
                if (bytes > ag_max_external_bytes() - required_bytes) {
                    throw JitThrow{"autograd_sgd(): momentum buffers exceed the tensor memory limit", l};
                }
                required_bytes += bytes;
            }
            ag_preflight_bytes(required_bytes, "autograd_sgd", l);
            for (GCTensor* tensor : parameters) {
                if (tensor->grad.empty() || !tensor->sgd_velocity.empty()) continue;
                size_t bytes = tensor->data.size() * sizeof(double);
                ag_reserve_bytes(tensor, bytes, "autograd_sgd", l);
                try {
                    tensor->sgd_velocity.assign(tensor->data.size(), 0.0);
                } catch (...) {
                    std::vector<double>().swap(tensor->sgd_velocity);
                    ag_release_bytes(tensor, bytes);
                    throw;
                }
                allocated_velocity.push_back(tensor);
            }
        }
    } catch (...) {
        for (GCTensor* tensor : allocated_velocity) {
            size_t bytes = tensor->sgd_velocity.size() * sizeof(double);
            std::vector<double>().swap(tensor->sgd_velocity);
            ag_release_bytes(tensor, bytes);
        }
        throw;
    }

    // An explicit momentum=0 step resets any dormant velocity so a later
    // momentum schedule starts fresh instead of reviving stale state.
    if (momentum == 0.0) {
        for (GCTensor* tensor : parameters) {
            size_t bytes = tensor->sgd_velocity.size() * sizeof(double);
            std::vector<double>().swap(tensor->sgd_velocity);
            ag_release_bytes(tensor, bytes);
        }
    }

    for (GCTensor* tensor : parameters) {
        if (tensor->grad.empty()) continue;
        for (size_t i = 0; i < tensor->data.size(); ++i) {
            double gradient = tensor->grad[i] + weight_decay * tensor->data[i];
            if (momentum > 0.0) {
                tensor->sgd_velocity[i] = momentum * tensor->sgd_velocity[i] + gradient;
                gradient = tensor->sgd_velocity[i];
            }
            tensor->data.set(i, tensor->data[i] - learning_rate * gradient);
        }
        ++tensor->version;
    }
    return Value::nil();
}

inline Value b_autograd_adam(const Value* a, int n, int l) {
    need_args("autograd_adam", n, 2, 3, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_adam", a[0], parameters, l);
    ag_require_unique_parameters("autograd_adam", parameters, l, true);
    double learning_rate = need_num("autograd_adam", a[1], 1, l);
    if (!std::isfinite(learning_rate) || learning_rate <= 0.0) {
        throw JitThrow{"autograd_adam(): learning_rate must be positive", l};
    }
    GCDict* options = nn_options("autograd_adam", a, n, 2, l);
    ag_validate_options("autograd_adam", options,
                        {"beta1", "beta2", "epsilon", "weight_decay"}, l);
    double beta1 = nn_option_number("autograd_adam", options, "beta1", 0.9, 0.0, 0.999999, l);
    double beta2 = nn_option_number("autograd_adam", options, "beta2", 0.999, 0.0, 0.999999999, l);
    double epsilon = nn_option_number("autograd_adam", options, "epsilon", 1e-8, 1e-16, 1.0, l);
    double weight_decay = nn_option_number("autograd_adam", options, "weight_decay", 0.0, 0.0, 100.0, l);
    const bool has_cuda_parameter = std::any_of(
        parameters.begin(), parameters.end(), [](GCTensor* tensor) {
            return ag_is_cuda(tensor);
        });
    if (has_cuda_parameter) {
        const float cuda_learning_rate = (float)learning_rate;
        const float cuda_epsilon = (float)epsilon;
        const float cuda_weight_decay = (float)weight_decay;
        if (!std::isfinite(cuda_learning_rate) || cuda_learning_rate <= 0.0f) {
            throw JitThrow{
                "autograd_adam(): learning_rate is not representable as positive CUDA float32",
                l};
        }
        if (!std::isfinite(cuda_epsilon) || cuda_epsilon <= 0.0f) {
            throw JitThrow{
                "autograd_adam(): epsilon is not representable as positive CUDA float32",
                l};
        }
        if (!std::isfinite(cuda_weight_decay)
            || (weight_decay > 0.0 && cuda_weight_decay == 0.0f)) {
            throw JitThrow{
                "autograd_adam(): weight_decay is not representable as CUDA float32",
                l};
        }
        SuraCudaDriver& driver = SuraCudaDriver::instance();
        const auto float32_storage = SuraCudaDriver::TensorStorage::FLOAT32;
        auto require_f32_state = [&](const std::shared_ptr<AgCudaAllocation>& state,
                                     const GCTensor* tensor,
                                     const char* description) {
            if (!state) return;
            if (state->handle == SuraCudaDriver::INVALID_DEVICE_HANDLE
                || state->elements != tensor->data.size()
                || state->device_index != tensor->cuda_data->device_index
                || state->storage != float32_storage) {
                throw JitThrow{std::string("autograd_adam(): corrupted CUDA ")
                               + description, l};
            }
        };
        int cuda_device_index = -1;
        for (GCTensor* tensor : parameters) {
            ag_validate_parameter("autograd_adam", tensor, l);
            if (!ag_is_cuda(tensor)) {
                throw JitThrow{
                    "autograd_adam(): mixed CPU/CUDA parameter lists are not supported", l};
            }
            ag_cuda_require_tensor("autograd_adam", tensor, l);
            if (cuda_device_index < 0) {
                cuda_device_index = tensor->cuda_data->device_index;
            } else if (tensor->cuda_data->device_index != cuda_device_index) {
                throw JitThrow{"autograd_adam(): parameters span multiple CUDA devices", l};
            }
            if (!tensor->grad.empty()) {
                throw JitThrow{"autograd_adam(): CUDA parameter has a legacy host gradient", l};
            }
            require_f32_state(tensor->cuda_grad, tensor, "gradient buffer");
            require_f32_state(tensor->cuda_adam_m, tensor,
                              "first-moment buffer");
            require_f32_state(tensor->cuda_adam_v, tensor,
                              "second-moment buffer");
            if (tensor->data.dtype() == TensorDType::FLOAT32) {
                if (tensor->cuda_master_data) {
                    throw JitThrow{
                        "autograd_adam(): float32 CUDA parameters must not have "
                        "master-weight state", l};
                }
            } else {
                require_f32_state(tensor->cuda_master_data, tensor,
                                  "master-weight buffer");
            }
            if (!std::isfinite(tensor->cuda_grad_scale)
                || tensor->cuda_grad_scale < 0.0f
                || (!tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f)) {
                throw JitThrow{
                    "autograd_adam(): corrupted CUDA gradient scale", l};
            }
            if (tensor->cuda_grad && tensor->cuda_grad_scale != 0.0f
                && tensor->cuda_grad_scale != 1.0f) {
                throw JitThrow{
                    "autograd_adam(): scaled CUDA gradient must be passed through "
                    "unscale_gradients() before the optimizer step", l};
            }
            const bool moments_empty = !tensor->cuda_adam_m && !tensor->cuda_adam_v;
            const bool moments_ready = tensor->cuda_adam_m && tensor->cuda_adam_v;
            if (!moments_empty && !moments_ready) {
                throw JitThrow{"autograd_adam(): corrupted CUDA moment buffers", l};
            }
            if (!tensor->adam_m.empty() || !tensor->adam_v.empty()) {
                throw JitThrow{"autograd_adam(): CUDA parameter has CPU Adam state", l};
            }
            if ((moments_empty && (tensor->adam_step != 0
                                   || tensor->adam_beta1_product != 1.0
                                   || tensor->adam_beta2_product != 1.0))
                || (moments_ready && tensor->adam_step == 0)
                || !std::isfinite(tensor->adam_beta1_product)
                || !std::isfinite(tensor->adam_beta2_product)
                || tensor->adam_beta1_product < 0.0 || tensor->adam_beta1_product > 1.0
                || tensor->adam_beta2_product < 0.0 || tensor->adam_beta2_product > 1.0) {
                throw JitThrow{"autograd_adam(): corrupted CUDA Adam counters", l};
            }
            if (tensor->cuda_grad
                && tensor->adam_step == std::numeric_limits<uint64_t>::max()) {
                throw JitThrow{"autograd_adam(): optimizer step counter overflow", l};
            }
        }

        struct PendingCudaAdam {
            GCTensor* tensor = nullptr;
            // Keep unpublished lazy master/moment inputs alive until the
            // asynchronous transaction has reached its status boundary.
            std::shared_ptr<AgCudaAllocation> old_parameter_f32;
            std::shared_ptr<AgCudaAllocation> old_first_moment;
            std::shared_ptr<AgCudaAllocation> old_second_moment;
            std::shared_ptr<AgCudaAllocation> next_parameter;
            std::shared_ptr<AgCudaAllocation> next_master;
            std::shared_ptr<AgCudaAllocation> next_first_moment;
            std::shared_ptr<AgCudaAllocation> next_second_moment;
            uint64_t step = 0;
            double beta1_product = 1.0;
            double beta2_product = 1.0;
        };
        std::vector<PendingCudaAdam> pending;
        pending.reserve(parameters.size());
        const float one_minus_beta1 = (float)(1.0 - beta1);
        const float one_minus_beta2 = (float)(1.0 - beta2);
        if (one_minus_beta1 <= 0.0f || one_minus_beta2 <= 0.0f) {
            throw JitThrow{"autograd_adam(): beta complement underflows CUDA float32", l};
        }
        const bool has_gradient = std::any_of(
            parameters.begin(), parameters.end(), [](GCTensor* tensor) {
                return (bool)tensor->cuda_grad;
            });
        if (!has_gradient) return Value::nil();
        auto status = ag_cuda_allocate("autograd_adam", 1, l);
        if (!driver.fill_f32(status->handle, 0.0f, 1)) ag_cuda_fail("autograd_adam", l);
        for (GCTensor* tensor : parameters) {
            if (!tensor->cuda_grad) continue;
            PendingCudaAdam next;
            next.tensor = tensor;
            next.old_first_moment = tensor->cuda_adam_m;
            next.old_second_moment = tensor->cuda_adam_v;
            const size_t count = tensor->data.size();
            const bool low_precision =
                tensor->data.dtype() == TensorDType::FLOAT16
                || tensor->data.dtype() == TensorDType::BFLOAT16;
            if (low_precision) {
                if (tensor->cuda_master_data) {
                    next.old_parameter_f32 = tensor->cuda_master_data;
                } else {
                    // The first Adam step publishes a master only after every
                    // parameter candidate and low-precision pack has passed.
                    next.old_parameter_f32 =
                        ag_cuda_allocate("autograd_adam", count, l);
                    if (!driver.unpack_u16_to_f32(
                            tensor->cuda_data->handle,
                            next.old_parameter_f32->handle, count)) {
                        ag_cuda_fail("autograd_adam", l);
                    }
                }
            } else {
                next.old_parameter_f32 = tensor->cuda_data;
            }
            if (!next.old_first_moment) {
                next.old_first_moment = ag_cuda_allocate("autograd_adam", count, l);
                next.old_second_moment = ag_cuda_allocate("autograd_adam", count, l);
                if (!driver.fill_f32(next.old_first_moment->handle, 0.0f, count)
                    || !driver.fill_f32(next.old_second_moment->handle, 0.0f, count)) {
                    ag_cuda_fail("autograd_adam", l);
                }
            }
            auto candidate_parameter_f32 =
                ag_cuda_allocate("autograd_adam", count, l);
            next.next_first_moment = ag_cuda_allocate("autograd_adam", count, l);
            next.next_second_moment = ag_cuda_allocate("autograd_adam", count, l);
            next.step = tensor->adam_step + 1;
            next.beta1_product = tensor->adam_beta1_product * beta1;
            next.beta2_product = tensor->adam_beta2_product * beta2;
            const double correction1 = 1.0 - next.beta1_product;
            const double correction2 = 1.0 - next.beta2_product;
            if (!std::isfinite(correction1) || correction1 <= 0.0
                || !std::isfinite(correction2) || correction2 <= 0.0
                || (float)correction1 <= 0.0f || (float)correction2 <= 0.0f) {
                throw JitThrow{"autograd_adam(): bias correction underflow", l};
            }
            if (!driver.adam_f32(
                    next.old_parameter_f32->handle, tensor->cuda_grad->handle,
                    next.old_first_moment->handle, next.old_second_moment->handle,
                    candidate_parameter_f32->handle,
                    next.next_first_moment->handle,
                    next.next_second_moment->handle,
                    status->handle, count,
                    cuda_learning_rate, one_minus_beta1, one_minus_beta2,
                    (float)correction1, (float)correction2,
                    cuda_epsilon, cuda_weight_decay)) {
                ag_cuda_fail("autograd_adam", l);
            }
            if (low_precision) {
                next.next_parameter = ag_cuda_allocate_storage(
                    "autograd_adam", count, tensor->data.dtype(), l);
                if (!driver.pack_f32_to_u16(
                        candidate_parameter_f32->handle,
                        next.next_parameter->handle,
                        status->handle, 16u, count)) {
                    ag_cuda_fail("autograd_adam", l);
                }
                next.next_master = std::move(candidate_parameter_f32);
            } else {
                // Float32 retains its original single visible allocation and
                // deliberately publishes no redundant master state.
                next.next_parameter = std::move(candidate_parameter_f32);
            }
            pending.push_back(std::move(next));
        }
        float status_storage = 0.0f;
        if (!driver.download_f32(status->handle, 0, &status_storage, 1)) {
            ag_cuda_fail("autograd_adam", l);
        }
        uint32_t status_bits = 0;
        std::memcpy(&status_bits, &status_storage, sizeof(status_bits));
        if (status_bits != 0) {
            throw JitThrow{
                "autograd_adam(): non-finite CUDA update rejected transactionally (status "
                + std::to_string(status_bits) + ")", l};
        }
        Value success = Value::nil();
        for (auto& next : pending) {
            next.tensor->cuda_data = std::move(next.next_parameter);
            next.tensor->cuda_master_data = std::move(next.next_master);
            next.tensor->cuda_adam_m = std::move(next.next_first_moment);
            next.tensor->cuda_adam_v = std::move(next.next_second_moment);
            next.tensor->adam_step = next.step;
            next.tensor->adam_beta1_product = next.beta1_product;
            next.tensor->adam_beta2_product = next.beta2_product;
            ag_cuda_mark_device_only(next.tensor);
            ++next.tensor->version;
        }
        return success;
    }

    // As with SGD, validate the complete update first. Optimizer state is
    // allocated only after arithmetic preflight and is rolled back on failure.
    for (GCTensor* tensor : parameters) {
        ag_validate_parameter("autograd_adam", tensor, l);
        if (!tensor->grad.empty() && tensor->grad.size() != tensor->data.size()) {
            throw JitThrow{"autograd_adam(): corrupted gradient buffer", l};
        }
        bool moments_empty = tensor->adam_m.empty() && tensor->adam_v.empty();
        bool moments_ready = tensor->adam_m.size() == tensor->data.size()
                          && tensor->adam_v.size() == tensor->data.size();
        if (!moments_empty && !moments_ready) {
            throw JitThrow{"autograd_adam(): corrupted moment buffers", l};
        }
        if (tensor->grad.empty()) continue;
        if (tensor->adam_step == std::numeric_limits<uint64_t>::max()) {
            throw JitThrow{"autograd_adam(): optimizer step counter overflow", l};
        }
        if (!std::isfinite(tensor->adam_beta1_product)
            || !std::isfinite(tensor->adam_beta2_product)
            || tensor->adam_beta1_product < 0.0 || tensor->adam_beta1_product > 1.0
            || tensor->adam_beta2_product < 0.0 || tensor->adam_beta2_product > 1.0) {
            throw JitThrow{"autograd_adam(): corrupted beta-product state", l};
        }
        double next_beta1_product = tensor->adam_beta1_product * beta1;
        double next_beta2_product = tensor->adam_beta2_product * beta2;
        double correction1 = 1.0 - next_beta1_product;
        double correction2 = 1.0 - next_beta2_product;
        for (size_t i = 0; i < tensor->grad.size(); ++i) {
            double gradient = tensor->grad[i] + weight_decay * tensor->data[i];
            double squared = gradient * gradient;
            double old_m = moments_empty ? 0.0 : tensor->adam_m[i];
            double old_v = moments_empty ? 0.0 : tensor->adam_v[i];
            double next_m = beta1 * old_m + (1.0 - beta1) * gradient;
            double next_v = beta2 * old_v + (1.0 - beta2) * squared;
            if (!std::isfinite(gradient) || !std::isfinite(squared)
                || !std::isfinite(next_m) || !std::isfinite(next_v) || next_v < 0.0) {
                throw JitThrow{"autograd_adam(): gradient magnitude is too large", l};
            }
            double update = (next_m / correction1)
                          / (std::sqrt(next_v / correction2) + epsilon);
            double next = tensor->data[i] - learning_rate * update;
            if (!std::isfinite(update) || !std::isfinite(next)) {
                throw JitThrow{"autograd_adam(): parameter update is not finite", l};
            }
            if (std::abs(next) > tensor_dtype_max_finite(tensor->data.dtype())) {
                throw JitThrow{"autograd_adam(): parameter update overflows storage dtype", l};
            }
        }
    }

    std::vector<GCTensor*> allocated_moments;
    allocated_moments.reserve(parameters.size());
    try {
        size_t required_bytes = 0;
        for (GCTensor* tensor : parameters) {
            if (tensor->grad.empty() || !tensor->adam_m.empty()) continue;
            size_t bytes = tensor->data.size() * sizeof(double) * 2;
            if (bytes > ag_max_external_bytes() - required_bytes) {
                throw JitThrow{"autograd_adam(): moment buffers exceed the tensor memory limit", l};
            }
            required_bytes += bytes;
        }
        ag_preflight_bytes(required_bytes, "autograd_adam", l);
        for (GCTensor* tensor : parameters) {
            if (tensor->grad.empty() || !tensor->adam_m.empty()) continue;
            size_t bytes = tensor->data.size() * sizeof(double) * 2;
            ag_reserve_bytes(tensor, bytes, "autograd_adam", l);
            try {
                tensor->adam_m.assign(tensor->data.size(), 0.0);
                tensor->adam_v.assign(tensor->data.size(), 0.0);
            } catch (...) {
                std::vector<double>().swap(tensor->adam_m);
                std::vector<double>().swap(tensor->adam_v);
                ag_release_bytes(tensor, bytes);
                throw;
            }
            allocated_moments.push_back(tensor);
        }
    } catch (...) {
        for (GCTensor* tensor : allocated_moments) {
            size_t bytes = (tensor->adam_m.size() + tensor->adam_v.size()) * sizeof(double);
            std::vector<double>().swap(tensor->adam_m);
            std::vector<double>().swap(tensor->adam_v);
            ag_release_bytes(tensor, bytes);
        }
        throw;
    }

    for (GCTensor* tensor : parameters) {
        if (tensor->grad.empty()) continue;
        ++tensor->adam_step;
        tensor->adam_beta1_product *= beta1;
        tensor->adam_beta2_product *= beta2;
        double correction1 = 1.0 - tensor->adam_beta1_product;
        double correction2 = 1.0 - tensor->adam_beta2_product;
        for (size_t i = 0; i < tensor->data.size(); ++i) {
            double gradient = tensor->grad[i] + weight_decay * tensor->data[i];
            tensor->adam_m[i] = beta1 * tensor->adam_m[i] + (1.0 - beta1) * gradient;
            tensor->adam_v[i] = beta2 * tensor->adam_v[i] + (1.0 - beta2) * gradient * gradient;
            double update = (tensor->adam_m[i] / correction1)
                          / (std::sqrt(tensor->adam_v[i] / correction2) + epsilon);
            tensor->data.set(i, tensor->data[i] - learning_rate * update);
        }
        ++tensor->version;
    }
    return Value::nil();
}

inline Value b_autograd_reset_optimizer(const Value* a, int n, int l) {
    need_args("autograd_reset_optimizer", n, 1, 1, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_reset_optimizer", a[0], parameters, l);
    ag_require_unique_parameters("autograd_reset_optimizer", parameters, l);
    for (GCTensor* tensor : parameters) {
        size_t bytes = (tensor->adam_m.size() + tensor->adam_v.size()
                        + tensor->sgd_velocity.size()) * sizeof(double);
        std::vector<double>().swap(tensor->adam_m);
        std::vector<double>().swap(tensor->adam_v);
        std::vector<double>().swap(tensor->sgd_velocity);
        tensor->cuda_master_data.reset();
        tensor->cuda_sgd_velocity.reset();
        tensor->cuda_adam_m.reset();
        tensor->cuda_adam_v.reset();
        tensor->adam_step = 0;
        tensor->adam_beta1_product = 1.0;
        tensor->adam_beta2_product = 1.0;
        ag_release_bytes(tensor, bytes);
    }
    return Value::nil();
}

inline Value b_autograd_grad_norm(const Value* a, int n, int l) {
    need_args("autograd_grad_norm", n, 1, 1, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_grad_norm", a[0], parameters, l);
    ag_require_unique_parameters("autograd_grad_norm", parameters, l);
    for (GCTensor* tensor : parameters) {
        if (tensor->cuda_grad) {
            throw JitThrow{
                "autograd_grad_norm(): CUDA gradient norm is not implemented yet", l};
        }
    }
    double norm = 0.0;
    for (GCTensor* tensor : parameters)
        for (double gradient : tensor->grad) {
            if (!std::isfinite(gradient)) throw JitThrow{"autograd_grad_norm(): gradient is not finite", l};
            norm = std::hypot(norm, gradient);
        }
    return Value(norm);
}

inline Value b_autograd_clip_grad_norm(const Value* a, int n, int l) {
    need_args("autograd_clip_grad_norm", n, 2, 2, l);
    std::vector<GCTensor*> parameters;
    ag_collect_parameters("autograd_clip_grad_norm", a[0], parameters, l);
    ag_require_unique_parameters("autograd_clip_grad_norm", parameters, l);
    for (GCTensor* tensor : parameters) {
        if (tensor->cuda_grad) {
            throw JitThrow{
                "autograd_clip_grad_norm(): CUDA gradient clipping is not implemented yet", l};
        }
    }
    double maximum = need_num("autograd_clip_grad_norm", a[1], 1, l);
    if (!std::isfinite(maximum) || maximum <= 0.0) {
        throw JitThrow{"autograd_clip_grad_norm(): max_norm must be positive", l};
    }
    double norm = 0.0;
    for (GCTensor* tensor : parameters)
        for (double gradient : tensor->grad) {
            if (!std::isfinite(gradient)) throw JitThrow{"autograd_clip_grad_norm(): gradient is not finite", l};
            norm = std::hypot(norm, gradient);
        }
    if (norm > maximum) {
        double scale = maximum / norm;
        for (GCTensor* tensor : parameters)
            for (double& gradient : tensor->grad) gradient *= scale;
    }
    return Value(norm);
}

} // namespace SuraStd
