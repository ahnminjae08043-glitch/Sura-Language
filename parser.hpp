#pragma once
#include "lexer.hpp"
#include "ast.hpp"
#include "platform.hpp"
#include <stdexcept>
#include <algorithm>

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?˜ë¼(SURA) Parser  ??v2.0
//  ? í° ?¤íŠ¸ë¦???AST
//  ë°©ì‹: Recursive Descent (?¬ê? ?˜ê°•)
//
//  ?°ì‚°???°ì„ ?œìœ„ (??Œ ???’ìŒ)
//    1. or
//    2. and
//    3. not  (?¨í•­)
//    4. ==  !=  >  <  >=  <=
//    5. +  -
//    6. *  /  %
//    7. -  (?¨í•­ ?Œìˆ˜)
//    8. primary  (ë¦¬í„°?? ?ë³„?? ê´„í˜¸)
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

struct ParseError : std::runtime_error {
    int line;
    std::string details;
    ParseError(const std::string& msg, int ln, const std::string& det = "")
        : std::runtime_error("[?˜ë¼ ?Œì„œ ?¤ë¥˜] " + std::to_string(ln) + "ì¤? " + msg + (det.empty() ? "" : " (" + det + ")"))
        , line(ln), details(det) {}
};

class Parser {
    std::vector<Token> tokens;
    size_t             pos = 0;

    // ?€?€ ?¤í? ì¶”ì²œ??ëª©ë¡ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    std::vector<std::string> get_all_keywords() {
        return {
            "is", "if", "then", "else", "end", "while", "do", "func", "repeat", 
            "break", "return", "and", "or", "not", "true", "false", "use", 
            "continue", "for", "in", "to", "try", "catch", "throw", "class", 
            "new", "extends", "super", "elif", "finally",
            "print", "print_n", "input", "exit", "clock", "type" // ?´ì¥ ëª…ë ¹?´ë„ ?¬í•¨
        };
    }

    // ?€?€ ? í° ?‘ê·¼ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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
            std::string msg = "'" + what + "' ?„ìš” ???€??'" + peek().type_str() + "' ?ˆìŒ";
            if (peek().type == TT::IDENT) {
                std::string sug = sura_suggest(peek().value, get_all_keywords());
                if (!sug.empty()) msg += " (?¹ì‹œ '" + sug + "'ë¥??…ë ¥?˜ì‹œ???ˆë‚˜??)";
            }
            throw ParseError(msg, peek().line, peek().value);
        }
        return advance();
    }

    void skip_newlines() { while (check(TT::NEWLINE)) advance(); }

    // ?€?€ ?€???´ë…¸?Œì´???Œì‹± ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    // ?„ì¬ ?„ì¹˜?ì„œ ?€???´ë¦„ IDENTë¥??½ì–´ TypeAnnot ë°˜í™˜
    // ì§€?? ?«ì, ë¬¸ì?? ë¶ˆë¦¬?? ë°°ì—´, ?•ì…”?ˆë¦¬, ?†ìŒ, ?„ë¬´,
    //       number, string, bool, array, dict, nil, any, ?´ë˜?¤ëª…
    TypeAnnot parse_type_annot() {
        TypeAnnot ta;
        ta.present = true;
        if (!check(TT::IDENT))
            throw ParseError("?€???´ë¦„ ?„ìš”", peek().line);
        std::string tname = advance().value;
        if      (tname == "?«ì"   || tname == "number") ta.kind = SType::NUMBER;
        else if (tname == "ë¬¸ì?? || tname == "string") ta.kind = SType::STRING;
        else if (tname == "ë¶ˆë¦¬?? || tname == "bool")   ta.kind = SType::BOOL;
        else if (tname == "ë°°ì—´"   || tname == "array")  ta.kind = SType::ARRAY;
        else if (tname == "?•ì…”?ˆë¦¬"|| tname == "dict")  ta.kind = SType::DICT;
        else if (tname == "?†ìŒ"   || tname == "nil")    ta.kind = SType::NIL;
        else if (tname == "?„ë¬´"   || tname == "any")    ta.kind = SType::ANY;
        else {
            // ?¬ìš©???•ì˜ ?´ë˜???€??            ta.kind       = SType::CLASS;
            ta.class_name = tname;
        }
        return ta;
    }

    // ë¬¸ì¥ ??ê°œí–‰ ?Œë¹„ (EOF, ë¸”ë¡ ì¢…ë£Œ?ë„ ?ˆìš©)
    void eat_newline() {
        if (check(TT::EOF_T)) return;
        // ë¸”ë¡ ì¢…ë£Œ?ëŠ” ê°œí–‰ ?†ì–´??OK (?¸ë¼???¨ìˆ˜ ë³¸ë¬¸ ì§€??
        if (check(TT::END) || check(TT::ELSE) || check(TT::ELIF) || check(TT::CATCH) || check(TT::FINALLY)) return;
        if (!match(TT::NEWLINE))
            throw ParseError("ì¤???ê°œí–‰) ?„ìš”", peek().line);
    }

    // ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
    //  ?œí˜„???Œì‹± ???°ì‚°???°ì„ ?œìœ„ ê³„ì¸µ
    // ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

    ExprPtr parse_expr() { return parse_ternary(); }

    // ?€?€ ?¼í•­: ì¡°ê±´ ? ì°¸ê°’ : ê±°ì§“ê°??€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_ternary() {
        auto left = parse_or();
        if (check(TT::QUESTION)) {
            int ln = peek().line; advance();
            auto then_val = parse_expr();
            expect(TT::COLON, ":");
            auto else_val = parse_ternary(); // ?°ê²°??            return std::make_unique<TernaryExpr>(
                std::move(left), std::move(then_val), std::move(else_val), ln);
        }
        return left;
    }

    // ?€?€ or ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_or() {
        auto left = parse_and();
        while (check(TT::OR)) {
            int ln = peek().line; advance();
            auto right = parse_and();
            left = std::make_unique<BinOp>("or", std::move(left), std::move(right), ln);
        }
        return left;
    }

    // ?€?€ and ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_and() {
        auto left = parse_not();
        while (check(TT::AND)) {
            int ln = peek().line; advance();
            auto right = parse_not();
            left = std::make_unique<BinOp>("and", std::move(left), std::move(right), ln);
        }
        return left;
    }

    // ?€?€ not (?¨í•­) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_not() {
        if (check(TT::NOT)) {
            int ln = peek().line; advance();
            return std::make_unique<UnaryOp>("not", parse_not(), ln);
        }
        return parse_compare();
    }

    // ?€?€ ë¹„êµ: ==  !=  >  <  >=  <=  in ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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

    // ?€?€ ë¹„íŠ¸ OR: | ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_bitor() {
        auto left = parse_bitxor();
        while (check(TT::PIPE)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("|", std::move(left), parse_bitxor(), ln);
        }
        return left;
    }

    // ?€?€ ë¹„íŠ¸ XOR: ^ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_bitxor() {
        auto left = parse_bitand();
        while (check(TT::CARET)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("^", std::move(left), parse_bitand(), ln);
        }
        return left;
    }

    // ?€?€ ë¹„íŠ¸ AND: & ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_bitand() {
        auto left = parse_shift();
        while (check(TT::AMP)) {
            int ln = peek().line; advance();
            left = std::make_unique<BinOp>("&", std::move(left), parse_shift(), ln);
        }
        return left;
    }

    // ?€?€ ?œí”„?? <<  >> ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    ExprPtr parse_shift() {
        auto left = parse_additive();
        while (check(TT::LSHIFT) || check(TT::RSHIFT)) {
            int ln = peek().line;
            std::string op = advance().value;
            left = std::make_unique<BinOp>(op, std::move(left), parse_additive(), ln);
        }
        return left;
    }

    // ?€?€ ?§ì…ˆ/ëº„ì…ˆ: +  - ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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

    // ?€?€ ê³±ì…ˆ/?˜ëˆ—?? *  /  % ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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

    // ?€?€ ?¨í•­: -x  ~x ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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

    // ?€?€ ë³´ê°„ ë¬¸ì???œë¸Œ ?Œì„œ (JIT ?”ì§„?ì„œ ì§ì ‘ ?Œì‹±?˜ë?ë¡??¬ê¸°?œëŠ” ?¨ìˆœ ì²˜ë¦¬) ?€?€
    ExprPtr parse_fstr(const Token& tok) {
        return std::make_unique<StrLit>(tok.value, tok.line);
    }

    ExprPtr parse_primary() {
        int ln = peek().line;

        // ?«ì
        if (check(TT::NUM)) {
            double v = std::stod(peek().value);
            advance();
            return std::make_unique<NumLit>(v, ln);
        }
        // ë¬¸ì??        if (check(TT::STR)) {
            std::string v = advance().value;
            return std::make_unique<StrLit>(std::move(v), ln);
        }
        // ë³´ê°„ ë¬¸ì??"hello {name}"
        if (check(TT::FSTR)) {
            Token tok = advance();
            return parse_fstr(tok);
        }
        // true / false
        if (check(TT::TRUE))  { advance(); return std::make_unique<BoolLit>(true,  ln); }
        if (check(TT::FALSE)) { advance(); return std::make_unique<BoolLit>(false, ln); }

        // ?ë³„??/ ?¨ìˆ˜ ?¸ì¶œ ?œí˜„??        if (check(TT::IDENT)) {
            std::string name = advance().value;

            // ?¨ìˆ˜ ?¸ì¶œ: name(arg1, arg2, ...)
            if (check(TT::LPAREN)) {
                advance(); // (
                auto call = std::make_unique<CallExpr>(name, ln);
                while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                    call->args.push_back(parse_expr());
                    if (!match(TT::COMMA)) break;
                }
                expect(TT::RPAREN, ")");
                return call;
            }

            // obj.prop / obj.method(args) / arr[i] ì²´ì¸
            ExprPtr expr = std::make_unique<Ident>(name, ln);
            while (true) {
                if (check(TT::DOT)) {
                    advance();
                    std::string prop = expect(TT::IDENT, "?ì„±ëª??ëŠ” ë©”ì„œ?œëª…").value;
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
                    // arr[i] ?½ê¸°
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
        // new ?´ë˜?¤ëª…[(?¸ì)]
        if (check(TT::NEW)) {
            advance();
            std::string cls = expect(TT::IDENT, "?´ë˜???´ë¦„").value;
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

        // ?•ì…”?ˆë¦¬ ë¦¬í„°?? {"??: ê°? ?ë³„?? ê°? ...}
        if (check(TT::LBRACE)) {
            advance(); // {
            auto dict = std::make_unique<DictLit>(ln);
            while (!check(TT::RBRACE) && !check(TT::EOF_T)) {
                std::string key;
                if (check(TT::STR))   key = advance().value;
                else if (check(TT::IDENT)) key = advance().value;
                else throw ParseError("?•ì…”?ˆë¦¬ ?¤ëŠ” ë¬¸ì???ëŠ” ?ë³„?ì—¬???©ë‹ˆ??, peek().line);
                expect(TT::COLON, ":");
                auto val = parse_expr();
                dict->key_order.push_back(key);
                dict->pairs[key] = std::move(val);
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACE, "}");
            return dict;
        }

        // ë°°ì—´ ë¦¬í„°?? [expr, expr, ...]
        if (check(TT::LBRACKET)) {
            advance(); // [
            auto arr = std::make_unique<ArrayLit>(ln);
            while (!check(TT::RBRACKET) && !check(TT::EOF_T)) {
                arr->elements.push_back(parse_expr());
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RBRACKET, "]");
            return arr;
        }

        // super.method(args)
        if (check(TT::SUPER)) {
            advance();
            expect(TT::DOT, ".");
            std::string method = expect(TT::IDENT, "ë©”ì„œ?œëª…").value;
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

        // func(params) do ... end ???µëª… ?¨ìˆ˜ ?œí˜„??        if (peek().type == TT::FUNC) {
            advance(); // func
            expect(TT::LPAREN, "(");
            std::vector<std::string> params;
            std::vector<ExprPtr> defaults;
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "ë§¤ê°œë³€?˜ëª…").value);
                ExprPtr def;
                if (check(TT::IS)) { advance(); def = parse_expr(); }
                defaults.push_back(std::move(def));
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
            expect(TT::DO, "do");
            match(TT::NEWLINE); // ê°œí–‰?€ ? íƒ (?¸ë¼???ˆìš©)
            auto body = parse_block();
            expect(TT::END, "end");
            auto fe = std::make_unique<FuncExpr>(ln);
            fe->params   = std::move(params);
            fe->defaults = std::move(defaults);
            fe->body     = std::move(body);
            return std::move(fe);
        }

        // ê´„í˜¸ ê·¸ë£¹: (expr)
        if (check(TT::LPAREN)) {
            advance();
            auto expr = parse_expr();
            expect(TT::RPAREN, ")");
            return expr;
        }

        throw ParseError("?œí˜„???„ìš” ???€??'" + peek().value + "' ?ˆìŒ", ln);
    }

    // ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
    //  ë¬¸ì¥ ?Œì‹±
    // ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

    // ë¸”ë¡: stmt* (end / else / catch / EOF ?ì„œ ë©ˆì¶¤, ?Œë¹„?˜ì? ?ŠìŒ)
    BlockPtr parse_block() {
        auto SuraBlock = std::make_unique<SuraBlock>(peek().line);
        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)
            && !check(TT::ELSE) && !check(TT::ELIF) && !check(TT::CATCH) && !check(TT::FINALLY)) {
            SuraBlock->body.push_back(parse_stmt());
            skip_newlines();
        }
        return SuraBlock;
    }

    // if ì¡°ê±´ë¬?    StmtPtr parse_if() {
        int ln = peek().line;
        expect(TT::IF, "if");
        auto cond = parse_expr();
        expect(TT::THEN, "then");

        // ?€?€ ?¸ë¼??if: then ??ê°™ì? ì¤„ì— ë¬¸ì¥???ˆìŒ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
        if (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
            auto inline_body = parse_stmt_body(ln); // ê°œí–‰ ?¬í•¨ ?Œë¹„
            auto then_block = std::make_unique<SuraBlock>(ln);
            then_block->body.push_back(std::move(inline_body));
            return std::make_unique<IfStmt>(std::move(cond), std::move(then_block), nullptr, ln);
        }

        // ?€?€ ë¸”ë¡ if ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
        eat_newline();
        auto then_block = parse_block();

        // elif ???˜ì§‘
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

        // elif ì²´ì¸???¤ì—???ìœ¼ë¡?ì¤‘ì²© IfStmt ë¡?ë³€??        BlockPtr cur_else = std::move(else_block);
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

    // while ë°˜ë³µ
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

    // repeat N ë°˜ë³µ
    StmtPtr parse_repeat() {
        int ln = peek().line;
        expect(TT::REPEAT, "repeat");
        auto count = parse_expr();
        match(TT::DO); // do ??? íƒ
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<RepeatStmt>(std::move(count), std::move(body), ln);
    }

    // for i in ?œì‘ to ??[step ê°? do ... end  (?«ì ë²”ìœ„)
    // for i in ?œì‘ ~ ?? [step ê°? do ... end  (~ ???ˆìš©)
    // for item in ë°°ì—´ do ... end                (foreach)
    StmtPtr parse_for() {
        int ln = peek().line;
        expect(TT::FOR, "for");
        std::string var = expect(TT::IDENT, "ë£¨í”„ ë³€??).value;
        std::string var2;
        if (check(TT::COMMA)) {
            advance();
            var2 = expect(TT::IDENT, "ë£¨í”„ ë³€??2").value;
        }
        expect(TT::IN, "in");
        auto first = parse_expr();

        // foreach: for item in ì»¬ë ‰??do ... end
        if (check(TT::DO) || check(TT::NEWLINE) || check(TT::EOF_T)) {
            match(TT::DO);
            eat_newline();
            auto body = parse_block();
            expect(TT::END, "end");
            eat_newline();
            return std::make_unique<ForeachStmt>(
                std::move(var), std::move(var2), std::move(first), std::move(body), ln);
        }

        // ?«ì ë²”ìœ„: to ?ëŠ” ~ ?????ˆìš©
        if (!match(TT::TO) && !match(TT::TILDE))
            throw ParseError("'to', '~', ?ëŠ” 'do' ?„ìš”", peek().line);
        auto end = parse_expr();
        // ? íƒ??step
        ExprPtr step;
        if (peek().type == TT::IDENT && peek().value == "step") {
            advance();
            step = parse_expr();
        }
        match(TT::DO); // do ??? íƒ
        eat_newline();
        auto body = parse_block();
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<ForStmt>(
            std::move(var), std::move(first), std::move(end), std::move(step), std::move(body), ln);
    }

    // try ... catch ?¤ë¥˜ë³€??... end
    StmtPtr parse_try() {
        int ln = peek().line;
        expect(TT::TRY, "try");
        eat_newline();
        auto try_block = parse_block();          // CATCH ?ì„œ ë©ˆì¶¤
        expect(TT::CATCH, "catch");
        std::string catch_var = expect(TT::IDENT, "?¤ë¥˜ ë³€?˜ëª…").value;
        eat_newline();
        auto catch_block = parse_block();        // END ?ëŠ” FINALLY ?ì„œ ë©ˆì¶¤

        BlockPtr finally_block;
        if (match(TT::FINALLY)) {
            expect(TT::DO, "do");
            eat_newline();
            finally_block = parse_block();       // END ?ì„œ ë©ˆì¶¤
        }
        expect(TT::END, "end");
        eat_newline();
        return std::make_unique<TryStmt>(
            std::move(try_block), std::move(catch_var),
            std::move(catch_block), std::move(finally_block), ln);
    }
    //   func ë©”ì„œ?œëª…(params) do ... end
    // end
    StmtPtr parse_class() {
        int ln = peek().line;
        expect(TT::CLASS, "class");
        std::string name = expect(TT::IDENT, "?´ë˜???´ë¦„").value;
        std::string parent;
        if (match(TT::EXTENDS))
            parent = expect(TT::IDENT, "ë¶€ëª??´ë˜???´ë¦„").value;
        expect(TT::DO, "do");
        eat_newline();

        auto cd = std::make_unique<ClassDef>(std::move(name), std::move(parent), ln);

        skip_newlines();
        while (!check(TT::EOF_T) && !check(TT::END)) {
            if (check(TT::FUNC)) {
                // ë©”ì„œ?? func ?´ë¦„(params) do ... end
                advance(); // func
                std::string mname = expect(TT::IDENT, "ë©”ì„œ???´ë¦„").value;
                std::vector<std::string> params;
                std::vector<ExprPtr>     defaults;
                if (match(TT::LPAREN)) {
                    while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                        params.push_back(expect(TT::IDENT, "ë§¤ê°œë³€?˜ëª…").value);
                        // ? íƒ???€???´ë…¸?Œì´??(?´ë˜??ë©”ì„œ??
                        if (check(TT::COLON)) { advance(); parse_type_annot(); } // ?Œì‹±ë§?(ë¯¸ë˜ ?•ì¥??
                        ExprPtr def;
                        if (check(TT::IS)) { advance(); def = parse_expr(); }
                        defaults.push_back(std::move(def));
                        if (!match(TT::COMMA)) break;
                    }
                    expect(TT::RPAREN, ")");
                }
                // ? íƒ??ë°˜í™˜ ?€??(?´ë˜??ë©”ì„œ??
                if (check(TT::ARROW)) { advance(); parse_type_annot(); } // ?Œì‹±ë§?(ë¯¸ë˜ ?•ì¥??
                expect(TT::DO, "do");
                eat_newline();
                auto mbody = parse_block();
                expect(TT::END, "end");
                eat_newline();
                cd->add_method(mname, std::move(params), std::move(defaults), std::move(mbody));
            } else if (check(TT::IDENT)) {
                // ?„ë“œ: ?„ë“œëª?is ê¸°ë³¸ê°?                std::string fname = advance().value;
                expect(TT::IS, "is");
                auto fval = parse_expr();
                eat_newline();
                cd->add_field(std::move(fname), std::move(fval));
            } else {
                throw ParseError("?´ë˜??ë°”ë””??func ?ëŠ” ?„ë“œ ?•ì˜ ?„ìš” ???€??'" + peek().value + "'", peek().line);
            }
            skip_newlines();
        }

        expect(TT::END, "end");
        eat_newline();
        return cd;
    }

    // func name(a, b) do  ?ëŠ”  func name do  (ë§¤ê°œë³€???†ëŠ” ê¸°ì¡´ ë¬¸ë²•)
    StmtPtr parse_func() {
        int ln = peek().line;
        expect(TT::FUNC, "func");
        std::string name = expect(TT::IDENT, "?¨ìˆ˜ ?´ë¦„").value;

        // ? íƒ??ë§¤ê°œë³€?? func name(a, b is ê¸°ë³¸ê°? c) do
        //                   func name(a: ?«ì, b: ë¬¸ì??is "ê¸°ë³¸") -> ?«ì do
        std::vector<std::string> params;
        std::vector<ExprPtr>     defaults;
        std::vector<TypeAnnot>   param_types;
        if (match(TT::LPAREN)) {
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                params.push_back(expect(TT::IDENT, "ë§¤ê°œë³€?˜ëª…").value);
                // ? íƒ???€???´ë…¸?Œì´?? param: ?€??                TypeAnnot pta;
                if (check(TT::COLON)) { advance(); pta = parse_type_annot(); }
                param_types.push_back(pta);
                ExprPtr def;
                if (check(TT::IS)) { advance(); def = parse_expr(); }
                defaults.push_back(std::move(def));
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
        }

        // ? íƒ??ë°˜í™˜ ?€?? -> ?€??        TypeAnnot return_type;
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

    // ?´ì¥ ëª…ë ¹?? cmd expr expr ...
    // random ?€ min ~ max result ?¨í„´ ?¹ìˆ˜ ì²˜ë¦¬
    StmtPtr parse_cmd(const std::string& cmd_name, int ln) {
        // ?Œë ¤ì§??¤ì›Œ?œë? ?¤ìˆ˜ë¡?ëª…ë ¹?´ë¡œ ?¼ëŠ”ì§€ ì²´í¬ (?¤í? ?ì?)
        static const std::vector<std::string> BUILTINS = {"print", "print_n", "input", "exit", "clock", "type"};
        static const std::vector<std::string> KWS = {"is", "if", "then", "else", "end", "while", "do", "func", "repeat", "break", "return", "use", "class", "try"};
        
        bool is_builtin = std::find(BUILTINS.begin(), BUILTINS.end(), cmd_name) != BUILTINS.end();
        if (!is_builtin) {
            std::string sug = sura_suggest(cmd_name, BUILTINS);
            if (sug.empty()) sug = sura_suggest(cmd_name, KWS);
            
            if (!sug.empty()) {
                // ?„ì „???€ë¦?ê²??„ë‹ ?˜ë„ ?ˆìœ¼??ê²½ê³ ??ë©”ì‹œì§€ë¥??´ì•„ ?ëŸ¬ë¥??˜ì§
                // (?¬ìš©?ê? ?•ì˜???¨ìˆ˜???˜ë„ ?ˆì?ë§? pirent ì²˜ëŸ¼ ?ˆë¬´ ? ì‚¬?˜ë©´ ?œì•ˆ)
                if (cmd_name != sug) {
                     // ?¬ê¸°??ë°”ë¡œ throw ?˜ê¸°ë³´ë‹¨, ?˜ì¤‘???¤í–‰ ???ëŸ¬ê°€ ?˜ê² ì§€ë§?
                     // ?Œì„œ ?˜ì??ì„œ ?•ì‹¤???¤í?(?? pirent)???¡ì•„ì£¼ëŠ” ê²?ì¢‹ìŒ
                     // ?˜ì?ë§?? ì—°?±ì„ ?„í•´ ?¼ë‹¨ CmdStmtë¡??˜ê¸°ê³? 
                     // ?˜ì¤‘??"?????†ëŠ” ëª…ë ¹?? ?ëŸ¬ ??suggestion???œìš©?˜ë„ë¡???
                }
            }
        }

        auto stmt = std::make_unique<CmdStmt>(cmd_name, ln);
        if (cmd_name == "random") {
            stmt->args.push_back(parse_expr());            // min
            expect(TT::TILDE, "~");
            stmt->args.push_back(parse_expr());            // max
            stmt->args.push_back(parse_expr());            // result ident
        } else {
            while (!check(TT::NEWLINE) && !check(TT::EOF_T)) {
                if (check(TT::COMMA)) { advance(); continue; }
                stmt->args.push_back(parse_expr());
            }
        }
        eat_newline();
        return stmt;
    }

    // ?€?€ ?ë³„?ë¡œ ?œì‘?˜ëŠ” ë¬¸ì¥ êµ¬ë¶„ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    // 1. name is expr         ??AssignStmt
    // 2. obj.prop is expr     ??DotAssignStmt
    // 3. arr[i] is expr       ??IndexAssignStmt
    // 4. name op= expr        ??InplaceStmt
    // 5. name(args)           ??CmdStmt (?¨ìˆ˜ ?¸ì¶œ)
    // 6. cmd args...          ??CmdStmt (?´ì¥ ëª…ë ¹??
    StmtPtr parse_ident_stmt(int ln) {
        std::string name = advance().value; // IDENT ?Œë¹„

        // obj.prop is expr  ??DotAssignStmt
        // obj.method(args) ??CmdStmt("obj.method", args)
        if (check(TT::DOT)) {
            advance(); // .
            std::string prop = expect(TT::IDENT, "?ì„±ëª??ëŠ” ë©”ì„œ?œëª…").value;

            // obj.prop is expr
            if (check(TT::IS)) {
                advance(); // is
                auto val = parse_expr();
                eat_newline();
                return std::make_unique<DotAssignStmt>(name, prop, std::move(val), ln);
            }

            // obj.method(args) ??ë©”ì„œ???¸ì¶œ ë¬¸ì¥
            if (check(TT::LPAREN)) {
                advance(); // (
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

            throw ParseError("???¤ì— 'is' ?ëŠ” '(' ?„ìš”", peek().line);
        }

        // arr[i] is expr  ??IndexAssignStmt
        if (check(TT::LBRACKET)) {
            advance(); // [
            auto key = parse_expr();
            expect(TT::RBRACKET, "]");
            expect(TT::IS, "is");
            auto val = parse_expr();
            eat_newline();
            return std::make_unique<IndexAssignStmt>(name, std::move(key), std::move(val), ln);
        }

        // name: ?€??is expr  ???€???´ë…¸?Œì´???ˆëŠ” ?€??        if (check(TT::COLON)) {
            advance(); // :
            TypeAnnot ta = parse_type_annot();
            expect(TT::IS, "is");
            auto val = parse_expr();
            eat_newline();
            auto stmt = std::make_unique<AssignStmt>(name, std::move(val), ln);
            stmt->type_annot = ta;
            return stmt;
        }

        if (check(TT::IS)) {
            advance(); // is
            auto val = parse_expr();
            eat_newline();
            return std::make_unique<AssignStmt>(name, std::move(val), ln);
        }

        // name += expr  (ë³µí•© ?€?? ëª…ì‹œ???•íƒœë§??ˆìš©)
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

        // name(args) ???¨ìˆ˜/?´ì¥?¨ìˆ˜ ?¸ì¶œ ë¬¸ì¥ (ExprStmt)
        if (check(TT::LPAREN)) {
            advance(); // (
            auto ce = std::make_unique<CallExpr>(name, ln);
            while (!check(TT::RPAREN) && !check(TT::EOF_T)) {
                ce->args.push_back(parse_expr());
                if (!match(TT::COMMA)) break;
            }
            expect(TT::RPAREN, ")");
            eat_newline();
            return std::make_unique<ExprStmt>(std::move(ce), ln);
        }

        // ?´ì¥ ëª…ë ¹??or ?¸ì ?†ëŠ” ?¨ìˆ˜ ?¸ì¶œ
        return parse_cmd(name, ln);
    }

    // ë¬¸ì¥ ë³¸ì²´ ?Œì‹± (if ?¸ë¼?¸ìš©, parse_stmt ?´ë??ì„œ???¬ìš©)
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
                std::string lib = expect(TT::IDENT, "?¼ì´ë¸ŒëŸ¬ë¦¬ëª…").value;
                eat_newline();
                return std::make_unique<UseStmt>(std::move(lib), ln);
            }
            case TT::IDENT:
                return parse_ident_stmt(ln);
            case TT::SUPER: {
                // super.method(args) ë¬¸ì¥ ??ExprStmt(SuperCallExpr)
                advance(); // super
                expect(TT::DOT, ".");
                std::string method = expect(TT::IDENT, "ë©”ì„œ?œëª…").value;
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
                // ?«ìÂ·ë¬¸ì?´Â·ë¶ˆë¦¬ì–¸Â·ê´„í˜¸Â·?¨í•­ ?°ì‚°???±ìœ¼ë¡??œì‘?˜ëŠ” ?œí˜„??ë¬¸ì¥ (REPL ?¸ì˜)
                TT t = peek().type;
                if (t == TT::NUM || t == TT::STR || t == TT::TRUE || t == TT::FALSE
                    || t == TT::LPAREN || t == TT::MINUS || t == TT::NOT
                    || t == TT::LBRACKET) {
                    auto expr = parse_expr();
                    eat_newline();
                    return std::make_unique<ExprStmt>(std::move(expr), ln);
                }
                throw ParseError("ë¬¸ì¥ ?Œì‹± ?¤íŒ¨ ??'" + peek().value + "'", ln);
            }
        }
    }

    // ìµœìƒ??ë¬¸ì¥ ?Œì‹±
    StmtPtr parse_stmt() {
        skip_newlines();
        int ln = peek().line;
        return parse_stmt_body(ln);
    }

public:
    // ?€?€ ì§„ì…?? ? í° ??AST (SuraBlock = ?„ì²´ ?„ë¡œê·¸ë¨) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
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

    // ?ŒìŠ¤ ë¬¸ì?´ì—??ë°”ë¡œ ?Œì‹± (Lexer ?¬í•¨)
    BlockPtr parse_source(const std::string& source) {
        Lexer lexer;
        return parse(lexer.tokenize(source));
    }
};
