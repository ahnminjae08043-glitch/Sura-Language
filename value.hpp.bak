#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <sstream>
#include <charconv>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <cstring>

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  GC ê°ì²´ ?€??
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
enum class ObjType {
    STRING,
    ARRAY,
    DICT,
    FUNC,
    UPVALUE,
    INSTANCE // ?¥í›„ ê°ì²´ ?¸ìŠ¤?´ìŠ¤??
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  GCObject ??ì»¤ìŠ¤?€ ê°€ë¹„ì? ì»¬ë ‰??ê´€ë¦?ê°ì²´
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
struct GCObject {
    ObjType obj_type;
    bool marked = false;
    
    GCObject(ObjType t) : obj_type(t) {}
    virtual ~GCObject() = default;
    virtual void mark() {} // ?˜ìœ„ ?€?…ì—??ë£¨íŠ¸ ì°¸ì¡° ë§ˆí‚¹
};

struct GCString : public GCObject {
    std::string str;
    GCString(const std::string& s) : GCObject(ObjType::STRING), str(s) {}
};

class Value;

struct GCClosure : public GCObject {
    std::string name;
    int func_idx = -1;
    std::vector<struct GCUpvalue*> upvalues;
    GCClosure(const std::string& n) : GCObject(ObjType::FUNC), name(n) {}
    void mark() override;
};

struct GCInstance : public GCObject {
    std::string class_name;
    std::vector<Value> fields;
    GCInstance(const std::string& name) : GCObject(ObjType::INSTANCE), class_name(name) {}
    void mark() override;
};

struct GCArray : public GCObject {
    std::vector<Value> elements;
    GCArray() : GCObject(ObjType::ARRAY) {}
    void mark() override;
};

struct GCDict : public GCObject {
    std::unordered_map<std::string, Value> elements;
    GCDict() : GCObject(ObjType::DICT) {}
    void mark() override;
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  GC ????ë©”ëª¨ë¦??¤ìº??ë°??˜ì§‘ê¸?
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
class GC {
public:
    static std::vector<GCObject*> objects;

    template<typename T, typename... Args>
    static T* allocate(Args&&... args) {
        T* obj = new T(std::forward<Args>(args)...);
        objects.push_back(obj);
        return obj;
    }

    static void sweep() {
        std::vector<GCObject*> survivors;
        for (auto* obj : objects) {
            if (obj->marked) {
                obj->marked = false;
                survivors.push_back(obj);
            } else {
                delete obj;
            }
        }
        objects = std::move(survivors);
    }
};

inline std::vector<GCObject*> GC::objects;

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  Value ???˜ë¼ ?¸ì–´??ëª¨ë“  ê°??œí˜„ (NaN-Boxing)
//  - ?¬ê¸°: 8ë°”ì´??ê³ ì • êµ¬ì¡°ì²?
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
const uint64_t QNAN = 0x7FFC000000000000;
const uint64_t SIGN_BIT = 0x8000000000000000;
const uint64_t TAG_NIL   = 1;
const uint64_t TAG_FALSE = 2;
const uint64_t TAG_TRUE  = 3;
const uint64_t VAL_NIL   = QNAN | TAG_NIL;
const uint64_t VAL_FALSE = QNAN | TAG_FALSE;
const uint64_t VAL_TRUE  = QNAN | TAG_TRUE;

class Value {
    uint64_t val;
public:
    Value() : val(VAL_NIL) {}

    explicit Value(double v) { std::memcpy(&val, &v, sizeof(double)); }
    explicit Value(float v) : Value((double)v) {}
    explicit Value(int v) : Value((double)v) {}
    explicit Value(long v) : Value((double)v) {}
    explicit Value(long long v) : Value((double)v) {}
    
    explicit Value(bool b) : val(b ? VAL_TRUE : VAL_FALSE) {}

    explicit Value(GCObject* ptr) {
        val = SIGN_BIT | QNAN | (reinterpret_cast<uint64_t>(ptr) & 0xFFFFFFFFFFFF);
    }

    explicit Value(const std::string& s) : Value((GCObject*)GC::allocate<GCString>(s)) {}
    explicit Value(const char* s) : Value(std::string(s ? s : "")) {}

    // ?€?€ ?•ì  ?©í† ë¦??€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    static Value nil() { return Value(); }
    static Value make_array() { return Value((GCObject*)GC::allocate<GCArray>()); }
    static Value make_dict() { return Value((GCObject*)GC::allocate<GCDict>()); }
    static Value make_closure(const std::string& fn_name) { return Value((GCObject*)GC::allocate<GCClosure>(fn_name)); }
    static Value make_inst(const std::string& cls_name) { return Value((GCObject*)GC::allocate<GCInstance>(cls_name)); }

    // ?€?€ ?€??ì²´í¬ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    bool is_num() const { return (val & QNAN) != QNAN; }
    bool is_nil() const { return val == VAL_NIL; }
    bool is_bool() const { return val == VAL_FALSE || val == VAL_TRUE; }
    bool is_obj() const { return (val & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN); }
    
    GCObject* as_obj() const { return reinterpret_cast<GCObject*>(val & 0xFFFFFFFFFFFF); }
    
    bool is_str() const { return is_obj() && as_obj()->obj_type == ObjType::STRING; }
    bool is_arr() const { return is_obj() && as_obj()->obj_type == ObjType::ARRAY; }
    bool is_dict() const { return is_obj() && as_obj()->obj_type == ObjType::DICT; }
    bool is_closure() const { return is_obj() && as_obj()->obj_type == ObjType::FUNC; }
    bool is_upvalue() const { return is_obj() && as_obj()->obj_type == ObjType::UPVALUE; }
    bool is_inst() const { return is_obj() && as_obj()->obj_type == ObjType::INSTANCE; }

    // ?€?€ ê°?ì¶”ì¶œ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    double as_num() const { double d; std::memcpy(&d, &val, sizeof(double)); return d; }
    bool as_bool() const { return val == VAL_TRUE; }
    std::string as_str() const { return is_str() ? static_cast<GCString*>(as_obj())->str : ""; }
    GCArray* as_arr() const { return is_arr() ? static_cast<GCArray*>(as_obj()) : nullptr; }
    GCDict* as_dict() const { return is_dict() ? static_cast<GCDict*>(as_obj()) : nullptr; }
    GCClosure* as_closure() const { return is_closure() ? static_cast<GCClosure*>(as_obj()) : nullptr; }
    GCUpvalue* as_upvalue() const;
    GCInstance* as_inst() const { return is_inst() ? static_cast<GCInstance*>(as_obj()) : nullptr; }

    void mark_value() {
        if (is_obj()) {
            GCObject* obj = as_obj();
            if (obj && !obj->marked) {
                obj->marked = true;
                obj->mark();
            }
        }
    }

    // ?€?€ ?•ë???& ê¸°ë³¸ ?°ì‚° ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    bool truthy() const {
        if (is_nil() || val == VAL_FALSE) return false;
        if (is_num()) return as_num() != 0.0;
        if (is_str()) return !as_str().empty();
        if (is_arr()) return !as_arr()->elements.empty();
        if (is_dict()) return !as_dict()->elements.empty();
        return true;
    }

    std::string to_str() const {
        if (is_nil()) return "nil";
        if (is_bool()) return as_bool() ? "true" : "false";
        if (is_num()) {
            double v = as_num();
            if (v == std::floor(v)) return std::to_string((long long)v);
            return std::to_string(v);
        }
        if (is_str()) return as_str();
        if (is_arr()) {
            std::string s = "[";
            auto* arr = as_arr();
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                if (i > 0) s += ", ";
                s += arr->elements[i].is_str() ? "\"" + arr->elements[i].as_str() + "\"" : arr->elements[i].to_str();
            }
            return s + "]";
        }
        if (is_dict()) {
            std::string s = "{";
            auto* dict = as_dict();
            bool first = true;
            for (const auto& [k, v] : dict->elements) {
                if (!first) s += ", ";
                s += "\"" + k + "\": " + (v.is_str() ? "\"" + v.as_str() + "\"" : v.to_str());
                first = false;
            }
            return s + "}";
        }
        if (is_closure()) return "<Func " + static_cast<GCClosure*>(as_obj())->name + ">";
        if (is_upvalue()) return "<Upvalue>";
        if (is_inst()) return "<Instance " + as_inst()->class_name + ">";
        return "<Unknown>";
    }

    double to_num() const {
        if (is_num()) return as_num();
        if (is_bool()) return as_bool() ? 1.0 : 0.0;
        if (is_str()) {
            try { return std::stod(as_str()); } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    // ?€?€ ?°ì‚° ?¤ë²„ë¡œë”© (?¸ì˜ ?¨ìˆ˜) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    Value operator+(const Value& r) const {
        if (is_num() && r.is_num()) return Value(as_num() + r.as_num());
        if (is_str() || r.is_str()) return Value(to_str() + r.to_str());
        if (is_arr() && r.is_arr()) {
            Value nv = make_array();
            auto* arr = nv.as_arr();
            auto* a1 = as_arr(); auto* a2 = r.as_arr();
            arr->elements.insert(arr->elements.end(), a1->elements.begin(), a1->elements.end());
            arr->elements.insert(arr->elements.end(), a2->elements.begin(), a2->elements.end());
            return nv;
        }
        return nil();
    }
    Value operator-(const Value& r) const { return (is_num() && r.is_num()) ? Value(as_num() - r.as_num()) : nil(); }
    Value operator*(const Value& r) const { return (is_num() && r.is_num()) ? Value(as_num() * r.as_num()) : nil(); }
    Value operator/(const Value& r) const { return (is_num() && r.is_num() && r.as_num() != 0.0) ? Value(as_num() / r.as_num()) : nil(); }
    Value mod(const Value& r) const { return (is_num() && r.is_num() && r.as_num() != 0.0) ? Value(std::fmod(as_num(), r.as_num())) : nil(); }
    Value negate() const { return is_num() ? Value(-as_num()) : nil(); }

    bool eq(const Value& r) const {
        if (is_num() && r.is_num()) return as_num() == r.as_num();
        if (is_nil() && r.is_nil()) return true;
        if (is_bool() && r.is_bool()) return as_bool() == r.as_bool();
        if (is_str() && r.is_str()) return as_str() == r.as_str();
        if (is_obj() && r.is_obj()) return as_obj() == r.as_obj(); // Pointer equality for array/dict
        return false;
    }
    bool neq(const Value& r) const { return !eq(r); }
    bool lt(const Value& r) const { return (is_num() && r.is_num()) ? as_num() < r.as_num() : false; }
    bool lte(const Value& r) const { return (is_num() && r.is_num()) ? as_num() <= r.as_num() : false; }
    bool gt(const Value& r) const { return (is_num() && r.is_num()) ? as_num() > r.as_num() : false; }
    bool gte(const Value& r) const { return (is_num() && r.is_num()) ? as_num() >= r.as_num() : false; }
    
    Value logical_not() const { return Value(!truthy()); }

    // ?€?€ ì»¬ë ‰???œí”Œ ?‘ê·¼ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    Value arr_get(int idx) const {
        if (!is_arr()) return nil();
        auto* arr = as_arr();
        if (idx < 0) idx += (int)arr->elements.size();
        if (idx >= 0 && idx < (int)arr->elements.size()) return arr->elements[idx];
        return nil();
    }
    void arr_set(int idx, const Value& val) {
        if (!is_arr()) return;
        auto* arr = as_arr();
        if (idx < 0) idx += (int)arr->elements.size();
        if (idx >= 0 && idx < (int)arr->elements.size()) arr->elements[idx] = val;
    }

    Value dict_get(const std::string& k) const {
        if (!is_dict()) return nil();
        auto* dict = as_dict();
        if (dict->elements.count(k)) return dict->elements.at(k);
        return nil();
    }
    void dict_set(const std::string& k, const Value& val) {
        if (is_dict()) as_dict()->elements[k] = val;
    }
    bool dict_has(const std::string& k) const {
        return is_dict() ? as_dict()->elements.count(k) > 0 : false;
    }
};
struct GCUpvalue : public GCObject {
    Value* location;
    Value closed;
    GCUpvalue(Value* loc) : GCObject(ObjType::UPVALUE), location(loc) {}
    void mark() override;
};

// êµ¬í˜„ë¶€ë¥??„ë°© ? ì–¸ ?¤ìª½?¼ë¡œ ëº?
inline void GCArray::mark() {
    for (auto& v : elements) v.mark_value();
}
inline void GCDict::mark() {
    for (auto& [k, v] : elements) v.mark_value();
}
inline void GCInstance::mark() {
    for (auto& v : fields) v.mark_value();
}
inline GCUpvalue* Value::as_upvalue() const { 
    return is_upvalue() ? static_cast<GCUpvalue*>(as_obj()) : nullptr; 
}
inline void GCUpvalue::mark() {
    if (location == nullptr) closed.mark_value();
}
inline void GCClosure::mark() {
    for (auto* uv : upvalues) if (uv && !uv->marked) { uv->marked = true; uv->mark(); }
}
