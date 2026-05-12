#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace herta::common {

// Содержимое исходного файла и его имя. Хранит весь текст в памяти —
// для учебного компилятора этого достаточно.
class SourceFile {
public:
    SourceFile(std::string name, std::string contents);

    // Загрузить файл с диска. При ошибке возвращает сообщение.
    static std::expected<SourceFile, std::string> load(
        const std::filesystem::path& path);

    std::string_view name() const noexcept { return name_; }
    std::string_view contents() const noexcept { return contents_; }
    std::size_t size() const noexcept { return contents_.size(); }

private:
    std::string name_;
    std::string contents_;
};

}  // namespace herta::common
