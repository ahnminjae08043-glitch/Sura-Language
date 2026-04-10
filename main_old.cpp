#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include <sstream>

// 코어 모듈
#include "lexer.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "platform.hpp"
#include "jit.hpp"

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�라(SURA) ?�합 진입??(Unified Engine Entry)
//  - ?�전 JIT & ?��??�터 기반 가??머신(VM)
//  - 지??기능: ?�일 ?�행, REPL, 벤치마크, 바이?�코???�프, LSP
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

int main(int argc, char* argv[]) {
    sura_init_console();

    auto report_error = [&](const std::string& source, int line, const std::string& msg) {
        std::cerr << "\n\033[1;31m" << msg << "\033[0m\n";
        std::stringstream ss(source);
        std::string l;
        int cur = 1;
        while (std::getline(ss, l)) {
            if (cur == line) {
                std::string line_str = std::to_string(line);
                std::cerr << "\033[1;34m" << line_str << " |\033[0m " << l << "\n";
                std::cerr << std::string(line_str.size(), ' ') << " \033[1;34m|\033[0m \033[1;33m^\033[0m\n";
                break;
            }
            cur++;
        }
        std::cerr << std::endl;
    };

    bool dump_bytecode = false;
    bool bench_mode    = false;
    bool repl_mode     = false;
    std::string filename;
    std::string src;

    // ?�?� 1. ?�션 ?�싱 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump" || arg == "-d") dump_bytecode = true;
        else if (arg == "--bench" || arg == "-b") bench_mode = true;
        else if (arg == "--repl" || arg == "-r") repl_mode = true;
        else if (arg == "--lsp") {
            // LSP (Language Server Protocol) 지??(?�순 메�??�이???�답)
            std::string req;
            while (std::getline(std::cin, req)) {
                std::cout << "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":["
                    << "{\"label\":\"is\",\"detail\":\"변???�??(x is ?�현??\"},"
                    << "{\"label\":\"if\",\"detail\":\"조건�?(if 조건 then)\"},"
                    << "{\"label\":\"while\",\"detail\":\"반복�?(while 조건 do)\"},"
                    << "{\"label\":\"repeat\",\"detail\":\"N�?반복 (repeat N do)\"},"
                    << "{\"label\":\"func\",\"detail\":\"?�수 ?�의 (func ?�름 do)\"},"
                    << "{\"label\":\"class\",\"detail\":\"?�래???�의 (class ?�름 parent 부�?\"},"
                    << "{\"label\":\"break\",\"detail\":\"반복 ?�출\"},"
                    << "{\"label\":\"return\",\"detail\":\"?�수 반환\"},"
                    << "{\"label\":\"print\",\"detail\":\"출력\"},"
                    << "{\"label\":\"use\",\"detail\":\"?�이브러�?불러?�기\"}"
                    << "]}" << std::endl;
            }
            return 0;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "?�라(SURA) JIT ?�진 v3.1 (Error Reporting Enhanced)\n"
                      << "?�용�?\n"
                      << "  SuraEngine <?�일.sura>           ?�행\n"
                      << "  SuraEngine --repl                ?�?�형(REPL) 모드 진입\n"
                      << "  SuraEngine --dump <?�일.sura>    바이?�코??구조 ?�인\n"
                      << "  SuraEngine --bench <?�일.sura>   ?�능 벤치마크 모드\n"
                      << "  SuraEngine --lsp                 ?�디???�동?�성 ?�버 모드\n";
            return 0;
        }
        else filename = arg;
    }

    // ?�?� 2. REPL ?�?�형 모드 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    if (repl_mode || filename.empty()) {
        std::cout << "=== ?�라(SURA) JIT REPL v3.1 ===\n"
                  << "명령?��? ?�력?�세?? ?�답 ?�안 �??�각???�류 보고가 ?�성?�되?�습?�다.\n\n";

        JitVM vm; // REPL ?�션 ?�내 ?�역 ?�태 ?��?�??�한 ?�합 VM (?��??�터 기반)

        while (true) {
            std::cout << "SURA> ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            std::string src = line;

            // ?�러 �?블록)???�구?�는 문법?��? 간단??체크
            auto needs_block = [](const std::string& s) {
                if (s.find("if ") == 0 || s.find("while ") == 0 ||
                    s.find("repeat ") == 0 || s.find("for ") == 0 ||
                    s.find("func ") == 0 || s.find("class ") == 0 ||
                    s.find("try") == 0) return true;
                return false;
            };

            if (needs_block(line)) {
                int depth = 1;
                while (depth > 0) {
                    std::cout << "...> ";
                    std::getline(std::cin, line);
                    src += "\n" + line;
                    
                    std::string trimmed = line;
                    size_t fs = trimmed.find_first_not_of(" \t");
                    if (fs != std::string::npos) trimmed = trimmed.substr(fs);
                    
                    if (trimmed == "end") depth--;
                    else if (needs_block(trimmed)) depth++;
                    
                    // 만약 ?��?�?빠져?��??�면 �?�?                    if (trimmed.empty() && depth > 0) break;
                }
            }

            try {
                Parser parser;
                auto ast = parser.parse_source(src);
                JitCompiler compiler;
                JitChunk chunk = compiler.compile(ast.get());
                
                if (dump_bytecode) JitVM::dump(chunk);
                vm.run(chunk);
            } catch (const LexError& e) {
                report_error(src, e.line, e.what());
            } catch (const ParseError& e) {
                report_error(src, e.line, e.what());
            } catch (const JitThrow& e) {
                report_error(src, e.line, "[?�행 ?�간 ?�류] " + e.message);
            } catch (const std::exception& e) {
                std::cerr << "\033[1;31m[기�? ?�류]\033[0m " << e.what() << "\n";
            }
        }
        return 0;
    }

    // ?�?� 3. ?�일 ?�일 ?�행 모드 (?�일 ?�프 �?벤치마크 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    std::ifstream f(filename);
    if (!f) {
        std::cerr << "[?�류] ?�일???????�습?�다: " << filename << "\n";
        return 1;
    }
    src = std::string((std::istreambuf_iterator<char>(f)), {});
    f.close();

    try {
        // [?�싱]
        auto parse_start = std::chrono::high_resolution_clock::now();
        Parser parser;
        auto ast = parser.parse_source(src);
        auto parse_end = std::chrono::high_resolution_clock::now();

        // [최적??컴파??
        auto compile_start = std::chrono::high_resolution_clock::now();
        JitCompiler compiler;
        JitChunk chunk = compiler.compile(ast.get());
        auto compile_end = std::chrono::high_resolution_clock::now();

        if (dump_bytecode) {
            std::cout << "========== 바이?�코???�프 ?�작 ==========\n";
            JitVM::dump(chunk);
            std::cout << "========== 바이?�코???�프 종료 ==========\n\n";
        }

        // [?�행]
        auto exec_start = std::chrono::high_resolution_clock::now();
        JitVM vm;
        vm.run(chunk);
        auto exec_end = std::chrono::high_resolution_clock::now();

        // [벤치마크 결과]
        if (bench_mode) {
            auto parse_us   = std::chrono::duration_cast<std::chrono::microseconds>(parse_end - parse_start).count();
            auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count();
            auto exec_us    = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();
            auto total_us   = parse_us + compile_us + exec_us;

            std::cout << "\n?�═ JIT 벤치마크 처리 ?�계 ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═\n"
                      << "  ?�싱:    " << parse_us   / 1000.0 << " ms\n"
                      << "  컴파??  " << compile_us / 1000.0 << " ms\n"
                      << "  ?�행:    " << exec_us    / 1000.0 << " ms\n"
                      << "  ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�\n"
                      << "  ?�계:    " << total_us   / 1000.0 << " ms\n"
                      << "  바이?�코?? " << chunk.code.size() << " 명령??n"
                      << "?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═\n";
        }

    } catch (const LexError& e) {
        report_error(src, e.line, e.what());
    } catch (const ParseError& e) {
        report_error(src, e.line, e.what());
    } catch (const JitThrow& e) {
        report_error(src, e.line, "[?�행 ?�간 ?�류] " + e.message);
    } catch (const std::exception& e) {
        std::cerr << "\033[1;31m[기�? ?�류]\033[0m " << e.what() << "\n";
        return 1;
    }

    return 0;
}
