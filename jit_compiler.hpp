#pragma once
#include "jit_op.hpp"
#include "ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include <fstream>
#include <filesystem>
#include <unordered_set>

// ===============================================================
// Register-based JIT Compiler
// ===============================================================
struct CompilerEnv {
    CompilerEnv* enclosing = nullptr;
    std::unordered_map<std::string, uint16_t> locals;
    std::unordered_set<std::string> global_decls;
    std::vector<std::string> debug_local_names;
    std::vector<UpvalueDesc> upvalues;
    uint16_t next_reg = 0;
    uint16_t max_regs = 0;
    
    CompilerEnv(CompilerEnv* enc) : enclosing(enc) {}
    
    uint16_t alloc_reg() {
        if (next_reg >= 65535) throw std::runtime_error("register limit exceeded (max 65535)");
        uint16_t r = next_reg++;
        if (next_reg > max_regs) max_regs = next_reg;
        return r;
    }

    int resolve_local(const std::string& name) {
        if (locals.count(name)) return locals[name];
        return -1;
    }

    bool is_declared_global(const std::string& name) const {
        return global_decls.count(name) != 0;
    }

    void define_local(const std::string& name, uint16_t reg) {
        locals[name] = reg;
        if (debug_local_names.size() <= (size_t)reg) debug_local_names.resize((size_t)reg + 1);
        debug_local_names[reg] = name;
    }

    uint16_t alloc_named_local(const std::string& name) {
        uint16_t reg = alloc_reg();
        define_local(name, reg);
        return reg;
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

    // ── Module cache ────────────────────────────────────────────────
    // Tracks files already compiled (absolute path) so `import` is idempotent.
    // `importing_now` is the in-flight set — used to detect circular imports.
    std::unordered_set<std::string> imported_paths;
    std::unordered_set<std::string> importing_now;
    // Directory stack — the directory of the currently-parsing file, used to
    // resolve relative import paths.
    std::vector<std::string> base_dir_stack;

    uint16_t alloc_reg() { return current_env->alloc_reg(); }

    struct ScopeGuard {
        CompilerEnv* env;
        uint16_t saved_reg;
        std::unordered_map<std::string, uint16_t> saved_locals;
        std::vector<std::string> saved_debug_local_names;
        ScopeGuard(CompilerEnv* e)
            : env(e),
              saved_reg(e->next_reg),
              saved_locals(e->locals),
              saved_debug_local_names(e->debug_local_names) {}
        ~ScopeGuard() {
            env->next_reg = saved_reg;
            env->locals = saved_locals;
            env->debug_local_names = saved_debug_local_names;
        }
    };

        // Internal JIT bookkeeping.
    struct LoopCtx {
        std::vector<size_t> break_patches;
        std::vector<size_t> continue_patches;
        size_t              continue_target = 0;
        bool                continue_known = false;
    };
    std::vector<LoopCtx> loop_stack;

        // Internal JIT bookkeeping.
    void emit_load_var(const std::string& name, uint16_t target, int ln, bool allow_missing = false) {
        if (current_env->is_declared_global(name)) {
            int glob = chunk.add_global(name);
            chunk.emit(JitOp::LOAD_GLOBAL, target, 0, 0, glob, allow_missing ? 1 : -1, ln);
            return;
        }
        int local = current_env->resolve_local(name);
        if (local != -1) {
            chunk.emit(JitOp::MOVE, target, local, 0, 0, -1, ln);
        } else {
            int upval = current_env->resolve_upvalue(name);
            if (upval != -1) {
                chunk.emit(JitOp::LOAD_UPVAL, target, 0, 0, upval, -1, ln);
            } else {
                int glob = chunk.add_global(name);
                chunk.emit(JitOp::LOAD_GLOBAL, target, 0, 0, glob, allow_missing ? 1 : -1, ln);
            }
        }
    }

    // Stores val_reg into the variable `name`. Returns true if val_reg was absorbed
    // as the permanent storage for a new local (caller must NOT release val_reg).
    //
    // Previous implementation allocated a fresh register for new locals and emitted
    // a MOVE. Combined with the caller's `next_reg -= 1`, that left next_reg pointing
    // at the new local's slot, causing the very next alloc_reg() to overwrite the
    // variable — a classic "i is clobbered by a compare result inside a while loop"
    // bug that showed up in method bodies with while+if patterns. See bug_repro.sura.
    bool emit_store_var(const std::string& name, uint16_t val_reg, int ln) {
        if (current_env->is_declared_global(name)) {
            int glob = chunk.add_global(name);
            chunk.emit(JitOp::STORE_GLOBAL, val_reg, 0, 0, glob, -1, ln);
            return false;
        }
        int local = current_env->resolve_local(name);
        if (local != -1) {
            chunk.emit(JitOp::MOVE, local, val_reg, 0, 0, -1, ln);
            return false;
        }
        int upval = current_env->resolve_upvalue(name);
        if (upval != -1) {
            chunk.emit(JitOp::STORE_UPVAL, val_reg, 0, 0, upval, -1, ln);
            return false;
        }
        if (current_env->enclosing == nullptr) {
            int glob = chunk.add_global(name);
            chunk.emit(JitOp::STORE_GLOBAL, val_reg, 0, 0, glob, -1, ln);
            return false;
        }
        // New local — repurpose val_reg as its permanent register.
        // No extra allocation, no MOVE. val_reg must not be released by caller.
        current_env->define_local(name, val_reg);
        return true;
    }

        // Internal JIT bookkeeping.
    void compile_interpolated_string(const std::string& s, int line, uint16_t target) {
        if (s.find('{') == std::string::npos) {
            chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value(s)), -1, line);
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
                if (i + 1 < s.size() && s[i + 1] == '{') { cur += '{'; i += 2; continue; }
                size_t end = s.find('}', i + 1);
                if (end == std::string::npos) { cur += s[i++]; continue; }
                std::string expr = s.substr(i + 1, end - i - 1);
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
            chunk.emit(JitOp::LOAD_CONST, target, 0, 0, chunk.add_const(Value("")), -1, line);
            return;
        }

        uint16_t saved_next = current_env->next_reg;
        uint16_t tmp_reg = 0;
        if (segs.size() > 1) tmp_reg = alloc_reg();
        uint16_t res_reg = target;

        for (size_t j = 0; j < segs.size(); ++j) {
            uint16_t out_reg = (j == 0) ? res_reg : tmp_reg;

            if (segs[j].is_expr) {
                // Bug fix: interpolated `{...}` may be a full expression
                // (e.g. `{obj.field}`, `{a + b}`, `{f(x)}`), not just a bare
                // variable. Re-parse the segment as an expression and compile.
                try {
                    Parser sub;
                    std::string expr = interp_expr_source(segs[j].text);
                    auto sub_expr = sub.parse_expr_from_source(expr);
                    if (sub_expr) {
                        compile_expr(sub_expr.get(), out_reg);
                    } else {
                        chunk.emit(JitOp::LOAD_NIL, out_reg, 0, 0, 0, -1, line);
                    }
                } catch (...) {
                    // Fall back to bare-variable load if parsing fails
                    emit_load_var(interp_expr_source(segs[j].text), out_reg, line);
                }
            } else {
                chunk.emit(JitOp::LOAD_CONST, out_reg, 0, 0, chunk.add_const(Value(segs[j].text)), -1, line);
            }

            if (j > 0) {
                chunk.emit(JitOp::ADD, res_reg, res_reg, tmp_reg, 1, -1, line);
            }
        }
        current_env->next_reg = saved_next;
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
            uint16_t start_reg = current_env->next_reg; // Internal JIT bookkeeping.
            for (auto& el : al->elements) {
                uint16_t r = alloc_reg();
                compile_expr(el.get(), r);
            }
            chunk.emit(JitOp::MAKE_ARRAY, target, start_reg, 0, (int)al->elements.size(), -1, ln);
            current_env->next_reg = start_reg; // Internal JIT bookkeeping.
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
            if (da->optional && da->obj && da->obj->kind == NK::IDENT)
                emit_load_var(static_cast<const Ident*>(da->obj.get())->name, obj_reg, ln, true);
            else
                compile_expr(da->obj.get(), obj_reg);
            if (da->optional) {
                uint16_t nil_reg = alloc_reg();
                uint16_t cmp_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_NIL, nil_reg, 0, 0, 0, -1, ln);
                chunk.emit(JitOp::CMP_EQ, cmp_reg, obj_reg, nil_reg, 0, -1, ln);
                size_t nil_jump = chunk.emit(JitOp::JUMP_IF_TRUE, cmp_reg, 0, 0, 0, -1, ln);
                chunk.emit(JitOp::DOT_GET, target, obj_reg, 0, 0, chunk.add_string(da->prop), ln);
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(nil_jump, (int)chunk.current_addr());
                chunk.emit(JitOp::LOAD_NIL, target, 0, 0, 0, -1, ln);
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
                current_env->next_reg -= 3;
            } else {
                chunk.emit(JitOp::DOT_GET, target, obj_reg, 0, 0, chunk.add_string(da->prop), ln);
                current_env->next_reg -= 1;
            }
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
            } else if (b->op == "??") {
                uint16_t nil_reg = alloc_reg();
                uint16_t cmp_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_NIL, nil_reg, 0, 0, 0, -1, ln);
                chunk.emit(JitOp::CMP_NEQ, cmp_reg, l_reg, nil_reg, 0, -1, ln);
                size_t use_left = chunk.emit(JitOp::JUMP_IF_TRUE, cmp_reg, 0, 0, 0, -1, ln);
                compile_expr(b->right.get(), target);
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(use_left, (int)chunk.current_addr());
                chunk.emit(JitOp::MOVE, target, l_reg, 0, 0, -1, ln);
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
                current_env->next_reg -= 3;
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
            emit_load_var(ce->name, fn_reg, ln, true);
            
            for (auto& a : ce->args) {
                uint16_t r = alloc_reg();
                compile_expr(a.get(), r);
            }
            
            chunk.emit(JitOp::CALL_FUNC, target, fn_reg, fn_reg + 1, (int)ce->args.size(), chunk.add_string(ce->name), ln);
            current_env->next_reg = args_start; 
            break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(e);
            uint16_t obj_reg = alloc_reg();
            if (mc->optional && mc->obj && mc->obj->kind == NK::IDENT)
                emit_load_var(static_cast<const Ident*>(mc->obj.get())->name, obj_reg, ln, true);
            else
                compile_expr(mc->obj.get(), obj_reg);
            if (mc->optional) {
                uint16_t nil_reg = alloc_reg();
                uint16_t cmp_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_NIL, nil_reg, 0, 0, 0, -1, ln);
                chunk.emit(JitOp::CMP_EQ, cmp_reg, obj_reg, nil_reg, 0, -1, ln);
                size_t nil_jump = chunk.emit(JitOp::JUMP_IF_TRUE, cmp_reg, 0, 0, 0, -1, ln);
                uint16_t args_start = current_env->next_reg;
                for (auto& a : mc->args) {
                    uint16_t r = alloc_reg();
                    compile_expr(a.get(), r);
                }
                chunk.emit(JitOp::METHOD_CALL, target, obj_reg, 0, (int)mc->args.size(), chunk.add_string(mc->method), ln);
                current_env->next_reg = args_start;
                size_t end_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                chunk.patch_jump(nil_jump, (int)chunk.current_addr());
                chunk.emit(JitOp::LOAD_NIL, target, 0, 0, 0, -1, ln);
                chunk.patch_jump(end_jump, (int)chunk.current_addr());
                current_env->next_reg = obj_reg;
                break;
            }
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
            
            for (auto& p : fi.params) current_env->alloc_named_local(p);

            std::vector<const Expr*> lambda_defaults(fi.params.size(), nullptr);
            for (size_t i = 0; i < fi.params.size() && i < fe->defaults.size(); ++i)
                lambda_defaults[i] = fe->defaults[i].get();
            compile_default_prologue(fi.params, lambda_defaults, 0, ln);
            
            compile_block(fe->body.get());
            chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
            
            fi.end_ip = chunk.current_addr();
            fi.max_regs = current_env->max_regs;
            fi.local_names = current_env->debug_local_names;
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

    // Lower executable default expressions into the callee entry. Calls bind
    // the actual positional count to a hidden register identified by the
    // marked NOP. Each expression is evaluated left-to-right only when its
    // parameter was omitted; an explicit nil therefore remains an explicit
    // argument rather than being mistaken for omission.
    void compile_default_prologue(const std::vector<std::string>& params,
                                  const std::vector<const Expr*>& defaults,
                                  uint16_t param_base,
                                  int line) {
        bool has_default = false;
        for (size_t i = 0; i < params.size() && i < defaults.size(); ++i) {
            if (defaults[i]) { has_default = true; break; }
        }
        if (!has_default) return;
        if (params.size() > 65535 ||
            static_cast<size_t>(param_base) + params.size() > 65535) {
            throw std::runtime_error("parameter register limit exceeded (max 65535)");
        }

        const uint16_t argc_reg = current_env->alloc_named_local("\x1f" "argc");
        chunk.emit(JitOp::NOP, argc_reg, param_base,
                   static_cast<uint16_t>(params.size()),
                   JIT_DEFAULT_PROLOGUE_MAGIC, -1, line);

        for (size_t i = 0; i < params.size() && i < defaults.size(); ++i) {
            if (!defaults[i]) continue;
            const uint16_t saved_next = current_env->next_reg;
            const uint16_t threshold_reg = alloc_reg();
            const uint16_t omitted_reg = alloc_reg();
            chunk.emit(JitOp::LOAD_CONST, threshold_reg, 0, 0,
                       chunk.add_const(Value(static_cast<double>(i + 1))), -1, line);
            chunk.emit(JitOp::CMP_LT, omitted_reg, argc_reg, threshold_reg, 0, -1, line);
            const size_t skip_default =
                chunk.emit(JitOp::JUMP_IF_FALSE, omitted_reg, 0, 0, 0, -1, line);
            compile_expr(defaults[i], static_cast<uint16_t>(param_base + i));
            chunk.patch_jump(skip_default, static_cast<int>(chunk.current_addr()));
            current_env->next_reg = saved_next;
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
        case NK::GLOBAL_DECL: {
            auto* gd = static_cast<const GlobalDeclStmt*>(s);
            for (const auto& name : gd->names) {
                current_env->global_decls.insert(name);
                chunk.add_global(name);
            }
            break;
        }
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(s);
            uint16_t val_reg = alloc_reg();
            compile_expr(a->value.get(), val_reg);
            bool absorbed = emit_store_var(a->name, val_reg, ln);
            // Only release val_reg when it was truly a temp (existing local / upvalue / global).
            // If absorbed as a new local, val_reg is its permanent slot — keep it reserved.
            if (!absorbed) current_env->next_reg -= 1;
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
            bool absorbed = emit_store_var(ip->name, res_reg, ln);
            // Always release val_reg; release res_reg only if it wasn't absorbed as a new local.
            current_env->next_reg -= (absorbed ? 1 : 2);
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
            
            compile_stmt(is->then_block.get()); // Internal JIT bookkeeping.
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
            loop_stack.back().continue_known = true;
            
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
                if (loop_stack.back().continue_known) {
                    chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_stack.back().continue_target, -1, ln);
                } else {
                    size_t addr = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
                    loop_stack.back().continue_patches.push_back(addr);
                }
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
            auto require_ident_arg = [&](size_t index, const std::string& form) -> std::string {
                if (index >= c->args.size() || c->args[index]->kind != NK::IDENT) {
                    throw std::runtime_error(form + " requires an output variable");
                }
                return static_cast<const Ident*>(c->args[index].get())->name;
            };
            auto emit_builtin_result_to_var = [&](const std::string& builtin,
                                                  const std::vector<const Expr*>& input_args,
                                                  const std::string& output_name) {
                uint16_t args_start = current_env->next_reg;
                for (const Expr* arg : input_args) {
                    uint16_t r = alloc_reg();
                    compile_expr(arg, r);
                }
                uint16_t dest = input_args.empty() ? alloc_reg() : args_start;
                chunk.emit(JitOp::CALL_BUILTIN, dest, args_start, 0, (int)input_args.size(),
                           chunk.add_string(builtin), ln);
                bool absorbed = emit_store_var(output_name, dest, ln);
                current_env->next_reg = absorbed ? (uint16_t)(dest + 1) : args_start;
            };

            if (c->cmd == "key_down") {
                if (c->args.size() != 2) throw std::runtime_error("key_down command form is: key_down \"key\" var");
                emit_builtin_result_to_var("key_down", {c->args[0].get()}, require_ident_arg(1, "key_down"));
                break;
            }
            if (c->cmd == "readkey_timeout") {
                if (c->args.size() != 2) throw std::runtime_error("readkey_timeout command form is: readkey_timeout var ms");
                emit_builtin_result_to_var("readkey_timeout", {c->args[1].get()}, require_ident_arg(0, "readkey_timeout"));
                break;
            }
            if (c->cmd == "readkey") {
                if (c->args.size() != 1) throw std::runtime_error("readkey command form is: readkey var");
                emit_builtin_result_to_var("readkey", {}, require_ident_arg(0, "readkey"));
                break;
            }
            if (c->cmd == "mouse_down") {
                if (c->args.size() != 2) throw std::runtime_error("mouse_down command form is: mouse_down \"left\" var");
                emit_builtin_result_to_var("mouse_down", {c->args[0].get()}, require_ident_arg(1, "mouse_down"));
                break;
            }
            if (c->cmd == "mouse_pos") {
                if (c->args.size() != 2) throw std::runtime_error("mouse_pos command form is: mouse_pos x y");
                emit_builtin_result_to_var("mouse_x", {}, require_ident_arg(0, "mouse_pos"));
                emit_builtin_result_to_var("mouse_y", {}, require_ident_arg(1, "mouse_pos"));
                break;
            }

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
                uint16_t dest = alloc_reg(); // Internal JIT bookkeeping.
                chunk.emit(JitOp::CALL_BUILTIN, dest, args_start, 0, (int)c->args.size(), chunk.add_string(c->cmd + std::string(1, '\0') + ident_info), ln);
        // Internal JIT bookkeeping.
                current_env->next_reg -= 1;
            }
            current_env->next_reg = args_start; // Internal JIT bookkeeping.
            break;
        }
        case NK::CLASS_DEF: {
            auto* cd = static_cast<const ClassDef*>(s);
            JitClassInfo ci; ci.name = cd->name; ci.parent = cd->parent;
            int fidx = 0;
            for (const auto& k : cd->field_order) {
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
                
                current_env->alloc_named_local("self");
                for (auto& p : mi.params) current_env->alloc_named_local(p);

                std::vector<const Expr*> method_defaults(mi.params.size(), nullptr);
                for (size_t i = 0; i < mi.params.size(); ++i) {
                    if (i < me.defaults.size() && me.defaults[i]) {
                        method_defaults[i] = me.defaults[i].get();
                    } else if (me.generated_field_init) {
                        auto field = cd->field_defaults.find(mi.params[i]);
                        auto explicit_default = cd->field_has_explicit_default.find(mi.params[i]);
                        if (field != cd->field_defaults.end() &&
                            explicit_default != cd->field_has_explicit_default.end() &&
                            explicit_default->second) {
                            method_defaults[i] = field->second.get();
                        }
                    }
                }
                compile_default_prologue(mi.params, method_defaults, 1, ln);
                
                compile_block(me.body);
                chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
                mi.end_ip = chunk.current_addr();
                mi.max_regs = current_env->max_regs;
                mi.local_names = current_env->debug_local_names;
                
                current_env = prev_env;
                ci.methods[mname] = mi;
            }

            // Class field expressions are instance initializers, not class
            // definition constants. Lower them into a reserved zero-argument
            // method. The VM invokes the directly defined initializer for
            // every class in the parent-to-child chain before the user ctor.
            // An auto-generated struct ctor already evaluates each omitted
            // field through its parameter-default prologue, so adding this
            // method there would evaluate defaults twice (and on explicit
            // arguments), which is intentionally avoided.
            bool auto_struct_init = false;
            auto auto_init = cd->methods.find("init");
            if (auto_init != cd->methods.end()) {
                auto_struct_init = auto_init->second.generated_field_init;
            }
            if (!auto_struct_init && !cd->field_order.empty()) {
                JitMethodInfo field_init;
                field_init.name = JIT_FIELD_INITIALIZER_METHOD;
                field_init.entry_ip = chunk.current_addr();

                CompilerEnv new_env(current_env);
                CompilerEnv* prev_env = current_env;
                current_env = &new_env;
                current_env->alloc_named_local("self");

                for (const auto& field_name : cd->field_order) {
                    auto field = cd->field_defaults.find(field_name);
                    if (field == cd->field_defaults.end() || !field->second) continue;
                    const uint16_t saved_next = current_env->next_reg;
                    const uint16_t value_reg = alloc_reg();
                    compile_expr(field->second.get(), value_reg);
                    chunk.emit(JitOp::DOT_SET, 0, value_reg, 0, 0,
                               chunk.add_string(field_name), ln);
                    current_env->next_reg = saved_next;
                }
                chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
                field_init.end_ip = chunk.current_addr();
                field_init.max_regs = current_env->max_regs;
                field_init.local_names = current_env->debug_local_names;
                current_env = prev_env;
                ci.methods[JIT_FIELD_INITIALIZER_METHOD] = std::move(field_init);
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
            for (auto& p : fi.params) current_env->alloc_named_local(p);

            std::vector<const Expr*> function_defaults(fi.params.size(), nullptr);
            for (size_t i = 0; i < fi.params.size() && i < fd->defaults.size(); ++i)
                function_defaults[i] = fd->defaults[i].get();
            compile_default_prologue(fi.params, function_defaults, 0, ln);
            
            compile_block(fd->body.get());
            chunk.emit(JitOp::RETURN_NONE, 0, 0, 0, 0, -1, ln);
            fi.end_ip = chunk.current_addr();
            fi.max_regs = current_env->max_regs;
            fi.local_names = current_env->debug_local_names;
            fi.upvalues = current_env->upvalues;
            
            current_env = prev_env;
            chunk.patch_jump(skip, (int)chunk.current_addr());
            int idx = (int)chunk.func_table.size();
            chunk.func_table.push_back(fi);
            
            uint16_t tmp = alloc_reg();
            chunk.emit(JitOp::MAKE_LAMBDA, tmp, 0, 0, idx, -1, ln);
            bool absorbed = emit_store_var(fd->name, tmp, ln);
            if (!absorbed) current_env->next_reg -= 1;
            break;
        }
        case NK::REPEAT: {
            // repeat N do ... end
            auto* rs = static_cast<const RepeatStmt*>(s);
            ScopeGuard g(current_env);

            uint16_t count_reg = alloc_reg();
            uint16_t iter_reg  = alloc_reg();
            compile_expr(rs->count.get(), count_reg);
            chunk.emit(JitOp::LOAD_CONST, iter_reg, 0, 0, chunk.add_const(Value(0.0)), -1, ln);

            loop_stack.push_back({});
            size_t loop_start = chunk.current_addr();

            // cond: iter < count
            uint16_t cond_reg = alloc_reg();
            chunk.emit(JitOp::CMP_LT, cond_reg, iter_reg, count_reg, 0, -1, ln);
            size_t exit_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            current_env->next_reg -= 1; // free cond_reg

            compile_stmt(rs->body.get());

            loop_stack.back().continue_target = chunk.current_addr();
            loop_stack.back().continue_known = true;
            for (auto addr : loop_stack.back().continue_patches) chunk.patch_jump(addr, (int)loop_stack.back().continue_target);

            // iter += 1
            uint16_t one_reg = alloc_reg();
            chunk.emit(JitOp::LOAD_CONST, one_reg, 0, 0, chunk.add_const(Value(1.0)), -1, ln);
            chunk.emit(JitOp::ADD, iter_reg, iter_reg, one_reg, 0, -1, ln);
            current_env->next_reg -= 1;

            chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_start, -1, ln);
            chunk.patch_jump(exit_jump, (int)chunk.current_addr());

            for (auto addr : loop_stack.back().break_patches) chunk.patch_jump(addr, (int)chunk.current_addr());
            loop_stack.pop_back();
            break;
        }
        case NK::FOR: {
            // for i in from to step do ... end
            auto* fs = static_cast<const ForStmt*>(s);
            ScopeGuard g(current_env);

            // Allocate the loop var as a local register
            uint16_t i_reg    = alloc_reg();
            current_env->define_local(fs->var, i_reg);
            uint16_t to_reg   = alloc_reg();
            uint16_t step_reg = alloc_reg();

            compile_expr(fs->from.get(), i_reg);
            compile_expr(fs->to.get(), to_reg);
            if (fs->step) {
                compile_expr(fs->step.get(), step_reg);
            } else {
                chunk.emit(JitOp::LOAD_CONST, step_reg, 0, 0, chunk.add_const(Value(1.0)), -1, ln);
            }

            uint16_t zero_reg = alloc_reg();
            chunk.emit(JitOp::LOAD_CONST, zero_reg, 0, 0, chunk.add_const(Value(0.0)), -1, ln);

            uint16_t zero_check_reg = alloc_reg();
            chunk.emit(JitOp::CMP_EQ, zero_check_reg, step_reg, zero_reg, 0, -1, ln);
            size_t non_zero_jump = chunk.emit(JitOp::JUMP_IF_FALSE, zero_check_reg, 0, 0, 0, -1, ln);
            uint16_t err_reg = alloc_reg();
            chunk.emit(JitOp::LOAD_CONST, err_reg, 0, 0, chunk.add_const(Value("for step must not be zero")), -1, ln);
            chunk.emit(JitOp::OP_THROW, err_reg, 0, 0, 0, -1, ln);
            chunk.patch_jump(non_zero_jump, (int)chunk.current_addr());
            current_env->next_reg -= 2; // free zero_check and error registers

            loop_stack.push_back({});
            size_t loop_start = chunk.current_addr();

            // Positive steps stop after i > to; negative steps stop after i < to.
            uint16_t cond_reg = alloc_reg();
            chunk.emit(JitOp::CMP_GT, cond_reg, step_reg, zero_reg, 0, -1, ln);
            size_t negative_check_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            chunk.emit(JitOp::CMP_LTE, cond_reg, i_reg, to_reg, 0, -1, ln);
            size_t exit_pos_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            size_t body_jump = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);
            chunk.patch_jump(negative_check_jump, (int)chunk.current_addr());
            chunk.emit(JitOp::CMP_GTE, cond_reg, i_reg, to_reg, 0, -1, ln);
            size_t exit_neg_jump = chunk.emit(JitOp::JUMP_IF_FALSE, cond_reg, 0, 0, 0, -1, ln);
            chunk.patch_jump(body_jump, (int)chunk.current_addr());
            current_env->next_reg -= 1; // free cond_reg

            compile_stmt(fs->body.get());

            loop_stack.back().continue_target = chunk.current_addr();
            loop_stack.back().continue_known = true;
            for (auto addr : loop_stack.back().continue_patches) chunk.patch_jump(addr, (int)loop_stack.back().continue_target);

            // i += step
            chunk.emit(JitOp::ADD, i_reg, i_reg, step_reg, 0, -1, ln);
            chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_start, -1, ln);
            chunk.patch_jump(exit_pos_jump, (int)chunk.current_addr());
            chunk.patch_jump(exit_neg_jump, (int)chunk.current_addr());

            for (auto addr : loop_stack.back().break_patches) chunk.patch_jump(addr, (int)chunk.current_addr());
            loop_stack.pop_back();
            break;
        }
        case NK::FOREACH: {
            // for x in collection do ... end
            // for k, v in dict do ... end
            auto* fe = static_cast<const ForeachStmt*>(s);
            ScopeGuard g(current_env);

            uint16_t coll_reg = alloc_reg();
            compile_expr(fe->collection.get(), coll_reg);

            bool has_kv = !fe->var2.empty();
            if (has_kv) {
                // dict key-value iteration: extract keys array, then iterate
                uint16_t keys_reg = alloc_reg();
                chunk.emit(JitOp::DICT_KEYS, keys_reg, coll_reg, 0, 0, -1, ln);

                uint16_t iter_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_CONST, iter_reg, 0, 0, chunk.add_const(Value(0.0)), -1, ln);

                uint16_t key_reg = alloc_reg();
                current_env->define_local(fe->var, key_reg);
                uint16_t val_reg = alloc_reg();
                current_env->define_local(fe->var2, val_reg);

                loop_stack.push_back({});
                size_t loop_start = chunk.current_addr();
                loop_stack.back().continue_target = loop_start;
                loop_stack.back().continue_known = true;

                // FOREACH_NEXT: key_reg = keys_reg[iter_reg], iter_reg++, or jump exit
                size_t foreach_inst = chunk.emit(JitOp::FOREACH_NEXT, key_reg, iter_reg, keys_reg, 0, -1, ln);

                // val_reg = coll_reg[key_reg]
                chunk.emit(JitOp::INDEX_GET, val_reg, coll_reg, key_reg, 0, -1, ln);

                compile_stmt(fe->body.get());

                chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_start, -1, ln);
                size_t exit_addr = chunk.current_addr();
                chunk.patch_jump(foreach_inst, (int)exit_addr);

                for (auto addr : loop_stack.back().break_patches) chunk.patch_jump(addr, (int)exit_addr);
                loop_stack.pop_back();
            } else {
                // single-var iteration: array, string
                uint16_t iter_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_CONST, iter_reg, 0, 0, chunk.add_const(Value(0.0)), -1, ln);

                uint16_t elem_reg = alloc_reg();
                current_env->define_local(fe->var, elem_reg);

                loop_stack.push_back({});
                size_t loop_start = chunk.current_addr();
                loop_stack.back().continue_target = loop_start;
                loop_stack.back().continue_known = true;

                size_t foreach_inst = chunk.emit(JitOp::FOREACH_NEXT, elem_reg, iter_reg, coll_reg, 0, -1, ln);

                compile_stmt(fe->body.get());

                chunk.emit(JitOp::JUMP, 0, 0, 0, (int)loop_start, -1, ln);
                size_t exit_addr = chunk.current_addr();
                chunk.patch_jump(foreach_inst, (int)exit_addr);

                for (auto addr : loop_stack.back().break_patches) chunk.patch_jump(addr, (int)exit_addr);
                loop_stack.pop_back();
            }
            break;
        }
        case NK::THROW: {
            auto* ts = static_cast<const ThrowStmt*>(s);
            uint16_t msg_reg = alloc_reg();
            compile_expr(ts->msg.get(), msg_reg);
            chunk.emit(JitOp::OP_THROW, msg_reg, 0, 0, 0, -1, ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::IMPORT: {
            auto* is = static_cast<const ImportStmt*>(s);
            namespace fs = std::filesystem;

            // Non-ASCII paths on Windows can throw inside fs::path itself,
            // so the entire path manipulation is wrapped in try/catch with
            // graceful string-level fallbacks.
            std::string key = is->path;
            std::string open_path = is->path;
            std::string parent_dir;
            try {
                fs::path target(is->path);
                if (target.is_relative() && !base_dir_stack.empty())
                    target = fs::path(base_dir_stack.back()) / target;
                open_path = target.string();
                std::error_code ec;
                fs::path canonical = fs::weakly_canonical(target, ec);
                key = (canonical.empty() || ec) ? target.string() : canonical.string();
                parent_dir = fs::path(key).parent_path().string();
            } catch (...) {
                // Pure string-level resolution
                std::string combined = is->path;
                if (!combined.empty() && combined[0] != '/' && combined[0] != '\\' &&
                    !(combined.size() > 1 && combined[1] == ':') &&
                    !base_dir_stack.empty())
                {
                    combined = base_dir_stack.back() + "/" + is->path;
                }
                key = combined;
                open_path = combined;
                size_t slash = combined.find_last_of("/\\");
                parent_dir = (slash == std::string::npos) ? "." : combined.substr(0, slash);
            }

            // Idempotent: skip if already compiled.
            if (imported_paths.count(key)) break;
            // Circular import check.
            if (importing_now.count(key)) throw std::runtime_error("순환 import 감지: " + key);

            // Open the file. std::ifstream on Windows+MinGW doesn't handle
            // non-ASCII paths well, so try a cascade: canonical → raw → basename.
            std::ifstream f(open_path);
            if (!f && open_path != is->path) { f.clear(); f.open(is->path); }
            if (!f) throw std::runtime_error("import 실패 - 파일 없음: " + key);
            std::string src((std::istreambuf_iterator<char>(f)), {});
            f.close();

            // Parse into a fresh AST.
            Parser p;
            auto imported_ast = p.parse_source(src);

            // Mark + compile inline into the same chunk.
            importing_now.insert(key);
            base_dir_stack.push_back(parent_dir);
            compile_block(static_cast<const SuraBlock*>(imported_ast.get()));
            base_dir_stack.pop_back();
            importing_now.erase(key);
            imported_paths.insert(key);
            break;
        }
        case NK::ENUM_DEF: {
            // enum Name do A, B is 42, C end
            //   → Name = {"A": "A", "B": 42, "C": "C"}   (global dict)
            auto* ed = static_cast<const EnumDef*>(s);
            uint16_t dict_reg = alloc_reg();
            chunk.emit(JitOp::MAKE_DICT, dict_reg, 0, 0, 0, -1, ln);
            for (auto& m : ed->members) {
                uint16_t key_reg = alloc_reg();
                uint16_t val_reg = alloc_reg();
                chunk.emit(JitOp::LOAD_CONST, key_reg, 0, 0,
                           chunk.add_const(Value(m.name)), -1, m.line);
                if (m.value) {
                    compile_expr(m.value.get(), val_reg);
                } else {
                    // Default: member's value is its name string
                    chunk.emit(JitOp::LOAD_CONST, val_reg, 0, 0,
                               chunk.add_const(Value(m.name)), -1, m.line);
                }
                chunk.emit(JitOp::INDEX_SET, dict_reg, key_reg, val_reg, 0, -1, m.line);
                current_env->next_reg -= 2;
            }
            // Store as global
            int glob = chunk.add_global(ed->name);
            chunk.emit(JitOp::STORE_GLOBAL, dict_reg, 0, 0, glob, -1, ln);
            current_env->next_reg -= 1;
            break;
        }
        case NK::MATCH: {
            auto* ms = static_cast<const MatchStmt*>(s);
            uint16_t subj = alloc_reg();
            compile_expr(ms->subject.get(), subj);
            std::vector<size_t> end_jumps;

            for (auto& arm : ms->arms) {
                if (arm.is_range) {
                    uint16_t start = alloc_reg();
                    uint16_t finish = alloc_reg();
                    uint16_t cmp1 = alloc_reg();
                    uint16_t cmp2 = alloc_reg();
                    compile_expr(arm.pattern.get(), start);
                    compile_expr(arm.range_end.get(), finish);
                    chunk.emit(JitOp::CMP_GTE, cmp1, subj, start, 0, -1, ln);
                    size_t skip1 = chunk.emit(JitOp::JUMP_IF_FALSE, cmp1, 0, 0, 0, -1, ln);
                    chunk.emit(JitOp::CMP_LTE, cmp2, subj, finish, 0, -1, ln);
                    size_t skip2 = chunk.emit(JitOp::JUMP_IF_FALSE, cmp2, 0, 0, 0, -1, ln);
                    current_env->next_reg -= 4;
                    compile_stmt(arm.body.get());
                    end_jumps.push_back(chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln));
                    chunk.patch_jump(skip1, (int)chunk.current_addr());
                    chunk.patch_jump(skip2, (int)chunk.current_addr());
                    continue;
                }
                if (arm.is_wildcard || !arm.pattern) {
                    compile_stmt(arm.body.get());
                    // Wildcard is always last — no jump needed
                } else {
                    uint16_t pat  = alloc_reg();
                    uint16_t cmp  = alloc_reg();
                    compile_expr(arm.pattern.get(), pat);
                    chunk.emit(JitOp::CMP_EQ, cmp, subj, pat, 0, -1, ln);
                    current_env->next_reg -= 2; // free pat + cmp (cmp still used below)
                    size_t skip = chunk.emit(JitOp::JUMP_IF_FALSE, cmp, 0, 0, 0, -1, ln);
                    compile_stmt(arm.body.get());
                    end_jumps.push_back(chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln));
                    chunk.patch_jump(skip, (int)chunk.current_addr());
                }
            }
            for (auto j : end_jumps) chunk.patch_jump(j, (int)chunk.current_addr());
            current_env->next_reg -= 1; // free subj
            break;
        }
        case NK::TRY: {
            // try ... catch e ... finally ...
            auto* ts = static_cast<const TryStmt*>(s);

            // Allocate register for the catch variable
            ScopeGuard catch_scope(current_env);
            uint16_t catch_var_reg = alloc_reg();
            current_env->define_local(ts->catch_var, catch_var_reg);

            // Emit TRY_BEGIN: operand will be patched to catch_ip
            size_t try_begin_inst = chunk.emit(JitOp::TRY_BEGIN, catch_var_reg, 0, 0, 0, -1, ln);

            // Compile try block
            compile_stmt(ts->try_block.get());

            // TRY_END: no exception path
            chunk.emit(JitOp::TRY_END, 0, 0, 0, 0, -1, ln);

            // Jump over catch block
            size_t skip_catch = chunk.emit(JitOp::JUMP, 0, 0, 0, 0, -1, ln);

            // Patch TRY_BEGIN to here (catch block start)
            size_t catch_addr = chunk.current_addr();
            chunk.patch_jump(try_begin_inst, (int)catch_addr);

            // Compile catch block
            if (ts->catch_block) compile_stmt(ts->catch_block.get());

            size_t after_catch = chunk.current_addr();
            chunk.patch_jump(skip_catch, (int)after_catch);

            // Compile finally block (runs on both paths - duplicated)
            // Note: full finally semantics (running on exception path too) would need
            // additional VM support. This handles the non-exception finally path.
            if (ts->finally_block) compile_stmt(ts->finally_block.get());
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
        return compile(program, "");
    }

    // REPL helper: pre-seed global_names so variable indices stay stable across
    // separate compile() calls. Without this, two REPL lines (e.g. `a is 10`
    // then `print b`) each see global index 0 → both alias vm->globals[0].
    JitChunk compile_with_globals(const SuraBlock* program,
                                  const std::vector<std::string>& seed_globals) {
        chunk = JitChunk();
        chunk.global_names = seed_globals;  // pre-populated; new names append after
        loop_stack.clear();
        imported_paths.clear();
        importing_now.clear();
        base_dir_stack.clear();
        CompilerEnv root_env(nullptr);
        current_env = &root_env;
        compile_block(program);
        chunk.emit(JitOp::HALT, 0, 0, 0, 0, -1, 0);
        chunk.max_regs = current_env->max_regs;
        jit_prepare_native_scratch(chunk);
        return chunk;
    }

    // `source_path` (optional): absolute/relative path of the source file being
    // compiled. Used to resolve `import` relative paths. Pass "" for REPL/inline.
    JitChunk compile(const SuraBlock* program, const std::string& source_path) {
        chunk = JitChunk();
        loop_stack.clear();
        imported_paths.clear();
        importing_now.clear();
        base_dir_stack.clear();

        if (!source_path.empty()) {
            namespace fs = std::filesystem;
            // Windows + non-ASCII paths can throw inside fs::path / weakly_canonical.
            // Fall back to the raw string if the locale can't encode it.
            std::string abs = source_path;
            std::string parent;
            try {
                std::error_code ec;
                fs::path p = fs::weakly_canonical(fs::path(source_path), ec);
                if (!ec && !p.empty()) abs = p.string();
                parent = fs::path(abs).parent_path().string();
            } catch (...) {
                // Best-effort: derive parent via last separator.
                size_t slash = source_path.find_last_of("/\\");
                parent = (slash == std::string::npos) ? "." : source_path.substr(0, slash);
            }
            imported_paths.insert(abs);
            base_dir_stack.push_back(parent);
        }

        CompilerEnv root_env(nullptr);
        current_env = &root_env;

        compile_block(program);
        chunk.emit(JitOp::HALT, 0, 0, 0, 0, -1, 0);
        chunk.max_regs = current_env->max_regs;
        jit_prepare_native_scratch(chunk);
        return chunk;
    }
};
