#include "../jit_alloc.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
extern "C" __declspec(noinline) uint64_t jit_unwind_throw_helper() {
    throw std::runtime_error("jit unwind probe");
}

void append_u64(std::vector<uint8_t>& code, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        code.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

std::vector<uint8_t> throwing_function_bytes() {
    std::vector<uint8_t> code = {
        0x53,                   // push rbx       (end offset 1)
        0x41, 0x54,             // push r12       (end offset 3)
        0x41, 0x55,             // push r13       (end offset 5)
        0x48, 0x83, 0xec, 0x30, // sub rsp, 48    (end offset 9)
        0x48, 0xb8              // mov rax, imm64
    };
    append_u64(code, reinterpret_cast<uint64_t>(&jit_unwind_throw_helper));
    code.insert(code.end(), {
        0xff, 0xd0,             // call rax
        0x48, 0x83, 0xc4, 0x30, // add rsp, 48
        0x41, 0x5d,             // pop r13
        0x41, 0x5c,             // pop r12
        0x5b,                   // pop rbx
        0xc3                    // ret
    });
    return code;
}

Win64UnwindSpec fixed_frame_spec() {
    Win64UnwindSpec spec;
    spec.prolog_size = 9;
    spec.stack_allocation_code_offset = 9;
    spec.stack_allocation_bytes = 48;
    spec.pushed_nonvolatiles = {{1, 3}, {3, 12}, {5, 13}};
    return spec;
}
#endif

} // namespace

int main() {
    try {
#ifndef _WIN32
        std::cout << "jit unwind registration: skipped (non-Windows)\n";
        return 0;
#else
        std::vector<uint8_t> bytes = throwing_function_bytes();
        ExecCode code = ExecCode::from_bytes(bytes, fixed_frame_spec());
        require(code.ptr != nullptr, "executable allocation is missing");
        require(code.code_size == bytes.size(), "function extent is wrong");
        require(code.has_unwind_registration(), "dynamic function table was not registered");
        require(code.runtime_function != nullptr, "RUNTIME_FUNCTION is missing");

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION found = RtlLookupFunctionEntry(
            reinterpret_cast<DWORD64>(code.ptr + 12), &image_base, nullptr);
        require(found == code.runtime_function, "RtlLookupFunctionEntry did not find registration");
        require(image_base == reinterpret_cast<DWORD64>(code.ptr), "dynamic image base is wrong");
        require(found->BeginAddress == 0 && found->EndAddress == bytes.size(),
                "registered function range is wrong");

        const uint8_t* unwind = code.ptr + found->UnwindData;
        require((unwind[0] & 0x07) == 1 && unwind[1] == 9 && unwind[2] == 4,
                "UNWIND_INFO header is wrong");
        require(unwind[4] == 9 && unwind[5] == 0x52, "UWOP_ALLOC_SMALL is wrong");
        require(unwind[6] == 5 && unwind[7] == 0xd0, "R13 unwind code is wrong");
        require(unwind[8] == 3 && unwind[9] == 0xc0, "R12 unwind code is wrong");
        require(unwind[10] == 1 && unwind[11] == 0x30, "RBX unwind code is wrong");

        RUNTIME_FUNCTION* stable_table = code.runtime_function;
        uint8_t* stable_code = code.ptr;
        ExecCode moved(std::move(code));
        require(code.ptr == nullptr && !code.has_unwind_registration(),
                "move constructor did not empty source");
        require(moved.runtime_function == stable_table && moved.ptr == stable_code,
                "move constructor changed registered addresses");

        ExecCode assigned = ExecCode::from_bytes(bytes, fixed_frame_spec());
        DWORD64 replaced_pc = reinterpret_cast<DWORD64>(assigned.ptr + 12);
        assigned = std::move(moved);
        require(moved.ptr == nullptr && assigned.runtime_function == stable_table,
                "move assignment changed registered addresses");
        DWORD64 replaced_base = 0;
        require(RtlLookupFunctionEntry(replaced_pc, &replaced_base, nullptr) == nullptr,
                "move assignment did not retire its previous function table");
        found = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(assigned.ptr + 12),
                                       &image_base, nullptr);
        require(found == stable_table, "registration was lost after move");

        bool caught = false;
        try {
            using ProbeFn = uint64_t (*)();
            reinterpret_cast<ProbeFn>(assigned.ptr)();
        } catch (const std::runtime_error& error) {
            caught = std::string(error.what()) == "jit unwind probe";
        }
        require(caught, "C++ exception did not unwind through generated frame");

        DWORD64 retired_pc = reinterpret_cast<DWORD64>(assigned.ptr + 12);
        assigned.release();
        found = RtlLookupFunctionEntry(retired_pc, &image_base, nullptr);
        require(found == nullptr, "RtlDeleteFunctionTable did not remove registration");

        Win64UnwindSpec invalid = fixed_frame_spec();
        invalid.pushed_nonvolatiles.push_back({7, 3});
        bool invalid_rejected = false;
        try { (void)ExecCode::from_bytes({0xc3}, invalid); }
        catch (const std::runtime_error&) { invalid_rejected = true; }
        require(invalid_rejected, "invalid unwind specification was accepted");

        std::cout << "jit unwind registration: PASS\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "jit unwind registration FAILED: " << error.what() << "\n";
        return 1;
    }
}
