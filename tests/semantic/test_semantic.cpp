// Тесты семантического анализатора.

import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;
import herta.semantic;

using herta::common::DiagnosticSink;
using herta::common::SourceFile;
using herta::lexer::Lexer;
using herta::parser::Parser;
using herta::semantic::SemanticAnalyzer;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

// Прогон полного pipeline до семантики. ok=true если ожидаем успех.
void run(std::string_view source, bool expect_ok, std::string_view test) {
    auto src = SourceFile{std::string(test), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto toks = lex.tokenize();
    if (!toks) {
        if (expect_ok) {
            std::ostringstream os;
            sink.print_all(os);
            fail(test, std::format("lex failed: {}", os.str()));
        }
        return;
    }
    Parser p(*toks, src.name(), sink);
    auto prog = p.parse_program();
    if (!prog) {
        if (expect_ok) {
            std::ostringstream os;
            sink.print_all(os);
            fail(test, std::format("parse failed: {}", os.str()));
        }
        return;
    }
    SemanticAnalyzer sema(*prog, src.name(), sink, {}, /*require_main=*/false);
    bool ok = sema.analyze();

    if (expect_ok && !ok) {
        std::ostringstream os;
        sink.print_all(os);
        fail(test, std::format("expected success, got error:\n{}", os.str()));
    } else if (!expect_ok && ok) {
        fail(test, "expected semantic error, got success");
    }
}

void ok(std::string_view source, std::string_view test)  { run(source, true,  test); }
void err(std::string_view source, std::string_view test) { run(source, false, test); }

}  // namespace

int main() {
    ok("module m; fn main() int32 { return 0; }", "minimal");
    ok("module m; fn main() int32 { let x: int32 = 5; return x; }", "let typed");
    ok("module m; fn main() int32 { var x := 5; return x; }", "var inferred");
    ok("module m; fn main() void { return; }", "void return");

    ok("module m; fn f() int8 { return 42; }",   "lit fits int8");
    err("module m; fn f() int8 { return 200; }", "lit doesn't fit int8");
    ok("module m; fn f() int8 { return -128; }", "lit -128 fits int8");
    err("module m; fn f() int8 { return -129; }", "lit -129 too small");
    ok("module m; fn f() uint8 { return 255; }",  "lit fits uint8");
    err("module m; fn f() uint8 { return -1; }",  "lit negative for uint");
    ok("module m; fn f() float32 { return 3.14; }", "float lit to float32");

    ok("module m; fn f() int32 { var x: int32 = 0; x = 5; return x; }",
       "var assign ok");
    err("module m; fn f() int32 { let x: int32 = 0; x = 5; return x; }",
        "let assign error");

    ok("module m; fn f() int32 { var x: int32 = 1; { var x: int32 = 2; } return x; }",
       "shadow ok");
    err("module m; fn f() int32 { var x: int32 = 1; var x: int32 = 2; return x; }",
        "redecl in same scope");
    err("module m; fn f() int32 { return y; }", "use undeclared");
    err("module m; fn f() int32 { let y: int32 = y; return y; }",
        "use before decl in same statement");  // y не виден в init самого себя

    ok("module m; fn f() int64 { let a: int32 = 1; return a; }",
       "int32 → int64 widening");
    err("module m; fn f() int8 { let a: int32 = 1; return a; }",
        "narrow not implicit");
    ok("module m; fn f() float64 { let a: int32 = 5; return a; }",
       "int32 → float64");
    err("module m; fn f() int32 { let a: float64 = 1.0; return a; }",
        "float→int not implicit");

    ok("module m; fn f() int64 { let a: int32 = 1; let b: int64 = 2; return a + b; }",
       "int32 + int64");
    err("module m; fn f() int32 { let a: int32 = 1; let b: uint32 = 2; return a + b; }",
        "signed+unsigned same width error");
    ok("module m; fn f() string { return \"a\" + \"b\"; }", "string concat");
    ok("module m; fn f() bool { return true && false; }", "bool and");
    err("module m; fn f() bool { return 1 && true; }", "non-bool to &&");
    ok("module m; fn f() bool { return 1 < 2; }", "comparison");
    err("module m; fn f() int32 { return 1 % 1.5; }", "% on float");
    ok("module m; fn f() int32 { return 7 / 2; }", "int division");

    ok("module m; fn f() bool { return !true; }",  "!bool");
    err("module m; fn f() bool { return !1; }",     "!non-bool");
    ok("module m; fn f() int32 { return -5; }",     "unary - on int");
    err("module m; fn f() bool { return -true; }",  "unary - on bool");

    ok("module m; fn f() int8 { return int8(200); }",     "cast int→int");
    ok("module m; fn f() int32 { return int32(3.14); }",  "cast float→int");
    err("module m; fn f() int32 { return int32(true); }",  "cast bool→int error");
    err("module m; fn f() string { return string(5); }",   "cast num→string error");

    ok("module m; fn f() int32 { if true { return 1; } return 0; }", "if true");
    err("module m; fn f() int32 { if 5 { return 1; } return 0; }",   "if non-bool");
    ok("module m; fn f() void { while false { break; } }",            "break in loop");
    err("module m; fn f() void { break; }",                            "break outside loop");
    err("module m; fn f() void { continue; }",                         "continue outside loop");
    ok("module m; fn f() void { while true { while true { break; } } }",
       "nested loops");
    err("module m; fn f() int32 { return; }",       "missing value in return");
    err("module m; fn f() void { return 0; }",       "value in void return");

    ok(R"(
        module m;
        struct P { x: int32, y: int32, }
        fn f() int32 { let p: P = P { x: 1, y: 2 }; return p.x; }
    )", "struct + literal + field");
    err(R"(
        module m;
        struct P { x: int32, y: int32, }
        fn f() int32 { let p: P = P { x: 1 }; return 0; }
    )", "struct missing field");
    err(R"(
        module m;
        struct P { x: int32, y: int32, }
        fn f() int32 { let p: P = P { y: 2, x: 1 }; return 0; }
    )", "struct wrong field order");
    err(R"(
        module m;
        struct P { x: int32, }
        fn f() int32 { let p: P = P { x: 1 }; return p.y; }
    )", "struct no such field");

    ok(R"(
        module m;
        fn f() int32 {
            var a: [int32; 3] = [1, 2, 3];
            return a[0];
        }
    )", "array literal + index");
    err(R"(
        module m;
        fn f() int32 {
            var a: [int32; 3] = [1, 2];
            return a[0];
        }
    )", "array wrong size");
    err(R"(
        module m;
        fn f() int32 {
            var a: int32 = 5;
            return a[0];
        }
    )", "index non-array");
    err(R"(
        module m;
        fn f() int32 {
            var a: [int32; 3] = [1, 2, 3];
            return a[1.5];
        }
    )", "index by float");

    ok(R"(
        module m;
        fn add(a: int32, b: int32) int32 { return a + b; }
        fn main() int32 { return add(1, 2); }
    )", "fn call");
    err(R"(
        module m;
        fn add(a: int32, b: int32) int32 { return a + b; }
        fn main() int32 { return add(1); }
    )", "wrong arg count");
    err(R"(
        module m;
        fn add(a: int32, b: int32) int32 { return a + b; }
        fn main() int32 { return add(1, true); }
    )", "wrong arg type");

    ok(R"(
        module m;
        namespace N {
            fn f() int32 { return 1; }
        }
        fn main() int32 { return N.f(); }
    )", "namespace fn call");
    err(R"(
        module m;
        namespace N { fn f() int32 { return 1; } }
        fn main() int32 { return N.g(); }
    )", "namespace no member");

    ok(R"(
        module m;
        type Meters = int32;
        fn f() Meters { let m: Meters = 5; return m; }
    )", "type alias");
    ok(R"(
        module m;
        type Meters = int32;
        fn f() int32 { let m: Meters = 5; return m; }
    )", "alias compatible with target");

    ok("module m; fn f() void { print(42); }", "print int");
    ok("module m; fn f() void { print(\"hi\"); }", "print string");
    err("module m; fn f() void { print(42, 43); }", "print wrong arg count");
    ok("module m; fn f() string { return input(); }", "input");
    ok("module m; fn f() void { exit(0); }", "exit");
    ok("module m; fn f() void { panic(\"boom\"); }", "panic");
    err("module m; fn f() void { panic(5); }", "panic wrong arg type");
    ok("module m; fn f() int32 { return len(\"abc\"); }", "len");

    ok(R"(
        module m;
        struct P { x: int32, }
        impl P { pub fn get(self: P) int32 { return self.x; } }
        fn main() int32 {
            let p: P = P { x: 7 };
            return p.get();
        }
    )", "instance method");
    ok(R"(
        module m;
        struct P { x: int32, }
        impl P { pub fn make() P { return P { x: 42 }; } }
        fn main() int32 {
            let p: P = P.make();
            return p.x;
        }
    )", "static method (no self)");
    ok(R"(
        module m;
        struct P { x: int32, }
        impl P { pub fn add(self: P, n: int32) int32 { return self.x + n; } }
        fn main() int32 {
            let p: P = P { x: 1 };
            return p.add(2);
        }
    )", "instance method with arg");
    // Видимость методов (A.2.12): без pub метод доступен только из методов
    // своего типа.
    err(R"(
        module m;
        struct P { x: int32, }
        impl P { fn secret(self: P) int32 { return self.x; } }
        fn main() int32 {
            let p: P = P { x: 1 };
            return p.secret();
        }
    )", "private method outside type");
    ok(R"(
        module m;
        struct P { x: int32, }
        impl P {
            fn secret(self: P) int32 { return self.x; }
            pub fn get(self: P) int32 { return self.secret(); }
        }
        fn main() int32 {
            let p: P = P { x: 7 };
            return p.get();
        }
    )", "private method from own method");
    // priv-поля (A.2.12): доступ и конструирование только в методах типа.
    err(R"(
        module m;
        struct P { priv x: int32, }
        impl P { pub fn make() P { return P { x: 1 }; } }
        fn main() int32 {
            let p: P = P.make();
            return p.x;
        }
    )", "priv field read outside type");
    err(R"(
        module m;
        struct P { priv x: int32, }
        fn main() int32 {
            let p: P = P { x: 1 };
            return 0;
        }
    )", "priv field literal outside type");
    ok(R"(
        module m;
        struct P { priv x: int32, }
        impl P {
            pub fn make() P { return P { x: 41 }; }
            pub fn get(self: P) int32 { return self.x + 1; }
        }
        fn main() int32 {
            let p: P = P.make();
            return p.get();
        }
    )", "priv field inside methods");
    err(R"(
        module m;
        struct P { x: int32, }
        impl P { fn f(self: P) int32 { return self.x; } }
        fn main() int32 {
            let p: P = P { x: 1 };
            return p.nope();
        }
    )", "no such method");
    err(R"(
        module m;
        struct P { x: int32, }
        impl P { fn f(self: P) int32 { return self.x; } fn f(self: P) int32 { return 0; } }
    )", "duplicate method");
    err(R"(
        module m;
        struct A { x: int32, }
        struct B { x: int32, }
        impl A { fn f(self: B) int32 { return 0; } }
    )", "self type mismatch");
    // Regression: для одного типа допустимы несколько impl-блоков; тела
    // методов должны типизироваться против СВОИХ сигнатур, а не сигнатур
    // методов из соседних impl-блоков.
    ok(R"(
        module m;
        struct P { x: int32, }
        impl P { pub fn a(self: P) int32 { return self.x; } }
        impl P { pub fn b(self: P, n: int32) int32 { return self.x + n; } }
        fn main() int32 {
            let p: P = P { x: 7 };
            return p.a() + p.b(3);
        }
    )", "multiple impl blocks for same type");
    err(R"(
        module m;
        struct P { x: int32, }
        impl P { fn a(self: P) int32 { return self.x; } }
        impl P { fn a(self: P) int32 { return 0; } }
    )", "duplicate method across impl blocks");

    ok("module m; fn f() char { return 'a'; }", "char literal");
    ok("module m; fn f() char { return '\\n'; }", "char escape");
    ok("module m; fn f() bool { return 'a' == 'b'; }", "char eq");
    ok("module m; fn f() bool { return 'a' != 'b'; }", "char neq");
    err("module m; fn f() bool { return 'a' < 'b'; }", "char no <");
    err("module m; fn f() char { return 'a' + 'b'; }", "char no +");
    ok("module m; fn f() int32 { return int32('A'); }",  "cast char→int");
    ok("module m; fn f() char  { return char(65); }",     "cast int→char");
    err("module m; fn f() string { return string('a'); }", "cast char→string error");
    ok("module m; fn f() void { print('a'); }", "print char");

    ok("module m; fn f() int32 { return 0b101010; }",  "binary literal");
    ok("module m; fn f() uint8 { return 0b11111111; }", "binary fits uint8");
    err("module m; fn f() int32 { return 0b; }", "empty binary literal");

    ok("module m; fn f() void { assert(true); }",  "assert bool");
    ok("module m; fn f() void { assert(1 == 1); }", "assert comparison");
    err("module m; fn f() void { assert(1); }",     "assert non-bool");
    err("module m; fn f() void { assert(); }",      "assert no args");

    ok(R"(
        module m;
        fn f() int32 {
            var a: [int32; 5] = [1, 2, 3, 4, 5];
            return len(a);
        }
    )", "len of array");
    ok("module m; fn f() int32 { return len(\"hello\"); }", "len of string");
    err("module m; fn f() int32 { return len(42); }", "len of int error");

    ok(R"(
        module m;
        fn f() bool {
            var a: [int32; 3] = [1, 2, 3];
            var b: [int32; 3] = [1, 2, 3];
            return a == b;
        }
    )", "array == same size");
    err(R"(
        module m;
        fn f() bool {
            var a: [int32; 3] = [1, 2, 3];
            var b: [int32; 5] = [1, 2, 3, 4, 5];
            return a == b;
        }
    )", "array == different sizes");

    ok(R"(
        module m;
        struct P { x: int32, }
        fn f() int32 {
            var p: P = P { x: 1 };
            p.x = 5;
            return p.x;
        }
    )", "var struct field mutable");
    err(R"(
        module m;
        struct P { x: int32, }
        fn f() int32 {
            let p: P = P { x: 1 };
            p.x = 5;
            return p.x;
        }
    )", "let struct field immutable (chain)");
    err(R"(
        module m;
        fn f() int32 {
            let a: [int32; 3] = [1, 2, 3];
            a[0] = 5;
            return a[0];
        }
    )", "let array element immutable (chain)");

    // Указатели: == / != включая null (fix Н-3); & только на переменной.
    ok(R"(
        module m;
        fn f() bool {
            var x: int32 = 1;
            var p: *int32 = &x;
            return p == null || p != null;
        }
    )", "pointer null compare");
    err(R"(
        module m;
        fn f() bool {
            var x: int32 = 1;
            var y: float64 = 1.0;
            var p: *int32 = &x;
            var q: *float64 = &y;
            return p == q;
        }
    )", "pointer compare different pointee");
    err(R"(
        module m;
        struct P { x: int32, }
        fn f() int32 {
            var p: P = P { x: 1 };
            let q: *int32 = &p.x;
            return 0;
        }
    )", "& on field rejected");
    err(R"(
        module m;
        fn f() int32 {
            var a: [int32; 2] = [1, 2];
            let q: *int32 = &a[0];
            return 0;
        }
    )", "& on index rejected");
    // print не принимает указатели и byte (fix БАГ-4).
    err(R"(
        module m;
        fn f() void {
            var x: int32 = 1;
            var p: *int32 = &x;
            print(p);
        }
    )", "print pointer rejected");
    // Отрицательные вещественные литералы адаптируются к float32 (fix Н-5).
    ok("module m; fn f() float32 { return -1.5; }", "neg float lit to float32");
    err("module m; fn f() float32 { return 3.4e50; }", "float lit too big for float32");
    ok("module m; fn f() float64 { var x: float32 = -2.5; return x; }",
       "neg float lit var decl");
    // Суффиксы литералов (A.1.1).
    ok("module m; fn f() int8 { return 42i8; }", "suffix i8");
    ok("module m; fn f() uint64 { return 7u64; }", "suffix u64");
    ok("module m; fn f() float32 { return 2.5f32; }", "suffix f32");
    ok("module m; fn f() int64 { return 42i16; }", "suffixed literal widens");
    err("module m; fn f() int8 { return 300i8; }", "suffix value does not fit");
    err("module m; fn f() int8 { return 42i16; }", "suffixed literal does not narrow");
    // Литералы без суффикса: int32, при переполнении — int64; uint64-макс
    // достижим (М-1).
    ok("module m; fn f() int64 { var x := 5000000000; return x; }",
       "big literal defaults to int64");
    ok("module m; fn f() uint64 { return 18446744073709551615; }",
       "uint64 max literal");
    err("module m; fn f() int64 { return 18446744073709551615; }",
        "uint64-range literal not int64");
    // byte: значение создаётся только явным cast'ом, сравнимо на == / !=.
    ok(R"(
        module m;
        fn f() bool {
            var b: byte = byte(7);
            let c: byte = byte(7);
            return b == c;
        }
    )", "byte cast + eq");
    err("module m; fn f() void { var b: byte = 0; }", "byte needs explicit cast");
    err(R"(
        module m;
        fn f() byte {
            var a: byte = byte(1);
            var b: byte = byte(2);
            return a + b;
        }
    )", "byte not arithmetic");
    // Вывод типа возврата функции (A.1.7).
    ok(R"(
        module m;
        fn double(x: int32) { return x * 2; }
        fn main() int32 { return double(21); }
    )", "fn return type inference");
    ok(R"(
        module m;
        fn greet() { print("hi"); }
        fn main() int32 { greet(); return 0; }
    )", "fn infers void");
    err(R"(
        module m;
        fn bad(c: bool) {
            if c { return 1; }
            return "no";
        }
    )", "conflicting inferred return types");
    // Стрелочная форма типа возврата.
    ok("module m; fn f() -> int32 { return 1; }", "arrow return type");
    // Зарезервированные имена builtin'ов (fix Н-4).
    err("module m; fn f() void { var input: int32 = 5; }", "shadow builtin var");
    err("module m; fn len() int32 { return 0; }", "redeclare builtin fn");
    err("module m; fn f(print: int32) int32 { return print; }", "builtin as param");
    // (строгий int32 у main проверяется e2e-тестами: здесь require_main=false)

    ok(R"(
        module m;
        namespace Utils {
            type Index = int32;
            fn swap(arr: [int32; 5], i: Index, j: Index) void {
                var tmp: int32 = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
        fn main() int32 {
            var arr: [int32; 5] = [5, 3, 1, 4, 2];
            Utils.swap(arr, 0, 1);
            return arr[0];
        }
    )", "bubble_sort fragment");

    if (failures == 0) {
        std::cout << "all semantic tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
