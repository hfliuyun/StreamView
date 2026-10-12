cmake_minimum_required(VERSION 3.28)

project(
    StreamView
    VERSION 0.1.0
    DESCRIPTION "Read-only bit-level media structure analyzer"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(STREAMVIEW_BUILD_APP "Build the StreamView desktop application" ON)
option(STREAMVIEW_BUILD_TESTS "Build StreamView tests" ON)
option(STREAMVIEW_BUILD_BENCHMARKS "Build the opt-in performance observation harness" OFF)
option(STREAMVIEW_ENABLE_SANITIZERS "Enable supported sanitizers" OFF)
option(STREAMVIEW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(STREAMVIEW_ENABLE_CLANG_TIDY "Run the pinned Clang-Tidy checks during builds" OFF)
set(
    STREAMVIEW_CLANG_TIDY_EXECUTABLE
    ""
    CACHE FILEPATH
    "Clang-Tidy 18.x executable used by the static-analysis preset"
)
set(
    STREAMVIEW_MINIMUM_QT_VERSION
    "6.11"
    CACHE STRING
    "Minimum Qt version; release builds use the default"
)

find_package(Qt6 ${STREAMVIEW_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Core Concurrent Sql)

if(STREAMVIEW_BUILD_APP)
    find_package(Qt6 ${STREAMVIEW_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Gui Widgets)
endif()

if(STREAMVIEW_BUILD_TESTS)
    include(CTest)
    enable_testing()
    find_package(Qt6 ${STREAMVIEW_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Test)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(StreamViewWarnings)
include(StreamViewClangTidy)
streamview_configure_clang_tidy()

add_subdirectory(src/core)
add_subdirectory(src/rules)
add_subdirectory(tools/svtool)

if(STREAMVIEW_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(STREAMVIEW_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(STREAMVIEW_BUILD_BENCHMARKS)
    if(NOT STREAMVIEW_BUILD_APP)
        message(FATAL_ERROR "Performance benchmarks require STREAMVIEW_BUILD_APP=ON")
    endif()
    add_subdirectory(benchmarks)
endif()
