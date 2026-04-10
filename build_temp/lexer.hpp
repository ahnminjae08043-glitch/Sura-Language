#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  ?˜ë¼(SURA) Lexer  ??v2.0
//  ?ŒìŠ¤ ë¬¸ì????? í° ?¤íŠ¸ë¦?
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•

// ?€?€ ? í° ì¢…ë¥˜ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
enum class TT {
    // ?€?€ ë¦¬í„°???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    NUM,    // 42  /  3.14
    STR,    // "?ˆë…•?˜ì„¸??  (?°ì˜´???œê±°??ê°?
    IDENT,  // ë³€?˜ëª…, ?¨ìˆ˜ëª?(?œê? ?¬í•¨)

    // ?€?€ ?¤ì›Œ???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    IS,        // is      (?€??
    IF,        // if
    THEN,      // then
    ELSE,      // else
    END,       // end
    WHILE,     // while
    DO,        // do
    FUNC,      // func
    REPEAT,    // repeat
    BREAK,     // break
    RETURN,    // return
    AND,       // and
    OR,        // or
    NOT,       // not
    TRUE,      // true
    FALSE,     // false
    USE,       // use      (?¼ì´ë¸ŒëŸ¬ë¦?ë¶ˆëŸ¬?¤ê¸°)
    CONTINUE,  // continue
    FOR,       // for
    IN,        // in
    TO,        // to
    TRY,       // try
    CATCH,     // catch
    THROW,     // throw
    CLASS,     // class
    NEW,       // new
    EXTENDS,   // extends
    SUPER,     // super
    ELIF,      // elif
    FINALLY,   // finally
    FSTR,      // "ë¬¸ì??{ë³´ê°„}"  (ë³´ê°„ ë¬¸ì??

    // ?€?€ ë¹„íŠ¸ / ?¼ë¦¬ ë³´ì¡° ê¸°í˜¸ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    AMP,      // &
    PIPE,     // |
    CARET,    // ^
    LSHIFT,   // <<
    RSHIFT,   // >>
    LBRACE,   // {
    RBRACE,   // }
    COLON,    // :
    QUESTION, // ?

    // ?€?€ ?°ìˆ  ?°ì‚°???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    PLUS,    // +
    MINUS,   // -
    STAR,    // *
    SLASH,   // /
    PERCENT, // %

    // ?€?€ ë³µí•© ?€???°ì‚°???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    PLUS_EQ,    // +=
    MINUS_EQ,   // -=
    STAR_EQ,    // *=
    SLASH_EQ,   // /=
    PERCENT_EQ, // %=

    // ?€?€ ë¹„êµ ?°ì‚°???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    EQ,      // ==
    NEQ,     // !=
    GT,      // >
    LT,      // <
    GTE,     // >=
    LTE,     // <=

    // ?€?€ ê¸°í? ê¸°í˜¸ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    TILDE,   // ~    (random ë²”ìœ„ êµ¬ë¶„??
    ARROW,   // ->   (?¨ìˆ˜ ë°˜í™˜ ?€???´ë…¸?Œì´??
    DOT,     // .    (?•ì…”?ˆë¦¬ ?‘ê·¼ obj.prop)
    LPAREN,  // (
    RPAREN,  // )
    LBRACKET,// [
    RBRACKET,// ]
    COMMA,   // ,

    // ?€?€ êµ¬ì¡° ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    NEWLINE, // ì¤???
    EOF_T,   // ?Œì¼ ??
};

// ?€?€ ? í° ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
struct Token {
    TT          type;
    std::string value; // ?ë³¸ ?ìŠ¤??(STR?€ ?°ì˜´???œê±°)
    int         line;  // 1ë¶€???œì‘?˜ëŠ” ì¤?ë²ˆí˜¸

    // ?ëŸ¬ ë©”ì‹œì§€???´ë¦„
    std::string type_str() const {
        switch (type) {
            case TT::NUM:     return "?«ì";
            case TT::STR:     return "ë¬¸ì??;
            case TT::IDENT:   return "?ë³„??" + value + ")";
            case TT::IS:      return "is";
            case TT::IF:      return "if";
            case TT::THEN:    return "then";
            case TT::ELSE:    return "else";
            case TT::END:     return "end";
            case TT::WHILE:   return "while";
            case TT::DO:      return "do";
            case TT::FUNC:    return "func";
            case TT::REPEAT:  return "repeat";
            case TT::BREAK:   return "break";
            case TT::RETURN:  return "return";
            case TT::AND:     return "and";
            case TT::OR:      return "or";
            case TT::NOT:     return "not";
            case TT::TRUE:    return "true";
            case TT::FALSE:   return "false";
            case TT::USE:      return "use";
            case TT::CONTINUE: return "continue";
            case TT::FOR:      return "for";
            case TT::IN:       return "in";
            case TT::TO:       return "to";
            case TT::TRY:      return "try";
            case TT::CATCH:    return "catch";
            case TT::THROW:    return "throw";
            case TT::CLASS:    return "class";
            case TT::NEW:      return "new";
            case TT::EXTENDS:  return "extends";
            case TT::SUPER:    return "super";
            case TT::ELIF:     return "elif";
            case TT::FINALLY:  return "finally";
            case TT::FSTR:     return "ë³´ê°„ë¬¸ì??;
            case TT::AMP:      return "&";
            case TT::PIPE:     return "|";
            case TT::CARET:    return "^";
            case TT::LSHIFT:   return "<<";
            case TT::RSHIFT:   return ">>";
            case TT::LBRACE:   return "{";
            case TT::RBRACE:   return "}";
            case TT::COLON:    return ":";
            case TT::QUESTION: return "?";
            case TT::PLUS:       return "+";
            case TT::MINUS:      return "-";
            case TT::STAR:       return "*";
            case TT::SLASH:      return "/";
            case TT::PERCENT:    return "%";
            case TT::PLUS_EQ:    return "+=";
            case TT::MINUS_EQ:   return "-=";
            case TT::STAR_EQ:    return "*=";
            case TT::SLASH_EQ:   return "/=";
            case TT::PERCENT_EQ: return "%=";
            case TT::EQ:      return "==";
            case TT::NEQ:     return "!=";
            case TT::GT:      return ">";
            case TT::LT:      return "<";
            case TT::GTE:     return ">=";
            case TT::LTE:     return "<=";
            case TT::TILDE:   return "~";
            case TT::ARROW:   return "->";
            case TT::DOT:     return ".";
            case TT::LPAREN:  return "(";
            case TT::RPAREN:  return ")";
            case TT::LBRACKET:return "[";
            case TT::RBRACKET:return "]";
            case TT::COMMA:   return ",";
            case TT::NEWLINE: return "ì¤„ë°”ê¿?;
            case TT::EOF_T:   return "?Œì¼??;
        }
        return "?";
    }
};

// ?€?€ ?‰ì„œ ?¤ë¥˜ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
struct LexError : std::runtime_error {
    int line;
    std::string details;
    LexError(const std::string& msg, int ln, const std::string& det = "")
        : std::runtime_error("[?˜ë¼ ?‰ì„œ ?¤ë¥˜] " + std::to_string(ln) + "ì¤? " + msg + (det.empty() ? "" : " (" + det + ")")), line(ln), details(det) {}
};

// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
//  Lexer ?´ë˜??
// ?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•?â•
class Lexer {
    std::string src;
    size_t      pos  = 0;
    int         line = 1;

    // ?€?€ ?¤ì›Œ???Œì´ë¸??€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    inline static const std::map<std::string, TT> KEYWORDS = {
        {"is",     TT::IS},
        {"if",     TT::IF},
        {"then",   TT::THEN},
        {"else",   TT::ELSE},
        {"end",    TT::END},
        {"while",  TT::WHILE},
        {"do",     TT::DO},
        {"func",   TT::FUNC},
        {"repeat", TT::REPEAT},
        {"break",  TT::BREAK},
        {"return", TT::RETURN},
        {"and",    TT::AND},
        {"or",     TT::OR},
        {"not",    TT::NOT},
        {"true",   TT::TRUE},
        {"false",  TT::FALSE},
        {"use",      TT::USE},
        {"continue", TT::CONTINUE},
        {"for",      TT::FOR},
        {"in",       TT::IN},
        {"to",       TT::TO},
        {"try",      TT::TRY},
        {"catch",    TT::CATCH},
        {"throw",    TT::THROW},
        {"class",    TT::CLASS},
        {"new",      TT::NEW},
        {"extends",  TT::EXTENDS},
        {"super",    TT::SUPER},
        {"elif",     TT::ELIF},
        {"finally",  TT::FINALLY},
    };

    // ?€?€ ?¬í¼ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    char peek(int off = 0) const {
        size_t p = pos + off;
        return p < src.size() ? src[p] : '\0';
    }
    char advance()  { return src[pos++]; }
    bool at_end()   const { return pos >= src.size(); }

    bool match(char c) {
        if (!at_end() && src[pos] == c) { pos++; return true; }
        return false;
    }

    Token make(TT t, const std::string& v) const {
        return {t, v, line};
    }

    // ì¤???ê³µë°±(???¤í˜?´ìŠ¤)ë§?ê±´ë„ˆ?€ ??ê°œí–‰?€ ? í°?¼ë¡œ ì²˜ë¦¬
    void skip_spaces() {
        while (!at_end() && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    }

    // ì¤??ê¹Œì§€ ê±´ë„ˆ?€ (ì£¼ì„ ì²˜ë¦¬)
    void skip_line() {
        while (!at_end() && src[pos] != '\n') pos++;
    }

    // ?€?€ ë¬¸ì??ë¦¬í„°???½ê¸° ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    Token read_string() {
        pos++; // ?¬ëŠ” " ê±´ë„ˆ?€
        std::string s;
        bool has_interp = false;
        while (!at_end() && src[pos] != '"') {
            if (src[pos] == '\n') throw LexError("ë¬¸ì?´ì´ ?«íˆì§€ ?Šì•˜?µë‹ˆ??, line);
            if (src[pos] == '\\') {
                pos++;
                switch (src[pos]) {
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    default:   s += '\\'; s += src[pos]; break;
                }
            } else if (src[pos] == '{') {
                has_interp = true;
                s += '{';
                pos++;
                int depth = 1;
                while (!at_end() && depth > 0) {
                    char c = src[pos++];
                    if (c == '{') depth++;
                    else if (c == '}') depth--;
                    s += c;
                }
                continue; // skip outer pos++
            } else {
                s += src[pos];
            }
            pos++;
        }
        if (at_end()) throw LexError("ë¬¸ì?´ì´ ?«íˆì§€ ?Šì•˜?µë‹ˆ??, line);
        pos++; // ?«ëŠ” " ê±´ë„ˆ?€
        return make(has_interp ? TT::FSTR : TT::STR, s);
    }

    // ?€?€ ?«ì ë¦¬í„°???½ê¸° (?•ìˆ˜ / ?¤ìˆ˜) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    Token read_number() {
        std::string n;
        while (!at_end() && (isdigit((unsigned char)src[pos]) || src[pos] == '.')) {
            // ?ì´ ??ë²??˜ì˜¤ë©?ì¤‘ë‹¨
            if (src[pos] == '.' && n.find('.') != std::string::npos) break;
            n += src[pos++];
        }
        return make(TT::NUM, n);
    }

    // ?€?€ ?ë³„??/ ?¤ì›Œ???½ê¸° (?œê? UTF-8 ?¬í•¨) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    // ?œê? UTF-8 ì²?ë°”ì´?? 0xEA ~ 0xED  (ê°€~??ë²”ìœ„)
    // 0x80 ?´ìƒ ë°”ì´?¸ëŠ” ëª¨ë‘ ?ë³„??êµ¬ì„± ë¬¸ìë¡??ˆìš©
    Token read_ident() {
        std::string s;
        while (!at_end()) {
            unsigned char c = (unsigned char)src[pos];
            if (isalnum(c) || c == '_') {
                s += src[pos++];
            } else {
                break;
            }
        }
        auto it = KEYWORDS.find(s);
        if (it != KEYWORDS.end()) return make(it->second, s);
        return make(TT::IDENT, s);
    }

public:
    // ?€?€ ë©”ì¸: ?ŒìŠ¤ ??? í° ë²¡í„° ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    std::vector<Token> tokenize(const std::string& source) {
        src = source;
        pos = 0;
        line = 1;
        std::vector<Token> tokens;
        bool line_has_token = false; // ??ì¤„ì— ? í°???˜ë‚˜?¼ë„ ?ˆì—ˆ?”ì?

        while (!at_end()) {
            skip_spaces();
            if (at_end()) break;

            char c = src[pos];

            // ?€?€ ê°œí–‰ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            if (c == '\n') {
                if (line_has_token) {  // ?´ìš© ?ˆëŠ” ì¤„ë§Œ NEWLINE ë°œí–‰
                    tokens.push_back(make(TT::NEWLINE, "\\n"));
                    line_has_token = false;
                }
                line++; pos++; continue;
            }
            if (c == '\r') { pos++; continue; }

            // ?€?€ ì£¼ì„: # ?ëŠ” // ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            if (c == '#') { skip_line(); continue; }
            if (c == '/' && peek(1) == '/') { skip_line(); continue; }

            // ?€?€ ë¬¸ì???€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            if (c == '"') {
                tokens.push_back(read_string());
                line_has_token = true; continue;
            }

            // ?€?€ ?«ì ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            if (isdigit((unsigned char)c)) {
                tokens.push_back(read_number());
                line_has_token = true; continue;
            }

            // ?€?€ ?ë³„??/ ?¤ì›Œ??(ASCII ?ë¬¸, ?«ì, ?¸ë”ë°”ë§Œ ?ˆìš©) ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            if (isalpha((unsigned char)c) || c == '_') {
                tokens.push_back(read_ident());
                line_has_token = true; continue;
            }

            // ?€?€ ?°ì‚°??/ ê¸°í˜¸ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
            pos++;
            line_has_token = true;
            switch (c) {
                case '+': { bool e=match('='); tokens.push_back(make(e?TT::PLUS_EQ:TT::PLUS,       e?"+=":"+")); break; }
                case '-': {
                    if (match('>'))      tokens.push_back(make(TT::ARROW,    "->"));
                    else if (match('=')) tokens.push_back(make(TT::MINUS_EQ, "-="));
                    else                 tokens.push_back(make(TT::MINUS,    "-"));
                    break;
                }
                case '*': { bool e=match('='); tokens.push_back(make(e?TT::STAR_EQ:TT::STAR,       e?"*=":"*")); break; }
                case '/': { bool e=match('='); tokens.push_back(make(e?TT::SLASH_EQ:TT::SLASH,     e?"/=":"/")); break; }
                case '%': { bool e=match('='); tokens.push_back(make(e?TT::PERCENT_EQ:TT::PERCENT, e?"%=":"%")); break; }
                case '~': tokens.push_back(make(TT::TILDE,   "~")); break;
                case '.': tokens.push_back(make(TT::DOT,     ".")); break;
                case '(': tokens.push_back(make(TT::LPAREN,  "(")); break;
                case ')': tokens.push_back(make(TT::RPAREN,  ")")); break;
                case '[': tokens.push_back(make(TT::LBRACKET,"[")); break;
                case ']': tokens.push_back(make(TT::RBRACKET,"]")); break;
                case ',': tokens.push_back(make(TT::COMMA,   ",")); break;
                case '>': {
                    if (match('>'))  tokens.push_back(make(TT::RSHIFT, ">>"));
                    else { bool e=match('='); tokens.push_back(make(e?TT::GTE:TT::GT, e?">=":">")); }
                    break;
                }
                case '<': {
                    if (match('<'))  tokens.push_back(make(TT::LSHIFT, "<<"));
                    else { bool e=match('='); tokens.push_back(make(e?TT::LTE:TT::LT, e?"<=":"<")); }
                    break;
                }
                case '&': tokens.push_back(make(TT::AMP,      "&")); break;
                case '|': tokens.push_back(make(TT::PIPE,     "|")); break;
                case '^': tokens.push_back(make(TT::CARET,    "^")); break;
                case '{': tokens.push_back(make(TT::LBRACE,   "{")); break;
                case '}': tokens.push_back(make(TT::RBRACE,   "}")); break;
                case ':': tokens.push_back(make(TT::COLON,    ":")); break;
                case '?': tokens.push_back(make(TT::QUESTION, "?")); break;
                case '=': {
                    if (match('=')) tokens.push_back(make(TT::EQ,  "=="));
                    else throw LexError("'=' ?¨ë… ?¬ìš© ë¶ˆê? ???€?…ì? 'is', ë¹„êµ??'==' ë¥??°ì„¸??, line);
                    break;
                }
                case '!': {
                    if (match('=')) tokens.push_back(make(TT::NEQ, "!="));
                    else throw LexError("'!' ?¨ë… ?¬ìš© ë¶ˆê? ??ë¶€?•ì? 'not' ???°ì„¸??, line);
                    break;
                }
                default:
                    throw LexError(std::string("?????†ëŠ” ë¬¸ì: '") + c + "'", line);
            }
        }

        // ë§ˆì?ë§?ì¤?NEWLINE???†ì„ ???ˆìœ¼ë¯€ë¡?ì²˜ë¦¬
        if (line_has_token) tokens.push_back(make(TT::NEWLINE, "\\n"));
        tokens.push_back(make(TT::EOF_T, ""));
        return tokens;
    }

    // ?€?€ ?”ë²„ê·? ? í° ëª©ë¡ ì¶œë ¥ ?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€?€
    static void dump(const std::vector<Token>& tokens) {
        for (auto& t : tokens) {
            if (t.type == TT::NEWLINE) {
                printf("%3d | NEWLINE\n", t.line);
            } else if (t.type == TT::EOF_T) {
                printf("%3d | EOF\n", t.line);
            } else {
                printf("%3d | %-12s '%s'\n", t.line, t.type_str().c_str(), t.value.c_str());
            }
        }
    }
};
