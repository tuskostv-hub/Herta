#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

import std;
import herta.common;
import herta.lexer;
import herta.ast;
import herta.parser;
import herta.semantic;
import herta.ir;
import herta.llvm;
import herta.driver;

namespace {

struct CliArgs {
    std::string input;
    std::string output;
    bool dump_tokens = false;
    bool dump_ast = false;
    bool dump_ir = false;
    bool emit_llvm = false;
    bool no_opt = false;
    bool run_after = false; // собрать бинарь и сразу запустить его
};

void print_usage(std::ostream& os) {
    os <<
        "usage: myc <source.herta> [options]\n"
        "  -o <path>       output executable (default: <input stem>)\n"
        "  --dump-tokens   print token stream and exit\n"
        "  --dump-ast      print AST and exit\n"
        "  --dump-ir       print three-address IR and exit\n"
        "  --emit-llvm     emit LLVM IR (.ll) to stdout and exit\n"
        "  --no-opt        disable IR optimizations (constant folding + DCE)\n"
        "  --run           compile to binary, run it, exit with its code\n";
}

std::expected<CliArgs, std::string> parse_args(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--dump-tokens") a.dump_tokens = true;
        else if (s == "--dump-ast") a.dump_ast = true;
        else if (s == "--dump-ir") a.dump_ir = true;
        else if (s == "--emit-llvm") a.emit_llvm = true;
        else if (s == "--no-opt") a.no_opt = true;
        else if (s == "--run") a.run_after = true;
        else if (s == "-o") {
            if (i + 1 >= argc) return std::unexpected("missing value for -o");
            a.output = argv[++i];
        } else if (!s.empty() && s[0] == '-') {
            return std::unexpected("unknown option: " + std::string(s));
        } else {
            if (!a.input.empty()) return std::unexpected("multiple input files not supported");
            a.input = std::string(s);
        }
    }
    if (a.input.empty()) return std::unexpected("no input file");
    return a;
}

// Зовёт clang для сборки .ll + runtime.c в исполняемый файл.
// Возвращает 0 при успехе.
int link_binary(const std::string& ll_path, const std::string& out_path) {
    auto cmd = std::format(
        "{} -O1 -Wno-override-module {} {} -o {} 2>&1",
        HERTA_CLANG_PATH, ll_path, HERTA_RUNTIME_C, out_path);
    std::FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "error: failed to invoke clang\n";
        return 1;
    }
    std::string output;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    int status = pclose(pipe);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code != 0) {
        std::cerr << "error: clang failed (" << code << "):\n" << output;
    }
    return code;
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

    // --dump-tokens и --dump-ast работают по одному корневому файлу без импортов
    if (args->dump_tokens || args->dump_ast) {
        auto src = herta::common::SourceFile::load(args->input);
        if (!src) { std::cerr << "error: " << src.error() << '\n'; return 1; }
        herta::lexer::Lexer lex(*src, sink);
        auto tokens_res = lex.tokenize();
        if (!tokens_res) { sink.print_all(std::cerr); return 1; }
        if (args->dump_tokens) {
            for (const auto& t : *tokens_res) std::cout << herta::lexer::to_string(t) << '\n';
            return 0;
        }
        herta::parser::Parser parser(*tokens_res, src->name(), sink);
        auto prog_res = parser.parse_program();
        if (!prog_res) { sink.print_all(std::cerr); return 1; }
        herta::ast::dump_ast(*prog_res, std::cout);
        return 0;
    }

    // Полный pipeline проходит через driver
    herta::driver::Driver driver(sink);
    driver.set_optimize(!args->no_opt);
    if (!driver.compile(args->input) || sink.has_errors()) {
        sink.print_all(std::cerr);
        return 1;
    }
    if (args->dump_ir) { driver.dump_ir(std::cout); return 0; }

    auto ir_modules = driver.lower_all();

    if (args->emit_llvm) {
        herta::llvm_be::Emitter em(std::move(ir_modules));
        std::cout << em.emit();
        return 0;
    }

    // Сборка нативного бинаря через LLVM IR и clang
    herta::llvm_be::Emitter em(std::move(ir_modules));
    auto ll = em.emit();

    // Пишем .ll во временный файл, дальше его подберёт clang
    namespace fs = std::filesystem;
    fs::path out_path = args->output.empty()
        ? fs::path(args->input).stem()
        : fs::path(args->output);
    fs::path ll_path = fs::temp_directory_path() /
        ("herta_" + std::to_string(::getpid()) + "_" + out_path.filename().string() + ".ll");
    {
        std::ofstream ofs(ll_path);
        ofs << ll;
    }

    int code = link_binary(ll_path.string(), out_path.string());
    std::error_code ec;
    fs::remove(ll_path, ec);
    if (code != 0) return code;

    if (args->run_after) {
        std::string run_cmd = "./" + out_path.string();
        int status = std::system(run_cmd.c_str());
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return 0;
}