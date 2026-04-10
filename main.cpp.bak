#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include <sstream>

// ì½”ì–´ ëª¨ë“ˆ
#include "lexer.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "platform.hpp"
#include "jit.hpp"

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?˜ë¼(SURA) ?µí•© ì§„ì…??(Unified Engine Entry)
//  - ?„ì „ JIT & ?ˆì??¤í„° ê¸°ë°˜ ê°€??ë¨¸ì‹ (VM)
//  - ì§€??ê¸°ëŠ¥: ?Œì¼ ?¤í–‰, REPL, ë²¤ì¹˜ë§ˆí¬, ë°”ì´?¸ì½”???¤í”„, LSP
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

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

    // ?€?€ 1. ?µì…˜ ?Œì‹± ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump" || arg == "-d") dump_bytecode = true;
        else if (arg == "--bench" || arg == "-b") bench_mode = true;
        else if (arg == "--repl" || arg == "-r") repl_mode = true;
        else if (arg == "--lsp") {
            // LSP (Language Server Protocol) ì§€??(?¨ìˆœ ë©”í??°ì´???‘ë‹µ)
            std::string req;
            while (std::getline(std::cin, req)) {
                std::cout << "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":["
                    << "{\"label\":\"is\",\"detail\":\"ë³€???€??(x is ?œí˜„??\"},"
                    << "{\"label\":\"if\",\"detail\":\"ì¡°ê±´ë¬?(if ì¡°ê±´ then)\"},"
                    << "{\"label\":\"while\",\"detail\":\"ë°˜ë³µë¬?(while ì¡°ê±´ do)\"},"
                    << "{\"label\":\"repeat\",\"detail\":\"Në²?ë°˜ë³µ (repeat N do)\"},"
                    << "{\"label\":\"func\",\"detail\":\"?¨ìˆ˜ ?•ì˜ (func ?´ë¦„ do)\"},"
                    << "{\"label\":\"class\",\"detail\":\"?´ë˜???•ì˜ (class ?´ë¦„ parent ë¶€ëª?\"},"
                    << "{\"label\":\"break\",\"detail\":\"ë°˜ë³µ ?ˆì¶œ\"},"
                    << "{\"label\":\"return\",\"detail\":\"?¨ìˆ˜ ë°˜í™˜\"},"
                    << "{\"label\":\"print\",\"detail\":\"ì¶œë ¥\"},"
                    << "{\"label\":\"use\",\"detail\":\"?¼ì´ë¸ŒëŸ¬ë¦?ë¶ˆëŸ¬?¤ê¸°\"}"
                    << "]}" << std::endl;
            }
            return 0;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "?˜ë¼(SURA) JIT ?”ì§„ v3.1 (Error Reporting Enhanced)\n"
                      << "?¬ìš©ë²?\n"
                      << "  SuraEngine <?Œì¼.sura>           ?¤í–‰\n"
                      << "  SuraEngine --repl                ?€?”í˜•(REPL) ëª¨ë“œ ì§„ì…\n"
                      << "  SuraEngine --dump <?Œì¼.sura>    ë°”ì´?¸ì½”??êµ¬ì¡° ?•ì¸\n"
                      << "  SuraEngine --bench <?Œì¼.sura>   ?±ëŠ¥ ë²¤ì¹˜ë§ˆí¬ ëª¨ë“œ\n"
                      << "  SuraEngine --lsp                 ?ë””???ë™?„ì„± ?œë²„ ëª¨ë“œ\n";
            return 0;
        }
        else filename = arg;
    }

    // ?€?€ 2. REPL ?€?”í˜• ëª¨ë“œ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    if (repl_mode || filename.empty()) {
        std::cout << "=== ?˜ë¼(SURA) JIT REPL v3.1 ===\n"
                  << "ëª…ë ¹?´ë? ?…ë ¥?˜ì„¸?? ?•ë‹µ ?œì•ˆ ë°??œê°???¤ë¥˜ ë³´ê³ ê°€ ?œì„±?”ë˜?ˆìŠµ?ˆë‹¤.\n\n";

        JitVM vm; // REPL ?¸ì…˜ ?´ë‚´ ?„ì—­ ?íƒœ ? ì?ë¥??„í•œ ?µí•© VM (?ˆì??¤í„° ê¸°ë°˜)

        while (true) {
            std::cout << "SURA> ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            std::string src = line;

            // ?¬ëŸ¬ ì¤?ë¸”ë¡)???”êµ¬?˜ëŠ” ë¬¸ë²•?¸ì? ê°„ë‹¨??ì²´í¬
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
                    
                    // ë§Œì•½ ?µì?ë¡?ë¹ ì ¸?˜ê??¤ë©´ ë¹?ì¤?                    if (trimmed.empty() && depth > 0) break;
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
                report_error(src, e.line, "[?¤í–‰ ?œê°„ ?¤ë¥˜] " + e.message);
            } catch (const std::exception& e) {
                std::cerr << "\033[1;31m[ê¸°í? ?¤ë¥˜]\033[0m " << e.what() << "\n";
            }
        }
        return 0;
    }

    // ?€?€ 3. ?¨ì¼ ?Œì¼ ?¤í–‰ ëª¨ë“œ (?Œì¼ ?¤í”„ ë°?ë²¤ì¹˜ë§ˆí¬ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    std::ifstream f(filename);
    if (!f) {
        std::cerr << "[?¤ë¥˜] ?Œì¼???????†ìŠµ?ˆë‹¤: " << filename << "\n";
        return 1;
    }
    src = std::string((std::istreambuf_iterator<char>(f)), {});
    f.close();

    try {
        // [?Œì‹±]
        auto parse_start = std::chrono::high_resolution_clock::now();
        Parser parser;
        auto ast = parser.parse_source(src);
        auto parse_end = std::chrono::high_resolution_clock::now();

        // [ìµœì ??ì»´íŒŒ??
        auto compile_start = std::chrono::high_resolution_clock::now();
        JitCompiler compiler;
        JitChunk chunk = compiler.compile(ast.get());
        auto compile_end = std::chrono::high_resolution_clock::now();

        if (dump_bytecode) {
            std::cout << "========== ë°”ì´?¸ì½”???¤í”„ ?œì‘ ==========\n";
            JitVM::dump(chunk);
            std::cout << "========== ë°”ì´?¸ì½”???¤í”„ ì¢…ë£Œ ==========\n\n";
        }

        // [?¤í–‰]
        auto exec_start = std::chrono::high_resolution_clock::now();
        JitVM vm;
        vm.run(chunk);
        auto exec_end = std::chrono::high_resolution_clock::now();

        // [ë²¤ì¹˜ë§ˆí¬ ê²°ê³¼]
        if (bench_mode) {
            auto parse_us   = std::chrono::duration_cast<std::chrono::microseconds>(parse_end - parse_start).count();
            auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(compile_end - compile_start).count();
            auto exec_us    = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();
            auto total_us   = parse_us + compile_us + exec_us;

            std::cout << "\n?â• JIT ë²¤ì¹˜ë§ˆí¬ ì²˜ë¦¬ ?µê³„ ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•\n"
                      << "  ?Œì‹±:    " << parse_us   / 1000.0 << " ms\n"
                      << "  ì»´íŒŒ??  " << compile_us / 1000.0 << " ms\n"
                      << "  ?¤í–‰:    " << exec_us    / 1000.0 << " ms\n"
                      << "  ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€\n"
                      << "  ?©ê³„:    " << total_us   / 1000.0 << " ms\n"
                      << "  ë°”ì´?¸ì½”?? " << chunk.code.size() << " ëª…ë ¹??n"
                      << "?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•\n";
        }

    } catch (const LexError& e) {
        report_error(src, e.line, e.what());
    } catch (const ParseError& e) {
        report_error(src, e.line, e.what());
    } catch (const JitThrow& e) {
        report_error(src, e.line, "[?¤í–‰ ?œê°„ ?¤ë¥˜] " + e.message);
    } catch (const std::exception& e) {
        std::cerr << "\033[1;31m[ê¸°í? ?¤ë¥˜]\033[0m " << e.what() << "\n";
        return 1;
    }

    return 0;
}
