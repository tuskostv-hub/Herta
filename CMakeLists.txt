cmake_minimum_required(VERSION 4.0)

# `import std;` is still gated as experimental in CMake 4.x — must acknowledge
# the version-specific UUID BEFORE project() is called. For CMake 4.3.x:
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

# libc++ is the only standard library that ships the `std` module today.
# Must be selected BEFORE project() so CMake's compiler probe uses it.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++")

# The system-level /usr/lib/x86_64-linux-gnu/libc++.modules.json from apt.llvm.org
# packaging points to non-existent paths; use the LLVM-22 internal one instead.
set(CMAKE_CXX_STDLIB_MODULES_JSON "/usr/lib/llvm-22/lib/libc++.modules.json")

project(herta
    VERSION 0.1.0
    DESCRIPTION "Herta language compiler"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Standard / modules
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable the standard library module (`import std;`) — CMake 4.0+.
set(CMAKE_CXX_MODULE_STD ON)

# Scan all C++ sources for module dependencies (Ninja required).
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# Toolchain checks: `import std;` requires Clang + libc++ + Ninja.
# -----------------------------------------------------------------------------
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
        "Herta requires the Ninja generator for C++ modules support.\n"
        "Re-run with: cmake -G Ninja -S . -B build -DCMAKE_CXX_COMPILER=clang++-22")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "Herta requires Clang (≥ 22) with libc++ for `import std;`.\n"
        "Re-run with: cmake -G Ninja -S . -B build -DCMAKE_CXX_COMPILER=clang++-22")
endif()

# -----------------------------------------------------------------------------
# Warnings
# -----------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
option(HERTA_BUILD_TESTS "Build tests" ON)

add_library(herta_core STATIC)
target_sources(herta_core
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/common/common.cppm
            src/lexer/lexer.cppm
            src/parser/ast.cppm
            src/parser/parser.cppm
            src/semantic/semantic.cppm
            src/ir/ir.cppm
            src/driver/driver.cppm
)

add_executable(myc src/main.cpp)
target_link_libraries(myc PRIVATE herta_core)

# Convenience targets.
add_custom_target(run
    COMMAND $<TARGET_FILE:myc> ${HERTA_RUN_ARGS}
    DEPENDS myc
    USES_TERMINAL
)

add_custom_target(debug
    COMMAND ${CMAKE_COMMAND} -E echo "Build type: ${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
)

if(HERTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
