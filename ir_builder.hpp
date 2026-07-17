#pragma once
#include "ir.hpp"
#include "ast.hpp"
#include "parser.hpp"
#include <stdexcept>

// ================================================================
//  IRBuilder — compiles AST → IRChunk
//  Same semantics as JitCompiler, but emits IR (virtual regs +
//  label-based control flow) instead of raw JitChunk bytecode.
// ================================================================

struct IREnv {
    IREnv* enclosing = nullptr;
    std::unordered_map<std::string, VReg> locals;
    std::vector<std::string> debug_local_names;
    std::vector<UpvalueDesc> upvalues;
    VReg next_reg = 0;
    VReg max_regs = 0;

    IREnv(IREnv* enc) : enclosing(enc) {}

    VReg alloc_reg() {
        VReg r = next_reg++;
        if (next_reg > max_regs) max_regs = next_reg;
        return r;
    }

    int resolve_local(const std::string& n) const {
        auto it = locals.find(n);
        return it != locals.end() ? (int)it->second : -1;
    }

    void define_local(const std::string& name, VReg reg) {
        locals[name] = reg;
        if (debug_local_names.size() <= (size_t)reg) debug_local_names.resize((size_t)reg + 1);
        debug_local_names[reg] = name;
    }

    VReg alloc_named_local(const std::string& name) {
        VReg reg = alloc_reg();
        define_local(name, reg);
        return reg;
    }

    int add_upvalue(uint16_t index, bool is_local) {
        for (size_t i = 0; i < upvalues.size(); i++)
            if (upvalues[i].index == (int)index && upvalues[i].is_local == is_local)
                return (int)i;
        upvalues.push_back({is_local, (int)index});
        return (int)upvalues.size() - 1;
    }

    int resolve_upvalue(const std::string& n) {
        if (!enclosing) return -1;
        int local = enclosing->resolve_local(n);
        if (local != -1) return add_upvalue((uint16_t)local, true);
        int up = enclosing->resolve_upvalue(n);
        if (up != -1) return add_upvalue((uint16_t)up, false);
        return -1;
    }
};

class IRBuilder {
    IRChunk  ir;
    IREnv*   env = nullptr;

    struct ScopeGuard {
        IREnv* e; VReg saved; std::unordered_map<std::string, VReg> saved_locals;
        std::vector<std::string> saved_debug_local_names;
        ScopeGuard(IREnv* env)
            : e(env), saved(env->next_reg), saved_locals(env->locals),
              saved_debug_local_names(env->debug_local_names) {}
        ~ScopeGuard() {
            e->next_reg = saved;
            e->locals = saved_locals;
            e->debug_local_names = saved_debug_local_names;
        }
    };

    struct LoopCtx {
        std::vector<IRLabel> break_labels;
        IRLabel continue_label = LABEL_NONE;
        IRLabel exit_label     = LABEL_NONE;
    };
    std::vector<LoopCtx> loop_stack;

    VReg alloc() { return env->alloc_reg(); }

    // ── variable load/store ──────────────────────────────────────
    void load_var(const std::string& name, VReg dst, int ln) {
        int local = env->resolve_local(name);
        if (local != -1) {
            ir.emit(IROp::MOV, dst, (VReg)local, VREG_NONE, VREG_NONE, 0,0,-1, ln);
        } else {
            int up = env->resolve_upvalue(name);
            if (up != -1) {
                ir.emit(IROp::UVLOAD, dst, VREG_NONE, VREG_NONE, VREG_NONE, up,0,-1, ln);
            } else {
                int g = ir.add_global(name);
                ir.emit(IROp::GLOAD, dst, VREG_NONE, VREG_NONE, VREG_NONE, g,0,-1, ln);
            }
        }
    }

    void store_var(const std::string& name, VReg val, int ln) {
        int local = env->resolve_local(name);
        if (local != -1) {
            ir.emit(IROp::MOV, (VReg)local, val, VREG_NONE, VREG_NONE, 0,0,-1, ln);
        } else {
            int up = env->resolve_upvalue(name);
            if (up != -1) {
                ir.emit(IROp::UVSTORE, VREG_NONE, val, VREG_NONE, VREG_NONE, up,0,-1, ln);
            } else if (env->enclosing == nullptr) {
                int g = ir.add_global(name);
                ir.emit(IROp::GSTORE, VREG_NONE, val, VREG_NONE, VREG_NONE, g,0,-1, ln);
            } else {
                VReg r = alloc();
                env->define_local(name, r);
                ir.emit(IROp::MOV, r, val, VREG_NONE, VREG_NONE, 0,0,-1, ln);
            }
        }
    }

    // ── interpolated string ──────────────────────────────────────
    void compile_interp_str(const std::string& s, int ln, VReg dst) {
        if (s.find('{') == std::string::npos) {
            ir.emit(IROp::FCONST, dst, VREG_NONE, VREG_NONE, VREG_NONE,
                    ir.add_const(Value(s)), 0, -1, ln);
            return;
        }

        struct Seg { bool is_expr; std::string text; };
        std::vector<Seg> segs;
        auto is_blank_interp = [](const std::string& text) {
            for (char ch : text) {
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') return false;
            }
            return true;
        };
        auto interp_expr_source = [](const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    char next = text[i + 1];
                    if (next == '"' || next == '\'' || next == '\\') {
                        out += next;
                        ++i;
                        continue;
                    }
                }
                out += text[i];
            }
            return out;
        };
        size_t i = 0; std::string cur;
        while (i < s.size()) {
            if (s[i] == '{') {
                if (i+1 < s.size() && s[i+1] == '{') { cur += '{'; i += 2; continue; }
                size_t end = s.find('}', i+1);
                if (end == std::string::npos) { cur += s[i++]; continue; }
                std::string expr = s.substr(i+1, end-i-1);
                if (is_blank_interp(expr)) {
                    cur += "{";
                    cur += expr;
                    cur += "}";
                    i = end + 1;
                    continue;
                }
                if (!cur.empty()) { segs.push_back({false, cur}); cur.clear(); }
                segs.push_back({true, expr});
                i = end + 1;
            } else cur += s[i++];
        }
        if (!cur.empty()) segs.push_back({false, cur});
        if (segs.empty()) {
            ir.emit(IROp::FCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE,
                    ir.add_const(Value("")), 0,-1, ln);
            return;
        }

        VReg saved_next = env->next_reg;
        VReg tmp = VREG_NONE;
        if (segs.size() > 1) tmp = alloc();
        for (size_t j = 0; j < segs.size(); ++j) {
            VReg out = (j == 0) ? dst : tmp;
            if (segs[j].is_expr) {
                std::string expr = interp_expr_source(segs[j].text);
                size_t colon = expr.find(':');
                if (colon != std::string::npos) {
                    std::string value_expr = expr.substr(0, colon);
                    std::string fmt = expr.substr(colon+1);
                    try {
                        Parser sub;
                        auto sub_expr = sub.parse_expr_from_source(value_expr);
                        if (sub_expr) compile_expr(sub_expr.get(), out);
                        else load_var(value_expr, out, ln);
                    } catch (...) {
                        load_var(value_expr, out, ln);
                    }
                    VReg fmt_r = alloc();
                    ir.emit(IROp::FCONST, fmt_r, VREG_NONE,VREG_NONE,VREG_NONE,
                            ir.add_const(Value(fmt)), 0,-1, ln);
                    ir.emit(IROp::FMTVAL, out, out, fmt_r, VREG_NONE, 0,0,-1, ln);
                    env->next_reg--; // free fmt_r
                } else {
                    try {
                        Parser sub;
                        auto sub_expr = sub.parse_expr_from_source(expr);
                        if (sub_expr) compile_expr(sub_expr.get(), out);
                        else load_var(expr, out, ln);
                    } catch (...) {
                        load_var(expr, out, ln);
                    }
                }
            } else {
                ir.emit(IROp::FCONST, out, VREG_NONE,VREG_NONE,VREG_NONE,
                        ir.add_const(Value(segs[j].text)), 0,-1, ln);
            }
            if (j > 0)
                ir.emit(IROp::ADD, dst, dst, tmp, VREG_NONE, 0,0,-1, ln);
        }
        env->next_reg = saved_next;
    }

    // ── binary op helper ─────────────────────────────────────────
    static IROp bin_op(const std::string& op) {
        if (op == "+")  return IROp::ADD;
        if (op == "-")  return IROp::SUB;
        if (op == "*")  return IROp::MUL;
        if (op == "/")  return IROp::DIV;
        if (op == "%")  return IROp::MOD;
        if (op == "==") return IROp::EQ;
        if (op == "!=") return IROp::NEQ;
        if (op == "<")  return IROp::LT;
        if (op == "<=") return IROp::LTE;
        if (op == ">")  return IROp::GT;
        if (op == ">=") return IROp::GTE;
        if (op == "&")  return IROp::BAND;
        if (op == "|")  return IROp::BOR;
        if (op == "^")  return IROp::BXOR;
        if (op == "<<") return IROp::SHL;
        if (op == ">>") return IROp::SHR;
        return IROp::ADD;
    }

    // ── compile_expr ─────────────────────────────────────────────
    void compile_expr(const Expr* e, VReg dst) {
        if (!e) { ir.emit(IROp::NCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, 0); return; }
        int ln = e->line;

        switch (e->kind) {
        case NK::NUM_LIT:
            ir.emit(IROp::FCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE,
                    ir.add_const(Value(static_cast<const NumLit*>(e)->value)), 0,-1, ln);
            break;
        case NK::STR_LIT: {
            const auto& s = static_cast<const StrLit*>(e)->value;
            if (s.find('{') != std::string::npos) compile_interp_str(s, ln, dst);
            else ir.emit(IROp::FCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE,
                         ir.add_const(Value(s)), 0,-1, ln);
            break;
        }
        case NK::BOOL_LIT:
            ir.emit(IROp::BCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE,
                    static_cast<const BoolLit*>(e)->value ? 1 : 0, 0,-1, ln);
            break;
        case NK::NIL_LIT:
            ir.emit(IROp::NCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, ln);
            break;
        case NK::IDENT:
            load_var(static_cast<const Ident*>(e)->name, dst, ln);
            break;
        case NK::ARRAY_LIT: {
            auto* al = static_cast<const ArrayLit*>(e);
            VReg start = env->next_reg;
            for (auto& el : al->elements) { VReg r = alloc(); compile_expr(el.get(), r); }
            ir.emit(IROp::MKARR, dst, start, VREG_NONE,VREG_NONE, (int)al->elements.size(),0,-1, ln);
            env->next_reg = start;
            break;
        }
        case NK::DICT_LIT: {
            auto* dl = static_cast<const DictLit*>(e);
            VReg start = env->next_reg;
            for (auto& k : dl->key_order) {
                VReg kr = alloc();
                ir.emit(IROp::FCONST, kr, VREG_NONE,VREG_NONE,VREG_NONE,
                        ir.add_const(Value(k)), 0,-1, ln);
                VReg vr = alloc();
                compile_expr(dl->pairs.at(k).get(), vr);
            }
            ir.emit(IROp::MKDICT, dst, start, VREG_NONE,VREG_NONE, (int)dl->key_order.size(),0,-1, ln);
            env->next_reg = start;
            break;
        }
        case NK::INDEX: {
            auto* ix = static_cast<const IndexExpr*>(e);
            VReg obj = alloc(); compile_expr(ix->obj.get(), obj);
            VReg key = alloc(); compile_expr(ix->key.get(), key);
            ir.emit(IROp::IGET, dst, obj, key, VREG_NONE, 0,0,-1, ln);
            env->next_reg -= 2;
            break;
        }
        case NK::DOT_ACCESS: {
            auto* da = static_cast<const DotAccess*>(e);
            VReg obj = alloc(); compile_expr(da->obj.get(), obj);
            ir.emit(IROp::DGET, dst, obj, VREG_NONE,VREG_NONE, 0,0, ir.add_string(da->prop), ln);
            env->next_reg--;
            break;
        }
        case NK::UNARY_OP: {
            auto* u = static_cast<const UnaryOp*>(e);
            VReg r = alloc(); compile_expr(u->operand.get(), r);
            if (u->op == "-")                   ir.emit(IROp::NEG,  dst, r, VREG_NONE,VREG_NONE, 0,0,-1, ln);
            else if (u->op == "not" || u->op == "!") ir.emit(IROp::LNOT, dst, r, VREG_NONE,VREG_NONE, 0,0,-1, ln);
            else if (u->op == "~")               ir.emit(IROp::BNOT, dst, r, VREG_NONE,VREG_NONE, 0,0,-1, ln);
            env->next_reg--;
            break;
        }
        case NK::BIN_OP: {
            auto* b = static_cast<const BinOp*>(e);
            VReg lr = alloc(); compile_expr(b->left.get(), lr);

            if (b->op == "and") {
                IRLabel lbl_false = ir.alloc_label(), lbl_end = ir.alloc_label();
                ir.emit(IROp::JF, VREG_NONE, lr, VREG_NONE,VREG_NONE, (int)lbl_false,0,-1, ln);
                compile_expr(b->right.get(), dst);
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_end,0,-1, ln);
                ir.emit_label(lbl_false, ln);
                ir.emit(IROp::MOV, dst, lr, VREG_NONE,VREG_NONE, 0,0,-1, ln);
                ir.emit_label(lbl_end, ln);
                env->next_reg--;
                break;
            }
            if (b->op == "or") {
                IRLabel lbl_true = ir.alloc_label(), lbl_end = ir.alloc_label();
                ir.emit(IROp::JT, VREG_NONE, lr, VREG_NONE,VREG_NONE, (int)lbl_true,0,-1, ln);
                compile_expr(b->right.get(), dst);
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_end,0,-1, ln);
                ir.emit_label(lbl_true, ln);
                ir.emit(IROp::MOV, dst, lr, VREG_NONE,VREG_NONE, 0,0,-1, ln);
                ir.emit_label(lbl_end, ln);
                env->next_reg--;
                break;
            }
            if (b->op == "in") {
                VReg rr = alloc(); compile_expr(b->right.get(), rr);
                ir.emit(IROp::IN, dst, lr, rr, VREG_NONE, 0,0,-1, ln);
                env->next_reg -= 2;
                break;
            }

            VReg rr = alloc(); compile_expr(b->right.get(), rr);
            ir.emit(bin_op(b->op), dst, lr, rr, VREG_NONE, 0,0,-1, ln);
            env->next_reg -= 2;
            break;
        }
        case NK::CALL: {
            auto* ce = static_cast<const CallExpr*>(e);
            VReg start = env->next_reg;
            VReg fn_r  = alloc(); load_var(ce->name, fn_r, ln);
            for (auto& a : ce->args) { VReg r = alloc(); compile_expr(a.get(), r); }
            ir.emit(IROp::CALL, dst, fn_r, fn_r+1, VREG_NONE,
                    (int)ce->args.size(), 0, ir.add_string(ce->name), ln);
            env->next_reg = start;
            break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(e);
            VReg start = env->next_reg;
            VReg obj_r = alloc(); compile_expr(mc->obj.get(), obj_r);
            for (auto& a : mc->args) { VReg r = alloc(); compile_expr(a.get(), r); }
            ir.emit(IROp::MCALL, dst, obj_r, VREG_NONE, VREG_NONE,
                    (int)mc->args.size(), 0, ir.add_string(mc->method), ln);
            env->next_reg = start;
            break;
        }
        case NK::SUPER_CALL: {
            auto* sc = static_cast<const SuperCallExpr*>(e);
            VReg start = env->next_reg;
            for (auto& a : sc->args) { VReg r = alloc(); compile_expr(a.get(), r); }
            ir.emit(IROp::SCALL, dst, start, VREG_NONE, VREG_NONE,
                    (int)sc->args.size(), 0, ir.add_string(sc->method), ln);
            env->next_reg = start;
            break;
        }
        case NK::NEW_EXPR: {
            auto* ne = static_cast<const NewExpr*>(e);
            VReg start = env->next_reg;
            for (auto& a : ne->args) { VReg r = alloc(); compile_expr(a.get(), r); }
            ir.emit(IROp::NCALL, dst, start, VREG_NONE, VREG_NONE,
                    (int)ne->args.size(), 0, ir.add_string(ne->class_name), ln);
            env->next_reg = start;
            break;
        }
        case NK::TERNARY: {
            auto* te = static_cast<const TernaryExpr*>(e);
            VReg cr = alloc(); compile_expr(te->cond.get(), cr);
            IRLabel lbl_else = ir.alloc_label(), lbl_end = ir.alloc_label();
            ir.emit(IROp::JF, VREG_NONE, cr, VREG_NONE,VREG_NONE, (int)lbl_else,0,-1, ln);
            compile_expr(te->then_val.get(), dst);
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_end,0,-1, ln);
            ir.emit_label(lbl_else, ln);
            compile_expr(te->else_val.get(), dst);
            ir.emit_label(lbl_end, ln);
            env->next_reg--;
            break;
        }
        case NK::FUNC_EXPR: {
            auto* fe = static_cast<const FuncExpr*>(e);
            compile_func_body("", fe->params, fe->body.get(), dst, ln);
            break;
        }
        case NK::STR_INTERP: {
            auto* si = static_cast<const StrInterp*>(e);
            if (si->parts.empty()) {
                ir.emit(IROp::FCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE,
                        ir.add_const(Value("")), 0,-1, ln);
                break;
            }
            compile_expr(si->parts[0].get(), dst);
            VReg tmp = alloc();
            for (size_t i = 1; i < si->parts.size(); ++i) {
                compile_expr(si->parts[i].get(), tmp);
                ir.emit(IROp::ADD, dst, dst, tmp, VREG_NONE, 0,0,-1, ln);
            }
            env->next_reg--;
            break;
        }
        default:
            ir.emit(IROp::NCONST, dst, VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, 0);
            break;
        }
    }

    // ── compile a function/lambda body into func_table ───────────
    void compile_func_body(const std::string& name,
                           const std::vector<std::string>& params,
                           const SuraBlock* body, VReg dst, int ln) {
        JitFuncInfo fi;
        fi.name = name; fi.params = params;
        for (size_t i = 0; i < params.size(); ++i) fi.defaults.push_back(Value::nil());

        IRLabel skip_lbl = ir.alloc_label();
        ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)skip_lbl,0,-1, ln);
        fi.entry_ip = ir.current_addr();

        IREnv new_env(env);
        IREnv* prev = env;
        env = &new_env;
        for (auto& p : params) env->alloc_named_local(p);

        compile_block(body);
        ir.emit(IROp::RETVOID, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, ln);

        fi.end_ip = ir.current_addr();
        fi.max_regs = (uint16_t)env->max_regs;
        fi.local_names = env->debug_local_names;
        fi.upvalues = env->upvalues;
        env = prev;

        ir.emit_label(skip_lbl, ln);
        int idx = (int)ir.func_table.size();
        ir.func_table.push_back(fi);
        ir.emit(IROp::MKFN, dst, VREG_NONE,VREG_NONE,VREG_NONE, idx,0,-1, ln);
    }

    // ── compile_stmt ─────────────────────────────────────────────
    void compile_stmt(const Stmt* s) {
        if (!s) return;
        int ln = s->line;

        switch (s->kind) {
        case NK::SuraBlock: {
            ScopeGuard g(env);
            for (auto& st : static_cast<const SuraBlock*>(s)->body) compile_stmt(st.get());
            break;
        }
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(s);
            VReg v = alloc(); compile_expr(a->value.get(), v);
            store_var(a->name, v, ln);
            env->next_reg--;
            break;
        }
        case NK::IN_PLACE: {
            auto* ip = static_cast<const InPlaceStmt*>(s);
            VReg cur = alloc(); load_var(ip->name, cur, ln);
            VReg v   = alloc(); compile_expr(ip->value.get(), v);
            ir.emit(bin_op(ip->op), cur, cur, v, VREG_NONE, 0,0,-1, ln);
            store_var(ip->name, cur, ln);
            env->next_reg -= 2;
            break;
        }
        case NK::DOT_ASSIGN: {
            auto* da = static_cast<const DotAssignStmt*>(s);
            VReg obj = alloc(); load_var(da->obj_name, obj, ln);
            VReg val = alloc(); compile_expr(da->value.get(), val);
            ir.emit(IROp::DSET, VREG_NONE, obj, val, VREG_NONE, 0,0, ir.add_string(da->prop), ln);
            env->next_reg -= 2;
            break;
        }
        case NK::INDEX_ASSIGN: {
            auto* ia = static_cast<const IndexAssignStmt*>(s);
            VReg obj = alloc(); load_var(ia->name, obj, ln);
            VReg key = alloc(); compile_expr(ia->key.get(), key);
            VReg val = alloc(); compile_expr(ia->value.get(), val);
            ir.emit(IROp::ISET, VREG_NONE, obj, key, val, 0,0,-1, ln);
            env->next_reg -= 3;
            break;
        }
        case NK::IF: {
            auto* is = static_cast<const IfStmt*>(s);
            VReg cr = alloc(); compile_expr(is->cond.get(), cr);
            env->next_reg--;
            IRLabel lbl_else = ir.alloc_label(), lbl_end = ir.alloc_label();
            ir.emit(IROp::JF, VREG_NONE, cr, VREG_NONE,VREG_NONE, (int)lbl_else,0,-1, ln);
            compile_stmt(is->then_block.get());
            if (is->else_block) {
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_end,0,-1, ln);
                ir.emit_label(lbl_else, ln);
                compile_stmt(is->else_block.get());
                ir.emit_label(lbl_end, ln);
            } else {
                ir.emit_label(lbl_else, ln);
            }
            break;
        }
        case NK::WHILE: {
            auto* ws = static_cast<const WhileStmt*>(s);
            IRLabel lbl_top  = ir.alloc_label();
            IRLabel lbl_exit = ir.alloc_label();
            loop_stack.push_back({{}, lbl_top, lbl_exit});

            ir.emit_label(lbl_top, ln);
            VReg cr = alloc(); compile_expr(ws->cond.get(), cr);
            env->next_reg--;
            ir.emit(IROp::JF, VREG_NONE, cr, VREG_NONE,VREG_NONE, (int)lbl_exit,0,-1, ln);
            compile_stmt(ws->body.get());
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_top,0,-1, ln);
            ir.emit_label(lbl_exit, ln);

            // patch break labels
            for (auto bl : loop_stack.back().break_labels)
                ; // labels already emitted as LABEL instructions — no patch needed
            loop_stack.pop_back();
            break;
        }
        case NK::RETURN: {
            auto* rs = static_cast<const ReturnStmt*>(s);
            if (rs->value) {
                VReg r = alloc(); compile_expr(rs->value.get(), r);
                ir.emit(IROp::RET, VREG_NONE, r, VREG_NONE,VREG_NONE, 0,0,-1, ln);
                env->next_reg--;
            } else {
                ir.emit(IROp::RETVOID, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, ln);
            }
            break;
        }
        case NK::BREAK: {
            if (!loop_stack.empty()) {
                IRLabel exit_lbl = loop_stack.back().exit_label;
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)exit_lbl,0,-1, ln);
            }
            break;
        }
        case NK::CONTINUE: {
            if (!loop_stack.empty()) {
                IRLabel cont_lbl = loop_stack.back().continue_label;
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)cont_lbl,0,-1, ln);
            }
            break;
        }
        case NK::EXPR_STMT: {
            auto* es = static_cast<const ExprStmt*>(s);
            VReg d = alloc(); compile_expr(es->expr.get(), d);
            env->next_reg--;
            break;
        }
        case NK::CMD: {
            auto* c = static_cast<const CmdStmt*>(s);
            VReg start = env->next_reg;
            for (auto& a : c->args) { VReg r = alloc(); compile_expr(a.get(), r); }

            if (c->cmd == "print") {
                ir.emit(IROp::PRINT, VREG_NONE, start, VREG_NONE,VREG_NONE,
                        (int)c->args.size(), 1, -1, ln);
            } else if (c->cmd == "print_n" || c->cmd == "print_no_nl") {
                ir.emit(IROp::PRINT, VREG_NONE, start, VREG_NONE,VREG_NONE,
                        (int)c->args.size(), 0, -1, ln);
            } else {
                // build ident_info string like jit_compiler does
                std::string ident_info;
                for (size_t i = 0; i < c->args.size(); ++i) {
                    if (i > 0) ident_info += '\x01';
                    ident_info += (c->args[i]->kind == NK::IDENT)
                        ? static_cast<const Ident*>(c->args[i].get())->name
                        : "\x02";
                }
                VReg d = alloc();
                ir.emit(IROp::BUILTIN, d, start, VREG_NONE, VREG_NONE,
                        (int)c->args.size(), 0,
                        ir.add_string(c->cmd + std::string(1, '\0') + ident_info), ln);
                env->next_reg--;
            }
            env->next_reg = start;
            break;
        }
        case NK::FUNC_DEF: {
            auto* fd = static_cast<const FuncDef*>(s);
            VReg tmp = alloc();
            compile_func_body(fd->name, fd->params, fd->body.get(), tmp, ln);
            store_var(fd->name, tmp, ln);
            env->next_reg--;
            break;
        }
        case NK::CLASS_DEF: {
            auto* cd = static_cast<const ClassDef*>(s);
            JitClassInfo ci; ci.name = cd->name; ci.parent = cd->parent;
            int fidx = 0;
            // This legacy IR path is not used by main.cpp. Keep its layout
            // ordering aligned with the active compiler, but executable
            // defaults remain intentionally implemented only in JitCompiler.
            for (const auto& k : cd->field_order) {
                ci.field_indices[k] = fidx++;
                ci.field_defaults.push_back(Value::nil());
            }

            IRLabel skip_lbl = ir.alloc_label();
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)skip_lbl,0,-1, ln);

            for (auto& [mname, me] : cd->methods) {
                JitMethodInfo mi; mi.name = mname; mi.params = me.params;
                for (size_t i = 0; i < me.params.size(); ++i) mi.defaults.push_back(Value::nil());
                mi.entry_ip = ir.current_addr();

                IREnv new_env(env);
                IREnv* prev = env; env = &new_env;
                env->alloc_named_local("self");
                for (auto& p : mi.params) env->alloc_named_local(p);

                compile_block(me.body);
                ir.emit(IROp::RETVOID, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, ln);
                mi.end_ip  = ir.current_addr();
                mi.max_regs = (uint16_t)env->max_regs;
                mi.local_names = env->debug_local_names;
                env = prev;
                ci.methods[mname] = mi;
            }
            ir.emit_label(skip_lbl, ln);
            int cidx = (int)ir.class_table.size();
            ir.class_table.push_back(ci);
            int gidx = ir.add_global(cd->name);
            ir.emit(IROp::DEFCLS, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, cidx, gidx,-1, ln);
            break;
        }
        case NK::REPEAT: {
            auto* rs = static_cast<const RepeatStmt*>(s);
            ScopeGuard g(env);
            VReg cnt = alloc(); compile_expr(rs->count.get(), cnt);
            VReg itr = alloc();
            ir.emit(IROp::FCONST, itr, VREG_NONE,VREG_NONE,VREG_NONE,
                    ir.add_const(Value(0.0)), 0,-1, ln);

            IRLabel lbl_top  = ir.alloc_label();
            IRLabel lbl_exit = ir.alloc_label();
            loop_stack.push_back({{}, lbl_top, lbl_exit});

            ir.emit_label(lbl_top, ln);
            VReg cr = alloc();
            ir.emit(IROp::LT, cr, itr, cnt, VREG_NONE, 0,0,-1, ln);
            ir.emit(IROp::JF, VREG_NONE, cr, VREG_NONE,VREG_NONE, (int)lbl_exit,0,-1, ln);
            env->next_reg--;

            compile_stmt(rs->body.get());

            VReg one = alloc();
            ir.emit(IROp::FCONST, one, VREG_NONE,VREG_NONE,VREG_NONE,
                    ir.add_const(Value(1.0)), 0,-1, ln);
            ir.emit(IROp::ADD, itr, itr, one, VREG_NONE, 0,0,-1, ln);
            env->next_reg--;
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_top,0,-1, ln);
            ir.emit_label(lbl_exit, ln);
            loop_stack.pop_back();
            break;
        }
        case NK::FOR: {
            auto* fs = static_cast<const ForStmt*>(s);
            ScopeGuard g(env);
            VReg i_r   = alloc(); env->define_local(fs->var, i_r);
            VReg to_r  = alloc();
            VReg stp_r = alloc();
            compile_expr(fs->from.get(), i_r);
            compile_expr(fs->to.get(),   to_r);
            if (fs->step) compile_expr(fs->step.get(), stp_r);
            else ir.emit(IROp::FCONST, stp_r, VREG_NONE,VREG_NONE,VREG_NONE,
                         ir.add_const(Value(1.0)), 0,-1, ln);

            IRLabel lbl_top  = ir.alloc_label();
            IRLabel lbl_exit = ir.alloc_label();
            loop_stack.push_back({{}, lbl_top, lbl_exit});

            ir.emit_label(lbl_top, ln);
            VReg cr = alloc();
            ir.emit(IROp::LTE, cr, i_r, to_r, VREG_NONE, 0,0,-1, ln);
            ir.emit(IROp::JF, VREG_NONE, cr, VREG_NONE,VREG_NONE, (int)lbl_exit,0,-1, ln);
            env->next_reg--;

            compile_stmt(fs->body.get());

            ir.emit(IROp::ADD, i_r, i_r, stp_r, VREG_NONE, 0,0,-1, ln);
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_top,0,-1, ln);
            ir.emit_label(lbl_exit, ln);
            loop_stack.pop_back();
            break;
        }
        case NK::FOREACH: {
            auto* fe = static_cast<const ForeachStmt*>(s);
            ScopeGuard g(env);
            VReg coll = alloc(); compile_expr(fe->collection.get(), coll);

            bool has_kv = !fe->var2.empty();
            IRLabel lbl_top  = ir.alloc_label();
            IRLabel lbl_exit = ir.alloc_label();
            loop_stack.push_back({{}, lbl_top, lbl_exit});

            if (has_kv) {
                VReg keys = alloc();
                ir.emit(IROp::DKEYS, keys, coll, VREG_NONE,VREG_NONE, 0,0,-1, ln);
                VReg itr  = alloc();
                ir.emit(IROp::FCONST, itr, VREG_NONE,VREG_NONE,VREG_NONE,
                        ir.add_const(Value(0.0)), 0,-1, ln);
                VReg k_r = alloc(); env->define_local(fe->var, k_r);
                VReg v_r = alloc(); env->define_local(fe->var2, v_r);

                ir.emit_label(lbl_top, ln);
                ir.emit(IROp::FOREACH, k_r, itr, keys, VREG_NONE, (int)lbl_exit,0,-1, ln);
                ir.emit(IROp::IGET, v_r, coll, k_r, VREG_NONE, 0,0,-1, ln);
                compile_stmt(fe->body.get());
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_top,0,-1, ln);
            } else {
                VReg itr  = alloc();
                ir.emit(IROp::FCONST, itr, VREG_NONE,VREG_NONE,VREG_NONE,
                        ir.add_const(Value(0.0)), 0,-1, ln);
                VReg el = alloc(); env->define_local(fe->var, el);

                ir.emit_label(lbl_top, ln);
                ir.emit(IROp::FOREACH, el, itr, coll, VREG_NONE, (int)lbl_exit,0,-1, ln);
                compile_stmt(fe->body.get());
                ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_top,0,-1, ln);
            }
            ir.emit_label(lbl_exit, ln);
            loop_stack.pop_back();
            break;
        }
        case NK::THROW: {
            auto* ts = static_cast<const ThrowStmt*>(s);
            VReg r = alloc(); compile_expr(ts->msg.get(), r);
            ir.emit(IROp::THROW, VREG_NONE, r, VREG_NONE,VREG_NONE, 0,0,-1, ln);
            env->next_reg--;
            break;
        }
        case NK::TRY: {
            auto* ts = static_cast<const TryStmt*>(s);
            ScopeGuard catch_scope(env);
            VReg cv = alloc(); env->define_local(ts->catch_var, cv);

            IRLabel lbl_catch = ir.alloc_label(), lbl_after = ir.alloc_label();
            ir.emit(IROp::TRYBEGIN, cv, VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_catch,0,-1, ln);
            compile_stmt(ts->try_block.get());
            ir.emit(IROp::TRYEND, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, ln);
            ir.emit(IROp::JMP, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, (int)lbl_after,0,-1, ln);
            ir.emit_label(lbl_catch, ln);
            if (ts->catch_block) compile_stmt(ts->catch_block.get());
            ir.emit_label(lbl_after, ln);
            if (ts->finally_block) compile_stmt(ts->finally_block.get());
            break;
        }
        case NK::USE:
            ir.emit(IROp::USLIB, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE,
                    0,0, ir.add_string(static_cast<const UseStmt*>(s)->lib), ln);
            break;
        default: break;
        }
    }

    void compile_block(const SuraBlock* b) {
        if (!b) return;
        for (auto& st : b->body) compile_stmt(st.get());
    }

public:
    IRChunk build(const SuraBlock* program) {
        ir = IRChunk{};
        loop_stack.clear();
        IREnv root(nullptr);
        env = &root;
        compile_block(program);
        ir.emit(IROp::HALT, VREG_NONE,VREG_NONE,VREG_NONE,VREG_NONE, 0,0,-1, 0);
        return std::move(ir);
    }
};
