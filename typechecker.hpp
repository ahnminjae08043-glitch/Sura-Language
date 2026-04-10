#pragma once
#include "ast.hpp"
#include "value.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <iostream>








struct TypeError {
    int         line;
    std::string message;
};


struct FuncSig {
    std::string              name;
    std::vector<TypeAnnot>   param_types;
    std::vector<std::string> param_names;
    TypeAnnot                return_type;
};


class TypeEnv {
    std::vector<std::unordered_map<std::string, TypeAnnot>> scopes;
public:
    TypeEnv() { scopes.push_back({}); }

    void push() { scopes.push_back({}); }
    void pop()  { if (scopes.size() > 1) scopes.pop_back(); }

    void set(const std::string& name, const TypeAnnot& ta) {
        scopes.back()[name] = ta;
    }

    
    TypeAnnot get(const std::string& name) const {
        for (int i = (int)scopes.size() - 1; i >= 0; --i) {
            auto it = scopes[i].find(name);
            if (it != scopes[i].end()) return it->second;
        }
        return TypeAnnot{}; 
    }

    bool has(const std::string& name) const {
        for (int i = (int)scopes.size() - 1; i >= 0; --i)
            if (scopes[i].count(name)) return true;
        return false;
    }
};



class TypeChecker {
    TypeEnv                                          env;
    std::unordered_map<std::string, FuncSig>         func_sigs;
    std::vector<TypeError>                           errors;
    TypeAnnot                                        current_return; 
    void error(int line, const std::string& msg) {
        errors.push_back({line, msg});
    }

    
    TypeAnnot infer(const Expr* e) {
        if (!e) return TypeAnnot{};
        switch (e->kind) {
            case NK::NUM_LIT:  { TypeAnnot t; t.present = true; t.kind = SType::NUMBER; return t; }
            case NK::STR_LIT:  { TypeAnnot t; t.present = true; t.kind = SType::STRING; return t; }
            case NK::BOOL_LIT: { TypeAnnot t; t.present = true; t.kind = SType::BOOL;   return t; }
            case NK::NIL_LIT:  { TypeAnnot t; t.present = true; t.kind = SType::NIL;    return t; }
            case NK::ARRAY_LIT:{ TypeAnnot t; t.present = true; t.kind = SType::ARRAY;  return t; }
            case NK::DICT_LIT: { TypeAnnot t; t.present = true; t.kind = SType::DICT;   return t; }
            case NK::STR_INTERP:{ TypeAnnot t; t.present = true; t.kind = SType::STRING; return t; }

            case NK::IDENT: {
                auto* id = static_cast<const Ident*>(e);
                return env.get(id->name);
            }

            case NK::BIN_OP: {
                auto* b = static_cast<const BinOp*>(e);
                TypeAnnot l = infer(b->left.get());
                TypeAnnot r = infer(b->right.get());
                
                if (b->op == "+" || b->op == "-" || b->op == "*" || b->op == "/" || b->op == "%") {
                    if (l.present && l.kind == SType::NUMBER &&
                        r.present && r.kind == SType::NUMBER) {
                        TypeAnnot t; t.present = true; t.kind = SType::NUMBER; return t;
                    }
                    
                    if (b->op == "+" &&
                        l.present && l.kind == SType::STRING &&
                        r.present && r.kind == SType::STRING) {
                        TypeAnnot t; t.present = true; t.kind = SType::STRING; return t;
                    }
                }

        if (b->op == "==" || b->op == "!=" || b->op == "<" || b->op == ">" ||
                    b->op == "<=" || b->op == ">=" || b->op == "and" || b->op == "or") {
                    TypeAnnot t; t.present = true; t.kind = SType::BOOL; return t;
                }
                return TypeAnnot{}; 
            }

            case NK::UNARY_OP: {
                auto* u = static_cast<const UnaryOp*>(e);
                if (u->op == "not") {
                    TypeAnnot t; t.present = true; t.kind = SType::BOOL; return t;
                }
                if (u->op == "-") {
                    TypeAnnot t; t.present = true; t.kind = SType::NUMBER; return t;
                }
                return TypeAnnot{};
            }

            case NK::CALL: {
                auto* c = static_cast<const CallExpr*>(e);
                auto it = func_sigs.find(c->name);
                if (it != func_sigs.end())
                    return it->second.return_type;
                return TypeAnnot{};
            }

            case NK::TERNARY: {
                auto* t = static_cast<const TernaryExpr*>(e);
                TypeAnnot ta = infer(t->then_val.get());
                TypeAnnot ea = infer(t->else_val.get());
                
                if (ta.present && ea.present && ta.kind == ea.kind) return ta;
                return TypeAnnot{};
            }

            default:
                return TypeAnnot{};
        }
    }

    
    bool compatible(const TypeAnnot& expected, const TypeAnnot& actual) {
        if (expected.is_any() || actual.is_any()) return true;
        if (expected.kind == SType::CLASS && actual.kind == SType::CLASS)
            return expected.class_name == actual.class_name;
        return expected.kind == actual.kind;
    }

    
    void check_stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case NK::SuraBlock:
                for (auto& stmt : static_cast<const SuraBlock*>(s)->body)
                    check_stmt(stmt.get());
                break;

            case NK::ASSIGN: {
                auto* a = static_cast<const AssignStmt*>(s);
                TypeAnnot val_type = infer(a->value.get());

                if (a->type_annot.present) {
                    
                    if (!compatible(a->type_annot, val_type)) {
                        error(a->line,
                            "'" + a->name + "' ?�??불일�? '" +
                            stype_name(a->type_annot.kind) + "' ?�언, '" +
                            stype_name(val_type.kind) + " 타입입니다.");
                    }
                    
                    env.set(a->name, a->type_annot);
                } else {
                    
                    env.set(a->name, val_type);
                }
                break;
            }

            case NK::IN_PLACE: {
                auto* ip = static_cast<const InPlaceStmt*>(s);
                TypeAnnot var_type = env.get(ip->name);
                TypeAnnot val_type = infer(ip->value.get());
                
                if (var_type.present && var_type.kind == SType::NUMBER &&
                    val_type.present && val_type.kind == SType::STRING) {
                    error(ip->line,
                        "'" + ip->name + "' ?�자 변?�에 문자?�로 복합 ?�??불�?");
                }
                break;
            }

            case NK::IF: {
                auto* is = static_cast<const IfStmt*>(s);
                check_stmt(is->then_block.get());
                if (is->else_block) check_stmt(is->else_block.get());
                break;
            }

            case NK::WHILE: {
                auto* ws = static_cast<const WhileStmt*>(s);
                check_stmt(ws->body.get());
                break;
            }

            case NK::REPEAT: {
                auto* rs = static_cast<const RepeatStmt*>(s);
                TypeAnnot cnt = infer(rs->count.get());
                if (cnt.present && cnt.kind != SType::NUMBER)
                    error(rs->line, "repeat 횟수는 숫자여야 합니다.");
                check_stmt(rs->body.get());
                break;
            }

            case NK::FOR: {
                auto* fs = static_cast<const ForStmt*>(s);
                env.push();
                TypeAnnot num; num.present = true; num.kind = SType::NUMBER;
                env.set(fs->var, num);
                check_stmt(fs->body.get());
                env.pop();
                break;
            }

            case NK::FOREACH: {
                auto* fe = static_cast<const ForeachStmt*>(s);
                env.push();
                env.set(fe->var, TypeAnnot{}); 
                if (!fe->var2.empty()) env.set(fe->var2, TypeAnnot{});
                check_stmt(fe->body.get());
                env.pop();
                break;
            }

            case NK::FUNC_DEF: {
                auto* fd = static_cast<const FuncDef*>(s);
                FuncSig sig;
                sig.name        = fd->name;
                sig.return_type = fd->return_type;
                sig.param_names = fd->params;
                sig.param_types = fd->param_types;


        while (sig.param_types.size() < fd->params.size())
                while (sig.param_types.size() < fd->params.size())

                func_sigs[fd->name] = sig;

                
                TypeAnnot prev_ret = current_return;

                env.push();
                for (size_t i = 0; i < fd->params.size(); ++i)
                    env.set(fd->params[i], sig.param_types[i]);
                check_stmt(fd->body.get());
                env.pop();

                current_return = prev_ret;
                break;
            }

            case NK::RETURN: {
                auto* rs = static_cast<const ReturnStmt*>(s);
                if (current_return.present && current_return.kind != SType::ANY) {
                    TypeAnnot ret_type = rs->value ? infer(rs->value.get()) : TypeAnnot{SType::NIL, "", true};
                    if (!compatible(current_return, ret_type)) {
                        error(rs->line,
                            std::string("return ?�??불일�? '") +
                            stype_name(current_return.kind) + "' 기�?, '" +
                            stype_name(ret_type.kind) + "' 반환");
                    }
                }
                break;
            }

            case NK::EXPR_STMT: {
                auto* es = static_cast<const ExprStmt*>(s);
                
                if (es->expr && es->expr->kind == NK::CALL) {
                    check_call(static_cast<const CallExpr*>(es->expr.get()));
                }
                break;
            }

            case NK::TRY: {
                auto* ts = static_cast<const TryStmt*>(s);
                check_stmt(ts->try_block.get());
                env.push();
                TypeAnnot str_t; str_t.present = true; str_t.kind = SType::STRING;
                env.set(ts->catch_var, str_t);
                check_stmt(ts->catch_block.get());
                env.pop();
                if (ts->finally_block) check_stmt(ts->finally_block.get());
                break;
            }

            case NK::CLASS_DEF: {
                auto* cd = static_cast<const ClassDef*>(s);
                for (auto& [mname, me] : cd->methods) {
                    env.push();
                    for (auto& p : me.params)
                        env.set(p, TypeAnnot{});
                    if (me.body) check_stmt(me.body);
                    env.pop();
                }
                break;
            }

            default:
                break;
        }
    }

    
    void check_call(const CallExpr* c) {
        auto it = func_sigs.find(c->name);
        if (it == func_sigs.end()) return;
        const FuncSig& sig = it->second;

        size_t n = std::min(c->args.size(), sig.param_types.size());
        for (size_t i = 0; i < n; ++i) {
            const TypeAnnot& expected = sig.param_types[i];
            if (!expected.present || expected.kind == SType::ANY) continue;
            TypeAnnot actual = infer(c->args[i].get());
            if (!compatible(expected, actual)) {
                error(c->line,
                    "?�수 '" + c->name + "' " + std::to_string(i + 1) +
                    "번째 ?�자 ?�??불일�? '" + stype_name(expected.kind) +
                    "' 기�?, '" + stype_name(actual.kind) + "' ?�달");
            }
        }
    }

public:
    
    
    int check(const SuraBlock* program) {
        if (!program) return 0;
        for (auto& stmt : program->body)
            check_stmt(stmt.get());
        return (int)errors.size();
    }

    
    void print_errors(bool as_warning = false) const {
        const char* RED    = "\033[1;31m";
        const char* YELLOW = "\033[1;33m";
        const char* RESET  = "\033[0m";
        for (auto& e : errors) {
            if (as_warning)
                std::cerr << YELLOW << "[?�라 ?�??경고] " << e.line << "�? " << e.message << RESET << "\n";
            else
                std::cerr << RED    << "[?�라 ?�???�류] " << e.line << "�? " << e.message << RESET << "\n";
        }
    }

    const std::vector<TypeError>& get_errors() const { return errors; }
};
