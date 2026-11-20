#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"
#include "speech/endpoints.h"
#include "speech/service.h"
#include "speech/transcription_hints.h"
#include "speech/upload_splitter.h"

namespace fv::speech {

// Chunk uploads run this many at a time. Deliberately small: uploads have no retry path, so more
// parallelism mostly raises the odds of a rate-limit rejection that fails the whole file.
inline constexpr int kChunkUploadConcurrency = 3;
// A readied upload client that no upload took is closed after this long.
inline constexpr std::chrono::milliseconds kStandbyIdle{600'000};
// The longest an upload waits for a warm-up still under way.
inline constexpr std::chrono::milliseconds kWarmTimeout{60'000};
// Connect timeout of every upload; the read timeout is the endpoint's (SpeechEndpoint::timeout_s).
inline constexpr std::chrono::milliseconds kUploadConnectTimeout{10'000};
// An upload is abandoned this long after its read timeout ("Speech engine timed out and was
// stopped. Reload the engine.").
inline constexpr std::chrono::milliseconds kUploadGrace{60'000};

// Where uploads go and with what: a client prepared for one connection never serves another.
struct CloudConnection {
    std::optional<std::string> base_url; // nullopt: OpenAI
    std::optional<std::string> key;      // nullopt: the key is missing
    double timeout_s = kHostedTimeoutS;  // read timeout, seconds

    bool operator==(const CloudConnection&) const = default;
};

// One POST /audio/transcriptions.
struct UploadRequest {
    std::filesystem::path audio;
    std::string model;
    std::string response_format; // "text" or "json"
    TranscriptionHints hints;
};

// One upload's client (OpenWhisper's upload worker process): readied ahead of the upload by
// prepare(), used for one upload, then closed.
// Affinity: thread-safe; close() may come from any thread while another call runs.
class UploadClient {
public:
    virtual ~UploadClient() = default;
    // Gets ready to upload; sends nothing anywhere. Throws when it cannot.
    virtual void prepare(const CancelToken& cancel) = 0;
    // The server's transcript, unstripped. Throws SpeechApiError for a failed request and
    // EngineError("Transcription canceled") when cancelled or closed.
    virtual std::string transcribe(const UploadRequest& request, const CancelToken& cancel) = 0;
    virtual bool alive() const = 0;
    // Ends an upload in flight at once; the client is not used again.
    virtual void close() = 0;
};

using UploadClientFactory = std::function<std::shared_ptr<UploadClient>(const CloudConnection&)>;

// The UploadClient over http::Client: multipart as the OpenAI SDK sends it (model, keywords[],
// language, languages[], prompt, response_format, then the file streamed from disk), bearer
// key, no retries, connect 10 s, read the connection's timeout, abandoned 60 s after that.
// `base_url` is where an OpenAI connection (no base URL of its own) goes.
std::shared_ptr<UploadClient> make_http_upload_client(http::Client& client,
                                                      const CloudConnection& connection,
                                                      std::string openai_base_url);

namespace detail {
struct UploadClientsState;
}

// Uploads for one connection, with a client readied ahead of the next upload
// (services/isolated.py IsolatedOpenAIClient). warm() readies one; an upload takes it when it is
// ready (waiting up to 60 s for a warm-up under way), else starts its own; each upload's client
// is closed after it. A readied client nobody takes is closed after the idle time.
// Affinity: thread-safe.
class UploadClients {
public:
    UploadClients(CloudConnection connection, UploadClientFactory factory, Executor& timers,
                  std::chrono::milliseconds standby_idle = kStandbyIdle,
                  std::chrono::milliseconds warm_timeout = kWarmTimeout);
    ~UploadClients(); // closes
    UploadClients(const UploadClients&) = delete;
    UploadClients& operator=(const UploadClients&) = delete;

    const CloudConnection& connection() const { return connection_; }

    // Readies the next upload's client. Blocking; never sends anything. Throws what the factory
    // throws; a failed prepare is logged and leaves the upload to start its own.
    void warm();
    // One upload. Throws EngineError("Transcription canceled") once closed, or when `cancel`
    // fires (also while it waits for a warm-up).
    std::string create(const UploadRequest& request, const CancelToken& cancel);
    // Ends every upload and warm-up; later calls are refused.
    void close();

private:
    CloudConnection connection_;
    std::shared_ptr<detail::UploadClientsState> state_;
};

// The `api` backend of architecture 5.1 (transcriber/openai_backend.py): uploads files to the
// selected OpenAI-compatible server (OpenAI, Groq, a server the user runs), splitting a file over
// 23 MB at silences and uploading its chunks 3 at a time on `uploads`, in chunk order, the first
// failure cancelling the rest. Nothing is retried, except a request whose hints a server rejects
// (400 or 422: once without them) and an OpenAI model gone before its shutdown day (once with
// gpt-transcribe). decode() blocks its caller (fv-speech) while the uploads run; they never run
// on the speech or GUI thread.
//
// The target (endpoint, model) is read from the settings at each decode, so a saved key or newly
// chosen server reaches the next request; the client is rebuilt when (base URL, key, timeout)
// changes. Keys, URLs with queries and transcript text are never logged.
// Affinity: thread-safe (decode on fv-speech, prepare_for_transcription on a thread of its own).
class CloudSpeechBackend final : public Backend {
public:
    // Seams for tests: `model_type` "api" follows the saved server and model; an OpenAI model id
    // pins the backend to that OpenAI model. Empty factory: HTTP uploads over `client`.
    struct Options {
        std::string model_type = "api";
        UploadClientFactory upload_clients;
        SplitLimits split_limits;
        std::chrono::milliseconds standby_idle = kStandbyIdle;
        // The splitter one decode uses; empty: UploadSplitter(split_limits).
        std::function<std::unique_ptr<UploadSplitter>(const SplitLimits&)> splitter;
    };

    CloudSpeechBackend(http::Client& client, CredentialStore& credentials, const EnvReader& env,
                       const DotEnv& dotenv, std::shared_ptr<const Json> settings,
                       Executor& uploads /*strand 3 wide*/, Clock& clock);
    CloudSpeechBackend(http::Client& client, CredentialStore& credentials, const EnvReader& env,
                       const DotEnv& dotenv, std::shared_ptr<const Json> settings,
                       Executor& uploads, Clock& clock, Options options);
    ~CloudSpeechBackend() override;

    std::string_view id() const override { return "api"; }
    // {windows = false, live_preview = false, accepts_files = true, max_window_s = 0,
    // split_over_mb = 23}.
    Capabilities capabilities() const override;
    // Ready when the selected server has its key and its client did not fail to build, else
    // NeedsKey ("The selected API transcription engine needs a valid API key").
    Readiness load(const CancelToken& cancel) override;
    // Takes the job's path (an AudioClip throws std::invalid_argument).
    DecodeResult decode(const DecodeJob& job) override;
    // Closes the client; the next request builds a fresh one.
    void release() override;

    std::string recognition_support() const override { return "model"; }
    bool supports_recognition() const override { return true; }
    bool prepares_for_transcription() const override { return true; }
    // Readies the upload's client while the recording is made; never throws.
    void prepare_for_transcription() override;
    // The file's size in MiB when it is over the upload limit. Throws EngineError("Audio file
    // not found: <path>").
    std::optional<double> large_file_size_mb(const std::filesystem::path& audio) const override;

    // prepare_for_transcription, by the name the composition calls at recording start.
    void warm() { prepare_for_transcription(); }
    // Whether the selected server has its key and a client that built (is_available). Never
    // builds a client. Blocking (the credential store).
    bool is_available();
    // "<endpoint name> (<model>)": "Groq (whisper-large-v3-turbo)".
    std::string name();
    // A newer settings snapshot for the next request.
    void set_settings(std::shared_ptr<const Json> settings);

    // (endpoint, model) the next request goes to. Throws std::invalid_argument for an unknown
    // model_type.
    std::pair<SpeechEndpoint, std::string> target() const;

    // The file's size in MiB, emitted when a decode splits it.
    Signal<double> large_file_detected;

private:
    CloudConnection connection_for(const SpeechEndpoint& endpoint);
    void prepare_client(const SpeechEndpoint& endpoint);
    std::shared_ptr<UploadClients> current_clients();
    void close_clients();
    // Each file's text in order; the first failure (or a cancel) ends the uploads not started.
    std::vector<std::string> run_uploads(const DecodeJob& job,
                                         const std::vector<std::filesystem::path>& files,
                                         const SpeechEndpoint& endpoint, const std::string& model,
                                         const std::shared_ptr<UploadClients>& clients);
    std::string transcribe_split(const DecodeJob& job, const std::filesystem::path& audio,
                                 const SpeechEndpoint& endpoint, const std::string& model,
                                 const std::shared_ptr<UploadClients>& clients);
    std::string transcribe_chunks(const DecodeJob& job,
                                  const std::vector<std::filesystem::path>& chunks,
                                  const SpeechEndpoint& endpoint, const std::string& model,
                                  const std::shared_ptr<UploadClients>& clients);

    http::Client& client_;
    CredentialStore& credentials_;
    const EnvReader& env_;
    const DotEnv& dotenv_;
    Executor& uploads_;
    Clock& clock_;
    Options options_;

    mutable std::mutex settings_mutex_;
    std::shared_ptr<const Json> settings_;

    std::mutex client_mutex_;
    std::shared_ptr<UploadClients> clients_;
    std::optional<CloudConnection> failed_connection_;
};

// The upload's text from one transcription request: what the OpenAI SDK parses from the reply to
// `response_format`, or the SpeechApiError it raises for an error status. Exposed for tests.
std::string parse_transcription_reply(const http::Response& response,
                                      std::string_view response_format);

} // namespace fv::speech
