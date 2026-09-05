#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <limits>
#include <charconv>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <iterator>
#include <mutex>




enum class ObjType {
    STRING,
    ARRAY,
    DICT,
    FUNC,
    UPVALUE,
    INSTANCE,
    TENSOR
};

enum class TensorOp : uint8_t {
    LEAF,
    ADD,
    SUB,
    MUL,
    DIV,
    NEG,
    MATMUL,
    TRANSPOSE,
    RELU,
    TANH,
    SIGMOID,
    SOFTMAX,
    SUM,
    MEAN,
    MSE,
    BCE,
    BCE_LOGITS,
    CROSS_ENTROPY,
    CAST,
    DEVICE_COPY,
    RESHAPE,
    GELU,
    LAYER_NORM,
    EMBEDDING,
    CAUSAL_ATTENTION,
    CROSS_ENTROPY_IDS
};




struct GCObject {
    ObjType obj_type;
    bool marked = false;
    
    GCObject(ObjType t) : obj_type(t) {}
    virtual ~GCObject() = default;
    virtual void mark() {}
};

// Iterative graph marker implemented after Value is complete.  All object
// edges funnel through this entry point so deeply nested user data cannot
// consume the native call stack.
inline void gc_mark_object(GCObject* object);

struct GCString : public GCObject {
    std::string str;
    GCString(const std::string& s) : GCObject(ObjType::STRING), str(s) {}
    GCString(std::string&& s) : GCObject(ObjType::STRING), str(std::move(s)) {}
};

class Value;

class SmallValueVec {
    static constexpr size_t InlineCapacity = 4;
    Value* data_;
    size_t size_;
    size_t capacity_;
    alignas(8) uint64_t inline_storage_[InlineCapacity];

    Value* inline_data() { return reinterpret_cast<Value*>(inline_storage_); }
    const Value* inline_data() const { return reinterpret_cast<const Value*>(inline_storage_); }
    bool using_inline() const { return data_ == inline_data(); }
    void ensure_capacity(size_t wanted);

public:
    SmallValueVec();
    SmallValueVec(const SmallValueVec& other);
    SmallValueVec(const std::vector<Value>& other);
    ~SmallValueVec();

    SmallValueVec& operator=(const SmallValueVec& other);
    SmallValueVec& operator=(const std::vector<Value>& other);

    size_t size() const;
    bool empty() const;
    void resize(size_t wanted, const Value& fill);
    void push_back(const Value& value);

    Value& operator[](size_t index);
    const Value& operator[](size_t index) const;
    Value* data();
    const Value* data() const;
    Value* begin();
    Value* end();
    const Value* begin() const;
    const Value* end() const;
};

struct GCClosure : public GCObject {
    std::string name;
    int func_idx = -1;
    std::vector<struct GCUpvalue*> upvalues;
    GCClosure(const std::string& n) : GCObject(ObjType::FUNC), name(n) {}
    void mark() override;
};

// Forward declaration for JIT inline IC class identity pointer.
// Full definition is in jit_op.hpp.
struct JitClassInfo;

struct GCInstance : public GCObject {
    std::string  class_name;
    const std::string* class_name_ref = nullptr;
    SmallValueVec fields;
    JitClassInfo* jit_info = nullptr; // set at construction; used by JIT inline IC guard
    GCInstance(const std::string& name) : GCObject(ObjType::INSTANCE), class_name(name), class_name_ref(&class_name) {}
    GCInstance(const std::string* name) : GCObject(ObjType::INSTANCE), class_name_ref(name) {}
    const std::string& type_name() const {
        static const std::string empty;
        return class_name_ref ? *class_name_ref : empty;
    }
    void mark() override;
};

struct GCArray : public GCObject {
    std::vector<Value> elements;
    GCArray() : GCObject(ObjType::ARRAY) {}
    void mark() override;
};

struct GCDict : public GCObject {
    std::unordered_map<std::string, Value> elements;
    GCDict() : GCObject(ObjType::DICT) {}
    void mark() override;
};





// The heap is process-global today. Embedding entry points hold this recursive
// mutex for an entire VM operation so marking and sweeping cannot race another
// context's allocation. GC internals lock it as well for exception-safe direct
// allocations and for idempotent process teardown.
inline std::recursive_mutex& gc_runtime_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

class GC {
public:
    static std::vector<GCObject*>& get_objects();

    template<typename T, typename... Args>
    static T* allocate(Args&&... args) {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        T* obj = nullptr;
        if constexpr (std::is_same_v<T, GCInstance>) {
            obj = allocate_instance(std::forward<Args>(args)...);
        } else if constexpr (is_pooled_v<T>) {
            obj = Pool<T>::allocate(std::forward<Args>(args)...);
        } else {
            obj = new T(std::forward<Args>(args)...);
        }
        try {
            get_objects().push_back(obj);
        } catch (...) {
            release_object(obj);
            throw;
        }
        return obj;
    }

    static void sweep() {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        std::vector<GCObject*> survivors;
#ifdef SURA_GC_TEST_HOOKS
        if (sweep_reserve_failure_for_test()) {
            sweep_reserve_failure_for_test() = false;
            throw std::bad_alloc();
        }
#endif
        // Allocate the complete survivor registry before releasing anything.
        // If reserve() throws, the original registry and every object remain
        // untouched; subsequent collection/shutdown cannot observe dangling
        // entries left by a partially completed sweep.
        survivors.reserve(get_objects().size());
        for (auto* obj : get_objects()) {
            if (obj->marked) {
                obj->marked = false;
                survivors.push_back(obj);
            } else {
                release_object(obj);
            }
        }
        get_objects() = std::move(survivors);
    }

    static size_t object_count() {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        return get_objects().size();
    }

#ifdef SURA_GC_TEST_HOOKS
    static void fail_next_sweep_reserve_for_test() {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        sweep_reserve_failure_for_test() = true;
    }
#endif

    // Release every managed object and the instance slab pool. This is
    // intentionally idempotent so CLI teardown and the final embedding context
    // can establish a leak-checkable lifetime boundary.
    static void shutdown() noexcept {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        auto& objects = get_objects();
        for (auto* object : objects) release_object(object);
        objects.clear();

        auto& blocks = instance_blocks();
        for (void* block : blocks) ::operator delete(block);
        blocks.clear();
        instance_free_list() = nullptr;
    }

private:
#ifdef SURA_GC_TEST_HOOKS
    static bool& sweep_reserve_failure_for_test() {
        static bool enabled = false;
        return enabled;
    }
#endif
    static constexpr size_t INSTANCE_BLOCK_SIZE = 4096;

    static void*& instance_free_list() {
        static void* head = nullptr;
        return head;
    }

    static std::vector<void*>& instance_blocks() {
        static std::vector<void*> blocks;
        return blocks;
    }

    static void replenish_instance_pool() {
        char* block = static_cast<char*>(::operator new(sizeof(GCInstance) * INSTANCE_BLOCK_SIZE));
        try {
            instance_blocks().push_back(block);
        } catch (...) {
            ::operator delete(block);
            throw;
        }

        void*& head = instance_free_list();
        for (size_t i = 0; i < INSTANCE_BLOCK_SIZE; ++i) {
            void* slot = block + i * sizeof(GCInstance);
            *reinterpret_cast<void**>(slot) = head;
            head = slot;
        }
    }

    // The same scheme for the objects programs allocate in bulk: strings,
    // arrays and dictionaries. A general-purpose malloc/free pair costs more
    // than the rest of a short string's life; a slot from a per-type free
    // list costs a few loads. Blocks are never returned to the OS. The
    // runtime mutex, held by allocate() and sweep(), guards the lists.
    template<typename T>
    static constexpr bool is_pooled_v =
        std::is_same_v<T, GCString> || std::is_same_v<T, GCArray> || std::is_same_v<T, GCDict>;

    template<typename T>
    struct Pool {
        static constexpr size_t BLOCK_SLOTS = 4096;
        static void*& free_list() {
            static void* head = nullptr;
            return head;
        }
        static std::vector<void*>& blocks() {
            static std::vector<void*> list;
            return list;
        }
        static void replenish() {
            char* block = static_cast<char*>(::operator new(sizeof(T) * BLOCK_SLOTS));
            try {
                blocks().push_back(block);
            } catch (...) {
                ::operator delete(block);
                throw;
            }
            void*& head = free_list();
            for (size_t i = 0; i < BLOCK_SLOTS; ++i) {
                void* slot = block + i * sizeof(T);
                *reinterpret_cast<void**>(slot) = head;
                head = slot;
            }
        }
        template<typename... Args>
        static T* allocate(Args&&... args) {
            void*& head = free_list();
            if (!head) replenish();
            void* slot = head;
            head = *reinterpret_cast<void**>(slot);
            try {
                return new (slot) T(std::forward<Args>(args)...);
            } catch (...) {
                *reinterpret_cast<void**>(slot) = head;
                head = slot;
                throw;
            }
        }
        static void release(T* obj) {
            obj->~T();
            void*& head = free_list();
            *reinterpret_cast<void**>(obj) = head;
            head = obj;
        }
    };

    template<typename... Args>
    static GCInstance* allocate_instance(Args&&... args) {
        void*& head = instance_free_list();
        if (!head) replenish_instance_pool();
        void* slot = head;
        head = *reinterpret_cast<void**>(slot);
        try {
            return new (slot) GCInstance(std::forward<Args>(args)...);
        } catch (...) {
            *reinterpret_cast<void**>(slot) = head;
            head = slot;
            throw;
        }
    }

    static void release_instance(GCInstance* obj) {
        obj->~GCInstance();
        void*& head = instance_free_list();
        *reinterpret_cast<void**>(obj) = head;
        head = obj;
    }

    static void release_object(GCObject* obj) {
        if (!obj) return;
        switch (obj->obj_type) {
            case ObjType::INSTANCE: release_instance(static_cast<GCInstance*>(obj)); break;
            case ObjType::STRING:   Pool<GCString>::release(static_cast<GCString*>(obj)); break;
            case ObjType::ARRAY:    Pool<GCArray>::release(static_cast<GCArray*>(obj)); break;
            case ObjType::DICT:     Pool<GCDict>::release(static_cast<GCDict*>(obj)); break;
            default:                delete obj; break;
        }
    }
};

// Native builtin implementations occasionally hold freshly allocated GC
// objects in C++ locals while an allocation preflight asks the VM to collect.
// Register those short-lived roots here so the collector can mark them too.
inline std::vector<GCObject*>& gc_native_roots_registry() {
    // Persistent embedding roots must be visible to every VM sharing the
    // process-global heap, not only to the thread that created the context.
    // FFI entry points serialize access with gc_runtime_mutex().
    static std::vector<GCObject*> roots;
    return roots;
}

// Marking iterates a stable snapshot. This avoids iterator invalidation or a
// data race if a host thread registers/unregisters a root at the same time.
inline std::vector<GCObject*> gc_native_roots_storage() {
    std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
    return gc_native_roots_registry();
}

class GCNativeRoot {
    GCObject* object_ = nullptr;
public:
    explicit GCNativeRoot(GCObject* object) : object_(object) {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        if (object_) gc_native_roots_registry().push_back(object_);
    }
    ~GCNativeRoot() {
        std::lock_guard<std::recursive_mutex> lock(gc_runtime_mutex());
        if (!object_) return;
        auto& roots = gc_native_roots_registry();
        if (!roots.empty() && roots.back() == object_) {
            roots.pop_back();
            return;
        }
        auto found = std::find(roots.rbegin(), roots.rend(), object_);
        if (found != roots.rend()) roots.erase(std::next(found).base());
    }
    GCNativeRoot(const GCNativeRoot&) = delete;
    GCNativeRoot& operator=(const GCNativeRoot&) = delete;
};

inline std::atomic<size_t>& tensor_external_bytes_storage() {
    static std::atomic<size_t> bytes{0};
    return bytes;
}

enum class TensorDType : uint8_t {
    FLOAT64 = 0,
    FLOAT32 = 1,
    FLOAT16 = 2,
    BFLOAT16 = 3
};

namespace SuraStd {
struct AgCudaAllocation;
}

enum class TensorCoherence : uint8_t {
    HOST_ONLY = 0,
    SYNCHRONIZED = 1,
    DEVICE_ONLY = 2
};

inline const char* tensor_dtype_name(TensorDType dtype) {
    switch (dtype) {
        case TensorDType::FLOAT64: return "float64";
        case TensorDType::FLOAT32: return "float32";
        case TensorDType::FLOAT16: return "float16";
        case TensorDType::BFLOAT16: return "bfloat16";
    }
    return "unknown";
}

inline size_t tensor_dtype_size(TensorDType dtype) {
    switch (dtype) {
        case TensorDType::FLOAT64: return sizeof(double);
        case TensorDType::FLOAT32: return sizeof(float);
        case TensorDType::FLOAT16:
        case TensorDType::BFLOAT16: return sizeof(uint16_t);
    }
    return 0;
}

inline double tensor_dtype_max_finite(TensorDType dtype) {
    switch (dtype) {
        case TensorDType::FLOAT64: return std::numeric_limits<double>::max();
        case TensorDType::FLOAT32: return (double)std::numeric_limits<float>::max();
        // BF16 has the float32 exponent width but only seven stored fraction
        // bits. Values above 0x7f7f round to infinity when packed, so using
        // float32::max here would silently admit a non-finite tensor.
        case TensorDType::BFLOAT16: return std::ldexp(255.0, 120);
        case TensorDType::FLOAT16: return 65504.0;
    }
    return 0.0;
}

// Owns the actual dtype-sized CPU representation. Arithmetic kernels read
// values as doubles for numerical robustness, then writes are quantized back
// to the selected storage type. Resident CUDA intermediates may carry only
// logical size/dtype metadata; their CPU bytes are allocated lazily at an
// explicit observation boundary.
class TensorBuffer {
    std::vector<uint8_t> bytes_;
    size_t size_ = 0;
    TensorDType dtype_ = TensorDType::FLOAT64;
    bool host_readable_ = true;

    static uint16_t float_to_half(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t sign = (bits >> 16) & 0x8000U;
        uint32_t exponent = (bits >> 23) & 0xffU;
        uint32_t mantissa = bits & 0x7fffffU;
        if (exponent == 0xffU) {
            return (uint16_t)(sign | (mantissa ? 0x7e00U : 0x7c00U));
        }
        int adjusted = (int)exponent - 127 + 15;
        if (adjusted >= 31) return (uint16_t)(sign | 0x7c00U);
        if (adjusted <= 0) {
            if (adjusted < -10) return (uint16_t)sign;
            mantissa |= 0x800000U;
            unsigned shift = (unsigned)(14 - adjusted);
            uint32_t result = mantissa >> shift;
            uint32_t remainder = mantissa & ((1U << shift) - 1U);
            uint32_t halfway = 1U << (shift - 1U);
            if (remainder > halfway || (remainder == halfway && (result & 1U))) ++result;
            return (uint16_t)(sign | result);
        }
        uint32_t result = mantissa >> 13;
        uint32_t remainder = mantissa & 0x1fffU;
        if (remainder > 0x1000U || (remainder == 0x1000U && (result & 1U))) {
            ++result;
            if (result == 0x400U) {
                result = 0;
                ++adjusted;
                if (adjusted >= 31) return (uint16_t)(sign | 0x7c00U);
            }
        }
        return (uint16_t)(sign | ((uint32_t)adjusted << 10) | result);
    }

    static float half_to_float(uint16_t value) {
        uint32_t sign = ((uint32_t)value & 0x8000U) << 16;
        uint32_t exponent = ((uint32_t)value >> 10) & 0x1fU;
        uint32_t mantissa = (uint32_t)value & 0x3ffU;
        uint32_t bits = 0;
        if (exponent == 0) {
            if (mantissa == 0) {
                bits = sign;
            } else {
                int normalized_exponent = 1;
                while ((mantissa & 0x400U) == 0) {
                    mantissa <<= 1;
                    --normalized_exponent;
                }
                mantissa &= 0x3ffU;
                uint32_t float_exponent = (uint32_t)(normalized_exponent + (127 - 15));
                bits = sign | (float_exponent << 23) | (mantissa << 13);
            }
        } else if (exponent == 0x1fU) {
            bits = sign | 0x7f800000U | (mantissa << 13);
        } else {
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    static uint16_t float_to_bfloat16(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t rounding = 0x7fffU + ((bits >> 16) & 1U);
        bits += rounding;
        return (uint16_t)(bits >> 16);
    }

    static float bfloat16_to_float(uint16_t value) {
        uint32_t bits = (uint32_t)value << 16;
        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

public:
    class const_iterator {
        const TensorBuffer* owner_ = nullptr;
        size_t index_ = 0;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = double;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = double;
        const_iterator() = default;
        const_iterator(const TensorBuffer* owner, size_t index)
            : owner_(owner), index_(index) {}
        double operator*() const { return (*owner_)[index_]; }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto copy = *this; ++index_; return copy; }
        bool operator==(const const_iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }
    };

    TensorBuffer() = default;
    TensorBuffer(const TensorBuffer&) = delete;
    TensorBuffer(TensorBuffer&&) noexcept = default;
    TensorBuffer& operator=(const TensorBuffer&) = delete;
    TensorBuffer& operator=(TensorBuffer&&) noexcept = default;

    TensorDType dtype() const { return dtype_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    // Logical packed storage size, independent of whether a CUDA tensor has
    // materialized its optional host mirror.  Serialization and the public
    // storage_bytes() API use this value.
    size_t byte_size() const { return size_ * tensor_dtype_size(dtype_); }
    // Actual resident host allocation.  This is deliberately separate from
    // byte_size() so device-only tensors do not lie to memory accounting.
    size_t host_byte_size() const { return bytes_.size(); }
    bool host_readable() const { return host_readable_; }
    const uint8_t* packed_data() const {
        if (!host_readable_) {
            throw std::logic_error("CUDA tensor host data is stale; use an explicit host observation or to(cpu)");
        }
        return bytes_.data();
    }
    void mark_host_stale() { host_readable_ = false; }
    void mark_host_current() { host_readable_ = true; }

    void assign_device_metadata(size_t count, TensorDType dtype) {
        if (!bytes_.empty() || size_ != 0) {
            throw std::logic_error("device metadata can only initialize an empty TensorBuffer");
        }
        std::vector<uint8_t>().swap(bytes_);
        size_ = count;
        dtype_ = dtype;
        host_readable_ = false;
    }

    double operator[](size_t index) const {
        if (!host_readable_) {
            throw std::logic_error("CUDA tensor host data is stale; use an explicit host observation or to(cpu)");
        }
        const uint8_t* source = bytes_.data() + index * tensor_dtype_size(dtype_);
        switch (dtype_) {
            case TensorDType::FLOAT64: {
                double value = 0.0;
                std::memcpy(&value, source, sizeof(value));
                return value;
            }
            case TensorDType::FLOAT32: {
                float value = 0.0f;
                std::memcpy(&value, source, sizeof(value));
                return (double)value;
            }
            case TensorDType::FLOAT16: {
                uint16_t value = 0;
                std::memcpy(&value, source, sizeof(value));
                return (double)half_to_float(value);
            }
            case TensorDType::BFLOAT16: {
                uint16_t value = 0;
                std::memcpy(&value, source, sizeof(value));
                return (double)bfloat16_to_float(value);
            }
        }
        return 0.0;
    }

    double operator[](size_t index) { return static_cast<const TensorBuffer&>(*this)[index]; }

    void set(size_t index, double value) {
        if (!host_readable_) {
            throw std::logic_error("CUDA tensor host data is stale; synchronize it before CPU mutation");
        }
        uint8_t* destination = bytes_.data() + index * tensor_dtype_size(dtype_);
        switch (dtype_) {
            case TensorDType::FLOAT64:
                std::memcpy(destination, &value, sizeof(value));
                return;
            case TensorDType::FLOAT32: {
                float converted = (float)value;
                std::memcpy(destination, &converted, sizeof(converted));
                return;
            }
            case TensorDType::FLOAT16: {
                uint16_t converted = float_to_half((float)value);
                std::memcpy(destination, &converted, sizeof(converted));
                return;
            }
            case TensorDType::BFLOAT16: {
                uint16_t converted = float_to_bfloat16((float)value);
                std::memcpy(destination, &converted, sizeof(converted));
                return;
            }
        }
    }

    void assign(const std::vector<double>& values, TensorDType dtype) {
        dtype_ = dtype;
        size_ = values.size();
        host_readable_ = true;
        bytes_.assign(size_ * tensor_dtype_size(dtype_), 0);
        for (size_t i = 0; i < size_; ++i) set(i, values[i]);
    }

    void assign(std::vector<double>&& values, TensorDType dtype) {
        assign(static_cast<const std::vector<double>&>(values), dtype);
        std::vector<double>().swap(values);
    }

    void assign_packed(const void* source, size_t count, TensorDType dtype) {
        const size_t element_bytes = tensor_dtype_size(dtype);
        if (count != 0 && element_bytes > std::numeric_limits<size_t>::max() / count) {
            throw std::length_error("packed tensor byte size overflow");
        }
        const size_t bytes = count * element_bytes;
        if (bytes != 0 && !source) {
            throw std::invalid_argument("packed tensor source is null");
        }
        std::vector<uint8_t> replacement(bytes, 0);
        if (bytes != 0) std::memcpy(replacement.data(), source, bytes);
        bytes_ = std::move(replacement);
        size_ = count;
        dtype_ = dtype;
        host_readable_ = true;
    }

    std::vector<double> to_vector() const {
        std::vector<double> values(size_);
        for (size_t i = 0; i < size_; ++i) values[i] = (*this)[i];
        return values;
    }

    operator std::vector<double>() const { return to_vector(); }

    void clear_and_release() {
        std::vector<uint8_t>().swap(bytes_);
        size_ = 0;
        host_readable_ = true;
    }

    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }
    const_iterator begin() { return const_iterator(this, 0); }
    const_iterator end() { return const_iterator(this, size_); }
};

// A row-major, contiguous typed tensor. Autograd graph edges point only to
// older tensors, so the graph is a DAG and can be traced by the normal GC.
struct GCTensor : public GCObject {
    TensorBuffer data;
    // CUDA storage is owned by an RAII wrapper defined by autograd.hpp.  The
    // CPU buffer keeps logical shape/dtype/numel metadata even when it has no
    // allocated values. Every host observation must pass through the explicit
    // coherence boundary before reading data.
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_data;
    // CUDA gradients and optimizer state remain device-resident across
    // backward/step calls.  Host `grad` is populated only by an explicit
    // observation boundary such as autograd.grad().
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_grad;
    // Scale attached to the persistent CUDA gradient. Zero denotes the
    // explicit all-zero buffer created by zero_grad(), one is optimizer-ready,
    // and any other positive finite value must be unscaled before a step.
    float cuda_grad_scale = 0.0f;
    // Low-precision CUDA parameters keep their optimizer-visible value in
    // packed f16/bf16 storage but update an f32 master copy. This prevents
    // sub-ULP training steps from disappearing after every optimizer call.
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_master_data;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_sgd_velocity;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_adam_m;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_adam_v;
    // Saved forward state for resident CUDA LayerNorm. These device-only f32
    // diagnostics hold one rounded mean and reciprocal standard deviation per
    // row. Backward recomputes f64 row statistics from the version-checked
    // input because an exact f32-row mean is not always representable as f32.
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_layer_norm_mean;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_layer_norm_rstd;
    // Host-validated token ids packed as raw uint32 slots for the resident
    // CUDA embedding gather/scatter kernels.
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_embedding_ids;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_cross_entropy_ids;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_cross_entropy_max;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_cross_entropy_inv_sum;
    // Per causal-attention row statistics retained on device for the
    // low-memory CUDA backward recomputation path.
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_attention_max;
    std::shared_ptr<SuraStd::AgCudaAllocation> cuda_attention_inv_sum;
    // The forward dispatch is part of the autograd contract. A graph that
    // used an optimized score reduction must keep the matching backward plan
    // even if process environment variables are changed before backward.
    bool cuda_attention_parallel_plan = false;
    bool cuda_attention_fused_plan = false;
    // Resident matmul outputs and gradients remain f32. Inputs may use f32 or
    // packed f16/bf16 storage; this records the forward compute/storage plan
    // that its matching backward must preserve.
    TensorDType cuda_matmul_compute_dtype = TensorDType::FLOAT32;
    TensorCoherence coherence = TensorCoherence::HOST_ONLY;
    std::vector<double> grad;
    std::vector<size_t> shape;
    std::vector<GCTensor*> parents;
    std::vector<uint64_t> parent_versions;
    // Operation-specific integer metadata. This stores reshape/transpose
    // axes and token ids without introducing a second graph object type.
    std::vector<size_t> op_indices;
    std::vector<double> adam_m;
    std::vector<double> adam_v;
    std::vector<double> sgd_velocity;
    uint64_t adam_step = 0;
    double adam_beta1_product = 1.0;
    double adam_beta2_product = 1.0;
    uint64_t version = 0;
    size_t tracked_bytes = 0;
    double op_scalar = 0.0;
    TensorOp op = TensorOp::LEAF;
    bool requires_grad = false;
    bool graph_freed = false;

    GCTensor() : GCObject(ObjType::TENSOR) {}
    GCTensor(const GCTensor&) = delete;
    GCTensor& operator=(const GCTensor&) = delete;
    GCTensor(GCTensor&&) = delete;
    GCTensor& operator=(GCTensor&&) = delete;
    ~GCTensor() override {
        if (tracked_bytes) tensor_external_bytes_storage().fetch_sub(tracked_bytes, std::memory_order_relaxed);
    }
    void mark() override;
};










// ── NaN-boxing: all Values are exactly 8 bytes ────────────────────────
// Encoding (bit layout of uint64_t bits_):
//   Number : (bits & NBQNAN) != NBQNAN  — any valid double + canonical NaN
//   NIL    : bits == NBQNAN             (0x7FFC000000000000)
//   FALSE  : bits == NBQNAN|1           (0x7FFC000000000001)
//   TRUE   : bits == NBQNAN|2           (0x7FFC000000000002)
//   Object : (bits & NBOBJ) == NBOBJ    (sign bit + NBQNAN + 48-bit ptr)
//
// Why NBQNAN=0x7FFC? Arithmetic NaN from x87/SSE = 0x7FF8...(bit50=0).
// Our sentinel has bit50=1 → no collision. Any input NaN is canonicalized.
static constexpr uint64_t NBQNAN  = 0x7FFC000000000000ULL;
static constexpr uint64_t NBSIGN  = 0x8000000000000000ULL;
static constexpr uint64_t NBOBJ   = NBQNAN | NBSIGN;      // 0xFFFC000000000000
static constexpr uint64_t NBNIL   = NBQNAN;
static constexpr uint64_t NBFALSE = NBQNAN | 1;
static constexpr uint64_t NBTRUE  = NBQNAN | 2;
static constexpr uint64_t NBPMASK = ~NBOBJ;                // lower 50 bits = pointer

class Value {
    uint64_t bits_;
public:
    // ── Constructors ──────────────────────────────────────────────────
    Value() : bits_(NBNIL) {}

    explicit Value(double v) {
        // Normalize any NaN to canonical 0x7FF8... (avoids sentinel collision)
        if (__builtin_expect(v != v, 0)) { bits_ = 0x7FF8000000000000ULL; return; }
        memcpy(&bits_, &v, 8);
    }
    explicit Value(float v)      : Value((double)v) {}
    explicit Value(int v)        : Value((double)v) {}
    explicit Value(long v)       : Value((double)v) {}
    explicit Value(long long v)  : Value((double)v) {}
    explicit Value(bool b)       : bits_(b ? NBTRUE : NBFALSE) {}
    explicit Value(GCObject* p)  : bits_(p ? NBOBJ | (uint64_t)(uintptr_t)p : NBNIL) {}
    explicit Value(const std::string& s) : Value((GCObject*)GC::allocate<GCString>(s)) {}
    explicit Value(std::string&& s) : Value((GCObject*)GC::allocate<GCString>(std::move(s))) {}
    explicit Value(const char* s)        : Value(std::string(s ? s : "")) {}

    // ── Factory helpers ───────────────────────────────────────────────
    static Value nil()          { return Value(); }
    // Construct a Value directly from its 64-bit NaN-boxed bit pattern.
    // Used by the native JIT to return Values through RAX.
    static Value from_bits(uint64_t b) {
        Value v;
        std::memcpy(&v.bits_, &b, 8);
        return v;
    }
    uint64_t raw_bits() const { return bits_; }
    static Value make_array()   { return Value((GCObject*)GC::allocate<GCArray>()); }
    static Value make_dict()    { return Value((GCObject*)GC::allocate<GCDict>()); }
    static Value make_tensor()  { return Value((GCObject*)GC::allocate<GCTensor>()); }
    static Value make_closure(const std::string& n) { return Value((GCObject*)GC::allocate<GCClosure>(n)); }
    static Value make_inst(const std::string& n)    { return Value((GCObject*)GC::allocate<GCInstance>(n)); }
    static Value make_inst_ref(const std::string* n) { return Value((GCObject*)GC::allocate<GCInstance>(n)); }

    // ── Type checks ───────────────────────────────────────────────────
    bool is_num()  const { return (bits_ & NBQNAN) != NBQNAN; }
    bool is_nil()  const { return bits_ == NBNIL; }
    bool is_bool() const { return (bits_ == NBFALSE) | (bits_ == NBTRUE); }
    bool is_obj()  const { return (bits_ & NBOBJ)  == NBOBJ; }

    GCObject* as_obj() const { return reinterpret_cast<GCObject*>(bits_ & NBPMASK); }

    bool is_str()     const { return is_obj() && as_obj()->obj_type == ObjType::STRING; }
    bool is_arr()     const { return is_obj() && as_obj()->obj_type == ObjType::ARRAY; }
    bool is_dict()    const { return is_obj() && as_obj()->obj_type == ObjType::DICT; }
    bool is_closure() const { return is_obj() && as_obj()->obj_type == ObjType::FUNC; }
    bool is_upvalue() const { return is_obj() && as_obj()->obj_type == ObjType::UPVALUE; }
    bool is_inst()    const { return is_obj() && as_obj()->obj_type == ObjType::INSTANCE; }
    bool is_tensor()  const { return is_obj() && as_obj()->obj_type == ObjType::TENSOR; }

    // ── Accessors ─────────────────────────────────────────────────────
    double      as_num()  const { double v; memcpy(&v, &bits_, 8); return v; }
    bool        as_bool() const { return bits_ == NBTRUE; }
    std::string as_str()  const { return is_str() ? static_cast<GCString*>(as_obj())->str : ""; }
    // Hot-path zero-copy reference. Caller must verify is_str() first.
    const std::string& as_str_ref() const { return static_cast<GCString*>(as_obj())->str; }
    GCArray*    as_arr()  const { return is_arr()  ? static_cast<GCArray*>(as_obj())    : nullptr; }
    GCDict*     as_dict() const { return is_dict() ? static_cast<GCDict*>(as_obj())     : nullptr; }
    GCClosure*  as_closure() const { return is_closure() ? static_cast<GCClosure*>(as_obj()) : nullptr; }
    GCUpvalue*  as_upvalue() const;
    GCInstance* as_inst() const { return is_inst() ? static_cast<GCInstance*>(as_obj()) : nullptr; }
    GCTensor*   as_tensor() const { return is_tensor() ? static_cast<GCTensor*>(as_obj()) : nullptr; }

    void mark_value() {
        if (is_obj()) gc_mark_object(as_obj());
    }

    
    bool truthy() const {
        if (is_nil()) return false;
        if (is_bool()) return as_bool();
        if (is_num()) return as_num() != 0.0;
        if (is_str()) return !as_str_ref().empty();
        if (is_arr()) return !as_arr()->elements.empty();
        if (is_dict()) return !as_dict()->elements.empty();
        return true;
    }

    std::string to_str() const {
        if (is_nil()) return "nil";
        if (is_bool()) return as_bool() ? "true" : "false";
        if (is_num()) {
            double v = as_num();
            const double integer_min = -std::ldexp(1.0, 63);
            const double integer_limit = std::ldexp(1.0, 63);
            if (std::isfinite(v) && v == std::floor(v)
                && v >= integer_min && v < integer_limit) {
                return std::to_string((long long)v);
            }
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), v,
                                           std::chars_format::general);
            if (converted.ec == std::errc()) return std::string(buffer, converted.ptr);
            std::ostringstream out;
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
            return out.str();
        }
        if (is_str()) return as_str();
        if (is_arr()) {
            std::string s = "[";
            auto* arr = as_arr();
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                if (i > 0) s += ", ";
                s += arr->elements[i].is_str() ? "\"" + arr->elements[i].as_str() + "\"" : arr->elements[i].to_str();
            }
            return s + "]";
        }
        if (is_dict()) {
            std::string s = "{";
            auto* dict = as_dict();
            bool first = true;
            for (const auto& [k, v] : dict->elements) {
                if (!first) s += ", ";
                s += "\"" + k + "\": " + (v.is_str() ? "\"" + v.as_str() + "\"" : v.to_str());
                first = false;
            }
            return s + "}";
        }
        if (is_closure()) return "<Func " + static_cast<GCClosure*>(as_obj())->name + ">";
        if (is_upvalue()) return "<Upvalue>";
        if (is_inst()) return "<Instance " + as_inst()->type_name() + ">";
        if (is_tensor()) {
            std::string out = "<Tensor shape=[";
            for (size_t i = 0; i < as_tensor()->shape.size(); ++i) {
                if (i) out += ",";
                out += std::to_string(as_tensor()->shape[i]);
            }
            out += "]";
            if (as_tensor()->data.dtype() != TensorDType::FLOAT64) {
                out += " dtype=";
                out += tensor_dtype_name(as_tensor()->data.dtype());
            }
            if (as_tensor()->requires_grad) out += " requires_grad";
            return out + ">";
        }
        return "<Unknown>";
    }

    double to_num() const {
        if (is_num()) return as_num();
        if (is_bool()) return as_bool() ? 1.0 : 0.0;
        if (is_str()) {
            try { return std::stod(as_str_ref()); } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    
    Value operator+(const Value& r) const {
        if (is_num() && r.is_num()) return Value(as_num() + r.as_num());
        if (is_str() || r.is_str()) {
            std::string joined;
            if (is_str()) joined = as_str_ref(); else joined = to_str();
            if (r.is_str()) joined += r.as_str_ref(); else joined += r.to_str();
            return Value(std::move(joined));
        }
        if (is_arr() && r.is_arr()) {
            Value nv = make_array();
            auto* arr = nv.as_arr();
            auto* a1 = as_arr(); auto* a2 = r.as_arr();
            arr->elements.insert(arr->elements.end(), a1->elements.begin(), a1->elements.end());
            arr->elements.insert(arr->elements.end(), a2->elements.begin(), a2->elements.end());
            return nv;
        }
        return nil();
    }
    Value operator-(const Value& r) const { return (is_num() && r.is_num()) ? Value(as_num() - r.as_num()) : nil(); }
    Value operator*(const Value& r) const { return (is_num() && r.is_num()) ? Value(as_num() * r.as_num()) : nil(); }
    Value operator/(const Value& r) const { return (is_num() && r.is_num() && r.as_num() != 0.0) ? Value(as_num() / r.as_num()) : nil(); }
    Value mod(const Value& r) const { return (is_num() && r.is_num() && r.as_num() != 0.0) ? Value(std::fmod(as_num(), r.as_num())) : nil(); }
    Value negate() const { return is_num() ? Value(-as_num()) : nil(); }

    bool eq(const Value& r) const {
        if (bits_ == r.bits_) return true;           // fast path: nil, bool, same ptr
        if (is_num() && r.is_num()) return as_num() == r.as_num();
        if (is_str() && r.is_str()) return as_str_ref() == r.as_str_ref();
        return false;
    }
    bool neq(const Value& r) const { return !eq(r); }
    bool lt(const Value& r) const { return (is_num() && r.is_num()) ? as_num() < r.as_num() : false; }
    bool lte(const Value& r) const { return (is_num() && r.is_num()) ? as_num() <= r.as_num() : false; }
    bool gt(const Value& r) const { return (is_num() && r.is_num()) ? as_num() > r.as_num() : false; }
    bool gte(const Value& r) const { return (is_num() && r.is_num()) ? as_num() >= r.as_num() : false; }
    
    Value logical_not() const { return Value(!truthy()); }

    
    Value arr_get(int idx) const {
        if (!is_arr()) return nil();
        auto* arr = as_arr();
        if (idx < 0) idx += (int)arr->elements.size();
        if (idx >= 0 && idx < (int)arr->elements.size()) return arr->elements[idx];
        return nil();
    }
    void arr_set(int idx, const Value& val) {
        if (!is_arr()) return;
        auto* arr = as_arr();
        if (idx < 0) idx += (int)arr->elements.size();
        if (idx >= 0 && idx < (int)arr->elements.size()) arr->elements[idx] = val;
    }

    Value dict_get(const std::string& k) const {
        if (!is_dict()) return nil();
        auto* dict = as_dict();
        auto found = dict->elements.find(k);
        return found == dict->elements.end() ? nil() : found->second;
    }
    void dict_set(const std::string& k, const Value& val) {
        if (is_dict()) as_dict()->elements[k] = val;
    }
    bool dict_has(const std::string& k) const {
        return is_dict() ? as_dict()->elements.count(k) > 0 : false;
    }
};

inline SmallValueVec::SmallValueVec()
    : data_(inline_data()), size_(0), capacity_(InlineCapacity) {}

inline SmallValueVec::SmallValueVec(const SmallValueVec& other)
    : data_(inline_data()), size_(0), capacity_(InlineCapacity) {
    *this = other;
}

inline SmallValueVec::SmallValueVec(const std::vector<Value>& other)
    : data_(inline_data()), size_(0), capacity_(InlineCapacity) {
    *this = other;
}

inline SmallValueVec::~SmallValueVec() {
    if (!using_inline()) delete[] data_;
}

inline size_t SmallValueVec::size() const {
    return size_;
}

inline bool SmallValueVec::empty() const {
    return size_ == 0;
}

inline void SmallValueVec::ensure_capacity(size_t wanted) {
    if (wanted <= capacity_) return;
    size_t next = capacity_ > 0 ? capacity_ : InlineCapacity;
    while (next < wanted) next *= 2;
    Value* fresh = new Value[next];
    for (size_t i = 0; i < size_; ++i) fresh[i] = data_[i];
    if (!using_inline()) delete[] data_;
    data_ = fresh;
    capacity_ = next;
}

inline void SmallValueVec::resize(size_t wanted, const Value& fill) {
    ensure_capacity(wanted);
    for (size_t i = size_; i < wanted; ++i) data_[i] = fill;
    size_ = wanted;
}

inline void SmallValueVec::push_back(const Value& value) {
    ensure_capacity(size_ + 1);
    data_[size_++] = value;
}

inline Value& SmallValueVec::operator[](size_t index) {
    return data_[index];
}

inline const Value& SmallValueVec::operator[](size_t index) const {
    return data_[index];
}

inline Value* SmallValueVec::data() {
    return data_;
}

inline const Value* SmallValueVec::data() const {
    return data_;
}

inline Value* SmallValueVec::begin() {
    return data_;
}

inline Value* SmallValueVec::end() {
    return data_ + size_;
}

inline const Value* SmallValueVec::begin() const {
    return data_;
}

inline const Value* SmallValueVec::end() const {
    return data_ + size_;
}

inline SmallValueVec& SmallValueVec::operator=(const SmallValueVec& other) {
    if (this == &other) return *this;
    resize(other.size_, Value::nil());
    for (size_t i = 0; i < other.size_; ++i) data_[i] = other.data_[i];
    return *this;
}

inline SmallValueVec& SmallValueVec::operator=(const std::vector<Value>& other) {
    resize(other.size(), Value::nil());
    for (size_t i = 0; i < other.size(); ++i) data_[i] = other[i];
    return *this;
}

struct GCUpvalue : public GCObject {
    Value* location;
    Value closed;
    GCUpvalue(Value* loc) : GCObject(ObjType::UPVALUE), location(loc) {}
    void mark() override;
};

inline std::vector<GCObject*>*& gc_active_mark_queue() {
    static thread_local std::vector<GCObject*>* queue = nullptr;
    return queue;
}

class GCMarkBatch {
    std::vector<GCObject*> pending_;
    bool finished_ = false;
public:
    GCMarkBatch() {
        if (gc_active_mark_queue() != nullptr)
            throw std::logic_error("nested GC mark batch");
        pending_.reserve(GC::object_count());
        gc_active_mark_queue() = &pending_;
    }
    ~GCMarkBatch() {
        if (gc_active_mark_queue() == &pending_)
            gc_active_mark_queue() = nullptr;
    }
    GCMarkBatch(const GCMarkBatch&) = delete;
    GCMarkBatch& operator=(const GCMarkBatch&) = delete;

    void finish() {
        if (finished_) return;
        try {
            while (!pending_.empty()) {
                GCObject* current = pending_.back();
                pending_.pop_back();
                current->mark();
            }
            finished_ = true;
            gc_active_mark_queue() = nullptr;
        } catch (...) {
            finished_ = true;
            gc_active_mark_queue() = nullptr;
            throw;
        }
    }
};

inline void gc_mark_object(GCObject* object) {
    if (!object || object->marked) return;
    std::vector<GCObject*>*& active = gc_active_mark_queue();
    if (active) {
        // The outer session reserves one slot per registered object before
        // changing a mark bit, so ordinary traversal performs no allocation.
        active->push_back(object);
        object->marked = true;
        return;
    }

    std::vector<GCObject*> pending;
    pending.reserve(GC::object_count());
    pending.push_back(object);
    object->marked = true;
    active = &pending;
    try {
        while (!pending.empty()) {
            GCObject* current = pending.back();
            pending.pop_back();
            current->mark();
        }
        active = nullptr;
    } catch (...) {
        active = nullptr;
        throw;
    }
}


inline void GCArray::mark() {
    for (auto& v : elements) v.mark_value();
}
inline void GCDict::mark() {
    for (auto& [k, v] : elements) v.mark_value();
}
inline void GCTensor::mark() {
    for (auto* parent : parents) gc_mark_object(parent);
}
inline void GCInstance::mark() {
    for (auto& v : fields) v.mark_value();
}
inline GCUpvalue* Value::as_upvalue() const { 
    return is_upvalue() ? static_cast<GCUpvalue*>(as_obj()) : nullptr; 
}
inline void GCUpvalue::mark() {
    if (location == nullptr) closed.mark_value();
}
inline void GCClosure::mark() {
    for (auto* uv : upvalues) gc_mark_object(uv);
}
