find_package(Threads REQUIRED)
add_library(
    showcase_features
    showcase/catalog.cpp
    showcase/service.cpp
    showcase/handlers.cpp
    showcase/module.cpp
    demos/text.cpp
    demos/media.cpp
    demos/interactive.cpp
    demos/location.cpp
    demos/advanced.cpp
)
target_include_directories(showcase_features PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(showcase_features PUBLIC tgpp::ext Threads::Threads)
set_target_properties(
    showcase_features
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(showcase_features PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(TGPP_BUILD_EXAMPLES AND TGPP_WITH_CURL)
    add_executable(showcase_bot app/main.cpp)
    target_link_libraries(showcase_bot PRIVATE showcase_features)
    target_compile_definitions(
        showcase_bot PRIVATE TGPP_SHOWCASE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    )
endif()
if(TGPP_BUILD_TESTS)
    add_executable(showcase_tests tests/test_showcase.cpp)
    target_link_libraries(showcase_tests PRIVATE showcase_features)
    target_compile_definitions(
        showcase_tests PRIVATE TGPP_SHOWCASE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    )
    add_test(NAME showcase COMMAND showcase_tests)
endif()
