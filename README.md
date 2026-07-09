# Herta

Учебный компилятор статически типизированного языка программирования. Синтаксис
вдохновлён Go, Rust и Odin. Программа компилируется в трёхадресный IR и затем
переводится в LLVM IR; clang собирает её в нативный x86-64 ELF-бинарь.

Полная спецификация языка — в каталоге [specs/](specs/):
- [grammar.md](specs/grammar.md) — лексика и синтаксис (EBNF)
- [semantics.md](specs/semantics.md) — семантика конструкций
- [types.md](specs/types.md) — система типов
- [codegen.md](specs/codegen.md) — кодогенерация (LLVM IR + clang)

Отчёт о проделанной работе — [report.md](report.md).

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
./build/myc <source.herta>                # скомпилировать в нативный бинарь
./build/myc <source.herta> -o prog        # … с указанием имени
./build/myc <source.herta> --run          # скомпилировать и сразу запустить
./build/myc <source.herta> --emit-llvm    # вывести .ll в stdout
./build/myc <source.herta> --dump-tokens  # поток токенов
./build/myc <source.herta> --dump-ast     # AST
./build/myc <source.herta> --dump-ir      # трёхадресный IR
```

Код завершения исполненной программы — значение, возвращённое из `main`.

`--dump-tokens` и `--dump-ast` работают по одному файлу и не подтягивают
импорты; остальные режимы проходят полный multi-module pipeline.

## Тесты

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## Структура репозитория

```
src/         — реализация (common, lexer, parser, semantic, ir, codegen, driver)
specs/       — спецификация языка
examples/    — примеры программ (dops/ — демонстрации допзаданий)
tests/       — unit-тесты фаз + e2e
report.md    — отчёт о проделанной работе
```
