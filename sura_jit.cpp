// ════════════════════════════════════════════════════════════════════
//  수라(SURA) JIT 컴파일러 — 독립 실행기
//  SFML 없이 순수 JIT 바이트코드 컴파일 + VM 실행
//
//  사용법:
//    sura_jit <파일.sura>            — JIT 컴파일 & 실행
//    sura_jit --dump <파일.sura>     — 바이트코드 덤프 후 실행
//    sura_jit --repl                 — JIT REPL 모드
//    sura_jit --bench <파일.sura>    — 벤치마크 (실행 시간 측정)
// ════════════════════════════════════════════════════════════════════

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>

#include "lexer.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "platform.hpp"
#include "jit.hpp"

int main(int argc, char* argv[]) {
    sura_init_console();

    bool dump_bytecode = false;
    bool bench_mode    = false;
    bool repl_mode     = false;
    std::string filename;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump" || arg == "-d")      dump_bytecode = true;
        else if (arg == "--bench" || arg == "-b") bench_mode = true;
        else if (arg == "--repl" || arg == "-r")  repl_mode = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "수라 JIT 컴파일러 v1.0\n"
                      << "사용법:\n"
                      << "  sura_jit <파일.sura>           JIT 컴파일 & 실행\n"
                      << "  sura_jit --dump <파일.sura>    바이트코드 덤프 후 실행\n"
                      << "  sura_jit --bench <파일.sura>   벤치마크 모드\n"
                      << "  sura_jit --repl                JIT REPL 모드\n";
            return 0;
        }
        else filename = arg;
    }

    // ── REPL 모드 ────────────────────────────────────────────────────
    if (repl_mode || filename.empty()) {
        std::cout << "=== 수라(SURA) JIT REPL v1.0 ===\n"
                  << "JIT 바이트코드 컴파일러 + VM\n"
                  << "종료: exit\n\n";

        // REPL용 영속 VM
        JitVM vm;

        while (true) {
            std::cout << "JIT> ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            // 여러 줄 입력 지원 (end로 끝나는 블록)
            std::string src = line;
            // 간단한 블록 감지: if/while/repeat/for/func/class/try 로 시작
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
                    // 간단한 깊이 추적
                    std::string trimmed = line;
                    size_t fs = trimmed.find_first_not_of(" \t");
                    if (fs != std::string::npos) trimmed = trimmed.substr(fs);
                    if (trimmed == "end") depth--;
                    else if (trimmed.find("if ") == 0 || trimmed.find("while ") == 0 ||
                             trimmed.find("repeat ") == 0 || trimmed.find("for ") == 0 ||
                             trimmed.find("func ") == 0 || trimmed.find("class ") == 0 ||
                             trimmed.find("try") == 0) depth++;
                }
            }

            try {
                Parser p;
                auto ast = p.parse_source(src);
                JitCompiler compiler;
                JitChunk chunk = compiler.compile(ast.get());
                if (dump_bytecode) JitVM::dump(chunk);
                vm.run(chunk);
            } catch (std::exception& e) {
                std::cerr << "\033[1;31m오류:\033[0m " << e.what() << "\n";
            }
        }
        return 0;
    }

    // ── 파일 실행 모드 ───────────────────────────────────────────────
    std::ifstream f(filename);
    if (!f) {
        std::cerr << "[JIT] 파일을 열 수 없습니다: " << filename << "\n";
        return 1;
    }
    std::string src((std::istreambuf_iterator<char>(f)), {});
    f.close();

    try {
        // 파싱
        auto parse_start = std::chrono::high_resolution_clock::now();
        Parser parser;
        auto ast = parser.parse_source(src);
        auto parse_end = std::chrono::high_resolution_clock::now();

        // 컴파일
        auto compile_start = std::chrono::high_resolution_clock::now();
        JitCompiler compiler;
        JitChunk chunk = compiler.compile(ast.get());
        auto compile_end = std::chrono::high_resolution_clock::now();

        if (dump_bytecode) JitVM::dump(chunk);

        // 실행
        auto exec_start = std::chrono::high_resolution_clock::now();
        JitVM vm;
        vm.run(chunk);
        auto exec_end = std::chrono::high_resolution_clock::now();

        if (bench_mode) {
            auto parse_us   = std::chrono::duration_cast<std::chrono::microseconds>(parse_end - parse_start).count();
            auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count();
            auto exec_us    = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();
            auto total_us   = parse_us + compile_us + exec_us;

            std::cout << "\n══ JIT 벤치마크 결과 ══════════════════════\n"
                      << "  파싱:    " << parse_us   / 1000.0 << " ms\n"
                      << "  컴파일:  " << compile_us / 1000.0 << " ms\n"
                      << "  실행:    " << exec_us    / 1000.0 << " ms\n"
                      << "  ────────────────────────────────────\n"
                      << "  합계:    " << total_us   / 1000.0 << " ms\n"
                      << "  바이트코드: " << chunk.code.size() << " 명령어\n"
                      << "  상수 풀:   " << chunk.constants.size() << " 항목\n"
                      << "  함수:     " << chunk.func_table.size() << " 개\n"
                      << "  클래스:   " << chunk.class_table.size() << " 개\n"
                      << "════════════════════════════════════════════\n";
        }
    } catch (std::exception& e) {
        std::cerr << "\033[1;31m[JIT 오류]\033[0m " << e.what() << "\n";
        return 1;
    }

    return 0;
}
