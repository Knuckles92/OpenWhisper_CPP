#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <string>

#include "base/cancel.h"
#include "base/clock.h"
#include "http/client.h"

namespace fv::llm::detail {

using RetryWait = std::function<void(std::chrono::milliseconds, const CancelToken&)>;

// One OpenAI SDK request with the SDK's default retries, by OpenAiCompatClient's rules
// (sdk_should_retry, sdk_retry_delay): two more attempts after a failed connection or a 408,
// 409, 429 or 5xx answer (x-should-retry decides when present; none when the server asks for
// more than 120 s), waiting retry-after-ms or retry-after (seconds or an HTTP date, measured
// from `clock`) when the server asks for up to 120 s, else 0.5 s doubling to at most 8 s, less
// up to a quarter of jitter. Returns the last answer, whatever its status, or the last failure;
// a cancel ends it at once.
std::expected<http::Response, http::Error>
sdk_send(http::Client& client, const http::Request& request, const CancelToken& cancel,
         const RetryWait& wait, const Clock& clock = system_clock());

// The SDK's APIStatusError text: "Error code: <n> - <str of the JSON body>", the stripped body
// when it is not JSON, or "Error code: <n>" when it is empty.
std::string sdk_status_message(const http::Response& response);

// A RetryWait that sleeps on the cancel token.
RetryWait sleeping_wait();

} // namespace fv::llm::detail
