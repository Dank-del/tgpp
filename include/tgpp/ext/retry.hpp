#pragma once
#include "polling.hpp"
#include <algorithm>
#include <random>

namespace tgpp::ext::detail {
inline std::optional<std::chrono::milliseconds> polling_retry_delay(
    std::exception_ptr error, const PollingRetryOptions& options, unsigned attempt) {
    if (!options.enabled) {
        return {};
    }
    std::optional<std::chrono::milliseconds> minimum;
    try {
        std::rethrow_exception(error);
    } catch (const CancelledError&) {
        return {};
    } catch (const TransportError& cause) {
        if (!cause.retryable) {
            return {};
        }
    } catch (const ApiError& cause) {
        if (cause.code == 429) {
            if (cause.retry_after && *cause.retry_after > 0) {
                auto cap = std::chrono::milliseconds::max().count() / 1000;
                minimum = std::chrono::milliseconds(std::min(*cause.retry_after, cap) * 1000);
            }
        } else if (cause.code < 500 || cause.code > 599) {
            return {};
        }
    } catch (...) {
        return {};
    }
    auto delay = options.initial_delay;
    for (unsigned i = 0; i < std::min(attempt, 32u) && delay < options.max_delay; ++i) {
        delay = delay > options.max_delay / 2 ? options.max_delay : delay * 2;
    }
    delay = std::min(delay, options.max_delay);
    thread_local std::mt19937 random(std::random_device{}());
    delay = std::chrono::milliseconds(
        std::uniform_int_distribution<long long>(delay.count() / 2, delay.count())(random));
    if (minimum) {
        delay = std::max(delay, *minimum);
    }
    return delay;
}
} // namespace tgpp::ext::detail
