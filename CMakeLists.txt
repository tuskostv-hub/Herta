cmake_minimum_required(VERSION 3.25)
project(herta
    VERSION 0.1.0
    DESCRIPTION "Herta language compiler"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HERTA_BUILD_TESTS "Build tests" ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

set(HERTA_CORE_SOURCES
    src/common/diagnostic.cpp
    src/common/source_file.cpp
    src/lexer/token.cpp
    src/lexer/lexer.cpp
)

add_library(herta_core STATIC ${HERTA_CORE_SOURCES})
target_include_directories(herta_core
    PUBLIC ${CMAKE_SOURCE_DIR}/inc
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
    COMMAND ${CMAKE_COMMAND} -E echo "Configured build type: ${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
)

if(HERTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
