// IR: промежуточное представление и фаза lowering AST → IR.
//
// Линейный трёхадресный код:
//   t = a OP b      // бинарные операции
//   t = OP a        // унарные
//   t = a           // move
//   t = (T) a       // явное приведение типов
//   t = call f(...) // вызовы (плоское имя; namespace/module/T.method уже разрешены)
//   t = a[b]        // индексирование
//   t = a.field     // доступ к полю
//   a[b] = c        // запись по индексу
//   a.field = b     // запись поля
//   t = [a, b, ...] // литерал массива
//   t = T { a, ... }// литерал структуры (поля в порядке struct-декларации)
//   label L:
//   goto L
//   if a goto L1 else L2
//   return [a]
//
// Lowering предполагает, что семантика отработала без ошибок и заполнила
// SemanticAnalyzer::expression_types() и methods(). Эта фаза диагностики не
// делает; любая unreachable-ситуация — внутренний баг компилятора.

export module herta.ir;

import std;
import herta.common;
import herta.ast;
import herta.semantic;

namespace herta::ir {

using herta::common::SourceLocation;
namespace ast = herta::ast;
namespace sema = herta::semantic;

// Operand: временный регистр, переменная или литерал.

export enum class OperandKind : std::uint8_t {
    Temp,        // %t<id>
    Var,         // именованная переменная или параметр
    IntC, FloatC, BoolC, StringC, CharC,
    NullC,       // нулевой указатель
    Unit,        // отсутствие значения (для вызовов, возвращающих void)
};

export struct Operand {
    OperandKind kind = OperandKind::Unit;
    std::int64_t int_v = 0;       // id для Temp или значение IntC
    double float_v = 0.0;          // значение FloatC
    bool bool_v = false;           // значение BoolC
    std::uint32_t char_v = 0;      // значение CharC (codepoint)
    std::string str_v;             // имя Var или значение StringC (без кавычек)

    static Operand temp(int id) {
        Operand o; o.kind = OperandKind::Temp; o.int_v = id; return o;
    }
    static Operand var(std::string name) {
        Operand o; o.kind = OperandKind::Var; o.str_v = std::move(name); return o;
    }
    static Operand int_c(std::int64_t v) {
        Operand o; o.kind = OperandKind::IntC; o.int_v = v; return o;
    }
    static Operand float_c(double v) {
        Operand o; o.kind = OperandKind::FloatC; o.float_v = v; return o;
    }
    static Operand bool_c(bool v) {
        Operand o; o.kind = OperandKind::BoolC; o.bool_v = v; return o;
    }
    static Operand string_c(std::string v) {
        Operand o; o.kind = OperandKind::StringC; o.str_v = std::move(v); return o;
    }
    static Operand char_c(std::uint32_t v) {
        Operand o; o.kind = OperandKind::CharC; o.char_v = v; return o;
    }
    static Operand null_c() {
        Operand o; o.kind = OperandKind::NullC; return o;
    }
    static Operand unit() { return Operand{}; }
};

export enum class BinOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    // Concat живёт отдельно от Add — бэкенд по нему сразу понимает, что это строки.
    Concat,
};

export enum class UnOp : std::uint8_t { Neg, Not };

export enum class InstrKind : std::uint8_t {
    Move,         // dst = a
    Bin,          // dst = a bin_op b
    Un,           // dst = un_op a
    Cast,         // dst = (type_name) a
    Call,         // [dst =] call callee(args...)
    LoadIndex,    // dst = a[b]
    StoreIndex,   // a[b] = value_to_store
    LoadField,    // dst = a.field
    StoreField,   // a.field = value_to_store
    MakeArray,    // dst = [args...]    (type_name = element type)
    MakeStruct,   // dst = type_name { struct_field_names[i]: args[i] }
    Label,        // label:
    Goto,         // goto label
    Branch,       // if a goto label_then else label_else
    Return,       // return [a]
    // Работа с указателями
    AddressOf,    // dst = &a            (a — Var/Temp, dst имеет тип *T)
    LoadPtr,      // dst = *a            (a — указатель)
    StorePtr,     // *a = value_to_store
};

export struct Instr {
    InstrKind kind;
    Operand dst;          // если has_dst
    bool has_dst = false;

    Operand a, b;         // основные операнды
    Operand value_to_store; // для StoreIndex / StoreField

    BinOp bin_op = BinOp::Add;
    UnOp un_op = UnOp::Neg;

    std::string type_name;       // Cast / MakeArray (element) / MakeStruct
    std::string callee;          // Call
    std::vector<Operand> args;   // Call / MakeArray / MakeStruct values
    std::vector<std::string> struct_field_names; // MakeStruct field names
    std::string field;           // LoadField / StoreField

    std::string label;           // Label / Goto
    std::string label_then;      // Branch
    std::string label_else;      // Branch

    bool has_ret_value = false;  // Return

    SourceLocation loc;
};

export struct IrParam {
    std::string name;
    std::string type_str;
};

export struct Function {
    std::string name;            // плоское имя: main, Math.add, Point.get
    std::vector<IrParam> params;
    std::string return_type;
    std::vector<Instr> instrs;
    SourceLocation loc;
};

// Описание struct-типа: имя плюс упорядоченный список полей (имя, тип).
// Нужно бэкендам, которым важна позиционная раскладка полей (LLVM).
export struct StructDef {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;  // имя → текст-тип
};

export struct Module {
    std::string name;
    std::vector<Function> functions;
    std::vector<StructDef> structs;
    // Псевдонимы типов (type Name = Target;). Кодген должен раскрывать их до
    // примитива, массива или структуры. Хранятся плоско, для v1 этого хватает.
    std::vector<std::pair<std::string, std::string>> type_aliases;
};

namespace {

std::string escape_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string operand_str(const Operand& o) {
    switch (o.kind) {
        case OperandKind::Temp:   return "%t" + std::to_string(o.int_v);
        case OperandKind::Var:    return o.str_v;
        case OperandKind::IntC:   return std::to_string(o.int_v);
        case OperandKind::FloatC: return std::format("{}", o.float_v);
        case OperandKind::BoolC:  return o.bool_v ? "true" : "false";
        case OperandKind::StringC: return escape_string(o.str_v);
        case OperandKind::CharC:  return std::format("'\\u{{{:x}}}'", o.char_v);
        case OperandKind::NullC:  return "null";
        case OperandKind::Unit:   return "()";
    }
    return "?";
}

std::string bin_op_str(BinOp op) {
    switch (op) {
        case BinOp::Add:    return "add";
        case BinOp::Sub:    return "sub";
        case BinOp::Mul:    return "mul";
        case BinOp::Div:    return "div";
        case BinOp::Mod:    return "mod";
        case BinOp::Eq:     return "eq";
        case BinOp::NotEq:  return "ne";
        case BinOp::Lt:     return "lt";
        case BinOp::Gt:     return "gt";
        case BinOp::LtEq:   return "le";
        case BinOp::GtEq:   return "ge";
        case BinOp::Concat: return "concat";
    }
    return "?";
}

std::string un_op_str(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "neg";
        case UnOp::Not: return "not";
    }
    return "?";
}

void dump_instr(std::ostream& os, const Instr& i) {
    using K = InstrKind;
    auto dst = [&]() { return operand_str(i.dst); };
    switch (i.kind) {
        case K::Label:
            os << i.label << ":\n";
            return;
        case K::Goto:
            os << "    goto " << i.label << "\n";
            return;
        case K::Branch:
            os << "    if " << operand_str(i.a)
               << " goto " << i.label_then
               << " else " << i.label_else << "\n";
            return;
        case K::Return:
            os << "    return";
            if (i.has_ret_value) os << ' ' << operand_str(i.a);
            os << "\n";
            return;
        case K::Move:
            os << "    " << dst() << " = " << operand_str(i.a) << "\n";
            return;
        case K::Bin:
            os << "    " << dst() << " = " << bin_op_str(i.bin_op)
               << ' ' << operand_str(i.a) << ", " << operand_str(i.b) << "\n";
            return;
        case K::Un:
            os << "    " << dst() << " = " << un_op_str(i.un_op)
               << ' ' << operand_str(i.a) << "\n";
            return;
        case K::Cast:
            os << "    " << dst() << " = cast " << i.type_name
               << ' ' << operand_str(i.a) << "\n";
            return;
        case K::Call: {
            os << "    ";
            if (i.has_dst) os << dst() << " = ";
            os << "call " << i.callee << "(";
            for (std::size_t k = 0; k < i.args.size(); ++k) {
                if (k) os << ", ";
                os << operand_str(i.args[k]);
            }
            os << ")\n";
            return;
        }
        case K::LoadIndex:
            os << "    " << dst() << " = index " << operand_str(i.a)
               << ", " << operand_str(i.b) << "\n";
            return;
        case K::StoreIndex:
            os << "    " << operand_str(i.a) << "[" << operand_str(i.b)
               << "] = " << operand_str(i.value_to_store) << "\n";
            return;
        case K::LoadField:
            os << "    " << dst() << " = field " << operand_str(i.a)
               << ", \"" << i.field << "\"\n";
            return;
        case K::StoreField:
            os << "    " << operand_str(i.a) << "." << i.field
               << " = " << operand_str(i.value_to_store) << "\n";
            return;
        case K::MakeArray: {
            os << "    " << dst() << " = array " << i.type_name << " [";
            for (std::size_t k = 0; k < i.args.size(); ++k) {
                if (k) os << ", ";
                os << operand_str(i.args[k]);
            }
            os << "]\n";
            return;
        }
        case K::MakeStruct: {
            os << "    " << dst() << " = struct " << i.type_name << " {";
            for (std::size_t k = 0; k < i.args.size(); ++k) {
                if (k) os << ", ";
                os << i.struct_field_names[k] << ": " << operand_str(i.args[k]);
            }
            os << "}\n";
            return;
        }
        case K::AddressOf:
            os << "    " << dst() << " = &" << operand_str(i.a) << "\n";
            return;
        case K::LoadPtr:
            os << "    " << dst() << " = *" << operand_str(i.a) << "\n";
            return;
        case K::StorePtr:
            os << "    *" << operand_str(i.a) << " = "
               << operand_str(i.value_to_store) << "\n";
            return;
    }
}

}  // anonymous namespace

export void dump_module(const Module& m, std::ostream& os) {
    os << "module " << m.name << "\n\n";
    for (const auto& fn : m.functions) {
        os << "fn " << fn.name << "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) os << ", ";
            os << fn.params[i].name << ": " << fn.params[i].type_str;
        }
        os << ") -> " << fn.return_type << " {\n";
        for (const auto& ins : fn.instrs) dump_instr(os, ins);
        os << "}\n\n";
    }
}

// Lowerer: AST → IR

export class Lowerer {
public:
    Lowerer(const ast::Program& prog, const sema::SemanticAnalyzer& sema_arg)
        : prog_(prog), sema_(sema_arg),
          expr_types_(sema_arg.expression_types()),
          methods_(sema_arg.methods()),
          imports_(sema_arg.imports()),
          module_scope_(sema_arg.module_scope()) {}

    Module lower();

    // Доступ к ещё не возвращённому списку структур (для тестов и отладки).
    const std::vector<StructDef>& structs() const noexcept { return struct_defs_; }

private:
    void lower_decls(const std::vector<std::unique_ptr<ast::Decl>>& decls,
                     const std::string& prefix);
    void lower_fn(const ast::FnDecl& fn, const std::string& flat_name);
    void lower_impl(const ast::ImplDecl& im);

    void lower_block(const ast::BlockStmt& b);
    void lower_stmt(const ast::Stmt& s);
    void lower_var_decl(const ast::VarDeclStmt& v);
    void lower_assign(const ast::AssignStmt& a);
    void lower_return(const ast::ReturnStmt& r);
    void lower_if(const ast::IfStmt& s);
    void lower_while(const ast::WhileStmt& s);

    Operand lower_expr(const ast::Expr& e);
    Operand lower_unary(const ast::UnaryExpr& u);
    Operand lower_binary(const ast::BinaryExpr& b);
    Operand lower_field(const ast::FieldExpr& f);
    Operand lower_index(const ast::IndexExpr& i);
    Operand lower_call(const ast::CallExpr& c);
    Operand lower_array_lit(const ast::ArrayLit& a);
    Operand lower_struct_lit(const ast::StructLit& s);

    Operand fresh_temp();
    std::string fresh_label();
    void emit(Instr i);

    std::string type_to_str(const sema::Type& t) const { return t.to_string(); }
    std::string ast_type_to_str(const ast::TypeExpr& te) const;
    sema::Type type_of(const ast::Expr& e) const;

    struct CalleeInfo {
        enum class Kind { Function, Cast, BuiltinPrint, BuiltinInput,
                          BuiltinExit, BuiltinPanic, BuiltinAssert,
                          BuiltinLen, StaticMethod, InstanceMethod };
        Kind kind;
        std::string flat_name;       // плоское имя, которое уйдёт в Call
        std::string cast_type;       // для Cast
        const ast::Expr* self_expr = nullptr;  // для метода: становится первым аргументом
    };
    CalleeInfo resolve_callee(const ast::Expr& callee);

    const ast::Program& prog_;
    [[maybe_unused]] const sema::SemanticAnalyzer& sema_;
    const std::unordered_map<const ast::Expr*, sema::Type>& expr_types_;
    const std::unordered_map<sema::StructTy*, std::vector<sema::MethodInfo>>& methods_;
    const std::unordered_map<std::string, std::shared_ptr<sema::Scope>>& imports_;
    std::shared_ptr<sema::Scope> module_scope_;

    std::vector<Function> functions_;
    std::vector<StructDef> struct_defs_;
    std::vector<std::pair<std::string, std::string>> type_aliases_;
    Function* current_fn_ = nullptr;
    int next_temp_ = 0;
    int next_label_ = 0;

    struct LoopCtx { std::string break_label; std::string continue_label; };
    std::vector<LoopCtx> loops_;
};

Module Lowerer::lower() {
    Module m;
    m.name = prog_.module_name;
    lower_decls(prog_.decls, /*prefix=*/"");
    m.functions = std::move(functions_);
    m.structs = std::move(struct_defs_);
    m.type_aliases = std::move(type_aliases_);
    return m;
}

void Lowerer::lower_decls(
        const std::vector<std::unique_ptr<ast::Decl>>& decls,
        const std::string& prefix) {
    for (const auto& d : decls) {
        if (auto* fn = dynamic_cast<const ast::FnDecl*>(d.get())) {
            std::string flat = prefix.empty() ? fn->name : (prefix + "." + fn->name);
            lower_fn(*fn, flat);
        } else if (auto* ns = dynamic_cast<const ast::NamespaceDecl*>(d.get())) {
            std::string sub_prefix = prefix.empty() ? ns->name : (prefix + "." + ns->name);
            lower_decls(ns->members, sub_prefix);
        } else if (auto* im = dynamic_cast<const ast::ImplDecl*>(d.get())) {
            // Методы impl уходят на верхний уровень; impl внутри namespace в v1 запрещён.
            lower_impl(*im);
        } else if (auto* sd = dynamic_cast<const ast::StructDecl*>(d.get())) {
            // Сохраняем позиционную раскладку полей: LLVM-бэкенду нужны их индексы.
            StructDef def;
            def.name = sd->name;
            def.fields.reserve(sd->fields.size());
            for (const auto& f : sd->fields) {
                def.fields.emplace_back(f.name, ast_type_to_str(*f.type));
            }
            struct_defs_.push_back(std::move(def));
        } else if (auto* ta = dynamic_cast<const ast::TypeAliasDecl*>(d.get())) {
            type_aliases_.emplace_back(ta->name, ast_type_to_str(*ta->target));
        }
        // module и import никаких инструкций в IR не дают
    }
}

void Lowerer::lower_fn(const ast::FnDecl& fn, const std::string& flat_name) {
    Function f;
    f.name = flat_name;
    f.loc = fn.loc;
    f.return_type = ast_type_to_str(*fn.return_type);
    for (const auto& p : fn.params) {
        f.params.push_back(IrParam{p.name, ast_type_to_str(*p.type)});
    }
    functions_.push_back(std::move(f));
    current_fn_ = &functions_.back();
    next_temp_ = 0;
    next_label_ = 0;
    lower_block(*fn.body);
    current_fn_ = nullptr;
}

void Lowerer::lower_impl(const ast::ImplDecl& im) {
    for (const auto& m : im.methods) {
        std::string flat = im.type_name + "." + m->name;
        lower_fn(*m, flat);
    }
}

void Lowerer::lower_block(const ast::BlockStmt& b) {
    for (const auto& s : b.stmts) lower_stmt(*s);
}

void Lowerer::lower_stmt(const ast::Stmt& s) {
    if (auto* v = dynamic_cast<const ast::VarDeclStmt*>(&s)) { lower_var_decl(*v); return; }
    if (auto* a = dynamic_cast<const ast::AssignStmt*>(&s))  { lower_assign(*a); return; }
    if (auto* r = dynamic_cast<const ast::ReturnStmt*>(&s))  { lower_return(*r); return; }
    if (auto* i = dynamic_cast<const ast::IfStmt*>(&s))      { lower_if(*i); return; }
    if (auto* w = dynamic_cast<const ast::WhileStmt*>(&s))   { lower_while(*w); return; }
    if (dynamic_cast<const ast::BreakStmt*>(&s)) {
        // Должна быть проверка в семантике; здесь — assume OK.
        Instr i; i.kind = InstrKind::Goto; i.label = loops_.back().break_label;
        i.loc = s.loc;
        emit(std::move(i));
        return;
    }
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        Instr i; i.kind = InstrKind::Goto; i.label = loops_.back().continue_label;
        i.loc = s.loc;
        emit(std::move(i));
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s)) {
        lower_expr(*es->expr);  // результат отбрасывается
        return;
    }
    if (dynamic_cast<const ast::EmptyStmt*>(&s)) return;
    if (auto* b = dynamic_cast<const ast::BlockStmt*>(&s)) { lower_block(*b); return; }
    // Сюда нормально попадать не должны — неподдерживаемый узел.
}

void Lowerer::lower_var_decl(const ast::VarDeclStmt& v) {
    auto rhs = lower_expr(*v.init);
    // Если в объявлении указан конкретный тип, вставляем Cast в этот тип.
    // Без этого бэкенд выводит тип переменной из rhs (IntC → int64), и
    // var x: int8 = 100; даст alloca i64 вместо i8 — нарушая wraparound
    // по разрядности из types.md §6.4. Cast также покрывает случай
    // var p: *T = null; (без него *p в бэкенде был бы *void).
    if (v.type) {
        auto cast_temp = fresh_temp();
        Instr c; c.kind = InstrKind::Cast; c.dst = cast_temp; c.has_dst = true;
        c.a = rhs; c.type_name = ast_type_to_str(*v.type); c.loc = v.loc;
        emit(std::move(c));
        rhs = cast_temp;
    }
    Instr i;
    i.kind = InstrKind::Move;
    i.dst = Operand::var(v.name);
    i.has_dst = true;
    i.a = rhs;
    i.loc = v.loc;
    emit(std::move(i));
}

void Lowerer::lower_assign(const ast::AssignStmt& a) {
    auto rhs = lower_expr(*a.value);
    // Возможные формы target: Ident, Field, Index, Deref — или глубже вложенные.
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(a.target.get())) {
        Instr i; i.kind = InstrKind::Move; i.dst = Operand::var(id->name);
        i.has_dst = true; i.a = rhs; i.loc = a.loc;
        emit(std::move(i));
        return;
    }
    if (auto* fe = dynamic_cast<const ast::FieldExpr*>(a.target.get())) {
        // x.field = rhs работает, если x — Ident; для более сложного — через временную
        auto base = lower_expr(*fe->base);
        Instr i; i.kind = InstrKind::StoreField;
        i.a = base; i.field = fe->field; i.value_to_store = rhs; i.loc = a.loc;
        emit(std::move(i));
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(a.target.get())) {
        auto base = lower_expr(*ix->base);
        auto idx = lower_expr(*ix->index);
        Instr i; i.kind = InstrKind::StoreIndex;
        i.a = base; i.b = idx; i.value_to_store = rhs; i.loc = a.loc;
        emit(std::move(i));
        return;
    }
    // Запись через указатель: *p = rhs
    if (auto* de = dynamic_cast<const ast::DerefExpr*>(a.target.get())) {
        auto p = lower_expr(*de->operand);
        Instr i; i.kind = InstrKind::StorePtr;
        i.a = p; i.value_to_store = rhs; i.loc = a.loc;
        emit(std::move(i));
        return;
    }
    // Сюда не доберёмся — семантика уже проверила, что слева lvalue.
}

void Lowerer::lower_return(const ast::ReturnStmt& r) {
    Instr i; i.kind = InstrKind::Return; i.loc = r.loc;
    if (r.value) {
        i.a = lower_expr(*r.value);
        i.has_ret_value = true;
    }
    emit(std::move(i));
}

void Lowerer::lower_if(const ast::IfStmt& s) {
    auto cond = lower_expr(*s.cond);
    auto then_l = fresh_label();
    auto else_l = fresh_label();
    auto end_l = s.else_branch ? fresh_label() : else_l;

    Instr br; br.kind = InstrKind::Branch; br.a = cond;
    br.label_then = then_l;
    br.label_else = s.else_branch ? else_l : end_l;
    br.loc = s.loc;
    emit(std::move(br));

    // then
    Instr ll1; ll1.kind = InstrKind::Label; ll1.label = then_l;
    emit(std::move(ll1));
    lower_block(*s.then_branch);
    if (s.else_branch) {
        Instr gt; gt.kind = InstrKind::Goto; gt.label = end_l;
        emit(std::move(gt));
        Instr ll2; ll2.kind = InstrKind::Label; ll2.label = else_l;
        emit(std::move(ll2));
        lower_stmt(*s.else_branch);
    }
    Instr endl; endl.kind = InstrKind::Label; endl.label = end_l;
    emit(std::move(endl));
}

void Lowerer::lower_while(const ast::WhileStmt& s) {
    auto cond_l = fresh_label();
    auto body_l = fresh_label();
    auto end_l = fresh_label();

    Instr ll1; ll1.kind = InstrKind::Label; ll1.label = cond_l;
    emit(std::move(ll1));
    auto cond = lower_expr(*s.cond);
    Instr br; br.kind = InstrKind::Branch; br.a = cond;
    br.label_then = body_l; br.label_else = end_l; br.loc = s.loc;
    emit(std::move(br));

    Instr ll2; ll2.kind = InstrKind::Label; ll2.label = body_l;
    emit(std::move(ll2));
    loops_.push_back(LoopCtx{end_l, cond_l});
    lower_block(*s.body);
    loops_.pop_back();
    Instr gt; gt.kind = InstrKind::Goto; gt.label = cond_l;
    emit(std::move(gt));

    Instr endl; endl.kind = InstrKind::Label; endl.label = end_l;
    emit(std::move(endl));
}

Operand Lowerer::lower_expr(const ast::Expr& e) {
    if (auto* il = dynamic_cast<const ast::IntLit*>(&e))    return Operand::int_c(il->value);
    if (auto* fl = dynamic_cast<const ast::FloatLit*>(&e))  return Operand::float_c(fl->value);
    if (auto* bl = dynamic_cast<const ast::BoolLit*>(&e))   return Operand::bool_c(bl->value);
    if (auto* sl = dynamic_cast<const ast::StringLit*>(&e)) return Operand::string_c(sl->value);
    if (auto* cl = dynamic_cast<const ast::CharLit*>(&e))   return Operand::char_c(cl->value);
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(&e)) return Operand::var(id->name);
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))  return lower_unary(*u);
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) return lower_binary(*b);
    if (auto* f = dynamic_cast<const ast::FieldExpr*>(&e))  return lower_field(*f);
    if (auto* i = dynamic_cast<const ast::IndexExpr*>(&e))  return lower_index(*i);
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e))   return lower_call(*c);
    if (auto* a = dynamic_cast<const ast::ArrayLit*>(&e))   return lower_array_lit(*a);
    if (auto* s = dynamic_cast<const ast::StructLit*>(&e))  return lower_struct_lit(*s);
    // Указатели
    if (dynamic_cast<const ast::NullLit*>(&e)) return Operand::null_c();
    if (auto* ao = dynamic_cast<const ast::AddressOfExpr*>(&e)) {
        // Семантика гарантировала, что operand — это Ident/Field/Index lvalue.
        // Пока поддерживаем только &Ident (адрес локальной переменной); Field
        // и Index лоуэрятся через временную allocaтуру на стороне LLVM.
        auto src = lower_expr(*ao->operand);
        auto dst = fresh_temp();
        Instr i; i.kind = InstrKind::AddressOf; i.dst = dst; i.has_dst = true;
        i.a = src; i.loc = ao->loc;
        emit(std::move(i));
        return dst;
    }
    if (auto* de = dynamic_cast<const ast::DerefExpr*>(&e)) {
        auto p = lower_expr(*de->operand);
        auto dst = fresh_temp();
        Instr i; i.kind = InstrKind::LoadPtr; i.dst = dst; i.has_dst = true;
        i.a = p; i.loc = de->loc;
        emit(std::move(i));
        return dst;
    }
    // unreachable
    return Operand::unit();
}

Operand Lowerer::lower_unary(const ast::UnaryExpr& u) {
    auto a = lower_expr(*u.operand);
    auto dst = fresh_temp();
    Instr i; i.kind = InstrKind::Un; i.dst = dst; i.has_dst = true;
    i.a = a;
    i.un_op = (u.op == ast::UnaryOp::Neg) ? UnOp::Neg : UnOp::Not;
    i.loc = u.loc;
    emit(std::move(i));
    return dst;
}

Operand Lowerer::lower_binary(const ast::BinaryExpr& b) {
    // Short-circuit для && / ||
    if (b.op == ast::BinaryOp::And || b.op == ast::BinaryOp::Or) {
        auto result = fresh_temp();
        auto lhs = lower_expr(*b.lhs);
        // Сохранить lhs в result
        Instr mv; mv.kind = InstrKind::Move; mv.dst = result; mv.has_dst = true;
        mv.a = lhs; mv.loc = b.loc;
        emit(std::move(mv));

        auto rhs_l = fresh_label();
        auto end_l = fresh_label();
        Instr br; br.kind = InstrKind::Branch; br.a = result; br.loc = b.loc;
        if (b.op == ast::BinaryOp::And) {
            // если lhs == true → eval rhs; иначе сохранить false
            br.label_then = rhs_l;
            br.label_else = end_l;
        } else {
            // Or: lhs == true → пропустить rhs; иначе eval
            br.label_then = end_l;
            br.label_else = rhs_l;
        }
        emit(std::move(br));

        Instr ll; ll.kind = InstrKind::Label; ll.label = rhs_l;
        emit(std::move(ll));
        auto rhs = lower_expr(*b.rhs);
        Instr mv2; mv2.kind = InstrKind::Move; mv2.dst = result; mv2.has_dst = true;
        mv2.a = rhs; mv2.loc = b.loc;
        emit(std::move(mv2));

        Instr endl; endl.kind = InstrKind::Label; endl.label = end_l;
        emit(std::move(endl));
        return result;
    }

    auto lhs = lower_expr(*b.lhs);
    auto rhs = lower_expr(*b.rhs);
    auto dst = fresh_temp();

    // Спец: string + string → Concat
    auto lhs_ty_it = expr_types_.find(b.lhs.get());
    bool is_string_op = (lhs_ty_it != expr_types_.end()
                         && lhs_ty_it->second.is_string());

    Instr i; i.kind = InstrKind::Bin; i.dst = dst; i.has_dst = true;
    i.a = lhs; i.b = rhs; i.loc = b.loc;
    if (b.op == ast::BinaryOp::Add && is_string_op) {
        i.bin_op = BinOp::Concat;
    } else {
        switch (b.op) {
            case ast::BinaryOp::Add:   i.bin_op = BinOp::Add; break;
            case ast::BinaryOp::Sub:   i.bin_op = BinOp::Sub; break;
            case ast::BinaryOp::Mul:   i.bin_op = BinOp::Mul; break;
            case ast::BinaryOp::Div:   i.bin_op = BinOp::Div; break;
            case ast::BinaryOp::Mod:   i.bin_op = BinOp::Mod; break;
            case ast::BinaryOp::Eq:    i.bin_op = BinOp::Eq; break;
            case ast::BinaryOp::NotEq: i.bin_op = BinOp::NotEq; break;
            case ast::BinaryOp::Lt:    i.bin_op = BinOp::Lt; break;
            case ast::BinaryOp::Gt:    i.bin_op = BinOp::Gt; break;
            case ast::BinaryOp::LtEq:  i.bin_op = BinOp::LtEq; break;
            case ast::BinaryOp::GtEq:  i.bin_op = BinOp::GtEq; break;
            default: break;  // And/Or обработаны выше
        }
    }
    emit(std::move(i));
    return dst;
}

Operand Lowerer::lower_field(const ast::FieldExpr& f) {
    // Возможен: Module.x | Namespace.x | struct_value.field.
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(f.base.get())) {
        auto sym = module_scope_->lookup(id->name);
        if (sym && (sym->kind == sema::Symbol::Kind::Namespace
                 || sym->kind == sema::Symbol::Kind::Module)) {
            // Namespace.x / Module.x как rvalue.
            // Если это глобальная переменная — её просто читать как Var с
            // плоским именем. Если это функция — это «функциональное значение»,
            // которое мы как value не поддерживаем (semantics запретит).
            auto m = sym->ns_scope->lookup_local(f.field);
            if (m && m->kind == sema::Symbol::Kind::Var) {
                return Operand::var(id->name + "." + f.field);
            }
            // Для не-Var: возвращаем «псевдо-имя»; реально это используется
            // только в составе CallExpr (lower_call перехватит до сюда).
            return Operand::var(id->name + "." + f.field);
        }
    }
    // Доступ к полю структуры по значению.
    auto base = lower_expr(*f.base);
    auto dst = fresh_temp();
    Instr i; i.kind = InstrKind::LoadField; i.dst = dst; i.has_dst = true;
    i.a = base; i.field = f.field; i.loc = f.loc;
    emit(std::move(i));
    return dst;
}

Operand Lowerer::lower_index(const ast::IndexExpr& e) {
    auto base = lower_expr(*e.base);
    auto idx = lower_expr(*e.index);
    auto dst = fresh_temp();
    Instr i; i.kind = InstrKind::LoadIndex; i.dst = dst; i.has_dst = true;
    i.a = base; i.b = idx; i.loc = e.loc;
    emit(std::move(i));
    return dst;
}

Operand Lowerer::lower_call(const ast::CallExpr& c) {
    auto info = resolve_callee(*c.callee);
    using K = CalleeInfo::Kind;

    // Cast: один аргумент, тип в info.cast_type.
    if (info.kind == K::Cast) {
        auto src = lower_expr(*c.args[0]);
        auto dst = fresh_temp();
        Instr i; i.kind = InstrKind::Cast; i.dst = dst; i.has_dst = true;
        i.a = src; i.type_name = info.cast_type; i.loc = c.loc;
        emit(std::move(i));
        return dst;
    }

    // Сначала лоуэрим аргументы (порядок слева направо).
    std::vector<Operand> args;
    if (info.kind == K::InstanceMethod && info.self_expr) {
        args.push_back(lower_expr(*info.self_expr));
    }
    for (const auto& arg : c.args) {
        args.push_back(lower_expr(*arg));
    }

    // Эмитим Call. Builtin'ы имеют флаги для бэкендов через имя.
    std::string callee_name = info.flat_name;
    bool void_call;
    // print/exit/panic/assert возвращают void; input/len — нет.
    if (info.kind == K::BuiltinPrint || info.kind == K::BuiltinExit
        || info.kind == K::BuiltinPanic || info.kind == K::BuiltinAssert) {
        void_call = true;
    } else if (info.kind == K::Function || info.kind == K::StaticMethod
            || info.kind == K::InstanceMethod) {
        // Определяем по типу выражения вызова (если он void).
        auto it = expr_types_.find(&c);
        void_call = (it != expr_types_.end() && it->second.is_void());
    } else {
        void_call = false;  // builtin input / len
    }

    Instr i; i.kind = InstrKind::Call; i.callee = callee_name; i.args = std::move(args);
    i.loc = c.loc;
    Operand result;
    if (void_call) {
        i.has_dst = false;
        result = Operand::unit();
    } else {
        auto dst = fresh_temp();
        i.dst = dst; i.has_dst = true;
        result = dst;
    }
    emit(std::move(i));
    return result;
}

Lowerer::CalleeInfo Lowerer::resolve_callee(const ast::Expr& callee) {
    using K = CalleeInfo::Kind;
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(&callee)) {
        // Сначала проверяем builtin'ы по имени.
        if (id->name == "print")  return CalleeInfo{K::BuiltinPrint,  "print",  {}, nullptr};
        if (id->name == "input")  return CalleeInfo{K::BuiltinInput,  "input",  {}, nullptr};
        if (id->name == "exit")   return CalleeInfo{K::BuiltinExit,   "exit",   {}, nullptr};
        if (id->name == "panic")  return CalleeInfo{K::BuiltinPanic,  "panic",  {}, nullptr};
        if (id->name == "assert") return CalleeInfo{K::BuiltinAssert, "assert", {}, nullptr};
        if (id->name == "len")    return CalleeInfo{K::BuiltinLen,    "len",    {}, nullptr};
        // Иначе — либо функция, либо тип-каст.
        auto sym = module_scope_->lookup(id->name);
        if (sym && sym->kind == sema::Symbol::Kind::TypeName) {
            return CalleeInfo{K::Cast, {}, id->name, nullptr};
        }
        return CalleeInfo{K::Function, id->name, {}, nullptr};
    }
    if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&callee)) {
        if (auto* base_id = dynamic_cast<const ast::IdentExpr*>(fe->base.get())) {
            auto base_sym = module_scope_->lookup(base_id->name);
            if (base_sym) {
                if (base_sym->kind == sema::Symbol::Kind::Namespace
                 || base_sym->kind == sema::Symbol::Kind::Module) {
                    return CalleeInfo{K::Function,
                                      base_id->name + "." + fe->field, {}, nullptr};
                }
                if (base_sym->kind == sema::Symbol::Kind::TypeName
                 && base_sym->type.is_struct()) {
                    // static method T.m(...)
                    auto* st_ptr = base_sym->type.struct_ptr().get();
                    auto it = methods_.find(st_ptr);
                    if (it != methods_.end()) {
                        for (const auto& m : it->second) {
                            if (m.name == fe->field && m.is_static) {
                                return CalleeInfo{K::StaticMethod,
                                                  base_id->name + "." + fe->field, {}, nullptr};
                            }
                        }
                    }
                }
            }
        }
        // Иначе — instance method: obj.method(args).
        auto base_ty_it = expr_types_.find(fe->base.get());
        if (base_ty_it != expr_types_.end() && base_ty_it->second.is_struct()) {
            auto* st_ptr = base_ty_it->second.struct_ptr().get();
            auto it = methods_.find(st_ptr);
            if (it != methods_.end()) {
                for (const auto& m : it->second) {
                    if (m.name == fe->field && !m.is_static) {
                        return CalleeInfo{K::InstanceMethod,
                                          base_ty_it->second.strukt().name + "." + fe->field,
                                          {}, fe->base.get()};
                    }
                }
            }
        }
    }
    return CalleeInfo{K::Function, "?", {}, nullptr};
}

Operand Lowerer::lower_array_lit(const ast::ArrayLit& al) {
    std::vector<Operand> args;
    args.reserve(al.elements.size());
    for (const auto& e : al.elements) args.push_back(lower_expr(*e));

    std::string elem_ty = "?";
    auto it = expr_types_.find(&al);
    if (it != expr_types_.end() && it->second.is_array()) {
        elem_ty = it->second.array().element.to_string();
    }

    auto dst = fresh_temp();
    Instr i; i.kind = InstrKind::MakeArray; i.dst = dst; i.has_dst = true;
    i.args = std::move(args); i.type_name = elem_ty; i.loc = al.loc;
    emit(std::move(i));
    return dst;
}

Operand Lowerer::lower_struct_lit(const ast::StructLit& sl) {
    std::vector<Operand> args;
    std::vector<std::string> names;
    args.reserve(sl.fields.size());
    names.reserve(sl.fields.size());
    for (const auto& f : sl.fields) {
        args.push_back(lower_expr(*f.value));
        names.push_back(f.name);
    }
    auto dst = fresh_temp();
    Instr i; i.kind = InstrKind::MakeStruct; i.dst = dst; i.has_dst = true;
    i.args = std::move(args); i.struct_field_names = std::move(names);
    i.type_name = sl.type_name; i.loc = sl.loc;
    emit(std::move(i));
    return dst;
}

Operand Lowerer::fresh_temp() {
    return Operand::temp(next_temp_++);
}

std::string Lowerer::fresh_label() {
    return "L" + std::to_string(next_label_++);
}

void Lowerer::emit(Instr i) {
    current_fn_->instrs.push_back(std::move(i));
}

std::string Lowerer::ast_type_to_str(const ast::TypeExpr& te) const {
    if (auto* nt = dynamic_cast<const ast::NamedType*>(&te)) return nt->name;
    if (auto* at = dynamic_cast<const ast::ArrayType*>(&te)) {
        return "[" + ast_type_to_str(*at->element) + "; " + std::to_string(at->size) + "]";
    }
    if (auto* pt = dynamic_cast<const ast::PointerType*>(&te)) {
        return "*" + ast_type_to_str(*pt->pointee);
    }
    return "?";
}

sema::Type Lowerer::type_of(const ast::Expr& e) const {
    auto it = expr_types_.find(&e);
    if (it != expr_types_.end()) return it->second;
    return sema::Type(sema::Primitive::Void);  // shouldn't happen
}


}  // namespace herta::ir
