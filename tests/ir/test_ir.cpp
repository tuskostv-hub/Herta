// Тесты IR lowering. Гоним полный pipeline до семантики, затем lower
// и проверяем что вывод (а) непустой и (б) содержит ожидаемые маркеры.

import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;
import herta.semantic;
import herta.ir;

using herta::common::DiagnosticSink;
using herta::common::SourceFile;
using herta::lexer::Lexer;
using herta::parser::Parser;
using herta::semantic::SemanticAnalyzer;
using herta::ir::Lowerer;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

// Лоуэрит source и возвращает текстовый дамп IR (или пустую строку при ошибке).
// optimize=true → запустить constant folding + DCE.
std::string lower(std::string_view source, std::string_view test_name,
                  bool require_main = false, bool optimize = false) {
    auto src = SourceFile{std::string(test_name), std::string(source)};
    DiagnosticSink sink;
    Lexer lex(src, sink);
    auto toks = lex.tokenize();
    if (!toks) {
        std::ostringstream os; sink.print_all(os);
        fail(test_name, std::format("lex failed: {}", os.str()));
        return {};
    }
    Parser p(*toks, src.name(), sink);
    auto prog = p.parse_program();
    if (!prog) {
        std::ostringstream os; sink.print_all(os);
        fail(test_name, std::format("parse failed: {}", os.str()));
        return {};
    }
    SemanticAnalyzer sema(*prog, src.name(), sink, {},
                          /*require_main=*/require_main);
    if (!sema.analyze()) {
        std::ostringstream os; sink.print_all(os);
        fail(test_name, std::format("semantic failed: {}", os.str()));
        return {};
    }
    Lowerer lw(*prog, sema);
    auto mod = lw.lower();
    if (optimize) herta::ir::optimize_module(mod);
    std::ostringstream out;
    herta::ir::dump_module(mod, out);
    return out.str();
}

void contains(std::string_view test, const std::string& ir,
              std::string_view needle) {
    if (ir.find(needle) == std::string::npos) {
        fail(test, std::format("expected substring '{}' in IR:\n{}", needle, ir));
    }
}
void not_contains(std::string_view test, const std::string& ir,
                  std::string_view needle) {
    if (ir.find(needle) != std::string::npos) {
        fail(test, std::format("unexpected substring '{}' in IR:\n{}", needle, ir));
    }
}

void test_smoke_hello() {
    auto ir = lower(R"(
        module m;
        fn main() int32 {
            print("hi");
            return 0;
        }
    )", "hello", /*main=*/true);
    contains("hello", ir, "module m");
    contains("hello", ir, "fn main()");
    contains("hello", ir, "call print(\"hi\")");
    contains("hello", ir, "return 0");
}

void test_arithmetic() {
    auto ir = lower(R"(
        module m;
        fn f(a: int32, b: int32) int32 { return a + b * 2 - 1; }
    )", "arith");
    contains("arith", ir, "mul ");
    contains("arith", ir, "add ");
    contains("arith", ir, "sub ");
    contains("arith", ir, "return");
}

void test_if_else() {
    auto ir = lower(R"(
        module m;
        fn f(x: int32) int32 {
            if x < 0 { return -x; } else { return x; }
        }
    )", "if");
    contains("if", ir, "lt ");
    contains("if", ir, "neg ");
    contains("if", ir, "if %t");
    contains("if", ir, "goto L");
}

void test_while_and_break() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            var i: int32 = 0;
            while i < 10 {
                if i == 5 { break; }
                i = i + 1;
            }
            return i;
        }
    )", "while");
    contains("while", ir, "lt ");
    contains("while", ir, "eq ");
    contains("while", ir, "goto L");
    // break → goto end label of the while
}

void test_short_circuit_and() {
    auto ir = lower(R"(
        module m;
        fn f(a: bool, b: bool) bool { return a && b; }
    )", "and");
    // && lowers to branch — должен быть Branch на промежуточный label
    contains("and", ir, "if ");
    contains("and", ir, "goto L");
}

void test_short_circuit_or() {
    auto ir = lower(R"(
        module m;
        fn f(a: bool, b: bool) bool { return a || b; }
    )", "or");
    contains("or", ir, "if ");
    contains("or", ir, "goto L");
}

void test_array_and_index() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            var a: [int32; 3] = [10, 20, 30];
            a[1] = 99;
            return a[2];
        }
    )", "array");
    contains("array", ir, "array int32 [10, 20, 30]");
    contains("array", ir, "a[1] = 99");
    contains("array", ir, "index a, 2");
}

void test_struct_and_field() {
    auto ir = lower(R"(
        module m;
        struct P { x: int32, y: int32 }
        fn f() int32 {
            var p: P = P { x: 1, y: 2 };
            p.x = 5;
            return p.y;
        }
    )", "struct");
    contains("struct", ir, "struct P {x: 1, y: 2}");
    contains("struct", ir, "p.x = 5");
    contains("struct", ir, "field p, \"y\"");
}

void test_cast() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            let x: float64 = 3.5;
            let y: int32 = int32(x);
            return y;
        }
    )", "cast");
    contains("cast", ir, "cast int32 x");
}

void test_methods() {
    auto ir = lower(R"(
        module m;
        struct P { x: int32 }
        impl P {
            fn make(v: int32) P { return P { x: v }; }
            fn get(self: P) int32 { return self.x; }
        }
        fn main() int32 {
            let p: P = P.make(7);
            return p.get();
        }
    )", "methods", /*main=*/true);
    contains("methods", ir, "fn P.make(v: int32) -> P");
    contains("methods", ir, "fn P.get(self: P) -> int32");
    contains("methods", ir, "call P.make(7)");
    // instance method: self передаётся первым аргументом, имя `P.get`
    contains("methods", ir, "call P.get(p)");
}

void test_namespace_call() {
    auto ir = lower(R"(
        module m;
        namespace Utils {
            fn dbl(x: int32) int32 { return x + x; }
        }
        fn main() int32 {
            return Utils.dbl(21);
        }
    )", "namespace", /*main=*/true);
    contains("namespace", ir, "fn Utils.dbl(x: int32) -> int32");
    contains("namespace", ir, "call Utils.dbl(21)");
}

void test_string_concat() {
    auto ir = lower(R"(
        module m;
        fn f() string { return "ab" + "cd"; }
    )", "concat");
    contains("concat", ir, "concat ");
}

// Тесты оптимизатора: constant folding, propagation, DCE.

void test_fold_int_arith() {
    auto ir = lower(R"(
        module m;
        fn f() int32 { return 1 + 2 * 3 - 4; }
    )", "fold_int", /*main=*/false, /*opt=*/true);
    // 1 + 6 - 4 = 3 — должен остаться только return 3
    contains("fold_int", ir, "return 3");
    not_contains("fold_int", ir, "add ");
    not_contains("fold_int", ir, "mul ");
    not_contains("fold_int", ir, "sub ");
}

void test_fold_comparison() {
    auto ir = lower(R"(
        module m;
        fn f() bool { return 5 < 10; }
    )", "fold_cmp", /*main=*/false, /*opt=*/true);
    contains("fold_cmp", ir, "return true");
    not_contains("fold_cmp", ir, "lt ");
}

void test_fold_div_by_zero_not_folded() {
    auto ir = lower(R"(
        module m;
        fn f() int32 { return 10 / 0; }
    )", "fold_div0", /*main=*/false, /*opt=*/true);
    // Деление на ноль не сворачивается, оно должно дойти до рантайма как ошибка.
    contains("fold_div0", ir, "div 10, 0");
}

void test_fold_unary_neg() {
    auto ir = lower(R"(
        module m;
        fn f() int32 { return -5; }
    )", "fold_neg", /*main=*/false, /*opt=*/true);
    contains("fold_neg", ir, "return -5");
    not_contains("fold_neg", ir, "neg ");
}

void test_fold_logical_not() {
    auto ir = lower(R"(
        module m;
        fn f() bool { return !true; }
    )", "fold_not", /*main=*/false, /*opt=*/true);
    contains("fold_not", ir, "return false");
    not_contains("fold_not", ir, "not ");
}

void test_fold_branch_with_true() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            if true { return 1; } else { return 2; }
        }
    )", "fold_br_true", /*main=*/false, /*opt=*/true);
    // Branch с константой true → Goto на then.
    not_contains("fold_br_true", ir, "if ");
    contains("fold_br_true", ir, "goto L");
}

void test_propagate_let_const() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            let x: int32 = 42;
            let y: int32 = x + 8;
            return y;
        }
    )", "prop_let", /*main=*/false, /*opt=*/true);
    contains("prop_let", ir, "return 50");
}

void test_dce_removes_unused_temp() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            let unused: int32 = 1 + 2;
            return 0;
        }
    )", "dce_unused", /*main=*/false, /*opt=*/true);
    // 1+2=3, потом x=3, потом x не используется — DCE удаляет.
    not_contains("dce_unused", ir, "unused");
    contains("dce_unused", ir, "return 0");
}

void test_fold_cast_int_to_float() {
    auto ir = lower(R"(
        module m;
        fn f() float64 { return float64(5); }
    )", "fold_cast", /*main=*/false, /*opt=*/true);
    not_contains("fold_cast", ir, "cast ");
    contains("fold_cast", ir, "return 5");
}

void test_fold_string_concat() {
    auto ir = lower(R"(
        module m;
        fn f() string { return "ab" + "cd"; }
    )", "fold_concat", /*main=*/false, /*opt=*/true);
    not_contains("fold_concat", ir, "concat ");
    contains("fold_concat", ir, "return \"abcd\"");
}

void test_var_reassigned_not_propagated() {
    auto ir = lower(R"(
        module m;
        fn f() int32 {
            var x: int32 = 5;
            x = x + 1;
            return x;
        }
    )", "var_reassign", /*main=*/false, /*opt=*/true);
    // x присваивается дважды (5, потом x+1) → не пропагируется.
    // В IR должны остаться обращения к x.
    contains("var_reassign", ir, " x");
}

void test_void_call_no_dst() {
    auto ir = lower(R"(
        module m;
        fn f() void { return; }
        fn main() int32 { f(); return 0; }
    )", "void", /*main=*/true);
    // void-вызов не должен присваивать в temp
    not_contains("void", ir, "= call f");
    contains("void", ir, "call f()");
}

}  // anonymous

int main() {
    test_smoke_hello();
    test_arithmetic();
    test_if_else();
    test_while_and_break();
    test_short_circuit_and();
    test_short_circuit_or();
    test_array_and_index();
    test_struct_and_field();
    test_cast();
    test_methods();
    test_namespace_call();
    test_string_concat();
    test_void_call_no_dst();

    // optimizer
    test_fold_int_arith();
    test_fold_comparison();
    test_fold_div_by_zero_not_folded();
    test_fold_unary_neg();
    test_fold_logical_not();
    test_fold_branch_with_true();
    test_propagate_let_const();
    test_dce_removes_unused_temp();
    test_fold_cast_int_to_float();
    test_fold_string_concat();
    test_var_reassigned_not_propagated();

    if (failures == 0) {
        std::cout << "all IR tests passed\n";
        return 0;
    }
    std::cerr << failures << " IR test(s) failed\n";
    return 1;
}
