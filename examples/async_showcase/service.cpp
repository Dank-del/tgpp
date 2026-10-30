#include "async_showcase/service.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace async_showcase {
namespace {
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(
            from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "replace async showcase report");
    }
#else
    std::filesystem::rename(from, to);
#endif
}

tgpp::Json load_report(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return tgpp::Json::array();
    }
    try {
        if (std::filesystem::file_size(path) > 1024 * 1024) {
            throw std::runtime_error("oversized report");
        }
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("unreadable report");
        }
        tgpp::Json value;
        input >> value;
        if (!value.is_array() || value.size() > 33) {
            throw std::runtime_error("invalid report");
        }
        std::set<std::string> ids;
        for (const auto& row : value) {
            for (const auto* field : {"id", "title", "group", "status"}) {
                if (!row.is_object() || !row.contains(field) || !row.at(field).is_string()) {
                    throw std::runtime_error("invalid report row");
                }
            }
            auto id = row.at("id").get<std::string>();
            const auto status = row.at("status").get<std::string>();
            if (id.empty() || id.size() > 40 || !ids.insert(id).second ||
                row.at("title").get_ref<const std::string&>().size() > 80 ||
                row.at("group").get_ref<const std::string&>().size() > 40 ||
                (status != "passed" && status != "failed") ||
                (row.contains("error") &&
                 (!row.at("error").is_string() ||
                  row.at("error").get_ref<const std::string&>().size() > 200))) {
                throw std::runtime_error("invalid report values");
            }
        }
        return value;
    } catch (const std::exception&) {
        // A stale/corrupt example report must not prevent the menu or a fresh tour.
        auto quarantine = path;
        quarantine += ".corrupt";
        try {
            replace_file(path, quarantine);
        } catch (const std::exception&) {
        }
        return tgpp::Json::array();
    }
}

void save_report(const std::filesystem::path& path, const std::string& snapshot) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream output(temporary, std::ios::trunc);
        output << snapshot << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("cannot persist async showcase report");
        }
        replace_file(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace

using namespace tgpp;
using namespace std::chrono_literals;

struct Sessions::Session {
    std::int64_t chat;
    std::deque<std::string> jobs;
    std::string active;
    bool running = false;
    bool report_loaded = false;
    Json report = Json::array();
    std::shared_ptr<asio::cancellation_signal> cancel;

    explicit Session(std::int64_t id) : chat(id) {}
};

struct Sessions::State {
    asio::strand<asio::any_io_executor> strand;
    const AsyncBot& bot;
    BlockingPool& files;
    std::filesystem::path assets, reports;
    Catalog catalog = make_catalog();
    std::map<std::int64_t, std::shared_ptr<Session>> sessions;
    bool closing = false;
    std::string bot_username;

    State(asio::any_io_executor ex,
          const AsyncBot& client,
          BlockingPool& pool,
          std::filesystem::path a,
          std::filesystem::path r,
          std::string username)
        : strand(asio::make_strand(ex)), bot(client), files(pool), assets(std::move(a)),
          reports(std::move(r)), bot_username(std::move(username)) {}
};

Sessions::Sessions(asio::any_io_executor ex,
                   const AsyncBot& bot,
                   BlockingPool& pool,
                   std::filesystem::path assets,
                   std::filesystem::path reports,
                   std::string bot_username)
    : state_(std::make_shared<State>(
          ex, bot, pool, std::move(assets), std::move(reports), std::move(bot_username))) {}

Task<bool>
Sessions::enqueue_owned(std::shared_ptr<State> state, std::int64_t chat, std::string job) {
    if (state->closing || chat <= 0) {
        co_return false;
    }
    if (job != "menu" && job != "tour" && job != "report" && !state->catalog.find(job)) {
        co_return false;
    }
    auto found = state->sessions.find(chat);
    if (found == state->sessions.end()) {
        if (state->sessions.size() >= 8) {
            auto idle =
                std::find_if(state->sessions.begin(), state->sessions.end(), [](const auto& item) {
                    return !item.second->running && item.second->jobs.empty();
                });
            if (idle == state->sessions.end()) {
                co_return false;
            }
            state->sessions.erase(idle);
        }
        found = state->sessions.emplace(chat, std::make_shared<Session>(chat)).first;
    }
    auto session = found->second;
    if (session->jobs.size() >= 3 || session->active == job ||
        std::find(session->jobs.begin(), session->jobs.end(), job) != session->jobs.end()) {
        co_return false;
    }
    session->jobs.push_back(std::move(job));
    if (!session->running) {
        session->running = true;
        asio::co_spawn(state->strand, work(state, session), [session](std::exception_ptr error) {
            if (error) {
                session->running = false;
                session->active.clear();
                session->jobs.clear();
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& e) {
                    std::cerr << "Async showcase worker: " << e.what() << '\n';
                }
            }
        });
    }
    co_return true;
}

Task<bool> Sessions::enqueue(std::int64_t chat, std::string job) {
    co_return co_await asio::co_spawn(
        state_->strand, enqueue_owned(state_, chat, std::move(job)), asio::use_awaitable);
}

Task<void> Sessions::cancel_owned(std::shared_ptr<State> state, std::int64_t chat) {
    auto found = state->sessions.find(chat);
    if (found != state->sessions.end()) {
        found->second->jobs.clear();
        if (found->second->cancel) {
            found->second->cancel->emit(asio::cancellation_type::all);
        }
    }
    co_return;
}

Task<void> Sessions::cancel(std::int64_t chat) {
    co_await asio::co_spawn(state_->strand, cancel_owned(state_, chat), asio::use_awaitable);
}

Task<void>
Sessions::execute(std::shared_ptr<State> state, std::shared_ptr<Session> session, std::string job) {
    auto wait = [](std::chrono::milliseconds duration) -> Task<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex, duration);
        co_await timer.async_wait(asio::use_awaitable);
    };
    AsyncDemoContext context(state->bot, session->chat, state->assets, wait);
    if (!session->report_loaded) {
        const auto path = state->reports / (std::to_string(session->chat) + ".json");
        session->report = co_await state->files.run([path] { return load_report(path); });
        session->report_loaded = true;
    }
    if (job == "menu") {
        SendMessageRequest request{};
        request.chat_id = session->chat;
        request.text = "tgpp async showcase · 33 demos\nChoose a demo or /tour. /stop cancels this "
                       "chat's work.";
        request.reply_markup = state->catalog.keyboard();
        request.disable_notification = true;
        co_await context.call([&] { return state->bot.send_message(request); });
        co_return;
    }
    if (job != "report") {
        if (job == "tour") {
            session->report = Json::array();
            co_await context.text("Starting all 33 async demos. /stop cancels your tour.");
        }
        for (const auto& demo : state->catalog.demos()) {
            if (job != "tour" && job != demo.id) {
                continue;
            }
            Json result = {{"id", demo.id},
                           {"title", demo.title},
                           {"group", demo.group},
                           {"status", "passed"}};
            std::exception_ptr error;
            try {
                co_await demo.run(context);
            } catch (...) {
                error = std::current_exception();
            }
            if (error) {
                const auto cancellation = co_await asio::this_coro::cancellation_state;
                if (cancellation.cancelled() != asio::cancellation_type::none) {
                    std::rethrow_exception(error);
                }
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& cause) {
                    result["status"] = "failed";
                    result["error"] = std::string(cause.what()).substr(0, 200);
                }
            }
            auto found =
                std::find_if(session->report.begin(), session->report.end(), [&](const auto& row) {
                    return row.value("id", std::string{}) == demo.id;
                });
            if (found == session->report.end()) {
                session->report.push_back(result);
            } else {
                *found = result;
            }
            const auto path = state->reports / (std::to_string(session->chat) + ".json");
            auto snapshot = session->report.dump(2);
            co_await state->files.run(
                [path, snapshot = std::move(snapshot)] { save_report(path, snapshot); });
            std::cout << result.dump() << '\n';
        }
    }
    if (job == "report" || job == "tour") {
        std::ostringstream report;
        report << "Async showcase results\n";
        if (session->report.empty()) {
            report << "No demos have run yet.";
        }
        for (const auto& result : session->report) {
            report << (result.value("status", std::string{}) == "passed" ? "✅ " : "⚠️ ")
                   << result.value("title", std::string{}) << '\n';
        }
        co_await context.text(report.str());
    }
}

Task<void> Sessions::work(std::shared_ptr<State> state, std::shared_ptr<Session> session) {
    while (!state->closing && !session->jobs.empty()) {
        auto job = std::move(session->jobs.front());
        session->jobs.pop_front();
        session->active = job;
        session->cancel = std::make_shared<asio::cancellation_signal>();
        try {
            co_await asio::co_spawn(
                state->strand,
                execute(state, session, std::move(job)),
                asio::bind_cancellation_slot(session->cancel->slot(), asio::use_awaitable));
        } catch (const std::exception& error) {
            std::cerr << "Async showcase job: " << error.what() << '\n';
        }
        session->cancel.reset();
        session->active.clear();
    }
    // Finish this state transition before another enqueue can observe idle.
    // A delayed completion callback must not clear a newly started worker's jobs.
    session->running = false;
}

Task<void> Sessions::stop_owned(std::shared_ptr<State> state) {
    state->closing = true;
    for (auto& [id, session] : state->sessions) {
        session->jobs.clear();
        if (session->cancel) {
            session->cancel->emit(asio::cancellation_type::all);
        }
    }
    while (std::any_of(state->sessions.begin(), state->sessions.end(), [](const auto& item) {
        return item.second->running;
    })) {
        asio::steady_timer timer(state->strand, 10ms);
        co_await timer.async_wait(asio::use_awaitable);
    }
}

Task<void> Sessions::stop() {
    co_await asio::co_spawn(state_->strand, stop_owned(state_), asio::use_awaitable);
}

Task<void> Sessions::handle(tgpp::ext::AsyncContext& context) {
    const auto* chat = context.chat();
    if (!chat || chat->type != "private") {
        co_return;
    }
    std::string action = "menu";
    if (auto* callback = context.callback_query()) {
        if (callback->from.id != chat->id) {
            co_return;
        }
        co_await context.answer_callback();
        const auto data = callback->data.value_or("");
        if (data == "show:alert") {
            co_await context.reply("Async buttons are responsive while demos run.");
            co_return;
        }
        if (data == "show:stop") {
            action = "stop";
        } else if (data == "show:tour") {
            action = "tour";
        } else if (data == "show:report") {
            action = "report";
        } else if (data.starts_with("show:demo:")) {
            action = data.substr(10);
        } else {
            co_return;
        }
    } else if (auto* message = context.message(); message && message->text) {
        std::istringstream input(*message->text);
        std::string command;
        input >> command;
        if (auto at = command.find('@'); at != std::string::npos) {
            if (state_->bot_username.empty() || command.substr(at + 1) != state_->bot_username) {
                co_return;
            }
            command.resize(at);
        }
        if (command == "/tour") {
            action = "tour";
        } else if (command == "/report") {
            action = "report";
        } else if (command == "/stop") {
            action = "stop";
        } else if (command == "/demo") {
            input >> action;
            if (action.empty()) {
                action = "menu";
            }
        } else if (command != "/showcase" && command != "/start") {
            co_return;
        }
    }
    if (action == "stop") {
        co_await cancel(chat->id);
        co_await context.reply("Your queued demos and active tour were cancelled.");
    } else if (!co_await enqueue(chat->id, std::move(action))) {
        co_await context.reply(
            "Unknown demo, duplicate work, or showcase capacity reached. Use /showcase or /stop.");
    }
}
} // namespace async_showcase
