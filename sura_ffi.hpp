#pragma once
// ================================================================
//  Sura FFI — C ABI entrypoints for Unity / other C# hosts.
//  Callers interact via opaque SuraHandle.  All functions are
//  nothrow: errors surface via sura_last_error().
// ================================================================

#ifdef __cplusplus
  #define SURA_EXTERN_C extern "C"
  #define SURA_NOEXCEPT noexcept
#else
  #define SURA_EXTERN_C extern
  #define SURA_NOEXCEPT
#endif

#ifdef _WIN32
  #ifdef SURA_FFI_BUILD
    #define SURA_EXPORT __declspec(dllexport)
  #else
    #define SURA_EXPORT
  #endif
#else
  #define SURA_EXPORT __attribute__((visibility("default")))
#endif

#define SURA_API SURA_EXTERN_C SURA_EXPORT

typedef void* SuraHandle;

#define SURA_FFI_ABI_VERSION_MAJOR 1
#define SURA_FFI_ABI_VERSION_MINOR 2
#define SURA_FFI_ABI_VERSION_PATCH 0
#define SURA_FFI_ABI_VERSION ((SURA_FFI_ABI_VERSION_MAJOR * 10000) + (SURA_FFI_ABI_VERSION_MINOR * 100) + SURA_FFI_ABI_VERSION_PATCH)

// Return codes for sura_run
enum SuraResult {
    SURA_OK           =  0,
    SURA_ERR_LEX      = -1,
    SURA_ERR_PARSE    = -2,
    SURA_ERR_RUNTIME  = -3,
    SURA_ERR_TYPE     = -4,
    SURA_ERR_BUSY     = -5,
    SURA_ERR_INTERNAL = -9
};

// ── Lifecycle ──────────────────────────────────────────────────
SURA_API SuraHandle sura_new(void) SURA_NOEXCEPT;
SURA_API void       sura_free(SuraHandle h) SURA_NOEXCEPT;

// ── Set inputs (called before sura_run) ────────────────────────
SURA_API void sura_set_number(SuraHandle h, const char* name, double v) SURA_NOEXCEPT;
SURA_API void sura_set_string(SuraHandle h, const char* name, const char* v) SURA_NOEXCEPT;
SURA_API void sura_set_bool  (SuraHandle h, const char* name, int v) SURA_NOEXCEPT;
SURA_API void sura_clear_global(SuraHandle h, const char* name) SURA_NOEXCEPT;

// Type errors stop execution by default. Set enabled=1 only while migrating a
// legacy script that needs the former warning-and-run behavior.
SURA_API void sura_set_legacy_types(SuraHandle h, int enabled) SURA_NOEXCEPT;

// ── Execute ────────────────────────────────────────────────────
// Returns SuraResult.  On error, call sura_last_error().
// The process-global GC heap serializes execution across independent handles.
// A second host thread calling the same handle while its run is active returns
// SURA_ERR_BUSY (or the documented default for non-int getters), rather than
// waiting behind a native callback that may be joining that thread.
// sura_last_error() reports the busy condition on the rejected host thread.
// Globals that contain only primitive values, strings, tensors, arrays and
// dictionaries of those values persist across calls on the same handle.
// Functions, captured upvalues and class instances are tied to one execution
// image and are omitted from the persistent globals after a successful run.
// If a failed run inserts one of those run-bound values into an existing
// persistent container, the handle's persistent globals are cleared.
SURA_API int sura_run(SuraHandle h, const char* source) SURA_NOEXCEPT;

// ── Read outputs (after sura_run) ──────────────────────────────
// sura_has returns 1 if the global exists, 0 otherwise.
SURA_API int         sura_has(SuraHandle h, const char* name) SURA_NOEXCEPT;
SURA_API double      sura_get_number(SuraHandle h, const char* name) SURA_NOEXCEPT;
SURA_API int         sura_get_bool  (SuraHandle h, const char* name) SURA_NOEXCEPT;
// Returned string pointers use a thread-local copy and remain valid until the
// next sura_get_string/sura_last_error call on the same host thread. Public
// entry points validate opaque handle tokens and acquire a context lease under
// the registry lock, so stale, duplicate-free, and free/call races do not
// dereference released context memory. Calls that lose a race with sura_free
// return their documented empty/default/error value.
SURA_API const char* sura_get_string(SuraHandle h, const char* name) SURA_NOEXCEPT;

// ── Error reporting ────────────────────────────────────────────
// Returns last error message; empty string if no error.
// Pointer valid until the next string/error getter on the same host thread.
SURA_API const char* sura_last_error(SuraHandle h) SURA_NOEXCEPT;

// ── Version ────────────────────────────────────────────────────
SURA_API int         sura_abi_version(void) SURA_NOEXCEPT;
SURA_API const char* sura_version(void) SURA_NOEXCEPT;

// Test-only fault/callback hooks. They are absent from production builds.
#ifdef SURA_FFI_TESTING
typedef void (*SuraFfiTestRunHook)(SuraHandle);
SURA_API void sura_test_set_run_hook(SuraFfiTestRunHook hook) SURA_NOEXCEPT;
SURA_API void sura_test_fail_next_root_commit(int enabled) SURA_NOEXCEPT;
SURA_API void sura_test_fail_next_error_store(int enabled) SURA_NOEXCEPT;
SURA_API void sura_test_set_gc_shutdown_enabled(int enabled) SURA_NOEXCEPT;
#endif
