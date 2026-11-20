#include "models/speaker_model.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "models/catalog.h"
#include "models/hf_access_policy.h"
#include "models/testing/speaker_model_state.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger kLog("services.components");

// One download at a time, so a meeting start and Settings' "Download now" never fetch it twice.
std::mutex g_download_mutex;
std::mutex g_error_mutex;
std::string g_error; // guarded by g_error_mutex

void keep_error(std::string reason) {
    std::lock_guard lock(g_error_mutex);
    g_error = std::move(reason);
}

std::string utf8(const fs::path& path) {
    return fv::base_utf8::path_to_utf8(path);
}

bool is_dir(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

bool is_regular(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool ends_with_onnx(const fs::path& name) {
    return py::lower(utf8(name)).ends_with(".onnx");
}

// os.walk(top) (top-down, symlinked folders listed but not entered, unreadable folders skipped)
// stopped at the first .onnx file of a folder's sorted file names.
std::optional<fs::path> walk_for_onnx(const fs::path& folder) {
    std::vector<std::pair<std::string, fs::path>> files;
    std::vector<std::pair<std::string, fs::path>> folders;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code type_error;
        const bool directory = it->is_directory(type_error);
        auto name = it->path().filename();
        if (directory) {
            const bool link = it->is_symlink(type_error);
            if (!link) {
                folders.emplace_back(utf8(name), it->path());
            }
        } else {
            files.emplace_back(utf8(name), it->path());
        }
    }
    if (error) {
        return std::nullopt;
    }
    // Code-point order, as sorted() orders str: UTF-8 bytes sort the same way.
    std::ranges::sort(files);
    for (const auto& [name, path] : files) {
        if (py::lower(name).ends_with(".onnx")) {
            return path;
        }
    }
    std::ranges::sort(folders);
    for (const auto& [name, path] : folders) {
        if (auto found = walk_for_onnx(path)) {
            return found;
        }
    }
    return std::nullopt;
}

// _env_speaker_model_path for one variable.
std::optional<fs::path> env_model(const EnvReader& env, const std::string& variable) {
    const std::string raw = py::strip(env.get(variable).value_or(std::string()));
    if (raw.empty()) {
        return std::nullopt;
    }
    const fs::path given = fv::base_utf8::path_from_utf8(raw);
    if (is_regular(given)) {
        if (!ends_with_onnx(given)) {
            kLog.warning("{} must point at an .onnx file (got {})", variable, raw);
            return std::nullopt;
        }
        std::error_code error;
        const fs::path absolute = fs::absolute(given, error);
        return error ? given : absolute.lexically_normal();
    }
    if (auto found = first_onnx_file(given)) {
        return found;
    }
    kLog.warning("{} is set but no .onnx file was found at {}", variable, raw);
    return std::nullopt;
}

// Python's truthiness of a saved JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

// The policy load_hf_access_policy would return for `settings`, without its migration write.
std::string saved_hf_policy(const Json& settings) {
    if (!settings.is_object()) {
        return std::string(hf_access_policy::kAsk);
    }
    const auto policy = settings.find(keys::hf_access_policy.key());
    if (policy != settings.end() && policy->is_string() &&
        std::ranges::find(hf_access_policy::kAll, policy->get_ref<const std::string&>()) !=
            hf_access_policy::kAll.end()) {
        return policy->get<std::string>();
    }
    const auto legacy = settings.find(keys::hf_hub_offline.key());
    return std::string(legacy != settings.end() && truthy(*legacy) ? hf_access_policy::kNever
                                                                   : hf_access_policy::kAsk);
}

std::string blocked_reason(std::string_view policy, const EnvReader& env) {
    if (is_hf_hub_offline_env_set(env)) {
        return std::string(kSpeakerModelEnvBlocked);
    }
    if (policy == hf_access_policy::kNever) {
        return std::string(kSpeakerModelPolicyBlocked);
    }
    return {};
}

// The first line of str.splitlines(): every boundary Python splits on ends it.
std::string first_line(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f' || byte == 0x1c ||
            byte == 0x1d || byte == 0x1e) {
            return std::string(text.substr(0, i));
        }
        // U+0085, U+2028 and U+2029 in UTF-8.
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("\xC2\x85") || rest.starts_with("\xE2\x80\xA8") ||
            rest.starts_with("\xE2\x80\xA9")) {
            return std::string(text.substr(0, i));
        }
    }
    return std::string(text);
}

// The strerror of str(OSError): os_error_text's "[WinError 5] Access is denied: 'C:\\x'" without
// the code in brackets and the file names Python appends.
std::string strerror_of(std::string_view os_text) {
    if (os_text.starts_with("[")) {
        if (const auto close = os_text.find("] "); close != std::string_view::npos) {
            os_text.remove_prefix(close + 2);
        }
    }
    if (const auto names = os_text.find(": '"); names != std::string_view::npos) {
        os_text = os_text.substr(0, names);
    }
    return std::string(os_text);
}

SpeakerDownloadFailure disk_failure(const std::error_code& code) {
    SpeakerDownloadFailure failure;
    failure.kind = SpeakerDownloadFailure::Kind::Disk;
    failure.message = download::os_error_text(code);
    failure.strerror = strerror_of(failure.message);
    failure.type_name = "OSError";
    return failure;
}

SpeakerDownloadFailure transport_failure(const std::optional<http::Error>& last) {
    SpeakerDownloadFailure failure;
    failure.kind = last && last->kind == http::ErrorKind::Timeout
                       ? SpeakerDownloadFailure::Kind::Timeout
                       : SpeakerDownloadFailure::Kind::Network;
    return failure;
}

// A failed verified download as the speaker model's sentence. `last` is the transport error of
// the transfer's last request, if it had one: the downloader's own failure record does not tell
// a timeout from a refused connection, nor a socket error after the request from a disk error.
std::string describe_download_failure(const download::DownloadError& error,
                                      const std::optional<http::Error>& last) {
    if (const auto& network = error.failure()) {
        switch (network->kind) {
        case download::NetworkFailure::Kind::HttpStatus: {
            SpeakerDownloadFailure failure;
            failure.kind = SpeakerDownloadFailure::Kind::HttpStatus;
            failure.status = network->code;
            return describe_speaker_download_error(failure);
        }
        case download::NetworkFailure::Kind::Certificate:
        case download::NetworkFailure::Kind::Url:
            return describe_speaker_download_error(transport_failure(last));
        case download::NetworkFailure::Kind::Other:
            if (last) {
                return describe_speaker_download_error(transport_failure(last));
            }
            {
                SpeakerDownloadFailure failure;
                failure.kind = SpeakerDownloadFailure::Kind::Disk;
                failure.message = network->reason;
                failure.strerror = strerror_of(network->reason);
                failure.type_name = "OSError";
                return describe_speaker_download_error(failure);
            }
        }
    }
    const std::string_view message = error.what();
    if (message == kSpeakerModelIntegrity) {
        return std::string(kSpeakerModelIntegrity);
    }
    // The downloader's own sentences end with a full stop; the sentence around them adds one.
    SpeakerDownloadFailure failure;
    failure.message =
        std::string(message.ends_with('.') ? message.substr(0, message.size() - 1) : message);
    failure.type_name = "DownloadError";
    return describe_speaker_download_error(failure);
}

// _download_speaker_model: the file into the cache, verified. Throws ComponentError with the
// user's sentence, or ComponentCanceled.
fs::path download_speaker_model(SpeakerModelDeps& deps, const CancelToken& cancel) {
    const fs::path cache = speaker_model_cache_dir(deps.paths);
    std::error_code made;
    fs::create_directories(cache, made);
    if (made) {
        throw ComponentError(describe_speaker_download_error(disk_failure(made)));
    }
    kLog.info("Downloading speaker embedding model {}/{}", kSpeakerModelRepo,
              kSpeakerModelFilename);
    const fs::path destination = cache / fv::base_utf8::path_from_utf8(kSpeakerModelFilename);

    std::optional<http::Error> last;
    download::DownloadOptions options;
    // A file of another size than the pinned one is another file, as a digest mismatch is.
    options.messages.integrity = std::string(kSpeakerModelIntegrity);
    options.messages.unexpected_length = std::string(kSpeakerModelIntegrity);
    options.messages.too_much_data = std::string(kSpeakerModelIntegrity);
    options.send = [&](const http::Request& request, const CancelToken& token,
                       http::BodySink& sink) -> download::SendResult {
        last.reset();
        auto sent = deps.client.send(request, token, &sink);
        if (!sent) {
            last = sent.error();
            return std::unexpected(std::variant<http::Error, download::NetworkFailure>(
                std::in_place_index<0>, std::move(sent.error())));
        }
        return std::move(*sent);
    };
    const download::ProgressCallback progress =
        deps.progress
            ? deps.progress
            : download::ProgressCallback([](std::string_view, std::int64_t, std::int64_t) {});
    try {
        download::download_verified(deps.client, deps.url, destination, deps.size_bytes,
                                    deps.sha256, progress, cancel,
                                    /*keep_partial_on_cancel=*/true, options);
    } catch (const download::DownloadCanceled&) {
        throw ComponentCanceled();
    } catch (const download::DownloadInterrupted&) {
        // The server closed the connection early: httpx reports that as a transport error.
        SpeakerDownloadFailure failure;
        failure.kind = SpeakerDownloadFailure::Kind::Network;
        throw ComponentError(describe_speaker_download_error(failure));
    } catch (const download::DownloadError& error) {
        throw ComponentError(describe_download_failure(error, last));
    }

    std::error_code error;
    fs::path resolved = fs::absolute(destination, error);
    if (error) {
        resolved = destination;
    }
    if (!is_regular(resolved)) {
        throw ComponentError(std::string(kSpeakerModelNoFile));
    }
    kLog.info("Speaker embedding model ready: {}", utf8(resolved));
    return resolved;
}

} // namespace

fs::path speaker_model_cache_dir(const Paths& paths) {
    return paths.local_app_dir() / "models" / "speaker-id";
}

std::optional<fs::path> first_onnx_file(const fs::path& root) {
    if (root.empty() || !is_dir(root)) {
        return std::nullopt;
    }
    return walk_for_onnx(root);
}

std::optional<fs::path> speaker_model_path(const Paths& paths, const EnvReader& env) {
    for (const AppIdentity* app : {&identity(), &legacy_identity()}) {
        if (auto found = env_model(env, std::string(app->env_prefix) + "SPEAKER_MODEL")) {
            return found;
        }
    }
    return first_onnx_file(speaker_model_cache_dir(paths));
}

std::string speaker_model_download_blocked(const Json& settings, const EnvReader& env) {
    return blocked_reason(saved_hf_policy(settings), env);
}

std::string speaker_model_error() {
    std::lock_guard lock(g_error_mutex);
    return g_error;
}

std::string describe_speaker_download_error(const SpeakerDownloadFailure& failure) {
    switch (failure.kind) {
    case SpeakerDownloadFailure::Kind::Timeout:
        return "Hugging Face did not respond in time, so the speaker model did not download.";
    case SpeakerDownloadFailure::Kind::Network:
        return "Could not reach Hugging Face to download the speaker model (network error).";
    case SpeakerDownloadFailure::Kind::HttpStatus:
        return std::format("Hugging Face refused the speaker model download (HTTP {}).",
                           failure.status);
    case SpeakerDownloadFailure::Kind::Disk:
        if (!failure.strerror.empty()) {
            return std::format("Could not save the speaker model: {}.", failure.strerror);
        }
        break;
    case SpeakerDownloadFailure::Kind::Other:
        break;
    }
    const std::string detail = py::slice(first_line(failure.message), 0, 160);
    return std::format("Could not download the speaker model: {}.",
                       detail.empty() ? failure.type_name : detail);
}

fs::path ensure_speaker_model(SpeakerModelDeps& deps, const CancelToken& cancel) {
    std::lock_guard lock(g_download_mutex);
    if (auto existing = speaker_model_path(deps.paths, deps.env)) {
        return *existing;
    }
    const std::string blocked = blocked_reason(load_hf_access_policy(deps.settings), deps.env);
    if (!blocked.empty()) {
        throw ComponentError(blocked);
    }
    fs::path path;
    try {
        path = download_speaker_model(deps, cancel);
    } catch (const ComponentCanceled&) {
        throw;
    } catch (const ComponentError& error) {
        kLog.warning("Speaker embedding model download failed: {}", error.what());
        keep_error(error.what());
        throw;
    } catch (const std::exception& error) {
        // Anything else is worded as Python words an exception it did not expect.
        SpeakerDownloadFailure failure;
        failure.message = error.what();
        failure.type_name = "Exception";
        const std::string reason = describe_speaker_download_error(failure);
        kLog.warning("Speaker embedding model download failed: {}", reason);
        keep_error(reason);
        throw ComponentError(reason);
    }
    keep_error({});
    return path;
}

namespace testing {

void reset_speaker_model_error() {
    keep_error({});
}

} // namespace testing

} // namespace fv::models
