#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "herta/common/source_location.hpp"

namespace herta::lexer {

enum class TokenKind : std::uint8_t {
    // Спец
    Eof,
    Invalid,

    // Литералы и идентификатор
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    Identifier,

    // Ключевые слова
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
    KwTrue,
    KwFalse,

    // Арифметика
    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    // Сравнения
    EqEq,
    BangEq,
    Lt,
    Gt,
    LtEq,
    GtEq,

    // Логика
    AmpAmp,
    PipePipe,
    Bang,

    // Присваивание / вывод типа / возвращаемый тип
    Eq,
    ColonEq,
    Arrow,

    // Разделители
    Colon,
    Comma,
    Semicolon,
    Dot,

    // Скобки
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
};

struct Token {
    TokenKind kind = TokenKind::Invalid;
    std::string_view lexeme;
    herta::common::SourceLocation loc;
};

// Человеко-читаемое имя вида токена (для --dump-tokens и диагностики).
std::string_view to_string(TokenKind k) noexcept;

// Полное представление токена: "<line>:<col>  <KIND>  '<lexeme>'".
std::string to_string(const Token& t);

}  // namespace herta::lexer
