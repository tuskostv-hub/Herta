// Тесты лексера. Покрывают грамматику §2 spec/grammar.md.

#include "herta/common/diagnostic.hpp"
#include "herta/common/source_file.hpp"
#include "herta/lexer/lexer.hpp"

import std;

using herta::common::DiagnosticSink;
using herta::common::SourceFile;
using herta::lexer::Lexer;
using herta::lexer::TokenKind;
using herta::lexer::to_string;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

void expect(bool ok, std::string_view test, std::string_view detail = {}) {
    if (!ok) fail(test, detail);
}

struct Expected {
    TokenKind kind;
    std::string_view lexeme;
};

void check_seq(std::string_view source,
               std::initializer_list<Expected> expected,
               std::string_view test) {
    auto src = SourceFile{std::string(test), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto result = lex.tokenize();

    if (!result) {
        std::ostringstream os;
        os << "tokenize() unexpectedly failed:\n";
        sink.print_all(os);
        fail(test, os.str());
        return;
    }

    auto& tokens = *result;
    auto exp_size = expected.size() + 1;  // +1 for Eof
    if (tokens.size() != exp_size) {
        fail(test, std::format("expected {} tokens (incl. Eof), got {}",
                               exp_size, tokens.size()));
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            std::cerr << "  [" << i << "] " << to_string(tokens[i]) << '\n';
        }
        return;
    }

    std::size_t i = 0;
    for (const auto& e : expected) {
        const auto& got = tokens[i];
        if (got.kind != e.kind || got.lexeme != e.lexeme) {
            fail(test,
                 std::format("token #{}: got {}('{}'), expected {}('{}')",
                             i, to_string(got.kind), got.lexeme,
                             to_string(e.kind), e.lexeme));
        }
        ++i;
    }
    if (tokens.back().kind != TokenKind::Eof) {
        fail(test, "last token is not Eof");
    }
}

void check_error(std::string_view source, std::string_view test) {
    auto src = SourceFile{std::string(test), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto result = lex.tokenize();
    if (result.has_value()) {
        fail(test, "expected error, got successful tokenize()");
    } else if (!sink.has_errors()) {
        fail(test, "tokenize() failed but no diagnostic in sink");
    }
}

}  // namespace

int main() {
    // ---------------- Пустой ввод / пробелы / комментарии ----------------
    check_seq("", {}, "empty");
    check_seq("   \t  ", {}, "spaces only");
    check_seq("\n\n\r\n", {}, "newlines only");
    check_seq("// just a comment", {}, "comment only (no newline)");
    check_seq("// a\n// b\n  // c\n", {}, "multiple comments");
    check_seq("// header\nfn",
        {{TokenKind::KwFn, "fn"}}, "comment then code");

    // ---------------- Идентификаторы ----------------
    check_seq("x foo Bar2 _under __ a1b2",
        {{TokenKind::Identifier, "x"},
         {TokenKind::Identifier, "foo"},
         {TokenKind::Identifier, "Bar2"},
         {TokenKind::Identifier, "_under"},
         {TokenKind::Identifier, "__"},
         {TokenKind::Identifier, "a1b2"}}, "identifiers");

    // Имена базовых типов и builtin'ов — по плану §2.7 это Identifier.
    check_seq("int32 float64 bool string void print input exit panic self",
        {{TokenKind::Identifier, "int32"},
         {TokenKind::Identifier, "float64"},
         {TokenKind::Identifier, "bool"},
         {TokenKind::Identifier, "string"},
         {TokenKind::Identifier, "void"},
         {TokenKind::Identifier, "print"},
         {TokenKind::Identifier, "input"},
         {TokenKind::Identifier, "exit"},
         {TokenKind::Identifier, "panic"},
         {TokenKind::Identifier, "self"}}, "builtins-as-identifiers");

    // ---------------- Ключевые слова (все 18) ----------------
    check_seq("fn let var return if else while break continue "
              "struct type namespace impl module import pub true false",
        {{TokenKind::KwFn, "fn"},
         {TokenKind::KwLet, "let"},
         {TokenKind::KwVar, "var"},
         {TokenKind::KwReturn, "return"},
         {TokenKind::KwIf, "if"},
         {TokenKind::KwElse, "else"},
         {TokenKind::KwWhile, "while"},
         {TokenKind::KwBreak, "break"},
         {TokenKind::KwContinue, "continue"},
         {TokenKind::KwStruct, "struct"},
         {TokenKind::KwType, "type"},
         {TokenKind::KwNamespace, "namespace"},
         {TokenKind::KwImpl, "impl"},
         {TokenKind::KwModule, "module"},
         {TokenKind::KwImport, "import"},
         {TokenKind::KwPub, "pub"},
         {TokenKind::KwTrue, "true"},
         {TokenKind::KwFalse, "false"}}, "keywords");

    // ---------------- Целочисленные литералы ----------------
    check_seq("0 42 1000 0xFF 0x0 0xAbCdEf",
        {{TokenKind::IntLiteral, "0"},
         {TokenKind::IntLiteral, "42"},
         {TokenKind::IntLiteral, "1000"},
         {TokenKind::IntLiteral, "0xFF"},
         {TokenKind::IntLiteral, "0x0"},
         {TokenKind::IntLiteral, "0xAbCdEf"}}, "int literals");

    // ---------------- Вещественные литералы ----------------
    check_seq("3.14 0.0 1.5e10 2.0E-3 1.23e+5 100.500",
        {{TokenKind::FloatLiteral, "3.14"},
         {TokenKind::FloatLiteral, "0.0"},
         {TokenKind::FloatLiteral, "1.5e10"},
         {TokenKind::FloatLiteral, "2.0E-3"},
         {TokenKind::FloatLiteral, "1.23e+5"},
         {TokenKind::FloatLiteral, "100.500"}}, "float literals");

    // "1." без цифры после точки — это IntLiteral "1" + Dot.
    check_seq("1.",
        {{TokenKind::IntLiteral, "1"},
         {TokenKind::Dot, "."}}, "int-then-dot");
    check_seq("p.x",
        {{TokenKind::Identifier, "p"},
         {TokenKind::Dot, "."},
         {TokenKind::Identifier, "x"}}, "field access");

    // ---------------- Строки ----------------
    check_seq(R"("hello" "" "with spaces")",
        {{TokenKind::StringLiteral, R"("hello")"},
         {TokenKind::StringLiteral, R"("")"},
         {TokenKind::StringLiteral, R"("with spaces")"}}, "strings basic");
    check_seq(R"("a\nb" "tab\there" "\\" "\"" "\r\t\n\\\"")",
        {{TokenKind::StringLiteral, R"("a\nb")"},
         {TokenKind::StringLiteral, R"("tab\there")"},
         {TokenKind::StringLiteral, R"("\\")"},
         {TokenKind::StringLiteral, R"("\"")"},
         {TokenKind::StringLiteral, R"("\r\t\n\\\"")"}}, "strings escapes");

    // ---------------- Операторы и разделители ----------------
    check_seq("+ - * / % == != < > <= >= && || ! = := -> : , ; . ( ) { } [ ]",
        {{TokenKind::Plus, "+"},
         {TokenKind::Minus, "-"},
         {TokenKind::Star, "*"},
         {TokenKind::Slash, "/"},
         {TokenKind::Percent, "%"},
         {TokenKind::EqEq, "=="},
         {TokenKind::BangEq, "!="},
         {TokenKind::Lt, "<"},
         {TokenKind::Gt, ">"},
         {TokenKind::LtEq, "<="},
         {TokenKind::GtEq, ">="},
         {TokenKind::AmpAmp, "&&"},
         {TokenKind::PipePipe, "||"},
         {TokenKind::Bang, "!"},
         {TokenKind::Eq, "="},
         {TokenKind::ColonEq, ":="},
         {TokenKind::Arrow, "->"},
         {TokenKind::Colon, ":"},
         {TokenKind::Comma, ","},
         {TokenKind::Semicolon, ";"},
         {TokenKind::Dot, "."},
         {TokenKind::LParen, "("},
         {TokenKind::RParen, ")"},
         {TokenKind::LBrace, "{"},
         {TokenKind::RBrace, "}"},
         {TokenKind::LBracket, "["},
         {TokenKind::RBracket, "]"}}, "operators");

    // ---------------- Maximal munch ----------------
    check_seq("== = != ! <= < >= >",
        {{TokenKind::EqEq, "=="},
         {TokenKind::Eq, "="},
         {TokenKind::BangEq, "!="},
         {TokenKind::Bang, "!"},
         {TokenKind::LtEq, "<="},
         {TokenKind::Lt, "<"},
         {TokenKind::GtEq, ">="},
         {TokenKind::Gt, ">"}}, "maximal munch");
    check_seq(":= : -> -",
        {{TokenKind::ColonEq, ":="},
         {TokenKind::Colon, ":"},
         {TokenKind::Arrow, "->"},
         {TokenKind::Minus, "-"}}, "munch :=, ->");

    // ---------------- Программа целиком ----------------
    check_seq("module hello;\nfn main() int32 { return 0; }",
        {{TokenKind::KwModule, "module"},
         {TokenKind::Identifier, "hello"},
         {TokenKind::Semicolon, ";"},
         {TokenKind::KwFn, "fn"},
         {TokenKind::Identifier, "main"},
         {TokenKind::LParen, "("},
         {TokenKind::RParen, ")"},
         {TokenKind::Identifier, "int32"},
         {TokenKind::LBrace, "{"},
         {TokenKind::KwReturn, "return"},
         {TokenKind::IntLiteral, "0"},
         {TokenKind::Semicolon, ";"},
         {TokenKind::RBrace, "}"}}, "hello world");

    check_seq(R"(let s: string = "hi\n";)",
        {{TokenKind::KwLet, "let"},
         {TokenKind::Identifier, "s"},
         {TokenKind::Colon, ":"},
         {TokenKind::Identifier, "string"},
         {TokenKind::Eq, "="},
         {TokenKind::StringLiteral, R"("hi\n")"},
         {TokenKind::Semicolon, ";"}}, "let with string");

    // ---------------- Позиции (line/col) ----------------
    {
        SourceFile src("<pos>", "a\n  b\n");
        DiagnosticSink sink;
        Lexer lex(src, sink);
        auto r = lex.tokenize();
        expect(r.has_value(), "pos", "tokenize must succeed");
        if (r) {
            const auto& ts = *r;
            expect(ts.size() == 3, "pos", "expected 3 tokens (a, b, Eof)");
            if (ts.size() >= 2) {
                expect(ts[0].loc.line == 1 && ts[0].loc.column == 1,
                       "pos", "a should be at 1:1");
                expect(ts[1].loc.line == 2 && ts[1].loc.column == 3,
                       "pos", "b should be at 2:3");
            }
        }
    }

    // ---------------- Ошибки ----------------
    check_error(R"("unterminated)", "err unterminated string at EOF");
    check_error("\"line\nbreak\"", "err newline inside string");
    check_error(R"("bad \q escape")", "err bad escape \\q");
    check_error("0x", "err empty hex literal");
    check_error("0xZ", "err non-hex digit");
    check_error("a & b", "err lone &");
    check_error("a | b", "err lone |");
    check_error("a @ b", "err invalid character @");
    check_error("1.0e", "err exponent without digit");
    check_error("1.0e+", "err exponent sign without digit");

    if (failures == 0) {
        std::cout << "all lexer tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
}
