#pragma once
// Legacy Sura compiler compatibility layer.
// The production register-bytecode compiler and VM are exposed by jit.hpp.
#include "ast.hpp"
#include "value.hpp"
#include <vector>
#include <unordered_map>
#include <string>

// Legacy stack-bytecode instructions retained for source compatibility.
enum class OpCode {
    PUSH_CONST, PUSH_NIL, POP,
    LOAD_VAR, STORE_VAR, LOAD_GLOBAL, STORE_GLOBAL,
    ADD, SUB, MUL, DIV, MOD, NEG,
    EQ, NEQ, LT, LTE, GT, GTE,
    AND, OR, NOT,
    JUMP, JUMP_IF_FALSE, CALL, RETURN, HALT,
    MAKE_ARRAY, ARRAY_GET, ARRAY_SET,
    MAKE_DICT, DICT_GET, DICT_SET,
    PRINT,
};

struct Instruction {
    OpCode op;
    int operand;
    std::string str_operand;
    Instruction(OpCode o, int opd = 0, const std::string& str = "")
        : op(o), operand(opd), str_operand(str) {}
};

struct BytecodeChunk {
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::unordered_map<std::string, int> global_vars;
};

// Legacy compiler. New code should use JitCompiler and JitVM from jit.hpp.
class Compiler {
    BytecodeChunk chunk;
    int next_const_index = 0;

    int add_constant(const Value& val) {
        chunk.constants.push_back(val);
        return next_const_index++;
    }

    int get_var_index(const std::string& name) {
        if (chunk.global_vars.count(name) == 0) {
            int idx = (int)chunk.global_vars.size();
            chunk.global_vars[name] = idx;
        }
        return chunk.global_vars[name];
    }

public:
    BytecodeChunk compile(const SuraBlock* ast) {
        chunk.code.clear();
        chunk.constants.clear();
        chunk.global_vars.clear();
        next_const_index = 0;

        compile_block(ast);
        chunk.code.push_back(Instruction(OpCode::HALT));
        return chunk;
    }

    void compile_block(const SuraBlock* SuraBlock) {
        if (!SuraBlock) return;
        for (auto& stmt : SuraBlock->body)
            compile_stmt(stmt.get());
    }

    void compile_stmt(const Stmt* stmt) {
        if (!stmt) return;
        switch (stmt->kind) {
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(stmt);
            compile_expr(a->value.get());
            chunk.code.push_back(Instruction(OpCode::STORE_VAR, 0, a->name));
            break;
        }
        case NK::EXPR_STMT: {
            auto* es = static_cast<const ExprStmt*>(stmt);
            compile_expr(es->expr.get());
            chunk.code.push_back(Instruction(OpCode::PRINT));
            break;
        }
        case NK::SuraBlock:
            compile_block(static_cast<const SuraBlock*>(stmt));
            break;
        default: break;
        }
    }

    void compile_expr(const Expr* expr) {
        if (!expr) return;
        switch (expr->kind) {
        case NK::NUM_LIT: {
            int idx = add_constant(Value(static_cast<const NumLit*>(expr)->value));
            chunk.code.push_back(Instruction(OpCode::PUSH_CONST, idx));
            break;
        }
        case NK::STR_LIT: {
            int idx = add_constant(Value(static_cast<const StrLit*>(expr)->value));
            chunk.code.push_back(Instruction(OpCode::PUSH_CONST, idx));
            break;
        }
        case NK::BOOL_LIT: {
            int idx = add_constant(Value(static_cast<const BoolLit*>(expr)->value));
            chunk.code.push_back(Instruction(OpCode::PUSH_CONST, idx));
            break;
        }
        case NK::NIL_LIT:
            chunk.code.push_back(Instruction(OpCode::PUSH_NIL));
            break;
        case NK::IDENT: {
            auto* id = static_cast<const Ident*>(expr);
            chunk.code.push_back(Instruction(OpCode::LOAD_VAR, 0, id->name));
            break;
        }
        case NK::BIN_OP: {
            auto* bo = static_cast<const BinOp*>(expr);
            compile_expr(bo->left.get());
            compile_expr(bo->right.get());
            if      (bo->op == "+")  chunk.code.push_back(Instruction(OpCode::ADD));
            else if (bo->op == "-")  chunk.code.push_back(Instruction(OpCode::SUB));
            else if (bo->op == "*")  chunk.code.push_back(Instruction(OpCode::MUL));
            else if (bo->op == "/")  chunk.code.push_back(Instruction(OpCode::DIV));
            else if (bo->op == "==") chunk.code.push_back(Instruction(OpCode::EQ));
            break;
        }
        default:
            chunk.code.push_back(Instruction(OpCode::PUSH_NIL));
            break;
        }
    }
};
