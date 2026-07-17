#include "parser.hpp"

#include <cstddef>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void parse_or_reject(const std::string& source) {
    Parser parser;
    try {
        (void)parser.parse_source(source);
    } catch (const std::exception&) {
    }
}

void require_nesting_rejected(const std::string& name, const std::string& source) {
    Parser parser;
    try {
        (void)parser.parse_source(source);
    } catch (const ParseError& error) {
        require(std::string(error.what()).find("parser nesting limit exceeded") != std::string::npos,
                name + " failed for an unexpected reason: " + error.what());
        return;
    }
    throw std::runtime_error(name + " was not rejected by the nesting limit");
}

} // namespace

int main() {
    try {
        std::string nested_parentheses = "value is ";
        nested_parentheses.append(600, '(');
        nested_parentheses += "1";
        nested_parentheses.append(600, ')');
        nested_parentheses += "\n";
        require_nesting_rejected("deep parentheses", nested_parentheses);

        std::string nested_unary = "value is ";
        nested_unary.append(600, '-');
        nested_unary += "1\n";
        require_nesting_rejected("deep unary expression", nested_unary);

        std::string nested_blocks;
        for (size_t i = 0; i < 600; ++i) nested_blocks += "if true then\n";
        nested_blocks += "print(1)\n";
        for (size_t i = 0; i < 600; ++i) nested_blocks += "end\n";
        require_nesting_rejected("deep statement blocks", nested_blocks);

        std::string supported_depth = "value is ";
        supported_depth.append(64, '(');
        supported_depth += "1";
        supported_depth.append(64, ')');
        supported_depth += "\n";
        Parser supported_parser;
        (void)supported_parser.parse_source(supported_depth);

        const std::vector<std::string> seeds = {
            "name is 1\n",
            "func add(a, b) do\nreturn a + b\nend\n",
            "items is [1, 2, 3]\nprint(items[0])\n",
            "profile is {name: \"Sura\", version: 1}\n",
            "if true then\nprint(\"yes\")\nelse\nprint(\"no\")\nend\n",
            "class Box do\nvalue is 0\nfunc init(value) do\nself.value is value\nend\nend\n",
            "try\nthrow \"x\"\ncatch error\nprint(error)\nend\n",
        };
        const std::string mutation_bytes =
            "abcdefghijklmnopqrstuvwxyz0123456789()[]{}:+-*/%?.,|&\n\t\"";
        std::mt19937_64 random(0x5355524150415253ULL);
        size_t cases = 0;

        for (size_t iteration = 0; iteration < 5000; ++iteration) {
            std::string source = seeds[static_cast<size_t>(random() % seeds.size())];
            const size_t operation = static_cast<size_t>(random() % 5);
            if (operation == 0 && !source.empty()) {
                source.erase(source.begin() + static_cast<std::ptrdiff_t>(random() % source.size()));
            } else if (operation == 1) {
                const size_t offset = static_cast<size_t>(random() % (source.size() + 1));
                source.insert(source.begin() + static_cast<std::ptrdiff_t>(offset),
                              mutation_bytes[static_cast<size_t>(random() % mutation_bytes.size())]);
            } else if (operation == 2 && !source.empty()) {
                source[static_cast<size_t>(random() % source.size())] =
                    mutation_bytes[static_cast<size_t>(random() % mutation_bytes.size())];
            } else if (operation == 3 && !source.empty()) {
                source.resize(static_cast<size_t>(random() % source.size()));
            } else {
                const size_t count = static_cast<size_t>(random() % 96);
                source.clear();
                for (size_t i = 0; i < count; ++i) {
                    source.push_back(static_cast<char>(random() & 0xff));
                }
            }
            parse_or_reject(source);
            ++cases;
        }

        std::cout << "parser untrusted input: PASS (" << cases
                  << " fixed-seed mutations, 3 nesting limits)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "parser untrusted input FAILED: " << error.what() << "\n";
        return 1;
    }
}
