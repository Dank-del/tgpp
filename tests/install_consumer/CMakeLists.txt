cmake_minimum_required(VERSION 3.24)
project(tgpp_installed_consumer LANGUAGES CXX)
include(CTest)
find_package(tgpp CONFIG REQUIRED COMPONENTS async async_ext webhook sqlite)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE tgpp::async_ext tgpp::webhook tgpp::sqlite)

add_test(NAME installed_consumer COMMAND consumer)
