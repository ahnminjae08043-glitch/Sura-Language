#pragma once

// Authoritative native-JIT target description.  Keep platform detection here
// so the compiler, CLI, tests, packaging, and documentation cannot disagree
// about whether --jit emits machine code or uses the register-VM fallback.
struct SuraJitTargetInfo {
    const char* os;
    const char* architecture;
    const char* abi;
    const char* backend;
    bool native_supported;
    const char* fallback;
    const char* reason;
};

#if defined(__linux__) && defined(__x86_64__)
    #define SURA_JIT_X64_SYSV_BASELINE 1
#else
    #define SURA_JIT_X64_SYSV_BASELINE 0
#endif

// Windows x86-64 has a full-featured tier of its own; the x64 baseline is
// tried first there, entered through the Win64 convention, and only kept for
// replayable pure-numeric closures it accepts whole (see NativeCompiler).
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    #define SURA_JIT_X64_WIN64_BASELINE_FIRST 1
#else
    #define SURA_JIT_X64_WIN64_BASELINE_FIRST 0
#endif

#if (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && \
    (defined(_WIN32) || defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))) && \
    !defined(__AARCH64EB__)
    #define SURA_JIT_ARM64_BASELINE 1
#else
    #define SURA_JIT_ARM64_BASELINE 0
#endif

inline constexpr SuraJitTargetInfo sura_jit_target_info() {
#if defined(_WIN32)
    #if defined(__x86_64__) || defined(_M_X64)
        return {"windows", "x86-64", "win64", "x64-win64", true,
                "register-vm", "native x86-64 emitter available; replayable pure-numeric closures take the guarded baseline with native direct calls first"};
    #elif defined(__aarch64__) || defined(_M_ARM64)
        #if SURA_JIT_ARM64_BASELINE
            return {"windows", "arm64", "windows-arm64", "arm64-aapcs-baseline", true,
                    "register-vm", "exception-free ARM64 baseline emitter supports constants, moves, proven-numeric +, -, *, unary -, comparisons, division by a proven nonzero divisor, guarded global reads and native direct calls between pure numeric functions; unsupported bytecode falls back to the register VM"};
        #else
            return {"windows", "arm64", "windows-arm64", "none", false,
                    "register-vm", "the ARM64 baseline requires a supported little-endian target"};
        #endif
    #else
        return {"windows", "unknown", "windows", "none", false,
                "register-vm", "no native emitter for this Windows architecture"};
    #endif
#elif defined(__APPLE__) && defined(__MACH__)
    #if defined(__aarch64__) || defined(__arm64__)
        #if SURA_JIT_ARM64_BASELINE
            return {"macos", "arm64", "aapcs64", "arm64-aapcs-baseline", true,
                    "register-vm", "exception-free ARM64 baseline emitter supports constants, moves, proven-numeric +, -, *, unary -, comparisons, division by a proven nonzero divisor, guarded global reads and native direct calls between pure numeric functions; unsupported bytecode falls back to the register VM"};
        #else
            return {"macos", "arm64", "aapcs64", "none", false,
                    "register-vm", "the ARM64 baseline requires a supported little-endian target"};
        #endif
    #elif defined(__x86_64__)
        return {"macos", "x86-64", "sysv-x86-64", "none", false,
                "register-vm", "SysV x86-64 emitter is not implemented"};
    #else
        return {"macos", "unknown", "darwin", "none", false,
                "register-vm", "no native emitter for this macOS architecture"};
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__)
        #if SURA_JIT_ARM64_BASELINE
            return {"linux", "arm64", "aapcs64", "arm64-aapcs-baseline", true,
                    "register-vm", "exception-free ARM64 baseline emitter supports constants, moves, proven-numeric +, -, *, unary -, comparisons, division by a proven nonzero divisor, guarded global reads and native direct calls between pure numeric functions; unsupported bytecode falls back to the register VM"};
        #else
            return {"linux", "arm64", "aapcs64", "none", false,
                    "register-vm", "the ARM64 baseline requires a supported little-endian target"};
        #endif
    #elif defined(__x86_64__)
        return {"linux", "x86-64", "sysv-x86-64", "x64-sysv-baseline", true,
                "register-vm", "exception-free straight-line System V baseline emitter supports constants, moves, proven-numeric +, -, *, unary -, comparisons, and division by a proven nonzero divisor; unsupported bytecode falls back to the register VM"};
    #else
        return {"linux", "unknown", "sysv", "none", false,
                "register-vm", "no native emitter for this Linux architecture"};
    #endif
#else
    return {"unknown", "unknown", "unknown", "none", false,
            "register-vm", "native JIT target is not recognized"};
#endif
}
