#include <iostream>
#include <optional>
#include <tgpp/ext/async.hpp>
#include <vector>
using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct NoNetwork final : AsyncTransport {
    Task<Json> perform(Request) override {
        throw std::logic_error("unexpected API call");
        co_return Json{};
    }
};

Update update() {
    Update value{};
    value.update_id = 1;
    return value;
}

template <class T> T run(Runtime& runtime, Task<T> task) {
    return asio::co_spawn(runtime.executor(), std::move(task), asio::use_future).get();
}

struct Metadata {
    int value;
};

struct ChildData {
    int value;
};

void scoped_tree() {
    Runtime runtime;
    AsyncApplication app(runtime.executor(), std::make_shared<NoNetwork>());
    auto root = std::make_shared<AsyncRouter>("root");
    std::vector<std::string> trace;
    int root_calls = 0, child_calls = 0;
    root->use([&](AsyncContext& context, AsyncNext next) -> Task<Result> {
        ++root_calls;
        trace.push_back("root-in");
        context.set(Metadata{42});
        auto result = co_await next();
        trace.push_back("root-out");
        co_return result;
    });
    auto skipped = std::make_shared<AsyncRouter>("skipped");
    skipped->use([&](AsyncContext&, AsyncNext) -> Task<Result> {
        trace.push_back("skip");
        co_return Result::unhandled;
    });
    skipped->add(
        "never",
        [](const AsyncContext&) { return true; },
        [](AsyncContext&) -> Task<void> {
            throw std::logic_error("blocked scope ran handler");
            co_return;
        });
    root->include(skipped);
    auto child = std::make_shared<AsyncRouter>("child", [&](const AsyncContext& context) {
        trace.push_back("child-filter");
        return context.find<Metadata>() && context.get<Metadata>().value == 42;
    });
    child->use([&](AsyncContext& context, AsyncNext next) -> Task<Result> {
        ++child_calls;
        trace.push_back("child-in");
        context.set(ChildData{7});
        auto result = co_await next();
        trace.push_back("child-out");
        co_return result;
    });
    child->add(
        "rejected",
        [&](const AsyncContext& context) {
            trace.push_back("first-filter");
            require(context.get<ChildData>().value == 7,
                    "scoped metadata unavailable to first filter");
            return false;
        },
        [](AsyncContext&) -> Task<void> {
            throw std::logic_error("rejected handler ran");
            co_return;
        });
    child->add(
        "accepted",
        [&](const AsyncContext& context) {
            trace.push_back("second-filter");
            return context.get<ChildData>().value == 7;
        },
        [&](AsyncContext& context) -> Task<void> {
            trace.push_back("handler");
            require(context.handler_name() == "root/child/accepted", "full leaf handler path lost");
            co_return;
        });
    root->include(child);
    app.include(root);
    app.add(
        "fallback",
        [](const AsyncContext&) { return true; },
        [](AsyncContext&) -> Task<void> {
            throw std::logic_error("handled tree fell through");
            co_return;
        });
    require(run(runtime, app.process(update())) == Result::handled, "tree did not handle update");
    require(root_calls == 1 && child_calls == 1, "middleware repeated per leaf");
    require(trace == std::vector<std::string>{"root-in",
                                              "skip",
                                              "child-filter",
                                              "child-in",
                                              "first-filter",
                                              "second-filter",
                                              "handler",
                                              "child-out",
                                              "root-out"},
            "nested scope ordering");
    bool frozen = false;
    try {
        child->use([](AsyncContext&, AsyncNext) -> Task<Result> { co_return Result::unhandled; });
    } catch (const std::logic_error&) {
        frozen = true;
    }
    require(frozen, "installed child tree remained mutable");
    frozen = false;
    try {
        app.set_bot_username("later_bot");
    } catch (const std::logic_error&) {
        frozen = true;
    }
    require(frozen, "started identity remained mutable");
    run(runtime, app.shutdown());
}

void fallback_and_next() {
    Runtime runtime;
    AsyncApplication app(runtime.executor(), std::make_shared<NoNetwork>());
    auto root = std::make_shared<AsyncRouter>("empty");
    std::optional<AsyncNext> retained;
    int calls = 0;
    root->use([&](AsyncContext&, AsyncNext next) -> Task<Result> {
        retained = next;
        ++calls;
        require(co_await next() == Result::unhandled, "empty scope should be unhandled");
        bool twice = false;
        try {
            co_await next();
        } catch (const std::logic_error&) {
            twice = true;
        }
        require(twice, "scope continuation ran twice");
        co_return Result::unhandled;
    });
    app.include(root);
    bool fallback = false;
    app.add(
        "fallback",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext& context) -> Task<void> {
            fallback = true;
            require(context.handler_name() == "fallback",
                    "fallback location polluted by unhandled scope");
            co_return;
        });
    require(run(runtime, app.process(update())) == Result::handled && fallback && calls == 1,
            "unhandled scope prevented fallback");
    bool expired = false;
    try {
        run(runtime, (*retained)());
    } catch (const std::logic_error&) {
        expired = true;
    }
    require(expired, "retained scoped Next outlived dispatch");
    run(runtime, app.shutdown());
}

void rejected_scope_and_error_location() {
    Runtime runtime;
    AsyncApplication app(runtime.executor(), std::make_shared<NoNetwork>());
    auto rejected =
        std::make_shared<AsyncRouter>("rejected", [](const AsyncContext&) { return false; });
    rejected->use([](AsyncContext&, AsyncNext) -> Task<Result> {
        throw std::logic_error("rejected scope ran middleware");
        co_return Result::failed;
    });
    app.include(rejected);
    auto root = std::make_shared<AsyncRouter>("failing");
    root->add(
        "leaf",
        [](const AsyncContext&) { return true; },
        [](AsyncContext&) -> Task<void> {
            throw std::runtime_error("test failure");
            co_return;
        });
    app.include(root);
    std::string location;
    app.on_error([&](AsyncContext& context, std::exception_ptr) -> Task<ErrorAction> {
        location = context.handler_name();
        co_return ErrorAction::skip;
    });
    require(run(runtime, app.process(update())) == Result::handled, "error skip failed");
    require(location == "failing/leaf", "nested failure location lost");
    run(runtime, app.shutdown());
}

void invalid_tree() {
    auto one = std::make_shared<AsyncRouter>("one");
    auto two = std::make_shared<AsyncRouter>("two");
    one->include(two);
    bool rejected = false;
    try {
        two->include(one);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "cyclic tree accepted");
    Runtime runtime;
    AsyncApplication app(runtime.executor(), std::make_shared<NoNetwork>());
    app.include(one);
    rejected = false;
    try {
        app.include(one);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "router shared between installed roots");
    run(runtime, app.shutdown());
}

void context_helpers() {
    auto transport = std::make_shared<NoNetwork>();
    AsyncBot bot(transport);
    Update message_update{};
    message_update.update_id = 1;
    Message message{};
    message.message_id = 1;
    message.date = 1;
    message.chat.id = 42;
    message.chat.type = "private";
    message.text = "/hello@sample_bot one two";
    MessageEntity command{};
    command.type = "bot_command";
    command.offset = 0;
    command.length = 17;
    message.entities = std::vector<MessageEntity>{command};
    message_update.message = message;
    AsyncContext context(bot, std::move(message_update), "sample_bot");
    require(context.text() == "/hello@sample_bot one two", "context text accessor");
    require(context.args().size() == 2 && context.arg(0) == "one" && context.arg(1) == "two" &&
                context.arg(2).empty(),
            "context argument accessors");
    require(context.callback_data().empty(), "noncallback data must be empty");
    Update callback_update{};
    callback_update.update_id = 2;
    CallbackQuery callback{};
    callback.id = "query";
    callback.from.id = 42;
    callback.from.is_bot = false;
    callback.from.first_name = "Person";
    callback.chat_instance = "chat";
    callback.data = "choose:42";
    callback_update.callback_query = callback;
    AsyncContext query(bot, std::move(callback_update));
    require(query.callback_data() == "choose:42" && query.text().empty() && query.args().empty(),
            "callback context helpers");
}
} // namespace

int main() {
    try {
        scoped_tree();
        fallback_and_next();
        rejected_scope_and_error_location();
        invalid_tree();
        context_helpers();
        std::cout
            << "Async router tree, metadata, fallback, continuation and helper checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
