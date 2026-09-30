#include "parser.hpp"

#include <functional>

#include "i18n.hpp"
#include "lexer.hpp"

namespace {
std::string tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Number: return i18n::tr("angka", "number");
        case TokenType::String: return i18n::tr("teks", "string");
        case TokenType::Ident: return i18n::tr("identifier", "identifier");
        case TokenType::Eof: return i18n::tr("akhir file", "end of file");
        default: return i18n::tr("token", "token");
    }
}
}  // namespace

ParseError::ParseError(const std::string& msg, const Token& token)
    : std::runtime_error(msg + " (line " + std::to_string(token.span.line) +
                          ", col " + std::to_string(token.span.column) + ")") {}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)), strictKeys_(lexerLastWasPython()) {}

const Token& Parser::peek() const { return tokens_[pos_]; }
const Token& Parser::peekAt(size_t offset) const {
    size_t idx = pos_ + offset;
    return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
}
bool Parser::atEnd() const { return peek().type == TokenType::Eof; }

const Token& Parser::advance() {
    const Token& tok = tokens_[pos_];
    if (!atEnd()) pos_++;
    return tok;
}

bool Parser::check(TokenType type) const { return peek().type == type; }

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    throw ParseError(message + i18n::tr(", dapet ", ", got ") + tokenTypeName(peek().type), peek());
}

void Parser::expectEnd(const std::string& message) {
    if (peek().type == TokenType::Semi) { pos_++; return; }
    if (peek().type == TokenType::RBrace || peek().type == TokenType::Eof) return;
    if (pos_ > 0 && peek().span.line > tokens_[pos_ - 1].span.line) return;
    throw ParseError(message, peek());
}

namespace {
const std::vector<std::string>& exceptionNames() {
    static const std::vector<std::string> n = {
        "BaseException", "Exception", "ValueError", "TypeError", "KeyError", "IndexError", "ZeroDivisionError",
        "ArithmeticError", "LookupError", "RuntimeError", "NotImplementedError", "AttributeError", "NameError",
        "OSError", "IOError", "FileNotFoundError", "PermissionError", "TimeoutError", "ConnectionError",
        "StopIteration", "AssertionError", "KeyboardInterrupt", "SystemExit", "ImportError", "ModuleNotFoundError",
        "RecursionError", "UnicodeError", "OverflowError", "EOFError", "Warning", "UserWarning",
        "DeprecationWarning", "GeneratorExit", "StopAsyncIteration", "FileExistsError", "IsADirectoryError",
        "NotADirectoryError", "BrokenPipeError", "UnicodeDecodeError", "UnicodeEncodeError"};
    return n;
}
}  // namespace

// Remembers which builtin exception classes a file mentions, so they can be bound at its top.
void Parser::noteName(const std::string& name) {
    for (const auto& n : exceptionNames()) {
        if (n == name) {
            for (const auto& u : usedExc_) if (u == name) return;
            usedExc_.push_back(name);
            return;
        }
    }
}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    while (!atEnd()) {
        program->statements.push_back(statement());
        for (auto& extra : pendingStmts_) program->statements.push_back(std::move(extra));
        pendingStmts_.clear();
    }
    if (usesGen_) injectGeneratorRuntime(*program);
    injectExceptionClasses(*program);
    return program;
}

StmtPtr Parser::statement() {
    // Stamped uniformly here (rather than in each sub-parser) so every
    // statement kind gets a location for free -- see Stmt::span.
    Span start = peek().span;
    StmtPtr result;
    std::vector<StmtPtr> outerPre = std::move(preStmts_);  // hoisted default values belong to THIS statement
    preStmts_.clear();
    if (isWord(peek(), "pass") && peekAt(1).type == TokenType::Semi) {
        advance();
        advance();
        result = std::make_unique<BlockStmt>(std::vector<StmtPtr>{});
    }
    else if (isWord(peek(), "import", "impor") && (peekAt(1).type == TokenType::Ident || peekAt(1).type == TokenType::String)) {
        advance();
        result = importStmt();
        // `import a, b, c`: each further module is its own import, spliced in after this one.
        while (match(TokenType::Comma)) pendingStmts_.push_back(importStmt());
    }
    else if (isWord(peek(), "from", "dari") && peekAt(1).type == TokenType::Ident) {
        advance();
        result = fromImportStmt();
    }
    else if (check(TokenType::At)) {
        result = decoratedStmt();
    }
    else if (isWord(peek(), "yield") && isWord(peekAt(1), "from", "dari")) {
        result = yieldFromStmt(start);
    }
    else if (isWord(peek(), "with", "dengan") && peekAt(1).type != TokenType::Eq && peekAt(1).type != TokenType::LParen) {
        advance();
        result = withStmt();
    }
    else if (match(TokenType::Let)) result = letStmt();
    else if (match(TokenType::Fn)) result = fnDecl();
    else if (match(TokenType::Class)) {
        result = classDecl();
        if (classPre_) {  // `class A(mod.B)`: the hidden alias of the parent goes first
            pendingStmts_.insert(pendingStmts_.begin(), std::move(result));
            result = std::move(classPre_);
        }
    }
    else if (match(TokenType::Struct)) result = structDecl();
    else if (match(TokenType::EnumKw)) result = enumDecl();
    else if (match(TokenType::Try)) result = tryStmt();
    else if (match(TokenType::Throw)) result = throwStmt();
    else if (match(TokenType::If)) result = ifStmt();
    else if (match(TokenType::While)) {
        result = whileStmt();
        if (check(TokenType::Else)) result = loopElse(std::move(result));
    }
    else if (match(TokenType::For)) {
        loopVarDeclared_ = tokens_[pos_ - 1].text == "for*";
        result = forStmt();
        loopVarDeclared_ = false;
        if (check(TokenType::Else)) result = loopElse(std::move(result));
    }
    else if (match(TokenType::Return)) result = returnStmt();
    else if (match(TokenType::Break)) result = breakStmt();
    else if (match(TokenType::Continue)) result = continueStmt();
    else if (check(TokenType::LBrace)) result = block();
    else result = exprStmt();
    result->span = start;
    if (!preStmts_.empty()) {  // `let __dflt = <expr>` for each mutable default, evaluated once, before the def
        pendingStmts_.insert(pendingStmts_.begin(), std::move(result));
        for (size_t i = preStmts_.size(); i-- > 1;) pendingStmts_.insert(pendingStmts_.begin(), std::move(preStmts_[i]));
        result = std::move(preStmts_[0]);
        result->span = start;
    }
    preStmts_ = std::move(outerPre);
    return result;
}

bool Parser::isWord(const Token& t, const char* a, const char* b) const {
    return t.type == TokenType::Ident && (t.text == a || (b && t.text == b));
}

bool Parser::matchWord(const char* a, const char* b) {
    if (isWord(peek(), a, b)) {
        advance();
        return true;
    }
    return false;
}

namespace {
ExprPtr mkIdent(const std::string& name, Span sp) {
    ExprPtr e = std::make_unique<IdentifierExpr>(name);
    e->span = sp;
    return e;
}
ExprPtr mkCall(const std::string& fn, std::vector<ExprPtr> args, Span sp) {
    ExprPtr e = std::make_unique<CallExpr>(mkIdent(fn, sp), std::move(args));
    e->span = sp;
    return e;
}
ExprPtr mkBin(const std::string& op, ExprPtr l, ExprPtr r, Span sp) {
    ExprPtr e = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
    e->span = sp;
    return e;
}
// Copy of an expression made only of names, literals and x[k] / x.k chains; nullptr for anything else.
ExprPtr cloneSimple(const Expr* e) {
    if (e->kind == ExprKind::Identifier) return mkIdent(static_cast<const IdentifierExpr*>(e)->name, e->span);
    if (e->kind == ExprKind::Literal) {
        auto* l = static_cast<const LiteralExpr*>(e);
        ExprPtr c;
        if (l->litKind == LiteralExpr::Kind::Number) c = LiteralExpr::makeNumber(l->number);
        else if (l->litKind == LiteralExpr::Kind::String) c = LiteralExpr::makeString(l->str);
        else if (l->litKind == LiteralExpr::Kind::Bool) c = LiteralExpr::makeBool(l->boolean);
        else c = LiteralExpr::makeNull();
        c->span = e->span;
        return c;
    }
    if (e->kind == ExprKind::Index) {
        auto* ix = static_cast<const IndexExpr*>(e);
        ExprPtr t = cloneSimple(ix->target.get());
        ExprPtr i = t ? cloneSimple(ix->index.get()) : nullptr;
        if (!t || !i) return nullptr;
        ExprPtr r = std::make_unique<IndexExpr>(std::move(t), std::move(i));
        r->span = e->span;
        return r;
    }
    return nullptr;
}
ExprPtr mkNum(double n, Span sp) {
    ExprPtr e = LiteralExpr::makeNumber(n);
    e->span = sp;
    return e;
}
StmtPtr mkLet(const std::string& name, ExprPtr value, Span sp) {
    StmtPtr s = std::make_unique<LetStmt>(name, std::move(value), "");
    s->span = sp;
    return s;
}
}  // namespace

// Dotted module path `a.b.c` -> "a/b/c" (the form impor() resolves).
static std::string readModulePath(Parser& p, std::function<const Token&()> peekFn,
                                  std::function<const Token&()> advanceFn) {
    (void)p;
    std::string path = advanceFn().text;
    while (peekFn().type == TokenType::Dot) {
        advanceFn();
        path += "/" + advanceFn().text;
    }
    return path;
}

StmtPtr Parser::importStmt() {
    Span sp = peek().span;
    // `import "path"` keeps working for string paths; `import a.b` is the
    // Python form. Either way it is `buat <name> = impor("<path>")`.
    std::string path, name;
    if (check(TokenType::String)) {
        path = advance().str;
        name = path;
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        size_t dot = name.find('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
    } else {
        path = readModulePath(*this, [&]() -> const Token& { return peek(); }, [&]() -> const Token& { return advance(); });
        name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    }
    bool aliased = false;
    if (matchWord("as", "sbg")) {
        name = expect(TokenType::Ident, i18n::tr("Nama alias diharapkan setelah 'as'", "Expected alias after 'as'")).text;
        aliased = true;
    }
    if (!check(TokenType::Comma)) expectEnd( i18n::tr("';' diharapkan setelah 'import'", "Expected ';' after import"));
    size_t firstSlash = path.find('/');
    if (!aliased && firstSlash != std::string::npos && !check(TokenType::String)) {
        // `import a.b.c` binds `a` (Python): a = _pkg("a"), then a.b / a.b.c are attached when importable
        std::string top = path.substr(0, firstSlash);
        std::vector<ExprPtr> pa;
        pa.push_back(LiteralExpr::makeString(top));
        StmtPtr first = mkLet(top, mkCall("_pkg", std::move(pa), sp), sp);
        size_t pos = firstSlash;
        while (pos != std::string::npos) {
            size_t next = path.find('/', pos + 1);
            std::string sub = path.substr(0, next);
            // a.b = <module a/b> when importable (a.b.c walks through the map stored at a.b)
            std::vector<ExprPtr> sa;
            sa.push_back(mkIdent(top, sp));
            sa.push_back(LiteralExpr::makeString(sub));
            StmtPtr st = std::make_unique<ExprStmtNode>(mkCall("_pkgsub", std::move(sa), sp));
            st->span = sp;
            pendingStmts_.push_back(std::move(st));
            pos = next;
        }
        return first;
    }
    std::vector<ExprPtr> args;
    args.push_back(LiteralExpr::makeString(path));
    return mkLet(name, mkCall("impor", std::move(args), sp), sp);
}

StmtPtr Parser::fromImportStmt() {
    Span sp = peek().span;
    std::string path = readModulePath(*this, [&]() -> const Token& { return peek(); }, [&]() -> const Token& { return advance(); });
    if (!matchWord("import", "impor")) {
        throw ParseError(i18n::tr("'import' diharapkan setelah nama modul", "Expected 'import' after module name"), peek());
    }
    // One module load, then one binding per imported name.
    std::vector<StmtPtr> out;
    std::string modVar = "__mod" + std::to_string(hiddenCounter_++);
    std::vector<ExprPtr> args;
    args.push_back(LiteralExpr::makeString(path));
    out.push_back(mkLet(modVar, mkCall("impor", std::move(args), sp), sp));
    do {
        std::string field = expect(TokenType::Ident, i18n::tr("Nama diharapkan setelah 'import'", "Expected a name after 'import'")).text;
        std::string alias = field;
        if (matchWord("as", "sbg")) alias = expect(TokenType::Ident, i18n::tr("Nama alias diharapkan setelah 'as'", "Expected alias after 'as'")).text;
        ExprPtr get = std::make_unique<IndexExpr>(mkIdent(modVar, sp), LiteralExpr::makeString(field));
        get->span = sp;
        out.push_back(mkLet(alias, std::move(get), sp));
    } while (match(TokenType::Comma));
    expectEnd( i18n::tr("';' diharapkan setelah 'import'", "Expected ';' after import"));
    StmtPtr first = std::move(out.front());
    for (size_t i = 1; i < out.size(); i++) pendingStmts_.push_back(std::move(out[i]));
    return first;
}

StmtPtr Parser::letStmt() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan", "Expected variable name")).text;
    std::string typeAnnotation;
    if (match(TokenType::Colon)) {
        typeAnnotation = this->typeAnnotation();
    }
    expect(TokenType::Eq, i18n::tr("'=' diharapkan setelah nama variabel", "Expected '=' after variable name"));
    ExprPtr value = expression();
    expectEnd( i18n::tr("';' diharapkan setelah pernyataan 'buat'", "Expected ';' after let statement"));
    return std::make_unique<LetStmt>(std::move(name), std::move(value), std::move(typeAnnotation));
}

// A type annotation: `int`, `List[int]`, `Optional[str]`, `a.B`, `int | None`, `"Fwd"`. Only a plain
// name is kept (the type checker uses it); anything compound reads as unknown ("").
std::string Parser::typeAnnotation() {
    std::string head;
    bool complex = false;
    auto one = [&]() {
        if (check(TokenType::String) || check(TokenType::Null_)) {
            advance();
            complex = true;
            return;
        }
        Token t = expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'"));
        if (head.empty()) head = t.text;
        while (check(TokenType::Dot)) {
            advance();
            expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'"));
            complex = true;
        }
        if (check(TokenType::LBracket)) {
            complex = true;
            int depth = 0;
            do {
                if (check(TokenType::LBracket)) depth++;
                else if (check(TokenType::RBracket)) depth--;
                if (atEnd()) break;
                advance();
            } while (depth > 0);
        }
    };
    one();
    while (check(TokenType::Pipe)) {
        advance();
        one();
        complex = true;
    }
    return complex ? "" : head;
}

StmtPtr Parser::fnDecl() {
    nextFnAsync_ = pos_ > 0 && tokens_[pos_ - 1].text == "async";
    std::string name = expect(TokenType::Ident, i18n::tr("Nama fungsi diharapkan", "Expected function name")).text;
    return fnDeclBody(std::move(name));
}

// Python evaluates a default once, when the def runs: `def f(x, acc=[])` shares one list. Literals stay inline;
// anything else is computed into a hidden variable just before the statement.
ExprPtr Parser::hoistDefault(ExprPtr e) {
    if (e->kind == ExprKind::Literal || e->kind == ExprKind::Identifier) return e;
    if (e->kind == ExprKind::Unary && static_cast<UnaryExpr*>(e.get())->operand->kind == ExprKind::Literal) return e;
    if (e->kind == ExprKind::Index && static_cast<IndexExpr*>(e.get())->target->kind == ExprKind::Identifier &&
        static_cast<IndexExpr*>(e.get())->index->kind == ExprKind::Literal) return e;  // Name.attr (dataclass defaults)
    Span sp = e->span;
    std::string name = "__dflt" + std::to_string(hiddenCounter_++);
    preStmts_.push_back(mkLet(name, std::move(e), sp));
    return mkIdent(name, sp);
}

std::unique_ptr<FnDeclStmt> Parser::fnDeclBody(std::string name) {
    bool isAsync = nextFnAsync_;
    nextFnAsync_ = false;
    expect(TokenType::LParen, i18n::tr("'(' diharapkan setelah nama fungsi", "Expected '(' after function name"));
    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    std::vector<ExprPtr> defaults;  // parallel to params; null = no default
    // Python-style explicit receiver: `def m(self, x)` -- the instance is
    // already bound as `ini`, so `self` is not a real parameter.
    if (check(TokenType::This)) {
        advance();
        match(TokenType::Comma);
    }
    int restIndex = -1, kwIndex = -1;
    auto paramName = [&]() {
        // *args / **kwargs
        if (check(TokenType::Star) || check(TokenType::StarStar)) {
            bool kw = check(TokenType::StarStar);
            advance();
            std::string n = expect(TokenType::Ident, i18n::tr("Nama parameter diharapkan", "Expected parameter name")).text;
            (kw ? kwIndex : restIndex) = static_cast<int>(params.size());
            return n;
        }
        return expect(TokenType::Ident, i18n::tr("Nama parameter diharapkan", "Expected parameter name")).text;
    };
    if (!check(TokenType::RParen)) {
        params.push_back(paramName());
        paramTypes.push_back(match(TokenType::Colon)
                                  ? typeAnnotation()
                                  : "");
        defaults.push_back(match(TokenType::Eq) ? expression() : nullptr);
        while (match(TokenType::Comma)) {
            if (check(TokenType::RParen)) break;
            params.push_back(paramName());
            paramTypes.push_back(match(TokenType::Colon)
                                      ? typeAnnotation()
                                      : "");
            defaults.push_back(match(TokenType::Eq) ? expression() : nullptr);
        }
    }
    expect(TokenType::RParen, i18n::tr("')' diharapkan setelah parameter", "Expected ')' after parameters"));
    std::string returnType;
    if (check(TokenType::Minus) && peekAt(1).type == TokenType::Gt) {  // Python: def f(x) -> int:
        advance();
        advance();
        returnType = typeAnnotation();
    } else if (match(TokenType::Colon)) {
        returnType = typeAnnotation();
    }
    yieldStack_.push_back(false);
    auto body = block();
    bool isGenerator = yieldStack_.back();
    yieldStack_.pop_back();
    int minArgs = static_cast<int>(params.size());
    for (size_t i = 0; i < defaults.size(); i++) {
        if (defaults[i]) { minArgs = static_cast<int>(i); break; }
    }
    for (size_t i = static_cast<size_t>(minArgs); i < defaults.size(); i++) {
        if (!defaults[i] && static_cast<int>(i) != restIndex && static_cast<int>(i) != kwIndex) throw ParseError(i18n::tr("Parameter tanpa nilai default nggak boleh setelah yang punya default",
                                                    "Non-default parameter follows a default one"), peek());
    }
    // `p = default` becomes `if p == None: p = default` at the top of the body (a missing
    // argument arrives as None).
    for (size_t i = defaults.size(); i-- > static_cast<size_t>(minArgs);) {
        if (!defaults[i]) continue;  // the *args / **kwargs slots
        Span sp = defaults[i]->span;
        ExprPtr cond = mkBin("==", mkIdent(params[i], sp), LiteralExpr::makeNull(), sp);
        ExprPtr assign = std::make_unique<AssignExpr>(params[i], hoistDefault(std::move(defaults[i])));
        assign->span = sp;
        std::vector<StmtPtr> thenStmts;
        StmtPtr st = std::make_unique<ExprStmtNode>(std::move(assign));
        st->span = sp;
        thenStmts.push_back(std::move(st));
        StmtPtr ifs = std::make_unique<IfStmt>(std::move(cond), std::make_unique<BlockStmt>(std::move(thenStmts)), nullptr);
        ifs->span = sp;
        body->statements.insert(body->statements.begin(), std::move(ifs));
    }
    if (isAsync && !isGenerator) {
        // async def f(..): body -> def f(..): return _mkco(fn(): body)   (a coroutine object, run by await / asyncio)
        usesAsync_ = true;
        Span sp = body->span;
        auto inner = std::make_unique<FnDeclStmt>("", std::vector<std::string>{}, std::move(body));
        ExprPtr fe = std::make_unique<FnExprNode>(std::move(inner));
        fe->span = sp;
        std::vector<ExprPtr> ga;
        ga.push_back(std::move(fe));
        StmtPtr ret = std::make_unique<ReturnStmt>(mkCall("_mkco", std::move(ga), sp));
        ret->span = sp;
        std::vector<StmtPtr> outerBody;
        outerBody.push_back(std::move(ret));
        body = std::make_unique<BlockStmt>(std::move(outerBody));
        body->span = sp;
    }
    if (isGenerator) {
        // def f(..): ..yield.. -> def f(..): return _mkgen(fn(__y): ..)   (the body runs on demand)
        Span sp = body->span;
        auto inner = std::make_unique<FnDeclStmt>("", std::vector<std::string>{"__y"}, std::move(body));
        ExprPtr fe = std::make_unique<FnExprNode>(std::move(inner));
        fe->span = sp;
        std::vector<ExprPtr> ga;
        ga.push_back(std::move(fe));
        StmtPtr ret = std::make_unique<ReturnStmt>(mkCall("_mkgen", std::move(ga), sp));
        ret->span = sp;
        std::vector<StmtPtr> outerBody;
        outerBody.push_back(std::move(ret));
        body = std::make_unique<BlockStmt>(std::move(outerBody));
        body->span = sp;
    }
    auto decl = std::make_unique<FnDeclStmt>(std::move(name), std::move(params), std::move(body),
                                              std::move(paramTypes), std::move(returnType));
    decl->restIndex = restIndex;
    decl->kwIndex = kwIndex;
    // With *args/**kw the required count is the ordinary parameters that have no default.
    if (restIndex >= 0 || kwIndex >= 0) {
        int ordinary = restIndex >= 0 ? restIndex : kwIndex;
        int req = std::min(minArgs, ordinary);
        decl->minArgs = req;
    } else if (minArgs < static_cast<int>(decl->params.size())) {
        decl->minArgs = minArgs;
    }
    return decl;
}

StmtPtr Parser::classDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama kelas diharapkan", "Expected class name")).text;
    declaredClasses_.push_back(name);
    std::string parentName, parentBase;
    if (match(TokenType::Extends)) {
        parentName = expect(TokenType::Ident, i18n::tr("Nama kelas induk diharapkan", "Expected parent class name")).text;
        noteName(parentName);
    } else if (match(TokenType::LParen)) {  // Python: class A(B):
        if (!check(TokenType::RParen)) {
            parentName = expect(TokenType::Ident, i18n::tr("Nama kelas induk diharapkan", "Expected parent class name")).text;
            parentBase = parentName;
            if (check(TokenType::Dot)) {  // unittest.TestCase: bind the module attribute to a hidden name first
                Span psp = peek().span;
                ExprPtr chain = mkIdent(parentName, psp);
                while (match(TokenType::Dot)) {
                    std::string field = expect(TokenType::Ident, i18n::tr("Nama kelas induk diharapkan", "Expected parent class name")).text;
                    parentBase = field;
                    chain = std::make_unique<IndexExpr>(std::move(chain), LiteralExpr::makeString(field));
                    chain->span = psp;
                }
                parentName = "__parent" + std::to_string(hiddenCounter_++);
                classPre_ = mkLet(parentName, std::move(chain), psp);
            } else if (parentName == "object") {
                parentName.clear();
            } else {
                noteName(parentName);
            }
            // more bases / keywords (class A(B, C), metaclass=M): only the first base is used
            int depth = 0;
            while (!atEnd() && !(depth == 0 && check(TokenType::RParen))) {
                if (check(TokenType::LParen)) depth++;
                else if (check(TokenType::RParen)) depth--;
                advance();
            }
        }
        expect(TokenType::RParen, i18n::tr("')' diharapkan setelah kelas induk", "Expected ')' after parent class"));
    }
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan setelah nama kelas", "Expected '{' after class name"));
    std::vector<std::unique_ptr<FnDeclStmt>> methods;
    std::vector<std::pair<std::string, ExprPtr>> classAttrs;
    std::vector<std::pair<std::string, bool>> annotated;
    std::vector<std::pair<std::string, std::string>> aliases;  // (new name, existing method)
    while (!check(TokenType::RBrace) && !atEnd()) {
        if (isWord(peek(), "pass") && peekAt(1).type == TokenType::Semi) {  // empty class body
            advance();
            advance();
            continue;
        }
        // class attribute: `name = value` (or `name: type = value`)
        if (check(TokenType::Ident) && (peekAt(1).type == TokenType::Eq || peekAt(1).type == TokenType::Colon)) {
            std::string attr = advance().text;
            bool hasAnnotation = false;
            if (match(TokenType::Colon)) { typeAnnotation(); hasAnnotation = true; }
            if (match(TokenType::Eq)) {
                if (hasAnnotation) annotated.emplace_back(attr, true);
                ExprPtr value = expression();
                bool aliased = false;
                if (value->kind == ExprKind::Identifier) {  // `baca = tulis` inside the body: another name for a method
                    const std::string& target = static_cast<IdentifierExpr*>(value.get())->name;
                    for (auto& m : methods) {
                        if (m->name == target || (target == "__init__" && m->name == "konstruktor")) {
                            aliases.emplace_back(attr, m->name == "konstruktor" ? "__init__" : target);
                            aliased = true;
                        }
                    }
                }
                if (aliased) { expectEnd(i18n::tr("';' diharapkan setelah atribut kelas", "Expected end of line after class attribute")); continue; }
                classAttrs.emplace_back(attr, std::move(value));
            } else {
                if (hasAnnotation) annotated.emplace_back(attr, false);
                classAttrs.emplace_back(attr, LiteralExpr::makeNull());
            }
            expectEnd(i18n::tr("';' diharapkan setelah atribut kelas", "Expected end of line after class attribute"));
            continue;
        }
        int memberKind = 0;
        std::string setterFor;
        while (match(TokenType::At)) {
            ExprPtr dec = expression();
            expectEnd(i18n::tr("';' diharapkan setelah dekorator", "Expected end of line after decorator"));
            if (dec->kind == ExprKind::Identifier) {
                const std::string& dn = static_cast<IdentifierExpr*>(dec.get())->name;
                if (dn == "staticmethod") memberKind = 1;
                else if (dn == "classmethod") memberKind = 2;
                else if (dn == "property") memberKind = 3;
                else throw ParseError(std::string(i18n::tr("Dekorator metode kelas belum didukung (yang ada: staticmethod, classmethod, property, x.setter): ",
                                                           "This class method decorator is not supported (available: staticmethod, classmethod, property, x.setter): ")) + dn, peek());
            } else if (dec->kind == ExprKind::Index &&
                       static_cast<IndexExpr*>(dec.get())->index->kind == ExprKind::Literal &&
                       static_cast<LiteralExpr*>(static_cast<IndexExpr*>(dec.get())->index.get())->str == "setter" &&
                       static_cast<IndexExpr*>(dec.get())->target->kind == ExprKind::Identifier) {
                memberKind = 4;
                setterFor = static_cast<IdentifierExpr*>(static_cast<IndexExpr*>(dec.get())->target.get())->name;
            } else {
                throw ParseError(i18n::tr("Dekorator metode kelas nggak dikenal", "Unknown class method decorator"), peek());
            }
        }
        expect(TokenType::Fn, i18n::tr("Cuma deklarasi fungsi yang boleh di dalam kelas", "Only function declarations are allowed inside a class"));
        StmtPtr m = fnDecl();
        auto* fnNode = static_cast<FnDeclStmt*>(m.get());
        if (fnNode->name == "__init__") fnNode->name = "konstruktor";
        fnNode->kind = memberKind;
        if (memberKind == 4) fnNode->name = "__set_" + setterFor;
        methods.push_back(std::unique_ptr<FnDeclStmt>(static_cast<FnDeclStmt*>(m.release())));
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan setelah isi kelas", "Expected '}' after class body"));
    // Class attributes are assigned right after the class exists: Name.attr = value
    for (auto& [attr, value] : classAttrs) {
        Span sp = value->span;
        ExprPtr set = std::make_unique<IndexAssignExpr>(mkIdent(name, sp), LiteralExpr::makeString(attr), std::move(value));
        set->span = sp;
        StmtPtr st = std::make_unique<ExprStmtNode>(std::move(set));
        st->span = sp;
        pendingStmts_.push_back(std::move(st));
    }
    for (auto& [alias, target] : aliases) {  // forwarding method: def alias(self, *a, **kw): return self.target(*a, **kw)
        std::string src = "class __AL:\n    def " + alias + "(self, *a, **kw):\n        return self." + target + "(*a, **kw)\n";
        Lexer lx(src);
        Parser sub(lx.tokenize());
        auto prog = sub.parse();
        for (auto& st : prog->statements) {
            if (st->kind != StmtKind::ClassDecl) continue;
            for (auto& m : static_cast<ClassDeclStmt*>(st.get())->methods) methods.push_back(std::move(m));
        }
    }
    if (parentBase == "Enum" || parentBase == "IntEnum" || parentBase == "Flag" || parentBase == "IntFlag" || parentBase == "StrEnum") {
        std::vector<ExprPtr> ea;  // members become objects with .name and .value once the attributes exist
        ea.push_back(mkIdent(name, peek().span));
        StmtPtr st = std::make_unique<ExprStmtNode>(mkCall("_enum_init", std::move(ea), peek().span));
        pendingStmts_.push_back(std::move(st));
    }
    lastAnnotated_ = annotated;
    dcFields_[name] = std::move(annotated);
    lastParent_ = parentName;
    return std::make_unique<ClassDeclStmt>(std::move(name), std::move(parentName), std::move(methods));
}

// @dataclass: __init__, __repr__, __eq__ (and ordering) written from the annotated fields.
void Parser::makeDataclass(ClassDeclStmt& cls, bool order) {
    std::vector<std::pair<std::string, bool>> fields;
    auto inherited = dcFields_.find(cls.parentName);
    if (inherited != dcFields_.end()) fields = inherited->second;
    for (auto& f : lastAnnotated_) {
        bool replaced = false;
        for (auto& g : fields) if (g.first == f.first) { g = f; replaced = true; }
        if (!replaced) fields.push_back(f);
    }
    dcFields_[cls.name] = fields;
    const std::string& C = cls.name;
    std::string params = "self", body, repr = "\"" + C + "(\"", eq = "isinstance(o, " + C + ")", tup;
    for (size_t i = 0; i < fields.size(); i++) {
        const std::string& n = fields[i].first;
        params += ", " + n + (fields[i].second ? "=" + C + "." + n : "");
        body += "        self." + n + " = _dcv(" + n + ")\n";
        repr += std::string(i ? " + \", " : " + \"") + n + "=\" + repr(self." + n + ")";
        eq += " and self." + n + " == o." + n;
        tup += (i ? ", " : "") + std::string("self.") + n;
    }
    if (fields.empty()) body = "        pass\n";
    repr += " + \")\"";
    std::string src = "class __DC:\n    def __init__(" + params + "):\n" + body +
                      "    def __repr__(self):\n        return " + repr + "\n" +
                      "    def __eq__(self, o):\n        return " + eq + "\n";
    if (order) {
        src += "    def _dc_key(self):\n        return [" + tup + "]\n"
               "    def __lt__(self, o):\n        return self._dc_key() < o._dc_key()\n"
               "    def __le__(self, o):\n        return self._dc_key() <= o._dc_key()\n"
               "    def __gt__(self, o):\n        return self._dc_key() > o._dc_key()\n"
               "    def __ge__(self, o):\n        return self._dc_key() >= o._dc_key()\n";
    }
    Lexer lx(src);
    Parser sub(lx.tokenize());
    auto prog = sub.parse();
    for (auto& st : prog->statements) {
        if (st->kind != StmtKind::ClassDecl) continue;
        for (auto& m : static_cast<ClassDeclStmt*>(st.get())->methods) {
            if (m->name == "__init__") m->name = "konstruktor";
            bool own = false;
            for (auto& e : cls.methods) if (e->name == m->name) own = true;  // hand-written methods win
            if (!own) cls.methods.push_back(std::move(m));
        }
    }
}

StmtPtr Parser::structDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama bentuk diharapkan", "Expected struct name")).text;
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan setelah nama bentuk", "Expected '{' after struct name"));
    std::vector<std::string> fields;
    auto readField = [&]() {
        fields.push_back(expect(TokenType::Ident, i18n::tr("Nama field diharapkan", "Expected field name")).text);
        if (match(TokenType::Colon)) {
            typeAnnotation();
        }
    };
    if (!check(TokenType::RBrace)) {
        readField();
        while (match(TokenType::Comma)) {
            if (check(TokenType::RBrace)) break;
            readField();
        }
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan setelah field bentuk", "Expected '}' after struct fields"));
    return std::make_unique<StructDeclStmt>(std::move(name), std::move(fields));
}

StmtPtr Parser::enumDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama jenis diharapkan", "Expected enum name")).text;
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan setelah nama jenis", "Expected '{' after enum name"));
    std::vector<std::string> variants;
    if (!check(TokenType::RBrace)) {
        variants.push_back(expect(TokenType::Ident, i18n::tr("Nama varian diharapkan", "Expected variant name")).text);
        while (match(TokenType::Comma)) {
            if (check(TokenType::RBrace)) break;
            variants.push_back(expect(TokenType::Ident, i18n::tr("Nama varian diharapkan", "Expected variant name")).text);
        }
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan setelah varian jenis", "Expected '}' after enum variants"));
    return std::make_unique<EnumDeclStmt>(std::move(name), std::move(variants));
}

StmtPtr Parser::tryStmt() {
    Span sp = peek().span;
    auto tryBlock = block();
    if (check(TokenType::Finally)) {
        // try/finally with no except: run the cleanup, then let the error keep going
        advance();
        auto fin = block();
        std::vector<StmtPtr> rethrow;
        rethrow.push_back(std::make_unique<ThrowStmt>(mkIdent("__fe", peek().span)));
        return std::make_unique<TryStmt>(std::move(tryBlock), "__fe", std::make_unique<BlockStmt>(std::move(rethrow)), std::move(fin));
    }
    if (!check(TokenType::Catch)) {
        throw ParseError(i18n::tr("'tangkap' diharapkan setelah blok 'coba'", "Expected 'tangkap' after 'coba' block"), peek());
    }
    // Python: any number of `except [Class | (A, B)] [as e]:` clauses, then optional else / finally.
    struct Clause {
        ExprPtr filter;      // class or tuple of classes; null = catch everything
        std::string var;     // bound name ("" = none)
        bool typed = false;  // `except X as e`: e becomes an exception object
        std::unique_ptr<BlockStmt> body;
    };
    std::vector<Clause> clauses;
    int id = hiddenCounter_++;
    std::string exVar = "__ex" + std::to_string(id);
    auto isClassLike = [&](const Token& t) {
        return t.type == TokenType::Ident && !t.text.empty() && std::isupper(static_cast<unsigned char>(t.text[0]));
    };
    while (check(TokenType::Catch)) {
        advance();
        Clause c;
        if (match(TokenType::LParen)) {
            if (check(TokenType::Ident) && !isClassLike(peek()) && peekAt(1).type == TokenType::RParen) {
                c.var = advance().text;  // legacy `tangkap (e)`
                advance();
            } else {  // except (A, B):
                std::vector<ExprPtr> items;
                do { items.push_back(logicOr()); } while (match(TokenType::Comma));
                expect(TokenType::RParen, i18n::tr("')' diharapkan", "Expected ')'"));
                c.filter = std::make_unique<ArrayLitExpr>(std::move(items));
                c.typed = true;
            }
        } else if (check(TokenType::Ident)) {
            if (isClassLike(peek()) || peekAt(1).type == TokenType::Dot) {
                c.filter = call();
                c.typed = true;
            } else {  // legacy `except e:`
                c.var = advance().text;
            }
        }
        if (matchWord("as", "sbg")) {
            c.var = expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan setelah 'as'", "Expected variable name after 'as'")).text;
        }
        catchVarStack_.push_back(exVar);
        c.body = block();
        catchVarStack_.pop_back();
        clauses.push_back(std::move(c));
    }
    std::unique_ptr<BlockStmt> elseBlock;
    if (match(TokenType::Else)) elseBlock = block();
    std::unique_ptr<BlockStmt> finallyBlock;
    if (match(TokenType::Finally)) finallyBlock = block();

    std::string okVar = "__ok" + std::to_string(id);
    if (elseBlock) {  // body finished without raising -> flag it; the else block runs after the try
        ExprPtr set = std::make_unique<AssignExpr>(okVar, LiteralExpr::makeBool(true));
        set->span = sp;
        tryBlock->statements.push_back(std::make_unique<ExprStmtNode>(std::move(set)));
    }

    std::unique_ptr<BlockStmt> catchBlock;
    std::string catchVar;
    if (clauses.size() == 1 && !clauses[0].filter) {
        // the classic single catch-all: keep the old shape
        catchVar = clauses[0].var.empty() ? "_e" : clauses[0].var;
        catchBlock = std::move(clauses[0].body);
    } else {
        catchVar = exVar;
        std::unique_ptr<BlockStmt> chain;  // else-chain built from the last clause backwards
        {
            std::vector<StmtPtr> rethrow;
            rethrow.push_back(std::make_unique<ThrowStmt>(mkIdent(exVar, sp)));
            chain = std::make_unique<BlockStmt>(std::move(rethrow));
        }
        for (size_t k = clauses.size(); k-- > 0;) {
            Clause& c = clauses[k];
            std::vector<StmtPtr> then;
            if (!c.var.empty()) {
                ExprPtr bound = mkIdent(exVar, sp);
                if (c.typed) {
                    std::vector<ExprPtr> wa;
                    wa.push_back(std::move(bound));
                    bound = mkCall("_exc_wrap", std::move(wa), sp);
                }
                then.push_back(mkLet(c.var, std::move(bound), sp));
            }
            for (auto& st : c.body->statements) then.push_back(std::move(st));
            auto thenBlock = std::make_unique<BlockStmt>(std::move(then));
            thenBlock->span = sp;
            if (!c.filter) {  // catches everything: nothing after it can run
                chain = std::move(thenBlock);
                continue;
            }
            std::vector<ExprPtr> ma;
            ma.push_back(mkIdent(exVar, sp));
            ma.push_back(std::move(c.filter));
            StmtPtr ifs = std::make_unique<IfStmt>(mkCall("_exc_match", std::move(ma), sp), std::move(thenBlock), std::move(chain));
            ifs->span = sp;
            std::vector<StmtPtr> wrap;
            wrap.push_back(std::move(ifs));
            chain = std::make_unique<BlockStmt>(std::move(wrap));
            chain->span = sp;
        }
        catchBlock = std::move(chain);
    }
    if (!elseBlock) {
        StmtPtr tr = std::make_unique<TryStmt>(std::move(tryBlock), std::move(catchVar), std::move(catchBlock), std::move(finallyBlock));
        tr->span = sp;
        return tr;
    }
    // else: runs after a clean body, before finally, and its own errors are not caught by the excepts:
    //   { ok = false; try { try { body; ok = true } catch ..; if ok { else } } finally { fin } }
    StmtPtr inner = std::make_unique<TryStmt>(std::move(tryBlock), std::move(catchVar), std::move(catchBlock), nullptr);
    inner->span = sp;
    std::vector<StmtPtr> seq;
    seq.push_back(std::move(inner));
    seq.push_back(std::make_unique<IfStmt>(mkIdent(okVar, sp), std::move(elseBlock), nullptr));
    StmtPtr body = std::make_unique<BlockStmt>(std::move(seq));
    body->span = sp;
    StmtPtr result;
    if (finallyBlock) {
        std::vector<StmtPtr> ob;
        ob.push_back(std::move(body));
        std::vector<StmtPtr> rethrow;
        rethrow.push_back(std::make_unique<ThrowStmt>(mkIdent("__fe", sp)));
        result = std::make_unique<TryStmt>(std::make_unique<BlockStmt>(std::move(ob)), "__fe",
                                           std::make_unique<BlockStmt>(std::move(rethrow)), std::move(finallyBlock));
        result->span = sp;
    } else {
        result = std::move(body);
    }
    std::vector<StmtPtr> outer;
    outer.push_back(mkLet(okVar, LiteralExpr::makeBool(false), sp));
    outer.push_back(std::move(result));
    StmtPtr blk = std::make_unique<BlockStmt>(std::move(outer));
    blk->span = sp;
    return blk;
}

// `@dec` lines before a def or class: `f = dec(f)` right after the definition.
StmtPtr Parser::decoratedStmt() {
    Span sp = peek().span;
    std::vector<ExprPtr> decorators;
    while (match(TokenType::At)) {
        decorators.push_back(expression());
        expectEnd(i18n::tr("';' diharapkan setelah dekorator", "Expected end of line after decorator"));
    }
    StmtPtr def;
    std::string name;
    if (match(TokenType::Fn)) {
        def = fnDecl();
        name = static_cast<FnDeclStmt*>(def.get())->name;
    } else if (match(TokenType::Class)) {
        def = classDecl();
        name = static_cast<ClassDeclStmt*>(def.get())->name;
        // @dataclass / @dataclass(order=True, ...) is done here, at parse time
        for (size_t i = decorators.size(); i-- > 0;) {
            Expr* d = decorators[i].get();
            bool order = false;
            if (d->kind == ExprKind::Call) {
                auto* c = static_cast<CallExpr*>(d);
                bool plain = c->callee->kind == ExprKind::Identifier && static_cast<IdentifierExpr*>(c->callee.get())->name == "dataclass";
                bool viaKw = c->callee->kind == ExprKind::Identifier && static_cast<IdentifierExpr*>(c->callee.get())->name == "_callkw" &&
                             !c->args.empty() && c->args[0]->kind == ExprKind::Identifier &&
                             static_cast<IdentifierExpr*>(c->args[0].get())->name == "dataclass";
                if (!plain && !viaKw) continue;
                order = true;  // ordering methods are harmless when not asked for
            } else if (d->kind != ExprKind::Identifier || static_cast<IdentifierExpr*>(d)->name != "dataclass") {
                continue;
            }
            makeDataclass(*static_cast<ClassDeclStmt*>(def.get()), order);
            decorators.erase(decorators.begin() + static_cast<std::ptrdiff_t>(i));
        }
    } else {
        throw ParseError(i18n::tr("Dekorator harus diikuti def atau class", "A decorator must be followed by def or class"), peek());
    }
    ExprPtr value = mkIdent(name, sp);
    for (size_t i = decorators.size(); i-- > 0;) {
        std::vector<ExprPtr> a;
        a.push_back(std::move(value));
        value = std::make_unique<CallExpr>(std::move(decorators[i]), std::move(a));
        value->span = sp;
    }
    ExprPtr assign = std::make_unique<AssignExpr>(name, std::move(value));
    assign->span = sp;
    StmtPtr st = std::make_unique<ExprStmtNode>(std::move(assign));
    st->span = sp;
    pendingStmts_.push_back(std::move(st));
    return def;
}

// `with mgr as f: body` follows Python's protocol:
//   { let m = mgr; let f = _with_enter(m); let h = false;
//     try { body } catch (e) { h = true; if !_with_exit(m, e) { throw e } } finally { if !h { _with_exit(m, None) } } }
StmtPtr Parser::withStmt() {
    Span sp = peek().span;
    ExprPtr resource = expression();
    int id = hiddenCounter_++;
    std::string mgr = "__wm" + std::to_string(id), handled = "__wh" + std::to_string(id), err = "__we" + std::to_string(id);
    std::string var;
    if (matchWord("as", "sbg")) var = expect(TokenType::Ident, i18n::tr("Nama diharapkan setelah 'as'", "Expected a name after 'as'")).text;
    auto body = block();
    std::vector<StmtPtr> outer;
    outer.push_back(mkLet(mgr, std::move(resource), sp));
    {
        std::vector<ExprPtr> a;
        a.push_back(mkIdent(mgr, sp));
        outer.push_back(mkLet(var.empty() ? "__wv" + std::to_string(id) : var, mkCall("_with_enter", std::move(a), sp), sp));
    }
    outer.push_back(mkLet(handled, LiteralExpr::makeBool(false), sp));
    std::vector<StmtPtr> onError;
    {
        ExprPtr set = std::make_unique<AssignExpr>(handled, LiteralExpr::makeBool(true));
        set->span = sp;
        onError.push_back(std::make_unique<ExprStmtNode>(std::move(set)));
        std::vector<ExprPtr> a;
        a.push_back(mkIdent(mgr, sp));
        a.push_back(mkIdent(err, sp));
        ExprPtr suppressed = mkCall("_with_exit", std::move(a), sp);
        std::vector<StmtPtr> rethrow;
        rethrow.push_back(std::make_unique<ThrowStmt>(mkIdent(err, sp)));
        onError.push_back(std::make_unique<IfStmt>(std::make_unique<UnaryExpr>("!", std::move(suppressed)),
                                                   std::make_unique<BlockStmt>(std::move(rethrow)), nullptr));
    }
    std::vector<StmtPtr> fin;
    {
        std::vector<ExprPtr> a;
        a.push_back(mkIdent(mgr, sp));
        a.push_back(LiteralExpr::makeNull());
        std::vector<StmtPtr> then;
        then.push_back(std::make_unique<ExprStmtNode>(mkCall("_with_exit", std::move(a), sp)));
        fin.push_back(std::make_unique<IfStmt>(std::make_unique<UnaryExpr>("!", mkIdent(handled, sp)),
                                               std::make_unique<BlockStmt>(std::move(then)), nullptr));
    }
    outer.push_back(std::make_unique<TryStmt>(std::move(body), err, std::make_unique<BlockStmt>(std::move(onError)),
                                              std::make_unique<BlockStmt>(std::move(fin))));
    // Spliced flat into the enclosing block: like Python, `as name` stays visible after the with.
    StmtPtr first = std::move(outer.front());
    for (size_t i = 1; i < outer.size(); i++) {
        outer[i]->span = sp;
        pendingStmts_.push_back(std::move(outer[i]));
    }
    return first;
}

StmtPtr Parser::throwStmt() {
    ExprPtr value;
    if (check(TokenType::Semi) || check(TokenType::RBrace)) {  // bare `raise`: re-throw the exception being handled
        if (catchVarStack_.empty()) {
            throw ParseError(i18n::tr("'raise' tanpa nilai cuma boleh di dalam blok 'except'", "A bare 'raise' is only allowed inside an 'except' block"), peek());
        }
        value = mkIdent(catchVarStack_.back(), peek().span);
    } else {
        value = expression();
        if (isWord(peek(), "from", "dari")) {  // raise A from B: the cause is dropped
            advance();
            expression();
        }
    }
    expectEnd( i18n::tr("';' diharapkan setelah 'lempar'", "Expected ';' after 'lempar'"));
    return std::make_unique<ThrowStmt>(std::move(value));
}

StmtPtr Parser::ifStmt() {
    ExprPtr condition = expression();
    auto thenBranch = block();
    std::unique_ptr<BlockStmt> elseBranch;
    if (match(TokenType::Else)) {
        if (check(TokenType::LBrace)) {
            elseBranch = block();
        } else {
            // supports `else if ...` chains
            std::vector<StmtPtr> wrapped;
            wrapped.push_back(statement());
            elseBranch = std::make_unique<BlockStmt>(std::move(wrapped));
        }
    }
    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

StmtPtr Parser::whileStmt() {
    ExprPtr condition = expression();
    auto body = block();
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

StmtPtr Parser::forClauseInit() {
    if (match(TokenType::Let)) {
        std::string name = expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan", "Expected variable name")).text;
        expect(TokenType::Eq, i18n::tr("'=' diharapkan setelah nama variabel", "Expected '=' after variable name"));
        ExprPtr value = expression();
        return std::make_unique<LetStmt>(std::move(name), std::move(value));
    }
    ExprPtr e = expression();
    return std::make_unique<ExprStmtNode>(std::move(e));
}

// Value of a numeric literal, allowing a leading unary minus.
static bool literalNumber(const Expr* e, double* out) {
    if (e->kind == ExprKind::Literal) {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->litKind != LiteralExpr::Kind::Number) return false;
        *out = lit->number;
        return true;
    }
    if (e->kind == ExprKind::Unary) {
        auto* u = static_cast<const UnaryExpr*>(e);
        double inner;
        if (u->op == "-" && literalNumber(u->operand.get(), &inner)) {
            *out = -inner;
            return true;
        }
    }
    return false;
}

// `for a, b in ...` / `for (a, b) in ...`: one or more loop variables.
std::vector<std::string> Parser::forTargets() {
    std::vector<std::string> vars;
    bool paren = match(TokenType::LParen);
    do {
        vars.push_back(expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan setelah 'for'", "Expected variable name after 'for'")).text);
    } while (match(TokenType::Comma));
    if (paren) expect(TokenType::RParen, i18n::tr("')' diharapkan", "Expected ')'"));
    return vars;
}

namespace {
// break -> { flag = true; break } for the breaks that belong to this loop (not nested loops / functions).
void flagBreaks(BlockStmt* b, const std::string& flag) {
    if (!b) return;
    for (auto& st : b->statements) {
        switch (st->kind) {
            case StmtKind::Break: {
                std::vector<StmtPtr> two;
                ExprPtr set = std::make_unique<AssignExpr>(flag, LiteralExpr::makeBool(true));
                two.push_back(std::make_unique<ExprStmtNode>(std::move(set)));
                two.push_back(std::make_unique<BreakStmt>());
                st = std::make_unique<BlockStmt>(std::move(two));
                break;
            }
            case StmtKind::If: {
                auto* n = static_cast<IfStmt*>(st.get());
                flagBreaks(n->thenBranch.get(), flag);
                flagBreaks(n->elseBranch.get(), flag);
                break;
            }
            case StmtKind::Block: flagBreaks(static_cast<BlockStmt*>(st.get()), flag); break;
            case StmtKind::Try: {
                auto* n = static_cast<TryStmt*>(st.get());
                flagBreaks(n->tryBlock.get(), flag);
                flagBreaks(n->catchBlock.get(), flag);
                flagBreaks(n->finallyBlock.get(), flag);
                break;
            }
            default: break;
        }
    }
}

void flagLoopBreaks(Stmt* st, const std::string& flag) {
    if (st->kind == StmtKind::For) flagBreaks(static_cast<ForStmt*>(st)->body.get(), flag);
    else if (st->kind == StmtKind::While) flagBreaks(static_cast<WhileStmt*>(st)->body.get(), flag);
    else if (st->kind == StmtKind::Block) {
        for (auto& c : static_cast<BlockStmt*>(st)->statements) flagLoopBreaks(c.get(), flag);
    }
}
}  // namespace

// `for/while ...: body else: alt` -- alt runs only when the loop ended without `break`.
StmtPtr Parser::loopElse(StmtPtr loop) {
    Span sp = loop->span;
    advance();  // else
    auto alt = block();
    std::string flag = "__brk" + std::to_string(hiddenCounter_++);
    flagLoopBreaks(loop.get(), flag);
    std::vector<StmtPtr> outer;
    outer.push_back(mkLet(flag, LiteralExpr::makeBool(false), sp));
    outer.push_back(std::move(loop));
    outer.push_back(std::make_unique<IfStmt>(std::make_unique<UnaryExpr>("!", mkIdent(flag, sp)), std::move(alt), nullptr));
    StmtPtr blk = std::make_unique<BlockStmt>(std::move(outer));
    blk->span = sp;
    return blk;
}

StmtPtr Parser::forInStmt(Span sp) {
    bool declared = loopVarDeclared_;
    loopVarDeclared_ = false;  // the body (and any comprehension in it) must not see this flag
    std::vector<std::string> vars = forTargets();
    if (!matchWord("in", "dalam")) {
        throw ParseError(i18n::tr("'in' diharapkan setelah variabel 'for'", "Expected 'in' after the 'for' variable"), peek());
    }
    ExprPtr iter = expression();
    auto body = block();
    loopVarDeclared_ = declared;
    return buildForIn(vars, std::move(iter), std::move(body), sp);
}

StmtPtr Parser::buildForIn(const std::vector<std::string>& vars, ExprPtr iter, std::unique_ptr<BlockStmt> body, Span sp) {
    const std::string& var = vars[0];
    int id = hiddenCounter_++;
    bool declaredHere = loopVarDeclared_;
    loopVarDeclared_ = false;  // only the outermost loop of this statement (not nested comprehension loops)

    // `for i in range(a, b[, literal step])` -> a plain counting loop, so it
    // stays on the fast (VM/JIT) path and allocates nothing.
    if (vars.size() == 1 && iter->kind == ExprKind::Call) {
        auto* c = static_cast<CallExpr*>(iter.get());
        if (c->callee->kind == ExprKind::Identifier) {
            const std::string& fname = static_cast<IdentifierExpr*>(c->callee.get())->name;
            size_t n = c->args.size();
            double step = 1;
            bool ok = (fname == "range" || fname == "rentang") && n >= 1 && n <= 3;
            bool dynamicStep = false;  // `range(a, b, k)` with a non-literal k
            if (ok && n == 3 && !(literalNumber(c->args[2].get(), &step) && step != 0)) {
                dynamicStep = true;
            }
            if (ok) {
                ExprPtr start = n >= 2 ? std::move(c->args[0]) : mkNum(0, sp);
                ExprPtr stop = n >= 2 ? std::move(c->args[1]) : std::move(c->args[0]);
                std::vector<StmtPtr> outer;
                ExprPtr stopRef;
                double lit;
                if (literalNumber(stop.get(), &lit)) {
                    stopRef = mkNum(lit, sp);
                } else {
                    // Python evaluates the bound once, not on every iteration.
                    std::string hidden = "__stop" + std::to_string(id);
                    outer.push_back(mkLet(hidden, std::move(stop), sp));
                    stopRef = mkIdent(hidden, sp);
                }
                ExprPtr cond, stepExpr;
                if (dynamicStep) {
                    // Direction is only known at run time: (k > 0 and v < stop) or (k < 0 and v > stop).
                    std::string stepVar = "__step" + std::to_string(id);
                    outer.push_back(mkLet(stepVar, std::move(c->args[2]), sp));
                    std::string stopVar = "__lim" + std::to_string(id);
                    outer.push_back(mkLet(stopVar, std::move(stopRef), sp));
                    ExprPtr up = mkBin("&&", mkBin(">", mkIdent(stepVar, sp), mkNum(0, sp), sp),
                                       mkBin("<", mkIdent(var, sp), mkIdent(stopVar, sp), sp), sp);
                    ExprPtr down = mkBin("&&", mkBin("<", mkIdent(stepVar, sp), mkNum(0, sp), sp),
                                         mkBin(">", mkIdent(var, sp), mkIdent(stopVar, sp), sp), sp);
                    cond = mkBin("||", std::move(up), std::move(down), sp);
                    stepExpr = mkIdent(stepVar, sp);
                } else {
                    cond = mkBin(step > 0 ? "<" : ">", mkIdent(var, sp), std::move(stopRef), sp);
                    stepExpr = mkNum(step, sp);
                }
                ExprPtr postAssign = std::make_unique<AssignExpr>(
                    var, mkBin("+", mkIdent(var, sp), std::move(stepExpr), sp));
                postAssign->span = sp;
                StmtPtr initStmt;
                if (declaredHere) {  // target already exists in the enclosing scope: assign, don't shadow
                    ExprPtr as = std::make_unique<AssignExpr>(var, std::move(start));
                    as->span = sp;
                    initStmt = std::make_unique<ExprStmtNode>(std::move(as));
                    initStmt->span = sp;
                } else {
                    initStmt = mkLet(var, std::move(start), sp);
                }
                StmtPtr loop = std::make_unique<ForStmt>(std::move(initStmt), std::move(cond),
                                                          std::move(postAssign), std::move(body));
                loop->span = sp;
                if (outer.empty()) return loop;
                outer.push_back(std::move(loop));
                StmtPtr blk = std::make_unique<BlockStmt>(std::move(outer));
                blk->span = sp;
                return blk;
            }
        }
    }

    // General case: iterate a snapshot array (`__iter` turns strings, maps
    // and arrays into something indexable).
    std::string cVar = "__it" + std::to_string(id), iVar = "__ix" + std::to_string(id);
    std::vector<StmtPtr> outer;
    std::vector<ExprPtr> iterArgs;
    iterArgs.push_back(std::move(iter));
    outer.push_back(mkLet(cVar, mkCall("__iter", std::move(iterArgs), sp), sp));
    std::vector<ExprPtr> lenArgs;
    lenArgs.push_back(mkIdent(cVar, sp));
    ExprPtr cond = mkBin("<", mkIdent(iVar, sp), mkCall("panjang", std::move(lenArgs), sp), sp);
    ExprPtr postAssign = std::make_unique<AssignExpr>(iVar, mkBin("+", mkIdent(iVar, sp), mkNum(1, sp), sp));
    postAssign->span = sp;
    ExprPtr elem = std::make_unique<IndexExpr>(mkIdent(cVar, sp), mkIdent(iVar, sp));
    elem->span = sp;
    auto bind = [&](const std::string& name, ExprPtr value) -> StmtPtr {
        if (!declaredHere) return mkLet(name, std::move(value), sp);
        ExprPtr as = std::make_unique<AssignExpr>(name, std::move(value));
        as->span = sp;
        StmtPtr st = std::make_unique<ExprStmtNode>(std::move(as));
        st->span = sp;
        return st;
    };
    if (vars.size() == 1) {
        body->statements.insert(body->statements.begin(), bind(var, std::move(elem)));
    } else {
        // Unpack each element: let __eN = it[i]; let a = __eN[0]; let b = __eN[1] ...
        std::string eVar = "__e" + std::to_string(id);
        std::vector<StmtPtr> pre;
        pre.push_back(mkLet(eVar, std::move(elem), sp));
        for (size_t k = 0; k < vars.size(); k++) {
            ExprPtr part = std::make_unique<IndexExpr>(mkIdent(eVar, sp), mkNum(static_cast<double>(k), sp));
            part->span = sp;
            pre.push_back(bind(vars[k], std::move(part)));
        }
        body->statements.insert(body->statements.begin(), std::make_move_iterator(pre.begin()), std::make_move_iterator(pre.end()));
    }
    StmtPtr loop = std::make_unique<ForStmt>(mkLet(iVar, mkNum(0, sp), sp), std::move(cond),
                                              std::move(postAssign), std::move(body));
    loop->span = sp;
    outer.push_back(std::move(loop));
    StmtPtr blk = std::make_unique<BlockStmt>(std::move(outer));
    blk->span = sp;
    return blk;
}

StmtPtr Parser::forStmt() {
    if (!check(TokenType::LParen)) return forInStmt(peek().span);
    expect(TokenType::LParen, i18n::tr("'(' diharapkan setelah 'untuk'", "Expected '(' after 'untuk'"));

    StmtPtr init;
    if (!check(TokenType::Semi)) init = forClauseInit();
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah inisialisasi perulangan 'untuk'", "Expected ';' after for-loop init"));

    ExprPtr condition;
    if (!check(TokenType::Semi)) condition = expression();
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah kondisi perulangan 'untuk'", "Expected ';' after for-loop condition"));

    ExprPtr post;
    if (!check(TokenType::RParen)) post = expression();
    expect(TokenType::RParen, i18n::tr("')' diharapkan setelah klausa perulangan 'untuk'", "Expected ')' after for-loop clauses"));

    auto body = block();
    return std::make_unique<ForStmt>(std::move(init), std::move(condition), std::move(post), std::move(body));
}

StmtPtr Parser::returnStmt() {
    ExprPtr value;
    if (!check(TokenType::Semi)) {
        value = expression();
    }
    expectEnd( i18n::tr("';' diharapkan setelah pernyataan 'hasil'", "Expected ';' after return statement"));
    return std::make_unique<ReturnStmt>(std::move(value));
}

StmtPtr Parser::breakStmt() {
    expectEnd( i18n::tr("';' diharapkan setelah 'berhenti'", "Expected ';' after 'berhenti'"));
    return std::make_unique<BreakStmt>();
}

StmtPtr Parser::continueStmt() {
    expectEnd( i18n::tr("';' diharapkan setelah 'lanjut'", "Expected ';' after 'lanjut'"));
    return std::make_unique<ContinueStmt>();
}

std::unique_ptr<BlockStmt> Parser::block() {
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan", "Expected '{'"));
    std::vector<StmtPtr> statements;
    while (!check(TokenType::RBrace) && !atEnd()) {
        statements.push_back(statement());
        for (auto& extra : pendingStmts_) statements.push_back(std::move(extra));
        pendingStmts_.clear();
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan", "Expected '}'"));
    return std::make_unique<BlockStmt>(std::move(statements));
}

// `a, b = x, y` / `a, b = pair` / `x[i], x[j] = x[j], x[i]`: the right side is evaluated in full
// first (so swaps work), then each target is assigned in order.
StmtPtr Parser::tupleAssign(ExprPtr first, Span sp) {
    std::vector<ExprPtr> targets;
    targets.push_back(std::move(first));
    int starAt = leadingStar_ ? 0 : -1;
    leadingStar_ = false;
    while (match(TokenType::Comma)) {
        if (check(TokenType::Eq)) break;
        if (check(TokenType::Star)) {  // a, *rest = xs
            advance();
            starAt = static_cast<int>(targets.size());
        }
        targets.push_back(logicOr());
    }
    expect(TokenType::Eq, i18n::tr("'=' diharapkan pada penugasan beruntun", "Expected '=' in tuple assignment"));
    std::vector<ExprPtr> rhs;
    rhs.push_back(assignment());
    bool literalTuple = false;
    while (match(TokenType::Comma)) {
        if (check(TokenType::Semi) || peek().span.line != tokens_[pos_ - 1].span.line) break;
        rhs.push_back(assignment());
        literalTuple = true;
    }
    expectEnd(i18n::tr("';' diharapkan setelah penugasan", "Expected ';' after assignment"));
    std::string tmp = "__t" + std::to_string(hiddenCounter_++);
    ExprPtr source = literalTuple ? ExprPtr(std::make_unique<ArrayLitExpr>(std::move(rhs))) : std::move(rhs[0]);
    source->span = sp;
    if (!literalTuple) {  // any iterable (generator, string, dict) unpacks: make it a list first
        std::vector<ExprPtr> la;
        la.push_back(std::move(source));
        source = mkCall("list", std::move(la), sp);
    }
    std::vector<StmtPtr> stmts;
    stmts.push_back(mkLet(tmp, std::move(source), sp));
    for (size_t k = 0; k < targets.size(); k++) {
        ExprPtr val;
        int after = static_cast<int>(targets.size()) - 1 - static_cast<int>(k);
        if (starAt >= 0 && static_cast<int>(k) == starAt) {  // xs[k : len(xs) - after]
            std::vector<ExprPtr> la;
            la.push_back(mkIdent(tmp, sp));
            ExprPtr stop = mkBin("-", mkCall("panjang", std::move(la), sp), mkNum(after, sp), sp);
            std::vector<ExprPtr> sa;
            sa.push_back(mkIdent(tmp, sp));
            sa.push_back(mkNum(static_cast<double>(k), sp));
            sa.push_back(std::move(stop));
            val = mkCall("__iris", std::move(sa), sp);
        } else if (starAt >= 0 && static_cast<int>(k) > starAt) {  // counted from the end
            val = std::make_unique<IndexExpr>(mkIdent(tmp, sp), mkNum(-static_cast<double>(after) - 1, sp));
        } else {
            val = std::make_unique<IndexExpr>(mkIdent(tmp, sp), mkNum(static_cast<double>(k), sp));
        }
        val->span = sp;
        ExprPtr assign;
        if (targets[k]->kind == ExprKind::Identifier) {
            assign = std::make_unique<AssignExpr>(static_cast<IdentifierExpr*>(targets[k].get())->name, std::move(val));
        } else if (targets[k]->kind == ExprKind::Index) {
            auto* idx = static_cast<IndexExpr*>(targets[k].get());
            assign = std::make_unique<IndexAssignExpr>(std::move(idx->target), std::move(idx->index), std::move(val));
        } else {
            throw ParseError(i18n::tr("Target penugasan nggak valid", "Invalid assignment target"), peek());
        }
        assign->span = sp;
        StmtPtr st = std::make_unique<ExprStmtNode>(std::move(assign));
        st->span = sp;
        stmts.push_back(std::move(st));
    }
    // A bare block would hide the temp; statements are spliced by the caller via pendingStmts_.
    StmtPtr blk = std::make_unique<BlockStmt>(std::move(stmts));
    blk->span = sp;
    return blk;
}

StmtPtr Parser::exprStmt() {
    Span sp = peek().span;
    if (isWord(peek(), "del", "hapus") && peekAt(1).type == TokenType::Ident) {
        advance();
        ExprPtr target = expression();
        expectEnd(i18n::tr("';' diharapkan setelah 'del'", "Expected ';' after 'del'"));
        if (target->kind != ExprKind::Index) {
            throw ParseError(i18n::tr("'del' butuh x[i] atau x[kunci]", "'del' needs x[i] or x[key]"), peek());
        }
        auto* idx = static_cast<IndexExpr*>(target.get());
        std::vector<ExprPtr> a;
        a.push_back(std::move(idx->target));
        a.push_back(std::move(idx->index));
        return std::make_unique<ExprStmtNode>(mkCall("_delitem", std::move(a), sp));
    }
    if (isWord(peek(), "assert", "pastikan")) {
        advance();
        std::vector<ExprPtr> a;
        a.push_back(expression());
        if (match(TokenType::Comma)) a.push_back(expression());
        expectEnd(i18n::tr("';' diharapkan setelah 'assert'", "Expected ';' after 'assert'"));
        return std::make_unique<ExprStmtNode>(mkCall("_assert", std::move(a), sp));
    }
    if (check(TokenType::Star) && peekAt(1).type == TokenType::Ident) {  // *init, last = xs
        advance();
        leadingStar_ = true;
    }
    ExprPtr expr = expression();
    if (check(TokenType::Comma) && (expr->kind == ExprKind::Identifier || expr->kind == ExprKind::Index)) {
        return tupleAssign(std::move(expr), sp);
    }
    expectEnd( i18n::tr("';' diharapkan setelah ekspresi", "Expected ';' after expression"));
    return std::make_unique<ExprStmtNode>(std::move(expr));
}

ExprPtr Parser::parseSingleExpression() {
    ExprPtr expr = expression();
    if (!atEnd()) {
        throw ParseError(
            i18n::tr("Ada input tambahan yang nggak terduga setelah ekspresi",
                      "Unexpected extra input after expression"),
            peek());
    }
    return expr;
}

ExprPtr Parser::expression() { return assignment(); }

// Builtin exception classes used here (and not defined here) come from the embedded __exc module.
void Parser::injectExceptionClasses(Program& program) {
    if (usesAsync_) {
        Span sp{};
        for (const char* nm : {"_await", "_mkco"}) {
            std::vector<ExprPtr> a;
            a.push_back(LiteralExpr::makeString("asyncio"));
            ExprPtr get = std::make_unique<IndexExpr>(mkCall("impor", std::move(a), sp), LiteralExpr::makeString(nm));
            get->span = sp;
            program.statements.insert(program.statements.begin(), mkLet(nm, std::move(get), sp));
        }
    }
    for (const auto& name : usedExc_) {
        bool own = false;
        for (const auto& d : declaredClasses_) if (d == name) own = true;
        if (own) continue;
        Span sp{};
        std::vector<ExprPtr> a;
        a.push_back(LiteralExpr::makeString("__exc"));
        ExprPtr get = std::make_unique<IndexExpr>(mkCall("impor", std::move(a), sp), LiteralExpr::makeString(name));
        get->span = sp;
        program.statements.insert(program.statements.begin(), mkLet(name, std::move(get), sp));
    }
}

void Parser::injectGeneratorRuntime(Program& program) {
        Span sp{};
        auto pick = [&](const char* field) {
            std::vector<ExprPtr> a;
            a.push_back(LiteralExpr::makeString("__gen"));
            ExprPtr get = std::make_unique<IndexExpr>(mkCall("impor", std::move(a), sp), LiteralExpr::makeString(field));
            get->span = sp;
            return get;
        };
        program.statements.insert(program.statements.begin(), mkLet("_mkgen", pick("mk"), sp));
}

// yield from it  ->  for __yv in it: __y(__yv)
StmtPtr Parser::yieldFromStmt(Span start) {
    advance();
    advance();
    usesGen_ = true;
    if (!yieldStack_.empty()) yieldStack_.back() = true;
    ExprPtr it = expression();
    expectEnd(i18n::tr("';' diharapkan setelah 'yield from'", "Expected ';' after 'yield from'"));
    std::string v = "__yv" + std::to_string(hiddenCounter_++);
    std::vector<ExprPtr> ya;
    ya.push_back(mkIdent(v, start));
    std::vector<StmtPtr> bodyStmts;
    StmtPtr call = std::make_unique<ExprStmtNode>(mkCall("__y", std::move(ya), start));
    call->span = start;
    bodyStmts.push_back(std::move(call));
    auto body = std::make_unique<BlockStmt>(std::move(bodyStmts));
    body->span = start;
    return buildForIn({v}, std::move(it), std::move(body), start);
}

// `yield v` / `yield from it` / bare `yield`: calls to the generator's own `__y` callback.
ExprPtr Parser::yieldExpr() {
    Span sp = advance().span;
    usesGen_ = true;
    if (!yieldStack_.empty()) yieldStack_.back() = true;
    std::vector<ExprPtr> a;
    if (isWord(peek(), "from", "dari")) {
        throw ParseError(i18n::tr("'yield from' cuma boleh sebagai pernyataan sendiri", "'yield from' is only allowed as a statement"), peek());
    }
    ExprPtr v;
    TokenType t = peek().type;
    if (t == TokenType::Semi || t == TokenType::RParen || t == TokenType::RBrace || t == TokenType::RBracket ||
        t == TokenType::Comma || t == TokenType::Eof) {
        v = LiteralExpr::makeNull();
    } else {
        v = expression();
        if (check(TokenType::Comma)) {  // yield a, b -> a tuple
            std::vector<ExprPtr> items;
            items.push_back(std::move(v));
            while (match(TokenType::Comma)) items.push_back(expression());
            v = std::make_unique<ArrayLitExpr>(std::move(items));
            v->span = sp;
        }
    }
    a.push_back(std::move(v));
    return mkCall("__y", std::move(a), sp);
}

// Every function below stamps `start` onto whichever new node it
// builds; an unchanged pass-through keeps the inner call's span, so
// span precision only ever improves going deeper, never regresses.

ExprPtr Parser::assignment() {
    Span start = peek().span;
    if (check(TokenType::Ident) && peek().text == "yield" && peekAt(1).type != TokenType::Eq &&
        peekAt(1).type != TokenType::Dot) {
        return yieldExpr();
    }
    ExprPtr expr = logicOr();

    // Python conditional expression: `a if cond else b` (the `if` must be on the same line).
    if (check(TokenType::If) && pos_ > 0 && peek().span.line == tokens_[pos_ - 1].span.line) {
        advance();
        ExprPtr cond = logicOr();
        expect(TokenType::Else, i18n::tr("'else' diharapkan dalam ekspresi kondisional", "Expected 'else' in conditional expression"));
        ExprPtr other = assignment();
        ExprPtr branches = std::make_unique<BinaryExpr>(":", std::move(expr), std::move(other));
        branches->span = start;
        expr = std::make_unique<BinaryExpr>("?", std::move(cond), std::move(branches));
        expr->span = start;
    }

    if (match(TokenType::Eq)) {
        ExprPtr value = assignment();
        if (expr->kind == ExprKind::Identifier) {
            std::string name = static_cast<IdentifierExpr*>(expr.get())->name;
            ExprPtr result = std::make_unique<AssignExpr>(std::move(name), std::move(value));
            result->span = start;
            return result;
        }
        if (expr->kind == ExprKind::Index) {
            auto* idx = static_cast<IndexExpr*>(expr.get());
            ExprPtr result = std::make_unique<IndexAssignExpr>(std::move(idx->target), std::move(idx->index), std::move(value));
            result->span = start;
            return result;
        }
        throw ParseError(i18n::tr("Target assignment nggak valid", "Invalid assignment target"), peek());
    }

    std::string compoundOp;
    switch (peek().type) {
        case TokenType::PlusEq: compoundOp = "+"; break;
        case TokenType::MinusEq: compoundOp = "-"; break;
        case TokenType::StarEq: compoundOp = "*"; break;
        case TokenType::SlashEq: compoundOp = "/"; break;
        case TokenType::CompoundAssign: compoundOp = peek().text; break;
        default: break;
    }
    if (!compoundOp.empty()) {
        if (expr->kind != ExprKind::Identifier && expr->kind != ExprKind::Index) {
            throw ParseError(i18n::tr("Target penugasan nggak valid", "Invalid assignment target"), peek());
        }
        advance();
        ExprPtr rhs = assignment();
        auto combine = [&](ExprPtr l, ExprPtr r) -> ExprPtr {
            ExprPtr e;
            if (compoundOp == "//") { std::vector<ExprPtr> a; a.push_back(std::move(l)); a.push_back(std::move(r)); e = mkCall("_floordiv", std::move(a), start); }
            else if (compoundOp == "**") { std::vector<ExprPtr> a; a.push_back(std::move(l)); a.push_back(std::move(r)); e = mkCall("_pow", std::move(a), start); }
            else if (compoundOp == "|" || compoundOp == "&" || compoundOp == "^" || compoundOp == "<<" || compoundOp == ">>") {
                const char* fn = compoundOp == "|" ? "_bitor" : compoundOp == "&" ? "_bitand" : compoundOp == "^" ? "_bitxor" : compoundOp == "<<" ? "_shl" : "_shr";
                std::vector<ExprPtr> a; a.push_back(std::move(l)); a.push_back(std::move(r)); e = mkCall(fn, std::move(a), start);
            } else {
                e = std::make_unique<BinaryExpr>(compoundOp, std::move(l), std::move(r));
            }
            e->span = start;
            return e;
        };
        if (expr->kind == ExprKind::Identifier) {
            std::string name = static_cast<IdentifierExpr*>(expr.get())->name;
            ExprPtr current = mkIdent(name, start);
            ExprPtr result = std::make_unique<AssignExpr>(std::move(name), combine(std::move(current), std::move(rhs)));
            result->span = start;
            return result;
        }
        // x[i] op= v   /   obj.field op= v
        auto* idx = static_cast<IndexExpr*>(expr.get());
        ExprPtr t2 = cloneSimple(idx->target.get());
        ExprPtr i2 = cloneSimple(idx->index.get());
        if (t2 && i2) {
            ExprPtr read = std::make_unique<IndexExpr>(std::move(t2), std::move(i2));
            read->span = start;
            ExprPtr result = std::make_unique<IndexAssignExpr>(std::move(idx->target), std::move(idx->index), combine(std::move(read), std::move(rhs)));
            result->span = start;
            return result;
        }
        // Complex target: evaluate its parts once inside a function called on the spot.
        std::string tn = "__ct" + std::to_string(hiddenCounter_), in = "__ci" + std::to_string(hiddenCounter_);
        hiddenCounter_++;
        std::vector<StmtPtr> body;
        body.push_back(mkLet(tn, std::move(idx->target), start));
        body.push_back(mkLet(in, std::move(idx->index), start));
        ExprPtr read = std::make_unique<IndexExpr>(mkIdent(tn, start), mkIdent(in, start));
        read->span = start;
        ExprPtr write = std::make_unique<IndexAssignExpr>(mkIdent(tn, start), mkIdent(in, start), combine(std::move(read), std::move(rhs)));
        write->span = start;
        StmtPtr ws = std::make_unique<ExprStmtNode>(std::move(write));
        ws->span = start;
        body.push_back(std::move(ws));
        ExprPtr rd2 = std::make_unique<IndexExpr>(mkIdent(tn, start), mkIdent(in, start));
        rd2->span = start;
        StmtPtr ret = std::make_unique<ReturnStmt>(std::move(rd2));
        ret->span = start;
        body.push_back(std::move(ret));
        auto decl = std::make_unique<FnDeclStmt>("", std::vector<std::string>{}, std::make_unique<BlockStmt>(std::move(body)));
        ExprPtr fn = std::make_unique<FnExprNode>(std::move(decl));
        fn->span = start;
        ExprPtr call = std::make_unique<CallExpr>(std::move(fn), std::vector<ExprPtr>{});
        call->span = start;
        return call;
    }

    return expr;
}

ExprPtr Parser::logicOr() {
    Span start = peek().span;
    ExprPtr expr = logicAnd();
    while (match(TokenType::Or) || matchWord("or", "atau")) {
        ExprPtr right = logicAnd();
        expr = std::make_unique<BinaryExpr>("||", std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

ExprPtr Parser::logicAnd() {
    Span start = peek().span;
    ExprPtr expr = equality();
    while (match(TokenType::And) || matchWord("and", "dan")) {
        ExprPtr right = equality();
        expr = std::make_unique<BinaryExpr>("&&", std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

ExprPtr Parser::equality() {
    Span start = peek().span;
    ExprPtr expr = comparison();
    while (check(TokenType::EqEq) || check(TokenType::Neq)) {
        std::string op = advance().text;
        ExprPtr right = comparison();
        expr = std::make_unique<BinaryExpr>(std::move(op), std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

// | ^ & << >> (Python precedence, looser than + -, tighter than comparisons).
static ExprPtr bitCall(const char* fn, ExprPtr l, ExprPtr r, Span sp) {
    std::vector<ExprPtr> a;
    a.push_back(std::move(l));
    a.push_back(std::move(r));
    return mkCall(fn, std::move(a), sp);
}

ExprPtr Parser::shiftExpr() {
    Span start = peek().span;
    ExprPtr expr = term();
    while (check(TokenType::Shl) || check(TokenType::Shr)) {
        bool left = check(TokenType::Shl);
        advance();
        expr = bitCall(left ? "_shl" : "_shr", std::move(expr), term(), start);
    }
    return expr;
}

ExprPtr Parser::bitAndExpr() {
    Span start = peek().span;
    ExprPtr expr = shiftExpr();
    while (check(TokenType::Amp)) {
        advance();
        expr = bitCall("_bitand", std::move(expr), shiftExpr(), start);
    }
    return expr;
}

ExprPtr Parser::bitXorExpr() {
    Span start = peek().span;
    ExprPtr expr = bitAndExpr();
    while (check(TokenType::Caret)) {
        advance();
        expr = bitCall("_bitxor", std::move(expr), bitAndExpr(), start);
    }
    return expr;
}

ExprPtr Parser::bitOrExpr() {
    Span start = peek().span;
    ExprPtr expr = bitXorExpr();
    while (check(TokenType::Pipe)) {
        advance();
        expr = bitCall("_bitor", std::move(expr), bitXorExpr(), start);
    }
    return expr;
}

ExprPtr Parser::comparison() {
    Span start = peek().span;
    ExprPtr expr = bitOrExpr();
    for (;;) {
        auto relational = [&]() {
            return check(TokenType::Lt) || check(TokenType::Lte) || check(TokenType::Gt) || check(TokenType::Gte);
        };
        if (relational()) {
            // Chains (`a < b < c`) mean (a < b) and (b < c) with `b` evaluated once.
            std::vector<ExprPtr> operands;
            std::vector<std::string> ops;
            operands.push_back(std::move(expr));
            while (relational()) {
                ops.push_back(advance().text);
                operands.push_back(bitOrExpr());
            }
            if (ops.size() == 1) {
                expr = std::make_unique<BinaryExpr>(std::move(ops[0]), std::move(operands[0]), std::move(operands[1]));
                expr->span = start;
                continue;
            }
            auto cloneSimple = [&](const Expr* e) -> ExprPtr {
                if (e->kind == ExprKind::Identifier) return mkIdent(static_cast<const IdentifierExpr*>(e)->name, e->span);
                if (e->kind == ExprKind::Literal) {
                    auto* l = static_cast<const LiteralExpr*>(e);
                    ExprPtr c;
                    if (l->litKind == LiteralExpr::Kind::Number) c = LiteralExpr::makeNumber(l->number);
                    else if (l->litKind == LiteralExpr::Kind::String) c = LiteralExpr::makeString(l->str);
                    else if (l->litKind == LiteralExpr::Kind::Bool) c = LiteralExpr::makeBool(l->boolean);
                    else c = LiteralExpr::makeNull();
                    c->span = e->span;
                    return c;
                }
                return nullptr;
            };
            size_t n = operands.size();
            bool simple = true;
            for (size_t i = 1; i + 1 < n; i++) if (!cloneSimple(operands[i].get())) simple = false;
            std::vector<ExprPtr> lefts(n - 1), rights(n - 1);
            std::vector<StmtPtr> temps;
            for (size_t k = 0; k + 1 < n; k++) {
                // operands[k+1] is the right side of comparison k and the left side of comparison k+1
                if (k == 0) lefts[0] = std::move(operands[0]);
                if (k + 2 < n) {  // interior operand: used twice
                    if (simple) {
                        lefts[k + 1] = cloneSimple(operands[k + 1].get());
                        rights[k] = std::move(operands[k + 1]);
                    } else {
                        std::string tmp = "__m" + std::to_string(hiddenCounter_++);
                        temps.push_back(mkLet(tmp, std::move(operands[k + 1]), start));
                        lefts[k + 1] = mkIdent(tmp, start);
                        rights[k] = mkIdent(tmp, start);
                    }
                } else {
                    rights[k] = std::move(operands[k + 1]);
                }
            }
            ExprPtr chain;
            for (size_t k = 0; k + 1 < n; k++) {
                ExprPtr cmp = std::make_unique<BinaryExpr>(ops[k], std::move(lefts[k]), std::move(rights[k]));
                cmp->span = start;
                if (!chain) chain = std::move(cmp);
                else {
                    chain = std::make_unique<BinaryExpr>("&&", std::move(chain), std::move(cmp));
                    chain->span = start;
                }
            }
            if (!simple) {
                // let __m = ...; return chain   -- inside a function called on the spot
                std::vector<StmtPtr> body = std::move(temps);
                StmtPtr ret = std::make_unique<ReturnStmt>(std::move(chain));
                ret->span = start;
                body.push_back(std::move(ret));
                auto decl = std::make_unique<FnDeclStmt>("", std::vector<std::string>{}, std::make_unique<BlockStmt>(std::move(body)));
                ExprPtr fn = std::make_unique<FnExprNode>(std::move(decl));
                fn->span = start;
                chain = std::make_unique<CallExpr>(std::move(fn), std::vector<ExprPtr>{});
                chain->span = start;
            }
            expr = std::move(chain);
        } else if (isWord(peek(), "in", "dalam")) {  // a in b
            advance();
            ExprPtr right = bitOrExpr();
            std::vector<ExprPtr> args;
            args.push_back(std::move(expr));
            args.push_back(std::move(right));
            expr = mkCall("_in", std::move(args), start);
        } else if (check(TokenType::Not) && peek().text == "not" && isWord(peekAt(1), "in", "dalam")) {  // a not in b
            advance();
            advance();
            ExprPtr right = bitOrExpr();
            std::vector<ExprPtr> args;
            args.push_back(std::move(expr));
            args.push_back(std::move(right));
            expr = std::make_unique<UnaryExpr>("!", mkCall("_in", std::move(args), start));
            expr->span = start;
        } else if (isWord(peek(), "is", "adalah")) {  // a is b / a is not b  (value equality)
            advance();
            bool negate = false;
            if (check(TokenType::Not) && peek().text == "not") { advance(); negate = true; }
            ExprPtr right = bitOrExpr();
            expr = std::make_unique<BinaryExpr>(negate ? "!=" : "==", std::move(expr), std::move(right));
            expr->span = start;
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::term() {
    Span start = peek().span;
    ExprPtr expr = factor();
    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        std::string op = advance().text;
        ExprPtr right = factor();
        expr = std::make_unique<BinaryExpr>(std::move(op), std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

ExprPtr Parser::factor() {
    Span start = peek().span;
    ExprPtr expr = unary();
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent) || check(TokenType::SlashSlash)) {
        bool floorDiv = check(TokenType::SlashSlash);
        std::string op = advance().text;
        ExprPtr right = unary();
        if (floorDiv) {
            std::vector<ExprPtr> args;
            args.push_back(std::move(expr));
            args.push_back(std::move(right));
            expr = mkCall("_floordiv", std::move(args), start);
        } else {
            expr = std::make_unique<BinaryExpr>(std::move(op), std::move(expr), std::move(right));
            expr->span = start;
        }
    }
    return expr;
}

ExprPtr Parser::unary() {
    Span start = peek().span;
    if (check(TokenType::Ident) && peek().text == "await") {
        TokenType nx = peekAt(1).type;
        if (nx != TokenType::Eq && nx != TokenType::Dot && nx != TokenType::Comma && nx != TokenType::RParen &&
            nx != TokenType::Semi && nx != TokenType::RBracket && nx != TokenType::Colon) {
            advance();
            usesAsync_ = true;
            std::vector<ExprPtr> a;
            a.push_back(unary());
            return mkCall("_await", std::move(a), start);
        }
    }
    if (check(TokenType::Not) && (peek().text == "not" || peek().text == "bukan")) {
        // Python `not`: binds looser than comparison (`not a == b` is `!(a == b)`).
        advance();
        ExprPtr operand = equality();
        ExprPtr result = std::make_unique<UnaryExpr>("!", std::move(operand));
        result->span = start;
        return result;
    }
    if (check(TokenType::Tilde)) {
        advance();
        std::vector<ExprPtr> a;
        a.push_back(unary());
        return mkCall("_bitnot", std::move(a), start);
    }
    if (check(TokenType::Minus) || check(TokenType::Not)) {
        std::string op = advance().text;
        ExprPtr operand = unary();
        ExprPtr result = std::make_unique<UnaryExpr>(std::move(op), std::move(operand));
        result->span = start;
        return result;
    }
    return power();
}

// `a ** b` (right associative, binds tighter than a unary minus on its left).
ExprPtr Parser::power() {
    Span start = peek().span;
    ExprPtr base = call();
    if (check(TokenType::StarStar)) {
        advance();
        ExprPtr exponent = unary();
        std::vector<ExprPtr> args;
        args.push_back(std::move(base));
        args.push_back(std::move(exponent));
        return mkCall("_pow", std::move(args), start);
    }
    return base;
}

ExprPtr Parser::call() {
    Span start = peek().span;
    ExprPtr expr = primary();
    while (true) {
        if (check(TokenType::LParen)) {
            advance();
            std::vector<ExprPtr> args;
            std::vector<ExprPtr> kwFlat;  // name, value, name, value ... for name=value arguments
            std::vector<ExprPtr> posParts;    // pieces of the positional list when *spread is used
            std::vector<ExprPtr> kwParts;     // map pieces: named pairs and **spread maps
            bool spread = false;
            auto flushNamed = [&]() {
                if (kwFlat.empty()) return;
                kwParts.push_back(mkCall("_peta", std::move(kwFlat), start));
                kwFlat.clear();
            };
            auto parseArg = [&]() {
                if (check(TokenType::Star)) {
                    advance();
                    spread = true;
                    if (!args.empty()) {
                        posParts.push_back(std::make_unique<ArrayLitExpr>(std::move(args)));
                        args.clear();
                    }
                    std::vector<ExprPtr> one;
                    one.push_back(expression());
                    posParts.push_back(mkCall("list", std::move(one), start));
                    return;
                }
                if (check(TokenType::StarStar)) {
                    advance();
                    spread = true;
                    flushNamed();
                    kwParts.push_back(expression());
                    return;
                }
                if (check(TokenType::Ident) && peekAt(1).type == TokenType::Eq) {
                    kwFlat.push_back(LiteralExpr::makeString(peek().text));
                    kwFlat.back()->span = peek().span;
                    advance();
                    advance();
                    kwFlat.push_back(expression());
                } else {
                    if (!kwFlat.empty() || !kwParts.empty()) {
                        throw ParseError(i18n::tr("Argumen posisi nggak boleh setelah argumen bernama",
                                                  "Positional argument follows keyword argument"), peek());
                    }
                    args.push_back(expression());
                    if (check(TokenType::For) && args.size() == 1) {  // f(x for x in xs): generator -> list
                        args[0] = comprehension(std::move(args[0]), nullptr, false, start);
                    }
                }
            };
            if (!check(TokenType::RParen)) {
                parseArg();
                while (match(TokenType::Comma)) {
                    if (check(TokenType::RParen)) break;  // trailing comma
                    parseArg();
                }
            }
            expect(TokenType::RParen, i18n::tr("')' diharapkan setelah argumen", "Expected ')' after arguments"));
            if (!kwFlat.empty() || spread) {
                // f(a, k=v) -> _callkw(f, [a], {k: v});  o.m(a, k=v) -> _callkwm(o, "m", [a], {k: v})
                // f(*xs, **d) -> _callkw(f, _concat([..], list(xs)), _kwmerge({..}, d))
                flushNamed();
                ExprPtr posList;
                if (!posParts.empty()) {
                    if (!args.empty()) posParts.push_back(std::make_unique<ArrayLitExpr>(std::move(args)));
                    posList = mkCall("_concat", std::move(posParts), start);
                } else {
                    posList = std::make_unique<ArrayLitExpr>(std::move(args));
                }
                posList->span = start;
                ExprPtr kwMap;
                if (kwParts.size() == 1) kwMap = std::move(kwParts[0]);
                else kwMap = mkCall("_kwmerge", std::move(kwParts), start);
                std::vector<ExprPtr> outer;
                bool isMethod = expr->kind == ExprKind::Index &&
                                static_cast<IndexExpr*>(expr.get())->index->kind == ExprKind::Literal &&
                                static_cast<LiteralExpr*>(static_cast<IndexExpr*>(expr.get())->index.get())->litKind == LiteralExpr::Kind::String;
                if (isMethod) {
                    auto* ix = static_cast<IndexExpr*>(expr.get());
                    outer.push_back(std::move(ix->target));
                    outer.push_back(std::move(ix->index));
                    outer.push_back(std::move(posList));
                    outer.push_back(std::move(kwMap));
                    expr = mkCall("_callkwm", std::move(outer), start);
                } else {
                    outer.push_back(std::move(expr));
                    outer.push_back(std::move(posList));
                    outer.push_back(std::move(kwMap));
                    expr = mkCall("_callkw", std::move(outer), start);
                }
            } else {
                expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
            }
            expr->span = start;
        } else if (check(TokenType::LBracket)) {
            advance();
            // Python slices: x[a:b], x[:b], x[a:], x[:] -> __iris(x, a|kosong, b|kosong)
            ExprPtr index;
            bool slice = false;
            ExprPtr hi;
            if (!check(TokenType::Colon)) index = expression();
            ExprPtr step;
            if (match(TokenType::Colon)) {
                slice = true;
                if (!check(TokenType::RBracket) && !check(TokenType::Colon)) hi = expression();
                if (match(TokenType::Colon)) {
                    if (!check(TokenType::RBracket)) step = expression();
                }
            }
            expect(TokenType::RBracket, i18n::tr("']' diharapkan setelah index", "Expected ']' after index"));
            if (slice) {
                std::vector<ExprPtr> args;
                args.push_back(std::move(expr));
                args.push_back(index ? std::move(index) : LiteralExpr::makeNull());
                args.push_back(hi ? std::move(hi) : LiteralExpr::makeNull());
                if (step) args.push_back(std::move(step));
                ExprPtr callee = std::make_unique<IdentifierExpr>("__iris");
                callee->span = start;
                expr = std::make_unique<CallExpr>(std::move(callee), std::move(args));
            } else {
                auto ix = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
                ix->strict = strictKeys_;
                expr = std::move(ix);
            }
            expr->span = start;
        } else if (check(TokenType::Dot)) {
            // `obj.field` is sugar for `obj["field"]` -- same IndexExpr
            // node, so reads, writes (via assignment()) and map/module
            // lookups all just work without any extra interpreter code.
            advance();
            std::string field;
            if (peek().text == "__init__") {
                advance();
                field = "konstruktor";
            } else if (!peek().text.empty() && peek().type != TokenType::Semi && peek().type != TokenType::RParen && peek().type != TokenType::RBrace && peek().type != TokenType::RBracket) {
                field = advance().text;
            } else {
                field = expect(TokenType::Ident, i18n::tr("Nama field diharapkan setelah '.'", "Expected field name after '.'")).text;
            }
            ExprPtr key = LiteralExpr::makeString(field);
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(key));
            expr->span = start;
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::primary() {
    const Token& tok = peek();
    Span start = tok.span;
    switch (tok.type) {
        case TokenType::Number: {
            advance();
            ExprPtr e = LiteralExpr::makeNumber(tok.number);
            e->span = start;
            return e;
        }
        case TokenType::String: {
            advance();
            ExprPtr e = LiteralExpr::makeString(tok.str);
            e->span = start;
            return e;
        }
        case TokenType::True_: {
            advance();
            ExprPtr e = LiteralExpr::makeBool(true);
            e->span = start;
            return e;
        }
        case TokenType::False_: {
            advance();
            ExprPtr e = LiteralExpr::makeBool(false);
            e->span = start;
            return e;
        }
        case TokenType::Null_: {
            advance();
            ExprPtr e = LiteralExpr::makeNull();
            e->span = start;
            return e;
        }
        case TokenType::Ident: {
            if (isWord(tok, "lambda") && (peekAt(1).type == TokenType::Ident || peekAt(1).type == TokenType::Colon)) {
                // Python: lambda a, b: <expr>
                advance();
                std::vector<std::string> params;
                std::vector<ExprPtr> defaults;
                while (!check(TokenType::Colon)) {
                    params.push_back(expect(TokenType::Ident, i18n::tr("Nama parameter diharapkan", "Expected parameter name")).text);
                    defaults.push_back(match(TokenType::Eq) ? expression() : nullptr);
                    if (!match(TokenType::Comma)) break;
                }
                expect(TokenType::Colon, i18n::tr("':' diharapkan setelah parameter lambda", "Expected ':' after lambda parameters"));
                ExprPtr body = assignment();
                std::vector<StmtPtr> stmts;
                int minArgs = static_cast<int>(params.size());
                for (size_t i = 0; i < defaults.size(); i++) {
                    if (defaults[i]) { minArgs = static_cast<int>(i); break; }
                }
                for (size_t i = defaults.size(); i-- > static_cast<size_t>(minArgs);) {
                    if (!defaults[i]) throw ParseError(i18n::tr("Parameter tanpa nilai default nggak boleh setelah yang punya default",
                                                                "Non-default parameter follows a default one"), peek());
                    Span sp = defaults[i]->span;
                    ExprPtr cond = mkBin("==", mkIdent(params[i], sp), LiteralExpr::makeNull(), sp);
                    ExprPtr assign = std::make_unique<AssignExpr>(params[i], hoistDefault(std::move(defaults[i])));
                    assign->span = sp;
                    std::vector<StmtPtr> thenStmts;
                    thenStmts.push_back(std::make_unique<ExprStmtNode>(std::move(assign)));
                    stmts.insert(stmts.begin(), std::make_unique<IfStmt>(std::move(cond), std::make_unique<BlockStmt>(std::move(thenStmts)), nullptr));
                }
                stmts.push_back(std::make_unique<ReturnStmt>(std::move(body)));
                auto decl = std::make_unique<FnDeclStmt>("", std::move(params), std::make_unique<BlockStmt>(std::move(stmts)));
                if (minArgs < static_cast<int>(decl->params.size())) decl->minArgs = minArgs;
                ExprPtr e = std::make_unique<FnExprNode>(std::move(decl));
                e->span = start;
                return e;
            }
            advance();
            noteName(tok.text);
            ExprPtr e = std::make_unique<IdentifierExpr>(tok.text);
            e->span = start;
            return e;
        }
        case TokenType::This: {
            advance();
            ExprPtr e = std::make_unique<IdentifierExpr>("ini");
            e->span = start;
            return e;
        }
        case TokenType::Super: {
            advance();
            if (check(TokenType::LParen) && peekAt(1).type == TokenType::RParen) {  // Python: super()
                advance();
                advance();
            }
            ExprPtr e = std::make_unique<IdentifierExpr>("induk");
            e->span = start;
            return e;
        }
        case TokenType::LBracket: {
            advance();
            std::vector<ExprPtr> elements;
            if (!check(TokenType::RBracket)) {
                elements.push_back(expression());
                if (check(TokenType::For)) {
                    ExprPtr comp = comprehension(std::move(elements[0]), nullptr, false, start);
                    expect(TokenType::RBracket, i18n::tr("']' diharapkan setelah komprehensi", "Expected ']' after comprehension"));
                    return comp;
                }
                while (match(TokenType::Comma)) {
                    if (check(TokenType::RBracket)) break;  // trailing comma
                    elements.push_back(expression());
                }
            }
            expect(TokenType::RBracket, i18n::tr("']' diharapkan setelah elemen larik", "Expected ']' after array elements"));
            ExprPtr e = std::make_unique<ArrayLitExpr>(std::move(elements));
            e->span = start;
            return e;
        }
        case TokenType::LDict: {
            advance();
            std::vector<ExprPtr> flat;  // k1, v1, k2, v2, ...
            while (!check(TokenType::RDict)) {
                flat.push_back(expression());
                if (flat.size() == 1 && !check(TokenType::Colon)) {
                    // {a, b, c} / {x for ...}: a set (a list without duplicates)
                    std::vector<ExprPtr> items;
                    ExprPtr setArg;
                    if (check(TokenType::For)) {
                        setArg = comprehension(std::move(flat[0]), nullptr, false, start);
                    } else {
                        items.push_back(std::move(flat[0]));
                        while (match(TokenType::Comma)) {
                            if (check(TokenType::RDict)) break;
                            items.push_back(expression());
                        }
                        setArg = std::make_unique<ArrayLitExpr>(std::move(items));
                        setArg->span = start;
                    }
                    expect(TokenType::RDict, i18n::tr("'}' diharapkan setelah himpunan", "Expected '}' after set"));
                    std::vector<ExprPtr> a;
                    a.push_back(std::move(setArg));
                    return mkCall("set", std::move(a), start);
                }
                expect(TokenType::Colon, i18n::tr("':' diharapkan setelah kunci peta", "Expected ':' after dict key"));
                flat.push_back(expression());
                if (flat.size() == 2 && check(TokenType::For)) {
                    ExprPtr comp = comprehension(std::move(flat[0]), std::move(flat[1]), true, start);
                    expect(TokenType::RDict, i18n::tr("'}' diharapkan setelah komprehensi", "Expected '}' after comprehension"));
                    return comp;
                }
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RDict, i18n::tr("'}' diharapkan setelah isi peta", "Expected '}' after dict entries"));
            return mkCall("_peta", std::move(flat), start);
        }
        case TokenType::LParen: {
            advance();
            if (match(TokenType::RParen)) {  // () -- empty tuple
                ExprPtr empty = std::make_unique<ArrayLitExpr>(std::vector<ExprPtr>{});
                empty->span = start;
                return empty;
            }
            ExprPtr expr = expression();
            if (check(TokenType::For)) {  // (x for x in xs): a lazy generator
                ExprPtr g = comprehension(std::move(expr), nullptr, false, start, true);
                expect(TokenType::RParen, i18n::tr("')' diharapkan setelah generator", "Expected ')' after generator"));
                return g;
            }
            if (check(TokenType::Comma)) {  // (a, b, ...) -- a tuple, represented as a list
                std::vector<ExprPtr> items;
                items.push_back(std::move(expr));
                while (match(TokenType::Comma)) {
                    if (check(TokenType::RParen)) break;
                    items.push_back(expression());
                }
                expect(TokenType::RParen, i18n::tr("')' diharapkan setelah tuple", "Expected ')' after tuple"));
                ExprPtr tuple = std::make_unique<ArrayLitExpr>(std::move(items));
                tuple->span = start;
                return tuple;
            }
            expect(TokenType::RParen, i18n::tr("')' diharapkan setelah ekspresi", "Expected ')' after expression"));
            return expr;
        }
        case TokenType::Fn: {
            advance();
            auto decl = fnDeclBody("");
            ExprPtr e = std::make_unique<FnExprNode>(std::move(decl));
            e->span = start;
            return e;
        }
        case TokenType::Lt: {
            return jsxElement();
        }
        default:
            throw ParseError(i18n::tr("Token nggak terduga", "Unexpected token"), tok);
    }
}

ExprPtr Parser::jsxElement() {
    Span start = peek().span;
    expect(TokenType::Lt, i18n::tr("'<' diharapkan di awal elemen JSX", "Expected '<' at start of JSX element"));

    ExprPtr tagExpr;
    if (check(TokenType::Gt)) {
        tagExpr = LiteralExpr::makeString("fragment");
    } else {
        const Token& nameTok = expect(TokenType::Ident, i18n::tr("Nama tag JSX diharapkan", "Expected JSX tag name"));
        std::string tagName = nameTok.text;
        if (!tagName.empty() && std::isupper(static_cast<unsigned char>(tagName[0]))) {
            tagExpr = std::make_unique<IdentifierExpr>(tagName);
        } else {
            tagExpr = LiteralExpr::makeString(tagName);
        }
    }

    struct JsxAttr {
        std::string key;
        ExprPtr val;
    };
    std::vector<JsxAttr> attrs;

    while (!atEnd() && !check(TokenType::Gt) && !(check(TokenType::Slash) && peekAt(1).type == TokenType::Gt)) {
        std::string key;
        if (!peek().text.empty() && peek().type != TokenType::Gt && peek().type != TokenType::Slash && peek().type != TokenType::Eq) {
            key = advance().text;
        } else if (check(TokenType::String)) {
            key = advance().str;
        } else {
            throw ParseError(i18n::tr("Nama atribut JSX tidak valid", "Invalid JSX attribute name"), peek());
        }

        ExprPtr val;
        if (match(TokenType::Eq)) {
            if (check(TokenType::LBrace)) {
                advance();
                val = expression();
                expect(TokenType::RBrace, i18n::tr("'}' diharapkan di akhir ekspresi atribut JSX", "Expected '}' after JSX attribute expression"));
            } else if (check(TokenType::String)) {
                val = LiteralExpr::makeString(advance().str);
            } else if (check(TokenType::Number)) {
                val = LiteralExpr::makeNumber(advance().number);
            } else {
                val = primary();
            }
        } else {
            val = LiteralExpr::makeBool(true);
        }
        attrs.push_back(JsxAttr{std::move(key), std::move(val)});
    }

    ExprPtr mapExpr;
    if (attrs.empty()) {
        mapExpr = std::make_unique<CallExpr>(std::make_unique<IdentifierExpr>("peta_baru"), std::vector<ExprPtr>{});
    } else {
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::make_unique<LetStmt>("_m", std::make_unique<CallExpr>(std::make_unique<IdentifierExpr>("peta_baru"), std::vector<ExprPtr>{})));
        for (auto& a : attrs) {
            stmts.push_back(std::make_unique<ExprStmtNode>(
                std::make_unique<IndexAssignExpr>(
                    std::make_unique<IdentifierExpr>("_m"),
                    LiteralExpr::makeString(a.key),
                    std::move(a.val)
                )
            ));
        }
        stmts.push_back(std::make_unique<ReturnStmt>(std::make_unique<IdentifierExpr>("_m")));

        auto fnDecl = std::make_unique<FnDeclStmt>("", std::vector<std::string>{}, std::make_unique<BlockStmt>(std::move(stmts)));
        auto fnExpr = std::make_unique<FnExprNode>(std::move(fnDecl));
        mapExpr = std::make_unique<CallExpr>(std::move(fnExpr), std::vector<ExprPtr>{});
    }

    std::vector<ExprPtr> children;

    if (check(TokenType::Slash) && peekAt(1).type == TokenType::Gt) {
        advance();
        advance();
    } else {
        // catat posisi token '>' pembuka buat deteksi gap baris pertama
        int refEnd = peek().span.end;
        int refLine = peek().span.line;
        expect(TokenType::Gt, i18n::tr("'>' diharapkan di akhir tag pembuka JSX", "Expected '>' after JSX opening tag"));

        while (!atEnd()) {
            if (check(TokenType::Lt) && peekAt(1).type == TokenType::Slash) {
                advance();
                advance();
                if (check(TokenType::Ident)) advance();
                lastJsxCloseSpan_ = peek().span;
                expect(TokenType::Gt, i18n::tr("'>' diharapkan di akhir tag penutup JSX", "Expected '>' at end of JSX closing tag"));
                break;
            } else if (check(TokenType::LBrace)) {
                advance();
                children.push_back(expression());
                // simpan posisi '}' sebagai anchor teks berikutnya
                refEnd = peek().span.end;
                refLine = peek().span.line;
                expect(TokenType::RBrace, i18n::tr("'}' diharapkan setelah ekspresi di anak JSX", "Expected '}' after JSX child expression"));
            } else if (check(TokenType::Lt)) {
                children.push_back(jsxElement());
                // anchor = '>' penutup anak, BUKAN start token berikutnya,
                // supaya spasi "</span> Berkelas" ke-deteksi
                refEnd = lastJsxCloseSpan_.end;
                refLine = lastJsxCloseSpan_.line;
            } else {
                std::string textContent;
                bool firstPiece = true;
                while (!atEnd() && !check(TokenType::Lt) && !check(TokenType::LBrace)) {
                    const Token& t = advance();
                    std::string piece = (t.type == TokenType::String ? t.str : t.text);
                    if (piece.empty()) continue;
                    // gap NYATA antar token (whitespace di source, baris sama)
                    bool gap = (refEnd >= 0 && t.span.line == refLine && t.span.start > refEnd);
                    char nextCh = piece[0];
                    bool joinNoSpace = (nextCh == ',' || nextCh == '.' || nextCh == ':' || nextCh == ';'
                                        || nextCh == ')' || nextCh == ']' || nextCh == '}' || nextCh == '?'
                                        || nextCh == '!' || nextCh == '%' || nextCh == '+' || nextCh == '/'
                                        || nextCh == '-');
                    if (!textContent.empty()) {
                        char lastCh = textContent.back();
                        if (lastCh != ' ' && gap && !joinNoSpace) {
                            textContent += " ";
                        }
                    } else if (firstPiece && gap && !joinNoSpace) {
                        // spasi setelah {expr} / <anak> di baris sama
                        textContent = " ";
                    }
                    textContent += piece;
                    refEnd = t.span.end;
                    refLine = t.span.line;
                    firstPiece = false;
                }
                if (!textContent.empty()) {
                    // spasi sebelum <anak> / {expr} di baris sama (bukan sebelum </tutup>)
                    bool beforeChild = check(TokenType::LBrace) || (check(TokenType::Lt) && peekAt(1).type != TokenType::Slash);
                    char lastCh = textContent.back();
                    if (beforeChild && lastCh != ' ' && peek().span.line == refLine && peek().span.start > refEnd) {
                        textContent += " ";
                    }
                    std::vector<ExprPtr> teksArgs;
                    teksArgs.push_back(LiteralExpr::makeString(textContent));
                    auto callTeks = std::make_unique<CallExpr>(
                        std::make_unique<IdentifierExpr>("teks"),
                        std::move(teksArgs)
                    );
                    children.push_back(std::move(callTeks));
                } else {
                    // run kosong tapi tetap maju anchor (mis. hanya whitespace)
                    refEnd = peek().span.start > 0 ? peek().span.start : refEnd;
                    refLine = peek().span.line;
                }
            }
        }
    }

    std::vector<ExprPtr> elemenArgs;
    elemenArgs.push_back(std::move(tagExpr));
    elemenArgs.push_back(std::move(mapExpr));
    elemenArgs.push_back(std::make_unique<ArrayLitExpr>(std::move(children)));

    auto result = std::make_unique<CallExpr>(std::make_unique<IdentifierExpr>("elemen"), std::move(elemenArgs));
    result->span = start;
    return result;
}

// `[expr for a in xs if cond ...]` / `{k: v for ...}`: an immediately-called function that builds
// the result, so the loop variables stay local to the comprehension.
ExprPtr Parser::comprehension(ExprPtr element, ExprPtr valueOrNull, bool isDict, Span sp, bool lazy) {
    struct Clause { std::vector<std::string> vars; ExprPtr iter; std::vector<ExprPtr> conds; };
    std::vector<Clause> clauses;
    while (check(TokenType::For)) {
        advance();
        Clause c;
        c.vars = forTargets();
        if (!matchWord("in", "dalam")) {
            throw ParseError(i18n::tr("'in' diharapkan setelah variabel 'for'", "Expected 'in' after the 'for' variable"), peek());
        }
        c.iter = logicOr();
        while (check(TokenType::If)) {
            advance();
            c.conds.push_back(logicOr());
        }
        clauses.push_back(std::move(c));
    }
    std::string res = "__c" + std::to_string(hiddenCounter_++);
    // innermost statement: append / assign
    StmtPtr inner;
    if (lazy) {
        usesGen_ = true;
        std::vector<ExprPtr> a;
        a.push_back(std::move(element));
        inner = std::make_unique<ExprStmtNode>(mkCall("__y", std::move(a), sp));
    } else if (isDict) {
        ExprPtr set = std::make_unique<IndexAssignExpr>(mkIdent(res, sp), std::move(element), std::move(valueOrNull));
        set->span = sp;
        inner = std::make_unique<ExprStmtNode>(std::move(set));
    } else {
        std::vector<ExprPtr> a;
        a.push_back(mkIdent(res, sp));
        a.push_back(std::move(element));
        inner = std::make_unique<ExprStmtNode>(mkCall("tambah", std::move(a), sp));
    }
    inner->span = sp;
    StmtPtr current = std::move(inner);
    for (size_t k = clauses.size(); k-- > 0;) {
        Clause& c = clauses[k];
        std::vector<StmtPtr> bodyStmts;
        // conditions wrap the inner statement: if c1 { if c2 { inner } }
        for (size_t q = c.conds.size(); q-- > 0;) {
            std::vector<StmtPtr> thenStmts;
            thenStmts.push_back(std::move(current));
            StmtPtr ifs = std::make_unique<IfStmt>(std::move(c.conds[q]), std::make_unique<BlockStmt>(std::move(thenStmts)), nullptr);
            ifs->span = sp;
            current = std::move(ifs);
        }
        bodyStmts.push_back(std::move(current));
        auto body = std::make_unique<BlockStmt>(std::move(bodyStmts));
        body->span = sp;
        current = buildForIn(c.vars, std::move(c.iter), std::move(body), sp);
    }
    if (lazy) {  // (expr for ...) -> _mkgen(fn(__y): for ...: __y(expr))
        std::vector<StmtPtr> gb;
        gb.push_back(std::move(current));
        auto inner = std::make_unique<FnDeclStmt>("", std::vector<std::string>{"__y"}, std::make_unique<BlockStmt>(std::move(gb)));
        ExprPtr fe = std::make_unique<FnExprNode>(std::move(inner));
        fe->span = sp;
        std::vector<ExprPtr> ga;
        ga.push_back(std::move(fe));
        return mkCall("_mkgen", std::move(ga), sp);
    }
    std::vector<StmtPtr> fnBody;
    ExprPtr init = isDict ? mkCall("_peta", std::vector<ExprPtr>{}, sp) : ExprPtr(std::make_unique<ArrayLitExpr>(std::vector<ExprPtr>{}));
    init->span = sp;
    fnBody.push_back(mkLet(res, std::move(init), sp));
    fnBody.push_back(std::move(current));
    StmtPtr ret = std::make_unique<ReturnStmt>(mkIdent(res, sp));
    ret->span = sp;
    fnBody.push_back(std::move(ret));
    auto decl = std::make_unique<FnDeclStmt>("", std::vector<std::string>{}, std::make_unique<BlockStmt>(std::move(fnBody)));
    ExprPtr fn = std::make_unique<FnExprNode>(std::move(decl));
    fn->span = sp;
    ExprPtr callExpr = std::make_unique<CallExpr>(std::move(fn), std::vector<ExprPtr>{});
    callExpr->span = sp;
    return callExpr;
}
