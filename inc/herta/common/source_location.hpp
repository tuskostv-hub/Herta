#pragma once

#include <cstdint>

namespace herta::common {

// Позиция в исходном файле. line/column — 1-based, offset — байтовое смещение
// от начала файла.
struct SourceLocation {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

}  // namespace herta::common
