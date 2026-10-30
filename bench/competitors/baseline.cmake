cmake_minimum_required(VERSION 3.24)
project(tgpp_baseline_bench LANGUAGES CXX)
set(TGPP_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE
)
set(TGPP_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE
)
add_subdirectory("${TGPP_BASELINE_SOURCE}" tgpp)
add_executable(tgpp_baseline_bench "${TGPP_BENCH_SOURCE}/baseline.cpp")
target_link_libraries(tgpp_baseline_bench PRIVATE tgpp::tgpp)
target_compile_features(tgpp_baseline_bench PRIVATE cxx_std_17)
