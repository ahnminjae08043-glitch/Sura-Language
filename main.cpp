#include <cstdio>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#ifdef _WIN32
  // Declare only what we need — do NOT include <windows.h>, which brings in
  // a flood of macros (TRUE, FALSE, IN, ERROR, NEW, ...) that collide with
  // our lexer's TT enum values.
  extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
  extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
  extern "C" __declspec(dllimport) wchar_t* __stdcall GetCommandLineW(void);
  extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(
      unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
#endif
// Force the Windows console to UTF-8 at program load so Korean output
// renders correctly without the user running `chcp 65001`.
struct PreMain {
    PreMain() {
#ifdef _WIN32
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
#endif
    }
} premain_inst;
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <initializer_list>

// Core headers
#include "lexer.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include "value.hpp"
#include "typechecker.hpp"
#include "platform.hpp"
#include "jit.hpp"
#include "bytecode_io.hpp"
#include "profiler.hpp"
#include "sura_version.hpp"
#include "os_target.hpp"

// ================================================================
//  Sura Language Runtime
//  - TypeChecker integrated into pipeline
//  - --strict flag for strict type checking
//  - Cross-platform: Windows, Linux, macOS, ARM64
// ================================================================

#ifdef _WIN32
static std::string wide_to_utf8(const wchar_t* text) {
    if (!text) return "";
    int needed = WideCharToMultiByte(65001, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return "";
    std::string out((size_t)needed - 1, '\0');
    WideCharToMultiByte(65001, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static std::vector<std::wstring> split_windows_command_line(const wchar_t* command_line) {
    std::vector<std::wstring> args;
    const wchar_t* p = command_line;
    while (p && *p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;

        std::wstring arg;
        bool in_quotes = false;
        int backslashes = 0;

        while (*p) {
            wchar_t ch = *p++;
            if (ch == L'\\') {
                ++backslashes;
                continue;
            }
            if (ch == L'"') {
                arg.append((size_t)(backslashes / 2), L'\\');
                if ((backslashes % 2) == 0) {
                    in_quotes = !in_quotes;
                } else {
                    arg.push_back(L'"');
                }
                backslashes = 0;
                continue;
            }
            if (!in_quotes && (ch == L' ' || ch == L'\t')) {
                arg.append((size_t)backslashes, L'\\');
                backslashes = 0;
                break;
            }
            arg.append((size_t)backslashes, L'\\');
            backslashes = 0;
            arg.push_back(ch);
        }

        arg.append((size_t)backslashes, L'\\');
        args.push_back(std::move(arg));
    }
    return args;
}
#endif

static std::vector<std::string> command_line_args(int argc, char* argv[]) {
#ifdef _WIN32
    std::vector<std::string> converted;
    for (const auto& arg : split_windows_command_line(GetCommandLineW())) {
        converted.push_back(wide_to_utf8(arg.c_str()));
    }
    if (!converted.empty()) return converted;
#endif

    std::vector<std::string> out;
    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) out.push_back(argv[i] ? argv[i] : "");
    return out;
}

static std::filesystem::path utf8_path(const std::string& path) {
#ifdef _WIN32
    return std::filesystem::u8path(path);
#else
    return std::filesystem::path(path);
#endif
}

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

struct StaticDiagnostic {
    int line = 0;
    std::string message;
};

static void collect_expr_refs(const Expr* expr, std::unordered_set<std::string>& refs);
static void collect_stmt_refs(const Stmt* stmt, std::unordered_set<std::string>& refs);

static void collect_expr_refs(const Expr* expr, std::unordered_set<std::string>& refs) {
    if (!expr) return;
    switch (expr->kind) {
        case NK::IDENT: {
            refs.insert(static_cast<const Ident*>(expr)->name);
            break;
        }
        case NK::BIN_OP: {
            auto* b = static_cast<const BinOp*>(expr);
            collect_expr_refs(b->left.get(), refs);
            collect_expr_refs(b->right.get(), refs);
            break;
        }
        case NK::UNARY_OP:
            collect_expr_refs(static_cast<const UnaryOp*>(expr)->operand.get(), refs);
            break;
        case NK::DOT_ACCESS:
            collect_expr_refs(static_cast<const DotAccess*>(expr)->obj.get(), refs);
            break;
        case NK::INDEX: {
            auto* idx = static_cast<const IndexExpr*>(expr);
            collect_expr_refs(idx->obj.get(), refs);
            collect_expr_refs(idx->key.get(), refs);
            break;
        }
        case NK::CALL:
            for (const auto& arg : static_cast<const CallExpr*>(expr)->args) collect_expr_refs(arg.get(), refs);
            break;
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(expr);
            collect_expr_refs(mc->obj.get(), refs);
            for (const auto& arg : mc->args) collect_expr_refs(arg.get(), refs);
            break;
        }
        case NK::SUPER_CALL:
            for (const auto& arg : static_cast<const SuperCallExpr*>(expr)->args) collect_expr_refs(arg.get(), refs);
            break;
        case NK::ARRAY_LIT:
            for (const auto& item : static_cast<const ArrayLit*>(expr)->elements) collect_expr_refs(item.get(), refs);
            break;
        case NK::DICT_LIT: {
            auto* dict = static_cast<const DictLit*>(expr);
            for (const auto& key : dict->key_order) {
                auto it = dict->pairs.find(key);
                if (it != dict->pairs.end()) collect_expr_refs(it->second.get(), refs);
            }
            break;
        }
        case NK::NEW_EXPR:
            for (const auto& arg : static_cast<const NewExpr*>(expr)->args) collect_expr_refs(arg.get(), refs);
            break;
        case NK::TERNARY: {
            auto* t = static_cast<const TernaryExpr*>(expr);
            collect_expr_refs(t->cond.get(), refs);
            collect_expr_refs(t->then_val.get(), refs);
            collect_expr_refs(t->else_val.get(), refs);
            break;
        }
        case NK::FUNC_EXPR:
            collect_stmt_refs(static_cast<const FuncExpr*>(expr)->body.get(), refs);
            break;
        case NK::STR_INTERP:
            for (const auto& part : static_cast<const StrInterp*>(expr)->parts) collect_expr_refs(part.get(), refs);
            break;
        default:
            break;
    }
}

static void collect_stmt_refs(const Stmt* stmt, std::unordered_set<std::string>& refs) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NK::SuraBlock:
            for (const auto& child : static_cast<const SuraBlock*>(stmt)->body) collect_stmt_refs(child.get(), refs);
            break;
        case NK::ASSIGN:
            collect_expr_refs(static_cast<const AssignStmt*>(stmt)->value.get(), refs);
            break;
        case NK::IN_PLACE:
            collect_expr_refs(static_cast<const InPlaceStmt*>(stmt)->value.get(), refs);
            refs.insert(static_cast<const InPlaceStmt*>(stmt)->name);
            break;
        case NK::DOT_ASSIGN:
            refs.insert(static_cast<const DotAssignStmt*>(stmt)->obj_name);
            collect_expr_refs(static_cast<const DotAssignStmt*>(stmt)->value.get(), refs);
            break;
        case NK::INDEX_ASSIGN:
            refs.insert(static_cast<const IndexAssignStmt*>(stmt)->name);
            collect_expr_refs(static_cast<const IndexAssignStmt*>(stmt)->key.get(), refs);
            collect_expr_refs(static_cast<const IndexAssignStmt*>(stmt)->value.get(), refs);
            break;
        case NK::IF: {
            auto* s = static_cast<const IfStmt*>(stmt);
            collect_expr_refs(s->cond.get(), refs);
            collect_stmt_refs(s->then_block.get(), refs);
            collect_stmt_refs(s->else_block.get(), refs);
            break;
        }
        case NK::WHILE: {
            auto* s = static_cast<const WhileStmt*>(stmt);
            collect_expr_refs(s->cond.get(), refs);
            collect_stmt_refs(s->body.get(), refs);
            break;
        }
        case NK::REPEAT: {
            auto* s = static_cast<const RepeatStmt*>(stmt);
            collect_expr_refs(s->count.get(), refs);
            collect_stmt_refs(s->body.get(), refs);
            break;
        }
        case NK::FOR: {
            auto* s = static_cast<const ForStmt*>(stmt);
            collect_expr_refs(s->from.get(), refs);
            collect_expr_refs(s->to.get(), refs);
            collect_expr_refs(s->step.get(), refs);
            collect_stmt_refs(s->body.get(), refs);
            break;
        }
        case NK::FOREACH: {
            auto* s = static_cast<const ForeachStmt*>(stmt);
            collect_expr_refs(s->collection.get(), refs);
            collect_stmt_refs(s->body.get(), refs);
            break;
        }
        case NK::RETURN:
            collect_expr_refs(static_cast<const ReturnStmt*>(stmt)->value.get(), refs);
            break;
        case NK::THROW:
            collect_expr_refs(static_cast<const ThrowStmt*>(stmt)->msg.get(), refs);
            break;
        case NK::TRY: {
            auto* s = static_cast<const TryStmt*>(stmt);
            collect_stmt_refs(s->try_block.get(), refs);
            collect_stmt_refs(s->catch_block.get(), refs);
            collect_stmt_refs(s->finally_block.get(), refs);
            break;
        }
        case NK::EXPR_STMT:
            collect_expr_refs(static_cast<const ExprStmt*>(stmt)->expr.get(), refs);
            break;
        case NK::MATCH: {
            auto* s = static_cast<const MatchStmt*>(stmt);
            collect_expr_refs(s->subject.get(), refs);
            for (const auto& arm : s->arms) {
                collect_expr_refs(arm.pattern.get(), refs);
                collect_expr_refs(arm.range_end.get(), refs);
                collect_stmt_refs(arm.body.get(), refs);
            }
            break;
        }
        default:
            break;
    }
}

static void collect_top_level_names(const SuraBlock* block, std::unordered_set<std::string>& globals) {
    if (!block) return;
    for (const auto& stmt : block->body) {
        if (!stmt) continue;
        switch (stmt->kind) {
            case NK::ASSIGN:
                globals.insert(static_cast<const AssignStmt*>(stmt.get())->name);
                break;
            case NK::IN_PLACE:
                globals.insert(static_cast<const InPlaceStmt*>(stmt.get())->name);
                break;
            case NK::FUNC_DEF:
                globals.insert(static_cast<const FuncDef*>(stmt.get())->name);
                break;
            case NK::CLASS_DEF:
                globals.insert(static_cast<const ClassDef*>(stmt.get())->name);
                break;
            case NK::ENUM_DEF:
                globals.insert(static_cast<const EnumDef*>(stmt.get())->name);
                break;
            default:
                break;
        }
    }
}

static void lint_function_shadowing_block(const SuraBlock* block,
                                          const std::unordered_set<std::string>& globals,
                                          std::unordered_set<std::string>& locals,
                                          std::unordered_set<std::string>& seen_refs,
                                          std::vector<StaticDiagnostic>& diagnostics);

static void lint_function_shadowing_stmt(const Stmt* stmt,
                                         const std::unordered_set<std::string>& globals,
                                         std::unordered_set<std::string>& locals,
                                         std::unordered_set<std::string>& seen_refs,
                                         std::vector<StaticDiagnostic>& diagnostics) {
    if (!stmt) return;
    switch (stmt->kind) {
        case NK::SuraBlock:
            lint_function_shadowing_block(static_cast<const SuraBlock*>(stmt), globals, locals, seen_refs, diagnostics);
            break;
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(stmt);
            std::unordered_set<std::string> value_refs;
            collect_expr_refs(a->value.get(), value_refs);
            bool known_global = globals.count(a->name) > 0;
            bool known_local = locals.count(a->name) > 0;
            bool probably_intended_outer = seen_refs.count(a->name) > 0 || value_refs.count(a->name) > 0;
            if (known_global && !known_local && probably_intended_outer) {
                diagnostics.push_back({
                    a->line,
                    "local assignment '" + a->name + "' shadows a top-level variable. "
                    "Assignments inside functions create locals; return the new value or update state outside the function."
                });
            }
            seen_refs.insert(value_refs.begin(), value_refs.end());
            locals.insert(a->name);
            break;
        }
        case NK::IN_PLACE: {
            auto* ip = static_cast<const InPlaceStmt*>(stmt);
            std::unordered_set<std::string> value_refs;
            collect_expr_refs(ip->value.get(), value_refs);
            if (globals.count(ip->name) > 0 && locals.count(ip->name) == 0) {
                diagnostics.push_back({
                    ip->line,
                    "in-place update '" + ip->name + " " + ip->op + "=' inside a function does not update the top-level variable. "
                    "Return the new value or update state outside the function."
                });
            }
            seen_refs.insert(value_refs.begin(), value_refs.end());
            seen_refs.insert(ip->name);
            locals.insert(ip->name);
            break;
        }
        case NK::GLOBAL_DECL: {
            auto* gd = static_cast<const GlobalDeclStmt*>(stmt);
            for (const auto& name : gd->names) locals.insert(name);
            break;
        }
        case NK::IF: {
            auto* s = static_cast<const IfStmt*>(stmt);
            collect_expr_refs(s->cond.get(), seen_refs);
            lint_function_shadowing_block(s->then_block.get(), globals, locals, seen_refs, diagnostics);
            lint_function_shadowing_block(s->else_block.get(), globals, locals, seen_refs, diagnostics);
            break;
        }
        case NK::WHILE: {
            auto* s = static_cast<const WhileStmt*>(stmt);
            collect_expr_refs(s->cond.get(), seen_refs);
            lint_function_shadowing_block(s->body.get(), globals, locals, seen_refs, diagnostics);
            break;
        }
        case NK::REPEAT: {
            auto* s = static_cast<const RepeatStmt*>(stmt);
            collect_expr_refs(s->count.get(), seen_refs);
            lint_function_shadowing_block(s->body.get(), globals, locals, seen_refs, diagnostics);
            break;
        }
        case NK::FOR: {
            auto* s = static_cast<const ForStmt*>(stmt);
            collect_expr_refs(s->from.get(), seen_refs);
            collect_expr_refs(s->to.get(), seen_refs);
            collect_expr_refs(s->step.get(), seen_refs);
            locals.insert(s->var);
            lint_function_shadowing_block(s->body.get(), globals, locals, seen_refs, diagnostics);
            break;
        }
        case NK::FOREACH: {
            auto* s = static_cast<const ForeachStmt*>(stmt);
            collect_expr_refs(s->collection.get(), seen_refs);
            locals.insert(s->var);
            if (!s->var2.empty()) locals.insert(s->var2);
            lint_function_shadowing_block(s->body.get(), globals, locals, seen_refs, diagnostics);
            break;
        }
        case NK::FUNC_DEF:
            break;
        case NK::FUNC_EXPR:
            break;
        default: {
            std::unordered_set<std::string> refs;
            collect_stmt_refs(stmt, refs);
            seen_refs.insert(refs.begin(), refs.end());
            break;
        }
    }
}

static void lint_function_shadowing_block(const SuraBlock* block,
                                          const std::unordered_set<std::string>& globals,
                                          std::unordered_set<std::string>& locals,
                                          std::unordered_set<std::string>& seen_refs,
                                          std::vector<StaticDiagnostic>& diagnostics) {
    if (!block) return;
    for (const auto& stmt : block->body) {
        lint_function_shadowing_stmt(stmt.get(), globals, locals, seen_refs, diagnostics);
    }
}

static void lint_function_shadowing_stmt_roots(const Stmt* stmt,
                                               const std::unordered_set<std::string>& globals,
                                               std::vector<StaticDiagnostic>& diagnostics) {
    if (!stmt) return;
    if (stmt->kind == NK::FUNC_DEF) {
        auto* fn = static_cast<const FuncDef*>(stmt);
        std::unordered_set<std::string> locals(fn->params.begin(), fn->params.end());
        std::unordered_set<std::string> seen_refs;
        lint_function_shadowing_block(fn->body.get(), globals, locals, seen_refs, diagnostics);
        return;
    }
    if (stmt->kind == NK::CLASS_DEF) {
        auto* cls = static_cast<const ClassDef*>(stmt);
        for (const auto& entry : cls->methods) {
            std::unordered_set<std::string> locals(entry.second.params.begin(), entry.second.params.end());
            locals.insert("self");
            locals.insert("this");
            std::unordered_set<std::string> seen_refs;
            lint_function_shadowing_block(entry.second.body, globals, locals, seen_refs, diagnostics);
        }
    }
}

static std::vector<StaticDiagnostic> lint_global_shadowing(const SuraBlock* ast) {
    std::vector<StaticDiagnostic> diagnostics;
    std::unordered_set<std::string> globals;
    collect_top_level_names(ast, globals);
    if (!ast || globals.empty()) return diagnostics;
    for (const auto& stmt : ast->body) {
        lint_function_shadowing_stmt_roots(stmt.get(), globals, diagnostics);
    }
    return diagnostics;
}

static std::string read_release_secret_file(const std::string& path) {
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read release secret file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string value = ss.str();
    if (value.size() >= 3 &&
        (unsigned char)value[0] == 0xEF &&
        (unsigned char)value[1] == 0xBB &&
        (unsigned char)value[2] == 0xBF) {
        value.erase(0, 3);
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

static std::string json_escape_text(const std::string& s) {
    std::ostringstream out;
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << (int)ch
                        << std::dec << std::setfill(' ');
                } else {
                    out << (char)ch;
                }
                break;
        }
    }
    return out.str();
}

static const char* ast_kind_name(NK kind) {
    switch (kind) {
        case NK::NUM_LIT: return "NUM_LIT";
        case NK::STR_LIT: return "STR_LIT";
        case NK::BOOL_LIT: return "BOOL_LIT";
        case NK::NIL_LIT: return "NIL_LIT";
        case NK::IDENT: return "IDENT";
        case NK::BIN_OP: return "BIN_OP";
        case NK::UNARY_OP: return "UNARY_OP";
        case NK::DOT_ACCESS: return "DOT_ACCESS";
        case NK::INDEX: return "INDEX";
        case NK::CALL: return "CALL";
        case NK::METHOD_CALL: return "METHOD_CALL";
        case NK::SUPER_CALL: return "SUPER_CALL";
        case NK::ARRAY_LIT: return "ARRAY_LIT";
        case NK::DICT_LIT: return "DICT_LIT";
        case NK::NEW_EXPR: return "NEW_EXPR";
        case NK::SuraBlock: return "SuraBlock";
        case NK::ASSIGN: return "ASSIGN";
        case NK::IN_PLACE: return "IN_PLACE";
        case NK::DOT_ASSIGN: return "DOT_ASSIGN";
        case NK::INDEX_ASSIGN: return "INDEX_ASSIGN";
        case NK::IF: return "IF";
        case NK::WHILE: return "WHILE";
        case NK::REPEAT: return "REPEAT";
        case NK::FOR: return "FOR";
        case NK::FOREACH: return "FOREACH";
        case NK::FUNC_DEF: return "FUNC_DEF";
        case NK::CLASS_DEF: return "CLASS_DEF";
        case NK::NEW_INST: return "NEW_INST";
        case NK::RETURN: return "RETURN";
        case NK::BREAK: return "BREAK";
        case NK::CONTINUE: return "CONTINUE";
        case NK::THROW: return "THROW";
        case NK::TRY: return "TRY";
        case NK::USE: return "USE";
        case NK::CMD: return "CMD";
        case NK::EXPR_STMT: return "EXPR_STMT";
        case NK::TERNARY: return "TERNARY";
        case NK::FUNC_EXPR: return "FUNC_EXPR";
        case NK::STR_INTERP: return "STR_INTERP";
        case NK::MATCH: return "MATCH";
        case NK::ENUM_DEF: return "ENUM_DEF";
        case NK::IMPORT: return "IMPORT";
        case NK::GLOBAL_DECL: return "GLOBAL_DECL";
    }
    return "UNKNOWN";
}

static const char* ast_type_name(SType kind) {
    switch (kind) {
        case SType::ANY: return "ANY";
        case SType::NUMBER: return "NUMBER";
        case SType::STRING: return "STRING";
        case SType::BOOL: return "BOOL";
        case SType::ARRAY: return "ARRAY";
        case SType::DICT: return "DICT";
        case SType::NIL: return "NIL";
        case SType::CLASS: return "CLASS";
    }
    return "ANY";
}

static void write_json_string(std::ostream& out, const std::string& value) {
    out << "\"" << json_escape_text(value) << "\"";
}

static void write_json_string_array(std::ostream& out, const std::vector<std::string>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        write_json_string(out, values[i]);
    }
    out << "]";
}

static void write_ast_type_json(std::ostream& out, const TypeAnnot& type) {
    out << "{\"present\":" << (type.present ? "true" : "false")
        << ",\"kind\":\"" << ast_type_name(type.kind) << "\""
        << ",\"class_name\":";
    write_json_string(out, type.class_name);
    out << ",\"source_name\":";
    write_json_string(out, type.source_name);
    out << "}";
}

static void write_ast_expr_json(std::ostream& out, const Expr* expr);
static void write_ast_stmt_json(std::ostream& out, const Stmt* stmt);

static void write_ast_expr_array_json(std::ostream& out, const std::vector<ExprPtr>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        if (values[i]) write_ast_expr_json(out, values[i].get());
        else out << "null";
    }
    out << "]";
}

static void write_ast_type_array_json(std::ostream& out, const std::vector<TypeAnnot>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        write_ast_type_json(out, values[i]);
    }
    out << "]";
}

static void write_ast_block_json(std::ostream& out, const SuraBlock* block) {
    if (!block) {
        out << "null";
        return;
    }
    out << "{\"node\":\"SuraBlock\",\"line\":" << block->line << ",\"body\":[";
    for (size_t i = 0; i < block->body.size(); ++i) {
        if (i) out << ",";
        write_ast_stmt_json(out, block->body[i].get());
    }
    out << "]}";
}

static void write_ast_node_head(std::ostream& out, const Node* node) {
    out << "{\"node\":\"" << ast_kind_name(node->kind) << "\",\"line\":" << node->line;
}

static void write_ast_expr_json(std::ostream& out, const Expr* expr) {
    if (!expr) {
        out << "null";
        return;
    }
    write_ast_node_head(out, expr);
    switch (expr->kind) {
        case NK::NUM_LIT: {
            auto* n = static_cast<const NumLit*>(expr);
            out << ",\"value\":" << std::setprecision(17) << n->value;
            break;
        }
        case NK::STR_LIT: {
            auto* s = static_cast<const StrLit*>(expr);
            out << ",\"value\":";
            write_json_string(out, s->value);
            break;
        }
        case NK::BOOL_LIT: {
            auto* b = static_cast<const BoolLit*>(expr);
            out << ",\"value\":" << (b->value ? "true" : "false");
            break;
        }
        case NK::NIL_LIT:
            break;
        case NK::IDENT: {
            auto* id = static_cast<const Ident*>(expr);
            out << ",\"name\":";
            write_json_string(out, id->name);
            break;
        }
        case NK::BIN_OP: {
            auto* b = static_cast<const BinOp*>(expr);
            out << ",\"op\":";
            write_json_string(out, b->op);
            out << ",\"left\":";
            write_ast_expr_json(out, b->left.get());
            out << ",\"right\":";
            write_ast_expr_json(out, b->right.get());
            break;
        }
        case NK::UNARY_OP: {
            auto* u = static_cast<const UnaryOp*>(expr);
            out << ",\"op\":";
            write_json_string(out, u->op);
            out << ",\"operand\":";
            write_ast_expr_json(out, u->operand.get());
            break;
        }
        case NK::DOT_ACCESS: {
            auto* d = static_cast<const DotAccess*>(expr);
            out << ",\"prop\":";
            write_json_string(out, d->prop);
            out << ",\"optional\":" << (d->optional ? "true" : "false")
                << ",\"obj\":";
            write_ast_expr_json(out, d->obj.get());
            break;
        }
        case NK::INDEX: {
            auto* idx = static_cast<const IndexExpr*>(expr);
            out << ",\"obj\":";
            write_ast_expr_json(out, idx->obj.get());
            out << ",\"key\":";
            write_ast_expr_json(out, idx->key.get());
            break;
        }
        case NK::CALL: {
            auto* ce = static_cast<const CallExpr*>(expr);
            out << ",\"name\":";
            write_json_string(out, ce->name);
            out << ",\"args\":";
            write_ast_expr_array_json(out, ce->args);
            break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(expr);
            out << ",\"method\":";
            write_json_string(out, mc->method);
            out << ",\"optional\":" << (mc->optional ? "true" : "false")
                << ",\"obj\":";
            write_ast_expr_json(out, mc->obj.get());
            out << ",\"args\":";
            write_ast_expr_array_json(out, mc->args);
            break;
        }
        case NK::SUPER_CALL: {
            auto* sc = static_cast<const SuperCallExpr*>(expr);
            out << ",\"method\":";
            write_json_string(out, sc->method);
            out << ",\"args\":";
            write_ast_expr_array_json(out, sc->args);
            break;
        }
        case NK::ARRAY_LIT: {
            auto* al = static_cast<const ArrayLit*>(expr);
            out << ",\"elements\":";
            write_ast_expr_array_json(out, al->elements);
            break;
        }
        case NK::DICT_LIT: {
            auto* dl = static_cast<const DictLit*>(expr);
            out << ",\"entries\":[";
            for (size_t i = 0; i < dl->key_order.size(); ++i) {
                if (i) out << ",";
                const std::string& key = dl->key_order[i];
                out << "{\"key\":";
                write_json_string(out, key);
                out << ",\"value\":";
                auto it = dl->pairs.find(key);
                if (it != dl->pairs.end()) write_ast_expr_json(out, it->second.get());
                else out << "null";
                out << "}";
            }
            out << "]";
            break;
        }
        case NK::NEW_EXPR: {
            auto* ne = static_cast<const NewExpr*>(expr);
            out << ",\"class_name\":";
            write_json_string(out, ne->class_name);
            out << ",\"args\":";
            write_ast_expr_array_json(out, ne->args);
            break;
        }
        case NK::TERNARY: {
            auto* t = static_cast<const TernaryExpr*>(expr);
            out << ",\"cond\":";
            write_ast_expr_json(out, t->cond.get());
            out << ",\"then\":";
            write_ast_expr_json(out, t->then_val.get());
            out << ",\"else\":";
            write_ast_expr_json(out, t->else_val.get());
            break;
        }
        case NK::FUNC_EXPR: {
            auto* fn = static_cast<const FuncExpr*>(expr);
            out << ",\"params\":";
            write_json_string_array(out, fn->params);
            out << ",\"defaults\":";
            write_ast_expr_array_json(out, fn->defaults);
            out << ",\"body\":";
            write_ast_block_json(out, fn->body.get());
            break;
        }
        case NK::STR_INTERP: {
            auto* si = static_cast<const StrInterp*>(expr);
            out << ",\"parts\":";
            write_ast_expr_array_json(out, si->parts);
            break;
        }
        default:
            out << ",\"unsupported\":true";
            break;
    }
    out << "}";
}

static void write_ast_stmt_json(std::ostream& out, const Stmt* stmt) {
    if (!stmt) {
        out << "null";
        return;
    }
    if (stmt->kind == NK::SuraBlock) {
        write_ast_block_json(out, static_cast<const SuraBlock*>(stmt));
        return;
    }
    write_ast_node_head(out, stmt);
    switch (stmt->kind) {
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(stmt);
            out << ",\"name\":";
            write_json_string(out, a->name);
            out << ",\"type\":";
            write_ast_type_json(out, a->type_annot);
            out << ",\"value\":";
            write_ast_expr_json(out, a->value.get());
            break;
        }
        case NK::IN_PLACE: {
            auto* ip = static_cast<const InPlaceStmt*>(stmt);
            out << ",\"name\":";
            write_json_string(out, ip->name);
            out << ",\"op\":";
            write_json_string(out, ip->op);
            out << ",\"value\":";
            write_ast_expr_json(out, ip->value.get());
            break;
        }
        case NK::DOT_ASSIGN: {
            auto* da = static_cast<const DotAssignStmt*>(stmt);
            out << ",\"obj_name\":";
            write_json_string(out, da->obj_name);
            out << ",\"prop\":";
            write_json_string(out, da->prop);
            out << ",\"value\":";
            write_ast_expr_json(out, da->value.get());
            break;
        }
        case NK::INDEX_ASSIGN: {
            auto* ia = static_cast<const IndexAssignStmt*>(stmt);
            out << ",\"name\":";
            write_json_string(out, ia->name);
            out << ",\"key\":";
            write_ast_expr_json(out, ia->key.get());
            out << ",\"value\":";
            write_ast_expr_json(out, ia->value.get());
            break;
        }
        case NK::IF: {
            auto* is = static_cast<const IfStmt*>(stmt);
            out << ",\"cond\":";
            write_ast_expr_json(out, is->cond.get());
            out << ",\"then_block\":";
            write_ast_block_json(out, is->then_block.get());
            out << ",\"else_block\":";
            write_ast_block_json(out, is->else_block.get());
            break;
        }
        case NK::WHILE: {
            auto* ws = static_cast<const WhileStmt*>(stmt);
            out << ",\"cond\":";
            write_ast_expr_json(out, ws->cond.get());
            out << ",\"body\":";
            write_ast_block_json(out, ws->body.get());
            break;
        }
        case NK::REPEAT: {
            auto* rs = static_cast<const RepeatStmt*>(stmt);
            out << ",\"count\":";
            write_ast_expr_json(out, rs->count.get());
            out << ",\"body\":";
            write_ast_block_json(out, rs->body.get());
            break;
        }
        case NK::FOR: {
            auto* fs = static_cast<const ForStmt*>(stmt);
            out << ",\"var\":";
            write_json_string(out, fs->var);
            out << ",\"from\":";
            write_ast_expr_json(out, fs->from.get());
            out << ",\"to\":";
            write_ast_expr_json(out, fs->to.get());
            out << ",\"step\":";
            write_ast_expr_json(out, fs->step.get());
            out << ",\"body\":";
            write_ast_block_json(out, fs->body.get());
            break;
        }
        case NK::FOREACH: {
            auto* fe = static_cast<const ForeachStmt*>(stmt);
            out << ",\"var\":";
            write_json_string(out, fe->var);
            out << ",\"var2\":";
            write_json_string(out, fe->var2);
            out << ",\"collection\":";
            write_ast_expr_json(out, fe->collection.get());
            out << ",\"body\":";
            write_ast_block_json(out, fe->body.get());
            break;
        }
        case NK::FUNC_DEF: {
            auto* fd = static_cast<const FuncDef*>(stmt);
            out << ",\"name\":";
            write_json_string(out, fd->name);
            out << ",\"params\":";
            write_json_string_array(out, fd->params);
            out << ",\"param_types\":";
            write_ast_type_array_json(out, fd->param_types);
            out << ",\"defaults\":";
            write_ast_expr_array_json(out, fd->defaults);
            out << ",\"return_type\":";
            write_ast_type_json(out, fd->return_type);
            out << ",\"body\":";
            write_ast_block_json(out, fd->body.get());
            break;
        }
        case NK::CLASS_DEF: {
            auto* cd = static_cast<const ClassDef*>(stmt);
            out << ",\"name\":";
            write_json_string(out, cd->name);
            out << ",\"parent\":";
            write_json_string(out, cd->parent);
            out << ",\"value_struct\":" << (cd->value_struct ? "true" : "false");
            out << ",\"packed_layout\":" << (cd->packed_layout ? "true" : "false");
            out << ",\"fields\":[";
            size_t field_i = 0;
            for (const auto& field_name : cd->field_order) {
                auto found = cd->field_defaults.find(field_name);
                if (found == cd->field_defaults.end()) continue;
                const auto& item = *found;
                if (field_i++) out << ",";
                out << "{\"name\":";
                write_json_string(out, item.first);
                out << ",\"type\":";
                auto type = cd->field_types.find(field_name);
                write_ast_type_json(out, type == cd->field_types.end()
                                             ? TypeAnnot{}
                                             : type->second);
                out << ",\"default\":";
                write_ast_expr_json(out, item.second.get());
                out << "}";
            }
            out << "],\"methods\":[";
            size_t method_i = 0;
            for (const auto& item : cd->methods) {
                if (method_i++) out << ",";
                out << "{\"name\":";
                write_json_string(out, item.first);
                out << ",\"params\":";
                write_json_string_array(out, item.second.params);
                out << ",\"defaults\":";
                write_ast_expr_array_json(out, item.second.defaults);
                out << ",\"body\":";
                write_ast_block_json(out, item.second.body);
                out << "}";
            }
            out << "]";
            break;
        }
        case NK::NEW_INST: {
            auto* ni = static_cast<const NewInstStmt*>(stmt);
            out << ",\"var\":";
            write_json_string(out, ni->var);
            out << ",\"class_name\":";
            write_json_string(out, ni->class_name);
            out << ",\"args\":";
            write_ast_expr_array_json(out, ni->args);
            break;
        }
        case NK::RETURN: {
            auto* rs = static_cast<const ReturnStmt*>(stmt);
            out << ",\"value\":";
            write_ast_expr_json(out, rs->value.get());
            break;
        }
        case NK::BREAK:
        case NK::CONTINUE:
            break;
        case NK::THROW: {
            auto* ts = static_cast<const ThrowStmt*>(stmt);
            out << ",\"msg\":";
            write_ast_expr_json(out, ts->msg.get());
            break;
        }
        case NK::TRY: {
            auto* ts = static_cast<const TryStmt*>(stmt);
            out << ",\"try_block\":";
            write_ast_block_json(out, ts->try_block.get());
            out << ",\"catch_var\":";
            write_json_string(out, ts->catch_var);
            out << ",\"catch_block\":";
            write_ast_block_json(out, ts->catch_block.get());
            out << ",\"finally_block\":";
            write_ast_block_json(out, ts->finally_block.get());
            break;
        }
        case NK::USE: {
            auto* us = static_cast<const UseStmt*>(stmt);
            out << ",\"lib\":";
            write_json_string(out, us->lib);
            break;
        }
        case NK::CMD: {
            auto* cs = static_cast<const CmdStmt*>(stmt);
            out << ",\"cmd\":";
            write_json_string(out, cs->cmd);
            out << ",\"args\":";
            write_ast_expr_array_json(out, cs->args);
            break;
        }
        case NK::EXPR_STMT: {
            auto* es = static_cast<const ExprStmt*>(stmt);
            out << ",\"expr\":";
            write_ast_expr_json(out, es->expr.get());
            break;
        }
        case NK::MATCH: {
            auto* ms = static_cast<const MatchStmt*>(stmt);
            out << ",\"subject\":";
            write_ast_expr_json(out, ms->subject.get());
            out << ",\"arms\":[";
            for (size_t i = 0; i < ms->arms.size(); ++i) {
                if (i) out << ",";
                const auto& arm = ms->arms[i];
                out << "{\"is_wildcard\":" << (arm.is_wildcard ? "true" : "false")
                    << ",\"is_range\":" << (arm.is_range ? "true" : "false")
                    << ",\"pattern\":";
                write_ast_expr_json(out, arm.pattern.get());
                out << ",\"range_end\":";
                write_ast_expr_json(out, arm.range_end.get());
                out << ",\"body\":";
                write_ast_block_json(out, arm.body.get());
                out << "}";
            }
            out << "]";
            break;
        }
        case NK::ENUM_DEF: {
            auto* ed = static_cast<const EnumDef*>(stmt);
            out << ",\"name\":";
            write_json_string(out, ed->name);
            out << ",\"members\":[";
            for (size_t i = 0; i < ed->members.size(); ++i) {
                if (i) out << ",";
                out << "{\"name\":";
                write_json_string(out, ed->members[i].name);
                out << ",\"line\":" << ed->members[i].line << ",\"value\":";
                write_ast_expr_json(out, ed->members[i].value.get());
                out << "}";
            }
            out << "]";
            break;
        }
        case NK::IMPORT: {
            auto* im = static_cast<const ImportStmt*>(stmt);
            out << ",\"path\":";
            write_json_string(out, im->path);
            break;
        }
        case NK::GLOBAL_DECL: {
            auto* gd = static_cast<const GlobalDeclStmt*>(stmt);
            out << ",\"names\":";
            write_json_string_array(out, gd->names);
            break;
        }
        default:
            out << ",\"unsupported\":true";
            break;
    }
    out << "}";
}

static std::string sura_ast_json(const SuraBlock* program, const std::string& source_path, int type_errors) {
    std::ostringstream out;
    out << "{\"schema\":\"sura.ast.v1\",\"source\":";
    write_json_string(out, source_path);
    out << ",\"type_errors\":" << type_errors << ",\"ast\":";
    write_ast_block_json(out, program);
    out << "}\n";
    return out.str();
}

static std::string trim_sura_line(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string format_sura_text(const std::string& text) {
    std::ostringstream out;
    int indent = 0;
    std::istringstream lines(text);
    std::string raw;
    std::regex dedent_re("^(end|else|elif|catch)\\b");
    std::regex indent_re("(\\b(do|then)\\s*$|^(try|else|catch\\b))");
    while (std::getline(lines, raw)) {
        std::string trim = trim_sura_line(raw);
        if (std::regex_search(trim, dedent_re)) indent = std::max(0, indent - 1);
        if (!trim.empty()) out << std::string((size_t)indent * 2, ' ') << trim;
        out << "\n";
        if (std::regex_search(trim, indent_re)) ++indent;
    }
    return out.str();
}

static std::string sura_utf8(std::initializer_list<unsigned int> codepoints) {
    std::string out;
    for (unsigned int cp : codepoints) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

int main(int argc, char* argv[]) {
    struct GCShutdownGuard {
        ~GCShutdownGuard() { GC::shutdown(); }
    } gc_shutdown_guard;

    std::vector<std::string> raw_args = command_line_args(argc, argv);
    if (!raw_args.empty()) SuraStd::set_runtime_executable(raw_args[0]);
    SuraStd::set_async_isolated_child(std::getenv("SURA_ASYNC_CHILD") != nullptr);

    auto normalize_language = [](std::string raw) -> std::string {
        raw.erase(std::remove_if(raw.begin(), raw.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }), raw.end());
        std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        if (raw.empty()) return "en";
        if (raw == "en" || raw == "eng" || raw == "english" || raw == "default") return "en";
        if (raw == "ko" || raw == "kor" || raw == "korean" || raw == "kr") return "ko";
        return "";
    };

    std::string output_language = "en";
    if (const char* env_language = std::getenv("SURA_LANG")) {
        std::string normalized = normalize_language(env_language);
        if (!normalized.empty()) output_language = normalized;
    }
    auto is_korean_output = [&]() { return output_language == "ko"; };
    const std::string ko_error = sura_utf8({0xC624, 0xB958});
    const std::string ko_runtime_error = sura_utf8({0xB7F0, 0xD0C0, 0xC784, 0x0020, 0xC624, 0xB958});
    const std::string ko_internal_error = sura_utf8({0xB0B4, 0xBD80, 0x0020, 0xC624, 0xB958});
    const std::string ko_parse_error = sura_utf8({0xC218, 0xB77C, 0x0020, 0xD30C, 0xC11C, 0x0020, 0xC624, 0xB958});
    const std::string ko_stack_trace = sura_utf8({0xC2A4, 0xD0DD, 0x0020, 0xD2B8, 0xB808, 0xC774, 0xC2A4, 0x003A});
    const std::string ko_line_unit = sura_utf8({0xBC88, 0xC9F8, 0x0020, 0xC904});
    const std::string ko_col_unit = sura_utf8({0xBC88, 0xC9F8, 0x0020, 0xCE78});
    const std::string ko_stack_overflow = sura_utf8({0xC2A4, 0xD0DD, 0x0020, 0xC624, 0xBC84, 0xD50C, 0xB85C, 0xC6B0});
    const std::string ko_undefined_function = sura_utf8({0xC815, 0xC758, 0xB418, 0xC9C0, 0x0020, 0xC54A, 0xC740, 0x0020, 0xD568, 0xC218});
    const std::string ko_invalid_argument_count = sura_utf8({0xC798, 0xBABB, 0xB41C, 0x0020, 0xC778, 0xC790, 0x0020, 0xAC1C, 0xC218});
    const std::string ko_type_mismatch = sura_utf8({0xD0C0, 0xC785, 0x0020, 0xBD88, 0xC77C, 0xCE58});
    const std::string ko_array_index_out_of_range = sura_utf8({0xBC30, 0xC5F4, 0x0020, 0xBC94, 0xC704, 0x0020, 0xCD08, 0xACFC});
    const std::string ko_nil_dereference = std::string("nil ") + sura_utf8({0xC5ED, 0xCC38, 0xC870});
    auto text = [&](const std::string& english, const std::string& korean) {
        return is_korean_output() ? korean : english;
    };
    auto replace_all_text = [](std::string& value, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    auto localize_runtime_message = [&](std::string value) {
        if (!is_korean_output()) {
            replace_all_text(value, "[E500] 스택 오버플로우", "[E500] stack overflow");
            replace_all_text(value, "[E101] 정의되지 않은 함수", "[E101] undefined function");
            replace_all_text(value, "[E300] 잘못된 인자 개수", "[E300] invalid argument count");
            replace_all_text(value, "[E200] 타입 불일치", "[E200] type mismatch");
            replace_all_text(value, "[E202] 배열 범위 초과", "[E202] array index out of range");
            replace_all_text(value, "[E201] nil 역참조", "[E201] nil dereference");
            replace_all_text(value, "스택 오버플로우", "stack overflow");
            replace_all_text(value, "정의되지 않은 함수", "undefined function");
            replace_all_text(value, "잘못된 인자 개수", "invalid argument count");
            replace_all_text(value, "타입 불일치", "type mismatch");
            replace_all_text(value, "배열 범위 초과", "array index out of range");
            replace_all_text(value, "nil 역참조", "nil dereference");
        } else {
            replace_all_text(value, "[E500] stack overflow", "[E500] " + ko_stack_overflow);
            replace_all_text(value, "[E101] undefined function", "[E101] " + ko_undefined_function);
            replace_all_text(value, "[E300] invalid argument count", "[E300] " + ko_invalid_argument_count);
            replace_all_text(value, "[E200] type mismatch", "[E200] " + ko_type_mismatch);
            replace_all_text(value, "[E202] array index out of range", "[E202] " + ko_array_index_out_of_range);
            replace_all_text(value, "[E201] nil dereference", "[E201] " + ko_nil_dereference);
            replace_all_text(value, "stack overflow", ko_stack_overflow);
            replace_all_text(value, "undefined function", ko_undefined_function);
            replace_all_text(value, "invalid argument count", ko_invalid_argument_count);
            replace_all_text(value, "type mismatch", ko_type_mismatch);
            replace_all_text(value, "array index out of range", ko_array_index_out_of_range);
            replace_all_text(value, "nil dereference", ko_nil_dereference);
        }
        return value;
    };
    auto localize_diagnostic = [&](std::string value) {
        value = localize_runtime_message(value);
        replace_all_text(value, "[Sura Parse Error]", "[" + text("Sura Parse Error", ko_parse_error) + "]");
        replace_all_text(value, "[Runtime Error]", "[" + text("Runtime Error", ko_runtime_error) + "]");
        replace_all_text(value, "[Internal Error]", "[" + text("Internal Error", ko_internal_error) + "]");
        replace_all_text(value, "[Error]", "[" + text("Error", ko_error) + "]");
        return value;
    };
    auto report_cli_error = [&](const std::string& message) {
        std::cerr << "[" << text("Error", ko_error) << "] " << message << "\n";
    };

    auto report_error = [&](const std::string& source, int line, const std::string& msg) {
        std::string token;
        size_t q1 = msg.find('\'');
        if (q1 != std::string::npos) {
            size_t q2 = msg.find('\'', q1 + 1);
            if (q2 != std::string::npos && q2 > q1 + 1)
                token = msg.substr(q1 + 1, q2 - q1 - 1);
        }
        std::stringstream ss(source);
        std::string l;
        int cur = 1;
        while (std::getline(ss, l)) {
            if (cur == line) {
                std::string line_str = std::to_string(line);
                size_t pos = token.empty() ? std::string::npos : l.find(token);
                int col = (pos == std::string::npos) ? 1 : (int)pos + 1;
                std::string display = localize_diagnostic(msg);
                if (display.find("(line ") == std::string::npos &&
                    display.find(ko_line_unit) == std::string::npos) {
                    std::string loc = is_korean_output()
                        ? " (" + std::to_string(line) + ko_line_unit + ", " + std::to_string(col) + ko_col_unit + ")"
                        : " (line " + std::to_string(line) + ", col " + std::to_string(col) + ")";
                    size_t nl = display.find('\n');
                    if (nl == std::string::npos) display += loc;
                    else display.insert(nl, loc);
                }
                std::cerr << "\n\033[1;31m" << display << "\033[0m\n";
                std::cerr << "\033[1;34m" << line_str << " |\033[0m " << l << "\n";
                int underline = token.empty() ? 1 : std::max(1, (int)token.size());
                std::cerr << std::string(line_str.size(), ' ') << " \033[1;34m|\033[0m "
                          << std::string(std::max(0, col - 1), ' ')
                          << "\033[1;33m" << std::string(underline, '^') << "\033[0m\n";
                break;
            }
            cur++;
        }
        if (line <= 0) std::cerr << "\n\033[1;31m" << localize_diagnostic(msg) << "\033[0m\n";
        std::cerr << std::endl;
    };

    auto report_stack_trace = [&](const std::vector<StackFrameInfo>& trace) {
        if (trace.empty()) return;
        std::cerr << "\033[1;36m" << text("Stack trace:", ko_stack_trace) << "\033[0m\n";
        for (size_t i = 0; i < trace.size(); ++i) {
            const auto& f = trace[i];
            std::cerr << "  " << (i == 0 ? "\033[1;31m->\033[0m " : "   ")
                      << f.func_name
                      << " (line " << f.line << ")\n";
        }
        std::cerr << std::endl;
    };

    bool dump_bytecode  = false;
    bool bench_mode     = false;
    bool repl_mode      = false;
    // Type errors are execution barriers by default. Existing gradual programs
    // can opt into the old warning-and-run behavior explicitly.
    bool strict_mode    = true;
    bool compile_only   = false; // --compile: save .sura.bc, don't run
    bool load_bc        = false; // --load: run .sura.bc directly
    bool release_only   = false; // --release: build a .sura.srp release container, don't run
    bool load_release   = false; // --load-release: run a .sura.srp release container directly
    bool profile_mode   = false; // --profile: collect runtime type feedback
    bool gc_stats_mode  = false; // --gc-stats: print collector measurements after execution
    bool jit_mode       = false; // --jit: enable native code generation
    bool trace_mode     = false; // --trace/--debug: print VM instruction trace
    bool debug_protocol = false; // --debug-protocol: line-stop debugger protocol for DAP
    bool test_mode      = false; // --test: discover and run Sura tests
    bool check_mode     = false; // --check: parse/typecheck without execution
    bool format_mode    = false; // --format: rewrite Sura source formatting
    bool format_check_mode = false; // --format-check: verify formatting only
    bool lint_mode      = false; // --lint: run lightweight static lint rules
    bool ast_json_mode  = false; // --ast-json: emit machine-readable AST JSON
    bool strict_syntax  = false; // --strict-syntax: reject legacy command-style calls
    std::string freestanding_target; // --target uefi-x86_64
    std::string profile_json_path;
    std::string gc_stats_json_path;
    std::string test_report_path;
    std::string output_path;
    std::string release_key;
    std::string release_license;
    std::string release_key_file;
    std::string release_license_file;
    std::string release_id;
    std::string release_expires;
    std::string filename;
    std::string src;
    std::vector<std::string> script_args;
    bool collect_script_args = false;

    // ==== 1. Argument Parsing ====
    for (int i = 1; i < (int)raw_args.size(); ++i) {
        std::string arg = raw_args[i];
        if (collect_script_args) {
            if (filename.empty()) filename = arg;
            else script_args.push_back(arg);
            continue;
        }
        if (arg == "--dump" || arg == "-d") dump_bytecode = true;
        else if (arg == "--bench" || arg == "-b") bench_mode = true;
        else if (arg == "--repl" || arg == "-r") repl_mode = true;
        else if (arg == "--version" || arg == "-V") {
            std::cout << "Sura Language " << SURA_LANGUAGE_VERSION << "\n";
            return 0;
        }
        else if (arg == "--jit-info" || arg == "--jit-info-json") {
            const SuraJitTargetInfo info = sura_jit_target_info();
            if (arg == "--jit-info-json") {
                std::cout << "{\"schema\":\"sura.jit_target.v1\"," 
                          << "\"os\":\"" << info.os << "\"," 
                          << "\"architecture\":\"" << info.architecture << "\"," 
                          << "\"abi\":\"" << info.abi << "\"," 
                          << "\"backend\":\"" << info.backend << "\"," 
                          << "\"native_supported\":"
                          << (info.native_supported ? "true" : "false") << ","
                          << "\"fallback\":\"" << info.fallback << "\"," 
                          << "\"reason\":\"" << info.reason << "\"}\n";
            } else {
                std::cout << "Native JIT target\n"
                          << "  OS: " << info.os << "\n"
                          << "  Architecture: " << info.architecture << "\n"
                          << "  ABI: " << info.abi << "\n"
                          << "  Backend: " << info.backend << "\n"
                          << "  Native machine code: "
                          << (info.native_supported ? "supported" : "not supported") << "\n"
                          << "  Fallback: " << info.fallback << "\n"
                          << "  Reason: " << info.reason << "\n";
            }
            return 0;
        }
        else if (arg == "--strict" || arg == "-s") strict_mode = true;
        else if (arg == "--legacy-types") strict_mode = false;
        else if (arg == "--compile" || arg == "-c") compile_only = true;
        else if (arg == "--load" || arg == "-l") load_bc = true;
        else if (arg == "--release") release_only = true;
        else if (arg == "--load-release" || arg == "--run-release") load_release = true;
        else if (arg == "--release-key" || arg == "--load-release-key") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires a key\n";
                return 1;
            }
            release_key = raw_args[++i];
        }
        else if (arg == "--release-key-file" || arg == "--load-release-key-file") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires a file path\n";
                return 1;
            }
            release_key_file = raw_args[++i];
        }
        else if (arg == "--release-license" || arg == "--load-release-license") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires a license value\n";
                return 1;
            }
            release_license = raw_args[++i];
        }
        else if (arg == "--release-license-file" || arg == "--load-release-license-file") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires a file path\n";
                return 1;
            }
            release_license_file = raw_args[++i];
        }
        else if (arg == "--release-id" || arg == "--release-customer") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires an id\n";
                return 1;
            }
            release_id = raw_args[++i];
        }
        else if (arg == "--release-expires") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] --release-expires requires YYYY-MM-DD\n";
                return 1;
            }
            release_expires = raw_args[++i];
        }
        else if (arg == "--out" || arg == "-o") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] " << arg << " requires an output path\n";
                return 1;
            }
            output_path = raw_args[++i];
        }
        else if (arg == "--target") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] --target requires a target name\n";
                return 1;
            }
            freestanding_target = raw_args[++i];
            if (freestanding_target == "uefi-x64" ||
                freestanding_target == "x86_64-uefi") {
                freestanding_target = "uefi-x86_64";
            }
            if (freestanding_target != "uefi-x86_64") {
                std::cerr << "[Error] Unsupported target: " << freestanding_target
                          << "\nSupported freestanding target: uefi-x86_64\n";
                return 1;
            }
        }
        else if (arg == "--profile" || arg == "-p") profile_mode = true;
        else if (arg == "--profile-json") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] --profile-json requires an output path\n";
                return 1;
            }
            profile_mode = true;
            profile_json_path = raw_args[++i];
        }
        else if (arg == "--gc-stats") gc_stats_mode = true;
        else if (arg == "--gc-stats-json") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] --gc-stats-json requires an output path\n";
                return 1;
            }
            gc_stats_json_path = raw_args[++i];
        }
        else if (arg == "--jit" || arg == "-j") jit_mode = true;
        else if (arg == "--trace") trace_mode = true;
        else if (arg == "--debug-protocol") debug_protocol = true;
        else if (arg == "--lang" || arg == "--language") {
            if (i + 1 >= (int)raw_args.size()) {
                report_cli_error("--lang requires en or ko");
                return 1;
            }
            std::string normalized_language = normalize_language(raw_args[++i]);
            if (normalized_language.empty()) {
                report_cli_error("--lang requires en or ko");
                return 1;
            }
            output_language = normalized_language;
        }
        else if (arg == "--test") test_mode = true;
        else if (arg == "--check") check_mode = true;
        else if (arg == "--ast-json") ast_json_mode = true;
        else if (arg == "--lint") lint_mode = true;
        else if (arg == "--strict-syntax") strict_syntax = true;
        else if (arg == "--format") format_mode = true;
        else if (arg == "--format-check") {
            format_mode = true;
            format_check_mode = true;
        }
        else if (arg == "--test-report") {
            if (i + 1 >= (int)raw_args.size()) {
                std::cerr << "[Error] --test-report requires an output path\n";
                return 1;
            }
            test_mode = true;
            test_report_path = raw_args[++i];
        }
        else if (arg == "--debug") {
            trace_mode = true;
            profile_mode = true;
            dump_bytecode = true;
        }
        else if (arg == "--") {
            collect_script_args = true;
        }
        else if (arg == "--lsp") {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            auto json_escape = [](const std::string& s) {
                std::string out;
                for (char ch : s) {
                    switch (ch) {
                        case '\\': out += "\\\\"; break;
                        case '"': out += "\\\""; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default: out += ch; break;
                    }
                }
                return out;
            };

            auto json_unescape = [](const std::string& s) {
                std::string out;
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] != '\\' || i + 1 >= s.size()) {
                        out.push_back(s[i]);
                        continue;
                    }
                    char ch = s[++i];
                    if (ch == 'n') out.push_back('\n');
                    else if (ch == 'r') out.push_back('\r');
                    else if (ch == 't') out.push_back('\t');
                    else out.push_back(ch);
                }
                return out;
            };

            std::vector<std::pair<std::string, std::string>> completions = {
                {"is", "assignment (x is value)"},
                {"if", "conditional (if cond then)"},
                {"while", "loop (while cond do)"},
                {"repeat", "repeat N times (repeat N do)"},
                {"func", "function definition"},
                {"class", "class definition"},
                {"struct", "struct definition"},
                {"enum", "enum definition"},
                {"when", "pattern-style branch"},
                {"try", "try/catch block"},
                {"break", "break loop"},
                {"continue", "continue loop"},
                {"return", "return value"},
                {"use", "import library"},
                {"import", "import Sura file"},
                {"global", "declare top-level variables used by this function"},
                {"array", "stdlib module (use array)"},
                {"set", "stdlib module (use set)"},
                {"math", "stdlib module (use math)"},
                {"path", "stdlib module (use path)"},
                {"string", "stdlib module (use string)"},
                {"cli", "stdlib module (use cli)"},
                {"json", "stdlib module (use json)"},
                {"fs", "stdlib module (use fs)"},
                {"regex", "stdlib module (use regex)"},
                {"datetime", "stdlib module (use datetime)"},
                {"crypto", "stdlib module (use crypto)"},
                {"db", "stdlib module (use db)"},
                {"log", "stdlib module (use log)"},
                {"console", "global console API and stdlib module"},
                {"http", "stdlib module (use http)"},
                {"async", "stdlib module (use async)"},
                {"test", "stdlib module (use test)"},
                {"random", "stdlib module (use random)"},
                {"os", "stdlib module (use os)"},
                {"python", "interop stdlib module (use python)"},
                {"ffi", "interop stdlib module (use ffi)"},
                {"plugin", "interop stdlib module (use plugin)"},
                {"vector", "AI stdlib module (use vector)"},
                {"graphics3d", "3D math and mesh stdlib module (use graphics3d)"},
                {"rag", "AI stdlib module (use rag)"},
                {"tensor", "AI stdlib module (use tensor)"},
                {"nn", "native neural-network module (use nn)"},
                {"ai", "alias for the native neural-network module (use ai)"},
                {"autograd", "native contiguous tensors and automatic differentiation (use autograd)"},
                {"tokenizer", "native bounded UTF-8 byte tokenizer (use tokenizer)"},
                {"dataset", "streaming packed-token training dataset loader (use dataset)"},
                {"media", "safe video-to-ASCII text frame conversion (use media)"},
                {"stream", "automation stdlib module (use stream)"},
                {"tool", "automation stdlib module (use tool)"},
                {"llm", "AI stdlib module (use llm)"}
            };
            auto builtin_names = SuraStd::names();
            std::sort(builtin_names.begin(), builtin_names.end());

            struct LspSignatureInfo {
                std::string label;
                std::string documentation;
                std::vector<std::string> parameters;
            };

            auto trim_lsp = [](std::string s) {
                auto not_space = [](unsigned char c) { return !std::isspace(c); };
                s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
                s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
                return s;
            };

            auto split_signature_parameters = [&](const std::string& label) {
                std::vector<std::string> params;
                size_t open = label.find('(');
                size_t close = (open == std::string::npos) ? std::string::npos : label.find(')', open + 1);
                if (open == std::string::npos || close == std::string::npos || close <= open + 1) return params;
                std::string inside = label.substr(open + 1, close - open - 1);
                std::string current;
                int bracket_depth = 0;
                for (char ch : inside) {
                    if (ch == '[') ++bracket_depth;
                    if (ch == ']') bracket_depth = std::max(0, bracket_depth - 1);
                    if (ch == ',' && bracket_depth == 0) {
                        std::string p = trim_lsp(current);
                        if (!p.empty()) params.push_back(p);
                        current.clear();
                        continue;
                    }
                    current.push_back(ch);
                }
                std::string p = trim_lsp(current);
                if (!p.empty()) params.push_back(p);
                return params;
            };

            auto builtin_signature_for = [&](const std::string& name, LspSignatureInfo& info) {
                static const std::unordered_map<std::string, std::pair<std::string, std::string>> docs = {
                    {"print", {"print(value, ...)", "Writes values with a newline."}},
                    {"print_n", {"print_n(value, ...)", "Writes values without a newline."}},
                    {"input", {"input([prompt])", "Reads one line from stdin."}},
                    {"type", {"type(value)", "Returns the runtime type name."}},
                    {"clock", {"clock()", "Returns a monotonic timestamp in seconds."}},
                    {"sleep", {"sleep(milliseconds)", "Blocks the current script for the given number of milliseconds."}},
                    {"cls", {"cls()", "Clears an interactive terminal. Alias for console_clear()."}},
                    {"silent", {"silent([on|off])", "Compatibility no-op for old console examples."}},
                    {"key_down", {"key_down(key)", "Returns true while a keyboard key is currently pressed."}},
                    {"readkey", {"readkey()", "Reads one key and blocks until input is available."}},
                    {"readkey_timeout", {"readkey_timeout(ms)", "Reads one key without blocking longer than the timeout; returns empty text on timeout."}},
                    {"win_init", {"win_init(width, height, title)", "Opens a native Sura drawing window."}},
                    {"win_clear", {"win_clear(r, g, b)", "Clears the Sura drawing window."}},
                    {"win_rect", {"win_rect(x, y, width, height, r, g, b)", "Draws a filled rectangle in the Sura drawing window."}},
                    {"win_circle", {"win_circle(x, y, radius, r, g, b)", "Draws a filled circle in the Sura drawing window."}},
                    {"win_line", {"win_line(x1, y1, x2, y2, r, g, b)", "Draws a line in the Sura drawing window."}},
                    {"win_text", {"win_text(text, x, y, r, g, b)", "Draws UTF-8 text in the Sura drawing window."}},
                    {"win_update", {"win_update()", "Pumps window events, presents a frame, and returns whether the drawing window is open."}},
                    {"win_poll", {"win_poll()", "Pumps window events without presenting a frame; returns whether the drawing window is open."}},
                    {"win_focus", {"win_focus()", "Requests keyboard focus for the Sura drawing window."}},
                    {"win_close", {"win_close()", "Closes the Sura drawing window."}},
                    {"mouse_pos", {"mouse_pos()", "Returns the mouse position as {x, y}."}},
                    {"mouse_down", {"mouse_down(button)", "Returns true while a mouse button is pressed."}},
                    {"grid_init", {"grid_init(width, height)", "Creates a terminal grid buffer for text games."}},
                    {"grid_clear", {"grid_clear()", "Clears the terminal grid buffer or the terminal screen."}},
                    {"grid_set", {"grid_set(x, y, char, [color])", "Writes one terminal grid cell, optionally with color."}},
                    {"grid_draw", {"grid_draw()", "Draws the terminal grid buffer to the console."}},
                    {"uuid_v4", {"uuid_v4()", "Returns a random RFC 4122 version 4 UUID."}},
                    {"uuid", {"uuid()", "Alias for uuid_v4."}},
                    {"array_sum", {"array_sum(array)", "Returns the sum of a numeric array."}},
                    {"array_avg", {"array_avg(array)", "Returns the average of a numeric array or nil for an empty array."}},
                    {"array_unique", {"array_unique(array)", "Returns a shallow de-duplicated copy of an array."}},
                    {"array_flatten", {"array_flatten(array, [depth])", "Returns an array flattened by depth levels."}},
                    {"array_range", {"array_range(end) | array_range(start, end, [step])", "Returns numeric values from start to end, excluding end."}},
                    {"array_chunk", {"array_chunk(array, size)", "Splits an array into fixed-size chunks."}},
                    {"array_zip", {"array_zip(array, ...)", "Combines arrays into rows up to the shortest input length."}},
                    {"array_repeat", {"array_repeat(value, count)", "Returns an array containing value repeated count times."}},
                    {"set_union", {"set_union(array, ...)", "Returns the unique union of arrays, preserving first-seen order."}},
                    {"set_intersection", {"set_intersection(array, ...)", "Returns unique values present in every array."}},
                    {"set_difference", {"set_difference(array, ...)", "Returns unique values from the first array that are missing from the rest."}},
                    {"set_symmetric_difference", {"set_symmetric_difference(left, right)", "Returns unique values present in exactly one of two arrays."}},
                    {"set_is_subset", {"set_is_subset(left, right)", "Checks whether every value in left is present in right."}},
                    {"set_is_superset", {"set_is_superset(left, right)", "Checks whether every value in right is present in left."}},
                    {"random_seed", {"random_seed(seed)", "Seeds the runtime RNG for reproducible tests and game logic."}},
                    {"random_int", {"random_int(max) | random_int(min, max)", "Returns an integer from the runtime RNG."}},
                    {"random_float", {"random_float([max]) | random_float(min, max)", "Returns a floating-point random number."}},
                    {"random_bool", {"random_bool([probability])", "Returns a random boolean with an optional true probability."}},
                    {"random_choice", {"random_choice(array)", "Returns a random item from a non-empty array."}},
                    {"random_shuffle", {"random_shuffle(array)", "Returns a shuffled shallow copy of an array."}},
                    {"random_bytes", {"random_bytes(count)", "Returns an array of random byte values from 0 to 255."}},
                    {"path_join", {"path_join(part, ...)", "Joins path segments using the host platform separator."}},
                    {"path_basename", {"path_basename(path)", "Returns the final path component."}},
                    {"path_dirname", {"path_dirname(path)", "Returns the parent path."}},
                    {"path_ext", {"path_ext(path)", "Returns the file extension."}},
                    {"path_stem", {"path_stem(path)", "Returns the final path component without its last extension."}},
                    {"path_normalize", {"path_normalize(path)", "Returns a lexically normalized path."}},
                    {"path_abs", {"path_abs(path)", "Returns an absolute normalized path."}},
                    {"path_relative", {"path_relative(path, [base])", "Returns path relative to base or the current directory."}},
                    {"file_walk", {"file_walk(path, [extension])", "Recursively lists files, optionally filtering by extension."}},
                    {"file_glob", {"file_glob(pattern)", "Recursively lists regular files matching *, ?, and ** glob patterns."}},
                    {"file_remove_tree", {"file_remove_tree(path)", "Recursively removes a file or directory tree and returns the removed item count."}},
                    {"file_read_bytes", {"file_read_bytes(path)", "Reads a binary file into an array of byte values."}},
                    {"file_write_bytes", {"file_write_bytes(path, bytes)", "Writes an array of byte values to a binary file."}},
                    {"file_sha256", {"file_sha256(path)", "Returns the SHA-256 digest for a file's raw bytes."}},
                    {"sha256_file", {"sha256_file(path)", "Alias for file_sha256."}},
                    {"file_is_dir", {"file_is_dir(path)", "Checks whether a path is a directory."}},
                    {"file_is_file", {"file_is_file(path)", "Checks whether a path is a regular file."}},
                    {"file_info", {"file_info(path)", "Returns path metadata such as exists, size, type, and modified time."}},
                    {"file_size", {"file_size(path)", "Returns the file size in bytes."}},
                    {"file_copy", {"file_copy(src, dst, [overwrite])", "Copies a file."}},
                    {"file_move", {"file_move(src, dst, [overwrite])", "Moves or renames a file."}},
                    {"assert_contains", {"assert_contains(container, value, [message])", "Fails unless a string, array, or dict contains a value or key."}},
                    {"assert_not_contains", {"assert_not_contains(container, value, [message])", "Fails when a string, array, or dict contains a value or key."}},
                    {"assert_match", {"assert_match(text, pattern, [message])", "Fails unless text matches a regular expression."}},
                    {"assert_type", {"assert_type(value, type, [message])", "Fails unless a value has the expected runtime type."}},
                    {"assert_len", {"assert_len(value, length, [message])", "Fails unless a string, array, or dict has the expected length."}},
                    {"assert_between", {"assert_between(value, min, max, [message])", "Fails unless a number is within an inclusive range."}},
                    {"assert_approx", {"assert_approx(actual, expected, [epsilon], [message])", "Fails unless two numbers are approximately equal."}},
                    {"check", {"check(name, condition, [message])", "Returns a pass/fail test result dictionary without throwing."}},
                    {"check_eq", {"check_eq(name, actual, expected, [message])", "Returns a non-throwing equality test result dictionary."}},
                    {"check_match", {"check_match(name, text, pattern, [message])", "Returns a non-throwing regex test result dictionary."}},
                    {"test_summary", {"test_summary(results)", "Summarizes test result dictionaries with total, passed, failed, ok, and failures."}},
                    {"test_report", {"test_report(results, [title])", "Formats test result dictionaries as a readable report."}},
                    {"string_lines", {"string_lines(text)", "Splits text into normalized lines."}},
                    {"string_words", {"string_words(text)", "Splits text on whitespace into words."}},
                    {"string_repeat", {"string_repeat(text, count)", "Repeats text count times."}},
                    {"string_pad_left", {"string_pad_left(text, width, [fill])", "Pads text on the left to a character width."}},
                    {"string_pad_right", {"string_pad_right(text, width, [fill])", "Pads text on the right to a character width."}},
                    {"text_chunks", {"text_chunks(text, [max_chars], [overlap])", "Splits text into UTF-8-safe character chunks for RAG."}},
                    {"json_parse", {"json_parse(text)", "Parses JSON into Sura values."}},
                    {"json_try_parse", {"json_try_parse(text, [fallback])", "Parses JSON or returns fallback/nil on invalid input."}},
                    {"json_stringify", {"json_stringify(value)", "Serializes a Sura value to JSON."}},
                    {"json_pretty", {"json_pretty(value, [indent])", "Serializes a Sura value to human-readable indented JSON."}},
                    {"file_read_json", {"file_read_json(path)", "Reads a UTF-8 JSON file and parses it into Sura values."}},
                    {"file_write_json", {"file_write_json(path, value)", "Serializes a Sura value to JSON and writes it to a UTF-8 file."}},
                    {"jsonl_parse", {"jsonl_parse(text)", "Parses JSON Lines text into an array of Sura values."}},
                    {"jsonl_stringify", {"jsonl_stringify(rows, [trailing_newline])", "Serializes array items as newline-delimited JSON."}},
                    {"sse_parse", {"sse_parse(text)", "Parses Server-Sent Events text into event dictionaries."}},
                    {"sse_data", {"sse_data(text, [parse_json])", "Extracts SSE data payloads, optionally parsing JSON and skipping [DONE]."}},
                    {"csv_parse", {"csv_parse(text, [has_header])", "Parses CSV text into rows or dictionaries."}},
                    {"csv_stringify", {"csv_stringify(rows, [headers])", "Serializes array or dictionary rows to CSV text."}},
                    {"ini_parse", {"ini_parse(text)", "Parses simple INI configuration text into a dictionary."}},
                    {"ini_stringify", {"ini_stringify(dict)", "Serializes a dictionary to deterministic INI configuration text."}},
                    {"json_path", {"json_path(value, path, [default])", "Reads a nested value with dot and bracket path syntax."}},
                    {"dict_get_path", {"dict_get_path(value, path, [default])", "Alias for json_path."}},
                    {"dict_keys", {"dict_keys(dict)", "Returns sorted dictionary keys."}},
                    {"dict_values", {"dict_values(dict)", "Returns dictionary values in sorted key order."}},
                    {"dict_items", {"dict_items(dict)", "Returns sorted key/value dictionaries from a dictionary."}},
                    {"dict_merge", {"dict_merge(dict, ...)", "Returns a shallow left-to-right merged dictionary."}},
                    {"dict_pick", {"dict_pick(dict, keys)", "Returns a dictionary with only selected string keys."}},
                    {"dict_omit", {"dict_omit(dict, keys)", "Returns a dictionary without selected string keys."}},
                    {"json_has_path", {"json_has_path(value, path)", "Checks whether a nested JSON path exists."}},
                    {"json_merge_patch", {"json_merge_patch(target, patch)", "Applies a JSON Merge Patch-style object update without mutating the target."}},
                    {"json_delete_path", {"json_delete_path(value, path)", "Removes a nested JSON path from a copied value."}},
                    {"json_set_path", {"json_set_path(value, path, new_value)", "Sets a nested JSON path on a copied value, creating containers as needed."}},
                    {"pluck", {"pluck(rows, path, [default])", "Extracts a field or JSON path from each array item."}},
                    {"count_by", {"count_by(rows, path)", "Counts array items by a field or JSON path value."}},
                    {"group_by", {"group_by(rows, path)", "Groups array items by a field or JSON path value."}},
                    {"sort_by", {"sort_by(rows, path, [descending])", "Sorts array items in place by a field or JSON path value."}},
                    {"template_render", {"template_render(text, data, [missing])", "Replaces [[path]] or {{path}} placeholders using JSON path-style lookups."}},
                    {"schema_validate", {"schema_validate(value, schema)", "Checks a dictionary against a simple JSON-style schema."}},
                    {"schema_errors", {"schema_errors(value, schema)", "Returns JSON-style schema validation error messages."}},
                    {"schema_to_json_schema", {"schema_to_json_schema(schema, [strict])", "Converts Sura schema shorthand into JSON Schema."}},
                    {"regex_match", {"regex_match(text, pattern)", "Checks whether text matches a regular expression."}},
                    {"regex_replace", {"regex_replace(text, pattern, replacement)", "Replaces regular expression matches."}},
                    {"regex_find_all", {"regex_find_all(text, pattern)", "Returns all regular expression matches."}},
                    {"regex_escape", {"regex_escape(text)", "Escapes text so it can be used as a literal regex pattern."}},
                    {"regex_capture", {"regex_capture(text, pattern)", "Returns the first regex match plus capture groups, or nil."}},
                    {"regex_captures", {"regex_captures(text, pattern)", "Returns all regex matches with capture groups."}},
                    {"regex_split", {"regex_split(text, pattern)", "Splits text using a regular expression delimiter."}},
                    {"datetime_now", {"datetime_now()", "Returns the current local datetime as text."}},
                    {"datetime_format", {"datetime_format(timestamp, format)", "Formats a timestamp with strftime-style tokens."}},
                    {"datetime_utc_format", {"datetime_utc_format(timestamp, format)", "Formats a timestamp in UTC with strftime-style tokens."}},
                    {"datetime_parse", {"datetime_parse(text, [format])", "Parses local datetime text into a Unix timestamp."}},
                    {"datetime_parts", {"datetime_parts(timestamp, [utc])", "Returns year, month, day, time, weekday, and yearday fields."}},
                    {"datetime_add", {"datetime_add(timestamp, seconds)", "Adds seconds to a Unix timestamp."}},
                    {"datetime_diff", {"datetime_diff(end_timestamp, start_timestamp)", "Returns the difference in seconds between two timestamps."}},
                    {"timestamp", {"timestamp()", "Returns the current Unix timestamp."}},
                    {"sha256", {"sha256(text)", "Returns the SHA-256 digest for text."}},
                    {"hmac_sha256", {"hmac_sha256(key, message)", "Returns an HMAC-SHA256 signature for API authentication."}},
                    {"file_hmac_sha256", {"file_hmac_sha256(key, path)", "Returns an HMAC-SHA256 signature for a file's raw bytes."}},
                    {"hmac_sha256_file", {"hmac_sha256_file(key, path)", "Alias for file_hmac_sha256."}},
                    {"crypto_random_bytes", {"crypto_random_bytes(count)", "Returns bytes from the host cryptographic random source."}},
                    {"secure_random_bytes", {"secure_random_bytes(count)", "Alias for crypto_random_bytes."}},
                    {"crypto_random_hex", {"crypto_random_hex(count)", "Returns count cryptographic random bytes encoded as lowercase hex."}},
                    {"secure_random_hex", {"secure_random_hex(count)", "Alias for crypto_random_hex."}},
                    {"constant_time_eq", {"constant_time_eq(left, right)", "Compares two strings without early exit for token and signature checks."}},
                    {"crypto_constant_time_eq", {"crypto_constant_time_eq(left, right)", "Alias for constant_time_eq."}},
                    {"secure_compare", {"secure_compare(left, right)", "Alias for constant_time_eq."}},
                    {"hex_encode", {"hex_encode(text)", "Encodes text bytes as lowercase hexadecimal."}},
                    {"hex_decode", {"hex_decode(text)", "Decodes hexadecimal text into bytes."}},
                    {"base64_encode", {"base64_encode(text)", "Base64-encodes text."}},
                    {"base64_decode", {"base64_decode(text)", "Decodes Base64 text into bytes."}},
                    {"base64_url_encode", {"base64_url_encode(text)", "Base64url-encodes text without padding."}},
                    {"base64_url_decode", {"base64_url_decode(text)", "Decodes unpadded Base64url text into bytes."}},
                    {"url_encode", {"url_encode(text)", "Percent-encodes text for URLs using UTF-8 bytes."}},
                    {"url_decode", {"url_decode(text)", "Decodes percent-encoded URL text."}},
                    {"url_parse", {"url_parse(url)", "Parses a URL into scheme, authority, path, query, params, and fragment fields."}},
                    {"url_build", {"url_build(parts)", "Builds a URL from parsed URL fields or params."}},
                    {"query_build", {"query_build(params)", "Builds a deterministic URL query string from a dictionary."}},
                    {"query_parse", {"query_parse(query)", "Parses a URL query string into a dictionary, preserving repeated keys as arrays."}},
                    {"form_build", {"form_build(params)", "Builds an application/x-www-form-urlencoded body from a dictionary."}},
                    {"form_parse", {"form_parse(body)", "Parses an application/x-www-form-urlencoded body into a dictionary."}},
                    {"auth_bearer", {"auth_bearer(token)", "Builds an Authorization Bearer header dictionary."}},
                    {"auth_basic", {"auth_basic(username, password)", "Builds an Authorization Basic header dictionary."}},
                    {"headers_merge", {"headers_merge(headers, ...)", "Merges HTTP header dictionaries."}},
                    {"headers_get", {"headers_get(headers, name, [default])", "Reads an HTTP header by name case-insensitively."}},
                    {"headers_has", {"headers_has(headers, name)", "Returns true when an HTTP header exists case-insensitively."}},
                    {"headers_redact", {"headers_redact(headers, [names], [mask])", "Returns a copy of HTTP headers with sensitive values masked for logs."}},
                    {"cookie_parse", {"cookie_parse(header_or_headers)", "Parses an HTTP Cookie header into a dictionary."}},
                    {"cookie_build", {"cookie_build(cookies)", "Builds a deterministic HTTP Cookie header value from a dictionary."}},
                    {"cookie_get", {"cookie_get(header_or_cookies, name, [default])", "Reads one cookie from a Cookie header, header dictionary, or cookie dictionary."}},
                    {"http_content_type", {"http_content_type(headers_or_value, [default])", "Extracts a normalized HTTP media type."}},
                    {"http_charset", {"http_charset(headers_or_value, [default])", "Extracts a charset from a Content-Type value."}},
                    {"http_is_json", {"http_is_json(headers_or_value)", "Returns true for JSON HTTP media types."}},
                    {"http_status_ok", {"http_status_ok(status)", "Returns true for 2xx HTTP status codes."}},
                    {"http_status_text", {"http_status_text(status)", "Returns the standard HTTP reason phrase or empty text."}},
                    {"http_status_retryable", {"http_status_retryable(status)", "Returns true for common transient HTTP status codes."}},
                    {"http_retry_after", {"http_retry_after(headers_or_value, [default_ms])", "Parses a Retry-After header value into milliseconds."}},
                    {"http_backoff_delays", {"http_backoff_delays(attempts, [base_ms], [factor], [max_ms])", "Builds capped exponential backoff delays in milliseconds."}},
                    {"env_get", {"env_get(name, [default])", "Reads an environment variable or returns nil/default."}},
                    {"env_require", {"env_require(name)", "Reads a required environment variable or fails."}},
                    {"env_set", {"env_set(name, value)", "Sets a process environment variable."}},
                    {"env_load", {"env_load(path, [override])", "Loads KEY=value pairs from a .env file."}},
                    {"argv", {"argv()", "Returns script command-line arguments passed after the script path."}},
                    {"argc", {"argc()", "Returns the number of script command-line arguments."}},
                    {"script_name", {"script_name()", "Returns the current script or loaded artifact path."}},
                    {"home_dir", {"home_dir()", "Returns the current user's home directory path or empty text if unavailable."}},
                    {"temp_dir", {"temp_dir()", "Returns the host temporary directory path."}},
                    {"path_separator", {"path_separator()", "Returns the host filesystem path separator."}},
                    {"os_name", {"os_name()", "Returns the host OS name such as windows, macos, linux, unix, or unknown."}},
                    {"is_windows", {"is_windows()", "Returns true on Windows hosts."}},
                    {"which", {"which(command)", "Returns the executable path found on PATH or empty text."}},
                    {"cmd_exists", {"cmd_exists(command)", "Returns true when a command can be found on PATH."}},
                    {"command_exists", {"command_exists(command)", "Alias for cmd_exists."}},
                    {"cmd_quote", {"cmd_quote(text)", "Quotes one shell argument for cmd_run."}},
                    {"cmd_join", {"cmd_join(args)", "Quotes and joins an array of shell arguments for cmd_run."}},
                    {"cmd_run", {"cmd_run(command)", "Runs a shell command and returns {command, exit_code, ok, output}."}},
                    {"cmd_run_checked", {"cmd_run_checked(command)", "Runs a shell command, fails on non-zero exit, and returns command result metadata."}},
                    {"db_set", {"db_set(path, key, value)", "Stores a key/value pair in a small JSON database file."}},
                    {"db_get", {"db_get(path, key, [default])", "Reads a value from a small JSON database file."}},
                    {"db_has", {"db_has(path, key)", "Checks whether a small JSON database file has a key."}},
                    {"db_delete", {"db_delete(path, key)", "Deletes a key from a small JSON database file."}},
                    {"db_keys", {"db_keys(path)", "Returns sorted keys from a small JSON database file."}},
                    {"db_all", {"db_all(path)", "Returns the full dictionary from a small JSON database file."}},
                    {"db_insert", {"db_insert(path, row)", "Appends a dictionary row to a small JSON document table."}},
                    {"db_find", {"db_find(path, criteria)", "Returns rows whose fields match a criteria dictionary."}},
                    {"db_count", {"db_count(path, [criteria])", "Counts rows in a JSON document table."}},
                    {"db_update", {"db_update(path, criteria, patch)", "Updates matching rows in a JSON document table."}},
                    {"db_remove", {"db_remove(path, criteria)", "Removes matching rows from a JSON document table."}},
                    {"db_query", {"db_query(path, [criteria], [options])", "Filters rows from a JSON document table with optional sort, offset, and limit."}},
                    {"cli_parse", {"cli_parse(text, [value_flags])", "Parses shell-like flags, quoted values, short options, repeated flags, and positional arguments."}},
                    {"log_set_file", {"log_set_file(path, [append])", "Sends log output to a UTF-8 file, optionally truncating first."}},
                    {"log_set_json", {"log_set_json(enabled)", "Switches log output between text lines and JSON Lines."}},
                    {"log_set_level", {"log_set_level(level)", "Sets the minimum emitted log level."}},
                    {"log_get_level", {"log_get_level()", "Returns the current minimum log level."}},
                    {"log_level", {"log_level([level])", "Gets or sets the current minimum log level."}},
                    {"log_event", {"log_event(level, message, [fields])", "Writes a structured log event with optional fields."}},
                    {"log_debug", {"log_debug(message)", "Writes a debug log line."}},
                    {"log_info", {"log_info(message)", "Writes an info log line."}},
                    {"log_warn", {"log_warn(message)", "Writes a warning log line."}},
                    {"log_error", {"log_error(message)", "Writes an error log line."}},
                    {"console_log", {"console_log(value, ...)", "Writes console values to stdout with spaces and a newline."}},
                    {"console_print", {"console_print(value, ...)", "Alias for console_log(value, ...)."}},
                    {"console_write", {"console_write(value, ...)", "Writes console values to stdout without a newline."}},
                    {"console_write_line", {"console_write_line(value, ...)", "Writes console values to stdout with spaces and a newline."}},
                    {"console_writeln", {"console_writeln(value, ...)", "Alias for console_write_line(value, ...)."}},
                    {"console_println", {"console_println(value, ...)", "Alias for console_write_line(value, ...)."}},
                    {"console_line", {"console_line([value, ...])", "Writes one console line."}},
                    {"console_info", {"console_info(value, ...)", "Writes informational console values to stdout."}},
                    {"console_debug", {"console_debug(value, ...)", "Writes debug console values to stdout."}},
                    {"console_warn", {"console_warn(value, ...)", "Writes warning console values to stderr."}},
                    {"console_warning", {"console_warning(value, ...)", "Alias for console_warn(value, ...)."}},
                    {"console_error", {"console_error(value, ...)", "Writes error console values to stderr."}},
                    {"console_exception", {"console_exception(value, ...)", "Alias for console_error(value, ...)."}},
                    {"console_clear", {"console_clear()", "Clears an interactive terminal when supported."}},
                    {"console_assert", {"console_assert(condition, [message...])", "Writes an assertion failure to stderr when condition is false."}},
                    {"console_time", {"console_time([label])", "Starts or resets a named console timer."}},
                    {"console_time_log", {"console_time_log([label], [message...])", "Prints elapsed milliseconds without stopping the named timer."}},
                    {"console_time_end", {"console_time_end([label])", "Stops a named console timer, prints elapsed milliseconds, and returns the duration."}},
                    {"console_time_stamp", {"console_time_stamp([label])", "Writes a timestamp marker and returns milliseconds."}},
                    {"console_count", {"console_count([label])", "Increments and prints a named counter."}},
                    {"console_count_reset", {"console_count_reset([label])", "Resets a named console counter."}},
                    {"console_table", {"console_table(value)", "Prints arrays and dictionaries as a compact text table."}},
                    {"console_dir", {"console_dir(value, [options])", "Prints one structured value."}},
                    {"console_dirxml", {"console_dirxml(value, ...)", "Prints XML/structured values using Sura runtime formatting."}},
                    {"console_trace", {"console_trace([message...])", "Writes a trace line with the current runtime line."}},
                    {"console_group", {"console_group([label...])", "Starts an indented console output group."}},
                    {"console_group_collapsed", {"console_group_collapsed([label...])", "Alias for console_group in terminal output."}},
                    {"console_group_end", {"console_group_end()", "Ends the current indented console output group."}},
                    {"console_profile", {"console_profile([label])", "Starts a lightweight named console profile timer."}},
                    {"console_profile_end", {"console_profile_end([label])", "Stops a console profile timer and returns elapsed milliseconds."}},
                    {"console_style", {"console_style(text, style_or_styles)", "Returns text wrapped in ANSI style escape codes."}},
                    {"console_color", {"console_color(text, fg, [bg])", "Returns text wrapped in ANSI foreground/background color escape codes."}},
                    {"console_colour", {"console_colour(text, fg, [bg])", "Alias for console_color(text, fg, [bg])."}},
                    {"console_strip_ansi", {"console_strip_ansi(text)", "Removes ANSI escape codes from text."}},
                    {"console_set_color", {"console_set_color(fg, [bg])", "Sets the current terminal ANSI color."}},
                    {"console_set_colour", {"console_set_colour(fg, [bg])", "Alias for console_set_color(fg, [bg])."}},
                    {"console_reset_color", {"console_reset_color()", "Resets terminal ANSI color and style."}},
                    {"console_reset_colour", {"console_reset_colour()", "Alias for console_reset_color()."}},
                    {"console_is_tty", {"console_is_tty()", "Returns whether stdout is an interactive terminal."}},
                    {"console_width", {"console_width()", "Returns terminal width in columns, or 0 when unknown."}},
                    {"console_height", {"console_height()", "Returns terminal height in rows, or 0 when unknown."}},
                    {"console_size", {"console_size()", "Returns terminal width, height, and is_tty fields."}},
                    {"console_input", {"console_input([prompt])", "Reads one line from stdin."}},
                    {"console_read_line", {"console_read_line([prompt])", "Reads one line from stdin."}},
                    {"console_readline", {"console_readline([prompt])", "Alias for console_read_line([prompt])."}},
                    {"console_readLine", {"console_readLine([prompt])", "Alias for console_read_line([prompt])."}},
                    {"console_prompt", {"console_prompt([prompt])", "Alias for console_read_line([prompt])."}},
                    {"vector_add", {"vector_add(a, b)", "Adds two numeric vectors element by element."}},
                    {"vector_dot", {"vector_dot(a, b)", "Returns the dot product for two numeric vectors."}},
                    {"vector_scale", {"vector_scale(vector, scalar)", "Scales a numeric vector."}},
                    {"vector_norm", {"vector_norm(vector)", "Returns the Euclidean norm of a numeric vector."}},
                    {"vector_cosine", {"vector_cosine(a, b)", "Returns cosine similarity for two numeric vectors."}},
                    {"vector_normalize", {"vector_normalize(vector)", "Returns a unit-length numeric vector."}},
                    {"vector_search", {"vector_search(query, rows, [k], [field])", "Ranks embedding arrays or dictionaries by cosine similarity."}},
                    {"mat4_identity", {"mat4_identity()", "Returns a row-major 4x4 identity transform matrix."}},
                    {"mat4_translate", {"mat4_translate(x, y, z)", "Returns a row-major 4x4 translation matrix."}},
                    {"mat4_scale", {"mat4_scale(x, y, z)", "Returns a row-major 4x4 scale matrix."}},
                    {"mat4_rotate_y", {"mat4_rotate_y(radians)", "Returns a row-major 4x4 Y-axis rotation matrix."}},
                    {"mat4_mul", {"mat4_mul(a, b)", "Multiplies two row-major 4x4 transform matrices."}},
                    {"mesh_cube", {"mesh_cube([size], [center])", "Builds a cube mesh dictionary with vertices, faces, and edges."}},
                    {"mesh_transform4", {"mesh_transform4(mesh, matrix)", "Applies a 4x4 transform matrix to mesh vertices."}},
                    {"mesh_bounds", {"mesh_bounds(mesh)", "Returns min, max, size, and center vectors for a mesh."}},
                    {"mesh_face_normals", {"mesh_face_normals(mesh)", "Returns normalized face normal vectors for a mesh."}},
                    {"camera_project", {"camera_project(point, camera, [width], [height])", "Projects a 3D point through a simple perspective camera."}},
                    {"rag_context", {"rag_context(query, docs, [k], [embedding_field], [text_field])", "Builds a top-K text context from embedded documents."}},
                    {"rag_sources", {"rag_sources(query, docs, [k], [embedding_field], [text_field], [title_field])", "Returns ranked RAG source dictionaries with text, score, item, and optional title/id."}},
                    {"rag_prepare", {"rag_prepare(question, query, docs, [k], [embedding_field], [text_field], [system], [title_field])", "Builds grounded context, sources, and chat messages for a RAG answer."}},
                    {"tensor_shape", {"tensor_shape(tensor)", "Returns tensor dimensions for nested arrays."}},
                    {"tensor_zeros", {"tensor_zeros(shape)", "Creates a zero-filled tensor from a shape array."}},
                    {"tensor_fill", {"tensor_fill(shape, value)", "Creates a tensor filled with a numeric value."}},
                    {"tensor_add", {"tensor_add(a, b)", "Adds tensors elementwise with scalar broadcasting."}},
                    {"tensor_mul", {"tensor_mul(a, b)", "Multiplies tensors elementwise with scalar broadcasting."}},
                    {"tensor_clip", {"tensor_clip(tensor, min, max)", "Clamps numeric tensor leaves to an inclusive range while preserving shape."}},
                    {"tensor_flatten", {"tensor_flatten(tensor)", "Flattens a numeric tensor into one array."}},
                    {"tensor_sum", {"tensor_sum(tensor)", "Returns the sum of all numeric tensor leaves."}},
                    {"tensor_mean", {"tensor_mean(tensor)", "Returns the average of numeric tensor leaves, or nil for an empty tensor."}},
                    {"tensor_variance", {"tensor_variance(tensor)", "Returns the population variance of numeric tensor leaves, or nil for an empty tensor."}},
                    {"tensor_std", {"tensor_std(tensor)", "Returns the population standard deviation of numeric tensor leaves, or nil for an empty tensor."}},
                    {"tensor_min", {"tensor_min(tensor)", "Returns the minimum numeric tensor leaf, or nil for an empty tensor."}},
                    {"tensor_max", {"tensor_max(tensor)", "Returns the maximum numeric tensor leaf, or nil for an empty tensor."}},
                    {"tensor_argmin", {"tensor_argmin(tensor)", "Returns the flattened index of the first minimum numeric tensor leaf, or nil for an empty tensor."}},
                    {"tensor_argmax", {"tensor_argmax(tensor)", "Returns the flattened index of the first maximum numeric tensor leaf, or nil for an empty tensor."}},
                    {"tensor_zscore", {"tensor_zscore(tensor)", "Returns a z-score normalized tensor while preserving the input shape."}},
                    {"tensor_softmax", {"tensor_softmax(tensor)", "Returns a stable softmax probability tensor while preserving the input shape."}},
                    {"tensor_transpose", {"tensor_transpose(matrix)", "Transposes a 2D numeric matrix."}},
                    {"tensor_matmul", {"tensor_matmul(a, b)", "Multiplies two 2D numeric matrices."}},
                    {"nn_mlp", {"nn_mlp(layer_sizes, [options])", "Creates a deterministic, JSON-serializable dense neural network."}},
                    {"nn_forward", {"nn_forward(model, inputs)", "Runs one sample or a batch through a native neural network."}},
                    {"nn_predict", {"nn_predict(model, inputs)", "Returns neural-network probabilities or regression outputs."}},
                    {"nn_train", {"nn_train(model, inputs, targets, [options])", "Trains a model with native backpropagation and Adam or SGD."}},
                    {"nn_classify", {"nn_classify(model, inputs, [threshold])", "Returns binary, multilabel, or softmax class predictions."}},
                    {"nn_evaluate", {"nn_evaluate(model, inputs, targets, [options])", "Returns loss and classification accuracy for a dataset."}},
                    {"nn_summary", {"nn_summary(model)", "Returns architecture, activations, and parameter count."}},
                    {"nn_one_hot", {"nn_one_hot(labels, class_count)", "Converts zero-based class indexes to one-hot target vectors."}},
                    {"nn_fit_standardizer", {"nn_fit_standardizer(inputs)", "Fits reusable per-feature mean and scale statistics."}},
                    {"nn_standardize", {"nn_standardize(inputs, standardizer)", "Applies fitted per-feature standardization to one sample or a batch."}},
                    {"nn_split", {"nn_split(inputs, targets, [options])", "Creates deterministic paired train/test dataset partitions."}},
                    {"nn_save", {"nn_save(model, path)", "Validates and saves a neural-network model as portable JSON."}},
                    {"nn_load", {"nn_load(path)", "Loads and validates a neural-network model from JSON."}},
                    {"autograd_tensor", {"autograd_tensor(data, [options])", "Creates a typed contiguous tensor; options include requires_grad, dtype, and cpu/cuda device."}},
                    {"autograd_parameter", {"autograd_parameter(data, [dtype_or_options])", "Creates a typed trainable leaf tensor."}},
                    {"autograd_zeros", {"autograd_zeros(shape, [options])", "Creates a typed contiguous tensor filled with zeros."}},
                    {"autograd_ones", {"autograd_ones(shape, [options])", "Creates a typed contiguous tensor filled with ones."}},
                    {"autograd_randn", {"autograd_randn(shape, [options])", "Creates a deterministic normally distributed tensor; options include mean, std, seed, requires_grad, and dtype."}},
                    {"autograd_data", {"autograd_data(tensor)", "Returns a nested-array copy of a tensor's values."}},
                    {"autograd_grad", {"autograd_grad(tensor)", "Returns a nested-array copy of accumulated gradients, or nil when no gradient exists."}},
                    {"autograd_grad_info", {"autograd_grad_info(tensor)", "Returns gradient storage, loss-scale, leaf/requires-grad, and basic optimizer-readiness metadata without reading gradient payload."}},
                    {"autograd_dtype", {"autograd_dtype(tensor)", "Returns the packed tensor storage dtype."}},
                    {"autograd_device", {"autograd_device(tensor)", "Returns cpu or the resident cuda:N device."}},
                    {"autograd_to", {"autograd_to(tensor, device)", "Copies a tensor explicitly between CPU and CUDA."}},
                    {"autograd_storage_bytes", {"autograd_storage_bytes(tensor)", "Returns packed parameter storage bytes excluding gradients and optimizer state."}},
                    {"autograd_cast", {"autograd_cast(tensor, dtype)", "Differentiably casts a tensor to float64, float32, float16, or bfloat16 storage."}},
                    {"autograd_shape", {"autograd_shape(tensor)", "Returns the tensor shape as an integer array."}},
                    {"autograd_numel", {"autograd_numel(tensor)", "Returns the number of scalar elements in a tensor."}},
                    {"autograd_limits", {"autograd_limits()", "Returns active CPU/dtype, tensor, graph, memory, CUDA fused-attention dispatch, and tracked-usage limits."}},
                    {"autograd_autocast", {"autograd_autocast([state])", "Queries or changes thread-local resident-CUDA matmul autocast; setters return the previous state for safe restoration."}},
                    {"autograd_item", {"autograd_item(tensor)", "Returns the numeric value of a one-element tensor."}},
                    {"autograd_detach", {"autograd_detach(tensor)", "Copies tensor values into a leaf tensor detached from the gradient graph."}},
                    {"autograd_set_requires_grad", {"autograd_set_requires_grad(tensor, requires_grad)", "Enables or disables gradient tracking on a leaf tensor."}},
                    {"autograd_requires_grad", {"autograd_requires_grad(tensor)", "Returns whether a tensor tracks operations for gradient computation."}},
                    {"autograd_add", {"autograd_add(a, b)", "Adds tensors with trailing-dimension broadcasting and records the operation for backpropagation."}},
                    {"autograd_sub", {"autograd_sub(a, b)", "Subtracts tensors with trailing-dimension broadcasting and records the operation for backpropagation."}},
                    {"autograd_mul", {"autograd_mul(a, b)", "Multiplies tensors with trailing-dimension broadcasting and records the operation for backpropagation."}},
                    {"autograd_div", {"autograd_div(a, b)", "Divides tensors with trailing-dimension broadcasting and records the operation for backpropagation."}},
                    {"autograd_neg", {"autograd_neg(tensor)", "Negates a tensor elementwise while preserving its gradient graph."}},
                    {"autograd_reshape", {"autograd_reshape(tensor, shape)", "Reshapes a tensor with one optional inferred -1 dimension and records the operation for backpropagation."}},
                    {"autograd_matmul", {"autograd_matmul(a, b, [options])", "Multiplies tensors on CPU; resident float32 CUDA supports ND matmul plus float32, float16, or bfloat16 compute selected explicitly or by autocast."}},
                    {"autograd_transpose", {"autograd_transpose(tensor, [axis1], [axis2])", "Swaps the last two axes by default, or two explicitly supplied axes, with automatic differentiation."}},
                    {"autograd_linear", {"autograd_linear(input, weights, [bias])", "Applies a differentiable matrix multiplication and optional broadcast bias."}},
                    {"autograd_relu", {"autograd_relu(tensor)", "Applies ReLU elementwise with automatic differentiation."}},
                    {"autograd_tanh", {"autograd_tanh(tensor)", "Applies hyperbolic tangent elementwise with automatic differentiation."}},
                    {"autograd_sigmoid", {"autograd_sigmoid(tensor)", "Applies a numerically stable sigmoid elementwise with automatic differentiation."}},
                    {"autograd_gelu", {"autograd_gelu(tensor)", "Applies exact GELU elementwise with automatic differentiation."}},
                    {"autograd_layer_norm", {"autograd_layer_norm(tensor, [weight], [bias], [epsilon])", "Normalizes the last tensor dimension with optional affine parameters and automatic differentiation."}},
                    {"autograd_embedding", {"autograd_embedding(token_ids, weight)", "Looks up CPU integer token IDs in a CPU or resident float32 CUDA weight table and scatter-adds gradients into the vocabulary rows."}},
                    {"autograd_causal_attention", {"autograd_causal_attention(query, key, value, [options])", "Applies differentiable scaled causal attention; options include scale and precision auto/fast/strict, where CUDA fast requires the fused f32 path and strict pins the f64 reference path."}},
                    {"autograd_softmax", {"autograd_softmax(tensor)", "Applies stable softmax across the last tensor dimension with automatic differentiation."}},
                    {"autograd_sum", {"autograd_sum(tensor)", "Reduces all tensor values to a differentiable scalar sum."}},
                    {"autograd_mean", {"autograd_mean(tensor)", "Reduces all tensor values to a differentiable scalar mean."}},
                    {"autograd_mse", {"autograd_mse(prediction, target)", "Returns differentiable mean squared error for equal-shaped tensors."}},
                    {"autograd_bce", {"autograd_bce(probabilities, target)", "Returns differentiable binary cross-entropy for probability inputs."}},
                    {"autograd_bce_logits", {"autograd_bce_logits(logits, target)", "Returns numerically stable differentiable binary cross-entropy from logits."}},
                    {"autograd_cross_entropy", {"autograd_cross_entropy(logits, one_hot_targets)", "Returns stable multiclass cross-entropy from logits and one-hot targets."}},
                    {"autograd_cross_entropy_ids", {"autograd_cross_entropy_ids(logits, class_ids)", "Returns stable mean multiclass cross-entropy from CPU integer class IDs, with a fused resident float32 CUDA forward/backward path."}},
                    {"autograd_backward", {"autograd_backward(tensor, [gradient], [retain_graph])", "Runs reverse-mode automatic differentiation from a scalar or explicitly seeded tensor."}},
                    {"autograd_backward_scaled", {"autograd_backward_scaled(loss, scale, [retain_graph])", "Seeds a resident-CUDA scalar loss with a declared scale and records that scale on persistent leaf gradients."}},
                    {"autograd_zero_grad", {"autograd_zero_grad(parameters)", "Zeros accumulated gradients for one parameter or a nested parameter array."}},
                    {"autograd_unscale_gradients", {"autograd_unscale_gradients(parameters, [scale])", "Transactionally unscales resident-CUDA gradients and reports found_inf without partially committing a parameter list."}},
                    {"autograd_sgd", {"autograd_sgd(parameters, learning_rate, [options])", "Updates leaf parameters with SGD; CUDA updates are transactional and options include momentum and weight_decay."}},
                    {"autograd_adam", {"autograd_adam(parameters, learning_rate, [options])", "Updates leaf parameters with Adam; options include beta1, beta2, epsilon, and weight_decay."}},
                    {"autograd_reset_optimizer", {"autograd_reset_optimizer(parameters)", "Clears optimizer state stored on one parameter or a nested parameter array."}},
                    {"autograd_grad_norm", {"autograd_grad_norm(parameters)", "Returns the global L2 norm of available parameter gradients."}},
                    {"autograd_clip_grad_norm", {"autograd_clip_grad_norm(parameters, max_norm)", "Clips gradients to a global L2 norm and returns the pre-clipping norm."}},
                    {"autograd_save_checkpoint", {"autograd_save_checkpoint(state_dict, path, [options])", "Writes a v3 SHA-256 checkpoint, including CUDA master/optimizer state unless optimizer is false."}},
                    {"autograd_load_checkpoint", {"autograd_load_checkpoint(path, [options])", "Loads v1-v3 leaf tensors; use {optimizer: true, device: \"cuda\"} for exact CUDA optimizer resume."}},
                    {"autograd_cuda_available", {"autograd_cuda_available()", "Returns whether the NVIDIA Driver/PTX CUDA backend initialized successfully."}},
                    {"autograd_cuda_info", {"autograd_cuda_info()", "Returns CUDA device, optional dynamic cuBLAS selection, active matmul backend, and implemented-kernel coverage metadata."}},
                    {"autograd_cuda_stats", {"autograd_cuda_stats()", "Returns CUDA allocation, transfer, cuBLAS/reference matmul, optimizer, fused/fast attention, and kernel-launch counters."}},
                    {"autograd_cuda_reset_stats", {"autograd_cuda_reset_stats()", "Resets CUDA counters while preserving current allocation accounting."}},
                    {"autograd_cuda_synchronize", {"autograd_cuda_synchronize()", "Synchronizes the active CUDA context and surfaces asynchronous errors."}},
                    {"autograd_save_safetensors", {"autograd_save_safetensors(state_dict, path)", "Writes typed PyTorch-compatible Safetensors weights atomically."}},
                    {"autograd_load_safetensors", {"autograd_load_safetensors(path, [options])", "Strictly validates and loads typed Safetensors weights."}},
                    {"autograd_save_onnx_weights", {"autograd_save_onnx_weights(state_dict, path)", "Writes a valid ONNX ModelProto exposing typed initializers through Identity outputs."}},
                    {"autograd_load_onnx_weights", {"autograd_load_onnx_weights(path, [options])", "Loads supported raw-data initializers without executing graph nodes; use autograd_run_onnx for the bounded CPU execution subset."}},
                    {"autograd_run_onnx", {"autograd_run_onnx(path, inputs, [options])", "Executes a validated CPU ONNX opset 7-18 inference subset and returns named tensor outputs."}},
                    {"autograd_all_reduce_gradients", {"autograd_all_reduce_gradients(parameters, options)", "Synchronously sums or averages gradients across shared-filesystem ranks."}},
                    {"tokenizer_byte", {"tokenizer_byte([options])", "Creates a deterministic UTF-8 byte tokenizer with optional BOS/EOS/PAD IDs."}},
                    {"tokenizer_train_bpe", {"tokenizer_train_bpe(corpus, [options])", "Deterministically trains a bounded byte-level BPE tokenizer."}},
                    {"tokenizer_encode", {"tokenizer_encode(tokenizer, text, [options])", "Encodes text with a byte or byte-level BPE tokenizer."}},
                    {"tokenizer_decode", {"tokenizer_decode(tokenizer, ids, [options])", "Losslessly decodes byte or BPE token IDs to UTF-8 text."}},
                    {"tokenizer_info", {"tokenizer_info(tokenizer)", "Returns tokenizer type, vocabulary, merge, and special-token metadata."}},
                    {"tokenizer_save", {"tokenizer_save(tokenizer, path)", "Atomically saves a versioned corruption-detecting tokenizer file."}},
                    {"tokenizer_load", {"tokenizer_load(path)", "Loads and validates a native tokenizer file."}},
                    {"dataset_pack_text", {"dataset_pack_text(source, tokenizer, path, [options])", "Streams text or source files into a bounded uint32 token dataset."}},
                    {"dataset_open", {"dataset_open(path, [options])", "Validates and opens a deterministic seek-based training loader."}},
                    {"dataset_next", {"dataset_next(loader)", "Reads one shifted input/target token batch without materializing the shard."}},
                    {"dataset_reset", {"dataset_reset(loader, [epoch])", "Resets loader position and deterministic epoch shuffling."}},
                    {"dataset_close", {"dataset_close(loader)", "Closes a logical dataset handle."}},
                    {"dataset_info", {"dataset_info(loader)", "Returns dataset and loader metadata."}},
                    {"media_available", {"media_available([ffmpeg_path])", "Returns whether the configured FFmpeg decoder is executable."}},
                    {"media_ffmpeg_available", {"media_ffmpeg_available([ffmpeg_path])", "Alias for media_available."}},
                    {"media_frame_to_text", {"media_frame_to_text(pixels, [options])", "Converts a grayscale, RGB, or RGBA pixel matrix to one ASCII text frame."}},
                    {"media_ascii_frames", {"media_ascii_frames(path, [options])", "Safely decodes a local video into bounded ASCII text frames."}},
                    {"media_video_to_text", {"media_video_to_text(path, [options])", "Alias for media_ascii_frames."}},
                    {"media_video_text_frames", {"media_video_text_frames(path, [options])", "Alias for media_ascii_frames."}},
                    {"stream_from", {"stream_from(array_or_text)", "Creates a consumable stream from an array or newline-delimited text."}},
                    {"stream_next", {"stream_next(stream)", "Returns the next stream item or nil at the end."}},
                    {"stream_take", {"stream_take(stream, count)", "Consumes and returns up to count stream items."}},
                    {"stream_batch", {"stream_batch(stream, size)", "Consumes remaining stream items and returns arrays grouped by size."}},
                    {"stream_map", {"stream_map(stream, path, [fallback])", "Consumes remaining stream items and returns a stream of JSON-path values."}},
                    {"stream_filter", {"stream_filter(stream, criteria)", "Consumes remaining stream items and returns a stream of matching dictionaries."}},
                    {"stream_window", {"stream_window(stream, size, [step])", "Consumes remaining stream items and returns rolling windows."}},
                    {"stream_skip", {"stream_skip(stream, count)", "Consumes up to count stream items and returns the skipped count."}},
                    {"stream_count", {"stream_count(stream)", "Returns the number of remaining stream items without consuming them."}},
                    {"stream_join", {"stream_join(stream, [separator])", "Consumes remaining stream items and joins them as text."}},
                    {"stream_sum", {"stream_sum(stream, [path])", "Consumes remaining numeric stream items and returns their sum."}},
                    {"stream_avg", {"stream_avg(stream, [path])", "Consumes remaining numeric stream items and returns their average or nil for empty streams."}},
                    {"stream_collect", {"stream_collect(stream)", "Consumes and returns all remaining stream items."}},
                    {"stream_lines", {"stream_lines(path)", "Creates a stream over the lines of a text file."}},
                    {"http_get", {"http_get(url)", "Fetches an http, https, or file URL as text."}},
                    {"http_json", {"http_json(url)", "Fetches a URL and parses the response as JSON."}},
                    {"http_post", {"http_post(url, body, [content_type])", "Sends an HTTP POST request and returns the response body."}},
                    {"http_request", {"http_request(spec)", "Sends an HTTP request spec with method, url, query, headers, body/json/form, and timeout."}},
                    {"http_request_full", {"http_request_full(spec)", "Sends an HTTP request spec and returns status, ok, body, headers, and url."}},
                    {"http_request_retry", {"http_request_retry(spec, [attempts], [delay_ms])", "Retries an HTTP request until it returns an ok full response."}},
                    {"http_request_retry_json", {"http_request_retry_json(spec, [attempts], [delay_ms])", "Retries an HTTP request and parses the final response body as JSON."}},
                    {"http_request_retry_json_checked", {"http_request_retry_json_checked(spec, [attempts], [delay_ms])", "Retries an HTTP request, requires a 2xx final response, and parses JSON."}},
                    {"http_request_json", {"http_request_json(spec)", "Sends an HTTP request spec and parses the response as JSON."}},
                    {"http_request_json_checked", {"http_request_json_checked(spec)", "Sends an HTTP request, requires a 2xx response, and parses JSON."}},
                    {"async_http_get", {"async_http_get(url, [scope_id])", "Fetches an http, https, or file URL on the bounded async runtime and returns a task id."}},
                    {"async_http_request", {"async_http_request(spec, [scope_id])", "Sends an HTTP request spec on the bounded async runtime and returns a task id."}},
                    {"async_sleep", {"async_sleep(milliseconds, [scope_id])", "Starts a cooperatively cancellable timer task."}},
                    {"async_sura", {"async_sura(spec, [scope_id])", "Runs a snapshotted Sura program in an isolated child process; spec contains program, JSON-safe input, and optional timeout_ms."}},
                    {"http_serve_static", {"http_serve_static(path, [port])", "Starts a static file server task."}},
                    {"http_serve_routes", {"http_serve_routes(routes, [port])", "Starts a local mock API server from route response specs."}},
                    {"http_server_url", {"http_server_url(server)", "Returns a static server URL from a server handle."}},
                    {"http_server_stop", {"http_server_stop(server)", "Stops a static server task."}},
                    {"tool_call", {"tool_call(spec)", "Runs a declared automation tool such as shell or http_get."}},
                    {"tool", {"tool(spec)", "Alias for tool_call."}},
                    {"tool_spec", {"tool_spec(name, args)", "Builds and validates a typed tool spec dictionary."}},
                    {"tool_validate", {"tool_validate(spec)", "Returns true when a raw tool spec has required typed fields."}},
                    {"tool_schema", {"tool_schema(name)", "Returns the required fields for a built-in tool."}},
                    {"tool_allowed", {"tool_allowed(spec, policy)", "Returns true when a tool spec is allowed by policy."}},
                    {"tool_call_policy", {"tool_call_policy(spec, policy)", "Runs a tool only when policy allows it."}},
                    {"tool_list", {"tool_list()", "Returns the built-in tool names."}},
                    {"llm_message", {"llm_message(role, content)", "Builds an OpenAI-style chat message dictionary."}},
                    {"llm_messages", {"llm_messages([system], user)", "Builds a chat messages array."}},
                    {"rag_messages", {"rag_messages(question, context, [system])", "Builds grounded OpenAI-style messages from a question and RAG context."}},
                    {"llm_request", {"llm_request(model, messages, [temperature])", "Builds an OpenAI-style chat request dictionary."}},
                    {"llm_request_json", {"llm_request_json(model, messages, [temperature])", "Serializes an LLM chat request."}},
                    {"llm_response_schema", {"llm_response_schema(name, schema, [strict])", "Builds an OpenAI-compatible structured-output response_format."}},
                    {"llm_request_schema", {"llm_request_schema(model, messages, schema, [temperature], [name], [strict])", "Builds a chat request that asks for JSON Schema-constrained output."}},
                    {"llm_request_schema_json", {"llm_request_schema_json(model, messages, schema, [temperature], [name], [strict])", "Serializes a schema-constrained LLM chat request."}},
                    {"llm_tools", {"llm_tools([names])", "Builds OpenAI-compatible function tool definitions from Sura tool schemas."}},
                    {"llm_tool_schemas", {"llm_tool_schemas([names])", "Alias for llm_tools."}},
                    {"llm_request_tools", {"llm_request_tools(model, messages, tool_names, [temperature])", "Builds a chat request with OpenAI-compatible function tools."}},
                    {"llm_request_tools_json", {"llm_request_tools_json(model, messages, tool_names, [temperature])", "Serializes a tool-enabled LLM chat request."}},
                    {"llm_request_tools_schema", {"llm_request_tools_schema(model, messages, tool_names, schema, [temperature], [name], [strict])", "Builds a chat request with function tools and JSON Schema-constrained output."}},
                    {"llm_request_tools_schema_json", {"llm_request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])", "Serializes a tool-enabled schema-constrained LLM chat request."}},
                    {"llm_extract_text", {"llm_extract_text(response)", "Extracts assistant text from an OpenAI-style response."}},
                    {"llm_extract_json", {"llm_extract_json(response, [schema])", "Parses assistant text as JSON and optionally validates it against a schema."}},
                    {"llm_usage", {"llm_usage(response)", "Normalizes LLM token usage fields from OpenAI-style responses."}},
                    {"llm_cost", {"llm_cost(response, pricing)", "Calculates LLM input/output costs from normalized token usage and per-million pricing."}},
                    {"llm_budget", {"llm_budget(response, pricing, limit)", "Calculates LLM cost and reports whether it is within a budget limit."}},
                    {"llm_tool_calls", {"llm_tool_calls(response)", "Extracts OpenAI-style tool calls as executable Sura tool specs."}},
                    {"llm_tool_result", {"llm_tool_result(call_or_id, result)", "Builds an OpenAI-style tool result message."}},
                    {"llm_run_tools", {"llm_run_tools(response, policy)", "Runs extracted LLM tool calls through a policy and returns tool result messages."}},
                    {"llm_next_messages", {"llm_next_messages(messages, response, policy)", "Appends an assistant response and policy-executed tool result messages for the next LLM request."}},
                    {"llm_next_request", {"llm_next_request(model, messages, response, policy, tool_names, [temperature])", "Builds the next tool-enabled LLM request after a policy-executed tool round."}},
                    {"llm_next_request_json", {"llm_next_request_json(model, messages, response, policy, tool_names, [temperature])", "Serializes the next tool-enabled LLM request after a tool round."}},
                    {"llm_next_schema_request", {"llm_next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])", "Builds the next tool-enabled schema-constrained LLM request after a tool round."}},
                    {"llm_next_schema_request_json", {"llm_next_schema_request_json(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])", "Serializes the next tool-enabled schema-constrained LLM request after a tool round."}},
                    {"llm_stream_text", {"llm_stream_text(sse_or_chunks)", "Combines OpenAI-style streaming delta chunks into text."}},
                    {"llm_chat", {"llm_chat(endpoint, api_key, model, messages, [temperature])", "Calls an OpenAI-compatible chat endpoint."}},
                    {"llm_chat_request", {"llm_chat_request(endpoint, api_key, request)", "Posts a prebuilt OpenAI-compatible chat request dict or JSON body."}},
                    {"python_available", {"python_available()", "Returns true when a Python interpreter is available."}},
                    {"python_executable", {"python_executable()", "Returns the Python executable selected by Sura."}},
                    {"python_eval", {"python_eval(code)", "Evaluates Python code and returns stdout."}},
                    {"python_call", {"python_call(module, function, [args], [kwargs])", "Calls a Python function with JSON-compatible arguments."}},
                    {"python_call_json", {"python_call_json(module, function, [args], [kwargs])", "Calls Python and parses JSON output into Sura values."}},
                    {"ffi_load", {"ffi_load(path)", "Loads a native dynamic library path for simple C ABI calls."}},
                    {"ffi_call", {"ffi_call(lib, symbol, signature, ...args)", "Calls a simple C ABI function with numeric or C string arguments."}},
                    {"plugin_load", {"plugin_load(path)", "Loads a native Sura plugin library."}},
                    {"plugin_load_manifest", {"plugin_load_manifest(path)", "Loads a plugin through a manifest allow-list."}},
                    {"plugin_call", {"plugin_call(plugin, export, ...args)", "Calls an export from a loaded native plugin."}},
                    {"plugin_info", {"plugin_info(plugin)", "Returns plugin descriptor information."}},
                    {"plugin_unload", {"plugin_unload(plugin)", "Unloads a native plugin handle."}},
                    {"async_cmd", {"async_cmd(command, [scope_id])", "Runs a cancellable child process on the bounded async runtime and returns a task id."}},
                    {"task", {"task(command)", "Alias for async_cmd."}},
                    {"async_ready", {"async_ready(task_id)", "Checks whether an async task has completed."}},
                    {"async_status", {"async_status(task_id)", "Returns the task state, cancellation flag, scope, and retained failure without consuming it."}},
                    {"async_pending", {"async_pending()", "Returns status dictionaries for all currently tracked async tasks."}},
                    {"async_forget", {"async_forget(task_id)", "Drops a completed async task without reading its output."}},
                    {"async_cleanup", {"async_cleanup()", "Drops all completed async tasks and returns the count removed."}},
                    {"async_cancel", {"async_cancel(task_id)", "Cooperatively cancels a queued or running task."}},
                    {"async_cancelled", {"async_cancelled(task_id)", "Checks whether a retained task reached the cancelled state."}},
                    {"async_configure", {"async_configure(max_workers, max_queue)", "Configures a quiescent bounded async worker pool."}},
                    {"async_limits", {"async_limits()", "Returns worker, queue, tracked-task, and scope limits and counts."}},
                    {"async_scope_open", {"async_scope_open()", "Creates an explicit structured-concurrency scope."}},
                    {"async_scope_attach", {"async_scope_attach(scope_id, task_id)", "Attaches an unscoped task to an open scope."}},
                    {"async_scope_cancel", {"async_scope_cancel(scope_id)", "Requests cancellation for every child in a scope."}},
                    {"async_scope_status", {"async_scope_status(scope_id)", "Returns aggregate child states for a scope."}},
                    {"async_scope_close", {"async_scope_close(scope_id, [milliseconds])", "Closes a scope, cancels children, joins them, cleans handles, and propagates failures."}},
                    {"async_scope_join", {"async_scope_join(scope_id, [milliseconds])", "Closes a scope to new children, joins existing children, cleans handles, and propagates failures."}},
                    {"async_await", {"async_await(task_id)", "Waits for an async task and returns captured output."}},
                    {"async_await_timeout", {"async_await_timeout(task_id, milliseconds, [default])", "Waits up to a timeout and leaves the task pending if it is not ready."}},
                    {"async_ready_all", {"async_ready_all(task_ids)", "Checks whether all async command tasks have completed."}},
                    {"async_any", {"async_any(task_ids, [milliseconds], [default])", "Waits for the first completed async command and returns {id,index,output}."}},
                    {"async_all", {"async_all(task_ids)", "Waits for multiple async command tasks and returns outputs."}},
                    {"async_all_timeout", {"async_all_timeout(task_ids, milliseconds, [default])", "Waits up to a timeout for all async command tasks without consuming pending tasks."}},
                    {"await_all", {"await_all(task_ids)", "Alias for async_all."}},
                    {"await_all_timeout", {"await_all_timeout(task_ids, milliseconds, [default])", "Alias for async_all_timeout."}},
                    {"await_any", {"await_any(task_ids, [milliseconds], [default])", "Alias for async_any."}},
                    {"await_timeout", {"await_timeout(task_id, milliseconds, [default])", "Alias for async_await_timeout."}},
                    {"await", {"await(task_id)", "Alias for async_await."}},
                    {"sleep_ms", {"sleep_ms(milliseconds)", "Sleeps for a duration."}},
                    {"wait", {"wait(milliseconds)", "Alias for sleep_ms."}}
                };

                auto it = docs.find(name);
                if (it != docs.end()) {
                    info.label = it->second.first;
                    info.documentation = it->second.second;
                    info.parameters = split_signature_parameters(info.label);
                    return true;
                }
                if (std::binary_search(builtin_names.begin(), builtin_names.end(), name)) {
                    info.label = name + "(...)";
                    info.documentation = "Sura stdlib function.";
                    info.parameters = {"..."};
                    return true;
                }
                return false;
            };

            for (const auto& name : builtin_names) {
                LspSignatureInfo info;
                std::string detail = builtin_signature_for(name, info) ? info.label : "Sura stdlib function";
                completions.push_back({name, detail});
            }

            auto canonical_lsp_module = [](std::string module) {
                if (module == "logging") return std::string("log");
                if (module == "filesystem" || module == "file") return std::string("fs");
                if (module == "time") return std::string("datetime");
                if (module == "web") return std::string("http");
                if (module == "data") return std::string("json");
                if (module == "testing") return std::string("test");
                if (module == "rng") return std::string("random");
                if (module == "py") return std::string("python");
                if (module == "ai") return std::string("nn");
                return module;
            };

            auto lsp_module_methods = [&](const std::string& raw_module) {
                std::string module = canonical_lsp_module(raw_module);
                std::vector<std::pair<std::string, std::string>> methods;
                auto add = [&](const std::string& label, const std::string& detail) {
                    methods.push_back({label, detail});
                };
                if (module == "array") {
                    add("len", "array.len(array)");
                    add("length", "array.length(array)");
                    add("size", "array.size(array)");
                    add("slice", "array.slice(array, start, [end])");
                    add("sort", "array.sort(array)");
                    add("reverse", "array.reverse(array)");
                    add("concat", "array.concat(array, ...)");
                    add("push", "array.push(array, value, ...)");
                    add("pop", "array.pop(array)");
                    add("clone", "array.clone(array)");
                    add("contains", "array.contains(array, value)");
                    add("index_of", "array.index_of(array, value)");
                    add("sum", "array.sum(array)");
                    add("avg", "array.avg(array)");
                    add("unique", "array.unique(array)");
                    add("flatten", "array.flatten(array, [depth])");
                    add("range", "array.range(end) | array.range(start, end, [step])");
                    add("chunk", "array.chunk(array, size)");
                    add("zip", "array.zip(array, ...)");
                    add("repeat", "array.repeat(value, count)");
                } else if (module == "set") {
                    add("union", "set.union(array, ...)");
                    add("intersection", "set.intersection(array, ...)");
                    add("difference", "set.difference(array, ...)");
                    add("symmetric_difference", "set.symmetric_difference(left, right)");
                    add("is_subset", "set.is_subset(left, right)");
                    add("is_superset", "set.is_superset(left, right)");
                } else if (module == "math") {
                    add("sqrt", "math.sqrt(value)");
                    add("sin", "math.sin(value)");
                    add("cos", "math.cos(value)");
                    add("tan", "math.tan(value)");
                    add("pow", "math.pow(base, exponent)");
                    add("floor", "math.floor(value)");
                    add("ceil", "math.ceil(value)");
                    add("round", "math.round(value)");
                    add("abs", "math.abs(value)");
                    add("sign", "math.sign(value)");
                    add("min", "math.min(value, ...)");
                    add("max", "math.max(value, ...)");
                    add("random", "math.random([max]) | math.random(min, max)");
                    add("clamp", "math.clamp(value, min, max)");
                } else if (module == "string") {
                    add("len", "string.len(text)");
                    add("length", "string.length(text)");
                    add("size", "string.size(text)");
                    add("split", "string.split(text, separator)");
                    add("join", "string.join(array, separator)");
                    add("trim", "string.trim(text)");
                    add("upper", "string.upper(text)");
                    add("lower", "string.lower(text)");
                    add("contains", "string.contains(text, needle)");
                    add("starts_with", "string.starts_with(text, prefix)");
                    add("ends_with", "string.ends_with(text, suffix)");
                    add("index_of", "string.index_of(text, needle)");
                    add("sub", "string.sub(text, start, [end])");
                    add("replace", "string.replace(text, from, to)");
                    add("lines", "string.lines(text)");
                    add("words", "string.words(text)");
                    add("repeat", "string.repeat(text, count)");
                    add("pad_left", "string.pad_left(text, width, [fill])");
                    add("pad_right", "string.pad_right(text, width, [fill])");
                    add("chunks", "string.chunks(text, [max_chars], [overlap])");
                } else if (module == "json") {
                    add("parse", "json.parse(text)");
                    add("try_parse", "json.try_parse(text, [fallback])");
                    add("stringify", "json.stringify(value)");
                    add("pretty", "json.pretty(value, [indent])");
                    add("path", "json.path(value, path, [default])");
                    add("get_path", "json.get_path(value, path, [default])");
                    add("has_path", "json.has_path(value, path)");
                    add("merge_patch", "json.merge_patch(target, patch)");
                    add("delete_path", "json.delete_path(value, path)");
                    add("set_path", "json.set_path(value, path, new_value)");
                    add("schema_validate", "json.schema_validate(value, schema)");
                    add("schema_errors", "json.schema_errors(value, schema)");
                    add("schema_to_json_schema", "json.schema_to_json_schema(schema, [strict])");
                    add("to_json_schema", "json.to_json_schema(schema, [strict])");
                    add("template_render", "json.template_render(text, data, [missing])");
                    add("render", "json.render(text, data, [missing])");
                    add("pluck", "json.pluck(rows, path, [default])");
                    add("count_by", "json.count_by(rows, path)");
                    add("group_by", "json.group_by(rows, path)");
                    add("sort_by", "json.sort_by(rows, path, [descending])");
                    add("jsonl_parse", "json.jsonl_parse(text)");
                    add("jsonl_stringify", "json.jsonl_stringify(rows, [trailing_newline])");
                    add("sse_parse", "json.sse_parse(text)");
                    add("sse_data", "json.sse_data(text, [parse_json])");
                    add("csv_parse", "json.csv_parse(text, [has_header])");
                    add("csv_stringify", "json.csv_stringify(rows, [headers])");
                    add("ini_parse", "json.ini_parse(text)");
                    add("ini_stringify", "json.ini_stringify(dict)");
                } else if (module == "dict") {
                    add("keys", "dict.keys(dict)");
                    add("values", "dict.values(dict)");
                    add("items", "dict.items(dict)");
                    add("merge", "dict.merge(dict, ...)");
                    add("pick", "dict.pick(dict, keys)");
                    add("omit", "dict.omit(dict, keys)");
                    add("get_path", "dict.get_path(value, path, [default])");
                } else if (module == "regex") {
                    add("match", "regex.match(text, pattern)");
                    add("replace", "regex.replace(text, pattern, replacement)");
                    add("find_all", "regex.find_all(text, pattern)");
                    add("escape", "regex.escape(text)");
                    add("capture", "regex.capture(text, pattern)");
                    add("captures", "regex.captures(text, pattern)");
                    add("split", "regex.split(text, pattern)");
                } else if (module == "random") {
                    add("seed", "random.seed(seed)");
                    add("int", "random.int(max) | random.int(min, max)");
                    add("integer", "random.integer(max) | random.integer(min, max)");
                    add("float", "random.float([max]) | random.float(min, max)");
                    add("number", "random.number([max]) | random.number(min, max)");
                    add("bool", "random.bool([probability])");
                    add("choice", "random.choice(array)");
                    add("shuffle", "random.shuffle(array)");
                    add("bytes", "random.bytes(count)");
                    add("uuid", "random.uuid()");
                } else if (module == "datetime") {
                    add("now", "datetime.now()");
                    add("parse", "datetime.parse(text, [format])");
                    add("format", "datetime.format(timestamp, format)");
                    add("utc_format", "datetime.utc_format(timestamp, format)");
                    add("parts", "datetime.parts(timestamp, [utc])");
                    add("add", "datetime.add(timestamp, seconds)");
                    add("diff", "datetime.diff(end_timestamp, start_timestamp)");
                    add("timestamp", "datetime.timestamp()");
                } else if (module == "crypto") {
                    add("sha256", "crypto.sha256(text)");
                    add("file_sha256", "crypto.file_sha256(path)");
                    add("hmac_sha256", "crypto.hmac_sha256(key, message)");
                    add("file_hmac_sha256", "crypto.file_hmac_sha256(key, path)");
                    add("random_bytes", "crypto.random_bytes(count)");
                    add("random_hex", "crypto.random_hex(count)");
                    add("constant_time_eq", "crypto.constant_time_eq(left, right)");
                    add("hex_encode", "crypto.hex_encode(text)");
                    add("hex_decode", "crypto.hex_decode(text)");
                    add("base64_encode", "crypto.base64_encode(text)");
                    add("base64_decode", "crypto.base64_decode(text)");
                    add("base64_url_encode", "crypto.base64_url_encode(text)");
                    add("base64_url_decode", "crypto.base64_url_decode(text)");
                    add("url_encode", "crypto.url_encode(text)");
                    add("url_decode", "crypto.url_decode(text)");
                    add("url_parse", "crypto.url_parse(url)");
                    add("url_build", "crypto.url_build(parts)");
                    add("query_build", "crypto.query_build(params)");
                    add("query_parse", "crypto.query_parse(query)");
                    add("form_build", "crypto.form_build(params)");
                    add("form_parse", "crypto.form_parse(body)");
                    add("auth_bearer", "crypto.auth_bearer(token)");
                    add("auth_basic", "crypto.auth_basic(username, password)");
                    add("headers_merge", "crypto.headers_merge(headers, ...)");
                    add("headers_get", "crypto.headers_get(headers, name, [default])");
                    add("headers_has", "crypto.headers_has(headers, name)");
                    add("headers_redact", "crypto.headers_redact(headers, [names], [mask])");
                    add("cookie_parse", "crypto.cookie_parse(header_or_headers)");
                    add("cookie_build", "crypto.cookie_build(cookies)");
                    add("cookie_get", "crypto.cookie_get(header_or_cookies, name, [default])");
                    add("content_type", "crypto.content_type(headers_or_value, [default])");
                    add("charset", "crypto.charset(headers_or_value, [default])");
                    add("is_json", "crypto.is_json(headers_or_value)");
                    add("status_ok", "crypto.status_ok(status)");
                    add("status_text", "crypto.status_text(status)");
                    add("status_retryable", "crypto.status_retryable(status)");
                    add("retry_after", "crypto.retry_after(headers_or_value, [default_ms])");
                    add("backoff_delays", "crypto.backoff_delays(attempts, [base_ms], [factor], [max_ms])");
                } else if (module == "db") {
                    add("set", "db.set(path, key, value)");
                    add("get", "db.get(path, key, [default])");
                    add("has", "db.has(path, key)");
                    add("delete", "db.delete(path, key)");
                    add("keys", "db.keys(path)");
                    add("all", "db.all(path)");
                    add("insert", "db.insert(path, row)");
                    add("find", "db.find(path, criteria)");
                    add("count", "db.count(path, [criteria])");
                    add("update", "db.update(path, criteria, patch)");
                    add("remove", "db.remove(path, criteria)");
                    add("query", "db.query(path, [criteria], [options])");
                } else if (module == "os") {
                    add("env_get", "os.env_get(name, [default])");
                    add("env_require", "os.env_require(name)");
                    add("env_set", "os.env_set(name, value)");
                    add("env_load", "os.env_load(path, [override])");
                    add("argv", "os.argv()");
                    add("argc", "os.argc()");
                    add("script_name", "os.script_name()");
                    add("cwd", "os.cwd()");
                    add("home_dir", "os.home_dir()");
                    add("temp_dir", "os.temp_dir()");
                    add("path_separator", "os.path_separator()");
                    add("name", "os.name()");
                    add("is_windows", "os.is_windows()");
                    add("which", "os.which(command)");
                    add("cmd_exists", "os.cmd_exists(command)");
                    add("cmd_quote", "os.cmd_quote(text)");
                    add("cmd_join", "os.cmd_join(args)");
                    add("run", "os.run(command)");
                    add("run_checked", "os.run_checked(command)");
                    add("cmd", "os.cmd(command)");
                    add("sleep_ms", "os.sleep_ms(milliseconds)");
                    add("wait", "os.wait(milliseconds)");
                } else if (module == "path") {
                    add("join", "path.join(part, ...)");
                    add("basename", "path.basename(path)");
                    add("dirname", "path.dirname(path)");
                    add("ext", "path.ext(path)");
                    add("stem", "path.stem(path)");
                    add("normalize", "path.normalize(path)");
                    add("abs", "path.abs(path)");
                    add("relative", "path.relative(path, [base])");
                } else if (module == "python") {
                    add("available", "python.available()");
                    add("executable", "python.executable()");
                    add("eval", "python.eval(code)");
                    add("call", "python.call(module, function, [args], [kwargs])");
                    add("call_json", "python.call_json(module, function, [args], [kwargs])");
                } else if (module == "ffi") {
                    add("load", "ffi.load(path)");
                    add("call", "ffi.call(lib, symbol, signature, ...args)");
                } else if (module == "plugin") {
                    add("load", "plugin.load(path)");
                    add("load_manifest", "plugin.load_manifest(path)");
                    add("call", "plugin.call(plugin, export, ...args)");
                    add("info", "plugin.info(plugin)");
                    add("unload", "plugin.unload(plugin)");
                } else if (module == "cli") {
                    add("parse", "cli.parse(text, [value_flags])");
                    add("argv", "cli.argv()");
                    add("argc", "cli.argc()");
                    add("script_name", "cli.script_name()");
                } else if (module == "log") {
                    add("set_file", "log.set_file(path, [append])");
                    add("set_json", "log.set_json(enabled)");
                    add("set_level", "log.set_level(level)");
                    add("get_level", "log.get_level()");
                    add("level", "log.level([level])");
                    add("event", "log.event(level, message, [fields])");
                    add("debug", "log.debug(message)");
                    add("info", "log.info(message)");
                    add("warn", "log.warn(message)");
                    add("error", "log.error(message)");
                } else if (module == "console") {
                    add("log", "console.log(value, ...)");
                    add("print", "console.print(value, ...)");
                    add("write", "console.write(value, ...)");
                    add("write_line", "console.write_line(value, ...)");
                    add("writeln", "console.writeln(value, ...)");
                    add("println", "console.println(value, ...)");
                    add("line", "console.line([value, ...])");
                    add("info", "console.info(value, ...)");
                    add("debug", "console.debug(value, ...)");
                    add("warn", "console.warn(value, ...)");
                    add("warning", "console.warning(value, ...)");
                    add("error", "console.error(value, ...)");
                    add("exception", "console.exception(value, ...)");
                    add("clear", "console.clear()");
                    add("assert", "console.assert(condition, [message...])");
                    add("time", "console.time([label])");
                    add("time_log", "console.time_log([label], [message...])");
                    add("timeLog", "console.timeLog([label], [message...])");
                    add("time_end", "console.time_end([label])");
                    add("timeEnd", "console.timeEnd([label])");
                    add("time_stamp", "console.time_stamp([label])");
                    add("timeStamp", "console.timeStamp([label])");
                    add("count", "console.count([label])");
                    add("count_reset", "console.count_reset([label])");
                    add("countReset", "console.countReset([label])");
                    add("table", "console.table(value)");
                    add("dir", "console.dir(value)");
                    add("dirxml", "console.dirxml(value, ...)");
                    add("trace", "console.trace([message...])");
                    add("group", "console.group([label...])");
                    add("group_collapsed", "console.group_collapsed([label...])");
                    add("groupCollapsed", "console.groupCollapsed([label...])");
                    add("group_end", "console.group_end()");
                    add("groupEnd", "console.groupEnd()");
                    add("profile", "console.profile([label])");
                    add("profile_end", "console.profile_end([label])");
                    add("profileEnd", "console.profileEnd([label])");
                    add("style", "console.style(text, style_or_styles)");
                    add("color", "console.color(text, fg, [bg])");
                    add("colour", "console.colour(text, fg, [bg])");
                    add("strip_ansi", "console.strip_ansi(text)");
                    add("stripAnsi", "console.stripAnsi(text)");
                    add("set_color", "console.set_color(fg, [bg])");
                    add("setColor", "console.setColor(fg, [bg])");
                    add("set_colour", "console.set_colour(fg, [bg])");
                    add("setColour", "console.setColour(fg, [bg])");
                    add("reset_color", "console.reset_color()");
                    add("resetColor", "console.resetColor()");
                    add("reset_colour", "console.reset_colour()");
                    add("resetColour", "console.resetColour()");
                    add("is_tty", "console.is_tty()");
                    add("isTTY", "console.isTTY()");
                    add("width", "console.width()");
                    add("height", "console.height()");
                    add("size", "console.size()");
                    add("input", "console.input([prompt])");
                    add("read_line", "console.read_line([prompt])");
                    add("readline", "console.readline([prompt])");
                    add("readLine", "console.readLine([prompt])");
                    add("prompt", "console.prompt([prompt])");
                } else if (module == "fs") {
                    add("read", "fs.read(path)");
                    add("write", "fs.write(path, text)");
                    add("read_json", "fs.read_json(path)");
                    add("write_json", "fs.write_json(path, value)");
                    add("read_bytes", "fs.read_bytes(path)");
                    add("write_bytes", "fs.write_bytes(path, bytes)");
                    add("sha256", "fs.sha256(path)");
                    add("append", "fs.append(path, text)");
                    add("exists", "fs.exists(path)");
                    add("delete", "fs.delete(path)");
                    add("remove_tree", "fs.remove_tree(path)");
                    add("list", "fs.list(path)");
                    add("walk", "fs.walk(path, [extension])");
                    add("glob", "fs.glob(pattern)");
                    add("mkdir", "fs.mkdir(path)");
                    add("cwd", "fs.cwd()");
                    add("join", "fs.join(part, ...)");
                    add("basename", "fs.basename(path)");
                    add("dirname", "fs.dirname(path)");
                    add("ext", "fs.ext(path)");
                    add("stem", "fs.stem(path)");
                    add("normalize", "fs.normalize(path)");
                    add("abs", "fs.abs(path)");
                    add("relative", "fs.relative(path, [base])");
                    add("is_dir", "fs.is_dir(path)");
                    add("is_file", "fs.is_file(path)");
                    add("info", "fs.info(path)");
                    add("size", "fs.size(path)");
                    add("copy", "fs.copy(src, dst, [overwrite])");
                    add("move", "fs.move(src, dst, [overwrite])");
                    add("lines", "fs.lines(path)");
                } else if (module == "http") {
                    add("get", "http.get(url)");
                    add("json", "http.json(url)");
                    add("post", "http.post(url, body, [content_type])");
                    add("request", "http.request(spec)");
                    add("request_full", "http.request_full(spec)");
                    add("request_retry", "http.request_retry(spec, [attempts], [delay_ms])");
                    add("request_json", "http.request_json(spec)");
                    add("request_json_checked", "http.request_json_checked(spec)");
                    add("request_retry_json", "http.request_retry_json(spec, [attempts], [delay_ms])");
                    add("request_retry_json_checked", "http.request_retry_json_checked(spec, [attempts], [delay_ms])");
                    add("serve_static", "http.serve_static(path, [port])");
                    add("serve_routes", "http.serve_routes(routes, [port])");
                    add("server_url", "http.server_url(server)");
                    add("server_stop", "http.server_stop(server)");
                    add("url_parse", "http.url_parse(url)");
                    add("url_build", "http.url_build(parts)");
                    add("query_build", "http.query_build(params)");
                    add("query_parse", "http.query_parse(query)");
                    add("form_build", "http.form_build(params)");
                    add("form_parse", "http.form_parse(body)");
                    add("auth_bearer", "http.auth_bearer(token)");
                    add("auth_basic", "http.auth_basic(username, password)");
                    add("headers_merge", "http.headers_merge(headers, ...)");
                    add("headers_get", "http.headers_get(headers, name, [default])");
                    add("headers_has", "http.headers_has(headers, name)");
                    add("headers_redact", "http.headers_redact(headers, [names], [mask])");
                    add("cookie_parse", "http.cookie_parse(header_or_headers)");
                    add("cookie_build", "http.cookie_build(cookies)");
                    add("cookie_get", "http.cookie_get(header_or_cookies, name, [default])");
                    add("content_type", "http.content_type(headers_or_value, [default])");
                    add("charset", "http.charset(headers_or_value, [default])");
                    add("is_json", "http.is_json(headers_or_value)");
                    add("status_ok", "http.status_ok(status)");
                    add("status_text", "http.status_text(status)");
                    add("status_retryable", "http.status_retryable(status)");
                    add("retry_after", "http.retry_after(headers_or_value, [default_ms])");
                    add("backoff_delays", "http.backoff_delays(attempts, [base_ms], [factor], [max_ms])");
                } else if (module == "async") {
                    add("cmd", "async.cmd(command, [scope_id])");
                    add("ready", "async.ready(task_id)");
                    add("http_get", "async.http_get(url, [scope_id])");
                    add("http_request", "async.http_request(spec, [scope_id])");
                    add("sleep", "async.sleep(milliseconds, [scope_id])");
                    add("sura", "async.sura(spec, [scope_id])");
                    add("status", "async.status(task_id)");
                    add("pending", "async.pending()");
                    add("forget", "async.forget(task_id)");
                    add("cleanup", "async.cleanup()");
                    add("cancel", "async.cancel(task_id)");
                    add("cancelled", "async.cancelled(task_id)");
                    add("configure", "async.configure(max_workers, max_queue)");
                    add("limits", "async.limits()");
                    add("scope", "async.scope()");
                    add("scope_open", "async.scope_open()");
                    add("scope_attach", "async.scope_attach(scope_id, task_id)");
                    add("scope_cancel", "async.scope_cancel(scope_id)");
                    add("scope_status", "async.scope_status(scope_id)");
                    add("scope_close", "async.scope_close(scope_id, [milliseconds])");
                    add("scope_join", "async.scope_join(scope_id, [milliseconds])");
                    add("await", "async.await(task_id)");
                    add("await_timeout", "async.await_timeout(task_id, milliseconds, [default])");
                    add("ready_all", "async.ready_all(task_ids)");
                    add("any", "async.any(task_ids, [milliseconds], [default])");
                    add("all", "async.all(task_ids)");
                    add("all_timeout", "async.all_timeout(task_ids, milliseconds, [default])");
                } else if (module == "test") {
                    add("assert", "test.assert(condition, [message])");
                    add("eq", "test.eq(actual, expected, [message])");
                    add("ne", "test.ne(actual, expected, [message])");
                    add("neq", "test.neq(actual, expected, [message])");
                    add("contains", "test.contains(container, value, [message])");
                    add("not_contains", "test.not_contains(container, value, [message])");
                    add("match", "test.match(text, pattern, [message])");
                    add("type", "test.type(value, type, [message])");
                    add("len", "test.len(value, length, [message])");
                    add("between", "test.between(value, min, max, [message])");
                    add("approx", "test.approx(actual, expected, [epsilon], [message])");
                    add("check", "test.check(name, condition, [message])");
                    add("check_eq", "test.check_eq(name, actual, expected, [message])");
                    add("check_match", "test.check_match(name, text, pattern, [message])");
                    add("summary", "test.summary(results)");
                    add("report", "test.report(results, [title])");
                } else if (module == "vector") {
                    add("vec3", "vector.vec3(x, y, z)");
                    add("add", "vector.add(a, b)");
                    add("dot", "vector.dot(a, b)");
                    add("scale", "vector.scale(vector, scalar)");
                    add("norm", "vector.norm(vector)");
                    add("add3", "vector.add3(a, b)");
                    add("sub3", "vector.sub3(a, b)");
                    add("dot3", "vector.dot3(a, b)");
                    add("cross", "vector.cross(a, b)");
                    add("scale3", "vector.scale3(vector, scalar)");
                    add("norm3", "vector.norm3(vector)");
                    add("normalize3", "vector.normalize3(vector)");
                    add("distance3", "vector.distance3(a, b)");
                    add("cosine", "vector.cosine(a, b)");
                    add("normalize", "vector.normalize(vector)");
                    add("search", "vector.search(query, rows, [k], [field])");
                } else if (module == "graphics3d" || module == "g3d" || module == "graphics") {
                    add("identity", "graphics3d.identity()");
                    add("translate", "graphics3d.translate(x, y, z)");
                    add("scale", "graphics3d.scale(x, y, z)");
                    add("rotate_y", "graphics3d.rotate_y(radians)");
                    add("mul", "graphics3d.mul(left, right)");
                    add("cube", "graphics3d.cube([size], [center])");
                    add("transform", "graphics3d.transform(mesh, matrix4)");
                    add("bounds", "graphics3d.bounds(mesh)");
                    add("face_normals", "graphics3d.face_normals(mesh)");
                    add("project", "graphics3d.project(point, camera, [width], [height])");
                } else if (module == "rag") {
                    add("context", "rag.context(query, docs, [k], [embedding_field], [text_field])");
                    add("sources", "rag.sources(query, docs, [k], [embedding_field], [text_field], [title_field])");
                    add("prepare", "rag.prepare(question, query, docs, [k], [embedding_field], [text_field], [system], [title_field])");
                    add("messages", "rag.messages(question, context, [system])");
                } else if (module == "tensor") {
                    add("shape", "tensor.shape(tensor)");
                    add("zeros", "tensor.zeros(shape)");
                    add("fill", "tensor.fill(shape, value)");
                    add("add", "tensor.add(a, b)");
                    add("mul", "tensor.mul(a, b)");
                    add("clip", "tensor.clip(tensor, min, max)");
                    add("flatten", "tensor.flatten(tensor)");
                    add("sum", "tensor.sum(tensor)");
                    add("mean", "tensor.mean(tensor)");
                    add("variance", "tensor.variance(tensor)");
                    add("std", "tensor.std(tensor)");
                    add("min", "tensor.min(tensor)");
                    add("max", "tensor.max(tensor)");
                    add("argmin", "tensor.argmin(tensor)");
                    add("argmax", "tensor.argmax(tensor)");
                    add("zscore", "tensor.zscore(tensor)");
                    add("softmax", "tensor.softmax(tensor)");
                    add("transpose", "tensor.transpose(matrix)");
                    add("matmul", "tensor.matmul(a, b)");
                } else if (module == "nn") {
                    add("mlp", "nn.mlp(layer_sizes, [options])");
                    add("forward", "nn.forward(model, inputs)");
                    add("predict", "nn.predict(model, inputs)");
                    add("train", "nn.train(model, inputs, targets, [options])");
                    add("classify", "nn.classify(model, inputs, [threshold])");
                    add("evaluate", "nn.evaluate(model, inputs, targets, [options])");
                    add("summary", "nn.summary(model)");
                    add("one_hot", "nn.one_hot(labels, class_count)");
                    add("fit_standardizer", "nn.fit_standardizer(inputs)");
                    add("standardize", "nn.standardize(inputs, standardizer)");
                    add("split", "nn.split(inputs, targets, [options])");
                    add("save", "nn.save(model, path)");
                    add("load", "nn.load(path)");
                } else if (module == "autograd") {
                    add("tensor", "autograd.tensor(data, [options])");
                    add("parameter", "autograd.parameter(data, [dtype_or_options])");
                    add("zeros", "autograd.zeros(shape, [options])");
                    add("ones", "autograd.ones(shape, [options])");
                    add("randn", "autograd.randn(shape, [options])");
                     add("data", "autograd.data(tensor)");
                     add("grad", "autograd.grad(tensor)");
                     add("grad_info", "autograd.grad_info(tensor)");
                    add("dtype", "autograd.dtype(tensor)");
                    add("device", "autograd.device(tensor)");
                    add("to", "autograd.to(tensor, device)");
                    add("storage_bytes", "autograd.storage_bytes(tensor)");
                    add("cast", "autograd.cast(tensor, dtype)");
                    add("shape", "autograd.shape(tensor)");
                    add("numel", "autograd.numel(tensor)");
                     add("limits", "autograd.limits()");
                     add("autocast", "autograd.autocast([state])");
                    add("item", "autograd.item(tensor)");
                    add("detach", "autograd.detach(tensor)");
                    add("set_requires_grad", "autograd.set_requires_grad(tensor, requires_grad)");
                    add("requires_grad", "autograd.requires_grad(tensor)");
                    add("add", "autograd.add(a, b)");
                    add("sub", "autograd.sub(a, b)");
                    add("mul", "autograd.mul(a, b)");
                    add("div", "autograd.div(a, b)");
                    add("neg", "autograd.neg(tensor)");
                    add("reshape", "autograd.reshape(tensor, shape)");
                    add("matmul", "autograd.matmul(a, b, [options])");
                    add("transpose", "autograd.transpose(tensor, [axis1], [axis2])");
                    add("linear", "autograd.linear(input, weights, [bias])");
                    add("relu", "autograd.relu(tensor)");
                    add("tanh", "autograd.tanh(tensor)");
                    add("sigmoid", "autograd.sigmoid(tensor)");
                    add("gelu", "autograd.gelu(tensor)");
                    add("layer_norm", "autograd.layer_norm(tensor, [weight], [bias], [epsilon])");
                    add("embedding", "autograd.embedding(token_ids, weight)");
                    add("causal_attention", "autograd.causal_attention(query, key, value, [options])");
                    add("softmax", "autograd.softmax(tensor)");
                    add("sum", "autograd.sum(tensor)");
                    add("mean", "autograd.mean(tensor)");
                    add("mse", "autograd.mse(prediction, target)");
                    add("bce", "autograd.bce(probabilities, target)");
                    add("bce_logits", "autograd.bce_logits(logits, target)");
                    add("cross_entropy", "autograd.cross_entropy(logits, one_hot_targets)");
                    add("cross_entropy_ids", "autograd.cross_entropy_ids(logits, class_ids)");
                     add("backward", "autograd.backward(tensor, [gradient], [retain_graph])");
                     add("backward_scaled", "autograd.backward_scaled(loss, scale, [retain_graph])");
                     add("zero_grad", "autograd.zero_grad(parameters)");
                     add("unscale_gradients", "autograd.unscale_gradients(parameters, [scale])");
                    add("sgd", "autograd.sgd(parameters, learning_rate, [options])");
                    add("adam", "autograd.adam(parameters, learning_rate, [options])");
                    add("reset_optimizer", "autograd.reset_optimizer(parameters)");
                    add("grad_norm", "autograd.grad_norm(parameters)");
                    add("clip_grad_norm", "autograd.clip_grad_norm(parameters, max_norm)");
                    add("save_checkpoint", "autograd.save_checkpoint(state_dict, path, [options])");
                    add("load_checkpoint", "autograd.load_checkpoint(path, [options])");
                    add("cuda_available", "autograd.cuda_available()");
                    add("cuda_info", "autograd.cuda_info()");
                    add("cuda_stats", "autograd.cuda_stats()");
                    add("cuda_reset_stats", "autograd.cuda_reset_stats()");
                    add("cuda_synchronize", "autograd.cuda_synchronize()");
                    add("save_safetensors", "autograd.save_safetensors(state_dict, path)");
                    add("load_safetensors", "autograd.load_safetensors(path, [options])");
                    add("save_onnx_weights", "autograd.save_onnx_weights(state_dict, path)");
                    add("load_onnx_weights", "autograd.load_onnx_weights(path, [options])");
                    add("run_onnx", "autograd.run_onnx(path, inputs, [options])");
                    add("all_reduce_gradients", "autograd.all_reduce_gradients(parameters, options)");
                } else if (module == "tokenizer") {
                    add("byte", "tokenizer.byte([options])");
                    add("train_bpe", "tokenizer.train_bpe(corpus, [options])");
                    add("encode", "tokenizer.encode(tokenizer, text, [options])");
                    add("decode", "tokenizer.decode(tokenizer, ids, [options])");
                    add("info", "tokenizer.info(tokenizer)");
                    add("save", "tokenizer.save(tokenizer, path)");
                    add("load", "tokenizer.load(path)");
                } else if (module == "dataset") {
                    add("pack_text", "dataset.pack_text(source, tokenizer, path, [options])");
                    add("open", "dataset.open(path, [options])");
                    add("next", "dataset.next(loader)");
                    add("reset", "dataset.reset(loader, [epoch])");
                    add("close", "dataset.close(loader)");
                    add("info", "dataset.info(loader)");
                } else if (module == "media") {
                    add("available", "media.available([ffmpeg_path])");
                    add("ffmpeg_available", "media.ffmpeg_available([ffmpeg_path])");
                    add("frame_to_text", "media.frame_to_text(pixels, [options])");
                    add("ascii_frames", "media.ascii_frames(path, [options])");
                    add("video_to_text", "media.video_to_text(path, [options])");
                    add("video_text_frames", "media.video_text_frames(path, [options])");
                } else if (module == "stream") {
                    add("from", "stream.from(array_or_text)");
                    add("next", "stream.next(stream)");
                    add("collect", "stream.collect(stream)");
                    add("take", "stream.take(stream, count)");
                    add("batch", "stream.batch(stream, size)");
                    add("map", "stream.map(stream, path, [fallback])");
                    add("filter", "stream.filter(stream, criteria)");
                    add("window", "stream.window(stream, size, [step])");
                    add("skip", "stream.skip(stream, count)");
                    add("count", "stream.count(stream)");
                    add("join", "stream.join(stream, [separator])");
                    add("sum", "stream.sum(stream, [path])");
                    add("avg", "stream.avg(stream, [path])");
                    add("lines", "stream.lines(path)");
                } else if (module == "tool") {
                    add("call", "tool.call(spec)");
                    add("spec", "tool.spec(name, args)");
                    add("validate", "tool.validate(spec)");
                    add("schema", "tool.schema(name)");
                    add("allowed", "tool.allowed(spec, policy)");
                    add("call_policy", "tool.call_policy(spec, policy)");
                    add("list", "tool.list()");
                } else if (module == "llm") {
                    add("message", "llm.message(role, content)");
                    add("messages", "llm.messages([system], user)");
                    add("rag_messages", "llm.rag_messages(question, context, [system])");
                    add("request", "llm.request(model, messages, [temperature])");
                    add("request_json", "llm.request_json(model, messages, [temperature])");
                    add("response_schema", "llm.response_schema(name, schema, [strict])");
                    add("request_schema", "llm.request_schema(model, messages, schema, [temperature], [name], [strict])");
                    add("request_schema_json", "llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])");
                    add("tools", "llm.tools([names])");
                    add("tool_schemas", "llm.tool_schemas([names])");
                    add("request_tools", "llm.request_tools(model, messages, tool_names, [temperature])");
                    add("request_tools_json", "llm.request_tools_json(model, messages, tool_names, [temperature])");
                    add("request_tools_schema", "llm.request_tools_schema(model, messages, tool_names, schema, [temperature], [name], [strict])");
                    add("request_tools_schema_json", "llm.request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])");
                    add("extract_text", "llm.extract_text(response)");
                    add("extract_json", "llm.extract_json(response, [schema])");
                    add("usage", "llm.usage(response)");
                    add("cost", "llm.cost(response, pricing)");
                    add("budget", "llm.budget(response, pricing, limit)");
                    add("tool_calls", "llm.tool_calls(response)");
                    add("tool_result", "llm.tool_result(call_or_id, result)");
                    add("run_tools", "llm.run_tools(response, policy)");
                    add("next_messages", "llm.next_messages(messages, response, policy)");
                    add("next_request", "llm.next_request(model, messages, response, policy, tool_names, [temperature])");
                    add("next_request_json", "llm.next_request_json(model, messages, response, policy, tool_names, [temperature])");
                    add("next_schema_request", "llm.next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])");
                    add("next_schema_request_json", "llm.next_schema_request_json(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])");
                    add("stream_text", "llm.stream_text(sse_or_chunks)");
                    add("chat", "llm.chat(endpoint, api_key, model, messages, [temperature])");
                    add("chat_request", "llm.chat_request(endpoint, api_key, request)");
                }
                return methods;
            };

            auto completion_items_for_module = [&](const std::string& module) {
                auto methods = lsp_module_methods(module);
                std::ostringstream out;
                out << "[";
                for (size_t idx = 0; idx < methods.size(); ++idx) {
                    if (idx) out << ",";
                    out << "{\"label\":\"" << json_escape(methods[idx].first)
                        << "\",\"detail\":\"" << json_escape(methods[idx].second)
                        << "\",\"kind\":2}";
                }
                out << "]";
                return out.str();
            };

            auto module_signature_for = [&](const std::string& raw_module,
                                            const std::string& method,
                                            LspSignatureInfo& info) {
                auto methods = lsp_module_methods(raw_module);
                for (const auto& item : methods) {
                    if (item.first == method) {
                        std::string module = canonical_lsp_module(raw_module);
                        info.label = item.second;
                        LspSignatureInfo builtin_info;
                        std::string builtin_prefix = module == "ai" ? "nn" : module;
                        if (builtin_signature_for(builtin_prefix + "_" + method, builtin_info)) {
                            info.documentation = builtin_info.documentation;
                        } else {
                            info.documentation = "Sura " + module + " standard library module member.";
                        }
                        info.parameters = split_signature_parameters(info.label);
                        return true;
                    }
                }
                return false;
            };

            auto module_signature_for_symbol = [&](const std::string& symbol, LspSignatureInfo& info) {
                size_t dot = symbol.find('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 >= symbol.size()) return false;
                return module_signature_for(symbol.substr(0, dot), symbol.substr(dot + 1), info);
            };

            auto send_lsp = [](const std::string& body) {
                std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
                std::cout.flush();
            };

            auto extract_json_field = [](const std::string& body, const std::string& field) {
                std::regex re("\"" + field + "\"\\s*:\\s*(\"(?:\\\\.|[^\"])*\"|-?[0-9]+|null)");
                std::smatch m;
                if (std::regex_search(body, m, re)) return m[1].str();
                return std::string("null");
            };

            auto extract_json_string = [&](const std::string& body, const std::string& field) {
                std::regex re("\"" + field + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
                std::smatch m;
                if (std::regex_search(body, m, re)) return json_unescape(m[1].str());
                return std::string();
            };

            auto extract_json_int = [](const std::string& body, const std::string& field, int fallback) {
                std::regex re("\"" + field + "\"\\s*:\\s*(-?[0-9]+)");
                std::smatch m;
                if (std::regex_search(body, m, re)) return std::stoi(m[1].str());
                return fallback;
            };

            auto extract_json_ints = [](const std::string& body, const std::string& field) {
                std::vector<int> out;
                std::regex re("\"" + field + "\"\\s*:\\s*(-?[0-9]+)");
                for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
                     it != std::sregex_iterator(); ++it) {
                    out.push_back(std::stoi((*it)[1].str()));
                }
                return out;
            };

            auto extract_json_array_objects = [](const std::string& body, const std::string& field) {
                std::vector<std::string> objects;
                std::string marker = "\"" + field + "\"";
                size_t key = body.find(marker);
                if (key == std::string::npos) return objects;
                size_t start = body.find('[', key + marker.size());
                if (start == std::string::npos) return objects;
                bool in_string = false;
                bool escape = false;
                int array_depth = 0;
                int object_depth = 0;
                size_t object_start = std::string::npos;
                for (size_t i = start; i < body.size(); ++i) {
                    char ch = body[i];
                    if (in_string) {
                        if (escape) escape = false;
                        else if (ch == '\\') escape = true;
                        else if (ch == '"') in_string = false;
                        continue;
                    }
                    if (ch == '"') {
                        in_string = true;
                        continue;
                    }
                    if (ch == '[') {
                        ++array_depth;
                        continue;
                    }
                    if (ch == ']') {
                        --array_depth;
                        if (array_depth <= 0) break;
                        continue;
                    }
                    if (array_depth != 1) continue;
                    if (ch == '{') {
                        if (object_depth == 0) object_start = i;
                        ++object_depth;
                        continue;
                    }
                    if (ch == '}') {
                        --object_depth;
                        if (object_depth == 0 && object_start != std::string::npos) {
                            objects.push_back(body.substr(object_start, i - object_start + 1));
                            object_start = std::string::npos;
                        }
                    }
                }
                return objects;
            };

            auto extract_method = [](const std::string& body) {
                std::regex re("\"method\"\\s*:\\s*\"([^\"]+)\"");
                std::smatch m;
                if (std::regex_search(body, m, re)) return m[1].str();
                return std::string();
            };

            std::unordered_map<std::string, std::string> documents;
            std::unordered_map<std::string, std::string> indexed_documents;
            std::string workspace_root_path;

            auto lower_ascii = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                    return (char)std::tolower(c);
                });
                return s;
            };

            auto trim_ascii = [](const std::string& s) {
                size_t start = 0;
                while (start < s.size() && std::isspace((unsigned char)s[start])) ++start;
                size_t end = s.size();
                while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
                return s.substr(start, end - start);
            };

            auto from_hex = [](char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };

            auto uri_decode = [&](const std::string& text) {
                std::string out;
                for (size_t i = 0; i < text.size(); ++i) {
                    if (text[i] == '%' && i + 2 < text.size()) {
                        int hi = from_hex(text[i + 1]);
                        int lo = from_hex(text[i + 2]);
                        if (hi >= 0 && lo >= 0) {
                            out.push_back((char)((hi << 4) | lo));
                            i += 2;
                            continue;
                        }
                    }
                    out.push_back(text[i]);
                }
                return out;
            };

            auto uri_encode_path = [](const std::string& text) {
                const char* hex = "0123456789ABCDEF";
                std::string out;
                for (unsigned char c : text) {
                    if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') {
                        out.push_back((char)c);
                    } else {
                        out.push_back('%');
                        out.push_back(hex[c >> 4]);
                        out.push_back(hex[c & 15]);
                    }
                }
                return out;
            };

            auto file_uri_to_path = [&](const std::string& uri) {
                if (uri.rfind("file://", 0) != 0) return std::string();
                std::string path = uri_decode(uri.substr(7));
                if (path.size() >= 3 && path[0] == '/' && std::isalpha((unsigned char)path[1]) && path[2] == ':')
                    path.erase(path.begin());
#ifdef _WIN32
                std::replace(path.begin(), path.end(), '/', '\\');
#endif
                return path;
            };

            auto path_to_file_uri = [&](const std::filesystem::path& path) {
                std::string generic = std::filesystem::absolute(path).generic_string();
                if (!generic.empty() && generic[0] == '/') return std::string("file://") + uri_encode_path(generic);
                return std::string("file:///") + uri_encode_path(generic);
            };

            auto read_index_file = [](const std::filesystem::path& path) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(path, ec)) return std::string();
                auto size = std::filesystem::file_size(path, ec);
                if (ec || size > 1024 * 1024) return std::string();
                std::ifstream f(path, std::ios::binary);
                if (!f) return std::string();
                return std::string((std::istreambuf_iterator<char>(f)), {});
            };

            auto index_workspace = [&]() {
                indexed_documents.clear();
                if (workspace_root_path.empty()) return;
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path root = utf8_path(workspace_root_path);
                if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;
                size_t count = 0;
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                while (!ec && it != end && count < 300) {
                    const fs::path p = it->path();
                    std::string name = p.filename().string();
                    if (it->is_directory(ec)) {
                        if (name == ".git" || name == "node_modules" || name == "sura_packages" ||
                            name == ".sura" || name == "dist" || name == "build") {
                            it.disable_recursion_pending();
                        }
                    } else if (p.extension() == ".sura") {
                        std::string text = read_index_file(p);
                        if (!text.empty()) {
                            indexed_documents[path_to_file_uri(p)] = std::move(text);
                            ++count;
                        }
                    }
                    it.increment(ec);
                }
            };

            auto merged_documents = [&]() {
                std::unordered_map<std::string, std::string> merged = indexed_documents;
                for (const auto& item : documents) merged[item.first] = item.second;
                return merged;
            };

            auto sorted_documents = [&]() {
                auto merged = merged_documents();
                std::vector<std::pair<std::string, std::string>> out(merged.begin(), merged.end());
                std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
                return out;
            };

            auto ordered_documents = [&](const std::string& preferred_uri) {
                std::vector<std::pair<std::string, std::string>> out;
                auto merged = merged_documents();
                if (!preferred_uri.empty() && merged.count(preferred_uri))
                    out.push_back({preferred_uri, merged[preferred_uri]});
                auto sorted = sorted_documents();
                for (const auto& item : sorted) {
                    if (item.first != preferred_uri) out.push_back(item);
                }
                return out;
            };

            auto document_text = [&](const std::string& uri) {
                if (documents.count(uri)) return documents[uri];
                if (indexed_documents.count(uri)) return indexed_documents[uri];
                return std::string();
            };

            auto byte_offset_for_lsp_position = [](const std::string& text, int line_no, int character) {
                line_no = std::max(0, line_no);
                character = std::max(0, character);
                size_t offset = 0;
                int line = 0;
                while (line < line_no && offset < text.size()) {
                    if (text[offset++] == '\n') ++line;
                }
                size_t line_start = offset;
                size_t line_end = text.find('\n', line_start);
                if (line_end == std::string::npos) line_end = text.size();
                if (line_end > line_start && text[line_end - 1] == '\r') --line_end;
                size_t max_chars = line_end - line_start;
                return line_start + std::min((size_t)character, max_chars);
            };

            auto apply_lsp_content_changes = [&](const std::string& uri, const std::string& body) {
                std::string current = document_text(uri);
                auto changes = extract_json_array_objects(body, "contentChanges");
                if (changes.empty()) {
                    return extract_json_string(body, "text");
                }
                for (const auto& change : changes) {
                    std::string new_text = extract_json_string(change, "text");
                    if (change.find("\"range\"") == std::string::npos) {
                        current = new_text;
                        continue;
                    }
                    auto lines = extract_json_ints(change, "line");
                    auto chars = extract_json_ints(change, "character");
                    if (lines.size() < 2 || chars.size() < 2) {
                        current = new_text;
                        continue;
                    }
                    size_t start = byte_offset_for_lsp_position(current, lines[0], chars[0]);
                    size_t end = byte_offset_for_lsp_position(current, lines[1], chars[1]);
                    if (end < start) std::swap(start, end);
                    current.replace(start, end - start, new_text);
                }
                return current;
            };

            auto symbol_kind_for = [](const std::string& kind_name) {
                if (kind_name == "func") return 12;
                if (kind_name == "class") return 5;
                if (kind_name == "enum") return 10;
                if (kind_name == "struct") return 23;
                if (kind_name == "constant") return 14;
                return 13;
            };

            auto completion_kind_for = [](const std::string& kind_name) {
                if (kind_name == "func") return 3;
                if (kind_name == "class") return 7;
                if (kind_name == "enum") return 13;
                if (kind_name == "struct") return 22;
                if (kind_name == "constant") return 21;
                return 6;
            };

            auto diagnostics_json = [&](const std::string& text) {
                std::ostringstream out;
                out << "[";
                bool first = true;
                auto add_diag = [&](int line_idx, int severity, const std::string& source, const std::string& message) {
                    int safe_line = std::max(0, line_idx);
                    if (!first) out << ",";
                    first = false;
                    out << "{\"range\":{\"start\":{\"line\":" << safe_line
                        << ",\"character\":0},\"end\":{\"line\":" << safe_line
                        << ",\"character\":1}},\"severity\":" << severity
                        << ",\"source\":\"" << json_escape(source)
                        << "\",\"message\":\"" << json_escape(message) << "\"}";
                };

                int depth = 0;
                int line_no = 0;
                std::istringstream lines(text);
                std::string line;
                std::regex close_re("^\\s*end\\b");
                std::regex open_re("^\\s*(if\\b.*\\bthen|while\\b.*\\bdo|for\\b.*\\bdo|foreach\\b.*\\bdo|repeat\\b.*\\bdo|func\\b.*\\bdo|class\\b.*\\bdo|struct\\b.*\\bdo|enum\\b.*\\bdo|try\\b)\\s*$");
                std::unordered_set<std::string> sensitive_header_vars;
                std::regex assignment_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+(?:is|=)\\s*(.+)$");
                std::regex sensitive_header_source_re(
                    "\\b(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
                    "\\bhttp\\.(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
                    "\\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key|x-auth-token|x-csrf-token|x-xsrf-token|token|secret|api[-_]?key)\\b",
                    std::regex_constants::ECMAScript | std::regex_constants::icase);
                std::regex output_re("^\\s*(print(?:_n)?\\b|log_(debug|info|warn|error)\\s*\\(|log\\.(debug|info|warn|error|event)\\s*\\(|console_(log|print|println|write|write_line|writeln|line|info|debug|warn|warning|error|exception|assert|time_log|time_end|time_stamp|count|count_reset|table|dir|dirxml|trace|group|group_collapsed|profile|profile_end)\\s*\\(|console\\.(log|print|println|write|write_line|writeln|line|info|debug|warn|warning|error|exception|assert|time_log|timeLog|time_end|timeEnd|time_stamp|timeStamp|count|count_reset|countReset|table|dir|dirxml|trace|group|group_collapsed|groupCollapsed|profile|profile_end|profileEnd)\\s*\\()");
                std::regex direct_tool_call_re("\\btool_call\\s*\\(|\\btool\\.call\\s*\\(");
                std::regex empty_tool_policy_re("^\\s*\\{\\s*\\}\\s*$");
                std::regex policy_literal_re("^\\s*\\{[^\\n]*\\}\\s*$");
                std::regex http_tool_policy_re("\\btools\\b[^\\n}]*http_(get|request)|http_(get|request)[^\\n}]*\\btools\\b",
                                               std::regex_constants::ECMAScript | std::regex_constants::icase);
                std::regex weak_tool_policy_call_re("\\b(tool_call_policy|tool\\.call_policy)\\s*\\(.*?,\\s*(\\{[^\\n]*\\}|[A-Za-z_][A-Za-z0-9_]*)\\s*\\)");
                std::regex legacy_command_re("^\\s*(print|print_n|print_no_nl|assert|assert_eq|assert_ne|assert_neq|assert_type|assert_len|assert_between|assert_approx|input|exit|clock|type|random)\\s+[^\\s(]");
                std::unordered_set<std::string> weak_tool_policy_vars;
                auto has_header_redaction = [](const std::string& value) {
                    return value.find("headers_redact") != std::string::npos;
                };
                auto has_sensitive_header_source = [&](const std::string& value) {
                    return !has_header_redaction(value) && std::regex_search(value, sensitive_header_source_re);
                };
                auto references_sensitive_header_var = [&](const std::string& value) {
                    for (const auto& name : sensitive_header_vars) {
                        if (std::regex_search(value, std::regex("\\b" + name + "\\b"))) return true;
                    }
                    return false;
                };
                auto is_weak_tool_policy_text = [&](const std::string& value) {
                    if (!std::regex_match(value, policy_literal_re)) return false;
                    if (std::regex_match(value, empty_tool_policy_re)) return true;
                    if (value.find("url_prefixes") != std::string::npos) return false;
                    if (std::regex_search(value, http_tool_policy_re)) return true;
                    return false;
                };
                auto is_weak_tool_policy_arg = [&](const std::string& value) {
                    if (is_weak_tool_policy_text(value)) return true;
                    return weak_tool_policy_vars.count(value) > 0;
                };
                while (std::getline(lines, line)) {
                    if (std::regex_search(line, close_re)) {
                        --depth;
                        if (depth < 0) {
                            add_diag(line_no, 1, "sura-lsp", "unmatched end");
                            depth = 0;
                        }
                    }
                    if (std::regex_search(line, open_re)) ++depth;
                    std::smatch assign_match;
                    if (std::regex_match(line, assign_match, assignment_re)) {
                        std::string name = assign_match[1].str();
                        std::string value = assign_match[2].str();
                        if (has_header_redaction(value)) {
                            sensitive_header_vars.erase(name);
                        } else if (has_sensitive_header_source(value) || references_sensitive_header_var(value)) {
                            sensitive_header_vars.insert(name);
                        }
                        if (is_weak_tool_policy_text(value)) {
                            weak_tool_policy_vars.insert(name);
                        } else {
                            weak_tool_policy_vars.erase(name);
                        }
                    }
                    if (!has_header_redaction(line) &&
                        std::regex_search(line, output_re) &&
                        (has_sensitive_header_source(line) || references_sensitive_header_var(line))) {
                        add_diag(line_no, 2, "sura-lint",
                                 "unredacted sensitive headers; use headers_redact before logging");
                    }
                    if (std::regex_search(line, direct_tool_call_re)) {
                        add_diag(line_no, 2, "sura-lint",
                                 "direct tool call bypasses policy; use tool_call_policy(spec, policy) or tool.call_policy(spec, policy)");
                    }
                    std::smatch weak_policy_match;
                    if (std::regex_search(line, weak_policy_match, weak_tool_policy_call_re) &&
                        weak_policy_match.size() >= 3 &&
                        is_weak_tool_policy_arg(weak_policy_match[2].str())) {
                        add_diag(line_no, 2, "sura-lint",
                                 "weak tool policy is too broad; restrict tools, url_prefixes, http_methods, and allow_shell");
                    }
                    if (std::regex_search(line, legacy_command_re)) {
                        add_diag(line_no, 2, "sura-lint",
                                 "legacy command syntax; use function-call syntax such as print(...)");
                    }
                    ++line_no;
                }
                if (depth > 0) add_diag(std::max(0, line_no - 1), 1, "sura-lsp", "unclosed block: expected end");

                try {
                    Parser parser;
                    parser.set_legacy_command_syntax(!strict_syntax);
                    auto parse_diagnostics = parser.diagnose_source(text);
                    if (!parse_diagnostics.empty()) {
                        for (const auto& e : parse_diagnostics) {
                            add_diag(e.line - 1, 1, "sura-parser", std::string("parse error: ") + e.message);
                        }
                    } else {
                        auto ast = parser.parse_source(text);
                        TypeChecker checker;
                        checker.check(static_cast<const SuraBlock*>(ast.get()));
                        for (const auto& e : checker.get_errors()) {
                            add_diag(e.line - 1, 2, "sura-typechecker", e.message.empty() ? "type checker warning" : e.message);
                        }
                    }
                } catch (const LexError& e) {
                    add_diag(e.line - 1, 1, "sura-lexer", std::string("lex error: ") + e.what());
                } catch (const ParseError& e) {
                    add_diag(e.line - 1, 1, "sura-parser", std::string("parse error: ") + e.what());
                } catch (const std::exception& e) {
                    add_diag(0, 1, "sura-analyzer", std::string("analysis error: ") + e.what());
                }

                out << "]";
                return out.str();
            };

            auto document_symbols_json = [&](const std::string& text) {
                std::ostringstream out;
                out << "[";
                bool first = true;
                int line_no = 0;
                int block_depth = 0;
                std::istringstream lines(text);
                std::string line;
                std::regex symbol_re("^\\s*(func|class|enum|struct)\\s+([A-Za-z_][A-Za-z0-9_]*)");
                std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
                std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
                std::regex block_open_re("\\bdo\\s*$");
                while (std::getline(lines, line)) {
                    std::smatch m;
                    if (std::regex_search(line, m, symbol_re)) {
                        std::string kind_name = m[1].str();
                        std::string name = m[2].str();
                        int kind = symbol_kind_for(kind_name);
                        if (!first) out << ",";
                        first = false;
                        out << "{\"name\":\"" << json_escape(name)
                            << "\",\"kind\":" << kind
                            << ",\"range\":{\"start\":{\"line\":" << line_no
                            << ",\"character\":0},\"end\":{\"line\":" << line_no
                            << ",\"character\":" << line.size()
                            << "}},\"selectionRange\":{\"start\":{\"line\":" << line_no
                            << ",\"character\":" << m.position(2)
                            << "},\"end\":{\"line\":" << line_no
                            << ",\"character\":" << (m.position(2) + m.length(2))
                            << "}}}";
                    } else if (block_depth == 0 && std::regex_match(line, m, constant_re)) {
                        std::string value = trim_ascii(m[2].str());
                        if (std::regex_match(value, literal_re)) {
                            std::string name = m[1].str();
                            if (!first) out << ",";
                            first = false;
                            out << "{\"name\":\"" << json_escape(name)
                                << "\",\"kind\":" << symbol_kind_for("constant")
                                << ",\"range\":{\"start\":{\"line\":" << line_no
                                << ",\"character\":0},\"end\":{\"line\":" << line_no
                                << ",\"character\":" << line.size()
                                << "}},\"selectionRange\":{\"start\":{\"line\":" << line_no
                                << ",\"character\":" << m.position(1)
                                << "},\"end\":{\"line\":" << line_no
                                << ",\"character\":" << (m.position(1) + m.length(1))
                                << "}}}";
                        }
                    }
                    std::string trimmed = trim_ascii(line);
                    if (std::regex_search(trimmed, block_open_re)) ++block_depth;
                    if (trimmed == "end" && block_depth > 0) --block_depth;
                    ++line_no;
                }
                out << "]";
                return out.str();
            };

            auto workspace_symbols_json = [&](const std::string& query) {
                std::ostringstream out;
                out << "[";
                bool first = true;
                std::string needle = lower_ascii(query);
                std::regex symbol_re("^\\s*(func|class|enum|struct)\\s+([A-Za-z_][A-Za-z0-9_]*)");
                std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
                std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
                std::regex block_open_re("\\bdo\\s*$");
                for (const auto& doc : sorted_documents()) {
                    int line_no = 0;
                    int block_depth = 0;
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        std::smatch m;
                        std::string trimmed = trim_ascii(line);
                        if (std::regex_search(line, m, symbol_re)) {
                            std::string kind_name = m[1].str();
                            std::string name = m[2].str();
                            if (needle.empty() || lower_ascii(name).find(needle) != std::string::npos) {
                                int start = (int)m.position(2);
                                int end = start + (int)m.length(2);
                                if (!first) out << ",";
                                first = false;
                                out << "{\"name\":\"" << json_escape(name)
                                    << "\",\"kind\":" << symbol_kind_for(kind_name)
                                    << ",\"location\":{\"uri\":\"" << json_escape(doc.first)
                                    << "\",\"range\":{\"start\":{\"line\":" << line_no
                                    << ",\"character\":" << start
                                    << "},\"end\":{\"line\":" << line_no
                                    << ",\"character\":" << end << "}}}"
                                    << ",\"containerName\":\"" << json_escape(doc.first) << "\"}";
                            }
                        } else if (block_depth == 0 && std::regex_match(line, m, constant_re)) {
                            std::string value = trim_ascii(m[2].str());
                            if (std::regex_match(value, literal_re)) {
                                std::string name = m[1].str();
                                if (needle.empty() || lower_ascii(name).find(needle) != std::string::npos) {
                                    int start = (int)m.position(1);
                                    int end = start + (int)m.length(1);
                                    if (!first) out << ",";
                                    first = false;
                                    out << "{\"name\":\"" << json_escape(name)
                                        << "\",\"kind\":" << symbol_kind_for("constant")
                                        << ",\"location\":{\"uri\":\"" << json_escape(doc.first)
                                        << "\",\"range\":{\"start\":{\"line\":" << line_no
                                        << ",\"character\":" << start
                                        << "},\"end\":{\"line\":" << line_no
                                        << ",\"character\":" << end << "}}}"
                                        << ",\"containerName\":\"" << json_escape(doc.first) << "\"}";
                                }
                            }
                        }
                        if (std::regex_search(trimmed, block_open_re)) ++block_depth;
                        if (trimmed == "end" && block_depth > 0) --block_depth;
                        ++line_no;
                    }
                }
                out << "]";
                return out.str();
            };

            auto completion_items_with_user_symbols = [&](const std::string& preferred_uri) {
                std::ostringstream out;
                out << "[";
                bool first = true;
                std::unordered_set<std::string> seen;
                auto append_item = [&](const std::string& label, const std::string& detail, int kind) {
                    if (label.empty() || seen.count(label)) return;
                    seen.insert(label);
                    if (!first) out << ",";
                    first = false;
                    out << "{\"label\":\"" << json_escape(label)
                        << "\",\"detail\":\"" << json_escape(detail)
                        << "\",\"kind\":" << kind << "}";
                };
                for (const auto& item : completions) append_item(item.first, item.second, 3);

                std::regex symbol_re("^\\s*(func|class|enum|struct)\\s+([A-Za-z_][A-Za-z0-9_]*)");
                std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
                std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
                std::regex block_open_re("\\bdo\\s*$");
                for (const auto& doc : ordered_documents(preferred_uri)) {
                    int block_depth = 0;
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        std::smatch m;
                        std::string trimmed = trim_ascii(line);
                        if (std::regex_search(line, m, symbol_re)) {
                            std::string kind_name = m[1].str();
                            append_item(m[2].str(), trimmed, completion_kind_for(kind_name));
                        } else if (block_depth == 0 && std::regex_match(line, m, constant_re)) {
                            std::string value = trim_ascii(m[2].str());
                            if (std::regex_match(value, literal_re)) {
                                append_item(m[1].str(), trimmed, completion_kind_for("constant"));
                            }
                        }
                        if (std::regex_search(trimmed, block_open_re)) ++block_depth;
                        if (trimmed == "end" && block_depth > 0) --block_depth;
                    }
                }
                out << "]";
                return out.str();
            };

            auto split_lines = [](const std::string& text) {
                std::vector<std::string> out;
                std::istringstream ss(text);
                std::string line;
                while (std::getline(ss, line)) out.push_back(line);
                if (!text.empty() && text.back() == '\n') out.push_back("");
                return out;
            };

            auto completion_items_for_position = [&](const std::string& uri, const std::string& text, int line_no, int character) {
                auto lines = split_lines(text);
                if (line_no >= 0 && line_no < (int)lines.size()) {
                    std::string line = lines[(size_t)line_no];
                    int pos = std::max(0, std::min(character, (int)line.size()));
                    std::string before = line.substr(0, (size_t)pos);
                    std::smatch match;
                    std::regex module_dot_re(R"(([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)?$)");
                    if (std::regex_search(before, match, module_dot_re)) {
                        std::string items = completion_items_for_module(match[1].str());
                        if (items != "[]") return items;
                    }
                }
                return completion_items_with_user_symbols(uri);
            };

            auto is_ident_char = [](char ch) {
                unsigned char c = (unsigned char)ch;
                return std::isalnum(c) || ch == '_';
            };

            auto word_at_position = [&](const std::string& text, int line_no, int character) {
                auto lines = split_lines(text);
                if (line_no < 0 || line_no >= (int)lines.size()) return std::string();
                const std::string& line = lines[(size_t)line_no];
                if (line.empty()) return std::string();
                int pos = std::max(0, std::min(character, (int)line.size()));
                if (pos == (int)line.size() || !is_ident_char(line[(size_t)pos])) {
                    if (pos > 0 && is_ident_char(line[(size_t)pos - 1])) --pos;
                }
                if (pos < 0 || pos >= (int)line.size() || !is_ident_char(line[(size_t)pos])) return std::string();
                int start = pos;
                int end = pos + 1;
                while (start > 0 && is_ident_char(line[(size_t)start - 1])) --start;
                while (end < (int)line.size() && is_ident_char(line[(size_t)end])) ++end;
                return line.substr((size_t)start, (size_t)(end - start));
            };

            auto dotted_symbol_at_position = [&](const std::string& text, int line_no, int character) {
                auto lines = split_lines(text);
                if (line_no < 0 || line_no >= (int)lines.size()) return std::string();
                const std::string& line = lines[(size_t)line_no];
                if (line.empty()) return std::string();
                int pos = std::max(0, std::min(character, (int)line.size()));
                if (pos == (int)line.size() || !is_ident_char(line[(size_t)pos])) {
                    if (pos > 0 && is_ident_char(line[(size_t)pos - 1])) --pos;
                }
                if (pos < 0 || pos >= (int)line.size() || !is_ident_char(line[(size_t)pos])) return std::string();
                int start = pos;
                int end = pos + 1;
                while (start > 0 && is_ident_char(line[(size_t)start - 1])) --start;
                while (end < (int)line.size() && is_ident_char(line[(size_t)end])) ++end;
                std::string word = line.substr((size_t)start, (size_t)(end - start));

                if (start > 0 && line[(size_t)start - 1] == '.') {
                    int module_end = start - 1;
                    int module_start = module_end - 1;
                    while (module_start >= 0 && is_ident_char(line[(size_t)module_start])) --module_start;
                    if (module_end > module_start + 1) {
                        return line.substr((size_t)(module_start + 1), (size_t)(module_end - (module_start + 1))) + "." + word;
                    }
                }
                if (end < (int)line.size() && line[(size_t)end] == '.') {
                    int member_start = end + 1;
                    int member_end = member_start;
                    while (member_end < (int)line.size() && is_ident_char(line[(size_t)member_end])) ++member_end;
                    if (member_end > member_start) {
                        return word + "." + line.substr((size_t)member_start, (size_t)(member_end - member_start));
                    }
                }
                return word;
            };

            auto user_function_signature_from_line = [&](const std::string& line, std::string& name, LspSignatureInfo& info) {
                std::smatch m;
                static const std::regex func_re("^\\s*func\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\(([^)]*)\\)\\s*(?:->\\s*([^\\s]+))?\\s*do\\b");
                if (!std::regex_search(line, m, func_re)) return false;
                name = m[1].str();
                std::string params = trim_lsp(m[2].str());
                info.label = name + "(" + params + ")";
                if (m.size() > 3 && m[3].matched) info.label += " -> " + m[3].str();
                info.documentation = "Sura function.";
                info.parameters.clear();
                if (!params.empty()) {
                    std::string current;
                    int bracket_depth = 0;
                    for (char ch : params) {
                        if (ch == '[') ++bracket_depth;
                        if (ch == ']') bracket_depth = std::max(0, bracket_depth - 1);
                        if (ch == ',' && bracket_depth == 0) {
                            std::string p = trim_lsp(current);
                            if (!p.empty()) info.parameters.push_back(p);
                            current.clear();
                            continue;
                        }
                        current.push_back(ch);
                    }
                    std::string p = trim_lsp(current);
                    if (!p.empty()) info.parameters.push_back(p);
                }
                return true;
            };

            auto constant_signature_for_symbol = [&](const std::string& preferred_uri, const std::string& symbol, LspSignatureInfo& info) {
                std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
                std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
                std::regex block_open_re("\\bdo\\s*$");
                for (const auto& doc : ordered_documents(preferred_uri)) {
                    int block_depth = 0;
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        std::smatch m;
                        std::string trimmed = trim_ascii(line);
                        if (block_depth == 0 && std::regex_match(line, m, constant_re) && m[1].str() == symbol) {
                            std::string value = trim_ascii(m[2].str());
                            if (std::regex_match(value, literal_re)) {
                                info.label = trimmed;
                                info.documentation = "Sura top-level constant.";
                                info.parameters.clear();
                                return true;
                            }
                        }
                        if (std::regex_search(trimmed, block_open_re)) ++block_depth;
                        if (trimmed == "end" && block_depth > 0) --block_depth;
                    }
                }
                return false;
            };

            auto inferred_type_for_expr = [&](const std::string& expr) {
                std::string value = trim_ascii(expr);
                static const std::regex number_re("^[-+]?[0-9]+(\\.[0-9]+)?$");
                static const std::regex string_re("^\"(\\\\.|[^\"])*\"$");
                if (std::regex_match(value, number_re)) return std::string("number");
                if (std::regex_match(value, string_re)) return std::string("string");
                if (value == "true" || value == "false") return std::string("bool");
                if (value == "nil") return std::string("nil");
                if (!value.empty() && value.front() == '[') return std::string("array");
                if (!value.empty() && value.front() == '{') return std::string("dict");
                return std::string();
            };

            auto variable_type_hover_json = [&](const std::string& text, int line_no, const std::string& symbol) {
                if (symbol.empty() || symbol.find('.') != std::string::npos) return std::string("null");
                std::regex assign_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+(?:is|=)\\s+(.+?)\\s*$");
                auto lines = split_lines(text);
                int max_line = std::min(line_no, (int)lines.size() - 1);
                std::string found_expr;
                std::string found_line;
                for (int i = 0; i <= max_line; ++i) {
                    std::smatch m;
                    const std::string& line = lines[(size_t)i];
                    if (std::regex_match(line, m, assign_re) && m[1].str() == symbol) {
                        std::string type = inferred_type_for_expr(m[2].str());
                        if (!type.empty()) {
                            found_expr = type;
                            found_line = trim_ascii(line);
                        }
                    }
                }
                if (found_expr.empty()) return std::string("null");
                std::string value = "```sura\n" + symbol + ": " + found_expr + "\n```\nInferred from `" + found_line + "`.";
                return std::string("{\"contents\":{\"kind\":\"markdown\",\"value\":\"") + json_escape(value) + "\"}}";
            };

            auto signature_info_for_symbol = [&](const std::string& preferred_uri, const std::string& symbol, LspSignatureInfo& info) {
                if (symbol.empty()) return false;
                if (module_signature_for_symbol(symbol, info)) return true;
                for (const auto& doc : ordered_documents(preferred_uri)) {
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        std::string name;
                        LspSignatureInfo found;
                        if (user_function_signature_from_line(line, name, found) && name == symbol) {
                            info = std::move(found);
                            return true;
                        }
                    }
                }
                if (constant_signature_for_symbol(preferred_uri, symbol, info)) return true;
                return builtin_signature_for(symbol, info);
            };

            struct LspCallInfo {
                std::string name;
                int active_parameter = 0;
            };

            auto active_call_at_position = [&](const std::string& text, int line_no, int character, LspCallInfo& call) {
                auto lines = split_lines(text);
                if (line_no < 0 || line_no >= (int)lines.size()) return false;
                const std::string& line = lines[(size_t)line_no];
                int limit = std::max(0, std::min(character, (int)line.size()));
                std::string prefix = line.substr(0, (size_t)limit);
                std::vector<LspCallInfo> stack;
                bool in_string = false;
                bool escape = false;
                int bracket_depth = 0;
                int brace_depth = 0;
                for (int i = 0; i < (int)prefix.size(); ++i) {
                    char ch = prefix[(size_t)i];
                    if (in_string) {
                        if (escape) escape = false;
                        else if (ch == '\\') escape = true;
                        else if (ch == '"') in_string = false;
                        continue;
                    }
                    if (ch == '"') {
                        in_string = true;
                        continue;
                    }
                    if (ch == '#') break;
                    if (ch == '[') {
                        ++bracket_depth;
                        continue;
                    }
                    if (ch == ']') {
                        bracket_depth = std::max(0, bracket_depth - 1);
                        continue;
                    }
                    if (ch == '{') {
                        ++brace_depth;
                        continue;
                    }
                    if (ch == '}') {
                        brace_depth = std::max(0, brace_depth - 1);
                        continue;
                    }
                    if (bracket_depth != 0 || brace_depth != 0) continue;
                    if (ch == '(') {
                        int j = i - 1;
                        while (j >= 0 && std::isspace((unsigned char)prefix[(size_t)j])) --j;
                        int end = j + 1;
                        while (j >= 0 && is_ident_char(prefix[(size_t)j])) --j;
                        std::string name = prefix.substr((size_t)(j + 1), (size_t)(end - (j + 1)));
                        if (j >= 0 && prefix[(size_t)j] == '.') {
                            int dot = j;
                            int k = dot - 1;
                            while (k >= 0 && is_ident_char(prefix[(size_t)k])) --k;
                            if (dot > k + 1 && !name.empty()) {
                                name = prefix.substr((size_t)(k + 1), (size_t)(dot - (k + 1))) + "." + name;
                            }
                        }
                        stack.push_back({name, 0});
                        continue;
                    }
                    if (ch == ')' && !stack.empty()) {
                        stack.pop_back();
                        continue;
                    }
                    if (ch == ',' && !stack.empty()) {
                        ++stack.back().active_parameter;
                        continue;
                    }
                }
                if (stack.empty() || stack.back().name.empty()) return false;
                call = stack.back();
                return true;
            };

            auto signature_help_json = [&](const std::string& uri, const std::string& text, int line_no, int character) {
                LspCallInfo call;
                if (!active_call_at_position(text, line_no, character, call)) return std::string("null");
                LspSignatureInfo info;
                if (!signature_info_for_symbol(uri, call.name, info)) return std::string("null");
                int active = call.active_parameter;
                if (!info.parameters.empty()) active = std::min(active, (int)info.parameters.size() - 1);
                else active = 0;

                std::ostringstream out;
                out << "{\"signatures\":[{\"label\":\"" << json_escape(info.label)
                    << "\",\"documentation\":\"" << json_escape(info.documentation)
                    << "\",\"parameters\":[";
                for (size_t i = 0; i < info.parameters.size(); ++i) {
                    if (i) out << ",";
                    out << "{\"label\":\"" << json_escape(info.parameters[i]) << "\"}";
                }
                out << "]}],\"activeSignature\":0,\"activeParameter\":" << active << "}";
                return out.str();
            };

            auto hover_json = [&](const std::string& uri, const std::string& text, int line_no, int character) {
                std::string symbol = dotted_symbol_at_position(text, line_no, character);
                LspSignatureInfo info;
                if (!signature_info_for_symbol(uri, symbol, info)) return variable_type_hover_json(text, line_no, symbol);
                std::string value = "```sura\n" + info.label + "\n```\n" + info.documentation;
                return std::string("{\"contents\":{\"kind\":\"markdown\",\"value\":\"") + json_escape(value) + "\"}}";
            };

            auto location_json = [&](const std::string& uri, int line_no, int start, int end) {
                std::ostringstream out;
                out << "{\"uri\":\"" << json_escape(uri)
                    << "\",\"range\":{\"start\":{\"line\":" << line_no
                    << ",\"character\":" << start
                    << "},\"end\":{\"line\":" << line_no
                    << ",\"character\":" << end << "}}}";
                return out.str();
            };

            auto definition_json = [&](const std::string& uri, const std::string& symbol) {
                if (symbol.empty()) return std::string("null");
                std::regex symbol_re("^\\s*(func|class|enum|struct)\\s+([A-Za-z_][A-Za-z0-9_]*)");
                std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
                std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
                std::regex block_open_re("\\bdo\\s*$");
                for (const auto& doc : ordered_documents(uri)) {
                    int line_no = 0;
                    int block_depth = 0;
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        std::smatch m;
                        if (std::regex_search(line, m, symbol_re) && m[2].str() == symbol) {
                            int start = (int)m.position(2);
                            int end = start + (int)m.length(2);
                            return location_json(doc.first, line_no, start, end);
                        }
                        if (block_depth == 0 && std::regex_match(line, m, constant_re) && m[1].str() == symbol) {
                            std::string value = trim_ascii(m[2].str());
                            if (std::regex_match(value, literal_re)) {
                                int start = (int)m.position(1);
                                int end = start + (int)m.length(1);
                                return location_json(doc.first, line_no, start, end);
                            }
                        }
                        std::string trimmed = trim_ascii(line);
                        if (std::regex_search(trimmed, block_open_re)) ++block_depth;
                        if (trimmed == "end" && block_depth > 0) --block_depth;
                        ++line_no;
                    }
                }
                return std::string("null");
            };

            auto references_json = [&](const std::string& symbol) {
                std::ostringstream out;
                out << "[";
                if (!symbol.empty()) {
                    bool first = true;
                    std::regex ref_re("\\b" + symbol + "\\b");
                    for (const auto& doc : sorted_documents()) {
                        int line_no = 0;
                        std::istringstream lines(doc.second);
                        std::string line;
                        while (std::getline(lines, line)) {
                            for (auto it = std::sregex_iterator(line.begin(), line.end(), ref_re);
                                 it != std::sregex_iterator(); ++it) {
                                if (!first) out << ",";
                                first = false;
                                int start = (int)(*it).position();
                                int end = start + (int)(*it).length();
                                out << location_json(doc.first, line_no, start, end);
                            }
                            ++line_no;
                        }
                    }
                }
                out << "]";
                return out.str();
            };

            auto semantic_tokens_json = [&](const std::string& text) {
                std::vector<std::string> keywords = {
                    "is", "if", "elif", "else", "then", "while", "for", "foreach", "in",
                    "repeat", "do", "end", "func", "class", "struct", "enum", "when",
                    "try", "catch", "break", "continue", "return", "use", "import", "global",
                    "true", "false", "nil", "and", "or", "not", "new"
                };
                auto is_keyword = [&](const std::string& word) {
                    return std::find(keywords.begin(), keywords.end(), word) != keywords.end();
                };
                auto emit_token = [&](std::ostringstream& out, bool& first, int& prev_line, int& prev_start,
                                      int line_no, int start, int length, int token_type, int modifiers) {
                    if (length <= 0) return;
                    int delta_line = first ? line_no : line_no - prev_line;
                    int delta_start = (first || delta_line != 0) ? start : start - prev_start;
                    if (!first) out << ",";
                    first = false;
                    out << delta_line << "," << delta_start << "," << length << "," << token_type << "," << modifiers;
                    prev_line = line_no;
                    prev_start = start;
                };

                std::ostringstream out;
                out << "{\"data\":[";
                bool first = true;
                int prev_line = 0;
                int prev_start = 0;
                auto lines = split_lines(text);
                for (int line_no = 0; line_no < (int)lines.size(); ++line_no) {
                    const std::string& line = lines[(size_t)line_no];
                    int pending_decl_type = -1;
                    for (int i = 0; i < (int)line.size();) {
                        unsigned char c = (unsigned char)line[(size_t)i];
                        if (std::isspace(c)) {
                            ++i;
                            continue;
                        }
                        if (line[(size_t)i] == '#') {
                            emit_token(out, first, prev_line, prev_start, line_no, i, (int)line.size() - i, 8, 0);
                            break;
                        }
                        if (line[(size_t)i] == '"') {
                            int start = i++;
                            bool escaped = false;
                            while (i < (int)line.size()) {
                                char ch = line[(size_t)i++];
                                if (escaped) {
                                    escaped = false;
                                } else if (ch == '\\') {
                                    escaped = true;
                                } else if (ch == '"') {
                                    break;
                                }
                            }
                            emit_token(out, first, prev_line, prev_start, line_no, start, i - start, 7, 0);
                            pending_decl_type = -1;
                            continue;
                        }
                        if (std::isdigit(c)) {
                            int start = i++;
                            while (i < (int)line.size()) {
                                unsigned char n = (unsigned char)line[(size_t)i];
                                if (!(std::isalnum(n) || line[(size_t)i] == '_' || line[(size_t)i] == '.')) break;
                                ++i;
                            }
                            emit_token(out, first, prev_line, prev_start, line_no, start, i - start, 6, 0);
                            pending_decl_type = -1;
                            continue;
                        }
                        if (std::isalpha(c) || line[(size_t)i] == '_') {
                            int start = i++;
                            while (i < (int)line.size() && is_ident_char(line[(size_t)i])) ++i;
                            std::string word = line.substr((size_t)start, (size_t)(i - start));
                            if (is_keyword(word)) {
                                emit_token(out, first, prev_line, prev_start, line_no, start, i - start, 0, 0);
                                if (word == "func") pending_decl_type = 1;
                                else if (word == "class") pending_decl_type = 2;
                                else if (word == "enum") pending_decl_type = 3;
                                else if (word == "struct") pending_decl_type = 4;
                                else pending_decl_type = -1;
                                continue;
                            }
                            if (pending_decl_type >= 0) {
                                emit_token(out, first, prev_line, prev_start, line_no, start, i - start, pending_decl_type, 3);
                                pending_decl_type = -1;
                                continue;
                            }
                            int look = i;
                            while (look < (int)line.size() && std::isspace((unsigned char)line[(size_t)look])) ++look;
                            int type = (look < (int)line.size() && line[(size_t)look] == '(') ? 1 : 5;
                            if (type != 1 && std::binary_search(builtin_names.begin(), builtin_names.end(), word))
                                type = 1;
                            emit_token(out, first, prev_line, prev_start, line_no, start, i - start, type, 0);
                            continue;
                        }
                        if (std::ispunct(c)) {
                            emit_token(out, first, prev_line, prev_start, line_no, i, 1, 9, 0);
                        }
                        pending_decl_type = -1;
                        ++i;
                    }
                }
                out << "]}";
                return out.str();
            };

            auto full_document_range_json = [&](const std::string& text) {
                auto lines = split_lines(text);
                int end_line = 0;
                int end_char = 0;
                if (!lines.empty()) {
                    end_line = (int)lines.size() - 1;
                    end_char = (int)lines.back().size();
                }
                std::ostringstream out;
                out << "{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":" << end_line
                    << ",\"character\":" << end_char << "}}";
                return out.str();
            };

            auto formatting_edits_json = [&](const std::string& text) {
                return std::string("[{\"range\":") + full_document_range_json(text)
                    + ",\"newText\":\"" + json_escape(format_sura_text(text)) + "\"}]";
            };

            auto document_end_range_json = [&](const std::string& text) {
                auto lines = split_lines(text);
                int end_line = 0;
                int end_char = 0;
                if (!lines.empty()) {
                    end_line = (int)lines.size() - 1;
                    end_char = (int)lines.back().size();
                }
                std::ostringstream out;
                out << "{\"start\":{\"line\":" << end_line << ",\"character\":" << end_char
                    << "},\"end\":{\"line\":" << end_line << ",\"character\":" << end_char << "}}";
                return out.str();
            };

            auto valid_identifier = [](const std::string& name) {
                if (name.empty()) return false;
                unsigned char first = (unsigned char)name[0];
                if (!(std::isalpha(first) || name[0] == '_')) return false;
                for (char ch : name) {
                    unsigned char c = (unsigned char)ch;
                    if (!(std::isalnum(c) || ch == '_')) return false;
                }
                return true;
            };

            auto rename_workspace_edit_json = [&](const std::string& symbol, const std::string& new_name) {
                if (symbol.empty() || !valid_identifier(new_name)) return std::string("null");
                std::ostringstream out;
                out << "{\"changes\":{";
                bool first_uri = true;
                std::regex ref_re("\\b" + symbol + "\\b");
                for (const auto& doc : sorted_documents()) {
                    std::ostringstream edits;
                    bool first_edit = true;
                    int line_no = 0;
                    std::istringstream lines(doc.second);
                    std::string line;
                    while (std::getline(lines, line)) {
                        for (auto it = std::sregex_iterator(line.begin(), line.end(), ref_re);
                             it != std::sregex_iterator(); ++it) {
                            if (!first_edit) edits << ",";
                            first_edit = false;
                            int start = (int)(*it).position();
                            int end = start + (int)(*it).length();
                            edits << "{\"range\":{\"start\":{\"line\":" << line_no
                                << ",\"character\":" << start
                                << "},\"end\":{\"line\":" << line_no
                                << ",\"character\":" << end
                                << "}},\"newText\":\"" << json_escape(new_name) << "\"}";
                        }
                        ++line_no;
                    }
                    if (!first_edit) {
                        if (!first_uri) out << ",";
                        first_uri = false;
                        out << "\"" << json_escape(doc.first) << "\":[" << edits.str() << "]";
                    }
                }
                out << "}}";
                return out.str();
            };

            auto code_actions_json = [&](const std::string& uri, const std::string& text) {
                std::vector<std::string> actions;
                int depth = 0;
                std::istringstream lines(text);
                std::string line;
                std::regex close_re("^\\s*end\\b");
                std::regex open_re("^\\s*(if\\b.*\\bthen|while\\b.*\\bdo|for\\b.*\\bdo|foreach\\b.*\\bdo|repeat\\b.*\\bdo|func\\b.*\\bdo|class\\b.*\\bdo|struct\\b.*\\bdo|enum\\b.*\\bdo|try\\b)\\s*$");
                while (std::getline(lines, line)) {
                    if (std::regex_search(line, close_re)) depth = std::max(0, depth - 1);
                    if (std::regex_search(line, open_re)) ++depth;
                }
                if (depth > 0 && !uri.empty()) {
                    std::string insertion = (!text.empty() && text.back() != '\n') ? "\nend\n" : "end\n";
                    actions.push_back(std::string("{\"title\":\"Insert missing end\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + document_end_range_json(text)
                        + ",\"newText\":\"" + json_escape(insertion) + "\"}]}}}");
                }

                auto matching_call_close = [&](const std::string& source_line, size_t open) -> size_t {
                    int call_depth = 0;
                    bool in_string = false;
                    bool escape = false;
                    for (size_t i = open; i < source_line.size(); ++i) {
                        char ch = source_line[i];
                        if (in_string) {
                            if (escape) escape = false;
                            else if (ch == '\\') escape = true;
                            else if (ch == '"') in_string = false;
                            continue;
                        }
                        if (ch == '"') {
                            in_string = true;
                            continue;
                        }
                        if (ch == '#') break;
                        if (ch == '(') ++call_depth;
                        else if (ch == ')') {
                            --call_depth;
                            if (call_depth == 0) return i;
                        }
                    }
                    return std::string::npos;
                };
                auto split_call_args = [&](const std::string& args_text) {
                    std::vector<std::string> args;
                    std::string current;
                    bool in_string = false;
                    bool escape = false;
                    int paren_depth = 0;
                    int bracket_depth = 0;
                    int brace_depth = 0;
                    for (char ch : args_text) {
                        if (in_string) {
                            current.push_back(ch);
                            if (escape) escape = false;
                            else if (ch == '\\') escape = true;
                            else if (ch == '"') in_string = false;
                            continue;
                        }
                        if (ch == '"') {
                            in_string = true;
                            current.push_back(ch);
                            continue;
                        }
                        if (ch == '(') ++paren_depth;
                        else if (ch == ')') paren_depth = std::max(0, paren_depth - 1);
                        else if (ch == '[') ++bracket_depth;
                        else if (ch == ']') bracket_depth = std::max(0, bracket_depth - 1);
                        else if (ch == '{') ++brace_depth;
                        else if (ch == '}') brace_depth = std::max(0, brace_depth - 1);
                        if (ch == ',' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
                            args.push_back(trim_lsp(current));
                            current.clear();
                            continue;
                        }
                        current.push_back(ch);
                    }
                    std::string tail = trim_lsp(current);
                    if (!tail.empty() || !args_text.empty()) args.push_back(tail);
                    return args;
                };

                auto doc_lines = split_lines(text);
                const std::string starter_tool_policy = "{tools: [\"http_get\"], url_prefixes: [\"file://\"], http_methods: [\"GET\"], allow_shell: false}";
                std::unordered_set<std::string> sensitive_header_vars;
                std::regex assignment_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+(?:is|=)\\s*(.+)$");
                std::regex empty_tool_policy_re("^\\s*\\{\\s*\\}\\s*$");
                std::regex policy_literal_re("^\\s*\\{[^\\n]*\\}\\s*$");
                std::regex http_tool_policy_re("\\btools\\b[^\\n}]*http_(get|request)|http_(get|request)[^\\n}]*\\btools\\b",
                                                std::regex_constants::ECMAScript | std::regex_constants::icase);
                std::regex sensitive_header_source_re(
                    "\\b(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
                    "\\bhttp\\.(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
                    "\\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key|x-auth-token|x-csrf-token|x-xsrf-token|token|secret|api[-_]?key)\\b",
                    std::regex_constants::ECMAScript | std::regex_constants::icase);
                auto has_header_redaction = [](const std::string& value) {
                    return value.find("headers_redact") != std::string::npos;
                };
                auto has_sensitive_header_source = [&](const std::string& value) {
                    return !has_header_redaction(value) && std::regex_search(value, sensitive_header_source_re);
                };
                auto references_sensitive_header_var = [&](const std::string& value) {
                    for (const auto& name : sensitive_header_vars) {
                        if (std::regex_search(value, std::regex("\\b" + name + "\\b"))) return true;
                    }
                    return false;
                };
                auto is_weak_tool_policy_text = [&](const std::string& value) {
                    if (!std::regex_match(value, policy_literal_re)) return false;
                    if (std::regex_match(value, empty_tool_policy_re)) return true;
                    if (value.find("url_prefixes") != std::string::npos) return false;
                    return std::regex_search(value, http_tool_policy_re);
                };
                std::regex print_output_re("^(\\s*print(?:_n)?\\s+)(.+?)(\\s*)$");
                std::vector<std::string> log_call_names = {
                    "log_debug", "log_info", "log_warn", "log_error",
                    "log.debug", "log.info", "log.warn", "log.error", "log.event",
                    "console_log", "console_print", "console_println", "console_write", "console_write_line",
                    "console_info", "console_debug", "console_warn", "console_error",
                    "console.log", "console.print", "console.println", "console.write", "console.write_line",
                    "console.info", "console.debug", "console.warn", "console.error"
                };
                auto add_redact_action = [&](int line_no, size_t expr_start, const std::string& expr) {
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << expr_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":"
                          << (expr_start + expr.size()) << "}}";
                    std::string replacement = "headers_redact(" + expr + ")";
                    actions.push_back(std::string("{\"title\":\"Wrap sensitive headers with headers_redact\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                };
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    std::smatch assign_match;
                    if (std::regex_match(source_line, assign_match, assignment_re)) {
                        std::string name = assign_match[1].str();
                        std::string value = assign_match[2].str();
                        if (has_header_redaction(value)) {
                            sensitive_header_vars.erase(name);
                        } else if (has_sensitive_header_source(value) || references_sensitive_header_var(value)) {
                            sensitive_header_vars.insert(name);
                        }
                    }
                    std::smatch print_match;
                    if (!has_header_redaction(source_line) &&
                        std::regex_match(source_line, print_match, print_output_re)) {
                        std::string expr = trim_lsp(print_match[2].str());
                        if (!expr.empty() &&
                            (has_sensitive_header_source(expr) || references_sensitive_header_var(expr))) {
                            size_t expr_start = source_line.find(expr);
                            if (expr_start != std::string::npos) {
                                add_redact_action(line_no, expr_start, expr);
                            }
                        }
                    }
                    if (has_header_redaction(source_line)) continue;
                    for (const auto& call_name : log_call_names) {
                        size_t call_start = source_line.find(call_name + "(");
                        if (call_start == std::string::npos) continue;
                        if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                        size_t open = call_start + call_name.size();
                        size_t close = matching_call_close(source_line, open);
                        if (close == std::string::npos) continue;
                        std::string args_text = source_line.substr(open + 1, close - open - 1);
                        auto args = split_call_args(args_text);
                        size_t search_from = 0;
                        for (const auto& arg : args) {
                            if (arg.empty()) continue;
                            size_t arg_pos = args_text.find(arg, search_from);
                            if (arg_pos == std::string::npos) arg_pos = args_text.find(arg);
                            if (arg_pos != std::string::npos) search_from = arg_pos + arg.size();
                            if (has_sensitive_header_source(arg) || references_sensitive_header_var(arg)) {
                                if (arg_pos != std::string::npos) add_redact_action(line_no, open + 1 + arg_pos, arg);
                                break;
                            }
                        }
                        break;
                    }
                }
                std::regex direct_tool_call_re("\\btool_call\\s*\\(");
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    std::smatch call_match;
                    if (!std::regex_search(source_line, call_match, direct_tool_call_re)) continue;
                    size_t call_start = (size_t)call_match.position();
                    size_t open = call_start + call_match.length() - 1;
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "tool_call_policy(" + args[0] + ", policy)";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Wrap tool_call with tool_call_policy\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                }
                std::regex module_tool_call_re("\\btool\\.call\\s*\\(");
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    std::smatch call_match;
                    if (!std::regex_search(source_line, call_match, module_tool_call_re)) continue;
                    size_t call_start = (size_t)call_match.position();
                    size_t open = call_start + call_match.length() - 1;
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "tool.call_policy(" + args[0] + ", policy)";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Wrap tool.call with tool.call_policy\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                }
                auto add_starter_policy_action = [&](int line_no, size_t start, size_t end) {
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << end << "}}";
                    actions.push_back(std::string("{\"title\":\"Replace empty tool policy with starter policy\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(starter_tool_policy) + "\"}]}}}");
                };
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    std::smatch assign_match;
                    if (!std::regex_match(source_line, assign_match, assignment_re)) continue;
                    std::string value = assign_match[2].str();
                    if (!is_weak_tool_policy_text(value)) continue;
                    size_t start = source_line.find(value);
                    if (start == std::string::npos) continue;
                    add_starter_policy_action(line_no, start, start + value.size());
                }
                std::vector<std::string> policy_call_names = {"tool_call_policy", "tool.call_policy"};
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    for (const auto& call_name : policy_call_names) {
                        size_t call_start = source_line.find(call_name + "(");
                        if (call_start == std::string::npos) continue;
                        if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                        size_t open = call_start + call_name.size();
                        size_t close = matching_call_close(source_line, open);
                        if (close == std::string::npos) continue;
                        std::string args_text = source_line.substr(open + 1, close - open - 1);
                        auto args = split_call_args(args_text);
                        if (args.size() != 2 || !is_weak_tool_policy_text(args[1])) continue;
                        size_t arg_pos = args_text.rfind(args[1]);
                        if (arg_pos == std::string::npos) continue;
                        add_starter_policy_action(line_no, open + 1 + arg_pos, open + 1 + arg_pos + args[1].size());
                        break;
                    }
                }
                auto add_call_name_conversion_actions = [&](const std::string& from, const std::string& to) {
                    const std::string needle = from + "(";
                    for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                        const std::string& source_line = doc_lines[(size_t)line_no];
                        size_t search_from = 0;
                        while (true) {
                            size_t call_start = source_line.find(needle, search_from);
                            if (call_start == std::string::npos) break;
                            search_from = call_start + needle.size();
                            if (call_start > 0 &&
                                (is_ident_char(source_line[call_start - 1]) || source_line[call_start - 1] == '.')) {
                                continue;
                            }
                            std::ostringstream range;
                            range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                                  << "},\"end\":{\"line\":" << line_no << ",\"character\":"
                                  << (call_start + from.size()) << "}}";
                            actions.push_back(std::string("{\"title\":\"Convert ") + from + " to " + to +
                                "\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"" + json_escape(uri) +
                                "\":[{\"range\":" + range.str() + ",\"newText\":\"" + json_escape(to) + "\"}]}}}");
                        }
                    }
                };
                add_call_name_conversion_actions("python_available", "python.available");
                add_call_name_conversion_actions("python_executable", "python.executable");
                add_call_name_conversion_actions("python_call_json", "python.call_json");
                add_call_name_conversion_actions("python_call", "python.call");
                add_call_name_conversion_actions("ffi_load", "ffi.load");
                add_call_name_conversion_actions("ffi_call", "ffi.call");
                add_call_name_conversion_actions("plugin_load_manifest", "plugin.load_manifest");
                add_call_name_conversion_actions("plugin_load", "plugin.load");
                add_call_name_conversion_actions("plugin_call", "plugin.call");
                add_call_name_conversion_actions("plugin_info", "plugin.info");
                add_call_name_conversion_actions("plugin_unload", "plugin.unload");
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    size_t call_start = source_line.find("http_post(");
                    if (call_start == std::string::npos) continue;
                    if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                    size_t open = call_start + std::string("http_post").size();
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() < 2 || args.size() > 3 || args[0].empty() || args[1].empty()) continue;
                    std::string replacement = "http_request({method: \"POST\", url: " + args[0] + ", body: " + args[1];
                    if (args.size() == 3 && !args[2].empty()) replacement += ", content_type: " + args[2];
                    replacement += "})";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Convert http_post to http_request\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                    break;
                }
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    size_t call_start = source_line.find("http_get(");
                    if (call_start == std::string::npos) continue;
                    if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                    size_t open = call_start + std::string("http_get").size();
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "http_request({method: \"GET\", url: " + args[0] + "})";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Convert http_get to http_request\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                    break;
                }
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    size_t call_start = source_line.find("http_json(");
                    if (call_start == std::string::npos) continue;
                    if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                    size_t open = call_start + std::string("http_json").size();
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "http_request_json({method: \"GET\", url: " + args[0] + "})";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Convert http_json to http_request_json\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                    break;
                }
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    size_t call_start = source_line.find("async_http_get(");
                    if (call_start == std::string::npos) continue;
                    if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                    size_t open = call_start + std::string("async_http_get").size();
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "async.http_get(" + args[0] + ")";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Convert async_http_get to async.http_get\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                    break;
                }
                for (int line_no = 0; line_no < (int)doc_lines.size(); ++line_no) {
                    const std::string& source_line = doc_lines[(size_t)line_no];
                    size_t call_start = source_line.find("async_http_request(");
                    if (call_start == std::string::npos) continue;
                    if (call_start > 0 && is_ident_char(source_line[call_start - 1])) continue;
                    size_t open = call_start + std::string("async_http_request").size();
                    size_t close = matching_call_close(source_line, open);
                    if (close == std::string::npos) continue;
                    auto args = split_call_args(source_line.substr(open + 1, close - open - 1));
                    if (args.size() != 1 || args[0].empty()) continue;
                    std::string replacement = "async.http_request(" + args[0] + ")";
                    std::ostringstream range;
                    range << "{\"start\":{\"line\":" << line_no << ",\"character\":" << call_start
                          << "},\"end\":{\"line\":" << line_no << ",\"character\":" << (close + 1) << "}}";
                    actions.push_back(std::string("{\"title\":\"Convert async_http_request to async.http_request\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{\"")
                        + json_escape(uri) + "\":[{\"range\":" + range.str()
                        + ",\"newText\":\"" + json_escape(replacement) + "\"}]}}}");
                    break;
                }

                std::ostringstream out;
                out << "[";
                for (size_t i = 0; i < actions.size(); ++i) {
                    if (i) out << ",";
                    out << actions[i];
                }
                out << "]";
                return out.str();
            };

            auto publish_diagnostics = [&](const std::string& uri, const std::string& text) {
                if (uri.empty()) return;
                send_lsp("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\""
                    + json_escape(uri) + "\",\"diagnostics\":" + diagnostics_json(text) + "}}");
            };

            auto handle_body = [&](const std::string& body, bool framed) {
                std::string id = extract_json_field(body, "id");
                std::string method = extract_method(body);
                if (method == "initialize") {
                    std::string root_uri = extract_json_string(body, "rootUri");
                    workspace_root_path = file_uri_to_path(root_uri);
                    index_workspace();
                    std::string result =
                        "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":2},\"completionProvider\":{\"triggerCharacters\":[\".\",\"(\"]},\"hoverProvider\":true,\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},\"documentSymbolProvider\":true,\"workspaceSymbolProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"documentFormattingProvider\":true,\"renameProvider\":true,\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\"]},\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"keyword\",\"function\",\"class\",\"enum\",\"struct\",\"variable\",\"number\",\"string\",\"comment\",\"operator\"],\"tokenModifiers\":[\"declaration\",\"definition\"]},\"full\":true}}}";
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
                    return true;
                }
                if (method == "textDocument/didOpen") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = extract_json_string(body, "text");
                    if (!uri.empty()) {
                        documents[uri] = text;
                        publish_diagnostics(uri, text);
                    }
                    return true;
                }
                if (method == "textDocument/didChange") {
                    std::string uri = extract_json_string(body, "uri");
                    if (!uri.empty()) {
                        std::string text = apply_lsp_content_changes(uri, body);
                        documents[uri] = text;
                        publish_diagnostics(uri, text);
                    }
                    return true;
                }
                if (method == "textDocument/didClose") {
                    std::string uri = extract_json_string(body, "uri");
                    if (!uri.empty()) {
                        documents.erase(uri);
                        send_lsp("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\""
                            + json_escape(uri) + "\",\"diagnostics\":[]}}");
                    }
                    return true;
                }
                if (method == "textDocument/completion") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    std::string result = "{\"isIncomplete\":false,\"items\":" +
                        completion_items_for_position(uri, text, line, character) + "}";
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
                    return true;
                }
                if (method == "textDocument/hover") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + hover_json(uri, text, line, character) + "}");
                    return true;
                }
                if (method == "textDocument/signatureHelp") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + signature_help_json(uri, text, line, character) + "}");
                    return true;
                }
                if (method == "textDocument/documentSymbol") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + document_symbols_json(text) + "}");
                    return true;
                }
                if (method == "workspace/symbol") {
                    std::string query = extract_json_string(body, "query");
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + workspace_symbols_json(query) + "}");
                    return true;
                }
                if (method == "textDocument/semanticTokens/full") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + semantic_tokens_json(text) + "}");
                    return true;
                }
                if (method == "textDocument/definition") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    std::string symbol = word_at_position(text, line, character);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + definition_json(uri, symbol) + "}");
                    return true;
                }
                if (method == "textDocument/references") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    std::string symbol = word_at_position(text, line, character);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + references_json(symbol) + "}");
                    return true;
                }
                if (method == "textDocument/formatting") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + formatting_edits_json(text) + "}");
                    return true;
                }
                if (method == "textDocument/rename") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    int line = extract_json_int(body, "line", 0);
                    int character = extract_json_int(body, "character", 0);
                    std::string symbol = word_at_position(text, line, character);
                    std::string new_name = extract_json_string(body, "newName");
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + rename_workspace_edit_json(symbol, new_name) + "}");
                    return true;
                }
                if (method == "textDocument/codeAction") {
                    std::string uri = extract_json_string(body, "uri");
                    std::string text = document_text(uri);
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + code_actions_json(uri, text) + "}");
                    return true;
                }
                if (method == "shutdown") {
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":null}");
                    return false;
                }
                if (method == "exit") return false;

                if (!framed) {
                    std::cout << "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":" << completion_items_with_user_symbols("") << "}" << std::endl;
                } else if (id != "null") {
                    send_lsp("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32601,\"message\":\"method not found\"}}");
                }
                return true;
            };

            while (true) {
                std::string line;
                if (!std::getline(std::cin, line)) break;
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.rfind("Content-Length:", 0) != 0) {
                    if (!line.empty() && !handle_body(line, false)) break;
                    continue;
                }

                int length = std::stoi(line.substr(std::string("Content-Length:").size()));
                while (std::getline(std::cin, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) break;
                }

                std::string body((size_t)length, '\0');
                std::cin.read(&body[0], length);
                if (!handle_body(body, true)) break;
            }
            return 0;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Sura Language Runtime " << SURA_LANGUAGE_VERSION << "\n"
                      << "Usage:\n"
                      << "  sura --version             Show the Sura Language version\n"
                      << "  sura --jit-info            Show native JIT OS, architecture, ABI, and fallback\n"
                      << "  sura --jit-info-json       Show machine-readable JIT target information\n"
                      << "  sura <file.sura> [--] [args...]  Run file with script argv\n"
                      << "  sura --repl                Interactive REPL\n"
                      << "  sura --dump <file.sura>    Show bytecode\n"
                      << "  sura --bench <file.sura>   Benchmark\n"
                      << "  sura --strict <file.sura>  Explicitly select the default strict type checking\n"
                      << "  sura --legacy-types <file.sura>  Legacy mode: report type warnings and continue\n"
                      << "  sura --compile <file.sura>  Compile to .sura.bc (no run)\n"
                      << "  sura --release <file.sura>  Build a .sura.srp release container\n"
                      << "  sura --release-key <key>    Require key for --release package execution\n"
                      << "  sura --release-key-file <path> Read release key from a UTF-8 file\n"
                      << "  sura --release-license <license> Require license value for release execution\n"
                      << "  sura --release-license-file <path> Read release license from a UTF-8 file\n"
                      << "  sura --release-id <id>      Store release/customer id in package metadata\n"
                      << "  sura --release-expires YYYY-MM-DD  Expire release package after date\n"
                      << "  sura --out <path>           Output path for --compile/--release\n"
                      << "  sura --target uefi-x86_64 --out BOOTX64.EFI <file.sura>\n"
                      << "                              Build a VM-free, bootable UEFI x86-64 image\n"
               << "  sura --profile <file.sura>  Run with type profiling report\n"
               << "  sura --profile-json out.json <file.sura>  Write machine-readable profile report\n"
               << "  sura --gc-stats <file.sura>  Run a final GC and print pause/object statistics\n"
               << "  sura --gc-stats-json out.json <file.sura>  Write machine-readable GC statistics\n"
              << "  sura --trace <file.sura>    Trace VM instructions\n"
              << "  sura --debug <file.sura>    Dump bytecode, trace, and profile\n"
              << "  sura --debug-protocol <file.sura>  Run line-stop debugger protocol\n"
              << "  sura --lang en|ko           Set diagnostic language (default: English; env SURA_LANG)\n"
              << "  sura --test [path]          Discover and run Sura tests\n"
              << "  sura --test-report out.json [path]  Write machine-readable test results\n"
              << "  sura --check [path]         Parse and typecheck without running\n"
              << "  sura --strict-syntax [path] Reject legacy command-style calls; use name(...)\n"
              << "  sura --ast-json [--out out.json] <file.sura>  Emit machine-readable AST JSON\n"
              << "  sura --lint [path]          Run lightweight static lint checks\n"
              << "  sura --format <path>        Format Sura files in place\n"
              << "  sura --format-check <path>  Check Sura formatting without writing\n"
              << "  sura --jit <file.sura>      Enable native JIT (Win64 x64 plus x64/ARM64 baselines; VM fallback)\n"
              << "  sura --load <file.sura.bc> Run precompiled bytecode\n"
              << "  sura --load-release <file.sura.srp> Run a .sura.srp release container\n"
              << "  sura --load-release-key <key> Key for .sura.srp release container\n"
              << "  sura --load-release-key-file <path> Read release key from a UTF-8 file\n"
              << "  sura --load-release-license <license> License for .sura.srp release container\n"
              << "  sura --load-release-license-file <path> Read release license from a UTF-8 file\n"
              << "  SURA_ALLOW_RELEASE_INSPECT=1 allows dump/trace/debug on protected packages\n"
              << "  sura --lsp                 Language Server mode\n";
            return 0;
        }
        else {
            if (filename.empty()) filename = arg;
            else script_args.push_back(arg);
        }
    }

    if ((release_only || compile_only || !freestanding_target.empty()) && !strict_mode) {
        const char* strict_operation = release_only ? "--release" :
            (compile_only ? "--compile" : "--target");
        std::cerr << "[Error] " << strict_operation
                  << " requires strict type safety; --legacy-types is only available "
                     "for source execution, checking, and tests.\n";
        return 1;
    }

    if (!freestanding_target.empty() &&
        (compile_only || release_only || load_bc || load_release || test_mode ||
         check_mode || format_mode || lint_mode || ast_json_mode || repl_mode ||
         jit_mode || bench_mode || profile_mode || gc_stats_mode ||
         !gc_stats_json_path.empty())) {
        std::cerr << "[Error] --target uefi-x86_64 is a standalone build mode; "
                     "do not combine it with run, VM, JIT, test, or inspection modes.\n";
        return 1;
    }

    if ((gc_stats_mode || !gc_stats_json_path.empty()) &&
        (test_mode || check_mode || format_mode || lint_mode ||
         compile_only || release_only)) {
        std::cerr << "[Error] GC statistics require execution of one program or a REPL session.\n";
        return 1;
    }

    auto emit_gc_stats = [&](JitVM& vm) {
        if (!gc_stats_mode && gc_stats_json_path.empty()) return true;

        // A final full collection makes short programs measurable too. It runs
        // after execution timing has stopped, so --bench remains program-only.
        vm.collect_garbage();
        const SuraGcStats& stats = vm.garbage_collection_stats();
        const double average_pause_us = stats.collections == 0
            ? 0.0
            : static_cast<double>(stats.total_pause_us) /
              static_cast<double>(stats.collections);

        if (gc_stats_mode) {
            std::cout << "\n=== Sura GC Statistics ===\n"
                      << "Collections: " << stats.collections << "\n"
                      << "Objects reclaimed: " << stats.objects_reclaimed << "\n"
                      << "Last objects: " << stats.last_objects_before << " -> "
                      << stats.last_objects_after << "\n"
                      << "Peak objects at collection: " << stats.peak_objects << "\n"
                      << "Total pause: " << stats.total_pause_us << " us\n"
                      << "Maximum pause: " << stats.max_pause_us << " us\n"
                      << "Average pause: " << std::fixed << std::setprecision(1)
                      << average_pause_us << " us\n"
                      << "Next object threshold: " << stats.next_object_threshold << "\n"
                      << "Live tensor bytes: " << stats.last_tensor_bytes << "\n";
        }

        if (!gc_stats_json_path.empty()) {
            std::ofstream report(utf8_path(gc_stats_json_path), std::ios::binary);
            if (!report) {
                std::cerr << "[Error] Failed to write GC statistics JSON: "
                          << gc_stats_json_path << "\n";
                return false;
            }
            report << "{\n"
                   << "  \"schema\": \"sura.gc_stats.v1\",\n"
                   << "  \"collections\": " << stats.collections << ",\n"
                   << "  \"objects_reclaimed\": " << stats.objects_reclaimed << ",\n"
                   << "  \"last_objects_before\": " << stats.last_objects_before << ",\n"
                   << "  \"last_objects_after\": " << stats.last_objects_after << ",\n"
                   << "  \"peak_objects\": " << stats.peak_objects << ",\n"
                   << "  \"total_pause_us\": " << stats.total_pause_us << ",\n"
                   << "  \"max_pause_us\": " << stats.max_pause_us << ",\n"
                   << "  \"average_pause_us\": " << std::fixed << std::setprecision(3)
                   << average_pause_us << ",\n"
                   << "  \"next_object_threshold\": " << stats.next_object_threshold << ",\n"
                   << "  \"live_tensor_bytes\": " << stats.last_tensor_bytes << "\n"
                   << "}\n";
            if (!report) {
                std::cerr << "[Error] Failed while writing GC statistics JSON: "
                          << gc_stats_json_path << "\n";
                return false;
            }
            std::cout << "[gc] wrote " << gc_stats_json_path << "\n";
        }
        return true;
    };

    try {
        if (release_key.empty() && !release_key_file.empty()) release_key = read_release_secret_file(release_key_file);
        if (release_license.empty() && !release_license_file.empty())
            release_license = read_release_secret_file(release_license_file);
    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
        return 1;
    }

    auto effective_release_key = [&]() {
        if (!release_key.empty()) return release_key;
        const char* env_key = std::getenv("SURA_RELEASE_KEY");
        return env_key ? std::string(env_key) : std::string();
    };

    auto effective_release_license = [&]() {
        if (!release_license.empty()) return release_license;
        const char* env_license = std::getenv("SURA_RELEASE_LICENSE");
        return env_license ? std::string(env_license) : std::string();
    };

    auto ends_with = [](const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() &&
               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (!test_mode && !check_mode && !format_mode && !lint_mode && !filename.empty() && !load_bc && !load_release) {
        if (ends_with(filename, ".sura.bc")) load_bc = true;
        else if (ends_with(filename, ".sura.srp") || ends_with(filename, ".srp")) load_release = true;
    }

    auto allow_release_inspect = []() {
        const char* raw = std::getenv("SURA_ALLOW_RELEASE_INSPECT");
        if (!raw) return false;
        std::string value = raw;
        for (char& ch : value) ch = (char)std::tolower((unsigned char)ch);
        return !value.empty() && value != "0" && value != "false" && value != "no";
    };

    if (load_release && !allow_release_inspect() && (dump_bytecode || trace_mode || debug_protocol)) {
        std::cerr << "[Error] Protected release package inspection is disabled by default. "
                  << "Set SURA_ALLOW_RELEASE_INSPECT=1 only in a trusted owner/debug environment.\n";
        return 1;
    }

    auto is_generated_or_vendor_dir = [](const std::filesystem::path& path) {
        std::string name = path.filename().string();
        return name == ".git" || name == "node_modules" || name == "sura_packages" ||
               name == "dist" || name == "build" || name == ".vscode";
    };

    auto discover_sura_files = [&](const std::filesystem::path& input) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_regular_file(input, ec)) {
            if (input.extension() == ".sura") files.push_back(input);
            return files;
        }
        if (!std::filesystem::is_directory(input, ec)) return files;
        for (std::filesystem::recursive_directory_iterator it(input, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && is_generated_or_vendor_dir(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || it->path().extension() != ".sura") continue;
            files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        return files;
    };

    auto check_sura_file = [&](const std::filesystem::path& path) {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::cerr << "[FAIL] " << path.generic_string() << "\n";
                std::cerr << "[Error] cannot open file\n";
                return 1;
            }
            std::string check_src((std::istreambuf_iterator<char>(in)), {});
            Parser parser;
            parser.set_legacy_command_syntax(!strict_syntax);
            auto parse_diagnostics = parser.diagnose_source(check_src);
            if (!parse_diagnostics.empty()) {
                std::cout << "[FAIL] " << path.generic_string() << "\n";
                for (const auto& diagnostic : parse_diagnostics) {
                    report_error(check_src, diagnostic.line, diagnostic.message);
                }
                return 1;
            }
            auto ast = parser.parse_source(check_src);
            auto static_diagnostics = lint_global_shadowing(static_cast<const SuraBlock*>(ast.get()));
            for (const auto& diagnostic : static_diagnostics) {
                std::cout << "[WARN] " << path.generic_string() << ":" << diagnostic.line
                          << ": " << diagnostic.message << "\n";
            }
            int type_errors = run_typecheck(static_cast<const SuraBlock*>(ast.get()), strict_mode);
            if (type_errors > 0) {
                std::cout << (strict_mode ? "[FAIL] " : "[WARN] ") << path.generic_string()
                          << " (" << type_errors << " type diagnostic(s))\n";
                return strict_mode ? 1 : 0;
            }
            std::cout << "[OK] " << path.generic_string() << "\n";
            return 0;
        } catch (const LexError& e) {
            std::ifstream in(path, std::ios::binary);
            std::string check_src((std::istreambuf_iterator<char>(in)), {});
            std::cout << "[FAIL] " << path.generic_string() << "\n";
            report_error(check_src, e.line, e.what());
            return 1;
        } catch (const ParseError& e) {
            std::ifstream in(path, std::ios::binary);
            std::string check_src((std::istreambuf_iterator<char>(in)), {});
            std::cout << "[FAIL] " << path.generic_string() << "\n";
            report_error(check_src, e.line, e.what());
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << path.generic_string() << "\n";
            std::cerr << "[Error] " << e.what() << "\n";
            return 1;
        }
    };

    auto format_sura_file = [&](const std::filesystem::path& path, bool check_only) {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::cerr << "[FAIL] " << path.generic_string() << "\n";
                std::cerr << "[Error] cannot open file\n";
                return 2;
            }
            std::string original((std::istreambuf_iterator<char>(in)), {});
            std::string formatted = format_sura_text(original);
            if (formatted == original) {
                std::cout << "[OK] " << path.generic_string() << "\n";
                return 0;
            }
            if (check_only) {
                std::cout << "[FAIL] " << path.generic_string() << " is not formatted\n";
                return 1;
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "[FAIL] " << path.generic_string() << "\n";
                std::cerr << "[Error] cannot write file\n";
                return 2;
            }
            out << formatted;
            std::cout << "[FORMAT] " << path.generic_string() << "\n";
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << path.generic_string() << "\n";
            std::cerr << "[Error] " << e.what() << "\n";
            return 2;
        }
    };

    struct LintOutcome {
        int errors = 0;
        int warnings = 0;
    };

    auto lint_sura_file = [&](const std::filesystem::path& path) {
        LintOutcome outcome;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "[lint] cannot open " << path.generic_string() << "\n";
            outcome.errors++;
            return outcome;
        }

        struct RiskRule { const char* name; std::regex pattern; };
        std::vector<RiskRule> risky = {
            {"shell execution", std::regex("\\b(async_cmd|cmd_run(?:_checked)?|task)\\s*\\(|\\b(os\\.run(?:_checked)?|os\\.cmd|async\\.cmd)\\s*\\(")},
            {"network access", std::regex("\\b(http_(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes)|http\\.(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes))\\s*\\(")},
            {"file deletion", std::regex("\\b(file_delete|file_remove_tree|remove_tree)\\s*\\(|\\bfs\\.(delete|remove|remove_tree|delete_tree)\\s*\\(")},
            {"python bridge", std::regex("\\bpython_[A-Za-z_]*\\s*\\(")},
            {"native ffi", std::regex("\\b(ffi_load|ffi_call|plugin_call)\\s*\\(|\\b(ffi|plugin)\\.(load|call|load_manifest|info|unload)\\s*\\(")},
            {"unpolicyed tool call", std::regex("\\btool_call\\s*\\(|\\btool\\.call\\s*\\(")}
        };
        std::unordered_set<std::string> sensitive_header_vars;
        std::unordered_set<std::string> weak_tool_policy_vars;
        std::regex assignment_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+(?:is|=)\\s*(.+)$");
        std::regex sensitive_header_source_re(
            "\\b(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
            "\\bhttp\\.(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
            "\\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key|x-auth-token|x-csrf-token|x-xsrf-token|token|secret|api[-_]?key)\\b",
            std::regex_constants::ECMAScript | std::regex_constants::icase);
        std::regex output_re("^\\s*(print(?:_n)?\\b|log_(debug|info|warn|error)\\s*\\(|log\\.(debug|info|warn|error|event)\\s*\\(|console_(log|print|println|write|write_line|writeln|line|info|debug|warn|warning|error|exception|assert|time_log|time_end|time_stamp|count|count_reset|table|dir|dirxml|trace|group|group_collapsed|profile|profile_end)\\s*\\(|console\\.(log|print|println|write|write_line|writeln|line|info|debug|warn|warning|error|exception|assert|time_log|timeLog|time_end|timeEnd|time_stamp|timeStamp|count|count_reset|countReset|table|dir|dirxml|trace|group|group_collapsed|groupCollapsed|profile|profile_end|profileEnd)\\s*\\()");
        std::regex empty_tool_policy_re("^\\s*\\{\\s*\\}\\s*$");
        std::regex policy_literal_re("^\\s*\\{[^\\n]*\\}\\s*$");
        std::regex http_tool_policy_re("\\btools\\b[^\\n}]*http_(get|request)|http_(get|request)[^\\n}]*\\btools\\b",
                                       std::regex_constants::ECMAScript | std::regex_constants::icase);
        std::regex weak_tool_policy_call_re("\\b(tool_call_policy|tool\\.call_policy)\\s*\\(.*?,\\s*(\\{[^\\n]*\\}|[A-Za-z_][A-Za-z0-9_]*)\\s*\\)");
        std::regex legacy_command_re("^\\s*(print|print_n|print_no_nl|assert|assert_eq|assert_ne|assert_neq|assert_type|assert_len|assert_between|assert_approx|input|exit|clock|type|random)\\s+[^\\s(]");
        auto has_header_redaction = [](const std::string& text) {
            return text.find("headers_redact") != std::string::npos;
        };
        auto has_sensitive_header_source = [&](const std::string& text) {
            return !has_header_redaction(text) && std::regex_search(text, sensitive_header_source_re);
        };
        auto references_sensitive_header_var = [&](const std::string& text) {
            for (const auto& name : sensitive_header_vars) {
                if (std::regex_search(text, std::regex("\\b" + name + "\\b"))) return true;
            }
            return false;
        };
        auto is_weak_tool_policy_text = [&](const std::string& value) {
            if (!std::regex_match(value, policy_literal_re)) return false;
            if (std::regex_match(value, empty_tool_policy_re)) return true;
            if (value.find("url_prefixes") != std::string::npos) return false;
            if (std::regex_search(value, http_tool_policy_re)) return true;
            return false;
        };
        auto is_weak_tool_policy_arg = [&](const std::string& value) {
            if (is_weak_tool_policy_text(value)) return true;
            return weak_tool_policy_vars.count(value) > 0;
        };
        std::regex block_close("^\\s*end\\b");
        std::regex if_open("^if\\b.*\\bthen\\s*$");
        std::regex loop_open("^(while|for|foreach|repeat)\\b.*\\bdo\\s*$");
        std::regex func_open("^func\\b.*\\bdo\\s*$");
        std::regex decl_open("^(class|struct|enum|try)\\b");

        int depth = 0;
        int line_no = 0;
        std::string line;
        while (std::getline(in, line)) {
            ++line_no;
            if (line_no == 1 && line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
            if (std::regex_search(line, block_close)) --depth;
            if (depth < 0) {
                std::cout << "[lint] unmatched end at " << path.generic_string() << ":" << line_no << "\n";
                outcome.errors++;
                depth = 0;
            }
            for (const auto& rule : risky) {
                if (std::regex_search(line, rule.pattern)) {
                    std::cout << "[lint] risky " << rule.name << " at " << path.generic_string() << ":" << line_no << "\n";
                    outcome.warnings++;
                }
            }
            std::smatch assign_match;
            if (std::regex_match(line, assign_match, assignment_re)) {
                std::string name = assign_match[1].str();
                std::string value = assign_match[2].str();
                if (has_header_redaction(value)) {
                    sensitive_header_vars.erase(name);
                } else if (has_sensitive_header_source(value) || references_sensitive_header_var(value)) {
                    sensitive_header_vars.insert(name);
                }
                if (is_weak_tool_policy_text(value)) {
                    weak_tool_policy_vars.insert(name);
                } else {
                    weak_tool_policy_vars.erase(name);
                }
            }
            if (!has_header_redaction(line) &&
                std::regex_search(line, output_re) &&
                (has_sensitive_header_source(line) || references_sensitive_header_var(line))) {
                std::cout << "[lint] risky unredacted sensitive headers at "
                          << path.generic_string() << ":" << line_no
                          << " (use headers_redact before logging)\n";
                outcome.warnings++;
            }
            std::smatch weak_policy_match;
            if (std::regex_search(line, weak_policy_match, weak_tool_policy_call_re) &&
                weak_policy_match.size() >= 3 &&
                is_weak_tool_policy_arg(weak_policy_match[2].str())) {
                std::cout << "[lint] risky weak tool policy at "
                          << path.generic_string() << ":" << line_no
                          << " (restrict tools, url_prefixes, http_methods, and allow_shell)\n";
                outcome.warnings++;
            }
            if (std::regex_search(line, legacy_command_re)) {
                std::cout << "[lint] legacy command syntax at "
                          << path.generic_string() << ":" << line_no
                          << " (use function-call syntax such as print(...))\n";
                outcome.warnings++;
            }
            std::string trim = trim_sura_line(line);
            if (std::regex_search(trim, if_open) ||
                std::regex_search(trim, loop_open) ||
                std::regex_search(trim, func_open) ||
                std::regex_search(trim, decl_open)) {
                ++depth;
            }
        }
        if (depth != 0) {
            std::cout << "[lint] unclosed block depth " << depth << " in " << path.generic_string() << "\n";
            outcome.errors++;
        }
        return outcome;
    };

    if (lint_mode) {
        std::filesystem::path root = filename.empty() ? std::filesystem::current_path() : utf8_path(filename);
        std::vector<std::filesystem::path> files = discover_sura_files(root);
        if (files.empty()) {
            std::cerr << "[Error] no Sura files found under " << root.string() << "\n";
            return 1;
        }
        int files_with_errors = 0;
        int warnings = 0;
        for (const auto& file : files) {
            LintOutcome outcome = lint_sura_file(file);
            if (outcome.errors > 0) ++files_with_errors;
            warnings += outcome.warnings;
        }
        if (files_with_errors == 0) {
            std::cout << "[OK] lint passed";
            if (warnings > 0) std::cout << " (" << warnings << " warning(s))";
            std::cout << "\n";
        }
        std::cout << "Sura lint: " << files.size() - files_with_errors << " passed, "
                  << files_with_errors << " failed, " << warnings << " warning(s)\n";
        return files_with_errors ? 1 : 0;
    }

    if (format_mode) {
        if (filename.empty()) {
            std::cerr << "[Error] " << (format_check_mode ? "--format-check" : "--format") << " requires a file or directory path\n";
            return 1;
        }
        std::filesystem::path root = utf8_path(filename);
        std::vector<std::filesystem::path> files = discover_sura_files(root);
        if (files.empty()) {
            std::cerr << "[Error] no Sura files found under " << root.string() << "\n";
            return 1;
        }
        int unchanged = 0;
        int changed = 0;
        int failed = 0;
        for (const auto& file : files) {
            int code = format_sura_file(file, format_check_mode);
            if (code == 0) ++unchanged;
            else if (code == 1) ++changed;
            else ++failed;
        }
        if (format_check_mode) {
            std::cout << "Sura format check: " << unchanged << " passed, " << changed + failed << " failed\n";
            return (changed + failed) ? 1 : 0;
        }
        std::cout << "Sura format: " << changed << " formatted, " << unchanged << " unchanged, " << failed << " failed\n";
        return failed ? 1 : 0;
    }

    if (check_mode) {
        std::filesystem::path root = filename.empty() ? std::filesystem::current_path() : utf8_path(filename);
        std::vector<std::filesystem::path> files = discover_sura_files(root);
        if (files.empty()) {
            std::cerr << "[Error] no Sura files found under " << root.string() << "\n";
            return 1;
        }
        int passed = 0;
        int failed = 0;
        for (const auto& file : files) {
            int code = check_sura_file(file);
            if (code == 0) ++passed;
            else ++failed;
        }
        std::cout << "Sura check: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }

    auto looks_like_test_file = [](const std::filesystem::path& file) {
        if (file.extension() != ".sura") return false;
        std::string name = file.filename().string();
        return name.rfind("test_", 0) == 0 ||
               (name.size() >= 10 && name.substr(name.size() - 10) == "_test.sura") ||
               (name.size() >= 10 && name.substr(name.size() - 10) == ".test.sura");
    };

    auto discover_test_files = [&](const std::filesystem::path& input) {
        std::vector<std::filesystem::path> tests;
        std::error_code ec;
        if (std::filesystem::is_regular_file(input, ec)) {
            if (input.extension() == ".sura") tests.push_back(input);
            return tests;
        }
        if (!std::filesystem::is_directory(input, ec)) return tests;

        std::filesystem::path tests_dir = input / "tests";
        bool input_is_tests_dir = input.filename() == "tests";
        bool use_tests_dir = input_is_tests_dir || std::filesystem::is_directory(tests_dir, ec);
        std::filesystem::path root = input_is_tests_dir ? input : (use_tests_dir ? tests_dir : input);
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".sura") continue;
            if (use_tests_dir || looks_like_test_file(it->path())) tests.push_back(it->path());
        }
        std::sort(tests.begin(), tests.end());
        return tests;
    };

    struct TestResult {
        std::filesystem::path path;
        std::string status;
        int exit_code = 0;
        long long duration_ms = 0;
        std::string output;
    };

    auto run_test_file = [&](const std::filesystem::path& path, std::string& output, long long& duration_ms) {
        auto start = std::chrono::steady_clock::now();
        std::ostringstream captured;
        std::streambuf* old_out = std::cout.rdbuf(captured.rdbuf());
        std::streambuf* old_err = std::cerr.rdbuf(captured.rdbuf());
        int code = 0;
        std::string test_src;
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot open test file: " + path.string());
            test_src = std::string((std::istreambuf_iterator<char>(in)), {});

            Parser parser;
            parser.set_legacy_command_syntax(!strict_syntax);
            auto ast = parser.parse_source(test_src);
            int type_errors = run_typecheck(static_cast<const SuraBlock*>(ast.get()), strict_mode);
            if (strict_mode && type_errors > 0) {
                std::cerr << "[strict] " << type_errors << " type error(s) found. Execution stopped.\n";
                code = 1;
            } else {
                JitCompiler compiler;
                JitChunk chunk = compiler.compile(ast.get(), path.string());
                JitVM vm;
                if (trace_mode) vm.enable_trace(true);
                if (jit_mode && !trace_mode) vm.enable_jit(true);
                SuraStd::set_script_context(path.string(), {});
                vm.run(chunk);
            }
        } catch (const LexError& e) {
            report_error(test_src, e.line, e.what());
            code = 1;
        } catch (const ParseError& e) {
            report_error(test_src, e.line, e.what());
            code = 1;
        } catch (const JitThrow& e) {
            report_error(test_src, e.line, "[Runtime Error] " + e.message);
            report_stack_trace(e.stack_trace);
            code = 1;
        } catch (const std::exception& e) {
            std::cerr << "[Error] " << e.what() << "\n";
            code = 1;
        }
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        auto end = std::chrono::steady_clock::now();
        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        output = captured.str();
        return code;
    };

    if (test_mode) {
        std::filesystem::path root = filename.empty() ? std::filesystem::current_path() : utf8_path(filename);
        std::vector<std::filesystem::path> tests = discover_test_files(root);
        if (tests.empty()) {
            std::cerr << "[Error] no Sura tests found under " << root.string() << "\n";
            return 1;
        }

        std::filesystem::path display_root = root;
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec)) display_root = root.parent_path();
        std::vector<TestResult> results;
        int passed = 0;
        int failed = 0;
        for (const auto& test : tests) {
            std::string output;
            long long ms = 0;
            int code = run_test_file(test, output, ms);
            std::error_code rel_ec;
            std::filesystem::path display = std::filesystem::relative(test, display_root, rel_ec);
            if (rel_ec) display = test;
            if (code == 0) {
                ++passed;
                std::cout << "[PASS] " << display.generic_string() << " (" << ms << " ms)\n";
                results.push_back({display, "pass", code, ms, output});
            } else {
                ++failed;
                std::cout << "[FAIL] " << display.generic_string() << " (" << ms << " ms)\n";
                if (!output.empty()) std::cout << output;
                results.push_back({display, "fail", code, ms, output});
            }
        }

        if (!test_report_path.empty()) {
            std::ofstream report(utf8_path(test_report_path), std::ios::binary);
            if (!report) {
                std::cerr << "[Error] failed to write test report: " << test_report_path << "\n";
                return 1;
            }
            report << "{\n"
                   << "  \"version\": 1,\n"
                   << "  \"engine\": \"Sura Language\",\n"
                   << "  \"jit\": " << (jit_mode ? "true" : "false") << ",\n"
                   << "  \"passed\": " << passed << ",\n"
                   << "  \"failed\": " << failed << ",\n"
                   << "  \"tests\": [\n";
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                report << "    {\"path\":\"" << json_escape_text(r.path.generic_string())
                       << "\",\"status\":\"" << r.status
                       << "\",\"exitCode\":" << r.exit_code
                       << ",\"durationMs\":" << r.duration_ms
                       << ",\"output\":\"" << json_escape_text(r.output) << "\"}"
                       << (i + 1 == results.size() ? "\n" : ",\n");
            }
            report << "  ]\n}\n";
            std::cout << "[OK] wrote " << test_report_path << "\n";
        }

        std::cout << "Sura tests: " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }

    // ==== 2. REPL Mode ====
    if (repl_mode || filename.empty()) {
        std::cout << "=== Sura Language REPL " << SURA_LANGUAGE_VERSION << " ===\n"
                  << "Type 'exit' or 'quit' to exit.\n";
        if (strict_mode) std::cout << "[STRICT MODE] Type errors will stop execution.\n";
        std::cout << "\n";

        JitVM vm;
        SuraStd::set_script_context("", {});
        if (trace_mode) vm.enable_trace(true);
        if (jit_mode && !trace_mode) vm.enable_jit(true);
        // Persistent global-name table so variable indices stay stable across
        // separate REPL inputs (otherwise `a is 10` then `print b` would both
        // resolve to index 0 → b would see a's value).
        std::vector<std::string> repl_globals;

        while (true) {
            std::cout << "SURA> ";
            std::string line;
            std::getline(std::cin, line);
            if (line == "exit" || line == "quit") break;
            if (line.empty()) continue;

            std::string src = line;

            auto needs_block = [](const std::string& s) {
                // 키워드 뒤에 공백이 없는 경우도 처리 (try_xxx 오탐 방지)
                auto starts_kw = [&](const char* kw) {
                    size_t len = strlen(kw);
                    if (s.size() < len) return false;
                    if (s.substr(0, len) != kw) return false;
                    return s.size() == len || s[len] == ' ' || s[len] == '\n' || s[len] == '\t';
                };
                return starts_kw("if") || starts_kw("while") || starts_kw("repeat") ||
                       starts_kw("for") || starts_kw("func") || starts_kw("class") ||
                       starts_kw("try");
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
                parser.set_legacy_command_syntax(!strict_syntax);
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
                JitChunk chunk = compiler.compile_with_globals(ast.get(), repl_globals);
                repl_globals = chunk.global_names;  // persist for next input

                if (dump_bytecode) JitVM::dump(chunk);
                vm.run(chunk);
            } catch (const LexError& e) {
                report_error(src, e.line, e.what());
            } catch (const ParseError& e) {
                report_error(src, e.line, e.what());
            } catch (const JitThrow& e) {
                report_error(src, e.line, "[Runtime Error] " + e.message);
                report_stack_trace(e.stack_trace);
            } catch (const std::exception& e) {
                std::cerr << "\033[1;31m[Internal Error]\033[0m " << e.what() << "\n";
            }
        }
        if (!emit_gc_stats(vm)) return 1;
        return 0;
    }

    // ==== 3. --load / --load-release: run precompiled artifacts directly ====
    if (load_bc || load_release) {
        if (filename.empty()) {
            std::cerr << "[Error] Specify a bytecode or release package file to load.\n";
            return 1;
        }
        try {
            auto exec_start = std::chrono::high_resolution_clock::now();
            JitChunk chunk = load_release
                ? load_release_package(filename, effective_release_key(), effective_release_license())
                : load_chunk(filename);
            if (dump_bytecode) { std::cout << "========== Bytecode Dump ==========\n"; JitVM::dump(chunk); std::cout << "=================================\n\n"; }
            JitVM vm;
            if (trace_mode) vm.enable_trace(true);
            if (jit_mode && !trace_mode) vm.enable_jit(true);
            SuraStd::set_script_context(filename, script_args);
            vm.run(chunk);
            auto exec_end = std::chrono::high_resolution_clock::now();
            if (!emit_gc_stats(vm)) return 1;
            if (bench_mode) {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count();
                std::cout << "\n=== " << (load_release ? "Release Package" : "Precompiled")
                          << " Execute: " << us / 1000.0 << " ms ===\n";
            }
        } catch (const JitThrow& e) {
            std::cerr << "\n\033[1;31m"
                      << localize_diagnostic("[Runtime Error] " + e.message)
                      << "\033[0m\n";
            report_stack_trace(e.stack_trace);
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "\033[1;31m[Error]\033[0m " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ==== 4. File Execution (with optional benchmark) ====
    std::ifstream f(utf8_path(filename));
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
        parser.set_legacy_command_syntax(!strict_syntax);
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

        if (ast_json_mode) {
            std::string json = sura_ast_json(static_cast<const SuraBlock*>(ast.get()), filename, type_errors);
            if (!output_path.empty()) {
                std::ofstream out(utf8_path(output_path), std::ios::binary | std::ios::trunc);
                if (!out) {
                    std::cerr << "[Error] Cannot write AST JSON: " << output_path << "\n";
                    return 1;
                }
                out << json;
                std::cout << "[AST JSON] " << output_path << "\n";
            } else {
                std::cout << json;
            }
            return 0;
        }

        if (!freestanding_target.empty()) {
            SuraOsCompileResult result =
                sura_compile_uefi_x64(static_cast<const SuraBlock*>(ast.get()));
            std::string out_path = output_path;
            if (out_path.empty()) {
                out_path = filename;
                if (out_path.size() > 5 &&
                    out_path.substr(out_path.size() - 5) == ".sura") {
                    out_path.resize(out_path.size() - 5);
                }
                out_path += ".efi";
            }
            std::ofstream out(utf8_path(out_path),
                              std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "[Error] Cannot write UEFI image: " << out_path << "\n";
                return 1;
            }
            out.write(reinterpret_cast<const char*>(result.image.data()),
                      static_cast<std::streamsize>(result.image.size()));
            if (!out) {
                std::cerr << "[Error] Failed while writing UEFI image: "
                          << out_path << "\n";
                return 1;
            }
            std::cout << "[UEFI] " << out_path << "\n"
                      << "  target: " << result.target << "\n"
                      << "  entry: " << result.entry_function << "\n"
                      << "  machine code: " << result.machine_code_bytes << " bytes\n"
                      << "  image: " << result.image.size() << " bytes\n";
            return 0;
        }

        // [Compile]
        auto compile_start = std::chrono::high_resolution_clock::now();
        JitCompiler compiler;
        JitChunk chunk = compiler.compile(ast.get(), filename);
        auto compile_end = std::chrono::high_resolution_clock::now();

        if (dump_bytecode) {
            std::cout << "========== Bytecode Dump ==========\n";
            JitVM::dump(chunk);
            std::cout << "========== End Dump ==========\n\n";
        }

        // [--compile / --release: save artifact and exit]
        if (compile_only || release_only) {
            if (compile_only && release_only) {
                std::cerr << "[Error] Use either --compile or --release, not both.\n";
                return 1;
            }
            std::string out_path = output_path;
            if (out_path.empty()) {
                out_path = release_only ? filename + ".srp" : filename + ".bc";
                if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".sura") {
                    out_path = filename.substr(0, filename.size() - 5) +
                               (release_only ? ".sura.srp" : ".sura.bc");
                }
            }
            if (release_only) {
                ReleaseMetadata release_metadata;
                release_metadata.release_id = release_id;
                release_metadata.expires = release_expires;
                release_metadata.license = release_license;
                save_release_package(chunk, out_path, effective_release_key(), release_metadata);
                std::cout << "[Release] " << out_path
                          << " (" << chunk.code.size() << " instructions, source stripped)\n";
            } else {
                save_chunk(chunk, out_path);
                std::cout << "[Compiled] " << out_path
                          << " (" << chunk.code.size() << " instructions)\n";
            }
            return 0;
        }

        // [Execute]
        auto exec_start = std::chrono::high_resolution_clock::now();
        JitVM vm;
        Profiler profiler;
        if (profile_mode) vm.set_profiler(&profiler);
        if (trace_mode)   vm.enable_trace(true);
        if (jit_mode && !trace_mode && !debug_protocol) vm.enable_jit(true);
        SuraStd::set_script_context(filename, script_args);
        if (debug_protocol) {
            struct DebugState {
                std::unordered_set<int> breakpoints;
                bool stop_on_entry = false;
                bool step_next = false;
                bool first_line = true;
                int last_line = -1;
                bool break_on_exception = false;
                JitDebugSnapshot last_snapshot;
            } dbg;

            auto parse_bool_env = [](const char* name) {
                const char* raw = std::getenv(name);
                return raw && std::string(raw) != "0" && std::string(raw) != "false";
            };
            auto parse_breakpoints = [&](const char* raw) {
                if (!raw) return;
                std::stringstream ss(raw);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    try {
                        int line = std::stoi(part);
                        if (line > 0) dbg.breakpoints.insert(line);
                    } catch (...) {}
                }
            };
            auto debug_json_escape = [](const std::string& s) {
                std::string out;
                for (char ch : s) {
                    switch (ch) {
                        case '\\': out += "\\\\"; break;
                        case '"': out += "\\\""; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default: out += ch; break;
                    }
                }
                return out;
            };
            std::function<std::string(const std::vector<JitDebugVar>&)> vars_json;
            vars_json = [&](const std::vector<JitDebugVar>& vars) {
                std::ostringstream out;
                out << "[";
                for (size_t i = 0; i < vars.size(); ++i) {
                    if (i) out << ",";
                    out << "{\"name\":\"" << debug_json_escape(vars[i].name)
                        << "\",\"value\":\"" << debug_json_escape(vars[i].value) << "\"";
                    if (!vars[i].children.empty())
                        out << ",\"children\":" << vars_json(vars[i].children);
                    out << "}";
                }
                out << "]";
                return out.str();
            };
            auto frames_json = [&](const std::vector<JitDebugFrame>& frames) {
                std::ostringstream out;
                out << "[";
                for (size_t i = 0; i < frames.size(); ++i) {
                    if (i) out << ",";
                    out << "{\"name\":\"" << debug_json_escape(frames[i].name)
                        << "\",\"line\":" << frames[i].line
                        << ",\"locals\":" << vars_json(frames[i].locals) << "}";
                }
                out << "]";
                return out.str();
            };
            auto send_debug_event = [&](const std::string& reason, const JitDebugSnapshot& snap, const std::string& description = "") {
                std::cerr << "@@SURA_DEBUG@@{\"event\":\"stopped\",\"reason\":\"" << debug_json_escape(reason)
                          << "\",\"line\":" << snap.line
                          << ",\"ip\":" << snap.ip
                          << ",\"function\":\"" << debug_json_escape(snap.function)
                          << "\",\"description\":\"" << debug_json_escape(description)
                          << "\",\"frames\":" << frames_json(snap.frames)
                          << ",\"globals\":" << vars_json(snap.globals)
                          << ",\"locals\":" << vars_json(snap.locals) << "}\n";
                std::cerr.flush();
            };
            auto wait_debug_command = [&]() {
                std::string cmd;
                while (std::getline(std::cin, cmd)) {
                    if (cmd.find("next") != std::string::npos ||
                        cmd.find("stepIn") != std::string::npos ||
                        cmd.find("stepOut") != std::string::npos) {
                        dbg.step_next = true;
                        break;
                    }
                    if (cmd.find("disconnect") != std::string::npos ||
                        cmd.find("terminate") != std::string::npos) {
                        std::exit(0);
                    }
                    if (cmd.find("continue") != std::string::npos || cmd.find("resume") != std::string::npos) {
                        break;
                    }
                }
            };

            parse_breakpoints(std::getenv("SURA_DEBUG_BREAKPOINTS"));
            dbg.stop_on_entry = parse_bool_env("SURA_DEBUG_STOP_ON_ENTRY");
            dbg.break_on_exception = parse_bool_env("SURA_DEBUG_BREAK_ON_EXCEPTION");
            vm.set_debug_hook([&](const JitDebugSnapshot& snap) {
                if (snap.line <= 0 || snap.line == dbg.last_line) return;
                dbg.last_snapshot = snap;
                dbg.last_line = snap.line;
                bool stop = false;
                std::string reason = "breakpoint";
                if (dbg.first_line && dbg.stop_on_entry) {
                    stop = true;
                    reason = "entry";
                } else if (dbg.step_next) {
                    stop = true;
                    reason = "step";
                } else if (dbg.breakpoints.count(snap.line)) {
                    stop = true;
                    reason = "breakpoint";
                }
                dbg.first_line = false;
                if (!stop) return;
                dbg.step_next = false;
                send_debug_event(reason, snap);
                wait_debug_command();
            });
            try {
                vm.run(chunk);
                std::cerr << "@@SURA_DEBUG@@{\"event\":\"terminated\"}\n";
                std::cerr.flush();
            } catch (const JitThrow& e) {
                if (dbg.break_on_exception) {
                    JitDebugSnapshot snap = dbg.last_snapshot;
                    if (snap.line <= 0) snap.line = e.line;
                    send_debug_event("exception", snap, localize_runtime_message(e.message));
                    wait_debug_command();
                }
                report_error(src, e.line, "[Runtime Error] " + e.message);
                report_stack_trace(e.stack_trace);
                std::cerr << "@@SURA_DEBUG@@{\"event\":\"terminated\"}\n";
                std::cerr.flush();
                return 1;
            }
        } else {
            vm.run(chunk);
        }
        auto exec_end = std::chrono::high_resolution_clock::now();
        if (!emit_gc_stats(vm)) return 1;
        if (profile_mode) {
            profiler.print_text_report(chunk);
            if (!profile_json_path.empty()) {
                if (!profiler.write_json(profile_json_path, chunk)) {
                    std::cerr << "[Error] Failed to write profile JSON: " << profile_json_path << "\n";
                    return 1;
                }
                std::cout << "[profile] wrote " << profile_json_path << "\n";
            }
        }
        if (jit_mode && !trace_mode) {
            std::cout << "\n[JIT] " << vm.native_funcs_count()
                      << " function(s), " << vm.native_methods_count()
                      << " method(s) compiled to native "
                      << sura_jit_target_info().architecture << " code, "
                      << vm.native_scalarized_count()
                      << " callable(s) used guarded record scalar replacement, "
                      << vm.native_record_reuse_sites()
                      << " guarded no-alias record reuse site(s), "
                      << vm.generic_record_loop_runs_count()
                      << " generic numeric record loop execution(s)\n";
        }

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
        return 1;
    } catch (const ParseError& e) {
        report_error(src, e.line, e.what());
        return 1;
    } catch (const SuraOsCompileError& e) {
        report_error(src, e.line, std::string("[OS Target Error] ") + e.what());
        return 1;
    } catch (const JitThrow& e) {
        report_error(src, e.line, "[Runtime Error] " + e.message);
        report_stack_trace(e.stack_trace);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\033[1;31m[Internal Error]\033[0m " << e.what() << "\n";
        return 1;
    }

    return 0;
}
