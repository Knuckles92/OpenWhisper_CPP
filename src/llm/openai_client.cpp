#include "llm/openai_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "http/user_agent.h"
#include "llm/detail/py_repr.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::llm {

namespace {

bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// httpx.Headers semantics for a merge: a later header replaces every earlier one of its name.
void set_header(http::Headers& headers, const std::string& name, const std::string& value) {
    std::erase_if(headers, [&](const auto& entry) { return same_name(entry.first, name); });
    headers.emplace_back(name, value);
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> date_tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == ',') {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

std::optional<int> small_int(std::string_view text) {
    if (text.empty() || text.size() > 9 ||
        !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        value = value * 10 + (c - '0');
    }
    return value;
}

// email.utils.parsedate_tz then mktime_tz for the shapes servers send: "Wed, 21 Oct 2015
// 07:28:00 GMT", RFC 850's "Wednesday, 21-Oct-15 07:28:00 GMT" and asctime's "Wed Oct 21
// 07:28:00 2015". Seconds since the epoch, or nullopt. A date without a zone is read as UTC
// where Python would read local time.
std::optional<double> http_date_epoch(std::string_view text) {
    static constexpr std::array<std::string_view, 12> kMonths{
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    auto tokens = date_tokens(text);
    if (tokens.empty()) {
        return std::nullopt;
    }
    // A leading day name.
    if (!tokens.empty() && std::isalpha(static_cast<unsigned char>(tokens.front().front())) &&
        tokens.front().size() >= 3) {
        const auto name = lower_ascii(tokens.front()).substr(0, 3);
        static constexpr std::array<std::string_view, 7> kDays{"mon", "tue", "wed", "thu",
                                                               "fri", "sat", "sun"};
        if (std::ranges::find(kDays, name) != kDays.end()) {
            tokens.erase(tokens.begin());
        }
    }
    // RFC 850: "21-Oct-15".
    if (!tokens.empty() && std::ranges::count(tokens.front(), '-') == 2) {
        const auto first = tokens.front();
        const auto a = first.find('-');
        const auto b = first.find('-', a + 1);
        std::vector<std::string> split{first.substr(0, a), first.substr(a + 1, b - a - 1),
                                       first.substr(b + 1)};
        tokens.erase(tokens.begin());
        tokens.insert(tokens.begin(), split.begin(), split.end());
    }
    if (tokens.size() < 4) {
        return std::nullopt;
    }
    const auto month_of = [&](const std::string& token) -> std::optional<int> {
        const auto name = lower_ascii(token).substr(0, 3);
        const auto found = std::ranges::find(kMonths, name);
        if (found == kMonths.end()) {
            return std::nullopt;
        }
        return static_cast<int>(found - kMonths.begin()) + 1;
    };
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;
    std::string year_token;
    std::string clock;
    std::string zone;
    if (const auto m = month_of(tokens[0]); m && !small_int(tokens[0])) {
        // asctime: "Oct 21 07:28:00 2015".
        month = m;
        day = small_int(tokens[1]);
        clock = tokens[2];
        year_token = tokens[3];
        zone = tokens.size() > 4 ? tokens[4] : "";
    } else {
        day = small_int(tokens[0]);
        month = month_of(tokens[1]);
        year_token = tokens[2];
        clock = tokens[3];
        zone = tokens.size() > 4 ? tokens[4] : "";
    }
    year = small_int(year_token);
    if (!day || !month || !year) {
        return std::nullopt;
    }
    int y = *year;
    if (y < 100) {
        y += y > 68 ? 1900 : 2000;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    {
        std::vector<std::string> parts;
        std::string part;
        for (const char c : clock) {
            if (c == ':') {
                parts.push_back(std::move(part));
                part.clear();
            } else {
                part.push_back(c);
            }
        }
        parts.push_back(std::move(part));
        if (parts.size() < 2 || parts.size() > 3) {
            return std::nullopt;
        }
        const auto h = small_int(parts[0]);
        const auto m = small_int(parts[1]);
        const auto s = parts.size() == 3 ? small_int(parts[2]) : std::optional<int>(0);
        if (!h || !m || !s) {
            return std::nullopt;
        }
        hour = *h;
        minute = *m;
        second = *s;
    }
    int offset_s = 0;
    if (!zone.empty()) {
        const auto name = lower_ascii(zone);
        static constexpr std::array<std::pair<std::string_view, int>, 14> kZones{{
            {"ut", 0},
            {"utc", 0},
            {"gmt", 0},
            {"z", 0},
            {"ast", -400},
            {"adt", -300},
            {"est", -500},
            {"edt", -400},
            {"cst", -600},
            {"cdt", -500},
            {"mst", -700},
            {"mdt", -600},
            {"pst", -800},
            {"pdt", -700},
        }};
        const auto known =
            std::ranges::find(kZones, name, &std::pair<std::string_view, int>::first);
        int hhmm = 0;
        if (known != kZones.end()) {
            hhmm = known->second;
        } else if ((zone[0] == '+' || zone[0] == '-') && small_int(zone.substr(1))) {
            hhmm = *small_int(zone.substr(1)) * (zone[0] == '-' ? -1 : 1);
        }
        const int sign = hhmm < 0 ? -1 : 1;
        const int magnitude = hhmm * sign;
        offset_s = sign * ((magnitude / 100) * 3600 + (magnitude % 100) * 60);
    }
    const std::chrono::year_month_day date{std::chrono::year{y},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    const double epoch =
        static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(days).count()) +
        hour * 3600.0 + minute * 60.0 + second - offset_s;
    return epoch;
}

const RetryHooks& default_hooks() {
    static const RetryHooks hooks{
        [](std::chrono::milliseconds delay, const CancelToken& token) { token.wait_for(delay); },
        [] {
            thread_local std::mt19937_64 engine{std::random_device{}()};
            return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
        },
        &system_clock()};
    return hooks;
}

// Whole milliseconds, rounded up from the nearest microsecond so that 0.007 s stays 7 ms.
std::chrono::milliseconds seconds_to_ms(double seconds) {
    const auto micros = std::llround(std::max(0.0, seconds) * 1e6);
    return std::chrono::milliseconds((micros + 999) / 1000);
}

// The SDK's APIStatusError text: "Error code: <n> - <str of the JSON body>", the stripped body
// when it is not JSON, or "Error code: <n>" when it is empty.
std::string status_message(const http::Response& response) {
    const std::string text = py::strip(response.body);
    try {
        const Json body = py::loads(text);
        return "Error code: " + std::to_string(response.status) + " - " + detail::py_str(body);
    } catch (const std::exception&) {
        if (!text.empty()) {
            return text;
        }
        return "Error code: " + std::to_string(response.status);
    }
}

} // namespace

std::optional<double> parse_retry_after(const http::Headers& headers, const Clock& clock) {
    if (const auto ms = http::find_header(headers, "retry-after-ms")) {
        if (const auto value = py::py_float(*ms)) {
            return *value / 1000.0;
        }
    }
    const auto seconds = http::find_header(headers, "retry-after");
    if (!seconds) {
        return std::nullopt;
    }
    if (const auto value = py::py_float(*seconds)) {
        return *value;
    }
    const auto epoch = http_date_epoch(*seconds);
    if (!epoch) {
        return std::nullopt;
    }
    const auto now = std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
    return *epoch - now;
}

std::string_view status_error_kind(int status) {
    switch (status) {
    case 400:
        return "BadRequestError";
    case 401:
        return "AuthenticationError";
    case 403:
        return "PermissionDeniedError";
    case 404:
        return "NotFoundError";
    case 409:
        return "ConflictError";
    case 422:
        return "UnprocessableEntityError";
    case 429:
        return "RateLimitError";
    default:
        return status >= 500 ? "InternalServerError" : "APIStatusError";
    }
}

OpenAiCompatClient::OpenAiCompatClient(std::shared_ptr<http::Client> http, std::string base_url,
                                       std::string api_key, http::Headers default_headers,
                                       double timeout_s, int max_retries)
    : http_(std::move(http)),
      base_url_(base_url.empty() ? std::string(kOpenAiDefaultBaseUrl) : std::move(base_url)),
      api_key_(std::move(api_key)), default_headers_(std::move(default_headers)),
      timeout_s_(timeout_s), max_retries_(max_retries),
      hooks_(std::make_shared<const RetryHooks>(default_hooks())) {}

OpenAiCompatClient OpenAiCompatClient::with_options(std::optional<double> timeout_s,
                                                    std::optional<int> max_retries) const {
    OpenAiCompatClient copy = *this;
    if (timeout_s) {
        copy.timeout_s_ = *timeout_s;
    }
    if (max_retries) {
        copy.max_retries_ = *max_retries;
    }
    return copy;
}

OpenAiCompatClient OpenAiCompatClient::with_http_client(std::shared_ptr<http::Client> http) const {
    OpenAiCompatClient copy = *this;
    copy.http_ = std::move(http);
    return copy;
}

void OpenAiCompatClient::set_hooks(RetryHooks hooks) {
    const auto& defaults = default_hooks();
    if (!hooks.sleep) {
        hooks.sleep = defaults.sleep;
    }
    if (!hooks.random) {
        hooks.random = defaults.random;
    }
    if (hooks.clock == nullptr) {
        hooks.clock = defaults.clock;
    }
    hooks_ = std::make_shared<const RetryHooks>(std::move(hooks));
}

const Clock& OpenAiCompatClient::clock() const {
    return *hooks_->clock;
}

std::string OpenAiCompatClient::url_for(std::string_view path) const {
    std::string url = base_url_;
    if (!url.ends_with('/')) {
        url.push_back('/');
    }
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    url.append(path);
    return url;
}

std::chrono::milliseconds sdk_retry_delay(int retries_taken, const http::Headers& response_headers,
                                          const Clock& clock, double random01) {
    if (const auto asked = parse_retry_after(response_headers, clock);
        asked && std::isfinite(*asked) && *asked > 0 && *asked <= kSdkMaxRetryAfterS) {
        return seconds_to_ms(*asked);
    }
    const int retries = std::min(retries_taken, 1000);
    const double sleep =
        std::min(kSdkInitialRetryDelayS * std::pow(2.0, retries), kSdkMaxRetryDelayS);
    const double jitter = 1.0 - 0.25 * random01;
    return seconds_to_ms(sleep * jitter);
}

bool sdk_should_retry(const http::Response& response, const Clock& clock) {
    if (const auto asked = parse_retry_after(response.headers, clock);
        asked && std::isfinite(*asked) && *asked > kSdkMaxRetryAfterS) {
        return false;
    }
    if (const auto header = http::find_header(response.headers, "x-should-retry")) {
        if (*header == "true") {
            return true;
        }
        if (*header == "false") {
            return false;
        }
    }
    const int status = response.status;
    return status == 408 || status == 409 || status == 429 || status >= 500;
}

std::chrono::milliseconds
OpenAiCompatClient::retry_delay(int retries_taken, const http::Headers& response_headers) const {
    return sdk_retry_delay(retries_taken, response_headers, clock(), hooks_->random());
}

bool OpenAiCompatClient::should_retry(const http::Response& response) const {
    return sdk_should_retry(response, clock());
}

Json OpenAiCompatClient::post_json(std::string_view path, const Json& body,
                                   const RequestOptions& options, const CancelToken& cancel) const {
    return send("POST", path, &body, options, cancel);
}

Json OpenAiCompatClient::get_json(std::string_view path, const RequestOptions& options,
                                  const CancelToken& cancel) const {
    return send("GET", path, nullptr, options, cancel);
}

Json OpenAiCompatClient::send(const std::string& method, std::string_view path, const Json* body,
                              const RequestOptions& options, const CancelToken& cancel) const {
    const int max_retries = options.max_retries.value_or(max_retries_);
    const double timeout_s = options.timeout_s.value_or(timeout_s_);
    const auto timeout = seconds_to_ms(timeout_s);

    http::Request request;
    request.method = method;
    request.url = url_for(path);
    request.connect_timeout = timeout;
    request.idle_timeout = timeout;
    request.follow_redirects = true;
    request.headers = {{"Accept", "application/json"},
                       {"User-Agent", http::user_agent()},
                       {"Authorization", "Bearer " + api_key_}};
    if (body != nullptr) {
        request.headers.emplace_back("Content-Type", "application/json");
        py::DumpOptions dump;
        dump.ensure_ascii = false;
        dump.item_sep = ",";
        dump.key_sep = ":";
        request.body = py::dumps(*body, dump);
    }
    for (const auto& [name, value] : default_headers_) {
        set_header(request.headers, name, value);
    }
    for (const auto& [name, value] : options.headers) {
        set_header(request.headers, name, value);
    }

    for (int retries_taken = 0;; ++retries_taken) {
        const int remaining = max_retries - retries_taken;
        if (cancel.cancelled()) {
            throw RequestCanceled();
        }
        auto answer = http_->send(request, cancel);
        if (!answer) {
            if (answer.error().kind == http::ErrorKind::Cancelled || cancel.cancelled()) {
                throw RequestCanceled();
            }
            const bool timed_out = answer.error().kind == http::ErrorKind::Timeout;
            if (remaining > 0) {
                hooks_->sleep(retry_delay(retries_taken, {}), cancel);
                continue;
            }
            if (timed_out) {
                throw ApiError("APITimeoutError", 0, "Request timed out.");
            }
            throw ApiError("APIConnectionError", 0, "Connection error.");
        }
        const http::Response& response = *answer;
        if (response.status < 200 || response.status >= 300) {
            if (remaining > 0 && should_retry(response)) {
                hooks_->sleep(retry_delay(retries_taken, response.headers), cancel);
                continue;
            }
            throw ApiError(status_error_kind(response.status), response.status,
                           status_message(response));
        }
        const std::string content_type =
            http::find_header(response.headers, "content-type").value_or("*");
        const std::string media = content_type.substr(0, content_type.find(';'));
        if (!media.ends_with("json")) {
            try {
                return py::loads(response.body);
            } catch (const std::exception&) {
                return Json(response.body);
            }
        }
        return py::loads(response.body);
    }
}

OpenAiCompatClient create_openai_client(std::shared_ptr<http::Client> http,
                                        const TextLLMProfile& profile, double timeout_s,
                                        std::string_view api_key, std::string_view session_id,
                                        std::optional<int> max_retries, KeySources* keys) {
    std::optional<std::string> key;
    if (!api_key.empty()) {
        key = std::string(api_key);
    } else if (!profile.requires_api_key()) {
        key = std::string(kAuthFreeApiKey);
    } else if (keys != nullptr) {
        key = resolve_api_key(profile, *keys);
    }
    if (!key || key->empty()) {
        throw py::PyError("RuntimeError", "No API key found for " + profile.name + " (set " +
                                              profile.api_key_env + ")");
    }
    return OpenAiCompatClient(std::move(http), profile.base_url.value_or(""), *key,
                              provider_headers(profile, session_id).value_or(http::Headers{}),
                              timeout_s, max_retries.value_or(kSdkDefaultMaxRetries));
}

} // namespace fv::llm
