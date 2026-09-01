#pragma once
#include "ast.hpp"
#include "value.hpp"
#include <vector>
#include <unordered_map>
#include <string>

// Register-based VM/JIT opcodes.
enum class JitOp : uint8_t {
    LOAD_CONST, LOAD_NIL, LOAD_BOOL, MOVE,
    LOAD_GLOBAL, STORE_GLOBAL, LOAD_UPVAL, STORE_UPVAL,
    ADD, SUB, MUL, DIV, MOD,
    BIT_AND, BIT_OR, BIT_XOR, LSHIFT, RSHIFT,
    CMP_EQ, CMP_NEQ, CMP_LT, CMP_LTE, CMP_GT, CMP_GTE,
    NEG, BIT_NOT, LOGICAL_NOT,
    JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE,
    CALL_FUNC, CALL_BUILTIN, METHOD_CALL, SUPER_CALL, RETURN_VAL, RETURN_NONE,
    MAKE_ARRAY, MAKE_DICT, INDEX_GET, INDEX_SET, DOT_GET, DOT_SET, OP_IN,
    NEW_INSTANCE, DEF_FUNC, MAKE_LAMBDA, DEF_CLASS,
    TRY_BEGIN, TRY_END, OP_THROW,
    FOREACH_NEXT, DICT_KEYS,
    PRINT, PRINT_NO_NL, USE_LIB, HALT, NOP
};

// One instruction plus monomorphic inline-cache state.
// Forward declarations — full definitions are later in this file.
// Used by JitInst IC fields.
struct JitClassInfo;
struct JitMethodInfo;

struct JitInst {
    JitOp       op;
    uint16_t    a, b, c;
    int         operand; // JUMP offset
    int         str_idx;
    int         line;
    mutable int ic_cache; // Inline Caching: field index (-1 = cold)
    mutable const JitClassInfo*  ic_class;      // class that set ic_cache (for JIT inline IC guard)
    mutable const JitMethodInfo* ic_method;     // cached method info (monomorphic METHOD_CALL)
    mutable void*                ic_native_fn;  // cached SuraNativeFn pointer (void* to avoid include)
    mutable uint32_t             ic_native_frame_regs; // exact initialized/root-scanned native frame extent
    mutable bool                 ic_scalar_materialize; // native-only escaping virtual record
    mutable uint16_t             ic_scalar_reuse_flag_reg;
    mutable uint16_t             ic_scalar_reuse_candidate_reg;
    mutable bool                 ic_numeric_fast; // scalar plan proved numeric operands at entry
    mutable bool                 ic_scalar_guarded_field; // class/layout guarded by scalar entry
    mutable bool                 ic_scalar_numeric_field; // numeric check fused into guarded load

    JitInst(JitOp op, uint16_t a = 0, uint16_t b = 0, uint16_t c = 0,
            int operand = 0, int str_idx = -1, int line = 0)
        : op(op), a(a), b(b), c(c), operand(operand), str_idx(str_idx), line(line),
          ic_cache(-1), ic_class(nullptr), ic_method(nullptr), ic_native_fn(nullptr),
          ic_native_frame_regs(0),
          ic_scalar_materialize(false), ic_scalar_reuse_flag_reg(65535),
          ic_scalar_reuse_candidate_reg(65535), ic_numeric_fast(false),
          ic_scalar_guarded_field(false), ic_scalar_numeric_field(false) {}
};

// Captured-variable metadata.
struct UpvalueDesc {
    bool is_local; // true: enclosing local register; false: enclosing upvalue
    int index;     // 인덱스
};

struct JitFuncInfo {
    std::string              name;
    std::vector<std::string> params;
    std::vector<std::string> local_names;
    std::vector<Value>       defaults;
    std::vector<UpvalueDesc> upvalues;
    size_t                   entry_ip = 0;
    size_t                   end_ip   = 0;
    uint16_t                 max_regs = 0;
    uint16_t                 max_depth = 0;
    // Runtime-only register window used by the native tier for guarded
    // inlining and scalar replacement. It is deliberately omitted from the
    // bytecode wire format; loaders recompute it from the callable body.
    uint16_t                 native_scratch_base = 0;
    uint16_t                 native_scratch_regs = 0;
    uint16_t                 native_reuse_flag_reg = 65535;
};

struct JitMethodInfo {
    std::string              name;
    std::vector<std::string> params;
    std::vector<std::string> local_names;
    std::vector<Value>       defaults;
    size_t                   entry_ip = 0;
    size_t                   end_ip   = 0;
    uint16_t                 max_regs = 0;
    uint16_t                 native_scratch_base = 0;
    uint16_t                 native_scratch_regs = 0;
};

struct JitClassInfo {
    std::string                                    name;
    std::string                                    parent;
    std::unordered_map<std::string, int>           field_indices; // 인덱스
    std::vector<Value>                             field_defaults;
    std::unordered_map<std::string, JitMethodInfo> methods;
};

// Compiled register-bytecode chunk.
struct JitChunk {
    std::vector<JitInst>      code;
    std::vector<Value>        constants;
    std::vector<std::string>  strings;
    std::vector<std::string>  global_names;
    std::vector<JitFuncInfo>  func_table;
    std::vector<JitClassInfo> class_table;
    uint16_t                  max_regs = 0;

    int add_global(const std::string& s) {
        auto it = std::find(global_names.begin(), global_names.end(), s);
        if (it != global_names.end()) return (int)std::distance(global_names.begin(), it);
        global_names.push_back(s);
        return (int)global_names.size() - 1;
    }

    int add_const(const Value& v) {
        constants.push_back(v);
        return (int)constants.size() - 1;
    }

    int add_string(const std::string& s) {
        auto it = std::find(strings.begin(), strings.end(), s);
        if (it != strings.end()) return (int)std::distance(strings.begin(), it);
        strings.push_back(s);
        return (int)strings.size() - 1;
    }

    const std::string& get_string(int idx) const {
        static std::string empty_str = "";
        if (idx < 0 || idx >= (int)strings.size()) return empty_str;
        return strings[idx];
    }

    size_t emit(JitOp op, uint16_t a = 0, uint16_t b = 0, uint16_t c = 0, 
                int operand = 0, int str_idx = -1, int line = 0) {
        code.emplace_back(op, a, b, c, operand, str_idx, line);
        return code.size() - 1;
    }

    size_t current_addr() const { return code.size(); }

    void patch_jump(size_t addr, int target) {
        if (addr < code.size())
            code[addr].operand = target;
    }
};

// A straight-line function containing monomorphic method calls can eliminate
// short-lived record allocations in the native tier. Keep a bounded scratch
// window in such callables. The interpreter treats the extra slots as ordinary
// zero-initialized roots; the bytecode writer stores native_scratch_base so the
// on-disk ABI remains unchanged and deterministic.
static constexpr uint16_t JIT_NATIVE_SCALAR_SCRATCH_REGS = 160;

inline bool jit_range_has_method_call(const JitChunk& chunk,
                                      size_t entry_ip,
                                      size_t end_ip) {
    if (entry_ip > end_ip || end_ip > chunk.code.size()) return false;
    for (size_t ip = entry_ip; ip < end_ip; ++ip) {
        if (chunk.code[ip].op == JitOp::METHOD_CALL) return true;
    }
    return false;
}

inline void jit_prepare_native_scratch(JitChunk& chunk) {
    auto prepare = [&](auto& callable) {
        if (callable.native_scratch_regs != 0 ||
            !jit_range_has_method_call(chunk, callable.entry_ip, callable.end_ip)) {
            return;
        }
        const uint32_t expanded = static_cast<uint32_t>(callable.max_regs) +
                                  JIT_NATIVE_SCALAR_SCRATCH_REGS;
        if (expanded > 65535U) return;
        callable.native_scratch_base = callable.max_regs;
        callable.native_scratch_regs = JIT_NATIVE_SCALAR_SCRATCH_REGS;
        callable.max_regs = static_cast<uint16_t>(expanded);
    };

    for (auto& function : chunk.func_table) {
        prepare(function);
        if (function.native_scratch_regs) {
            // Keep the reuse flag at the beginning of the native-only window.
            // Scalar temporaries start after it, so the exact active frame no
            // longer has to include an otherwise-unused 160th scratch slot.
            function.native_reuse_flag_reg = function.native_scratch_base;
        }
    }
    for (auto& cls : chunk.class_table) {
        for (auto& method : cls.methods) {
            prepare(method.second);
        }
    }
}

// Internal lowering markers carried by otherwise ordinary bytecode fields.
// They reuse the JitInst layout. Bytecode version 3 distinguishes executable
// default semantics from version-2 writers that do not know the argc marker.
static constexpr int      JIT_DEFAULT_PROLOGUE_MAGIC = 0x53444654; // "SDFT"
static constexpr uint16_t JIT_CLASS_DEFAULTS_MARKER  = 0x5346;     // "SF"
inline constexpr const char* JIT_FIELD_INITIALIZER_METHOD = "\x1f" "field_init";

// A function/method with executable parameter defaults starts with a marked
// NOP. `a` names the hidden argument-count register, `b` is the first parameter
// register (0 for functions, 1 for methods), and `c` is the parameter count.
// Old bytecode has no marker and therefore keeps its metadata-value defaults.
inline int jit_default_arg_count_reg(const JitChunk& chunk,
                                     size_t entry_ip,
                                     uint16_t param_base,
                                     size_t param_count) {
    if (entry_ip >= chunk.code.size() || param_count > 65535) return -1;
    const JitInst& marker = chunk.code[entry_ip];
    if (marker.op != JitOp::NOP ||
        marker.operand != JIT_DEFAULT_PROLOGUE_MAGIC ||
        marker.str_idx != -1 ||
        marker.b != param_base ||
        marker.c != static_cast<uint16_t>(param_count)) {
        return -1;
    }
    return static_cast<int>(marker.a);
}
