cmake_minimum_required(VERSION 3.24)
project(tgpp_competitor_bench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)
if(NOT EXISTS "${TGBOT_CPP_SOURCE}/src/Api.cpp")
    message(FATAL_ERROR "Set TGBOT_CPP_SOURCE to the pinned tgbot-cpp checkout")
endif()
# Compile the upstream API client sources unchanged. Webhook/polling sources are deliberately
# omitted: this executable measures outbound client calls only.
add_library(
    tgbot_api STATIC
    ${TGBOT_CPP_SOURCE}/src/Api.cpp
    ${TGBOT_CPP_SOURCE}/src/ApiCodec.cpp
    ${TGBOT_CPP_SOURCE}/src/ApiMethods.cpp
    ${TGBOT_CPP_SOURCE}/src/HttpClient.cpp
    ${TGBOT_CPP_SOURCE}/src/TgException.cpp
    ${TGBOT_CPP_SOURCE}/src/CurlHttpClient.cpp
    ${TGBOT_CPP_SOURCE}/src/InputFile.cpp
    ${TGBOT_CPP_SOURCE}/src/Types.cpp
)
target_include_directories(tgbot_api PUBLIC ${TGBOT_CPP_SOURCE}/include)
target_link_libraries(tgbot_api PUBLIC CURL::libcurl nlohmann_json::nlohmann_json Threads::Threads)
add_executable(tgbot_cpp_bench tgbot_cpp.cpp)
target_link_libraries(tgbot_cpp_bench PRIVATE tgbot_api)
