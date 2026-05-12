#include "herta/common/source_file.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace herta::common {

SourceFile::SourceFile(std::string name, std::string contents)
    : name_(std::move(name)), contents_(std::move(contents)) {}

std::expected<SourceFile, std::string> SourceFile::load(
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

}  // namespace herta::common
