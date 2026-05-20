#include "herta/common/diagnostic.hpp"

import std;

namespace herta::common {

void DiagnosticSink::report(Diagnostic d) {
    diags_.push_back(std::move(d));
}

void DiagnosticSink::print_all(std::ostream& os) const {
    for (const auto& d : diags_) {
        os << d.file << ':' << d.loc.line << ':' << d.loc.column
           << ": error: " << d.message << '\n';
    }
}

}  // namespace herta::common
