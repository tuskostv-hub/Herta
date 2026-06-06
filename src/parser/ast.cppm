// Module `herta.ast` — абстрактное синтаксическое дерево языка Herta.
// Покрывает грамматику §3 specs/grammar.md.
//
// Узлы AST построены как class hierarchy с виртуальным деструктором
// и `dump()` для печати через `--dump-ast`. Дети хранятся через
// std::unique_ptr<T>. Все узлы помнят SourceLocation для диагностики.

export module herta.ast;

import std;
import herta.common;

namespace herta::ast {

using herta::common::SourceLocation;

// ===========================================================================
// Операторы
// ===========================================================================

export enum class UnaryOp : std::uint8_t {
    Neg,   // -x
    Not,   // !x
};

export enum class BinaryOp : std::uint8_t {
    // Арифметика
    Add, Sub, Mul, Div, Mod,
    // Сравнения
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    // Логика
    And, Or,
};

export std::string_view to_string(UnaryOp op) noexcept {
    switch (op) {
        case UnaryOp::Neg: return "-";
        case UnaryOp::Not: return "!";
    }
    return "?";
}

export std::string_view to_string(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Add:   return "+";
        case BinaryOp::Sub:   return "-";
        case BinaryOp::Mul:   return "*";
        case BinaryOp::Div:   return "/";
        case BinaryOp::Mod:   return "%";
        case BinaryOp::Eq:    return "==";
        case BinaryOp::NotEq: return "!=";
        case BinaryOp::Lt:    return "<";
        case BinaryOp::Gt:    return ">";
        case BinaryOp::LtEq:  return "<=";
        case BinaryOp::GtEq:  return ">=";
        case BinaryOp::And:   return "&&";
        case BinaryOp::Or:    return "||";
    }
    return "?";
}

// ===========================================================================
// Forward declarations
// ===========================================================================

export struct Expr;
export struct Stmt;
export struct TypeExpr;
export struct Decl;
export struct BlockStmt;

// Утилита: печать с отступом.
inline void indent(std::ostream& os, int level) {
    for (int i = 0; i < level; ++i) os << "  ";
}

// ===========================================================================
// TypeExpr — выражение типа (§3.5 grammar.md)
// ===========================================================================

export struct TypeExpr {
    SourceLocation loc;
    virtual ~TypeExpr() = default;
    virtual void dump(std::ostream& os, int level) const = 0;
};

// `int32`, `Point`, `Meters` — простое имя типа.
export struct NamedType : TypeExpr {
    std::string name;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "NamedType '" << name << "'\n";
    }
};

// `[T; N]` — массив фиксированного размера.
export struct ArrayType : TypeExpr {
    std::unique_ptr<TypeExpr> element;
    std::int64_t size = 0;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "ArrayType size=" << size << '\n';
        element->dump(os, level + 1);
    }
};

// `*T` — указатель на T (A.2.14). T может быть `void` (raw pointer, A.3.7).
export struct PointerType : TypeExpr {
    std::unique_ptr<TypeExpr> pointee;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "PointerType\n";
        pointee->dump(os, level + 1);
    }
};

// `fn(T1, T2) R` — указатель на функцию (A.3.7).
export struct FnPointerType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> params;
    std::unique_ptr<TypeExpr> return_type;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "FnPointerType\n";
        for (const auto& p : params) p->dump(os, level + 1);
        indent(os, level + 1); os << "->\n";
        return_type->dump(os, level + 2);
    }
};

// ===========================================================================
// Expr — выражения (§3.4 grammar.md)
// ===========================================================================

export struct Expr {
    SourceLocation loc;
    virtual ~Expr() = default;
    virtual void dump(std::ostream& os, int level) const = 0;
};

export struct IntLit : Expr {
    std::int64_t value = 0;
    std::string lexeme;  // оригинальное представление (для диагностики)

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "IntLit " << value << '\n';
    }
};

export struct FloatLit : Expr {
    double value = 0.0;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "FloatLit " << value << '\n';
    }
};

export struct BoolLit : Expr {
    bool value = false;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "BoolLit " << (value ? "true" : "false") << '\n';
    }
};

export struct StringLit : Expr {
    std::string value;   // уже с раскрытыми escape
    std::string lexeme;  // оригинальный лексический literal с кавычками

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "StringLit " << lexeme << '\n';
    }
};

export struct CharLit : Expr {
    std::uint32_t value = 0;  // Unicode code point (8-битный ASCII укладывается)
    std::string lexeme;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "CharLit " << lexeme << '\n';
    }
};

// `null` — нулевой указатель (A.2.14). Тип присваивается семантикой из контекста.
export struct NullLit : Expr {
    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "NullLit\n";
    }
};

// `&expr` — взятие адреса. operand обязан быть lvalue (Ident/Field/Index/Deref).
export struct AddressOfExpr : Expr {
    std::unique_ptr<Expr> operand;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "AddressOf\n";
        operand->dump(os, level + 1);
    }
};

// `*expr` — разыменование. Является lvalue (можно `*p = …`).
export struct DerefExpr : Expr {
    std::unique_ptr<Expr> operand;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Deref\n";
        operand->dump(os, level + 1);
    }
};

export struct IdentExpr : Expr {
    std::string name;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Ident '" << name << "'\n";
    }
};

export struct ArrayLit : Expr {
    std::vector<std::unique_ptr<Expr>> elements;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "ArrayLit (" << elements.size() << " elements)\n";
        for (const auto& e : elements) e->dump(os, level + 1);
    }
};

export struct StructLitField {
    std::string name;
    std::unique_ptr<Expr> value;
    SourceLocation loc;
};

export struct StructLit : Expr {
    std::string type_name;
    std::vector<StructLitField> fields;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "StructLit '" << type_name << "'\n";
        for (const auto& f : fields) {
            indent(os, level + 1);
            os << "field '" << f.name << "'\n";
            f.value->dump(os, level + 2);
        }
    }
};

export struct UnaryExpr : Expr {
    UnaryOp op = UnaryOp::Neg;
    std::unique_ptr<Expr> operand;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Unary '" << to_string(op) << "'\n";
        operand->dump(os, level + 1);
    }
};

export struct BinaryExpr : Expr {
    BinaryOp op = BinaryOp::Add;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Binary '" << to_string(op) << "'\n";
        lhs->dump(os, level + 1);
        rhs->dump(os, level + 1);
    }
};

// `obj.field` или доступ к элементу неймспейса/модуля.
// Семантика разбирает: namespace-access / field-access / method-ref.
export struct FieldExpr : Expr {
    std::unique_ptr<Expr> base;
    std::string field;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Field '." << field << "'\n";
        base->dump(os, level + 1);
    }
};

// `arr[i]` — индексирование.
export struct IndexExpr : Expr {
    std::unique_ptr<Expr> base;
    std::unique_ptr<Expr> index;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Index\n";
        base->dump(os, level + 1);
        index->dump(os, level + 1);
    }
};

// `f(a, b)` — вызов функции, метода, либо cast (T(x)).
// Различение делает семантика по типу callee.
export struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Call (" << args.size() << " args)\n";
        callee->dump(os, level + 1);
        for (const auto& a : args) a->dump(os, level + 1);
    }
};

// ===========================================================================
// Stmt — инструкции (§3.3 grammar.md)
// ===========================================================================

export struct Stmt {
    SourceLocation loc;
    virtual ~Stmt() = default;
    virtual void dump(std::ostream& os, int level) const = 0;
};

// `let x: T = expr;` или `var x: T = expr;`. Тип опционален (вывод через `:=`).
export struct VarDeclStmt : Stmt {
    bool is_mutable = false;                // true → var, false → let
    std::string name;
    std::unique_ptr<TypeExpr> type;         // nullptr → вывод типа
    std::unique_ptr<Expr> init;             // всегда есть (по семантике §4)

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << (is_mutable ? "VarDecl" : "LetDecl") << " '" << name << "'";
        if (!type) os << " (inferred)";
        os << '\n';
        if (type) type->dump(os, level + 1);
        init->dump(os, level + 1);
    }
};

// `lvalue = expr;`
export struct AssignStmt : Stmt {
    std::unique_ptr<Expr> target;  // Ident / Field / Index
    std::unique_ptr<Expr> value;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Assign\n";
        target->dump(os, level + 1);
        value->dump(os, level + 1);
    }
};

// `return [expr];`
export struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;  // nullptr для return; в void

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Return" << (value ? "" : " (void)") << '\n';
        if (value) value->dump(os, level + 1);
    }
};

// `if cond { ... } [else block-or-if]`
export struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> then_branch;
    std::unique_ptr<Stmt> else_branch;  // BlockStmt или IfStmt; nullptr — без else

    void dump(std::ostream& os, int level) const override;
};

// `while cond { ... }`
export struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::unique_ptr<BlockStmt> body;

    void dump(std::ostream& os, int level) const override;
};

export struct BreakStmt : Stmt {
    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Break\n";
    }
};

export struct ContinueStmt : Stmt {
    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Continue\n";
    }
};

export struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "ExprStmt\n";
        expr->dump(os, level + 1);
    }
};

export struct EmptyStmt : Stmt {
    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Empty\n";
    }
};

export struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Block (" << stmts.size() << " stmts)\n";
        for (const auto& s : stmts) s->dump(os, level + 1);
    }
};

inline void IfStmt::dump(std::ostream& os, int level) const {
    indent(os, level);
    os << "If\n";
    indent(os, level + 1); os << "cond:\n";
    cond->dump(os, level + 2);
    indent(os, level + 1); os << "then:\n";
    then_branch->dump(os, level + 2);
    if (else_branch) {
        indent(os, level + 1); os << "else:\n";
        else_branch->dump(os, level + 2);
    }
}

inline void WhileStmt::dump(std::ostream& os, int level) const {
    indent(os, level);
    os << "While\n";
    indent(os, level + 1); os << "cond:\n";
    cond->dump(os, level + 2);
    indent(os, level + 1); os << "body:\n";
    body->dump(os, level + 2);
}

// ===========================================================================
// Decl — объявления верхнего уровня (§3.2 grammar.md)
// ===========================================================================

export struct Decl {
    bool is_pub = false;
    SourceLocation loc;
    virtual ~Decl() = default;
    virtual void dump(std::ostream& os, int level) const = 0;
};

export struct Param {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    SourceLocation loc;
};

export struct FnDecl : Decl {
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> return_type;
    std::unique_ptr<BlockStmt> body;  // nullptr для extern fn (A.3.12)
    bool is_extern = false;            // extern fn: только сигнатура, тело отсутствует

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << (is_pub ? "pub " : "") << (is_extern ? "extern " : "")
           << "Fn '" << name << "'\n";
        if (!params.empty()) {
            indent(os, level + 1); os << "params:\n";
            for (const auto& p : params) {
                indent(os, level + 2); os << "'" << p.name << "'\n";
                p.type->dump(os, level + 3);
            }
        }
        indent(os, level + 1); os << "return:\n";
        return_type->dump(os, level + 2);
        if (body) body->dump(os, level + 1);
    }
};

export struct StructField {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    SourceLocation loc;
};

export struct StructDecl : Decl {
    std::string name;
    std::vector<StructField> fields;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << (is_pub ? "pub " : "") << "Struct '" << name << "'\n";
        for (const auto& f : fields) {
            indent(os, level + 1); os << "field '" << f.name << "'\n";
            f.type->dump(os, level + 2);
        }
    }
};

export struct TypeAliasDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> target;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << (is_pub ? "pub " : "") << "TypeAlias '" << name << "'\n";
        target->dump(os, level + 1);
    }
};

export struct NamespaceDecl : Decl {
    std::string name;
    std::vector<std::unique_ptr<Decl>> members;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << (is_pub ? "pub " : "") << "Namespace '" << name << "'\n";
        for (const auto& d : members) d->dump(os, level + 1);
    }
};

// Внутри impl-блока: метод = (флаг pub, FnDecl).
// `is_pub` хранится на самом FnDecl.
export struct ImplDecl : Decl {
    std::string type_name;
    std::vector<std::unique_ptr<FnDecl>> methods;

    void dump(std::ostream& os, int level) const override {
        indent(os, level);
        os << "Impl '" << type_name << "'\n";
        for (const auto& m : methods) m->dump(os, level + 1);
    }
};

// ===========================================================================
// Program — корневой узел AST
// ===========================================================================

export struct Program {
    std::string module_name;
    SourceLocation module_loc;
    std::vector<std::string> imports;
    std::vector<std::unique_ptr<Decl>> decls;
};

export void dump_ast(const Program& p, std::ostream& os) {
    os << "Program\n";
    indent(os, 1); os << "module '" << p.module_name << "'\n";
    if (!p.imports.empty()) {
        indent(os, 1); os << "imports:\n";
        for (const auto& i : p.imports) {
            indent(os, 2); os << "'" << i << "'\n";
        }
    }
    indent(os, 1); os << "decls (" << p.decls.size() << "):\n";
    for (const auto& d : p.decls) d->dump(os, 2);
}

}  // namespace herta::ast
