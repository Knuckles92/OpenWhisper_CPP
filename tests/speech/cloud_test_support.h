#pragma once

// What the cloud speech tests share: a CloudSpeechBackend over FakeHttpClient (or a real client),
// an in-memory credential store, a pinned clock and a real 3-wide upload strand; reading back the
// multipart fields a request carried; the committed replies under tests/fixtures/cloud.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "speech/cloud_backend.h"
#include "speech/endpoints.h"
#include "testkit/testkit.h"

namespace fv::speech::test {

inline std::filesystem::path cloud_fixture(const std::string& name) {
    return testkit::source_root() / "tests" / "fixtures" / "cloud" / name;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string cloud_reply(const std::string& name) {
    return read_file(cloud_fixture(name));
}

inline void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

// The multipart fields of one request, by name (repeated names keep every value in order), and
// the uploaded file's name and bytes.
struct SentForm {
    std::map<std::string, std::vector<std::string>> fields;
    std::vector<std::string> order; // part names as sent
    std::string filename;
    std::string file_bytes;

    bool has(const std::string& name) const { return fields.contains(name); }
    std::string field(const std::string& name) const {
        const auto found = fields.find(name);
        return found == fields.end() || found->second.empty() ? std::string()
                                                              : found->second.front();
    }
    // Every field but model, file and response_format (test_personalize_s4_openai's hints_of).
    std::map<std::string, std::vector<std::string>> hints() const {
        auto rest = fields;
        rest.erase("model");
        rest.erase("response_format");
        return rest;
    }
};

inline SentForm form_of(const std::vector<http::MultipartPart>& parts) {
    SentForm form;
    for (const auto& part : parts) {
        form.order.push_back(part.name);
        const auto& bytes = std::get<std::string>(part.data);
        if (!part.filename.empty()) {
            form.filename = part.filename;
            form.file_bytes = bytes;
        } else {
            form.fields[part.name].push_back(bytes);
        }
    }
    return form;
}

// The form of a request a handler sees (file parts still on disk).
inline SentForm form_of(const http::Request& request) {
    SentForm form;
    for (const auto& part : std::get<http::Multipart>(request.body).parts) {
        form.order.push_back(part.name);
        if (const auto* path = std::get_if<std::filesystem::path>(&part.data)) {
            form.filename = part.filename;
            form.file_bytes = read_file(*path);
        } else {
            form.fields[part.name].push_back(std::get<std::string>(part.data));
        }
    }
    return form;
}

inline http::ScriptedResponse json_reply(int status, std::string body) {
    http::ScriptedResponse reply;
    reply.status = status;
    reply.reason = status == 200 ? "OK" : "Error";
    reply.headers = {{"Content-Type", "application/json"}};
    reply.body = std::move(body);
    return reply;
}

inline http::ScriptedResponse text_reply(std::string body) {
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/plain"}};
    reply.body = std::move(body);
    return reply;
}

// A transcription server: "text for <file name>" in the format the request asked for.
inline http::ScriptedResponse echo_file_name(const http::Request& request) {
    const SentForm form = form_of(request);
    const std::string text = "  text for " + form.filename + "  ";
    if (form.field("response_format") == "text") {
        return text_reply(text);
    }
    return json_reply(200, Json{{"text", text}}.dump());
}

inline DecodeJob file_job(const std::filesystem::path& path, std::string language = {},
                          std::vector<std::string> phrases = {}) {
    DecodeJob job;
    job.input = path;
    job.options.language = std::move(language);
    job.phrases = std::move(phrases);
    return job;
}

// A splitter that splits nothing itself: the test's check_file_size and chunk list, counting the
// splits and cleanups the backend asked for (test_openai_backend.py's `split` fixture).
struct SplitRecord {
    std::mutex mutex;
    std::vector<std::filesystem::path> splits;
    int cleanups = 0;
    std::filesystem::path large_file;
    double size_mb = 30.0;
    std::vector<std::filesystem::path> chunks;
    std::function<void()> during_split;

    std::vector<std::filesystem::path> split_calls() {
        std::lock_guard lock(mutex);
        return splits;
    }
    int cleanup_calls() {
        std::lock_guard lock(mutex);
        return cleanups;
    }
};

class StubSplitter final : public UploadSplitter {
public:
    explicit StubSplitter(std::shared_ptr<SplitRecord> record) : record_(std::move(record)) {}

    std::pair<bool, double> check_file_size(const std::filesystem::path& audio) const override {
        const bool large = audio == record_->large_file;
        return {large, large ? record_->size_mb : 0.01};
    }
    std::vector<std::filesystem::path> split_audio_file(const std::filesystem::path& audio,
                                                        const Progress& progress,
                                                        const CancelToken&) override {
        {
            std::lock_guard lock(record_->mutex);
            record_->splits.push_back(audio);
        }
        if (progress) {
            progress("Loading audio file...");
        }
        if (record_->during_split) {
            record_->during_split();
        }
        return record_->chunks;
    }
    void cleanup_temp_files() override {
        std::lock_guard lock(record_->mutex);
        ++record_->cleanups;
    }

private:
    std::shared_ptr<SplitRecord> record_;
};

// A CloudSpeechBackend with every collaborator in reach.
class CloudRig {
public:
    CloudRig()
        : credential_backend_(std::make_shared<MemoryCredentialBackend>()),
          credentials(credential_backend_, identity()), dotenv(root.path() / ".env", env),
          pool("cloud-test", 4),
          uploads(pool, "uploads", kChunkUploadConcurrency, ShutdownMode::CancelQueued) {}

    ~CloudRig() {
        backend.reset();
        uploads.shutdown(std::chrono::milliseconds(5'000));
    }

    std::shared_ptr<SplitRecord> stub_splits(std::vector<std::filesystem::path> chunks,
                                             double size_mb = 30.0) {
        auto record = std::make_shared<SplitRecord>();
        record->large_file = root.path() / "large-upload.wav";
        record->size_mb = size_mb;
        record->chunks = std::move(chunks);
        options.splitter = [record](const SplitLimits&) {
            return std::make_unique<StubSplitter>(record);
        };
        return record;
    }

    // Python's make_backend: an OpenAI key saved, the backend pinned to `model_type`.
    CloudSpeechBackend& make(std::string model_type = "whisper-1", bool save_key = true,
                             http::Client* client = nullptr) {
        if (save_key) {
            credentials.set("OPENAI_API_KEY", "sk-test");
        }
        options.model_type = std::move(model_type);
        backend = std::make_unique<CloudSpeechBackend>(
            client != nullptr ? *client : http, credentials, env, dotenv,
            std::make_shared<const Json>(settings), uploads, clock, options);
        return *backend;
    }

    void apply(const std::function<void(Json&)>& change) {
        change(settings);
        if (backend) {
            backend->set_settings(std::make_shared<const Json>(settings));
        }
    }

    // Six small chunk files (the `chunks` fixture).
    std::vector<std::filesystem::path> chunk_files(int count = 6) {
        std::vector<std::filesystem::path> paths;
        for (int index = 0; index < count; ++index) {
            const auto path = root.path() / ("part" + std::to_string(index) + ".wav");
            write_file(path, "RIFF" + std::string(32, static_cast<char>(index)));
            paths.push_back(path);
        }
        return paths;
    }

    testkit::TempDataRoot root;
    http::FakeHttpClient http;

private:
    std::shared_ptr<MemoryCredentialBackend> credential_backend_;

public:
    CredentialStore credentials;
    MapEnvReader env;
    DotEnv dotenv;
    FakeClock clock{std::chrono::sys_days{std::chrono::year{2026} / 11 / 15}};
    ThreadPool pool;
    Strand uploads;
    Json settings = Json::object();
    CloudSpeechBackend::Options options;
    std::unique_ptr<CloudSpeechBackend> backend;
};

// Counts the uploads in flight across threads; the work runs outside FakeHttpClient's lock.
struct InFlight {
    std::atomic<int> now{0};
    std::atomic<int> peak{0};

    void enter() {
        const int current = ++now;
        int seen = peak.load();
        while (current > seen && !peak.compare_exchange_weak(seen, current)) {
        }
    }
    void leave() { --now; }
};

} // namespace fv::speech::test
