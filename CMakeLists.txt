cmake_minimum_required(VERSION 3.25)

project(
    expression-calculator-server
    VERSION 0.1.0
    DESCRIPTION "Multithreaded TCP server for evaluating math expressions"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Pass -DFORCE_FETCH_CONTENT=ON to always download regardless of system packages.
option(FORCE_FETCH_CONTENT "Always download dependencies via FetchContent, skip system packages" OFF)

if(FORCE_FETCH_CONTENT)
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
endif()

include(FetchContent)

FetchContent_Declare(
    asio_dep
    URL "https://sourceforge.net/projects/asio/files/asio/1.36.0%20%28Stable%29/asio-1.36.0.zip/download"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    FIND_PACKAGE_ARGS 1.17.0
)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    FIND_PACKAGE_ARGS 3
)

FetchContent_MakeAvailable(asio_dep spdlog Catch2)

add_library(Asio INTERFACE)
target_include_directories(Asio INTERFACE "${asio_dep_SOURCE_DIR}/include")

add_subdirectory(src)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
    add_subdirectory(tools)
endif()

include(GNUInstallDirs)
install(TARGETS expression_calculator_server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
