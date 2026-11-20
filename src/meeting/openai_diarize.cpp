#include "meeting/openai_diarize.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "http/multipart.h"
#include "meeting/detail/py_value.h"
#include "pyport/json.h"
#include "speech/api_models.h"

namespace fv::meeting {
namespace {

using namespace std::chrono_literals;
constexpr std::string_view kEndpoint = "https://api.openai.com/v1/audio/transcriptions";

std::optional<std::string> error_code(std::string_view body) {
    try {
        const auto parsed = py::loads(body);
        const auto& error = detail::get(parsed, "error");
        const auto& code = detail::get(error.is_null() ? parsed : error, "code");
        if (code.is_string()) {
            return code.get<std::string>();
        }
    } catch (const std::exception&) {
        // Provider bodies may echo audio metadata. Never include them in app messages.
    }
    return std::nullopt;
}

} // namespace

DiarizeFn make_openai_diarize(std::shared_ptr<http::Client> client, std::string api_key) {
    return [client = std::move(client), key = std::move(api_key)](
               const DiarizeRequest& audio, const CancelToken& cancel) -> Json {
        if (!client || key.empty() || audio.audio.empty() || cancel.cancelled()) {
            throw std::runtime_error("The approved speaker upload is unavailable.");
        }
        http::Multipart body;
        body.parts.push_back(http::form_field("model", std::string(kDiarizeModel)));
        body.parts.push_back(http::form_field("response_format", "diarized_json"));
        body.parts.push_back(http::form_field("chunking_strategy", "auto"));
        body.parts.push_back({"file", audio.filename, audio.content_type, audio.audio});
        if (audio.known_speaker_names.size() == audio.known_speaker_references.size() &&
            audio.known_speaker_names.size() <= kMaxKnownSpeakers) {
            for (std::size_t i = 0; i < audio.known_speaker_names.size(); ++i) {
                body.parts.push_back(
                    http::form_field("known_speaker_names[]", audio.known_speaker_names[i]));
                body.parts.push_back(http::form_field("known_speaker_references[]",
                                                      audio.known_speaker_references[i]));
            }
        }
        http::Request request;
        request.method = "POST";
        request.url = kEndpoint;
        request.headers = {{"Accept", "application/json"}, {"Authorization", "Bearer " + key}};
        request.body = std::move(body);
        request.connect_timeout = 5s;
        request.idle_timeout = 120s;
        request.deadline = std::chrono::steady_clock::now() + 3min;
        request.max_body_bytes = 4u << 20;
        request.bypass_proxy = true;
        const auto response = client->send(request, cancel);
        if (!response) {
            throw speech::SpeechApiError("OpenAI speaker identification could not connect.",
                                         std::nullopt, std::nullopt);
        }
        if (response->status < 200 || response->status >= 300) {
            throw speech::SpeechApiError("OpenAI declined the speaker identification request.",
                                         response->status, error_code(response->body));
        }
        const auto answer = py::loads(response->body);
        if (!detail::get(answer, "segments").is_array()) {
            throw std::runtime_error("OpenAI returned no speaker-turn information.");
        }
        return Json(parse_diarized_result(answer));
    };
}

} // namespace fv::meeting
