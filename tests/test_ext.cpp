#include <tgpp/ext/ext.hpp>
#include <iostream>
#include <limits>

using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class F> void rejects(F function) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    require(threw, "expected exception");
}
struct MockTransport : Transport {
    std::vector<Request> requests;
    std::function<Json(const Request&)> respond = [](const Request& request) -> Json {
        if (request.method == "sendMessage")
            return {{"ok", true}, {"result", {{"message_id", 2}, {"date", 1}, {"chat", {{"id", 42}, {"type", "private"}}}}}};
        return {{"ok", true}, {"result", true}};
    };
    Json perform(const Request& request) override { requests.push_back(request); return respond(request); }
};
Update text_update(std::string text, std::int64_t id = 1) {
    Message message{};
    message.message_id = 10; message.date = 1;
    message.chat.id = 42; message.chat.type = "private";
    User user{}; user.id = 7; user.first_name = "Alice";
    message.from = user;
    message.text = std::move(text);
    if (!message.text->empty() && message.text->front() == '/') {
        const auto end = message.text->find_first_of(" \t\r\n");
        MessageEntity entity{}; entity.type = "bot_command";
        entity.length = std::int64_t(end == std::string::npos ? message.text->size() : end);
        message.entities = std::vector<MessageEntity>{entity};
    }
    Update update{}; update.update_id = id; update.message = std::move(message);
    return update;
}
struct CountingCommand final : CommandHandler {
    int& count;
    CountingCommand(std::string name, int& count) : CommandHandler(std::move(name)), count(count) {}
    void handle(Context&) override { ++count; }
};
struct TraceMiddleware final : Middleware {
    std::vector<std::string>& events;
    std::string label;
    TraceMiddleware(std::vector<std::string>& events, std::string label) : events(events), label(std::move(label)) {}
    Result handle(Context&, Next next) override {
        events.push_back(label + ":before");
        auto result = next();
        events.push_back(label + ":after");
        return result;
    }
};
struct Block final : Middleware {
    Result handle(Context&, Next) override { return Result::stopped; }
};
struct Actor { int id; };
struct Authenticate final : Middleware {
    Result handle(Context& context, Next next) override { context.set(Actor{99}); return next(); }
};
struct SaveNext final : Middleware {
    std::optional<Next>& saved;
    explicit SaveNext(std::optional<Next>& saved) : saved(saved) {}
    Result handle(Context&, Next next) override { saved = next; return next(); }
};
struct DoubleNext final : Middleware {
    Result handle(Context&, Next next) override { next(); return next(); }
};
struct LifecycleModule final : Module {
    std::string label;
    std::vector<std::string>& events;
    bool fail;
    LifecycleModule(std::string label, std::vector<std::string>& events, bool fail = false)
        : label(std::move(label)), events(events), fail(fail) {}
    std::string name() const override { return label; }
    void configure(Router&) override { events.push_back(label + ":configure"); }
    void start() override { events.push_back(label + ":start"); if (fail) throw std::runtime_error("startup failed"); }
    void stop() noexcept override { events.push_back(label + ":stop"); }
};

void context_and_commands() {
    auto mock = std::make_shared<MockTransport>(); Bot bot(mock);
    auto update = text_update("/AdD@Example_Bot   buy milk 🥛  ");
    Context context(bot, update, "example_bot");
    require(context.command() && context.command()->name == "add", "command normalization");
    require(context.command()->argument_text == "buy milk 🥛", "Unicode arguments must remain intact");
    require(context.command()->arguments.size() == 3, "argument splitting");
    require(context.user()->id == 7 && context.chat()->id == 42, "context actor and chat");
    require(filters::both(filters::text(), filters::private_chat())(context), "combined filter");
    require(filters::either(filters::command("missing"), filters::command("add"))(context), "either filter");
    require(!filters::negate(filters::text())(context), "negative filter");
    Context other(bot, update, "other_bot");
    require(!other.command(), "do not run commands addressed to a different bot");
    Context unknown(bot, update);
    require(!unknown.command(), "addressed commands need an identity");
    update.message->entities.reset();
    Context unmarked(bot, update, "example_bot");
    require(!unmarked.command(), "literal slash text without an entity is not a command");
    auto empty = text_update("");
    Context no_text(bot, empty);
    require(!no_text.command(), "empty message cannot be a command");
    auto wrong_boundary = text_update("/startmore");
    wrong_boundary.message->entities->at(0).length = 6;
    Context truncated(bot, wrong_boundary);
    require(!truncated.command(), "command entity must end at a token boundary");

    update = text_update("/start hello");
    update.message->message_thread_id = 123;
    update.message->business_connection_id = "business";
    Context reply(bot, update);
    reply.reply("welcome");
    const auto& parameters = mock->requests.back().parameters;
    require(parameters.at("chat_id") == 42 && parameters.at("text") == "welcome", "reply helper");
    require(parameters.at("message_thread_id") == 123 && parameters.at("business_connection_id") == "business", "reply scope");
    require(parameters.at("reply_parameters").at("message_id") == 10, "reply reference");

    auto callback_update = Update{};
    CallbackQuery callback{}; callback.id = "query"; callback.from.id = 8;
    callback.data = "tasks:complete:1";
    MaybeInaccessibleMessage accessible{}; accessible.value = Box<Message>(*update.message);
    callback.message = accessible; callback_update.callback_query = callback;
    Context clicked(bot, callback_update);
    require(!clicked.command() && !clicked.message(), "callback messages do not trigger message commands");
    require(clicked.user()->id == 8 && clicked.chat()->id == 42, "callback actor and chat");
    require(filters::callback_prefix("tasks:")(clicked), "callback prefix routing");
    clicked.answer_callback("done");
    require(mock->requests.back().method == "answerCallbackQuery", "callback acknowledgement");
    require(mock->requests.back().parameters.at("callback_query_id") == "query", "callback query ID");
    rejects([&] { reply.answer_callback(); });
    rejects([] { filters::command("bad command"); });
}

void routing_and_middleware() {
    auto mock = std::make_shared<MockTransport>();
    Application app(mock);
    std::vector<std::string> events;
    auto& root = app.router();
    root.use<TraceMiddleware>(events, "global");
    auto& tasks = root.child("tasks", filters::private_chat());
    tasks.use<TraceMiddleware>(events, "feature");
    tasks.use<Authenticate>();
    tasks.add<FunctionHandler>("add", filters::command("add"), [&](Context& context) {
        require(context.get<Actor>().id == 99, "typed middleware data");
        require(context.route() == "root/tasks" && context.handler_name() == "add", "route names");
        events.push_back("handler");
    });
    int fallback = 0;
    root.add<FunctionHandler>("fallback", filters::all(), [&](Context& context) {
        require(!context.find<Actor>(), "feature metadata must not be present on a different update"); ++fallback;
    });
    require(app.process(text_update("/add")) == Result::handled, "nested routing");
    require(events == std::vector<std::string>{"global:before", "feature:before", "handler", "feature:after", "global:after"}, "middleware nesting order");
    auto group = text_update("hello"); group.message->chat.type = "supergroup";
    require(app.process(group) == Result::handled && fallback == 1, "feature filter fallthrough");
    rejects([&] { root.child("late"); });
    rejects([&] { tasks.use<Block>(); });

    Application blocked(mock); int called = 0;
    blocked.router().use<Block>();
    blocked.router().add<CountingCommand>("start", called);
    require(blocked.process(text_update("/start")) == Result::stopped && called == 0, "middleware short circuit");

    Application saved(mock); std::optional<Next> continuation;
    saved.router().use<SaveNext>(continuation);
    saved.router().add<CountingCommand>("start", called);
    saved.process(text_update("/start"));
    rejects([&] { (*continuation)(); });
    Application doubled(mock);
    doubled.router().use<DoubleNext>();
    doubled.router().add<CountingCommand>("start", called);
    const int before = called;
    rejects([&] { doubled.process(text_update("/start")); });
    require(called == before + 1, "next must never dispatch twice");

    Application ordered(mock); int first = 0, second = 0;
    ordered.router().add<CountingCommand>("start", first);
    ordered.router().add<CountingCommand>("start", second);
    ordered.process(text_update("/start"));
    require(first == 1 && second == 0, "first matching handler must win");
}

void lifecycle_and_errors() {
    auto mock = std::make_shared<MockTransport>();
    std::vector<std::string> events;
    {
        Application app(mock);
        app.install<LifecycleModule>("a", events);
        app.install<LifecycleModule>("b", events);
        rejects([&] { app.install<LifecycleModule>("b", events); });
        app.start(); app.start(); app.stop(); app.stop();
        rejects([&] { app.start(); });
    }
    require(events == std::vector<std::string>{"a:configure", "b:configure", "a:start", "b:start", "b:stop", "a:stop"}, "lifecycle start and reverse cleanup");
    events.clear();
    {
        Application app(mock);
        app.install<LifecycleModule>("a", events);
        app.install<LifecycleModule>("b", events, true);
        rejects([&] { app.start(); });
    }
    require(events == std::vector<std::string>{"a:configure", "b:configure", "a:start", "b:start", "b:stop", "a:stop"}, "startup failure cleanup");

    Application app(mock);
    app.router().child("broken").add<FunctionHandler>("explode", filters::all(), [](Context&) { throw std::runtime_error("failure"); });
    app.on_error([](Context& context, std::exception_ptr error) {
        require(context.route() == "root/broken" && context.handler_name() == "explode", "failure location");
        try { std::rethrow_exception(error); } catch (const std::runtime_error& e) { require(std::string(e.what()) == "failure", "original exception"); }
        return ErrorAction::skip;
    });
    require(app.process(text_update("hello")) == Result::failed, "explicit skip result");
    Application recursive(mock);
    recursive.router().add<FunctionHandler>("recursive", filters::all(), [&](Context& context) { recursive.process(context.update()); });
    rejects([&] { recursive.process(text_update("hello")); });

    events.clear();
    Application stopped(mock);
    stopped.install<LifecycleModule>("feature", events);
    stopped.router().add<FunctionHandler>("stop", filters::all(), [&](Context&) {
        stopped.stop();
        require(stopped.is_running(), "shutdown must wait for the current handler");
    });
    stopped.process(text_update("hello"));
    require(!stopped.is_running() && events.back() == "feature:stop", "deferred shutdown must complete after dispatch");
}

void polling_checkpoints() {
    auto mock = std::make_shared<MockTransport>();
    auto first = text_update("/ok", 10), second = text_update("/bad", 11);
    mock->respond = [first, second](const Request&) -> Json {
        return {{"ok", true}, {"result", Json::array({to_json(first), to_json(second)})}};
    };
    Application app(mock);
    int handled = 0; bool fail = true;
    app.router().add<FunctionHandler>("process", filters::all(), [&](Context& context) {
        if (context.update().update_id == 11 && fail) throw std::runtime_error("handler failed");
        ++handled;
    });
    Polling polling(app);
    rejects([&] { polling.poll_once(); });
    require(polling.offset() == 11 && handled == 1, "failed update must not advance checkpoint");
    fail = false;
    require(polling.poll_once() == 1 && handled == 2 && polling.offset() == 12, "resume checkpoint without replaying confirmed updates");
    require(mock->requests.back().parameters.at("offset") == 11, "getUpdates checkpoint");
    polling.request_stop();
    const auto calls = mock->requests.size();
    require(polling.poll_once() == 0 && mock->requests.size() == calls, "stop must prevent fetching");

    Application skipped(mock);
    skipped.router().add<FunctionHandler>("bad", filters::all(), [](Context&) { throw std::runtime_error("skip"); });
    skipped.on_error([](Context&, std::exception_ptr) { return ErrorAction::skip; });
    Polling skip_polling(skipped);
    require(skip_polling.poll_once() == 2 && skip_polling.offset() == 12, "explicit skip confirms failed update");
    PollingOptions options; options.request.limit = 101;
    rejects([&] { Polling invalid(skipped, options); });
    options.request.limit = 100; options.request.offset = -1;
    rejects([&] { Polling invalid(skipped, options); });
}
} // namespace

int main() {
    try {
        context_and_commands(); routing_and_middleware(); lifecycle_and_errors(); polling_checkpoints();
        std::cout << "Framework routing, commands, middleware, lifecycle, errors, and polling passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
