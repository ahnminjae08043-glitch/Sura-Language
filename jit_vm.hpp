#pragma once
#include "jit_compiler.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <vector>
#include "platform.hpp"


struct CallFrame {
    std::vector<Value> registers;
    GCClosure* closure = nullptr;
    size_t ip = 0;
    size_t end_ip = 0;
    const JitChunk* chunk = nullptr;
    // try/catch support
    bool   in_try        = false;
    size_t catch_ip      = (size_t)-1;
    uint16_t catch_var_reg = 0;
};


struct JitReturn { Value value; };
struct JitThrow { std::string message; int line; };




class JitVM {
    std::vector<Value> globals;
    std::vector<GCUpvalue*> open_upvalues; 
    
    std::unordered_map<std::string, JitFuncInfo> rt_functions;
    std::unordered_map<std::string, JitClassInfo> rt_classes;
    
    int lambda_counter = 0;
    std::vector<CallFrame*> active_frames;
    size_t gc_threshold = 1024;

    void run_gc() {
        
        for (auto& v : globals) v.mark_value();
        
        
        for (auto* uv : open_upvalues) {
            if (!uv->marked) {
                uv->marked = true;
                uv->mark();
            }
        }

        
        for (auto* frame : active_frames) {
            for (auto& v : frame->registers) {
                v.mark_value();
            }
        }

        
        for (auto* frame : active_frames) {
            if (frame->chunk) {
                for (auto v : frame->chunk->constants) { Value temp = v; temp.mark_value(); }
                for (auto& ci : frame->chunk->class_table) {
                    for (auto v : ci.field_defaults) { Value temp = v; temp.mark_value(); }
                    for (auto& [mname, mi] : ci.methods) {
                        for (auto v : mi.defaults) { Value temp = v; temp.mark_value(); }
                    }
                }
                for (auto& fi : frame->chunk->func_table) {
                    for (auto v : fi.defaults) { Value temp = v; temp.mark_value(); }
                }
            }
        }
        
        for (auto& [cname, c_info] : rt_classes) {
            for (auto& v : c_info.field_defaults) v.mark_value();
        }

        
        size_t before = GC::get_objects().size();
        GC::sweep();
        size_t after = GC::get_objects().size();
        gc_threshold = std::max((size_t)1024, after * 2);
    }

    static std::string op_to_str(JitOp op) {
        switch(op) {
            case JitOp::LOAD_CONST: return "LOAD_CONST";
            case JitOp::LOAD_GLOBAL: return "LOAD_GLOBAL";
            case JitOp::STORE_GLOBAL: return "STORE_GLOBAL";
            case JitOp::LOAD_UPVAL: return "LOAD_UPVAL";
            case JitOp::STORE_UPVAL: return "STORE_UPVAL";
            case JitOp::MOVE: return "MOVE";
            case JitOp::ADD: return "ADD";
            case JitOp::CALL_FUNC: return "CALL_FUNC";
            case JitOp::JUMP: return "JUMP";
            case JitOp::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
            case JitOp::PRINT: return "PRINT";
            case JitOp::HALT: return "HALT";
            case JitOp::DEF_CLASS: return "DEF_CLASS";
            case JitOp::NEW_INSTANCE: return "NEW_INSTANCE";
            default: return "OP_" + std::to_string((int)op);
        }
    }

public:
    static void dump(const JitChunk& chunk) {
        std::cout << "--- bytecode dump ---\n";
        for (size_t i=0; i<chunk.code.size(); ++i) {
            auto& c = chunk.code[i];
            std::cout << "[" << i << "] " << op_to_str(c.op) << " R" << (int)c.a << " R" << (int)c.b << " R" << (int)c.c;
            if (c.operand != 0) std::cout << " op=" << c.operand;
            if (c.str_idx != -1) std::cout << " str='" << chunk.get_string(c.str_idx) << "'";
            if (c.ic_cache != -1) std::cout << " ic=" << c.ic_cache;
            std::cout << "\n";
        }
    }

    Value run(const JitChunk& main_chunk) {
        CallFrame frame;
        frame.registers.resize(main_chunk.max_regs > 0 ? main_chunk.max_regs : 256, Value::nil());
        frame.chunk = &main_chunk;
        frame.ip = 0;
        frame.end_ip = main_chunk.code.size();
        
        if (globals.size() < main_chunk.global_names.size()) {
            globals.resize(main_chunk.global_names.size(), Value::nil());
        }

        try {
            return execute_frame(frame);
        } catch (JitReturn& r) {
            return r.value;
        } catch (JitThrow& t) {
            throw; 
        } catch (std::exception& e) {
            std::cerr << "C++ Exception: " << e.what() << "\n";
        }
        return Value::nil();
    }

private:
    GCUpvalue* capture_upvalue(Value* local) {
        for (auto* uv : open_upvalues) {
            if (uv->location == local) return uv;
        }
        auto* uv = GC::allocate<GCUpvalue>(local);
        open_upvalues.push_back(uv);
        return uv;
    }

    void close_upvalues(Value* last_local) {
        for (auto it = open_upvalues.begin(); it != open_upvalues.end(); ) {
            GCUpvalue* uv = *it;
            if (uv->location >= last_local) {
                uv->closed = *(uv->location); 
                uv->location = nullptr;
                it = open_upvalues.erase(it);
            } else {
                ++it;
            }
        }
    }
    const JitMethodInfo* find_method(const std::string& cls, const std::string& method) {
        std::string cur = cls;
        while (!cur.empty() && rt_classes.count(cur)) {
            auto& c = rt_classes[cur];
            if (c.methods.count(method)) return &c.methods[method];
            if (method == "init" && c.methods.count("생성자")) return &c.methods["생성자"];
            if (method == "생성자" && c.methods.count("init")) return &c.methods["init"];
            cur = c.parent;
        }
        return nullptr;
    }

    Value execute_frame(CallFrame& frame) {
        // RAII: automatically remove frame from active_frames on any exit path
        struct FrameGuard {
            std::vector<CallFrame*>& frames;
            FrameGuard(std::vector<CallFrame*>& f, CallFrame* fr) : frames(f) { frames.push_back(fr); }
            ~FrameGuard() { frames.pop_back(); }
        } frame_guard(active_frames, &frame);

        auto* R = frame.registers.data();
        const auto& chunk = *frame.chunk;

        int instruction_counter = 0;

        while (frame.ip < frame.end_ip) {
            if (++instruction_counter > 1000) {
                instruction_counter = 0;
                if (GC::get_objects().size() > gc_threshold) run_gc();
            }

            // Inline caching: ic_cache is mutable so no const_cast needed
            const JitInst& inst = chunk.code[frame.ip++];
            uint16_t a = inst.a, b = inst.b, c = inst.c;

            try {
            switch (inst.op) {
            case JitOp::LOAD_CONST:
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.constants.size())
                    throw JitThrow{"?�수 ?????벗어?? ?�버리??(LOAD_CONST)", inst.line};
                R[a] = chunk.constants[inst.operand];
                break;
            case JitOp::LOAD_NIL:   R[a] = Value::nil(); break;
            case JitOp::LOAD_BOOL:  R[a] = Value(inst.operand != 0); break;
            case JitOp::MOVE:       R[a] = R[b]; break;

            case JitOp::LOAD_GLOBAL:
                if (inst.operand < 0 || (size_t)inst.operand >= globals.size())
                    { R[a] = Value::nil(); break; }
                R[a] = globals[inst.operand];
                break;
            case JitOp::STORE_GLOBAL:
                if (inst.operand < 0 || (size_t)inst.operand >= globals.size()) break;
                globals[inst.operand] = R[a];
                break;
            case JitOp::LOAD_UPVAL: {
                if (!frame.closure || inst.operand < 0 || (size_t)inst.operand >= frame.closure->upvalues.size())
                    throw JitThrow{"Upvalue ?????범위 벗어??(LOAD_UPVAL)", inst.line};
                GCUpvalue* uv = frame.closure->upvalues[inst.operand];
                R[a] = uv->location ? *(uv->location) : uv->closed;
                break;
            }
            case JitOp::STORE_UPVAL: {
                if (!frame.closure || inst.operand < 0 || (size_t)inst.operand >= frame.closure->upvalues.size())
                    throw JitThrow{"Upvalue ?????범위 벗어??(STORE_UPVAL)", inst.line};
                GCUpvalue* uv = frame.closure->upvalues[inst.operand];
                if (uv->location) *(uv->location) = R[a];
                else uv->closed = R[a];
                break;
            }
            
            case JitOp::ADD: R[a] = R[b] + R[c]; break;
            case JitOp::SUB: R[a] = R[b] - R[c]; break;
            case JitOp::MUL: R[a] = R[b] * R[c]; break;
            case JitOp::DIV: R[a] = R[b] / R[c]; break;
            case JitOp::MOD: R[a] = R[b].mod(R[c]); break;
            case JitOp::NEG: R[a] = R[b].negate(); break;
            
            case JitOp::CMP_EQ:  R[a] = Value(R[b].eq(R[c])); break;
            case JitOp::CMP_NEQ: R[a] = Value(R[b].neq(R[c])); break;
            case JitOp::CMP_LT:  R[a] = Value(R[b].lt(R[c])); break;
            case JitOp::CMP_LTE: R[a] = Value(R[b].lte(R[c])); break;
            case JitOp::CMP_GT:  R[a] = Value(R[b].gt(R[c])); break;
            case JitOp::CMP_GTE: R[a] = Value(R[b].gte(R[c])); break;
            
            case JitOp::LOGICAL_NOT: R[a] = R[b].logical_not(); break;
            case JitOp::BIT_AND: R[a] = Value((double)((long long)R[b].to_num() & (long long)R[c].to_num())); break;
            case JitOp::BIT_OR:  R[a] = Value((double)((long long)R[b].to_num() | (long long)R[c].to_num())); break;
            case JitOp::BIT_XOR: R[a] = Value((double)((long long)R[b].to_num() ^ (long long)R[c].to_num())); break;
            case JitOp::BIT_NOT: R[a] = Value((double)(~(long long)R[b].to_num())); break;
            case JitOp::LSHIFT:  R[a] = Value((double)((long long)R[b].to_num() << (long long)R[c].to_num())); break;
            case JitOp::RSHIFT:  R[a] = Value((double)((long long)R[b].to_num() >> (long long)R[c].to_num())); break;

            case JitOp::JUMP: frame.ip = inst.operand; break;
            case JitOp::JUMP_IF_FALSE: if (!R[a].truthy()) frame.ip = inst.operand; break;
            case JitOp::JUMP_IF_TRUE:  if (R[a].truthy()) frame.ip = inst.operand; break;
            
            case JitOp::MAKE_LAMBDA: {
                if (inst.operand < 0 || (size_t)inst.operand >= chunk.func_table.size())
                    throw JitThrow{"?????함수 ?????MAKE_LAMBDA)", inst.line};
                auto& fi = chunk.func_table[inst.operand];
                std::string fname = fi.name.empty() ? "<lambda>" : fi.name;
                GCClosure* closure = GC::allocate<GCClosure>(fname);
                closure->func_idx = inst.operand;
                for (auto& up : fi.upvalues) {
                    if (up.is_local) {
                        closure->upvalues.push_back(capture_upvalue(&R[up.index]));
                    } else {
                        if (!frame.closure || up.index < 0 || (size_t)up.index >= frame.closure->upvalues.size())
                            throw JitThrow{"Upvalue ?????범위 벗어??(MAKE_LAMBDA)", inst.line};
                        closure->upvalues.push_back(frame.closure->upvalues[up.index]);
                    }
                }
                R[a] = Value((GCObject*)closure);
                break;
            }
            case JitOp::DEF_CLASS: {
                JitClassInfo ci = chunk.class_table[inst.operand];
                if (!ci.parent.empty() && rt_classes.count(ci.parent)) {
                    JitClassInfo& p = rt_classes[ci.parent];
                    std::vector<Value> new_defs = p.field_defaults;
                    std::unordered_map<std::string, int> new_idx = p.field_indices; 
                    int offset = p.field_defaults.size();
                    for (auto& [k, v] : ci.field_indices) { 
                        new_idx[k] = offset + v;
                        new_defs.push_back(ci.field_defaults[v]);
                    }
                    ci.field_defaults = new_defs;
                    ci.field_indices = new_idx;
                }
                
                rt_classes[chunk.global_names[inst.str_idx]] = ci;
                break;
            }

            case JitOp::PRINT: {
                std::string out;
                int reg_limit = (int)frame.registers.size();
                for (int i=0; i<inst.operand && (int)(a+i) < reg_limit; ++i)
                    out += R[a+i].to_str();
                std::cout << out << "\n";
                break;
            }
            case JitOp::PRINT_NO_NL: {
                std::string out;
                int reg_limit = (int)frame.registers.size();
                for (int i=0; i<inst.operand && (int)(a+i) < reg_limit; ++i)
                    out += R[a+i].to_str();
                std::cout << out;
                break;
            }

            case JitOp::MAKE_ARRAY: {
                Value arr = Value::make_array();
                for (int i=0; i<inst.operand; ++i) arr.as_arr()->elements.push_back(R[b+i]);
                R[a] = arr;
                break;
            }
            case JitOp::MAKE_DICT: {
                Value dict = Value::make_dict();
                for (int i=0; i<inst.operand; ++i) 
                    dict.dict_set(R[b + i*2].to_str(), R[b + i*2 + 1]);
                R[a] = dict;
                break;
            }

            case JitOp::INDEX_GET: {
                if (R[b].is_arr()) R[a] = R[b].arr_get((int)R[c].to_num());
                else if (R[b].is_dict()) R[a] = R[b].dict_get(R[c].to_str());
                else if (R[b].is_str()) {
                    int i = (int)R[c].to_num();
                    if (i < 0) i += (int)R[b].as_str().size();
                    R[a] = (i>=0 && i<(int)R[b].as_str().size()) ? Value(std::string(1, R[b].as_str()[i])) : Value::nil();
                } else R[a] = Value::nil();
                break;
            }
            case JitOp::INDEX_SET: {
                if (R[a].is_arr()) R[a].arr_set((int)R[b].to_num(), R[c]);
                else if (R[a].is_dict()) R[a].dict_set(R[b].to_str(), R[c]);
                break;
            }
            
            
            case JitOp::DOT_GET: {
                const std::string& prop = chunk.get_string(inst.str_idx);
                if (R[b].is_inst()) {
                    GCInstance* inst_obj = R[b].as_inst();
                    
                    if (inst.ic_cache != -1 && inst_obj->fields.size() > (size_t)inst.ic_cache) {
                        R[a] = inst_obj->fields[inst.ic_cache];
                    } else {
                        
                        int offset = -1;
                        if (rt_classes.count(inst_obj->class_name)) {
                            auto& c = rt_classes[inst_obj->class_name];
                            if (c.field_indices.count(prop)) offset = c.field_indices[prop];
                        }
                        if (offset != -1) {
                            if (rt_classes.count(inst_obj->class_name) && inst_obj->fields.size() < rt_classes[inst_obj->class_name].field_defaults.size()) {
                                inst_obj->fields.resize(rt_classes[inst_obj->class_name].field_defaults.size(), Value::nil());
                            }
                            R[a] = inst_obj->fields[offset];
                            inst.ic_cache = offset; 
                        } else {
                            R[a] = Value::nil();
                        }
                    }
                } else if (R[b].is_dict()) R[a] = R[b].dict_get(prop);
                else R[a] = Value::nil();
                break;
            }
            case JitOp::DOT_SET: {
                const std::string& prop = chunk.get_string(inst.str_idx);
                if (R[a].is_inst()) {
                    GCInstance* inst_obj = R[a].as_inst();
                    if (inst.ic_cache != -1 && inst_obj->fields.size() > (size_t)inst.ic_cache) {
                        inst_obj->fields[inst.ic_cache] = R[b];
                    } else {
                        int offset = -1;
                        if (rt_classes.count(inst_obj->class_name)) {
                            auto& c = rt_classes[inst_obj->class_name];
                            if (c.field_indices.count(prop)) offset = c.field_indices[prop];
                        }
                        if (offset != -1) {
                            inst_obj->fields[offset] = R[b];
                            inst.ic_cache = offset;
                        } else {
                            if (rt_classes.count(inst_obj->class_name)) {
                                auto& c = rt_classes[inst_obj->class_name];
                                offset = c.field_indices.size();
                                c.field_indices[prop] = offset;
                                c.field_defaults.push_back(Value::nil());
                                
                                if (inst_obj->fields.size() < c.field_defaults.size()) {
                                    inst_obj->fields.resize(c.field_defaults.size(), Value::nil());
                                }
                                inst_obj->fields[offset] = R[b];
                                inst.ic_cache = offset;
                            }
                        }
                    }
                } else if (R[a].is_dict()) R[a].dict_set(prop, R[b]);
                break;
            }
            

            case JitOp::OP_IN: {
                if (R[c].is_arr() && R[c].as_arr()) {
                    bool f=false; for(auto& el: R[c].as_arr()->elements) if(R[b].eq(el)){f=true;break;}
                    R[a] = Value(f);
                } else if (R[c].is_dict()) R[a] = Value(R[c].dict_has(R[b].to_str()));
                else R[a] = Value::nil();
                break;
            }
            case JitOp::CALL_FUNC: {
                Value fn_val = R[b];
                if (!fn_val.is_closure()) throw JitThrow{"Not a function: " + fn_val.to_str(), inst.line};
                GCClosure* closure = fn_val.as_closure();
                if (closure->func_idx < 0 || (size_t)closure->func_idx >= chunk.func_table.size())
                    throw JitThrow{"?????함수 ?????CALL_FUNC)", inst.line};
                auto& fi = chunk.func_table[closure->func_idx];
                
                CallFrame new_frame;
                new_frame.registers.resize(fi.max_regs > 0 ? fi.max_regs : 256, Value::nil());
                new_frame.closure = closure;
                new_frame.chunk = frame.chunk;
                new_frame.ip = fi.entry_ip;
                new_frame.end_ip = fi.end_ip;
                
                for (size_t i=0; i<fi.params.size(); ++i) {
                    Value arg = (i < (size_t)inst.operand) ? R[inst.c + i] : fi.defaults[i];
                    new_frame.registers[i] = arg;
                }
                
                try { 
                    Value ret = execute_frame(new_frame);
                    close_upvalues(new_frame.registers.data());
                    R[a] = ret;
                } catch (JitReturn& r) { 
                    close_upvalues(new_frame.registers.data());
                    R[a] = r.value; 
                } catch (...) {
                    close_upvalues(new_frame.registers.data());
                    throw;
                }
                break;
            }
            case JitOp::CALL_BUILTIN: {
                std::string full_name = chunk.get_string(inst.str_idx);
                size_t sep = full_name.find('\0');
                std::string cmd = full_name.substr(0, sep);
                
                if (cmd == "input") {
                    std::string in;
                    if (inst.operand > 0) std::cout << R[b].to_str();
                    std::getline(std::cin, in);
                    R[a] = Value(in);
                } else if (cmd == "exit") {
                    exit(0);
                } else if (cmd == "clock") {
                    auto now = std::chrono::steady_clock::now().time_since_epoch();
                    R[a] = Value(std::chrono::duration<double>(now).count());
                } else if (cmd == "type") {
                    R[a] = Value(R[b].to_str());
                } else {
                    static const std::vector<std::string> BUILTINS = {"print", "input", "exit", "clock", "type"};
                    std::string sug = sura_suggest(cmd, BUILTINS);
                    std::string msg = "?????�는 명령?? '" + cmd + "'";
                    if (!sug.empty()) msg += " (?�시 '" + sug + "'�??�력?�시???�나??)";
                    throw JitThrow{msg, inst.line};
                }
                break;
            }
            case JitOp::METHOD_CALL: {
                const std::string& meth = chunk.get_string(inst.str_idx);
                int nargs = inst.operand;
                // Built-in array methods
                if (R[b].is_arr()) {
                    GCArray* arr = R[b].as_arr();
                    if (meth == "push" || meth == "append") {
                        if (nargs >= 1) arr->elements.push_back(R[b+1]);
                        R[a] = Value::nil();
                    } else if (meth == "pop") {
                        if (!arr->elements.empty()) { R[a] = arr->elements.back(); arr->elements.pop_back(); }
                        else R[a] = Value::nil();
                    } else if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)arr->elements.size());
                    } else if (meth == "remove") {
                        int idx = nargs >= 1 ? (int)R[b+1].to_num() : -1;
                        if (idx < 0) idx += (int)arr->elements.size();
                        if (idx >= 0 && idx < (int)arr->elements.size()) {
                            R[a] = arr->elements[idx];
                            arr->elements.erase(arr->elements.begin() + idx);
                        } else R[a] = Value::nil();
                    } else if (meth == "contains" || meth == "has") {
                        bool found = false;
                        if (nargs >= 1) for (auto& el : arr->elements) if (el.eq(R[b+1])) { found = true; break; }
                        R[a] = Value(found);
                    } else if (meth == "join") {
                        std::string sep = (nargs >= 1) ? R[b+1].to_str() : "";
                        std::string out;
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            if (i > 0) out += sep;
                            out += arr->elements[i].to_str();
                        }
                        R[a] = Value(out);
                    } else if (meth == "slice") {
                        int from = nargs >= 1 ? (int)R[b+1].to_num() : 0;
                        int to   = nargs >= 2 ? (int)R[b+2].to_num() : (int)arr->elements.size();
                        if (from < 0) from += (int)arr->elements.size();
                        if (to   < 0) to   += (int)arr->elements.size();
                        from = std::max(0, std::min(from, (int)arr->elements.size()));
                        to   = std::max(from, std::min(to, (int)arr->elements.size()));
                        Value nv = Value::make_array();
                        nv.as_arr()->elements.insert(nv.as_arr()->elements.end(),
                            arr->elements.begin() + from, arr->elements.begin() + to);
                        R[a] = nv;
                    } else if (meth == "sort") {
                        std::sort(arr->elements.begin(), arr->elements.end(),
                            [](const Value& x, const Value& y) { return x.lt(y); });
                        R[a] = Value::nil();
                    } else if (meth == "reverse") {
                        std::reverse(arr->elements.begin(), arr->elements.end());
                        R[a] = Value::nil();
                    } else if (meth == "clear") {
                        arr->elements.clear(); R[a] = Value::nil();
                    } else if (meth == "insert") {
                        if (nargs >= 2) {
                            int idx = (int)R[b+1].to_num();
                            if (idx < 0) idx += (int)arr->elements.size();
                            idx = std::max(0, std::min(idx, (int)arr->elements.size()));
                            arr->elements.insert(arr->elements.begin() + idx, R[b+2]);
                        }
                        R[a] = Value::nil();
                    } else {
                        R[a] = Value::nil();
                    }
                // Built-in string methods
                } else if (R[b].is_str()) {
                    std::string s = R[b].as_str();
                    if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)s.size());
                    } else if (meth == "upper") {
                        std::string u = s;
                        for (char& ch : u) ch = (char)toupper((unsigned char)ch);
                        R[a] = Value(u);
                    } else if (meth == "lower") {
                        std::string l = s;
                        for (char& ch : l) ch = (char)tolower((unsigned char)ch);
                        R[a] = Value(l);
                    } else if (meth == "trim") {
                        size_t st = s.find_first_not_of(" \t\n\r");
                        size_t en = s.find_last_not_of(" \t\n\r");
                        R[a] = (st == std::string::npos) ? Value(std::string("")) : Value(s.substr(st, en - st + 1));
                    } else if (meth == "contains" || meth == "has") {
                        std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.find(needle) != std::string::npos);
                    } else if (meth == "startswith") {
                        std::string pre = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.size() >= pre.size() && s.substr(0, pre.size()) == pre);
                    } else if (meth == "endswith") {
                        std::string suf = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf);
                    } else if (meth == "find" || meth == "index") {
                        std::string needle = nargs >= 1 ? R[b+1].to_str() : "";
                        auto pos = s.find(needle);
                        R[a] = (pos != std::string::npos) ? Value((double)pos) : Value(-1.0);
                    } else if (meth == "replace") {
                        if (nargs >= 2) {
                            std::string from = R[b+1].to_str(), to = R[b+2].to_str();
                            std::string res; size_t p = 0;
                            while (true) {
                                size_t f = s.find(from, p);
                                if (f == std::string::npos) { res += s.substr(p); break; }
                                res += s.substr(p, f - p) + to;
                                p = f + from.size();
                            }
                            R[a] = Value(res);
                        } else R[a] = R[b];
                    } else if (meth == "split") {
                        std::string sep = nargs >= 1 ? R[b+1].to_str() : " ";
                        Value arr = Value::make_array();
                        if (sep.empty()) {
                            for (char ch : s) arr.as_arr()->elements.push_back(Value(std::string(1, ch)));
                        } else {
                            size_t p = 0;
                            while (true) {
                                size_t f = s.find(sep, p);
                                if (f == std::string::npos) { arr.as_arr()->elements.push_back(Value(s.substr(p))); break; }
                                arr.as_arr()->elements.push_back(Value(s.substr(p, f - p)));
                                p = f + sep.size();
                            }
                        }
                        R[a] = arr;
                    } else if (meth == "slice" || meth == "substr") {
                        int from = nargs >= 1 ? (int)R[b+1].to_num() : 0;
                        int to   = nargs >= 2 ? (int)R[b+2].to_num() : (int)s.size();
                        if (from < 0) from += (int)s.size();
                        if (to   < 0) to   += (int)s.size();
                        from = std::max(0, std::min(from, (int)s.size()));
                        to   = std::max(from, std::min(to, (int)s.size()));
                        R[a] = Value(s.substr(from, to - from));
                    } else if (meth == "to_num" || meth == "tonumber") {
                        try { R[a] = Value(std::stod(s)); } catch (...) { R[a] = Value::nil(); }
                    } else {
                        R[a] = Value::nil();
                    }
                // Built-in dict methods
                } else if (R[b].is_dict()) {
                    GCDict* dict = R[b].as_dict();
                    if (meth == "len" || meth == "size" || meth == "length") {
                        R[a] = Value((double)dict->elements.size());
                    } else if (meth == "has" || meth == "contains") {
                        std::string key = nargs >= 1 ? R[b+1].to_str() : "";
                        R[a] = Value(dict->elements.count(key) > 0);
                    } else if (meth == "keys") {
                        Value arr = Value::make_array();
                        for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(Value(k));
                        R[a] = arr;
                    } else if (meth == "values") {
                        Value arr = Value::make_array();
                        for (auto& [k, v] : dict->elements) arr.as_arr()->elements.push_back(v);
                        R[a] = arr;
                    } else if (meth == "remove" || meth == "delete") {
                        if (nargs >= 1) dict->elements.erase(R[b+1].to_str());
                        R[a] = Value::nil();
                    } else if (meth == "clear") {
                        dict->elements.clear(); R[a] = Value::nil();
                    } else {
                        R[a] = Value::nil();
                    }
                // User-defined class method
                } else if (R[b].is_inst()) {
                    auto mi = find_method(R[b].as_inst()->class_name, meth);
                    if (mi) {
                        CallFrame new_frame;
                        new_frame.registers.resize(mi->max_regs > 0 ? mi->max_regs : 256, Value::nil());
                        new_frame.chunk = frame.chunk;
                        new_frame.ip = mi->entry_ip;
                        new_frame.end_ip = mi->end_ip;
                        new_frame.registers[0] = R[b];
                        for (size_t i = 0; i < mi->params.size(); ++i) {
                            Value arg = (i < (size_t)nargs) ? R[b+1+i] : mi->defaults[i];
                            new_frame.registers[i+1] = arg;
                        }
                        try { execute_frame(new_frame); R[a] = Value::nil(); }
                        catch (JitReturn& r) { R[a] = r.value; }
                        catch (...) { close_upvalues(new_frame.registers.data()); throw; }
                        close_upvalues(new_frame.registers.data());
                    } else R[a] = Value::nil();
                } else {
                    R[a] = Value::nil();
                }
                break;
            }
            case JitOp::NEW_INSTANCE: {
                std::string cls = chunk.get_string(inst.str_idx);
                Value inst_val = Value::make_inst(cls);
                GCInstance* idata = inst_val.as_inst();
                
                if (rt_classes.count(cls)) idata->fields = rt_classes[cls].field_defaults;

                auto ctor = find_method(cls, "생성자"); if (!ctor) ctor = find_method(cls, "init");
                if (ctor) {
                    CallFrame new_frame;
                    new_frame.registers.resize(ctor->max_regs > 0 ? ctor->max_regs : 256, Value::nil());
                    new_frame.chunk = frame.chunk; new_frame.ip = ctor->entry_ip; new_frame.end_ip = ctor->end_ip;
                    new_frame.registers[0] = inst_val; 
                    for (size_t i=0; i<ctor->params.size(); ++i) {
                        Value arg = (i < (size_t)inst.operand) ? R[b+i] : ctor->defaults[i];
                        new_frame.registers[i+1] = arg;
                    }
                    try { execute_frame(new_frame); } catch (JitReturn&) {}
                    close_upvalues(new_frame.registers.data());
                }
                R[a] = inst_val;
                break;
            }
            case JitOp::RETURN_VAL:
                throw JitReturn{R[a]};
            case JitOp::RETURN_NONE:
                throw JitReturn{Value::nil()};
            case JitOp::OP_THROW:
                throw JitThrow{R[a].to_str(), inst.line};
            case JitOp::TRY_BEGIN:
                // a=catch_var_reg, operand=catch_ip
                frame.in_try      = true;
                frame.catch_ip    = (size_t)inst.operand;
                frame.catch_var_reg = a;
                break;
            case JitOp::TRY_END:
                frame.in_try = false;
                break;
            case JitOp::FOREACH_NEXT: {
                // a=value_reg, b=iter_reg (index), c=collection_reg, operand=exit_jump
                if (R[c].is_arr()) {
                    int idx = (int)R[b].to_num();
                    auto* arr = R[c].as_arr();
                    if (idx >= (int)arr->elements.size()) { frame.ip = (size_t)inst.operand; break; }
                    R[a] = arr->elements[idx];
                    R[b] = Value((double)(idx + 1));
                } else if (R[c].is_str()) {
                    int idx = (int)R[b].to_num();
                    const std::string& s = R[c].as_str();
                    if (idx >= (int)s.size()) { frame.ip = (size_t)inst.operand; break; }
                    R[a] = Value(std::string(1, s[idx]));
                    R[b] = Value((double)(idx + 1));
                } else {
                    frame.ip = (size_t)inst.operand;
                }
                break;
            }
            case JitOp::DICT_KEYS: {
                // a=keys_array_reg, b=dict_reg
                Value keys = Value::make_array();
                if (R[b].is_dict()) {
                    auto* dict = R[b].as_dict();
                    for (auto& [k, v] : dict->elements)
                        keys.as_arr()->elements.push_back(Value(k));
                }
                R[a] = keys;
                break;
            }
            case JitOp::HALT: break;
            default: break;
            } // end switch
            } catch (JitThrow& t) {
                if (frame.in_try) {
                    // caught: jump to catch block, store error message
                    frame.registers[frame.catch_var_reg] = Value(t.message);
                    frame.ip    = frame.catch_ip;
                    frame.in_try = false;
                    R = frame.registers.data(); // re-sync pointer after potential resize
                } else {
                    throw; // not in try block, propagate up
                }
            }
        }
        return Value::nil();
    }
};
