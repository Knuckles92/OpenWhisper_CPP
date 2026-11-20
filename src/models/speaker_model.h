#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/env.h"
#include "base/json.h"
#include "base/paths.h"
#include "download/verified_download.h"
#include "http/client.h"
#include "settings/store.h"

// The speaker-embedding model meetings label speakers with (services/components.py:174-184,
// 836-1073): WeSpeaker ResNet34-LM as ONNX, found locally or fetched once from Hugging Face.
// OpenWhisper downloads it with hf_hub_download and checks the digest afterwards; here it comes
// through the verified downloader from the same URL, so the size is pinned as well.
// Loading and running the model is fv_diarize's.
namespace fv::models {

inline constexpr std::string_view kSpeakerModelRepo = "Wespeaker/wespeaker-voxceleb-resnet34-LM";
inline constexpr std::string_view kSpeakerModelFilename = "voxceleb_resnet34_LM.onnx";
// The Hub branch, not a commit: OpenWhisper does not pin one, the digest does.
inline constexpr std::string_view kSpeakerModelRevision = "main";
inline constexpr std::string_view kSpeakerModelSha256 =
    "7bb2f06e9df17cdf1ef14ee8a15ab08ed28e8d0ef5054ee135741560df2ec068";
// Bytes of the file with that digest; the verified downloader needs the exact size up front.
inline constexpr std::int64_t kSpeakerModelSizeBytes = 26'530'309;
inline constexpr std::string_view kSpeakerModelUrl =
    "https://huggingface.co/Wespeaker/wespeaker-voxceleb-resnet34-LM/resolve/main/"
    "voxceleb_resnet34_LM.onnx";

// Why a download is not allowed (speaker_model_download_blocked's texts; not kept as errors).
inline constexpr std::string_view kSpeakerModelEnvBlocked =
    "Hugging Face downloads are disabled by HF_HUB_OFFLINE, so the speaker model can't download.";
inline constexpr std::string_view kSpeakerModelPolicyBlocked =
    "Hugging Face access is set to Never connect, so the speaker model can't download.";
// _verify_speaker_model's texts.
inline constexpr std::string_view kSpeakerModelNoFile =
    "The speaker model download produced no file.";
inline constexpr std::string_view kSpeakerModelIntegrity =
    "The speaker model failed its integrity check and was discarded.";

// <local_app_dir>/models/speaker-id: where ensure_speaker_model saves the model.
std::filesystem::path speaker_model_cache_dir(const Paths& paths);

// _first_onnx_file: the first file whose name ends ".onnx" (any case) under `root`, walking
// top-down with each folder's file names sorted (subfolders in name order too), or nullopt when
// `root` is not a folder or holds none.
std::optional<std::filesystem::path> first_onnx_file(const std::filesystem::path& root);

// A usable speaker model already on this computer, in this order: OPENWHISPER_CPP_SPEAKER_MODEL,
// then OPENWHISPER_SPEAKER_MODEL (each a .onnx file, made absolute, or a folder holding one;
// anything else is logged and skipped), then speaker_model_cache_dir. OpenWhisper's last place, the
// source checkout's models/speaker-id, does not exist for an installed app and is not consulted.
std::optional<std::filesystem::path> speaker_model_path(const Paths& paths, const EnvReader& env);

// Why the speaker model may not download now, or "": HF_HUB_OFFLINE set (kSpeakerModelEnvBlocked)
// wins over a saved Hugging Face policy of "never" (kSpeakerModelPolicyBlocked). `settings` is a
// settings snapshot; a legacy hf_hub_offline reads as load_hf_access_policy would migrate it,
// without saving anything.
std::string speaker_model_download_blocked(const Json& settings, const EnvReader& env);

// Why this process's last speaker-model download failed; "" once one works (or before any).
// Blocked downloads and cancels are not failures and leave it as it was.
std::string speaker_model_error();

// A failed download, told apart the way _describe_speaker_download_error tells Python's
// exception chain apart (any timeout, then any transport error, then the first HTTP status, then
// the first OSError with a strerror, else the failure itself).
struct SpeakerDownloadFailure {
    enum class Kind {
        Timeout,    // httpx.TimeoutException: connect, read, write or pool timeout
        Network,    // httpx.TransportError: refused, reset, TLS, a cut or malformed answer
        HttpStatus, // the server answered `status`
        Disk,       // an OSError: `strerror` ("" falls through to the failure itself)
        Other,
    };
    Kind kind = Kind::Other;
    int status = 0;
    std::string strerror;
    // str() of the failure, whose first line (at most 160 code points) is shown, and the type
    // name shown when that line is empty.
    std::string message;
    std::string type_name;
};

// The failure as one sentence for the user: "Hugging Face did not respond in time, so the
// speaker model did not download.", "Could not reach Hugging Face to download the speaker model
// (network error).", "Hugging Face refused the speaker model download (HTTP {status}).", "Could
// not save the speaker model: {strerror}." or "Could not download the speaker model: {first line
// or type name}."
std::string describe_speaker_download_error(const SpeakerDownloadFailure& failure);

// What ensure_speaker_model reads and downloads with. The file to fetch is the pinned model;
// tests point it at a loopback server and a small file of their own.
struct SpeakerModelDeps {
    SpeakerModelDeps(const Paths& paths, const EnvReader& env, SettingsStore& settings,
                     http::Client& client)
        : paths(paths), env(env), settings(settings), client(client) {}

    const Paths& paths;
    const EnvReader& env;
    SettingsStore& settings; // the Hugging Face access policy (a legacy one is migrated)
    http::Client& client;
    std::string url{kSpeakerModelUrl};
    std::string sha256{kSpeakerModelSha256};
    std::int64_t size_bytes = kSpeakerModelSizeBytes;
    download::ProgressCallback progress; // empty: none
};

// A local speaker model, downloaded first when there is none and downloads are allowed. One
// download at a time in the process: a caller that arrives during another's waits, then finds
// its file. Throws ComponentError with the blocked text, or with the failure's sentence
// (describe_speaker_download_error, kSpeakerModelIntegrity, kSpeakerModelNoFile), which
// speaker_model_error() then keeps; ComponentCanceled when `cancel` fires (an interrupted
// transfer stays as <file>.part and resumes next time). Blocking: never on the GUI thread.
std::filesystem::path ensure_speaker_model(SpeakerModelDeps& deps, const CancelToken& cancel = {});

} // namespace fv::models
