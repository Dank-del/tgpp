if(TGPP_BUILD_TESTS
   AND TGPP_BUILD_ASYNC
   AND Python3_FOUND
)
    set(future_api_compiler_style unix)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(future_api_compiler_style msvc)
    endif()
    add_test(
        NAME future_fluent_api
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_future_fluent.py"
            --compiler "${CMAKE_CXX_COMPILER}" --compiler-style "${future_api_compiler_style}"
            --work-dir "${CMAKE_CURRENT_BINARY_DIR}" --include
            "${CMAKE_CURRENT_SOURCE_DIR}/include" "${TGPP_ASIO_INCLUDE_DIR}"
            "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
            COMMAND_EXPAND_LISTS
    )
    set_tests_properties(future_fluent_api PROPERTIES TIMEOUT 180)
endif()
