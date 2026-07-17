#pragma once
#include "ir.hpp"
#include "jit_op.hpp"
#include <unordered_map>
#include <vector>
#include <stdexcept>

// ================================================================
//  ir_lower  —  IRChunk → JitChunk
//
//  Two-pass:
//    Pass 1: scan for LABEL instructions, record label → code offset
//    Pass 2: emit JitInst for every non-LABEL IR instruction,
//            resolve label references to concrete offsets
//
//  Register mapping: virtual VReg → uint16_t physical register
//    Simple 1-to-1 truncation (optimisation passes can be added later)
// ================================================================

inline JitChunk ir_lower(const IRChunk& ir) {
    JitChunk out;
    // ── copy metadata ───────────────────────────────────────────
    out.constants    = ir.constants;
    out.strings      = ir.strings;
    out.global_names = ir.global_names;
    out.func_table   = ir.func_table;
    out.class_table  = ir.class_table;

    // ── Pass 1: label → instruction index ───────────────────────
    // We emit one JitInst per non-LABEL IR instruction, so we need
    // to know how many non-LABEL instructions precede each label.
    std::unordered_map<IRLabel, int> label_addr;
    {
        int addr = 0;
        for (auto& inst : ir.code) {
            if (inst.op == IROp::LABEL) {
                label_addr[(IRLabel)inst.imm] = addr;
            } else {
                ++addr;
            }
        }
    }

    auto resolve = [&](int32_t lbl) -> int {
        auto it = label_addr.find((IRLabel)lbl);
        if (it == label_addr.end())
            throw std::runtime_error("ir_lower: unresolved label");
        return it->second;
    };

    auto R = [](VReg v) -> uint16_t {
        if (v == VREG_NONE) return 0;
        return (uint16_t)(v & 0xFFFF);
    };

    uint16_t max_r = 0;
    auto track = [&](VReg v) {
        if (v != VREG_NONE && (uint16_t)(v & 0xFFFF) > max_r)
            max_r = (uint16_t)(v & 0xFFFF);
    };

    // ── Pass 2: emit ─────────────────────────────────────────────
    for (auto& i : ir.code) {
        int ln = i.line;
        // track register usage
        track(i.dst); track(i.src1); track(i.src2); track(i.src3);

        switch (i.op) {
        case IROp::LABEL:
            break; // no instruction emitted

        case IROp::FCONST:
            out.emit(JitOp::LOAD_CONST, R(i.dst), 0,0, i.imm, -1, ln);
            break;
        case IROp::BCONST:
            out.emit(JitOp::LOAD_BOOL, R(i.dst), 0,0, i.imm, -1, ln);
            break;
        case IROp::NCONST:
            out.emit(JitOp::LOAD_NIL, R(i.dst), 0,0, 0, -1, ln);
            break;
        case IROp::MOV:
            out.emit(JitOp::MOVE, R(i.dst), R(i.src1), 0, 0, -1, ln);
            break;

        case IROp::ADD:  out.emit(JitOp::ADD,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::SUB:  out.emit(JitOp::SUB,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::MUL:  out.emit(JitOp::MUL,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::DIV:  out.emit(JitOp::DIV,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::MOD:  out.emit(JitOp::MOD,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::NEG:  out.emit(JitOp::NEG,  R(i.dst),R(i.src1), 0, 0,-1, ln); break;
        case IROp::BAND: out.emit(JitOp::BIT_AND, R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::BOR:  out.emit(JitOp::BIT_OR,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::BXOR: out.emit(JitOp::BIT_XOR, R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::SHL:  out.emit(JitOp::LSHIFT,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::SHR:  out.emit(JitOp::RSHIFT,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::BNOT: out.emit(JitOp::BIT_NOT, R(i.dst),R(i.src1), 0, 0,-1, ln); break;

        case IROp::EQ:   out.emit(JitOp::CMP_EQ,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::NEQ:  out.emit(JitOp::CMP_NEQ, R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::LT:   out.emit(JitOp::CMP_LT,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::LTE:  out.emit(JitOp::CMP_LTE, R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::GT:   out.emit(JitOp::CMP_GT,  R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::GTE:  out.emit(JitOp::CMP_GTE, R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;
        case IROp::LNOT: out.emit(JitOp::LOGICAL_NOT, R(i.dst),R(i.src1), 0, 0,-1, ln); break;
        case IROp::IN:   out.emit(JitOp::OP_IN,   R(i.dst),R(i.src1),R(i.src2), 0,-1, ln); break;

        case IROp::JMP:
            out.emit(JitOp::JUMP, 0,0,0, resolve(i.imm), -1, ln);
            break;
        case IROp::JT:
            out.emit(JitOp::JUMP_IF_TRUE,  R(i.src1),0,0, resolve(i.imm), -1, ln);
            break;
        case IROp::JF:
            out.emit(JitOp::JUMP_IF_FALSE, R(i.src1),0,0, resolve(i.imm), -1, ln);
            break;

        case IROp::GLOAD:
            out.emit(JitOp::LOAD_GLOBAL,  R(i.dst), 0,0, i.imm, -1, ln);
            break;
        case IROp::GSTORE:
            out.emit(JitOp::STORE_GLOBAL, R(i.src1), 0,0, i.imm, -1, ln);
            break;
        case IROp::UVLOAD:
            out.emit(JitOp::LOAD_UPVAL,  R(i.dst), 0,0, i.imm, -1, ln);
            break;
        case IROp::UVSTORE:
            out.emit(JitOp::STORE_UPVAL, R(i.src1), 0,0, i.imm, -1, ln);
            break;

        case IROp::CALL:
            out.emit(JitOp::CALL_FUNC, R(i.dst), R(i.src1), R(i.src1)+1,
                     i.imm, i.str_id, ln);
            break;
        case IROp::MCALL:
            out.emit(JitOp::METHOD_CALL, R(i.dst), R(i.src1), 0,
                     i.imm, i.str_id, ln);
            break;
        case IROp::SCALL:
            out.emit(JitOp::SUPER_CALL, R(i.dst), R(i.src1), 0,
                     i.imm, i.str_id, ln);
            break;
        case IROp::NCALL:
            out.emit(JitOp::NEW_INSTANCE, R(i.dst), R(i.src1), 0,
                     i.imm, i.str_id, ln);
            break;
        case IROp::BUILTIN:
            out.emit(JitOp::CALL_BUILTIN, R(i.dst), R(i.src1), 0,
                     i.imm, i.str_id, ln);
            break;
        case IROp::RET:
            out.emit(JitOp::RETURN_VAL, R(i.src1), 0,0, 0, -1, ln);
            break;
        case IROp::RETVOID:
            out.emit(JitOp::RETURN_NONE, 0,0,0, 0, -1, ln);
            break;

        case IROp::MKARR:
            out.emit(JitOp::MAKE_ARRAY, R(i.dst), R(i.src1), 0, i.imm, -1, ln);
            break;
        case IROp::MKDICT:
            out.emit(JitOp::MAKE_DICT, R(i.dst), R(i.src1), 0, i.imm, -1, ln);
            break;
        case IROp::IGET:
            out.emit(JitOp::INDEX_GET, R(i.dst), R(i.src1), R(i.src2), 0, -1, ln);
            break;
        case IROp::ISET:
            out.emit(JitOp::INDEX_SET, R(i.src1), R(i.src2), R(i.src3), 0, -1, ln);
            break;
        case IROp::DGET:
            out.emit(JitOp::DOT_GET, R(i.dst), R(i.src1), 0, 0, i.str_id, ln);
            break;
        case IROp::DSET:
            out.emit(JitOp::DOT_SET, R(i.src1), R(i.src2), 0, 0, i.str_id, ln);
            break;

        case IROp::MKFN:
            out.emit(JitOp::MAKE_LAMBDA, R(i.dst), 0,0, i.imm, -1, ln);
            break;
        case IROp::DEFCLS:
            out.emit(JitOp::DEF_CLASS, 0,0,0, i.imm, i.imm2, ln);
            break;

        case IROp::PRINT:
            if (i.imm2 == 1)
                out.emit(JitOp::PRINT,       R(i.src1), 0,0, i.imm, -1, ln);
            else
                out.emit(JitOp::PRINT_NO_NL, R(i.src1), 0,0, i.imm, -1, ln);
            break;
        case IROp::FMTVAL:
            out.emit(JitOp::FORMAT_VAL, R(i.dst), R(i.src1), R(i.src2), 0, -1, ln);
            break;

        case IROp::THROW:
            out.emit(JitOp::OP_THROW, R(i.src1), 0,0, 0, -1, ln);
            break;
        case IROp::TRYBEGIN:
            out.emit(JitOp::TRY_BEGIN, R(i.dst), 0,0, resolve(i.imm), -1, ln);
            break;
        case IROp::TRYEND:
            out.emit(JitOp::TRY_END, 0,0,0, 0, -1, ln);
            break;

        case IROp::FOREACH:
            out.emit(JitOp::FOREACH_NEXT, R(i.dst), R(i.src1), R(i.src2),
                     resolve(i.imm), -1, ln);
            break;
        case IROp::DKEYS:
            out.emit(JitOp::DICT_KEYS, R(i.dst), R(i.src1), 0, 0, -1, ln);
            break;

        case IROp::USLIB:
            out.emit(JitOp::USE_LIB, 0,0,0, 0, i.str_id, ln);
            break;

        case IROp::HALT:
            out.emit(JitOp::HALT, 0,0,0, 0, -1, ln);
            break;
        }
    }

    out.max_regs = max_r + 1;
    return out;
}
