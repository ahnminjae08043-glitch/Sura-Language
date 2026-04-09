#include <iostream>
#include "interpreter.hpp"

// ════════════════════════════════════════════════════════════════════
//  수라(SURA) 엔진 v2.0
//  구조: Lexer → Parser → AST → Interpreter
// ════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    sura_init_console();
    srand((unsigned)time(0));

    Interpreter engine;
    bool use_bytecode = false;

    // ── 옵션 파싱 ─────────────────────────────────────────────────────
    int file_arg_index = -1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bytecode") {
            use_bytecode = true;
        } else if (arg == "--lsp") {
            // LSP 모드
            std::string req;
            while (std::getline(std::cin, req)) {
                std::cout << "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":["
                    << "{\"label\":\"is\",\"detail\":\"변수 대입 (x is 표현식)\"},"
                    << "{\"label\":\"if\",\"detail\":\"조건문 (if 조건 then)\"},"
                    << "{\"label\":\"while\",\"detail\":\"반복문 (while 조건 do)\"},"
                    << "{\"label\":\"repeat\",\"detail\":\"N번 반복 (repeat N do)\"},"
                    << "{\"label\":\"func\",\"detail\":\"함수 정의 (func 이름 do)\"},"
                    << "{\"label\":\"break\",\"detail\":\"반복 탈출\"},"
                    << "{\"label\":\"return\",\"detail\":\"함수 반환\"},"
                    << "{\"label\":\"print\",\"detail\":\"출력\"},"
                    << "{\"label\":\"input\",\"detail\":\"입력\"},"
                    << "{\"label\":\"random\",\"detail\":\"랜덤 (random 최소 ~ 최대 결과)\"},"
                    << "{\"label\":\"readkey\",\"detail\":\"키 입력\"},"
                    << "{\"label\":\"use\",\"detail\":\"라이브러리 불러오기\"}"
                    << "]}" << std::endl;
            }
            return 0;
        } else if (file_arg_index == -1) {
            file_arg_index = i;
        }
    }

    // ── 파일 실행 ─────────────────────────────────────────────────────
    if (file_arg_index != -1) {
        try {
            engine.run_file(argv[file_arg_index], use_bytecode);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── REPL ──────────────────────────────────────────────────────────
    std::cout << "=== 수라(SURA) 엔진 v2.0 ===\n";
    std::cout << "종료: exit | 여러 줄: 블록 입력 후 빈 줄\n\n";

    std::string buffer; // 누적 입력
    std::string line;

    while (true) {
        // 프롬프트: 첫 줄이면 SURA>, 이어지는 줄이면 ...>
        std::cout << (buffer.empty() ? "SURA> " : "...>  ");
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;

        // "SURA> " 또는 "...>  " 프롬프트가 복사·붙여넣기로 섞여 들어온 경우 제거
        if (line.rfind("SURA> ", 0) == 0) line = line.substr(6);
        else if (line.rfind("...>  ", 0) == 0) line = line.substr(6);

        if (line == "exit") break;

        // 빈 줄: 누적된 내용 실행 시도
        if (line.empty()) {
            if (buffer.empty()) continue;
            engine.run(buffer);
            buffer.clear();
            continue;
        }

        buffer += line + "\n";

        // 단순한 한 줄 문장인지 파싱 시도
        // "파일끝" 오류면 → 더 입력 필요, 그 외 오류/성공이면 → 실행
        try {
            Parser p;
            p.parse_source(buffer); // 파싱만 테스트
            // 파싱 성공 → 실행
            engine.run(buffer);
            buffer.clear();
        } catch (const std::exception& e) {
            std::string msg = e.what();
            // "파일끝" 이 오류 메시지에 있으면 입력이 아직 미완성
            if (msg.find("파일끝") != std::string::npos) {
                // 계속 입력받기
                continue;
            }
            // 그 외 오류 → parse_source가 이미 출력했으므로 버퍼만 초기화
            buffer.clear();
        }
    }
    return 0;
}
