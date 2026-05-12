#pragma once

#include <expected>
#include <variant>
#include <vector>

#include "herta/common/diagnostic.hpp"
#include "herta/common/source_file.hpp"
#include "herta/lexer/token.hpp"

namespace herta::lexer {

// Лексический анализатор: преобразует исходный текст в поток токенов.
// При обнаружении ошибки добавляет диагностику в sink и останавливается.
class Lexer {
public:
    Lexer(const herta::common::SourceFile& src,
          herta::common::DiagnosticSink& sink);

    // Возвращает все токены до Eof включительно.
    // В случае ошибки результат содержит std::unexpected; детали — в sink.
    std::expected<std::vector<Token>, std::monostate> tokenize();

private:
    const herta::common::SourceFile& src_;
    herta::common::DiagnosticSink& sink_;
};

}  // namespace herta::lexer
