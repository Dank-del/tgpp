cmake_minimum_required(VERSION 3.24)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TGPP_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" TGPP_VERSION_CORE "${TGPP_VERSION}")
project(
    tgpp
    VERSION ${TGPP_VERSION_CORE}
    LANGUAGES CXX
)
message(STATUS "tgpp ${TGPP_VERSION}")

include(FetchContent)
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    set(TGPP_TESTS_DEFAULT "${BUILD_TESTING}")
else()
    set(TGPP_TESTS_DEFAULT OFF)
endif()
option(TGPP_BUILD_TESTS "Build tgpp's tests" "${TGPP_TESTS_DEFAULT}")
option(TGPP_WITH_CURL "Build the libcurl HTTP transport" ON)
option(TGPP_BUILD_EXAMPLES "Build the bot examples" OFF)
option(TGPP_BUILD_BENCHMARKS "Build dispatch throughput benchmarks" OFF)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
        URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(JSON_BuildTests OFF)
    set(JSON_Install ON)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS src/generated/*.cpp)
add_library(tgpp src/core.cpp ${GENERATED_SOURCES})
add_library(tgpp::tgpp ALIAS tgpp)
target_compile_features(tgpp PUBLIC cxx_std_20)
set_target_properties(
    tgpp
    PROPERTIES CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
)
target_include_directories(
    tgpp PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(tgpp PUBLIC nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(tgpp PUBLIC /utf-8)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tgpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
add_library(
    tgpp_ext src/ext/context.cpp src/ext/handler.cpp src/ext/router.cpp src/ext/application.cpp
             src/ext/polling.cpp src/ext/concurrent.cpp
)
add_library(tgpp::ext ALIAS tgpp_ext)
target_link_libraries(tgpp_ext PUBLIC tgpp::tgpp Threads::Threads)
set_target_properties(
    tgpp_ext
    PROPERTIES CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tgpp_ext PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TGPP_WITH_CURL)
    find_package(CURL 7.68 REQUIRED)
    target_sources(tgpp PRIVATE src/curl_transport.cpp)
    target_link_libraries(tgpp PUBLIC CURL::libcurl Threads::Threads)
    target_compile_definitions(tgpp PUBLIC TGPP_WITH_CURL=1)
endif()

include(cmake/Async.cmake)

if(PROJECT_IS_TOP_LEVEL OR TGPP_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
endif()
if(PROJECT_IS_TOP_LEVEL AND Python3_FOUND)
    add_custom_target(
        format
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/format.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_custom_target(
        check-format
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/format.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_custom_target(
        generate
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_custom_target(
        check-generated
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

if(TGPP_BUILD_TESTS)
    add_test(
        NAME cmake_consumer
        COMMAND
            "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_consumer" -B
            "${CMAKE_CURRENT_BINARY_DIR}/consumer-test"
            "-DFETCHCONTENT_SOURCE_DIR_TGPP=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json_SOURCE_DIR}"
            "-Dnlohmann_json_DIR=${nlohmann_json_DIR}" "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    )
    add_executable(tgpp_concurrency_tests tests/test_concurrent.cpp)
    target_link_libraries(tgpp_concurrency_tests PRIVATE tgpp::ext)
    add_test(NAME concurrency COMMAND tgpp_concurrency_tests)
    set_tests_properties(concurrency PROPERTIES TIMEOUT 30)
    add_executable(tgpp_ext_tests tests/test_ext.cpp)
    target_link_libraries(tgpp_ext_tests PRIVATE tgpp::ext)
    add_test(NAME framework COMMAND tgpp_ext_tests)
    add_executable(tgpp_tests tests/test_api.cpp)
    target_link_libraries(tgpp_tests PRIVATE tgpp)
    add_test(NAME api COMMAND tgpp_tests)
    if(Python3_FOUND)
        add_test(NAME generated COMMAND "${Python3_EXECUTABLE}"
                                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate.py" --check
        )
        add_test(NAME generator COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests -p
                                        test_generator.py
        )
        set_tests_properties(generator PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        if(TGPP_WITH_CURL)
            add_executable(tgpp_http_test tests/http_client.cpp)
            target_link_libraries(tgpp_http_test PRIVATE tgpp Threads::Threads)
            add_test(
                NAME http
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_http.py"
                        "$<TARGET_FILE:tgpp_http_test>"
            )
        endif()
    endif()
endif()

if(TGPP_BUILD_BENCHMARKS)
    add_executable(tgpp_dispatch_bench bench/dispatch.cpp)
    target_link_libraries(tgpp_dispatch_bench PRIVATE tgpp::ext)
endif()

if(TGPP_BUILD_EXAMPLES)
    if(NOT TGPP_WITH_CURL)
        message(FATAL_ERROR "The echo example requires TGPP_WITH_CURL=ON")
    endif()
    add_executable(echo_bot examples/echo.cpp)
    target_link_libraries(echo_bot PRIVATE tgpp)
    add_executable(send_message examples/send_message.cpp)
    target_link_libraries(send_message PRIVATE tgpp)
    add_executable(concurrent_bot examples/concurrent.cpp)
    target_link_libraries(concurrent_bot PRIVATE tgpp::ext)
endif()

if(TGPP_BUILD_EXAMPLES OR TGPP_BUILD_TESTS)
    add_subdirectory(examples/modular_bot)
    add_subdirectory(examples/showcase_bot)
endif()

include(cmake/Install.cmake)
include(cmake/WebhookTests.cmake)
foreach(
    test_file
    CoreTests
    FrameworkTests
    StoreTests
    FluentTests
    BotAppTests
    RouterTests
    ApiBuilderTests
    FutureApiTests
)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${test_file}.cmake")
        include("cmake/${test_file}.cmake")
    endif()
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AsyncShowcase.cmake")
    include(cmake/AsyncShowcase.cmake)
endif()
