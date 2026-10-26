#include "welcome_module.hpp"
#include "start_command.hpp"

namespace example {
void WelcomeModule::configure(tgpp::ext::Router& router) {
    router.add<StartCommand>();
}
} // namespace example
