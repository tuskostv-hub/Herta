// Парсер: строит AST из потока токенов. Рекурсивный спуск по грамматике.
// Падает на первой же ошибке, восстановления нет.

export module herta.parser;

import std;
import herta.common;
import herta.lexer;
import herta.ast;

namespace herta::parser {

using herta::common::Diagnostic;
using herta::common::DiagnosticSink;
using herta::common::SourceLocation;
using herta::lexer::Token;
using herta::lexer::TokenKind;

namespace ast = herta::ast;

export class Parser {
public:
    Parser(std::span<const Token> tokens,
           std::string_view filename,
           DiagnosticSink& sink);

    // На ошибке возвращает std::unexpected{}, подробности уходят в sink.
    std::expected<ast::Program, std::monostate> parse_program();

private:
    const Token& peek(std::size_t offset = 0) const noexcept;
    const Token& current() const noexcept { return peek(0); }
    bool check(TokenKind k) const noexcept { return current().kind == k; }
    bool match(TokenKind k) noexcept;
    const Token& advance() noexcept;
    bool expect(TokenKind k, std::string_view what);  // ругается, если не совпало

    void error_at(const Token& t, std::string msg);
    void error(std::string msg) { error_at(current(), std::move(msg)); }

    bool parse_module_header(ast::Program& prog);
    bool parse_imports(ast::Program& prog);
    std::unique_ptr<ast::Decl> parse_top_level();
    std::unique_ptr<ast::FnDecl> parse_fn_decl();
    std::unique_ptr<ast::StructDecl> parse_struct_decl();
    std::unique_ptr<ast::TypeAliasDecl> parse_type_alias_decl();
    std::unique_ptr<ast::NamespaceDecl> parse_namespace_decl();
    std::unique_ptr<ast::ImplDecl> parse_impl_decl();

    std::unique_ptr<ast::TypeExpr> parse_type_expr();

    std::unique_ptr<ast::BlockStmt> parse_block();
    std::unique_ptr<ast::Stmt> parse_stmt();
    std::unique_ptr<ast::Stmt> parse_var_decl_stmt(bool is_mutable);
    std::unique_ptr<ast::Stmt> parse_return_stmt();
    std::unique_ptr<ast::Stmt> parse_if_stmt();
    std::unique_ptr<ast::Stmt> parse_while_stmt();
    std::unique_ptr<ast::Stmt> parse_assign_or_expr_stmt();

    std::unique_ptr<ast::Expr> parse_expr();
    std::unique_ptr<ast::Expr> parse_or_expr();
    std::unique_ptr<ast::Expr> parse_and_expr();
    std::unique_ptr<ast::Expr> parse_eq_expr();
    std::unique_ptr<ast::Expr> parse_rel_expr();
    std::unique_ptr<ast::Expr> parse_add_expr();
    std::unique_ptr<ast::Expr> parse_mul_expr();
    std::unique_ptr<ast::Expr> parse_unary_expr();
    std::unique_ptr<ast::Expr> parse_postfix_expr();
    std::unique_ptr<ast::Expr> parse_primary_expr();

    std::unique_ptr<ast::Expr> parse_array_lit(SourceLocation start);
    std::unique_ptr<ast::Expr> parse_struct_lit(std::string type_name,
                                                SourceLocation start);

    std::optional<std::int64_t> parse_int_lexeme(std::string_view lex,
                                                 SourceLocation loc);
    std::optional<double> parse_float_lexeme(std::string_view lex,
                                             SourceLocation loc);
    std::optional<std::string> parse_string_lexeme(std::string_view lex,
                                                   SourceLocation loc);
    std::optional<std::uint32_t> parse_char_lexeme(std::string_view lex,
                                                   SourceLocation loc);

    std::span<const Token> tokens_;
    std::string filename_;
    DiagnosticSink& sink_;
    std::size_t pos_ = 0;
    bool fatal_ = false;
    bool allow_struct_lit_ = true;  // выключается в условиях if/while, чтобы не путать с блоком
    Token sentinel_{TokenKind::Eof, {}, {}};
};

// Implementation: общие хелперы

Parser::Parser(std::span<const Token> tokens,
               std::string_view filename,
               DiagnosticSink& sink)
    : tokens_(tokens), filename_(filename), sink_(sink) {}

const Token& Parser::peek(std::size_t offset) const noexcept {
    auto idx = pos_ + offset;
    return idx < tokens_.size() ? tokens_[idx] : sentinel_;
}

bool Parser::match(TokenKind k) noexcept {
    if (current().kind != k) return false;
    advance();
    return true;
}

const Token& Parser::advance() noexcept {
    const Token& t = current();
    if (pos_ < tokens_.size()) ++pos_;
    return t;
}

void Parser::error_at(const Token& t, std::string msg) {
    sink_.report(Diagnostic{
        .file = filename_,
        .loc = t.loc,
        .message = std::move(msg),
    });
    fatal_ = true;
}

bool Parser::expect(TokenKind k, std::string_view what) {
    if (current().kind == k) {
        advance();
        return true;
    }
    error(std::format("expected {}, got {} '{}'",
                      what,
                      herta::lexer::to_string(current().kind),
                      current().lexeme));
    return false;
}

// Implementation: литералы

std::optional<std::int64_t> Parser::parse_int_lexeme(std::string_view lex,
                                                     SourceLocation loc) {
    int base = 10;
    if (lex.size() >= 2 && lex[0] == '0' && (lex[1] == 'x' || lex[1] == 'X')) {
        base = 16;
        lex.remove_prefix(2);
    } else if (lex.size() >= 2 && lex[0] == '0' && (lex[1] == 'b' || lex[1] == 'B')) {
        base = 2;
        lex.remove_prefix(2);
    }
    std::int64_t value = 0;
    auto [ptr, ec] = std::from_chars(lex.data(), lex.data() + lex.size(),
                                     value, base);
    if (ec != std::errc{} || ptr != lex.data() + lex.size()) {
        error_at(Token{TokenKind::Invalid, lex, loc},
                 std::format("invalid integer literal '{}'", lex));
        return std::nullopt;
    }
    return value;
}

std::optional<double> Parser::parse_float_lexeme(std::string_view lex,
                                                 SourceLocation loc) {
    double value = 0.0;
    auto [ptr, ec] = std::from_chars(lex.data(), lex.data() + lex.size(),
                                     value);
    if (ec != std::errc{} || ptr != lex.data() + lex.size()) {
        error_at(Token{TokenKind::Invalid, lex, loc},
                 std::format("invalid float literal '{}'", lex));
        return std::nullopt;
    }
    return value;
}

// lex приходит целиком, вместе с одинарными кавычками.
std::optional<std::uint32_t> Parser::parse_char_lexeme(std::string_view lex,
                                                       SourceLocation loc) {
    if (lex.size() < 3 || lex.front() != '\'' || lex.back() != '\'') {
        error_at(Token{TokenKind::Invalid, lex, loc}, "malformed char literal");
        return std::nullopt;
    }
    auto inner = lex.substr(1, lex.size() - 2);
    if (inner.empty()) {
        error_at(Token{TokenKind::Invalid, lex, loc}, "empty char literal");
        return std::nullopt;
    }
    if (inner[0] == '\\') {
        if (inner.size() != 2) {
            error_at(Token{TokenKind::Invalid, lex, loc}, "malformed char escape");
            return std::nullopt;
        }
        switch (inner[1]) {
            case '\'': return static_cast<std::uint32_t>('\'');
            case '\\': return static_cast<std::uint32_t>('\\');
            case 'n':  return static_cast<std::uint32_t>('\n');
            case 't':  return static_cast<std::uint32_t>('\t');
            case 'r':  return static_cast<std::uint32_t>('\r');
            case '0':  return 0u;
            default:
                error_at(Token{TokenKind::Invalid, lex, loc},
                         std::format("invalid char escape: \\{}", inner[1]));
                return std::nullopt;
        }
    }
    // UTF-8 декодирование: один codepoint из 1–4 байт.
    auto b0 = static_cast<unsigned char>(inner[0]);
    int n;
    std::uint32_t cp;
    if (b0 < 0x80) {
        if (inner.size() != 1) {
            error_at(Token{TokenKind::Invalid, lex, loc}, "malformed char literal");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(b0);
    }
    if ((b0 & 0xE0) == 0xC0)      { n = 2; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { n = 3; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { n = 4; cp = b0 & 0x07u; }
    else {
        error_at(Token{TokenKind::Invalid, lex, loc}, "invalid UTF-8 in char literal");
        return std::nullopt;
    }
    if (inner.size() != static_cast<std::size_t>(n)) {
        error_at(Token{TokenKind::Invalid, lex, loc},
                 "char literal: expected UTF-8 of length matching lead byte");
        return std::nullopt;
    }
    for (std::size_t i = 1; i < static_cast<std::size_t>(n); ++i) {
        auto b = static_cast<unsigned char>(inner[i]);
        if ((b & 0xC0) != 0x80) {
            error_at(Token{TokenKind::Invalid, lex, loc},
                     "invalid UTF-8 continuation byte in char literal");
            return std::nullopt;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    return cp;
}

// lex приходит вместе с двойными кавычками.
std::optional<std::string> Parser::parse_string_lexeme(std::string_view lex,
                                                       SourceLocation loc) {
    if (lex.size() < 2 || lex.front() != '"' || lex.back() != '"') {
        error_at(Token{TokenKind::Invalid, lex, loc}, "malformed string literal");
        return std::nullopt;
    }
    std::string out;
    out.reserve(lex.size() - 2);
    for (std::size_t i = 1; i + 1 < lex.size(); ++i) {
        char c = lex[i];
        if (c == '\\') {
            ++i;
            switch (lex[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                default:
                    // лексер должен был это поймать, но на всякий случай
                    error_at(Token{TokenKind::Invalid, lex, loc},
                             std::format("invalid escape: \\{}", lex[i]));
                    return std::nullopt;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Implementation: верхний уровень

std::expected<ast::Program, std::monostate> Parser::parse_program() {
    ast::Program prog;
    if (!parse_module_header(prog)) return std::unexpected{std::monostate{}};
    if (!parse_imports(prog)) return std::unexpected{std::monostate{}};

    while (!check(TokenKind::Eof)) {
        auto d = parse_top_level();
        if (!d) return std::unexpected{std::monostate{}};
        prog.decls.push_back(std::move(d));
    }
    return prog;
}

bool Parser::parse_module_header(ast::Program& prog) {
    if (!check(TokenKind::KwModule)) {
        error("expected 'module' declaration at start of file");
        return false;
    }
    prog.module_loc = current().loc;
    advance();  // 'module'

    if (!check(TokenKind::Identifier)) {
        error("expected module name after 'module'");
        return false;
    }
    prog.module_name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::Semicolon, "';' after module declaration")) {
        return false;
    }
    return true;
}

bool Parser::parse_imports(ast::Program& prog) {
    while (check(TokenKind::KwImport)) {
        advance();  // 'import'
        if (!check(TokenKind::Identifier)) {
            error("expected module name after 'import'");
            return false;
        }
        prog.imports.push_back(std::string(current().lexeme));
        advance();
        if (!expect(TokenKind::Semicolon, "';' after import declaration")) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<ast::Decl> Parser::parse_top_level() {
    bool is_pub = false;
    SourceLocation pub_loc{};
    if (check(TokenKind::KwPub)) {
        is_pub = true;
        pub_loc = current().loc;
        advance();
    }

    std::unique_ptr<ast::Decl> d;
    switch (current().kind) {
        case TokenKind::KwFn:        d = parse_fn_decl(); break;
        case TokenKind::KwStruct:    d = parse_struct_decl(); break;
        case TokenKind::KwType:      d = parse_type_alias_decl(); break;
        case TokenKind::KwNamespace: d = parse_namespace_decl(); break;
        case TokenKind::KwImpl:
            if (is_pub) {
                error_at(Token{TokenKind::KwPub, {}, pub_loc},
                         "'pub' cannot be applied to impl block "
                         "(use 'pub' on individual methods instead)");
                return nullptr;
            }
            d = parse_impl_decl();
            break;
        case TokenKind::KwModule:
            error("unexpected 'module' — only one 'module' declaration per file");
            return nullptr;
        case TokenKind::KwImport:
            error("unexpected 'import' — all imports must precede declarations");
            return nullptr;
        default:
            error(std::format("expected declaration, got {} '{}'",
                              herta::lexer::to_string(current().kind),
                              current().lexeme));
            return nullptr;
    }
    if (d) d->is_pub = is_pub;
    return d;
}

std::unique_ptr<ast::FnDecl> Parser::parse_fn_decl() {
    auto start = current().loc;
    advance();  // 'fn'

    if (!check(TokenKind::Identifier)) {
        error("expected function name after 'fn'");
        return nullptr;
    }
    auto name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::LParen, "'(' after function name")) return nullptr;

    std::vector<ast::Param> params;
    if (!check(TokenKind::RParen)) {
        while (true) {
            if (!check(TokenKind::Identifier)) {
                error("expected parameter name");
                return nullptr;
            }
            ast::Param p;
            p.loc = current().loc;
            p.name = std::string(current().lexeme);
            advance();
            if (!expect(TokenKind::Colon, "':' after parameter name")) return nullptr;
            p.type = parse_type_expr();
            if (!p.type) return nullptr;
            params.push_back(std::move(p));
            if (!match(TokenKind::Comma)) break;
            if (check(TokenKind::RParen)) break;  // trailing comma
        }
    }
    if (!expect(TokenKind::RParen, "')' after parameter list")) return nullptr;

    auto ret_type = parse_type_expr();
    if (!ret_type) return nullptr;

    auto body = parse_block();
    if (!body) return nullptr;

    auto fn = std::make_unique<ast::FnDecl>();
    fn->loc = start;
    fn->name = std::move(name);
    fn->params = std::move(params);
    fn->return_type = std::move(ret_type);
    fn->body = std::move(body);
    return fn;
}

std::unique_ptr<ast::StructDecl> Parser::parse_struct_decl() {
    auto start = current().loc;
    advance();  // 'struct'

    if (!check(TokenKind::Identifier)) {
        error("expected struct name after 'struct'");
        return nullptr;
    }
    auto name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::LBrace, "'{' after struct name")) return nullptr;

    std::vector<ast::StructField> fields;
    if (!check(TokenKind::RBrace)) {
        while (true) {
            if (!check(TokenKind::Identifier)) {
                error("expected field name");
                return nullptr;
            }
            ast::StructField f;
            f.loc = current().loc;
            f.name = std::string(current().lexeme);
            advance();
            if (!expect(TokenKind::Colon, "':' after field name")) return nullptr;
            f.type = parse_type_expr();
            if (!f.type) return nullptr;
            fields.push_back(std::move(f));
            if (!match(TokenKind::Comma)) break;
            if (check(TokenKind::RBrace)) break;  // trailing comma
        }
    }
    if (!expect(TokenKind::RBrace, "'}' to close struct body")) return nullptr;

    auto s = std::make_unique<ast::StructDecl>();
    s->loc = start;
    s->name = std::move(name);
    s->fields = std::move(fields);
    return s;
}

std::unique_ptr<ast::TypeAliasDecl> Parser::parse_type_alias_decl() {
    auto start = current().loc;
    advance();  // 'type'

    if (!check(TokenKind::Identifier)) {
        error("expected type name after 'type'");
        return nullptr;
    }
    auto name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::Eq, "'=' in type alias")) return nullptr;
    auto target = parse_type_expr();
    if (!target) return nullptr;
    if (!expect(TokenKind::Semicolon, "';' after type alias")) return nullptr;

    auto a = std::make_unique<ast::TypeAliasDecl>();
    a->loc = start;
    a->name = std::move(name);
    a->target = std::move(target);
    return a;
}

std::unique_ptr<ast::NamespaceDecl> Parser::parse_namespace_decl() {
    auto start = current().loc;
    advance();  // 'namespace'

    if (!check(TokenKind::Identifier)) {
        error("expected namespace name after 'namespace'");
        return nullptr;
    }
    auto name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::LBrace, "'{' after namespace name")) return nullptr;

    std::vector<std::unique_ptr<ast::Decl>> members;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        auto d = parse_top_level();
        if (!d) return nullptr;
        members.push_back(std::move(d));
    }
    if (!expect(TokenKind::RBrace, "'}' to close namespace")) return nullptr;

    auto ns = std::make_unique<ast::NamespaceDecl>();
    ns->loc = start;
    ns->name = std::move(name);
    ns->members = std::move(members);
    return ns;
}

std::unique_ptr<ast::ImplDecl> Parser::parse_impl_decl() {
    auto start = current().loc;
    advance();  // 'impl'

    if (!check(TokenKind::Identifier)) {
        error("expected type name after 'impl'");
        return nullptr;
    }
    auto type_name = std::string(current().lexeme);
    advance();

    if (!expect(TokenKind::LBrace, "'{' after impl type")) return nullptr;

    std::vector<std::unique_ptr<ast::FnDecl>> methods;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        bool is_pub = false;
        if (check(TokenKind::KwPub)) {
            is_pub = true;
            advance();
        }
        if (!check(TokenKind::KwFn)) {
            error("expected 'fn' inside impl block");
            return nullptr;
        }
        auto fn = parse_fn_decl();
        if (!fn) return nullptr;
        fn->is_pub = is_pub;
        methods.push_back(std::move(fn));
    }
    if (!expect(TokenKind::RBrace, "'}' to close impl")) return nullptr;

    auto im = std::make_unique<ast::ImplDecl>();
    im->loc = start;
    im->type_name = std::move(type_name);
    im->methods = std::move(methods);
    return im;
}

// Implementation: типы

std::unique_ptr<ast::TypeExpr> Parser::parse_type_expr() {
    auto start = current().loc;

    // *T — указатель
    if (match(TokenKind::Star)) {
        auto pointee = parse_type_expr();
        if (!pointee) return nullptr;
        auto p = std::make_unique<ast::PointerType>();
        p->loc = start;
        p->pointee = std::move(pointee);
        return p;
    }

    // fn(T1, T2) R — указатель на функцию
    if (match(TokenKind::KwFn)) {
        if (!expect(TokenKind::LParen, "'(' after 'fn' in function pointer type"))
            return nullptr;
        std::vector<std::unique_ptr<ast::TypeExpr>> params;
        if (!check(TokenKind::RParen)) {
            while (true) {
                auto pt = parse_type_expr();
                if (!pt) return nullptr;
                params.push_back(std::move(pt));
                if (!match(TokenKind::Comma)) break;
                if (check(TokenKind::RParen)) break;
            }
        }
        if (!expect(TokenKind::RParen, "')' in function pointer type")) return nullptr;
        auto ret = parse_type_expr();
        if (!ret) return nullptr;
        auto fp = std::make_unique<ast::FnPointerType>();
        fp->loc = start;
        fp->params = std::move(params);
        fp->return_type = std::move(ret);
        return fp;
    }

    if (match(TokenKind::LBracket)) {
        // [T; N]
        auto elem = parse_type_expr();
        if (!elem) return nullptr;
        if (!expect(TokenKind::Semicolon, "';' in array type")) return nullptr;
        if (!check(TokenKind::IntLiteral)) {
            error("expected integer literal as array size");
            return nullptr;
        }
        auto size_lex = current().lexeme;
        auto size_loc = current().loc;
        advance();
        auto val = parse_int_lexeme(size_lex, size_loc);
        if (!val) return nullptr;
        if (*val < 0) {
            error_at(Token{TokenKind::IntLiteral, size_lex, size_loc},
                     "array size must be non-negative");
            return nullptr;
        }
        if (!expect(TokenKind::RBracket, "']' to close array type")) return nullptr;
        auto a = std::make_unique<ast::ArrayType>();
        a->loc = start;
        a->element = std::move(elem);
        a->size = *val;
        return a;
    }

    if (!check(TokenKind::Identifier)) {
        error("expected type name");
        return nullptr;
    }
    auto n = std::make_unique<ast::NamedType>();
    n->loc = start;
    n->name = std::string(current().lexeme);
    advance();
    return n;
}

// Implementation: инструкции

std::unique_ptr<ast::BlockStmt> Parser::parse_block() {
    auto start = current().loc;
    if (!expect(TokenKind::LBrace, "'{' to start block")) return nullptr;
    auto b = std::make_unique<ast::BlockStmt>();
    b->loc = start;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        auto s = parse_stmt();
        if (!s) return nullptr;
        b->stmts.push_back(std::move(s));
    }
    if (!expect(TokenKind::RBrace, "'}' to close block")) return nullptr;
    return b;
}

std::unique_ptr<ast::Stmt> Parser::parse_stmt() {
    switch (current().kind) {
        case TokenKind::KwVar:    return parse_var_decl_stmt(/*is_mutable*/ true);
        case TokenKind::KwLet:    return parse_var_decl_stmt(/*is_mutable*/ false);
        case TokenKind::KwReturn: return parse_return_stmt();
        case TokenKind::KwIf:     return parse_if_stmt();
        case TokenKind::KwWhile:  return parse_while_stmt();
        case TokenKind::KwBreak: {
            auto loc = current().loc;
            advance();
            if (!expect(TokenKind::Semicolon, "';' after 'break'")) return nullptr;
            auto s = std::make_unique<ast::BreakStmt>();
            s->loc = loc;
            return s;
        }
        case TokenKind::KwContinue: {
            auto loc = current().loc;
            advance();
            if (!expect(TokenKind::Semicolon, "';' after 'continue'")) return nullptr;
            auto s = std::make_unique<ast::ContinueStmt>();
            s->loc = loc;
            return s;
        }
        case TokenKind::LBrace: return parse_block();
        case TokenKind::Semicolon: {
            auto loc = current().loc;
            advance();
            auto s = std::make_unique<ast::EmptyStmt>();
            s->loc = loc;
            return s;
        }
        default: return parse_assign_or_expr_stmt();
    }
}

std::unique_ptr<ast::Stmt> Parser::parse_var_decl_stmt(bool is_mutable) {
    auto start = current().loc;
    advance();  // 'var' / 'let'

    if (!check(TokenKind::Identifier)) {
        error("expected variable name");
        return nullptr;
    }
    auto name = std::string(current().lexeme);
    advance();

    std::unique_ptr<ast::TypeExpr> type;
    if (match(TokenKind::Colon)) {
        type = parse_type_expr();
        if (!type) return nullptr;
        if (!expect(TokenKind::Eq, "'=' in variable declaration")) return nullptr;
    } else if (match(TokenKind::ColonEq)) {
        // тип будет выведен из инициализатора
    } else {
        error("expected ':' (with type) or ':=' (type inference)");
        return nullptr;
    }

    auto init = parse_expr();
    if (!init) return nullptr;
    if (!expect(TokenKind::Semicolon, "';' after variable declaration")) {
        return nullptr;
    }

    auto v = std::make_unique<ast::VarDeclStmt>();
    v->loc = start;
    v->is_mutable = is_mutable;
    v->name = std::move(name);
    v->type = std::move(type);
    v->init = std::move(init);
    return v;
}

std::unique_ptr<ast::Stmt> Parser::parse_return_stmt() {
    auto start = current().loc;
    advance();  // 'return'

    auto r = std::make_unique<ast::ReturnStmt>();
    r->loc = start;
    if (!check(TokenKind::Semicolon)) {
        r->value = parse_expr();
        if (!r->value) return nullptr;
    }
    if (!expect(TokenKind::Semicolon, "';' after return")) return nullptr;
    return r;
}

std::unique_ptr<ast::Stmt> Parser::parse_if_stmt() {
    auto start = current().loc;
    advance();  // 'if'

    // В условии запрещаем struct-литералы, чтобы не путались с блоком (как в Go)
    bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    auto cond = parse_expr();
    allow_struct_lit_ = saved;
    if (!cond) return nullptr;

    auto then_b = parse_block();
    if (!then_b) return nullptr;

    std::unique_ptr<ast::Stmt> else_b;
    if (match(TokenKind::KwElse)) {
        if (check(TokenKind::KwIf)) {
            else_b = parse_if_stmt();
        } else {
            else_b = parse_block();
        }
        if (!else_b) return nullptr;
    }

    auto s = std::make_unique<ast::IfStmt>();
    s->loc = start;
    s->cond = std::move(cond);
    s->then_branch = std::move(then_b);
    s->else_branch = std::move(else_b);
    return s;
}

std::unique_ptr<ast::Stmt> Parser::parse_while_stmt() {
    auto start = current().loc;
    advance();  // 'while'

    bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    auto cond = parse_expr();
    allow_struct_lit_ = saved;
    if (!cond) return nullptr;

    auto body = parse_block();
    if (!body) return nullptr;

    auto s = std::make_unique<ast::WhileStmt>();
    s->loc = start;
    s->cond = std::move(cond);
    s->body = std::move(body);
    return s;
}

namespace {
// Проверяет, что выражение — допустимый lvalue: Ident, Field, Index или Deref.
bool is_lvalue(const ast::Expr& e) {
    return dynamic_cast<const ast::IdentExpr*>(&e) != nullptr
        || dynamic_cast<const ast::FieldExpr*>(&e) != nullptr
        || dynamic_cast<const ast::IndexExpr*>(&e) != nullptr
        || dynamic_cast<const ast::DerefExpr*>(&e) != nullptr;  // *p = ... тоже сюда
}
}  // anonymous namespace

std::unique_ptr<ast::Stmt> Parser::parse_assign_or_expr_stmt() {
    auto start = current().loc;
    auto e = parse_expr();
    if (!e) return nullptr;

    if (check(TokenKind::Eq)) {
        // assign
        if (!is_lvalue(*e)) {
            error_at(Token{TokenKind::Eq, "=", current().loc},
                     "left-hand side of '=' must be an lvalue "
                     "(identifier, field access, or array index)");
            return nullptr;
        }
        advance();  // '='
        auto rhs = parse_expr();
        if (!rhs) return nullptr;
        if (!expect(TokenKind::Semicolon, "';' after assignment")) return nullptr;
        auto a = std::make_unique<ast::AssignStmt>();
        a->loc = start;
        a->target = std::move(e);
        a->value = std::move(rhs);
        return a;
    }

    if (!expect(TokenKind::Semicolon, "';' after expression statement")) {
        return nullptr;
    }
    auto s = std::make_unique<ast::ExprStmt>();
    s->loc = start;
    s->expr = std::move(e);
    return s;
}

// Implementation: выражения (precedence climbing)

std::unique_ptr<ast::Expr> Parser::parse_expr() {
    return parse_or_expr();
}

std::unique_ptr<ast::Expr> Parser::parse_or_expr() {
    auto lhs = parse_and_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::PipePipe)) {
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_and_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc;
        b->op = ast::BinaryOp::Or;
        b->lhs = std::move(lhs);
        b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_and_expr() {
    auto lhs = parse_eq_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::AmpAmp)) {
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_eq_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc;
        b->op = ast::BinaryOp::And;
        b->lhs = std::move(lhs);
        b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_eq_expr() {
    auto lhs = parse_rel_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::EqEq) || check(TokenKind::BangEq)) {
        auto op = check(TokenKind::EqEq) ? ast::BinaryOp::Eq : ast::BinaryOp::NotEq;
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_rel_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc; b->op = op;
        b->lhs = std::move(lhs); b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_rel_expr() {
    auto lhs = parse_add_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::Lt) || check(TokenKind::Gt)
           || check(TokenKind::LtEq) || check(TokenKind::GtEq)) {
        ast::BinaryOp op{};
        switch (current().kind) {
            case TokenKind::Lt:   op = ast::BinaryOp::Lt;   break;
            case TokenKind::Gt:   op = ast::BinaryOp::Gt;   break;
            case TokenKind::LtEq: op = ast::BinaryOp::LtEq; break;
            case TokenKind::GtEq: op = ast::BinaryOp::GtEq; break;
            default: break;
        }
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_add_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc; b->op = op;
        b->lhs = std::move(lhs); b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_add_expr() {
    auto lhs = parse_mul_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        auto op = check(TokenKind::Plus) ? ast::BinaryOp::Add : ast::BinaryOp::Sub;
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_mul_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc; b->op = op;
        b->lhs = std::move(lhs); b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_mul_expr() {
    auto lhs = parse_unary_expr();
    if (!lhs) return nullptr;
    while (check(TokenKind::Star) || check(TokenKind::Slash)
           || check(TokenKind::Percent)) {
        ast::BinaryOp op{};
        switch (current().kind) {
            case TokenKind::Star:    op = ast::BinaryOp::Mul; break;
            case TokenKind::Slash:   op = ast::BinaryOp::Div; break;
            case TokenKind::Percent: op = ast::BinaryOp::Mod; break;
            default: break;
        }
        auto op_loc = current().loc;
        advance();
        auto rhs = parse_unary_expr();
        if (!rhs) return nullptr;
        auto b = std::make_unique<ast::BinaryExpr>();
        b->loc = op_loc; b->op = op;
        b->lhs = std::move(lhs); b->rhs = std::move(rhs);
        lhs = std::move(b);
    }
    return lhs;
}

std::unique_ptr<ast::Expr> Parser::parse_unary_expr() {
    if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
        auto op = check(TokenKind::Minus) ? ast::UnaryOp::Neg : ast::UnaryOp::Not;
        auto op_loc = current().loc;
        advance();
        auto operand = parse_unary_expr();
        if (!operand) return nullptr;
        auto u = std::make_unique<ast::UnaryExpr>();
        u->loc = op_loc;
        u->op = op;
        u->operand = std::move(operand);
        return u;
    }
    // &expr — взятие адреса
    if (check(TokenKind::Amp)) {
        auto op_loc = current().loc;
        advance();
        auto operand = parse_unary_expr();
        if (!operand) return nullptr;
        auto a = std::make_unique<ast::AddressOfExpr>();
        a->loc = op_loc;
        a->operand = std::move(operand);
        return a;
    }
    // *expr — разыменование, это lvalue
    if (check(TokenKind::Star)) {
        auto op_loc = current().loc;
        advance();
        auto operand = parse_unary_expr();
        if (!operand) return nullptr;
        auto d = std::make_unique<ast::DerefExpr>();
        d->loc = op_loc;
        d->operand = std::move(operand);
        return d;
    }
    return parse_postfix_expr();
}

std::unique_ptr<ast::Expr> Parser::parse_postfix_expr() {
    auto e = parse_primary_expr();
    if (!e) return nullptr;
    while (true) {
        if (check(TokenKind::Dot)) {
            auto dot_loc = current().loc;
            advance();  // '.'
            if (!check(TokenKind::Identifier)) {
                error("expected field name after '.'");
                return nullptr;
            }
            auto name = std::string(current().lexeme);
            advance();
            auto f = std::make_unique<ast::FieldExpr>();
            f->loc = dot_loc;
            f->base = std::move(e);
            f->field = std::move(name);
            e = std::move(f);
        } else if (check(TokenKind::LBracket)) {
            auto bracket_loc = current().loc;
            advance();  // '['
            auto idx = parse_expr();
            if (!idx) return nullptr;
            if (!expect(TokenKind::RBracket, "']'")) return nullptr;
            auto ix = std::make_unique<ast::IndexExpr>();
            ix->loc = bracket_loc;
            ix->base = std::move(e);
            ix->index = std::move(idx);
            e = std::move(ix);
        } else if (check(TokenKind::LParen)) {
            auto call_loc = current().loc;
            advance();
            std::vector<std::unique_ptr<ast::Expr>> args;
            if (!check(TokenKind::RParen)) {
                while (true) {
                    auto a = parse_expr();
                    if (!a) return nullptr;
                    args.push_back(std::move(a));
                    if (!match(TokenKind::Comma)) break;
                    if (check(TokenKind::RParen)) break;  // trailing comma
                }
            }
            if (!expect(TokenKind::RParen, "')' to close call")) return nullptr;
            auto c = std::make_unique<ast::CallExpr>();
            c->loc = call_loc;
            c->callee = std::move(e);
            c->args = std::move(args);
            e = std::move(c);
        } else {
            return e;
        }
    }
}

std::unique_ptr<ast::Expr> Parser::parse_array_lit(SourceLocation start) {
    auto a = std::make_unique<ast::ArrayLit>();
    a->loc = start;
    if (!check(TokenKind::RBracket)) {
        while (true) {
            auto e = parse_expr();
            if (!e) return nullptr;
            a->elements.push_back(std::move(e));
            if (!match(TokenKind::Comma)) break;
            if (check(TokenKind::RBracket)) break;  // trailing comma
        }
    }
    if (!expect(TokenKind::RBracket, "']' to close array literal")) return nullptr;
    return a;
}

std::unique_ptr<ast::Expr> Parser::parse_struct_lit(std::string type_name,
                                                    SourceLocation start) {
    advance();  // '{'
    auto s = std::make_unique<ast::StructLit>();
    s->loc = start;
    s->type_name = std::move(type_name);
    if (!check(TokenKind::RBrace)) {
        while (true) {
            if (!check(TokenKind::Identifier)) {
                error("expected field name in struct literal");
                return nullptr;
            }
            ast::StructLitField f;
            f.loc = current().loc;
            f.name = std::string(current().lexeme);
            advance();
            if (!expect(TokenKind::Colon, "':' after field name")) return nullptr;
            f.value = parse_expr();
            if (!f.value) return nullptr;
            s->fields.push_back(std::move(f));
            if (!match(TokenKind::Comma)) break;
            if (check(TokenKind::RBrace)) break;  // trailing comma
        }
    }
    if (!expect(TokenKind::RBrace, "'}' to close struct literal")) return nullptr;
    return s;
}

std::unique_ptr<ast::Expr> Parser::parse_primary_expr() {
    auto loc = current().loc;
    switch (current().kind) {
        case TokenKind::IntLiteral: {
            auto lex = std::string(current().lexeme);
            advance();
            auto v = parse_int_lexeme(lex, loc);
            if (!v) return nullptr;
            auto n = std::make_unique<ast::IntLit>();
            n->loc = loc;
            n->value = *v;
            n->lexeme = std::move(lex);
            return n;
        }
        case TokenKind::FloatLiteral: {
            auto lex = current().lexeme;
            advance();
            auto v = parse_float_lexeme(lex, loc);
            if (!v) return nullptr;
            auto n = std::make_unique<ast::FloatLit>();
            n->loc = loc;
            n->value = *v;
            return n;
        }
        case TokenKind::KwTrue: {
            advance();
            auto n = std::make_unique<ast::BoolLit>();
            n->loc = loc;
            n->value = true;
            return n;
        }
        case TokenKind::KwFalse: {
            advance();
            auto n = std::make_unique<ast::BoolLit>();
            n->loc = loc;
            n->value = false;
            return n;
        }
        case TokenKind::KwNull: {
            advance();
            auto n = std::make_unique<ast::NullLit>();
            n->loc = loc;
            return n;
        }
        case TokenKind::StringLiteral: {
            auto lex = std::string(current().lexeme);
            advance();
            auto v = parse_string_lexeme(lex, loc);
            if (!v) return nullptr;
            auto n = std::make_unique<ast::StringLit>();
            n->loc = loc;
            n->value = std::move(*v);
            n->lexeme = std::move(lex);
            return n;
        }
        case TokenKind::CharLiteral: {
            auto lex = std::string(current().lexeme);
            advance();
            auto v = parse_char_lexeme(lex, loc);
            if (!v) return nullptr;
            auto n = std::make_unique<ast::CharLit>();
            n->loc = loc;
            n->value = *v;
            n->lexeme = std::move(lex);
            return n;
        }
        case TokenKind::LParen: {
            advance();
            auto e = parse_expr();
            if (!e) return nullptr;
            if (!expect(TokenKind::RParen, "')'")) return nullptr;
            return e;
        }
        case TokenKind::LBracket: {
            advance();
            return parse_array_lit(loc);
        }
        case TokenKind::Identifier: {
            auto name = std::string(current().lexeme);
            advance();
            // Возможно, это struct-литерал.
            if (allow_struct_lit_ && check(TokenKind::LBrace)) {
                // Смотрим вперёд: пустой {} или id : — это литерал, иначе просто идентификатор.
                bool is_struct_lit =
                    peek(1).kind == TokenKind::RBrace
                    || (peek(1).kind == TokenKind::Identifier
                        && peek(2).kind == TokenKind::Colon);
                if (is_struct_lit) {
                    return parse_struct_lit(std::move(name), loc);
                }
            }
            auto n = std::make_unique<ast::IdentExpr>();
            n->loc = loc;
            n->name = std::move(name);
            return n;
        }
        default:
            error(std::format("unexpected token in expression: {} '{}'",
                              herta::lexer::to_string(current().kind),
                              current().lexeme));
            return nullptr;
    }
}

}  // namespace herta::parser
