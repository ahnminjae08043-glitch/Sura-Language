#ifndef SURA_FREESTANDING_IMPORT_H
#define SURA_FREESTANDING_IMPORT_H

#include "ast.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace SuraFreestandingImport {

struct State {
    std::unordered_set<std::string> imported;
    std::unordered_set<std::string> active;
    bool legacy_command_syntax = true;
};

inline std::filesystem::path utf8_path(const std::string& path) {
#ifdef _WIN32
    return std::filesystem::u8path(path);
#else
    return std::filesystem::path(path);
#endif
}

inline std::filesystem::path normalize(
    const std::filesystem::path& importing_file,
    const std::string& requested) {
    namespace fs = std::filesystem;
    fs::path path = utf8_path(requested);
    if (path.is_relative()) path = importing_file.parent_path() / path;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec && !canonical.empty()) return canonical;
    ec.clear();
    fs::path absolute = fs::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

inline std::string key(const std::filesystem::path& path) {
    std::string value = path.generic_u8string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
#endif
    return value;
}

inline void expand_block(
    SuraBlock* source,
    const std::filesystem::path& source_file,
    SuraBlock* destination,
    State& state) {
    for (auto& statement : source->body) {
        if (!statement || statement->kind != NK::IMPORT) {
            destination->body.push_back(std::move(statement));
            continue;
        }

        const auto* import = static_cast<const ImportStmt*>(statement.get());
        const std::filesystem::path path =
            normalize(source_file, import->path);
        const std::string path_key = key(path);
        if (state.imported.count(path_key)) continue;
        if (state.active.count(path_key)) {
            throw std::runtime_error(
                "circular freestanding import detected at '" +
                path.generic_u8string() + "'");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "freestanding import file was not found: " +
                path.generic_u8string());
        }
        const std::string text(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());

        Parser parser;
        parser.set_legacy_command_syntax(state.legacy_command_syntax);
        BlockPtr imported_ast;
        try {
            imported_ast = parser.parse_source(text);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "freestanding import '" + path.generic_u8string() +
                "' failed: " + error.what());
        }
        if (!imported_ast) {
            throw std::runtime_error(
                "freestanding import did not produce a source block: " +
                path.generic_u8string());
        }

        state.active.insert(path_key);
        try {
            expand_block(imported_ast.get(), path, destination, state);
        } catch (...) {
            state.active.erase(path_key);
            throw;
        }
        state.active.erase(path_key);
        state.imported.insert(path_key);
    }
}

inline BlockPtr expand(
    BlockPtr root,
    const std::filesystem::path& root_file,
    bool legacy_command_syntax) {
    if (!root) {
        throw std::runtime_error(
            "freestanding source did not produce a source block");
    }
    auto expanded = std::make_unique<SuraBlock>(root->line);
    State state;
    state.legacy_command_syntax = legacy_command_syntax;

    const std::filesystem::path normalized_root =
        normalize(root_file.parent_path() / "_", root_file.filename().generic_u8string());
    const std::string root_key = key(normalized_root);
    state.active.insert(root_key);
    try {
        expand_block(root.get(), normalized_root, expanded.get(), state);
    } catch (...) {
        state.active.erase(root_key);
        throw;
    }
    state.active.erase(root_key);
    state.imported.insert(root_key);
    return expanded;
}

} // namespace SuraFreestandingImport

#endif
