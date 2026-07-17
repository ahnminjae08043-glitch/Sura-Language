#pragma once
#include "jit_op.hpp"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <ctime>
#include <cctype>
#include <chrono>
#include <random>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

// ================================================================
//  Sura Bytecode I/O — .sura.bc binary format
//  Magic: "SURB" + version(1) + payload sections
// ================================================================

static constexpr uint8_t  BC_MAGIC[4] = {'S','U','R','B'};
static constexpr uint8_t  BC_VERSION_LEGACY = 2;
static constexpr uint8_t  BC_VERSION  = 3;
static constexpr uint8_t  RELEASE_MAGIC[4] = {'S','U','R','P'};
static constexpr uint8_t  RELEASE_VERSION  = 5;
static constexpr uint8_t  RELEASE_VERSION_LICENSED = 4;
static constexpr uint8_t  RELEASE_VERSION_METADATA = 3;
static constexpr uint8_t  RELEASE_VERSION_KEYED = 2;
static constexpr uint8_t  RELEASE_VERSION_LEGACY = 1;
static constexpr uint8_t  RELEASE_FLAG_KEYED = 1;
static constexpr uint8_t  RELEASE_FLAG_METADATA = 2;
static constexpr uint8_t  RELEASE_FLAG_LICENSED = 4;
static constexpr uint64_t RELEASE_STREAM_KEY = 0x9E3779B97F4A7C15ULL;

// Bytecode and release packages are untrusted input.  These limits are high
// enough for normal programs, while preventing a four-byte forged length from
// turning into a multi-gigabyte allocation before validation can run.
static constexpr uint64_t BC_MAX_FILE_BYTES       = 256ULL * 1024ULL * 1024ULL;
static constexpr uint32_t BC_MAX_STRING_BYTES     = 16U * 1024U * 1024U;
static constexpr uint32_t BC_MAX_SECTION_ENTRIES  = 1U * 1024U * 1024U;
static constexpr uint32_t BC_MAX_CODE_ENTRIES     = 8U * 1024U * 1024U;
static constexpr uint32_t BC_MAX_CALLABLE_ENTRIES = 256U * 1024U;
static constexpr uint32_t BC_MAX_CLASS_ENTRIES    = 64U * 1024U;
static constexpr uint32_t BC_MAX_RELEASE_TEXT     = 64U * 1024U;

// Value tags for serialization (only constant-legal types)
static constexpr uint8_t  BCTAG_NIL    = 0;
static constexpr uint8_t  BCTAG_BOOL   = 1;
static constexpr uint8_t  BCTAG_NUMBER = 2;
static constexpr uint8_t  BCTAG_STRING = 3;  // index into strings table

// ── Low-level helpers ────────────────────────────────────────────

static void w8(std::ostream& o, uint8_t v)   { o.write((char*)&v, 1); }
static void w16(std::ostream& o, uint16_t v) { o.write((char*)&v, 2); }
static void w32(std::ostream& o, uint32_t v) { o.write((char*)&v, 4); }
static void w64(std::ostream& o, uint64_t v) { o.write((char*)&v, 8); }

static void bytecode_read_exact(std::istream& i, void* out, size_t size,
                                const char* what = "bytecode data") {
    if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error(std::string("Invalid ") + what + " length");
    if (size == 0) return;
    i.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    if (!i || static_cast<size_t>(i.gcount()) != size)
        throw std::runtime_error(std::string("Truncated ") + what);
}

static uint8_t  r8(std::istream& i)  { uint8_t  v = 0; bytecode_read_exact(i, &v, 1); return v; }
static uint16_t r16(std::istream& i) { uint16_t v = 0; bytecode_read_exact(i, &v, 2); return v; }
static uint32_t r32(std::istream& i) { uint32_t v = 0; bytecode_read_exact(i, &v, 4); return v; }
static uint64_t r64(std::istream& i) { uint64_t v = 0; bytecode_read_exact(i, &v, 8); return v; }

static uint32_t bytecode_read_count(std::istream& i, uint32_t limit, const char* what) {
    uint32_t count = r32(i);
    if (count > limit)
        throw std::runtime_error(std::string("Bytecode ") + what + " count exceeds limit");
    return count;
}

static void wstr(std::ostream& o, const std::string& s) {
    w32(o, (uint32_t)s.size());
    o.write(s.data(), s.size());
}
static std::string rstr(std::istream& i, uint32_t limit = BC_MAX_STRING_BYTES,
                        const char* what = "string") {
    uint32_t len = r32(i);
    if (len > limit)
        throw std::runtime_error(std::string("Bytecode ") + what + " length exceeds limit");
    std::string s(len, '\0');
    bytecode_read_exact(i, s.data(), len, what);
    return s;
}

static void wstrlist(std::ostream& o, const std::vector<std::string>& v) {
    w32(o, (uint32_t)v.size());
    for (auto& s : v) wstr(o, s);
}
static std::vector<std::string> rstrlist(std::istream& i) {
    uint32_t n = bytecode_read_count(i, BC_MAX_SECTION_ENTRIES, "string table");
    std::vector<std::string> v(n);
    for (auto& s : v) s = rstr(i);
    return v;
}

// Serialize a Value that can appear in constants/defaults (num, bool, nil, string)
// For strings: stores the string content directly (not an index, to keep it self-contained)
static void wvalue(std::ostream& o, const Value& v) {
    if (v.is_nil())  { w8(o, BCTAG_NIL); return; }
    if (v.is_bool()) { w8(o, BCTAG_BOOL); w8(o, v.as_bool() ? 1 : 0); return; }
    if (v.is_num())  { w8(o, BCTAG_NUMBER); double d = v.as_num(); o.write((char*)&d, 8); return; }
    if (v.is_str())  { w8(o, BCTAG_STRING); wstr(o, v.as_str()); return; }
    // Fallback: store nil for unsupported types (closure, instance, etc.)
    w8(o, BCTAG_NIL);
}
static Value rvalue(std::istream& i) {
    uint8_t tag = r8(i);
    switch (tag) {
        case BCTAG_NIL:    return Value::nil();
        case BCTAG_BOOL: {
            uint8_t value = r8(i);
            if (value > 1) throw std::runtime_error("Invalid bytecode boolean value");
            return Value(value != 0);
        }
        case BCTAG_NUMBER: {
            double d = 0.0;
            bytecode_read_exact(i, &d, sizeof(d), "bytecode number");
            if (!std::isfinite(d))
                throw std::runtime_error("Non-finite bytecode number constant");
            return Value(d);
        }
        case BCTAG_STRING: return Value(rstr(i));
        default: throw std::runtime_error("Invalid bytecode value tag " + std::to_string(tag));
    }
}
static void wvaluelist(std::ostream& o, const std::vector<Value>& v) {
    w32(o, (uint32_t)v.size());
    for (auto& val : v) wvalue(o, val);
}
static std::vector<Value> rvaluelist(std::istream& i) {
    uint32_t n = bytecode_read_count(i, BC_MAX_SECTION_ENTRIES, "value table");
    std::vector<Value> v;
    v.reserve(n);
    for (uint32_t j = 0; j < n; ++j) v.push_back(rvalue(i));
    return v;
}

// ── JitInst ─────────────────────────────────────────────────────

static void winst(std::ostream& o, const JitInst& ins) {
    w8(o,  (uint8_t)ins.op);
    w16(o, ins.a);
    w16(o, ins.b);
    w16(o, ins.c);
    w32(o, (uint32_t)ins.operand);
    w32(o, (uint32_t)ins.str_idx);
    w32(o, (uint32_t)ins.line);
    // ic_cache is runtime-only, not persisted
}
static JitInst rinst(std::istream& i) {
    uint8_t raw_op = r8(i);
    if (raw_op > static_cast<uint8_t>(JitOp::NOP))
        throw std::runtime_error("Invalid bytecode opcode " + std::to_string(raw_op));
    JitOp    op      = static_cast<JitOp>(raw_op);
    uint16_t a       = r16(i);
    uint16_t b       = r16(i);
    uint16_t c       = r16(i);
    int      operand = static_cast<int32_t>(r32(i));
    int      str_idx = static_cast<int32_t>(r32(i));
    int      line    = static_cast<int32_t>(r32(i));
    if (line < 0) throw std::runtime_error("Invalid negative bytecode source line");
    return JitInst(op, a, b, c, operand, str_idx, line);
}

// ── JitFuncInfo ─────────────────────────────────────────────────

static void wfunc(std::ostream& o, const JitFuncInfo& f) {
    wstr(o, f.name);
    wstrlist(o, f.params);
    wvaluelist(o, f.defaults);
    w32(o, (uint32_t)f.upvalues.size());
    for (auto& uv : f.upvalues) {
        w8(o, uv.is_local ? 1 : 0);
        w32(o, (uint32_t)uv.index);
    }
    w64(o, (uint64_t)f.entry_ip);
    w64(o, (uint64_t)f.end_ip);
    w16(o, f.native_scratch_regs ? f.native_scratch_base : f.max_regs);
    w16(o, f.max_depth);
}
static JitFuncInfo rfunc(std::istream& i) {
    JitFuncInfo f;
    f.name     = rstr(i);
    f.params   = rstrlist(i);
    f.defaults = rvaluelist(i);
    uint32_t nuv = bytecode_read_count(i, BC_MAX_CALLABLE_ENTRIES, "upvalue");
    f.upvalues.resize(nuv);
    for (auto& uv : f.upvalues) {
        uint8_t is_local = r8(i);
        if (is_local > 1) throw std::runtime_error("Invalid bytecode upvalue locality flag");
        uv.is_local = is_local != 0;
        uv.index    = static_cast<int32_t>(r32(i));
        if (uv.index < 0) throw std::runtime_error("Invalid negative bytecode upvalue index");
    }
    uint64_t entry_ip = r64(i);
    uint64_t end_ip = r64(i);
    if (entry_ip > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        end_ip > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("Bytecode function range exceeds platform size");
    f.entry_ip  = static_cast<size_t>(entry_ip);
    f.end_ip    = static_cast<size_t>(end_ip);
    f.max_regs  = r16(i);
    f.max_depth = r16(i);
    return f;
}

// ── JitMethodInfo ───────────────────────────────────────────────

static void wmethod(std::ostream& o, const JitMethodInfo& m) {
    wstr(o, m.name);
    wstrlist(o, m.params);
    wvaluelist(o, m.defaults);
    w64(o, (uint64_t)m.entry_ip);
    w64(o, (uint64_t)m.end_ip);
    w16(o, m.native_scratch_regs ? m.native_scratch_base : m.max_regs);
}
static JitMethodInfo rmethod(std::istream& i) {
    JitMethodInfo m;
    m.name     = rstr(i);
    m.params   = rstrlist(i);
    m.defaults = rvaluelist(i);
    uint64_t entry_ip = r64(i);
    uint64_t end_ip = r64(i);
    if (entry_ip > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        end_ip > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("Bytecode method range exceeds platform size");
    m.entry_ip = static_cast<size_t>(entry_ip);
    m.end_ip   = static_cast<size_t>(end_ip);
    m.max_regs = r16(i);
    return m;
}

// ── JitClassInfo ────────────────────────────────────────────────

static void wclass(std::ostream& o, const JitClassInfo& c) {
    wstr(o, c.name);
    wstr(o, c.parent);
    // field_indices (string -> int map)
    w32(o, (uint32_t)c.field_indices.size());
    for (auto& [k, v] : c.field_indices) {
        wstr(o, k);
        w32(o, (uint32_t)v);
    }
    wvaluelist(o, c.field_defaults);
    // methods
    w32(o, (uint32_t)c.methods.size());
    for (auto& [k, m] : c.methods) {
        wstr(o, k);
        wmethod(o, m);
    }
}
static JitClassInfo rclass(std::istream& i) {
    JitClassInfo c;
    c.name   = rstr(i);
    c.parent = rstr(i);
    uint32_t nf = bytecode_read_count(i, BC_MAX_SECTION_ENTRIES, "class field");
    for (uint32_t j = 0; j < nf; ++j) {
        std::string k = rstr(i);
        int index = static_cast<int32_t>(r32(i));
        if (index < 0) throw std::runtime_error("Invalid negative bytecode class field index");
        if (!c.field_indices.emplace(std::move(k), index).second)
            throw std::runtime_error("Duplicate bytecode class field name");
    }
    c.field_defaults = rvaluelist(i);
    uint32_t nm = bytecode_read_count(i, BC_MAX_CALLABLE_ENTRIES, "class method");
    for (uint32_t j = 0; j < nm; ++j) {
        std::string k = rstr(i);
        JitMethodInfo method = rmethod(i);
        if (!c.methods.emplace(std::move(k), std::move(method)).second)
            throw std::runtime_error("Duplicate bytecode class method name");
    }
    return c;
}

// ── Public API ───────────────────────────────────────────────────

inline std::filesystem::path bytecode_utf8_path(const std::string& path) {
#ifdef _WIN32
    return std::filesystem::u8path(path);
#else
    return std::filesystem::path(path);
#endif
}

inline void write_chunk(std::ostream& f, const JitChunk& chunk) {
    // Header
    f.write((char*)BC_MAGIC, 4);
    w8(f, BC_VERSION);
    w16(f, chunk.max_regs);

    // strings
    wstrlist(f, chunk.strings);

    // constants
    wvaluelist(f, chunk.constants);

    // global_names
    wstrlist(f, chunk.global_names);

    // code
    w32(f, (uint32_t)chunk.code.size());
    for (auto& ins : chunk.code) winst(f, ins);

    // func_table
    w32(f, (uint32_t)chunk.func_table.size());
    for (auto& fn : chunk.func_table) wfunc(f, fn);

    // class_table
    w32(f, (uint32_t)chunk.class_table.size());
    for (auto& cls : chunk.class_table) wclass(f, cls);
}

struct BytecodeCallableRegion {
    size_t begin = 0;
    size_t end = 0;
    size_t regs = 0;
    const JitFuncInfo* function = nullptr;
    const JitMethodInfo* method = nullptr;
    std::string label;
};

inline void validate_chunk_structure(const JitChunk& chunk, const std::string& label) {
    auto invalid = [&](const std::string& message) -> void {
        throw std::runtime_error("Invalid bytecode in " + label + ": " + message);
    };
    if (chunk.code.size() > BC_MAX_CODE_ENTRIES ||
        chunk.strings.size() > BC_MAX_SECTION_ENTRIES ||
        chunk.constants.size() > BC_MAX_SECTION_ENTRIES ||
        chunk.global_names.size() > BC_MAX_SECTION_ENTRIES ||
        chunk.func_table.size() > BC_MAX_CALLABLE_ENTRIES ||
        chunk.class_table.size() > BC_MAX_CLASS_ENTRIES) {
        invalid("section count exceeds loader limit");
    }

    std::unordered_set<std::string> global_names;
    for (const auto& name : chunk.global_names) {
        if (!global_names.insert(name).second) invalid("duplicate global name '" + name + "'");
    }

    std::vector<BytecodeCallableRegion> regions;
    regions.reserve(chunk.func_table.size() + chunk.class_table.size() * 2);
    auto add_callable = [&](size_t entry_ip, size_t end_ip, size_t max_regs,
                            size_t parameter_base, size_t parameter_count,
                            size_t default_count, const JitFuncInfo* function,
                            const JitMethodInfo* method, const std::string& callable_label) {
        if (entry_ip >= end_ip || end_ip > chunk.code.size())
            invalid("callable range is outside code: " + callable_label);
        const size_t regs = max_regs > 0 ? max_regs : 32;
        if (parameter_count > 65535 || parameter_base > regs ||
            parameter_count > regs - parameter_base)
            invalid("parameter registers exceed frame: " + callable_label);
        if (default_count > parameter_count)
            invalid("too many parameter defaults: " + callable_label);
        regions.push_back({entry_ip, end_ip, regs, function, method, callable_label});
    };

    for (size_t i = 0; i < chunk.func_table.size(); ++i) {
        const auto& function = chunk.func_table[i];
        for (const auto& upvalue : function.upvalues) {
            if (upvalue.index < 0 || static_cast<size_t>(upvalue.index) >= 65535)
                invalid("upvalue index exceeds VM register/index range in function[" +
                        std::to_string(i) + "]");
        }
        add_callable(function.entry_ip, function.end_ip, function.max_regs,
                     0, function.params.size(), function.defaults.size(), &function, nullptr,
                     "function[" + std::to_string(i) + "]");
    }

    std::unordered_map<std::string, size_t> classes_by_name;
    for (size_t ci = 0; ci < chunk.class_table.size(); ++ci) {
        const auto& cls = chunk.class_table[ci];
        if (cls.name.empty()) invalid("class[" + std::to_string(ci) + "] has an empty name");
        if (!classes_by_name.emplace(cls.name, ci).second)
            invalid("duplicate class name '" + cls.name + "'");
        if (cls.field_indices.size() != cls.field_defaults.size())
            invalid("class field index/default counts differ: " + cls.name);
        std::vector<bool> seen_fields(cls.field_defaults.size(), false);
        for (const auto& [field, index] : cls.field_indices) {
            if (index < 0 || static_cast<size_t>(index) >= cls.field_defaults.size())
                invalid("class field index is out of range: " + cls.name + "." + field);
            if (seen_fields[static_cast<size_t>(index)])
                invalid("duplicate class field slot in " + cls.name);
            seen_fields[static_cast<size_t>(index)] = true;
        }
        for (const auto& [method_name, method] : cls.methods) {
            if (method_name == JIT_FIELD_INITIALIZER_METHOD &&
                (!method.params.empty() || !method.defaults.empty()))
                invalid("hidden field initializer must not have parameters: " + cls.name);
            add_callable(method.entry_ip, method.end_ip, method.max_regs,
                         1, method.params.size(), method.defaults.size(), nullptr, &method,
                         "class[" + std::to_string(ci) + "]." + method_name);
        }
    }

    // A forged parent cycle makes find_method() loop forever.  Detect cycles in
    // the serialized class graph even when definition order would delay linking.
    std::vector<uint8_t> class_color(chunk.class_table.size(), 0);
    for (size_t start = 0; start < chunk.class_table.size(); ++start) {
        if (class_color[start] != 0) continue;
        std::vector<size_t> path;
        size_t current = start;
        bool reached_external_parent = false;
        while (class_color[current] == 0) {
            class_color[current] = 1;
            path.push_back(current);
            const auto found = classes_by_name.find(chunk.class_table[current].parent);
            if (found == classes_by_name.end()) {
                reached_external_parent = true;
                break;
            }
            current = found->second;
        }
        if (!reached_external_parent && class_color[current] == 1 &&
            std::find(path.begin(), path.end(), current) != path.end())
            invalid("class inheritance cycle involving '" + chunk.class_table[current].name + "'");
        for (size_t index : path) class_color[index] = 2;
    }

    // Callable regions may be disjoint or strictly nested (a nested function is
    // serialized inside its enclosing body), but partial overlap makes frame
    // ownership ambiguous and is rejected.
    std::vector<size_t> sorted_regions(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) sorted_regions[i] = i;
    std::sort(sorted_regions.begin(), sorted_regions.end(), [&](size_t left, size_t right) {
        if (regions[left].begin != regions[right].begin)
            return regions[left].begin < regions[right].begin;
        return regions[left].end > regions[right].end;
    });
    std::vector<size_t> nesting;
    for (size_t region_index : sorted_regions) {
        const auto& region = regions[region_index];
        while (!nesting.empty() && region.begin >= regions[nesting.back()].end)
            nesting.pop_back();
        if (!nesting.empty()) {
            const auto& parent = regions[nesting.back()];
            if (region.begin == parent.begin || region.end > parent.end)
                invalid("overlapping callable ranges: " + parent.label + " and " + region.label);
        }
        nesting.push_back(region_index);
    }

    // Assign every instruction to its smallest containing callable. -1 denotes
    // the top-level frame. This supports precise register and control-flow checks.
    std::vector<int32_t> owner(chunk.code.size(), -1);
    nesting.clear();
    size_t next_region = 0;
    for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
        while (!nesting.empty() && regions[nesting.back()].end == ip) nesting.pop_back();
        while (next_region < sorted_regions.size() &&
               regions[sorted_regions[next_region]].begin == ip) {
            nesting.push_back(sorted_regions[next_region++]);
        }
        if (!nesting.empty()) owner[ip] = static_cast<int32_t>(nesting.back());
    }

    auto owner_begin = [&](int32_t region) -> size_t {
        return region < 0 ? 0 : regions[static_cast<size_t>(region)].begin;
    };
    auto owner_end = [&](int32_t region) -> size_t {
        return region < 0 ? chunk.code.size() : regions[static_cast<size_t>(region)].end;
    };
    auto owner_regs = [&](int32_t region) -> size_t {
        if (region < 0) return chunk.max_regs > 0 ? chunk.max_regs : 256;
        return regions[static_cast<size_t>(region)].regs;
    };
    auto fail_ip = [&](size_t ip, const std::string& message) -> void {
        invalid("instruction " + std::to_string(ip) + ": " + message);
    };
    auto check_reg = [&](size_t ip, size_t regs, uint16_t reg, const char* role) {
        if (static_cast<size_t>(reg) >= regs)
            fail_ip(ip, std::string(role) + " register R" + std::to_string(reg) +
                         " exceeds frame size " + std::to_string(regs));
    };
    auto check_window = [&](size_t ip, size_t regs, uint16_t first, int count,
                            size_t multiplier, const char* role) {
        if (count < 0) fail_ip(ip, std::string(role) + " count is negative");
        size_t n = static_cast<size_t>(count);
        if (n > (std::numeric_limits<size_t>::max() / multiplier))
            fail_ip(ip, std::string(role) + " count overflows");
        n *= multiplier;
        size_t start = static_cast<size_t>(first);
        if (start > regs || n > regs - start)
            fail_ip(ip, std::string(role) + " register window exceeds frame");
    };
    auto check_string = [&](size_t ip, int index, bool optional = false) {
        if (optional && index == -1) return;
        if (index < 0 || static_cast<size_t>(index) >= chunk.strings.size())
            fail_ip(ip, "string table index is out of range");
    };
    auto check_global = [&](size_t ip, int index) {
        if (index < 0 || static_cast<size_t>(index) >= chunk.global_names.size())
            fail_ip(ip, "global table index is out of range");
    };
    auto check_jump = [&](size_t ip, int target) {
        int32_t region = owner[ip];
        if (target < 0 || static_cast<size_t>(target) < owner_begin(region) ||
            static_cast<size_t>(target) > owner_end(region))
            fail_ip(ip, "control-flow target escapes its frame");
        if (static_cast<size_t>(target) < owner_end(region) &&
            owner[static_cast<size_t>(target)] != region)
            fail_ip(ip, "control-flow target enters another callable frame");
    };

    for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
        const JitInst& instruction = chunk.code[ip];
        const size_t regs = owner_regs(owner[ip]);
        auto one = [&](uint16_t reg, const char* role) { check_reg(ip, regs, reg, role); };
        auto window = [&](uint16_t first, int count, size_t multiplier, const char* role) {
            check_window(ip, regs, first, count, multiplier, role);
        };

        switch (instruction.op) {
            case JitOp::LOAD_CONST:
                one(instruction.a, "destination");
                if (instruction.operand < 0 ||
                    static_cast<size_t>(instruction.operand) >= chunk.constants.size())
                    fail_ip(ip, "constant table index is out of range");
                break;
            case JitOp::LOAD_NIL: case JitOp::LOAD_BOOL:
                one(instruction.a, "destination");
                if (instruction.op == JitOp::LOAD_BOOL &&
                    instruction.operand != 0 && instruction.operand != 1)
                    fail_ip(ip, "boolean operand must be 0 or 1");
                break;
            case JitOp::MOVE:
                one(instruction.a, "destination"); one(instruction.b, "source"); break;
            case JitOp::LOAD_GLOBAL:
                one(instruction.a, "destination"); check_global(ip, instruction.operand);
                if (instruction.str_idx != -1 && instruction.str_idx != 1)
                    fail_ip(ip, "LOAD_GLOBAL missing-value flag is invalid");
                break;
            case JitOp::STORE_GLOBAL:
                one(instruction.a, "source"); check_global(ip, instruction.operand); break;
            case JitOp::LOAD_UPVAL: case JitOp::STORE_UPVAL: {
                one(instruction.a, instruction.op == JitOp::LOAD_UPVAL ? "destination" : "source");
                const BytecodeCallableRegion* region = owner[ip] < 0
                    ? nullptr : &regions[static_cast<size_t>(owner[ip])];
                if (!region || !region->function || instruction.operand < 0 ||
                    static_cast<size_t>(instruction.operand) >= region->function->upvalues.size())
                    fail_ip(ip, "upvalue access is not valid for this callable frame");
                break;
            }
            case JitOp::ADD: case JitOp::SUB: case JitOp::MUL: case JitOp::DIV:
            case JitOp::MOD: case JitOp::BIT_AND: case JitOp::BIT_OR:
            case JitOp::BIT_XOR: case JitOp::LSHIFT: case JitOp::RSHIFT:
            case JitOp::CMP_EQ: case JitOp::CMP_NEQ: case JitOp::CMP_LT:
            case JitOp::CMP_LTE: case JitOp::CMP_GT: case JitOp::CMP_GTE:
            case JitOp::INDEX_GET: case JitOp::OP_IN:
                one(instruction.a, "destination"); one(instruction.b, "left/source");
                one(instruction.c, "right/key"); break;
            case JitOp::NEG: case JitOp::BIT_NOT: case JitOp::LOGICAL_NOT:
            case JitOp::DICT_KEYS:
                one(instruction.a, "destination"); one(instruction.b, "source"); break;
            case JitOp::JUMP:
                check_jump(ip, instruction.operand); break;
            case JitOp::JUMP_IF_FALSE: case JitOp::JUMP_IF_TRUE:
                one(instruction.a, "condition"); check_jump(ip, instruction.operand); break;
            case JitOp::CALL_FUNC:
                one(instruction.a, "destination"); one(instruction.b, "callee");
                window(instruction.c, instruction.operand, 1, "argument");
                check_string(ip, instruction.str_idx, true); break;
            case JitOp::CALL_BUILTIN:
                one(instruction.a, "destination");
                window(instruction.b, instruction.operand, 1, "builtin argument");
                check_string(ip, instruction.str_idx); break;
            case JitOp::METHOD_CALL:
                one(instruction.a, "destination"); one(instruction.b, "receiver");
                if (instruction.operand < 0) fail_ip(ip, "method argument count is negative");
                if (static_cast<size_t>(instruction.b) >= regs ||
                    static_cast<size_t>(instruction.operand) >
                        regs - static_cast<size_t>(instruction.b) - 1)
                    fail_ip(ip, "receiver and method argument register window exceeds frame");
                check_string(ip, instruction.str_idx); break;
            case JitOp::SUPER_CALL:
                one(instruction.a, "destination");
                window(instruction.b, instruction.operand, 1, "super argument");
                check_string(ip, instruction.str_idx); break;
            case JitOp::RETURN_VAL: case JitOp::OP_THROW:
                one(instruction.a, "value"); break;
            case JitOp::RETURN_NONE: case JitOp::TRY_END: case JitOp::HALT:
                break;
            case JitOp::MAKE_ARRAY:
                one(instruction.a, "destination");
                window(instruction.b, instruction.operand, 1, "array element"); break;
            case JitOp::MAKE_DICT:
                one(instruction.a, "destination");
                window(instruction.b, instruction.operand, 2, "dict entry"); break;
            case JitOp::INDEX_SET:
                one(instruction.a, "collection"); one(instruction.b, "key");
                one(instruction.c, "value"); break;
            case JitOp::DOT_GET:
                one(instruction.a, "destination"); one(instruction.b, "receiver");
                check_string(ip, instruction.str_idx); break;
            case JitOp::DOT_SET:
                one(instruction.a, "receiver"); one(instruction.b, "value");
                check_string(ip, instruction.str_idx); break;
            case JitOp::NEW_INSTANCE:
                one(instruction.a, "destination");
                window(instruction.b, instruction.operand, 1, "constructor argument");
                check_string(ip, instruction.str_idx); break;
            case JitOp::DEF_FUNC:
                if (instruction.operand < 0 ||
                    static_cast<size_t>(instruction.operand) >= chunk.func_table.size())
                    fail_ip(ip, "function table index is out of range");
                if (instruction.str_idx >= 0) check_global(ip, instruction.str_idx);
                break;
            case JitOp::MAKE_LAMBDA: {
                one(instruction.a, "destination");
                if (instruction.operand < 0 ||
                    static_cast<size_t>(instruction.operand) >= chunk.func_table.size())
                    fail_ip(ip, "function table index is out of range");
                const auto& function = chunk.func_table[static_cast<size_t>(instruction.operand)];
                const BytecodeCallableRegion* enclosing = owner[ip] < 0
                    ? nullptr : &regions[static_cast<size_t>(owner[ip])];
                for (const auto& upvalue : function.upvalues) {
                    if (upvalue.is_local) {
                        if (upvalue.index < 0 || static_cast<size_t>(upvalue.index) >= regs)
                            fail_ip(ip, "lambda captures a register outside the enclosing frame");
                    } else if (!enclosing || !enclosing->function || upvalue.index < 0 ||
                               static_cast<size_t>(upvalue.index) >=
                                   enclosing->function->upvalues.size()) {
                        fail_ip(ip, "lambda captures an upvalue outside the enclosing closure");
                    }
                }
                break;
            }
            case JitOp::DEF_CLASS:
                if (instruction.operand < 0 ||
                    static_cast<size_t>(instruction.operand) >= chunk.class_table.size())
                    fail_ip(ip, "class table index is out of range");
                check_global(ip, instruction.str_idx);
                if (chunk.global_names[static_cast<size_t>(instruction.str_idx)] !=
                    chunk.class_table[static_cast<size_t>(instruction.operand)].name)
                    fail_ip(ip, "class table name does not match its global name");
                if (instruction.c != 0 && instruction.c != JIT_CLASS_DEFAULTS_MARKER)
                    fail_ip(ip, "class definition marker is invalid");
                break;
            case JitOp::TRY_BEGIN:
                one(instruction.a, "catch value"); check_jump(ip, instruction.operand); break;
            case JitOp::FOREACH_NEXT:
                one(instruction.a, "value"); one(instruction.b, "iterator");
                one(instruction.c, "collection"); check_jump(ip, instruction.operand); break;
            case JitOp::PRINT: case JitOp::PRINT_NO_NL:
                window(instruction.a, instruction.operand, 1, "print argument"); break;
            case JitOp::USE_LIB:
                check_string(ip, instruction.str_idx); break;
            case JitOp::NOP:
                break;
            default:
                fail_ip(ip, "unknown opcode");
        }

        // Falling through into a nested/adjacent callable would execute its
        // registers using the wrong frame. Every compiler-emitted callable body
        // is guarded by an unconditional jump.
        const bool no_fallthrough = instruction.op == JitOp::JUMP ||
                                    instruction.op == JitOp::RETURN_VAL ||
                                    instruction.op == JitOp::RETURN_NONE ||
                                    instruction.op == JitOp::OP_THROW ||
                                    instruction.op == JitOp::HALT;
        if (!no_fallthrough && ip + 1 < owner_end(owner[ip]) &&
            owner[ip + 1] != owner[ip])
            fail_ip(ip, "fallthrough enters another callable frame");
    }
}

inline void validate_default_markers(const JitChunk& chunk, const std::string& label) {
    auto validate_callable = [&](size_t entry_ip, size_t end_ip, size_t max_regs,
                                 uint16_t param_base, size_t param_count,
                                 const std::string& callable) {
        if (entry_ip > end_ip || end_ip > chunk.code.size())
            throw std::runtime_error("Invalid callable bytecode range in " + label + ": " + callable);
        const int argc_reg = jit_default_arg_count_reg(
            chunk, entry_ip, param_base, param_count);
        if (argc_reg < 0) return;
        if (static_cast<size_t>(param_base) + param_count > max_regs ||
            static_cast<size_t>(argc_reg) >= max_regs) {
            throw std::runtime_error("Invalid default-argument marker register in " + label +
                                     ": " + callable);
        }
    };

    for (size_t i = 0; i < chunk.func_table.size(); ++i) {
        const auto& function = chunk.func_table[i];
        validate_callable(function.entry_ip, function.end_ip, function.max_regs,
                          0, function.params.size(), "function[" + std::to_string(i) + "]");
    }
    for (size_t ci = 0; ci < chunk.class_table.size(); ++ci) {
        for (const auto& [name, method] : chunk.class_table[ci].methods) {
            validate_callable(method.entry_ip, method.end_ip, method.max_regs,
                              1, method.params.size(),
                              "class[" + std::to_string(ci) + "]." + name);
        }
    }

    // Version-2 chunks produced during development may contain executable
    // class-default markers. Validate their register windows before the VM can
    // read from R[a..a+b). New version-3 output uses per-instance initializer
    // methods and no longer emits this marker.
    for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
        const JitInst& instruction = chunk.code[ip];
        if (instruction.op != JitOp::DEF_CLASS || instruction.c != JIT_CLASS_DEFAULTS_MARKER) continue;
        if (instruction.operand < 0 || static_cast<size_t>(instruction.operand) >= chunk.class_table.size())
            throw std::runtime_error("Invalid class-default marker class index in " + label);
        const size_t field_count = chunk.class_table[static_cast<size_t>(instruction.operand)].field_defaults.size();
        if (field_count != static_cast<size_t>(instruction.b))
            throw std::runtime_error("Invalid class-default marker field count in " + label);

        size_t frame_regs = chunk.max_regs;
        size_t owner_width = chunk.code.size() + 1;
        auto consider_owner = [&](size_t begin, size_t end, size_t regs) {
            if (begin <= ip && ip < end && end - begin < owner_width) {
                owner_width = end - begin;
                frame_regs = regs;
            }
        };
        for (const auto& function : chunk.func_table)
            consider_owner(function.entry_ip, function.end_ip, function.max_regs);
        for (const auto& cls : chunk.class_table)
            for (const auto& [_, method] : cls.methods)
                consider_owner(method.entry_ip, method.end_ip, method.max_regs);
        if (static_cast<size_t>(instruction.a) + field_count > frame_regs)
            throw std::runtime_error("Invalid class-default marker register range in " + label);
    }
}

inline JitChunk read_chunk(std::istream& f, const std::string& label) {
    // Header
    uint8_t magic[4] = {};
    bytecode_read_exact(f, magic, sizeof(magic), "bytecode header");
    if (memcmp(magic, BC_MAGIC, 4) != 0)
        throw std::runtime_error("Not a Sura bytecode file: " + label);
    uint8_t ver = r8(f);
    if (ver != BC_VERSION_LEGACY && ver != BC_VERSION)
        throw std::runtime_error("Bytecode version mismatch (got " + std::to_string(ver) +
                                 ", expected " + std::to_string(BC_VERSION_LEGACY) + " or " +
                                 std::to_string(BC_VERSION) + ")");

    JitChunk chunk;
    chunk.max_regs = r16(f);

    chunk.strings      = rstrlist(f);
    chunk.constants    = rvaluelist(f);
    chunk.global_names = rstrlist(f);

    uint32_t nc = bytecode_read_count(f, BC_MAX_CODE_ENTRIES, "instruction");
    chunk.code.reserve(nc);
    for (uint32_t i = 0; i < nc; ++i) chunk.code.push_back(rinst(f));

    uint32_t nf = bytecode_read_count(f, BC_MAX_CALLABLE_ENTRIES, "function");
    chunk.func_table.reserve(nf);
    for (uint32_t i = 0; i < nf; ++i) chunk.func_table.push_back(rfunc(f));

    uint32_t ncls = bytecode_read_count(f, BC_MAX_CLASS_ENTRIES, "class");
    chunk.class_table.reserve(ncls);
    for (uint32_t i = 0; i < ncls; ++i) chunk.class_table.push_back(rclass(f));

    if (f.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Trailing data after Sura bytecode payload: " + label);
    if (!f.eof())
        throw std::runtime_error("Cannot finish reading Sura bytecode payload: " + label);

    validate_chunk_structure(chunk, label);
    validate_default_markers(chunk, label);
    jit_prepare_native_scratch(chunk);
    return chunk;
}

inline void validate_bytecode_file_size(const std::string& path, const char* kind) {
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(bytecode_utf8_path(path), ec);
    if (!ec && size > BC_MAX_FILE_BYTES)
        throw std::runtime_error(std::string(kind) + " exceeds 256 MiB loader limit: " + path);
}

inline void save_chunk(const JitChunk& chunk, const std::string& path) {
    std::ofstream f(bytecode_utf8_path(path), std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write bytecode file: " + path);
    write_chunk(f, chunk);
}

inline JitChunk load_chunk(const std::string& path) {
    validate_bytecode_file_size(path, "Bytecode file");
    std::ifstream f(bytecode_utf8_path(path), std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read bytecode file: " + path);
    return read_chunk(f, path);
}

inline std::string chunk_to_bytes(const JitChunk& chunk) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    write_chunk(out, chunk);
    return out.str();
}

inline JitChunk chunk_from_bytes(const std::string& bytes) {
    if (bytes.size() > BC_MAX_FILE_BYTES)
        throw std::runtime_error("Release bytecode payload exceeds 256 MiB loader limit");
    std::istringstream in(bytes, std::ios::in | std::ios::binary);
    return read_chunk(in, "<release package>");
}

inline uint64_t release_hash_bytes(const std::string& data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : data) {
        h ^= (uint64_t)ch;
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint64_t release_next_stream(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

inline uint64_t release_mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline std::string release_random_salt() {
    uint64_t state = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    state ^= release_hash_bytes(std::to_string((uintptr_t)&state));
    try {
        std::random_device rd;
        state ^= ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    } catch (...) {
        state ^= 0xA5A5A5A5C3C3C3C3ULL;
    }
    uint64_t extra = release_next_stream(state);
    std::ostringstream out;
    out << std::hex << state << ":" << extra;
    return out.str();
}

inline std::string release_nonce_for(const std::string& payload) {
    std::ostringstream out;
    out << std::hex << release_hash_bytes(payload) << ":" << release_random_salt()
        << ":" << std::dec << payload.size();
    return out.str();
}

inline uint64_t release_key_hash(const std::string& key) {
    return key.empty() ? 0 : release_hash_bytes(std::string("sura-release-key:") + key);
}

inline uint64_t release_license_hash(const std::string& license) {
    return license.empty() ? 0 : release_hash_bytes(std::string("sura-release-license:") + license);
}

struct ReleaseMetadata {
    std::string release_id;
    std::string expires;
    std::string license;

    bool empty() const {
        return release_id.empty() && expires.empty() && license.empty();
    }
};

inline bool release_parse_date_int(const std::string& text, int& out) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit((unsigned char)text[i])) return false;
    }
    int year = std::stoi(text.substr(0, 4));
    int month = std::stoi(text.substr(5, 2));
    int day = std::stoi(text.substr(8, 2));
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    static const int days_in_month[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int max_day = days_in_month[month] + ((month == 2 && leap) ? 1 : 0);
    if (day > max_day) return false;
    out = year * 10000 + month * 100 + day;
    return true;
}

inline int release_today_int() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

inline void release_validate_metadata_for_save(const ReleaseMetadata& metadata) {
    if (metadata.release_id.size() > BC_MAX_RELEASE_TEXT)
        throw std::runtime_error("release id must be 65536 bytes or less");
    if (!metadata.expires.empty()) {
        int parsed = 0;
        if (!release_parse_date_int(metadata.expires, parsed))
            throw std::runtime_error("release expiration must use YYYY-MM-DD");
    }
    if (metadata.license.size() > 512)
        throw std::runtime_error("release license must be 512 bytes or less");
}

inline void release_validate_metadata_for_load(const ReleaseMetadata& metadata, const std::string& path) {
    if (!metadata.expires.empty()) {
        int expires = 0;
        if (!release_parse_date_int(metadata.expires, expires))
            throw std::runtime_error("Release package has invalid expiration date: " + path);
        if (release_today_int() > expires)
            throw std::runtime_error("Release package expired on " + metadata.expires + ": " + path);
    }
}

inline void release_xor_stream(std::string& payload, const std::string& nonce, const std::string& key = "",
                               const std::string& license = "") {
    uint64_t state = RELEASE_STREAM_KEY ^ release_hash_bytes(nonce);
    if (!key.empty()) state ^= release_key_hash(key);
    if (!license.empty()) state ^= release_license_hash(license);
    uint64_t word = 0;
    for (size_t i = 0; i < payload.size(); ++i) {
        if ((i & 7) == 0) word = release_next_stream(state);
        payload[i] = (char)((unsigned char)payload[i] ^ (unsigned char)((word >> ((i & 7) * 8)) & 0xFF));
    }
}

inline void release_xor_stream_hardened(std::string& payload, const std::string& nonce, const std::string& key = "",
                                        const std::string& license = "") {
    uint64_t a = RELEASE_STREAM_KEY ^ release_hash_bytes(std::string("sura-release-v5-a:") + nonce);
    uint64_t b = release_mix64(release_hash_bytes(std::string("sura-release-v5-b:") + nonce) ^ payload.size());
    if (!key.empty()) {
        a ^= release_mix64(release_key_hash(key) ^ 0xA0761D6478BD642FULL);
        b ^= release_mix64(release_hash_bytes(key) ^ 0xE7037ED1A0B428DBULL);
    }
    if (!license.empty()) {
        a ^= release_mix64(release_license_hash(license) ^ 0x8EBC6AF09C88C6E3ULL);
        b ^= release_mix64(release_hash_bytes(license) ^ 0x589965CC75374CC3ULL);
    }
    uint64_t word0 = 0;
    uint64_t word1 = 0;
    for (size_t i = 0; i < payload.size(); ++i) {
        if ((i & 15) == 0) {
            a = release_mix64(a + b + (uint64_t)i);
            b = release_mix64(b ^ a ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL));
            word0 = a;
            word1 = b;
        }
        uint64_t word = (i & 8) ? word1 : word0;
        payload[i] = (char)((unsigned char)payload[i] ^ (unsigned char)((word >> ((i & 7) * 8)) & 0xFF));
    }
}

inline uint64_t release_hardened_payload_hash(const std::string& payload, const std::string& nonce,
                                              uint64_t plain_hash, uint64_t key_hash,
                                              uint64_t license_hash) {
    uint64_t h = release_hash_bytes(payload);
    h ^= release_mix64(release_hash_bytes(nonce) ^ plain_hash);
    h ^= release_mix64(key_hash ^ 0xC2B2AE3D27D4EB4FULL);
    h ^= release_mix64(license_hash ^ 0x165667B19E3779F9ULL);
    return h;
}

inline JitChunk strip_chunk_for_release(JitChunk chunk) {
    for (auto& ins : chunk.code) ins.line = 0;
    auto strip_names = [](std::vector<std::string>& names) {
        for (auto& name : names) name.clear();
    };
    for (auto& fn : chunk.func_table) {
        fn.name.clear();
        fn.local_names.clear();
        strip_names(fn.params);
    }
    for (auto& cls : chunk.class_table) {
        for (auto& [_, method] : cls.methods) {
            method.name.clear();
            method.local_names.clear();
            strip_names(method.params);
        }
    }
    return chunk;
}

inline void save_release_package(const JitChunk& chunk, const std::string& path, const std::string& key = "",
                                 const ReleaseMetadata& metadata = {}) {
    release_validate_metadata_for_save(metadata);
    JitChunk release_chunk = strip_chunk_for_release(chunk);
    std::string plain = chunk_to_bytes(release_chunk);
    std::string nonce = release_nonce_for(plain);
    uint64_t plain_hash = release_hash_bytes(plain);
    std::string payload = plain;
    release_xor_stream_hardened(payload, nonce, key, metadata.license);

    std::ofstream f(bytecode_utf8_path(path), std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write release package: " + path);
    f.write((char*)RELEASE_MAGIC, 4);
    uint8_t flags = 0;
    if (!metadata.release_id.empty() || !metadata.expires.empty()) flags |= RELEASE_FLAG_METADATA;
    if (!key.empty()) flags |= RELEASE_FLAG_KEYED;
    if (!metadata.license.empty()) flags |= RELEASE_FLAG_LICENSED;
    uint64_t key_hash = release_key_hash(key);
    uint64_t license_hash = release_license_hash(metadata.license);
    uint64_t sealed_hash = release_hardened_payload_hash(payload, nonce, plain_hash, key_hash, license_hash);
    w8(f, RELEASE_VERSION);
    w8(f, flags);
    wstr(f, nonce);
    w64(f, plain_hash);
    w64(f, key_hash);
    w64(f, license_hash);
    w64(f, sealed_hash);
    if (flags & RELEASE_FLAG_METADATA) {
        wstr(f, metadata.release_id);
        wstr(f, metadata.expires);
    }
    wstr(f, payload);
}

inline JitChunk load_release_package(const std::string& path, const std::string& key = "",
                                     const std::string& license = "") {
    validate_bytecode_file_size(path, "Release package");
    std::ifstream f(bytecode_utf8_path(path), std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read release package: " + path);
    uint8_t magic[4] = {};
    bytecode_read_exact(f, magic, sizeof(magic), "release header");
    if (memcmp(magic, RELEASE_MAGIC, 4) != 0)
        throw std::runtime_error("Not a Sura release package: " + path);
    uint8_t ver = r8(f);
    auto finish_wrapper = [&]() {
        if (f.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Trailing data after Sura release package: " + path);
        if (!f.eof())
            throw std::runtime_error("Cannot finish reading Sura release package: " + path);
    };
    auto validate_flags = [&](uint8_t flags, uint8_t allowed) {
        if ((flags & static_cast<uint8_t>(~allowed)) != 0)
            throw std::runtime_error("Release package has invalid flags: " + path);
    };
    if (ver == RELEASE_VERSION_LEGACY) {
        std::string nonce = rstr(f, BC_MAX_RELEASE_TEXT, "release nonce");
        uint64_t expected_hash = r64(f);
        std::string payload = rstr(f, static_cast<uint32_t>(BC_MAX_FILE_BYTES), "release payload");
        finish_wrapper();
        release_xor_stream(payload, nonce);
        if (release_hash_bytes(payload) != expected_hash)
            throw std::runtime_error("Release package integrity check failed: " + path);
        return chunk_from_bytes(payload);
    }
    if (ver == RELEASE_VERSION_KEYED) {
        uint8_t flags = r8(f);
        validate_flags(flags, RELEASE_FLAG_KEYED);
        std::string nonce = rstr(f, BC_MAX_RELEASE_TEXT, "release nonce");
        uint64_t expected_hash = r64(f);
        uint64_t expected_key_hash = r64(f);
        std::string payload = rstr(f, static_cast<uint32_t>(BC_MAX_FILE_BYTES), "release payload");
        finish_wrapper();
        bool keyed = (flags & RELEASE_FLAG_KEYED) != 0;
        if (!keyed && expected_key_hash != 0)
            throw std::runtime_error("Release package has a key hash without the keyed flag: " + path);
        if (keyed && key.empty())
            throw std::runtime_error("Release package requires --load-release-key or SURA_RELEASE_KEY: " + path);
        if (keyed && release_key_hash(key) != expected_key_hash)
            throw std::runtime_error("Release package key mismatch: " + path);
        release_xor_stream(payload, nonce, keyed ? key : "");
        if (release_hash_bytes(payload) != expected_hash)
            throw std::runtime_error("Release package integrity check failed: " + path);
        return chunk_from_bytes(payload);
    }
    if (ver != RELEASE_VERSION_METADATA && ver != RELEASE_VERSION_LICENSED && ver != RELEASE_VERSION)
        throw std::runtime_error("Release package version mismatch (got " + std::to_string(ver) +
                                 ", expected " + std::to_string(RELEASE_VERSION) + ")");
    uint8_t flags = r8(f);
    uint8_t allowed_flags = RELEASE_FLAG_KEYED | RELEASE_FLAG_METADATA;
    if (ver == RELEASE_VERSION_LICENSED || ver == RELEASE_VERSION)
        allowed_flags |= RELEASE_FLAG_LICENSED;
    validate_flags(flags, allowed_flags);
    std::string nonce = rstr(f, BC_MAX_RELEASE_TEXT, "release nonce");
    uint64_t expected_hash = r64(f);
    uint64_t expected_key_hash = r64(f);
    uint64_t expected_license_hash = 0;
    uint64_t expected_sealed_hash = 0;
    if (ver == RELEASE_VERSION_LICENSED || ver == RELEASE_VERSION) expected_license_hash = r64(f);
    if (ver == RELEASE_VERSION) expected_sealed_hash = r64(f);
    ReleaseMetadata metadata;
    if (flags & RELEASE_FLAG_METADATA) {
        metadata.release_id = rstr(f, BC_MAX_RELEASE_TEXT, "release id");
        metadata.expires = rstr(f, 32, "release expiration");
    }
    std::string payload = rstr(f, static_cast<uint32_t>(BC_MAX_FILE_BYTES), "release payload");
    finish_wrapper();
    bool keyed = (flags & RELEASE_FLAG_KEYED) != 0;
    if (!keyed && expected_key_hash != 0)
        throw std::runtime_error("Release package has a key hash without the keyed flag: " + path);
    if (keyed && key.empty())
        throw std::runtime_error("Release package requires --load-release-key or SURA_RELEASE_KEY: " + path);
    if (keyed && release_key_hash(key) != expected_key_hash)
        throw std::runtime_error("Release package key mismatch: " + path);
    bool licensed = (flags & RELEASE_FLAG_LICENSED) != 0;
    if (!licensed && expected_license_hash != 0)
        throw std::runtime_error("Release package has a license hash without the licensed flag: " + path);
    if (licensed && license.empty())
        throw std::runtime_error("Release package requires --load-release-license or SURA_RELEASE_LICENSE: " + path);
    if (licensed && release_license_hash(license) != expected_license_hash)
        throw std::runtime_error("Release package license mismatch: " + path);
    if (ver == RELEASE_VERSION &&
        release_hardened_payload_hash(payload, nonce, expected_hash, expected_key_hash, expected_license_hash) !=
            expected_sealed_hash)
        throw std::runtime_error("Release package sealed payload check failed: " + path);
    release_validate_metadata_for_load(metadata, path);
    if (ver == RELEASE_VERSION) {
        release_xor_stream_hardened(payload, nonce, keyed ? key : "", licensed ? license : "");
    } else {
        release_xor_stream(payload, nonce, keyed ? key : "", licensed ? license : "");
    }
    if (release_hash_bytes(payload) != expected_hash)
        throw std::runtime_error("Release package integrity check failed: " + path);
    return chunk_from_bytes(payload);
}
