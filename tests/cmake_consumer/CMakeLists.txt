cmake_minimum_required(VERSION 3.24)
project(tgpp_parent_test LANGUAGES CXX)
include(CTest)
add_custom_target(generate)

include(FetchContent)
FetchContent_Declare(
    tgpp
    GIT_REPOSITORY https://github.com/Dank-del/tgpp.git
    GIT_TAG v0.0.1-rc1
)
set(TGPP_WITH_CURL
    OFF
    CACHE BOOL "" FORCE
)
set(TGPP_BUILD_ASYNC
    OFF
    CACHE BOOL "" FORCE
)
FetchContent_MakeAvailable(tgpp)

if(NOT BUILD_TESTING)
    message(FATAL_ERROR "tgpp disabled the parent's tests")
endif()
if(NOT TARGET tgpp::tgpp OR NOT TARGET tgpp::ext)
    message(FATAL_ERROR "tgpp did not provide its public targets")
endif()
if(TARGET tgpp_tests
   OR TARGET tgpp_ext_tests
   OR TARGET showcase_tests
   OR TARGET modular_bot_tests
)
    message(FATAL_ERROR "tgpp enabled its own tests inside the parent build")
endif()
add_executable(consumer "${CMAKE_CURRENT_LIST_DIR}/../../examples/fetchcontent/main.cpp")
target_link_libraries(consumer PRIVATE tgpp::ext)
