#include "parser.hpp"
#include "typechecker.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void fail(const std::string& message) {
    std::cerr << "[FAIL] " << message << "\n";
    std::exit(1);
}

std::unique_ptr<SuraBlock> parse(const std::string& source) {
    Parser parser;
    return parser.parse_source(source);
}

bool has_code(const TypeChecker& checker, const std::string& code) {
    for (const auto& error : checker.get_errors()) {
        if (error.message.find("[" + code + "]") != std::string::npos) return true;
    }
    return false;
}

void expect_clean(const std::string& name, const std::string& source) {
    auto program = parse(source);
    TypeChecker checker;
    if (checker.check(program.get()) != 0) {
        checker.print_errors();
        fail(name + " unexpectedly produced type errors");
    }
}

void expect_code(const std::string& name, const std::string& source,
                 const std::string& code) {
    auto program = parse(source);
    TypeChecker checker;
    checker.check(program.get());
    if (!has_code(checker, code)) {
        checker.print_errors();
        fail(name + " did not produce " + code);
    }
}

} // namespace

int main() {
    {
        auto program = parse("item: Widget is nil\n");
        if (program->body.size() != 1 || program->body[0]->kind != NK::ASSIGN)
            fail("custom type fixture did not parse as an assignment");
        auto* assignment = static_cast<const AssignStmt*>(program->body[0].get());
        if (assignment->type_annot.kind != SType::CLASS ||
            assignment->type_annot.class_name != "Widget") {
            fail("custom type annotation was not represented as SType::CLASS");
        }
    }

    {
        auto program = parse("count: 숫자 is 1\n");
        auto* assignment = static_cast<const AssignStmt*>(program->body[0].get());
        if (assignment->type_annot.kind != SType::NUMBER)
            fail("Korean built-in type alias was not preserved");
    }

    expect_clean("forward call with a default argument", R"SURA(
result: number is add_later(2)
func add_later(value: number, delta: number is 1) -> number do
    return value + delta
end
)SURA");

    expect_clean("mutual recursion", R"SURA(
func even(value: number) -> bool do
    if value == 0 then
        return true
    end
    return odd(value - 1)
end
func odd(value: number) -> bool do
    if value == 0 then
        return false
    end
    return even(value - 1)
end
answer: bool is even(4)
)SURA");

    expect_clean("gradual any compatibility", R"SURA(
func passthrough(value) do
    return value
end
result is passthrough(unknown_value)
)SURA");

    expect_clean("dynamic variable can change type", R"SURA(
value is 1
value is "now a string"
)SURA");

    expect_clean("class annotation and subclass value", R"SURA(
class Animal do
end
class Dog extends Animal do
end
pet: Animal is new Dog()
)SURA");

    expect_code("too few arguments", R"SURA(
use_number()
func use_number(value: number) -> number do
    return value
end
)SURA", "E203");

    expect_code("too many arguments", R"SURA(
func use_number(value: number) -> number do
    return value
end
use_number(1, 2)
)SURA", "E203");

    expect_code("forward argument type", R"SURA(
result is use_number("not a number")
func use_number(value: number) -> number do
    return value
end
)SURA", "E204");

    expect_code("nested call argument type", R"SURA(
func use_number(value: number) -> number do
    return value
end
print(use_number("nested mismatch"))
)SURA", "E204");

    expect_clean("runtime truthiness conditions", R"SURA(
if 42 then
    print("truthy")
end
if "text" and [1] then
    print("truthy collections")
end
value is not {}
)SURA");

    expect_clean("numeric spelling aliases", R"SURA(
func scale(value: int, factor: float) -> double do
    return value * factor
end
result: number is scale(2, 3)
)SURA");

    expect_clean("nullable annotations and nil-safe iteration", R"SURA(
func maybe_number() -> number do
    return nil
end
items: array is nil
for item in items do
    print(item)
end
)SURA");

    expect_clean("string concatenation stringifies values", R"SURA(
label: string is "count=" + 3
label += true
)SURA");

    expect_clean("array addition concatenates arrays", R"SURA(
numbers: array is [1, 2] + [3, 4]
labels: array is ["su"] + ["ra"]
)SURA");

    expect_code("return type", R"SURA(
func wrong_return() -> number do
    return "wrong"
end
)SURA", "E205");

    expect_code("default argument type", R"SURA(
func wrong_default(value: number is "wrong") -> number do
    return value
end
)SURA", "E206");

    expect_code("typed reassignment", R"SURA(
count: number is 1
count is "wrong"
)SURA", "E200");

    expect_code("ordering requires numbers", R"SURA(
ordered is "left" < "right"
)SURA", "E201");

    expect_clean("numeric match range", R"SURA(
when 7 do
    in 1 to 10 then print("inside")
    else then print("outside")
end
)SURA");

    expect_code("match range subject requires a number", R"SURA(
when "seven" do
    in 1 to 10 then print("inside")
end
)SURA", "E210");

    expect_code("match range bounds require numbers", R"SURA(
when 7 do
    in "one" to "ten" then print("inside")
end
)SURA", "E210");

    std::cout << "[PASS] typechecker hardening tests\n";
    return 0;
}
