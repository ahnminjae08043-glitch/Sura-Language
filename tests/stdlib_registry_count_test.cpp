#include "../stdlib.hpp"
#include <iostream>
#include <unordered_set>

int main() {
    const auto names = SuraStd::names();
    const std::unordered_set<std::string> unique(names.begin(), names.end());
    if (names.size() != unique.size()) {
        std::cerr << "duplicate keys escaped the stdlib registry map\n";
        return 1;
    }
    if (unique.size() != 661) {
        std::cerr << "expected 661 case-sensitive global builtin names, got "
                  << unique.size() << "\n";
        return 1;
    }
    std::cout << "stdlib registry names: " << unique.size() << "\n";
    return 0;
}
