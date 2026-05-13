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

Следующие идентификаторы зарезервированы и не могут использоваться как имена:

```
fn        let       var       return    if        else
while     break     continue  struct    type      namespace
impl      module    import    pub       true      false
print     input     exit      panic
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
int_literal   = decimal_lit | hex_lit ;
decimal_lit   = digit { digit } ;
hex_lit       = "0x" hex_digit { hex_digit } ;
```

Примеры: `0`, `42`, `1000`, `0xFF`

#### Вещественные числа

```
float_literal = digit { digit } "." digit { digit } [ exponent ] ;
exponent      = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
```

Примеры: `3.14`, `2.0`, `1.5e10`

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
"="                               -- присваивание
":="                              -- объявление с выводом типа (доп.)
":"  ","  ";"  "."               -- разделители
"("  ")"  "{"  "}"  "["  "]"    -- скобки
"->"                              -- возвращаемый тип функции (альтернатива)
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
fn_decl        = "fn" identifier "(" [ param_list ] ")" type_expr block ;

param_list     = param { "," param } ;
param          = identifier ":" type_expr ;
```

Пример:
```
fn add(a: int32, b: int32) int32 {
    return a + b;
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
field_decl     = identifier ":" type_expr ;
```

Пример:
```
struct Point {
    x: float64,
    y: float64,
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
               | lvalue "." identifier ;
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

unary          = ( "!" | "-" ) unary
               | postfix ;

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
               | array_literal
               | struct_literal
               | identifier
               | namespace_access
               | cast_expr
               | "(" expr ")" ;

namespace_access = identifier "." identifier ;

cast_expr      = type_expr "(" expr ")" ;
```

Примеры:
```
float64(x)          // явное приведение типов
Math.abs(-5)        // доступ к элементу пространства имён
arr[0]              // индексирование массива
point.x             // доступ к полю структуры
add(1, 2)           // вызов функции
```

---

### 3.5 Типы

```
type_expr      = base_type
               | array_type
               | identifier ;     -- пользовательский тип (struct / alias)

base_type      = "int8"  | "int16"  | "int32"  | "int64"
               | "uint8" | "uint16" | "uint32" | "uint64"
               | "float32" | "float64"
               | "bool"
               | "string"
               | "void" ;

array_type     = "[" type_expr ";" int_literal "]" ;
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
```

---

## 4. Встроенные функции

Встроенные функции вызываются как обычные, но не требуют объявления.

| Имя      | Сигнатура                  | Описание                            |
|----------|----------------------------|-------------------------------------|
| `print`  | `fn print(v: T) void`      | Вывод значения в stdout             |
| `input`  | `fn input() string`        | Чтение строки из stdin              |
| `exit`   | `fn exit(code: int32) void`| Завершение программы с кодом        |
| `panic`  | `fn panic(msg: string) void`| Аварийное завершение с сообщением  |

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

```
// Пример: сортировка пузырьком

namespace Utils {
    type Index = int32;

    fn swap(arr: [int32; 5], i: Index, j: Index) void {
        var tmp: int32 = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

fn bubble_sort(arr: [int32; 5], n: int32) void {
    var i: int32 = 0;
    while i < n - 1 {
        var j: int32 = 0;
        while j < n - 1 - i {
            if arr[j] > arr[j + 1] {
                Utils.swap(arr, j, j + 1);
            }
            j = j + 1;
        }
        i = i + 1;
    }
}

fn main() int32 {
    var arr: [int32; 5] = [5, 3, 1, 4, 2];
    bubble_sort(arr, 5);

    var k: int32 = 0;
    while k < 5 {
        print(arr[k]);
        k = k + 1;
    }

    return 0;
}
```
