#include "llm/detail/sdk_request.h"

#include <random>

#include "llm/detail/py_repr.h"
#include "llm/openai_client.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::llm::detail {

namespace {

constexpr int kMaxRetries = kSdkDefaultMaxRetries;

double jitter_draw() {
    thread_local std::mt19937 random{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(random);
}

} // namespace

std::expected<http::Response, http::Error> sdk_send(http::Client& client,
                                                    const http::Request& request,
                                                    const CancelToken& cancel,
                                                    const RetryWait& wait, const Clock& clock) {
    for (int retries_taken = 0;; ++retries_taken) {
        const int remaining = kMaxRetries - retries_taken;
        auto response = client.send(request, cancel);
        if (!response) {
            if (response.error().kind == http::ErrorKind::Cancelled || cancel.cancelled() ||
                remaining <= 0) {
                return response;
            }
            wait(sdk_retry_delay(retries_taken, {}, clock, jitter_draw()), cancel);
            if (cancel.cancelled()) {
                return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
            }
            continue;
        }
        const int status = response->status;
        if ((status < 200 || status >= 300) && remaining > 0 &&
            sdk_should_retry(*response, clock)) {
            wait(sdk_retry_delay(retries_taken, response->headers, clock, jitter_draw()), cancel);
            if (cancel.cancelled()) {
                return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
            }
            continue;
        }
        return response;
    }
}

std::string sdk_status_message(const http::Response& response) {
    const std::string text = py::strip(response.body);
    try {
        const Json body = py::loads(text);
        return "Error code: " + std::to_string(response.status) + " - " + py_str(body);
    } catch (const std::exception&) {
        if (!text.empty()) {
            return text;
        }
        return "Error code: " + std::to_string(response.status);
    }
}

RetryWait sleeping_wait() {
    return [](std::chrono::milliseconds delay, const CancelToken& token) {
        token.wait_for(delay);
    };
}

} // namespace fv::llm::detail
