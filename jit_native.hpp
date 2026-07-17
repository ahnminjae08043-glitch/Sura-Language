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
//  Supported opcodes (everything else causes compile() to bail & return null):
//      LOAD_CONST, MOVE, LOAD_NIL, LOAD_BOOL,
//      ADD, SUB, MUL, MOD,
//      CMP_EQ, CMP_NEQ, CMP_LT, CMP_LTE, CMP_GT, CMP_GTE,
//      NEG, LOGICAL_NOT, bitwise integer ops,
//      JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE,
//      RETURN_VAL, RETURN_NONE, NOP.
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
extern "C" inline uint64_t sura_jit_construct_exact2(const JitClassInfo* cls,
                                                      uint64_t v0,
                                                      uint64_t v1) {
    Value obj = Value::make_inst_ref(&cls->name);
    GCInstance* instance = obj.as_inst();
    instance->fields.resize(2, Value::nil());
    instance->jit_info = const_cast<JitClassInfo*>(cls);
    instance->fields[0] = Value::from_bits(v0);
    instance->fields[1] = Value::from_bits(v1);
    return obj.raw_bits();
}

extern "C" inline uint64_t sura_jit_construct_exact3(const JitClassInfo* cls,
                                                      uint64_t v0,
                                                      uint64_t v1,
                                                      uint64_t v2) {
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
extern "C" void     sura_jit_print(JitVM* vm, struct Value* R, const JitInst* ins, int newline);
extern "C" uint64_t sura_jit_add(uint64_t a, uint64_t b);  // Phase 10 safe ADD

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
};

// Linux x86-64 starts with a deliberately small, exception-free System V
// tier. It emits no helper calls, so C++ exceptions never need to unwind
// through dynamically generated code without registered DWARF CFI. Only
// straight-line operations whose numeric inputs are proven from constants are
// accepted; every other callable remains in the register VM.
class SysVBaselineCompiler {
    const JitChunk& chunk;
    size_t entry_ip;
    size_t end_ip;
    uint32_t frame_regs;

    enum class RegisterKind : uint8_t { Unknown, Numeric, Other };

    static int32_t off_r(uint16_t reg) {
        return static_cast<int32_t>(static_cast<uint32_t>(reg) * 8U);
    }
    static int32_t off_c(int index) {
        return static_cast<int32_t>(static_cast<uint32_t>(index) * 8U);
    }

public:
    SysVBaselineCompiler(const JitChunk& source, size_t begin, size_t end,
                         uint32_t registers)
        : chunk(source), entry_ip(begin), end_ip(end), frame_regs(registers) {}

    std::vector<uint8_t> compile_bytes() const {
        if (entry_ip >= end_ip || end_ip > chunk.code.size() || frame_regs == 0 ||
            frame_regs > std::numeric_limits<uint16_t>::max()) {
            return {};
        }

        std::vector<uint8_t> code;
        X64Emitter em(code);
        std::vector<RegisterKind> kinds(frame_regs, RegisterKind::Unknown);
        std::vector<uint8_t> definitely_nonzero(frame_regs, 0);
        bool returned = false;

        // System V entry: RDI=vm, RSI=R, RDX=consts. RBX and R12 are
        // nonvolatile and hold the register and constant bases.
        em.push_r(XR::RBX);
        em.push_r(XR::R12);
        em.mov_rr(XR::RBX, XR::RSI);
        em.mov_rr(XR::R12, XR::RDX);

        auto valid_reg = [&](uint16_t reg) {
            return static_cast<uint32_t>(reg) < frame_regs;
        };
        auto emit_return = [&]() {
            em.pop_r(XR::R12);
            em.pop_r(XR::RBX);
            em.ret();
        };

        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (returned) {
                if (inst.op != JitOp::NOP) return {};
                continue;
            }

            switch (inst.op) {
                case JitOp::NOP:
                    break;
                case JitOp::LOAD_CONST:
                    if (!valid_reg(inst.a) || inst.operand < 0 ||
                        static_cast<size_t>(inst.operand) >= chunk.constants.size()) return {};
                    em.mov_r_mem(XR::RAX, XR::R12, off_c(inst.operand));
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = chunk.constants[static_cast<size_t>(inst.operand)].is_num()
                        ? RegisterKind::Numeric : RegisterKind::Other;
                    definitely_nonzero[inst.a] =
                        kinds[inst.a] == RegisterKind::Numeric &&
                        chunk.constants[static_cast<size_t>(inst.operand)].as_num() != 0.0;
                    break;
                case JitOp::LOAD_NIL:
                    if (!valid_reg(inst.a)) return {};
                    em.mov_ri64(XR::RAX, NBNIL);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                case JitOp::LOAD_BOOL:
                    if (!valid_reg(inst.a)) return {};
                    em.mov_ri64(XR::RAX, inst.operand ? NBTRUE : NBFALSE);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                case JitOp::MOVE:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b)) return {};
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = kinds[inst.b];
                    definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    break;
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c) ||
                        kinds[inst.b] != RegisterKind::Numeric ||
                        kinds[inst.c] != RegisterKind::Numeric ||
                        (inst.op == JitOp::DIV && !definitely_nonzero[inst.c])) return {};
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
                    kinds[inst.a] = RegisterKind::Numeric;
                    if (inst.op == JitOp::MUL) {
                        definitely_nonzero[inst.a] =
                            definitely_nonzero[inst.b] && definitely_nonzero[inst.c];
                    } else if (inst.op == JitOp::DIV) {
                        definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    } else {
                        definitely_nonzero[inst.a] = 0;
                    }
                    break;
                case JitOp::NEG:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) ||
                        kinds[inst.b] != RegisterKind::Numeric) return {};
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.b));
                    em.mov_ri64(XR::RCX, 0x8000000000000000ULL);
                    em.xor_rr(XR::RAX, XR::RCX);
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = RegisterKind::Numeric;
                    definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE: {
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c) ||
                        kinds[inst.b] != RegisterKind::Numeric ||
                        kinds[inst.c] != RegisterKind::Numeric) return {};
                    em.movsd_x_mem(XR::XMM0, XR::RBX, off_r(inst.b));
                    em.ucomisd_x_mem(XR::XMM0, XR::RBX, off_r(inst.c));
                    em.mov_ri64(XR::RAX, NBFALSE);
                    em.mov_ri64(XR::RCX, NBTRUE);
                    switch (inst.op) {
                        case JitOp::CMP_EQ:
                            em.cmov_rr(CC::E, XR::RAX, XR::RCX);
                            em.mov_ri64(XR::RDX, NBFALSE);
                            em.cmov_rr(CC::P, XR::RAX, XR::RDX);
                            break;
                        case JitOp::CMP_NEQ:
                            em.cmov_rr(CC::NE, XR::RAX, XR::RCX);
                            em.cmov_rr(CC::P, XR::RAX, XR::RCX);
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
                        case JitOp::CMP_GTE:
                            em.cmov_rr(CC::AE, XR::RAX, XR::RCX);
                            break;
                        default:
                            return {};
                    }
                    em.mov_mem_r(XR::RBX, off_r(inst.a), XR::RAX);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                }
                case JitOp::RETURN_VAL:
                    if (!valid_reg(inst.a)) return {};
                    em.mov_r_mem(XR::RAX, XR::RBX, off_r(inst.a));
                    emit_return();
                    returned = true;
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    em.mov_ri64(XR::RAX, NBNIL);
                    emit_return();
                    returned = true;
                    break;
                default:
                    return {};
            }
        }

        if (!returned) {
            em.mov_ri64(XR::RAX, NBNIL);
            emit_return();
        }
        return code;
    }
};

// ARM64 uses the common three-register entry shape on Windows, Linux, and
// macOS: x0=vm, x1=R, x2=consts, x0=result. This first tier deliberately has
// no stack frame and emits no helper calls. As with the System V baseline,
// only straight-line arithmetic whose inputs are proven numeric is accepted;
// unsupported bytecode stays in the register VM.
class Arm64BaselineCompiler {
    const JitChunk& chunk;
    size_t entry_ip;
    size_t end_ip;
    uint32_t frame_regs;

    enum class RegisterKind : uint8_t { Unknown, Numeric, Other };

    class Emitter {
        std::vector<uint8_t>& code;

    public:
        explicit Emitter(std::vector<uint8_t>& output) : code(output) {}

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
        void mov_imm64(unsigned target, uint64_t value) {
            word(0xD2800000U |
                 (static_cast<uint32_t>(value & 0xffffU) << 5) | target);
            for (unsigned half = 1; half < 4; ++half) {
                const uint32_t part = static_cast<uint32_t>(
                    (value >> (half * 16U)) & 0xffffU);
                word(0xF2800000U | (half << 21) | (part << 5) | target);
            }
        }
        void bti_c() { word(0xD503245FU); }
        void ret() { word(0xD65F03C0U); }
    };

public:
    Arm64BaselineCompiler(const JitChunk& source, size_t begin, size_t end,
                          uint32_t registers)
        : chunk(source), entry_ip(begin), end_ip(end), frame_regs(registers) {}

    std::vector<uint8_t> compile_bytes() const {
        // AArch64 unsigned load/store immediates are 12-bit values scaled by
        // eight for X/D registers, so register and constant indices must fit.
        if (entry_ip >= end_ip || end_ip > chunk.code.size() || frame_regs == 0 ||
            frame_regs > 4096U) {
            return {};
        }

        std::vector<uint8_t> code;
        Emitter em(code);
        // BTI is a HINT-space no-op on implementations without FEAT_BTI and
        // a valid indirect-call landing pad when branch protection is active.
        em.bti_c();
        std::vector<RegisterKind> kinds(frame_regs, RegisterKind::Unknown);
        std::vector<uint8_t> definitely_nonzero(frame_regs, 0);
        bool returned = false;

        auto valid_reg = [&](uint16_t reg) {
            return static_cast<uint32_t>(reg) < frame_regs && reg <= 4095U;
        };
        auto valid_constant = [&](int index) {
            return index >= 0 && index <= 4095 &&
                   static_cast<size_t>(index) < chunk.constants.size();
        };
        auto load_register_x9 = [&](uint16_t reg) {
            em.ldr_x(9, 1, reg);
        };
        auto store_register_x9 = [&](uint16_t reg) {
            em.str_x(9, 1, reg);
        };

        for (size_t ip = entry_ip; ip < end_ip; ++ip) {
            const JitInst& inst = chunk.code[ip];
            if (returned) {
                if (inst.op != JitOp::NOP) return {};
                continue;
            }

            switch (inst.op) {
                case JitOp::NOP:
                    break;
                case JitOp::LOAD_CONST:
                    if (!valid_reg(inst.a) || !valid_constant(inst.operand)) return {};
                    em.ldr_x(9, 2, static_cast<uint16_t>(inst.operand));
                    store_register_x9(inst.a);
                    kinds[inst.a] = chunk.constants[static_cast<size_t>(inst.operand)].is_num()
                        ? RegisterKind::Numeric : RegisterKind::Other;
                    definitely_nonzero[inst.a] =
                        kinds[inst.a] == RegisterKind::Numeric &&
                        chunk.constants[static_cast<size_t>(inst.operand)].as_num() != 0.0;
                    break;
                case JitOp::LOAD_NIL:
                    if (!valid_reg(inst.a)) return {};
                    em.mov_imm64(9, NBNIL);
                    store_register_x9(inst.a);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                case JitOp::LOAD_BOOL:
                    if (!valid_reg(inst.a)) return {};
                    em.mov_imm64(9, inst.operand ? NBTRUE : NBFALSE);
                    store_register_x9(inst.a);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                case JitOp::MOVE:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b)) return {};
                    load_register_x9(inst.b);
                    store_register_x9(inst.a);
                    kinds[inst.a] = kinds[inst.b];
                    definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    break;
                case JitOp::ADD:
                case JitOp::SUB:
                case JitOp::MUL:
                case JitOp::DIV:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c) ||
                        kinds[inst.b] != RegisterKind::Numeric ||
                        kinds[inst.c] != RegisterKind::Numeric ||
                        (inst.op == JitOp::DIV && !definitely_nonzero[inst.c])) return {};
                    em.ldr_d(0, 1, inst.b);
                    em.ldr_d(1, 1, inst.c);
                    if (inst.op == JitOp::ADD) {
                        em.fadd_d(0, 0, 1);
                    } else if (inst.op == JitOp::SUB) {
                        em.fsub_d(0, 0, 1);
                    } else if (inst.op == JitOp::MUL) {
                        em.fmul_d(0, 0, 1);
                    } else {
                        em.fdiv_d(0, 0, 1);
                    }
                    em.str_d(0, 1, inst.a);
                    kinds[inst.a] = RegisterKind::Numeric;
                    if (inst.op == JitOp::MUL) {
                        definitely_nonzero[inst.a] =
                            definitely_nonzero[inst.b] && definitely_nonzero[inst.c];
                    } else if (inst.op == JitOp::DIV) {
                        definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    } else {
                        definitely_nonzero[inst.a] = 0;
                    }
                    break;
                case JitOp::NEG:
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) ||
                        kinds[inst.b] != RegisterKind::Numeric) return {};
                    em.ldr_d(0, 1, inst.b);
                    em.fneg_d(0, 0);
                    em.str_d(0, 1, inst.a);
                    kinds[inst.a] = RegisterKind::Numeric;
                    definitely_nonzero[inst.a] = definitely_nonzero[inst.b];
                    break;
                case JitOp::CMP_EQ:
                case JitOp::CMP_NEQ:
                case JitOp::CMP_LT:
                case JitOp::CMP_LTE:
                case JitOp::CMP_GT:
                case JitOp::CMP_GTE: {
                    if (!valid_reg(inst.a) || !valid_reg(inst.b) || !valid_reg(inst.c) ||
                        kinds[inst.b] != RegisterKind::Numeric ||
                        kinds[inst.c] != RegisterKind::Numeric) return {};
                    em.ldr_d(0, 1, inst.b);
                    em.ldr_d(1, 1, inst.c);
                    em.fcmp_d(0, 1);
                    em.mov_imm64(10, NBTRUE);
                    em.mov_imm64(11, NBFALSE);
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
                    store_register_x9(inst.a);
                    kinds[inst.a] = RegisterKind::Other;
                    definitely_nonzero[inst.a] = 0;
                    break;
                }
                case JitOp::RETURN_VAL:
                    if (!valid_reg(inst.a)) return {};
                    em.ldr_x(0, 1, inst.a);
                    em.ret();
                    returned = true;
                    break;
                case JitOp::RETURN_NONE:
                case JitOp::HALT:
                    em.mov_imm64(0, NBNIL);
                    em.ret();
                    returned = true;
                    break;
                default:
                    return {};
            }
        }

        if (!returned) {
            em.mov_imm64(0, NBNIL);
            em.ret();
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
    std::vector<JitStrictCountedLoop> strict_counted_loops;

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

#if SURA_JIT_X64_SYSV_BASELINE
        try {
            SysVBaselineCompiler baseline(chunk, entry_ip, end_ip, native_frame_regs);
            std::vector<uint8_t> baseline_code = baseline.compile_bytes();
            if (baseline_code.empty()) return nullptr;
            auto nf = std::make_unique<NativeFunc>();
            nf->code = ExecCode::from_bytes(baseline_code);
            nf->fn = reinterpret_cast<SuraNativeFn>(nf->code.ptr);
            nf->frame_regs = native_frame_regs;
            return nf;
        } catch (...) {
            return nullptr;
        }
#endif

#if SURA_JIT_ARM64_BASELINE
        try {
            Arm64BaselineCompiler baseline(chunk, entry_ip, end_ip, native_frame_regs);
            std::vector<uint8_t> baseline_code = baseline.compile_bytes();
            if (baseline_code.empty()) return nullptr;
            auto nf = std::make_unique<NativeFunc>();
            nf->code = ExecCode::from_bytes(baseline_code);
            nf->fn = reinterpret_cast<SuraNativeFn>(nf->code.ptr);
            nf->frame_regs = native_frame_regs;
            return nf;
        } catch (...) {
            return nullptr;
        }
#endif

        ip_to_native.assign(chunk.code.size() + 1, SIZE_MAX);
        compute_definitely_initialized_globals();

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
        em.sub_rsp_imm8(48);
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
                    if (!emit_op(*stable, planned.source_ip, stable)) return nullptr;
                }
                active_scalar_guard_jumps = nullptr;
                emit_epilogue_with_reg(scalar_plan.result.real_reg);

                const size_t general_body = em.pos();
                for (size_t jump : scalar_guard_jumps) {
                    em.patch_rel32(jump, general_body);
                }
            }

            for (size_t ip = entry_ip; ip < end_ip; ++ip) {
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
                if (!emit_op(chunk.code[ip], ip)) return nullptr;
            }
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
                9, 9, 48, {{1, 3}, {3, 12}, {5, 13}}
            };
            nf->code = ExecCode::from_bytes(buf, unwind);
            nf->fn   = (SuraNativeFn)nf->code.ptr;
            nf->scalarized = used_scalar_plan;
            nf->record_reuse_capable = has_record_reuse_materializer;
            nf->frame_regs = native_frame_regs;
            nf->inline_insts = std::move(inline_inst_storage);
            nf->strict_loops = std::move(strict_loop_storage);
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
        em.add_rsp_imm8(48);
        em.pop_r(XR::R13);
        em.pop_r(XR::R12);
        em.pop_r(XR::RBX);
        em.ret();
    }
    void emit_epilogue_nil() {
        em.mov_ri64(XR::RAX, JIT_NBNIL);
        em.add_rsp_imm8(48);
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
    size_t emit_non_number_jump(int32_t value_offset) {
        static constexpr uint32_t TAG32 = 0x7ffc0000U;
        em.mov_r32_mem(XR::RAX, XR::RBX, value_offset + 4);
        em.and_r32_imm32(XR::RAX, TAG32);
        em.cmp_r32_imm32(XR::RAX, TAG32);
        return em.jcc_rel32_placeholder(CC::E);
    }

    bool emit_op(const JitInst& inst, size_t ip,
                 const JitInst* runtime_inst = nullptr) {
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

            case O::ADD: case O::SUB: case O::MUL: case O::DIV: {
                if (inst.op == O::DIV) return false;
                if (inst.ic_numeric_fast) {
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

            case O::CMP_LT: case O::CMP_LTE: case O::CMP_GT: case O::CMP_GTE: {
                std::vector<size_t> slow_jmps;
                slow_jmps.push_back(emit_non_number_jump(ob));
                slow_jmps.push_back(emit_non_number_jump(oc));

                // xmm0 = R[b];  ucomisd xmm0, R[c]
                // rax = NBFALSE;  rcx = NBTRUE;  cmov<cc> rax, rcx;  R[a] = rax
                em.movsd_x_mem(XR::XMM0, XR::RBX, ob);
                em.ucomisd_x_mem(XR::XMM0, XR::RBX, oc);
                em.mov_ri64(XR::RAX, JIT_NBFALSE);
                em.mov_ri64(XR::RCX, JIT_NBTRUE);
                uint8_t cc;
                switch (inst.op) {
                    case O::CMP_LT:  cc = CC::B;  break;
                    case O::CMP_LTE: cc = CC::BE; break;
                    case O::CMP_GT:  cc = CC::A;  break;
                    case O::CMP_GTE: cc = CC::AE; break;
                    default: return false;
                }
                em.cmov_rr(cc, XR::RAX, XR::RCX);
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                size_t done_jmp = em.jmp_rel32_placeholder();

                int helper_op = JIT_CMP_LT;
                switch (inst.op) {
                    case O::CMP_LT:  helper_op = JIT_CMP_LT; break;
                    case O::CMP_LTE: helper_op = JIT_CMP_LTE; break;
                    case O::CMP_GT:  helper_op = JIT_CMP_GT; break;
                    case O::CMP_GTE: helper_op = JIT_CMP_GTE; break;
                    default: return false;
                }
                size_t slow_pos = em.pos();
                for (size_t p : slow_jmps) em.patch_rel32(p, slow_pos);
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, (uint64_t)helper_op);
                em.mov_ri64(XR::R9, (uint64_t)(uint32_t)inst.line);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_checked_binary);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                em.patch_rel32(done_jmp, em.pos());
                return true;
            }

            // ── Phase 9: equality comparisons (bit-equal fast path) ──
            // Matches interpreter for: nil, bool, identical numbers, same pointer.
            // Differs for: strings with same content but different GCString*.
            // (The existing arithmetic ops similarly skip type checks for speed.)
            case O::CMP_EQ: case O::CMP_NEQ: {
                em.mov_r_mem(XR::RCX, XR::RBX, ob);
                em.mov_r_mem(XR::RDX, XR::RBX, oc);
                em.mov_ri64(XR::R8, inst.op == O::CMP_NEQ ? 1 : 0);
                em.mov_ri64(XR::RAX, (uint64_t)(uintptr_t)&sura_jit_eq);
                em.call_rax();
                em.mov_mem_r(XR::RBX, oa, XR::RAX);
                return true;
            }

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
                            em.mov_ri64(XR::RCX, (uint64_t)(uintptr_t)inst.ic_class);
                            em.mov_r_mem(XR::RDX, XR::RBX, off_r(inst.c));
                            em.mov_r_mem(XR::R8, XR::RBX,
                                         off_r((uint16_t)(inst.c + 1)));
                            if (inst.operand == 3) {
                                em.mov_r_mem(XR::R9, XR::RBX,
                                             off_r((uint16_t)(inst.c + 2)));
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
            case O::USE_LIB:
                return false;

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
