import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;
import herta.semantic;
import herta.ir;
import herta.driver;

namespace {

struct CliArgs {
    std::string input;
    std::string output;
    bool dump_tokens = false;
    bool dump_ast = false;
    bool dump_ir = false;
    bool no_opt = false;
};

void print_usage(std::ostream& os) {
    os << "usage: myc <source.herta> [-o <output>] [--dump-tokens] [--dump-ast] [--dump-ir] [--no-opt]\n";
}

std::expected<CliArgs, std::string> parse_args(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--dump-tokens") {
            a.dump_tokens = true;
        } else if (s == "--dump-ast") {
            a.dump_ast = true;
        } else if (s == "--dump-ir") {
            a.dump_ir = true;
        } else if (s == "--no-opt") {
            a.no_opt = true;
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
        return 1;
    }

    herta::common::DiagnosticSink sink;

    // --dump-tokens / --dump-ast работают только с корневым файлом
    // (без подгрузки импортов — для отладки).
    if (args->dump_tokens || args->dump_ast) {
        auto src = herta::common::SourceFile::load(args->input);
        if (!src) {
            std::cerr << "error: " << src.error() << '\n';
            return 1;
        }
        herta::lexer::Lexer lex(*src, sink);
        auto tokens_res = lex.tokenize();
        if (!tokens_res) {
            sink.print_all(std::cerr);
            return 1;
        }
        if (args->dump_tokens) {
            for (const auto& t : *tokens_res) {
                std::cout << herta::lexer::to_string(t) << '\n';
            }
            return 0;
        }
        herta::parser::Parser parser(*tokens_res, src->name(), sink);
        auto prog_res = parser.parse_program();
        if (!prog_res) {
            sink.print_all(std::cerr);
            return 1;
        }
        herta::ast::dump_ast(*prog_res, std::cout);
        return 0;
    }

    // Полный pipeline через driver (с разрешением `import`).
    herta::driver::Driver driver(sink);
    driver.set_optimize(!args->no_opt);
    if (!driver.compile(args->input) || sink.has_errors()) {
        sink.print_all(std::cerr);
        return 1;
    }
    if (args->dump_ir) {
        driver.dump_ir(std::cout);
    }
    return 0;
}
