// Module `herta.interp` — исполнитель IR (регистровая виртуальная машина).
//
// Финальная фаза pipeline (codegen.md, путь «Интерпретация»). Принимает
// набор IR-модулей из herta.ir в топологическом порядке и исполняет функцию
// `main` напрямую, без генерации нативного кода.
//
// Модель ВМ:
//   * Каждый вызов функции — кадр (Frame) с независимыми хранилищами
//     именованных переменных (Var) и временных регистров (Temp).
//   * Инструкции исполняются по программному счётчику; метки разрешаются
//     в индексы через предвычисленную карту label → index.
//   * Значения — value semantics: массивы и структуры копируются при
//     присваивании (хранятся по значению внутри Value).
//
// Runtime-ошибки (деление на ноль, выход за границы), `panic`, `assert` и
// `exit` немедленно завершают процесс через std::exit с нужным кодом — это
// исполнение интерпретируемой программы, а не диагностика компилятора.

export module herta.interp;

import std;
import herta.common;
import herta.ir;

namespace herta::interp {

using herta::common::SourceLocation;
namespace ir = herta::ir;

// ===========================================================================
// Runtime value (value semantics)
// ===========================================================================

struct Value;
struct StructVal {
    std::string type_name;
    std::map<std::string, Value> fields;  // sorted → порядок-независимое равенство
};

struct Value {
    // monostate = unit/void; int64 покрывает все целые; double — вещественные;
    // uint32 — char (codepoint); string; vector — массив; StructVal — структура.
    std::variant<std::monostate, std::int64_t, double, bool, std::uint32_t,
                 std::string, std::vector<Value>, StructVal> data;

    static Value unit() { return Value{std::monostate{}}; }
    static Value integer(std::int64_t v) { return Value{v}; }
    static Value real(double v) { return Value{v}; }
    static Value boolean(bool v) { return Value{v}; }
    static Value character(std::uint32_t v) { return Value{v}; }
    static Value str(std::string v) { return Value{std::move(v)}; }

    bool is_int() const { return std::holds_alternative<std::int64_t>(data); }
    bool is_float() const { return std::holds_alternative<double>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_char() const { return std::holds_alternative<std::uint32_t>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<std::vector<Value>>(data); }
    bool is_struct() const { return std::holds_alternative<StructVal>(data); }

    std::int64_t as_int() const { return std::get<std::int64_t>(data); }
    double as_float() const { return std::get<double>(data); }
    bool as_bool() const { return std::get<bool>(data); }
    std::uint32_t as_char() const { return std::get<std::uint32_t>(data); }
    const std::string& as_string() const { return std::get<std::string>(data); }
    std::vector<Value>& as_array() { return std::get<std::vector<Value>>(data); }
    const std::vector<Value>& as_array() const { return std::get<std::vector<Value>>(data); }
    StructVal& as_struct() { return std::get<StructVal>(data); }
    const StructVal& as_struct() const { return std::get<StructVal>(data); }
};

// Глубокое равенство (для ==/!= над любыми типами, включая массивы/структуры).
bool value_eq(const Value& a, const Value& b) {
    if (a.data.index() != b.data.index()) {
        // Разные альтернативы: единственный осмысленный кросс-тип — числовой
        // (int ↔ float после неявного widening). Прочее семантика отсекла.
        if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            double av = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
            double bv = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
            return av == bv;
        }
        return false;
    }
    if (a.is_int())    return a.as_int() == b.as_int();
    if (a.is_float())  return a.as_float() == b.as_float();
    if (a.is_bool())   return a.as_bool() == b.as_bool();
    if (a.is_char())   return a.as_char() == b.as_char();
    if (a.is_string()) return a.as_string() == b.as_string();
    if (a.is_array()) {
        const auto& xa = a.as_array(); const auto& xb = b.as_array();
        if (xa.size() != xb.size()) return false;
        for (std::size_t i = 0; i < xa.size(); ++i)
            if (!value_eq(xa[i], xb[i])) return false;
        return true;
    }
    if (a.is_struct()) {
        const auto& sa = a.as_struct(); const auto& sb = b.as_struct();
        if (sa.type_name != sb.type_name) return false;
        if (sa.fields.size() != sb.fields.size()) return false;
        for (const auto& [k, v] : sa.fields) {
            auto it = sb.fields.find(k);
            if (it == sb.fields.end() || !value_eq(v, it->second)) return false;
        }
        return true;
    }
    return true;  // unit == unit
}

// ===========================================================================
// UTF-8 кодирование codepoint → байты (для print char).
// ===========================================================================

std::string encode_utf8(std::uint32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string format_scalar(const Value& v) {
    if (v.is_int())    return std::to_string(v.as_int());
    if (v.is_float())  return std::format("{}", v.as_float());
    if (v.is_bool())   return v.as_bool() ? "true" : "false";
    if (v.is_char())   return encode_utf8(v.as_char());
    if (v.is_string()) return v.as_string();
    return "()";  // unit
}

// ===========================================================================
// Interpreter
// ===========================================================================

export class Interpreter {
public:
    explicit Interpreter(std::vector<ir::Module> modules)
        : modules_(std::move(modules)) {
        build_tables();
    }

    // Исполняет main; возвращает её int-результат как код завершения процесса.
    int run();

private:
    struct Frame {
        std::unordered_map<std::string, Value> vars;
        std::unordered_map<std::int64_t, Value> temps;
    };

    void build_tables();

    const ir::Function* resolve(std::size_t caller_mod,
                                const std::string& callee,
                                std::size_t& out_mod) const;

    Value call(const ir::Function& fn, std::size_t mod, std::vector<Value> args);

    // Исполнение тела функции: возвращает её return-значение.
    Value exec_body(const ir::Function& fn, std::size_t mod, Frame& fr);

    Value eval(const ir::Operand& o, Frame& fr) const;
    Value* lvalue(const ir::Operand& o, Frame& fr);

    Value do_bin(ir::BinOp op, const Value& a, const Value& b,
                 const SourceLocation& loc) const;
    Value do_un(ir::UnOp op, const Value& a) const;
    Value do_cast(const std::string& target, const Value& a) const;

    // Builtin'ы; возвращают Value (unit для void). may_have_dst — есть ли приёмник.
    std::optional<Value> try_builtin(const std::string& name,
                                     std::vector<Value>& args,
                                     const SourceLocation& loc);

    [[noreturn]] void runtime_error(const std::string& msg,
                                    const SourceLocation& loc) const;

    std::vector<ir::Module> modules_;
    // По модулю: имя функции → указатель в modules_.
    std::vector<std::unordered_map<std::string, const ir::Function*>> module_funcs_;
    std::unordered_map<std::string, std::size_t> module_idx_by_name_;
    std::size_t main_mod_ = 0;
    const ir::Function* main_fn_ = nullptr;
};

// ---------------------------------------------------------------------------
void Interpreter::build_tables() {
    module_funcs_.resize(modules_.size());
    for (std::size_t mi = 0; mi < modules_.size(); ++mi) {
        module_idx_by_name_[modules_[mi].name] = mi;
        for (const auto& fn : modules_[mi].functions) {
            module_funcs_[mi][fn.name] = &fn;
            if (fn.name == "main") {
                main_fn_ = &fn;
                main_mod_ = mi;
            }
        }
    }
}

const ir::Function* Interpreter::resolve(std::size_t caller_mod,
                                         const std::string& callee,
                                         std::size_t& out_mod) const {
    // 1. Локально в модуле вызывающего (покрывает плоские имена, namespace.fn
    //    и Type.method — все они лежат в карте того же модуля).
    const auto& local = module_funcs_[caller_mod];
    if (auto it = local.find(callee); it != local.end()) {
        out_mod = caller_mod;
        return it->second;
    }
    // 2. Кросс-модульный вызов Module.fn.
    if (auto dot = callee.find('.'); dot != std::string::npos) {
        auto head = callee.substr(0, dot);
        auto rest = callee.substr(dot + 1);
        if (auto mit = module_idx_by_name_.find(head);
            mit != module_idx_by_name_.end()) {
            const auto& m = module_funcs_[mit->second];
            if (auto it = m.find(rest); it != m.end()) {
                out_mod = mit->second;
                return it->second;
            }
        }
    }
    return nullptr;
}

int Interpreter::run() {
    if (!main_fn_) {
        std::cerr << "internal: no main function in interpreted program\n";
        return 1;
    }
    Value result = call(*main_fn_, main_mod_, {});
    if (result.is_int()) {
        return static_cast<int>(result.as_int());
    }
    return 0;
}

Value Interpreter::call(const ir::Function& fn, std::size_t mod,
                        std::vector<Value> args) {
    Frame fr;
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        fr.vars[fn.params[i].name] = std::move(args[i]);
    }
    return exec_body(fn, mod, fr);
}

Value Interpreter::exec_body(const ir::Function& fn, std::size_t mod, Frame& fr) {
    // Карта label → индекс инструкции.
    std::unordered_map<std::string, std::size_t> labels;
    for (std::size_t i = 0; i < fn.instrs.size(); ++i) {
        if (fn.instrs[i].kind == ir::InstrKind::Label) {
            labels[fn.instrs[i].label] = i;
        }
    }

    std::size_t pc = 0;
    while (pc < fn.instrs.size()) {
        const ir::Instr& ins = fn.instrs[pc];
        using K = ir::InstrKind;
        switch (ins.kind) {
            case K::Label:
                break;
            case K::Goto:
                pc = labels.at(ins.label);
                continue;
            case K::Branch: {
                Value c = eval(ins.a, fr);
                pc = labels.at(c.as_bool() ? ins.label_then : ins.label_else);
                continue;
            }
            case K::Return:
                return ins.has_ret_value ? eval(ins.a, fr) : Value::unit();
            case K::Move:
                *lvalue(ins.dst, fr) = eval(ins.a, fr);
                break;
            case K::Bin:
                *lvalue(ins.dst, fr) =
                    do_bin(ins.bin_op, eval(ins.a, fr), eval(ins.b, fr), ins.loc);
                break;
            case K::Un:
                *lvalue(ins.dst, fr) = do_un(ins.un_op, eval(ins.a, fr));
                break;
            case K::Cast:
                *lvalue(ins.dst, fr) = do_cast(ins.type_name, eval(ins.a, fr));
                break;
            case K::Call: {
                std::vector<Value> args;
                args.reserve(ins.args.size());
                for (const auto& a : ins.args) args.push_back(eval(a, fr));

                if (auto b = try_builtin(ins.callee, args, ins.loc)) {
                    if (ins.has_dst) *lvalue(ins.dst, fr) = std::move(*b);
                    break;
                }
                std::size_t target_mod = 0;
                const ir::Function* callee = resolve(mod, ins.callee, target_mod);
                if (!callee) {
                    std::cerr << "internal: unresolved call '" << ins.callee << "'\n";
                    std::exit(1);
                }
                Value ret = call(*callee, target_mod, std::move(args));
                if (ins.has_dst) *lvalue(ins.dst, fr) = std::move(ret);
                break;
            }
            case K::LoadIndex: {
                Value base = eval(ins.a, fr);
                std::int64_t idx = eval(ins.b, fr).as_int();
                const auto& arr = base.as_array();
                if (idx < 0 || static_cast<std::size_t>(idx) >= arr.size()) {
                    runtime_error("index out of bounds: " + std::to_string(idx) +
                                  ", size " + std::to_string(arr.size()), ins.loc);
                }
                *lvalue(ins.dst, fr) = arr[static_cast<std::size_t>(idx)];
                break;
            }
            case K::StoreIndex: {
                Value* base = lvalue(ins.a, fr);
                std::int64_t idx = eval(ins.b, fr).as_int();
                auto& arr = base->as_array();
                if (idx < 0 || static_cast<std::size_t>(idx) >= arr.size()) {
                    runtime_error("index out of bounds: " + std::to_string(idx) +
                                  ", size " + std::to_string(arr.size()), ins.loc);
                }
                arr[static_cast<std::size_t>(idx)] = eval(ins.value_to_store, fr);
                break;
            }
            case K::LoadField: {
                Value base = eval(ins.a, fr);
                *lvalue(ins.dst, fr) = base.as_struct().fields.at(ins.field);
                break;
            }
            case K::StoreField: {
                Value* base = lvalue(ins.a, fr);
                base->as_struct().fields[ins.field] = eval(ins.value_to_store, fr);
                break;
            }
            case K::MakeArray: {
                std::vector<Value> elems;
                elems.reserve(ins.args.size());
                for (const auto& a : ins.args) elems.push_back(eval(a, fr));
                *lvalue(ins.dst, fr) = Value{std::move(elems)};
                break;
            }
            case K::MakeStruct: {
                StructVal sv;
                sv.type_name = ins.type_name;
                for (std::size_t i = 0; i < ins.args.size(); ++i) {
                    sv.fields[ins.struct_field_names[i]] = eval(ins.args[i], fr);
                }
                *lvalue(ins.dst, fr) = Value{std::move(sv)};
                break;
            }
        }
        ++pc;
    }
    return Value::unit();  // упала с конца (void-функция)
}

// ---------------------------------------------------------------------------
Value Interpreter::eval(const ir::Operand& o, Frame& fr) const {
    using OK = ir::OperandKind;
    switch (o.kind) {
        case OK::Temp: {
            auto it = fr.temps.find(o.int_v);
            return it != fr.temps.end() ? it->second : Value::unit();
        }
        case OK::Var: {
            auto it = fr.vars.find(o.str_v);
            return it != fr.vars.end() ? it->second : Value::unit();
        }
        case OK::IntC:    return Value::integer(o.int_v);
        case OK::FloatC:  return Value::real(o.float_v);
        case OK::BoolC:   return Value::boolean(o.bool_v);
        case OK::StringC: return Value::str(o.str_v);
        case OK::CharC:   return Value::character(o.char_v);
        case OK::Unit:    return Value::unit();
    }
    return Value::unit();
}

Value* Interpreter::lvalue(const ir::Operand& o, Frame& fr) {
    if (o.kind == ir::OperandKind::Var) return &fr.vars[o.str_v];
    // Temp (единственная иная цель приёмника в нашем IR).
    return &fr.temps[o.int_v];
}

// ---------------------------------------------------------------------------
Value Interpreter::do_bin(ir::BinOp op, const Value& a, const Value& b,
                          const SourceLocation& loc) const {
    using B = ir::BinOp;

    if (op == B::Eq)    return Value::boolean(value_eq(a, b));
    if (op == B::NotEq) return Value::boolean(!value_eq(a, b));
    if (op == B::Concat)
        return Value::str(a.as_string() + b.as_string());

    // Сравнения строк (лексикографически) и char.
    if (a.is_string() && b.is_string()) {
        const auto& x = a.as_string(); const auto& y = b.as_string();
        switch (op) {
            case B::Lt:   return Value::boolean(x < y);
            case B::Gt:   return Value::boolean(x > y);
            case B::LtEq: return Value::boolean(x <= y);
            case B::GtEq: return Value::boolean(x >= y);
            default: break;
        }
    }
    if (a.is_char() && b.is_char()) {
        auto x = a.as_char(); auto y = b.as_char();
        switch (op) {
            case B::Lt:   return Value::boolean(x < y);
            case B::Gt:   return Value::boolean(x > y);
            case B::LtEq: return Value::boolean(x <= y);
            case B::GtEq: return Value::boolean(x >= y);
            default: break;
        }
    }

    // Числовые операции. Если хотя бы один операнд float — обе ветви в double.
    bool float_mode = a.is_float() || b.is_float();
    if (float_mode) {
        double x = a.is_float() ? a.as_float() : static_cast<double>(a.as_int());
        double y = b.is_float() ? b.as_float() : static_cast<double>(b.as_int());
        switch (op) {
            case B::Add: return Value::real(x + y);
            case B::Sub: return Value::real(x - y);
            case B::Mul: return Value::real(x * y);
            case B::Div: return Value::real(x / y);  // IEEE 754: /0 → ±inf
            case B::Lt:   return Value::boolean(x < y);
            case B::Gt:   return Value::boolean(x > y);
            case B::LtEq: return Value::boolean(x <= y);
            case B::GtEq: return Value::boolean(x >= y);
            default: break;
        }
    } else {
        std::int64_t x = a.as_int(), y = b.as_int();
        switch (op) {
            case B::Add: return Value::integer(x + y);
            case B::Sub: return Value::integer(x - y);
            case B::Mul: return Value::integer(x * y);
            case B::Div:
                if (y == 0) runtime_error("division by zero", loc);
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    return Value::integer(x);  // wraparound, избегаем UB
                return Value::integer(x / y);
            case B::Mod:
                if (y == 0) runtime_error("division by zero", loc);
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    return Value::integer(0);
                return Value::integer(x % y);
            case B::Lt:   return Value::boolean(x < y);
            case B::Gt:   return Value::boolean(x > y);
            case B::LtEq: return Value::boolean(x <= y);
            case B::GtEq: return Value::boolean(x >= y);
            default: break;
        }
    }
    return Value::unit();  // unreachable при корректной семантике
}

Value Interpreter::do_un(ir::UnOp op, const Value& a) const {
    if (op == ir::UnOp::Neg) {
        if (a.is_float()) return Value::real(-a.as_float());
        std::int64_t v = a.as_int();
        if (v == std::numeric_limits<std::int64_t>::min()) return Value::integer(v);
        return Value::integer(-v);
    }
    // Not
    return Value::boolean(!a.as_bool());
}

Value Interpreter::do_cast(const std::string& target, const Value& a) const {
    auto src_to_int = [&]() -> std::int64_t {
        if (a.is_int())   return a.as_int();
        if (a.is_float()) return static_cast<std::int64_t>(a.as_float());
        if (a.is_char())  return static_cast<std::int64_t>(a.as_char());
        if (a.is_bool())  return a.as_bool() ? 1 : 0;
        return 0;
    };
    if (target == "float64" || target == "float32") {
        if (a.is_int())   return Value::real(static_cast<double>(a.as_int()));
        if (a.is_float()) return Value::real(a.as_float());
        if (a.is_char())  return Value::real(static_cast<double>(a.as_char()));
    }
    if (target == "char") {
        return Value::character(static_cast<std::uint32_t>(src_to_int()));
    }
    // Целочисленные приведения с усечением по разрядности (semantics §6.4).
    std::int64_t v = src_to_int();
    if (target == "int8")   return Value::integer(static_cast<std::int8_t>(static_cast<std::uint8_t>(v)));
    if (target == "int16")  return Value::integer(static_cast<std::int16_t>(static_cast<std::uint16_t>(v)));
    if (target == "int32")  return Value::integer(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
    if (target == "int64")  return Value::integer(v);
    if (target == "uint8")  return Value::integer(static_cast<std::int64_t>(static_cast<std::uint8_t>(v)));
    if (target == "uint16") return Value::integer(static_cast<std::int64_t>(static_cast<std::uint16_t>(v)));
    if (target == "uint32") return Value::integer(static_cast<std::int64_t>(static_cast<std::uint32_t>(v)));
    if (target == "uint64") return Value::integer(v);
    return a;  // тождественное приведение
}

std::optional<Value> Interpreter::try_builtin(const std::string& name,
                                              std::vector<Value>& args,
                                              const SourceLocation& loc) {
    if (name == "print") {
        std::string out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out.push_back(' ');
            out += format_scalar(args[i]);
        }
        out.push_back('\n');
        std::cout << out;
        return Value::unit();
    }
    if (name == "input") {
        std::string line;
        std::getline(std::cin, line);
        return Value::str(std::move(line));
    }
    if (name == "exit") {
        int code = args.empty() ? 0 : static_cast<int>(args[0].as_int());
        std::cout.flush();
        std::exit(code);
    }
    if (name == "panic") {
        std::cout.flush();
        std::cerr << "panic: " << (args.empty() ? "" : args[0].as_string()) << '\n';
        std::exit(1);
    }
    if (name == "assert") {
        if (args.empty() || !args[0].as_bool()) {
            std::cout.flush();
            std::cerr << "assertion failed at line " << loc.line << '\n';
            std::exit(1);
        }
        return Value::unit();
    }
    if (name == "len") {
        if (!args.empty() && args[0].is_string())
            return Value::integer(static_cast<std::int64_t>(args[0].as_string().size()));
        if (!args.empty() && args[0].is_array())
            return Value::integer(static_cast<std::int64_t>(args[0].as_array().size()));
        return Value::integer(0);
    }
    return std::nullopt;
}

void Interpreter::runtime_error(const std::string& msg,
                                const SourceLocation& loc) const {
    std::cout.flush();
    std::cerr << "runtime error: " << msg << " at line " << loc.line << '\n';
    std::exit(1);
}

}  // namespace herta::interp
