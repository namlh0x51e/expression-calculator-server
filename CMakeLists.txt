cmake_minimum_required(VERSION 3.30)

project(cmake-playground LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(FetchContent)

FetchContent_Declare(
    Asio
    URL "https://sourceforge.net/projects/asio/files/asio/1.36.0%20%28Stable%29/asio-1.36.0.zip/download"
)

FetchContent_MakeAvailable(Asio)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
)

FetchContent_MakeAvailable(spdlog)

add_library(Asio INTERFACE)
target_include_directories(Asio INTERFACE "${asio_SOURCE_DIR}/include")

add_subdirectory(src)


