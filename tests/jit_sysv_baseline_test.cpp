#include "jit_native.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
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

// Stand-in for the pieces of JitVM that v3 bodies touch: the globals vector
// (read through its begin pointer at a fixed offset) and the direct-call
// depth budget. The generated code never dereferences anything else.
struct FakeVM {
    std::vector<Value> globals;
    int64_t depth_budget = 0;
};

struct FakeLink final : BaselineLinkContext {
    FakeVM* vm = nullptr;
    std::vector<uint64_t> pinned;
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
    bool resolve_callee(int, BaselineCalleeInfo&) override { return false; }
    void pin_value(uint64_t bits) override { pinned.push_back(bits); }
};
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

        // ── v2: guarded numeric parameters ──
        // With entry guards the dynamic-add shape becomes compileable: the
        // prologue verifies both arguments are NaN-boxed numbers and deopts
        // (returns the sentinel) when the caller passes anything else.
        JitChunk guarded_add;
        guarded_add.max_regs = 3;
        guarded_add.code.emplace_back(JitOp::ADD, 2, 0, 1);
        guarded_add.code.emplace_back(JitOp::RETURN_VAL, 2);
        std::vector<uint8_t> guarded_bytes = SysVBaselineCompiler(
            guarded_add, 0, guarded_add.code.size(), guarded_add.max_regs,
            2).compile_bytes();
        require(!guarded_bytes.empty(),
                "guarded numeric parameters should make dynamic add compileable");
        ExecCode guarded_code = ExecCode::from_bytes(guarded_bytes);
        auto guarded_function = reinterpret_cast<SysVNativeTestFn>(guarded_code.ptr);
        {
            std::vector<Value> regs(guarded_add.max_regs, Value::nil());
            regs[0] = Value(20.0);
            regs[1] = Value(22.0);
            Value sum = Value::from_bits(
                guarded_function(nullptr, regs.data(), nullptr));
            require(sum.is_num() && std::fabs(sum.as_num() - 42.0) < 1e-12,
                    "guarded add returned the wrong sum for numeric arguments");
        }
        {
            std::vector<Value> regs(guarded_add.max_regs, Value::nil());
            regs[0] = Value(true);
            regs[1] = Value(22.0);
            const uint64_t bits =
                guarded_function(nullptr, regs.data(), nullptr);
            require(bits == SURA_JIT_DEOPT_SENTINEL,
                    "non-numeric argument must return the deopt sentinel");
        }

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
        std::vector<uint8_t> loop_bytes = SysVBaselineCompiler(
            loop, 0, loop.code.size(), loop.max_regs, 1).compile_bytes();
        require(!loop_bytes.empty(), "guarded whole-body loop was rejected");
        ExecCode loop_code = ExecCode::from_bytes(loop_bytes);
        auto loop_function = reinterpret_cast<SysVNativeTestFn>(loop_code.ptr);
        {
            std::vector<Value> regs(loop.max_regs, Value::nil());
            regs[0] = Value(100.0);
            Value total = Value::from_bits(
                loop_function(nullptr, regs.data(), loop.constants.data()));
            require(total.is_num() && std::fabs(total.as_num() - 5050.0) < 1e-12,
                    "loop sum 1..100 must be 5050");
        }
        {
            std::vector<Value> regs(loop.max_regs, Value::nil());
            regs[0] = Value(false);
            const uint64_t bits =
                loop_function(nullptr, regs.data(), loop.constants.data());
            require(bits == SURA_JIT_DEOPT_SENTINEL,
                    "loop entry guard must deopt on a non-numeric argument");
        }

        // ── v2: runtime zero-divisor guard on a parameter divisor ──
        // A parameter can never be proven nonzero, so the emitter checks it and
        // deopts, letting the interpreter raise [E202] exactly as it would.
        JitChunk guarded_div;
        guarded_div.max_regs = 3;
        guarded_div.code.emplace_back(JitOp::DIV, 2, 0, 1);
        guarded_div.code.emplace_back(JitOp::RETURN_VAL, 2);
        std::vector<uint8_t> guarded_div_bytes = SysVBaselineCompiler(
            guarded_div, 0, guarded_div.code.size(), guarded_div.max_regs,
            2).compile_bytes();
        require(!guarded_div_bytes.empty(),
                "a numeric-guarded parameter divisor should compile with a zero check");
        {
            ExecCode div_code = ExecCode::from_bytes(guarded_div_bytes);
            auto div_function = reinterpret_cast<SysVNativeTestFn>(div_code.ptr);
            std::vector<Value> regs(guarded_div.max_regs, Value::nil());
            regs[0] = Value(84.0);
            regs[1] = Value(4.0);
            Value quotient = Value::from_bits(
                div_function(nullptr, regs.data(), nullptr));
            require(quotient.is_num() && std::fabs(quotient.as_num() - 21.0) < 1e-12,
                    "guarded division returned the wrong quotient");
            regs[1] = Value(0.0);
            require(div_function(nullptr, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "division by zero must deopt so the VM raises [E202]");
            regs[1] = Value(-0.0);
            require(div_function(nullptr, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "negative zero is zero for the interpreter, so it must deopt too");
            regs[1] = Value(std::numeric_limits<double>::quiet_NaN());
            Value nan_quotient = Value::from_bits(
                div_function(nullptr, regs.data(), nullptr));
            require(nan_quotient.is_num() && std::isnan(nan_quotient.as_num()),
                    "a NaN divisor divides to NaN rather than deopting");
        }

        // Without a deopt-capable caller the same body must fall back, because
        // the guard would have nowhere to escape to.
        require(SysVBaselineCompiler(
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
        require(SysVBaselineCompiler(
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

        // Without a link the body is refused: LOAD_GLOBAL has no guard to
        // check against and CALL_FUNC no target.
        require(SysVBaselineCompiler(fib, 0, fib.code.size(), fib.max_regs, 1)
                    .compile_bytes().empty(),
                "an unlinked body with globals and calls must fall back");

        SysVBaselineCompiler fib_compiler(fib, 0, fib.code.size(), fib.max_regs, 1);
        fib_compiler.set_link(&link, 0, /*callable=*/true);
        std::vector<uint8_t> fib_bytes = fib_compiler.compile_bytes();
        require(!fib_bytes.empty(), "linked self-recursive fib was rejected");
        require(fib_compiler.return_kind == BaselineBodyAnalysis::kNum,
                "fib's self-return hypothesis must settle on Num");
        require(fib_compiler.unguarded_entry_offset > 0 &&
                    fib_compiler.unguarded_entry_offset < fib_bytes.size(),
                "fib must expose an unguarded entry past its parameter guard");
        require(link.pinned.size() == 2 &&
                    link.pinned[0] == fake_vm.globals[0].raw_bits() &&
                    link.pinned[1] == fake_vm.globals[0].raw_bits(),
                "each identity guard must pin the closure it compares against");
        ExecCode fib_code = ExecCode::from_bytes(fib_bytes);
        auto fib_function = reinterpret_cast<SysVNativeTestFn>(fib_code.ptr);
        {
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "direct-call fib(20) must be 6765");
            require(fake_vm.depth_budget == 100,
                    "the depth budget must be restored after the calls return");
        }
        {
            // fib(20) nests 19 direct calls below the entry frame: a budget
            // of 18 must deopt, and the budget must read as untouched
            // afterwards.
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 18;
            require(fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "exhausting the depth budget must return the deopt sentinel");
            require(fake_vm.depth_budget == 18,
                    "a budget deopt must leave the budget as it found it");
            fake_vm.depth_budget = 19;
            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "a budget of exactly the recursion depth must succeed");
        }
        {
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(true);
            fake_vm.depth_budget = 100;
            require(fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "a non-numeric argument must still hit the entry guard");
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
                    "a rebound global must fail the identity guard");
            require(fake_vm.depth_budget == 100,
                    "an identity-guard deopt must not consume budget");
            fake_vm.globals[0] = Value(static_cast<GCObject*>(&fib_closure));
            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(1.0);
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 1.0) < 1e-12,
                    "restoring the binding must make the guard pass again");
        }

        // A body that is not callable by direct call (it reads its argument
        // count, say) keeps its globals but must refuse to call itself.
        {
            SysVBaselineCompiler uncallable(fib, 0, fib.code.size(), fib.max_regs, 1);
            uncallable.set_link(&link, 0, /*callable=*/false);
            require(uncallable.compile_bytes().empty(),
                    "a self call into an uncallable body must fall back");
        }

        // ── v3: numeric global through a NaN-tag guard ──
        // scale(n): return n * RATE
        JitChunk scale;
        scale.max_regs = 3;
        scale.code.emplace_back(JitOp::LOAD_GLOBAL, 1, 0, 0, 1);    // RATE
        scale.code.emplace_back(JitOp::MUL, 2, 0, 1);
        scale.code.emplace_back(JitOp::RETURN_VAL, 2);
        fake_vm.globals.push_back(Value(3.0));
        SysVBaselineCompiler scale_compiler(scale, 0, scale.code.size(), scale.max_regs, 1);
        scale_compiler.set_link(&link, 1, /*callable=*/true);
        std::vector<uint8_t> scale_bytes = scale_compiler.compile_bytes();
        require(!scale_bytes.empty(), "a numeric global read was rejected");
        ExecCode scale_code = ExecCode::from_bytes(scale_bytes);
        auto scale_function = reinterpret_cast<SysVNativeTestFn>(scale_code.ptr);
        {
            std::vector<Value> regs(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            Value result = Value::from_bits(
                scale_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 21.0) < 1e-12,
                    "scale(7) with RATE = 3 must be 21");
            // The guard checks the tag, not the value: a new number passes.
            fake_vm.globals[1] = Value(5.0);
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            result = Value::from_bits(
                scale_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 35.0) < 1e-12,
                    "scale(7) with RATE = 5 must read the live value 35");
            fake_vm.globals[1] = Value(true);
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            require(scale_function(fake_vm_as_jit, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "a non-numeric global must fail the tag guard");
        }

        std::cout << "jit sysv baseline: PASS (division, six comparisons, NaN semantics, "
                     "guarded fallbacks, parameter guards, loops, global guards, "
                     "and direct calls)\n";
#else
        std::cout << "jit sysv baseline: PASS (non-x86-64 compile-only target)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit sysv baseline FAILED: " << error.what() << "\n";
        return 1;
    }
}
