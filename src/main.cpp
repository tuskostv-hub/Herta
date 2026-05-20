#include "herta/common/diagnostic.hpp"
#include "herta/common/source_file.hpp"
#include "herta/lexer/lexer.hpp"
#include "herta/lexer/token.hpp"

import std;

namespace {

struct CliArgs {
    std::string input;
    std::string output;
    bool dump_tokens = false;
    bool dump_ast = false;
};

void print_usage(std::ostream& os) {
    os << "usage: myc <source.herta> [-o <output>] [--dump-tokens] [--dump-ast]\n";
}

std::expected<CliArgs, std::string> parse_args(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--dump-tokens") {
            a.dump_tokens = true;
        } else if (s == "--dump-ast") {
            a.dump_ast = true;
        } else if (s == "-o") {
            if (i + 1 >= argc) return std::unexpected("missing value for -o");
            a.output = argv[++i];
        } else if (!s.empty() && s[0] == '-') {
            return std::unexpected("unknown option: " + std::string(s));
        } else {
            if (!a.input.empty()) {
                return std::unexpected("multiple input files not supported");
            }
            a.input = std::string(s);
        }
    }
    if (a.input.empty()) return std::unexpected("no input file");
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);
    if (!args) {
        std::cerr << "error: " << args.error() << '\n';
        print_usage(std::cerr);
        return EXIT_FAILURE;
    }

    auto src = herta::common::SourceFile::load(args->input);
    if (!src) {
        std::cerr << "error: " << src.error() << '\n';
        return EXIT_FAILURE;
    }

    herta::common::DiagnosticSink sink;

    herta::lexer::Lexer lex(*src, sink);
    auto tokens = lex.tokenize();

    if (args->dump_tokens && tokens) {
        for (const auto& t : *tokens) {
            std::cout << herta::lexer::to_string(t) << '\n';
        }
    }

    if (sink.has_errors()) {
        sink.print_all(std::cerr);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
