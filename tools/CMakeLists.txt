add_executable(expression_gen expression_gen.cpp)

target_compile_features(expression_gen PRIVATE cxx_std_23)

target_enable_warnings(expression_gen)
target_compile_options(expression_gen PRIVATE -Wno-pedantic)  # __int128 is a GCC/Clang extension

target_link_libraries(expression_gen PRIVATE Asio spdlog::spdlog)

set_target_properties(expression_gen PROPERTIES
    OUTPUT_NAME "expression-gen"
)

install(TARGETS expression_gen
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
