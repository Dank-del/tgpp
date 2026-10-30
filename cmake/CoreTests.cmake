if(TGPP_BUILD_TESTS
   AND TGPP_BUILD_ASYNC
   AND TGPP_WITH_CURL
   AND Python3_FOUND
)
    add_executable(tgpp_async_http_test tests/async_http_client.cpp)
    target_link_libraries(tgpp_async_http_test PRIVATE tgpp::async)
    add_test(NAME async_http
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_http.py"
                     "$<TARGET_FILE:tgpp_async_http_test>"
    )
    set_tests_properties(async_http PROPERTIES TIMEOUT 30)
endif()
