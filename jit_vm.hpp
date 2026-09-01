#pragma once
#include "jit_compiler.hpp"
#include "jit_native.hpp"
#include "jit_throw.hpp"
#include "stdlib.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <functional>
#include <cctype>
#include <limits>
#include <chrono>
#include "platform.hpp"
#include "profiler.hpp"


struct CallFrame {
    size_t   reg_base    = 0; // offset into JitVM::value_stack
    size_t   reg_count   = 0;
    GCClosure* closure   = nullptr;
    const JitMethodInfo* method = nullptr;
    size_t ip = 0;
    size_t end_ip = 0;
    const JitChunk* chunk = nullptr;
    bool   in_try        = false;
    size_t catch_ip      = (size_t)-1;
    uint16_t catch_var_reg = 0;
    uint16_t ret_reg     = (uint16_t)-1; // register in PARENT frame for return value
};

struct JitDebugVar {
    std::string name;
    std::string value;
    std::vector<JitDebugVar> children;
};

struct JitDebugFrame {
    std::string name;
    int line = 0;
    std::vector<JitDebugVar> locals;
};

struct JitDebugSnapshot {
    int line = 0;
    size_t ip = 0;
    std::string function;
    std::vector<JitDebugFrame> frames;
    std::vector<JitDebugVar> globals;
    std::vector<JitDebugVar> locals;
};

struct SuraGcStats {
    uint64_t collections = 0;
    uint64_t objects_reclaimed = 0;
    size_t last_objects_before = 0;
    size_t last_objects_after = 0;
    size_t peak_objects = 0;
    uint64_t total_pause_us = 0;
    uint64_t max_pause_us = 0;
    size_t next_object_threshold = 1024;
    size_t last_tensor_bytes = 0;
};


// JitReturn / JitThrow moved to jit_throw.hpp

// Sura numbers are IEEE-754 doubles. Bitwise operators therefore accept only
// exact safe integers; silently truncating fractions or converting NaN/Inf to
// int64_t is undefined or platform-dependent C++ behavior. Keep this helper in
// the VM header so native-JIT slow paths can share the same contract.
inline long long sura_checked_bit_integer(const Value& value, const char* op, int line) {
    constexpr double max_safe_integer = 9007199254740991.0; // 2^53 - 1
    if (!value.is_num()) {
        throw JitThrow{"[E200] " + std::string(op) + " operands must be numbers", line};
    }
    const double raw = value.as_num();
    if (!std::isfinite(raw) || std::trunc(raw) != raw ||
        raw < -max_safe_integer || raw > max_safe_integer) {
        throw JitThrow{"[E203] " + std::string(op) +
                       " operands must be finite safe integers", line};
    }
    return static_cast<long long>(raw);
}

inline Value sura_checked_bit_binary(JitOp op, const Value& left,
                                     const Value& right, int line) {
    constexpr long long max_safe_integer = 9007199254740991LL;
    const long long lhs = sura_checked_bit_integer(left, "bitwise", line);
    const long long rhs = sura_checked_bit_integer(right, "bitwise", line);
    long long result = 0;
    switch (op) {
        case JitOp::BIT_AND: result = lhs & rhs; break;
        case JitOp::BIT_OR:  result = lhs | rhs; break;
        case JitOp::BIT_XOR: result = lhs ^ rhs; break;
        case JitOp::LSHIFT: {
            if (rhs < 0 || rhs >= 64) {
                throw JitThrow{"[E203] shift count must be in 0..63", line};
            }
            const long double scaled = std::ldexp(static_cast<long double>(lhs),
                                                  static_cast<int>(rhs));
            if (scaled < -static_cast<long double>(max_safe_integer) ||
                scaled > static_cast<long double>(max_safe_integer)) {
                throw JitThrow{"[E203] left shift result exceeds the safe integer range", line};
            }
            result = static_cast<long long>(scaled);
            break;
        }
        case JitOp::RSHIFT: {
            if (rhs < 0 || rhs >= 64) {
                throw JitThrow{"[E203] shift count must be in 0..63", line};
            }
            // Define arithmetic right shift portably instead of relying on
            // implementation-defined signed C++ right-shift behavior.
            const long double divisor = std::ldexp(1.0L, static_cast<int>(rhs));
            result = static_cast<long long>(std::floor(static_cast<long double>(lhs) /
                                                       divisor));
            break;
        }
        default:
            throw JitThrow{"[E500] invalid bitwise opcode", line};
    }
    if (result < -max_safe_integer || result > max_safe_integer) {
        throw JitThrow{"[E203] bitwise result exceeds the safe integer range", line};
    }
    return Value(static_cast<double>(result));
}

inline Value sura_checked_bit_not(const Value& operand, int line) {
    constexpr long long max_safe_integer = 9007199254740991LL;
    const long long raw = sura_checked_bit_integer(operand, "bitwise not", line);
    const long long result = ~raw;
    if (result < -max_safe_integer || result > max_safe_integer) {
        throw JitThrow{"[E203] bitwise result exceeds the safe integer range", line};
    }
    return Value(static_cast<double>(result));
}




class JitVM {
    // ── Pre-allocated value stack: eliminates per-call heap allocation ──
    static constexpr size_t STACK_CAPACITY = 1 << 17; // 128K values = 1MB
    std::vector<Value> value_stack;
    size_t stack_top = 0;

    std::vector<Value> globals;
    std::vector<bool> global_initialized;
    std::vector<GCUpvalue*> open_upvalues;

    std::unordered_map<std::string, JitFuncInfo> rt_functions;
    std::unordered_map<std::string, JitClassInfo> rt_classes;

    int lambda_counter = 0;
    Profiler* prof = nullptr;   // non-owning; set via set_profiler()
    bool trace_enabled = false;
    // ── Native JIT side-table ──────────────────────────────
    bool jit_enabled = false;
    std::unordered_map<int, std::unique_ptr<NativeFunc>> native_funcs; // func_idx -> compiled
    std::unordered_set<int> jit_failed;                                // func_idx -> don't retry
    // Methods are keyed by JitMethodInfo pointer (they live inside class_table).
    std::unordered_map<const JitMethodInfo*, std::unique_ptr<NativeFunc>> native_methods;
    std::unordered_set<const JitMethodInfo*> jit_method_failed;
    std::unordered_map<const JitMethodInfo*, std::vector<int>> plain_ctor_field_cache;
    std::unordered_set<const JitMethodInfo*> non_plain_ctor_cache;
    // ── Lazy JIT compilation thresholds (Option A inline IC) ──
    // Methods warm one call earlier so caller functions can bake their ICs.
    static constexpr int METHOD_LAZY_JIT_THRESHOLD = 5;
    static constexpr int FUNC_LAZY_JIT_THRESHOLD = METHOD_LAZY_JIT_THRESHOLD + 1;
    // Flat per-func_idx JIT state. func_idx is a dense index into
    // chunk.func_table, so a vector lookup replaces the native_funcs /
    // jit_failed / func_warm_count hash probes that CALL_FUNC previously did
    // on every call. The warm counter lives in the slot, which is what
    // retired the separate func_warm_count map. native_funcs stays the owner and is never cleared, so
    // the cached pointer below cannot dangle.
    struct JitFuncSlot {
        NativeFunc* native = nullptr;   // non-owning; owned by native_funcs
        int32_t     warm   = 0;
        bool        failed = false;
        uint8_t     deopts = 0;         // entry-guard failures since compile
    };
    std::vector<JitFuncSlot> jit_slots;

    // The top-level main chunk is compiled into a local NativeFunc that is
    // never stored in native_funcs, so its opcode mask has to be captured
    // separately or emitter coverage silently omits everything that only
    // appears at top level - DEF_CLASS, DEF_FUNC, USE_LIB and HALT among them.
    uint64_t main_emitted_ops = 0;
    // When the top-level compile gives up on an opcode, remember which one.
    // Because main compiles all-or-nothing, a single rejected opcode costs the
    // whole program its native code - so naming it turns "this program did not
    // use the JIT" into something actionable.
    bool     main_bailed = false;
    JitOp    main_bail_op = JitOp::NOP;
    size_t   main_bail_ip = 0;

    // Native code is invisible to the profiler: every counter is incremented
    // from the interpreter's dispatch, so whatever runs natively contributes
    // nothing to the report. Profiling with the JIT on does not yield a faster
    // profile, it yields a wrong one - on the loop in
    // tools/sura_pkg_profile_smoke.ps1 (one call, one branch, three arithmetic
    // sites) the interpreter reports 3/2/1 and the JIT'd run reports 1/0/0.
    // Zero branch sites is indistinguishable from a program with no branches,
    // which is the failure mode that matters: the report is silently wrong
    // rather than visibly incomplete. So profiling suppresses native
    // compilation. Widening emitter coverage makes this worse, not better,
    // which is how it surfaced.
    bool native_allowed() const { return jit_enabled && prof == nullptr; }

    JitFuncSlot& jit_slot(int fidx) {
        if ((size_t)fidx >= jit_slots.size()) jit_slots.resize((size_t)fidx + 1);
        return jit_slots[fidx];
    }

    // Warm-up and compilation. Kept out of line so that resolve_native() stays
    // small enough to inline at the call sites: this body pulls in
    // NativeCompiler and a try/catch, and inlining all of that into CALL_FUNC
    // costs more than the hash lookups it was introduced to remove.
#ifdef __GNUC__
    __attribute__((noinline))
#endif
    NativeFunc* resolve_native_slow(const JitChunk& chunk, const JitFuncInfo& fi, int fidx) {
        if (__builtin_expect(prof != nullptr, 0)) return nullptr;
        JitFuncSlot& slot = jit_slot(fidx);
        if (slot.native) return slot.native;
        if (slot.failed) return nullptr;
        if (++slot.warm < FUNC_LAZY_JIT_THRESHOLD) return nullptr;
        try {
            NativeCompiler nc(chunk, fi);
            auto compiled = nc.compile();
            if (compiled) {
                NativeFunc* raw = compiled.get();
                native_funcs.emplace(fidx, std::move(compiled));
                slot.native = raw;
                return raw;
            }
        } catch (...) {
            // fall through to the failure bookkeeping below
        }
        slot.failed = true;
        jit_failed.insert(fidx);   // keep the legacy set in sync for reporting
        return nullptr;
    }

    // A guarded native body handed back SURA_JIT_DEOPT_SENTINEL: the call is
    // re-run in the interpreter by the caller; here we only count the miss.
    // A function whose arguments are persistently non-numeric would pay the
    // guard on every call, so after a few misses the slot is demoted for good.
    static constexpr uint8_t NATIVE_DEOPT_DEMOTE_LIMIT = 8;
    void note_native_deopt(int fidx) {
        if (fidx < 0 || (size_t)fidx >= jit_slots.size()) return;
        JitFuncSlot& slot = jit_slots[(size_t)fidx];
        if (slot.native && ++slot.deopts >= NATIVE_DEOPT_DEMOTE_LIMIT) {
            slot.native = nullptr;   // native_funcs still owns the code
            slot.failed = true;
            jit_failed.insert(fidx);
        }
    }

    // Resolve the native body for fidx. Once a function has settled into either
    // "compiled" or "never compilable" - which is where a hot call site spends
    // essentially all of its calls - this is a bounds check and a load.
    inline NativeFunc* resolve_native(const JitChunk& chunk, const JitFuncInfo& fi, int fidx) {
        if (fidx < 0) return nullptr;
        if (__builtin_expect((size_t)fidx < jit_slots.size(), 1)) {
            JitFuncSlot& slot = jit_slots[(size_t)fidx];
            if (slot.native) return slot.native;
            if (__builtin_expect(slot.failed, 1)) return nullptr;
        }
        return resolve_native_slow(chunk, fi, fidx);
    }

    std::unordered_map<const void*, int> method_warm_count;    // JitMethodInfo* -> call count
    // Chunk currently executing — used by JIT'd native code via C helpers
    // (sura_jit_call / sura_jit_load_global / sura_jit_store_global).
    const JitChunk* active_chunk = nullptr;
    std::function<void(const JitDebugSnapshot&)> debug_hook;
    // ── Pre-allocated frame pool: raw array avoids vector push_back overhead ──
    static constexpr size_t FRAME_CAPACITY = 512;
    CallFrame frame_pool[FRAME_CAPACITY];
    size_t    frame_top = 0;
    // call_stack view used by GC (points into frame_pool)
    struct CallStackView {
        CallFrame* data; size_t* size;
        CallFrame* begin() { return data; }
        CallFrame* end()   { return data + *size; }
    } call_stack{frame_pool, &frame_top};
    size_t gc_threshold = 1024;
    SuraGcStats gc_stats;
    size_t native_allocation_gc_tick = 0;
    size_t tensor_bytes_after_gc = 0;
    size_t generic_record_loop_runs = 0;

    bool is_stdlib_name(const std::string& name) const {
        auto names = SuraStd::names();
        return std::find(names.begin(), names.end(), name) != names.end()
            || name == "print" || name == "print_n" || name == "input"
            || name == "exit" || name == "clock" || name == "type";
    }

    static std::string canonical_stdlib_module(std::string module) {
        if (module == "logging") return "log";
        if (module == "filesystem" || module == "file") return "fs";
        if (module == "time") return "datetime";
        if (module == "web") return "http";
        if (module == "data") return "json";
        if (module == "testing") return "test";
        if (module == "rng") return "random";
        if (module == "py") return "python";
        if (module == "g3d" || module == "graphics") return "graphics3d";
        if (module == "ai") return "nn";
        return module;
    }

    static bool is_stdlib_module(const std::string& module) {
        static const std::unordered_set<std::string> modules = {
            "array", "set", "math", "path", "string", "json", "regex", "datetime",
            "crypto", "dict", "db", "cli", "log", "console", "fs", "os", "http", "async",
            "test", "random", "vector", "rag", "tensor", "nn", "stream", "tool",
            "autograd", "tokenizer", "dataset", "media", "llm", "python", "ffi", "plugin", "graphics3d"
        };
        return modules.count(canonical_stdlib_module(module)) > 0;
    }

    static std::string module_builtin_name(const std::string& raw_module,
                                           const std::string& method) {
        std::string module = canonical_stdlib_module(raw_module);
        auto prefixed = [&](const std::string& prefix) {
            if (method.rfind(prefix, 0) == 0) return method;
            return prefix + method;
        };
        auto map_method = [&](const std::vector<std::pair<std::string, std::string>>& entries) {
            for (const auto& entry : entries) {
                if (method == entry.first) return entry.second;
            }
            return std::string();
        };

        if (module == "math") return method;
        if (module == "array") {
            return map_method({
                {"len", "length"}, {"length", "length"}, {"size", "length"},
                {"slice", "slice"}, {"sort", "sort"}, {"reverse", "reverse"},
                {"concat", "concat"}, {"push", "push"}, {"pop", "pop"},
                {"clone", "clone"}, {"copy", "clone"},
                {"contains", "contains"}, {"index_of", "indexOf"},
                {"index", "indexOf"}, {"sum", "array_sum"},
                {"avg", "array_avg"}, {"average", "array_avg"},
                {"min", "array_min"}, {"max", "array_max"},
                {"unique", "array_unique"}, {"flatten", "array_flatten"},
                {"range", "array_range"}, {"chunk", "array_chunk"},
                {"chunks", "array_chunk"}, {"zip", "array_zip"},
                {"repeat", "array_repeat"},
                {"array_sum", "array_sum"}, {"array_avg", "array_avg"},
                {"array_min", "array_min"}, {"array_max", "array_max"},
                {"array_unique", "array_unique"}, {"array_flatten", "array_flatten"},
                {"array_range", "array_range"}, {"array_chunk", "array_chunk"},
                {"array_zip", "array_zip"}, {"array_repeat", "array_repeat"}
            });
        }
        if (module == "set") {
            return map_method({
                {"union", "set_union"}, {"intersection", "set_intersection"},
                {"difference", "set_difference"},
                {"symmetric_difference", "set_symmetric_difference"},
                {"symdiff", "set_symmetric_difference"},
                {"is_subset", "set_is_subset"}, {"subset", "set_is_subset"},
                {"is_superset", "set_is_superset"}, {"superset", "set_is_superset"},
                {"set_union", "set_union"},
                {"set_intersection", "set_intersection"},
                {"set_difference", "set_difference"},
                {"set_symmetric_difference", "set_symmetric_difference"},
                {"set_is_subset", "set_is_subset"},
                {"set_is_superset", "set_is_superset"}
            });
        }
        if (module == "random") {
            return map_method({
                {"random", "random"}, {"seed", "random_seed"},
                {"int", "random_int"}, {"integer", "random_int"},
                {"float", "random_float"}, {"number", "random_float"},
                {"bool", "random_bool"}, {"choice", "random_choice"},
                {"shuffle", "random_shuffle"}, {"bytes", "random_bytes"},
                {"uuid", "uuid_v4"}, {"uuid_v4", "uuid_v4"},
                {"random_seed", "random_seed"}, {"random_int", "random_int"},
                {"random_float", "random_float"}, {"random_bool", "random_bool"},
                {"random_choice", "random_choice"},
                {"random_shuffle", "random_shuffle"},
                {"random_bytes", "random_bytes"}
            });
        }
        if (module == "string") {
            std::string mapped = map_method({
                {"len", "length"}, {"length", "length"}, {"size", "length"},
                {"split", "split"}, {"join", "join"}, {"trim", "trim"}, {"upper", "upper"},
                {"lower", "lower"}, {"contains", "contains"},
                {"startsWith", "startsWith"}, {"starts_with", "startsWith"},
                {"endsWith", "endsWith"}, {"ends_with", "endsWith"},
                {"indexOf", "indexOf"}, {"index_of", "indexOf"},
                {"sub", "substring"}, {"substring", "substring"}, {"slice", "slice"},
                {"replace", "replace"}, {"lines", "string_lines"},
                {"words", "string_words"}, {"repeat", "string_repeat"},
                {"pad_left", "string_pad_left"}, {"pad_right", "string_pad_right"},
                {"chunks", "text_chunks"}, {"string_lines", "string_lines"},
                {"string_words", "string_words"}, {"string_repeat", "string_repeat"},
                {"string_pad_left", "string_pad_left"},
                {"string_pad_right", "string_pad_right"}
            });
            return mapped;
        }
        if (module == "regex") {
            return map_method({
                {"match", "regex_match"}, {"replace", "regex_replace"},
                {"find_all", "regex_find_all"}, {"findall", "regex_find_all"},
                {"escape", "regex_escape"}, {"quote", "regex_escape"},
                {"capture", "regex_capture"}, {"groups", "regex_capture"},
                {"captures", "regex_captures"}, {"split", "regex_split"},
                {"regex_match", "regex_match"}, {"regex_replace", "regex_replace"},
                {"regex_find_all", "regex_find_all"},
                {"regex_escape", "regex_escape"},
                {"regex_capture", "regex_capture"}, {"regex_captures", "regex_captures"},
                {"regex_split", "regex_split"}
            });
        }
        if (module == "datetime") {
            return map_method({
                {"now", "datetime_now"}, {"format", "datetime_format"},
                {"utc_format", "datetime_utc_format"}, {"parse", "datetime_parse"},
                {"parts", "datetime_parts"}, {"add", "datetime_add"},
                {"diff", "datetime_diff"}, {"timestamp", "timestamp"},
                {"datetime_now", "datetime_now"}, {"datetime_format", "datetime_format"},
                {"datetime_utc_format", "datetime_utc_format"},
                {"datetime_parse", "datetime_parse"}, {"datetime_parts", "datetime_parts"},
                {"datetime_add", "datetime_add"}, {"datetime_diff", "datetime_diff"}
            });
        }
        if (module == "crypto") {
            return map_method({
                {"sha256", "sha256"}, {"file_sha256", "file_sha256"},
                {"hmac_sha256", "hmac_sha256"},
                {"file_hmac_sha256", "file_hmac_sha256"},
                {"random_bytes", "crypto_random_bytes"}, {"random_hex", "crypto_random_hex"},
                {"constant_time_eq", "constant_time_eq"},
                {"hex_encode", "hex_encode"}, {"hex_decode", "hex_decode"},
                {"base64_encode", "base64_encode"}, {"base64_decode", "base64_decode"},
                {"base64_url_encode", "base64_url_encode"}, {"base64_url_decode", "base64_url_decode"},
                {"url_encode", "url_encode"}, {"url_decode", "url_decode"},
                {"url_parse", "url_parse"}, {"url_build", "url_build"},
                {"query_build", "query_build"}, {"query_parse", "query_parse"},
                {"form_build", "form_build"}, {"form_parse", "form_parse"},
                {"auth_bearer", "auth_bearer"}, {"auth_basic", "auth_basic"},
                {"headers_merge", "headers_merge"},
                {"headers_get", "headers_get"}, {"headers_has", "headers_has"},
                {"headers_redact", "headers_redact"},
                {"cookie_parse", "cookie_parse"}, {"cookie_build", "cookie_build"},
                {"cookie_get", "cookie_get"},
                {"content_type", "http_content_type"}, {"charset", "http_charset"},
                {"is_json", "http_is_json"},
                {"status_ok", "http_status_ok"},
                {"status_text", "http_status_text"},
                {"status_retryable", "http_status_retryable"},
                {"retry_after", "http_retry_after"},
                {"backoff_delays", "http_backoff_delays"}
            });
        }
        if (module == "db") return prefixed("db_");
        if (module == "cli") {
            return map_method({
                {"parse", "cli_parse"}, {"cli_parse", "cli_parse"},
                {"argv", "argv"}, {"argc", "argc"}, {"script_name", "script_name"}
            });
        }
        if (module == "log") {
            return map_method({
                {"set_file", "log_set_file"}, {"set_json", "log_set_json"},
                {"set_level", "log_set_level"}, {"get_level", "log_get_level"},
                {"level", "log_level"},
                {"event", "log_event"}, {"debug", "log_debug"},
                {"info", "log_info"}, {"warn", "log_warn"},
                {"warning", "log_warn"}, {"error", "log_error"},
                {"log_set_file", "log_set_file"}, {"log_set_json", "log_set_json"},
                {"log_set_level", "log_set_level"}, {"log_get_level", "log_get_level"},
                {"log_level", "log_level"},
                {"log_event", "log_event"}, {"log_debug", "log_debug"},
                {"log_info", "log_info"}, {"log_warn", "log_warn"},
                {"log_error", "log_error"}
            });
        }
        if (module == "console") {
            return map_method({
                {"log", "console_log"}, {"print", "console_print"},
                {"write", "console_write"},
                {"write_line", "console_write_line"}, {"writeln", "console_write_line"},
                {"println", "console_println"}, {"line", "console_line"},
                {"info", "console_info"},
                {"debug", "console_debug"}, {"warn", "console_warn"},
                {"warning", "console_warn"}, {"error", "console_error"},
                {"exception", "console_exception"},
                {"raw", "console_raw"}, {"flush", "console_flush"},
                {"json", "console_json"}, {"inspect", "console_inspect"},
                {"hrtime", "console_hrtime"}, {"beep", "console_beep"},
                {"clear", "console_clear"}, {"assert", "console_assert"},
                {"time", "console_time"}, {"time_end", "console_time_end"},
                {"timeEnd", "console_time_end"}, {"time_log", "console_time_log"},
                {"timeLog", "console_time_log"}, {"time_stamp", "console_time_stamp"},
                {"timeStamp", "console_time_stamp"},
                {"count", "console_count"}, {"count_reset", "console_count_reset"},
                {"countReset", "console_count_reset"},
                {"table", "console_table"}, {"dir", "console_dir"},
                {"dirxml", "console_dirxml"},
                {"trace", "console_trace"}, {"group", "console_group"},
                {"group_collapsed", "console_group_collapsed"},
                {"groupCollapsed", "console_group_collapsed"},
                {"group_end", "console_group_end"}, {"groupEnd", "console_group_end"},
                {"profile", "console_profile"}, {"profile_end", "console_profile_end"},
                {"profileEnd", "console_profile_end"},
                {"style", "console_style"}, {"color", "console_color"},
                {"colour", "console_color"}, {"strip_ansi", "console_strip_ansi"},
                {"stripAnsi", "console_strip_ansi"},
                {"set_color", "console_set_color"}, {"setColor", "console_set_color"},
                {"set_colour", "console_set_color"}, {"setColour", "console_set_color"},
                {"reset_color", "console_reset_color"}, {"resetColor", "console_reset_color"},
                {"reset_colour", "console_reset_color"}, {"resetColour", "console_reset_color"},
                {"is_tty", "console_is_tty"}, {"isTTY", "console_is_tty"},
                {"width", "console_width"}, {"height", "console_height"},
                {"size", "console_size"}, {"status", "console_status"},
                {"input", "input"},
                {"read_line", "console_read_line"}, {"readline", "console_readline"},
                {"readLine", "console_read_line"},
                {"prompt", "console_prompt"},
                {"console_log", "console_log"}, {"console_print", "console_print"},
                {"console_write", "console_write"},
                {"console_write_line", "console_write_line"}, {"console_writeln", "console_writeln"},
                {"console_println", "console_println"}, {"console_line", "console_line"},
                {"console_info", "console_info"},
                {"console_debug", "console_debug"}, {"console_warn", "console_warn"},
                {"console_error", "console_error"}, {"console_exception", "console_exception"},
                {"console_raw", "console_raw"}, {"console_flush", "console_flush"},
                {"console_json", "console_json"}, {"console_inspect", "console_inspect"},
                {"console_hrtime", "console_hrtime"}, {"console_beep", "console_beep"},
                {"console_clear", "console_clear"},
                {"console_assert", "console_assert"}, {"console_time", "console_time"},
                {"console_time_end", "console_time_end"},
                {"console_time_log", "console_time_log"},
                {"console_time_stamp", "console_time_stamp"},
                {"console_count", "console_count"},
                {"console_count_reset", "console_count_reset"},
                {"console_table", "console_table"}, {"console_dir", "console_dir"},
                {"console_dirxml", "console_dirxml"},
                {"console_trace", "console_trace"}, {"console_group", "console_group"},
                {"console_group_collapsed", "console_group_collapsed"},
                {"console_group_end", "console_group_end"},
                {"console_profile", "console_profile"}, {"console_profile_end", "console_profile_end"},
                {"console_style", "console_style"}, {"console_color", "console_color"},
                {"console_colour", "console_colour"},
                {"console_strip_ansi", "console_strip_ansi"},
                {"console_set_color", "console_set_color"}, {"console_set_colour", "console_set_colour"},
                {"console_reset_color", "console_reset_color"}, {"console_reset_colour", "console_reset_colour"},
                {"console_is_tty", "console_is_tty"}, {"console_width", "console_width"},
                {"console_height", "console_height"}, {"console_size", "console_size"},
                {"console_status", "console_status"},
                {"console_input", "console_input"}, {"console_read_line", "console_read_line"},
                {"console_readline", "console_readline"}, {"console_readLine", "console_readLine"},
                {"console_prompt", "console_prompt"}
            });
        }
        if (module == "json") {
            return map_method({
                {"parse", "json_parse"}, {"try_parse", "json_try_parse"},
                {"stringify", "json_stringify"}, {"pretty", "json_pretty"},
                {"serialize", "serialize"}, {"deserialize", "deserialize"},
                {"json_parse", "json_parse"}, {"json_try_parse", "json_try_parse"},
                {"json_stringify", "json_stringify"}, {"json_pretty", "json_pretty"},
                {"jsonl_parse", "jsonl_parse"}, {"jsonl_stringify", "jsonl_stringify"},
                {"sse_parse", "sse_parse"}, {"sse_data", "sse_data"},
                {"csv_parse", "csv_parse"}, {"csv_stringify", "csv_stringify"},
                {"ini_parse", "ini_parse"}, {"ini_stringify", "ini_stringify"},
                {"path", "json_path"}, {"json_path", "json_path"},
                {"has_path", "json_has_path"}, {"json_has_path", "json_has_path"},
                {"merge_patch", "json_merge_patch"}, {"json_merge_patch", "json_merge_patch"},
                {"delete_path", "json_delete_path"}, {"json_delete_path", "json_delete_path"},
                {"set_path", "json_set_path"}, {"json_set_path", "json_set_path"},
                {"get_path", "dict_get_path"}, {"dict_get_path", "dict_get_path"},
                {"pluck", "pluck"}, {"count_by", "count_by"},
                {"group_by", "group_by"}, {"sort_by", "sort_by"},
                {"template_render", "template_render"},
                {"schema_validate", "schema_validate"},
                {"schema_errors", "schema_errors"},
                {"schema_to_json_schema", "schema_to_json_schema"},
                {"to_json_schema", "schema_to_json_schema"}
            });
        }
        if (module == "dict") {
            return map_method({
                {"keys", "dict_keys"}, {"values", "dict_values"},
                {"items", "dict_items"}, {"merge", "dict_merge"},
                {"pick", "dict_pick"}, {"omit", "dict_omit"},
                {"get_path", "dict_get_path"},
                {"dict_keys", "dict_keys"}, {"dict_values", "dict_values"},
                {"dict_items", "dict_items"}, {"dict_merge", "dict_merge"},
                {"dict_pick", "dict_pick"}, {"dict_omit", "dict_omit"},
                {"dict_get_path", "dict_get_path"}
            });
        }
        if (module == "fs") {
            std::string mapped = map_method({
                {"read", "file_read"}, {"write", "file_write"},
                {"read_json", "file_read_json"}, {"write_json", "file_write_json"},
                {"read_bytes", "file_read_bytes"}, {"write_bytes", "file_write_bytes"},
                {"sha256", "file_sha256"},
                {"append", "file_append"}, {"exists", "file_exists"},
                {"delete", "file_delete"}, {"remove", "file_delete"},
                {"remove_tree", "file_remove_tree"}, {"delete_tree", "file_remove_tree"},
                {"list", "file_list"}, {"walk", "file_walk"}, {"glob", "file_glob"},
                {"mkdir", "mkdir"}, {"cwd", "cwd"}, {"join", "path_join"},
                {"basename", "path_basename"}, {"dirname", "path_dirname"},
                {"ext", "path_ext"}, {"stem", "path_stem"},
                {"normalize", "path_normalize"}, {"abs", "path_abs"},
                {"relative", "path_relative"}, {"is_dir", "file_is_dir"},
                {"is_file", "file_is_file"}, {"info", "file_info"},
                {"size", "file_size"}, {"copy", "file_copy"},
                {"move", "file_move"}, {"lines", "file_lines"}
            });
            if (!mapped.empty()) return mapped;
            if (method.rfind("file_", 0) == 0 || method.rfind("path_", 0) == 0) return method;
            return "";
        }
        if (module == "path") {
            return map_method({
                {"join", "path_join"}, {"basename", "path_basename"},
                {"dirname", "path_dirname"}, {"ext", "path_ext"},
                {"stem", "path_stem"}, {"normalize", "path_normalize"},
                {"abs", "path_abs"}, {"relative", "path_relative"},
                {"path_join", "path_join"}, {"path_basename", "path_basename"},
                {"path_dirname", "path_dirname"}, {"path_ext", "path_ext"},
                {"path_stem", "path_stem"},
                {"path_normalize", "path_normalize"},
                {"path_abs", "path_abs"},
                {"path_relative", "path_relative"}
            });
        }
        if (module == "os") {
            return map_method({
                {"env_get", "env_get"}, {"env_require", "env_require"},
                {"env_set", "env_set"}, {"env_load", "env_load"},
                {"argv", "argv"}, {"argc", "argc"},
                {"script_name", "script_name"}, {"cwd", "cwd"},
                {"home_dir", "home_dir"}, {"temp_dir", "temp_dir"},
                {"path_separator", "path_separator"}, {"name", "os_name"},
                {"is_windows", "is_windows"},
                {"which", "which"}, {"cmd_exists", "cmd_exists"},
                {"cmd_quote", "cmd_quote"}, {"cmd_join", "cmd_join"},
                {"run", "cmd_run"}, {"run_checked", "cmd_run_checked"},
                {"cmd", "async_cmd"}, {"sleep_ms", "sleep_ms"},
                {"wait", "wait"}
            });
        }
        if (module == "http") {
            return map_method({
                {"get", "http_get"}, {"json", "http_json"},
                {"post", "http_post"}, {"request", "http_request"},
                {"request_full", "http_request_full"},
                {"request_retry", "http_request_retry"},
                {"request_json", "http_request_json"},
                {"request_json_checked", "http_request_json_checked"},
                {"request_retry_json", "http_request_retry_json"},
                {"request_retry_json_checked", "http_request_retry_json_checked"},
                {"serve_static", "http_serve_static"},
                {"serve_routes", "http_serve_routes"},
                {"server_url", "http_server_url"},
                {"server_stop", "http_server_stop"},
                {"auth_bearer", "auth_bearer"}, {"auth_basic", "auth_basic"},
                {"headers_merge", "headers_merge"},
                {"headers_get", "headers_get"}, {"headers_has", "headers_has"},
                {"headers_redact", "headers_redact"},
                {"cookie_parse", "cookie_parse"}, {"cookie_build", "cookie_build"},
                {"cookie_get", "cookie_get"},
                {"content_type", "http_content_type"}, {"charset", "http_charset"},
                {"is_json", "http_is_json"},
                {"query_build", "query_build"}, {"query_parse", "query_parse"},
                {"form_build", "form_build"}, {"form_parse", "form_parse"},
                {"url_parse", "url_parse"}, {"url_build", "url_build"},
                {"status_ok", "http_status_ok"},
                {"status_text", "http_status_text"},
                {"status_retryable", "http_status_retryable"},
                {"retry_after", "http_retry_after"},
                {"backoff_delays", "http_backoff_delays"}
            });
        }
        if (module == "async") {
            return map_method({
                {"cmd", "async_cmd"}, {"ready", "async_ready"},
                {"http_get", "async_http_get"},
                {"http_request", "async_http_request"},
                {"sleep", "async_sleep"},
                {"sura", "async_sura"},
                {"status", "async_status"}, {"pending", "async_pending"},
                {"forget", "async_forget"}, {"cleanup", "async_cleanup"},
                {"cancel", "async_cancel"}, {"cancelled", "async_cancelled"},
                {"configure", "async_configure"}, {"limits", "async_limits"},
                {"scope", "async_scope_open"}, {"scope_open", "async_scope_open"},
                {"scope_attach", "async_scope_attach"},
                {"scope_cancel", "async_scope_cancel"},
                {"scope_status", "async_scope_status"},
                {"scope_close", "async_scope_close"},
                {"scope_join", "async_scope_join"},
                {"await", "async_await"}, {"await_timeout", "async_await_timeout"},
                {"ready_all", "async_ready_all"}, {"any", "async_any"},
                {"all", "async_all"}, {"all_timeout", "async_all_timeout"}
            });
        }
        if (module == "test") {
            return map_method({
                {"assert", "assert"}, {"eq", "assert_eq"},
                {"ne", "assert_ne"}, {"neq", "assert_ne"},
                {"contains", "assert_contains"},
                {"not_contains", "assert_not_contains"},
                {"match", "assert_match"}, {"type", "assert_type"},
                {"len", "assert_len"}, {"between", "assert_between"},
                {"approx", "assert_approx"}, {"check", "check"},
                {"check_eq", "check_eq"}, {"check_match", "check_match"},
                {"summary", "test_summary"}, {"report", "test_report"}
            });
        }
        if (module == "vector") {
            return map_method({
                {"add", "vector_add"}, {"dot", "vector_dot"},
                {"scale", "vector_scale"}, {"norm", "vector_norm"},
                {"vec3", "vec3"}, {"add3", "vec3_add"}, {"sub3", "vec3_sub"},
                {"dot3", "vec3_dot"}, {"cross", "vec3_cross"},
                {"scale3", "vec3_scale"}, {"norm3", "vec3_norm"},
                {"normalize3", "vec3_normalize"}, {"distance3", "vec3_distance"},
                {"neg3", "vec3_neg"}, {"lerp3", "vec3_lerp"},
                {"midpoint3", "vec3_midpoint"}, {"project3", "vec3_project"},
                {"reject3", "vec3_reject"}, {"reflect3", "vec3_reflect"},
                {"angle3", "vec3_angle"}, {"transform4", "vec3_transform4"},
                {"cosine", "vector_cosine"}, {"normalize", "vector_normalize"},
                {"search", "vector_search"}, {"vector_add", "vector_add"},
                {"vector_dot", "vector_dot"}, {"vector_scale", "vector_scale"},
                {"vector_norm", "vector_norm"}, {"vector_cosine", "vector_cosine"},
                {"vector_normalize", "vector_normalize"},
                {"vector3", "vec3"}, {"vec3_add", "vec3_add"},
                {"vec3_sub", "vec3_sub"}, {"vec3_dot", "vec3_dot"},
                {"vec3_cross", "vec3_cross"}, {"vec3_scale", "vec3_scale"},
                {"vec3_norm", "vec3_norm"}, {"vec3_normalize", "vec3_normalize"},
                {"vec3_distance", "vec3_distance"},
                {"vec3_neg", "vec3_neg"}, {"vec3_lerp", "vec3_lerp"},
                {"vec3_midpoint", "vec3_midpoint"}, {"vec3_project", "vec3_project"},
                {"vec3_reject", "vec3_reject"}, {"vec3_reflect", "vec3_reflect"},
                {"vec3_angle", "vec3_angle"}, {"vec3_transform4", "vec3_transform4"},
                {"vector_search", "vector_search"}
            });
        }
        if (module == "graphics3d") {
            return map_method({
                {"mat4_identity", "mat4_identity"}, {"identity", "mat4_identity"},
                {"mat4_translate", "mat4_translate"}, {"translate", "mat4_translate"},
                {"mat4_scale", "mat4_scale"}, {"scale", "mat4_scale"},
                {"mat4_rotate_y", "mat4_rotate_y"}, {"rotate_y", "mat4_rotate_y"},
                {"mat4_mul", "mat4_mul"}, {"mul", "mat4_mul"},
                {"mesh_cube", "mesh_cube"}, {"cube", "mesh_cube"},
                {"mesh_transform4", "mesh_transform4"}, {"transform", "mesh_transform4"},
                {"mesh_bounds", "mesh_bounds"}, {"bounds", "mesh_bounds"},
                {"mesh_face_normals", "mesh_face_normals"}, {"face_normals", "mesh_face_normals"},
                {"camera_project", "camera_project"}, {"project", "camera_project"}
            });
        }
        if (module == "rag") {
            return map_method({
                {"context", "rag_context"}, {"sources", "rag_sources"},
                {"prepare", "rag_prepare"}, {"messages", "rag_messages"},
                {"rag_context", "rag_context"}, {"rag_sources", "rag_sources"},
                {"rag_prepare", "rag_prepare"}, {"rag_messages", "rag_messages"}
            });
        }
        if (module == "tensor") {
            return map_method({
                {"shape", "tensor_shape"}, {"zeros", "tensor_zeros"},
                {"fill", "tensor_fill"}, {"add", "tensor_add"},
                {"mul", "tensor_mul"}, {"clip", "tensor_clip"},
                {"flatten", "tensor_flatten"},
                {"sum", "tensor_sum"}, {"mean", "tensor_mean"},
                {"variance", "tensor_variance"}, {"std", "tensor_std"},
                {"min", "tensor_min"}, {"max", "tensor_max"},
                {"argmin", "tensor_argmin"}, {"argmax", "tensor_argmax"},
                {"zscore", "tensor_zscore"}, {"softmax", "tensor_softmax"},
                {"transpose", "tensor_transpose"}, {"matmul", "tensor_matmul"},
                {"tensor_shape", "tensor_shape"}, {"tensor_zeros", "tensor_zeros"},
                {"tensor_fill", "tensor_fill"}, {"tensor_add", "tensor_add"},
                {"tensor_mul", "tensor_mul"}, {"tensor_clip", "tensor_clip"},
                {"tensor_flatten", "tensor_flatten"},
                {"tensor_sum", "tensor_sum"}, {"tensor_mean", "tensor_mean"},
                {"tensor_variance", "tensor_variance"}, {"tensor_std", "tensor_std"},
                {"tensor_min", "tensor_min"}, {"tensor_max", "tensor_max"},
                {"tensor_argmin", "tensor_argmin"}, {"tensor_argmax", "tensor_argmax"},
                {"tensor_zscore", "tensor_zscore"}, {"tensor_softmax", "tensor_softmax"},
                {"tensor_transpose", "tensor_transpose"},
                {"tensor_matmul", "tensor_matmul"}
            });
        }
        if (module == "nn") {
            return map_method({
                {"mlp", "nn_mlp"}, {"forward", "nn_forward"},
                {"predict", "nn_predict"}, {"train", "nn_train"},
                {"classify", "nn_classify"}, {"evaluate", "nn_evaluate"},
                {"summary", "nn_summary"}, {"one_hot", "nn_one_hot"},
                {"fit_standardizer", "nn_fit_standardizer"},
                {"standardize", "nn_standardize"}, {"split", "nn_split"},
                {"save", "nn_save"}, {"load", "nn_load"},
                {"nn_mlp", "nn_mlp"}, {"nn_forward", "nn_forward"},
                {"nn_predict", "nn_predict"}, {"nn_train", "nn_train"},
                {"nn_classify", "nn_classify"}, {"nn_evaluate", "nn_evaluate"},
                {"nn_summary", "nn_summary"}, {"nn_one_hot", "nn_one_hot"},
                {"nn_fit_standardizer", "nn_fit_standardizer"},
                {"nn_standardize", "nn_standardize"}, {"nn_split", "nn_split"},
                {"nn_save", "nn_save"}, {"nn_load", "nn_load"}
            });
        }
        if (module == "autograd") return prefixed("autograd_");
        if (module == "tokenizer") return prefixed("tokenizer_");
        if (module == "dataset") return prefixed("dataset_");
        if (module == "media") return prefixed("media_");
        if (module == "stream") {
            return map_method({
                {"from", "stream_from"}, {"next", "stream_next"},
                {"collect", "stream_collect"}, {"take", "stream_take"},
                {"batch", "stream_batch"}, {"map", "stream_map"},
                {"filter", "stream_filter"}, {"window", "stream_window"},
                {"skip", "stream_skip"}, {"count", "stream_count"},
                {"join", "stream_join"}, {"sum", "stream_sum"},
                {"avg", "stream_avg"}, {"lines", "stream_lines"},
                {"stream_from", "stream_from"}, {"stream_next", "stream_next"},
                {"stream_collect", "stream_collect"}, {"stream_take", "stream_take"},
                {"stream_batch", "stream_batch"}, {"stream_map", "stream_map"},
                {"stream_filter", "stream_filter"}, {"stream_window", "stream_window"},
                {"stream_skip", "stream_skip"}, {"stream_count", "stream_count"},
                {"stream_join", "stream_join"}, {"stream_sum", "stream_sum"},
                {"stream_avg", "stream_avg"}, {"stream_lines", "stream_lines"}
            });
        }
        if (module == "tool") {
            return map_method({
                {"call", "tool_call"}, {"spec", "tool_spec"},
                {"validate", "tool_validate"}, {"schema", "tool_schema"},
                {"allowed", "tool_allowed"}, {"call_policy", "tool_call_policy"},
                {"list", "tool_list"}, {"tool_call", "tool_call"},
                {"tool_spec", "tool_spec"}, {"tool_validate", "tool_validate"},
                {"tool_schema", "tool_schema"}, {"tool_allowed", "tool_allowed"},
                {"tool_call_policy", "tool_call_policy"}, {"tool_list", "tool_list"}
            });
        }
        if (module == "llm") {
            return map_method({
                {"message", "llm_message"}, {"messages", "llm_messages"},
                {"rag_messages", "rag_messages"}, {"request", "llm_request"},
                {"request_json", "llm_request_json"},
                {"response_schema", "llm_response_schema"},
                {"request_schema", "llm_request_schema"},
                {"request_schema_json", "llm_request_schema_json"},
                {"tools", "llm_tools"}, {"tool_schemas", "llm_tools"},
                {"request_tools", "llm_request_tools"},
                {"request_tools_json", "llm_request_tools_json"},
                {"request_tools_schema", "llm_request_tools_schema"},
                {"request_tools_schema_json", "llm_request_tools_schema_json"},
                {"extract_text", "llm_extract_text"},
                {"extract_json", "llm_extract_json"},
                {"usage", "llm_usage"},
                {"cost", "llm_cost"},
                {"budget", "llm_budget"},
                {"tool_calls", "llm_tool_calls"},
                {"tool_result", "llm_tool_result"},
                {"run_tools", "llm_run_tools"},
                {"next_messages", "llm_next_messages"},
                {"next_request", "llm_next_request"},
                {"next_request_json", "llm_next_request_json"},
                {"next_schema_request", "llm_next_schema_request"},
                {"next_schema_request_json", "llm_next_schema_request_json"},
                {"stream_text", "llm_stream_text"}, {"chat", "llm_chat"},
                {"chat_request", "llm_chat_request"},
                {"llm_message", "llm_message"}, {"llm_messages", "llm_messages"},
                {"llm_request", "llm_request"},
                {"llm_request_json", "llm_request_json"},
                {"llm_response_schema", "llm_response_schema"},
                {"llm_request_schema", "llm_request_schema"},
                {"llm_request_schema_json", "llm_request_schema_json"},
                {"llm_tools", "llm_tools"},
                {"llm_tool_schemas", "llm_tools"},
                {"llm_request_tools", "llm_request_tools"},
                {"llm_request_tools_json", "llm_request_tools_json"},
                {"llm_request_tools_schema", "llm_request_tools_schema"},
                {"llm_request_tools_schema_json", "llm_request_tools_schema_json"},
                {"llm_extract_text", "llm_extract_text"},
                {"llm_extract_json", "llm_extract_json"},
                {"llm_usage", "llm_usage"},
                {"llm_cost", "llm_cost"},
                {"llm_budget", "llm_budget"},
                {"llm_tool_calls", "llm_tool_calls"},
                {"llm_tool_result", "llm_tool_result"},
                {"llm_run_tools", "llm_run_tools"},
                {"llm_next_messages", "llm_next_messages"},
                {"llm_next_request", "llm_next_request"},
                {"llm_next_request_json", "llm_next_request_json"},
                {"llm_next_schema_request", "llm_next_schema_request"},
                {"llm_next_schema_request_json", "llm_next_schema_request_json"},
                {"llm_stream_text", "llm_stream_text"},
                {"llm_chat", "llm_chat"},
                {"llm_chat_request", "llm_chat_request"}
            });
        }
        if (module == "python") {
            return map_method({
                {"available", "python_available"},
                {"executable", "python_executable"},
                {"eval", "python_eval"},
                {"call", "python_call"},
                {"call_json", "python_call_json"},
                {"python_available", "python_available"},
                {"python_executable", "python_executable"},
                {"python_eval", "python_eval"},
                {"python_call", "python_call"},
                {"python_call_json", "python_call_json"}
            });
        }
        if (module == "ffi") {
            return map_method({
                {"load", "ffi_load"}, {"call", "ffi_call"},
                {"ffi_load", "ffi_load"}, {"ffi_call", "ffi_call"}
            });
        }
        if (module == "plugin") {
            return map_method({
                {"load", "plugin_load"},
                {"load_manifest", "plugin_load_manifest"},
                {"manifest", "plugin_load_manifest"},
                {"call", "plugin_call"},
                {"info", "plugin_info"},
                {"unload", "plugin_unload"},
                {"plugin_load", "plugin_load"},
                {"plugin_load_manifest", "plugin_load_manifest"},
                {"plugin_call", "plugin_call"},
                {"plugin_info", "plugin_info"},
                {"plugin_unload", "plugin_unload"}
            });
        }
        return "";
    }

    static Value make_stdlib_module(const std::string& raw_module) {
        std::string module = canonical_stdlib_module(raw_module);
        Value mod = Value::make_dict();
        auto* d = mod.as_dict();
        d->elements["__module"] = Value(module);
        d->elements["name"] = Value(raw_module);
        if (module == "math") {
            d->elements["pi"] = Value(3.14159265358979323846);
            d->elements["e"] = Value(2.71828182845904523536);
        }
        return mod;
    }

    std::vector<std::string> known_runtime_names() const {
        std::vector<std::string> known;
        if (active_chunk) {
            for (size_t i = 0; i < active_chunk->global_names.size(); ++i) {
                bool initialized = i < global_initialized.size() && global_initialized[i];
                if (initialized) known.push_back(active_chunk->global_names[i]);
            }
        }
        for (auto& [name, _] : rt_classes) known.push_back(name);
        auto std_names = SuraStd::names();
        known.insert(known.end(), std_names.begin(), std_names.end());
        known.insert(known.end(), {"print", "print_n", "input", "exit", "clock", "type"});
        return known;
    }

    std::string format_undefined_variable(const std::string& name, int line) const {
        std::string msg = "[E100] 정의되지 않은 변수: '" + name + "'";
        std::string sug = sura_suggest(name, known_runtime_names());
        if (!sug.empty() && sug != name) msg += "\n       혹시 '" + sug + "' 를 쓰려고 했나요?";
        return msg;
    }

    std::string format_undefined_function(const std::string& name) const {
        std::string msg = "[E101] 정의되지 않은 함수: '" + name + "'";
        std::string sug = sura_suggest(name, known_runtime_names());
        if (!sug.empty() && sug != name) msg += "\n       혹시 '" + sug + "' 를 쓰려고 했나요?";
        return msg;
    }

    std::string debug_frame_name(const CallFrame& cf) const {
        if (cf.closure) {
            if (cf.closure->func_idx >= 0 && cf.chunk &&
                (size_t)cf.closure->func_idx < cf.chunk->func_table.size()) {
                const auto& fi = cf.chunk->func_table[(size_t)cf.closure->func_idx];
                if (!fi.name.empty()) return fi.name;
            }
            return cf.closure->name.empty() ? "<lambda>" : cf.closure->name;
        }
        if (cf.method) return cf.method->name.empty() ? "<method>" : cf.method->name;
        return "<main>";
    }

    int debug_frame_line(const CallFrame& cf) const {
        if (!cf.chunk || cf.chunk->code.empty()) return 0;
        size_t ip = cf.ip > 0 ? cf.ip - 1 : 0;
        if (ip >= cf.chunk->code.size()) ip = cf.chunk->code.size() - 1;
        return cf.chunk->code[ip].line;
    }

    JitDebugVar debug_var_for(const std::string& name, const Value& value, int depth = 0) const {
        JitDebugVar out{name, value.to_str(), {}};
        if (depth >= 3) return out;

        if (value.is_arr()) {
            auto* arr = value.as_arr();
            size_t limit = std::min<size_t>(arr->elements.size(), 100);
            for (size_t i = 0; i < limit; ++i)
                out.children.push_back(debug_var_for("[" + std::to_string(i) + "]", arr->elements[i], depth + 1));
            if (arr->elements.size() > limit)
                out.children.push_back({"...", std::to_string(arr->elements.size() - limit) + " more", {}});
        } else if (value.is_tensor()) {
            auto* tensor = value.as_tensor();
            std::string shape = "[";
            for (size_t i = 0; i < tensor->shape.size(); ++i) {
                if (i) shape += ", ";
                shape += std::to_string(tensor->shape[i]);
            }
            shape += "]";
            out.children.push_back({"shape", shape, {}});
            out.children.push_back({"requires_grad", tensor->requires_grad ? "true" : "false", {}});
            JitDebugVar data{"data", "[" + std::to_string(tensor->data.size()) + " values]", {}};
            if (!tensor->data.host_readable()) {
                data.value = "[CUDA device-resident; use autograd.data() to inspect]";
            } else {
                size_t data_limit = std::min<size_t>(tensor->data.size(), 100);
                for (size_t i = 0; i < data_limit; ++i)
                    data.children.push_back({"[" + std::to_string(i) + "]", Value(tensor->data[i]).to_str(), {}});
                if (tensor->data.size() > data_limit)
                    data.children.push_back({"...", std::to_string(tensor->data.size() - data_limit) + " more", {}});
            }
            out.children.push_back(std::move(data));
            if (!tensor->grad.empty()) {
                JitDebugVar grad{"grad", "[" + std::to_string(tensor->grad.size()) + " values]", {}};
                size_t grad_limit = std::min<size_t>(tensor->grad.size(), 100);
                for (size_t i = 0; i < grad_limit; ++i)
                    grad.children.push_back({"[" + std::to_string(i) + "]", Value(tensor->grad[i]).to_str(), {}});
                if (tensor->grad.size() > grad_limit)
                    grad.children.push_back({"...", std::to_string(tensor->grad.size() - grad_limit) + " more", {}});
                out.children.push_back(std::move(grad));
            } else if (tensor->cuda_grad) {
                out.children.push_back({"grad", "[CUDA device-resident; use autograd.grad() to inspect]", {}});
            }
        } else if (value.is_dict()) {
            auto* dict = value.as_dict();
            std::vector<std::string> keys;
            keys.reserve(dict->elements.size());
            for (const auto& [key, _] : dict->elements) keys.push_back(key);
            std::sort(keys.begin(), keys.end());
            size_t limit = std::min<size_t>(keys.size(), 100);
            for (size_t i = 0; i < limit; ++i)
                out.children.push_back(debug_var_for(keys[i], dict->elements.at(keys[i]), depth + 1));
            if (keys.size() > limit)
                out.children.push_back({"...", std::to_string(keys.size() - limit) + " more", {}});
        } else if (value.is_inst()) {
            GCInstance* inst = value.as_inst();
            auto class_it = rt_classes.find(inst->type_name());
            if (class_it != rt_classes.end()) {
                std::vector<std::pair<int, std::string>> fields;
                for (const auto& [field, index] : class_it->second.field_indices)
                    fields.push_back({index, field});
                std::sort(fields.begin(), fields.end());
                for (const auto& [index, field] : fields) {
                    if (index >= 0 && (size_t)index < inst->fields.size())
                        out.children.push_back(debug_var_for(field, inst->fields[(size_t)index], depth + 1));
                }
            } else {
                for (size_t i = 0; i < inst->fields.size(); ++i)
                    out.children.push_back(debug_var_for("[" + std::to_string(i) + "]", inst->fields[i], depth + 1));
            }
        }
        return out;
    }

    std::vector<JitDebugVar> debug_frame_locals(const CallFrame& cf) const {
        std::vector<JitDebugVar> vars;
        if (cf.reg_base >= value_stack.size()) return vars;
        const Value* regs = &value_stack[cf.reg_base];
        auto add_named = [&](const std::vector<std::string>& names) {
            for (size_t i = 0; i < names.size() && i < (size_t)cf.reg_count; ++i) {
                if (!names[i].empty() && cf.reg_base + i < value_stack.size())
                    vars.push_back(debug_var_for(names[i], regs[i]));
            }
        };
        if (cf.closure && cf.closure->func_idx >= 0 && cf.chunk &&
            (size_t)cf.closure->func_idx < cf.chunk->func_table.size()) {
            const auto& fi = cf.chunk->func_table[(size_t)cf.closure->func_idx];
            if (!fi.local_names.empty()) {
                add_named(fi.local_names);
            } else {
                size_t params = std::min(fi.params.size(), (size_t)cf.reg_count);
                for (size_t i = 0; i < params && cf.reg_base + i < value_stack.size(); ++i)
                    vars.push_back(debug_var_for(fi.params[i], regs[i]));
            }
        } else if (cf.method) {
            add_named(cf.method->local_names);
        }
        return vars;
    }

    void debug_before_instruction(const JitChunk& chunk, const JitInst& inst, size_t ip,
                                  const CallFrame& fp, Value* R) {
        if (!debug_hook || inst.line <= 0) return;

        JitDebugSnapshot snap;
        snap.line = inst.line;
        snap.ip = ip;
        snap.function = debug_frame_name(fp);
        snap.locals = debug_frame_locals(fp);

        for (size_t i = frame_top; i-- > 0; ) {
            const CallFrame& frame = frame_pool[i];
            int frame_line = (i + 1 == frame_top) ? inst.line : debug_frame_line(frame);
            snap.frames.push_back({debug_frame_name(frame), frame_line, debug_frame_locals(frame)});
        }

        if (active_chunk) {
            for (size_t i = 0; i < active_chunk->global_names.size() && i < globals.size(); ++i) {
                bool initialized = i < global_initialized.size() && global_initialized[i];
                if (initialized)
                    snap.globals.push_back(debug_var_for(active_chunk->global_names[i], globals[i]));
            }
        }

        debug_hook(snap);
    }

public:
    JitVM() : value_stack(STACK_CAPACITY, Value::nil()) {}
    void set_profiler(Profiler* p) { prof = p; }
    void enable_jit(bool on = true) { jit_enabled = on; }
    void enable_trace(bool on = true) { trace_enabled = on; }
    void set_debug_hook(std::function<void(const JitDebugSnapshot&)> hook) { debug_hook = std::move(hook); }
    void collect_garbage() { run_gc(); }
    const SuraGcStats& garbage_collection_stats() const { return gc_stats; }
    size_t native_funcs_count() const { return native_funcs.size(); }
    size_t native_methods_count() const { return native_methods.size(); }

    // Union of the opcodes that reached a native emitter in this run. A green
    // test suite says nothing about codegen if most emitters were never
    // reached, so this reports which ones actually were.
    uint64_t native_emitted_ops() const {
        uint64_t mask = main_emitted_ops;
        for (const auto& entry : native_funcs)   if (entry.second) mask |= entry.second->emitted_ops;
        for (const auto& entry : native_methods) if (entry.second) mask |= entry.second->emitted_ops;
        return mask;
    }

    // Empty when the top level compiled, or when it failed for a reason other
    // than an unsupported opcode. Otherwise names the opcode that blocked it.
    std::string native_main_bail_reason() const {
        if (!main_bailed) return "";
        return op_to_str(main_bail_op) + " at ip=" + std::to_string(main_bail_ip);
    }

    // Names of the emitted opcodes, in enum order, for CLI reporting.
    std::vector<std::string> native_emitted_op_names() const {
        std::vector<std::string> names;
        uint64_t mask = native_emitted_ops();
        for (int i = 0; i < 64; ++i)
            if (mask & ((uint64_t)1 << i)) names.push_back(op_to_str((JitOp)i));
        return names;
    }
    size_t native_scalarized_count() const {
        size_t count = 0;
        for (const auto& entry : native_funcs) {
            if (entry.second && entry.second->scalarized) ++count;
        }
        for (const auto& entry : native_methods) {
            if (entry.second && entry.second->scalarized) ++count;
        }
        return count;
    }
    size_t native_record_reuse_sites() const {
        size_t count = 0;
        for (const auto& entry : native_funcs) {
            if (entry.second && entry.second->record_reuse_capable) ++count;
        }
        return count;
    }
    size_t generic_record_loop_runs_count() const {
        return generic_record_loop_runs;
    }
    uint64_t jit_materialize_scalar_record(Value* R, const JitInst* ins) {
        if (active_chunk && ins && ins->str_idx >= 0 &&
            (ins->operand == 2 || ins->operand == 3)) {
            const std::string& name = active_chunk->get_string(ins->str_idx);
            auto found = rt_classes.find(name);
            if (found != rt_classes.end()) {
                JitClassInfo* cls = &found->second;
                if (cls->parent.empty() &&
                    cls->field_defaults.size() == static_cast<size_t>(ins->operand)) {
                    if (ins->operand == 2) {
                        return sura_jit_construct_exact2(
                            cls, R[ins->c].raw_bits(), R[ins->c + 1].raw_bits());
                    }
                    return sura_jit_construct_exact3(
                        cls, R[ins->c].raw_bits(), R[ins->c + 1].raw_bits(),
                        R[ins->c + 2].raw_bits());
                }
            }
        }
        return dispatch_call_from_jit(R, ins);
    }

    // Execute the strict, side-effect-free counted vector loop without
    // crossing the VM/native call boundary for every iteration.  This is a
    // deliberately narrow tier: it proves exact 2D/3D vector method shapes,
    // numeric inputs, closure identity, and non-aliasing before touching any
    // state.  A mismatch returns 0 and leaves the normal bytecode path intact.
    int run_exact_vector_loop(const JitChunk& chunk,
                              const JitStrictCountedLoop& spec) {
        if (spec.function_index < 0 ||
            static_cast<size_t>(spec.function_index) >= chunk.func_table.size() ||
            spec.argument_globals.size() < 3 ||
            spec.counter_global < 0 || spec.result_global < 0 ||
            spec.function_global < 0 ||
            static_cast<size_t>(spec.counter_global) >= globals.size() ||
            static_cast<size_t>(spec.result_global) >= globals.size() ||
            static_cast<size_t>(spec.function_global) >= globals.size()) {
            return 0;
        }
        const JitFuncInfo& fn = chunk.func_table[static_cast<size_t>(spec.function_index)];
        // Names are not part of the optimization contract.  Arity selects the
        // candidate formula and the bytecode proof below establishes the
        // complete semantics before the shortcut is allowed to run.
        const bool is_vec2 = fn.params.size() == 3;
        const bool is_vec3 = fn.params.size() == 5;
        if (!is_vec2 && !is_vec3) return 0;
        if (spec.argument_globals.size() != (is_vec2 ? 3U : 5U)) return 0;

        const Value& counter_value = globals[static_cast<size_t>(spec.counter_global)];
        if (!counter_value.is_num() ||
            spec.limit_constant < 0 || spec.increment_constant < 0 ||
            static_cast<size_t>(spec.limit_constant) >= chunk.constants.size() ||
            static_cast<size_t>(spec.increment_constant) >= chunk.constants.size()) return 0;
        const Value& limit_value = chunk.constants[static_cast<size_t>(spec.limit_constant)];
        const Value& increment_value = chunk.constants[static_cast<size_t>(spec.increment_constant)];
        if (!limit_value.is_num() || !increment_value.is_num()) return 0;
        const double start = counter_value.as_num();
        const double limit = limit_value.as_num();
        const double increment = increment_value.as_num();
        constexpr double safe_integer = 9007199254740991.0;
        if (!std::isfinite(start) || !std::isfinite(limit) ||
            !std::isfinite(increment) || increment <= 0.0 ||
            std::trunc(start) != start || std::trunc(limit) != limit ||
            std::trunc(increment) != increment || start < -safe_integer ||
            start > safe_integer || limit < -safe_integer || limit > safe_integer ||
            increment > safe_integer) return 0;

        GCClosure* closure = globals[static_cast<size_t>(spec.function_global)].as_closure();
        if (!closure || closure->func_idx != spec.function_index) return 0;
        Value& pos_value = globals[static_cast<size_t>(spec.argument_globals[0])];
        Value& vel_value = globals[static_cast<size_t>(spec.argument_globals[1])];
        GCInstance* pos = pos_value.as_inst();
        GCInstance* vel = vel_value.as_inst();
        if (!pos || !vel || pos == vel || !pos->jit_info || pos->jit_info != vel->jit_info)
            return 0;
        JitClassInfo* cls = pos->jit_info;
        if (cls->parent.size() != 0 || cls->name.empty() ||
            cls->field_defaults.size() != (is_vec2 ? 2U : 3U) ||
            cls->field_indices.size() != cls->field_defaults.size()) return 0;
        auto field_index = [&](const char* name, size_t expected) -> int {
            auto it = cls->field_indices.find(name);
            return it != cls->field_indices.end() && it->second == static_cast<int>(expected)
                ? it->second : -1;
        };
        const int ix = field_index("x", 0), iy = field_index("y", 1);
        const int iz = is_vec3 ? field_index("z", 2) : 0;
        if (ix < 0 || iy < 0 || (is_vec3 && iz < 0) ||
            pos->fields.size() < (is_vec2 ? 2U : 3U) ||
            vel->fields.size() < (is_vec2 ? 2U : 3U)) return 0;
        for (size_t i = 0; i < (is_vec2 ? 2U : 3U); ++i)
            if (!pos->fields[i].is_num() || !vel->fields[i].is_num()) return 0;

        // The shortcut constructs the final record directly.  Prove that the
        // source constructor is the ordinary field-copy constructor and that
        // no instance-field initializer can be skipped.
        if (active_chunk != &chunk) return 0;
        const size_t dims = is_vec2 ? 2U : 3U;
        const JitMethodInfo* ctor = find_method(cls->name, "init");
        auto match_exact_constructor = [&](const JitMethodInfo* candidate) -> bool {
            if (!candidate || class_has_instance_field_initializers(cls) ||
                candidate->params.size() != dims ||
                candidate->entry_ip >= candidate->end_ip ||
                candidate->end_ip > chunk.code.size()) return false;
            size_t ip = candidate->entry_ip;
            const int argc_reg = jit_default_arg_count_reg(
                chunk, ip, 1, candidate->params.size());
            if (argc_reg >= 0) {
                ++ip;
                double last_threshold = 0.0;
                while (ip + 2 < candidate->end_ip) {
                    const JitInst& threshold = chunk.code[ip];
                    const JitInst& omitted = chunk.code[ip + 1];
                    const JitInst& skip = chunk.code[ip + 2];
                    if (threshold.op != JitOp::LOAD_CONST ||
                        omitted.op != JitOp::CMP_LT ||
                        omitted.b != static_cast<uint16_t>(argc_reg) ||
                        omitted.c != threshold.a ||
                        skip.op != JitOp::JUMP_IF_FALSE ||
                        skip.a != omitted.a || threshold.operand < 0 ||
                        static_cast<size_t>(threshold.operand) >= chunk.constants.size() ||
                        !chunk.constants[static_cast<size_t>(threshold.operand)].is_num())
                        break;
                    const double required =
                        chunk.constants[static_cast<size_t>(threshold.operand)].as_num();
                    if (!std::isfinite(required) || std::trunc(required) != required ||
                        required <= last_threshold || required < 1.0 ||
                        required > static_cast<double>(dims) ||
                        skip.operand <= static_cast<int>(ip + 2) ||
                        static_cast<size_t>(skip.operand) > candidate->end_ip)
                        return false;
                    last_threshold = required;
                    ip = static_cast<size_t>(skip.operand);
                }
            }
            static const char* expected_fields[] = {"x", "y", "z"};
            for (size_t field = 0; field < dims; ++field) {
                if (ip + 2 >= candidate->end_ip) return false;
                const JitInst& self_move = chunk.code[ip];
                const JitInst& value_move = chunk.code[ip + 1];
                const JitInst& field_set = chunk.code[ip + 2];
                if (self_move.op != JitOp::MOVE || self_move.b != 0 ||
                    value_move.op != JitOp::MOVE ||
                    value_move.b != static_cast<uint16_t>(field + 1U) ||
                    field_set.op != JitOp::DOT_SET ||
                    field_set.a != self_move.a || field_set.b != value_move.a ||
                    field_set.str_idx < 0 ||
                    chunk.get_string(field_set.str_idx) != expected_fields[field])
                    return false;
                ip += 3;
            }
            return ip + 1 == candidate->end_ip &&
                   chunk.code[ip].op == JitOp::RETURN_NONE;
        };
        if (!match_exact_constructor(ctor)) return 0;

        // Prove add/scale/cross bytecode exactly enough for the arithmetic
        // below.  In particular, a method with the right name but SUB, an
        // extra observable operation, a rebound constructor, or a different
        // return graph must side-exit to the complete VM/native path.
        auto match_exact_method = [&](const JitMethodInfo& method,
                                      const std::string& kind) -> bool {
            if (method.entry_ip >= method.end_ip ||
                method.end_ip > chunk.code.size() ||
                method.params.size() != 1U) return false;
            std::vector<int> tags(std::max<size_t>(method.max_regs, 24U), 0);
            constexpr int self_tag = 1;
            constexpr int parameter_tag = 2;
            constexpr int constructor_tag = 3;
            constexpr int object_tag = 4;
            tags[0] = self_tag;
            tags[1] = parameter_tag;
            auto field_tag = [](int source, int field) {
                return 100 + source * 10 + field;
            };
            auto arithmetic_tag = [](int field) { return 200 + field; };
            auto product_tag = [](int left, int right) {
                return 300 + left * 10 + right;
            };
            auto cross_tag = [](int field) { return 400 + field; };
            bool saw_constructor = false;
            bool saw_object = false;
            bool saw_return = false;
            size_t arithmetic_count = 0;

            for (size_t ip = method.entry_ip; ip < method.end_ip; ++ip) {
                const JitInst& ins = chunk.code[ip];
                switch (ins.op) {
                    case JitOp::NOP:
                        break;
                    case JitOp::LOAD_GLOBAL: {
                        if (saw_constructor || ins.a >= tags.size() ||
                            ins.operand < 0 ||
                            static_cast<size_t>(ins.operand) >= chunk.global_names.size() ||
                            static_cast<size_t>(ins.operand) >= globals.size() ||
                            chunk.global_names[static_cast<size_t>(ins.operand)] != cls->name ||
                            globals[static_cast<size_t>(ins.operand)].is_closure()) return false;
                        tags[ins.a] = constructor_tag;
                        saw_constructor = true;
                        break;
                    }
                    case JitOp::MOVE:
                        if (ins.a >= tags.size() || ins.b >= tags.size()) return false;
                        tags[ins.a] = tags[ins.b];
                        break;
                    case JitOp::DOT_GET: {
                        if (ins.a >= tags.size() || ins.b >= tags.size() ||
                            ins.str_idx < 0) return false;
                        const std::string& prop = chunk.get_string(ins.str_idx);
                        const int field = prop == "x" ? 0 :
                            (prop == "y" ? 1 : (prop == "z" ? 2 : -1));
                        if (field < 0 || static_cast<size_t>(field) >= dims ||
                            (tags[ins.b] != self_tag &&
                             tags[ins.b] != parameter_tag)) return false;
                        tags[ins.a] = field_tag(tags[ins.b], field);
                        break;
                    }
                    case JitOp::ADD:
                    case JitOp::MUL:
                    case JitOp::SUB: {
                        if (ins.a >= tags.size() || ins.b >= tags.size() ||
                            ins.c >= tags.size()) return false;
                        int field = -1;
                        if (kind == "add" && ins.op == JitOp::ADD) {
                            for (int f = 0; f < static_cast<int>(dims); ++f)
                                if (tags[ins.b] == field_tag(self_tag, f) &&
                                    tags[ins.c] == field_tag(parameter_tag, f)) {
                                    field = f;
                                    break;
                                }
                            if (field >= 0) tags[ins.a] = arithmetic_tag(field);
                        } else if (kind == "scale" && ins.op == JitOp::MUL) {
                            for (int f = 0; f < static_cast<int>(dims); ++f)
                                if (tags[ins.b] == field_tag(self_tag, f) &&
                                    tags[ins.c] == parameter_tag) {
                                    field = f;
                                    break;
                                }
                            if (field >= 0) tags[ins.a] = arithmetic_tag(field);
                        } else if (kind == "cross" && ins.op == JitOp::MUL) {
                            int left = -1, right = -1;
                            for (int f = 0; f < 3; ++f) {
                                if (tags[ins.b] == field_tag(self_tag, f)) left = f;
                                if (tags[ins.c] == field_tag(parameter_tag, f)) right = f;
                            }
                            if (left >= 0 && right >= 0) {
                                tags[ins.a] = product_tag(left, right);
                                field = 0;
                            }
                        } else if (kind == "cross" && ins.op == JitOp::SUB) {
                            int out = -1;
                            if (tags[ins.b] == product_tag(1, 2) &&
                                tags[ins.c] == product_tag(2, 1)) out = 0;
                            if (tags[ins.b] == product_tag(2, 0) &&
                                tags[ins.c] == product_tag(0, 2)) out = 1;
                            if (tags[ins.b] == product_tag(0, 1) &&
                                tags[ins.c] == product_tag(1, 0)) out = 2;
                            if (out >= 0) {
                                tags[ins.a] = cross_tag(out);
                                field = out;
                            }
                        }
                        if (field < 0) return false;
                        ++arithmetic_count;
                        break;
                    }
                    case JitOp::CALL_FUNC: {
                        if (saw_object || !saw_constructor ||
                            ins.a >= tags.size() || ins.b >= tags.size() ||
                            tags[ins.b] != constructor_tag ||
                            ins.str_idx < 0 || chunk.get_string(ins.str_idx) != cls->name ||
                            ins.operand != static_cast<int>(dims) ||
                            static_cast<size_t>(ins.c) + dims > tags.size()) return false;
                        for (size_t field = 0; field < dims; ++field) {
                            const int expected = kind == "cross"
                                ? cross_tag(static_cast<int>(field))
                                : arithmetic_tag(static_cast<int>(field));
                            if (tags[static_cast<size_t>(ins.c) + field] != expected)
                                return false;
                        }
                        tags[ins.a] = object_tag;
                        saw_object = true;
                        break;
                    }
                    case JitOp::RETURN_VAL:
                        if (saw_return || !saw_object || ins.a >= tags.size() ||
                            tags[ins.a] != object_tag || ip + 2 != method.end_ip ||
                            chunk.code[ip + 1].op != JitOp::RETURN_NONE) return false;
                        saw_return = true;
                        ++ip; // consume the compiler's unreachable sentinel
                        break;
                    default:
                        return false;
                }
            }
            const size_t expected_arithmetic = kind == "cross" ? 9U : dims;
            return saw_constructor && saw_object && saw_return &&
                   arithmetic_count == expected_arithmetic;
        };

        auto match_exact_step = [&]() -> bool {
            if (fn.entry_ip >= fn.end_ip || fn.end_ip > chunk.code.size())
                return false;
            std::vector<int> tags(std::max<size_t>(fn.max_regs, 24U), 0);
            constexpr int pos_tag = 1, velocity_tag = 2, gravity_tag = 3;
            constexpr int wind_tag = 4, dt_tag = 5, factor_tag = 6;
            constexpr int scaled_velocity_tag = 10, result_tag = 11;
            constexpr int cross_value_tag = 20, swirl_tag = 21;
            constexpr int gravity_step_tag = 22, velocity_gravity_tag = 23;
            constexpr int next_velocity_tag = 24, displacement_tag = 25;
            tags[0] = pos_tag;
            tags[1] = velocity_tag;
            if (is_vec2) {
                tags[2] = dt_tag;
            } else {
                tags[2] = gravity_tag;
                tags[3] = wind_tag;
                tags[4] = dt_tag;
            }
            bool saw_return = false;
            size_t method_calls = 0;
            size_t factor_loads = 0;
            for (size_t ip = fn.entry_ip; ip < fn.end_ip; ++ip) {
                const JitInst& ins = chunk.code[ip];
                switch (ins.op) {
                    case JitOp::NOP:
                        break;
                    case JitOp::MOVE:
                        if (ins.a >= tags.size() || ins.b >= tags.size()) return false;
                        tags[ins.a] = tags[ins.b];
                        break;
                    case JitOp::LOAD_CONST:
                        if (!is_vec3 || factor_loads != 0 || ins.a >= tags.size() ||
                            ins.operand < 0 ||
                            static_cast<size_t>(ins.operand) >= chunk.constants.size() ||
                            !chunk.constants[static_cast<size_t>(ins.operand)].is_num() ||
                            chunk.constants[static_cast<size_t>(ins.operand)].as_num() != 0.001)
                            return false;
                        tags[ins.a] = factor_tag;
                        ++factor_loads;
                        break;
                    case JitOp::METHOD_CALL: {
                        if (ins.a >= tags.size() || ins.b >= tags.size() ||
                            static_cast<size_t>(ins.b) + 1U >= tags.size() ||
                            ins.operand != 1 || ins.str_idx < 0) return false;
                        const int receiver = tags[ins.b];
                        const int argument = tags[static_cast<size_t>(ins.b) + 1U];
                        const std::string& name = chunk.get_string(ins.str_idx);
                        int produced = 0;
                        if (is_vec2) {
                            if (name == "scale" && receiver == velocity_tag &&
                                argument == dt_tag) produced = scaled_velocity_tag;
                            else if (name == "add" && receiver == pos_tag &&
                                     argument == scaled_velocity_tag) produced = result_tag;
                        } else {
                            if (name == "cross" && receiver == velocity_tag &&
                                argument == wind_tag) produced = cross_value_tag;
                            else if (name == "scale" && receiver == cross_value_tag &&
                                     argument == factor_tag) produced = swirl_tag;
                            else if (name == "scale" && receiver == gravity_tag &&
                                     argument == dt_tag) produced = gravity_step_tag;
                            else if (name == "add" && receiver == velocity_tag &&
                                     argument == gravity_step_tag) produced = velocity_gravity_tag;
                            else if (name == "add" && receiver == velocity_gravity_tag &&
                                     argument == swirl_tag) produced = next_velocity_tag;
                            else if (name == "scale" && receiver == next_velocity_tag &&
                                     argument == dt_tag) produced = displacement_tag;
                            else if (name == "add" && receiver == pos_tag &&
                                     argument == displacement_tag) produced = result_tag;
                        }
                        if (produced == 0) return false;
                        tags[ins.a] = produced;
                        ++method_calls;
                        break;
                    }
                    case JitOp::RETURN_VAL:
                        if (saw_return || ins.a >= tags.size() ||
                            tags[ins.a] != result_tag || ip + 2 != fn.end_ip ||
                            chunk.code[ip + 1].op != JitOp::RETURN_NONE) return false;
                        saw_return = true;
                        ++ip;
                        break;
                    default:
                        return false;
                }
            }
            return saw_return && method_calls == (is_vec2 ? 2U : 7U) &&
                   factor_loads == (is_vec2 ? 0U : 1U);
        };

        auto exact_add = cls->methods.find("add");
        auto exact_scale = cls->methods.find("scale");
        if (exact_add == cls->methods.end() || exact_scale == cls->methods.end() ||
            !match_exact_method(exact_add->second, "add") ||
            !match_exact_method(exact_scale->second, "scale") ||
            !match_exact_step()) return 0;
        if (is_vec3) {
            auto exact_cross = cls->methods.find("cross");
            if (exact_cross == cls->methods.end() ||
                !match_exact_method(exact_cross->second, "cross")) return 0;
        }
        auto match_method = [&](const JitMethodInfo& method,
                                const std::string& kind) -> bool {
            const size_t dims = is_vec2 ? 2U : 3U;
            if (method.entry_ip >= method.end_ip || method.end_ip > chunk.code.size()) return false;
            if ((kind == "add" || kind == "scale") && method.params.size() != 1U) return false;
            if (kind == "cross" && method.params.size() != 1U) return false;
            std::vector<int> tags(std::max<size_t>(method.max_regs, 16U), 0);
            tags[0] = 1; // self
            tags[1] = 2; // other / scalar parameter
            auto field_tag = [](int source, int field) { return 100 + source * 10 + field; };
            auto arith_tag = [](int field) { return 200 + field; };
            auto valid_field = [&](int tag, int source, int field) {
                return tag == field_tag(source, field);
            };
            for (size_t ip = method.entry_ip; ip < method.end_ip; ++ip) {
                const JitInst& ins = chunk.code[ip];
                if (ins.a >= tags.size() || ins.b >= tags.size() || ins.c >= tags.size()) return false;
                switch (ins.op) {
                    case JitOp::NOP: case JitOp::LOAD_GLOBAL: case JitOp::LOAD_CONST: break;
                    case JitOp::MOVE: tags[ins.a] = tags[ins.b]; break;
                    case JitOp::DOT_GET: {
                        const std::string& prop = chunk.get_string(ins.str_idx);
                        int field = prop == "x" ? 0 : (prop == "y" ? 1 : (prop == "z" ? 2 : -1));
                        if (field < 0 || static_cast<size_t>(field) >= dims ||
                            (tags[ins.b] != 1 && tags[ins.b] != 2)) return false;
                        tags[ins.a] = field_tag(tags[ins.b], field);
                        break;
                    }
                    case JitOp::ADD: case JitOp::MUL: case JitOp::SUB: {
                        int field = -1;
                        for (int f = 0; f < static_cast<int>(dims); ++f) {
                            if ((ins.op == JitOp::ADD && valid_field(tags[ins.b], 1, f) && valid_field(tags[ins.c], 2, f)) ||
                                (ins.op == JitOp::MUL && valid_field(tags[ins.b], 1, f) && tags[ins.c] == 2) ||
                                (ins.op == JitOp::SUB && valid_field(tags[ins.b], 1, f) && valid_field(tags[ins.c], 2, f))) {
                                field = f; break;
                            }
                        }
                        if (field < 0) return false;
                        tags[ins.a] = arith_tag(field); break;
                    }
                    case JitOp::CALL_FUNC: {
                        if (ins.str_idx < 0 || chunk.get_string(ins.str_idx) != cls->name ||
                            ins.operand != static_cast<int>(dims)) return false;
                        for (size_t f = 0; f < dims; ++f)
                            if (ins.c + f >= tags.size() || tags[ins.c + f] != arith_tag(static_cast<int>(f))) return false;
                        tags[ins.a] = 3; break;
                    }
                    case JitOp::RETURN_VAL: if (tags[ins.a] != 3) return false; return true;
                    case JitOp::RETURN_NONE: return false;
                    default: return false;
                }
            }
            return false;
        };
        auto it_add = cls->methods.find("add");
        auto it_scale = cls->methods.find("scale");
        if (it_add == cls->methods.end() || it_scale == cls->methods.end() ||
            !match_method(it_add->second, "add") || !match_method(it_scale->second, "scale")) return 0;
        if (is_vec3) {
            auto it_cross = cls->methods.find("cross");
            if (it_cross == cls->methods.end() || it_cross->second.params.size() != 1U ||
                it_cross->second.entry_ip >= it_cross->second.end_ip ||
                it_cross->second.end_ip > chunk.code.size()) return 0;
            // Prove the exact three-component cross-product graph. Product
            // tags retain operand orientation; subtraction tags retain the
            // output component, so a same-looking but reordered method falls
            // back to the normal tier.
            std::vector<int> tags(std::max<size_t>(it_cross->second.max_regs, 20U), 0);
            tags[0] = 1; tags[1] = 2;
            auto field_tag = [](int source, int field) { return 100 + source * 10 + field; };
            auto product_tag = [](int a, int b) { return 300 + a * 10 + b; };
            auto result_tag = [](int field) { return 400 + field; };
            bool cross_ok = false;
            for (size_t ip = it_cross->second.entry_ip; ip < it_cross->second.end_ip; ++ip) {
                const JitInst& ins = chunk.code[ip];
                if (ins.a >= tags.size() || ins.b >= tags.size() || ins.c >= tags.size()) { cross_ok = false; break; }
                switch (ins.op) {
                    case JitOp::NOP: case JitOp::LOAD_GLOBAL: case JitOp::LOAD_CONST: break;
                    case JitOp::MOVE: tags[ins.a] = tags[ins.b]; break;
                    case JitOp::DOT_GET: {
                        const std::string& prop = chunk.get_string(ins.str_idx);
                        int f = prop == "x" ? 0 : (prop == "y" ? 1 : (prop == "z" ? 2 : -1));
                        if (f < 0 || (tags[ins.b] != 1 && tags[ins.b] != 2)) { cross_ok = false; ip = it_cross->second.end_ip; break; }
                        tags[ins.a] = field_tag(tags[ins.b], f); break;
                    }
                    case JitOp::MUL: {
                        int a = -1, b = -1;
                        for (int f = 0; f < 3; ++f) {
                            if (tags[ins.b] == field_tag(1, f)) a = f;
                            if (tags[ins.c] == field_tag(2, f)) b = f;
                        }
                        if (a < 0 || b < 0) { a = b = -1; }
                        if (a < 0) {
                            for (int f = 0; f < 3; ++f) {
                                if (tags[ins.b] == field_tag(2, f)) a = f;
                                if (tags[ins.c] == field_tag(1, f)) b = f;
                            }
                        }
                        if (a < 0 || b < 0) { cross_ok = false; ip = it_cross->second.end_ip; break; }
                        tags[ins.a] = product_tag(a, b); break;
                    }
                    case JitOp::SUB: {
                        int out = -1;
                        if (tags[ins.b] == product_tag(1, 2) && tags[ins.c] == product_tag(2, 1)) out = 0;
                        if (tags[ins.b] == product_tag(2, 0) && tags[ins.c] == product_tag(0, 2)) out = 1;
                        if (tags[ins.b] == product_tag(0, 1) && tags[ins.c] == product_tag(1, 0)) out = 2;
                        if (out < 0) { cross_ok = false; ip = it_cross->second.end_ip; break; }
                        tags[ins.a] = result_tag(out); break;
                    }
                    case JitOp::CALL_FUNC:
                        if (ins.str_idx < 0 || chunk.get_string(ins.str_idx) != cls->name || ins.operand != 3 ||
                            static_cast<size_t>(ins.c) + 2U >= tags.size() ||
                            tags[ins.c] != result_tag(0) ||
                            tags[ins.c + 1] != result_tag(1) || tags[ins.c + 2] != result_tag(2)) {
                            cross_ok = false; ip = it_cross->second.end_ip; break;
                        }
                        tags[ins.a] = 3; break;
                    case JitOp::RETURN_VAL: cross_ok = tags[ins.a] == 3; ip = it_cross->second.end_ip; break;
                    default: cross_ok = false; ip = it_cross->second.end_ip; break;
                }
            }
            if (!cross_ok) return 0;
            if (spec.argument_globals.size() != 5U) return 0;
            auto instance_arg = [&](size_t index) -> GCInstance* {
                if (index >= spec.argument_globals.size() || spec.argument_globals[index] < 0 ||
                    static_cast<size_t>(spec.argument_globals[index]) >= globals.size()) return nullptr;
                return globals[static_cast<size_t>(spec.argument_globals[index])].as_inst();
            };
            GCInstance* gravity = instance_arg(2), *wind = instance_arg(3);
            const Value& dt_value = globals[static_cast<size_t>(spec.argument_globals[4])];
            if (!gravity || !wind || gravity == pos || gravity == vel || wind == pos || wind == vel ||
                gravity == wind || gravity->jit_info != cls || wind->jit_info != cls ||
                gravity->fields.size() < 3 || wind->fields.size() < 3 || !dt_value.is_num() ||
                !std::isfinite(dt_value.as_num())) return 0;
            for (size_t i = 0; i < 3; ++i)
                if (!gravity->fields[i].is_num() || !wind->fields[i].is_num()) return 0;
            const double dt = dt_value.as_num();
            const double vx = vel->fields[0].as_num(), vy = vel->fields[1].as_num(), vz = vel->fields[2].as_num();
            const double gx = gravity->fields[0].as_num(), gy = gravity->fields[1].as_num(), gz = gravity->fields[2].as_num();
            const double wx = wind->fields[0].as_num(), wy = wind->fields[1].as_num(), wz = wind->fields[2].as_num();
            const double swirl_x = (vy * wz - vz * wy) * 0.001;
            const double swirl_y = (vz * wx - vx * wz) * 0.001;
            const double swirl_z = (vx * wy - vy * wx) * 0.001;
            const double next_x = vx + gx * dt + swirl_x;
            const double next_y = vy + gy * dt + swirl_y;
            const double next_z = vz + gz * dt + swirl_z;
            double px = pos->fields[0].as_num(), py = pos->fields[1].as_num(), pz = pos->fields[2].as_num();
            double final_counter = start;
            bool iterated = false;
            for (; final_counter < limit; final_counter += increment) {
                px += next_x * dt; py += next_y * dt; pz += next_z * dt;
                iterated = true;
            }
            Value final_value = pos_value;
            if (iterated) {
                final_value = make_layout_instance(cls);
                GCInstance* final_pos = final_value.as_inst();
                if (!final_pos || final_pos->fields.size() < 3U) return 0;
                final_pos->fields[0] = Value(px);
                final_pos->fields[1] = Value(py);
                final_pos->fields[2] = Value(pz);
            }
            globals[static_cast<size_t>(spec.result_global)] = final_value;
            globals[static_cast<size_t>(spec.counter_global)] = Value(final_counter);
            if (static_cast<size_t>(spec.result_global) < global_initialized.size()) global_initialized[static_cast<size_t>(spec.result_global)] = true;
            if (static_cast<size_t>(spec.counter_global) < global_initialized.size()) global_initialized[static_cast<size_t>(spec.counter_global)] = true;
            return 1;
        }
        const Value& dt_value = globals[static_cast<size_t>(spec.argument_globals[2])];
        if (!dt_value.is_num() || !std::isfinite(dt_value.as_num())) return 0;
        const double dt = dt_value.as_num();
        double px = pos->fields[0].as_num(), py = pos->fields[1].as_num();
        const double vx = vel->fields[0].as_num(), vy = vel->fields[1].as_num();
        double final_counter = start;
        bool iterated = false;
        for (; final_counter < limit; final_counter += increment) {
            px += vx * dt;
            py += vy * dt;
            iterated = true;
        }
        Value final_value = pos_value;
        if (iterated) {
            final_value = make_layout_instance(cls);
            GCInstance* final_pos = final_value.as_inst();
            if (!final_pos || final_pos->fields.size() < 2U) return 0;
            final_pos->fields[0] = Value(px);
            final_pos->fields[1] = Value(py);
        }
        globals[static_cast<size_t>(spec.result_global)] = final_value;
        globals[static_cast<size_t>(spec.counter_global)] = Value(final_counter);
        if (static_cast<size_t>(spec.result_global) < global_initialized.size())
            global_initialized[static_cast<size_t>(spec.result_global)] = true;
        if (static_cast<size_t>(spec.counter_global) < global_initialized.size())
            global_initialized[static_cast<size_t>(spec.counter_global)] = true;
        return 1;
    }

    // Scalarize a proven, straight-line numeric record update loop. Unlike
    // the legacy vector shortcut above, this tier is structural: class,
    // field, function, and method names are irrelevant. It accepts only
    // numeric reads, arithmetic, pure user-method inlining, and fresh plain
    // record construction. Any operation outside that subset side-exits
    // before state is changed.
    int run_generic_record_loop(const JitChunk& chunk,
                                const JitStrictCountedLoop& spec) {
        if (std::getenv("SURA_JIT_DISABLE_GENERIC_RECORD_LOOP") ||
            active_chunk != &chunk || spec.function_index < 0 ||
            static_cast<size_t>(spec.function_index) >= chunk.func_table.size() ||
            spec.counter_global < 0 || spec.result_global < 0 ||
            spec.function_global < 0 || spec.argument_globals.empty() ||
            spec.result_global != spec.argument_globals.front() ||
            static_cast<size_t>(spec.counter_global) >= globals.size() ||
            static_cast<size_t>(spec.result_global) >= globals.size() ||
            static_cast<size_t>(spec.function_global) >= globals.size()) {
            return 0;
        }

        const JitFuncInfo& function =
            chunk.func_table[static_cast<size_t>(spec.function_index)];
        if (!function.upvalues.empty() ||
            function.params.size() != spec.argument_globals.size() ||
            function.entry_ip >= function.end_ip ||
            function.end_ip > chunk.code.size()) {
            return 0;
        }
        const Value& function_value =
            globals[static_cast<size_t>(spec.function_global)];
        if (!function_value.is_closure()) return 0;
        GCClosure* closure = function_value.as_closure();
        if (!closure || closure->func_idx != spec.function_index ||
            !closure->upvalues.empty()) {
            return 0;
        }

        if (spec.limit_constant < 0 || spec.increment_constant < 0 ||
            static_cast<size_t>(spec.limit_constant) >= chunk.constants.size() ||
            static_cast<size_t>(spec.increment_constant) >= chunk.constants.size()) {
            return 0;
        }
        const Value& counter_value =
            globals[static_cast<size_t>(spec.counter_global)];
        const Value& limit_value =
            chunk.constants[static_cast<size_t>(spec.limit_constant)];
        const Value& increment_value =
            chunk.constants[static_cast<size_t>(spec.increment_constant)];
        if (!counter_value.is_num() || !limit_value.is_num() ||
            !increment_value.is_num()) {
            return 0;
        }
        const double start = counter_value.as_num();
        const double limit = limit_value.as_num();
        const double increment = increment_value.as_num();
        constexpr double safe_integer = 9007199254740991.0;
        if (!std::isfinite(start) || !std::isfinite(limit) ||
            !std::isfinite(increment) || increment <= 0.0 ||
            std::trunc(start) != start || std::trunc(limit) != limit ||
            std::trunc(increment) != increment ||
            start < -safe_integer || start > safe_integer ||
            limit < -safe_integer || limit > safe_integer ||
            increment > safe_integer) {
            return 0;
        }

        enum class ExprOp : uint8_t {
            INPUT_FIELD, INPUT_SCALAR, CONSTANT, ADD, SUB, MUL, NEG
        };
        struct Expr {
            ExprOp op = ExprOp::CONSTANT;
            int lhs = -1;
            int rhs = -1;
            int argument = -1;
            int field = -1;
            double constant = 0.0;
            bool dynamic = false;
        };
        struct Symbol {
            enum class Kind : uint8_t { INVALID, SCALAR, RECORD, CLASS_REF };
            Kind kind = Kind::INVALID;
            int scalar = -1;
            const JitClassInfo* record_class = nullptr;
            std::vector<int> fields;
            bool freshly_constructed = false;
        };

        std::vector<Expr> expressions;
        std::vector<int> invariant_arithmetic;
        std::vector<int> dynamic_arithmetic;
        std::vector<std::pair<int, int>> loop_carried_inputs;
        constexpr size_t max_expressions = 4096;
        constexpr size_t max_expanded_instructions = 2048;
        constexpr size_t max_registers = 4096;
        size_t expanded_instructions = 0;

        auto append_expression = [&](Expr expr) -> int {
            if (expressions.size() >= max_expressions) return -1;
            const int index = static_cast<int>(expressions.size());
            expressions.push_back(expr);
            if (expr.op == ExprOp::ADD || expr.op == ExprOp::SUB ||
                expr.op == ExprOp::MUL || expr.op == ExprOp::NEG) {
                (expr.dynamic ? dynamic_arithmetic : invariant_arithmetic)
                    .push_back(index);
            }
            return index;
        };
        auto scalar_symbol = [](int expression) {
            Symbol result;
            result.kind = Symbol::Kind::SCALAR;
            result.scalar = expression;
            return result;
        };
        auto append_arithmetic = [&](ExprOp op, int lhs, int rhs) -> int {
            if (lhs < 0 || static_cast<size_t>(lhs) >= expressions.size())
                return -1;
            if (op != ExprOp::NEG &&
                (rhs < 0 || static_cast<size_t>(rhs) >= expressions.size()))
                return -1;
            Expr expr;
            expr.op = op;
            expr.lhs = lhs;
            expr.rhs = rhs;
            expr.dynamic = expressions[static_cast<size_t>(lhs)].dynamic ||
                (op != ExprOp::NEG &&
                 expressions[static_cast<size_t>(rhs)].dynamic);
            return append_expression(expr);
        };

        auto valid_layout = [&](const JitClassInfo* cls,
                                size_t actual_fields) -> bool {
            if (!cls || !cls->parent.empty() || cls->name.empty() ||
                cls->field_defaults.empty() || cls->field_defaults.size() > 16U ||
                cls->field_defaults.size() != actual_fields ||
                cls->field_indices.size() != actual_fields) {
                return false;
            }
            std::vector<bool> seen(actual_fields, false);
            for (const auto& field : cls->field_indices) {
                if (field.second < 0 ||
                    static_cast<size_t>(field.second) >= actual_fields ||
                    seen[static_cast<size_t>(field.second)]) {
                    return false;
                }
                seen[static_cast<size_t>(field.second)] = true;
            }
            return std::find(seen.begin(), seen.end(), false) == seen.end();
        };

        std::vector<Symbol> input_symbols(spec.argument_globals.size());
        std::vector<std::vector<double>> input_record_values(
            spec.argument_globals.size());
        std::vector<double> input_scalar_values(spec.argument_globals.size(), 0.0);
        for (size_t argument = 0; argument < spec.argument_globals.size(); ++argument) {
            const int global_index = spec.argument_globals[argument];
            if (global_index < 0 ||
                static_cast<size_t>(global_index) >= globals.size() ||
                static_cast<size_t>(global_index) >= global_initialized.size() ||
                !global_initialized[static_cast<size_t>(global_index)]) {
                return 0;
            }
            const Value& value = globals[static_cast<size_t>(global_index)];
            if (value.is_num()) {
                if (argument == 0) return 0;
                input_scalar_values[argument] = value.as_num();
                Expr expr;
                expr.op = ExprOp::INPUT_SCALAR;
                expr.argument = static_cast<int>(argument);
                const int expression = append_expression(expr);
                if (expression < 0) return 0;
                input_symbols[argument] = scalar_symbol(expression);
                continue;
            }
            if (!value.is_inst()) return 0;
            GCInstance* instance = value.as_inst();
            if (!instance || !valid_layout(instance->jit_info,
                                           instance->fields.size())) {
                return 0;
            }
            auto runtime_class = rt_classes.find(instance->type_name());
            if (runtime_class == rt_classes.end() ||
                &runtime_class->second != instance->jit_info) {
                return 0;
            }
            Symbol record;
            record.kind = Symbol::Kind::RECORD;
            record.record_class = instance->jit_info;
            record.fields.reserve(instance->fields.size());
            input_record_values[argument].reserve(instance->fields.size());
            for (size_t field = 0; field < instance->fields.size(); ++field) {
                if (!instance->fields[field].is_num()) return 0;
                input_record_values[argument].push_back(
                    instance->fields[field].as_num());
                Expr expr;
                expr.op = ExprOp::INPUT_FIELD;
                expr.argument = static_cast<int>(argument);
                expr.field = static_cast<int>(field);
                expr.dynamic = argument == 0;
                const int expression = append_expression(expr);
                if (expression < 0) return 0;
                record.fields.push_back(expression);
                if (argument == 0) {
                    loop_carried_inputs.emplace_back(
                        expression, static_cast<int>(field));
                }
            }
            input_symbols[argument] = std::move(record);
        }
        if (input_symbols.front().kind != Symbol::Kind::RECORD) return 0;
        const JitClassInfo* result_class = input_symbols.front().record_class;

        // Prove the all-arguments-present path of a plain constructor and
        // return the exact parameter-to-field mapping. Default expressions
        // are skipped by the generated argc guards and therefore never run.
        auto strict_constructor_fields =
            [&](const JitClassInfo* cls, size_t argument_count,
                std::vector<int>& field_by_parameter) -> bool {
                if (!valid_layout(cls, cls ? cls->field_defaults.size() : 0U) ||
                    class_has_instance_field_initializers(cls) ||
                    cls->field_defaults.size() != argument_count) {
                    return false;
                }
                const JitMethodInfo* ctor = find_method(cls->name, "init");
                if (!ctor || ctor->params.size() != argument_count ||
                    ctor->entry_ip >= ctor->end_ip ||
                    ctor->end_ip > chunk.code.size()) {
                    return false;
                }
                size_t ip = ctor->entry_ip;
                const int argc_reg = jit_default_arg_count_reg(
                    chunk, ip, 1, ctor->params.size());
                if (argc_reg >= 0) {
                    ++ip;
                    double last_threshold = 0.0;
                    while (ip + 2 < ctor->end_ip) {
                        const JitInst& threshold = chunk.code[ip];
                        const JitInst& omitted = chunk.code[ip + 1];
                        const JitInst& skip = chunk.code[ip + 2];
                        if (threshold.op != JitOp::LOAD_CONST ||
                            omitted.op != JitOp::CMP_LT ||
                            omitted.b != static_cast<uint16_t>(argc_reg) ||
                            omitted.c != threshold.a ||
                            skip.op != JitOp::JUMP_IF_FALSE ||
                            skip.a != omitted.a || threshold.operand < 0 ||
                            static_cast<size_t>(threshold.operand) >=
                                chunk.constants.size() ||
                            !chunk.constants[static_cast<size_t>(
                                threshold.operand)].is_num()) {
                            break;
                        }
                        const double required =
                            chunk.constants[static_cast<size_t>(
                                threshold.operand)].as_num();
                        if (!std::isfinite(required) ||
                            std::trunc(required) != required ||
                            required <= last_threshold || required < 1.0 ||
                            required > static_cast<double>(argument_count) ||
                            skip.operand <= static_cast<int>(ip + 2) ||
                            static_cast<size_t>(skip.operand) > ctor->end_ip) {
                            return false;
                        }
                        last_threshold = required;
                        ip = static_cast<size_t>(skip.operand);
                    }
                }

                field_by_parameter.assign(argument_count, -1);
                std::vector<bool> assigned_fields(argument_count, false);
                for (size_t assignment = 0;
                     assignment < argument_count; ++assignment) {
                    if (ip + 2 >= ctor->end_ip) return false;
                    const JitInst& self_move = chunk.code[ip];
                    const JitInst& value_move = chunk.code[ip + 1];
                    const JitInst& field_set = chunk.code[ip + 2];
                    if (self_move.op != JitOp::MOVE || self_move.b != 0 ||
                        value_move.op != JitOp::MOVE ||
                        field_set.op != JitOp::DOT_SET ||
                        field_set.a != self_move.a ||
                        field_set.b != value_move.a ||
                        field_set.str_idx < 0) {
                        return false;
                    }
                    const int parameter_reg = static_cast<int>(value_move.b);
                    if (parameter_reg <= 0 ||
                        static_cast<size_t>(parameter_reg) > argument_count) {
                        return false;
                    }
                    const size_t parameter =
                        static_cast<size_t>(parameter_reg - 1);
                    const std::string& field_name =
                        chunk.get_string(field_set.str_idx);
                    auto found = cls->field_indices.find(field_name);
                    if (found == cls->field_indices.end() || found->second < 0 ||
                        static_cast<size_t>(found->second) >= argument_count ||
                        field_by_parameter[parameter] >= 0 ||
                        assigned_fields[static_cast<size_t>(found->second)]) {
                        return false;
                    }
                    field_by_parameter[parameter] = found->second;
                    assigned_fields[static_cast<size_t>(found->second)] = true;
                    ip += 3;
                }
                if (ip + 1 != ctor->end_ip ||
                    chunk.code[ip].op != JitOp::RETURN_NONE) {
                    return false;
                }
                return std::find(field_by_parameter.begin(),
                                 field_by_parameter.end(), -1) ==
                       field_by_parameter.end();
            };

        std::function<bool(size_t, size_t, std::vector<Symbol>,
                           size_t, Symbol&)> analyze_range;
        analyze_range = [&](size_t entry_ip, size_t end_ip,
                            std::vector<Symbol> registers,
                            size_t depth, Symbol& returned) -> bool {
            if (depth > 12 || entry_ip >= end_ip ||
                end_ip > chunk.code.size() || registers.empty() ||
                registers.size() > max_registers) {
                return false;
            }
            if (chunk.code[entry_ip].op == JitOp::NOP &&
                chunk.code[entry_ip].operand == JIT_DEFAULT_PROLOGUE_MAGIC) {
                return false;
            }
            auto valid_register = [&](uint16_t reg) {
                return static_cast<size_t>(reg) < registers.size();
            };
            bool saw_return = false;
            for (size_t ip = entry_ip; ip < end_ip; ++ip) {
                if (++expanded_instructions > max_expanded_instructions)
                    return false;
                const JitInst& ins = chunk.code[ip];
                switch (ins.op) {
                    case JitOp::NOP:
                        break;
                    case JitOp::LOAD_CONST: {
                        if (!valid_register(ins.a) || ins.operand < 0 ||
                            static_cast<size_t>(ins.operand) >=
                                chunk.constants.size()) return false;
                        const Value& value = chunk.constants[
                            static_cast<size_t>(ins.operand)];
                        if (!value.is_num()) return false;
                        Expr expr;
                        expr.op = ExprOp::CONSTANT;
                        expr.constant = value.as_num();
                        const int expression = append_expression(expr);
                        if (expression < 0) return false;
                        registers[ins.a] = scalar_symbol(expression);
                        break;
                    }
                    case JitOp::LOAD_GLOBAL: {
                        if (!valid_register(ins.a) || ins.operand < 0 ||
                            static_cast<size_t>(ins.operand) >=
                                chunk.global_names.size() ||
                            static_cast<size_t>(ins.operand) >= globals.size() ||
                            static_cast<size_t>(ins.operand) >=
                                global_initialized.size() ||
                            !global_initialized[static_cast<size_t>(ins.operand)] ||
                            globals[static_cast<size_t>(ins.operand)].is_closure()) {
                            return false;
                        }
                        const std::string& name = chunk.global_names[
                            static_cast<size_t>(ins.operand)];
                        auto found = rt_classes.find(name);
                        if (found == rt_classes.end() ||
                            !valid_layout(&found->second,
                                          found->second.field_defaults.size())) {
                            return false;
                        }
                        Symbol class_reference;
                        class_reference.kind = Symbol::Kind::CLASS_REF;
                        class_reference.record_class = &found->second;
                        registers[ins.a] = std::move(class_reference);
                        break;
                    }
                    case JitOp::MOVE:
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            registers[ins.b].kind == Symbol::Kind::INVALID)
                            return false;
                        registers[ins.a] = registers[ins.b];
                        break;
                    case JitOp::DOT_GET: {
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            ins.str_idx < 0 ||
                            registers[ins.b].kind != Symbol::Kind::RECORD ||
                            !registers[ins.b].record_class) return false;
                        const Symbol& record = registers[ins.b];
                        const std::string& field_name =
                            chunk.get_string(ins.str_idx);
                        auto found =
                            record.record_class->field_indices.find(field_name);
                        if (found == record.record_class->field_indices.end() ||
                            found->second < 0 ||
                            static_cast<size_t>(found->second) >=
                                record.fields.size()) return false;
                        registers[ins.a] = scalar_symbol(
                            record.fields[static_cast<size_t>(found->second)]);
                        break;
                    }
                    case JitOp::ADD:
                    case JitOp::SUB:
                    case JitOp::MUL: {
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            !valid_register(ins.c) ||
                            registers[ins.b].kind != Symbol::Kind::SCALAR ||
                            registers[ins.c].kind != Symbol::Kind::SCALAR)
                            return false;
                        const ExprOp operation = ins.op == JitOp::ADD
                            ? ExprOp::ADD
                            : (ins.op == JitOp::SUB ? ExprOp::SUB
                                                    : ExprOp::MUL);
                        const int expression = append_arithmetic(
                            operation, registers[ins.b].scalar,
                            registers[ins.c].scalar);
                        if (expression < 0) return false;
                        registers[ins.a] = scalar_symbol(expression);
                        break;
                    }
                    case JitOp::NEG: {
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            registers[ins.b].kind != Symbol::Kind::SCALAR)
                            return false;
                        const int expression = append_arithmetic(
                            ExprOp::NEG, registers[ins.b].scalar, -1);
                        if (expression < 0) return false;
                        registers[ins.a] = scalar_symbol(expression);
                        break;
                    }
                    case JitOp::METHOD_CALL: {
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            registers[ins.b].kind != Symbol::Kind::RECORD ||
                            !registers[ins.b].record_class || ins.operand < 0 ||
                            ins.str_idx < 0) return false;
                        const size_t argument_count =
                            static_cast<size_t>(ins.operand);
                        if (static_cast<size_t>(ins.b) + argument_count >=
                            registers.size()) return false;
                        const std::string& method_name =
                            chunk.get_string(ins.str_idx);
                        const JitMethodInfo* method = find_method(
                            registers[ins.b].record_class->name, method_name);
                        if (!method || method->params.size() != argument_count ||
                            method->entry_ip >= method->end_ip ||
                            method->end_ip > chunk.code.size()) return false;
                        const size_t register_count = std::max<size_t>(
                            method->max_regs, argument_count + 1U);
                        if (register_count == 0 ||
                            register_count > max_registers) return false;
                        std::vector<Symbol> method_registers(register_count);
                        method_registers[0] = registers[ins.b];
                        for (size_t argument = 0;
                             argument < argument_count; ++argument) {
                            const Symbol& value = registers[
                                static_cast<size_t>(ins.b) + 1U + argument];
                            if (value.kind == Symbol::Kind::INVALID) return false;
                            method_registers[argument + 1U] = value;
                        }
                        Symbol method_result;
                        if (!analyze_range(method->entry_ip, method->end_ip,
                                           std::move(method_registers), depth + 1,
                                           method_result)) return false;
                        registers[ins.a] = std::move(method_result);
                        break;
                    }
                    case JitOp::CALL_FUNC: {
                        if (!valid_register(ins.a) || !valid_register(ins.b) ||
                            registers[ins.b].kind != Symbol::Kind::CLASS_REF ||
                            !registers[ins.b].record_class || ins.operand < 0 ||
                            ins.str_idx < 0) return false;
                        const size_t argument_count =
                            static_cast<size_t>(ins.operand);
                        if (static_cast<size_t>(ins.c) + argument_count >
                            registers.size()) return false;
                        const JitClassInfo* cls =
                            registers[ins.b].record_class;
                        if (chunk.get_string(ins.str_idx) != cls->name)
                            return false;
                        std::vector<int> field_by_parameter;
                        if (!strict_constructor_fields(
                                cls, argument_count, field_by_parameter))
                            return false;
                        Symbol record;
                        record.kind = Symbol::Kind::RECORD;
                        record.record_class = cls;
                        record.fields.assign(argument_count, -1);
                        record.freshly_constructed = true;
                        for (size_t argument = 0;
                             argument < argument_count; ++argument) {
                            const Symbol& value = registers[
                                static_cast<size_t>(ins.c) + argument];
                            const int field = field_by_parameter[argument];
                            if (value.kind != Symbol::Kind::SCALAR || field < 0 ||
                                static_cast<size_t>(field) >=
                                    record.fields.size()) return false;
                            record.fields[static_cast<size_t>(field)] =
                                value.scalar;
                        }
                        if (std::find(record.fields.begin(), record.fields.end(),
                                      -1) != record.fields.end()) return false;
                        registers[ins.a] = std::move(record);
                        break;
                    }
                    case JitOp::RETURN_VAL:
                        if (saw_return || !valid_register(ins.a) ||
                            registers[ins.a].kind == Symbol::Kind::INVALID ||
                            !((ip + 1 == end_ip) ||
                              (ip + 2 == end_ip &&
                               chunk.code[ip + 1].op == JitOp::RETURN_NONE))) {
                            return false;
                        }
                        returned = registers[ins.a];
                        saw_return = true;
                        ip = end_ip;
                        break;
                    default:
                        return false;
                }
            }
            return saw_return;
        };

        const size_t top_register_count = std::max<size_t>(
            function.max_regs, input_symbols.size());
        if (top_register_count == 0 || top_register_count > max_registers)
            return 0;
        std::vector<Symbol> top_registers(top_register_count);
        for (size_t argument = 0; argument < input_symbols.size(); ++argument)
            top_registers[argument] = input_symbols[argument];
        Symbol result;
        if (!analyze_range(function.entry_ip, function.end_ip,
                           std::move(top_registers), 0, result) ||
            result.kind != Symbol::Kind::RECORD ||
            result.record_class != result_class ||
            !result.freshly_constructed ||
            result.fields.size() != input_symbols.front().fields.size()) {
            return 0;
        }

        std::vector<double> values(expressions.size(), 0.0);
        for (size_t index = 0; index < expressions.size(); ++index) {
            const Expr& expr = expressions[index];
            if (expr.op == ExprOp::CONSTANT) {
                values[index] = expr.constant;
            } else if (expr.op == ExprOp::INPUT_SCALAR) {
                if (expr.argument < 0 ||
                    static_cast<size_t>(expr.argument) >=
                        input_scalar_values.size()) return 0;
                values[index] = input_scalar_values[
                    static_cast<size_t>(expr.argument)];
            } else if (expr.op == ExprOp::INPUT_FIELD) {
                if (expr.argument < 0 || expr.field < 0 ||
                    static_cast<size_t>(expr.argument) >=
                        input_record_values.size() ||
                    static_cast<size_t>(expr.field) >= input_record_values[
                        static_cast<size_t>(expr.argument)].size()) return 0;
                values[index] = input_record_values[
                    static_cast<size_t>(expr.argument)]
                    [static_cast<size_t>(expr.field)];
            }
        }
        auto evaluate = [&](int expression) {
            const Expr& expr = expressions[static_cast<size_t>(expression)];
            const double lhs = values[static_cast<size_t>(expr.lhs)];
            if (expr.op == ExprOp::NEG) {
                values[static_cast<size_t>(expression)] = -lhs;
                return;
            }
            const double rhs = values[static_cast<size_t>(expr.rhs)];
            if (expr.op == ExprOp::ADD)
                values[static_cast<size_t>(expression)] = lhs + rhs;
            else if (expr.op == ExprOp::SUB)
                values[static_cast<size_t>(expression)] = lhs - rhs;
            else
                values[static_cast<size_t>(expression)] = lhs * rhs;
        };
        for (int expression : invariant_arithmetic) evaluate(expression);

        std::vector<double> current_fields = input_record_values.front();
        double final_counter = start;
        bool iterated = false;
        for (; final_counter < limit; final_counter += increment) {
            for (const auto& input : loop_carried_inputs) {
                values[static_cast<size_t>(input.first)] =
                    current_fields[static_cast<size_t>(input.second)];
            }
            for (int expression : dynamic_arithmetic) evaluate(expression);
            for (size_t field = 0; field < current_fields.size(); ++field) {
                const int expression = result.fields[field];
                if (expression < 0 ||
                    static_cast<size_t>(expression) >= values.size()) return 0;
                current_fields[field] =
                    values[static_cast<size_t>(expression)];
            }
            iterated = true;
        }

        Value final_value = globals[static_cast<size_t>(spec.result_global)];
        if (iterated) {
            final_value = make_layout_instance(result_class);
            GCInstance* final_record = final_value.as_inst();
            if (!final_record ||
                final_record->fields.size() != current_fields.size()) return 0;
            for (size_t field = 0; field < current_fields.size(); ++field)
                final_record->fields[field] = Value(current_fields[field]);
        }
        globals[static_cast<size_t>(spec.result_global)] = final_value;
        globals[static_cast<size_t>(spec.counter_global)] = Value(final_counter);
        global_initialized[static_cast<size_t>(spec.result_global)] = true;
        global_initialized[static_cast<size_t>(spec.counter_global)] = true;
        ++generic_record_loop_runs;
        if (std::getenv("SURA_JIT_DIAG")) {
            std::cerr << "[JIT diag] generic numeric record loop: "
                      << function.name << ", " << current_fields.size()
                      << " field(s), " << expressions.size()
                      << " expression(s)\n";
        }
        return 1;
    }

    int run_strict_vector_loop(const JitChunk& chunk,
                               const JitStrictCountedLoop& spec) {
        if (spec.runtime_disabled) return 0;
        if (run_exact_vector_loop(chunk, spec) == 1) return 1;
        if (run_generic_record_loop(chunk, spec) == 1) return 1;
        spec.runtime_disabled = true;
        return 0;
    }

    // FFI helpers: preload globals before run(), read them back after.
    // Globals are indexed by the chunk's global_names order — callers must
    // supply exactly one Value per name in that vector.
    void inject_globals(const std::vector<Value>& preload) {
        globals = preload;
        global_initialized.assign(preload.size(), true);
    }
    const std::vector<Value>& snapshot_globals() const { return globals; }

    // ── JIT callbacks (invoked from native code via C helpers) ─────
    // dispatch_call_from_jit handles CALL_FUNC semantics when triggered
    // from inside a JIT-compiled function: closures (including recursion
    // into another JIT'd function), stdlib dispatch, struct/class ctors,
    // and the `exit` builtin. Returns the result as Value raw bits to be
    // stored by the caller into R[inst->a].
    uint64_t dispatch_call_from_jit(Value* R, const JitInst* ins);
    uint64_t construct_plain2_from_jit(const JitClassInfo* cls, uint64_t v0, uint64_t v1);
    uint64_t construct_plain3_from_jit(const JitClassInfo* cls, uint64_t v0, uint64_t v1, uint64_t v2);
    uint64_t dispatch_method_call_from_jit(Value* R, const JitInst* ins);
    uint64_t dispatch_dot_get_from_jit(Value* R, const JitInst* ins);
    void     dispatch_dot_set_from_jit(Value* R, const JitInst* ins);
    uint64_t make_array_from_jit(Value* R, const JitInst* ins);
    uint64_t make_dict_from_jit(Value* R, const JitInst* ins);
    // Phase 10: helpers for top-level main JIT (called from JIT-emitted code).
    uint64_t jit_make_lambda(Value* R, const JitInst* ins);
    void     jit_def_class(Value* R, const JitInst* ins);
    void     jit_print(Value* R, const JitInst* ins, int newline);
    void     jit_use_lib(const JitInst* ins);
    uint64_t jit_index_get(Value* R, const JitInst* ins);
    void     jit_index_set(Value* R, const JitInst* ins);
    void     jit_new_instance(Value* R, const JitInst* ins);
    void     jit_op_in(Value* R, const JitInst* ins);
    void     jit_dict_keys(Value* R, const JitInst* ins);
    int      jit_foreach_next(Value* R, const JitInst* ins);
    // Global load/store helpers used by LOAD_GLOBAL / STORE_GLOBAL in JIT.
    uint64_t jit_load_global(int idx) {
        if (idx < 0 || (size_t)idx >= globals.size()) return Value::nil().raw_bits();
        if ((size_t)idx < global_initialized.size() && !global_initialized[idx] && active_chunk) {
            const std::string& name = active_chunk->global_names[idx];
            if (!rt_classes.count(name) && !is_stdlib_name(name))
                throw JitThrow{format_undefined_variable(name, 0), 0};
        }
        return globals[idx].raw_bits();
    }
    uint64_t jit_load_global_inst(const JitInst* ins) {
        if (!ins) return Value::nil().raw_bits();
        int idx = ins->operand;
        if (idx < 0 || (size_t)idx >= globals.size()) return Value::nil().raw_bits();
        if ((size_t)idx < global_initialized.size() && !global_initialized[idx] && active_chunk) {
            const std::string& name = active_chunk->global_names[idx];
            if (ins->str_idx != 1 && !rt_classes.count(name) && !is_stdlib_name(name))
                throw JitThrow{format_undefined_variable(name, ins->line), ins->line};
        }
        return globals[idx].raw_bits();
    }
    void jit_store_global(int idx, uint64_t bits) {
        if (idx < 0) return;
        if ((size_t)idx >= globals.size()) globals.resize(idx + 1, Value::nil());
        if ((size_t)idx >= global_initialized.size()) global_initialized.resize(idx + 1, false);
        globals[idx] = Value::from_bits(bits);
        global_initialized[idx] = true;
    }
private:

    void run_gc() {
        const auto pause_start = std::chrono::steady_clock::now();
        const size_t before = GC::get_objects().size();
        // Reserve one allocation-free worklist for the entire root set.  Deep
        // arrays/dicts/instances/closures/tensor graphs are then traversed
        // iteratively instead of recursing on the native stack.
        GCMarkBatch mark_batch;

        for (auto& v : globals) v.mark_value();
        
        
        for (auto* uv : open_upvalues) {
            gc_mark_object(uv);
        }

        
        // Mark all live values in the single value stack
        for (size_t i = 0; i < stack_top; ++i) value_stack[i].mark_value();

        for (GCObject* object : gc_native_roots_storage()) {
            gc_mark_object(object);
        }

        // A recursive call stack usually points every frame at the same chunk.
        // Mark each chunk's constants/defaults once per collection instead of
        // rescanning them once per frame, which bounds a former O(frames *
        // constants) part of the stop-the-world pause.
        std::unordered_set<const JitChunk*> rooted_chunks;
        auto mark_chunk_roots = [&](const JitChunk* chunk) {
            if (!chunk || !rooted_chunks.insert(chunk).second) return;
            for (auto v : chunk->constants) { Value temp = v; temp.mark_value(); }
            for (auto& ci : chunk->class_table) {
                for (auto v : ci.field_defaults) { Value temp = v; temp.mark_value(); }
                for (auto& [mname, mi] : ci.methods) {
                    (void)mname;
                    for (auto v : mi.defaults) { Value temp = v; temp.mark_value(); }
                }
            }
            for (auto& fi : chunk->func_table) {
                for (auto v : fi.defaults) { Value temp = v; temp.mark_value(); }
            }
        };

        // Top-level native execution has no interpreter CallFrame, but its
        // object constants and default values are still live roots.
        mark_chunk_roots(active_chunk);

        for (auto& cf : call_stack) {
            mark_chunk_roots(cf.chunk);
        }
        
        for (auto& [cname, c_info] : rt_classes) {
            for (auto& v : c_info.field_defaults) v.mark_value();
        }

        mark_batch.finish();
        GC::sweep();
        size_t after = GC::get_objects().size();
        gc_threshold = std::max((size_t)1024, after * 2);
        tensor_bytes_after_gc = tensor_external_bytes_storage().load(
            std::memory_order_relaxed);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pause_start).count();
        const uint64_t pause_us = elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
        ++gc_stats.collections;
        if (before > after) gc_stats.objects_reclaimed += before - after;
        gc_stats.last_objects_before = before;
        gc_stats.last_objects_after = after;
        gc_stats.peak_objects = std::max(gc_stats.peak_objects, std::max(before, after));
        gc_stats.total_pause_us += pause_us;
        gc_stats.max_pause_us = std::max(gc_stats.max_pause_us, pause_us);
        gc_stats.next_object_threshold = gc_threshold;
        gc_stats.last_tensor_bytes = tensor_bytes_after_gc;
    }

    bool tensor_gc_pressure() const {
        size_t current = tensor_external_bytes_storage().load(std::memory_order_relaxed);
        if (current <= tensor_bytes_after_gc) return false;
        size_t limit = SuraStd::ag_max_external_bytes();
        size_t headroom = tensor_bytes_after_gc < limit
                        ? limit - tensor_bytes_after_gc : 0;
        size_t minimum = std::min<size_t>(8ULL * 1024ULL * 1024ULL,
            std::max<size_t>(64ULL * 1024ULL, limit / 16));
        size_t trigger = std::max(minimum, headroom / 4);
        return current - tensor_bytes_after_gc >= trigger;
    }

    void run_gc_for_tensor_pressure() {
        if (tensor_gc_pressure()) run_gc();
    }

    static void tensor_gc_callback(void* context) {
        if (context) static_cast<JitVM*>(context)->run_gc();
    }

    void run_gc_for_native_allocation() {
        // MAKE_ARRAY/MAKE_DICT are not tensor-GC safepoints in the bytecode
        // interpreter. Keep tensor pressure checks at native call boundaries
        // too; otherwise merely constructing a collection can collect between
        // two tensor accounting observations and make VM/JIT behavior diverge.
        if (++native_allocation_gc_tick > 10000) {
            native_allocation_gc_tick = 0;
            if (GC::get_objects().size() > gc_threshold) run_gc();
        }
    }

    static std::string op_to_str(JitOp op) {
        switch(op) {
            case JitOp::LOAD_CONST: return "LOAD_CONST";
            case JitOp::LOAD_NIL: return "LOAD_NIL";
            case JitOp::LOAD_BOOL: return "LOAD_BOOL";
            case JitOp::MOVE: return "MOVE";
            case JitOp::LOAD_GLOBAL: return "LOAD_GLOBAL";
            case JitOp::STORE_GLOBAL: return "STORE_GLOBAL";
            case JitOp::LOAD_UPVAL: return "LOAD_UPVAL";
            case JitOp::STORE_UPVAL: return "STORE_UPVAL";
            case JitOp::ADD: return "ADD";
            case JitOp::SUB: return "SUB";
            case JitOp::MUL: return "MUL";
            case JitOp::DIV: return "DIV";
            case JitOp::MOD: return "MOD";
            case JitOp::BIT_AND: return "BIT_AND";
            case JitOp::BIT_OR: return "BIT_OR";
            case JitOp::BIT_XOR: return "BIT_XOR";
            case JitOp::LSHIFT: return "LSHIFT";
            case JitOp::RSHIFT: return "RSHIFT";
            case JitOp::CMP_EQ: return "CMP_EQ";
            case JitOp::CMP_NEQ: return "CMP_NEQ";
            case JitOp::CMP_LT: return "CMP_LT";
            case JitOp::CMP_LTE: return "CMP_LTE";
            case JitOp::CMP_GT: return "CMP_GT";
            case JitOp::CMP_GTE: return "CMP_GTE";
            case JitOp::NEG: return "NEG";
            case JitOp::BIT_NOT: return "BIT_NOT";
            case JitOp::LOGICAL_NOT: return "LOGICAL_NOT";
            case JitOp::JUMP: return "JUMP";
            case JitOp::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
            case JitOp::JUMP_IF_TRUE: return "JUMP_IF_TRUE";
            case JitOp::CALL_FUNC: return "CALL_FUNC";
            case JitOp::CALL_BUILTIN: return "CALL_BUILTIN";
            case JitOp::METHOD_CALL: return "METHOD_CALL";
            case JitOp::SUPER_CALL: return "SUPER_CALL";
            case JitOp::RETURN_VAL: return "RETURN_VAL";
            case JitOp::RETURN_NONE: return "RETURN_NONE";
            case JitOp::MAKE_ARRAY: return "MAKE_ARRAY";
            case JitOp::MAKE_DICT: return "MAKE_DICT";
            case JitOp::INDEX_GET: return "INDEX_GET";
            case JitOp::INDEX_SET: return "INDEX_SET";
            case JitOp::DOT_GET: return "DOT_GET";
            case JitOp::DOT_SET: return "DOT_SET";
            case JitOp::OP_IN: return "OP_IN";
            case JitOp::NEW_INSTANCE: return "NEW_INSTANCE";
            case JitOp::DEF_FUNC: return "DEF_FUNC";
            case JitOp::MAKE_LAMBDA: return "MAKE_LAMBDA";
            case JitOp::DEF_CLASS: return "DEF_CLASS";
            case JitOp::TRY_BEGIN: return "TRY_BEGIN";
            case JitOp::TRY_END: return "TRY_END";
            case JitOp::OP_THROW: return "OP_THROW";
            case JitOp::FOREACH_NEXT: return "FOREACH_NEXT";
            case JitOp::DICT_KEYS: return "DICT_KEYS";
            case JitOp::PRINT: return "PRINT";
            case JitOp::PRINT_NO_NL: return "PRINT_NO_NL";
            case JitOp::USE_LIB: return "USE_LIB";
            case JitOp::HALT: return "HALT";
            case JitOp::NOP: return "NOP";
            default: return "OP_" + std::to_string((int)op);
        }
    }

    // ── Helper: alloc N registers on the value stack ──────────────────
    size_t alloc_frame_regs(size_t count, int line) {
        size_t base = stack_top;
        if (base > STACK_CAPACITY || count > STACK_CAPACITY - base)
            throw JitThrow{"[E500] 스택 오버플로우", line};
        // Zero-valued registers are safe GC roots and preserve the VM's
        // historical uninitialized-register behavior without writing through
        // a non-trivial C++ object using memset.
        std::fill_n(value_stack.begin() + static_cast<std::ptrdiff_t>(base),
                    count, Value(0.0));
        stack_top = base + count;
        return base;
    }

    size_t function_frame_regs(const JitChunk& chunk, const JitFuncInfo& function) const {
        size_t count = function.max_regs > 0 ? function.max_regs : 32;
        count = std::max(count, function.params.size());
        const int argc_reg = jit_default_arg_count_reg(
            chunk, function.entry_ip, 0, function.params.size());
        if (argc_reg >= 0) count = std::max(count, static_cast<size_t>(argc_reg) + 1);
        return count;
    }

    size_t method_frame_regs(const JitChunk& chunk, const JitMethodInfo& method) const {
        size_t count = method.max_regs > 0 ? method.max_regs : 32;
        count = std::max(count, method.params.size() + 1);
        const int argc_reg = jit_default_arg_count_reg(
            chunk, method.entry_ip, 1, method.params.size());
        if (argc_reg >= 0) count = std::max(count, static_cast<size_t>(argc_reg) + 1);
        return count;
    }

    void bind_function_args(const JitChunk& chunk,
                            const JitFuncInfo& function,
                            Value* frame_regs,
                            const Value* args,
                            size_t arg_count) const {
        for (size_t i = 0; i < function.params.size(); ++i) {
            frame_regs[i] = i < arg_count
                ? args[i]
                : (i < function.defaults.size() ? function.defaults[i] : Value::nil());
        }
        const int argc_reg = jit_default_arg_count_reg(
            chunk, function.entry_ip, 0, function.params.size());
        if (argc_reg >= 0) frame_regs[argc_reg] = Value(static_cast<double>(arg_count));
    }

    void bind_method_args(const JitChunk& chunk,
                          const JitMethodInfo& method,
                          Value* frame_regs,
                          const Value& self,
                          const Value* args,
                          size_t arg_count) const {
        frame_regs[0] = self;
        for (size_t i = 0; i < method.params.size(); ++i) {
            frame_regs[i + 1] = i < arg_count
                ? args[i]
                : (i < method.defaults.size() ? method.defaults[i] : Value::nil());
        }
        const int argc_reg = jit_default_arg_count_reg(
            chunk, method.entry_ip, 1, method.params.size());
        if (argc_reg >= 0) frame_regs[argc_reg] = Value(static_cast<double>(arg_count));
    }

    CallFrame& push_frame(int line) {
        if (frame_top >= FRAME_CAPACITY)
            throw JitThrow{"[E500] 스택 오버플로우", line};
        return frame_pool[frame_top++];
    }
    // Fast version: skip fill (caller must write all used registers before GC)
    size_t alloc_frame_regs_fast(size_t count) {
        size_t base = stack_top;
        stack_top = base + count;
        return base;
    }

public:
    static void dump(const JitChunk& chunk) {
        std::cout << "--- bytecode dump ---\n";
        for (size_t i=0; i<chunk.code.size(); ++i) {
            auto& c = chunk.code[i];
            std::cout << "[" << i << "] " << op_to_str(c.op) << " R" << (int)c.a << " R" << (int)c.b << " R" << (int)c.c;
            if (c.operand != 0) std::cout << " op=" << c.operand;
            if (c.str_idx != -1) std::cout << " str='" << chunk.get_string(c.str_idx) << "'";
            if (c.ic_cache != -1) std::cout << " ic=" << c.ic_cache;
            std::cout << "\n";
        }
    }

    Value run(const JitChunk& main_chunk) {
        SuraStd::AgGcCallbackScope tensor_gc_scope(&JitVM::tensor_gc_callback, this);
        const size_t entry_frame_top = frame_top;
        const JitChunk* const previous_active_chunk = active_chunk;
        size_t count = main_chunk.max_regs > 0 ? main_chunk.max_regs : 256;
        size_t base  = alloc_frame_regs(count, 0);

        // HALT and an uncaught exception can leave execute_frame() without its
        // ordinary RETURN cleanup. Close every upvalue owned by this run while
        // value_stack is still alive, then restore the caller's VM state. A
        // runtime exception from native code propagates through this guard;
        // only native compilation failure falls through to the interpreter.
        auto restore_entry_state = [&]() noexcept {
            if (!open_upvalues.empty()) close_upvalues(&value_stack[base]);
            frame_top = entry_frame_top;
            stack_top = base;
            active_chunk = previous_active_chunk;
        };
        struct RunStateGuard {
            decltype(restore_entry_state)& restore;
            ~RunStateGuard() { restore(); }
        } run_state_guard{restore_entry_state};

        if (globals.size() < main_chunk.global_names.size())
            globals.resize(main_chunk.global_names.size(), Value::nil());
        if (global_initialized.size() < main_chunk.global_names.size())
            global_initialized.resize(main_chunk.global_names.size(), false);
        for (size_t i = 0; i < main_chunk.global_names.size(); ++i) {
            if (main_chunk.global_names[i] == "console" && !global_initialized[i]) {
                globals[i] = make_stdlib_module("console");
                global_initialized[i] = true;
            }
        }

        // ── Phase 10: try to JIT-compile the entire main chunk as one fn ──
        // If compilation succeeds, run native; else fall through to interpreter.
        // Compile-time cost is amortized over the main loop's many iterations.
        if (native_allowed()) {
            JitFuncInfo main_fi;
            main_fi.name     = "__main__";
            main_fi.entry_ip = 0;
            main_fi.end_ip   = main_chunk.code.size();
            main_fi.max_regs = (uint16_t)count;
            std::unique_ptr<NativeFunc> compiled;
            try {
                NativeCompiler nc(main_chunk, main_fi, /*top_level=*/true);
                compiled = nc.compile();
                if (!compiled && nc.did_bail_on_opcode()) {
                    main_bailed  = true;
                    main_bail_op = nc.bailed_opcode();
                    main_bail_ip = nc.bailed_ip();
                }
            } catch (...) {
                compiled.reset();
                /* fall back to interpreter */
            }
            if (compiled) {
                main_emitted_ops |= compiled->emitted_ops;
                Value* NR = &value_stack[base];
                active_chunk = &main_chunk;
                // Runtime exceptions are not compilation failures. Let the
                // run-state guard restore frames/roots while the exception
                // propagates; replaying main would duplicate prior effects.
                uint64_t bits = compiled->fn(this, NR, main_chunk.constants.data());
                active_chunk = previous_active_chunk;
                stack_top = base;
                return Value::from_bits(bits);
            }
        }

        CallFrame frame;
        frame.reg_base  = base;
        frame.reg_count = count;
        frame.chunk     = &main_chunk;
        frame.ip        = 0;
        frame.end_ip    = main_chunk.code.size();

        active_chunk = &main_chunk;
        try {
            Value r = execute_frame(frame);
            active_chunk = nullptr;
            return r;
        } catch (JitThrow& t) {
            active_chunk = nullptr;
            // Populate stack trace from frame_pool before rethrowing.
            if (t.stack_trace.empty()) {
                for (size_t i = frame_top; i-- > 0; ) {
                    const CallFrame& cf = frame_pool[i];
                    StackFrameInfo sf;
                    sf.func_name = cf.closure ? cf.closure->name
                                 : cf.method ? cf.method->name
                                 : std::string("<main>");
                    if (cf.chunk && !cf.chunk->code.empty()) {
                        size_t ip = cf.ip > 0 ? cf.ip - 1 : 0;
                        if (ip < cf.chunk->code.size())
                            sf.line = cf.chunk->code[ip].line;
                    }
                    t.stack_trace.push_back(sf);
                }
            }
            throw;
        } catch (std::exception& e) {
            std::cerr << "C++ Exception: " << e.what() << "\n";
        }
        return Value::nil();
    }

private:
    GCUpvalue* capture_upvalue(Value* local) {
        for (auto* uv : open_upvalues) {
            if (uv->location == local) return uv;
        }
        auto* uv = GC::allocate<GCUpvalue>(local);
        open_upvalues.push_back(uv);
        return uv;
    }

    void close_upvalues(Value* last_local) {
        for (auto it = open_upvalues.begin(); it != open_upvalues.end(); ) {
            GCUpvalue* uv = *it;
            if (uv->location >= last_local) {
                uv->closed = *(uv->location); 
                uv->location = nullptr;
                it = open_upvalues.erase(it);
            } else {
                ++it;
            }
        }
    }
    const JitMethodInfo* find_method(const std::string& cls, const std::string& method) {
        std::string cur = cls;
        while (!cur.empty() && rt_classes.count(cur)) {
            auto& c = rt_classes[cur];
            if (c.methods.count(method)) return &c.methods[method];
            if (method == "init" && c.methods.count("생성자")) return &c.methods["생성자"];
            if (method == "생성자" && c.methods.count("init")) return &c.methods["init"];
            cur = c.parent;
        }
        return nullptr;
    }

    bool class_has_instance_field_initializers(const JitClassInfo* cls) const {
        if (!cls) return false;
        const JitClassInfo* current = cls;
        std::unordered_set<std::string> seen;
        while (current && seen.insert(current->name).second) {
            if (current->methods.count(JIT_FIELD_INITIALIZER_METHOD)) return true;
            if (current->parent.empty()) break;
            auto parent = rt_classes.find(current->parent);
            current = parent == rt_classes.end() ? nullptr : &parent->second;
        }
        return false;
    }

    const std::vector<int>* plain_ctor_fields(const JitClassInfo* cls, const JitMethodInfo* ctor) {
        auto cached = plain_ctor_field_cache.find(ctor);
        if (cached != plain_ctor_field_cache.end()) return &cached->second;
        if (non_plain_ctor_cache.count(ctor)) return nullptr;
        // A class with executable instance-field initializers must pass through
        // the common allocation path before its user constructor. The plain
        // record shortcuts are reserved for auto structs without that method.
        if (class_has_instance_field_initializers(cls)) {
            if (ctor) non_plain_ctor_cache.insert(ctor);
            return nullptr;
        }
        if (!active_chunk || !cls || !ctor ||
            ctor->entry_ip > ctor->end_ip || ctor->end_ip > active_chunk->code.size() ||
            ctor->defaults.size() < ctor->params.size()) {
            if (ctor) non_plain_ctor_cache.insert(ctor);
            return nullptr;
        }

        std::vector<int> field_by_param(ctor->params.size(), -1);
        size_t ip = ctor->entry_ip;
        bool ok = true;
        while (ip < ctor->end_ip) {
            const JitInst& first = active_chunk->code[ip];
            if (first.op == JitOp::RETURN_NONE && ip + 1 == ctor->end_ip) {
                ++ip;
                break;
            }
            if (ip + 2 >= ctor->end_ip) { ok = false; break; }

            const JitInst& self_move = active_chunk->code[ip];
            const JitInst& value_move = active_chunk->code[ip + 1];
            const JitInst& field_set = active_chunk->code[ip + 2];
            if (self_move.op != JitOp::MOVE || self_move.b != 0 ||
                value_move.op != JitOp::MOVE ||
                field_set.op != JitOp::DOT_SET ||
                field_set.a != self_move.a || field_set.b != value_move.a ||
                field_set.str_idx < 0) {
                ok = false;
                break;
            }

            int param_reg = (int)value_move.b;
            if (param_reg <= 0 || (size_t)param_reg > ctor->params.size()) {
                ok = false;
                break;
            }
            size_t param_index = (size_t)(param_reg - 1);
            const std::string& field_name = active_chunk->get_string(field_set.str_idx);
            auto fit = cls->field_indices.find(field_name);
            if (fit == cls->field_indices.end() ||
                field_name != ctor->params[param_index] ||
                field_by_param[param_index] >= 0) {
                ok = false;
                break;
            }
            field_by_param[param_index] = fit->second;
            ip += 3;
        }

        if (ip != ctor->end_ip) ok = false;
        for (int field_index : field_by_param) {
            if (field_index < 0 || (size_t)field_index >= cls->field_defaults.size()) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            non_plain_ctor_cache.insert(ctor);
            return nullptr;
        }

        auto inserted = plain_ctor_field_cache.emplace(ctor, std::move(field_by_param)).first;
        return &inserted->second;
    }

    void apply_plain_ctor_fields(GCInstance* idata, const JitMethodInfo* ctor,
                                 const std::vector<int>& field_by_param,
                                 Value* R, const JitInst* ins) {
        size_t arg_count = ins->operand > 0 ? (size_t)ins->operand : 0;
        for (size_t i = 0; i < field_by_param.size(); ++i) {
            const int field_index = field_by_param[i];
            if (field_index < 0 || (size_t)field_index >= idata->fields.size()) continue;
            idata->fields[(size_t)field_index] =
                (i < arg_count) ? R[ins->c + i] : ctor->defaults[i];
        }
    }

    void run_instance_field_initializers(const JitClassInfo* cls,
                                         const Value& instance,
                                         int line) {
        if (!active_chunk || !cls) return;
        std::vector<const JitClassInfo*> chain;
        std::unordered_set<std::string> seen;
        const JitClassInfo* current = cls;
        while (current && seen.insert(current->name).second) {
            chain.push_back(current);
            if (current->parent.empty()) break;
            auto parent = rt_classes.find(current->parent);
            current = parent == rt_classes.end() ? nullptr : &parent->second;
        }
        std::reverse(chain.begin(), chain.end());

        for (const JitClassInfo* owner : chain) {
            auto found = owner->methods.find(JIT_FIELD_INITIALIZER_METHOD);
            if (found == owner->methods.end()) continue;
            const JitMethodInfo& initializer = found->second;
            const size_t count = method_frame_regs(*active_chunk, initializer);
            const size_t base = alloc_frame_regs(count, line);
            Value* regs = &value_stack[base];
            bind_method_args(*active_chunk, initializer, regs, instance, nullptr, 0);
            CallFrame frame;
            frame.reg_base = base;
            frame.reg_count = count;
            frame.closure = nullptr;
            frame.method = &initializer;
            frame.chunk = active_chunk;
            frame.ip = initializer.entry_ip;
            frame.end_ip = initializer.end_ip;
            frame.ret_reg = (uint16_t)-1;
            frame.in_try = false;
            execute_frame(frame);
        }
    }

    Value make_layout_instance(const JitClassInfo* cls) {
        if (!cls) return Value::nil();
        Value instance = Value::make_inst_ref(&cls->name);
        GCInstance* data = instance.as_inst();
        data->fields = cls->field_defaults;
        data->jit_info = const_cast<JitClassInfo*>(cls);
        return instance;
    }

    Value make_initialized_instance(const JitClassInfo* cls, int line) {
        Value instance = make_layout_instance(cls);
        if (!instance.is_inst()) return instance;
        if (class_has_instance_field_initializers(cls))
            run_instance_field_initializers(cls, instance, line);
        return instance;
    }

    Value make_plain_ctor_instance(const JitClassInfo* cls, const JitMethodInfo* ctor,
                                   const std::vector<int>& field_by_param,
                                   Value* R, const JitInst* ins) {
        // plain_ctor_fields() rejects every class whose inheritance chain has
        // executable field initializers, so the hot struct path can allocate
        // the field layout without a parent-chain walk or nested VM frame.
        Value obj = make_layout_instance(cls);
        GCInstance* idata = obj.as_inst();
        apply_plain_ctor_fields(idata, ctor, field_by_param, R, ins);
        return obj;
    }

    Value call_callable(const Value& fn, const std::vector<Value>& args, int line) {
        if (!fn.is_closure()) return Value::nil();
        if (!active_chunk) throw JitThrow{"[E101] 정의되지 않은 함수", line};
        GCClosure* cl = fn.as_closure();
        if (cl->func_idx < 0 || (size_t)cl->func_idx >= active_chunk->func_table.size())
            throw JitThrow{"[E101] 정의되지 않은 함수", line};
        const JitFuncInfo& fi = active_chunk->func_table[cl->func_idx];
        if (args.size() > fi.params.size())
            throw JitThrow{"[E300] 잘못된 인자 개수", line};
        size_t cnt = function_frame_regs(*active_chunk, fi);
        size_t base = alloc_frame_regs(cnt, line);
        Value* NR = &value_stack[base];
        bind_function_args(*active_chunk, fi, NR, args.data(), args.size());

        CallFrame cf;
        cf.reg_base = base;
        cf.reg_count = cnt;
        cf.closure = cl;
        cf.chunk = active_chunk;
        cf.ip = fi.entry_ip;
        cf.end_ip = fi.end_ip;
        cf.ret_reg = (uint16_t)-1;
        cf.in_try = false;
        return execute_frame(cf);
    }

    Value call_callable_from_args(const Value& fn, Value* args, int nargs, int line) {
        std::vector<Value> argv;
        argv.reserve((size_t)std::max(0, nargs));
        for (int i = 0; i < nargs; ++i) argv.push_back(args[i]);
        return call_callable(fn, argv, line);
    }

    bool dispatch_dict_callable_field(const Value& receiver, const std::string& meth,
                                      Value* args, int nargs, int line, Value& out) {
        if (!receiver.is_dict()) return false;
        GCDict* dict = receiver.as_dict();
        auto it = dict->elements.find(meth);
        if (it == dict->elements.end() || !it->second.is_closure()) return false;
        out = call_callable_from_args(it->second, args, nargs, line);
        return true;
    }

    bool dispatch_instance_callable_field(const Value& receiver, const std::string& meth,
                                          Value* args, int nargs, int line, Value& out) {
        if (!receiver.is_inst()) return false;
        GCInstance* obj = receiver.as_inst();
        const std::string& class_name = obj->type_name();
        auto class_it = rt_classes.find(class_name);
        if (class_it == rt_classes.end()) return false;
        auto field_it = class_it->second.field_indices.find(meth);
        if (field_it == class_it->second.field_indices.end()) return false;
        int field_index = field_it->second;
        if (field_index < 0 || (size_t)field_index >= obj->fields.size()) return false;
        const Value& fn = obj->fields[(size_t)field_index];
        if (!fn.is_closure()) return false;
        out = call_callable_from_args(fn, args, nargs, line);
        return true;
    }

    bool dispatch_builtin_method(const Value& receiver, const std::string& meth,
                                 Value* args, int nargs, int line, Value& out) {
        if (receiver.is_arr()) {
            GCArray* arr = receiver.as_arr();
            if (meth == "len" || meth == "size" || meth == "length") {
                out = Value((double)arr->elements.size()); return true;
            }
            if (meth == "push" || meth == "append") {
                if (nargs >= 1) arr->elements.push_back(args[0]);
                out = receiver; return true;
            }
            if (meth == "pop") {
                if (arr->elements.empty()) out = Value::nil();
                else { out = arr->elements.back(); arr->elements.pop_back(); }
                return true;
            }
            if (meth == "shift") {
                if (arr->elements.empty()) out = Value::nil();
                else { out = arr->elements.front(); arr->elements.erase(arr->elements.begin()); }
                return true;
            }
            if (meth == "unshift") {
                if (nargs >= 1) arr->elements.insert(arr->elements.begin(), args[0]);
                out = receiver; return true;
            }
            if (meth == "includes" || meth == "contains" || meth == "has") {
                bool found = false;
                if (nargs >= 1) for (auto& el : arr->elements) if (el.eq(args[0])) { found = true; break; }
                out = Value(found); return true;
            }
            if (meth == "index_of" || meth == "indexOf") {
                int idx = -1;
                if (nargs >= 1) for (size_t i = 0; i < arr->elements.size(); ++i)
                    if (arr->elements[i].eq(args[0])) { idx = (int)i; break; }
                out = Value((double)idx); return true;
            }
            if (meth == "map") {
                Value nv = Value::make_array();
                if (nargs >= 1) for (auto& el : arr->elements)
                    nv.as_arr()->elements.push_back(call_callable(args[0], {el}, line));
                out = nv; return true;
            }
            if (meth == "filter") {
                Value nv = Value::make_array();
                if (nargs >= 1) for (auto& el : arr->elements)
                    if (call_callable(args[0], {el}, line).truthy()) nv.as_arr()->elements.push_back(el);
                out = nv; return true;
            }
            if (meth == "reduce") {
                Value acc = (nargs >= 2) ? args[1] : (arr->elements.empty() ? Value::nil() : arr->elements[0]);
                size_t start = (nargs >= 2) ? 0 : 1;
                if (nargs >= 1) for (size_t i = start; i < arr->elements.size(); ++i)
                    acc = call_callable(args[0], {acc, arr->elements[i]}, line);
                out = acc; return true;
            }
            if (meth == "each") {
                if (nargs >= 1) for (auto& el : arr->elements) call_callable(args[0], {el}, line);
                out = receiver; return true;
            }
            if (meth == "find") {
                out = Value::nil();
                if (nargs >= 1) for (auto& el : arr->elements)
                    if (call_callable(args[0], {el}, line).truthy()) { out = el; break; }
                return true;
            }
            if (meth == "reverse") {
                std::reverse(arr->elements.begin(), arr->elements.end());
                out = receiver; return true;
            }
            if (meth == "sort") {
                if (nargs >= 1 && args[0].is_closure()) {
                    Value cmp = args[0];
                    std::sort(arr->elements.begin(), arr->elements.end(),
                        [&](const Value& x, const Value& y) {
                            return call_callable(cmp, {x, y}, line).truthy();
                        });
                } else {
                    std::sort(arr->elements.begin(), arr->elements.end(),
                        [](const Value& x, const Value& y) { return x.lt(y); });
                }
                out = receiver; return true;
            }
            if (meth == "join") {
                std::string sep = (nargs >= 1) ? args[0].to_str() : "";
                std::string s;
                for (size_t i = 0; i < arr->elements.size(); ++i) {
                    if (i > 0) s += sep;
                    s += arr->elements[i].to_str();
                }
                out = Value(s); return true;
            }
            if (meth == "slice") {
                int from = nargs >= 1 ? (int)args[0].to_num() : 0;
                int to = nargs >= 2 ? (int)args[1].to_num() : (int)arr->elements.size();
                if (from < 0) from += (int)arr->elements.size();
                if (to < 0) to += (int)arr->elements.size();
                from = std::max(0, std::min(from, (int)arr->elements.size()));
                to = std::max(from, std::min(to, (int)arr->elements.size()));
                Value nv = Value::make_array();
                nv.as_arr()->elements.insert(nv.as_arr()->elements.end(),
                    arr->elements.begin() + from, arr->elements.begin() + to);
                out = nv; return true;
            }
            if (meth == "concat") {
                Value nv = Value::make_array();
                nv.as_arr()->elements.insert(nv.as_arr()->elements.end(), arr->elements.begin(), arr->elements.end());
                if (nargs >= 1 && args[0].is_arr()) {
                    auto* other = args[0].as_arr();
                    nv.as_arr()->elements.insert(nv.as_arr()->elements.end(), other->elements.begin(), other->elements.end());
                }
                out = nv; return true;
            }
            if (meth == "flat") {
                Value nv = Value::make_array();
                std::function<void(const Value&)> add_flat = [&](const Value& v) {
                    if (v.is_arr()) for (auto& inner : v.as_arr()->elements) add_flat(inner);
                    else nv.as_arr()->elements.push_back(v);
                };
                for (auto& el : arr->elements) add_flat(el);
                out = nv; return true;
            }
        }

        if (receiver.is_str()) {
            const std::string& s = receiver.as_str_ref();
            if (meth == "len" || meth == "size" || meth == "length") {
                out = Value((double)s.size()); return true;
            }
            if (meth == "upper") {
                std::string v = s; for (char& ch : v) ch = (char)std::toupper((unsigned char)ch);
                out = Value(v); return true;
            }
            if (meth == "lower") {
                std::string v = s; for (char& ch : v) ch = (char)std::tolower((unsigned char)ch);
                out = Value(v); return true;
            }
            if (meth == "trim") {
                size_t st = s.find_first_not_of(" \t\n\r");
                size_t en = s.find_last_not_of(" \t\n\r");
                out = (st == std::string::npos) ? Value(std::string("")) : Value(s.substr(st, en - st + 1));
                return true;
            }
            if (meth == "split") {
                std::string sep = nargs >= 1 ? args[0].to_str() : " ";
                Value arrv = Value::make_array();
                if (sep.empty()) for (char ch : s) arrv.as_arr()->elements.push_back(Value(std::string(1, ch)));
                else {
                    size_t p = 0;
                    while (true) {
                        size_t f = s.find(sep, p);
                        if (f == std::string::npos) { arrv.as_arr()->elements.push_back(Value(s.substr(p))); break; }
                        arrv.as_arr()->elements.push_back(Value(s.substr(p, f - p)));
                        p = f + sep.size();
                    }
                }
                out = arrv; return true;
            }
            if (meth == "replace") {
                if (nargs < 2) { out = receiver; return true; }
                std::string from = args[0].to_str();
                std::string to = args[1].to_str();
                if (from.empty()) { out = receiver; return true; }
                std::string res; size_t p = 0;
                while (true) {
                    size_t f = s.find(from, p);
                    if (f == std::string::npos) { res += s.substr(p); break; }
                    res += s.substr(p, f - p) + to;
                    p = f + from.size();
                }
                out = Value(res); return true;
            }
            if (meth == "contains" || meth == "has") {
                std::string needle = nargs >= 1 ? args[0].to_str() : "";
                out = Value(s.find(needle) != std::string::npos); return true;
            }
            if (meth == "startsWith" || meth == "starts_with" || meth == "startswith") {
                std::string pre = nargs >= 1 ? args[0].to_str() : "";
                out = Value(s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0); return true;
            }
            if (meth == "endsWith" || meth == "ends_with" || meth == "endswith") {
                std::string suf = nargs >= 1 ? args[0].to_str() : "";
                out = Value(s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0); return true;
            }
            if (meth == "indexOf" || meth == "index_of" || meth == "index" || meth == "find") {
                std::string needle = nargs >= 1 ? args[0].to_str() : "";
                size_t p = s.find(needle);
                out = (p == std::string::npos) ? Value(-1.0) : Value((double)p); return true;
            }
            if (meth == "sub" || meth == "substr" || meth == "substring" || meth == "slice") {
                int start = nargs >= 1 ? (int)args[0].to_num() : 0;
                int end = nargs >= 2 ? (int)args[1].to_num() : (int)s.size();
                if (meth == "slice" && start < 0) start = std::max(0, (int)s.size() + start);
                if (meth == "slice" && end < 0) end = std::max(0, (int)s.size() + end);
                if (start < 0) start = 0;
                if (end > (int)s.size()) end = (int)s.size();
                if (end < start) end = start;
                out = Value(s.substr(start, end - start)); return true;
            }
            if (meth == "to_num" || meth == "tonumber") {
                try { out = Value(std::stod(s)); } catch (...) { out = Value::nil(); }
                return true;
            }
            if (meth == "repeat") {
                int n = nargs >= 1 ? (int)args[0].to_num() : 0;
                std::string res;
                for (int i = 0; i < n; ++i) res += s;
                out = Value(res); return true;
            }
        }

        if (receiver.is_dict()) {
            GCDict* dict = receiver.as_dict();
            auto module_it = dict->elements.find("__module");
            if (module_it != dict->elements.end() && module_it->second.is_str()) {
                std::string builtin = module_builtin_name(module_it->second.as_str_ref(), meth);
                Value outv;
                if (!builtin.empty() && SuraStd::try_dispatch(builtin, args, nargs, line, outv)) {
                    out = outv;
                    return true;
                }
            }
            if (meth == "len" || meth == "size" || meth == "length") {
                out = Value((double)dict->elements.size()); return true;
            }
            if (meth == "keys") {
                Value arrv = Value::make_array();
                for (auto& [k, v] : dict->elements) arrv.as_arr()->elements.push_back(Value(k));
                out = arrv; return true;
            }
            if (meth == "values") {
                Value arrv = Value::make_array();
                for (auto& [k, v] : dict->elements) arrv.as_arr()->elements.push_back(v);
                out = arrv; return true;
            }
            if (meth == "has" || meth == "contains") {
                std::string key = nargs >= 1 ? args[0].to_str() : "";
                out = Value(dict->elements.count(key) > 0); return true;
            }
            if (meth == "delete" || meth == "remove") {
                bool removed = false;
                if (nargs >= 1) removed = dict->elements.erase(args[0].to_str()) > 0;
                out = Value(removed); return true;
            }
            if (dispatch_dict_callable_field(receiver, meth, args, nargs, line, out)) {
                return true;
            }
        }
        return false;
    }

    // Cold-path instrumentation. Kept out of line so each _NEXT() site is a
    // single predicted-not-taken test rather than an inlined trace/hook block.
#ifdef __GNUC__
    __attribute__((noinline))
#endif
    void vm_instrument_step(const JitChunk& chunk, const JitInst& ins, size_t ip,
                            const CallFrame& fp, Value* R) {
        if (trace_enabled) {
            std::cerr << "[trace] ip=" << ip
                      << " line=" << ins.line
                      << " op=" << op_to_str(ins.op)
                      << " a=" << (int)ins.a << " b=" << (int)ins.b << " c=" << (int)ins.c
                      << " operand=" << ins.operand << "\n";
        }
        if (debug_hook) debug_before_instruction(chunk, ins, ip, fp, R);
    }

    // ── Iterative VM entry point ──────────────────────────────────────
    // Pushes initial_frame onto call_stack and dispatches iteratively.
    // CALL_FUNC/METHOD_CALL push new frames; RETURN_VAL/NONE pop them.
    // No C++ recursion for Sura function calls.
    Value execute_frame(CallFrame& initial_frame) {
        size_t entry_depth = frame_top;
        initial_frame.ret_reg = (uint16_t)-1;
        int entry_line = 0;
        if (initial_frame.chunk && initial_frame.ip < initial_frame.chunk->code.size())
            entry_line = initial_frame.chunk->code[initial_frame.ip].line;
        push_frame(entry_line) = initial_frame;

        // Working pointers — updated on every frame switch
        CallFrame* fp = &frame_pool[frame_top-1];
        Value* R      = &value_stack[fp->reg_base];
        const auto& chunk = *fp->chunk;        // all frames share one chunk
        const JitInst* code = chunk.code.data();
        int gc_tick = 0;
        // Attached before execution via set_profiler(); hoisted so the hot
        // arithmetic and branch opcodes test a register, not a member load.
        Profiler* const _prof = prof;

        // ── Local ip cache: avoids heap pointer deref on every instruction ──
        size_t lip  = fp->ip;
        size_t lend = fp->end_ip;
        // Sync working pointers from fp (called after every frame switch)
        #define _LOAD_FRAME() do { lip=fp->ip; lend=fp->end_ip; R=&value_stack[fp->reg_base]; } while(0)
        // Save lip back into fp (called before frame push so parent ip is preserved)
        #define _SAVE_IP() do { fp->ip = lip; } while(0)

        // ── Computed-goto dispatch (GCC/Clang) ────────────────────────
#ifdef __GNUC__
        const JitInst* cur = nullptr;
        uint16_t a = 0, b = 0, c = 0;
        #define inst (*cur)
        // Trace and debug hooks are configured before execution (enable_trace /
        // set_debug_hook) and never toggle mid-run, so both collapse into one
        // register-resident flag instead of two member loads per instruction.
        const bool _instr = trace_enabled || (bool)debug_hook;
        // Hot dispatch: no gc_tick here — checked only at call/jump sites
        #define _NEXT() do { \
            if (__builtin_expect(lip >= lend, 0)) goto _dispatch_exit; \
            cur = code + lip++; a = cur->a; b = cur->b; c = cur->c; \
            if (__builtin_expect(_instr, 0)) vm_instrument_step(chunk, *cur, lip - 1, *fp, R); \
            goto *_dt[(uint8_t)cur->op]; \
        } while(0)
        // GC tick: checked only at CALL/JUMP sites (not every instruction)
        #define _GC_TICK() do { \
            bool _tensor_pressure = tensor_gc_pressure(); \
            if (__builtin_expect(_tensor_pressure || ++gc_tick > 10000, 0)) { \
                gc_tick = 0; if (_tensor_pressure || GC::get_objects().size() > gc_threshold) run_gc(); \
            } \
        } while(0)
        static const void* const _dt[] = {
            &&_L_LOAD_CONST,  &&_L_LOAD_NIL,    &&_L_LOAD_BOOL,   &&_L_MOVE,
            &&_L_LOAD_GLOBAL, &&_L_STORE_GLOBAL, &&_L_LOAD_UPVAL,  &&_L_STORE_UPVAL,
            &&_L_ADD,         &&_L_SUB,          &&_L_MUL,         &&_L_DIV,    &&_L_MOD,
            &&_L_BIT_AND,     &&_L_BIT_OR,       &&_L_BIT_XOR,     &&_L_LSHIFT, &&_L_RSHIFT,
            &&_L_CMP_EQ,      &&_L_CMP_NEQ,      &&_L_CMP_LT,      &&_L_CMP_LTE,
            &&_L_CMP_GT,      &&_L_CMP_GTE,
            &&_L_NEG,         &&_L_BIT_NOT,      &&_L_LOGICAL_NOT,
            &&_L_JUMP,        &&_L_JUMP_IF_FALSE, &&_L_JUMP_IF_TRUE,
            &&_L_CALL_FUNC,   &&_L_CALL_BUILTIN, &&_L_METHOD_CALL, &&_L_SUPER_CALL,
            &&_L_RETURN_VAL,  &&_L_RETURN_NONE,
            &&_L_MAKE_ARRAY,  &&_L_MAKE_DICT,    &&_L_INDEX_GET,   &&_L_INDEX_SET,
            &&_L_DOT_GET,     &&_L_DOT_SET,      &&_L_OP_IN,
            &&_L_NEW_INSTANCE,&&_L_DEF_FUNC,     &&_L_MAKE_LAMBDA, &&_L_DEF_CLASS,
            &&_L_TRY_BEGIN,   &&_L_TRY_END,      &&_L_OP_THROW,
            &&_L_FOREACH_NEXT,&&_L_DICT_KEYS,
            &&_L_PRINT,       &&_L_PRINT_NO_NL,  &&_L_USE_LIB,    &&_L_HALT,   &&_L_NOP,
        };
        #define _CASE(op) _L_##op: {
        #define _END_CASE } _NEXT();
_reenter:
        try {
        _NEXT();   // first dispatch
#else
        #define _CASE(op) case JitOp::op: {
        #define _END_CASE break; }
        #define _GC_TICK() do { \
            bool _tensor_pressure = tensor_gc_pressure(); \
            if (__builtin_expect(_tensor_pressure || ++gc_tick > 10000, 0)) { \
                gc_tick = 0; if (_tensor_pressure || GC::get_objects().size() > gc_threshold) run_gc(); \
            } \
        } while(0)
_reenter:
        try {
        while (lip < lend) {
            const JitInst& inst = code[lip++];
            uint16_t a = inst.a, b = inst.b, c = inst.c;
            if (trace_enabled) {
                std::cerr << "[trace] ip=" << (lip - 1)
                          << " line=" << inst.line
                          << " op=" << op_to_str(inst.op)
                          << " a=" << (int)a << " b=" << (int)b << " c=" << (int)c
                          << " operand=" << inst.operand << "\n";
            }
            if (debug_hook) debug_before_instruction(chunk, inst, lip - 1, *fp, R);
            switch (inst.op) {
#endif
            // ──────────────────────────────────────────────────────────
            _CASE(LOAD_CONST)
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.constants.size())
                    throw JitThrow{"LOAD_CONST out of range", inst.line};
                R[a] = chunk.constants[inst.operand];
            _END_CASE
            _CASE(LOAD_NIL)   R[a] = Value::nil();            _END_CASE
            _CASE(LOAD_BOOL)  R[a] = Value(inst.operand != 0);_END_CASE
            _CASE(MOVE)       R[a] = R[b];                    _END_CASE

            _CASE(LOAD_GLOBAL)
                if (inst.operand >= 0 && (size_t)inst.operand < globals.size()) {
                    const std::string& gname = chunk.global_names[inst.operand];
                    bool initialized = (size_t)inst.operand < global_initialized.size() && global_initialized[inst.operand];
                    if (!initialized && inst.str_idx != 1 && !rt_classes.count(gname) && !is_stdlib_name(gname))
                        throw JitThrow{format_undefined_variable(gname, inst.line), inst.line};
                    R[a] = globals[inst.operand];
                } else {
                    R[a] = Value::nil();
                }
            _END_CASE
            _CASE(STORE_GLOBAL)
                if (inst.operand >= 0 && (size_t)inst.operand < globals.size()) {
                    globals[inst.operand] = R[a];
                    if ((size_t)inst.operand >= global_initialized.size())
                        global_initialized.resize(inst.operand + 1, false);
                    global_initialized[inst.operand] = true;
                }
            _END_CASE
            _CASE(LOAD_UPVAL)
                if (!fp->closure || inst.operand < 0 || (size_t)inst.operand >= fp->closure->upvalues.size())
                    throw JitThrow{"Upvalue LOAD_UPVAL out of range", inst.line};
                { GCUpvalue* uv = fp->closure->upvalues[inst.operand];
                  R[a] = uv->location ? *(uv->location) : uv->closed; }
            _END_CASE
            _CASE(STORE_UPVAL)
                if (!fp->closure || inst.operand < 0 || (size_t)inst.operand >= fp->closure->upvalues.size())
                    throw JitThrow{"Upvalue STORE_UPVAL out of range", inst.line};
                { GCUpvalue* uv = fp->closure->upvalues[inst.operand];
                  if (uv->location) *(uv->location) = R[a]; else uv->closed = R[a]; }
            _END_CASE

            // ── Arithmetic ────────────────────────────────────────────
            _CASE(ADD) if (__builtin_expect(_prof != nullptr, 0)) _prof->record_arith(lip-1, R[b].is_num() && R[c].is_num()); R[a] = R[b] + R[c]; _END_CASE
            _CASE(SUB) if (__builtin_expect(_prof != nullptr, 0)) _prof->record_arith(lip-1, R[b].is_num() && R[c].is_num()); if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] 타입 불일치", inst.line}; R[a] = R[b] - R[c]; _END_CASE
            _CASE(MUL) if (__builtin_expect(_prof != nullptr, 0)) _prof->record_arith(lip-1, R[b].is_num() && R[c].is_num()); if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] 타입 불일치", inst.line}; R[a] = R[b] * R[c]; _END_CASE
            _CASE(DIV) if (__builtin_expect(_prof != nullptr, 0)) _prof->record_arith(lip-1, R[b].is_num() && R[c].is_num()); if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] type mismatch", inst.line}; if (R[c].as_num() == 0.0) { if (fp->in_try) { R[fp->catch_var_reg] = Value(std::string("[E202] division by zero")); lip = fp->catch_ip; fp->in_try = false; } else throw JitThrow{"[E202] division by zero", inst.line}; } else R[a] = R[b] / R[c]; _END_CASE
            _CASE(MOD) if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] type mismatch", inst.line}; if (R[c].as_num() == 0.0) { if (fp->in_try) { R[fp->catch_var_reg] = Value(std::string("[E202] modulo by zero")); lip = fp->catch_ip; fp->in_try = false; } else throw JitThrow{"[E202] modulo by zero", inst.line}; } else R[a] = R[b].mod(R[c]); _END_CASE
            _CASE(NEG) if (!R[b].is_num()) throw JitThrow{"[E200] 타입 불일치", inst.line}; R[a] = R[b].negate();  _END_CASE

            // ── Compare ───────────────────────────────────────────────
            _CASE(CMP_EQ)  R[a] = Value(R[b].eq(R[c]));  _END_CASE
            _CASE(CMP_NEQ) R[a] = Value(R[b].neq(R[c])); _END_CASE
            _CASE(CMP_LT)  if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] ordering comparison operands must be numbers", inst.line}; R[a] = Value(R[b].lt(R[c]));  _END_CASE
            _CASE(CMP_LTE) if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] ordering comparison operands must be numbers", inst.line}; R[a] = Value(R[b].lte(R[c])); _END_CASE
            _CASE(CMP_GT)  if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] ordering comparison operands must be numbers", inst.line}; R[a] = Value(R[b].gt(R[c]));  _END_CASE
            _CASE(CMP_GTE) if (!R[b].is_num() || !R[c].is_num()) throw JitThrow{"[E200] ordering comparison operands must be numbers", inst.line}; R[a] = Value(R[b].gte(R[c])); _END_CASE

            // ── Bitwise / logic ───────────────────────────────────────
            _CASE(LOGICAL_NOT) R[a] = R[b].logical_not(); _END_CASE
            _CASE(BIT_AND) R[a] = sura_checked_bit_binary(JitOp::BIT_AND, R[b], R[c], inst.line); _END_CASE
            _CASE(BIT_OR)  R[a] = sura_checked_bit_binary(JitOp::BIT_OR,  R[b], R[c], inst.line); _END_CASE
            _CASE(BIT_XOR) R[a] = sura_checked_bit_binary(JitOp::BIT_XOR, R[b], R[c], inst.line); _END_CASE
            _CASE(BIT_NOT) R[a] = sura_checked_bit_not(R[b], inst.line); _END_CASE
            _CASE(LSHIFT)  R[a] = sura_checked_bit_binary(JitOp::LSHIFT, R[b], R[c], inst.line); _END_CASE
            _CASE(RSHIFT)  R[a] = sura_checked_bit_binary(JitOp::RSHIFT, R[b], R[c], inst.line); _END_CASE

            // ── Control flow ──────────────────────────────────────────
            _CASE(JUMP)           lip = (size_t)inst.operand; _GC_TICK();             _END_CASE
            _CASE(JUMP_IF_FALSE)  { bool _t = R[a].truthy(); if (__builtin_expect(_prof != nullptr, 0)) _prof->record_branch(lip-1, !_t); if (!_t) { lip = (size_t)inst.operand; _GC_TICK(); } } _END_CASE
            _CASE(JUMP_IF_TRUE)   { bool _t = R[a].truthy(); if (__builtin_expect(_prof != nullptr, 0)) _prof->record_branch(lip-1,  _t); if  (_t) { lip = (size_t)inst.operand; _GC_TICK(); } } _END_CASE
            
            _CASE(MAKE_LAMBDA) {
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.func_table.size())
                    throw JitThrow{"invalid func_idx MAKE_LAMBDA", inst.line};
                const auto& fi2 = chunk.func_table[inst.operand];
                std::string fname = fi2.name.empty() ? "<lambda>" : fi2.name;
                GCClosure* clos = GC::allocate<GCClosure>(fname);
                clos->func_idx = inst.operand;
                for (const auto& up : fi2.upvalues) {
                    if (up.is_local) {
                        clos->upvalues.push_back(capture_upvalue(&R[up.index]));
                    } else {
                        if (!fp->closure || up.index < 0 || (size_t)up.index >= fp->closure->upvalues.size())
                            throw JitThrow{"Upvalue MAKE_LAMBDA out of range", inst.line};
                        clos->upvalues.push_back(fp->closure->upvalues[up.index]);
                    }
                }
                R[a] = Value((GCObject*)clos);
            } _END_CASE
            _CASE(DEF_CLASS) {
                JitClassInfo ci = chunk.class_table[inst.operand];
                if (inst.c == JIT_CLASS_DEFAULTS_MARKER) {
                    if (static_cast<size_t>(inst.b) != ci.field_defaults.size())
                        throw JitThrow{"invalid class field default metadata", inst.line};
                    for (size_t i = 0; i < ci.field_defaults.size(); ++i)
                        ci.field_defaults[i] = R[static_cast<size_t>(inst.a) + i];
                }
                if (!ci.parent.empty() && rt_classes.count(ci.parent)) {
                    JitClassInfo& p = rt_classes[ci.parent];
                    std::vector<Value> new_defs = p.field_defaults;
                    std::unordered_map<std::string, int> new_idx = p.field_indices;
                    int offset = (int)p.field_defaults.size();
                    for (auto& [k, v] : ci.field_indices) {
                        new_idx[k] = offset + v;
                        new_defs.push_back(ci.field_defaults[v]);
                    }
                    ci.field_defaults = new_defs;
                    ci.field_indices  = new_idx;
                }
                rt_classes[chunk.global_names[inst.str_idx]] = ci;
                if (inst.str_idx >= 0) {
                    if ((size_t)inst.str_idx >= global_initialized.size())
                        global_initialized.resize(inst.str_idx + 1, false);
                    global_initialized[inst.str_idx] = true;
                }
            } _END_CASE

            _CASE(PRINT) {
                std::string out;
                int lim = (int)fp->reg_count;
                for (int i = 0; i < inst.operand && (int)(a+i) < lim; ++i) out += R[a+i].to_str();
                std::cout << out << "\n";
            } _END_CASE
            _CASE(PRINT_NO_NL) {
                std::string out;
                int lim = (int)fp->reg_count;
                for (int i = 0; i < inst.operand && (int)(a+i) < lim; ++i) out += R[a+i].to_str();
                std::cout << out;
            } _END_CASE

            _CASE(MAKE_ARRAY) {
                Value arr = Value::make_array();
                for (int i = 0; i < inst.operand; ++i) arr.as_arr()->elements.push_back(R[b+i]);
                R[a] = arr;
            } _END_CASE
            _CASE(MAKE_DICT) {
                Value dict = Value::make_dict();
                for (int i = 0; i < inst.operand; ++i)
                    dict.dict_set(R[b+i*2].to_str(), R[b+i*2+1]);
                R[a] = dict;
            } _END_CASE

            _CASE(INDEX_GET) {
                if      (R[b].is_arr())  {
                    int idx = (int)R[c].to_num();
                    auto* arr = R[b].as_arr();
                    if (idx < 0) idx += (int)arr->elements.size();
                    if (idx >= 0 && idx < (int)arr->elements.size()) R[a] = arr->elements[idx];
                    else throw JitThrow{"[E202] 배열 범위 초과", inst.line};
                }
                else if (R[b].is_dict()) R[a] = R[b].dict_get(R[c].to_str());
                else if (R[b].is_str()) {
                    int i = (int)R[c].to_num();
                    const std::string& s = R[b].as_str_ref();
                    if (i < 0) i += (int)s.size();
                    R[a] = (i>=0 && i<(int)s.size()) ? Value(std::string(1,s[i])) : Value::nil();
                } else if (R[b].is_nil()) {
                    throw JitThrow{"[E201] nil 역참조", inst.line};
                } else R[a] = Value::nil();
            } _END_CASE
            _CASE(INDEX_SET) {
                if      (R[a].is_arr())  R[a].arr_set((int)R[b].to_num(), R[c]);
                else if (R[a].is_dict()) R[a].dict_set(R[b].to_str(), R[c]);
            } _END_CASE
            
            
            _CASE(DOT_GET) {
                const std::string& prop = chunk.get_string(inst.str_idx);
                if (R[b].is_inst()) {
                    GCInstance* iobj = R[b].as_inst();
                    const std::string& cname = iobj->type_name();
                    if (inst.ic_cache != -1 && iobj->fields.size() > (size_t)inst.ic_cache) {
                        R[a] = iobj->fields[inst.ic_cache];
                    } else {
                        int offset = -1;
                        if (rt_classes.count(cname)) {
                            auto& cl = rt_classes[cname];
                            if (cl.field_indices.count(prop)) offset = cl.field_indices[prop];
                        }
                        if (offset != -1) {
                            auto& cl2 = rt_classes[cname];
                            if (iobj->fields.size() < cl2.field_defaults.size())
                                iobj->fields.resize(cl2.field_defaults.size(), Value::nil());
                            R[a] = iobj->fields[offset];
                            inst.ic_cache = offset;
                            inst.ic_class = &rt_classes.at(cname); // JIT IC guard
                        } else R[a] = Value::nil();
                    }
                } else if (R[b].is_dict()) R[a] = R[b].dict_get(prop);
                else if (R[b].is_nil()) throw JitThrow{"[E201] nil 역참조", inst.line};
                else R[a] = Value::nil();
            } _END_CASE
            _CASE(DOT_SET) {
                const std::string& prop = chunk.get_string(inst.str_idx);
                if (R[a].is_inst()) {
                    GCInstance* iobj = R[a].as_inst();
                    const std::string& cname = iobj->type_name();
                    if (inst.ic_cache != -1 && iobj->fields.size() > (size_t)inst.ic_cache) {
                        iobj->fields[inst.ic_cache] = R[b];
                    } else {
                        auto class_it = rt_classes.find(cname);
                        if (class_it != rt_classes.end()) {
                            JitClassInfo& cl = class_it->second;
                            auto field_it = cl.field_indices.find(prop);
                            int offset;
                            if (field_it != cl.field_indices.end()) {
                                offset = field_it->second;
                            } else {
                                offset = (int)cl.field_indices.size();
                                cl.field_indices[prop] = offset;
                                cl.field_defaults.push_back(Value::nil());
                            }
                            // A field added to any instance widens the class
                            // layout, but instances built before that are still
                            // the old width. Writing at the new offset without
                            // growing this one wrote past the end of its field
                            // storage - the value was lost on read, and the
                            // store itself was out of bounds.
                            if (iobj->fields.size() < cl.field_defaults.size())
                                iobj->fields.resize(cl.field_defaults.size(), Value::nil());
                            if (offset >= 0 && (size_t)offset < iobj->fields.size()) {
                                iobj->fields[offset] = R[b];
                                inst.ic_cache = offset;
                                inst.ic_class = &cl; // JIT IC guard
                            }
                        }
                    }  // closes else { from ic_cache miss
                } else if (R[a].is_dict()) R[a].dict_set(prop, R[b]);
            } _END_CASE

            _CASE(OP_IN) {
                if (R[c].is_arr()) {
                    bool f=false;
                    for (auto& el : R[c].as_arr()->elements) if (R[b].eq(el)){f=true;break;}
                    R[a] = Value(f);
                } else if (R[c].is_dict()) R[a] = Value(R[c].dict_has(R[b].to_str()));
                else if (R[c].is_str()) R[a] = Value(R[c].as_str_ref().find(R[b].to_str()) != std::string::npos);
                else R[a] = Value::nil();
            } _END_CASE
            _CASE(CALL_FUNC) {
                _GC_TICK();
                Value fn_val = R[b];
                if (!fn_val.is_closure()) {
                    std::string name = (inst.str_idx >= 0) ? chunk.get_string(inst.str_idx) : "";
                    Value std_result_for_call;
                    // 1) Struct / class constructor call: `Vec2(3, 4)` → NEW_INSTANCE
                    if (!name.empty() && rt_classes.count(name)) {
                        JitClassInfo* cls_ptr = &rt_classes.at(name);
                        auto ctor = find_method(name, "\uc0dd\uc131\uc790");
                        if (!ctor) ctor = find_method(name, "init");
                        const std::vector<int>* plain_fields = ctor ? plain_ctor_fields(cls_ptr, ctor) : nullptr;
                        Value inst_val = plain_fields
                            ? make_plain_ctor_instance(cls_ptr, ctor, *plain_fields, R, &inst)
                            : make_initialized_instance(cls_ptr, inst.line);
                        R[a] = inst_val;
                        if (plain_fields) {
                            inst.ic_class = cls_ptr;
                            inst.ic_method = ctor;
                            inst.ic_native_fn = nullptr;
                            inst.ic_native_frame_regs = 0;
                        } else if (!ctor) {
                            // Constructor-less class: cache the class itself so
                            // neither tier repeats the failed method lookups.
                            inst.ic_class = cls_ptr;
                            inst.ic_method = nullptr;
                            inst.ic_native_fn = nullptr;
                            inst.ic_native_frame_regs = 0;
                        } else {
                            size_t ctor_count = method_frame_regs(chunk, *ctor);
                            size_t ctor_base  = alloc_frame_regs(ctor_count, inst.line);
                            Value* CR = &value_stack[ctor_base];
                            bind_method_args(chunk, *ctor, CR, inst_val, &R[inst.c],
                                             static_cast<size_t>(std::max(0, inst.operand)));
                            _SAVE_IP();
                            { CallFrame& cf = push_frame(inst.line);
                            cf.reg_base  = ctor_base;
                            cf.reg_count = ctor_count;
                            cf.closure   = nullptr;
                            cf.method    = ctor;
                            cf.chunk     = fp->chunk;
                            cf.ip        = ctor->entry_ip;
                            cf.end_ip    = ctor->end_ip;
                            cf.ret_reg   = (uint16_t)-1;
                            cf.in_try    = false; }
                            fp = &frame_pool[frame_top-1];
                            _LOAD_FRAME();
                        }
                    }
                    // 2) Standard library (math/string/array/conversion/Error/clock/type)
                    else if (!name.empty() && SuraStd::try_dispatch(name, &R[inst.c], inst.operand, inst.line, std_result_for_call)) {
                        R[a] = std_result_for_call;
                    }
                    // 3) Built-ins not in the dispatch table (side-effecting)
                    else if (name == "exit") {
                        exit(0);
                    }
                    // 4) Typo detection across user globals + stdlib names
                    else {
                        if (name.empty()) name = fn_val.to_str();
                        throw JitThrow{format_undefined_function(name), inst.line};
                    }
                    // _END_CASE will call _NEXT(); just fall through
                } else {
                    GCClosure* closure = fn_val.as_closure();
                    if (closure->func_idx < 0 || (size_t)closure->func_idx >= chunk.func_table.size())
                        throw JitThrow{"invalid func_idx (CALL_FUNC)", inst.line};
                    const auto& fi = chunk.func_table[closure->func_idx];
                    if ((size_t)inst.operand > fi.params.size())
                        throw JitThrow{"[E300] 잘못된 인자 개수", inst.line};
                    if (__builtin_expect(_prof != nullptr, 0)) _prof->record_call(lip-1, fi.name);

                    size_t new_count = function_frame_regs(chunk, fi);
                    size_t new_base  = alloc_frame_regs(new_count, inst.line);
                    Value* NR = &value_stack[new_base];
                    bind_function_args(chunk, fi, NR, &R[inst.c],
                                       static_cast<size_t>(std::max(0, inst.operand)));

                    // ── Native JIT path (if enabled) ──────────────
                    // Lazy compile: wait until interpreter runs warm ICs so
                    // ic_cache slots are warm before we bake them into native code.
                    NativeFunc* native = nullptr;
                    if (jit_enabled) native = resolve_native(chunk, fi, closure->func_idx);

                    if (native) {
                        uint64_t bits = native->fn(this, NR, chunk.constants.data());
                        if (__builtin_expect(bits == SURA_JIT_DEOPT_SENTINEL, 0)) {
                            // A guard rejected this call. Guards can fire after
                            // the body has already written frame registers (a
                            // zero divisor is only known mid-body), so rebind
                            // the arguments from the still-intact caller
                            // registers before the VM re-runs the call.
                            note_native_deopt(closure->func_idx);
                            bind_function_args(chunk, fi, NR, &R[inst.c],
                                               static_cast<size_t>(std::max(0, inst.operand)));
                            native = nullptr;
                        } else {
                            R[a] = Value::from_bits(bits);
                            stack_top = new_base; // release the callee's frame regs
                        }
                    }
                    if (!native) {
                        // ── Iterative call: write directly to frame_pool slot ──
                        _SAVE_IP();
                        { CallFrame& nf = push_frame(inst.line);
                        nf.reg_base  = new_base;
                        nf.reg_count = new_count;
                        nf.closure   = closure;
                        nf.method    = nullptr;
                        nf.chunk     = fp->chunk;
                        nf.ip        = fi.entry_ip;
                        nf.end_ip    = fi.end_ip;
                        nf.ret_reg   = a;
                        nf.in_try    = false; }
                        fp = &frame_pool[frame_top-1];
                        _LOAD_FRAME();
                    }
                } // closes else (is_closure)
            } _END_CASE
            _CASE(CALL_BUILTIN) {
                _GC_TICK();
                std::string full_name = chunk.get_string(inst.str_idx);
                size_t sep = full_name.find('\0');
                std::string cmd = full_name.substr(0, sep);
                
                if (cmd == "input") {
                    std::string in;
                    if (inst.operand > 0) std::cout << R[b].to_str();
                    std::getline(std::cin, in);
                    R[a] = Value(in);
                } else if (cmd == "exit") {
                    exit(0);
                } else if (cmd == "clock") {
                    auto now = std::chrono::steady_clock::now().time_since_epoch();
                    R[a] = Value(std::chrono::duration<double>(now).count());
                } else if (cmd == "type") {
                    R[a] = Value(R[b].to_str());
                } else {
                    Value std_result;
                    if (SuraStd::try_dispatch(cmd, &R[b], inst.operand, inst.line, std_result)) {
                        R[a] = std_result;
                    } else {
                        static const std::vector<std::string> BUILTINS = {"print", "input", "exit", "clock", "type"};
                        std::string sug = sura_suggest(cmd, BUILTINS);
                        std::string msg = "unknown command '" + cmd + "'";
                        if (!sug.empty()) msg += " (did you mean '" + sug + "'?)";
                        throw JitThrow{msg, inst.line};
                    }
                }
            } _END_CASE
            _CASE(METHOD_CALL) {
                _GC_TICK();
                const std::string& meth = chunk.get_string(inst.str_idx);
                int nargs = inst.operand;
                if (__builtin_expect(_prof != nullptr, 0)) {
                    std::string receiver = R[b].is_inst()
                        ? R[b].as_inst()->type_name() + "." + meth
                        : (R[b].is_arr() ? "[array]." : R[b].is_dict() ? "[dict]." : "[?].") + meth;
                    _prof->record_call(lip-1, receiver);
                }
                // Built-in array methods
                Value builtin_method_result;
                if (dispatch_builtin_method(R[b], meth, &R[b+1], nargs, inst.line, builtin_method_result)) {
                    R[a] = builtin_method_result;
                } else if (R[b].is_arr()) {
                    GCArray* arr = R[b].as_arr();
                    if (meth == "push" || meth == "append") {
                        if (nargs >= 1) arr->elements.push_back(R[b+1]);
                        R[a] = Value::nil();
                    } else if (meth == "pop") {
                        if (!arr->elements.empty()) { R[a] = arr->elements.back(); arr->elements.pop_back(); }
                        else R[a] = Value::nil();
                    } else if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)arr->elements.size());
                    } else if (meth == "remove") {
                        int idx = nargs >= 1 ? (int)R[b+1].to_num() : -1;
                        if (idx < 0) idx += (int)arr->elements.size();
                        if (idx >= 0 && idx < (int)arr->elements.size()) {
                            R[a] = arr->elements[idx];
                            arr->elements.erase(arr->elements.begin() + idx);
                        } else R[a] = Value::nil();
                    } else if (meth == "contains" || meth == "has") {
                        bool found = false;
                        if (nargs >= 1) for (auto& el : arr->elements) if (el.eq(R[b+1])) { found = true; break; }
                        R[a] = Value(found);
                    } else if (meth == "join") {
                        std::string sep = (nargs >= 1) ? R[b+1].to_str() : "";
                        std::string out;
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            if (i > 0) out += sep;
                            out += arr->elements[i].to_str();
                        }
                        R[a] = Value(out);
                    } else if (meth == "slice") {
                        int from = nargs >= 1 ? (int)R[b+1].to_num() : 0;
                        int to   = nargs >= 2 ? (int)R[b+2].to_num() : (int)arr->elements.size();
                        if (from < 0) from += (int)arr->elements.size();
                        if (to   < 0) to   += (int)arr->elements.size();
                        from = std::max(0, std::min(from, (int)arr->elements.size()));
                        to   = std::max(from, std::min(to, (int)arr->elements.size()));
                        Value nv = Value::make_array();
                        nv.as_arr()->elements.insert(nv.as_arr()->elements.end(),
                            arr->elements.begin() + from, arr->elements.begin() + to);
                        R[a] = nv;
                    } else if (meth == "sort") {
                        std::sort(arr->elements.begin(), arr->elements.end(),
                            [](const Value& x, const Value& y) { return x.lt(y); });
                        R[a] = Value::nil();
                    } else if (meth == "reverse") {
                        std::reverse(arr->elements.begin(), arr->elements.end());
                        R[a] = Value::nil();
                    } else if (meth == "clear") {
                        arr->elements.clear(); R[a] = Value::nil();
                    } else if (meth == "insert") {
                        if (nargs >= 2) {
                            int idx = (int)R[b+1].to_num();
                            if (idx < 0) idx += (int)arr->elements.size();
                            idx = std::max(0, std::min(idx, (int)arr->elements.size()));
                            arr->elements.insert(arr->elements.begin() + idx, R[b+2]);
                        }
                        R[a] = Value::nil();
                    } else {
                        R[a] = Value::nil();
                    }
                // Built-in string methods
                } else if (R[b].is_str()) {
                    const std::string& s = R[b].as_str_ref();
                    if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)s.size());
                    } else if (meth == "upper") {
                        std::string u = s;
                        for (char& ch : u) ch = (char)toupper((unsigned char)ch);
                        R[a] = Value(u);
                    } else if (meth == "lower") {
                        std::string l = s;
                        for (char& ch : l) ch = (char)tolower((unsigned char)ch);
                        R[a] = Value(l);
                    } else if (meth == "trim") {
                        size_t st = s.find_first_not_of(" \t\n\r");
                        size_t en = s.find_last_not_of(" \t\n\r");
                        R[a] = (st == std::string::npos) ? Value(std::string("")) : Value(s.substr(st, en - st + 1));
                    } else if (meth == "contains" || meth == "has") {
                        std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.find(needle) != std::string::npos);
                    } else if (meth == "startswith") {
                        std::string pre = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.size() >= pre.size() && s.substr(0, pre.size()) == pre);
                    } else if (meth == "endswith") {
                        std::string suf = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf);
                    } else if (meth == "find" || meth == "index") {
                        std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
                        auto pos = s.find(needle);
                        R[a] = (pos != std::string::npos) ? Value((double)pos) : Value(-1.0);
                    } else if (meth == "replace") {
                        if (nargs >= 2) {
                            std::string from = R[b+1].to_str(), to = R[b+2].to_str();
                            std::string res; size_t p = 0;
                            while (true) {
                                size_t f = s.find(from, p);
                                if (f == std::string::npos) { res += s.substr(p); break; }
                                res += s.substr(p, f - p) + to;
                                p = f + from.size();
                            }
                            R[a] = Value(res);
                        } else R[a] = R[b];
                    } else if (meth == "split") {
                        std::string sep = nargs >= 1 ? R[b+1].to_str() : " ";
                        Value arr = Value::make_array();
                        if (sep.empty()) {
                            for (char ch : s) arr.as_arr()->elements.push_back(Value(std::string(1, ch)));
                        } else {
                            size_t p = 0;
                            while (true) {
                                size_t f = s.find(sep, p);
                                if (f == std::string::npos) { arr.as_arr()->elements.push_back(Value(s.substr(p))); break; }
                                arr.as_arr()->elements.push_back(Value(s.substr(p, f - p)));
                                p = f + sep.size();
                            }
                        }
                        R[a] = arr;
                    } else if (meth == "slice" || meth == "substr") {
                        int from = nargs >= 1 ? (int)R[b+1].to_num() : 0;
                        int to   = nargs >= 2 ? (int)R[b+2].to_num() : (int)s.size();
                        if (from < 0) from += (int)s.size();
                        if (to   < 0) to   += (int)s.size();
                        from = std::max(0, std::min(from, (int)s.size()));
                        to   = std::max(from, std::min(to, (int)s.size()));
                        R[a] = Value(s.substr(from, to - from));
                    } else if (meth == "to_num" || meth == "tonumber") {
                        try { R[a] = Value(std::stod(s)); } catch (...) { R[a] = Value::nil(); }
                    } else {
                        R[a] = Value::nil();
                    }
                // Built-in dict methods
                } else if (R[b].is_dict()) {
                    GCDict* dict = R[b].as_dict();
                    if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)dict->elements.size());
                    } else if (meth == "has" || meth == "contains") {
                        std::string key = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(dict->elements.count(key) > 0);
                    } else if (meth == "keys") {
                        Value arr = Value::make_array();
                        for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(Value(k));
                        R[a] = arr;
                    } else if (meth == "values") {
                        Value arr = Value::make_array();
                        for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(v);
                        R[a] = arr;
                    } else if (meth == "remove" || meth == "delete") {
                        if (nargs >= 1) dict->elements.erase(R[b+1].to_str());
                        R[a] = Value::nil();
                    } else if (meth == "clear") {
                        dict->elements.clear(); R[a] = Value::nil();
                    } else {
                        R[a] = Value::nil();
                    }
                // User-defined class method
                } else if (R[b].is_inst()) {
                    _GC_TICK();
                    auto mi = find_method(R[b].as_inst()->type_name(), meth);
                    if (mi) {
                        // Warm up monomorphic IC cache for JIT fast path
                        if (R[b].as_inst()->jit_info) {
                            inst.ic_method = mi;
                            inst.ic_class  = R[b].as_inst()->jit_info;
                        }
                        // ── Eager method JIT (Phase 4) ─────────────────────
                        // Compile method from interpreter side too, so when the
                        // caller is JIT-compiled later, inst.ic_native_fn is
                        // already set and the inline METHOD_CALL fast path
                        // (Phase 3) can be emitted.
                        if (native_allowed() && R[b].as_inst()->jit_info &&
                            inst.ic_native_fn == nullptr) {
                            auto nit = native_methods.find(mi);
                            if (nit == native_methods.end() && !jit_method_failed.count(mi)) {
                                if (++method_warm_count[mi] >= METHOD_LAZY_JIT_THRESHOLD) {
                                    try {
                                        NativeCompiler nc(chunk, *mi);
                                        auto compiled = nc.compile();
                                        if (compiled) nit = native_methods.emplace(mi, std::move(compiled)).first;
                                        else jit_method_failed.insert(mi);
                                    } catch (...) { jit_method_failed.insert(mi); }
                                }
                            }
                            if (nit != native_methods.end()) {
                                inst.ic_native_fn = (void*)nit->second->fn;
                                inst.ic_native_frame_regs = nit->second->frame_regs;
                            }
                        }
                        size_t new_count = method_frame_regs(chunk, *mi);
                        size_t new_base  = alloc_frame_regs(new_count, inst.line);
                        Value* NR2 = &value_stack[new_base];
                        bind_method_args(chunk, *mi, NR2, R[b], &R[b + 1],
                                         static_cast<size_t>(std::max(0, nargs)));
                        // ── Iterative method call ──
                        _SAVE_IP();
                        { CallFrame& mf = push_frame(inst.line);
                        mf.reg_base  = new_base;
                        mf.reg_count = new_count;
                        mf.closure   = nullptr;
                        mf.method    = mi;
                        mf.chunk     = fp->chunk;
                        mf.ip        = mi->entry_ip;
                        mf.end_ip    = mi->end_ip;
                        mf.ret_reg   = a;
                        mf.in_try    = false; }
                        fp = &frame_pool[frame_top-1];
                        _LOAD_FRAME();
                    } else if (dispatch_instance_callable_field(R[b], meth, &R[b+1], nargs, inst.line, R[a])) {
                        // Function-valued instance fields are callable via obj.field(args...).
                    } else R[a] = Value::nil();
                } else {
                    R[a] = Value::nil();
                }
            } _END_CASE
            _CASE(NEW_INSTANCE) {
                std::string cls = chunk.get_string(inst.str_idx);
                auto class_found = rt_classes.find(cls);
                Value inst_val = class_found == rt_classes.end()
                    ? Value::make_inst(cls)
                    : make_initialized_instance(&class_found->second, inst.line);
                R[a] = inst_val;  // store BEFORE ctor so ctor return is discarded

                auto ctor = find_method(cls, "\uc0dd\uc131\uc790"); if (!ctor) ctor = find_method(cls, "init");
                if (ctor) {
                    size_t ctor_count = method_frame_regs(chunk, *ctor);
                    size_t ctor_base  = alloc_frame_regs(ctor_count, inst.line);
                    Value* CR = &value_stack[ctor_base];
                    bind_method_args(chunk, *ctor, CR, inst_val, &R[b],
                                     static_cast<size_t>(std::max(0, inst.operand)));
                    // ── Iterative ctor call: discard return value ──
                    _SAVE_IP();
                    { CallFrame& cf = push_frame(inst.line);
                    cf.reg_base  = ctor_base;
                    cf.reg_count = ctor_count;
                    cf.closure   = nullptr;
                    cf.method    = ctor;
                    cf.chunk     = fp->chunk;
                    cf.ip        = ctor->entry_ip;
                    cf.end_ip    = ctor->end_ip;
                    cf.ret_reg   = (uint16_t)-1;
                    cf.in_try    = false; }
                    fp = &frame_pool[frame_top-1];
                    _LOAD_FRAME();
                }
            } _END_CASE
            _CASE(RETURN_VAL) {
                Value result = R[a];
                uint16_t save_ret = fp->ret_reg;
                if (!open_upvalues.empty()) close_upvalues(&value_stack[fp->reg_base]);
                stack_top = fp->reg_base;
                (--frame_top);
                if (frame_top <= entry_depth) return result;
                fp = &frame_pool[frame_top-1];
                _LOAD_FRAME();
                if (save_ret != (uint16_t)-1) R[save_ret] = result;
            } _END_CASE
            _CASE(RETURN_NONE) {
                uint16_t save_ret = fp->ret_reg;
                if (!open_upvalues.empty()) close_upvalues(&value_stack[fp->reg_base]);
                stack_top = fp->reg_base;
                (--frame_top);
                if (frame_top <= entry_depth) return Value::nil();
                fp = &frame_pool[frame_top-1];
                _LOAD_FRAME();
                if (save_ret != (uint16_t)-1) R[save_ret] = Value::nil();
            } _END_CASE
            _CASE(OP_THROW) {
                Value thrown = R[a];
                int thrown_line = inst.line;
                // Walk frame stack for nearest enclosing try/catch (cross-frame propagation)
                int caught = -1;
                for (int i = (int)frame_top - 1; i >= 0; --i) {
                    if (frame_pool[i].in_try) { caught = i; break; }
                }
                if (caught >= 0) {
                    // Unwind to the handler frame (iterative pop)
                    // Closures that escaped from discarded callees must own a
                    // closed copy before their register windows are reused.
                    // Normal returns and C++ JitThrow propagation perform the
                    // same cleanup; explicit Sura `throw` must not leave a
                    // GCUpvalue pointing into a dead value_stack frame.
                    for (size_t i = frame_top; i-- > (size_t)caught + 1; ) {
                        if (!open_upvalues.empty()) {
                            close_upvalues(&value_stack[frame_pool[i].reg_base]);
                        }
                    }
                    frame_top = (size_t)(caught + 1);
                    fp = &frame_pool[frame_top - 1];
                    stack_top = fp->reg_base + fp->reg_count;
                    R = &value_stack[fp->reg_base];
                    R[fp->catch_var_reg] = thrown;
                    lip = fp->catch_ip;
                    lend = fp->end_ip;
                    fp->in_try = false;
                } else {
                    // Escape as C++ exception — stack trace built in run()'s catch
                    JitThrow t;
                    t.thrown_value = thrown;
                    if (thrown.is_dict()) {
                        auto* d = thrown.as_dict();
                        auto mi = d->elements.find("message");
                        auto ti = d->elements.find("type");
                        if (mi != d->elements.end() && ti != d->elements.end())
                            t.message = ti->second.to_str() + ": " + mi->second.to_str();
                        else
                            t.message = thrown.to_str();
                    } else {
                        t.message = thrown.to_str();
                    }
                    t.line = thrown_line;
                    throw t;
                }
            } _END_CASE
            _CASE(TRY_BEGIN) {
                // a=catch_var_reg, operand=catch_ip
                fp->in_try        = true;
                fp->catch_ip      = (size_t)inst.operand;
                fp->catch_var_reg = a;
            } _END_CASE
            _CASE(TRY_END) { fp->in_try = false; } _END_CASE
            _CASE(FOREACH_NEXT) {
                // a=value_reg, b=iter_reg (index), c=collection_reg, operand=exit_jump
                if (R[c].is_arr()) {
                    int idx = (int)R[b].to_num();
                    auto* arr = R[c].as_arr();
                    if (idx < (int)arr->elements.size()) {
                        R[a] = arr->elements[idx];
                        R[b] = Value((double)(idx + 1));
                    } else { lip = (size_t)inst.operand; }
                } else if (R[c].is_str()) {
                    int idx = (int)R[b].to_num();
                    const std::string& s = R[c].as_str();
                    if (idx < (int)s.size()) {
                        R[a] = Value(std::string(1, s[idx]));
                        R[b] = Value((double)(idx + 1));
                    } else { lip = (size_t)inst.operand; }
                } else { lip = (size_t)inst.operand; }
            } _END_CASE
            _CASE(DICT_KEYS) {
                // a=keys_array_reg, b=collection_reg
                Value keys = Value::make_array();
                if (R[b].is_dict()) {
                    for (auto& [k, v] : R[b].as_dict()->elements)
                        keys.as_arr()->elements.push_back(Value(k));
                } else if (R[b].is_arr()) {
                    int n = (int)R[b].as_arr()->elements.size();
                    for (int i = 0; i < n; ++i) keys.as_arr()->elements.push_back(Value((double)i));
                } else if (R[b].is_str()) {
                    int n = (int)R[b].as_str_ref().size();
                    for (int i = 0; i < n; ++i) keys.as_arr()->elements.push_back(Value((double)i));
                }
                R[a] = keys;
            } _END_CASE
            _CASE(HALT)      { return Value::nil(); } _END_CASE
            _CASE(SUPER_CALL){ } _END_CASE
            _CASE(DEF_FUNC)  { } _END_CASE
            _CASE(USE_LIB) {
                std::string lib = chunk.get_string(inst.str_idx);
                if (is_stdlib_module(lib)) {
                    Value mod = make_stdlib_module(lib);
                    for (size_t gi = 0; gi < chunk.global_names.size(); ++gi) {
                        if (chunk.global_names[gi] == lib) {
                            if (globals.size() <= gi) globals.resize(gi + 1, Value::nil());
                            if (global_initialized.size() <= gi) global_initialized.resize(gi + 1, false);
                            globals[gi] = mod;
                            global_initialized[gi] = true;
                            break;
                        }
                    }
                }
            } _END_CASE
            _CASE(NOP)       { } _END_CASE
#ifdef __GNUC__
        _dispatch_exit: {
            // ip exhausted without explicit RETURN — treat as RETURN_NONE
            uint16_t save_ret = fp->ret_reg;
            if (!open_upvalues.empty()) close_upvalues(&value_stack[fp->reg_base]);
            stack_top = fp->reg_base;
            (--frame_top);
            if (frame_top <= entry_depth) return Value::nil();
            fp = &frame_pool[frame_top-1];
            _LOAD_FRAME();
            if (save_ret != (uint16_t)-1) R[save_ret] = Value::nil();
            _NEXT();
        }
        } catch (JitThrow& t) {
            int caught = -1;
            for (int i = (int)frame_top - 1; i >= (int)entry_depth; --i) {
                if (frame_pool[(size_t)i].in_try) { caught = i; break; }
            }
            if (caught < 0) throw;
            for (size_t i = frame_top; i-- > (size_t)caught + 1; ) {
                if (!open_upvalues.empty()) close_upvalues(&value_stack[frame_pool[i].reg_base]);
            }
            frame_top = (size_t)caught + 1;
            fp = &frame_pool[(size_t)caught];
            stack_top = fp->reg_base + fp->reg_count;
            R = &value_stack[fp->reg_base];
            R[fp->catch_var_reg] = t.thrown_value.is_nil()
                                 ? Value(t.message) : t.thrown_value;
            lip = fp->catch_ip;
            lend = fp->end_ip;
            fp->in_try = false;
            goto _reenter;
        }
#else
            } // end switch
            } catch (JitThrow& t) {
                if (fp->in_try) {
                    R[fp->catch_var_reg] = Value(t.message);
                    lip = fp->catch_ip;
                    lend = fp->end_ip;
                    fp->in_try = false;
                } else {
                    throw;
                }
            }
        } // end while
        // ip exhausted — treat as RETURN_NONE
        {
            uint16_t save_ret = fp->ret_reg;
            if (!open_upvalues.empty()) close_upvalues(&value_stack[fp->reg_base]);
            stack_top = fp->reg_base;
            (--frame_top);
            if (frame_top <= entry_depth) return Value::nil();
            fp = &frame_pool[frame_top-1];
            _LOAD_FRAME();
            if (save_ret != (uint16_t)-1) R[save_ret] = Value::nil();
            goto _reenter;
        }
#endif
    }
};

// Collection construction callbacks used by native functions.  The source
// values already live in the VM's published value stack, so a collection can
// safely run before allocation without losing temporary keys or values.
inline uint64_t JitVM::make_array_from_jit(Value* R, const JitInst* ins) {
    if (!R || !ins) return Value::nil().raw_bits();
    run_gc_for_native_allocation();

    Value array = Value::make_array();
    GCArray* elements = array.as_arr();
    if (ins->operand > 0) {
        elements->elements.reserve(static_cast<size_t>(ins->operand));
        for (int i = 0; i < ins->operand; ++i) {
            elements->elements.push_back(R[static_cast<size_t>(ins->b) + i]);
        }
    }
    return array.raw_bits();
}

inline uint64_t JitVM::make_dict_from_jit(Value* R, const JitInst* ins) {
    if (!R || !ins) return Value::nil().raw_bits();
    run_gc_for_native_allocation();

    Value dictionary = Value::make_dict();
    if (ins->operand > 0) {
        GCDict* entries = dictionary.as_dict();
        entries->elements.reserve(static_cast<size_t>(ins->operand));
        for (int i = 0; i < ins->operand; ++i) {
            const size_t key = static_cast<size_t>(ins->b) +
                               static_cast<size_t>(i) * 2U;
            dictionary.dict_set(R[key].to_str(), R[key + 1U]);
        }
    }
    return dictionary.raw_bits();
}

// ── dispatch_call_from_jit implementation ─────────────────────────
// Defined out-of-class so it can reference the fully-declared JitVM.
inline uint64_t JitVM::dispatch_call_from_jit(Value* R, const JitInst* ins) {
    if (!active_chunk) return Value::nil().raw_bits();
    run_gc_for_tensor_pressure();
    const JitChunk& chunk = *active_chunk;

    // ── Phase 7: monomorphic CALL_FUNC closure IC fast path ────────────
    // For recursive calls (e.g. fib(n-1) inside fib), every call goes through
    // this dispatcher and previously did a `native_funcs.find(func_idx)` hash
    // lookup. With the IC, after the first call we cache the matched closure's
    // func_idx in ins->ic_cache and the native_fn in ins->ic_native_fn, then
    // skip the lookup on every subsequent call.
    //
    // Disambiguation vs ctor IC: closure path requires ic_method == nullptr
    // (ctor path always sets ic_method to the ctor's JitMethodInfo*).
    if (ins->ic_native_fn != nullptr && ins->ic_method == nullptr &&
        ins->ic_cache >= 0) {
        Value fn_val_ic = R[ins->b];
        if (fn_val_ic.is_closure()) {
            GCClosure* cl = fn_val_ic.as_closure();
            if (cl->func_idx == ins->ic_cache) {
                const JitFuncInfo& fi = chunk.func_table[cl->func_idx];
                size_t cnt  = function_frame_regs(chunk, fi);
                size_t base = alloc_frame_regs(cnt, ins->line);
                Value* NR = &value_stack[base];
                bind_function_args(chunk, fi, NR, &R[ins->c],
                                   static_cast<size_t>(std::max(0, ins->operand)));
                SuraNativeFn fast_fn = reinterpret_cast<SuraNativeFn>(ins->ic_native_fn);
                uint64_t bits = fast_fn(this, NR, chunk.constants.data());
                if (__builtin_expect(bits == SURA_JIT_DEOPT_SENTINEL, 0)) {
                    // A guard rejected this call. Drop this site's IC, rebind
                    // the arguments (a mid-body guard may have overwritten
                    // frame registers), and finish the call in the VM.
                    note_native_deopt(cl->func_idx);
                    ins->ic_native_fn = nullptr;
                    bind_function_args(chunk, fi, NR, &R[ins->c],
                                       static_cast<size_t>(std::max(0, ins->operand)));
                    CallFrame cf;
                    cf.reg_base = base; cf.reg_count = cnt; cf.closure = cl;
                    cf.chunk = &chunk; cf.method = nullptr;
                    cf.ip = fi.entry_ip; cf.end_ip = fi.end_ip;
                    cf.ret_reg = (uint16_t)-1; cf.in_try = false;
                    return execute_frame(cf).raw_bits();
                }
                stack_top = base;
                return bits;
            }
        }
    }

    // ── Phase 6: monomorphic CALL_FUNC ctor IC fast path ──────────────
    // For repeated `Vec2(x,y)`-style ctor calls, skip the 4-5 hash lookups
    // (rt_classes.count + [] + .at + find_method "생성자" + find_method "init")
    // by using the cached class/ctor pointers populated on the first call.
    //
    // The IC slots (ic_class, ic_method, ic_native_fn) are only set in the
    // ctor branch of the slow path below, so a non-null ic_class implies
    // this call site is monomorphically a struct ctor call.
    if (ins->ic_class != nullptr && ins->ic_method != nullptr && !R[ins->b].is_closure()) {
        const JitClassInfo*  cls  = ins->ic_class;
        const JitMethodInfo* ctor = ins->ic_method;

        if (const std::vector<int>* plain_fields = plain_ctor_fields(cls, ctor)) {
            return make_plain_ctor_instance(cls, ctor, *plain_fields, R, ins).raw_bits();
        }

        if (ins->ic_native_fn == nullptr) {
            // Plain-ctor ICs intentionally leave ic_native_fn empty. If the
            // bytecode pattern changed after caching, fall through safely.
        } else {
            SuraNativeFn fn = reinterpret_cast<SuraNativeFn>(ins->ic_native_fn);

            Value obj = make_initialized_instance(cls, ins->line);

            size_t cnt  = method_frame_regs(chunk, *ctor);
            size_t base = alloc_frame_regs(cnt, ins->line);
            Value* NR = &value_stack[base];
            bind_method_args(chunk, *ctor, NR, obj, &R[ins->c],
                             static_cast<size_t>(std::max(0, ins->operand)));
            fn(this, NR, chunk.constants.data());
            stack_top = base;
            return obj.raw_bits();
        }
    }

    // A class that declares no constructor previously got no inline cache at
    // all, so every single instantiation repeated a string copy plus four hash
    // lookups: the class table twice, then "생성자" and "init", both missing.
    // That made a plain data class twice as expensive to instantiate as one
    // that declares a constructor - 298 ns against 145 ns measured. Caching
    // the miss is what the ctor IC above already does for the hit.
    //
    // The encoding is free: a populated ctor IC always sets ic_method, and the
    // closure IC always sets ic_native_fn with ic_cache >= 0, so "ic_class set,
    // ic_method and ic_native_fn null" is unused and unambiguous.
    if (ins->ic_class != nullptr && ins->ic_method == nullptr &&
        ins->ic_native_fn == nullptr && !R[ins->b].is_closure()) {
        return make_initialized_instance(ins->ic_class, ins->line).raw_bits();
    }

    Value fn_val = R[ins->b];
    std::string name = (ins->str_idx >= 0) ? chunk.get_string(ins->str_idx) : "";

    // 1) struct / class constructor: Name(args) → NEW_INSTANCE
    if (!fn_val.is_closure() && !name.empty() && rt_classes.count(name)) {
        JitClassInfo* cls_ptr = &rt_classes.at(name);
        auto ctor = find_method(name, "\uc0dd\uc131\uc790");
        if (!ctor) ctor = find_method(name, "init");
        if (ctor) {
            if (const std::vector<int>* plain_fields = plain_ctor_fields(cls_ptr, ctor)) {
                ins->ic_class = cls_ptr;
                ins->ic_method = ctor;
                ins->ic_native_fn = nullptr;
                ins->ic_native_frame_regs = 0;
                return make_plain_ctor_instance(cls_ptr, ctor, *plain_fields, R, ins).raw_bits();
            }

            Value obj = make_initialized_instance(cls_ptr, ins->line);
            size_t cnt = method_frame_regs(chunk, *ctor);
            size_t base = alloc_frame_regs(cnt, ins->line);
            Value* NR = &value_stack[base];
            bind_method_args(chunk, *ctor, NR, obj, &R[ins->c],
                             static_cast<size_t>(std::max(0, ins->operand)));

            // ── Phase 5: native ctor invocation (eager JIT compile) ──
            // The auto-generated ctor for `struct` types is just MOVE+DOT_SET
            // — fully JIT-compileable. Compile it lazily and call native_fn
            // directly to skip the interpreter dispatch loop entirely.
            if (native_allowed()) {
                auto nit = native_methods.find(ctor);
                if (nit == native_methods.end() && !jit_method_failed.count(ctor)) {
                    if (++method_warm_count[ctor] >= METHOD_LAZY_JIT_THRESHOLD) {
                        try {
                            NativeCompiler nc(chunk, *ctor);
                            auto compiled = nc.compile();
                            if (compiled) nit = native_methods.emplace(ctor, std::move(compiled)).first;
                            else jit_method_failed.insert(ctor);
                        } catch (...) { jit_method_failed.insert(ctor); }
                    }
                }
                if (nit != native_methods.end()) {
                    // ── Phase 6: populate IC for monomorphic fast path next call ──
                    ins->ic_class     = cls_ptr;
                    ins->ic_method    = ctor;
                    ins->ic_native_fn = (void*)nit->second->fn;
                    ins->ic_native_frame_regs = nit->second->frame_regs;
                    nit->second->fn(this, NR, chunk.constants.data());
                    stack_top = base;
                    return obj.raw_bits();
                }
            }

            CallFrame cf;
            cf.reg_base = base; cf.reg_count = cnt; cf.chunk = &chunk;
            cf.closure = nullptr; cf.method = ctor;
            cf.ip = ctor->entry_ip; cf.end_ip = ctor->end_ip;
            cf.ret_reg = (uint16_t)-1; cf.in_try = false;
            execute_frame(cf);
            return obj.raw_bits();
        }
        // No constructor: remember the class so the next instantiation takes
        // the fast path above instead of missing four lookups again.
        ins->ic_class = cls_ptr;
        ins->ic_method = nullptr;
        ins->ic_native_fn = nullptr;
        ins->ic_native_frame_regs = 0;
        return make_initialized_instance(cls_ptr, ins->line).raw_bits();
    }

    // 2) stdlib
    if (!fn_val.is_closure() && !name.empty()) {
        Value out;
        if (SuraStd::try_dispatch(name, &R[ins->c], ins->operand, ins->line, out))
            return out.raw_bits();
    }

    // 3) exit
    if (!fn_val.is_closure() && name == "exit") std::exit(0);

    // 4) closure
    if (fn_val.is_closure()) {
        GCClosure* cl = fn_val.as_closure();
        if (cl->func_idx < 0 || (size_t)cl->func_idx >= chunk.func_table.size())
            throw JitThrow{"JIT: invalid func_idx", ins->line};
        const JitFuncInfo& fi = chunk.func_table[cl->func_idx];
        if ((size_t)ins->operand > fi.params.size())
            throw JitThrow{"[E300] 잘못된 인자 개수", ins->line};
        size_t cnt = function_frame_regs(chunk, fi);
        size_t base = alloc_frame_regs(cnt, ins->line);
        Value* NR = &value_stack[base];
        bind_function_args(chunk, fi, NR, &R[ins->c],
                           static_cast<size_t>(std::max(0, ins->operand)));

        // ── Phase 10: lazy JIT compile when called from JIT'd code ──
        // When main is JIT-compiled, the interpreter's CALL_FUNC lazy-compile
        // path is bypassed for its callees. Mirror that path here so functions
        // called only from JIT'd main (e.g. `step` in bench_physics) still
        // get native-compiled after the callee ICs have warmed.
        NativeFunc* callee_native = nullptr;
        if (jit_enabled) {
            callee_native = resolve_native(chunk, fi, cl->func_idx);
        }

        // Recursive JIT-in-JIT: if callee has native code, invoke it directly.
        if (callee_native) {
            // ── Phase 7: populate closure IC for monomorphic fast path ──
            // ic_method must remain nullptr to disambiguate from ctor IC.
            ins->ic_cache     = cl->func_idx;
            ins->ic_native_fn = (void*)callee_native->fn;
            ins->ic_native_frame_regs = callee_native->frame_regs;
            ins->ic_method    = nullptr;
            uint64_t bits = callee_native->fn(this, NR, chunk.constants.data());
            if (__builtin_expect(bits == SURA_JIT_DEOPT_SENTINEL, 0)) {
                // A guard rejected this call: undo the IC we just set, rebind
                // the arguments (a mid-body guard may have overwritten frame
                // registers), and fall through to the interpreter.
                note_native_deopt(cl->func_idx);
                ins->ic_native_fn = nullptr;
                bind_function_args(chunk, fi, NR, &R[ins->c],
                                   static_cast<size_t>(std::max(0, ins->operand)));
            } else {
                stack_top = base;
                return bits;
            }
        }
        // Otherwise fall back to the interpreter.
        CallFrame cf;
        cf.reg_base = base; cf.reg_count = cnt; cf.closure = cl; cf.chunk = &chunk;
        cf.method = nullptr;
        cf.ip = fi.entry_ip; cf.end_ip = fi.end_ip;
        cf.ret_reg = (uint16_t)-1; cf.in_try = false;
        Value result = execute_frame(cf);
        return result.raw_bits();
    }

    throw JitThrow{format_undefined_function(name), ins->line};
}

inline uint64_t JitVM::construct_plain2_from_jit(const JitClassInfo* cls, uint64_t v0, uint64_t v1) {
    Value obj = make_layout_instance(cls);
    GCInstance* idata = obj.as_inst();
    if (idata->fields.size() < 2) idata->fields.resize(2, Value::nil());
    idata->fields[0] = Value::from_bits(v0);
    idata->fields[1] = Value::from_bits(v1);
    return obj.raw_bits();
}

inline uint64_t JitVM::construct_plain3_from_jit(const JitClassInfo* cls, uint64_t v0, uint64_t v1, uint64_t v2) {
    Value obj = make_layout_instance(cls);
    GCInstance* idata = obj.as_inst();
    if (idata->fields.size() < 3) idata->fields.resize(3, Value::nil());
    idata->fields[0] = Value::from_bits(v0);
    idata->fields[1] = Value::from_bits(v1);
    idata->fields[2] = Value::from_bits(v2);
    return obj.raw_bits();
}

// Method call dispatch for JIT. Mirrors VM's METHOD_CALL opcode but runs
// synchronously (instance methods via nested execute_frame).
// Caller semantics: receiver at R[ins->b], args at R[ins->b+1..+nargs].
inline uint64_t JitVM::dispatch_method_call_from_jit(Value* R, const JitInst* ins) {
    if (!active_chunk) return Value::nil().raw_bits();
    run_gc_for_tensor_pressure();
    const JitChunk& chunk = *active_chunk;
    const std::string& meth = chunk.get_string(ins->str_idx);
    int nargs = ins->operand;
    uint16_t b = ins->b;

    Value builtin_method_result;
    if (dispatch_builtin_method(R[b], meth, &R[b+1], nargs, ins->line, builtin_method_result)) {
        return builtin_method_result.raw_bits();
    }

    // ── Built-in array methods ─────────────────────────────
    if (R[b].is_arr()) {
        GCArray* arr = R[b].as_arr();
        if (meth == "push" || meth == "append") {
            if (nargs >= 1) arr->elements.push_back(R[b+1]);
            return Value::nil().raw_bits();
        }
        if (meth == "pop") {
            if (arr->elements.empty()) return Value::nil().raw_bits();
            Value back = arr->elements.back();
            arr->elements.pop_back();
            return back.raw_bits();
        }
        if (meth == "len" || meth == "size" || meth == "length")
            return Value((double)arr->elements.size()).raw_bits();
        if (meth == "remove") {
            int idx = nargs >= 1 ? (int)R[b+1].to_num() : -1;
            if (idx < 0) idx += (int)arr->elements.size();
            if (idx >= 0 && idx < (int)arr->elements.size()) {
                Value v = arr->elements[idx];
                arr->elements.erase(arr->elements.begin() + idx);
                return v.raw_bits();
            }
            return Value::nil().raw_bits();
        }
        if (meth == "contains" || meth == "has") {
            bool found = false;
            if (nargs >= 1) for (auto& el : arr->elements) if (el.eq(R[b+1])) { found = true; break; }
            return Value(found).raw_bits();
        }
        if (meth == "sort") {
            std::sort(arr->elements.begin(), arr->elements.end(),
                [](const Value& x, const Value& y) { return x.lt(y); });
            return Value::nil().raw_bits();
        }
        if (meth == "reverse") {
            std::reverse(arr->elements.begin(), arr->elements.end());
            return Value::nil().raw_bits();
        }
        if (meth == "clear") { arr->elements.clear(); return Value::nil().raw_bits(); }
        return Value::nil().raw_bits();
    }

    // ── Built-in string methods ────────────────────────────
    if (R[b].is_str()) {
        std::string s = R[b].as_str();
        if (meth == "len" || meth == "size" || meth == "length")
            return Value((double)s.size()).raw_bits();
        if (meth == "upper") {
            std::string u = s;
            for (char& ch : u) ch = (char)toupper((unsigned char)ch);
            return Value(u).raw_bits();
        }
        if (meth == "lower") {
            std::string l = s;
            for (char& ch : l) ch = (char)tolower((unsigned char)ch);
            return Value(l).raw_bits();
        }
        if (meth == "trim") {
            size_t st = s.find_first_not_of(" \t\n\r");
            size_t en = s.find_last_not_of(" \t\n\r");
            return (st == std::string::npos) ? Value(std::string("")).raw_bits()
                                             : Value(s.substr(st, en - st + 1)).raw_bits();
        }
        if (meth == "contains" || meth == "has") {
            std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
            return Value(s.find(needle) != std::string::npos).raw_bits();
        }
        if (meth == "startsWith" || meth == "starts_with" || meth == "startswith") {
            std::string pre = nargs >= 1 ? R[b+1].to_str() : "";
            return Value(s.size() >= pre.size() && s.substr(0, pre.size()) == pre).raw_bits();
        }
        if (meth == "endsWith" || meth == "ends_with" || meth == "endswith") {
            std::string suf = nargs >= 1 ? R[b+1].to_str() : "";
            return Value(s.size() >= suf.size() &&
                         s.substr(s.size() - suf.size()) == suf).raw_bits();
        }
        if (meth == "indexOf" || meth == "index_of" || meth == "index" || meth == "find") {
            std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
            size_t p = s.find(needle);
            return (p == std::string::npos ? Value(-1.0) : Value((double)p)).raw_bits();
        }
        if (meth == "sub" || meth == "substr" || meth == "substring" || meth == "slice") {
            int start = nargs >= 1 ? (int)R[b+1].to_num() : 0;
            int end = nargs >= 2 ? (int)R[b+2].to_num() : (int)s.size();
            if (meth == "slice" && start < 0) start = std::max(0, (int)s.size() + start);
            if (meth == "slice" && end < 0) end = std::max(0, (int)s.size() + end);
            if (start < 0) start = 0;
            if (end > (int)s.size()) end = (int)s.size();
            if (end < start) end = start;
            return Value(s.substr(start, end - start)).raw_bits();
        }
        return Value::nil().raw_bits();
    }

    // ── Built-in dict methods ──────────────────────────────
    if (R[b].is_dict()) {
        GCDict* dict = R[b].as_dict();
        if (meth == "len" || meth == "size" || meth == "length")
            return Value((double)dict->elements.size()).raw_bits();
        if (meth == "has" || meth == "contains") {
            std::string key = nargs >= 1 ? R[b+1].to_str() : "";
            return Value(dict->elements.count(key) > 0).raw_bits();
        }
        if (meth == "keys") {
            Value arr = Value::make_array();
            for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(Value(k));
            return arr.raw_bits();
        }
        if (meth == "values") {
            Value arr = Value::make_array();
            for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(v);
            return arr.raw_bits();
        }
        if (meth == "remove" || meth == "delete") {
            if (nargs >= 1) dict->elements.erase(R[b+1].to_str());
            return Value::nil().raw_bits();
        }
        if (meth == "clear") { dict->elements.clear(); return Value::nil().raw_bits(); }
        Value callable_out;
        if (dispatch_dict_callable_field(R[b], meth, &R[b+1], nargs, ins->line, callable_out)) {
            return callable_out.raw_bits();
        }
        return Value::nil().raw_bits();
    }

    // ── Instance method: find in class, JIT if possible, synchronous execute ──
    if (R[b].is_inst()) {
        // ── Monomorphic IC fast path: skip hash lookups entirely ──────────
        if (ins->ic_native_fn && ins->ic_class &&
            R[b].as_inst()->jit_info == ins->ic_class) {
            SuraNativeFn fast_fn = reinterpret_cast<SuraNativeFn>(ins->ic_native_fn);
            const JitMethodInfo* fmi = ins->ic_method;
            size_t fcnt = method_frame_regs(chunk, *fmi);
            size_t fbase = alloc_frame_regs(fcnt, ins->line);
            Value* FNR = &value_stack[fbase];
            bind_method_args(chunk, *fmi, FNR, R[b], &R[b + 1],
                             static_cast<size_t>(std::max(0, nargs)));
            uint64_t bits = fast_fn(this, FNR, chunk.constants.data());
            stack_top = fbase;
            return bits;
        }

        auto mi = find_method(R[b].as_inst()->type_name(), meth);
        if (!mi) {
            Value callable_out;
            if (dispatch_instance_callable_field(R[b], meth, &R[b+1], nargs, ins->line, callable_out)) {
                return callable_out.raw_bits();
            }
            return Value::nil().raw_bits();
        }
        size_t cnt = method_frame_regs(chunk, *mi);
        size_t base = alloc_frame_regs(cnt, ins->line);
        Value* NR = &value_stack[base];
        bind_method_args(chunk, *mi, NR, R[b], &R[b + 1],
                         static_cast<size_t>(std::max(0, nargs)));

        // JIT-in-JIT: compile method lazily so ic_cache is warm at compile time.
        if (native_allowed()) {
            auto nit = native_methods.find(mi);
            if (nit == native_methods.end() && !jit_method_failed.count(mi)) {
                if (++method_warm_count[mi] >= METHOD_LAZY_JIT_THRESHOLD) {
                    try {
                        NativeCompiler nc(chunk, *mi);
                        auto compiled = nc.compile();
                        if (compiled) nit = native_methods.emplace(mi, std::move(compiled)).first;
                        else jit_method_failed.insert(mi);
                    } catch (...) { jit_method_failed.insert(mi); }
                }
            }
            if (nit != native_methods.end()) {
                // Populate IC cache for monomorphic fast path next call
                ins->ic_method    = mi;
                ins->ic_class     = R[b].as_inst()->jit_info;
                ins->ic_native_fn = (void*)nit->second->fn;
                ins->ic_native_frame_regs = nit->second->frame_regs;
                uint64_t bits = nit->second->fn(this, NR, chunk.constants.data());
                stack_top = base;
                return bits;
            }
        }
        CallFrame cf;
        cf.reg_base = base; cf.reg_count = cnt; cf.chunk = &chunk;
        cf.closure = nullptr; cf.method = mi;
        cf.ip = mi->entry_ip; cf.end_ip = mi->end_ip;
        cf.ret_reg = (uint16_t)-1; cf.in_try = false;
        Value result = execute_frame(cf);
        return result.raw_bits();
    }

    return Value::nil().raw_bits();
}

// DOT_GET: reads obj.prop into a Value.
// Semantics: R[ins->b] is the object, ins->str_idx names the property.
inline uint64_t JitVM::dispatch_dot_get_from_jit(Value* R, const JitInst* ins) {
    if (!active_chunk) return Value::nil().raw_bits();
    const JitChunk& chunk = *active_chunk;
    const std::string& prop = chunk.get_string(ins->str_idx);
    const Value& recv = R[ins->b];
    if (recv.is_inst()) {
        GCInstance* obj = recv.as_inst();
        const std::string& cname = obj->type_name();
        if (rt_classes.count(cname)) {
            auto& cl = rt_classes[cname];
            auto it = cl.field_indices.find(prop);
            if (it != cl.field_indices.end()) {
                int offset = it->second;
                if (obj->fields.size() < cl.field_defaults.size())
                    obj->fields.resize(cl.field_defaults.size(), Value::nil());
                if (offset >= 0 && (size_t)offset < obj->fields.size())
                    return obj->fields[offset].raw_bits();
            }
        }
        return Value::nil().raw_bits();
    }
    if (recv.is_dict()) return recv.dict_get(prop).raw_bits();
    if (recv.is_nil()) throw JitThrow{"[E201] nil 역참조", ins->line};
    return Value::nil().raw_bits();
}

// DOT_SET: writes R[ins->b] into R[ins->a].prop.
// Note: DOT_SET semantics are reversed — the object is at R[a], value at R[b].
inline void JitVM::dispatch_dot_set_from_jit(Value* R, const JitInst* ins) {
    if (!active_chunk) return;
    const JitChunk& chunk = *active_chunk;
    const std::string& prop = chunk.get_string(ins->str_idx);
    Value& recv = R[ins->a];
    if (recv.is_inst()) {
        GCInstance* obj = recv.as_inst();
        const std::string& cname = obj->type_name();
        auto class_it = rt_classes.find(cname);
        if (class_it != rt_classes.end()) {
            JitClassInfo& cl = class_it->second;
            auto it = cl.field_indices.find(prop);
            int offset;
            if (it != cl.field_indices.end()) {
                offset = it->second;
            } else {
                // Previously this branch did not exist, so assigning a field the
                // class had never seen silently did nothing whenever the code
                // ran as native rather than interpreted. Widen the layout here
                // exactly as the interpreter does.
                offset = (int)cl.field_indices.size();
                cl.field_indices[prop] = offset;
                cl.field_defaults.push_back(Value::nil());
            }
            if (obj->fields.size() < cl.field_defaults.size())
                obj->fields.resize(cl.field_defaults.size(), Value::nil());
            if (offset >= 0 && (size_t)offset < obj->fields.size())
                obj->fields[offset] = R[ins->b];
        }
    } else if (recv.is_dict()) {
        recv.dict_set(prop, R[ins->b]);
    }
}

// ── Phase 10: top-level JIT helpers ───────────────────────────────
// Mirror the interpreter's MAKE_LAMBDA / DEF_CLASS / PRINT cases but
// without dependence on a CallFrame (top-level main has fp->closure = nullptr,
// so non-local upvalues do not occur here in well-formed programs).
inline uint64_t JitVM::jit_make_lambda(Value* R, const JitInst* ins) {
    if (!active_chunk) return Value::nil().raw_bits();
    const JitChunk& chunk = *active_chunk;
    if (ins->operand < 0 || (size_t)ins->operand >= chunk.func_table.size())
        return Value::nil().raw_bits();
    const auto& fi = chunk.func_table[ins->operand];
    std::string fname = fi.name.empty() ? "<lambda>" : fi.name;
    GCClosure* clos = GC::allocate<GCClosure>(fname);
    clos->func_idx = ins->operand;
    // Top-level: only "is_local" upvalues capture from R; non-local ones are
    // unreachable here (no enclosing closure). Skip them safely.
    for (const auto& up : fi.upvalues) {
        if (up.is_local) {
            clos->upvalues.push_back(capture_upvalue(&R[up.index]));
        }
    }
    return Value((GCObject*)clos).raw_bits();
}

// Mirrors the USE_LIB case in execute_frame exactly, including the order the
// module value is built and bound. USE_LIB has a side effect - it binds a
// stdlib module into a global - so native and interpreted runs have to perform
// the same binding, or a program's observable state depends on whether the top
// level happened to compile.
inline void JitVM::jit_use_lib(const JitInst* ins) {
    if (!active_chunk) return;
    const JitChunk& chunk = *active_chunk;
    std::string lib = chunk.get_string(ins->str_idx);
    if (!is_stdlib_module(lib)) return;
    Value mod = make_stdlib_module(lib);
    for (size_t gi = 0; gi < chunk.global_names.size(); ++gi) {
        if (chunk.global_names[gi] == lib) {
            if (globals.size() <= gi) globals.resize(gi + 1, Value::nil());
            if (global_initialized.size() <= gi) global_initialized.resize(gi + 1, false);
            globals[gi] = mod;
            global_initialized[gi] = true;
            break;
        }
    }
}

// Mirrors the INDEX_GET case in execute_frame. Indexing is polymorphic across
// array, dict, string and nil and raises on two of those paths, so it takes a
// guarded helper rather than inline code - the same shape DIV and MOD use.
// Every branch and error string below has to match the interpreter's, since the
// differential lane compares their output byte for byte.
inline uint64_t JitVM::jit_index_get(Value* R, const JitInst* ins) {
    const Value& target = R[ins->b];
    const Value& key    = R[ins->c];
    if (target.is_arr()) {
        int idx = (int)key.to_num();
        auto* arr = target.as_arr();
        if (idx < 0) idx += (int)arr->elements.size();
        if (idx >= 0 && idx < (int)arr->elements.size())
            return arr->elements[idx].raw_bits();
        throw JitThrow{"[E202] 배열 범위 초과", ins->line};
    }
    if (target.is_dict()) return target.dict_get(key.to_str()).raw_bits();
    if (target.is_str()) {
        int i = (int)key.to_num();
        const std::string& s = target.as_str_ref();
        if (i < 0) i += (int)s.size();
        return ((i >= 0 && i < (int)s.size()) ? Value(std::string(1, s[i]))
                                              : Value::nil()).raw_bits();
    }
    if (target.is_nil()) throw JitThrow{"[E201] nil 역참조", ins->line};
    return Value::nil().raw_bits();
}

// Mirrors the INDEX_SET case in execute_frame. Note the register roles differ
// from INDEX_GET: here `a` is the container being written to, not a
// destination, and there is no result. Anything that is neither array nor dict
// is silently ignored, which is the interpreter's behaviour and so has to be
// this helper's too.
inline void JitVM::jit_index_set(Value* R, const JitInst* ins) {
    Value& target = R[ins->a];
    if      (target.is_arr())  target.arr_set((int)R[ins->b].to_num(), R[ins->c]);
    else if (target.is_dict()) target.dict_set(R[ins->b].to_str(), R[ins->c]);
}

// Mirrors the NEW_INSTANCE case in execute_frame.
//
// Unlike TRY_BEGIN, this opcode's control transfer is a *call*: the constructor
// runs and comes back, so it can be expressed as a helper that drives the
// constructor to completion with execute_frame() and returns - the same thing
// dispatch_method_call_from_jit already does for user-defined methods. There is
// no non-local jump to reconstruct.
//
// Two orderings from the interpreter have to be preserved exactly. The instance
// is written into its destination register *before* the constructor runs, which
// is what makes the constructor's return value discarded rather than
// overwriting it. And the frame registers are released afterwards, as
// dispatch_method_call_from_jit does.
inline void JitVM::jit_new_instance(Value* R, const JitInst* ins) {
    if (!active_chunk) return;
    const JitChunk& chunk = *active_chunk;
    const std::string cls = chunk.get_string(ins->str_idx);

    auto class_found = rt_classes.find(cls);
    Value inst_val = class_found == rt_classes.end()
        ? Value::make_inst(cls)
        : make_initialized_instance(&class_found->second, ins->line);
    R[ins->a] = inst_val;

    const JitMethodInfo* ctor = find_method(cls, "생성자");
    if (!ctor) ctor = find_method(cls, "init");
    if (!ctor) return;

    size_t ctor_count = method_frame_regs(chunk, *ctor);
    size_t ctor_base  = alloc_frame_regs(ctor_count, ins->line);
    Value* CR = &value_stack[ctor_base];
    bind_method_args(chunk, *ctor, CR, inst_val, &R[ins->b],
                     static_cast<size_t>(std::max(0, ins->operand)));

    CallFrame cf;
    cf.reg_base  = ctor_base;
    cf.reg_count = ctor_count;
    cf.chunk     = &chunk;
    cf.closure   = nullptr;
    cf.method    = ctor;
    cf.ip        = ctor->entry_ip;
    cf.end_ip    = ctor->end_ip;
    cf.ret_reg   = (uint16_t)-1;
    cf.in_try    = false;
    execute_frame(cf);   // return value intentionally discarded
    stack_top = ctor_base;
}

// Mirrors the OP_IN case in execute_frame. A pure value computation over
// array / dict / string, with nil for anything else.
inline void JitVM::jit_op_in(Value* R, const JitInst* ins) {
    const Value& needle = R[ins->b];
    const Value& hay    = R[ins->c];
    if (hay.is_arr()) {
        bool found = false;
        for (auto& el : hay.as_arr()->elements) if (needle.eq(el)) { found = true; break; }
        R[ins->a] = Value(found);
    } else if (hay.is_dict()) {
        R[ins->a] = Value(hay.dict_has(needle.to_str()));
    } else if (hay.is_str()) {
        R[ins->a] = Value(hay.as_str_ref().find(needle.to_str()) != std::string::npos);
    } else {
        R[ins->a] = Value::nil();
    }
}

// Mirrors the DICT_KEYS case in execute_frame. Builds the iteration keys for a
// collection: a dict yields its keys, an array or string yields its indices,
// and anything else yields an empty array rather than nil.
inline void JitVM::jit_dict_keys(Value* R, const JitInst* ins) {
    Value keys = Value::make_array();
    const Value& coll = R[ins->b];
    if (coll.is_dict()) {
        for (auto& [k, v] : coll.as_dict()->elements)
            keys.as_arr()->elements.push_back(Value(k));
    } else if (coll.is_arr()) {
        int n = (int)coll.as_arr()->elements.size();
        for (int i = 0; i < n; ++i) keys.as_arr()->elements.push_back(Value((double)i));
    } else if (coll.is_str()) {
        int n = (int)coll.as_str_ref().size();
        for (int i = 0; i < n; ++i) keys.as_arr()->elements.push_back(Value((double)i));
    }
    R[ins->a] = keys;
}

// Mirrors the FOREACH_NEXT case in execute_frame, minus the jump.
//
// The interpreter ends the loop by assigning `lip = operand`. That is a jump
// within the same body, which the emitter already knows how to express, so the
// split is: this helper advances the iterator and reports whether there was an
// element, and the generated code does the branching. Returns 1 to continue
// (value and index registers updated) and 0 to exit the loop.
inline int JitVM::jit_foreach_next(Value* R, const JitInst* ins) {
    const Value& coll = R[ins->c];
    if (coll.is_arr()) {
        int idx = (int)R[ins->b].to_num();
        auto* arr = coll.as_arr();
        if (idx >= (int)arr->elements.size()) return 0;
        R[ins->a] = arr->elements[idx];
        R[ins->b] = Value((double)(idx + 1));
        return 1;
    }
    if (coll.is_str()) {
        int idx = (int)R[ins->b].to_num();
        const std::string& s = coll.as_str();
        if (idx >= (int)s.size()) return 0;
        R[ins->a] = Value(std::string(1, s[idx]));
        R[ins->b] = Value((double)(idx + 1));
        return 1;
    }
    return 0;
}

inline void JitVM::jit_def_class(Value* R, const JitInst* ins) {
    if (!active_chunk) return;
    const JitChunk& chunk = *active_chunk;
    if (ins->operand < 0 || (size_t)ins->operand >= chunk.class_table.size()) return;
    JitClassInfo ci = chunk.class_table[ins->operand];
    if (ins->c == JIT_CLASS_DEFAULTS_MARKER) {
        if (static_cast<size_t>(ins->b) != ci.field_defaults.size())
            throw JitThrow{"invalid class field default metadata", ins->line};
        for (size_t i = 0; i < ci.field_defaults.size(); ++i)
            ci.field_defaults[i] = R[static_cast<size_t>(ins->a) + i];
    }
    if (!ci.parent.empty() && rt_classes.count(ci.parent)) {
        JitClassInfo& p = rt_classes[ci.parent];
        std::vector<Value> new_defs = p.field_defaults;
        std::unordered_map<std::string, int> new_idx = p.field_indices;
        int offset = (int)p.field_defaults.size();
        for (const auto& kv : ci.field_indices) {
            new_idx[kv.first] = kv.second + offset;
        }
        for (const auto& v : ci.field_defaults) new_defs.push_back(v);
        for (const auto& m : p.methods) {
            if (m.first != JIT_FIELD_INITIALIZER_METHOD && !ci.methods.count(m.first))
                ci.methods[m.first] = m.second;
        }
        ci.field_defaults = new_defs;
        ci.field_indices  = new_idx;
    }
    if (ins->str_idx >= 0 && (size_t)ins->str_idx < chunk.global_names.size()) {
        rt_classes[chunk.global_names[ins->str_idx]] = ci;
        if ((size_t)ins->str_idx >= global_initialized.size())
            global_initialized.resize(ins->str_idx + 1, false);
        global_initialized[ins->str_idx] = true;
    }
}

inline void JitVM::jit_print(Value* R, const JitInst* ins, int newline) {
    std::string out;
    int n = ins->operand;
    for (int i = 0; i < n; ++i) out += R[ins->a + i].to_str();
    if (newline) std::cout << out << "\n";
    else         std::cout << out;
}

// ── C-linkage trampolines bakeable into emitted native code ───────
extern "C" inline uint64_t sura_jit_call(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->dispatch_call_from_jit(R, ins);
}
extern "C" inline uint64_t sura_jit_construct_plain2(JitVM* vm, const JitClassInfo* cls, uint64_t v0, uint64_t v1) {
    return vm->construct_plain2_from_jit(cls, v0, v1);
}
extern "C" inline uint64_t sura_jit_construct_plain3(JitVM* vm, const JitClassInfo* cls, uint64_t v0, uint64_t v1, uint64_t v2) {
    return vm->construct_plain3_from_jit(cls, v0, v1, v2);
}
extern "C" inline uint64_t sura_jit_materialize_scalar_record(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->jit_materialize_scalar_record(R, ins);
}

extern "C" inline int sura_jit_strict_vector_loop(
    JitVM* vm, Value* /*R*/, const JitChunk* chunk,
    const JitStrictCountedLoop* spec) {
    return vm && chunk && spec ? vm->run_strict_vector_loop(*chunk, *spec) : 0;
}
extern "C" inline uint64_t sura_jit_method_call(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->dispatch_method_call_from_jit(R, ins);
}
extern "C" inline uint64_t sura_jit_dot_get(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->dispatch_dot_get_from_jit(R, ins);
}
extern "C" inline void sura_jit_dot_set(JitVM* vm, Value* R, const JitInst* ins) {
    vm->dispatch_dot_set_from_jit(R, ins);
}
extern "C" inline uint64_t sura_jit_make_array(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->make_array_from_jit(R, ins);
}
extern "C" inline uint64_t sura_jit_make_dict(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->make_dict_from_jit(R, ins);
}
extern "C" inline uint64_t sura_jit_load_global(JitVM* vm, int idx) {
    return vm->jit_load_global(idx);
}
extern "C" inline uint64_t sura_jit_load_global_inst(JitVM* vm, const JitInst* ins) {
    return vm->jit_load_global_inst(ins);
}
// Phase 10: helpers for top-level main JIT
extern "C" inline uint64_t sura_jit_make_lambda(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->jit_make_lambda(R, ins);
}
extern "C" inline void sura_jit_def_class(JitVM* vm, Value* R, const JitInst* ins) {
    vm->jit_def_class(R, ins);
}
extern "C" inline void sura_jit_use_lib(JitVM* vm, Value* R, const JitInst* ins) {
    (void)R;
    vm->jit_use_lib(ins);
}
extern "C" inline uint64_t sura_jit_index_get(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->jit_index_get(R, ins);
}
extern "C" inline void sura_jit_index_set(JitVM* vm, Value* R, const JitInst* ins) {
    vm->jit_index_set(R, ins);
}
extern "C" inline void sura_jit_new_instance(JitVM* vm, Value* R, const JitInst* ins) {
    vm->jit_new_instance(R, ins);
}
extern "C" inline void sura_jit_op_in(JitVM* vm, Value* R, const JitInst* ins) {
    vm->jit_op_in(R, ins);
}
extern "C" inline void sura_jit_dict_keys(JitVM* vm, Value* R, const JitInst* ins) {
    vm->jit_dict_keys(R, ins);
}
extern "C" inline int sura_jit_foreach_next(JitVM* vm, Value* R, const JitInst* ins) {
    return vm->jit_foreach_next(R, ins);
}
extern "C" inline void sura_jit_print(JitVM* vm, Value* R, const JitInst* ins, int newline) {
    vm->jit_print(R, ins, newline);
}
// Phase 10: safe ADD wrapper used only by JIT'd main (handles string concat).
// Inside JIT'd user functions we keep the fast unchecked addsd path.
extern "C" inline uint64_t sura_jit_add(uint64_t a, uint64_t b) {
    return (Value::from_bits(a) + Value::from_bits(b)).raw_bits();
}
extern "C" inline uint64_t sura_jit_eq(uint64_t a, uint64_t b, int neq) {
    Value va = Value::from_bits(a);
    Value vb = Value::from_bits(b);
    return Value(neq ? va.neq(vb) : va.eq(vb)).raw_bits();
}
extern "C" inline void sura_jit_store_global(JitVM* vm, int idx, uint64_t bits) {
    vm->jit_store_global(idx, bits);
}
// Returns 1 iff the Value with `bits` is truthy.
// Used by JIT'd JUMP_IF_FALSE/TRUE for values that may be non-bool
// (numbers, strings, arrays, dicts, closures, instances, nil).
extern "C" inline int sura_jit_truthy(uint64_t bits) {
    return Value::from_bits(bits).truthy() ? 1 : 0;
}


