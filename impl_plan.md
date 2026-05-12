# План реализации компилятора Herta — этапы 1-2

Документ-плана для пары «человек + Claude». Содержит структуру проекта и подробный план реализации лексера. Каждый шаг — компактный, проверяемый, с чек-листом. Имя исполняемого файла компилятора: **`myc`**.

## Реализуемые «Доп» из ТЗ

| Доп                                  | Решение                              |
|--------------------------------------|--------------------------------------|
| Вывод типов (`:=`)                   | **Да** — синтаксис и семантика в specs |
| Неявное приведение типов (widening)  | **Да** — `types.md §5.4`              |
| Методы структур (`impl`)             | **Да** — `grammar.md §3.2.5`, `semantics.md §13.1` |
| IR + оптимизации (constant folding)  | **Да** — отдельная фаза между semantic и codegen (см. §3 этого плана) |
| Остальные (generics, ADT, лямбды, модули, перегрузка) | Нет |

---

## Этап 1. Базовая структура проекта

### 1.1 Целевая структура каталогов

```
Herta/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── projectv0.1.md              # ТЗ (есть)
├── impl_plan.md                # этот файл
├── report.md                   # отчёт (пишется параллельно с разработкой)
├── specs/                      # спецификация языка (есть)
│   ├── grammar.md
│   ├── semantics.md
│   ├── types.md
│   └── codegen.md              # будет добавлен на этапе кодогенерации
├── examples/                   # примеры программ .herta
│   ├── hello.herta
│   ├── arith.herta
│   ├── bubble_sort.herta
│   └── ...
├── inc/                        # публичные заголовки компилятора (.hpp)
│   └── herta/
│       ├── common/
│       │   ├── source_location.hpp  # struct SourceLocation { file, line, col, offset }
│       │   ├── diagnostic.hpp       # struct Diagnostic + класс DiagnosticSink
│       │   ├── source_file.hpp      # struct SourceFile: содержимое + имя
│       │   └── expected.hpp         # alias-обёртки, если нужны
│       ├── lexer/
│       │   ├── token.hpp            # enum TokenKind, struct Token
│       │   └── lexer.hpp            # class Lexer (herta::lexer::)
│       ├── parser/                  # пустые stubs для следующих этапов
│       │   └── .gitkeep
│       ├── semantic/
│       │   └── .gitkeep
│       └── codegen/
│           └── .gitkeep
├── src/                        # реализация (.cpp)
│   ├── main.cpp                # точка входа: парсинг CLI, оркестрация фаз
│   ├── common/
│   │   ├── diagnostic.cpp
│   │   └── source_file.cpp
│   ├── lexer/
│   │   ├── token.cpp           # to_string(TokenKind), to_string(Token)
│   │   └── lexer.cpp
│   ├── parser/
│   │   └── .gitkeep
│   ├── semantic/
│   │   └── .gitkeep
│   └── codegen/
│       └── .gitkeep
└── tests/                      # тесты (рекомендуется с самого начала)
    ├── CMakeLists.txt
    └── lexer/
        ├── test_tokens.cpp
        └── fixtures/           # .herta-файлы и ожидаемые .tokens
```

> Разделение `inc/` ↔ `src/`: заголовки публичные для всех фаз компилятора и тестов, реализация скрыта в `src/`. Включения в коде делаются как `#include "herta/lexer/lexer.hpp"` — однозначно и без относительных путей.

> Тесты: используем простой self-contained подход без сторонних библиотек (т.к. они под запретом без согласования) — функции `assert_*` в `tests/test_main.cpp` либо `ctest add_test` с golden-сравнением через `diff`. Окончательное решение — в шаге 1.7.

### 1.2 Конвенции

- **Компилятор:** `g++` (≥ 13 для нормальной поддержки C++23).
- **Стандарт:** C++23. Флаги: `-std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- **Namespace per phase:** `herta::lexer`, `herta::parser`, `herta::semantic`, `herta::codegen`, `herta::common`. Корневой `herta::` оборачивает всё.
- **Стиль:** `snake_case` для функций/переменных, `PascalCase` для типов/enum, `kPascalCase` или `UPPER_SNAKE` для констант (выбираем `kPascalCase` для enum-значений, чтобы соответствовать ровным `TokenKind::KwFn`).
- **Файлы:** заголовки `.hpp` в `inc/herta/<phase>/`, реализация `.cpp` в `src/<phase>/`. В заголовках `#pragma once`. Включения — через корень `inc/`: `#include "herta/lexer/lexer.hpp"`.
- **Ошибки:** `std::expected<T, Diagnostic>` для нефатальных, `std::vector<Diagnostic>` для накопления; исключения — только для внутренних UB-ситуаций. Без глобального state ошибок.

### 1.3 CMakeLists.txt — каркас

Шаги:
1. `cmake_minimum_required(VERSION 3.25)` (нужно для нормального C++23).
2. `project(herta CXX)`.
3. `set(CMAKE_CXX_STANDARD 23)`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`.
4. Опции: `option(HERTA_BUILD_TESTS "Build tests" ON)`.
5. `add_library(herta_core STATIC ...)` — все фазы как библиотека.
   - `target_include_directories(herta_core PUBLIC ${CMAKE_SOURCE_DIR}/inc)` — публичный include-путь.
   - Источники из `src/**/*.cpp` (кроме `main.cpp`).
6. `add_executable(myc src/main.cpp)`; `target_link_libraries(myc PRIVATE herta_core)`.
7. Цели **`build`** (обычно дефолтная), **`run`**, **`debug`**, **`clean`** — `clean` есть по умолчанию; `run`/`debug` добавляем через `add_custom_target` или Makefile-обёртку.
   - `add_custom_target(run COMMAND myc ${HERTA_RUN_ARGS} DEPENDS myc)`
   - `add_custom_target(debug COMMAND ${CMAKE_COMMAND} --build . --config Debug)`
8. Если `HERTA_BUILD_TESTS` → `enable_testing()`, `add_subdirectory(tests)`.

### 1.4 .gitignore — каркас

```
build/
cmake-build-*/
*.o
*.obj
*.a
*.exe
.vscode/
.idea/
.cache/
compile_commands.json
```

### 1.5 README.md — минимум

Разделы: что такое Herta (одна фраза), статус, требования (cmake ≥ 3.25, `g++` ≥ 13 с поддержкой C++23), команды сборки/запуска, ссылка на `specs/`.

### 1.6 Чек-лист этапа 1

- [ ] Создать каталоги: `inc/herta/{common,lexer,parser,semantic,codegen}`, `src/{common,lexer,parser,semantic,codegen}`, `examples/`, `tests/lexer/fixtures/`.
- [ ] Положить `.gitkeep` в пустые `inc/herta/{parser,semantic,codegen}` и `src/{parser,semantic,codegen}`.
- [ ] Написать `CMakeLists.txt` (корень и `tests/`).
- [ ] Написать `.gitignore`, минимальный `README.md`.
- [ ] Создать пустые модули: заголовки `inc/herta/common/{source_location.hpp,diagnostic.hpp,source_file.hpp}` + реализации `src/common/{diagnostic.cpp,source_file.cpp}` со скелетом классов.
- [ ] `src/main.cpp` — заглушка: парсит `argv`, читает файл, **пока ничего не делает**, возвращает 0.
- [ ] Убедиться, что `cmake -S . -B build && cmake --build build` проходит без ошибок и предупреждений.
- [ ] Прогнать `./build/myc examples/hello.herta` — заглушка не должна падать.
- [ ] Первый коммит структуры.

---

## Этап 2. Реализация лексера

### 2.1 Что именно должен распознавать лексер (из grammar.md §2)

**Ключевые слова (синтаксические):**
```
fn  let  var  return  if  else  while  break  continue  struct  type  namespace  impl  true  false
```

(15 ключевых слов. `impl` добавлен для методов структур — см. `grammar.md §3.2.5`.)

> Примечание про `print` / `input` / `exit` / `panic` и имена базовых типов (`int8`..`int64`, `uint8`..`uint64`, `float32`, `float64`, `bool`, `string`, `void`):
> формально в §2.1 grammar `print/input/exit/panic` помечены как keywords, а имена типов — нет. Чтобы не плодить десятки `TokenKind`, **в лексере** они все тегируются как обычный `Identifier`. Их «зарезервированность» обеспечит семантический анализатор (запретит переопределение). Это решение нужно отразить в `report.md`.
>
> `self` — обычный идентификатор, не keyword. «Особость» проявляется только в позиции первого параметра метода в `impl`-блоке (см. semantics.md §13.1).

**Литералы:**
- `IntLiteral` — `decimal_lit` (`[0-9]+`) и `hex_lit` (`0x[0-9a-fA-F]+`).
- `FloatLiteral` — `[0-9]+ "." [0-9]+ (("e"|"E") ("+"|"-")? [0-9]+)?`.
  Важно: `2.0e10` — float, но `2e10` без точки — **не** float (по грамматике).
- `StringLiteral` — `"..."` с escape-последовательностями `\" \\ \n \t \r`. Не допускается перевод строки внутри.
- `true` / `false` → отдельные токены `KwTrue` / `KwFalse` (хранятся как ключевые слова).

**Идентификаторы:** `letter (letter | digit)*`, где `letter = [A-Za-z_]`, `digit = [0-9]`.

**Операторы и разделители (мульти-символьные имеют приоритет — matching по принципу maximal munch):**
```
==  !=  <=  >=  &&  ||  ->  :=
+   -   *   /   %
<   >   =   !
:   ,   ;   .
(   )   {   }   [   ]
```

**Комментарии:** `// ... \n` — поглощаются, не порождают токенов.

**Whitespace:** ` \t \r \n` — поглощается, считается переводов строк для `line`.

**EOF:** отдельный токен `Eof`, выдаётся ровно один раз в конце.

### 2.2 Структуры данных

```cpp
// inc/herta/common/source_location.hpp
namespace herta::common {
struct SourceLocation {
    std::uint32_t line = 1;     // 1-based
    std::uint32_t column = 1;   // 1-based, в кодовых точках UTF-8 нам пока пофиг — считаем байты
    std::uint32_t offset = 0;   // байтовое смещение от начала файла
};
}

// inc/herta/lexer/token.hpp
namespace herta::lexer {
enum class TokenKind : std::uint8_t {
    // Спец
    Eof, Invalid,
    // Литералы
    IntLiteral, FloatLiteral, StringLiteral,
    Identifier,
    // Ключевые слова
    KwFn, KwLet, KwVar, KwReturn,
    KwIf, KwElse, KwWhile, KwBreak, KwContinue,
    KwStruct, KwType, KwNamespace, KwImpl,
    KwTrue, KwFalse,
    // Операторы
    Plus, Minus, Star, Slash, Percent,
    EqEq, BangEq, Lt, Gt, LtEq, GtEq,
    AmpAmp, PipePipe, Bang,
    Eq, ColonEq, Arrow,
    // Разделители
    Colon, Comma, Semicolon, Dot,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
};

struct Token {
    TokenKind kind;
    std::string_view lexeme;         // указывает в исходный буфер
    herta::common::SourceLocation loc;
    // Опционально — уже разобранные значения, чтобы парсер не парсил повторно:
    // std::variant<std::monostate, std::int64_t, double, std::string> value;
};
}
```

Решение по `value`: на этом этапе **не** разбираем числовые значения в лексере — оставляем как `lexeme`, разбор сделает парсер/семантика. Это упрощает лексер. Строковый литерал — храним `lexeme` с кавычками; раскрытие escape-последовательностей делается отложенно при формировании AST.

### 2.3 Интерфейс лексера

```cpp
namespace herta::lexer {

class Lexer {
public:
    // Принимает SourceFile (не владеет — ссылка должна жить дольше).
    explicit Lexer(const herta::common::SourceFile& src,
                   herta::common::DiagnosticSink& sink);

    // Возвращает все токены до Eof включительно. При ошибках добавляет
    // диагностику в sink и завершает работу (по ТЗ — остановка на первой ошибке).
    // Возвращает std::expected<std::vector<Token>, std::monostate>:
    // ошибка маркирует, что были ошибки (детали в sink).
    std::expected<std::vector<Token>, std::monostate> tokenize();

private:
    Token next_token();
    Token make(TokenKind k, std::size_t start_offset, std::size_t start_line, std::size_t start_col);
    char peek(std::size_t lookahead = 0) const;
    char advance();
    bool match(char c);                          // если peek() == c, advance() и true
    void skip_whitespace_and_comments();

    Token scan_identifier_or_keyword();
    Token scan_number();
    Token scan_string();
    Token scan_punct_or_op();

    void error(std::string_view msg);

    const herta::common::SourceFile& src_;
    herta::common::DiagnosticSink& sink_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
    bool fatal_ = false;
};

} // namespace herta::lexer
```

> Возврат `std::vector<Token>` за раз, а не итератор/корутина: проще для парсера, для учебного компилятора этого хватит. Если позже захочется stream — переделаем интерфейс, парсер не пострадает.

### 2.4 Алгоритм `next_token` (псевдокод)

```
1. skip_whitespace_and_comments()
2. start = (pos, line, col)
3. if pos >= size: return Token{Eof, "", start}
4. c = peek()
5. if is_letter(c): return scan_identifier_or_keyword()
6. if is_digit(c):  return scan_number()
7. if c == '"':     return scan_string()
8. return scan_punct_or_op()
```

**Maximal munch для операторов:** в `scan_punct_or_op` для каждого «двухсимвольного» оператора проверяем оба символа подряд, иначе откатываемся к односимвольному:
```
=  → если следующий '=' → EqEq, иначе Eq
!  → если следующий '=' → BangEq, иначе Bang
<  → если следующий '=' → LtEq,  иначе Lt
>  → если следующий '=' → GtEq,  иначе Gt
&  → если следующий '&' → AmpAmp, иначе ошибка ("expected '&&', got '&'")
|  → если следующий '|' → PipePipe, иначе ошибка
-  → если следующий '>' → Arrow,  иначе Minus
:  → если следующий '=' → ColonEq, иначе Colon
/  → если следующий '/' → это комментарий (но мы сюда не попадём, т.к. съели в skip)
```

**`scan_number`:**
```
1. Если начинается с "0x" / "0X": читаем hex_digit+ → IntLiteral.
2. Иначе читаем digit+. Если следующий — '.' и за ним digit:
   - читаем "." digit+
   - опционально 'e'/'E', опционально '+'/'-', digit+ (обязательно ≥1)
   - → FloatLiteral
3. Иначе → IntLiteral.
4. Граничный случай: "0xZ" / "0x" без цифр → ошибка "invalid hex literal".
5. "1.e10" — по грамматике не допускается (за точкой обязана быть цифра).
   "1." без цифры за точкой — рассматриваем как IntLiteral "1" и затем токен Dot;
   парсер либо распознает это как доступ к полю, либо упадёт. Это соответствует
   "maximal munch" применённому к корректным float-литералам.
```

**`scan_string`:**
```
1. съесть открывающую кавычку
2. цикл: пока не встретили '"':
   - если EOF или '\n' → ошибка "unterminated string literal"
   - если '\\': прочитать следующий символ; допустимы: " \ n t r; иначе — ошибка
   - иначе: пропустить байт
3. съесть закрывающую кавычку
4. lexeme = [start, end) включая кавычки
```

**`skip_whitespace_and_comments`:** цикл — пока `peek()` это пробел/таб/`\r`/`\n` или начинается `//`. На `\n` инкрементируем `line_`, обнуляем `col_`.

### 2.5 Диагностика

Формат сообщений жёстко фиксируется (по ТЗ §"Диагностика ошибок"):
```
<file>:<line>:<column>: error: <message>
```

`DiagnosticSink` минимальный:
```cpp
struct Diagnostic {
    SourceLocation loc;
    std::string message;
    std::string file;
};
class DiagnosticSink {
public:
    void report(Diagnostic d);
    bool has_errors() const noexcept;
    void print_all(std::ostream& os) const;  // в stderr
    std::span<const Diagnostic> diagnostics() const noexcept;
private:
    std::vector<Diagnostic> diags_;
};
```

Лексер при ошибке: вызывает `sink_.report(...)`, ставит `fatal_ = true`, возвращает `Token{Invalid, ...}`. После выхода из `tokenize()` `main.cpp` проверяет `sink.has_errors()` → печатает и выходит с кодом ≠0.

### 2.6 CLI и `--dump-tokens`

`main.cpp`:
```
1. parse args:
   - position 1: <source>
   - -o <file>          (на будущее)
   - --dump-tokens      (флаг; работает на этом этапе)
   - --dump-ast         (на будущее — пока no-op или "not implemented")
2. load SourceFile.
3. Lexer lex(src, sink); auto tokens = lex.tokenize();
4. if (--dump-tokens) {
       печать каждого токена в формате:
         <line>:<col>  <KIND>  '<lexeme>'
       завершение работы независимо от наличия ошибок (предварительно вывести
       диагностику в stderr, если есть).
   }
5. if sink.has_errors(): print to stderr, return 1.
6. return 0.
```

### 2.7 Чек-лист этапа 2

- [ ] `inc/herta/common/source_file.hpp` + `src/common/source_file.cpp` — чтение файла в `std::string`, хранение имени; helper `line_for_offset` (на будущее, для подсветки).
- [ ] `inc/herta/common/diagnostic.hpp` + `src/common/diagnostic.cpp` — `Diagnostic`, `DiagnosticSink`, форматирование.
- [ ] `inc/herta/lexer/token.hpp` + `src/lexer/token.cpp` — `TokenKind`, `Token`, `to_string(TokenKind)`.
- [ ] `inc/herta/lexer/lexer.hpp` + `src/lexer/lexer.cpp` — реализация `Lexer::tokenize()` по алгоритму выше.
- [ ] Таблица ключевых слов: `static const std::unordered_map<std::string_view, TokenKind>` либо `if`-каскад / `constexpr` массив (для 15 ключевых слов — массив с линейным поиском оптимален).
- [ ] `main.cpp` — поддержка `--dump-tokens` и базовая обработка CLI.
- [ ] Подключить `src/lexer/*.cpp` в `CMakeLists.txt` (в `herta_core`).

### 2.8 Тесты лексера

Минимум — golden-тесты на `examples/`:

- [ ] `tests/lexer/test_tokens.cpp` — функция: для каждого `.herta`-файла в фикстурах прогоняет лексер и сравнивает с `.tokens`-файлом.
- [ ] Фикстуры:
  - `empty.herta` → только `Eof`.
  - `keywords.herta` → каждое ключевое слово по разу.
  - `idents.herta` → `x`, `_foo`, `Bar2`, `__`.
  - `ints.herta` → `0`, `42`, `1000`, `0xFF`, `0x0`, `0xAbCdEf`.
  - `floats.herta` → `3.14`, `0.0`, `1.5e10`, `2.0E-3`, `1.23e+5`.
  - `strings.herta` → `"hello"`, `"a\nb"`, `"tab\there"`, `"\\"`, `"\""`.
  - `ops.herta` → все операторы и разделители.
  - `comments.herta` → строки с `//`, гарантировать отсутствие токенов от них.
  - `full.herta` → `examples/bubble_sort.herta` целиком.
  - **Ошибки** — отдельные файлы, ожидаемая диагностика:
    - `err_unterminated_string.herta` — `"hello`
    - `err_bad_escape.herta` — `"\q"`
    - `err_bad_hex.herta` — `0x`
    - `err_lone_amp.herta` — `a & b`
    - `err_newline_in_string.herta` — `"a` + `\n` + `b"`
    - `err_invalid_char.herta` — `@`

- [ ] Регистрация целей в CTest: `add_test(NAME lexer_tokens COMMAND lexer_tests)`.

### 2.9 Готовность этапа 2 — критерий

- `cmake --build build && ctest --test-dir build` проходит без падений.
- `./build/myc examples/bubble_sort.herta --dump-tokens` печатает ожидаемый поток токенов.
- Для каждого `err_*.herta` лексер выдаёт сообщение в формате `file:line:col: error: ...` и возвращает ≠0.
- Никаких сторонних библиотек, только STL.

---

## Что дальше (превью)

Полный pipeline с учётом «Доп»:

```
исходный код → [Lexer] → [Parser] → [Semantic] → [Lowering→IR] → [Optimizer] → [Codegen] → exe
```

- **Этап 3 — парсер.** Recursive Descent, отдельный subparser для выражений по таблице приоритетов §3.4 grammar. Парсит и `impl`-блоки (просто список `fn_decl` внутри).
- **Этап 4 — семантика.** Символьные таблицы со scope-стеком, type checker с реализацией неявных приведений (§5.4 types.md), проверка mutability (`let`/`var`), резолвинг методов (lookup по типу `obj`/`T`).
- **Этап 5 — IR (lowering).** Линейный трёхадресный код: `t = a OP b`, `t = call f(args)`, `goto L`, `if t goto L`, `label L`. Печать в текстовом виде для отладки (`--dump-ir`).
- **Этап 6 — оптимизатор IR.** На старте — **constant folding** (свёртка константных выражений). По возможности — простой DCE (dead code elimination).
- **Этап 7 — кодогенерация.** NASM x86-64 либо LLVM IR — решение фиксируем в `specs/codegen.md` до начала этапа. Соглашение о вызовах — System V AMD64 ABI.

Каждый из этапов 3-7 будет детализирован в этом плане по мере завершения предыдущего.
