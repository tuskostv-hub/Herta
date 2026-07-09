// Лексер: режет исходный текст на токены.

export module herta.lexer;

import std;
import herta.common;

namespace herta::lexer {

export enum class TokenKind : std::uint8_t {
    // Служебные
    Eof,
    Invalid,

    // Литералы и идентификатор
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,
    Identifier,

    // Ключевые слова языка
    KwFn,
    KwLet,
    KwVar,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwBreak,
    KwContinue,
    KwStruct,
    KwType,
    KwNamespace,
    KwImpl,
    KwModule,
    KwImport,
    KwPub,
    KwPriv,    // приватное поле структуры
    KwTrue,
    KwFalse,
    KwNull,    // нулевой указатель

    // Арифметика
    Plus, Minus, Star, Slash, Percent,

    // Сравнения
    EqEq, BangEq, Lt, Gt, LtEq, GtEq,

    // Логика
    AmpAmp, PipePipe, Bang,
    // Унарный & — взятие адреса. Битового AND в языке нет.
    Amp,

    // Присваивание, вывод типа, стрелка возврата
    Eq, ColonEq, Arrow,

    // Разделители
    Colon, Comma, Semicolon, Dot,

    // Скобки
    LParen, RParen,
    LBrace, RBrace,
    LBracket, RBracket,
};

export struct Token {
    TokenKind kind = TokenKind::Invalid;
    std::string_view lexeme;
    herta::common::SourceLocation loc;
};

// Понятное имя вида токена для --dump-tokens и сообщений об ошибках.
export std::string_view to_string(TokenKind k) noexcept;

// Полное представление токена: "<line>:<col>  <KIND>  '<lexeme>'".
export std::string to_string(const Token& t);

// На первой же ошибке кладёт диагностику в sink и останавливается.
export class Lexer {
public:
    Lexer(const herta::common::SourceFile& src,
          herta::common::DiagnosticSink& sink);

    // Отдаёт все токены до Eof включительно. На ошибке — std::unexpected{},
    // подробности уходят в sink.
    std::expected<std::vector<Token>, std::monostate> tokenize();

private:
    // Текущая позиция в исходнике
    bool at_end() const noexcept;
    char peek(std::size_t lookahead = 0) const noexcept;
    char advance() noexcept;
    bool match(char c) noexcept;
    herta::common::SourceLocation current_loc() const noexcept;

    void skip_whitespace_and_comments();

    // Собрать токен по диапазону [start_pos, pos_).
    Token make_token(TokenKind kind,
                     std::size_t start_pos,
                     herta::common::SourceLocation start_loc) const;

    Token scan_identifier_or_keyword(herta::common::SourceLocation start,
                                     std::size_t start_pos);
    Token scan_number(herta::common::SourceLocation start,
                      std::size_t start_pos);
    Token scan_string(herta::common::SourceLocation start,
                      std::size_t start_pos);
    Token scan_char(herta::common::SourceLocation start,
                    std::size_t start_pos);
    Token scan_punct_or_op(herta::common::SourceLocation start,
                           std::size_t start_pos);

    void error(std::string message, herta::common::SourceLocation loc);

    const herta::common::SourceFile& src_;
    herta::common::DiagnosticSink& sink_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
    bool fatal_ = false;
};

namespace {

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

// Таблица ключевых слов в порядке из грамматики.
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
    {"priv",      TokenKind::KwPriv},
    {"true",      TokenKind::KwTrue},
    {"false",     TokenKind::KwFalse},
    {"null",      TokenKind::KwNull},
};

TokenKind keyword_lookup(std::string_view text) noexcept {
    for (const auto& e : kKeywords) {
        if (e.text == text) return e.kind;
    }
    return TokenKind::Identifier;
}

}  // anonymous namespace

std::string_view to_string(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::Eof: return "Eof";
        case TokenKind::Invalid: return "Invalid";
        case TokenKind::IntLiteral: return "IntLiteral";
        case TokenKind::FloatLiteral: return "FloatLiteral";
        case TokenKind::StringLiteral: return "StringLiteral";
        case TokenKind::CharLiteral: return "CharLiteral";
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::KwFn: return "KwFn";
        case TokenKind::KwLet: return "KwLet";
        case TokenKind::KwVar: return "KwVar";
        case TokenKind::KwReturn: return "KwReturn";
        case TokenKind::KwIf: return "KwIf";
        case TokenKind::KwElse: return "KwElse";
        case TokenKind::KwWhile: return "KwWhile";
        case TokenKind::KwBreak: return "KwBreak";
        case TokenKind::KwContinue: return "KwContinue";
        case TokenKind::KwStruct: return "KwStruct";
        case TokenKind::KwType: return "KwType";
        case TokenKind::KwNamespace: return "KwNamespace";
        case TokenKind::KwImpl: return "KwImpl";
        case TokenKind::KwModule: return "KwModule";
        case TokenKind::KwImport: return "KwImport";
        case TokenKind::KwPub: return "KwPub";
        case TokenKind::KwPriv: return "KwPriv";
        case TokenKind::KwTrue: return "KwTrue";
        case TokenKind::KwFalse: return "KwFalse";
        case TokenKind::KwNull: return "KwNull";
        case TokenKind::Amp: return "Amp";
        case TokenKind::Plus: return "Plus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::Star: return "Star";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Percent: return "Percent";
        case TokenKind::EqEq: return "EqEq";
        case TokenKind::BangEq: return "BangEq";
        case TokenKind::Lt: return "Lt";
        case TokenKind::Gt: return "Gt";
        case TokenKind::LtEq: return "LtEq";
        case TokenKind::GtEq: return "GtEq";
        case TokenKind::AmpAmp: return "AmpAmp";
        case TokenKind::PipePipe: return "PipePipe";
        case TokenKind::Bang: return "Bang";
        case TokenKind::Eq: return "Eq";
        case TokenKind::ColonEq: return "ColonEq";
        case TokenKind::Arrow: return "Arrow";
        case TokenKind::Colon: return "Colon";
        case TokenKind::Comma: return "Comma";
        case TokenKind::Semicolon: return "Semicolon";
        case TokenKind::Dot: return "Dot";
        case TokenKind::LParen: return "LParen";
        case TokenKind::RParen: return "RParen";
        case TokenKind::LBrace: return "LBrace";
        case TokenKind::RBrace: return "RBrace";
        case TokenKind::LBracket: return "LBracket";
        case TokenKind::RBracket: return "RBracket";
    }
    return "?";
}

std::string to_string(const Token& t) {
    std::ostringstream os;
    os << t.loc.line << ':' << t.loc.column << "  "
       << to_string(t.kind) << "  '" << t.lexeme << '\'';
    return os.str();
}

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

herta::common::SourceLocation Lexer::current_loc() const noexcept {
    return herta::common::SourceLocation{
        .line = line_,
        .column = col_,
        .offset = static_cast<std::uint32_t>(pos_),
    };
}

Token Lexer::make_token(TokenKind kind,
                        std::size_t start_pos,
                        herta::common::SourceLocation start_loc) const {
    auto contents = src_.contents();
    return Token{
        .kind = kind,
        .lexeme = contents.substr(start_pos, pos_ - start_pos),
        .loc = start_loc,
    };
}

void Lexer::error(std::string message, herta::common::SourceLocation loc) {
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
            // Однострочный комментарий: пропускаем всё до перевода строки
            while (!at_end() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::scan_identifier_or_keyword(herta::common::SourceLocation start,
                                        std::size_t start_pos) {
    while (!at_end() && is_letter_or_digit(peek())) advance();
    auto lexeme = src_.contents().substr(start_pos, pos_ - start_pos);
    return Token{
        .kind = keyword_lookup(lexeme),
        .lexeme = lexeme,
        .loc = start,
    };
}

// Суффиксы размера для числовых литералов (A.1.1): 42i32, 7u8, 3.14f32 и т.п.
namespace {
constexpr std::string_view kIntSuffixes[] = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
};
constexpr std::string_view kFloatSuffixes[] = {"f32", "f64"};

bool is_int_suffix(std::string_view s) noexcept {
    for (auto v : kIntSuffixes) if (v == s) return true;
    return false;
}
bool is_float_suffix(std::string_view s) noexcept {
    for (auto v : kFloatSuffixes) if (v == s) return true;
    return false;
}
}  // anonymous namespace

Token Lexer::scan_number(herta::common::SourceLocation start,
                         std::size_t start_pos) {
    // После тела литерала может идти суффикс типа. Съедает его и
    // возвращает "": суффикса нет, сам суффикс, либо nullopt при ошибке.
    auto scan_suffix = [&]() -> std::optional<std::string> {
        if (!is_letter(peek())) return std::string{};
        auto sfx_loc = current_loc();
        std::string sfx;
        while (!at_end() && is_letter_or_digit(peek())) sfx.push_back(advance());
        if (is_int_suffix(sfx) || is_float_suffix(sfx)) return sfx;
        error("invalid numeric literal suffix '" + sfx +
              "' (expected i8..i64, u8..u64, f32 or f64)", sfx_loc);
        return std::nullopt;
    };

    // Шестнадцатеричный литерал вида "0x" + hex-цифры
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();  // '0'
        advance();  // 'x' / 'X'
        if (!is_hex_digit(peek())) {
            error("invalid hex literal: at least one hex digit required", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        while (!at_end() && is_hex_digit(peek())) advance();
        // Суффикс hex-литерала может начинаться только с i/u: f-суффиксы и
        // суффиксы с a..f неотличимы от hex-цифр и потому недоступны.
        auto sfx = scan_suffix();
        if (!sfx) return Token{TokenKind::Invalid, {}, start};
        if (is_float_suffix(*sfx)) {
            error("float suffix is not allowed on hex literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        return make_token(TokenKind::IntLiteral, start_pos, start);
    }
    // Двоичный литерал вида "0b" + нули и единицы
    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        advance();  // '0'
        advance();  // 'b' / 'B'
        if (peek() != '0' && peek() != '1') {
            error("invalid binary literal: at least one binary digit required", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        while (peek() == '0' || peek() == '1') advance();
        auto sfx = scan_suffix();
        if (!sfx) return Token{TokenKind::Invalid, {}, start};
        if (is_float_suffix(*sfx)) {
            error("float suffix is not allowed on binary literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        return make_token(TokenKind::IntLiteral, start_pos, start);
    }

    // Десятичная целая часть
    while (!at_end() && is_digit(peek())) advance();

    // Float По грамматике нужно digit "." digit { digit } [ exponent ].
    // То есть "1." без цифры после точки даёт два токена: IntLiteral "1" и Dot.
    if (peek() == '.' && is_digit(peek(1))) {
        advance();  // '.'
        while (!at_end() && is_digit(peek())) advance();

        // экспонента: (e|E) [+|-] цифры
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
        auto sfx = scan_suffix();
        if (!sfx) return Token{TokenKind::Invalid, {}, start};
        if (is_int_suffix(*sfx)) {
            error("integer suffix is not allowed on float literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        return make_token(TokenKind::FloatLiteral, start_pos, start);
    }

    // Целый литерал; f32/f64-суффикс превращает его в вещественный (42f64).
    auto sfx = scan_suffix();
    if (!sfx) return Token{TokenKind::Invalid, {}, start};
    if (is_float_suffix(*sfx)) {
        return make_token(TokenKind::FloatLiteral, start_pos, start);
    }
    return make_token(TokenKind::IntLiteral, start_pos, start);
}

Token Lexer::scan_string(herta::common::SourceLocation start,
                         std::size_t start_pos) {
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

Token Lexer::scan_char(herta::common::SourceLocation start, std::size_t start_pos) {
    advance();  // открывающая '
    if (at_end() || peek() == '\n') {
        error("unterminated char literal", start);
        return Token{TokenKind::Invalid, {}, start};
    }
    if (peek() == '\'') {
        error("empty char literal", start);
        return Token{TokenKind::Invalid, {}, start};
    }
    if (peek() == '\\') {
        auto esc_loc = current_loc();
        advance();  // '\\'
        if (at_end()) {
            error("unterminated char literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        char e = peek();
        if (e == '\'' || e == '\\' || e == 'n' || e == 't' || e == 'r' || e == '0') {
            advance();
        } else {
            error(std::string("invalid escape sequence in char literal: \\") + e, esc_loc);
            return Token{TokenKind::Invalid, {}, start};
        }
    } else {
        // UTF-8: длину последовательности задаёт первый байт
        auto first = static_cast<unsigned char>(peek());
        int total;
        if (first < 0x80)            total = 1;  // обычный ASCII
        else if ((first & 0xE0) == 0xC0) total = 2;
        else if ((first & 0xF0) == 0xE0) total = 3;
        else if ((first & 0xF8) == 0xF0) total = 4;
        else {
            error("invalid UTF-8 lead byte in char literal", start);
            return Token{TokenKind::Invalid, {}, start};
        }
        advance();  // первый байт
        for (int i = 1; i < total; ++i) {
            if (at_end()) {
                error("unterminated UTF-8 sequence in char literal", start);
                return Token{TokenKind::Invalid, {}, start};
            }
            auto b = static_cast<unsigned char>(peek());
            if ((b & 0xC0) != 0x80) {
                error("invalid UTF-8 continuation byte in char literal", start);
                return Token{TokenKind::Invalid, {}, start};
            }
            advance();
        }
    }
    if (peek() != '\'') {
        error("char literal must contain exactly one character", start);
        return Token{TokenKind::Invalid, {}, start};
    }
    advance();  // закрывающая '
    return make_token(TokenKind::CharLiteral, start_pos, start);
}

Token Lexer::scan_punct_or_op(herta::common::SourceLocation start,
                              std::size_t start_pos) {
    char c = advance();
    switch (c) {
        // Однозначные односимвольные токены
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

        // Двусимвольные с альтернативой из одного символа (maximal munch)
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

        // Только двусимвольные. Одиночный символ — ошибка.
        case '&':
            if (match('&')) return make_token(TokenKind::AmpAmp, start_pos, start);
            // Одиночный & — взятие адреса
            return make_token(TokenKind::Amp, start_pos, start);
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
        } else if (c == '\'') {
            t = scan_char(start, start_pos);
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
