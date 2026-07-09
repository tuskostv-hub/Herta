// Бэкенд. Принимает IR-модули в топологическом порядке, склеивает их в один
// LLVM-модуль и отдаёт текст .ll. Дальше его собирает clang вместе с runtime.c.
//
// Каждая переменная и каждый временный регистр получают alloca во входном
// блоке функции, чтение и запись идут через load/store. clang -O1 потом
// прогоняет mem2reg и почти всё это разворачивается обратно в регистры.
// Массивы и структуры — обычные LLVM-агрегаты в alloca того же типа,
// копирование по значению получается через load и store целиком. Строки —
// пара { i64 len, ptr data }, операции с ними идут через функции C-рантайма.
//
// Неявное расширение типов в IR явно не несётся, sext/zext/sitofp вставляются
// здесь в точке использования. Перед делением, остатком и индексацией массивов
// эмитятся проверки с переходом в helper-ы из runtime.c.

export module herta.llvm;

import std;
import herta.common;
import herta.ir;

namespace herta::llvm_be {

namespace ir = herta::ir;

struct TypeInfo {
    enum class Kind { Int, UInt, Float, Bool, Char, String, Void, Byte,
                      Array, Struct, Pointer };
    Kind kind = Kind::Void;
    int bits = 0;                 // для int/uint/float
    int array_size = 0;           // для Array
    std::string elem_or_name;     // элемент для Array/Pointer, имя для Struct
};

bool is_digit(char c) { return c >= '0' && c <= '9'; }

TypeInfo parse_type(std::string_view t) {
    TypeInfo r;
    if (t == "void" || t.empty()) { r.kind = TypeInfo::Kind::Void; return r; }
    if (t == "bool")   { r.kind = TypeInfo::Kind::Bool; return r; }
    if (t == "char")   { r.kind = TypeInfo::Kind::Char; return r; }
    if (t == "byte")   { r.kind = TypeInfo::Kind::Byte; return r; }
    if (t == "string") { r.kind = TypeInfo::Kind::String; return r; }
    if (t == "float32") { r.kind = TypeInfo::Kind::Float; r.bits = 32; return r; }
    if (t == "float64") { r.kind = TypeInfo::Kind::Float; r.bits = 64; return r; }
    // *T — указатель
    if (!t.empty() && t.front() == '*') {
        r.kind = TypeInfo::Kind::Pointer;
        r.elem_or_name = std::string(t.substr(1));
        return r;
    }
    auto try_prefix = [&](std::string_view p, TypeInfo::Kind k) {
        if (t.size() > p.size() && t.starts_with(p)) {
            int bits = 0;
            for (std::size_t i = p.size(); i < t.size(); ++i) {
                if (!is_digit(t[i])) return false;
                bits = bits * 10 + (t[i] - '0');
            }
            r.kind = k; r.bits = bits; return true;
        }
        return false;
    };
    if (try_prefix("int", TypeInfo::Kind::Int))   return r;
    if (try_prefix("uint", TypeInfo::Kind::UInt)) return r;
    // [T; N]
    if (t.size() >= 4 && t.front() == '[') {
        // Скан с учётом вложенности: найти `;` и `]` на нулевой глубине.
        int depth = 0;
        auto semi = std::string_view::npos;
        auto rb = std::string_view::npos;
        for (std::size_t k = 1; k < t.size(); ++k) {
            if (t[k] == '[') { ++depth; }
            else if (t[k] == ']') {
                if (depth == 0) { rb = k; break; }
                --depth;
            }
            else if (t[k] == ';' && depth == 0 && semi == std::string_view::npos) {
                semi = k;
            }
        }
        if (semi != std::string_view::npos && rb != std::string_view::npos) {
            r.kind = TypeInfo::Kind::Array;
            r.elem_or_name = std::string(t.substr(1, semi - 1));
            // Очистить пробелы вокруг N.
            auto num_sv = t.substr(semi + 1, rb - semi - 1);
            while (!num_sv.empty() && num_sv.front() == ' ') num_sv.remove_prefix(1);
            while (!num_sv.empty() && num_sv.back() == ' ') num_sv.remove_suffix(1);
            int n = 0;
            for (char c : num_sv) {
                if (!is_digit(c)) return r;  // повреждено — пусть упадёт ниже
                n = n * 10 + (c - '0');
            }
            r.array_size = n;
            return r;
        }
    }
    // Имя структуры.
    r.kind = TypeInfo::Kind::Struct;
    r.elem_or_name = std::string(t);
    return r;
}

std::string llvm_type(const TypeInfo& ti) {
    switch (ti.kind) {
        case TypeInfo::Kind::Void:   return "void";
        case TypeInfo::Kind::Bool:   return "i1";
        case TypeInfo::Kind::Char:   return "i32";
        case TypeInfo::Kind::Byte:   return "i8";
        case TypeInfo::Kind::Int:
        case TypeInfo::Kind::UInt:   return std::format("i{}", ti.bits);
        case TypeInfo::Kind::Float:  return ti.bits == 32 ? "float" : "double";
        case TypeInfo::Kind::String: return "%struct.herta_string";
        case TypeInfo::Kind::Array: {
            auto el = parse_type(ti.elem_or_name);
            return std::format("[{} x {}]", ti.array_size, llvm_type(el));
        }
        case TypeInfo::Kind::Struct: return "%struct." + ti.elem_or_name;
        case TypeInfo::Kind::Pointer: return "ptr";  // opaque LLVM pointer
    }
    return "void";
}

std::string llvm_type(std::string_view t) { return llvm_type(parse_type(t)); }

bool is_signed_int_kind(TypeInfo::Kind k) { return k == TypeInfo::Kind::Int; }
bool is_int_or_char(const TypeInfo& t) {
    return t.kind == TypeInfo::Kind::Int || t.kind == TypeInfo::Kind::UInt
        || t.kind == TypeInfo::Kind::Char || t.kind == TypeInfo::Kind::Bool
        || t.kind == TypeInfo::Kind::Byte;  // byte ↔ int только через явный cast
}
bool is_float(const TypeInfo& t) { return t.kind == TypeInfo::Kind::Float; }

// Общий числовой тип для widening. char и bool здесь не участвуют
// (они только в Eq/NotEq, обработанных отдельно).
std::string common_numeric(const std::string& a, const std::string& b) {
    auto ta = parse_type(a), tb = parse_type(b);
    if (is_float(ta) || is_float(tb)) {
        int bits = std::max(is_float(ta) ? ta.bits : 0,
                            is_float(tb) ? tb.bits : 0);
        return bits == 32 ? "float32" : "float64";
    }
    // Оба целые/char. Берём больший по разрядности; если char (i32) — char.
    int abits = ta.bits ? ta.bits : (ta.kind == TypeInfo::Kind::Char ? 32 : 32);
    int bbits = tb.bits ? tb.bits : (tb.kind == TypeInfo::Kind::Char ? 32 : 32);
    int bits = std::max(abits, bbits);
    bool any_unsigned = ta.kind == TypeInfo::Kind::UInt
                     || tb.kind == TypeInfo::Kind::UInt;
    return std::format("{}{}", any_unsigned ? "uint" : "int", bits);
}

export class Emitter {
public:
    explicit Emitter(std::vector<ir::Module> modules)
        : modules_(std::move(modules)) {}

    std::string emit();

private:
    void emit_preamble();
    void emit_struct_typedefs();
    void emit_string_globals();
    void emit_runtime_decls();

    // Пер-функциональное состояние.
    struct FnState {
        const ir::Module* mod = nullptr;
        const ir::Function* fn = nullptr;
        // Имя var → herta-type string. Заполняется по мере встречи Move.
        std::unordered_map<std::string, std::string> var_ty;
        // id temp → herta-type string.
        std::unordered_map<std::int64_t, std::string> temp_ty;
        // Имя var → имя alloca в LLVM (например, "%x.addr").
        std::unordered_map<std::string, std::string> var_addr;
        // id temp → имя alloca.
        std::unordered_map<std::int64_t, std::string> temp_addr;
        // Тело функции (накапливаемое).
        std::string body;
        // Прологе перед первым label: alloca.
        std::string prolog;
        int ssa_counter = 0;
        // true сразу после терминатора (ret/br) — следующая инструкция
        // обязана начать новый блок, иначе LLVM IR невалиден.
        bool terminated = false;
    };

    // Если текущая позиция «после терминатора» — открыть новый блок мёртвого
    // кода с уникальной меткой. Вызывать перед эмиссией любой не-Label
    // инструкции.
    void ensure_block(FnState& st);

    void infer_types(FnState& st);
    void emit_function(std::size_t mod_idx, const ir::Function& fn);
    std::string mangle(std::size_t mod_idx, const std::string& fn_name) const;
    std::string resolve_callee(std::size_t caller_mod, const std::string& callee,
                               std::size_t& out_mod) const;

    std::string fresh_ssa(FnState& st);
    void ensure_alloca_var(FnState& st, const std::string& name,
                           const std::string& herta_ty);
    void ensure_alloca_temp(FnState& st, std::int64_t id,
                            const std::string& herta_ty);
    // Возвращает (llvm_value, herta_type).
    std::pair<std::string, std::string> eval_operand(FnState& st,
                                                     const ir::Operand& o);
    void store_to(FnState& st, const ir::Operand& dst,
                  const std::string& value, const std::string& herta_ty);
    std::string widen(FnState& st, const std::string& value,
                      const std::string& from_ty, const std::string& to_ty);

    void emit_instr(FnState& st, const ir::Instr& ins);
    void emit_call(FnState& st, const ir::Instr& ins);
    void emit_bin(FnState& st, const ir::Instr& ins);
    void emit_load_index(FnState& st, const ir::Instr& ins);
    void emit_store_index(FnState& st, const ir::Instr& ins);
    void emit_make_array(FnState& st, const ir::Instr& ins);
    void emit_make_struct(FnState& st, const ir::Instr& ins);
    void emit_cast(FnState& st, const ir::Instr& ins);

    std::vector<ir::Module> modules_;
    std::string out_;
    // Пер-модуль: имя функции → индекс модуля (для resolve), и общий список
    // канонических LLVM-имён.
    std::vector<std::unordered_map<std::string, const ir::Function*>> mod_funcs_;
    std::unordered_map<std::string, std::size_t> mod_idx_;
    // Интернированные строковые литералы.
    std::vector<std::string> strings_;
    std::unordered_map<std::string, std::size_t> string_idx_;
    // Описания структур (объединённые из всех модулей; имена уникальны).
    std::unordered_map<std::string, ir::StructDef> struct_defs_;
    // Type aliases (`type Name = Target;`) — после построения раскрываем
    // во всех типовых строках в modules_ заранее, чтобы parse_type/llvm_type
    // могли быть простыми функциями без контекста.
    std::unordered_map<std::string, std::string> alias_map_;
    std::string resolve_type(std::string s) const;
    void resolve_aliases_in_modules();
    // Индекс поля для GEP по имени структуры и имени поля.
    int field_index(const std::string& struct_name, const std::string& field) const;
};

std::string Emitter::resolve_type(std::string s) const {
    // Рекурсивно раскрываем элемент массива (с учётом вложенности).
    if (s.size() >= 4 && s.front() == '[') {
        int depth = 0;
        auto semi = std::string::npos;
        auto rb = std::string::npos;
        for (std::size_t k = 1; k < s.size(); ++k) {
            if (s[k] == '[') { ++depth; }
            else if (s[k] == ']') {
                if (depth == 0) { rb = k; break; }
                --depth;
            }
            else if (s[k] == ';' && depth == 0 && semi == std::string::npos) {
                semi = k;
            }
        }
        if (semi != std::string::npos && rb != std::string::npos) {
            auto elem = s.substr(1, semi - 1);
            auto rest = s.substr(semi);  // "; N]"
            return "[" + resolve_type(elem) + rest;
        }
    }
    // Раскрываем алиас итеративно (защита от циклов — лимит).
    for (int i = 0; i < 8; ++i) {
        auto it = alias_map_.find(s);
        if (it == alias_map_.end()) break;
        s = it->second;
    }
    return s;
}

void Emitter::resolve_aliases_in_modules() {
    for (auto& mod : modules_) {
        for (auto& fn : mod.functions) {
            fn.return_type = resolve_type(fn.return_type);
            for (auto& p : fn.params) p.type_str = resolve_type(p.type_str);
            for (auto& ins : fn.instrs) {
                using IK = ir::InstrKind;
                if (ins.kind == IK::Cast || ins.kind == IK::MakeArray
                 || ins.kind == IK::MakeStruct) {
                    ins.type_name = resolve_type(ins.type_name);
                }
            }
        }
        for (auto& sd : mod.structs) {
            for (auto& f : sd.fields) f.second = resolve_type(f.second);
        }
    }
}

std::string Emitter::emit() {
    // Сбор алиасов (по всем модулям, плоско).
    for (const auto& mod : modules_) {
        for (const auto& [name, target] : mod.type_aliases) {
            alias_map_[name] = target;
        }
    }
    resolve_aliases_in_modules();

    // Построение таблиц и сбор глобалок (структуры, главный модуль).
    mod_funcs_.resize(modules_.size());
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        mod_idx_[modules_[i].name] = i;
        for (const auto& fn : modules_[i].functions) {
            mod_funcs_[i][fn.name] = &fn;
        }
        for (const auto& sd : modules_[i].structs) {
            struct_defs_[sd.name] = sd;
        }
    }

    emit_preamble();
    emit_struct_typedefs();
    emit_runtime_decls();
    // Эмиссия функций (струнные литералы могут открыться по ходу).
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        for (const auto& fn : modules_[i].functions) {
            emit_function(i, fn);
        }
    }
    // Строковые глобалки — выводим в начало, поэтому собираем в отдельный
    // буфер и подмешиваем после preamble. Чтобы упростить — выведем здесь:
    emit_string_globals();
    return out_;
}

void Emitter::emit_preamble() {
    out_ += "target triple = \"x86_64-pc-linux-gnu\"\n\n";
    out_ += "%struct.herta_string = type { i64, ptr }\n\n";
}

void Emitter::emit_struct_typedefs() {
    for (const auto& [name, sd] : struct_defs_) {
        out_ += std::format("%struct.{} = type {{ ", name);
        for (std::size_t i = 0; i < sd.fields.size(); ++i) {
            if (i) out_ += ", ";
            out_ += llvm_type(sd.fields[i].second);
        }
        out_ += " }\n";
    }
    if (!struct_defs_.empty()) out_.push_back('\n');
}

void Emitter::emit_runtime_decls() {
    out_ +=
        "declare void @herta_print_int(i64)\n"
        "declare void @herta_print_uint(i64)\n"
        "declare void @herta_print_float(double)\n"
        "declare void @herta_print_bool(i8)\n"
        "declare void @herta_print_char(i32)\n"
        "declare void @herta_print_string(%struct.herta_string)\n"
        "declare i64 @herta_string_len(%struct.herta_string)\n"
        "declare %struct.herta_string @herta_string_concat("
            "%struct.herta_string, %struct.herta_string)\n"
        "declare i8 @herta_string_eq(%struct.herta_string, %struct.herta_string)\n"
        "declare %struct.herta_string @herta_input()\n"
        "declare void @herta_sleep_ms(i64)\n"
        "declare %struct.herta_string @herta_int_to_string(i64)\n"
        "declare %struct.herta_string @herta_float_to_string(double)\n"
        "declare i32 @herta_parse_int(%struct.herta_string, i64*)\n"
        "declare i32 @herta_parse_float(%struct.herta_string, double*)\n"
        "declare void @herta_exit(i64) noreturn\n"
        "declare void @herta_panic(%struct.herta_string) noreturn\n"
        "declare void @herta_assert_fail(i64) noreturn\n"
        "declare void @herta_rt_div_zero(i64) noreturn\n"
        "declare void @herta_rt_oob(i64, i64, i64) noreturn\n"
        "declare void @herta_rt_null_deref(i64) noreturn\n\n";
}

void Emitter::emit_string_globals() {
    // Кладём в конец — LLVM IR такое разрешает. Все литералы null-терминированы
    // (последний байт — \00), чтобы их .data можно было отдавать в C как const char*
    // без лишних конверсий.
    for (std::size_t i = 0; i < strings_.size(); ++i) {
        const auto& s = strings_[i];
        out_ += std::format("@.str.{} = private unnamed_addr constant [{} x i8] c\"",
                            i, s.size() + 1);
        for (unsigned char c : s) {
            if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7F) {
                out_ += std::format("\\{:02X}", c);
            } else {
                out_.push_back(static_cast<char>(c));
            }
        }
        out_ += "\\00\"\n";
    }
}

int Emitter::field_index(const std::string& struct_name,
                         const std::string& field) const {
    auto it = struct_defs_.find(struct_name);
    if (it == struct_defs_.end()) return -1;
    for (std::size_t i = 0; i < it->second.fields.size(); ++i) {
        if (it->second.fields[i].first == field) return static_cast<int>(i);
    }
    return -1;
}

std::string Emitter::mangle(std::size_t mod_idx, const std::string& fn_name) const {
    if (fn_name == "main") return "main";
    return modules_[mod_idx].name + "." + fn_name;
}

std::string Emitter::resolve_callee(std::size_t caller_mod,
                                    const std::string& callee,
                                    std::size_t& out_mod) const {
    // Локально в модуле вызывающего.
    if (auto it = mod_funcs_[caller_mod].find(callee);
        it != mod_funcs_[caller_mod].end()) {
        out_mod = caller_mod;
        return mangle(caller_mod, callee);
    }
    // Module.fn cross-module.
    if (auto dot = callee.find('.'); dot != std::string::npos) {
        auto head = callee.substr(0, dot);
        auto rest = callee.substr(dot + 1);
        if (auto mit = mod_idx_.find(head); mit != mod_idx_.end()) {
            if (auto it = mod_funcs_[mit->second].find(rest);
                it != mod_funcs_[mit->second].end()) {
                out_mod = mit->second;
                return mangle(mit->second, rest);
            }
        }
    }
    return {};
}

void Emitter::infer_types(FnState& st) {
    auto operand_type = [&](const ir::Operand& o) -> std::string {
        using K = ir::OperandKind;
        switch (o.kind) {
            case K::IntC:    return "int64";
            case K::FloatC:  return "float64";
            case K::BoolC:   return "bool";
            case K::CharC:   return "char";
            case K::StringC: return "string";
            case K::NullC:   return "*void";  // null приведётся к любому *T
            case K::Unit:    return "void";
            case K::Var: {
                auto it = st.var_ty.find(o.str_v);
                return it == st.var_ty.end() ? std::string{} : it->second;
            }
            case K::Temp: {
                auto it = st.temp_ty.find(o.int_v);
                return it == st.temp_ty.end() ? std::string{} : it->second;
            }
        }
        return {};
    };

    // Параметры функции.
    for (const auto& p : st.fn->params) st.var_ty[p.name] = p.type_str;

    for (const auto& ins : st.fn->instrs) {
        using IK = ir::InstrKind;
        auto set_dst = [&](const std::string& ty) {
            if (!ins.has_dst) return;
            if (ins.dst.kind == ir::OperandKind::Temp) st.temp_ty[ins.dst.int_v] = ty;
            else if (ins.dst.kind == ir::OperandKind::Var) {
                auto& cur = st.var_ty[ins.dst.str_v];
                if (cur.empty()) cur = ty;
            }
        };
        switch (ins.kind) {
            case IK::Move:
                set_dst(operand_type(ins.a));
                break;
            case IK::Un:
                set_dst(operand_type(ins.a));
                break;
            case IK::Cast:
                set_dst(ins.type_name);
                break;
            case IK::Bin: {
                using B = ir::BinOp;
                auto at = operand_type(ins.a);
                auto bt = operand_type(ins.b);
                if (ins.bin_op == B::Eq || ins.bin_op == B::NotEq
                 || ins.bin_op == B::Lt || ins.bin_op == B::Gt
                 || ins.bin_op == B::LtEq || ins.bin_op == B::GtEq) {
                    set_dst("bool");
                } else if (ins.bin_op == B::Concat) {
                    set_dst("string");
                } else {
                    set_dst(common_numeric(at, bt));
                }
                break;
            }
            case IK::Call: {
                if (ins.callee == "len") { set_dst("int64"); break; }
                if (ins.callee == "input") { set_dst("string"); break; }
                if (ins.callee == "inf" || ins.callee == "nan") { set_dst("float64"); break; }
                if (ins.callee == "int_to_string" || ins.callee == "float_to_string") {
                    set_dst("string"); break;
                }
                if (ins.callee == "parse_int") { set_dst("int64"); break; }
                if (ins.callee == "parse_float") { set_dst("float64"); break; }
                if (ins.callee == "print" || ins.callee == "exit"
                 || ins.callee == "panic" || ins.callee == "assert"
                 || ins.callee == "sleep_ms") break;
                auto caller_mod = static_cast<std::size_t>(st.mod - modules_.data());
                std::size_t target_mod = 0;
                auto canonical = resolve_callee(caller_mod, ins.callee, target_mod);
                if (canonical.empty()) break;
                std::string fn_name = ins.callee;
                if (auto dot = ins.callee.find('.'); dot != std::string::npos
                    && mod_idx_.count(ins.callee.substr(0, dot))) {
                    fn_name = ins.callee.substr(dot + 1);
                }
                auto it = mod_funcs_[target_mod].find(fn_name);
                if (it != mod_funcs_[target_mod].end())
                    set_dst(it->second->return_type);
                break;
            }
            case IK::LoadIndex: {
                auto at = operand_type(ins.a);
                auto ti = parse_type(at);
                if (ti.kind == TypeInfo::Kind::Array) set_dst(ti.elem_or_name);
                break;
            }
            case IK::LoadField: {
                auto at = operand_type(ins.a);
                auto ti = parse_type(at);
                if (ti.kind == TypeInfo::Kind::Struct) {
                    auto sit = struct_defs_.find(ti.elem_or_name);
                    if (sit != struct_defs_.end()) {
                        for (const auto& f : sit->second.fields) {
                            if (f.first == ins.field) { set_dst(f.second); break; }
                        }
                    }
                }
                break;
            }
            case IK::MakeArray:
                set_dst(std::format("[{}; {}]", ins.type_name, ins.args.size()));
                break;
            case IK::MakeStruct:
                set_dst(ins.type_name);
                break;
            // Работа с указателями
            case IK::AddressOf: {
                auto at = operand_type(ins.a);
                if (!at.empty()) set_dst("*" + at);
                break;
            }
            case IK::LoadPtr: {
                auto at = operand_type(ins.a);
                auto ti = parse_type(at);
                if (ti.kind == TypeInfo::Kind::Pointer) set_dst(ti.elem_or_name);
                break;
            }
            case IK::StorePtr: break;  // нет dst
            default: break;
        }
    }
}

std::string Emitter::fresh_ssa(FnState& st) {
    return std::format("%t{}", st.ssa_counter++);
}

void Emitter::ensure_alloca_var(FnState& st, const std::string& name,
                                const std::string& herta_ty) {
    if (st.var_addr.count(name)) return;
    auto addr = std::format("%{}.addr", name);
    st.var_addr[name] = addr;
    st.prolog += std::format("  {} = alloca {}\n", addr, llvm_type(herta_ty));
}

void Emitter::ensure_alloca_temp(FnState& st, std::int64_t id,
                                 const std::string& herta_ty) {
    if (st.temp_addr.count(id)) return;
    auto addr = std::format("%t{}.addr", id);
    st.temp_addr[id] = addr;
    st.prolog += std::format("  {} = alloca {}\n", addr, llvm_type(herta_ty));
}

void Emitter::emit_function(std::size_t mod_idx, const ir::Function& fn) {
    FnState st;
    st.mod = &modules_[mod_idx];
    st.fn = &fn;
    infer_types(st);

    // Сигнатура.
    auto canonical = mangle(mod_idx, fn.name);
    out_ += std::format("define {} @{}(", llvm_type(fn.return_type), canonical);
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out_ += ", ";
        out_ += std::format("{} %arg.{}", llvm_type(fn.params[i].type_str),
                             fn.params[i].name);
    }
    out_ += ") {\nentry:\n";

    // Allocas для параметров + store аргументов.
    for (const auto& p : fn.params) {
        auto lty = llvm_type(p.type_str);
        auto addr = std::format("%{}.addr", p.name);
        st.var_addr[p.name] = addr;
        st.prolog += std::format("  {} = alloca {}\n", addr, lty);
        st.prolog += std::format("  store {} %arg.{}, ptr {}\n", lty, p.name, addr);
    }

    // Allocas для всех Var, у которых известен тип (из инфера).
    for (const auto& [name, ty] : st.var_ty) {
        if (!st.var_addr.count(name)) ensure_alloca_var(st, name, ty);
    }
    // Allocas для всех Temp с известным типом.
    for (const auto& [id, ty] : st.temp_ty) ensure_alloca_temp(st, id, ty);

    // Тело: эмиссия инструкций.
    for (const auto& ins : fn.instrs) emit_instr(st, ins);

    out_ += st.prolog;
    out_ += st.body;
    // Если тело не закрыто терминатором (например, последняя инструкция —
    // Label из лоуэрера для пустого end-блока), нужно его закрыть.
    if (!st.terminated) {
        auto rt = parse_type(fn.return_type);
        if (rt.kind == TypeInfo::Kind::Void) out_ += "  ret void\n";
        else out_ += "  unreachable\n";
    }
    out_ += "}\n\n";
}

// eval_operand / store_to / widen

std::pair<std::string, std::string>
Emitter::eval_operand(FnState& st, const ir::Operand& o) {
    using K = ir::OperandKind;
    switch (o.kind) {
        case K::NullC:   return { "null", "*void" };
        case K::IntC:    return { std::to_string(o.int_v), "int64" };
        case K::FloatC: {
            // LLVM требует точное hex-представление для double, иначе теряем биты.
            std::uint64_t bits;
            double v = o.float_v;
            std::memcpy(&bits, &v, sizeof(bits));
            return { std::format("0x{:016X}", bits), "float64" };
        }
        case K::BoolC:   return { o.bool_v ? "true" : "false", "bool" };
        case K::CharC:   return { std::to_string(o.char_v), "char" };
        case K::Unit:    return { "undef", "void" };
        case K::StringC: {
            auto it = string_idx_.find(o.str_v);
            std::size_t idx;
            if (it == string_idx_.end()) {
                idx = strings_.size();
                strings_.push_back(o.str_v);
                string_idx_[o.str_v] = idx;
            } else {
                idx = it->second;
            }
            auto agg1 = fresh_ssa(st);
            auto agg2 = fresh_ssa(st);
            st.body += std::format(
                "  {} = insertvalue %struct.herta_string undef, i64 {}, 0\n",
                agg1, o.str_v.size());
            st.body += std::format(
                "  {} = insertvalue %struct.herta_string {}, ptr @.str.{}, 1\n",
                agg2, agg1, idx);
            return { agg2, "string" };
        }
        case K::Var: {
            auto ty = st.var_ty.at(o.str_v);
            auto addr = st.var_addr.at(o.str_v);
            auto v = fresh_ssa(st);
            st.body += std::format("  {} = load {}, ptr {}\n", v, llvm_type(ty), addr);
            return { v, ty };
        }
        case K::Temp: {
            auto ty = st.temp_ty.at(o.int_v);
            auto addr = st.temp_addr.at(o.int_v);
            auto v = fresh_ssa(st);
            st.body += std::format("  {} = load {}, ptr {}\n", v, llvm_type(ty), addr);
            return { v, ty };
        }
    }
    return { "undef", "void" };
}

void Emitter::store_to(FnState& st, const ir::Operand& dst,
                       const std::string& value, const std::string& herta_ty) {
    using K = ir::OperandKind;
    if (dst.kind == K::Var) {
        auto& vty = st.var_ty[dst.str_v];
        if (vty.empty()) vty = herta_ty;
        if (!st.var_addr.count(dst.str_v)) ensure_alloca_var(st, dst.str_v, vty);
        st.body += std::format("  store {} {}, ptr {}\n",
                                llvm_type(vty), value, st.var_addr.at(dst.str_v));
    } else if (dst.kind == K::Temp) {
        auto& tty = st.temp_ty[dst.int_v];
        if (tty.empty()) tty = herta_ty;
        if (!st.temp_addr.count(dst.int_v)) ensure_alloca_temp(st, dst.int_v, tty);
        st.body += std::format("  store {} {}, ptr {}\n",
                                llvm_type(tty), value, st.temp_addr.at(dst.int_v));
    }
}

// Приводит value (типа from_ty) к to_ty. Поддерживает sext/zext/trunc для целых,
// sitofp/uitofp/fpext/fptrunc для float, и char ↔ int как i32 без преобразования.
std::string Emitter::widen(FnState& st, const std::string& value,
                           const std::string& from_ty, const std::string& to_ty) {
    if (from_ty == to_ty) return value;
    auto a = parse_type(from_ty);
    auto b = parse_type(to_ty);

    // Указатель в указатель: в LLVM это opaque ptr, никаких инструкций не нужно.
    if (a.kind == TypeInfo::Kind::Pointer && b.kind == TypeInfo::Kind::Pointer) {
        return value;
    }

    auto la = llvm_type(a);
    auto lb = llvm_type(b);
    if (la == lb) return value;

    auto v = fresh_ssa(st);
    if (is_int_or_char(a) && is_int_or_char(b)) {
        auto width_of = [](const TypeInfo& t) {
            switch (t.kind) {
                case TypeInfo::Kind::Char: return 32;
                case TypeInfo::Kind::Byte: return 8;
                case TypeInfo::Kind::Bool: return 1;
                default: return t.bits ? t.bits : 1;
            }
        };
        int abits = width_of(a);
        int bbits = width_of(b);
        if (bbits > abits) {
            bool sext = is_signed_int_kind(a.kind);
            st.body += std::format("  {} = {} {} {} to {}\n", v,
                                    sext ? "sext" : "zext", la, value, lb);
        } else if (bbits < abits) {
            st.body += std::format("  {} = trunc {} {} to {}\n", v, la, value, lb);
        } else {
            return value;  // bitwidth тот же, разные kind (uint32 ↔ int32) — bit-identical
        }
        return v;
    }
    if (is_int_or_char(a) && is_float(b)) {
        bool sext = is_signed_int_kind(a.kind);
        st.body += std::format("  {} = {} {} {} to {}\n", v,
                                sext ? "sitofp" : "uitofp", la, value, lb);
        return v;
    }
    if (is_float(a) && is_int_or_char(b)) {
        bool to_signed = is_signed_int_kind(b.kind) || b.kind == TypeInfo::Kind::Char;
        st.body += std::format("  {} = {} {} {} to {}\n", v,
                                to_signed ? "fptosi" : "fptoui", la, value, lb);
        return v;
    }
    if (is_float(a) && is_float(b)) {
        st.body += std::format("  {} = {} {} {} to {}\n", v,
                                a.bits < b.bits ? "fpext" : "fptrunc",
                                la, value, lb);
        return v;
    }
    return value;
}

void Emitter::ensure_block(FnState& st) {
    if (!st.terminated) return;
    st.body += std::format("dead_{}:\n", st.ssa_counter++);
    st.terminated = false;
}

void Emitter::emit_instr(FnState& st, const ir::Instr& ins) {
    using IK = ir::InstrKind;
    if (ins.kind == IK::Label) {
        // Метка сама по себе закрывает предыдущий блок (если он был terminate'нут)
        // и открывает новый. Если предыдущий не закрыт — нужно явно проложить br.
        if (!st.terminated) st.body += std::format("  br label %L_{}\n", ins.label);
        st.body += std::format("L_{}:\n", ins.label);
        st.terminated = false;
        return;
    }
    ensure_block(st);
    switch (ins.kind) {
        case IK::Label:
            return;  // обработано выше
        case IK::Goto:
            st.body += std::format("  br label %L_{}\n", ins.label);
            st.terminated = true;
            return;
        case IK::Branch: {
            auto [c, _] = eval_operand(st, ins.a);
            st.body += std::format("  br i1 {}, label %L_{}, label %L_{}\n",
                                    c, ins.label_then, ins.label_else);
            st.terminated = true;
            return;
        }
        case IK::Return: {
            auto rt = parse_type(st.fn->return_type);
            if (ins.has_ret_value && rt.kind != TypeInfo::Kind::Void) {
                auto [v, vty] = eval_operand(st, ins.a);
                auto vc = widen(st, v, vty, st.fn->return_type);
                st.body += std::format("  ret {} {}\n", llvm_type(rt), vc);
            } else {
                st.body += "  ret void\n";
            }
            st.terminated = true;
            return;
        }
        case IK::Move: {
            auto [v, vty] = eval_operand(st, ins.a);
            // Целевой тип для приведения — известный (по infer'у); иначе оставляем как есть.
            std::string dst_ty;
            if (ins.dst.kind == ir::OperandKind::Var)
                dst_ty = st.var_ty[ins.dst.str_v];
            else if (ins.dst.kind == ir::OperandKind::Temp)
                dst_ty = st.temp_ty[ins.dst.int_v];
            if (dst_ty.empty()) dst_ty = vty;
            auto cv = widen(st, v, vty, dst_ty);
            store_to(st, ins.dst, cv, dst_ty);
            return;
        }
        case IK::Un: {
            auto [v, vty] = eval_operand(st, ins.a);
            auto r = fresh_ssa(st);
            if (ins.un_op == ir::UnOp::Neg) {
                auto t = parse_type(vty);
                if (is_float(t)) {
                    st.body += std::format("  {} = fneg {} {}\n", r, llvm_type(t), v);
                } else {
                    st.body += std::format("  {} = sub {} 0, {}\n", r, llvm_type(t), v);
                }
            } else {  // Not
                st.body += std::format("  {} = xor i1 {}, true\n", r, v);
            }
            store_to(st, ins.dst, r, vty);
            return;
        }
        case IK::Bin:        emit_bin(st, ins); return;
        case IK::Cast:       emit_cast(st, ins); return;
        case IK::Call:       emit_call(st, ins); return;
        case IK::LoadIndex:  emit_load_index(st, ins); return;
        case IK::StoreIndex: emit_store_index(st, ins); return;
        case IK::LoadField: {
            auto [v, vty] = eval_operand(st, ins.a);
            auto ti = parse_type(vty);
            int fidx = field_index(ti.elem_or_name, ins.field);
            auto agg_alloca = fresh_ssa(st);
            // Поместить агрегатную копию в новую alloca, чтобы GEP'нуть в поле.
            st.prolog += std::format("  {} = alloca {}\n", agg_alloca, llvm_type(vty));
            st.body += std::format("  store {} {}, ptr {}\n", llvm_type(vty), v, agg_alloca);
            auto gep = fresh_ssa(st);
            st.body += std::format("  {} = getelementptr {}, ptr {}, i32 0, i32 {}\n",
                                    gep, llvm_type(vty), agg_alloca, fidx);
            auto fty = struct_defs_.at(ti.elem_or_name).fields[fidx].second;
            auto val = fresh_ssa(st);
            st.body += std::format("  {} = load {}, ptr {}\n", val, llvm_type(fty), gep);
            store_to(st, ins.dst, val, fty);
            return;
        }
        case IK::StoreField: {
            // ins.a — база (lvalue), ins.value_to_store — записываемое.
            // База должна быть Var или Temp (lvalue). Берём её alloca напрямую.
            std::string base_addr;
            std::string base_ty;
            if (ins.a.kind == ir::OperandKind::Var) {
                base_addr = st.var_addr.at(ins.a.str_v);
                base_ty = st.var_ty.at(ins.a.str_v);
            } else {
                base_addr = st.temp_addr.at(ins.a.int_v);
                base_ty = st.temp_ty.at(ins.a.int_v);
            }
            auto ti = parse_type(base_ty);
            int fidx = field_index(ti.elem_or_name, ins.field);
            auto fty = struct_defs_.at(ti.elem_or_name).fields[fidx].second;
            auto [v, vty] = eval_operand(st, ins.value_to_store);
            auto cv = widen(st, v, vty, fty);
            auto gep = fresh_ssa(st);
            st.body += std::format("  {} = getelementptr {}, ptr {}, i32 0, i32 {}\n",
                                    gep, llvm_type(base_ty), base_addr, fidx);
            st.body += std::format("  store {} {}, ptr {}\n", llvm_type(fty), cv, gep);
            return;
        }
        case IK::MakeArray:  emit_make_array(st, ins); return;
        case IK::MakeStruct: emit_make_struct(st, ins); return;
        // Работа с указателями
        case IK::AddressOf: {
            // ins.a — Var/Temp; их адрес alloca и есть указатель.
            std::string addr;
            if (ins.a.kind == ir::OperandKind::Var) addr = st.var_addr.at(ins.a.str_v);
            else                                    addr = st.temp_addr.at(ins.a.int_v);
            store_to(st, ins.dst, addr, st.temp_ty[ins.dst.int_v]);
            return;
        }
        case IK::LoadPtr: {
            auto [p, pty] = eval_operand(st, ins.a);
            // Тип результата = pointee, определён infer'ом.
            std::string dst_ty;
            if (ins.dst.kind == ir::OperandKind::Var) dst_ty = st.var_ty[ins.dst.str_v];
            else                                       dst_ty = st.temp_ty[ins.dst.int_v];
            // null-check.
            auto null_cmp = fresh_ssa(st);
            st.body += std::format("  {} = icmp eq ptr {}, null\n", null_cmp, p);
            auto ok = std::format("nullok_{}", st.ssa_counter++);
            auto bad = std::format("nullbad_{}", st.ssa_counter++);
            st.body += std::format("  br i1 {}, label %{}, label %{}\n", null_cmp, bad, ok);
            st.body += std::format("{}:\n", bad);
            st.body += std::format("  call void @herta_rt_null_deref(i64 {})\n", ins.loc.line);
            st.body += "  unreachable\n";
            st.body += std::format("{}:\n", ok);
            auto v = fresh_ssa(st);
            st.body += std::format("  {} = load {}, ptr {}\n", v, llvm_type(dst_ty), p);
            store_to(st, ins.dst, v, dst_ty);
            return;
        }
        case IK::StorePtr: {
            auto [p, pty] = eval_operand(st, ins.a);
            auto [v, vty] = eval_operand(st, ins.value_to_store);
            // Тип записи определяется pointee, иначе IntC литерал (int64)
            // переедет 8 байт в i32-слот и затрёт соседнюю стэк-память.
            auto pt = parse_type(pty);
            auto dst_ty = pt.kind == TypeInfo::Kind::Pointer ? pt.elem_or_name : vty;
            auto vw = widen(st, v, vty, dst_ty);
            // null-check.
            auto null_cmp = fresh_ssa(st);
            st.body += std::format("  {} = icmp eq ptr {}, null\n", null_cmp, p);
            auto ok = std::format("snullok_{}", st.ssa_counter++);
            auto bad = std::format("snullbad_{}", st.ssa_counter++);
            st.body += std::format("  br i1 {}, label %{}, label %{}\n", null_cmp, bad, ok);
            st.body += std::format("{}:\n", bad);
            st.body += std::format("  call void @herta_rt_null_deref(i64 {})\n", ins.loc.line);
            st.body += "  unreachable\n";
            st.body += std::format("{}:\n", ok);
            st.body += std::format("  store {} {}, ptr {}\n", llvm_type(dst_ty), vw, p);
            return;
        }
    }
}

void Emitter::emit_bin(FnState& st, const ir::Instr& ins) {
    using B = ir::BinOp;
    auto [av, at] = eval_operand(st, ins.a);
    auto [bv, bt] = eval_operand(st, ins.b);

    // Сравнения строк/массивов/структур и Concat.
    if (ins.bin_op == B::Concat) {
        auto r = fresh_ssa(st);
        st.body += std::format(
            "  {} = call %struct.herta_string @herta_string_concat("
            "%struct.herta_string {}, %struct.herta_string {})\n", r, av, bv);
        store_to(st, ins.dst, r, "string");
        return;
    }
    if ((ins.bin_op == B::Eq || ins.bin_op == B::NotEq)
        && (at == "string")) {
        auto r8 = fresh_ssa(st);
        st.body += std::format(
            "  {} = call i8 @herta_string_eq("
            "%struct.herta_string {}, %struct.herta_string {})\n", r8, av, bv);
        auto r1 = fresh_ssa(st);
        st.body += std::format("  {} = trunc i8 {} to i1\n", r1, r8);
        if (ins.bin_op == B::NotEq) {
            auto r = fresh_ssa(st);
            st.body += std::format("  {} = xor i1 {}, true\n", r, r1);
            r1 = r;
        }
        store_to(st, ins.dst, r1, "bool");
        return;
    }
    // Указатели: == и != (включая сравнение с null).
    auto ati = parse_type(at);
    auto bti = parse_type(bt);
    if ((ins.bin_op == B::Eq || ins.bin_op == B::NotEq)
        && (ati.kind == TypeInfo::Kind::Pointer
         || bti.kind == TypeInfo::Kind::Pointer)) {
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = icmp {} ptr {}, {}\n", r,
                                ins.bin_op == B::Eq ? "eq" : "ne", av, bv);
        store_to(st, ins.dst, r, "bool");
        return;
    }
    // byte: == и != (тип не арифметический, прочее семантика запретила).
    if (at == "byte" && bt == "byte") {
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = icmp {} i8 {}, {}\n", r,
                                ins.bin_op == B::Eq ? "eq" : "ne", av, bv);
        store_to(st, ins.dst, r, "bool");
        return;
    }
    // Array/Struct == / != — поэлементно через цепочку AND'ов.
    if ((ins.bin_op == B::Eq || ins.bin_op == B::NotEq)
        && (ati.kind == TypeInfo::Kind::Array
         || ati.kind == TypeInfo::Kind::Struct)) {
        // Положим обе агрегаты в alloca и пройдёмся по элементам/полям.
        auto a_alloca = fresh_ssa(st);
        auto b_alloca = fresh_ssa(st);
        st.prolog += std::format("  {} = alloca {}\n", a_alloca, llvm_type(at));
        st.prolog += std::format("  {} = alloca {}\n", b_alloca, llvm_type(at));
        st.body += std::format("  store {} {}, ptr {}\n", llvm_type(at), av, a_alloca);
        st.body += std::format("  store {} {}, ptr {}\n", llvm_type(at), bv, b_alloca);

        std::vector<std::pair<std::string, int>> field_specs;  // (herta_ty, idx)
        if (ati.kind == TypeInfo::Kind::Array) {
            for (int i = 0; i < ati.array_size; ++i)
                field_specs.emplace_back(ati.elem_or_name, i);
        } else {
            const auto& sd = struct_defs_.at(ati.elem_or_name);
            for (std::size_t i = 0; i < sd.fields.size(); ++i)
                field_specs.emplace_back(sd.fields[i].second, static_cast<int>(i));
        }

        std::string acc;  // i1 SSA value представляющий накопленное "все равны"
        for (const auto& [fty, idx] : field_specs) {
            auto ga = fresh_ssa(st);
            auto gb = fresh_ssa(st);
            st.body += std::format("  {} = getelementptr {}, ptr {}, i32 0, i32 {}\n",
                                    ga, llvm_type(at), a_alloca, idx);
            st.body += std::format("  {} = getelementptr {}, ptr {}, i32 0, i32 {}\n",
                                    gb, llvm_type(at), b_alloca, idx);
            auto la = fresh_ssa(st);
            auto lb = fresh_ssa(st);
            st.body += std::format("  {} = load {}, ptr {}\n", la, llvm_type(fty), ga);
            st.body += std::format("  {} = load {}, ptr {}\n", lb, llvm_type(fty), gb);
            auto eq = fresh_ssa(st);
            auto fti = parse_type(fty);
            if (is_float(fti)) {
                st.body += std::format("  {} = fcmp oeq {} {}, {}\n",
                                        eq, llvm_type(fti), la, lb);
            } else if (fti.kind == TypeInfo::Kind::String) {
                auto i8r = fresh_ssa(st);
                st.body += std::format(
                    "  {} = call i8 @herta_string_eq("
                    "%struct.herta_string {}, %struct.herta_string {})\n",
                    i8r, la, lb);
                st.body += std::format("  {} = trunc i8 {} to i1\n", eq, i8r);
            } else {
                st.body += std::format("  {} = icmp eq {} {}, {}\n",
                                        eq, llvm_type(fti), la, lb);
            }
            if (acc.empty()) {
                acc = eq;
            } else {
                auto na = fresh_ssa(st);
                st.body += std::format("  {} = and i1 {}, {}\n", na, acc, eq);
                acc = na;
            }
        }
        if (acc.empty()) acc = "true";  // пустой агрегат — равны
        if (ins.bin_op == B::NotEq) {
            auto neg = fresh_ssa(st);
            st.body += std::format("  {} = xor i1 {}, true\n", neg, acc);
            acc = neg;
        }
        store_to(st, ins.dst, acc, "bool");
        return;
    }

    // Сравнения char (==,!=,<,>,<=,>=).
    if (at == "char" && bt == "char") {
        const char* pred;
        switch (ins.bin_op) {
            case B::Eq: pred = "eq"; break;
            case B::NotEq: pred = "ne"; break;
            case B::Lt: pred = "ult"; break;
            case B::Gt: pred = "ugt"; break;
            case B::LtEq: pred = "ule"; break;
            case B::GtEq: pred = "uge"; break;
            default: pred = "eq"; break;
        }
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = icmp {} i32 {}, {}\n", r, pred, av, bv);
        store_to(st, ins.dst, r, "bool");
        return;
    }
    if (at == "bool" && bt == "bool"
        && (ins.bin_op == B::Eq || ins.bin_op == B::NotEq)) {
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = icmp {} i1 {}, {}\n", r,
                                ins.bin_op == B::Eq ? "eq" : "ne", av, bv);
        store_to(st, ins.dst, r, "bool");
        return;
    }

    // Числовые операции: arithmetic + comparisons.
    auto common = common_numeric(at, bt);
    auto ct = parse_type(common);
    auto avc = widen(st, av, at, common);
    auto bvc = widen(st, bv, bt, common);
    auto lty = llvm_type(ct);

    auto is_cmp = [&] {
        return ins.bin_op == B::Eq || ins.bin_op == B::NotEq
            || ins.bin_op == B::Lt || ins.bin_op == B::Gt
            || ins.bin_op == B::LtEq || ins.bin_op == B::GtEq;
    }();

    auto r = fresh_ssa(st);
    if (is_cmp) {
        const char* pred;
        if (is_float(ct)) {
            switch (ins.bin_op) {
                case B::Eq: pred = "oeq"; break;
                case B::NotEq: pred = "one"; break;
                case B::Lt: pred = "olt"; break;
                case B::Gt: pred = "ogt"; break;
                case B::LtEq: pred = "ole"; break;
                case B::GtEq: pred = "oge"; break;
                default: pred = "oeq"; break;
            }
            st.body += std::format("  {} = fcmp {} {} {}, {}\n",
                                    r, pred, lty, avc, bvc);
        } else {
            bool s = is_signed_int_kind(ct.kind);
            switch (ins.bin_op) {
                case B::Eq: pred = "eq"; break;
                case B::NotEq: pred = "ne"; break;
                case B::Lt: pred = s ? "slt" : "ult"; break;
                case B::Gt: pred = s ? "sgt" : "ugt"; break;
                case B::LtEq: pred = s ? "sle" : "ule"; break;
                case B::GtEq: pred = s ? "sge" : "uge"; break;
                default: pred = "eq"; break;
            }
            st.body += std::format("  {} = icmp {} {} {}, {}\n",
                                    r, pred, lty, avc, bvc);
        }
        store_to(st, ins.dst, r, "bool");
        return;
    }

    // Арифметика.
    if (is_float(ct)) {
        const char* op;
        switch (ins.bin_op) {
            case B::Add: op = "fadd"; break;
            case B::Sub: op = "fsub"; break;
            case B::Mul: op = "fmul"; break;
            case B::Div: op = "fdiv"; break;
            default: op = "fadd"; break;
        }
        st.body += std::format("  {} = {} {} {}, {}\n", r, op, lty, avc, bvc);
        store_to(st, ins.dst, r, common);
        return;
    }
    // Целые. Для Div/Mod — runtime-проверка деления на ноль.
    if (ins.bin_op == ir::BinOp::Div || ins.bin_op == ir::BinOp::Mod) {
        auto zcmp = fresh_ssa(st);
        st.body += std::format("  {} = icmp eq {} {}, 0\n", zcmp, lty, bvc);
        auto ok_lbl = std::format("div_ok_{}", st.ssa_counter++);
        auto bad_lbl = std::format("div_bad_{}", st.ssa_counter++);
        st.body += std::format("  br i1 {}, label %{}, label %{}\n",
                                zcmp, bad_lbl, ok_lbl);
        st.body += std::format("{}:\n", bad_lbl);
        st.body += std::format("  call void @herta_rt_div_zero(i64 {})\n", ins.loc.line);
        st.body += "  unreachable\n";
        st.body += std::format("{}:\n", ok_lbl);
    }
    bool s = is_signed_int_kind(ct.kind);
    const char* op;
    switch (ins.bin_op) {
        case B::Add: op = "add"; break;
        case B::Sub: op = "sub"; break;
        case B::Mul: op = "mul"; break;
        case B::Div: op = s ? "sdiv" : "udiv"; break;
        case B::Mod: op = s ? "srem" : "urem"; break;
        default: op = "add"; break;
    }
    st.body += std::format("  {} = {} {} {}, {}\n", r, op, lty, avc, bvc);
    store_to(st, ins.dst, r, common);
}

void Emitter::emit_cast(FnState& st, const ir::Instr& ins) {
    auto [v, vty] = eval_operand(st, ins.a);
    auto cv = widen(st, v, vty, ins.type_name);
    store_to(st, ins.dst, cv, ins.type_name);
}

void Emitter::emit_call(FnState& st, const ir::Instr& ins) {
    // Аргументы.
    std::vector<std::pair<std::string, std::string>> args;
    args.reserve(ins.args.size());
    for (const auto& a : ins.args) args.push_back(eval_operand(st, a));

    auto& name = ins.callee;

    // Builtin'ы.
    if (name == "print") {
        // ровно один аргумент в семантике.
        auto [v, ty] = args[0];
        auto ti = parse_type(ty);
        if (is_float(ti)) {
            auto vw = widen(st, v, ty, "float64");
            st.body += std::format("  call void @herta_print_float(double {})\n", vw);
        } else if (ti.kind == TypeInfo::Kind::Bool) {
            auto ext = fresh_ssa(st);
            st.body += std::format("  {} = zext i1 {} to i8\n", ext, v);
            st.body += std::format("  call void @herta_print_bool(i8 {})\n", ext);
        } else if (ti.kind == TypeInfo::Kind::Char) {
            st.body += std::format("  call void @herta_print_char(i32 {})\n", v);
        } else if (ti.kind == TypeInfo::Kind::String) {
            st.body += std::format(
                "  call void @herta_print_string(%struct.herta_string {})\n", v);
        } else {
            // int/uint — приведём к i64 с правильным знаком.
            auto vw = widen(st, v, ty, ti.kind == TypeInfo::Kind::UInt ? "uint64" : "int64");
            const char* fn = ti.kind == TypeInfo::Kind::UInt
                ? "herta_print_uint" : "herta_print_int";
            st.body += std::format("  call void @{}(i64 {})\n", fn, vw);
        }
        return;
    }
    if (name == "input") {
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = call %struct.herta_string @herta_input()\n", r);
        if (ins.has_dst) store_to(st, ins.dst, r, "string");
        return;
    }
    if (name == "sleep_ms") {
        auto [v, ty] = args[0];
        auto vw = widen(st, v, ty, "int64");
        st.body += std::format("  call void @herta_sleep_ms(i64 {})\n", vw);
        return;
    }
    if (name == "exit") {
        auto [v, ty] = args[0];
        auto vw = widen(st, v, ty, "int64");
        st.body += std::format("  call void @herta_exit(i64 {})\n", vw);
        st.body += "  unreachable\n";
        st.terminated = true;
        return;
    }
    if (name == "panic") {
        auto [v, ty] = args[0];
        st.body += std::format("  call void @herta_panic(%struct.herta_string {})\n", v);
        st.body += "  unreachable\n";
        st.terminated = true;
        return;
    }
    if (name == "assert") {
        auto [v, ty] = args[0];
        auto ok_lbl = std::format("assert_ok_{}", st.ssa_counter++);
        auto fail_lbl = std::format("assert_fail_{}", st.ssa_counter++);
        st.body += std::format("  br i1 {}, label %{}, label %{}\n",
                                v, ok_lbl, fail_lbl);
        st.body += std::format("{}:\n", fail_lbl);
        st.body += std::format("  call void @herta_assert_fail(i64 {})\n", ins.loc.line);
        st.body += "  unreachable\n";
        st.body += std::format("{}:\n", ok_lbl);
        return;
    }
    if (name == "len") {
        auto [v, ty] = args[0];
        auto ti = parse_type(ty);
        if (ti.kind == TypeInfo::Kind::String) {
            auto r = fresh_ssa(st);
            st.body += std::format(
                "  {} = call i64 @herta_string_len(%struct.herta_string {})\n", r, v);
            if (ins.has_dst) store_to(st, ins.dst, r, "int64");
        } else if (ti.kind == TypeInfo::Kind::Array) {
            if (ins.has_dst)
                store_to(st, ins.dst, std::to_string(ti.array_size), "int64");
        }
        return;
    }
    if (name == "inf") {
        if (ins.has_dst) store_to(st, ins.dst, "0x7FF0000000000000", "float64");
        return;
    }
    if (name == "nan") {
        if (ins.has_dst) store_to(st, ins.dst, "0x7FF8000000000000", "float64");
        return;
    }
    if (name == "int_to_string") {
        auto [v, ty] = args[0];
        auto vw = widen(st, v, ty, "int64");
        auto r = fresh_ssa(st);
        st.body += std::format(
            "  {} = call %struct.herta_string @herta_int_to_string(i64 {})\n", r, vw);
        if (ins.has_dst) store_to(st, ins.dst, r, "string");
        return;
    }
    if (name == "float_to_string") {
        auto [v, ty] = args[0];
        auto vw = widen(st, v, ty, "float64");
        auto r = fresh_ssa(st);
        st.body += std::format(
            "  {} = call %struct.herta_string @herta_float_to_string(double {})\n", r, vw);
        if (ins.has_dst) store_to(st, ins.dst, r, "string");
        return;
    }
    if (name == "parse_int") {
        // На неуспехе оставим 0; runtime возвращает 1/0, мы игнорируем.
        auto [v, ty] = args[0];
        auto slot = fresh_ssa(st);
        st.body += std::format("  {} = alloca i64\n", slot);
        st.body += std::format("  store i64 0, i64* {}\n", slot);
        auto ignored = fresh_ssa(st);
        st.body += std::format(
            "  {} = call i32 @herta_parse_int(%struct.herta_string {}, i64* {})\n",
            ignored, v, slot);
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = load i64, i64* {}\n", r, slot);
        if (ins.has_dst) store_to(st, ins.dst, r, "int64");
        return;
    }
    if (name == "parse_float") {
        auto [v, ty] = args[0];
        auto slot = fresh_ssa(st);
        st.body += std::format("  {} = alloca double\n", slot);
        st.body += std::format("  store double 0.0, double* {}\n", slot);
        auto ignored = fresh_ssa(st);
        st.body += std::format(
            "  {} = call i32 @herta_parse_float(%struct.herta_string {}, double* {})\n",
            ignored, v, slot);
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = load double, double* {}\n", r, slot);
        if (ins.has_dst) store_to(st, ins.dst, r, "float64");
        return;
    }

    // Обычный вызов.
    std::size_t target_mod = 0;
    auto caller_mod = static_cast<std::size_t>(st.mod - modules_.data());
    auto canonical = resolve_callee(caller_mod, name, target_mod);
    const auto* tfn = mod_funcs_[target_mod][
        name.find('.') == std::string::npos ? name
            : (mod_idx_.count(name.substr(0, name.find('.')))
                ? name.substr(name.find('.') + 1) : name)];
    auto ret_ty = tfn->return_type;
    auto ret_li = llvm_type(ret_ty);

    // Подготовить строку аргументов с приведением к ожидаемым типам параметра.
    std::string call_args;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) call_args += ", ";
        auto want = tfn->params[i].type_str;
        auto cv = widen(st, args[i].first, args[i].second, want);
        call_args += std::format("{} {}", llvm_type(want), cv);
    }

    if (ret_li == "void") {
        st.body += std::format("  call void @{}({})\n", canonical, call_args);
    } else {
        auto r = fresh_ssa(st);
        st.body += std::format("  {} = call {} @{}({})\n",
                                r, ret_li, canonical, call_args);
        if (ins.has_dst) store_to(st, ins.dst, r, ret_ty);
    }
}

void Emitter::emit_load_index(FnState& st, const ir::Instr& ins) {
    // База — Var или Temp с типом [T; N].
    std::string base_addr, base_ty;
    if (ins.a.kind == ir::OperandKind::Var) {
        base_addr = st.var_addr.at(ins.a.str_v);
        base_ty = st.var_ty.at(ins.a.str_v);
    } else {
        base_addr = st.temp_addr.at(ins.a.int_v);
        base_ty = st.temp_ty.at(ins.a.int_v);
    }
    auto ti = parse_type(base_ty);
    auto elem_ty = ti.elem_or_name;
    auto [iv, ity] = eval_operand(st, ins.b);
    // Привести индекс к i64 для GEP и bounds check.
    auto iw = widen(st, iv, ity, "int64");

    // Bounds check.
    auto cmp_lo = fresh_ssa(st);
    auto cmp_hi = fresh_ssa(st);
    auto bad = fresh_ssa(st);
    st.body += std::format("  {} = icmp slt i64 {}, 0\n", cmp_lo, iw);
    st.body += std::format("  {} = icmp sge i64 {}, {}\n", cmp_hi, iw, ti.array_size);
    st.body += std::format("  {} = or i1 {}, {}\n", bad, cmp_lo, cmp_hi);
    auto ok_lbl = std::format("idx_ok_{}", st.ssa_counter++);
    auto bad_lbl = std::format("idx_bad_{}", st.ssa_counter++);
    st.body += std::format("  br i1 {}, label %{}, label %{}\n", bad, bad_lbl, ok_lbl);
    st.body += std::format("{}:\n", bad_lbl);
    st.body += std::format("  call void @herta_rt_oob(i64 {}, i64 {}, i64 {})\n",
                            iw, ti.array_size, ins.loc.line);
    st.body += "  unreachable\n";
    st.body += std::format("{}:\n", ok_lbl);

    auto gep = fresh_ssa(st);
    st.body += std::format("  {} = getelementptr {}, ptr {}, i64 0, i64 {}\n",
                            gep, llvm_type(base_ty), base_addr, iw);
    auto val = fresh_ssa(st);
    st.body += std::format("  {} = load {}, ptr {}\n", val, llvm_type(elem_ty), gep);
    store_to(st, ins.dst, val, elem_ty);
}

void Emitter::emit_store_index(FnState& st, const ir::Instr& ins) {
    std::string base_addr, base_ty;
    if (ins.a.kind == ir::OperandKind::Var) {
        base_addr = st.var_addr.at(ins.a.str_v);
        base_ty = st.var_ty.at(ins.a.str_v);
    } else {
        base_addr = st.temp_addr.at(ins.a.int_v);
        base_ty = st.temp_ty.at(ins.a.int_v);
    }
    auto ti = parse_type(base_ty);
    auto elem_ty = ti.elem_or_name;
    auto [iv, ity] = eval_operand(st, ins.b);
    auto iw = widen(st, iv, ity, "int64");

    // Bounds check.
    auto cmp_lo = fresh_ssa(st);
    auto cmp_hi = fresh_ssa(st);
    auto bad = fresh_ssa(st);
    st.body += std::format("  {} = icmp slt i64 {}, 0\n", cmp_lo, iw);
    st.body += std::format("  {} = icmp sge i64 {}, {}\n", cmp_hi, iw, ti.array_size);
    st.body += std::format("  {} = or i1 {}, {}\n", bad, cmp_lo, cmp_hi);
    auto ok_lbl = std::format("sidx_ok_{}", st.ssa_counter++);
    auto bad_lbl = std::format("sidx_bad_{}", st.ssa_counter++);
    st.body += std::format("  br i1 {}, label %{}, label %{}\n", bad, bad_lbl, ok_lbl);
    st.body += std::format("{}:\n", bad_lbl);
    st.body += std::format("  call void @herta_rt_oob(i64 {}, i64 {}, i64 {})\n",
                            iw, ti.array_size, ins.loc.line);
    st.body += "  unreachable\n";
    st.body += std::format("{}:\n", ok_lbl);

    auto [vv, vty] = eval_operand(st, ins.value_to_store);
    auto cv = widen(st, vv, vty, elem_ty);
    auto gep = fresh_ssa(st);
    st.body += std::format("  {} = getelementptr {}, ptr {}, i64 0, i64 {}\n",
                            gep, llvm_type(base_ty), base_addr, iw);
    st.body += std::format("  store {} {}, ptr {}\n", llvm_type(elem_ty), cv, gep);
}

void Emitter::emit_make_array(FnState& st, const ir::Instr& ins) {
    // Тип элементов в ins.type_name, размер = ins.args.size().
    auto arr_ty = std::format("[{}; {}]", ins.type_name, ins.args.size());
    // Целевой alloca уже создан в infer (по dst type). Запишем поэлементно.
    std::string dst_addr;
    if (ins.dst.kind == ir::OperandKind::Var)
        dst_addr = st.var_addr.at(ins.dst.str_v);
    else
        dst_addr = st.temp_addr.at(ins.dst.int_v);
    auto lty = llvm_type(arr_ty);
    for (std::size_t i = 0; i < ins.args.size(); ++i) {
        auto [v, vty] = eval_operand(st, ins.args[i]);
        auto cv = widen(st, v, vty, ins.type_name);
        auto gep = fresh_ssa(st);
        st.body += std::format("  {} = getelementptr {}, ptr {}, i64 0, i64 {}\n",
                                gep, lty, dst_addr, i);
        st.body += std::format("  store {} {}, ptr {}\n",
                                llvm_type(ins.type_name), cv, gep);
    }
}

void Emitter::emit_make_struct(FnState& st, const ir::Instr& ins) {
    std::string dst_addr;
    if (ins.dst.kind == ir::OperandKind::Var)
        dst_addr = st.var_addr.at(ins.dst.str_v);
    else
        dst_addr = st.temp_addr.at(ins.dst.int_v);
    auto lty = llvm_type(ins.type_name);
    const auto& sd = struct_defs_.at(ins.type_name);
    for (std::size_t i = 0; i < ins.args.size(); ++i) {
        // ins.struct_field_names[i] — имя поля; найдём индекс в декларации.
        int idx = field_index(ins.type_name, ins.struct_field_names[i]);
        auto fty = sd.fields[idx].second;
        auto [v, vty] = eval_operand(st, ins.args[i]);
        auto cv = widen(st, v, vty, fty);
        auto gep = fresh_ssa(st);
        st.body += std::format("  {} = getelementptr {}, ptr {}, i32 0, i32 {}\n",
                                gep, lty, dst_addr, idx);
        st.body += std::format("  store {} {}, ptr {}\n", llvm_type(fty), cv, gep);
    }
}

}  // namespace herta::llvm_be
