if(TGPP_BUILD_ASYNC AND (TGPP_BUILD_TESTS OR TGPP_BUILD_EXAMPLES))
    get_filename_component(async_showcase_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(async_showcase_dir "${async_showcase_root}/examples/async_showcase")
    add_library(
        async_showcase_features
        "${async_showcase_dir}/service.cpp"
        "${async_showcase_dir}/generated/catalog.cpp"
        "${async_showcase_dir}/generated/text.cpp"
        "${async_showcase_dir}/generated/media.cpp"
        "${async_showcase_dir}/generated/interactive.cpp"
        "${async_showcase_dir}/generated/location.cpp"
        "${async_showcase_dir}/generated/advanced.cpp"
    )
    target_include_directories(async_showcase_features PUBLIC "${async_showcase_dir}")
    target_link_libraries(async_showcase_features PUBLIC tgpp::async_ext)
    if(TGPP_BUILD_EXAMPLES AND TGPP_WITH_CURL)
        add_executable(async_showcase_bot "${async_showcase_dir}/main.cpp")
        target_link_libraries(async_showcase_bot PRIVATE async_showcase_features)
        target_compile_definitions(
            async_showcase_bot
            PRIVATE TGPP_SHOWCASE_ASSETS="${async_showcase_root}/examples/showcase_bot/assets"
        )
    endif()
    if(TGPP_BUILD_TESTS)
        add_executable(async_showcase_tests "${async_showcase_dir}/test_catalog.cpp")
        target_link_libraries(async_showcase_tests PRIVATE async_showcase_features)
        target_compile_definitions(
            async_showcase_tests
            PRIVATE TGPP_SHOWCASE_ASSETS="${async_showcase_root}/examples/showcase_bot/assets"
        )
        add_test(NAME async_showcase COMMAND async_showcase_tests)
        set_tests_properties(async_showcase PROPERTIES TIMEOUT 60)
        if(Python3_FOUND)
            add_test(NAME async_showcase_generated
                     COMMAND "${Python3_EXECUTABLE}" "${async_showcase_dir}/generate.py" --check
            )
        endif()
    endif()
endif()
