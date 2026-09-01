#include "jit_target.hpp"
#include <cstring>
#include <iostream>

static int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

int main() {
    constexpr SuraJitTargetInfo info = sura_jit_target_info();
    if (!info.os || !*info.os || !info.architecture || !*info.architecture ||
        !info.abi || !*info.abi || !info.backend || !*info.backend ||
        !info.fallback || std::strcmp(info.fallback, "register-vm") != 0 ||
        !info.reason || !*info.reason) {
        return fail("incomplete JIT target metadata");
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (!info.native_supported || std::strcmp(info.backend, "x64-win64") != 0 ||
        std::strcmp(info.abi, "win64") != 0) {
        return fail("Windows x86-64 must select the x64-win64 backend");
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (!info.native_supported || std::strcmp(info.backend, "x64-sysv-baseline") != 0 ||
        std::strcmp(info.abi, "sysv-x86-64") != 0) {
        return fail("Linux x86-64 must select the x64-sysv-baseline backend");
    }
#elif (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && \
      (defined(_WIN32) || defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))) && \
      !defined(__AARCH64EB__)
    if (!info.native_supported ||
        std::strcmp(info.backend, "arm64-aapcs-baseline") != 0) {
        return fail("supported ARM64 targets must select the AAPCS64 baseline backend");
    }
#else
    if (info.native_supported || std::strcmp(info.backend, "none") != 0) {
        return fail("unsupported targets must select the register-VM fallback");
    }
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    if (std::strcmp(info.architecture, "arm64") != 0) {
        return fail("ARM64 build reported the wrong architecture");
    }
#elif defined(__x86_64__) || defined(_M_X64)
    if (std::strcmp(info.architecture, "x86-64") != 0) {
        return fail("x86-64 build reported the wrong architecture");
    }
#endif

    std::cout << "jit_target_info_test: PASS (" << info.os << ", "
              << info.architecture << ", " << info.abi << ", "
              << info.backend << ")\n";
    return 0;
}
