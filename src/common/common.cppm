// Общие штуки для всех фаз: позиции в исходнике, загрузка файлов,
// сбор и печать диагностик.

export module herta.common;

import std;

namespace herta::common {

// Позиция в исходнике. line и column нумеруются с 1, offset — байтовое
// смещение от начала файла.
export struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

// Содержимое исходника плюс его имя. Файл целиком держится в памяти.
export class SourceFile {
public:
    SourceFile(std::string name, std::string contents)
        : name_(std::move(name)), contents_(std::move(contents)) {}

    // Подгружает файл с диска. На ошибке отдаёт текст сообщения.
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

// Формат сообщения об ошибке:
//   <file>:<line>:<column>: error: <message>
export struct Diagnostic {
    std::string file;
    SourceLocation loc;
    std::string message;
};

// Копилка диагностик. Передаётся фазам по ссылке, глобального состояния нет.
export class DiagnosticSink {
public:
    void report(Diagnostic d) {
        diags_.push_back(std::move(d));
    }

    bool has_errors() const noexcept { return !diags_.empty(); }

    // Возвращает ссылку на внутренний вектор. Держать её через последующий
    // report() нельзя, может произойти реаллокация. Спан заменён на ссылку,
    // чтобы время жизни явно совпадало с самим Sink.
    const std::vector<Diagnostic>& diagnostics() const noexcept {
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
