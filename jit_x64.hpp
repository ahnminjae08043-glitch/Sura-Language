#pragma once
#include <cstdint>
#include <vector>

// ================================================================
//  X64Emitter — minimal x86-64 machine code emitter
//  Covers: push/pop, mov (r,r / r,imm64 / r,[base+disp32] / inverse),
//          movsd / addsd / subsd / mulsd / divsd / ucomisd,
//          cmp r64,r64,  cmovcc, jmp/jcc rel32, ret.
//  ABI target: Win64 (args RCX, RDX, R8, R9; RBX/R12-R15 callee-saved).
// ================================================================

namespace XR {
    enum : int {
        RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7,
        R8=8,  R9=9,  R10=10, R11=11, R12=12, R13=13, R14=14, R15=15
    };
    enum : int { XMM0=0, XMM1=1, XMM2=2, XMM3=3, XMM4=4, XMM5=5, XMM6=6, XMM7=7,
                 XMM8=8, XMM9=9, XMM10=10, XMM11=11, XMM12=12, XMM13=13,
                 XMM14=14, XMM15=15 };
}

// Condition codes used for cmov/jcc (the low byte of the opcode)
namespace CC {
    constexpr uint8_t B  = 0x2; // below  (CF=1,  unsigned <)
    constexpr uint8_t AE = 0x3; // above or equal (CF=0)
    constexpr uint8_t E  = 0x4; // equal  (ZF=1)
    constexpr uint8_t NE = 0x5; // not-equal (ZF=0)
    constexpr uint8_t BE = 0x6; // below or equal (CF=1 or ZF=1)
    constexpr uint8_t A  = 0x7; // above  (CF=0 and ZF=0)
    constexpr uint8_t S  = 0x8; // sign flag set
    constexpr uint8_t NS = 0x9; // sign flag not set
    constexpr uint8_t P  = 0xA; // parity set (unordered floating comparison)
    constexpr uint8_t NP = 0xB; // parity clear
}

class X64Emitter {
    std::vector<uint8_t>& buf;
public:
    explicit X64Emitter(std::vector<uint8_t>& b) : buf(b) {}

    size_t pos() const { return buf.size(); }

    void emit8(uint8_t v) { buf.push_back(v); }
    void emit32(uint32_t v) {
        buf.push_back((uint8_t)(v & 0xff));
        buf.push_back((uint8_t)((v >> 8)  & 0xff));
        buf.push_back((uint8_t)((v >> 16) & 0xff));
        buf.push_back((uint8_t)((v >> 24) & 0xff));
    }
    void emit64(uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back((uint8_t)((v >> (i*8)) & 0xff));
    }
    void patch32(size_t p, uint32_t v) {
        for (int i = 0; i < 4; ++i) buf[p+i] = (uint8_t)((v >> (i*8)) & 0xff);
    }

    // ── REX prefix  (W=64-bit, R/X/B=extend high bit of reg/index/rm) ──
    void rex(bool w, int r, int x, int b) {
        uint8_t v = 0x40;
        if (w)      v |= 0x08;
        if (r & 8)  v |= 0x04;
        if (x & 8)  v |= 0x02;
        if (b & 8)  v |= 0x01;
        if (v != 0x40) emit8(v);
    }

    // ── ModR/M byte ──
    static uint8_t modrm(int mod, int reg, int rm) {
        return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    }

    // Emit memory operand [base + disp32].
    // If base low-3-bits == 4 (RSP/R12), a SIB byte must follow ModR/M.
    void mem_disp32(int reg, int base, int32_t disp) {
        emit8(modrm(2, reg, base));
        if ((base & 7) == 4) emit8(0x24);  // SIB: scale=0 index=none base=rm
        emit32((uint32_t)disp);
    }

    // ── push / pop ─────────────────────────────────────────
    void push_r(int r) { if (r & 8) emit8(0x41); emit8((uint8_t)(0x50 | (r & 7))); }
    void pop_r (int r) { if (r & 8) emit8(0x41); emit8((uint8_t)(0x58 | (r & 7))); }

    // ── mov r64, r64  (REX.W 89 /r) ────────────────────────
    void mov_rr(int dst, int src) {
        rex(true, src, 0, dst);
        emit8(0x89);
        emit8(modrm(3, src, dst));
    }

    // ── mov r64, imm64  (REX.W B8+rd imm64) ───────────────
    void mov_ri64(int dst, uint64_t imm) {
        rex(true, 0, 0, dst);
        emit8((uint8_t)(0xB8 | (dst & 7)));
        emit64(imm);
    }

    // ── mov r64, [base + disp32]  (REX.W 8B /r) ───────────
    void mov_r_mem(int dst, int base, int32_t disp) {
        rex(true, dst, 0, base);
        emit8(0x8B);
        mem_disp32(dst, base, disp);
    }

    void mov_r32_mem(int dst, int base, int32_t disp) {
        rex(false, dst, 0, base);
        emit8(0x8B);
        mem_disp32(dst, base, disp);
    }

    // ── mov [base + disp32], r64  (REX.W 89 /r) ───────────
    void mov_mem_r(int base, int32_t disp, int src) {
        rex(true, src, 0, base);
        emit8(0x89);
        mem_disp32(src, base, disp);
    }

    // ── movsd xmm, [base + disp32]  (F2 0F 10 /r) ─────────
    void movsd_x_mem(int x, int base, int32_t disp) {
        emit8(0xF2);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(0x10);
        mem_disp32(x, base, disp);
    }

    // ── movsd [base + disp32], xmm  (F2 0F 11 /r) ─────────
    void movsd_mem_x(int base, int32_t disp, int x) {
        emit8(0xF2);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(0x11);
        mem_disp32(x, base, disp);
    }

    // pxor xmm, xmm and unaligned 128-bit store. Native call-frame clearing
    // uses these to initialize two Value slots per instruction.
    void pxor_xx(int dst, int src) {
        emit8(0x66);
        rex(false, dst, 0, src);
        emit8(0x0F); emit8(0xEF);
        emit8(modrm(3, dst, src));
    }
    void movdqu_mem_x(int base, int32_t disp, int x) {
        emit8(0xF3);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(0x7F);
        mem_disp32(x, base, disp);
    }

    // ── xmm arith with memory operand  (F2 0F <op> /r) ───
    // op: 0x58=add, 0x5C=sub, 0x59=mul, 0x5E=div
    void xmm_arith_mem(uint8_t op, int x, int base, int32_t disp) {
        emit8(0xF2);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(op);
        mem_disp32(x, base, disp);
    }
    void addsd_x_mem(int x, int base, int32_t d) { xmm_arith_mem(0x58, x, base, d); }
    void subsd_x_mem(int x, int base, int32_t d) { xmm_arith_mem(0x5C, x, base, d); }
    void mulsd_x_mem(int x, int base, int32_t d) { xmm_arith_mem(0x59, x, base, d); }
    void divsd_x_mem(int x, int base, int32_t d) { xmm_arith_mem(0x5E, x, base, d); }

    // ── ucomisd xmm, [base + disp32]  (66 0F 2E /r) ───────
    void ucomisd_x_mem(int x, int base, int32_t disp) {
        emit8(0x66);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(0x2E);
        mem_disp32(x, base, disp);
    }

    // ── Register-to-register SSE forms for the loop register cache ──
    // movaps xmm, xmm (0F 28 /r): whole-register copy, so it neither keeps
    // a dependency on the destination's old upper half nor costs a merge.
    void movaps_xx(int dst, int src) {
        rex(false, dst, 0, src);
        emit8(0x0F); emit8(0x28);
        emit8(modrm(3, dst, src));
    }
    // addsd / subsd / mulsd / divsd xmm, xmm  (F2 0F <op> /r)
    void xmm_arith_xx(uint8_t op, int dst, int src) {
        emit8(0xF2);
        rex(false, dst, 0, src);
        emit8(0x0F); emit8(op);
        emit8(modrm(3, dst, src));
    }
    void addsd_xx(int dst, int src) { xmm_arith_xx(0x58, dst, src); }
    void subsd_xx(int dst, int src) { xmm_arith_xx(0x5C, dst, src); }
    void mulsd_xx(int dst, int src) { xmm_arith_xx(0x59, dst, src); }
    void divsd_xx(int dst, int src) { xmm_arith_xx(0x5E, dst, src); }
    // ucomisd xmm, xmm  (66 0F 2E /r)
    void ucomisd_xx(int a, int b) {
        emit8(0x66);
        rex(false, a, 0, b);
        emit8(0x0F); emit8(0x2E);
        emit8(modrm(3, a, b));
    }
    // movq r64, xmm  (66 REX.W 0F 7E /r)
    void movq_r_x(int dst, int x) {
        emit8(0x66);
        rex(true, x, 0, dst);
        emit8(0x0F); emit8(0x7E);
        emit8(modrm(3, x, dst));
    }
    // movq xmm, r64  (66 REX.W 0F 6E /r)
    void movq_x_r(int x, int src) {
        emit8(0x66);
        rex(true, x, 0, src);
        emit8(0x0F); emit8(0x6E);
        emit8(modrm(3, x, src));
    }
    // movdqu xmm, [base + disp32]  (F3 0F 6F /r): callee-saved XMM restore.
    void movdqu_x_mem(int x, int base, int32_t disp) {
        emit8(0xF3);
        rex(false, x, 0, base);
        emit8(0x0F); emit8(0x6F);
        mem_disp32(x, base, disp);
    }

    // ── and r64, r64  (REX.W 23 /r;  dst=reg, src=r/m) ────
    void and_rr(int dst, int src) {
        rex(true, dst, 0, src);
        emit8(0x23);
        emit8(modrm(3, dst, src));
    }

    void and_r32_imm32(int dst, uint32_t imm) {
        rex(false, 0, 0, dst);
        emit8(0x81);
        emit8(modrm(3, 4, dst));
        emit32(imm);
    }

    // ── add r64, r64  (REX.W 03 /r;  dst=reg, src=r/m) ────
    void add_rr(int dst, int src) {
        rex(true, dst, 0, src);
        emit8(0x03);
        emit8(modrm(3, dst, src));
    }

    // ── add r64, imm32  (REX.W 81 /0 imm32) ───────────────
    void add_r_imm32(int dst, int32_t imm) {
        rex(true, 0, 0, dst);
        emit8(0x81);
        emit8(modrm(3, 0, dst));  // /0 = ADD
        emit32((uint32_t)imm);
    }

    // ── sub r64, imm32  (REX.W 81 /5 imm32) ───────────────
    void sub_r_imm32(int dst, int32_t imm) {
        rex(true, 0, 0, dst);
        emit8(0x81);
        emit8(modrm(3, 5, dst));  // /5 = SUB
        emit32((uint32_t)imm);
    }

    // ── shl r64, imm8  (REX.W C1 /4 ib) ───────────────────
    void shl_r_imm8(int r, uint8_t imm) {
        rex(true, 0, 0, r);
        emit8(0xC1);
        emit8(modrm(3, 4, r));  // /4 = SHL
        emit8(imm);
    }

    // ── mov r64, [rsp + disp8]  (REX.W 8B mod=01 SIB) ─────
    void mov_r_rsp_disp8(int dst, int8_t disp) {
        rex(true, dst, 0, XR::RSP);
        emit8(0x8B);
        emit8(modrm(1, dst, 4));  // mod=01, rm=100 → SIB follows
        emit8(0x24);               // SIB: scale=0, index=none, base=rsp
        emit8((uint8_t)disp);
    }

    // ── or r64, r64  (REX.W 0B /r;  dst=reg, src=r/m) ──────
    void or_rr(int dst, int src) {
        rex(true, dst, 0, src);
        emit8(0x0B);
        emit8(modrm(3, dst, src));
    }
    // ── xor r64, r64  (REX.W 33 /r;  dst=reg, src=r/m) ─────
    void xor_rr(int dst, int src) {
        rex(true, dst, 0, src);
        emit8(0x33);
        emit8(modrm(3, dst, src));
    }
    // ── not r64  (REX.W F7 /2) ─────────────────────────────
    void not_r(int r) {
        rex(true, 0, 0, r);
        emit8(0xF7);
        emit8(modrm(3, 2, r));  // /2 = NOT
    }

    // ── cmp r64, [base+disp32]  (REX.W 3B /r) ──────────────
    void cmp_r_mem(int dst, int base, int32_t disp) {
        rex(true, dst, 0, base);
        emit8(0x3B);
        mem_disp32(dst, base, disp);
    }

    // ── shl r64, cl  (REX.W D3 /4) ─────────────────────────
    void shl_r_cl(int r) {
        rex(true, 0, 0, r);
        emit8(0xD3);
        emit8(modrm(3, 4, r));  // /4 = SHL
    }
    // ── sar r64, cl  (REX.W D3 /7;  arithmetic right shift) ─
    void sar_r_cl(int r) {
        rex(true, 0, 0, r);
        emit8(0xD3);
        emit8(modrm(3, 7, r));  // /7 = SAR
    }

    // ── cvttsd2si r64, [base+disp32]  (F2 REX.W 0F 2C /r) ──
    // Truncating convert: double in mem → int64 in reg
    void cvttsd2si_r_mem(int dst, int base, int32_t disp) {
        emit8(0xF2);
        rex(true, dst, 0, base);
        emit8(0x0F); emit8(0x2C);
        mem_disp32(dst, base, disp);
    }
    // ── cvttsd2si r64, xmm  (F2 REX.W 0F 2C /r) ─────────────
    void cvttsd2si_r_x(int dst, int x) {
        emit8(0xF2);
        rex(true, dst, 0, x);
        emit8(0x0F); emit8(0x2C);
        emit8(modrm(3, dst, x));
    }
    // ── cvtsi2sd xmm, r64  (F2 REX.W 0F 2A /r) ─────────────
    // Convert int64 in reg → double in xmm
    void cvtsi2sd_x_r(int x, int src) {
        emit8(0xF2);
        rex(true, x, 0, src);
        emit8(0x0F); emit8(0x2A);
        emit8(modrm(3, x, src));
    }

    // ── xorps xmm, xmm (66 0F 57 /r) — for XOR-based negation ─
    // Not currently used (we use mov+xor on integer regs for NEG)

    // ── cmp r64, r64  (REX.W 39 /r;  dst=r/m, src=reg) ────
    void cmp_rr(int a, int b) {
        rex(true, b, 0, a);
        emit8(0x39);
        emit8(modrm(3, b, a));
    }
    // ── test r64, r64  (REX.W 85 /r) ──────────────────────
    void test_rr(int a, int b) {
        rex(true, b, 0, a);
        emit8(0x85);
        emit8(modrm(3, b, a));
    }
    // ── lea r64, [base + index*8]  (REX.W 8D /r + SIB) ────
    // index must not be RSP; a base of RBP/R13 takes the disp8 form.
    void lea_r_base_index8(int dst, int base, int index) {
        rex(true, dst, index, base);
        emit8(0x8D);
        const uint8_t sib = (uint8_t)((3 << 6) | ((index & 7) << 3) | (base & 7));
        if ((base & 7) == 5) {
            emit8(modrm(1, dst, 4)); emit8(sib); emit8(0);
        } else {
            emit8(modrm(0, dst, 4)); emit8(sib);
        }
    }

    void cmp_r_imm32(int reg, int32_t imm) {
        rex(true, 0, 0, reg);
        emit8(0x81);
        emit8(modrm(3, 7, reg));
        emit32((uint32_t)imm);
    }

    void cmp_r32_imm32(int reg, uint32_t imm) {
        rex(false, 0, 0, reg);
        emit8(0x81);
        emit8(modrm(3, 7, reg));
        emit32(imm);
    }

    // ── cmp dword ptr [base+disp], imm8  (83 /7 + mem_disp32) ──
    // Used to test GCObject::obj_type (32-bit enum) against a small constant.
    void cmp_mem32_imm8(int base, int32_t disp, int8_t imm) {
        if (base & 8) emit8(0x41); // REX.B if base is R8..R15
        emit8(0x83);
        mem_disp32(7, base, disp); // /7 = CMP opcode extension
        emit8((uint8_t)imm);
    }
    void cmp_mem32_imm32(int base, int32_t disp, int32_t imm) {
        if (base & 8) emit8(0x41);
        emit8(0x81);
        mem_disp32(7, base, disp);
        emit32((uint32_t)imm);
    }

    // ── cmov<cc> r64, r64  (REX.W 0F 4<cc> /r) ────────────
    void cmov_rr(uint8_t cc, int dst, int src) {
        rex(true, dst, 0, src);
        emit8(0x0F); emit8((uint8_t)(0x40 | cc));
        emit8(modrm(3, dst, src));
    }

    // ── Jumps (rel32).  Return position of disp field to patch later. ─
    size_t jmp_rel32_placeholder() {
        emit8(0xE9); size_t p = pos(); emit32(0); return p;
    }
    size_t jcc_rel32_placeholder(uint8_t cc) {
        // cc is the short-form opcode (E=4, NE=5, B=2, AE=3, BE=6, A=7)
        emit8(0x0F); emit8((uint8_t)(0x80 | cc));
        size_t p = pos(); emit32(0); return p;
    }
    // Patches a rel32 placeholder so it branches to native_target.
    void patch_rel32(size_t disp_pos, size_t native_target) {
        int32_t rel = (int32_t)native_target - (int32_t)(disp_pos + 4);
        patch32(disp_pos, (uint32_t)rel);
    }

    // ── Stack manipulation (RSP adjustments for Win64 shadow space) ───
    // sub rsp, imm8  (REX.W 83 /5 ib)
    void sub_rsp_imm8(int8_t v) {
        emit8(0x48); emit8(0x83); emit8(0xEC); emit8((uint8_t)v);
    }
    void add_rsp_imm8(int8_t v) {
        emit8(0x48); emit8(0x83); emit8(0xC4); emit8((uint8_t)v);
    }
    // sub/add rsp, imm32  (REX.W 81 /5 id, REX.W 81 /0 id) for frames past 127 bytes.
    void sub_rsp_imm32(int32_t v) {
        emit8(0x48); emit8(0x81); emit8(0xEC); emit32((uint32_t)v);
    }
    void add_rsp_imm32(int32_t v) {
        emit8(0x48); emit8(0x81); emit8(0xC4); emit32((uint32_t)v);
    }

    // ── Indirect call: call rax  (FF /2, opcode FF D0) ─────
    void call_rax() { emit8(0xFF); emit8(0xD0); }

    // ── Direct call: call rel32 (E8 cd). Returns the disp position so the
    // caller can patch_rel32() it once the target offset is known - the
    // baseline tier uses this for self-recursive direct calls. ──
    size_t call_rel32_placeholder() {
        emit8(0xE8); size_t p = pos(); emit32(0); return p;
    }

    // ── mov qword [base + disp32], imm32 (sign-extended)  (REX.W C7 /0) ──
    void mov_mem_imm32(int base, int32_t disp, int32_t imm) {
        rex(true, 0, 0, base);
        emit8(0xC7);
        mem_disp32(0, base, disp);
        emit32((uint32_t)imm);
    }

    // ── add qword [base + disp32], imm8 (sign-extended)  (REX.W 83 /0 ib) ──
    void add_mem_imm8(int base, int32_t disp, int8_t imm) {
        rex(true, 0, 0, base);
        emit8(0x83);
        mem_disp32(0, base, disp);
        emit8((uint8_t)imm);
    }

    // ── Store reg into [rsp + disp8] (for 5th+ Win64 args) ──
    // REX.W 89 /r  mod=01 reg=src rm=100 SIB=00 100 100 disp8
    void mov_rsp_disp8_r(int8_t disp, int src) {
        rex(true, src, 0, XR::RSP);
        emit8(0x89);
        emit8(modrm(1, src, 4)); // mod=01, rm=100 → SIB follows
        emit8(0x24);              // SIB: scale=0 index=4(none) base=4(rsp)
        emit8((uint8_t)disp);
    }

    // ret
    void ret() { emit8(0xC3); }
};
