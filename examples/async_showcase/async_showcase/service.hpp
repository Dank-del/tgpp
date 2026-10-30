#pragma once
#include "catalog.hpp"
#include <deque>
#include <map>
#include <tgpp/ext/async.hpp>

namespace async_showcase {
class Sessions {
    struct Session;
    struct State;
    std::shared_ptr<State> state_;
    static tgpp::Task<bool> enqueue_owned(std::shared_ptr<State>, std::int64_t, std::string);
    static tgpp::Task<void> cancel_owned(std::shared_ptr<State>, std::int64_t);
    static tgpp::Task<void> work(std::shared_ptr<State>, std::shared_ptr<Session>);
    static tgpp::Task<void> execute(std::shared_ptr<State>, std::shared_ptr<Session>, std::string);
    static tgpp::Task<void> stop_owned(std::shared_ptr<State>);

  public:
    Sessions(asio::any_io_executor,
             const tgpp::AsyncBot&,
             tgpp::BlockingPool&,
             std::filesystem::path assets,
             std::filesystem::path reports,
             std::string bot_username = {});
    tgpp::Task<bool> enqueue(std::int64_t chat, std::string job);
    tgpp::Task<void> cancel(std::int64_t chat);
    tgpp::Task<void> stop();
    tgpp::Task<void> handle(tgpp::ext::AsyncContext&);
};
} // namespace async_showcase
