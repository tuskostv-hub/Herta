#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "herta/common/source_location.hpp"

namespace herta::common {

// Одна диагностическая запись. Формат вывода:
//   <file>:<line>:<column>: error: <message>
struct Diagnostic {
    std::string file;
    SourceLocation loc;
    std::string message;
};

// Накопитель диагностики. Передаётся в фазы компилятора по ссылке.
class DiagnosticSink {
public:
    void report(Diagnostic d);
    bool has_errors() const noexcept { return !diags_.empty(); }
    std::span<const Diagnostic> diagnostics() const noexcept { return diags_; }

    // Печать всех записей в указанный поток (обычно stderr).
    void print_all(std::ostream& os) const;

private:
    std::vector<Diagnostic> diags_;
};

}  // namespace herta::common
