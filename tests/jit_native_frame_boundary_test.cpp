#include "../jit_vm.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

JitChunk scalar_rejecting_method_call_chunk(JitFuncInfo& function) {
    JitChunk chunk;
    const int method_name = chunk.add_string("touch");

    // An executable-default marker makes scalar_process_range reject the
    // plan before the METHOD_CALL. The callable still receives native scratch
    // metadata and a reuse flag, which is the boundary that previously
    // published one register too few.
    chunk.code.emplace_back(JitOp::NOP, 9, 0, 3,
                            JIT_DEFAULT_PROLOGUE_MAGIC, -1, 1);
    chunk.code.emplace_back(JitOp::METHOD_CALL, 4, 0, 0,
                            0, method_name, 2);
    chunk.code.emplace_back(JitOp::RETURN_VAL, 4, 0, 0, 0, -1, 3);

    function.name = "frame_boundary";
    function.params = {"record", "value", "mode"};
    function.entry_ip = 0;
    function.end_ip = chunk.code.size();
    function.max_regs = 171;
    function.native_scratch_base = 11;
    function.native_scratch_regs = 160;
    function.native_reuse_flag_reg = 11;
    chunk.func_table.push_back(function);
    chunk.max_regs = function.max_regs;
    return chunk;
}

} // namespace

int main() {
    try {
        JitFuncInfo function;
        JitChunk chunk = scalar_rejecting_method_call_chunk(function);

        require(jit_range_has_method_call(
                    chunk, function.entry_ip, function.end_ip),
                "fixture is not a method-call callable");
        require(jit_default_arg_count_reg(
                    chunk, function.entry_ip, 0, function.params.size()) == 9,
                "fixture argc marker is invalid");

        const uint32_t required =
            sura_jit_required_function_frame_regs(chunk, function);
        require(required == 12,
                "required extent must include reuse_flag_reg + 1");
        require(required > function.native_scratch_base,
                "fixture does not reproduce the old truncated extent");

        NativeCompiler compiler(chunk, function);
        std::unique_ptr<NativeFunc> native = compiler.compile();
        require(native != nullptr,
                "scalar-rejecting METHOD_CALL callable did not compile");
        require(!native->scalarized,
                "fixture unexpectedly produced a scalar plan");
        require(native->frame_regs >= required,
                "published native frame excludes argc/reuse slots");

        constexpr uint64_t stack_capacity = uint64_t{1} << 17;
        const uint64_t last_valid_base = stack_capacity - required;
        require(last_valid_base + native->frame_regs <= stack_capacity,
                "last valid frame does not fit at STACK_CAPACITY");
        require(last_valid_base + 1 + native->frame_regs > stack_capacity,
                "one-past boundary was not rejected by extent arithmetic");

        std::cout << "jit native frame boundary: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit native frame boundary FAILED: "
                  << error.what() << "\n";
        return 1;
    }
}
