#pragma once
#include "jit_op.hpp"
#include "jit_alloc.hpp"
#include "jit_target.hpp"
#include "jit_x64.hpp"
#include "jit_throw.hpp"
#include <memory>
#include <iostream>
#include <vector>
#include <cmath>   // std::fmod for MOD opcode
#include <cstring>
#include <limits>
#include <unordered_set>
#include <functional>

// ================================================================
//  NativeCompiler — translates a JitChunk function body to a selected native
//  backend, with register-VM fallback when the target or opcode is unsupported.
//
//  ABI (Win64):
//      uint64_t fn(Value* R, const Value* consts);
//        RCX = R (register file base)
//        RDX = consts (constants pool base)
//        RAX = returned Value bits
//
//  Prologue saves RBX/R12 and caches:
//      RBX = R
//      R12 = consts
//  Register file is accessed as [RBX + idx*8].  Constants as [R12 + idx*8].
//
//  Supported opcodes: the switch in emit_op() is the authoritative list, and
//  anything it does not handle makes compile() bail and return null. Do not
//  treat this comment as the contract — many cases carry additional guards and
//  return false when those are not met, so "has a case" and "is compiled" are
//  not the same thing. Two facts worth knowing before reading further:
//
//    * DIV does not use the inline SSE path ADD/SUB/MUL take, because dividing
//      by zero must raise [E202] rather than yield an infinity. It goes through
//      the guarded sura_jit_checked_div helper, the same shape MOD uses.
//    * Coverage is wider than plain arithmetic. Calls, method calls, field
//      reads, closure creation and instance creation all have emitters, so a
//      hot object-shaped callee can go native too.
//
//  The three backends do not accept the same set. NativeCompiler (Win64) is
//  the broad one; SysVBaselineCompiler and Arm64BaselineCompiler are
//  deliberately narrow, and jit_target.hpp is the single source of truth for
//  which one a platform gets.
//
//  To find out what a given program actually reached, do not read this header
//  — run tools/sura_jit_differential.ps1, which reports the number of
//  callables that were compiled natively and diffs their output against the
//  register VM.
//
//  JUMP_IF_FALSE/TRUE only emits code that understands bool results
//  (NBFALSE/NBTRUE/NBNIL).  Numeric-truthiness is not handled in native;
//  the compiler bails unless the preceding op is a CMP_* writing the
//  condition register, guaranteeing it holds a bool.
// ================================================================

// Forward decl; real definition in jit_vm.hpp
class JitVM;

// C-linkage trampolines called from JIT'd native code. Defined as
// `extern "C" inline` in jit_vm.hpp so their addresses are stable.
extern "C" uint64_t sura_jit_call(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_construct_plain2(JitVM* vm, const JitClassInfo* cls, uint64_t v0, uint64_t v1);
// Collector safepoint taken before every record allocation issued from native
// code. Without it a loop that only constructs records never reaches a GC
// trigger: MAKE_ARRAY/MAKE_DICT tick the collector, but constructors did not,
// so five million short-lived instances piled up and every allocation paid
// for fresh, cold memory. Defined in jit_vm.hpp.
extern "C" void sura_jit_record_alloc_safepoint(JitVM* vm);
extern "C" uint64_t sura_jit_construct_plain3(JitVM* vm, const JitClassInfo* cls, uint64_t v0, uint64_t v1, uint64_t v2);
extern "C" uint64_t sura_jit_materialize_scalar_record(JitVM* vm,
                                                         Value* R,
                                                         const JitInst* ins);
struct JitStrictCountedLoop;
struct JitChunk;
extern "C" int sura_jit_strict_vector_loop(JitVM* vm,
                                              Value* R,
                                              const JitChunk* chunk,
                                              const JitStrictCountedLoop* spec);

// Exact-width record constructors avoid the generic default-field copy when
// the class has no trailing fields. The optimization is shape-based and keeps
// the generic helpers below for prefix constructors on larger records.
extern "C" inline uint64_t sura_jit_construct_exact2(JitVM* vm,
                                                      const JitClassInfo* cls,
                                                      uint64_t v0,
                                                      uint64_t v1) {
    sura_jit_record_alloc_safepoint(vm);
    Value obj = Value::make_inst_ref(&cls->name);
    GCInstance* instance = obj.as_inst();
    instance->fields.resize(2, Value::nil());
    instance->jit_info = const_cast<JitClassInfo*>(cls);
    instance->fields[0] = Value::from_bits(v0);
    instance->fields[1] = Value::from_bits(v1);
    return obj.raw_bits();
}

extern "C" inline uint64_t sura_jit_construct_exact3(JitVM* vm,
                                                      const JitClassInfo* cls,
                                                      uint64_t v0,
                                                      uint64_t v1,
                                                      uint64_t v2) {
    sura_jit_record_alloc_safepoint(vm);
    Value obj = Value::make_inst_ref(&cls->name);
    GCInstance* instance = obj.as_inst();
    instance->fields.resize(3, Value::nil());
    instance->jit_info = const_cast<JitClassInfo*>(cls);
    instance->fields[0] = Value::from_bits(v0);
    instance->fields[1] = Value::from_bits(v1);
    instance->fields[2] = Value::from_bits(v2);
    return obj.raw_bits();
}
extern "C" uint64_t sura_jit_method_call(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_dot_get(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_dot_set(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_make_array(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_make_dict(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_load_global(JitVM* vm, int idx);
extern "C" uint64_t sura_jit_load_global_inst(JitVM* vm, const JitInst* ins);
extern "C" void     sura_jit_store_global(JitVM* vm, int idx, uint64_t bits);
extern "C" int      sura_jit_truthy(uint64_t bits);
extern "C" uint64_t sura_jit_eq(uint64_t a, uint64_t b, int neq);
// Phase 10: top-level JIT helpers
extern "C" uint64_t sura_jit_make_lambda(JitVM* vm, struct Value* R, const JitInst* ins);
extern "C" void     sura_jit_def_class(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_use_lib(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_jit_index_get(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_index_set(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_new_instance(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_op_in(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_dict_keys(JitVM* vm, Value* R, const JitInst* ins);
extern "C" int      sura_jit_foreach_next(JitVM* vm, Value* R, const JitInst* ins);
extern "C" void     sura_jit_print(JitVM* vm, struct Value* R, const JitInst* ins, int newline);
extern "C" uint64_t sura_jit_add(uint64_t a, uint64_t b, JitVM* vm);  // Phase 10 safe ADD
// Baseline-tier helpers (jit_vm.hpp, SURA_BL_HELPER): one instruction of
// interpreter semantics each, with every throw parked in the VM and turned
// into SURA_JIT_DEOPT_SENTINEL so nothing unwinds through baseline code.
extern "C" uint64_t sura_bl_arith(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_truthy(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_load_global(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_store_global(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_make_array(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_make_dict(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_index_get(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_index_set(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_dot_get(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_dot_set(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_op_in(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_dict_keys(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_foreach_next(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_print(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_call(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_method_call(JitVM* vm, Value* R, const JitInst* ins);
extern "C" uint64_t sura_bl_call_builtin(JitVM* vm, Value* R, const JitInst* ins);

// Checked arithmetic slow paths. Native code guards the overwhelmingly common
// numeric cases inline and reaches these helpers only for a semantic error or
// an operation whose C++ integer lowering needs explicit UB-free validation.
enum : int {
    JIT_ARITH_SUB = 1,
    JIT_ARITH_MUL = 2,
    JIT_CMP_LT = 3,
    JIT_CMP_LTE = 4,
    JIT_CMP_GT = 5,
    JIT_CMP_GTE = 6,
    JIT_BIT_AND = 7,
    JIT_BIT_OR = 8,
    JIT_BIT_XOR = 9,
    JIT_LSHIFT = 10,
    JIT_RSHIFT = 11,
    JIT_BIT_NOT = 12
};

[[noreturn]] inline void sura_jit_numeric_error(const char* message, int line) {
    throw JitThrow{message, line, Value::nil(), {}};
}

inline int64_t sura_jit_checked_i64(const Value& value, int line) {
    if (!value.is_num()) sura_jit_numeric_error("[E200] type mismatch", line);
    const double number = value.as_num();
    constexpr double max_safe_integer = 9007199254740991.0; // 2^53 - 1
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < -max_safe_integer || number > max_safe_integer) {
        sura_jit_numeric_error("[E203] operands must be finite safe integers", line);
    }
    return static_cast<int64_t>(number);
}

extern "C" inline uint64_t sura_jit_checked_binary(uint64_t lhs_bits,
                                                    uint64_t rhs_bits,
                                                    int operation,
                                                    int line) {
    const Value lhs = Value::from_bits(lhs_bits);
    const Value rhs = Value::from_bits(rhs_bits);
    if (operation >= JIT_ARITH_SUB && operation <= JIT_CMP_GTE) {
        if (!lhs.is_num() || !rhs.is_num()) {
            sura_jit_numeric_error("[E200] type mismatch", line);
        }
        switch (operation) {
            case JIT_ARITH_SUB: return (lhs - rhs).raw_bits();
            case JIT_ARITH_MUL: return (lhs * rhs).raw_bits();
            case JIT_CMP_LT:  return Value(lhs.as_num() <  rhs.as_num()).raw_bits();
            case JIT_CMP_LTE: return Value(lhs.as_num() <= rhs.as_num()).raw_bits();
            case JIT_CMP_GT:  return Value(lhs.as_num() >  rhs.as_num()).raw_bits();
            case JIT_CMP_GTE: return Value(lhs.as_num() >= rhs.as_num()).raw_bits();
            default: break;
        }
    }

    constexpr int64_t max_safe_integer = 9007199254740991LL;
    const int64_t left = sura_jit_checked_i64(lhs, line);
    const int64_t right = sura_jit_checked_i64(rhs, line);
    int64_t result = 0;
    switch (operation) {
        case JIT_BIT_AND: result = left & right; break;
        case JIT_BIT_OR:  result = left | right; break;
        case JIT_BIT_XOR: result = left ^ right; break;
        case JIT_LSHIFT: {
            if (right < 0 || right >= 64) {
                sura_jit_numeric_error("[E203] shift count must be in 0..63", line);
            }
            const long double scaled = std::ldexp(static_cast<long double>(left),
                                                  static_cast<int>(right));
            if (scaled < -static_cast<long double>(max_safe_integer) ||
                scaled > static_cast<long double>(max_safe_integer)) {
                sura_jit_numeric_error(
                    "[E203] left shift result exceeds the safe integer range", line);
            }
            result = static_cast<int64_t>(scaled);
            break;
        }
        case JIT_RSHIFT: {
            if (right < 0 || right >= 64) {
                sura_jit_numeric_error("[E203] shift count must be in 0..63", line);
            }
            const long double divisor = std::ldexp(1.0L, static_cast<int>(right));
            result = static_cast<int64_t>(std::floor(
                static_cast<long double>(left) / divisor));
            break;
        }
        default:
            sura_jit_numeric_error("[E200] invalid numeric operation", line);
    }
    if (result < -max_safe_integer || result > max_safe_integer) {
        sura_jit_numeric_error("[E203] bitwise result exceeds the safe integer range", line);
    }
    return Value(static_cast<double>(result)).raw_bits();
}

extern "C" inline uint64_t sura_jit_checked_unary(uint64_t value_bits,
                                                   int operation,
                                                   int line) {
    const Value value = Value::from_bits(value_bits);
    if (operation == JIT_BIT_NOT) {
        constexpr int64_t max_safe_integer = 9007199254740991LL;
        const int64_t integer = sura_jit_checked_i64(value, line);
        const int64_t result = ~integer;
        if (result < -max_safe_integer || result > max_safe_integer) {
            sura_jit_numeric_error(
                "[E203] bitwise result exceeds the safe integer range", line);
        }
        return Value(static_cast<double>(result)).raw_bits();
    }
    if (!value.is_num()) sura_jit_numeric_error("[E200] type mismatch", line);
    return value.negate().raw_bits();
}

extern "C" inline uint64_t sura_jit_checked_mod(uint64_t lhs_bits,
                                                 uint64_t rhs_bits,
                                                 int line) {
    const Value lhs = Value::from_bits(lhs_bits);
    const Value rhs = Value::from_bits(rhs_bits);
    if (!lhs.is_num() || !rhs.is_num()) {
        sura_jit_numeric_error("[E200] type mismatch", line);
    }
    if (rhs.as_num() == 0.0) {
        sura_jit_numeric_error("[E202] modulo by zero", line);
    }
    return lhs.mod(rhs).raw_bits();
}

// Division cannot use the inline SSE path that ADD/SUB/MUL take: dividing by
// zero has to raise [E202] rather than produce an infinity, so it needs the
// same guarded helper shape as MOD. The two error strings below must stay
// identical to the DIV case in JitVM::execute_frame - the differential lane
// compares native and interpreter output byte for byte.
extern "C" inline uint64_t sura_jit_checked_div(uint64_t lhs_bits,
                                                 uint64_t rhs_bits,
                                                 int line) {
    const Value lhs = Value::from_bits(lhs_bits);
    const Value rhs = Value::from_bits(rhs_bits);
    if (!lhs.is_num() || !rhs.is_num()) {
        sura_jit_numeric_error("[E200] type mismatch", line);
    }
    if (rhs.as_num() == 0.0) {
        sura_jit_numeric_error("[E202] division by zero", line);
    }
    return (lhs / rhs).raw_bits();
}

// Win64 ABI:
//   RCX = vm     (caller)
//   RDX = R      (register file base)
//   R8  = consts (constant pool base)
//   RAX = result (Value raw bits)
using SuraNativeFn = uint64_t (*)(JitVM* vm, struct Value* R, const struct Value* consts);

struct NativeFunc {
    ExecCode     code;
    SuraNativeFn fn = nullptr;
    bool         scalarized = false;
    bool         record_reuse_capable = false;
    uint32_t     frame_regs = 0; // exact initialized range published to GC
    // Remapped instructions used by scalarized inline bodies must outlive the
    // generated code because cold helpers receive stable JitInst pointers.
    std::vector<std::unique_ptr<JitInst>> inline_insts;
    // Heap-stable strict-loop descriptors referenced by the generated code.
    std::vector<std::unique_ptr<JitStrictCountedLoop>> strict_loops;
    // Bit i set means opcode i was actually emitted into this body. Answers
    // "which emitters has anything ever exercised", which is the question a
    // differential lane needs - a suite can be green while most emitters were
    // never reached. JitOp has 56 members, so one word is enough.
    uint64_t     emitted_ops = 0;
    // Baseline direct-call linkage (BaselineLinkContext::resolve_callee).
    // A body is directly callable when it was compiled with entry guards on
    // exactly its parameters, binds no argument-count register, and fits the
    // machine-stack frame limit. The offsets index `code`.
    bool         baseline_direct_callable = false;
    uint32_t     baseline_params = 0;
    size_t       baseline_unguarded_entry = 0;
    uint8_t      baseline_return_kind = 0;
    int32_t      baseline_return_fidx = -1;
    // Bit p set: parameter p enters through a number guard. A clear bit is a
    // parameter the VM has seen a non-numeric argument for: it enters
    // unguarded and the body treats it as unknown. Parameters past bit 63
    // are always guarded.
    uint64_t     baseline_guard_mask = ~0ULL;
    // True when a direct call to this body must place the callee frame on
    // the VM value stack (BaselineBodyAnalysis::vm_frame): the body, or a
    // body it calls directly, may run a helper and so may trigger the
    // collector while the frame is live.
    bool         baseline_vm_frame = false;
};
static_assert((int)JitOp::NOP < 64,
              "NativeFunc::emitted_ops is a 64-bit mask; JitOp outgrew it");

// Returned by baseline-tier native code instead of a real Value when an entry
// guard fails. The bit pattern is the NaN-boxed object tag with a null
// pointer payload, which no reachable Value construction can produce, so the
// caller can treat it as an unambiguous "re-run this call in the VM" signal.
inline constexpr uint64_t SURA_JIT_DEOPT_SENTINEL = 0xFFFC000000000000ULL;

// Direct-call linkage for the baseline native tiers.
//
// A baseline body carries no unwind metadata, so nothing may throw through
// it. Numeric work is inlined; everything else goes through a helper that
// runs the interpreter's semantics for that one instruction and, if that
// throws, parks the exception in the VM and returns the deopt sentinel.
// The body then records the instruction to resume at and returns the
// sentinel itself; the interpreter picks the frame up from there and
// rethrows, so the error, its line and its stack trace are exactly the
// interpreted ones. Callee frames of native-to-native calls live on the
// VM's value stack, where the collector sees them while a helper allocates.
// Only a pure body (no store, print or generic call - see
// BaselineBodyAnalysis::pure) may be a direct callee: when a call into it
// deopts, the caller's interpreter re-runs the whole call, which must not
// repeat an effect. The link context is the compiler's window into the VM:
// what a global currently holds (so LOAD_GLOBAL can become a guarded
// constant), where the VM's slots live, and the native body of a callee
// (compiled on demand). Everything it answers is verified again at run time
// by a guard whose failure path is the deopt sentinel.
struct BaselineCalleeInfo {
    const uint8_t* guarded_entry   = nullptr; // code start: entry guards, then body
    const uint8_t* unguarded_entry = nullptr; // past the guards; every guarded argument must be a proven number
    uint32_t       frame_regs      = 0;       // value-stack frame the callee expects, in Values
    uint32_t       params          = 0;
    uint64_t       guard_mask      = ~0ULL;   // NativeFunc::baseline_guard_mask
    uint8_t        return_kind     = 0;       // BaselineBodyAnalysis::Kind
    int32_t        return_fidx     = -1;
    bool           vm_frame        = false;   // NativeFunc::baseline_vm_frame
};

class BaselineLinkContext {
public:
    virtual ~BaselineLinkContext() = default;
    // Current value of global slot `idx` when the slot exists and has been
    // written. False for anything the interpreter would refuse or treat as
    // nil, so a body never compiles against a value that is not really there.
    virtual bool global_value(int idx, uint64_t& bits) const = 0;
    // Byte offset from the vm pointer to the `Value*` that begins the globals
    // vector, or -1 when the VM cannot vouch for that layout.
    virtual int32_t globals_vector_offset() const = 0;
    // Byte offset from the vm pointer to the signed 64-bit count of nested
    // native calls still allowed before the interpreter's frame limit.
    virtual int32_t depth_budget_offset() const = 0;
    // Byte offset of the signed 64-bit slot a body writes the bytecode index
    // of the instruction it stopped at before returning the sentinel.
    virtual int32_t resume_ip_offset() const = 0;
    // Byte offset of the 64-bit flag that says a helper parked an exception
    // to be rethrown at the resume point. A caller that propagates a direct
    // callee's sentinel clears it: the re-run call raises its own.
    virtual int32_t exc_valid_offset() const = 0;
    // Byte offset of the `Value*` that begins the VM's value stack, and of
    // the size_t count of Values in use on it. Direct calls place the
    // callee's frame at the top and pop it on return.
    virtual int32_t value_stack_data_offset() const = 0;
    virtual int32_t stack_top_offset() const = 0;
    // Total Values the value stack holds; a frame that would exceed it deopts
    // so the interpreter raises its own overflow error. 0 when unknown.
    virtual int64_t stack_capacity() const = 0;
    // Native body of func_table[fidx], compiling it first if necessary. False
    // when it cannot be entered directly: unsupported body, executable
    // defaults, a frame too large for the machine stack, or a cycle.
    virtual bool resolve_callee(int fidx, BaselineCalleeInfo& out) = 0;
    // Keep the object behind `bits` alive for as long as generated code may
    // compare against it. An identity guard compares raw bits, so an address
    // reused by a later allocation would otherwise pass for the old object.
    virtual void pin_value(uint64_t bits) = 0;
};

// Shared whole-body verification for the baseline native tiers. The baseline
// emitters translate one bytecode instruction at a time and never merge
// control flow, so everything that needs whole-body reasoning lives here:
// opcode support, operand ranges, jump targets, per-point register kinds, and
// the provably-nonzero-divisor rule. An emitter may assume every reachable
// instruction of an accepted body is encodable and type-proven.
class BaselineBodyAnalysis {
public:
    // What a register is proven to hold at one program point. Real bytecode
    // reuses scratch registers across kinds (a comparison result, then an
    // arithmetic result), so kinds are tracked per instruction with a forward
    // dataflow pass instead of one kind per register for the whole body.
    // Closure carries the function index the register is proven to name (a
    // LOAD_GLOBAL whose identity guard passed). Conflict means "unknown
    // contents": it is still legal to copy or return such bits (the
    // interpreter would move the same 64 bits), but any operation that
    // interprets them (arithmetic, comparison, branch condition, call)
    // refuses to compile.
    enum Kind : uint8_t { Num, Bool, Other, Closure, Conflict };
    struct RegKind {
        Kind    k    = Conflict;
        int32_t fidx = -1;
        bool operator==(const RegKind& o) const { return k == o.k && fidx == o.fidx; }
        bool operator!=(const RegKind& o) const { return !(*this == o); }
    };
    static constexpr RegKind kNum{Num, -1};
    static constexpr RegKind kBool{Bool, -1};
    static constexpr RegKind kOther{Other, -1};
    static constexpr RegKind kConflict{Conflict, -1};

    static RegKind merge_kind(RegKind a, RegKind b) {
        return a == b ? a : kConflict;
    }

    // Per LOAD_GLOBAL: the run-time check that makes the compile-time value
    // trustworthy. Identity compares the slot's raw bits against the closure
    // seen at compile time; NumTag only checks the NaN-box number tag, which
    // is all a numeric global needs. Raw reads a written slot of any other
    // kind with no check (its contents are unknown to the analysis), and
    // Helper asks the interpreter's helper for a slot that is not written
    // yet, since only it knows whether that is an error.
    struct GlobalGuard {
        enum Mode : uint8_t { None, Identity, NumTag, Raw, Helper };
        Mode     mode  = None;
        int32_t  index = -1;
        uint64_t bits  = 0;
        int32_t  fidx  = -1;
    };
    // Per CALL_FUNC: how to reach the callee. `self` is a recursive call
    // into the body being compiled; otherwise `callee` is a compiled body
    // resolved through the link context. `args_num` means every argument is
    // a proven number, so the callee's entry guards can be skipped.
    struct DirectCall {
        bool valid    = false;
        bool self     = false;
        bool args_num = false;
        int32_t fidx  = -1;
        BaselineCalleeInfo callee;
    };

    // Largest callee frame (in Values) a direct call may place on the
    // value stack. With the interpreter's 512-frame limit this bounds what
    // a native recursion can consume to a quarter of the stack.
    static constexpr uint32_t kMaxDirectCalleeRegs = 64;

    // Whether parameter `p` is entry-guarded under `mask`
    // (NativeFunc::baseline_guard_mask).
    static bool guarded_bit(uint64_t mask, uint32_t p) {
        return p >= 64U || ((mask >> p) & 1ULL) != 0;
    }

private:
    std::vector<RegKind> kinds_;
    size_t entry_ip_ = 0;
    size_t nregs_ = 0;
    BaselineLinkContext* link_ = nullptr;
    int self_fidx_ = -1;
    bool self_callable_ = false;
    uint64_t guard_mask_ = ~0ULL;
    bool allow_helpers_ = false;
    // Proof-only mode (the Win64 full tier): the analysis never refuses a
    // body. Opcodes the baseline cannot emit are treated as the
    // interpreter's own, contributing only what they may write, so the
    // numeric proof survives arrays, calls and objects and the tier can drop
    // its per-operation guards around them.
    bool proof_only_ = false;
    std::vector<std::pair<int, std::pair<bool, BaselineCalleeInfo>>> callee_memo_;

    // What an opcode the proof does not interpret may write:
    //   0 nothing, 1 register a, 2 registers a and b, -1 unknown (everything).
    static int proof_write_shape(JitOp op) {
        switch (op) {
            case JitOp::STORE_GLOBAL: case JitOp::STORE_UPVAL:
            case JitOp::INDEX_SET: case JitOp::DOT_SET:
            case JitOp::PRINT: case JitOp::PRINT_NO_NL:
                return 0;
            case JitOp::LOAD_GLOBAL: case JitOp::LOAD_UPVAL:
            case JitOp::CALL_FUNC: case JitOp::CALL_BUILTIN: case JitOp::METHOD_CALL:
            case JitOp::MAKE_ARRAY: case JitOp::MAKE_DICT:
            case JitOp::INDEX_GET: case JitOp::DOT_GET: case JitOp::OP_IN:
            case JitOp::DICT_KEYS: case JitOp::MAKE_LAMBDA:
            case JitOp::BIT_AND: case JitOp::BIT_OR: case JitOp::BIT_XOR:
            case JitOp::LSHIFT: case JitOp::RSHIFT: case JitOp::BIT_NOT:
                return 1;
            case JitOp::FOREACH_NEXT:
                return 2;
            default:
                return -1;
        }
    }
    static bool proof_interprets(JitOp op) {
        switch (op) {
            case JitOp::NOP: case JitOp::LOAD_CONST: case JitOp::LOAD_NIL: case JitOp::LOAD_BOOL:
            case JitOp::MOVE: case JitOp::NEG: case JitOp::LOGICAL_NOT:
            case JitOp::ADD: case JitOp::SUB: case JitOp::MUL: case JitOp::DIV: case JitOp::MOD:
            case JitOp::CMP_EQ: case JitOp::CMP_NEQ: case JitOp::CMP_LT: case JitOp::CMP_LTE:
            case JitOp::CMP_GT: case JitOp::CMP_GTE:
            case JitOp::JUMP: case JitOp::JUMP_IF_FALSE: case JitOp::JUMP_IF_TRUE:
            case JitOp::RETURN_VAL: case JitOp::RETURN_NONE: case JitOp::HALT:
                return true;
            default:
                return false;
        }
    }

    const BaselineCalleeInfo* callee(int fidx) {
        for (auto& entry : callee_memo_)
            if (entry.first == fidx) return entry.second.first ? &entry.second.second : nullptr;
        BaselineCalleeInfo info;
        bool okc = link_ && link_->resolve_callee(fidx, info) &&
                   info.guarded_entry && info.unguarded_entry &&
                   info.frame_regs > 0 && info.frame_regs <= kMaxDirectCalleeRegs;
        callee_memo_.push_back({fidx, {okc, info}});
        return okc ? &callee_memo_.back().second.second : nullptr;
    }

public:
    bool ok = false;
    // Per body instruction: 1 when control can reach it. Emitters skip
    // unreachable instructions (typically the compiler's trailing
    // RETURN_NONE sentinel after an explicit return), which nothing can
    // branch to, so skipping them keeps the generated code identical to the
    // straight-line tier for bodies without branches.
    std::vector<uint8_t> reached;
    // Per body instruction: 1 when a DIV needs a runtime zero-divisor guard
    // because the divisor is numeric but not a proven-nonzero constant. Only
    // set when the caller allows deopt, since the guard's escape route is the
    // deopt sentinel and the interpreter's exact [E202] error.
    std::vector<uint8_t> div_zero_guard;
    // Per body instruction: guard for a LOAD_GLOBAL / target of a CALL_FUNC.
    // Only filled for reachable instructions of those opcodes.
    std::vector<GlobalGuard> global_guard;
    std::vector<DirectCall>  direct_call;
    // Kind of every value the body can return: the merge over reachable
    // RETURN_VAL operands, with RETURN_NONE, HALT and falling off the end
    // contributing nil (Other). A recursive body assumes its own calls return
    // Num and is only accepted with that kind if the assumption proves itself.
    RegKind return_kind = kOther;
    // True when the body reads VM state (globals, the recursion budget) or
    // calls a helper, and therefore needs the vm pointer at run time.
    bool uses_vm = false;
    // Per body instruction: 1 when the emitter must route the instruction
    // through its interpreter helper - operands not proven numeric, or an
    // opcode the baseline never inlines. Only set when helpers are allowed.
    std::vector<uint8_t> dynamic;
    // True when no reachable instruction has an effect outside the body's
    // own registers and fresh allocations. Only a pure body may be a direct
    // callee (see BaselineCalleeInfo): a deopt inside a direct call re-runs
    // the whole call in the interpreter, which must not repeat a print, a
    // store or a call to unknown code.
    bool pure = true;
    // True when the body may run a helper, directly or through a direct
    // callee. Only such a body needs its frame on the VM value stack when
    // it is a direct callee: helpers are the only place the collector runs
    // from native code, so a frame that can never reach one is invisible
    // to it anyway and stays on the machine stack, where a call costs no
    // value-stack bookkeeping. Self-calls consult the body's own flag.
    bool vm_frame = false;

    // require_provable_div=false lets a caller that has its own checked
    // division helper reuse the kind analysis without the divisor rule
    // rejecting the whole body. The full Win64 tier does exactly that: it
    // always routes DIV through sura_jit_checked_div, so the [E202] contract
    // does not depend on anything proven here.
    // link/self_fidx/self_callable enable LOAD_GLOBAL and CALL_FUNC: without
    // a link context both opcodes are refused as before. self_callable says
    // the body being compiled may be its own direct callee (exact argument
    // count, no argument-count register, small frame).
    // guard_mask: which of the guarded parameters are actually guarded
    // (guarded_bit); the others are unknown at entry.
    // allow_helpers: let instructions the baseline cannot inline (arrays,
    // strings, dictionaries, unproven arithmetic, prints, generic calls)
    // compile as helper calls that may deopt with a resume point. Needs a
    // link context that vouches for the resume and value-stack slots.
    BaselineBodyAnalysis(const JitChunk& chunk, size_t entry_ip, size_t end_ip,
                         uint32_t frame_regs, uint32_t guarded_params,
                         uint32_t reg_index_limit, int const_index_limit,
                         bool allow_runtime_deopt,
                         bool require_provable_div = true,
                         BaselineLinkContext* link = nullptr,
                         int self_fidx = -1,
                         bool self_callable = false,
                         uint64_t guard_mask = ~0ULL,
                         bool allow_helpers = false,
                         bool proof_only = false)
        : link_(link), self_fidx_(self_fidx), self_callable_(self_callable),
          guard_mask_(guard_mask), allow_helpers_(allow_helpers),
          proof_only_(proof_only) {
        if (entry_ip >= end_ip || end_ip > chunk.code.size() ||
            frame_regs == 0 || guarded_params > frame_regs ||
            (guarded_params > 0 && !allow_runtime_deopt)) {
            return;
        }
        // A recursive body is first analysed under the assumption that its
        // own calls return numbers. If every return then proves numeric the
        // assumption is inductively justified (a call either returns through
        // one of those returns or deopts, and the sentinel never lands in a
        // register). Otherwise fall back to "unknown", which is always sound.
        bool saw_self_call = false;
        run(chunk, entry_ip, end_ip, frame_regs, guarded_params, reg_index_limit,
            const_index_limit, allow_runtime_deopt, require_provable_div,
            kNum, saw_self_call);
        if (ok && saw_self_call && return_kind != kNum) {
            ok = false;
            run(chunk, entry_ip, end_ip, frame_regs, guarded_params, reg_index_limit,
                const_index_limit, allow_runtime_deopt, require_provable_div,
                kConflict, saw_self_call);
        }
        // An impure body cannot be a direct callee, not even its own: its
        // recursive calls go through the generic call helper instead.
        if (ok && saw_self_call && !pure && self_callable_) {
            ok = false;
            self_callable_ = false;
            run(chunk, entry_ip, end_ip, frame_regs, guarded_params, reg_index_limit,
                const_index_limit, allow_runtime_deopt, require_provable_div,
                kConflict, saw_self_call);
        }
    }

private:
    void run(const JitChunk& chunk, size_t entry_ip, size_t end_ip,
             uint32_t frame_regs, uint32_t guarded_params,
             uint32_t reg_index_limit, int const_index_limit,
             bool allow_runtime_deopt, bool require_provable_div,
             RegKind self_return_hypothesis, bool& saw_self_call) {
        const size_t body_len = end_ip - entry_ip;
        const size_t nregs = frame_regs;
        reached.assign(body_len, 0);
        div_zero_guard.assign(body_len, 0);
        global_guard.assign(body_len, GlobalGuard{});
        direct_call.assign(body_len, DirectCall{});
        dynamic.assign(body_len, 0);
        pure = true;
        return_kind = kOther;
        uses_vm = false;
        saw_self_call = false;
        bool any_return = false;
        auto valid_reg = [&](uint16_t reg) {
            return static_cast<uint32_t>(reg) < frame_regs &&
                   static_cast<uint32_t>(reg) <= reg_index_limit;
        };
        auto valid_const = [&](int index) {
            return index >= 0 && index <= const_index_limit &&
                   static_cast<size_t>(index) < chunk.constants.size();
        };
        auto valid_target = [&](int target) {
            return target >= 0 && static_cast<size_t>(target) >= entry_ip &&
                   static_cast<size_t>(target) < end_ip;
        };
        // Linked opcodes escape through the deopt sentinel, and need VM
        // state the link context has vouched for: the globals vector and
        // budget for guards and calls, the resume/exception slots and the
        // value stack for helper calls and value-stack callee frames.
        const bool linked = link_ != nullptr && allow_runtime_deopt &&
                            link_->globals_vector_offset() >= 0 &&
                            link_->depth_budget_offset() >= 0 &&
                            link_->resume_ip_offset() >= 0 &&
                            link_->exc_valid_offset() >= 0 &&
                            link_->value_stack_data_offset() >= 0 &&
                            link_->stack_top_offset() >= 0 &&
                            link_->stack_capacity() > 0;
        const bool helpers_ok = linked && allow_helpers_;

        // ── Pass 1: opcode support, operand ranges, and jump targets. ──
        // For division the divisor must be provably nonzero on every path:
        // only registers whose every write is a nonzero numeric constant
        // qualify. Guarded parameters prove "number", never "nonzero".
        std::vector<uint8_t> wrote_nonzero_const(nregs, 1);
        std::vector<uint8_t> wrote_only_const(nregs, 1);
        std::vector<uint8_t> wrote_anything(nregs, 0);
        auto wrote_dynamic = [&](uint16_t reg) {
            wrote_anything[reg] = 1;
            wrote_nonzero_const[reg] = 0;
            wrote_only_const[reg] = 0;
        };
        // `count` consecutive registers from `first`, all in range.
        auto valid_run = [&](uint32_t first, int count) {
            if (count < 0 || count > 0xFFFF) return false;
            if (first + static_cast<uint32_t>(count) > frame_regs) return false;
            for (int i = 0; i < count; ++i)
                if (!valid_reg(static_cast<uint16_t>(first + static_cast<uint32_t>(i)))) return false;
            return true;
        };
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (proof_only_ && !proof_interprets(inst.op)) {
                if (inst.op == JitOp::FOREACH_NEXT && !valid_target(inst.operand)) return;
                const int shape = proof_write_shape(inst.op);
                if (shape == 1 || shape == 2) {
                    if (!valid_reg(inst.a)) return;
                    wrote_dynamic(inst.a);
                }
                if (shape == 2) {
                    if (!valid_reg(inst.b)) return;
                    wrote_dynamic(inst.b);
                }
                if (shape < 0) {
                    for (uint32_t r = 0; r < nregs; ++r) wrote_dynamic(static_cast<uint16_t>(r));
                }
                continue;
            }
            switch (inst.op) {
                case JitOp::NOP:
                    break;
                case JitOp::LOAD_CONST: {
                    if (!valid_reg(inst.a) || !valid_const(inst.operand)) return;
                    const Value& c = chunk.constants[static_cast<size_t>(inst.operand)];
                    wrote_anything[inst.a] = 1;
                    if (!(c.is_num() && c.as_num() != 0.0)) wrote_nonzero_const[inst.a] = 0;
                    break;
                }
                case JitOp::LOAD_NIL:
                case JitOp::LOAD_BOOL:
                    if (!valid_reg(inst.a)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::MOVE:
                case JitOp::NEG:
                case JitOp::LOGICAL_NOT:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV:
                case JitOp::MOD:
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::JUMP:
                    if (!valid_target(inst.operand)) return;
                    break;
                case JitOp::JUMP_IF_FALSE:
                case JitOp::JUMP_IF_TRUE:
                    if (!valid_target(inst.operand) || !valid_reg(inst.a)) return;
                    break;
                case JitOp::RETURN_VAL:
                    if (!valid_reg(inst.a)) return;
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    break;
                case JitOp::LOAD_GLOBAL: {
                    // The slot's current value decides what the body may
                    // assume; the emitted guard re-checks it on every
                    // execution. A closure of this chunk or a number gets a
                    // guard the analysis can build on; any other written
                    // value is read as it is (a written slot stays written,
                    // so the read cannot become an error later); a slot not
                    // written yet is left to the helper, which raises the
                    // interpreter's undefined-variable error or resolves a
                    // class or stdlib name.
                    if (!linked || !valid_reg(inst.a)) return;
                    uint64_t bits = 0;
                    GlobalGuard& g = global_guard[ip - entry_ip];
                    g.index = inst.operand;
                    if (!link_->global_value(inst.operand, bits)) {
                        if (!helpers_ok) return;
                        g.mode = GlobalGuard::Helper;
                    } else {
                        const Value v = Value::from_bits(bits);
                        g.bits = bits;
                        if (v.is_closure()) {
                            const int fidx = v.as_closure()->func_idx;
                            if (fidx < 0 || static_cast<size_t>(fidx) >= chunk.func_table.size()) return;
                            g.mode = GlobalGuard::Identity;
                            g.fidx = fidx;
                        } else if (v.is_num()) {
                            g.mode = GlobalGuard::NumTag;
                        } else {
                            if (!helpers_ok) return;
                            g.mode = GlobalGuard::Raw;
                        }
                    }
                    wrote_dynamic(inst.a);
                    break;
                }
                case JitOp::STORE_GLOBAL:
                    if (!helpers_ok || !valid_reg(inst.a)) return;
                    break;
                case JitOp::CALL_FUNC: {
                    if (!linked || !valid_reg(inst.a) || !valid_reg(inst.b)) return;
                    if (inst.operand < 0 || inst.operand > 0xFFFF) return;
                    if (!valid_run(inst.c, inst.operand)) return;
                    wrote_dynamic(inst.a);
                    break;
                }
                case JitOp::MAKE_ARRAY:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_run(inst.b, inst.operand)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::MAKE_DICT:
                    if (!helpers_ok || !valid_reg(inst.a) || inst.operand < 0 ||
                        inst.operand > 0x7FFF || !valid_run(inst.b, inst.operand * 2)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::INDEX_GET:
                case JitOp::INDEX_SET:
                case JitOp::OP_IN:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c)) return;
                    if (inst.op != JitOp::INDEX_SET) wrote_dynamic(inst.a);
                    break;
                case JitOp::DOT_GET:
                case JitOp::DOT_SET:
                case JitOp::DICT_KEYS:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_reg(inst.b)) return;
                    if (inst.op != JitOp::DOT_SET) wrote_dynamic(inst.a);
                    break;
                case JitOp::FOREACH_NEXT:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_reg(inst.b) ||
                        !valid_reg(inst.c) || !valid_target(inst.operand)) return;
                    wrote_dynamic(inst.a);
                    wrote_dynamic(inst.b);
                    break;
                case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                    if (!helpers_ok || !valid_run(inst.a, inst.operand)) return;
                    break;
                case JitOp::CALL_BUILTIN:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_run(inst.b, inst.operand)) return;
                    wrote_dynamic(inst.a);
                    break;
                case JitOp::METHOD_CALL:
                    if (!helpers_ok || !valid_reg(inst.a) || !valid_reg(inst.b) ||
                        !valid_run(static_cast<uint32_t>(inst.b) + 1U, inst.operand)) return;
                    wrote_dynamic(inst.a);
                    break;
                default:
                    return;
            }
        }

        // ── Pass 2: forward dataflow of per-point register kinds. ──
        // in_state[ip][reg] is the kind register `reg` holds just before the
        // instruction at `ip` runs. The entry state proves the guarded
        // parameters numeric and everything else unknown. Merges of unequal
        // kinds weaken toward Conflict, so the iteration is monotone over a
        // finite lattice and terminates.
        entry_ip_ = entry_ip;
        nregs_ = nregs;
        kinds_.assign(body_len * nregs, kConflict);
        std::vector<RegKind>& in_state = kinds_;
        auto state_at = [&](size_t ip) { return &in_state[(ip - entry_ip) * nregs]; };
        for (uint32_t p = 0; p < guarded_params; ++p)
            if (guarded_bit(guard_mask_, p)) state_at(entry_ip)[p] = kNum;
        reached[0] = 1;

        // What a call returns, given what the function register holds. A
        // callee resolved through the link is compiled (and its return kind
        // known) before the answer is used; anything else is unknown, and
        // pass 3 then routes the call through the generic helper (or
        // refuses it when helpers are not allowed).
        auto call_result = [&](RegKind fn) -> RegKind {
            if (fn.k != Closure) return kConflict;
            if (fn.fidx == self_fidx_) return self_callable_ ? self_return_hypothesis : kConflict;
            const BaselineCalleeInfo* ci = callee(fn.fidx);
            if (!ci) return kConflict;
            return RegKind{static_cast<Kind>(ci->return_kind), ci->return_fidx};
        };

        std::vector<RegKind> out(nregs);
        auto transfer = [&](size_t ip, RegKind* state) {
            const JitInst& inst = chunk.code[ip];
            if (proof_only_ && !proof_interprets(inst.op)) {
                const int shape = proof_write_shape(inst.op);
                if (shape == 1 || shape == 2) state[inst.a] = kConflict;
                if (shape == 2) state[inst.b] = kConflict;
                if (shape < 0) std::fill(state, state + nregs, kConflict);
                return;
            }
            switch (inst.op) {
                case JitOp::LOAD_CONST:
                    state[inst.a] =
                        chunk.constants[static_cast<size_t>(inst.operand)].is_num()
                            ? kNum : kOther;
                    break;
                case JitOp::LOAD_NIL:  state[inst.a] = kOther; break;
                case JitOp::LOAD_BOOL: state[inst.a] = kBool;  break;
                case JitOp::MOVE:      state[inst.a] = state[inst.b];  break;
                case JitOp::ADD:
                    // Two numbers add to a number; anything else is the
                    // helper's business (string concatenation, or an error).
                    state[inst.a] = (state[inst.b] == kNum && state[inst.c] == kNum)
                        ? kNum : kConflict;
                    break;
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV:
                case JitOp::MOD:
                case JitOp::NEG:
                    // Inlined on proven numbers; the helper throws on
                    // anything else, so a number is all that can come back.
                    state[inst.a] = kNum;
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE:
                case JitOp::LOGICAL_NOT:
                    state[inst.a] = kBool;
                    break;
                case JitOp::LOAD_GLOBAL: {
                    const GlobalGuard& g = global_guard[ip - entry_ip];
                    state[inst.a] = g.mode == GlobalGuard::Identity ? RegKind{Closure, g.fidx}
                                  : g.mode == GlobalGuard::NumTag   ? kNum
                                  : kConflict;
                    break;
                }
                case JitOp::CALL_FUNC:
                    state[inst.a] = call_result(state[inst.b]);
                    break;
                case JitOp::MAKE_ARRAY:
                case JitOp::MAKE_DICT:
                case JitOp::DICT_KEYS:
                    state[inst.a] = kOther;
                    break;
                case JitOp::INDEX_GET:
                case JitOp::DOT_GET:
                case JitOp::OP_IN:
                case JitOp::CALL_BUILTIN:
                case JitOp::METHOD_CALL:
                    state[inst.a] = kConflict;
                    break;
                case JitOp::FOREACH_NEXT:
                    // Continuing: the element and the advanced index. The
                    // exit edge leaves both registers as they were.
                    state[inst.a] = kConflict;
                    state[inst.b] = kNum;
                    break;
                default:
                    break;
            }
        };
        auto flow_into = [&](size_t succ_ip, const RegKind* from, bool& changed) {
            RegKind* dst = state_at(succ_ip);
            uint8_t& seen = reached[succ_ip - entry_ip];
            if (!seen) {
                seen = 1;
                std::copy(from, from + nregs, dst);
                changed = true;
                return;
            }
            for (size_t r = 0; r < nregs; ++r) {
                RegKind merged = merge_kind(dst[r], from[r]);
                if (merged != dst[r]) { dst[r] = merged; changed = true; }
            }
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t ip = entry_ip; ip < end_ip; ++ip) {
                if (!reached[ip - entry_ip]) continue;
                const JitInst& inst = chunk.code[ip];
                const RegKind* in = state_at(ip);
                std::copy(in, in + nregs, out.begin());
                transfer(ip, out.data());
                const bool falls_through =
                    inst.op != JitOp::JUMP && inst.op != JitOp::RETURN_VAL &&
                    inst.op != JitOp::RETURN_NONE && inst.op != JitOp::HALT;
                if (falls_through && ip + 1 < end_ip) {
                    flow_into(ip + 1, out.data(), changed);
                }
                if (inst.op == JitOp::JUMP || inst.op == JitOp::JUMP_IF_FALSE ||
                    inst.op == JitOp::JUMP_IF_TRUE) {
                    flow_into(static_cast<size_t>(inst.operand), out.data(), changed);
                }
                if (inst.op == JitOp::FOREACH_NEXT) {
                    flow_into(static_cast<size_t>(inst.operand), in, changed);
                }
            }
        }

        // ── Pass 3: every interpreting read must see the kind it needs, ──
        // or the instruction goes to its helper. MOVE and RETURN_VAL copy raw
        // bits and need no proof; unreachable instructions are never emitted,
        // so they are exempt too.
        auto to_helper = [&](size_t ip) -> bool {
            if (proof_only_) {
                dynamic[ip - entry_ip] = 1;
                return true;
            }
            if (!helpers_ok) return false;
            dynamic[ip - entry_ip] = 1;
            uses_vm = true;
            vm_frame = true;
            return true;
        };
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            if (!reached[ip - entry_ip]) continue;
            const JitInst& inst = chunk.code[ip];
            const RegKind* in = state_at(ip);
            if (proof_only_ && !proof_interprets(inst.op)) {
                dynamic[ip - entry_ip] = 1;
                pure = false;
                continue;
            }
            switch (inst.op) {
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                    if (in[inst.b] != kNum || in[inst.c] != kNum) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::DIV:
                    if (in[inst.b] != kNum || in[inst.c] != kNum) {
                        if (!to_helper(ip)) return;
                        break;
                    }
                    // A divisor proven to be a nonzero constant needs no check.
                    // Otherwise the emitter tests it at run time: division by
                    // zero must raise [E202], not yield an infinity, and only
                    // the interpreter can raise it, so the guard deopts.
                    if (!(wrote_anything[inst.c] && wrote_nonzero_const[inst.c])) {
                        // A divisor written only by constants is fully known
                        // here: if it is not provably nonzero it is zero, so a
                        // runtime guard would deopt on every execution. Let
                        // the helper raise the error instead, or reject.
                        if (wrote_anything[inst.c] && wrote_only_const[inst.c]) {
                            if (!to_helper(ip)) return;
                            break;
                        }
                        if (!require_provable_div) break;
                        if (!allow_runtime_deopt) return;
                        div_zero_guard[ip - entry_ip] = 1;
                    }
                    break;
                case JitOp::MOD:
                    if (!to_helper(ip)) return;
                    break;
                case JitOp::NEG:
                    if (in[inst.b] != kNum) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::LOGICAL_NOT:
                    if (in[inst.b] != kBool) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE:
                    if (in[inst.b] != kNum || in[inst.c] != kNum) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::JUMP_IF_FALSE:
                case JitOp::JUMP_IF_TRUE:
                    if (in[inst.a] != kBool) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::LOAD_GLOBAL:
                    uses_vm = true;
                    if (global_guard[ip - entry_ip].mode == GlobalGuard::Helper) {
                        if (!to_helper(ip)) return;
                    }
                    break;
                case JitOp::CALL_FUNC: {
                    // A direct call needs the function register to name one
                    // specific compiled body and the call to bind exactly its
                    // parameters: the interpreter fills missing arguments
                    // from defaults and records the argument count, neither
                    // of which a direct call reproduces. Every other call
                    // (unknown closure, constructor, stdlib function) goes
                    // through the dispatcher and makes the body impure.
                    const RegKind fn = in[inst.b];
                    const uint32_t argc = static_cast<uint32_t>(inst.operand);
                    DirectCall& dc = direct_call[ip - entry_ip];
                    uint64_t callee_mask = ~0ULL;
                    bool direct = false;
                    if (fn.k == Closure && fn.fidx == self_fidx_) {
                        if (self_callable_ &&
                            static_cast<size_t>(fn.fidx) < chunk.func_table.size() &&
                            chunk.func_table[static_cast<size_t>(fn.fidx)].params.size() == argc &&
                            frame_regs <= kMaxDirectCalleeRegs) {
                            dc.self = true;
                            saw_self_call = true;
                            callee_mask = guard_mask_;
                            direct = true;
                        }
                    } else if (fn.k == Closure) {
                        const BaselineCalleeInfo* ci = callee(fn.fidx);
                        if (ci && ci->params == argc) {
                            dc.callee = *ci;
                            callee_mask = ci->guard_mask;
                            if (ci->vm_frame) vm_frame = true;
                            direct = true;
                        }
                    }
                    if (!direct) {
                        pure = false;
                        if (!to_helper(ip)) return;
                        break;
                    }
                    dc.valid = true;
                    dc.fidx = fn.fidx;
                    // The callee's unguarded entry may be taken when every
                    // argument its guards would check is a proven number.
                    dc.args_num = true;
                    for (uint32_t i = 0; i < argc; ++i)
                        if (guarded_bit(callee_mask, i) && in[inst.c + i] != kNum) dc.args_num = false;
                    uses_vm = true;
                    break;
                }
                case JitOp::STORE_GLOBAL:
                case JitOp::INDEX_SET:
                case JitOp::DOT_SET:
                case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                case JitOp::CALL_BUILTIN:
                case JitOp::METHOD_CALL:
                    pure = false;
                    if (!to_helper(ip)) return;
                    break;
                case JitOp::MAKE_ARRAY:
                case JitOp::MAKE_DICT:
                case JitOp::INDEX_GET:
                case JitOp::DOT_GET:
                case JitOp::OP_IN:
                case JitOp::DICT_KEYS:
                case JitOp::FOREACH_NEXT:
                    if (!to_helper(ip)) return;
                    break;
                case JitOp::RETURN_VAL:
                    return_kind = any_return ? merge_kind(return_kind, in[inst.a]) : in[inst.a];
                    any_return = true;
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    return_kind = any_return ? merge_kind(return_kind, kOther) : kOther;
                    any_return = true;
                    break;
                default:
                    break;
            }
        }
        if (falls_off_end(chunk, entry_ip, end_ip)) {
            return_kind = any_return ? merge_kind(return_kind, kOther) : kOther;
        }
        ok = true;
    }

public:
    // True when `reg` provably holds a number just before `ip` executes.
    // Only meaningful when ok is set; a false answer means "not proven",
    // never "proven otherwise", so a caller may only use it to remove work.
    bool proven_num(size_t ip, uint16_t reg) const {
        if (!ok || ip < entry_ip_) return false;
        const size_t row = ip - entry_ip_;
        if (row >= reached.size() || !reached[row]) return false;
        if (static_cast<size_t>(reg) >= nregs_) return false;
        return kinds_[row * nregs_ + reg] == kNum;
    }

    // True when execution can fall off the end of the body: the last
    // reachable instruction does not unconditionally leave the function.
    bool falls_off_end(const JitChunk& chunk, size_t entry_ip, size_t end_ip) const {
        for (size_t ip = end_ip; ip > entry_ip; --ip) {
            if (!reached[ip - 1 - entry_ip]) continue;
            const JitOp last = chunk.code[ip - 1].op;
            return last != JitOp::JUMP && last != JitOp::RETURN_VAL &&
                   last != JitOp::RETURN_NONE && last != JitOp::HALT;
        }
        return true;
    }
};

// Linux x86-64 starts with a deliberately small, exception-free System V
// tier. It emits no helper calls, so C++ exceptions never need to unwind
// through dynamically generated code without registered DWARF CFI. The
// second revision adds branches (whole-body loops) and NaN-box entry
// guards on numeric parameters: a non-number argument returns a deopt
// sentinel and the call falls back to the register VM for that call. The
// third revision links pure bodies to each other: LOAD_GLOBAL of a closure
// or number becomes a guarded constant, and CALL_FUNC to such a closure
// becomes a native-to-native call whose callee frame lives on the machine
// stack. Callee bodies are pure, so no frame is ever visible to the GC or to
// exception unwinding, and a guard failure anywhere in the call tree simply
// propagates the sentinel up to the interpreter's call site.
// Calling conventions the x86-64 baseline can be entered through. The body
// itself is convention-neutral: it keeps R in RBX, the constants in R12 and
// the VM in R13, all callee-saved under both conventions. Only the entry
// (which argument register holds what) and the direct-call sequence differ.
// Win64 shadow space is not reserved: a baseline body only ever calls other
// baseline bodies, none of which touch it.
enum class X64BaselineAbi : uint8_t { SysV, Win64 };

// ── Loop register cache ─────────────────────────────────────────────────────
// Every tier keeps a Value in its register slot in memory, so a loop-carried
// chain like `i = i + 1` costs a store and a forwarded load per iteration on
// top of the addition. Inside an accepted loop the hottest registers also
// live in XMM registers. The scheme is write-through: every definition the
// cache emits writes both the slot and the XMM copy, so memory is always
// current and helpers, the collector, deopts and every operation the cache
// does not emit itself keep working unchanged; such an operation only has to
// refresh the copies it may have overwritten. A loop qualifies when nothing
// outside [header, backedge] jumps into it (the copies are loaded on the
// fall-in edge just before the header) and it contains arithmetic to speed
// up. Outer loops are tried first; a loop nested in an accepted loop shares
// its copies.
struct JitLoopCache {
    size_t header_ip = 0;
    size_t backedge_ip = 0;
    std::vector<int8_t> xmm_of;    // per register: XMM number or -1
    std::vector<uint16_t> regs;    // cached registers in XMM order
};

inline bool jit_is_branch_op(JitOp op) {
    return op == JitOp::JUMP || op == JitOp::JUMP_IF_FALSE ||
           op == JitOp::JUMP_IF_TRUE || op == JitOp::FOREACH_NEXT;
}

inline bool jit_is_cache_arith_op(JitOp op) {
    switch (op) {
        case JitOp::ADD: case JitOp::SUB: case JitOp::MUL:
        case JitOp::CMP_EQ: case JitOp::CMP_NEQ:
        case JitOp::CMP_LT: case JitOp::CMP_LTE:
        case JitOp::CMP_GT: case JitOp::CMP_GTE:
            return true;
        default:
            return false;
    }
}

// `reached` (optional, indexed by ip - entry_ip) skips dead loops. Registers
// at or above `reg_limit`, and `excluded_reg`, are never cached.
inline std::vector<JitLoopCache> jit_plan_loop_caches(
        const JitChunk& chunk, size_t entry_ip, size_t end_ip,
        const std::vector<uint8_t>* reached, uint32_t reg_limit,
        uint32_t excluded_reg, int xmm_first, int xmm_count,
        const char* tier_name) {
    std::vector<JitLoopCache> caches;
    if (xmm_count <= 0 || std::getenv("SURA_JIT_DISABLE_LOOP_CACHE")) return caches;
    struct Candidate { size_t header; size_t backedge; };
    std::vector<Candidate> candidates;
    for (size_t ip = entry_ip; ip < end_ip; ++ip) {
        const JitInst& inst = chunk.code[ip];
        if (inst.op != JitOp::JUMP || inst.operand < 0) continue;
        const size_t target = static_cast<size_t>(inst.operand);
        if (target < entry_ip || target >= ip) continue;
        if (reached && !(*reached)[target - entry_ip]) continue;
        candidates.push_back({target, ip});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& l, const Candidate& r) {
                  return (l.backedge - l.header) > (r.backedge - r.header);
              });
    for (const Candidate& cand : candidates) {
        bool covered = false;
        for (const JitLoopCache& done : caches) {
            const bool disjoint = cand.backedge < done.header_ip || cand.header > done.backedge_ip;
            if (!disjoint) { covered = true; break; }
        }
        if (covered) continue;
        bool valid = true;
        for (size_t ip = entry_ip; ip < end_ip && valid; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (!jit_is_branch_op(inst.op) || inst.operand < 0) continue;
            const size_t target = static_cast<size_t>(inst.operand);
            const bool target_inside = target >= cand.header && target <= cand.backedge;
            const bool source_inside = ip >= cand.header && ip <= cand.backedge;
            if (target_inside && !source_inside) valid = false;
        }
        if (!valid) continue;
        size_t max_reg = 0;
        bool has_arith = false;
        for (size_t ip = cand.header; ip <= cand.backedge; ++ip) {
            const JitInst& inst = chunk.code[ip];
            max_reg = std::max<size_t>(max_reg, std::max<size_t>(inst.a, std::max<size_t>(inst.b, inst.c)));
            if (jit_is_cache_arith_op(inst.op)) has_arith = true;
        }
        if (!has_arith) continue;
        // A register earns a copy when a number flows through it into or
        // out of arithmetic. The compiler spells `acc = acc + x` as
        // MOVE tmp, acc / ADD res, tmp, x / MOVE acc, res, so MOVEs carry
        // arithmetic values and their registers count too - but only when
        // the value they carry comes from arithmetic (forward scan: what
        // last defined the source) or goes to it (backward scan: what next
        // uses the destination). A call result moved into a variable, or a
        // variable moved into an argument slot, gets no copy: it would only
        // pay GPR/XMM transfers for nothing.
        std::vector<uint32_t> reads(max_reg + 1, 0), writes(max_reg + 1, 0);
        std::vector<uint8_t> eligible(max_reg + 1, 0);
        enum : uint8_t { kNone = 0, kArith = 1, kOther = 2 };
        auto numeric_op = [](JitOp op) {
            switch (op) {
                case JitOp::ADD: case JitOp::SUB: case JitOp::MUL:
                case JitOp::DIV: case JitOp::MOD: case JitOp::NEG:
                case JitOp::CMP_EQ: case JitOp::CMP_NEQ:
                case JitOp::CMP_LT: case JitOp::CMP_LTE:
                case JitOp::CMP_GT: case JitOp::CMP_GTE:
                    return true;
                default:
                    return false;
            }
        };
        auto reads_a = [](JitOp op) {
            switch (op) {
                case JitOp::INDEX_SET: case JitOp::DOT_SET: case JitOp::STORE_GLOBAL:
                case JitOp::STORE_UPVAL: case JitOp::JUMP_IF_FALSE: case JitOp::JUMP_IF_TRUE:
                case JitOp::RETURN_VAL: case JitOp::PRINT: case JitOp::PRINT_NO_NL:
                    return true;
                default:
                    return false;
            }
        };
        for (size_t ip = cand.header; ip <= cand.backedge; ++ip) {
            const JitInst& inst = chunk.code[ip];
            switch (inst.op) {
                case JitOp::LOAD_CONST: case JitOp::LOAD_NIL: case JitOp::LOAD_BOOL:
                    ++writes[inst.a]; break;
                case JitOp::MOVE:
                    ++writes[inst.a]; ++reads[inst.b]; break;
                default:
                    if (numeric_op(inst.op)) {
                        ++writes[inst.a]; ++reads[inst.b];
                        if (inst.op != JitOp::NEG) ++reads[inst.c];
                        eligible[inst.a] = 1; eligible[inst.b] = 1;
                        if (inst.op != JitOp::NEG) eligible[inst.c] = 1;
                    }
                    break;
            }
        }
        // Forward: the kind of value each register last received. Two
        // rounds so the end of the loop body feeds its beginning.
        std::vector<uint8_t> last_def(max_reg + 1, kNone);
        for (int round = 0; round < 2; ++round) {
            for (size_t ip = cand.header; ip <= cand.backedge; ++ip) {
                const JitInst& inst = chunk.code[ip];
                if (inst.op == JitOp::LOAD_CONST) {
                    last_def[inst.a] = kArith;
                } else if (inst.op == JitOp::MOVE) {
                    if (last_def[inst.b] == kArith) eligible[inst.a] = 1;
                    last_def[inst.a] = last_def[inst.b];
                } else if (numeric_op(inst.op)) {
                    last_def[inst.a] = kArith;
                } else if (jit_is_branch_op(inst.op) || inst.op == JitOp::NOP ||
                           reads_a(inst.op)) {
                    if (inst.op == JitOp::FOREACH_NEXT) {
                        last_def[inst.a] = kOther; last_def[inst.b] = kOther;
                    }
                } else if (inst.op == JitOp::LOAD_NIL || inst.op == JitOp::LOAD_BOOL) {
                    last_def[inst.a] = kOther;
                } else {
                    last_def[inst.a] = kOther;
                }
            }
        }
        // Backward: what next reads each register.
        std::vector<uint8_t> next_use(max_reg + 1, kNone);
        for (int round = 0; round < 2; ++round) {
            for (size_t ip = cand.backedge + 1; ip-- > cand.header;) {
                const JitInst& inst = chunk.code[ip];
                if (inst.op == JitOp::MOVE) {
                    if (next_use[inst.a] == kArith) eligible[inst.b] = 1;
                    next_use[inst.b] = next_use[inst.a] == kNone ? next_use[inst.b] : next_use[inst.a];
                    next_use[inst.a] = kNone;
                } else if (numeric_op(inst.op)) {
                    next_use[inst.a] = kNone;
                    next_use[inst.b] = kArith;
                    if (inst.op != JitOp::NEG) next_use[inst.c] = kArith;
                } else if (inst.op == JitOp::LOAD_CONST || inst.op == JitOp::LOAD_NIL ||
                           inst.op == JitOp::LOAD_BOOL) {
                    next_use[inst.a] = kNone;
                } else if (reads_a(inst.op)) {
                    next_use[inst.a] = kOther;
                    if (inst.op == JitOp::INDEX_SET || inst.op == JitOp::DOT_SET) {
                        next_use[inst.b] = kOther;
                        if (inst.op == JitOp::INDEX_SET) next_use[inst.c] = kOther;
                    }
                } else if (inst.op == JitOp::JUMP || inst.op == JitOp::NOP) {
                } else {
                    // Everything else writes a and reads through b, c and
                    // (for calls) the argument window starting at c.
                    next_use[inst.a] = kNone;
                    next_use[inst.b] = kOther;
                    next_use[inst.c] = kOther;
                    if (inst.op == JitOp::CALL_FUNC || inst.op == JitOp::METHOD_CALL ||
                        inst.op == JitOp::CALL_BUILTIN || inst.op == JitOp::MAKE_ARRAY ||
                        inst.op == JitOp::MAKE_DICT || inst.op == JitOp::PRINT) {
                        const uint32_t first = inst.op == JitOp::CALL_FUNC ? inst.c : inst.b;
                        const int count = inst.op == JitOp::MAKE_DICT ? inst.operand * 2
                                        : inst.op == JitOp::METHOD_CALL ? inst.operand + 1
                                        : inst.operand;
                        for (int i = 0; i < count && first + static_cast<uint32_t>(i) <= max_reg; ++i)
                            next_use[first + static_cast<uint32_t>(i)] = kOther;
                    }
                }
            }
        }
        std::vector<std::pair<uint32_t, uint16_t>> scored;
        for (size_t r = 0; r <= max_reg; ++r) {
            if (r >= reg_limit || r == excluded_reg || !eligible[r]) continue;
            const uint32_t uses = reads[r] + writes[r];
            if (uses == 0) continue;
            const uint32_t carried = (reads[r] && writes[r]) ? 64U : 0U;
            scored.push_back({uses + carried, static_cast<uint16_t>(r)});
        }
        if (scored.empty()) continue;
        std::sort(scored.begin(), scored.end(),
                  [](const std::pair<uint32_t, uint16_t>& l,
                     const std::pair<uint32_t, uint16_t>& r) {
                      return l.first != r.first ? l.first > r.first : l.second < r.second;
                  });
        JitLoopCache cache;
        cache.header_ip = cand.header;
        cache.backedge_ip = cand.backedge;
        cache.xmm_of.assign(max_reg + 1, -1);
        for (size_t k = 0; k < scored.size() && k < static_cast<size_t>(xmm_count); ++k) {
            cache.xmm_of[scored[k].second] = static_cast<int8_t>(xmm_first + static_cast<int>(k));
            cache.regs.push_back(scored[k].second);
        }
        caches.push_back(std::move(cache));
    }
    std::sort(caches.begin(), caches.end(),
              [](const JitLoopCache& l, const JitLoopCache& r) { return l.header_ip < r.header_ip; });
    if (std::getenv("SURA_JIT_DIAG")) {
        for (const JitLoopCache& cache : caches) {
            std::fprintf(stderr, "[jit] %s loop cache ip %zu..%zu: %zu register(s) in XMM%d..\n",
                         tier_name, cache.header_ip, cache.backedge_ip, cache.regs.size(), xmm_first);
        }
    }
    return caches;
}

// NaN-box sentinels (mirrored from value.hpp so we can bake them into code)
static constexpr uint64_t JIT_NBNIL   = 0x7FFC000000000000ULL;
static constexpr uint64_t JIT_NBFALSE = 0x7FFC000000000001ULL;
static constexpr uint64_t JIT_NBTRUE  = 0x7FFC000000000002ULL;
// NaN-box object mask constants
static constexpr uint64_t JIT_NBOBJ   = 0xFFFC000000000000ULL; // NBQNAN | NBSIGN
static constexpr uint64_t JIT_NBPMASK = 0x0003FFFFFFFFFFFFULL; // ~NBOBJ (lower 50 bits = ptr)

// ── GCInstance memory-layout offsets (libstdc++ / MinGW64) ─────────────────
// GCObject:   vtable*(8) + obj_type(4) + marked(1) + pad(3) = 16 bytes
// GCInstance uses SmallValueVec for fields. Its first member is a Value* data
// pointer, mirroring std::vector's data-pointer-at-offset-0 layout for JIT ICs.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const int32_t INST_OBJTYPE_OFFSET = (int32_t)__builtin_offsetof(GCInstance, obj_type);
static const int32_t INST_FIELDS_OFFSET  = (int32_t)__builtin_offsetof(GCInstance, fields);
static const int32_t INST_JITINFO_OFFSET = (int32_t)__builtin_offsetof(GCInstance, jit_info);
static const int32_t CLOSURE_FUNC_IDX_OFFSET = (int32_t)__builtin_offsetof(GCClosure, func_idx);
static const int32_t JITINST_IC_NATIVE_FN_OFFSET = (int32_t)__builtin_offsetof(JitInst, ic_native_fn);
static const int32_t JITINST_IC_NATIVE_FRAME_REGS_OFFSET =
    (int32_t)__builtin_offsetof(JitInst, ic_native_frame_regs);
#pragma GCC diagnostic pop
static constexpr int32_t VECTOR_DATA_OFFSET = 0;   // _M_start at vector base
static constexpr int32_t SMALL_VALUE_VEC_SIZE_OFFSET = 8; // data_ then size_ on Win64
static constexpr int8_t  OBJ_TYPE_INSTANCE  = 5;   // ObjType::INSTANCE enum value
static constexpr int8_t  OBJ_TYPE_FUNC      = 3;   // ObjType::FUNC enum value
static_assert(static_cast<int>(ObjType::INSTANCE) == OBJ_TYPE_INSTANCE,
              "native JIT INSTANCE tag must match ObjType");
static_assert(static_cast<int>(ObjType::FUNC) == OBJ_TYPE_FUNC,
              "native JIT FUNC tag must match ObjType");

// ── GCArray layout for the inline array-index fast path ─────────────────────
// GCArray is GCObject + std::vector<Value>. libstdc++ lays the vector out as
// {_M_start, _M_finish, _M_end_of_storage}; the fast path reads the first two
// to bounds-check without a call. The layout is probed once at runtime, so a
// different standard library simply keeps the helper call instead of
// reading garbage.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const int32_t ARRAY_OBJTYPE_OFFSET  = (int32_t)__builtin_offsetof(GCArray, obj_type);
static const int32_t ARRAY_ELEMENTS_OFFSET = (int32_t)__builtin_offsetof(GCArray, elements);
#pragma GCC diagnostic pop
static constexpr int32_t VECTOR_FINISH_OFFSET = 8;  // _M_finish follows _M_start
static constexpr int32_t VECTOR_CAP_OFFSET    = 16; // _M_end_of_storage
static constexpr int8_t  OBJ_TYPE_ARRAY = 1;        // ObjType::ARRAY enum value
static constexpr int8_t  OBJ_TYPE_DICT  = 2;        // ObjType::DICT enum value
static_assert(static_cast<int>(ObjType::ARRAY) == OBJ_TYPE_ARRAY,
              "native JIT ARRAY tag must match ObjType");
static_assert(static_cast<int>(ObjType::DICT) == OBJ_TYPE_DICT,
              "native JIT DICT tag must match ObjType");

// `d.has(k)` on a proven dict: one hash probe, no method-name dispatch. The
// key is converted with Value::to_str exactly as the builtin does.
extern "C" inline uint64_t sura_jit_dict_has(GCDict* dict, uint64_t key_bits) {
    const Value key = Value::from_bits(key_bits);
    const bool found = key.is_str()
        ? dict->elements.find(key.as_str_ref()) != dict->elements.end()
        : dict->elements.find(key.to_str()) != dict->elements.end();
    return found ? JIT_NBTRUE : JIT_NBFALSE;
}

inline bool jit_array_layout_verified() {
    static const bool ok = [] {
        if (std::getenv("SURA_JIT_DISABLE_ARRAY_IC")) return false;
        std::vector<Value> probe(3);
        probe.reserve(8);
        const char* base = reinterpret_cast<const char*>(&probe);
        Value* start = nullptr;
        Value* finish = nullptr;
        Value* cap = nullptr;
        std::memcpy(&start, base + VECTOR_DATA_OFFSET, sizeof start);
        std::memcpy(&finish, base + VECTOR_FINISH_OFFSET, sizeof finish);
        std::memcpy(&cap, base + VECTOR_CAP_OFFSET, sizeof cap);
        return start == probe.data() && finish == probe.data() + probe.size() &&
               cap == probe.data() + probe.capacity();
    }();
    return ok;
}

// ── Inline guards shared by every x64 tier ──────────────────────────────────
// The Win64 full tier and the x64 baseline (SysV on Linux, Win64 as the
// baseline-first tier) all keep the register file base in RBX, so the same
// byte sequences serve both. Clobbers RAX, RCX, RDX, R10.
inline void sura_x64_emit_object_receiver_guard(X64Emitter& em, int32_t container_off,
                                                int8_t obj_tag,
                                                std::vector<size_t>& slow_jmps) {
    em.mov_r_mem(XR::RAX, XR::RBX, container_off);
    em.mov_ri64(XR::R10, JIT_NBOBJ);
    em.mov_rr(XR::RCX, XR::RAX);
    em.and_rr(XR::RCX, XR::R10);
    em.cmp_rr(XR::RCX, XR::R10);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));
    em.mov_ri64(XR::R10, JIT_NBPMASK);
    em.and_rr(XR::RAX, XR::R10);
    em.cmp_mem32_imm8(XR::RAX, ARRAY_OBJTYPE_OFFSET, obj_tag);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));
}

// On exit RCX = &elements[idx] for an array receiver with an in-bounds,
// non-negative numeric key; every failing guard is queued in slow_jmps.
//   1. R[container] is an object            (bits & NBOBJ) == NBOBJ
//   2. obj_type == ARRAY
//   3. R[key] is a number                   (hi32 & 0x7ffc0000) != 0x7ffc0000
//   4. idx = (int64)trunc(key), 0 <= idx <= INT32_MAX  (one unsigned compare)
//   5. &elements[idx] < _M_finish            (bounds)
inline void sura_x64_emit_array_index_guard(X64Emitter& em, int32_t container_off,
                                            int32_t key_off,
                                            std::vector<size_t>& slow_jmps) {
    sura_x64_emit_object_receiver_guard(em, container_off, OBJ_TYPE_ARRAY, slow_jmps);
    static constexpr uint32_t TAG32 = 0x7ffc0000U;
    em.mov_r32_mem(XR::RCX, XR::RBX, key_off + 4);
    em.and_r32_imm32(XR::RCX, TAG32);
    em.cmp_r32_imm32(XR::RCX, TAG32);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::E));
    em.cvttsd2si_r_mem(XR::RCX, XR::RBX, key_off);
    em.cmp_r_imm32(XR::RCX, 0x7fffffff);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::A));
    em.shl_r_imm8(XR::RCX, 3);
    em.mov_r_mem(XR::RDX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_DATA_OFFSET);
    em.mov_r_mem(XR::R10, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET);
    em.add_rr(XR::RCX, XR::RDX);
    em.cmp_rr(XR::RCX, XR::R10);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::AE));
}

// a.push(v): store + pointer bump when the vector has spare capacity; leaves
// R[a] = receiver like the builtin. RAX = GCArray* is expected from the guard.
inline void sura_x64_emit_array_push_fast(X64Emitter& em, int32_t recv_off, int32_t arg_off,
                                          int32_t dst_off, std::vector<size_t>& slow_jmps) {
    em.mov_r_mem(XR::RDX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET);
    em.mov_r_mem(XR::R10, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_CAP_OFFSET);
    em.cmp_rr(XR::RDX, XR::R10);
    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::AE));
    em.mov_r_mem(XR::RCX, XR::RBX, arg_off);
    em.mov_mem_r(XR::RDX, 0, XR::RCX);
    em.add_r_imm32(XR::RDX, 8);
    em.mov_mem_r(XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET, XR::RDX);
    em.mov_r_mem(XR::RAX, XR::RBX, recv_off);
    em.mov_mem_r(XR::RBX, dst_off, XR::RAX);
}

// a.len(): (finish - start) / 8 as a double into R[dst]. RAX = GCArray*.
inline void sura_x64_emit_array_len_fast(X64Emitter& em, int32_t dst_off) {
    em.mov_r_mem(XR::RCX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET);
    em.mov_r_mem(XR::RDX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_DATA_OFFSET);
    em.not_r(XR::RDX);
    em.add_r_imm32(XR::RDX, 1);   // rdx = -start
    em.add_rr(XR::RCX, XR::RDX);  // rcx = finish - start (bytes)
    em.mov_ri64(XR::RDX, 3);
    em.mov_rr(XR::R10, XR::RCX);
    em.mov_rr(XR::RCX, XR::RDX);
    em.sar_r_cl(XR::R10);         // r10 = element count
    em.cvtsi2sd_x_r(XR::XMM0, XR::R10);
    em.movsd_mem_x(XR::RBX, dst_off, XR::XMM0);
}

// Which METHOD_CALL shapes the baseline tiers inline. Only the receiver
// kind is decided at run time by a guard; the name and arity are static.
constexpr int kInlineArrayPush = 1;
constexpr int kInlineArrayLen  = 2;
constexpr int kInlineDictHas   = 3;
inline int inline_method_kind(const JitChunk& chunk, const JitInst& inst) {
    if (inst.str_idx < 0) return 0;
    const std::string& name = chunk.get_string(inst.str_idx);
    if (inst.operand == 1 && (name == "push" || name == "append")) return kInlineArrayPush;
    if (inst.operand == 0 && (name == "len" || name == "size" || name == "length"))
        return kInlineArrayLen;
    if (inst.operand == 1 && (name == "has" || name == "contains")) return kInlineDictHas;
    return 0;
}

class SysVBaselineCompiler {
    const JitChunk& chunk;
    size_t entry_ip;
    size_t end_ip;
    uint32_t frame_regs;
    uint32_t guarded_params;
    bool deopt_allowed;
    BaselineLinkContext* link = nullptr;
    int self_fidx = -1;
    bool self_callable = false;
    uint64_t guard_mask = ~0ULL;
    bool allow_helpers = false;
    X64BaselineAbi abi = X64BaselineAbi::SysV;

    // Argument registers of the convention in use: (vm, R, consts).
    int arg_vm() const     { return abi == X64BaselineAbi::SysV ? XR::RDI : XR::RCX; }
    int arg_regs() const   { return abi == X64BaselineAbi::SysV ? XR::RSI : XR::RDX; }
    int arg_consts() const { return abi == X64BaselineAbi::SysV ? XR::RDX : XR::R8; }

    static int32_t off_r(uint16_t reg) {
        return static_cast<int32_t>(static_cast<uint32_t>(reg) * 8U);
    }
    static int32_t off_c(int index) {
        return static_cast<int32_t>(static_cast<uint32_t>(index) * 8U);
    }

public:
    static constexpr uint64_t kDeoptSentinel = SURA_JIT_DEOPT_SENTINEL;

    // Filled by compile_bytes() for the direct-call linkage of this body.
    size_t unguarded_entry_offset = 0;   // byte offset past the entry guards
    BaselineBodyAnalysis::RegKind return_kind = BaselineBodyAnalysis::kOther;
    bool pure = true;                    // BaselineBodyAnalysis::pure
    bool vm_frame = false;               // BaselineBodyAnalysis::vm_frame

    // guarded_param_regs: registers 0..n-1 hold caller arguments that the
    // prologue verifies are NaN-boxed numbers, deopting otherwise (except
    // the parameters the guard mask leaves unguarded).
    // allow_deopt: the caller re-runs a sentinel-returning call in the VM with
    // freshly bound arguments. Only the closure call sites do that, so the
    // top-level chunk (which cannot be replayed) and methods pass false and
    // get a body that never deopts. Passing guards without deopt is refused.
    SysVBaselineCompiler(const JitChunk& source, size_t begin, size_t end,
                         uint32_t registers, uint32_t guarded_param_regs = 0,
                         bool allow_deopt = true)
        : chunk(source), entry_ip(begin), end_ip(end), frame_regs(registers),
          guarded_params(guarded_param_regs), deopt_allowed(allow_deopt) {}

    // Enable LOAD_GLOBAL and CALL_FUNC through a link context. `fidx` is this
    // body's own index in func_table (-1 when unknown), and `callable` says
    // the body may be entered by a direct call with exactly its parameters.
    void set_link(BaselineLinkContext* ctx, int fidx, bool callable) {
        link = ctx;
        self_fidx = fidx;
        self_callable = callable;
    }

    // Select the convention the emitted code is entered through. Direct
    // callees resolved through the link must have been compiled for the
    // same convention.
    void set_abi(X64BaselineAbi a) { abi = a; }

    // See BaselineBodyAnalysis: which guarded parameters are checked, and
    // whether non-numeric instructions may compile as helper calls.
    void set_guard_mask(uint64_t mask) { guard_mask = mask; }
    void set_allow_helpers(bool allow) { allow_helpers = allow; }

    std::vector<uint8_t> compile_bytes() {
        if (entry_ip >= end_ip || end_ip > chunk.code.size() || frame_regs == 0 ||
            frame_regs > std::numeric_limits<uint16_t>::max()) {
            return {};
        }
        const size_t body_len = end_ip - entry_ip;
        BaselineBodyAnalysis analysis(
            chunk, entry_ip, end_ip, frame_regs, guarded_params,
            std::numeric_limits<uint16_t>::max(),
            std::numeric_limits<int32_t>::max() / 8, deopt_allowed,
            /*require_provable_div=*/true, link, self_fidx, self_callable,
            guard_mask, allow_helpers);
        if (!analysis.ok) return {};
        return_kind = analysis.return_kind;
        pure = analysis.pure;
        vm_frame = analysis.vm_frame;
        const bool needs_vm = analysis.uses_vm;
        const int32_t globals_off = needs_vm ? link->globals_vector_offset() : 0;
        const int32_t budget_off = needs_vm ? link->depth_budget_offset() : 0;
        const int32_t resume_off = needs_vm ? link->resume_ip_offset() : 0;
        const int32_t exc_off = needs_vm ? link->exc_valid_offset() : 0;
        const int32_t vs_off = needs_vm ? link->value_stack_data_offset() : 0;
        const int32_t top_off = needs_vm ? link->stack_top_offset() : 0;
        const uint64_t capacity = needs_vm ? static_cast<uint64_t>(link->stack_capacity()) : 0;

        // ── Emit. ──
        std::vector<uint8_t> code;
        X64Emitter em(code);

        // Entry guards come before the prologue so a direct caller that has
        // already proven its arguments numeric can enter past them. They
        // read R through its argument register and escape to a tail that has
        // nothing to pop. R10 is the scratch: it is caller-saved and not an
        // argument register under either convention, so the arguments stay
        // intact for the prologue. Every guarded parameter must be a
        // NaN-boxed number ((bits & NBQNAN) != NBQNAN); otherwise branch to
        // the deopt tail.
        std::vector<size_t> entry_deopt_fixups;
        for (uint32_t p = 0; p < guarded_params; ++p) {
            if (!BaselineBodyAnalysis::guarded_bit(guard_mask, p)) continue;
            em.mov_r_mem(XR::RAX, arg_regs(), off_r(static_cast<uint16_t>(p)));
            em.mov_ri64(XR::R10, NBQNAN);
            em.and_rr(XR::RAX, XR::R10);
            em.cmp_rr(XR::RAX, XR::R10);
            entry_deopt_fixups.push_back(em.jcc_rel32_placeholder(CC::E));
        }

        // Loop register cache (JitLoopCache). Under Win64 the copies live in
        // the callee-saved XMM6..XMM15, saved here and restored by every
        // return, so helper calls and direct calls leave them intact. Under
        // System V every XMM is caller-saved: nothing is saved, and every
        // call refreshes every copy from memory afterwards.
        const bool xmm_preserved = abi == X64BaselineAbi::Win64;
        const int cache_xmm_first = xmm_preserved ? XR::XMM6 : XR::XMM2;
        const int cache_xmm_count = xmm_preserved ? 10 : 14;
        const std::vector<JitLoopCache> loop_caches = jit_plan_loop_caches(
            chunk, entry_ip, end_ip, &analysis.reached, frame_regs,
            std::numeric_limits<uint32_t>::max(), cache_xmm_first, cache_xmm_count,
            xmm_preserved ? "win64-baseline" : "sysv-baseline");
        int xmm_saved = 0;
        if (xmm_preserved) {
            for (const JitLoopCache& cache : loop_caches)
                xmm_saved = std::max<int>(xmm_saved, static_cast<int>(cache.regs.size()));
        }
        const int32_t cache_frame_bytes = 16 * xmm_saved;

        unguarded_entry_offset = em.pos();
        em.push_r(XR::RBX);
        em.push_r(XR::R12);
        if (needs_vm) em.push_r(XR::R13);
        if (xmm_saved > 0) {
            if (cache_frame_bytes <= 127) em.sub_rsp_imm8(static_cast<int8_t>(cache_frame_bytes));
            else em.sub_rsp_imm32(cache_frame_bytes);
            for (int k = 0; k < xmm_saved; ++k)
                em.movdqu_mem_x(XR::RSP, 16 * k, cache_xmm_first + k);
        }
        em.mov_rr(XR::RBX, arg_regs());
        em.mov_rr(XR::R12, arg_consts());
        if (needs_vm) em.mov_rr(XR::R13, arg_vm());

        auto emit_return = [&]() {
            if (xmm_saved > 0) {
                for (int k = 0; k < xmm_saved; ++k)
                    em.movdqu_x_mem(cache_xmm_first + k, XR::RSP, 16 * k);
                if (cache_frame_bytes <= 127) em.add_rsp_imm8(static_cast<int8_t>(cache_frame_bytes));
                else em.add_rsp_imm32(cache_frame_bytes);
            }
            if (needs_vm) em.pop_r(XR::R13);
            em.pop_r(XR::R12);
            em.pop_r(XR::RBX);
            em.ret();
        };

        // Deopt sites. A leaf body (no vm pointer) shares one tail that
        // hands back the sentinel; the interpreter re-runs the whole call.
        // A body with the vm pointer may have side effects, so each site
        // records the instruction to resume at (and whether a parked
        // exception belongs to it) before returning the sentinel.
        std::vector<size_t> deopt_fixups;
        struct DeoptSite { size_t disp_pos; size_t ip; bool discard_exc; };
        std::vector<DeoptSite> deopt_sites;
        auto deopt_here = [&](uint8_t cc, size_t ip, bool discard_exc) {
            if (needs_vm) {
                deopt_sites.push_back({em.jcc_rel32_placeholder(cc), ip, discard_exc});
            } else {
                deopt_fixups.push_back(em.jcc_rel32_placeholder(cc));
            }
        };
        // Call an interpreter helper for the instruction at `ip`. The
        // helper returns the sentinel when it parked an exception; the body
        // then resumes at `ip`, where the interpreter rethrows it. RSP is
        // 16-byte aligned here (return address plus three pushes), and the
        // Win64 convention wants 32 bytes of shadow space below it.
        auto emit_helper = [&](uint64_t (*fn)(JitVM*, Value*, const JitInst*), size_t ip) {
            em.mov_rr(arg_vm(), XR::R13);
            em.mov_rr(arg_regs(), XR::RBX);
            em.mov_ri64(arg_consts(), static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(&chunk.code[ip])));
            if (abi == X64BaselineAbi::Win64) em.sub_rsp_imm8(32);
            em.mov_ri64(XR::RAX, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn)));
            em.call_rax();
            if (abi == X64BaselineAbi::Win64) em.add_rsp_imm8(32);
            em.mov_ri64(XR::RCX, kDeoptSentinel);
            em.cmp_rr(XR::RAX, XR::RCX);
            deopt_here(CC::E, ip, false);
        };
        auto helper_store = [&](uint64_t (*fn)(JitVM*, Value*, const JitInst*), size_t ip,
                                uint16_t dst) {
            emit_helper(fn, ip);
            em.mov_mem_r(XR::RBX, off_r(dst), XR::RAX);
        };
        std::vector<size_t> ip_off(body_len, 0);
        struct JumpFixup { size_t disp_pos; size_t target_ip; };
        std::vector<JumpFixup> jump_fixups;
        // Self-recursive calls: (disp position, enter past the guards)
        struct SelfCallFixup { size_t disp_pos; bool unguarded; };
        std::vector<SelfCallFixup> self_call_fixups;
        const bool inline_collections = allow_helpers && jit_array_layout_verified();
        // Branch to `slow` unless R[off] is a NaN-boxed number: the high
        // 32 bits carry the whole tag, so a 32-bit compare suffices.
        auto emit_non_number_to = [&](int32_t off, std::vector<size_t>& slow) {
            static constexpr uint32_t TAG32 = 0x7ffc0000U;
            em.mov_r32_mem(XR::RCX, XR::RBX, off + 4);
            em.and_r32_imm32(XR::RCX, TAG32);
            em.cmp_r32_imm32(XR::RCX, TAG32);
            slow.push_back(em.jcc_rel32_placeholder(CC::E));
        };

        // ── Loop register cache: emission ──
        const JitLoopCache* cache = nullptr;
        auto inst_index_of = [&](const JitInst& inst) -> size_t {
            return static_cast<size_t>(&inst - chunk.code.data()) - entry_ip;
        };
        auto cached_xmm = [&](uint16_t r) -> int {
            if (cache == nullptr || r >= cache->xmm_of.size()) return -1;
            return cache->xmm_of[r];
        };
        auto reload_cached = [&](uint16_t r) {
            const int x = cached_xmm(r);
            if (x >= 0) em.movsd_x_mem(x, XR::RBX, off_r(r));
        };
        auto reload_all_cached = [&]() {
            if (cache == nullptr) return;
            for (uint16_t r : cache->regs) reload_cached(r);
        };
        // A copy refreshed from RAX (which still holds the value most
        // instructions just stored) stays in registers; a reload from the
        // slot would wait on store forwarding, and a mixed GPR/SSE pair
        // forwards slower than the GPR-only pairs the CPU can rename away.
        auto refresh_from_rax = [&](uint16_t r) {
            const int x = cached_xmm(r);
            if (x >= 0) em.movq_x_r(x, XR::RAX);
        };
        // After a call that wrote R[written] (or nothing, when written is
        // 65535): the copies that survived the call stay, the rest reload.
        // `in_rax` says RAX still holds the written value.
        auto after_call = [&](uint16_t written, bool in_rax = true) {
            if (cache == nullptr) return;
            if (!xmm_preserved) {
                for (uint16_t r : cache->regs) {
                    if (r == written && in_rax) refresh_from_rax(r);
                    else reload_cached(r);
                }
            } else if (written != 65535) {
                if (in_rax) refresh_from_rax(written); else reload_cached(written);
            }
        };
        auto load_operand_xmm0 = [&](uint16_t r) {
            const int x = cached_xmm(r);
            if (x >= 0) em.movaps_xx(XR::XMM0, x);
            else em.movsd_x_mem(XR::XMM0, XR::RBX, off_r(r));
        };
        auto arith_xmm0 = [&](JitOp op, uint16_t r) {
            const int x = cached_xmm(r);
            if (op == JitOp::ADD) {
                if (x >= 0) em.addsd_xx(XR::XMM0, x); else em.addsd_x_mem(XR::XMM0, XR::RBX, off_r(r));
            } else if (op == JitOp::SUB) {
                if (x >= 0) em.subsd_xx(XR::XMM0, x); else em.subsd_x_mem(XR::XMM0, XR::RBX, off_r(r));
            } else {
                if (x >= 0) em.mulsd_xx(XR::XMM0, x); else em.mulsd_x_mem(XR::XMM0, XR::RBX, off_r(r));
            }
        };
        auto ucomisd_xmm0 = [&](uint16_t r) {
            const int x = cached_xmm(r);
            if (x >= 0) em.ucomisd_xx(XR::XMM0, x);
            else em.ucomisd_x_mem(XR::XMM0, XR::RBX, off_r(r));
        };
        auto load_bits = [&](int gpr, uint16_t r) {
            const int x = cached_xmm(r);
            if (x >= 0) em.movq_r_x(gpr, x);
            else em.mov_r_mem(gpr, XR::RBX, off_r(r));
        };
        auto store_result_xmm0 = [&](uint16_t a) {
            em.movsd_mem_x(XR::RBX, off_r(a), XR::XMM0);
            const int x = cached_xmm(a);
            if (x >= 0) em.movaps_xx(x, XR::XMM0);
        };
        auto store_result_rax = [&](uint16_t a) {
            em.mov_mem_r(XR::RBX, off_r(a), XR::RAX);
            const int x = cached_xmm(a);
            if (x >= 0) em.movq_x_r(x, XR::RAX);
        };
        auto touches_cache = [&](const JitInst& inst) {
            return cached_xmm(inst.a) >= 0 || cached_xmm(inst.b) >= 0 || cached_xmm(inst.c) >= 0;
        };
        // The operations the cache emits itself; false hands the instruction
        // to the switch below, followed by reload_after.
        auto emit_cached = [&](const JitInst& inst, size_t ip, bool dyn) -> bool {
            switch (inst.op) {
                case JitOp::LOAD_CONST: {
                    const int x = cached_xmm(inst.a);
                    if (x < 0) return false;
                    em.movsd_x_mem(x, XR::R12, off_c(inst.operand));
                    em.movsd_mem_x(XR::RBX, off_r(inst.a), x);
                    return true;
                }
                case JitOp::LOAD_NIL:
                case JitOp::LOAD_BOOL: {
                    if (cached_xmm(inst.a) < 0) return false;
                    em.mov_ri64(XR::RAX, inst.op == JitOp::LOAD_NIL ? NBNIL
                                         : (inst.operand ? NBTRUE : NBFALSE));
                    store_result_rax(inst.a);
                    return true;
                }
                case JitOp::MOVE: {
                    const int xa = cached_xmm(inst.a);
                    const int xb = cached_xmm(inst.b);
                    if (xa < 0 && xb < 0) return false;
                    if (xb >= 0 && xa >= 0) {
                        em.movsd_mem_x(XR::RBX, off_r(inst.a), xb);
                        if (xa != xb) em.movaps_xx(xa, xb);
                    } else if (xb >= 0) {
                        // The destination has no copy, so nothing arithmetic
                        // reads it: copy the slot with a GPR pair (the slot
                        // is current), which the CPU renames when the value
                        // came from a call or a helper, instead of pulling
                        // it out of the XMM copy.
                        em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                        em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    } else {
                        em.movsd_x_mem(xa, XR::RBX, off_r(inst.b));
                        em.movsd_mem_x(XR::RBX, off_r(inst.a), xa);
                    }
                    return true;
                }
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL: {
                    if (!touches_cache(inst)) return false;
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(off_r(inst.b), slow);
                        emit_non_number_to(off_r(inst.c), slow);
                    }
                    load_operand_xmm0(inst.b);
                    arith_xmm0(inst.op, inst.c);
                    store_result_xmm0(inst.a);
                    if (dyn) {
                        const size_t done = em.jmp_rel32_placeholder();
                        for (size_t j : slow) em.patch_rel32(j, em.pos());
                        helper_store(&sura_bl_arith, ip, inst.a);
                        after_call(inst.a);
                        em.patch_rel32(done, em.pos());
                    }
                    return true;
                }
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE: {
                    if (!touches_cache(inst)) return false;
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(off_r(inst.b), slow);
                        emit_non_number_to(off_r(inst.c), slow);
                    }
                    // b < c is evaluated as c > b so an unordered compare
                    // (NaN) yields false, the way Value::lt does.
                    const bool swapped = inst.op == JitOp::CMP_LT || inst.op == JitOp::CMP_LTE;
                    load_operand_xmm0(swapped ? inst.c : inst.b);
                    ucomisd_xmm0(swapped ? inst.b : inst.c);
                    em.mov_ri64(XR::RAX, NBFALSE);
                    em.mov_ri64(XR::RCX, NBTRUE);
                    switch (inst.op) {
                        case JitOp::CMP_EQ:
                        case JitOp::CMP_NEQ: {
                            // Value::eq on numbers: identical bits, or an
                            // ordered equal compare (+0 == -0; a NaN equals
                            // only itself).
                            const bool neq = inst.op == JitOp::CMP_NEQ;
                            em.mov_ri64(XR::RAX, neq ? NBTRUE : NBFALSE);
                            em.mov_ri64(XR::RCX, neq ? NBFALSE : NBTRUE);
                            em.cmov_rr(CC::E, XR::RAX, XR::RCX);
                            em.mov_ri64(XR::RDX, neq ? NBTRUE : NBFALSE);
                            em.cmov_rr(CC::P, XR::RAX, XR::RDX);
                            load_bits(XR::RDX, inst.b);
                            load_bits(XR::R8, inst.c);
                            em.cmp_rr(XR::RDX, XR::R8);
                            em.cmov_rr(CC::E, XR::RAX, XR::RCX);
                            break;
                        }
                        case JitOp::CMP_LT:
                        case JitOp::CMP_GT:
                            em.cmov_rr(CC::A, XR::RAX, XR::RCX);
                            break;
                        default:
                            em.cmov_rr(CC::AE, XR::RAX, XR::RCX);
                            break;
                    }
                    store_result_rax(inst.a);
                    if (dyn) {
                        const size_t done = em.jmp_rel32_placeholder();
                        for (size_t j : slow) em.patch_rel32(j, em.pos());
                        helper_store(&sura_bl_arith, ip, inst.a);
                        after_call(inst.a);
                        em.patch_rel32(done, em.pos());
                    }
                    return true;
                }
                default:
                    return false;
            }
        };
        // Bring the copies back in line after an instruction the switch
        // emitted. Only instructions that can reach a call use after_call;
        // the ones that never call refresh just the register they wrote.
        auto reload_after = [&](const JitInst& inst, bool dyn) {
            if (cache == nullptr) return;
            switch (inst.op) {
                case JitOp::NOP: case JitOp::JUMP:
                case JitOp::RETURN_VAL: case JitOp::RETURN_NONE: case JitOp::HALT:
                    return;
                case JitOp::JUMP_IF_FALSE: case JitOp::JUMP_IF_TRUE:
                    if (dyn) after_call(65535);
                    return;
                // The switch only emits these when no operand is cached,
                // so the destination has no copy to refresh.
                case JitOp::LOAD_CONST: case JitOp::LOAD_NIL: case JitOp::LOAD_BOOL:
                case JitOp::MOVE:
                case JitOp::ADD: case JitOp::SUB: case JitOp::MUL:
                case JitOp::CMP_EQ: case JitOp::CMP_NEQ: case JitOp::CMP_LT:
                case JitOp::CMP_LTE: case JitOp::CMP_GT: case JitOp::CMP_GTE:
                    return;
                case JitOp::DIV: {
                    // Inline: result in XMM0. Dynamic: helper, result in RAX.
                    if (dyn) { after_call(inst.a); return; }
                    const int x = cached_xmm(inst.a);
                    if (x >= 0) em.movaps_xx(x, XR::XMM0);
                    return;
                }
                case JitOp::NEG: case JitOp::LOGICAL_NOT:
                    if (dyn) after_call(inst.a); else refresh_from_rax(inst.a);
                    return;
                case JitOp::LOAD_GLOBAL:
                    if (analysis.global_guard[inst_index_of(inst)].mode ==
                        BaselineBodyAnalysis::GlobalGuard::Helper) after_call(inst.a);
                    else refresh_from_rax(inst.a);
                    return;
                case JitOp::INDEX_GET: case JitOp::INDEX_SET:
                    // Both paths of INDEX_GET leave the element in RAX; the
                    // helper slow paths refresh the other copies themselves.
                    if (inst.op == JitOp::INDEX_GET) refresh_from_rax(inst.a);
                    return;
                case JitOp::STORE_GLOBAL: case JitOp::DOT_SET: case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                    after_call(65535);
                    return;
                case JitOp::MOD: case JitOp::MAKE_ARRAY: case JitOp::MAKE_DICT:
                case JitOp::DOT_GET: case JitOp::CALL_BUILTIN: case JitOp::CALL_FUNC:
                    after_call(inst.a);
                    return;
                case JitOp::METHOD_CALL:
                    // The inline push / len paths do not end in RAX.
                    after_call(inst.a, false);
                    return;
                default:
                    // OP_IN, DICT_KEYS, FOREACH_NEXT and anything new: the
                    // helper may write more than one register.
                    reload_all_cached();
                    return;
            }
        };

        // Instruction ips that some branch targets: a value left in XMM0 by
        // the previous instruction is only known there when control cannot
        // arrive from elsewhere.
        std::vector<uint8_t> branch_target(body_len, 0);
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (!jit_is_branch_op(inst.op) || inst.operand < 0) continue;
            const size_t target = static_cast<size_t>(inst.operand);
            if (target >= entry_ip && target < end_ip) branch_target[target - entry_ip] = 1;
        }
        int xmm0_holds_reg = -1;      // register whose value XMM0 holds
        size_t xmm0_holds_ip = 0;     // ip of the instruction that left it there

        size_t next_loop_cache = 0;
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            if (!analysis.reached[ip - entry_ip]) continue;
            if (next_loop_cache < loop_caches.size() &&
                loop_caches[next_loop_cache].header_ip == ip) {
                cache = &loop_caches[next_loop_cache++];
                for (uint16_t r : cache->regs)
                    em.movsd_x_mem(cache->xmm_of[r], XR::RBX, off_r(r));
            }
            ip_off[ip - entry_ip] = em.pos();
            const JitInst& inst = chunk.code[ip];
            const bool dyn = analysis.dynamic[ip - entry_ip] != 0;
            const bool xmm0_fresh = xmm0_holds_reg >= 0 && xmm0_holds_ip + 1 == ip &&
                                    !branch_target[ip - entry_ip];
            const int xmm0_reg = xmm0_fresh ? xmm0_holds_reg : -1;
            xmm0_holds_reg = -1;
            if (cache != nullptr && emit_cached(inst, ip, dyn)) {
                if (ip == cache->backedge_ip) cache = nullptr;
                continue;
            }
            switch (inst.op) {
                case JitOp::NOP:
                    break;
                case JitOp::LOAD_CONST:
                    em.mov_r_mem(XR::RAX, XR::R12, off_c(inst.operand));
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::LOAD_NIL:
                    em.mov_ri64(XR::RAX, NBNIL);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::LOAD_BOOL:
                    em.mov_ri64(XR::RAX, inst.operand ? NBTRUE : NBFALSE);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::MOVE:
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV: {
                    // Not proven numeric (a value read from a collection, a
                    // possible string concatenation, an error, or a division
                    // by a constant zero): check the NaN-box tags at run time
                    // and compute inline when both are numbers, otherwise the
                    // helper keeps the interpreter's exact semantics. A
                    // dynamic divisor is also required to be nonzero here so
                    // the helper raises [E202].
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(off_r(inst.b), slow);
                        emit_non_number_to(off_r(inst.c), slow);
                        if (inst.op == JitOp::DIV) {
                            em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.c));
                            em.mov_ri64(XR::RCX, 0x7FFFFFFFFFFFFFFFULL);
                            em.and_rr(XR::RAX, XR::RCX);
                            em.cmp_r_imm32(XR::RAX, 0);
                            slow.push_back(em.jcc_rel32_placeholder(CC::E));
                        }
                    } else if (analysis.div_zero_guard[ip - entry_ip]) {
                        // Division by zero must raise [E202]; the interpreter
                        // owns that error, so deopt instead of dividing.
                        // Clearing the sign bit makes -0.0 test equal to 0.0,
                        // matching the interpreter's `rhs == 0.0`; NaN keeps a
                        // nonzero payload and divides to NaN as it does there.
                        em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.c));
                        em.mov_ri64(XR::RCX, 0x7FFFFFFFFFFFFFFFULL);
                        em.and_rr(XR::RAX, XR::RCX);
                        em.cmp_r_imm32(XR::RAX, 0);
                        deopt_here(CC::E, ip, false);
                    }
                    em.movsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.b));
                    if (inst.op == JitOp::ADD) {
                        em.addsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    } else if (inst.op == JitOp::SUB) {
                        em.subsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    } else if (inst.op == JitOp::MUL) {
                        em.mulsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    } else {
                        em.divsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    }
                    em.movsd_mem_x(XR::RBX, off_r(inst.a), XR::XMM0);
                    if (dyn) {
                        const size_t done = em.jmp_rel32_placeholder();
                        for (size_t j : slow) em.patch_rel32(j, em.pos());
                        helper_store(&sura_bl_arith, ip, inst.a);
                        em.patch_rel32(done, em.pos());
                    } else {
                        xmm0_holds_reg = inst.a;
                        xmm0_holds_ip = ip;
                    }
                    break;
                }
                case JitOp::MOD:
                    helper_store(&sura_bl_arith, ip, inst.a);
                    break;
                case JitOp::NEG:
                    if (dyn) {
                        helper_store(&sura_bl_arith, ip, inst.a);
                        break;
                    }
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                    em.mov_ri64(XR::RCX, 0x8000000000000000ULL);
                    em.xor_rr(XR::RAX, XR::RCX);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::LOGICAL_NOT:
                    if (dyn) {
                        helper_store(&sura_bl_arith, ip, inst.a);
                        break;
                    }
                    // A proven Bool toggles between NBFALSE and NBTRUE.
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                    em.mov_ri64(XR::RCX, NBFALSE ^ NBTRUE);
                    em.xor_rr(XR::RAX, XR::RCX);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE: {
                    // Dynamic operands: numbers compare inline, anything else
                    // (string equality, the ordering type error) in the helper.
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(off_r(inst.b), slow);
                        emit_non_number_to(off_r(inst.c), slow);
                    }
                    em.movsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.b));
                    em.ucomisd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    em.mov_ri64(XR::RAX, NBFALSE);
                    em.mov_ri64(XR::RCX, NBTRUE);
                    switch (inst.op) {
                        case JitOp::CMP_EQ:
                            // Value::eq: an ordered equal compare, or
                            // identical bits (a NaN equals itself).
                            em.cmov_rr(CC::E, XR::RAX, XR::RCX);
                            em.mov_ri64(XR::RDX, NBFALSE);
                            em.cmov_rr(CC::P, XR::RAX, XR::RDX);
                            em.mov_r_mem(XR::RDX, XR::RBX, off_r(inst.b));
                            em.mov_r_mem(XR::R8, XR::RBX, off_r(inst.c));
                            em.cmp_rr(XR::RDX, XR::R8);
                            em.cmov_rr(CC::E, XR::RAX, XR::RCX);
                            break;
                        case JitOp::CMP_NEQ:
                            em.cmov_rr(CC::NE, XR::RAX, XR::RCX);
                            em.cmov_rr(CC::P, XR::RAX, XR::RCX);
                            em.mov_r_mem(XR::RDX, XR::RBX, off_r(inst.b));
                            em.mov_r_mem(XR::R8, XR::RBX, off_r(inst.c));
                            em.cmp_rr(XR::RDX, XR::R8);
                            em.mov_ri64(XR::RDX, NBFALSE);
                            em.cmov_rr(CC::E, XR::RAX, XR::RDX);
                            break;
                        case JitOp::CMP_LT:
                            em.cmov_rr(CC::B, XR::RAX, XR::RCX);
                            em.mov_ri64(XR::RDX, NBFALSE);
                            em.cmov_rr(CC::P, XR::RAX, XR::RDX);
                            break;
                        case JitOp::CMP_LTE:
                            em.cmov_rr(CC::BE, XR::RAX, XR::RCX);
                            em.mov_ri64(XR::RDX, NBFALSE);
                            em.cmov_rr(CC::P, XR::RAX, XR::RDX);
                            break;
                        case JitOp::CMP_GT:
                            em.cmov_rr(CC::A, XR::RAX, XR::RCX);
                            break;
                        default:
                            em.cmov_rr(CC::AE, XR::RAX, XR::RCX);
                            break;
                    }
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    if (dyn) {
                        const size_t done = em.jmp_rel32_placeholder();
                        for (size_t j : slow) em.patch_rel32(j, em.pos());
                        helper_store(&sura_bl_arith, ip, inst.a);
                        em.patch_rel32(done, em.pos());
                    }
                    break;
                }
                case JitOp::JUMP:
                    jump_fixups.push_back({em.jmp_rel32_placeholder(),
                                           static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::JUMP_IF_FALSE:
                case JitOp::JUMP_IF_TRUE:
                    if (dyn) {
                        // Not a proven Bool. A value that already is one
                        // (the usual case: a guarded comparison result) is
                        // used directly; anything else goes through the
                        // helper, which applies the interpreter's truthiness
                        // and hands back a Bool in RAX.
                        std::vector<size_t> is_bool;
                        em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.a));
                        em.mov_ri64(XR::RCX, NBFALSE);
                        em.cmp_rr(XR::RAX, XR::RCX);
                        is_bool.push_back(em.jcc_rel32_placeholder(CC::E));
                        em.mov_ri64(XR::RCX, NBTRUE);
                        em.cmp_rr(XR::RAX, XR::RCX);
                        is_bool.push_back(em.jcc_rel32_placeholder(CC::E));
                        emit_helper(&sura_bl_truthy, ip);
                        for (size_t j : is_bool) em.patch_rel32(j, em.pos());
                    } else {
                        em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.a));
                    }
                    em.mov_ri64(XR::RCX, inst.op == JitOp::JUMP_IF_FALSE ? NBFALSE : NBTRUE);
                    em.cmp_rr(XR::RAX, XR::RCX);
                    jump_fixups.push_back({em.jcc_rel32_placeholder(CC::E),
                                           static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::RETURN_VAL:
                    if (xmm0_reg == inst.a) em.movq_r_x(XR::RAX, XR::XMM0);
                    else load_bits(XR::RAX, inst.a);
                    emit_return();
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    em.mov_ri64(XR::RAX, NBNIL);
                    emit_return();
                    break;
                case JitOp::LOAD_GLOBAL: {
                    // Read the slot through the vector's begin pointer on
                    // every execution: the vector reallocates as globals are
                    // added, so only the vm-relative location is stable.
                    const auto& g = analysis.global_guard[ip - entry_ip];
                    if (g.mode == BaselineBodyAnalysis::GlobalGuard::Helper) {
                        helper_store(&sura_bl_load_global, ip, inst.a);
                        break;
                    }
                    em.mov_r_mem(XR::RAX, XR::R13, globals_off);
                    em.mov_r_mem(XR::RAX, XR::RAX, off_c(g.index));
                    if (g.mode == BaselineBodyAnalysis::GlobalGuard::Identity) {
                        em.mov_ri64(XR::RCX, g.bits);
                        em.cmp_rr(XR::RAX, XR::RCX);
                        deopt_here(CC::NE, ip, false);
                        link->pin_value(g.bits);
                    } else if (g.mode == BaselineBodyAnalysis::GlobalGuard::NumTag) {
                        em.mov_rr(XR::RCX, XR::RAX);
                        em.mov_ri64(XR::RDX, NBQNAN);
                        em.and_rr(XR::RCX, XR::RDX);
                        em.cmp_rr(XR::RCX, XR::RDX);
                        deopt_here(CC::E, ip, false);
                    }
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                }
                case JitOp::STORE_GLOBAL:
                    emit_helper(&sura_bl_store_global, ip);
                    break;
                case JitOp::MAKE_ARRAY:
                    helper_store(&sura_bl_make_array, ip, inst.a);
                    break;
                case JitOp::MAKE_DICT:
                    helper_store(&sura_bl_make_dict, ip, inst.a);
                    break;
                case JitOp::INDEX_GET: {
                    // Array + in-bounds numeric index reads inline (the same
                    // guards as the Win64 full tier); everything else, and
                    // the range error, goes through the helper.
                    if (!inline_collections) {
                        helper_store(&sura_bl_index_get, ip, inst.a);
                        after_call(inst.a);
                        break;
                    }
                    std::vector<size_t> slow;
                    sura_x64_emit_array_index_guard(em, off_r(inst.b), off_r(inst.c), slow);
                    em.mov_r_mem(XR::RAX, XR::RCX, 0);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    const size_t done = em.jmp_rel32_placeholder();
                    for (size_t j : slow) em.patch_rel32(j, em.pos());
                    helper_store(&sura_bl_index_get, ip, inst.a);
                    if (cache != nullptr && !xmm_preserved) reload_all_cached();
                    em.patch_rel32(done, em.pos());
                    break;
                }
                case JitOp::INDEX_SET: {
                    if (!inline_collections) {
                        emit_helper(&sura_bl_index_set, ip);
                        after_call(65535);
                        break;
                    }
                    std::vector<size_t> slow;
                    sura_x64_emit_array_index_guard(em, off_r(inst.a), off_r(inst.b), slow);
                    em.mov_r_mem(XR::RDX, XR::RBX, off_r(inst.c));
                    em.mov_mem_r(XR::RCX, 0, XR::RDX);
                    const size_t done = em.jmp_rel32_placeholder();
                    for (size_t j : slow) em.patch_rel32(j, em.pos());
                    emit_helper(&sura_bl_index_set, ip);
                    if (cache != nullptr && !xmm_preserved) reload_all_cached();
                    em.patch_rel32(done, em.pos());
                    break;
                }
                case JitOp::DOT_GET:
                    helper_store(&sura_bl_dot_get, ip, inst.a);
                    break;
                case JitOp::DOT_SET:
                    emit_helper(&sura_bl_dot_set, ip);
                    break;
                case JitOp::OP_IN:
                    emit_helper(&sura_bl_op_in, ip);
                    break;
                case JitOp::DICT_KEYS:
                    emit_helper(&sura_bl_dict_keys, ip);
                    break;
                case JitOp::FOREACH_NEXT:
                    // The helper writes the element and index registers and
                    // returns 0 when the collection is exhausted (or is not
                    // iterable), which is the interpreter's exit edge.
                    emit_helper(&sura_bl_foreach_next, ip);
                    em.cmp_r_imm32(XR::RAX, 0);
                    jump_fixups.push_back({em.jcc_rel32_placeholder(CC::E),
                                           static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                    emit_helper(&sura_bl_print, ip);
                    break;
                case JitOp::CALL_BUILTIN:
                    helper_store(&sura_bl_call_builtin, ip, inst.a);
                    break;
                case JitOp::METHOD_CALL: {
                    // push / len on arrays and has on dicts inline; the
                    // helper remains the slow path and every other method.
                    const int kind = inline_collections
                        ? inline_method_kind(chunk, inst) : 0;
                    if (kind == 0) {
                        helper_store(&sura_bl_method_call, ip, inst.a);
                        break;
                    }
                    std::vector<size_t> slow;
                    if (kind == kInlineDictHas) {
                        sura_x64_emit_object_receiver_guard(em, off_r(inst.b), OBJ_TYPE_DICT, slow);
                        em.mov_rr(arg_vm(), XR::RAX);
                        em.mov_r_mem(arg_regs(), XR::RBX, off_r(static_cast<uint16_t>(inst.b + 1)));
                        if (abi == X64BaselineAbi::Win64) em.sub_rsp_imm8(32);
                        em.mov_ri64(XR::RAX, static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(&sura_jit_dict_has)));
                        em.call_rax();
                        if (abi == X64BaselineAbi::Win64) em.add_rsp_imm8(32);
                        em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    } else {
                        sura_x64_emit_object_receiver_guard(em, off_r(inst.b), OBJ_TYPE_ARRAY, slow);
                        if (kind == kInlineArrayPush) {
                            sura_x64_emit_array_push_fast(
                                em, off_r(inst.b), off_r(static_cast<uint16_t>(inst.b + 1)),
                                off_r(inst.a), slow);
                        } else {
                            sura_x64_emit_array_len_fast(em, off_r(inst.a));
                        }
                    }
                    const size_t done = em.jmp_rel32_placeholder();
                    for (size_t j : slow) em.patch_rel32(j, em.pos());
                    helper_store(&sura_bl_method_call, ip, inst.a);
                    em.patch_rel32(done, em.pos());
                    break;
                }
                case JitOp::CALL_FUNC: {
                    const auto& dc = analysis.direct_call[ip - entry_ip];
                    if (!dc.valid) {
                        // Unknown closure, constructor or stdlib function:
                        // the interpreter's dispatcher runs the call.
                        helper_store(&sura_bl_call, ip, inst.a);
                        break;
                    }
                    const uint32_t argc = static_cast<uint32_t>(inst.operand);
                    const uint32_t callee_regs = dc.self ? frame_regs : dc.callee.frame_regs;
                    const bool callee_vm_frame = dc.self ? analysis.vm_frame : dc.callee.vm_frame;
                    if (!callee_vm_frame) {
                        // The callee can never reach a helper, so no
                        // collector runs while its frame is live: the frame
                        // goes on the machine stack, where a call costs
                        // only the budget bookkeeping. The 16-byte rounding
                        // keeps the ABI alignment for the call.
                        const int32_t frame_bytes =
                            static_cast<int32_t>((callee_regs * 8U + 15U) & ~15U);
                        // The interpreter refuses the call that would exceed
                        // its frame limit; the budget mirrors that limit so
                        // a deep recursion deopts before the machine stack
                        // is at risk and the interpreter raises its own
                        // [E500].
                        em.mov_r_mem(XR::RAX, XR::R13, budget_off);
                        em.sub_r_imm32(XR::RAX, 1);
                        deopt_here(CC::S, ip, false);
                        em.mov_mem_r(XR::R13, budget_off, XR::RAX);
                        // Callee frame: arguments in registers 0..argc-1,
                        // the rest zero like alloc_frame_regs' Value(0.0)
                        // fill.
                        em.sub_r_imm32(XR::RSP, frame_bytes);
                        for (uint32_t i = 0; i < argc; ++i) {
                            load_bits(XR::RAX, static_cast<uint16_t>(inst.c + i));
                            em.mov_mem_r(XR::RSP, static_cast<int32_t>(i * 8U), XR::RAX);
                        }
                        for (uint32_t r = argc; r < callee_regs; ++r) {
                            em.mov_mem_imm32(XR::RSP, static_cast<int32_t>(r * 8U), 0);
                        }
                        em.mov_rr(arg_vm(), XR::R13);
                        em.mov_rr(arg_regs(), XR::RSP);
                        em.mov_rr(arg_consts(), XR::R12);
                        if (dc.self) {
                            self_call_fixups.push_back({em.call_rel32_placeholder(), dc.args_num});
                        } else {
                            const uint8_t* target = dc.args_num ? dc.callee.unguarded_entry
                                                                : dc.callee.guarded_entry;
                            em.mov_ri64(XR::RAX, static_cast<uint64_t>(
                                reinterpret_cast<uintptr_t>(target)));
                            em.call_rax();
                        }
                        em.add_r_imm32(XR::RSP, frame_bytes);
                        em.add_mem_imm8(XR::R13, budget_off, 1);
                        // A guard that failed anywhere below hands back the
                        // sentinel; propagate it rather than storing it.
                        em.mov_ri64(XR::RCX, kDeoptSentinel);
                        em.cmp_rr(XR::RAX, XR::RCX);
                        deopt_here(CC::E, ip, true);
                        em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                        break;
                    }
                    // Callee frame on the VM's value stack, above every live
                    // frame, so the collector sees the callee's registers
                    // while its helpers allocate. A frame that would
                    // overflow the stack deopts before anything is touched
                    // and the interpreter raises its own error.
                    em.mov_r_mem(XR::RAX, XR::R13, top_off);
                    em.mov_rr(XR::RCX, XR::RAX);
                    em.add_r_imm32(XR::RCX, static_cast<int32_t>(callee_regs));
                    em.mov_ri64(XR::RDX, capacity);
                    em.cmp_rr(XR::RCX, XR::RDX);
                    deopt_here(CC::A, ip, false);
                    // The interpreter refuses the call that would exceed its
                    // frame limit; the budget mirrors that limit so a deep
                    // recursion deopts before the machine stack is at risk
                    // and the interpreter raises its own [E500]. Checked
                    // after the capacity so every deopt above leaves the
                    // budget as it found it.
                    em.mov_r_mem(XR::RDX, XR::R13, budget_off);
                    em.sub_r_imm32(XR::RDX, 1);
                    deopt_here(CC::S, ip, false);
                    em.mov_mem_r(XR::R13, budget_off, XR::RDX);
                    // Bump the top, then fill: arguments in registers
                    // 0..argc-1, the rest zero like alloc_frame_regs'
                    // Value(0.0) fill.
                    em.mov_mem_r(XR::R13, top_off, XR::RCX);
                    em.mov_r_mem(XR::R10, XR::R13, vs_off);
                    em.shl_r_imm8(XR::RAX, 3);
                    em.add_rr(XR::R10, XR::RAX);
                    for (uint32_t i = 0; i < argc; ++i) {
                        load_bits(XR::RAX, static_cast<uint16_t>(inst.c + i));
                        em.mov_mem_r(XR::R10, static_cast<int32_t>(i * 8U), XR::RAX);
                    }
                    for (uint32_t r = argc; r < callee_regs; ++r) {
                        em.mov_mem_imm32(XR::R10, static_cast<int32_t>(r * 8U), 0);
                    }
                    em.mov_rr(arg_vm(), XR::R13);
                    em.mov_rr(arg_regs(), XR::R10);
                    em.mov_rr(arg_consts(), XR::R12);
                    if (dc.self) {
                        self_call_fixups.push_back({em.call_rel32_placeholder(), dc.args_num});
                    } else {
                        const uint8_t* target = dc.args_num ? dc.callee.unguarded_entry
                                                            : dc.callee.guarded_entry;
                        em.mov_ri64(XR::RAX, static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(target)));
                        em.call_rax();
                    }
                    em.mov_r_mem(XR::RCX, XR::R13, top_off);
                    em.sub_r_imm32(XR::RCX, static_cast<int32_t>(callee_regs));
                    em.mov_mem_r(XR::R13, top_off, XR::RCX);
                    em.add_mem_imm8(XR::R13, budget_off, 1);
                    // A guard that failed anywhere below hands back the
                    // sentinel; propagate it rather than storing it. The
                    // callee is pure, so the interpreter re-runs the call
                    // from this instruction; an exception it parked belongs
                    // to that re-run, not to this frame.
                    em.mov_ri64(XR::RCX, kDeoptSentinel);
                    em.cmp_rr(XR::RAX, XR::RCX);
                    deopt_here(CC::E, ip, true);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    break;
                }
                default:
                    return {};
            }
            if (cache != nullptr) {
                reload_after(inst, dyn);
                if (ip == cache->backedge_ip) cache = nullptr;
            }
        }
        cache = nullptr;

        // Fall-through end of body: only needed when the last reachable
        // instruction does not unconditionally leave the function.
        if (analysis.falls_off_end(chunk, entry_ip, end_ip)) {
            em.mov_ri64(XR::RAX, NBNIL);
            emit_return();
        }

        // Deopt tail shared by a leaf body's zero-divisor checks: restore
        // the callee-saved registers and hand back the sentinel.
        if (!deopt_fixups.empty()) {
            const size_t deopt_off = em.pos();
            em.mov_ri64(XR::RAX, kDeoptSentinel);
            emit_return();
            for (size_t disp : deopt_fixups) em.patch_rel32(disp, deopt_off);
        }
        // Per-site deopt stubs of a vm body: record the resume point, drop
        // a parked exception that belongs to a re-run call, then return
        // the sentinel through the same epilogue.
        for (const DeoptSite& site : deopt_sites) {
            em.patch_rel32(site.disp_pos, em.pos());
            em.mov_mem_imm32(XR::R13, resume_off, static_cast<int32_t>(site.ip));
            if (site.discard_exc) em.mov_mem_imm32(XR::R13, exc_off, 0);
            em.mov_ri64(XR::RAX, kDeoptSentinel);
            emit_return();
        }
        // Entry-guard tail: nothing has been pushed yet.
        if (!entry_deopt_fixups.empty()) {
            const size_t deopt_off = em.pos();
            em.mov_ri64(XR::RAX, kDeoptSentinel);
            em.ret();
            for (size_t disp : entry_deopt_fixups) em.patch_rel32(disp, deopt_off);
        }
        for (const JumpFixup& fx : jump_fixups) {
            em.patch_rel32(fx.disp_pos, ip_off[fx.target_ip - entry_ip]);
        }
        for (const SelfCallFixup& fx : self_call_fixups) {
            em.patch_rel32(fx.disp_pos, fx.unguarded ? unguarded_entry_offset : 0);
        }
        if (const char* dump_dir = std::getenv("SURA_JIT_DUMP_DIR")) {
            // Raw machine code for `objdump -D -b binary -m i386:x86-64`.
            const std::string path = std::string(dump_dir) + "/baseline_" +
                                     std::to_string(entry_ip) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(code.data(), 1, code.size(), f);
                std::fclose(f);
            }
        }
        return code;
    }
};

// ARM64 uses the common three-register entry shape on Windows, Linux, and
// macOS: x0=vm, x1=R, x2=consts, x0=result. A leaf body has no stack frame
// and makes no calls; a body that needs the vm pointer keeps a small frame
// and calls the same helpers as the x86-64 baseline. The second revision
// mirrors the System V baseline: whole-body branches (loops) are encoded
// with patched B/B.cond displacements, and guarded numeric parameters
// return the shared deopt sentinel when a caller passes a non-number.
class Arm64BaselineCompiler {
    const JitChunk& chunk;
    size_t entry_ip;
    size_t end_ip;
    uint32_t frame_regs;
    uint32_t guarded_params;
    bool deopt_allowed;
    BaselineLinkContext* link = nullptr;
    int self_fidx = -1;
    bool self_callable = false;
    uint64_t guard_mask = ~0ULL;
    bool allow_helpers = false;

    class Emitter {
        std::vector<uint8_t>& code;

    public:
        explicit Emitter(std::vector<uint8_t>& output) : code(output) {}

        size_t pos() const { return code.size(); }

        void word(uint32_t instruction) {
            code.push_back(static_cast<uint8_t>(instruction & 0xffU));
            code.push_back(static_cast<uint8_t>((instruction >> 8) & 0xffU));
            code.push_back(static_cast<uint8_t>((instruction >> 16) & 0xffU));
            code.push_back(static_cast<uint8_t>((instruction >> 24) & 0xffU));
        }

        void ldr_x(unsigned target, unsigned base, uint16_t scaled_offset) {
            word(0xF9400000U | (static_cast<uint32_t>(scaled_offset) << 10) |
                 (base << 5) | target);
        }
        void str_x(unsigned source, unsigned base, uint16_t scaled_offset) {
            word(0xF9000000U | (static_cast<uint32_t>(scaled_offset) << 10) |
                 (base << 5) | source);
        }
        void ldr_d(unsigned target, unsigned base, uint16_t scaled_offset) {
            word(0xFD400000U | (static_cast<uint32_t>(scaled_offset) << 10) |
                 (base << 5) | target);
        }
        void str_d(unsigned source, unsigned base, uint16_t scaled_offset) {
            word(0xFD000000U | (static_cast<uint32_t>(scaled_offset) << 10) |
                 (base << 5) | source);
        }
        void fadd_d(unsigned target, unsigned left, unsigned right) {
            word(0x1E602800U | (right << 16) | (left << 5) | target);
        }
        void fsub_d(unsigned target, unsigned left, unsigned right) {
            word(0x1E603800U | (right << 16) | (left << 5) | target);
        }
        void fmul_d(unsigned target, unsigned left, unsigned right) {
            word(0x1E600800U | (right << 16) | (left << 5) | target);
        }
        void fdiv_d(unsigned target, unsigned left, unsigned right) {
            word(0x1E601800U | (right << 16) | (left << 5) | target);
        }
        void fneg_d(unsigned target, unsigned source) {
            word(0x1E614000U | (source << 5) | target);
        }
        void fcmp_d(unsigned left, unsigned right) {
            word(0x1E602000U | (right << 16) | (left << 5));
        }
        void csel_x(unsigned target, unsigned when_true, unsigned when_false,
                    unsigned condition) {
            word(0x9A800000U | (when_false << 16) | (condition << 12) |
                 (when_true << 5) | target);
        }
        void and_xx(unsigned target, unsigned left, unsigned right) {
            word(0x8A000000U | (right << 16) | (left << 5) | target);
        }
        void cmp_xx(unsigned left, unsigned right) {
            word(0xEB00001FU | (right << 16) | (left << 5));
        }
        void mov_imm64(unsigned target, uint64_t value) {
            word(0xD2800000U |
                 (static_cast<uint32_t>(value & 0xffffU) << 5) | target);
            for (unsigned half = 1; half < 4; ++half) {
                const uint32_t part = static_cast<uint32_t>(
                    (value >> (half * 16U)) & 0xffffU);
                word(0xF2800000U | (half << 21) | (part << 5) | target);
            }
        }
        // Branches are emitted with a zero displacement and patched once
        // every instruction offset is known. A64 displacements are relative
        // to the branch instruction itself, in words.
        size_t b_placeholder() {
            const size_t at = code.size();
            word(0x14000000U);
            return at;
        }
        size_t b_cond_placeholder(unsigned condition) {
            const size_t at = code.size();
            word(0x54000000U | condition);
            return at;
        }
        bool patch_branch(size_t at, size_t target) {
            const int64_t delta = (static_cast<int64_t>(target) -
                                   static_cast<int64_t>(at)) / 4;
            uint32_t inst = static_cast<uint32_t>(code[at]) |
                            (static_cast<uint32_t>(code[at + 1]) << 8) |
                            (static_cast<uint32_t>(code[at + 2]) << 16) |
                            (static_cast<uint32_t>(code[at + 3]) << 24);
            if ((inst & 0xFF000000U) == 0x54000000U) {
                if (delta < -(1 << 18) || delta >= (1 << 18)) return false;
                inst |= (static_cast<uint32_t>(delta) & 0x7FFFFU) << 5;
            } else {
                if (delta < -(1 << 25) || delta >= (1 << 25)) return false;
                inst |= static_cast<uint32_t>(delta) & 0x03FFFFFFU;
            }
            code[at]     = static_cast<uint8_t>(inst & 0xffU);
            code[at + 1] = static_cast<uint8_t>((inst >> 8) & 0xffU);
            code[at + 2] = static_cast<uint8_t>((inst >> 16) & 0xffU);
            code[at + 3] = static_cast<uint8_t>((inst >> 24) & 0xffU);
            return true;
        }
        // Register-offset forms for vm-relative slots whose displacement
        // may exceed the 12-bit scaled immediate: [base + index].
        void ldr_x_reg(unsigned target, unsigned base, unsigned index) {
            word(0xF8606800U | (index << 16) | (base << 5) | target);
        }
        void str_x_reg(unsigned source, unsigned base, unsigned index) {
            word(0xF8206800U | (index << 16) | (base << 5) | source);
        }
        // ldr w, [base, #imm] (scaled by 4): zero-extends into the X register.
        void ldr_w(unsigned target, unsigned base, uint16_t scaled_offset) {
            word(0xB9400000U | (static_cast<uint32_t>(scaled_offset) << 10) |
                 (base << 5) | target);
        }
        // ldr x, [base] with no offset (the address was formed in `base`).
        void ldr_x0(unsigned target, unsigned base) { ldr_x(target, base, 0); }
        void str_x0(unsigned source, unsigned base) { str_x(source, base, 0); }
        // fcvtzs x, d: truncate toward zero (matches cvttsd2si / C `(int)`).
        void fcvtzs_x_d(unsigned target, unsigned source) {
            word(0x9E780000U | (source << 5) | target);
        }
        // scvtf d, x: signed 64-bit integer to double.
        void scvtf_d_x(unsigned target, unsigned source) {
            word(0x9E620000U | (source << 5) | target);
        }
        void sub_xx(unsigned target, unsigned left, unsigned right) {
            word(0xCB000000U | (right << 16) | (left << 5) | target);
        }
        // lsr x, x, #shift (UBFM with imms = 63).
        void lsr_imm(unsigned target, unsigned source, unsigned shift) {
            word(0xD340FC00U | (shift << 16) | (source << 5) | target);
        }
        void add_imm(unsigned target, unsigned source, uint16_t imm12) {
            word(0x91000000U | (static_cast<uint32_t>(imm12) << 10) |
                 (source << 5) | target);
        }
        // ADD Xd, Xn, Xm, LSL #shift (shifted-register form).
        void add_lsl(unsigned target, unsigned source, unsigned index, unsigned shift) {
            word(0x8B000000U | (index << 16) | (shift << 10) | (source << 5) | target);
        }
        void eor_xx(unsigned target, unsigned a, unsigned b) {
            word(0xCA000000U | (b << 16) | (a << 5) | target);
        }
        void sub_imm(unsigned target, unsigned source, uint16_t imm12) {
            word(0xD1000000U | (static_cast<uint32_t>(imm12) << 10) |
                 (source << 5) | target);
        }
        void subs_imm(unsigned target, unsigned source, uint16_t imm12) {
            word(0xF1000000U | (static_cast<uint32_t>(imm12) << 10) |
                 (source << 5) | target);
        }
        void mov_xx(unsigned target, unsigned source) {  // orr xd, xzr, xs
            word(0xAA0003E0U | (source << 16) | target);
        }
        // STP/LDP with a signed 7-bit offset in eight-byte units.
        void stp_pre(unsigned t1, unsigned t2, unsigned base, int imm7) {
            word(0xA9800000U | ((static_cast<uint32_t>(imm7) & 0x7FU) << 15) |
                 (t2 << 10) | (base << 5) | t1);
        }
        void stp_off(unsigned t1, unsigned t2, unsigned base, int imm7) {
            word(0xA9000000U | ((static_cast<uint32_t>(imm7) & 0x7FU) << 15) |
                 (t2 << 10) | (base << 5) | t1);
        }
        void ldp_off(unsigned t1, unsigned t2, unsigned base, int imm7) {
            word(0xA9400000U | ((static_cast<uint32_t>(imm7) & 0x7FU) << 15) |
                 (t2 << 10) | (base << 5) | t1);
        }
        void ldp_post(unsigned t1, unsigned t2, unsigned base, int imm7) {
            word(0xA8C00000U | ((static_cast<uint32_t>(imm7) & 0x7FU) << 15) |
                 (t2 << 10) | (base << 5) | t1);
        }
        size_t bl_placeholder() {
            const size_t at = code.size();
            word(0x94000000U);
            return at;
        }
        void blr(unsigned target) { word(0xD63F0000U | (target << 5)); }
        void bti_c() { word(0xD503245FU); }
        void ret() { word(0xD65F03C0U); }
    };

public:
    // guarded_param_regs / allow_deopt carry the same contract as the System V
    // tier: guards and zero-divisor checks escape through the deopt sentinel,
    // which only the closure call sites know how to re-run.
    Arm64BaselineCompiler(const JitChunk& source, size_t begin, size_t end,
                          uint32_t registers, uint32_t guarded_param_regs = 0,
                          bool allow_deopt = true)
        : chunk(source), entry_ip(begin), end_ip(end), frame_regs(registers),
          guarded_params(guarded_param_regs), deopt_allowed(allow_deopt) {}

    // Offset of the entry past the parameter guards, for direct callers
    // that have proven their arguments numeric. Valid after compile_bytes.
    size_t unguarded_entry_offset = 0;
    // Static kind of the value this body returns (see BaselineBodyAnalysis).
    BaselineBodyAnalysis::RegKind return_kind = BaselineBodyAnalysis::kOther;
    bool pure = true;  // BaselineBodyAnalysis::pure
    bool vm_frame = false;  // BaselineBodyAnalysis::vm_frame

    // Same contract as SysVBaselineCompiler::set_link: the context resolves
    // globals and callees; `callable` says whether this body itself may be
    // entered by a direct call (needed for self recursion).
    void set_link(BaselineLinkContext* ctx, int fidx, bool callable) {
        link = ctx;
        self_fidx = fidx;
        self_callable = callable;
    }
    void set_guard_mask(uint64_t mask) { guard_mask = mask; }
    void set_allow_helpers(bool allow) { allow_helpers = allow; }

    std::vector<uint8_t> compile_bytes() {
        // AArch64 unsigned load/store immediates are 12-bit values scaled by
        // eight for X/D registers, so register and constant indices must fit.
        if (entry_ip >= end_ip || end_ip > chunk.code.size() || frame_regs == 0 ||
            frame_regs > 4096U) {
            return {};
        }
        const size_t body_len = end_ip - entry_ip;
        BaselineBodyAnalysis analysis(chunk, entry_ip, end_ip, frame_regs,
                                      guarded_params, 4095U, 4095,
                                      deopt_allowed,
                                      /*require_provable_div=*/true,
                                      link, self_fidx, self_callable,
                                      guard_mask, allow_helpers);
        if (!analysis.ok) return {};
        return_kind = analysis.return_kind;
        pure = analysis.pure;
        vm_frame = analysis.vm_frame;

        // A body that reads globals or calls needs the VM pointer and must
        // survive calls, so it keeps R, the constants and the VM in the
        // callee-saved x19/x20/x21 behind a frame. Leaf bodies keep working
        // straight out of the argument registers with no frame at all, so
        // their encoding is unchanged.
        const bool needs_vm = analysis.uses_vm;
        const unsigned R = needs_vm ? 19U : 1U;
        const unsigned K = needs_vm ? 20U : 2U;
        const unsigned VM = 21U;
        const int32_t globals_off = needs_vm ? link->globals_vector_offset() : 0;
        const int32_t budget_off = needs_vm ? link->depth_budget_offset() : 0;
        const int32_t resume_off = needs_vm ? link->resume_ip_offset() : 0;
        const int32_t exc_off = needs_vm ? link->exc_valid_offset() : 0;
        const int32_t vs_off = needs_vm ? link->value_stack_data_offset() : 0;
        const int32_t top_off = needs_vm ? link->stack_top_offset() : 0;
        const uint64_t capacity = needs_vm ? static_cast<uint64_t>(link->stack_capacity()) : 0;

        std::vector<uint8_t> code;
        Emitter em(code);
        // BTI is a HINT-space no-op on implementations without FEAT_BTI and
        // a valid indirect-call landing pad when branch protection is active.
        em.bti_c();

        // Entry guards: (bits & NBQNAN) == NBQNAN means "not a number";
        // branch to a tail that returns the sentinel. They run before the
        // prologue, so their tail has nothing to unwind.
        std::vector<size_t> entry_deopt_branches;
        for (uint32_t p = 0; p < guarded_params; ++p) {
            if (!BaselineBodyAnalysis::guarded_bit(guard_mask, p)) continue;
            em.ldr_x(9, 1, static_cast<uint16_t>(p));
            em.mov_imm64(10, NBQNAN);
            em.and_xx(9, 9, 10);
            em.cmp_xx(9, 10);
            entry_deopt_branches.push_back(em.b_cond_placeholder(0));  // EQ
        }

        unguarded_entry_offset = em.pos();
        if (needs_vm) {
            em.stp_pre(29, 30, 31, -6);    // stp x29, x30, [sp, #-48]!
            em.stp_off(19, 20, 31, 2);     // stp x19, x20, [sp, #16]
            em.str_x(VM, 31, 4);           // str x21, [sp, #32]
            em.add_imm(29, 31, 0);         // mov x29, sp
            em.mov_xx(R, 1);
            em.mov_xx(K, 2);
            em.mov_xx(VM, 0);
        }
        auto emit_return = [&]() {
            if (needs_vm) {
                em.ldr_x(VM, 31, 4);
                em.ldp_off(19, 20, 31, 2);
                em.ldp_post(29, 30, 31, 6);
            }
            em.ret();
        };
        // Access a vm-relative slot: the scaled immediate form reaches the
        // first 32 KiB of JitVM; deeper displacements go through x10.
        auto slot_fits_immediate = [](int32_t disp) {
            return disp >= 0 && disp % 8 == 0 && disp / 8 < 4096;
        };
        auto load_vm_slot = [&](unsigned target, int32_t disp) {
            if (slot_fits_immediate(disp)) {
                em.ldr_x(target, VM, static_cast<uint16_t>(disp / 8));
                return;
            }
            em.mov_imm64(10, static_cast<uint64_t>(static_cast<int64_t>(disp)));
            em.ldr_x_reg(target, VM, 10);
        };
        auto store_vm_slot = [&](unsigned source, int32_t disp) {
            if (slot_fits_immediate(disp)) {
                em.str_x(source, VM, static_cast<uint16_t>(disp / 8));
                return;
            }
            em.mov_imm64(10, static_cast<uint64_t>(static_cast<int64_t>(disp)));
            em.str_x_reg(source, VM, 10);
        };

        // Deopt sites: a leaf body shares one tail; a framed body records
        // the resume point per site (see SysVBaselineCompiler).
        std::vector<size_t> deopt_branches;
        struct DeoptSite { size_t at; size_t ip; bool discard_exc; };
        std::vector<DeoptSite> deopt_sites;
        auto deopt_here = [&](unsigned condition, size_t ip, bool discard_exc) {
            if (needs_vm) {
                deopt_sites.push_back({em.b_cond_placeholder(condition), ip, discard_exc});
            } else {
                deopt_branches.push_back(em.b_cond_placeholder(condition));
            }
        };
        // Call an interpreter helper for the instruction at `ip`: x0=vm,
        // x1=R, x2=&instruction. The sentinel in x0 means it parked an
        // exception; resume at `ip` so the interpreter rethrows it.
        auto emit_helper = [&](uint64_t (*fn)(JitVM*, Value*, const JitInst*), size_t ip) {
            em.mov_xx(0, VM);
            em.mov_xx(1, R);
            em.mov_imm64(2, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&chunk.code[ip])));
            em.mov_imm64(9, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn)));
            em.blr(9);
            em.mov_imm64(10, SURA_JIT_DEOPT_SENTINEL);
            em.cmp_xx(0, 10);
            deopt_here(0, ip, false);  // EQ
        };
        auto helper_store = [&](uint64_t (*fn)(JitVM*, Value*, const JitInst*), size_t ip,
                                uint16_t dst) {
            emit_helper(fn, ip);
            em.str_x(0, R, dst);
        };
        std::vector<size_t> ip_off(body_len, 0);
        struct BranchFixup { size_t at; size_t target_ip; };
        std::vector<BranchFixup> branch_fixups;
        struct SelfCallFixup { size_t at; bool unguarded; };
        std::vector<SelfCallFixup> self_call_fixups;
        const bool inline_collections = allow_helpers && jit_array_layout_verified();
        // Branch to `slow` unless R[reg] is a NaN-boxed number. Uses x9-x11.
        auto emit_non_number_to = [&](uint16_t reg, std::vector<size_t>& slow) {
            em.ldr_x(9, R, reg);
            em.mov_imm64(10, NBQNAN);
            em.and_xx(11, 9, 10);
            em.cmp_xx(11, 10);
            slow.push_back(em.b_cond_placeholder(0));  // EQ: not a number
        };
        // Guards R[reg] is an object with the given tag; leaves x9 = GCObject*.
        // Uses x9, x10, x11. Failing guards are queued as branch offsets.
        auto emit_object_receiver_guard = [&](uint16_t reg, int8_t obj_tag,
                                              std::vector<size_t>& slow) {
            em.ldr_x(9, R, reg);
            em.mov_imm64(10, JIT_NBOBJ);
            em.and_xx(11, 9, 10);
            em.cmp_xx(11, 10);
            slow.push_back(em.b_cond_placeholder(1));  // NE
            em.mov_imm64(10, JIT_NBPMASK);
            em.and_xx(9, 9, 10);
            em.ldr_w(10, 9, static_cast<uint16_t>(ARRAY_OBJTYPE_OFFSET / 4));
            em.subs_imm(31, 10, static_cast<uint16_t>(obj_tag));
            slow.push_back(em.b_cond_placeholder(1));  // NE
        };
        // Array receiver + in-bounds non-negative numeric key; leaves
        // x9 = GCArray*, x11 = &elements[idx]. Uses x9-x13, d0.
        auto emit_array_index_guard = [&](uint16_t container, uint16_t key,
                                          std::vector<size_t>& slow) {
            emit_object_receiver_guard(container, OBJ_TYPE_ARRAY, slow);
            em.ldr_x(11, R, key);
            em.mov_imm64(10, NBQNAN);
            em.and_xx(12, 11, 10);
            em.cmp_xx(12, 10);
            slow.push_back(em.b_cond_placeholder(0));  // EQ: not a number
            em.ldr_d(0, R, key);
            em.fcvtzs_x_d(11, 0);
            em.mov_imm64(10, 0x7fffffffULL);
            em.cmp_xx(11, 10);
            slow.push_back(em.b_cond_placeholder(8));  // HI: negative or huge
            em.ldr_x(12, 9, static_cast<uint16_t>(
                (ARRAY_ELEMENTS_OFFSET + VECTOR_DATA_OFFSET) / 8));
            em.ldr_x(13, 9, static_cast<uint16_t>(
                (ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET) / 8));
            em.add_lsl(11, 12, 11, 3);
            em.cmp_xx(11, 13);
            slow.push_back(em.b_cond_placeholder(2));  // HS: out of bounds
        };

        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            if (!analysis.reached[ip - entry_ip]) continue;
            ip_off[ip - entry_ip] = em.pos();
            const JitInst& inst = chunk.code[ip];
            const bool dyn = analysis.dynamic[ip - entry_ip] != 0;
            switch (inst.op) {
                case JitOp::NOP:
                    break;
                case JitOp::LOAD_CONST:
                    em.ldr_x(9, K, static_cast<uint16_t>(inst.operand));
                    em.str_x(9, R, inst.a);
                    break;
                case JitOp::LOAD_NIL:
                    em.mov_imm64(9, NBNIL);
                    em.str_x(9, R, inst.a);
                    break;
                case JitOp::LOAD_BOOL:
                    em.mov_imm64(9, inst.operand ? NBTRUE : NBFALSE);
                    em.str_x(9, R, inst.a);
                    break;
                case JitOp::MOVE:
                    em.ldr_x(9, R, inst.b);
                    em.str_x(9, R, inst.a);
                    break;
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV: {
                    // Same contract as the x64 tier: dynamic operands are
                    // tag-checked at run time and computed inline when both
                    // are numbers (and a dynamic divisor is nonzero); the
                    // helper handles the rest.
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(inst.b, slow);
                        emit_non_number_to(inst.c, slow);
                        if (inst.op == JitOp::DIV) {
                            em.ldr_x(9, R, inst.c);
                            em.mov_imm64(10, 0x7FFFFFFFFFFFFFFFULL);
                            em.and_xx(9, 9, 10);
                            em.cmp_xx(9, 31);  // xzr
                            slow.push_back(em.b_cond_placeholder(0));  // EQ
                        }
                    } else if (analysis.div_zero_guard[ip - entry_ip]) {
                        // Same contract as the System V tier: clear the sign
                        // bit so -0.0 counts as zero, then deopt so the
                        // interpreter raises [E202].
                        em.ldr_x(9, R, inst.c);
                        em.mov_imm64(10, 0x7FFFFFFFFFFFFFFFULL);
                        em.and_xx(9, 9, 10);
                        em.cmp_xx(9, 31);  // xzr
                        deopt_here(0, ip, false);  // EQ
                    }
                    em.ldr_d(0, R, inst.b);
                    em.ldr_d(1, R, inst.c);
                    if (inst.op == JitOp::ADD) {
                        em.fadd_d(0, 0, 1);
                    } else if (inst.op == JitOp::SUB) {
                        em.fsub_d(0, 0, 1);
                    } else if (inst.op == JitOp::MUL) {
                        em.fmul_d(0, 0, 1);
                    } else {
                        em.fdiv_d(0, 0, 1);
                    }
                    em.str_d(0, R, inst.a);
                    if (dyn) {
                        const size_t done = em.b_placeholder();
                        for (size_t at : slow) if (!em.patch_branch(at, em.pos())) return {};
                        helper_store(&sura_bl_arith, ip, inst.a);
                        if (!em.patch_branch(done, em.pos())) return {};
                    }
                    break;
                }
                case JitOp::MOD:
                    helper_store(&sura_bl_arith, ip, inst.a);
                    break;
                case JitOp::NEG:
                    if (dyn) {
                        helper_store(&sura_bl_arith, ip, inst.a);
                        break;
                    }
                    em.ldr_d(0, R, inst.b);
                    em.fneg_d(0, 0);
                    em.str_d(0, R, inst.a);
                    break;
                case JitOp::LOGICAL_NOT:
                    if (dyn) {
                        helper_store(&sura_bl_arith, ip, inst.a);
                        break;
                    }
                    // A proven Bool toggles between NBFALSE and NBTRUE.
                    em.ldr_x(9, R, inst.b);
                    em.mov_imm64(10, NBFALSE ^ NBTRUE);
                    em.eor_xx(9, 9, 10);
                    em.str_x(9, R, inst.a);
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE: {
                    std::vector<size_t> slow;
                    if (dyn) {
                        emit_non_number_to(inst.b, slow);
                        emit_non_number_to(inst.c, slow);
                    }
                    em.ldr_d(0, R, inst.b);
                    em.ldr_d(1, R, inst.c);
                    em.fcmp_d(0, 1);
                    em.mov_imm64(10, NBTRUE);
                    em.mov_imm64(11, NBFALSE);
                    // Conditions are chosen so an unordered comparison (any
                    // NaN operand) selects false for the ordering operators
                    // and true only for CMP_NEQ, matching the interpreter.
                    unsigned condition = 0;
                    switch (inst.op) {
                        case JitOp::CMP_EQ:  condition = 0; break;  // EQ
                        case JitOp::CMP_NEQ: condition = 1; break;  // NE
                        case JitOp::CMP_LT:  condition = 4; break;  // MI
                        case JitOp::CMP_LTE: condition = 9; break;  // LS
                        case JitOp::CMP_GT:  condition = 12; break; // GT
                        case JitOp::CMP_GTE: condition = 10; break; // GE
                        default: return {};
                    }
                    em.csel_x(9, 10, 11, condition);
                    em.str_x(9, R, inst.a);
                    if (dyn) {
                        const size_t done = em.b_placeholder();
                        for (size_t at : slow) if (!em.patch_branch(at, em.pos())) return {};
                        helper_store(&sura_bl_arith, ip, inst.a);
                        if (!em.patch_branch(done, em.pos())) return {};
                    }
                    break;
                }
                case JitOp::JUMP:
                    branch_fixups.push_back({em.b_placeholder(),
                                             static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::JUMP_IF_FALSE:
                case JitOp::JUMP_IF_TRUE:
                    if (dyn) {
                        // A value that already is a Bool is used directly;
                        // the helper applies truthiness to anything else.
                        std::vector<size_t> is_bool;
                        em.ldr_x(9, R, inst.a);
                        em.mov_imm64(10, NBFALSE);
                        em.cmp_xx(9, 10);
                        is_bool.push_back(em.b_cond_placeholder(0));  // EQ
                        em.mov_imm64(10, NBTRUE);
                        em.cmp_xx(9, 10);
                        is_bool.push_back(em.b_cond_placeholder(0));  // EQ
                        emit_helper(&sura_bl_truthy, ip);
                        em.mov_xx(9, 0);
                        for (size_t at : is_bool) if (!em.patch_branch(at, em.pos())) return {};
                    } else {
                        em.ldr_x(9, R, inst.a);
                    }
                    em.mov_imm64(10, inst.op == JitOp::JUMP_IF_FALSE ? NBFALSE : NBTRUE);
                    em.cmp_xx(9, 10);
                    branch_fixups.push_back({em.b_cond_placeholder(0),  // EQ
                                             static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::RETURN_VAL:
                    em.ldr_x(0, R, inst.a);
                    emit_return();
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    em.mov_imm64(0, NBNIL);
                    emit_return();
                    break;
                case JitOp::LOAD_GLOBAL: {
                    // Read the slot through the vector's begin pointer on
                    // every execution: the vector reallocates as globals are
                    // added, so only the vm-relative location is stable.
                    const auto& g = analysis.global_guard[ip - entry_ip];
                    if (g.mode == BaselineBodyAnalysis::GlobalGuard::Helper) {
                        helper_store(&sura_bl_load_global, ip, inst.a);
                        break;
                    }
                    load_vm_slot(9, globals_off);
                    if (g.index < 4096) {
                        em.ldr_x(9, 9, static_cast<uint16_t>(g.index));
                    } else {
                        em.mov_imm64(10, static_cast<uint64_t>(g.index) * 8U);
                        em.ldr_x_reg(9, 9, 10);
                    }
                    if (g.mode == BaselineBodyAnalysis::GlobalGuard::Identity) {
                        em.mov_imm64(10, g.bits);
                        em.cmp_xx(9, 10);
                        deopt_here(1, ip, false);  // NE
                        link->pin_value(g.bits);
                    } else if (g.mode == BaselineBodyAnalysis::GlobalGuard::NumTag) {
                        em.mov_imm64(10, NBQNAN);
                        em.and_xx(11, 9, 10);
                        em.cmp_xx(11, 10);
                        deopt_here(0, ip, false);  // EQ
                    }
                    em.str_x(9, R, inst.a);
                    break;
                }
                case JitOp::STORE_GLOBAL:
                    emit_helper(&sura_bl_store_global, ip);
                    break;
                case JitOp::MAKE_ARRAY:
                    helper_store(&sura_bl_make_array, ip, inst.a);
                    break;
                case JitOp::MAKE_DICT:
                    helper_store(&sura_bl_make_dict, ip, inst.a);
                    break;
                case JitOp::INDEX_GET: {
                    // Same shape as the x64 tiers: array + in-bounds numeric
                    // index inline, helper otherwise. x9 = GCArray*, x11 =
                    // &elements[idx] on the fast path.
                    if (!inline_collections) {
                        helper_store(&sura_bl_index_get, ip, inst.a);
                        break;
                    }
                    std::vector<size_t> slow;
                    emit_array_index_guard(inst.b, inst.c, slow);
                    em.ldr_x0(9, 11);
                    em.str_x(9, R, inst.a);
                    const size_t done = em.b_placeholder();
                    for (size_t at : slow) if (!em.patch_branch(at, em.pos())) return {};
                    helper_store(&sura_bl_index_get, ip, inst.a);
                    if (!em.patch_branch(done, em.pos())) return {};
                    break;
                }
                case JitOp::INDEX_SET: {
                    if (!inline_collections) {
                        emit_helper(&sura_bl_index_set, ip);
                        break;
                    }
                    std::vector<size_t> slow;
                    emit_array_index_guard(inst.a, inst.b, slow);
                    em.ldr_x(10, R, inst.c);
                    em.str_x0(10, 11);
                    const size_t done = em.b_placeholder();
                    for (size_t at : slow) if (!em.patch_branch(at, em.pos())) return {};
                    emit_helper(&sura_bl_index_set, ip);
                    if (!em.patch_branch(done, em.pos())) return {};
                    break;
                }
                case JitOp::DOT_GET:
                    helper_store(&sura_bl_dot_get, ip, inst.a);
                    break;
                case JitOp::DOT_SET:
                    emit_helper(&sura_bl_dot_set, ip);
                    break;
                case JitOp::OP_IN:
                    emit_helper(&sura_bl_op_in, ip);
                    break;
                case JitOp::DICT_KEYS:
                    emit_helper(&sura_bl_dict_keys, ip);
                    break;
                case JitOp::FOREACH_NEXT:
                    // 0 from the helper is the interpreter's exit edge.
                    emit_helper(&sura_bl_foreach_next, ip);
                    em.cmp_xx(0, 31);  // xzr
                    branch_fixups.push_back({em.b_cond_placeholder(0),  // EQ
                                             static_cast<size_t>(inst.operand)});
                    break;
                case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                    emit_helper(&sura_bl_print, ip);
                    break;
                case JitOp::CALL_BUILTIN:
                    helper_store(&sura_bl_call_builtin, ip, inst.a);
                    break;
                case JitOp::METHOD_CALL: {
                    const int kind = inline_collections
                        ? inline_method_kind(chunk, inst) : 0;
                    if (kind == 0) {
                        helper_store(&sura_bl_method_call, ip, inst.a);
                        break;
                    }
                    std::vector<size_t> slow;
                    if (kind == kInlineDictHas) {
                        emit_object_receiver_guard(inst.b, OBJ_TYPE_DICT, slow);
                        em.mov_xx(0, 9);
                        em.ldr_x(1, R, static_cast<uint16_t>(inst.b + 1));
                        em.mov_imm64(9, static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(&sura_jit_dict_has)));
                        em.blr(9);
                        em.str_x(0, R, inst.a);
                    } else {
                        emit_object_receiver_guard(inst.b, OBJ_TYPE_ARRAY, slow);
                        if (kind == kInlineArrayPush) {
                            // x12 = finish, x13 = capacity end
                            em.ldr_x(12, 9, static_cast<uint16_t>(
                                (ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET) / 8));
                            em.ldr_x(13, 9, static_cast<uint16_t>(
                                (ARRAY_ELEMENTS_OFFSET + VECTOR_CAP_OFFSET) / 8));
                            em.cmp_xx(12, 13);
                            slow.push_back(em.b_cond_placeholder(2));  // HS: full
                            em.ldr_x(10, R, static_cast<uint16_t>(inst.b + 1));
                            em.str_x0(10, 12);
                            em.add_imm(12, 12, 8);
                            em.str_x(12, 9, static_cast<uint16_t>(
                                (ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET) / 8));
                            em.ldr_x(9, R, inst.b);
                            em.str_x(9, R, inst.a);
                        } else {
                            em.ldr_x(12, 9, static_cast<uint16_t>(
                                (ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET) / 8));
                            em.ldr_x(13, 9, static_cast<uint16_t>(
                                (ARRAY_ELEMENTS_OFFSET + VECTOR_DATA_OFFSET) / 8));
                            em.sub_xx(12, 12, 13);
                            em.lsr_imm(12, 12, 3);
                            em.scvtf_d_x(0, 12);
                            em.str_d(0, R, inst.a);
                        }
                    }
                    const size_t done = em.b_placeholder();
                    for (size_t at : slow) if (!em.patch_branch(at, em.pos())) return {};
                    helper_store(&sura_bl_method_call, ip, inst.a);
                    if (!em.patch_branch(done, em.pos())) return {};
                    break;
                }
                case JitOp::CALL_FUNC: {
                    const auto& dc = analysis.direct_call[ip - entry_ip];
                    if (!dc.valid) {
                        helper_store(&sura_bl_call, ip, inst.a);
                        break;
                    }
                    const uint32_t argc = static_cast<uint32_t>(inst.operand);
                    const uint32_t callee_regs = dc.self ? frame_regs : dc.callee.frame_regs;
                    const bool callee_vm_frame = dc.self ? analysis.vm_frame : dc.callee.vm_frame;
                    if (!callee_vm_frame) {
                        // Helper-free callee (see the x86-64 tier): its
                        // frame lives on the machine stack, 16-byte rounded
                        // for the AAPCS64 sp alignment rule.
                        const uint16_t frame_bytes =
                            static_cast<uint16_t>((callee_regs * 8U + 15U) & ~15U);
                        load_vm_slot(9, budget_off);
                        em.subs_imm(9, 9, 1);
                        deopt_here(4, ip, false);  // MI
                        store_vm_slot(9, budget_off);
                        em.sub_imm(31, 31, frame_bytes);
                        for (uint32_t i = 0; i < argc; ++i) {
                            em.ldr_x(9, R, static_cast<uint16_t>(inst.c + i));
                            em.str_x(9, 31, static_cast<uint16_t>(i));
                        }
                        for (uint32_t r = argc; r < callee_regs; ++r) {
                            em.str_x(31, 31, static_cast<uint16_t>(r));  // xzr
                        }
                        em.mov_xx(0, VM);
                        em.add_imm(1, 31, 0);  // mov x1, sp
                        em.mov_xx(2, K);
                        if (dc.self) {
                            self_call_fixups.push_back({em.bl_placeholder(), dc.args_num});
                        } else {
                            em.mov_imm64(9, static_cast<uint64_t>(
                                reinterpret_cast<uintptr_t>(dc.callee.guarded_entry)));
                            em.blr(9);
                        }
                        em.add_imm(31, 31, frame_bytes);
                        load_vm_slot(9, budget_off);
                        em.add_imm(9, 9, 1);
                        store_vm_slot(9, budget_off);
                        em.mov_imm64(10, SURA_JIT_DEOPT_SENTINEL);
                        em.cmp_xx(0, 10);
                        deopt_here(0, ip, true);  // EQ
                        em.str_x(0, R, inst.a);
                        break;
                    }
                    // Callee frame on the VM's value stack (see the x86-64
                    // tier): x9 = old top, x12 = new top, x11 = frame base.
                    // x10 stays free for load/store_vm_slot's scratch use.
                    // The capacity check comes first so a deopt here leaves
                    // the budget untouched.
                    load_vm_slot(9, top_off);
                    em.add_imm(12, 9, static_cast<uint16_t>(callee_regs));
                    em.mov_imm64(11, capacity);
                    em.cmp_xx(12, 11);
                    deopt_here(8, ip, false);  // HI
                    // The interpreter refuses the call that would exceed its
                    // frame limit; the budget mirrors that limit so a deep
                    // recursion deopts before the machine stack is at risk
                    // and the interpreter raises its own [E500].
                    load_vm_slot(11, budget_off);
                    em.subs_imm(11, 11, 1);
                    deopt_here(4, ip, false);  // MI
                    store_vm_slot(11, budget_off);
                    store_vm_slot(12, top_off);
                    load_vm_slot(11, vs_off);
                    em.add_lsl(11, 11, 9, 3);
                    for (uint32_t i = 0; i < argc; ++i) {
                        em.ldr_x(9, R, static_cast<uint16_t>(inst.c + i));
                        em.str_x(9, 11, static_cast<uint16_t>(i));
                    }
                    for (uint32_t r = argc; r < callee_regs; ++r) {
                        em.str_x(31, 11, static_cast<uint16_t>(r));  // xzr
                    }
                    em.mov_xx(0, VM);
                    em.mov_xx(1, 11);
                    em.mov_xx(2, K);
                    if (dc.self) {
                        // BL is a direct branch, so it may skip the guards
                        // and their BTI landing pad.
                        self_call_fixups.push_back({em.bl_placeholder(), dc.args_num});
                    } else {
                        // Indirect calls always take the callee's guarded
                        // entry: it starts with the BTI landing pad, and the
                        // guards are a handful of instructions per argument.
                        em.mov_imm64(9, static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(dc.callee.guarded_entry)));
                        em.blr(9);
                    }
                    load_vm_slot(9, top_off);
                    em.sub_imm(9, 9, static_cast<uint16_t>(callee_regs));
                    store_vm_slot(9, top_off);
                    load_vm_slot(9, budget_off);
                    em.add_imm(9, 9, 1);
                    store_vm_slot(9, budget_off);
                    // A guard that failed anywhere below hands back the
                    // sentinel; propagate it rather than storing it. The
                    // pure callee is re-run by the interpreter, so an
                    // exception it parked is not this frame's to rethrow.
                    em.mov_imm64(10, SURA_JIT_DEOPT_SENTINEL);
                    em.cmp_xx(0, 10);
                    deopt_here(0, ip, true);  // EQ
                    em.str_x(0, R, inst.a);
                    break;
                }
                default:
                    return {};
            }
        }

        // Fall-through end of body: only needed when the last reachable
        // instruction does not unconditionally leave the function.
        if (analysis.falls_off_end(chunk, entry_ip, end_ip)) {
            em.mov_imm64(0, NBNIL);
            emit_return();
        }

        // A leaf body has no frame to unwind, so its entry guards and its
        // body checks share one tail, exactly as before.
        if (!needs_vm) {
            deopt_branches.insert(deopt_branches.end(),
                                  entry_deopt_branches.begin(),
                                  entry_deopt_branches.end());
            entry_deopt_branches.clear();
        }
        // Deopt tail shared by a leaf body's zero-divisor checks: hand back
        // the sentinel.
        if (!deopt_branches.empty()) {
            const size_t deopt_at = em.pos();
            em.mov_imm64(0, SURA_JIT_DEOPT_SENTINEL);
            emit_return();
            for (size_t at : deopt_branches) {
                if (!em.patch_branch(at, deopt_at)) return {};
            }
        }
        // Per-site deopt stubs of a framed body: record the resume point,
        // drop a parked exception that belongs to a re-run call, unwind
        // and return the sentinel.
        for (const DeoptSite& site : deopt_sites) {
            if (!em.patch_branch(site.at, em.pos())) return {};
            em.mov_imm64(9, static_cast<uint64_t>(site.ip));
            store_vm_slot(9, resume_off);
            if (site.discard_exc) store_vm_slot(31, exc_off);  // xzr
            em.mov_imm64(0, SURA_JIT_DEOPT_SENTINEL);
            emit_return();
        }
        // Entry-guard tail of a framed body: nothing has been pushed yet.
        if (!entry_deopt_branches.empty()) {
            const size_t deopt_at = em.pos();
            em.mov_imm64(0, SURA_JIT_DEOPT_SENTINEL);
            em.ret();
            for (size_t at : entry_deopt_branches) {
                if (!em.patch_branch(at, deopt_at)) return {};
            }
        }
        for (const BranchFixup& fx : branch_fixups) {
            if (!em.patch_branch(fx.at, ip_off[fx.target_ip - entry_ip])) return {};
        }
        for (const SelfCallFixup& fx : self_call_fixups) {
            if (!em.patch_branch(fx.at, fx.unguarded ? unguarded_entry_offset : 0)) return {};
        }
        return code;
    }
};

// Canonical top-level counted loop accepted by the first OSR tier. Keeping
// the recognizer deliberately strict gives the trace compiler an entry-only
// side-exit contract: no body branch or observable operation can occur before
// the single scalarizable call commits its result.
struct JitStrictCountedLoop {
    size_t header_ip = 0;
    size_t condition_jump_ip = 0;
    size_t call_ip = 0;
    size_t result_store_ip = 0;
    size_t backedge_ip = 0;
    size_t exit_ip = 0;
    int counter_global = -1;
    int result_global = -1;
    int function_global = -1;
    int function_index = -1;
    int limit_constant = -1;
    int increment_constant = -1;
    std::vector<int> argument_globals;
    // Runtime guards depend on live values and class layouts. Once a site has
    // failed those guards, repeatedly rebuilding the same proof at every
    // backedge can be slower than the complete native path. Disabling that
    // site is a semantics-preserving one-way side exit for this compiled VM.
    mutable bool runtime_disabled = false;
};

inline std::vector<uint8_t> jit_osr_callable_ip_mask(const JitChunk& chunk) {
    std::vector<uint8_t> callable_ip(chunk.code.size(), 0);
    auto mark_callable = [&](size_t begin, size_t end) {
        if (begin > end || end > callable_ip.size()) return;
        for (size_t ip = begin; ip < end; ++ip) callable_ip[ip] = 1;
    };
    for (const JitFuncInfo& function : chunk.func_table)
        mark_callable(function.entry_ip, function.end_ip);
    for (const JitClassInfo& cls : chunk.class_table)
        for (const auto& method : cls.methods)
            mark_callable(method.second.entry_ip, method.second.end_ip);
    return callable_ip;
}

// This is only the syntax-level half of OSR callee validation. The trace tier
// must still require a successful, warm-IC ScalarPlan before executing the
// candidate because METHOD_CALL purity cannot be proven from bytecode alone.
inline bool jit_osr_direct_callee_shape(const JitChunk& chunk,
                                        const JitFuncInfo& function) {
    if (function.entry_ip >= function.end_ip ||
        function.end_ip > chunk.code.size()) return false;
    for (size_t ip = function.entry_ip; ip < function.end_ip; ++ip) {
        switch (chunk.code[ip].op) {
            case JitOp::NOP:
            case JitOp::LOAD_CONST:
            case JitOp::LOAD_NIL:
            case JitOp::LOAD_BOOL:
            case JitOp::LOAD_GLOBAL:
            case JitOp::MOVE:
            case JitOp::ADD:
            case JitOp::SUB:
            case JitOp::MUL:
            case JitOp::DIV:
            case JitOp::MOD:
            case JitOp::NEG:
            case JitOp::DOT_GET:
            case JitOp::METHOD_CALL:
            case JitOp::RETURN_VAL:
            case JitOp::RETURN_NONE:
                break;
            default:
                return false;
        }
    }
    return true;
}

inline std::vector<JitStrictCountedLoop>
jit_recognize_strict_counted_loops(const JitChunk& chunk) {
    std::vector<JitStrictCountedLoop> loops;
    if (std::getenv("SURA_JIT_DISABLE_OSR")) return loops;

    const size_t code_size = chunk.code.size();
    const std::vector<uint8_t> callable_ip = jit_osr_callable_ip_mask(chunk);
    for (size_t backedge = 0; backedge < code_size; ++backedge) {
        const JitInst& jump = chunk.code[backedge];
        if (jump.op != JitOp::JUMP || jump.operand < 0 ||
            static_cast<size_t>(jump.operand) >= backedge) {
            continue;
        }
        const size_t header = static_cast<size_t>(jump.operand);
        if (header + 4 > backedge || backedge < 5) continue;
        if (callable_ip[header] || callable_ip[backedge]) continue;

        const JitInst& counter_load = chunk.code[header];
        const JitInst& limit_load = chunk.code[header + 1];
        const JitInst& compare = chunk.code[header + 2];
        const JitInst& condition_jump = chunk.code[header + 3];
        if (counter_load.op != JitOp::LOAD_GLOBAL ||
            limit_load.op != JitOp::LOAD_CONST ||
            compare.op != JitOp::CMP_LT ||
            compare.b != counter_load.a || compare.c != limit_load.a ||
            compare.a == counter_load.a || compare.a == limit_load.a ||
            counter_load.a == limit_load.a ||
            condition_jump.op != JitOp::JUMP_IF_FALSE ||
            condition_jump.a != compare.a || condition_jump.operand < 0 ||
            static_cast<size_t>(condition_jump.operand) != backedge + 1) {
            continue;
        }

        const JitInst& tail_counter_load = chunk.code[backedge - 4];
        const JitInst& increment_load = chunk.code[backedge - 3];
        const JitInst& increment_add = chunk.code[backedge - 2];
        const JitInst& counter_store = chunk.code[backedge - 1];
        if (tail_counter_load.op != JitOp::LOAD_GLOBAL ||
            tail_counter_load.operand != counter_load.operand ||
            increment_load.op != JitOp::LOAD_CONST ||
            increment_add.op != JitOp::ADD ||
            increment_add.b != tail_counter_load.a ||
            increment_add.c != increment_load.a ||
            increment_add.a == tail_counter_load.a ||
            increment_add.a == increment_load.a ||
            tail_counter_load.a == increment_load.a ||
            counter_store.op != JitOp::STORE_GLOBAL ||
            counter_store.a != increment_add.a ||
            counter_store.operand != counter_load.operand) {
            continue;
        }

        if (limit_load.operand < 0 || increment_load.operand < 0 ||
            static_cast<size_t>(limit_load.operand) >= chunk.constants.size() ||
            static_cast<size_t>(increment_load.operand) >= chunk.constants.size()) {
            continue;
        }
        const Value& limit = chunk.constants[static_cast<size_t>(limit_load.operand)];
        const Value& increment =
            chunk.constants[static_cast<size_t>(increment_load.operand)];
        if (!limit.is_num() || !increment.is_num() ||
            !std::isfinite(limit.as_num()) ||
            !std::isfinite(increment.as_num()) || increment.as_num() <= 0.0) {
            continue;
        }

        const size_t body_begin = header + 4;
        const size_t body_end = backedge - 4;
        if (body_begin >= body_end) continue;
        const JitInst& function_load = chunk.code[body_begin];
        if (function_load.op != JitOp::LOAD_GLOBAL) continue;

        size_t call_ip = body_begin + 1;
        while (call_ip < body_end &&
               chunk.code[call_ip].op == JitOp::LOAD_GLOBAL) {
            ++call_ip;
        }
        if (call_ip >= body_end) continue;
        const JitInst& call = chunk.code[call_ip];
        if (call.op != JitOp::CALL_FUNC || call.operand <= 0 ||
            call.b != function_load.a || call.str_idx < 0 ||
            call_ip + 2 != body_end ||
            chunk.code[call_ip + 1].op != JitOp::STORE_GLOBAL ||
            chunk.code[call_ip + 1].a != call.a) {
            continue;
        }
        if (call_ip != body_begin + 1 + static_cast<size_t>(call.operand))
            continue;

        std::vector<int> argument_globals;
        argument_globals.reserve(static_cast<size_t>(call.operand));
        bool arguments_match = true;
        for (int i = 0; i < call.operand; ++i) {
            const JitInst& load = chunk.code[body_begin + 1 + static_cast<size_t>(i)];
            if (load.op != JitOp::LOAD_GLOBAL ||
                load.a != static_cast<uint16_t>(call.c + i)) {
                arguments_match = false;
                break;
            }
            argument_globals.push_back(load.operand);
        }
        if (!arguments_match || argument_globals.empty()) continue;

        const uint32_t argument_begin = call.c;
        const uint32_t argument_end = argument_begin +
            static_cast<uint32_t>(call.operand);
        if ((function_load.a >= argument_begin &&
             function_load.a < argument_end) ||
            call.a == function_load.a ||
            (call.a >= argument_begin && call.a < argument_end)) {
            continue;
        }

        const int result_global = chunk.code[call_ip + 1].operand;
        if (result_global < 0 || result_global != argument_globals.front()) continue;
        if (counter_load.operand < 0 || function_load.operand < 0 ||
            result_global == counter_load.operand ||
            function_load.operand == counter_load.operand ||
            function_load.operand == result_global ||
            static_cast<size_t>(counter_load.operand) >= chunk.global_names.size() ||
            static_cast<size_t>(result_global) >= chunk.global_names.size() ||
            static_cast<size_t>(function_load.operand) >= chunk.global_names.size()) {
            continue;
        }
        bool global_alias = false;
        for (size_t i = 0; i < argument_globals.size(); ++i) {
            const int global = argument_globals[i];
            if (global < 0 ||
                static_cast<size_t>(global) >= chunk.global_names.size() ||
                global == counter_load.operand ||
                global == function_load.operand) {
                global_alias = true;
                break;
            }
            if (std::find(argument_globals.begin(),
                          argument_globals.begin() + static_cast<ptrdiff_t>(i),
                          global) != argument_globals.begin() +
                                    static_cast<ptrdiff_t>(i)) {
                global_alias = true;
                break;
            }
        }
        if (global_alias) continue;

        const std::string& function_name = chunk.get_string(call.str_idx);
        int function_index = -1;
        for (size_t i = 0; i < chunk.func_table.size(); ++i) {
            const JitFuncInfo& function = chunk.func_table[i];
            if (function.name != function_name) continue;
            if (function_index >= 0) {
                function_index = -2;
                break;
            }
            function_index = static_cast<int>(i);
        }
        if (function_index < 0) continue;
        const JitFuncInfo& function =
            chunk.func_table[static_cast<size_t>(function_index)];
        if (!function.upvalues.empty() ||
            function.params.size() != static_cast<size_t>(call.operand) ||
            !jit_osr_direct_callee_shape(chunk, function)) {
            continue;
        }
        if (chunk.global_names[static_cast<size_t>(function_load.operand)] !=
                function_name) {
            continue;
        }

        // The first OSR tier never enters a loop with an active exception
        // handler. In structured top-level bytecode, a handler target beyond
        // the header means the TRY_BEGIN still encloses that header.
        bool unsafe_context = false;
        for (size_t ip = 0; ip < header; ++ip) {
            if (!callable_ip[ip] && chunk.code[ip].op == JitOp::TRY_BEGIN &&
                chunk.code[ip].operand > static_cast<int>(header)) {
                unsafe_context = true;
                break;
            }
        }
        if (unsafe_context) continue;

        // Pin the direct function name to exactly one canonical top-level
        // MAKE_LAMBDA/STORE_GLOBAL definition before the loop. Any callable
        // or later rebinding would invalidate a trace's callee identity.
        size_t function_store_count = 0;
        bool canonical_function_binding = true;
        for (size_t ip = 0; ip < code_size; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (inst.op != JitOp::STORE_GLOBAL ||
                inst.operand != function_load.operand) continue;
            ++function_store_count;
            if (callable_ip[ip] || ip == 0 || ip >= header ||
                callable_ip[ip - 1]) {
                canonical_function_binding = false;
                break;
            }
            const JitInst& initializer = chunk.code[ip - 1];
            if (initializer.op != JitOp::MAKE_LAMBDA ||
                initializer.a != inst.a ||
                initializer.operand != function_index) {
                canonical_function_binding = false;
                break;
            }
        }
        if (!canonical_function_binding || function_store_count != 1) continue;

        JitStrictCountedLoop spec;
        spec.header_ip = header;
        spec.condition_jump_ip = header + 3;
        spec.call_ip = call_ip;
        spec.result_store_ip = call_ip + 1;
        spec.backedge_ip = backedge;
        spec.exit_ip = backedge + 1;
        spec.counter_global = counter_load.operand;
        spec.result_global = result_global;
        spec.function_global = function_load.operand;
        spec.function_index = function_index;
        spec.limit_constant = limit_load.operand;
        spec.increment_constant = increment_load.operand;
        spec.argument_globals = std::move(argument_globals);
        loops.push_back(std::move(spec));
    }
    return loops;
}

// Minimum register extent a direct native function call must publish before
// binding arguments. This deliberately does not include scalar temporaries;
// NativeCompiler extends the result to the successful plan's scratch_next.
inline uint32_t sura_jit_required_function_frame_regs(
    const JitChunk& chunk, const JitFuncInfo& function) {
    uint32_t required = static_cast<uint32_t>(function.params.size());
    const int argc_reg = jit_default_arg_count_reg(
        chunk, function.entry_ip, 0, function.params.size());
    if (argc_reg >= 0) {
        required = std::max<uint32_t>(
            required, static_cast<uint32_t>(argc_reg) + 1U);
    }
    if (function.native_reuse_flag_reg != 65535) {
        required = std::max<uint32_t>(
            required,
            static_cast<uint32_t>(function.native_reuse_flag_reg) + 1U);
    }
    return required;
}


// ── JitVM memory-layout offsets (Win64 / MinGW64) ──────────────────────────
// JitVM first data members (no vtable; static constexpr have no storage):
//   std::vector<Value> value_stack    → 3 pointers = 24 bytes, at offset 0
//     _M_start (Value* data ptr)      → at JitVM + 0
//   size_t stack_top                  → at JitVM + 24
//   std::vector<Value> globals        → 24 bytes, at offset 32
//     _M_start (Value* data ptr)      → at JitVM + 32
static constexpr int32_t JVM_VALUE_STACK_DATA_OFFSET = 0;   // vm->value_stack._M_start
static constexpr int32_t JVM_STACK_TOP_OFFSET        = 24;  // vm->stack_top
static constexpr int32_t JVM_GLOBALS_DATA_OFFSET     = 32;  // vm->globals._M_start
static constexpr int32_t JVM_STACK_CAPACITY          = 1 << 17;

class NativeCompiler {
    const JitChunk& chunk;
    size_t          entry_ip;
    size_t          end_ip;
    std::vector<uint8_t> buf;
    X64Emitter           em;
    bool                 is_top_level = false;  // Phase 10: main chunk compile flag
    // Opcodes actually emitted by this compile; copied into NativeFunc on
    // success. Recorded at the emit_op call sites rather than inside emit_op,
    // which has many early-return paths.
    uint64_t             emitted_ops = 0;
    // The opcode that made compile() give up, for diagnostics. A bail is
    // all-or-nothing - one rejected opcode disqualifies the whole callable -
    // so knowing which one it was is the difference between "this program does
    // not use the JIT" and an actionable "this opcode is disqualifying it",
    // which is how the DIV and USE_LIB bails were found.
    bool                 bailed = false;
    JitOp                bail_op = JitOp::NOP;
    size_t               bail_ip = 0;

    // Pending jumps to resolve after full body is emitted.
    // (disp_field_pos, bytecode_target_ip)
    std::vector<std::pair<size_t, size_t>> pending;
    // Map from bytecode IP to native code offset.
    std::vector<size_t> ip_to_native;
    std::vector<std::vector<uint8_t>> definitely_initialized_globals;
    uint16_t native_scratch_base = 0;
    uint16_t native_scratch_regs = 0;
    uint16_t native_reuse_flag_reg = 65535;
    uint32_t native_frame_regs = 0;
    size_t callable_param_count = 0;
    bool callable_is_method = false;
    bool used_scalar_plan = false;
    bool has_record_reuse_materializer = false;
    uint16_t scalar_scratch_used = 0;
    size_t scalar_last_ip = 0;
    JitOp scalar_last_op = JitOp::NOP;
    std::vector<std::unique_ptr<JitInst>> inline_inst_storage;
    std::vector<std::unique_ptr<JitStrictCountedLoop>> strict_loop_storage;
    std::vector<size_t>* active_scalar_guard_jumps = nullptr;
    // Numeric proof for the general body, used to drop per-operation type
    // guards. Null whenever the proof does not apply.
    std::unique_ptr<BaselineBodyAnalysis> numeric_proof;
    std::vector<JitStrictCountedLoop> strict_counted_loops;
    // Direct-call linkage for the baseline tiers; null keeps LOAD_GLOBAL and
    // CALL_FUNC out of those tiers. Set by the VM for function bodies only.
    BaselineLinkContext* baseline_link = nullptr;
    int                  baseline_self_fidx = -1;
    // NativeFunc::baseline_guard_mask for the body being compiled.
    uint64_t             baseline_guard_mask_ = ~0ULL;

    // Loop register cache. Inside an accepted loop the hottest registers
    // also live in XMM6..XMM15. Every definition through the cache writes
    // both the register slot and the XMM copy, so memory is always current:
    // helpers, the GC, exceptions and every operation this cache does not
    // emit itself keep working unchanged, and such an operation only has to
    // reload the copies it may have overwritten. What the cache removes is
    // the store-to-load round trip on every loop-carried arithmetic chain.
    using LoopCache = JitLoopCache;
    std::vector<LoopCache> loop_caches;
    const LoopCache*     active_cache = nullptr;
    int                  xmm_saved_count = 0;   // XMM6..XMM(5+n) saved by the prologue
    uint32_t             frame_bytes = 48;      // sub rsp, frame_bytes
    uint8_t              prolog_size_ = 9;
    uint8_t              alloc_code_offset_ = 9;
    std::vector<Win64UnwindXmmSave> xmm_saves_;
    static constexpr int kLoopCacheXmmFirst = 6;
    static constexpr int kLoopCacheXmmCount = 10;

    struct ScalarValue {
        bool is_virtual_record = false;
        bool known_numeric = false;
        uint16_t real_reg = 0;
        int guard_input_reg = -1;
        const JitClassInfo* record_class = nullptr;
        std::vector<uint16_t> fields;
        std::vector<uint8_t> field_numeric;
        JitInst constructor = JitInst(JitOp::NOP);
        size_t constructor_ip = 0;
    };

    struct ScalarPlannedOp {
        JitInst inst;
        size_t source_ip = 0;
    };

    struct ScalarGuard {
        uint16_t reg = 0;
        const JitClassInfo* expected_class = nullptr;
    };

    struct ScalarNumericGuard {
        uint16_t input_reg = 0;
        const JitClassInfo* record_class = nullptr;
        int field_index = -1; // -1 guards the input Value itself
    };

    struct ScalarPlan {
        std::vector<ScalarPlannedOp> ops;
        std::vector<ScalarGuard> guards;
        std::vector<ScalarNumericGuard> numeric_guards;
        ScalarValue result;
        uint16_t result_reg = 0;
        uint16_t scratch_next = 0;
    };

public:
    // Accept either JitFuncInfo or JitMethodInfo via their common (entry_ip, end_ip) fields.
    NativeCompiler(const JitChunk& c, const JitFuncInfo& f, bool top_level = false)
        : chunk(c), entry_ip(f.entry_ip), end_ip(f.end_ip), em(buf), is_top_level(top_level),
          native_scratch_base(f.native_scratch_base),
          native_scratch_regs(f.native_scratch_regs),
          native_reuse_flag_reg(f.native_reuse_flag_reg),
          native_frame_regs(f.native_scratch_regs ? f.native_scratch_base :
                            (f.max_regs > 0 ? f.max_regs : 32)),
          callable_param_count(f.params.size()), callable_is_method(false) {
        // This also includes the reuse flag when scalar planning later fails.
        native_frame_regs = std::max<uint32_t>(
            native_frame_regs,
            sura_jit_required_function_frame_regs(chunk, f));
        if (is_top_level) {
            strict_counted_loops = jit_recognize_strict_counted_loops(chunk);
            strict_counted_loops.erase(
                std::remove_if(strict_counted_loops.begin(),
                               strict_counted_loops.end(),
                    [&](const JitStrictCountedLoop& loop) {
                        return top_level_record_reuse_reg(
                            loop.call_ip, chunk.code[loop.call_ip]) < 0;
                    }),
                strict_counted_loops.end());
        }
    }
    NativeCompiler(const JitChunk& c, const JitMethodInfo& m)
        : chunk(c), entry_ip(m.entry_ip), end_ip(m.end_ip), em(buf),
          native_scratch_base(m.native_scratch_base),
          native_scratch_regs(m.native_scratch_regs),
          native_frame_regs(m.native_scratch_regs ? m.native_scratch_base :
                            (m.max_regs > 0 ? m.max_regs : 32)),
          callable_param_count(m.params.size()), callable_is_method(true) {}

    // Set when compile() returned null because emit_op rejected an opcode.
    // Other bail paths (target unsupported, bad ip range, unresolvable jump)
    // leave this false, so a false here means "did not compile, but not
    // because of an unsupported opcode".
    void set_baseline_link(BaselineLinkContext* link, int self_fidx) {
        baseline_link = link;
        baseline_self_fidx = self_fidx;
    }
    // Parameters the VM has seen non-numeric arguments for enter the
    // baseline unguarded (bit clear); see NativeFunc::baseline_guard_mask.
    void set_baseline_guard_mask(uint64_t mask) { baseline_guard_mask_ = mask; }

    bool     did_bail_on_opcode() const { return bailed; }
    JitOp    bailed_opcode() const      { return bail_op; }
    size_t   bailed_ip() const          { return bail_ip; }

    size_t strict_counted_loop_count() const {
        return strict_counted_loops.size();
    }

    bool explicit_throw_dependency(size_t begin_ip,
                                   size_t limit_ip,
                                   std::unordered_set<size_t>& visiting,
                                   std::unordered_map<size_t, bool>& memo) const {
        if (begin_ip >= limit_ip || limit_ip > chunk.code.size()) return false;
        const auto cached = memo.find(begin_ip);
        if (cached != memo.end()) return cached->second;
        if (!visiting.insert(begin_ip).second) return false;

        auto finish = [&](bool result) {
            visiting.erase(begin_ip);
            memo[begin_ip] = result;
            return result;
        };
        auto accepts_arity = [](int argument_count, size_t parameter_count) {
            return argument_count >= 0 &&
                   static_cast<size_t>(argument_count) <= parameter_count;
        };
        auto function_candidate_throws = [&](int argument_count) {
            // CALL_FUNC reads a closure from a register. Even when the source
            // spelling matches a declared function, that variable can later
            // hold a different closure. Every compatible function/lambda in
            // the module is therefore a possible dynamic target.
            for (const JitFuncInfo& function : chunk.func_table) {
                if (!accepts_arity(argument_count, function.params.size())) continue;
                if (explicit_throw_dependency(
                        function.entry_ip, function.end_ip, visiting, memo)) return true;
            }
            return false;
        };
        auto method_candidate_throws = [&](const std::string& method_name,
                                           int argument_count) {
            for (const JitClassInfo& cls : chunk.class_table) {
                const auto found = cls.methods.find(method_name);
                if (found == cls.methods.end() ||
                    !accepts_arity(argument_count, found->second.params.size())) {
                    continue;
                }
                if (explicit_throw_dependency(found->second.entry_ip,
                                               found->second.end_ip,
                                               visiting,
                                               memo)) return true;
            }
            return false;
        };
        auto constructor_candidate_throws = [&](const std::string& class_name,
                                                int argument_count) {
            std::unordered_set<std::string> seen_classes;
            std::string current_name = class_name;
            while (!current_name.empty() && seen_classes.insert(current_name).second) {
                const JitClassInfo* cls = nullptr;
                for (const JitClassInfo& candidate : chunk.class_table) {
                    if (candidate.name == current_name) {
                        cls = &candidate;
                        break;
                    }
                }
                if (!cls) break;
                for (const char* method_name : {
                         JIT_FIELD_INITIALIZER_METHOD, "init", "\uc0dd\uc131\uc790"}) {
                    const auto found = cls->methods.find(method_name);
                    if (found == cls->methods.end()) continue;
                    if (std::string(method_name) != JIT_FIELD_INITIALIZER_METHOD &&
                        !accepts_arity(argument_count, found->second.params.size())) {
                        continue;
                    }
                    if (explicit_throw_dependency(found->second.entry_ip,
                                                  found->second.end_ip,
                                                  visiting,
                                                  memo)) return true;
                }
                current_name = cls->parent;
            }
            return false;
        };

        for (size_t ip = begin_ip; ip < limit_ip; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (inst.op == JitOp::OP_THROW) return finish(true);

            if (inst.op == JitOp::CALL_FUNC) {
                const std::string call_name = inst.str_idx >= 0
                    ? chunk.get_string(inst.str_idx) : std::string();
                if (!call_name.empty() &&
                    constructor_candidate_throws(call_name, inst.operand)) {
                    return finish(true);
                }
                if (function_candidate_throws(inst.operand)) return finish(true);
                continue;
            }

            if (inst.op == JitOp::METHOD_CALL || inst.op == JitOp::SUPER_CALL) {
                const std::string method_name = inst.str_idx >= 0
                    ? chunk.get_string(inst.str_idx) : std::string();
                if (!method_name.empty() &&
                    method_candidate_throws(method_name, inst.operand)) {
                    return finish(true);
                }
                // Dictionaries and instance fields can expose closures as
                // methods, so a method site also has function-value targets.
                if (inst.op == JitOp::METHOD_CALL &&
                    function_candidate_throws(inst.operand)) {
                    return finish(true);
                }
                continue;
            }

            if (inst.op == JitOp::NEW_INSTANCE && inst.str_idx >= 0 &&
                constructor_candidate_throws(chunk.get_string(inst.str_idx),
                                             inst.operand)) {
                return finish(true);
            }
        }
        return finish(false);
    }

    std::unique_ptr<NativeFunc> compile() {
        // Backend selection is centralized in jit_target.hpp so CLI reports,
        // release metadata, and platform tests use the exact same decision.
        if (!sura_jit_target_info().native_supported) return nullptr;
        if (entry_ip >= end_ip) return nullptr;
        if (end_ip > chunk.code.size()) return nullptr;

        // Explicit Sura throws and every callable site that can reach one must
        // keep the interpreter's frame-aware catch path until native frames
        // carry equivalent try/catch metadata. CALL_FUNC is closure-valued,
        // METHOD_CALL is runtime-dispatched, and constructors can execute
        // field initializers, so a source-name-only call graph is insufficient.
        std::unordered_set<size_t> throw_scan;
        std::unordered_map<size_t, bool> throw_memo;
        if (explicit_throw_dependency(entry_ip, end_ip, throw_scan, throw_memo)) {
            return nullptr;
        }

#if SURA_JIT_X64_SYSV_BASELINE || SURA_JIT_X64_WIN64_BASELINE_FIRST
        try {
            // Only closure calls re-run a deopted call, and only they rebind
            // arguments first. The top-level chunk cannot be replayed at all,
            // and method call sites do not check the sentinel, so both compile
            // without guards. A method's register 0 is the receiver anyway.
            const bool deoptable = !is_top_level && !callable_is_method;
            const uint32_t guarded = deoptable
                ? static_cast<uint32_t>(callable_param_count) : 0U;
            SysVBaselineCompiler baseline(chunk, entry_ip, end_ip,
                                          native_frame_regs, guarded, deoptable);
#if SURA_JIT_X64_WIN64_BASELINE_FIRST
            // On Win64 the full tier below handles everything, so the
            // baseline is only worth taking for bodies it accepts whole and
            // can replay: pure numeric closures, where its guarded entry and
            // native-to-native calls beat the full tier's helper calls.
            if (!deoptable) throw std::runtime_error("baseline: not replayable");
            baseline.set_abi(X64BaselineAbi::Win64);
#endif
            // A body can be a direct callee only when a direct call binds
            // exactly what the interpreter would: every parameter, guarded,
            // and no hidden argument-count register.
            const bool direct_callable =
                deoptable && baseline_link != nullptr &&
                jit_default_arg_count_reg(chunk, entry_ip, 0, callable_param_count) < 0 &&
                native_frame_regs <= BaselineBodyAnalysis::kMaxDirectCalleeRegs;
            if (baseline_link) {
                baseline.set_link(baseline_link, baseline_self_fidx, direct_callable);
                baseline.set_guard_mask(baseline_guard_mask_);
#if SURA_JIT_X64_WIN64_BASELINE_FIRST
                // The full tier below already compiles arrays, strings and
                // dictionaries with its own helpers; the baseline keeps to
                // the bodies it beats that tier on.
                baseline.set_allow_helpers(false);
#else
                baseline.set_allow_helpers(true);
#endif
            }
            std::vector<uint8_t> baseline_code = baseline.compile_bytes();
#if SURA_JIT_X64_WIN64_BASELINE_FIRST
            if (baseline_code.empty()) throw std::runtime_error("baseline: refused");
#else
            if (baseline_code.empty()) return nullptr;
#endif
            auto nf = std::make_unique<NativeFunc>();
            nf->code = ExecCode::from_bytes(baseline_code);
            nf->fn = reinterpret_cast<SuraNativeFn>(nf->code.ptr);
            nf->frame_regs = native_frame_regs;
            nf->baseline_direct_callable = direct_callable && baseline.pure;
            nf->baseline_vm_frame = baseline.vm_frame;
            nf->baseline_guard_mask = baseline_guard_mask_;
            nf->baseline_params = static_cast<uint32_t>(callable_param_count);
            nf->baseline_unguarded_entry = baseline.unguarded_entry_offset;
            nf->baseline_return_kind = static_cast<uint8_t>(baseline.return_kind.k);
            nf->baseline_return_fidx = baseline.return_kind.fidx;
            for (size_t ip = entry_ip; ip < end_ip; ++ip)
                nf->emitted_ops |= 1ULL << static_cast<int>(chunk.code[ip].op);
            return nf;
        } catch (...) {
#if SURA_JIT_X64_WIN64_BASELINE_FIRST
            // Fall through to the full Win64 tier.
#else
            return nullptr;
#endif
        }
#endif

#if SURA_JIT_ARM64_BASELINE
        try {
            const bool deoptable = !is_top_level && !callable_is_method;
            const uint32_t guarded = deoptable
                ? static_cast<uint32_t>(callable_param_count) : 0U;
            Arm64BaselineCompiler baseline(chunk, entry_ip, end_ip,
                                           native_frame_regs, guarded, deoptable);
            const bool direct_callable =
                deoptable && baseline_link != nullptr &&
                jit_default_arg_count_reg(chunk, entry_ip, 0, callable_param_count) < 0 &&
                native_frame_regs <= BaselineBodyAnalysis::kMaxDirectCalleeRegs;
            if (baseline_link) {
                baseline.set_link(baseline_link, baseline_self_fidx, direct_callable);
                baseline.set_guard_mask(baseline_guard_mask_);
                baseline.set_allow_helpers(true);
            }
            std::vector<uint8_t> baseline_code = baseline.compile_bytes();
            if (baseline_code.empty()) return nullptr;
            auto nf = std::make_unique<NativeFunc>();
            nf->code = ExecCode::from_bytes(baseline_code);
            nf->fn = reinterpret_cast<SuraNativeFn>(nf->code.ptr);
            nf->frame_regs = native_frame_regs;
            nf->baseline_direct_callable = direct_callable && baseline.pure;
            nf->baseline_vm_frame = baseline.vm_frame;
            nf->baseline_guard_mask = baseline_guard_mask_;
            nf->baseline_params = static_cast<uint32_t>(callable_param_count);
            nf->baseline_unguarded_entry = baseline.unguarded_entry_offset;
            nf->baseline_return_kind = static_cast<uint8_t>(baseline.return_kind.k);
            nf->baseline_return_fidx = baseline.return_kind.fidx;
            for (size_t ip = entry_ip; ip < end_ip; ++ip)
                nf->emitted_ops |= 1ULL << static_cast<int>(chunk.code[ip].op);
            return nf;
        } catch (...) {
            return nullptr;
        }
#endif

        ip_to_native.assign(chunk.code.size() + 1, SIZE_MAX);
        compute_definitely_initialized_globals();

        // Whole-body numeric proof, used below to drop per-operation type
        // guards. Parameters are deliberately not assumed numeric (0 guarded
        // registers): this tier has no entry guard to enforce that, so only
        // values this body itself produced from constants and arithmetic
        // count as proven. DIV keeps its checked helper, so the analysis is
        // told not to reject a body over an unprovable divisor.
        try {
            auto proof = std::make_unique<BaselineBodyAnalysis>(
                chunk, entry_ip, end_ip, native_frame_regs,
                /*guarded_params=*/0u,
                std::numeric_limits<uint16_t>::max(),
                std::numeric_limits<int32_t>::max() / 8,
                /*allow_runtime_deopt=*/false,
                /*require_provable_div=*/false,
                /*link=*/nullptr, /*self_fidx=*/-1, /*self_callable=*/false,
                /*guard_mask=*/~0ULL, /*allow_helpers=*/false,
                /*proof_only=*/true);
            if (proof->ok) numeric_proof = std::move(proof);
        } catch (...) {
            numeric_proof.reset();
        }
        plan_loop_caches();

        // ── Prologue ──────────────────────────────────────
        // Save non-volatile regs we use (Win64: RBX, R12-R15 are callee-saved).
        // Three pushes = 24 bytes. Combined with the call's 8-byte return
        // address, RSP becomes 16-aligned. Reserve one fixed 48-byte frame for
        // Win64 shadow space, a [rsp+32] spill/5th-argument slot, and padding.
        // Keeping RSP fixed across the body makes the whole generated function
        // exactly describable by one dynamic UNWIND_INFO record.
        //   push rbx; push r12; push r13; sub rsp, 48
        //   mov r13, rcx   ; r13 = vm
        //   mov rbx, rdx   ; rbx = R
        //   mov r12, r8    ; r12 = consts
        em.push_r(XR::RBX);
        em.push_r(XR::R12);
        em.push_r(XR::R13);
        if (frame_bytes <= 127) em.sub_rsp_imm8(static_cast<int8_t>(frame_bytes));
        else em.sub_rsp_imm32(static_cast<int32_t>(frame_bytes));
        alloc_code_offset_ = static_cast<uint8_t>(em.pos());
        // Loop register cache: XMM6.. are callee-saved, so a body that keeps
        // registers in them saves them at [rsp+48..] and the unwind record
        // below describes those saves for exceptions thrown by helpers.
        xmm_saves_.clear();
        for (int k = 0; k < xmm_saved_count; ++k) {
            em.movdqu_mem_x(XR::RSP, 48 + 16 * k, XR::XMM6 + k);
            xmm_saves_.push_back({static_cast<uint8_t>(em.pos()),
                                  static_cast<uint8_t>(XR::XMM6 + k),
                                  static_cast<uint32_t>(48 + 16 * k)});
        }
        prolog_size_ = static_cast<uint8_t>(em.pos());
        if (is_top_level) {
            // [rsp+40] is an integer base index for the one eligible
            // GC-visible persistent callee frame. It never stores a Value or
            // replaces the VM root stack; zero means no frame is published.
            em.xor_rr(XR::RAX, XR::RAX);
            em.mov_rsp_disp8_r(40, XR::RAX);
        }
        em.mov_rr(XR::R13, XR::RCX);
        em.mov_rr(XR::RBX, XR::RDX);
        em.mov_rr(XR::R12, XR::R8);

        // Materialize strict counted-loop descriptors once.  The probe is
        // deliberately a side path: a failed runtime shape check returns 0
        // and the original bytecode remains the semantic fallback.
        std::vector<const JitStrictCountedLoop*> strict_at(chunk.code.size(), nullptr);
        if (is_top_level) {
            // Use the constructor's already-filtered candidates.  Re-running
            // the syntax recognizer here would bypass escape/reuse checks and
            // environment switches used by top_level_record_reuse_reg().
            for (const auto& recognized : strict_counted_loops) {
                if (recognized.function_index < 0 ||
                    static_cast<size_t>(recognized.function_index) >= chunk.func_table.size()) {
                    continue;
                }
                auto owned = std::make_unique<JitStrictCountedLoop>(recognized);
                const auto* stable = owned.get();
                if (stable->header_ip < strict_at.size() &&
                    strict_at[stable->header_ip] == nullptr) {
                    strict_at[stable->header_ip] = stable;
                    strict_loop_storage.push_back(std::move(owned));
                }
            }
        }

        // ── Body ──────────────────────────────────────────
        // Guarded escape analysis for straight-line record code. A successful
        // plan inlines monomorphic methods and keeps their constructor results
        // as virtual field tuples. Only the value that escapes through RETURN
        // is materialized. A class-identity miss enters the complete general
        // body below, preserving overrides and polymorphic dispatch.
        {
            ScalarPlan scalar_plan;
            std::vector<size_t> scalar_guard_jumps;
            if (build_scalar_plan(scalar_plan)) {
                used_scalar_plan = true;
                scalar_scratch_used = static_cast<uint16_t>(
                    scalar_plan.scratch_next - native_scratch_base);
                native_frame_regs = std::max<uint32_t>(
                    native_frame_regs, scalar_plan.scratch_next);
                for (const ScalarGuard& guard : scalar_plan.guards) {
                    emit_scalar_guard(guard, scalar_guard_jumps);
                }
                for (const ScalarNumericGuard& guard : scalar_plan.numeric_guards) {
                    emit_scalar_numeric_guard(guard, scalar_guard_jumps);
                }
                // Every native call entry initializes its complete published
                // frame before stack_top is advanced. Do not clear the scalar
                // window a second time here; keeping one initialization point
                // also covers the class-guard side exit into the general body.
                active_scalar_guard_jumps = &scalar_guard_jumps;
                for (const ScalarPlannedOp& planned : scalar_plan.ops) {
                    auto runtime_inst = std::make_unique<JitInst>(planned.inst);
                    const JitInst* stable = runtime_inst.get();
                    inline_inst_storage.push_back(std::move(runtime_inst));
                    if (!emit_op(*stable, planned.source_ip, stable)) {
                        bailed = true; bail_op = stable->op; bail_ip = planned.source_ip;
                        return nullptr;
                    }
                    emitted_ops |= (uint64_t)1 << (int)stable->op;
                }
                active_scalar_guard_jumps = nullptr;
                emit_epilogue_with_reg(scalar_plan.result.real_reg);

                const size_t general_body = em.pos();
                for (size_t jump : scalar_guard_jumps) {
                    em.patch_rel32(jump, general_body);
                }
            }

            size_t next_loop_cache = 0;
            active_cache = nullptr;
            for (size_t ip = entry_ip; ip < end_ip; ++ip) {
                if (next_loop_cache < loop_caches.size() &&
                    loop_caches[next_loop_cache].header_ip == ip) {
                    active_cache = &loop_caches[next_loop_cache++];
                    for (uint16_t r : active_cache->regs)
                        em.movsd_x_mem(active_cache->xmm_of[r], XR::RBX, off_r(r));
                }
                ip_to_native[ip] = em.pos();
                if (is_top_level && ip < strict_at.size() && strict_at[ip] != nullptr) {
                    const JitStrictCountedLoop* spec = strict_at[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)&chunk);
                    em.mov_ri64(XR::R9, (uint64_t)(uintptr_t)spec);
                    em.mov_ri64(XR::RAX,
                                (uint64_t)(uintptr_t)&sura_jit_strict_vector_loop);
                    em.call_rax();
                    em.cmp_r_imm32(XR::RAX, 1);
                    const size_t fast_exit =
                        em.jcc_rel32_placeholder(CC::E);
                    pending.push_back({fast_exit, spec->exit_ip});
                }
                if (!emit_op(chunk.code[ip], ip)) {
                    bailed = true; bail_op = chunk.code[ip].op; bail_ip = ip;
                    return nullptr;
                }
                emitted_ops |= (uint64_t)1 << (int)chunk.code[ip].op;
                if (active_cache != nullptr && ip == active_cache->backedge_ip)
                    active_cache = nullptr;
            }
            active_cache = nullptr;
            // Safety net: if control falls off the end, return nil.
            emit_epilogue_nil();

        // ── Resolve jumps ─────────────────────────────────
            for (auto& pj : pending) {
                size_t target_ip = pj.second;
                if (target_ip >= ip_to_native.size()) return nullptr;
                size_t native_target = ip_to_native[target_ip];
                if (native_target == SIZE_MAX) return nullptr;
                em.patch_rel32(pj.first, native_target);
            }
        }

        try {
            auto nf = std::make_unique<NativeFunc>();
            const Win64UnwindSpec unwind{
                prolog_size_, alloc_code_offset_, frame_bytes,
                {{1, 3}, {3, 12}, {5, 13}}, xmm_saves_
            };
            nf->code = ExecCode::from_bytes(buf, unwind);
            if (const char* dump_dir = std::getenv("SURA_JIT_DUMP_DIR")) {
                const std::string path = std::string(dump_dir) + "/full_" +
                                         std::to_string(entry_ip) + ".bin";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(buf.data(), 1, buf.size(), f);
                    std::fclose(f);
                }
            }
            nf->fn   = (SuraNativeFn)nf->code.ptr;
            nf->scalarized = used_scalar_plan;
            nf->record_reuse_capable = has_record_reuse_materializer;
            nf->frame_regs = native_frame_regs;
            nf->inline_insts = std::move(inline_inst_storage);
            nf->strict_loops = std::move(strict_loop_storage);
            nf->emitted_ops = emitted_ops;
            return nf;
        } catch (...) {
            return nullptr;
        }
    }

private:
    // Byte offset of R[idx] from RBX
    static int32_t off_r(uint16_t idx) { return (int32_t)(idx * 8); }
    static int32_t off_c(int idx)      { return (int32_t)(idx * 8); }

    void emit_clear_register_range(uint16_t first, uint16_t count) {
        if (count == 0) return;
        em.pxor_xx(XR::XMM5, XR::XMM5);
        uint16_t i = 0;
        for (; static_cast<uint32_t>(i) + 1U < count; i = static_cast<uint16_t>(i + 2)) {
            em.movdqu_mem_x(XR::RBX,
                            off_r(static_cast<uint16_t>(first + i)), XR::XMM5);
        }
        if (i < count) {
            em.xor_rr(XR::RAX, XR::RAX);
            em.mov_mem_r(XR::RBX,
                         off_r(static_cast<uint16_t>(first + i)), XR::RAX);
        }
    }

    // RCX = exact frame register count, RDX = frame base. Clears the active
    // root range two Values at a time. RDX is advanced and R8/RAX/XMM5 are
    // clobbered; callers recompute the frame base before binding arguments.
    void emit_clear_dynamic_frame() {
        em.pxor_xx(XR::XMM5, XR::XMM5);
        em.mov_rr(XR::R8, XR::RCX);
        em.cmp_r_imm32(XR::R8, 2);
        const size_t to_tail = em.jcc_rel32_placeholder(CC::B);

        const size_t pair_loop = em.pos();
        em.movdqu_mem_x(XR::RDX, 0, XR::XMM5);
        em.add_r_imm32(XR::RDX, 16);
        em.sub_r_imm32(XR::R8, 2);
        em.cmp_r_imm32(XR::R8, 2);
        const size_t repeat_pairs = em.jcc_rel32_placeholder(CC::AE);
        em.patch_rel32(repeat_pairs, pair_loop);

        const size_t tail = em.pos();
        em.patch_rel32(to_tail, tail);
        em.cmp_r_imm32(XR::R8, 0);
        const size_t done = em.jcc_rel32_placeholder(CC::E);
        em.xor_rr(XR::RAX, XR::RAX);
        em.mov_mem_r(XR::RDX, 0, XR::RAX);
        em.patch_rel32(done, em.pos());
    }

    static ScalarValue scalar_real(uint16_t reg, int guard_input_reg = -1) {
        ScalarValue value;
        value.real_reg = reg;
        value.guard_input_reg = guard_input_reg;
        return value;
    }

    bool scalar_alloc(ScalarPlan& plan, size_t count, uint16_t& first) const {
        if (count == 0) return false;
        const uint32_t limit = static_cast<uint32_t>(native_scratch_base) +
                               native_scratch_regs;
        const uint32_t next = static_cast<uint32_t>(plan.scratch_next) +
                              static_cast<uint32_t>(count);
        if (next > limit || next > 65535U) return false;
        first = plan.scratch_next;
        plan.scratch_next = static_cast<uint16_t>(next);
        return true;
    }

    static bool scalar_is_binary(JitOp op) {
        switch (op) {
            case JitOp::ADD: case JitOp::SUB: case JitOp::MUL:
            case JitOp::MOD: case JitOp::BIT_AND: case JitOp::BIT_OR:
            case JitOp::BIT_XOR: case JitOp::LSHIFT: case JitOp::RSHIFT:
            case JitOp::CMP_EQ: case JitOp::CMP_NEQ: case JitOp::CMP_LT:
            case JitOp::CMP_LTE: case JitOp::CMP_GT: case JitOp::CMP_GTE:
                return true;
            default:
                return false;
        }
    }

    static bool scalar_is_unary(JitOp op) {
        return op == JitOp::NEG || op == JitOp::BIT_NOT ||
               op == JitOp::LOGICAL_NOT;
    }

    bool scalar_exact_plain_ctor(const JitInst& inst,
                                 const JitClassInfo*& resolved_class,
                                 const JitMethodInfo*& resolved_ctor) const {
        resolved_class = inst.ic_class;
        resolved_ctor = inst.ic_method;
        if ((!resolved_class || !resolved_ctor) && inst.str_idx >= 0) {
            const std::string& class_name = chunk.get_string(inst.str_idx);
            size_t matches = 0;
            for (const JitClassInfo& cls : chunk.class_table) {
                if (cls.name != class_name) continue;
                ++matches;
                resolved_class = &cls;
                auto ctor = cls.methods.find("init");
                if (ctor == cls.methods.end()) ctor = cls.methods.find("생성자");
                if (ctor != cls.methods.end()) resolved_ctor = &ctor->second;
            }
            // An inherited or multiply-defined static layout needs a runtime
            // layout/deoptimization guard that this tier does not yet carry.
            if (matches != 1 || !resolved_class || !resolved_class->parent.empty()) {
                resolved_class = nullptr;
                resolved_ctor = nullptr;
            }
        }
        auto reject = [&](const char* stage) {
            if (std::getenv("SURA_JIT_DIAG")) {
                std::cerr << "[JIT diag] scalar ctor rejected at " << stage
                          << " nargs=" << inst.operand
                          << " class=" << (resolved_class ? resolved_class->name : "<none>")
                          << " method=" << (resolved_ctor ? resolved_ctor->name : "<none>")
                          << " native=" << (inst.ic_native_fn != nullptr) << "\n";
            }
            return false;
        };
        size_t static_name_matches = 0;
        if (resolved_class) {
            for (const JitClassInfo& cls : chunk.class_table) {
                if (cls.name == resolved_class->name) ++static_name_matches;
            }
        }
        if (inst.op != JitOp::CALL_FUNC ||
            (inst.operand != 2 && inst.operand != 3) ||
            !resolved_class || !resolved_ctor ||
            static_name_matches != 1 ||
            !resolved_class->parent.empty() ||
            resolved_ctor->params.size() != static_cast<size_t>(inst.operand) ||
            resolved_class->field_defaults.size() != static_cast<size_t>(inst.operand)) {
            return reject("shape");
        }
        for (int i = 0; i < inst.operand; ++i) {
            const auto field = resolved_class->field_indices.find(
                resolved_ctor->params[static_cast<size_t>(i)]);
            if (field == resolved_class->field_indices.end() || field->second != i) {
                return reject("field-map");
            }
        }

        // With every argument present, an executable-default prologue is
        // skipped. The remaining constructor must be exactly the generated
        // self.field = parameter sequence followed by RETURN_NONE; otherwise
        // user side effects require the general constructor path.
        const JitMethodInfo& ctor = *resolved_ctor;
        const size_t tail_width = static_cast<size_t>(inst.operand) * 3U + 1U;
        if (ctor.entry_ip > ctor.end_ip || ctor.end_ip > chunk.code.size() ||
            ctor.end_ip - ctor.entry_ip < tail_width) {
            return reject("range");
        }
        const size_t tail = ctor.end_ip - tail_width;
        if (tail != ctor.entry_ip &&
            jit_default_arg_count_reg(chunk, ctor.entry_ip, 1,
                                      ctor.params.size()) < 0) {
            return reject("default-marker");
        }
        for (int i = 0; i < inst.operand; ++i) {
            const size_t ip = tail + static_cast<size_t>(i) * 3U;
            const JitInst& self_move = chunk.code[ip];
            const JitInst& value_move = chunk.code[ip + 1];
            const JitInst& field_set = chunk.code[ip + 2];
            if (self_move.op != JitOp::MOVE || self_move.b != 0 ||
                value_move.op != JitOp::MOVE || value_move.b != i + 1 ||
                field_set.op != JitOp::DOT_SET ||
                field_set.a != self_move.a || field_set.b != value_move.a ||
                field_set.str_idx < 0 ||
                chunk.get_string(field_set.str_idx) !=
                    ctor.params[static_cast<size_t>(i)]) {
                return reject("setter-tail");
            }
        }
        if (chunk.code[ctor.end_ip - 1].op != JitOp::RETURN_NONE)
            return reject("return-tail");
        return true;
    }

    bool scalar_add_guard(ScalarPlan& plan, uint16_t reg,
                          const JitClassInfo* expected) const {
        if (!expected) return false;
        const size_t input_regs = callable_param_count +
                                  (callable_is_method ? 1U : 0U);
        // Guards are hoisted ahead of all optimized work. Only original input
        // registers are stable there; derived object values take the general
        // path instead of risking reordered observable behavior.
        if (reg >= input_regs) return false;
        for (const ScalarGuard& guard : plan.guards) {
            if (guard.reg == reg && guard.expected_class == expected) return true;
        }
        plan.guards.push_back({reg, expected});
        return true;
    }

    bool scalar_add_numeric_guard(ScalarPlan& plan, uint16_t input_reg,
                                  const JitClassInfo* record_class = nullptr,
                                  int field_index = -1) const {
        if (std::getenv("SURA_JIT_DISABLE_NUMERIC_HOIST")) return false;
        const size_t input_regs = callable_param_count +
                                  (callable_is_method ? 1U : 0U);
        if (input_reg >= input_regs || field_index < -1) return false;
        if ((field_index >= 0) != (record_class != nullptr)) return false;
        for (const ScalarNumericGuard& guard : plan.numeric_guards) {
            if (guard.input_reg == input_reg &&
                guard.record_class == record_class &&
                guard.field_index == field_index) {
                return true;
            }
        }
        plan.numeric_guards.push_back({input_reg, record_class, field_index});
        return true;
    }

    bool scalar_process_range(size_t begin, size_t end,
                              std::vector<ScalarValue>& regs,
                              bool allocate_destinations,
                              ScalarPlan& plan,
                              ScalarValue& result,
                              int depth,
                              size_t& virtual_ctor_count,
                              size_t& inlined_method_count) {
        if (depth > 8 || begin >= end || end > chunk.code.size()) return false;

        auto read = [&](uint16_t reg, ScalarValue& value) -> bool {
            if (static_cast<size_t>(reg) >= regs.size()) return false;
            value = regs[reg];
            return true;
        };
        auto write = [&](uint16_t reg, const ScalarValue& value) -> bool {
            if (static_cast<size_t>(reg) >= regs.size()) return false;
            regs[reg] = value;
            return true;
        };
        auto destination = [&](uint16_t original, uint16_t& physical) -> bool {
            if (!allocate_destinations) {
                if (original >= native_scratch_base) return false;
                physical = original;
                return true;
            }
            return scalar_alloc(plan, 1, physical);
        };
        auto add_op = [&](const JitInst& original, size_t ip,
                          uint16_t a, uint16_t b, uint16_t c,
                          bool numeric_fast = false,
                          bool scalar_guarded_field = false,
                          bool scalar_numeric_field = false,
                          int guarded_field_index = -1,
                          const JitClassInfo* guarded_class = nullptr) {
            JitInst remapped = original;
            remapped.a = a;
            remapped.b = b;
            remapped.c = c;
            remapped.ic_numeric_fast = numeric_fast;
            remapped.ic_scalar_guarded_field = scalar_guarded_field;
            remapped.ic_scalar_numeric_field = scalar_numeric_field;
            if (scalar_guarded_field) {
                remapped.ic_cache = guarded_field_index;
                remapped.ic_class = guarded_class;
            }
            plan.ops.push_back({remapped, ip});
        };

        for (size_t ip = begin; ip < end; ++ip) {
            const JitInst& inst = chunk.code[ip];
            scalar_last_ip = ip;
            scalar_last_op = inst.op;

            if (inst.op == JitOp::RETURN_VAL) {
                return read(inst.a, result);
            }
            if (inst.op == JitOp::RETURN_NONE) {
                // Compiler-appended fallback after an unconditional value
                // return is unreachable and may be ignored. A reachable nil
                // return is not a record-scalarization candidate.
                return false;
            }
            if (inst.op == JitOp::NOP) {
                if (inst.operand == JIT_DEFAULT_PROLOGUE_MAGIC) return false;
                continue;
            }

            if (inst.op == JitOp::LOAD_CONST || inst.op == JitOp::LOAD_NIL ||
                inst.op == JitOp::LOAD_BOOL || inst.op == JitOp::LOAD_GLOBAL) {
                uint16_t out = 0;
                if (!destination(inst.a, out)) return false;
                add_op(inst, ip, out, inst.b, inst.c);
                ScalarValue loaded = scalar_real(out);
                loaded.known_numeric =
                    inst.op == JitOp::LOAD_CONST && inst.operand >= 0 &&
                    static_cast<size_t>(inst.operand) < chunk.constants.size() &&
                    chunk.constants[static_cast<size_t>(inst.operand)].is_num();
                if (!write(inst.a, loaded)) return false;
                continue;
            }

            if (inst.op == JitOp::MOVE) {
                ScalarValue source;
                if (!read(inst.b, source)) return false;
                if (source.is_virtual_record) {
                    if (!write(inst.a, source)) return false;
                    continue;
                }
                uint16_t out = 0;
                if (!destination(inst.a, out)) return false;
                add_op(inst, ip, out, source.real_reg, 0);
                ScalarValue moved = scalar_real(out, source.guard_input_reg);
                moved.known_numeric = source.known_numeric;
                moved.record_class = source.record_class;
                if (!write(inst.a, moved)) return false;
                continue;
            }

            if (scalar_is_binary(inst.op)) {
                ScalarValue left, right;
                uint16_t out = 0;
                if (!read(inst.b, left) || !read(inst.c, right) ||
                    left.is_virtual_record || right.is_virtual_record ||
                    !destination(inst.a, out)) {
                    return false;
                }
                const bool numeric_candidate =
                    inst.op == JitOp::ADD || inst.op == JitOp::SUB ||
                    inst.op == JitOp::MUL;
                auto ensure_numeric = [&](ScalarValue& value) {
                    if (value.known_numeric) return true;
                    if (value.guard_input_reg < 0) return false;
                    if (!scalar_add_numeric_guard(
                            plan, static_cast<uint16_t>(value.guard_input_reg))) {
                        return false;
                    }
                    value.known_numeric = true;
                    return true;
                };
                const bool numeric_fast = numeric_candidate &&
                                          ensure_numeric(left) &&
                                          ensure_numeric(right);
                add_op(inst, ip, out, left.real_reg, right.real_reg, numeric_fast);
                ScalarValue calculated = scalar_real(out);
                calculated.known_numeric = numeric_fast;
                if (!write(inst.a, calculated)) return false;
                continue;
            }

            if (scalar_is_unary(inst.op)) {
                ScalarValue operand;
                uint16_t out = 0;
                if (!read(inst.b, operand) || operand.is_virtual_record ||
                    !destination(inst.a, out)) {
                    return false;
                }
                add_op(inst, ip, out, operand.real_reg, 0);
                if (!write(inst.a, scalar_real(out))) return false;
                continue;
            }

            if (inst.op == JitOp::DOT_GET) {
                ScalarValue object;
                if (!read(inst.b, object)) return false;
                if (object.is_virtual_record) {
                    if (!object.record_class || inst.str_idx < 0) return false;
                    const auto field = object.record_class->field_indices.find(
                        chunk.get_string(inst.str_idx));
                    if (field == object.record_class->field_indices.end() ||
                        field->second < 0 ||
                        static_cast<size_t>(field->second) >= object.fields.size()) {
                        return false;
                    }
                    ScalarValue field_value = scalar_real(
                        object.fields[static_cast<size_t>(field->second)]);
                    if (static_cast<size_t>(field->second) < object.field_numeric.size()) {
                        field_value.known_numeric =
                            object.field_numeric[static_cast<size_t>(field->second)] != 0;
                    }
                    if (!write(inst.a, field_value)) {
                        return false;
                    }
                    continue;
                }
                uint16_t out = 0;
                if (!destination(inst.a, out)) return false;
                ScalarValue field_value = scalar_real(out);
                const JitClassInfo* expected = object.record_class
                    ? object.record_class : inst.ic_class;
                bool guarded_field = false;
                int guarded_field_index = -1;
                if (!std::getenv("SURA_JIT_DISABLE_FIELD_HOIST") &&
                    object.guard_input_reg >= 0 && expected && inst.str_idx >= 0) {
                    const auto field = expected->field_indices.find(
                        chunk.get_string(inst.str_idx));
                    if (field != expected->field_indices.end() && field->second >= 0 &&
                        scalar_add_guard(plan,
                            static_cast<uint16_t>(object.guard_input_reg), expected)) {
                        guarded_field = true;
                        guarded_field_index = field->second;
                    }
                    if (guarded_field &&
                        !std::getenv("SURA_JIT_DISABLE_NUMERIC_HOIST")) {
                        field_value.known_numeric = true;
                    }
                }
                add_op(inst, ip, out, object.real_reg, 0, false,
                       guarded_field, field_value.known_numeric,
                       guarded_field_index, expected);
                if (!write(inst.a, field_value)) return false;
                continue;
            }

            if (inst.op == JitOp::CALL_FUNC) {
                const JitClassInfo* record_class = nullptr;
                const JitMethodInfo* record_ctor = nullptr;
                if (!scalar_exact_plain_ctor(inst, record_class, record_ctor)) return false;
                ScalarValue function_value;
                if (!read(inst.b, function_value) ||
                    function_value.is_virtual_record) {
                    return false;
                }
                uint16_t fields_begin = 0;
                if (!scalar_alloc(plan, static_cast<size_t>(inst.operand), fields_begin)) {
                    return false;
                }
                for (int i = 0; i < inst.operand; ++i) {
                    ScalarValue argument;
                    if (!read(static_cast<uint16_t>(inst.c + i), argument) ||
                        argument.is_virtual_record) {
                        return false;
                    }
                    JitInst move(JitOp::MOVE,
                                 static_cast<uint16_t>(fields_begin + i),
                                 argument.real_reg, 0, 0, -1, inst.line);
                    plan.ops.push_back({move, ip});
                }

                ScalarValue virtual_value;
                virtual_value.is_virtual_record = true;
                virtual_value.record_class = record_class;
                virtual_value.fields.reserve(static_cast<size_t>(inst.operand));
                virtual_value.field_numeric.reserve(static_cast<size_t>(inst.operand));
                for (int i = 0; i < inst.operand; ++i) {
                    ScalarValue argument;
                    if (!read(static_cast<uint16_t>(inst.c + i), argument)) return false;
                    virtual_value.fields.push_back(
                        static_cast<uint16_t>(fields_begin + i));
                    virtual_value.field_numeric.push_back(argument.known_numeric ? 1U : 0U);
                }
                virtual_value.constructor = inst;
                virtual_value.constructor.b = function_value.real_reg;
                virtual_value.constructor.c = fields_begin;
                virtual_value.constructor_ip = ip;
                if (!write(inst.a, virtual_value)) return false;
                ++virtual_ctor_count;
                continue;
            }

            if (inst.op == JitOp::METHOD_CALL) {
                if (!inst.ic_class || !inst.ic_method || inst.operand < 0 ||
                    static_cast<size_t>(inst.operand) != inst.ic_method->params.size()) {
                    return false;
                }
                ScalarValue receiver;
                if (!read(inst.b, receiver)) return false;
                if (receiver.is_virtual_record) {
                    if (!receiver.record_class ||
                        receiver.record_class->name != inst.ic_class->name) return false;
                } else if (receiver.guard_input_reg < 0 ||
                           !scalar_add_guard(plan,
                               static_cast<uint16_t>(receiver.guard_input_reg),
                               inst.ic_class)) {
                    return false;
                } else {
                    receiver.record_class = inst.ic_class;
                }

                const JitMethodInfo& method = *inst.ic_method;
                const size_t method_base_regs = method.native_scratch_regs
                    ? method.native_scratch_base : method.max_regs;
                if (method_base_regs == 0 || method_base_regs > 65535U) return false;
                std::vector<ScalarValue> method_regs(method_base_regs);
                for (size_t i = 0; i < method_regs.size(); ++i) {
                    method_regs[i] = scalar_real(static_cast<uint16_t>(i));
                }
                method_regs[0] = receiver;
                for (size_t i = 0; i < method.params.size(); ++i) {
                    ScalarValue argument;
                    const uint32_t arg_reg = static_cast<uint32_t>(inst.b) + 1U +
                                             static_cast<uint32_t>(i);
                    if (arg_reg > 65535U || !read(static_cast<uint16_t>(arg_reg), argument) ||
                        i + 1 >= method_regs.size()) {
                        return false;
                    }
                    method_regs[i + 1] = argument;
                }

                ScalarValue method_result;
                if (!scalar_process_range(method.entry_ip, method.end_ip,
                                          method_regs, true, plan, method_result,
                                          depth + 1, virtual_ctor_count,
                                          inlined_method_count)) {
                    return false;
                }
                if (!write(inst.a, method_result)) return false;
                ++inlined_method_count;
                continue;
            }

            // Control flow, mutation, external calls and exception regions are
            // kept on the complete native/VM path until a deoptimization map is
            // available for them.
            return false;
        }
        return false;
    }

    bool build_scalar_plan(ScalarPlan& plan) {
        if (std::getenv("SURA_JIT_DISABLE_SCALAR")) return false;
        if (is_top_level || native_scratch_regs == 0 ||
            native_scratch_base == 0 || entry_ip >= end_ip) {
            if (std::getenv("SURA_JIT_DIAG")) {
                std::cerr << "[JIT diag] scalar precondition rejected range "
                          << entry_ip << ".." << end_ip
                          << " scratch=" << native_scratch_base << "+"
                          << native_scratch_regs << " top=" << is_top_level << "\n";
            }
            return false;
        }
        plan.scratch_next = static_cast<uint16_t>(
            native_scratch_base + (native_reuse_flag_reg != 65535 ? 1U : 0U));
        std::vector<ScalarValue> regs(native_scratch_base);
        const size_t input_regs = callable_param_count +
                                  (callable_is_method ? 1U : 0U);
        for (size_t i = 0; i < regs.size(); ++i) {
            regs[i] = scalar_real(static_cast<uint16_t>(i),
                                  i < input_regs ? static_cast<int>(i) : -1);
        }

        size_t virtual_ctor_count = 0;
        size_t inlined_method_count = 0;
        ScalarValue result;
        const bool processed = scalar_process_range(
            entry_ip, end_ip, regs, false, plan, result, 0,
            virtual_ctor_count, inlined_method_count);
        if (!processed ||
            inlined_method_count == 0 || virtual_ctor_count == 0 ||
            !result.is_virtual_record || result.fields.empty()) {
            if (std::getenv("SURA_JIT_DIAG")) {
                std::cerr << "[JIT diag] scalar plan rejected range "
                          << entry_ip << ".." << end_ip
                          << " at ip=" << scalar_last_ip
                          << " op=" << static_cast<int>(scalar_last_op)
                          << " processed=" << processed
                          << " methods=" << inlined_method_count
                          << " virtual-ctors=" << virtual_ctor_count
                          << " virtual-result=" << result.is_virtual_record
                          << " scratch-used="
                          << static_cast<unsigned>(plan.scratch_next - native_scratch_base)
                          << "\n";
            }
            return false;
        }

        uint16_t materialized = 0;
        if (!scalar_alloc(plan, 1, materialized)) return false;
        JitInst constructor = result.constructor;
        constructor.a = materialized;
        constructor.c = result.fields.front();
        constructor.ic_scalar_materialize = true;
        if (!callable_is_method && callable_param_count > 0 &&
            native_reuse_flag_reg != 65535 && result.record_class) {
            for (const ScalarGuard& guard : plan.guards) {
                if (guard.reg == 0 && guard.expected_class &&
                    guard.expected_class->name == result.record_class->name) {
                    constructor.ic_scalar_reuse_flag_reg = native_reuse_flag_reg;
                    constructor.ic_scalar_reuse_candidate_reg = 0;
                    has_record_reuse_materializer = true;
                    break;
                }
            }
        }
        if (constructor.operand != static_cast<int>(result.fields.size())) return false;
        for (size_t i = 0; i < result.fields.size(); ++i) {
            if (result.fields[i] !=
                static_cast<uint16_t>(result.fields.front() + i)) {
                return false;
            }
        }
        plan.ops.push_back({constructor, result.constructor_ip});
        plan.result = scalar_real(materialized);
        plan.result_reg = materialized;
        return true;
    }

    void emit_scalar_guard(const ScalarGuard& guard,
                           std::vector<size_t>& slow_jumps) {
        em.mov_r_mem(XR::RAX, XR::RBX, off_r(guard.reg));
        em.mov_ri64(XR::R10, JIT_NBOBJ);
        em.mov_rr(XR::RCX, XR::RAX);
        em.and_rr(XR::RCX, XR::R10);
        em.cmp_rr(XR::RCX, XR::R10);
        slow_jumps.push_back(em.jcc_rel32_placeholder(CC::NE));

        em.mov_ri64(XR::R10, JIT_NBPMASK);
        em.and_rr(XR::RAX, XR::R10);
        em.cmp_mem32_imm8(XR::RAX, INST_OBJTYPE_OFFSET, OBJ_TYPE_INSTANCE);
        slow_jumps.push_back(em.jcc_rel32_placeholder(CC::NE));
        em.mov_r_mem(XR::RCX, XR::RAX, INST_JITINFO_OFFSET);
        em.mov_ri64(XR::R10,
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                        guard.expected_class)));
        em.cmp_rr(XR::RCX, XR::R10);
        slow_jumps.push_back(em.jcc_rel32_placeholder(CC::NE));
    }

    void emit_scalar_numeric_guard(const ScalarNumericGuard& guard,
                                   std::vector<size_t>& slow_jumps) {
        static constexpr uint32_t TAG32 = 0x7ffc0000U;
        if (guard.field_index < 0) {
            slow_jumps.push_back(emit_non_number_jump(off_r(guard.input_reg)));
            return;
        }

        // Class guards run first, so the object layout and field index are
        // valid here. Test the field with Value::is_num()'s exact predicate.
        em.mov_r_mem(XR::RAX, XR::RBX, off_r(guard.input_reg));
        em.mov_ri64(XR::R10, JIT_NBPMASK);
        em.and_rr(XR::RAX, XR::R10);
        em.mov_r_mem(XR::RCX, XR::RAX,
                     INST_FIELDS_OFFSET + VECTOR_DATA_OFFSET);
        em.mov_r32_mem(XR::RAX, XR::RCX, guard.field_index * 8 + 4);
        em.and_r32_imm32(XR::RAX, TAG32);
        em.cmp_r32_imm32(XR::RAX, TAG32);
        slow_jumps.push_back(em.jcc_rel32_placeholder(CC::E));
    }

    int top_level_record_reuse_reg(size_t call_ip, const JitInst& call) const {
        if (!is_top_level || std::getenv("SURA_JIT_DISABLE_SCALAR") ||
            std::getenv("SURA_JIT_DISABLE_REUSE") ||
            call.op != JitOp::CALL_FUNC || call.operand <= 0 ||
            call_ip + 1 >= chunk.code.size()) {
            return -1;
        }
        const JitInst& store = chunk.code[call_ip + 1];
        if (store.op != JitOp::STORE_GLOBAL || store.a != call.a ||
            store.operand < 0) {
            return -1;
        }
        const int global = store.operand;
        const size_t reg_count = std::max<size_t>(chunk.max_regs, 1);
        if (call.c >= reg_count) return -1;

        std::vector<uint8_t> callable_ip(chunk.code.size(), 0);
        auto mark_callable = [&](size_t begin, size_t end) {
            if (begin > end || end > callable_ip.size()) return;
            for (size_t ip = begin; ip < end; ++ip) callable_ip[ip] = 1;
        };
        for (const JitFuncInfo& function : chunk.func_table)
            mark_callable(function.entry_ip, function.end_ip);
        for (const JitClassInfo& cls : chunk.class_table)
            for (const auto& method : cls.methods)
                mark_callable(method.second.entry_ip, method.second.end_ip);

        // A callable that names the candidate global can retain or mutate the
        // old object independently of its positional parameter.
        for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
            if (!callable_ip[ip]) continue;
            const JitInst& ins = chunk.code[ip];
            if ((ins.op == JitOp::LOAD_GLOBAL || ins.op == JitOp::STORE_GLOBAL) &&
                ins.operand == global) return -1;
        }

        std::vector<uint8_t> tainted(reg_count, 0);
        bool candidate_seen = false;
        auto has = [&](uint32_t reg) {
            return reg < tainted.size() && tainted[reg] != 0;
        };
        auto clear = [&](uint16_t reg) {
            if (reg < tainted.size()) tainted[reg] = 0;
        };
        auto reject_window = [&](uint32_t first, int count) {
            if (count < 0) return true;
            for (int i = 0; i < count; ++i)
                if (has(first + static_cast<uint32_t>(i))) return true;
            return false;
        };

        for (size_t ip = 0; ip < chunk.code.size(); ++ip) {
            if (callable_ip[ip]) continue;
            const JitInst& ins = chunk.code[ip];
            switch (ins.op) {
                case JitOp::LOAD_GLOBAL:
                    if (ins.a < tainted.size()) tainted[ins.a] = ins.operand == global;
                    break;
                case JitOp::MOVE:
                    if (ins.a < tainted.size()) tainted[ins.a] = has(ins.b);
                    break;
                case JitOp::DOT_GET:
                    // Reading a field does not expose the instance identity.
                    clear(ins.a);
                    break;
                case JitOp::CALL_FUNC: {
                    for (int i = 0; i < ins.operand; ++i) {
                        const bool is_candidate_arg =
                            ip == call_ip && i == 0 &&
                            static_cast<uint32_t>(ins.c) + i == call.c;
                        if (has(static_cast<uint32_t>(ins.c) + i)) {
                            if (!is_candidate_arg) return -1;
                            candidate_seen = true;
                        }
                    }
                    clear(ins.a);
                    break;
                }
                case JitOp::STORE_GLOBAL:
                    if (has(ins.a) && ins.operand != global) return -1;
                    break;
                case JitOp::METHOD_CALL:
                    if (has(ins.b) || reject_window(
                            static_cast<uint32_t>(ins.b) + 1U, ins.operand)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::CALL_BUILTIN:
                    if (reject_window(ins.b, ins.operand)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::STORE_UPVAL:
                case JitOp::DOT_SET:
                case JitOp::INDEX_SET:
                    if (has(ins.a) || has(ins.b) || has(ins.c)) return -1;
                    break;
                case JitOp::MAKE_ARRAY:
                    if (reject_window(ins.b, ins.operand)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::MAKE_DICT:
                    if (reject_window(ins.b, ins.operand * 2)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::PRINT:
                case JitOp::PRINT_NO_NL:
                    if (reject_window(ins.a, ins.operand)) return -1;
                    break;
                case JitOp::ADD: case JitOp::SUB: case JitOp::MUL:
                case JitOp::DIV: case JitOp::MOD: case JitOp::BIT_AND:
                case JitOp::BIT_OR: case JitOp::BIT_XOR: case JitOp::LSHIFT:
                case JitOp::RSHIFT: case JitOp::CMP_EQ: case JitOp::CMP_NEQ:
                case JitOp::CMP_LT: case JitOp::CMP_LTE: case JitOp::CMP_GT:
                case JitOp::CMP_GTE:
                    if (has(ins.b) || has(ins.c)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::NEG: case JitOp::BIT_NOT: case JitOp::LOGICAL_NOT:
                case JitOp::INDEX_GET:
                    if (has(ins.b) || has(ins.c)) return -1;
                    clear(ins.a);
                    break;
                case JitOp::LOAD_CONST: case JitOp::LOAD_NIL:
                case JitOp::LOAD_BOOL: case JitOp::MAKE_LAMBDA:
                case JitOp::NEW_INSTANCE: case JitOp::DICT_KEYS:
                    clear(ins.a);
                    break;
                default:
                    break;
            }
        }
        return candidate_seen ? static_cast<int>(call.c) : -1;
    }

    bool unique_top_level_persistent_frame_site(size_t call_ip,
                                                const JitInst& call) const {
        if (!is_top_level || std::getenv("SURA_JIT_DISABLE_PERSISTENT_FRAME") ||
            top_level_record_reuse_reg(call_ip, call) < 0) {
            return false;
        }
        size_t eligible = 0;
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            if (chunk.code[ip].op == JitOp::CALL_FUNC &&
                top_level_record_reuse_reg(ip, chunk.code[ip]) >= 0) {
                if (++eligible > 1) return false;
            }
        }
        return eligible == 1;
    }

    void compute_definitely_initialized_globals() {
        definitely_initialized_globals.clear();
        if (!is_top_level || chunk.global_names.empty() || entry_ip >= end_ip) return;

        const size_t global_count = chunk.global_names.size();
        const size_t code_count = chunk.code.size();
        definitely_initialized_globals.assign(code_count + 1, std::vector<uint8_t>(global_count, 0));
        std::vector<std::vector<uint8_t>> in_states(code_count + 1, std::vector<uint8_t>(global_count, 0));
        std::vector<uint8_t> seen(code_count + 1, 0);
        std::vector<size_t> worklist;

        auto merge_into = [&](size_t target, const std::vector<uint8_t>& state) {
            if (target < entry_ip || target > end_ip) return;
            if (!seen[target]) {
                in_states[target] = state;
                seen[target] = 1;
                worklist.push_back(target);
                return;
            }

            bool changed = false;
            for (size_t i = 0; i < global_count; ++i) {
                uint8_t merged = (uint8_t)(in_states[target][i] & state[i]);
                if (merged != in_states[target][i]) {
                    in_states[target][i] = merged;
                    changed = true;
                }
            }
            if (changed) worklist.push_back(target);
        };

        merge_into(entry_ip, std::vector<uint8_t>(global_count, 0));
        for (size_t cursor = 0; cursor < worklist.size(); ++cursor) {
            size_t ip = worklist[cursor];
            if (ip >= end_ip || ip >= code_count) continue;

            const JitInst& inst = chunk.code[ip];
            definitely_initialized_globals[ip] = in_states[ip];
            std::vector<uint8_t> out = in_states[ip];

            if (inst.op == JitOp::STORE_GLOBAL &&
                inst.operand >= 0 && (size_t)inst.operand < global_count) {
                out[inst.operand] = 1;
            } else if (inst.op == JitOp::DEF_CLASS &&
                       inst.str_idx >= 0 && (size_t)inst.str_idx < global_count) {
                out[inst.str_idx] = 1;
            }

            switch (inst.op) {
                case JitOp::JUMP:
                    merge_into((size_t)inst.operand, out);
                    break;
                case JitOp::JUMP_IF_FALSE:
                case JitOp::JUMP_IF_TRUE:
                    merge_into(ip + 1, out);
                    merge_into((size_t)inst.operand, out);
                    break;
                case JitOp::RETURN_VAL:
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    break;
                default:
                    merge_into(ip + 1, out);
                    break;
            }
        }
    }

    bool global_definitely_initialized(size_t ip, int idx) const {
        if (!is_top_level || idx < 0) return false;
        if (ip >= definitely_initialized_globals.size()) return false;
        const auto& state = definitely_initialized_globals[ip];
        return (size_t)idx < state.size() && state[idx] != 0;
    }

    void emit_epilogue_with_reg(int reg_a) {
        // mov rax, R[a]; pop r13; pop r12; pop rbx; ret
        em.mov_r_mem(XR::RAX, XR::RBX, off_r((uint16_t)reg_a));
        emit_epilogue_with_rax();
    }
    void emit_epilogue_with_rax() {
        emit_frame_teardown();
        em.pop_r(XR::R13);
        em.pop_r(XR::R12);
        em.pop_r(XR::RBX);
        em.ret();
    }
    void emit_epilogue_nil() {
        em.mov_ri64(XR::RAX, JIT_NBNIL);
        emit_frame_teardown();
        em.pop_r(XR::R13);
        em.pop_r(XR::R12);
        em.pop_r(XR::RBX);
        em.ret();
    }

    // For JUMP_IF_FALSE / JUMP_IF_TRUE we need to know the condition
    // register holds a bool (NBTRUE/NBFALSE). We enforce that by refusing
    // to compile unless the previous op is a CMP_* writing the same reg.
    bool prev_is_cmp_to(size_t ip, uint16_t reg) const {
        if (ip == 0 || ip - 1 < entry_ip) return false;
        const JitInst& p = chunk.code[ip - 1];
        bool is_cmp = (p.op == JitOp::CMP_EQ || p.op == JitOp::CMP_NEQ ||
                       p.op == JitOp::CMP_LT || p.op == JitOp::CMP_LTE ||
                       p.op == JitOp::CMP_GT || p.op == JitOp::CMP_GTE);
        return is_cmp && p.a == reg;
    }

    // A lexical CMP immediately before a conditional only proves the value is
    // boolean when control cannot branch directly to the conditional.
    bool has_non_fallthrough_predecessor(size_t target_ip) const {
        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            const JitInst& candidate = chunk.code[ip];
            if ((candidate.op == JitOp::JUMP ||
                 candidate.op == JitOp::JUMP_IF_FALSE ||
                 candidate.op == JitOp::JUMP_IF_TRUE) &&
                candidate.operand >= 0 &&
                (size_t)candidate.operand == target_ip) {
                return true;
            }
        }
        return false;
    }

    // Value::is_num() is false exactly when the upper 32-bit word contains
    // the complete 0x7ffc NaN-box tag. Testing that word avoids materializing
    // a 64-bit mask and shortens every guarded numeric operation.
    // A guard exists to catch a non-number reaching an arithmetic operation.
    // When the whole-body dataflow already proves both operands numeric at
    // this point, the guard can only ever fall through, so emitting it costs
    // two loads, two compares and two branches per operation for nothing.
    //
    // Only valid on the general body: the scalar-plan path rewrites register
    // numbers, so its instructions do not line up with the analysed stream.
    bool operands_proven_numeric(const JitInst& inst, size_t ip,
                                 const JitInst* runtime_inst) const {
        if (runtime_inst != nullptr || used_scalar_plan) return false;
        if (!numeric_proof) return false;
        return numeric_proof->proven_num(ip, inst.b) &&
               numeric_proof->proven_num(ip, inst.c);
    }

    size_t emit_non_number_jump(int32_t value_offset) {
        static constexpr uint32_t TAG32 = 0x7ffc0000U;
        em.mov_r32_mem(XR::RAX, XR::RBX, value_offset + 4);
        em.and_r32_imm32(XR::RAX, TAG32);
        em.cmp_r32_imm32(XR::RAX, TAG32);
        return em.jcc_rel32_placeholder(CC::E);
    }

    // Emits the guards shared by the INDEX_GET / INDEX_SET array fast paths.
    // On exit RCX holds &elements[idx] and every failing guard has been
    // recorded in slow_jmps for the caller to patch to its helper call.
    //   1. R[container] is an object            (bits & NBOBJ) == NBOBJ
    //   2. obj_type == ARRAY
    //   3. R[key] is a number                   (hi32 & 0x7ffc0000) != 0x7ffc0000
    //   4. idx = (int64)trunc(key), 0 <= idx <= INT32_MAX  (one unsigned compare)
    //   5. &elements[idx] < _M_finish            (bounds)
    // Clobbers RAX, RCX, RDX, R10.
    // Guards that R[container] is an array; leaves RAX = GCArray*.
    // Clobbers RAX, RCX, R10.
    void emit_array_receiver_guard(X64Emitter& em, int32_t container_off,
                                   std::vector<size_t>& slow_jmps) {
        sura_x64_emit_object_receiver_guard(em, container_off, OBJ_TYPE_ARRAY, slow_jmps);
    }

    void emit_object_receiver_guard(X64Emitter& em, int32_t container_off,
                                    int8_t obj_tag, std::vector<size_t>& slow_jmps) {
        sura_x64_emit_object_receiver_guard(em, container_off, obj_tag, slow_jmps);
    }

    void emit_array_index_guard(X64Emitter& em, int32_t container_off,
                                int32_t key_off, std::vector<size_t>& slow_jmps) {
        sura_x64_emit_array_index_guard(em, container_off, key_off, slow_jmps);
    }

    // ── Loop register cache: planning ─────────────────────────────────
    // The copies live in XMM6..XMM15, which Win64 makes callee-saved: the
    // prologue saves the ones in use and the unwind record describes the
    // saves, so helpers may keep throwing through this frame.
    void plan_loop_caches() {
        loop_caches.clear();
        xmm_saved_count = 0;
        frame_bytes = 48;
        if (is_top_level) return;
        const uint32_t reg_limit = native_scratch_regs ? native_scratch_base : native_frame_regs;
        loop_caches = jit_plan_loop_caches(chunk, entry_ip, end_ip, nullptr, reg_limit,
                                           native_reuse_flag_reg, kLoopCacheXmmFirst,
                                           kLoopCacheXmmCount, "full-tier");
        for (const LoopCache& cache : loop_caches)
            xmm_saved_count = std::max<int>(xmm_saved_count, static_cast<int>(cache.regs.size()));
        frame_bytes = 48 + 16U * static_cast<uint32_t>(xmm_saved_count);
    }

    void emit_frame_teardown() {
        for (int k = 0; k < xmm_saved_count; ++k)
            em.movdqu_x_mem(XR::XMM6 + k, XR::RSP, 48 + 16 * k);
        if (frame_bytes <= 127) em.add_rsp_imm8(static_cast<int8_t>(frame_bytes));
        else em.add_rsp_imm32(static_cast<int32_t>(frame_bytes));
    }

    // ── Loop register cache: emission ─────────────────────────────────
    int cached_xmm(uint16_t r) const {
        if (!active_cache || r >= active_cache->xmm_of.size()) return -1;
        return active_cache->xmm_of[r];
    }
    bool proven_num_at(size_t ip, uint16_t r) const {
        return !used_scalar_plan && numeric_proof && numeric_proof->proven_num(ip, r);
    }
    // xmm0 = R[r], from the XMM copy when there is one.
    void load_operand_xmm0(uint16_t r) {
        const int x = cached_xmm(r);
        if (x >= 0) em.movaps_xx(XR::XMM0, x);
        else em.movsd_x_mem(XR::XMM0, XR::RBX, off_r(r));
    }
    void arith_xmm0(JitOp op, uint16_t r) {
        const int x = cached_xmm(r);
        switch (op) {
            case JitOp::ADD: if (x >= 0) em.addsd_xx(XR::XMM0, x); else em.addsd_x_mem(XR::XMM0, XR::RBX, off_r(r)); break;
            case JitOp::SUB: if (x >= 0) em.subsd_xx(XR::XMM0, x); else em.subsd_x_mem(XR::XMM0, XR::RBX, off_r(r)); break;
            default:         if (x >= 0) em.mulsd_xx(XR::XMM0, x); else em.mulsd_x_mem(XR::XMM0, XR::RBX, off_r(r)); break;
        }
    }
    void ucomisd_xmm0(uint16_t r) {
        const int x = cached_xmm(r);
        if (x >= 0) em.ucomisd_xx(XR::XMM0, x);
        else em.ucomisd_x_mem(XR::XMM0, XR::RBX, off_r(r));
    }
    void load_bits(int gpr, uint16_t r) {
        const int x = cached_xmm(r);
        if (x >= 0) em.movq_r_x(gpr, x);
        else em.mov_r_mem(gpr, XR::RBX, off_r(r));
    }
    // R[a] = xmm0 / rax: the slot and, when cached, the XMM copy.
    void store_result_xmm0(uint16_t a) {
        em.movsd_mem_x(XR::RBX, off_r(a), XR::XMM0);
        const int x = cached_xmm(a);
        if (x >= 0) em.movaps_xx(x, XR::XMM0);
    }
    void store_result_rax(uint16_t a) {
        em.mov_mem_r(XR::RBX, off_r(a), XR::RAX);
        const int x = cached_xmm(a);
        if (x >= 0) em.movq_x_r(x, XR::RAX);
    }
    void reload_cached(uint16_t r) {
        const int x = cached_xmm(r);
        if (x >= 0) em.movsd_x_mem(x, XR::RBX, off_r(r));
    }
    void reload_all_cached() {
        if (!active_cache) return;
        for (uint16_t r : active_cache->regs) reload_cached(r);
    }
    bool touches_cache(const JitInst& inst) const {
        return cached_xmm(inst.a) >= 0 || cached_xmm(inst.b) >= 0 || cached_xmm(inst.c) >= 0;
    }

    // After an operation emitted by the general path, bring the XMM copies
    // back in line with what it wrote. Calls may run arbitrary code, so they
    // refresh every copy; the cheap operations write only R[a].
    void refresh_from_rax(uint16_t r) {
        const int x = cached_xmm(r);
        if (x >= 0) em.movq_x_r(x, XR::RAX);
    }
    void reload_after(const JitInst& inst) {
        using O = JitOp;
        switch (inst.op) {
            case O::NOP: case O::JUMP: case O::JUMP_IF_FALSE: case O::JUMP_IF_TRUE:
            case O::INDEX_SET: case O::DOT_SET: case O::STORE_GLOBAL: case O::STORE_UPVAL:
            case O::PRINT: case O::PRINT_NO_NL:
            case O::RETURN_VAL: case O::RETURN_NONE: case O::HALT:
                return;
            // Every path of these ends with `mov [R[a]], rax`, so the copy
            // comes straight from RAX instead of waiting on the store.
            case O::LOAD_GLOBAL: case O::DIV: case O::MOD:
            case O::BIT_AND: case O::BIT_OR: case O::BIT_XOR:
            case O::CMP_EQ: case O::CMP_NEQ: case O::NEG: case O::LOGICAL_NOT:
            case O::MAKE_ARRAY: case O::MAKE_DICT: case O::INDEX_GET: case O::CALL_FUNC:
            case O::DOT_GET:
                refresh_from_rax(inst.a);
                return;
            case O::LOAD_CONST: case O::LOAD_NIL: case O::LOAD_BOOL: case O::MOVE:
            case O::LOAD_UPVAL:
            case O::ADD: case O::SUB: case O::MUL:
            case O::LSHIFT: case O::RSHIFT: case O::BIT_NOT:
            case O::CMP_LT: case O::CMP_LTE: case O::CMP_GT: case O::CMP_GTE:
            case O::OP_IN: case O::DICT_KEYS: case O::METHOD_CALL:
                reload_cached(inst.a);
                return;
            default:
                reload_all_cached();
                return;
        }
    }

    // Operations the cache emits itself. Returns false to hand the
    // instruction to the general path (followed by reload_after).
    bool emit_cached_op(const JitInst& inst, size_t ip) {
        using O = JitOp;
        const int32_t oa = off_r(inst.a);
        const int32_t ob = off_r(inst.b);
        const int32_t oc = off_r(inst.c);
        switch (inst.op) {
            case O::LOAD_CONST: {
                const int x = cached_xmm(inst.a);
                if (x < 0) return false;
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.constants.size()) return false;
                em.movsd_x_mem(x, XR::R12, off_c(inst.operand));
                em.movsd_mem_x(XR::RBX, oa, x);
                return true;
            }
            case O::LOAD_NIL: case O::LOAD_BOOL: {
                if (cached_xmm(inst.a) < 0) return false;
                em.mov_ri64(XR::RAX, inst.op == O::LOAD_NIL ? JIT_NBNIL
                                     : (inst.operand ? JIT_NBTRUE : JIT_NBFALSE));
                store_result_rax(inst.a);
                return true;
            }
            case O::MOVE: {
                const int xa = cached_xmm(inst.a);
                const int xb = cached_xmm(inst.b);
                if (xa < 0 && xb < 0) return false;
                if (xb >= 0 && xa >= 0) {
                    em.movsd_mem_x(XR::RBX, oa, xb);
                    if (xa != xb) em.movaps_xx(xa, xb);
                } else if (xb >= 0) {
                    em.mov_r_mem(XR::RAX, XR::RBX, ob);
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                } else {
                    em.movsd_x_mem(xa, XR::RBX, ob);
                    em.movsd_mem_x(XR::RBX, oa, xa);
                }
                return true;
            }
            case O::ADD: case O::SUB: case O::MUL: {
                if (!touches_cache(inst)) return false;
                const bool fast = inst.ic_numeric_fast ||
                                  (proven_num_at(ip, inst.b) && proven_num_at(ip, inst.c));
                std::vector<size_t> slow_jmps;
                if (!fast) {
                    if (!proven_num_at(ip, inst.b)) slow_jmps.push_back(emit_non_number_jump(ob));
                    if (!proven_num_at(ip, inst.c)) slow_jmps.push_back(emit_non_number_jump(oc));
                }
                load_operand_xmm0(inst.b);
                arith_xmm0(inst.op, inst.c);
                store_result_xmm0(inst.a);
                if (slow_jmps.empty()) return true;
                const size_t done_jmp = em.jmp_rel32_placeholder();
                const size_t slow_pos = em.pos();
                for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                if (inst.op == O::ADD) {
                    em.mov_rr(XR::R8, XR::R13);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_add);
                } else {
                    em.mov_ri64(XR::R8, inst.op == O::SUB ? JIT_ARITH_SUB : JIT_ARITH_MUL);
                    em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
                }
                em.call_rax();
                store_result_rax(inst.a);
                em.patch_rel32(done_jmp, em.pos());
                return true;
            }
            case O::CMP_LT: case O::CMP_LTE: case O::CMP_GT: case O::CMP_GTE:
                return emit_cmp_ord(inst, ip);
            case O::CMP_EQ: case O::CMP_NEQ:
                return emit_cmp_eq(inst, ip);
            default:
                return false;
        }
    }

    // CMP_LT / LTE / GT / GTE: proven or tag-checked numbers compare inline;
    // b < c is evaluated as c > b so that an unordered compare (NaN) yields
    // false, the way Value::lt does. Anything else keeps the checked helper
    // (the ordering type error). Works with or without an active loop cache.
    bool emit_cmp_ord(const JitInst& inst, size_t ip) {
        using O = JitOp;
        const int32_t ob = off_r(inst.b);
        const int32_t oc = off_r(inst.c);
        std::vector<size_t> slow_jmps;
        if (!proven_num_at(ip, inst.b)) slow_jmps.push_back(emit_non_number_jump(ob));
        if (!proven_num_at(ip, inst.c)) slow_jmps.push_back(emit_non_number_jump(oc));
        const bool swapped = inst.op == O::CMP_LT || inst.op == O::CMP_LTE;
        load_operand_xmm0(swapped ? inst.c : inst.b);
        ucomisd_xmm0(swapped ? inst.b : inst.c);
        const uint8_t cc = (inst.op == O::CMP_LT || inst.op == O::CMP_GT) ? CC::A : CC::AE;
        em.mov_ri64(XR::RAX, JIT_NBFALSE);
        em.mov_ri64(XR::RCX, JIT_NBTRUE);
        em.cmov_rr(cc, XR::RAX, XR::RCX);
        store_result_rax(inst.a);
        if (slow_jmps.empty()) return true;
        const size_t done_jmp = em.jmp_rel32_placeholder();
        const size_t slow_pos = em.pos();
        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
        int helper_op = JIT_CMP_LT;
        switch (inst.op) {
            case O::CMP_LTE: helper_op = JIT_CMP_LTE; break;
            case O::CMP_GT:  helper_op = JIT_CMP_GT; break;
            case O::CMP_GTE: helper_op = JIT_CMP_GTE; break;
            default: break;
        }
        em.mov_r_mem(XR::RCX, XR::RBX, ob);
        em.mov_r_mem(XR::RDX, XR::RBX, oc);
        em.mov_ri64(XR::R8, (uint64_t)helper_op);
        em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
        em.call_rax();
        store_result_rax(inst.a);
        em.patch_rel32(done_jmp, em.pos());
        return true;
    }

    // CMP_EQ / CMP_NEQ: two numbers compare inline (Value::eq: identical
    // bits, or an ordered equal compare, so +0 == -0 and a NaN equals only
    // itself); operands not proven numeric are tag-checked first and
    // anything else (strings, objects, nil) keeps the helper. Works with or
    // without an active loop cache.
    bool emit_cmp_eq(const JitInst& inst, size_t ip) {
        const bool neq = inst.op == JitOp::CMP_NEQ;
        const uint64_t when_equal = neq ? JIT_NBFALSE : JIT_NBTRUE;
        const uint64_t when_differ = neq ? JIT_NBTRUE : JIT_NBFALSE;
        std::vector<size_t> slow_jmps;
        if (!proven_num_at(ip, inst.b)) slow_jmps.push_back(emit_non_number_jump(off_r(inst.b)));
        if (!proven_num_at(ip, inst.c)) slow_jmps.push_back(emit_non_number_jump(off_r(inst.c)));
        load_operand_xmm0(inst.b);
        ucomisd_xmm0(inst.c);
        em.mov_ri64(XR::RAX, when_differ);
        em.mov_ri64(XR::RCX, when_equal);
        em.cmov_rr(CC::E, XR::RAX, XR::RCX);
        em.mov_ri64(XR::RDX, when_differ);
        em.cmov_rr(CC::P, XR::RAX, XR::RDX);
        load_bits(XR::RDX, inst.b);
        load_bits(XR::R8, inst.c);
        em.cmp_rr(XR::RDX, XR::R8);
        em.cmov_rr(CC::E, XR::RAX, XR::RCX);
        store_result_rax(inst.a);
        if (slow_jmps.empty()) return true;
        const size_t done_jmp = em.jmp_rel32_placeholder();
        const size_t slow_pos = em.pos();
        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
        em.mov_r_mem(XR::RCX, XR::RBX, off_r(inst.b));
        em.mov_r_mem(XR::RDX, XR::RBX, off_r(inst.c));
        em.mov_ri64(XR::R8, neq ? 1 : 0);
        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_eq);
        em.call_rax();
        store_result_rax(inst.a);
        em.patch_rel32(done_jmp, em.pos());
        return true;
    }

    bool emit_op(const JitInst& inst, size_t ip,
                 const JitInst* runtime_inst = nullptr) {
        if (active_cache != nullptr && runtime_inst == nullptr) {
            if (emit_cached_op(inst, ip)) return true;
            if (!emit_op_impl(inst, ip, runtime_inst)) return false;
            reload_after(inst);
            return true;
        }
        return emit_op_impl(inst, ip, runtime_inst);
    }

    bool emit_op_impl(const JitInst& inst, size_t ip,
                      const JitInst* runtime_inst) {
        using O = JitOp;
        int32_t oa = off_r(inst.a);
        int32_t ob = off_r(inst.b);
        int32_t oc = off_r(inst.c);

        switch (inst.op) {
            case O::NOP:
                return true;

            case O::LOAD_CONST: {
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.constants.size()) return false;
                // Only allow numeric constants — anything else (strings, nil) is
                // legal but then numeric arith following may or may not be safe;
                // to stay conservative we still accept any constant here, because
                // the VM's Value representation is identical.
                em.mov_r_mem(XR::RAX, XR::R12, off_c(inst.operand));
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }
            case O::LOAD_NIL:
                em.mov_ri64(XR::RAX, JIT_NBNIL);
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            case O::LOAD_BOOL:
                em.mov_ri64(XR::RAX, inst.operand ? JIT_NBTRUE : JIT_NBFALSE);
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            case O::MOVE:
                em.mov_r_mem(XR::RAX, XR::RBX, ob);
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;

            // Guarded helper, same shape as MOD: division by zero must raise
            // [E202] instead of yielding an infinity, so the inline SSE path
            // below is not usable here.
            case O::DIV: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_div);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            case O::ADD: case O::SUB: case O::MUL: {
                if (inst.ic_numeric_fast ||
                    operands_proven_numeric(inst, ip, runtime_inst)) {
                    em.movsd_x_mem(XR::XMM0, XR::RBX, ob);
                    if (inst.op == O::ADD) em.addsd_x_mem(XR::XMM0, XR::RBX, oc);
                    else if (inst.op == O::SUB) em.subsd_x_mem(XR::XMM0, XR::RBX, oc);
                    else em.mulsd_x_mem(XR::XMM0, XR::RBX, oc);
                    em.movsd_mem_x(XR::RBX, oa, XR::XMM0);
                    return true;
                }
                // ── Phase 10: in top-level main, ADD must handle string concat ──
                // The guarded slow path preserves string/array concat and the
                // numeric error contract for dynamic operands.
                if (inst.op == O::ADD) {
                    // Guarded numeric fast path. This uses the same NaN-box
                    // predicate as Value::is_num(); strings and all other
                    // values retain the exact helper semantics below.
                    std::vector<size_t> slow_jmps;
                    slow_jmps.push_back(emit_non_number_jump(ob));
                    slow_jmps.push_back(emit_non_number_jump(oc));

                    em.movsd_x_mem(XR::XMM0, XR::RBX, ob);
                    em.addsd_x_mem(XR::XMM0, XR::RBX, oc);
                    em.movsd_mem_x(XR::RBX, oa, XR::XMM0);
                    size_t done_jmp = em.jmp_rel32_placeholder();

                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                    em.mov_r_mem(XR::RCX, XR::RBX, ob);
                    em.mov_r_mem(XR::RDX, XR::RBX, oc);
                    em.mov_rr(XR::R8, XR::R13);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_add);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    em.patch_rel32(done_jmp, em.pos());
                    return true;
                }
                if (inst.op == O::SUB || inst.op == O::MUL) {
                    std::vector<size_t> slow_jmps;
                    slow_jmps.push_back(emit_non_number_jump(ob));
                    slow_jmps.push_back(emit_non_number_jump(oc));

                    em.movsd_x_mem(XR::XMM0, XR::RBX, ob);
                    if (inst.op == O::SUB) em.subsd_x_mem(XR::XMM0, XR::RBX, oc);
                    else                   em.mulsd_x_mem(XR::XMM0, XR::RBX, oc);
                    em.movsd_mem_x(XR::RBX, oa, XR::XMM0);
                    size_t done_jmp = em.jmp_rel32_placeholder();

                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                    em.mov_r_mem(XR::RCX, XR::RBX, ob);
                    em.mov_r_mem(XR::RDX, XR::RBX, oc);
                    em.mov_ri64(XR::R8, inst.op == O::SUB ? JIT_ARITH_SUB : JIT_ARITH_MUL);
                    em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    em.patch_rel32(done_jmp, em.pos());
                    return true;
                }
                // xmm0 = R[b];  xmm0 <op>= R[c];  R[a] = xmm0
                em.movsd_x_mem(XR::XMM0, XR::RBX, ob);
                switch (inst.op) {
                    case O::ADD: em.addsd_x_mem(XR::XMM0, XR::RBX, oc); break;
                    case O::SUB: em.subsd_x_mem(XR::XMM0, XR::RBX, oc); break;
                    case O::MUL: em.mulsd_x_mem(XR::XMM0, XR::RBX, oc); break;
                    case O::DIV: em.divsd_x_mem(XR::XMM0, XR::RBX, oc); break;
                    default: return false;
                }
                em.movsd_mem_x(XR::RBX, oa, XR::XMM0);
                return true;
            }

            case O::CMP_LT: case O::CMP_LTE: case O::CMP_GT: case O::CMP_GTE:
                return emit_cmp_ord(inst, ip);

            // ── Phase 9: equality comparisons (bit-equal fast path) ──
            // Matches interpreter for: nil, bool, identical numbers, same pointer.
            // Differs for: strings with same content but different GCString*.
            // (The existing arithmetic ops similarly skip type checks for speed.)
            case O::CMP_EQ: case O::CMP_NEQ:
                return emit_cmp_eq(inst, ip);

            // ── Phase 9: NEG (flip sign bit) ─────────────────────────
            // Numeric values flip the sign bit after a NaN-box type guard;
            // non-numbers take the checked error path.
            case O::NEG: {
                size_t slow_jmp = emit_non_number_jump(ob);
                em.mov_r_mem(XR::RAX, XR::RBX, ob);
                em.mov_ri64(XR::RCX, 0x8000000000000000ULL);
                em.xor_rr(XR::RAX, XR::RCX);
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                size_t done_jmp = em.jmp_rel32_placeholder();

                em.patch_rel32(slow_jmp, em.pos());
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_ri64(XR::RDX, 0);
                em.mov_ri64(XR::R8, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_unary);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                em.patch_rel32(done_jmp, em.pos());
                return true;
            }

            // ── Phase 9: LOGICAL_NOT via sura_jit_truthy ────────────
            case O::LOGICAL_NOT: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_truthy);
                em.call_rax();
                em.emit8(0x85); em.emit8(0xC0);     // test eax, eax
                em.mov_ri64(XR::RAX, JIT_NBTRUE);
                em.mov_ri64(XR::RCX, JIT_NBFALSE);
                em.cmov_rr(CC::NE, XR::RAX, XR::RCX); // truthy → NBFALSE
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            // ── Phase 9: MOD via libm fmod() ─────────────────────────
            case O::MOD: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_mod);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            // ── Phase 9: BIT_AND / BIT_OR / BIT_XOR ──────────────────
            // Truncating cast double→int64, op, cast back.
            case O::BIT_AND: case O::BIT_OR: case O::BIT_XOR: {
                int helper_op = JIT_BIT_AND;
                switch (inst.op) {
                    case O::BIT_AND: helper_op = JIT_BIT_AND; break;
                    case O::BIT_OR:  helper_op = JIT_BIT_OR; break;
                    case O::BIT_XOR: helper_op = JIT_BIT_XOR; break;
                    default: return false;
                }
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, (uint64_t)helper_op);
                em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            // ── Phase 9: LSHIFT / RSHIFT ─────────────────────────────
            // RSHIFT uses SAR (signed; matches interpreter's `(long long)>>`).
            case O::LSHIFT: case O::RSHIFT: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, inst.op == O::LSHIFT ? JIT_LSHIFT : JIT_RSHIFT);
                em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            // ── Phase 9: BIT_NOT ─────────────────────────────────────
            case O::BIT_NOT: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_ri64(XR::RDX, JIT_BIT_NOT);
                em.mov_ri64(XR::R8, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_unary);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            case O::JUMP: {
                size_t p = em.jmp_rel32_placeholder();
                pending.push_back({p, (size_t)inst.operand});
                return true;
            }

            case O::JUMP_IF_FALSE: {
                if (prev_is_cmp_to(ip, inst.a) &&
                    !has_non_fallthrough_predecessor(ip)) {
                    em.mov_r_mem(XR::RAX, XR::RBX, oa);
                    em.mov_ri64(XR::RCX, JIT_NBFALSE);
                    em.cmp_rr(XR::RAX, XR::RCX);
                    size_t p = em.jcc_rel32_placeholder(CC::E);
                    pending.push_back({p, (size_t)inst.operand});
                    return true;
                }
                // Full truthiness via C helper — supports any Value type
                // (numbers, bools, nil, strings, arrays, dicts, closures, instances).
                //   sub rsp, 40       ; shadow + align
                //   mov rcx, [rbx+oa] ; arg1 = bits
                //   mov rax, <sura_jit_truthy>
                //   call rax
                //   add rsp, 40
                //   test eax, eax
                //   je <target>       ; if 0 → jump
                em.mov_r_mem(XR::RCX, XR::RBX, oa);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_truthy);
                em.call_rax();
                // test eax, eax — encoded as 85 C0
                em.emit8(0x85); em.emit8(0xC0);
                size_t p = em.jcc_rel32_placeholder(CC::E);
                pending.push_back({p, (size_t)inst.operand});
                return true;
            }
            case O::JUMP_IF_TRUE: {
                if (prev_is_cmp_to(ip, inst.a) &&
                    !has_non_fallthrough_predecessor(ip)) {
                    em.mov_r_mem(XR::RAX, XR::RBX, oa);
                    em.mov_ri64(XR::RCX, JIT_NBTRUE);
                    em.cmp_rr(XR::RAX, XR::RCX);
                    size_t p = em.jcc_rel32_placeholder(CC::E);
                    pending.push_back({p, (size_t)inst.operand});
                    return true;
                }
                em.mov_r_mem(XR::RCX, XR::RBX, oa);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_truthy);
                em.call_rax();
                em.emit8(0x85); em.emit8(0xC0); // test eax, eax
                size_t p = em.jcc_rel32_placeholder(CC::NE);
                pending.push_back({p, (size_t)inst.operand});
                return true;
            }

            case O::MAKE_ARRAY:
            case O::MAKE_DICT: {
                if (std::getenv("SURA_JIT_DISABLE_COLLECTIONS")) return false;
                // Collection construction allocates through the VM so the
                // process-wide GC can collect before allocation while this
                // native frame's complete register window remains published.
                // The helper mirrors the interpreter's contiguous register
                // layout exactly: array values start at b; dictionary key and
                // value pairs start at b and consume two registers per entry.
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);  // vm
                em.mov_rr(XR::RDX, XR::RBX);  // R
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(
                    XR::RAX,
                    (uint64_t)(uintptr_t)(inst.op == O::MAKE_ARRAY
                        ? &sura_jit_make_array
                        : &sura_jit_make_dict));
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            case O::CALL_FUNC: {
                // Call sura_jit_call(vm, R, &inst)
                // Win64: RCX=vm, RDX=R, R8=inst_ptr. Reserve 32 bytes shadow
                // space + 8 padding = 40 → round up to 48 to keep 16-aligned.
                // After sub rsp,48  stack is 48 mod 16 → 0 (already 16-aligned
                // from prologue). Actually 48 is 16-aligned. Good.
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                if (inst.ic_scalar_materialize) {
                    std::vector<size_t> allocate_jumps;
                    size_t done_jump = SIZE_MAX;
                    if (inst.ic_scalar_reuse_flag_reg != 65535 &&
                        inst.ic_scalar_reuse_candidate_reg != 65535 &&
                        (inst.operand == 2 || inst.operand == 3)) {
                        em.mov_r_mem(XR::RAX, XR::RBX,
                                     off_r(inst.ic_scalar_reuse_flag_reg));
                        em.mov_ri64(XR::RCX, JIT_NBTRUE);
                        em.cmp_rr(XR::RAX, XR::RCX);
                        allocate_jumps.push_back(em.jcc_rel32_placeholder(CC::NE));

                        em.mov_r_mem(XR::RAX, XR::RBX,
                                     off_r(inst.ic_scalar_reuse_candidate_reg));
                        em.mov_rr(XR::R10, XR::RAX);
                        em.mov_ri64(XR::RCX, JIT_NBPMASK);
                        em.and_rr(XR::R10, XR::RCX);
                        em.mov_r_mem(XR::RDX, XR::R10,
                                     INST_FIELDS_OFFSET + SMALL_VALUE_VEC_SIZE_OFFSET);
                        em.cmp_r_imm32(XR::RDX, inst.operand);
                        allocate_jumps.push_back(em.jcc_rel32_placeholder(CC::NE));
                        em.mov_r_mem(XR::RCX, XR::R10,
                                     INST_FIELDS_OFFSET + VECTOR_DATA_OFFSET);
                        for (int field = 0; field < inst.operand; ++field) {
                            em.mov_r_mem(XR::RDX, XR::RBX,
                                         off_r(static_cast<uint16_t>(inst.c + field)));
                            em.mov_mem_r(XR::RCX, field * 8, XR::RDX);
                        }
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        done_jump = em.jmp_rel32_placeholder();
                        for (size_t jump : allocate_jumps)
                            em.patch_rel32(jump, em.pos());
                    }
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX,
                                (uint64_t)(uintptr_t)&sura_jit_materialize_scalar_record);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    if (done_jump != SIZE_MAX) em.patch_rel32(done_jump, em.pos());
                    return true;
                }
                if (inst.ic_class != nullptr && inst.ic_method != nullptr &&
                    inst.ic_native_fn == nullptr &&
                    (inst.operand == 2 || inst.operand == 3) &&
                    inst.ic_method->params.size() == (size_t)inst.operand &&
                    inst.ic_class->field_defaults.size() >= (size_t)inst.operand) {
                    bool plain_prefix_ctor = true;
                    for (int arg_i = 0; arg_i < inst.operand; ++arg_i) {
                        const auto fit = inst.ic_class->field_indices.find(inst.ic_method->params[(size_t)arg_i]);
                        if (fit == inst.ic_class->field_indices.end() || fit->second != arg_i) {
                            plain_prefix_ctor = false;
                            break;
                        }
                    }
                    if (plain_prefix_ctor) {
                        const bool exact_width =
                            inst.ic_class->field_defaults.size() == (size_t)inst.operand;
                        if (exact_width) {
                            em.mov_rr(XR::RCX, XR::R13);
                            em.mov_ri64(XR::RDX, (uint64_t)(uintptr_t)inst.ic_class);
                            em.mov_r_mem(XR::R8, XR::RBX, off_r(inst.c));
                            em.mov_r_mem(XR::R9, XR::RBX,
                                         off_r((uint16_t)(inst.c + 1)));
                            if (inst.operand == 3) {
                                em.mov_r_mem(XR::R10, XR::RBX,
                                             off_r((uint16_t)(inst.c + 2)));
                                em.mov_rsp_disp8_r(32, XR::R10);
                                em.mov_ri64(XR::RAX,
                                    (uint64_t)(uintptr_t)&sura_jit_construct_exact3);
                            } else {
                                em.mov_ri64(XR::RAX,
                                    (uint64_t)(uintptr_t)&sura_jit_construct_exact2);
                            }
                            em.call_rax();
                            em.mov_mem_r(XR::RBX, oa, XR::RAX);
                            return true;
                        }

                        em.mov_rr(XR::RCX, XR::R13);
                        em.mov_ri64(XR::RDX, (uint64_t)(uintptr_t)inst.ic_class);
                        em.mov_r_mem(XR::R8, XR::RBX, off_r(inst.c));
                        em.mov_r_mem(XR::R9, XR::RBX, off_r((uint16_t)(inst.c + 1)));
                        if (inst.operand == 3) {
                            em.mov_r_mem(XR::R10, XR::RBX, off_r((uint16_t)(inst.c + 2)));
                            em.mov_rsp_disp8_r(32, XR::R10);
                            em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_construct_plain3);
                        } else {
                            em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_construct_plain2);
                        }
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        return true;
                    }
                }
                int direct_func_idx = -1;
                const JitFuncInfo* direct_fi = nullptr;
                if (inst.str_idx >= 0) {
                    const std::string& call_name = chunk.get_string(inst.str_idx);
                    for (size_t fi_idx = 0; fi_idx < chunk.func_table.size(); ++fi_idx) {
                        if (chunk.func_table[fi_idx].name == call_name) {
                            direct_func_idx = (int)fi_idx;
                            direct_fi = &chunk.func_table[fi_idx];
                            break;
                        }
                    }
                }
                if (direct_fi && inst.operand == (int)direct_fi->params.size()) {
                    const int argc_reg = jit_default_arg_count_reg(
                        chunk, direct_fi->entry_ip, 0, direct_fi->params.size());
                    const int reuse_reg = top_level_record_reuse_reg(ip, inst);
                    const bool persistent_frame =
                        unique_top_level_persistent_frame_site(ip, inst);
                    const uint32_t required_frame_regs =
                        sura_jit_required_function_frame_regs(chunk, *direct_fi);
                    std::vector<size_t> slow_jmps;
                    em.mov_ri64(XR::R11, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_r_mem(XR::RAX, XR::R11, JITINST_IC_NATIVE_FN_OFFSET);
                    em.cmp_r_imm32(XR::RAX, 0);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::E));
                    em.mov_r_mem(XR::RCX, XR::RBX, ob);
                    em.mov_ri64(XR::R10, JIT_NBOBJ);
                    em.mov_rr(XR::RDX, XR::RCX);
                    em.and_rr(XR::RDX, XR::R10);
                    em.cmp_rr(XR::RDX, XR::R10);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));
                    em.mov_ri64(XR::R10, JIT_NBPMASK);
                    em.and_rr(XR::RCX, XR::R10);
                    em.cmp_mem32_imm8(XR::RCX, INST_OBJTYPE_OFFSET, OBJ_TYPE_FUNC);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));
                    em.cmp_mem32_imm32(XR::RCX, CLOSURE_FUNC_IDX_OFFSET, direct_func_idx);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // The callee is compiled lazily after this caller. Load
                    // the exact frame extent published with the native IC,
                    // rather than exposing the full reserved scratch window.
                    em.mov_r32_mem(XR::RCX, XR::R11,
                                   JITINST_IC_NATIVE_FRAME_REGS_OFFSET);
                    em.cmp_r_imm32(XR::RCX, 0);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::E));
                    if (required_frame_regs != 0) {
                        em.cmp_r_imm32(XR::RCX,
                                       static_cast<int32_t>(required_frame_regs));
                        const size_t extent_is_sufficient =
                            em.jcc_rel32_placeholder(CC::AE);
                        em.mov_ri64(XR::RCX, required_frame_regs);
                        em.patch_rel32(extent_is_sufficient, em.pos());
                    }
                    size_t persistent_ready = SIZE_MAX;
                    size_t allocate_persistent = SIZE_MAX;
                    if (persistent_frame) {
                        em.mov_r_rsp_disp8(XR::R9, 40);
                        em.cmp_r_imm32(XR::R9, 0);
                        allocate_persistent = em.jcc_rel32_placeholder(CC::E);

                        // The frame stays continuously published, so every
                        // stale scratch Value remains a valid GC root. Reuse it
                        // only while stack_top still names its exact end.
                        em.mov_rr(XR::R10, XR::R9);
                        em.add_rr(XR::R10, XR::RCX);
                        em.cmp_r_mem(XR::R10, XR::R13,
                                     JVM_STACK_TOP_OFFSET);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));
                        persistent_ready = em.jmp_rel32_placeholder();
                        em.patch_rel32(allocate_persistent, em.pos());
                    }

                    em.mov_r_mem(XR::R9, XR::R13, JVM_STACK_TOP_OFFSET);
                    em.mov_rr(XR::R10, XR::R9);
                    em.add_rr(XR::R10, XR::RCX);
                    em.cmp_r_imm32(XR::R10, JVM_STACK_CAPACITY);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::A));
                    em.mov_mem_r(XR::R13, JVM_STACK_TOP_OFFSET, XR::R10);
                    em.mov_rsp_disp8_r(32, XR::R9);
                    if (persistent_frame) em.mov_rsp_disp8_r(40, XR::R9);
                    em.mov_r_mem(XR::RDX, XR::R13, JVM_VALUE_STACK_DATA_OFFSET);
                    em.shl_r_imm8(XR::R9, 3);
                    em.add_rr(XR::RDX, XR::R9);
                    emit_clear_dynamic_frame();
                    em.mov_r_rsp_disp8(XR::R9,
                                       persistent_frame ? 40 : 32);
                    if (persistent_ready != SIZE_MAX)
                        em.patch_rel32(persistent_ready, em.pos());
                    em.mov_r_mem(XR::RDX, XR::R13, JVM_VALUE_STACK_DATA_OFFSET);
                    em.shl_r_imm8(XR::R9, 3);
                    em.add_rr(XR::RDX, XR::R9);
                    for (int i = 0; i < (int)direct_fi->params.size(); ++i) {
                        em.mov_r_mem(XR::RCX, XR::RBX, off_r((uint16_t)(inst.c + i)));
                        em.mov_mem_r(XR::RDX, (int32_t)(i * 8), XR::RCX);
                    }
                    if (argc_reg >= 0) {
                        em.mov_ri64(XR::RCX, Value((double)inst.operand).raw_bits());
                        em.mov_mem_r(XR::RDX, (int32_t)(argc_reg * 8), XR::RCX);
                    }
                    if (direct_fi->native_reuse_flag_reg != 65535) {
                        em.mov_ri64(XR::RCX,
                            reuse_reg >= 0 ? JIT_NBTRUE : JIT_NBFALSE);
                        em.mov_mem_r(XR::RDX,
                            (int32_t)(direct_fi->native_reuse_flag_reg * 8), XR::RCX);
                    }
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::R8, XR::R12);
                    em.mov_r_mem(XR::RAX, XR::R11, JITINST_IC_NATIVE_FN_OFFSET);
                    em.call_rax();
                    if (!persistent_frame) {
                        em.mov_r_rsp_disp8(XR::R9, 32);
                        em.mov_mem_r(XR::R13, JVM_STACK_TOP_OFFSET, XR::R9);
                    }
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    size_t done_jmp = em.jmp_rel32_placeholder();
                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_call);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    em.patch_rel32(done_jmp, em.pos());
                    return true;
                }
                em.mov_rr(XR::RCX, XR::R13);  // vm
                em.mov_rr(XR::RDX, XR::RBX);  // R
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_call);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            case O::METHOD_CALL: {
                // ── Inline array push ───────────────────────────────────────
                // `a.push(v)` with one argument on an array whose vector still
                // has spare capacity is a store plus a pointer bump. The
                // interpreter's builtin dispatch returns the receiver, so the
                // fast path does too. Reallocation, other receivers and any
                // other arity fall through to the ordinary helper below.
                if (jit_array_layout_verified()) {
                    const std::string& mname = chunk.get_string(inst.str_idx);
                    // a.len() / a.size() / a.length(): (finish - start) / 8 as a double.
                    if (inst.operand == 0 &&
                        (mname == "len" || mname == "size" || mname == "length")) {
                        std::vector<size_t> slow_jmps;
                        emit_array_receiver_guard(em, ob, slow_jmps);
                        em.mov_r_mem(XR::RCX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET);
                        em.mov_r_mem(XR::RDX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_DATA_OFFSET);
                        em.not_r(XR::RDX);
                        em.add_r_imm32(XR::RDX, 1);   // rdx = -start
                        em.add_rr(XR::RCX, XR::RDX);  // rcx = finish - start (bytes)
                        em.mov_ri64(XR::RDX, 3);
                        em.mov_rr(XR::R10, XR::RCX);
                        em.mov_rr(XR::RCX, XR::RDX);
                        em.sar_r_cl(XR::R10);         // r10 = element count
                        em.cvtsi2sd_x_r(XR::XMM0, XR::R10);
                        em.movsd_mem_x(XR::RBX, oa, XR::XMM0);
                        size_t done_jmp = em.jmp_rel32_placeholder();
                        size_t slow_pos = em.pos();
                        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                        const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                        em.mov_rr(XR::RCX, XR::R13);
                        em.mov_rr(XR::RDX, XR::RBX);
                        em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_method_call);
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        em.patch_rel32(done_jmp, em.pos());
                        return true;
                    }
                    // d.has(k) / d.contains(k): proven dict -> single hash probe.
                    if (inst.operand == 1 && (mname == "has" || mname == "contains")) {
                        std::vector<size_t> slow_jmps;
                        emit_object_receiver_guard(em, ob, OBJ_TYPE_DICT, slow_jmps);
                        em.mov_rr(XR::RCX, XR::RAX);
                        em.mov_r_mem(XR::RDX, XR::RBX, off_r(static_cast<uint16_t>(inst.b + 1)));
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dict_has);
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        size_t done_jmp = em.jmp_rel32_placeholder();
                        size_t slow_pos = em.pos();
                        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                        const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                        em.mov_rr(XR::RCX, XR::R13);
                        em.mov_rr(XR::RDX, XR::RBX);
                        em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_method_call);
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        em.patch_rel32(done_jmp, em.pos());
                        return true;
                    }
                    if (inst.operand == 1 && (mname == "push" || mname == "append")) {
                        const int32_t arg_off = off_r(static_cast<uint16_t>(inst.b + 1));
                        std::vector<size_t> slow_jmps;
                        emit_array_receiver_guard(em, ob, slow_jmps);
                        em.mov_r_mem(XR::RDX, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET);
                        em.mov_r_mem(XR::R10, XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_CAP_OFFSET);
                        em.cmp_rr(XR::RDX, XR::R10);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::AE));
                        em.mov_r_mem(XR::RCX, XR::RBX, arg_off);
                        em.mov_mem_r(XR::RDX, 0, XR::RCX);
                        em.add_r_imm32(XR::RDX, 8);
                        em.mov_mem_r(XR::RAX, ARRAY_ELEMENTS_OFFSET + VECTOR_FINISH_OFFSET, XR::RDX);
                        em.mov_r_mem(XR::RAX, XR::RBX, ob);
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        size_t done_jmp = em.jmp_rel32_placeholder();

                        size_t slow_pos = em.pos();
                        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                        const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                        em.mov_rr(XR::RCX, XR::R13);
                        em.mov_rr(XR::RDX, XR::RBX);
                        em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_method_call);
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        em.patch_rel32(done_jmp, em.pos());
                        return true;
                    }
                }
                // ── Monomorphic IC fast path (ic_native_fn warm at JIT compile time) ──
                // Emits inline class guard + direct call to ic_native_fn, bypassing
                // sura_jit_method_call and its hash lookups entirely.
                //
                //   Guard 1 : R[b] is an object      (bits & NBOBJ) == NBOBJ
                //   Guard 2 : obj_type == INSTANCE
                //   Guard 3 : jit_info == ic_class   (monomorphic class guard)
                //   Alloc   : vm->stack_top += frame_count  (inline alloc_frame_regs_fast)
                //   Setup   : NR[0]=self, NR[1..p]=args
                //   Call    : ic_native_fn(vm, NR, consts)
                //   Restore : vm->stack_top = old_top
                //
                // Slow path: call sura_jit_method_call (handles polymorphic / cold cases)
                if (inst.ic_class != nullptr && inst.ic_method != nullptr &&
                    inst.ic_native_fn != nullptr) {
                    const JitClassInfo*  expected_cls = inst.ic_class;
                    SuraNativeFn         native_fn    = reinterpret_cast<SuraNativeFn>(inst.ic_native_fn);
                    const JitMethodInfo* mi           = inst.ic_method;
                    int nargs   = inst.operand;
                    int nparams = (int)mi->params.size();
                    // Only inline when all required args are present (skip defaults handling)
                    if (nargs >= nparams) {
                        const int argc_reg = jit_default_arg_count_reg(
                            chunk, mi->entry_ip, 1, mi->params.size());
                        int32_t frame_count = inst.ic_native_frame_regs > 0
                            ? static_cast<int32_t>(inst.ic_native_frame_regs)
                            : static_cast<int32_t>(mi->max_regs > 0 ? mi->max_regs : 32);
                        if (argc_reg >= 0) frame_count = std::max(frame_count, argc_reg + 1);
                        const int32_t clear_count = frame_count;
                        std::vector<size_t> slow_jmps;

                        // ── Guards ──────────────────────────────────────────
                        // 1. rax = R[b] (receiver) bits
                        em.mov_r_mem(XR::RAX, XR::RBX, ob);

                        // 2. Guard 1: (rax & NBOBJ) == NBOBJ  →  is_obj()
                        em.mov_ri64(XR::R10, JIT_NBOBJ);
                        em.mov_rr(XR::RCX, XR::RAX);
                        em.and_rr(XR::RCX, XR::R10);
                        em.cmp_rr(XR::RCX, XR::R10);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                        // 3. Extract GCInstance* (rax &= NBPMASK)
                        em.mov_ri64(XR::R10, JIT_NBPMASK);
                        em.and_rr(XR::RAX, XR::R10);

                        // 4. Guard 2: obj_type == INSTANCE (5)
                        em.cmp_mem32_imm8(XR::RAX, INST_OBJTYPE_OFFSET, OBJ_TYPE_INSTANCE);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                        // 5. Guard 3: jit_info == expected_cls
                        em.mov_r_mem(XR::RCX, XR::RAX, INST_JITINFO_OFFSET);
                        em.mov_ri64(XR::R10, (uint64_t)(uintptr_t)expected_cls);
                        em.cmp_rr(XR::RCX, XR::R10);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                        // ── Inline frame alloc + direct call ────────────────
                        // r9 = old_stack_top; r10 = new_stack_top; update vm->stack_top
                        em.mov_r_mem(XR::R9,  XR::R13, JVM_STACK_TOP_OFFSET);
                        em.mov_rr(XR::R10, XR::R9);
                        em.add_r_imm32(XR::R10, frame_count);
                        em.cmp_r_imm32(XR::R10, JVM_STACK_CAPACITY);
                        slow_jmps.push_back(em.jcc_rel32_placeholder(CC::A));

                        // Capacity failures branch to the checked helper slow
                        // path before RSP or vm->stack_top is modified.
                        em.mov_mem_r(XR::R13, JVM_STACK_TOP_OFFSET, XR::R10);
                        // Spill old_stack_top to [rsp+32] before we shift r9
                        em.mov_rsp_disp8_r(32, XR::R9);

                        // rdx = value_stack.data() + old_top*8  =  NR
                        em.mov_r_mem(XR::RDX, XR::R13, JVM_VALUE_STACK_DATA_OFFSET);
                        em.shl_r_imm8(XR::R9, 3);   // r9 = old_top * sizeof(Value)
                        em.add_rr(XR::RDX, XR::R9); // rdx = NR

                        // GC scans the entire active register window. Clear
                        // every slot before publishing self/arguments so a
                        // pressure-triggered collection cannot follow stale
                        // object bits left by an older frame.
                        em.pxor_xx(XR::XMM5, XR::XMM5);
                        int clear_i = 0;
                        for (; clear_i + 1 < clear_count; clear_i += 2) {
                            em.movdqu_mem_x(XR::RDX, (int32_t)(clear_i * 8), XR::XMM5);
                        }
                        if (clear_i < clear_count) {
                            em.xor_rr(XR::RAX, XR::RAX);
                            em.mov_mem_r(XR::RDX, (int32_t)(clear_i * 8), XR::RAX);
                        }

                        // NR[0] = R[b]  (self)
                        em.mov_r_mem(XR::RCX, XR::RBX, ob);
                        em.mov_mem_r(XR::RDX, 0, XR::RCX);

                        // NR[1..nparams] = R[b+1..b+nparams]  (unrolled)
                        for (int i = 0; i < nparams; ++i) {
                            em.mov_r_mem(XR::RCX, XR::RBX,
                                         off_r((uint16_t)(inst.b + 1 + i)));
                            em.mov_mem_r(XR::RDX, (int32_t)((i + 1) * 8), XR::RCX);
                        }
                        if (argc_reg >= 0) {
                            em.mov_ri64(XR::RCX, Value((double)nargs).raw_bits());
                            em.mov_mem_r(XR::RDX, (int32_t)(argc_reg * 8), XR::RCX);
                        }

                        // Call: native_fn(vm=rcx, NR=rdx, consts=r8)
                        em.mov_rr(XR::RCX, XR::R13);  // rcx = vm
                        // rdx = NR (already set, not touched since above)
                        em.mov_rr(XR::R8,  XR::R12);  // r8  = consts
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)native_fn);
                        em.call_rax();

                        // Restore vm->stack_top = old_stack_top
                        em.mov_r_rsp_disp8(XR::R9, 32); // r9 = old_stack_top (from spill)
                        em.mov_mem_r(XR::R13, JVM_STACK_TOP_OFFSET, XR::R9);

                        // R[a] = result (rax)
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);
                        size_t done_jmp = em.jmp_rel32_placeholder();

                        // ── Slow path: call sura_jit_method_call ────────────
                        size_t slow_pos = em.pos();
                        for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);

                        const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                        em.mov_rr(XR::RCX, XR::R13);
                        em.mov_rr(XR::RDX, XR::RBX);
                        em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                        em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_method_call);
                        em.call_rax();
                        em.mov_mem_r(XR::RBX, oa, XR::RAX);

                        em.patch_rel32(done_jmp, em.pos());
                        return true;
                    }
                    // nargs < nparams: need defaults — fall through to cold path
                }

                // Cold / not-warm: plain helper call
                {
                    const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_method_call);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    return true;
                }
            }

            case O::DOT_GET: {
                if (inst.ic_scalar_guarded_field && inst.ic_cache >= 0 &&
                    inst.ic_class != nullptr) {
                    // The scalar-plan entry already proved the source object's
                    // exact class and fixed field layout.
                    em.mov_r_mem(XR::RAX, XR::RBX, ob);
                    em.mov_ri64(XR::R10, JIT_NBPMASK);
                    em.and_rr(XR::RAX, XR::R10);
                    em.mov_r_mem(XR::RCX, XR::RAX,
                                 INST_FIELDS_OFFSET + VECTOR_DATA_OFFSET);
                    em.mov_r_mem(XR::RAX, XR::RCX, inst.ic_cache * 8);
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    if (inst.ic_scalar_numeric_field) {
                        if (!active_scalar_guard_jumps) return false;
                        static constexpr uint32_t TAG32 = 0x7ffc0000U;
                        em.mov_r32_mem(XR::RAX, XR::RCX,
                                       inst.ic_cache * 8 + 4);
                        em.and_r32_imm32(XR::RAX, TAG32);
                        em.cmp_r32_imm32(XR::RAX, TAG32);
                        active_scalar_guard_jumps->push_back(
                            em.jcc_rel32_placeholder(CC::E));
                    }
                    return true;
                }
                // ── Inline IC fast path (if ic_cache is warm) ──────────────
                // Fast path:  R[a] = GCInstance->fields[ic_cache]
                //   Guard 1 : value is an object  (bits & NBOBJ) == NBOBJ
                //   Guard 2 : obj_type == INSTANCE (GCObject::obj_type == 5)
                //   Guard 3 : jit_info == ic_class (correct class identity)
                //   Load    : fields._M_start[ic_cache * 8]
                // Slow path: call sura_jit_dot_get helper (handles any case)
                if (inst.ic_cache >= 0 && inst.ic_class != nullptr) {
                    const JitClassInfo* expected_cls = inst.ic_class;
                    int32_t field_disp = (int32_t)inst.ic_cache * 8;
                    std::vector<size_t> slow_jmps;

                    // 1. rax = R[b] value bits
                    em.mov_r_mem(XR::RAX, XR::RBX, ob);

                    // 2. Guard 1: (rax & NBOBJ) == NBOBJ  →  is_obj()
                    em.mov_ri64(XR::R10, JIT_NBOBJ);
                    em.mov_rr(XR::RCX, XR::RAX);
                    em.and_rr(XR::RCX, XR::R10);
                    em.cmp_rr(XR::RCX, XR::R10);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 3. Extract GCInstance* (rax &= NBPMASK)
                    em.mov_ri64(XR::R10, JIT_NBPMASK);
                    em.and_rr(XR::RAX, XR::R10);

                    // 4. Guard 2: obj_type == INSTANCE (5)
                    em.cmp_mem32_imm8(XR::RAX, INST_OBJTYPE_OFFSET, OBJ_TYPE_INSTANCE);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 5. Guard 3: jit_info == expected_cls
                    em.mov_r_mem(XR::RCX, XR::RAX, INST_JITINFO_OFFSET);
                    em.mov_ri64(XR::R10, (uint64_t)(uintptr_t)expected_cls);
                    em.cmp_rr(XR::RCX, XR::R10);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 6. Fast path: load fields._M_start[ic_cache]
                    em.mov_r_mem(XR::RCX, XR::RAX, INST_FIELDS_OFFSET + VECTOR_DATA_OFFSET);
                    em.mov_r_mem(XR::RAX, XR::RCX, field_disp);
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    size_t done_jmp = em.jmp_rel32_placeholder();

                    // 7. Slow path: call helper
                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);

                    const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dot_get);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);

                    // 8. Done
                    em.patch_rel32(done_jmp, em.pos());
                    return true;
                }

                // Cold (ic_cache not set yet): plain helper call
                {
                    const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dot_get);
                    em.call_rax();
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    return true;
                }
            }

            case O::DOT_SET: {
                // ── Inline IC fast path (if ic_cache is warm) ──────────────
                // Fast path:  GCInstance->fields[ic_cache] = R[b]
                //   Guard 1 : R[a] is an object (NBOBJ check)
                //   Guard 2 : obj_type == INSTANCE
                //   Guard 3 : jit_info == ic_class (class identity)
                //   Store   : fields._M_start[ic_cache * 8] = R[b]
                // Slow path: call sura_jit_dot_set helper
                if (inst.ic_cache >= 0 && inst.ic_class != nullptr) {
                    const JitClassInfo* expected_cls = inst.ic_class;
                    int32_t field_disp = (int32_t)inst.ic_cache * 8;
                    std::vector<size_t> slow_jmps;

                    // 1. rax = R[a] (receiver) bits
                    em.mov_r_mem(XR::RAX, XR::RBX, oa);

                    // 2. Guard 1: is_obj()
                    em.mov_ri64(XR::R10, JIT_NBOBJ);
                    em.mov_rr(XR::RCX, XR::RAX);
                    em.and_rr(XR::RCX, XR::R10);
                    em.cmp_rr(XR::RCX, XR::R10);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 3. Extract ptr
                    em.mov_ri64(XR::R10, JIT_NBPMASK);
                    em.and_rr(XR::RAX, XR::R10);

                    // 4. Guard 2: obj_type == INSTANCE
                    em.cmp_mem32_imm8(XR::RAX, INST_OBJTYPE_OFFSET, OBJ_TYPE_INSTANCE);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 5. Guard 3: jit_info == expected_cls
                    em.mov_r_mem(XR::RCX, XR::RAX, INST_JITINFO_OFFSET);
                    em.mov_ri64(XR::R10, (uint64_t)(uintptr_t)expected_cls);
                    em.cmp_rr(XR::RCX, XR::R10);
                    slow_jmps.push_back(em.jcc_rel32_placeholder(CC::NE));

                    // 6. Fast path: fields._M_start[ic_cache] = R[b]
                    em.mov_r_mem(XR::RCX, XR::RAX, INST_FIELDS_OFFSET + VECTOR_DATA_OFFSET);
                    em.mov_r_mem(XR::RDX, XR::RBX, ob);        // R[b] = value to store
                    em.mov_mem_r(XR::RCX, field_disp, XR::RDX);// fields[ic] = R[b]
                    size_t done_jmp = em.jmp_rel32_placeholder();

                    // 7. Slow path: call helper
                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);

                    const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dot_set);
                    em.call_rax();

                    // 8. Done
                    em.patch_rel32(done_jmp, em.pos());
                    return true;
                }

                // Cold: plain helper call
                {
                    const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                    em.mov_rr(XR::RCX, XR::R13);
                    em.mov_rr(XR::RDX, XR::RBX);
                    em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                    em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dot_set);
                    em.call_rax();
                    return true;
                }
            }

            case O::LOAD_GLOBAL: {
                // ── Inline globals load (Phase 8) ───────────────────────
                // Equivalent to: R[a] = vm->globals[inst.operand]
                // Skips the helper-call overhead (~20 cycles per access).
                // Bounds check is intentionally omitted — bytecode-emitted
                // global indices are always valid (compiler invariant).
                //
                //   mov rax, [r13 + JVM_GLOBALS_DATA_OFFSET]   ; rax = data ptr
                //   mov rax, [rax + idx*8]                     ; rax = globals[idx]
                //   mov [rbx + a*8], rax                       ; R[a] = result
                if (global_definitely_initialized(ip, inst.operand)) {
                    em.mov_r_mem(XR::RAX, XR::R13, JVM_GLOBALS_DATA_OFFSET);
                    em.mov_r_mem(XR::RAX, XR::RAX, off_c(inst.operand));
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    return true;
                }
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_ri64(XR::RDX, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_load_global_inst);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            case O::STORE_GLOBAL: {
                if (global_definitely_initialized(ip, inst.operand)) {
                    // run() pre-sizes globals. The data-flow proof also means
                    // global_initialized[idx] is already true on every path,
                    // so only the Value bits need updating here.
                    em.mov_r_mem(XR::RAX, XR::R13, JVM_GLOBALS_DATA_OFFSET);
                    em.mov_r_mem(XR::RCX, XR::RBX, oa);
                    em.mov_mem_r(XR::RAX, off_c(inst.operand), XR::RCX);
                    return true;
                }
                // sura_jit_store_global(vm, inst.operand, R[a])
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_ri64(XR::RDX, (uint64_t)(uint32_t)inst.operand);
                em.mov_r_mem(XR::R8, XR::RBX, oa);  // R[a]
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_store_global);
                em.call_rax();
                return true;
            }

            case O::RETURN_VAL:
                emit_epilogue_with_reg(inst.a);
                return true;
            case O::RETURN_NONE:
                emit_epilogue_nil();
                return true;

            // ── Phase 10: top-level main JIT support ───────────────────
            // HALT terminates the main chunk — same effect as RETURN_NONE.
            case O::HALT:
                emit_epilogue_nil();
                return true;
            // DEF_FUNC and USE_LIB are no-ops in the interpreter.
            case O::DEF_FUNC:
                return true;
            // DICT_KEYS — sura_jit_dict_keys(vm, R, &inst); writes R[a] itself.
            // Builds the iteration keys a `for ... in` over a dict walks.
            case O::DICT_KEYS: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_dict_keys);
                em.call_rax();
                return true;
            }

            // OP_IN — sura_jit_op_in(vm, R, &inst); writes R[a] itself.
            case O::OP_IN: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_op_in);
                em.call_rax();
                return true;
            }

            // FOREACH_NEXT — the interpreter ends the loop by assigning
            // `lip = operand`, which is a jump inside this same body. So the
            // helper advances the iterator and returns 1 to continue / 0 to
            // exit, and the branch is emitted here in the same shape
            // JUMP_IF_FALSE uses.
            case O::FOREACH_NEXT: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_foreach_next);
                em.call_rax();
                em.emit8(0x85); em.emit8(0xC0);   // test eax, eax
                size_t p = em.jcc_rel32_placeholder(CC::E);
                pending.push_back({p, (size_t)inst.operand});
                return true;
            }

            // NEW_INSTANCE — sura_jit_new_instance(vm, R, &inst); writes R[a]
            // itself. The constructor runs to completion inside the helper via
            // execute_frame, which is safe because this opcode's control
            // transfer is a call that returns - unlike TRY_BEGIN, there is no
            // non-local jump back into generated code to reconstruct.
            case O::NEW_INSTANCE: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_new_instance);
                em.call_rax();
                return true;
            }

            // INDEX_SET — sura_jit_index_set(vm, R, &inst); no result. Unlike
            // INDEX_GET, register a is the container being written to.
            case O::INDEX_SET: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                std::vector<size_t> slow_jmps;
                size_t done_jmp = 0;
                const bool fast = jit_array_layout_verified();
                if (fast) {
                    // Fast path: R[a] is an array, R[b] a non-negative number
                    // inside its bounds -> elements[idx] = R[c]. arr_set only
                    // ever writes in-bounds slots, so this is exactly what the
                    // helper would do; everything else (dict, negative index,
                    // out of range, non-number key) takes the helper.
                    emit_array_index_guard(em, oa, ob, slow_jmps);
                    em.mov_r_mem(XR::RDX, XR::RBX, oc);
                    em.mov_mem_r(XR::RCX, 0, XR::RDX);
                    done_jmp = em.jmp_rel32_placeholder();
                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                }
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_index_set);
                em.call_rax();
                if (fast) em.patch_rel32(done_jmp, em.pos());
                return true;
            }

            // INDEX_GET — sura_jit_index_get(vm, R, &inst); R[a] = result.
            // Indexing is polymorphic (array / dict / string / nil) and raises
            // on two of those paths, so it goes through a guarded helper rather
            // than inline code. Previously it had no case at all and fell to
            // the default bail, which disqualified 53 of the 136 programs in
            // tests/ from native compilation - the largest single blocker.
            case O::INDEX_GET: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                std::vector<size_t> slow_jmps;
                size_t done_jmp = 0;
                const bool fast = jit_array_layout_verified();
                if (fast) {
                    // Fast path: array + in-bounds non-negative numeric index
                    // -> R[a] = elements[idx], no call. Mirrors the array arm
                    // of jit_index_get; negative wrap-around, dict/string
                    // targets and the range error stay in the helper.
                    emit_array_index_guard(em, ob, oc, slow_jmps);
                    em.mov_r_mem(XR::RAX, XR::RCX, 0);
                    em.mov_mem_r(XR::RBX, oa, XR::RAX);
                    done_jmp = em.jmp_rel32_placeholder();
                    size_t slow_pos = em.pos();
                    for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                }
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_index_get);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                if (fast) em.patch_rel32(done_jmp, em.pos());
                return true;
            }

            // USE_LIB — sura_jit_use_lib(vm, R, &inst); binds a stdlib module
            // into its global slot. Previously an unconditional bail, which
            // disqualified the entire top-level chunk: main compiles
            // all-or-nothing, so one `use` sent the whole program back to the
            // interpreter.
            case O::USE_LIB: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_use_lib);
                em.call_rax();
                return true;
            }

            // MAKE_LAMBDA — sura_jit_make_lambda(vm, R, &inst); R[a] = result
            case O::MAKE_LAMBDA: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_make_lambda);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

            // DEF_CLASS — sura_jit_def_class(vm, R, &inst); no result
            case O::DEF_CLASS: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8, (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_def_class);
                em.call_rax();
                return true;
            }

            // PRINT / PRINT_NO_NL — sura_jit_print(vm, R, &inst, newline)
            case O::PRINT: case O::PRINT_NO_NL: {
                const JitInst* inst_ptr = runtime_inst ? runtime_inst : &chunk.code[ip];
                em.mov_rr(XR::RCX, XR::R13);
                em.mov_rr(XR::RDX, XR::RBX);
                em.mov_ri64(XR::R8,  (uint64_t)(uintptr_t)inst_ptr);
                em.mov_ri64(XR::R9,  (uint64_t)(inst.op == O::PRINT ? 1 : 0));
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_print);
                em.call_rax();
                return true;
            }

            default:
                return false; // unsupported — compiler bails
        }
    }
};
