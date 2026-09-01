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
        : std::runtime_error("[Sura Parse Error] line " + std::to_string(ln) + ": " + msg + (det.empty() ? "" : " (" + det + ")"))
        , line(ln), details(det) {}
};

struct ParseDiagnostic {
    int line = 0;
    std::string message;
    std::string details;
};

class Parser {
    std::vector<Token> tokens;
    size_t             pos = 0;
    bool               allow_legacy_command_syntax = true;
    static constexpr size_t MAX_NESTING_DEPTH = 512;
    size_t nesting_depth = 0;

    struct NestingGuard {
        size_t& depth;

        NestingGuard(size_t& active_depth, int line) : depth(active_depth) {
            if (depth >= MAX_NESTING_DEPTH) {
                throw ParseError(
                    "parser nesting limit exceeded (maximum " +
                    std::to_string(MAX_NESTING_DEPTH) + ")",
                    line);
            }
            ++depth;
        }

        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;
        ~NestingGuard() { --depth; }
    };

    
    std::vector<std::string> get_all_keywords() {
        return {
            "is", "if", "then", "else", "end", "while", "do", "func", "repeat", 
            "break", "return", "and", "or", "not", "true", "false", "nil", "use", 
            "continue", "for", "in", "to", "try", "catch", "throw", "class", 
            "new", "extends", "super", "elif", "finally", "match", "when",
            "enum", "struct", "import", "global",
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
            std::string msg = "expected '" + what + "' but got '" + peek().type_str() + "'";
            if (peek().type == TT::IDENT) {
                std::string sug = sura_suggest(peek().value, get_all_keywords());
                if (!sug.empty() && peek().value != sug)
                    msg += "\n  did you mean '" + sug + "'?";
            }
            throw ParseError(msg, peek().line, peek().value);
        }
        return advance();
    }

    void skip_newlines() { while (check(TT::NEWLINE)) advance(); }

    void synchronize_after_error() {
        while (!check(TT::EOF_T)) {
            if (match(TT::NEWLINE)) {
                skip_newlines();
                return;
            }
            advance();
        }
    }

    
    
    
    
    TypeAnnot parse_type_annot() {
        TypeAnnot ta;
        ta.present = true;
        if (!check(TT::IDENT))
            throw ParseError("syntax error", peek().line);
        std::string tname = advance().value;
        ta.source_name = tname;
        // A pointer annotation keeps its pointee spelling as frontend
        // metadata while remaining a scalar NUMBER in hosted Sura:
        //     device: ptr[PciHeader]
        // The freestanding backend uses the metadata for checked field
        // offsets and width-correct loads/stores.
        if (tname == "ptr" && match(TT::LBRACKET)) {
            const std::string pointee = expect(TT::IDENT, "type name").value;
            expect(TT::RBRACKET, "]");
            ta.source_name = "ptr[" + pointee + "]";
        }
        // Sura stores all scalar numeric values in one NUMBER runtime
        // representation. Keep familiar spelling aliases semantic aliases too;
        // treating `int` as a class made valid programs fail strict checking.
        if      (tname == "number" || tname == "int" || tname == "float" ||
                 tname == "double" || tname == "i8" || tname == "u8" ||
                 tname == "i16" || tname == "u16" || tname == "i32" ||
                 tname == "u32" || tname == "i64" || tname == "u64" ||
                 tname == "isize" || tname == "usize" || tname == "ptr" ||
                 tname == "숫자")   ta.kind = SType::NUMBER;
        else if (tname == "string" || tname == "문자열") ta.kind = SType::STRING;
        else if (tname == "bool"   || tname == "불리언" || tname == "논리") ta.kind = SType::BOOL;
        else if (tname == "array"  || tname == "배열")   ta.kind = SType::ARRAY;
        else if (tname == "dict"   || tname == "사전")   ta.kind = SType::DICT;
        else if (tname == "nil"    || tname == "없음")   ta.kind = SType::NIL;
        else if (tname == "any"    || tname == "아무")   ta.kind = SType::ANY;
        else {
            ta.kind = SType::CLASS;
            ta.class_name = tname;
        }
        return ta;
    }

    
    void eat_newline() {
        if (check(TT::EOF_T)) return;
        
        if (check(TT::END) || check(TT::ELSE) || check(TT::ELIF) || check(TT::CATCH) || check(TT::FINALLY)) return;
        if (!match(TT::NEWLINE))
            throw ParseError("syntax error", peek().line);
    }

    
    
    

    // `|` is both the bitwise-OR operator and the closing delimiter of the
    // compact lambda syntax.  Only the outer expression of a lambda default
    // stops at PIPE; nested expressions in parentheses, calls, arrays, and
    // dictionaries call parse_expr() with the normal default and can still
    // use bitwise OR.
    ExprPtr parse_expr(bool stop_at_pipe = false) {
        NestingGuard guard(nesting_depth, peek().line);
        return parse_coalesce(stop_at_pipe);
    }

    ExprPtr parse_coalesce(bool stop_at_pipe = false) {
        auto left = parse_ternary(stop_at_pipe);
        if (check(TT::NULL_COALESCE)) {
            int ln = peek().line; advance();
            NestingGuard guard(nesting_depth, ln);
            auto right = parse_coalesce(stop_at_pipe);
            return std::make_unique<BinOp>("??", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_ternary(bool stop_at_pipe = false) {
        auto left = parse_or(stop_at_pipe);
        if (check(TT::QUESTION)) {
            int ln = peek().line; advance();
            auto then_val = parse_expr(stop_at_pipe);
            expect(TT::COLON, ":");
            NestingGuard guard(nesting_depth, ln);
            auto else_val = parse_ternary(stop_at_pipe);
            return std::make_unique<TernaryExpr>(
                std::move(left), std::move(then_val), std::move(else_val), ln);
        }
        return left;
    }

    
    ExprPtr parse_or(bool stop_at_pipe = false) {
        auto left = parse_and(stop_at_pipe);
        while (check(TT::OR)) {
            int ln = peek().line; advance();
            auto right = parse_and(stop_at_pipe);
            left = std::make_unique<BinOp>("or", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_and(bool stop_at_pipe = false) {
        auto left = parse_not(stop_at_pipe);
        while (check(TT::AND)) {
            int ln = peek().line; advance();
            auto right = parse_not(stop_at_pipe);
            left = std::make_unique<BinOp>("and", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_not(bool stop_at_pipe = false) {
        if (check(TT::NOT)) {
            int ln = peek().line; advance();
            NestingGuard guard(nesting_depth, ln);
            return std::make_unique<UnaryOp>("not", parse_not(stop_at_pipe), ln);
        }
        return parse_compare(stop_at_pipe);
    }

    
    ExprPtr parse_compare(bool stop_at_pipe = false) {
        auto left = parse_bitor(stop_at_pipe);
        TT t = peek().type;
        if (t==TT::EQ||t==TT::NEQ||t==TT::GT||t==TT::LT||t==TT::GTE||t==TT::LTE) {
            int ln = peek().line;
            std::string op = advance().value;
            auto right = parse_bitor(stop_at_pipe);
            return std::make_unique<BinOp>(op, std::move(left), std::move(right), ln);
        }
        if (t == TT::IN) {
            int ln = peek().line; advance();
            auto right = parse_bitor(stop_at_pipe);
            return std::make_unique<BinOp>("in", std::move(left), std::move(right), ln);
        }
        return left;
    }

    
    ExprPtr parse_bitor(bool stop_at_pipe = false) {
        auto left = parse_bitxor();
        while (!stop_at_pipe && check(TT::PIPE)) {
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
            NestingGuard guard(nesting_depth, ln);
            return std::make_unique<UnaryOp>("-", parse_unary(), ln);
        }
        if (check(TT::TILDE)) {
            int ln = peek().line; advance();
            NestingGuard guard(nesting_depth, ln);
            return std::make_unique<UnaryOp>("~", parse_unary(), ln);
        }
        return parse_primary();
    }

    
    ExprPtr parse_fstr(const Token& tok) {
        return std::make_unique<StrLit>(tok.value, tok.line);
    }

    std::string parse_member_name() {
        switch (peek().type) {
            case TT::IDENT:
            case TT::IS:
            case TT::IF:
            case TT::THEN:
            case TT::ELSE:
            case TT::END:
            case TT::WHILE:
            case TT::DO:
            case TT::FUNC:
            case TT::REPEAT:
            case TT::BREAK:
            case TT::RETURN:
            case TT::AND:
            case TT::OR:
            case TT::NOT:
            case TT::TRUE:
            case TT::FALSE:
            case TT::NIL:
            case TT::USE:
            case TT::CONTINUE:
            case TT::FOR:
            case TT::IN:
            case TT::TO:
            case TT::TRY:
            case TT::CATCH:
            case TT::THROW:
            case TT::CLASS:
            case TT::NEW:
            case TT::EXTENDS:
            case TT::SUPER:
            case TT::ELIF:
            case TT::FINALLY:
            case TT::MATCH:
            case TT::WHEN:
            case TT::ENUM:
            case TT::STRUCT:
            case TT::IMPORT:
                return advance().value;
            default:
                break;
        }
        throw ParseError("unexpected token '" + peek().value + "'", peek().line);
    }

    ExprPtr parse_postfix(ExprPtr expr, int ln) {
        while (true) {
            bool optional = false;
            if (check(TT::OPTIONAL_DOT)) {
                optional = true;
                advance();
            } else if (check(TT::DOT)) {
                advance();
            } else if (check(TT::LBRACKET)) {
                advance();
                auto key = parse_expr();
                expect(TT::RBRACKET, "]");
                expr = std::make_unique<IndexExpr>(std::move(expr), std::move(key), ln);
                continue;
            } else {
                break;
            }

            std::string prop = parse_member_name();
            if (check(TT::LPAREN)) {
                advance();
                auto mc = std::make_unique<MethodCallExpr>(std::move(expr), prop, ln, optional);
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    mc->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                expr = std::move(mc);
            } else {
                expr = std::make_unique<DotAccess>(std::move(expr), prop, ln, optional);
            }
        }
        return expr;
    }

    std::unique_ptr<DictLit> parse_dict_literal(int ln) {
        expect(TT::LBRACE, "{");
        auto dict = std::make_unique<DictLit>(ln);
        while (!check(TT::RBRACE) && !check(TT::EOF_T)) {
            std::string key;
            if (check(TT::STR))   key = advance().value;
            else if (check(TT::IDENT)) key = advance().value;
            else throw ParseError("dict key must be a string or identifier", peek().line);
            expect(TT::COLON, ":");
            auto val = parse_expr();
            dict->key_order.push_back(key);
            dict->pairs[key] = std::move(val);
            if (!match(TT::COMMA)) break;
        }
        expect(TT::RBRACE, "}");
        return dict;
    }

    ExprPtr make_tool_call_from_spec(std::unique_ptr<DictLit> spec, int ln) {
        auto call = std::make_unique<CallExpr>("tool_call", ln);
        call->args.push_back(std::move(spec));
        return parse_postfix(std::move(call), ln);
    }

    ExprPtr parse_primary() {
        int ln = peek().line;

        
        if (check(TT::NUM)) {
            double v = std::stod(peek().value);
            advance();
            return parse_postfix(std::make_unique<NumLit>(v, ln), ln);
        }
        
        if (check(TT::STR)) {
            std::string v = advance().value;
            return parse_postfix(std::make_unique<StrLit>(std::move(v), ln), ln);
        }
        
        if (check(TT::FSTR)) {
            Token tok = advance();
            return parse_postfix(parse_fstr(tok), ln);
        }
        
        if (check(TT::TRUE))  { advance(); return parse_postfix(std::make_unique<BoolLit>(true,  ln), ln); }
        if (check(TT::FALSE)) { advance(); return parse_postfix(std::make_unique<BoolLit>(false, ln), ln); }
        if (check(TT::NIL))   { advance(); return parse_postfix(std::make_unique<NilLit>(ln), ln); }


        if (check(TT::IDENT)) {
            std::string name = advance().value;

            if (name == "tool" && (check(TT::IDENT) || check(TT::STR)) && peek(1).type == TT::LBRACE) {
                std::string tool_name = advance().value;
                auto spec = parse_dict_literal(ln);
                if (spec->pairs.count("name")) {
                    throw ParseError("tool <name> { ... } already supplies a name field", ln);
                }
                spec->key_order.insert(spec->key_order.begin(), "name");
                spec->pairs["name"] = std::make_unique<StrLit>(tool_name, ln);
                return make_tool_call_from_spec(std::move(spec), ln);
            }

            if (name == "tool" && check(TT::LBRACE)) {
                return make_tool_call_from_spec(parse_dict_literal(ln), ln);
            }

            
            if (check(TT::LPAREN)) {
                advance(); 
                auto call = std::make_unique<CallExpr>(name, ln);
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    call->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                return parse_postfix(std::move(call), ln);
            }

            
            ExprPtr expr = std::make_unique<Ident>(name, ln);
            return parse_postfix(std::move(expr), ln);
            while (true) {
                if (check(TT::DOT)) {
                    advance();
                    std::string prop = expect(TT::IDENT, "identifier").value;
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
            std::string cls = expect(TT::IDENT, "identifier").value;
            auto ne = std::make_unique<NewExpr>(cls, ln);
            if (match(TT::LPAREN)) {
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    ne->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
            }
            return parse_postfix(std::move(ne), ln);
        }

        
        if (check(TT::LBRACE)) {
            return parse_postfix(parse_dict_literal(ln), ln);
            advance(); 
            auto dict = std::make_unique<DictLit>(ln);
            while (!check(TT::RBRACE) && !check(TT::EOF_T)) {
                std::string key;
                if (check(TT::STR))   key = advance().value;
                else if (check(TT::IDENT)) key = advance().value;
                else throw ParseError("dict key must be a string or identifier", peek().line);
                expect(TT::COLON, ":");
                auto val = parse_expr();
                dict->key_order.push_back(key);
                dict->pairs[key] = std::move(val);
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACE, "}");
            return parse_postfix(std::move(dict), ln);
        }

        
        if (check(TT::LBRACKET)) {
            advance(); 
            auto arr = std::make_unique<ArrayLit>(ln);
            while (!check(TT::RBRACKET) && !check(TT::EOF_T)) {
                arr->elements.push_back(parse_expr());
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACKET, "]");
            return parse_postfix(std::move(arr), ln);
        }

        
        if (check(TT::SUPER)) {
            advance();
            expect(TT::DOT, ".");
            std::string method = expect(TT::IDENT, "identifier").value;
            auto sc = std::make_unique<SuperCallExpr>(method, ln);
            if (match(TT::LPAREN)) {
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    sc->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
            }
            return parse_postfix(std::move(sc), ln);
        }


        // Lambda: |x, y| expr  or  || expr  (empty params)
        if (check(TT::PIPE)) {
            advance(); // consume first |
            std::vector<std::string> params;
            std::vector<ExprPtr> defaults;
            if (!check(TT::PIPE)) {
                while (!check(TT::PIPE) && !check(TT::EOF_T)) {
                    params.push_back(expect(TT::IDENT, "identifier").value);
                    ExprPtr def;
                    if (check(TT::IS)) {
                        advance();
                        def = parse_expr(true);
                    }
                    defaults.push_back(std::move(def));
                    if (!match(TT::COMMA)) break;
                }
            }
            expect(TT::PIPE, "|");
            int bln = peek().line;
            auto body_expr = parse_expr();
            auto ret_stmt  = std::make_unique<ReturnStmt>(std::move(body_expr), bln);
            auto body_blk  = std::make_unique<SuraBlock>(bln);
            body_blk->body.push_back(std::move(ret_stmt));
            auto fe = std::make_unique<FuncExpr>(ln);
            fe->params   = std::move(params);
            fe->defaults = std::move(defaults);
            fe->body     = std::move(body_blk);
            return parse_postfix(std::move(fe), ln);
        }

        if (peek().type == TT::FUNC) {
            advance();
            expect(TT::LPAREN, "(");
            std::vector<std::string> params;
            std::vector<ExprPtr> defaults;
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "identifier").value);
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
            return parse_postfix(std::move(fe), ln);
        }

        
        if (check(TT::LPAREN)) {
            advance();
            auto expr = parse_expr();
            expect(TT::RPAREN, ")");
            return parse_postfix(std::move(expr), ln);
        }

        throw ParseError("expected expression but got '" + peek().value + "'", ln);
    }

    
    
    

    
    BlockPtr parse_block() {
        auto blk = std::make_unique<SuraBlock>(peek().line);
        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)
            && !check(TT::ELSE) && !check(TT::ELIF) && !check(TT::CATCH) && !check(TT::FINALLY)
            && !check(TT::WHEN)) {
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

            BlockPtr else_block;
            if (check(TT::ELSE)) {
                int else_ln = peek().line;
                advance();
                match(TT::THEN);
                else_block = std::make_unique<SuraBlock>(else_ln);
                if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                    else_block->body.push_back(parse_stmt_body(else_ln));
                } else {
                    eat_newline();
                    else_block = parse_block();
                    expect(TT::END, "end");
                    eat_newline();
                }
            }

            return std::make_unique<IfStmt>(std::move(cond), std::move(then_block), std::move(else_block), ln);
        }

        
        eat_newline();
        auto then_block = parse_block();

        
        struct ElifClause { ExprPtr cond; BlockPtr body; int ln; };
        std::vector<ElifClause> elifs;
        auto parse_elif_clause = [&](bool else_if_alias) {
            int eln = peek().line;
            if (else_if_alias) {
                advance();
                eln = peek().line;
                expect(TT::IF, "if");
            } else {
                expect(TT::ELIF, "elif");
            }
            auto econd = parse_expr();
            expect(TT::THEN, "then");
            BlockPtr ebody;
            if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                auto inline_body = parse_stmt_body(eln);
                ebody = std::make_unique<SuraBlock>(eln);
                ebody->body.push_back(std::move(inline_body));
            } else {
                eat_newline();
                ebody = parse_block();
            }
            elifs.push_back({std::move(econd), std::move(ebody), eln});
        };
        while (check(TT::ELIF)) {
            parse_elif_clause(false);
        }
        while (check(TT::ELSE) && peek(1).type == TT::IF) {
            parse_elif_clause(true);
            while (check(TT::ELIF)) {
                parse_elif_clause(false);
            }
        }

        BlockPtr else_block;
        if (check(TT::ELSE)) {
            advance();
            if (match(TT::THEN)) {
                else_block = std::make_unique<SuraBlock>(peek().line);
                if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                    auto inline_body = parse_stmt_body(ln);
                    else_block->body.push_back(std::move(inline_body));
                } else {
                    eat_newline();
                    else_block = parse_block();
                }
            } else {
                eat_newline();
                else_block = parse_block();
            }
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
        std::string var = expect(TT::IDENT, "identifier").value;
        std::string var2;
        if (check(TT::COMMA)) {
            advance();
            var2 = expect(TT::IDENT, "identifier").value;
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
            throw ParseError("expected 'to', '~', or 'do'", peek().line);
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

    
    // struct Name do
    //     field [is <default>]
    //     ...
    //     func method(...) do ... end
    // end
    //
    // Compiles to a class under the hood, but with an auto-generated
    // positional constructor `init` that fills fields in declaration order.
    // Struct values can be created without `new`:  Vec2(3, 4)
    StmtPtr parse_struct() {
        int ln = peek().line;
        expect(TT::STRUCT, "struct");
        std::string name = expect(TT::IDENT, "identifier").value;
        bool packed_layout = false;
        if (check(TT::IDENT) && peek().value == "packed") {
            advance();
            packed_layout = true;
        }
        expect(TT::DO, "do");
        eat_newline();
        skip_newlines();

        auto cd = std::make_unique<ClassDef>(std::move(name), std::string(), ln);
        cd->value_struct = true;
        cd->packed_layout = packed_layout;
        std::vector<std::string> field_order; // preserve declaration order for init
        bool user_init_present = false;

        while (!check(TT::EOF_T) && !check(TT::END)) {
            if (check(TT::FUNC)) {
                advance();
                std::string mname = expect(TT::IDENT, "identifier").value;
                if (mname == "init") user_init_present = true;
                expect(TT::LPAREN, "(");
                std::vector<std::string> params;
                std::vector<ExprPtr> defaults;
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    params.push_back(expect(TT::IDENT, "identifier").value);
                    ExprPtr d;
                    if (match(TT::IS)) d = parse_expr();
                    defaults.push_back(std::move(d));
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                expect(TT::DO, "do");
                eat_newline();
                auto body = parse_block();
                expect(TT::END, "end");
                eat_newline();
                cd->add_method(mname, std::move(params), std::move(defaults), std::move(body));
            } else {
                // Field: `NAME [: TYPE] [is default]`
                std::string fname = expect(TT::IDENT, "identifier").value;
                TypeAnnot field_type;
                if (match(TT::COLON)) field_type = parse_type_annot();
                ExprPtr def;
                bool explicit_default = match(TT::IS);
                if (explicit_default) def = parse_expr();
                else def = std::make_unique<NilLit>(ln);
                field_order.push_back(fname);
                cd->add_field(fname, std::move(def), explicit_default,
                              std::move(field_type));
                if (check(TT::COMMA)) advance();
                eat_newline();
            }
            skip_newlines();
        }
        expect(TT::END, "end");
        eat_newline();

        // Auto-generate positional `init(f1, f2, ...)` unless the user wrote one.
        if (!user_init_present && !field_order.empty()) {
            auto body = std::make_unique<SuraBlock>(ln);
            std::vector<ExprPtr> defaults;
            for (auto& f : field_order) {
                // self.<f> is <f>   (parameter named after field)
                auto assign = std::make_unique<DotAssignStmt>(
                    std::string("self"), f,
                    std::make_unique<Ident>(f, ln), ln);
                body->body.push_back(std::move(assign));
                defaults.push_back(nullptr);
            }
            cd->add_method("init", field_order, std::move(defaults), std::move(body), true);
        }
        return cd;
    }

    // import "relative/or/absolute/path.sura"
    // Handled by the compiler: it loads & compiles the file into the same chunk.
    // The compiler's module cache prevents re-import and detects cycles.
    StmtPtr parse_import() {
        int ln = peek().line;
        expect(TT::IMPORT, "import");
        Token s = advance();
        if (s.type != TT::STR) throw ParseError("import requires a string path", ln);
        eat_newline();
        return std::make_unique<ImportStmt>(s.value, ln);
    }

    StmtPtr parse_global_decl() {
        int ln = peek().line;
        expect(TT::GLOBAL, "global");
        std::vector<std::string> names;
        while (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
            names.push_back(expect(TT::IDENT, "identifier").value);
            if (!match(TT::COMMA)) break;
        }
        if (names.empty()) throw ParseError("global requires at least one identifier", ln);
        eat_newline();
        return std::make_unique<GlobalDeclStmt>(std::move(names), ln);
    }

    // enum Name do
    //     MEMBER1
    //     MEMBER2 is <expr>
    //     ...
    // end
    StmtPtr parse_enum() {
        int ln = peek().line;
        expect(TT::ENUM, "enum");
        std::string name = expect(TT::IDENT, "identifier").value;
        expect(TT::DO, "do");
        eat_newline();
        skip_newlines();

        auto ed = std::make_unique<EnumDef>(std::move(name), ln);

        while (!check(TT::END) && !check(TT::EOF_T)) {
            EnumMember m;
            m.line = peek().line;
            m.name = expect(TT::IDENT, "identifier").value;
            if (match(TT::IS)) {
                m.value = parse_expr();
            }
            // Members may be separated by newlines or commas (both accepted)
            if (check(TT::COMMA)) advance();
            skip_newlines();
            ed->members.push_back(std::move(m));
        }
        expect(TT::END, "end");
        eat_newline();
        return ed;
    }

    StmtPtr parse_match() {
        int ln = peek().line;
        expect(TT::MATCH, "match");
        auto subject = parse_expr();
        eat_newline();
        skip_newlines();

        auto ms = std::make_unique<MatchStmt>(std::move(subject), ln);

        while (!check(TT::END) && !check(TT::EOF_T)) {
            expect(TT::WHEN, "when");
            MatchArm arm;
            // Wildcard: when _
            if (check(TT::IDENT) && peek().value == "_") {
                advance();
                arm.is_wildcard = true;
            } else {
                arm.pattern = parse_expr();
            }
            // Body: inline (then stmt) or multi-line block
            if (check(TT::THEN)) {
                advance();
                if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                    auto blk = std::make_unique<SuraBlock>(peek().line);
                    blk->body.push_back(parse_stmt_body(peek().line));
                    arm.body = std::move(blk);
                } else {
                    eat_newline();
                    arm.body = parse_block(); // stops at WHEN/END
                }
            } else {
                eat_newline();
                arm.body = parse_block();
            }
            skip_newlines();
            ms->arms.push_back(std::move(arm));
        }
        expect(TT::END, "end");
        return ms;
    }

    BlockPtr parse_when_arm_block() {
        auto blk = std::make_unique<SuraBlock>(peek().line);
        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)
            && !check(TT::IS) && !check(TT::IN) && !check(TT::ELSE)) {
            blk->body.push_back(parse_stmt());
            skip_newlines();
        }
        return blk;
    }

    StmtPtr parse_when() {
        int ln = peek().line;
        expect(TT::WHEN, "when");
        auto subject = parse_expr();
        expect(TT::DO, "do");
        eat_newline();
        skip_newlines();

        auto ms = std::make_unique<MatchStmt>(std::move(subject), ln);
        while (!check(TT::END) && !check(TT::EOF_T)) {
            MatchArm arm;
            if (match(TT::IS)) {
                arm.pattern = parse_expr();
            } else if (match(TT::IN)) {
                arm.is_range = true;
                arm.pattern = parse_expr();
                if (!match(TT::TO) && !match(TT::TILDE))
                    throw ParseError("'to' or '~' expected in when range arm", peek().line);
                arm.range_end = parse_expr();
            } else if (match(TT::ELSE)) {
                arm.is_wildcard = true;
            } else {
                throw ParseError("when arm에는 'is', 'in', 'else'가 필요합니다", peek().line);
            }

            expect(TT::THEN, "then");
            if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                auto blk = std::make_unique<SuraBlock>(peek().line);
                blk->body.push_back(parse_stmt_body(peek().line));
                arm.body = std::move(blk);
            } else {
                eat_newline();
                arm.body = parse_when_arm_block();
            }
            skip_newlines();
            ms->arms.push_back(std::move(arm));
        }
        expect(TT::END, "end");
        eat_newline();
        return ms;
    }

    StmtPtr parse_try() {
        int ln = peek().line;
        expect(TT::TRY, "try");
        eat_newline();
        auto try_block = parse_block();          
        expect(TT::CATCH, "catch");
        std::string catch_var = expect(TT::IDENT, "identifier").value;
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
        std::string name = expect(TT::IDENT, "identifier").value;
        std::string parent;
        if (match(TT::EXTENDS))
            parent = expect(TT::IDENT, "identifier").value;
        expect(TT::DO, "do");
        eat_newline();

        auto cd = std::make_unique<ClassDef>(std::move(name), std::move(parent), ln);

        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)) {
            if (check(TT::FUNC)) {
                
                advance(); 
                std::string mname = expect(TT::IDENT, "identifier").value;
                std::vector<std::string> params;
                std::vector<ExprPtr>     defaults;
                if (match(TT::LPAREN)) {
                    while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                        params.push_back(expect(TT::IDENT, "identifier").value);
                        
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
                throw ParseError("unexpected token '" + peek().value + "'", peek().line);
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
        std::string name = expect(TT::IDENT, "identifier").value;

        
        
        std::vector<std::string> params;
        std::vector<ExprPtr>     defaults;
        std::vector<TypeAnnot>   param_types;
        if (match(TT::LPAREN)) {
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "identifier").value);
                
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

        std::string function_abi = "sura";
        if (check(TT::IDENT) &&
            (peek().value == "interrupt" ||
             peek().value == "interrupt_error")) {
            function_abi = advance().value;
        }

        expect(TT::DO, "do");
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();

        auto fd = std::make_unique<FuncDef>(std::move(name), std::move(body), ln);
        fd->abi          = std::move(function_abi);
        fd->params       = std::move(params);
        fd->defaults     = std::move(defaults);
        fd->param_types  = std::move(param_types);
        fd->return_type  = return_type;
        return fd;
    }

    
    
    StmtPtr parse_cmd(const std::string& cmd_name, int ln) {
        
        static const std::vector<std::string> BUILTINS = {
            "print", "print_n", "input", "exit", "clock", "type", "sleep", "wait",
            "cls", "silent",
            "key_down", "readkey", "readkey_timeout",
            "win_init", "win_clear", "win_rect", "win_circle", "win_line", "win_text", "win_update", "win_poll", "win_focus", "win_close",
            "mouse_pos", "mouse_down",
            "grid_init", "grid_clear", "grid_set", "grid_draw"
        };
        static const std::vector<std::string> KWS = {"is", "if", "then", "else", "end", "while", "do", "func", "repeat", "break", "return", "use", "import", "global", "class", "struct", "enum", "try"};
        
        bool is_builtin = std::find(BUILTINS.begin(), BUILTINS.end(), cmd_name) != BUILTINS.end();
        if (!is_builtin) {
            std::string sug = sura_suggest(cmd_name, BUILTINS);
            if (sug.empty()) sug = sura_suggest(cmd_name, KWS);

            if (!sug.empty() && cmd_name != sug) {
                throw ParseError(
                    "'" + cmd_name + "' is not a known command.\n"
                    "  did you mean '" + sug + "'?",
                    ln, cmd_name
                );
            }
        }

        auto stmt = std::make_unique<CmdStmt>(cmd_name, ln);
        auto token_as_string_expr = [&]() -> ExprPtr {
            Token tok = advance();
            return std::make_unique<StrLit>(tok.value, tok.line);
        };
        auto parse_grid_cell_expr = [&]() -> ExprPtr {
            if (check(TT::IDENT) && peek().value.size() == 1) return token_as_string_expr();
            switch (peek().type) {
                case TT::STAR:
                case TT::PLUS:
                case TT::MINUS:
                case TT::SLASH:
                case TT::PERCENT:
                case TT::AMP:
                case TT::PIPE:
                case TT::CARET:
                case TT::LT:
                case TT::GT:
                case TT::QUESTION:
                case TT::COLON:
                    return token_as_string_expr();
                default:
                    return parse_expr();
            }
        };
        auto parse_grid_color_expr = [&]() -> ExprPtr {
            if (check(TT::IDENT)) return token_as_string_expr();
            return parse_expr();
        };
        auto parse_grid_coord_expr = [&]() -> ExprPtr {
            auto left = parse_unary();
            while (check(TT::PLUS) || check(TT::MINUS)) {
                int op_ln = peek().line;
                std::string op = advance().value;
                auto right = parse_unary();
                left = std::make_unique<BinOp>(op, std::move(left), std::move(right), op_ln);
            }
            return left;
        };
        if (cmd_name == "random") {
            stmt->args.push_back(parse_expr());            
            expect(TT::TILDE, "~");
            stmt->args.push_back(parse_expr());            
            stmt->args.push_back(parse_expr());            
        } else if (cmd_name == "silent") {
            while (!check(TT::NEWLINE) && !check(TT::EOF_T) && !check(TT::ELSE)) {
                if (check(TT::COMMA)) { advance(); continue; }
                stmt->args.push_back(token_as_string_expr());
            }
        } else if (cmd_name == "grid_set") {
            stmt->args.push_back(parse_grid_coord_expr());
            stmt->args.push_back(parse_grid_coord_expr());
            stmt->args.push_back(parse_grid_cell_expr());
            if (!check(TT::NEWLINE) && !check(TT::EOF_T) && !check(TT::ELSE)) {
                stmt->args.push_back(parse_grid_color_expr());
            }
        } else {
            while (!check(TT::NEWLINE) && !check(TT::EOF_T) && !check(TT::ELSE)) {
                if (check(TT::COMMA)) { advance(); continue; }
                stmt->args.push_back(parse_expr());
            }
        }
        eat_newline();
        return stmt;
    }

    
    
    
    
    
    
    
    std::string compound_assign_op() {
        TT t = peek().type;
        if      (t == TT::PLUS_EQ)    return "+";
        else if (t == TT::MINUS_EQ)   return "-";
        else if (t == TT::STAR_EQ)    return "*";
        else if (t == TT::SLASH_EQ)   return "/";
        else if (t == TT::PERCENT_EQ) return "%";
        return "";
    }

    ExprPtr clone_expr_for_compound_assign(const Expr* expr) {
        if (!expr) return nullptr;
        switch (expr->kind) {
            case NK::NUM_LIT: {
                auto* n = static_cast<const NumLit*>(expr);
                return std::make_unique<NumLit>(n->value, n->line);
            }
            case NK::STR_LIT: {
                auto* s = static_cast<const StrLit*>(expr);
                return std::make_unique<StrLit>(s->value, s->line);
            }
            case NK::BOOL_LIT: {
                auto* b = static_cast<const BoolLit*>(expr);
                return std::make_unique<BoolLit>(b->value, b->line);
            }
            case NK::NIL_LIT:
                return std::make_unique<NilLit>(expr->line);
            case NK::IDENT: {
                auto* id = static_cast<const Ident*>(expr);
                return std::make_unique<Ident>(id->name, id->line);
            }
            case NK::BIN_OP: {
                auto* b = static_cast<const BinOp*>(expr);
                return std::make_unique<BinOp>(
                    b->op,
                    clone_expr_for_compound_assign(b->left.get()),
                    clone_expr_for_compound_assign(b->right.get()),
                    b->line);
            }
            case NK::UNARY_OP: {
                auto* u = static_cast<const UnaryOp*>(expr);
                return std::make_unique<UnaryOp>(
                    u->op,
                    clone_expr_for_compound_assign(u->operand.get()),
                    u->line);
            }
            case NK::DOT_ACCESS: {
                auto* d = static_cast<const DotAccess*>(expr);
                return std::make_unique<DotAccess>(
                    clone_expr_for_compound_assign(d->obj.get()),
                    d->prop,
                    d->line,
                    d->optional);
            }
            case NK::INDEX: {
                auto* i = static_cast<const IndexExpr*>(expr);
                return std::make_unique<IndexExpr>(
                    clone_expr_for_compound_assign(i->obj.get()),
                    clone_expr_for_compound_assign(i->key.get()),
                    i->line);
            }
            case NK::CALL: {
                auto* c = static_cast<const CallExpr*>(expr);
                auto out = std::make_unique<CallExpr>(c->name, c->line);
                for (const auto& arg : c->args) out->args.push_back(clone_expr_for_compound_assign(arg.get()));
                return out;
            }
            case NK::METHOD_CALL: {
                auto* m = static_cast<const MethodCallExpr*>(expr);
                auto out = std::make_unique<MethodCallExpr>(
                    clone_expr_for_compound_assign(m->obj.get()),
                    m->method,
                    m->line,
                    m->optional);
                for (const auto& arg : m->args) out->args.push_back(clone_expr_for_compound_assign(arg.get()));
                return out;
            }
            case NK::SUPER_CALL: {
                auto* s = static_cast<const SuperCallExpr*>(expr);
                auto out = std::make_unique<SuperCallExpr>(s->method, s->line);
                for (const auto& arg : s->args) out->args.push_back(clone_expr_for_compound_assign(arg.get()));
                return out;
            }
            case NK::ARRAY_LIT: {
                auto* a = static_cast<const ArrayLit*>(expr);
                auto out = std::make_unique<ArrayLit>(a->line);
                for (const auto& item : a->elements) out->elements.push_back(clone_expr_for_compound_assign(item.get()));
                return out;
            }
            case NK::DICT_LIT: {
                auto* d = static_cast<const DictLit*>(expr);
                auto out = std::make_unique<DictLit>(d->line);
                for (const auto& key : d->key_order) {
                    auto it = d->pairs.find(key);
                    if (it != d->pairs.end()) out->add(key, clone_expr_for_compound_assign(it->second.get()));
                }
                return out;
            }
            case NK::NEW_EXPR: {
                auto* n = static_cast<const NewExpr*>(expr);
                auto out = std::make_unique<NewExpr>(n->class_name, n->line);
                for (const auto& arg : n->args) out->args.push_back(clone_expr_for_compound_assign(arg.get()));
                return out;
            }
            case NK::TERNARY: {
                auto* t = static_cast<const TernaryExpr*>(expr);
                return std::make_unique<TernaryExpr>(
                    clone_expr_for_compound_assign(t->cond.get()),
                    clone_expr_for_compound_assign(t->then_val.get()),
                    clone_expr_for_compound_assign(t->else_val.get()),
                    t->line);
            }
            case NK::STR_INTERP: {
                auto* s = static_cast<const StrInterp*>(expr);
                auto out = std::make_unique<StrInterp>(s->line);
                for (const auto& part : s->parts) out->parts.push_back(clone_expr_for_compound_assign(part.get()));
                return out;
            }
            default:
                throw ParseError("unsupported compound assignment target expression", expr->line);
        }
    }

    StmtPtr parse_ident_stmt(int ln) {
        std::string name = advance().value; 

        
        
        if (check(TT::DOT)) {
            advance(); 
            std::string prop = expect(TT::IDENT, "identifier").value;

            
            if (check(TT::IS)) {
                advance(); 
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<DotAssignStmt>(name, prop, std::move(val), ln);
            }

            {
                std::string op = compound_assign_op();
                if (!op.empty()) {
                    advance();
                    auto val = parse_expr();
                    auto current = std::make_unique<DotAccess>(std::make_unique<Ident>(name, ln), prop, ln);
                    auto combined = std::make_unique<BinOp>(op, std::move(current), std::move(val), ln);
                    eat_newline();
                    return std::make_unique<DotAssignStmt>(name, prop, std::move(combined), ln);
                }
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

            throw ParseError("syntax error", peek().line);
        }

        
        if (check(TT::LBRACKET)) {
            advance(); 
            auto key = parse_expr();
            expect(TT::RBRACKET, "]");
            if (check(TT::IS)) {
                advance();
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<IndexAssignStmt>(name, std::move(key), std::move(val), ln);
            }
            {
                std::string op = compound_assign_op();
                if (!op.empty()) {
                    advance();
                    auto read_key = clone_expr_for_compound_assign(key.get());
                    auto val = parse_expr();
                    auto current = std::make_unique<IndexExpr>(std::make_unique<Ident>(name, ln), std::move(read_key), ln);
                    auto combined = std::make_unique<BinOp>(op, std::move(current), std::move(val), ln);
                    eat_newline();
                    return std::make_unique<IndexAssignStmt>(name, std::move(key), std::move(combined), ln);
                }
            }
            expect(TT::IS, "is");
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
            std::string op = compound_assign_op();
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

        
        if (!allow_legacy_command_syntax) {
            throw ParseError(
                "legacy command-style syntax is disabled; use '" + name + "(...)' instead",
                ln, name
            );
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
            case TT::MATCH:  return parse_match();
            case TT::WHEN:   return parse_when();
            case TT::ENUM:   return parse_enum();
            case TT::STRUCT: return parse_struct();
            case TT::IMPORT: return parse_import();
            case TT::GLOBAL: return parse_global_decl();
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
                std::string lib = expect(TT::IDENT, "identifier").value;
                eat_newline();
                return std::make_unique<UseStmt>(std::move(lib), ln);
            }
            case TT::IDENT:
                return parse_ident_stmt(ln);
            case TT::SUPER: {
                
                advance(); 
                expect(TT::DOT, ".");
                std::string method = expect(TT::IDENT, "identifier").value;
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
                {
                    std::string tok = peek().value;
                    std::string sug = sura_suggest(tok, get_all_keywords());
                    if (!sug.empty() && tok != sug) {
                        throw ParseError(
                            "'" + tok + "' is not a known statement keyword.\n"
                            "  did you mean '" + sug + "'?",
                            ln, tok
                        );
                    }
                }
                throw ParseError("statement parse failed: '" + peek().value + "'", ln);
            }
        }
    }

    
    StmtPtr parse_stmt() {
        skip_newlines();
        NestingGuard guard(nesting_depth, peek().line);
        int ln = peek().line;
        return parse_stmt_body(ln);
    }

public:
    void set_legacy_command_syntax(bool enabled) {
        allow_legacy_command_syntax = enabled;
    }

    
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

    std::vector<ParseDiagnostic> diagnose(const std::vector<Token>& toks, size_t max_errors = 50) {
        tokens = toks;
        pos    = 0;
        std::vector<ParseDiagnostic> diagnostics;
        skip_newlines();
        while (!check(TT::EOF_T) && diagnostics.size() < max_errors) {
            size_t before = pos;
            try {
                (void)parse_stmt();
            } catch (const ParseError& e) {
                diagnostics.push_back(ParseDiagnostic{e.line, e.what(), e.details});
                if (pos == before && !check(TT::EOF_T)) advance();
                synchronize_after_error();
            }
            skip_newlines();
        }
        return diagnostics;
    }

    
    BlockPtr parse_source(const std::string& source) {
        Lexer lexer;
        return parse(lexer.tokenize(source));
    }

    std::vector<ParseDiagnostic> diagnose_source(const std::string& source, size_t max_errors = 50) {
        Lexer lexer;
        return diagnose(lexer.tokenize(source), max_errors);
    }

    // Parse a single expression from a source snippet. Used by the compiler's
    // string-interpolation path so `{obj.field}`, `{a + b}`, `{f()}` etc. parse
    // as full expressions instead of being treated as raw variable names.
    ExprPtr parse_expr_from_source(const std::string& source) {
        Lexer lexer;
        tokens = lexer.tokenize(source);
        pos    = 0;
        skip_newlines();
        return parse_expr();
    }
};
