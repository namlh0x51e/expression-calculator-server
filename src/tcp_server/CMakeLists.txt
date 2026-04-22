add_subdirectory(evaluation_engine)

add_library(tcp_server STATIC worker_thread_manager.cpp tcp_server.cpp connection.cpp)

target_include_directories(tcp_server
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tcp_server
    PUBLIC Asio spdlog::spdlog PRIVATE evaluation_engine
)

target_enable_warnings(tcp_server)

