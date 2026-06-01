# Herta

Учебный компилятор статически типизированного языка программирования. Синтаксис
вдохновлён Go, Rust и Odin. Программа компилируется в трёхадресный IR и затем
либо переводится в LLVM IR и собирается clang-ом в нативный x86-64 бинарь
(путь по умолчанию), либо исполняется встроенной регистровой ВМ (`--interp`).

Полная спецификация языка — в каталоге [specs/](specs/):
- [grammar.md](specs/grammar.md) — лексика и синтаксис (EBNF)
- [semantics.md](specs/semantics.md) — семантика конструкций
- [types.md](specs/types.md) — система типов
- [codegen.md](specs/codegen.md) — модель исполнения (интерпретатор IR)

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
./build/myc <source.herta>                # скомпилировать в нативный бинарь
./build/myc <source.herta> -o prog        # … с указанием имени
./build/myc <source.herta> --run          # скомпилировать и сразу запустить
./build/myc <source.herta> --interp       # исполнить через встроенную ВМ
./build/myc <source.herta> --emit-llvm    # вывести .ll в stdout
./build/myc <source.herta> --dump-tokens  # поток токенов
./build/myc <source.herta> --dump-ast     # AST
./build/myc <source.herta> --dump-ir      # трёхадресный IR (после оптимизаций)
./build/myc <source.herta> --dump-ir --no-opt   # … без constant folding / DCE
```

Код завершения исполненной программы — значение, возвращённое из `main`.

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
