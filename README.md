# Herta

Учебный компилятор статически типизированного языка программирования. Синтаксис
вдохновлён Go, Rust и Odin. Целевая платформа — x86-64.

Полная спецификация языка — в каталоге [specs/](specs/):
- [grammar.md](specs/grammar.md) — лексика и синтаксис (EBNF)
- [semantics.md](specs/semantics.md) — семантика конструкций
- [types.md](specs/types.md) — система типов

План разработки — [impl_plan.md](impl_plan.md).

## Требования

- CMake ≥ 3.25
- `g++` ≥ 13 (поддержка C++23)
- POSIX-совместимая ОС (Linux, WSL)

## Сборка

```bash
cmake -S . -B build
cmake --build build
```

Исполняемый файл компилятора — `build/myc`.

## Запуск

```bash
./build/myc <source.herta>             # компиляция
./build/myc <source.herta> --dump-tokens  # вывести поток токенов
./build/myc <source.herta> --dump-ast     # вывести AST
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
tests/       — unit и golden тесты
```
