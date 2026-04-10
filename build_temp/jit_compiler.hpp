#pragma once
#include "jit_op.hpp"
#include "ast.hpp"

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?��??�터 기반 JIT Compiler
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
struct CompilerEnv {
    CompilerEnv* enclosing = nullptr;
    std::unordered_map<std::string, uint16_t> locals;
    std::vector<UpvalueDesc> upvalues;
    uint16_t next_reg = 0;
    uint16_t max_regs = 0;
    
    CompilerEnv(CompilerEnv* enc) : enclosing(enc) {}
    
    uint16_t alloc_reg() {
        if (next_reg >= 65535) throw std::runtime_error("?�수/블록 ??가???��??�터 초과 (최�? 65535�?");
        uint16_t r = next_reg++;
        if (next_reg > max_regs) max_regs = next_reg;
        return r;
    }

    int resolve_local(const std::string& name) {
        if (locals.count(name)) return locals[name];
        return -1;
    }
    
    int add_upvalue(uint16_t index, bool is_local) {
        for (size_t i = 0; i < upvalues.size(); i++) {
            if (upvalues[i].index == (int)index && upvalues[i].is_local == is_local) return (int)i;
        }
        upvalues.push_back({is_local, (int)index});
        return (int)upvalues.size() - 1;
    }

    int resolve_upvalue(const std::string& name) {
        if (enclosing == nullptr) return -1;
        int local = enclosing->resolve_local(name);
        if (local != -1) return add_upvalue(local, true);
        int upvalue = enclosing->resolve_upvalue(name);
        if (upvalue != -1) return add_upvalue(upvalue, false);
        return -1;
    }
};

class JitCompiler {
    JitChunk chunk;
    CompilerEnv* current_env = nullptr;

    uint16_t alloc_reg() { return current_env->alloc_reg(); }

    struct ScopeGuard {
        CompilerEnv* env;
        uint16_t saved_reg;
        std::unordered_map<std::string, uint16_t> saved_locals;
        ScopeGuard(CompilerEnv* e) : env(e), saved_reg(e->next_reg), saved_locals(e->locals) {}
        ~ScopeGuard() { env->next_reg = saved_reg; env->locals = saved_locals; }
    };

    // ?�?� 루프 컨텍?�트 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    struct LoopCtx {
        std::vector<size_t> break_patches;
        size_t              continue_target;
    };
    std::vector<LoopCtx> loop_stack;

    // ?�?� 변???�별 보조 ?�수 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    void emit_load_var(const std::string& name, uint16_t target, int ln) {
        int local = current_env->resolve_local(name);
        if (local != -1) {
            chunk.emit(JitOp::MOVE, target, local, 0, 0, -1, ln);
        } else {
            int upval = current_env->resolve_upvalue(name);
            if (upval != -1) {
                chunk.emit(JitOp::LOAD_UPVAL, target, 0, 0, upval, -1, ln);
            } else {
                int glob = chunk.add_global(name);
                chunk.emit(JitOp::LOAD_GLOBAL, target, 0, 0, glob, -1, ln);
            }
        }
    }

    uint16_t emit_store_var(const std::string& name, uint16_t val_reg, int ln) {
        int local = current_env->resolve_local(name);
        if (local != -1) {
            chunk.emit(JitOp::MOVE, local, val_reg, 0, 0, -1, ln);
            return local;
        } else {
            int upval = current_env->resolve_upvalue(name);
            if (upval != -1) {
                chunk.emit(JitOp::STORE_UPVAL, val_reg, 0, 0, upval, -1, ln);
                return -1; 
            } else {
                if (current_env->enclosing == nullptr) {
                    int glob = chunk.add_global(name);
                    chunk.emit(JitOp::STORE_GLOBAL, val_reg, 0, 0, glob, -1, ln);
                    return -1;
                } else {
                    int new_local = alloc_reg();
                    current_env->locals[name] = new_local;
                    chunk.emit(JitOp::MOVE, new_local, val_reg, 0, 0, -1, ln);
                    return new_local;
                }
            }
        }
    }

    // ?�?� 보조 ?�수 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    void compile_interpolated_string(const std::string& s, int line, uint16_t target) {
        if (s.find('{') == std::string::npos) {
            chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value(s)), -1, line);
            return;
        }

        struct Seg { bool is_expr; std::string text; };
        std::vector<Seg> segs;
        size_t i = 0; std::string cur;
        while (i < s.size()) {
            if (s[i] == '{') {
                if (i + 1 < s.size() && s[i + 1] == '{') { cur += '{'; i += 2; continue; }
                size_t end = s.find('}', i + 1);
                if (end == std::string::npos) { cur += s[i++]; continue; }
                if (!cur.empty()) { segs.push_back({false, cur}); cur.clear(); }
                segs.push_back({true, s.substr(i + 1, end - i - 1)});
                i = end + 1;
            } else cur += s[i++];
        }
        if (!cur.empty()) segs.push_back({false, cur});

        if (segs.empty()) {
            chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value("")), -1, line);
            return;
        }

        uint16_t tmp_reg = alloc_reg();
        uint16_t res_reg = target;

        for (size_t j = 0; j < segs.size(); ++j) {
            uint16_t out_reg = (j == 0) ? res_reg : tmp_reg;
            
            if (segs[j].is_expr) {
                emit_load_var(segs[j].text, out_reg, line);
            } else {
                chunk.emit(JitOp::LOAD_CONST, out_reg, 0, 0, chunk.add_const(Value(segs[j].text)), -1, line);
            }

            if (j > 0) {
                chunk.emit(JitOp::ADD, res_reg, res_reg, tmp_reg, 0, -1, line);
            }
        }
    }

public:
    void compile_expr(const Expr* e, uint16_t target) {
        if (!e) { chunk.emit(JitOp::LOAD_NIL, target); return; }
        int ln = e->line;

        switch (e->kind) {
        case NK::NUM_LIT:
            chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value(static_cast<const NumLit*>(e)->value)), -1, ln);
            break;
        case NK::STR_LIT: {
            const auto& s = static_cast<const StrLit*>(e)->value;
            if (s.find('{') != std::string::npos) compile_interpolated_string(s, ln, target);
            else chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value(s)), -1, ln);
            break;
        }
        case NK::BOOL_LIT:
            chunk.emit(JitOp::LOAD_BOOL, target, 0, 0, static_cast<const BoolLit*>(e)->value ? 1 : 0, -1, ln);
            break;
        case NK::NIL_LIT:
            chunk.emit(JitOp::LOAD_NIL, target, 0, 0, 0, -1, ln);
            break;
        case NK::IDENT: {
            emit_load_var(static_cast<const Ident*>(e)->name, target, ln);
            break;
        }
        case NK::ARRAY_LIT: {
            auto* al = static_cast<const ArrayLit*>(e);
            uint16_t start_reg = current_env->next_reg; // ?�재 ?�당???��??�터????(배열 ?�소 ?�시 ?�??
            for (auto& el : al->elements) {
                uint16_t r = alloc_reg();
                compile_expr(el.get(), r);
            }
            chunk.emit(JitOp::MAKE_ARRAY, target, start_reg, 0, (int)al->elements.size(), -1, ln);
            current_env->next_reg = start_reg; // ?�시 ?��??�터 ?�제
            break;
        }
        case NK::DICT_LIT: {
            auto* dl = static_cast<const DictLit*>(e);
            uint16_t start_reg = current_env->next_reg;
            for (auto& k : dl->key_order) {
                uint16_t k_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_CONST, k_reg, 0, 0, chunk.add_const(Value(k)), -1, ln);
                uint16_t v_reg = alloc_reg();
                compile_expr(dl->pairs.at(k).get(), v_reg);
            }
            chunk.emit(JitOp::MAKE_DICT, target, start_reg, 0, (int)dl->key_order.size(), -1, ln);
            current_env->next_reg = start_reg;
            break;
        }
        case NK::INDEX: {
            auto* idx = static_cast<const IndexExpr*>(e);
            uint16_t obj_reg = alloc_reg();
            uint16_t key_reg = alloc_reg();
            compile_expr(idx->obj.get(), obj_reg);
            compile_expr(idx->key.get(), key_reg);
            chunk.emit(JitOp::INDEX_GET, target, obj_reg, key_reg, 0, -1, ln);
            current_env->next_reg -= 2;
            break;
        }
        case NK::DOT_ACCESS: {
            auto* da = static_cast<const DotAccess*>(e);
            uint16_t obj_reg = alloc_reg();
            compile_expr(da->obj.get(), obj_reg);
            chunk.emit(JitOp::DOT_GET, target, obj_reg, 0, 0, chunk.add_string(da->prop), ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::UNARY_OP: {
            auto* u = static_cast<const UnaryOp*>(e);
            uint16_t op_reg = alloc_reg();
            compile_expr(u->operand.get(), op_reg);
            if (u->op == "-") chunk.emit(JitOp::NEG, target, op_reg, 0, 0, -1, ln);
            else if (u->op == "not" || u->op == "!") chunk.emit(JitOp::LOGICAL_NOT, target, op_reg, 0, 0, -1, ln);
            else if (u->op == "~") chunk.emit(JitOp::BIT_NOT, target, op_reg, 0, 0, -1, ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::BIN_OP: {
            auto* b = static_cast<const BinOp*>(e);
            uint16_t l_reg = alloc_reg();
            compile_expr(b->left.get(), l_reg);

            if (b->op == "and") {
                size_t skip = chunk.emit(JitOp::JUMP_IF_FALSE, l_reg, 0, 0, 0, -1, ln);
                compile_expr(b->right.get(), target);
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(skip, (int)chunk.current_addr());
                chunk.emit(JitOp::MOVE, target, l_reg, 0, 0, -1, ln);
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
                current_env->next_reg -= 1;
                break;
            } else if (b->op == "or") {
                size_t skip = chunk.emit(JitOp::JUMP_IF_TRUE, l_reg, 0, 0, 0, -1, ln);
                compile_expr(b->right.get(), target);
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(skip, (int)chunk.current_addr());
                chunk.emit(JitOp::MOVE, target, l_reg, 0, 0, -1, ln);
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
                current_env->next_reg -= 1;
                break;
            } else if (b->op == "in") {
                uint16_t r_reg = alloc_reg();
                compile_expr(b->right.get(), r_reg);
                chunk.emit(JitOp::OP_IN, target, l_reg, r_reg, 0, -1, ln);
                current_env->next_reg -= 2;
                break;
            }

            uint16_t r_reg = alloc_reg();
            compile_expr(b->right.get(), r_reg);

            JitOp op = JitOp::NOP;
            if (b->op == "+") op = JitOp::ADD;
            else if (b->op == "-") op = JitOp::SUB;
            else if (b->op == "*") op = JitOp::MUL;
            else if (b->op == "/") op = JitOp::DIV;
            else if (b->op == "%") op = JitOp::MOD;
            else if (b->op == "==") op = JitOp::CMP_EQ;
            else if (b->op == "!=") op = JitOp::CMP_NEQ;
            else if (b->op == ">") op = JitOp::CMP_GT;
            else if (b->op == "<") op = JitOp::CMP_LT;
            else if (b->op == ">=") op = JitOp::CMP_GTE;
            else if (b->op == "<=") op = JitOp::CMP_LTE;
            else if (b->op == "&") op = JitOp::BIT_AND;
            else if (b->op == "|") op = JitOp::BIT_OR;
            else if (b->op == "^") op = JitOp::BIT_XOR;
            else if (b->op == "<<") op = JitOp::LSHIFT;
            else if (b->op == ">>") op = JitOp::RSHIFT;
            
            chunk.emit(op, target, l_reg, r_reg, 0, -1, ln);
            current_env->next_reg -= 2;
            break;
        }
        case NK::CALL: {
            auto* ce = static_cast<const CallExpr*>(e);
            uint16_t args_start = current_env->next_reg;
            
            uint16_t fn_reg = alloc_reg();
            emit_load_var(ce->name, fn_reg, ln);
            
            for (auto& a : ce->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            
            chunk.emit(JitOp::CALL_FUNC, target, fn_reg, fn_reg + 1, (int)ce->args.size(), -1, ln);
            current_env->next_reg = args_start; 
            break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(e);
            uint16_t obj_reg = alloc_reg();
            compile_expr(mc->obj.get(), obj_reg);
            for (auto& a : mc->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            chunk.emit(JitOp::METHOD_CALL, target, obj_reg, 0, (int)mc->args.size(), chunk.add_string(mc->method), ln);
            current_env->next_reg = obj_reg; 
            break;
        }
        case NK::SUPER_CALL: {
            auto* sc = static_cast<const SuperCallExpr*>(e);
            uint16_t args_start = current_env->next_reg;
            for (auto& a : sc->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            chunk.emit(JitOp::SUPER_CALL, target, args_start, 0, (int)sc->args.size(), chunk.add_string(sc->method), ln);
            current_env->next_reg = args_start;
            break;
        }
        case NK::NEW_EXPR: {
            auto* ne = static_cast<const NewExpr*>(e);
            uint16_t args_start = current_env->next_reg;
            for (auto& a : ne->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            chunk.emit(JitOp::NEW_INSTANCE, target, args_start, 0, (int)ne->args.size(), chunk.add_string(ne->class_name), ln);
            current_env->next_reg = args_start;
            break;
        }
        case NK::TERNARY: {
            auto* te = static_cast<const TernaryExpr*>(e);
            uint16_t cond_reg = alloc_reg();
            compile_expr(te->cond.get(), cond_reg);
            size_t skip_else = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            compile_expr(te->then_val.get(), target);
            size_t jump_end = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            chunk.patch_jump(skip_else, (int)chunk.current_addr());
            compile_expr(te->else_val.get(), target);
            chunk.patch_jump(jump_end, (int)chunk.current_addr());
            current_env->next_reg -= 1;
            break;
        }
        case NK::FUNC_EXPR: {
            auto* fe = static_cast<const FuncExpr*>(e);
            JitFuncInfo fi; fi.name = ""; fi.params = fe->params;
            for (size_t i = 0; i < fe->params.size(); ++i) fi.defaults.push_back(Value::nil());
            size_t skip_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            fi.entry_ip = chunk.current_addr();
            
            CompilerEnv new_env(current_env);
            CompilerEnv* prev_env = current_env;
            current_env = &new_env;
            
            for (auto& p : fi.params) current_env->locals[p] = alloc_reg();
            
            compile_block(fe->body.get());
            chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
            
            fi.end_ip = chunk.current_addr();
            fi.max_regs = current_env->max_regs;
            fi.upvalues = current_env->upvalues;

            current_env = prev_env;
            
            chunk.patch_jump(skip_jump, (int)chunk.current_addr());
            int idx = (int)chunk.func_table.size();
            chunk.func_table.push_back(fi);
            chunk.emit(JitOp::MAKE_LAMBDA, target, 0, 0, idx, -1, ln);
            break;
        }
        case NK::STR_INTERP: {
            auto* si = static_cast<const StrInterp*>(e);
            if (si->parts.empty()) {
                chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value("")), -1, ln);
                break;
            }
            compile_expr(si->parts[0].get(), target);
            uint16_t tmp = alloc_reg();
            for (size_t i = 1; i < si->parts.size(); ++i) {
                compile_expr(si->parts[i].get(), tmp);
                chunk.emit(JitOp::ADD, target, target, tmp, 0, -1, ln);
            }
            current_env->next_reg -= 1;
            break;
        }
        default:
            chunk.emit(JitOp::LOAD_NIL, target, 0, 0, 0, -1, ln);
            break;
        }
    }

    void compile_stmt(const Stmt* s) {
        if (!s) return;
        int ln = s->line;

        switch (s->kind) {
        case NK::SuraBlock: {
            ScopeGuard g(current_env);
            for (auto& stmt : static_cast<const SuraBlock*>(s)->body) compile_stmt(stmt.get());
            break;
        }
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(s);
            uint16_t val_reg = alloc_reg();
            compile_expr(a->value.get(), val_reg);
            emit_store_var(a->name, val_reg, ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::IN_PLACE: {
            auto* ip = static_cast<const InPlaceStmt*>(s);
            uint16_t res_reg = alloc_reg();
            emit_load_var(ip->name, res_reg, ln);
            
            uint16_t val_reg = alloc_reg();
            compile_expr(ip->value.get(), val_reg);
            
            JitOp op = JitOp::NOP;
            if (ip->op == "+") op = JitOp::ADD;
            else if (ip->op == "-") op = JitOp::SUB;
            else if (ip->op == "*") op = JitOp::MUL;
            else if (ip->op == "/") op = JitOp::DIV;
            else if (ip->op == "%") op = JitOp::MOD;
            
            chunk.emit(op, res_reg, res_reg, val_reg, 0, -1, ln);
            emit_store_var(ip->name, res_reg, ln);
            
            current_env->next_reg -= 2;
            break;
        }
        case NK::DOT_ASSIGN: {
            auto* da = static_cast<const DotAssignStmt*>(s);
            uint16_t obj_reg = alloc_reg();
            uint16_t val_reg = alloc_reg();
            
            emit_load_var(da->obj_name, obj_reg, ln);
            
            compile_expr(da->value.get(), val_reg);
            chunk.emit(JitOp::DOT_SET, obj_reg, val_reg, 0, 0, chunk.add_string(da->prop), ln);
            current_env->next_reg -= 2;
            break;
        }
        case NK::INDEX_ASSIGN: {
            auto* ia = static_cast<const IndexAssignStmt*>(s);
            uint16_t obj_reg = alloc_reg();
            uint16_t key_reg = alloc_reg();
            uint16_t val_reg = alloc_reg();
            
            emit_load_var(ia->name, obj_reg, ln);
            
            compile_expr(ia->key.get(), key_reg);
            compile_expr(ia->value.get(), val_reg);
            
            chunk.emit(JitOp::INDEX_SET, obj_reg, key_reg, val_reg, 0, -1, ln);
            current_env->next_reg -= 3;
            break;
        }
        case NK::IF: {
            auto* is = static_cast<const IfStmt*>(s);
            uint16_t cond_reg = alloc_reg();
            compile_expr(is->cond.get(), cond_reg);
            size_t else_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            current_env->next_reg -= 1;
            
            compile_stmt(is->then_block.get()); // SuraBlock 처리 ?�함??
            if (is->else_block) {
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(else_jump, (int)chunk.current_addr());
                compile_stmt(is->else_block.get());
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
            } else {
                chunk.patch_jump(else_jump, (int)chunk.current_addr());
            }
            break;
        }
        case NK::WHILE: {
            auto* ws = static_cast<const WhileStmt*>(s);
            loop_stack.push_back({});
            size_t loop_start = chunk.current_addr();
            loop_stack.back().continue_target = loop_start;
            
            uint16_t cond_reg = alloc_reg();
            compile_expr(ws->cond.get(), cond_reg);
            size_t exit_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            current_env->next_reg -= 1;
            
            compile_stmt(ws->body.get());
            chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_start, -1, ln);
            chunk.patch_jump(exit_jump, (int)chunk.current_addr());
            
            for (auto addr : loop_stack.back().break_patches) chunk.patch_jump(addr, (int)chunk.current_addr());
            loop_stack.pop_back();
            break;
        }
        case NK::RETURN: {
            auto* rs = static_cast<const ReturnStmt*>(s);
            if (rs->value) {
                uint16_t res_reg = alloc_reg();
                compile_expr(rs->value.get(), res_reg);
                chunk.emit(JitOp::RETURN_VAL, res_reg, 0, 0, 0, -1, ln);
                current_env->next_reg -= 1;
            } else {
                chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
            }
            break;
        }
        case NK::BREAK: {
            size_t addr = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            if (!loop_stack.empty()) loop_stack.back().break_patches.push_back(addr);
            break;
        }
        case NK::CONTINUE: {
            if (!loop_stack.empty()) {
                chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_stack.back().continue_target, -1, ln);
            }
            break;
        }
        case NK::EXPR_STMT: {
            auto* es = static_cast<const ExprStmt*>(s);
            uint16_t dev_null = alloc_reg();
            compile_expr(es->expr.get(), dev_null);
            current_env->next_reg -= 1;
            break;
        }
        case NK::CMD: {
            auto* c = static_cast<const CmdStmt*>(s);
            uint16_t args_start = current_env->next_reg;
            for (auto& a : c->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            
            if (c->cmd == "print") chunk.emit(JitOp::PRINT, args_start, 0, 0, (int)c->args.size(), -1, ln);
            else if (c->cmd == "print_n" || c->cmd == "print_no_nl") chunk.emit(JitOp::PRINT_NO_NL, args_start, 0, 0, (int)c->args.size(), -1, ln);
            else {
                std::string ident_info;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i > 0) ident_info += '\x01';
                    ident_info += (c->args[i]->kind == NK::IDENT) ? static_cast<const Ident*>(c->args[i].get())->name : "\x02";
                }
                uint16_t dest = alloc_reg(); // ?�장 명령?�도 결과�?반환?????�음
                chunk.emit(JitOp::CALL_BUILTIN, dest, args_start, 0, (int)c->args.size(), chunk.add_string(c->cmd + std::string(1, '\0') + ident_info), ln);
                // 결과??dest???�어지�?무시?�거???�요?�면 ?�용??보통 CMD??무시??
                current_env->next_reg -= 1;
            }
            current_env->next_reg = args_start; // ?�자???�시 ?��??�터 즉시 반환 
            break;
        }
        case NK::CLASS_DEF: {
            auto* cd = static_cast<const ClassDef*>(s);
            JitClassInfo ci; ci.name = cd->name; ci.parent = cd->parent;
            int fidx = 0;
            for(auto& [k,v] : cd->field_defaults) {
                ci.field_indices[k] = fidx++;
                ci.field_defaults.push_back(Value::nil());
            }

            size_t skip = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            for (auto& [mname, me] : cd->methods) {
                JitMethodInfo mi; mi.name = mname; mi.params = me.params;
                for (size_t i=0; i<me.params.size(); ++i) mi.defaults.push_back(Value::nil());
                mi.entry_ip = chunk.current_addr();
                
                CompilerEnv new_env(current_env);
                CompilerEnv* prev_env = current_env;
                current_env = &new_env;
                
                current_env->locals["self"] = alloc_reg();
                for (auto& p : mi.params) current_env->locals[p] = alloc_reg();
                
                compile_block(me.body);
                chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
                mi.end_ip = chunk.current_addr();
                mi.max_regs = current_env->max_regs;
                
                current_env = prev_env;
                ci.methods[mname] = mi;
            }
            chunk.patch_jump(skip, (int)chunk.current_addr());
            int idx = (int)chunk.class_table.size();
            chunk.class_table.push_back(ci);
            
            int glob = chunk.add_global(cd->name);
            chunk.emit(JitOp::DEF_CLASS, 0, 0, 0, idx, glob, ln);
            break;
        }
        case NK::FUNC_DEF: {
            auto* fd = static_cast<const FuncDef*>(s);
            JitFuncInfo fi; fi.name = fd->name; fi.params = fd->params;
            for(size_t i=0; i<fd->params.size(); ++i) fi.defaults.push_back(Value::nil());
            size_t skip = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            fi.entry_ip = chunk.current_addr();
            
            CompilerEnv new_env(current_env);
            CompilerEnv* prev_env = current_env;
            current_env = &new_env;
            for (auto& p : fi.params) current_env->locals[p] = alloc_reg();
            
            compile_block(fd->body.get());
            chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
            fi.end_ip = chunk.current_addr();
            fi.max_regs = current_env->max_regs;
            fi.upvalues = current_env->upvalues;
            
            current_env = prev_env;
            chunk.patch_jump(skip, (int)chunk.current_addr());
            int idx = (int)chunk.func_table.size();
            chunk.func_table.push_back(fi);
            
            uint16_t tmp = alloc_reg();
            chunk.emit(JitOp::MAKE_LAMBDA, tmp, 0, 0, idx, -1, ln);
            emit_store_var(fd->name, tmp, ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::USE:
            chunk.emit(JitOp::USE_LIB, 0, 0, 0, 0, chunk.add_string(static_cast<const UseStmt*>(s)->lib), ln);
            break;
        default: break;
        }
    }

    void compile_block(const SuraBlock* SuraBlock) {
        if (!SuraBlock) return;
        for (auto& stmt : SuraBlock->body) compile_stmt(stmt.get());
    }

    JitChunk compile(const SuraBlock* program) {
        chunk = JitChunk();
        loop_stack.clear();
        
        CompilerEnv root_env(nullptr);
        current_env = &root_env;
        
        compile_block(program);
        chunk.emit(JitOp::HALT, 0, 0, 0, 0, -1, 0);
        chunk.max_regs = current_env->max_regs;
        return chunk;
    }
};
