#include <cstdio>
struct PreMain { PreMain() { printf("DEBUG: PRE-MAIN OK\n"); fflush(stdout); } } premain_inst;
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include <sstream>

// Core headers
#include "lexer.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "typechecker.hpp"
#include "platform.hpp"
#include "jit.hpp"

// ================================================================
//  Sura(SURA) Unified Engine Entry v3.2
//  - TypeChecker integrated into pipeline
//  - --strict flag for strict type checking
//  - Cross-platform: Windows, Linux, macOS, ARM64
// ================================================================

// Run TypeChecker on AST, return number of errors found
static int run_typecheck(const SuraBlock* ast, bool strict_mode) {
    TypeChecker checker;
    int err_count = checker.check(ast);
    if (err_count > 0) {
        // In strict mode: print as errors
        // In normal mode: print as warnings
        checker.print_errors(!strict_mode);
    }
    return err_count;
}

int main(int argc, char* argv[]) {
int main(int argc, char* argv[]) {

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
    bool strict_mode   = false;
    std::string filename;
    std::string src;

    // ==== 1. Argument Parsing ====
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump" || arg == "-d") dump_bytecode = true;
        else if (arg == "--bench" || arg == "-b") bench_mode = true;
        else if (arg == "--repl" || arg == "-r") repl_mode = true;
        else if (arg == "--strict" || arg == "-s") strict_mode = true;
        else if (arg == "--lsp") {
            std::string req;
            while (std::getline(std::cin, req)) {
                std::cout << "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":["
                    << "{\"label\":\"is\",\"detail\":\"assignment (x is value)\"},"
                    << "{\"label\":\"if\",\"detail\":\"conditional (if cond then)\"},"
                    << "{\"label\":\"while\",\"detail\":\"loop (while cond do)\"},"
                    << "{\"label\":\"repeat\",\"detail\":\"repeat N times (repeat N do)\"},"
                    << "{\"label\":\"func\",\"detail\":\"function def (func name do)\"},"
                    << "{\"label\":\"class\",\"detail\":\"class def (class name extends parent)\"},"
                    << "{\"label\":\"break\",\"detail\":\"break loop\"},"
                    << "{\"label\":\"return\",\"detail\":\"return value\"},"
                    << "{\"label\":\"print\",\"detail\":\"output\"},"
                    << "{\"label\":\"use\",\"detail\":\"import library\"}"
                    << "]}" << std::endl;
            }
            return 0;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Sura(SURA) JIT Engine v3.2 (TypeChecker Integrated)\n"
                      << "Usage:\n"
                      << "  SuraEngine <file.sura>           Run file\n"
                      << "  SuraEngine --repl                Interactive REPL\n"
                      << "  SuraEngine --dump <file.sura>    Show bytecode\n"
                      << "  SuraEngine --bench <file.sura>   Benchmark\n"
                      << "  SuraEngine --strict <file.sura>  Strict type checking (errors stop execution)\n"
                      << "  SuraEngine --lsp                 Language Server mode\n";
            return 0;
        }
        else filename = arg;
    }

    // ==== 2. REPL Mode ====
    if (repl_mode || filename.empty()) {
        std::cout << "=== Sura(SURA) JIT REPL v3.2 ===\n"
                  << "Type 'exit' or 'quit' to exit.\n";
        if (strict_mode) std::cout << "[STRICT MODE] Type errors will stop execution.\n";
        std::cout << "\n";

        JitVM vm;

        while (true) {
            std::cout << "SURA> ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            std::string src = line;

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
                    
                    if (trimmed.empty() && depth > 0) break;
                }
            }

            try {
                Parser parser;
                auto ast = parser.parse_source(src);

                // TypeChecker: run before compilation
                int type_errors = run_typecheck(
                    static_cast<const SuraBlock*>(ast.get()), strict_mode);
                if (strict_mode && type_errors > 0) {
                    std::cerr << "\033[1;31m[strict] " << type_errors 
                              << " type error(s) found. Execution stopped.\033[0m\n";
                    continue;
                }

                JitCompiler compiler;
                JitChunk chunk = compiler.compile(ast.get());
                
                if (dump_bytecode) JitVM::dump(chunk);
                vm.run(chunk);
            } catch (const LexError& e) {
                report_error(src, e.line, e.what());
            } catch (const ParseError& e) {
                report_error(src, e.line, e.what());
            } catch (const JitThrow& e) {
                report_error(src, e.line, "[Runtime Error] " + e.message);
            } catch (const std::exception& e) {
                std::cerr << "\033[1;31m[Internal Error]\033[0m " << e.what() << "\n";
            }
        }
        return 0;
    }

    // ==== 3. File Execution (with optional benchmark) ====
    std::ifstream f(filename);
    if (!f) {
        std::cerr << "[Error] Cannot open file: " << filename << "\n";
        return 1;
    }
    src = std::string((std::istreambuf_iterator<char>(f)), {});
    f.close();

    try {
        // [Parse]
        auto parse_start = std::chrono::high_resolution_clock::now();
        Parser parser;
        auto ast = parser.parse_source(src);
        auto parse_end = std::chrono::high_resolution_clock::now();

        // [TypeCheck] - NEW: integrated into pipeline
        auto tc_start = std::chrono::high_resolution_clock::now();
        int type_errors = run_typecheck(
            static_cast<const SuraBlock*>(ast.get()), strict_mode);
        auto tc_end = std::chrono::high_resolution_clock::now();

        if (strict_mode && type_errors > 0) {
            std::cerr << "\033[1;31m[strict] " << type_errors 
                      << " type error(s) found. Execution stopped.\033[0m\n";
            return 1;
        }

        // [Compile]
        auto compile_start = std::chrono::high_resolution_clock::now();
        JitCompiler compiler;
        JitChunk chunk = compiler.compile(ast.get());
        auto compile_end = std::chrono::high_resolution_clock::now();

        if (dump_bytecode) {
            std::cout << "========== Bytecode Dump ==========\n";
            JitVM::dump(chunk);
            std::cout << "========== End Dump ==========\n\n";
        }

        // [Execute]
        auto exec_start = std::chrono::high_resolution_clock::now();
        JitVM vm;
        vm.run(chunk);
        auto exec_end = std::chrono::high_resolution_clock::now();

        // [Benchmark]
        if (bench_mode) {
            auto parse_us   = std::chrono::duration_cast<std::chrono::microseconds>(parse_end - parse_start).count();
            auto tc_us      = std::chrono::duration_cast<std::chrono::microseconds>(tc_end - tc_start).count();
            auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count();
            auto exec_us    = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();
            auto total_us   = parse_us + tc_us + compile_us + exec_us;

            std::cout << "\n=== JIT Benchmark Results ===\n"
                      << "  Parse:     " << parse_us   / 1000.0 << " ms\n"
                      << "  TypeCheck: " << tc_us      / 1000.0 << " ms\n"
                      << "  Compile:   " << compile_us / 1000.0 << " ms\n"
                      << "  Execute:   " << exec_us    / 1000.0 << " ms\n"
                      << "  ----------------------------\n"
                      << "  Total:     " << total_us   / 1000.0 << " ms\n"
                      << "  Bytecode:  " << chunk.code.size() << " instructions\n"
                      << "============================\n";
        }

    } catch (const LexError& e) {
        report_error(src, e.line, e.what());
    } catch (const ParseError& e) {
        report_error(src, e.line, e.what());
    } catch (const JitThrow& e) {
        report_error(src, e.line, "[Runtime Error] " + e.message);
    } catch (const std::exception& e) {
        std::cerr << "\033[1;31m[Internal Error]\033[0m " << e.what() << "\n";
        return 1;
    }

    return 0;
}
