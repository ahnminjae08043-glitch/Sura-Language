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

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
using SysVNativeTestFn = uint64_t (__attribute__((sysv_abi)) *)(
    JitVM*, Value*, const Value*);
#endif

} // namespace

int main() {
    try {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
        JitChunk chunk;
        chunk.max_regs = 7;
        chunk.constants.emplace_back(20.0);
        chunk.constants.emplace_back(22.0);
        chunk.constants.emplace_back(2.0);
        chunk.constants.emplace_back(4.0);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 1, 0, 0, 1);
        chunk.code.emplace_back(JitOp::ADD, 2, 0, 1);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 3, 0, 0, 2);
        chunk.code.emplace_back(JitOp::MUL, 4, 2, 3);
        chunk.code.emplace_back(JitOp::LOAD_CONST, 5, 0, 0, 3);
        chunk.code.emplace_back(JitOp::DIV, 6, 4, 5);
        chunk.code.emplace_back(JitOp::NEG, 6, 6);
        chunk.code.emplace_back(JitOp::RETURN_VAL, 6);

        SysVBaselineCompiler compiler(chunk, 0, chunk.code.size(), chunk.max_regs);
        std::vector<uint8_t> bytes = compiler.compile_bytes();
        require(!bytes.empty(), "valid straight-line numeric chunk was rejected");

        ExecCode executable = ExecCode::from_bytes(bytes);
        auto function = reinterpret_cast<SysVNativeTestFn>(executable.ptr);
        std::vector<Value> registers(chunk.max_regs, Value::nil());
        const uint64_t result_bits = function(nullptr, registers.data(), chunk.constants.data());
        const Value result = Value::from_bits(result_bits);
        require(result.is_num() && std::fabs(result.as_num() + 21.0) < 1e-12,
                "System V baseline returned the wrong numeric result");

        struct ComparisonCase {
            JitOp op;
            double left;
            double right;
            bool expected;
        };
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<ComparisonCase> comparisons = {
            {JitOp::CMP_EQ, 4.0, 4.0, true},
            {JitOp::CMP_NEQ, 4.0, 5.0, true},
            {JitOp::CMP_LT, 4.0, 5.0, true},
            {JitOp::CMP_LTE, 4.0, 4.0, true},
            {JitOp::CMP_GT, 5.0, 4.0, true},
            {JitOp::CMP_GTE, 4.0, 4.0, true},
            {JitOp::CMP_EQ, nan, nan, false},
            {JitOp::CMP_NEQ, nan, nan, true},
            {JitOp::CMP_LT, nan, 1.0, false},
            {JitOp::CMP_LTE, nan, 1.0, false},
            {JitOp::CMP_GT, nan, 1.0, false},
            {JitOp::CMP_GTE, nan, 1.0, false},
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
            std::vector<uint8_t> compared_bytes = SysVBaselineCompiler(
                compared, 0, compared.code.size(), compared.max_regs).compile_bytes();
            require(!compared_bytes.empty(), "proven numeric comparison was rejected");
            ExecCode compared_code = ExecCode::from_bytes(compared_bytes);
            auto compared_function = reinterpret_cast<SysVNativeTestFn>(compared_code.ptr);
            std::vector<Value> compared_registers(compared.max_regs, Value::nil());
            Value compared_result = Value::from_bits(compared_function(
                nullptr, compared_registers.data(), compared.constants.data()));
            require(compared_result.is_bool() &&
                    compared_result.as_bool() == comparison.expected,
                    "System V numeric comparison returned the wrong boolean");
        }

        JitChunk dynamic;
        dynamic.max_regs = 3;
        dynamic.code.emplace_back(JitOp::ADD, 2, 0, 1);
        dynamic.code.emplace_back(JitOp::RETURN_VAL, 2);
        SysVBaselineCompiler dynamic_compiler(
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
        require(SysVBaselineCompiler(
                    zero_divisor, 0, zero_divisor.code.size(),
                    zero_divisor.max_regs).compile_bytes().empty(),
                "division with a possible zero divisor must fall back");

        JitChunk dynamic_comparison;
        dynamic_comparison.max_regs = 3;
        dynamic_comparison.code.emplace_back(JitOp::CMP_LT, 2, 0, 1);
        dynamic_comparison.code.emplace_back(JitOp::RETURN_VAL, 2);
        require(SysVBaselineCompiler(
                    dynamic_comparison, 0, dynamic_comparison.code.size(),
                    dynamic_comparison.max_regs).compile_bytes().empty(),
                "unproven dynamic comparison must fall back");

        JitChunk helper_required;
        helper_required.max_regs = 1;
        helper_required.code.emplace_back(JitOp::CALL_BUILTIN, 0, 0, 0, 0);
        helper_required.code.emplace_back(JitOp::RETURN_VAL, 0);
        SysVBaselineCompiler helper_compiler(
            helper_required, 0, helper_required.code.size(), helper_required.max_regs);
        require(helper_compiler.compile_bytes().empty(),
                "helper-calling bytecode must not enter the exception-free baseline");

        std::cout << "jit sysv baseline: PASS (division, six comparisons, NaN semantics, and guarded fallbacks)\n";
#else
        std::cout << "jit sysv baseline: PASS (non-x86-64 compile-only target)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit sysv baseline FAILED: " << error.what() << "\n";
        return 1;
    }
}
