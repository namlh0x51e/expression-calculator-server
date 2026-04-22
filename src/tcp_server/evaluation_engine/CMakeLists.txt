add_library(evaluation_engine STATIC tokenizer.cpp evaluation_engine.cpp)

target_include_directories(evaluation_engine
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(evaluation_engine
    PUBLIC spdlog::spdlog
)

target_enable_warnings(evaluation_engine)
