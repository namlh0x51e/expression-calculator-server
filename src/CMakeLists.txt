add_library(evaluation_engine STATIC tokenizer.cpp evaluation_engine.cpp)

target_include_directories(evaluation_engine
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(evaluation_engine
    PUBLIC spdlog::spdlog
)

target_enable_warnings(evaluation_engine)

add_executable(expression_calculator_server main.cpp)

target_link_libraries(expression_calculator_server
    PRIVATE evaluation_engine Asio
)

target_enable_warnings(expression_calculator_server)

set_target_properties(expression_calculator_server PROPERTIES
    OUTPUT_NAME "expression-calculator-server"
)
