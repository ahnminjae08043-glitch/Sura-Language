#include "../jit.hpp"
#include "../parser.hpp"
#include "../value.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::string source =
        "func install_then_throw() do\n"
        "  captured is 41\n"
        "  local_saved is func() do\n"
        "    return captured\n"
        "  end\n"
        "  box[\"saved\"] is local_saved\n"
        "  throw \"stop\"\n"
        "end\n"
        "install_then_throw()\n";

    Parser parser;
    auto ast = parser.parse_source(source);
    JitCompiler compiler;
    JitChunk chunk = compiler.compile(ast.get());

    Value box = Value::make_dict();
    GCNativeRoot root(box.as_obj());
    std::vector<Value> preload(chunk.global_names.size(), Value::nil());
    bool found_box = false;
    for (size_t i = 0; i < chunk.global_names.size(); ++i) {
        if (chunk.global_names[i] == "box") {
            preload[i] = box;
            found_box = true;
        }
    }
    if (!found_box) {
        std::cerr << "compiled chunk has no box global\n";
        return 1;
    }

    JitVM vm;
    vm.inject_globals(preload);
    bool threw = false;
    try {
        vm.run(chunk);
    } catch (const JitThrow& error) {
        threw = error.message == "stop";
    }
    if (!threw) {
        std::cerr << "expected uncaught Sura throw\n";
        return 2;
    }

    auto saved_it = box.as_dict()->elements.find("saved");
    if (saved_it == box.as_dict()->elements.end() || !saved_it->second.is_closure()) {
        std::cerr << "escaped closure was not stored\n";
        return 3;
    }
    GCClosure* closure = saved_it->second.as_closure();
    if (closure->upvalues.size() != 1 || !closure->upvalues[0]) {
        std::cerr << "escaped closure has no captured upvalue\n";
        return 4;
    }
    GCUpvalue* upvalue = closure->upvalues[0];
    if (upvalue->location != nullptr || std::fabs(upvalue->closed.to_num() - 41.0) > 0.0001) {
        std::cerr << "uncaught run left an open or corrupted upvalue\n";
        return 5;
    }

    std::cout << "jit uncaught upvalue cleanup: PASS\n";
    return 0;
}
