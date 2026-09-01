#pragma once
#include "jit_op.hpp"   // JitFuncInfo, JitClassInfo, JitChunk re-used for metadata
#include "value.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

// ================================================================
//  Sura IR (Intermediate Representation)
//  Pipeline: AST → IRChunk (IRBuilder) → JitChunk (ir_lower) → JitVM
//
//  Key differences from JitChunk bytecode:
//    1. Virtual registers (VReg = uint32_t, unlimited)
//    2. Label-based control flow (no raw offset patching)
//    3. Explicit LABEL instructions mark branch targets
//    4. Optimization passes can be inserted between IR and lowering
// ================================================================

using VReg    = uint32_t;
using IRLabel = uint32_t;
static constexpr VReg    VREG_NONE  = UINT32_MAX;
static constexpr IRLabel LABEL_NONE = UINT32_MAX;

// ── IR opcode ───────────────────────────────────────────────────
enum class IROp : uint8_t {
    // Constants
    FCONST,     // dst = constants[imm]        (float/string Value)
    BCONST,     // dst = (bool)imm
    NCONST,     // dst = nil

    // Move
    MOV,        // dst = src1

    // Arithmetic
    ADD, SUB, MUL, DIV, MOD,
    NEG,

    // Bitwise
    BAND, BOR, BXOR, SHL, SHR, BNOT,

    // Comparison
    EQ, NEQ, LT, LTE, GT, GTE,

    // Logical
    LNOT,

    // Collection membership
    IN,

    // Control flow
    LABEL,      // define label[imm]  (no registers used)
    JMP,        // jump to label[imm]
    JT,         // if src1 jump to label[imm]
    JF,         // if !src1 jump to label[imm]

    // Globals / upvalues
    GLOAD,      // dst = globals[imm]      (imm = global_names index)
    GSTORE,     // globals[imm] = src1
    UVLOAD,     // dst = upval[imm]
    UVSTORE,    // upval[imm] = src1

    // Calls
    CALL,       // dst = call(src1=fn_reg, args start at src2, imm=argc, str_id=name)
    MCALL,      // dst = method_call(src1=obj, args after, imm=argc, str_id=method)
    SCALL,      // dst = super_call(args start at src1, imm=argc, str_id=method)
    NCALL,      // dst = new(class_name=str_id, args start at src1, imm=argc)
    BUILTIN,    // dst = builtin(cmd=str_id, args start at src1, imm=argc)
    RET,        // return src1
    RETVOID,    // return nil

    // Collections
    MKARR,      // dst = make_array(start=src1, imm=count)
    MKDICT,     // dst = make_dict(start=src1, imm=count)
    IGET,       // dst = src1[src2]
    ISET,       // src1[src2] = src3
    DGET,       // dst = src1.prop   (str_id=prop)
    DSET,       // src1.prop = src2  (str_id=prop)

    // Functions / classes
    MKFN,       // dst = make_closure(func_table[imm])
    DEFCLS,     // define class(class_table[imm]) → global(imm2)

    // Print / formatting
    PRINT,      // print args at src1, imm=count, imm2=newline(1) or not(0)
    FMTVAL,     // dst = format(src1, fmt_const_at_src2)

    // Exception
    THROW,      // throw src1
    TRYBEGIN,   // try begin, catch reg=dst, patch to label[imm]
    TRYEND,     // try end

    // Iteration
    FOREACH,    // FOREACH_NEXT: dst=elem, src1=iter_reg, src2=coll, label[imm]=exit
    DKEYS,      // dst = dict_keys(src1)

    // Module
    USLIB,      // use_lib str_id

    // Terminator
    HALT,
};

// ── IR Instruction ───────────────────────────────────────────────
struct IRInst {
    IROp     op;
    VReg     dst  = VREG_NONE;
    VReg     src1 = VREG_NONE;
    VReg     src2 = VREG_NONE;
    VReg     src3 = VREG_NONE;
    int32_t  imm  = 0;     // label id / const index / count / bool value
    int32_t  imm2 = 0;     // second count / global index
    int32_t  str_id = -1;  // index into IRChunk::strings
    int      line = 0;

    IRInst() = default;
    IRInst(IROp op, VReg dst, VReg src1, VReg src2, VReg src3,
           int32_t imm, int32_t imm2, int32_t str_id, int line)
        : op(op), dst(dst), src1(src1), src2(src2), src3(src3),
          imm(imm), imm2(imm2), str_id(str_id), line(line) {}
};

// ── IR Chunk ─────────────────────────────────────────────────────
struct IRChunk {
    std::vector<IRInst>       code;
    std::vector<Value>        constants;
    std::vector<std::string>  strings;
    std::vector<std::string>  global_names;
    std::vector<JitFuncInfo>  func_table;
    std::vector<JitClassInfo> class_table;

    uint32_t next_vreg  = 0;
    uint32_t next_label = 0;

    VReg    alloc_vreg()  { return next_vreg++; }
    IRLabel alloc_label() { return next_label++; }

    int add_const(const Value& v) {
        constants.push_back(v);
        return (int)constants.size() - 1;
    }

    int add_string(const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return (int)i;
        strings.push_back(s);
        return (int)strings.size() - 1;
    }

    int add_global(const std::string& s) {
        for (size_t i = 0; i < global_names.size(); ++i)
            if (global_names[i] == s) return (int)i;
        global_names.push_back(s);
        return (int)global_names.size() - 1;
    }

    // Emit helpers — return index of emitted instruction
    size_t emit(IROp op,
                VReg dst = VREG_NONE, VReg src1 = VREG_NONE,
                VReg src2 = VREG_NONE, VReg src3 = VREG_NONE,
                int32_t imm = 0, int32_t imm2 = 0,
                int32_t str_id = -1, int line = 0) {
        code.emplace_back(op, dst, src1, src2, src3, imm, imm2, str_id, line);
        return code.size() - 1;
    }

    void emit_label(IRLabel lbl, int line = 0) {
        emit(IROp::LABEL, VREG_NONE, VREG_NONE, VREG_NONE, VREG_NONE,
             (int32_t)lbl, 0, -1, line);
    }

    size_t current_addr() const { return code.size(); }
};
