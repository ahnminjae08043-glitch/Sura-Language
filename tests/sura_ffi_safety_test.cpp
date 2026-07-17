#include "../sura_ffi.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int fail(const char* message, int code) {
    std::cerr << message << "\n";
    return code;
}

std::atomic<int> callback_result{0};
std::atomic<long long> callback_elapsed_ms{0};

void cross_thread_callback(SuraHandle handle) {
    const auto started = std::chrono::steady_clock::now();
    std::thread callback([handle]() {
        const int result = sura_run(handle, "callback_must_not_run is true\n");
        if (result == SURA_ERR_BUSY &&
            std::strstr(sura_last_error(handle), "busy") != nullptr) {
            callback_result.store(1, std::memory_order_release);
        } else {
            callback_result.store(-1, std::memory_order_release);
        }
    });
    callback.join();
    callback_elapsed_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count(),
        std::memory_order_release);
}

void reentrant_free_callback(SuraHandle handle) {
    sura_free(handle);
}

bool run_independent_handle_stress(int thread_count) {
    int iterations = 20;
    if (const char* raw = std::getenv("SURA_FFI_TEST_RUN_COUNT")) {
        const int parsed = std::atoi(raw);
        if (parsed >= 0 && parsed <= 1000) iterations = parsed;
    }
    std::string source =
        "counter is counter + 1\n"
        "label is seed + to_str(counter)\n";
    if (std::getenv("SURA_FFI_TEST_COUNTER_ONLY")) {
        source = "counter is counter + 1\n";
    } else if (std::getenv("SURA_FFI_TEST_LABEL_SEED")) {
        source = "counter is counter + 1\nlabel is seed\n";
    }
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve((size_t)thread_count);
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([thread_index, iterations, source, &failures]() {
            SuraHandle handle = sura_new();
            if (!handle) {
                ++failures;
                return;
            }
            const std::string seed = "worker-" + std::to_string(thread_index) + ":";
            sura_set_string(handle, "seed", seed.c_str());
            sura_set_number(handle, "counter", 0);
            for (int i = 0; i < iterations; ++i) {
                const int run_code = sura_run(handle, source.c_str());
                if (run_code != SURA_OK) {
                    ++failures;
                    break;
                }
            }
            const std::string expected = seed + std::to_string(iterations);
            const bool counter_ok = std::fabs(sura_get_number(handle, "counter") - iterations) <= 0.0001;
            bool label_ok = true;
            if (!std::getenv("SURA_FFI_TEST_COUNTER_ONLY")) {
                const std::string expected_label = std::getenv("SURA_FFI_TEST_LABEL_SEED")
                    ? seed
                    : expected;
                label_ok = expected_label == sura_get_string(handle, "label");
            }
            if (!counter_ok || !label_ok) {
                ++failures;
            }
            sura_free(handle);
        });
    }
    for (auto& worker : workers) worker.join();
    return failures.load() == 0;
}

bool run_inline_handle_stress() {
    int iterations = 20;
    if (const char* raw = std::getenv("SURA_FFI_TEST_RUN_COUNT")) {
        const int parsed = std::atoi(raw);
        if (parsed >= 0 && parsed <= 1000) iterations = parsed;
    }
    std::string source =
        "counter is counter + 1\n"
        "label is seed + to_str(counter)\n";
    if (std::getenv("SURA_FFI_TEST_COUNTER_ONLY")) {
        source = "counter is counter + 1\n";
    } else if (std::getenv("SURA_FFI_TEST_LABEL_SEED")) {
        source = "counter is counter + 1\nlabel is seed\n";
    }

    SuraHandle handle = sura_new();
    if (!handle) return false;
    const std::string seed = "inline:";
    sura_set_string(handle, "seed", seed.c_str());
    sura_set_number(handle, "counter", 0);
    for (int i = 0; i < iterations; ++i) {
        if (sura_run(handle, source.c_str()) != SURA_OK) {
            sura_free(handle);
            return false;
        }
    }
    const bool counter_ok =
        std::fabs(sura_get_number(handle, "counter") - iterations) <= 0.0001;
    bool label_ok = true;
    if (!std::getenv("SURA_FFI_TEST_COUNTER_ONLY")) {
        const std::string expected = std::getenv("SURA_FFI_TEST_LABEL_SEED")
            ? seed
            : seed + std::to_string(iterations);
        label_ok = expected == sura_get_string(handle, "label");
    }
    sura_free(handle);
    return counter_ok && label_ok;
}

} // namespace

static_assert(noexcept(sura_new()), "sura_new must be a nothrow C ABI call");
static_assert(noexcept(sura_free(nullptr)), "sura_free must be a nothrow C ABI call");
static_assert(noexcept(sura_set_number(nullptr, nullptr, 0)), "setter must be nothrow");
static_assert(noexcept(sura_set_string(nullptr, nullptr, nullptr)), "setter must be nothrow");
static_assert(noexcept(sura_set_bool(nullptr, nullptr, 0)), "setter must be nothrow");
static_assert(noexcept(sura_clear_global(nullptr, nullptr)), "clear must be nothrow");
static_assert(noexcept(sura_run(nullptr, nullptr)), "sura_run must be nothrow");
static_assert(noexcept(sura_get_string(nullptr, nullptr)), "getter must be nothrow");
static_assert(noexcept(sura_last_error(nullptr)), "error getter must be nothrow");

int main() {
    if (std::getenv("SURA_FFI_TEST_DISABLE_SHUTDOWN")) {
        sura_test_set_gc_shutdown_enabled(0);
    }
    if (std::getenv("SURA_FFI_TEST_PHASE6_ONLY")) {
        const bool passed = std::getenv("SURA_FFI_TEST_INLINE")
            ? run_inline_handle_stress()
            : run_independent_handle_stress(
                std::getenv("SURA_FFI_TEST_SINGLE_THREAD") ? 1 : 4);
        if (!passed) {
            return fail("isolated independent-handle stress failed", 90);
        }
        std::cout << "sura_ffi_safety: PASS\n";
        return 0;
    }
    std::cerr << "[ffi 1/7] ABI and strict execution\n";
    if (sura_abi_version() != 10200 || std::strcmp(sura_version(), "Sura FFI 1.2.0") != 0) {
        return fail("FFI ABI/version did not advance to 1.2.0", 1);
    }
    SuraHandle strict = sura_new();
    if (!strict) return fail("failed to create strict context", 2);

    int rc = sura_run(strict,
        "count: number is \"not a number\"\n"
        "unsafe_body_ran is true\n");
    if (rc != SURA_ERR_TYPE) {
        sura_free(strict);
        return fail("FFI did not reject a type error by default", 3);
    }
    if (sura_has(strict, "unsafe_body_ran")) {
        sura_free(strict);
        return fail("typed-invalid FFI source executed before rejection", 4);
    }
    if (std::strstr(sura_last_error(strict), "[E200]") == nullptr) {
        sura_free(strict);
        return fail("FFI type error omitted its stable diagnostic code", 5);
    }

    sura_set_legacy_types(strict, 1);
    rc = sura_run(strict,
        "legacy_count: number is \"migration-only\"\n"
        "legacy_body_ran is true\n");
    if (rc != SURA_OK || !sura_get_bool(strict, "legacy_body_ran")) {
        sura_free(strict);
        return fail("explicit FFI legacy type mode did not preserve compatibility", 6);
    }
    sura_free(strict);

    // Opaque handles are validated tokens, not raw context addresses. A stale
    // handle and a repeated free must stay harmless even after creating a new
    // context; the stale token must never alias the replacement context.
    sura_free(strict);
    if (sura_run(strict, "stale_body_ran is true\n") != SURA_ERR_INTERNAL ||
        sura_has(strict, "stale_body_ran") != 0 ||
        std::strcmp(sura_get_string(strict, "stale_body_ran"), "") != 0) {
        return fail("stale FFI handle was accepted after free", 7);
    }
    SuraHandle replacement = sura_new();
    if (!replacement || replacement == strict) {
        if (replacement) sura_free(replacement);
        return fail("FFI handle token was reused", 8);
    }
    sura_set_number(strict, "must_not_reach_replacement", 99);
    if (sura_has(replacement, "must_not_reach_replacement")) {
        sura_free(replacement);
        return fail("stale FFI token aliased a replacement context", 9);
    }
    sura_free(replacement);

    SuraHandle copied_string_context = sura_new();
    if (!copied_string_context) return fail("failed to create FFI string-lifetime context", 10);
    sura_set_string(copied_string_context, "message", "thread-local-copy");
    const char* copied_string = sura_get_string(copied_string_context, "message");
    sura_free(copied_string_context);
    if (std::strcmp(copied_string, "thread-local-copy") != 0) {
        return fail("FFI returned string was invalidated by context free", 11);
    }

    SuraHandle forged = reinterpret_cast<SuraHandle>(
        std::numeric_limits<std::uintptr_t>::max() - 1);
    sura_set_number(forged, "forged", 1);
    sura_free(forged);
    if (sura_run(forged, "forged is 2\n") != SURA_ERR_INTERNAL || sura_has(forged, "forged")) {
        return fail("forged FFI handle was accepted", 12);
    }

    std::cerr << "[ffi 2/7] transactional roots and error fallback\n";
    // A root-build allocation failure must not expose a partially updated map,
    // and no exception may cross the noexcept C ABI boundary.
    SuraHandle transactional = sura_new();
    if (!transactional) return fail("failed to create transactional context", 13);
    sura_set_string(transactional, "saved", "stable-before-failure");
    sura_test_fail_next_root_commit(1);
    sura_set_string(transactional, "saved", "must-rollback");
    const std::string setter_error = sura_last_error(transactional);
    if (setter_error.find("sura_set_string") == std::string::npos ||
        std::strcmp(sura_get_string(transactional, "saved"), "stable-before-failure") != 0) {
        sura_free(transactional);
        return fail("failed setter root commit was not transactional", 14);
    }
    sura_test_fail_next_root_commit(1);
    rc = sura_run(transactional, "saved is \"must-also-rollback\"\n");
    if (rc != SURA_ERR_INTERNAL ||
        std::strcmp(sura_get_string(transactional, "saved"), "stable-before-failure") != 0) {
        sura_free(transactional);
        return fail("failed run root commit was not transactional", 15);
    }

    sura_test_fail_next_error_store(1);
    rc = sura_run(transactional, "assert(false)\n");
    if (rc != SURA_ERR_RUNTIME ||
        std::strstr(sura_last_error(transactional), "failed to store") == nullptr) {
        sura_free(transactional);
        return fail("error-record allocation fallback escaped or lost diagnostics", 16);
    }
    sura_free(transactional);

    std::cerr << "[ffi 3/7] cross-thread callback busy rejection\n";
    // A native callback that joins a different host thread must receive BUSY
    // immediately when that thread re-enters the same active handle.
    SuraHandle callback_handle = sura_new();
    if (!callback_handle) return fail("failed to create callback context", 17);
    callback_result.store(0, std::memory_order_release);
    callback_elapsed_ms.store(0, std::memory_order_release);
    sura_test_set_run_hook(cross_thread_callback);
    rc = sura_run(callback_handle, "callback_outer_completed is true\n");
    if (rc != SURA_OK || callback_result.load(std::memory_order_acquire) != 1 ||
        callback_elapsed_ms.load(std::memory_order_acquire) >= 1000 ||
        !sura_get_bool(callback_handle, "callback_outer_completed") ||
        sura_has(callback_handle, "callback_must_not_run")) {
        sura_free(callback_handle);
        return fail("cross-thread callback re-entry did not reject promptly", 18);
    }
    sura_free(callback_handle);

    std::cerr << "[ffi 4/7] callback free deferral\n";
    // Same-thread reentrant free marks the handle closed but defers destruction
    // until the outer run releases its context lease.
    SuraHandle reentrant = sura_new();
    if (!reentrant) return fail("failed to create reentrant-free context", 19);
    sura_test_set_run_hook(reentrant_free_callback);
    rc = sura_run(reentrant, "outer_survived_reentrant_free is true\n");
    if (rc != SURA_OK || sura_run(reentrant, "must_not_run is true\n") != SURA_ERR_INTERNAL) {
        return fail("reentrant free did not defer context destruction safely", 20);
    }
    sura_free(reentrant);

    std::cerr << "[ffi 5/7] free/use race\n";
    SuraHandle racing = sura_new();
    if (!racing) return fail("failed to create FFI lifetime-race context", 21);
    sura_set_number(racing, "value", 1);
    std::atomic<bool> race_started{false};
    std::thread lifetime_reader([&]() {
        race_started.store(true, std::memory_order_release);
        for (int i = 0; i < 2000; ++i) {
            (void)sura_has(racing, "value");
            (void)sura_get_number(racing, "value");
        }
    });
    while (!race_started.load(std::memory_order_acquire)) std::this_thread::yield();
    sura_free(racing);
    lifetime_reader.join();
    sura_free(racing);
    if (sura_run(racing, "value is 2\n") != SURA_ERR_INTERNAL) {
        return fail("freed FFI handle survived lifetime race", 22);
    }

    std::cerr << "[ffi 6/7] independent-handle concurrency\n";
    // Exercise independent handles from multiple host threads. The current heap
    // is process-global, so the C ABI serializes complete VM operations while
    // retaining per-context state and roots.
    if (!run_independent_handle_stress(4)) return fail("parallel FFI handle stress failed", 23);

    std::cerr << "[ffi 7/7] complete\n";
    std::cout << "sura_ffi_safety: PASS\n";
    return 0;
}
