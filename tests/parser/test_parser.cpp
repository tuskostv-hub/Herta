// Тесты парсера.

import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;

using herta::common::DiagnosticSink;
using herta::common::SourceFile;
using herta::lexer::Lexer;
using herta::parser::Parser;
namespace ast = herta::ast;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

void expect(bool ok, std::string_view test, std::string_view detail = {}) {
    if (!ok) fail(test, detail);
}

// Прогоняет лексер с парсером и ожидает успех.
std::optional<ast::Program> parse_ok(std::string_view source,
                                     std::string_view test) {
    auto src = SourceFile{std::string(test), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto toks = lex.tokenize();
    if (!toks) {
        std::ostringstream os;
        sink.print_all(os);
        fail(test, std::format("lex failed: {}", os.str()));
        return std::nullopt;
    }
    Parser p(*toks, src.name(), sink);
    auto prog = p.parse_program();
    if (!prog) {
        std::ostringstream os;
        sink.print_all(os);
        fail(test, std::format("parse failed: {}", os.str()));
        return std::nullopt;
    }
    return std::move(*prog);
}

// То же самое, но теперь ожидает ошибку парсера.
void check_parse_error(std::string_view source, std::string_view test) {
    auto src = SourceFile{std::string(test), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto toks = lex.tokenize();
    if (!toks) {
        // если упала уже лексика — это не наш кейс, но это тоже считается ок
        return;
    }
    Parser p(*toks, src.name(), sink);
    auto prog = p.parse_program();
    if (prog.has_value()) {
        fail(test, "expected parser error, got success");
    } else if (!sink.has_errors()) {
        fail(test, "parser failed but no diagnostic in sink");
    }
}

// Маленький хелпер: каст указателя на конкретный узел AST.
template <typename T>
const T* as(const ast::Stmt* s) { return dynamic_cast<const T*>(s); }
template <typename T>
const T* as(const ast::Expr* e) { return dynamic_cast<const T*>(e); }
template <typename T>
const T* as(const ast::Decl* d) { return dynamic_cast<const T*>(d); }

}  // namespace

int main() {
    {
        auto p = parse_ok("module hello; fn main() int32 { return 0; }",
                          "minimal");
        if (p) {
            expect(p->module_name == "hello", "minimal", "module name");
            expect(p->imports.empty(), "minimal", "no imports");
            expect(p->decls.size() == 1, "minimal", "one decl");
        }
    }
    {
        auto p = parse_ok("module a; import b; import c; fn main() int32 { return 0; }",
                          "imports");
        if (p) {
            expect(p->imports.size() == 2, "imports", "two imports");
            if (p->imports.size() == 2) {
                expect(p->imports[0] == "b" && p->imports[1] == "c",
                       "imports", "names");
            }
        }
    }

    {
        auto p = parse_ok("module m; pub fn f() int32 { return 0; } fn g() int32 { return 0; }",
                          "pub fn");
        if (p && p->decls.size() == 2) {
            expect(p->decls[0]->is_pub, "pub fn", "first pub");
            expect(!p->decls[1]->is_pub, "pub fn", "second not pub");
        }
    }

    parse_ok("module m; pub struct Point { x: float64, y: float64, }",
             "struct");
    parse_ok("module m; pub type Meters = int32;", "type alias");
    parse_ok("module m; type Mat = [[float64; 4]; 4];", "nested array type");

    {
        auto p = parse_ok(R"(
            module m;
            struct Point { x: int32, }
            impl Point {
                pub fn make() Point { return Point { x: 0 }; }
                fn helper(self: Point) int32 { return self.x; }
            }
        )", "impl");
        if (p) {
            const auto* im = as<ast::ImplDecl>(p->decls.back().get());
            expect(im != nullptr, "impl", "impl decl present");
            if (im) {
                expect(im->type_name == "Point", "impl", "type name");
                expect(im->methods.size() == 2, "impl", "two methods");
                if (im->methods.size() == 2) {
                    expect(im->methods[0]->is_pub, "impl", "first is pub");
                    expect(!im->methods[1]->is_pub, "impl", "second not pub");
                }
            }
        }
    }

    parse_ok(R"(
        module m;
        namespace N {
            fn f() int32 { return 0; }
            pub type T = int32;
        }
    )", "namespace");

    parse_ok("module m; fn f() int32 { let x: int32 = 5; return x; }",
             "let with type");
    parse_ok("module m; fn f() int32 { var x := 5; return x; }",
             "var with inference");
    parse_ok("module m; fn f() void { if true { } }",
             "if no else");
    parse_ok("module m; fn f() void { if true { } else { } }",
             "if-else");
    parse_ok("module m; fn f() void { if true { } else if false { } else { } }",
             "if-else if-else");
    parse_ok("module m; fn f() void { while true { break; continue; } }",
             "while + break + continue");
    parse_ok("module m; fn f() void { ; ; }", "empty stmts");
    parse_ok("module m; fn f() void { { let x := 1; } }", "nested block");

    {
        // 1 + 2 * 3 → +(1, *(2, 3))
        auto p = parse_ok("module m; fn f() int32 { return 1 + 2 * 3; }",
                          "precedence");
        if (p) {
            const auto& body = static_cast<const ast::FnDecl&>(*p->decls[0]).body;
            const auto* ret = as<ast::ReturnStmt>(body->stmts[0].get());
            expect(ret != nullptr, "precedence", "return stmt");
            if (ret) {
                const auto* bin = as<ast::BinaryExpr>(ret->value.get());
                expect(bin && bin->op == ast::BinaryOp::Add,
                       "precedence", "top is +");
                if (bin) {
                    const auto* rhs = as<ast::BinaryExpr>(bin->rhs.get());
                    expect(rhs && rhs->op == ast::BinaryOp::Mul,
                           "precedence", "rhs is *");
                }
            }
        }
    }
    parse_ok("module m; fn f() bool { return !a && b || c == d; }",
             "bool precedence");
    parse_ok("module m; fn f() int32 { return -x + y; }", "unary minus");

    parse_ok("module m; fn f() int32 { return arr[0]; }", "index");
    parse_ok("module m; fn f() int32 { return p.x; }", "field");
    parse_ok("module m; fn f() int32 { return add(1, 2); }", "call");
    parse_ok("module m; fn f() int32 { return arr[i][j].field(1, 2); }",
             "chained postfix");
    parse_ok("module m; fn f() int32 { return int32(3.14); }",
             "cast-like call");
    parse_ok("module m; fn f() int32 { return Module.func(1); }",
             "module access");

    parse_ok(R"(module m; fn f() string { return "hello\n"; })",
             "string with escape");
    parse_ok("module m; fn f() bool { return true; }", "bool literal");
    parse_ok("module m; fn f() float64 { return 3.14e+5; }", "float literal");

    parse_ok("module m; fn f() void { var a: [int32; 3] = [1, 2, 3]; }",
             "array literal");
    parse_ok(R"(
        module m;
        struct P { x: int32, y: int32, }
        fn f() P { return P { x: 1, y: 2 }; }
    )", "struct literal");
    parse_ok("module m; struct E {} fn f() E { return E {}; }",
             "empty struct literal");

    // struct-литерал прямо в условии if запрещён (как в Go)
    {
        // `if x { y: 1 }` — `x` это условие, `{ y: 1 }` это блок (но он невалидный — `y` бессмыслен)
        // Проверяем, что парсер не пытается съесть {y:1} как struct lit.
        // Простая регрессия: if Point { x } должен распарсить Point как ident,
        // а потом блок с одним выражением. Тут x без ; тоже ошибка, так что
        // подбираем другой кейс:
        // `if x { return 0; }` — должно работать
        parse_ok("module m; fn f() int32 { if x { return 0; } return 1; }",
                 "if cond no struct lit");
    }

    check_parse_error("fn main() int32 { return 0; }",
                      "err missing module");
    check_parse_error("module m fn main() int32 { return 0; }",
                      "err missing semicolon after module");
    check_parse_error("module m; fn main() int32 { return }",
                      "err missing semicolon after return");
    check_parse_error("module m; fn main() int32 { 1 + 2 }",
                      "err missing semicolon after expr");
    check_parse_error("module m; fn main() int32 { 5 = x; }",
                      "err non-lvalue assign");
    check_parse_error("module m; pub impl T { }",
                      "err pub on impl");
    check_parse_error("module m; fn main() int32 { let x = 5; }",
                      "err let without type or :=");
    check_parse_error("module m; fn main() int32 { var x: = 5; }",
                      "err colon without type");

    if (failures == 0) {
        std::cout << "all parser tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
