#include "../freestanding_import.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>

static BlockPtr parse_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not open import test source: " + path.generic_string());
    }
    const std::string source(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    Parser parser;
    return parser.parse_source(source);
}

int main(int argc, char** argv) {
    const std::filesystem::path root =
        argc > 1
            ? std::filesystem::u8path(argv[1])
            : std::filesystem::path(
                  "examples/os/freestanding_features.sura");
    BlockPtr expanded = SuraFreestandingImport::expand(
        parse_file(root), root, true);

    std::unordered_map<std::string, size_t> functions;
    for (const auto& statement : expanded->body) {
        assert(statement && statement->kind != NK::IMPORT);
        if (statement->kind == NK::FUNC_DEF) {
            const auto* function =
                static_cast<const FuncDef*>(statement.get());
            ++functions[function->name];
        }
    }
    assert(functions["os_feature_seed"] == 1);
    assert(functions["os_feature_mix"] == 1);
    assert(functions["efi_main"] == 1);

    const std::filesystem::path cycle =
        std::filesystem::path(
            "tests/fixtures/freestanding_import/cycle_a.sura");
    bool rejected_cycle = false;
    try {
        (void)SuraFreestandingImport::expand(
            parse_file(cycle), cycle, true);
    } catch (const std::exception& error) {
        rejected_cycle =
            std::string(error.what()).find(
                "circular freestanding import") != std::string::npos;
    }
    assert(rejected_cycle);

    std::cout << "freestanding_import_unit: PASS ("
              << expanded->body.size() << " flattened statements)\n";
    return 0;
}
