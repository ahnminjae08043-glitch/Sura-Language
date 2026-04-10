#ifndef SURA_AST_H
#define SURA_AST_H
#include <string>
#include <vector>
#include <memory>
#include <map>

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?˜ë¼(SURA) AST  ??v3.0
//  interpreter.hpp ???¤ì œ ?¬ìš© ?¨í„´??ë§ì¶° ?•ì˜
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

// ?€?€ ?¸ë“œ ?œê·¸ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
enum class NK {
    // ?€?€ ë¦¬í„°???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    NUM_LIT,      // ?«ì ë¦¬í„°??     42 / 3.14
    STR_LIT,      // ë¬¸ì??ë¦¬í„°??   "?ˆë…•"
    BOOL_LIT,     // ë¶ˆë¦¬??          ì°?/ ê±°ì§“
    NIL_LIT,      // ??ë¦¬í„°??       nil

    // ?€?€ ?œí˜„???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    IDENT,        // ?ë³„??          x, ?ìˆ˜
    BIN_OP,       // ?´í•­ ?°ì‚°        x + y, x == y
    UNARY_OP,     // ?¨í•­ ?°ì‚°        -x, not x
    DOT_ACCESS,   // ???‘ê·¼          obj.prop
    INDEX,        // ?¸ë±???‘ê·¼      arr[i]
    CALL,         // ?¨ìˆ˜ ?¸ì¶œ        ?”í•˜ê¸?3, 5)
    METHOD_CALL,  // ë©”ì„œ???¸ì¶œ      obj.ë©”ì„œ???¸ì)
    SUPER_CALL,   // super ?¸ì¶œ       super.ë©”ì„œ???¸ì)
    ARRAY_LIT,    // ë°°ì—´ ë¦¬í„°??     [1, 2, 3]
    DICT_LIT,     // ?•ì…”?ˆë¦¬ ë¦¬í„°?? {"??: ê°?
    NEW_EXPR,     // new ?œí˜„??(eval ê²½ë¡œ??

    // ?€?€ ë¬¸ì¥ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    SuraBlock,        // ë¸”ë¡             { stmt* }
    ASSIGN,       // ?¨ìˆœ ?€??       name is expr
    IN_PLACE,     // ë³µí•© ?€??       name += expr
    DOT_ASSIGN,   // ???€??         obj.prop is expr
    INDEX_ASSIGN, // ?¸ë±???€??     arr[i] is expr
    IF,           // ì¡°ê±´ë¬?
    WHILE,        // while ë°˜ë³µ
    REPEAT,       // repeat N ë°˜ë³µ
    FOR,          // for i from s to e [step st]
    FOREACH,      // for item in collection
    FUNC_DEF,     // ?¨ìˆ˜ ?•ì˜
    CLASS_DEF,    // ?´ë˜???•ì˜
    NEW_INST,     // ?¸ìŠ¤?´ìŠ¤ ?ì„±    var is new ClassName(?¸ì)
    RETURN,       // return [ê°?
    BREAK,        // break
    CONTINUE,     // continue
    THROW,        // throw ë©”ì‹œì§€
    TRY,          // try ... catch ?¤ë¥˜ë³€??... end
    USE,          // use ?¼ì´ë¸ŒëŸ¬ë¦?
    CMD,          // ?´ì¥ ëª…ë ¹??     print, arr_push, ...
    EXPR_STMT,    // ?œí˜„??ë¬¸ì¥      8  /  1+2  (REPL??
    TERNARY,      // ?¼í•­ ?°ì‚°??     ì¡°ê±´ ? ì°¸ê°’ : ê±°ì§“ê°?
    FUNC_EXPR,    // ?µëª… ?¨ìˆ˜ ?œí˜„?? func(params) do ... end
    STR_INTERP,   // ë³´ê°„ ë¬¸ì??      "?ˆë…• {name}"
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?€???´ë…¸?Œì´??
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

enum class SType {
    ANY,        // ?€???†ìŒ (ê¸°ì¡´ ?™ì )
    NUMBER,     // ?«ì
    STRING,     // ë¬¸ì??
    BOOL,       // ë¶ˆë¦¬??
    ARRAY,      // ë°°ì—´
    DICT,       // ?•ì…”?ˆë¦¬
    NIL,        // nil
    CLASS,      // ?¬ìš©???•ì˜ ?´ë˜??
};

inline const char* stype_name(SType t) {
    switch (t) {
        case SType::ANY:    return "?„ë¬´";
        case SType::NUMBER: return "?«ì";
        case SType::STRING: return "ë¬¸ì??;
        case SType::BOOL:   return "ë¶ˆë¦¬??;
        case SType::ARRAY:  return "ë°°ì—´";
        case SType::DICT:   return "?•ì…”?ˆë¦¬";
        case SType::NIL:    return "?†ìŒ";
        case SType::CLASS:  return "<?´ë˜??";
    }
    return "?„ë¬´";
}

struct TypeAnnot {
    SType       kind       = SType::ANY;
    std::string class_name;          // CLASS?????´ë˜???´ë¦„
    bool        present    = false;  // ?´ë…¸?Œì´?˜ì´ ëª…ì‹œ?ëŠ”ì§€ ?¬ë?

    bool is_any() const { return !present || kind == SType::ANY; }

    // VType(?°í????€???????´ë…¸?Œì´?˜ê³¼ ?¼ì¹˜?˜ëŠ”ì§€ ?•ì¸
    // value.hpp ??VType ê³?ë¹„êµ ???„ë°© ? ì–¸ ?†ì´ ?¸ë¼?¸ìœ¼ë¡?ì²˜ë¦¬
    bool matches_vtype(int vt) const {
        if (is_any()) return true;
        // vt: 0=NIL, 1=NUMBER, 2=BOOL, 3=STRING, 4=ARRAY, 5=DICT, 6=FUNC
        switch (kind) {
            case SType::NUMBER: return vt == 1;
            case SType::STRING: return vt == 3;
            case SType::BOOL:   return vt == 2;
            case SType::ARRAY:  return vt == 4;
            case SType::DICT:   return vt == 5;
            case SType::NIL:    return vt == 0;
            case SType::CLASS:  return vt == 5; // ?¸ìŠ¤?´ìŠ¤??DICTë¡??œí˜„
            default:            return true;
        }
    }
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ê¸°ë°˜ ?¸ë“œ
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
struct Node {
    NK  kind;
    int line = 0;
    virtual ~Node() = default;
protected:
    Node(NK k, int l = 0) : kind(k), line(l) {}
};

struct Expr : Node {
protected:
    Expr(NK k, int l = 0) : Node(k, l) {}
};
using ExprPtr = std::unique_ptr<Expr>;

struct Stmt : Node {
protected:
    Stmt(NK k, int l = 0) : Node(k, l) {}
};
using StmtPtr = std::unique_ptr<Stmt>;

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ë¦¬í„°??
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct NumLit : Expr {
    double value;
    NumLit(double v, int l = 0) : Expr(NK::NUM_LIT, l), value(v) {}
};

struct StrLit : Expr {
    std::string value;
    StrLit(std::string v, int l = 0) : Expr(NK::STR_LIT, l), value(std::move(v)) {}
};

struct BoolLit : Expr {
    bool value;
    BoolLit(bool v, int l = 0) : Expr(NK::BOOL_LIT, l), value(v) {}
};

struct NilLit : Expr {
    NilLit(int l = 0) : Expr(NK::NIL_LIT, l) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?œí˜„??
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct Ident : Expr {
    std::string name;
    Ident(std::string n, int l = 0) : Expr(NK::IDENT, l), name(std::move(n)) {}
};

struct BinOp : Expr {
    std::string op;
    ExprPtr     left, right;
    BinOp(std::string o, ExprPtr l, ExprPtr r, int ln = 0)
        : Expr(NK::BIN_OP, ln), op(std::move(o)),
          left(std::move(l)), right(std::move(r)) {}
};

struct UnaryOp : Expr {
    std::string op;
    ExprPtr     operand;
    UnaryOp(std::string o, ExprPtr e, int l = 0)
        : Expr(NK::UNARY_OP, l), op(std::move(o)), operand(std::move(e)) {}
};

// ???‘ê·¼: obj.prop
struct DotAccess : Expr {
    ExprPtr     obj;
    std::string prop;
    DotAccess(ExprPtr o, std::string p, int l = 0)
        : Expr(NK::DOT_ACCESS, l), obj(std::move(o)), prop(std::move(p)) {}
};

// ?¸ë±???‘ê·¼: arr[i]  ??interpreter.hpp 250: idx->obj, idx->key
struct IndexExpr : Expr {
    ExprPtr obj;
    ExprPtr key;
    IndexExpr(ExprPtr o, ExprPtr k, int l = 0)
        : Expr(NK::INDEX, l), obj(std::move(o)), key(std::move(k)) {}
};

struct CallExpr : Expr {
    std::string          name;
    std::vector<ExprPtr> args;
    CallExpr(std::string n, int l = 0)
        : Expr(NK::CALL, l), name(std::move(n)) {}
};

// ë©”ì„œ???¸ì¶œ ?œí˜„?? obj.ë©”ì„œ???¸ì)
// interpreter.hpp 338: mc->obj.get()
struct MethodCallExpr : Expr {
    ExprPtr              obj;
    std::string          method;
    std::vector<ExprPtr> args;
    MethodCallExpr(ExprPtr o, std::string m, int l = 0)
        : Expr(NK::METHOD_CALL, l), obj(std::move(o)), method(std::move(m)) {}
};

// super.ë©”ì„œ???¸ì)
struct SuperCallExpr : Expr {
    std::string          method;
    std::vector<ExprPtr> args;
    SuperCallExpr(std::string m, int l = 0)
        : Expr(NK::SUPER_CALL, l), method(std::move(m)) {}
};

// ë°°ì—´ ë¦¬í„°?? [1, 2, "?¬ê³¼"]
struct ArrayLit : Expr {
    std::vector<ExprPtr> elements;
    ArrayLit(int l = 0) : Expr(NK::ARRAY_LIT, l) {}
};

// ?•ì…”?ˆë¦¬ ë¦¬í„°?? {"??: ê°? ...}
// interpreter.hpp 243: `for (auto& [k, v] : dl->pairs)`
//   ??pairs ??map<string, ExprPtr>
struct DictLit : Expr {
    std::map<std::string, ExprPtr> pairs;
    std::vector<std::string>       key_order; // ?½ì… ?œì„œ ë³´ì¡´
    DictLit(int l = 0) : Expr(NK::DICT_LIT, l) {}
    void add(std::string k, ExprPtr v) {
        key_order.push_back(k);
        pairs[k] = std::move(v);
    }
};

// new ?œí˜„??(eval ê²½ë¡œ?? ?¨ë… ?œí˜„?ìœ¼ë¡?????
struct NewExpr : Expr {
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewExpr(std::string c, int l = 0)
        : Expr(NK::NEW_EXPR, l), class_name(std::move(c)) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ë¸”ë¡
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
struct SuraBlock : Stmt {
    std::vector<StmtPtr> body;
    SuraBlock(int l = 0) : Stmt(NK::SuraBlock, l) {}
};
using BlockPtr = std::unique_ptr<SuraBlock>;

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?€??ë¬¸ì¥
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

// ?¨ìˆœ ?€?? name is expr  /  name: ?€??is expr
// interpreter.hpp 373: a->name
struct AssignStmt : Stmt {
    std::string name;
    ExprPtr     value;
    TypeAnnot   type_annot; // ? íƒ???€???´ë…¸?Œì´??
    AssignStmt(std::string n, ExprPtr v, int l = 0)
        : Stmt(NK::ASSIGN, l), name(std::move(n)), value(std::move(v)) {}
};

// ë³µí•© ?€?? name += expr  (IN_PLACE)
// interpreter.hpp 382: ip->name, ip->op, ip->value
struct InPlaceStmt : Stmt {
    std::string name;
    std::string op;   // "+", "-", "*", "/", "%"
    ExprPtr     value;
    InPlaceStmt(std::string n, std::string o, ExprPtr v, int l = 0)
        : Stmt(NK::IN_PLACE, l), name(std::move(n)),
          op(std::move(o)), value(std::move(v)) {}
};
// parser.hpp 451 ?¸í™˜ ë³„ì¹­
using InplaceStmt = InPlaceStmt;

// ???€?? obj.prop is expr  (DOT_ASSIGN)
// interpreter.hpp 399: da->obj_name, da->prop, da->value
struct DotAssignStmt : Stmt {
    std::string obj_name;
    std::string prop;
    ExprPtr     value;
    DotAssignStmt(std::string o, std::string p, ExprPtr v, int l = 0)
        : Stmt(NK::DOT_ASSIGN, l), obj_name(std::move(o)),
          prop(std::move(p)), value(std::move(v)) {}
};

// ?¸ë±???€?? arr[i] is expr  (INDEX_ASSIGN)
// interpreter.hpp 412: ia->name, ia->key, ia->value
struct IndexAssignStmt : Stmt {
    std::string name;
    ExprPtr     key;
    ExprPtr     value;
    IndexAssignStmt(std::string n, ExprPtr k, ExprPtr v, int l = 0)
        : Stmt(NK::INDEX_ASSIGN, l), name(std::move(n)),
          key(std::move(k)), value(std::move(v)) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?œì–´ ?ë¦„
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct IfStmt : Stmt {
    ExprPtr  cond;
    BlockPtr then_block;
    BlockPtr else_block; // nullptr = else ?†ìŒ
    IfStmt(ExprPtr c, BlockPtr t, BlockPtr e, int l = 0)
        : Stmt(NK::IF, l), cond(std::move(c)),
          then_block(std::move(t)), else_block(std::move(e)) {}
};

struct WhileStmt : Stmt {
    ExprPtr  cond;
    BlockPtr body;
    WhileStmt(ExprPtr c, BlockPtr b, int l = 0)
        : Stmt(NK::WHILE, l), cond(std::move(c)), body(std::move(b)) {}
};

struct RepeatStmt : Stmt {
    ExprPtr  count;
    BlockPtr body;
    RepeatStmt(ExprPtr c, BlockPtr b, int l = 0)
        : Stmt(NK::REPEAT, l), count(std::move(c)), body(std::move(b)) {}
};

// for ?˜ì¹˜ ë°˜ë³µ: for i from ?œì‘ to ??[step ê°?
// interpreter.hpp 446: fs->from, fs->to, fs->step, fs->var, fs->body
struct ForStmt : Stmt {
    std::string var;
    ExprPtr     from;
    ExprPtr     to;
    ExprPtr     step; // nullptr ???ë™ +1/-1
    BlockPtr    body;
    ForStmt(std::string v, ExprPtr fr, ExprPtr t, ExprPtr st, BlockPtr b, int l = 0)
        : Stmt(NK::FOR, l), var(std::move(v)),
          from(std::move(fr)), to(std::move(t)),
          step(std::move(st)), body(std::move(b)) {}
};

// foreach ë°˜ë³µ: for item in collection  /  for k, v in dict
struct ForeachStmt : Stmt {
    std::string var;
    std::string var2; // ë¹?ë¬¸ì??= ?¨ì¼ ë³€??
    ExprPtr     collection;
    BlockPtr    body;
    ForeachStmt(std::string v, std::string v2, ExprPtr col, BlockPtr b, int l = 0)
        : Stmt(NK::FOREACH, l), var(std::move(v)), var2(std::move(v2)),
          collection(std::move(col)), body(std::move(b)) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?¨ìˆ˜ / ?´ë˜???•ì˜
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct FuncDef : Stmt {
    std::string              name;
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults;    // params ?€ 1:1, nullptr = ê¸°ë³¸ê°??†ìŒ
    std::vector<TypeAnnot>   param_types; // params ?€ 1:1, ? íƒ??
    TypeAnnot                return_type; // -> ë°˜í™˜ ?€?? ? íƒ??
    BlockPtr                 body;
    FuncDef(std::string n, std::vector<std::string> p, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)),
          params(std::move(p)), body(std::move(b)) {}
    // ?˜ìœ„ ?¸í™˜: params ?˜ì¤‘??push_back
    FuncDef(std::string n, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)), body(std::move(b)) {}
};

// ?´ë˜????ë©”ì„œ????ª©
struct MethodEntry {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; // params ?€ 1:1, nullptr = ê¸°ë³¸ê°??†ìŒ
    SuraBlock*                   body = nullptr; // ?Œìœ ê¶Œì? ClassDef
};

// ?´ë˜???•ì˜
// interpreter.hpp 500: cd->field_defaults  map<string, ExprPtr>
// interpreter.hpp 502: cd->methods         map<string, MethodEntry>
struct ClassDef : Stmt {
    std::string name;
    std::string parent; // ë¹?ë¬¸ì??= ?ì† ?†ìŒ

    std::map<std::string, ExprPtr>     field_defaults;
    std::map<std::string, MethodEntry> methods;

    // method_bodies ê°€ BlockPtr ???Œìœ ê¶Œì„ ë³´ê?
    std::vector<BlockPtr> method_bodies;

    // (name, parent, line)
    ClassDef(std::string n, std::string p = {}, int l = 0)
        : Stmt(NK::CLASS_DEF, l), name(std::move(n)), parent(std::move(p)) {}

    // parser.hpp 367 ?¸í™˜: make_unique<ClassDef>(name, body, ln)
    // ?Œì„œê°€ BlockPtr ???˜ê¸°?????œê·¸?ˆì²˜ ??body ??ë¬´ì‹œ?˜ê³  name/line ë§??¬ìš©
    ClassDef(std::string n, BlockPtr /*body_unused*/, int l = 0)
        : Stmt(NK::CLASS_DEF, l), name(std::move(n)) {}

    void add_method(const std::string& mname,
                    std::vector<std::string> params,
                    std::vector<ExprPtr>     defaults,
                    BlockPtr                 body_ptr) {
        method_bodies.push_back(std::move(body_ptr));
        MethodEntry me;
        me.params   = std::move(params);
        me.defaults = std::move(defaults);
        me.body     = method_bodies.back().get();
        methods[mname] = std::move(me);
    }

    void add_field(std::string fname, ExprPtr default_val) {
        field_defaults[std::move(fname)] = std::move(default_val);
    }
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?¸ìŠ¤?´ìŠ¤ ?ì„± ë¬¸ì¥
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

// var is new ClassName(?¸ì)
// interpreter.hpp 510: ni->var, ni->class_name, ni->args
struct NewInstStmt : Stmt {
    std::string          var;
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewInstStmt(std::string v, std::string c, int l = 0)
        : Stmt(NK::NEW_INST, l), var(std::move(v)), class_name(std::move(c)) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ê¸°í? ë¬¸ì¥
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct ReturnStmt : Stmt {
    ExprPtr value; // nullptr = ê°??†ëŠ” return
    ReturnStmt(ExprPtr v, int l = 0) : Stmt(NK::RETURN, l), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    BreakStmt(int l = 0) : Stmt(NK::BREAK, l) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt(int l = 0) : Stmt(NK::CONTINUE, l) {}
};

// throw ë©”ì‹œì§€
// interpreter.hpp 547: ts->msg.get()
struct ThrowStmt : Stmt {
    ExprPtr msg;
    ThrowStmt(ExprPtr m, int l = 0) : Stmt(NK::THROW, l), msg(std::move(m)) {}
};

struct TryStmt : Stmt {
    BlockPtr    try_block;
    std::string catch_var;
    BlockPtr    catch_block;
    BlockPtr    finally_block; // optional
    TryStmt(BlockPtr t, std::string cv, BlockPtr c, BlockPtr f, int l = 0)
        : Stmt(NK::TRY, l), try_block(std::move(t)),
          catch_var(std::move(cv)), catch_block(std::move(c)),
          finally_block(std::move(f)) {}
};

struct UseStmt : Stmt {
    std::string lib;
    UseStmt(std::string lib, int l = 0) : Stmt(NK::USE, l), lib(std::move(lib)) {}
};

struct CmdStmt : Stmt {
    std::string          cmd;
    std::vector<ExprPtr> args;
    CmdStmt(std::string c, int l = 0) : Stmt(NK::CMD, l), cmd(std::move(c)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(ExprPtr e, int l = 0) : Stmt(NK::EXPR_STMT, l), expr(std::move(e)) {}
};

// ?¼í•­ ?°ì‚°?? ì¡°ê±´ ? ì°¸ê°’ : ê±°ì§“ê°?
struct TernaryExpr : Expr {
    ExprPtr cond, then_val, else_val;
    TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e, int l)
        : Expr(NK::TERNARY, l), cond(std::move(c)),
          then_val(std::move(t)), else_val(std::move(e)) {}
};

// ?µëª… ?¨ìˆ˜ ?œí˜„????func(params) do ... end
struct FuncExpr : Expr {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; // nullptr = ê¸°ë³¸ê°??†ìŒ
    BlockPtr                 body;
    FuncExpr(int l = 0) : Expr(NK::FUNC_EXPR, l) {}
};

// ë³´ê°„ ë¬¸ì????"?ˆë…• {name}! ?˜ì´: {age}??
struct StrInterp : Expr {
    std::vector<ExprPtr> parts; // StrLit ê³??„ì˜ ?œí˜„?ì˜ ?¼í•©
    StrInterp(int l = 0) : Expr(NK::STR_INTERP, l) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  AST ?¤í”„ (?”ë²„ê·¸ìš©)
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
#include <cstdio>
struct SuraBlock;

static void dump_expr(const Expr* e, int depth = 0) {
    auto ind = std::string(depth * 2, ' ');
    if (!e) { printf("%s(null)\n", ind.c_str()); return; }
    switch (e->kind) {
        case NK::NUM_LIT:
            printf("%sNUM(%g)\n", ind.c_str(), static_cast<const NumLit*>(e)->value); break;
        case NK::STR_LIT:
            printf("%sSTR(\"%s\")\n", ind.c_str(), static_cast<const StrLit*>(e)->value.c_str()); break;
        case NK::BOOL_LIT:
            printf("%sBOOL(%s)\n", ind.c_str(), (static_cast<const BoolLit*>(e)->value ? "ì°? : "ê±°ì§“")); break;
        case NK::NIL_LIT:
            printf("%sNIL\n", ind.c_str()); break;
        case NK::IDENT:
            printf("%sIDENT(%s)\n", ind.c_str(), static_cast<const Ident*>(e)->name.c_str()); break;
        case NK::BIN_OP: {
            auto* b = static_cast<const BinOp*>(e);
            printf("%sBINOP(%s)\n", ind.c_str(), b->op.c_str());
            dump_expr(b->left.get(),  depth + 1);
            dump_expr(b->right.get(), depth + 1); break;
        }
        case NK::UNARY_OP: {
            auto* u = static_cast<const UnaryOp*>(e);
            printf("%sUNARY(%s)\n", ind.c_str(), u->op.c_str());
            dump_expr(u->operand.get(), depth + 1); break;
        }
        case NK::DOT_ACCESS: {
            auto* d = static_cast<const DotAccess*>(e);
            printf("%sDOT(.%s)\n", ind.c_str(), d->prop.c_str());
            dump_expr(d->obj.get(), depth + 1); break;
        }
        case NK::INDEX: {
            auto* idx = static_cast<const IndexExpr*>(e);
            printf("%sINDEX\n", ind.c_str());
            dump_expr(idx->obj.get(), depth + 1);
            dump_expr(idx->key.get(), depth + 1); break;
        }
        case NK::CALL: {
            auto* ce = static_cast<const CallExpr*>(e);
            printf("%sCALL %s (%zu ?¸ì)\n", ind.c_str(), ce->name.c_str(), ce->args.size());
            for (auto& a : ce->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(e);
            printf("%sMETHOD .%s (%zu ?¸ì)\n", ind.c_str(), mc->method.c_str(), mc->args.size());
            dump_expr(mc->obj.get(), depth + 1);
            for (auto& a : mc->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::SUPER_CALL: {
            auto* sc = static_cast<const SuperCallExpr*>(e);
            printf("%sSUPER.%s (%zu ?¸ì)\n", ind.c_str(), sc->method.c_str(), sc->args.size());
            for (auto& a : sc->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::ARRAY_LIT: {
            auto* al = static_cast<const ArrayLit*>(e);
            printf("%sARRAY [%zu]\n", ind.c_str(), al->elements.size());
            for (auto& el : al->elements) dump_expr(el.get(), depth + 1); break;
        }
        case NK::DICT_LIT: {
            auto* dl = static_cast<const DictLit*>(e);
            printf("%sDICT {%zu}\n", ind.c_str(), dl->key_order.size());
            for (auto& k : dl->key_order) {
                printf("%s  \"%s\":\n", ind.c_str(), k.c_str());
                dump_expr(dl->pairs.at(k).get(), depth + 2);
            }
            break;
        }
        case NK::NEW_EXPR: {
            auto* ne = static_cast<const NewExpr*>(e);
            printf("%sNEW_EXPR %s\n", ind.c_str(), ne->class_name.c_str());
            for (auto& a : ne->args) dump_expr(a.get(), depth + 1); break;
        }
        default: printf("%s(?????†ëŠ” ?œí˜„??\n", ind.c_str()); break;
    }
}

static void dump_block(const SuraBlock* SuraBlock, int depth = 0);

static void dump_stmt(const Stmt* s, int depth = 0) {
    auto ind = std::string(depth * 2, ' ');
    if (!s) return;
    switch (s->kind) {
        case NK::SuraBlock:
            dump_block(static_cast<const SuraBlock*>(s), depth); break;
        case NK::ASSIGN: {
            auto* a = static_cast<const AssignStmt*>(s);
            printf("%sASSIGN %s\n", ind.c_str(), a->name.c_str());
            dump_expr(a->value.get(), depth + 1); break;
        }
        case NK::IN_PLACE: {
            auto* ip = static_cast<const InPlaceStmt*>(s);
            printf("%sIN_PLACE %s %s=\n", ind.c_str(), ip->name.c_str(), ip->op.c_str());
            dump_expr(ip->value.get(), depth + 1); break;
        }
        case NK::DOT_ASSIGN: {
            auto* da = static_cast<const DotAssignStmt*>(s);
            printf("%sDOT_ASSIGN %s.%s\n", ind.c_str(), da->obj_name.c_str(), da->prop.c_str());
            dump_expr(da->value.get(), depth + 1); break;
        }
        case NK::INDEX_ASSIGN: {
            auto* ia = static_cast<const IndexAssignStmt*>(s);
            printf("%sINDEX_ASSIGN %s[]\n", ind.c_str(), ia->name.c_str());
            dump_expr(ia->key.get(),   depth + 1);
            dump_expr(ia->value.get(), depth + 1); break;
        }
        case NK::IF: {
            auto* is = static_cast<const IfStmt*>(s);
            printf("%sIF\n", ind.c_str());
            printf("%s  ì¡°ê±´:\n", ind.c_str());
            dump_expr(is->cond.get(), depth + 2);
            printf("%s  then:\n", ind.c_str());
            dump_block(is->then_block.get(), depth + 2);
            if (is->else_block) {
                printf("%s  else:\n", ind.c_str());
                dump_block(is->else_block.get(), depth + 2);
            }
            break;
        }
        case NK::WHILE: {
            auto* ws = static_cast<const WhileStmt*>(s);
            printf("%sWHILE\n", ind.c_str());
            dump_expr(ws->cond.get(), depth + 1);
            dump_block(ws->body.get(), depth + 1); break;
        }
        case NK::REPEAT: {
            auto* rs = static_cast<const RepeatStmt*>(s);
            printf("%sREPEAT\n", ind.c_str());
            dump_expr(rs->count.get(), depth + 1);
            dump_block(rs->body.get(), depth + 1); break;
        }
        case NK::FOR: {
            auto* fs = static_cast<const ForStmt*>(s);
            printf("%sFOR %s\n", ind.c_str(), fs->var.c_str());
            printf("%s  from:\n", ind.c_str()); dump_expr(fs->from.get(), depth + 2);
            printf("%s  to:\n",   ind.c_str()); dump_expr(fs->to.get(),   depth + 2);
            if (fs->step) { printf("%s  step:\n", ind.c_str()); dump_expr(fs->step.get(), depth + 2); }
            dump_block(fs->body.get(), depth + 1); break;
        }
        case NK::FOREACH: {
            auto* fe = static_cast<const ForeachStmt*>(s);
            printf("%sFOREACH %s in\n", ind.c_str(), fe->var.c_str());
            dump_expr(fe->collection.get(), depth + 1);
            dump_block(fe->body.get(), depth + 1); break;
        }
        case NK::FUNC_DEF: {
            auto* fd = static_cast<const FuncDef*>(s);
            printf("%sFUNC %s(", ind.c_str(), fd->name.c_str());
            for (size_t i = 0; i < fd->params.size(); ++i) {
                if (i) printf(", ");
                printf("%s", fd->params[i].c_str());
            }
            printf(")\n");
            dump_block(fd->body.get(), depth + 1); break;
        }
        case NK::CLASS_DEF: {
            auto* cd = static_cast<const ClassDef*>(s);
            printf("%sCLASS %s", ind.c_str(), cd->name.c_str());
            if (!cd->parent.empty()) printf(" extends %s", cd->parent.c_str());
            printf("\n");
            for (auto& [k, _] : cd->field_defaults)
                printf("%s  field: %s\n", ind.c_str(), k.c_str());
            for (auto& [m, _] : cd->methods)
                printf("%s  method: %s\n", ind.c_str(), m.c_str());
            break;
        }
        case NK::NEW_INST: {
            auto* ni = static_cast<const NewInstStmt*>(s);
            printf("%sNEW_INST %s = new %s (%zu ?¸ì)\n",
                   ind.c_str(), ni->var.c_str(), ni->class_name.c_str(), ni->args.size());
            for (auto& a : ni->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::RETURN: {
            auto* rs = static_cast<const ReturnStmt*>(s);
            printf("%sRETURN\n", ind.c_str());
            if (rs->value) dump_expr(rs->value.get(), depth + 1); break;
        }
        case NK::BREAK:    printf("%sBREAK\n",    ind.c_str()); break;
        case NK::CONTINUE: printf("%sCONTINUE\n", ind.c_str()); break;
        case NK::THROW: {
            auto* ts = static_cast<const ThrowStmt*>(s);
            printf("%sTHROW\n", ind.c_str());
            dump_expr(ts->msg.get(), depth + 1); break;
        }
        case NK::TRY: {
            auto* ts = static_cast<const TryStmt*>(s);
            printf("%sTRY\n", ind.c_str());
            dump_block(ts->try_block.get(), depth + 1);
            printf("%sCATCH %s\n", ind.c_str(), ts->catch_var.c_str());
            dump_block(ts->catch_block.get(), depth + 1);
            if (ts->finally_block) {
                printf("%sFINALLY\n", ind.c_str());
                dump_block(ts->finally_block.get(), depth + 1);
            }
            break;
        }
        case NK::USE:
            printf("%sUSE %s\n", ind.c_str(), static_cast<const UseStmt*>(s)->lib.c_str()); break;
        case NK::CMD: {
            auto* cs = static_cast<const CmdStmt*>(s);
            printf("%sCMD %s (%zu ?¸ì)\n", ind.c_str(), cs->cmd.c_str(), cs->args.size());
            for (auto& a : cs->args) dump_expr(a.get(), depth + 1); break;
        }
        default: printf("%s(?????†ëŠ” ë¬¸ì¥)\n", ind.c_str()); break;
    }
}

static void dump_block(const SuraBlock* SuraBlock, int depth) {
    if (!SuraBlock) return;
    for (auto& s : SuraBlock->body) dump_stmt(s.get(), depth);
}

static void dump_ast(const SuraBlock* program) {
    printf("?â• AST ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•\n");
    dump_block(program, 0);
    printf("?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•\n");
}

#endif // SURA_AST_H
