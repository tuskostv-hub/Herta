#include "herta/lexer/lexer.hpp"

import std;

namespace herta::lexer {

namespace {

using herta::common::SourceLocation;

constexpr bool is_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
constexpr bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
constexpr bool is_letter_or_digit(char c) noexcept {
    return is_letter(c) || is_digit(c);
}

// Таблица ключевых слов — порядок согласно grammar.md §2.1.
struct KeywordEntry {
    std::string_view text;
    TokenKind kind;
};

constexpr KeywordEntry kKeywords[] = {
    {"fn",        TokenKind::KwFn},
    {"let",       TokenKind::KwLet},
    {"var",       TokenKind::KwVar},
    {"return",    TokenKind::KwReturn},
    {"if",        TokenKind::KwIf},
    {"else",      TokenKind::KwElse},
    {"while",     TokenKind::KwWhile},
    {"break",     TokenKind::KwBreak},
    {"continue",  TokenKind::KwContinue},
    {"struct",    TokenKind::KwStruct},
    {"type",      TokenKind::KwType},
    {"namespace", TokenKind::KwNamespace},
    {"impl",      TokenKind::KwImpl},
    {"module",    TokenKind::KwModule},
    {"import",    TokenKind::KwImport},
    {"pub",       TokenKind::KwPub},
    {"true",      TokenKind::KwTrue},
    {"false",     TokenKind::KwFalse},
};

TokenKind keyword_lookup(std::string_view text) noexcept {
    for (const auto& e : kKeywords) {
        if (e.text == text) return e.kind;
    }
    return TokenKind::Identifier;
}

}  // namespace

Lexer::Lexer(const herta::common::SourceFile& src,
             herta::common::DiagnosticSink& sink)
    : src_(src), sink_(sink) {}

bool Lexer::at_end() const noexcept {
    return pos_ >= src_.size();
}

char Lexer::peek(std::size_t lookahead) const noexcept {
    auto idx = pos_ + lookahead;
    return idx < src_.size() ? src_.contents()[idx] : '\0';
}

char Lexer::advance() noexcept {
    char c = src_.contents()[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

bool Lexer::match(char c) noexcept {
    if (peek() != c) return false;
    advance();
    return true;
}

SourceLocation Lexer::current_loc() const noexcept {
    return SourceLocation{
        .line = line_,
        .column = col_,
        .offset = static_cast<std::uint32_t>(pos_),
    };
}

Token Lexer::make_token(TokenKind kind,
                        std::size_t start_pos,
                        SourceLocation start_loc) const {
    auto contents = src_.contents();
    return Token{
        .kind = kind,
        .lexeme = contents.substr(start_pos, pos_ - start_pos),
        .loc = start_loc,
    };
}

void Lexer::error(std::string message, SourceLocation loc) {
    sink_.report(herta::common::Diagnostic{
        .file = std::string(src_.name()),
        .loc = loc,
        .message = std::move(message),
    });
    fatal_ = true;
}

void Lexer::skip_whitespace_and_comments() {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            // Однострочный комментарий — поглощаем до '\n' или EOF.
            // Сам '\n' оставляем — он отработает на следующем шаге.
            while (!at_end() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::scan_identifier_or_keyword(SourceLocation start,
                                        std::size_t start_pos) {
    while (!at_end() && is_letter_or_digit(peek())) advance();
    auto lexeme = src_.contents().substr(start_pos, pos_ - start_pos);
    return Token{
        .kind = keyword_lookup(lexeme),
        .lexeme = lexeme,
        .loc = start,
    };
}

Token Lexer::scan_number(SourceLocation start, std::size_t start_pos) {
    // Шестнадцатеричный литерал: "0x" hex_digit+
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();  // '0'
        advance();  // 'x' / 'X'
        if (!is_hex_digit(peek())) {
            error("invalid hex literal: at least one hex digit required", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        while (!at_end() && is_hex_digit(peek())) advance();
        return make_token(TokenKind::IntLiteral, start_pos, start);
    }

    // Десятичная целая часть.
    while (!at_end() && is_digit(peek())) advance();

    // Float? Грамматика требует digit "." digit { digit } [ exponent ].
    // Значит "1." без цифры после точки — это IntLiteral "1" + Dot.
    if (peek() == '.' && is_digit(peek(1))) {
        advance();  // '.'
        while (!at_end() && is_digit(peek())) advance();

        // Опциональная экспонента: (e|E) [+|-] digit+
        if (peek() == 'e' || peek() == 'E') {
            auto exp_loc = current_loc();
            advance();  // 'e' / 'E'
            if (peek() == '+' || peek() == '-') advance();
            if (!is_digit(peek())) {
                error("invalid float literal: digit required after exponent",
                      exp_loc);
                return Token{TokenKind::Invalid, {}, start};
            }
            while (!at_end() && is_digit(peek())) advance();
        }
        return make_token(TokenKind::FloatLiteral, start_pos, start);
    }

    return make_token(TokenKind::IntLiteral, start_pos, start);
}

Token Lexer::scan_string(SourceLocation start, std::size_t start_pos) {
    advance();  // открывающая "
    while (true) {
        if (at_end()) {
            error("unterminated string literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        char c = peek();
        if (c == '\n') {
            error("unterminated string literal (newline inside)", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        if (c == '"') {
            advance();  // закрывающая "
            return make_token(TokenKind::StringLiteral, start_pos, start);
        }
        if (c == '\\') {
            auto esc_loc = current_loc();
            advance();  // '\\'
            if (at_end()) {
                error("unterminated string literal", start);
                return Token{TokenKind::Invalid, {}, start};
            }
            char e = peek();
            if (e == '"' || e == '\\' || e == 'n' || e == 't' || e == 'r') {
                advance();
            } else {
                error(std::string("invalid escape sequence: \\") + e, esc_loc);
                return Token{TokenKind::Invalid, {}, start};
            }
        } else {
            advance();
        }
    }
}

Token Lexer::scan_punct_or_op(SourceLocation start, std::size_t start_pos) {
    char c = advance();
    switch (c) {
        // Однозначные односимвольные.
        case '+': return make_token(TokenKind::Plus, start_pos, start);
        case '*': return make_token(TokenKind::Star, start_pos, start);
        case '/': return make_token(TokenKind::Slash, start_pos, start);
        case '%': return make_token(TokenKind::Percent, start_pos, start);
        case ',': return make_token(TokenKind::Comma, start_pos, start);
        case ';': return make_token(TokenKind::Semicolon, start_pos, start);
        case '.': return make_token(TokenKind::Dot, start_pos, start);
        case '(': return make_token(TokenKind::LParen, start_pos, start);
        case ')': return make_token(TokenKind::RParen, start_pos, start);
        case '{': return make_token(TokenKind::LBrace, start_pos, start);
        case '}': return make_token(TokenKind::RBrace, start_pos, start);
        case '[': return make_token(TokenKind::LBracket, start_pos, start);
        case ']': return make_token(TokenKind::RBracket, start_pos, start);

        // Двусимвольные с одиночной альтернативой (maximal munch).
        case '=':
            if (match('=')) return make_token(TokenKind::EqEq, start_pos, start);
            return make_token(TokenKind::Eq, start_pos, start);
        case '!':
            if (match('=')) return make_token(TokenKind::BangEq, start_pos, start);
            return make_token(TokenKind::Bang, start_pos, start);
        case '<':
            if (match('=')) return make_token(TokenKind::LtEq, start_pos, start);
            return make_token(TokenKind::Lt, start_pos, start);
        case '>':
            if (match('=')) return make_token(TokenKind::GtEq, start_pos, start);
            return make_token(TokenKind::Gt, start_pos, start);
        case '-':
            if (match('>')) return make_token(TokenKind::Arrow, start_pos, start);
            return make_token(TokenKind::Minus, start_pos, start);
        case ':':
            if (match('=')) return make_token(TokenKind::ColonEq, start_pos, start);
            return make_token(TokenKind::Colon, start_pos, start);

        // Только двусимвольные.
        case '&':
            if (match('&')) return make_token(TokenKind::AmpAmp, start_pos, start);
            error("expected '&&', got lone '&'", start);
            return Token{TokenKind::Invalid, {}, start};
        case '|':
            if (match('|')) return make_token(TokenKind::PipePipe, start_pos, start);
            error("expected '||', got lone '|'", start);
            return Token{TokenKind::Invalid, {}, start};
    }
    error(std::string("unexpected character: '") + c + "'", start);
    return Token{TokenKind::Invalid, {}, start};
}

std::expected<std::vector<Token>, std::monostate> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skip_whitespace_and_comments();

        auto start = current_loc();
        auto start_pos = pos_;

        if (at_end()) {
            tokens.push_back(Token{TokenKind::Eof, {}, start});
            return tokens;
        }

        char c = peek();
        Token t;
        if (is_letter(c)) {
            t = scan_identifier_or_keyword(start, start_pos);
        } else if (is_digit(c)) {
            t = scan_number(start, start_pos);
        } else if (c == '"') {
            t = scan_string(start, start_pos);
        } else {
            t = scan_punct_or_op(start, start_pos);
        }

        if (fatal_ || t.kind == TokenKind::Invalid) {
            return std::unexpected{std::monostate{}};
        }
        tokens.push_back(t);
    }
}

}  // namespace herta::lexer
