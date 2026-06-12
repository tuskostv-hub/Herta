// Интеграционные end-to-end тесты компилятора. Каждый кейс компилируется
// в нативный бинарь и запускается через `myc --run`; проверяются stdout и
// код возврата. Подпроцесс нужен потому, что runtime (assert/panic/exit/
// runtime errors) завершают процесс через exit().

#include <stdio.h>
#include <sys/wait.h>

import std;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

struct RunResult {
    std::string output;
    int code = 0;
};

// Пишет source во временный файл и запускает myc --run.
RunResult run(std::string_view name, std::string_view source) {
    auto dir = std::filesystem::temp_directory_path() / "herta_e2e_tests";
    std::filesystem::create_directories(dir);
    auto file = dir / (std::string(name) + ".herta");
    {
        std::ofstream ofs(file);
        ofs << source;
    }
    auto cmd = std::format("{} {} --run 2>&1", MYC_PATH, file.string());
    RunResult r;
    std::FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) { r.code = -1; return r; }
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) r.output += buf;
    int status = pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

// Проверяет, что бинарь даёт ожидаемый код и (опционально) подстроку.
void check(std::string_view test, std::string_view src,
           int want_code, std::string_view want_substr = "") {
    auto r = run(test, src);
    if (r.code != want_code) {
        fail(test, std::format("exit {} (want {}), output: {}",
                                r.code, want_code, r.output));
        return;
    }
    if (!want_substr.empty() && r.output.find(want_substr) == std::string::npos) {
        fail(test, std::format("output '{}' missing '{}'", r.output, want_substr));
    }
}

}  // namespace

int main() {
    check("ret42",
        "module ret42;\nfn main() int32 { return 42; }\n", 42);

    check("arith",
        "module arith;\nfn main() int32 {\n"
        "  let a: int32 = 2 + 3 * 4;\n"
        "  assert(a == 14);\n"
        "  return 0;\n}\n", 0);

    check("print",
        "module print;\nfn main() int32 {\n"
        "  print(7);\n  print(true);\n  return 0;\n}\n", 0, "7\ntrue\n");

    check("divzero",
        "module divzero;\nfn main() int32 {\n"
        "  var a: int32 = 1;\n  var b: int32 = 0;\n"
        "  return a / b;\n}\n", 1, "runtime error: division by zero at line 5");

    check("oob",
        "module oob;\nfn main() int32 {\n"
        "  var a: [int32; 3] = [1, 2, 3];\n"
        "  var i: int32 = 9;\n"
        "  return a[i];\n}\n", 1, "index out of bounds: 9, size 3");

    check("assertfail",
        "module assertfail;\nfn main() int32 {\n"
        "  assert(1 == 2);\n  return 0;\n}\n", 1, "assertion failed at line 3");

    check("panic",
        "module panic;\nfn main() int32 { panic(\"boom\"); return 0; }\n",
        1, "panic: boom");

    check("exitcode",
        "module exitcode;\nfn main() int32 { exit(5); return 0; }\n", 5);

    check("loop",
        "module loop;\nfn main() int32 {\n"
        "  var i: int32 = 1;\n  var s: int32 = 0;\n"
        "  while i <= 5 { s = s + i; i = i + 1; }\n"
        "  assert(s == 15);\n  return 0;\n}\n", 0);

    check("fact",
        "module fact;\n"
        "fn fact(n: int32) int32 {\n"
        "  if n <= 1 { return 1; }\n"
        "  return n * fact(n - 1);\n}\n"
        "fn main() int32 { assert(fact(5) == 120); return 0; }\n", 0);

    check("strings",
        "module strings;\nfn main() int32 {\n"
        "  let s: string = \"ab\" + \"cd\";\n"
        "  assert(s == \"abcd\");\n  assert(len(s) == 4);\n  return 0;\n}\n", 0);

    check("structs",
        "module structs;\nstruct P { x: int32, y: int32 }\n"
        "fn main() int32 {\n"
        "  var p: P = P { x: 1, y: 2 };\n"
        "  var q: P = P { x: 1, y: 2 };\n"
        "  assert(p == q);\n  p.x = 9;\n  assert(p != q);\n  return 0;\n}\n", 0);

    check("valuesem",
        "module valuesem;\n"
        "fn mut(a: [int32; 2]) int32 { a[0] = 99; return a[0]; }\n"
        "fn main() int32 {\n"
        "  var a: [int32; 2] = [1, 2];\n"
        "  assert(mut(a) == 99);\n  assert(a[0] == 1);\n  return 0;\n}\n", 0);

    // Указатели: взятие адреса, разыменование, запись через указатель.
    check("pointers",
        "module pointers;\n"
        "fn main() int32 {\n"
        "  var x: int32 = 42;\n"
        "  var p: *int32 = &x;\n"
        "  assert(*p == 42);\n"
        "  *p = 99;\n"
        "  assert(*p == 99);\n"
        "  return 0;\n"
        "}\n", 0);

    // Литерал null и его проверка в рантайме.
    check("null_pointer",
        "module null_pointer;\n"
        "fn main() int32 {\n"
        "  var p: *int32 = null;\n"
        "  return *p;\n"  // должен упасть с null-deref
        "}\n", 1, "null pointer dereference");

    if (failures == 0) {
        std::cout << "all e2e tests passed\n";
        return 0;
    }
    std::cerr << failures << " e2e test(s) failed\n";
    return 1;
}
