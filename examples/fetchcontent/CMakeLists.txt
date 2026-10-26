cmake_minimum_required(VERSION 3.24)
project(tgpp_consumer LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    tgpp
    GIT_REPOSITORY https://github.com/Dank-del/tgpp.git
    GIT_TAG v0.0.1-rc1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(tgpp)

add_executable(my_bot main.cpp)
target_link_libraries(my_bot PRIVATE tgpp::ext)
