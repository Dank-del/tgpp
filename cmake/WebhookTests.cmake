if(TGPP_BUILD_TESTS AND TGPP_WITH_WEBHOOK)
    add_executable(tgpp_webhook_tests tests/test_webhook.cpp)
    target_link_libraries(tgpp_webhook_tests PRIVATE tgpp::webhook)
    add_test(NAME webhook COMMAND tgpp_webhook_tests)
    set_tests_properties(webhook PROPERTIES TIMEOUT 30)
endif()
if(TGPP_BUILD_EXAMPLES
   AND TGPP_WITH_WEBHOOK
   AND TGPP_WITH_CURL
)
    add_executable(webhook_bot examples/webhook_bot.cpp)
    target_link_libraries(webhook_bot PRIVATE tgpp::webhook)
endif()
