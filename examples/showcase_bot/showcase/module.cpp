#include "module.hpp"
#include "handlers.hpp"

namespace showcase {
tgpp::ext::Result PrivateChat::handle(tgpp::ext::Context& context, tgpp::ext::Next next) {
    if (!context.user() || !context.chat() || context.user()->id != context.chat()->id ||
        context.chat()->type != "private") {
        return tgpp::ext::Result::stopped;
    }
    return next();
}

ShowcaseModule::ShowcaseModule(std::shared_ptr<Sessions> sessions)
    : sessions_(std::move(sessions)) {
    if (!sessions_) {
        throw std::invalid_argument("showcase needs sessions");
    }
}

void ShowcaseModule::configure(tgpp::ext::Router& router) {
    router.use<PrivateChat>();
    for (const auto* command : {"start", "help", "showcase"}) {
        router.add<ShowcaseCommand>(command, "menu", sessions_);
    }
    for (const auto* command : {"tour", "report", "stop", "demo"}) {
        router.add<ShowcaseCommand>(command, command, sessions_);
    }
    router.add<ShowcaseCallback>(sessions_);
    router.add<ReplyDemoHandler>();
    router.add<tgpp::ext::FunctionHandler>(
        "showcase_menu",
        tgpp::ext::filters::text(),
        [sessions = sessions_](tgpp::ext::Context& context) {
            if (const auto service = sessions->get(context.chat()->id)) {
                service->enqueue("menu");
            } else {
                context.reply("All demo workers are busy. Please try again shortly.");
            }
        });
}
} // namespace showcase
