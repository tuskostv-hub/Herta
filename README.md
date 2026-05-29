# Herta

Учебный компилятор статически типизированного языка программирования. Синтаксис
вдохновлён Go, Rust и Odin. Целевая платформа — x86-64.

Полная спецификация языка — в каталоге [specs/](specs/):
- [grammar.md](specs/grammar.md) — лексика и синтаксис (EBNF)
- [semantics.md](specs/semantics.md) — семантика конструкций
- [types.md](specs/types.md) — система типов

План разработки — [impl_plan.md](impl_plan.md).

## Требования

- CMake ≥ 4.0
- Ninja
- Clang ≥ 22 с libc++ (для `import std;`)
- POSIX-совместимая ОС (Linux, WSL)

Компилятор написан на C++23 и использует модуль стандартной библиотеки
(`import std;`), поэтому набор тулчейна жёстко зафиксирован.

## Сборка

```bash
cmake -G Ninja -S . -B build -DCMAKE_CXX_COMPILER=clang++-22
cmake --build build
```

Исполняемый файл компилятора — `build/myc`.

## Запуск

```bash
./build/myc <source.herta>                # компиляция
./build/myc <source.herta> --dump-tokens  # вывести поток токенов
./build/myc <source.herta> --dump-ast     # вывести AST (на этапе парсера)
./build/myc <source.herta> --dump-ir      # вывести трёхадресный IR (после оптимизаций)
./build/myc <source.herta> --dump-ir --no-opt   # ... без constant folding / DCE
./build/myc <source.herta> -o <output>    # указать имя выходного файла
```

## Тесты

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## Структура репозитория

```
inc/herta/   — публичные заголовки фаз компилятора
src/         — реализация (lexer, parser, semantic, codegen)
specs/       — спецификация языка
examples/    — примеры программ
tests/       — unit-тесты фаз
```
