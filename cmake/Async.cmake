option(TGPP_BUILD_ASYNC "Build C++20 coroutine runtime" ON)
option(TGPP_WITH_WEBHOOK "Build reverse-proxy webhook listener" OFF)
option(TGPP_WITH_SQLITE "Build SQLite session and inbox stores" OFF)
if(NOT TGPP_BUILD_ASYNC)
    if(TGPP_WITH_WEBHOOK OR TGPP_WITH_SQLITE)
        message(FATAL_ERROR "Webhook and SQLite components require TGPP_BUILD_ASYNC")
    endif()
    return()
endif()
find_path(TGPP_ASIO_INCLUDE_DIR asio.hpp)
if(TGPP_ASIO_INCLUDE_DIR)
    file(STRINGS "${TGPP_ASIO_INCLUDE_DIR}/asio/version.hpp" asio_version_line
         REGEX "^#define ASIO_VERSION [0-9]+"
    )
    string(REGEX MATCH "[0-9]+" asio_version "${asio_version_line}")
    if(NOT asio_version OR asio_version LESS 103002)
        message(STATUS "System standalone Asio is too old; using pinned 1.30.2")
        set(TGPP_ASIO_INCLUDE_DIR "")
    endif()
endif()
if(NOT TGPP_ASIO_INCLUDE_DIR)
    FetchContent_Declare(
        tgpp_asio_source
        URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/asio-1-30-2
        URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(tgpp_asio_source)
    set(TGPP_ASIO_INCLUDE_DIR "${tgpp_asio_source_SOURCE_DIR}/asio/include")
endif()
add_library(tgpp_asio INTERFACE)
set_target_properties(tgpp_asio PROPERTIES EXPORT_NAME asio)
target_include_directories(
    tgpp_asio INTERFACE "$<BUILD_INTERFACE:${TGPP_ASIO_INCLUDE_DIR}>"
                        "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(tgpp_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    target_link_libraries(tgpp_asio INTERFACE ws2_32 mswsock)
endif()
add_library(tgpp_async src/async.cpp)
add_library(tgpp::async ALIAS tgpp_async)
set_target_properties(tgpp_async PROPERTIES EXPORT_NAME async)
target_link_libraries(tgpp_async PUBLIC tgpp::tgpp tgpp_asio Threads::Threads)
if(TGPP_WITH_CURL)
    target_sources(tgpp_async PRIVATE src/async_curl.cpp)
endif()
add_library(tgpp_async_ext src/ext/async.cpp src/ext/stores.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ext/bot_app.cpp")
    target_sources(tgpp_async_ext PRIVATE src/ext/bot_app.cpp)
endif()
add_library(tgpp::async_ext ALIAS tgpp_async_ext)
set_target_properties(tgpp_async_ext PROPERTIES EXPORT_NAME async_ext)
target_link_libraries(tgpp_async_ext PUBLIC tgpp_async tgpp::ext)
if(TGPP_WITH_SQLITE)
    find_package(SQLite3 REQUIRED)
    add_library(tgpp_sqlite src/ext/sqlite_stores.cpp)
    add_library(tgpp::sqlite ALIAS tgpp_sqlite)
    set_target_properties(tgpp_sqlite PROPERTIES EXPORT_NAME sqlite)
    target_link_libraries(tgpp_sqlite PUBLIC tgpp_async_ext SQLite::SQLite3)
endif()
if(TGPP_WITH_WEBHOOK)
    enable_language(C)
    FetchContent_Declare(
        tgpp_llhttp_source
        URL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.3.0
        URL_HASH SHA256=1a2b45cb8dda7082b307d336607023aa65549d6f060da1d246b1313da22b685a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused
    )
    FetchContent_GetProperties(tgpp_llhttp_source)
    if(NOT tgpp_llhttp_source_POPULATED)
        FetchContent_MakeAvailable(tgpp_llhttp_source)
    endif()
    add_library(
        tgpp_llhttp STATIC
        ${tgpp_llhttp_source_SOURCE_DIR}/src/api.c ${tgpp_llhttp_source_SOURCE_DIR}/src/http.c
        ${tgpp_llhttp_source_SOURCE_DIR}/src/llhttp.c
    )
    target_include_directories(
        tgpp_llhttp PUBLIC "$<BUILD_INTERFACE:${tgpp_llhttp_source_SOURCE_DIR}/include>"
    )
    set_target_properties(tgpp_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME llhttp)
    add_library(tgpp_webhook src/ext/webhook.cpp)
    add_library(tgpp::webhook ALIAS tgpp_webhook)
    set_target_properties(tgpp_webhook PROPERTIES EXPORT_NAME webhook)
    target_link_libraries(
        tgpp_webhook
        PUBLIC tgpp_async_ext
        PRIVATE tgpp_llhttp
    )
endif()
