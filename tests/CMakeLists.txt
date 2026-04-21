foreach(test_name IN ITEMS test_tokenizer test_evaluation_engine)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE Catch2::Catch2WithMain evaluation_engine)
    target_enable_warnings(${test_name})
endforeach()

# Expose catch_discover_tests() when Catch2 was populated via FetchContent
if(DEFINED Catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
endif()

include(Catch)
catch_discover_tests(test_tokenizer)
catch_discover_tests(test_evaluation_engine)
