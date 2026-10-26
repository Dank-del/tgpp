#include "handlers.hpp"
#include <algorithm>

namespace showcase {
ShowcaseCommand::ShowcaseCommand(std::string command, std::string action, std::shared_ptr<Sessions> sessions)
    : CommandHandler(std::move(command)), sessions_(std::move(sessions)), action_(std::move(action)) {}
void ShowcaseCommand::handle(tgpp::ext::Context& context) {
    const auto service = sessions_->get(context.chat()->id);
    if (!service) { context.reply("All demo workers are busy. Please try again shortly."); return; }
    if (action_ == "stop") { service->cancel(); context.reply("Your tour stopped. /showcase opens the menu again."); return; }
    auto job = action_;
    if (action_ == "demo") {
        if (!context.command() || context.command()->arguments.size() != 1 || !service->knows(context.command()->arguments.front())) {
            context.reply("Use /showcase to pick a demo, or /demo photo, /demo album, /demo poll, /demo rich…"); return;
        }
        job = context.command()->arguments.front();
    }
    if (!service->enqueue(job)) context.reply("That demo is already running/queued, or your queue is full. /stop cancels your queued work.");
}
ShowcaseCallback::ShowcaseCallback(std::shared_ptr<Sessions> sessions)
    : CallbackQueryHandler("show:"), sessions_(std::move(sessions)) {}
void ShowcaseCallback::handle(tgpp::ext::Context& context) {
    const auto data = *context.callback_query()->data;
    if (data == "show:alert") { context.answer_callback("This popup came from answerCallbackQuery in your C++ bot.", true); return; }
    if (data == "show:stop") {
        if (const auto service = sessions_->get(context.chat()->id)) service->cancel();
        context.answer_callback("Your tour cancelled"); return;
    }
    if (data.compare(0, 11, "show:count:") == 0) {
        const auto value = data.substr(11);
        if (value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            context.answer_callback("Invalid counter"); return;
        }
        const auto& query = *context.callback_query();
        const auto* original = query.message ? std::get_if<tgpp::Box<tgpp::Message>>(&query.message->value) : nullptr;
        if (!original || !*original) { context.answer_callback("Original message is unavailable"); return; }
        const auto message_id = (*original)->message_id;
        const auto key = std::make_pair(context.chat()->id, message_id);
        // Several taps can arrive with the old callback payload before the
        // client receives the first edit. Keep the authoritative count here.
        if (counters_.find(key) == counters_.end() && counters_.size() >= 128) counters_.erase(counters_.begin());
        const auto supplied = std::int64_t(std::stoll(value));
        auto& previous = counters_.try_emplace(key, supplied).first->second;
        const auto count = std::max(previous, supplied) + 1;
        context.answer_callback("Counter updated");
        tgpp::InlineKeyboardMarkup keyboard{};
        keyboard.inline_keyboard = {{button("Count: " + std::to_string(count), "show:count:" + std::to_string(count)), button("Show alert", "show:alert")}};
        tgpp::EditMessageTextRequest request{}; request.chat_id = context.chat()->id; request.message_id = (*original)->message_id;
        request.text = "tgpp · interactive counter\nYou clicked the button " + std::to_string(count) + " times."; request.reply_markup = keyboard;
        try { context.bot().edit_message_text(request); }
        catch (const tgpp::ApiError& error) {
            // A stale callback after a process restart may already be reflected
            // by Telegram's message. Treat that one explicit case as success.
            if (error.code != 400 || std::string(error.what()).find("message is not modified") == std::string::npos) throw;
        }
        previous = count;
        return;
    }
    auto job = data.substr(5);
    if (job.compare(0, 5, "demo:") == 0) job.erase(0, 5);
    const auto service = sessions_->get(context.chat()->id);
    if (!service) { context.answer_callback("All demo workers are busy. Try again shortly."); return; }
    const auto queued = service->enqueue(job);
    context.answer_callback(queued ? "Queued — the demo will appear shortly" : "Already running, unknown demo, or queue full");
}
bool ReplyDemoHandler::matches(const tgpp::ext::Context& context) const {
    const auto* message = context.message();
    if (!message || !message->text || !message->reply_to_message || !*message->reply_to_message) return false;
    const auto& parent = **message->reply_to_message;
    return parent.text && parent.text->compare(0, std::string("tgpp · reply to this message").size(), "tgpp · reply to this message") == 0;
}
void ReplyDemoHandler::handle(tgpp::ext::Context& context) {
    context.reply("✅ The force-reply handler received your message.");
}
} // namespace showcase
