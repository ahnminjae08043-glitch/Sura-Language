#include <iostream>
#include <windows.h>
#include "parser.hpp"

int main() {
    SetConsoleOutputCP(65001);

    std::string code = R"(
// 파서 테스트 — 수라 v2.0

// 변수 대입
x is 10
이름 is "수라"

// 산술 표현식 (우선순위 테스트)
결과 is (x + 3) * 2
나머지 is 결과 % 3

// 조건문 (인라인)
if x > 5 then print "크다"

// 조건문 (블록 + else)
if x >= 10 then
    print "10 이상"
else
    print "10 미만"
end

// while 반복
while x > 0 do
    x - 1
end

// repeat 반복
repeat 3 do
    print "반복!"
end

// 함수 정의
func 인사 do
    print "안녕하세요!"
    return 1
end

// 내장 명령어
arr_push 목록 "항목1"
arr_len 목록 길이
random 1 ~ 100 행운
print 이름
)";

    Parser parser;
    try {
        auto ast = parser.parse_source(code);
        dump_ast(ast.get());
        std::cout << "\n파싱 성공! 문장 수: " << ast->body.size() << "\n";
    } catch (const ParseError& e) {
        std::cerr << e.what() << "\n";
    } catch (const LexError& e) {
        std::cerr << e.what() << "\n";
    }
    return 0;
}
