#include "herta/lexer/lexer.hpp"

namespace herta::lexer {

Lexer::Lexer(const herta::common::SourceFile& src,
             herta::common::DiagnosticSink& sink)
    : src_(src), sink_(sink) {}

std::expected<std::vector<Token>, std::monostate> Lexer::tokenize() {
    // TODO(этап 2): реализовать сканирование согласно impl_plan.md §2.4.
    // Пока возвращаем только Eof, чтобы остальной pipeline собирался.
    std::vector<Token> tokens;
    tokens.push_back(Token{
        .kind = TokenKind::Eof,
        .lexeme = {},
        .loc = {.line = 1, .column = 1, .offset = 0},
    });
    return tokens;
}

}  // namespace herta::lexer
