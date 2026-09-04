#include "jit_native.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// v4 bodies call the VM helpers declared in jit_native.hpp for every op the
// analysis marks dynamic. This test links no VM, so each helper is a stub
// that records how it was called and returns whatever the test chose:
// a value the body must store, or the deopt sentinel the body must propagate.
struct HelperCall {
    const char* name;
    JitVM* vm;
    Value* R;
    const JitInst* ins;
};
std::vector<HelperCall> g_helper_calls;
uint64_t g_helper_result = SURA_JIT_DEOPT_SENTINEL;

#define SURA_TEST_HELPER_STUB(name)                                              \
    extern "C" uint64_t name(JitVM* vm, Value* R, const JitInst* ins) {         \
        g_helper_calls.push_back({#name, vm, R, ins});                           \
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

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
using SysVNativeTestFn = uint64_t (__attribute__((sysv_abi)) *)(
    JitVM*, Value*, const Value*);
// The same body entered through the Windows x64 convention. GCC and Clang
// honour ms_abi on every x86-64 target, so this section also runs on Linux.
using Win64NativeTestFn = uint64_t (__attribute__((ms_abi)) *)(
    JitVM*, Value*, const Value*);

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
            // Value::eq compares bits first, so a NaN equals itself (the
            // interpreter's answer); an ordered compare alone would say no.
            {JitOp::CMP_EQ, nan, nan, true},
            {JitOp::CMP_NEQ, nan, nan, false},
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
            require(fake_vm.stack_top == 0,
                    "every direct callee frame must be popped off the value stack");
        }
        {
            // The entry frame's registers stay on the caller's side; a
            // callee frame is carved at stack_top, so the caller sees the
            // callee's registers only through the stored result.
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(5.0);
            fake_vm.depth_budget = 100;
            fake_vm.value_stack.assign(fake_vm.value_stack.size(), Value::nil());
            fake_vm.stack_top = 40;
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 5.0) < 1e-12,
                    "direct-call fib(5) must be 5");
            require(fake_vm.stack_top == 40,
                    "a non-zero stack_top must read as it was after the calls");
            for (size_t i = 0; i < 40; ++i)
                require(fake_vm.value_stack[i].is_nil(),
                        "callee frames must not touch slots below stack_top");
            fake_vm.stack_top = 0;
        }
        {
            // fib never reaches a helper, so no collector can run while a
            // callee frame is live: the frames stay on the machine stack
            // and the value stack is never consulted, even when it is too
            // full for a single frame.
            require(!fib_compiler.vm_frame,
                    "a helper-free body must take machine-stack frames");
            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            fake_vm.stack_top = fake_vm.value_stack.size() - fib.max_regs + 1;
            fake_vm.resume_ip = -1;
            Value result = Value::from_bits(
                fib_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "machine-stack frames must not depend on value-stack room");
            require(fake_vm.stack_top == fake_vm.value_stack.size() - fib.max_regs + 1 &&
                        fake_vm.depth_budget == 100 && fake_vm.resume_ip == -1,
                    "machine-stack frames must leave stack_top, budget and the "
                    "resume slot alone");
            fake_vm.stack_top = 0;
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
            require(fake_vm.stack_top == 0,
                    "a budget deopt must unwind every callee frame");
            require(fake_vm.resume_ip == 7,
                    "a budget deopt propagated to the entry frame must resume at "
                    "its first CALL_FUNC");
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

        // ── Win64 convention: the same fib and scale bodies, entered with
        // vm/R/consts in RCX/RDX/R8. The direct-call sequence must hand the
        // callee its arguments in the same registers, and the entry guard
        // must leave RCX and RDX intact for the prologue.
        {
            SysVBaselineCompiler win_fib(fib, 0, fib.code.size(), fib.max_regs, 1);
            win_fib.set_link(&link, 0, /*callable=*/true);
            win_fib.set_abi(X64BaselineAbi::Win64);
            std::vector<uint8_t> win_bytes = win_fib.compile_bytes();
            require(!win_bytes.empty(), "Win64 self-recursive fib was rejected");
            ExecCode win_code = ExecCode::from_bytes(win_bytes);
            auto win_function = reinterpret_cast<Win64NativeTestFn>(win_code.ptr);

            std::vector<Value> regs(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 100;
            Value result = Value::from_bits(
                win_function(fake_vm_as_jit, regs.data(), fib.constants.data()));
            require(result.is_num() && std::fabs(result.as_num() - 6765.0) < 1e-12,
                    "Win64 direct-call fib(20) must be 6765");
            require(fake_vm.depth_budget == 100,
                    "the Win64 sequence must restore the depth budget");

            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(20.0);
            fake_vm.depth_budget = 18;
            require(win_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "Win64 budget exhaustion must return the deopt sentinel");
            require(fake_vm.depth_budget == 18,
                    "a Win64 budget deopt must leave the budget as it found it");
            require(fake_vm.stack_top == 0 && fake_vm.resume_ip == 7,
                    "a Win64 budget deopt must unwind the value stack and set "
                    "the resume slot");

            regs.assign(fib.max_regs, Value::nil());
            regs[0] = Value(true);
            fake_vm.depth_budget = 100;
            require(win_function(fake_vm_as_jit, regs.data(), fib.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "the Win64 entry guard must reject a non-numeric argument");

            // The tag guard is chosen from the live global at compile time,
            // so RATE must hold a number again before compiling.
            fake_vm.globals[1] = Value(4.0);
            SysVBaselineCompiler win_scale(scale, 0, scale.code.size(), scale.max_regs, 1);
            win_scale.set_link(&link, 1, /*callable=*/true);
            win_scale.set_abi(X64BaselineAbi::Win64);
            std::vector<uint8_t> win_scale_bytes = win_scale.compile_bytes();
            require(!win_scale_bytes.empty(), "Win64 numeric global read was rejected");
            ExecCode win_scale_code = ExecCode::from_bytes(win_scale_bytes);
            auto win_scale_function =
                reinterpret_cast<Win64NativeTestFn>(win_scale_code.ptr);
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            result = Value::from_bits(
                win_scale_function(fake_vm_as_jit, regs.data(), nullptr));
            require(result.is_num() && std::fabs(result.as_num() - 28.0) < 1e-12,
                    "Win64 scale(7) with RATE = 4 must be 28");
            fake_vm.globals[1] = Value::nil();
            regs.assign(scale.max_regs, Value::nil());
            regs[0] = Value(7.0);
            require(win_scale_function(fake_vm_as_jit, regs.data(), nullptr) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "a Win64 tag guard must reject a non-numeric global");
        }

        // ── v4: dynamic ops route to VM helpers, numeric ops stay inline ──
        // build(): r1 = []; r2 = 5; r3 = r2 + r2; r1[0] = r3; return r1
        JitChunk dyn;
        dyn.max_regs = 5;
        dyn.constants.emplace_back(5.0);
        dyn.constants.emplace_back(0.0);
        dyn.code.emplace_back(JitOp::MAKE_ARRAY, 1, 0, 0, 0);     // helper
        dyn.code.emplace_back(JitOp::LOAD_CONST, 2, 0, 0, 0);
        dyn.code.emplace_back(JitOp::ADD, 3, 2, 2);               // inline
        dyn.code.emplace_back(JitOp::LOAD_CONST, 4, 0, 0, 1);
        dyn.code.emplace_back(JitOp::INDEX_SET, 1, 4, 3);         // helper
        dyn.code.emplace_back(JitOp::RETURN_VAL, 1);
        {
            // Helpers are opt-in: without them the body still falls back.
            SysVBaselineCompiler no_helpers(dyn, 0, dyn.code.size(), dyn.max_regs, 0);
            no_helpers.set_link(&link, 2, /*callable=*/true);
            require(no_helpers.compile_bytes().empty(),
                    "dynamic ops must fall back while helpers are disabled");
        }
        SysVBaselineCompiler dyn_compiler(dyn, 0, dyn.code.size(), dyn.max_regs, 0);
        dyn_compiler.set_link(&link, 2, /*callable=*/true);
        dyn_compiler.set_allow_helpers(true);
        std::vector<uint8_t> dyn_bytes = dyn_compiler.compile_bytes();
        require(!dyn_bytes.empty(), "a helper-backed body was rejected");
        require(!dyn_compiler.pure,
                "INDEX_SET is a side effect: the body must not be direct-callable");
        // The helper stubs above carry the platform's native convention,
        // so only the body compiled for that convention may run them: the
        // System V body here, the Win64 body below on Windows.
#if !defined(_WIN32)
        ExecCode dyn_code = ExecCode::from_bytes(dyn_bytes);
        auto dyn_function = reinterpret_cast<SysVNativeTestFn>(dyn_code.ptr);
        {
            std::vector<Value> regs(dyn.max_regs, Value::nil());
            g_helper_calls.clear();
            g_helper_result = Value(42.0).raw_bits();
            fake_vm.resume_ip = -1;
            Value result = Value::from_bits(
                dyn_function(fake_vm_as_jit, regs.data(), dyn.constants.data()));
            require(g_helper_calls.size() == 2,
                    "MAKE_ARRAY and INDEX_SET must each call their helper once");
            require(std::string(g_helper_calls[0].name) == "sura_bl_make_array" &&
                        std::string(g_helper_calls[1].name) == "sura_bl_index_set",
                    "helpers must be called in bytecode order");
            for (const auto& call : g_helper_calls)
                require(call.vm == fake_vm_as_jit && call.R == regs.data(),
                        "every helper must receive the VM and the live frame");
            require(g_helper_calls[0].ins == &dyn.code[0] &&
                        g_helper_calls[1].ins == &dyn.code[4],
                    "every helper must receive its own instruction");
            require(regs[1].is_num() && regs[1].as_num() == 42.0,
                    "a value-returning helper's result must land in R[a]");
            require(regs[3].is_num() && regs[3].as_num() == 10.0,
                    "the numeric add between the helpers must run inline");
            require(result.is_num() && result.as_num() == 42.0,
                    "the body must return the helper-produced value");
            require(fake_vm.resume_ip == -1,
                    "a body that completes must not touch the resume slot");
        }
        {
            // A helper that raised returns the sentinel: the body stops at
            // that instruction, names it in the resume slot, and leaves the
            // pending exception alone for the VM to rethrow.
            std::vector<Value> regs(dyn.max_regs, Value::nil());
            g_helper_calls.clear();
            g_helper_result = SURA_JIT_DEOPT_SENTINEL;
            fake_vm.resume_ip = -1;
            fake_vm.exc_valid = 1;
            require(dyn_function(fake_vm_as_jit, regs.data(), dyn.constants.data()) ==
                        SURA_JIT_DEOPT_SENTINEL,
                    "a helper sentinel must propagate out of the body");
            require(g_helper_calls.size() == 1,
                    "nothing after a raising helper may run");
            require(fake_vm.resume_ip == 0,
                    "the resume slot must name the instruction that raised");
            require(fake_vm.exc_valid == 1,
                    "a helper deopt must keep the pending exception");
            fake_vm.exc_valid = 0;
        }
#endif
        {
            // Win64 entry: the same body with the shadow space around each
            // helper call.
            SysVBaselineCompiler win_dyn(dyn, 0, dyn.code.size(), dyn.max_regs, 0);
            win_dyn.set_link(&link, 2, /*callable=*/true);
            win_dyn.set_allow_helpers(true);
            win_dyn.set_abi(X64BaselineAbi::Win64);
            std::vector<uint8_t> win_dyn_bytes = win_dyn.compile_bytes();
            require(!win_dyn_bytes.empty(), "a Win64 helper-backed body was rejected");
#if defined(_WIN32)
            ExecCode win_dyn_code = ExecCode::from_bytes(win_dyn_bytes);
            auto win_dyn_function = reinterpret_cast<Win64NativeTestFn>(win_dyn_code.ptr);
            std::vector<Value> regs(dyn.max_regs, Value::nil());
            g_helper_calls.clear();
            g_helper_result = Value(7.0).raw_bits();
            Value result = Value::from_bits(
                win_dyn_function(fake_vm_as_jit, regs.data(), dyn.constants.data()));
            require(g_helper_calls.size() == 2 &&
                        g_helper_calls[0].vm == fake_vm_as_jit &&
                        g_helper_calls[0].R == regs.data() &&
                        g_helper_calls[0].ins == &dyn.code[0],
                    "Win64 helpers must receive vm, R and the instruction");
            require(result.is_num() && result.as_num() == 7.0 &&
                        regs[3].is_num() && regs[3].as_num() == 10.0,
                    "the Win64 helper body must mix helper results with inline math");
#endif
        }

        // ── v4: a pure body that reaches a helper takes value-stack frames ──
        // vfib(n): if n <= 1 return n; tmp = []; return vfib(n-1) + vfib(n-2)
        // MAKE_ARRAY is pure but allocates, so the collector may run under
        // any direct call into this body: every callee frame must be on the
        // VM value stack where the collector can see it.
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
        {
            SysVBaselineCompiler vfib_compiler(vfib, 0, vfib.code.size(), vfib.max_regs, 1);
            vfib_compiler.set_link(&link, 0, /*callable=*/true);
            vfib_compiler.set_allow_helpers(true);
#if defined(_WIN32)
            vfib_compiler.set_abi(X64BaselineAbi::Win64);
#endif
            std::vector<uint8_t> vfib_bytes = vfib_compiler.compile_bytes();
            require(!vfib_bytes.empty(), "a helper-using recursive body was rejected");
            require(vfib_compiler.pure && vfib_compiler.vm_frame,
                    "MAKE_ARRAY keeps a body pure but puts its frames on the value stack");
            ExecCode vfib_code = ExecCode::from_bytes(vfib_bytes);
#if defined(_WIN32)
            auto vfib_function = reinterpret_cast<Win64NativeTestFn>(vfib_code.ptr);
#else
            auto vfib_function = reinterpret_cast<SysVNativeTestFn>(vfib_code.ptr);
#endif
            {
                std::vector<Value> regs(vfib.max_regs, Value::nil());
                regs[0] = Value(15.0);
                fake_vm.depth_budget = 100;
                fake_vm.stack_top = 40;
                fake_vm.value_stack.assign(fake_vm.value_stack.size(), Value::nil());
                g_helper_calls.clear();
                g_helper_result = Value(42.0).raw_bits();
                Value result = Value::from_bits(
                    vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()));
                require(result.is_num() && std::fabs(result.as_num() - 610.0) < 1e-12,
                        "value-stack vfib(15) must be 610");
                require(fake_vm.depth_budget == 100 && fake_vm.stack_top == 40,
                        "every value-stack callee frame must be popped again");
                for (size_t i = 0; i < 40; ++i)
                    require(fake_vm.value_stack[i].is_nil(),
                            "value-stack frames must not touch slots below stack_top");
                // The first helper runs in the entry frame; every later
                // one runs in a callee frame carved from the value stack.
                require(!g_helper_calls.empty() && g_helper_calls[0].R == regs.data(),
                        "the entry frame's helper must see the entry registers");
                bool callee_frames_on_stack = g_helper_calls.size() > 1;
                for (size_t i = 1; i < g_helper_calls.size(); ++i) {
                    Value* base = fake_vm.value_stack.data();
                    if (g_helper_calls[i].R < base + 40 ||
                        g_helper_calls[i].R >= base + fake_vm.value_stack.size())
                        callee_frames_on_stack = false;
                }
                require(callee_frames_on_stack,
                        "a helper in a direct callee must see a value-stack frame");
                fake_vm.stack_top = 0;
            }
            {
                // A value stack too small for one more callee frame deopts
                // before anything is pushed: stack_top and budget read as
                // they were, and the resume slot names the CALL_FUNC.
                std::vector<Value> regs(vfib.max_regs, Value::nil());
                regs[0] = Value(15.0);
                fake_vm.depth_budget = 100;
                fake_vm.stack_top = fake_vm.value_stack.size() - vfib.max_regs + 1;
                fake_vm.resume_ip = -1;
                g_helper_result = Value(42.0).raw_bits();
                require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                            SURA_JIT_DEOPT_SENTINEL,
                        "a full value stack must return the deopt sentinel");
                require(fake_vm.stack_top == fake_vm.value_stack.size() - vfib.max_regs + 1,
                        "a capacity deopt must leave stack_top as it found it");
                require(fake_vm.depth_budget == 100,
                        "a capacity deopt must leave the budget as it found it");
                require(fake_vm.resume_ip == 8,
                        "a capacity deopt must resume at the first CALL_FUNC");
                fake_vm.stack_top = 0;
            }
            {
                // vfib(15) nests 14 direct calls: a budget of 13 deopts,
                // and every value-stack frame is popped on the way out.
                std::vector<Value> regs(vfib.max_regs, Value::nil());
                regs[0] = Value(15.0);
                fake_vm.depth_budget = 13;
                fake_vm.resume_ip = -1;
                require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                            SURA_JIT_DEOPT_SENTINEL,
                        "exhausting the budget under value-stack frames must deopt");
                require(fake_vm.depth_budget == 13 && fake_vm.stack_top == 0 &&
                            fake_vm.resume_ip == 8,
                        "a budget deopt must unwind every value-stack frame");
                fake_vm.depth_budget = 14;
                regs.assign(vfib.max_regs, Value::nil());
                regs[0] = Value(15.0);
                Value result = Value::from_bits(
                    vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()));
                require(result.is_num() && std::fabs(result.as_num() - 610.0) < 1e-12,
                        "a budget of exactly the recursion depth must succeed");
            }
            {
                // A helper that raised inside a callee: the sentinel walks
                // back up through every direct call, frames popped, budget
                // restored, and the entry frame names its own CALL_FUNC.
                std::vector<Value> regs(vfib.max_regs, Value::nil());
                regs[0] = Value(15.0);
                fake_vm.depth_budget = 100;
                fake_vm.resume_ip = -1;
                fake_vm.exc_valid = 1;
                g_helper_calls.clear();
                g_helper_result = SURA_JIT_DEOPT_SENTINEL;
                require(vfib_function(fake_vm_as_jit, regs.data(), vfib.constants.data()) ==
                            SURA_JIT_DEOPT_SENTINEL,
                        "a raising helper must propagate the sentinel");
                require(g_helper_calls.size() == 1 && fake_vm.resume_ip == 4 &&
                            fake_vm.exc_valid == 1 && fake_vm.stack_top == 0 &&
                            fake_vm.depth_budget == 100,
                        "the entry frame's own helper deopt keeps the exception and "
                        "names the raising instruction");
                fake_vm.exc_valid = 0;
                g_helper_result = Value(42.0).raw_bits();
            }
        }

        std::cout << "jit sysv baseline: PASS (division, six comparisons, NaN semantics, "
                     "guarded fallbacks, parameter guards, loops, global guards, "
                     "direct calls, VM helpers, hybrid frames, and the Win64 entry)\n";
#else
        std::cout << "jit sysv baseline: PASS (non-x86-64 compile-only target)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit sysv baseline FAILED: " << error.what() << "\n";
        return 1;
    }
}
