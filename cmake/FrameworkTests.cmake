if(TGPP_BUILD_TESTS AND TGPP_BUILD_ASYNC)
    add_executable(tgpp_async_framework_tests tests/test_async_framework.cpp)
    target_link_libraries(tgpp_async_framework_tests PRIVATE tgpp::async_ext)
    add_test(NAME async_framework COMMAND tgpp_async_framework_tests)
    set_tests_properties(async_framework PROPERTIES TIMEOUT 30)
endif()
if(TGPP_BUILD_EXAMPLES
   AND TGPP_WITH_CURL
   AND TGPP_BUILD_ASYNC
)
    add_executable(async_bot examples/async_bot.cpp)
    target_link_libraries(async_bot PRIVATE tgpp::async_ext)
endif()
if(TGPP_BUILD_BENCHMARKS
   AND TGPP_BUILD_ASYNC
   AND TGPP_WITH_CURL
)
    add_executable(tgpp_http_bench bench/http.cpp)
    target_link_libraries(tgpp_http_bench PRIVATE tgpp::async)
endif()
if(TGPP_BUILD_EXAMPLES
   AND TGPP_WITH_CURL
   AND TGPP_BUILD_ASYNC
   AND TGPP_WITH_SQLITE
)
    add_executable(durable_async_bot examples/durable_async.cpp)
    target_link_libraries(durable_async_bot PRIVATE tgpp::async_ext tgpp::sqlite)
endif()
