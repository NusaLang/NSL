#include "layout.hpp"

#include <algorithm>
#include <set>

#include "i18n.hpp"

namespace {

bool isBlockKeyword(TokenType t) {
    switch (t) {
        case TokenType::If: case TokenType::Else: case TokenType::While: case TokenType::For:
        case TokenType::Fn: case TokenType::Class: case TokenType::Try: case TokenType::Catch:
        case TokenType::Finally: case TokenType::Struct: case TokenType::EnumKw:
            return true;
        default:
            return false;
    }
}

// A line ending in one of these has not finished its statement.
bool continuesLine(const Token& t) {
    switch (t.type) {
        case TokenType::Plus: case TokenType::Minus: case TokenType::Star: case TokenType::Slash:
        case TokenType::Percent: case TokenType::Eq: case TokenType::EqEq: case TokenType::Neq:
        case TokenType::Lt: case TokenType::Lte: case TokenType::Gt: case TokenType::Gte:
        case TokenType::PlusEq: case TokenType::MinusEq: case TokenType::StarEq: case TokenType::SlashEq:
        case TokenType::And: case TokenType::Or: case TokenType::Comma: case TokenType::Dot:
        case TokenType::LParen: case TokenType::LBracket:
            return true;
        case TokenType::Not:
            return true;
        case TokenType::Ident:
            return t.text == "and" || t.text == "or" || t.text == "dan" || t.text == "atau";
        default:
            return false;
    }
}

struct Lines {
    const std::string& src;
    std::vector<size_t> starts{0};
    explicit Lines(const std::string& s) : src(s) {
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\n') starts.push_back(i + 1);
        }
    }
    int lineOf(int offset) const {
        auto it = std::upper_bound(starts.begin(), starts.end(), static_cast<size_t>(std::max(offset, 0)));
        return static_cast<int>(it - starts.begin());  // 1-based
    }
    // Leading whitespace of a line, tabs advancing to the next multiple of 8.
    int indentOf(int line) const {
        size_t i = starts[static_cast<size_t>(line - 1)];
        int col = 0;
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) {
            col = src[i] == '\t' ? (col / 8 + 1) * 8 : col + 1;
            i++;
        }
        return col;
    }
};

Token makeTok(TokenType type, const std::string& text, Span at) {
    Token t;
    t.type = type;
    t.text = text;
    t.span = at;
    return t;
}

bool usesLayout(const std::vector<Token>& toks, const Lines& lines) {
    // Braces anywhere mean brace-style: check the whole file before anything else.
    for (const Token& t : toks) {
        if (t.type == TokenType::LBrace || t.type == TokenType::RBrace) return false;
    }
    int depth = 0;
    bool anySemi = false;
    int firstLine = -1, lastLine = -1;
    TokenType lineFirst = TokenType::Eof;
    int prevEndLine = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        const Token& t = toks[i];
        if (t.type == TokenType::Eof) break;
        if (t.type == TokenType::LBrace || t.type == TokenType::RBrace) return false;
        if (t.type == TokenType::Semi) anySemi = true;
        int sl = lines.lineOf(t.span.start);
        int el = lines.lineOf(std::max(t.span.end - 1, t.span.start));
        if (firstLine < 0) firstLine = sl;
        lastLine = std::max(lastLine, el);
        if (sl > prevEndLine) lineFirst = t.type;
        prevEndLine = std::max(prevEndLine, el);
        if (t.type == TokenType::LParen || t.type == TokenType::LBracket || t.type == TokenType::LDict) depth++;
        else if (t.type == TokenType::RParen || t.type == TokenType::RBracket || t.type == TokenType::RDict) depth--;
        else if (t.type == TokenType::Colon && depth == 0) {
            bool eol = (i + 1 >= toks.size()) || toks[i + 1].type == TokenType::Eof ||
                       lines.lineOf(toks[i + 1].span.start) > el;
            if (eol || isBlockKeyword(lineFirst)) return true;
        }
    }
    return !anySemi && firstLine >= 0;  // no braces, no `;`: only Python layout can parse it
}

struct Scope {
    std::set<std::string> names;
    std::set<std::string> outer;         // `global` / `nonlocal`
    size_t bodyStart = 0;                // index in `out` where hoisted lets go
    int bodyDepth = 0;                   // block depth of the body's own statements
    std::vector<std::pair<std::string, Span>> hoisted;
};

struct BlockInfo {
    bool ownsScope = false;
    bool commaMode = false;  // struct/enum bodies separate members with commas
};

}  // namespace

std::vector<Token> applyLayout(std::vector<Token> toks, const std::string& source) {
    Lines lines(source);
    if (!usesLayout(toks, lines)) return toks;

    std::vector<Token> out;
    out.reserve(toks.size() + toks.size() / 4);
    std::vector<int> indents{0};
    std::vector<BlockInfo> blocks{BlockInfo{}};
    std::vector<Scope> scopes(1);
    struct Inline { bool ownsScope; };
    std::vector<Inline> inlineOpen;

    int depth = 0;                 // ( and [ nesting
    int prevEndLine = 0;
    bool pendingBlock = false;     // ':' at end of line seen; next line must indent
    BlockInfo pendingInfo;
    std::vector<std::string> pendingParams;
    bool pendingIsFn = false;
    bool atStmtStart = false;
    TokenType lineFirst = TokenType::Eof;
    size_t lineFirstOut = 0;       // index in `out` of the logical line's first token
    bool lineOpen = false;         // a logical line has tokens that still need a terminator

    auto curDepth = [&]() { return static_cast<int>(indents.size()) - 1 + static_cast<int>(inlineOpen.size()); };
    auto spanAfterLast = [&]() {
        if (out.empty()) return Span{0, 0, 1, 1};
        Span s = out.back().span;
        return Span{s.end, s.end, s.line, s.column};
    };

    auto finishScope = [&]() {
        Scope sc = std::move(scopes.back());
        scopes.pop_back();
        // Names first assigned inside nested blocks are declared once at the top
        // of the function, so they stay visible afterwards like in Python.
        std::vector<Token> decls;
        for (auto& [name, sp] : sc.hoisted) {
            decls.push_back(makeTok(TokenType::Let, "let", sp));
            decls.push_back(makeTok(TokenType::Ident, name, sp));
            decls.push_back(makeTok(TokenType::Eq, "=", sp));
            decls.push_back(makeTok(TokenType::Null_, "None", sp));
            decls.push_back(makeTok(TokenType::Semi, ";", sp));
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(sc.bodyStart), decls.begin(), decls.end());
    };

    auto endLogicalLine = [&](bool commaMode) {
        // Close blocks opened inline on this line (`if x: y`).
        while (!inlineOpen.empty()) {
            if (!out.empty() && out.back().type != TokenType::Semi && out.back().type != TokenType::LBrace &&
                out.back().type != TokenType::RBrace) {
                out.push_back(makeTok(TokenType::Semi, ";", spanAfterLast()));
            }
            out.push_back(makeTok(TokenType::RBrace, "}", spanAfterLast()));
            if (inlineOpen.back().ownsScope) finishScope();
            inlineOpen.pop_back();
        }
        if (lineOpen && !out.empty()) {
            TokenType last = out.back().type;
            if (commaMode) {
                if (last != TokenType::Comma && last != TokenType::LBrace)
                    out.push_back(makeTok(TokenType::Comma, ",", spanAfterLast()));
            } else if (last != TokenType::Semi && last != TokenType::LBrace && last != TokenType::RBrace) {
                out.push_back(makeTok(TokenType::Semi, ";", spanAfterLast()));
            }
        }
        lineOpen = false;
    };

    auto popBlock = [&]() {
        BlockInfo bi = blocks.back();
        out.push_back(makeTok(TokenType::RBrace, "}", spanAfterLast()));
        if (bi.ownsScope) finishScope();
        indents.pop_back();
        blocks.pop_back();
    };

    // Params of `def name(a, b: t, c)` from tokens already in `out`.
    auto collectParams = [&](size_t from) {
        std::vector<std::string> ps;
        int d = 0;
        bool afterColon = false;
        for (size_t k = from; k < out.size(); k++) {
            const Token& t = out[k];
            if (t.type == TokenType::LParen) { d++; continue; }
            if (t.type == TokenType::RParen) { d--; if (d == 0) break; continue; }
            if (d != 1) continue;
            if (t.type == TokenType::Comma) afterColon = false;
            else if (t.type == TokenType::Colon) afterColon = true;
            else if (t.type == TokenType::Ident && !afterColon) ps.push_back(t.text);
        }
        return ps;
    };

    auto declare = [&](const std::string& name) { scopes.back().names.insert(name); };

    for (size_t i = 0; i < toks.size(); i++) {
        Token t = toks[i];
        bool isEof = t.type == TokenType::Eof;
        int sl = isEof ? prevEndLine + 1 : lines.lineOf(t.span.start);
        int el = isEof ? sl : lines.lineOf(std::max(t.span.end - 1, t.span.start));
        bool physicalStart = out.empty() && i == 0 ? true : (sl > prevEndLine);

        bool newLogical = false;
        if (depth == 0 && physicalStart) {
            bool continuation = !out.empty() && !pendingBlock && lineOpen && continuesLine(out.back());
            if (!continuation) newLogical = true;
        }

        if (newLogical) {
            int indent = isEof ? 0 : lines.indentOf(sl);
            if (pendingBlock) {
                if (isEof || indent <= indents.back()) {
                    throw LexError(i18n::tr("Blok berindentasi diharapkan setelah ':'",
                                            "Expected an indented block after ':'"),
                                   t.span.line, t.span.column);
                }
                indents.push_back(indent);
                blocks.push_back(pendingInfo);
                if (pendingIsFn) {
                    Scope sc;
                    sc.bodyStart = out.size();
                    sc.bodyDepth = curDepth();
                    for (auto& p : pendingParams) sc.names.insert(p);
                    scopes.push_back(std::move(sc));
                }
                pendingBlock = false;
                pendingIsFn = false;
                pendingParams.clear();
            } else {
                endLogicalLine(blocks.back().commaMode);
                bool dedented = false;
                while (indent < indents.back()) {
                    popBlock();
                    dedented = true;
                }
                if (indent > indents.back() && !dedented) {
                    throw LexError(i18n::tr("Indentasi tak terduga", "Unexpected indent"), t.span.line, t.span.column);
                }
                if (indent != indents.back()) {
                    throw LexError(i18n::tr("Indentasi nggak konsisten", "Inconsistent indentation"),
                                   t.span.line, t.span.column);
                }
            }
            if (!isEof) {
                atStmtStart = true;
                lineFirst = t.type;
                lineFirstOut = out.size();
            }
        }

        if (isEof) {
            // Blocks are closed by the loop above (indent 0); flush the base scope.
            while (indents.size() > 1) popBlock();
            finishScope();
            out.push_back(t);
            return out;
        }

        // ---- statement-start handling (implicit declarations, global, imports) ----
        if (atStmtStart && depth == 0) {
            atStmtStart = false;
            if (t.type == TokenType::Ident && (t.text == "global" || t.text == "nonlocal" || t.text == "umum") &&
                i + 1 < toks.size() && toks[i + 1].type == TokenType::Ident) {
                size_t k = i + 1;
                int declLine = lines.lineOf(t.span.start);
                while (k < toks.size() && toks[k].type == TokenType::Ident &&
                       lines.lineOf(toks[k].span.start) == declLine) {
                    scopes.back().outer.insert(toks[k].text);
                    k++;
                    if (k < toks.size() && toks[k].type == TokenType::Comma) k++;
                }
                i = k - 1;
                prevEndLine = lines.lineOf(std::max(toks[i].span.end - 1, toks[i].span.start));
                continue;  // swallowed: emits nothing, opens no logical line
            }
            if (t.type == TokenType::Ident && i + 1 < toks.size() && toks[i + 1].type == TokenType::Eq) {
                Scope& sc = scopes.back();
                if (!sc.names.count(t.text) && !sc.outer.count(t.text)) {
                    sc.names.insert(t.text);
                    if (curDepth() == sc.bodyDepth) {
                        out.push_back(makeTok(TokenType::Let, "let", t.span));
                    } else {
                        sc.hoisted.push_back({t.text, t.span});
                    }
                }
            } else if (t.type == TokenType::Let && i + 1 < toks.size() && toks[i + 1].type == TokenType::Ident) {
                declare(toks[i + 1].text);
            } else if ((t.type == TokenType::Fn || t.type == TokenType::Class) && i + 1 < toks.size() &&
                       toks[i + 1].type == TokenType::Ident) {
                declare(toks[i + 1].text);
            } else if (t.type == TokenType::Catch) {
                for (size_t k = i + 1; k < toks.size() && k < i + 6; k++) {
                    if (toks[k].type == TokenType::Colon || toks[k].type == TokenType::LBrace) break;
                    if (toks[k].type == TokenType::Ident) declare(toks[k].text);
                }
            } else if (t.type == TokenType::Ident &&
                       (t.text == "import" || t.text == "impor" || t.text == "from" || t.text == "dari")) {
                // Names bound by an import statement: every identifier on the line
                // that isn't a keyword-ish word, plus aliases.
                int lineNo = lines.lineOf(t.span.start);
                for (size_t k = i + 1; k < toks.size() && toks[k].type != TokenType::Eof &&
                                       lines.lineOf(toks[k].span.start) == lineNo; k++) {
                    if (toks[k].type != TokenType::Ident) continue;
                    const std::string& w = toks[k].text;
                    if (w == "import" || w == "impor" || w == "as" || w == "sbg") continue;
                    declare(w);
                }
            }
        }

        // ---- bracket depth ----
        if (t.type == TokenType::LParen || t.type == TokenType::LBracket || t.type == TokenType::LDict) depth++;
        else if ((t.type == TokenType::RParen || t.type == TokenType::RBracket || t.type == TokenType::RDict) && depth > 0) depth--;

        // ---- block-opening ':' ----
        if (t.type == TokenType::Colon && depth == 0) {
            bool eol = (i + 1 >= toks.size()) || toks[i + 1].type == TokenType::Eof ||
                       lines.lineOf(toks[i + 1].span.start) > el;
            bool header = isBlockKeyword(lineFirst) && !out.empty();
            if (eol || header) {
                bool isFn = lineFirst == TokenType::Fn;
                std::vector<std::string> params = isFn ? collectParams(lineFirstOut) : std::vector<std::string>{};
                t.type = TokenType::LBrace;
                t.text = "{";
                out.push_back(t);
                if (eol) {
                    pendingBlock = true;
                    pendingIsFn = isFn;
                    pendingParams = std::move(params);
                    pendingInfo = BlockInfo{isFn, lineFirst == TokenType::Struct || lineFirst == TokenType::EnumKw};
                    lineOpen = false;
                } else {
                    Scope sc;
                    if (isFn) {
                        sc.bodyStart = out.size();
                        sc.bodyDepth = curDepth() + 1;
                        for (auto& p : params) sc.names.insert(p);
                        scopes.push_back(std::move(sc));
                    }
                    inlineOpen.push_back({isFn});
                    atStmtStart = true;
                    lineOpen = false;
                }
                prevEndLine = std::max(prevEndLine, el);
                continue;
            }
        }

        out.push_back(t);
        lineOpen = true;
        prevEndLine = std::max(prevEndLine, el);
    }
    return out;
}
