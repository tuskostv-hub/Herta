# граматика языка Herta (EBNF)

---

## 1. Алфавит

Язык использует кодировку UTF-8. Исходный файл состоит из символов Unicode.

```
letter        = "a" .. "z" | "A" .. "Z" | "_" ;
digit         = "0" .. "9" ;
hex_digit     = digit | "a" .. "f" | "A" .. "F" ;
whitespace    = " " | "\t" | "\r" | "\n" ;
```

---

## 2. Лексика

### 2.1 Ключевые слова

Следующие идентификаторы зарезервированы лексером и не могут использоваться как имена:

```
fn        let       var       return    if        else
while     break     continue  struct    type      namespace
impl      module    import    pub       priv      true
false     null
```

Кроме того, имена встроенных функций зарезервированы **семантикой**: объявить
переменную, параметр, функцию, тип или пространство имён с таким именем —
ошибка компиляции (иначе затенение молча ломало бы builtin):

```
print  input  exit  panic  assert  len  sleep_ms
int_to_string  float_to_string  parse_int  parse_float  inf  nan
```

> `self` **не** является ключевым словом — это обычный идентификатор, используемый по соглашению как имя первого параметра инстанс-метода (см. §3.2.5).

### 2.2 Идентификаторы

```
identifier    = letter { letter | digit } ;
```

Примеры: `x`, `myVar`, `Point2D`, `_count`

### 2.3 Литералы

#### Целые числа

```
int_literal   = ( decimal_lit | hex_lit | bin_lit ) [ int_suffix ] ;
decimal_lit   = digit { digit } ;
hex_lit       = "0x" hex_digit { hex_digit } ;
bin_lit       = "0b" ( "0" | "1" ) { "0" | "1" } ;
int_suffix    = "i8" | "i16" | "i32" | "i64"
              | "u8" | "u16" | "u32" | "u64" ;
```

Примеры: `0`, `42`, `1000`, `0xFF`, `0b101010`, `42i8`, `255u8`, `0xFFu32`

Суффикс задаёт тип литерала (A.1.1); значение обязано помещаться в него.
Литерал без суффикса охватывает весь диапазон `uint64`
(до `18446744073709551615`); правила выбора типа по умолчанию — types.md §6.

> В hex-литералах суффиксы `f32`/`f64` не распознаются: символы `a..f`
> неотличимы от hex-цифр (`0xFFf32` — это hex-число `0xFFF32`).

#### Вещественные числа

```
float_literal = ( digit { digit } "." digit { digit } [ exponent ]
                | digit { digit } )            (* целая форма — только с суффиксом *)
                [ float_suffix ] ;
exponent      = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
float_suffix  = "f32" | "f64" ;
```

Примеры: `3.14`, `2.0`, `1.5e10`, `2.5f32`, `42f64`

#### Булевы значения

```
bool_literal  = "true" | "false" ;
```

#### Строки

```
string_literal = '"' { string_char } '"' ;
string_char    = any_utf8_char_except_quote_and_newline
               | escape_seq ;
escape_seq     = "\\" ( '"' | "\\" | "n" | "t" | "r" ) ;
```

Примеры: `"hello"`, `"line\n"`, `"tab\there"`

#### Символы (char)

```
char_literal   = "'" ( single_char | char_escape ) "'" ;
single_char    = any_byte_except_quote_and_backslash_and_newline ;
char_escape    = "\\" ( "'" | "\\" | "n" | "t" | "r" | "0" ) ;
```

Примеры: `'a'`, `'\n'`, `'\''`, `'\0'`

#### Литерал массива

```
array_literal  = "[" [ expr { "," expr } ] "]" ;
```

Пример: `[1, 2, 3]`

#### Литерал структуры

```
struct_literal = identifier "{" [ field_init { "," field_init } ] "}" ;
field_init     = identifier ":" expr ;
```

Пример: `Point { x: 1.0, y: 2.0 }`

### 2.4 Операторы и разделители

```
"+"  "-"  "*"  "/"  "%"          -- арифметика
"=="  "!="  "<"  ">"  "<="  ">=" -- сравнение
"&&"  "||"  "!"                  -- логика
"&"                               -- унарное взятие адреса (см. §3.4)
"*"                               -- также унарное разыменование указателя
"="                               -- присваивание
":="                              -- объявление с выводом типа (доп.)
":"  ","  ";"  "."               -- разделители
"("  ")"  "{"  "}"  "["  "]"    -- скобки
"->"                              -- возвращаемый тип функции (альтернативная форма)
```

### 2.5 Комментарии

```
line_comment  = "//" { any_char_except_newline } newline ;
```

Комментарии игнорируются лексером и не попадают в поток токенов.

Пример: `// это комментарий`

---

## 3. Синтаксис

### 3.1 Структура программы

Программа состоит из одного или нескольких **модулей** — отдельных
исходных файлов. Каждый файл обязан начинаться с объявления модуля,
за которым могут следовать импорты и объявления верхнего уровня.
Выражения и инструкции допускаются **только** внутри тел функций.

```
program        = module_decl
                 { import_decl }
                 { top_level_decl }
                 EOF ;

module_decl    = "module" identifier ";" ;
import_decl    = "import" identifier ";" ;

top_level_decl = [ "pub" ] ( fn_decl
                           | struct_decl
                           | type_alias_decl
                           | namespace_decl
                           | impl_decl ) ;
```

Префикс `pub` помечает объявление как экспортируемое; объявления без `pub`
видны только внутри своего модуля. Префикс `pub` к `impl_decl` не применяется —
видимость регулируется на уровне отдельных методов внутри `impl` (см. §3.2.5).

#### Модуль и импорт

```
// файл math.herta — модуль Math
module Math;

pub fn add(a: int32, b: int32) int32 {
    return a + b;
}

fn helper() int32 { return 0; }   // не экспортируется
```

```
// файл main.herta — модуль Main
module Main;
import Math;

fn main() int32 {
    return Math.add(1, 2);
}
```

Правила объявления модуля и импорта подробно описаны в semantics.md §13.

### 3.2 Объявления верхнего уровня

#### Функции

```
fn_decl        = "fn" identifier "(" [ param_list ] ")"
                 [ ret_clause ] block ;
ret_clause     = type_expr | "->" type_expr ;

param_list     = param { "," param } ;
param          = identifier ":" type_expr ;
```

Тип возврата задаётся сразу после `)`, через стрелку `->`, либо опускается —
тогда он **выводится** по `return`-инструкциям тела (A.1.7, см. semantics.md §7.8.2).

Пример:
```
fn add(a: int32, b: int32) int32 {
    return a + b;
}

fn sub(a: int32, b: int32) -> int32 {   // стрелочная форма
    return a - b;
}

fn twice(x: int32) {                    // тип возврата выведен: int32
    return x * 2;
}
```

Точка входа — функция `main` с возвращаемым типом `int32`:

```
fn main() int32 {
    return 0;
}
```

#### Структуры

```
struct_decl    = "struct" identifier "{" [ field_list ] "}" ;

field_list     = field_decl { "," field_decl } ;
field_decl     = [ "priv" ] identifier ":" type_expr ;
```

Поле с префиксом `priv` приватно: доступ к нему (чтение, запись,
инициализация в литерале) разрешён только из методов своего типа
(см. semantics.md §13.6). Поля без `priv` публичны.

Пример:
```
struct Point {
    x: float64,
    y: float64,
}

struct Account {
    owner: string,
    priv balance: int32,   // только для методов Account
}
```

#### Синонимы типов

```
type_alias_decl = "type" identifier "=" type_expr ";" ;
```

Пример:
```
type Meters = int32;
type Name = string;
```

#### Методы структур (impl-блоки)

```
impl_decl       = "impl" identifier "{" { [ "pub" ] fn_decl } "}" ;
```

Блок `impl T { ... }` добавляет функции, ассоциированные с типом `T`
(который обязан быть объявлен ранее как `struct` или `type alias` на `struct`).

- Функция, у которой **первый параметр** имеет имя `self` и тип, совпадающий
  с `T` (после раскрытия type alias), является **инстанс-методом**.
  Вызов: `obj.method(args)`. Значение `obj` копируется в `self`
  (call-by-value, см. semantics.md §2).
- Функция без `self`-параметра является **статическим методом**.
  Вызов: `T.method(args)`.
- В одном `impl T` запрещены два метода с одинаковым именем.
- Несколько `impl T`-блоков для одного типа допускаются (например, в разных
  пространствах имён), но имена их методов не должны конфликтовать.

Пример:

```
struct Point {
    x: float64,
    y: float64,
}

impl Point {
    fn length(self: Point) float64 {
        return self.x * self.x + self.y * self.y;
    }

    fn origin() Point {
        return Point { x: 0.0, y: 0.0 };
    }
}

fn main() int32 {
    let p: Point = Point.origin();
    let d: float64 = p.length();
    return 0;
}
```

#### Пространства имён

```
namespace_decl  = "namespace" identifier "{" { top_level_decl } "}" ;
```

Пример:
```
namespace Math {
    fn abs(x: int32) int32 {
        if x < 0 { return -x; }
        return x;
    }
}
```

---

### 3.3 Инструкции (statements)

```
stmt           = var_decl_stmt
               | let_decl_stmt
               | assign_stmt
               | return_stmt
               | if_stmt
               | while_stmt
               | break_stmt
               | continue_stmt
               | expr_stmt
               | empty_stmt
               | block ;

block          = "{" { stmt } "}" ;
empty_stmt     = ";" ;
```

#### Объявление переменных

Мутабельная переменная (значение можно изменить):

```
var_decl_stmt  = "var" identifier ":" type_expr "=" expr ";"
               | "var" identifier ":=" expr ";" ;   -- вывод типа (доп.)
```

Иммутабельная переменная (значение нельзя переприсвоить):

```
let_decl_stmt  = "let" identifier ":" type_expr "=" expr ";"
               | "let" identifier ":=" expr ";" ;   -- вывод типа (доп.)
```

Примеры:
```
var x: int32 = 0;
let name: string = "world";
var count := 10;        // тип выводится как int32
```

#### Присваивание

```
assign_stmt    = lvalue "=" expr ";" ;

lvalue         = identifier
               | lvalue "[" expr "]"
               | lvalue "." identifier
               | "*" expr ;        (* запись через указатель: *p = v *)
```

Пример: `x = x + 1;`

#### Возврат из функции

```
return_stmt    = "return" [ expr ] ";" ;
```

#### Условное ветвление

```
if_stmt        = "if" expr block [ "else" ( block | if_stmt ) ] ;
```

Примеры:
```
if x > 0 {
    print(x);
}

if x > 0 {
    print(x);
} else {
    print(-x);
}

if x > 0 {
    print("positive");
} else if x < 0 {
    print("negative");
} else {
    print("zero");
}
```

#### Цикл while

```
while_stmt     = "while" expr block ;
break_stmt     = "break" ";" ;
continue_stmt  = "continue" ";" ;
```

Пример:
```
var i: int32 = 0;
while i < 10 {
    print(i);
    i = i + 1;
}
```

#### Инструкция-выражение

```
expr_stmt      = expr ";" ;
```

Используется для вызовов функций как инструкций:

```
print("hello");
exit(0);
```

---

### 3.4 Выражения

Приоритет операторов (от **низшего** к **высшему**):

| Уровень | Оператор(ы)           | Ассоциативность |
|---------|-----------------------|-----------------|
| 1       | `\|\|`                | левая           |
| 2       | `&&`                  | левая           |
| 3       | `==`  `!=`            | левая           |
| 4       | `<`  `>`  `<=`  `>=`  | левая           |
| 5       | `+`  `-`              | левая           |
| 6       | `*`  `/`  `%`         | левая           |
| 7       | унарный `!`  `-`      | правая (prefix) |
| 8       | постфиксные: `[]` `.` | левая           |
| 9       | primary               | —               |

```
expr           = or_expr ;

or_expr        = and_expr { "||" and_expr } ;
and_expr       = eq_expr  { "&&" eq_expr  } ;
eq_expr        = rel_expr { ( "==" | "!=" ) rel_expr } ;
rel_expr       = add_expr { ( "<" | ">" | "<=" | ">=" ) add_expr } ;
add_expr       = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr       = unary    { ( "*" | "/" | "%" ) unary } ;

unary          = ( "!" | "-" | "&" | "*" ) unary
               | postfix ;

(*  & — взятие адреса, даёт *T для операнда типа T. Семантика допускает &
    только на целой переменной (&x); &s.f и &a[i] — ошибка компиляции,
    потому что при value semantics это был бы адрес временной копии.
    * — разыменование указателя; в позиции lvalue допустимо как цель
    присваивания (*p = v). См. semantics.md §15 «Указатели». *)

postfix        = primary { postfix_op } ;
postfix_op     = "[" expr "]"
               | "." identifier
               | "(" [ arg_list ] ")" ;

arg_list       = expr { "," expr } ;
```

#### Первичные выражения

```
primary        = int_literal
               | float_literal
               | bool_literal
               | string_literal
               | "null"               (* нулевой указатель — совместим с любым *T *)
               | array_literal
               | struct_literal
               | identifier
               | namespace_access
               | cast_expr
               | "(" expr ")" ;

namespace_access = identifier "." identifier { "." identifier } ;

cast_expr      = type_expr "(" expr ")" ;
```

Цепочки имён через точку разрешаются по namespace и модулям:
`NS.f(...)`, `M.f(...)`, `M.NS.f(...)`, `T.static_method(...)`,
`M.T.static_method(...)`. Литерал структуры тоже может быть
квалифицированным: `M.Vec2 { x: 1.0, y: 2.0 }`.

Примеры:
```
float64(x)          // явное приведение типов
Math.abs(-5)        // доступ к элементу пространства имён
Lib.Util.neg(5)     // pub-функция в pub-namespace другого модуля
arr[0]              // индексирование массива
point.x             // доступ к полю структуры
add(1, 2)           // вызов функции
```

---

### 3.5 Типы

```
type_expr      = base_type
               | array_type
               | pointer_type
               | named_type ;

named_type     = identifier { "." identifier } ;
                 -- пользовательский тип (struct / alias), возможно
                 -- квалифицированный: Math.Vec2, M.NS.T

base_type      = "int8"  | "int16"  | "int32"  | "int64"
               | "uint8" | "uint16" | "uint32" | "uint64"
               | "float32" | "float64"
               | "bool"
               | "char"
               | "string"
               | "byte"           -- неарифметический сырой 1-байтный тип
               | "void" ;

array_type     = "[" type_expr ";" int_literal "]" ;

pointer_type   = "*" type_expr ;   (* *T — указатель; допустимо *void для raw *)
```

Примеры типов:
```
int32
float64
bool
string
void
[int32; 10]       -- массив из 10 элементов int32
[float64; 3]      -- массив из 3 элементов float64
Point             -- пользовательский тип
Meters            -- синоним типа
Math.Vec2         -- pub-тип из импортированного модуля
```

---

## 4. Встроенные функции

Встроенные функции вызываются как обычные, но не требуют объявления.
Их имена зарезервированы (см. §2.1). Полная семантика — semantics.md §10.

| Имя               | Сигнатура                          | Описание                              |
|-------------------|------------------------------------|---------------------------------------|
| `print`           | `fn print(v: T) void`              | Вывод скаляра/строки в stdout + `\n`   |
| `input`           | `fn input() string`                | Чтение строки из stdin                 |
| `exit`            | `fn exit(code: int32) void`        | Завершение программы с кодом           |
| `panic`           | `fn panic(msg: string) void`       | Аварийное завершение с сообщением      |
| `assert`          | `fn assert(cond: bool) void`       | Аварийное завершение при ложном cond   |
| `len`             | `fn len(x: string \| [T; N]) int32`| Длина строки в байтах / размер массива |
| `sleep_ms`        | `fn sleep_ms(ms: int64) void`      | Пауза в миллисекундах                  |
| `int_to_string`   | `fn int_to_string(x: int64) string`| Число → строка                         |
| `float_to_string` | `fn float_to_string(x: float64) string` | Число → строка                    |
| `parse_int`       | `fn parse_int(s: string) int64`    | Строка → число (0 при неуспехе)        |
| `parse_float`     | `fn parse_float(s: string) float64`| Строка → число (0.0 при неуспехе)      |
| `inf`             | `fn inf() float64`                 | +бесконечность IEEE 754                |
| `nan`             | `fn nan() float64`                 | quiet NaN IEEE 754                     |

---

## 5. Разрешение имён

- Используется **лексическая область видимости** (lexical scoping).
- Идентификатор разрешается в ближайшей охватывающей области видимости.
- Shadowing допускается: внутреннее объявление скрывает внешнее.
- Использование идентификатора **до** его объявления запрещено.
- Повторное объявление имени в **одной** области видимости запрещено.
- Доступ к элементам пространства имён: `namespace.identifier`.

---

## 6. Полный пример программы

Язык использует value semantics (semantics.md §2): массив-параметр — это
копия, мутации внутри функции не видны вызывающему. Поэтому сортировка
принимает массив по значению и **возвращает** отсортированную копию.

```
// Пример: сортировка пузырьком

module bubble;

namespace Utils {
    type Index = int32;

    // Массив передаётся по значению: меняем копию и возвращаем её.
    fn swapped(arr: [int32; 5], i: Index, j: Index) [int32; 5] {
        var tmp: int32 = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
        return arr;
    }
}

fn bubble_sort(arr: [int32; 5], n: int32) [int32; 5] {
    var i: int32 = 0;
    while i < n - 1 {
        var j: int32 = 0;
        while j < n - 1 - i {
            if arr[j] > arr[j + 1] {
                arr = Utils.swapped(arr, j, j + 1);
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return arr;
}

fn main() int32 {
    var arr: [int32; 5] = [5, 3, 1, 4, 2];
    arr = bubble_sort(arr, 5);

    var k: int32 = 0;
    while k < 5 {
        print(arr[k]);
        k = k + 1;
    }

    return 0;
}
```
