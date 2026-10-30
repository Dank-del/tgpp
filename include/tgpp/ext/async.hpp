#pragma once
#include "../async_methods.hpp"
#include "polling.hpp"
#include "stores.hpp"
#include <any>
#include <typeindex>
#include <unordered_map>

namespace tgpp::ext {
class AsyncContext {
    const AsyncBot& bot_;
    std::shared_ptr<const Update> update_;
    std::optional<Command> command_;
    std::string handler_;
    std::unordered_map<std::type_index, std::any> data_;
    friend class AsyncApplication;
    friend class AsyncRouter;
    AsyncContext(const AsyncBot& bot,
                 std::shared_ptr<const Update> update,
                 std::string_view username);

  public:
    AsyncContext(const AsyncBot& bot, const Update& update, std::string_view username = {});
    AsyncContext(const AsyncBot& bot, Update&& update, std::string_view username = {});

    const std::optional<Command>& command() const noexcept {
        return command_;
    }

    const User* user() const noexcept;

    const AsyncBot& bot() const noexcept {
        return bot_;
    }

    const Update& update() const noexcept {
        return *update_;
    }

    const Message* message() const noexcept;
    const Chat* chat() const noexcept;
    const CallbackQuery* callback_query() const noexcept;

    std::string_view text() const noexcept {
        const auto* value = message();
        return value && value->text ? std::string_view(*value->text) : std::string_view{};
    }

    const std::vector<std::string>& args() const noexcept {
        static const std::vector<std::string> empty;
        return command_ ? command_->arguments : empty;
    }

    std::string_view arg(std::size_t index) const noexcept {
        const auto& values = args();
        return index < values.size() ? std::string_view(values[index]) : std::string_view{};
    }

    std::string_view callback_data() const noexcept {
        const auto* value = callback_query();
        return value && value->data ? std::string_view(*value->data) : std::string_view{};
    }

    std::string_view handler_name() const noexcept {
        return handler_;
    }

    MessageBuilder say(std::string text) const;
    Task<Message> reply(std::string text) const;
    Task<bool> answer_callback(std::string text = {}, bool alert = false) const;

    template <class T> void set(T value) {
        data_.insert_or_assign(typeid(T), std::move(value));
    }

    template <class T> T* find() {
        auto it = data_.find(typeid(T));
        return it == data_.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    template <class T> const T* find() const {
        auto it = data_.find(typeid(T));
        return it == data_.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    template <class T> T& get() {
        auto* p = find<T>();
        if (!p) {
            throw std::logic_error("missing context metadata");
        }
        return *p;
    }

    template <class T> const T& get() const {
        const auto* p = find<T>();
        if (!p) {
            throw std::logic_error("missing context metadata");
        }
        return *p;
    }
};

using AsyncFilter = std::function<bool(const AsyncContext&)>;
using AsyncCallback = std::function<Task<void>(AsyncContext&)>;
using AsyncErrorHandler = std::function<Task<ErrorAction>(AsyncContext&, std::exception_ptr)>;

class AsyncNext {
    struct State {
        bool active = true, called = false;
        std::function<Task<Result>()> continuation;
    };

    std::shared_ptr<State> state_;

    explicit AsyncNext(std::shared_ptr<State> state) : state_(std::move(state)) {}
    friend class AsyncApplication;
    friend class AsyncRouter;

  public:
    Task<Result> operator()() const;
};

using AsyncMiddleware = std::function<Task<Result>(AsyncContext&, AsyncNext)>;

class AsyncHandler {
  public:
    virtual ~AsyncHandler() = default;
    virtual std::string name() const = 0;
    virtual bool matches(const AsyncContext&) const = 0;
    virtual Task<void> handle(AsyncContext&) = 0;
};

struct AsyncApplicationOptions {
    std::size_t max_active_keys = 64;
    std::size_t queue_capacity = 1024;
    std::string bot_username;
    std::function<std::uint64_t(const Update&)> key;
};

struct AsyncApplicationStats {
    std::size_t queued = 0, active_keys = 0;
    std::uint64_t processed = 0, failed = 0;
};
// Route registration happens before the first process(). All handler frames and
// their contexts remain alive until awaited completion. Never block the executor.
class AsyncApplication;

class AsyncRouter {
    struct Route {
        std::string name;
        AsyncFilter filter;
        AsyncCallback callback;
    };

    std::string name_;
    AsyncFilter filter_;
    std::vector<std::variant<Route, std::shared_ptr<AsyncRouter>>> entries_;
    std::vector<AsyncMiddleware> middleware_;
    bool frozen_ = false;
    friend class AsyncApplication;
    void validate_tree(std::vector<AsyncRouter*>& nodes);
    static Task<Result> dispatch(std::shared_ptr<AsyncRouter>, AsyncContext&, std::string parent);
    static Task<Result> invoke(std::shared_ptr<AsyncRouter>,
                               AsyncContext&,
                               std::string scope,
                               std::size_t middleware_index);

  public:
    explicit AsyncRouter(std::string name = "root", AsyncFilter filter = {});
    void add(std::string name, AsyncFilter, AsyncCallback);
    void include(std::shared_ptr<AsyncRouter>);
    void use(AsyncMiddleware);
};

class AsyncModule {
  public:
    virtual ~AsyncModule() = default;
    virtual void configure(AsyncApplication&) = 0;

    virtual Task<void> start() {
        co_return;
    }

    virtual Task<void> stop() {
        co_return;
    }
};

class AsyncApplication {
    struct State;
    std::shared_ptr<State> state_;
    static Task<Result> invoke(std::shared_ptr<State>, AsyncContext&, std::size_t);
    static Task<void> work(std::shared_ptr<State>, std::uint64_t);
    static void pump(const std::shared_ptr<State>&);
    static Task<Result>
        process_owned(std::shared_ptr<State>, std::shared_ptr<Update>, std::optional<UpdateClaim>);
    friend class AsyncRouter;
    void add_result(std::string, AsyncFilter, std::function<Task<Result>(AsyncContext&)>);

  public:
    AsyncApplication(asio::any_io_executor,
                     std::shared_ptr<AsyncTransport>,
                     AsyncApplicationOptions = {});
    const AsyncBot& bot() const noexcept;
    void set_bot_username(std::string username);
    void add(std::string name, AsyncFilter, AsyncCallback);
    void add(std::shared_ptr<AsyncHandler>);
    void command(std::string name, AsyncCallback);
    void use(AsyncMiddleware);
    void on_error(AsyncErrorHandler);
    void install(std::shared_ptr<AsyncModule>);
    void include(std::shared_ptr<AsyncRouter>);
    Task<Result> process(const Update& update, std::optional<UpdateClaim> claim = {});
    Task<Result> process(Update&& update, std::optional<UpdateClaim> claim = {});
    std::uint64_t ordering_key(const Update& update) const;
    Task<Result> process_json(Json update, std::optional<UpdateClaim> claim = {});
    void request_stop();
    void claim_ingestion(std::string mode);
    Task<void> shutdown(std::chrono::milliseconds grace = std::chrono::seconds(30));
    Task<AsyncApplicationStats> stats() const;
};

struct SessionData {
    Json value = Json::object();
};

AsyncMiddleware sessions(std::shared_ptr<SessionStore>,
                         std::string bot,
                         std::function<std::string(const Update&)> key = {},
                         std::optional<std::chrono::milliseconds> ttl = {});

struct AsyncPollingOptions {
    PollingOptions polling;
    std::shared_ptr<UpdateStore> store;
    std::string bot_identity;
    std::string owner = "tgpp";
    std::chrono::milliseconds lease{30000};
};

class AsyncPolling {
    AsyncApplication& app_;
    AsyncPollingOptions options_;
    std::optional<std::int64_t> offset_;
    std::stop_source stop_;
    Task<std::vector<Update>> retrieve();

  public:
    explicit AsyncPolling(AsyncApplication&, AsyncPollingOptions = {});
    // Runs one claimed durable update. Multiple processors may call this on
    // independent instances sharing a distributed store implementation.
    Task<bool> process_pending();
    Task<std::size_t> poll_once();
    Task<void> run();

    void request_stop() noexcept {
        stop_.request_stop();
    }

    std::optional<std::int64_t> offset() const noexcept {
        return offset_;
    }
};
} // namespace tgpp::ext
