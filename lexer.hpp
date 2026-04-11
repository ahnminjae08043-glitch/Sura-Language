#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>







enum class TT {
    
    NUM,    
    STR,    
    IDENT,  

    
    IS,        
    IF,        
    THEN,      
    ELSE,      
    END,       
    WHILE,     
    DO,        
    FUNC,      
    REPEAT,    
    BREAK,     
    RETURN,    
    AND,       
    OR,        
    NOT,       
    TRUE,      
    FALSE,     
    USE,       
    CONTINUE,  
    FOR,       
    IN,        
    TO,        
    TRY,       
    CATCH,     
    THROW,     
    CLASS,     
    NEW,       
    EXTENDS,   
    SUPER,     
    ELIF,      
    FINALLY,   
    FSTR,      

    
    AMP,      
    PIPE,     
    CARET,    
    LSHIFT,   
    RSHIFT,   
    LBRACE,   
    RBRACE,   
    COLON,    
    QUESTION, 

    
    PLUS,    
    MINUS,   
    STAR,    
    SLASH,   
    PERCENT, 

    
    PLUS_EQ,    
    MINUS_EQ,   
    STAR_EQ,    
    SLASH_EQ,   
    PERCENT_EQ, 

    
    EQ,      
    NEQ,     
    GT,      
    LT,      
    GTE,     
    LTE,     

    
    TILDE,   
    ARROW,   
    DOT,     
    LPAREN,  
    RPAREN,  
    LBRACKET,
    RBRACKET,
    COMMA,   

    
    NEWLINE, 
    EOF_T,   
};


struct Token {
    TT          type;
    std::string value; 
    int         line;  

    
    std::string type_str() const {
        switch (type) {
            case TT::NUM:     return "?�자";
            case TT::STR:     return "문자열";
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
            case TT::FSTR:    return "보간문자열";
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
            case TT::NEWLINE: return "줄바꿈";
            case TT::EOF_T:   return "파일끝";
        }
        return "?";
    }
};


struct LexError : std::runtime_error {
    int line;
    std::string details;
    LexError(const std::string& msg, int ln, const std::string& det = "")
        : std::runtime_error("[?�라 ?�서 ?�류] " + std::to_string(ln) + "�? " + msg + (det.empty() ? "" : " (" + det + ")")), line(ln), details(det) {}
};




class Lexer {
    std::string src;
    size_t      pos  = 0;
    int         line = 1;

    
    static const std::map<std::string, TT>& get_keywords() {
        static const std::map<std::string, TT> KEYWORDS = {
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
        return KEYWORDS;
    }

    
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

    
    void skip_spaces() {
        while (!at_end() && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    }

    
    void skip_line() {
        while (!at_end() && src[pos] != '\n') pos++;
    }

    
    Token read_string() {
        pos++; 
        std::string s;
        bool has_interp = false;
        while (!at_end() && src[pos] != '"') {
            if (src[pos] == '\n') throw LexError("문자열이 닫히지 않았습니다.", line);
            if (src[pos] == '\\') {
                pos++;
                if (at_end()) throw LexError("문자열이 닫히지 않았습니다.", line);
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
                continue; 
            } else {
                s += src[pos];
            }
            pos++;
        }
        if (at_end()) throw LexError("문자열이 닫히지 않았습니다.", line);
        pos++; 
        return make(has_interp ? TT::FSTR : TT::STR, s);
    }

    
    Token read_number() {
        std::string n;
        while (!at_end() && (isdigit((unsigned char)src[pos]) || src[pos] == '.')) {
            
            if (src[pos] == '.' && n.find('.') != std::string::npos) break;
            n += src[pos++];
        }
        return make(TT::NUM, n);
    }

    
    
    
    Token read_ident() {
        std::string s;
        while (!at_end()) {
            unsigned char c = (unsigned char)src[pos];
            if (isalnum(c) || c == '_' || c >= 0x80) {
                // 0x80 이상은 UTF-8 다중 바이트 문자 (한국어 등)
                s += src[pos++];
            } else {
                break;
            }
        }
        auto it = get_keywords().find(s);
        if (it != get_keywords().end()) return make(it->second, s);
        return make(TT::IDENT, s);
    }

public:
    
    std::vector<Token> tokenize(const std::string& source) {
        src = source;
        pos = 0;
        line = 1;
        std::vector<Token> tokens;
        bool line_has_token = false; 

        while (!at_end()) {
            skip_spaces();
            if (at_end()) break;

            char c = src[pos];

            
            if (c == '\n') {
                if (line_has_token) {  
                    tokens.push_back(make(TT::NEWLINE, "\\n"));
                    line_has_token = false;
                }
                line++; pos++; continue;
            }
            if (c == '\r') { pos++; continue; }

            
            if (c == '#') { skip_line(); continue; }
            if (c == '/' && peek(1) == '/') { skip_line(); continue; }

            
            if (c == '"') {
                tokens.push_back(read_string());
                line_has_token = true; continue;
            }

            
            if (isdigit((unsigned char)c)) {
                tokens.push_back(read_number());
                line_has_token = true; continue;
            }

            
            if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
                // 0x80 이상은 UTF-8 다중 바이트 문자 (한국어 식별자 지원)
                tokens.push_back(read_ident());
                line_has_token = true; continue;
            }

            
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
                    else throw LexError("'=' 단독 사용 불가. 할당은 'is', 비교는 '==' 을 사용하세요.", line);
                    break;
                }
                case '!': {
                    if (match('=')) tokens.push_back(make(TT::NEQ, "!="));
                    else throw LexError("'!' 단독 사용 불가. 부정은 'not' 을 사용하세요.", line);
                    break;
                }
                default:
                    throw LexError(std::string("?????�는 문자: '") + c + "'", line);
            }
        }

        
        if (line_has_token) tokens.push_back(make(TT::NEWLINE, "\\n"));
        tokens.push_back(make(TT::EOF_T, ""));
        return tokens;
    }

    
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
};
};
