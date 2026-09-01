#include "jit_native.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

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

#if SURA_JIT_ARM64_BASELINE
        // The smoke gate distinguishes real ARM64 runs by the literal phrase
        // "native execution"; keep it in this branch's summary.
        std::cout << "jit arm64 baseline: PASS (native execution of division/comparisons, "
                     "parameter guards, loops, and guarded fallbacks)\n";
#else
        std::cout << "jit arm64 baseline: PASS (division/comparison encoder, parameter "
                     "guards, loops, and guarded fallbacks)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit arm64 baseline FAILED: " << error.what() << "\n";
        return 1;
    }
}
