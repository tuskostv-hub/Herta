// Тесты multi-module компиляции. Создают временные .herta-файлы и
// прогоняют их через Driver.

import std;
import herta.common;
import herta.driver;

namespace {

int failures = 0;

void fail(std::string_view test, std::string_view detail) {
    std::cerr << "FAIL [" << test << "] " << detail << '\n';
    ++failures;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        auto base = std::filesystem::temp_directory_path() / "herta_test_modules";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);
        path = base;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::ofstream f(p);
    f << content;
}

// Прогоняет компиляцию корневого файла. Возвращает true если успешно.
bool compile_root(const std::filesystem::path& root) {
    herta::common::DiagnosticSink sink;
    herta::driver::Driver drv(sink);
    bool ok = drv.compile(root);
    return ok && !sink.has_errors();
}

void ok(const std::filesystem::path& root, std::string_view test) {
    if (!compile_root(root)) fail(test, "expected success");
}
void err(const std::filesystem::path& root, std::string_view test) {
    if (compile_root(root)) fail(test, "expected error");
}

}  // namespace

int main() {
    TempDir tmp;

    {
        write_file(tmp.path / "Math.herta", R"(
            module Math;
            pub fn add(a: int32, b: int32) int32 { return a + b; }
        )");
        write_file(tmp.path / "main_ok.herta", R"(
            module main_ok;
            import Math;
            fn main() int32 { return Math.add(1, 2); }
        )");
        ok(tmp.path / "main_ok.herta", "basic two-module");
    }

    {
        write_file(tmp.path / "Lib.herta", R"(
            module Lib;
            fn priv() int32 { return 1; }
            pub fn pub_fn() int32 { return 2; }
        )");
        write_file(tmp.path / "main_pub_ok.herta", R"(
            module main_pub_ok;
            import Lib;
            fn main() int32 { return Lib.pub_fn(); }
        )");
        ok(tmp.path / "main_pub_ok.herta", "pub fn accessible");
        write_file(tmp.path / "main_priv.herta", R"(
            module main_priv;
            import Lib;
            fn main() int32 { return Lib.priv(); }
        )");
        err(tmp.path / "main_priv.herta", "private fn not accessible");
    }

    {
        write_file(tmp.path / "Geom.herta", R"(
            module Geom;
            pub struct Point { x: int32, y: int32, }
            impl Point {
                pub fn make(x: int32, y: int32) Point {
                    return Point { x: x, y: y };
                }
                pub fn sum(self: Point) int32 { return self.x + self.y; }
            }
        )");
        write_file(tmp.path / "main_geom.herta", R"(
            module main_geom;
            import Geom;
            fn main() int32 {
                let p: Geom.Point = Geom.Point.make(3, 4);
                return p.sum();
            }
        )");
        // Здесь синтаксис `Geom.Point` как тип не поддерживается парсером
        // (тип-выражение — только NamedType с одним идентификатором).
        // Поэтому ожидаем ошибку — это known limitation, документирую в plan.
        err(tmp.path / "main_geom.herta", "qualified type name not supported");
    }

    {
        write_file(tmp.path / "A.herta", R"(
            module A;
            import B;
            pub fn a() int32 { return 1; }
        )");
        write_file(tmp.path / "B.herta", R"(
            module B;
            import A;
            pub fn b() int32 { return 2; }
        )");
        err(tmp.path / "A.herta", "circular import");
    }

    {
        write_file(tmp.path / "main_nx.herta", R"(
            module main_nx;
            import NotExist;
            fn main() int32 { return 0; }
        )");
        err(tmp.path / "main_nx.herta", "missing module");
    }

    {
        write_file(tmp.path / "wrong_name.herta", R"(
            module Different;
            fn main() int32 { return 0; }
        )");
        err(tmp.path / "wrong_name.herta", "module name mismatch");
    }

    {
        write_file(tmp.path / "Cdep.herta", R"(
            module Cdep;
            pub fn v() int32 { return 42; }
        )");
        write_file(tmp.path / "Bdep.herta", R"(
            module Bdep;
            import Cdep;
            pub fn b() int32 { return Cdep.v(); }
        )");
        write_file(tmp.path / "main_trans.herta", R"(
            module main_trans;
            import Bdep;
            fn main() int32 { return Bdep.b(); }
        )");
        ok(tmp.path / "main_trans.herta", "transitive import");
    }

    if (failures == 0) {
        std::cout << "all driver tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
