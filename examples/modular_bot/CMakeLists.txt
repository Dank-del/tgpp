add_library(
    modular_bot_domain features/tasks/domain/task_service.cpp
                       infrastructure/memory_task_repository.cpp
)
target_compile_features(modular_bot_domain PUBLIC cxx_std_20)
target_include_directories(modular_bot_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(
    modular_bot_features
    features/tasks/handlers/add_task_command.cpp features/tasks/handlers/list_tasks_command.cpp
    features/tasks/tasks_module.cpp features/welcome/start_command.cpp
    features/welcome/welcome_module.cpp middleware/logging_middleware.cpp
)
target_link_libraries(modular_bot_features PUBLIC tgpp::ext modular_bot_domain)

if(TGPP_BUILD_EXAMPLES AND TGPP_WITH_CURL)
    add_executable(modular_bot app/main.cpp app/config.cpp)
    target_link_libraries(modular_bot PRIVATE modular_bot_features)
endif()

if(TGPP_BUILD_TESTS)
    add_executable(modular_bot_tests tests/test_tasks.cpp)
    target_link_libraries(modular_bot_tests PRIVATE modular_bot_features)
    add_test(NAME modular_bot COMMAND modular_bot_tests)
endif()
