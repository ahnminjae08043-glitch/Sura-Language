#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  ?�라(SURA) Lexer  ??v2.0
//  ?�스 문자?????�큰 ?�트�?
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═

// ?�?� ?�큰 종류 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
enum class TT {
    // ?�?� 리터???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    NUM,    // 42  /  3.14
    STR,    // "?�녕?�세??  (?�옴???�거??�?
    IDENT,  // 변?�명, ?�수�?(?��? ?�함)

    // ?�?� ?�워???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    IS,        // is      (?�??
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
    USE,       // use      (?�이브러�?불러?�기)
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
    FSTR,      // "문자??{보간}"  (보간 문자??

    // ?�?� 비트 / ?�리 보조 기호 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    AMP,      // &
    PIPE,     // |
    CARET,    // ^
    LSHIFT,   // <<
    RSHIFT,   // >>
    LBRACE,   // {
    RBRACE,   // }
    COLON,    // :
    QUESTION, // ?

    // ?�?� ?�술 ?�산???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    PLUS,    // +
    MINUS,   // -
    STAR,    // *
    SLASH,   // /
    PERCENT, // %

    // ?�?� 복합 ?�???�산???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    PLUS_EQ,    // +=
    MINUS_EQ,   // -=
    STAR_EQ,    // *=
    SLASH_EQ,   // /=
    PERCENT_EQ, // %=

    // ?�?� 비교 ?�산???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    EQ,      // ==
    NEQ,     // !=
    GT,      // >
    LT,      // <
    GTE,     // >=
    LTE,     // <=

    // ?�?� 기�? 기호 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    TILDE,   // ~    (random 범위 구분??
    ARROW,   // ->   (?�수 반환 ?�???�노?�이??
    DOT,     // .    (?�셔?�리 ?�근 obj.prop)
    LPAREN,  // (
    RPAREN,  // )
    LBRACKET,// [
    RBRACKET,// ]
    COMMA,   // ,

    // ?�?� 구조 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    NEWLINE, // �???
    EOF_T,   // ?�일 ??
};

// ?�?� ?�큰 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
struct Token {
    TT          type;
    std::string value; // ?�본 ?�스??(STR?� ?�옴???�거)
    int         line;  // 1부???�작?�는 �?번호

    // ?�러 메시지???�름
    std::string type_str() const {
        switch (type) {
            case TT::NUM:     return "?�자";
            case TT::STR:     return "문자??;
            case TT::IDENT:   return "?�별??" + value + ")";
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
            case TT::FSTR:     return "보간문자??;
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
            case TT::NEWLINE: return "줄바�?;
            case TT::EOF_T:   return "?�일??;
        }
        return "?";
    }
};

// ?�?� ?�서 ?�류 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
struct LexError : std::runtime_error {
    int line;
    std::string details;
    LexError(const std::string& msg, int ln, const std::string& det = "")
        : std::runtime_error("[?�라 ?�서 ?�류] " + std::to_string(ln) + "�? " + msg + (det.empty() ? "" : " (" + det + ")")), line(ln), details(det) {}
};

// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
//  Lexer ?�래??
// ?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═?�═
class Lexer {
    std::string src;
    size_t      pos  = 0;
    int         line = 1;

    // ?�?� ?�워???�이�??�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // ?�?� ?�퍼 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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

    // �???공백(???�페?�스)�?건너?� ??개행?� ?�큰?�로 처리
    void skip_spaces() {
        while (!at_end() && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    }

    // �??�까지 건너?� (주석 처리)
    void skip_line() {
        while (!at_end() && src[pos] != '\n') pos++;
    }

    // ?�?� 문자??리터???�기 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    Token read_string() {
        pos++; // ?�는 " 건너?�
        std::string s;
        bool has_interp = false;
        while (!at_end() && src[pos] != '"') {
            if (src[pos] == '\n') throw LexError("문자?�이 ?�히지 ?�았?�니??, line);
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
        if (at_end()) throw LexError("문자?�이 ?�히지 ?�았?�니??, line);
        pos++; // ?�는 " 건너?�
        return make(has_interp ? TT::FSTR : TT::STR, s);
    }

    // ?�?� ?�자 리터???�기 (?�수 / ?�수) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    Token read_number() {
        std::string n;
        while (!at_end() && (isdigit((unsigned char)src[pos]) || src[pos] == '.')) {
            // ?�이 ??�??�오�?중단
            if (src[pos] == '.' && n.find('.') != std::string::npos) break;
            n += src[pos++];
        }
        return make(TT::NUM, n);
    }

    // ?�?� ?�별??/ ?�워???�기 (?��? UTF-8 ?�함) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    // ?��? UTF-8 �?바이?? 0xEA ~ 0xED  (가~??범위)
    // 0x80 ?�상 바이?�는 모두 ?�별??구성 문자�??�용
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
    // ?�?� 메인: ?�스 ???�큰 벡터 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
    std::vector<Token> tokenize(const std::string& source) {
        src = source;
        pos = 0;
        line = 1;
        std::vector<Token> tokens;
        bool line_has_token = false; // ??줄에 ?�큰???�나?�도 ?�었?��?

        while (!at_end()) {
            skip_spaces();
            if (at_end()) break;

            char c = src[pos];

            // ?�?� 개행 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
            if (c == '\n') {
                if (line_has_token) {  // ?�용 ?�는 줄만 NEWLINE 발행
                    tokens.push_back(make(TT::NEWLINE, "\\n"));
                    line_has_token = false;
                }
                line++; pos++; continue;
            }
            if (c == '\r') { pos++; continue; }

            // ?�?� 주석: # ?�는 // ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
            if (c == '#') { skip_line(); continue; }
            if (c == '/' && peek(1) == '/') { skip_line(); continue; }

            // ?�?� 문자???�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
            if (c == '"') {
                tokens.push_back(read_string());
                line_has_token = true; continue;
            }

            // ?�?� ?�자 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
            if (isdigit((unsigned char)c)) {
                tokens.push_back(read_number());
                line_has_token = true; continue;
            }

            // ?�?� ?�별??/ ?�워??(ASCII ?�문, ?�자, ?�더바만 ?�용) ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
            if (isalpha((unsigned char)c) || c == '_') {
                tokens.push_back(read_ident());
                line_has_token = true; continue;
            }

            // ?�?� ?�산??/ 기호 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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
                    else throw LexError("'=' ?�독 ?�용 불�? ???�?��? 'is', 비교??'==' �??�세??, line);
                    break;
                }
                case '!': {
                    if (match('=')) tokens.push_back(make(TT::NEQ, "!="));
                    else throw LexError("'!' ?�독 ?�용 불�? ??부?��? 'not' ???�세??, line);
                    break;
                }
                default:
                    throw LexError(std::string("?????�는 문자: '") + c + "'", line);
            }
        }

        // 마�?�?�?NEWLINE???�을 ???�으므�?처리
        if (line_has_token) tokens.push_back(make(TT::NEWLINE, "\\n"));
        tokens.push_back(make(TT::EOF_T, ""));
        return tokens;
    }

    // ?�?� ?�버�? ?�큰 목록 출력 ?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�?�
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
