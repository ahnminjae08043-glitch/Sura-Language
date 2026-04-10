#ifndef SURA_AST_H
#define SURA_AST_H
#include <string>
#include <vector>
#include <memory>
#include <map>







enum class NK {
    
    NUM_LIT,      
    STR_LIT,      
    BOOL_LIT,     
    NIL_LIT,      

    
    IDENT,        
    BIN_OP,       
    UNARY_OP,     
    DOT_ACCESS,   
    INDEX,        
    CALL,         
    METHOD_CALL,  
    SUPER_CALL,   
    ARRAY_LIT,    
    DICT_LIT,     
    NEW_EXPR,     

    
    SuraBlock,        
    ASSIGN,       
    IN_PLACE,     
    DOT_ASSIGN,   
    INDEX_ASSIGN, 
    IF,           
    WHILE,        
    REPEAT,       
    FOR,          
    FOREACH,      
    FUNC_DEF,     
    CLASS_DEF,    
    NEW_INST,     
    RETURN,       
    BREAK,        
    CONTINUE,     
    THROW,        
    TRY,          
    USE,          
    CMD,          
    EXPR_STMT,    
    TERNARY,      
    FUNC_EXPR,    
    STR_INTERP,   
};





enum class SType {
    ANY,        
    NUMBER,     
    STRING,     
    BOOL,       
    ARRAY,      
    DICT,       
    NIL,        
    CLASS,      
};

inline const char* stype_name(SType t) {
    switch (t) {
        case SType::ANY:    return "?�무";
        case SType::NUMBER: return "?�자";
        case SType::STRING: return "문자열";
        case SType::BOOL:   return "불리언";
        case SType::ARRAY:  return "배열";
        case SType::DICT:   return "?�셔?�리";
        case SType::NIL:    return "?�음";
        case SType::CLASS:  return "<?�래??";
    }
    return "?�무";
}

struct TypeAnnot {
    SType       kind       = SType::ANY;
    std::string class_name;          
    bool        present    = false;  

    bool is_any() const { return !present || kind == SType::ANY; }

    
    
    bool matches_vtype(int vt) const {
        if (is_any()) return true;
        
        switch (kind) {
            case SType::NUMBER: return vt == 1;
            case SType::STRING: return vt == 3;
            case SType::BOOL:   return vt == 2;
            case SType::ARRAY:  return vt == 4;
            case SType::DICT:   return vt == 5;
            case SType::NIL:    return vt == 0;
            case SType::CLASS:  return vt == 5; 
            default:            return true;
        }
    }
};




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


struct DotAccess : Expr {
    ExprPtr     obj;
    std::string prop;
    DotAccess(ExprPtr o, std::string p, int l = 0)
        : Expr(NK::DOT_ACCESS, l), obj(std::move(o)), prop(std::move(p)) {}
};


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



struct MethodCallExpr : Expr {
    ExprPtr              obj;
    std::string          method;
    std::vector<ExprPtr> args;
    MethodCallExpr(ExprPtr o, std::string m, int l = 0)
        : Expr(NK::METHOD_CALL, l), obj(std::move(o)), method(std::move(m)) {}
};


struct SuperCallExpr : Expr {
    std::string          method;
    std::vector<ExprPtr> args;
    SuperCallExpr(std::string m, int l = 0)
        : Expr(NK::SUPER_CALL, l), method(std::move(m)) {}
};


struct ArrayLit : Expr {
    std::vector<ExprPtr> elements;
    ArrayLit(int l = 0) : Expr(NK::ARRAY_LIT, l) {}
};




struct DictLit : Expr {
    std::map<std::string, ExprPtr> pairs;
    std::vector<std::string>       key_order; 
    DictLit(int l = 0) : Expr(NK::DICT_LIT, l) {}
    void add(std::string k, ExprPtr v) {
        key_order.push_back(k);
        pairs[k] = std::move(v);
    }
};


struct NewExpr : Expr {
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewExpr(std::string c, int l = 0)
        : Expr(NK::NEW_EXPR, l), class_name(std::move(c)) {}
};




struct SuraBlock : Stmt {
    std::vector<StmtPtr> body;
    SuraBlock(int l = 0) : Stmt(NK::SuraBlock, l) {}
};
using BlockPtr = std::unique_ptr<SuraBlock>;







struct AssignStmt : Stmt {
    std::string name;
    ExprPtr     value;
    TypeAnnot   type_annot; 
    AssignStmt(std::string n, ExprPtr v, int l = 0)
        : Stmt(NK::ASSIGN, l), name(std::move(n)), value(std::move(v)) {}
};



struct InPlaceStmt : Stmt {
    std::string name;
    std::string op;   
    ExprPtr     value;
    InPlaceStmt(std::string n, std::string o, ExprPtr v, int l = 0)
        : Stmt(NK::IN_PLACE, l), name(std::move(n)),
          op(std::move(o)), value(std::move(v)) {}
};

using InplaceStmt = InPlaceStmt;



struct DotAssignStmt : Stmt {
    std::string obj_name;
    std::string prop;
    ExprPtr     value;
    DotAssignStmt(std::string o, std::string p, ExprPtr v, int l = 0)
        : Stmt(NK::DOT_ASSIGN, l), obj_name(std::move(o)),
          prop(std::move(p)), value(std::move(v)) {}
};



struct IndexAssignStmt : Stmt {
    std::string name;
    ExprPtr     key;
    ExprPtr     value;
    IndexAssignStmt(std::string n, ExprPtr k, ExprPtr v, int l = 0)
        : Stmt(NK::INDEX_ASSIGN, l), name(std::move(n)),
          key(std::move(k)), value(std::move(v)) {}
};





struct IfStmt : Stmt {
    ExprPtr  cond;
    BlockPtr then_block;
    BlockPtr else_block; 
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



struct ForStmt : Stmt {
    std::string var;
    ExprPtr     from;
    ExprPtr     to;
    ExprPtr     step; 
    BlockPtr    body;
    ForStmt(std::string v, ExprPtr fr, ExprPtr t, ExprPtr st, BlockPtr b, int l = 0)
        : Stmt(NK::FOR, l), var(std::move(v)),
          from(std::move(fr)), to(std::move(t)),
          step(std::move(st)), body(std::move(b)) {}
};


struct ForeachStmt : Stmt {
    std::string var;
    std::string var2; 
    ExprPtr     collection;
    BlockPtr    body;
    ForeachStmt(std::string v, std::string v2, ExprPtr col, BlockPtr b, int l = 0)
        : Stmt(NK::FOREACH, l), var(std::move(v)), var2(std::move(v2)),
          collection(std::move(col)), body(std::move(b)) {}
};





struct FuncDef : Stmt {
    std::string              name;
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults;    
    std::vector<TypeAnnot>   param_types; 
    TypeAnnot                return_type; 
    BlockPtr                 body;
    FuncDef(std::string n, std::vector<std::string> p, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)),
          params(std::move(p)), body(std::move(b)) {}
    
    FuncDef(std::string n, BlockPtr b, int l = 0)
        : Stmt(NK::FUNC_DEF, l), name(std::move(n)), body(std::move(b)) {}
};


struct MethodEntry {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; 
    SuraBlock*                   body = nullptr; 
};




struct ClassDef : Stmt {
    std::string name;
    std::string parent; 

    std::map<std::string, ExprPtr>     field_defaults;
    std::map<std::string, MethodEntry> methods;

    
    std::vector<BlockPtr> method_bodies;

    
    ClassDef(std::string n, std::string p = {}, int l = 0)
        : Stmt(NK::CLASS_DEF, l), name(std::move(n)), parent(std::move(p)) {}

    
    
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







struct NewInstStmt : Stmt {
    std::string          var;
    std::string          class_name;
    std::vector<ExprPtr> args;
    NewInstStmt(std::string v, std::string c, int l = 0)
        : Stmt(NK::NEW_INST, l), var(std::move(v)), class_name(std::move(c)) {}
};





struct ReturnStmt : Stmt {
    ExprPtr value; 
    ReturnStmt(ExprPtr v, int l = 0) : Stmt(NK::RETURN, l), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    BreakStmt(int l = 0) : Stmt(NK::BREAK, l) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt(int l = 0) : Stmt(NK::CONTINUE, l) {}
};



struct ThrowStmt : Stmt {
    ExprPtr msg;
    ThrowStmt(ExprPtr m, int l = 0) : Stmt(NK::THROW, l), msg(std::move(m)) {}
};

struct TryStmt : Stmt {
    BlockPtr    try_block;
    std::string catch_var;
    BlockPtr    catch_block;
    BlockPtr    finally_block; 
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


struct TernaryExpr : Expr {
    ExprPtr cond, then_val, else_val;
    TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e, int l)
        : Expr(NK::TERNARY, l), cond(std::move(c)),
          then_val(std::move(t)), else_val(std::move(e)) {}
};


struct FuncExpr : Expr {
    std::vector<std::string> params;
    std::vector<ExprPtr>     defaults; 
    BlockPtr                 body;
    FuncExpr(int l = 0) : Expr(NK::FUNC_EXPR, l) {}
};


struct StrInterp : Expr {
    std::vector<ExprPtr> parts; 
    StrInterp(int l = 0) : Expr(NK::STR_INTERP, l) {}
};




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
            printf("%sBOOL(%s)\n", ind.c_str(), (static_cast<const BoolLit*>(e)->value ? "참" : "거짓")); break;
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

#endif 
