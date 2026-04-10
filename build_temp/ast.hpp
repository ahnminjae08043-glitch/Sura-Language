#ifndef SURA_AST_H
#define SURA_AST_H
#include <string>
#include <vector>
#include <memory>
#include <map>

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�라(SURA) AST  ??v3.0
//  interpreter.hpp ???�제 ?�용 ?�턴??맞춰 ?�의
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

// ?�?� ?�드 ?�그 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
enum class NK {
    // ?�?� 리터???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    NUM_LIT,      // ?�자 리터??     42 / 3.14
    STR_LIT,      // 문자??리터??   "?�녕"
    BOOL_LIT,     // 불리??          �?/ 거짓
    NIL_LIT,      // ??리터??       nil

    // ?�?� ?�현???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    IDENT,        // ?�별??          x, ?�수
    BIN_OP,       // ?�항 ?�산        x + y, x == y
    UNARY_OP,     // ?�항 ?�산        -x, not x
    DOT_ACCESS,   // ???�근          obj.prop
    INDEX,        // ?�덱???�근      arr[i]
    CALL,         // ?�수 ?�출        ?�하�?3, 5)
    METHOD_CALL,  // 메서???�출      obj.메서???�자)
    SUPER_CALL,   // super ?�출       super.메서???�자)
    ARRAY_LIT,    // 배열 리터??     [1, 2, 3]
    DICT_LIT,     // ?�셔?�리 리터?? {"??: �?
    NEW_EXPR,     // new ?�현??(eval 경로??

    // ?�?� 문장 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    SuraBlock,        // 블록             { stmt* }
    ASSIGN,       // ?�순 ?�??       name is expr
    IN_PLACE,     // 복합 ?�??       name += expr
    DOT_ASSIGN,   // ???�??         obj.prop is expr
    INDEX_ASSIGN, // ?�덱???�??     arr[i] is expr
    IF,           // 조건�?
    WHILE,        // while 반복
    REPEAT,       // repeat N 반복
    FOR,          // for i from s to e [step st]
    FOREACH,      // for item in collection
    FUNC_DEF,     // ?�수 ?�의
    CLASS_DEF,    // ?�래???�의
    NEW_INST,     // ?�스?�스 ?�성    var is new ClassName(?�자)
    RETURN,       // return [�?
    BREAK,        // break
    CONTINUE,     // continue
    THROW,        // throw 메시지
    TRY,          // try ... catch ?�류변??... end
    USE,          // use ?�이브러�?
    CMD,          // ?�장 명령??     print, arr_push, ...
    EXPR_STMT,    // ?�현??문장      8  /  1+2  (REPL??
    TERNARY,      // ?�항 ?�산??     조건 ? 참값 : 거짓�?
    FUNC_EXPR,    // ?�명 ?�수 ?�현?? func(params) do ... end
    STR_INTERP,   // 보간 문자??      "?�녕 {name}"
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�???�노?�이??
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

enum class SType {
    ANY,        // ?�???�음 (기존 ?�적)
    NUMBER,     // ?�자
    STRING,     // 문자??
    BOOL,       // 불리??
    ARRAY,      // 배열
    DICT,       // ?�셔?�리
    NIL,        // nil
    CLASS,      // ?�용???�의 ?�래??
};

inline const char* stype_name(SType t) {
    switch (t) {
        case SType::ANY:    return "?�무";
        case SType::NUMBER: return "?�자";
        case SType::STRING: return "문자??;
        case SType::BOOL:   return "불리??;
        case SType::ARRAY:  return "배열";
        case SType::DICT:   return "?�셔?�리";
        case SType::NIL:    return "?�음";
        case SType::CLASS:  return "<?�래??";
    }
    return "?�무";
}

struct TypeAnnot {
    SType       kind       = SType::ANY;
    std::string class_name;          // CLASS?????�래???�름
    bool        present    = false;  // ?�노?�이?�이 명시?�는지 ?��?

    bool is_any() const { return !present || kind == SType::ANY; }

    // VType(?��????�???????�노?�이?�과 ?�치?�는지 ?�인
    // value.hpp ??VType �?비교 ???�방 ?�언 ?�이 ?�라?�으�?처리
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
            case SType::CLASS:  return vt == 5; // ?�스?�스??DICT�??�현
            default:            return true;
        }
    }
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  기반 ?�드
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
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

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  리터??
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

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

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�현??
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

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

// ???�근: obj.prop
struct DotAccess : Expr {
    ExprPtr     obj;
    std::string prop;
    DotAccess(ExprPtr o, std::string p, int l = 0)
        : Expr(NK::DOT_ACCESS, l), obj(std::move(o)), prop(std::move(p)) {}
};

// ?�덱???�근: arr[i]  ??interpreter.hpp 250: idx->obj, idx->key
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

// 메서???�출 ?�현?? obj.메서???�자)
// interpreter.hpp 338: mc->obj.get()
struct MethodCallExpr : Expr {
    ExprPtr              obj;
    std::string          method;
    std::vector<ExprPtr> args;
    MethodCallExpr(ExprPtr o, std::string m, int l = 0)
        : Expr(NK::METHOD_CALL, l), obj(std::move(o)), method(std::move(m)) {}
};

// super.메서???�자)
struct SuperCallExpr : Expr {
    std::string          method;
    std::vector<ExprPtr> args;
    SuperCallExpr(std::string m, int l = 0)
        : Expr(NK::SUPER_CALL, l), method(std::move(m)) {}
};

// 배열 리터?? [1, 2, "?�과"]
struct ArrayLit : Expr {
    std::vector<ExprPtr> elements;
    ArrayLit(int l = 0) : Expr(NK::ARRAY_LIT, l) {}
};

// ?�셔?�리 리터?? {"??: �? ...}
// interpreter.hpp 243: `for (auto& [k, v] : dl->pairs)`
//   ??pairs ??map<string, ExprPtr>
struct DictLit : Expr {
    std::map<std::string, ExprPtr> pairs;
    std::vector<std::string>       key_order; // ?�입 ?�서 보존
    DictLit(int l = 0) : Expr(NK::DICT_LIT, l) {}
    void add(std::string k, ExprPtr v) {
        key_order.push_back(k);
        pairs[k] = std::move(v);
    }
};

// new ?�현??(eval 경로?? ?�독 ?�현?�으�?????
struct NewExpr : Expr {
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewExpr(std::string c, int l = 0)
        : Expr(NK::NEW_EXPR, l), class_name(std::move(c)) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  블록
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
struct SuraBlock : Stmt {
    std::vector<StmtPtr> body;
    SuraBlock(int l = 0) : Stmt(NK::SuraBlock, l) {}
};
using BlockPtr = std::unique_ptr<SuraBlock>;

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�??문장
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

// ?�순 ?�?? name is expr  /  name: ?�??is expr
// interpreter.hpp 373: a->name
struct AssignStmt : Stmt {
    std::string name;
    ExprPtr     value;
    TypeAnnot   type_annot; // ?�택???�???�노?�이??
    AssignStmt(std::string n, ExprPtr v, int l = 0)
        : Stmt(NK::ASSIGN, l), name(std::move(n)), value(std::move(v)) {}
};

// 복합 ?�?? name += expr  (IN_PLACE)
// interpreter.hpp 382: ip->name, ip->op, ip->value
struct InPlaceStmt : Stmt {
    std::string name;
    std::string op;   // "+", "-", "*", "/", "%"
    ExprPtr     value;
    InPlaceStmt(std::string n, std::string o, ExprPtr v, int l = 0)
        : Stmt(NK::IN_PLACE, l), name(std::move(n)),
          op(std::move(o)), value(std::move(v)) {}
};
// parser.hpp 451 ?�환 별칭
using InplaceStmt = InPlaceStmt;

// ???�?? obj.prop is expr  (DOT_ASSIGN)
// interpreter.hpp 399: da->obj_name, da->prop, da->value
struct DotAssignStmt : Stmt {
    std::string obj_name;
    std::string prop;
    ExprPtr     value;
    DotAssignStmt(std::string o, std::string p, ExprPtr v, int l = 0)
        : Stmt(NK::DOT_ASSIGN, l), obj_name(std::move(o)),
          prop(std::move(p)), value(std::move(v)) {}
};

// ?�덱???�?? arr[i] is expr  (INDEX_ASSIGN)
// interpreter.hpp 412: ia->name, ia->key, ia->value
struct IndexAssignStmt : Stmt {
    std::string name;
    ExprPtr     key;
    ExprPtr     value;
    IndexAssignStmt(std::string n, ExprPtr k, ExprPtr v, int l = 0)
        : Stmt(NK::INDEX_ASSIGN, l), name(std::move(n)),
          key(std::move(k)), value(std::move(v)) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�어 ?�름
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

struct IfStmt : Stmt {
    ExprPtr  cond;
    BlockPtr then_block;
    BlockPtr else_block; // nullptr = else ?�음
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

// for ?�치 반복: for i from ?�작 to ??[step �?
// interpreter.hpp 446: fs->from, fs->to, fs->step, fs->var, fs->body
struct ForStmt : Stmt {
    std::string var;
    ExprPtr     from;
    ExprPtr     to;
    ExprPtr     step; // nullptr ???�동 +1/-1
    BlockPtr    body;
    ForStmt(std::string v, ExprPtr fr, ExprPtr t, ExprPtr st, BlockPtr b, int l = 0)
        : Stmt(NK::FOR, l), var(std::move(v)),
          from(std::move(fr)), to(std::move(t)),
          step(std::move(st)), body(std::move(b)) {}
};

// foreach 반복: for item in collection  /  for k, v in dict
struct ForeachStmt : Stmt {
    std::string var;
    std::string var2; // �?문자??= ?�일 변??
    ExprPtr     collection;
    BlockPtr    body;
    ForeachStmt(std::string v, std::string v2, ExprPtr col, BlockPtr b, int l = 0)
        : Stmt(NK::FOREACH, l), var(std::move(v)), var2(std::move(v2)),
          collection(std::move(col)), body(std::move(b)) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�수 / ?�래???�의
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

struct FuncDef : Stmt {
    std::string              name;
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults;    // params ?� 1:1, nullptr = 기본�??�음
    std::vector<TypeAnnot>   param_types; // params ?� 1:1, ?�택??
    TypeAnnot                return_type; // -> 반환 ?�?? ?�택??
    BlockPtr                 body;
    FuncDef(std::string n, std::vector<std::string> p, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)),
          params(std::move(p)), body(std::move(b)) {}
    // ?�위 ?�환: params ?�중??push_back
    FuncDef(std::string n, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)), body(std::move(b)) {}
};

// ?�래????메서????��
struct MethodEntry {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; // params ?� 1:1, nullptr = 기본�??�음
    SuraBlock*                   body = nullptr; // ?�유권�? ClassDef
};

// ?�래???�의
// interpreter.hpp 500: cd->field_defaults  map<string, ExprPtr>
// interpreter.hpp 502: cd->methods         map<string, MethodEntry>
struct ClassDef : Stmt {
    std::string name;
    std::string parent; // �?문자??= ?�속 ?�음

    std::map<std::string, ExprPtr>     field_defaults;
    std::map<std::string, MethodEntry> methods;

    // method_bodies 가 BlockPtr ???�유권을 보�?
    std::vector<BlockPtr> method_bodies;

    // (name, parent, line)
    ClassDef(std::string n, std::string p = {}, int l = 0)
        : Stmt(NK::CLASS_DEF, l), name(std::move(n)), parent(std::move(p)) {}

    // parser.hpp 367 ?�환: make_unique<ClassDef>(name, body, ln)
    // ?�서가 BlockPtr ???�기?????�그?�처 ??body ??무시?�고 name/line �??�용
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

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�스?�스 ?�성 문장
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

// var is new ClassName(?�자)
// interpreter.hpp 510: ni->var, ni->class_name, ni->args
struct NewInstStmt : Stmt {
    std::string          var;
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewInstStmt(std::string v, std::string c, int l = 0)
        : Stmt(NK::NEW_INST, l), var(std::move(v)), class_name(std::move(c)) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  기�? 문장
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

struct ReturnStmt : Stmt {
    ExprPtr value; // nullptr = �??�는 return
    ReturnStmt(ExprPtr v, int l = 0) : Stmt(NK::RETURN, l), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    BreakStmt(int l = 0) : Stmt(NK::BREAK, l) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt(int l = 0) : Stmt(NK::CONTINUE, l) {}
};

// throw 메시지
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

// ?�항 ?�산?? 조건 ? 참값 : 거짓�?
struct TernaryExpr : Expr {
    ExprPtr cond, then_val, else_val;
    TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e, int l)
        : Expr(NK::TERNARY, l), cond(std::move(c)),
          then_val(std::move(t)), else_val(std::move(e)) {}
};

// ?�명 ?�수 ?�현????func(params) do ... end
struct FuncExpr : Expr {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; // nullptr = 기본�??�음
    BlockPtr                 body;
    FuncExpr(int l = 0) : Expr(NK::FUNC_EXPR, l) {}
};

// 보간 문자????"?�녕 {name}! ?�이: {age}??
struct StrInterp : Expr {
    std::vector<ExprPtr> parts; // StrLit �??�의 ?�현?�의 ?�합
    StrInterp(int l = 0) : Expr(NK::STR_INTERP, l) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  AST ?�프 (?�버그용)
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
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
            printf("%sBOOL(%s)\n", ind.c_str(), (static_cast<const BoolLit*>(e)->value ? "�? : "거짓")); break;
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
            printf("%sCALL %s (%zu ?�자)\n", ind.c_str(), ce->name.c_str(), ce->args.size());
            for (auto& a : ce->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::METHOD_CALL: {
            auto* mc = static_cast<const MethodCallExpr*>(e);
            printf("%sMETHOD .%s (%zu ?�자)\n", ind.c_str(), mc->method.c_str(), mc->args.size());
            dump_expr(mc->obj.get(), depth + 1);
            for (auto& a : mc->args) dump_expr(a.get(), depth + 1); break;
        }
        case NK::SUPER_CALL: {
            auto* sc = static_cast<const SuperCallExpr*>(e);
            printf("%sSUPER.%s (%zu ?�자)\n", ind.c_str(), sc->method.c_str(), sc->args.size());
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
        default: printf("%s(?????�는 ?�현??\n", ind.c_str()); break;
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
            printf("%s  조건:\n", ind.c_str());
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
            printf("%sNEW_INST %s = new %s (%zu ?�자)\n",
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
            printf("%sCMD %s (%zu ?�자)\n", ind.c_str(), cs->cmd.c_str(), cs->args.size());
            for (auto& a : cs->args) dump_expr(a.get(), depth + 1); break;
        }
        default: printf("%s(?????�는 문장)\n", ind.c_str()); break;
    }
}

static void dump_block(const SuraBlock* SuraBlock, int depth) {
    if (!SuraBlock) return;
    for (auto& s : SuraBlock->body) dump_stmt(s.get(), depth);
}

static void dump_ast(const SuraBlock* program) {
    printf("?�═ AST ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═\n");
    dump_block(program, 0);
    printf("?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═\n");
}

#endif // SURA_AST_H
