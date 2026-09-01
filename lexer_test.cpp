#include <iostream>
#include "lexer.hpp"

int main() {
    SetConsoleOutputCP(65001);

    std::string code = R"(
// 수라 렉서 테스트
x is 10
y is 20
합계 is x + y

if 합계 >= 25 then
    print "크다"
else
    print "작다"
end

while x > 0 do
    x - 1
end

func 인사 do
    print "안녕하세요!"
end

random 1 ~ 100 결과
print 결과
)";

    Lexer lexer;
    try {
        auto tokens = lexer.tokenize(code);
        Lexer::dump(tokens);
    } catch (const LexError& e) {
        std::cerr << e.what() << "\n";
    }
    return 0;
}
