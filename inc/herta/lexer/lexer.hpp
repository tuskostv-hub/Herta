#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>
#include <vector>

#include "herta/common/diagnostic.hpp"
#include "herta/common/source_file.hpp"
#include "herta/lexer/token.hpp"

namespace herta::lexer {

// Лексический анализатор: преобразует исходный текст в поток токенов.
// При обнаружении ошибки добавляет диагностику в sink и останавливается
// (поведение «остановка на первой ошибке» из ТЗ).
class Lexer {
public:
    Lexer(const herta::common::SourceFile& src,
          herta::common::DiagnosticSink& sink);

    // Возвращает все токены до Eof включительно.
    // На ошибке — std::unexpected{}; детали в sink.
    std::expected<std::vector<Token>, std::monostate> tokenize();

private:
    // Курсор / источник.
    bool at_end() const noexcept;
    char peek(std::size_t lookahead = 0) const noexcept;
    char advance() noexcept;
    bool match(char c) noexcept;
    herta::common::SourceLocation current_loc() const noexcept;

    void skip_whitespace_and_comments();

    // Конструктор токена по диапазону [start_pos, pos_).
    Token make_token(TokenKind kind,
                     std::size_t start_pos,
                     herta::common::SourceLocation start_loc) const;

    // Сканеры конкретных категорий лексем.
    Token scan_identifier_or_keyword(herta::common::SourceLocation start,
                                     std::size_t start_pos);
    Token scan_number(herta::common::SourceLocation start,
                      std::size_t start_pos);
    Token scan_string(herta::common::SourceLocation start,
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

}  // namespace herta::lexer
