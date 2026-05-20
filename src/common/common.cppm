// Module `herta.common` — общие сущности для всех фаз компилятора:
// позиции в исходниках, загрузка файлов, диагностика.

export module herta.common;

import std;

namespace herta::common {

// ---------------------------------------------------------------------------
// SourceLocation
// ---------------------------------------------------------------------------
// Позиция в исходном файле. line/column — 1-based,
// offset — байтовое смещение от начала файла.
export struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

// ---------------------------------------------------------------------------
// SourceFile
// ---------------------------------------------------------------------------
// Содержимое исходного файла и его имя. Хранит весь текст в памяти.
export class SourceFile {
public:
    SourceFile(std::string name, std::string contents)
        : name_(std::move(name)), contents_(std::move(contents)) {}

    // Загрузить файл с диска. При ошибке возвращает текстовое сообщение.
    static std::expected<SourceFile, std::string> load(
        const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::unexpected("cannot open file: " + path.string());
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        if (!in && !in.eof()) {
            return std::unexpected("read error: " + path.string());
        }
        return SourceFile(path.string(), std::move(buf).str());
    }

    std::string_view name() const noexcept { return name_; }
    std::string_view contents() const noexcept { return contents_; }
    std::size_t size() const noexcept { return contents_.size(); }

private:
    std::string name_;
    std::string contents_;
};

// ---------------------------------------------------------------------------
// Diagnostic / DiagnosticSink
// ---------------------------------------------------------------------------
// Формат вывода ошибки (по ТЗ):
//   <file>:<line>:<column>: error: <message>
export struct Diagnostic {
    std::string file;
    SourceLocation loc;
    std::string message;
};

// Накопитель диагностики. Передаётся в фазы по ссылке; без глобального state.
export class DiagnosticSink {
public:
    void report(Diagnostic d) {
        diags_.push_back(std::move(d));
    }

    bool has_errors() const noexcept { return !diags_.empty(); }

    std::span<const Diagnostic> diagnostics() const noexcept {
        return diags_;
    }

    void print_all(std::ostream& os) const {
        for (const auto& d : diags_) {
            os << d.file << ':' << d.loc.line << ':' << d.loc.column
               << ": error: " << d.message << '\n';
        }
    }

private:
    std::vector<Diagnostic> diags_;
};

}  // namespace herta::common
