#pragma once
#include "value.hpp"
#include "jit_throw.hpp"
#include "sura_plugin.h"
#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

// ================================================================
//  Sura Standard Library — built-in functions available as globals.
//
//  All functions share the signature:
//      Value fn(const Value* args, int nargs, int line)
//  and throw JitThrow on error (arg count/type/domain problems).
//
//  Dispatched from jit_vm.hpp::CALL_FUNC when the callee is not a
//  user-defined closure.  SuraStd::try_dispatch() returns true if the
//  name is known (and writes the result to `out`).
// ================================================================

namespace SuraStd {

using BuiltinFn = Value(*)(const Value*, int, int);

inline Value tool_schema_for(const std::string& name, int line);
inline size_t ag_max_external_bytes();
inline void ag_preflight_bytes(size_t bytes, const char* name, int line);
inline Value ag_clone_tensor_value(const char* name, const GCTensor* source,
                                   bool requires_grad, TensorDType dtype,
                                   int line);
inline void ag_cuda_materialize_host(GCTensor* tensor,
                                     const char* name, int line);

inline std::vector<std::string>& script_args_storage() {
    static std::vector<std::string> args;
    return args;
}

inline std::string& script_name_storage() {
    static std::string name;
    return name;
}

inline std::mutex& runtime_executable_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::string& runtime_executable_storage() {
    static std::string path;
    return path;
}

// The CLI records argv[0] before any Sura code runs. Embedders that do not
// launch through main() can set SURA_EXECUTABLE to enable async.sura().
inline void set_runtime_executable(const std::string& raw_path) {
    std::lock_guard<std::mutex> lock(runtime_executable_mutex());
    runtime_executable_storage() = raw_path;
}

inline std::string runtime_executable() {
    if (const char* override_path = std::getenv("SURA_EXECUTABLE")) {
        if (*override_path) return override_path;
    }
    std::lock_guard<std::mutex> lock(runtime_executable_mutex());
    return runtime_executable_storage();
}

inline std::atomic<bool>& async_isolated_child_storage() {
    static std::atomic<bool> isolated{false};
    return isolated;
}

inline void set_async_isolated_child(bool isolated) {
    async_isolated_child_storage().store(isolated, std::memory_order_release);
}

inline bool is_async_isolated_child() {
    return async_isolated_child_storage().load(std::memory_order_acquire);
}

inline void set_script_context(const std::string& script_name, const std::vector<std::string>& args) {
    script_name_storage() = script_name;
    script_args_storage() = args;
}

// ── Helpers ───────────────────────────────────────────────────────

inline void need_args(const char* name, int got, int min_args, int max_args, int line) {
    if (got < min_args || (max_args >= 0 && got > max_args)) {
        std::string msg = std::string(name) + "(): expected "
            + std::to_string(min_args)
            + (max_args == min_args ? "" : (max_args < 0 ? "+" : ".." + std::to_string(max_args)))
            + " arg(s), got " + std::to_string(got);
        throw JitThrow{msg, line};
    }
}
inline double need_num(const char* name, const Value& v, int idx, int line) {
    if (!v.is_num()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx+1)
                       + " must be a number, got " + v.to_str(), line};
    }
    return v.as_num();
}
inline const std::string need_str(const char* name, const Value& v, int idx, int line) {
    if (!v.is_str()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx+1)
                       + " must be a string, got " + v.to_str(), line};
    }
    return v.as_str();
}
#ifdef _WIN32
inline std::wstring windows_path_bytes_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  text.data(), (int)text.size(), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        code_page = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(code_page, flags, text.data(), (int)text.size(), nullptr, 0);
    }
    if (len <= 0) {
        std::wstring out;
        out.reserve(text.size());
        for (unsigned char ch : text) out.push_back((wchar_t)ch);
        return out;
    }
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), (int)text.size(), out.data(), len);
    return out;
}

inline std::string windows_wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                        out.data(), len, nullptr, nullptr);
    return out;
}
#endif

inline std::filesystem::path fs_path_from_utf8(const std::string& path) {
#ifdef _WIN32
    return std::filesystem::path(windows_path_bytes_to_wide(path));
#else
    return std::filesystem::path(path);
#endif
}
inline std::string fs_path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return windows_wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}
inline std::string fs_path_to_generic_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return windows_wide_to_utf8(path.generic_wstring());
#else
    return path.generic_string();
#endif
}
inline GCArray* need_arr(const char* name, const Value& v, int idx, int line) {
    if (!v.is_arr()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx+1)
                       + " must be an array, got " + v.to_str(), line};
    }
    return v.as_arr();
}
inline GCDict* need_dict(const char* name, const Value& v, int idx, int line) {
    if (!v.is_dict()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx+1)
                       + " must be a dict, got " + v.to_str(), line};
    }
    return v.as_dict();
}

inline std::mt19937_64& rng() {
    static std::mt19937_64 g{std::random_device{}()};
    return g;
}

// ── Math ─────────────────────────────────────────────────────────

inline Value b_sqrt (const Value* a, int n, int l) { need_args("sqrt",  n, 1, 1, l); return Value(std::sqrt(need_num("sqrt",  a[0], 0, l))); }
inline Value b_sin  (const Value* a, int n, int l) { need_args("sin",   n, 1, 1, l); return Value(std::sin (need_num("sin",   a[0], 0, l))); }
inline Value b_cos  (const Value* a, int n, int l) { need_args("cos",   n, 1, 1, l); return Value(std::cos (need_num("cos",   a[0], 0, l))); }
inline Value b_tan  (const Value* a, int n, int l) { need_args("tan",   n, 1, 1, l); return Value(std::tan (need_num("tan",   a[0], 0, l))); }
inline Value b_floor(const Value* a, int n, int l) { need_args("floor", n, 1, 1, l); return Value(std::floor(need_num("floor",a[0], 0, l))); }
inline Value b_ceil (const Value* a, int n, int l) { need_args("ceil",  n, 1, 1, l); return Value(std::ceil(need_num("ceil", a[0], 0, l))); }
inline Value b_round(const Value* a, int n, int l) { need_args("round", n, 1, 1, l); return Value(std::round(need_num("round", a[0], 0, l))); }
inline Value b_abs  (const Value* a, int n, int l) { need_args("abs",   n, 1, 1, l); return Value(std::fabs(need_num("abs",  a[0], 0, l))); }
inline Value b_sign (const Value* a, int n, int l) { need_args("sign",  n, 1, 1, l); double v = need_num("sign", a[0], 0, l); return Value(v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0)); }
inline Value b_pow  (const Value* a, int n, int l) { need_args("pow",   n, 2, 2, l);
    return Value(std::pow(need_num("pow", a[0], 0, l), need_num("pow", a[1], 1, l)));
}
inline Value b_random(const Value* a, int n, int l) {
    need_args("random", n, 0, 2, l);
    if (n == 0) {
        std::uniform_real_distribution<double> d(0.0, 1.0);
        return Value(d(rng()));
    }
    if (n == 1) {
        double hi_raw = need_num("random", a[0], 0, l);
        if (!std::isfinite(hi_raw)) throw JitThrow{"random(): max must be finite", l};
        long long hi = (long long)hi_raw;
        if (hi <= 0) throw JitThrow{"random(): max must be positive", l};
        std::uniform_int_distribution<long long> d(0, hi - 1);
        return Value((double)d(rng()));
    }
    double lo_raw = need_num("random", a[0], 0, l);
    double hi_raw = need_num("random", a[1], 1, l);
    if (!std::isfinite(lo_raw) || !std::isfinite(hi_raw)) {
        throw JitThrow{"random(): min and max must be finite", l};
    }
    long long lo = (long long)lo_raw;
    long long hi = (long long)hi_raw;
    if (hi < lo) throw JitThrow{"random(): max must be greater than or equal to min", l};
    std::uniform_int_distribution<long long> d(lo, hi);
    return Value((double)d(rng()));
}

inline long long random_integral_arg(const char* name, const Value& v, int idx, int line) {
    double raw = need_num(name, v, idx, line);
    if (!std::isfinite(raw) || std::floor(raw) != raw) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1)
                       + " must be an integer", line};
    }
    return (long long)raw;
}

inline Value b_random_seed(const Value* a, int n, int l) {
    need_args("random_seed", n, 1, 1, l);
    long long seed = random_integral_arg("random_seed", a[0], 0, l);
    if (seed < 0) throw JitThrow{"random_seed(): seed must be non-negative", l};
    rng().seed((uint64_t)seed);
    return Value((double)seed);
}

inline Value b_random_int(const Value* a, int n, int l) {
    need_args("random_int", n, 1, 2, l);
    long long lo = 0;
    long long hi = random_integral_arg("random_int", a[0], 0, l);
    if (n == 1) {
        if (hi <= 0) throw JitThrow{"random_int(): max must be positive", l};
        --hi;
    } else {
        lo = hi;
        hi = random_integral_arg("random_int", a[1], 1, l);
        if (hi < lo) throw JitThrow{"random_int(): max must be greater than or equal to min", l};
    }
    std::uniform_int_distribution<long long> d(lo, hi);
    return Value((double)d(rng()));
}

inline Value b_random_float(const Value* a, int n, int l) {
    need_args("random_float", n, 0, 2, l);
    double lo = 0.0;
    double hi = 1.0;
    if (n == 1) {
        hi = need_num("random_float", a[0], 0, l);
    } else if (n == 2) {
        lo = need_num("random_float", a[0], 0, l);
        hi = need_num("random_float", a[1], 1, l);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo) {
        throw JitThrow{"random_float(): max must be greater than or equal to min", l};
    }
    std::uniform_real_distribution<double> d(lo, hi);
    return Value(d(rng()));
}

inline Value b_random_bool(const Value* a, int n, int l) {
    need_args("random_bool", n, 0, 1, l);
    double probability = n == 1 ? need_num("random_bool", a[0], 0, l) : 0.5;
    if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
        throw JitThrow{"random_bool(): probability must be between 0 and 1", l};
    }
    std::bernoulli_distribution d(probability);
    return Value(d(rng()));
}

inline Value b_random_choice(const Value* a, int n, int l) {
    need_args("random_choice", n, 1, 1, l);
    GCArray* arr = need_arr("random_choice", a[0], 0, l);
    if (arr->elements.empty()) throw JitThrow{"random_choice(): array must not be empty", l};
    std::uniform_int_distribution<size_t> d(0, arr->elements.size() - 1);
    return arr->elements[d(rng())];
}

inline Value b_random_shuffle(const Value* a, int n, int l) {
    need_args("random_shuffle", n, 1, 1, l);
    GCArray* arr = need_arr("random_shuffle", a[0], 0, l);
    Value out = Value::make_array();
    out.as_arr()->elements = arr->elements;
    std::shuffle(out.as_arr()->elements.begin(), out.as_arr()->elements.end(), rng());
    return out;
}

inline Value b_random_bytes(const Value* a, int n, int l) {
    need_args("random_bytes", n, 1, 1, l);
    long long count = random_integral_arg("random_bytes", a[0], 0, l);
    if (count < 0) throw JitThrow{"random_bytes(): count must be non-negative", l};
    if (count > 1048576) throw JitThrow{"random_bytes(): count exceeds 1048576", l};
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve((size_t)count);
    std::uniform_int_distribution<int> d(0, 255);
    for (long long i = 0; i < count; ++i) arr->elements.push_back(Value((double)d(rng())));
    return out;
}

inline Value b_uuid_v4(const Value*, int n, int l) {
    need_args("uuid_v4", n, 0, 0, l);
    std::uniform_int_distribution<int> d(0, 255);
    unsigned char bytes[16];
    for (unsigned char& byte : bytes) byte = (unsigned char)d(rng());
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << (int)bytes[i];
    }
    return Value(out.str());
}
inline Value b_clamp(const Value* a, int n, int l) {
    need_args("clamp", n, 3, 3, l);
    double x  = need_num("clamp", a[0], 0, l);
    double lo = need_num("clamp", a[1], 1, l);
    double hi = need_num("clamp", a[2], 2, l);
    return Value(x < lo ? lo : (x > hi ? hi : x));
}
inline Value b_min(const Value* a, int n, int l) {
    need_args("min", n, 1, -1, l);
    double best = need_num("min", a[0], 0, l);
    for (int i = 1; i < n; ++i) {
        double v = need_num("min", a[i], i, l);
        if (v < best) best = v;
    }
    return Value(best);
}
inline Value b_max(const Value* a, int n, int l) {
    need_args("max", n, 1, -1, l);
    double best = need_num("max", a[0], 0, l);
    for (int i = 1; i < n; ++i) {
        double v = need_num("max", a[i], i, l);
        if (v > best) best = v;
    }
    return Value(best);
}

// ── String ───────────────────────────────────────────────────────

inline Value b_split(const Value* a, int n, int l) {
    need_args("split", n, 2, 2, l);
    std::string s   = need_str("split", a[0], 0, l);
    std::string sep = need_str("split", a[1], 1, l);
    Value arr = Value::make_array();
    auto* ga = arr.as_arr();
    if (sep.empty()) {
        for (char c : s) ga->elements.push_back(Value(std::string(1, c)));
    } else {
        size_t start = 0;
        while (true) {
            size_t p = s.find(sep, start);
            if (p == std::string::npos) {
                ga->elements.push_back(Value(s.substr(start)));
                break;
            }
            ga->elements.push_back(Value(s.substr(start, p - start)));
            start = p + sep.size();
        }
    }
    return arr;
}
inline Value b_join(const Value* a, int n, int l) {
    need_args("join", n, 2, 2, l);
    GCArray* arr = need_arr("join", a[0], 0, l);
    std::string sep = need_str("join", a[1], 1, l);
    std::string out;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        if (i) out += sep;
        out += arr->elements[i].to_str();
    }
    return Value(out);
}
inline Value b_trim(const Value* a, int n, int l) {
    need_args("trim", n, 1, 1, l);
    std::string s = need_str("trim", a[0], 0, l);
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return Value(std::string());
    size_t e = s.find_last_not_of(" \t\r\n");
    return Value(s.substr(b, e - b + 1));
}
inline Value b_upper(const Value* a, int n, int l) {
    need_args("upper", n, 1, 1, l);
    std::string s = need_str("upper", a[0], 0, l);
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return Value(s);
}
inline Value b_lower(const Value* a, int n, int l) {
    need_args("lower", n, 1, 1, l);
    std::string s = need_str("lower", a[0], 0, l);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return Value(s);
}
inline Value b_contains(const Value* a, int n, int l) {
    need_args("contains", n, 2, 2, l);
    if (a[0].is_arr()) {
        GCArray* arr = a[0].as_arr();
        for (const auto& item : arr->elements) {
            if (item.eq(a[1])) return Value(true);
        }
        return Value(false);
    }
    if (a[0].is_dict()) {
        std::string key = need_str("contains", a[1], 1, l);
        return Value(a[0].as_dict()->elements.find(key) != a[0].as_dict()->elements.end());
    }
    if (a[0].is_str()) {
        std::string s   = a[0].as_str_ref();
        std::string sub = need_str("contains", a[1], 1, l);
        return Value(s.find(sub) != std::string::npos);
    }
    throw JitThrow{"contains(): arg 1 must be a string, array, or dict, got " + a[0].to_str(), l};
}
inline Value b_startsWith(const Value* a, int n, int l) {
    need_args("startsWith", n, 2, 2, l);
    std::string s      = need_str("startsWith", a[0], 0, l);
    std::string prefix = need_str("startsWith", a[1], 1, l);
    return Value(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0);
}
inline Value b_endsWith(const Value* a, int n, int l) {
    need_args("endsWith", n, 2, 2, l);
    std::string s      = need_str("endsWith", a[0], 0, l);
    std::string suffix = need_str("endsWith", a[1], 1, l);
    return Value(s.size() >= suffix.size() &&
                 s.compare(s.size()-suffix.size(), suffix.size(), suffix) == 0);
}
inline Value b_indexOf(const Value* a, int n, int l) {
    need_args("indexOf", n, 2, 2, l);
    if (a[0].is_arr()) {
        GCArray* arr = a[0].as_arr();
        for (size_t i = 0; i < arr->elements.size(); ++i)
            if (arr->elements[i].eq(a[1])) return Value((double)i);
        return Value(-1.0);
    }
    std::string s    = need_str("indexOf", a[0], 0, l);
    std::string sub  = need_str("indexOf", a[1], 1, l);
    size_t p = s.find(sub);
    return Value(p == std::string::npos ? -1.0 : (double)p);
}
inline Value b_substring(const Value* a, int n, int l) {
    need_args("substring", n, 2, 3, l);
    std::string s = need_str("substring", a[0], 0, l);
    int start = (int)need_num("substring", a[1], 1, l);
    int end   = n >= 3 ? (int)need_num("substring", a[2], 2, l) : (int)s.size();
    if (start < 0) start = 0;
    if (end > (int)s.size()) end = (int)s.size();
    if (end < start) end = start;
    return Value(s.substr(start, end - start));
}
inline Value b_replace(const Value* a, int n, int l) {
    need_args("replace", n, 3, 3, l);
    std::string s    = need_str("replace", a[0], 0, l);
    std::string from = need_str("replace", a[1], 1, l);
    std::string to   = need_str("replace", a[2], 2, l);
    if (from.empty()) return Value(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + from.size() <= s.size() && s.compare(i, from.size(), from) == 0) {
            out += to;
            i += from.size();
        } else {
            out += s[i++];
        }
    }
    return Value(out);
}

inline bool utf8_continuation(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

inline size_t utf8_next_index(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    unsigned char ch = (unsigned char)s[i];
    size_t len = 1;
    if ((ch & 0x80) == 0) len = 1;
    else if ((ch & 0xE0) == 0xC0) len = 2;
    else if ((ch & 0xF0) == 0xE0) len = 3;
    else if ((ch & 0xF8) == 0xF0) len = 4;
    else return i + 1;
    if (i + len > s.size()) return i + 1;
    for (size_t j = i + 1; j < i + len; ++j) {
        if (!utf8_continuation((unsigned char)s[j])) return i + 1;
    }
    return i + len;
}

inline std::vector<size_t> utf8_boundaries(const std::string& s) {
    std::vector<size_t> offsets;
    offsets.push_back(0);
    for (size_t i = 0; i < s.size();) {
        i = utf8_next_index(s, i);
        offsets.push_back(i);
    }
    return offsets;
}

inline int need_positive_int(const char* name, const Value& v, int idx, int line) {
    double x = need_num(name, v, idx, line);
    if (x <= 0 || x != std::floor(x))
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1) + " must be a positive integer", line};
    return (int)x;
}

inline int need_nonnegative_int(const char* name, const Value& v, int idx, int line) {
    double x = need_num(name, v, idx, line);
    if (x < 0 || x != std::floor(x))
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1) + " must be a non-negative integer", line};
    return (int)x;
}

inline Value b_string_lines(const Value* a, int n, int l) {
    need_args("string_lines", n, 1, 1, l);
    std::string text = need_str("string_lines", a[0], 0, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\r' || ch == '\n') {
            oa->elements.push_back(Value(current));
            current.clear();
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            current.push_back(ch);
        }
    }
    if (!text.empty() && text.back() != '\n' && text.back() != '\r') oa->elements.push_back(Value(current));
    return out;
}

inline Value b_string_words(const Value* a, int n, int l) {
    need_args("string_words", n, 1, 1, l);
    std::string text = need_str("string_words", a[0], 0, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
        size_t start = i;
        while (i < text.size() && !std::isspace((unsigned char)text[i])) ++i;
        if (start < i) oa->elements.push_back(Value(text.substr(start, i - start)));
    }
    return out;
}

inline Value b_string_repeat(const Value* a, int n, int l) {
    need_args("string_repeat", n, 2, 2, l);
    std::string text = need_str("string_repeat", a[0], 0, l);
    int count = need_nonnegative_int("string_repeat", a[1], 1, l);
    std::string out;
    out.reserve(text.size() * (size_t)count);
    for (int i = 0; i < count; ++i) out += text;
    return Value(out);
}

inline std::string repeat_to_utf8_width(const std::string& fill, size_t count) {
    std::string out;
    if (count == 0) return out;
    std::vector<size_t> fill_offsets = utf8_boundaries(fill);
    size_t fill_chars = fill_offsets.size() - 1;
    while (count >= fill_chars) {
        out += fill;
        count -= fill_chars;
    }
    if (count > 0) out += fill.substr(0, fill_offsets[count]);
    return out;
}

inline Value b_string_pad_left(const Value* a, int n, int l) {
    need_args("string_pad_left", n, 2, 3, l);
    std::string text = need_str("string_pad_left", a[0], 0, l);
    int width = need_nonnegative_int("string_pad_left", a[1], 1, l);
    std::string fill = n >= 3 ? need_str("string_pad_left", a[2], 2, l) : " ";
    if (fill.empty()) throw JitThrow{"string_pad_left(): fill must not be empty", l};
    size_t chars = utf8_boundaries(text).size() - 1;
    if (chars >= (size_t)width) return Value(text);
    return Value(repeat_to_utf8_width(fill, (size_t)width - chars) + text);
}

inline Value b_string_pad_right(const Value* a, int n, int l) {
    need_args("string_pad_right", n, 2, 3, l);
    std::string text = need_str("string_pad_right", a[0], 0, l);
    int width = need_nonnegative_int("string_pad_right", a[1], 1, l);
    std::string fill = n >= 3 ? need_str("string_pad_right", a[2], 2, l) : " ";
    if (fill.empty()) throw JitThrow{"string_pad_right(): fill must not be empty", l};
    size_t chars = utf8_boundaries(text).size() - 1;
    if (chars >= (size_t)width) return Value(text);
    return Value(text + repeat_to_utf8_width(fill, (size_t)width - chars));
}

inline Value b_text_chunks(const Value* a, int n, int l) {
    need_args("text_chunks", n, 1, 3, l);
    std::string text = need_str("text_chunks", a[0], 0, l);
    int max_chars = n >= 2 ? need_positive_int("text_chunks", a[1], 1, l) : 800;
    int overlap = n >= 3 ? need_nonnegative_int("text_chunks", a[2], 2, l) : 0;
    if (overlap >= max_chars) throw JitThrow{"text_chunks(): overlap must be smaller than max_chars", l};

    Value out = Value::make_array();
    if (text.empty()) return out;
    std::vector<size_t> offsets = utf8_boundaries(text);
    size_t char_count = offsets.size() - 1;
    size_t start = 0;
    size_t step = (size_t)(max_chars - overlap);
    while (start < char_count) {
        size_t end = std::min(char_count, start + (size_t)max_chars);
        out.as_arr()->elements.push_back(Value(text.substr(offsets[start], offsets[end] - offsets[start])));
        if (end == char_count) break;
        start += step;
    }
    return out;
}

// ── Array ────────────────────────────────────────────────────────

inline Value b_length(const Value* a, int n, int l) {
    need_args("length", n, 1, 1, l);
    if (a[0].is_arr()) return Value((double)a[0].as_arr()->elements.size());
    if (a[0].is_str()) return Value((double)a[0].as_str().size());
    if (a[0].is_dict()) return Value((double)a[0].as_dict()->elements.size());
    throw JitThrow{"length(): requires array/string/dict", l};
}
inline Value b_slice(const Value* a, int n, int l) {
    need_args("slice", n, 2, 3, l);
    if (a[0].is_arr()) {
        GCArray* arr = a[0].as_arr();
        int start = (int)need_num("slice", a[1], 1, l);
        int end   = n >= 3 ? (int)need_num("slice", a[2], 2, l) : (int)arr->elements.size();
        if (start < 0) start = std::max(0, (int)arr->elements.size() + start);
        if (end   < 0) end   = std::max(0, (int)arr->elements.size() + end);
        if (end > (int)arr->elements.size()) end = (int)arr->elements.size();
        if (end < start) end = start;
        Value out = Value::make_array();
        auto* oa = out.as_arr();
        for (int i = start; i < end; ++i) oa->elements.push_back(arr->elements[i]);
        return out;
    }
    if (a[0].is_str()) {
        std::string s = a[0].as_str();
        int start = (int)need_num("slice", a[1], 1, l);
        int end   = n >= 3 ? (int)need_num("slice", a[2], 2, l) : (int)s.size();
        if (start < 0) start = std::max(0, (int)s.size() + start);
        if (end   < 0) end   = std::max(0, (int)s.size() + end);
        if (end > (int)s.size()) end = (int)s.size();
        if (end < start) end = start;
        return Value(s.substr(start, end - start));
    }
    throw JitThrow{"slice(): requires array or string", l};
}
inline Value b_sort(const Value* a, int n, int l) {
    need_args("sort", n, 1, 1, l);
    GCArray* arr = need_arr("sort", a[0], 0, l);
    std::sort(arr->elements.begin(), arr->elements.end(),
              [](const Value& x, const Value& y){ return x.lt(y); });
    return a[0]; // return same array for chaining
}
inline Value b_reverse(const Value* a, int n, int l) {
    need_args("reverse", n, 1, 1, l);
    if (a[0].is_arr()) {
        GCArray* arr = a[0].as_arr();
        std::reverse(arr->elements.begin(), arr->elements.end());
        return a[0];
    }
    if (a[0].is_str()) {
        std::string s = a[0].as_str();
        std::reverse(s.begin(), s.end());
        return Value(s);
    }
    throw JitThrow{"reverse(): requires array or string", l};
}
inline Value b_concat(const Value* a, int n, int l) {
    need_args("concat", n, 2, -1, l);
    if (a[0].is_arr()) {
        Value out = Value::make_array();
        auto* oa = out.as_arr();
        for (int i = 0; i < n; ++i) {
            GCArray* src = need_arr("concat", a[i], i, l);
            oa->elements.insert(oa->elements.end(), src->elements.begin(), src->elements.end());
        }
        return out;
    }
    if (a[0].is_str()) {
        std::string out;
        for (int i = 0; i < n; ++i) out += need_str("concat", a[i], i, l);
        return Value(out);
    }
    throw JitThrow{"concat(): requires arrays or strings", l};
}
inline Value b_push(const Value* a, int n, int l) {
    need_args("push", n, 2, -1, l);
    GCArray* arr = need_arr("push", a[0], 0, l);
    for (int i = 1; i < n; ++i) arr->elements.push_back(a[i]);
    return Value((double)arr->elements.size());
}
inline Value b_pop(const Value* a, int n, int l) {
    need_args("pop", n, 1, 1, l);
    GCArray* arr = need_arr("pop", a[0], 0, l);
    if (arr->elements.empty()) return Value::nil();
    Value back = arr->elements.back();
    arr->elements.pop_back();
    return back;
}

inline Value b_array_sum(const Value* a, int n, int l) {
    need_args("array_sum", n, 1, 1, l);
    GCArray* arr = need_arr("array_sum", a[0], 0, l);
    double sum = 0.0;
    for (const auto& item : arr->elements) {
        if (!item.is_num()) throw JitThrow{"array_sum(): array value must be a number, got " + item.to_str(), l};
        sum += item.as_num();
    }
    return Value(sum);
}

inline Value b_array_avg(const Value* a, int n, int l) {
    need_args("array_avg", n, 1, 1, l);
    GCArray* arr = need_arr("array_avg", a[0], 0, l);
    if (arr->elements.empty()) return Value::nil();
    double sum = 0.0;
    for (const auto& item : arr->elements) {
        if (!item.is_num()) throw JitThrow{"array_avg(): array value must be a number, got " + item.to_str(), l};
        sum += item.as_num();
    }
    return Value(sum / (double)arr->elements.size());
}

inline Value b_array_min(const Value* a, int n, int l) {
    need_args("array_min", n, 1, 1, l);
    GCArray* arr = need_arr("array_min", a[0], 0, l);
    if (arr->elements.empty()) return Value::nil();
    if (!arr->elements[0].is_num()) throw JitThrow{"array_min(): array value must be a number, got " + arr->elements[0].to_str(), l};
    double best = arr->elements[0].as_num();
    for (size_t i = 1; i < arr->elements.size(); ++i) {
        const auto& item = arr->elements[i];
        if (!item.is_num()) throw JitThrow{"array_min(): array value must be a number, got " + item.to_str(), l};
        best = std::min(best, item.as_num());
    }
    return Value(best);
}

inline Value b_array_max(const Value* a, int n, int l) {
    need_args("array_max", n, 1, 1, l);
    GCArray* arr = need_arr("array_max", a[0], 0, l);
    if (arr->elements.empty()) return Value::nil();
    if (!arr->elements[0].is_num()) throw JitThrow{"array_max(): array value must be a number, got " + arr->elements[0].to_str(), l};
    double best = arr->elements[0].as_num();
    for (size_t i = 1; i < arr->elements.size(); ++i) {
        const auto& item = arr->elements[i];
        if (!item.is_num()) throw JitThrow{"array_max(): array value must be a number, got " + item.to_str(), l};
        best = std::max(best, item.as_num());
    }
    return Value(best);
}

inline Value b_array_unique(const Value* a, int n, int l) {
    need_args("array_unique", n, 1, 1, l);
    GCArray* arr = need_arr("array_unique", a[0], 0, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (const auto& item : arr->elements) {
        bool seen = false;
        for (const auto& existing : oa->elements) {
            if (existing.eq(item)) {
                seen = true;
                break;
            }
        }
        if (!seen) oa->elements.push_back(item);
    }
    return out;
}

inline void array_flatten_into(Value& out, const Value& item, int depth) {
    if (depth > 0 && item.is_arr()) {
        for (const auto& child : item.as_arr()->elements) array_flatten_into(out, child, depth - 1);
        return;
    }
    out.as_arr()->elements.push_back(item);
}

inline Value b_array_flatten(const Value* a, int n, int l) {
    need_args("array_flatten", n, 1, 2, l);
    GCArray* arr = need_arr("array_flatten", a[0], 0, l);
    int depth = n >= 2 ? need_nonnegative_int("array_flatten", a[1], 1, l) : 1;
    Value out = Value::make_array();
    for (const auto& item : arr->elements) array_flatten_into(out, item, depth);
    return out;
}

inline Value b_array_range(const Value* a, int n, int l) {
    need_args("array_range", n, 1, 3, l);
    double start = 0.0;
    double end = need_num("array_range", a[0], 0, l);
    double step = 1.0;
    if (n >= 2) {
        start = end;
        end = need_num("array_range", a[1], 1, l);
    }
    if (n >= 3) step = need_num("array_range", a[2], 2, l);
    if (step == 0.0) throw JitThrow{"array_range(): step must not be zero", l};
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    if (step > 0.0) {
        for (double v = start; v < end; v += step) oa->elements.push_back(Value(v));
    } else {
        for (double v = start; v > end; v += step) oa->elements.push_back(Value(v));
    }
    return out;
}

inline Value b_array_chunk(const Value* a, int n, int l) {
    need_args("array_chunk", n, 2, 2, l);
    GCArray* arr = need_arr("array_chunk", a[0], 0, l);
    int size = need_positive_int("array_chunk", a[1], 1, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (size_t start = 0; start < arr->elements.size(); start += (size_t)size) {
        Value chunk = Value::make_array();
        auto* ca = chunk.as_arr();
        size_t end = std::min(arr->elements.size(), start + (size_t)size);
        for (size_t i = start; i < end; ++i) ca->elements.push_back(arr->elements[i]);
        oa->elements.push_back(chunk);
    }
    return out;
}

inline Value b_array_zip(const Value* a, int n, int l) {
    need_args("array_zip", n, 1, -1, l);
    std::vector<GCArray*> arrays;
    arrays.reserve((size_t)n);
    size_t limit = std::numeric_limits<size_t>::max();
    for (int i = 0; i < n; ++i) {
        GCArray* arr = need_arr("array_zip", a[i], i, l);
        arrays.push_back(arr);
        limit = std::min(limit, arr->elements.size());
    }
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (size_t row = 0; row < limit; ++row) {
        Value zipped = Value::make_array();
        auto* za = zipped.as_arr();
        for (auto* arr : arrays) za->elements.push_back(arr->elements[row]);
        oa->elements.push_back(zipped);
    }
    return out;
}

inline Value b_array_repeat(const Value* a, int n, int l) {
    need_args("array_repeat", n, 2, 2, l);
    int count = need_nonnegative_int("array_repeat", a[1], 1, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (int i = 0; i < count; ++i) oa->elements.push_back(a[0]);
    return out;
}

inline bool array_contains_equal(const GCArray* arr, const Value& needle) {
    for (const auto& item : arr->elements) {
        if (item.eq(needle)) return true;
    }
    return false;
}

inline void set_push_unique(GCArray* out, const Value& item) {
    if (!array_contains_equal(out, item)) out->elements.push_back(item);
}

inline Value b_set_union(const Value* a, int n, int l) {
    need_args("set_union", n, 1, -1, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (int i = 0; i < n; ++i) {
        GCArray* arr = need_arr("set_union", a[i], i, l);
        for (const auto& item : arr->elements) set_push_unique(oa, item);
    }
    return out;
}

inline Value b_set_intersection(const Value* a, int n, int l) {
    need_args("set_intersection", n, 1, -1, l);
    GCArray* first = need_arr("set_intersection", a[0], 0, l);
    std::vector<GCArray*> rest;
    for (int i = 1; i < n; ++i) rest.push_back(need_arr("set_intersection", a[i], i, l));
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (const auto& item : first->elements) {
        bool present = true;
        for (auto* arr : rest) {
            if (!array_contains_equal(arr, item)) {
                present = false;
                break;
            }
        }
        if (present) set_push_unique(oa, item);
    }
    return out;
}

inline Value b_set_difference(const Value* a, int n, int l) {
    need_args("set_difference", n, 1, -1, l);
    GCArray* first = need_arr("set_difference", a[0], 0, l);
    std::vector<GCArray*> rest;
    for (int i = 1; i < n; ++i) rest.push_back(need_arr("set_difference", a[i], i, l));
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (const auto& item : first->elements) {
        bool found = false;
        for (auto* arr : rest) {
            if (array_contains_equal(arr, item)) {
                found = true;
                break;
            }
        }
        if (!found) set_push_unique(oa, item);
    }
    return out;
}

inline Value b_set_symmetric_difference(const Value* a, int n, int l) {
    need_args("set_symmetric_difference", n, 2, 2, l);
    GCArray* left = need_arr("set_symmetric_difference", a[0], 0, l);
    GCArray* right = need_arr("set_symmetric_difference", a[1], 1, l);
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (const auto& item : left->elements) {
        if (!array_contains_equal(right, item)) set_push_unique(oa, item);
    }
    for (const auto& item : right->elements) {
        if (!array_contains_equal(left, item)) set_push_unique(oa, item);
    }
    return out;
}

inline Value b_set_is_subset(const Value* a, int n, int l) {
    need_args("set_is_subset", n, 2, 2, l);
    GCArray* left = need_arr("set_is_subset", a[0], 0, l);
    GCArray* right = need_arr("set_is_subset", a[1], 1, l);
    for (const auto& item : left->elements) {
        if (!array_contains_equal(right, item)) return Value(false);
    }
    return Value(true);
}

inline Value b_set_is_superset(const Value* a, int n, int l) {
    need_args("set_is_superset", n, 2, 2, l);
    GCArray* left = need_arr("set_is_superset", a[0], 0, l);
    GCArray* right = need_arr("set_is_superset", a[1], 1, l);
    for (const auto& item : right->elements) {
        if (!array_contains_equal(left, item)) return Value(false);
    }
    return Value(true);
}

// ── Type conversion ──────────────────────────────────────────────

inline Value b_to_int(const Value* a, int n, int l) {
    need_args("to_int", n, 1, 1, l);
    if (a[0].is_num())  return Value(std::floor(a[0].as_num()));
    if (a[0].is_bool()) return Value(a[0].as_bool() ? 1.0 : 0.0);
    if (a[0].is_str()) {
        try { return Value((double)std::stoll(a[0].as_str())); }
        catch (...) { throw JitThrow{"to_int(): cannot parse '" + a[0].as_str() + "'", l}; }
    }
    if (a[0].is_nil()) return Value(0.0);
    throw JitThrow{"to_int(): unsupported type", l};
}
inline Value b_to_float(const Value* a, int n, int l) {
    need_args("to_float", n, 1, 1, l);
    if (a[0].is_num())  return a[0];
    if (a[0].is_bool()) return Value(a[0].as_bool() ? 1.0 : 0.0);
    if (a[0].is_str()) {
        try { return Value(std::stod(a[0].as_str())); }
        catch (...) { throw JitThrow{"to_float(): cannot parse '" + a[0].as_str() + "'", l}; }
    }
    if (a[0].is_nil()) return Value(0.0);
    throw JitThrow{"to_float(): unsupported type", l};
}
inline Value b_to_str(const Value* a, int n, int l) {
    need_args("to_str", n, 1, 1, l);
    return Value(a[0].to_str());
}
inline Value b_to_bool(const Value* a, int n, int l) {
    need_args("to_bool", n, 1, 1, l);
    return Value(a[0].truthy());
}

// ── Clone (shallow copy for value-type semantics on structs) ─────

inline Value b_clone(const Value* a, int n, int l) {
    need_args("clone", n, 1, 1, l);
    const Value& src = a[0];
    if (src.is_inst()) {
        // Shallow copy: new GCInstance with same class + field values
        GCInstance* srci = src.as_inst();
        Value dst = Value::make_inst(srci->type_name());
        dst.as_inst()->fields = srci->fields;
        return dst;
    }
    if (src.is_arr()) {
        Value out = Value::make_array();
        out.as_arr()->elements = src.as_arr()->elements;
        return out;
    }
    if (src.is_dict()) {
        Value out = Value::make_dict();
        out.as_dict()->elements = src.as_dict()->elements;
        return out;
    }
    if (src.is_tensor()) {
        GCTensor* tensor = src.as_tensor();
        return ag_clone_tensor_value("clone", tensor, tensor->requires_grad,
                                     tensor->data.dtype(), l);
    }
    // Numbers, bools, strings, nil are already values — return as-is
    return src;
}

// ── Error helpers (custom error types) ───────────────────────────

// Error("msg")            → {type: "Error",   message: "msg"}
// Error("TypeX", "msg")   → {type: "TypeX",   message: "msg"}
inline Value b_Error(const Value* a, int n, int l) {
    need_args("Error", n, 1, 2, l);
    Value dict = Value::make_dict();
    auto* d = dict.as_dict();
    if (n == 1) {
        d->elements["type"]    = Value(std::string("Error"));
        d->elements["message"] = Value(a[0].to_str());
    } else {
        d->elements["type"]    = Value(a[0].to_str());
        d->elements["message"] = Value(a[1].to_str());
    }
    return dict;
}

inline Value b_assert(const Value* a, int n, int l) {
    need_args("assert", n, 1, 2, l);
    if (!a[0].truthy()) {
        std::string msg = n >= 2 ? a[1].to_str() : "assertion failed";
        throw JitThrow{"assert(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_eq(const Value* a, int n, int l) {
    need_args("assert_eq", n, 2, 3, l);
    if (!a[0].eq(a[1])) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected " + a[0].to_str() + " == " + a[1].to_str();
        throw JitThrow{"assert_eq(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_ne(const Value* a, int n, int l) {
    need_args("assert_ne", n, 2, 3, l);
    if (a[0].eq(a[1])) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected " + a[0].to_str() + " != " + a[1].to_str();
        throw JitThrow{"assert_ne(): " + msg, l};
    }
    return Value(true);
}

inline std::string value_type_name(const Value& v) {
    return v.is_num()     ? "number" :
           v.is_str()     ? "string" :
           v.is_bool()    ? "bool"   :
           v.is_nil()     ? "nil"    :
           v.is_arr()     ? "array"  :
           v.is_dict()    ? "dict"   :
           v.is_tensor()  ? "tensor" :
           v.is_closure() ? "function" :
           v.is_inst()    ? "instance" : "object";
}

inline bool value_contains_for_assert(const Value& haystack, const Value& needle) {
    if (haystack.is_str()) {
        return haystack.as_str_ref().find(needle.to_str()) != std::string::npos;
    }
    if (haystack.is_arr()) {
        for (const auto& item : haystack.as_arr()->elements) {
            if (item.eq(needle)) return true;
        }
        return false;
    }
    if (haystack.is_dict()) {
        return needle.is_str() && haystack.as_dict()->elements.find(needle.as_str()) != haystack.as_dict()->elements.end();
    }
    return false;
}

inline Value b_assert_contains(const Value* a, int n, int l) {
    need_args("assert_contains", n, 2, 3, l);
    if (!value_contains_for_assert(a[0], a[1])) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected " + a[0].to_str() + " to contain " + a[1].to_str();
        throw JitThrow{"assert_contains(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_not_contains(const Value* a, int n, int l) {
    need_args("assert_not_contains", n, 2, 3, l);
    if (value_contains_for_assert(a[0], a[1])) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected " + a[0].to_str() + " to not contain " + a[1].to_str();
        throw JitThrow{"assert_not_contains(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_match(const Value* a, int n, int l) {
    need_args("assert_match", n, 2, 3, l);
    std::string text = need_str("assert_match", a[0], 0, l);
    std::string pattern = need_str("assert_match", a[1], 1, l);
    try {
        if (!std::regex_search(text, std::regex(pattern))) {
            std::string msg = n >= 3 ? a[2].to_str()
                : "expected " + text + " to match " + pattern;
            throw JitThrow{"assert_match(): " + msg, l};
        }
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("assert_match(): ") + e.what(), l};
    }
    return Value(true);
}

inline Value b_assert_type(const Value* a, int n, int l) {
    need_args("assert_type", n, 2, 3, l);
    std::string actual = value_type_name(a[0]);
    std::string expected = need_str("assert_type", a[1], 1, l);
    if (actual != expected) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected type " + expected + ", got " + actual;
        throw JitThrow{"assert_type(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_len(const Value* a, int n, int l) {
    need_args("assert_len", n, 2, 3, l);
    int expected = (int)need_num("assert_len", a[1], 1, l);
    Value len_args[1] = {a[0]};
    int actual = (int)b_length(len_args, 1, l).as_num();
    if (actual != expected) {
        std::string msg = n >= 3 ? a[2].to_str()
            : "expected length " + std::to_string(expected) + ", got " + std::to_string(actual);
        throw JitThrow{"assert_len(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_between(const Value* a, int n, int l) {
    need_args("assert_between", n, 3, 4, l);
    double value = need_num("assert_between", a[0], 0, l);
    double low = need_num("assert_between", a[1], 1, l);
    double high = need_num("assert_between", a[2], 2, l);
    if (value < low || value > high) {
        std::string msg = n >= 4 ? a[3].to_str()
            : "expected " + a[0].to_str() + " to be between " + a[1].to_str() + " and " + a[2].to_str();
        throw JitThrow{"assert_between(): " + msg, l};
    }
    return Value(true);
}

inline Value b_assert_approx(const Value* a, int n, int l) {
    need_args("assert_approx", n, 2, 4, l);
    double actual = need_num("assert_approx", a[0], 0, l);
    double expected = need_num("assert_approx", a[1], 1, l);
    double epsilon = 1e-9;
    int message_index = -1;
    if (n >= 3) {
        if (a[2].is_num()) epsilon = a[2].as_num();
        else message_index = 2;
    }
    if (n >= 4) message_index = 3;
    if (epsilon < 0) throw JitThrow{"assert_approx(): epsilon must be non-negative", l};
    if (std::fabs(actual - expected) > epsilon) {
        std::string msg = message_index >= 0 ? a[message_index].to_str()
            : "expected " + a[0].to_str() + " ~= " + a[1].to_str() + " within " + std::to_string(epsilon);
        throw JitThrow{"assert_approx(): " + msg, l};
    }
    return Value(true);
}

inline Value test_result_dict(const std::string& name, bool passed, const std::string& message, int line) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["name"] = Value(name);
    d->elements["passed"] = Value(passed);
    d->elements["ok"] = Value(passed);
    d->elements["status"] = Value(passed ? "pass" : "fail");
    d->elements["message"] = Value(message);
    d->elements["line"] = Value(line);
    return out;
}

inline Value b_check(const Value* a, int n, int l) {
    need_args("check", n, 2, 3, l);
    std::string name = need_str("check", a[0], 0, l);
    bool passed = a[1].truthy();
    std::string message = n >= 3 ? a[2].to_str() : (passed ? "" : "condition failed");
    return test_result_dict(name, passed, message, l);
}

inline Value b_check_eq(const Value* a, int n, int l) {
    need_args("check_eq", n, 3, 4, l);
    std::string name = need_str("check_eq", a[0], 0, l);
    bool passed = a[1].eq(a[2]);
    std::string message = n >= 4 ? a[3].to_str()
        : (passed ? "" : "expected " + a[1].to_str() + " == " + a[2].to_str());
    Value out = test_result_dict(name, passed, message, l);
    out.as_dict()->elements["actual"] = a[1];
    out.as_dict()->elements["expected"] = a[2];
    return out;
}

inline Value b_check_match(const Value* a, int n, int l) {
    need_args("check_match", n, 3, 4, l);
    std::string name = need_str("check_match", a[0], 0, l);
    std::string text = need_str("check_match", a[1], 1, l);
    std::string pattern = need_str("check_match", a[2], 2, l);
    bool passed = false;
    std::string message;
    try {
        passed = std::regex_search(text, std::regex(pattern));
        message = n >= 4 ? a[3].to_str() : (passed ? "" : "expected " + text + " to match " + pattern);
    } catch (const std::regex_error& e) {
        message = std::string("regex error: ") + e.what();
    }
    Value out = test_result_dict(name, passed, message, l);
    out.as_dict()->elements["text"] = Value(text);
    out.as_dict()->elements["pattern"] = Value(pattern);
    return out;
}

inline bool test_result_passed(const Value& item) {
    if (item.is_bool()) return item.as_bool();
    if (!item.is_dict()) return false;
    auto* d = item.as_dict();
    auto ok = d->elements.find("ok");
    if (ok != d->elements.end()) return ok->second.truthy();
    auto passed = d->elements.find("passed");
    if (passed != d->elements.end()) return passed->second.truthy();
    return false;
}

inline std::string test_result_name(const Value& item, size_t index) {
    if (item.is_dict()) {
        auto* d = item.as_dict();
        auto it = d->elements.find("name");
        if (it != d->elements.end()) return it->second.to_str();
    }
    return "check " + std::to_string(index + 1);
}

inline std::string test_result_message(const Value& item) {
    if (item.is_dict()) {
        auto* d = item.as_dict();
        auto it = d->elements.find("message");
        if (it != d->elements.end()) return it->second.to_str();
    }
    return item.truthy() ? "" : "failed";
}

inline Value b_test_summary(const Value* a, int n, int l) {
    need_args("test_summary", n, 1, 1, l);
    auto* results = need_arr("test_summary", a[0], 0, l);
    int passed = 0;
    Value failures = Value::make_array();
    for (size_t i = 0; i < results->elements.size(); ++i) {
        const Value& item = results->elements[i];
        if (test_result_passed(item)) ++passed;
        else failures.as_arr()->elements.push_back(item);
    }
    int total = (int)results->elements.size();
    int failed = total - passed;
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["total"] = Value(total);
    d->elements["passed"] = Value(passed);
    d->elements["failed"] = Value(failed);
    d->elements["ok"] = Value(failed == 0);
    d->elements["failures"] = failures;
    return out;
}

inline Value b_test_report(const Value* a, int n, int l) {
    need_args("test_report", n, 1, 2, l);
    auto* results = need_arr("test_report", a[0], 0, l);
    std::string title = n >= 2 ? need_str("test_report", a[1], 1, l) : "Sura tests";
    int passed = 0;
    int failed = 0;
    std::ostringstream out;
    out << title << "\n";
    for (size_t i = 0; i < results->elements.size(); ++i) {
        const Value& item = results->elements[i];
        bool ok = test_result_passed(item);
        if (ok) ++passed;
        else ++failed;
        out << "[" << (ok ? "PASS" : "FAIL") << "] " << test_result_name(item, i);
        std::string message = test_result_message(item);
        if (!message.empty()) out << " - " << message;
        out << "\n";
    }
    out << "total: " << results->elements.size()
        << ", passed: " << passed
        << ", failed: " << failed;
    return Value(out.str());
}

// File, JSON, network, and async helpers keep Sura useful for practical scripts.

inline std::string read_text_file(const std::string& path, int line) {
    std::ifstream in(fs_path_from_utf8(path), std::ios::binary);
    if (!in) throw JitThrow{"file_read(): cannot open '" + path + "'", line};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::string read_binary_file_for(const std::string& path, int line, const char* name) {
    std::ifstream in(fs_path_from_utf8(path), std::ios::binary);
    if (!in) throw JitThrow{std::string(name) + "(): cannot open '" + path + "'", line};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline Value b_file_read(const Value* a, int n, int l) {
    need_args("file_read", n, 1, 1, l);
    return Value(read_text_file(need_str("file_read", a[0], 0, l), l));
}

inline Value b_file_write(const Value* a, int n, int l) {
    need_args("file_write", n, 2, 2, l);
    std::string path = need_str("file_write", a[0], 0, l);
    std::filesystem::path p = fs_path_from_utf8(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw JitThrow{"file_write(): cannot open '" + path + "'", l};
    std::string text = a[1].to_str();
    out << text;
    if (!out) throw JitThrow{"file_write(): failed writing '" + path + "'", l};
    return Value((double)text.size());
}

inline unsigned char need_byte_element(const char* name, const Value& v, size_t index, int line) {
    if (!v.is_num()) {
        throw JitThrow{std::string(name) + "(): byte " + std::to_string(index + 1)
                       + " must be a number, got " + v.to_str(), line};
    }
    double raw = v.as_num();
    if (raw < 0 || raw > 255 || raw != std::floor(raw)) {
        throw JitThrow{std::string(name) + "(): byte " + std::to_string(index + 1)
                       + " must be an integer from 0 to 255", line};
    }
    return (unsigned char)raw;
}

inline Value b_file_read_bytes(const Value* a, int n, int l) {
    need_args("file_read_bytes", n, 1, 1, l);
    std::string path = need_str("file_read_bytes", a[0], 0, l);
    std::string bytes = read_binary_file_for(path, l, "file_read_bytes");

    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(bytes.size());
    for (unsigned char byte : bytes) arr->elements.push_back(Value((double)byte));
    return out;
}

inline Value b_file_write_bytes(const Value* a, int n, int l) {
    need_args("file_write_bytes", n, 2, 2, l);
    std::string path = need_str("file_write_bytes", a[0], 0, l);
    auto* bytes = need_arr("file_write_bytes", a[1], 1, l);
    std::filesystem::path p = fs_path_from_utf8(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw JitThrow{"file_write_bytes(): cannot open '" + path + "'", l};
    for (size_t i = 0; i < bytes->elements.size(); ++i) {
        char ch = (char)need_byte_element("file_write_bytes", bytes->elements[i], i, l);
        out.write(&ch, 1);
    }
    if (!out) throw JitThrow{"file_write_bytes(): failed writing '" + path + "'", l};
    return Value((double)bytes->elements.size());
}

inline Value b_file_append(const Value* a, int n, int l) {
    need_args("file_append", n, 2, 2, l);
    std::string path = need_str("file_append", a[0], 0, l);
    std::filesystem::path p = fs_path_from_utf8(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) throw JitThrow{"file_append(): cannot open '" + path + "'", l};
    std::string text = a[1].to_str();
    out << text;
    if (!out) throw JitThrow{"file_append(): failed writing '" + path + "'", l};
    return Value((double)text.size());
}

inline Value b_file_exists(const Value* a, int n, int l) {
    need_args("file_exists", n, 1, 1, l);
    return Value(std::filesystem::exists(fs_path_from_utf8(need_str("file_exists", a[0], 0, l))));
}

inline Value b_file_delete(const Value* a, int n, int l) {
    need_args("file_delete", n, 1, 1, l);
    std::error_code ec;
    bool removed = std::filesystem::remove(fs_path_from_utf8(need_str("file_delete", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"file_delete(): " + ec.message(), l};
    return Value(removed);
}

inline Value b_file_remove_tree(const Value* a, int n, int l) {
    need_args("file_remove_tree", n, 1, 1, l);
    std::string raw = need_str("file_remove_tree", a[0], 0, l);
    bool blank = true;
    for (unsigned char ch : raw) {
        if (!std::isspace(ch)) {
            blank = false;
            break;
        }
    }
    if (blank) throw JitThrow{"file_remove_tree(): path must not be empty", l};

    std::filesystem::path target = fs_path_from_utf8(raw);
    std::filesystem::path normalized = target.lexically_normal();
    std::string normalized_text = fs_path_to_generic_utf8(normalized);
    if (normalized_text == "." || normalized_text == "..") {
        throw JitThrow{"file_remove_tree(): refusing to remove current or parent directory", l};
    }

    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        if (ec) throw JitThrow{"file_remove_tree(): " + ec.message(), l};
        return Value(0.0);
    }

    std::filesystem::path absolute = std::filesystem::absolute(target, ec).lexically_normal();
    if (ec) throw JitThrow{"file_remove_tree(): " + ec.message(), l};
    std::filesystem::path root = absolute.root_path();
    if (!root.empty() && absolute == root) {
        throw JitThrow{"file_remove_tree(): refusing to remove filesystem root", l};
    }

    std::filesystem::path cwd = std::filesystem::current_path(ec).lexically_normal();
    if (!ec && absolute == cwd) {
        throw JitThrow{"file_remove_tree(): refusing to remove current directory", l};
    }
    ec.clear();
    std::filesystem::path temp = std::filesystem::temp_directory_path(ec).lexically_normal();
    if (!ec && absolute == temp) {
        throw JitThrow{"file_remove_tree(): refusing to remove temp directory root", l};
    }

    ec.clear();
    std::uintmax_t removed = std::filesystem::remove_all(target, ec);
    if (ec) throw JitThrow{"file_remove_tree(): " + ec.message(), l};
    return Value((double)removed);
}

inline Value b_file_list(const Value* a, int n, int l) {
    need_args("file_list", n, 1, 1, l);
    std::string path = need_str("file_list", a[0], 0, l);
    Value out = Value::make_array();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(fs_path_from_utf8(path), ec)) {
        out.as_arr()->elements.push_back(Value(fs_path_to_utf8(entry.path().filename())));
    }
    if (ec) throw JitThrow{"file_list(): " + ec.message(), l};
    return out;
}

inline Value b_file_walk(const Value* a, int n, int l) {
    need_args("file_walk", n, 1, 2, l);
    std::string path = need_str("file_walk", a[0], 0, l);
    std::string ext = n >= 2 ? need_str("file_walk", a[1], 1, l) : "";
    if (!ext.empty() && ext[0] != '.') ext = "." + ext;

    std::vector<std::string> paths;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        fs_path_from_utf8(path), std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) throw JitThrow{"file_walk(): " + ec.message(), l};
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec) continue;
        if (!ext.empty() && it->path().extension().string() != ext) continue;
        paths.push_back(fs_path_to_generic_utf8(it->path().lexically_normal()));
    }
    std::sort(paths.begin(), paths.end());

    Value out = Value::make_array();
    for (const auto& item : paths) out.as_arr()->elements.push_back(Value(item));
    return out;
}

inline bool glob_has_wildcard(const std::string& pattern) {
    return pattern.find_first_of("*?") != std::string::npos;
}

inline std::string glob_normalize_slashes(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

inline std::string glob_base_dir(const std::string& pattern) {
    size_t wild = pattern.find_first_of("*?");
    if (wild == std::string::npos) {
        std::filesystem::path p = fs_path_from_utf8(pattern);
        std::filesystem::path parent = p.parent_path();
        return parent.empty() ? std::string(".") : fs_path_to_generic_utf8(parent);
    }
    size_t slash = pattern.substr(0, wild).find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    std::string base = pattern.substr(0, slash + 1);
    if (base.empty()) return ".";
    return base;
}

inline std::string glob_regex_escape(char ch) {
    static const std::string special = R"(\.^$|()[]{}+)";
    if (special.find(ch) != std::string::npos) return std::string("\\") + ch;
    return std::string(1, ch);
}

inline std::regex glob_to_regex(const std::string& raw_pattern) {
    std::string pattern = glob_normalize_slashes(raw_pattern);
    std::string out = "^";
    for (size_t i = 0; i < pattern.size();) {
        char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                    out += "(?:.*/)?";
                    i += 3;
                } else {
                    out += ".*";
                    i += 2;
                }
            } else {
                out += "[^/]*";
                ++i;
            }
        } else if (ch == '?') {
            out += "[^/]";
            ++i;
        } else if (ch == '/') {
            out += "/";
            ++i;
        } else {
            out += glob_regex_escape(ch);
            ++i;
        }
    }
    out += "$";
    return std::regex(out);
}

inline Value b_file_glob(const Value* a, int n, int l) {
    need_args("file_glob", n, 1, 1, l);
    std::string pattern = need_str("file_glob", a[0], 0, l);
    if (pattern.empty()) throw JitThrow{"file_glob(): pattern must not be empty", l};

    std::vector<std::string> paths;
    std::error_code ec;
    if (!glob_has_wildcard(pattern)) {
        std::filesystem::path exact = fs_path_from_utf8(pattern);
        if (std::filesystem::is_regular_file(exact, ec) && !ec) {
            paths.push_back(fs_path_to_generic_utf8(exact.lexically_normal()));
        }
    } else {
        std::string normalized_pattern = glob_normalize_slashes(pattern);
        std::filesystem::path base = fs_path_from_utf8(glob_base_dir(pattern));
        if (std::filesystem::exists(base, ec) && std::filesystem::is_directory(base, ec)) {
            std::regex rx = glob_to_regex(normalized_pattern);
            std::filesystem::recursive_directory_iterator it(
                base, std::filesystem::directory_options::skip_permission_denied, ec);
            std::filesystem::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                std::error_code item_ec;
                if (!it->is_regular_file(item_ec) || item_ec) continue;
                std::string candidate = glob_normalize_slashes(fs_path_to_generic_utf8(it->path().lexically_normal()));
                if (std::regex_match(candidate, rx)) paths.push_back(candidate);
            }
        }
    }
    std::sort(paths.begin(), paths.end());

    Value out = Value::make_array();
    for (const auto& item : paths) out.as_arr()->elements.push_back(Value(item));
    return out;
}

inline Value b_mkdir(const Value* a, int n, int l) {
    need_args("mkdir", n, 1, 1, l);
    std::error_code ec;
    bool created = std::filesystem::create_directories(fs_path_from_utf8(need_str("mkdir", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"mkdir(): " + ec.message(), l};
    return Value(created);
}

inline Value b_cwd(const Value*, int, int l) {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    if (ec) throw JitThrow{"cwd(): " + ec.message(), l};
    return Value(fs_path_to_utf8(p));
}

inline Value b_temp_dir(const Value*, int n, int l) {
    need_args("temp_dir", n, 0, 0, l);
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (ec) throw JitThrow{"temp_dir(): " + ec.message(), l};
    return Value(fs_path_to_utf8(p));
}

inline std::string home_dir_text() {
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home);
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && *userprofile) return std::string(userprofile);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && *drive && path && *path) return std::string(drive) + std::string(path);
#endif
    return "";
}

inline Value b_home_dir(const Value*, int n, int l) {
    need_args("home_dir", n, 0, 0, l);
    return Value(home_dir_text());
}

inline Value b_path_separator(const Value*, int n, int l) {
    need_args("path_separator", n, 0, 0, l);
#ifdef _WIN32
    return Value("\\");
#else
    return Value("/");
#endif
}

inline Value b_os_name(const Value*, int n, int l) {
    need_args("os_name", n, 0, 0, l);
#ifdef _WIN32
    return Value("windows");
#elif defined(__APPLE__)
    return Value("macos");
#elif defined(__linux__)
    return Value("linux");
#elif defined(__unix__)
    return Value("unix");
#else
    return Value("unknown");
#endif
}

inline Value b_is_windows(const Value*, int n, int l) {
    need_args("is_windows", n, 0, 0, l);
#ifdef _WIN32
    return Value(true);
#else
    return Value(false);
#endif
}

inline bool command_has_path_separator(const std::string& command) {
    return command.find('/') != std::string::npos || command.find('\\') != std::string::npos;
}

inline std::vector<std::string> split_path_list(const std::string& raw) {
    std::vector<std::string> out;
#ifdef _WIN32
    char sep = ';';
#else
    char sep = ':';
#endif
    size_t start = 0;
    while (start <= raw.size()) {
        size_t pos = raw.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(raw.substr(start));
            break;
        }
        out.push_back(raw.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

inline std::vector<std::string> executable_suffixes_for(const std::string& command) {
#ifdef _WIN32
    std::filesystem::path command_path = fs_path_from_utf8(command);
    if (command_path.has_extension()) return {""};
    const char* raw_pathext = std::getenv("PATHEXT");
    std::string raw = raw_pathext && *raw_pathext ? std::string(raw_pathext) : ".COM;.EXE;.BAT;.CMD";
    std::vector<std::string> suffixes = {""};
    for (const auto& item : split_path_list(raw)) {
        if (item.empty()) continue;
        suffixes.push_back(item[0] == '.' ? item : "." + item);
    }
    return suffixes;
#else
    (void)command;
    return {""};
#endif
}

inline bool executable_candidate_ok(const std::filesystem::path& path) {
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return false;
#ifdef _WIN32
    return true;
#else
    auto exec_bits = std::filesystem::perms::owner_exec |
                     std::filesystem::perms::group_exec |
                     std::filesystem::perms::others_exec;
    return (st.permissions() & exec_bits) != std::filesystem::perms::none;
#endif
}

inline std::string find_command_on_path(const std::string& command) {
    if (command.empty()) return "";
    std::filesystem::path command_path = fs_path_from_utf8(command);
    bool direct = command_path.is_absolute() || command_has_path_separator(command);
    std::vector<std::string> dirs;
    if (direct) {
        dirs.push_back("");
    } else {
        const char* raw_path = std::getenv("PATH");
        if (!raw_path || !*raw_path) return "";
        dirs = split_path_list(raw_path);
    }

    auto suffixes = executable_suffixes_for(command);
    for (const auto& dir : dirs) {
        std::filesystem::path base;
        if (direct) {
            base = command_path;
        } else if (dir.empty()) {
            std::error_code cwd_ec;
            base = std::filesystem::current_path(cwd_ec) / command_path;
            if (cwd_ec) continue;
        } else {
            base = fs_path_from_utf8(dir) / command_path;
        }
        for (const auto& suffix : suffixes) {
            auto candidate = base;
            if (!suffix.empty()) candidate += suffix;
            if (!executable_candidate_ok(candidate)) continue;
            std::error_code abs_ec;
            auto abs_path = std::filesystem::absolute(candidate, abs_ec);
            return fs_path_to_utf8((abs_ec ? candidate : abs_path).lexically_normal());
        }
    }
    return "";
}

inline Value b_which(const Value* a, int n, int l) {
    need_args("which", n, 1, 1, l);
    return Value(find_command_on_path(need_str("which", a[0], 0, l)));
}

inline Value b_cmd_exists(const Value* a, int n, int l) {
    need_args("cmd_exists", n, 1, 1, l);
    return Value(!find_command_on_path(need_str("cmd_exists", a[0], 0, l)).empty());
}

inline bool cmd_quote_safe_char(char ch) {
    unsigned char c = (unsigned char)ch;
#ifdef _WIN32
    return std::isalnum(c) || ch == '_' || ch == '-' || ch == '.' ||
           ch == '/' || ch == '\\' || ch == ':' || ch == '@' ||
           ch == '+' || ch == '=' || ch == ',';
#else
    return std::isalnum(c) || ch == '_' || ch == '-' || ch == '.' ||
           ch == '/' || ch == ':' || ch == '@' ||
           ch == '%' || ch == '+' || ch == '=' || ch == ',';
#endif
}

inline std::string shell_quote_arg(const std::string& text) {
    if (text.empty()) {
#ifdef _WIN32
        return "\"\"";
#else
        return "''";
#endif
    }
    bool safe = true;
    for (char ch : text) {
        if (!cmd_quote_safe_char(ch)) {
            safe = false;
            break;
        }
    }
    if (safe) return text;
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"') out += "\\\"";
        else if (ch == '^') out += "^^";
        else if (ch == '%') out += "^%";
        else if (ch == '!') out += "^!";
        else out.push_back(ch);
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
#endif
}

inline Value b_cmd_quote(const Value* a, int n, int l) {
    need_args("cmd_quote", n, 1, 1, l);
    return Value(shell_quote_arg(need_str("cmd_quote", a[0], 0, l)));
}

inline Value b_cmd_join(const Value* a, int n, int l) {
    need_args("cmd_join", n, 1, 1, l);
    auto* arr = need_arr("cmd_join", a[0], 0, l);
    std::string out;
    for (const auto& item : arr->elements) {
        if (!item.is_str()) throw JitThrow{"cmd_join(): all args must be strings", l};
        if (!out.empty()) out += " ";
        out += shell_quote_arg(item.as_str());
    }
    return Value(out);
}

inline Value b_path_join(const Value* a, int n, int l) {
    need_args("path_join", n, 1, -1, l);
    std::filesystem::path out;
    auto append = [&](const Value& v) {
        std::string part = v.to_str();
        if (out.empty()) out = fs_path_from_utf8(part);
        else out /= fs_path_from_utf8(part);
    };
    if (n == 1 && a[0].is_arr()) {
        auto* parts = a[0].as_arr();
        for (const auto& part : parts->elements) append(part);
    } else {
        for (int i = 0; i < n; ++i) append(a[i]);
    }
    return Value(fs_path_to_utf8(out));
}

inline Value b_path_basename(const Value* a, int n, int l) {
    need_args("path_basename", n, 1, 1, l);
    return Value(fs_path_to_utf8(fs_path_from_utf8(need_str("path_basename", a[0], 0, l)).filename()));
}

inline Value b_path_dirname(const Value* a, int n, int l) {
    need_args("path_dirname", n, 1, 1, l);
    return Value(fs_path_to_utf8(fs_path_from_utf8(need_str("path_dirname", a[0], 0, l)).parent_path()));
}

inline Value b_path_ext(const Value* a, int n, int l) {
    need_args("path_ext", n, 1, 1, l);
    return Value(fs_path_to_utf8(fs_path_from_utf8(need_str("path_ext", a[0], 0, l)).extension()));
}

inline Value b_path_stem(const Value* a, int n, int l) {
    need_args("path_stem", n, 1, 1, l);
    return Value(fs_path_to_utf8(fs_path_from_utf8(need_str("path_stem", a[0], 0, l)).stem()));
}

inline Value b_path_normalize(const Value* a, int n, int l) {
    need_args("path_normalize", n, 1, 1, l);
    return Value(fs_path_to_utf8(fs_path_from_utf8(need_str("path_normalize", a[0], 0, l)).lexically_normal()));
}

inline Value b_path_abs(const Value* a, int n, int l) {
    need_args("path_abs", n, 1, 1, l);
    std::error_code ec;
    std::filesystem::path out = std::filesystem::absolute(fs_path_from_utf8(need_str("path_abs", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"path_abs(): " + ec.message(), l};
    return Value(fs_path_to_utf8(out.lexically_normal()));
}

inline Value b_path_relative(const Value* a, int n, int l) {
    need_args("path_relative", n, 1, 2, l);
    std::filesystem::path path = fs_path_from_utf8(need_str("path_relative", a[0], 0, l));
    std::filesystem::path base;
    if (n >= 2) base = fs_path_from_utf8(need_str("path_relative", a[1], 1, l));
    else {
        std::error_code cwd_ec;
        base = std::filesystem::current_path(cwd_ec);
        if (cwd_ec) throw JitThrow{"path_relative(): " + cwd_ec.message(), l};
    }
    std::error_code ec;
    std::filesystem::path out = std::filesystem::relative(path, base, ec);
    if (ec) throw JitThrow{"path_relative(): " + ec.message(), l};
    return Value(fs_path_to_utf8(out));
}

inline Value b_file_is_dir(const Value* a, int n, int l) {
    need_args("file_is_dir", n, 1, 1, l);
    std::error_code ec;
    bool ok = std::filesystem::is_directory(fs_path_from_utf8(need_str("file_is_dir", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"file_is_dir(): " + ec.message(), l};
    return Value(ok);
}

inline Value b_file_is_file(const Value* a, int n, int l) {
    need_args("file_is_file", n, 1, 1, l);
    std::error_code ec;
    bool ok = std::filesystem::is_regular_file(fs_path_from_utf8(need_str("file_is_file", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"file_is_file(): " + ec.message(), l};
    return Value(ok);
}

inline Value b_file_size(const Value* a, int n, int l) {
    need_args("file_size", n, 1, 1, l);
    std::error_code ec;
    auto size = std::filesystem::file_size(fs_path_from_utf8(need_str("file_size", a[0], 0, l)), ec);
    if (ec) throw JitThrow{"file_size(): " + ec.message(), l};
    return Value((double)size);
}

inline Value b_file_info(const Value* a, int n, int l) {
    need_args("file_info", n, 1, 1, l);
    std::string raw = need_str("file_info", a[0], 0, l);
    std::filesystem::path path = fs_path_from_utf8(raw);
    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);
    if (ec) throw JitThrow{"file_info(): " + ec.message(), l};
    bool is_file = exists && std::filesystem::is_regular_file(path, ec);
    if (ec) throw JitThrow{"file_info(): " + ec.message(), l};
    bool is_dir = exists && std::filesystem::is_directory(path, ec);
    if (ec) throw JitThrow{"file_info(): " + ec.message(), l};

    Value info = Value::make_dict();
    auto* d = info.as_dict();
    d->elements["path"] = Value(fs_path_to_utf8(path));
    d->elements["absolute"] = Value(fs_path_to_utf8(std::filesystem::absolute(path, ec).lexically_normal()));
    if (ec) d->elements["absolute"] = Value::nil();
    d->elements["name"] = Value(fs_path_to_utf8(path.filename()));
    d->elements["dir"] = Value(fs_path_to_utf8(path.parent_path()));
    d->elements["ext"] = Value(fs_path_to_utf8(path.extension()));
    d->elements["stem"] = Value(fs_path_to_utf8(path.stem()));
    d->elements["exists"] = Value(exists);
    d->elements["is_file"] = Value(is_file);
    d->elements["is_dir"] = Value(is_dir);
    if (is_file) {
        std::error_code size_ec;
        d->elements["size"] = Value((double)std::filesystem::file_size(path, size_ec));
        if (size_ec) d->elements["size"] = Value::nil();
    } else {
        d->elements["size"] = Value::nil();
    }
    if (exists) {
        std::error_code time_ec;
        auto file_time = std::filesystem::last_write_time(path, time_ec);
        if (!time_ec) {
            auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            d->elements["modified"] = Value((double)std::chrono::system_clock::to_time_t(system_time));
        } else {
            d->elements["modified"] = Value::nil();
        }
    } else {
        d->elements["modified"] = Value::nil();
    }
    return info;
}

inline Value b_file_copy(const Value* a, int n, int l) {
    need_args("file_copy", n, 2, 3, l);
    std::string src = need_str("file_copy", a[0], 0, l);
    std::string dst = need_str("file_copy", a[1], 1, l);
    bool overwrite = n < 3 || a[2].truthy();
    std::filesystem::path src_path = fs_path_from_utf8(src);
    std::filesystem::path dst_path = fs_path_from_utf8(dst);
    if (dst_path.has_parent_path()) std::filesystem::create_directories(dst_path.parent_path());
    std::error_code ec;
    auto options = overwrite ? std::filesystem::copy_options::overwrite_existing
                             : std::filesystem::copy_options::none;
    bool copied = std::filesystem::copy_file(src_path, dst_path, options, ec);
    if (ec) throw JitThrow{"file_copy(): " + ec.message(), l};
    return Value(copied);
}

inline Value b_file_move(const Value* a, int n, int l) {
    need_args("file_move", n, 2, 3, l);
    std::string src = need_str("file_move", a[0], 0, l);
    std::string dst = need_str("file_move", a[1], 1, l);
    bool overwrite = n < 3 || a[2].truthy();
    std::filesystem::path src_path = fs_path_from_utf8(src);
    std::filesystem::path dst_path = fs_path_from_utf8(dst);
    if (dst_path.has_parent_path()) std::filesystem::create_directories(dst_path.parent_path());
    std::error_code ec;
    if (overwrite && std::filesystem::exists(dst_path, ec)) {
        std::filesystem::remove(dst_path, ec);
        if (ec) throw JitThrow{"file_move(): " + ec.message(), l};
    }
    std::filesystem::rename(src_path, dst_path, ec);
    if (ec) throw JitThrow{"file_move(): " + ec.message(), l};
    return Value(true);
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(ch >> 4) & 0xf];
                    out += hex[ch & 0xf];
                } else {
                    out += (char)ch;
                }
        }
    }
    return out;
}

class JsonParser {
    const std::string& s;
    size_t pos = 0;
    int line = 0;

    void skip_ws() {
        while (pos < s.size() && std::isspace((unsigned char)s[pos])) ++pos;
    }
    bool match(const char* lit) {
        size_t len = std::strlen(lit);
        if (s.compare(pos, len, lit) == 0) {
            pos += len;
            return true;
        }
        return false;
    }
    [[noreturn]] void fail(const std::string& msg) const {
        throw JitThrow{"json_parse(): " + msg + " at byte " + std::to_string(pos), line};
    }
    std::string parse_string() {
        if (pos >= s.size() || s[pos] != '"') fail("expected string");
        ++pos;
        std::string out;
        while (pos < s.size()) {
            char ch = s[pos++];
            if (ch == '"') return out;
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (pos >= s.size()) fail("unterminated escape");
            char esc = s[pos++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (pos + 4 > s.size()) fail("short unicode escape");
                    out += '?';
                    pos += 4;
                    break;
                default:
                    fail("bad escape");
            }
        }
        fail("unterminated string");
    }
    Value parse_number() {
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
        }
        try {
            return Value(std::stod(s.substr(start, pos - start)));
        } catch (...) {
            fail("invalid number");
        }
    }
    Value parse_array() {
        ++pos;
        Value arr = Value::make_array();
        skip_ws();
        if (pos < s.size() && s[pos] == ']') { ++pos; return arr; }
        while (true) {
            arr.as_arr()->elements.push_back(parse_value());
            skip_ws();
            if (pos >= s.size()) fail("unterminated array");
            if (s[pos] == ']') { ++pos; return arr; }
            if (s[pos] != ',') fail("expected ',' or ']'");
            ++pos;
        }
    }
    Value parse_object() {
        ++pos;
        Value dict = Value::make_dict();
        skip_ws();
        if (pos < s.size() && s[pos] == '}') { ++pos; return dict; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (pos >= s.size() || s[pos] != ':') fail("expected ':'");
            ++pos;
            dict.as_dict()->elements[key] = parse_value();
            skip_ws();
            if (pos >= s.size()) fail("unterminated object");
            if (s[pos] == '}') { ++pos; return dict; }
            if (s[pos] != ',') fail("expected ',' or '}'");
            ++pos;
        }
    }

public:
    JsonParser(const std::string& input, int src_line) : s(input), line(src_line) {}

    Value parse_value() {
        skip_ws();
        if (pos >= s.size()) fail("unexpected end");
        char ch = s[pos];
        if (ch == '"') return Value(parse_string());
        if (ch == '[') return parse_array();
        if (ch == '{') return parse_object();
        if (ch == '-' || std::isdigit((unsigned char)ch)) return parse_number();
        if (match("true")) return Value(true);
        if (match("false")) return Value(false);
        if (match("null") || match("nil")) return Value::nil();
        fail("unexpected token");
    }

    Value parse_document() {
        Value out = parse_value();
        skip_ws();
        if (pos != s.size()) fail("trailing data");
        return out;
    }
};

inline Value b_json_parse(const Value* a, int n, int l) {
    need_args("json_parse", n, 1, 1, l);
    return JsonParser(need_str("json_parse", a[0], 0, l), l).parse_document();
}

inline Value b_json_try_parse(const Value* a, int n, int l) {
    need_args("json_try_parse", n, 1, 2, l);
    std::string text = need_str("json_try_parse", a[0], 0, l);
    try {
        return JsonParser(text, l).parse_document();
    } catch (const JitThrow&) {
        return n >= 2 ? a[1] : Value::nil();
    }
}

inline std::string json_stringify_value(const Value& v) {
    if (v.is_nil()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_num()) {
        double x = v.as_num();
        if (!std::isfinite(x)) return "null";
        const double integer_min = -std::ldexp(1.0, 63);
        const double integer_limit = std::ldexp(1.0, 63);
        if (x == std::floor(x)
            && x >= integer_min && x < integer_limit) {
            return std::to_string((long long)x);
        }
        char buffer[64];
        auto converted = std::to_chars(buffer, buffer + sizeof(buffer), x, std::chars_format::general);
        if (converted.ec == std::errc()) return std::string(buffer, converted.ptr);
        std::ostringstream ss;
        ss << std::setprecision(std::numeric_limits<double>::max_digits10) << x;
        return ss.str();
    }
    if (v.is_str()) return "\"" + json_escape(v.as_str_ref()) + "\"";
    if (v.is_tensor()) {
        GCTensor* tensor = v.as_tensor();
        ag_cuda_materialize_host(tensor, "json_stringify", 0);
        size_t offset = 0;
        std::function<std::string(size_t)> encode = [&](size_t depth) -> std::string {
            if (depth == tensor->shape.size()) {
                return json_stringify_value(Value(tensor->data[offset++]));
            }
            std::string out = "[";
            for (size_t i = 0; i < tensor->shape[depth]; ++i) {
                if (i) out += ",";
                out += encode(depth + 1);
            }
            return out + "]";
        };
        return encode(0);
    }
    if (v.is_arr()) {
        std::string out = "[";
        auto* arr = v.as_arr();
        for (size_t i = 0; i < arr->elements.size(); ++i) {
            if (i) out += ",";
            out += json_stringify_value(arr->elements[i]);
        }
        return out + "]";
    }
    if (v.is_dict()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, val] : v.as_dict()->elements) {
            if (!first) out += ",";
            first = false;
            out += "\"" + json_escape(k) + "\":" + json_stringify_value(val);
        }
        return out + "}";
    }
    return "\"" + json_escape(v.to_str()) + "\"";
}

inline std::string json_pretty_indent(int indent, int depth) {
    return std::string((size_t)indent * (size_t)depth, ' ');
}

inline std::string json_pretty_value(const Value& v, int indent, int depth) {
    if (!v.is_arr() && !v.is_dict()) return json_stringify_value(v);
    if (v.is_arr()) {
        auto* arr = v.as_arr();
        if (arr->elements.empty()) return "[]";
        std::string out = "[\n";
        for (size_t i = 0; i < arr->elements.size(); ++i) {
            if (i) out += ",\n";
            out += json_pretty_indent(indent, depth + 1);
            out += json_pretty_value(arr->elements[i], indent, depth + 1);
        }
        out += "\n" + json_pretty_indent(indent, depth) + "]";
        return out;
    }

    auto* dict = v.as_dict();
    if (dict->elements.empty()) return "{}";
    std::vector<std::string> keys;
    keys.reserve(dict->elements.size());
    for (const auto& kv : dict->elements) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    std::string out = "{\n";
    const std::string key_sep = indent > 0 ? ": " : ":";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ",\n";
        const std::string& key = keys[i];
        out += json_pretty_indent(indent, depth + 1);
        out += "\"" + json_escape(key) + "\"" + key_sep;
        out += json_pretty_value(dict->elements.at(key), indent, depth + 1);
    }
    out += "\n" + json_pretty_indent(indent, depth) + "}";
    return out;
}

inline Value b_json_stringify(const Value* a, int n, int l) {
    need_args("json_stringify", n, 1, 1, l);
    return Value(json_stringify_value(a[0]));
}

inline Value b_json_pretty(const Value* a, int n, int l) {
    need_args("json_pretty", n, 1, 2, l);
    double raw_indent = n >= 2 ? need_num("json_pretty", a[1], 1, l) : 2.0;
    if (raw_indent < 0 || raw_indent > 16 || raw_indent != std::floor(raw_indent)) {
        throw JitThrow{"json_pretty(): indent must be an integer from 0 to 16", l};
    }
    return Value(json_pretty_value(a[0], (int)raw_indent, 0));
}

inline Value b_file_read_json(const Value* a, int n, int l) {
    need_args("file_read_json", n, 1, 1, l);
    std::string path = need_str("file_read_json", a[0], 0, l);
    try {
        return JsonParser(read_text_file(path, l), l).parse_document();
    } catch (const JitThrow& e) {
        throw JitThrow{"file_read_json(): " + e.message, l};
    }
}

inline Value b_file_write_json(const Value* a, int n, int l) {
    need_args("file_write_json", n, 2, 2, l);
    std::string path = need_str("file_write_json", a[0], 0, l);
    std::string text = json_stringify_value(a[1]);
    Value args[2] = {Value(path), Value(text)};
    return b_file_write(args, 2, l);
}

inline bool jsonl_line_blank(const std::string& line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
}

inline Value b_jsonl_parse(const Value* a, int n, int l) {
    need_args("jsonl_parse", n, 1, 1, l);
    const std::string text = need_str("jsonl_parse", a[0], 0, l);
    Value out = Value::make_array();
    size_t start = 0;
    int record = 1;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        bool last = end == std::string::npos;
        std::string line = last ? text.substr(start) : text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!jsonl_line_blank(line)) {
            try {
                out.as_arr()->elements.push_back(JsonParser(line, l).parse_document());
            } catch (const JitThrow& e) {
                throw JitThrow{"jsonl_parse(): record " + std::to_string(record) + ": " + e.message, l};
            }
        }
        if (last) break;
        start = end + 1;
        ++record;
    }
    return out;
}

inline Value b_jsonl_stringify(const Value* a, int n, int l) {
    need_args("jsonl_stringify", n, 1, 2, l);
    auto* rows = need_arr("jsonl_stringify", a[0], 0, l);
    bool trailing_newline = n >= 2 && a[1].truthy();
    std::string out;
    for (size_t i = 0; i < rows->elements.size(); ++i) {
        if (i) out.push_back('\n');
        out += json_stringify_value(rows->elements[i]);
    }
    if (trailing_newline && !out.empty()) out.push_back('\n');
    return Value(out);
}

inline void sse_flush_event(Value& out, std::string& event_name, std::vector<std::string>& data_lines,
                            std::string& id, std::string& retry, bool& seen) {
    if (!seen) return;
    Value ev = Value::make_dict();
    auto* d = ev.as_dict();
    d->elements["event"] = Value(event_name.empty() ? std::string("message") : event_name);
    std::string data;
    for (size_t i = 0; i < data_lines.size(); ++i) {
        if (i) data.push_back('\n');
        data += data_lines[i];
    }
    d->elements["data"] = Value(data);
    if (!id.empty()) d->elements["id"] = Value(id);
    if (!retry.empty()) {
        bool numeric = std::all_of(retry.begin(), retry.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        });
        if (numeric) {
            double retry_num = 0;
            for (unsigned char ch : retry) retry_num = retry_num * 10 + (ch - '0');
            d->elements["retry"] = Value(retry_num);
        } else {
            d->elements["retry"] = Value(retry);
        }
    }
    out.as_arr()->elements.push_back(ev);
    event_name.clear();
    data_lines.clear();
    id.clear();
    retry.clear();
    seen = false;
}

inline Value b_sse_parse(const Value* a, int n, int l) {
    need_args("sse_parse", n, 1, 1, l);
    std::istringstream lines(need_str("sse_parse", a[0], 0, l));
    Value out = Value::make_array();
    std::string event_name;
    std::vector<std::string> data_lines;
    std::string id;
    std::string retry;
    bool seen = false;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            sse_flush_event(out, event_name, data_lines, id, retry, seen);
            continue;
        }
        if (!line.empty() && line[0] == ':') continue;
        size_t colon = line.find(':');
        std::string field = colon == std::string::npos ? line : line.substr(0, colon);
        std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(value.begin());
        seen = true;
        if (field == "event") event_name = value;
        else if (field == "data") data_lines.push_back(value);
        else if (field == "id") id = value;
        else if (field == "retry") retry = value;
    }
    sse_flush_event(out, event_name, data_lines, id, retry, seen);
    return out;
}

inline Value b_sse_data(const Value* a, int n, int l) {
    need_args("sse_data", n, 1, 2, l);
    Value events = b_sse_parse(a, 1, l);
    bool parse_json = n >= 2 && a[1].truthy();
    Value out = Value::make_array();
    auto* arr = events.as_arr();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        if (!arr->elements[i].is_dict()) continue;
        auto* ev = arr->elements[i].as_dict();
        auto it = ev->elements.find("data");
        if (it == ev->elements.end()) continue;
        std::string data = it->second.to_str();
        if (data == "[DONE]") continue;
        if (parse_json) {
            try {
                out.as_arr()->elements.push_back(JsonParser(data, l).parse_document());
            } catch (const JitThrow& e) {
                throw JitThrow{"sse_data(): event " + std::to_string(i + 1) + ": " + e.message, l};
            }
        } else {
            out.as_arr()->elements.push_back(Value(data));
        }
    }
    return out;
}

inline std::string run_capture_command(const std::string& command);
inline Value b_http_get(const Value* a, int n, int l);

inline std::vector<std::vector<std::string>> csv_parse_rows(const std::string& text, int line) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    bool field_started = false;
    bool just_ended_row = true;

    auto end_field = [&]() {
        row.push_back(field);
        field.clear();
        field_started = false;
    };
    auto end_row = [&]() {
        end_field();
        rows.push_back(row);
        row.clear();
        just_ended_row = true;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(ch);
            }
            field_started = true;
            just_ended_row = false;
            continue;
        }

        if (ch == '"' && !field_started) {
            in_quotes = true;
            field_started = true;
            just_ended_row = false;
        } else if (ch == ',') {
            end_field();
            just_ended_row = false;
        } else if (ch == '\n') {
            end_row();
        } else if (ch == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_row();
        } else {
            field.push_back(ch);
            field_started = true;
            just_ended_row = false;
        }
    }

    if (in_quotes) throw JitThrow{"csv_parse(): unterminated quoted field", line};
    if (field_started || !row.empty() || !just_ended_row) {
        end_field();
        rows.push_back(row);
    }
    return rows;
}

inline Value csv_rows_to_value(const std::vector<std::vector<std::string>>& rows) {
    Value out = Value::make_array();
    for (const auto& row : rows) {
        Value row_value = Value::make_array();
        for (const auto& cell : row) row_value.as_arr()->elements.push_back(Value(cell));
        out.as_arr()->elements.push_back(row_value);
    }
    return out;
}

inline Value b_csv_parse(const Value* a, int n, int l) {
    need_args("csv_parse", n, 1, 2, l);
    auto rows = csv_parse_rows(need_str("csv_parse", a[0], 0, l), l);
    bool has_header = n >= 2 && a[1].truthy();
    if (!has_header) return csv_rows_to_value(rows);

    Value out = Value::make_array();
    if (rows.empty()) return out;
    const auto& header = rows[0];
    for (size_t r = 1; r < rows.size(); ++r) {
        Value item = Value::make_dict();
        auto* d = item.as_dict();
        for (size_t c = 0; c < header.size(); ++c) {
            std::string value = c < rows[r].size() ? rows[r][c] : "";
            d->elements[header[c]] = Value(value);
        }
        out.as_arr()->elements.push_back(item);
    }
    return out;
}

inline std::string csv_escape_cell(const std::string& text) {
    bool quote = text.find_first_of(",\"\r\n") != std::string::npos;
    if (!quote) return text;
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

inline void csv_append_row(std::string& out, const std::vector<std::string>& cells) {
    if (!out.empty()) out.push_back('\n');
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) out.push_back(',');
        out += csv_escape_cell(cells[i]);
    }
}

inline std::vector<std::string> csv_header_values(const Value& v, int line) {
    auto* arr = need_arr("csv_stringify", v, 1, line);
    std::vector<std::string> headers;
    headers.reserve(arr->elements.size());
    for (const auto& item : arr->elements) headers.push_back(item.to_str());
    return headers;
}

inline std::vector<std::string> csv_derive_headers(GCArray* rows) {
    std::vector<std::string> headers;
    for (const auto& row : rows->elements) {
        if (!row.is_dict()) continue;
        for (const auto& [key, _] : row.as_dict()->elements) {
            if (std::find(headers.begin(), headers.end(), key) == headers.end()) headers.push_back(key);
        }
    }
    std::sort(headers.begin(), headers.end());
    return headers;
}

inline Value b_csv_stringify(const Value* a, int n, int l) {
    need_args("csv_stringify", n, 1, 2, l);
    auto* rows = need_arr("csv_stringify", a[0], 0, l);
    std::vector<std::string> headers = n >= 2 ? csv_header_values(a[1], l) : csv_derive_headers(rows);
    std::string out;
    if (!headers.empty()) csv_append_row(out, headers);

    for (const auto& row : rows->elements) {
        std::vector<std::string> cells;
        if (row.is_arr()) {
            for (const auto& cell : row.as_arr()->elements) cells.push_back(cell.is_nil() ? "" : cell.to_str());
        } else if (row.is_dict()) {
            if (headers.empty()) throw JitThrow{"csv_stringify(): dict rows require headers", l};
            auto* dict = row.as_dict();
            for (const auto& header : headers) {
                auto it = dict->elements.find(header);
                cells.push_back(it == dict->elements.end() || it->second.is_nil() ? "" : it->second.to_str());
            }
        } else {
            throw JitThrow{"csv_stringify(): rows must contain arrays or dicts", l};
        }
        csv_append_row(out, cells);
    }
    return Value(out);
}

inline std::string ini_trim_ascii(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline std::string ini_unquote_value(const std::string& text, int line) {
    if (text.size() < 2) return text;
    char quote = text.front();
    if ((quote != '"' && quote != '\'') || text.back() != quote) return text;
    std::string out;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < text.size() - 1) {
            char next = text[++i];
            if (next == 'n') out.push_back('\n');
            else if (next == 'r') out.push_back('\r');
            else if (next == 't') out.push_back('\t');
            else out.push_back(next);
        } else if (ch == '\r' || ch == '\n') {
            throw JitThrow{"ini_parse(): quoted values must stay on one line", line};
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

inline Value b_ini_parse(const Value* a, int n, int l) {
    need_args("ini_parse", n, 1, 1, l);
    std::istringstream lines(need_str("ini_parse", a[0], 0, l));
    Value root = Value::make_dict();
    GCDict* current = root.as_dict();
    std::string line;
    int line_no = 0;
    while (std::getline(lines, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string work = ini_trim_ascii(line);
        if (work.empty() || work[0] == '#' || work[0] == ';') continue;
        if (work.front() == '[' && work.back() == ']') {
            std::string section = ini_trim_ascii(work.substr(1, work.size() - 2));
            if (section.empty()) throw JitThrow{"ini_parse(): empty section name at line " + std::to_string(line_no), l};
            auto it = root.as_dict()->elements.find(section);
            if (it == root.as_dict()->elements.end()) {
                Value section_value = Value::make_dict();
                root.as_dict()->elements[section] = section_value;
                current = section_value.as_dict();
            } else {
                if (!it->second.is_dict()) throw JitThrow{"ini_parse(): section conflicts with scalar key '" + section + "'", l};
                current = it->second.as_dict();
            }
            continue;
        }
        size_t eq = work.find('=');
        size_t colon = work.find(':');
        size_t sep = std::min(eq == std::string::npos ? work.size() : eq,
                              colon == std::string::npos ? work.size() : colon);
        if (sep >= work.size()) throw JitThrow{"ini_parse(): expected key=value at line " + std::to_string(line_no), l};
        std::string key = ini_trim_ascii(work.substr(0, sep));
        if (key.empty()) throw JitThrow{"ini_parse(): empty key at line " + std::to_string(line_no), l};
        std::string value = ini_unquote_value(ini_trim_ascii(work.substr(sep + 1)), l);
        current->elements[key] = Value(value);
    }
    return root;
}

inline bool ini_key_safe(const std::string& text) {
    return !text.empty() && text.find_first_of(":=[]\r\n") == std::string::npos;
}

inline std::string ini_escape_value(const Value& value) {
    std::string text = value.is_nil() ? std::string() : value.to_str();
    bool quote = text.empty() || std::isspace((unsigned char)text.front()) ||
                 std::isspace((unsigned char)text.back()) ||
                 text.find_first_of("#;=[]\r\n") != std::string::npos;
    if (!quote) return text;
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

inline void ini_append_assignments(std::string& out, GCDict* dict, int line) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : dict->elements) {
        if (!value.is_dict()) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        if (!ini_key_safe(key)) throw JitThrow{"ini_stringify(): invalid key '" + key + "'", line};
        if (!out.empty()) out.push_back('\n');
        out += key + "=" + ini_escape_value(dict->elements[key]);
    }
}

inline Value b_ini_stringify(const Value* a, int n, int l) {
    need_args("ini_stringify", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"ini_stringify(): arg 1 must be a dict", l};
    GCDict* root = a[0].as_dict();
    std::string out;
    ini_append_assignments(out, root, l);

    std::vector<std::string> sections;
    for (const auto& [key, value] : root->elements) {
        if (value.is_dict()) sections.push_back(key);
    }
    std::sort(sections.begin(), sections.end());
    for (const auto& section : sections) {
        if (!ini_key_safe(section)) throw JitThrow{"ini_stringify(): invalid section '" + section + "'", l};
        if (!out.empty()) out += "\n\n";
        out += "[" + section + "]";
        ini_append_assignments(out, root->elements[section].as_dict(), l);
    }
    return Value(out);
}

inline std::string schema_type_name(const Value& v) {
    if (v.is_num()) return "number";
    if (v.is_str()) return "string";
    if (v.is_bool()) return "bool";
    if (v.is_arr()) return "array";
    if (v.is_dict()) return "dict";
    if (v.is_nil()) return "nil";
    if (v.is_inst()) return "instance";
    if (v.is_closure()) return "function";
    return "value";
}

inline const Value* dict_get_ptr(GCDict* d, const std::string& key) {
    auto it = d->elements.find(key);
    return it == d->elements.end() ? nullptr : &it->second;
}

inline Value json_deep_copy(const Value& src) {
    if (src.is_arr()) {
        Value out = Value::make_array();
        auto* dst = out.as_arr();
        for (const auto& item : src.as_arr()->elements) {
            dst->elements.push_back(json_deep_copy(item));
        }
        return out;
    }
    if (src.is_dict()) {
        Value out = Value::make_dict();
        auto* dst = out.as_dict();
        for (const auto& [key, value] : src.as_dict()->elements) {
            dst->elements[key] = json_deep_copy(value);
        }
        return out;
    }
    return src;
}

inline Value json_merge_patch_value(const Value& target, const Value& patch) {
    if (!patch.is_dict()) return json_deep_copy(patch);

    Value out = Value::make_dict();
    auto* dst = out.as_dict();
    if (target.is_dict()) {
        for (const auto& [key, value] : target.as_dict()->elements) {
            dst->elements[key] = json_deep_copy(value);
        }
    }

    for (const auto& [key, patch_value] : patch.as_dict()->elements) {
        if (patch_value.is_nil()) {
            dst->elements.erase(key);
            continue;
        }
        auto existing = dst->elements.find(key);
        Value base = existing == dst->elements.end() ? Value::nil() : existing->second;
        dst->elements[key] = json_merge_patch_value(base, patch_value);
    }
    return out;
}

struct JsonPathToken {
    bool is_index = false;
    std::string key;
    size_t index = 0;
};

inline Value json_path_missing(const Value* fallback) {
    return fallback ? *fallback : Value::nil();
}

[[noreturn]] inline void json_path_fail(const char* fn, const std::string& message, int line) {
    throw JitThrow{std::string(fn) + "(): " + message, line};
}

inline std::string json_path_parse_key_segment(const std::string& path, size_t& pos, int line, const char* fn) {
    size_t start = pos;
    while (pos < path.size() && path[pos] != '.' && path[pos] != '[') pos++;
    if (start == pos) json_path_fail(fn, "expected key at byte " + std::to_string(pos), line);
    return path.substr(start, pos - start);
}

inline std::string json_path_parse_quoted_key(const std::string& path, size_t& pos, int line, const char* fn) {
    char quote = path[pos++];
    std::string key;
    bool closed = false;
    while (pos < path.size()) {
        char ch = path[pos++];
        if (ch == '\\') {
            if (pos >= path.size()) json_path_fail(fn, "unfinished escape in quoted key", line);
            char escaped = path[pos++];
            switch (escaped) {
                case '\\': key += '\\'; break;
                case '"': key += '"'; break;
                case '\'': key += '\''; break;
                case 'n': key += '\n'; break;
                case 'r': key += '\r'; break;
                case 't': key += '\t'; break;
                default: key += escaped; break;
            }
        } else if (ch == quote) {
            closed = true;
            break;
        } else {
            key += ch;
        }
    }
    if (!closed) json_path_fail(fn, "unterminated quoted key", line);
    if (pos >= path.size() || path[pos] != ']') json_path_fail(fn, "expected ] after quoted key", line);
    pos++;
    return key;
}

inline size_t json_path_parse_index(const std::string& path, size_t& pos, int line, const char* fn) {
    if (pos >= path.size() || !std::isdigit((unsigned char)path[pos]))
        json_path_fail(fn, "expected array index at byte " + std::to_string(pos), line);
    size_t index = 0;
    while (pos < path.size() && std::isdigit((unsigned char)path[pos])) {
        size_t digit = (size_t)(path[pos] - '0');
        if (index > (((size_t)-1) - digit) / 10)
            json_path_fail(fn, "array index is too large", line);
        index = index * 10 + digit;
        pos++;
    }
    if (pos >= path.size() || path[pos] != ']') json_path_fail(fn, "expected ] after array index", line);
    pos++;
    return index;
}

inline const Value* json_path_lookup_key(const Value* current, const std::string& key) {
    if (!current || !current->is_dict()) return nullptr;
    auto* dict = current->as_dict();
    auto it = dict->elements.find(key);
    return it == dict->elements.end() ? nullptr : &it->second;
}

inline const Value* json_path_lookup_index(const Value* current, size_t index) {
    if (!current || !current->is_arr()) return nullptr;
    auto* arr = current->as_arr();
    if (index >= arr->elements.size()) return nullptr;
    return &arr->elements[index];
}

inline std::vector<JsonPathToken> json_path_tokenize(const std::string& path, int line, const char* fn) {
    std::vector<JsonPathToken> tokens;
    if (path.empty() || path == "$") return tokens;

    size_t pos = 0;
    if (path[pos] == '$') {
        pos++;
        if (pos < path.size() && path[pos] != '.' && path[pos] != '[')
            json_path_fail(fn, "expected . or [ after $", line);
    }

    while (pos < path.size()) {
        JsonPathToken token;
        if (path[pos] == '.') {
            pos++;
            token.key = json_path_parse_key_segment(path, pos, line, fn);
        } else if (path[pos] == '[') {
            pos++;
            if (pos < path.size() && (path[pos] == '"' || path[pos] == '\'')) {
                token.key = json_path_parse_quoted_key(path, pos, line, fn);
            } else {
                token.is_index = true;
                token.index = json_path_parse_index(path, pos, line, fn);
            }
        } else {
            token.key = json_path_parse_key_segment(path, pos, line, fn);
        }
        tokens.push_back(token);
    }
    return tokens;
}

inline const Value* json_path_find(const Value& root, const std::string& path, int line, const char* fn) {
    if (path.empty() || path == "$") return &root;
    const Value* current = &root;
    size_t pos = 0;
    if (path[pos] == '$') {
        pos++;
        if (pos < path.size() && path[pos] != '.' && path[pos] != '[')
            json_path_fail(fn, "expected . or [ after $", line);
    }

    while (pos < path.size()) {
        if (path[pos] == '.') {
            pos++;
            std::string key = json_path_parse_key_segment(path, pos, line, fn);
            current = json_path_lookup_key(current, key);
        } else if (path[pos] == '[') {
            pos++;
            if (pos < path.size() && (path[pos] == '"' || path[pos] == '\'')) {
                std::string key = json_path_parse_quoted_key(path, pos, line, fn);
                current = json_path_lookup_key(current, key);
            } else {
                size_t index = json_path_parse_index(path, pos, line, fn);
                current = json_path_lookup_index(current, index);
            }
        } else {
            std::string key = json_path_parse_key_segment(path, pos, line, fn);
            current = json_path_lookup_key(current, key);
        }
        if (!current) return nullptr;
    }

    return current;
}

inline Value json_path_lookup(const Value& root, const std::string& path, const Value* fallback, int line, const char* fn) {
    const Value* found = json_path_find(root, path, line, fn);
    return found ? *found : json_path_missing(fallback);
}

inline bool json_delete_path_in_place(Value& current, const std::vector<JsonPathToken>& tokens, size_t token_index) {
    if (token_index >= tokens.size()) {
        current = Value::nil();
        return true;
    }

    const JsonPathToken& token = tokens[token_index];
    const bool last = token_index + 1 == tokens.size();
    if (token.is_index) {
        if (!current.is_arr()) return false;
        auto* arr = current.as_arr();
        if (token.index >= arr->elements.size()) return false;
        if (last) {
            arr->elements.erase(arr->elements.begin() + (std::ptrdiff_t)token.index);
            return true;
        }
        return json_delete_path_in_place(arr->elements[token.index], tokens, token_index + 1);
    }

    if (!current.is_dict()) return false;
    auto* dict = current.as_dict();
    auto it = dict->elements.find(token.key);
    if (it == dict->elements.end()) return false;
    if (last) {
        dict->elements.erase(it);
        return true;
    }
    return json_delete_path_in_place(it->second, tokens, token_index + 1);
}

inline Value json_delete_path_value(const Value& target, const std::string& path, int line, const char* fn) {
    std::vector<JsonPathToken> tokens = json_path_tokenize(path, line, fn);
    if (tokens.empty()) return Value::nil();
    Value out = json_deep_copy(target);
    json_delete_path_in_place(out, tokens, 0);
    return out;
}

inline void json_ensure_child_container(Value& child, const JsonPathToken& next) {
    if (next.is_index) {
        if (!child.is_arr()) child = Value::make_array();
    } else {
        if (!child.is_dict()) child = Value::make_dict();
    }
}

inline void json_set_path_in_place(Value& current, const std::vector<JsonPathToken>& tokens, size_t token_index, const Value& replacement) {
    if (token_index >= tokens.size()) {
        current = json_deep_copy(replacement);
        return;
    }

    const JsonPathToken& token = tokens[token_index];
    const bool last = token_index + 1 == tokens.size();
    if (token.is_index) {
        if (!current.is_arr()) current = Value::make_array();
        auto* arr = current.as_arr();
        if (arr->elements.size() <= token.index) arr->elements.resize(token.index + 1);
        if (last) {
            arr->elements[token.index] = json_deep_copy(replacement);
            return;
        }
        json_ensure_child_container(arr->elements[token.index], tokens[token_index + 1]);
        json_set_path_in_place(arr->elements[token.index], tokens, token_index + 1, replacement);
        return;
    }

    if (!current.is_dict()) current = Value::make_dict();
    auto* dict = current.as_dict();
    if (last) {
        dict->elements[token.key] = json_deep_copy(replacement);
        return;
    }
    Value& child = dict->elements[token.key];
    json_ensure_child_container(child, tokens[token_index + 1]);
    json_set_path_in_place(child, tokens, token_index + 1, replacement);
}

inline Value json_set_path_value(const Value& target, const std::string& path, const Value& replacement, int line, const char* fn) {
    std::vector<JsonPathToken> tokens = json_path_tokenize(path, line, fn);
    if (tokens.empty()) return json_deep_copy(replacement);
    Value out = json_deep_copy(target);
    json_set_path_in_place(out, tokens, 0, replacement);
    return out;
}

inline Value b_json_path(const Value* a, int n, int l) {
    need_args("json_path", n, 2, 3, l);
    const std::string path = need_str("json_path", a[1], 1, l);
    return json_path_lookup(a[0], path, n >= 3 ? &a[2] : nullptr, l, "json_path");
}

inline Value b_dict_get_path(const Value* a, int n, int l) {
    need_args("dict_get_path", n, 2, 3, l);
    const std::string path = need_str("dict_get_path", a[1], 1, l);
    return json_path_lookup(a[0], path, n >= 3 ? &a[2] : nullptr, l, "dict_get_path");
}

inline std::vector<std::string> sorted_dict_keys(GCDict* dict) {
    std::vector<std::string> keys;
    keys.reserve(dict->elements.size());
    for (const auto& entry : dict->elements) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

inline Value b_dict_keys(const Value* a, int n, int l) {
    need_args("dict_keys", n, 1, 1, l);
    GCDict* dict = need_dict("dict_keys", a[0], 0, l);
    Value out = Value::make_array();
    for (const auto& key : sorted_dict_keys(dict)) out.as_arr()->elements.push_back(Value(key));
    return out;
}

inline Value b_dict_values(const Value* a, int n, int l) {
    need_args("dict_values", n, 1, 1, l);
    GCDict* dict = need_dict("dict_values", a[0], 0, l);
    Value out = Value::make_array();
    for (const auto& key : sorted_dict_keys(dict)) out.as_arr()->elements.push_back(dict->elements[key]);
    return out;
}

inline Value b_dict_items(const Value* a, int n, int l) {
    need_args("dict_items", n, 1, 1, l);
    GCDict* dict = need_dict("dict_items", a[0], 0, l);
    Value out = Value::make_array();
    for (const auto& key : sorted_dict_keys(dict)) {
        Value item = Value::make_dict();
        auto* id = item.as_dict();
        id->elements["key"] = Value(key);
        id->elements["value"] = dict->elements[key];
        out.as_arr()->elements.push_back(item);
    }
    return out;
}

inline Value b_dict_merge(const Value* a, int n, int l) {
    need_args("dict_merge", n, 1, -1, l);
    Value out = Value::make_dict();
    auto* od = out.as_dict();
    for (int i = 0; i < n; ++i) {
        GCDict* dict = need_dict("dict_merge", a[i], i, l);
        for (const auto& entry : dict->elements) od->elements[entry.first] = entry.second;
    }
    return out;
}

inline std::vector<std::string> need_key_list(const char* name, const Value& value, int idx, int line) {
    GCArray* arr = need_arr(name, value, idx, line);
    std::vector<std::string> keys;
    keys.reserve(arr->elements.size());
    for (const auto& item : arr->elements) {
        if (!item.is_str()) throw JitThrow{std::string(name) + "(): keys must be strings, got " + item.to_str(), line};
        keys.push_back(item.as_str());
    }
    return keys;
}

inline Value b_dict_pick(const Value* a, int n, int l) {
    need_args("dict_pick", n, 2, 2, l);
    GCDict* dict = need_dict("dict_pick", a[0], 0, l);
    Value out = Value::make_dict();
    auto* od = out.as_dict();
    for (const auto& key : need_key_list("dict_pick", a[1], 1, l)) {
        auto it = dict->elements.find(key);
        if (it != dict->elements.end()) od->elements[key] = it->second;
    }
    return out;
}

inline Value b_dict_omit(const Value* a, int n, int l) {
    need_args("dict_omit", n, 2, 2, l);
    GCDict* dict = need_dict("dict_omit", a[0], 0, l);
    std::vector<std::string> keys = need_key_list("dict_omit", a[1], 1, l);
    Value out = Value::make_dict();
    auto* od = out.as_dict();
    for (const auto& entry : dict->elements) {
        if (std::find(keys.begin(), keys.end(), entry.first) == keys.end()) od->elements[entry.first] = entry.second;
    }
    return out;
}

inline Value b_json_has_path(const Value* a, int n, int l) {
    need_args("json_has_path", n, 2, 2, l);
    const std::string path = need_str("json_has_path", a[1], 1, l);
    return Value(json_path_find(a[0], path, l, "json_has_path") != nullptr);
}

inline Value b_json_merge_patch(const Value* a, int n, int l) {
    need_args("json_merge_patch", n, 2, 2, l);
    return json_merge_patch_value(a[0], a[1]);
}

inline Value b_json_delete_path(const Value* a, int n, int l) {
    need_args("json_delete_path", n, 2, 2, l);
    const std::string path = need_str("json_delete_path", a[1], 1, l);
    return json_delete_path_value(a[0], path, l, "json_delete_path");
}

inline Value b_json_set_path(const Value* a, int n, int l) {
    need_args("json_set_path", n, 3, 3, l);
    const std::string path = need_str("json_set_path", a[1], 1, l);
    return json_set_path_value(a[0], path, a[2], l, "json_set_path");
}

inline std::string collection_key_text(const Value& value) {
    if (value.is_nil()) return "nil";
    if (value.is_str()) return value.as_str();
    if (value.is_arr() || value.is_dict()) return json_stringify_value(value);
    return value.to_str();
}

inline bool collection_value_less(const Value& left, const Value& right) {
    if (left.is_num() && right.is_num()) return left.as_num() < right.as_num();
    if (left.is_str() && right.is_str()) return left.as_str_ref() < right.as_str_ref();
    std::string left_type = schema_type_name(left);
    std::string right_type = schema_type_name(right);
    if (left_type != right_type) return left_type < right_type;
    return collection_key_text(left) < collection_key_text(right);
}

inline Value b_pluck(const Value* a, int n, int l) {
    need_args("pluck", n, 2, 3, l);
    GCArray* rows = need_arr("pluck", a[0], 0, l);
    std::string path = need_str("pluck", a[1], 1, l);
    const Value* fallback = n >= 3 ? &a[2] : nullptr;
    Value out = Value::make_array();
    auto* oa = out.as_arr();
    for (const auto& row : rows->elements) {
        oa->elements.push_back(json_path_lookup(row, path, fallback, l, "pluck"));
    }
    return out;
}

inline Value b_count_by(const Value* a, int n, int l) {
    need_args("count_by", n, 2, 2, l);
    GCArray* rows = need_arr("count_by", a[0], 0, l);
    std::string path = need_str("count_by", a[1], 1, l);
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    for (const auto& row : rows->elements) {
        std::string key = collection_key_text(json_path_lookup(row, path, nullptr, l, "count_by"));
        auto it = d->elements.find(key);
        double count = (it == d->elements.end() || !it->second.is_num()) ? 0 : it->second.as_num();
        d->elements[key] = Value(count + 1);
    }
    return out;
}

inline Value b_group_by(const Value* a, int n, int l) {
    need_args("group_by", n, 2, 2, l);
    GCArray* rows = need_arr("group_by", a[0], 0, l);
    std::string path = need_str("group_by", a[1], 1, l);
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    for (const auto& row : rows->elements) {
        std::string key = collection_key_text(json_path_lookup(row, path, nullptr, l, "group_by"));
        auto it = d->elements.find(key);
        if (it == d->elements.end() || !it->second.is_arr()) {
            d->elements[key] = Value::make_array();
            it = d->elements.find(key);
        }
        it->second.as_arr()->elements.push_back(row);
    }
    return out;
}

inline Value b_sort_by(const Value* a, int n, int l) {
    need_args("sort_by", n, 2, 3, l);
    GCArray* rows = need_arr("sort_by", a[0], 0, l);
    std::string path = need_str("sort_by", a[1], 1, l);
    bool desc = n >= 3 && a[2].truthy();
    std::stable_sort(rows->elements.begin(), rows->elements.end(),
        [&](const Value& left, const Value& right) {
            Value lv = json_path_lookup(left, path, nullptr, l, "sort_by");
            Value rv = json_path_lookup(right, path, nullptr, l, "sort_by");
            if (desc) return collection_value_less(rv, lv);
            return collection_value_less(lv, rv);
        });
    return a[0];
}

inline std::string template_trim_ascii(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline std::string template_value_text(const Value& value) {
    if (value.is_nil()) return "";
    if (value.is_str()) return value.as_str();
    if (value.is_arr() || value.is_dict()) return json_stringify_value(value);
    return value.to_str();
}

inline Value b_template_render(const Value* a, int n, int l) {
    need_args("template_render", n, 2, 3, l);
    const std::string input = need_str("template_render", a[0], 0, l);
    Value missing = n >= 3 ? a[2] : Value("");
    std::string out;
    size_t pos = 0;

    while (pos < input.size()) {
        size_t open_brace = input.find("{{", pos);
        size_t open_bracket = input.find("[[", pos);
        size_t open = std::string::npos;
        std::string close_token;
        if (open_brace != std::string::npos &&
            (open_bracket == std::string::npos || open_brace < open_bracket)) {
            open = open_brace;
            close_token = "}}";
        } else if (open_bracket != std::string::npos) {
            open = open_bracket;
            close_token = "]]";
        }
        if (open == std::string::npos) {
            out += input.substr(pos);
            break;
        }
        out += input.substr(pos, open - pos);
        size_t close = input.find(close_token, open + 2);
        if (close == std::string::npos) {
            throw JitThrow{"template_render(): unterminated placeholder", l};
        }
        std::string path = template_trim_ascii(input.substr(open + 2, close - open - 2));
        if (path.empty()) throw JitThrow{"template_render(): empty placeholder", l};
        Value value = json_path_lookup(a[1], path, &missing, l, "template_render");
        out += template_value_text(value);
        pos = close + close_token.size();
    }
    return Value(out);
}

inline bool schema_type_matches(const Value& value, const std::string& type_name) {
    std::string t = type_name;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (t == "any") return true;
    if (t == "number" || t == "num") return value.is_num();
    if (t == "integer" || t == "int") return value.is_num() && value.as_num() == std::floor(value.as_num());
    if (t == "string" || t == "str") return value.is_str();
    if (t == "bool" || t == "boolean") return value.is_bool();
    if (t == "array" || t == "list") return value.is_arr();
    if (t == "dict" || t == "object") return value.is_dict();
    if (t == "nil" || t == "null") return value.is_nil();
    return false;
}

inline bool schema_values_equal(const Value& left, const Value& right) {
    if (left.eq(right)) return true;
    if (left.is_arr() && right.is_arr()) {
        auto* la = left.as_arr();
        auto* ra = right.as_arr();
        if (la->elements.size() != ra->elements.size()) return false;
        for (size_t i = 0; i < la->elements.size(); ++i)
            if (!schema_values_equal(la->elements[i], ra->elements[i])) return false;
        return true;
    }
    if (left.is_dict() && right.is_dict()) {
        auto* ld = left.as_dict();
        auto* rd = right.as_dict();
        if (ld->elements.size() != rd->elements.size()) return false;
        for (const auto& [key, lv] : ld->elements) {
            auto it = rd->elements.find(key);
            if (it == rd->elements.end() || !schema_values_equal(lv, it->second)) return false;
        }
        return true;
    }
    return false;
}

inline bool schema_is_control_key(const std::string& key) {
    static const std::unordered_set<std::string> keys = {
        "type", "required", "properties", "items", "enum",
        "min", "max", "min_len", "max_len", "pattern", "additional"
    };
    return keys.find(key) != keys.end();
}

inline bool schema_has_control_keys(GCDict* schema) {
    for (const auto& [key, _] : schema->elements)
        if (schema_is_control_key(key)) return true;
    return false;
}

inline void schema_error(std::vector<std::string>& errors, const std::string& path, const std::string& message) {
    errors.push_back(path + ": " + message);
}

inline std::string schema_child_path(const std::string& path, const std::string& key) {
    if (!key.empty() && key.find_first_of(" .[]'\"") == std::string::npos) return path + "." + key;
    return path + "[\"" + json_escape(key) + "\"]";
}

inline void schema_validate_into(const Value& value, const Value& schema,
                                 const std::string& path, std::vector<std::string>& errors) {
    if (schema.is_str()) {
        std::string want = schema.as_str();
        if (!schema_type_matches(value, want))
            schema_error(errors, path, "expected " + want + ", got " + schema_type_name(value));
        return;
    }

    if (!schema.is_dict()) {
        schema_error(errors, path, "schema must be a type string or dict");
        return;
    }

    auto* sd = schema.as_dict();
    if (!schema_has_control_keys(sd)) {
        if (!value.is_dict()) {
            schema_error(errors, path, "expected dict, got " + schema_type_name(value));
            return;
        }
        auto* vd = value.as_dict();
        for (const auto& [key, child_schema] : sd->elements) {
            auto it = vd->elements.find(key);
            std::string child_path = schema_child_path(path, key);
            if (it == vd->elements.end()) {
                schema_error(errors, child_path, "missing required field");
                continue;
            }
            schema_validate_into(it->second, child_schema, child_path, errors);
        }
        return;
    }

    if (const Value* enum_values = dict_get_ptr(sd, "enum")) {
        if (!enum_values->is_arr()) {
            schema_error(errors, path, "enum must be an array");
        } else {
            bool found = false;
            for (const auto& candidate : enum_values->as_arr()->elements) {
                if (schema_values_equal(value, candidate)) {
                    found = true;
                    break;
                }
            }
            if (!found) schema_error(errors, path, "value is not in enum");
        }
    }

    if (const Value* type_spec = dict_get_ptr(sd, "type")) {
        bool matched = false;
        std::string expected = "";
        if (type_spec->is_str()) {
            expected = type_spec->as_str();
            matched = schema_type_matches(value, expected);
        } else if (type_spec->is_arr()) {
            for (const auto& item : type_spec->as_arr()->elements) {
                if (!item.is_str()) continue;
                if (!expected.empty()) expected += "|";
                expected += item.as_str();
                if (schema_type_matches(value, item.as_str())) matched = true;
            }
        } else {
            schema_error(errors, path, "type must be a string or array of strings");
            matched = true;
        }
        if (!matched) schema_error(errors, path, "expected " + expected + ", got " + schema_type_name(value));
    }

    if (const Value* min_v = dict_get_ptr(sd, "min")) {
        if (!min_v->is_num()) schema_error(errors, path, "min must be a number");
        else if (!value.is_num()) schema_error(errors, path, "expected number for min check, got " + schema_type_name(value));
        else if (value.as_num() < min_v->as_num()) schema_error(errors, path, "number is below min " + min_v->to_str());
    }
    if (const Value* max_v = dict_get_ptr(sd, "max")) {
        if (!max_v->is_num()) schema_error(errors, path, "max must be a number");
        else if (!value.is_num()) schema_error(errors, path, "expected number for max check, got " + schema_type_name(value));
        else if (value.as_num() > max_v->as_num()) schema_error(errors, path, "number is above max " + max_v->to_str());
    }

    auto length_of = [](const Value& v, size_t* out) {
        if (v.is_str()) { *out = v.as_str_ref().size(); return true; }
        if (v.is_arr()) { *out = v.as_arr()->elements.size(); return true; }
        if (v.is_dict()) { *out = v.as_dict()->elements.size(); return true; }
        return false;
    };
    if (const Value* min_len = dict_get_ptr(sd, "min_len")) {
        size_t len = 0;
        if (!min_len->is_num()) schema_error(errors, path, "min_len must be a number");
        else if (!length_of(value, &len)) schema_error(errors, path, "expected string, array, or dict for min_len check");
        else if (len < (size_t)min_len->as_num()) schema_error(errors, path, "length is below min_len " + min_len->to_str());
    }
    if (const Value* max_len = dict_get_ptr(sd, "max_len")) {
        size_t len = 0;
        if (!max_len->is_num()) schema_error(errors, path, "max_len must be a number");
        else if (!length_of(value, &len)) schema_error(errors, path, "expected string, array, or dict for max_len check");
        else if (len > (size_t)max_len->as_num()) schema_error(errors, path, "length is above max_len " + max_len->to_str());
    }

    if (const Value* pattern = dict_get_ptr(sd, "pattern")) {
        if (!pattern->is_str()) {
            schema_error(errors, path, "pattern must be a string");
        } else if (!value.is_str()) {
            schema_error(errors, path, "expected string for pattern check, got " + schema_type_name(value));
        } else {
            try {
                if (!std::regex_search(value.as_str_ref(), std::regex(pattern->as_str_ref())))
                    schema_error(errors, path, "string does not match pattern");
            } catch (const std::regex_error& e) {
                schema_error(errors, path, std::string("invalid pattern: ") + e.what());
            }
        }
    }

    if (const Value* items = dict_get_ptr(sd, "items")) {
        if (!value.is_arr()) {
            schema_error(errors, path, "expected array for items check, got " + schema_type_name(value));
        } else {
            auto* arr = value.as_arr();
            for (size_t i = 0; i < arr->elements.size(); ++i)
                schema_validate_into(arr->elements[i], *items, path + "[" + std::to_string(i) + "]", errors);
        }
    }

    const Value* properties = dict_get_ptr(sd, "properties");
    const Value* required = dict_get_ptr(sd, "required");
    if (properties || required) {
        if (!value.is_dict()) {
            schema_error(errors, path, "expected dict for properties check, got " + schema_type_name(value));
        } else {
            auto* vd = value.as_dict();
            if (required) {
                if (!required->is_arr()) {
                    schema_error(errors, path, "required must be an array");
                } else {
                    for (const auto& item : required->as_arr()->elements) {
                        if (!item.is_str()) continue;
                        if (vd->elements.find(item.as_str()) == vd->elements.end())
                            schema_error(errors, schema_child_path(path, item.as_str()), "missing required field");
                    }
                }
            }
            if (properties) {
                if (!properties->is_dict()) {
                    schema_error(errors, path, "properties must be a dict");
                } else {
                    auto* pd = properties->as_dict();
                    for (const auto& [key, child_schema] : pd->elements) {
                        auto it = vd->elements.find(key);
                        if (it != vd->elements.end())
                            schema_validate_into(it->second, child_schema, schema_child_path(path, key), errors);
                    }
                    if (const Value* additional = dict_get_ptr(sd, "additional")) {
                        if (additional->is_bool() && !additional->as_bool()) {
                            for (const auto& [key, _] : vd->elements) {
                                if (pd->elements.find(key) == pd->elements.end())
                                    schema_error(errors, schema_child_path(path, key), "unexpected field");
                            }
                        }
                    }
                }
            }
        }
    }
}

inline Value b_schema_errors(const Value* a, int n, int l) {
    need_args("schema_errors", n, 2, 2, l);
    std::vector<std::string> errors;
    schema_validate_into(a[0], a[1], "$", errors);
    Value out = Value::make_array();
    for (const auto& error : errors) out.as_arr()->elements.push_back(Value(error));
    return out;
}

inline Value b_schema_validate(const Value* a, int n, int l) {
    need_args("schema_validate", n, 2, 2, l);
    std::vector<std::string> errors;
    schema_validate_into(a[0], a[1], "$", errors);
    return Value(errors.empty());
}

inline std::string schema_json_schema_type(std::string type_name) {
    std::transform(type_name.begin(), type_name.end(), type_name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (type_name == "any") return "";
    if (type_name == "num") return "number";
    if (type_name == "int") return "integer";
    if (type_name == "str") return "string";
    if (type_name == "bool") return "boolean";
    if (type_name == "dict") return "object";
    if (type_name == "list") return "array";
    if (type_name == "nil") return "null";
    return type_name;
}

inline Value schema_json_schema_type_value(const Value& type_spec) {
    if (type_spec.is_str()) {
        std::string mapped = schema_json_schema_type(type_spec.as_str());
        return mapped.empty() ? Value::nil() : Value(mapped);
    }
    if (type_spec.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : type_spec.as_arr()->elements) {
            if (!item.is_str()) continue;
            std::string mapped = schema_json_schema_type(item.as_str());
            if (!mapped.empty()) out.as_arr()->elements.push_back(Value(mapped));
        }
        return out.as_arr()->elements.empty() ? Value::nil() : out;
    }
    return type_spec;
}

inline bool schema_json_schema_handled_key(const std::string& key) {
    static const std::unordered_set<std::string> handled = {
        "type", "required", "properties", "items", "enum",
        "min", "max", "min_len", "max_len", "pattern", "additional"
    };
    return handled.find(key) != handled.end();
}

inline Value schema_to_json_schema_value(const Value& schema, bool strict) {
    if (schema.is_str()) {
        Value out = Value::make_dict();
        Value type_value = schema_json_schema_type_value(schema);
        if (!type_value.is_nil()) out.as_dict()->elements["type"] = type_value;
        return out;
    }

    if (!schema.is_dict()) return schema;

    auto* sd = schema.as_dict();
    Value out = Value::make_dict();
    auto* od = out.as_dict();
    bool has_controls = schema_has_control_keys(sd);

    if (!has_controls) {
        Value properties = Value::make_dict();
        Value required = Value::make_array();
        for (const auto& [key, child] : sd->elements) {
            properties.as_dict()->elements[key] = schema_to_json_schema_value(child, strict);
            required.as_arr()->elements.push_back(Value(key));
        }
        od->elements["type"] = Value(std::string("object"));
        od->elements["properties"] = properties;
        od->elements["required"] = required;
        if (strict) od->elements["additionalProperties"] = Value(false);
        return out;
    }

    for (const auto& [key, value] : sd->elements) {
        if (!schema_json_schema_handled_key(key)) od->elements[key] = value;
    }

    if (const Value* type_spec = dict_get_ptr(sd, "type")) {
        Value type_value = schema_json_schema_type_value(*type_spec);
        if (!type_value.is_nil()) od->elements["type"] = type_value;
    }
    if (const Value* enum_values = dict_get_ptr(sd, "enum")) od->elements["enum"] = *enum_values;
    if (const Value* pattern = dict_get_ptr(sd, "pattern")) od->elements["pattern"] = *pattern;
    if (const Value* min_v = dict_get_ptr(sd, "min")) od->elements["minimum"] = *min_v;
    if (const Value* max_v = dict_get_ptr(sd, "max")) od->elements["maximum"] = *max_v;
    if (const Value* min_len = dict_get_ptr(sd, "min_len")) od->elements["minLength"] = *min_len;
    if (const Value* max_len = dict_get_ptr(sd, "max_len")) od->elements["maxLength"] = *max_len;
    if (const Value* items = dict_get_ptr(sd, "items")) od->elements["items"] = schema_to_json_schema_value(*items, strict);

    const Value* properties_spec = dict_get_ptr(sd, "properties");
    if (properties_spec) {
        if (properties_spec->is_dict()) {
            Value properties = Value::make_dict();
            Value strict_required = Value::make_array();
            for (const auto& [key, child] : properties_spec->as_dict()->elements) {
                properties.as_dict()->elements[key] = schema_to_json_schema_value(child, strict);
                strict_required.as_arr()->elements.push_back(Value(key));
            }
            od->elements["properties"] = properties;
            if (!dict_get_ptr(sd, "required") && strict) od->elements["required"] = strict_required;
        } else {
            od->elements["properties"] = *properties_spec;
        }
        if (!dict_get_ptr(od, "type")) od->elements["type"] = Value(std::string("object"));
    }

    if (const Value* required = dict_get_ptr(sd, "required")) od->elements["required"] = *required;
    if (const Value* additional = dict_get_ptr(sd, "additional")) {
        od->elements["additionalProperties"] = *additional;
    } else if (strict && properties_spec && !dict_get_ptr(od, "additionalProperties")) {
        od->elements["additionalProperties"] = Value(false);
    }
    return out;
}

inline Value b_schema_to_json_schema(const Value* a, int n, int l) {
    need_args("schema_to_json_schema", n, 1, 2, l);
    if (!a[0].is_str() && !a[0].is_dict())
        throw JitThrow{"schema_to_json_schema(): schema must be a type string or dict", l};
    bool strict = n >= 2 ? a[1].truthy() : true;
    return schema_to_json_schema_value(a[0], strict);
}

inline std::unordered_map<std::string, std::shared_ptr<const std::regex>>& regex_cache() {
    static std::unordered_map<std::string, std::shared_ptr<const std::regex>> cache;
    return cache;
}

inline std::mutex& regex_cache_mutex() {
    static std::mutex m;
    return m;
}

inline std::shared_ptr<const std::regex> cached_regex(const std::string& pattern) {
    {
        std::lock_guard<std::mutex> lock(regex_cache_mutex());
        auto it = regex_cache().find(pattern);
        if (it != regex_cache().end()) return it->second;
    }

    auto compiled = std::make_shared<const std::regex>(pattern);
    std::lock_guard<std::mutex> lock(regex_cache_mutex());
    auto& cache = regex_cache();
    auto it = cache.find(pattern);
    if (it != cache.end()) return it->second;
    if (cache.size() >= 256) cache.clear();
    cache.emplace(pattern, compiled);
    return compiled;
}

inline Value b_regex_match(const Value* a, int n, int l) {
    need_args("regex_match", n, 2, 2, l);
    try {
        auto re = cached_regex(need_str("regex_match", a[1], 1, l));
        return Value(std::regex_search(need_str("regex_match", a[0], 0, l),
                                       *re));
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_match(): ") + e.what(), l};
    }
}

inline Value b_regex_replace(const Value* a, int n, int l) {
    need_args("regex_replace", n, 3, 3, l);
    try {
        auto re = cached_regex(need_str("regex_replace", a[1], 1, l));
        return Value(std::regex_replace(need_str("regex_replace", a[0], 0, l),
                                        *re,
                                        need_str("regex_replace", a[2], 2, l)));
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_replace(): ") + e.what(), l};
    }
}

inline Value b_regex_find_all(const Value* a, int n, int l) {
    need_args("regex_find_all", n, 2, 2, l);
    try {
        std::string text = need_str("regex_find_all", a[0], 0, l);
        auto re = cached_regex(need_str("regex_find_all", a[1], 1, l));
        Value out = Value::make_array();
        for (std::sregex_iterator it(text.begin(), text.end(), *re), end; it != end; ++it)
            out.as_arr()->elements.push_back(Value(it->str()));
        return out;
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_find_all(): ") + e.what(), l};
    }
}

inline Value b_regex_escape(const Value* a, int n, int l) {
    need_args("regex_escape", n, 1, 1, l);
    std::string text = need_str("regex_escape", a[0], 0, l);
    const std::string specials = "\\.^$|()[]{}*+?";
    std::string out;
    out.reserve(text.size() * 2);
    for (char ch : text) {
        if (specials.find(ch) != std::string::npos) out.push_back('\\');
        out.push_back(ch);
    }
    return Value(out);
}

inline Value regex_match_array(const std::smatch& match) {
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(match.size());
    for (size_t i = 0; i < match.size(); ++i) {
        arr->elements.push_back(match[i].matched ? Value(match[i].str()) : Value::nil());
    }
    return out;
}

inline Value b_regex_capture(const Value* a, int n, int l) {
    need_args("regex_capture", n, 2, 2, l);
    try {
        std::string text = need_str("regex_capture", a[0], 0, l);
        auto re = cached_regex(need_str("regex_capture", a[1], 1, l));
        std::smatch match;
        if (!std::regex_search(text, match, *re)) return Value::nil();
        return regex_match_array(match);
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_capture(): ") + e.what(), l};
    }
}

inline Value b_regex_captures(const Value* a, int n, int l) {
    need_args("regex_captures", n, 2, 2, l);
    try {
        std::string text = need_str("regex_captures", a[0], 0, l);
        auto re = cached_regex(need_str("regex_captures", a[1], 1, l));
        Value out = Value::make_array();
        auto* rows = out.as_arr();
        for (std::sregex_iterator it(text.begin(), text.end(), *re), end; it != end; ++it) {
            rows->elements.push_back(regex_match_array(*it));
        }
        return out;
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_captures(): ") + e.what(), l};
    }
}

inline Value b_regex_split(const Value* a, int n, int l) {
    need_args("regex_split", n, 2, 2, l);
    try {
        std::string text = need_str("regex_split", a[0], 0, l);
        auto re = cached_regex(need_str("regex_split", a[1], 1, l));
        Value out = Value::make_array();
        for (std::sregex_token_iterator it(text.begin(), text.end(), *re, -1), end; it != end; ++it)
            out.as_arr()->elements.push_back(Value(it->str()));
        return out;
    } catch (const std::regex_error& e) {
        throw JitThrow{std::string("regex_split(): ") + e.what(), l};
    }
}

inline Value b_datetime_now(const Value*, int, int) {
    auto now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return Value(std::string(buf));
}

inline Value b_datetime_format(const Value* a, int n, int l) {
    need_args("datetime_format", n, 2, 2, l);
    std::time_t t = (std::time_t)need_num("datetime_format", a[0], 0, l);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[256];
    std::string fmt = need_str("datetime_format", a[1], 1, l);
    if (!std::strftime(buf, sizeof(buf), fmt.c_str(), &tmv)) return Value(std::string(""));
    return Value(std::string(buf));
}

inline Value b_datetime_utc_format(const Value* a, int n, int l) {
    need_args("datetime_utc_format", n, 2, 2, l);
    std::time_t t = (std::time_t)need_num("datetime_utc_format", a[0], 0, l);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[256];
    std::string fmt = need_str("datetime_utc_format", a[1], 1, l);
    if (!std::strftime(buf, sizeof(buf), fmt.c_str(), &tmv)) return Value(std::string(""));
    return Value(std::string(buf));
}

inline Value b_datetime_parse(const Value* a, int n, int l) {
    need_args("datetime_parse", n, 1, 2, l);
    std::string text = need_str("datetime_parse", a[0], 0, l);
    std::string fmt = n >= 2 ? need_str("datetime_parse", a[1], 1, l)
                             : std::string("%Y-%m-%dT%H:%M:%S");
    std::tm tmv{};
    tmv.tm_isdst = -1;
    std::istringstream in(text);
    in >> std::get_time(&tmv, fmt.c_str());
    if (in.fail()) {
        throw JitThrow{"datetime_parse(): input does not match format", l};
    }
    std::time_t parsed = std::mktime(&tmv);
    if (parsed == (std::time_t)-1) {
        throw JitThrow{"datetime_parse(): parsed time is out of range", l};
    }
    return Value((double)parsed);
}

inline Value b_datetime_parts(const Value* a, int n, int l) {
    need_args("datetime_parts", n, 1, 2, l);
    std::time_t t = (std::time_t)need_num("datetime_parts", a[0], 0, l);
    bool utc = n >= 2 && a[1].truthy();
    std::tm tmv{};
#ifdef _WIN32
    if (utc) gmtime_s(&tmv, &t);
    else localtime_s(&tmv, &t);
#else
    if (utc) gmtime_r(&t, &tmv);
    else localtime_r(&t, &tmv);
#endif
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["year"] = Value((double)tmv.tm_year + 1900);
    d->elements["month"] = Value((double)tmv.tm_mon + 1);
    d->elements["day"] = Value((double)tmv.tm_mday);
    d->elements["hour"] = Value((double)tmv.tm_hour);
    d->elements["minute"] = Value((double)tmv.tm_min);
    d->elements["second"] = Value((double)tmv.tm_sec);
    d->elements["weekday"] = Value((double)tmv.tm_wday);
    d->elements["yearday"] = Value((double)tmv.tm_yday + 1);
    d->elements["is_dst"] = Value((double)tmv.tm_isdst);
    d->elements["utc"] = Value(utc);
    return out;
}

inline Value b_datetime_add(const Value* a, int n, int l) {
    need_args("datetime_add", n, 2, 2, l);
    return Value(need_num("datetime_add", a[0], 0, l) + need_num("datetime_add", a[1], 1, l));
}

inline Value b_datetime_diff(const Value* a, int n, int l) {
    need_args("datetime_diff", n, 2, 2, l);
    return Value(need_num("datetime_diff", a[0], 0, l) - need_num("datetime_diff", a[1], 1, l));
}

inline Value b_timestamp(const Value*, int, int) {
    return Value((double)std::time(nullptr));
}

inline std::string env_trim_ascii(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline bool env_valid_name(const std::string& name) {
    if (name.empty()) return false;
    unsigned char first = (unsigned char)name[0];
    if (!(std::isalpha(first) || name[0] == '_')) return false;
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (!(std::isalnum(c) || ch == '_')) return false;
    }
    return true;
}

inline bool env_set_process(const std::string& name, const std::string& value) {
#ifdef _WIN32
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

inline Value b_env_get(const Value* a, int n, int l) {
    need_args("env_get", n, 1, 2, l);
    std::string name = need_str("env_get", a[0], 0, l);
    const char* value = std::getenv(name.c_str());
    if (value) return Value(std::string(value));
    return n >= 2 ? a[1] : Value::nil();
}

inline Value b_env_require(const Value* a, int n, int l) {
    need_args("env_require", n, 1, 1, l);
    std::string name = need_str("env_require", a[0], 0, l);
    const char* value = std::getenv(name.c_str());
    if (!value) throw JitThrow{"env_require(): missing environment variable '" + name + "'", l};
    return Value(std::string(value));
}

inline Value b_env_set(const Value* a, int n, int l) {
    need_args("env_set", n, 2, 2, l);
    std::string name = need_str("env_set", a[0], 0, l);
    std::string value = need_str("env_set", a[1], 1, l);
    if (!env_valid_name(name)) throw JitThrow{"env_set(): invalid environment variable name '" + name + "'", l};
    return Value(env_set_process(name, value));
}

inline std::string env_unescape_double_quoted(const std::string& text, size_t& pos, int line) {
    std::string out;
    pos++;
    while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '"') return out;
        if (ch == '\\') {
            if (pos >= text.size()) throw JitThrow{"env_load(): unfinished escape in quoted value", line};
            char escaped = text[pos++];
            switch (escaped) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += escaped; break;
            }
        } else {
            out += ch;
        }
    }
    throw JitThrow{"env_load(): unterminated quoted value", line};
}

inline std::string env_unescape_single_quoted(const std::string& text, size_t& pos, int line) {
    std::string out;
    pos++;
    while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '\'') return out;
        out += ch;
    }
    throw JitThrow{"env_load(): unterminated quoted value", line};
}

inline std::string env_parse_value(std::string raw, int line) {
    raw = env_trim_ascii(raw);
    if (raw.empty()) return "";
    if (raw[0] == '"') {
        size_t pos = 0;
        std::string value = env_unescape_double_quoted(raw, pos, line);
        std::string tail = env_trim_ascii(raw.substr(pos));
        if (!tail.empty() && tail[0] != '#') throw JitThrow{"env_load(): unexpected text after quoted value", line};
        return value;
    }
    if (raw[0] == '\'') {
        size_t pos = 0;
        std::string value = env_unescape_single_quoted(raw, pos, line);
        std::string tail = env_trim_ascii(raw.substr(pos));
        if (!tail.empty() && tail[0] != '#') throw JitThrow{"env_load(): unexpected text after quoted value", line};
        return value;
    }

    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' && (i == 0 || std::isspace((unsigned char)raw[i - 1]))) {
            raw = raw.substr(0, i);
            break;
        }
    }
    return env_trim_ascii(raw);
}

inline Value b_env_load(const Value* a, int n, int l) {
    need_args("env_load", n, 1, 2, l);
    std::string path = need_str("env_load", a[0], 0, l);
    bool override_existing = n >= 2 ? a[1].truthy() : false;
    std::string text = read_text_file(path, l);
    if (text.size() >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text = text.substr(3);
    }

    Value loaded = Value::make_dict();
    std::istringstream in(text);
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string work = env_trim_ascii(line);
        if (work.empty() || work[0] == '#') continue;
        if (work.rfind("export ", 0) == 0) work = env_trim_ascii(work.substr(7));
        size_t eq = work.find('=');
        if (eq == std::string::npos) throw JitThrow{"env_load(): expected KEY=value at line " + std::to_string(line_no), l};
        std::string name = env_trim_ascii(work.substr(0, eq));
        if (!env_valid_name(name)) throw JitThrow{"env_load(): invalid environment variable name '" + name + "'", l};
        std::string value = env_parse_value(work.substr(eq + 1), l);
        if (!override_existing && std::getenv(name.c_str())) continue;
        if (!env_set_process(name, value)) throw JitThrow{"env_load(): failed to set '" + name + "'", l};
        loaded.as_dict()->elements[name] = Value(value);
    }
    return loaded;
}

inline std::string sha256_hex_bytes(const std::string& text, int line) {
    auto tmp = std::filesystem::temp_directory_path() /
        ("sura_sha256_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
#ifdef _WIN32
    std::string cmd = "certutil -hashfile \"" + tmp.string() + "\" SHA256";
#else
    std::string cmd = "sha256sum \"" + tmp.string() + "\" 2>/dev/null || shasum -a 256 \"" + tmp.string() + "\"";
#endif
    std::string raw = run_capture_command(cmd);
    std::filesystem::remove(tmp);
    std::regex hex_re("([A-Fa-f0-9]{64})");
    std::smatch m;
    if (std::regex_search(raw, m, hex_re)) {
        std::string h = m[1].str();
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return h;
    }
    throw JitThrow{"sha256(): system SHA256 tool failed", line};
}

inline Value b_sha256(const Value* a, int n, int l) {
    need_args("sha256", n, 1, 1, l);
    return Value(sha256_hex_bytes(need_str("sha256", a[0], 0, l), l));
}

inline Value b_file_sha256(const Value* a, int n, int l) {
    need_args("file_sha256", n, 1, 1, l);
    std::string path = need_str("file_sha256", a[0], 0, l);
    return Value(sha256_hex_bytes(read_binary_file_for(path, l, "file_sha256"), l));
}

inline std::vector<unsigned char> crypto_random_bytes_vec(const char* name, long long count, int line) {
    if (count < 0) throw JitThrow{std::string(name) + "(): count must be non-negative", line};
    if (count > 1048576) throw JitThrow{std::string(name) + "(): count exceeds 1048576", line};
    std::vector<unsigned char> out;
    out.reserve((size_t)count);
    try {
        std::random_device rd;
        while ((long long)out.size() < count) {
            unsigned int word = rd();
            for (int shift = 0; shift < 32 && (long long)out.size() < count; shift += 8) {
                out.push_back((unsigned char)((word >> shift) & 0xff));
            }
        }
    } catch (const std::exception& e) {
        throw JitThrow{std::string(name) + "(): OS random source failed: " + e.what(), line};
    }
    return out;
}

inline Value b_crypto_random_bytes(const Value* a, int n, int l) {
    need_args("crypto_random_bytes", n, 1, 1, l);
    long long count = random_integral_arg("crypto_random_bytes", a[0], 0, l);
    auto bytes = crypto_random_bytes_vec("crypto_random_bytes", count, l);
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(bytes.size());
    for (unsigned char byte : bytes) arr->elements.push_back(Value((double)byte));
    return out;
}

inline Value b_crypto_random_hex(const Value* a, int n, int l) {
    need_args("crypto_random_hex", n, 1, 1, l);
    long long count = random_integral_arg("crypto_random_hex", a[0], 0, l);
    auto bytes = crypto_random_bytes_vec("crypto_random_hex", count, l);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(hex[(byte >> 4) & 0xf]);
        out.push_back(hex[byte & 0xf]);
    }
    return Value(out);
}

inline bool constant_time_equal_bytes(const std::string& left, const std::string& right) {
    size_t max_len = std::max(left.size(), right.size());
    size_t size_diff = left.size() ^ right.size();
    unsigned char diff = 0;
    for (size_t i = 0; i < max_len; ++i) {
        unsigned char l = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        unsigned char r = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        diff |= static_cast<unsigned char>(l ^ r);
    }
    return size_diff == 0 && diff == 0;
}

inline Value b_constant_time_eq(const Value* a, int n, int l) {
    need_args("constant_time_eq", n, 2, 2, l);
    std::string left = need_str("constant_time_eq", a[0], 0, l);
    std::string right = need_str("constant_time_eq", a[1], 1, l);
    return Value(constant_time_equal_bytes(left, right));
}

inline unsigned char hex_to_byte_nibble(char ch, int line, const char* name) {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned char>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned char>(10 + ch - 'A');
    throw JitThrow{std::string(name) + "(): invalid hex input", line};
}

inline std::string bytes_from_hex(const std::string& hex, int line, const char* name) {
    if (hex.size() % 2 != 0) throw JitThrow{std::string(name) + "(): hex input must have an even length", line};
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned char hi = hex_to_byte_nibble(hex[i], line, name);
        unsigned char lo = hex_to_byte_nibble(hex[i + 1], line, name);
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

inline std::string hmac_sha256_hex_bytes(const std::string& key_in, const std::string& message, int line, const char* name) {
    std::string key = key_in;
    constexpr size_t block_size = 64;
    if (key.size() > block_size) key = bytes_from_hex(sha256_hex_bytes(key, line), line, name);
    key.resize(block_size, '\0');
    std::string outer_pad(block_size, '\0');
    std::string inner_pad(block_size, '\0');
    for (size_t i = 0; i < block_size; ++i) {
        unsigned char byte = static_cast<unsigned char>(key[i]);
        outer_pad[i] = static_cast<char>(byte ^ 0x5c);
        inner_pad[i] = static_cast<char>(byte ^ 0x36);
    }
    std::string inner_digest = bytes_from_hex(sha256_hex_bytes(inner_pad + message, line), line, name);
    return sha256_hex_bytes(outer_pad + inner_digest, line);
}

inline Value b_hmac_sha256(const Value* a, int n, int l) {
    need_args("hmac_sha256", n, 2, 2, l);
    std::string key = need_str("hmac_sha256", a[0], 0, l);
    std::string message = need_str("hmac_sha256", a[1], 1, l);
    return Value(hmac_sha256_hex_bytes(key, message, l, "hmac_sha256"));
}

inline Value b_file_hmac_sha256(const Value* a, int n, int l) {
    need_args("file_hmac_sha256", n, 2, 2, l);
    std::string key = need_str("file_hmac_sha256", a[0], 0, l);
    std::string path = need_str("file_hmac_sha256", a[1], 1, l);
    return Value(hmac_sha256_hex_bytes(key, read_binary_file_for(path, l, "file_hmac_sha256"), l, "file_hmac_sha256"));
}

inline Value b_hex_encode(const Value* a, int n, int l) {
    need_args("hex_encode", n, 1, 1, l);
    static const char* hex = "0123456789abcdef";
    std::string s = need_str("hex_encode", a[0], 0, l);
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(hex[(c >> 4) & 0xf]);
        out.push_back(hex[c & 0xf]);
    }
    return Value(out);
}

inline Value b_hex_decode(const Value* a, int n, int l) {
    need_args("hex_decode", n, 1, 1, l);
    return Value(bytes_from_hex(need_str("hex_decode", a[0], 0, l), l, "hex_decode"));
}

inline std::string base64_encode_text(const std::string& s) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : s) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3f]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3f]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline Value b_base64_encode(const Value* a, int n, int l) {
    need_args("base64_encode", n, 1, 1, l);
    std::string s = need_str("base64_encode", a[0], 0, l);
    std::string out = base64_encode_text(s);
    return Value(out);
}

inline int base64_decode_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return 26 + ch - 'a';
    if (ch >= '0' && ch <= '9') return 52 + ch - '0';
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

inline std::string base64_decode_text(const std::string& s, int l, const char* name) {
    std::string clean;
    clean.reserve(s.size());
    for (unsigned char c : s) {
        if (!std::isspace(c)) clean.push_back(static_cast<char>(c));
    }
    if (clean.size() % 4 == 1) throw JitThrow{std::string(name) + "(): invalid input length", l};
    bool seen_padding = false;
    int padding = 0;
    int val = 0;
    int valb = -8;
    std::string out;
    for (char ch : clean) {
        if (ch == '=') {
            seen_padding = true;
            if (++padding > 2) throw JitThrow{std::string(name) + "(): invalid padding", l};
            continue;
        }
        if (seen_padding) throw JitThrow{std::string(name) + "(): invalid padding", l};
        int decoded = base64_decode_value(ch);
        if (decoded < 0) throw JitThrow{std::string(name) + "(): invalid character", l};
        val = (val << 6) | decoded;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    if (padding > 0 && clean.size() % 4 != 0) throw JitThrow{std::string(name) + "(): padded input length must be a multiple of 4", l};
    return out;
}

inline Value b_base64_decode(const Value* a, int n, int l) {
    need_args("base64_decode", n, 1, 1, l);
    return Value(base64_decode_text(need_str("base64_decode", a[0], 0, l), l, "base64_decode"));
}

inline Value b_base64_url_encode(const Value* a, int n, int l) {
    need_args("base64_url_encode", n, 1, 1, l);
    std::string out = base64_encode_text(need_str("base64_url_encode", a[0], 0, l));
    for (char& ch : out) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return Value(out);
}

inline Value b_base64_url_decode(const Value* a, int n, int l) {
    need_args("base64_url_decode", n, 1, 1, l);
    std::string s = need_str("base64_url_decode", a[0], 0, l);
    std::string normalized;
    normalized.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (std::isspace(c)) continue;
        if (c == '-') normalized.push_back('+');
        else if (c == '_') normalized.push_back('/');
        else normalized.push_back(static_cast<char>(c));
    }
    if (normalized.size() % 4 == 1) throw JitThrow{"base64_url_decode(): invalid input length", l};
    while (normalized.size() % 4) normalized.push_back('=');
    return Value(base64_decode_text(normalized, l, "base64_url_decode"));
}

inline bool url_unreserved_byte(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

inline int url_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

inline std::string url_encode_text(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (url_unreserved_byte(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

inline Value b_url_encode(const Value* a, int n, int l) {
    need_args("url_encode", n, 1, 1, l);
    return Value(url_encode_text(need_str("url_encode", a[0], 0, l)));
}

inline std::string form_encode_text(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c == ' ') {
            out.push_back('+');
        } else if (url_unreserved_byte(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

inline std::string url_percent_decode_text(const std::string& s, int l, const char* name) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 2 >= s.size()) throw JitThrow{std::string(name) + "(): incomplete percent escape", l};
        int hi = url_hex_value(s[i + 1]);
        int lo = url_hex_value(s[i + 2]);
        if (hi < 0 || lo < 0) throw JitThrow{std::string(name) + "(): invalid percent escape", l};
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

inline Value b_url_decode(const Value* a, int n, int l) {
    need_args("url_decode", n, 1, 1, l);
    return Value(url_percent_decode_text(need_str("url_decode", a[0], 0, l), l, "url_decode"));
}

inline std::string url_decode_component(const std::string& s, int line, const char* name) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%') {
            if (i + 2 >= s.size()) throw JitThrow{std::string(name) + "(): incomplete percent escape", line};
            int hi = url_hex_value(s[i + 1]);
            int lo = url_hex_value(s[i + 2]);
            if (hi < 0 || lo < 0) throw JitThrow{std::string(name) + "(): invalid percent escape", line};
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::string query_scalar_string(const Value& v, int line, const char* name) {
    if (v.is_arr() || v.is_dict()) throw JitThrow{std::string(name) + "(): nested arrays or dicts are not supported as query values", line};
    if (v.is_nil()) return "";
    return v.to_str();
}

inline void query_append_pair(std::string& out, const std::string& key, const Value& value, int line, const char* name) {
    if (!out.empty()) out.push_back('&');
    out += url_encode_text(key);
    out.push_back('=');
    out += url_encode_text(query_scalar_string(value, line, name));
}

inline std::string query_build_from_dict(GCDict* d, int l, const char* name) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : d->elements) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (const auto& key : keys) {
        const Value& value = d->elements.at(key);
        if (value.is_arr()) {
            for (const auto& item : value.as_arr()->elements) query_append_pair(out, key, item, l, name);
        } else {
            query_append_pair(out, key, value, l, name);
        }
    }
    return out;
}

inline Value b_query_build(const Value* a, int n, int l) {
    need_args("query_build", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"query_build(): expected dict", l};
    std::string out = query_build_from_dict(a[0].as_dict(), l, "query_build");
    return Value(out);
}

inline std::string form_scalar_string(const Value& v, int line, const char* name) {
    if (v.is_arr() || v.is_dict()) throw JitThrow{std::string(name) + "(): nested arrays or dicts are not supported as form values", line};
    if (v.is_nil()) return "";
    return v.to_str();
}

inline void form_append_pair(std::string& out, const std::string& key, const Value& value, int line, const char* name) {
    if (!out.empty()) out.push_back('&');
    out += form_encode_text(key);
    out.push_back('=');
    out += form_encode_text(form_scalar_string(value, line, name));
}

inline std::string form_build_from_dict(GCDict* d, int l, const char* name) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : d->elements) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (const auto& key : keys) {
        const Value& value = d->elements.at(key);
        if (value.is_arr()) {
            for (const auto& item : value.as_arr()->elements) form_append_pair(out, key, item, l, name);
        } else {
            form_append_pair(out, key, value, l, name);
        }
    }
    return out;
}

inline Value b_form_build(const Value* a, int n, int l) {
    need_args("form_build", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"form_build(): expected dict", l};
    return Value(form_build_from_dict(a[0].as_dict(), l, "form_build"));
}

inline std::string url_with_query_from_spec(std::string url, GCDict* spec, const char* fn, int l) {
    const Value* query_value = dict_get_ptr(spec, "query");
    if (!query_value) return url;
    if (!query_value->is_dict()) throw JitThrow{std::string(fn) + "(): query must be a dict", l};
    std::string query = query_build_from_dict(query_value->as_dict(), l, fn);
    if (query.empty()) return url;
    if (url.rfind("file://", 0) == 0) throw JitThrow{std::string(fn) + "(): file:// URLs do not support query", l};

    size_t hash = url.find('#');
    std::string base = hash == std::string::npos ? url : url.substr(0, hash);
    std::string fragment = hash == std::string::npos ? "" : url.substr(hash);
    if (!base.empty() && base.back() != '?' && base.back() != '&') {
        base.push_back(base.find('?') == std::string::npos ? '?' : '&');
    }
    return base + query + fragment;
}

inline void query_add_value(GCDict* d, const std::string& key, const std::string& value) {
    auto it = d->elements.find(key);
    if (it == d->elements.end()) {
        d->elements[key] = Value(value);
        return;
    }
    if (!it->second.is_arr()) {
        Value arr = Value::make_array();
        arr.as_arr()->elements.push_back(it->second);
        it->second = arr;
    }
    it->second.as_arr()->elements.push_back(Value(value));
}

inline Value query_parse_text(std::string input, int l, const char* name) {
    size_t question = input.find('?');
    if (question != std::string::npos) input = input.substr(question + 1);
    size_t hash = input.find('#');
    if (hash != std::string::npos) input = input.substr(0, hash);

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    size_t start = 0;
    while (start <= input.size()) {
        size_t amp = input.find('&', start);
        bool last = amp == std::string::npos;
        std::string pair = last ? input.substr(start) : input.substr(start, amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string raw_key = eq == std::string::npos ? pair : pair.substr(0, eq);
            std::string raw_value = eq == std::string::npos ? "" : pair.substr(eq + 1);
            std::string key = url_decode_component(raw_key, l, name);
            std::string value = url_decode_component(raw_value, l, name);
            query_add_value(d, key, value);
        }
        if (last) break;
        start = amp + 1;
    }
    return out;
}

inline Value b_query_parse(const Value* a, int n, int l) {
    need_args("query_parse", n, 1, 1, l);
    return query_parse_text(need_str("query_parse", a[0], 0, l), l, "query_parse");
}

inline Value form_parse_text(std::string input, int l, const char* name) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    size_t start = 0;
    while (start <= input.size()) {
        size_t amp = input.find('&', start);
        bool last = amp == std::string::npos;
        std::string pair = last ? input.substr(start) : input.substr(start, amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string raw_key = eq == std::string::npos ? pair : pair.substr(0, eq);
            std::string raw_value = eq == std::string::npos ? "" : pair.substr(eq + 1);
            std::string key = url_decode_component(raw_key, l, name);
            std::string value = url_decode_component(raw_value, l, name);
            query_add_value(d, key, value);
        }
        if (last) break;
        start = amp + 1;
    }
    return out;
}

inline Value b_form_parse(const Value* a, int n, int l) {
    need_args("form_parse", n, 1, 1, l);
    return form_parse_text(need_str("form_parse", a[0], 0, l), l, "form_parse");
}

inline bool url_valid_scheme(const std::string& scheme) {
    if (scheme.empty() || !std::isalpha((unsigned char)scheme[0])) return false;
    for (unsigned char ch : scheme) {
        if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.') return false;
    }
    return true;
}

inline bool url_ascii_digits(const std::string& text) {
    if (text.empty()) return false;
    for (unsigned char ch : text) if (!std::isdigit(ch)) return false;
    return true;
}

inline Value b_url_parse(const Value* a, int n, int l) {
    need_args("url_parse", n, 1, 1, l);
    std::string input = need_str("url_parse", a[0], 0, l);
    std::string rest = input;
    std::string scheme, authority, userinfo, host, port_text, path, query, fragment;
    bool has_authority = false;

    size_t hash = rest.find('#');
    if (hash != std::string::npos) {
        fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
    }
    size_t question = rest.find('?');
    if (question != std::string::npos) {
        query = rest.substr(question + 1);
        rest = rest.substr(0, question);
    }
    size_t colon = rest.find(':');
    size_t first_slash = rest.find('/');
    if (colon != std::string::npos && (first_slash == std::string::npos || colon < first_slash)) {
        std::string candidate = rest.substr(0, colon);
        if (url_valid_scheme(candidate)) {
            scheme = candidate;
            rest = rest.substr(colon + 1);
        }
    }

    if (rest.rfind("//", 0) == 0) {
        has_authority = true;
        rest = rest.substr(2);
        size_t slash = rest.find('/');
        authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        path = slash == std::string::npos ? "" : rest.substr(slash);

        std::string hostport = authority;
        size_t at = hostport.rfind('@');
        if (at != std::string::npos) {
            userinfo = hostport.substr(0, at);
            hostport = hostport.substr(at + 1);
        }

        if (!hostport.empty() && hostport[0] == '[') {
            size_t close = hostport.find(']');
            if (close != std::string::npos) {
                host = hostport.substr(0, close + 1);
                if (close + 1 < hostport.size() && hostport[close + 1] == ':') port_text = hostport.substr(close + 2);
            } else {
                host = hostport;
            }
        } else {
            size_t last_colon = hostport.rfind(':');
            if (last_colon != std::string::npos &&
                hostport.find(':') == last_colon &&
                url_ascii_digits(hostport.substr(last_colon + 1))) {
                host = hostport.substr(0, last_colon);
                port_text = hostport.substr(last_colon + 1);
            } else {
                host = hostport;
            }
        }
    } else {
        path = rest;
    }
    long long port_number = -1;
    if (!port_text.empty()) {
        if (!url_ascii_digits(port_text)) throw JitThrow{"url_parse(): port must contain digits only", l};
        port_number = 0;
        for (char ch : port_text) {
            port_number = port_number * 10 + (ch - '0');
            if (port_number > 65535) throw JitThrow{"url_parse(): port must be from 0 to 65535", l};
        }
    }

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["url"] = Value(input);
    d->elements["scheme"] = Value(scheme);
    d->elements["has_authority"] = Value(has_authority);
    d->elements["authority"] = Value(authority);
    d->elements["userinfo"] = Value(userinfo);
    d->elements["host"] = Value(host);
    d->elements["port"] = port_text.empty() ? Value() : Value((double)port_number);
    d->elements["path"] = Value(path);
    d->elements["query"] = Value(query);
    d->elements["params"] = query_parse_text(query, l, "url_parse");
    d->elements["fragment"] = Value(fragment);
    if (!scheme.empty() && !host.empty()) {
        std::string origin = scheme + "://" + host;
        if (!port_text.empty()) origin += ":" + port_text;
        d->elements["origin"] = Value(origin);
    } else {
        d->elements["origin"] = Value("");
    }
    return out;
}

inline std::string url_part_string(GCDict* d, const std::string& key, const char* name, int l) {
    const Value* value = dict_get_ptr(d, key);
    if (!value || value->is_nil()) return "";
    if (!value->is_str()) throw JitThrow{std::string(name) + "(): " + key + " must be a string", l};
    return value->as_str();
}

inline std::string url_port_string(GCDict* d, const char* name, int l) {
    const Value* value = dict_get_ptr(d, "port");
    if (!value || value->is_nil()) return "";
    if (value->is_num()) {
        double raw = value->as_num();
        if (raw < 0 || raw > 65535 || std::floor(raw) != raw) throw JitThrow{std::string(name) + "(): port must be an integer from 0 to 65535", l};
        return std::to_string((long long)raw);
    }
    if (value->is_str()) {
        std::string text = value->as_str();
        if (!text.empty() && !url_ascii_digits(text)) throw JitThrow{std::string(name) + "(): port string must contain digits only", l};
        long long parsed = 0;
        for (char ch : text) {
            parsed = parsed * 10 + (ch - '0');
            if (parsed > 65535) throw JitThrow{std::string(name) + "(): port must be an integer from 0 to 65535", l};
        }
        return text;
    }
    throw JitThrow{std::string(name) + "(): port must be a number or string", l};
}

inline Value b_url_build(const Value* a, int n, int l) {
    need_args("url_build", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"url_build(): expected dict", l};
    auto* d = a[0].as_dict();
    std::string scheme = url_part_string(d, "scheme", "url_build", l);
    if (!scheme.empty()) {
        if (scheme.back() == ':') scheme.pop_back();
        if (!url_valid_scheme(scheme)) throw JitThrow{"url_build(): invalid scheme", l};
    }

    std::string authority = url_part_string(d, "authority", "url_build", l);
    if (authority.empty()) {
        std::string host = url_part_string(d, "host", "url_build", l);
        if (!host.empty()) {
            std::string userinfo = url_part_string(d, "userinfo", "url_build", l);
            authority = userinfo.empty() ? host : userinfo + "@" + host;
            std::string port = url_port_string(d, "url_build", l);
            if (!port.empty()) authority += ":" + port;
        }
    }

    std::string path = url_part_string(d, "path", "url_build", l);
    std::string out;
    bool force_authority = false;
    if (const Value* value = dict_get_ptr(d, "has_authority")) force_authority = value->truthy();
    if (!authority.empty() || force_authority) {
        if (!scheme.empty()) out += scheme + ":";
        out += "//" + authority;
        if (!path.empty() && path[0] != '/') out.push_back('/');
    } else if (!scheme.empty()) {
        out += scheme + ":";
    }
    out += path;

    std::string query = url_part_string(d, "query", "url_build", l);
    if (!query.empty() && query[0] == '?') query = query.substr(1);
    if (query.empty()) {
        if (const Value* params = dict_get_ptr(d, "params")) {
            if (!params->is_nil()) {
                if (!params->is_dict()) throw JitThrow{"url_build(): params must be a dict", l};
                query = query_build_from_dict(params->as_dict(), l, "url_build");
            }
        }
    }
    if (!query.empty()) out += "?" + query;

    std::string fragment = url_part_string(d, "fragment", "url_build", l);
    if (!fragment.empty() && fragment[0] == '#') fragment = fragment.substr(1);
    if (!fragment.empty()) out += "#" + fragment;
    return Value(out);
}

inline std::vector<std::string> cli_tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quote = false;
    char quote = 0;
    bool escaped = false;
    bool had_token = false;
    for (char ch : text) {
        if (escaped) {
            cur.push_back(ch);
            had_token = true;
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            had_token = true;
            continue;
        }
        if (in_quote) {
            if (ch == quote) {
                in_quote = false;
            } else {
                cur.push_back(ch);
            }
            had_token = true;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_quote = true;
            quote = ch;
            had_token = true;
            continue;
        }
        if (std::isspace((unsigned char)ch)) {
            if (had_token) {
                tokens.push_back(cur);
                cur.clear();
                had_token = false;
            }
            continue;
        }
        cur.push_back(ch);
        had_token = true;
    }
    if (escaped) cur.push_back('\\');
    if (had_token) tokens.push_back(cur);
    return tokens;
}

inline bool cli_is_value_token(const std::string& token) {
    if (token.empty() || token == "-") return true;
    if (token.size() >= 2 && token[0] == '-' && (std::isdigit((unsigned char)token[1]) || token[1] == '.')) return true;
    return !(token.size() >= 2 && token[0] == '-');
}

inline void cli_add_value(GCDict* d, const std::string& key, const Value& value) {
    auto it = d->elements.find(key);
    if (it == d->elements.end()) {
        d->elements[key] = value;
        return;
    }
    if (!it->second.is_arr()) {
        Value arr = Value::make_array();
        arr.as_arr()->elements.push_back(it->second);
        it->second = arr;
    }
    it->second.as_arr()->elements.push_back(value);
}

inline void cli_add_arg(GCDict* d, const std::string& value) {
    auto& args = d->elements["args"];
    if (!args.is_arr()) args = Value::make_array();
    args.as_arr()->elements.push_back(Value(value));
}

inline void cli_add_value_flag(std::unordered_set<std::string>& flags, const std::string& text) {
    std::string cur;
    for (char ch : text) {
        if (ch == ',' || std::isspace((unsigned char)ch)) {
            if (!cur.empty()) {
                flags.insert(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) flags.insert(cur);
}

inline std::unordered_set<std::string> cli_value_flags_from(const Value& spec, int line) {
    std::unordered_set<std::string> flags;
    if (spec.is_str()) {
        cli_add_value_flag(flags, spec.as_str());
        return flags;
    }
    if (spec.is_arr()) {
        for (const auto& item : spec.as_arr()->elements) cli_add_value_flag(flags, item.to_str());
        return flags;
    }
    if (spec.is_dict()) {
        for (const auto& entry : spec.as_dict()->elements) {
            if (entry.second.truthy()) flags.insert(entry.first);
        }
        return flags;
    }
    throw JitThrow{"cli_parse(): arg 2 must be a string, array, or dict of value-taking flags", line};
}

inline Value b_cli_parse(const Value* a, int n, int l) {
    need_args("cli_parse", n, 1, 2, l);
    std::vector<std::string> tokens = cli_tokenize(need_str("cli_parse", a[0], 0, l));
    std::unordered_set<std::string> value_flags;
    if (n >= 2) value_flags = cli_value_flags_from(a[1], l);
    Value dict = Value::make_dict();
    auto* d = dict.as_dict();
    d->elements["args"] = Value::make_array();
    bool positional_only = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string tok = tokens[i];
        if (positional_only) {
            cli_add_arg(d, tok);
            continue;
        }
        if (tok == "--") {
            positional_only = true;
            continue;
        }
        if (tok.rfind("--", 0) == 0 && tok.size() > 2) {
            tok = tok.substr(2);
            size_t eq = tok.find('=');
            if (eq != std::string::npos) {
                cli_add_value(d, tok.substr(0, eq), Value(tok.substr(eq + 1)));
            } else if (tok.rfind("no-", 0) == 0 && tok.size() > 3) {
                cli_add_value(d, tok.substr(3), Value(false));
            } else if (value_flags.find(tok) != value_flags.end() && i + 1 < tokens.size() && cli_is_value_token(tokens[i + 1])) {
                cli_add_value(d, tok, Value(tokens[++i]));
            } else {
                cli_add_value(d, tok, Value(true));
            }
        } else if (tok.size() > 1 && tok[0] == '-' && !cli_is_value_token(tok)) {
            std::string flags = tok.substr(1);
            size_t eq = flags.find('=');
            if (eq != std::string::npos) {
                cli_add_value(d, flags.substr(0, eq), Value(flags.substr(eq + 1)));
            } else if (flags.size() == 1 && value_flags.find(flags) != value_flags.end() && i + 1 < tokens.size() && cli_is_value_token(tokens[i + 1])) {
                cli_add_value(d, flags, Value(tokens[++i]));
            } else {
                for (char flag : flags) cli_add_value(d, std::string(1, flag), Value(true));
            }
        } else {
            cli_add_arg(d, tok);
        }
    }
    return dict;
}

inline Value b_argv(const Value* a, int n, int l) {
    need_args("argv", n, 0, 0, l);
    Value out = Value::make_array();
    for (const auto& arg : script_args_storage()) {
        out.as_arr()->elements.push_back(Value(arg));
    }
    return out;
}

inline Value b_argc(const Value* a, int n, int l) {
    need_args("argc", n, 0, 0, l);
    return Value((double)script_args_storage().size());
}

inline Value b_script_name(const Value* a, int n, int l) {
    need_args("script_name", n, 0, 0, l);
    return Value(script_name_storage());
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline std::string& log_file_path_storage() {
    static std::string path;
    return path;
}

inline bool& log_json_storage() {
    static bool enabled = false;
    return enabled;
}

inline std::string& log_level_storage() {
    static std::string level = "DEBUG";
    return level;
}

inline int log_level_rank(const std::string& level) {
    if (level == "TRACE") return 0;
    if (level == "DEBUG") return 10;
    if (level == "INFO") return 20;
    if (level == "WARN" || level == "WARNING") return 30;
    if (level == "ERROR") return 40;
    if (level == "FATAL") return 50;
    if (level == "OFF") return 1000000;
    return 20;
}

inline std::string log_normalize_level(const std::string& raw, const char* name, int line) {
    std::string level = raw;
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    if (level == "WARNING") level = "WARN";
    if (level != "TRACE" && level != "DEBUG" && level != "INFO" &&
        level != "WARN" && level != "ERROR" && level != "FATAL" && level != "OFF") {
        throw JitThrow{std::string(name) + "(): level must be TRACE, DEBUG, INFO, WARN, ERROR, FATAL, or OFF", line};
    }
    return level;
}

inline std::string log_join_message(const Value* a, int n) {
    std::string message;
    for (int i = 0; i < n; ++i) {
        if (i) message += " ";
        message += a[i].to_str();
    }
    return message;
}

inline void log_append_line_locked(const std::string& line, int src_line) {
    const std::string& path = log_file_path_storage();
    if (path.empty()) return;
    std::filesystem::path file = fs_path_from_utf8(path);
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) throw JitThrow{"log(): cannot open log file '" + path + "'", src_line};
    out << line << "\n";
    if (!out) throw JitThrow{"log(): failed writing log file '" + path + "'", src_line};
}

inline std::string log_json_line(const std::string& level, const std::string& message, const Value* fields) {
    Value record = Value::make_dict();
    auto* d = record.as_dict();
    d->elements["time"] = b_datetime_now(nullptr, 0, 0);
    d->elements["level"] = Value(level);
    d->elements["message"] = Value(message);
    if (fields && fields->is_dict()) d->elements["fields"] = *fields;
    return json_stringify_value(record);
}

inline void log_emit(const std::string& level, const std::string& message, const Value* fields, int src_line) {
    std::lock_guard<std::mutex> lock(log_mutex());
    if (log_level_rank(level) < log_level_rank(log_level_storage())) return;
    std::string line = log_json_storage()
        ? log_json_line(level, message, fields)
        : ("[" + level + " " + b_datetime_now(nullptr, 0, 0).as_str() + "] " + message);
    std::cout << line << "\n";
    log_append_line_locked(line, src_line);
}

inline std::string log_upper_level(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return level;
}

inline Value b_log_set_file(const Value* a, int n, int l) {
    need_args("log_set_file", n, 1, 2, l);
    std::string path = need_str("log_set_file", a[0], 0, l);
    bool append = n >= 2 ? a[1].truthy() : true;
    std::lock_guard<std::mutex> lock(log_mutex());
    log_file_path_storage() = path;
    if (!path.empty() && !append) {
        std::filesystem::path file = fs_path_from_utf8(path);
        std::error_code ec;
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw JitThrow{"log_set_file(): cannot open '" + path + "'", l};
    }
    return Value(true);
}

inline Value b_log_set_json(const Value* a, int n, int l) {
    need_args("log_set_json", n, 1, 1, l);
    std::lock_guard<std::mutex> lock(log_mutex());
    log_json_storage() = a[0].truthy();
    return Value(log_json_storage());
}

inline Value b_log_set_level(const Value* a, int n, int l) {
    need_args("log_set_level", n, 1, 1, l);
    std::string level = log_normalize_level(need_str("log_set_level", a[0], 0, l), "log_set_level", l);
    std::lock_guard<std::mutex> lock(log_mutex());
    log_level_storage() = level;
    return Value(level);
}

inline Value b_log_get_level(const Value* a, int n, int l) {
    need_args("log_get_level", n, 0, 0, l);
    std::lock_guard<std::mutex> lock(log_mutex());
    return Value(log_level_storage());
}

inline Value b_log_level(const Value* a, int n, int l) {
    need_args("log_level", n, 0, 1, l);
    if (n == 0) return b_log_get_level(a, n, l);
    return b_log_set_level(a, n, l);
}

inline Value b_log_event(const Value* a, int n, int l) {
    need_args("log_event", n, 2, 3, l);
    if (n >= 3 && !a[2].is_dict()) throw JitThrow{"log_event(): fields must be a dictionary", l};
    const Value* fields = n >= 3 ? &a[2] : nullptr;
    log_emit(log_upper_level(need_str("log_event", a[0], 0, l)), a[1].to_str(), fields, l);
    return Value::nil();
}

inline void log_with_level(const char* level, const Value* a, int n, int l) {
    log_emit(level, log_join_message(a, n), nullptr, l);
}

inline Value b_log_debug(const Value* a, int n, int l) { log_with_level("DEBUG", a, n, l); return Value::nil(); }
inline Value b_log_info(const Value* a, int n, int l) { log_with_level("INFO", a, n, l); return Value::nil(); }
inline Value b_log_warn(const Value* a, int n, int l) { log_with_level("WARN", a, n, l); return Value::nil(); }
inline Value b_log_error(const Value* a, int n, int l) { log_with_level("ERROR", a, n, l); return Value::nil(); }

inline Value db_read_dict_file(const std::string& path, int line) {
    Value db = Value::make_dict();
    if (std::filesystem::exists(fs_path_from_utf8(path))) {
        std::string text = read_text_file(path, line);
        if (!text.empty()) db = JsonParser(text, line).parse_document();
        if (!db.is_dict()) db = Value::make_dict();
    }
    return db;
}

inline Value db_write_dict_file(const std::string& path, const Value& db, int line) {
    Value args[2] = {Value(path), Value(json_stringify_value(db))};
    return b_file_write(args, 2, line);
}

inline Value b_db_set(const Value* a, int n, int l) {
    need_args("db_set", n, 3, 3, l);
    std::string path = need_str("db_set", a[0], 0, l);
    std::string key = need_str("db_set", a[1], 1, l);
    Value db = db_read_dict_file(path, l);
    db.as_dict()->elements[key] = a[2];
    return db_write_dict_file(path, db, l);
}

inline Value b_db_get(const Value* a, int n, int l) {
    need_args("db_get", n, 2, 3, l);
    std::string path = need_str("db_get", a[0], 0, l);
    std::string key = need_str("db_get", a[1], 1, l);
    Value db = db_read_dict_file(path, l);
    auto it = db.as_dict()->elements.find(key);
    return it == db.as_dict()->elements.end() ? (n >= 3 ? a[2] : Value::nil()) : it->second;
}

inline Value b_db_has(const Value* a, int n, int l) {
    need_args("db_has", n, 2, 2, l);
    std::string path = need_str("db_has", a[0], 0, l);
    std::string key = need_str("db_has", a[1], 1, l);
    Value db = db_read_dict_file(path, l);
    return Value(db.as_dict()->elements.find(key) != db.as_dict()->elements.end());
}

inline Value b_db_delete(const Value* a, int n, int l) {
    need_args("db_delete", n, 2, 2, l);
    std::string path = need_str("db_delete", a[0], 0, l);
    std::string key = need_str("db_delete", a[1], 1, l);
    Value db = db_read_dict_file(path, l);
    auto erased = db.as_dict()->elements.erase(key);
    if (!erased) return Value(false);
    db_write_dict_file(path, db, l);
    return Value(true);
}

inline Value b_db_keys(const Value* a, int n, int l) {
    need_args("db_keys", n, 1, 1, l);
    std::string path = need_str("db_keys", a[0], 0, l);
    Value db = db_read_dict_file(path, l);
    std::vector<std::string> keys;
    for (const auto& [key, _] : db.as_dict()->elements) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    Value out = Value::make_array();
    for (const auto& key : keys) out.as_arr()->elements.push_back(Value(key));
    return out;
}

inline Value b_db_all(const Value* a, int n, int l) {
    need_args("db_all", n, 1, 1, l);
    return db_read_dict_file(need_str("db_all", a[0], 0, l), l);
}

inline GCDict* db_need_dict(const char* name, const Value& v, int idx, int line) {
    if (!v.is_dict()) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1)
                       + " must be a dict, got " + v.to_str(), line};
    }
    return v.as_dict();
}

inline bool db_value_equal(const Value& left, const Value& right) {
    if (left.eq(right)) return true;
    if (left.is_arr() && right.is_arr()) {
        auto* la = left.as_arr();
        auto* ra = right.as_arr();
        if (la->elements.size() != ra->elements.size()) return false;
        for (size_t i = 0; i < la->elements.size(); ++i)
            if (!db_value_equal(la->elements[i], ra->elements[i])) return false;
        return true;
    }
    if (left.is_dict() && right.is_dict()) {
        auto* ld = left.as_dict();
        auto* rd = right.as_dict();
        if (ld->elements.size() != rd->elements.size()) return false;
        for (const auto& [key, value] : ld->elements) {
            auto it = rd->elements.find(key);
            if (it == rd->elements.end() || !db_value_equal(value, it->second)) return false;
        }
        return true;
    }
    return false;
}

inline Value db_read_rows_file(const std::string& path, int line, const char* name) {
    Value rows = Value::make_array();
    if (!std::filesystem::exists(fs_path_from_utf8(path))) return rows;
    std::string text = read_text_file(path, line);
    if (text.empty()) return rows;
    Value parsed = JsonParser(text, line).parse_document();
    if (parsed.is_arr()) return parsed;
    if (parsed.is_dict()) {
        auto it = parsed.as_dict()->elements.find("rows");
        if (it != parsed.as_dict()->elements.end() && it->second.is_arr()) return it->second;
    }
    throw JitThrow{std::string(name) + "(): database file must contain an array or {rows: [...]}", line};
}

inline Value db_write_rows_file(const std::string& path, const Value& rows, int line) {
    Value args[2] = {Value(path), Value(json_stringify_value(rows))};
    return b_file_write(args, 2, line);
}

inline bool db_row_matches(const Value& row, GCDict* criteria) {
    if (!row.is_dict()) return false;
    auto* d = row.as_dict();
    for (const auto& [key, expected] : criteria->elements) {
        auto it = d->elements.find(key);
        if (it == d->elements.end() || !db_value_equal(it->second, expected)) return false;
    }
    return true;
}

inline Value b_db_insert(const Value* a, int n, int l) {
    need_args("db_insert", n, 2, 2, l);
    std::string path = need_str("db_insert", a[0], 0, l);
    db_need_dict("db_insert", a[1], 1, l);
    Value rows = db_read_rows_file(path, l, "db_insert");
    rows.as_arr()->elements.push_back(a[1]);
    db_write_rows_file(path, rows, l);
    return Value((double)rows.as_arr()->elements.size());
}

inline Value b_db_find(const Value* a, int n, int l) {
    need_args("db_find", n, 2, 2, l);
    std::string path = need_str("db_find", a[0], 0, l);
    GCDict* criteria = db_need_dict("db_find", a[1], 1, l);
    Value rows = db_read_rows_file(path, l, "db_find");
    Value out = Value::make_array();
    for (const auto& row : rows.as_arr()->elements) {
        if (db_row_matches(row, criteria)) out.as_arr()->elements.push_back(row);
    }
    return out;
}

inline Value b_db_count(const Value* a, int n, int l) {
    need_args("db_count", n, 1, 2, l);
    std::string path = need_str("db_count", a[0], 0, l);
    Value rows = db_read_rows_file(path, l, "db_count");
    if (n == 1) return Value((double)rows.as_arr()->elements.size());
    GCDict* criteria = db_need_dict("db_count", a[1], 1, l);
    int count = 0;
    for (const auto& row : rows.as_arr()->elements) if (db_row_matches(row, criteria)) ++count;
    return Value((double)count);
}

inline Value b_db_update(const Value* a, int n, int l) {
    need_args("db_update", n, 3, 3, l);
    std::string path = need_str("db_update", a[0], 0, l);
    GCDict* criteria = db_need_dict("db_update", a[1], 1, l);
    GCDict* patch = db_need_dict("db_update", a[2], 2, l);
    Value rows = db_read_rows_file(path, l, "db_update");
    int count = 0;
    for (auto& row : rows.as_arr()->elements) {
        if (!db_row_matches(row, criteria)) continue;
        auto* d = row.as_dict();
        for (const auto& [key, value] : patch->elements) d->elements[key] = value;
        ++count;
    }
    if (count > 0) db_write_rows_file(path, rows, l);
    return Value((double)count);
}

inline Value b_db_remove(const Value* a, int n, int l) {
    need_args("db_remove", n, 2, 2, l);
    std::string path = need_str("db_remove", a[0], 0, l);
    GCDict* criteria = db_need_dict("db_remove", a[1], 1, l);
    Value rows = db_read_rows_file(path, l, "db_remove");
    Value kept = Value::make_array();
    int removed = 0;
    for (const auto& row : rows.as_arr()->elements) {
        if (db_row_matches(row, criteria)) ++removed;
        else kept.as_arr()->elements.push_back(row);
    }
    if (removed > 0) db_write_rows_file(path, kept, l);
    return Value((double)removed);
}

inline size_t db_query_nonnegative_option(GCDict* options, const char* key, size_t fallback, int line) {
    auto it = options->elements.find(key);
    if (it == options->elements.end() || it->second.is_nil()) return fallback;
    if (!it->second.is_num()) throw JitThrow{std::string("db_query(): option ") + key + " must be a number", line};
    double raw = it->second.as_num();
    if (raw < 0 || std::floor(raw) != raw || raw > (double)std::numeric_limits<size_t>::max()) {
        throw JitThrow{std::string("db_query(): option ") + key + " must be a non-negative integer", line};
    }
    return (size_t)raw;
}

inline std::string db_query_string_option(GCDict* options, const char* primary, const char* alias, int line) {
    auto it = options->elements.find(primary);
    if ((it == options->elements.end() || it->second.is_nil()) && alias) it = options->elements.find(alias);
    if (it == options->elements.end() || it->second.is_nil()) return "";
    if (!it->second.is_str()) throw JitThrow{std::string("db_query(): sort option must be a string"), line};
    return it->second.as_str();
}

inline bool db_query_bool_option(GCDict* options, const char* primary, const char* alias) {
    auto it = options->elements.find(primary);
    if ((it == options->elements.end() || it->second.is_nil()) && alias) it = options->elements.find(alias);
    return it != options->elements.end() && it->second.truthy();
}

inline Value b_db_query(const Value* a, int n, int l) {
    need_args("db_query", n, 1, 3, l);
    std::string path = need_str("db_query", a[0], 0, l);
    GCDict* criteria = nullptr;
    GCDict* options = nullptr;
    if (n >= 2 && !a[1].is_nil()) criteria = db_need_dict("db_query", a[1], 1, l);
    if (n >= 3 && !a[2].is_nil()) options = db_need_dict("db_query", a[2], 2, l);

    Value rows = db_read_rows_file(path, l, "db_query");
    Value out = Value::make_array();
    for (const auto& row : rows.as_arr()->elements) {
        if (!criteria || db_row_matches(row, criteria)) out.as_arr()->elements.push_back(row);
    }

    if (options) {
        std::string sort_path = db_query_string_option(options, "sort_by", "sort", l);
        bool desc = db_query_bool_option(options, "desc", "descending");
        if (!sort_path.empty()) {
            std::stable_sort(out.as_arr()->elements.begin(), out.as_arr()->elements.end(),
                [&](const Value& left, const Value& right) {
                    Value lv = json_path_lookup(left, sort_path, nullptr, l, "db_query");
                    Value rv = json_path_lookup(right, sort_path, nullptr, l, "db_query");
                    if (db_value_equal(lv, rv)) return false;
                    return desc ? collection_value_less(rv, lv) : collection_value_less(lv, rv);
                });
        }

        size_t offset = db_query_nonnegative_option(options, "offset", 0, l);
        size_t total = out.as_arr()->elements.size();
        size_t start = std::min(offset, total);
        size_t limit = db_query_nonnegative_option(options, "limit", total - start, l);
        size_t end = std::min(total, start + std::min(limit, total - start));
        if (start > 0 || end < total) {
            Value sliced = Value::make_array();
            for (size_t i = start; i < end; ++i) sliced.as_arr()->elements.push_back(out.as_arr()->elements[i]);
            return sliced;
        }
    }

    return out;
}

inline Value b_vec_add(const Value* a, int n, int l) {
    need_args("vec_add", n, 2, 2, l);
    auto* x = need_arr("vec_add", a[0], 0, l);
    auto* y = need_arr("vec_add", a[1], 1, l);
    size_t m = std::min(x->elements.size(), y->elements.size());
    Value out = Value::make_array();
    for (size_t i = 0; i < m; ++i)
        out.as_arr()->elements.push_back(Value(x->elements[i].to_num() + y->elements[i].to_num()));
    return out;
}

inline Value b_vec_dot(const Value* a, int n, int l) {
    need_args("vec_dot", n, 2, 2, l);
    auto* x = need_arr("vec_dot", a[0], 0, l);
    auto* y = need_arr("vec_dot", a[1], 1, l);
    size_t m = std::min(x->elements.size(), y->elements.size());
    double sum = 0;
    for (size_t i = 0; i < m; ++i) sum += x->elements[i].to_num() * y->elements[i].to_num();
    return Value(sum);
}

inline Value b_vec_scale(const Value* a, int n, int l) {
    need_args("vec_scale", n, 2, 2, l);
    auto* x = need_arr("vec_scale", a[0], 0, l);
    double k = need_num("vec_scale", a[1], 1, l);
    Value out = Value::make_array();
    for (const auto& v : x->elements) out.as_arr()->elements.push_back(Value(v.to_num() * k));
    return out;
}

inline Value b_vec_norm(const Value* a, int n, int l) {
    need_args("vec_norm", n, 1, 1, l);
    auto* x = need_arr("vec_norm", a[0], 0, l);
    double sum = 0;
    for (const auto& v : x->elements) sum += v.to_num() * v.to_num();
    return Value(std::sqrt(sum));
}

inline Value make_vec3_value(double x, double y, double z) {
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(3);
    arr->elements.push_back(Value(x));
    arr->elements.push_back(Value(y));
    arr->elements.push_back(Value(z));
    return out;
}

inline void need_vec3(const char* name, const Value& value, int idx, int line, double& x, double& y, double& z) {
    auto* arr = need_arr(name, value, idx, line);
    if (arr->elements.size() != 3) {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1) + " must be a 3D vector", line};
    }
    x = arr->elements[0].to_num();
    y = arr->elements[1].to_num();
    z = arr->elements[2].to_num();
}

inline Value b_vec3(const Value* a, int n, int l) {
    need_args("vec3", n, 3, 3, l);
    return make_vec3_value(need_num("vec3", a[0], 0, l),
                           need_num("vec3", a[1], 1, l),
                           need_num("vec3", a[2], 2, l));
}

inline Value b_vec3_add(const Value* a, int n, int l) {
    need_args("vec3_add", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_add", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_add", a[1], 1, l, bx, by, bz);
    return make_vec3_value(ax + bx, ay + by, az + bz);
}

inline Value b_vec3_sub(const Value* a, int n, int l) {
    need_args("vec3_sub", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_sub", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_sub", a[1], 1, l, bx, by, bz);
    return make_vec3_value(ax - bx, ay - by, az - bz);
}

inline Value b_vec3_dot(const Value* a, int n, int l) {
    need_args("vec3_dot", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_dot", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_dot", a[1], 1, l, bx, by, bz);
    return Value(ax * bx + ay * by + az * bz);
}

inline Value b_vec3_cross(const Value* a, int n, int l) {
    need_args("vec3_cross", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_cross", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_cross", a[1], 1, l, bx, by, bz);
    return make_vec3_value(ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx);
}

inline Value b_vec3_scale(const Value* a, int n, int l) {
    need_args("vec3_scale", n, 2, 2, l);
    double x, y, z;
    need_vec3("vec3_scale", a[0], 0, l, x, y, z);
    double k = need_num("vec3_scale", a[1], 1, l);
    return make_vec3_value(x * k, y * k, z * k);
}

inline Value b_vec3_norm(const Value* a, int n, int l) {
    need_args("vec3_norm", n, 1, 1, l);
    double x, y, z;
    need_vec3("vec3_norm", a[0], 0, l, x, y, z);
    return Value(std::sqrt(x * x + y * y + z * z));
}

inline Value b_vec3_normalize(const Value* a, int n, int l) {
    need_args("vec3_normalize", n, 1, 1, l);
    double x, y, z;
    need_vec3("vec3_normalize", a[0], 0, l, x, y, z);
    double norm = std::sqrt(x * x + y * y + z * z);
    if (norm == 0) return make_vec3_value(0, 0, 0);
    return make_vec3_value(x / norm, y / norm, z / norm);
}

inline Value b_vec3_distance(const Value* a, int n, int l) {
    need_args("vec3_distance", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_distance", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_distance", a[1], 1, l, bx, by, bz);
    double dx = ax - bx;
    double dy = ay - by;
    double dz = az - bz;
    return Value(std::sqrt(dx * dx + dy * dy + dz * dz));
}

inline Value b_vec3_neg(const Value* a, int n, int l) {
    need_args("vec3_neg", n, 1, 1, l);
    double x, y, z;
    need_vec3("vec3_neg", a[0], 0, l, x, y, z);
    return make_vec3_value(-x, -y, -z);
}

inline Value b_vec3_lerp(const Value* a, int n, int l) {
    need_args("vec3_lerp", n, 3, 3, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_lerp", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_lerp", a[1], 1, l, bx, by, bz);
    double t = need_num("vec3_lerp", a[2], 2, l);
    return make_vec3_value(ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t);
}

inline Value b_vec3_midpoint(const Value* a, int n, int l) {
    need_args("vec3_midpoint", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_midpoint", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_midpoint", a[1], 1, l, bx, by, bz);
    return make_vec3_value((ax + bx) * 0.5, (ay + by) * 0.5, (az + bz) * 0.5);
}

inline Value b_vec3_project(const Value* a, int n, int l) {
    need_args("vec3_project", n, 2, 2, l);
    double vx, vy, vz, ox, oy, oz;
    need_vec3("vec3_project", a[0], 0, l, vx, vy, vz);
    need_vec3("vec3_project", a[1], 1, l, ox, oy, oz);
    double denom = ox * ox + oy * oy + oz * oz;
    if (denom == 0) return make_vec3_value(0, 0, 0);
    double scale = (vx * ox + vy * oy + vz * oz) / denom;
    return make_vec3_value(ox * scale, oy * scale, oz * scale);
}

inline Value b_vec3_reject(const Value* a, int n, int l) {
    need_args("vec3_reject", n, 2, 2, l);
    double vx, vy, vz, ox, oy, oz;
    need_vec3("vec3_reject", a[0], 0, l, vx, vy, vz);
    need_vec3("vec3_reject", a[1], 1, l, ox, oy, oz);
    double denom = ox * ox + oy * oy + oz * oz;
    if (denom == 0) return make_vec3_value(vx, vy, vz);
    double scale = (vx * ox + vy * oy + vz * oz) / denom;
    return make_vec3_value(vx - ox * scale, vy - oy * scale, vz - oz * scale);
}

inline Value b_vec3_reflect(const Value* a, int n, int l) {
    need_args("vec3_reflect", n, 2, 2, l);
    double vx, vy, vz, nx, ny, nz;
    need_vec3("vec3_reflect", a[0], 0, l, vx, vy, vz);
    need_vec3("vec3_reflect", a[1], 1, l, nx, ny, nz);
    double denom = nx * nx + ny * ny + nz * nz;
    if (denom == 0) return make_vec3_value(vx, vy, vz);
    double scale = 2.0 * (vx * nx + vy * ny + vz * nz) / denom;
    return make_vec3_value(vx - nx * scale, vy - ny * scale, vz - nz * scale);
}

inline Value b_vec3_angle(const Value* a, int n, int l) {
    need_args("vec3_angle", n, 2, 2, l);
    double ax, ay, az, bx, by, bz;
    need_vec3("vec3_angle", a[0], 0, l, ax, ay, az);
    need_vec3("vec3_angle", a[1], 1, l, bx, by, bz);
    double an = std::sqrt(ax * ax + ay * ay + az * az);
    double bn = std::sqrt(bx * bx + by * by + bz * bz);
    if (an == 0 || bn == 0) return Value(0);
    double c = (ax * bx + ay * by + az * bz) / (an * bn);
    c = std::max(-1.0, std::min(1.0, c));
    return Value(std::acos(c));
}

inline void need_mat4(const char* name, const Value& value, int idx, int line, std::array<double, 16>& out) {
    auto* arr = need_arr(name, value, idx, line);
    if (arr->elements.size() == 16) {
        for (size_t i = 0; i < 16; ++i) out[i] = arr->elements[i].to_num();
        return;
    }
    if (arr->elements.size() == 4) {
        for (size_t row = 0; row < 4; ++row) {
            if (!arr->elements[row].is_arr() || arr->elements[row].as_arr()->elements.size() != 4) {
                throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1) + " must be a flat 16-value matrix or 4x4 nested matrix", line};
            }
            auto* row_arr = arr->elements[row].as_arr();
            for (size_t col = 0; col < 4; ++col) out[row * 4 + col] = row_arr->elements[col].to_num();
        }
        return;
    }
    throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1) + " must be a flat 16-value matrix or 4x4 nested matrix", line};
}

inline Value transform_vec3_by_mat4(double x, double y, double z, const std::array<double, 16>& m) {
    double tx = m[0] * x + m[1] * y + m[2] * z + m[3];
    double ty = m[4] * x + m[5] * y + m[6] * z + m[7];
    double tz = m[8] * x + m[9] * y + m[10] * z + m[11];
    double tw = m[12] * x + m[13] * y + m[14] * z + m[15];
    if (std::fabs(tw) > 1e-12 && std::fabs(tw - 1.0) > 1e-12) {
        tx /= tw;
        ty /= tw;
        tz /= tw;
    }
    return make_vec3_value(tx, ty, tz);
}

inline Value b_vec3_transform4(const Value* a, int n, int l) {
    need_args("vec3_transform4", n, 2, 2, l);
    double x, y, z;
    need_vec3("vec3_transform4", a[0], 0, l, x, y, z);
    std::array<double, 16> m{};
    need_mat4("vec3_transform4", a[1], 1, l, m);
    return transform_vec3_by_mat4(x, y, z, m);
}

inline Value make_number_array(const std::vector<double>& values) {
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    for (double value : values) arr->elements.push_back(Value(value));
    return out;
}

inline Value make_mat4_value(const std::array<double, 16>& matrix) {
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(16);
    for (double value : matrix) arr->elements.push_back(Value(value));
    return out;
}

inline Value b_mat4_identity(const Value* a, int n, int l) {
    need_args("mat4_identity", n, 0, 0, l);
    return make_mat4_value({1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            0, 0, 0, 1});
}

inline Value b_mat4_translate(const Value* a, int n, int l) {
    need_args("mat4_translate", n, 3, 3, l);
    double x = need_num("mat4_translate", a[0], 0, l);
    double y = need_num("mat4_translate", a[1], 1, l);
    double z = need_num("mat4_translate", a[2], 2, l);
    return make_mat4_value({1, 0, 0, x,
                            0, 1, 0, y,
                            0, 0, 1, z,
                            0, 0, 0, 1});
}

inline Value b_mat4_scale(const Value* a, int n, int l) {
    need_args("mat4_scale", n, 3, 3, l);
    double x = need_num("mat4_scale", a[0], 0, l);
    double y = need_num("mat4_scale", a[1], 1, l);
    double z = need_num("mat4_scale", a[2], 2, l);
    return make_mat4_value({x, 0, 0, 0,
                            0, y, 0, 0,
                            0, 0, z, 0,
                            0, 0, 0, 1});
}

inline Value b_mat4_rotate_y(const Value* a, int n, int l) {
    need_args("mat4_rotate_y", n, 1, 1, l);
    double radians = need_num("mat4_rotate_y", a[0], 0, l);
    double c = std::cos(radians);
    double s = std::sin(radians);
    return make_mat4_value({c, 0, s, 0,
                            0, 1, 0, 0,
                            -s, 0, c, 0,
                            0, 0, 0, 1});
}

inline Value b_mat4_mul(const Value* a, int n, int l) {
    need_args("mat4_mul", n, 2, 2, l);
    std::array<double, 16> left{};
    std::array<double, 16> right{};
    need_mat4("mat4_mul", a[0], 0, l, left);
    need_mat4("mat4_mul", a[1], 1, l, right);
    std::array<double, 16> out{};
    for (size_t row = 0; row < 4; ++row) {
        const size_t r = row * 4;
        const double l0 = left[r + 0];
        const double l1 = left[r + 1];
        const double l2 = left[r + 2];
        const double l3 = left[r + 3];
        out[r + 0] = l0 * right[0] + l1 * right[4] + l2 * right[8] + l3 * right[12];
        out[r + 1] = l0 * right[1] + l1 * right[5] + l2 * right[9] + l3 * right[13];
        out[r + 2] = l0 * right[2] + l1 * right[6] + l2 * right[10] + l3 * right[14];
        out[r + 3] = l0 * right[3] + l1 * right[7] + l2 * right[11] + l3 * right[15];
    }
    return make_mat4_value(out);
}

inline Value make_index_array(const int* values, size_t count) {
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.reserve(count);
    for (size_t i = 0; i < count; ++i) arr->elements.push_back(Value((double)values[i]));
    return out;
}

inline Value make_index_array(const std::vector<int>& values) {
    return make_index_array(values.data(), values.size());
}

inline Value b_mesh_cube(const Value* a, int n, int l) {
    need_args("mesh_cube", n, 0, 2, l);
    double size = n >= 1 ? need_num("mesh_cube", a[0], 0, l) : 1.0;
    if (!std::isfinite(size) || size <= 0) throw JitThrow{"mesh_cube(): size must be a positive finite number", l};
    double cx = 0, cy = 0, cz = 0;
    if (n >= 2 && !a[1].is_nil()) need_vec3("mesh_cube", a[1], 1, l, cx, cy, cz);
    double h = size * 0.5;

    Value vertices = Value::make_array();
    auto* v = vertices.as_arr();
    v->elements.reserve(8);
    v->elements.push_back(make_vec3_value(cx - h, cy - h, cz - h));
    v->elements.push_back(make_vec3_value(cx + h, cy - h, cz - h));
    v->elements.push_back(make_vec3_value(cx + h, cy + h, cz - h));
    v->elements.push_back(make_vec3_value(cx - h, cy + h, cz - h));
    v->elements.push_back(make_vec3_value(cx - h, cy - h, cz + h));
    v->elements.push_back(make_vec3_value(cx + h, cy - h, cz + h));
    v->elements.push_back(make_vec3_value(cx + h, cy + h, cz + h));
    v->elements.push_back(make_vec3_value(cx - h, cy + h, cz + h));

    Value faces = Value::make_array();
    auto* f = faces.as_arr();
    f->elements.reserve(12);
    static constexpr int face_indices[12][3] = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {3, 6, 2}, {3, 7, 6},
        {1, 2, 6}, {1, 6, 5},
        {0, 4, 7}, {0, 7, 3}
    };
    for (const auto& face : face_indices) {
        f->elements.push_back(make_index_array(face, 3));
    }

    Value edges = Value::make_array();
    auto* e = edges.as_arr();
    e->elements.reserve(12);
    static constexpr int edge_indices[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    for (const auto& edge : edge_indices) {
        e->elements.push_back(make_index_array(edge, 2));
    }

    Value mesh = Value::make_dict();
    auto* d = mesh.as_dict();
    d->elements.reserve(4);
    d->elements["kind"] = Value("mesh");
    d->elements["vertices"] = vertices;
    d->elements["faces"] = faces;
    d->elements["edges"] = edges;
    return mesh;
}

inline GCArray* mesh_vertices(const char* name, const Value& mesh, int idx, int line) {
    GCDict* d = need_dict(name, mesh, idx, line);
    auto it = d->elements.find("vertices");
    if (it == d->elements.end() || !it->second.is_arr()) {
        throw JitThrow{std::string(name) + "(): mesh must contain a vertices array", line};
    }
    return it->second.as_arr();
}

inline Value mesh_copy_optional_array(GCDict* mesh, const std::string& key) {
    auto it = mesh->elements.find(key);
    if (it != mesh->elements.end() && it->second.is_arr()) return it->second;
    return Value::make_array();
}

inline int mesh_face_index(const char* name, const Value& value, size_t limit, int line) {
    if (!value.is_num()) throw JitThrow{std::string(name) + "(): face indices must be numbers", line};
    double raw = value.as_num();
    int index = (int)raw;
    if (raw != (double)index || index < 0 || (size_t)index >= limit) {
        throw JitThrow{std::string(name) + "(): face index out of range", line};
    }
    return index;
}

inline Value b_mesh_transform4(const Value* a, int n, int l) {
    need_args("mesh_transform4", n, 2, 2, l);
    GCDict* src = need_dict("mesh_transform4", a[0], 0, l);
    GCArray* vertices = mesh_vertices("mesh_transform4", a[0], 0, l);
    std::array<double, 16> matrix{};
    need_mat4("mesh_transform4", a[1], 1, l, matrix);

    Value out_vertices = Value::make_array();
    auto* out_arr = out_vertices.as_arr();
    out_arr->elements.reserve(vertices->elements.size());
    for (const auto& vertex : vertices->elements) {
        double x, y, z;
        need_vec3("mesh_transform4", vertex, 0, l, x, y, z);
        out_arr->elements.push_back(transform_vec3_by_mat4(x, y, z, matrix));
    }

    Value out = Value::make_dict();
    auto* od = out.as_dict();
    od->elements["kind"] = Value("mesh");
    od->elements["vertices"] = out_vertices;
    od->elements["faces"] = mesh_copy_optional_array(src, "faces");
    od->elements["edges"] = mesh_copy_optional_array(src, "edges");
    return out;
}

inline Value b_mesh_bounds(const Value* a, int n, int l) {
    need_args("mesh_bounds", n, 1, 1, l);
    GCArray* vertices = mesh_vertices("mesh_bounds", a[0], 0, l);
    if (vertices->elements.empty()) throw JitThrow{"mesh_bounds(): mesh must contain at least one vertex", l};

    double min_x, min_y, min_z, max_x, max_y, max_z;
    need_vec3("mesh_bounds", vertices->elements[0], 0, l, min_x, min_y, min_z);
    max_x = min_x;
    max_y = min_y;
    max_z = min_z;
    for (size_t i = 1; i < vertices->elements.size(); ++i) {
        double x, y, z;
        need_vec3("mesh_bounds", vertices->elements[i], 0, l, x, y, z);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        min_z = std::min(min_z, z);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        max_z = std::max(max_z, z);
    }

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["min"] = make_vec3_value(min_x, min_y, min_z);
    d->elements["max"] = make_vec3_value(max_x, max_y, max_z);
    d->elements["size"] = make_vec3_value(max_x - min_x, max_y - min_y, max_z - min_z);
    d->elements["center"] = make_vec3_value((min_x + max_x) * 0.5, (min_y + max_y) * 0.5, (min_z + max_z) * 0.5);
    return out;
}

inline Value b_mesh_face_normals(const Value* a, int n, int l) {
    need_args("mesh_face_normals", n, 1, 1, l);
    GCDict* mesh = need_dict("mesh_face_normals", a[0], 0, l);
    GCArray* vertices = mesh_vertices("mesh_face_normals", a[0], 0, l);
    auto face_it = mesh->elements.find("faces");
    if (face_it == mesh->elements.end() || !face_it->second.is_arr()) {
        throw JitThrow{"mesh_face_normals(): mesh must contain a faces array", l};
    }

    Value out = Value::make_array();
    auto* normals = out.as_arr();
    normals->elements.reserve(face_it->second.as_arr()->elements.size());
    for (const auto& face_value : face_it->second.as_arr()->elements) {
        GCArray* face = need_arr("mesh_face_normals", face_value, 0, l);
        if (face->elements.size() < 3) throw JitThrow{"mesh_face_normals(): each face needs at least 3 indices", l};
        int ia = mesh_face_index("mesh_face_normals", face->elements[0], vertices->elements.size(), l);
        int ib = mesh_face_index("mesh_face_normals", face->elements[1], vertices->elements.size(), l);
        int ic = mesh_face_index("mesh_face_normals", face->elements[2], vertices->elements.size(), l);

        double ax, ay, az, bx, by, bz, cx, cy, cz;
        need_vec3("mesh_face_normals", vertices->elements[(size_t)ia], 0, l, ax, ay, az);
        need_vec3("mesh_face_normals", vertices->elements[(size_t)ib], 0, l, bx, by, bz);
        need_vec3("mesh_face_normals", vertices->elements[(size_t)ic], 0, l, cx, cy, cz);
        double ux = bx - ax, uy = by - ay, uz = bz - az;
        double vx = cx - ax, vy = cy - ay, vz = cz - az;
        double nx = uy * vz - uz * vy;
        double ny = uz * vx - ux * vz;
        double nz = ux * vy - uy * vx;
        double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        normals->elements.push_back(len == 0 ? make_vec3_value(0, 0, 0)
                                             : make_vec3_value(nx / len, ny / len, nz / len));
    }
    return out;
}

inline double dict_num_or_default(const char* name, GCDict* d, const std::string& key, double fallback, int line) {
    auto it = d->elements.find(key);
    if (it == d->elements.end() || it->second.is_nil()) return fallback;
    if (!it->second.is_num()) throw JitThrow{std::string(name) + "(): camera field '" + key + "' must be a number", line};
    return it->second.as_num();
}

inline void dict_vec3_or_default(const char* name, GCDict* d, const std::string& key,
                                 double fx, double fy, double fz,
                                 int line, double& x, double& y, double& z) {
    auto it = d->elements.find(key);
    if (it == d->elements.end() || it->second.is_nil()) {
        x = fx;
        y = fy;
        z = fz;
        return;
    }
    need_vec3(name, it->second, 1, line, x, y, z);
}

inline Value b_camera_project(const Value* a, int n, int l) {
    need_args("camera_project", n, 2, 4, l);
    double px, py, pz;
    need_vec3("camera_project", a[0], 0, l, px, py, pz);
    GCDict* camera = need_dict("camera_project", a[1], 1, l);
    double width = n >= 3 ? need_num("camera_project", a[2], 2, l) : 1.0;
    double height = n >= 4 ? need_num("camera_project", a[3], 3, l) : 1.0;
    if (width <= 0 || height <= 0) throw JitThrow{"camera_project(): viewport must be positive", l};

    double cx, cy, cz, tx, ty, tz, ux, uy, uz;
    dict_vec3_or_default("camera_project", camera, "position", 0, 0, -1, l, cx, cy, cz);
    dict_vec3_or_default("camera_project", camera, "target", 0, 0, 0, l, tx, ty, tz);
    dict_vec3_or_default("camera_project", camera, "up", 0, 1, 0, l, ux, uy, uz);
    double fov_deg = dict_num_or_default("camera_project", camera, "fov_deg", 60.0, l);
    double aspect = dict_num_or_default("camera_project", camera, "aspect", width / height, l);
    double near_plane = dict_num_or_default("camera_project", camera, "near", 0.001, l);
    if (fov_deg <= 0 || fov_deg >= 179) throw JitThrow{"camera_project(): fov_deg must be between 0 and 179", l};
    if (aspect <= 0 || near_plane <= 0) throw JitThrow{"camera_project(): aspect and near must be positive", l};

    auto normalize = [](double& x, double& y, double& z) {
        double len = std::sqrt(x * x + y * y + z * z);
        if (len == 0) {
            x = 0;
            y = 0;
            z = 0;
            return;
        }
        x /= len;
        y /= len;
        z /= len;
    };
    auto dot3 = [](double ax, double ay, double az, double bx, double by, double bz) {
        return ax * bx + ay * by + az * bz;
    };

    double fx = tx - cx, fy = ty - cy, fz = tz - cz;
    normalize(fx, fy, fz);
    double rx = fy * uz - fz * uy;
    double ry = fz * ux - fx * uz;
    double rz = fx * uy - fy * ux;
    if (std::sqrt(rx * rx + ry * ry + rz * rz) == 0) {
        rx = 1;
        ry = 0;
        rz = 0;
    } else {
        normalize(rx, ry, rz);
    }
    double tux = ry * fz - rz * fy;
    double tuy = rz * fx - rx * fz;
    double tuz = rx * fy - ry * fx;

    double rel_x = px - cx, rel_y = py - cy, rel_z = pz - cz;
    double view_x = dot3(rel_x, rel_y, rel_z, rx, ry, rz);
    double view_y = dot3(rel_x, rel_y, rel_z, tux, tuy, tuz);
    double depth = dot3(rel_x, rel_y, rel_z, fx, fy, fz);
    bool visible = depth >= near_plane;
    double ndc_x = 0;
    double ndc_y = 0;
    if (visible) {
        double f = 1.0 / std::tan((fov_deg * 3.14159265358979323846 / 180.0) * 0.5);
        ndc_x = (view_x * f) / (depth * aspect);
        ndc_y = (view_y * f) / depth;
    }

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["x"] = Value((ndc_x + 1.0) * 0.5 * width);
    d->elements["y"] = Value((1.0 - ndc_y) * 0.5 * height);
    d->elements["depth"] = Value(depth);
    d->elements["visible"] = Value(visible);
    d->elements["ndc"] = make_vec3_value(ndc_x, ndc_y, depth);
    return out;
}

inline Value b_vector_cosine(const Value* a, int n, int l) {
    need_args("vector_cosine", n, 2, 2, l);
    auto* x = need_arr("vector_cosine", a[0], 0, l);
    auto* y = need_arr("vector_cosine", a[1], 1, l);
    size_t m = std::min(x->elements.size(), y->elements.size());
    double dot = 0;
    double x2 = 0;
    double y2 = 0;
    for (size_t i = 0; i < m; ++i) {
        double xv = x->elements[i].to_num();
        double yv = y->elements[i].to_num();
        dot += xv * yv;
        x2 += xv * xv;
        y2 += yv * yv;
    }
    if (x2 == 0 || y2 == 0) return Value(0.0);
    return Value(dot / (std::sqrt(x2) * std::sqrt(y2)));
}

inline Value b_vector_normalize(const Value* a, int n, int l) {
    need_args("vector_normalize", n, 1, 1, l);
    auto* x = need_arr("vector_normalize", a[0], 0, l);
    double sum = 0;
    for (const auto& v : x->elements) {
        double x_num = v.to_num();
        sum += x_num * x_num;
    }
    double norm = std::sqrt(sum);
    Value out = Value::make_array();
    for (const auto& v : x->elements) {
        out.as_arr()->elements.push_back(Value(norm == 0 ? 0.0 : v.to_num() / norm));
    }
    return out;
}

inline double vector_cosine_arrays(GCArray* x, GCArray* y) {
    size_t m = std::min(x->elements.size(), y->elements.size());
    double dot = 0;
    double x2 = 0;
    double y2 = 0;
    for (size_t i = 0; i < m; ++i) {
        double xv = x->elements[i].to_num();
        double yv = y->elements[i].to_num();
        dot += xv * yv;
        x2 += xv * xv;
        y2 += yv * yv;
    }
    if (x2 == 0 || y2 == 0) return 0.0;
    return dot / (std::sqrt(x2) * std::sqrt(y2));
}

inline GCArray* vector_embedding_for_item(const Value& item, const std::string& field, size_t index, int line) {
    if (item.is_arr()) return item.as_arr();
    if (item.is_dict()) {
        auto* d = item.as_dict();
        auto it = d->elements.find(field);
        if (it == d->elements.end())
            throw JitThrow{"vector_search(): item " + std::to_string(index) + " missing '" + field + "' embedding", line};
        if (!it->second.is_arr())
            throw JitThrow{"vector_search(): item " + std::to_string(index) + " field '" + field + "' must be an array", line};
        return it->second.as_arr();
    }
    throw JitThrow{"vector_search(): item " + std::to_string(index) + " must be an embedding array or dict", line};
}

inline Value b_vector_search(const Value* a, int n, int l) {
    need_args("vector_search", n, 2, 4, l);
    auto* query = need_arr("vector_search", a[0], 0, l);
    auto* rows = need_arr("vector_search", a[1], 1, l);
    int k = n >= 3 ? (int)need_num("vector_search", a[2], 2, l) : (int)rows->elements.size();
    std::string field = n >= 4 ? need_str("vector_search", a[3], 3, l) : "embedding";
    if (k < 0) throw JitThrow{"vector_search(): k must be non-negative", l};
    if (field.empty()) throw JitThrow{"vector_search(): field must not be empty", l};

    struct Hit {
        size_t index;
        double score;
        Value item;
    };
    std::vector<Hit> hits;
    hits.reserve(rows->elements.size());
    for (size_t i = 0; i < rows->elements.size(); ++i) {
        GCArray* embedding = vector_embedding_for_item(rows->elements[i], field, i, l);
        hits.push_back(Hit{i, vector_cosine_arrays(query, embedding), rows->elements[i]});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.score > b.score;
    });

    Value out = Value::make_array();
    int limit = std::min(k, (int)hits.size());
    for (int i = 0; i < limit; ++i) {
        Value row = Value::make_dict();
        auto* d = row.as_dict();
        d->elements["index"] = Value((double)hits[i].index);
        d->elements["score"] = Value(hits[i].score);
        d->elements["item"] = hits[i].item;
        out.as_arr()->elements.push_back(row);
    }
    return out;
}

inline std::string rag_item_text(const Value& item, const std::string& text_field, size_t rank, int line,
                                 const char* caller = "rag_context") {
    if (item.is_dict()) {
        auto* d = item.as_dict();
        auto it = d->elements.find(text_field);
        if (it == d->elements.end())
            throw JitThrow{std::string(caller) + "(): result " + std::to_string(rank) +
                           " missing '" + text_field + "' text field", line};
        return it->second.to_str();
    }
    if (item.is_str()) return item.as_str();
    throw JitThrow{std::string(caller) + "(): result " + std::to_string(rank) + " must be a dict or string", line};
}

inline Value b_rag_context(const Value* a, int n, int l) {
    need_args("rag_context", n, 2, 5, l);
    int k = n >= 3 ? (int)need_num("rag_context", a[2], 2, l) : 3;
    std::string embedding_field = n >= 4 ? need_str("rag_context", a[3], 3, l) : "embedding";
    std::string text_field = n >= 5 ? need_str("rag_context", a[4], 4, l) : "text";
    Value search_args[4] = {a[0], a[1], Value((double)k), Value(embedding_field)};
    Value hits = b_vector_search(search_args, 4, l);
    std::string out;
    auto* arr = hits.as_arr();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        if (!arr->elements[i].is_dict()) continue;
        auto* hit = arr->elements[i].as_dict();
        auto it = hit->elements.find("item");
        if (it == hit->elements.end()) continue;
        if (!out.empty()) out += "\n\n";
        out += rag_item_text(it->second, text_field, i + 1, l);
    }
    return Value(out);
}

inline Value b_rag_sources(const Value* a, int n, int l) {
    need_args("rag_sources", n, 2, 6, l);
    int k = n >= 3 ? (int)need_num("rag_sources", a[2], 2, l) : 3;
    std::string embedding_field = n >= 4 ? need_str("rag_sources", a[3], 3, l) : "embedding";
    std::string text_field = n >= 5 ? need_str("rag_sources", a[4], 4, l) : "text";
    std::string title_field = n >= 6 ? need_str("rag_sources", a[5], 5, l) : "title";
    Value search_args[4] = {a[0], a[1], Value((double)k), Value(embedding_field)};
    Value hits = b_vector_search(search_args, 4, l);
    Value out = Value::make_array();
    auto* out_arr = out.as_arr();
    auto* hit_arr = hits.as_arr();
    for (size_t i = 0; i < hit_arr->elements.size(); ++i) {
        if (!hit_arr->elements[i].is_dict()) continue;
        auto* hit = hit_arr->elements[i].as_dict();
        auto item_it = hit->elements.find("item");
        if (item_it == hit->elements.end()) continue;
        Value source = Value::make_dict();
        auto* d = source.as_dict();
        d->elements["rank"] = Value((double)i + 1);
        d->elements["item"] = item_it->second;
        d->elements["text"] = Value(rag_item_text(item_it->second, text_field, i + 1, l, "rag_sources"));
        auto index_it = hit->elements.find("index");
        if (index_it != hit->elements.end()) d->elements["index"] = index_it->second;
        auto score_it = hit->elements.find("score");
        if (score_it != hit->elements.end()) d->elements["score"] = score_it->second;
        if (item_it->second.is_dict()) {
            auto* item = item_it->second.as_dict();
            auto id_it = item->elements.find("id");
            if (id_it != item->elements.end()) d->elements["id"] = id_it->second;
            if (!title_field.empty()) {
                auto title_it = item->elements.find(title_field);
                if (title_it != item->elements.end()) d->elements["title"] = title_it->second;
            }
        }
        out_arr->elements.push_back(source);
    }
    return out;
}

inline Value b_rag_prepare(const Value* a, int n, int l) {
    need_args("rag_prepare", n, 3, 8, l);
    std::string question = need_str("rag_prepare", a[0], 0, l);
    int k = n >= 4 ? (int)need_num("rag_prepare", a[3], 3, l) : 3;
    std::string embedding_field = n >= 5 ? need_str("rag_prepare", a[4], 4, l) : "embedding";
    std::string text_field = n >= 6 ? need_str("rag_prepare", a[5], 5, l) : "text";
    std::string title_field = n >= 8 ? need_str("rag_prepare", a[7], 7, l) : "title";
    Value source_args[6] = {a[1], a[2], Value((double)k), Value(embedding_field), Value(text_field), Value(title_field)};
    Value sources = b_rag_sources(source_args, 6, l);

    std::string context;
    auto* source_arr = sources.as_arr();
    for (size_t i = 0; i < source_arr->elements.size(); ++i) {
        if (!source_arr->elements[i].is_dict()) continue;
        auto* src = source_arr->elements[i].as_dict();
        auto text_it = src->elements.find("text");
        if (text_it == src->elements.end()) continue;
        if (!context.empty()) context += "\n\n";
        std::string label = "[" + std::to_string(i + 1) + "]";
        auto title_it = src->elements.find("title");
        auto id_it = src->elements.find("id");
        if (title_it != src->elements.end()) label += " " + title_it->second.to_str();
        else if (id_it != src->elements.end()) label += " " + id_it->second.to_str();
        context += label + "\n" + text_it->second.to_str();
    }

    std::string system = n >= 7
        ? need_str("rag_prepare", a[6], 6, l)
        : std::string("Answer using only the provided context. If the context is insufficient, say you do not know.");
    std::string user = "Context:\n" + context + "\n\nQuestion:\n" + question;
    Value messages = Value::make_array();
    Value system_msg = Value::make_dict();
    system_msg.as_dict()->elements["role"] = Value(std::string("system"));
    system_msg.as_dict()->elements["content"] = Value(system);
    Value user_msg = Value::make_dict();
    user_msg.as_dict()->elements["role"] = Value(std::string("user"));
    user_msg.as_dict()->elements["content"] = Value(user);
    messages.as_arr()->elements.push_back(system_msg);
    messages.as_arr()->elements.push_back(user_msg);
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["question"] = Value(question);
    d->elements["context"] = Value(context);
    d->elements["sources"] = sources;
    d->elements["messages"] = messages;
    return out;
}

inline void tensor_shape_collect(const Value& v, std::vector<size_t>& dims) {
    if (!v.is_arr()) return;
    auto* arr = v.as_arr();
    dims.push_back(arr->elements.size());
    if (!arr->elements.empty()) tensor_shape_collect(arr->elements[0], dims);
}

inline Value dims_to_array(const std::vector<size_t>& dims) {
    Value out = Value::make_array();
    for (size_t d : dims) out.as_arr()->elements.push_back(Value((double)d));
    return out;
}

inline Value b_tensor_shape(const Value* a, int n, int l) {
    need_args("tensor_shape", n, 1, 1, l);
    if (!a[0].is_arr()) throw JitThrow{"tensor_shape(): tensor must be an array", l};
    std::vector<size_t> dims;
    tensor_shape_collect(a[0], dims);
    return dims_to_array(dims);
}

inline Value tensor_fill_recursive(const std::vector<size_t>& dims, size_t depth, double value) {
    if (depth >= dims.size()) return Value(value);
    Value out = Value::make_array();
    for (size_t i = 0; i < dims[depth]; ++i)
        out.as_arr()->elements.push_back(tensor_fill_recursive(dims, depth + 1, value));
    return out;
}

inline std::vector<size_t> tensor_dims_from_shape(const Value& shape, const char* name, int line) {
    auto* arr = need_arr(name, shape, 0, line);
    std::vector<size_t> dims;
    for (const auto& dim : arr->elements) {
        double x = dim.to_num();
        if (x < 0 || x != std::floor(x)) throw JitThrow{std::string(name) + "(): shape dimensions must be non-negative integers", line};
        dims.push_back((size_t)x);
    }
    return dims;
}

inline Value b_tensor_zeros(const Value* a, int n, int l) {
    need_args("tensor_zeros", n, 1, 1, l);
    return tensor_fill_recursive(tensor_dims_from_shape(a[0], "tensor_zeros", l), 0, 0.0);
}

inline Value b_tensor_fill(const Value* a, int n, int l) {
    need_args("tensor_fill", n, 2, 2, l);
    return tensor_fill_recursive(tensor_dims_from_shape(a[0], "tensor_fill", l), 0, need_num("tensor_fill", a[1], 1, l));
}

template<typename Op>
inline Value tensor_elementwise(const char* name, const Value& x, const Value& y, int line, Op op) {
    if (x.is_arr() && y.is_arr()) {
        auto* xa = x.as_arr();
        auto* ya = y.as_arr();
        if (xa->elements.size() != ya->elements.size())
            throw JitThrow{std::string(name) + "(): tensor shapes do not match", line};
        Value out = Value::make_array();
        for (size_t i = 0; i < xa->elements.size(); ++i)
            out.as_arr()->elements.push_back(tensor_elementwise(name, xa->elements[i], ya->elements[i], line, op));
        return out;
    }
    if (x.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : x.as_arr()->elements)
            out.as_arr()->elements.push_back(tensor_elementwise(name, item, y, line, op));
        return out;
    }
    if (y.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : y.as_arr()->elements)
            out.as_arr()->elements.push_back(tensor_elementwise(name, x, item, line, op));
        return out;
    }
    if (!x.is_num() || !y.is_num())
        throw JitThrow{std::string(name) + "(): tensor leaves must be numbers", line};
    return Value(op(x.as_num(), y.as_num()));
}

inline Value b_tensor_add(const Value* a, int n, int l) {
    need_args("tensor_add", n, 2, 2, l);
    return tensor_elementwise("tensor_add", a[0], a[1], l, [](double x, double y) { return x + y; });
}

inline Value b_tensor_mul(const Value* a, int n, int l) {
    need_args("tensor_mul", n, 2, 2, l);
    return tensor_elementwise("tensor_mul", a[0], a[1], l, [](double x, double y) { return x * y; });
}

inline Value tensor_clip_value(const Value& v, double low, double high, int line) {
    if (v.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : v.as_arr()->elements) {
            out.as_arr()->elements.push_back(tensor_clip_value(item, low, high, line));
        }
        return out;
    }
    if (!v.is_num()) throw JitThrow{"tensor_clip(): tensor leaves must be numbers", line};
    double value = v.as_num();
    if (value < low) value = low;
    if (value > high) value = high;
    return Value(value);
}

inline Value b_tensor_clip(const Value* a, int n, int l) {
    need_args("tensor_clip", n, 3, 3, l);
    double low = need_num("tensor_clip", a[1], 1, l);
    double high = need_num("tensor_clip", a[2], 2, l);
    if (low > high) throw JitThrow{"tensor_clip(): min must be <= max", l};
    return tensor_clip_value(a[0], low, high, l);
}

inline void tensor_flatten_into(const Value& v, Value& out, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_flatten_into(item, out, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{"tensor_flatten(): tensor leaves must be numbers", line};
    out.as_arr()->elements.push_back(v);
}

inline Value b_tensor_flatten(const Value* a, int n, int l) {
    need_args("tensor_flatten", n, 1, 1, l);
    Value out = Value::make_array();
    tensor_flatten_into(a[0], out, l);
    return out;
}

inline void tensor_sum_count_into(const char* name, const Value& v, double& sum, size_t& count, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_sum_count_into(name, item, sum, count, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{std::string(name) + "(): tensor leaves must be numbers", line};
    sum += v.as_num();
    ++count;
}

inline Value b_tensor_sum(const Value* a, int n, int l) {
    need_args("tensor_sum", n, 1, 1, l);
    double sum = 0.0;
    size_t count = 0;
    tensor_sum_count_into("tensor_sum", a[0], sum, count, l);
    return Value(sum);
}

inline Value b_tensor_mean(const Value* a, int n, int l) {
    need_args("tensor_mean", n, 1, 1, l);
    double sum = 0.0;
    size_t count = 0;
    tensor_sum_count_into("tensor_mean", a[0], sum, count, l);
    if (count == 0) return Value::nil();
    return Value(sum / (double)count);
}

inline void tensor_squared_diff_sum_into(const char* name, const Value& v, double mean, double& sum_sq, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_squared_diff_sum_into(name, item, mean, sum_sq, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{std::string(name) + "(): tensor leaves must be numbers", line};
    double diff = v.as_num() - mean;
    sum_sq += diff * diff;
}

inline Value b_tensor_variance(const Value* a, int n, int l) {
    need_args("tensor_variance", n, 1, 1, l);
    double sum = 0.0;
    size_t count = 0;
    tensor_sum_count_into("tensor_variance", a[0], sum, count, l);
    if (count == 0) return Value::nil();
    double mean = sum / (double)count;
    double sum_sq = 0.0;
    tensor_squared_diff_sum_into("tensor_variance", a[0], mean, sum_sq, l);
    return Value(sum_sq / (double)count);
}

inline Value b_tensor_std(const Value* a, int n, int l) {
    need_args("tensor_std", n, 1, 1, l);
    Value variance = b_tensor_variance(a, n, l);
    return variance.is_nil() ? Value::nil() : Value(std::sqrt(variance.as_num()));
}

inline void tensor_min_max_into(const char* name, const Value& v, bool want_min, bool& seen, double& result, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_min_max_into(name, item, want_min, seen, result, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{std::string(name) + "(): tensor leaves must be numbers", line};
    double value = v.as_num();
    if (!seen) {
        result = value;
        seen = true;
    } else if (want_min ? value < result : value > result) {
        result = value;
    }
}

inline Value b_tensor_min(const Value* a, int n, int l) {
    need_args("tensor_min", n, 1, 1, l);
    bool seen = false;
    double result = 0.0;
    tensor_min_max_into("tensor_min", a[0], true, seen, result, l);
    return seen ? Value(result) : Value::nil();
}

inline Value b_tensor_max(const Value* a, int n, int l) {
    need_args("tensor_max", n, 1, 1, l);
    bool seen = false;
    double result = 0.0;
    tensor_min_max_into("tensor_max", a[0], false, seen, result, l);
    return seen ? Value(result) : Value::nil();
}

inline void tensor_arg_min_max_into(const char* name, const Value& v, bool want_min,
                                    bool& seen, double& best_value, size_t& best_index,
                                    size_t& current_index, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) {
            tensor_arg_min_max_into(name, item, want_min, seen, best_value, best_index, current_index, line);
        }
        return;
    }
    if (!v.is_num()) throw JitThrow{std::string(name) + "(): tensor leaves must be numbers", line};
    double value = v.as_num();
    if (!seen) {
        best_value = value;
        best_index = current_index;
        seen = true;
    } else if (want_min ? value < best_value : value > best_value) {
        best_value = value;
        best_index = current_index;
    }
    ++current_index;
}

inline Value b_tensor_argmin(const Value* a, int n, int l) {
    need_args("tensor_argmin", n, 1, 1, l);
    bool seen = false;
    double best_value = 0.0;
    size_t best_index = 0;
    size_t current_index = 0;
    tensor_arg_min_max_into("tensor_argmin", a[0], true, seen, best_value, best_index, current_index, l);
    return seen ? Value((double)best_index) : Value::nil();
}

inline Value b_tensor_argmax(const Value* a, int n, int l) {
    need_args("tensor_argmax", n, 1, 1, l);
    bool seen = false;
    double best_value = 0.0;
    size_t best_index = 0;
    size_t current_index = 0;
    tensor_arg_min_max_into("tensor_argmax", a[0], false, seen, best_value, best_index, current_index, l);
    return seen ? Value((double)best_index) : Value::nil();
}

inline void tensor_softmax_scan(const Value& v, bool& seen, double& max_value, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_softmax_scan(item, seen, max_value, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{"tensor_softmax(): tensor leaves must be numbers", line};
    double value = v.as_num();
    if (!seen || value > max_value) {
        max_value = value;
        seen = true;
    }
}

inline void tensor_softmax_sum(const Value& v, double max_value, double& denom, int line) {
    if (v.is_arr()) {
        for (const auto& item : v.as_arr()->elements) tensor_softmax_sum(item, max_value, denom, line);
        return;
    }
    if (!v.is_num()) throw JitThrow{"tensor_softmax(): tensor leaves must be numbers", line};
    denom += std::exp(v.as_num() - max_value);
}

inline Value tensor_empty_array_like(const Value& v) {
    Value out = Value::make_array();
    if (!v.is_arr()) return out;
    for (const auto& item : v.as_arr()->elements) {
        out.as_arr()->elements.push_back(tensor_empty_array_like(item));
    }
    return out;
}

inline Value tensor_zscore_build(const Value& v, double mean, double stddev, int line) {
    if (v.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : v.as_arr()->elements) {
            out.as_arr()->elements.push_back(tensor_zscore_build(item, mean, stddev, line));
        }
        return out;
    }
    if (!v.is_num()) throw JitThrow{"tensor_zscore(): tensor leaves must be numbers", line};
    return Value(stddev == 0.0 ? 0.0 : (v.as_num() - mean) / stddev);
}

inline Value b_tensor_zscore(const Value* a, int n, int l) {
    need_args("tensor_zscore", n, 1, 1, l);
    double sum = 0.0;
    size_t count = 0;
    tensor_sum_count_into("tensor_zscore", a[0], sum, count, l);
    if (count == 0) return tensor_empty_array_like(a[0]);
    double mean = sum / (double)count;
    double sum_sq = 0.0;
    tensor_squared_diff_sum_into("tensor_zscore", a[0], mean, sum_sq, l);
    double stddev = std::sqrt(sum_sq / (double)count);
    return tensor_zscore_build(a[0], mean, stddev, l);
}

inline Value tensor_softmax_build(const Value& v, double max_value, double denom, int line) {
    if (v.is_arr()) {
        Value out = Value::make_array();
        for (const auto& item : v.as_arr()->elements) {
            out.as_arr()->elements.push_back(tensor_softmax_build(item, max_value, denom, line));
        }
        return out;
    }
    if (!v.is_num()) throw JitThrow{"tensor_softmax(): tensor leaves must be numbers", line};
    return Value(std::exp(v.as_num() - max_value) / denom);
}

inline Value b_tensor_softmax(const Value* a, int n, int l) {
    need_args("tensor_softmax", n, 1, 1, l);
    bool seen = false;
    double max_value = 0.0;
    tensor_softmax_scan(a[0], seen, max_value, l);
    if (!seen) return tensor_empty_array_like(a[0]);
    double denom = 0.0;
    tensor_softmax_sum(a[0], max_value, denom, l);
    return tensor_softmax_build(a[0], max_value, denom, l);
}

inline Value b_tensor_transpose(const Value* a, int n, int l) {
    need_args("tensor_transpose", n, 1, 1, l);
    auto* m = need_arr("tensor_transpose", a[0], 0, l);
    if (m->elements.empty()) return Value::make_array();
    size_t rows = m->elements.size();
    size_t cols = need_arr("tensor_transpose", m->elements[0], 0, l)->elements.size();
    Value out = Value::make_array();
    for (size_t c = 0; c < cols; ++c) {
        Value row = Value::make_array();
        for (size_t r = 0; r < rows; ++r) {
            auto* src_row = need_arr("tensor_transpose", m->elements[r], 0, l);
            if (src_row->elements.size() != cols) throw JitThrow{"tensor_transpose(): rows must have equal length", l};
            row.as_arr()->elements.push_back(src_row->elements[c]);
        }
        out.as_arr()->elements.push_back(row);
    }
    return out;
}

inline Value b_tensor_matmul(const Value* a, int n, int l) {
    need_args("tensor_matmul", n, 2, 2, l);
    auto* left = need_arr("tensor_matmul", a[0], 0, l);
    auto* right = need_arr("tensor_matmul", a[1], 1, l);
    if (left->elements.empty() || right->elements.empty()) return Value::make_array();
    size_t left_rows = left->elements.size();
    size_t left_cols = need_arr("tensor_matmul", left->elements[0], 0, l)->elements.size();
    size_t right_rows = right->elements.size();
    size_t right_cols = need_arr("tensor_matmul", right->elements[0], 1, l)->elements.size();
    if (left_cols != right_rows) throw JitThrow{"tensor_matmul(): inner dimensions do not match", l};
    Value out = Value::make_array();
    for (size_t r = 0; r < left_rows; ++r) {
        auto* lrow = need_arr("tensor_matmul", left->elements[r], 0, l);
        if (lrow->elements.size() != left_cols) throw JitThrow{"tensor_matmul(): left rows must have equal length", l};
        Value out_row = Value::make_array();
        for (size_t c = 0; c < right_cols; ++c) {
            double sum = 0;
            for (size_t k = 0; k < left_cols; ++k) {
                auto* rrow = need_arr("tensor_matmul", right->elements[k], 1, l);
                if (rrow->elements.size() != right_cols) throw JitThrow{"tensor_matmul(): right rows must have equal length", l};
                sum += lrow->elements[k].to_num() * rrow->elements[c].to_num();
            }
            out_row.as_arr()->elements.push_back(Value(sum));
        }
        out.as_arr()->elements.push_back(out_row);
    }
    return out;
}

// Native neural-network runtime. Models are plain Sura dictionaries so they
// remain inspectable, clonable, and JSON serializable without Python or a
// third-party package.
using NnVector = std::vector<double>;
using NnMatrix = std::vector<NnVector>;

struct NnLayer {
    NnMatrix weights; // [output][input]
    NnVector bias;
    std::string activation;
};

struct NnForwardCache {
    std::vector<NnVector> activations; // input, then each layer output
    std::vector<NnVector> preactivations;
};

struct NnGradients {
    std::vector<NnMatrix> weights;
    std::vector<NnVector> bias;
};

static constexpr size_t NN_MAX_LAYERS = 64;
static constexpr size_t NN_MAX_PARAMETERS = 5000000;

inline std::string nn_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    return text;
}

inline const Value* nn_dict_get(const GCDict* dict, const char* key) {
    auto it = dict->elements.find(key);
    return it == dict->elements.end() ? nullptr : &it->second;
}

inline GCDict* nn_options(const char* name, const Value* a, int n, int index, int line) {
    if (n <= index || a[index].is_nil()) return nullptr;
    return need_dict(name, a[index], index, line);
}

inline double nn_option_number(const char* name, const GCDict* options, const char* key,
                               double fallback, double minimum, double maximum, int line) {
    if (!options) return fallback;
    const Value* value = nn_dict_get(options, key);
    if (!value || value->is_nil()) return fallback;
    if (!value->is_num() || !std::isfinite(value->as_num())) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a finite number", line};
    }
    double raw = value->as_num();
    if (raw < minimum || raw > maximum) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be between "
                       + std::to_string(minimum) + " and " + std::to_string(maximum), line};
    }
    return raw;
}

inline size_t nn_option_integer(const char* name, const GCDict* options, const char* key,
                                size_t fallback, size_t minimum, size_t maximum, int line) {
    double raw = nn_option_number(name, options, key, (double)fallback,
                                  (double)minimum, (double)maximum, line);
    if (raw != std::floor(raw)) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be an integer", line};
    }
    return (size_t)raw;
}

inline bool nn_option_bool(const char* name, const GCDict* options, const char* key,
                           bool fallback, int line) {
    if (!options) return fallback;
    const Value* value = nn_dict_get(options, key);
    if (!value || value->is_nil()) return fallback;
    if (!value->is_bool()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a bool", line};
    }
    return value->as_bool();
}

inline std::string nn_option_string(const char* name, const GCDict* options, const char* key,
                                    const std::string& fallback, int line) {
    if (!options) return fallback;
    const Value* value = nn_dict_get(options, key);
    if (!value || value->is_nil()) return fallback;
    if (!value->is_str()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a string", line};
    }
    return nn_lower(value->as_str());
}

inline std::string nn_activation_name(const char* name, const std::string& raw,
                                      bool output_layer, int line) {
    std::string activation = nn_lower(raw);
    if (activation == "none" || activation == "identity") activation = "linear";
    if (activation != "linear" && activation != "relu" && activation != "tanh" &&
        activation != "sigmoid" && activation != "softmax") {
        throw JitThrow{std::string(name) + "(): unsupported activation '" + raw
                       + "' (use linear, relu, tanh, sigmoid, or softmax)", line};
    }
    if (!output_layer && activation == "softmax") {
        throw JitThrow{std::string(name) + "(): softmax is supported only on the output layer", line};
    }
    return activation;
}

inline double nn_finite_number(const char* name, const Value& value,
                               const std::string& location, int line) {
    if (!value.is_num() || !std::isfinite(value.as_num())) {
        throw JitThrow{std::string(name) + "(): " + location + " must be a finite number", line};
    }
    return value.as_num();
}

inline NnVector nn_parse_vector(const char* name, const Value& value,
                                const std::string& location, size_t expected, int line) {
    if (!value.is_arr()) {
        throw JitThrow{std::string(name) + "(): " + location + " must be an array", line};
    }
    auto* array = value.as_arr();
    if (expected && array->elements.size() != expected) {
        throw JitThrow{std::string(name) + "(): " + location + " has "
                       + std::to_string(array->elements.size()) + " value(s), expected "
                       + std::to_string(expected), line};
    }
    NnVector out;
    out.reserve(array->elements.size());
    for (size_t i = 0; i < array->elements.size(); ++i) {
        out.push_back(nn_finite_number(name, array->elements[i],
                                      location + "[" + std::to_string(i) + "]", line));
    }
    return out;
}

inline size_t nn_parameter_count(const std::vector<NnLayer>& layers) {
    size_t count = 0;
    for (const auto& layer : layers) {
        count += layer.bias.size();
        for (const auto& row : layer.weights) count += row.size();
    }
    return count;
}

inline std::vector<NnLayer> nn_parse_model(const char* name, const Value& model, int line) {
    if (!model.is_dict()) throw JitThrow{std::string(name) + "(): model must be a dict", line};
    auto* dict = model.as_dict();
    if (const Value* format = nn_dict_get(dict, "format")) {
        if (!format->is_str() || format->as_str() != "sura.nn.mlp.v1") {
            throw JitThrow{std::string(name) + "(): unsupported model format", line};
        }
    }
    if (const Value* kind = nn_dict_get(dict, "kind")) {
        if (!kind->is_str() || kind->as_str() != "mlp") {
            throw JitThrow{std::string(name) + "(): model kind must be 'mlp'", line};
        }
    }
    const Value* layers_value = nn_dict_get(dict, "layers");
    if (!layers_value || !layers_value->is_arr()) {
        throw JitThrow{std::string(name) + "(): model.layers must be an array", line};
    }
    auto* layer_values = layers_value->as_arr();
    if (layer_values->elements.empty() || layer_values->elements.size() > NN_MAX_LAYERS) {
        throw JitThrow{std::string(name) + "(): model must contain 1.."
                       + std::to_string(NN_MAX_LAYERS) + " layers", line};
    }

    std::vector<NnLayer> layers;
    layers.reserve(layer_values->elements.size());
    size_t previous_output = 0;
    size_t parameters = 0;
    for (size_t li = 0; li < layer_values->elements.size(); ++li) {
        const Value& layer_value = layer_values->elements[li];
        if (!layer_value.is_dict()) {
            throw JitThrow{std::string(name) + "(): model.layers[" + std::to_string(li)
                           + "] must be a dict", line};
        }
        auto* layer_dict = layer_value.as_dict();
        const Value* weights_value = nn_dict_get(layer_dict, "weights");
        const Value* bias_value = nn_dict_get(layer_dict, "bias");
        const Value* activation_value = nn_dict_get(layer_dict, "activation");
        if (!weights_value || !weights_value->is_arr() || weights_value->as_arr()->elements.empty()) {
            throw JitThrow{std::string(name) + "(): layer " + std::to_string(li)
                           + " weights must be a non-empty matrix", line};
        }
        if (!bias_value || !bias_value->is_arr()) {
            throw JitThrow{std::string(name) + "(): layer " + std::to_string(li)
                           + " bias must be an array", line};
        }
        if (!activation_value || !activation_value->is_str()) {
            throw JitThrow{std::string(name) + "(): layer " + std::to_string(li)
                           + " activation must be a string", line};
        }

        auto* weight_rows = weights_value->as_arr();
        size_t output_size = weight_rows->elements.size();
        size_t input_size = 0;
        NnMatrix weights;
        weights.reserve(output_size);
        for (size_t oi = 0; oi < output_size; ++oi) {
            NnVector row = nn_parse_vector(name, weight_rows->elements[oi],
                                           "layer " + std::to_string(li) + " weights["
                                           + std::to_string(oi) + "]", input_size, line);
            if (oi == 0) {
                input_size = row.size();
                if (input_size == 0) {
                    throw JitThrow{std::string(name) + "(): layer weights cannot have empty rows", line};
                }
            }
            weights.push_back(std::move(row));
        }
        if (li > 0 && input_size != previous_output) {
            throw JitThrow{std::string(name) + "(): layer " + std::to_string(li)
                           + " expects " + std::to_string(input_size)
                           + " inputs but the previous layer outputs "
                           + std::to_string(previous_output), line};
        }
        NnVector bias = nn_parse_vector(name, *bias_value,
                                        "layer " + std::to_string(li) + " bias",
                                        output_size, line);
        parameters += output_size * input_size + output_size;
        if (parameters > NN_MAX_PARAMETERS) {
            throw JitThrow{std::string(name) + "(): model exceeds the "
                           + std::to_string(NN_MAX_PARAMETERS) + " parameter safety limit", line};
        }
        bool output_layer = li + 1 == layer_values->elements.size();
        std::string activation = nn_activation_name(name, activation_value->as_str(), output_layer, line);
        if (output_layer && activation == "softmax" && output_size < 2) {
            throw JitThrow{std::string(name) + "(): softmax output requires at least 2 units", line};
        }
        layers.push_back({std::move(weights), std::move(bias), std::move(activation)});
        previous_output = output_size;
    }
    return layers;
}

inline Value nn_vector_value(const NnVector& vector) {
    Value out = Value::make_array();
    out.as_arr()->elements.reserve(vector.size());
    for (double value : vector) out.as_arr()->elements.push_back(Value(value));
    return out;
}

inline Value nn_matrix_value(const NnMatrix& matrix) {
    Value out = Value::make_array();
    out.as_arr()->elements.reserve(matrix.size());
    for (const auto& row : matrix) out.as_arr()->elements.push_back(nn_vector_value(row));
    return out;
}

inline Value nn_model_value(const std::vector<NnLayer>& layers) {
    Value model = Value::make_dict();
    auto* dict = model.as_dict();
    dict->elements["format"] = Value(std::string("sura.nn.mlp.v1"));
    dict->elements["kind"] = Value(std::string("mlp"));
    dict->elements["layers"] = Value::make_array();
    auto* out_layers = dict->elements["layers"].as_arr();
    out_layers->elements.reserve(layers.size());
    for (const auto& layer : layers) {
        Value item = Value::make_dict();
        item.as_dict()->elements["weights"] = nn_matrix_value(layer.weights);
        item.as_dict()->elements["bias"] = nn_vector_value(layer.bias);
        item.as_dict()->elements["activation"] = Value(layer.activation);
        out_layers->elements.push_back(item);
    }
    return model;
}

inline NnVector nn_activate(const NnVector& input, const std::string& activation) {
    NnVector out(input.size(), 0.0);
    if (activation == "softmax") {
        if (input.empty()) return out;
        double maximum = *std::max_element(input.begin(), input.end());
        double total = 0.0;
        for (size_t i = 0; i < input.size(); ++i) {
            out[i] = std::exp(input[i] - maximum);
            total += out[i];
        }
        for (double& value : out) value /= total;
        return out;
    }
    for (size_t i = 0; i < input.size(); ++i) {
        double value = input[i];
        if (activation == "linear") out[i] = value;
        else if (activation == "relu") out[i] = std::max(0.0, value);
        else if (activation == "tanh") out[i] = std::tanh(value);
        else if (value >= 0.0) out[i] = 1.0 / (1.0 + std::exp(-value));
        else {
            double e = std::exp(value);
            out[i] = e / (1.0 + e);
        }
    }
    return out;
}

inline NnVector nn_activation_backward(const std::string& activation,
                                       const NnVector& preactivation,
                                       const NnVector& output,
                                       const NnVector& output_gradient) {
    NnVector gradient(output.size(), 0.0);
    if (activation == "softmax") {
        double dot = 0.0;
        for (size_t i = 0; i < output.size(); ++i) dot += output_gradient[i] * output[i];
        for (size_t i = 0; i < output.size(); ++i) {
            gradient[i] = output[i] * (output_gradient[i] - dot);
        }
        return gradient;
    }
    for (size_t i = 0; i < output.size(); ++i) {
        double derivative = 1.0;
        if (activation == "relu") derivative = preactivation[i] > 0.0 ? 1.0 : 0.0;
        else if (activation == "tanh") derivative = 1.0 - output[i] * output[i];
        else if (activation == "sigmoid") derivative = output[i] * (1.0 - output[i]);
        gradient[i] = output_gradient[i] * derivative;
    }
    return gradient;
}

inline NnVector nn_forward_one(const std::vector<NnLayer>& layers, const NnVector& input,
                               NnForwardCache* cache = nullptr) {
    NnVector current = input;
    if (cache) {
        cache->activations.clear();
        cache->preactivations.clear();
        cache->activations.push_back(current);
    }
    for (const auto& layer : layers) {
        NnVector z(layer.bias.size(), 0.0);
        for (size_t output = 0; output < layer.weights.size(); ++output) {
            double sum = layer.bias[output];
            for (size_t input_index = 0; input_index < current.size(); ++input_index) {
                sum += layer.weights[output][input_index] * current[input_index];
            }
            z[output] = sum;
        }
        current = nn_activate(z, layer.activation);
        if (cache) {
            cache->preactivations.push_back(std::move(z));
            cache->activations.push_back(current);
        }
    }
    return current;
}

inline NnMatrix nn_parse_inputs(const char* name, const Value& value, size_t input_size,
                                bool& single, bool allow_empty, int line) {
    if (!value.is_arr()) throw JitThrow{std::string(name) + "(): inputs must be an array", line};
    auto* array = value.as_arr();
    if (array->elements.empty()) {
        if (!allow_empty) throw JitThrow{std::string(name) + "(): inputs cannot be empty", line};
        single = false;
        return {};
    }
    single = !array->elements[0].is_arr();
    NnMatrix inputs;
    if (single) {
        inputs.push_back(nn_parse_vector(name, value, "input", input_size, line));
        return inputs;
    }
    inputs.reserve(array->elements.size());
    for (size_t i = 0; i < array->elements.size(); ++i) {
        inputs.push_back(nn_parse_vector(name, array->elements[i],
                                         "inputs[" + std::to_string(i) + "]", input_size, line));
    }
    return inputs;
}

inline NnMatrix nn_parse_targets(const char* name, const Value& value, size_t samples,
                                 size_t output_size, bool single_input, int line) {
    if (!value.is_arr()) throw JitThrow{std::string(name) + "(): targets must be an array", line};
    auto* array = value.as_arr();
    if (array->elements.empty()) throw JitThrow{std::string(name) + "(): targets cannot be empty", line};
    NnMatrix targets;

    bool nested = array->elements[0].is_arr();
    if (single_input && !nested && array->elements.size() == output_size) {
        targets.push_back(nn_parse_vector(name, value, "target", output_size, line));
        return targets;
    }
    if (nested) {
        if (array->elements.size() != samples) {
            throw JitThrow{std::string(name) + "(): target count must match input count", line};
        }
        targets.reserve(samples);
        for (size_t i = 0; i < samples; ++i) {
            targets.push_back(nn_parse_vector(name, array->elements[i],
                                              "targets[" + std::to_string(i) + "]",
                                              output_size, line));
        }
        return targets;
    }
    if (array->elements.size() != samples) {
        throw JitThrow{std::string(name) + "(): target count must match input count", line};
    }
    targets.reserve(samples);
    for (size_t i = 0; i < samples; ++i) {
        double raw = nn_finite_number(name, array->elements[i],
                                      "targets[" + std::to_string(i) + "]", line);
        if (output_size == 1) {
            targets.push_back({raw});
        } else {
            if (raw < 0.0 || raw != std::floor(raw) || raw >= (double)output_size) {
                throw JitThrow{std::string(name) + "(): class targets must be integer indexes from 0 to "
                               + std::to_string(output_size - 1), line};
            }
            NnVector one_hot(output_size, 0.0);
            one_hot[(size_t)raw] = 1.0;
            targets.push_back(std::move(one_hot));
        }
    }
    return targets;
}

inline std::string nn_loss_name(const char* name, std::string loss,
                                const std::string& output_activation,
                                size_t output_size, int line) {
    loss = nn_lower(loss);
    if (loss.empty() || loss == "auto") {
        if (output_activation == "sigmoid") return "binary_cross_entropy";
        if (output_activation == "softmax") return "categorical_cross_entropy";
        return "mse";
    }
    if (loss == "mean_squared_error" || loss == "l2") loss = "mse";
    if (loss == "bce" || loss == "binary_crossentropy") loss = "binary_cross_entropy";
    if (loss == "cce" || loss == "categorical_crossentropy") loss = "categorical_cross_entropy";
    if (loss == "cross_entropy") {
        loss = output_size == 1 ? "binary_cross_entropy" : "categorical_cross_entropy";
    }
    if (loss != "mse" && loss != "binary_cross_entropy" &&
        loss != "categorical_cross_entropy") {
        throw JitThrow{std::string(name) + "(): unsupported loss '" + loss
                       + "' (use auto, mse, binary_cross_entropy, or categorical_cross_entropy)", line};
    }
    return loss;
}

inline void nn_validate_targets(const char* name, const NnMatrix& targets,
                                const std::string& loss, int line) {
    if (loss == "binary_cross_entropy") {
        for (const auto& row : targets) {
            for (double value : row) {
                if (value < 0.0 || value > 1.0) {
                    throw JitThrow{std::string(name) + "(): binary targets must be between 0 and 1", line};
                }
            }
        }
    } else if (loss == "categorical_cross_entropy") {
        for (const auto& row : targets) {
            double total = 0.0;
            for (double value : row) {
                if (value < 0.0 || value > 1.0) {
                    throw JitThrow{std::string(name) + "(): categorical targets must be probabilities", line};
                }
                total += value;
            }
            if (std::abs(total - 1.0) > 0.000001) {
                throw JitThrow{std::string(name) + "(): each categorical target must sum to 1", line};
            }
        }
    }
}

inline void nn_validate_loss_activation(const char* name, const std::string& loss,
                                        const std::string& output_activation,
                                        size_t output_size, int line) {
    if (loss == "binary_cross_entropy" && output_activation != "sigmoid") {
        throw JitThrow{std::string(name) + "(): binary_cross_entropy requires a sigmoid output", line};
    }
    if (loss == "categorical_cross_entropy" && output_activation != "softmax") {
        throw JitThrow{std::string(name) + "(): categorical_cross_entropy requires a softmax output", line};
    }
    if (output_activation == "softmax" && output_size < 2) {
        throw JitThrow{std::string(name) + "(): softmax output requires at least 2 units", line};
    }
}

inline double nn_sample_loss(const NnVector& prediction, const NnVector& target,
                             const std::string& loss) {
    constexpr double epsilon = 1e-12;
    double total = 0.0;
    if (loss == "mse") {
        for (size_t i = 0; i < prediction.size(); ++i) {
            double difference = prediction[i] - target[i];
            total += difference * difference;
        }
        return total / (double)prediction.size();
    }
    if (loss == "binary_cross_entropy") {
        for (size_t i = 0; i < prediction.size(); ++i) {
            double probability = std::max(epsilon, std::min(1.0 - epsilon, prediction[i]));
            total -= target[i] * std::log(probability)
                   + (1.0 - target[i]) * std::log(1.0 - probability);
        }
        return total / (double)prediction.size();
    }
    for (size_t i = 0; i < prediction.size(); ++i) {
        double probability = std::max(epsilon, std::min(1.0, prediction[i]));
        total -= target[i] * std::log(probability);
    }
    return total;
}

inline double nn_dataset_loss(const std::vector<NnLayer>& layers, const NnMatrix& inputs,
                              const NnMatrix& targets, const std::string& loss) {
    double total = 0.0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        total += nn_sample_loss(nn_forward_one(layers, inputs[i]), targets[i], loss);
    }
    return total / (double)inputs.size();
}

inline NnGradients nn_zero_gradients(const std::vector<NnLayer>& layers) {
    NnGradients gradients;
    gradients.weights.reserve(layers.size());
    gradients.bias.reserve(layers.size());
    for (const auto& layer : layers) {
        gradients.weights.emplace_back(layer.weights.size(),
                                       NnVector(layer.weights[0].size(), 0.0));
        gradients.bias.emplace_back(layer.bias.size(), 0.0);
    }
    return gradients;
}

inline void nn_accumulate_gradients(const std::vector<NnLayer>& layers,
                                    const NnForwardCache& cache, const NnVector& target,
                                    const std::string& loss, NnGradients& gradients) {
    size_t last = layers.size() - 1;
    const NnVector& output = cache.activations.back();
    NnVector delta(output.size(), 0.0);
    bool sigmoid_bce = loss == "binary_cross_entropy" && layers[last].activation == "sigmoid";
    bool softmax_cce = loss == "categorical_cross_entropy" && layers[last].activation == "softmax";
    if (sigmoid_bce) {
        for (size_t i = 0; i < output.size(); ++i) {
            delta[i] = (output[i] - target[i]) / (double)output.size();
        }
    } else if (softmax_cce) {
        for (size_t i = 0; i < output.size(); ++i) delta[i] = output[i] - target[i];
    } else {
        constexpr double epsilon = 1e-12;
        NnVector output_gradient(output.size(), 0.0);
        if (loss == "mse") {
            for (size_t i = 0; i < output.size(); ++i) {
                output_gradient[i] = 2.0 * (output[i] - target[i]) / (double)output.size();
            }
        } else if (loss == "binary_cross_entropy") {
            for (size_t i = 0; i < output.size(); ++i) {
                double p = std::max(epsilon, std::min(1.0 - epsilon, output[i]));
                output_gradient[i] = (p - target[i]) / (p * (1.0 - p) * (double)output.size());
            }
        } else {
            for (size_t i = 0; i < output.size(); ++i) {
                output_gradient[i] = -target[i] / std::max(epsilon, output[i]);
            }
        }
        delta = nn_activation_backward(layers[last].activation, cache.preactivations[last],
                                       output, output_gradient);
    }

    for (size_t reverse = layers.size(); reverse-- > 0;) {
        size_t li = reverse;
        const NnVector& previous = cache.activations[li];
        for (size_t output_index = 0; output_index < delta.size(); ++output_index) {
            gradients.bias[li][output_index] += delta[output_index];
            for (size_t input_index = 0; input_index < previous.size(); ++input_index) {
                gradients.weights[li][output_index][input_index]
                    += delta[output_index] * previous[input_index];
            }
        }
        if (li == 0) break;
        NnVector previous_gradient(previous.size(), 0.0);
        for (size_t output_index = 0; output_index < delta.size(); ++output_index) {
            for (size_t input_index = 0; input_index < previous.size(); ++input_index) {
                previous_gradient[input_index]
                    += layers[li].weights[output_index][input_index] * delta[output_index];
            }
        }
        delta = nn_activation_backward(layers[li - 1].activation,
                                       cache.preactivations[li - 1],
                                       cache.activations[li], previous_gradient);
    }
}

inline void nn_prepare_gradients(NnGradients& gradients, const std::vector<NnLayer>& layers,
                                 double divisor, double weight_decay, double clip_norm) {
    double norm_squared = 0.0;
    for (size_t li = 0; li < layers.size(); ++li) {
        for (size_t oi = 0; oi < gradients.weights[li].size(); ++oi) {
            for (size_t ii = 0; ii < gradients.weights[li][oi].size(); ++ii) {
                double& value = gradients.weights[li][oi][ii];
                value = value / divisor + weight_decay * layers[li].weights[oi][ii];
                norm_squared += value * value;
            }
        }
        for (double& value : gradients.bias[li]) {
            value /= divisor;
            norm_squared += value * value;
        }
    }
    if (clip_norm > 0.0) {
        double norm = std::sqrt(norm_squared);
        if (norm > clip_norm) {
            double scale = clip_norm / norm;
            for (auto& matrix : gradients.weights)
                for (auto& row : matrix)
                    for (double& value : row) value *= scale;
            for (auto& row : gradients.bias)
                for (double& value : row) value *= scale;
        }
    }
}

inline void nn_update_parameters(std::vector<NnLayer>& layers, const NnGradients& gradients,
                                 NnGradients& first_moment, NnGradients& second_moment,
                                 const std::string& optimizer, double learning_rate,
                                 double momentum, double beta1, double beta2, double epsilon,
                                 size_t step) {
    double correction1 = optimizer == "adam" ? 1.0 - std::pow(beta1, (double)step) : 1.0;
    double correction2 = optimizer == "adam" ? 1.0 - std::pow(beta2, (double)step) : 1.0;
    for (size_t li = 0; li < layers.size(); ++li) {
        for (size_t oi = 0; oi < layers[li].weights.size(); ++oi) {
            for (size_t ii = 0; ii < layers[li].weights[oi].size(); ++ii) {
                double gradient = gradients.weights[li][oi][ii];
                double update = gradient;
                if (optimizer == "adam") {
                    double& m = first_moment.weights[li][oi][ii];
                    double& v = second_moment.weights[li][oi][ii];
                    m = beta1 * m + (1.0 - beta1) * gradient;
                    v = beta2 * v + (1.0 - beta2) * gradient * gradient;
                    update = (m / correction1) / (std::sqrt(v / correction2) + epsilon);
                } else if (momentum > 0.0) {
                    double& velocity = first_moment.weights[li][oi][ii];
                    velocity = momentum * velocity + gradient;
                    update = velocity;
                }
                layers[li].weights[oi][ii] -= learning_rate * update;
            }
        }
        for (size_t oi = 0; oi < layers[li].bias.size(); ++oi) {
            double gradient = gradients.bias[li][oi];
            double update = gradient;
            if (optimizer == "adam") {
                double& m = first_moment.bias[li][oi];
                double& v = second_moment.bias[li][oi];
                m = beta1 * m + (1.0 - beta1) * gradient;
                v = beta2 * v + (1.0 - beta2) * gradient * gradient;
                update = (m / correction1) / (std::sqrt(v / correction2) + epsilon);
            } else if (momentum > 0.0) {
                double& velocity = first_moment.bias[li][oi];
                velocity = momentum * velocity + gradient;
                update = velocity;
            }
            layers[li].bias[oi] -= learning_rate * update;
        }
    }
}

inline Value b_nn_mlp(const Value* a, int n, int l) {
    need_args("nn_mlp", n, 1, 2, l);
    auto* sizes_value = need_arr("nn_mlp", a[0], 0, l);
    if (sizes_value->elements.size() < 2 || sizes_value->elements.size() > NN_MAX_LAYERS + 1) {
        throw JitThrow{"nn_mlp(): layer_sizes must contain 2..65 positive integers", l};
    }
    std::vector<size_t> sizes;
    sizes.reserve(sizes_value->elements.size());
    for (size_t i = 0; i < sizes_value->elements.size(); ++i) {
        double raw = nn_finite_number("nn_mlp", sizes_value->elements[i],
                                      "layer_sizes[" + std::to_string(i) + "]", l);
        if (raw < 1.0 || raw != std::floor(raw) || raw > 1000000.0) {
            throw JitThrow{"nn_mlp(): layer sizes must be positive integers up to 1000000", l};
        }
        sizes.push_back((size_t)raw);
    }
    GCDict* options = nn_options("nn_mlp", a, n, 1, l);
    std::string hidden_activation = nn_activation_name(
        "nn_mlp", nn_option_string("nn_mlp", options, "activation", "relu", l), false, l);
    std::string task = nn_option_string("nn_mlp", options, "task",
                                        sizes.back() == 1 ? "binary" : "multiclass", l);
    if (task != "binary" && task != "multiclass" && task != "regression") {
        throw JitThrow{"nn_mlp(): option task must be binary, multiclass, or regression", l};
    }
    std::string default_output = task == "regression" ? "linear"
                               : (task == "multiclass" ? "softmax" : "sigmoid");
    std::string output_activation = nn_activation_name(
        "nn_mlp", nn_option_string("nn_mlp", options, "output_activation", default_output, l), true, l);
    if (task == "multiclass" && sizes.back() < 2) {
        throw JitThrow{"nn_mlp(): multiclass models require at least 2 output units", l};
    }
    if (output_activation == "softmax" && sizes.back() < 2) {
        throw JitThrow{"nn_mlp(): softmax output requires at least 2 units", l};
    }
    std::string initialization = nn_option_string("nn_mlp", options, "init", "auto", l);
    if (initialization != "auto" && initialization != "xavier" &&
        initialization != "he" && initialization != "zeros") {
        throw JitThrow{"nn_mlp(): option init must be auto, xavier, he, or zeros", l};
    }
    size_t seed = nn_option_integer("nn_mlp", options, "seed", 42, 0,
                                    (size_t)9007199254740991ULL, l);
    std::mt19937_64 generator((uint64_t)seed);
    std::vector<NnLayer> layers;
    layers.reserve(sizes.size() - 1);
    size_t parameters = 0;
    for (size_t li = 0; li + 1 < sizes.size(); ++li) {
        size_t input_size = sizes[li];
        size_t output_size = sizes[li + 1];
        parameters += input_size * output_size + output_size;
        if (parameters > NN_MAX_PARAMETERS) {
            throw JitThrow{"nn_mlp(): requested model exceeds the 5000000 parameter safety limit", l};
        }
        std::string activation = li + 2 == sizes.size() ? output_activation : hidden_activation;
        NnMatrix weights(output_size, NnVector(input_size, 0.0));
        if (initialization != "zeros") {
            bool use_he = initialization == "he"
                       || (initialization == "auto" && activation == "relu");
            if (use_he) {
                std::normal_distribution<double> distribution(0.0, std::sqrt(2.0 / (double)input_size));
                for (auto& row : weights) for (double& value : row) value = distribution(generator);
            } else {
                double limit = std::sqrt(6.0 / (double)(input_size + output_size));
                std::uniform_real_distribution<double> distribution(-limit, limit);
                for (auto& row : weights) for (double& value : row) value = distribution(generator);
            }
        }
        layers.push_back({std::move(weights), NnVector(output_size, 0.0), activation});
    }
    Value model = nn_model_value(layers);
    model.as_dict()->elements["task"] = Value(task);
    model.as_dict()->elements["seed"] = Value((double)seed);
    return model;
}

inline Value nn_forward_builtin(const char* name, const Value* a, int n, int l) {
    need_args(name, n, 2, 2, l);
    std::vector<NnLayer> layers = nn_parse_model(name, a[0], l);
    size_t input_size = layers.front().weights.front().size();
    bool single = false;
    NnMatrix inputs = nn_parse_inputs(name, a[1], input_size, single, true, l);
    NnMatrix outputs;
    outputs.reserve(inputs.size());
    for (const auto& input : inputs) outputs.push_back(nn_forward_one(layers, input));
    if (single) return nn_vector_value(outputs.front());
    return nn_matrix_value(outputs);
}

inline Value b_nn_forward(const Value* a, int n, int l) {
    return nn_forward_builtin("nn_forward", a, n, l);
}

inline Value b_nn_predict(const Value* a, int n, int l) {
    return nn_forward_builtin("nn_predict", a, n, l);
}

inline Value b_nn_train(const Value* a, int n, int l) {
    need_args("nn_train", n, 3, 4, l);
    std::vector<NnLayer> layers = nn_parse_model("nn_train", a[0], l);
    bool single = false;
    NnMatrix inputs = nn_parse_inputs("nn_train", a[1], layers.front().weights.front().size(),
                                      single, false, l);
    NnMatrix targets = nn_parse_targets("nn_train", a[2], inputs.size(),
                                        layers.back().bias.size(), single, l);
    GCDict* options = nn_options("nn_train", a, n, 3, l);
    std::string loss = nn_loss_name(
        "nn_train", nn_option_string("nn_train", options, "loss", "auto", l),
        layers.back().activation, layers.back().bias.size(), l);
    nn_validate_loss_activation("nn_train", loss, layers.back().activation,
                                layers.back().bias.size(), l);
    nn_validate_targets("nn_train", targets, loss, l);

    std::string optimizer = nn_option_string("nn_train", options, "optimizer", "adam", l);
    if (optimizer != "adam" && optimizer != "sgd") {
        throw JitThrow{"nn_train(): option optimizer must be adam or sgd", l};
    }
    size_t epochs = nn_option_integer("nn_train", options, "epochs", 1000, 1, 1000000, l);
    size_t default_batch = std::min<size_t>(32, inputs.size());
    size_t batch_size = nn_option_integer("nn_train", options, "batch_size", default_batch,
                                          1, inputs.size(), l);
    double learning_rate = nn_option_number("nn_train", options, "learning_rate",
                                            optimizer == "adam" ? 0.01 : 0.1,
                                            1e-12, 100.0, l);
    double momentum = nn_option_number("nn_train", options, "momentum", 0.9, 0.0, 0.999999, l);
    double beta1 = nn_option_number("nn_train", options, "beta1", 0.9, 0.0, 0.999999, l);
    double beta2 = nn_option_number("nn_train", options, "beta2", 0.999, 0.0, 0.999999999, l);
    double epsilon = nn_option_number("nn_train", options, "epsilon", 1e-8, 1e-16, 1.0, l);
    double weight_decay = nn_option_number("nn_train", options, "weight_decay", 0.0, 0.0, 100.0, l);
    double clip_norm = nn_option_number("nn_train", options, "clip_norm", 5.0, 0.0, 1e12, l);
    double min_delta = nn_option_number("nn_train", options, "min_delta", 1e-6, 0.0, 1e12, l);
    double target_loss = nn_option_number("nn_train", options, "target_loss", -1.0, -1.0, 1e12, l);
    size_t patience = nn_option_integer("nn_train", options, "patience", 0, 0, epochs, l);
    size_t history_every = nn_option_integer("nn_train", options, "history_every",
                                             std::max<size_t>(1, epochs / 100), 1, epochs, l);
    if (epochs / history_every + 2 > 100000) {
        throw JitThrow{"nn_train(): history options would exceed the 100000 point safety limit", l};
    }
    size_t seed = nn_option_integer("nn_train", options, "seed", 42, 0,
                                    (size_t)9007199254740991ULL, l);
    bool shuffle = nn_option_bool("nn_train", options, "shuffle", true, l);
    bool restore_best = nn_option_bool("nn_train", options, "restore_best", patience > 0, l);

    std::vector<size_t> order(inputs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::mt19937_64 generator((uint64_t)seed);
    NnGradients first_moment = nn_zero_gradients(layers);
    NnGradients second_moment = nn_zero_gradients(layers);
    size_t update_step = 0;
    size_t completed_epochs = 0;
    size_t stale_epochs = 0;
    double best_loss = std::numeric_limits<double>::infinity();
    std::vector<NnLayer> best_layers;
    std::vector<std::pair<size_t, double>> history;

    for (size_t epoch = 1; epoch <= epochs; ++epoch) {
        if (shuffle && order.size() > 1) std::shuffle(order.begin(), order.end(), generator);
        for (size_t start = 0; start < order.size(); start += batch_size) {
            size_t end = std::min(order.size(), start + batch_size);
            NnGradients gradients = nn_zero_gradients(layers);
            for (size_t position = start; position < end; ++position) {
                size_t sample = order[position];
                NnForwardCache cache;
                nn_forward_one(layers, inputs[sample], &cache);
                nn_accumulate_gradients(layers, cache, targets[sample], loss, gradients);
            }
            nn_prepare_gradients(gradients, layers, (double)(end - start), weight_decay, clip_norm);
            ++update_step;
            nn_update_parameters(layers, gradients, first_moment, second_moment,
                                 optimizer, learning_rate, momentum, beta1, beta2,
                                 epsilon, update_step);
        }
        completed_epochs = epoch;
        double epoch_loss = nn_dataset_loss(layers, inputs, targets, loss);
        if (!std::isfinite(epoch_loss)) {
            throw JitThrow{"nn_train(): training diverged; lower learning_rate or enable clip_norm", l};
        }
        bool improved = epoch_loss < best_loss - min_delta;
        if (improved) {
            best_loss = epoch_loss;
            stale_epochs = 0;
            if (restore_best) best_layers = layers;
        } else {
            ++stale_epochs;
        }
        if (epoch == 1 || epoch % history_every == 0 || epoch == epochs) {
            history.push_back({epoch, epoch_loss});
        }
        if ((target_loss >= 0.0 && epoch_loss <= target_loss) ||
            (patience > 0 && stale_epochs >= patience)) {
            if (history.empty() || history.back().first != epoch) history.push_back({epoch, epoch_loss});
            break;
        }
    }
    if (restore_best && !best_layers.empty()) layers = std::move(best_layers);
    double final_loss = nn_dataset_loss(layers, inputs, targets, loss);
    Value trained_model = Value::make_dict();
    trained_model.as_dict()->elements = a[0].as_dict()->elements;
    Value normalized_model = nn_model_value(layers);
    trained_model.as_dict()->elements["format"] = normalized_model.as_dict()->elements["format"];
    trained_model.as_dict()->elements["kind"] = normalized_model.as_dict()->elements["kind"];
    trained_model.as_dict()->elements["layers"] = normalized_model.as_dict()->elements["layers"];
    auto* model_dict = trained_model.as_dict();
    model_dict->elements["trained_epochs"] = Value((double)completed_epochs);
    model_dict->elements["last_loss"] = Value(final_loss);
    model_dict->elements["optimizer"] = Value(optimizer);

    Value result = Value::make_dict();
    auto* result_dict = result.as_dict();
    result_dict->elements["model"] = trained_model;
    result_dict->elements["loss"] = Value(final_loss);
    result_dict->elements["loss_name"] = Value(loss);
    result_dict->elements["epochs"] = Value((double)completed_epochs);
    result_dict->elements["samples"] = Value((double)inputs.size());
    result_dict->elements["optimizer"] = Value(optimizer);
    result_dict->elements["stopped_early"] = Value(completed_epochs < epochs);
    result_dict->elements["converged"] = Value(target_loss >= 0.0 && final_loss <= target_loss);
    result_dict->elements["history"] = Value::make_array();
    for (const auto& point : history) {
        Value item = Value::make_dict();
        item.as_dict()->elements["epoch"] = Value((double)point.first);
        item.as_dict()->elements["loss"] = Value(point.second);
        result_dict->elements["history"].as_arr()->elements.push_back(item);
    }
    return result;
}

inline Value b_nn_classify(const Value* a, int n, int l) {
    need_args("nn_classify", n, 2, 3, l);
    std::vector<NnLayer> layers = nn_parse_model("nn_classify", a[0], l);
    std::string activation = layers.back().activation;
    if (activation != "sigmoid" && activation != "softmax") {
        throw JitThrow{"nn_classify(): output activation must be sigmoid or softmax", l};
    }
    double threshold = n >= 3 ? need_num("nn_classify", a[2], 2, l) : 0.5;
    if (!std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0) {
        throw JitThrow{"nn_classify(): threshold must be between 0 and 1", l};
    }
    bool single = false;
    NnMatrix inputs = nn_parse_inputs("nn_classify", a[1], layers.front().weights.front().size(),
                                      single, true, l);
    Value labels = Value::make_array();
    for (const auto& input : inputs) {
        NnVector prediction = nn_forward_one(layers, input);
        if (activation == "softmax") {
            size_t label = (size_t)std::distance(prediction.begin(),
                                                 std::max_element(prediction.begin(), prediction.end()));
            labels.as_arr()->elements.push_back(Value((double)label));
        } else if (prediction.size() == 1) {
            labels.as_arr()->elements.push_back(Value(prediction[0] >= threshold ? 1.0 : 0.0));
        } else {
            Value row = Value::make_array();
            for (double probability : prediction) {
                row.as_arr()->elements.push_back(Value(probability >= threshold ? 1.0 : 0.0));
            }
            labels.as_arr()->elements.push_back(row);
        }
    }
    if (single) return labels.as_arr()->elements.front();
    return labels;
}

inline Value b_nn_evaluate(const Value* a, int n, int l) {
    need_args("nn_evaluate", n, 3, 4, l);
    std::vector<NnLayer> layers = nn_parse_model("nn_evaluate", a[0], l);
    bool single = false;
    NnMatrix inputs = nn_parse_inputs("nn_evaluate", a[1], layers.front().weights.front().size(),
                                      single, false, l);
    NnMatrix targets = nn_parse_targets("nn_evaluate", a[2], inputs.size(),
                                        layers.back().bias.size(), single, l);
    GCDict* options = nn_options("nn_evaluate", a, n, 3, l);
    std::string loss = nn_loss_name(
        "nn_evaluate", nn_option_string("nn_evaluate", options, "loss", "auto", l),
        layers.back().activation, layers.back().bias.size(), l);
    nn_validate_loss_activation("nn_evaluate", loss, layers.back().activation,
                                layers.back().bias.size(), l);
    nn_validate_targets("nn_evaluate", targets, loss, l);
    double threshold = nn_option_number("nn_evaluate", options, "threshold", 0.5, 0.0, 1.0, l);
    NnMatrix predictions;
    predictions.reserve(inputs.size());
    for (const auto& input : inputs) predictions.push_back(nn_forward_one(layers, input));

    Value result = Value::make_dict();
    auto* dict = result.as_dict();
    dict->elements["loss"] = Value(nn_dataset_loss(layers, inputs, targets, loss));
    dict->elements["loss_name"] = Value(loss);
    dict->elements["samples"] = Value((double)inputs.size());
    if (layers.back().activation == "softmax") {
        size_t correct = 0;
        for (size_t i = 0; i < predictions.size(); ++i) {
            size_t predicted = (size_t)std::distance(predictions[i].begin(),
                                                     std::max_element(predictions[i].begin(), predictions[i].end()));
            size_t expected = (size_t)std::distance(targets[i].begin(),
                                                    std::max_element(targets[i].begin(), targets[i].end()));
            if (predicted == expected) ++correct;
        }
        dict->elements["accuracy"] = Value((double)correct / (double)inputs.size());
    } else if (layers.back().activation == "sigmoid") {
        size_t correct = 0;
        size_t total = 0;
        for (size_t i = 0; i < predictions.size(); ++i) {
            for (size_t j = 0; j < predictions[i].size(); ++j) {
                if ((predictions[i][j] >= threshold) == (targets[i][j] >= threshold)) ++correct;
                ++total;
            }
        }
        dict->elements["accuracy"] = Value((double)correct / (double)total);
    } else {
        dict->elements["accuracy"] = Value::nil();
    }
    return result;
}

inline Value b_nn_summary(const Value* a, int n, int l) {
    need_args("nn_summary", n, 1, 1, l);
    std::vector<NnLayer> layers = nn_parse_model("nn_summary", a[0], l);
    Value summary = Value::make_dict();
    auto* dict = summary.as_dict();
    dict->elements["format"] = Value(std::string("sura.nn.mlp.v1"));
    dict->elements["kind"] = Value(std::string("mlp"));
    dict->elements["input_size"] = Value((double)layers.front().weights.front().size());
    dict->elements["output_size"] = Value((double)layers.back().bias.size());
    dict->elements["layer_count"] = Value((double)layers.size());
    dict->elements["parameters"] = Value((double)nn_parameter_count(layers));
    dict->elements["architecture"] = Value::make_array();
    dict->elements["activations"] = Value::make_array();
    dict->elements["architecture"].as_arr()->elements.push_back(
        Value((double)layers.front().weights.front().size()));
    for (const auto& layer : layers) {
        dict->elements["architecture"].as_arr()->elements.push_back(Value((double)layer.bias.size()));
        dict->elements["activations"].as_arr()->elements.push_back(Value(layer.activation));
    }
    return summary;
}

inline Value b_nn_one_hot(const Value* a, int n, int l) {
    need_args("nn_one_hot", n, 2, 2, l);
    auto* labels = need_arr("nn_one_hot", a[0], 0, l);
    double raw_classes = need_num("nn_one_hot", a[1], 1, l);
    if (!std::isfinite(raw_classes) || raw_classes < 2.0 || raw_classes != std::floor(raw_classes)
        || raw_classes > 1000000.0) {
        throw JitThrow{"nn_one_hot(): class_count must be an integer from 2 to 1000000", l};
    }
    size_t classes = (size_t)raw_classes;
    if (!labels->elements.empty() && classes > 10000000 / labels->elements.size()) {
        throw JitThrow{"nn_one_hot(): output exceeds the 10000000 element safety limit", l};
    }
    Value out = Value::make_array();
    out.as_arr()->elements.reserve(labels->elements.size());
    for (size_t i = 0; i < labels->elements.size(); ++i) {
        double raw = nn_finite_number("nn_one_hot", labels->elements[i],
                                      "labels[" + std::to_string(i) + "]", l);
        if (raw < 0.0 || raw != std::floor(raw) || raw >= raw_classes) {
            throw JitThrow{"nn_one_hot(): labels must be valid zero-based class indexes", l};
        }
        NnVector row(classes, 0.0);
        row[(size_t)raw] = 1.0;
        out.as_arr()->elements.push_back(nn_vector_value(row));
    }
    return out;
}

inline NnMatrix nn_feature_matrix(const char* name, const Value& value,
                                  size_t& feature_count, int line) {
    if (!value.is_arr() || value.as_arr()->elements.empty()) {
        throw JitThrow{std::string(name) + "(): inputs must be a non-empty matrix", line};
    }
    const Value& first = value.as_arr()->elements[0];
    if (!first.is_arr() || first.as_arr()->elements.empty()) {
        throw JitThrow{std::string(name) + "(): inputs must contain non-empty feature rows", line};
    }
    feature_count = first.as_arr()->elements.size();
    bool single = false;
    NnMatrix matrix = nn_parse_inputs(name, value, feature_count, single, false, line);
    if (single) throw JitThrow{std::string(name) + "(): inputs must be a matrix", line};
    return matrix;
}

inline Value b_nn_fit_standardizer(const Value* a, int n, int l) {
    need_args("nn_fit_standardizer", n, 1, 1, l);
    size_t features = 0;
    NnMatrix inputs = nn_feature_matrix("nn_fit_standardizer", a[0], features, l);
    NnVector mean(features, 0.0);
    NnVector scale(features, 0.0);
    for (const auto& row : inputs) {
        for (size_t feature = 0; feature < features; ++feature) mean[feature] += row[feature];
    }
    for (double& value : mean) value /= (double)inputs.size();
    for (const auto& row : inputs) {
        for (size_t feature = 0; feature < features; ++feature) {
            double difference = row[feature] - mean[feature];
            scale[feature] += difference * difference;
        }
    }
    for (double& value : scale) {
        value = std::sqrt(value / (double)inputs.size());
        if (value < 1e-12) value = 1.0;
    }
    Value standardizer = Value::make_dict();
    auto* dict = standardizer.as_dict();
    dict->elements["format"] = Value(std::string("sura.nn.standardizer.v1"));
    dict->elements["mean"] = nn_vector_value(mean);
    dict->elements["scale"] = nn_vector_value(scale);
    dict->elements["samples"] = Value((double)inputs.size());
    dict->elements["features"] = Value((double)features);
    return standardizer;
}

inline Value b_nn_standardize(const Value* a, int n, int l) {
    need_args("nn_standardize", n, 2, 2, l);
    auto* standardizer = need_dict("nn_standardize", a[1], 1, l);
    if (const Value* format = nn_dict_get(standardizer, "format")) {
        if (!format->is_str() || format->as_str() != "sura.nn.standardizer.v1") {
            throw JitThrow{"nn_standardize(): unsupported standardizer format", l};
        }
    }
    const Value* mean_value = nn_dict_get(standardizer, "mean");
    const Value* scale_value = nn_dict_get(standardizer, "scale");
    if (!mean_value || !scale_value || !mean_value->is_arr() || mean_value->as_arr()->elements.empty()) {
        throw JitThrow{"nn_standardize(): standardizer requires non-empty mean and scale arrays", l};
    }
    size_t features = mean_value->as_arr()->elements.size();
    NnVector mean = nn_parse_vector("nn_standardize", *mean_value, "standardizer.mean", features, l);
    NnVector scale = nn_parse_vector("nn_standardize", *scale_value, "standardizer.scale", features, l);
    for (double value : scale) {
        if (value <= 0.0) throw JitThrow{"nn_standardize(): standardizer scales must be positive", l};
    }
    bool single = false;
    NnMatrix inputs = nn_parse_inputs("nn_standardize", a[0], features, single, true, l);
    for (auto& row : inputs) {
        for (size_t feature = 0; feature < features; ++feature) {
            row[feature] = (row[feature] - mean[feature]) / scale[feature];
        }
    }
    if (single) return nn_vector_value(inputs.front());
    return nn_matrix_value(inputs);
}

inline Value b_nn_split(const Value* a, int n, int l) {
    need_args("nn_split", n, 2, 3, l);
    auto* inputs = need_arr("nn_split", a[0], 0, l);
    auto* targets = need_arr("nn_split", a[1], 1, l);
    if (inputs->elements.size() != targets->elements.size()) {
        throw JitThrow{"nn_split(): input and target counts must match", l};
    }
    if (inputs->elements.size() < 2) {
        throw JitThrow{"nn_split(): dataset must contain at least 2 samples", l};
    }
    GCDict* options = nn_options("nn_split", a, n, 2, l);
    double ratio = nn_option_number("nn_split", options, "test_ratio", 0.2, 0.0, 1.0, l);
    if (ratio <= 0.0 || ratio >= 1.0) {
        throw JitThrow{"nn_split(): option test_ratio must be greater than 0 and less than 1", l};
    }
    size_t samples = inputs->elements.size();
    size_t test_count = (size_t)std::ceil((double)samples * ratio);
    if (options && nn_dict_get(options, "test_count")) {
        test_count = nn_option_integer("nn_split", options, "test_count", test_count,
                                       1, samples - 1, l);
    }
    test_count = std::max<size_t>(1, std::min(samples - 1, test_count));
    size_t seed = nn_option_integer("nn_split", options, "seed", 42, 0,
                                    (size_t)9007199254740991ULL, l);
    bool shuffle = nn_option_bool("nn_split", options, "shuffle", true, l);
    std::vector<size_t> order(samples);
    for (size_t i = 0; i < samples; ++i) order[i] = i;
    if (shuffle) {
        std::mt19937_64 generator((uint64_t)seed);
        std::shuffle(order.begin(), order.end(), generator);
    }

    Value train = Value::make_dict();
    Value test = Value::make_dict();
    train.as_dict()->elements["inputs"] = Value::make_array();
    train.as_dict()->elements["targets"] = Value::make_array();
    test.as_dict()->elements["inputs"] = Value::make_array();
    test.as_dict()->elements["targets"] = Value::make_array();
    size_t train_count = samples - test_count;
    for (size_t position = 0; position < samples; ++position) {
        size_t index = order[position];
        Value& part = position < train_count ? train : test;
        part.as_dict()->elements["inputs"].as_arr()->elements.push_back(inputs->elements[index]);
        part.as_dict()->elements["targets"].as_arr()->elements.push_back(targets->elements[index]);
    }
    Value result = Value::make_dict();
    result.as_dict()->elements["train"] = train;
    result.as_dict()->elements["test"] = test;
    result.as_dict()->elements["train_size"] = Value((double)train_count);
    result.as_dict()->elements["test_size"] = Value((double)test_count);
    result.as_dict()->elements["seed"] = Value((double)seed);
    return result;
}

inline Value b_nn_save(const Value* a, int n, int l) {
    need_args("nn_save", n, 2, 2, l);
    nn_parse_model("nn_save", a[0], l);
    std::string path = need_str("nn_save", a[1], 1, l);
    Value args[2] = {Value(path), a[0]};
    return b_file_write_json(args, 2, l);
}

inline Value b_nn_load(const Value* a, int n, int l) {
    need_args("nn_load", n, 1, 1, l);
    Value loaded = b_file_read_json(a, 1, l);
    nn_parse_model("nn_load", loaded, l);
    return loaded;
}

} // namespace SuraStd

#include "cuda_backend.hpp"
#include "autograd.hpp"
#include "checkpoint.hpp"
#include "safetensors.hpp"
#include "onnx_weights.hpp"
#include "distributed.hpp"
#include "tokenizer.hpp"
#include "dataset.hpp"
#include "media.hpp"

namespace SuraStd {

inline Value make_stream_from_array(const Value& data) {
    Value stream = Value::make_dict();
    auto* d = stream.as_dict();
    d->elements["type"] = Value(std::string("stream"));
    d->elements["index"] = Value(0.0);
    d->elements["data"] = data;
    return stream;
}

inline void need_stream_state(const char* name, const Value& stream, int line,
                              GCDict*& dict, GCArray*& data, size_t& index) {
    if (!stream.is_dict()) throw JitThrow{std::string(name) + "(): expected stream dict", line};
    dict = stream.as_dict();
    auto data_it = dict->elements.find("data");
    auto index_it = dict->elements.find("index");
    if (data_it == dict->elements.end() || index_it == dict->elements.end() ||
        !data_it->second.is_arr() || !index_it->second.is_num()) {
        throw JitThrow{std::string(name) + "(): invalid stream", line};
    }
    double raw_index = index_it->second.as_num();
    if (raw_index < 0) throw JitThrow{std::string(name) + "(): invalid stream index", line};
    data = data_it->second.as_arr();
    index = (size_t)raw_index;
}

inline size_t need_non_negative_count(const char* name, const Value& value, int idx, int line) {
    double raw = need_num(name, value, idx, line);
    if (raw < 0) throw JitThrow{std::string(name) + "(): count must be non-negative", line};
    return (size_t)raw;
}

inline Value b_stream_from(const Value* a, int n, int l) {
    need_args("stream_from", n, 1, 1, l);
    if (a[0].is_arr()) return make_stream_from_array(a[0]);
    if (a[0].is_str()) {
        std::istringstream ss(a[0].as_str());
        Value lines = Value::make_array();
        std::string line;
        while (std::getline(ss, line)) lines.as_arr()->elements.push_back(Value(line));
        return make_stream_from_array(lines);
    }
    throw JitThrow{"stream_from(): expected array or string", l};
}

inline Value b_stream_next(const Value* a, int n, int l) {
    need_args("stream_next", n, 1, 1, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_next", a[0], l, d, data, idx);
    if (idx >= data->elements.size()) return Value::nil();
    d->elements["index"] = Value((double)(idx + 1));
    return data->elements[idx];
}

inline Value b_stream_collect(const Value* a, int n, int l) {
    need_args("stream_collect", n, 1, 1, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_collect", a[0], l, d, data, idx);
    Value out = Value::make_array();
    while (idx < data->elements.size()) out.as_arr()->elements.push_back(data->elements[idx++]);
    d->elements["index"] = Value((double)data->elements.size());
    return out;
}

inline Value b_stream_take(const Value* a, int n, int l) {
    need_args("stream_take", n, 2, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_take", a[0], l, d, data, idx);
    size_t count = need_non_negative_count("stream_take", a[1], 1, l);
    size_t available = idx >= data->elements.size() ? 0 : data->elements.size() - idx;
    size_t end = idx + std::min(count, available);
    Value out = Value::make_array();
    while (idx < end) out.as_arr()->elements.push_back(data->elements[idx++]);
    d->elements["index"] = Value((double)idx);
    return out;
}

inline Value b_stream_batch(const Value* a, int n, int l) {
    need_args("stream_batch", n, 2, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_batch", a[0], l, d, data, idx);
    size_t size = need_non_negative_count("stream_batch", a[1], 1, l);
    if (size == 0) throw JitThrow{"stream_batch(): size must be positive", l};
    Value out = Value::make_array();
    while (idx < data->elements.size()) {
        Value batch = Value::make_array();
        size_t end = std::min(idx + size, data->elements.size());
        while (idx < end) batch.as_arr()->elements.push_back(data->elements[idx++]);
        out.as_arr()->elements.push_back(batch);
    }
    d->elements["index"] = Value((double)data->elements.size());
    return out;
}

inline Value b_stream_map(const Value* a, int n, int l) {
    need_args("stream_map", n, 2, 3, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_map", a[0], l, d, data, idx);
    std::string path = need_str("stream_map", a[1], 1, l);
    const Value* fallback = n >= 3 ? &a[2] : nullptr;
    Value out = Value::make_array();
    while (idx < data->elements.size()) {
        out.as_arr()->elements.push_back(json_path_lookup(data->elements[idx++], path, fallback, l, "stream_map"));
    }
    d->elements["index"] = Value((double)data->elements.size());
    return make_stream_from_array(out);
}

inline Value b_stream_filter(const Value* a, int n, int l) {
    need_args("stream_filter", n, 2, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_filter", a[0], l, d, data, idx);
    GCDict* criteria = db_need_dict("stream_filter", a[1], 1, l);
    Value out = Value::make_array();
    while (idx < data->elements.size()) {
        const Value& row = data->elements[idx++];
        if (db_row_matches(row, criteria)) out.as_arr()->elements.push_back(row);
    }
    d->elements["index"] = Value((double)data->elements.size());
    return make_stream_from_array(out);
}

inline Value b_stream_window(const Value* a, int n, int l) {
    need_args("stream_window", n, 2, 3, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_window", a[0], l, d, data, idx);
    size_t size = need_non_negative_count("stream_window", a[1], 1, l);
    size_t step = n >= 3 ? need_non_negative_count("stream_window", a[2], 2, l) : 1;
    if (size == 0) throw JitThrow{"stream_window(): size must be positive", l};
    if (step == 0) throw JitThrow{"stream_window(): step must be positive", l};
    Value out = Value::make_array();
    for (size_t start = idx; start + size <= data->elements.size(); start += step) {
        Value window = Value::make_array();
        for (size_t offset = 0; offset < size; ++offset) {
            window.as_arr()->elements.push_back(data->elements[start + offset]);
        }
        out.as_arr()->elements.push_back(window);
    }
    d->elements["index"] = Value((double)data->elements.size());
    return out;
}

inline Value b_stream_skip(const Value* a, int n, int l) {
    need_args("stream_skip", n, 2, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_skip", a[0], l, d, data, idx);
    size_t count = need_non_negative_count("stream_skip", a[1], 1, l);
    size_t available = idx >= data->elements.size() ? 0 : data->elements.size() - idx;
    size_t next = idx + std::min(count, available);
    size_t skipped = next > idx ? next - idx : 0;
    d->elements["index"] = Value((double)next);
    return Value((double)skipped);
}

inline Value b_stream_count(const Value* a, int n, int l) {
    need_args("stream_count", n, 1, 1, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_count", a[0], l, d, data, idx);
    size_t remaining = idx >= data->elements.size() ? 0 : data->elements.size() - idx;
    return Value((double)remaining);
}

inline Value b_stream_join(const Value* a, int n, int l) {
    need_args("stream_join", n, 1, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_join", a[0], l, d, data, idx);
    std::string sep = n >= 2 ? need_str("stream_join", a[1], 1, l) : "";
    std::string out;
    bool first = true;
    while (idx < data->elements.size()) {
        if (!first) out += sep;
        out += data->elements[idx++].to_str();
        first = false;
    }
    d->elements["index"] = Value((double)data->elements.size());
    return Value(out);
}

inline double stream_numeric_value(const char* name, const Value& item, const std::string* path, int line) {
    Value value = path ? json_path_lookup(item, *path, nullptr, line, name) : item;
    if (!value.is_num()) throw JitThrow{std::string(name) + "(): stream value must be a number, got " + value.to_str(), line};
    return value.as_num();
}

inline Value b_stream_sum(const Value* a, int n, int l) {
    need_args("stream_sum", n, 1, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_sum", a[0], l, d, data, idx);
    std::string path;
    const std::string* path_ptr = nullptr;
    if (n >= 2) {
        path = need_str("stream_sum", a[1], 1, l);
        path_ptr = &path;
    }
    double sum = 0.0;
    while (idx < data->elements.size()) sum += stream_numeric_value("stream_sum", data->elements[idx++], path_ptr, l);
    d->elements["index"] = Value((double)data->elements.size());
    return Value(sum);
}

inline Value b_stream_avg(const Value* a, int n, int l) {
    need_args("stream_avg", n, 1, 2, l);
    GCDict* d = nullptr;
    GCArray* data = nullptr;
    size_t idx = 0;
    need_stream_state("stream_avg", a[0], l, d, data, idx);
    std::string path;
    const std::string* path_ptr = nullptr;
    if (n >= 2) {
        path = need_str("stream_avg", a[1], 1, l);
        path_ptr = &path;
    }
    double sum = 0.0;
    size_t count = 0;
    while (idx < data->elements.size()) {
        sum += stream_numeric_value("stream_avg", data->elements[idx++], path_ptr, l);
        ++count;
    }
    d->elements["index"] = Value((double)data->elements.size());
    return count == 0 ? Value::nil() : Value(sum / (double)count);
}

inline Value b_stream_lines(const Value* a, int n, int l) {
    need_args("stream_lines", n, 1, 1, l);
    std::istringstream ss(read_text_file(need_str("stream_lines", a[0], 0, l), l));
    Value lines = Value::make_array();
    std::string line;
    while (std::getline(ss, line)) lines.as_arr()->elements.push_back(Value(line));
    return make_stream_from_array(lines);
}

inline Value b_llm_message(const Value* a, int n, int l) {
    need_args("llm_message", n, 2, 2, l);
    Value msg = Value::make_dict();
    auto* d = msg.as_dict();
    d->elements["role"] = Value(need_str("llm_message", a[0], 0, l));
    d->elements["content"] = Value(need_str("llm_message", a[1], 1, l));
    return msg;
}

inline Value b_llm_messages(const Value* a, int n, int l) {
    need_args("llm_messages", n, 1, 2, l);
    Value out = Value::make_array();
    if (n >= 2 && !a[0].to_str().empty()) {
        Value sys_args[2] = {Value(std::string("system")), Value(a[0].to_str())};
        out.as_arr()->elements.push_back(b_llm_message(sys_args, 2, l));
        Value user_args[2] = {Value(std::string("user")), Value(a[1].to_str())};
        out.as_arr()->elements.push_back(b_llm_message(user_args, 2, l));
    } else {
        Value user_args[2] = {Value(std::string("user")), Value(a[0].to_str())};
        out.as_arr()->elements.push_back(b_llm_message(user_args, 2, l));
    }
    return out;
}

inline Value b_rag_messages(const Value* a, int n, int l) {
    need_args("rag_messages", n, 2, 3, l);
    std::string question = need_str("rag_messages", a[0], 0, l);
    std::string context = need_str("rag_messages", a[1], 1, l);
    std::string system = n >= 3
        ? need_str("rag_messages", a[2], 2, l)
        : std::string("Answer using only the provided context. If the context is insufficient, say you do not know.");
    std::string user = "Context:\n" + context + "\n\nQuestion:\n" + question;
    Value args[2] = {Value(system), Value(user)};
    return b_llm_messages(args, 2, l);
}

inline Value b_llm_request(const Value* a, int n, int l) {
    need_args("llm_request", n, 2, 3, l);
    need_str("llm_request", a[0], 0, l);
    need_arr("llm_request", a[1], 1, l);
    Value req = Value::make_dict();
    auto* d = req.as_dict();
    d->elements["model"] = a[0];
    d->elements["messages"] = a[1];
    d->elements["temperature"] = Value(n >= 3 ? need_num("llm_request", a[2], 2, l) : 0.2);
    return req;
}

inline Value b_llm_request_json(const Value* a, int n, int l) {
    Value req = b_llm_request(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value b_llm_response_schema(const Value* a, int n, int l) {
    need_args("llm_response_schema", n, 2, 3, l);
    std::string name = need_str("llm_response_schema", a[0], 0, l);
    if (name.empty()) throw JitThrow{"llm_response_schema(): name must not be empty", l};
    if (!a[1].is_str() && !a[1].is_dict())
        throw JitThrow{"llm_response_schema(): schema must be a type string or dict", l};
    bool strict = n >= 3 ? a[2].truthy() : true;

    Value json_schema = Value::make_dict();
    auto* jd = json_schema.as_dict();
    jd->elements["name"] = Value(name);
    jd->elements["strict"] = Value(strict);
    jd->elements["schema"] = schema_to_json_schema_value(a[1], strict);

    Value response_format = Value::make_dict();
    auto* rd = response_format.as_dict();
    rd->elements["type"] = Value(std::string("json_schema"));
    rd->elements["json_schema"] = json_schema;
    return response_format;
}

inline Value b_llm_request_schema(const Value* a, int n, int l) {
    need_args("llm_request_schema", n, 3, 6, l);
    Value req_args[3] = {a[0], a[1], n >= 4 ? a[3] : Value(0.2)};
    Value req = b_llm_request(req_args, 3, l);
    std::string name = n >= 5 ? need_str("llm_request_schema", a[4], 4, l) : std::string("sura_response");
    bool strict = n >= 6 ? a[5].truthy() : true;
    Value schema_args[3] = {Value(name), a[2], Value(strict)};
    req.as_dict()->elements["response_format"] = b_llm_response_schema(schema_args, 3, l);
    return req;
}

inline Value b_llm_request_schema_json(const Value* a, int n, int l) {
    Value req = b_llm_request_schema(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value llm_json_type_array(const std::vector<std::string>& types) {
    Value out = Value::make_array();
    for (const auto& type : types) out.as_arr()->elements.push_back(Value(type));
    return out;
}

inline Value llm_tool_field_schema(const std::string& field, const std::string& spec) {
    Value prop = Value::make_dict();
    auto* d = prop.as_dict();
    d->elements["description"] = Value(spec);
    if (spec.find("number") != std::string::npos) {
        d->elements["type"] = Value("number");
    } else if (spec.find("dict") != std::string::npos) {
        d->elements["type"] = Value("object");
        if (field == "headers") {
            Value additional = Value::make_dict();
            additional.as_dict()->elements["type"] = Value("string");
            d->elements["additionalProperties"] = additional;
        } else {
            d->elements["additionalProperties"] = Value(true);
        }
    } else if (spec.find("json-value") != std::string::npos) {
        d->elements["type"] = llm_json_type_array({"string", "number", "boolean", "object", "array", "null"});
    } else {
        d->elements["type"] = Value("string");
    }
    if (field == "method") {
        Value methods = Value::make_array();
        auto* arr = methods.as_arr();
        arr->elements.push_back(Value("GET"));
        arr->elements.push_back(Value("POST"));
        arr->elements.push_back(Value("PUT"));
        arr->elements.push_back(Value("PATCH"));
        arr->elements.push_back(Value("DELETE"));
        d->elements["enum"] = methods;
    }
    return prop;
}

inline std::string llm_tool_description(const std::string& name) {
    if (name == "http_get") return "Fetch a URL and return the response body as text.";
    if (name == "http_request") return "Send an HTTP request with method, URL, headers, query, body, JSON, and timeout fields.";
    if (name == "shell") return "Run a shell command when an explicit tool policy allows shell execution.";
    return "Sura automation tool.";
}

inline Value llm_openai_tool_schema(const std::string& name, int line) {
    Value schema = tool_schema_for(name, line);
    auto* root = schema.as_dict();
    auto* fields = root->elements["fields"].as_dict();

    Value properties = Value::make_dict();
    auto* props = properties.as_dict();
    for (const auto& [field, spec_value] : fields->elements) {
        props->elements[field] = llm_tool_field_schema(field, spec_value.to_str());
    }

    Value parameters = Value::make_dict();
    auto* params = parameters.as_dict();
    params->elements["type"] = Value("object");
    params->elements["properties"] = properties;
    params->elements["required"] = root->elements["required"];
    params->elements["additionalProperties"] = Value(false);

    Value fn = Value::make_dict();
    auto* f = fn.as_dict();
    f->elements["name"] = Value(name);
    f->elements["description"] = Value(llm_tool_description(name));
    f->elements["parameters"] = parameters;

    Value tool = Value::make_dict();
    auto* t = tool.as_dict();
    t->elements["type"] = Value("function");
    t->elements["function"] = fn;
    return tool;
}

inline Value b_llm_tools(const Value* a, int n, int l) {
    need_args("llm_tools", n, 0, 1, l);
    Value names = n >= 1 ? a[0] : Value::nil();
    if (names.is_nil()) {
        names = Value::make_array();
        auto* arr = names.as_arr();
        arr->elements.push_back(Value("http_get"));
        arr->elements.push_back(Value("http_request"));
        arr->elements.push_back(Value("shell"));
    } else if (names.is_str()) {
        Value single = Value::make_array();
        single.as_arr()->elements.push_back(names);
        names = single;
    } else if (!names.is_arr()) {
        throw JitThrow{"llm_tools(): expected nil, string, or array of tool names", l};
    }

    Value out = Value::make_array();
    auto* arr = out.as_arr();
    std::unordered_set<std::string> seen;
    for (const auto& name_value : names.as_arr()->elements) {
        std::string name = name_value.to_str();
        if (name.empty() || seen.count(name)) continue;
        seen.insert(name);
        arr->elements.push_back(llm_openai_tool_schema(name, l));
    }
    return out;
}

inline Value b_llm_request_tools(const Value* a, int n, int l) {
    need_args("llm_request_tools", n, 3, 4, l);
    Value req_args[3] = {a[0], a[1], n >= 4 ? a[3] : Value(0.2)};
    Value req = b_llm_request(req_args, 3, l);
    Value tools_args[1] = {a[2]};
    req.as_dict()->elements["tools"] = b_llm_tools(tools_args, 1, l);
    return req;
}

inline Value b_llm_request_tools_json(const Value* a, int n, int l) {
    Value req = b_llm_request_tools(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value b_llm_request_tools_schema(const Value* a, int n, int l) {
    need_args("llm_request_tools_schema", n, 4, 7, l);
    Value req_args[3] = {a[0], a[1], n >= 5 ? a[4] : Value(0.2)};
    Value req = b_llm_request(req_args, 3, l);
    Value tools_args[1] = {a[2]};
    req.as_dict()->elements["tools"] = b_llm_tools(tools_args, 1, l);
    std::string name = n >= 6 ? need_str("llm_request_tools_schema", a[5], 5, l) : std::string("sura_response");
    bool strict = n >= 7 ? a[6].truthy() : true;
    Value schema_args[3] = {Value(name), a[3], Value(strict)};
    req.as_dict()->elements["response_format"] = b_llm_response_schema(schema_args, 3, l);
    return req;
}

inline Value b_llm_request_tools_schema_json(const Value* a, int n, int l) {
    Value req = b_llm_request_tools_schema(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value b_llm_extract_text(const Value* a, int n, int l) {
    need_args("llm_extract_text", n, 1, 1, l);
    Value response = a[0];
    if (response.is_str()) response = JsonParser(response.as_str(), l).parse_document();
    if (!response.is_dict()) return Value(std::string(""));
    auto* root = response.as_dict();
    auto choices_it = root->elements.find("choices");
    if (choices_it == root->elements.end() || !choices_it->second.is_arr()) return Value(std::string(""));
    auto* choices = choices_it->second.as_arr();
    if (choices->elements.empty() || !choices->elements[0].is_dict()) return Value(std::string(""));
    auto* choice = choices->elements[0].as_dict();
    auto msg_it = choice->elements.find("message");
    if (msg_it != choice->elements.end() && msg_it->second.is_dict()) {
        auto* msg = msg_it->second.as_dict();
        auto content_it = msg->elements.find("content");
        if (content_it != msg->elements.end()) return Value(content_it->second.to_str());
    }
    auto text_it = choice->elements.find("text");
    if (text_it != choice->elements.end()) return Value(text_it->second.to_str());
    return Value(std::string(""));
}

inline Value b_llm_extract_json(const Value* a, int n, int l) {
    need_args("llm_extract_json", n, 1, 2, l);
    Value text_arg[1] = {a[0]};
    Value text = b_llm_extract_text(text_arg, 1, l);
    Value parsed = JsonParser(text.as_str(), l).parse_document();
    if (n >= 2) {
        Value schema_args[2] = {parsed, a[1]};
        Value errors = b_schema_errors(schema_args, 2, l);
        auto* arr = errors.as_arr();
        if (!arr->elements.empty()) {
            std::ostringstream joined;
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                if (i) joined << " | ";
                joined << arr->elements[i].to_str();
            }
            throw JitThrow{"llm_extract_json(): schema validation failed: " + joined.str(), l};
        }
    }
    return parsed;
}

inline double llm_usage_number(GCDict* usage, const std::vector<std::string>& keys) {
    if (!usage) return 0.0;
    for (const auto& key : keys) {
        auto it = usage->elements.find(key);
        if (it != usage->elements.end() && it->second.is_num()) return it->second.as_num();
    }
    return 0.0;
}

inline Value b_llm_usage(const Value* a, int n, int l) {
    need_args("llm_usage", n, 1, 1, l);
    Value response = a[0];
    if (response.is_str()) response = JsonParser(response.as_str(), l).parse_document();

    GCDict* usage = nullptr;
    if (response.is_dict()) {
        auto* root = response.as_dict();
        auto it = root->elements.find("usage");
        if (it != root->elements.end() && it->second.is_dict()) usage = it->second.as_dict();
        else usage = root;
    }

    double prompt = llm_usage_number(usage, {"prompt_tokens", "input_tokens"});
    double completion = llm_usage_number(usage, {"completion_tokens", "output_tokens"});
    double input = llm_usage_number(usage, {"input_tokens", "prompt_tokens"});
    double output = llm_usage_number(usage, {"output_tokens", "completion_tokens"});
    double total = llm_usage_number(usage, {"total_tokens"});
    if (total == 0.0 && (prompt != 0.0 || completion != 0.0)) total = prompt + completion;

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["prompt_tokens"] = Value(prompt);
    d->elements["completion_tokens"] = Value(completion);
    d->elements["input_tokens"] = Value(input);
    d->elements["output_tokens"] = Value(output);
    d->elements["total_tokens"] = Value(total);
    return out;
}

inline double llm_price_number(GCDict* pricing, const std::vector<std::string>& keys) {
    if (!pricing) return 0.0;
    for (const auto& key : keys) {
        auto it = pricing->elements.find(key);
        if (it != pricing->elements.end() && it->second.is_num()) return it->second.as_num();
    }
    return 0.0;
}

inline Value b_llm_cost(const Value* a, int n, int l) {
    need_args("llm_cost", n, 2, 2, l);
    if (!a[1].is_dict()) throw JitThrow{"llm_cost(): pricing must be a dict", l};

    Value usage_args[1] = {a[0]};
    Value usage_value = b_llm_usage(usage_args, 1, l);
    auto* usage = usage_value.as_dict();
    auto* pricing = a[1].as_dict();

    double input_tokens = usage->elements["input_tokens"].as_num();
    double output_tokens = usage->elements["output_tokens"].as_num();
    double input_price = llm_price_number(pricing, {"input_per_million", "prompt_per_million"});
    double output_price = llm_price_number(pricing, {"output_per_million", "completion_per_million"});
    double input_cost = input_tokens * input_price / 1000000.0;
    double output_cost = output_tokens * output_price / 1000000.0;

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["input_tokens"] = Value(input_tokens);
    d->elements["output_tokens"] = Value(output_tokens);
    d->elements["input_per_million"] = Value(input_price);
    d->elements["output_per_million"] = Value(output_price);
    d->elements["input_cost"] = Value(input_cost);
    d->elements["output_cost"] = Value(output_cost);
    d->elements["total_cost"] = Value(input_cost + output_cost);
    auto currency_it = pricing->elements.find("currency");
    if (currency_it != pricing->elements.end() && currency_it->second.is_str()) d->elements["currency"] = currency_it->second;
    return out;
}

inline Value b_llm_budget(const Value* a, int n, int l) {
    need_args("llm_budget", n, 3, 3, l);
    double limit = need_num("llm_budget", a[2], 2, l);
    if (limit < 0) throw JitThrow{"llm_budget(): limit must be non-negative", l};

    Value cost_args[2] = {a[0], a[1]};
    Value out = b_llm_cost(cost_args, 2, l);
    auto* d = out.as_dict();
    double total_cost = d->elements["total_cost"].as_num();
    double remaining = limit - total_cost;
    d->elements["limit"] = Value(limit);
    d->elements["remaining"] = Value(remaining);
    d->elements["within_budget"] = Value(total_cost <= limit);
    d->elements["over_budget"] = Value(total_cost > limit);
    return out;
}

inline void llm_copy_tool_arguments(Value& out, const Value& parsed) {
    auto* d = out.as_dict();
    d->elements["arguments"] = parsed;
    if (!parsed.is_dict()) return;
    for (const auto& [key, value] : parsed.as_dict()->elements) {
        if (key == "name" || key == "id" || key == "type" || key == "function" ||
            key == "arguments" || key == "raw_arguments") {
            continue;
        }
        d->elements[key] = value;
    }
}

inline Value llm_normalize_tool_call(const Value& raw, int line) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    if (!raw.is_dict()) return out;
    auto* call = raw.as_dict();

    auto id_it = call->elements.find("id");
    if (id_it != call->elements.end()) d->elements["id"] = Value(id_it->second.to_str());
    auto type_it = call->elements.find("type");
    if (type_it != call->elements.end()) d->elements["type"] = Value(type_it->second.to_str());

    const Value* args_value = nullptr;
    std::string name;
    auto fn_it = call->elements.find("function");
    if (fn_it != call->elements.end() && fn_it->second.is_dict()) {
        auto* fn = fn_it->second.as_dict();
        auto name_it = fn->elements.find("name");
        if (name_it != fn->elements.end()) name = name_it->second.to_str();
        auto args_it = fn->elements.find("arguments");
        if (args_it != fn->elements.end()) args_value = &args_it->second;
    } else {
        auto name_it = call->elements.find("name");
        if (name_it != call->elements.end()) name = name_it->second.to_str();
        auto args_it = call->elements.find("arguments");
        if (args_it != call->elements.end()) args_value = &args_it->second;
    }

    if (args_value) {
        if (args_value->is_str()) {
            std::string raw_args = args_value->as_str();
            d->elements["raw_arguments"] = Value(raw_args);
            try {
                llm_copy_tool_arguments(out, JsonParser(raw_args, line).parse_document());
            } catch (const JitThrow&) {
                d->elements["arguments"] = Value(raw_args);
            }
        } else {
            llm_copy_tool_arguments(out, *args_value);
        }
    }
    if (!name.empty()) d->elements["name"] = Value(name);
    return out;
}

inline Value b_llm_tool_calls(const Value* a, int n, int l) {
    need_args("llm_tool_calls", n, 1, 1, l);
    Value response = a[0];
    if (response.is_str()) response = JsonParser(response.as_str(), l).parse_document();
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    if (!response.is_dict()) return out;
    auto* root = response.as_dict();

    auto top_it = root->elements.find("tool_calls");
    if (top_it != root->elements.end() && top_it->second.is_arr()) {
        for (const auto& call : top_it->second.as_arr()->elements)
            arr->elements.push_back(llm_normalize_tool_call(call, l));
        return out;
    }

    auto choices_it = root->elements.find("choices");
    if (choices_it == root->elements.end() || !choices_it->second.is_arr()) return out;
    auto* choices = choices_it->second.as_arr();
    for (const auto& choice_value : choices->elements) {
        if (!choice_value.is_dict()) continue;
        auto* choice = choice_value.as_dict();
        GCDict* container = choice;
        auto msg_it = choice->elements.find("message");
        if (msg_it != choice->elements.end() && msg_it->second.is_dict()) container = msg_it->second.as_dict();

        auto calls_it = container->elements.find("tool_calls");
        if (calls_it != container->elements.end() && calls_it->second.is_arr()) {
            for (const auto& call : calls_it->second.as_arr()->elements)
                arr->elements.push_back(llm_normalize_tool_call(call, l));
            continue;
        }
        auto fn_it = container->elements.find("function_call");
        if (fn_it != container->elements.end() && fn_it->second.is_dict())
            arr->elements.push_back(llm_normalize_tool_call(fn_it->second, l));
    }
    return out;
}

inline Value b_llm_tool_result(const Value* a, int n, int l) {
    need_args("llm_tool_result", n, 2, 2, l);
    std::string id;
    if (a[0].is_dict()) {
        auto* d = a[0].as_dict();
        auto it = d->elements.find("id");
        if (it == d->elements.end()) it = d->elements.find("tool_call_id");
        if (it != d->elements.end()) id = it->second.to_str();
    } else {
        id = a[0].to_str();
    }
    Value msg = Value::make_dict();
    auto* out = msg.as_dict();
    out->elements["role"] = Value(std::string("tool"));
    out->elements["tool_call_id"] = Value(id);
    out->elements["content"] = a[1].is_str() ? a[1] : Value(json_stringify_value(a[1]));
    return msg;
}

inline std::string llm_stream_chunk_text(const Value& chunk) {
    if (!chunk.is_dict()) return "";
    auto* root = chunk.as_dict();
    auto choices_it = root->elements.find("choices");
    if (choices_it == root->elements.end() || !choices_it->second.is_arr()) return "";
    auto* choices = choices_it->second.as_arr();
    if (choices->elements.empty() || !choices->elements[0].is_dict()) return "";
    auto* choice = choices->elements[0].as_dict();
    auto delta_it = choice->elements.find("delta");
    if (delta_it != choice->elements.end() && delta_it->second.is_dict()) {
        auto* delta = delta_it->second.as_dict();
        auto content_it = delta->elements.find("content");
        if (content_it != delta->elements.end() && !content_it->second.is_nil()) return content_it->second.to_str();
    }
    auto msg_it = choice->elements.find("message");
    if (msg_it != choice->elements.end() && msg_it->second.is_dict()) {
        auto* msg = msg_it->second.as_dict();
        auto content_it = msg->elements.find("content");
        if (content_it != msg->elements.end() && !content_it->second.is_nil()) return content_it->second.to_str();
    }
    auto text_it = choice->elements.find("text");
    if (text_it != choice->elements.end() && !text_it->second.is_nil()) return text_it->second.to_str();
    return "";
}

inline Value b_llm_stream_text(const Value* a, int n, int l) {
    need_args("llm_stream_text", n, 1, 1, l);
    Value chunks = a[0];
    if (chunks.is_str()) {
        Value args[2] = {chunks, Value(true)};
        chunks = b_sse_data(args, 2, l);
    }
    if (!chunks.is_arr()) throw JitThrow{"llm_stream_text(): expected SSE text or array of chunks", l};
    std::string out;
    for (const auto& chunk : chunks.as_arr()->elements) out += llm_stream_chunk_text(chunk);
    return Value(out);
}

inline std::string llm_request_body_from_value(const char* name, const Value& request, int line) {
    if (request.is_str()) return request.as_str();
    if (request.is_dict()) return json_stringify_value(request);
    throw JitThrow{std::string(name) + "(): request must be a dict or JSON string", line};
}

inline Value llm_send_request_body(const char* name, const std::string& endpoint, const std::string& api_key,
                                   const std::string& body, int line) {
    if (endpoint.find_first_of("\"\r\n") != std::string::npos || api_key.find_first_of("\"\r\n") != std::string::npos)
        throw JitThrow{std::string(name) + "(): endpoint and api key must not contain quotes or newlines", line};
    if (endpoint.rfind("file://", 0) == 0) {
        return Value(read_text_file(endpoint.substr(7), line));
    }
    auto tmp = std::filesystem::temp_directory_path() /
        ("sura_llm_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << body;
    }
    std::string cmd = "curl -L -s --max-time 60 -X POST -H \"Content-Type: application/json\" -H \"Authorization: Bearer " +
        api_key + "\" --data-binary @\"" + tmp.string() + "\" -- \"" + endpoint + "\"";
    std::string raw = run_capture_command(cmd);
    std::filesystem::remove(tmp);
    return Value(raw);
}

inline Value b_llm_chat(const Value* a, int n, int l) {
    need_args("llm_chat", n, 4, 5, l);
    std::string endpoint = need_str("llm_chat", a[0], 0, l);
    std::string api_key = need_str("llm_chat", a[1], 1, l);
    Value req_args[3] = {a[2], a[3], n >= 5 ? a[4] : Value(0.2)};
    std::string body = json_stringify_value(b_llm_request(req_args, 3, l));
    return llm_send_request_body("llm_chat", endpoint, api_key, body, l);
}

inline Value b_llm_chat_request(const Value* a, int n, int l) {
    need_args("llm_chat_request", n, 3, 3, l);
    std::string endpoint = need_str("llm_chat_request", a[0], 0, l);
    std::string api_key = need_str("llm_chat_request", a[1], 1, l);
    std::string body = llm_request_body_from_value("llm_chat_request", a[2], l);
    return llm_send_request_body("llm_chat_request", endpoint, api_key, body, l);
}

inline Value b_http_json(const Value* a, int n, int l) {
    Value text = b_http_get(a, n, l);
    Value arg[1] = {text};
    return b_json_parse(arg, 1, l);
}

inline Value b_http_post(const Value* a, int n, int l) {
    need_args("http_post", n, 2, 3, l);
    std::string url = need_str("http_post", a[0], 0, l);
    std::string body = a[1].to_str();
    if (url.find_first_of("\"\r\n") != std::string::npos)
        throw JitThrow{"http_post(): URL contains unsupported characters", l};
    auto tmp = std::filesystem::temp_directory_path() /
        ("sura_post_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << body;
    }
    std::string content_type = n >= 3 ? need_str("http_post", a[2], 2, l) : "application/json";
    std::string cmd = "curl -L -s --max-time 20 -X POST -H \"Content-Type: " + content_type +
        "\" --data-binary @\"" + tmp.string() + "\" -- \"" + url + "\"";
    std::string out = run_capture_command(cmd);
    std::filesystem::remove(tmp);
    return Value(out);
}

inline bool http_header_name_safe(const std::string& name) {
    if (name.empty()) return false;
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (!(std::isalnum(c) || ch == '-' || ch == '_')) return false;
    }
    return true;
}

inline bool http_shell_value_safe(const std::string& value) {
    return value.find_first_of("\"\r\n") == std::string::npos;
}

inline Value make_header_dict(const std::string& name, const std::string& value) {
    Value out = Value::make_dict();
    out.as_dict()->elements[name] = Value(value);
    return out;
}

inline Value b_auth_bearer(const Value* a, int n, int l) {
    need_args("auth_bearer", n, 1, 1, l);
    std::string token = need_str("auth_bearer", a[0], 0, l);
    std::string value = "Bearer " + token;
    if (!http_shell_value_safe(value)) throw JitThrow{"auth_bearer(): token contains unsupported characters", l};
    return make_header_dict("Authorization", value);
}

inline Value b_auth_basic(const Value* a, int n, int l) {
    need_args("auth_basic", n, 2, 2, l);
    std::string username = need_str("auth_basic", a[0], 0, l);
    std::string password = need_str("auth_basic", a[1], 1, l);
    if (username.find_first_of("\r\n") != std::string::npos || password.find_first_of("\r\n") != std::string::npos) {
        throw JitThrow{"auth_basic(): username or password contains unsupported characters", l};
    }
    return make_header_dict("Authorization", "Basic " + base64_encode_text(username + ":" + password));
}

inline Value b_headers_merge(const Value* a, int n, int l) {
    need_args("headers_merge", n, 1, -1, l);
    Value out = Value::make_dict();
    auto* dict = out.as_dict();
    for (int i = 0; i < n; ++i) {
        if (!a[i].is_dict()) throw JitThrow{"headers_merge(): expected header dictionaries", l};
        for (const auto& [name, raw_value] : a[i].as_dict()->elements) {
            if (!raw_value.is_str()) throw JitThrow{"headers_merge(): header values must be strings", l};
            if (!http_header_name_safe(name)) throw JitThrow{"headers_merge(): header name contains unsupported characters", l};
            if (!http_shell_value_safe(raw_value.as_str())) throw JitThrow{"headers_merge(): header value contains unsupported characters", l};
            dict->elements[name] = raw_value;
        }
    }
    return out;
}

inline std::string http_header_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

inline std::string http_header_trim(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline const Value* http_header_find(GCDict* headers, const std::string& name) {
    std::string target = http_header_lower(name);
    for (const auto& [key, value] : headers->elements) {
        if (http_header_lower(key) == target) return &value;
    }
    return nullptr;
}

inline Value b_headers_get(const Value* a, int n, int l) {
    need_args("headers_get", n, 2, 3, l);
    if (!a[0].is_dict()) throw JitThrow{"headers_get(): expected header dictionary", l};
    std::string name = need_str("headers_get", a[1], 1, l);
    if (!http_header_name_safe(name)) throw JitThrow{"headers_get(): header name contains unsupported characters", l};
    const Value* value = http_header_find(a[0].as_dict(), name);
    return value ? *value : (n >= 3 ? a[2] : Value::nil());
}

inline Value b_headers_has(const Value* a, int n, int l) {
    need_args("headers_has", n, 2, 2, l);
    if (!a[0].is_dict()) throw JitThrow{"headers_has(): expected header dictionary", l};
    std::string name = need_str("headers_has", a[1], 1, l);
    if (!http_header_name_safe(name)) throw JitThrow{"headers_has(): header name contains unsupported characters", l};
    return Value(http_header_find(a[0].as_dict(), name) != nullptr);
}

inline bool headers_redact_default_name(const std::string& lower) {
    if (lower == "authorization" || lower == "proxy-authorization" ||
        lower == "cookie" || lower == "set-cookie" ||
        lower == "x-api-key" || lower == "api-key" ||
        lower == "x-auth-token" || lower == "x-csrf-token" || lower == "x-xsrf-token") {
        return true;
    }
    return lower.find("token") != std::string::npos ||
        lower.find("secret") != std::string::npos ||
        lower.find("api-key") != std::string::npos ||
        lower.find("apikey") != std::string::npos;
}

inline bool headers_redact_list_has(const Value& names, const std::string& lower, int line) {
    if (names.is_nil()) return false;
    if (names.is_str()) {
        std::string text = names.as_str();
        size_t start = 0;
        while (start <= text.size()) {
            size_t comma = text.find(',', start);
            bool last = comma == std::string::npos;
            std::string item = http_header_lower(http_header_trim(last ? text.substr(start) : text.substr(start, comma - start)));
            if (!item.empty() && item == lower) return true;
            if (last) break;
            start = comma + 1;
        }
        return false;
    }
    if (names.is_arr()) {
        for (const auto& item : names.as_arr()->elements) {
            if (!item.is_str()) throw JitThrow{"headers_redact(): names array must contain strings", line};
            if (http_header_lower(item.as_str()) == lower) return true;
        }
        return false;
    }
    if (names.is_dict()) {
        for (const auto& [key, value] : names.as_dict()->elements) {
            if (value.truthy() && http_header_lower(key) == lower) return true;
        }
        return false;
    }
    throw JitThrow{"headers_redact(): names must be nil, string, array, or dict", line};
}

inline Value b_headers_redact(const Value* a, int n, int l) {
    need_args("headers_redact", n, 1, 3, l);
    if (!a[0].is_dict()) throw JitThrow{"headers_redact(): expected header dictionary", l};
    const Value* extra_names = n >= 2 ? &a[1] : nullptr;
    std::string mask = n >= 3 ? need_str("headers_redact", a[2], 2, l) : "[REDACTED]";
    if (mask.find_first_of("\r\n") != std::string::npos) {
        throw JitThrow{"headers_redact(): mask contains unsupported characters", l};
    }

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    for (const auto& [name, raw_value] : a[0].as_dict()->elements) {
        if (!http_header_name_safe(name)) throw JitThrow{"headers_redact(): header name contains unsupported characters", l};
        std::string lower = http_header_lower(name);
        bool redact = headers_redact_default_name(lower);
        if (!redact && extra_names) redact = headers_redact_list_has(*extra_names, lower, l);
        d->elements[name] = Value(redact ? mask : raw_value.to_str());
    }
    return out;
}

inline bool cookie_name_safe(const std::string& name) {
    if (name.empty()) return false;
    static const std::string separators = "()<>@,;:\\\"/[]?={} \t";
    for (unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7f) return false;
        if (separators.find((char)c) != std::string::npos) return false;
    }
    return true;
}

inline std::string cookie_unquote_value(const std::string& value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return value;
    std::string out;
    bool escaped = false;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char ch = value[i];
        if (escaped) {
            out.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else {
            out.push_back(ch);
        }
    }
    if (escaped) out.push_back('\\');
    return out;
}

inline void cookie_add_value(GCDict* d, const std::string& key, const std::string& value) {
    auto it = d->elements.find(key);
    if (it == d->elements.end()) {
        d->elements[key] = Value(value);
        return;
    }
    if (!it->second.is_arr()) {
        Value arr = Value::make_array();
        arr.as_arr()->elements.push_back(it->second);
        it->second = arr;
    }
    it->second.as_arr()->elements.push_back(Value(value));
}

inline Value cookie_parse_text(std::string text, int line, const char* name) {
    text = http_header_trim(text);
    std::string lower = http_header_lower(text);
    if (lower.rfind("cookie:", 0) == 0) text = http_header_trim(text.substr(7));

    Value out = Value::make_dict();
    auto* d = out.as_dict();
    size_t start = 0;
    while (start <= text.size()) {
        size_t semi = text.find(';', start);
        bool last = semi == std::string::npos;
        std::string part = http_header_trim(last ? text.substr(start) : text.substr(start, semi - start));
        if (!part.empty()) {
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                std::string key = http_header_trim(part.substr(0, eq));
                if (!key.empty() && key[0] != '$') {
                    if (!cookie_name_safe(key)) throw JitThrow{std::string(name) + "(): cookie name contains unsupported characters", line};
                    std::string raw_value = http_header_trim(part.substr(eq + 1));
                    std::string value = url_percent_decode_text(cookie_unquote_value(raw_value), line, name);
                    cookie_add_value(d, key, value);
                }
            }
        }
        if (last) break;
        start = semi + 1;
    }
    return out;
}

inline std::string cookie_value_string(const Value& value, int line, const char* name) {
    if (value.is_arr() || value.is_dict()) throw JitThrow{std::string(name) + "(): nested arrays or dicts are not supported as cookie values", line};
    if (value.is_nil()) return "";
    return value.to_str();
}

inline void cookie_append_pair(std::string& out, const std::string& key, const Value& value, int line, const char* name) {
    if (!cookie_name_safe(key)) throw JitThrow{std::string(name) + "(): cookie name contains unsupported characters", line};
    if (!out.empty()) out += "; ";
    out += key;
    out.push_back('=');
    out += url_encode_text(cookie_value_string(value, line, name));
}

inline std::string cookie_build_from_dict(GCDict* d, int line, const char* name) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : d->elements) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (const auto& key : keys) {
        const Value& value = d->elements.at(key);
        if (value.is_arr()) {
            for (const auto& item : value.as_arr()->elements) cookie_append_pair(out, key, item, line, name);
        } else {
            cookie_append_pair(out, key, value, line, name);
        }
    }
    return out;
}

inline Value b_cookie_parse(const Value* a, int n, int l) {
    need_args("cookie_parse", n, 1, 1, l);
    if (a[0].is_str()) return cookie_parse_text(a[0].as_str(), l, "cookie_parse");
    if (a[0].is_dict()) {
        const Value* header = http_header_find(a[0].as_dict(), "cookie");
        if (!header) return Value::make_dict();
        if (!header->is_str()) throw JitThrow{"cookie_parse(): Cookie header must be a string", l};
        return cookie_parse_text(header->as_str(), l, "cookie_parse");
    }
    throw JitThrow{"cookie_parse(): expected cookie header text or header dictionary", l};
}

inline Value b_cookie_build(const Value* a, int n, int l) {
    need_args("cookie_build", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"cookie_build(): expected cookie dictionary", l};
    return Value(cookie_build_from_dict(a[0].as_dict(), l, "cookie_build"));
}

inline Value b_cookie_get(const Value* a, int n, int l) {
    need_args("cookie_get", n, 2, 3, l);
    std::string name = need_str("cookie_get", a[1], 1, l);
    if (!cookie_name_safe(name)) throw JitThrow{"cookie_get(): cookie name contains unsupported characters", l};
    Value parsed;
    GCDict* cookies = nullptr;
    if (a[0].is_str()) {
        parsed = cookie_parse_text(a[0].as_str(), l, "cookie_get");
        cookies = parsed.as_dict();
    } else if (a[0].is_dict()) {
        const Value* header = http_header_find(a[0].as_dict(), "cookie");
        if (header) {
            if (!header->is_str()) throw JitThrow{"cookie_get(): Cookie header must be a string", l};
            parsed = cookie_parse_text(header->as_str(), l, "cookie_get");
            cookies = parsed.as_dict();
        } else {
            cookies = a[0].as_dict();
        }
    } else {
        throw JitThrow{"cookie_get(): expected cookie header text, header dictionary, or cookie dictionary", l};
    }
    auto it = cookies->elements.find(name);
    if (it != cookies->elements.end()) return it->second;
    return n >= 3 ? a[2] : Value::nil();
}

inline std::string http_content_type_text(const Value& v, int line, const char* fn) {
    if (v.is_str()) return v.as_str();
    if (v.is_dict()) {
        const Value* value = http_header_find(v.as_dict(), "content-type");
        if (!value || !value->is_str()) return "";
        return value->as_str();
    }
    throw JitThrow{std::string(fn) + "(): expected header dict or content-type text", line};
}

inline std::string http_media_type_from_text(std::string text) {
    size_t semi = text.find(';');
    if (semi != std::string::npos) text = text.substr(0, semi);
    return http_header_lower(http_header_trim(text));
}

inline std::string http_charset_from_text(const std::string& text) {
    size_t pos = text.find(';');
    while (pos != std::string::npos) {
        size_t next = text.find(';', pos + 1);
        std::string part = http_header_trim(text.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1));
        size_t eq = part.find('=');
        if (eq != std::string::npos && http_header_lower(http_header_trim(part.substr(0, eq))) == "charset") {
            std::string value = http_header_trim(part.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            return http_header_lower(value);
        }
        pos = next;
    }
    return "";
}

inline Value b_http_content_type(const Value* a, int n, int l) {
    need_args("http_content_type", n, 1, 2, l);
    std::string media = http_media_type_from_text(http_content_type_text(a[0], l, "http_content_type"));
    if (media.empty()) return n >= 2 ? Value(need_str("http_content_type", a[1], 1, l)) : Value("");
    return Value(media);
}

inline Value b_http_charset(const Value* a, int n, int l) {
    need_args("http_charset", n, 1, 2, l);
    std::string charset = http_charset_from_text(http_content_type_text(a[0], l, "http_charset"));
    if (charset.empty()) return n >= 2 ? Value(need_str("http_charset", a[1], 1, l)) : Value("");
    return Value(charset);
}

inline Value b_http_is_json(const Value* a, int n, int l) {
    need_args("http_is_json", n, 1, 1, l);
    std::string media = http_media_type_from_text(http_content_type_text(a[0], l, "http_is_json"));
    bool is_json = media == "application/json";
    if (!is_json && media.size() > 5) is_json = media.rfind("+json") == media.size() - 5;
    return Value(is_json);
}

inline int need_http_status_code(const char* name, const Value& v, int idx, int line) {
    double raw = need_num(name, v, idx, line);
    if (raw < 0 || raw > 999 || std::floor(raw) != raw) {
        throw JitThrow{std::string(name) + "(): status must be an integer from 0 to 999", line};
    }
    return (int)raw;
}

inline std::string http_status_reason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 102: return "Processing";
        case 103: return "Early Hints";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 208: return "Already Reported";
        case 226: return "IM Used";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 506: return "Variant Also Negotiates";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 510: return "Not Extended";
        case 511: return "Network Authentication Required";
        default: return "";
    }
}

inline Value b_http_status_ok(const Value* a, int n, int l) {
    need_args("http_status_ok", n, 1, 1, l);
    int status = need_http_status_code("http_status_ok", a[0], 0, l);
    return Value(status >= 200 && status < 300);
}

inline Value b_http_status_text(const Value* a, int n, int l) {
    need_args("http_status_text", n, 1, 1, l);
    return Value(http_status_reason(need_http_status_code("http_status_text", a[0], 0, l)));
}

inline Value b_http_status_retryable(const Value* a, int n, int l) {
    need_args("http_status_retryable", n, 1, 1, l);
    int status = need_http_status_code("http_status_retryable", a[0], 0, l);
    bool retryable = status == 408 || status == 409 || status == 425 || status == 429 ||
                     status == 500 || status == 502 || status == 503 || status == 504;
    return Value(retryable);
}

inline std::string http_retry_trim(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline std::string http_retry_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

inline long long http_retry_after_ms_from_text(std::string text, long long fallback_ms) {
    text = http_retry_trim(text);
    if (text.empty()) return fallback_ms;
    long long seconds = 0;
    for (unsigned char ch : text) {
        if (!std::isdigit(ch)) return fallback_ms;
        seconds = seconds * 10 + (ch - '0');
        if (seconds > 2147483) return fallback_ms;
    }
    return seconds * 1000;
}

inline Value b_http_retry_after(const Value* a, int n, int l) {
    need_args("http_retry_after", n, 1, 2, l);
    long long fallback_ms = n >= 2 ? need_nonnegative_int("http_retry_after", a[1], 1, l) : 0;
    if (a[0].is_str()) return Value((double)http_retry_after_ms_from_text(a[0].as_str(), fallback_ms));
    if (a[0].is_num()) {
        double seconds = a[0].as_num();
        if (seconds < 0 || std::floor(seconds) != seconds || seconds > 2147483) return Value((double)fallback_ms);
        return Value(seconds * 1000.0);
    }
    if (!a[0].is_dict()) throw JitThrow{"http_retry_after(): expected header dict or header value text", l};
    for (const auto& [key, value] : a[0].as_dict()->elements) {
        if (http_retry_lower(key) != "retry-after") continue;
        if (value.is_str()) return Value((double)http_retry_after_ms_from_text(value.as_str(), fallback_ms));
        if (value.is_num()) {
            double seconds = value.as_num();
            if (seconds < 0 || std::floor(seconds) != seconds || seconds > 2147483) return Value((double)fallback_ms);
            return Value(seconds * 1000.0);
        }
        return Value((double)fallback_ms);
    }
    return Value((double)fallback_ms);
}

inline Value b_http_backoff_delays(const Value* a, int n, int l) {
    need_args("http_backoff_delays", n, 1, 4, l);
    int attempts = need_positive_int("http_backoff_delays", a[0], 0, l);
    if (attempts > 50) throw JitThrow{"http_backoff_delays(): attempts must be 1..50", l};
    int base_ms = n >= 2 ? need_nonnegative_int("http_backoff_delays", a[1], 1, l) : 250;
    double factor = n >= 3 ? need_num("http_backoff_delays", a[2], 2, l) : 2.0;
    int max_ms = n >= 4 ? need_nonnegative_int("http_backoff_delays", a[3], 3, l) : 60000;
    if (factor < 1.0 || !std::isfinite(factor)) throw JitThrow{"http_backoff_delays(): factor must be >= 1", l};
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    double delay = (double)base_ms;
    for (int i = 0; i < attempts; ++i) {
        double capped = std::min(delay, (double)max_ms);
        arr->elements.push_back(Value((double)(long long)std::llround(capped)));
        if (delay < (double)max_ms) delay = std::min(delay * factor, (double)max_ms);
    }
    return out;
}

inline std::string http_upper_method(std::string method) {
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return method;
}

inline bool http_method_safe(const std::string& method) {
    if (method.empty()) return false;
    for (char ch : method) {
        if (!std::isalpha((unsigned char)ch)) return false;
    }
    return true;
}

inline bool http_header_is_content_type(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return lower == "content-type";
}

inline std::string http_trim_header_part(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline std::string http_lower_header_name(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

inline int http_status_from_headers(const std::string& raw_headers) {
    int status = 0;
    std::istringstream in(raw_headers);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("HTTP/", 0) != 0) continue;
        size_t first_space = line.find(' ');
        if (first_space == std::string::npos) continue;
        size_t second_space = line.find(' ', first_space + 1);
        std::string code = line.substr(first_space + 1, second_space == std::string::npos ? std::string::npos : second_space - first_space - 1);
        try { status = std::stoi(code); } catch (...) {}
    }
    return status;
}

inline Value http_headers_dict_from_text(const std::string& raw_headers) {
    Value headers = Value::make_dict();
    auto* d = headers.as_dict();
    std::istringstream in(raw_headers);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {
            headers = Value::make_dict();
            d = headers.as_dict();
            continue;
        }
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = http_lower_header_name(http_trim_header_part(line.substr(0, colon)));
        std::string value = http_trim_header_part(line.substr(colon + 1));
        if (!name.empty()) d->elements[name] = Value(value);
    }
    return headers;
}

inline Value http_response_dict(int status, const std::string& body, const std::string& raw_headers, const std::string& url) {
    Value response = Value::make_dict();
    auto* d = response.as_dict();
    d->elements["status"] = Value(status);
    d->elements["ok"] = Value(status >= 200 && status < 300);
    d->elements["body"] = Value(body);
    d->elements["headers"] = http_headers_dict_from_text(raw_headers);
    d->elements["url"] = Value(url);
    return response;
}

inline int http_body_source_count(const Value* body_value, const Value* json_body, const Value* form_body) {
    return (body_value ? 1 : 0) + (json_body ? 1 : 0) + (form_body ? 1 : 0);
}

inline void http_require_single_body_source(const char* fn, const Value* body_value, const Value* json_body, const Value* form_body, int line) {
    if (http_body_source_count(body_value, json_body, form_body) > 1) {
        throw JitThrow{std::string(fn) + "(): accepts only one of body, json, or form", line};
    }
}

inline Value http_request_common(const Value* a, int n, int l, bool full_response) {
    need_args("http_request", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"http_request(): spec must be a dict", l};
    auto* spec = a[0].as_dict();

    const Value* url_value = dict_get_ptr(spec, "url");
    if (!url_value || !url_value->is_str()) throw JitThrow{"http_request(): spec.url must be a string", l};
    std::string url = url_value->as_str();
    if (url.find_first_of("\"\r\n") != std::string::npos) {
        throw JitThrow{"http_request(): URL contains unsupported characters", l};
    }
    url = url_with_query_from_spec(url, spec, "http_request", l);

    const Value* json_body = dict_get_ptr(spec, "json");
    const Value* body_value = dict_get_ptr(spec, "body");
    const Value* form_body = dict_get_ptr(spec, "form");
    http_require_single_body_source("http_request", body_value, json_body, form_body, l);
    bool has_body = json_body != nullptr || body_value != nullptr || form_body != nullptr;
    std::string method = has_body ? "POST" : "GET";
    if (const Value* method_value = dict_get_ptr(spec, "method")) method = need_str("http_request", *method_value, 0, l);
    method = http_upper_method(method);
    if (!http_method_safe(method)) throw JitThrow{"http_request(): method must contain letters only", l};

    if (url.rfind("file://", 0) == 0) {
        if (method != "GET" || has_body) {
            throw JitThrow{"http_request(): file:// only supports body-less GET", l};
        }
        std::string body = read_text_file(url.substr(7), l);
        return full_response ? http_response_dict(200, body, "", url) : Value(body);
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        throw JitThrow{"http_request(): URL must start with http://, https://, or file://", l};
    }

    int timeout = 20;
    if (const Value* timeout_value = dict_get_ptr(spec, "timeout")) {
        if (!timeout_value->is_num()) throw JitThrow{"http_request(): timeout must be a number", l};
        timeout = (int)timeout_value->as_num();
        if (timeout <= 0 || timeout > 3600) throw JitThrow{"http_request(): timeout must be 1..3600 seconds", l};
    }

    std::vector<std::pair<std::string, std::string>> headers;
    bool has_content_type = false;
    if (const Value* headers_value = dict_get_ptr(spec, "headers")) {
        if (!headers_value->is_dict()) throw JitThrow{"http_request(): headers must be a dict", l};
        for (const auto& [name, raw_value] : headers_value->as_dict()->elements) {
            if (!http_header_name_safe(name)) throw JitThrow{"http_request(): header name contains unsupported characters", l};
            std::string value = raw_value.to_str();
            if (!http_shell_value_safe(value)) throw JitThrow{"http_request(): header value contains unsupported characters", l};
            if (http_header_is_content_type(name)) has_content_type = true;
            headers.push_back({name, value});
        }
    }

    std::string body;
    if (json_body) {
        body = json_stringify_value(*json_body);
        if (!has_content_type) {
            headers.push_back({"Content-Type", "application/json"});
            has_content_type = true;
        }
    } else if (form_body) {
        if (!form_body->is_dict()) throw JitThrow{"http_request(): form must be a dict", l};
        body = form_build_from_dict(form_body->as_dict(), l, "http_request");
        if (!has_content_type) {
            std::string content_type = "application/x-www-form-urlencoded";
            if (const Value* ct = dict_get_ptr(spec, "content_type")) content_type = need_str("http_request", *ct, 0, l);
            if (!http_shell_value_safe(content_type)) throw JitThrow{"http_request(): content_type contains unsupported characters", l};
            headers.push_back({"Content-Type", content_type});
            has_content_type = true;
        }
    } else if (body_value) {
        body = body_value->is_str() ? body_value->as_str() : json_stringify_value(*body_value);
        if (!has_content_type) {
            std::string content_type = "application/json";
            if (const Value* ct = dict_get_ptr(spec, "content_type")) content_type = need_str("http_request", *ct, 0, l);
            if (!http_shell_value_safe(content_type)) throw JitThrow{"http_request(): content_type contains unsupported characters", l};
            headers.push_back({"Content-Type", content_type});
            has_content_type = true;
        }
    }

    std::filesystem::path tmp;
    if (has_body) {
        tmp = std::filesystem::temp_directory_path() /
            ("sura_http_request_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        std::ofstream out(tmp, std::ios::binary);
        out << body;
    }

    auto append_request_options = [&](std::string& cmd) {
        for (const auto& [name, value] : headers) {
            cmd += " -H \"" + name + ": " + value + "\"";
        }
        if (has_body) cmd += " --data-binary @\"" + tmp.string() + "\"";
        cmd += " -- \"" + url + "\"";
    };

    if (!full_response) {
        std::string cmd = "curl -L -s --max-time " + std::to_string(timeout) + " -X " + method;
        append_request_options(cmd);
        std::string out = run_capture_command(cmd);
        if (has_body) std::filesystem::remove(tmp);
        return Value(out);
    }

    auto header_tmp = std::filesystem::temp_directory_path() /
        ("sura_http_headers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    auto body_tmp = std::filesystem::temp_directory_path() /
        ("sura_http_body_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::string cmd = "curl -L -s -D \"" + header_tmp.string() + "\" -o \"" + body_tmp.string() +
        "\" --max-time " + std::to_string(timeout) + " -X " + method;
    append_request_options(cmd);
    run_capture_command(cmd);
    std::string response_body = std::filesystem::exists(body_tmp) ? read_text_file(body_tmp.string(), l) : "";
    std::string response_headers = std::filesystem::exists(header_tmp) ? read_text_file(header_tmp.string(), l) : "";
    if (has_body) std::filesystem::remove(tmp);
    std::filesystem::remove(header_tmp);
    std::filesystem::remove(body_tmp);
    return http_response_dict(http_status_from_headers(response_headers), response_body, response_headers, url);
}

inline Value b_http_request(const Value* a, int n, int l) {
    return http_request_common(a, n, l, false);
}

inline Value b_http_request_full(const Value* a, int n, int l) {
    return http_request_common(a, n, l, true);
}

inline Value b_http_request_retry(const Value* a, int n, int l) {
    need_args("http_request_retry", n, 1, 3, l);
    int attempts = n >= 2 ? (int)need_num("http_request_retry", a[1], 1, l) : 3;
    int delay_ms = n >= 3 ? (int)need_num("http_request_retry", a[2], 2, l) : 250;
    if (attempts < 1 || attempts > 20) throw JitThrow{"http_request_retry(): attempts must be 1..20", l};
    if (delay_ms < 0 || delay_ms > 60000) throw JitThrow{"http_request_retry(): delay_ms must be 0..60000", l};
    Value request_arg[1] = {a[0]};
    Value last = Value::nil();
    for (int i = 1; i <= attempts; ++i) {
        last = b_http_request_full(request_arg, 1, l);
        if (last.is_dict()) {
            auto* d = last.as_dict();
            d->elements["attempts"] = Value(i);
            auto ok_it = d->elements.find("ok");
            if (ok_it != d->elements.end() && ok_it->second.truthy()) return last;
        }
        if (i < attempts && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    return last;
}

inline Value b_http_request_retry_json(const Value* a, int n, int l) {
    Value response = b_http_request_retry(a, n, l);
    if (!response.is_dict()) throw JitThrow{"http_request_retry_json(): expected retry response dict", l};
    auto* d = response.as_dict();
    auto body_it = d->elements.find("body");
    if (body_it == d->elements.end() || !body_it->second.is_str()) {
        throw JitThrow{"http_request_retry_json(): response body is not text", l};
    }
    Value arg[1] = {body_it->second};
    return b_json_parse(arg, 1, l);
}

inline Value http_parse_checked_response_json(const char* fn, const Value& response, int l) {
    if (!response.is_dict()) throw JitThrow{std::string(fn) + "(): expected response dict", l};
    auto* d = response.as_dict();
    int status = 0;
    auto status_it = d->elements.find("status");
    if (status_it != d->elements.end() && status_it->second.is_num()) {
        status = (int)status_it->second.as_num();
    }
    auto ok_it = d->elements.find("ok");
    if (ok_it == d->elements.end() || !ok_it->second.truthy()) {
        throw JitThrow{std::string(fn) + "(): HTTP status " + std::to_string(status), l};
    }
    auto body_it = d->elements.find("body");
    if (body_it == d->elements.end() || !body_it->second.is_str()) {
        throw JitThrow{std::string(fn) + "(): response body is not text", l};
    }
    Value arg[1] = {body_it->second};
    return b_json_parse(arg, 1, l);
}

inline Value b_http_request_retry_json_checked(const Value* a, int n, int l) {
    return http_parse_checked_response_json("http_request_retry_json_checked", b_http_request_retry(a, n, l), l);
}

inline Value b_http_request_json(const Value* a, int n, int l) {
    Value text = b_http_request(a, n, l);
    Value arg[1] = {text};
    return b_json_parse(arg, 1, l);
}

inline Value b_http_request_json_checked(const Value* a, int n, int l) {
    return http_parse_checked_response_json("http_request_json_checked", b_http_request_full(a, n, l), l);
}

inline bool tool_array_contains(const Value& list, const std::string& needle) {
    if (!list.is_arr()) return false;
    for (const auto& item : list.as_arr()->elements) {
        if (item.is_str() && item.as_str() == needle) return true;
    }
    return false;
}

inline bool tool_starts_with_any(const std::string& text, const Value& prefixes) {
    if (!prefixes.is_arr()) return false;
    for (const auto& item : prefixes.as_arr()->elements) {
        if (!item.is_str()) continue;
        const std::string prefix = item.as_str();
        if (text.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

inline std::string tool_spec_name(GCDict* d) {
    auto it = d->elements.find("name");
    return it == d->elements.end() ? "" : it->second.to_str();
}

inline bool tool_known_name(const std::string& name) {
    return name == "http_get" || name == "http_request" || name == "shell";
}

inline bool tool_is_http_name(const std::string& name) {
    return name == "http_get" || name == "http_request";
}

inline std::string tool_http_request_method(GCDict* d) {
    if (const Value* method = dict_get_ptr(d, "method")) {
        if (method->is_str()) return http_upper_method(method->as_str());
    }
    return (dict_get_ptr(d, "body") || dict_get_ptr(d, "json") || dict_get_ptr(d, "form")) ? "POST" : "GET";
}

inline bool tool_array_contains_upper(const Value& list, const std::string& needle) {
    if (!list.is_arr()) return false;
    std::string want = http_upper_method(needle);
    for (const auto& item : list.as_arr()->elements) {
        if (!item.is_str()) continue;
        if (http_upper_method(item.as_str()) == want) return true;
    }
    return false;
}

inline bool tool_array_contains_header(const Value& list, const std::string& needle) {
    if (!list.is_arr()) return false;
    std::string want = http_lower_header_name(needle);
    for (const auto& item : list.as_arr()->elements) {
        if (!item.is_str()) continue;
        if (http_lower_header_name(item.as_str()) == want) return true;
    }
    return false;
}

inline const Value* tool_header_value(GCDict* headers, const std::string& name) {
    std::string want = http_lower_header_name(name);
    for (const auto& [header_name, value] : headers->elements) {
        if (http_lower_header_name(header_name) == want) return &value;
    }
    return nullptr;
}

inline std::string tool_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

inline bool tool_env_truthy(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return false;
    std::string value = tool_lower_ascii(raw);
    return value != "0" && value != "false" && value != "no" &&
           value != "off" && value != "deny" && value != "denied";
}

inline bool tool_env_allow_value(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return false;
    std::string value = tool_lower_ascii(raw);
    return value == "1" || value == "true" || value == "yes" ||
           value == "y" || value == "allow" || value == "approve" ||
           value == "approved";
}

inline void tool_set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

inline void tool_unset_env_var(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

struct ScopedToolEnvVar {
    std::string name;
    bool had_old = false;
    std::string old_value;

    ScopedToolEnvVar(std::string n, std::string v) : name(std::move(n)) {
        const char* old = std::getenv(name.c_str());
        if (old) {
            had_old = true;
            old_value = old;
        }
        tool_set_env_var(name, v);
    }

    ~ScopedToolEnvVar() {
        if (had_old) tool_set_env_var(name, old_value);
        else tool_unset_env_var(name);
    }
};

inline std::string tool_trim_ascii(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline bool tool_policy_approval_required_value(const Value& value, bool* required, std::string* reason) {
    auto fail = [&](const std::string& msg) {
        if (reason) *reason = msg;
        return false;
    };
    if (value.is_bool()) {
        *required = value.as_bool();
        return true;
    }
    if (value.is_num()) {
        *required = value.as_num() != 0.0;
        return true;
    }
    if (value.is_str()) {
        std::string mode = tool_lower_ascii(value.as_str());
        if (mode == "true" || mode == "yes" || mode == "on" ||
            mode == "required" || mode == "always" || mode == "manual" ||
            mode == "interactive" || mode == "token") {
            *required = true;
            return true;
        }
        if (mode == "false" || mode == "no" || mode == "off" ||
            mode == "none" || mode == "never" || mode == "optional") {
            *required = false;
            return true;
        }
        return fail("policy.approval has unsupported mode '" + value.as_str() + "'");
    }
    return fail("policy.approval must be a bool, number, or string");
}

inline bool tool_policy_requires_approval(GCDict* policy, bool* required, std::string* reason) {
    *required = false;
    const Value* approval = dict_get_ptr(policy, "approval");
    if (!approval) return true;
    return tool_policy_approval_required_value(*approval, required, reason);
}

inline std::string tool_redact_newlines(std::string text) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() > 160) text = text.substr(0, 157) + "...";
    return text;
}

inline std::string tool_approval_summary(GCDict* spec) {
    std::string name = tool_spec_name(spec);
    std::string detail;
    if (const Value* url = dict_get_ptr(spec, "url")) {
        if (url->is_str()) detail = " url=" + tool_redact_newlines(url->as_str());
    } else if (const Value* command = dict_get_ptr(spec, "command")) {
        if (command->is_str()) detail = " command=" + tool_redact_newlines(command->as_str());
    }
    return name + detail;
}

inline std::string tool_approval_target(GCDict* spec) {
    if (const Value* url = dict_get_ptr(spec, "url")) {
        if (url->is_str()) return tool_redact_newlines(url->as_str());
    }
    if (const Value* command = dict_get_ptr(spec, "command")) {
        if (command->is_str()) return tool_redact_newlines(command->as_str());
    }
    return "";
}

inline bool tool_policy_token_safe(const std::string& token) {
    return !token.empty() && token.size() <= 512 && token.find_first_of("\r\n") == std::string::npos;
}

inline bool tool_approval_command_granted(GCDict* spec, GCDict* policy, bool* attempted, std::string* reason) {
    if (attempted) *attempted = false;
    const char* raw_command = std::getenv("SURA_TOOL_APPROVAL_COMMAND");
    if (!raw_command || !*raw_command) return false;
    if (attempted) *attempted = true;

    std::string command = raw_command;
    if (command.size() > 4096 || command.find_first_of("\r\n") != std::string::npos) {
        if (reason) *reason = "SURA_TOOL_APPROVAL_COMMAND must be 1..4096 bytes and contain no newlines";
        return false;
    }

    std::string message;
    if (const Value* message_value = dict_get_ptr(policy, "approval_message")) {
        if (message_value->is_str()) message = tool_redact_newlines(message_value->as_str());
    }
    std::string target;
    target = tool_approval_target(spec);
    bool token_configured = policy && dict_get_ptr(policy, "approval_token");

    ScopedToolEnvVar tool_env("SURA_TOOL_APPROVAL_TOOL", tool_spec_name(spec));
    ScopedToolEnvVar target_env("SURA_TOOL_APPROVAL_TARGET", target);
    ScopedToolEnvVar message_env("SURA_TOOL_APPROVAL_MESSAGE", message);
    ScopedToolEnvVar token_env("SURA_TOOL_APPROVAL_TOKEN_CONFIGURED", token_configured ? "1" : "0");

#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        if (reason) *reason = "approval command could not be started";
        return false;
    }

    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
        if (output.size() > 4096) {
            output.resize(4096);
            break;
        }
    }
#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif

    std::string verdict = tool_lower_ascii(tool_trim_ascii(output));
    if (rc == 0 && (verdict == "1" || verdict == "true" || verdict == "yes" ||
                    verdict == "y" || verdict == "allow" || verdict == "approve" ||
                    verdict == "approved")) {
        return true;
    }

    if (reason) {
        if (rc != 0) {
            *reason = "approval command denied tool '" + tool_spec_name(spec) +
                      "' with exit code " + std::to_string(rc);
        } else if (!verdict.empty()) {
            *reason = "approval command denied tool '" + tool_spec_name(spec) +
                      "' with response '" + tool_redact_newlines(verdict) + "'";
        } else {
            *reason = "approval command denied tool '" + tool_spec_name(spec) +
                      "' with empty response";
        }
    }
    return false;
}

inline std::string tool_approval_request_id() {
    static std::mutex request_mutex;
    static uint64_t request_counter = 0;
    uint64_t counter = 0;
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        counter = ++request_counter;
    }
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return "sura-tool-" + std::to_string((long long)ns) + "-" + std::to_string(counter);
}

inline int tool_approval_file_timeout_ms() {
    const char* raw = std::getenv("SURA_TOOL_APPROVAL_FILE_TIMEOUT_MS");
    if (!raw || !*raw) return 30000;
    try {
        int value = std::stoi(raw);
        if (value < 0) return 0;
        if (value > 300000) return 300000;
        return value;
    } catch (...) {
        return 30000;
    }
}

inline bool tool_approval_path_safe(const std::string& path) {
    return !path.empty() && path.size() <= 4096 && path.find_first_of("\r\n") == std::string::npos;
}

inline bool tool_approval_write_request_file(const std::string& path, GCDict* spec, GCDict* policy,
                                             const std::string& request_id, std::string* reason) {
    std::error_code ec;
    std::filesystem::path request_path = fs_path_from_utf8(path);
    if (request_path.has_parent_path()) std::filesystem::create_directories(request_path.parent_path(), ec);
    std::ofstream out(request_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (reason) *reason = "approval request file could not be written";
        return false;
    }

    std::string message;
    if (const Value* message_value = dict_get_ptr(policy, "approval_message")) {
        if (message_value->is_str()) message = tool_redact_newlines(message_value->as_str());
    }
    bool token_configured = policy && dict_get_ptr(policy, "approval_token");

    out << "{\"version\":1"
        << ",\"requestId\":\"" << json_escape(request_id) << "\""
        << ",\"tool\":\"" << json_escape(tool_spec_name(spec)) << "\""
        << ",\"target\":\"" << json_escape(tool_approval_target(spec)) << "\""
        << ",\"message\":\"" << json_escape(message) << "\""
        << ",\"approvalTokenConfigured\":" << (token_configured ? "true" : "false")
        << ",\"summary\":\"" << json_escape(tool_approval_summary(spec)) << "\""
        << "}\n";
    if (!out) {
        if (reason) *reason = "approval request file could not be written";
        return false;
    }
    return true;
}

inline bool tool_approval_response_allows(const std::string& text, const std::string& request_id,
                                          bool* complete, std::string* reason) {
    *complete = false;
    std::string trimmed = tool_trim_ascii(text);
    if (trimmed.empty()) return false;
    try {
        Value parsed = JsonParser(trimmed, 0).parse_document();
        if (!parsed.is_dict()) {
            *complete = true;
            if (reason) *reason = "approval response file must contain a JSON object";
            return false;
        }
        auto* d = parsed.as_dict();
        const Value* id = dict_get_ptr(d, "requestId");
        if (!id || !id->is_str() || id->as_str() != request_id) return false;
        *complete = true;
        const Value* allow = dict_get_ptr(d, "allow");
        if (allow && allow->is_bool()) {
            if (allow->as_bool()) return true;
            if (reason) *reason = "approval file denied tool request";
            return false;
        }
        const Value* decision = dict_get_ptr(d, "decision");
        if (decision && decision->is_str()) {
            std::string verdict = tool_lower_ascii(tool_trim_ascii(decision->as_str()));
            if (verdict == "1" || verdict == "true" || verdict == "yes" ||
                verdict == "y" || verdict == "allow" || verdict == "approve" ||
                verdict == "approved") {
                return true;
            }
            if (reason) {
                *reason = "approval file denied tool request with decision '" +
                          tool_redact_newlines(verdict) + "'";
            }
            return false;
        }
        if (reason) *reason = "approval response file must contain bool 'allow' or string 'decision'";
        return false;
    } catch (...) {
        return false;
    }
}

inline bool tool_approval_file_granted(GCDict* spec, GCDict* policy, bool* attempted, std::string* reason) {
    if (attempted) *attempted = false;
    const char* raw_request = std::getenv("SURA_TOOL_APPROVAL_REQUEST_FILE");
    const char* raw_response = std::getenv("SURA_TOOL_APPROVAL_RESPONSE_FILE");
    if (!raw_request || !*raw_request || !raw_response || !*raw_response) return false;
    if (attempted) *attempted = true;

    std::string request_path = raw_request;
    std::string response_path = raw_response;
    if (!tool_approval_path_safe(request_path) || !tool_approval_path_safe(response_path)) {
        if (reason) *reason = "approval request/response file paths must be 1..4096 bytes and contain no newlines";
        return false;
    }

    std::string request_id = tool_approval_request_id();
    if (!tool_approval_write_request_file(request_path, spec, policy, request_id, reason)) return false;

    int timeout_ms = tool_approval_file_timeout_ms();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::filesystem::path response_fs_path = fs_path_from_utf8(response_path);
    while (true) {
        std::ifstream in(response_fs_path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            bool complete = false;
            std::string response_reason;
            if (tool_approval_response_allows(ss.str(), request_id, &complete, &response_reason)) return true;
            if (complete) {
                if (reason) *reason = response_reason.empty() ? "approval file denied tool request" : response_reason;
                return false;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (reason) *reason = "approval file timed out waiting for response";
    return false;
}

inline bool tool_policy_approval_granted(GCDict* spec, GCDict* policy, std::string* reason) {
    bool required = false;
    if (!tool_policy_requires_approval(policy, &required, reason)) return false;
    if (!required) return true;

    const Value* token_value = dict_get_ptr(policy, "approval_token");
    if (token_value && !token_value->is_str()) {
        if (reason) *reason = "policy.approval_token must be a string";
        return false;
    }
    std::string required_token = token_value ? token_value->as_str() : "";
    if (!required_token.empty() && !tool_policy_token_safe(required_token)) {
        if (reason) *reason = "policy.approval_token must be 1..512 bytes and contain no newlines";
        return false;
    }
    const Value* message_value = dict_get_ptr(policy, "approval_message");
    if (message_value && !message_value->is_str()) {
        if (reason) *reason = "policy.approval_message must be a string";
        return false;
    }

    if (tool_env_truthy("SURA_TOOL_AUTO_APPROVE")) return true;

    const char* env_token = std::getenv("SURA_TOOL_APPROVAL_TOKEN");
    if (!required_token.empty() && env_token && required_token == std::string(env_token)) return true;

    if (required_token.empty() && tool_env_allow_value("SURA_TOOL_APPROVAL")) return true;

    bool file_attempted = false;
    std::string file_reason;
    if (tool_approval_file_granted(spec, policy, &file_attempted, &file_reason)) return true;

    bool command_attempted = false;
    std::string command_reason;
    if (tool_approval_command_granted(spec, policy, &command_attempted, &command_reason)) return true;

    if (tool_env_truthy("SURA_TOOL_INTERACTIVE_APPROVAL")) {
        std::cerr << "[Sura tool approval] Allow " << tool_approval_summary(spec);
        if (message_value && message_value->is_str() && !message_value->as_str().empty()) {
            std::cerr << " (" << tool_redact_newlines(message_value->as_str()) << ")";
        }
        std::cerr << "? type yes: ";
        std::string answer;
        if (std::getline(std::cin, answer)) {
            answer = tool_lower_ascii(answer);
            if (answer == "yes" || answer == "y" || answer == "allow" || answer == "approve") return true;
        }
    }

    if (reason) {
        if (command_attempted && !command_reason.empty()) {
            *reason = command_reason;
        } else if (file_attempted && !file_reason.empty()) {
            *reason = file_reason;
        } else if (!required_token.empty()) {
            *reason = "approval required for tool '" + tool_spec_name(spec) +
                      "': set SURA_TOOL_APPROVAL_TOKEN to the policy token, SURA_TOOL_APPROVAL_REQUEST_FILE/SURA_TOOL_APPROVAL_RESPONSE_FILE, SURA_TOOL_APPROVAL_COMMAND, or SURA_TOOL_AUTO_APPROVE=1";
        } else {
            *reason = "approval required for tool '" + tool_spec_name(spec) +
                      "': set SURA_TOOL_APPROVAL=allow, SURA_TOOL_APPROVAL_REQUEST_FILE/SURA_TOOL_APPROVAL_RESPONSE_FILE, SURA_TOOL_APPROVAL_COMMAND, SURA_TOOL_INTERACTIVE_APPROVAL=1, or SURA_TOOL_AUTO_APPROVE=1";
        }
    }
    return false;
}

inline std::mutex& tool_audit_mutex() {
    static std::mutex m;
    return m;
}

inline std::string tool_audit_timestamp_utc() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    char buf[32];
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv)) return "";
    return std::string(buf);
}

inline std::string tool_audit_target(GCDict* spec) {
    if (const Value* url = dict_get_ptr(spec, "url")) {
        if (url->is_str()) return tool_redact_newlines(url->as_str());
    }
    if (const Value* command = dict_get_ptr(spec, "command")) {
        if (command->is_str()) return tool_redact_newlines(command->as_str());
    }
    return "";
}

inline void tool_audit_log_event(GCDict* spec, GCDict* policy, const std::string& event,
                                 bool approval_required, const std::string& reason, int line) {
    const char* raw_path = std::getenv("SURA_TOOL_AUDIT_LOG");
    if (!raw_path || !*raw_path || !spec) return;

    std::string path = raw_path;
    std::error_code ec;
    std::filesystem::path log_path = fs_path_from_utf8(path);
    if (log_path.has_parent_path()) std::filesystem::create_directories(log_path.parent_path(), ec);

    std::lock_guard<std::mutex> lock(tool_audit_mutex());
    std::ofstream out(log_path, std::ios::binary | std::ios::app);
    if (!out) return;

    bool token_configured = policy && dict_get_ptr(policy, "approval_token");
    std::string approval_message;
    if (policy) {
        if (const Value* message = dict_get_ptr(policy, "approval_message")) {
            if (message->is_str()) approval_message = tool_redact_newlines(message->as_str());
        }
    }
    out << "{\"version\":1"
        << ",\"ts\":\"" << json_escape(tool_audit_timestamp_utc()) << "\""
        << ",\"event\":\"" << json_escape(event) << "\""
        << ",\"tool\":\"" << json_escape(tool_spec_name(spec)) << "\""
        << ",\"target\":\"" << json_escape(tool_audit_target(spec)) << "\""
        << ",\"approvalRequired\":" << (approval_required ? "true" : "false")
        << ",\"approvalTokenConfigured\":" << (token_configured ? "true" : "false")
        << ",\"approvalMessage\":\"" << json_escape(approval_message) << "\""
        << ",\"line\":" << line
        << ",\"reason\":\"" << json_escape(reason) << "\""
        << "}\n";
}

inline size_t tool_http_request_body_size(GCDict* spec) {
    if (const Value* json_body = dict_get_ptr(spec, "json")) return json_stringify_value(*json_body).size();
    if (const Value* form_body = dict_get_ptr(spec, "form")) {
        return form_body->is_dict() ? form_build_from_dict(form_body->as_dict(), 0, "http_request").size() : 0;
    }
    if (const Value* body = dict_get_ptr(spec, "body")) {
        return body->is_str() ? body->as_str().size() : json_stringify_value(*body).size();
    }
    return 0;
}

inline bool tool_validate_spec(const Value& spec, std::string* reason) {
    auto deny = [&](const std::string& r) {
        if (reason) *reason = r;
        return false;
    };

    if (!spec.is_dict()) return deny("tool spec must be a dict");
    auto* d = spec.as_dict();
    auto name_it = d->elements.find("name");
    if (name_it == d->elements.end() || !name_it->second.is_str()) {
        return deny("tool spec requires string field 'name'");
    }
    std::string name = name_it->second.as_str();
    if (!tool_known_name(name)) return deny("unknown tool '" + name + "'");

    if (tool_is_http_name(name)) {
        auto url_it = d->elements.find("url");
        if (url_it == d->elements.end() || !url_it->second.is_str()) {
            return deny(name + " tool requires string field 'url'");
        }
        std::string url = url_it->second.as_str();
        if (url.rfind("http://", 0) != 0 &&
            url.rfind("https://", 0) != 0 &&
            url.rfind("file://", 0) != 0) {
            return deny(name + " URL must start with http://, https://, or file://");
        }
        if (url.find_first_of("\"\r\n") != std::string::npos) {
            return deny(name + " URL contains unsupported characters");
        }
    }

    if (name == "http_request") {
        bool has_body_field = d->elements.find("body") != d->elements.end();
        bool has_json_field = d->elements.find("json") != d->elements.end();
        bool has_form_field = d->elements.find("form") != d->elements.end();
        bool has_body = has_body_field || has_json_field || has_form_field;
        if ((has_body_field ? 1 : 0) + (has_json_field ? 1 : 0) + (has_form_field ? 1 : 0) > 1) {
            return deny("http_request tool accepts only one of 'body', 'json', or 'form'");
        }
        auto method_it = d->elements.find("method");
        if (method_it != d->elements.end() && !method_it->second.is_str()) {
            return deny("http_request method must be a string");
        }
        std::string method = tool_http_request_method(d);
        if (!http_method_safe(method)) return deny("http_request method must contain letters only");
        auto query_it = d->elements.find("query");
        bool has_query_params = false;
        if (query_it != d->elements.end()) {
            if (!query_it->second.is_dict()) return deny("http_request query must be a dict");
            has_query_params = !query_it->second.as_dict()->elements.empty();
            for (const auto& [_, query_value] : query_it->second.as_dict()->elements) {
                if (query_value.is_dict()) return deny("http_request query values must be scalars or arrays of scalars");
                if (query_value.is_arr()) {
                    for (const auto& item : query_value.as_arr()->elements) {
                        if (item.is_arr() || item.is_dict()) return deny("http_request query values must be scalars or arrays of scalars");
                    }
                }
            }
        }
        auto form_it = d->elements.find("form");
        if (form_it != d->elements.end()) {
            if (!form_it->second.is_dict()) return deny("http_request form must be a dict");
            for (const auto& [_, form_value] : form_it->second.as_dict()->elements) {
                if (form_value.is_dict()) return deny("http_request form values must be scalars or arrays of scalars");
                if (form_value.is_arr()) {
                    for (const auto& item : form_value.as_arr()->elements) {
                        if (item.is_arr() || item.is_dict()) return deny("http_request form values must be scalars or arrays of scalars");
                    }
                }
            }
        }
        auto url_it = d->elements.find("url");
        if (url_it != d->elements.end() && url_it->second.is_str() &&
            url_it->second.as_str().rfind("file://", 0) == 0 &&
            (method != "GET" || has_body)) {
            return deny("http_request file:// only supports body-less GET");
        }
        if (url_it != d->elements.end() && url_it->second.is_str() &&
            url_it->second.as_str().rfind("file://", 0) == 0 && has_query_params) {
            return deny("http_request file:// does not support query");
        }
        auto headers_it = d->elements.find("headers");
        if (headers_it != d->elements.end()) {
            if (!headers_it->second.is_dict()) return deny("http_request headers must be a dict");
            for (const auto& [header_name, header_value] : headers_it->second.as_dict()->elements) {
                if (!http_header_name_safe(header_name)) return deny("http_request header name contains unsupported characters");
                if (!header_value.is_str()) return deny("http_request header values must be strings");
                if (!http_shell_value_safe(header_value.as_str())) return deny("http_request header value contains unsupported characters");
            }
        }
        auto content_type_it = d->elements.find("content_type");
        if (content_type_it != d->elements.end()) {
            if (!content_type_it->second.is_str()) return deny("http_request content_type must be a string");
            if (!http_shell_value_safe(content_type_it->second.as_str())) return deny("http_request content_type contains unsupported characters");
        }
        auto timeout_it = d->elements.find("timeout");
        if (timeout_it != d->elements.end()) {
            if (!timeout_it->second.is_num()) return deny("http_request timeout must be a number");
            double timeout = timeout_it->second.as_num();
            if (timeout <= 0 || timeout > 3600) return deny("http_request timeout must be 1..3600 seconds");
        }
    }

    if (name == "shell") {
        auto command_it = d->elements.find("command");
        if (command_it == d->elements.end() || !command_it->second.is_str()) {
            return deny("shell tool requires string field 'command'");
        }
        std::string command = command_it->second.as_str();
        if (command.empty()) return deny("shell command must not be empty");
        if (command.find_first_of("\r\n") != std::string::npos) {
            return deny("shell command contains unsupported newline characters");
        }
    }

    if (reason) *reason = "";
    return true;
}

inline Value tool_schema_for(const std::string& name, int l) {
    if (!tool_known_name(name)) throw JitThrow{"tool_schema(): unknown tool '" + name + "'", l};
    Value schema = Value::make_dict();
    auto* root = schema.as_dict();
    root->elements["name"] = Value(name);
    root->elements["kind"] = Value("tool");
    root->elements["required"] = Value::make_array();
    root->elements["fields"] = Value::make_dict();

    auto* required = root->elements["required"].as_arr();
    auto* fields = root->elements["fields"].as_dict();
    if (name == "http_get") {
        required->elements.push_back(Value("url"));
        fields->elements["url"] = Value("string:http-url|file-url");
    } else if (name == "http_request") {
        required->elements.push_back(Value("url"));
        fields->elements["url"] = Value("string:http-url|file-url");
        fields->elements["query"] = Value("dict:query-params");
        fields->elements["method"] = Value("string:http-method");
        fields->elements["headers"] = Value("dict:string-values");
        fields->elements["body"] = Value("string|json-value");
        fields->elements["json"] = Value("json-value");
        fields->elements["form"] = Value("dict:form-params");
        fields->elements["content_type"] = Value("string:mime-type");
        fields->elements["timeout"] = Value("number:seconds");
    } else if (name == "shell") {
        required->elements.push_back(Value("command"));
        fields->elements["command"] = Value("string:shell-command");
    }
    return schema;
}

inline Value run_tool_spec(GCDict* d, int l) {
    Value spec_value((GCObject*)d);
    std::string reason;
    if (!tool_validate_spec(spec_value, &reason)) {
        throw JitThrow{"tool_call(): invalid spec: " + reason, l};
    }
    std::string name = d->elements.count("name") ? d->elements["name"].to_str() : "";
    if (name == "shell") {
        std::string command = d->elements.count("command") ? d->elements["command"].to_str() : "";
        return Value(run_capture_command(command));
    }
    if (name == "http_get") {
        Value arg[1] = {Value(d->elements.count("url") ? d->elements["url"].to_str() : "")};
        return b_http_get(arg, 1, l);
    }
    if (name == "http_request") {
        Value arg[1] = {spec_value};
        return b_http_request(arg, 1, l);
    }
    throw JitThrow{"tool_call(): unknown tool '" + name + "'", l};
}

inline bool tool_allowed_by_policy(const Value& spec, const Value& policy, std::string* reason) {
    auto deny = [&](const std::string& r) {
        if (reason) *reason = r;
        return false;
    };

    if (!spec.is_dict()) return deny("tool spec must be a dict");
    if (!policy.is_dict()) return deny("tool policy must be a dict");

    auto* sd = spec.as_dict();
    auto* pd = policy.as_dict();
    std::string name = tool_spec_name(sd);
    if (!tool_validate_spec(spec, reason)) return false;

    auto tools_it = pd->elements.find("tools");
    if (tools_it != pd->elements.end() && !tool_array_contains(tools_it->second, name)) {
        return deny("tool '" + name + "' is not listed in policy.tools");
    }

    bool approval_required = false;
    if (!tool_policy_requires_approval(pd, &approval_required, reason)) return false;
    (void)approval_required;
    if (const Value* token_value = dict_get_ptr(pd, "approval_token")) {
        if (!token_value->is_str()) return deny("policy.approval_token must be a string");
        if (!tool_policy_token_safe(token_value->as_str())) {
            return deny("policy.approval_token must be 1..512 bytes and contain no newlines");
        }
    }
    if (const Value* message_value = dict_get_ptr(pd, "approval_message")) {
        if (!message_value->is_str()) return deny("policy.approval_message must be a string");
    }

    if (name == "shell") {
        auto allow_it = pd->elements.find("allow_shell");
        if (allow_it == pd->elements.end() || !allow_it->second.truthy()) {
            return deny("shell tool requires allow_shell: true");
        }
        auto prefix_it = pd->elements.find("command_prefixes");
        if (prefix_it != pd->elements.end()) {
            std::string command = sd->elements.count("command") ? sd->elements["command"].to_str() : "";
            if (!tool_starts_with_any(command, prefix_it->second)) {
                return deny("shell command is outside policy.command_prefixes");
            }
        }
    }

    if (tool_is_http_name(name)) {
        auto prefix_it = pd->elements.find("url_prefixes");
        if (prefix_it != pd->elements.end()) {
            std::string url = sd->elements.count("url") ? sd->elements["url"].to_str() : "";
            if (!tool_starts_with_any(url, prefix_it->second)) {
                return deny(name + " URL is outside policy.url_prefixes");
            }
        }
    }

    if (name == "http_request") {
        auto methods_it = pd->elements.find("http_methods");
        if (methods_it != pd->elements.end()) {
            std::string method = tool_http_request_method(sd);
            if (!tool_array_contains_upper(methods_it->second, method)) {
                return deny("http_request method is outside policy.http_methods");
            }
        }

        auto allowed_headers_it = pd->elements.find("allowed_headers");
        if (allowed_headers_it != pd->elements.end()) {
            if (!allowed_headers_it->second.is_arr()) return deny("policy.allowed_headers must be an array");
            if (const Value* headers_value = dict_get_ptr(sd, "headers")) {
                if (!headers_value->is_dict()) return deny("http_request headers must be a dict");
                for (const auto& [header_name, _] : headers_value->as_dict()->elements) {
                    if (!tool_array_contains_header(allowed_headers_it->second, header_name)) {
                        return deny("http_request header '" + header_name + "' is outside policy.allowed_headers");
                    }
                }
            }
        }

        auto required_headers_it = pd->elements.find("required_headers");
        if (required_headers_it != pd->elements.end()) {
            if (!required_headers_it->second.is_dict()) return deny("policy.required_headers must be a dict");
            const Value* headers_value = dict_get_ptr(sd, "headers");
            if (!headers_value || !headers_value->is_dict()) return deny("http_request missing policy.required_headers");
            auto* headers = headers_value->as_dict();
            for (const auto& [required_name, required_value] : required_headers_it->second.as_dict()->elements) {
                if (!required_value.is_str()) return deny("policy.required_headers values must be strings");
                const Value* actual = tool_header_value(headers, required_name);
                if (!actual || !actual->is_str()) {
                    return deny("http_request missing required header '" + required_name + "'");
                }
                if (actual->as_str() != required_value.as_str()) {
                    return deny("http_request header '" + required_name + "' does not match policy.required_headers");
                }
            }
        }

        auto max_body_it = pd->elements.find("max_body_bytes");
        if (max_body_it != pd->elements.end()) {
            if (!max_body_it->second.is_num()) return deny("policy.max_body_bytes must be a number");
            double max_body = max_body_it->second.as_num();
            if (max_body < 0) return deny("policy.max_body_bytes must be non-negative");
            if ((double)tool_http_request_body_size(sd) > max_body) {
                return deny("http_request body exceeds policy.max_body_bytes");
            }
        }

        auto max_timeout_it = pd->elements.find("max_timeout");
        if (max_timeout_it != pd->elements.end()) {
            if (!max_timeout_it->second.is_num()) return deny("policy.max_timeout must be a number");
            double max_timeout = max_timeout_it->second.as_num();
            if (max_timeout <= 0) return deny("policy.max_timeout must be positive");
            double requested_timeout = 30.0;
            if (const Value* timeout_value = dict_get_ptr(sd, "timeout")) {
                if (!timeout_value->is_num()) return deny("http_request timeout must be a number");
                requested_timeout = timeout_value->as_num();
            }
            if (requested_timeout > max_timeout) {
                return deny("http_request timeout exceeds policy.max_timeout");
            }
        }
    }

    if (reason) *reason = "";
    return true;
}

inline Value b_tool_call(const Value* a, int n, int l) {
    need_args("tool_call", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"tool_call(): expected dict", l};
    return run_tool_spec(a[0].as_dict(), l);
}

inline Value b_tool_spec(const Value* a, int n, int l) {
    need_args("tool_spec", n, 2, 2, l);
    std::string name = need_str("tool_spec", a[0], 0, l);
    if (!a[1].is_dict()) throw JitThrow{"tool_spec(): arg 2 must be a dict", l};
    Value out = Value::make_dict();
    auto* dst = out.as_dict();
    dst->elements = a[1].as_dict()->elements;
    dst->elements["name"] = Value(name);
    std::string reason;
    if (!tool_validate_spec(out, &reason)) {
        throw JitThrow{"tool_spec(): invalid spec: " + reason, l};
    }
    return out;
}

inline Value b_tool_validate(const Value* a, int n, int l) {
    need_args("tool_validate", n, 1, 1, l);
    std::string reason;
    return Value(tool_validate_spec(a[0], &reason));
}

inline Value b_tool_schema(const Value* a, int n, int l) {
    need_args("tool_schema", n, 1, 1, l);
    return tool_schema_for(need_str("tool_schema", a[0], 0, l), l);
}

inline Value b_tool_allowed(const Value* a, int n, int l) {
    need_args("tool_allowed", n, 2, 2, l);
    std::string reason;
    return Value(tool_allowed_by_policy(a[0], a[1], &reason));
}

inline Value b_tool_call_policy(const Value* a, int n, int l) {
    need_args("tool_call_policy", n, 2, 2, l);
    std::string reason;
    GCDict* spec_dict = a[0].is_dict() ? a[0].as_dict() : nullptr;
    GCDict* policy_dict = a[1].is_dict() ? a[1].as_dict() : nullptr;
    bool approval_required = false;
    if (policy_dict) {
        std::string approval_reason;
        if (!tool_policy_requires_approval(policy_dict, &approval_required, &approval_reason)) {
            reason = approval_reason;
        }
    }
    if (!tool_allowed_by_policy(a[0], a[1], &reason)) {
        if (spec_dict && policy_dict) {
            tool_audit_log_event(spec_dict, policy_dict, "policy_denied", approval_required, reason, l);
        }
        throw JitThrow{"tool_call_policy(): blocked by policy: " + reason, l};
    }
    tool_audit_log_event(spec_dict, policy_dict, "policy_allowed", approval_required, "", l);
    if (!tool_policy_approval_granted(a[0].as_dict(), a[1].as_dict(), &reason)) {
        tool_audit_log_event(spec_dict, policy_dict, "approval_denied", approval_required, reason, l);
        throw JitThrow{"tool_call_policy(): blocked by policy: " + reason, l};
    }
    if (approval_required) {
        tool_audit_log_event(spec_dict, policy_dict, "approval_granted", approval_required, "", l);
    }
    try {
        Value result = run_tool_spec(a[0].as_dict(), l);
        tool_audit_log_event(spec_dict, policy_dict, "executed", approval_required, "", l);
        return result;
    } catch (const JitThrow& e) {
        tool_audit_log_event(spec_dict, policy_dict, "execution_failed", approval_required, e.message, l);
        throw;
    } catch (const std::exception& e) {
        tool_audit_log_event(spec_dict, policy_dict, "execution_failed", approval_required, e.what(), l);
        throw;
    } catch (...) {
        tool_audit_log_event(spec_dict, policy_dict, "execution_failed", approval_required, "unknown error", l);
        throw;
    }
}

inline Value b_llm_run_tools(const Value* a, int n, int l) {
    need_args("llm_run_tools", n, 2, 2, l);
    Value calls = b_llm_tool_calls(a, 1, l);
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    for (const auto& call : calls.as_arr()->elements) {
        Value policy_args[2] = {call, a[1]};
        Value result = b_tool_call_policy(policy_args, 2, l);
        Value result_args[2] = {call, result};
        arr->elements.push_back(b_llm_tool_result(result_args, 2, l));
    }
    return out;
}

inline Value llm_assistant_message_from_response(Value response, int line) {
    if (response.is_str()) response = JsonParser(response.as_str(), line).parse_document();

    Value fallback = Value::make_dict();
    auto* fallback_dict = fallback.as_dict();
    fallback_dict->elements["role"] = Value(std::string("assistant"));
    if (!response.is_dict()) return fallback;

    auto clone_message = [&](const Value& source) {
        Value clone_args[1] = {source};
        Value msg = b_clone(clone_args, 1, line);
        if (!msg.is_dict()) return fallback;
        auto* msg_dict = msg.as_dict();
        if (msg_dict->elements.find("role") == msg_dict->elements.end())
            msg_dict->elements["role"] = Value(std::string("assistant"));
        return msg;
    };

    auto* root = response.as_dict();
    auto message_it = root->elements.find("message");
    if (message_it != root->elements.end() && message_it->second.is_dict())
        return clone_message(message_it->second);

    auto choices_it = root->elements.find("choices");
    if (choices_it != root->elements.end() && choices_it->second.is_arr()) {
        auto* choices = choices_it->second.as_arr();
        if (!choices->elements.empty() && choices->elements[0].is_dict()) {
            auto* choice = choices->elements[0].as_dict();
            auto choice_message_it = choice->elements.find("message");
            if (choice_message_it != choice->elements.end() && choice_message_it->second.is_dict())
                return clone_message(choice_message_it->second);
            auto text_it = choice->elements.find("text");
            if (text_it != choice->elements.end()) {
                fallback_dict->elements["content"] = Value(text_it->second.to_str());
                return fallback;
            }
        }
    }

    auto tool_calls_it = root->elements.find("tool_calls");
    if (tool_calls_it != root->elements.end() && tool_calls_it->second.is_arr())
        fallback_dict->elements["tool_calls"] = tool_calls_it->second;
    auto content_it = root->elements.find("content");
    if (content_it != root->elements.end())
        fallback_dict->elements["content"] = Value(content_it->second.to_str());
    return fallback;
}

inline Value b_llm_next_messages(const Value* a, int n, int l) {
    need_args("llm_next_messages", n, 3, 3, l);
    if (!a[0].is_arr()) throw JitThrow{"llm_next_messages(): messages must be an array", l};
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    for (const auto& msg : a[0].as_arr()->elements) arr->elements.push_back(msg);
    arr->elements.push_back(llm_assistant_message_from_response(a[1], l));

    Value tool_args[2] = {a[1], a[2]};
    Value tool_messages = b_llm_run_tools(tool_args, 2, l);
    for (const auto& msg : tool_messages.as_arr()->elements) arr->elements.push_back(msg);
    return out;
}

inline Value b_llm_next_request(const Value* a, int n, int l) {
    need_args("llm_next_request", n, 5, 6, l);
    Value next_args[3] = {a[1], a[2], a[3]};
    Value next_messages = b_llm_next_messages(next_args, 3, l);
    Value req_args[4] = {a[0], next_messages, a[4], n >= 6 ? a[5] : Value(0.2)};
    return b_llm_request_tools(req_args, 4, l);
}

inline Value b_llm_next_request_json(const Value* a, int n, int l) {
    Value req = b_llm_next_request(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value b_llm_next_schema_request(const Value* a, int n, int l) {
    need_args("llm_next_schema_request", n, 6, 9, l);
    Value next_args[3] = {a[1], a[2], a[3]};
    Value next_messages = b_llm_next_messages(next_args, 3, l);
    Value req_args[7] = {
        a[0],
        next_messages,
        a[4],
        a[5],
        n >= 7 ? a[6] : Value(0.2),
        n >= 8 ? a[7] : Value(std::string("sura_response")),
        n >= 9 ? a[8] : Value(true)
    };
    int req_count = 4 + std::max(0, n - 6);
    return b_llm_request_tools_schema(req_args, req_count, l);
}

inline Value b_llm_next_schema_request_json(const Value* a, int n, int l) {
    Value req = b_llm_next_schema_request(a, n, l);
    return Value(json_stringify_value(req));
}

inline Value b_tool_list(const Value* a, int n, int l) {
    need_args("tool_list", n, 0, 0, l);
    Value out = Value::make_array();
    auto* arr = out.as_arr();
    arr->elements.push_back(Value("http_get"));
    arr->elements.push_back(Value("http_request"));
    arr->elements.push_back(Value("shell"));
    return out;
}

inline Value b_file_lines(const Value* a, int n, int l) {
    need_args("file_lines", n, 1, 1, l);
    std::istringstream ss(read_text_file(need_str("file_lines", a[0], 0, l), l));
    Value out = Value::make_array();
    std::string line;
    while (std::getline(ss, line)) out.as_arr()->elements.push_back(Value(line));
    return out;
}

inline Value b_python_eval(const Value* a, int n, int l) {
    need_args("python_eval", n, 1, 1, l);
    auto tmp = std::filesystem::temp_directory_path() /
        ("sura_py_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".py");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << need_str("python_eval", a[0], 0, l) << "\n";
    }
    std::string result = run_capture_command("python \"" + tmp.string() + "\"");
    std::filesystem::remove(tmp);
    return Value(result);
}

inline std::string command_arg_quote(const std::string& text) {
    return shell_quote_arg(text);
}

inline bool python_ref_is_safe(const std::string& ref) {
    std::regex re("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*$");
    return std::regex_match(ref, re);
}

inline std::string python_command_candidate(const std::string& raw) {
    if (raw.empty() || raw.find_first_of("\r\n") != std::string::npos) return "";
    if (raw.find(' ') != std::string::npos && raw.find('/') == std::string::npos && raw.find('\\') == std::string::npos) {
        return raw;
    }
    return raw.find_first_of(" \t\"") == std::string::npos ? raw : command_arg_quote(raw);
}

inline std::string find_python_command() {
    static std::string cached;
    static bool searched = false;
    if (searched) return cached;
    searched = true;

    std::vector<std::string> candidates;
    const char* env = std::getenv("SURA_PYTHON");
    if (env && *env) candidates.push_back(env);
    candidates.push_back("python");
    candidates.push_back("python3");
#ifdef _WIN32
    candidates.push_back("py -3");
    candidates.push_back("C:\\msys64\\mingw64\\bin\\python.exe");
    candidates.push_back("C:\\msys64\\ucrt64\\bin\\python.exe");
#endif

    std::regex version_re("Python\\s+[0-9]+\\.[0-9]+");
    for (const auto& candidate : candidates) {
        std::string cmd = python_command_candidate(candidate);
        if (cmd.empty()) continue;
        std::string out = run_capture_command(cmd + " --version 2>&1");
        if (std::regex_search(out, version_re)) {
            cached = cmd;
            return cached;
        }
    }
    return "";
}

inline Value b_python_available(const Value*, int n, int l) {
    need_args("python_available", n, 0, 0, l);
    return Value(!find_python_command().empty());
}

inline Value b_python_executable(const Value*, int n, int l) {
    need_args("python_executable", n, 0, 0, l);
    std::string cmd = find_python_command();
    if (cmd.empty()) return Value(std::string(""));
    return Value(cmd);
}

inline std::string python_args_json(const Value* a, int n, int index, int line) {
    if (n <= index || a[index].is_nil()) return "[]";
    if (a[index].is_str()) return a[index].as_str();
    if (!a[index].is_arr()) throw JitThrow{"python_call(): args must be an array or JSON string", line};
    return json_stringify_value(a[index]);
}

inline std::string python_kwargs_json(const Value* a, int n, int index, int line) {
    if (n <= index || a[index].is_nil()) return "{}";
    if (a[index].is_str()) return a[index].as_str();
    if (!a[index].is_dict()) throw JitThrow{"python_call(): kwargs must be a dict or JSON string", line};
    return json_stringify_value(a[index]);
}

inline Value b_python_call(const Value* a, int n, int l) {
    need_args("python_call", n, 2, 4, l);
    std::string module = need_str("python_call", a[0], 0, l);
    std::string func = need_str("python_call", a[1], 1, l);
    if (!python_ref_is_safe(module) || !python_ref_is_safe(func)) {
        throw JitThrow{"python_call(): module and function must be dotted Python identifiers", l};
    }
    std::string python = find_python_command();
    if (python.empty()) throw JitThrow{"python_call(): Python executable not found; set SURA_PYTHON", l};
    std::string json_args = python_args_json(a, n, 2, l);
    std::string json_kwargs = python_kwargs_json(a, n, 3, l);
    auto tmp = std::filesystem::temp_directory_path() /
        ("sura_pycall_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".py");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "import importlib,json,sys\n"
            << "m=importlib.import_module(" << json_stringify_value(Value(module)) << ")\n"
            << "args=json.loads(" << json_stringify_value(Value(json_args)) << ")\n"
            << "kwargs=json.loads(" << json_stringify_value(Value(json_kwargs)) << ")\n"
            << "target=m\n"
            << "for part in " << json_stringify_value(Value(func)) << ".split('.'):\n"
            << "    target=getattr(target, part)\n"
            << "print(json.dumps(target(*args, **kwargs), ensure_ascii=False, default=str))\n";
    }
    std::string result = run_capture_command(python + " " + command_arg_quote(tmp.string()) + " 2>&1");
    std::filesystem::remove(tmp);
    return Value(result);
}

inline Value b_python_call_json(const Value* a, int n, int l) {
    Value raw = b_python_call(a, n, l);
    std::string text = raw.as_str();
    try {
        return JsonParser(text, l).parse_document();
    } catch (...) {
        throw JitThrow{"python_call_json(): Python result is not JSON: " + text, l};
    }
}

inline void* ffi_open_library(const std::string& path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW);
#endif
}

inline void* ffi_find_symbol(void* handle, const std::string& name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name.c_str()));
#else
    return dlsym(handle, name.c_str());
#endif
}

inline void ffi_close_library(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

inline const SuraPluginHostApi& plugin_host_api();
inline void plugin_destroy_descriptor_state(SuraPluginDescriptor& desc, const SuraPluginHostApi& host_api);

struct PluginHostQuota {
    size_t max_memory_bytes = 0;
    size_t current_memory_bytes = 0;
    std::unordered_map<void*, size_t> allocations;
};

inline thread_local PluginHostQuota* active_plugin_host_quota = nullptr;
inline thread_local bool active_plugin_cancel_deadline_enabled = false;
inline thread_local std::chrono::steady_clock::time_point active_plugin_cancel_deadline;

struct PluginHostQuotaScope {
    PluginHostQuota* previous = nullptr;

    explicit PluginHostQuotaScope(PluginHostQuota* quota)
        : previous(active_plugin_host_quota) {
        active_plugin_host_quota = quota;
    }

    ~PluginHostQuotaScope() {
        active_plugin_host_quota = previous;
    }
};

struct PluginCancelScope {
    bool previous_enabled = false;
    std::chrono::steady_clock::time_point previous_deadline{};

    explicit PluginCancelScope(size_t max_call_ms)
        : previous_enabled(active_plugin_cancel_deadline_enabled),
          previous_deadline(active_plugin_cancel_deadline) {
        if (max_call_ms > 0) {
            active_plugin_cancel_deadline_enabled = true;
            active_plugin_cancel_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)max_call_ms);
        } else {
            active_plugin_cancel_deadline_enabled = false;
        }
    }

    ~PluginCancelScope() {
        active_plugin_cancel_deadline_enabled = previous_enabled;
        active_plugin_cancel_deadline = previous_deadline;
    }
};

inline void plugin_host_quota_free_leaks(PluginHostQuota& quota) {
    for (const auto& entry : quota.allocations) {
        std::free(entry.first);
    }
    quota.allocations.clear();
    quota.current_memory_bytes = 0;
}

struct LoadedPlugin {
    std::string path;
    std::string manifest_path;
    void* handle = nullptr;
    SuraPluginDescriptor descriptor{};
    SuraPluginHostApi host_api{};
    PluginHostQuota quota{};
    SuraPluginLifecycleFn on_load = nullptr;
    SuraPluginLifecycleFn on_unload = nullptr;
    size_t max_call_ms = 0;
    size_t call_count = 0;
    double last_call_ms = 0.0;
    double total_call_ms = 0.0;
    std::unordered_set<std::string> allowed_exports;
    std::unordered_set<std::string> host_capabilities;
    bool restrict_exports = false;

    LoadedPlugin(std::string p, void* h, const SuraPluginDescriptor& d,
                 const SuraPluginHostApi& api,
                 PluginHostQuota q = {},
                 SuraPluginLifecycleFn load_hook = nullptr,
                 SuraPluginLifecycleFn unload_hook = nullptr,
                 size_t call_limit_ms = 0,
                 std::unordered_set<std::string> allowed = {},
                 std::unordered_set<std::string> host_caps = {},
                 std::string manifest = "")
        : path(std::move(p)),
          manifest_path(std::move(manifest)),
          handle(h),
          descriptor(d),
          host_api(api),
          quota(std::move(q)),
          on_load(load_hook),
          on_unload(unload_hook),
          max_call_ms(call_limit_ms),
          allowed_exports(std::move(allowed)),
          host_capabilities(std::move(host_caps)),
          restrict_exports(!allowed_exports.empty()) {}
    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;
    ~LoadedPlugin() {
        PluginHostQuotaScope scope(&quota);
        if (on_unload) {
            SuraPluginContext ctx{&host_api, descriptor.user_data};
            int rc = on_unload(&ctx);
            if (rc != 0 && host_api.log) {
                host_api.log("sura_plugin_on_unload returned a non-zero status");
            }
        }
        plugin_destroy_descriptor_state(descriptor, host_api);
        plugin_host_quota_free_leaks(quota);
        ffi_close_library(handle);
    }
};

inline std::mutex& plugin_registry_mutex() {
    static std::mutex m;
    return m;
}

inline std::vector<std::unique_ptr<LoadedPlugin>>& plugin_registry() {
    static std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    return plugins;
}

inline void plugin_host_log(const char* message) {
    std::cerr << "[SuraPlugin] " << (message ? message : "") << "\n";
}

inline void plugin_host_log_disabled(const char*) {}

inline void* plugin_host_alloc(size_t bytes) {
    PluginHostQuota* quota = active_plugin_host_quota;
    if (quota && quota->max_memory_bytes > 0) {
        if (bytes > quota->max_memory_bytes ||
            quota->current_memory_bytes > quota->max_memory_bytes - bytes) {
            return nullptr;
        }
    }

    void* ptr = std::malloc(bytes == 0 ? 1 : bytes);
    if (!ptr) return nullptr;
    if (quota) {
        try {
            quota->allocations[ptr] = bytes;
            quota->current_memory_bytes += bytes;
        } catch (...) {
            std::free(ptr);
            return nullptr;
        }
    }
    return ptr;
}

inline void* plugin_host_alloc_disabled(size_t) {
    return nullptr;
}

inline void plugin_host_free(void* ptr) {
    if (!ptr) return;
    PluginHostQuota* quota = active_plugin_host_quota;
    if (quota) {
        auto it = quota->allocations.find(ptr);
        if (it != quota->allocations.end()) {
            if (quota->current_memory_bytes >= it->second) quota->current_memory_bytes -= it->second;
            else quota->current_memory_bytes = 0;
            quota->allocations.erase(it);
        }
    }
    std::free(ptr);
}

inline void plugin_host_free_disabled(void*) {}

inline std::unordered_set<std::string> plugin_default_host_capabilities() {
    return {"log", "memory", "cancel"};
}

struct PluginRuntimePolicy {
    std::unordered_set<std::string> allowed_exports;
    std::unordered_set<std::string> host_capabilities;
    size_t max_memory_bytes = 0;
    size_t max_call_ms = 0;
    bool restrict_host_capabilities = false;
    bool restrict_memory_bytes = false;
    bool restrict_call_ms = false;
};

inline const SuraPluginHostApi& plugin_host_api() {
    static const SuraPluginHostApi api{
        SURA_PLUGIN_ABI_VERSION,
        plugin_host_log,
        plugin_host_alloc,
        plugin_host_free,
        []() -> int {
            return active_plugin_cancel_deadline_enabled &&
                   std::chrono::steady_clock::now() >= active_plugin_cancel_deadline;
        }
    };
    return api;
}

inline int plugin_host_should_cancel() {
    return active_plugin_cancel_deadline_enabled &&
           std::chrono::steady_clock::now() >= active_plugin_cancel_deadline;
}

inline int plugin_host_should_cancel_disabled() {
    return 0;
}

inline SuraPluginHostApi plugin_host_api_for(const std::unordered_set<std::string>& capabilities) {
    bool log_enabled = capabilities.count("log") > 0;
    bool memory_enabled = capabilities.count("memory") > 0;
    bool cancel_enabled = capabilities.count("cancel") > 0;
    return SuraPluginHostApi{
        SURA_PLUGIN_ABI_VERSION,
        log_enabled ? plugin_host_log : plugin_host_log_disabled,
        memory_enabled ? plugin_host_alloc : plugin_host_alloc_disabled,
        memory_enabled ? plugin_host_free : plugin_host_free_disabled,
        cancel_enabled ? plugin_host_should_cancel : plugin_host_should_cancel_disabled
    };
}

inline void plugin_destroy_descriptor_state(SuraPluginDescriptor& desc, const SuraPluginHostApi& host_api) {
    if (desc.user_data && desc.destroy_user_data) {
        desc.destroy_user_data(&host_api, desc.user_data);
    }
    desc.user_data = nullptr;
    desc.destroy_user_data = nullptr;
}

inline SuraPluginLifecycleFn plugin_lifecycle_symbol(void* handle, const char* symbol_name) {
    return reinterpret_cast<SuraPluginLifecycleFn>(ffi_find_symbol(handle, symbol_name));
}

inline void plugin_invoke_load_hook(SuraPluginLifecycleFn hook,
                                    const SuraPluginHostApi& host_api,
                                    PluginHostQuota& quota,
                                    SuraPluginDescriptor& desc,
                                    size_t max_call_ms,
                                    const std::string& path,
                                    int line) {
    if (!hook) return;
    SuraPluginContext ctx{&host_api, desc.user_data};
    auto started = std::chrono::steady_clock::now();
    int rc = 0;
    {
        PluginCancelScope cancel_scope(max_call_ms);
        PluginHostQuotaScope scope(&quota);
        rc = hook(&ctx);
    }
    auto finished = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    if (rc == SURA_PLUGIN_CANCELLED) {
        throw JitThrow{"plugin_load(): sura_plugin_on_load cancelled: " + path, line};
    }
    if (rc != 0) {
        throw JitThrow{"plugin_load(): sura_plugin_on_load rejected plugin: " + path, line};
    }
    if (max_call_ms > 0 && elapsed_ms > (double)max_call_ms) {
        throw JitThrow{"plugin_load(): sura_plugin_on_load exceeded max_call_ms: " + path, line};
    }
}

inline int plugin_id_from_value(const char* name, const Value& v, int idx, int line) {
    if (v.is_num()) {
        int id = (int)v.as_num();
        if (id >= 0 && (double)id == v.as_num()) return id;
    }
    if (v.is_str()) {
        std::string token = v.as_str();
        const std::string prefix = "plugin:";
        if (token.rfind(prefix, 0) == 0) {
            size_t start = prefix.size();
            size_t end = token.find(':', start);
            std::string num = token.substr(start, end == std::string::npos ? std::string::npos : end - start);
            try {
                size_t used = 0;
                int id = std::stoi(num, &used);
                if (used == num.size() && id >= 0) return id;
            } catch (...) {}
        }
    }
    throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1)
                   + " must be a plugin handle from plugin_load()", line};
}

inline LoadedPlugin* plugin_from_value(const char* name, const Value& v, int idx, int line) {
    int id = plugin_id_from_value(name, v, idx, line);
    auto& plugins = plugin_registry();
    if (id < 0 || (size_t)id >= plugins.size() || !plugins[(size_t)id]) {
        throw JitThrow{std::string(name) + "(): plugin handle is not loaded", line};
    }
    return plugins[(size_t)id].get();
}

inline SuraPluginValue plugin_value_from_sura(const char* name, const Value& v, int idx,
                                              int line, std::vector<std::string>& strings) {
    SuraPluginValue out{};
    if (v.is_nil()) {
        out.type = SURA_PLUGIN_NIL;
    } else if (v.is_num()) {
        out.type = SURA_PLUGIN_NUMBER;
        out.as.number_value = v.as_num();
    } else if (v.is_bool()) {
        out.type = SURA_PLUGIN_BOOL;
        out.as.bool_value = v.as_bool() ? 1 : 0;
    } else if (v.is_str()) {
        out.type = SURA_PLUGIN_STRING;
        strings.push_back(v.as_str());
        out.as.string_value = strings.back().c_str();
    } else {
        throw JitThrow{std::string(name) + "(): arg " + std::to_string(idx + 1)
                       + " must be nil, number, bool, or string for native plugin calls", line};
    }
    return out;
}

inline Value plugin_value_to_sura(const SuraPluginValue& v) {
    switch (v.type) {
        case SURA_PLUGIN_NIL: return Value::nil();
        case SURA_PLUGIN_NUMBER: return Value(v.as.number_value);
        case SURA_PLUGIN_BOOL: return Value(v.as.bool_value != 0);
        case SURA_PLUGIN_STRING: return Value(std::string(v.as.string_value ? v.as.string_value : ""));
        default: return Value::nil();
    }
}

inline const SuraPluginExport* plugin_find_export(const LoadedPlugin& plugin, const std::string& name) {
    if (!plugin.descriptor.exports) return nullptr;
    for (size_t i = 0; i < plugin.descriptor.export_count; ++i) {
        const SuraPluginExport& ex = plugin.descriptor.exports[i];
        if (ex.name && name == ex.name) return &ex;
    }
    return nullptr;
}

inline bool plugin_descriptor_has_export(const SuraPluginDescriptor& desc, const std::string& name) {
    if (!desc.exports) return false;
    for (size_t i = 0; i < desc.export_count; ++i) {
        const SuraPluginExport& ex = desc.exports[i];
        if (ex.name && name == ex.name) return true;
    }
    return false;
}

inline std::string plugin_file_sha256(const std::string& path, int line) {
    if (!std::filesystem::exists(fs_path_from_utf8(path))) throw JitThrow{"plugin_load_manifest(): library not found: " + path, line};
#ifdef _WIN32
    std::string cmd = "certutil -hashfile \"" + path + "\" SHA256";
#else
    std::string cmd = "sha256sum \"" + path + "\" 2>/dev/null || shasum -a 256 \"" + path + "\"";
#endif
    std::string raw = run_capture_command(cmd);
    std::regex hex_re("([A-Fa-f0-9]{64})");
    std::smatch m;
    if (std::regex_search(raw, m, hex_re)) {
        std::string h = m[1].str();
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return h;
    }
    throw JitThrow{"plugin_load_manifest(): system SHA256 tool failed", line};
}

inline std::string plugin_trim_lower(std::string text) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return text;
}

inline bool plugin_manifest_policy_required() {
    const char* raw = std::getenv("SURA_PLUGIN_POLICY");
    if (!raw) return false;
    std::string mode = plugin_trim_lower(raw);
    return mode == "manifest-locked" || mode == "locked" ||
           mode == "require" || mode == "required" || mode == "1" || mode == "true";
}

inline std::unordered_set<std::string> plugin_policy_exports(GCDict* d, int line) {
    auto it = d->elements.find("allowed_exports");
    if (it == d->elements.end()) it = d->elements.find("exports");
    std::unordered_set<std::string> out;
    if (it == d->elements.end()) return out;
    if (!it->second.is_arr()) {
        throw JitThrow{"plugin_load_manifest(): sura.plugins.json allowed_exports must be an array", line};
    }
    for (const auto& item : it->second.as_arr()->elements) {
        if (!item.is_str()) {
            throw JitThrow{"plugin_load_manifest(): every sura.plugins.json allowed_exports item must be a string", line};
        }
        out.insert(item.as_str());
    }
    if (out.empty()) {
        throw JitThrow{"plugin_load_manifest(): sura.plugins.json allowed_exports cannot be empty when present", line};
    }
    return out;
}

inline std::unordered_set<std::string> plugin_manifest_host_capabilities(GCDict* d, int line) {
    auto it = d->elements.find("host_capabilities");
    if (it == d->elements.end()) return plugin_default_host_capabilities();
    if (!it->second.is_arr()) {
        throw JitThrow{"plugin_load_manifest(): host_capabilities must be an array", line};
    }
    std::unordered_set<std::string> out;
    for (const auto& item : it->second.as_arr()->elements) {
        if (!item.is_str()) {
            throw JitThrow{"plugin_load_manifest(): every host_capabilities item must be a string", line};
        }
        std::string cap = item.as_str();
        if (cap != "log" && cap != "memory" && cap != "cancel") {
            throw JitThrow{"plugin_load_manifest(): unsupported host capability: " + cap, line};
        }
        out.insert(cap);
    }
    return out;
}

inline std::unordered_set<std::string> plugin_policy_host_capabilities(GCDict* d, int line, bool* present = nullptr) {
    auto it = d->elements.find("host_capabilities");
    if (it == d->elements.end()) {
        if (present) *present = false;
        return {};
    }
    if (present) *present = true;
    if (!it->second.is_arr()) {
        throw JitThrow{"plugin_load_manifest(): sura.plugins.json host_capabilities must be an array", line};
    }
    std::unordered_set<std::string> out;
    for (const auto& item : it->second.as_arr()->elements) {
        if (!item.is_str()) {
            throw JitThrow{"plugin_load_manifest(): every sura.plugins.json host_capabilities item must be a string", line};
        }
        std::string cap = item.as_str();
        if (cap != "log" && cap != "memory" && cap != "cancel") {
            throw JitThrow{"plugin_load_manifest(): unsupported sura.plugins.json host capability: " + cap, line};
        }
        out.insert(cap);
    }
    return out;
}

inline size_t plugin_nonnegative_size_field(GCDict* d, const std::string& field,
                                            const std::string& context, int line,
                                            bool* present = nullptr) {
    auto it = d->elements.find(field);
    if (it == d->elements.end()) {
        if (present) *present = false;
        return 0;
    }
    if (present) *present = true;
    if (!it->second.is_num()) {
        throw JitThrow{"plugin_load_manifest(): " + context + " " + field + " must be a non-negative integer", line};
    }
    double raw = it->second.as_num();
    if (!std::isfinite(raw) || raw < 0 || std::floor(raw) != raw ||
        raw > (double)std::numeric_limits<size_t>::max()) {
        throw JitThrow{"plugin_load_manifest(): " + context + " " + field + " must be a non-negative integer", line};
    }
    return (size_t)raw;
}

inline size_t plugin_manifest_max_memory_bytes(GCDict* d, int line) {
    return plugin_nonnegative_size_field(d, "max_memory_bytes", "manifest field", line);
}

inline size_t plugin_policy_max_memory_bytes(GCDict* d, int line, bool* present = nullptr) {
    return plugin_nonnegative_size_field(d, "max_memory_bytes", "sura.plugins.json", line, present);
}

inline size_t plugin_manifest_max_call_ms(GCDict* d, int line) {
    return plugin_nonnegative_size_field(d, "max_call_ms", "manifest field", line);
}

inline size_t plugin_policy_max_call_ms(GCDict* d, int line, bool* present = nullptr) {
    return plugin_nonnegative_size_field(d, "max_call_ms", "sura.plugins.json", line, present);
}

inline std::unordered_set<std::string> plugin_intersect_capabilities(
        const std::unordered_set<std::string>& base,
        const std::unordered_set<std::string>& limiter) {
    std::unordered_set<std::string> out;
    for (const auto& cap : base) {
        if (limiter.count(cap)) out.insert(cap);
    }
    return out;
}

inline std::string plugin_policy_manifest_ref(const std::string& manifest_path) {
    std::error_code ec;
    std::filesystem::path manifest = std::filesystem::absolute(fs_path_from_utf8(manifest_path), ec);
    if (ec) manifest = fs_path_from_utf8(manifest_path);
    manifest = manifest.lexically_normal();

    std::filesystem::path root = std::filesystem::current_path(ec);
    if (!ec) {
        std::error_code rel_ec;
        std::filesystem::path rel = std::filesystem::relative(manifest, root, rel_ec);
        if (!rel_ec && !rel.empty()) return fs_path_to_generic_utf8(rel.lexically_normal());
    }
    return fs_path_to_generic_utf8(fs_path_from_utf8(manifest_path).lexically_normal());
}

inline PluginRuntimePolicy plugin_enforce_runtime_manifest_policy(const std::string& manifest_path, int line) {
    if (!plugin_manifest_policy_required()) return {};

    std::error_code cwd_ec;
    std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    if (cwd_ec) {
        throw JitThrow{"plugin_load_manifest(): cannot resolve current package root for SURA_PLUGIN_POLICY", line};
    }
    std::filesystem::path policy_path = cwd / "sura.plugins.json";
    if (!std::filesystem::exists(policy_path)) {
        throw JitThrow{"plugin_load_manifest(): SURA_PLUGIN_POLICY=manifest-locked requires sura.plugins.json in the current package root", line};
    }
    Value parsed = JsonParser(read_text_file(fs_path_to_utf8(policy_path), line), line).parse_document();
    if (!parsed.is_dict()) throw JitThrow{"plugin_load_manifest(): sura.plugins.json root must be a JSON object", line};
    auto* d = parsed.as_dict();
    auto sandbox_it = d->elements.find("sandbox");
    if (sandbox_it == d->elements.end() || !sandbox_it->second.is_str() ||
        sandbox_it->second.as_str() != "manifest-locked") {
        throw JitThrow{"plugin_load_manifest(): sura.plugins.json must set sandbox to manifest-locked", line};
    }
    auto manifests_it = d->elements.find("manifests");
    if (manifests_it == d->elements.end() || !manifests_it->second.is_arr()) {
        throw JitThrow{"plugin_load_manifest(): sura.plugins.json must contain a manifests array", line};
    }
    PluginRuntimePolicy policy;
    policy.allowed_exports = plugin_policy_exports(d, line);
    bool host_caps_present = false;
    policy.host_capabilities = plugin_policy_host_capabilities(d, line, &host_caps_present);
    policy.restrict_host_capabilities = host_caps_present;
    bool max_memory_present = false;
    policy.max_memory_bytes = plugin_policy_max_memory_bytes(d, line, &max_memory_present);
    policy.restrict_memory_bytes = max_memory_present;
    bool max_call_present = false;
    policy.max_call_ms = plugin_policy_max_call_ms(d, line, &max_call_present);
    policy.restrict_call_ms = max_call_present;

    std::string target = plugin_policy_manifest_ref(manifest_path);
    auto* manifests = manifests_it->second.as_arr();
    for (const auto& item : manifests->elements) {
        if (!item.is_str()) {
            throw JitThrow{"plugin_load_manifest(): every sura.plugins.json manifest entry must be a string", line};
        }
        std::string listed = fs_path_to_generic_utf8(fs_path_from_utf8(item.as_str()).lexically_normal());
        if (listed == target) return policy;
    }
    throw JitThrow{"plugin_load_manifest(): manifest not allowed by sura.plugins.json: " + target, line};
}

inline Value plugin_register_loaded(const std::string& path, void* handle, const SuraPluginDescriptor& desc,
                                    const SuraPluginHostApi& host_api,
                                    PluginHostQuota quota,
                                    SuraPluginLifecycleFn on_load,
                                    SuraPluginLifecycleFn on_unload,
                                    size_t max_call_ms,
                                    std::unordered_set<std::string> allowed_exports,
                                    std::unordered_set<std::string> host_capabilities,
                                    const std::string& manifest_path) {
    std::lock_guard<std::mutex> lock(plugin_registry_mutex());
    auto& plugins = plugin_registry();
    size_t id = plugins.size();
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (!plugins[i]) {
            id = i;
            break;
        }
    }
    auto loaded = std::make_unique<LoadedPlugin>(path, handle, desc, host_api,
                                                 std::move(quota),
                                                 on_load,
                                                 on_unload,
                                                 max_call_ms,
                                                 std::move(allowed_exports),
                                                 std::move(host_capabilities),
                                                 manifest_path);
    if (id == plugins.size()) plugins.push_back(std::move(loaded));
    else plugins[id] = std::move(loaded);
    return Value(std::string("plugin:") + std::to_string(id) + ":" + desc.name);
}

inline Value plugin_load_checked(const std::string& path, int line,
                                 const std::string& expected_name = "",
                                 const std::string& expected_version = "",
                                 const std::string& expected_sha256 = "",
                                 std::unordered_set<std::string> allowed_exports = {},
                                 std::unordered_set<std::string> host_capabilities = plugin_default_host_capabilities(),
                                 const std::string& manifest_path = "",
                                 size_t max_memory_bytes = 0,
                                 size_t max_call_ms = 0) {
    if (!std::filesystem::exists(fs_path_from_utf8(path))) throw JitThrow{"plugin_load(): library not found: " + path, line};
    if (!expected_sha256.empty()) {
        std::string actual = plugin_file_sha256(path, line);
        std::string expected = expected_sha256;
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (actual != expected) {
            throw JitThrow{"plugin_load_manifest(): sha256 mismatch for " + path, line};
        }
    }

    void* handle = ffi_open_library(path);
    if (!handle) throw JitThrow{"plugin_load(): failed to load library: " + path, line};

    void* raw_init = ffi_find_symbol(handle, "sura_plugin_init");
    if (!raw_init) {
        ffi_close_library(handle);
        throw JitThrow{"plugin_load(): sura_plugin_init symbol not found: " + path, line};
    }
    SuraPluginLifecycleFn on_load = plugin_lifecycle_symbol(handle, "sura_plugin_on_load");
    SuraPluginLifecycleFn on_unload = plugin_lifecycle_symbol(handle, "sura_plugin_on_unload");

    SuraPluginDescriptor desc{};
    auto init = reinterpret_cast<SuraPluginInitFn>(raw_init);
    SuraPluginHostApi host_api = plugin_host_api_for(host_capabilities);
    PluginHostQuota quota;
    quota.max_memory_bytes = max_memory_bytes;
    auto cleanup = [&]() {
        PluginHostQuotaScope scope(&quota);
        plugin_destroy_descriptor_state(desc, host_api);
        plugin_host_quota_free_leaks(quota);
        ffi_close_library(handle);
    };
    int rc = 0;
    {
        PluginHostQuotaScope scope(&quota);
        rc = init(&host_api, &desc);
    }
    uint32_t major = desc.abi_version / 10000u;
    if (rc != 0 || major != SURA_PLUGIN_ABI_VERSION_MAJOR || !desc.name) {
        cleanup();
        throw JitThrow{"plugin_load(): plugin rejected by ABI/version check: " + path, line};
    }

    if (desc.export_count > 0 && !desc.exports) {
        cleanup();
        throw JitThrow{"plugin_load(): invalid export table: " + path, line};
    }
    for (size_t i = 0; i < desc.export_count; ++i) {
        if (!desc.exports[i].name || !desc.exports[i].function) {
            cleanup();
            throw JitThrow{"plugin_load(): invalid export entry: " + path, line};
        }
    }

    std::string actual_name = desc.name ? desc.name : "";
    std::string actual_version = desc.version ? desc.version : "";
    if (!expected_name.empty() && actual_name != expected_name) {
        cleanup();
        throw JitThrow{"plugin_load_manifest(): expected plugin name '" + expected_name + "', got '" + actual_name + "'", line};
    }
    if (!expected_version.empty() && actual_version != expected_version) {
        cleanup();
        throw JitThrow{"plugin_load_manifest(): expected plugin version '" + expected_version + "', got '" + actual_version + "'", line};
    }
    for (const auto& allowed : allowed_exports) {
        if (!plugin_descriptor_has_export(desc, allowed)) {
            cleanup();
            throw JitThrow{"plugin_load_manifest(): allowed export not found: " + allowed, line};
        }
    }
    try {
        plugin_invoke_load_hook(on_load, host_api, quota, desc, max_call_ms, path, line);
    } catch (...) {
        cleanup();
        throw;
    }

    return plugin_register_loaded(path, handle, desc, host_api, std::move(quota), on_load, on_unload,
                                  max_call_ms, std::move(allowed_exports),
                                  std::move(host_capabilities), manifest_path);
}

inline Value b_plugin_load(const Value* a, int n, int l) {
    need_args("plugin_load", n, 1, 1, l);
    std::string path = need_str("plugin_load", a[0], 0, l);
    if (plugin_manifest_policy_required()) {
        throw JitThrow{"plugin_load(): direct plugin loading blocked by SURA_PLUGIN_POLICY=manifest-locked; use plugin_load_manifest()", l};
    }
    return plugin_load_checked(path, l);
}

inline std::string plugin_manifest_string(GCDict* d, const std::string& key, int line) {
    auto it = d->elements.find(key);
    if (it == d->elements.end() || !it->second.is_str()) {
        throw JitThrow{"plugin_load_manifest(): manifest field '" + key + "' must be a string", line};
    }
    return it->second.as_str();
}

inline std::unordered_set<std::string> plugin_manifest_exports(GCDict* d, int line) {
    auto it = d->elements.find("exports");
    if (it == d->elements.end()) it = d->elements.find("allowed_exports");
    if (it == d->elements.end() || !it->second.is_arr()) {
        throw JitThrow{"plugin_load_manifest(): manifest field 'exports' must be an array", line};
    }
    std::unordered_set<std::string> out;
    for (const auto& item : it->second.as_arr()->elements) {
        if (!item.is_str()) {
            throw JitThrow{"plugin_load_manifest(): every export allow-list item must be a string", line};
        }
        out.insert(item.as_str());
    }
    if (out.empty()) throw JitThrow{"plugin_load_manifest(): exports allow-list cannot be empty", line};
    return out;
}

inline Value b_plugin_load_manifest(const Value* a, int n, int l) {
    need_args("plugin_load_manifest", n, 1, 1, l);
    std::string manifest_path = need_str("plugin_load_manifest", a[0], 0, l);
    PluginRuntimePolicy runtime_policy = plugin_enforce_runtime_manifest_policy(manifest_path, l);
    Value parsed = JsonParser(read_text_file(manifest_path, l), l).parse_document();
    if (!parsed.is_dict()) throw JitThrow{"plugin_load_manifest(): manifest root must be a JSON object", l};
    auto* d = parsed.as_dict();

    std::string rel_path = plugin_manifest_string(d, "path", l);
    std::string expected_name = plugin_manifest_string(d, "name", l);
    std::string expected_version = plugin_manifest_string(d, "version", l);
    std::string expected_sha256 = plugin_manifest_string(d, "sha256", l);
    std::unordered_set<std::string> allowed = plugin_manifest_exports(d, l);
    std::unordered_set<std::string> host_capabilities = plugin_manifest_host_capabilities(d, l);
    size_t max_memory_bytes = plugin_manifest_max_memory_bytes(d, l);
    size_t max_call_ms = plugin_manifest_max_call_ms(d, l);
    if (runtime_policy.restrict_host_capabilities) {
        host_capabilities = plugin_intersect_capabilities(host_capabilities, runtime_policy.host_capabilities);
    }
    if (runtime_policy.restrict_memory_bytes && runtime_policy.max_memory_bytes > 0 &&
        (max_memory_bytes == 0 || runtime_policy.max_memory_bytes < max_memory_bytes)) {
        max_memory_bytes = runtime_policy.max_memory_bytes;
    }
    if (runtime_policy.restrict_call_ms && runtime_policy.max_call_ms > 0 &&
        (max_call_ms == 0 || runtime_policy.max_call_ms < max_call_ms)) {
        max_call_ms = runtime_policy.max_call_ms;
    }
    if (!runtime_policy.allowed_exports.empty()) {
        for (const auto& name : runtime_policy.allowed_exports) {
            if (!allowed.count(name)) {
                throw JitThrow{"plugin_load_manifest(): sura.plugins.json allowed export not declared by plugin manifest: " + name, l};
            }
        }
        allowed = std::move(runtime_policy.allowed_exports);
    }

    std::filesystem::path lib_path = fs_path_from_utf8(rel_path);
    if (lib_path.is_relative()) {
        lib_path = fs_path_from_utf8(manifest_path).parent_path() / lib_path;
    }
    return plugin_load_checked(fs_path_to_utf8(lib_path), l, expected_name, expected_version,
                               expected_sha256, std::move(allowed),
                               std::move(host_capabilities), manifest_path, max_memory_bytes, max_call_ms);
}

inline Value b_plugin_info(const Value* a, int n, int l) {
    need_args("plugin_info", n, 1, 1, l);
    std::lock_guard<std::mutex> lock(plugin_registry_mutex());
    LoadedPlugin* plugin = plugin_from_value("plugin_info", a[0], 0, l);

    Value info = Value::make_dict();
    auto* d = info.as_dict();
    d->elements["name"] = Value(std::string(plugin->descriptor.name ? plugin->descriptor.name : ""));
    d->elements["version"] = Value(std::string(plugin->descriptor.version ? plugin->descriptor.version : ""));
    d->elements["abi_version"] = Value((double)plugin->descriptor.abi_version);
    d->elements["path"] = Value(plugin->path);
    d->elements["manifest"] = Value(plugin->manifest_path);
    d->elements["policy"] = Value(plugin->restrict_exports ? "manifest" : "direct");
    d->elements["has_state"] = Value(plugin->descriptor.user_data != nullptr);
    d->elements["has_on_load"] = Value(plugin->on_load != nullptr);
    d->elements["has_on_unload"] = Value(plugin->on_unload != nullptr);

    Value exports = Value::make_array();
    auto* arr = exports.as_arr();
    for (size_t i = 0; i < plugin->descriptor.export_count; ++i) {
        const SuraPluginExport& ex = plugin->descriptor.exports[i];
        Value item = Value::make_dict();
        auto* ed = item.as_dict();
        ed->elements["name"] = Value(std::string(ex.name ? ex.name : ""));
        ed->elements["min_args"] = Value((double)ex.min_args);
        ed->elements["max_args"] = Value((double)ex.max_args);
        ed->elements["doc"] = Value(std::string(ex.doc ? ex.doc : ""));
        arr->elements.push_back(item);
    }
    d->elements["exports"] = exports;

    Value allowed = Value::make_array();
    for (const auto& name : plugin->allowed_exports) allowed.as_arr()->elements.push_back(Value(name));
    d->elements["allowed_exports"] = allowed;

    Value host_caps = Value::make_array();
    for (const auto& name : plugin->host_capabilities) host_caps.as_arr()->elements.push_back(Value(name));
    d->elements["host_capabilities"] = host_caps;
    d->elements["max_memory_bytes"] = Value((double)plugin->quota.max_memory_bytes);
    d->elements["memory_bytes"] = Value((double)plugin->quota.current_memory_bytes);
    d->elements["max_call_ms"] = Value((double)plugin->max_call_ms);
    d->elements["call_count"] = Value((double)plugin->call_count);
    d->elements["last_call_ms"] = Value(plugin->last_call_ms);
    d->elements["total_call_ms"] = Value(plugin->total_call_ms);
    return info;
}

inline Value b_plugin_call(const Value* a, int n, int l) {
    need_args("plugin_call", n, 2, -1, l);
    std::lock_guard<std::mutex> lock(plugin_registry_mutex());
    LoadedPlugin* plugin = plugin_from_value("plugin_call", a[0], 0, l);
    std::string export_name = need_str("plugin_call", a[1], 1, l);
    if (plugin->restrict_exports && !plugin->allowed_exports.count(export_name)) {
        throw JitThrow{"plugin_call(): export not allowed by manifest: " + export_name, l};
    }
    const SuraPluginExport* ex = plugin_find_export(*plugin, export_name);
    if (!ex) throw JitThrow{"plugin_call(): export not found: " + export_name, l};

    int argc = n - 2;
    if (argc < ex->min_args || (ex->max_args >= 0 && argc > ex->max_args)) {
        throw JitThrow{"plugin_call(): " + export_name + " expects "
                       + std::to_string(ex->min_args)
                       + (ex->max_args == ex->min_args ? "" : ".." + std::to_string(ex->max_args))
                       + " arg(s), got " + std::to_string(argc), l};
    }

    std::vector<std::string> string_storage;
    string_storage.reserve((size_t)argc);
    std::vector<SuraPluginValue> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) {
        args.push_back(plugin_value_from_sura("plugin_call", a[2 + i], 2 + i, l, string_storage));
    }

    SuraPluginValue out{};
    out.type = SURA_PLUGIN_NIL;
    SuraPluginContext ctx{&plugin->host_api, plugin->descriptor.user_data};
    auto started = std::chrono::steady_clock::now();
    int rc = 0;
    {
        PluginCancelScope cancel_scope(plugin->host_capabilities.count("cancel") > 0 ? plugin->max_call_ms : 0);
        PluginHostQuotaScope scope(&plugin->quota);
        rc = ex->function(&ctx, args.empty() ? nullptr : args.data(), argc, &out);
    }
    auto finished = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    plugin->call_count += 1;
    plugin->last_call_ms = elapsed_ms;
    plugin->total_call_ms += elapsed_ms;
    if (rc == SURA_PLUGIN_CANCELLED) {
        throw JitThrow{"plugin_call(): native export cancelled: " + export_name, l};
    }
    if (rc != 0) throw JitThrow{"plugin_call(): native export failed: " + export_name, l};
    if (plugin->max_call_ms > 0 && elapsed_ms > (double)plugin->max_call_ms) {
        throw JitThrow{"plugin_call(): native export exceeded max_call_ms for " + export_name, l};
    }
    return plugin_value_to_sura(out);
}

inline Value b_plugin_unload(const Value* a, int n, int l) {
    need_args("plugin_unload", n, 1, 1, l);
    int id = plugin_id_from_value("plugin_unload", a[0], 0, l);
    std::lock_guard<std::mutex> lock(plugin_registry_mutex());
    auto& plugins = plugin_registry();
    if (id < 0 || (size_t)id >= plugins.size() || !plugins[(size_t)id]) return Value(false);
    plugins[(size_t)id].reset();
    return Value(true);
}

inline Value b_ffi_load(const Value* a, int n, int l) {
    need_args("ffi_load", n, 1, 1, l);
    std::string path = need_str("ffi_load", a[0], 0, l);
    if (!std::filesystem::exists(fs_path_from_utf8(path))) throw JitThrow{"ffi_load(): library not found: " + path, l};
    return Value(path);
}

inline bool ffi_signature_uses_double(const std::string& sig) {
    return sig.find("double") != std::string::npos || sig.find("float") != std::string::npos;
}

inline bool ffi_signature_returns_void(const std::string& sig) {
    return sig.rfind("void", 0) == 0;
}

inline bool ffi_signature_returns_string(const std::string& sig) {
    return sig.rfind("char*", 0) == 0 || sig.rfind("const char*", 0) == 0;
}

inline std::string ffi_trim_type(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    text = text.substr(start, end - start);
    text = std::regex_replace(text, std::regex("\\s+"), " ");
    text = std::regex_replace(text, std::regex("\\s*\\*\\s*"), "*");
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

inline std::vector<std::string> ffi_signature_arg_types(const std::string& sig) {
    std::vector<std::string> out;
    size_t open = sig.find('(');
    size_t close = sig.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) return out;
    std::string body = sig.substr(open + 1, close - open - 1);
    if (ffi_trim_type(body).empty() || ffi_trim_type(body) == "void") return out;
    std::string current;
    for (char ch : body) {
        if (ch == ',') {
            out.push_back(ffi_trim_type(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!ffi_trim_type(current).empty()) out.push_back(ffi_trim_type(current));
    return out;
}

inline bool ffi_type_is_string(const std::string& type) {
    std::string t = ffi_trim_type(type);
    return t == "char*" || t == "const char*";
}

inline bool ffi_type_is_double(const std::string& type) {
    std::string t = ffi_trim_type(type);
    return t == "double" || t == "float";
}

enum class FfiSimpleArgKind { Int, CString };

template <typename Ret>
inline Ret ffi_dispatch_int_string(void* fn, int argc,
                                   const std::array<FfiSimpleArgKind, 4>& kinds,
                                   const std::array<long long, 4>& ints,
                                   const std::array<const char*, 4>& strings) {
    switch (argc) {
        case 0:
            return reinterpret_cast<Ret(*)()>(fn)();
        case 1:
            if (kinds[0] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*)>(fn)(strings[0]);
            return reinterpret_cast<Ret(*)(long long)>(fn)(ints[0]);
        case 2:
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*)>(fn)(strings[0], strings[1]);
            if (kinds[0] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long)>(fn)(strings[0], ints[1]);
            if (kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*)>(fn)(ints[0], strings[1]);
            return reinterpret_cast<Ret(*)(long long, long long)>(fn)(ints[0], ints[1]);
        case 3:
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, const char*)>(fn)(strings[0], strings[1], strings[2]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, long long)>(fn)(strings[0], strings[1], ints[2]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, const char*)>(fn)(strings[0], ints[1], strings[2]);
            if (kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, const char*)>(fn)(ints[0], strings[1], strings[2]);
            if (kinds[0] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, long long)>(fn)(strings[0], ints[1], ints[2]);
            if (kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, long long)>(fn)(ints[0], strings[1], ints[2]);
            if (kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, long long, const char*)>(fn)(ints[0], ints[1], strings[2]);
            return reinterpret_cast<Ret(*)(long long, long long, long long)>(fn)(ints[0], ints[1], ints[2]);
        case 4:
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, const char*, const char*)>(fn)(strings[0], strings[1], strings[2], strings[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, const char*, long long)>(fn)(strings[0], strings[1], strings[2], ints[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, long long, const char*)>(fn)(strings[0], strings[1], ints[2], strings[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, const char*, const char*)>(fn)(strings[0], ints[1], strings[2], strings[3]);
            if (kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, const char*, const char*)>(fn)(ints[0], strings[1], strings[2], strings[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, const char*, long long, long long)>(fn)(strings[0], strings[1], ints[2], ints[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, const char*, long long)>(fn)(strings[0], ints[1], strings[2], ints[3]);
            if (kinds[0] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, long long, const char*)>(fn)(strings[0], ints[1], ints[2], strings[3]);
            if (kinds[1] == FfiSimpleArgKind::CString && kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, const char*, long long)>(fn)(ints[0], strings[1], strings[2], ints[3]);
            if (kinds[1] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, long long, const char*)>(fn)(ints[0], strings[1], ints[2], strings[3]);
            if (kinds[2] == FfiSimpleArgKind::CString && kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, long long, const char*, const char*)>(fn)(ints[0], ints[1], strings[2], strings[3]);
            if (kinds[0] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(const char*, long long, long long, long long)>(fn)(strings[0], ints[1], ints[2], ints[3]);
            if (kinds[1] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, const char*, long long, long long)>(fn)(ints[0], strings[1], ints[2], ints[3]);
            if (kinds[2] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, long long, const char*, long long)>(fn)(ints[0], ints[1], strings[2], ints[3]);
            if (kinds[3] == FfiSimpleArgKind::CString)
                return reinterpret_cast<Ret(*)(long long, long long, long long, const char*)>(fn)(ints[0], ints[1], ints[2], strings[3]);
            return reinterpret_cast<Ret(*)(long long, long long, long long, long long)>(fn)(ints[0], ints[1], ints[2], ints[3]);
    }
    if constexpr (std::is_void<Ret>::value) return;
    else return Ret{};
}

inline Value b_ffi_call(const Value* a, int n, int l) {
    need_args("ffi_call", n, 2, 7, l);
    std::string lib = need_str("ffi_call", a[0], 0, l);
    std::string sym = need_str("ffi_call", a[1], 1, l);
    std::string sig = "double()";
    int first_arg = 2;
    bool explicit_signature = false;
    if (n >= 3 && a[2].is_str() && a[2].as_str().find('(') != std::string::npos) {
        sig = a[2].as_str();
        first_arg = 3;
        explicit_signature = true;
    }
    int argc = n - first_arg;
    if (argc < 0 || argc > 4) throw JitThrow{"ffi_call(): supports 0..4 arguments", l};
    std::vector<std::string> sig_args = ffi_signature_arg_types(sig);
    if (explicit_signature && (int)sig_args.size() != argc) {
        throw JitThrow{"ffi_call(): signature argument count does not match provided values", l};
    }
    bool has_string_arg = std::any_of(sig_args.begin(), sig_args.end(), ffi_type_is_string);
    bool has_double_arg = std::any_of(sig_args.begin(), sig_args.end(), ffi_type_is_double);
    if (has_string_arg && has_double_arg) {
        throw JitThrow{"ffi_call(): string and double arguments cannot be mixed in this FFI mode", l};
    }

    void* handle = ffi_open_library(lib);
    if (!handle) throw JitThrow{"ffi_call(): failed to load library: " + lib, l};
    void* fn = ffi_find_symbol(handle, sym);
    if (!fn) {
        ffi_close_library(handle);
        throw JitThrow{"ffi_call(): symbol not found: " + sym, l};
    }

    Value result = Value::nil();
    if (has_string_arg) {
        std::array<FfiSimpleArgKind, 4> kinds = {
            FfiSimpleArgKind::Int, FfiSimpleArgKind::Int, FfiSimpleArgKind::Int, FfiSimpleArgKind::Int
        };
        std::array<long long, 4> ints = {0, 0, 0, 0};
        std::array<std::string, 4> owned_strings;
        std::array<const char*, 4> strings = {"", "", "", ""};
        for (int i = 0; i < argc; ++i) {
            if (ffi_type_is_string(sig_args[(size_t)i])) {
                kinds[(size_t)i] = FfiSimpleArgKind::CString;
                owned_strings[(size_t)i] = need_str("ffi_call", a[first_arg + i], first_arg + i, l);
                strings[(size_t)i] = owned_strings[(size_t)i].c_str();
            } else {
                ints[(size_t)i] = (long long)a[first_arg + i].to_num();
            }
        }
        if (ffi_signature_returns_void(sig)) {
            ffi_dispatch_int_string<void>(fn, argc, kinds, ints, strings);
            result = Value::nil();
        } else if (ffi_signature_returns_string(sig)) {
            const char* out = ffi_dispatch_int_string<const char*>(fn, argc, kinds, ints, strings);
            result = Value(std::string(out ? out : ""));
        } else {
            long long out = ffi_dispatch_int_string<long long>(fn, argc, kinds, ints, strings);
            result = Value((double)out);
        }
    } else if (ffi_signature_uses_double(sig)) {
        double x[4] = {0, 0, 0, 0};
        for (int i = 0; i < argc; ++i) x[i] = need_num("ffi_call", a[first_arg + i], first_arg + i, l);
        if (ffi_signature_returns_void(sig)) {
            if (argc == 0) reinterpret_cast<void(*)()>(fn)();
            else if (argc == 1) reinterpret_cast<void(*)(double)>(fn)(x[0]);
            else if (argc == 2) reinterpret_cast<void(*)(double,double)>(fn)(x[0], x[1]);
            else if (argc == 3) reinterpret_cast<void(*)(double,double,double)>(fn)(x[0], x[1], x[2]);
            else reinterpret_cast<void(*)(double,double,double,double)>(fn)(x[0], x[1], x[2], x[3]);
            result = Value::nil();
        } else {
            double out = 0;
            if (argc == 0) out = reinterpret_cast<double(*)()>(fn)();
            else if (argc == 1) out = reinterpret_cast<double(*)(double)>(fn)(x[0]);
            else if (argc == 2) out = reinterpret_cast<double(*)(double,double)>(fn)(x[0], x[1]);
            else if (argc == 3) out = reinterpret_cast<double(*)(double,double,double)>(fn)(x[0], x[1], x[2]);
            else out = reinterpret_cast<double(*)(double,double,double,double)>(fn)(x[0], x[1], x[2], x[3]);
            result = Value(out);
        }
    } else {
        long long x[4] = {0, 0, 0, 0};
        for (int i = 0; i < argc; ++i) x[i] = (long long)a[first_arg + i].to_num();
        if (ffi_signature_returns_void(sig)) {
            if (argc == 0) reinterpret_cast<void(*)()>(fn)();
            else if (argc == 1) reinterpret_cast<void(*)(long long)>(fn)(x[0]);
            else if (argc == 2) reinterpret_cast<void(*)(long long,long long)>(fn)(x[0], x[1]);
            else if (argc == 3) reinterpret_cast<void(*)(long long,long long,long long)>(fn)(x[0], x[1], x[2]);
            else reinterpret_cast<void(*)(long long,long long,long long,long long)>(fn)(x[0], x[1], x[2], x[3]);
            result = Value::nil();
        } else if (ffi_signature_returns_string(sig)) {
            const char* out = nullptr;
            if (argc == 0) out = reinterpret_cast<const char*(*)()>(fn)();
            else if (argc == 1) out = reinterpret_cast<const char*(*)(long long)>(fn)(x[0]);
            else if (argc == 2) out = reinterpret_cast<const char*(*)(long long,long long)>(fn)(x[0], x[1]);
            else if (argc == 3) out = reinterpret_cast<const char*(*)(long long,long long,long long)>(fn)(x[0], x[1], x[2]);
            else out = reinterpret_cast<const char*(*)(long long,long long,long long,long long)>(fn)(x[0], x[1], x[2], x[3]);
            result = Value(std::string(out ? out : ""));
        } else {
            long long out = 0;
            if (argc == 0) out = reinterpret_cast<long long(*)()>(fn)();
            else if (argc == 1) out = reinterpret_cast<long long(*)(long long)>(fn)(x[0]);
            else if (argc == 2) out = reinterpret_cast<long long(*)(long long,long long)>(fn)(x[0], x[1]);
            else if (argc == 3) out = reinterpret_cast<long long(*)(long long,long long,long long)>(fn)(x[0], x[1], x[2]);
            else out = reinterpret_cast<long long(*)(long long,long long,long long,long long)>(fn)(x[0], x[1], x[2], x[3]);
            result = Value((double)out);
        }
    }

    ffi_close_library(handle);
    return result;
}

struct CommandRunResult {
    int exit_code = -1;
    std::string output;
};

inline int command_exit_code_from_status(int status) {
    if (status < 0) return -1;
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
#endif
}

inline CommandRunResult run_capture_command_status(const std::string& command) {
    CommandRunResult result;
#ifdef _WIN32
    std::wstring wide_command = windows_path_bytes_to_wide(command);
    FILE* pipe = _wpopen(wide_command.c_str(), L"r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return result;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) result.output += buf;
#ifdef _WIN32
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
#endif
    result.exit_code = command_exit_code_from_status(status);
    return result;
}

inline std::string run_capture_command(const std::string& command) {
    return run_capture_command_status(command).output;
}

// A cancellation source is deliberately separate from AsyncTask.  Native
// operations can hold this small object without keeping a task handle alive.
struct AsyncCancellationState {
    std::atomic<bool> requested{false};
    std::mutex mutex;
    std::condition_variable cv;
};

struct AsyncCancelled {};

class AsyncCancellationToken {
    std::shared_ptr<AsyncCancellationState> state_;

public:
    AsyncCancellationToken() = default;
    explicit AsyncCancellationToken(std::shared_ptr<AsyncCancellationState> state)
        : state_(std::move(state)) {}

    bool stop_requested() const {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

    void throw_if_cancelled() const {
        if (stop_requested()) throw AsyncCancelled{};
    }

    template <class Rep, class Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (!state_) {
            std::this_thread::sleep_for(duration);
            return false;
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->cv.wait_for(lock, duration, [&]() {
            return state_->requested.load(std::memory_order_acquire);
        });
    }
};

inline bool async_request_stop(const std::shared_ptr<AsyncCancellationState>& state) {
    if (!state) return false;
    bool first_request = false;
    {
        // Modify the predicate while holding the same mutex used by waiters.
        // Atomic visibility alone does not prevent the notify-before-sleep
        // window required by condition_variable's protocol.
        std::lock_guard<std::mutex> lock(state->mutex);
        first_request = !state->requested.exchange(true, std::memory_order_acq_rel);
    }
    state->cv.notify_all();
    return first_request;
}

// Captured subprocess output is retained by a task until it is consumed.  A
// bound here prevents one producer (or many concurrent producers) from
// exhausting the host process before backpressure at the task queue can help.
inline constexpr size_t ASYNC_MAX_CAPTURE_BYTES = 64u * 1024u * 1024u;
inline constexpr size_t ASYNC_MAX_RETAINED_BYTES = 256u * 1024u * 1024u;
inline constexpr size_t ASYNC_MAX_ERROR_BYTES = 64u * 1024u;

inline bool async_append_bounded(std::string& output, const char* data, size_t size,
                                 size_t max_bytes = ASYNC_MAX_CAPTURE_BYTES) {
    const size_t room = output.size() < max_bytes
        ? max_bytes - output.size()
        : 0;
    const size_t accepted = std::min(room, size);
    if (accepted != 0) output.append(data, accepted);
    return accepted == size;
}

// std::filesystem::is_regular_file() returns false for two different
// situations: the path is not a regular file, and the query itself failed.
// Reporting the first when the second happened sends the reader looking for a
// missing file that is actually present but momentarily unreadable - a sharing
// violation, an antivirus or indexer holding it open, or a cloud-sync recall on
// a OneDrive-backed checkout. Keep the two claims apart.
inline void async_require_regular_file(const std::filesystem::path& path,
                                       const char* what, const char* fn, int line) {
    std::error_code status_error;
    if (std::filesystem::is_regular_file(path, status_error)) return;
    // "It is definitively not there" is an answer, not a failure to answer, so
    // it keeps the plain wording. libstdc++ on Windows reports a missing path
    // through the error_code rather than returning a not_found status, which
    // would otherwise route the most common case into the message meant for
    // genuine inspection failures.
    const bool absent = status_error == std::errc::no_such_file_or_directory ||
                        status_error == std::errc::not_a_directory;
    if (status_error && !absent) {
        throw JitThrow{std::string(fn) + "(): cannot inspect " + what + " (" +
                           status_error.message() + ")",
                       line};
    }
    throw JitThrow{std::string(fn) + "(): " + what + " must name a regular file", line};
}

inline std::string async_read_text_file_cancellable(
        const std::string& path, int line, const char* fn,
        const AsyncCancellationToken& token) {
    token.throw_if_cancelled();
    const std::filesystem::path native_path = fs_path_from_utf8(path);
    async_require_regular_file(native_path, "file URL", fn, line);
    std::error_code size_error;
    const uintmax_t initial_size = std::filesystem::file_size(native_path, size_error);
    if (!size_error && initial_size > ASYNC_MAX_CAPTURE_BYTES) {
        throw JitThrow{std::string(fn) + "(): file exceeds 64 MiB async result limit", line};
    }

    std::ifstream in(native_path, std::ios::binary);
    if (!in) throw JitThrow{std::string(fn) + "(): cannot open '" + path + "'", line};
    std::string output;
    if (!size_error) output.reserve((size_t)initial_size);
    std::array<char, 64 * 1024> buffer{};
    while (in) {
        token.throw_if_cancelled();
        in.read(buffer.data(), (std::streamsize)buffer.size());
        const std::streamsize got = in.gcount();
        if (got > 0 && !async_append_bounded(output, buffer.data(), (size_t)got)) {
            throw JitThrow{std::string(fn) + "(): file exceeds 64 MiB async result limit", line};
        }
    }
    if (!in.eof()) throw JitThrow{std::string(fn) + "(): cannot read '" + path + "'", line};
    token.throw_if_cancelled();
    return output;
}

// Unlike popen(), this runner can terminate an async child process when its
// cancellation token is requested.  It is used only by the async module; the
// synchronous command API retains its historical behavior.
inline CommandRunResult run_capture_command_cancellable_status(
        const std::string& command, const AsyncCancellationToken& token,
        size_t max_capture_bytes = ASYNC_MAX_CAPTURE_BYTES,
        long long timeout_ms = -1, bool require_process_tree = false) {
    token.throw_if_cancelled();
    CommandRunResult result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        throw std::runtime_error("cannot create async command pipe");
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring command_line = L"cmd.exe /D /S /C " + windows_path_bytes_to_wide(command);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                  flags, nullptr, nullptr, &startup, &process);
    CloseHandle(write_pipe);
    write_pipe = nullptr;
    if (!created) {
        CloseHandle(read_pipe);
        if (job) CloseHandle(job);
        throw std::runtime_error("cannot start async command");
    }

    bool assigned_to_job = job && AssignProcessToJobObject(job, process.hProcess);
    if (!assigned_to_job && job) {
        CloseHandle(job);
        job = nullptr;
    }
    if (require_process_tree && !assigned_to_job) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(read_pipe);
        throw std::runtime_error("cannot establish async subprocess job isolation");
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(read_pipe);
        if (job) CloseHandle(job);
        throw std::runtime_error("cannot resume async command");
    }
    CloseHandle(process.hThread);

    bool output_limit_exceeded = false;
    // A producer can keep a pipe perpetually readable. Bound each drain pass
    // so cancellation and the capture limit are observed by the outer loop.
    static constexpr size_t DRAIN_BUDGET_BYTES = 256 * 1024;
    auto drain_pipe = [&]() -> size_t {
        char buffer[4096];
        size_t drained = 0;
        while (drained < DRAIN_BUDGET_BYTES &&
               !output_limit_exceeded && !token.stop_requested()) {
            DWORD available = 0;
            if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            DWORD read = 0;
            DWORD wanted = std::min<DWORD>(available, (DWORD)sizeof(buffer));
            wanted = std::min<DWORD>(wanted,
                static_cast<DWORD>(DRAIN_BUDGET_BYTES - drained));
            if (!ReadFile(read_pipe, buffer, wanted, &read, nullptr) || read == 0) break;
            drained += static_cast<size_t>(read);
            if (!async_append_bounded(result.output, buffer, (size_t)read, max_capture_bytes)) {
                output_limit_exceeded = true;
                break;
            }
        }
        return drained;
    };

    bool cancelled = false;
    bool timed_out = false;
    bool killed_for_output_limit = false;
    const auto started = std::chrono::steady_clock::now();
    while (true) {
        drain_pipe();
        if (token.stop_requested()) {
            cancelled = true;
            if (assigned_to_job) TerminateJobObject(job, 1);
            else TerminateProcess(process.hProcess, 1);
        }
        if (!timed_out && timeout_ms >= 0 &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(timeout_ms)) {
            timed_out = true;
            if (assigned_to_job) TerminateJobObject(job, 1);
            else TerminateProcess(process.hProcess, 1);
        }
        if (output_limit_exceeded && !killed_for_output_limit) {
            killed_for_output_limit = true;
            if (assigned_to_job) TerminateJobObject(job, 1);
            else TerminateProcess(process.hProcess, 1);
        }
        DWORD wait = WaitForSingleObject(process.hProcess, 10);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            if (assigned_to_job) TerminateJobObject(job, 1);
            else TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    while (!output_limit_exceeded && !token.stop_requested() && drain_pipe() != 0) {}
    DWORD exit_code = (DWORD)-1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = (int)exit_code;
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    if (job) CloseHandle(job);
    if (cancelled || token.stop_requested()) throw AsyncCancelled{};
    if (timed_out) {
        throw std::runtime_error("async subprocess timed out after " +
                                 std::to_string(timeout_ms) + " ms");
    }
    if (output_limit_exceeded) {
        throw std::runtime_error("async command output exceeded " +
                                 std::to_string(max_capture_bytes) + " byte limit");
    }
#else
    int pipes[2];
    if (pipe(pipes) != 0) throw std::runtime_error("cannot create async command pipe");
    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        throw std::runtime_error("cannot fork async command");
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipes[1]);
    fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL, 0) | O_NONBLOCK);
    const int group_result = setpgid(pid, pid);
    if (require_process_tree && group_result != 0 && getpgid(pid) != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        close(pipes[0]);
        throw std::runtime_error("cannot establish async subprocess process group");
    }

    bool cancelled = false;
    bool timed_out = false;
    bool output_limit_exceeded = false;
    bool killed_for_output_limit = false;
    int status = -1;
    const auto started = std::chrono::steady_clock::now();
    char buffer[4096];
    static constexpr size_t DRAIN_BUDGET_BYTES = 256 * 1024;
    auto drain_pipe = [&]() -> size_t {
        size_t drained = 0;
        while (drained < DRAIN_BUDGET_BYTES &&
               !output_limit_exceeded && !token.stop_requested()) {
            const size_t wanted = std::min(sizeof(buffer), DRAIN_BUDGET_BYTES - drained);
            ssize_t got = read(pipes[0], buffer, wanted);
            if (got > 0) {
                drained += static_cast<size_t>(got);
                if (!async_append_bounded(result.output, buffer, (size_t)got, max_capture_bytes)) {
                    output_limit_exceeded = true;
                    break;
                }
            }
            else if (got < 0 && errno == EINTR) continue;
            else break;
        }
        return drained;
    };
    while (true) {
        drain_pipe();
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) break;
        if (token.stop_requested()) {
            cancelled = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        if (!timed_out && timeout_ms >= 0 &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(timeout_ms)) {
            timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        if (output_limit_exceeded && !killed_for_output_limit) {
            killed_for_output_limit = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (!output_limit_exceeded && !token.stop_requested() && drain_pipe() != 0) {}
    close(pipes[0]);
    result.exit_code = command_exit_code_from_status(status);
    if (cancelled || token.stop_requested()) throw AsyncCancelled{};
    if (timed_out) {
        throw std::runtime_error("async subprocess timed out after " +
                                 std::to_string(timeout_ms) + " ms");
    }
    if (output_limit_exceeded) {
        throw std::runtime_error("async command output exceeded " +
                                 std::to_string(max_capture_bytes) + " byte limit");
    }
#endif
    return result;
}

inline std::string run_capture_command_cancellable(
        const std::string& command, const AsyncCancellationToken& token,
        size_t max_capture_bytes = ASYNC_MAX_CAPTURE_BYTES,
        long long timeout_ms = -1, bool require_process_tree = false) {
    return run_capture_command_cancellable_status(
        command, token, max_capture_bytes, timeout_ms, require_process_tree).output;
}

inline std::string checked_command_text(const char* fn, const Value& value, int idx, int line) {
    std::string command = need_str(fn, value, idx, line);
    if (command.empty()) throw JitThrow{std::string(fn) + "(): command must not be empty", line};
    if (command.find_first_of("\r\n") != std::string::npos) {
        throw JitThrow{std::string(fn) + "(): command contains unsupported newline characters", line};
    }
    return command;
}

inline Value cmd_run_result_value(const std::string& command, const CommandRunResult& result) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["command"] = Value(command);
    d->elements["exit_code"] = Value((double)result.exit_code);
    d->elements["ok"] = Value(result.exit_code == 0);
    d->elements["output"] = Value(result.output);
    return out;
}

inline Value b_cmd_run(const Value* a, int n, int l) {
    need_args("cmd_run", n, 1, 1, l);
    std::string command = checked_command_text("cmd_run", a[0], 0, l);
    CommandRunResult result = run_capture_command_status(command + " 2>&1");
    return cmd_run_result_value(command, result);
}

inline Value b_cmd_run_checked(const Value* a, int n, int l) {
    need_args("cmd_run_checked", n, 1, 1, l);
    std::string command = checked_command_text("cmd_run_checked", a[0], 0, l);
    CommandRunResult result = run_capture_command_status(command + " 2>&1");
    if (result.exit_code != 0) {
        std::string msg = "cmd_run_checked(): command failed with exit code " +
                          std::to_string(result.exit_code);
        if (!result.output.empty()) msg += ": " + result.output;
        throw JitThrow{msg, l};
    }
    return cmd_run_result_value(command, result);
}

inline std::string http_get_text(const std::string& url, int l, const char* fn) {
    if (url.rfind("file://", 0) == 0) {
        return read_text_file(url.substr(7), l);
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        throw JitThrow{std::string(fn) + "(): URL must start with http://, https://, or file://", l};
    }
    if (url.find_first_of("\"\r\n") != std::string::npos) {
        throw JitThrow{std::string(fn) + "(): URL contains unsupported characters", l};
    }
    std::string cmd = "curl -L -s --max-time 15 -- \"" + url + "\"";
    return run_capture_command(cmd);
}

inline Value b_http_get(const Value* a, int n, int l) {
    need_args("http_get", n, 1, 1, l);
    return Value(http_get_text(need_str("http_get", a[0], 0, l), l, "http_get"));
}

enum class AsyncTaskState { Queued, Running, Succeeded, Failed, Cancelled };

inline bool async_state_terminal(AsyncTaskState state) {
    return state == AsyncTaskState::Succeeded || state == AsyncTaskState::Failed ||
           state == AsyncTaskState::Cancelled;
}

inline const char* async_state_name(AsyncTaskState state) {
    switch (state) {
        case AsyncTaskState::Queued: return "queued";
        case AsyncTaskState::Running: return "running";
        case AsyncTaskState::Succeeded: return "succeeded";
        case AsyncTaskState::Failed: return "failed";
        case AsyncTaskState::Cancelled: return "cancelled";
    }
    return "unknown";
}

struct AsyncTask;
using AsyncTaskQueue = std::list<std::shared_ptr<AsyncTask>>;

struct AsyncTask {
    int id = 0;
    // Scope attachment is changed under AsyncRuntime::mutex_, while status
    // readers deliberately do not take that global lock.  Keep the relation
    // atomic so attach/status/cleanup are data-race-free without introducing a
    // task->runtime lock inversion.
    std::atomic<int> scope_id{0};
    std::shared_ptr<AsyncCancellationState> cancellation =
        std::make_shared<AsyncCancellationState>();
    std::function<std::string(const AsyncCancellationToken&)> work;
    mutable std::mutex mutex;
    std::condition_variable cv;
    AsyncTaskState state = AsyncTaskState::Queued;
    // Accessed only under AsyncRuntime::mutex_. A stable list iterator makes
    // queued cancellation O(1), so cancelling a large scope is linear rather
    // than repeatedly scanning the whole queue.
    AsyncTaskQueue::iterator queue_position;
    bool enqueued = false;
    std::string output;
    std::string error;
    int error_line = 0;
};

struct AsyncScope {
    int id = 0;
    bool open = true;
    std::unordered_set<int> task_ids;
};

struct AsyncScopeCloseSummary {
    int id = 0;
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    int first_failed_task = 0;
    std::string first_error;
};

class AsyncRuntime {
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable completion_cv_;
    AsyncTaskQueue queue_;
    std::unordered_map<int, std::shared_ptr<AsyncTask>> tasks_;
    std::unordered_map<int, AsyncScope> scopes_;
    std::vector<std::thread> workers_;
    // IDs are allocated while mutex_ is held.  A wide monotonic counter lets
    // us reject exhaustion instead of wrapping an int and aliasing a live or
    // previously observed handle.
    uint64_t next_task_id_ = 1;
    uint64_t next_scope_id_ = 1;
    size_t max_workers_ = 4;
    size_t max_queue_ = 1024;
    size_t running_ = 0;
    size_t retained_output_bytes_ = 0;
    size_t max_retained_output_bytes_ = ASYNC_MAX_RETAINED_BYTES;
    uint64_t completion_epoch_ = 0;
    bool workers_started_ = false;
    bool stopping_ = false;
    bool reconfiguring_ = false;

    static size_t default_worker_count() {
        size_t hardware = (size_t)std::thread::hardware_concurrency();
        if (hardware == 0) hardware = 4;
        return std::max<size_t>(1, std::min<size_t>(hardware, 8));
    }

    void purge_terminal_queue_locked() {
        for (auto it = queue_.begin(); it != queue_.end();) {
            const auto& task = *it;
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if (!async_state_terminal(task->state)) {
                ++it;
                continue;
            }
            task->enqueued = false;
            it = queue_.erase(it);
        }
    }

    void start_workers_locked(std::unique_lock<std::mutex>& lock,
                              const char* fn, int line) {
        if (workers_started_) return;
        std::vector<std::thread> pending;
        try {
            pending.reserve(max_workers_);
            for (size_t i = 0; i < max_workers_; ++i) {
                pending.emplace_back([this]() { worker_loop(); });
            }
        } catch (...) {
            // Threads already created in this attempt are blocked on mutex_.
            // Publish a temporary stop state, release the lock, and join them
            // before reporting failure.  No half-started pool escapes.
            const std::exception_ptr failure = std::current_exception();
            stopping_ = true;
            reconfiguring_ = true;
            lock.unlock();
            work_cv_.notify_all();
            for (auto& worker : pending) if (worker.joinable()) worker.join();
            lock.lock();
            stopping_ = false;
            reconfiguring_ = false;
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                throw JitThrow{std::string(fn) + "(): cannot start async workers: " + error.what(), line};
            } catch (...) {
                throw JitThrow{std::string(fn) + "(): cannot start async workers", line};
            }
        }
        workers_.swap(pending);
        workers_started_ = true;
    }

    void detach_task_from_scope_locked(const std::shared_ptr<AsyncTask>& task) {
        const int scope_id = task->scope_id.load(std::memory_order_acquire);
        if (scope_id == 0) return;
        auto scope_it = scopes_.find(scope_id);
        if (scope_it != scopes_.end()) scope_it->second.task_ids.erase(task->id);
    }

    void release_task_output_locked(const std::shared_ptr<AsyncTask>& task) {
        const size_t bytes = task->output.size();
        retained_output_bytes_ = bytes <= retained_output_bytes_
            ? retained_output_bytes_ - bytes
            : 0;
    }

    void worker_loop() {
        while (true) {
            std::shared_ptr<AsyncTask> task;
            std::function<std::string(const AsyncCancellationToken&)> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [&]() { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = queue_.front();
                task->enqueued = false;
                queue_.pop_front();

                std::lock_guard<std::mutex> task_lock(task->mutex);
                if (async_state_terminal(task->state)) continue;
                if (task->cancellation->requested.load(std::memory_order_acquire)) {
                    task->state = AsyncTaskState::Cancelled;
                    ++completion_epoch_;
                    task->cv.notify_all();
                    completion_cv_.notify_all();
                    continue;
                }
                task->state = AsyncTaskState::Running;
                work = std::move(task->work);
                ++running_;
            }

            AsyncTaskState final_state = AsyncTaskState::Succeeded;
            std::string output;
            std::string error;
            int error_line = 0;
            try {
                AsyncCancellationToken token(task->cancellation);
                token.throw_if_cancelled();
                output = work(token);
                token.throw_if_cancelled();
            } catch (const AsyncCancelled&) {
                final_state = AsyncTaskState::Cancelled;
            } catch (const JitThrow& thrown) {
                final_state = AsyncTaskState::Failed;
                error = thrown.message;
                error_line = thrown.line;
            } catch (const std::exception& ex) {
                final_state = AsyncTaskState::Failed;
                error = ex.what();
            } catch (...) {
                final_state = AsyncTaskState::Failed;
                error = "unknown native exception";
            }

            {
                // Publish the result, terminal state, and running count as one
                // linearizable transition.  Every path that needs both locks
                // follows runtime -> task ordering.
                std::lock_guard<std::mutex> lock(mutex_);
                std::lock_guard<std::mutex> task_lock(task->mutex);
                if (task->cancellation->requested.load(std::memory_order_acquire)) {
                    final_state = AsyncTaskState::Cancelled;
                    output.clear();
                    error.clear();
                } else if (final_state == AsyncTaskState::Succeeded &&
                           output.size() > max_retained_output_bytes_ - retained_output_bytes_) {
                    final_state = AsyncTaskState::Failed;
                    output.clear();
                    error = "async retained output capacity exhausted; consume completed tasks";
                    error_line = 0;
                }
                if (error.size() > ASYNC_MAX_ERROR_BYTES) {
                    static constexpr const char* suffix = "... [truncated]";
                    static constexpr size_t suffix_size = 15;
                    error.resize(ASYNC_MAX_ERROR_BYTES - suffix_size);
                    error += suffix;
                }
                if (final_state == AsyncTaskState::Succeeded) {
                    retained_output_bytes_ += output.size();
                }
                task->output = std::move(output);
                task->error = std::move(error);
                task->error_line = error_line;
                task->state = final_state;
                if (running_ > 0) --running_;
                ++completion_epoch_;
            }
            task->cv.notify_all();
            completion_cv_.notify_all();
        }
    }

public:
    explicit AsyncRuntime(size_t max_retained_output_bytes = ASYNC_MAX_RETAINED_BYTES)
        : max_workers_(default_worker_count()),
          max_retained_output_bytes_(std::max<size_t>(1, max_retained_output_bytes)) {}

    ~AsyncRuntime() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (const auto& [_, task] : tasks_) {
                std::lock_guard<std::mutex> task_lock(task->mutex);
                async_request_stop(task->cancellation);
                if (task->state == AsyncTaskState::Queued) {
                    task->state = AsyncTaskState::Cancelled;
                    task->enqueued = false;
                    ++completion_epoch_;
                    task->cv.notify_all();
                }
            }
            // Do not make workers walk a potentially very large cancelled
            // backlog during teardown. Running work receives the same stop
            // request below and is joined normally.
            queue_.clear();
        }
        work_cv_.notify_all();
        completion_cv_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    bool configure(size_t workers, size_t queue_capacity) {
        if (workers == 0 || queue_capacity == 0) return false;
        std::vector<std::thread> old_workers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reconfiguring_) return false;
            if (!tasks_.empty() || !scopes_.empty() || !queue_.empty() || running_ != 0) return false;
            if (!workers_started_) {
                max_workers_ = workers;
                max_queue_ = queue_capacity;
                return true;
            }
            // A quiescent runtime can be reconfigured between independent
            // scripts in the same VM process.  `stopping_` also rejects a
            // concurrent submit during the short worker restart window.
            reconfiguring_ = true;
            stopping_ = true;
            old_workers.swap(workers_);
        }
        work_cv_.notify_all();
        for (auto& worker : old_workers) if (worker.joinable()) worker.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            max_workers_ = workers;
            max_queue_ = queue_capacity;
            workers_started_ = false;
            stopping_ = false;
            reconfiguring_ = false;
        }
        return true;
    }

    int submit(std::function<std::string(const AsyncCancellationToken&)> work,
               int scope_id, const char* fn, int line) {
        auto task = std::make_shared<AsyncTask>();
        task->work = std::move(work);
        task->scope_id.store(scope_id, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) throw JitThrow{std::string(fn) + "(): async runtime is stopping", line};
        if (scope_id != 0) {
            auto scope_it = scopes_.find(scope_id);
            if (scope_it == scopes_.end()) {
                throw JitThrow{std::string(fn) + "(): unknown scope id", line};
            }
            if (!scope_it->second.open) {
                throw JitThrow{std::string(fn) + "(): scope is closed", line};
            }
        }
        const size_t max_tracked = max_workers_ + max_queue_;
        if (queue_.size() >= max_queue_ || tasks_.size() >= max_tracked) {
            throw JitThrow{std::string(fn) + "(): async capacity exhausted; await or cleanup tasks", line};
        }
        start_workers_locked(lock, fn, line);
        if (next_task_id_ > (uint64_t)std::numeric_limits<int>::max()) {
            throw JitThrow{std::string(fn) + "(): async task id space exhausted", line};
        }
        task->id = (int)next_task_id_++;
        tasks_.emplace(task->id, task);
        bool scope_attached = false;
        try {
            if (scope_id != 0) {
                scopes_.at(scope_id).task_ids.insert(task->id);
                scope_attached = true;
            }
            queue_.push_back(task);
            task->queue_position = std::prev(queue_.end());
            task->enqueued = true;
        } catch (...) {
            if (scope_attached) scopes_.at(scope_id).task_ids.erase(task->id);
            tasks_.erase(task->id);
            throw;
        }
        work_cv_.notify_one();
        return task->id;
    }

    std::shared_ptr<AsyncTask> find(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        return it == tasks_.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<AsyncTask>> snapshot_tasks() const {
        std::vector<std::shared_ptr<AsyncTask>> out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(tasks_.size());
        for (const auto& [_, task] : tasks_) out.push_back(task);
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
            return left->id < right->id;
        });
        return out;
    }

    bool cancel(int id) {
        std::shared_ptr<AsyncTask> task;
        std::function<std::string(const AsyncCancellationToken&)> released_work;
        bool became_terminal = false;
        bool first_request = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tasks_.find(id);
            if (it == tasks_.end()) return false;
            task = it->second;
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if (async_state_terminal(task->state)) return false;
            first_request = async_request_stop(task->cancellation);
            if (task->state == AsyncTaskState::Queued) {
                task->state = AsyncTaskState::Cancelled;
                released_work = std::move(task->work);
                if (task->enqueued) {
                    queue_.erase(task->queue_position);
                    task->enqueued = false;
                }
                ++completion_epoch_;
                became_terminal = true;
            }
        }
        if (became_terminal) {
            task->cv.notify_all();
            completion_cv_.notify_all();
        }
        return first_request;
    }

    uint64_t completion_epoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completion_epoch_;
    }

    // Wait for any task transition without polling.  Taking an epoch before a
    // caller scans its task set closes the notify-before-wait window: a
    // completion between the scan and this call changes the predicate.
    bool wait_for_completion(uint64_t observed_epoch, long long timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto changed = [&]() {
            return completion_epoch_ != observed_epoch || stopping_;
        };
        if (changed()) return true;
        if (timeout_ms < 0) {
            completion_cv_.wait(lock, changed);
            return true;
        }
        return completion_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), changed);
    }

    bool wait(const std::shared_ptr<AsyncTask>& task, long long timeout_ms) const {
        std::unique_lock<std::mutex> lock(task->mutex);
        auto ready = [&]() { return async_state_terminal(task->state); };
        if (timeout_ms < 0) {
            task->cv.wait(lock, ready);
            return true;
        }
        return task->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    }

    bool erase_terminal(int id, const std::shared_ptr<AsyncTask>& expected) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || it->second != expected) return false;
        {
            std::lock_guard<std::mutex> task_lock(expected->mutex);
            if (!async_state_terminal(expected->state)) return false;
            release_task_output_locked(expected);
        }
        detach_task_from_scope_locked(expected);
        tasks_.erase(it);
        return true;
    }

    bool erase_terminal_batch(const std::vector<std::shared_ptr<AsyncTask>>& expected) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& task : expected) {
            auto it = tasks_.find(task->id);
            if (it == tasks_.end() || it->second != task) return false;
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if (!async_state_terminal(task->state)) return false;
        }
        for (const auto& task : expected) {
            auto it = tasks_.find(task->id);
            {
                std::lock_guard<std::mutex> task_lock(task->mutex);
                release_task_output_locked(task);
            }
            detach_task_from_scope_locked(task);
            tasks_.erase(it);
        }
        return true;
    }

    int cleanup() {
        int removed = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            bool terminal = false;
            {
                std::lock_guard<std::mutex> task_lock(it->second->mutex);
                terminal = async_state_terminal(it->second->state);
            }
            if (!terminal) {
                ++it;
                continue;
            }
            release_task_output_locked(it->second);
            detach_task_from_scope_locked(it->second);
            it = tasks_.erase(it);
            ++removed;
        }
        purge_terminal_queue_locked();
        return removed;
    }

    int open_scope(const char* fn = "async_scope_open", int line = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) throw JitThrow{std::string(fn) + "(): async runtime is stopping", line};
        if (next_scope_id_ > (uint64_t)std::numeric_limits<int>::max()) {
            throw JitThrow{std::string(fn) + "(): async scope id space exhausted", line};
        }
        int id = (int)next_scope_id_++;
        AsyncScope scope;
        scope.id = id;
        scopes_.emplace(id, std::move(scope));
        return id;
    }

    bool attach(int scope_id, int task_id, const char* fn, int line) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto scope_it = scopes_.find(scope_id);
        if (scope_it == scopes_.end()) throw JitThrow{std::string(fn) + "(): unknown scope id", line};
        if (!scope_it->second.open) throw JitThrow{std::string(fn) + "(): scope is closed", line};
        auto task_it = tasks_.find(task_id);
        if (task_it == tasks_.end()) throw JitThrow{std::string(fn) + "(): unknown task id", line};
        const int old_scope = task_it->second->scope_id.load(std::memory_order_acquire);
        if (old_scope != 0 && old_scope != scope_id) {
            throw JitThrow{std::string(fn) + "(): task already belongs to another scope", line};
        }
        if (old_scope == scope_id) return false;
        task_it->second->scope_id.store(scope_id, std::memory_order_release);
        scope_it->second.task_ids.insert(task_id);
        return true;
    }

    bool scope_snapshot(int scope_id, bool& open,
                        std::vector<std::shared_ptr<AsyncTask>>& tasks) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = scopes_.find(scope_id);
        if (it == scopes_.end()) return false;
        open = it->second.open;
        tasks.reserve(it->second.task_ids.size());
        for (int id : it->second.task_ids) {
            auto task_it = tasks_.find(id);
            if (task_it != tasks_.end()) tasks.push_back(task_it->second);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            return left->id < right->id;
        });
        return true;
    }

    int cancel_scope(int scope_id, const char* fn, int line) {
        std::vector<std::shared_ptr<AsyncTask>> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto scope = scopes_.find(scope_id);
            if (scope == scopes_.end()) {
                throw JitThrow{std::string(fn) + "(): unknown scope id", line};
            }
            // Cancellation is persistent for a structured scope. Closing the
            // admission gate under the same lock as submit prevents a child
            // from slipping in after the cancellation snapshot.
            scope->second.open = false;
            tasks.reserve(scope->second.task_ids.size());
            for (int id : scope->second.task_ids) {
                auto task = tasks_.find(id);
                if (task != tasks_.end()) tasks.push_back(task->second);
            }
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            return left->id < right->id;
        });
        int requested = 0;
        for (const auto& task : tasks) if (cancel(task->id)) ++requested;
        return requested;
    }

    bool close_scope(int scope_id, bool cancel_remaining, long long timeout_ms,
                     AsyncScopeCloseSummary& summary, bool& known) {
        std::vector<std::shared_ptr<AsyncTask>> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = scopes_.find(scope_id);
            if (it == scopes_.end()) {
                known = false;
                return false;
            }
            known = true;
            it->second.open = false;
            tasks.reserve(it->second.task_ids.size());
            for (int id : it->second.task_ids) {
                auto task_it = tasks_.find(id);
                if (task_it != tasks_.end()) tasks.push_back(task_it->second);
            }
        }
        if (cancel_remaining) {
            for (const auto& task : tasks) cancel(task->id);
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(std::max<long long>(0, timeout_ms));
        for (const auto& task : tasks) {
            long long remaining = -1;
            if (timeout_ms >= 0) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline) remaining = 0;
                else remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            }
            if (!wait(task, remaining)) return false;
        }

        summary.id = scope_id;
        summary.total = (int)tasks.size();
        for (const auto& task : tasks) {
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if (task->state == AsyncTaskState::Succeeded) ++summary.succeeded;
            else if (task->state == AsyncTaskState::Cancelled) ++summary.cancelled;
            else if (task->state == AsyncTaskState::Failed) {
                ++summary.failed;
                if (summary.first_failed_task == 0) {
                    summary.first_failed_task = task->id;
                    summary.first_error = task->error;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& task : tasks) {
                auto it = tasks_.find(task->id);
                if (it != tasks_.end() && it->second == task) {
                    std::lock_guard<std::mutex> task_lock(task->mutex);
                    release_task_output_locked(task);
                    tasks_.erase(it);
                }
            }
            scopes_.erase(scope_id);
            purge_terminal_queue_locked();
        }
        return true;
    }

    void limits(size_t& workers, size_t& queue_capacity, size_t& queued,
                size_t& running, size_t& tracked, size_t& scopes,
                size_t* max_retained = nullptr, size_t* retained = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        workers = max_workers_;
        queue_capacity = max_queue_;
        queued = queue_.size();
        running = running_;
        tracked = tasks_.size();
        scopes = scopes_.size();
        if (max_retained) *max_retained = max_retained_output_bytes_;
        if (retained) *retained = retained_output_bytes_;
    }
};

inline AsyncRuntime& async_runtime() {
    static AsyncRuntime runtime;
    return runtime;
}

inline int async_id_arg(const char* fn, const Value& value, int idx, int line) {
    double raw = need_num(fn, value, idx, line);
    if (!std::isfinite(raw) || raw != std::floor(raw) ||
        raw < (double)std::numeric_limits<int>::min() ||
        raw > (double)std::numeric_limits<int>::max()) {
        throw JitThrow{std::string(fn) + "(): arg " + std::to_string(idx + 1) +
                       " must be an integer id", line};
    }
    return (int)raw;
}

inline int async_optional_scope_arg(const char* fn, const Value* a, int n, int idx, int line) {
    return n > idx ? async_id_arg(fn, a[idx], idx, line) : 0;
}

inline Value async_store_task(
        std::function<std::string(const AsyncCancellationToken&)> work,
        int scope_id, const char* fn, int line) {
    return Value((double)async_runtime().submit(std::move(work), scope_id, fn, line));
}

inline Value b_async_cmd(const Value* a, int n, int l) {
    need_args("async_cmd", n, 1, 2, l);
    std::string command = checked_command_text("async_cmd", a[0], 0, l);
    int scope_id = async_optional_scope_arg("async_cmd", a, n, 1, l);
    return async_store_task([command](const AsyncCancellationToken& token) {
        return run_capture_command_cancellable(command, token);
    }, scope_id, "async_cmd", l);
}

inline Value b_async_http_get(const Value* a, int n, int l) {
    need_args("async_http_get", n, 1, 2, l);
    std::string url = need_str("async_http_get", a[0], 0, l);
    int scope_id = async_optional_scope_arg("async_http_get", a, n, 1, l);
    return async_store_task([url, l](const AsyncCancellationToken& token) {
        token.throw_if_cancelled();
        if (url.rfind("file://", 0) == 0) {
            return async_read_text_file_cancellable(
                url.substr(7), l, "async_http_get", token);
        }
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            throw JitThrow{"async_http_get(): URL must start with http://, https://, or file://", l};
        }
        if (url.find_first_of("\"\r\n") != std::string::npos) {
            throw JitThrow{"async_http_get(): URL contains unsupported characters", l};
        }
        return run_capture_command_cancellable(
            "curl -L -s --max-time 15 -- \"" + url + "\"", token);
    }, scope_id, "async_http_get", l);
}

struct AsyncHttpRequestPlan {
    std::string url;
    std::string method;
    int timeout = 20;
    std::vector<std::pair<std::string, std::string>> headers;
    bool has_body = false;
    std::string body;
};

// Create request-body files with O_EXCL/CREATE_NEW semantics. Timestamp-only
// names can collide when multiple worker threads prepare requests in the same
// clock tick, which used to let one request truncate or delete another's body.
inline std::filesystem::path async_write_request_body_temp(const std::string& body, int line) {
    static std::atomic<uint64_t> sequence{1};
    const auto temp = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 128; ++attempt) {
        const uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const unsigned long process_id = (unsigned long)GetCurrentProcessId();
#else
        const unsigned long process_id = (unsigned long)getpid();
#endif
        std::filesystem::path path = temp /
            ("sura_async_http_request_" + std::to_string(process_id) + "_" +
             std::to_string(id) + ".body");

#ifdef _WIN32
        int fd = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                        _S_IREAD | _S_IWRITE);
#else
        int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd < 0) {
            if (errno == EEXIST) continue;
            throw JitThrow{"async_http_request(): cannot create temporary body file", line};
        }

        bool ok = true;
        size_t offset = 0;
        while (offset < body.size()) {
#ifdef _WIN32
            const unsigned int chunk = (unsigned int)std::min<size_t>(
                body.size() - offset, (size_t)std::numeric_limits<int>::max());
            int written = _write(fd, body.data() + offset, chunk);
#else
            ssize_t written = ::write(fd, body.data() + offset, body.size() - offset);
#endif
            if (written > 0) {
                offset += (size_t)written;
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            ok = false;
            break;
        }
#ifdef _WIN32
        if (_close(fd) != 0) ok = false;
#else
        if (::close(fd) != 0) ok = false;
#endif
        if (ok) return path;

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw JitThrow{"async_http_request(): cannot write temporary body file", line};
    }
    throw JitThrow{"async_http_request(): cannot allocate unique temporary body file", line};
}

inline constexpr size_t ASYNC_SURA_MAX_PROGRAM_BYTES = 64u * 1024u * 1024u;
inline constexpr size_t ASYNC_SURA_MAX_JSON_BYTES = 64u * 1024u * 1024u;
inline constexpr size_t ASYNC_SURA_MAX_JSON_NODES = 1000000u;
inline constexpr size_t ASYNC_SURA_MAX_JSON_DEPTH = 128u;

inline std::filesystem::path async_write_snapshot_temp(
        const std::string& bytes, const char* suffix, int line) {
    static std::atomic<uint64_t> sequence{1};
    const auto temp = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 128; ++attempt) {
        const uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const unsigned long process_id = (unsigned long)GetCurrentProcessId();
#else
        const unsigned long process_id = (unsigned long)getpid();
#endif
        std::filesystem::path path = temp /
            ("sura_async_isolate_" + std::to_string(process_id) + "_" +
             std::to_string(id) + suffix);
#ifdef _WIN32
        int fd = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                        _S_IREAD | _S_IWRITE);
#else
        int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd < 0) {
            if (errno == EEXIST) continue;
            throw JitThrow{"async_sura(): cannot create isolated snapshot file", line};
        }

        bool ok = true;
        size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            const unsigned int chunk = (unsigned int)std::min<size_t>(
                bytes.size() - offset, (size_t)std::numeric_limits<int>::max());
            int written = _write(fd, bytes.data() + offset, chunk);
#else
            ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
#endif
            if (written > 0) {
                offset += (size_t)written;
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            ok = false;
            break;
        }
#ifdef _WIN32
        if (_close(fd) != 0) ok = false;
#else
        if (::close(fd) != 0) ok = false;
#endif
        if (ok) return path;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw JitThrow{"async_sura(): cannot write isolated snapshot file", line};
    }
    throw JitThrow{"async_sura(): cannot allocate unique isolated snapshot file", line};
}

inline std::string async_read_program_snapshot(
        const std::filesystem::path& path, int line) {
    async_require_regular_file(path, "program", "async_sura", line);
    std::error_code size_error;
    const uintmax_t size = std::filesystem::file_size(path, size_error);
    // A failed size query is not the same as an oversized program. Saying
    // "exceeds 64 MiB" when the file could not be measured sends the reader
    // looking for a huge file that may be perfectly small.
    if (size_error) {
        throw JitThrow{"async_sura(): cannot measure program (" +
                           size_error.message() + ")",
                       line};
    }
    if (size > ASYNC_SURA_MAX_PROGRAM_BYTES) {
        throw JitThrow{"async_sura(): program exceeds 64 MiB snapshot limit", line};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw JitThrow{"async_sura(): cannot open program", line};
    std::string bytes((size_t)size, '\0');
    if (!bytes.empty()) input.read(bytes.data(), (std::streamsize)bytes.size());
    if (!input || input.gcount() != (std::streamsize)bytes.size()) {
        throw JitThrow{"async_sura(): cannot read complete program snapshot", line};
    }
    return bytes;
}

class AsyncJsonSnapshotEncoder {
    std::string output_;
    std::unordered_set<const GCObject*> active_;
    size_t nodes_ = 0;
    int line_ = 0;

    [[noreturn]] void fail(const std::string& reason) const {
        throw JitThrow{"async_sura(): input " + reason, line_};
    }

    void append(const char* text, size_t size) {
        if (!async_append_bounded(output_, text, size, ASYNC_SURA_MAX_JSON_BYTES)) {
            fail("exceeds 64 MiB JSON snapshot limit");
        }
    }

    void append(const std::string& text) { append(text.data(), text.size()); }
    void append(char ch) { append(&ch, 1); }

    void append_string(const std::string& text) {
        append('"');
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned char ch : text) {
            switch (ch) {
                case '"': append("\\\"", 2); break;
                case '\\': append("\\\\", 2); break;
                case '\b': append("\\b", 2); break;
                case '\f': append("\\f", 2); break;
                case '\n': append("\\n", 2); break;
                case '\r': append("\\r", 2); break;
                case '\t': append("\\t", 2); break;
                default:
                    if (ch < 0x20) {
                        char escaped[6] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};
                        append(escaped, sizeof(escaped));
                    } else {
                        const char raw = (char)ch;
                        append(&raw, 1);
                    }
            }
        }
        append('"');
    }

    void encode(const Value& value, size_t depth) {
        if (++nodes_ > ASYNC_SURA_MAX_JSON_NODES) fail("exceeds JSON node limit");
        if (depth > ASYNC_SURA_MAX_JSON_DEPTH) fail("exceeds JSON nesting limit");
        if (value.is_nil()) { append("null", 4); return; }
        if (value.is_bool()) { append(value.as_bool() ? "true" : "false"); return; }
        if (value.is_num()) {
            if (!std::isfinite(value.as_num())) fail("contains a non-finite number");
            append(json_stringify_value(value));
            return;
        }
        if (value.is_str()) { append_string(value.as_str_ref()); return; }
        if (!value.is_arr() && !value.is_dict()) {
            fail("supports only nil, bool, finite number, string, array, and dict values");
        }

        const GCObject* object = value.as_obj();
        if (!active_.insert(object).second) fail("contains a cyclic array or dict");
        if (value.is_arr()) {
            append('[');
            const auto& elements = value.as_arr()->elements;
            for (size_t i = 0; i < elements.size(); ++i) {
                if (i) append(',');
                encode(elements[i], depth + 1);
            }
            append(']');
        } else {
            append('{');
            bool first = true;
            for (const auto& entry : value.as_dict()->elements) {
                if (!first) append(',');
                first = false;
                append_string(entry.first);
                append(':');
                encode(entry.second, depth + 1);
            }
            append('}');
        }
        active_.erase(object);
    }

public:
    explicit AsyncJsonSnapshotEncoder(int line) : line_(line) {
        output_.reserve(1024);
    }

    std::string encode(const Value& value) {
        encode(value, 0);
        return std::move(output_);
    }
};

struct AsyncSuraInvocation {
    std::filesystem::path program_snapshot;
    std::filesystem::path input_snapshot;
    std::atomic<bool> cleaned{false};

    void cleanup() noexcept {
        if (cleaned.exchange(true, std::memory_order_acq_rel)) return;
        std::error_code ignored;
        if (!program_snapshot.empty()) std::filesystem::remove(program_snapshot, ignored);
        ignored.clear();
        if (!input_snapshot.empty()) std::filesystem::remove(input_snapshot, ignored);
    }

    ~AsyncSuraInvocation() { cleanup(); }
};

inline AsyncHttpRequestPlan async_http_request_plan(const Value& spec_value, int l) {
    if (!spec_value.is_dict()) throw JitThrow{"async_http_request(): spec must be a dict", l};
    auto* spec = spec_value.as_dict();

    const Value* url_value = dict_get_ptr(spec, "url");
    if (!url_value || !url_value->is_str()) throw JitThrow{"async_http_request(): spec.url must be a string", l};

    AsyncHttpRequestPlan plan;
    plan.url = url_value->as_str();
    if (plan.url.find_first_of("\"\r\n") != std::string::npos) {
        throw JitThrow{"async_http_request(): URL contains unsupported characters", l};
    }
    plan.url = url_with_query_from_spec(plan.url, spec, "async_http_request", l);

    const Value* json_body = dict_get_ptr(spec, "json");
    const Value* body_value = dict_get_ptr(spec, "body");
    const Value* form_body = dict_get_ptr(spec, "form");
    http_require_single_body_source("async_http_request", body_value, json_body, form_body, l);
    plan.has_body = json_body != nullptr || body_value != nullptr || form_body != nullptr;
    plan.method = plan.has_body ? "POST" : "GET";
    if (const Value* method_value = dict_get_ptr(spec, "method")) plan.method = need_str("async_http_request", *method_value, 0, l);
    plan.method = http_upper_method(plan.method);
    if (!http_method_safe(plan.method)) throw JitThrow{"async_http_request(): method must contain letters only", l};

    if (const Value* timeout_value = dict_get_ptr(spec, "timeout")) {
        if (!timeout_value->is_num()) throw JitThrow{"async_http_request(): timeout must be a number", l};
        plan.timeout = (int)timeout_value->as_num();
        if (plan.timeout <= 0 || plan.timeout > 3600) throw JitThrow{"async_http_request(): timeout must be 1..3600 seconds", l};
    }

    bool has_content_type = false;
    if (const Value* headers_value = dict_get_ptr(spec, "headers")) {
        if (!headers_value->is_dict()) throw JitThrow{"async_http_request(): headers must be a dict", l};
        for (const auto& [name, raw_value] : headers_value->as_dict()->elements) {
            if (!http_header_name_safe(name)) throw JitThrow{"async_http_request(): header name contains unsupported characters", l};
            std::string value = raw_value.to_str();
            if (!http_shell_value_safe(value)) throw JitThrow{"async_http_request(): header value contains unsupported characters", l};
            if (http_header_is_content_type(name)) has_content_type = true;
            plan.headers.push_back({name, value});
        }
    }

    if (json_body) {
        plan.body = json_stringify_value(*json_body);
        if (!has_content_type) plan.headers.push_back({"Content-Type", "application/json"});
    } else if (form_body) {
        if (!form_body->is_dict()) throw JitThrow{"async_http_request(): form must be a dict", l};
        plan.body = form_build_from_dict(form_body->as_dict(), l, "async_http_request");
        if (!has_content_type) {
            std::string content_type = "application/x-www-form-urlencoded";
            if (const Value* ct = dict_get_ptr(spec, "content_type")) content_type = need_str("async_http_request", *ct, 0, l);
            if (!http_shell_value_safe(content_type)) throw JitThrow{"async_http_request(): content_type contains unsupported characters", l};
            plan.headers.push_back({"Content-Type", content_type});
        }
    } else if (body_value) {
        plan.body = body_value->is_str() ? body_value->as_str() : json_stringify_value(*body_value);
        if (!has_content_type) {
            std::string content_type = "application/json";
            if (const Value* ct = dict_get_ptr(spec, "content_type")) content_type = need_str("async_http_request", *ct, 0, l);
            if (!http_shell_value_safe(content_type)) throw JitThrow{"async_http_request(): content_type contains unsupported characters", l};
            plan.headers.push_back({"Content-Type", content_type});
        }
    }

    return plan;
}

inline std::string async_http_request_send(const AsyncHttpRequestPlan& plan, int l,
                                           const AsyncCancellationToken& token) {
    token.throw_if_cancelled();
    if (plan.url.rfind("file://", 0) == 0) {
        if (plan.method != "GET" || plan.has_body) {
            throw JitThrow{"async_http_request(): file:// only supports body-less GET", l};
        }
        return async_read_text_file_cancellable(
            plan.url.substr(7), l, "async_http_request", token);
    }
    if (plan.url.rfind("http://", 0) != 0 && plan.url.rfind("https://", 0) != 0) {
        throw JitThrow{"async_http_request(): URL must start with http://, https://, or file://", l};
    }

    std::filesystem::path tmp;
    try {
        if (plan.has_body) tmp = async_write_request_body_temp(plan.body, l);

        std::string cmd = "curl -L -s --max-time " + std::to_string(plan.timeout) + " -X " + plan.method;
        for (const auto& [name, value] : plan.headers) cmd += " -H \"" + name + ": " + value + "\"";
        if (plan.has_body) cmd += " --data-binary @\"" + fs_path_to_utf8(tmp) + "\"";
        cmd += " -- \"" + plan.url + "\"";

        std::string out = run_capture_command_cancellable(cmd, token);
        if (plan.has_body) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        return out;
    } catch (...) {
        if (plan.has_body) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        throw;
    }
}

inline Value b_async_http_request(const Value* a, int n, int l) {
    need_args("async_http_request", n, 1, 2, l);
    AsyncHttpRequestPlan plan = async_http_request_plan(a[0], l);
    int scope_id = async_optional_scope_arg("async_http_request", a, n, 1, l);
    return async_store_task([plan, l](const AsyncCancellationToken& token) {
        return async_http_request_send(plan, l, token);
    }, scope_id, "async_http_request", l);
}

inline Value b_async_sleep(const Value* a, int n, int l) {
    need_args("async_sleep", n, 1, 2, l);
    double ms = need_num("async_sleep", a[0], 0, l);
    if (!std::isfinite(ms) || ms < 0 || ms > (double)std::numeric_limits<long long>::max()) {
        throw JitThrow{"async_sleep(): duration must be a finite non-negative number", l};
    }
    auto delay = std::chrono::milliseconds((long long)ms);
    int scope_id = async_optional_scope_arg("async_sleep", a, n, 1, l);
    return async_store_task([delay](const AsyncCancellationToken& token) {
        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (true) {
            token.throw_if_cancelled();
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            auto slice = std::min(std::chrono::milliseconds(10), remaining);
            if (slice.count() <= 0) slice = std::chrono::milliseconds(1);
            if (token.wait_for(slice)) throw AsyncCancelled{};
        }
        return std::string();
    }, scope_id, "async_sleep", l);
}

enum class AsyncSuraProgramKind { Source, Bytecode, Release };

inline AsyncSuraProgramKind async_sura_program_kind(
        const std::filesystem::path& path, int line) {
    std::string name = fs_path_to_utf8(path.filename());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    auto ends_with = [&](const char* suffix) {
        const size_t size = std::strlen(suffix);
        return name.size() >= size && name.compare(name.size() - size, size, suffix) == 0;
    };
    if (ends_with(".sura.bc") || ends_with(".bc")) return AsyncSuraProgramKind::Bytecode;
    if (ends_with(".sura.srp") || ends_with(".srp")) return AsyncSuraProgramKind::Release;
    if (ends_with(".sura")) return AsyncSuraProgramKind::Source;
    throw JitThrow{"async_sura(): program must end in .sura, .sura.bc, .bc, .sura.srp, or .srp", line};
}

inline std::filesystem::path async_sura_regular_absolute_path(
        const char* fn, const std::string& raw, int line) {
    if (raw.empty() || raw.find_first_of("\r\n") != std::string::npos) {
        throw JitThrow{std::string(fn) + "(): path is empty or contains a newline", line};
    }
    std::error_code absolute_error;
    std::filesystem::path path = fs_path_from_utf8(raw);
    std::filesystem::path absolute = std::filesystem::absolute(path, absolute_error);
    if (absolute_error) throw JitThrow{std::string(fn) + "(): cannot resolve path", line};
    absolute = absolute.lexically_normal();
    async_require_regular_file(absolute, "path", fn, line);
    return absolute;
}

inline std::filesystem::path async_sura_engine_path(
        const std::string& raw, int line) {
    try {
        return async_sura_regular_absolute_path("async_sura", raw, line);
    } catch (const JitThrow&) {
        const std::string found = find_command_on_path(raw);
        if (found.empty()) throw;
        return async_sura_regular_absolute_path("async_sura", found, line);
    }
}

inline std::string async_sura_command(
        const std::filesystem::path& engine,
        const std::filesystem::path& program,
        const std::filesystem::path& input,
        AsyncSuraProgramKind kind) {
    std::vector<std::string> arguments;
    arguments.push_back(fs_path_to_utf8(engine));
    if (kind == AsyncSuraProgramKind::Bytecode) arguments.push_back("--load");
    else if (kind == AsyncSuraProgramKind::Release) arguments.push_back("--load-release");
    arguments.push_back(fs_path_to_utf8(program));
    arguments.push_back("--");
    arguments.push_back(fs_path_to_utf8(input));

#ifdef _WIN32
    std::string command = "set \"SURA_ASYNC_CHILD=1\"&& ";
#else
    std::string command = "SURA_ASYNC_CHILD=1 ";
#endif
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) command.push_back(' ');
        command += shell_quote_arg(arguments[i]);
    }
    return command;
}

// async.sura is an isolated-program task, not a closure-spawn primitive.
// `input` is snapshotted by value through bounded JSON and is exposed to the
// child as argv()[0], the path of a read-only-by-contract JSON snapshot. The
// child writes its result to stdout; async.await returns those bytes. Closures,
// instances, tensors, native resources, cycles, and shared mutable captures
// are rejected instead of being passed as raw GC pointers. Repeated array/dict
// aliases are serialized by value; JSON does not preserve object identity.
inline Value b_async_sura(const Value* a, int n, int l) {
    need_args("async_sura", n, 1, 2, l);
    if (is_async_isolated_child()) {
        throw JitThrow{"async_sura(): recursive isolated program spawning is disabled", l};
    }
    GCDict* spec = need_dict("async_sura", a[0], 0, l);
    const Value* program_value = dict_get_ptr(spec, "program");
    if (!program_value || !program_value->is_str()) {
        throw JitThrow{"async_sura(): spec.program must be a string", l};
    }
    const Value* input_value = dict_get_ptr(spec, "input");
    const Value nil_input = Value::nil();
    if (!input_value) input_value = &nil_input;

    long long timeout_ms = 30000;
    if (const Value* timeout = dict_get_ptr(spec, "timeout_ms")) {
        if (!timeout->is_num() || !std::isfinite(timeout->as_num()) ||
            timeout->as_num() != std::floor(timeout->as_num()) ||
            timeout->as_num() < 1 || timeout->as_num() > 3600000) {
            throw JitThrow{"async_sura(): spec.timeout_ms must be an integer from 1 to 3600000", l};
        }
        timeout_ms = (long long)timeout->as_num();
    }
    const int scope_id = async_optional_scope_arg("async_sura", a, n, 1, l);

    const std::string engine_raw = runtime_executable();
    if (engine_raw.empty()) {
        throw JitThrow{"async_sura(): runtime executable is unavailable; launch through the Sura CLI or set SURA_EXECUTABLE", l};
    }
    const std::filesystem::path engine = async_sura_engine_path(engine_raw, l);
    const std::filesystem::path program = async_sura_regular_absolute_path(
        "async_sura", program_value->as_str_ref(), l);
    const AsyncSuraProgramKind kind = async_sura_program_kind(program, l);

    std::string input_json = AsyncJsonSnapshotEncoder(l).encode(*input_value);
    std::string program_bytes = async_read_program_snapshot(program, l);
    auto invocation = std::make_shared<AsyncSuraInvocation>();
    const char* program_suffix = kind == AsyncSuraProgramKind::Source ? ".sura"
        : kind == AsyncSuraProgramKind::Bytecode ? ".sura.bc" : ".sura.srp";
    invocation->program_snapshot = async_write_snapshot_temp(program_bytes, program_suffix, l);
    try {
        invocation->input_snapshot = async_write_snapshot_temp(input_json, ".json", l);
    } catch (...) {
        invocation->cleanup();
        throw;
    }
    const std::string command = async_sura_command(
        engine, invocation->program_snapshot, invocation->input_snapshot, kind);

    return async_store_task(
        [invocation, command, timeout_ms, l](const AsyncCancellationToken& token) {
            struct CleanupGuard {
                std::shared_ptr<AsyncSuraInvocation> invocation;
                ~CleanupGuard() { invocation->cleanup(); }
            } cleanup{invocation};
            CommandRunResult result = run_capture_command_cancellable_status(
                command, token, ASYNC_MAX_CAPTURE_BYTES, timeout_ms, true);
            if (result.exit_code != 0) {
                std::string message = "async_sura(): isolated program exited with code " +
                                      std::to_string(result.exit_code);
                if (!result.output.empty()) {
                    static constexpr size_t max_detail = 8192;
                    message += ": " + result.output.substr(0, max_detail);
                    if (result.output.size() > max_detail) message += "... [truncated]";
                }
                throw JitThrow{message, l};
            }
            return result.output;
        }, scope_id, "async_sura", l);
}

inline Value b_async_ready(const Value* a, int n, int l) {
    need_args("async_ready", n, 1, 1, l);
    int id = async_id_arg("async_ready", a[0], 0, l);
    auto task = async_runtime().find(id);
    if (!task) return Value(false);
    std::lock_guard<std::mutex> lock(task->mutex);
    return Value(async_state_terminal(task->state));
}

inline Value async_status_dict(int id, const std::shared_ptr<AsyncTask>& task) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["id"] = Value((double)id);
    d->elements["known"] = Value((bool)task);
    if (!task) {
        d->elements["state"] = Value("unknown");
        d->elements["ready"] = Value(false);
        d->elements["queued"] = Value(false);
        d->elements["running"] = Value(false);
        d->elements["succeeded"] = Value(false);
        d->elements["failed"] = Value(false);
        d->elements["cancelled"] = Value(false);
        d->elements["cancel_requested"] = Value(false);
        d->elements["scope"] = Value(0.0);
        return out;
    }
    std::lock_guard<std::mutex> lock(task->mutex);
    const AsyncTaskState state = task->state;
    d->elements["state"] = Value(async_state_name(state));
    d->elements["ready"] = Value(async_state_terminal(state));
    d->elements["queued"] = Value(state == AsyncTaskState::Queued);
    // Historically `running` meant "not ready". Keep that compatibility and
    // expose the precise queued/running distinction through `state`/`queued`.
    d->elements["running"] = Value(state == AsyncTaskState::Queued || state == AsyncTaskState::Running);
    d->elements["succeeded"] = Value(state == AsyncTaskState::Succeeded);
    d->elements["failed"] = Value(state == AsyncTaskState::Failed);
    d->elements["cancelled"] = Value(state == AsyncTaskState::Cancelled);
    d->elements["cancel_requested"] = Value(
        task->cancellation->requested.load(std::memory_order_acquire));
    d->elements["scope"] = Value((double)task->scope_id.load(std::memory_order_acquire));
    if (state == AsyncTaskState::Failed) d->elements["error"] = Value(task->error);
    return out;
}

inline Value b_async_status(const Value* a, int n, int l) {
    need_args("async_status", n, 1, 1, l);
    int id = async_id_arg("async_status", a[0], 0, l);
    return async_status_dict(id, async_runtime().find(id));
}

inline Value b_async_pending(const Value* a, int n, int l) {
    need_args("async_pending", n, 0, 0, l);
    Value out = Value::make_array();
    for (const auto& task : async_runtime().snapshot_tasks()) {
        out.as_arr()->elements.push_back(async_status_dict(task->id, task));
    }
    return out;
}

inline Value b_async_forget(const Value* a, int n, int l) {
    need_args("async_forget", n, 1, 1, l);
    int id = async_id_arg("async_forget", a[0], 0, l);
    auto task = async_runtime().find(id);
    return Value(task && async_runtime().erase_terminal(id, task));
}

inline Value b_async_cleanup(const Value* a, int n, int l) {
    need_args("async_cleanup", n, 0, 0, l);
    return Value((double)async_runtime().cleanup());
}

inline Value b_async_cancel(const Value* a, int n, int l) {
    need_args("async_cancel", n, 1, 1, l);
    return Value(async_runtime().cancel(async_id_arg("async_cancel", a[0], 0, l)));
}

inline Value b_async_cancelled(const Value* a, int n, int l) {
    need_args("async_cancelled", n, 1, 1, l);
    int id = async_id_arg("async_cancelled", a[0], 0, l);
    auto task = async_runtime().find(id);
    if (!task) return Value(false);
    std::lock_guard<std::mutex> lock(task->mutex);
    return Value(task->state == AsyncTaskState::Cancelled);
}

inline Value b_async_configure(const Value* a, int n, int l) {
    need_args("async_configure", n, 2, 2, l);
    int workers = need_positive_int("async_configure", a[0], 0, l);
    int capacity = need_positive_int("async_configure", a[1], 1, l);
    if (workers > 256) throw JitThrow{"async_configure(): workers must be 1..256", l};
    if (capacity > 1000000) throw JitThrow{"async_configure(): queue capacity must be 1..1000000", l};
    if (!async_runtime().configure((size_t)workers, (size_t)capacity)) {
        throw JitThrow{"async_configure(): requires no tracked tasks or open scopes", l};
    }
    return Value(true);
}

inline Value b_async_limits(const Value* a, int n, int l) {
    need_args("async_limits", n, 0, 0, l);
    size_t workers = 0, capacity = 0, queued = 0, running = 0, tracked = 0, scopes = 0;
    size_t max_retained = 0, retained = 0;
    async_runtime().limits(workers, capacity, queued, running, tracked, scopes,
                           &max_retained, &retained);
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["max_workers"] = Value((double)workers);
    d->elements["max_queue"] = Value((double)capacity);
    d->elements["max_tracked"] = Value((double)(workers + capacity));
    d->elements["queued"] = Value((double)queued);
    d->elements["running"] = Value((double)running);
    d->elements["tracked"] = Value((double)tracked);
    d->elements["scopes"] = Value((double)scopes);
    d->elements["max_retained_bytes"] = Value((double)max_retained);
    d->elements["retained_bytes"] = Value((double)retained);
    return out;
}

inline Value b_async_scope_open(const Value* a, int n, int l) {
    need_args("async_scope_open", n, 0, 0, l);
    return Value((double)async_runtime().open_scope("async_scope_open", l));
}

inline Value b_async_scope_attach(const Value* a, int n, int l) {
    need_args("async_scope_attach", n, 2, 2, l);
    int scope_id = async_id_arg("async_scope_attach", a[0], 0, l);
    int task_id = async_id_arg("async_scope_attach", a[1], 1, l);
    return Value(async_runtime().attach(scope_id, task_id, "async_scope_attach", l));
}

inline Value b_async_scope_cancel(const Value* a, int n, int l) {
    need_args("async_scope_cancel", n, 1, 1, l);
    int scope_id = async_id_arg("async_scope_cancel", a[0], 0, l);
    return Value((double)async_runtime().cancel_scope(scope_id, "async_scope_cancel", l));
}

inline Value async_scope_status_value(int scope_id, bool known, bool open,
                                      const std::vector<std::shared_ptr<AsyncTask>>& tasks) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["id"] = Value((double)scope_id);
    d->elements["known"] = Value(known);
    d->elements["open"] = Value(known && open);
    d->elements["closing"] = Value(known && !open);
    int queued = 0, running = 0, succeeded = 0, failed = 0, cancelled = 0;
    for (const auto& task : tasks) {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->state == AsyncTaskState::Queued) ++queued;
        else if (task->state == AsyncTaskState::Running) ++running;
        else if (task->state == AsyncTaskState::Succeeded) ++succeeded;
        else if (task->state == AsyncTaskState::Failed) ++failed;
        else if (task->state == AsyncTaskState::Cancelled) ++cancelled;
    }
    d->elements["total"] = Value((double)tasks.size());
    d->elements["queued"] = Value((double)queued);
    d->elements["running"] = Value((double)running);
    d->elements["ready"] = Value((double)(succeeded + failed + cancelled));
    d->elements["succeeded"] = Value((double)succeeded);
    d->elements["failed"] = Value((double)failed);
    d->elements["cancelled"] = Value((double)cancelled);
    return out;
}

inline Value b_async_scope_status(const Value* a, int n, int l) {
    need_args("async_scope_status", n, 1, 1, l);
    int scope_id = async_id_arg("async_scope_status", a[0], 0, l);
    bool open = false;
    std::vector<std::shared_ptr<AsyncTask>> tasks;
    bool known = async_runtime().scope_snapshot(scope_id, open, tasks);
    return async_scope_status_value(scope_id, known, open, tasks);
}

inline long long async_scope_timeout_arg(const char* fn, const Value* a, int n, int l) {
    if (n < 2) return -1;
    double raw = need_num(fn, a[1], 1, l);
    if (!std::isfinite(raw) || raw < 0 || raw > (double)std::numeric_limits<long long>::max()) {
        throw JitThrow{std::string(fn) + "(): timeout must be a finite non-negative number", l};
    }
    return (long long)raw;
}

inline Value async_close_scope_value(const char* fn, const Value* a, int n, int l,
                                     bool cancel_remaining) {
    need_args(fn, n, 1, 2, l);
    int scope_id = async_id_arg(fn, a[0], 0, l);
    long long timeout_ms = async_scope_timeout_arg(fn, a, n, l);
    AsyncScopeCloseSummary summary;
    bool known = false;
    bool closed = async_runtime().close_scope(scope_id, cancel_remaining, timeout_ms, summary, known);
    if (!known) throw JitThrow{std::string(fn) + "(): unknown scope id", l};
    if (!closed) {
        bool open = false;
        std::vector<std::shared_ptr<AsyncTask>> tasks;
        async_runtime().scope_snapshot(scope_id, open, tasks);
        Value out = async_scope_status_value(scope_id, true, open, tasks);
        out.as_dict()->elements["closed"] = Value(false);
        out.as_dict()->elements["timed_out"] = Value(true);
        return out;
    }
    if (summary.failed > 0) {
        std::string message = std::string(fn) + "(): child task " +
                              std::to_string(summary.first_failed_task) + " failed";
        if (!summary.first_error.empty()) message += ": " + summary.first_error;
        throw JitThrow{message, l};
    }
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["id"] = Value((double)scope_id);
    d->elements["closed"] = Value(true);
    d->elements["timed_out"] = Value(false);
    d->elements["total"] = Value((double)summary.total);
    d->elements["succeeded"] = Value((double)summary.succeeded);
    d->elements["failed"] = Value((double)summary.failed);
    d->elements["cancelled"] = Value((double)summary.cancelled);
    return out;
}

inline Value b_async_scope_close(const Value* a, int n, int l) {
    return async_close_scope_value("async_scope_close", a, n, l, true);
}

inline Value b_async_scope_join(const Value* a, int n, int l) {
    return async_close_scope_value("async_scope_join", a, n, l, false);
}

inline std::string async_result_or_throw(const char* fn, int line,
                                         const std::shared_ptr<AsyncTask>& task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    if (task->state == AsyncTaskState::Cancelled) {
        throw JitThrow{std::string(fn) + "(): task " + std::to_string(task->id) + " cancelled", line};
    }
    if (task->state == AsyncTaskState::Failed) {
        std::string message = std::string(fn) + "(): task " + std::to_string(task->id) + " failed";
        if (!task->error.empty()) message += ": " + task->error;
        throw JitThrow{message, line};
    }
    if (task->state != AsyncTaskState::Succeeded) {
        throw JitThrow{std::string(fn) + "(): task is not ready", line};
    }
    return task->output;
}

inline Value async_consume_result(const char* fn, int line,
                                  const std::shared_ptr<AsyncTask>& task) {
    if (!async_runtime().erase_terminal(task->id, task)) {
        throw JitThrow{std::string(fn) + "(): task was already consumed", line};
    }
    return Value(async_result_or_throw(fn, line, task));
}

inline Value b_async_await(const Value* a, int n, int l) {
    need_args("async_await", n, 1, 1, l);
    int id = async_id_arg("async_await", a[0], 0, l);
    auto task = async_runtime().find(id);
    if (!task) throw JitThrow{"async_await(): unknown task id", l};
    async_runtime().wait(task, -1);
    return async_consume_result("async_await", l, task);
}

inline Value b_async_await_timeout(const Value* a, int n, int l) {
    need_args("async_await_timeout", n, 2, 3, l);
    int id = async_id_arg("async_await_timeout", a[0], 0, l);
    double timeout_ms_raw = need_num("async_await_timeout", a[1], 1, l);
    if (!std::isfinite(timeout_ms_raw) || timeout_ms_raw < 0 ||
        timeout_ms_raw > (double)std::numeric_limits<long long>::max()) {
        throw JitThrow{"async_await_timeout(): timeout must be a finite non-negative number", l};
    }
    auto task = async_runtime().find(id);
    if (!task) throw JitThrow{"async_await_timeout(): unknown task id", l};
    if (!async_runtime().wait(task, (long long)timeout_ms_raw)) {
        return n >= 3 ? a[2] : Value::nil();
    }
    return async_consume_result("async_await_timeout", l, task);
}

inline Value b_async_ready_all(const Value* a, int n, int l) {
    need_args("async_ready_all", n, 1, 1, l);
    auto* ids = need_arr("async_ready_all", a[0], 0, l);
    for (const auto& item : ids->elements) {
        int id = async_id_arg("async_ready_all", item, 0, l);
        auto task = async_runtime().find(id);
        if (!task) return Value(false);
        std::lock_guard<std::mutex> task_lock(task->mutex);
        if (!async_state_terminal(task->state)) return Value(false);
    }
    return Value(true);
}

inline Value async_result_dict(int id, int index, const std::string& output) {
    Value out = Value::make_dict();
    auto* d = out.as_dict();
    d->elements["id"] = Value((double)id);
    d->elements["index"] = Value((double)index);
    d->elements["output"] = Value(output);
    return out;
}

inline bool async_take_ready(GCArray* ids, int line, const char* fn,
                             std::shared_ptr<AsyncTask>& ready_task, int& ready_index) {
    for (size_t i = 0; i < ids->elements.size(); ++i) {
        int id = async_id_arg(fn, ids->elements[i], 0, line);
        auto task = async_runtime().find(id);
        if (!task) throw JitThrow{std::string(fn) + "(): unknown task id", line};
        std::lock_guard<std::mutex> task_lock(task->mutex);
        if (async_state_terminal(task->state)) {
            ready_index = (int)i;
            ready_task = task;
            return true;
        }
    }
    return false;
}

inline Value b_async_any(const Value* a, int n, int l) {
    need_args("async_any", n, 1, 3, l);
    auto* raw_ids = need_arr("async_any", a[0], 0, l);
    if (raw_ids->elements.empty()) throw JitThrow{"async_any(): task id array must not be empty", l};
    GCArray* ids = a[0].as_arr();
    bool has_timeout = n >= 2;
    long long timeout_ms = 0;
    std::chrono::steady_clock::time_point deadline;
    if (has_timeout) {
        double raw_timeout = need_num("async_any", a[1], 1, l);
        if (!std::isfinite(raw_timeout) || raw_timeout < 0 ||
            raw_timeout > (double)std::numeric_limits<long long>::max()) {
            throw JitThrow{"async_any(): timeout must be a finite non-negative number", l};
        }
        timeout_ms = (long long)raw_timeout;
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }

    while (true) {
        const uint64_t observed_epoch = async_runtime().completion_epoch();
        std::shared_ptr<AsyncTask> ready_task;
        int ready_index = -1;
        if (async_take_ready(ids, l, "async_any", ready_task, ready_index)) {
            std::string output;
            try {
                output = async_result_or_throw("async_any", l, ready_task);
            } catch (...) {
                async_runtime().erase_terminal(ready_task->id, ready_task);
                throw;
            }
            if (!async_runtime().erase_terminal(ready_task->id, ready_task)) {
                throw JitThrow{"async_any(): task was already consumed", l};
            }
            return async_result_dict(ready_task->id, ready_index, output);
        }
        if (has_timeout) {
            auto now = std::chrono::steady_clock::now();
            if (timeout_ms == 0 || now >= deadline) return n >= 3 ? a[2] : Value::nil();
            long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining <= 0) remaining = 1;
            async_runtime().wait_for_completion(observed_epoch, remaining);
        } else {
            async_runtime().wait_for_completion(observed_epoch, -1);
        }
    }
}

inline Value b_async_all(const Value* a, int n, int l) {
    need_args("async_all", n, 1, 1, l);
    auto* ids = need_arr("async_all", a[0], 0, l);
    std::vector<std::shared_ptr<AsyncTask>> tasks;
    tasks.reserve(ids->elements.size());
    std::unordered_set<int> seen;
    for (const auto& item : ids->elements) {
        int id = async_id_arg("async_all", item, 0, l);
        if (!seen.insert(id).second) throw JitThrow{"async_all(): duplicate task id", l};
        auto task = async_runtime().find(id);
        if (!task) throw JitThrow{"async_all(): unknown task id", l};
        tasks.push_back(task);
    }
    for (const auto& task : tasks) async_runtime().wait(task, -1);

    if (!async_runtime().erase_terminal_batch(tasks)) {
        throw JitThrow{"async_all(): one or more tasks were already consumed", l};
    }
    Value out = Value::make_array();
    std::string first_error;
    for (const auto& task : tasks) {
        try {
            out.as_arr()->elements.push_back(Value(async_result_or_throw("async_all", l, task)));
        } catch (const JitThrow& thrown) {
            if (first_error.empty()) first_error = thrown.message;
        }
    }
    if (!first_error.empty()) throw JitThrow{first_error, l};
    return out;
}

inline Value b_async_all_timeout(const Value* a, int n, int l) {
    need_args("async_all_timeout", n, 2, 3, l);
    auto* ids = need_arr("async_all_timeout", a[0], 0, l);
    double raw_timeout = need_num("async_all_timeout", a[1], 1, l);
    if (!std::isfinite(raw_timeout) || raw_timeout < 0 ||
        raw_timeout > (double)std::numeric_limits<long long>::max()) {
        throw JitThrow{"async_all_timeout(): timeout must be a finite non-negative number", l};
    }
    long long timeout_ms = (long long)raw_timeout;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        const uint64_t observed_epoch = async_runtime().completion_epoch();
        bool all_ready = true;
        for (const auto& item : ids->elements) {
            int id = async_id_arg("async_all_timeout", item, 0, l);
            auto task = async_runtime().find(id);
            if (!task) throw JitThrow{"async_all_timeout(): unknown task id", l};
            std::lock_guard<std::mutex> task_lock(task->mutex);
            if (!async_state_terminal(task->state)) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            Value wait_arg[1] = {a[0]};
            return b_async_all(wait_arg, 1, l);
        }

        auto now = std::chrono::steady_clock::now();
        if (timeout_ms == 0 || now >= deadline) return n >= 3 ? a[2] : Value::nil();
        long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining <= 0) remaining = 1;
        async_runtime().wait_for_completion(observed_epoch, remaining);
    }
}

inline std::string trim_ascii(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

inline std::string first_nonempty_line(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ascii(line);
        if (!line.empty()) return line;
    }
    return "";
}

inline std::string ps_single_quote(const std::string& text) {
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

inline std::string sh_single_quote(const std::string& text) {
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

inline int http_server_pid_from_value(const Value& server, int line) {
    if (server.is_num()) return (int)server.as_num();
    if (!server.is_dict()) throw JitThrow{"http_server_stop(): expected server dict or pid", line};
    auto* d = server.as_dict();
    auto it = d->elements.find("pid");
    if (it == d->elements.end() || !it->second.is_num()) throw JitThrow{"http_server_stop(): server has no numeric pid", line};
    return (int)it->second.as_num();
}

inline bool command_has_stdout(const std::string& command) {
    return !first_nonempty_line(run_capture_command(command)).empty();
}

inline std::filesystem::path http_server_runner_path() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("sura_http_static_" + std::to_string(stamp) + ".js");
}

inline std::filesystem::path http_routes_data_path() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("sura_http_routes_" + std::to_string(stamp) + ".json");
}

inline bool write_http_server_runner(const std::filesystem::path& script) {
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << R"JS(
const http = require('http');
const fs = require('fs');
const path = require('path');
const root = path.resolve(process.argv[2]);
const port = Number(process.argv[3] || '8000');

function send(res, status, body, type) {
  res.writeHead(status, {'Content-Type': type || 'text/plain; charset=utf-8'});
  res.end(body);
}

function insideRoot(file) {
  const resolved = path.resolve(file);
  return resolved === root || resolved.startsWith(root + path.sep);
}

const server = http.createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') return send(res, 405, 'method not allowed');
  let pathname = '/';
  try {
    pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
  } catch {
    return send(res, 400, 'bad request');
  }
  let rel = pathname.replace(/^\/+/, '');
  if (!rel) rel = 'index.html';
  let file = path.resolve(root, rel);
  if (!insideRoot(file)) return send(res, 403, 'forbidden');
  fs.stat(file, (statErr, stat) => {
    if (statErr) return send(res, 404, 'not found');
    if (stat.isDirectory()) file = path.join(file, 'index.html');
    fs.readFile(file, (readErr, data) => {
      if (readErr) return send(res, 404, 'not found');
      res.writeHead(200, {'Content-Type': 'application/octet-stream'});
      if (req.method === 'HEAD') return res.end();
      res.end(data);
    });
  });
});

server.listen(port, '127.0.0.1');
)JS";
    return (bool)out;
}

inline bool write_http_routes_runner(const std::filesystem::path& script) {
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << R"JS(
const http = require('http');
const fs = require('fs');
const routesPath = process.argv[2];
const port = Number(process.argv[3] || '8000');
const routes = JSON.parse(fs.readFileSync(routesPath, 'utf8'));

function send(res, req, status, body, headers) {
  const finalHeaders = Object.assign({'Content-Type': 'text/plain; charset=utf-8'}, headers || {});
  res.writeHead(status, finalHeaders);
  if (req.method === 'HEAD') return res.end();
  res.end(body);
}

function requestUrl(req) {
  try {
    return new URL(req.url, 'http://127.0.0.1');
  } catch {
    return null;
  }
}

function requestQuery(url) {
  const out = {};
  for (const [key, value] of url.searchParams.entries()) {
    if (Object.prototype.hasOwnProperty.call(out, key)) {
      if (!Array.isArray(out[key])) out[key] = [out[key]];
      out[key].push(value);
    } else {
      out[key] = value;
    }
  }
  return out;
}

function routeFor(method, pathname) {
  const upper = method.toUpperCase();
  return routes[upper + ' ' + pathname] ??
         routes['* ' + pathname] ??
         routes[pathname] ??
         routes['*'];
}

function headersFrom(spec) {
  const headers = {};
  if (!spec || typeof spec !== 'object' || Array.isArray(spec) || !spec.headers) return headers;
  for (const [key, value] of Object.entries(spec.headers)) headers[key] = String(value);
  return headers;
}

function handle(req, res, route, url, body) {
  const pathname = decodeURIComponent(url.pathname);
  const spec = route && typeof route === 'object' && !Array.isArray(route) ? route : {body: String(route ?? '')};
  const status = Number(spec.status || 200);
  const headers = headersFrom(spec);
  let responseBody = '';
  if (spec.echo === true) {
    headers['Content-Type'] = headers['Content-Type'] || 'application/json; charset=utf-8';
    responseBody = JSON.stringify({
      method: req.method,
      path: pathname,
      query: requestQuery(url),
      headers: req.headers,
      body
    });
  } else if (Object.prototype.hasOwnProperty.call(spec, 'json')) {
    headers['Content-Type'] = headers['Content-Type'] || 'application/json; charset=utf-8';
    responseBody = JSON.stringify(spec.json);
  } else if (Object.prototype.hasOwnProperty.call(spec, 'body')) {
    responseBody = String(spec.body ?? '');
  }
  send(res, req, status, responseBody, headers);
}

const server = http.createServer((req, res) => {
  const url = requestUrl(req);
  if (!url) return send(res, req, 400, 'bad request');
  let pathname = '/';
  try {
    pathname = decodeURIComponent(url.pathname);
  } catch {
    return send(res, req, 400, 'bad request');
  }
  const route = routeFor(req.method, pathname);
  if (route === undefined) {
    return send(res, req, 404, JSON.stringify({error: 'not found', method: req.method, path: pathname}), {'Content-Type': 'application/json; charset=utf-8'});
  }
  let body = '';
  req.setEncoding('utf8');
  req.on('data', chunk => {
    body += chunk;
    if (body.length > 1048576) req.destroy();
  });
  req.on('end', () => handle(req, res, route, url, body));
});

server.listen(port, '127.0.0.1');
)JS";
    return (bool)out;
}

inline Value b_http_serve_static(const Value* a, int n, int l) {
    need_args("http_serve_static", n, 1, 2, l);
    std::string dir = need_str("http_serve_static", a[0], 0, l);
    int port = n >= 2 ? (int)need_num("http_serve_static", a[1], 1, l) : 8000;
    if (port <= 0 || port > 65535) throw JitThrow{"http_serve_static(): port must be 1..65535", l};
    if (dir.find_first_of("\r\n") != std::string::npos) {
        throw JitThrow{"http_serve_static(): directory contains unsupported characters", l};
    }
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(fs_path_from_utf8(dir), ec);
    if (ec || !std::filesystem::is_directory(root)) {
        throw JitThrow{"http_serve_static(): directory not found: " + dir, l};
    }

    std::string cmd;
    std::string runner;
#ifdef _WIN32
    if (command_has_stdout("node --version 2>NUL")) {
        std::filesystem::path script = http_server_runner_path();
        if (!write_http_server_runner(script)) throw JitThrow{"http_serve_static(): failed to write node server runner", l};
        runner = script.string();
        std::string args = "@(" + ps_single_quote(runner) + "," + ps_single_quote(fs_path_to_utf8(root)) + ",'" + std::to_string(port) + "')";
        cmd = "powershell -NoProfile -ExecutionPolicy Bypass -Command "
              "\"$p=Start-Process -FilePath 'node' -ArgumentList " + args +
              " -WindowStyle Hidden -PassThru; $p.Id\"";
    } else {
        std::string args =
            "@('-m','http.server','" + std::to_string(port) +
            "','--bind','127.0.0.1','--directory'," + ps_single_quote(fs_path_to_utf8(root)) + ")";
        cmd = "powershell -NoProfile -ExecutionPolicy Bypass -Command "
              "\"$p=Start-Process -FilePath 'python' -ArgumentList " + args +
              " -WindowStyle Hidden -PassThru; $p.Id\"";
    }
#else
    if (command_has_stdout("node --version 2>/dev/null")) {
        std::filesystem::path script = http_server_runner_path();
        if (!write_http_server_runner(script)) throw JitThrow{"http_serve_static(): failed to write node server runner", l};
        runner = script.string();
        cmd = "node " + sh_single_quote(runner) + " " + sh_single_quote(fs_path_to_utf8(root)) + " " + std::to_string(port) +
              " >/dev/null 2>&1 & echo $!";
    } else {
        cmd = "python -m http.server " + std::to_string(port) +
              " --bind 127.0.0.1 --directory " + sh_single_quote(fs_path_to_utf8(root)) +
              " >/dev/null 2>&1 & echo $!";
    }
#endif
    std::string line = first_nonempty_line(run_capture_command(cmd));
    int pid = 0;
    try { pid = std::stoi(line); } catch (...) { pid = 0; }
    if (pid <= 0) throw JitThrow{"http_serve_static(): failed to start python http.server", l};

    Value server = Value::make_dict();
    auto* d = server.as_dict();
    d->elements["type"] = Value(std::string("http_server"));
    d->elements["pid"] = Value((double)pid);
    d->elements["port"] = Value((double)port);
    d->elements["host"] = Value(std::string("127.0.0.1"));
    d->elements["url"] = Value(std::string("http://127.0.0.1:") + std::to_string(port) + "/");
    d->elements["directory"] = Value(fs_path_to_utf8(root));
    d->elements["runner"] = Value(runner);
    return server;
}

inline Value b_http_serve_routes(const Value* a, int n, int l) {
    need_args("http_serve_routes", n, 1, 2, l);
    if (!a[0].is_dict()) throw JitThrow{"http_serve_routes(): routes must be a dictionary", l};
    int port = n >= 2 ? (int)need_num("http_serve_routes", a[1], 1, l) : 8000;
    if (port <= 0 || port > 65535) throw JitThrow{"http_serve_routes(): port must be 1..65535", l};
    if (!command_has_stdout(
#ifdef _WIN32
            "node --version 2>NUL"
#else
            "node --version 2>/dev/null"
#endif
        )) {
        throw JitThrow{"http_serve_routes(): Node.js is required for route servers", l};
    }

    std::filesystem::path routes_file = http_routes_data_path();
    {
        std::ofstream out(routes_file, std::ios::binary | std::ios::trunc);
        if (!out) throw JitThrow{"http_serve_routes(): failed to write route data", l};
        out << json_stringify_value(a[0]);
    }

    std::filesystem::path script = http_server_runner_path();
    if (!write_http_routes_runner(script)) throw JitThrow{"http_serve_routes(): failed to write node server runner", l};

    std::string cmd;
#ifdef _WIN32
    std::string args = "@(" + ps_single_quote(script.string()) + "," + ps_single_quote(fs_path_to_utf8(routes_file)) + ",'" + std::to_string(port) + "')";
    cmd = "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"$p=Start-Process -FilePath 'node' -ArgumentList " + args +
          " -WindowStyle Hidden -PassThru; $p.Id\"";
#else
    cmd = "node " + sh_single_quote(script.string()) + " " + sh_single_quote(fs_path_to_utf8(routes_file)) + " " + std::to_string(port) +
          " >/dev/null 2>&1 & echo $!";
#endif
    std::string line = first_nonempty_line(run_capture_command(cmd));
    int pid = 0;
    try { pid = std::stoi(line); } catch (...) { pid = 0; }
    if (pid <= 0) {
        std::error_code ec;
        std::filesystem::remove(script, ec);
        std::filesystem::remove(routes_file, ec);
        throw JitThrow{"http_serve_routes(): failed to start node route server", l};
    }

    Value server = Value::make_dict();
    auto* d = server.as_dict();
    d->elements["type"] = Value(std::string("http_routes_server"));
    d->elements["pid"] = Value((double)pid);
    d->elements["port"] = Value((double)port);
    d->elements["host"] = Value(std::string("127.0.0.1"));
    d->elements["url"] = Value(std::string("http://127.0.0.1:") + std::to_string(port) + "/");
    d->elements["routes_file"] = Value(fs_path_to_utf8(routes_file));
    d->elements["runner"] = Value(script.string());
    return server;
}

inline Value b_http_server_url(const Value* a, int n, int l) {
    need_args("http_server_url", n, 1, 1, l);
    if (!a[0].is_dict()) throw JitThrow{"http_server_url(): expected server dict", l};
    auto* d = a[0].as_dict();
    auto it = d->elements.find("url");
    if (it == d->elements.end()) throw JitThrow{"http_server_url(): server has no url", l};
    return it->second;
}

inline Value b_http_server_stop(const Value* a, int n, int l) {
    need_args("http_server_stop", n, 1, 1, l);
    int pid = http_server_pid_from_value(a[0], l);
    if (pid <= 0) return Value(false);
    std::string runner;
    if (a[0].is_dict()) {
        auto* d = a[0].as_dict();
        auto it = d->elements.find("runner");
        if (it != d->elements.end() && it->second.is_str()) runner = it->second.as_str();
    }
    std::string routes_file;
    if (a[0].is_dict()) {
        auto* d = a[0].as_dict();
        auto it = d->elements.find("routes_file");
        if (it != d->elements.end() && it->second.is_str()) routes_file = it->second.as_str();
    }
#ifdef _WIN32
    std::string cmd = "taskkill /PID " + std::to_string(pid) + " /T /F >NUL 2>NUL && echo stopped";
#else
    std::string cmd = "kill " + std::to_string(pid) + " >/dev/null 2>&1 && echo stopped";
#endif
    bool stopped = run_capture_command(cmd).find("stopped") != std::string::npos;
    if (!runner.empty()) {
        std::error_code ec;
        std::filesystem::remove(runner, ec);
    }
    if (!routes_file.empty()) {
        std::error_code ec;
        std::filesystem::remove(fs_path_from_utf8(routes_file), ec);
    }
    return Value(stopped);
}

#ifdef _WIN32
inline void sura_window_pump();
#endif

inline void sura_runtime_sleep_ms(long long ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (true) {
        sura_window_pump();
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(remaining, 4)));
    }
    sura_window_pump();
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

inline Value b_sleep_ms(const Value* a, int n, int l) {
    need_args("sleep_ms", n, 1, 1, l);
    double ms = need_num("sleep_ms", a[0], 0, l);
    if (ms < 0) throw JitThrow{"sleep_ms(): duration must be non-negative", l};
    sura_runtime_sleep_ms((long long)ms);
    return Value::nil();
}

inline Value b_wait(const Value* a, int n, int l) {
    need_args("wait", n, 1, 1, l);
    double ms = need_num("wait", a[0], 0, l);
    if (ms < 0) throw JitThrow{"wait(): duration must be non-negative", l};
    sura_runtime_sleep_ms((long long)ms);
    return Value::nil();
}

// ── Pre-existing built-ins (moved here for consistency) ──────────

inline Value b_print(const Value* args, int nargs, int) {
    std::string out;
    for (int i = 0; i < nargs; ++i) out += args[i].to_str();
    std::cout << out << "\n";
    return Value::nil();
}

inline Value b_print_n(const Value* args, int nargs, int) {
    std::string out;
    for (int i = 0; i < nargs; ++i) out += args[i].to_str();
    std::cout << out;
    return Value::nil();
}

inline std::string console_join(const Value* args, int nargs, int start = 0) {
    std::string out;
    for (int i = start; i < nargs; ++i) {
        if (i > start) out += " ";
        out += args[i].to_str();
    }
    return out;
}

inline std::mutex& console_state_mutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<std::string, std::chrono::steady_clock::time_point>& console_timers() {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> timers;
    return timers;
}

inline std::unordered_map<std::string, long long>& console_counts() {
    static std::unordered_map<std::string, long long> counts;
    return counts;
}

inline std::unordered_map<std::string, std::chrono::steady_clock::time_point>& console_profiles() {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> profiles;
    return profiles;
}

inline std::vector<std::string>& console_groups() {
    static std::vector<std::string> groups;
    return groups;
}

inline std::string console_indent_prefix() {
    std::lock_guard<std::mutex> lock(console_state_mutex());
    return std::string(console_groups().size() * 2, ' ');
}

inline void console_write_line(std::ostream& out, const std::string& text) {
    out << console_indent_prefix() << text << "\n";
}

inline void console_write_text(std::ostream& out, const std::string& text) {
    out << console_indent_prefix() << text;
    out.flush();
}

inline Value b_console_log(const Value* args, int nargs, int) {
    console_write_line(std::cout, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_write(const Value* args, int nargs, int) {
    console_write_text(std::cout, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_write_line(const Value* args, int nargs, int) {
    console_write_line(std::cout, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_info(const Value* args, int nargs, int) {
    console_write_line(std::cout, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_debug(const Value* args, int nargs, int) {
    console_write_line(std::cout, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_warn(const Value* args, int nargs, int) {
    console_write_line(std::cerr, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_error(const Value* args, int nargs, int) {
    console_write_line(std::cerr, console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_exception(const Value* args, int nargs, int) {
    console_write_line(std::cerr, console_join(args, nargs));
    return Value::nil();
}

inline std::string console_join_raw(const Value* args, int nargs) {
    std::string out;
    for (int i = 0; i < nargs; ++i) out += args[i].to_str();
    return out;
}

inline int console_optional_indent(const char* name, const Value* args, int nargs, int line, int default_indent = 2) {
    double raw_indent = nargs >= 2 ? need_num(name, args[1], 1, line) : (double)default_indent;
    if (raw_indent < 0 || raw_indent > 16 || raw_indent != std::floor(raw_indent)) {
        throw JitThrow{std::string(name) + "(): indent must be an integer from 0 to 16", line};
    }
    return (int)raw_indent;
}

inline std::string console_format_structured_value(const Value& value, int indent) {
    if (indent == 0) return json_stringify_value(value);
    return json_pretty_value(value, indent, 0);
}

inline Value b_console_raw(const Value* args, int nargs, int) {
    std::cout << console_join_raw(args, nargs);
    std::cout.flush();
    return Value::nil();
}

inline Value b_console_flush(const Value*, int nargs, int l) {
    need_args("console_flush", nargs, 0, 0, l);
    std::cout.flush();
    std::cerr.flush();
    return Value::nil();
}

inline Value b_console_json(const Value* args, int nargs, int l) {
    need_args("console_json", nargs, 1, 2, l);
    int indent = console_optional_indent("console_json", args, nargs, l);
    console_write_line(std::cout, console_format_structured_value(args[0], indent));
    return Value::nil();
}

inline Value b_console_inspect(const Value* args, int nargs, int l) {
    need_args("console_inspect", nargs, 1, 2, l);
    if (args[0].is_arr() || args[0].is_dict()) {
        int indent = console_optional_indent("console_inspect", args, nargs, l);
        return Value(console_format_structured_value(args[0], indent));
    }
    return Value(args[0].to_str());
}

inline Value b_console_hrtime(const Value*, int nargs, int l) {
    need_args("console_hrtime", nargs, 0, 0, l);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return Value(std::chrono::duration<double, std::milli>(now).count());
}

inline Value b_console_beep(const Value*, int nargs, int l) {
    need_args("console_beep", nargs, 0, 0, l);
    std::cout << '\a';
    std::cout.flush();
    return Value::nil();
}

inline std::string console_cell_text(const Value& value) {
    std::string text = value.to_str();
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return text;
}

inline std::string console_token(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        out.push_back((char)std::tolower(ch));
    }
    return out;
}

inline bool console_try_style_code(const std::string& raw, int& code) {
    std::string name = console_token(raw);
    if (name == "reset" || name == "default" || name == "normal") { code = 0; return true; }
    if (name == "bold") { code = 1; return true; }
    if (name == "dim" || name == "faint") { code = 2; return true; }
    if (name == "italic") { code = 3; return true; }
    if (name == "underline") { code = 4; return true; }
    if (name == "blink") { code = 5; return true; }
    if (name == "inverse" || name == "invert") { code = 7; return true; }
    if (name == "hidden") { code = 8; return true; }
    if (name == "strike" || name == "strikethrough") { code = 9; return true; }
    return false;
}

inline bool console_try_color_code(const std::string& raw, bool background, int& code) {
    std::string name = console_token(raw);
    if (name == "reset" || name == "default" || name == "none") { code = background ? 49 : 39; return true; }
    const int base = background ? 40 : 30;
    const int bright = background ? 100 : 90;
    if (name == "black") { code = base + 0; return true; }
    if (name == "red") { code = base + 1; return true; }
    if (name == "green") { code = base + 2; return true; }
    if (name == "yellow") { code = base + 3; return true; }
    if (name == "blue") { code = base + 4; return true; }
    if (name == "magenta" || name == "purple") { code = base + 5; return true; }
    if (name == "cyan") { code = base + 6; return true; }
    if (name == "white") { code = base + 7; return true; }
    if (name == "gray" || name == "grey" || name == "brightblack") { code = bright + 0; return true; }
    if (name == "brightred") { code = bright + 1; return true; }
    if (name == "brightgreen") { code = bright + 2; return true; }
    if (name == "brightyellow") { code = bright + 3; return true; }
    if (name == "brightblue") { code = bright + 4; return true; }
    if (name == "brightmagenta" || name == "brightpurple") { code = bright + 5; return true; }
    if (name == "brightcyan") { code = bright + 6; return true; }
    if (name == "brightwhite") { code = bright + 7; return true; }
    return false;
}

inline std::string console_ansi_sequence(const std::vector<int>& codes) {
    if (codes.empty()) return "";
    std::string out = "\x1B[";
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i) out += ";";
        out += std::to_string(codes[i]);
    }
    out += "m";
    return out;
}

inline void console_add_style_code(const std::string& raw, std::vector<int>& codes, int line) {
    int code = 0;
    if (console_try_style_code(raw, code) || console_try_color_code(raw, false, code)) {
        codes.push_back(code);
        return;
    }
    throw JitThrow{"console_style(): unknown ANSI style or color '" + raw + "'", line};
}

inline std::vector<int> console_style_codes(const Value& styles, int line) {
    std::vector<int> codes;
    if (styles.is_arr()) {
        for (const auto& item : styles.as_arr()->elements) {
            if (!item.is_str()) throw JitThrow{"console_style(): style array must contain strings", line};
            console_add_style_code(item.as_str_ref(), codes, line);
        }
    } else if (styles.is_str()) {
        console_add_style_code(styles.as_str_ref(), codes, line);
    } else {
        throw JitThrow{"console_style(): style must be a string or array of strings", line};
    }
    return codes;
}

inline std::string console_strip_ansi_text(const std::string& text) {
    static const std::regex ansi_re("\x1B\\[[0-9;?]*[ -/]*[@-~]");
    return std::regex_replace(text, ansi_re, "");
}

inline Value b_console_style(const Value* args, int nargs, int l) {
    need_args("console_style", nargs, 2, 2, l);
    std::vector<int> codes = console_style_codes(args[1], l);
    return Value(console_ansi_sequence(codes) + args[0].to_str() + "\x1B[0m");
}

inline Value b_console_color(const Value* args, int nargs, int l) {
    need_args("console_color", nargs, 2, 3, l);
    std::vector<int> codes;
    int code = 0;
    std::string fg = need_str("console_color", args[1], 1, l);
    if (!console_try_color_code(fg, false, code)) {
        throw JitThrow{"console_color(): unknown foreground color '" + fg + "'", l};
    }
    codes.push_back(code);
    if (nargs >= 3 && !args[2].is_nil()) {
        std::string bg = need_str("console_color", args[2], 2, l);
        if (!console_try_color_code(bg, true, code)) {
            throw JitThrow{"console_color(): unknown background color '" + bg + "'", l};
        }
        codes.push_back(code);
    }
    return Value(console_ansi_sequence(codes) + args[0].to_str() + "\x1B[0m");
}

inline Value b_console_strip_ansi(const Value* args, int nargs, int l) {
    need_args("console_strip_ansi", nargs, 1, 1, l);
    return Value(console_strip_ansi_text(args[0].to_str()));
}

inline Value b_console_set_color(const Value* args, int nargs, int l) {
    need_args("console_set_color", nargs, 1, 2, l);
    std::vector<int> codes;
    int code = 0;
    std::string fg = need_str("console_set_color", args[0], 0, l);
    if (!console_try_color_code(fg, false, code)) {
        throw JitThrow{"console_set_color(): unknown foreground color '" + fg + "'", l};
    }
    codes.push_back(code);
    if (nargs >= 2 && !args[1].is_nil()) {
        std::string bg = need_str("console_set_color", args[1], 1, l);
        if (!console_try_color_code(bg, true, code)) {
            throw JitThrow{"console_set_color(): unknown background color '" + bg + "'", l};
        }
        codes.push_back(code);
    }
    std::cout << console_ansi_sequence(codes);
    std::cout.flush();
    return Value::nil();
}

inline Value b_console_reset_color(const Value*, int nargs, int l) {
    need_args("console_reset_color", nargs, 0, 0, l);
    std::cout << "\x1B[0m";
    std::cout.flush();
    return Value::nil();
}

inline bool console_stdout_is_tty() {
#ifdef _WIN32
    DWORD mode = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return out != nullptr && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode);
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

inline std::pair<int, int> console_terminal_size() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        int width = (int)(info.srWindow.Right - info.srWindow.Left + 1);
        int height = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
        return {std::max(0, width), std::max(0, height)};
    }
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        return {(int)ws.ws_col, (int)ws.ws_row};
    }
#endif
    return {0, 0};
}

inline Value b_console_is_tty(const Value*, int nargs, int l) {
    need_args("console_is_tty", nargs, 0, 0, l);
    return Value(console_stdout_is_tty());
}

inline Value b_console_width(const Value*, int nargs, int l) {
    need_args("console_width", nargs, 0, 0, l);
    return Value(console_terminal_size().first);
}

inline Value b_console_height(const Value*, int nargs, int l) {
    need_args("console_height", nargs, 0, 0, l);
    return Value(console_terminal_size().second);
}

inline Value b_console_size(const Value*, int nargs, int l) {
    need_args("console_size", nargs, 0, 0, l);
    auto size = console_terminal_size();
    Value out = Value::make_dict();
    auto* dict = out.as_dict();
    dict->elements["width"] = Value(size.first);
    dict->elements["height"] = Value(size.second);
    dict->elements["is_tty"] = Value(console_stdout_is_tty());
    return out;
}

inline Value b_console_status(const Value*, int nargs, int l) {
    need_args("console_status", nargs, 0, 0, l);
    auto size = console_terminal_size();
    Value out = Value::make_dict();
    auto* dict = out.as_dict();
    dict->elements["width"] = Value(size.first);
    dict->elements["height"] = Value(size.second);
    dict->elements["is_tty"] = Value(console_stdout_is_tty());
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        dict->elements["group_depth"] = Value((int)console_groups().size());
        dict->elements["timers"] = Value((int)console_timers().size());
        dict->elements["counters"] = Value((int)console_counts().size());
        dict->elements["profiles"] = Value((int)console_profiles().size());
    }
    return out;
}

inline void console_table_print_row(const std::vector<std::string>& cells,
                                    const std::vector<size_t>& widths) {
    std::cout << console_indent_prefix();
    for (size_t i = 0; i < cells.size(); ++i) {
        std::cout << (i == 0 ? "| " : " | ") << cells[i];
        if (widths[i] > cells[i].size()) {
            std::cout << std::string(widths[i] - cells[i].size(), ' ');
        }
    }
    std::cout << " |\n";
}

inline Value b_console_table(const Value* args, int nargs, int l) {
    need_args("console_table", nargs, 1, 1, l);
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> headers;

    if (args[0].is_arr()) {
        headers.push_back("(index)");
        std::vector<std::string> keys;
        auto* arr = args[0].as_arr();
        for (const auto& item : arr->elements) {
            if (!item.is_dict()) continue;
            for (const auto& entry : item.as_dict()->elements) {
                if (std::find(keys.begin(), keys.end(), entry.first) == keys.end()) {
                    keys.push_back(entry.first);
                }
            }
        }
        std::sort(keys.begin(), keys.end());
        if (keys.empty()) headers.push_back("value");
        else headers.insert(headers.end(), keys.begin(), keys.end());

        for (size_t i = 0; i < arr->elements.size(); ++i) {
            std::vector<std::string> row{std::to_string(i)};
            const Value& item = arr->elements[i];
            if (keys.empty()) {
                row.push_back(console_cell_text(item));
            } else if (item.is_dict()) {
                auto* dict = item.as_dict();
                for (const auto& key : keys) {
                    auto it = dict->elements.find(key);
                    row.push_back(it == dict->elements.end() ? "" : console_cell_text(it->second));
                }
            } else {
                row.resize(headers.size(), "");
                if (headers.size() > 1) row[1] = console_cell_text(item);
            }
            rows.push_back(std::move(row));
        }
    } else if (args[0].is_dict()) {
        headers = {"key", "value"};
        std::vector<std::string> keys;
        for (const auto& entry : args[0].as_dict()->elements) keys.push_back(entry.first);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            rows.push_back({key, console_cell_text(args[0].as_dict()->elements[key])});
        }
    } else {
        headers = {"value"};
        rows.push_back({console_cell_text(args[0])});
    }

    std::vector<size_t> widths(headers.size(), 0);
    for (size_t i = 0; i < headers.size(); ++i) widths[i] = headers[i].size();
    for (const auto& row : rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }
    console_table_print_row(headers, widths);
    for (const auto& row : rows) console_table_print_row(row, widths);
    return Value::nil();
}

inline Value b_console_dir(const Value* args, int nargs, int l) {
    need_args("console_dir", nargs, 1, 2, l);
    console_write_line(std::cout, console_cell_text(args[0]));
    return Value::nil();
}

inline Value b_console_dirxml(const Value* args, int nargs, int l) {
    need_args("console_dirxml", nargs, 1, -1, l);
    console_write_line(std::cout, nargs == 1 ? console_cell_text(args[0]) : console_join(args, nargs));
    return Value::nil();
}

inline Value b_console_trace(const Value* args, int nargs, int l) {
    std::string message = nargs > 0 ? console_join(args, nargs) : "";
    std::string line = message.empty() ? "Trace" : "Trace: " + message;
    line += " (line " + std::to_string(l) + ")";
    console_write_line(std::cerr, line);
    return Value::nil();
}

inline Value b_console_group(const Value* args, int nargs, int) {
    if (nargs > 0) console_write_line(std::cout, console_join(args, nargs));
    std::lock_guard<std::mutex> lock(console_state_mutex());
    console_groups().push_back(nargs > 0 ? console_join(args, nargs) : "");
    return Value::nil();
}

inline Value b_console_group_end(const Value*, int nargs, int l) {
    need_args("console_group_end", nargs, 0, 0, l);
    std::lock_guard<std::mutex> lock(console_state_mutex());
    if (!console_groups().empty()) console_groups().pop_back();
    return Value::nil();
}

inline Value b_console_clear(const Value*, int nargs, int l) {
    need_args("console_clear", nargs, 0, 0, l);
#ifdef _WIN32
    DWORD mode = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        std::cout << "\x1B[2J\x1B[H";
        std::cout.flush();
    }
#else
    std::cout << "\x1B[2J\x1B[H";
    std::cout.flush();
#endif
    return Value::nil();
}

inline Value b_silent(const Value*, int nargs, int l) {
    need_args("silent", nargs, 0, 1, l);
    return Value::nil();
}

inline Value b_console_assert(const Value* args, int nargs, int l) {
    need_args("console_assert", nargs, 1, -1, l);
    if (!args[0].truthy()) {
        std::cerr << "Assertion failed";
        if (nargs > 1) std::cerr << ": " << console_join(args, nargs, 1);
        std::cerr << "\n";
    }
    return Value::nil();
}

inline Value b_console_time(const Value* args, int nargs, int l) {
    need_args("console_time", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    std::lock_guard<std::mutex> lock(console_state_mutex());
    console_timers()[label] = std::chrono::steady_clock::now();
    return Value::nil();
}

inline Value b_console_time_end(const Value* args, int nargs, int l) {
    need_args("console_time_end", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    double elapsed_ms = 0.0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        auto& timers = console_timers();
        auto it = timers.find(label);
        if (it != timers.end()) {
            elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->second).count();
            timers.erase(it);
            found = true;
        }
    }
    if (!found) {
        std::cerr << "Timer '" << label << "' does not exist\n";
        return Value::nil();
    }
    console_write_line(std::cout, label + ": " + std::to_string(elapsed_ms) + " ms");
    return Value(elapsed_ms);
}

inline Value b_console_time_log(const Value* args, int nargs, int l) {
    need_args("console_time_log", nargs, 0, -1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    double elapsed_ms = 0.0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        auto& timers = console_timers();
        auto it = timers.find(label);
        if (it != timers.end()) {
            elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->second).count();
            found = true;
        }
    }
    if (!found) {
        console_write_line(std::cerr, "Timer '" + label + "' does not exist");
        return Value::nil();
    }
    std::string suffix = nargs > 1 ? " " + console_join(args, nargs, 1) : "";
    console_write_line(std::cout, label + ": " + std::to_string(elapsed_ms) + " ms" + suffix);
    return Value(elapsed_ms);
}

inline Value b_console_time_stamp(const Value* args, int nargs, int l) {
    need_args("console_time_stamp", nargs, 0, 1, l);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string label = nargs >= 1 ? " " + args[0].to_str() : "";
    console_write_line(std::cout, "Timestamp" + label + ": " + std::to_string(elapsed_ms) + " ms");
    return Value(elapsed_ms);
}

inline Value b_console_count(const Value* args, int nargs, int l) {
    need_args("console_count", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    long long count = 0;
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        count = ++console_counts()[label];
    }
    console_write_line(std::cout, label + ": " + std::to_string(count));
    return Value(count);
}

inline Value b_console_count_reset(const Value* args, int nargs, int l) {
    need_args("console_count_reset", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    std::lock_guard<std::mutex> lock(console_state_mutex());
    console_counts().erase(label);
    return Value::nil();
}

inline Value b_console_profile(const Value* args, int nargs, int l) {
    need_args("console_profile", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        console_profiles()[label] = std::chrono::steady_clock::now();
    }
    console_write_line(std::cout, "Profile '" + label + "' started");
    return Value::nil();
}

inline Value b_console_profile_end(const Value* args, int nargs, int l) {
    need_args("console_profile_end", nargs, 0, 1, l);
    std::string label = nargs >= 1 ? args[0].to_str() : "default";
    double elapsed_ms = 0.0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(console_state_mutex());
        auto& profiles = console_profiles();
        auto it = profiles.find(label);
        if (it != profiles.end()) {
            elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->second).count();
            profiles.erase(it);
            found = true;
        }
    }
    if (!found) {
        console_write_line(std::cerr, "Profile '" + label + "' does not exist");
        return Value::nil();
    }
    console_write_line(std::cout, "Profile '" + label + "': " + std::to_string(elapsed_ms) + " ms");
    return Value(elapsed_ms);
}

inline std::string sura_game_token(std::string raw) {
    std::string out;
    for (unsigned char ch : raw) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        out.push_back((char)std::tolower(ch));
    }
    return out;
}

#ifdef _WIN32
inline int sura_key_virtual_code(const std::string& raw) {
    std::string key = sura_game_token(raw);
    static const std::unordered_map<std::string, int> named = {
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"tab", VK_TAB},
        {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"insert", VK_INSERT}, {"ins", VK_INSERT}, {"home", VK_HOME},
        {"end", VK_END}, {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"shift", VK_SHIFT}, {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT},
        {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"lctrl", VK_LCONTROL},
        {"rctrl", VK_RCONTROL}, {"alt", VK_MENU}, {"lalt", VK_LMENU}, {"ralt", VK_RMENU},
        {"mouseleft", VK_LBUTTON}, {"mouseright", VK_RBUTTON}, {"mousemiddle", VK_MBUTTON}
    };
    auto it = named.find(key);
    if (it != named.end()) return it->second;
    if (key.size() >= 2 && key[0] == 'f') {
        try {
            int n = std::stoi(key.substr(1));
            if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
        } catch (...) {}
    }
    if (key.size() == 1) {
        unsigned char ch = (unsigned char)key[0];
        if (std::isalpha(ch)) return std::toupper(ch);
        if (std::isdigit(ch)) return ch;
        SHORT vk = VkKeyScanA((char)ch);
        if (vk != -1) return vk & 0xff;
    }
    return 0;
}

inline std::array<unsigned char, 256>& sura_window_key_states() {
    static std::array<unsigned char, 256> keys {};
    return keys;
}

inline std::deque<std::string>& sura_window_key_events() {
    static std::deque<std::string> events;
    return events;
}

inline std::string sura_key_name_from_vk(WPARAM key) {
    if (key >= 'A' && key <= 'Z') return std::string(1, (char)std::tolower((unsigned char)key));
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    switch (key) {
        case VK_SPACE: return "space";
        case VK_RETURN: return "enter";
        case VK_ESCAPE: return "escape";
        case VK_TAB: return "tab";
        case VK_BACK: return "backspace";
        case VK_DELETE: return "delete";
        case VK_INSERT: return "insert";
        case VK_HOME: return "home";
        case VK_END: return "end";
        case VK_PRIOR: return "pageup";
        case VK_NEXT: return "pagedown";
        case VK_UP: return "up";
        case VK_DOWN: return "down";
        case VK_LEFT: return "left";
        case VK_RIGHT: return "right";
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: return "shift";
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: return "ctrl";
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU: return "alt";
        default:
            if (key >= VK_F1 && key <= VK_F24) return "f" + std::to_string((int)(key - VK_F1 + 1));
            return "";
    }
}

inline void sura_window_push_key_event(WPARAM key) {
    std::string name = sura_key_name_from_vk(key);
    if (name.empty()) return;
    auto& events = sura_window_key_events();
    if (events.size() >= 128) events.pop_front();
    events.push_back(name);
}

inline std::string sura_window_pop_key_event() {
    auto& events = sura_window_key_events();
    if (events.empty()) return "";
    std::string out = events.front();
    events.pop_front();
    return out;
}

inline bool sura_window_key_state_down(int vk) {
    if (vk < 0 || vk >= (int)sura_window_key_states().size()) return false;
    return sura_window_key_states()[(size_t)vk] != 0;
}

inline void sura_window_set_key_state(WPARAM key, bool down) {
    if (key < sura_window_key_states().size()) {
        sura_window_key_states()[(size_t)key] = down ? 1 : 0;
    }
}

inline void sura_window_clear_key_states() {
    sura_window_key_states().fill(0);
    sura_window_key_events().clear();
}

inline std::string sura_readkey_name(int ch) {
    if (ch == 13) return "enter";
    if (ch == 27) return "escape";
    if (ch == 8) return "backspace";
    if (ch == 9) return "tab";
    if (ch >= 32 && ch <= 126) return std::string(1, (char)ch);
    return "";
}

inline std::string sura_readkey_extended_name(int ch) {
    switch (ch) {
        case 72: return "up";
        case 80: return "down";
        case 75: return "left";
        case 77: return "right";
        case 71: return "home";
        case 79: return "end";
        case 73: return "pageup";
        case 81: return "pagedown";
        case 82: return "insert";
        case 83: return "delete";
        default:
            if (ch >= 59 && ch <= 68) return "f" + std::to_string(ch - 58);
            if (ch >= 133 && ch <= 134) return "f" + std::to_string(ch - 122);
            return "";
    }
}
#endif

#ifdef _WIN32
inline void sura_window_pump();
#endif

inline bool sura_windows_key_is_down(int vk) {
#ifdef _WIN32
    if (vk <= 0 || vk >= 256) return false;
    sura_window_pump();

    bool message_down = sura_window_key_state_down(vk);
    SHORT async_state = GetAsyncKeyState(vk);
    bool async_down = (async_state & 0x8000) != 0;
    bool async_pressed_since_last_poll = (async_state & 0x0001) != 0;
    SHORT queue_state = GetKeyState(vk);
    bool queue_down = (queue_state & 0x8000) != 0;

    BYTE keyboard_state[256] {};
    bool keyboard_down = false;
    if (GetKeyboardState(keyboard_state)) {
        keyboard_down = (keyboard_state[vk] & 0x80) != 0;
    }

    return message_down || async_down || async_pressed_since_last_poll || queue_down || keyboard_down;
#else
    (void)vk;
    return false;
#endif
}

inline Value b_key_down(const Value* args, int nargs, int l) {
    need_args("key_down", nargs, 1, 1, l);
    std::string key = need_str("key_down", args[0], 0, l);
#ifdef _WIN32
    int vk = sura_key_virtual_code(key);
    if (vk == 0) return Value(false);
    return Value(sura_windows_key_is_down(vk));
#else
    (void)key;
    return Value(false);
#endif
}

inline Value b_readkey_timeout(const Value* args, int nargs, int l) {
    need_args("readkey_timeout", nargs, 1, 1, l);
    int ms = (int)std::max(0.0, std::floor(need_num("readkey_timeout", args[0], 0, l)));
#ifdef _WIN32
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        sura_window_pump();
        std::string window_key = sura_window_pop_key_event();
        if (!window_key.empty()) return Value(window_key);
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) return Value(sura_readkey_extended_name(_getch()));
            return Value(sura_readkey_name(ch));
        }
        if (ms == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return Value(std::string(""));
#else
    struct termios oldt {};
    struct termios raw {};
    bool raw_set = false;
    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        raw = oldt;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        raw_set = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv {};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    std::string out;
    if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) > 0) {
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) == 1) out.assign(1, ch);
    }
    if (raw_set) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return Value(out);
#endif
}

inline Value b_readkey(const Value*, int nargs, int l) {
    need_args("readkey", nargs, 0, 0, l);
#ifdef _WIN32
    for (;;) {
        sura_window_pump();
        std::string window_key = sura_window_pop_key_event();
        if (!window_key.empty()) return Value(window_key);
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) return Value(sura_readkey_extended_name(_getch()));
            return Value(sura_readkey_name(ch));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#else
    struct termios oldt {};
    struct termios raw {};
    bool raw_set = false;
    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        raw = oldt;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        raw_set = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    char ch = 0;
    std::string out;
    if (read(STDIN_FILENO, &ch, 1) == 1) out.assign(1, ch);
    if (raw_set) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return Value(out);
#endif
}

#ifdef _WIN32
inline bool sura_stdout_is_rex_anomaly(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    return trimmed.rfind("[0x", 0) == 0 &&
        trimmed.find("] ANOMALY: use of REX.w is meaningless (default operand size is 64)") != std::string::npos;
}

inline bool sura_stdout_is_possible_rex_anomaly_prefix(const std::string& text) {
    static const std::string prefix = "[0x";
    if (text.empty()) return false;
    if (text.size() < prefix.size()) {
        return prefix.compare(0, text.size(), text) == 0;
    }
    return text.rfind(prefix, 0) == 0;
}

inline void sura_stdout_write_all(int fd, const char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        int chunk = (int)std::min<size_t>(size - offset, 32767);
        int wrote = _write(fd, data + offset, chunk);
        if (wrote <= 0) break;
        offset += (size_t)wrote;
    }
}

inline void sura_stdout_forward_line(int out_fd, const std::string& line) {
    if (!sura_stdout_is_rex_anomaly(line)) {
        sura_stdout_write_all(out_fd, line.data(), line.size());
    }
}

inline void sura_stdout_filter_worker(int read_fd, int out_fd) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        int count = _read(read_fd, buffer, sizeof(buffer));
        if (count <= 0) break;
        pending.append(buffer, buffer + count);

        for (;;) {
            size_t newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline + 1);
            sura_stdout_forward_line(out_fd, line);
            pending.erase(0, newline + 1);
        }

        if (!pending.empty() && !sura_stdout_is_possible_rex_anomaly_prefix(pending)) {
            sura_stdout_write_all(out_fd, pending.data(), pending.size());
            pending.clear();
        }
    }

    if (!pending.empty()) {
        sura_stdout_forward_line(out_fd, pending);
    }
    _close(read_fd);
    _close(out_fd);
}

inline void sura_start_stdout_anomaly_filter() {
    static std::once_flag once;
    std::call_once(once, []() {
        std::cout.flush();
        std::fflush(stdout);

        int original = _dup(1);
        if (original < 0) return;

        int pipefd[2] = {-1, -1};
        if (_pipe(pipefd, 16384, _O_BINARY) != 0) {
            _close(original);
            return;
        }

        if (_dup2(pipefd[1], 1) != 0) {
            _close(pipefd[0]);
            _close(pipefd[1]);
            _close(original);
            return;
        }

        _close(pipefd[1]);
        std::thread(sura_stdout_filter_worker, pipefd[0], original).detach();
        std::atexit([]() {
            std::cout.flush();
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    });
}

struct SuraWindowState {
    HWND hwnd = nullptr;
    HDC back_dc = nullptr;
    HBITMAP back_bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    int width = 0;
    int height = 0;
    bool open = false;
};

inline SuraWindowState& sura_window_state() {
    static SuraWindowState state;
    return state;
}

inline void sura_window_release_backbuffer(SuraWindowState& state) {
    if (state.back_dc && state.old_bitmap) {
        SelectObject(state.back_dc, state.old_bitmap);
    }
    if (state.back_bitmap) {
        DeleteObject(state.back_bitmap);
    }
    if (state.back_dc) {
        DeleteDC(state.back_dc);
    }
    state.back_dc = nullptr;
    state.back_bitmap = nullptr;
    state.old_bitmap = nullptr;
}

inline bool sura_window_create_backbuffer(HWND hwnd, int width, int height) {
    auto& state = sura_window_state();
    sura_window_release_backbuffer(state);

    HDC window_dc = GetDC(hwnd);
    if (!window_dc) return false;
    HDC mem_dc = CreateCompatibleDC(window_dc);
    HBITMAP bitmap = mem_dc ? CreateCompatibleBitmap(window_dc, width, height) : nullptr;
    HGDIOBJ old_bitmap = bitmap ? SelectObject(mem_dc, bitmap) : nullptr;
    ReleaseDC(hwnd, window_dc);

    if (!mem_dc || !bitmap || !old_bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (mem_dc) DeleteDC(mem_dc);
        return false;
    }

    state.back_dc = mem_dc;
    state.back_bitmap = bitmap;
    state.old_bitmap = old_bitmap;
    RECT rect {0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(state.back_dc, &rect, brush);
    DeleteObject(brush);
    return true;
}

inline HDC sura_window_draw_dc(HWND hwnd, bool& release_dc) {
    auto& state = sura_window_state();
    if (state.back_dc) {
        release_dc = false;
        return state.back_dc;
    }
    release_dc = true;
    return GetDC(hwnd);
}

inline void sura_window_finish_draw(HWND hwnd, HDC dc, bool release_dc) {
    if (release_dc && dc) {
        ReleaseDC(hwnd, dc);
    }
}

inline void sura_window_present(HWND hwnd, HDC target_dc) {
    auto& state = sura_window_state();
    if (!target_dc || !state.back_dc || state.width <= 0 || state.height <= 0) return;
    RECT client {};
    GetClientRect(hwnd, &client);
    int client_w = std::max(1L, client.right - client.left);
    int client_h = std::max(1L, client.bottom - client.top);
    SetStretchBltMode(target_dc, COLORONCOLOR);
    StretchBlt(target_dc, 0, 0, client_w, client_h,
               state.back_dc, 0, 0, state.width, state.height, SRCCOPY);
}

inline void sura_window_request_focus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
}

inline LRESULT CALLBACK sura_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        sura_window_state().open = false;
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        sura_window_set_key_state(wp, true);
        sura_window_push_key_event(wp);
        return 0;
    }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        sura_window_set_key_state(wp, false);
        return 0;
    }
    if (msg == WM_KILLFOCUS) {
        sura_window_clear_key_states();
        return 0;
    }
    if (msg == WM_ACTIVATEAPP && !wp) {
        sura_window_clear_key_states();
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
        SetFocus(hwnd);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) {
        SetFocus(hwnd);
        return MA_ACTIVATE;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        sura_window_request_focus(hwnd);
    }
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps {};
        HDC dc = BeginPaint(hwnd, &ps);
        sura_window_present(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_SIZE) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (sura_window_state().hwnd == hwnd) {
            sura_window_release_backbuffer(sura_window_state());
            sura_window_state().hwnd = nullptr;
            sura_window_state().open = false;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline bool sura_window_register_class() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc {};
    wc.lpfnWndProc = sura_window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SuraRuntimeWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

inline void sura_window_pump() {
    MSG msg {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            sura_window_state().open = false;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

inline int sura_color_component(const char* name, const Value& v, int idx, int line) {
    double raw = need_num(name, v, idx, line);
    return (int)std::clamp(std::llround(raw), 0LL, 255LL);
}

inline COLORREF sura_color_rgb(const char* name, const Value* args, int offset, int line) {
    return RGB(
        sura_color_component(name, args[offset], offset, line),
        sura_color_component(name, args[offset + 1], offset + 1, line),
        sura_color_component(name, args[offset + 2], offset + 2, line));
}

inline HWND sura_require_window(const char* name, int line) {
    HWND hwnd = sura_window_state().hwnd;
    if (!hwnd || !sura_window_state().open || !IsWindow(hwnd)) {
        throw JitThrow{std::string(name) + "(): call win_init(width, height, title) first", line};
    }
    return hwnd;
}
#endif

inline Value b_win_init(const Value* args, int nargs, int l) {
    need_args("win_init", nargs, 3, 3, l);
#ifdef _WIN32
    sura_start_stdout_anomaly_filter();
    int width = (int)std::max(1.0, std::floor(need_num("win_init", args[0], 0, l)));
    int height = (int)std::max(1.0, std::floor(need_num("win_init", args[1], 1, l)));
    std::wstring title = windows_path_bytes_to_wide(need_str("win_init", args[2], 2, l));
    if (!sura_window_register_class()) return Value(false);

    auto& state = sura_window_state();
    if (state.hwnd && IsWindow(state.hwnd)) {
        sura_window_release_backbuffer(state);
        DestroyWindow(state.hwnd);
    }

    RECT rect {0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, L"SuraRuntimeWindow", title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return Value(false);
    state.hwnd = hwnd;
    state.width = width;
    state.height = height;
    state.open = true;
    sura_window_clear_key_states();
    sura_window_create_backbuffer(hwnd, width, height);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    sura_window_request_focus(hwnd);
    sura_window_pump();
    return Value(true);
#else
    (void)args;
    return Value(false);
#endif
}

inline Value b_win_clear(const Value* args, int nargs, int l) {
    need_args("win_clear", nargs, 3, 3, l);
#ifdef _WIN32
    HWND hwnd = sura_require_window("win_clear", l);
    bool release_dc = false;
    HDC dc = sura_window_draw_dc(hwnd, release_dc);
    if (!dc) return Value::nil();
    RECT rect {};
    auto& state = sura_window_state();
    if (state.back_dc) {
        rect = RECT{0, 0, state.width, state.height};
    } else {
        GetClientRect(hwnd, &rect);
    }
    HBRUSH brush = CreateSolidBrush(sura_color_rgb("win_clear", args, 0, l));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    sura_window_finish_draw(hwnd, dc, release_dc);
#endif
    return Value::nil();
}

inline Value b_win_rect(const Value* args, int nargs, int l) {
    need_args("win_rect", nargs, 7, 7, l);
#ifdef _WIN32
    HWND hwnd = sura_require_window("win_rect", l);
    int x = (int)std::floor(need_num("win_rect", args[0], 0, l));
    int y = (int)std::floor(need_num("win_rect", args[1], 1, l));
    int w = (int)std::floor(need_num("win_rect", args[2], 2, l));
    int h = (int)std::floor(need_num("win_rect", args[3], 3, l));
    COLORREF color = sura_color_rgb("win_rect", args, 4, l);
    bool release_dc = false;
    HDC dc = sura_window_draw_dc(hwnd, release_dc);
    if (!dc) return Value::nil();
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Rectangle(dc, x, y, x + w, y + h);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    sura_window_finish_draw(hwnd, dc, release_dc);
#endif
    return Value::nil();
}

inline Value b_win_circle(const Value* args, int nargs, int l) {
    need_args("win_circle", nargs, 6, 6, l);
#ifdef _WIN32
    HWND hwnd = sura_require_window("win_circle", l);
    int x = (int)std::floor(need_num("win_circle", args[0], 0, l));
    int y = (int)std::floor(need_num("win_circle", args[1], 1, l));
    int radius = (int)std::max(0.0, std::floor(need_num("win_circle", args[2], 2, l)));
    COLORREF color = sura_color_rgb("win_circle", args, 3, l);
    bool release_dc = false;
    HDC dc = sura_window_draw_dc(hwnd, release_dc);
    if (!dc) return Value::nil();
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    sura_window_finish_draw(hwnd, dc, release_dc);
#endif
    return Value::nil();
}

inline Value b_win_line(const Value* args, int nargs, int l) {
    need_args("win_line", nargs, 7, 7, l);
#ifdef _WIN32
    HWND hwnd = sura_require_window("win_line", l);
    int x1 = (int)std::floor(need_num("win_line", args[0], 0, l));
    int y1 = (int)std::floor(need_num("win_line", args[1], 1, l));
    int x2 = (int)std::floor(need_num("win_line", args[2], 2, l));
    int y2 = (int)std::floor(need_num("win_line", args[3], 3, l));
    COLORREF color = sura_color_rgb("win_line", args, 4, l);
    bool release_dc = false;
    HDC dc = sura_window_draw_dc(hwnd, release_dc);
    if (!dc) return Value::nil();
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    sura_window_finish_draw(hwnd, dc, release_dc);
#endif
    return Value::nil();
}

inline Value b_win_text(const Value* args, int nargs, int l) {
    need_args("win_text", nargs, 6, 6, l);
#ifdef _WIN32
    HWND hwnd = sura_require_window("win_text", l);
    std::wstring text = windows_path_bytes_to_wide(need_str("win_text", args[0], 0, l));
    int x = (int)std::floor(need_num("win_text", args[1], 1, l));
    int y = (int)std::floor(need_num("win_text", args[2], 2, l));
    COLORREF color = sura_color_rgb("win_text", args, 3, l);
    bool release_dc = false;
    HDC dc = sura_window_draw_dc(hwnd, release_dc);
    if (!dc) return Value::nil();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.c_str(), (int)text.size());
    sura_window_finish_draw(hwnd, dc, release_dc);
#endif
    return Value::nil();
}

inline Value b_win_update(const Value*, int nargs, int l) {
    need_args("win_update", nargs, 0, 0, l);
#ifdef _WIN32
    sura_window_pump();
    HWND hwnd = sura_window_state().hwnd;
    if (hwnd && sura_window_state().open && IsWindow(hwnd) && sura_window_state().back_dc) {
        HDC dc = GetDC(hwnd);
        if (dc) {
            sura_window_present(hwnd, dc);
            ReleaseDC(hwnd, dc);
        }
    }
    return Value(hwnd && sura_window_state().open && IsWindow(hwnd));
#else
    return Value(false);
#endif
}

inline Value b_win_poll(const Value*, int nargs, int l) {
    need_args("win_poll", nargs, 0, 0, l);
#ifdef _WIN32
    sura_window_pump();
    HWND hwnd = sura_window_state().hwnd;
    return Value(hwnd && sura_window_state().open && IsWindow(hwnd));
#else
    return Value(false);
#endif
}

inline Value b_win_focus(const Value*, int nargs, int l) {
    need_args("win_focus", nargs, 0, 0, l);
#ifdef _WIN32
    HWND hwnd = sura_window_state().hwnd;
    if (!hwnd || !sura_window_state().open || !IsWindow(hwnd)) return Value(false);
    sura_window_request_focus(hwnd);
    sura_window_pump();
    return Value(GetForegroundWindow() == hwnd || GetFocus() == hwnd);
#else
    return Value(false);
#endif
}

inline Value b_win_close(const Value*, int nargs, int l) {
    need_args("win_close", nargs, 0, 0, l);
#ifdef _WIN32
    auto& state = sura_window_state();
    sura_window_release_backbuffer(state);
    sura_window_clear_key_states();
    if (state.hwnd && IsWindow(state.hwnd)) DestroyWindow(state.hwnd);
    state.hwnd = nullptr;
    state.open = false;
#endif
    return Value::nil();
}

inline std::pair<int, int> sura_mouse_position() {
#ifdef _WIN32
    POINT pt {};
    GetCursorPos(&pt);
    HWND hwnd = sura_window_state().hwnd;
    if (hwnd && IsWindow(hwnd)) ScreenToClient(hwnd, &pt);
    return {(int)pt.x, (int)pt.y};
#else
    return {0, 0};
#endif
}

inline Value b_mouse_pos(const Value*, int nargs, int l) {
    need_args("mouse_pos", nargs, 0, 0, l);
    auto pos = sura_mouse_position();
    Value out = Value::make_dict();
    out.as_dict()->elements["x"] = Value(pos.first);
    out.as_dict()->elements["y"] = Value(pos.second);
    return out;
}

inline Value b_mouse_x(const Value*, int nargs, int l) {
    need_args("mouse_x", nargs, 0, 0, l);
    return Value(sura_mouse_position().first);
}

inline Value b_mouse_y(const Value*, int nargs, int l) {
    need_args("mouse_y", nargs, 0, 0, l);
    return Value(sura_mouse_position().second);
}

inline Value b_mouse_down(const Value* args, int nargs, int l) {
    need_args("mouse_down", nargs, 1, 1, l);
    std::string button = sura_game_token(need_str("mouse_down", args[0], 0, l));
#ifdef _WIN32
    int vk = 0;
    if (button == "left" || button == "l") vk = VK_LBUTTON;
    else if (button == "right" || button == "r") vk = VK_RBUTTON;
    else if (button == "middle" || button == "mid" || button == "m") vk = VK_MBUTTON;
    else if (button == "x1") vk = VK_XBUTTON1;
    else if (button == "x2") vk = VK_XBUTTON2;
    else return Value(false);
    return Value(sura_windows_key_is_down(vk));
#else
    (void)button;
    return Value(false);
#endif
}

struct SuraConsoleGridState {
    int width = 0;
    int height = 0;
    std::vector<std::string> cells;
    std::vector<std::string> colors;
};

inline SuraConsoleGridState& sura_console_grid_state() {
    static SuraConsoleGridState state;
    return state;
}

inline std::string sura_grid_color_ansi(const Value& color, int line) {
    if (color.is_nil()) return "";
    if (color.is_num()) {
        int code = (int)std::clamp(std::llround(color.as_num()), 0LL, 255LL);
        return "\x1B[38;5;" + std::to_string(code) + "m";
    }
    int code = 0;
    std::string name = need_str("grid_set", color, 3, line);
    if (!console_try_color_code(name, false, code)) {
        throw JitThrow{"grid_set(): unknown color '" + name + "'", line};
    }
    return console_ansi_sequence({code});
}

inline void sura_console_grid_clear_cells(SuraConsoleGridState& grid) {
    size_t count = (size_t)std::max(0, grid.width) * (size_t)std::max(0, grid.height);
    grid.cells.assign(count, " ");
    grid.colors.assign(count, "");
}

inline Value b_grid_init(const Value* args, int nargs, int l) {
    need_args("grid_init", nargs, 2, 2, l);
    int width = (int)std::floor(need_num("grid_init", args[0], 0, l));
    int height = (int)std::floor(need_num("grid_init", args[1], 1, l));
    if (width <= 0 || height <= 0 || width > 400 || height > 200) {
        throw JitThrow{"grid_init(): width/height must be in range 1..400 and 1..200", l};
    }
    auto& grid = sura_console_grid_state();
    grid.width = width;
    grid.height = height;
    sura_console_grid_clear_cells(grid);
    std::cout << "\x1B[2J\x1B[H";
    std::cout.flush();
    return Value::nil();
}

inline Value b_grid_clear(const Value*, int nargs, int l) {
    need_args("grid_clear", nargs, 0, 0, l);
    auto& grid = sura_console_grid_state();
    if (grid.width <= 0 || grid.height <= 0) {
        std::cout << "\x1B[2J\x1B[H";
        std::cout.flush();
        return Value::nil();
    }
    sura_console_grid_clear_cells(grid);
    return Value::nil();
}

inline Value b_grid_set(const Value* args, int nargs, int l) {
    need_args("grid_set", nargs, 3, 4, l);
    int x = (int)std::floor(need_num("grid_set", args[0], 0, l));
    int y = (int)std::floor(need_num("grid_set", args[1], 1, l));
    std::string cell = args[2].to_str();
    if (cell.empty()) cell = " ";
    std::string ansi = nargs >= 4 ? sura_grid_color_ansi(args[3], l) : "";

    auto& grid = sura_console_grid_state();
    if (grid.width > 0 && grid.height > 0) {
        if (x < 0 || y < 0 || x >= grid.width || y >= grid.height) return Value::nil();
        size_t idx = (size_t)y * (size_t)grid.width + (size_t)x;
        grid.cells[idx] = cell;
        grid.colors[idx] = ansi;
        return Value::nil();
    }

    if (x < 0 || y < 0) return Value::nil();
    std::cout << "\x1B[" << (y + 1) << ";" << (x + 1) << "H";
    if (!ansi.empty()) std::cout << ansi;
    std::cout << cell;
    if (!ansi.empty()) std::cout << "\x1B[0m";
    std::cout.flush();
    return Value::nil();
}

inline Value b_grid_draw(const Value*, int nargs, int l) {
    need_args("grid_draw", nargs, 0, 0, l);
    auto& grid = sura_console_grid_state();
    if (grid.width <= 0 || grid.height <= 0) return Value::nil();

    std::ostringstream out;
    out << "\x1B[H";
    for (int y = 0; y < grid.height; ++y) {
        for (int x = 0; x < grid.width; ++x) {
            size_t idx = (size_t)y * (size_t)grid.width + (size_t)x;
            const std::string& ansi = grid.colors[idx];
            if (!ansi.empty()) out << ansi << grid.cells[idx] << "\x1B[0m";
            else out << grid.cells[idx];
        }
        if (y + 1 < grid.height) out << "\n";
    }
    std::cout << out.str();
    std::cout.flush();
    return Value::nil();
}

inline Value b_clock(const Value*, int, int) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return Value(std::chrono::duration<double>(now).count());
}

// input([prompt]) — read a line from stdin, return as string.
// Optional first arg is a prompt printed before reading (no newline).
// Returns an empty string at EOF.
inline Value b_input(const Value* args, int nargs, int) {
    if (nargs >= 1) {
        std::cout << args[0].to_str();
        std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) return Value(std::string(""));
    return Value(line);
}
inline Value b_type(const Value* a, int n, int l) {
    need_args("type", n, 1, 1, l);
    return Value(value_type_name(a[0]));
}

// ── Registry & dispatch ──────────────────────────────────────────

inline const std::unordered_map<std::string, BuiltinFn>& table() {
    static const std::unordered_map<std::string, BuiltinFn> T = {
        // Math
        {"sqrt", b_sqrt}, {"sin", b_sin}, {"cos", b_cos}, {"tan", b_tan},
        {"floor", b_floor}, {"ceil", b_ceil}, {"round", b_round}, {"abs", b_abs}, {"sign", b_sign}, {"pow", b_pow},
        {"random", b_random}, {"clamp", b_clamp}, {"min", b_min}, {"max", b_max},
        {"random_seed", b_random_seed}, {"random_int", b_random_int},
        {"random_float", b_random_float}, {"random_bool", b_random_bool},
        {"random_choice", b_random_choice}, {"random_shuffle", b_random_shuffle},
        {"random_bytes", b_random_bytes},
        {"uuid_v4", b_uuid_v4}, {"uuid", b_uuid_v4},
        // String
        {"split", b_split}, {"join", b_join}, {"trim", b_trim},
        {"upper", b_upper}, {"lower", b_lower}, {"string_upper", b_upper}, {"string_lower", b_lower},
        {"string_trim", b_trim},
        {"contains", b_contains}, {"startsWith", b_startsWith}, {"endsWith", b_endsWith},
        {"indexOf", b_indexOf},
        {"string_contains", b_contains}, {"string_indexOf", b_indexOf}, {"string_index_of", b_indexOf},
        {"string_startsWith", b_startsWith}, {"string_starts_with", b_startsWith},
        {"string_endsWith", b_endsWith}, {"string_ends_with", b_endsWith},
        {"substring", b_substring}, {"string_substring", b_substring},
        {"replace", b_replace}, {"string_replace", b_replace},
        {"string_slice", b_slice}, {"string_sub", b_slice},
        {"string_lines", b_string_lines}, {"string_words", b_string_words},
        {"string_repeat", b_string_repeat},
        {"string_pad_left", b_string_pad_left},
        {"string_pad_right", b_string_pad_right},
        {"text_chunks", b_text_chunks}, {"text_chunk", b_text_chunks},
        // Array / collection
        {"length", b_length},
        {"array_len", b_length}, {"array_length", b_length}, {"array_size", b_length},
        {"string_len", b_length}, {"string_length", b_length}, {"string_size", b_length},
        {"slice", b_slice}, {"sort", b_sort},
        {"reverse", b_reverse}, {"concat", b_concat},
        {"push", b_push}, {"pop", b_pop},
        {"array_sum", b_array_sum}, {"array_avg", b_array_avg},
        {"array_min", b_array_min}, {"array_max", b_array_max},
        {"array_unique", b_array_unique}, {"array_flatten", b_array_flatten},
        {"array_range", b_array_range}, {"array_chunk", b_array_chunk},
        {"array_zip", b_array_zip}, {"array_repeat", b_array_repeat},
        {"set_union", b_set_union}, {"set_intersection", b_set_intersection},
        {"set_difference", b_set_difference},
        {"set_symmetric_difference", b_set_symmetric_difference},
        {"set_is_subset", b_set_is_subset},
        {"set_is_superset", b_set_is_superset},
        // Type conversion
        {"to_int", b_to_int}, {"to_float", b_to_float},
        {"to_str", b_to_str}, {"to_bool", b_to_bool},
        // Error constructor + clone
        {"Error", b_Error},
        {"clone", b_clone},
        {"assert", b_assert}, {"assert_eq", b_assert_eq},
        {"assert_ne", b_assert_ne}, {"assert_neq", b_assert_ne},
        {"assert_contains", b_assert_contains}, {"assert_not_contains", b_assert_not_contains},
        {"assert_match", b_assert_match}, {"assert_type", b_assert_type},
        {"assert_len", b_assert_len}, {"assert_between", b_assert_between},
        {"assert_approx", b_assert_approx},
        {"check", b_check}, {"check_eq", b_check_eq}, {"check_match", b_check_match},
        {"test_summary", b_test_summary}, {"test_report", b_test_report},
        // File I/O
        {"file_read", b_file_read}, {"read_file", b_file_read},
        {"file_write", b_file_write}, {"write_file", b_file_write},
        {"file_read_bytes", b_file_read_bytes}, {"file_write_bytes", b_file_write_bytes},
        {"file_read_json", b_file_read_json}, {"file_write_json", b_file_write_json},
        {"file_sha256", b_file_sha256}, {"sha256_file", b_file_sha256},
        {"file_append", b_file_append}, {"append_file", b_file_append},
        {"file_exists", b_file_exists}, {"exists", b_file_exists},
        {"file_delete", b_file_delete}, {"delete_file", b_file_delete},
        {"file_remove_tree", b_file_remove_tree}, {"remove_tree", b_file_remove_tree},
        {"file_list", b_file_list}, {"list_dir", b_file_list},
        {"file_walk", b_file_walk}, {"walk_files", b_file_walk},
        {"file_glob", b_file_glob}, {"glob_files", b_file_glob},
        {"mkdir", b_mkdir}, {"cwd", b_cwd},
        {"path_join", b_path_join}, {"path_basename", b_path_basename},
        {"path_dirname", b_path_dirname}, {"path_ext", b_path_ext},
        {"path_stem", b_path_stem}, {"path_normalize", b_path_normalize},
        {"path_abs", b_path_abs}, {"path_relative", b_path_relative},
        {"file_is_dir", b_file_is_dir}, {"is_dir", b_file_is_dir},
        {"file_is_file", b_file_is_file}, {"is_file", b_file_is_file},
        {"file_info", b_file_info},
        {"file_size", b_file_size}, {"file_copy", b_file_copy},
        {"copy_file", b_file_copy}, {"file_move", b_file_move}, {"move_file", b_file_move},
        // JSON
        {"json_parse", b_json_parse}, {"json_try_parse", b_json_try_parse},
        {"json_stringify", b_json_stringify},
        {"json_pretty", b_json_pretty}, {"pretty_json", b_json_pretty},
        {"serialize", b_json_stringify}, {"deserialize", b_json_parse},
        {"jsonl_parse", b_jsonl_parse}, {"jsonl_stringify", b_jsonl_stringify},
        {"sse_parse", b_sse_parse}, {"sse_data", b_sse_data},
        {"csv_parse", b_csv_parse}, {"csv_stringify", b_csv_stringify},
        {"ini_parse", b_ini_parse}, {"ini_stringify", b_ini_stringify},
        {"json_path", b_json_path}, {"dict_get_path", b_dict_get_path},
        {"dict_keys", b_dict_keys}, {"dict_values", b_dict_values},
        {"dict_items", b_dict_items}, {"dict_merge", b_dict_merge},
        {"dict_pick", b_dict_pick}, {"dict_omit", b_dict_omit},
        {"json_has_path", b_json_has_path},
        {"json_merge_patch", b_json_merge_patch},
        {"json_delete_path", b_json_delete_path},
        {"json_set_path", b_json_set_path},
        {"pluck", b_pluck}, {"array_pluck", b_pluck},
        {"count_by", b_count_by}, {"array_count_by", b_count_by},
        {"group_by", b_group_by}, {"array_group_by", b_group_by},
        {"sort_by", b_sort_by}, {"array_sort_by", b_sort_by},
        {"template_render", b_template_render},
        {"schema_validate", b_schema_validate}, {"schema_errors", b_schema_errors},
        {"schema_to_json_schema", b_schema_to_json_schema},
        // Regex / datetime / crypto / db / CLI / logging
        {"regex_match", b_regex_match}, {"regex_replace", b_regex_replace},
        {"regex_find_all", b_regex_find_all}, {"regex_split", b_regex_split},
        {"regex_escape", b_regex_escape},
        {"regex_capture", b_regex_capture}, {"regex_captures", b_regex_captures},
        {"datetime_now", b_datetime_now}, {"datetime_format", b_datetime_format},
        {"datetime_utc_format", b_datetime_utc_format},
        {"datetime_parse", b_datetime_parse}, {"datetime_parts", b_datetime_parts},
        {"datetime_add", b_datetime_add}, {"datetime_diff", b_datetime_diff},
        {"timestamp", b_timestamp},
        {"sha256", b_sha256}, {"hmac_sha256", b_hmac_sha256},
        {"file_hmac_sha256", b_file_hmac_sha256}, {"hmac_sha256_file", b_file_hmac_sha256},
        {"crypto_random_bytes", b_crypto_random_bytes}, {"secure_random_bytes", b_crypto_random_bytes},
        {"crypto_random_hex", b_crypto_random_hex}, {"secure_random_hex", b_crypto_random_hex},
        {"constant_time_eq", b_constant_time_eq}, {"crypto_constant_time_eq", b_constant_time_eq},
        {"secure_compare", b_constant_time_eq},
        {"hex_encode", b_hex_encode}, {"hex_decode", b_hex_decode},
        {"base64_encode", b_base64_encode}, {"base64_decode", b_base64_decode},
        {"base64_url_encode", b_base64_url_encode}, {"base64_url_decode", b_base64_url_decode},
        {"url_encode", b_url_encode}, {"url_decode", b_url_decode},
        {"url_parse", b_url_parse}, {"url_build", b_url_build},
        {"query_build", b_query_build}, {"query_parse", b_query_parse},
        {"form_build", b_form_build}, {"form_parse", b_form_parse},
        {"auth_bearer", b_auth_bearer}, {"auth_basic", b_auth_basic},
        {"headers_merge", b_headers_merge},
        {"headers_get", b_headers_get},
        {"headers_has", b_headers_has},
        {"headers_redact", b_headers_redact},
        {"cookie_parse", b_cookie_parse},
        {"cookie_build", b_cookie_build},
        {"cookie_get", b_cookie_get},
        {"http_content_type", b_http_content_type},
        {"http_charset", b_http_charset},
        {"http_is_json", b_http_is_json},
        {"http_status_ok", b_http_status_ok},
        {"http_status_text", b_http_status_text},
        {"http_status_retryable", b_http_status_retryable},
        {"http_retry_after", b_http_retry_after},
        {"http_backoff_delays", b_http_backoff_delays},
        {"env_get", b_env_get}, {"env_require", b_env_require},
        {"env_set", b_env_set}, {"env_load", b_env_load},
        {"home_dir", b_home_dir}, {"temp_dir", b_temp_dir},
        {"path_separator", b_path_separator}, {"os_name", b_os_name},
        {"is_windows", b_is_windows},
        {"which", b_which}, {"cmd_exists", b_cmd_exists}, {"command_exists", b_cmd_exists},
        {"cmd_quote", b_cmd_quote}, {"cmd_join", b_cmd_join},
        {"db_set", b_db_set}, {"db_get", b_db_get},
        {"db_has", b_db_has}, {"db_delete", b_db_delete},
        {"db_keys", b_db_keys}, {"db_all", b_db_all},
        {"db_insert", b_db_insert}, {"db_find", b_db_find},
        {"db_count", b_db_count}, {"db_update", b_db_update},
        {"db_remove", b_db_remove}, {"db_query", b_db_query},
        {"cli_parse", b_cli_parse},
        {"argv", b_argv}, {"argc", b_argc}, {"script_name", b_script_name},
        {"log_set_file", b_log_set_file}, {"log_set_json", b_log_set_json},
        {"log_set_level", b_log_set_level}, {"log_get_level", b_log_get_level},
        {"log_level", b_log_level},
        {"log_event", b_log_event}, {"log_debug", b_log_debug},
        {"log_info", b_log_info}, {"log_warn", b_log_warn}, {"log_error", b_log_error},
        {"console_log", b_console_log},
        {"console_print", b_console_log},
        {"console_write", b_console_write}, {"console_write_line", b_console_write_line},
        {"console_writeln", b_console_write_line}, {"console_println", b_console_write_line},
        {"console_line", b_console_write_line},
        {"console_info", b_console_info},
        {"console_debug", b_console_debug}, {"console_warn", b_console_warn},
        {"console_warning", b_console_warn}, {"console_error", b_console_error},
        {"console_exception", b_console_exception},
        {"console_raw", b_console_raw},
        {"console_flush", b_console_flush},
        {"console_json", b_console_json},
        {"console_inspect", b_console_inspect},
        {"console_hrtime", b_console_hrtime},
        {"console_beep", b_console_beep},
        {"console_clear", b_console_clear}, {"cls", b_console_clear},
        {"console_assert", b_console_assert},
        {"console_time", b_console_time}, {"console_time_end", b_console_time_end},
        {"console_time_log", b_console_time_log}, {"console_time_stamp", b_console_time_stamp},
        {"console_count", b_console_count}, {"console_count_reset", b_console_count_reset},
        {"console_table", b_console_table}, {"console_dir", b_console_dir},
        {"console_dirxml", b_console_dirxml},
        {"console_trace", b_console_trace}, {"console_group", b_console_group},
        {"console_group_collapsed", b_console_group},
        {"console_group_end", b_console_group_end},
        {"console_profile", b_console_profile}, {"console_profile_end", b_console_profile_end},
        {"console_style", b_console_style},
        {"console_color", b_console_color}, {"console_colour", b_console_color},
        {"console_strip_ansi", b_console_strip_ansi},
        {"console_set_color", b_console_set_color}, {"console_set_colour", b_console_set_color},
        {"console_reset_color", b_console_reset_color}, {"console_reset_colour", b_console_reset_color},
        {"console_is_tty", b_console_is_tty}, {"console_width", b_console_width},
        {"console_height", b_console_height}, {"console_size", b_console_size},
        {"console_status", b_console_status},
        {"console_input", b_input}, {"console_read_line", b_input},
        {"console_readline", b_input}, {"console_readLine", b_input},
        {"console_prompt", b_input},
        {"key_down", b_key_down}, {"readkey", b_readkey},
        {"readkey_timeout", b_readkey_timeout}, {"silent", b_silent},
        {"win_init", b_win_init}, {"win_clear", b_win_clear},
        {"win_rect", b_win_rect}, {"win_circle", b_win_circle},
        {"win_line", b_win_line}, {"win_text", b_win_text},
        {"win_update", b_win_update}, {"win_poll", b_win_poll},
        {"win_close", b_win_close},
        {"win_focus", b_win_focus},
        {"mouse_pos", b_mouse_pos}, {"mouse_x", b_mouse_x},
        {"mouse_y", b_mouse_y}, {"mouse_down", b_mouse_down},
        {"grid_init", b_grid_init}, {"grid_clear", b_grid_clear},
        {"grid_set", b_grid_set}, {"grid_draw", b_grid_draw},
        {"vec_add", b_vec_add}, {"vector_add", b_vec_add},
        {"vec_dot", b_vec_dot}, {"vector_dot", b_vec_dot},
        {"vec_scale", b_vec_scale}, {"vector_scale", b_vec_scale},
        {"vec_norm", b_vec_norm}, {"vector_norm", b_vec_norm},
        {"vec3", b_vec3}, {"vector3", b_vec3},
        {"vec3_add", b_vec3_add}, {"vector3_add", b_vec3_add},
        {"vec3_sub", b_vec3_sub}, {"vector3_sub", b_vec3_sub},
        {"vec3_dot", b_vec3_dot}, {"vector3_dot", b_vec3_dot},
        {"vec3_cross", b_vec3_cross}, {"vector3_cross", b_vec3_cross},
        {"vec3_scale", b_vec3_scale}, {"vector3_scale", b_vec3_scale},
        {"vec3_norm", b_vec3_norm}, {"vector3_norm", b_vec3_norm},
        {"vec3_normalize", b_vec3_normalize}, {"vector3_normalize", b_vec3_normalize},
        {"vec3_distance", b_vec3_distance}, {"vector3_distance", b_vec3_distance},
        {"vec3_neg", b_vec3_neg}, {"vector3_neg", b_vec3_neg},
        {"vec3_lerp", b_vec3_lerp}, {"vector3_lerp", b_vec3_lerp},
        {"vec3_midpoint", b_vec3_midpoint}, {"vector3_midpoint", b_vec3_midpoint},
        {"vec3_project", b_vec3_project}, {"vector3_project", b_vec3_project},
        {"vec3_reject", b_vec3_reject}, {"vector3_reject", b_vec3_reject},
        {"vec3_reflect", b_vec3_reflect}, {"vector3_reflect", b_vec3_reflect},
        {"vec3_angle", b_vec3_angle}, {"vector3_angle", b_vec3_angle},
        {"vec3_transform4", b_vec3_transform4}, {"vector3_transform4", b_vec3_transform4},
        {"mat4_identity", b_mat4_identity}, {"mat4_translate", b_mat4_translate},
        {"mat4_scale", b_mat4_scale}, {"mat4_rotate_y", b_mat4_rotate_y},
        {"mat4_mul", b_mat4_mul},
        {"mesh_cube", b_mesh_cube}, {"mesh_transform4", b_mesh_transform4},
        {"mesh_bounds", b_mesh_bounds}, {"mesh_face_normals", b_mesh_face_normals},
        {"camera_project", b_camera_project},
        {"vec_cosine", b_vector_cosine}, {"vector_cosine", b_vector_cosine},
        {"vec_normalize", b_vector_normalize}, {"vector_normalize", b_vector_normalize},
        {"vector_search", b_vector_search}, {"embedding_search", b_vector_search},
        {"rag_context", b_rag_context},
        {"rag_sources", b_rag_sources}, {"rag_prepare", b_rag_prepare},
        {"tensor_shape", b_tensor_shape}, {"tensor_zeros", b_tensor_zeros},
        {"tensor_fill", b_tensor_fill}, {"tensor_add", b_tensor_add},
        {"tensor_mul", b_tensor_mul}, {"tensor_clip", b_tensor_clip},
        {"tensor_flatten", b_tensor_flatten},
        {"tensor_sum", b_tensor_sum}, {"tensor_mean", b_tensor_mean},
        {"tensor_variance", b_tensor_variance}, {"tensor_std", b_tensor_std},
        {"tensor_min", b_tensor_min}, {"tensor_max", b_tensor_max},
        {"tensor_argmin", b_tensor_argmin}, {"tensor_argmax", b_tensor_argmax},
        {"tensor_zscore", b_tensor_zscore}, {"tensor_softmax", b_tensor_softmax},
        {"tensor_transpose", b_tensor_transpose}, {"tensor_matmul", b_tensor_matmul},
        {"nn_mlp", b_nn_mlp}, {"nn_forward", b_nn_forward},
        {"nn_predict", b_nn_predict}, {"nn_train", b_nn_train},
        {"nn_classify", b_nn_classify}, {"nn_evaluate", b_nn_evaluate},
        {"nn_summary", b_nn_summary}, {"nn_one_hot", b_nn_one_hot},
        {"nn_fit_standardizer", b_nn_fit_standardizer},
        {"nn_standardize", b_nn_standardize}, {"nn_split", b_nn_split},
        {"nn_save", b_nn_save}, {"nn_load", b_nn_load},
        {"autograd_tensor", b_autograd_tensor},
        {"autograd_parameter", b_autograd_parameter},
        {"autograd_zeros", b_autograd_zeros}, {"autograd_ones", b_autograd_ones},
        {"autograd_randn", b_autograd_randn},
        {"autograd_data", b_autograd_data}, {"autograd_grad", b_autograd_grad},
        {"autograd_grad_info", b_autograd_grad_info},
        {"autograd_dtype", b_autograd_dtype}, {"autograd_device", b_autograd_device},
        {"autograd_to", b_autograd_to},
        {"autograd_storage_bytes", b_autograd_storage_bytes},
        {"autograd_cast", b_autograd_cast},
        {"autograd_shape", b_autograd_shape}, {"autograd_numel", b_autograd_numel},
        {"autograd_limits", b_autograd_limits},
        {"autograd_autocast", b_autograd_autocast},
        {"autograd_cuda_available", b_autograd_cuda_available},
        {"autograd_cuda_info", b_autograd_cuda_info},
        {"autograd_cuda_stats", b_autograd_cuda_stats},
        {"autograd_cuda_reset_stats", b_autograd_cuda_reset_stats},
        {"autograd_cuda_synchronize", b_autograd_cuda_synchronize},
        {"autograd_requires_grad", b_autograd_requires_grad},
        {"autograd_item", b_autograd_item}, {"autograd_detach", b_autograd_detach},
        {"autograd_set_requires_grad", b_autograd_set_requires_grad},
        {"autograd_add", b_autograd_add}, {"autograd_sub", b_autograd_sub},
        {"autograd_mul", b_autograd_mul}, {"autograd_div", b_autograd_div},
        {"autograd_neg", b_autograd_neg}, {"autograd_reshape", b_autograd_reshape},
        {"autograd_matmul", b_autograd_matmul},
        {"autograd_transpose", b_autograd_transpose}, {"autograd_linear", b_autograd_linear},
        {"autograd_relu", b_autograd_relu}, {"autograd_tanh", b_autograd_tanh},
        {"autograd_sigmoid", b_autograd_sigmoid}, {"autograd_gelu", b_autograd_gelu},
        {"autograd_layer_norm", b_autograd_layer_norm},
        {"autograd_embedding", b_autograd_embedding},
        {"autograd_causal_attention", b_autograd_causal_attention},
        {"autograd_softmax", b_autograd_softmax},
        {"autograd_sum", b_autograd_sum}, {"autograd_mean", b_autograd_mean},
        {"autograd_mse", b_autograd_mse}, {"autograd_bce", b_autograd_bce},
        {"autograd_bce_logits", b_autograd_bce_logits},
        {"autograd_cross_entropy", b_autograd_cross_entropy},
        {"autograd_cross_entropy_ids", b_autograd_cross_entropy_ids},
        {"autograd_backward", b_autograd_backward},
        {"autograd_backward_scaled", b_autograd_backward_scaled},
        {"autograd_zero_grad", b_autograd_zero_grad},
        {"autograd_unscale_gradients", b_autograd_unscale_gradients},
        {"autograd_sgd", b_autograd_sgd}, {"autograd_adam", b_autograd_adam},
        {"autograd_reset_optimizer", b_autograd_reset_optimizer},
        {"autograd_grad_norm", b_autograd_grad_norm},
        {"autograd_clip_grad_norm", b_autograd_clip_grad_norm},
        {"autograd_save_checkpoint", b_autograd_save_checkpoint},
        {"autograd_load_checkpoint", b_autograd_load_checkpoint},
        {"autograd_save_safetensors", b_autograd_save_safetensors},
        {"autograd_load_safetensors", b_autograd_load_safetensors},
        {"autograd_save_onnx_weights", b_autograd_save_onnx_weights},
        {"autograd_load_onnx_weights", b_autograd_load_onnx_weights},
        {"autograd_run_onnx", b_autograd_run_onnx},
        {"autograd_all_reduce_gradients", b_autograd_all_reduce_gradients},
        {"tokenizer_byte", b_tokenizer_byte},
        {"tokenizer_train_bpe", b_tokenizer_train_bpe},
        {"tokenizer_encode", b_tokenizer_encode},
        {"tokenizer_decode", b_tokenizer_decode},
        {"tokenizer_info", b_tokenizer_info},
        {"tokenizer_save", b_tokenizer_save},
        {"tokenizer_load", b_tokenizer_load},
        {"dataset_pack_text", b_dataset_pack_text},
        {"dataset_open", b_dataset_open},
        {"dataset_next", b_dataset_next},
        {"dataset_reset", b_dataset_reset},
        {"dataset_close", b_dataset_close},
        {"dataset_info", b_dataset_info},
        {"media_available", b_media_ffmpeg_available},
        {"media_ffmpeg_available", b_media_ffmpeg_available},
        {"media_ascii_frames", b_media_video_to_text},
        {"media_frame_to_text", b_media_frame_to_text},
        {"media_video_to_text", b_media_video_to_text},
        {"media_video_text_frames", b_media_video_text_frames},
        {"stream_from", b_stream_from}, {"stream_next", b_stream_next},
        {"stream_collect", b_stream_collect}, {"stream_take", b_stream_take},
        {"stream_batch", b_stream_batch},
        {"stream_map", b_stream_map}, {"stream_filter", b_stream_filter},
        {"stream_window", b_stream_window},
        {"stream_skip", b_stream_skip}, {"stream_count", b_stream_count},
        {"stream_join", b_stream_join}, {"stream_sum", b_stream_sum},
        {"stream_avg", b_stream_avg}, {"stream_lines", b_stream_lines},
        {"llm_message", b_llm_message}, {"llm_messages", b_llm_messages},
        {"rag_messages", b_rag_messages},
        {"llm_request", b_llm_request}, {"llm_request_json", b_llm_request_json},
        {"llm_response_schema", b_llm_response_schema},
        {"llm_request_schema", b_llm_request_schema},
        {"llm_request_schema_json", b_llm_request_schema_json},
        {"llm_tools", b_llm_tools},
        {"llm_tool_schemas", b_llm_tools},
        {"llm_request_tools", b_llm_request_tools},
        {"llm_request_tools_json", b_llm_request_tools_json},
        {"llm_request_tools_schema", b_llm_request_tools_schema},
        {"llm_request_tools_schema_json", b_llm_request_tools_schema_json},
        {"llm_extract_text", b_llm_extract_text},
        {"llm_extract_json", b_llm_extract_json},
        {"llm_usage", b_llm_usage},
        {"llm_cost", b_llm_cost},
        {"llm_budget", b_llm_budget},
        {"llm_tool_calls", b_llm_tool_calls}, {"llm_tool_result", b_llm_tool_result},
        {"llm_run_tools", b_llm_run_tools}, {"llm_next_messages", b_llm_next_messages},
        {"llm_next_request", b_llm_next_request}, {"llm_next_request_json", b_llm_next_request_json},
        {"llm_next_schema_request", b_llm_next_schema_request},
        {"llm_next_schema_request_json", b_llm_next_schema_request_json},
        {"llm_stream_text", b_llm_stream_text},
        {"llm_chat", b_llm_chat},
        {"llm_chat_request", b_llm_chat_request},
        // Network and async process helpers
        {"http_get", b_http_get}, {"http_json", b_http_json},
        {"http_post", b_http_post}, {"http_request", b_http_request},
        {"http_request_full", b_http_request_full}, {"http_request_retry", b_http_request_retry},
        {"http_request_retry_json", b_http_request_retry_json},
        {"http_request_retry_json_checked", b_http_request_retry_json_checked},
        {"http_request_json", b_http_request_json},
        {"http_request_json_checked", b_http_request_json_checked},
        {"tool_call", b_tool_call}, {"tool", b_tool_call},
        {"tool_spec", b_tool_spec}, {"tool_validate", b_tool_validate},
        {"tool_schema", b_tool_schema},
        {"tool_allowed", b_tool_allowed}, {"tool_call_policy", b_tool_call_policy},
        {"tool_list", b_tool_list},
        {"http_serve_static", b_http_serve_static},
        {"http_serve_routes", b_http_serve_routes},
        {"http_server_url", b_http_server_url}, {"http_server_stop", b_http_server_stop},
        {"file_lines", b_file_lines},
        {"python_eval", b_python_eval}, {"python_call", b_python_call},
        {"python_call_json", b_python_call_json},
        {"python_available", b_python_available}, {"python_executable", b_python_executable},
        {"ffi_load", b_ffi_load}, {"ffi_call", b_ffi_call},
        {"plugin_load", b_plugin_load}, {"plugin_call", b_plugin_call},
        {"plugin_load_manifest", b_plugin_load_manifest},
        {"plugin_info", b_plugin_info}, {"plugin_unload", b_plugin_unload},
        {"cmd_run", b_cmd_run}, {"cmd_run_checked", b_cmd_run_checked},
        {"async_cmd", b_async_cmd}, {"async_ready", b_async_ready},
        {"async_http_get", b_async_http_get}, {"async_http_request", b_async_http_request},
        {"async_sleep", b_async_sleep}, {"async_sura", b_async_sura},
        {"async_status", b_async_status}, {"async_pending", b_async_pending},
        {"async_forget", b_async_forget}, {"async_cleanup", b_async_cleanup},
        {"async_cancel", b_async_cancel}, {"async_cancelled", b_async_cancelled},
        {"async_configure", b_async_configure}, {"async_limits", b_async_limits},
        {"async_scope_open", b_async_scope_open},
        {"async_scope_attach", b_async_scope_attach},
        {"async_scope_cancel", b_async_scope_cancel},
        {"async_scope_status", b_async_scope_status},
        {"async_scope_close", b_async_scope_close},
        {"async_scope_join", b_async_scope_join},
        {"async_await", b_async_await}, {"async_await_timeout", b_async_await_timeout},
        {"async_ready_all", b_async_ready_all},
        {"async_any", b_async_any}, {"async_all", b_async_all},
        {"async_all_timeout", b_async_all_timeout}, {"task", b_async_cmd},
        {"await", b_async_await}, {"await_timeout", b_async_await_timeout},
        {"await_any", b_async_any}, {"await_all", b_async_all},
        {"await_all_timeout", b_async_all_timeout}, {"sleep_ms", b_sleep_ms},
        {"wait", b_wait}, {"sleep", b_wait},
        // Pre-existing
        {"print", b_print}, {"print_n", b_print_n}, {"print_no_nl", b_print_n},
        {"clock", b_clock}, {"type", b_type},
        // Interactive I/O
        {"input", b_input},
    };
    return T;
}

// Returns true iff `name` is a known stdlib function. Writes result to `out`.
inline bool try_dispatch(const std::string& name, const Value* args, int nargs, int line, Value& out) {
    auto& t = table();
    auto it = t.find(name);
    if (it == t.end()) return false;
    out = it->second(args, nargs, line);
    return true;
}

// Enumerates all names (used by typo-suggestion).
inline std::vector<std::string> names() {
    std::vector<std::string> out;
    for (auto& [k, _] : table()) out.push_back(k);
    return out;
}

} // namespace SuraStd
