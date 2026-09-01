#pragma once
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "platform.hpp"
#include "jit.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

// ════════════════════════════════════════════════════════════════════
//  Interpreter (JIT VM Wrapper)
//  기존 AST 해석기를 제거하고 레지스터 기반 JIT 엔진으로 통합
// ════════════════════════════════════════════════════════════════════

class Interpreter {
    JitVM vm;

public:
    Interpreter() {}

    void run(const std::string& src, bool use_bytecode = true) {
        // use_bytecode 플래그는 무시됨 (이제 항상 JIT 바이트코드 실행)
        try {
            Parser parser;
            auto ast = parser.parse_source(src);
            
            JitCompiler compiler;
            JitChunk chunk = compiler.compile(ast.get());
            
            // JIT VM 실행
            vm.run(chunk);

        } catch (std::exception& e) {
            std::cerr << "[실행 오류] " << e.what() << "\n";
        }
    }

    void run_file(const std::string& filename, bool use_bytecode = true) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "[오류] 파일을 여는 데 실패했습니다: " << filename << "\n";
            return;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        
        run(buffer.str(), use_bytecode);
    }
};
