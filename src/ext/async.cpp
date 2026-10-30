#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/retry.hpp>
#include <tgpp/ext/stores.hpp>

namespace tgpp::ext {
namespace {
bool command_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return value;
}

std::optional<Command> parse_command(const Message* message, std::string_view username) {
    if (!message || !message->text || !message->entities) {
        return {};
    }
    const auto& text = *message->text;
    for (const auto& entity : *message->entities) {
        if (entity.type != "bot_command" || entity.offset != 0 || entity.length < 2) {
            continue;
        }
        if (std::uint64_t(entity.length) > text.size() || text[0] != '/') {
            return {};
        }
        if (std::uint64_t(entity.length) < text.size() &&
            text.find_first_of(" \t\r\n", static_cast<std::size_t>(entity.length)) !=
                static_cast<std::size_t>(entity.length)) {
            return {};
        }
        // Command tokens are ASCII: their byte length equals their UTF-16
        // length. Reject non-ASCII tokens instead of slicing Unicode by bytes.
        const auto token = text.substr(1, static_cast<std::size_t>(entity.length) - 1);
        const auto at = token.find('@');
        auto name = token.substr(0, at);
        if (name.empty() || name.size() > 32 ||
            !std::all_of(name.begin(), name.end(), command_char)) {
            return {};
        }
        if (at != std::string::npos) {
            const auto target = token.substr(at + 1);
            auto own = std::string(username);
            if (!own.empty() && own.front() == '@') {
                own.erase(0, 1);
            }
            if (target.empty() || !std::all_of(target.begin(), target.end(), command_char) ||
                own.empty() || lower(target) != lower(own)) {
                return {};
            }
        }
        auto arguments = text.substr(static_cast<std::size_t>(entity.length));
        auto first = arguments.find_first_not_of(" \t\r\n");
        arguments =
            first == std::string::npos
                ? ""
                : arguments.substr(first, arguments.find_last_not_of(" \t\r\n") - first + 1);
        Command command{lower(name), arguments, {}};
        std::istringstream stream(arguments);
        for (std::string argument; stream >> argument;) {
            command.arguments.push_back(std::move(argument));
        }
        return command;
    }
    return {};
}

} // namespace

AsyncContext::AsyncContext(const AsyncBot& bot, const Update& update, std::string_view username)
    : AsyncContext(bot, std::make_shared<Update>(update), username) {}

AsyncContext::AsyncContext(const AsyncBot& bot, Update&& update, std::string_view username)
    : AsyncContext(bot, std::make_shared<Update>(std::move(update)), username) {}

AsyncContext::AsyncContext(const AsyncBot& bot,
                           std::shared_ptr<const Update> update,
                           std::string_view username)
    : bot_(bot), update_(std::move(update)) {
    if (!update_) {
        throw std::invalid_argument("update must not be null");
    }
    command_ = parse_command(message(), username);
}

const User* AsyncContext::user() const noexcept {
    if (auto* q = callback_query()) {
        return &q->from;
    }
    if (auto* m = message()) {
        return m->from ? &*m->from : nullptr;
    }
    if (update_->inline_query) {
        return &update_->inline_query->from;
    }
    if (update_->chosen_inline_result) {
        return &update_->chosen_inline_result->from;
    }
    if (update_->shipping_query) {
        return &update_->shipping_query->from;
    }
    if (update_->pre_checkout_query) {
        return &update_->pre_checkout_query->from;
    }
    if (update_->chat_member) {
        return &update_->chat_member->from;
    }
    if (update_->my_chat_member) {
        return &update_->my_chat_member->from;
    }
    if (update_->chat_join_request) {
        return &update_->chat_join_request->from;
    }
    return nullptr;
}

const Message* AsyncContext::message() const noexcept {
    for (const auto* item : {&update_->message,
                             &update_->edited_message,
                             &update_->channel_post,
                             &update_->edited_channel_post,
                             &update_->business_message,
                             &update_->edited_business_message,
                             &update_->guest_message}) {
        if (*item) {
            return &**item;
        }
    }
    return nullptr;
}

const CallbackQuery* AsyncContext::callback_query() const noexcept {
    return update_->callback_query ? &*update_->callback_query : nullptr;
}

const Chat* AsyncContext::chat() const noexcept {
    if (auto* m = message()) {
        return &m->chat;
    }
    if (auto* q = callback_query(); q && q->message) {
        if (auto* m = std::get_if<Box<Message>>(&q->message->value); m && *m) {
            return &(**m).chat;
        }
        if (auto* m = std::get_if<InaccessibleMessage>(&q->message->value)) {
            return &m->chat;
        }
    }
    if (update_->chat_member) {
        return &update_->chat_member->chat;
    }
    if (update_->my_chat_member) {
        return &update_->my_chat_member->chat;
    }
    if (update_->chat_join_request) {
        return &update_->chat_join_request->chat;
    }
    return nullptr;
}

MessageBuilder AsyncContext::say(std::string text) const {
    auto* target = chat();
    if (!target) {
        throw std::logic_error("update has no reply chat");
    }
    SendMessageRequest request{};
    request.chat_id = target->id;
    request.text = std::move(text);
    auto* source = message();
    if (!source) {
        if (auto* q = callback_query(); q && q->message) {
            if (auto* p = std::get_if<Box<Message>>(&q->message->value); p && *p) {
                source = &**p;
            }
        }
    }
    if (source) {
        if (source->guest_query_id || source->ephemeral_message_id) {
            throw std::logic_error("use explicit API for guest or ephemeral reply");
        }
        request.message_thread_id = source->message_thread_id;
        request.business_connection_id = source->business_connection_id;
        if (source->direct_messages_topic) {
            request.direct_messages_topic_id = source->direct_messages_topic->topic_id;
        }
        if (source->message_id > 0) {
            ReplyParameters reply{};
            reply.message_id = source->message_id;
            request.reply_parameters = std::move(reply);
        }
    }
    auto builder = bot_.to(target->id).text("");
    builder.request(std::move(request));
    return builder;
}

Task<Message> AsyncContext::reply(std::string text) const {
    return say(std::move(text)).send();
}

Task<bool> AsyncContext::answer_callback(std::string text, bool alert) const {
    auto* q = callback_query();
    if (!q) {
        throw std::logic_error("update has no callback query");
    }
    AnswerCallbackQueryRequest request{};
    request.callback_query_id = q->id;
    request.text = std::move(text);
    request.show_alert = alert;
    co_return co_await bot_.answer_callback_query(std::move(request));
}

Task<Result> AsyncNext::operator()() const {
    if (!state_->active || state_->called) {
        throw std::logic_error("async middleware continuation is inactive or already called");
    }
    state_->called = true;
    co_return co_await state_->continuation();
}

struct AsyncApplication::State {
    struct Route {
        std::string name;
        AsyncFilter filter;
        std::function<Task<Result>(AsyncContext&)> callback;
    };

    struct Job {
        std::shared_ptr<Update> update;
        std::optional<UpdateClaim> claim;
        std::function<void(std::exception_ptr, Result)> complete;
    };

    struct Lane {
        std::deque<Job> jobs;
        bool active = false;
        std::shared_ptr<asio::cancellation_signal> cancellation;
    };

    asio::strand<asio::any_io_executor> strand;
    AsyncBot bot;
    AsyncApplicationOptions options;
    std::vector<Route> routes;
    std::vector<AsyncMiddleware> middleware;
    AsyncErrorHandler error;
    std::vector<std::shared_ptr<AsyncModule>> modules;
    std::size_t started_modules = 0;
    bool starting = false, started = false;
    std::exception_ptr startup_error;
    std::map<std::uint64_t, Lane> lanes;
    std::deque<std::uint64_t> ready;
    AsyncApplicationStats stats;
    bool frozen = false, closing = false;
    std::mutex ingestion_mutex;
    std::string ingestion;

    State(asio::any_io_executor ex, std::shared_ptr<AsyncTransport> t, AsyncApplicationOptions o)
        : strand(asio::make_strand(ex)), bot(std::move(t)), options(std::move(o)) {}
};

AsyncApplication::AsyncApplication(asio::any_io_executor ex,
                                   std::shared_ptr<AsyncTransport> t,
                                   AsyncApplicationOptions o)
    : state_(std::make_shared<State>(ex, std::move(t), std::move(o))) {
    if (!state_->options.max_active_keys || !state_->options.queue_capacity) {
        throw std::invalid_argument("async dispatch limits must be positive");
    }
    if (!state_->options.key) {
        state_->options.key = chat_ordering_key;
    }
}

const AsyncBot& AsyncApplication::bot() const noexcept {
    return state_->bot;
}

void AsyncApplication::add(std::string name, AsyncFilter filter, AsyncCallback callback) {
    if (!callback) {
        throw std::invalid_argument("empty async callback");
    }
    add_result(std::move(name),
               std::move(filter),
               [callback = std::move(callback)](AsyncContext& c) -> Task<Result> {
                   co_await callback(c);
                   co_return Result::handled;
               });
}

void AsyncApplication::add_result(std::string name,
                                  AsyncFilter filter,
                                  std::function<Task<Result>(AsyncContext&)> callback) {
    if (state_->frozen) {
        throw std::logic_error("async application is frozen");
    }
    if (!filter || !callback || name.empty()) {
        throw std::invalid_argument("async route needs name, filter and callback");
    }
    state_->routes.push_back({std::move(name), std::move(filter), std::move(callback)});
}

void AsyncApplication::add(std::shared_ptr<AsyncHandler> h) {
    if (!h) {
        throw std::invalid_argument("null async handler");
    }
    add(
        h->name(),
        [h](const AsyncContext& c) { return h->matches(c); },
        [h](AsyncContext& c) -> Task<void> { co_await h->handle(c); });
}

void AsyncApplication::command(std::string name, AsyncCallback callback) {
    if (name.empty()) {
        throw std::invalid_argument("command name is empty");
    }
    name = lower(std::move(name));
    add(
        name,
        [name](const AsyncContext& context) {
            return context.command() && context.command()->name == name;
        },
        std::move(callback));
}

void AsyncApplication::use(AsyncMiddleware middleware) {
    if (state_->frozen) {
        throw std::logic_error("async application is frozen");
    }
    if (!middleware) {
        throw std::invalid_argument("empty middleware");
    }
    state_->middleware.push_back(std::move(middleware));
}

void AsyncApplication::on_error(AsyncErrorHandler error) {
    if (state_->frozen) {
        throw std::logic_error("async application is frozen");
    }
    state_->error = std::move(error);
}

void AsyncApplication::install(std::shared_ptr<AsyncModule> module) {
    if (state_->frozen) {
        throw std::logic_error("async application is frozen");
    }
    if (!module) {
        throw std::invalid_argument("null async module");
    }
    module->configure(*this);
    state_->modules.push_back(std::move(module));
}

Task<Result>
AsyncApplication::invoke(std::shared_ptr<State> state, AsyncContext& context, std::size_t index) {
    if (index == state->middleware.size()) {
        for (auto& route : state->routes) {
            if (route.filter(context)) {
                context.handler_ = route.name;
                auto result = co_await route.callback(context);
                if (result != Result::unhandled) {
                    co_return result;
                }
            }
        }
        co_return Result::unhandled;
    }
    auto next = std::make_shared<AsyncNext::State>();
    next->continuation = [state, &context, index]() { return invoke(state, context, index + 1); };
    try {
        auto result = co_await state->middleware[index](context, AsyncNext(next));
        next->active = false;
        co_return result;
    } catch (...) {
        next->active = false;
        throw;
    }
}

void AsyncApplication::pump(const std::shared_ptr<State>& state) {
    while (state->stats.active_keys < state->options.max_active_keys && !state->ready.empty()) {
        auto key = state->ready.front();
        state->ready.pop_front();
        state->lanes.at(key).active = true;
        ++state->stats.active_keys;
        auto cancellation = std::make_shared<asio::cancellation_signal>();
        state->lanes.at(key).cancellation = cancellation;
        asio::co_spawn(state->strand,
                       work(state, key),
                       asio::bind_cancellation_slot(
                           cancellation->slot(), [state, cancellation](std::exception_ptr error) {
                               if (error) {
                                   std::terminate();
                               }
                           }));
    }
}

Task<void> AsyncApplication::work(std::shared_ptr<State> state, std::uint64_t key) {
    auto& lane = state->lanes.at(key);
    auto job = std::move(lane.jobs.front());
    lane.jobs.pop_front();
    --state->stats.queued;
    AsyncContext context(state->bot, std::move(job.update), state->options.bot_username);
    if (job.claim) {
        context.set(*job.claim);
    }
    std::exception_ptr error;
    Result result = Result::failed;
    try {
        if (!state->starting && !state->started) {
            state->starting = true;
            try {
                for (auto& module : state->modules) {
                    co_await module->start();
                    ++state->started_modules;
                }
            } catch (...) {
                state->startup_error = std::current_exception();
            }
            state->started = true;
            state->starting = false;
        }
        while (state->starting) {
            asio::steady_timer timer(state->strand, std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        if (state->startup_error) {
            std::rethrow_exception(state->startup_error);
        }
        result = co_await invoke(state, context, 0);
    } catch (...) {
        error = std::current_exception();
    }
    if (error && state->error) {
        try {
            if (co_await state->error(context, error) == ErrorAction::skip) {
                error = {};
                result = Result::handled;
            }
        } catch (...) {
            error = std::current_exception();
        }
    }
    if (error) {
        ++state->stats.failed;
    } else {
        ++state->stats.processed;
    }
    job.complete(error, result);
    --state->stats.active_keys;
    if (lane.jobs.empty()) {
        state->lanes.erase(key);
    } else {
        lane.active = false;
        state->ready.push_back(key);
    }
    pump(state);
    co_return;
}

Task<Result> AsyncApplication::process_owned(std::shared_ptr<State> state,
                                             std::shared_ptr<Update> update,
                                             std::optional<UpdateClaim> claim) {
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Result)>(
        [state, update = std::move(update), claim = std::move(claim)](auto handler) mutable {
            auto ex = asio::get_associated_executor(handler, state->strand);
            auto owned = std::make_shared<decltype(handler)>(std::move(handler));
            auto complete = [owned, ex](std::exception_ptr error, Result result) {
                asio::post(ex, [owned, error, result]() mutable { (*owned)(error, result); });
            };
            asio::post(state->strand,
                       [state,
                        update = std::move(update),
                        claim = std::move(claim),
                        complete = std::move(complete)]() mutable {
                           state->frozen = true;
                           if (state->closing) {
                               complete(std::make_exception_ptr(CancelledError()), Result::failed);
                               return;
                           }
                           if (state->stats.queued >= state->options.queue_capacity) {
                               complete(std::make_exception_ptr(OverloadError()), Result::failed);
                               return;
                           }
                           std::uint64_t key;
                           try {
                               key = state->options.key(*update);
                           } catch (...) {
                               complete(std::current_exception(), Result::failed);
                               return;
                           }
                           auto [it, created] = state->lanes.try_emplace(key);
                           it->second.jobs.push_back(
                               {std::move(update), std::move(claim), std::move(complete)});
                           ++state->stats.queued;
                           if (created) {
                               state->ready.push_back(key);
                           }
                           pump(state);
                       });
        },
        token);
}

Task<Result> AsyncApplication::process(const Update& update, std::optional<UpdateClaim> claim) {
    return process_owned(state_, std::make_shared<Update>(update), std::move(claim));
}

Task<Result> AsyncApplication::process(Update&& update, std::optional<UpdateClaim> claim) {
    return process_owned(state_, std::make_shared<Update>(std::move(update)), std::move(claim));
}

std::uint64_t AsyncApplication::ordering_key(const Update& update) const {
    return state_->options.key(update);
}

Task<Result> AsyncApplication::process_json(Json update, std::optional<UpdateClaim> claim) {
    auto owned = std::make_shared<Update>();
    Codec<Update>::decode(update, *owned);
    return process_owned(state_, std::move(owned), std::move(claim));
}

void AsyncApplication::claim_ingestion(std::string mode) {
    if (mode != "polling" && mode != "webhook") {
        throw std::invalid_argument("invalid ingestion mode");
    }
    std::lock_guard lock(state_->ingestion_mutex);
    if (!state_->ingestion.empty() && state_->ingestion != mode) {
        throw std::logic_error("polling and webhooks cannot share an application");
    }
    state_->ingestion = std::move(mode);
}

void AsyncApplication::request_stop() {
    auto state = state_;
    asio::post(state->strand, [state] { state->closing = true; });
}

Task<AsyncApplicationStats> AsyncApplication::stats() const {
    auto state = state_;
    co_return co_await asio::co_spawn(
        state->strand,
        [](std::shared_ptr<State> s) -> Task<AsyncApplicationStats> { co_return s->stats; }(state),
        asio::use_awaitable);
}

Task<void> AsyncApplication::shutdown(std::chrono::milliseconds grace) {
    request_stop();
    auto deadline = std::chrono::steady_clock::now() + grace;
    auto ex = co_await asio::this_coro::executor;
    while (true) {
        auto snapshot = co_await stats();
        if (!snapshot.active_keys && !snapshot.queued) {
            auto state = state_;
            co_await asio::co_spawn(
                state->strand,
                [](std::shared_ptr<State> s) -> Task<void> {
                    while (s->started_modules) {
                        auto module = s->modules[--s->started_modules];
                        co_await module->stop();
                    }
                }(state),
                asio::use_awaitable);
            co_return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            auto state = state_;
            co_await asio::co_spawn(
                state->strand,
                [](std::shared_ptr<State> s) -> Task<void> {
                    s->ready.clear();
                    for (auto it = s->lanes.begin(); it != s->lanes.end();) {
                        auto& lane = it->second;
                        for (auto& job : lane.jobs) {
                            job.complete(std::make_exception_ptr(CancelledError()), Result::failed);
                            --s->stats.queued;
                        }
                        lane.jobs.clear();
                        if (lane.active) {
                            lane.cancellation->emit(asio::cancellation_type::all);
                            ++it;
                        } else {
                            it = s->lanes.erase(it);
                        }
                    }
                    co_return;
                }(state),
                asio::use_awaitable);
            throw TimeoutError();
        }
        asio::steady_timer timer(ex, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
}
} // namespace tgpp::ext

namespace tgpp::ext {
AsyncMiddleware sessions(std::shared_ptr<SessionStore> store,
                         std::string bot,
                         std::function<std::string(const Update&)> key,
                         std::optional<std::chrono::milliseconds> ttl) {
    if (!store || bot.empty() || (ttl && ttl->count() <= 0)) {
        throw std::invalid_argument("invalid session middleware options");
    }
    if (!key) {
        key = [](const Update& update) { return std::to_string(chat_ordering_key(update)); };
    }
    return [store = std::move(store), bot = std::move(bot), key = std::move(key), ttl](
               AsyncContext& context, AsyncNext next) -> Task<Result> {
        SessionKey id{bot, key(context.update())};
        auto record = co_await store->load(id);
        context.set(SessionData{record ? record->value : Json::object()});
        auto result = co_await next();
        if (result == Result::failed) {
            co_return result;
        }
        std::optional<std::int64_t> expiry;
        if (ttl) {
            expiry = store_time_ms() + ttl->count();
        }
        std::optional<SessionFence> fence;
        if (const auto* claim = context.find<UpdateClaim>()) {
            fence = SessionFence{
                claim->bot, claim->update.update_id, claim->owner, claim->fencing_token};
        }
        auto version = co_await store->compare_exchange(
            id, record ? record->version : 0, context.get<SessionData>().value, expiry, fence);
        if (!version) {
            throw std::runtime_error("session compare-and-swap conflict");
        }
        co_return result;
    };
}

AsyncPolling::AsyncPolling(AsyncApplication& app, AsyncPollingOptions options)
    : app_(app), options_(std::move(options)), offset_(options_.polling.request.offset) {
    if (options_.store &&
        (options_.bot_identity.empty() || options_.owner.empty() || options_.lease.count() < 30)) {
        throw std::invalid_argument("durable polling needs bot identity, owner and lease >=30ms");
    }
    auto& request = options_.polling.request;
    if ((request.limit && (*request.limit < 1 || *request.limit > 100)) ||
        (request.timeout && *request.timeout < 0) || (request.offset && *request.offset < 0)) {
        throw std::invalid_argument("invalid polling options");
    }
    if (options_.polling.retry.initial_delay.count() <= 0 ||
        options_.polling.retry.max_delay < options_.polling.retry.initial_delay) {
        throw std::invalid_argument("invalid retry delays");
    }
}

Task<std::vector<Update>> AsyncPolling::retrieve() {
    app_.claim_ingestion("polling");
    unsigned attempt = 0;
    auto ex = co_await asio::this_coro::executor;
    while (!stop_.stop_requested()) {
        auto request = options_.polling.request;
        request.offset = offset_;
        auto control = std::make_shared<RequestControl>();
        control->stop = stop_.get_token();
        std::exception_ptr error;
        try {
            co_return co_await app_.bot().get_updates(std::move(request), {}, control);
        } catch (...) {
            error = std::current_exception();
        }
        auto delay = detail::polling_retry_delay(error, options_.polling.retry, attempt);
        if (!delay) {
            std::rethrow_exception(error);
        }
        if (attempt < 32) {
            ++attempt;
        }
        if (options_.polling.retry.on_retry) {
            options_.polling.retry.on_retry(error, *delay);
        }
        auto remaining = *delay;
        while (remaining.count() > 0 && !stop_.stop_requested()) {
            auto slice = std::min(remaining, std::chrono::milliseconds(25));
            asio::steady_timer timer(ex, slice);
            co_await timer.async_wait(asio::use_awaitable);
            remaining -= slice;
        }
    }
    throw CancelledError();
}

Task<std::size_t> AsyncPolling::poll_once() {
    if (stop_.stop_requested()) {
        co_return 0;
    }
    if (options_.store) {
        auto saved = co_await options_.store->checkpoint(options_.bot_identity);
        if (saved) {
            offset_ = saved;
        }
    }
    auto updates = co_await retrieve();
    std::vector<Update> accepted;
    auto next = offset_;
    for (auto& update : updates) {
        if (next && update.update_id < *next) {
            continue;
        }
        if (update.update_id < 0 || update.update_id == std::numeric_limits<std::int64_t>::max()) {
            throw ProtocolError("invalid update identifier");
        }
        next = update.update_id + 1;
        accepted.push_back(std::move(update));
    }
    if (options_.store) {
        std::vector<InboxUpdate> items;
        for (auto& update : accepted) {
            items.push_back(
                {update.update_id, std::to_string(app_.ordering_key(update)), to_json(update)});
        }
        auto count =
            co_await options_.store->enqueue(options_.bot_identity, std::move(items), next);
        offset_ = next;
        co_return count;
    }
    if (accepted.empty()) {
        co_return 0;
    }

    struct Batch {
        std::mutex mutex;
        std::vector<std::exception_ptr> errors;
        std::size_t remaining;
        std::function<void()> finish;
    };

    auto batch = std::make_shared<Batch>();
    batch->errors.resize(accepted.size());
    batch->remaining = accepted.size();
    auto ex = co_await asio::this_coro::executor;
    auto token = asio::use_awaitable;
    co_await asio::async_initiate<decltype(token), void()>(
        [this, batch, &accepted, ex](auto handler) {
            // A completion posted on another executor thread may resume and
            // destroy the initiation frame before the final co_spawn returns.
            // Own all loop inputs on this call's stack before starting work;
            // never access initiation captures after scheduling a completion.
            auto items = std::make_shared<std::vector<Update>>(accepted);
            auto owned_batch = batch;
            auto executor = ex;
            auto* application = &app_;
            auto h = std::make_shared<decltype(handler)>(std::move(handler));
            owned_batch->finish = [h, executor] {
                asio::post(executor, [h]() mutable { (*h)(); });
            };
            for (std::size_t i = 0; i < items->size(); ++i) {
                asio::co_spawn(executor,
                               application->process((*items)[i]),
                               [owned_batch, i](std::exception_ptr error, Result) {
                                   bool done;
                                   {
                                       std::lock_guard lock(owned_batch->mutex);
                                       owned_batch->errors[i] = error;
                                       done = !--owned_batch->remaining;
                                   }
                                   if (done) {
                                       owned_batch->finish();
                                   }
                               });
            }
        },
        token);
    std::size_t count = 0;
    std::exception_ptr error;
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        if (batch->errors[i]) {
            error = batch->errors[i];
            break;
        }
        offset_ = accepted[i].update_id + 1;
        ++count;
    }
    if (error) {
        std::rethrow_exception(error);
    }
    co_return count;
}

Task<bool> AsyncPolling::process_pending() {
    if (!options_.store) {
        throw std::logic_error("no durable update store configured");
    }
    auto claim =
        co_await options_.store->claim(options_.bot_identity, options_.owner, options_.lease);
    if (!claim) {
        co_return false;
    }
    auto ex = co_await asio::this_coro::executor;

    struct Renewal {
        std::atomic<bool> done = false, lost = false;
        asio::strand<asio::any_io_executor> strand;
        std::shared_ptr<asio::steady_timer> timer;

        explicit Renewal(asio::any_io_executor ex)
            : strand(asio::make_strand(ex)), timer(std::make_shared<asio::steady_timer>(strand)) {}
    };

    auto renewal = std::make_shared<Renewal>(ex);
    auto renew = [](std::shared_ptr<UpdateStore> store,
                    UpdateClaim claim,
                    std::chrono::milliseconds lease,
                    std::shared_ptr<Renewal> state) -> Task<void> {
        auto executor = co_await asio::this_coro::executor;
        while (!state->done) {
            state->timer->expires_after(lease / 3);
            asio::error_code ec;
            co_await state->timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (state->done) {
                co_return;
            }
            if (!co_await store->renew(claim, lease)) {
                state->lost = true;
                co_return;
            }
        }
    };
    asio::co_spawn(renewal->strand,
                   renew(options_.store, *claim, options_.lease, renewal),
                   [renewal](std::exception_ptr e) {
                       if (e) {
                           renewal->lost = true;
                       }
                   });
    std::exception_ptr error;
    try {
        if (co_await app_.process_json(claim->update.payload, *claim) == Result::failed) {
            throw std::runtime_error("durable handler returned failed");
        }
    } catch (...) {
        error = std::current_exception();
    }
    renewal->done = true;
    asio::post(renewal->strand, [renewal] { renewal->timer->cancel(); });
    if (renewal->lost) {
        throw std::runtime_error("durable update lease lost; side effects may have occurred");
    }
    if (error) {
        std::string reason = "handler failed";
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            reason = e.what();
        } catch (...) {
        }
        if (!co_await options_.store->fail(*claim, reason)) {
            throw std::runtime_error("stale durable update claim");
        }
        std::rethrow_exception(error);
    }
    if (!co_await options_.store->complete(*claim)) {
        throw std::runtime_error("stale durable update claim");
    }
    co_return true;
}

Task<void> AsyncPolling::run() {
    const auto executor = co_await asio::this_coro::executor;
    while (!stop_.stop_requested()) {
        std::exception_ptr error;
        try {
            co_await poll_once();
        } catch (...) {
            error = std::current_exception();
        }
        if (stop_.stop_requested()) {
            co_return;
        }
        if (error) {
            std::rethrow_exception(error);
        }
        // A custom/cache transport may complete synchronously with no updates.
        // Yield explicitly so timers, shutdown and dispatch cannot be starved.
        co_await asio::post(executor, asio::use_awaitable);
    }
}
} // namespace tgpp::ext

namespace tgpp::ext {
AsyncRouter::AsyncRouter(std::string name, AsyncFilter filter)
    : name_(std::move(name)), filter_(std::move(filter)) {
    if (name_.empty() || name_.find('/') != std::string::npos) {
        throw std::invalid_argument("router name must be nonempty and contain no slash");
    }
    if (!filter_) {
        filter_ = [](const AsyncContext&) { return true; };
    }
}

void AsyncRouter::add(std::string name, AsyncFilter filter, AsyncCallback callback) {
    if (frozen_) {
        throw std::logic_error("async router frozen");
    }
    if (!filter || !callback) {
        throw std::invalid_argument("empty async route");
    }
    entries_.emplace_back(Route{std::move(name), std::move(filter), std::move(callback)});
}

void AsyncRouter::include(std::shared_ptr<AsyncRouter> child) {
    if (frozen_) {
        throw std::logic_error("async router frozen");
    }
    if (!child || child.get() == this) {
        throw std::invalid_argument("invalid child router");
    }
    std::vector<AsyncRouter*> nodes;
    child->validate_tree(nodes);
    if (std::find(nodes.begin(), nodes.end(), this) != nodes.end()) {
        throw std::invalid_argument("cyclic async router tree");
    }
    for (const auto& entry : entries_) {
        if (const auto* existing = std::get_if<std::shared_ptr<AsyncRouter>>(&entry);
            existing && (*existing)->name_ == child->name_) {
            throw std::invalid_argument("duplicate child router");
        }
    }
    entries_.emplace_back(std::move(child));
}

void AsyncRouter::use(AsyncMiddleware middleware) {
    if (frozen_) {
        throw std::logic_error("async router frozen");
    }
    if (!middleware) {
        throw std::invalid_argument("empty middleware");
    }
    middleware_.push_back(std::move(middleware));
}

void AsyncRouter::validate_tree(std::vector<AsyncRouter*>& nodes) {
    if (frozen_ || std::find(nodes.begin(), nodes.end(), this) != nodes.end()) {
        throw std::logic_error("async router already installed, cyclic, or shared in tree");
    }
    nodes.push_back(this);
    for (const auto& entry : entries_) {
        if (const auto* child = std::get_if<std::shared_ptr<AsyncRouter>>(&entry)) {
            (*child)->validate_tree(nodes);
        }
    }
}

Task<Result> AsyncRouter::invoke(std::shared_ptr<AsyncRouter> router,
                                 AsyncContext& context,
                                 std::string scope,
                                 std::size_t index) {
    if (index < router->middleware_.size()) {
        auto next = std::make_shared<AsyncNext::State>();
        next->continuation = [router, &context, scope, index] {
            return invoke(router, context, scope, index + 1);
        };
        try {
            auto result = co_await router->middleware_[index](context, AsyncNext(next));
            next->active = false;
            next->continuation = {};
            co_return result;
        } catch (...) {
            next->active = false;
            next->continuation = {};
            throw;
        }
    }
    for (auto& entry : router->entries_) {
        if (auto* child = std::get_if<std::shared_ptr<AsyncRouter>>(&entry)) {
            auto result = co_await dispatch(*child, context, scope);
            if (result != Result::unhandled) {
                co_return result;
            }
        } else {
            auto& route = std::get<Route>(entry);
            context.handler_ = scope + "/" + route.name;
            if (route.filter(context)) {
                co_await route.callback(context);
                co_return Result::handled;
            }
            context.handler_ = scope;
        }
    }
    co_return Result::unhandled;
}

Task<Result> AsyncRouter::dispatch(std::shared_ptr<AsyncRouter> router,
                                   AsyncContext& context,
                                   std::string parent) {
    auto previous = context.handler_;
    const auto scope = parent.empty() ? router->name_ : parent + "/" + router->name_;
    context.handler_ = scope;
    // Scope filtering precedes its middleware; descendants' filters run inside
    // that middleware's continuation and can consume metadata it populated.
    auto result =
        router->filter_(context) ? co_await invoke(router, context, scope, 0) : Result::unhandled;
    if (result == Result::unhandled) {
        context.handler_ = std::move(previous);
    }
    co_return result;
}

void AsyncApplication::include(std::shared_ptr<AsyncRouter> router) {
    if (state_->frozen) {
        throw std::logic_error("async application frozen");
    }
    if (!router) {
        throw std::invalid_argument("null async router");
    }
    // Validate before changing either application or tree, so a rejected cycle
    // cannot leave a partially installed/frozen collection of routes.
    std::vector<AsyncRouter*> nodes;
    router->validate_tree(nodes);
    add_result(
        router->name_,
        [](const AsyncContext&) { return true; },
        [router](AsyncContext& context) { return AsyncRouter::dispatch(router, context, ""); });
    for (auto* node : nodes) {
        node->frozen_ = true;
    }
}

void AsyncApplication::set_bot_username(std::string username) {
    if (state_->frozen) {
        throw std::logic_error("async application frozen");
    }
    state_->options.bot_username = std::move(username);
}
} // namespace tgpp::ext
