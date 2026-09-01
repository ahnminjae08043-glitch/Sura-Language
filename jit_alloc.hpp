#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <limits>
#include <unordered_set>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

// Description of a Win64 stack-affecting prologue. Code offsets are the byte
// offsets immediately after their instructions, as required by UNWIND_CODE.
// Register numbers use the architectural encoding (RBX=3, RBP=5, RSI=6,
// RDI=7, R12=12, R13=13, R14=14, R15=15).
struct Win64UnwindPush {
    uint8_t code_offset = 0;
    uint8_t register_number = 0;
};

struct Win64UnwindSpec {
    uint8_t prolog_size = 0;
    uint8_t stack_allocation_code_offset = 0;
    uint32_t stack_allocation_bytes = 0;
    std::vector<Win64UnwindPush> pushed_nonvolatiles;

    bool enabled() const noexcept {
        return prolog_size != 0 || stack_allocation_bytes != 0 ||
               !pushed_nonvolatiles.empty();
    }
};

// OS-backed executable memory with an optional move-safe Win64 dynamic
// function-table registration. The code and UNWIND_INFO share one allocation;
// RUNTIME_FUNCTION has a separate stable address so moving ExecCode is safe.
struct ExecCode {
    uint8_t* ptr = nullptr;
    size_t size = 0;      // complete mapping, including unwind bytes
    size_t code_size = 0; // executable function extent
#ifdef _WIN32
    RUNTIME_FUNCTION* runtime_function = nullptr;
    bool runtime_function_registered = false;
#endif

    ExecCode() = default;
    ExecCode(ExecCode&& other) noexcept { move_from(other); }
    ExecCode& operator=(ExecCode&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }
    ExecCode(const ExecCode&) = delete;
    ExecCode& operator=(const ExecCode&) = delete;
    ~ExecCode() { release(); }

    void release() noexcept {
#ifdef _WIN32
        if (runtime_function_registered && runtime_function) {
            if (!RtlDeleteFunctionTable(runtime_function)) {
                // Do not leave Windows holding a table whose UnwindData points
                // into freed memory. A process-lifetime leak is safer here.
                ptr = nullptr;
                size = 0;
                code_size = 0;
                runtime_function = nullptr;
                runtime_function_registered = false;
                return;
            }
            runtime_function_registered = false;
        }
        delete runtime_function;
        runtime_function = nullptr;
        if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
#else
        if (ptr) munmap(ptr, size);
#endif
        ptr = nullptr;
        size = 0;
        code_size = 0;
    }

    bool has_unwind_registration() const noexcept {
#ifdef _WIN32
        return runtime_function_registered;
#else
        return false;
#endif
    }

    // Allocates executable memory, appends optional unwind metadata, flips the
    // mapping to RX, and registers the dynamic Win64 function table.
    static ExecCode from_bytes(const std::vector<uint8_t>& code,
                               const Win64UnwindSpec& unwind = {}) {
        if (code.empty()) throw std::runtime_error("ExecCode: empty code");
        ExecCode result;
        result.code_size = code.size();
#ifdef _WIN32
        std::vector<uint8_t> unwind_bytes;
        size_t unwind_offset = 0;
        if (unwind.enabled()) {
            unwind_bytes = encode_win64_unwind(unwind, code.size());
            unwind_offset = align_up(code.size(), 4);
            if (unwind_offset > std::numeric_limits<DWORD>::max())
                throw std::runtime_error("ExecCode: unwind RVA exceeds Win64 range");
            if (unwind_offset > std::numeric_limits<size_t>::max() - unwind_bytes.size())
                throw std::runtime_error("ExecCode: unwind allocation size overflow");
            result.size = unwind_offset + unwind_bytes.size();
        } else {
            result.size = code.size();
        }

        result.ptr = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, result.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!result.ptr) throw std::runtime_error("VirtualAlloc failed");
        std::memcpy(result.ptr, code.data(), code.size());
        if (!unwind_bytes.empty()) {
            std::memset(result.ptr + code.size(), 0, unwind_offset - code.size());
            std::memcpy(result.ptr + unwind_offset, unwind_bytes.data(), unwind_bytes.size());
        }

        DWORD old_protection = 0;
        if (!VirtualProtect(result.ptr, result.size, PAGE_EXECUTE_READ, &old_protection)) {
            result.release();
            throw std::runtime_error("VirtualProtect failed");
        }
        if (!FlushInstructionCache(GetCurrentProcess(), result.ptr, code.size())) {
            result.release();
            throw std::runtime_error("FlushInstructionCache failed");
        }

        if (!unwind_bytes.empty()) {
            result.runtime_function = new RUNTIME_FUNCTION{};
            result.runtime_function->BeginAddress = 0;
            result.runtime_function->EndAddress = static_cast<DWORD>(code.size());
            result.runtime_function->UnwindData = static_cast<DWORD>(unwind_offset);
            if (!RtlAddFunctionTable(result.runtime_function, 1,
                                     reinterpret_cast<DWORD64>(result.ptr))) {
                delete result.runtime_function;
                result.runtime_function = nullptr;
                result.release();
                throw std::runtime_error("RtlAddFunctionTable failed");
            }
            result.runtime_function_registered = true;
        }
#else
        (void)unwind;
        result.size = code.size();
        result.ptr = static_cast<uint8_t*>(mmap(nullptr, result.size,
            PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
        if (result.ptr == MAP_FAILED) {
            result.ptr = nullptr;
            throw std::runtime_error("mmap failed");
        }
        std::memcpy(result.ptr, code.data(), result.size);
        if (mprotect(result.ptr, result.size, PROT_READ | PROT_EXEC) != 0) {
            result.release();
            throw std::runtime_error("mprotect failed");
        }
  #if defined(__GNUC__) || defined(__clang__)
        __builtin___clear_cache(reinterpret_cast<char*>(result.ptr),
                                reinterpret_cast<char*>(result.ptr + result.code_size));
  #endif
#endif
        return result;
    }

private:
    void move_from(ExecCode& other) noexcept {
        ptr = other.ptr;
        size = other.size;
        code_size = other.code_size;
#ifdef _WIN32
        runtime_function = other.runtime_function;
        runtime_function_registered = other.runtime_function_registered;
        other.runtime_function = nullptr;
        other.runtime_function_registered = false;
#endif
        other.ptr = nullptr;
        other.size = 0;
        other.code_size = 0;
    }

#ifdef _WIN32
    struct UnwindAction {
        uint8_t code_offset = 0;
        uint8_t operation = 0;
        uint8_t info = 0;
        std::vector<uint16_t> extra_slots;
    };

    static size_t align_up(size_t value, size_t alignment) {
        if (value > std::numeric_limits<size_t>::max() - (alignment - 1))
            throw std::runtime_error("ExecCode: alignment overflow");
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static bool is_win64_nonvolatile_gpr(uint8_t reg) noexcept {
        return reg == 3 || reg == 5 || reg == 6 || reg == 7 ||
               (reg >= 12 && reg <= 15);
    }

    static std::vector<uint8_t> encode_win64_unwind(const Win64UnwindSpec& spec,
                                                     size_t function_size) {
        if (function_size > std::numeric_limits<DWORD>::max())
            throw std::runtime_error("ExecCode: function exceeds Win64 unwind RVA range");
        if (spec.prolog_size == 0 || spec.prolog_size > function_size)
            throw std::runtime_error("ExecCode: invalid Win64 prologue size");

        std::vector<UnwindAction> actions;
        actions.reserve(spec.pushed_nonvolatiles.size() +
                        (spec.stack_allocation_bytes != 0 ? 1 : 0));
        std::unordered_set<uint8_t> pushed;
        std::unordered_set<uint8_t> action_offsets;
        for (const auto& push : spec.pushed_nonvolatiles) {
            if (push.code_offset == 0 || push.code_offset > spec.prolog_size ||
                !is_win64_nonvolatile_gpr(push.register_number))
                throw std::runtime_error("ExecCode: invalid Win64 nonvolatile push");
            if (!pushed.insert(push.register_number).second)
                throw std::runtime_error("ExecCode: duplicate Win64 nonvolatile push");
            if (!action_offsets.insert(push.code_offset).second)
                throw std::runtime_error("ExecCode: duplicate Win64 unwind code offset");
            actions.push_back({push.code_offset, 0 /* UWOP_PUSH_NONVOL */,
                               push.register_number, {}});
        }

        if (spec.stack_allocation_bytes != 0) {
            const uint32_t bytes = spec.stack_allocation_bytes;
            if (spec.stack_allocation_code_offset == 0 ||
                spec.stack_allocation_code_offset > spec.prolog_size ||
                (bytes & 7U) != 0 || bytes < 8)
                throw std::runtime_error("ExecCode: invalid Win64 stack allocation");
            if (!action_offsets.insert(spec.stack_allocation_code_offset).second)
                throw std::runtime_error("ExecCode: duplicate Win64 unwind code offset");
            UnwindAction allocation;
            allocation.code_offset = spec.stack_allocation_code_offset;
            if (bytes <= 128) {
                allocation.operation = 2; // UWOP_ALLOC_SMALL
                allocation.info = static_cast<uint8_t>((bytes - 8) / 8);
            } else if (bytes / 8 <= std::numeric_limits<uint16_t>::max()) {
                allocation.operation = 1; // UWOP_ALLOC_LARGE, scaled uint16
                allocation.info = 0;
                allocation.extra_slots.push_back(static_cast<uint16_t>(bytes / 8));
            } else {
                allocation.operation = 1; // UWOP_ALLOC_LARGE, uint32 bytes
                allocation.info = 1;
                allocation.extra_slots.push_back(static_cast<uint16_t>(bytes & 0xffffU));
                allocation.extra_slots.push_back(static_cast<uint16_t>(bytes >> 16));
            }
            actions.push_back(std::move(allocation));
        } else if (spec.stack_allocation_code_offset != 0) {
            throw std::runtime_error("ExecCode: allocation offset without stack allocation");
        }

        // Windows requires unwind operations in descending prologue offset.
        std::sort(actions.begin(), actions.end(), [](const auto& left, const auto& right) {
            return left.code_offset > right.code_offset;
        });
        size_t slot_count = 0;
        for (const auto& action : actions) slot_count += 1 + action.extra_slots.size();
        if (slot_count == 0 || slot_count > 255)
            throw std::runtime_error("ExecCode: invalid Win64 unwind code count");

        std::vector<uint8_t> encoded;
        encoded.reserve(4 + ((slot_count + 1) & ~size_t(1)) * 2);
        encoded.push_back(1); // Version=1, Flags=UNW_FLAG_NHANDLER
        encoded.push_back(spec.prolog_size);
        encoded.push_back(static_cast<uint8_t>(slot_count));
        encoded.push_back(0); // no frame pointer register
        for (const auto& action : actions) {
            encoded.push_back(action.code_offset);
            encoded.push_back(static_cast<uint8_t>((action.info << 4) | action.operation));
            for (uint16_t slot : action.extra_slots) {
                encoded.push_back(static_cast<uint8_t>(slot & 0xff));
                encoded.push_back(static_cast<uint8_t>(slot >> 8));
            }
        }
        if ((slot_count & 1U) != 0) {
            encoded.push_back(0);
            encoded.push_back(0);
        }
        return encoded;
    }
#endif
};
