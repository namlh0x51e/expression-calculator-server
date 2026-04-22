add_subdirectory(tcp_server)

add_executable(expression_calculator_server main.cpp signal_listener.cpp)

target_link_libraries(expression_calculator_server
    PRIVATE tcp_server
)

target_enable_warnings(expression_calculator_server)

set_target_properties(expression_calculator_server PROPERTIES
    OUTPUT_NAME "expression-calculator-server"
)
