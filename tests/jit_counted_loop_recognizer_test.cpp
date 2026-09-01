#include "../jit_compiler.hpp"
#include "../jit_native.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

JitChunk canonical_loop(const std::string& function_name = "step") {
    JitChunk chunk;
    chunk.global_names = {function_name, "position", "velocity", "factor",
                          "unused", "counter"};
    const int step_name = chunk.add_string(function_name);
    chunk.constants.push_back(Value(100000.0));
    chunk.constants.push_back(Value(1.0));

    JitFuncInfo step;
    step.name = function_name;
    step.params = {"position", "velocity", "factor"};
    step.entry_ip = 1;
    step.end_ip = 2;
    step.max_regs = 8;
    chunk.func_table.push_back(step);

    chunk.emit(JitOp::JUMP, 0, 0, 0, 2);
    chunk.emit(JitOp::RETURN_VAL, 0);
    chunk.emit(JitOp::MAKE_LAMBDA, 0, 0, 0, 0);
    chunk.emit(JitOp::STORE_GLOBAL, 0, 0, 0, 0);
    chunk.emit(JitOp::LOAD_GLOBAL, 1, 0, 0, 5);
    chunk.emit(JitOp::LOAD_CONST, 2, 0, 0, 0);
    chunk.emit(JitOp::CMP_LT, 0, 1, 2);
    chunk.emit(JitOp::JUMP_IF_FALSE, 0, 0, 0, 19);
    chunk.emit(JitOp::LOAD_GLOBAL, 1, 0, 0, 0);
    chunk.emit(JitOp::LOAD_GLOBAL, 2, 0, 0, 1);
    chunk.emit(JitOp::LOAD_GLOBAL, 3, 0, 0, 2);
    chunk.emit(JitOp::LOAD_GLOBAL, 4, 0, 0, 3);
    chunk.emit(JitOp::CALL_FUNC, 0, 1, 2, 3, step_name);
    chunk.emit(JitOp::STORE_GLOBAL, 0, 0, 0, 1);
    chunk.emit(JitOp::LOAD_GLOBAL, 1, 0, 0, 5);
    chunk.emit(JitOp::LOAD_CONST, 2, 0, 0, 1);
    chunk.emit(JitOp::ADD, 0, 1, 2);
    chunk.emit(JitOp::STORE_GLOBAL, 0, 0, 0, 5);
    chunk.emit(JitOp::JUMP, 0, 0, 0, 4);
    chunk.emit(JitOp::HALT);
    chunk.max_regs = 5;
    return chunk;
}

JitChunk compile_source(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    Parser parser;
    auto ast = parser.parse_source(source);
    JitCompiler compiler;
    return compiler.compile(static_cast<const SuraBlock*>(ast.get()), path);
}

} // namespace

int main() {
    try {
        JitChunk chunk = canonical_loop();
        auto loops = jit_recognize_strict_counted_loops(chunk);
        require(loops.size() == 1, "canonical counted loop was not recognized");
        const JitStrictCountedLoop& loop = loops.front();
        require(loop.header_ip == 4 && loop.call_ip == 12 &&
                    loop.backedge_ip == 18 && loop.exit_ip == 19,
                "recognized instruction boundaries are wrong");
        require(loop.counter_global == 5 && loop.result_global == 1 &&
                    loop.function_global == 0 && loop.function_index == 0,
                "recognized global/function mapping is wrong");
        require(loop.argument_globals == std::vector<int>({1, 2, 3}),
                "recognized argument globals are wrong");

        JitFuncInfo main;
        main.name = "__main__";
        main.entry_ip = 0;
        main.end_ip = chunk.code.size();
        main.max_regs = chunk.max_regs;
        NativeCompiler compiler(chunk, main, true);
        require(compiler.strict_counted_loop_count() == 1,
                "alias-safe canonical loop was rejected by NativeCompiler");

        JitChunk renamed = canonical_loop("advance_position");
        JitFuncInfo renamed_main;
        renamed_main.name = "__main__";
        renamed_main.entry_ip = 0;
        renamed_main.end_ip = renamed.code.size();
        renamed_main.max_regs = renamed.max_regs;
        NativeCompiler renamed_compiler(renamed, renamed_main, true);
        require(renamed_compiler.strict_counted_loop_count() == 1,
                "structurally valid loop was tied to a benchmark function name");

        JitChunk wrong_exit = canonical_loop();
        wrong_exit.code[7].operand = 18;
        require(jit_recognize_strict_counted_loops(wrong_exit).empty(),
                "condition exit into the loop was accepted");

        JitChunk wrong_result = canonical_loop();
        wrong_result.code[13].operand = 2;
        require(jit_recognize_strict_counted_loops(wrong_result).empty(),
                "result stored away from the loop-carried input was accepted");

        JitChunk extra_branch = canonical_loop();
        extra_branch.code[10].op = JitOp::JUMP_IF_TRUE;
        require(jit_recognize_strict_counted_loops(extra_branch).empty(),
                "extra body branch was accepted");

        JitChunk negative_increment = canonical_loop();
        negative_increment.constants[1] = Value(-1.0);
        require(jit_recognize_strict_counted_loops(negative_increment).empty(),
                "negative increment was accepted");

        JitChunk captured_function = canonical_loop();
        captured_function.func_table[0].upvalues.push_back({true, 0});
        require(jit_recognize_strict_counted_loops(captured_function).empty(),
                "capturing callee was accepted");

        JitChunk ambiguous_function = canonical_loop();
        ambiguous_function.func_table.push_back(ambiguous_function.func_table[0]);
        require(jit_recognize_strict_counted_loops(ambiguous_function).empty(),
                "ambiguous function name was accepted");

        JitChunk vec2 = compile_source("bench_physics.sura");
        auto vec2_loops = jit_recognize_strict_counted_loops(vec2);
        require(vec2_loops.size() == 1 &&
                    vec2_loops[0].header_ip == 72 &&
                    vec2_loops[0].call_ip == 80 &&
                    vec2_loops[0].backedge_ip == 86,
                "Vec2 benchmark loop was not recognized exactly");
        JitFuncInfo vec2_main;
        vec2_main.name = "__main__";
        vec2_main.entry_ip = 0;
        vec2_main.end_ip = vec2.code.size();
        vec2_main.max_regs = vec2.max_regs;
        NativeCompiler vec2_compiler(vec2, vec2_main, true);
        require(vec2_compiler.strict_counted_loop_count() == 1,
                "Vec2 benchmark loop failed the runtime escape proof");

        JitChunk vec3 = compile_source("bench_physics3d.sura");
        auto vec3_loops = jit_recognize_strict_counted_loops(vec3);
        require(vec3_loops.size() == 1 &&
                    vec3_loops[0].header_ip == 151 &&
                    vec3_loops[0].call_ip == 161 &&
                    vec3_loops[0].backedge_ip == 167,
                "Vec3 benchmark loop was not recognized exactly");
        JitFuncInfo vec3_main;
        vec3_main.name = "__main__";
        vec3_main.entry_ip = 0;
        vec3_main.end_ip = vec3.code.size();
        vec3_main.max_regs = vec3.max_regs;
        NativeCompiler vec3_compiler(vec3, vec3_main, true);
        require(vec3_compiler.strict_counted_loop_count() == 1,
                "Vec3 benchmark loop failed the runtime escape proof");

        std::cout << "jit counted-loop recognizer: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jit counted-loop recognizer FAILED: "
                  << error.what() << "\n";
        return 1;
    }
}
