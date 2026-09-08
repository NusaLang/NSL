#include "parser.hpp"

#include "i18n.hpp"

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

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

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

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    while (!atEnd()) {
        program->statements.push_back(statement());
    }
    return program;
}

StmtPtr Parser::statement() {
    // Stamped uniformly here (rather than in each sub-parser) so every
    // statement kind gets a location for free -- see Stmt::span.
    Span start = peek().span;
    StmtPtr result;
    if (match(TokenType::Let)) result = letStmt();
    else if (match(TokenType::Fn)) result = fnDecl();
    else if (match(TokenType::Class)) result = classDecl();
    else if (match(TokenType::Struct)) result = structDecl();
    else if (match(TokenType::EnumKw)) result = enumDecl();
    else if (match(TokenType::Try)) result = tryStmt();
    else if (match(TokenType::Throw)) result = throwStmt();
    else if (match(TokenType::If)) result = ifStmt();
    else if (match(TokenType::While)) result = whileStmt();
    else if (match(TokenType::For)) result = forStmt();
    else if (match(TokenType::Return)) result = returnStmt();
    else if (match(TokenType::Break)) result = breakStmt();
    else if (match(TokenType::Continue)) result = continueStmt();
    else if (check(TokenType::LBrace)) result = block();
    else result = exprStmt();
    result->span = start;
    return result;
}

StmtPtr Parser::letStmt() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan", "Expected variable name")).text;
    std::string typeAnnotation;
    if (match(TokenType::Colon)) {
        typeAnnotation = expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'")).text;
    }
    expect(TokenType::Eq, i18n::tr("'=' diharapkan setelah nama variabel", "Expected '=' after variable name"));
    ExprPtr value = expression();
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah pernyataan 'buat'", "Expected ';' after let statement"));
    return std::make_unique<LetStmt>(std::move(name), std::move(value), std::move(typeAnnotation));
}

StmtPtr Parser::fnDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama fungsi diharapkan", "Expected function name")).text;
    return fnDeclBody(std::move(name));
}

std::unique_ptr<FnDeclStmt> Parser::fnDeclBody(std::string name) {
    expect(TokenType::LParen, i18n::tr("'(' diharapkan setelah nama fungsi", "Expected '(' after function name"));
    std::vector<std::string> params;
    std::vector<std::string> paramTypes;
    if (!check(TokenType::RParen)) {
        params.push_back(expect(TokenType::Ident, i18n::tr("Nama parameter diharapkan", "Expected parameter name")).text);
        paramTypes.push_back(match(TokenType::Colon)
                                  ? expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'")).text
                                  : "");
        while (match(TokenType::Comma)) {
            params.push_back(expect(TokenType::Ident, i18n::tr("Nama parameter diharapkan", "Expected parameter name")).text);
            paramTypes.push_back(match(TokenType::Colon)
                                      ? expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'")).text
                                      : "");
        }
    }
    expect(TokenType::RParen, i18n::tr("')' diharapkan setelah parameter", "Expected ')' after parameters"));
    std::string returnType;
    if (match(TokenType::Colon)) {
        returnType = expect(TokenType::Ident, i18n::tr("Tipe kembalian diharapkan setelah ':'", "Expected return type after ':'")).text;
    }
    auto body = block();
    return std::make_unique<FnDeclStmt>(std::move(name), std::move(params), std::move(body),
                                         std::move(paramTypes), std::move(returnType));
}

StmtPtr Parser::classDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama kelas diharapkan", "Expected class name")).text;
    std::string parentName;
    if (match(TokenType::Extends)) {
        parentName = expect(TokenType::Ident, i18n::tr("Nama kelas induk diharapkan", "Expected parent class name")).text;
    }
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan setelah nama kelas", "Expected '{' after class name"));
    std::vector<std::unique_ptr<FnDeclStmt>> methods;
    while (!check(TokenType::RBrace) && !atEnd()) {
        expect(TokenType::Fn, i18n::tr("Cuma deklarasi fungsi yang boleh di dalam kelas", "Only function declarations are allowed inside a class"));
        StmtPtr m = fnDecl();
        methods.push_back(std::unique_ptr<FnDeclStmt>(static_cast<FnDeclStmt*>(m.release())));
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan setelah isi kelas", "Expected '}' after class body"));
    return std::make_unique<ClassDeclStmt>(std::move(name), std::move(parentName), std::move(methods));
}

StmtPtr Parser::structDecl() {
    std::string name = expect(TokenType::Ident, i18n::tr("Nama bentuk diharapkan", "Expected struct name")).text;
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan setelah nama bentuk", "Expected '{' after struct name"));
    std::vector<std::string> fields;
    auto readField = [&]() {
        fields.push_back(expect(TokenType::Ident, i18n::tr("Nama field diharapkan", "Expected field name")).text);
        if (match(TokenType::Colon)) {
            expect(TokenType::Ident, i18n::tr("Nama tipe diharapkan setelah ':'", "Expected type name after ':'"));
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
    auto tryBlock = block();
    expect(TokenType::Catch, i18n::tr("'tangkap' diharapkan setelah blok 'coba'", "Expected 'tangkap' after 'coba' block"));
    expect(TokenType::LParen, i18n::tr("'(' diharapkan setelah 'tangkap'", "Expected '(' after 'tangkap'"));
    std::string catchVar = expect(TokenType::Ident, i18n::tr("Nama variabel diharapkan di 'tangkap'", "Expected variable name in 'tangkap'")).text;
    expect(TokenType::RParen, i18n::tr("')' diharapkan setelah variabel 'tangkap'", "Expected ')' after 'tangkap' variable"));
    auto catchBlock = block();
    std::unique_ptr<BlockStmt> finallyBlock;
    if (match(TokenType::Finally)) {
        finallyBlock = block();
    }
    return std::make_unique<TryStmt>(std::move(tryBlock), std::move(catchVar), std::move(catchBlock), std::move(finallyBlock));
}

StmtPtr Parser::throwStmt() {
    ExprPtr value = expression();
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah 'lempar'", "Expected ';' after 'lempar'"));
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

StmtPtr Parser::forStmt() {
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
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah pernyataan 'hasil'", "Expected ';' after return statement"));
    return std::make_unique<ReturnStmt>(std::move(value));
}

StmtPtr Parser::breakStmt() {
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah 'berhenti'", "Expected ';' after 'berhenti'"));
    return std::make_unique<BreakStmt>();
}

StmtPtr Parser::continueStmt() {
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah 'lanjut'", "Expected ';' after 'lanjut'"));
    return std::make_unique<ContinueStmt>();
}

std::unique_ptr<BlockStmt> Parser::block() {
    expect(TokenType::LBrace, i18n::tr("'{' diharapkan", "Expected '{'"));
    std::vector<StmtPtr> statements;
    while (!check(TokenType::RBrace) && !atEnd()) {
        statements.push_back(statement());
    }
    expect(TokenType::RBrace, i18n::tr("'}' diharapkan", "Expected '}'"));
    return std::make_unique<BlockStmt>(std::move(statements));
}

StmtPtr Parser::exprStmt() {
    ExprPtr expr = expression();
    expect(TokenType::Semi, i18n::tr("';' diharapkan setelah ekspresi", "Expected ';' after expression"));
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

// Every function below stamps `start` onto whichever new node it
// builds; an unchanged pass-through keeps the inner call's span, so
// span precision only ever improves going deeper, never regresses.

ExprPtr Parser::assignment() {
    Span start = peek().span;
    ExprPtr expr = logicOr();

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
        default: break;
    }
    if (!compoundOp.empty()) {
        if (expr->kind != ExprKind::Identifier) {
            throw ParseError(
                i18n::tr("Compound assignment (+=/-=/*=//=) baru didukung buat variabel biasa, "
                          "belum buat larik/peta -- tulis 'x[i] = x[i] + ...' manual",
                          "Compound assignment (+=/-=/*=//=) is only supported for plain "
                          "variables, not array/map yet -- write 'x[i] = x[i] + ...' manually"),
                peek());
        }
        advance();
        std::string name = static_cast<IdentifierExpr*>(expr.get())->name;
        ExprPtr rhs = assignment();
        ExprPtr current = std::make_unique<IdentifierExpr>(name);
        current->span = start;
        ExprPtr combined = std::make_unique<BinaryExpr>(compoundOp, std::move(current), std::move(rhs));
        combined->span = start;
        ExprPtr result = std::make_unique<AssignExpr>(std::move(name), std::move(combined));
        result->span = start;
        return result;
    }

    return expr;
}

ExprPtr Parser::logicOr() {
    Span start = peek().span;
    ExprPtr expr = logicAnd();
    while (match(TokenType::Or)) {
        ExprPtr right = logicAnd();
        expr = std::make_unique<BinaryExpr>("||", std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

ExprPtr Parser::logicAnd() {
    Span start = peek().span;
    ExprPtr expr = equality();
    while (match(TokenType::And)) {
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

ExprPtr Parser::comparison() {
    Span start = peek().span;
    ExprPtr expr = term();
    while (check(TokenType::Lt) || check(TokenType::Lte) || check(TokenType::Gt) || check(TokenType::Gte)) {
        std::string op = advance().text;
        ExprPtr right = term();
        expr = std::make_unique<BinaryExpr>(std::move(op), std::move(expr), std::move(right));
        expr->span = start;
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
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        std::string op = advance().text;
        ExprPtr right = unary();
        expr = std::make_unique<BinaryExpr>(std::move(op), std::move(expr), std::move(right));
        expr->span = start;
    }
    return expr;
}

ExprPtr Parser::unary() {
    Span start = peek().span;
    if (check(TokenType::Minus) || check(TokenType::Not)) {
        std::string op = advance().text;
        ExprPtr operand = unary();
        ExprPtr result = std::make_unique<UnaryExpr>(std::move(op), std::move(operand));
        result->span = start;
        return result;
    }
    return call();
}

ExprPtr Parser::call() {
    Span start = peek().span;
    ExprPtr expr = primary();
    while (true) {
        if (check(TokenType::LParen)) {
            advance();
            std::vector<ExprPtr> args;
            if (!check(TokenType::RParen)) {
                args.push_back(expression());
                while (match(TokenType::Comma)) {
                    args.push_back(expression());
                }
            }
            expect(TokenType::RParen, i18n::tr("')' diharapkan setelah argumen", "Expected ')' after arguments"));
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
            expr->span = start;
        } else if (check(TokenType::LBracket)) {
            advance();
            ExprPtr index = expression();
            expect(TokenType::RBracket, i18n::tr("']' diharapkan setelah index", "Expected ']' after index"));
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
            expr->span = start;
        } else if (check(TokenType::Dot)) {
            // `obj.field` is sugar for `obj["field"]` -- same IndexExpr
            // node, so reads, writes (via assignment()) and map/module
            // lookups all just work without any extra interpreter code.
            advance();
            std::string field;
            if (!peek().text.empty() && peek().type != TokenType::Semi && peek().type != TokenType::RParen && peek().type != TokenType::RBrace && peek().type != TokenType::RBracket) {
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
            advance();
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
            ExprPtr e = std::make_unique<IdentifierExpr>("induk");
            e->span = start;
            return e;
        }
        case TokenType::LBracket: {
            advance();
            std::vector<ExprPtr> elements;
            if (!check(TokenType::RBracket)) {
                elements.push_back(expression());
                while (match(TokenType::Comma)) {
                    elements.push_back(expression());
                }
            }
            expect(TokenType::RBracket, i18n::tr("']' diharapkan setelah elemen larik", "Expected ']' after array elements"));
            ExprPtr e = std::make_unique<ArrayLitExpr>(std::move(elements));
            e->span = start;
            return e;
        }
        case TokenType::LParen: {
            advance();
            ExprPtr expr = expression();
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
