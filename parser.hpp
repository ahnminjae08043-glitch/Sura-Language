#pragma once
#include "lexer.hpp"
#include "ast.hpp"
#include "platform.hpp"
#include <stdexcept>
#include <algorithm>

















struct ParseError : std::runtime_error {
    int line;
    std::string details;
    ParseError(const std::string& msg, int ln, const std::string& det = "")
        : std::runtime_error("[?�라 ?�서 ?�류] " + std::to_string(ln) + "�? " + msg + (det.empty() ? "" : " (" + det + ")"))
        , line(ln), details(det) {}
};

class Parser {
    std::vector<Token> tokens;
    size_t             pos = 0;

    
    std::vector<std::string> get_all_keywords() {
        return {
            "is", "if", "then", "else", "end", "while", "do", "func", "repeat", 
            "break", "return", "and", "or", "not", "true", "false", "use", 
            "continue", "for", "in", "to", "try", "catch", "throw", "class", 
            "new", "extends", "super", "elif", "finally",
            "print", "print_n", "input", "exit", "clock", "type" 
        };
    }

    
    Token& peek(int off = 0) {
        static Token eof = {TT::EOF_T, "", 0};
        size_t i = pos + (size_t)off;
        return i < tokens.size() ? tokens[i] : eof;
    }

    Token advance() {
        Token t = peek();
        if (t.type != TT::EOF_T) pos++;
        return t;
    }

    bool check(TT t)  { return peek().type == t; }
    bool match(TT t)  { if (check(t)) { advance(); return true; } return false; }

    Token expect(TT t, const std::string& what) {
        if (!check(t)) {
            std::string msg = "'" + what + "' ?�요 ???�??'" + peek().type_str() + "' ?�음";
            if (peek().type == TT::IDENT) {
                std::string sug = sura_suggest(peek().value, get_all_keywords());
                if (!sug.empty()) msg += " (?�시 '" + sug + "'�??�력?�시???�나??)";
            }
            throw ParseError(msg, peek().line, peek().value);
        }
        return advance();
    }

    void skip_newlines() { while (check(TT::NEWLINE)) advance(); }

    
    
    
    
    TypeAnnot parse_type_annot() {
        TypeAnnot ta;
        ta.present = true;
        if (!check(TT::IDENT))
            throw ParseError("?�???�름 ?�요", peek().line);
        std::string tname = advance().value;
        if      (tname == "?�자"   || tname == "number") ta.kind = SType::NUMBER;
        else if (tname == "문자열" || tname == "string") ta.kind = SType::STRING;
        else if (tname == "불리언" || tname == "bool")   ta.kind = SType::BOOL;
        else if (tname == "배열"   || tname == "array")  ta.kind = SType::ARRAY;
        else if (tname == "?�셔?�리"|| tname == "dict")  ta.kind = SType::DICT;
        else if (tname == "?�음"   || tname == "nil")    ta.kind = SType::NIL;
        else if (tname == "?�무"   || tname == "any")    ta.kind = SType::ANY;
        else {
            
            ta.class_name = tname;
        }
        return ta;
    }

    
    void eat_newline() {
        if (check(TT::EOF_T)) return;
        
        if (check(TT::END) || check(TT::ELSE) || check(TT::ELIF) || check(TT::CATCH) || check(TT::FINALLY)) return;
        if (!match(TT::NEWLINE))
            throw ParseError("�???개행) ?�요", peek().line);
    }

    
    
    

    ExprPtr parse_expr() { return parse_ternary(); }

    
    ExprPtr parse_ternary() {
        auto left = parse_or();
        if (check(TT::QUESTION)) {
            int ln = peek().line; advance();
            auto then_val = parse_expr();
            expect(TT::COLON, ":");
            auto else_val = parse_ternary();
            return std::make_unique<TernaryExpr>(
                std::move(left), std::move(then_val), std::move(else_val), ln);
        }
        return left;
    }

    
    ExprPtr parse_or() {
        auto left = parse_and();
        while (check(TT::OR)) {
            int ln = peek().line; advance();
            auto right = parse_and();
            left = std::make_unique<BinOp>("or", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_and() {
        auto left = parse_not();
        while (check(TT::AND)) {
            int ln = peek().line; advance();
            auto right = parse_not();
            left = std::make_unique<BinOp>("and", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_not() {
        if (check(TT::NOT)) {
            int ln = peek().line; advance();
            return std::make_unique<UnaryOp>("not", parse_not(), ln);
        }
        return parse_compare();
    }

    
    ExprPtr parse_compare() {
        auto left = parse_bitor();
        TT t = peek().type;
        if (t==TT::EQ||t==TT::NEQ||t==TT::GT||t==TT::LT||t==TT::GTE||t==TT::LTE) {
            int ln = peek().line;
            std::string op = advance().value;
            auto right = parse_bitor();
            return std::make_unique<BinOp>(op, std::move(left), std::move(right), ln);
        }
        if (t == TT::IN) {
            int ln = peek().line; advance();
            auto right = parse_bitor();
            return std::make_unique<BinOp>("in", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_bitor() {
        auto left = parse_bitxor();
        while (check(TT::PIPE)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("|", std::move(left), parse_bitxor(), ln);
        }
        return left;
    }

    
    ExprPtr parse_bitxor() {
        auto left = parse_bitand();
        while (check(TT::CARET)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("^", std::move(left), parse_bitand(), ln);
        }
        return left;
    }

    
    ExprPtr parse_bitand() {
        auto left = parse_shift();
        while (check(TT::AMP)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("&", std::move(left), parse_shift(), ln);
        }
        return left;
    }

    
    ExprPtr parse_shift() {
        auto left = parse_additive();
        while (check(TT::LSHIFT) || check(TT::RSHIFT)) {
            int ln = peek().line;
            std::string op = advance().value;
            left = std::make_unique<BinOp>(op, std::move(left), parse_additive(), ln);
        }
        return left;
    }

    
    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        while (check(TT::PLUS) || check(TT::MINUS)) {
            int ln = peek().line;
            std::string op = advance().value;
            auto right = parse_multiplicative();
            left = std::make_unique<BinOp>(op, std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
        while (check(TT::STAR) || check(TT::SLASH) || check(TT::PERCENT)) {
            int ln = peek().line;
            std::string op = advance().value;
            auto right = parse_unary();
            left = std::make_unique<BinOp>(op, std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_unary() {
        if (check(TT::MINUS)) {
            int ln = peek().line; advance();
            return std::make_unique<UnaryOp>("-", parse_unary(), ln);
        }
        if (check(TT::TILDE)) {
            int ln = peek().line; advance();
            return std::make_unique<UnaryOp>("~", parse_unary(), ln);
        }
        return parse_primary();
    }

    
    ExprPtr parse_fstr(const Token& tok) {
        return std::make_unique<StrLit>(tok.value, tok.line);
    }

    ExprPtr parse_primary() {
        int ln = peek().line;

        
        if (check(TT::NUM)) {
            double v = std::stod(peek().value);
            advance();
            return std::make_unique<NumLit>(v, ln);
        }
        
        if (check(TT::STR)) {
            std::string v = advance().value;
            return std::make_unique<StrLit>(std::move(v), ln);
        }
        
        if (check(TT::FSTR)) {
            Token tok = advance();
            return parse_fstr(tok);
        }
        
        if (check(TT::TRUE))  { advance(); return std::make_unique<BoolLit>(true,  ln); }
        if (check(TT::FALSE)) { advance(); return std::make_unique<BoolLit>(false, ln); }


        if (check(TT::IDENT)) {
            std::string name = advance().value;

            
            if (check(TT::LPAREN)) {
                advance(); 
                auto call = std::make_unique<CallExpr>(name, ln);
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    call->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                return call;
            }

            
            ExprPtr expr = std::make_unique<Ident>(name, ln);
            while (true) {
                if (check(TT::DOT)) {
                    advance();
                    std::string prop = expect(TT::IDENT, "?�성�??�는 메서?�명").value;
                    if (check(TT::LPAREN)) {
                        advance();
                        auto mc = std::make_unique<MethodCallExpr>(std::move(expr), prop, ln);
                        while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                            mc->args.push_back(parse_expr());
                            if (!match(TT::COMMA)) break;
                        }
                        expect(TT::RPAREN, ")");
                        expr = std::move(mc);
                    } else {
                        expr = std::make_unique<DotAccess>(std::move(expr), prop, ln);
                    }
                } else if (check(TT::LBRACKET)) {
                    
                    advance();
                    auto key = parse_expr();
                    expect(TT::RBRACKET, "]");
                    expr = std::make_unique<IndexExpr>(std::move(expr), std::move(key), ln);
                } else {
                    break;
                }
            }
            return expr;
        }
        
        if (check(TT::NEW)) {
            advance();
            std::string cls = expect(TT::IDENT, "?�래???�름").value;
            auto ne = std::make_unique<NewExpr>(cls, ln);
            if (match(TT::LPAREN)) {
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    ne->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
            }
            return ne;
        }

        
        if (check(TT::LBRACE)) {
            advance(); 
            auto dict = std::make_unique<DictLit>(ln);
            while (!check(TT::RBRACE) && !check(TT::EOF_T)) {
                std::string key;
                if (check(TT::STR))   key = advance().value;
                else if (check(TT::IDENT)) key = advance().value;
                else throw ParseError("딕셔너리 키는 문자열 또는 식별자여야 합니다.", peek().line);
                expect(TT::COLON, ":");
                auto val = parse_expr();
                dict->key_order.push_back(key);
                dict->pairs[key] = std::move(val);
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACE, "}");
            return dict;
        }

        
        if (check(TT::LBRACKET)) {
            advance(); 
            auto arr = std::make_unique<ArrayLit>(ln);
            while (!check(TT::RBRACKET) && !check(TT::EOF_T)) {
                arr->elements.push_back(parse_expr());
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACKET, "]");
            return arr;
        }

        
        if (check(TT::SUPER)) {
            advance();
            expect(TT::DOT, ".");
            std::string method = expect(TT::IDENT, "메서?�명").value;
            auto sc = std::make_unique<SuperCallExpr>(method, ln);
            if (match(TT::LPAREN)) {
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    sc->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
            }
            return sc;
        }


        if (peek().type == TT::FUNC) {
            advance(); 
            expect(TT::LPAREN, "(");
            std::vector<std::string> params;
            std::vector<ExprPtr> defaults;
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "매개변?�명").value);
                ExprPtr def;
                if (check(TT::IS)) { advance(); def = parse_expr(); }
                defaults.push_back(std::move(def));
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
            expect(TT::DO, "do");
            match(TT::NEWLINE); 
            auto body = parse_block();
            expect(TT::END, "end");
            auto fe = std::make_unique<FuncExpr>(ln);
            fe->params   = std::move(params);
            fe->defaults = std::move(defaults);
            fe->body     = std::move(body);
            return std::move(fe);
        }

        
        if (check(TT::LPAREN)) {
            advance();
            auto expr = parse_expr();
            expect(TT::RPAREN, ")");
            return expr;
        }

        throw ParseError("?�현???�요 ???�??'" + peek().value + "' ?�음", ln);
    }

    
    
    

    
    BlockPtr parse_block() {
        auto blk = std::make_unique<SuraBlock>(peek().line);
        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)
            && !check(TT::ELSE) && !check(TT::ELIF) && !check(TT::CATCH) && !check(TT::FINALLY)) {
            blk->body.push_back(parse_stmt());
            skip_newlines();
        }
        return blk;
    }


        StmtPtr parse_if() {
        int ln = peek().line;
        expect(TT::IF, "if");
        auto cond = parse_expr();
        expect(TT::THEN, "then");

        
        if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
            auto inline_body = parse_stmt_body(ln); 
            auto then_block = std::make_unique<SuraBlock>(ln);
            then_block->body.push_back(std::move(inline_body));
            return std::make_unique<IfStmt>(std::move(cond), std::move(then_block), nullptr, ln);
        }

        
        eat_newline();
        auto then_block = parse_block();

        
        struct ElifClause { ExprPtr cond; BlockPtr body; int ln; };
        std::vector<ElifClause> elifs;
        while (check(TT::ELIF)) {
            int eln = peek().line; advance();
            auto econd = parse_expr();
            expect(TT::THEN, "then");
            eat_newline();
            auto ebody = parse_block();
            elifs.push_back({std::move(econd), std::move(ebody), eln});
        }

        BlockPtr else_block;
        if (check(TT::ELSE)) {
            advance(); eat_newline();
            else_block = parse_block();
        }
        expect(TT::END, "end");
        eat_newline();


        BlockPtr cur_else = std::move(else_block);
        for (int i = (int)elifs.size() - 1; i >= 0; --i) {
            auto inner = std::make_unique<IfStmt>(
                std::move(elifs[i].cond), std::move(elifs[i].body),
                std::move(cur_else), elifs[i].ln);
            cur_else = std::make_unique<SuraBlock>(elifs[i].ln);
            cur_else->body.push_back(std::move(inner));
        }

        return std::make_unique<IfStmt>(
            std::move(cond), std::move(then_block), std::move(cur_else), ln);
    }

    
    StmtPtr parse_while() {
        int ln = peek().line;
        expect(TT::WHILE, "while");
        auto cond = parse_expr();
        expect(TT::DO, "do");
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body), ln);
    }

    
    StmtPtr parse_repeat() {
        int ln = peek().line;
        expect(TT::REPEAT, "repeat");
        auto count = parse_expr();
        match(TT::DO); 
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<RepeatStmt>(std::move(count), std::move(body), ln);
    }

    
    
    
    StmtPtr parse_for() {
        int ln = peek().line;
        expect(TT::FOR, "for");
        std::string var = expect(TT::IDENT, "루프 변수").value;
        std::string var2;
        if (check(TT::COMMA)) {
            advance();
            var2 = expect(TT::IDENT, "루프 변??2").value;
        }
        expect(TT::IN, "in");
        auto first = parse_expr();

        
        if (check(TT::DO) || check(TT::NEWLINE) || check(TT::EOF_T)) {
            match(TT::DO);
            eat_newline();
            auto body = parse_block();
            expect(TT::END, "end");
            eat_newline();
            return std::make_unique<ForeachStmt>(
                std::move(var), std::move(var2), std::move(first), std::move(body), ln);
        }

        
        if (!match(TT::TO) && !match(TT::TILDE))
            throw ParseError("'to', '~', ?�는 'do' ?�요", peek().line);
        auto end = parse_expr();
        
        ExprPtr step;
        if (peek().type == TT::IDENT && peek().value == "step") {
            advance();
            step = parse_expr();
        }
        match(TT::DO); 
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<ForStmt>(
            std::move(var), std::move(first), std::move(end), std::move(step), std::move(body), ln);
    }

    
    StmtPtr parse_try() {
        int ln = peek().line;
        expect(TT::TRY, "try");
        eat_newline();
        auto try_block = parse_block();          
        expect(TT::CATCH, "catch");
        std::string catch_var = expect(TT::IDENT, "?�류 변?�명").value;
        eat_newline();
        auto catch_block = parse_block();        

        BlockPtr finally_block;
        if (match(TT::FINALLY)) {
            expect(TT::DO, "do");
            eat_newline();
            finally_block = parse_block();       
        }
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<TryStmt>(
            std::move(try_block), std::move(catch_var),
            std::move(catch_block), std::move(finally_block), ln);
    }
    
    
    StmtPtr parse_class() {
        int ln = peek().line;
        expect(TT::CLASS, "class");
        std::string name = expect(TT::IDENT, "?�래???�름").value;
        std::string parent;
        if (match(TT::EXTENDS))
            parent = expect(TT::IDENT, "부�??�래???�름").value;
        expect(TT::DO, "do");
        eat_newline();

        auto cd = std::make_unique<ClassDef>(std::move(name), std::move(parent), ln);

        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)) {
            if (check(TT::FUNC)) {
                
                advance(); 
                std::string mname = expect(TT::IDENT, "메서???�름").value;
                std::vector<std::string> params;
                std::vector<ExprPtr>     defaults;
                if (match(TT::LPAREN)) {
                    while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                        params.push_back(expect(TT::IDENT, "매개변?�명").value);
                        
                        if (check(TT::COLON)) { advance(); parse_type_annot(); } 
                        ExprPtr def;
                        if (check(TT::IS)) { advance(); def = parse_expr(); }
                        defaults.push_back(std::move(def));
                        if (!match(TT::COMMA)) break;
                    }
                    expect(TT::RPAREN, ")");
                }
                
                if (check(TT::ARROW)) { advance(); parse_type_annot(); } 
                expect(TT::DO, "do");
                eat_newline();
                auto mbody = parse_block();
                expect(TT::END, "end");
                eat_newline();
                cd->add_method(mname, std::move(params), std::move(defaults), std::move(mbody));
            } else if (check(TT::IDENT)) {
                
                std::string fname = advance().value;
                expect(TT::IS, "is");
                auto fval = parse_expr();
                eat_newline();
                cd->add_field(std::move(fname), std::move(fval));
            } else {
                throw ParseError("?�래??바디??func ?�는 ?�드 ?�의 ?�요 ???�??'" + peek().value + "'", peek().line);
            }
            skip_newlines();
        }

        expect(TT::END, "end");
        eat_newline();
        return cd;
    }

    
    StmtPtr parse_func() {
        int ln = peek().line;
        expect(TT::FUNC, "func");
        std::string name = expect(TT::IDENT, "?�수 ?�름").value;

        
        
        std::vector<std::string> params;
        std::vector<ExprPtr>     defaults;
        std::vector<TypeAnnot>   param_types;
        if (match(TT::LPAREN)) {
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "매개변?�명").value);
                
                TypeAnnot pta;
                if (check(TT::COLON)) { advance(); pta = parse_type_annot(); }
                param_types.push_back(pta);
                ExprPtr def;
                if (check(TT::IS)) { advance(); def = parse_expr(); }
                defaults.push_back(std::move(def));
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
        }

        
        TypeAnnot return_type;
        if (check(TT::ARROW)) { advance(); return_type = parse_type_annot(); }

        expect(TT::DO, "do");
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();

        auto fd = std::make_unique<FuncDef>(std::move(name), std::move(body), ln);
        fd->params       = std::move(params);
        fd->defaults     = std::move(defaults);
        fd->param_types  = std::move(param_types);
        fd->return_type  = return_type;
        return fd;
    }

    
    
    StmtPtr parse_cmd(const std::string& cmd_name, int ln) {
        
        static const std::vector<std::string> BUILTINS = {"print", "print_n", "input", "exit", "clock", "type"};
        static const std::vector<std::string> KWS = {"is", "if", "then", "else", "end", "while", "do", "func", "repeat", "break", "return", "use", "class", "try"};
        
        bool is_builtin = std::find(BUILTINS.begin(), BUILTINS.end(), cmd_name) != BUILTINS.end();
        if (!is_builtin) {
            std::string sug = sura_suggest(cmd_name, BUILTINS);
            if (sug.empty()) sug = sura_suggest(cmd_name, KWS);
            
            if (!sug.empty()) {
                
                
                if (cmd_name != sug) {
                     
                     
                     
                     
                }
            }
        }

        auto stmt = std::make_unique<CmdStmt>(cmd_name, ln);
        if (cmd_name == "random") {
            stmt->args.push_back(parse_expr());            
            expect(TT::TILDE, "~");
            stmt->args.push_back(parse_expr());            
            stmt->args.push_back(parse_expr());            
        } else {
            while (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                if (check(TT::COMMA)) { advance(); continue; }
                stmt->args.push_back(parse_expr());
            }
        }
        eat_newline();
        return stmt;
    }

    
    
    
    
    
    
    
    StmtPtr parse_ident_stmt(int ln) {
        std::string name = advance().value; 

        
        
        if (check(TT::DOT)) {
            advance(); 
            std::string prop = expect(TT::IDENT, "?�성�??�는 메서?�명").value;

            
            if (check(TT::IS)) {
                advance(); 
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<DotAssignStmt>(name, prop, std::move(val), ln);
            }

            
            if (check(TT::LPAREN)) {
                advance(); 
                auto mc = std::make_unique<MethodCallExpr>(
                    std::make_unique<Ident>(name, ln), prop, ln);
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    mc->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                eat_newline();
                return std::make_unique<ExprStmt>(std::move(mc), ln);
            }

            throw ParseError("???�에 'is' ?�는 '(' ?�요", peek().line);
        }

        
        if (check(TT::LBRACKET)) {
            advance(); 
            auto key = parse_expr();
            expect(TT::RBRACKET, "]");
            expect(TT::IS, "is");
            auto val = parse_expr();
            eat_newline();
            return std::make_unique<IndexAssignStmt>(name, std::move(key), std::move(val), ln);
        }


        if (check(TT::COLON)) {
            advance(); 
            TypeAnnot ta = parse_type_annot();
            expect(TT::IS, "is");
            auto val = parse_expr();
            eat_newline();
            auto stmt = std::make_unique<AssignStmt>(name, std::move(val), ln);
            stmt->type_annot = ta;
            return stmt;
        }

        if (check(TT::IS)) {
            advance(); 
            auto val = parse_expr();
            eat_newline();
            return std::make_unique<AssignStmt>(name, std::move(val), ln);
        }

        
        {
            TT t = peek().type;
            std::string op;
            if      (t == TT::PLUS_EQ)    op = "+";
            else if (t == TT::MINUS_EQ)   op = "-";
            else if (t == TT::STAR_EQ)    op = "*";
            else if (t == TT::SLASH_EQ)   op = "/";
            else if (t == TT::PERCENT_EQ) op = "%";
            if (!op.empty()) {
                advance();
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<InplaceStmt>(name, op, std::move(val), ln);
            }
        }

        
        if (check(TT::LPAREN)) {
            advance(); 
            auto ce = std::make_unique<CallExpr>(name, ln);
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                ce->args.push_back(parse_expr());
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
            eat_newline();
            return std::make_unique<ExprStmt>(std::move(ce), ln);
        }

        
        return parse_cmd(name, ln);
    }

    
    StmtPtr parse_stmt_body(int ln) {
        switch (peek().type) {
            case TT::IF:     return parse_if();
            case TT::WHILE:  return parse_while();
            case TT::REPEAT: return parse_repeat();
            case TT::FOR:    return parse_for();
            case TT::TRY:    return parse_try();
            case TT::CLASS:  return parse_class();
            case TT::FUNC:   return parse_func();
            case TT::BREAK:    { advance(); eat_newline(); return std::make_unique<BreakStmt>(ln); }
            case TT::CONTINUE: { advance(); eat_newline(); return std::make_unique<ContinueStmt>(ln); }
            case TT::THROW: {
                advance();
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<ThrowStmt>(std::move(val), ln);
            }
            case TT::RETURN: {
                advance();
                ExprPtr val;
                if (!check(TT::NEWLINE) && !check(TT::EOF_T)) val = parse_expr();
                eat_newline();
                return std::make_unique<ReturnStmt>(std::move(val), ln);
            }
            case TT::USE: {
                advance();
                std::string lib = expect(TT::IDENT, "?�이브러리명").value;
                eat_newline();
                return std::make_unique<UseStmt>(std::move(lib), ln);
            }
            case TT::IDENT:
                return parse_ident_stmt(ln);
            case TT::SUPER: {
                
                advance(); 
                expect(TT::DOT, ".");
                std::string method = expect(TT::IDENT, "메서?�명").value;
                auto sc = std::make_unique<SuperCallExpr>(method, ln);
                if (match(TT::LPAREN)) {
                    while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                        sc->args.push_back(parse_expr());
                        if (!match(TT::COMMA)) break;
                    }
                    expect(TT::RPAREN, ")");
                }
                eat_newline();
                return std::make_unique<ExprStmt>(std::move(sc), ln);
            }
            default: {
                
                TT t = peek().type;
                if (t == TT::NUM || t == TT::STR || t == TT::TRUE || t == TT::FALSE
                    || t == TT::LPAREN || t == TT::MINUS || t == TT::NOT
                    || t == TT::LBRACKET) {
                    auto expr = parse_expr();
                    eat_newline();
                    return std::make_unique<ExprStmt>(std::move(expr), ln);
                }
                throw ParseError("문장 ?�싱 ?�패 ??'" + peek().value + "'", ln);
            }
        }
    }

    
    StmtPtr parse_stmt() {
        skip_newlines();
        int ln = peek().line;
        return parse_stmt_body(ln);
    }

public:
    
    BlockPtr parse(const std::vector<Token>& toks) {
        tokens = toks;
        pos    = 0;
        skip_newlines();
        auto program = std::make_unique<SuraBlock>(0);
        while (!check(TT::EOF_T)) {
            program->body.push_back(parse_stmt());
            skip_newlines();
        }
        return program;
    }

    
    BlockPtr parse_source(const std::string& source) {
        Lexer lexer;
        return parse(lexer.tokenize(source));
    }
};
