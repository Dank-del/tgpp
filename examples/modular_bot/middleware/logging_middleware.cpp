#include "logging_middleware.hpp"
#include <chrono>

namespace example {
tgpp::ext::Result LoggingMiddleware::handle(tgpp::ext::Context& context, tgpp::ext::Next next) {
    const auto start = std::chrono::steady_clock::now();
    try {
        const auto result = next();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        output_ << "update=" << context.update().update_id << " result=" << int(result)
                << " elapsed_ms=" << elapsed.count() << '\n';
        return result;
    } catch (...) {
        output_ << "update=" << context.update().update_id << " failed at " << context.route()
                << '/' << context.handler_name() << '\n';
        throw;
    }
}
} // namespace example
