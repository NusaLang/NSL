#include "lexer.hpp"
#include <cctype>
#include <unordered_map>

#include "i18n.hpp"

namespace {
// Indonesian keywords, each with an English alias mapping to the same
// TokenType (fungsi/func, buat/let, jika/if, ...) -- interchangeable,
// not two separate features to keep in sync.
const std::unordered_map<std::string, TokenType> kKeywords = {
    {"buat", TokenType::Let},        {"let", TokenType::Let},
    {"fungsi", TokenType::Fn},       {"func", TokenType::Fn},
    {"function", TokenType::Fn},
    {"jika", TokenType::If},         {"if", TokenType::If},
    {"lain", TokenType::Else},       {"else", TokenType::Else},
    {"selama", TokenType::While},    {"while", TokenType::While},
    {"untuk", TokenType::For},       {"for", TokenType::For},
    {"hasil", TokenType::Return},    {"return", TokenType::Return},
    {"berhenti", TokenType::Break},  {"break", TokenType::Break},
    {"lanjut", TokenType::Continue}, {"continue", TokenType::Continue},
    {"benar", TokenType::True_},     {"true", TokenType::True_},
    {"salah", TokenType::False_},    {"false", TokenType::False_},
    {"kosong", TokenType::Null_},    {"null", TokenType::Null_},
    {"kelas", TokenType::Class},     {"class", TokenType::Class},
    {"ini", TokenType::This},        {"this", TokenType::This},
    {"induk", TokenType::Super},     {"super", TokenType::Super},
    {"turunan", TokenType::Extends}, {"extends", TokenType::Extends},
    {"bentuk", TokenType::Struct},   {"struct", TokenType::Struct},
    {"jenis", TokenType::EnumKw},    {"enum", TokenType::EnumKw},
    {"coba", TokenType::Try},        {"try", TokenType::Try},
    {"tangkap", TokenType::Catch},   {"catch", TokenType::Catch},
    {"akhirnya", TokenType::Finally},{"finally", TokenType::Finally},
    {"lempar", TokenType::Throw},    {"throw", TokenType::Throw},
};
}  // namespace

LexError::LexError(const std::string& msg, int line, int column)
    : std::runtime_error(msg + " (line " + std::to_string(line) + ", col " +
                          std::to_string(column) + ")"),
      line(line),
      column(column) {}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

char Lexer::peek(int offset) const {
    size_t idx = pos_ + static_cast<size_t>(offset);
    return idx < source_.size() ? source_[idx] : '\0';
}

char Lexer::advance() {
    char ch = source_[pos_++];
    if (ch == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return ch;
}

void Lexer::skipWhitespaceAndComments() {
    while (pos_ < source_.size()) {
        char ch = peek();
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            advance();
        } else if (ch == '/' && peek(1) == '/') {
            while (pos_ < source_.size() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

std::vector<Token> Lexer::nextTokens() {
    skipWhitespaceAndComments();
    if (pos_ >= source_.size()) {
        Token eof;
        eof.type = TokenType::Eof;
        eof.span = {static_cast<int>(pos_), static_cast<int>(pos_), line_, column_};
        return {eof};
    }
    size_t start = pos_;
    int line = line_, col = column_;
    char ch = peek();

    if (std::isdigit(static_cast<unsigned char>(ch))) {
        return {readNumber(start, line, col)};
    } else if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
        return {readIdent(start, line, col)};
    } else if (ch == '"') {
        return {readString(start, line, col)};
    } else if (ch == '`') {
        return readBacktickTemplate(start, line, col);
    } else {
        return {readSymbol(start, line, col)};
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        std::vector<Token> next = nextTokens();
        bool sawEof = !next.empty() && next.back().type == TokenType::Eof;
        for (auto& t : next) tokens.push_back(std::move(t));
        if (sawEof) break;
    }
    return tokens;
}

Token Lexer::readNumber(size_t start, int line, int col) {
    while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }
    std::string text = source_.substr(start, pos_ - start);
    Token tok;
    tok.type = TokenType::Number;
    tok.number = std::stod(text);
    tok.text = text;
    tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
    return tok;
}

Token Lexer::readIdent(size_t start, int line, int col) {
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
    std::string text = source_.substr(start, pos_ - start);
    Token tok;
    auto it = kKeywords.find(text);
    tok.type = (it != kKeywords.end()) ? it->second : TokenType::Ident;
    tok.text = text;
    tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
    return tok;
}

Token Lexer::readString(size_t start, int line, int col) {
    advance();  // opening quote
    std::string value;
    while (peek() != '"') {
        if (pos_ >= source_.size()) {
            throw LexError(i18n::tr("Teks nggak ditutup", "Unterminated string"), line, col);
        }
        char ch = advance();
        if (ch == '\\') {
            char esc = advance();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '0': value += '\0'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                default: value += esc; break;
            }
        } else {
            value += ch;
        }
    }
    advance();  // closing quote
    Token tok;
    tok.type = TokenType::String;
    tok.str = value;
    tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
    return tok;
}

// Template strings desugar to a `+`/`ke_teks(...)` token chain at lex
// time (no TemplateStringExpr AST node needed), so errors inside
// `${...}` keep normal spans. Brace depth for `${...}` tracks
// LBrace/RBrace TOKENS, not raw chars, so nested strings/templates
// work correctly.
std::vector<Token> Lexer::readBacktickTemplate(size_t start, int line, int col) {
    advance();  // opening backtick

    struct Part {
        bool isLiteral;
        std::string literal;
        std::vector<Token> exprTokens;
    };
    std::vector<Part> parts;
    std::string currentLiteral;
    auto flushLiteral = [&]() {
        parts.push_back(Part{true, currentLiteral, {}});
        currentLiteral.clear();
    };

    while (true) {
        if (pos_ >= source_.size()) {
            throw LexError(i18n::tr("Template string nggak ditutup", "Unterminated template string"), line, col);
        }
        char ch = peek();
        if (ch == '`') {
            advance();  // closing backtick
            break;
        }
        if (ch == '$' && peek(1) == '{') {
            flushLiteral();
            advance();
            advance();  // consume "${"
            std::vector<Token> exprTokens;
            int braceDepth = 1;
            while (true) {
                std::vector<Token> got = nextTokens();
                for (Token& t : got) {
                    if (t.type == TokenType::Eof) {
                        throw LexError(i18n::tr("${...} nggak ditutup di dalam template string", "Unterminated ${...} in template string"), line, col);
                    }
                    if (t.type == TokenType::LBrace) {
                        braceDepth++;
                    } else if (t.type == TokenType::RBrace) {
                        braceDepth--;
                        if (braceDepth == 0) goto exprDone;  // the '}' closing this ${...}
                    }
                    exprTokens.push_back(std::move(t));
                }
            }
        exprDone:
            parts.push_back(Part{false, "", std::move(exprTokens)});
            continue;
        }
        char c = advance();
        if (c == '\\') {
            if (pos_ >= source_.size()) throw LexError(i18n::tr("Template string nggak ditutup", "Unterminated template string"), line, col);
            char esc = advance();
            switch (esc) {
                case 'n': currentLiteral += '\n'; break;
                case 't': currentLiteral += '\t'; break;
                case 'r': currentLiteral += '\r'; break;
                case '`': currentLiteral += '`'; break;
                case '$': currentLiteral += '$'; break;
                case '\\': currentLiteral += '\\'; break;
                default: currentLiteral += esc; break;
            }
        } else {
            currentLiteral += c;
        }
    }
    flushLiteral();

    auto mkTok = [&](TokenType tt, std::string text = "") {
        Token t;
        t.type = tt;
        t.text = std::move(text);
        t.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
        return t;
    };
    auto mkString = [&](std::string s) {
        Token t;
        t.type = TokenType::String;
        t.str = std::move(s);
        t.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
        return t;
    };

    std::vector<Token> out;
    out.push_back(mkTok(TokenType::LParen));
    bool first = true;
    bool emittedAny = false;
    for (Part& p : parts) {
        if (p.isLiteral && p.literal.empty()) continue;  // "" + x == x, skip the no-op term
        if (!first) out.push_back(mkTok(TokenType::Plus, "+"));
        first = false;
        emittedAny = true;
        if (p.isLiteral) {
            out.push_back(mkString(p.literal));
        } else {
            out.push_back(mkTok(TokenType::Ident, "ke_teks"));
            out.push_back(mkTok(TokenType::LParen));
            for (Token& t : p.exprTokens) out.push_back(std::move(t));
            out.push_back(mkTok(TokenType::RParen));
        }
    }
    if (!emittedAny) out.push_back(mkString(""));  // whole template was ``
    out.push_back(mkTok(TokenType::RParen));
    return out;
}

Token Lexer::readSymbol(size_t start, int line, int col) {
    std::string two = source_.substr(pos_, 2);
    static const std::unordered_map<std::string, TokenType> kTwo = {
        {"==", TokenType::EqEq}, {"!=", TokenType::Neq},
        {"<=", TokenType::Lte},  {">=", TokenType::Gte},
        {"&&", TokenType::And},  {"||", TokenType::Or},
        {"+=", TokenType::PlusEq}, {"-=", TokenType::MinusEq},
        {"*=", TokenType::StarEq}, {"/=", TokenType::SlashEq},
    };
    auto itTwo = kTwo.find(two);
    if (itTwo != kTwo.end()) {
        advance();
        advance();
        Token tok;
        tok.type = itTwo->second;
        tok.text = two;
        tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
        return tok;
    }

    static const std::unordered_map<char, TokenType> kOne = {
        {'+', TokenType::Plus},   {'-', TokenType::Minus}, {'*', TokenType::Star},
        {'/', TokenType::Slash},  {'%', TokenType::Percent}, {'=', TokenType::Eq},
        {'<', TokenType::Lt},     {'>', TokenType::Gt},    {'!', TokenType::Not},
        {'(', TokenType::LParen},{')', TokenType::RParen},{'{', TokenType::LBrace},
        {'}', TokenType::RBrace},{',', TokenType::Comma}, {';', TokenType::Semi},
        {'[', TokenType::LBracket},{']', TokenType::RBracket},{'.', TokenType::Dot},
        {':', TokenType::Colon},  {'&', TokenType::And},   {'|', TokenType::Or},
    };
    char one = peek();
    unsigned char uc = static_cast<unsigned char>(one);
    if (uc >= 0x80) {
        size_t len = 1;
        if ((uc & 0xE0) == 0xC0) len = 2;
        else if ((uc & 0xF0) == 0xE0) len = 3;
        else if ((uc & 0xF8) == 0xF0) len = 4;

        std::string utf8Char = "";
        for (size_t i = 0; i < len && pos_ < source_.size(); i++) {
            utf8Char += advance();
        }
        Token tok;
        tok.type = TokenType::Ident;
        tok.text = utf8Char;
        tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
        return tok;
    }
    auto itOne = kOne.find(one);
    if (itOne != kOne.end()) {
        advance();
        Token tok;
        tok.type = itOne->second;
        tok.text = std::string(1, one);
        tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
        return tok;
    }
    advance();
    Token tok;
    tok.type = TokenType::Ident;
    tok.text = std::string(1, one);
    tok.span = {static_cast<int>(start), static_cast<int>(pos_), line, col};
    return tok;
}
