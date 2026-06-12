// Driver склеивает все фазы компилятора. Берёт корневой .herta файл,
// рекурсивно подтягивает импорты (Name.herta из той же папки), ловит
// циклы и расхождения между module Name; и именем файла. Гонит модули
// через лексер, парсер и семантику в топологическом порядке, попутно
// прокидывая scope-ы для разрешения имён между модулями.

export module herta.driver;

import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;
import herta.semantic;
import herta.ir;

namespace herta::driver {

using herta::common::DiagnosticSink;
using herta::common::SourceFile;
namespace ast = herta::ast;

struct LoadedModule {
    std::string name;
    std::filesystem::path path;
    std::shared_ptr<SourceFile> source;       // лексер и парсер ссылаются сюда
    std::vector<herta::lexer::Token> tokens;  // string_view-ы смотрят в source
    std::unique_ptr<ast::Program> program;
    std::unique_ptr<herta::semantic::SemanticAnalyzer> sema;  // живёт пока жив Driver
};

export class Driver {
public:
    Driver(DiagnosticSink& sink) : sink_(sink) {}

    // Прогоняет программу: lex и parse всех модулей, потом семантика
    // в порядке зависимостей. true при успехе.
    bool compile(const std::filesystem::path& root_file);

    // Печатает IR всех модулей в топологическом порядке.
    // Звать после успешного compile().
    void dump_ir(std::ostream& os) const;

    // Лоуэрит все модули в IR и возвращает их в топологическом порядке.
    // Звать после compile().
    std::vector<herta::ir::Module> lower_all() const;

private:
    bool load_recursive(const std::filesystem::path& path);

    DiagnosticSink& sink_;
    std::filesystem::path search_dir_;
    // Топологический порядок: листья дерева зависимостей идут первыми.
    std::vector<std::unique_ptr<LoadedModule>> modules_;
    // Цвета для DFS: 0 — не тронут, 1 — в процессе, 2 — обработан.
    std::unordered_map<std::string, int> colors_;
    // Имя модуля → его индекс в modules_.
    std::unordered_map<std::string, std::size_t> by_name_;
};

bool Driver::compile(const std::filesystem::path& root_file) {
    auto root_abs = std::filesystem::absolute(root_file);
    search_dir_ = root_abs.parent_path();
    if (!load_recursive(root_abs)) return false;

    // Семантика идёт в том же топологическом порядке.
    // Scope каждого готового модуля копится, чтобы зависящие могли им пользоваться.
    std::unordered_map<std::string, std::shared_ptr<herta::semantic::Scope>> exports;

    for (std::size_t i = 0; i < modules_.size(); ++i) {
        const auto& mod = *modules_[i];

        // Собираем карту импортов для текущего модуля.
        std::unordered_map<std::string, std::shared_ptr<herta::semantic::Scope>> imports;
        for (const auto& imp : mod.program->imports) {
            auto it = exports.find(imp);
            if (it == exports.end()) {
                // Сюда нормально не попадаем: топологический порядок гарантирован load_recursive.
                sink_.report(herta::common::Diagnostic{
                    .file = mod.path.string(),
                    .loc = mod.program->module_loc,
                    .message = "internal: missing export scope for '" + imp + "'",
                });
                return false;
            }
            imports[imp] = it->second;
        }

        // require_main применяется только к корневому модулю (он идёт последним).
        bool is_root = (i + 1 == modules_.size());
        auto sema = std::make_unique<herta::semantic::SemanticAnalyzer>(
            *mod.program, mod.path.string(), sink_,
            std::move(imports), /*require_main=*/is_root);

        if (!sema->analyze()) return false;
        exports[mod.name] = sema->module_scope();
        modules_[i]->sema = std::move(sema);
    }
    return true;
}

std::vector<herta::ir::Module> Driver::lower_all() const {
    std::vector<herta::ir::Module> result;
    result.reserve(modules_.size());
    for (const auto& mod : modules_) {
        if (!mod->sema) continue;
        herta::ir::Lowerer lower(*mod->program, *mod->sema);
        auto ir_mod = lower.lower();
        result.push_back(std::move(ir_mod));
    }
    return result;
}

void Driver::dump_ir(std::ostream& os) const {
    for (const auto& ir_mod : lower_all()) {
        herta::ir::dump_module(ir_mod, os);
    }
}

bool Driver::load_recursive(const std::filesystem::path& path) {
    auto src_res = SourceFile::load(path);
    if (!src_res) {
        sink_.report(herta::common::Diagnostic{
            .file = path.string(), .loc = {},
            .message = src_res.error(),
        });
        return false;
    }
    auto source = std::make_shared<SourceFile>(std::move(*src_res));

    // Lex
    herta::lexer::Lexer lex(*source, sink_);
    auto toks_res = lex.tokenize();
    if (!toks_res) return false;

    // Parse
    herta::parser::Parser parser(*toks_res, source->name(), sink_);
    auto prog_res = parser.parse_program();
    if (!prog_res) return false;

    auto prog = std::make_unique<ast::Program>(std::move(*prog_res));
    auto module_name = prog->module_name;

    // Имя файла должно совпадать с module Name; в заголовке.
    auto stem = path.stem().string();
    if (module_name != stem) {
        sink_.report(herta::common::Diagnostic{
            .file = path.string(),
            .loc = prog->module_loc,
            .message = "module name '" + module_name +
                       "' does not match file name '" + stem + "'",
        });
        return false;
    }

    // Проверка на цикл импортов
    auto cit = colors_.find(module_name);
    if (cit != colors_.end()) {
        if (cit->second == 1) {
            sink_.report(herta::common::Diagnostic{
                .file = path.string(), .loc = prog->module_loc,
                .message = "circular import involving module '" + module_name + "'",
            });
            return false;
        }
        // Цвет 2 значит «уже обработан», перезагружать не нужно.
        return true;
    }
    colors_[module_name] = 1;  // помечаем как in-progress

    // Рекурсивно тянем зависимости.
    for (const auto& imp : prog->imports) {
        if (imp == module_name) {
            sink_.report(herta::common::Diagnostic{
                .file = path.string(), .loc = prog->module_loc,
                .message = "module '" + module_name + "' imports itself",
            });
            return false;
        }
        auto dep_path = search_dir_ / (imp + ".herta");
        if (!std::filesystem::exists(dep_path)) {
            sink_.report(herta::common::Diagnostic{
                .file = path.string(), .loc = prog->module_loc,
                .message = "imported module '" + imp + "' not found: " +
                           dep_path.string(),
            });
            return false;
        }
        if (!load_recursive(dep_path)) return false;
    }

    // Добавляем в список — уже после всех зависимостей, как и положено.
    auto mod = std::make_unique<LoadedModule>();
    mod->name = module_name;
    mod->path = path;
    mod->source = std::move(source);
    mod->tokens = std::move(*toks_res);
    mod->program = std::move(prog);
    by_name_[module_name] = modules_.size();
    modules_.push_back(std::move(mod));
    colors_[module_name] = 2;  // black
    return true;
}

}  // namespace herta::driver
