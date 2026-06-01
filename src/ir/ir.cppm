// Module `herta.ir` — промежуточное представление и фаза lowering AST → IR.
//
// Этап 5 из impl_plan.md (B.2.1). Линейный трёхадресный код:
//   t = a OP b      ; бинарные операции
//   t = OP a        ; унарные
//   t = a           ; move
//   t = (T) a       ; явное приведение типов
//   t = call f(...) ; вызовы (плоское имя; namespace/module/T.method разрешены здесь)
//   t = a[b]        ; индексирование
//   t = a.field     ; доступ к полю
//   a[b] = c        ; запись по индексу
//   a.field = b     ; запись поля
//   t = [a, b, ...] ; литерал массива
//   t = T { a, ... }; литерал структуры (поля в порядке struct-декларации)
//   label L:
//   goto L
//   if a goto L1 else L2
//   return [a]
//
// Lowering предполагает, что семантика прошла без ошибок и заполнила
// SemanticAnalyzer::expression_types() и methods(). Эта фаза не диагностирует;
// все unreachable-ситуации трактуются как internal-bugs.

export module herta.ir;

import std;
import herta.common;
import herta.ast;
import herta.semantic;

namespace herta::ir {

using herta::common::SourceLocation;
namespace ast = herta::ast;
namespace sema = herta::semantic;

// ===========================================================================
// Operand: temp register / variable / literal constant.
// ===========================================================================

export enum class OperandKind : std::uint8_t {
    Temp,        // %t<id>
    Var,         // именованная переменная / параметр
    IntC, FloatC, BoolC, StringC, CharC,
    Unit,        // отсутствие значения (для void-call)
};

export struct Operand {
    OperandKind kind = OperandKind::Unit;
    std::int64_t int_v = 0;       // Temp id (кастуется); IntC value
    double float_v = 0.0;          // FloatC value
    bool bool_v = false;           // BoolC value
    std::uint32_t char_v = 0;      // CharC value (codepoint)
    std::string str_v;             // Var name; StringC value (без кавычек)

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
    static Operand unit() { return Operand{}; }
};

// ===========================================================================
// Operations
// ===========================================================================

export enum class BinOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    // Concat — отдельно от Add: бэкенд знает, что это операция со строками.
    Concat,
};

export enum class UnOp : std::uint8_t { Neg, Not };

// ===========================================================================
// Instruction
// ===========================================================================

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

// ===========================================================================
// Function / Module
// ===========================================================================

export struct IrParam {
    std::string name;
    std::string type_str;
};

export struct Function {
    std::string name;            // плоское имя: 'main', 'Math.add', 'Point.get'
    std::vector<IrParam> params;
    std::string return_type;
    std::vector<Instr> instrs;
    SourceLocation loc;
};

// Описание struct-типа: имя + упорядоченный список полей (имя, тип-строка).
// Нужен для бэкендов, которым требуется позиционная раскладка (LLVM).
export struct StructDef {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;  // имя → строка-тип
};

export struct Module {
    std::string name;
    std::vector<Function> functions;
    std::vector<StructDef> structs;
    // Псевдонимы типов (`type Name = Target;`): кодген должен раскрывать их
    // до примитива/массива/структуры. Хранятся плоско; для v1 этого хватает.
    std::vector<std::pair<std::string, std::string>> type_aliases;
};

// ===========================================================================
// Dump (текстовый вид)
// ===========================================================================

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

// ===========================================================================
// Lowerer: AST → IR
// ===========================================================================

export class Lowerer {
public:
    Lowerer(const ast::Program& prog, const sema::SemanticAnalyzer& sema_arg)
        : prog_(prog), sema_(sema_arg),
          expr_types_(sema_arg.expression_types()),
          methods_(sema_arg.methods()),
          imports_(sema_arg.imports()),
          module_scope_(sema_arg.module_scope()) {}

    Module lower();

    // Доступ к ещё-не-возвращённому списку структур (для тестов/отладки).
    const std::vector<StructDef>& structs() const noexcept { return struct_defs_; }

private:
    // --- декларации ---
    void lower_decls(const std::vector<std::unique_ptr<ast::Decl>>& decls,
                     const std::string& prefix);
    void lower_fn(const ast::FnDecl& fn, const std::string& flat_name);
    void lower_impl(const ast::ImplDecl& im);

    // --- инструкции ---
    void lower_block(const ast::BlockStmt& b);
    void lower_stmt(const ast::Stmt& s);
    void lower_var_decl(const ast::VarDeclStmt& v);
    void lower_assign(const ast::AssignStmt& a);
    void lower_return(const ast::ReturnStmt& r);
    void lower_if(const ast::IfStmt& s);
    void lower_while(const ast::WhileStmt& s);

    // --- выражения ---
    Operand lower_expr(const ast::Expr& e);
    Operand lower_unary(const ast::UnaryExpr& u);
    Operand lower_binary(const ast::BinaryExpr& b);
    Operand lower_field(const ast::FieldExpr& f);
    Operand lower_index(const ast::IndexExpr& i);
    Operand lower_call(const ast::CallExpr& c);
    Operand lower_array_lit(const ast::ArrayLit& a);
    Operand lower_struct_lit(const ast::StructLit& s);

    // --- helpers ---
    Operand fresh_temp();
    std::string fresh_label();
    void emit(Instr i);

    std::string type_to_str(const sema::Type& t) const { return t.to_string(); }
    std::string ast_type_to_str(const ast::TypeExpr& te) const;
    sema::Type type_of(const ast::Expr& e) const;

    // --- callee resolution ---
    struct CalleeInfo {
        enum class Kind { Function, Cast, BuiltinPrint, BuiltinInput,
                          BuiltinExit, BuiltinPanic, BuiltinAssert,
                          BuiltinLen, StaticMethod, InstanceMethod };
        Kind kind;
        std::string flat_name;       // плоское имя для эмиссии Call
        std::string cast_type;       // для Cast
        const ast::Expr* self_expr = nullptr;  // для InstanceMethod (lower → self argument)
    };
    CalleeInfo resolve_callee(const ast::Expr& callee);

    // --- state ---
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

// ===========================================================================
// Implementation
// ===========================================================================

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
            // Методы импла — на верхнем уровне (impl уровня namespace в v1 запрещён).
            lower_impl(*im);
        } else if (auto* sd = dynamic_cast<const ast::StructDecl*>(d.get())) {
            // Сохраняем позиционную раскладку для бэкендов (LLVM нужны индексы полей).
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
        // module / import не порождают IR-кода.
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

// ---------------------------------------------------------------------------
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
    // Не должно встречаться — unsupported.
}

void Lowerer::lower_var_decl(const ast::VarDeclStmt& v) {
    auto rhs = lower_expr(*v.init);
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
    // target формы: Ident | Field(Ident, name) | Index(Ident, expr) | глубже.
    if (auto* id = dynamic_cast<const ast::IdentExpr*>(a.target.get())) {
        Instr i; i.kind = InstrKind::Move; i.dst = Operand::var(id->name);
        i.has_dst = true; i.a = rhs; i.loc = a.loc;
        emit(std::move(i));
        return;
    }
    if (auto* fe = dynamic_cast<const ast::FieldExpr*>(a.target.get())) {
        // x.field = rhs (только если x — Ident; иначе через временную)
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
    // Не должно произойти — семантика проверила lvalue.
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

// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
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
    return "?";
}

sema::Type Lowerer::type_of(const ast::Expr& e) const {
    auto it = expr_types_.find(&e);
    if (it != expr_types_.end()) return it->second;
    return sema::Type(sema::Primitive::Void);  // shouldn't happen
}

// ===========================================================================
// Constant folding + tiny DCE (B.2.2)
// ===========================================================================
// SSA-предположение: temp'ы в Lowerer присваиваются один раз —
// КРОМЕ short-circuit && / ||, где результат-temp присваивается дважды.
// Поэтому propagation map для temp'ов инвалидируется при любом не-constant
// присваивании в Move/Bin/Un/Cast.

namespace {

bool is_numeric_const(const Operand& o) noexcept {
    using K = OperandKind;
    return o.kind == K::IntC || o.kind == K::FloatC
        || o.kind == K::BoolC || o.kind == K::CharC;
}
bool is_known_const(const Operand& o) noexcept {
    return is_numeric_const(o) || o.kind == OperandKind::StringC;
}

std::optional<Operand> try_fold_bin(BinOp op, const Operand& a, const Operand& b) {
    using K = OperandKind;
    if (a.kind == K::IntC && b.kind == K::IntC) {
        auto av = a.int_v, bv = b.int_v;
        switch (op) {
            case BinOp::Add: return Operand::int_c(av + bv);
            case BinOp::Sub: return Operand::int_c(av - bv);
            case BinOp::Mul: return Operand::int_c(av * bv);
            case BinOp::Div: if (bv == 0) return std::nullopt;
                              // Avoid INT64_MIN / -1 → UB.
                              if (av == std::numeric_limits<std::int64_t>::min() && bv == -1)
                                  return std::nullopt;
                              return Operand::int_c(av / bv);
            case BinOp::Mod: if (bv == 0) return std::nullopt;
                              if (av == std::numeric_limits<std::int64_t>::min() && bv == -1)
                                  return Operand::int_c(0);
                              return Operand::int_c(av % bv);
            case BinOp::Eq:    return Operand::bool_c(av == bv);
            case BinOp::NotEq: return Operand::bool_c(av != bv);
            case BinOp::Lt:    return Operand::bool_c(av < bv);
            case BinOp::Gt:    return Operand::bool_c(av > bv);
            case BinOp::LtEq:  return Operand::bool_c(av <= bv);
            case BinOp::GtEq:  return Operand::bool_c(av >= bv);
            case BinOp::Concat: return std::nullopt;
        }
    }
    if (a.kind == K::FloatC && b.kind == K::FloatC) {
        auto av = a.float_v, bv = b.float_v;
        switch (op) {
            case BinOp::Add: return Operand::float_c(av + bv);
            case BinOp::Sub: return Operand::float_c(av - bv);
            case BinOp::Mul: return Operand::float_c(av * bv);
            case BinOp::Div: return Operand::float_c(av / bv);  // IEEE 754 OK даже /0 → ±inf
            case BinOp::Eq:    return Operand::bool_c(av == bv);
            case BinOp::NotEq: return Operand::bool_c(av != bv);
            case BinOp::Lt:    return Operand::bool_c(av < bv);
            case BinOp::Gt:    return Operand::bool_c(av > bv);
            case BinOp::LtEq:  return Operand::bool_c(av <= bv);
            case BinOp::GtEq:  return Operand::bool_c(av >= bv);
            case BinOp::Mod:    return std::nullopt;
            case BinOp::Concat: return std::nullopt;
        }
    }
    if (a.kind == K::BoolC && b.kind == K::BoolC) {
        switch (op) {
            case BinOp::Eq:    return Operand::bool_c(a.bool_v == b.bool_v);
            case BinOp::NotEq: return Operand::bool_c(a.bool_v != b.bool_v);
            default:           return std::nullopt;
        }
    }
    if (a.kind == K::CharC && b.kind == K::CharC) {
        switch (op) {
            case BinOp::Eq:    return Operand::bool_c(a.char_v == b.char_v);
            case BinOp::NotEq: return Operand::bool_c(a.char_v != b.char_v);
            default:           return std::nullopt;
        }
    }
    if (a.kind == K::StringC && b.kind == K::StringC) {
        switch (op) {
            case BinOp::Concat: return Operand::string_c(a.str_v + b.str_v);
            case BinOp::Eq:     return Operand::bool_c(a.str_v == b.str_v);
            case BinOp::NotEq:  return Operand::bool_c(a.str_v != b.str_v);
            default:            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<Operand> try_fold_un(UnOp op, const Operand& a) {
    using K = OperandKind;
    if (op == UnOp::Neg) {
        if (a.kind == K::IntC) {
            // -INT64_MIN — UB; не сворачиваем.
            if (a.int_v == std::numeric_limits<std::int64_t>::min()) return std::nullopt;
            return Operand::int_c(-a.int_v);
        }
        if (a.kind == K::FloatC) return Operand::float_c(-a.float_v);
    }
    if (op == UnOp::Not && a.kind == K::BoolC) {
        return Operand::bool_c(!a.bool_v);
    }
    return std::nullopt;
}

std::optional<Operand> try_fold_cast(const std::string& target, const Operand& a) {
    using K = OperandKind;
    // Float widening / identity / narrowing.
    if (target == "float64" || target == "float32") {
        if (a.kind == K::IntC)   return Operand::float_c(static_cast<double>(a.int_v));
        if (a.kind == K::FloatC) return Operand::float_c(a.float_v);
    }
    // Int casts с truncation (semantics.md §6.4: wraparound).
    auto trunc_int = [&](const std::string& t, std::int64_t v) -> std::int64_t {
        if (t == "int8")   return static_cast<std::int8_t>(static_cast<std::uint8_t>(v));
        if (t == "int16")  return static_cast<std::int16_t>(static_cast<std::uint16_t>(v));
        if (t == "int32")  return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
        if (t == "int64")  return v;
        if (t == "uint8")  return static_cast<std::int64_t>(static_cast<std::uint8_t>(v));
        if (t == "uint16") return static_cast<std::int64_t>(static_cast<std::uint16_t>(v));
        if (t == "uint32") return static_cast<std::int64_t>(static_cast<std::uint32_t>(v));
        if (t == "uint64") return v;
        return v;
    };
    bool is_int_target = (target == "int8" || target == "int16" || target == "int32"
        || target == "int64" || target == "uint8" || target == "uint16"
        || target == "uint32" || target == "uint64");
    if (is_int_target) {
        if (a.kind == K::IntC)   return Operand::int_c(trunc_int(target, a.int_v));
        if (a.kind == K::FloatC) return Operand::int_c(trunc_int(target,
            static_cast<std::int64_t>(a.float_v)));
        if (a.kind == K::CharC)  return Operand::int_c(trunc_int(target,
            static_cast<std::int64_t>(a.char_v)));
    }
    if (target == "char" && a.kind == K::IntC) {
        return Operand::char_c(static_cast<std::uint32_t>(a.int_v));
    }
    return std::nullopt;
}

}  // anonymous

export void fold_constants_in_function(Function& fn) {
    // Пре-пасс: находим named-vars, которые: (а) присваиваются ровно один
    // раз и сразу константой, (б) не используются как база для StoreField /
    // StoreIndex (мутации внутри). Такие vars безопасно пропагировать.
    std::unordered_map<std::string, int> var_assigns;
    std::unordered_set<std::string> var_mutated;
    std::unordered_map<std::string, Operand> var_const_src;
    for (const auto& ins : fn.instrs) {
        if (ins.kind == InstrKind::Move && ins.dst.kind == OperandKind::Var) {
            ++var_assigns[ins.dst.str_v];
            if (is_known_const(ins.a)) {
                var_const_src[ins.dst.str_v] = ins.a;
            }
        }
        if (ins.kind == InstrKind::StoreField || ins.kind == InstrKind::StoreIndex) {
            if (ins.a.kind == OperandKind::Var) var_mutated.insert(ins.a.str_v);
        }
    }
    std::unordered_map<std::string, Operand> var_const;
    for (const auto& [name, count] : var_assigns) {
        if (count == 1 && var_mutated.find(name) == var_mutated.end()) {
            auto it = var_const_src.find(name);
            if (it != var_const_src.end()) var_const[name] = it->second;
        }
    }

    std::unordered_map<std::int64_t, Operand> temp_const;

    auto resolve = [&](Operand o) -> Operand {
        if (o.kind == OperandKind::Temp) {
            auto it = temp_const.find(o.int_v);
            if (it != temp_const.end()) return it->second;
        } else if (o.kind == OperandKind::Var) {
            auto it = var_const.find(o.str_v);
            if (it != var_const.end()) return it->second;
        }
        return o;
    };

    auto record_or_invalidate = [&](const Operand& dst, std::optional<Operand> v) {
        if (dst.kind != OperandKind::Temp) return;
        if (v) temp_const[dst.int_v] = *v;
        else temp_const.erase(dst.int_v);
    };

    for (auto& ins : fn.instrs) {
        // Подставить константы в операнды.
        ins.a = resolve(ins.a);
        ins.b = resolve(ins.b);
        ins.value_to_store = resolve(ins.value_to_store);
        for (auto& arg : ins.args) arg = resolve(arg);

        switch (ins.kind) {
            case InstrKind::Bin: {
                auto folded = try_fold_bin(ins.bin_op, ins.a, ins.b);
                if (folded) {
                    ins.kind = InstrKind::Move;
                    ins.a = *folded;
                    ins.b = Operand::unit();
                    record_or_invalidate(ins.dst, folded);
                } else {
                    record_or_invalidate(ins.dst, std::nullopt);
                }
                break;
            }
            case InstrKind::Un: {
                auto folded = try_fold_un(ins.un_op, ins.a);
                if (folded) {
                    ins.kind = InstrKind::Move;
                    ins.a = *folded;
                    record_or_invalidate(ins.dst, folded);
                } else {
                    record_or_invalidate(ins.dst, std::nullopt);
                }
                break;
            }
            case InstrKind::Cast: {
                auto folded = try_fold_cast(ins.type_name, ins.a);
                if (folded) {
                    ins.kind = InstrKind::Move;
                    ins.a = *folded;
                    ins.type_name.clear();
                    record_or_invalidate(ins.dst, folded);
                } else {
                    record_or_invalidate(ins.dst, std::nullopt);
                }
                break;
            }
            case InstrKind::Move: {
                if (is_known_const(ins.a)) {
                    record_or_invalidate(ins.dst, ins.a);
                } else {
                    record_or_invalidate(ins.dst, std::nullopt);
                }
                break;
            }
            case InstrKind::Branch: {
                if (ins.a.kind == OperandKind::BoolC) {
                    bool taken = ins.a.bool_v;
                    ins.kind = InstrKind::Goto;
                    ins.label = taken ? ins.label_then : ins.label_else;
                    ins.label_then.clear();
                    ins.label_else.clear();
                    ins.a = Operand::unit();
                }
                break;
            }
            case InstrKind::Call:
            case InstrKind::LoadIndex:
            case InstrKind::LoadField:
            case InstrKind::MakeArray:
            case InstrKind::MakeStruct: {
                // Не сворачиваем; результат — неизвестен → инвалидируем.
                if (ins.has_dst) record_or_invalidate(ins.dst, std::nullopt);
                break;
            }
            default: break;
        }
    }
}

// Простой DCE: удаляет «чистые» инструкции (без побочных эффектов),
// чей dst (temp или var) нигде не используется. Не трогает Call (могут
// иметь побочные эффекты), Store*, Branch, Goto, Label, Return.
export void dce_in_function(Function& fn) {
    std::unordered_set<std::int64_t> used_temps;
    std::unordered_set<std::string> used_vars;
    auto mark = [&](const Operand& o) {
        if (o.kind == OperandKind::Temp) used_temps.insert(o.int_v);
        else if (o.kind == OperandKind::Var) used_vars.insert(o.str_v);
    };
    for (const auto& ins : fn.instrs) {
        mark(ins.a); mark(ins.b); mark(ins.value_to_store);
        for (const auto& arg : ins.args) mark(arg);
    }
    auto is_pure_with_dst = [](const Instr& i) {
        using K = InstrKind;
        return i.has_dst
            && (i.kind == K::Move || i.kind == K::Bin || i.kind == K::Un
             || i.kind == K::Cast || i.kind == K::LoadIndex || i.kind == K::LoadField
             || i.kind == K::MakeArray || i.kind == K::MakeStruct);
    };
    fn.instrs.erase(
        std::remove_if(fn.instrs.begin(), fn.instrs.end(),
            [&](const Instr& i) {
                if (!is_pure_with_dst(i)) return false;
                if (i.dst.kind == OperandKind::Temp)
                    return used_temps.find(i.dst.int_v) == used_temps.end();
                if (i.dst.kind == OperandKind::Var)
                    return used_vars.find(i.dst.str_v) == used_vars.end();
                return false;
            }),
        fn.instrs.end());
}

export void optimize_module(Module& m) {
    for (auto& fn : m.functions) {
        // Чередуем fold ↔ DCE до фиксации (или 4 итераций — учебный потолок).
        std::size_t prev = 0, curr = fn.instrs.size();
        for (int pass = 0; pass < 4 && prev != curr; ++pass) {
            prev = fn.instrs.size();
            fold_constants_in_function(fn);
            dce_in_function(fn);
            curr = fn.instrs.size();
        }
    }
}

}  // namespace herta::ir
