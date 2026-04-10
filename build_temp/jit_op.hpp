#pragma once
#include "ast.hpp"
#include "value.hpp"
#include <vector>
#include <unordered_map>
#include <string>

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?��??�터 기반 JIT 가?�머??명령????(OpCode) 
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
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
    PRINT, PRINT_NO_NL, USE_LIB, HALT, NOP
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
// ?�일 명령??구조�?(Inline Caching 캐시 ?�롯 추�? - ?�기 최적???��?)
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
struct JitInst {
    JitOp       op;
    uint16_t    a, b, c;
    int         operand; // JUMP offset ??    int         str_idx;
    int         line;
    int         ic_cache; // Inline Caching ?�용 (?�드 ?�프???�?�용)

    JitInst(JitOp op, uint16_t a = 0, uint16_t b = 0, uint16_t c = 0, 
            int operand = 0, int str_idx = -1, int line = 0)
        : op(op), a(a), b(b), c(c), operand(operand), str_idx(str_idx), line(line), ic_cache(-1) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
// 메�??�이??// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
struct UpvalueDesc {
    bool is_local; // true�?바로 바깥 ?�수??로컬 ?��??�터, false�?바깥 ?�수??Upvalue
    int index;     // ?�덱??};

struct JitFuncInfo {
    std::string              name;
    std::vector<std::string> params;
    std::vector<Value>       defaults;
    std::vector<UpvalueDesc> upvalues;
    size_t                   entry_ip = 0;
    size_t                   end_ip   = 0;
    uint16_t                 max_regs = 0;
    uint16_t                 max_depth = 0;
};

struct JitMethodInfo {
    std::string              name;
    std::vector<std::string> params;
    std::vector<Value>       defaults;
    size_t                   entry_ip = 0;
    size_t                   end_ip   = 0;
    uint16_t                 max_regs = 0;
};

struct JitClassInfo {
    std::string                                    name;
    std::string                                    parent;
    std::unordered_map<std::string, int>           field_indices; // ?�름 -> 배열 ?�덱??    std::vector<Value>                             field_defaults;
    std::unordered_map<std::string, JitMethodInfo> methods;
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
// 컴파??결과 �?��
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
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
