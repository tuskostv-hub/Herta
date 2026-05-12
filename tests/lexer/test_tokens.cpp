// Заглушка для тестов лексера. Полноценные golden-тесты появятся вместе с
// реализацией Lexer::tokenize() (см. impl_plan.md §2.8).

#include <cstdlib>
#include <iostream>

#include "herta/common/diagnostic.hpp"
#include "herta/common/source_file.hpp"
#include "herta/lexer/lexer.hpp"

int main() {
    herta::common::SourceFile src("<test>", "");
    herta::common::DiagnosticSink sink;
    herta::lexer::Lexer lex(src, sink);

    auto tokens = lex.tokenize();
    if (!tokens) {
        std::cerr << "tokenize() failed on empty input\n";
        return EXIT_FAILURE;
    }
    if (tokens->size() != 1 || tokens->front().kind != herta::lexer::TokenKind::Eof) {
        std::cerr << "expected single Eof token on empty input\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
