// ================================================================
//  Sura FFI implementation
// ================================================================
#define SURA_FFI_BUILD 1
#include "sura_ffi.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "typechecker.hpp"
#include "jit.hpp"
#include "value.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

using GlobalMap = std::unordered_map<std::string, Value>;
using RootList = std::vector<std::unique_ptr<GCNativeRoot>>;

#ifdef SURA_FFI_TESTING
std::atomic<SuraFfiTestRunHook> test_run_hook{nullptr};
std::atomic<bool> test_fail_root_commit{false};
std::atomic<bool> test_fail_error_store{false};
std::atomic<bool> test_gc_shutdown_enabled{true};
#endif

struct SuraContext {
    std::recursive_mutex mutex;
    GlobalMap user_globals;
    std::string last_error;
    bool legacy_types = false;
    RootList persistent_roots;

    static RootList roots_for(const GlobalMap& globals) {
#ifdef SURA_FFI_TESTING
        if (test_fail_root_commit.exchange(false, std::memory_order_acq_rel)) {
            throw std::bad_alloc();
        }
#endif
        RootList roots;
        roots.reserve(globals.size());
        for (const auto& entry : globals) {
            const Value& value = entry.second;
            if (value.is_obj()) {
                roots.emplace_back(std::make_unique<GCNativeRoot>(value.as_obj()));
            }
        }
        return roots;
    }

    // Build every fallible object and root before changing the visible map.
    // If allocation fails, the old globals and old roots remain paired.
    void commit_globals(GlobalMap next_globals) {
        RootList next_roots = roots_for(next_globals);
        user_globals.swap(next_globals);
        persistent_roots.swap(next_roots);
    }

    // Used only when an interrupted run has inserted VM-owned executable
    // objects into an already-persistent container. Clearing both collections
    // is allocation-free and prevents a later JitChunk from interpreting a
    // closure/function index that belongs to the destroyed chunk.
    void clear_persistent_state() noexcept {
        user_globals.clear();
        persistent_roots.clear();
    }
};

// Values backed only by process-owned storage can survive across sura_run()
// calls. Closures, upvalues and instances contain pointers or indices owned by
// the JitChunk/JitVM for one run, so they must never enter the persistent map.
// Arrays and dictionaries are checked transitively and cycle-safely. Tensors
// contain process-owned buffers and typed tensor-parent edges, not JitChunk
// metadata, so they are persistent values.
class PersistentValueClassifier {
    static constexpr size_t kVisitLimit = 1'000'000;
    size_t visits_ = 0;
    std::unordered_set<GCObject*> known_safe_;

public:
    bool is_safe(const Value& value) {
        if (!value.is_obj()) return true;
        GCObject* root = value.as_obj();
        if (!root) return true;
        if (known_safe_.find(root) != known_safe_.end()) return true;

        std::vector<GCObject*> pending;
        std::unordered_set<GCObject*> seen;
        pending.push_back(root);

        while (!pending.empty()) {
            GCObject* object = pending.back();
            pending.pop_back();
            if (!object || known_safe_.find(object) != known_safe_.end()) continue;
            if (!seen.insert(object).second) continue;
            if (++visits_ > kVisitLimit) {
                throw std::length_error("FFI persistent value graph exceeds 1000000 objects");
            }

            switch (object->obj_type) {
                case ObjType::STRING:
                case ObjType::TENSOR:
                    break;
                case ObjType::ARRAY: {
                    auto* array = static_cast<GCArray*>(object);
                    for (const Value& child : array->elements) {
                        if (child.is_obj()) pending.push_back(child.as_obj());
                    }
                    break;
                }
                case ObjType::DICT: {
                    auto* dict = static_cast<GCDict*>(object);
                    for (const auto& entry : dict->elements) {
                        if (entry.second.is_obj()) pending.push_back(entry.second.as_obj());
                    }
                    break;
                }
                case ObjType::FUNC:
                case ObjType::UPVALUE:
                case ObjType::INSTANCE:
                    return false;
                default:
                    return false;
            }
        }

        // The graph was fully visited without finding VM-owned metadata.
        // Caching every visited node also makes shared/cyclic containers linear
        // in the total number of distinct objects for an ordinary FFI commit.
        known_safe_.insert(seen.begin(), seen.end());
        return true;
    }
};

bool contains_run_bound_globals(const GlobalMap& globals) {
    PersistentValueClassifier classifier;
    for (const auto& entry : globals) {
        if (!classifier.is_safe(entry.second)) return true;
    }
    return false;
}

size_t remove_run_bound_globals(GlobalMap& globals) {
    PersistentValueClassifier classifier;
    size_t removed = 0;
    for (auto it = globals.begin(); it != globals.end();) {
        if (!classifier.is_safe(it->second)) {
            it = globals.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void quarantine_failed_run_state(SuraContext& context) noexcept {
    try {
        if (contains_run_bound_globals(context.user_globals)) {
            context.clear_persistent_state();
        }
    } catch (...) {
        // If validation itself cannot allocate its bounded work lists, keeping
        // a possibly contaminated graph would be unsafe. Fail closed.
        context.clear_persistent_state();
    }
}

struct ContextControl {
    std::unique_ptr<SuraContext> context;
    std::uintptr_t token = 0;
    size_t active_calls = 0;
    bool closing = false;
    bool run_active = false;
    std::thread::id run_owner;
};

struct RegistryState {
    std::mutex mutex;
    std::unordered_map<std::uintptr_t, std::shared_ptr<ContextControl>> contexts;
    std::uintptr_t next_token = 1;
    size_t process_active_runs = 0;
    size_t finalizers_in_progress = 0;
    bool shutdown_disabled = false;
};

RegistryState& registry() {
    // The registry intentionally outlives ordinary static destruction. DLL or
    // process teardown must not race a late thread-local/error destructor.
    static auto* state = new RegistryState();
    return *state;
}

struct ThreadStatus {
    std::uintptr_t token = 0;
    bool present = false;
    char message[768]{};
};

thread_local ThreadStatus ffi_thread_status;

// A returned C string must survive until this host thread's next string/error
// getter. MinGW's non-trivial C++ thread_local destructor registration is not
// reliable when several short-lived native threads initialize and exit at the
// same time, so use the OS thread-exit facility to own the std::string.
#ifdef _WIN32
void CALLBACK destroy_return_buffer(void* value) noexcept {
    delete static_cast<std::string*>(value);
}

DWORD return_buffer_key() noexcept {
    // The process owns the FLS key until exit. FlsFree here would race worker
    // callbacks during DLL/process teardown; Windows releases the key itself.
    static const DWORD key = FlsAlloc(destroy_return_buffer);
    return key;
}

std::string* return_buffer_for_current_thread() noexcept {
    const DWORD key = return_buffer_key();
    if (key == FLS_OUT_OF_INDEXES) return nullptr;
    auto* buffer = static_cast<std::string*>(FlsGetValue(key));
    if (buffer) return buffer;
    buffer = new (std::nothrow) std::string();
    if (!buffer) return nullptr;
    if (!FlsSetValue(key, buffer)) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}
#else
pthread_key_t ffi_return_buffer_key;
pthread_once_t ffi_return_buffer_key_once = PTHREAD_ONCE_INIT;
std::atomic<bool> ffi_return_buffer_key_ready{false};

void destroy_return_buffer(void* value) noexcept {
    delete static_cast<std::string*>(value);
}

void initialize_return_buffer_key() noexcept {
    if (pthread_key_create(&ffi_return_buffer_key, destroy_return_buffer) == 0) {
        ffi_return_buffer_key_ready.store(true, std::memory_order_release);
    }
}

std::string* return_buffer_for_current_thread() noexcept {
    if (pthread_once(&ffi_return_buffer_key_once, initialize_return_buffer_key) != 0 ||
        !ffi_return_buffer_key_ready.load(std::memory_order_acquire)) {
        return nullptr;
    }
    auto* buffer = static_cast<std::string*>(pthread_getspecific(ffi_return_buffer_key));
    if (buffer) return buffer;
    buffer = new (std::nothrow) std::string();
    if (!buffer) return nullptr;
    if (pthread_setspecific(ffi_return_buffer_key, buffer) != 0) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}
#endif

std::uintptr_t token_of(SuraHandle handle) noexcept {
    return reinterpret_cast<std::uintptr_t>(handle);
}

void clear_thread_error(std::uintptr_t token) noexcept {
    if (ffi_thread_status.present && ffi_thread_status.token == token) {
        ffi_thread_status.present = false;
        ffi_thread_status.message[0] = '\0';
    }
}

void record_thread_error(std::uintptr_t token, const char* message,
                         const char* detail = nullptr) noexcept {
    ffi_thread_status.token = token;
    ffi_thread_status.present = true;
    if (!message) message = "Sura FFI internal error";
    if (detail && *detail) {
        std::snprintf(ffi_thread_status.message, sizeof(ffi_thread_status.message),
                      "%s: %s", message, detail);
    } else {
        std::snprintf(ffi_thread_status.message, sizeof(ffi_thread_status.message),
                      "%s", message);
    }
    ffi_thread_status.message[sizeof(ffi_thread_status.message) - 1] = '\0';
}

const char* recorded_thread_error(std::uintptr_t token) noexcept {
    return ffi_thread_status.present && ffi_thread_status.token == token
        ? ffi_thread_status.message
        : nullptr;
}

void record_current_exception(std::uintptr_t token, const char* operation) noexcept {
    try {
        throw;
    } catch (const std::exception& error) {
        record_thread_error(token, operation, error.what());
    } catch (...) {
        record_thread_error(token, operation, "unknown native exception");
    }
}

void store_context_error(SuraContext& context, std::uintptr_t token,
                         const char* prefix, const char* detail,
                         int line = 0, bool include_line = false) noexcept {
    try {
#ifdef SURA_FFI_TESTING
        if (test_fail_error_store.exchange(false, std::memory_order_acq_rel)) {
            throw std::bad_alloc();
        }
#endif
        std::string text = prefix ? prefix : "[Internal]";
        if (detail && *detail) {
            text += " ";
            text += detail;
        }
        if (include_line) {
            text += " (line ";
            text += std::to_string(line);
            text += ")";
        }
        context.last_error = std::move(text);
        clear_thread_error(token);
    } catch (...) {
        record_thread_error(token,
            "Sura FFI failed to store the detailed error; allocation unavailable");
    }
}

enum class LeaseResult { Ok, Invalid, Busy };

class ContextLease;
void release_lease(ContextLease& lease) noexcept;
void collect_retired_contexts() noexcept;

class ContextLease {
    friend void release_lease(ContextLease&) noexcept;
    std::shared_ptr<ContextControl> control_;
    bool run_ = false;

public:
    ContextLease() = default;
    ContextLease(std::shared_ptr<ContextControl> control, bool run)
        : control_(std::move(control)), run_(run) {}
    ContextLease(const ContextLease&) = delete;
    ContextLease& operator=(const ContextLease&) = delete;
    ContextLease(ContextLease&& other) noexcept
        : control_(std::move(other.control_)), run_(other.run_) {
        other.run_ = false;
    }
    ContextLease& operator=(ContextLease&& other) noexcept {
        if (this == &other) return *this;
        release_lease(*this);
        control_ = std::move(other.control_);
        run_ = other.run_;
        other.run_ = false;
        return *this;
    }
    ~ContextLease() { release_lease(*this); }

    explicit operator bool() const noexcept {
        return control_ && control_->context;
    }
    SuraContext* context() const noexcept {
        return control_ ? control_->context.get() : nullptr;
    }
    std::uintptr_t token() const noexcept {
        return control_ ? control_->token : 0;
    }
};

LeaseResult acquire_lease(SuraHandle handle, bool begin_run,
                          ContextLease& lease) {
    const std::uintptr_t token = token_of(handle);
    RegistryState& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.contexts.find(token);
    if (found == state.contexts.end() || found->second->closing ||
        !found->second->context) {
        return LeaseResult::Invalid;
    }

    const std::thread::id caller = std::this_thread::get_id();
    ContextControl& control = *found->second;
    if (begin_run) {
        if (control.active_calls != 0 || control.run_active) {
            return LeaseResult::Busy;
        }
        ++state.process_active_runs;
        control.run_active = true;
        control.run_owner = caller;
    } else if (control.run_active && control.run_owner != caller) {
        // Do not wait behind a run: it may currently be inside a native callback
        // that spawned and joined this caller.
        return LeaseResult::Busy;
    }

    ++control.active_calls;
    lease = ContextLease(found->second, begin_run);
    return LeaseResult::Ok;
}

std::unique_ptr<SuraContext> detach_if_retired_locked(
        RegistryState& state, const std::shared_ptr<ContextControl>& control) noexcept {
    if (!control || !control->closing || control->active_calls != 0 ||
        !control->context || state.process_active_runs != 0) {
        return nullptr;
    }
    std::unique_ptr<SuraContext> context = std::move(control->context);
    ++state.finalizers_in_progress;
    state.contexts.erase(control->token);
    return context;
}

void finalize_context(std::unique_ptr<SuraContext> context) noexcept {
    if (!context) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        context.reset();

        bool shutdown = false;
        {
            RegistryState& state = registry();
            std::lock_guard<std::mutex> registry_lock(state.mutex);
            if (state.finalizers_in_progress != 0) --state.finalizers_in_progress;
            shutdown = state.contexts.empty() && state.finalizers_in_progress == 0 &&
                       !state.shutdown_disabled;
        }
        // A context registered after the empty check cannot enter the GC lock
        // until this shutdown finishes, and it owns no GC objects yet.
        if (shutdown) {
#ifdef SURA_FFI_TESTING
            if (test_gc_shutdown_enabled.load(std::memory_order_acquire)) GC::shutdown();
#else
            GC::shutdown();
#endif
        }
    } catch (...) {
        // A synchronization primitive failure leaves no safe teardown path.
        // Leak the context/heap instead of freeing objects without the GC lock.
        (void)context.release();
        try {
            RegistryState& state = registry();
            std::lock_guard<std::mutex> registry_lock(state.mutex);
            state.shutdown_disabled = true;
        } catch (...) {
        }
    }
}

void collect_retired_contexts() noexcept {
    while (true) {
        std::unique_ptr<SuraContext> retired;
        try {
            RegistryState& state = registry();
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.process_active_runs != 0) return;
                for (auto it = state.contexts.begin(); it != state.contexts.end(); ++it) {
                    const auto& control = it->second;
                    if (!control->closing || control->active_calls != 0 || !control->context) {
                        continue;
                    }
                    retired = std::move(control->context);
                    ++state.finalizers_in_progress;
                    state.contexts.erase(it);
                    break;
                }
            }
            if (!retired) return;
            finalize_context(std::move(retired));
        } catch (...) {
            // Retaining a closed context is safe; a future release/free can
            // retry collection. Never let cleanup escape a C ABI call.
            return;
        }
    }
}

void release_lease(ContextLease& lease) noexcept {
    if (!lease.control_) return;
    try {
        RegistryState& state = registry();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ContextControl& control = *lease.control_;
            if (control.active_calls != 0) --control.active_calls;
            if (lease.run_) {
                control.run_active = false;
                control.run_owner = std::thread::id();
                if (state.process_active_runs != 0) --state.process_active_runs;
            }
        }
    } catch (...) {
        // The registry still owns every non-finalized control. If accounting
        // itself fails, retain that registry ownership rather than risking an
        // unlocked destruction path.
    }
    lease.run_ = false;
    lease.control_.reset();
    collect_retired_contexts();
}

LeaseResult acquire_or_record(SuraHandle handle, bool begin_run,
                              const char* operation, ContextLease& lease) noexcept {
    const std::uintptr_t token = token_of(handle);
    try {
        LeaseResult result = acquire_lease(handle, begin_run, lease);
        if (result == LeaseResult::Invalid) {
            record_thread_error(token, "Sura FFI invalid or closed handle", operation);
        } else if (result == LeaseResult::Busy) {
            record_thread_error(token, "Sura FFI runtime is busy on another host thread", operation);
        }
        return result;
    } catch (...) {
        record_current_exception(token, operation);
        return LeaseResult::Invalid;
    }
}

std::string type_error_text(const TypeChecker& checker) {
    std::ostringstream out;
    out << "[Type] " << checker.get_errors().size() << " type error(s)";
    if (!checker.get_errors().empty()) {
        const auto& first = checker.get_errors().front();
        out << "; line " << first.line << ": " << first.message;
    }
    return out.str();
}

template <typename Mutation>
void mutate_globals_transactionally(SuraContext& context, Mutation&& mutation) {
    GlobalMap next = context.user_globals;
    mutation(next);
    context.commit_globals(std::move(next));
}

} // namespace

SURA_API SuraHandle sura_new() SURA_NOEXCEPT {
    try {
        auto context = std::make_unique<SuraContext>();
        auto control = std::make_shared<ContextControl>();
        control->context = std::move(context);

        RegistryState& state = registry();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.next_token == 0 ||
            state.next_token == std::numeric_limits<std::uintptr_t>::max()) {
            record_thread_error(0, "Sura FFI handle token space exhausted");
            return nullptr;
        }
        const std::uintptr_t token = state.next_token++;
        control->token = token;
        auto inserted = state.contexts.emplace(token, control);
        if (!inserted.second) {
            record_thread_error(0, "Sura FFI handle registry collision");
            return nullptr;
        }
        clear_thread_error(0);
        return reinterpret_cast<SuraHandle>(token);
    } catch (...) {
        record_current_exception(0, "sura_new");
        return nullptr;
    }
}

SURA_API void sura_free(SuraHandle handle) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle) return;
    try {
        std::unique_ptr<SuraContext> retired;
        {
            RegistryState& state = registry();
            std::lock_guard<std::mutex> lock(state.mutex);
            auto found = state.contexts.find(token);
            if (found == state.contexts.end() || found->second->closing) return;
            found->second->closing = true;
            retired = detach_if_retired_locked(state, found->second);
        }
        clear_thread_error(token);
        finalize_context(std::move(retired));
        collect_retired_contexts();
    } catch (...) {
        record_current_exception(token, "sura_free");
    }
}

SURA_API void sura_set_number(SuraHandle handle, const char* name, double value) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_set_number received a null handle or name");
        return;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_set_number", lease) != LeaseResult::Ok) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        mutate_globals_transactionally(*lease.context(), [&](GlobalMap& next) {
            next.insert_or_assign(name, Value(value));
        });
        clear_thread_error(token);
    } catch (...) {
        record_current_exception(token, "sura_set_number");
    }
}

SURA_API void sura_set_string(SuraHandle handle, const char* name, const char* value) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_set_string received a null handle or name");
        return;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_set_string", lease) != LeaseResult::Ok) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        Value next_value(std::string(value ? value : ""));
        mutate_globals_transactionally(*lease.context(), [&](GlobalMap& next) {
            next.insert_or_assign(name, next_value);
        });
        clear_thread_error(token);
    } catch (...) {
        record_current_exception(token, "sura_set_string");
    }
}

SURA_API void sura_set_bool(SuraHandle handle, const char* name, int value) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_set_bool received a null handle or name");
        return;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_set_bool", lease) != LeaseResult::Ok) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        mutate_globals_transactionally(*lease.context(), [&](GlobalMap& next) {
            next.insert_or_assign(name, Value(value != 0));
        });
        clear_thread_error(token);
    } catch (...) {
        record_current_exception(token, "sura_set_bool");
    }
}

SURA_API void sura_clear_global(SuraHandle handle, const char* name) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_clear_global received a null handle or name");
        return;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_clear_global", lease) != LeaseResult::Ok) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        mutate_globals_transactionally(*lease.context(), [&](GlobalMap& next) {
            next.erase(name);
        });
        clear_thread_error(token);
    } catch (...) {
        record_current_exception(token, "sura_clear_global");
    }
}

SURA_API void sura_set_legacy_types(SuraHandle handle, int enabled) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle) {
        record_thread_error(token, "sura_set_legacy_types received a null handle");
        return;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_set_legacy_types", lease) != LeaseResult::Ok) return;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        lease.context()->legacy_types = enabled != 0;
        clear_thread_error(token);
    } catch (...) {
        record_current_exception(token, "sura_set_legacy_types");
    }
}

SURA_API int sura_run(SuraHandle handle, const char* source) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !source) {
        record_thread_error(token, "sura_run received a null handle or source");
        return SURA_ERR_INTERNAL;
    }
    ContextLease lease;
    LeaseResult acquired = acquire_or_record(handle, true, "sura_run", lease);
    if (acquired == LeaseResult::Busy) return SURA_ERR_BUSY;
    if (acquired != LeaseResult::Ok) return SURA_ERR_INTERNAL;

    try {
        // The lease is declared before both locks, so locks are released before
        // lease teardown can finalize a reentrantly closed context.
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        SuraContext& context = *lease.context();
        context.last_error.clear();
        clear_thread_error(token);

        try {
            if (contains_run_bound_globals(context.user_globals)) {
                context.clear_persistent_state();
                store_context_error(context, token, "[Internal]",
                    "run-bound FFI globals from an earlier execution were quarantined");
                return SURA_ERR_INTERNAL;
            }
        } catch (...) {
            context.clear_persistent_state();
            store_context_error(context, token, "[Internal]",
                "failed to validate the persistent FFI value graph; globals were cleared");
            return SURA_ERR_INTERNAL;
        }

#ifdef SURA_FFI_TESTING
        if (SuraFfiTestRunHook hook = test_run_hook.exchange(nullptr, std::memory_order_acq_rel)) {
            hook(handle);
        }
#endif

        try {
            Parser parser;
            auto ast = parser.parse_source(source);

            TypeChecker checker;
            if (checker.check(ast.get()) > 0 && !context.legacy_types) {
                try {
                    context.last_error = type_error_text(checker);
                    clear_thread_error(token);
                } catch (...) {
                    store_context_error(context, token, "[Internal]",
                        "failed to format type diagnostics");
                }
                return SURA_ERR_TYPE;
            }

            JitCompiler compiler;
            JitChunk chunk = compiler.compile(ast.get());
            JitVM vm;
            std::vector<Value> preload(chunk.global_names.size(), Value::nil());
            for (size_t i = 0; i < chunk.global_names.size(); ++i) {
                auto it = context.user_globals.find(chunk.global_names[i]);
                if (it != context.user_globals.end()) preload[i] = it->second;
            }
            vm.inject_globals(preload);
            vm.run(chunk);

            // Commit script outputs and their roots atomically while the VM is
            // still alive and therefore still roots every candidate object.
            GlobalMap next_globals = context.user_globals;
            const auto& after = vm.snapshot_globals();
            for (size_t i = 0; i < chunk.global_names.size() && i < after.size(); ++i) {
                next_globals.insert_or_assign(chunk.global_names[i], after[i]);
            }
            // Function declarations and class instances remain usable for the
            // duration of this run. They are omitted from cross-run globals
            // because their executable metadata belongs to `chunk`.
            remove_run_bound_globals(next_globals);
            context.commit_globals(std::move(next_globals));
            clear_thread_error(token);
            return SURA_OK;
        } catch (const LexError& error) {
            quarantine_failed_run_state(context);
            store_context_error(context, token, "[Lex]", error.what());
            return SURA_ERR_LEX;
        } catch (const ParseError& error) {
            quarantine_failed_run_state(context);
            store_context_error(context, token, "[Parse]", error.what());
            return SURA_ERR_PARSE;
        } catch (const JitThrow& error) {
            quarantine_failed_run_state(context);
            store_context_error(context, token, "[Runtime]", error.message.c_str(),
                                error.line, true);
            return SURA_ERR_RUNTIME;
        } catch (const std::exception& error) {
            quarantine_failed_run_state(context);
            store_context_error(context, token, "[Internal]", error.what());
            return SURA_ERR_INTERNAL;
        } catch (...) {
            quarantine_failed_run_state(context);
            store_context_error(context, token, "[Internal]", "unknown native exception");
            return SURA_ERR_INTERNAL;
        }
    } catch (...) {
        record_current_exception(token, "sura_run boundary");
        return SURA_ERR_INTERNAL;
    }
}

SURA_API int sura_has(SuraHandle handle, const char* name) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_has received a null handle or name");
        return 0;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_has", lease) != LeaseResult::Ok) return 0;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        const int found = lease.context()->user_globals.find(name) !=
                          lease.context()->user_globals.end() ? 1 : 0;
        clear_thread_error(token);
        return found;
    } catch (...) {
        record_current_exception(token, "sura_has");
        return 0;
    }
}

SURA_API double sura_get_number(SuraHandle handle, const char* name) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_get_number received a null handle or name");
        return 0.0;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_get_number", lease) != LeaseResult::Ok) return 0.0;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        auto it = lease.context()->user_globals.find(name);
        const double value = it == lease.context()->user_globals.end() ? 0.0 : it->second.to_num();
        clear_thread_error(token);
        return value;
    } catch (...) {
        record_current_exception(token, "sura_get_number");
        return 0.0;
    }
}

SURA_API int sura_get_bool(SuraHandle handle, const char* name) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_get_bool received a null handle or name");
        return 0;
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_get_bool", lease) != LeaseResult::Ok) return 0;
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        auto it = lease.context()->user_globals.find(name);
        const int value = it == lease.context()->user_globals.end() ? 0 : (it->second.truthy() ? 1 : 0);
        clear_thread_error(token);
        return value;
    } catch (...) {
        record_current_exception(token, "sura_get_bool");
        return 0;
    }
}

SURA_API const char* sura_get_string(SuraHandle handle, const char* name) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (!handle || !name) {
        record_thread_error(token, "sura_get_string received a null handle or name");
        return "";
    }
    ContextLease lease;
    if (acquire_or_record(handle, false, "sura_get_string", lease) != LeaseResult::Ok) return "";
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        auto it = lease.context()->user_globals.find(name);
        std::string* return_buffer = return_buffer_for_current_thread();
        if (!return_buffer) {
            record_thread_error(token, "sura_get_string could not allocate its thread return buffer");
            return "";
        }
        *return_buffer = it == lease.context()->user_globals.end()
            ? std::string()
            : it->second.to_str();
        clear_thread_error(token);
        return return_buffer->c_str();
    } catch (...) {
        record_current_exception(token, "sura_get_string");
        return "";
    }
}

SURA_API const char* sura_last_error(SuraHandle handle) SURA_NOEXCEPT {
    const std::uintptr_t token = token_of(handle);
    if (const char* recorded = recorded_thread_error(token)) return recorded;
    if (!handle) return "";

    ContextLease lease;
    LeaseResult acquired = acquire_or_record(handle, false, "sura_last_error", lease);
    if (acquired != LeaseResult::Ok) {
        const char* recorded = recorded_thread_error(token);
        return recorded ? recorded : "Sura FFI error unavailable";
    }
    try {
        std::lock_guard<std::recursive_mutex> runtime_lock(gc_runtime_mutex());
        std::lock_guard<std::recursive_mutex> context_lock(lease.context()->mutex);
        std::string* return_buffer = return_buffer_for_current_thread();
        if (!return_buffer) {
            record_thread_error(token, "sura_last_error could not allocate its thread return buffer");
            const char* recorded = recorded_thread_error(token);
            return recorded ? recorded : "Sura FFI error unavailable";
        }
        *return_buffer = lease.context()->last_error;
        return return_buffer->c_str();
    } catch (...) {
        record_current_exception(token, "sura_last_error");
        const char* recorded = recorded_thread_error(token);
        return recorded ? recorded : "Sura FFI error unavailable";
    }
}

SURA_API int sura_abi_version() SURA_NOEXCEPT {
    return SURA_FFI_ABI_VERSION;
}

SURA_API const char* sura_version() SURA_NOEXCEPT {
    return "Sura FFI 1.2.0";
}

#ifdef SURA_FFI_TESTING
SURA_API void sura_test_set_run_hook(SuraFfiTestRunHook hook) SURA_NOEXCEPT {
    test_run_hook.store(hook, std::memory_order_release);
}

SURA_API void sura_test_fail_next_root_commit(int enabled) SURA_NOEXCEPT {
    test_fail_root_commit.store(enabled != 0, std::memory_order_release);
}

SURA_API void sura_test_fail_next_error_store(int enabled) SURA_NOEXCEPT {
    test_fail_error_store.store(enabled != 0, std::memory_order_release);
}

SURA_API void sura_test_set_gc_shutdown_enabled(int enabled) SURA_NOEXCEPT {
    test_gc_shutdown_enabled.store(enabled != 0, std::memory_order_release);
}
#endif
