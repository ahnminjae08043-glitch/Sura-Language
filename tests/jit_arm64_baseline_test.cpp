#include "jit_native.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <vector>

// v4 bodies reference the VM helpers declared in jit_native.hpp; this test
// links no VM, so they are stubs that record the frame they were handed and
// return whatever the test chose (the sentinel by default).
std::vector<Value*> g_helper_frames;
uint64_t g_helper_result = SURA_JIT_DEOPT_SENTINEL;
#define SURA_TEST_HELPER_STUB(name)                                              \
    extern "C" uint64_t name(JitVM*, Value* R, const JitInst*) {                \
        g_helper_frames.push_back(R);                                            \
        return g_helper_result;                                                  \
    }
SURA_TEST_HELPER_STUB(sura_bl_arith)
SURA_TEST_HELPER_STUB(sura_bl_truthy)
SURA_TEST_HELPER_STUB(sura_bl_load_global)
SURA_TEST_HELPER_STUB(sura_bl_store_global)
SURA_TEST_HELPER_STUB(sura_bl_make_array)
SURA_TEST_HELPER_STUB(sura_bl_make_dict)
SURA_TEST_HELPER_STUB(sura_bl_index_get)
SURA_TEST_HELPER_STUB(sura_bl_index_set)
SURA_TEST_HELPER_STUB(sura_bl_dot_get)
SURA_TEST_HELPER_STUB(sura_bl_dot_set)
SURA_TEST_HELPER_STUB(sura_bl_op_in)
SURA_TEST_HELPER_STUB(sura_bl_dict_keys)
SURA_TEST_HELPER_STUB(sura_bl_foreach_next)
SURA_TEST_HELPER_STUB(sura_bl_print)
SURA_TEST_HELPER_STUB(sura_bl_call)
SURA_TEST_HELPER_STUB(sura_bl_method_call)
SURA_TEST_HELPER_STUB(sura_bl_call_builtin)
#undef SURA_TEST_HELPER_STUB

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t read_word(const std::vector<uint8_t>& code, size_t offset) {
    require(offset + 4 <= code.size(), "ARM64 instruction read is out of range");
    return static_cast<uint32_t>(code[offset]) |
           (static_cast<uint32_t>(code[offset + 1]) << 8) |
           (static_cast<uint32_t>(code[offset + 2]) << 16) |
           (static_cast<uint32_t>(code[offset + 3]) << 24);
}

// Byte offsets of every BL in `code`, each paired with its resolved target
// offset relative to the start of the body.
std::vector<std::pair<size_t, size_t>> find_bl(const std::vector<uint8_t>& code) {
    std::vector<std::pair<size_t, size_t>> found;
    for (size_t at = 0; at + 4 <= code.size(); at += 4) {
        const uint32_t w = read_word(code, at);
        if ((w & 0xFC000000U) != 0x94000000U) continue;
        int32_t imm = static_cast<int32_t>(w & 0x03FFFFFFU);
        if (imm & 0x02000000) imm -= 0x04000000;
        found.emplace_back(at, static_cast<size_t>(
            static_cast<int64_t>(at) + static_cast<int64_t>(imm) * 4));
    }
    return found;
}

// Stand-in for the pieces of JitVM that v3/v4 bodies touch: the globals
// vector (read through its begin pointer at a fixed offset), the direct-call
// depth budget, the value stack direct callees take their frames from, and
// the resume/exception slots a deopt stub writes. Nothing else is
// dereferenced.
struct FakeVM {
    std::vector<Value> globals;
    int64_t depth_budget = 0;
    // v4: direct callees take their frames from the VM's value stack.
    std::vector<Value> value_stack = std::vector<Value>(256, Value::nil());
    size_t stack_top = 0;
    int64_t resume_ip = -1;
    int64_t exc_valid = 0;
};

struct FakeLink final : BaselineLinkContext {
    FakeVM* vm = nullptr;
    std::vector<uint64_t> pinned;
    std::map<int, BaselineCalleeInfo> callees;
    bool global_value(int idx, uint64_t& bits) const override {
        if (idx < 0 || static_cast<size_t>(idx) >= vm->globals.size()) return false;
        bits = vm->globals[static_cast<size_t>(idx)].raw_bits();
        return true;
    }
    int32_t globals_vector_offset() const override {
        Value* begin = nullptr;
        std::memcpy(&begin, &vm->globals, sizeof(begin));
        if (begin != vm->globals.data()) return -1;
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->globals) -
                                    reinterpret_cast<const char*>(vm));
    }
    int32_t depth_budget_offset() const override {
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->depth_budget) -
                                    reinterpret_cast<const char*>(vm));
    }
    int32_t resume_ip_offset() const override {
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->resume_ip) -
                                    reinterpret_cast<const char*>(vm));
    }
    int32_t exc_valid_offset() const override {
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->exc_valid) -
                                    reinterpret_cast<const char*>(vm));
    }
    int32_t value_stack_data_offset() const override {
        Value* begin = nullptr;
        std::memcpy(&begin, &vm->value_stack, sizeof(begin));
        if (begin != vm->value_stack.data()) return -1;
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->value_stack) -
                                    reinterpret_cast<const char*>(vm));
    }
    int32_t stack_top_offset() const override {
        return static_cast<int32_t>(reinterpret_cast<const char*>(&vm->stack_top) -
                                    reinterpret_cast<const char*>(vm));
    }
    int64_t stack_capacity() const override {
        return static_cast<int64_t>(vm->value_stack.size());
    }
    bool resolve_callee(int fidx, BaselineCalleeInfo& out) override {
        auto it = callees.find(fidx);
        if (it == callees.end()) return false;
        out = it->second;
        return true;
    }
    void pin_value(uint64_t bits) override { pinned.push_back(bits); }
};

} // namespace

int main() {
    try {
        JitChunk chunk;
        chunk.max_regs = 6;
        chunk.constants.emplace_back(20.0);
        chunk.constants.emplace_back(22.0);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 1);
        chunk.code.emplace_back(JitOp::ADD, 2, 0, 1);
        chunk.code.emplace_back(JitOp::SUB, 3, 1, 0);
        chunk.code.emplace_back(JitOp::MUL, 4, 2, 3);
        chunk.code.emplace_back(JitOp::NEG, 5, 4);
        chunk.code.emplace_back(JitOp::RETURN_VAL, 5);

        Arm64BaselineCompiler compiler(chunk, 0, chunk.code.size(), chunk.max_regs);
        std::vector<uint8_t> bytes = compiler.compile_bytes();
        require(!bytes.empty(), "valid straight-line numeric chunk was rejected");
        require((bytes.size() % 4) == 0, "ARM64 output is not instruction-aligned");
        require(bytes.size() == 88, "ARM64 baseline emitted an unexpected instruction count");
        require(read_word(bytes, 0) == 0xD503245FU,
                "ARM64 baseline does not start with a BTI call landing pad");
        require(read_word(bytes, 4) == 0xF9400049U,
                "first ARM64 body instruction is not ldr x9, [x2]");
        require(read_word(bytes, 8) == 0xF9000029U,
                "second ARM64 body instruction is not str x9, [x1]");
        require(read_word(bytes, 28) == 0x1E612800U,
                "ARM64 ADD did not emit fadd d0, d0, d1");
        require(read_word(bytes, 44) == 0x1E613800U,
                "ARM64 SUB did not emit fsub d0, d0, d1");
        require(read_word(bytes, 60) == 0x1E610800U,
                "ARM64 MUL did not emit fmul d0, d0, d1");
        require(read_word(bytes, 72) == 0x1E614000U,
                "ARM64 NEG did not emit fneg d0, d0");
        require(read_word(bytes, 80) == 0xF9401420U,
                "ARM64 return did not load register 5 into x0");
        require(read_word(bytes, bytes.size() - 4) == 0xD65F03C0U,
                "ARM64 output does not end in ret");

#if SURA_JIT_ARM64_BASELINE
        ExecCode executable = ExecCode::from_bytes(bytes);
        auto function = reinterpret_cast<SuraNativeFn>(executable.ptr);
        std::vector<Value> registers(chunk.max_regs, Value::nil());
        const uint64_t result_bits = function(
            nullptr, registers.data(), chunk.constants.data());
        const Value result = Value::from_bits(result_bits);
        require(result.is_num() && std::fabs(result.as_num() + 84.0) < 1e-12,
                "ARM64 baseline returned the wrong numeric result");
#endif

        JitChunk divided;
        divided.max_regs = 3;
        divided.constants.emplace_back(84.0);
        divided.constants.emplace_back(4.0);
        divided.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        divided.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 1);
        divided.code.emplace_back(JitOp::DIV, 2, 0, 1);
        divided.code.emplace_back(JitOp::RETURN_VAL, 2);
        std::vector<uint8_t> divided_bytes = Arm64BaselineCompiler(
            divided, 0, divided.code.size(), divided.max_regs).compile_bytes();
        require(divided_bytes.size() == 44,
                "ARM64 proven nonzero division emitted an unexpected instruction count");
        require(read_word(divided_bytes, 28) == 0x1E611800U,
                "ARM64 DIV did not emit fdiv d0, d0, d1");
#if SURA_JIT_ARM64_BASELINE
        {
            ExecCode divided_code = ExecCode::from_bytes(divided_bytes);
            auto divided_function = reinterpret_cast<SuraNativeFn>(divided_code.ptr);
            std::vector<Value> registers(divided.max_regs, Value::nil());
            Value result = Value::from_bits(divided_function(
                nullptr, registers.data(), divided.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 21.0) < 1e-12,
                    "ARM64 baseline division returned the wrong result");
        }
#endif

        struct ComparisonCase {
            JitOp op;
            unsigned condition;
            double left;
            double right;
            bool expected;
        };
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<ComparisonCase> comparisons = {
            {JitOp::CMP_EQ, 0, 4.0, 4.0, true},
            {JitOp::CMP_NEQ, 1, 4.0, 5.0, true},
            {JitOp::CMP_LT, 4, 4.0, 5.0, true},
            {JitOp::CMP_LTE, 9, 4.0, 4.0, true},
            {JitOp::CMP_GT, 12, 5.0, 4.0, true},
            {JitOp::CMP_GTE, 10, 4.0, 4.0, true},
            {JitOp::CMP_EQ, 0, nan, nan, false},
            {JitOp::CMP_NEQ, 1, nan, nan, true},
            {JitOp::CMP_LT, 4, nan, 1.0, false},
            {JitOp::CMP_LTE, 9, nan, 1.0, false},
            {JitOp::CMP_GT, 12, nan, 1.0, false},
            {JitOp::CMP_GTE, 10, nan, 1.0, false},
        };
        for (const ComparisonCase& comparison : comparisons) {
            JitChunk compared;
            compared.max_regs = 3;
            compared.constants.emplace_back(comparison.left);
            compared.constants.emplace_back(comparison.right);
            compared.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
            compared.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 1);
            compared.code.emplace_back(comparison.op, 2, 0, 1);
            compared.code.emplace_back(JitOp::RETURN_VAL, 2);
            std::vector<uint8_t> compared_bytes = Arm64BaselineCompiler(
                compared, 0, compared.code.size(), compared.max_regs).compile_bytes();
            require(compared_bytes.size() == 80,
                    "ARM64 numeric comparison emitted an unexpected instruction count");
            require(read_word(compared_bytes, 28) == 0x1E612000U,
                    "ARM64 comparison did not emit fcmp d0, d1");
            const uint32_t expected_csel = 0x9A800000U | (11U << 16) |
                (comparison.condition << 12) | (10U << 5) | 9U;
            require(read_word(compared_bytes, 64) == expected_csel,
                    "ARM64 comparison emitted the wrong csel condition");
#if SURA_JIT_ARM64_BASELINE
            ExecCode compared_code = ExecCode::from_bytes(compared_bytes);
            auto compared_function = reinterpret_cast<SuraNativeFn>(compared_code.ptr);
            std::vector<Value> registers(compared.max_regs, Value::nil());
            Value result = Value::from_bits(compared_function(
                nullptr, registers.data(), compared.constants.data()));
            require(result.is_bool() && result.as_bool() == comparison.expected,
                    "ARM64 numeric comparison returned the wrong boolean");
#endif
        }

        JitChunk moved;
        moved.max_regs = 2;
        moved.constants.emplace_back(7.5);
        moved.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        moved.code.emplace_back(JitOp::MOVE, 1, 0);
        moved.code.emplace_back(JitOp::RETURN_VAL, 1);
        std::vector<uint8_t> moved_bytes = Arm64BaselineCompiler(
            moved, 0, moved.code.size(), moved.max_regs).compile_bytes();
        require(!moved_bytes.empty(), "ARM64 baseline rejected a constant move");

        JitChunk boolean;
        boolean.max_regs = 1;
        boolean.code.emplace_back(JitOp::LOAD_BOOL, 0, 0, 0, 1);
        boolean.code.emplace_back(JitOp::RETURN_VAL, 0);
        std::vector<uint8_t> boolean_bytes = Arm64BaselineCompiler(
            boolean, 0, boolean.code.size(), boolean.max_regs).compile_bytes();
        require(!boolean_bytes.empty(), "ARM64 baseline rejected a bool return");

        JitChunk nil;
        nil.max_regs = 1;
        nil.code.emplace_back(JitOp::RETURN_NONE);
        std::vector<uint8_t> nil_bytes = Arm64BaselineCompiler(
            nil, 0, nil.code.size(), nil.max_regs).compile_bytes();
        require(!nil_bytes.empty(), "ARM64 baseline rejected a nil return");

#if SURA_JIT_ARM64_BASELINE
        {
            ExecCode moved_code = ExecCode::from_bytes(moved_bytes);
            auto moved_function = reinterpret_cast<SuraNativeFn>(moved_code.ptr);
            std::vector<Value> registers(moved.max_regs, Value::nil());
            Value result = Value::from_bits(moved_function(
                nullptr, registers.data(), moved.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 7.5) < 1e-12,
                    "ARM64 MOVE returned the wrong value");
        }
        {
            ExecCode boolean_code = ExecCode::from_bytes(boolean_bytes);
            auto boolean_function = reinterpret_cast<SuraNativeFn>(boolean_code.ptr);
            std::vector<Value> registers(boolean.max_regs, Value::nil());
            Value result = Value::from_bits(boolean_function(
                nullptr, registers.data(), boolean.constants.data()));
            require(result.is_bool() && result.as_bool(),
                    "ARM64 bool return produced the wrong value");
        }
        {
            ExecCode nil_code = ExecCode::from_bytes(nil_bytes);
            auto nil_function = reinterpret_cast<SuraNativeFn>(nil_code.ptr);
            std::vector<Value> registers(nil.max_regs, Value::nil());
            Value result = Value::from_bits(nil_function(
                nullptr, registers.data(), nil.constants.data()));
            require(result.is_nil(), "ARM64 nil return produced the wrong value");
        }
#endif

        JitChunk dynamic;
        dynamic.max_regs = 3;
        dynamic.code.emplace_back(JitOp::ADD, 2, 0, 1);
        dynamic.code.emplace_back(JitOp::RETURN_VAL, 2);
        Arm64BaselineCompiler dynamic_compiler(
            dynamic, 0, dynamic.code.size(), dynamic.max_regs);
        require(dynamic_compiler.compile_bytes().empty(),
                "unproven dynamic arithmetic must fall back to the register VM");

        JitChunk zero_divisor;
        zero_divisor.max_regs = 3;
        zero_divisor.constants.emplace_back(1.0);
        zero_divisor.constants.emplace_back(0.0);
        zero_divisor.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        zero_divisor.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 1);
        zero_divisor.code.emplace_back(JitOp::DIV, 2, 0, 1);
        zero_divisor.code.emplace_back(JitOp::RETURN_VAL, 2);
        require(Arm64BaselineCompiler(
                    zero_divisor, 0, zero_divisor.code.size(),
                    zero_divisor.max_regs).compile_bytes().empty(),
                "division with a possible zero divisor must fall back");

        JitChunk dynamic_comparison;
        dynamic_comparison.max_regs = 3;
        dynamic_comparison.code.emplace_back(JitOp::CMP_LT, 2, 0, 1);
        dynamic_comparison.code.emplace_back(JitOp::RETURN_VAL, 2);
        require(Arm64BaselineCompiler(
                    dynamic_comparison, 0, dynamic_comparison.code.size(),
                    dynamic_comparison.max_regs).compile_bytes().empty(),
                "unproven dynamic comparison must fall back");

        JitChunk helper_required;
        helper_required.max_regs = 1;
        helper_required.code.emplace_back(JitOp::CALL_BUILTIN, 0, 0, 0, 0);
        helper_required.code.emplace_back(JitOp::RETURN_VAL, 0);
        Arm64BaselineCompiler helper_compiler(
            helper_required, 0, helper_required.code.size(), helper_required.max_regs);
        require(helper_compiler.compile_bytes().empty(),
                "helper-calling bytecode must not enter the exception-free baseline");

        Arm64BaselineCompiler oversized_compiler(
            chunk, 0, chunk.code.size(), 4097);
        require(oversized_compiler.compile_bytes().empty(),
                "register files outside the scaled-immediate range must fall back");

        JitChunk maximum_offsets;
        maximum_offsets.max_regs = 4096;
        maximum_offsets.constants.resize(4096, Value(1.0));
        maximum_offsets.code.emplace_back(JitOp::LOAD_CONST, 4095, 0, 0, 4095);
        maximum_offsets.code.emplace_back(JitOp::RETURN_VAL, 4095);
        std::vector<uint8_t> maximum_bytes = Arm64BaselineCompiler(
            maximum_offsets, 0, maximum_offsets.code.size(),
            maximum_offsets.max_regs).compile_bytes();
        require(maximum_bytes.size() == 20,
                "largest ARM64 scaled register/constant offsets were rejected");
        require(read_word(maximum_bytes, 4) == 0xF97FFC49U,
                "largest ARM64 constant offset was encoded incorrectly");
        require(read_word(maximum_bytes, 8) == 0xF93FFC29U,
                "largest ARM64 register store offset was encoded incorrectly");
        require(read_word(maximum_bytes, 12) == 0xF97FFC20U,
                "largest ARM64 register return offset was encoded incorrectly");

        JitChunk oversized_constant;
        oversized_constant.max_regs = 1;
        oversized_constant.constants.resize(4097, Value(1.0));
        oversized_constant.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 4096);
        oversized_constant.code.emplace_back(JitOp::RETURN_VAL, 0);
        require(Arm64BaselineCompiler(
                    oversized_constant, 0, oversized_constant.code.size(),
                    oversized_constant.max_regs).compile_bytes().empty(),
                "constant offsets outside the scaled-immediate range must fall back");

        // ── v2: guarded numeric parameters ──
        // With entry guards the dynamic-add shape becomes compileable: the
        // prologue verifies both arguments are NaN-boxed numbers and branches
        // to a shared tail returning the deopt sentinel otherwise.
        JitChunk guarded_add;
        guarded_add.max_regs = 3;
        guarded_add.code.emplace_back(JitOp::ADD, 2, 0, 1);
        guarded_add.code.emplace_back(JitOp::RETURN_VAL, 2);
        std::vector<uint8_t> guarded_bytes = Arm64BaselineCompiler(
            guarded_add, 0, guarded_add.code.size(), guarded_add.max_regs,
            2).compile_bytes();
        require(guarded_bytes.size() == 112,
                "ARM64 guarded add emitted an unexpected instruction count");
        require(read_word(guarded_bytes, 4) == 0xF9400029U,
                "ARM64 entry guard did not load parameter 0 into x9");
        require(read_word(guarded_bytes, 24) == 0x8A0A0129U,
                "ARM64 entry guard did not emit and x9, x9, x10");
        require(read_word(guarded_bytes, 28) == 0xEB0A013FU,
                "ARM64 entry guard did not emit cmp x9, x10");
        require(read_word(guarded_bytes, 32) == 0x540001E0U,
                "first ARM64 guard branch has the wrong deopt displacement");
        require(read_word(guarded_bytes, 64) == 0x540000E0U,
                "second ARM64 guard branch has the wrong deopt displacement");
        require(read_word(guarded_bytes, guarded_bytes.size() - 4) == 0xD65F03C0U,
                "ARM64 guarded output does not end in ret");
#if SURA_JIT_ARM64_BASELINE
        {
            ExecCode guarded_code = ExecCode::from_bytes(guarded_bytes);
            auto guarded_function = reinterpret_cast<SuraNativeFn>(guarded_code.ptr);
            std::vector<Value> registers(guarded_add.max_regs, Value::nil());
            registers[0] = Value(20.0);
            registers[1] = Value(22.0);
            Value sum = Value::from_bits(guarded_function(
                nullptr, registers.data(), nullptr));
            require(sum.is_num() && std::fabs(sum.as_num() - 42.0) < 1e-12,
                    "ARM64 guarded add returned the wrong sum");
            registers[0] = Value(true);
            require(guarded_function(nullptr, registers.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 non-numeric argument must return the deopt sentinel");
        }
#endif

        // ── v2: whole-body loop (sum 1..n via JUMP / JUMP_IF_FALSE) ──
        JitChunk loop;
        loop.max_regs = 5;
        loop.constants.emplace_back(0.0);
        loop.constants.emplace_back(1.0);
        loop.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 0);      // acc = 0
        loop.code.emplace_back(JitOp::LOAD_CONST, 2, 0, 0, 1);      // one = 1
        loop.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 0);      // zero = 0
        loop.code.emplace_back(JitOp::CMP_GT, 3, 0, 4);             // n > 0 ?
        loop.code.emplace_back(JitOp::JUMP_IF_FALSE, 3, 0, 0, 8);
        loop.code.emplace_back(JitOp::ADD, 1, 1, 0);                // acc += n
        loop.code.emplace_back(JitOp::SUB, 0, 0, 2);                // n -= 1
        loop.code.emplace_back(JitOp::JUMP, 0, 0, 0, 3);
        loop.code.emplace_back(JitOp::RETURN_VAL, 1);
        std::vector<uint8_t> loop_bytes = Arm64BaselineCompiler(
            loop, 0, loop.code.size(), loop.max_regs, 1).compile_bytes();
        require(loop_bytes.size() == 204,
                "ARM64 guarded loop emitted an unexpected instruction count");
        require(read_word(loop_bytes, 136) == 0x54000140U,
                "ARM64 loop exit branch has the wrong forward displacement");
        require(read_word(loop_bytes, 172) == 0x17FFFFE4U,
                "ARM64 loop backedge has the wrong backward displacement");
#if SURA_JIT_ARM64_BASELINE
        {
            ExecCode loop_code = ExecCode::from_bytes(loop_bytes);
            auto loop_function = reinterpret_cast<SuraNativeFn>(loop_code.ptr);
            std::vector<Value> registers(loop.max_regs, Value::nil());
            registers[0] = Value(100.0);
            Value total = Value::from_bits(loop_function(
                nullptr, registers.data(), loop.constants.data()));
            require(total.is_num() && std::fabs(total.as_num() - 5050.0) < 1e-12,
                    "ARM64 loop sum 1..100 must be 5050");
            registers[0] = Value(false);
            require(loop_function(nullptr, registers.data(),
                                  loop.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 loop entry guard must deopt on a non-number");
        }
#endif

        // ── v2: runtime zero-divisor guard on a parameter divisor ──
        JitChunk guarded_div;
        guarded_div.max_regs = 3;
        guarded_div.code.emplace_back(JitOp::DIV, 2, 0, 1);
        guarded_div.code.emplace_back(JitOp::RETURN_VAL, 2);
        std::vector<uint8_t> guarded_div_bytes = Arm64BaselineCompiler(
            guarded_div, 0, guarded_div.code.size(), guarded_div.max_regs,
            2).compile_bytes();
        require(!guarded_div_bytes.empty(),
                "a numeric-guarded parameter divisor should compile with a zero check");
        require(read_word(guarded_div_bytes, 92) == 0xEB1F013FU,
                "ARM64 zero-divisor check did not emit cmp x9, xzr");
#if SURA_JIT_ARM64_BASELINE
        {
            ExecCode div_code = ExecCode::from_bytes(guarded_div_bytes);
            auto div_function = reinterpret_cast<SuraNativeFn>(div_code.ptr);
            std::vector<Value> regs(guarded_div.max_regs, Value::nil());
            regs[0] = Value(84.0);
            regs[1] = Value(4.0);
            Value quotient = Value::from_bits(
                div_function(nullptr, regs.data(), nullptr));
            require(quotient.is_num() && std::fabs(quotient.as_num() - 21.0) < 1e-12,
                    "ARM64 guarded division returned the wrong quotient");
            regs[1] = Value(0.0);
            require(div_function(nullptr, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 division by zero must deopt so the VM raises [E202]");
            regs[1] = Value(-0.0);
            require(div_function(nullptr, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 negative zero must deopt as well");
        }
#endif
        require(Arm64BaselineCompiler(
                    guarded_div, 0, guarded_div.code.size(),
                    guarded_div.max_regs, 0, /*allow_deopt=*/false)
                    .compile_bytes().empty(),
                "an unprovable divisor must fall back when deopt is not allowed");

        // A jump that escapes the body must be rejected outright.
        JitChunk bad_jump;
        bad_jump.max_regs = 2;
        bad_jump.constants.emplace_back(1.0);
        bad_jump.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        bad_jump.code.emplace_back(JitOp::JUMP, 0, 0, 0, 9);
        bad_jump.code.emplace_back(JitOp::RETURN_VAL, 0);
        require(Arm64BaselineCompiler(
                    bad_jump, 0, bad_jump.code.size(),
                    bad_jump.max_regs).compile_bytes().empty(),
                "out-of-body jump target must fall back to the register VM");

        // ── v3: LOAD_GLOBAL identity guard + self-recursive direct call ──
        // fib(n): if n <= 1 return n; return fib(n - 1) + fib(n - 2)
        JitChunk fib;
        fib.max_regs = 9;
        fib.constants.emplace_back(1.0);
        fib.constants.emplace_back(2.0);
        fib.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 0);       // one = 1
        fib.code.emplace_back(JitOp::CMP_LTE, 2, 0, 1);              // n <= 1 ?
        fib.code.emplace_back(JitOp::JUMP_IF_FALSE, 2, 0, 0, 4);
        fib.code.emplace_back(JitOp::RETURN_VAL, 0);
        fib.code.emplace_back(JitOp::LOAD_GLOBAL, 3, 0, 0, 0);      // fib
        fib.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 0);
        fib.code.emplace_back(JitOp::SUB, 5, 0, 4);                  // n - 1
        fib.code.emplace_back(JitOp::CALL_FUNC, 6, 3, 5, 1);         // fib(n - 1)
        fib.code.emplace_back(JitOp::LOAD_GLOBAL, 3, 0, 0, 0);
        fib.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 1);
        fib.code.emplace_back(JitOp::SUB, 5, 0, 4);                  // n - 2
        fib.code.emplace_back(JitOp::CALL_FUNC, 7, 3, 5, 1);         // fib(n - 2)
        fib.code.emplace_back(JitOp::ADD, 8, 6, 7);
        fib.code.emplace_back(JitOp::RETURN_VAL, 8);
        JitFuncInfo fib_info;
        fib_info.name = "fib";
        fib_info.params = {"n"};
        fib_info.entry_ip = 0;
        fib_info.end_ip = fib.code.size();
        fib_info.max_regs = fib.max_regs;
        fib.func_table.push_back(fib_info);

        GCClosure fib_closure("fib");
        fib_closure.func_idx = 0;
        GCClosure other_closure("other");
        other_closure.func_idx = 0;
        FakeVM fake_vm;
        fake_vm.globals.push_back(Value(static_cast<GCObject*>(&fib_closure)));
        FakeLink link;
        link.vm = &fake_vm;
        auto* fake_vm_as_jit = reinterpret_cast<JitVM*>(&fake_vm);
        (void)fake_vm_as_jit;  // only dereferenced by native runs

        // Without a link the body is refused: LOAD_GLOBAL has no guard to
        // check against and CALL_FUNC no target.
        require(Arm64BaselineCompiler(fib, 0, fib.code.size(), fib.max_regs, 1)
                    .compile_bytes().empty(),
                "an unlinked ARM64 body with globals and calls must fall back");

        Arm64BaselineCompiler fib_compiler(fib, 0, fib.code.size(), fib.max_regs, 1);
        fib_compiler.set_link(&link, 0, /*callable=*/true);
        std::vector<uint8_t> fib_bytes = fib_compiler.compile_bytes();
        require(!fib_bytes.empty(), "linked ARM64 self-recursive fib was rejected");
        require(!fib_compiler.vm_frame,
                "an ARM64 helper-free body must take machine-stack frames");
        require(fib_compiler.return_kind == BaselineBodyAnalysis::kNum,
                "fib's self-return hypothesis must settle on Num");
        require(link.pinned.size() == 2 &&
                    link.pinned[0] == fake_vm.globals[0].raw_bits() &&
                    link.pinned[1] == fake_vm.globals[0].raw_bits(),
                "each ARM64 identity guard must pin the closure it compares against");
        // The parameter guard (9 words, as in the leaf bodies) is followed
        // by the framed prologue; that boundary is the unguarded entry
        // native callers jump to.
        require(fib_compiler.unguarded_entry_offset == 36,
                "ARM64 fib must expose its unguarded entry right after the guard");
        require(read_word(fib_bytes, 36) == 0xA9BD7BFDU,
                "ARM64 framed prologue must start with stp x29, x30, [sp, #-48]!");
        require(read_word(fib_bytes, 40) == 0xA90153F3U,
                "ARM64 framed prologue must save x19 and x20");
        require(read_word(fib_bytes, 44) == 0xF90013F5U,
                "ARM64 framed prologue must save x21");
        require(read_word(fib_bytes, 48) == 0x910003FDU,
                "ARM64 framed prologue must set the frame pointer");
        require(read_word(fib_bytes, 52) == 0xAA0103F3U &&
                    read_word(fib_bytes, 56) == 0xAA0203F4U &&
                    read_word(fib_bytes, 60) == 0xAA0003F5U,
                "ARM64 framed prologue must move R, consts and vm into x19-x21");
        require(read_word(fib_bytes, fib_bytes.size() - 4) == 0xD65F03C0U,
                "ARM64 fib does not end in ret");
        {
            // Both self calls must be direct BLs to the unguarded entry.
            const auto calls = find_bl(fib_bytes);
            require(calls.size() == 2, "ARM64 fib must contain exactly two BLs");
            for (const auto& call : calls)
                require(call.second == fib_compiler.unguarded_entry_offset,
                        "ARM64 self calls must target the unguarded entry");
            // fib is helper-free, so each call hands the callee a machine
            // stack frame: mov x0, x21 / mov x1, sp / mov x2, x20.
            require(read_word(fib_bytes, calls[0].first - 12) == 0xAA1503E0U &&
                        read_word(fib_bytes, calls[0].first - 8) == 0x910003E1U &&
                        read_word(fib_bytes, calls[0].first - 4) == 0xAA1403E2U,
                    "ARM64 direct call must pass vm, sp and consts in x0-x2");
        }

        // A body that is not callable by direct call (it reads its argument
        // count, say) keeps its globals but must refuse to call itself.
        {
            Arm64BaselineCompiler uncallable(fib, 0, fib.code.size(), fib.max_regs, 1);
            uncallable.set_link(&link, 0, /*callable=*/false);
            require(uncallable.compile_bytes().empty(),
                    "an ARM64 self call into an uncallable body must fall back");
        }

        // ── v3: a leaf callee and a caller that reaches it through BLR ──
        // square(n) = n * n; sum_squares(a, b) = square(a) + square(b)
        JitChunk pair;
        pair.max_regs = 7;
        JitFuncInfo square_info;
        square_info.name = "square";
        square_info.params = {"n"};
        square_info.entry_ip = pair.code.size();
        pair.code.emplace_back(JitOp::MUL, 1, 0, 0);
        pair.code.emplace_back(JitOp::RETURN_VAL, 1);
        square_info.end_ip = pair.code.size();
        square_info.max_regs = 2;
        pair.func_table.push_back(square_info);
        JitFuncInfo sum_info;
        sum_info.name = "sum_squares";
        sum_info.params = {"a", "b"};
        sum_info.entry_ip = pair.code.size();
        pair.code.emplace_back(JitOp::LOAD_GLOBAL, 2, 0, 0, 1);     // square
        pair.code.emplace_back(JitOp::MOVE, 3, 0);
        pair.code.emplace_back(JitOp::CALL_FUNC, 4, 2, 3, 1);        // square(a)
        pair.code.emplace_back(JitOp::LOAD_GLOBAL, 2, 0, 0, 1);
        pair.code.emplace_back(JitOp::MOVE, 3, 1);
        pair.code.emplace_back(JitOp::CALL_FUNC, 5, 2, 3, 1);        // square(b)
        pair.code.emplace_back(JitOp::ADD, 6, 4, 5);
        pair.code.emplace_back(JitOp::RETURN_VAL, 6);
        sum_info.end_ip = pair.code.size();
        sum_info.max_regs = 7;
        pair.func_table.push_back(sum_info);

        GCClosure square_closure("square");
        square_closure.func_idx = 0;
        fake_vm.globals.push_back(Value(static_cast<GCObject*>(&square_closure)));

        Arm64BaselineCompiler square_compiler(
            pair, square_info.entry_ip, square_info.end_ip, square_info.max_regs, 1);
        square_compiler.set_link(&link, 0, /*callable=*/true);
        std::vector<uint8_t> square_bytes = square_compiler.compile_bytes();
        require(!square_bytes.empty(), "ARM64 square was rejected");
        require(square_compiler.unguarded_entry_offset == 36,
                "a leaf callee's unguarded entry follows its parameter guard");
        require(read_word(square_bytes, 36) == 0xFD400020U,
                "a leaf callee must not open a frame");
#if SURA_JIT_ARM64_BASELINE
        ExecCode square_code = ExecCode::from_bytes(square_bytes);
        const uint8_t* square_entry = static_cast<const uint8_t*>(square_code.ptr);
#else
        const uint8_t* square_entry = square_bytes.data();
#endif
        {
            BaselineCalleeInfo info;
            info.guarded_entry = square_entry;
            info.unguarded_entry = square_entry + square_compiler.unguarded_entry_offset;
            info.frame_regs = square_info.max_regs;
            info.params = 1;
            info.return_kind = static_cast<uint8_t>(square_compiler.return_kind.k);
            info.return_fidx = square_compiler.return_kind.fidx;
            link.callees[0] = info;
        }
        Arm64BaselineCompiler sum_compiler(
            pair, sum_info.entry_ip, sum_info.end_ip, sum_info.max_regs, 2);
        sum_compiler.set_link(&link, 1, /*callable=*/true);
        std::vector<uint8_t> sum_bytes = sum_compiler.compile_bytes();
        require(!sum_bytes.empty(), "ARM64 sum_squares was rejected");
        require(find_bl(sum_bytes).empty(),
                "calls into another body must not use a PC-relative BL");
        {
            size_t blr_count = 0;
            for (size_t at = 0; at + 4 <= sum_bytes.size(); at += 4)
                if (read_word(sum_bytes, at) == 0xD63F0120U) ++blr_count;  // blr x9
            require(blr_count == 2,
                    "ARM64 sum_squares must reach square through two BLRs");
        }

        // ── v4: a pure body that reaches a helper takes value-stack frames ──
        // vfib(n): if n <= 1 return n; tmp = []; return vfib(n-1) + vfib(n-2)
        JitChunk vfib;
        vfib.max_regs = 10;
        vfib.constants.emplace_back(1.0);
        vfib.constants.emplace_back(2.0);
        vfib.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 0);
        vfib.code.emplace_back(JitOp::CMP_LTE, 2, 0, 1);
        vfib.code.emplace_back(JitOp::JUMP_IF_FALSE, 2, 0, 0, 4);
        vfib.code.emplace_back(JitOp::RETURN_VAL, 0);
        vfib.code.emplace_back(JitOp::MAKE_ARRAY, 9, 0, 0, 0);       // helper
        vfib.code.emplace_back(JitOp::LOAD_GLOBAL, 3, 0, 0, 0);      // vfib
        vfib.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 0);
        vfib.code.emplace_back(JitOp::SUB, 5, 0, 4);
        vfib.code.emplace_back(JitOp::CALL_FUNC, 6, 3, 5, 1);        // ip 8
        vfib.code.emplace_back(JitOp::LOAD_GLOBAL, 3, 0, 0, 0);
        vfib.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 1);
        vfib.code.emplace_back(JitOp::SUB, 5, 0, 4);
        vfib.code.emplace_back(JitOp::CALL_FUNC, 7, 3, 5, 1);        // ip 12
        vfib.code.emplace_back(JitOp::ADD, 8, 6, 7);
        vfib.code.emplace_back(JitOp::RETURN_VAL, 8);
        JitFuncInfo vfib_info;
        vfib_info.name = "vfib";
        vfib_info.params = {"n"};
        vfib_info.entry_ip = 0;
        vfib_info.end_ip = vfib.code.size();
        vfib_info.max_regs = vfib.max_regs;
        vfib.func_table.push_back(vfib_info);
        Arm64BaselineCompiler vfib_compiler(vfib, 0, vfib.code.size(), vfib.max_regs, 1);
        vfib_compiler.set_link(&link, 0, /*callable=*/true);
        vfib_compiler.set_allow_helpers(true);
        std::vector<uint8_t> vfib_bytes = vfib_compiler.compile_bytes();
        require(!vfib_bytes.empty(), "an ARM64 helper-using recursive body was rejected");
        require(vfib_compiler.pure && vfib_compiler.vm_frame,
                "MAKE_ARRAY keeps an ARM64 body pure but puts its frames on the value stack");
        {
            // Each vfib self call hands the callee its value-stack frame
            // (x11) as R instead of sp.
            const auto calls = find_bl(vfib_bytes);
            require(calls.size() == 2, "ARM64 vfib must contain exactly two BLs");
            require(read_word(vfib_bytes, calls[0].first - 12) == 0xAA1503E0U &&
                        read_word(vfib_bytes, calls[0].first - 8) == 0xAA0B03E1U &&
                        read_word(vfib_bytes, calls[0].first - 4) == 0xAA1403E2U,
                    "ARM64 value-stack direct call must pass vm, the frame base and "
                    "consts in x0-x2");
        }

        // ── v3: numeric global through a NaN-tag guard ──
        // scale(n): return n * RATE
        JitChunk scale;
        scale.max_regs = 3;
        scale.code.emplace_back(JitOp::LOAD_GLOBAL, 1, 0, 0, 2);    // RATE
        scale.code.emplace_back(JitOp::MUL, 2, 0, 1);
        scale.code.emplace_back(JitOp::RETURN_VAL, 2);
        fake_vm.globals.push_back(Value(3.0));
        Arm64BaselineCompiler scale_compiler(scale, 0, scale.code.size(), scale.max_regs, 1);
        scale_compiler.set_link(&link, 2, /*callable=*/true);
        std::vector<uint8_t> scale_bytes = scale_compiler.compile_bytes();
        require(!scale_bytes.empty(), "an ARM64 numeric global read was rejected");

#if SURA_JIT_ARM64_BASELINE
        ExecCode fib_code = ExecCode::from_bytes(fib_bytes);
        auto fib_function = reinterpret_cast<SuraNativeFn>(fib_code.ptr);
        {
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "ARM64 direct-call fib(20) must be 6765");
            require(fake_vm.depth_budget == 100,
                    "ARM64 must restore the depth budget after the calls return");
            require(fake_vm.stack_top == 0,
                    "ARM64 must pop every callee frame off the value stack");
        }
        {
            // fib never reaches a helper: its frames stay on the machine
            // stack and a full value stack is no obstacle.
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            fake_vm.stack_top = fake_vm.value_stack.size() - fib.max_regs + 1;
            fake_vm.resume_ip = -1;
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "ARM64 machine-stack frames must not depend on value-stack room");
            require(fake_vm.stack_top == fake_vm.value_stack.size() - fib.max_regs + 1 &&
                        fake_vm.depth_budget == 100 && fake_vm.resume_ip == -1,
                    "ARM64 machine-stack frames must leave stack_top, budget and "
                    "the resume slot alone");
            fake_vm.stack_top = 0;
        }
        {
            // fib(20) nests 19 direct calls below the entry frame: a budget
            // of 18 must deopt and read as untouched afterwards.
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 18;
            require(fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 budget exhaustion must return the deopt sentinel");
            require(fake_vm.depth_budget == 18,
                    "an ARM64 budget deopt must leave the budget as it found it");
            require(fake_vm.stack_top == 0 && fake_vm.resume_ip == 7,
                    "an ARM64 budget deopt must unwind the value stack and set "
                    "the resume slot");
            fake_vm.depth_budget = 19;
            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "an ARM64 budget of exactly the recursion depth must succeed");
        }
        {
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(true);
            fake_vm.depth_budget = 100;
            require(fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 non-numeric argument must still hit the entry guard");
        }
        {
            // Rebinding the global to another closure breaks the identity
            // guard even though the new closure names the same body.
            fake_vm.globals[0] = Value(static_cast<GCObject*>(&other_closure));
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            require(fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 rebound global must fail the identity guard");
            require(fake_vm.depth_budget == 100,
                    "an ARM64 identity-guard deopt must not consume budget");
            fake_vm.globals[0] = Value(static_cast<GCObject*>(&fib_closure));
            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(1.0);
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 1.0) < 1e-12,
                    "restoring the ARM64 binding must make the guard pass again");
        }
        {
            ExecCode sum_code = ExecCode::from_bytes(sum_bytes);
            auto sum_function = reinterpret_cast<SuraNativeFn>(sum_code.ptr);
            std::vector<Value> regs(sum_info.max_regs, Value::nil());
            regs[0] = Value(3.0);
            regs[1] = Value(4.0);
            fake_vm.depth_budget = 100;
            Value result = Value::from_bits(
                sum_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 25.0) < 1e-12,
                    "ARM64 sum_squares(3, 4) through BLR must be 25");
            require(fake_vm.depth_budget == 100,
                    "ARM64 BLR calls must restore the depth budget");
            fake_vm.depth_budget = 0;
            regs.assign(sum_info.max_regs, Value::nil());
            regs[0] = Value(3.0);
            regs[1] = Value(4.0);
            require(sum_function(fake_vm_as_jit, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 BLR call with no budget must deopt");
            fake_vm.globals[1] = Value(static_cast<GCObject*>(&fib_closure));
            fake_vm.depth_budget = 100;
            regs.assign(sum_info.max_regs, Value::nil());
            regs[0] = Value(3.0);
            regs[1] = Value(4.0);
            require(sum_function(fake_vm_as_jit, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 rebound callee must fail the identity guard");
            fake_vm.globals[1] = Value(static_cast<GCObject*>(&square_closure));
        }
        {
            ExecCode vfib_code = ExecCode::from_bytes(vfib_bytes);
            auto vfib_function = reinterpret_cast<SuraNativeFn>(vfib_code.ptr);
            std::vector<Value> regs(vfib.max_regs, Value::nil());
            regs[0] = Value(15.0);
            fake_vm.depth_budget = 100;
            fake_vm.stack_top = 40;
            fake_vm.value_stack.assign(fake_vm.value_stack.size(), Value::nil());
            g_helper_frames.clear();
            g_helper_result = Value(42.0).raw_bits();
            Value result = Value::from_bits(
                vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 610.0) < 1e-12,
                    "ARM64 value-stack vfib(15) must be 610");
            require(fake_vm.depth_budget == 100 && fake_vm.stack_top == 40,
                    "ARM64 must pop every value-stack callee frame again");
            for (size_t i = 0; i < 40; ++i)
                require(fake_vm.value_stack[i].is_nil(),
                        "ARM64 value-stack frames must not touch slots below stack_top");
            require(!g_helper_frames.empty() && g_helper_frames[0] == regs.data(),
                    "the ARM64 entry frame's helper must see the entry registers");
            bool callee_frames_on_stack = g_helper_frames.size() > 1;
            for (size_t i = 1; i < g_helper_frames.size(); ++i) {
                Value* base = fake_vm.value_stack.data();
                if (g_helper_frames[i] < base + 40 ||
                    g_helper_frames[i] >= base + fake_vm.value_stack.size())
                    callee_frames_on_stack = false;
            }
            require(callee_frames_on_stack,
                    "an ARM64 helper in a direct callee must see a value-stack frame");

            regs.assign(vfib.max_regs, Value::nil());
            regs[0] = Value(15.0);
            fake_vm.stack_top = fake_vm.value_stack.size() - vfib.max_regs + 1;
            fake_vm.resume_ip = -1;
            require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 full value stack must return the deopt sentinel");
            require(fake_vm.stack_top == fake_vm.value_stack.size() - vfib.max_regs + 1 &&
                        fake_vm.depth_budget == 100 && fake_vm.resume_ip == 8,
                    "an ARM64 capacity deopt must leave stack_top and budget "
                    "untouched and resume at the first CALL_FUNC");
            fake_vm.stack_top = 0;

            regs.assign(vfib.max_regs, Value::nil());
            regs[0] = Value(15.0);
            fake_vm.depth_budget = 13;
            fake_vm.resume_ip = -1;
            require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "ARM64 budget exhaustion under value-stack frames must deopt");
            require(fake_vm.depth_budget == 13 && fake_vm.stack_top == 0 &&
                        fake_vm.resume_ip == 8,
                    "an ARM64 budget deopt must unwind every value-stack frame");

            regs.assign(vfib.max_regs, Value::nil());
            regs[0] = Value(15.0);
            fake_vm.depth_budget = 100;
            fake_vm.resume_ip = -1;
            fake_vm.exc_valid = 1;
            g_helper_frames.clear();
            g_helper_result = SURA_JIT_DEOPT_SENTINEL;
            require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 raising helper must propagate the sentinel");
            require(g_helper_frames.size() == 1 && fake_vm.resume_ip == 4 &&
                        fake_vm.exc_valid == 1 && fake_vm.stack_top == 0 &&
                        fake_vm.depth_budget == 100,
                    "the ARM64 entry frame's helper deopt keeps the exception and "
                    "names the raising instruction");
            fake_vm.exc_valid = 0;
            fake_vm.depth_budget = 0;
        }
        {
            ExecCode scale_code = ExecCode::from_bytes(scale_bytes);
            auto scale_function = reinterpret_cast<SuraNativeFn>(scale_code.ptr);
            std::vector<Value> regs(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            Value result = Value::from_bits(
                scale_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 21.0) < 1e-12,
                    "ARM64 scale(7) with RATE = 3 must be 21");
            // The guard checks the tag, not the value: a new number passes.
            fake_vm.globals[2] = Value(5.0);
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            result = Value::from_bits(
                scale_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 35.0) < 1e-12,
                    "ARM64 scale(7) with RATE = 5 must read the live value 35");
            fake_vm.globals[2] = Value(true);
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            require(scale_function(fake_vm_as_jit, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "an ARM64 non-numeric global must fail the tag guard");
        }
#endif

#if SURA_JIT_ARM64_BASELINE
        // The smoke gate distinguishes real ARM64 runs by the literal phrase
        // "native execution"; keep it in this branch's summary.
        std::cout << "jit arm64 baseline: PASS (native execution of division/comparisons, "
                     "parameter guards, loops, guarded fallbacks, global guards, "
                     "and direct calls)\n";
#else
        std::cout << "jit arm64 baseline: PASS (division/comparison encoder, parameter "
                     "guards, loops, guarded fallbacks, global guards, and direct calls)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit arm64 baseline FAILED: " << error.what() << "\n";
        return 1;
    }
}
