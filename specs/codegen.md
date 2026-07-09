# Генерация кода

## Выбранный путь

Herta — **компилятор в нативный код**: текстовый LLVM IR + clang. Программа
проходит весь pipeline до .ll-файла, который компилятор передаёт clang-у
вместе с C-runtime'ом для получения исполняемого x86-64 ELF-бинаря.

```
Исходник (.herta)
   → [Лексер]            → токены
   → [Парсер]            → AST
   → [Семантика]         → аннотированный AST
   → [Lowering]          → трёхадресный IR              ← herta.ir
   → [LLVM emitter]      → текстовый LLVM IR (.ll)      ← herta.llvm
   → clang -O1           → нативный x86-64 ELF
```

### Обоснование

- **«Бесплатный» нативный код.** LLVM делает всю сложную часть: register
  allocation, SSA-форму, peephole, mem2reg. Мы пишем только высокоуровневый
  маппинг IR → LLVM IR.
- **Текстовая эмиссия, а не C++ API libLLVM.** Тонны заголовков и сложный
  CMake-конфиг не нужны: компилятор пишет `.ll` как строку и зовёт clang
  через `popen`. Проще читать и отлаживать (можно глазами посмотреть `.ll`
  через `--emit-llvm`).
## Модуль эмиттера

[`src/codegen/llvm_emit.cppm`](../src/codegen/llvm_emit.cppm) принимает
`std::vector<ir::Module>` и возвращает строку с текстовым LLVM IR.

### Сборка бинаря

`myc` пишет `.ll` во временный файл и вызывает:

```
clang-22 -O1 -Wno-override-module prog.ll src/codegen/runtime.c -o <out>
```

`-O1` важен: clang проводит `mem2reg`, устраняя alloca'и, которые эмиттер
ставит вместо честной SSA-формы (см. ниже). Без `-O1` код тоже работает,
но сильно медленнее.

### Соглашение о вызовах

System V AMD64 ABI (то, что использует clang на Linux/x86-64). Это даёт
бесплатный интероп с C-runtime'ом: `printf`, `getline`, `malloc` — всё
через обычные сишные сигнатуры.

### Представление типов Herta в LLVM IR

| Herta              | LLVM IR                                |
|--------------------|----------------------------------------|
| `int8..int64`      | `i8 / i16 / i32 / i64`                 |
| `uint8..uint64`    | `i8 / i16 / i32 / i64` (signedness — на операторах) |
| `float32`          | `float`                                |
| `float64`          | `double`                               |
| `bool`             | `i1`                                   |
| `char`             | `i32` (Unicode codepoint)              |
| `byte`             | `i8` (raw, non-arithmetic)             |
| `string`           | `%struct.herta_string = type { i64, ptr }` (по значению) |
| `[T; N]`           | `[N x <T>]` (по значению, копируется при присваивании) |
| `struct S {…}`     | `%struct.S = type { … }` (по значению) |
| `*T`               | `ptr` (opaque)                         |
| `void` / `unit`    | `void`                                 |

Знаковость отделена от типа: для int используется `sdiv/srem/sext/sgt/…`,
для uint — `udiv/urem/zext/ugt/…`. LLVM-тип одинаковый, поэтому
бит-идентичные приведения `int32 ↔ uint32` не порождают инструкций.

### Memory form

Каждая `Var` и каждый `Temp` IR'а получает alloca'у на входе функции
(prolog). Одноимённые переменные из разных блоков (shadowing) ещё на этапе
lowering переименовываются в уникальные (`x`, `x.1`, `x.2`, …), поэтому
«одна alloca на имя» корректна. Чтение операнда — `load`, запись — `store`.
Это даёт:

- **Естественную value-семантику.** Загрузка `%struct.X*` через `load`
  возвращает first-class aggregate; запись обратно в другую alloca'у
  копирует значение целиком.
- **Простоту эмиссии.** Не нужно вести phi-узлы вручную; clang в `-O1`
  пропускает `mem2reg`, и в финальном бинаре аллок не остаётся.

### Неявное widening

IR не несёт явных `Cast` для widening (semantics.md §6.2). Эмиттер вычисляет
общий числовой тип для каждой бинарной операции/сравнения и вставляет
`sext` / `zext` / `sitofp` в точке использования.

### Runtime-проверки

Все runtime-ошибки имеют формат `runtime error: <msg> at line <N>` (stderr,
exit 1):

| Ситуация               | Helper                       | Сообщение                          |
|------------------------|------------------------------|------------------------------------|
| Деление целых на ноль  | `@herta_rt_div_zero`         | `division by zero`                 |
| Out-of-bounds индекс   | `@herta_rt_oob`              | `index out of bounds: <i>, size <M>` |
| `null` разыменование   | `@herta_rt_null_deref`       | `null pointer dereference`         |
| `assert(false)`        | `@herta_assert_fail`         | `assertion failed at line <N>`     |
| `panic(msg)`           | `@herta_panic`               | `panic: <msg>`                     |
| `exit(code)`           | `@herta_exit`                | (немедленный выход с кодом)        |

Проверки вставляются эмиттером перед каждой потенциально опасной
инструкцией: целочисленные `/` и `%` — через `icmp eq … 0`, индексирование
массива — через сравнение с границами, `LoadPtr`/`StorePtr` — через
`icmp eq ptr … null`.

### C-runtime

[`src/codegen/runtime.c`](../src/codegen/runtime.c) экспортирует:

- `herta_print_int/uint/float/bool/char/string` — вывод + `\n`.
- `herta_string_len`, `herta_string_concat`, `herta_string_eq`,
  `herta_input` — операции над строками.
- `herta_exit`, `herta_panic`, `herta_assert_fail`, `herta_rt_div_zero`,
  `herta_rt_oob`, `herta_rt_null_deref` — терминаторы исполнения.

Память для строковых буферов (конкатенация, `input`) — malloc без free
(арена-модель, semantics.md §5).

### Вложенные lvalue-присваивания

Цепочки вида `m.points[i].x = v` лоуэрер разворачивает в
read–modify–write-back: `t = m.points` (копия), `t2 = t[i]`, `t2.x = v`,
`t[i] = t2`, `m.points = t` — запись доходит до корневой переменной
(semantics.md §2.1).

### Имена функций и кросс-модульные вызовы

Все функции получают имя `@<Module>.<flat>`, кроме `main` (`@main`).
Методы структур лоуэрятся как `<Type>.<method>` в своём модуле; вызов
метода импортированного типа получает в IR имя `<Module>.<Type>.<method>`,
которое бэкенд разрешает в функцию нужного модуля. Из-за плоской таблицы
структур в бэкенде имена структур должны быть уникальны на всю программу.

### Псевдонимы типов

`type Alias = Target;` — синонимы, не отдельные типы (semantics.md §10).
Перед эмиссией собирается плоская таблица алиасов из всех модулей; типы
во всех инструкциях IR раскрываются рекурсивно (включая элементы массивов).

### Указатели (A.2.18 / A.3.9)

| Конструкция | LLVM IR |
|-------------|---------|
| `*T` (тип)  | `ptr` (opaque) |
| `&x`        | адрес alloca'и `x` (`%x.addr`); только на целой переменной |
| `*p`        | `load <T>, ptr %p` + runtime null-check |
| `*p = v`    | runtime null-check + `store <T> %v, ptr %p` |
| `null`      | LLVM `null` константа |
| `p == q`, `p != null` | `icmp eq/ne ptr` |
| `byte(x)`, `uint8(b)` | `trunc`/`zext` через i8 |

## Точка входа CLI

```
myc <source.herta> [options]
```

| Флаг              | Действие                                              |
|-------------------|-------------------------------------------------------|
| (без флагов)      | компиляция в нативный бинарь                          |
| `-o <path>`       | путь выходного бинаря (по умолчанию — stem от входа)  |
| `--run`           | скомпилировать, запустить, вернуть код процесса       |
| `--emit-llvm`     | вывести `.ll` в stdout и выйти                        |
| `--dump-tokens`   | поток токенов                                         |
| `--dump-ast`      | AST                                                   |
| `--dump-ir`       | трёхадресный IR                                       |
