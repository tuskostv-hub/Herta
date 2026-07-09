// Семантика: проверка типов, областей видимости, мутабельности.

export module herta.semantic;

import std;
import herta.common;
import herta.ast;

namespace herta::semantic {

using herta::common::Diagnostic;
using herta::common::DiagnosticSink;
using herta::common::SourceLocation;
namespace ast = herta::ast;

export enum class Primitive : std::uint8_t {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Bool, String, Char, Void,
    Byte,  // 1-байтный raw-тип, не арифметический
};

export struct ArrayTy;
export struct StructTy;
export struct PointerTy;

export class Type {
public:
    using Repr = std::variant<Primitive,
                              std::shared_ptr<ArrayTy>,
                              std::shared_ptr<StructTy>,
                              std::shared_ptr<PointerTy>>;

    Type() : repr_(Primitive::Void) {}
    explicit Type(Primitive p) : repr_(p) {}
    explicit Type(std::shared_ptr<ArrayTy> a) : repr_(std::move(a)) {}
    explicit Type(std::shared_ptr<StructTy> s) : repr_(std::move(s)) {}
    explicit Type(std::shared_ptr<PointerTy> p) : repr_(std::move(p)) {}

    bool is_primitive() const noexcept {
        return std::holds_alternative<Primitive>(repr_);
    }
    Primitive prim() const noexcept { return std::get<Primitive>(repr_); }
    bool is(Primitive p) const noexcept { return is_primitive() && prim() == p; }

    bool is_array() const noexcept {
        return std::holds_alternative<std::shared_ptr<ArrayTy>>(repr_);
    }
    const ArrayTy& array() const noexcept {
        return *std::get<std::shared_ptr<ArrayTy>>(repr_);
    }
    std::shared_ptr<ArrayTy> array_ptr() const {
        return std::get<std::shared_ptr<ArrayTy>>(repr_);
    }

    bool is_struct() const noexcept {
        return std::holds_alternative<std::shared_ptr<StructTy>>(repr_);
    }
    const StructTy& strukt() const noexcept {
        return *std::get<std::shared_ptr<StructTy>>(repr_);
    }
    std::shared_ptr<StructTy> struct_ptr() const {
        return std::get<std::shared_ptr<StructTy>>(repr_);
    }

    bool is_pointer() const noexcept {
        return std::holds_alternative<std::shared_ptr<PointerTy>>(repr_);
    }
    const PointerTy& pointer() const noexcept {
        return *std::get<std::shared_ptr<PointerTy>>(repr_);
    }
    std::shared_ptr<PointerTy> pointer_ptr() const {
        return std::get<std::shared_ptr<PointerTy>>(repr_);
    }

    bool is_void() const noexcept { return is(Primitive::Void); }
    bool is_bool() const noexcept { return is(Primitive::Bool); }
    bool is_string() const noexcept { return is(Primitive::String); }

    bool same_as(const Type& o) const noexcept;
    std::string to_string() const;

private:
    Repr repr_;
};

struct ArrayTy {
    Type element;
    std::int64_t size = 0;
};

export struct FieldInfo {
    std::string name;
    Type type;
    bool is_priv = false;  // priv-поле доступно только методам своего типа
};

struct StructTy {
    std::string name;
    std::vector<FieldInfo> fields;
    SourceLocation loc;
};

struct PointerTy {
    Type pointee;
    // is_raw — указатель без конкретного типа (*void и *byte).
    // Свободно приводится к любому другому указателю без явного каста.
    bool is_raw = false;
};

bool Type::same_as(const Type& o) const noexcept {
    if (repr_.index() != o.repr_.index()) return false;
    if (is_primitive()) return prim() == o.prim();
    if (is_array()) {
        const auto& a = array();
        const auto& b = o.array();
        return a.size == b.size && a.element.same_as(b.element);
    }
    if (is_pointer()) {
        return pointer().pointee.same_as(o.pointer().pointee);
    }
    // nominal struct: identity by shared_ptr
    return std::get<std::shared_ptr<StructTy>>(repr_)
         == std::get<std::shared_ptr<StructTy>>(o.repr_);
}

std::string Type::to_string() const {
    if (is_primitive()) {
        switch (prim()) {
            case Primitive::I8: return "int8";
            case Primitive::I16: return "int16";
            case Primitive::I32: return "int32";
            case Primitive::I64: return "int64";
            case Primitive::U8: return "uint8";
            case Primitive::U16: return "uint16";
            case Primitive::U32: return "uint32";
            case Primitive::U64: return "uint64";
            case Primitive::F32: return "float32";
            case Primitive::F64: return "float64";
            case Primitive::Bool: return "bool";
            case Primitive::String: return "string";
            case Primitive::Char: return "char";
            case Primitive::Void: return "void";
            case Primitive::Byte: return "byte";
        }
        return "?";
    }
    if (is_array()) {
        return std::format("[{}; {}]",
                          array().element.to_string(), array().size);
    }
    if (is_pointer()) {
        return "*" + pointer().pointee.to_string();
    }
    if (is_struct()) return strukt().name;
    return "?";
}

namespace {

constexpr bool is_signed_int(Primitive p) noexcept {
    return p == Primitive::I8 || p == Primitive::I16
        || p == Primitive::I32 || p == Primitive::I64;
}
constexpr bool is_unsigned_int(Primitive p) noexcept {
    return p == Primitive::U8 || p == Primitive::U16
        || p == Primitive::U32 || p == Primitive::U64;
}
constexpr bool is_int(Primitive p) noexcept {
    return is_signed_int(p) || is_unsigned_int(p);
}
constexpr bool is_float(Primitive p) noexcept {
    return p == Primitive::F32 || p == Primitive::F64;
}
constexpr bool is_numeric(Primitive p) noexcept {
    return is_int(p) || is_float(p);
}
constexpr int int_width(Primitive p) noexcept {
    switch (p) {
        case Primitive::I8: case Primitive::U8: return 8;
        case Primitive::I16: case Primitive::U16: return 16;
        case Primitive::I32: case Primitive::U32: return 32;
        case Primitive::I64: case Primitive::U64: return 64;
        default: return 0;
    }
}

// Неявное расширение числовых типов
bool can_widen_prim(Primitive from, Primitive to) noexcept {
    if (from == to) return true;
    if (is_signed_int(from) && is_signed_int(to))
        return int_width(from) < int_width(to);
    if (is_unsigned_int(from) && is_unsigned_int(to))
        return int_width(from) < int_width(to);
    if (is_unsigned_int(from) && is_signed_int(to))
        return int_width(from) < int_width(to);
    if (is_int(from) && to == Primitive::F64) return true;
    if (is_int(from) && to == Primitive::F32 && int_width(from) <= 16) return true;
    if (from == Primitive::F32 && to == Primitive::F64) return true;
    return false;
}

bool can_implicit_convert(const Type& from, const Type& to) noexcept {
    if (from.same_as(to)) return true;
    if (from.is_primitive() && to.is_primitive())
        return can_widen_prim(from.prim(), to.prim());
    // Указатели: null приводится к любому *T, сырой указатель — к типизированному и обратно.
    if (from.is_pointer() && to.is_pointer()) {
        if (from.pointer().is_raw || to.pointer().is_raw) return true;
        return from.pointer().pointee.same_as(to.pointer().pointee);
    }
    // string → raw pointer (нужно на границе с C). На уровне рантайма это просто .data из строки.
    if (from.is_string() && to.is_pointer() && to.pointer().is_raw) return true;
    return false;
}

// Явное приведение типов T(x)
bool can_explicit_cast(const Type& from, const Type& to) noexcept {
    if (from.same_as(to)) return true;
    // Любой указатель в любой указатель — разрешено
    if (from.is_pointer() && to.is_pointer()) return true;
    if (!from.is_primitive() || !to.is_primitive()) return false;
    auto a = from.prim(), b = to.prim();
    if (a == Primitive::Bool || b == Primitive::Bool) return false;
    if (a == Primitive::String || b == Primitive::String) return false;
    if (a == Primitive::Void || b == Primitive::Void) return false;
    // byte не арифметический, но явно конвертируется в целые и обратно —
    // иначе значение byte невозможно ни создать, ни рассмотреть.
    if (a == Primitive::Byte && is_int(b)) return true;
    if (is_int(a) && b == Primitive::Byte) return true;
    if (a == Primitive::Byte || b == Primitive::Byte) return false;
    // char ↔ int/uint — это получение/установка числового кода символа.
    if (a == Primitive::Char && is_int(b)) return true;
    if (is_int(a) && b == Primitive::Char) return true;
    if (a == Primitive::Char || b == Primitive::Char) return false;
    return is_numeric(a) && is_numeric(b);
}

// Общий тип для арифметических операций через widening.
std::optional<Primitive> common_arith(Primitive a, Primitive b) noexcept {
    if (!is_numeric(a) || !is_numeric(b)) return std::nullopt;
    if (a == b) return a;
    if (can_widen_prim(a, b)) return b;
    if (can_widen_prim(b, a)) return a;
    return std::nullopt;
}

// Имена встроенных функций зарезервированы: объявить переменную, функцию
// или тип с таким именем нельзя — иначе затенение молча ломает builtin.
bool is_reserved_builtin(std::string_view name) noexcept {
    constexpr std::string_view kBuiltins[] = {
        "print", "input", "exit", "panic", "assert", "len", "sleep_ms",
        "int_to_string", "float_to_string", "parse_int", "parse_float",
        "inf", "nan",
    };
    for (auto b : kBuiltins) if (b == name) return true;
    return false;
}

// Помещается ли литерал в целевой целочисленный тип?
bool int_fits(std::int64_t v, Primitive target) noexcept {
    switch (target) {
        case Primitive::I8:  return v >= -128 && v <= 127;
        case Primitive::I16: return v >= -32768 && v <= 32767;
        case Primitive::I32: return v >= std::numeric_limits<std::int32_t>::min()
                                 && v <= std::numeric_limits<std::int32_t>::max();
        case Primitive::I64: return true;  // int64_t всегда вмещает
        case Primitive::U8:  return v >= 0 && v <= 255;
        case Primitive::U16: return v >= 0 && v <= 65535;
        case Primitive::U32: return v >= 0
                                 && v <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
        case Primitive::U64: return v >= 0;
        default: return false;
    }
}

// Тип из суффикса числового литерала ("i8".."u64", "f32"/"f64").
std::optional<Primitive> suffix_to_prim(std::string_view s) noexcept {
    if (s == "i8")  return Primitive::I8;
    if (s == "i16") return Primitive::I16;
    if (s == "i32") return Primitive::I32;
    if (s == "i64") return Primitive::I64;
    if (s == "u8")  return Primitive::U8;
    if (s == "u16") return Primitive::U16;
    if (s == "u32") return Primitive::U32;
    if (s == "u64") return Primitive::U64;
    if (s == "f32") return Primitive::F32;
    if (s == "f64") return Primitive::F64;
    return std::nullopt;
}

}  // anonymous

export class Scope;  // forward — full definition ниже.

export struct Symbol {
    enum class Kind { Var, Fn, TypeName, Namespace, Module };
    Kind kind;
    std::string name;
    SourceLocation loc;
    // Var: type+mutable; Fn: return_type(in type)+params; TypeName: type
    Type type;
    bool is_mutable = false;
    bool is_pub = false;
    std::vector<Type> param_types;
    std::shared_ptr<Scope> ns_scope;  // Namespace / Module
};

// Метод из impl-блока. Лежит в side-table SemanticAnalyzer::methods_,
// привязанной к идентичности StructTy через shared_ptr.
export struct MethodInfo {
    std::string name;
    std::string module;      // модуль, в котором объявлен impl (для кросс-модульных вызовов)
    std::vector<Type> param_types;
    std::vector<std::string> param_names;
    Type return_type;
    bool is_static = false;  // true — первый параметр не self
    bool is_pub = false;     // без pub метод доступен только методам своего типа
    SourceLocation loc;
    const ast::FnDecl* ast_node = nullptr;
};

export class Scope {
public:
    Scope() = default;
    explicit Scope(Scope* parent) : parent_(parent) {}

    Symbol* declare(Symbol s) {
        auto key = s.name;
        auto [it, inserted] = entries_.emplace(std::move(key), std::move(s));
        return inserted ? &it->second : nullptr;
    }

    const Symbol* lookup(std::string_view name) const noexcept {
        for (auto cur = this; cur; cur = cur->parent_) {
            auto it = cur->entries_.find(std::string(name));
            if (it != cur->entries_.end()) return &it->second;
        }
        return nullptr;
    }
    const Symbol* lookup_local(std::string_view name) const noexcept {
        auto it = entries_.find(std::string(name));
        return it != entries_.end() ? &it->second : nullptr;
    }

private:
    Scope* parent_ = nullptr;
    std::unordered_map<std::string, Symbol> entries_;
};

export class SemanticAnalyzer {
public:
    // require_main=true нужен для компиляции точки входа; false — для unit-тестов
    // фрагментов, в которых fn main() нет.
    // imports — карта "имя импортированного модуля → его публичный scope".
    // Pub-фильтрация делается на стороне получателя.
    // imported_methods — методы impl-блоков ранее обработанных модулей
    // (ключ — идентичность StructTy). Нужны для вызова методов на значениях
    // импортированных типов.
    SemanticAnalyzer(const ast::Program& prog,
                     std::string_view filename,
                     DiagnosticSink& sink,
                     std::unordered_map<std::string, std::shared_ptr<Scope>> imports = {},
                     bool require_main = true,
                     std::unordered_map<StructTy*, std::vector<MethodInfo>>
                         imported_methods = {});

    bool analyze();

    // После успешного analyze() — глобальный scope этого модуля. Им
    // пользуются модули, которые его импортируют (с фильтрацией по pub).
    std::shared_ptr<Scope> module_scope() const { return shared_scope_; }

    // Side-table: тип каждого выражения после проверки. Заполняется в
    // check_expr / check_expr_ctx. Используется фазой lowering (IR), чтобы
    // не повторять разрешение типов.
    const std::unordered_map<const ast::Expr*, Type>& expression_types() const noexcept {
        return expr_types_;
    }

    // Методы impl-блоков: ключ — указатель на StructTy (т.е. идентичность типа).
    // Используется lowering для разрешения `obj.method(...)` и `T.method(...)`.
    const std::unordered_map<StructTy*, std::vector<MethodInfo>>& methods() const noexcept {
        return methods_;
    }

    // Карта импортированных модулей: имя → их публичный scope.
    // Используется lowering для эмиссии cross-module вызовов как `M.fn`.
    const std::unordered_map<std::string, std::shared_ptr<Scope>>& imports() const noexcept {
        return imports_;
    }

    // Side-table: тип возврата каждой функции (включая выведенные по return
    // и методы impl). Используется lowering вместо повторного разбора AST.
    const std::unordered_map<const ast::FnDecl*, Type>& fn_return_types() const noexcept {
        return fn_return_types_;
    }

    // Side-table: разрешённый тип каждого TypeExpr (алиасы и квалифицированные
    // имена уже раскрыты). Используется lowering для канонических строк типов.
    const std::unordered_map<const ast::TypeExpr*, Type>& type_expr_types() const noexcept {
        return type_expr_types_;
    }

private:
    bool process_decls(const std::vector<std::unique_ptr<ast::Decl>>& decls,
                       Scope& target);
    bool process_struct(const ast::StructDecl&, Scope&);
    bool process_type_alias(const ast::TypeAliasDecl&, Scope&);
    bool process_fn_signature(const ast::FnDecl&, Scope&);
    bool process_fn_body(const ast::FnDecl&, Scope&);
    bool process_namespace(const ast::NamespaceDecl&, Scope&);
    bool process_impl_signatures(const ast::ImplDecl&, Scope&);
    bool process_impl_bodies(const ast::ImplDecl&, Scope&);

    std::optional<Type> resolve_type(const ast::TypeExpr&, Scope&);

    // Разрешение квалифицированного имени (a / a.b / a.b.c) по цепочке
    // namespace/модулей с проверкой pub при пересечении границы модуля.
    struct ChainHit {
        const Symbol* sym = nullptr;  // финальный символ
        std::string path;             // полное имя для диагностик ("M.NS.f")
    };
    enum class ChainStatus { Found, NotChain, Error };
    ChainStatus resolve_chain(const ast::Expr& e, Scope& scope, ChainHit& out);
    // То же по готовому списку частей имени (для типов "M.NS.T").
    ChainStatus resolve_chain_parts(std::span<const std::string> parts,
                                    SourceLocation loc, Scope& scope,
                                    ChainHit& out);

    // Вывод типа возврата функции по её телу (A.1.7). params уже разрешены.
    std::optional<Type> infer_return_type(const ast::FnDecl& fd, Scope& target,
                                          const std::vector<Type>& param_types,
                                          StructTy* impl_type);

    // Проверка «имя не является зарезервированным builtin'ом».
    bool check_not_builtin(std::string_view name, SourceLocation loc);

    bool check_block(const ast::BlockStmt&, Scope& parent);
    bool check_stmt(const ast::Stmt&, Scope&);
    bool check_var_decl(const ast::VarDeclStmt&, Scope&);
    bool check_assign(const ast::AssignStmt&, Scope&);
    bool check_return(const ast::ReturnStmt&, Scope&);
    bool check_if(const ast::IfStmt&, Scope&);
    bool check_while(const ast::WhileStmt&, Scope&);

    std::optional<Type> check_expr(const ast::Expr&, Scope&);
    // Версия с ожидаемым контекстом — для адаптации литералов и финальной
    // проверки конвертируемости.
    std::optional<Type> check_expr_ctx(const ast::Expr&, Scope&, const Type& ctx);

    std::optional<Type> check_unary(const ast::UnaryExpr&, Scope&);
    std::optional<Type> check_binary(const ast::BinaryExpr&, Scope&);
    std::optional<Type> check_index(const ast::IndexExpr&, Scope&);
    std::optional<Type> check_field(const ast::FieldExpr&, Scope&);
    std::optional<Type> check_call(const ast::CallExpr&, Scope&);
    std::optional<Type> check_array_lit(const ast::ArrayLit&, Scope&);
    std::optional<Type> check_struct_lit(const ast::StructLit&, Scope&);

    bool is_lvalue(const ast::Expr&) const;

    std::optional<Type> check_builtin_print(const ast::CallExpr&, Scope&);

    void error(SourceLocation loc, std::string msg);

    const ast::Program& prog_;
    std::string filename_;
    DiagnosticSink& sink_;
    // Глобальный scope модуля. Используется как shared_ptr, чтобы
    // другие модули (импортеры) могли держать ссылку.
    std::shared_ptr<Scope> shared_scope_ = std::make_shared<Scope>();
    Scope& global_scope_;  // ссылка на *shared_scope_ для совместимости
    Type current_return_type_{Primitive::Void};
    int loop_depth_ = 0;
    bool require_main_ = true;
    // Тип, метод которого сейчас проверяется (для priv-полей и приватных
    // методов: они доступны только из методов своего типа).
    StructTy* current_impl_type_ = nullptr;
    // Режим вывода типа возврата (A.1.7): check_return копит тип сюда.
    bool inferring_return_ = false;
    std::optional<Type> inferred_return_;
    std::unordered_map<std::string, std::shared_ptr<Scope>> imports_;
    // impl-методы: ключ — указатель на StructTy (нестираемая идентичность).
    std::unordered_map<StructTy*, std::vector<MethodInfo>> methods_;
    // Side-table: тип каждой проверенной AST-Expr (см. expression_types()).
    std::unordered_map<const ast::Expr*, Type> expr_types_;
    // Side-tables для lowering: типы возврата функций и разрешённые TypeExpr.
    std::unordered_map<const ast::FnDecl*, Type> fn_return_types_;
    std::unordered_map<const ast::TypeExpr*, Type> type_expr_types_;
    // Где начинаются методы каждого impl-блока внутри methods_[StructTy*].
    // Нужно потому, что для одного типа допустимо несколько impl-блоков, и
    // process_impl_bodies должен сопоставить fn.body ↔ MethodInfo по индексу
    // внутри своего блока (а не глобально по methods_).
    std::unordered_map<const ast::ImplDecl*, std::size_t> impl_method_offset_;
};

SemanticAnalyzer::SemanticAnalyzer(const ast::Program& prog,
                                   std::string_view filename,
                                   DiagnosticSink& sink,
                                   std::unordered_map<std::string, std::shared_ptr<Scope>> imports,
                                   bool require_main,
                                   std::unordered_map<StructTy*, std::vector<MethodInfo>>
                                       imported_methods)
    : prog_(prog), filename_(filename), sink_(sink),
      global_scope_(*shared_scope_),
      require_main_(require_main),
      imports_(std::move(imports)),
      methods_(std::move(imported_methods)) {
    auto add_type = [&](std::string n, Primitive p) {
        Symbol s; s.kind = Symbol::Kind::TypeName; s.name = std::move(n);
        s.type = Type(p);
        global_scope_.declare(std::move(s));
    };
    add_type("int8", Primitive::I8);
    add_type("int16", Primitive::I16);
    add_type("int32", Primitive::I32);
    add_type("int64", Primitive::I64);
    add_type("uint8", Primitive::U8);
    add_type("uint16", Primitive::U16);
    add_type("uint32", Primitive::U32);
    add_type("uint64", Primitive::U64);
    add_type("float32", Primitive::F32);
    add_type("float64", Primitive::F64);
    add_type("bool", Primitive::Bool);
    add_type("string", Primitive::String);
    add_type("char", Primitive::Char);
    add_type("void", Primitive::Void);
    add_type("byte", Primitive::Byte);  // 1-байтный raw-тип

    auto add_fn = [&](std::string n, std::vector<Type> params, Type ret) {
        Symbol s; s.kind = Symbol::Kind::Fn; s.name = std::move(n);
        s.param_types = std::move(params); s.type = ret;
        global_scope_.declare(std::move(s));
    };
    // print особый: не описывается списком типов, см. check_builtin_print
    add_fn("input", {}, Type(Primitive::String));
    add_fn("exit",  {Type(Primitive::I32)},    Type(Primitive::Void));
    add_fn("panic", {Type(Primitive::String)}, Type(Primitive::Void));
    add_fn("len",   {Type(Primitive::String)}, Type(Primitive::I32));
    add_fn("sleep_ms", {Type(Primitive::I64)}, Type(Primitive::Void));

    // Конверсии число ↔ строка. parse_* возвращает 0 / 0.0 на неуспех.
    add_fn("int_to_string",   {Type(Primitive::I64)},    Type(Primitive::String));
    add_fn("float_to_string", {Type(Primitive::F64)},    Type(Primitive::String));
    add_fn("parse_int",       {Type(Primitive::String)}, Type(Primitive::I64));
    add_fn("parse_float",     {Type(Primitive::String)}, Type(Primitive::F64));

    // Float-константы: бесконечность и NaN (по спеке IEEE 754).
    add_fn("inf", {}, Type(Primitive::F64));
    add_fn("nan", {}, Type(Primitive::F64));
}

void SemanticAnalyzer::error(SourceLocation loc, std::string msg) {
    sink_.report(Diagnostic{
        .file = filename_, .loc = loc, .message = std::move(msg),
    });
}

bool SemanticAnalyzer::analyze() {
    // Регистрируем импортированные модули как Symbol::Module в нашем
    // глобальном scope. К их членам обращаемся через M.name, с фильтрацией
    // по pub при доступе.
    for (const auto& mod_name : prog_.imports) {
        auto it = imports_.find(mod_name);
        if (it == imports_.end()) {
            error({}, "imported module '" + mod_name + "' was not loaded");
            return false;
        }
        Symbol s;
        s.kind = Symbol::Kind::Module;
        s.name = mod_name;
        s.ns_scope = it->second;
        if (!global_scope_.declare(std::move(s))) {
            error({}, "import name '" + mod_name + "' clashes with another declaration");
            return false;
        }
    }
    if (!process_decls(prog_.decls, global_scope_)) return false;

    if (!require_main_) return !sink_.has_errors();

    // Проверка точки входа: fn main() int32 (строго по спецификации §12).
    auto main_sym = global_scope_.lookup_local("main");
    if (!main_sym || main_sym->kind != Symbol::Kind::Fn) {
        error({}, "program must declare 'fn main() int32 { ... }'");
        return false;
    }
    if (!main_sym->param_types.empty()) {
        error(main_sym->loc, "'main' must take no parameters");
        return false;
    }
    if (!main_sym->type.is(Primitive::I32)) {
        error(main_sym->loc, std::format(
            "'main' must return 'int32', got '{}'",
            main_sym->type.to_string()));
        return false;
    }

    return !sink_.has_errors();
}

bool SemanticAnalyzer::process_decls(
        const std::vector<std::unique_ptr<ast::Decl>>& decls,
        Scope& target) {
    // Pass 1: типы, алиасы, неймспейсы, сигнатуры функций.
    for (const auto& d : decls) {
        if (auto* s = dynamic_cast<const ast::StructDecl*>(d.get())) {
            if (!process_struct(*s, target)) return false;
        } else if (auto* a = dynamic_cast<const ast::TypeAliasDecl*>(d.get())) {
            if (!process_type_alias(*a, target)) return false;
        } else if (auto* n = dynamic_cast<const ast::NamespaceDecl*>(d.get())) {
            if (!process_namespace(*n, target)) return false;
        } else if (auto* f = dynamic_cast<const ast::FnDecl*>(d.get())) {
            if (!process_fn_signature(*f, target)) return false;
        }
    }
    // Pass 2: impl-блоки регистрируют методы (нужны типы из pass 1).
    for (const auto& d : decls) {
        if (auto* im = dynamic_cast<const ast::ImplDecl*>(d.get())) {
            if (!process_impl_signatures(*im, target)) return false;
        }
    }
    // Pass 3: тела функций.
    for (const auto& d : decls) {
        if (auto* f = dynamic_cast<const ast::FnDecl*>(d.get())) {
            if (!process_fn_body(*f, target)) return false;
        }
    }
    // Pass 4: тела методов в impl.
    for (const auto& d : decls) {
        if (auto* im = dynamic_cast<const ast::ImplDecl*>(d.get())) {
            if (!process_impl_bodies(*im, target)) return false;
        }
    }
    return true;
}

bool SemanticAnalyzer::process_struct(const ast::StructDecl& sd, Scope& target) {
    if (!check_not_builtin(sd.name, sd.loc)) return false;
    auto st = std::make_shared<StructTy>();
    st->name = sd.name;
    st->loc = sd.loc;
    std::unordered_set<std::string> seen;
    for (const auto& f : sd.fields) {
        if (!seen.insert(f.name).second) {
            error(f.loc, "duplicate field '" + f.name + "' in struct '" + sd.name + "'");
            return false;
        }
        auto t = resolve_type(*f.type, target);
        if (!t) return false;
        if (t->is_void()) {
            error(f.loc, "field cannot have type 'void'");
            return false;
        }
        st->fields.push_back(FieldInfo{f.name, std::move(*t), f.is_priv});
    }
    Symbol s; s.kind = Symbol::Kind::TypeName; s.name = sd.name;
    s.loc = sd.loc; s.type = Type(std::move(st)); s.is_pub = sd.is_pub;
    if (!target.declare(std::move(s))) {
        error(sd.loc, "redeclaration of name '" + sd.name + "'");
        return false;
    }
    return true;
}

bool SemanticAnalyzer::process_type_alias(const ast::TypeAliasDecl& ad,
                                          Scope& target) {
    if (!check_not_builtin(ad.name, ad.loc)) return false;
    auto t = resolve_type(*ad.target, target);
    if (!t) return false;
    Symbol s; s.kind = Symbol::Kind::TypeName; s.name = ad.name;
    s.loc = ad.loc; s.type = std::move(*t); s.is_pub = ad.is_pub;
    if (!target.declare(std::move(s))) {
        error(ad.loc, "redeclaration of name '" + ad.name + "'");
        return false;
    }
    return true;
}

bool SemanticAnalyzer::process_fn_signature(const ast::FnDecl& fd, Scope& target) {
    if (!check_not_builtin(fd.name, fd.loc)) return false;
    Symbol s; s.kind = Symbol::Kind::Fn; s.name = fd.name; s.loc = fd.loc;
    s.is_pub = fd.is_pub;
    for (const auto& p : fd.params) {
        if (!check_not_builtin(p.name, p.loc)) return false;
        auto t = resolve_type(*p.type, target);
        if (!t) return false;
        if (t->is_void()) {
            error(p.loc, "parameter cannot have type 'void'");
            return false;
        }
        s.param_types.push_back(std::move(*t));
    }
    if (fd.return_type) {
        auto rt = resolve_type(*fd.return_type, target);
        if (!rt) return false;
        s.type = std::move(*rt);
    } else {
        // A.1.7: тип возврата выводится по return-инструкциям тела.
        auto rt = infer_return_type(fd, target, s.param_types,
                                    /*impl_type=*/nullptr);
        if (!rt) return false;
        s.type = std::move(*rt);
    }
    fn_return_types_[&fd] = s.type;
    if (!target.declare(std::move(s))) {
        error(fd.loc, "redeclaration of name '" + fd.name + "'");
        return false;
    }
    return true;
}

// Вывод типа возврата (A.1.7): тело проверяется в режиме инференции, все
// return-инструкции обязаны сходиться к одному типу; тело без return —
// void. Функция с выводимым типом видит только объявления выше по файлу.
std::optional<Type> SemanticAnalyzer::infer_return_type(
        const ast::FnDecl& fd, Scope& target,
        const std::vector<Type>& param_types, StructTy* impl_type) {
    auto saved_ret = current_return_type_;
    auto saved_inferring = inferring_return_;
    auto saved_inferred = inferred_return_;
    auto saved_impl = current_impl_type_;
    inferring_return_ = true;
    inferred_return_ = std::nullopt;
    current_impl_type_ = impl_type;

    auto fn_scope = std::make_unique<Scope>(&target);
    for (std::size_t i = 0; i < fd.params.size(); ++i) {
        Symbol p; p.kind = Symbol::Kind::Var;
        p.name = fd.params[i].name;
        p.loc = fd.params[i].loc;
        p.type = param_types[i];
        p.is_mutable = true;
        fn_scope->declare(std::move(p));
    }
    bool ok = check_block(*fd.body, *fn_scope);
    auto result = inferred_return_;

    current_return_type_ = saved_ret;
    inferring_return_ = saved_inferring;
    inferred_return_ = saved_inferred;
    current_impl_type_ = saved_impl;

    if (!ok) return std::nullopt;
    return result.value_or(Type(Primitive::Void));
}

bool SemanticAnalyzer::process_fn_body(const ast::FnDecl& fd, Scope& target) {
    auto sym = target.lookup_local(fd.name);
    if (!sym) return true;  // ошибка уже была
    auto saved_ret = current_return_type_;
    current_return_type_ = sym->type;

    auto fn_scope = std::make_unique<Scope>(&target);
    for (std::size_t i = 0; i < fd.params.size(); ++i) {
        Symbol p; p.kind = Symbol::Kind::Var;
        p.name = fd.params[i].name;
        p.loc = fd.params[i].loc;
        p.type = sym->param_types[i];
        // Параметры локально мутабельны: язык call-by-value, изменения
        // не видны вызывающей стороне.
        p.is_mutable = true;
        if (!fn_scope->declare(std::move(p))) {
            error(fd.params[i].loc,
                  "duplicate parameter name '" + fd.params[i].name + "'");
            current_return_type_ = saved_ret;
            return false;
        }
    }
    bool ok = check_block(*fd.body, *fn_scope);
    current_return_type_ = saved_ret;
    return ok;
}

bool SemanticAnalyzer::process_namespace(const ast::NamespaceDecl& nd,
                                         Scope& target) {
    if (!check_not_builtin(nd.name, nd.loc)) return false;
    auto ns = std::make_shared<Scope>(&target);
    if (!process_decls(nd.members, *ns)) return false;
    Symbol s; s.kind = Symbol::Kind::Namespace; s.name = nd.name; s.loc = nd.loc;
    s.is_pub = nd.is_pub;
    s.ns_scope = std::move(ns);
    if (!target.declare(std::move(s))) {
        error(nd.loc, "redeclaration of name '" + nd.name + "'");
        return false;
    }
    return true;
}

bool SemanticAnalyzer::process_impl_signatures(const ast::ImplDecl& im,
                                               Scope& target) {
    auto sym = target.lookup(im.type_name);
    if (!sym || sym->kind != Symbol::Kind::TypeName) {
        error(im.loc, "impl target '" + im.type_name + "' is not a type");
        return false;
    }
    if (!sym->type.is_struct()) {
        error(im.loc, "impl is only allowed for struct types, got '" +
                      sym->type.to_string() + "'");
        return false;
    }
    auto* st_ptr = sym->type.struct_ptr().get();
    impl_method_offset_[&im] = methods_[st_ptr].size();

    for (const auto& fn : im.methods) {
        MethodInfo info;
        info.name = fn->name;
        info.module = prog_.module_name;
        info.loc = fn->loc;
        info.is_pub = fn->is_pub;
        info.ast_node = fn.get();

        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            const auto& p = fn->params[i];
            if (!check_not_builtin(p.name, p.loc)) return false;
            auto t = resolve_type(*p.type, target);
            if (!t) return false;
            if (t->is_void()) {
                error(p.loc, "parameter cannot have type 'void'");
                return false;
            }
            if (i == 0 && p.name == "self") {
                if (!t->same_as(sym->type)) {
                    error(p.loc, "parameter 'self' must have type '" +
                                 sym->type.to_string() + "', got '" +
                                 t->to_string() + "'");
                    return false;
                }
            }
            info.param_types.push_back(std::move(*t));
            info.param_names.push_back(p.name);
        }
        info.is_static = !(info.param_names.size() >= 1
                            && info.param_names[0] == "self");

        if (fn->return_type) {
            auto rt = resolve_type(*fn->return_type, target);
            if (!rt) return false;
            info.return_type = std::move(*rt);
        } else {
            auto rt = infer_return_type(*fn, target, info.param_types, st_ptr);
            if (!rt) return false;
            info.return_type = std::move(*rt);
        }
        fn_return_types_[fn.get()] = info.return_type;

        // Дубли запрещены среди методов одного типа.
        for (const auto& existing : methods_[st_ptr]) {
            if (existing.name == info.name) {
                error(fn->loc, "duplicate method '" + info.name +
                               "' for type '" + im.type_name + "'");
                return false;
            }
        }
        methods_[st_ptr].push_back(std::move(info));
    }
    return true;
}

bool SemanticAnalyzer::process_impl_bodies(const ast::ImplDecl& im,
                                           Scope& target) {
    auto sym = target.lookup(im.type_name);
    if (!sym) return true;
    auto* st_ptr = sym->type.struct_ptr().get();
    const auto& infos = methods_[st_ptr];
    auto off_it = impl_method_offset_.find(&im);
    std::size_t base = (off_it != impl_method_offset_.end()) ? off_it->second : 0;

    for (std::size_t k = 0; k < im.methods.size(); ++k) {
        const auto& fn = *im.methods[k];
        const auto& info = infos[base + k];
        auto saved_ret = current_return_type_;
        auto saved_impl = current_impl_type_;
        current_return_type_ = info.return_type;
        current_impl_type_ = st_ptr;  // priv-члены типа доступны в теле метода

        auto fn_scope = std::make_unique<Scope>(&target);
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            Symbol p; p.kind = Symbol::Kind::Var;
            p.name = fn.params[i].name;
            p.loc = fn.params[i].loc;
            p.type = info.param_types[i];
            p.is_mutable = true;
            if (!fn_scope->declare(std::move(p))) {
                error(fn.params[i].loc,
                      "duplicate parameter name '" + fn.params[i].name + "'");
                current_return_type_ = saved_ret;
                current_impl_type_ = saved_impl;
                return false;
            }
        }
        bool ok = check_block(*fn.body, *fn_scope);
        current_return_type_ = saved_ret;
        current_impl_type_ = saved_impl;
        if (!ok) return false;
    }
    return true;
}

namespace {
// Разбивает "M.NS.T" на части по точкам.
std::vector<std::string> split_dotted(std::string_view s) {
    std::vector<std::string> parts;
    while (true) {
        auto dot = s.find('.');
        if (dot == std::string_view::npos) {
            parts.emplace_back(s);
            return parts;
        }
        parts.emplace_back(s.substr(0, dot));
        s.remove_prefix(dot + 1);
    }
}
}  // anonymous

// Разрешение цепочки имён a.b.c по namespace/модулям. При пересечении
// границы модуля каждый следующий член обязан быть pub (semantics.md §14).
SemanticAnalyzer::ChainStatus SemanticAnalyzer::resolve_chain_parts(
        std::span<const std::string> parts, SourceLocation loc,
        Scope& scope, ChainHit& out) {
    auto sym = scope.lookup(parts[0]);
    if (!sym) return ChainStatus::NotChain;
    bool crossed_module = false;
    std::string path = parts[0];
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (sym->kind != Symbol::Kind::Namespace
            && sym->kind != Symbol::Kind::Module) {
            return ChainStatus::NotChain;  // например, переменная.поле
        }
        crossed_module = crossed_module || sym->kind == Symbol::Kind::Module;
        auto m = sym->ns_scope->lookup_local(parts[i]);
        if (!m) {
            error(loc, std::string(sym->kind == Symbol::Kind::Module
                                   ? "module '" : "namespace '") +
                       path + "' has no member '" + parts[i] + "'");
            return ChainStatus::Error;
        }
        if (crossed_module && !m->is_pub) {
            error(loc, "'" + parts[i] + "' is not exported from '" + path + "'");
            return ChainStatus::Error;
        }
        path += '.';
        path += parts[i];
        sym = m;
    }
    out.sym = sym;
    out.path = std::move(path);
    return ChainStatus::Found;
}

SemanticAnalyzer::ChainStatus SemanticAnalyzer::resolve_chain(
        const ast::Expr& e, Scope& scope, ChainHit& out) {
    // Собираем части цепочки; не-идентификаторные базы — не цепочка имён.
    std::vector<std::string> parts;
    const ast::Expr* cur = &e;
    while (auto* fe = dynamic_cast<const ast::FieldExpr*>(cur)) {
        parts.push_back(fe->field);
        cur = fe->base.get();
    }
    auto* id = dynamic_cast<const ast::IdentExpr*>(cur);
    if (!id) return ChainStatus::NotChain;
    parts.push_back(id->name);
    std::ranges::reverse(parts);
    return resolve_chain_parts(parts, e.loc, scope, out);
}

std::optional<Type> SemanticAnalyzer::resolve_type(const ast::TypeExpr& te,
                                                   Scope& scope) {
    auto record = [&](std::optional<Type> t) -> std::optional<Type> {
        if (t) type_expr_types_[&te] = *t;
        return t;
    };
    if (auto* nt = dynamic_cast<const ast::NamedType*>(&te)) {
        // Квалифицированное имя: Module.Type / Module.Namespace.Type.
        if (nt->name.find('.') != std::string::npos) {
            auto parts = split_dotted(nt->name);
            ChainHit hit;
            switch (resolve_chain_parts(parts, nt->loc, scope, hit)) {
                case ChainStatus::Error: return std::nullopt;
                case ChainStatus::NotChain:
                    error(nt->loc, "unknown type '" + nt->name + "'");
                    return std::nullopt;
                case ChainStatus::Found: break;
            }
            if (hit.sym->kind != Symbol::Kind::TypeName) {
                error(nt->loc, "'" + nt->name + "' is not a type");
                return std::nullopt;
            }
            return record(hit.sym->type);
        }
        auto sym = scope.lookup(nt->name);
        if (!sym || sym->kind != Symbol::Kind::TypeName) {
            error(nt->loc, "unknown type '" + nt->name + "'");
            return std::nullopt;
        }
        return record(sym->type);
    }
    if (auto* at = dynamic_cast<const ast::ArrayType*>(&te)) {
        auto elem = resolve_type(*at->element, scope);
        if (!elem) return std::nullopt;
        if (elem->is_void()) {
            error(at->loc, "array element type cannot be 'void'");
            return std::nullopt;
        }
        if (at->size < 0) {
            error(at->loc, "array size cannot be negative");
            return std::nullopt;
        }
        auto a = std::make_shared<ArrayTy>();
        a->element = std::move(*elem);
        a->size = at->size;
        return record(Type(std::move(a)));
    }
    // *T — указатель. *void и *byte считаются сырыми указателями.
    if (auto* pt = dynamic_cast<const ast::PointerType*>(&te)) {
        auto pointee = resolve_type(*pt->pointee, scope);
        if (!pointee) return std::nullopt;
        auto p = std::make_shared<PointerTy>();
        p->pointee = std::move(*pointee);
        p->is_raw = p->pointee.is(Primitive::Void) || p->pointee.is(Primitive::Byte);
        return record(Type(std::move(p)));
    }
    error(te.loc, "unsupported type expression");
    return std::nullopt;
}

bool SemanticAnalyzer::check_block(const ast::BlockStmt& b, Scope& parent) {
    auto scope = std::make_unique<Scope>(&parent);
    for (const auto& s : b.stmts) {
        if (!check_stmt(*s, *scope)) return false;
    }
    return true;
}

bool SemanticAnalyzer::check_stmt(const ast::Stmt& s, Scope& scope) {
    if (auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) return check_var_decl(*v, scope);
    if (auto* a = dynamic_cast<const ast::AssignStmt*>(&s))  return check_assign(*a, scope);
    if (auto* r = dynamic_cast<const ast::ReturnStmt*>(&s))  return check_return(*r, scope);
    if (auto* i = dynamic_cast<const ast::IfStmt*>(&s))      return check_if(*i, scope);
    if (auto* w = dynamic_cast<const ast::WhileStmt*>(&s))   return check_while(*w, scope);
    if (dynamic_cast<const ast::BreakStmt*>(&s)) {
        if (loop_depth_ == 0) {
            error(s.loc, "'break' outside of loop");
            return false;
        }
        return true;
    }
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (loop_depth_ == 0) {
            error(s.loc, "'continue' outside of loop");
            return false;
        }
        return true;
    }
    if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
        return check_expr(*es->expr, scope).has_value();
    }
    if (dynamic_cast<const ast::EmptyStmt*>(&s)) return true;
    if (auto* b = dynamic_cast<const ast::BlockStmt*>(&s)) return check_block(*b, scope);
    error(s.loc, "unsupported statement");
    return false;
}

bool SemanticAnalyzer::check_not_builtin(std::string_view name,
                                         SourceLocation loc) {
    if (!is_reserved_builtin(name)) return true;
    error(loc, std::format(
        "'{}' is a builtin name and cannot be used as an identifier", name));
    return false;
}

bool SemanticAnalyzer::check_var_decl(const ast::VarDeclStmt& v, Scope& scope) {
    if (!check_not_builtin(v.name, v.loc)) return false;
    Type var_type;
    if (v.type) {
        auto t = resolve_type(*v.type, scope);
        if (!t) return false;
        if (t->is_void()) {
            error(v.loc, "variable cannot have type 'void'");
            return false;
        }
        var_type = std::move(*t);
        auto init = check_expr_ctx(*v.init, scope, var_type);
        if (!init) return false;
    } else {
        // вывод типа через :=
        auto init = check_expr(*v.init, scope);
        if (!init) return false;
        if (init->is_void()) {
            error(v.loc, "cannot infer 'void' as variable type");
            return false;
        }
        var_type = std::move(*init);
    }
    Symbol s; s.kind = Symbol::Kind::Var; s.name = v.name;
    s.loc = v.loc; s.type = var_type; s.is_mutable = v.is_mutable;
    if (!scope.declare(std::move(s))) {
        error(v.loc, "redeclaration of '" + v.name + "' in this scope");
        return false;
    }
    return true;
}

namespace {
// Поиск корневого идентификатора в цепочке `a.b[i].c.d`.
// Возвращает nullptr для не-lvalue-выражений (литералы, вызовы и т.п.).
const ast::IdentExpr* root_ident(const ast::Expr& e) {
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) return id;
    if (auto* f = dynamic_cast<const ast::FieldExpr*>(&e))  return root_ident(*f->base);
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) return root_ident(*ix->base);
    return nullptr;
}
}  // anonymous

bool SemanticAnalyzer::check_assign(const ast::AssignStmt& a, Scope& scope) {
    if (!is_lvalue(*a.target)) {
        error(a.target->loc,
              "left-hand side of assignment is not an lvalue");
        return false;
    }
    // Mutability — раскручиваем цепочку до корневой переменной:
    // `s.x = ...` или `arr[i].field = ...` запрещены, если корень — `let`.
    if (auto* root = root_ident(*a.target)) {
        auto sym = scope.lookup(root->name);
        if (!sym) {
            error(root->loc, "unknown identifier '" + root->name + "'");
            return false;
        }
        if (sym->kind != Symbol::Kind::Var) {
            error(root->loc, "cannot assign to non-variable '" + root->name + "'");
            return false;
        }
        if (!sym->is_mutable) {
            error(a.target->loc, "cannot assign through immutable '" + root->name +
                                 "' (declared with 'let')");
            return false;
        }
    }
    auto lhs = check_expr(*a.target, scope);
    if (!lhs) return false;
    auto rhs = check_expr_ctx(*a.value, scope, *lhs);
    if (!rhs) return false;
    return true;
}

bool SemanticAnalyzer::check_return(const ast::ReturnStmt& r, Scope& scope) {
    // Режим вывода типа возврата (A.1.7): типы всех return должны совпадать.
    if (inferring_return_) {
        Type t{Primitive::Void};
        if (r.value) {
            auto v = check_expr(*r.value, scope);
            if (!v) return false;
            t = std::move(*v);
        }
        if (!inferred_return_) {
            inferred_return_ = t;
            return true;
        }
        if (!inferred_return_->same_as(t)) {
            error(r.loc, std::format(
                "conflicting return types in function with inferred return "
                "type: '{}' vs '{}'",
                inferred_return_->to_string(), t.to_string()));
            return false;
        }
        return true;
    }
    if (!r.value) {
        if (!current_return_type_.is_void()) {
            error(r.loc, "'return' without value in function returning '" +
                         current_return_type_.to_string() + "'");
            return false;
        }
        return true;
    }
    if (current_return_type_.is_void()) {
        error(r.loc, "'return' with value in void function");
        return false;
    }
    return check_expr_ctx(*r.value, scope, current_return_type_).has_value();
}

bool SemanticAnalyzer::check_if(const ast::IfStmt& i, Scope& scope) {
    auto c = check_expr(*i.cond, scope);
    if (!c) return false;
    if (!c->is_bool()) {
        error(i.cond->loc, "'if' condition must be 'bool', got '" +
                           c->to_string() + "'");
        return false;
    }
    if (!check_block(*i.then_branch, scope)) return false;
    if (i.else_branch) {
        if (!check_stmt(*i.else_branch, scope)) return false;
    }
    return true;
}

bool SemanticAnalyzer::check_while(const ast::WhileStmt& w, Scope& scope) {
    auto c = check_expr(*w.cond, scope);
    if (!c) return false;
    if (!c->is_bool()) {
        error(w.cond->loc, "'while' condition must be 'bool', got '" +
                           c->to_string() + "'");
        return false;
    }
    ++loop_depth_;
    bool ok = check_block(*w.body, scope);
    --loop_depth_;
    return ok;
}

bool SemanticAnalyzer::is_lvalue(const ast::Expr& e) const {
    return dynamic_cast<const ast::IdentExpr*>(&e) != nullptr
        || dynamic_cast<const ast::FieldExpr*>(&e) != nullptr
        || dynamic_cast<const ast::IndexExpr*>(&e) != nullptr
        || dynamic_cast<const ast::DerefExpr*>(&e) != nullptr;  // *p = ... тоже сюда
}

std::optional<Type> SemanticAnalyzer::check_expr(const ast::Expr& e, Scope& scope) {
    auto record = [&](std::optional<Type> t) -> std::optional<Type> {
        if (t) expr_types_[&e] = *t;
        return t;
    };
    if (auto* il = dynamic_cast<const ast::IntLit*>(&e)) {
        // Суффикс задаёт тип литерала явно; без суффикса — int32, а если
        // значение в int32 не помещается — int64 (uint64 для значений выше
        // int64::max, доступных только этому типу).
        if (!il->suffix.empty()) {
            auto p = suffix_to_prim(il->suffix);
            if (!p) {
                error(e.loc, "invalid literal suffix '" + il->suffix + "'");
                return std::nullopt;
            }
            bool fits = il->u64_only ? (*p == Primitive::U64)
                                     : int_fits(il->value, *p);
            if (!fits) {
                error(e.loc, std::format(
                    "integer literal does not fit in '{}'", Type(*p).to_string()));
                return std::nullopt;
            }
            return record(Type(*p));
        }
        if (il->u64_only) return record(Type(Primitive::U64));
        return record(Type(int_fits(il->value, Primitive::I32)
                               ? Primitive::I32 : Primitive::I64));
    }
    if (auto* fl = dynamic_cast<const ast::FloatLit*>(&e)) {
        if (fl->suffix == "f32") {
            if (std::isfinite(fl->value)
                && std::abs(fl->value) > std::numeric_limits<float>::max()) {
                error(e.loc, "float literal does not fit in 'float32'");
                return std::nullopt;
            }
            return record(Type(Primitive::F32));
        }
        return record(Type(Primitive::F64));
    }
    if (dynamic_cast<const ast::BoolLit*>(&e))   return record(Type(Primitive::Bool));
    if (dynamic_cast<const ast::StringLit*>(&e)) return record(Type(Primitive::String));
    if (dynamic_cast<const ast::CharLit*>(&e))   return record(Type(Primitive::Char));
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) {
        auto sym = scope.lookup(id->name);
        if (!sym) {
            error(id->loc, "unknown identifier '" + id->name + "'");
            return std::nullopt;
        }
        if (sym->kind != Symbol::Kind::Var) {
            error(id->loc, "'" + id->name + "' is not a value");
            return std::nullopt;
        }
        return record(sym->type);
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))     return record(check_unary(*u, scope));
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e))    return record(check_binary(*b, scope));
    if (auto* i = dynamic_cast<const ast::IndexExpr*>(&e))     return record(check_index(*i, scope));
    if (auto* f = dynamic_cast<const ast::FieldExpr*>(&e))     return record(check_field(*f, scope));
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e))      return record(check_call(*c, scope));
    if (auto* a = dynamic_cast<const ast::ArrayLit*>(&e))      return record(check_array_lit(*a, scope));
    if (auto* s = dynamic_cast<const ast::StructLit*>(&e))     return record(check_struct_lit(*s, scope));
    // Указатели
    if (dynamic_cast<const ast::NullLit*>(&e)) {
        // null имеет тип *void (raw) и приводится к любому *T неявно
        auto p = std::make_shared<PointerTy>();
        p->pointee = Type(Primitive::Void);
        p->is_raw = true;
        return record(Type(std::move(p)));
    }
    if (auto* ao = dynamic_cast<const ast::AddressOfExpr*>(&e)) {
        // &x допустим только на целой переменной: адрес поля или элемента
        // массива указывал бы на временную копию (value semantics), и запись
        // через такой указатель молча терялась бы. Честная реализация
        // требует lvalue-адресации в IR; до неё — ошибка компиляции.
        auto* id = dynamic_cast<const ast::IdentExpr*>(ao->operand.get());
        if (!id) {
            error(ao->loc,
                  "'&' is supported only on whole variables; take the address "
                  "of the variable and access parts through it");
            return std::nullopt;
        }
        auto sym = scope.lookup(id->name);
        if (sym && sym->kind == Symbol::Kind::Fn) {
            error(ao->loc,
                  "function pointers are not yet supported (use direct call)");
            return std::nullopt;
        }
        auto inner = check_expr(*ao->operand, scope);
        if (!inner) return std::nullopt;
        auto p = std::make_shared<PointerTy>();
        p->pointee = std::move(*inner);
        return record(Type(std::move(p)));
    }
    if (auto* de = dynamic_cast<const ast::DerefExpr*>(&e)) {
        auto inner = check_expr(*de->operand, scope);
        if (!inner) return std::nullopt;
        if (!inner->is_pointer()) {
            error(de->loc, "'*' requires a pointer operand, got '" +
                            inner->to_string() + "'");
            return std::nullopt;
        }
        if (inner->pointer().pointee.is_void()) {
            error(de->loc, "cannot dereference '*void'; cast to a typed pointer first");
            return std::nullopt;
        }
        return record(inner->pointer().pointee);
    }
    error(e.loc, "unsupported expression");
    return std::nullopt;
}

std::optional<Type> SemanticAnalyzer::check_expr_ctx(const ast::Expr& e,
                                                     Scope& scope,
                                                     const Type& ctx) {
    auto record = [&](Type t) -> std::optional<Type> {
        expr_types_[&e] = t;
        return t;
    };
    // Хелпер: помещается ли значение вещественного литерала в тип ctx.
    auto float_fits_ctx = [&](double v, SourceLocation loc) -> bool {
        if (ctx.is(Primitive::F32) && std::isfinite(v)
            && std::abs(v) > std::numeric_limits<float>::max()) {
            error(loc, "float literal does not fit in 'float32'");
            return false;
        }
        return true;
    };
    // Литерал → подгоняем к контексту. Литералы с суффиксом уже типизированы
    // и адаптации не подлежат (идут по общему пути с неявным приведением).
    if (auto* il = dynamic_cast<const ast::IntLit*>(&e);
        il && il->suffix.empty() && !il->u64_only) {
        if (ctx.is_primitive() && is_int(ctx.prim())) {
            if (int_fits(il->value, ctx.prim())) return record(ctx);
            error(e.loc, std::format(
                "integer literal {} does not fit in '{}'",
                il->value, ctx.to_string()));
            return std::nullopt;
        }
        if (ctx.is_primitive() && is_float(ctx.prim())) return record(ctx);
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e);
        un && un->op == ast::UnaryOp::Neg) {
        // -<целый литерал>: адаптируется и к целому, и к вещественному ctx.
        if (auto* il = dynamic_cast<const ast::IntLit*>(un->operand.get());
            il && il->suffix.empty() && !il->u64_only) {
            if (ctx.is_primitive() && is_int(ctx.prim())) {
                if (int_fits(-il->value, ctx.prim())) {
                    expr_types_[un->operand.get()] = ctx;  // вложенный IntLit
                    return record(ctx);
                }
                error(e.loc, std::format(
                    "integer literal {} does not fit in '{}'",
                    -il->value, ctx.to_string()));
                return std::nullopt;
            }
            if (ctx.is_primitive() && is_float(ctx.prim())) {
                expr_types_[un->operand.get()] = ctx;
                return record(ctx);
            }
        }
        // -<вещественный литерал>: симметрично положительному (fix Н-5).
        if (auto* fl = dynamic_cast<const ast::FloatLit*>(un->operand.get());
            fl && fl->suffix.empty()) {
            if (ctx.is_primitive() && is_float(ctx.prim())) {
                if (!float_fits_ctx(fl->value, e.loc)) return std::nullopt;
                expr_types_[un->operand.get()] = ctx;
                return record(ctx);
            }
        }
    }
    if (auto* fl = dynamic_cast<const ast::FloatLit*>(&e);
        fl && fl->suffix.empty()) {
        if (ctx.is_primitive() && is_float(ctx.prim())) {
            if (!float_fits_ctx(fl->value, e.loc)) return std::nullopt;
            return record(ctx);
        }
    }
    // Литерал массива — каждый элемент в контексте element_type.
    if (auto* al = dynamic_cast<const ast::ArrayLit*>(&e); al && ctx.is_array()) {
        if (static_cast<std::int64_t>(al->elements.size()) != ctx.array().size) {
            error(e.loc, std::format(
                "array literal has {} elements, expected {} (type '{}')",
                al->elements.size(), ctx.array().size, ctx.to_string()));
            return std::nullopt;
        }
        for (const auto& el : al->elements) {
            if (!check_expr_ctx(*el, scope, ctx.array().element)) return std::nullopt;
        }
        return record(ctx);
    }
    // Иначе — обычная проверка + конверсия.
    auto t = check_expr(e, scope);
    if (!t) return std::nullopt;
    if (!can_implicit_convert(*t, ctx)) {
        error(e.loc, std::format(
            "cannot convert '{}' to '{}'",
            t->to_string(), ctx.to_string()));
        return std::nullopt;
    }
    return record(ctx);
}

std::optional<Type> SemanticAnalyzer::check_unary(const ast::UnaryExpr& u,
                                                  Scope& scope) {
    auto t = check_expr(*u.operand, scope);
    if (!t) return std::nullopt;
    if (u.op == ast::UnaryOp::Not) {
        if (!t->is_bool()) {
            error(u.loc, "'!' requires bool, got '" + t->to_string() + "'");
            return std::nullopt;
        }
        return Type(Primitive::Bool);
    }
    // Neg
    if (!t->is_primitive() || !is_numeric(t->prim())) {
        error(u.loc, "unary '-' requires numeric type, got '" + t->to_string() + "'");
        return std::nullopt;
    }
    return *t;
}

std::optional<Type> SemanticAnalyzer::check_binary(const ast::BinaryExpr& b,
                                                   Scope& scope) {
    auto a = check_expr(*b.lhs, scope);
    if (!a) return std::nullopt;
    auto c = check_expr(*b.rhs, scope);
    if (!c) return std::nullopt;

    using Op = ast::BinaryOp;
    auto op = b.op;

    if (op == Op::And || op == Op::Or) {
        if (!a->is_bool() || !c->is_bool()) {
            error(b.loc, std::format("'{}' requires bool operands, got '{}' and '{}'",
                  to_string(op), a->to_string(), c->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::Bool);
    }

    // String == / !=
    if ((op == Op::Eq || op == Op::NotEq) && a->is_string() && c->is_string()) {
        return Type(Primitive::Bool);
    }
    // String concatenation
    if (op == Op::Add && a->is_string() && c->is_string()) {
        return Type(Primitive::String);
    }
    // Bool == / !=
    if ((op == Op::Eq || op == Op::NotEq) && a->is_bool() && c->is_bool()) {
        return Type(Primitive::Bool);
    }
    // Указатели: == и != (включая сравнение с null). Типизированные
    // указатели сравнимы при совпадении pointee; raw (*void/*byte) — с любым.
    if ((op == Op::Eq || op == Op::NotEq) && a->is_pointer() && c->is_pointer()) {
        bool compatible = a->pointer().is_raw || c->pointer().is_raw
                       || a->pointer().pointee.same_as(c->pointer().pointee);
        if (!compatible) {
            error(b.loc, std::format(
                "cannot compare pointers of different types: '{}' and '{}'",
                a->to_string(), c->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::Bool);
    }
    if (a->is_pointer() || c->is_pointer()) {
        error(b.loc, std::format(
            "operator '{}' is not defined for pointers (only == and !=)",
            to_string(op)));
        return std::nullopt;
    }
    // byte: только == и != — тип не арифметический.
    if ((op == Op::Eq || op == Op::NotEq)
        && a->is(Primitive::Byte) && c->is(Primitive::Byte)) {
        return Type(Primitive::Bool);
    }
    if (a->is(Primitive::Byte) || c->is(Primitive::Byte)) {
        error(b.loc, std::format(
            "operator '{}' is not defined for byte (only == and != are allowed)",
            to_string(op)));
        return std::nullopt;
    }
    // У char доступны только == и !=, потому что он не арифметический.
    if ((op == Op::Eq || op == Op::NotEq)
        && a->is(Primitive::Char) && c->is(Primitive::Char)) {
        return Type(Primitive::Bool);
    }
    if (a->is(Primitive::Char) || c->is(Primitive::Char)) {
        error(b.loc, std::format(
            "operator '{}' is not defined for char (only == and != are allowed)",
            to_string(op)));
        return std::nullopt;
    }
    // Array == / != — поэлементно, типы должны полностью совпадать.
    if ((op == Op::Eq || op == Op::NotEq) && a->is_array() && c->is_array()) {
        if (!a->same_as(*c)) {
            error(b.loc, std::format(
                "cannot compare arrays of different types: '{}' and '{}'",
                a->to_string(), c->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::Bool);
    }
    // Struct == / != — номинально (по имени типа), поэлементно по полям.
    if ((op == Op::Eq || op == Op::NotEq) && a->is_struct() && c->is_struct()) {
        if (!a->same_as(*c)) {
            error(b.loc, std::format(
                "cannot compare structs of different types: '{}' and '{}'",
                a->to_string(), c->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::Bool);
    }

    // Numeric ops.
    if (!a->is_primitive() || !c->is_primitive()) {
        error(b.loc, std::format("operator '{}' not defined for '{}' and '{}'",
              to_string(op), a->to_string(), c->to_string()));
        return std::nullopt;
    }
    auto common = common_arith(a->prim(), c->prim());
    if (!common) {
        error(b.loc, std::format("incompatible operand types '{}' and '{}' for '{}'",
              a->to_string(), c->to_string(), to_string(op)));
        return std::nullopt;
    }

    if (op == Op::Mod) {
        if (!is_int(*common)) {
            error(b.loc, "'%' is defined only for integer types");
            return std::nullopt;
        }
    }
    switch (op) {
        case Op::Add: case Op::Sub: case Op::Mul:
        case Op::Div: case Op::Mod:
            return Type(*common);
        case Op::Eq: case Op::NotEq:
        case Op::Lt: case Op::Gt: case Op::LtEq: case Op::GtEq:
            return Type(Primitive::Bool);
        default: break;
    }
    error(b.loc, "internal: unhandled binary op");
    return std::nullopt;
}

std::optional<Type> SemanticAnalyzer::check_index(const ast::IndexExpr& e,
                                                  Scope& scope) {
    auto base = check_expr(*e.base, scope);
    if (!base) return std::nullopt;
    if (!base->is_array()) {
        error(e.loc, "indexing requires array type, got '" + base->to_string() + "'");
        return std::nullopt;
    }
    auto idx = check_expr(*e.index, scope);
    if (!idx) return std::nullopt;
    if (!idx->is_primitive() || !is_int(idx->prim())) {
        error(e.index->loc, "array index must be integer, got '" + idx->to_string() + "'");
        return std::nullopt;
    }
    return base->array().element;
}

std::optional<Type> SemanticAnalyzer::check_field(const ast::FieldExpr& e,
                                                  Scope& scope) {
    // Возможен: namespace.x | module.x | module.ns.x | struct_value.field.
    ChainHit hit;
    switch (resolve_chain(e, scope, hit)) {
        case ChainStatus::Error: return std::nullopt;
        case ChainStatus::Found:
            if (hit.sym->kind == Symbol::Kind::Var) return hit.sym->type;
            error(e.loc, "cannot use '" + hit.path + "' as a value here");
            return std::nullopt;
        case ChainStatus::NotChain: break;  // значение → доступ к полю
    }
    auto base = check_expr(*e.base, scope);
    if (!base) return std::nullopt;
    if (!base->is_struct()) {
        error(e.loc, "field access requires struct, got '" + base->to_string() + "'");
        return std::nullopt;
    }
    for (const auto& f : base->strukt().fields) {
        if (f.name == e.field) {
            // priv-поле доступно только методам своего типа (A.2.12).
            if (f.is_priv && current_impl_type_ != base->struct_ptr().get()) {
                error(e.loc, "field '" + e.field + "' of struct '" +
                             base->strukt().name +
                             "' is private (accessible only from its methods)");
                return std::nullopt;
            }
            return f.type;
        }
    }
    error(e.loc, "struct '" + base->strukt().name + "' has no field '" + e.field + "'");
    return std::nullopt;
}

std::optional<Type> SemanticAnalyzer::check_call(const ast::CallExpr& e,
                                                 Scope& scope) {
    // print — особый builtin (полиморфный по аргументу).
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(e.callee.get());
        id && id->name == "print") {
        return check_builtin_print(e, scope);
    }
    // len — полиморфный builtin (string или array → int32).
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(e.callee.get());
        id && id->name == "len") {
        if (e.args.size() != 1) {
            error(e.loc, std::format("'len' expects 1 argument, got {}", e.args.size()));
            return std::nullopt;
        }
        auto t = check_expr(*e.args[0], scope);
        if (!t) return std::nullopt;
        if (!t->is_string() && !t->is_array()) {
            error(e.args[0]->loc, std::format(
                "'len' requires string or array, got '{}'", t->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::I32);
    }
    // assert(cond: bool) void — builtin.
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(e.callee.get());
        id && id->name == "assert") {
        if (e.args.size() != 1) {
            error(e.loc, std::format(
                "'assert' expects 1 argument, got {}", e.args.size()));
            return std::nullopt;
        }
        auto t = check_expr(*e.args[0], scope);
        if (!t) return std::nullopt;
        if (!t->is_bool()) {
            error(e.args[0]->loc, std::format(
                "'assert' condition must be 'bool', got '{}'", t->to_string()));
            return std::nullopt;
        }
        return Type(Primitive::Void);
    }

    // Резолвим callee:
    //   * IdentExpr — function | type-cast | builtin
    //   * FieldExpr-цепочка имён — Module.x | NS.x | Module.NS.x |
    //     T.static_method | Module.T.static_method
    //   * FieldExpr(<expr>, name) — method call на инстансе
    const Symbol* callee_sym = nullptr;
    std::string callee_name;
    SourceLocation callee_loc;

    // Проверка доступа к методу: без pub метод приватен и доступен только
    // из методов своего типа (ТЗ A.2.12).
    auto method_access_ok = [&](const MethodInfo& m, StructTy* st,
                                std::string_view type_name) -> bool {
        if (m.is_pub || current_impl_type_ == st) return true;
        error(e.loc, std::format(
            "method '{}.{}' is private (accessible only from methods of '{}')",
            type_name, m.name, type_name));
        return false;
    };
    // Вызов статического метода st_ptr.field(args). nullopt — метода нет.
    auto try_static_method = [&](StructTy* st_ptr, std::string_view type_name,
                                 const std::string& mname)
            -> std::optional<std::optional<Type>> {
        auto it = methods_.find(st_ptr);
        if (it == methods_.end()) return std::nullopt;
        for (const auto& m : it->second) {
            if (m.name != mname || !m.is_static) continue;
            if (!method_access_ok(m, st_ptr, type_name))
                return std::optional<std::optional<Type>>{std::optional<Type>{}};
            if (e.args.size() != m.param_types.size()) {
                error(e.loc, std::format(
                    "method '{}.{}' expects {} arguments, got {}",
                    type_name, m.name, m.param_types.size(), e.args.size()));
                return std::optional<std::optional<Type>>{std::optional<Type>{}};
            }
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                if (!check_expr_ctx(*e.args[i], scope, m.param_types[i]))
                    return std::optional<std::optional<Type>>{std::optional<Type>{}};
            }
            return std::optional<std::optional<Type>>{m.return_type};
        }
        return std::nullopt;
    };

    if (auto* id = dynamic_cast<const ast::IdentExpr*>(e.callee.get())) {
        callee_sym = scope.lookup(id->name);
        callee_name = id->name;
        callee_loc = id->loc;
    } else if (auto* fe = dynamic_cast<const ast::FieldExpr*>(e.callee.get())) {
        // Попытка 1: вся цепочка — имя в namespace/модуле (функция или тип).
        ChainHit hit;
        switch (resolve_chain(*fe, scope, hit)) {
            case ChainStatus::Error: return std::nullopt;
            case ChainStatus::Found:
                callee_sym = hit.sym;
                callee_name = hit.path;
                callee_loc = fe->loc;
                break;
            case ChainStatus::NotChain: break;
        }
        // Попытка 2: статический метод T.m(...) или Module.T.m(...):
        // база цепочки — тип-структура.
        if (!callee_sym) {
            ChainHit base_hit;
            auto base_stat = resolve_chain(*fe->base, scope, base_hit);
            if (base_stat == ChainStatus::Error) return std::nullopt;
            if (base_stat == ChainStatus::Found
                && base_hit.sym->kind == Symbol::Kind::TypeName
                && base_hit.sym->type.is_struct()) {
                auto* st_ptr = base_hit.sym->type.struct_ptr().get();
                if (auto r = try_static_method(st_ptr, base_hit.path, fe->field)) {
                    return *r;
                }
                error(e.loc, "type '" + base_hit.path +
                             "' has no static method '" + fe->field + "'");
                return std::nullopt;
            }
        }
        // Попытка 3: instance-метод на выражении: obj.method(args)
        if (!callee_sym) {
            auto base_ty = check_expr(*fe->base, scope);
            if (!base_ty) return std::nullopt;
            if (!base_ty->is_struct()) {
                error(e.loc, "method call on non-struct type '" + base_ty->to_string() + "'");
                return std::nullopt;
            }
            auto* st_ptr = base_ty->struct_ptr().get();
            auto it = methods_.find(st_ptr);
            if (it != methods_.end()) {
                for (const auto& m : it->second) {
                    if (m.name == fe->field && !m.is_static) {
                        if (!method_access_ok(m, st_ptr, base_ty->to_string()))
                            return std::nullopt;
                        std::size_t need = m.param_types.size() - 1;  // -self
                        if (e.args.size() != need) {
                            error(e.loc, std::format(
                                "method '{}.{}' expects {} arguments, got {}",
                                base_ty->to_string(), m.name, need, e.args.size()));
                            return std::nullopt;
                        }
                        for (std::size_t i = 0; i < e.args.size(); ++i) {
                            if (!check_expr_ctx(*e.args[i], scope, m.param_types[i + 1]))
                                return std::nullopt;
                        }
                        return m.return_type;
                    }
                }
            }
            error(e.loc, "type '" + base_ty->to_string() +
                         "' has no method '" + fe->field + "'");
            return std::nullopt;
        }
    } else {
        error(e.loc, "callee must be a function, type, or method");
        return std::nullopt;
    }

    if (!callee_sym) {
        error(callee_loc, "unknown identifier '" + callee_name + "'");
        return std::nullopt;
    }

    if (callee_sym->kind == Symbol::Kind::TypeName) {
        // cast
        if (e.args.size() != 1) {
            error(e.loc, std::format("cast to '{}' expects 1 argument, got {}",
                  callee_name, e.args.size()));
            return std::nullopt;
        }
        auto from = check_expr(*e.args[0], scope);
        if (!from) return std::nullopt;
        if (!can_explicit_cast(*from, callee_sym->type)) {
            error(e.loc, std::format("cannot cast '{}' to '{}'",
                  from->to_string(), callee_sym->type.to_string()));
            return std::nullopt;
        }
        return callee_sym->type;
    }

    if (callee_sym->kind != Symbol::Kind::Fn) {
        error(callee_loc, "'" + callee_name + "' is not callable");
        return std::nullopt;
    }

    if (e.args.size() != callee_sym->param_types.size()) {
        error(e.loc, std::format("function '{}' expects {} arguments, got {}",
              callee_name, callee_sym->param_types.size(), e.args.size()));
        return std::nullopt;
    }
    for (std::size_t i = 0; i < e.args.size(); ++i) {
        if (!check_expr_ctx(*e.args[i], scope, callee_sym->param_types[i])) {
            return std::nullopt;
        }
    }
    return callee_sym->type;
}

std::optional<Type> SemanticAnalyzer::check_builtin_print(const ast::CallExpr& e,
                                                          Scope& scope) {
    if (e.args.size() != 1) {
        error(e.loc, std::format("'print' expects 1 argument, got {}", e.args.size()));
        return std::nullopt;
    }
    auto t = check_expr(*e.args[0], scope);
    if (!t) return std::nullopt;
    // Печатаются только числовые скаляры, bool, char и string; указатели,
    // byte и агрегаты не имеют текстового представления.
    if (!t->is_primitive() || t->is_void() || t->is(Primitive::Byte)) {
        error(e.args[0]->loc, "'print' accepts only scalar types or string, got '" +
                              t->to_string() + "'");
        return std::nullopt;
    }
    return Type(Primitive::Void);
}

std::optional<Type> SemanticAnalyzer::check_array_lit(const ast::ArrayLit& al,
                                                      Scope& scope) {
    if (al.elements.empty()) {
        error(al.loc, "empty array literal has no inferable type "
                      "(use a typed declaration to specify element type)");
        return std::nullopt;
    }
    auto first = check_expr(*al.elements[0], scope);
    if (!first) return std::nullopt;
    for (std::size_t i = 1; i < al.elements.size(); ++i) {
        auto t = check_expr(*al.elements[i], scope);
        if (!t) return std::nullopt;
        if (!t->same_as(*first)) {
            error(al.elements[i]->loc, std::format(
                "array element {} has type '{}', expected '{}'",
                i, t->to_string(), first->to_string()));
            return std::nullopt;
        }
    }
    auto a = std::make_shared<ArrayTy>();
    a->element = std::move(*first);
    a->size = static_cast<std::int64_t>(al.elements.size());
    return Type(std::move(a));
}

std::optional<Type> SemanticAnalyzer::check_struct_lit(const ast::StructLit& sl,
                                                       Scope& scope) {
    // Имя типа может быть квалифицированным: Module.Vec2, Module.NS.T.
    const Symbol* sym = nullptr;
    if (sl.type_name.find('.') != std::string::npos) {
        auto parts = split_dotted(sl.type_name);
        ChainHit hit;
        switch (resolve_chain_parts(parts, sl.loc, scope, hit)) {
            case ChainStatus::Error: return std::nullopt;
            case ChainStatus::NotChain: break;
            case ChainStatus::Found: sym = hit.sym; break;
        }
    } else {
        sym = scope.lookup(sl.type_name);
    }
    if (!sym || sym->kind != Symbol::Kind::TypeName || !sym->type.is_struct()) {
        error(sl.loc, "'" + sl.type_name + "' is not a struct type");
        return std::nullopt;
    }
    const auto& st = sym->type.strukt();
    if (sl.fields.size() != st.fields.size()) {
        error(sl.loc, std::format(
            "struct literal for '{}' has {} fields, expected {}",
            st.name, sl.fields.size(), st.fields.size()));
        return std::nullopt;
    }
    // Спека требует фиксированного порядка полей.
    for (std::size_t i = 0; i < st.fields.size(); ++i) {
        const auto& expected = st.fields[i];
        if (sl.fields[i].name != expected.name) {
            error(sl.fields[i].loc, std::format(
                "expected field '{}' at position {}, got '{}'",
                expected.name, i, sl.fields[i].name));
            return std::nullopt;
        }
        // Структуру с priv-полем можно сконструировать только в её методах.
        if (expected.is_priv
            && current_impl_type_ != sym->type.struct_ptr().get()) {
            error(sl.fields[i].loc, std::format(
                "field '{}' of struct '{}' is private: the literal is allowed "
                "only inside methods of '{}'",
                expected.name, st.name, st.name));
            return std::nullopt;
        }
        if (!check_expr_ctx(*sl.fields[i].value, scope, expected.type)) {
            return std::nullopt;
        }
    }
    return sym->type;
}

}  // namespace herta::semantic
