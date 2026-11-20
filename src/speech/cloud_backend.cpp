#include "speech/cloud_backend.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <format>
#include <set>
#include <stdexcept>
#include <system_error>
#include <typeinfo>
#include <variant>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "engine/error.h"
#include "http/multipart.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "speech/api_models.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"transcriber.openai_backend"};
constexpr log::Logger kIsolatedLog{"services.isolated"};

constexpr std::string_view kCanceled = "Transcription canceled";
constexpr std::string_view kNeedsKey =
    "The selected API transcription engine needs a valid API key";
// local_asr/process.py _TIMED_OUT: the upload outlived its read timeout and the grace after it.
constexpr std::string_view kTimedOut =
    "Speech engine timed out and was stopped. Reload the engine.";

[[noreturn]] void throw_canceled() {
    throw EngineError(std::string(kCanceled));
}

// type(exc).__name__ for logs that must not quote an error's text (it can hold an address).
std::string type_name(const std::exception& error) {
    std::string name = typeid(error).name();
    for (const std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    if (const auto colon = name.rfind("::"); colon != std::string::npos) {
        name.erase(0, colon + 2);
    }
    return name;
}

// repr() of what json.loads returned, as the OpenAI SDK puts it into "Error code: N - {...}".
std::string py_repr(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return "None";
    case Json::value_t::boolean:
        return value.get<bool>() ? "True" : "False";
    case Json::value_t::number_integer:
        return std::to_string(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
        return std::to_string(value.get<std::uint64_t>());
    case Json::value_t::number_float:
        return py::float_repr(value.get<double>());
    case Json::value_t::string:
        return py::repr(value.get_ref<const std::string&>());
    case Json::value_t::array: {
        std::string out = "[";
        for (const Json& item : value) {
            out += (out.size() > 1 ? ", " : "") + py_repr(item);
        }
        return out + "]";
    }
    case Json::value_t::object: {
        std::string out = "{";
        for (const auto& [key, item] : value.items()) {
            out += (out.size() > 1 ? ", " : "") + py::repr(key) + ": " + py_repr(item);
        }
        return out + "}";
    }
    default:
        return value.dump();
    }
}

// Python's type(x).__name__ for a decoded JSON value.
std::string_view py_type(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return "NoneType";
    case Json::value_t::boolean:
        return "bool";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "int";
    case Json::value_t::number_float:
        return "float";
    case Json::value_t::array:
        return "list";
    case Json::value_t::object:
        return "dict";
    default:
        return "str";
    }
}

std::string text_of(const Json& data) {
    if (!data.is_object()) {
        throw EngineError(std::format("'{}' object has no attribute 'text'", py_type(data)));
    }
    const auto text = data.find("text");
    if (text == data.end()) {
        throw EngineError("'Transcription' object has no attribute 'text'");
    }
    if (!text->is_string()) {
        throw EngineError(std::format("'{}' object has no attribute 'strip'", py_type(*text)));
    }
    return text->get<std::string>();
}

class HttpUploadClient final : public UploadClient {
public:
    HttpUploadClient(http::Client& client, CloudConnection connection, std::string base_url)
        : client_(client), connection_(std::move(connection)), base_url_(std::move(base_url)) {}

    void prepare(const CancelToken& cancel) override {
        // Nothing to load: the request is built when the upload starts.
        if (cancel.cancelled() || closed_.cancelled()) {
            throw_canceled();
        }
    }

    std::string transcribe(const UploadRequest& upload, const CancelToken& cancel) override {
        if (cancel.cancelled() || closed_.cancelled()) {
            throw_canceled();
        }
        // Cancelled by the caller or by close(), whichever comes first.
        CancelSource request_cancel(cancel);
        const Subscription link =
            closed_.token().on_cancel([request_cancel]() mutable { request_cancel.cancel(); });

        http::Request request;
        request.method = "POST";
        request.url = base_url_ + "/audio/transcriptions";
        request.headers.emplace_back("Authorization",
                                     "Bearer " + connection_.key.value_or(std::string()));
        request.headers.emplace_back("Accept", "application/json");
        http::Multipart form;
        form.parts.push_back(http::form_field("model", upload.model));
        const TranscriptionHints& hints = upload.hints;
        if (hints.keywords) {
            for (const std::string& keyword : *hints.keywords) {
                form.parts.push_back(http::form_field("keywords[]", keyword));
            }
        }
        if (hints.language) {
            form.parts.push_back(http::form_field("language", *hints.language));
        }
        if (hints.languages) {
            for (const std::string& language : *hints.languages) {
                form.parts.push_back(http::form_field("languages[]", language));
            }
        }
        if (hints.prompt) {
            form.parts.push_back(http::form_field("prompt", *hints.prompt));
        }
        form.parts.push_back(http::form_field("response_format", upload.response_format));
        form.parts.push_back(
            http::file_part("file", upload.audio, http::content_type_for(upload.audio)));
        request.body = std::move(form);
        request.connect_timeout = kUploadConnectTimeout;
        const auto read_timeout =
            std::chrono::milliseconds(static_cast<std::int64_t>(connection_.timeout_s * 1000.0));
        request.idle_timeout = read_timeout;
        const auto deadline = std::chrono::steady_clock::now() + read_timeout + kUploadGrace;
        request.deadline = deadline;
        // The SDK's httpx client follows redirects.
        request.follow_redirects = true;

        const auto response = client_.send(request, request_cancel.token());
        if (!response) {
            const http::Error& error = response.error();
            if (error.kind == http::ErrorKind::Cancelled || request_cancel.cancelled()) {
                throw_canceled();
            }
            if (error.kind == http::ErrorKind::Timeout) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw SpeechApiError(std::string(kTimedOut), std::nullopt, std::nullopt);
                }
                throw SpeechApiError("Request timed out.", std::nullopt, std::nullopt);
            }
            throw SpeechApiError("Connection error.", std::nullopt, std::nullopt);
        }
        if (request_cancel.cancelled()) {
            throw_canceled();
        }
        return parse_transcription_reply(*response, upload.response_format);
    }

    bool alive() const override { return !closed_.cancelled(); }

    void close() override { closed_.cancel(); }

private:
    http::Client& client_;
    CloudConnection connection_;
    std::string base_url_;
    CancelSource closed_;
};

} // namespace

std::string parse_transcription_reply(const http::Response& response,
                                      std::string_view response_format) {
    if (response.status < 200 || response.status >= 300) {
        const std::string text = py::strip(response.body);
        std::string message;
        std::optional<std::string> code;
        try {
            const Json body = py::loads(text);
            message = std::format("Error code: {} - {}", response.status, py_repr(body));
            const Json* data = &body;
            if (body.is_object()) {
                if (const auto error = body.find("error"); error != body.end()) {
                    data = &*error;
                }
            }
            if (data->is_object()) {
                const auto found = data->find("code");
                if (found != data->end() && found->is_string()) {
                    code = found->get<std::string>();
                }
            }
        } catch (const std::exception&) {
            message = text.empty() ? std::format("Error code: {}", response.status) : text;
        }
        throw SpeechApiError(message, response.status, std::move(code));
    }
    if (response_format == "text") {
        return response.body;
    }
    std::string content_type = http::find_header(response.headers, "Content-Type").value_or("*");
    content_type = content_type.substr(0, content_type.find(';'));
    if (!content_type.ends_with("json")) {
        // Content that is not JSON is returned as it came, unless it parses as JSON anyway.
        Json data;
        try {
            data = py::loads(response.body);
        } catch (const std::exception&) {
            return response.body;
        }
        return text_of(data);
    }
    Json data;
    try {
        data = py::loads(response.body);
    } catch (const std::exception& error) {
        throw EngineError(error.what());
    }
    return text_of(data);
}

std::shared_ptr<UploadClient> make_http_upload_client(http::Client& client,
                                                      const CloudConnection& connection,
                                                      std::string openai_base_url) {
    std::string base = connection.base_url.value_or(std::move(openai_base_url));
    // The SDK joins its paths to the base URL with one slash.
    while (base.ends_with('/')) {
        base.pop_back();
    }
    return std::make_shared<HttpUploadClient>(client, connection, std::move(base));
}

// ---- UploadClients ----

namespace detail {

struct Standby {
    std::shared_ptr<UploadClient> worker;
    bool ready = false; // the warm-up has ended, whichever way
    bool ok = false;
    TimerHandle expiry;
};

struct UploadClientsState : std::enable_shared_from_this<UploadClientsState> {
    CloudConnection connection;
    UploadClientFactory factory;
    Executor* timers = nullptr;
    std::chrono::milliseconds standby_idle{};
    std::chrono::milliseconds warm_timeout{};

    std::mutex mutex;
    std::condition_variable changed;
    bool closed = false;
    std::set<std::shared_ptr<UploadClient>> workers;
    std::shared_ptr<Standby> standby;

    // Closes a readied client no upload took: its warm-up failed, or it sat unused.
    void retire(const std::shared_ptr<Standby>& retiring) {
        {
            std::lock_guard lock(mutex);
            if (standby != retiring) {
                return;
            }
            standby.reset();
            workers.erase(retiring->worker);
        }
        retiring->worker->close();
    }

    std::shared_ptr<UploadClient> start_worker_locked() {
        auto worker = factory(connection);
        workers.insert(worker);
        return worker;
    }

    // The client for one upload: the readied one when it is ready, else a new one.
    std::shared_ptr<UploadClient> upload_worker(const CancelToken& cancel) {
        std::shared_ptr<Standby> taken;
        {
            std::lock_guard lock(mutex);
            if (closed || cancel.cancelled()) {
                throw_canceled();
            }
            taken = std::exchange(standby, nullptr);
            if (taken == nullptr) {
                return start_worker_locked();
            }
            taken->expiry.cancel();
        }
        // A warm-up still under way is further along than a new client would be, and close()
        // ends the wait. So does the upload's cancel: Python's cancel closed the whole client,
        // here a cancelled job must not sit out a stalled warm-up.
        const Subscription woken = cancel.on_cancel([this] {
            std::lock_guard lock(mutex);
            changed.notify_all();
        });
        bool ok = false;
        bool cancelled = false;
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, warm_timeout,
                             [&] { return taken->ready || closed || cancel.cancelled(); });
            ok = taken->ready && taken->ok;
            cancelled = cancel.cancelled();
        }
        const bool usable = ok && !cancelled && taken->worker->alive();
        if (!usable) {
            taken->worker->close();
        }
        std::lock_guard lock(mutex);
        if (!usable) {
            workers.erase(taken->worker);
        }
        if (closed || cancelled) {
            throw_canceled();
        }
        if (usable) {
            return taken->worker;
        }
        return start_worker_locked();
    }
};

} // namespace detail

UploadClients::UploadClients(CloudConnection connection, UploadClientFactory factory,
                             Executor& timers, std::chrono::milliseconds standby_idle,
                             std::chrono::milliseconds warm_timeout)
    : connection_(std::move(connection)), state_(std::make_shared<detail::UploadClientsState>()) {
    state_->connection = connection_;
    state_->factory = std::move(factory);
    state_->timers = &timers;
    state_->standby_idle = standby_idle;
    state_->warm_timeout = warm_timeout;
}

UploadClients::~UploadClients() {
    close();
}

void UploadClients::warm() {
    auto& state = *state_;
    std::shared_ptr<detail::Standby> stale;
    std::shared_ptr<detail::Standby> standby;
    {
        std::lock_guard lock(state.mutex);
        if (state.closed) {
            return;
        }
        stale = state.standby;
        if (stale != nullptr && stale->worker->alive()) {
            return;
        }
        standby = std::make_shared<detail::Standby>();
        standby->worker = state.factory(state.connection);
        state.standby = standby;
        state.workers.insert(standby->worker);
        if (stale != nullptr) {
            state.workers.erase(stale->worker);
        }
    }
    if (stale != nullptr) {
        stale->expiry.cancel();
        stale->worker->close();
    }
    bool ok = false;
    try {
        standby->worker->prepare(CancelToken());
        ok = true;
    } catch (const std::exception& error) {
        // Type only: a client's errors can quote the server's address.
        kIsolatedLog.info("Upload worker warm-up failed ({}); the upload starts its own",
                          type_name(error));
    }
    {
        std::lock_guard lock(state.mutex);
        standby->ready = true;
        standby->ok = ok;
    }
    state.changed.notify_all();
    if (!ok) {
        state.retire(standby);
        return;
    }
    std::lock_guard lock(state.mutex);
    if (state.standby == standby && !state.closed) {
        const std::weak_ptr<detail::UploadClientsState> weak = state_;
        const std::weak_ptr<detail::Standby> weak_standby = standby;
        standby->expiry = state.timers->post_after(state.standby_idle, [weak, weak_standby] {
            const auto alive = weak.lock();
            const auto expired = weak_standby.lock();
            if (alive != nullptr && expired != nullptr) {
                alive->retire(expired);
            }
        });
    }
}

std::string UploadClients::create(const UploadRequest& request, const CancelToken& cancel) {
    const auto worker = state_->upload_worker(cancel);
    try {
        std::string text = worker->transcribe(request, cancel);
        worker->close();
        std::lock_guard lock(state_->mutex);
        state_->workers.erase(worker);
        return text;
    } catch (...) {
        worker->close();
        std::lock_guard lock(state_->mutex);
        state_->workers.erase(worker);
        throw;
    }
}

void UploadClients::close() {
    std::vector<std::shared_ptr<UploadClient>> workers;
    std::shared_ptr<detail::Standby> standby;
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        workers.assign(state_->workers.begin(), state_->workers.end());
        standby = std::exchange(state_->standby, nullptr);
    }
    state_->changed.notify_all();
    if (standby != nullptr) {
        standby->expiry.cancel();
    }
    // Every upload is signalled before any one is waited on.
    for (const auto& worker : workers) {
        worker->close();
    }
}

// ---- CloudSpeechBackend ----

namespace {

// What one decode's uploads share: results in chunk order, the first failure, and the cancel
// that ends the rest.
struct UploadBatch {
    explicit UploadBatch(const CancelToken& job) : cancel(job) {}

    std::mutex mutex;
    std::condition_variable finished;
    CancelSource cancel;
    std::vector<std::optional<std::string>> results;
    std::exception_ptr failure;
    std::size_t done = 0;
    std::size_t running = 0;
    // Uploads the executor dropped without running them (it was shut down or destroyed).
    std::size_t dropped = 0;
};

// Shared by every copy of one upload's task: counts the upload as done when the executor
// destroys the task without running it, so decode() never waits for an upload that cannot come.
struct UploadTicket {
    explicit UploadTicket(std::shared_ptr<UploadBatch> owner) : batch(std::move(owner)) {}
    UploadTicket(const UploadTicket&) = delete;
    UploadTicket& operator=(const UploadTicket&) = delete;
    ~UploadTicket() {
        if (ran) {
            return;
        }
        std::lock_guard lock(batch->mutex);
        ++batch->done;
        ++batch->dropped;
        batch->finished.notify_all();
    }

    std::shared_ptr<UploadBatch> batch;
    std::atomic<bool> ran{false}; // set by the task before anything else
};

} // namespace

CloudSpeechBackend::CloudSpeechBackend(http::Client& client, CredentialStore& credentials,
                                       const EnvReader& env, const DotEnv& dotenv,
                                       std::shared_ptr<const Json> settings, Executor& uploads,
                                       Clock& clock)
    : CloudSpeechBackend(client, credentials, env, dotenv, std::move(settings), uploads, clock,
                         Options{}) {}

CloudSpeechBackend::CloudSpeechBackend(http::Client& client, CredentialStore& credentials,
                                       const EnvReader& env, const DotEnv& dotenv,
                                       std::shared_ptr<const Json> settings, Executor& uploads,
                                       Clock& clock, Options options)
    : client_(client), credentials_(credentials), env_(env), dotenv_(dotenv), uploads_(uploads),
      clock_(clock), options_(std::move(options)),
      settings_(settings ? std::move(settings) : std::make_shared<const Json>(Json::object())) {
    if (!options_.upload_clients) {
        options_.upload_clients = [this](const CloudConnection& connection) {
            // No base URL means OpenAI, or the SDK's OPENAI_BASE_URL.
            const auto from_env = env_.get("OPENAI_BASE_URL");
            return make_http_upload_client(
                client_, connection,
                from_env && !from_env->empty() ? *from_env : std::string(kOpenAiBaseUrl));
        };
    }
    if (!options_.splitter) {
        options_.splitter = [](const SplitLimits& limits) {
            return std::make_unique<UploadSplitter>(limits);
        };
    }
}

CloudSpeechBackend::~CloudSpeechBackend() {
    close_clients();
}

Capabilities CloudSpeechBackend::capabilities() const {
    Capabilities capabilities;
    capabilities.windows = false;
    capabilities.live_preview = false;
    capabilities.accepts_files = true;
    capabilities.max_window_s = 0;
    capabilities.split_over_mb = options_.split_limits.max_file_size_mb;
    return capabilities;
}

void CloudSpeechBackend::set_settings(std::shared_ptr<const Json> settings) {
    std::lock_guard lock(settings_mutex_);
    settings_ = settings ? std::move(settings) : std::make_shared<const Json>(Json::object());
}

std::pair<SpeechEndpoint, std::string> CloudSpeechBackend::target() const {
    std::shared_ptr<const Json> settings;
    {
        std::lock_guard lock(settings_mutex_);
        settings = settings_;
    }
    const auto today = local_today(clock_);
    if (options_.model_type == "api") {
        SpeechEndpoint endpoint = selected_endpoint(*settings);
        std::string model = endpoint_model(endpoint, *settings, today);
        return {std::move(endpoint), std::move(model)};
    }
    if (std::ranges::find(kApiModelChoices, options_.model_type) != kApiModelChoices.end()) {
        return {configured_endpoint(kOpenAiEndpointId, *settings),
                serving_api_model(options_.model_type, today)};
    }
    throw std::invalid_argument("Unknown API transcription model: " + options_.model_type);
}

std::string CloudSpeechBackend::name() {
    const auto [endpoint, model] = target();
    return endpoint.name + " (" + model + ")";
}

CloudConnection CloudSpeechBackend::connection_for(const SpeechEndpoint& endpoint) {
    return CloudConnection{endpoint.base_url, api_key(endpoint, credentials_, env_, dotenv_),
                           endpoint.timeout_s()};
}

void CloudSpeechBackend::prepare_client(const SpeechEndpoint& endpoint) {
    // The key is read before the lock: the credential store blocks.
    const CloudConnection connection = connection_for(endpoint);
    std::shared_ptr<UploadClients> previous;
    {
        // Replaced and built under one lock, as Python's _client_lock: two threads preparing at
        // once (a recording's start and a decode) must not each build a client and leave one
        // that release() can no longer reach. Building starts nothing, so the lock is brief.
        std::lock_guard lock(client_mutex_);
        if (clients_ != nullptr && clients_->connection() == connection) {
            return;
        }
        previous = std::exchange(clients_, nullptr);
        if (!connection.key) {
            kLog.warning("No API key for {} transcription", endpoint.name);
        } else {
            try {
                clients_ = std::make_shared<UploadClients>(connection, options_.upload_clients,
                                                           uploads_, options_.standby_idle);
                failed_connection_.reset();
                kLog.info("{} transcription client initialized", endpoint.name);
            } catch (const std::exception& error) {
                kLog.error("Failed to initialize the {} client: {}", endpoint.name, error.what());
                failed_connection_ = connection;
            }
        }
    }
    // Closing only signals the previous client's uploads; none is waited on.
    if (previous != nullptr) {
        previous->close();
    }
}

std::shared_ptr<UploadClients> CloudSpeechBackend::current_clients() {
    std::lock_guard lock(client_mutex_);
    return clients_;
}

void CloudSpeechBackend::close_clients() {
    std::shared_ptr<UploadClients> clients;
    {
        std::lock_guard lock(client_mutex_);
        clients = std::exchange(clients_, nullptr);
    }
    if (clients != nullptr) {
        clients->close();
    }
}

void CloudSpeechBackend::prepare_for_transcription() {
    try {
        prepare_client(target().first);
        if (const auto clients = current_clients()) {
            clients->warm();
        }
    } catch (const std::exception& error) {
        // Type only: a client's errors can quote the server's address.
        kLog.info("Could not ready the upload worker ({}); the upload starts its own",
                  type_name(error));
    }
}

bool CloudSpeechBackend::is_available() {
    const CloudConnection connection = connection_for(target().first);
    std::lock_guard lock(client_mutex_);
    return connection.key.has_value() && connection != failed_connection_;
}

Readiness CloudSpeechBackend::load(const CancelToken&) {
    if (is_available()) {
        return Readiness{ReadyState::Ready, name(), ""};
    }
    return Readiness{ReadyState::NeedsKey, std::string(kNeedsKey), std::string(kNeedsKey)};
}

void CloudSpeechBackend::release() {
    close_clients();
}

std::optional<double>
CloudSpeechBackend::large_file_size_mb(const std::filesystem::path& audio) const {
    const auto [needs_splitting, megabytes] =
        options_.splitter(options_.split_limits)->check_file_size(audio);
    return needs_splitting ? std::optional<double>(megabytes) : std::nullopt;
}

namespace {

// One request with the job's hints, then once more without them when the server blames the
// request (400 or 422): hints must never cost a dictation, and neither OpenAI nor the compatible
// servers document limits for them.
std::string request_hinted(UploadClients& clients, const std::filesystem::path& audio,
                           const SpeechEndpoint& endpoint, const std::string& model,
                           const std::string& language, const std::vector<std::string>& phrases,
                           const CancelToken& cancel) {
    const auto request = [&](TranscriptionHints hints) {
        if (cancel.cancelled()) {
            throw_canceled();
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(audio, error)) {
            throw EngineError("[Errno 2] No such file or directory: " +
                              py::repr(path_to_utf8(audio)));
        }
        const UploadRequest upload{audio, model, response_format(endpoint, model),
                                   std::move(hints)};
        return py::strip(clients.create(upload, cancel));
    };
    const TranscriptionHints hints = transcription_hints(endpoint, model, language, phrases);
    if (hints.empty()) {
        return request({});
    }
    try {
        return request(hints);
    } catch (const SpeechApiError& error) {
        if ((error.status != 400 && error.status != 422) || is_model_gone_error(error)) {
            throw;
        }
        kLog.warning("{} rejected a request with hints (HTTP {}: {} keywords, {} prompt "
                     "characters, language {}); retrying without them",
                     endpoint.name, *error.status, hints.keywords ? hints.keywords->size() : 0,
                     hints.prompt ? py::len(*hints.prompt) : 0,
                     hints.language || hints.languages ? "set" : "unset");
        return request({});
    }
}

std::string transcribe_file(UploadClients& clients, const std::filesystem::path& audio,
                            const SpeechEndpoint& endpoint, const std::string& model,
                            const std::string& language, const std::vector<std::string>& phrases,
                            const CancelToken& cancel) {
    try {
        return request_hinted(clients, audio, endpoint, model, language, phrases, cancel);
    } catch (const SpeechApiError& error) {
        if (!endpoint.is_openai() || !is_retiring_model(model) || !is_model_gone_error(error)) {
            throw;
        }
        // OpenAI switched the model off before this computer's clock reached the shutdown day.
        // The fallback model takes other hint fields, so they are chosen again for it.
        kLog.warning("OpenAI no longer serves {}; retrying with {}", model, kDefaultApiModel);
        return request_hinted(clients, audio, endpoint, std::string(kDefaultApiModel), language,
                              phrases, cancel);
    }
}

} // namespace

std::vector<std::string>
CloudSpeechBackend::run_uploads(const DecodeJob& job,
                                const std::vector<std::filesystem::path>& files,
                                const SpeechEndpoint& endpoint, const std::string& model,
                                const std::shared_ptr<UploadClients>& clients) {
    const std::size_t total = files.size();
    auto batch = std::make_shared<UploadBatch>(job.cancel);
    batch->results.resize(total);
    // A cancel (the job's, or the first failure's) wakes the wait below.
    const Subscription woken = batch->cancel.token().on_cancel([batch] {
        std::lock_guard lock(batch->mutex);
        batch->finished.notify_all();
    });
    for (std::size_t index = 0; index < total; ++index) {
        auto ticket = std::make_shared<UploadTicket>(batch);
        uploads_.post([ticket, batch, index, file = files[index], clients, endpoint, model,
                       language = job.options.language, phrases = job.phrases] {
            ticket->ran = true;
            {
                std::lock_guard lock(batch->mutex);
                if (batch->cancel.cancelled()) {
                    // A later chunk of a failed or cancelled file is never sent.
                    ++batch->done;
                    batch->finished.notify_all();
                    return;
                }
                ++batch->running;
            }
            std::optional<std::string> text;
            std::exception_ptr failure;
            try {
                text = transcribe_file(*clients, file, endpoint, model, language, phrases,
                                       batch->cancel.token());
            } catch (...) {
                failure = std::current_exception();
            }
            bool first_failure = false;
            {
                std::lock_guard lock(batch->mutex);
                --batch->running;
                ++batch->done;
                if (failure) {
                    first_failure = !batch->failure;
                    if (first_failure) {
                        batch->failure = failure;
                    }
                } else {
                    batch->results[index] = std::move(text);
                }
                batch->finished.notify_all();
            }
            if (first_failure) {
                // A rejected chunk must not wait out an unrelated stalled upload.
                batch->cancel.cancel();
            }
        });
    }
    std::unique_lock lock(batch->mutex);
    // Every upload done or, once cancelled, every running one ended: a queued upload that starts
    // later sees the cancel and sends nothing.
    batch->finished.wait(lock, [&] {
        return batch->done == total || (batch->cancel.cancelled() && batch->running == 0);
    });
    if (batch->failure) {
        const auto failure = batch->failure;
        lock.unlock();
        std::rethrow_exception(failure);
    }
    // A dropped upload has no text: the executor is shutting down, which ends the job.
    if (batch->done != total || batch->dropped != 0 || job.cancel.cancelled()) {
        throw_canceled();
    }
    std::vector<std::string> texts;
    for (auto& result : batch->results) {
        texts.push_back(result.value_or(std::string()));
    }
    return texts;
}

std::string CloudSpeechBackend::transcribe_chunks(const DecodeJob& job,
                                                  const std::vector<std::filesystem::path>& chunks,
                                                  const SpeechEndpoint& endpoint,
                                                  const std::string& model,
                                                  const std::shared_ptr<UploadClients>& clients) {
    kLog.info("Starting chunked transcription with {} model: {} ({} chunks, up to {} at a time)",
              endpoint.name, model, chunks.size(), kChunkUploadConcurrency);
    try {
        return combine_transcriptions(run_uploads(job, chunks, endpoint, model, clients));
    } catch (const std::exception&) {
        // As cancel_transcription after a rejected chunk: the next request builds a fresh client.
        if (chunks.size() > 1 && !job.cancel.cancelled()) {
            close_clients();
        }
        throw;
    }
}

std::string CloudSpeechBackend::transcribe_split(const DecodeJob& job,
                                                 const std::filesystem::path& audio,
                                                 const SpeechEndpoint& endpoint,
                                                 const std::string& model,
                                                 const std::shared_ptr<UploadClients>& clients) {
    const auto splitter = options_.splitter(options_.split_limits);
    // The chunks are temporary files, removed here however the job ends.
    const auto cleanup = [&splitter] {
        try {
            splitter->cleanup_temp_files();
        } catch (const std::exception& error) {
            kLog.warning("Failed to cleanup temp files: {}", error.what());
        }
    };
    try {
        const auto chunks = splitter->split_audio_file(
            audio,
            [this, &job](const std::string& message) { report_progress(job, message, false); },
            job.cancel);
        if (chunks.empty()) {
            throw EngineError("Failed to split audio file");
        }
        if (job.cancel.cancelled()) {
            throw_canceled();
        }
        report_progress(job, std::format("Transcribing {} chunks...", chunks.size()), true);
        std::string text = transcribe_chunks(job, chunks, endpoint, model, clients);
        cleanup();
        return text;
    } catch (...) {
        cleanup();
        throw;
    }
}

DecodeResult CloudSpeechBackend::decode(const DecodeJob& job) {
    const auto* path = std::get_if<std::filesystem::path>(&job.input);
    if (path == nullptr) {
        throw std::invalid_argument("The api speech backend uploads audio files only");
    }
    try {
        const auto [endpoint, model] = target();
        prepare_client(endpoint);
        if (job.cancel.cancelled()) {
            throw_canceled();
        }
        const auto clients = current_clients();
        if (clients == nullptr) {
            throw EngineError(endpoint.name +
                              " transcription is not available (no API key or client "
                              "initialization failed)");
        }
        kLog.info("Using {} model: {}", endpoint.name, model);
        std::string transcript;
        if (const auto megabytes = large_file_size_mb(*path)) {
            large_file_detected.emit(*megabytes);
            transcript = transcribe_split(job, *path, endpoint, model, clients);
        } else {
            kLog.info("Sending audio file to {}...", endpoint.name);
            // Even one upload runs on the upload strand, never on the speech thread.
            transcript = run_uploads(job, {*path}, endpoint, model, clients).front();
        }
        if (job.cancel.cancelled()) {
            kLog.info("Transcription canceled by user");
            throw_canceled();
        }
        kLog.info("API transcription complete. Length: {} characters", py::len(transcript));
        DecodeResult result;
        result.transcript.text = std::move(transcript);
        return result;
    } catch (const std::exception& error) {
        kLog.error("API transcription failed: {}", error.what());
        throw;
    }
}

} // namespace fv::speech
