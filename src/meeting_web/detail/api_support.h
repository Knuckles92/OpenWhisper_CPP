#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "net/http_server.h"

// What the dashboard's route files share: FastAPI's query parameter validation, the path
// parameters and api.py's limits and sentences.
namespace fv::meeting_web::detail {

inline constexpr std::int64_t kTranscriptPageDefault = 500;
inline constexpr std::int64_t kTranscriptPageMax = 1000;
inline constexpr std::int64_t kHistoryPageDefault = 50;
inline constexpr std::int64_t kHistoryPageMax = 100;
// An End request slower than this is logged with where its time went (seconds).
inline constexpr double kSlowEndWarnS = 1.0;
// Longest note request the dashboard accepts, in characters.
inline constexpr std::int64_t kNoteRequestMaxChars = 4000;
// Bodies the POST routes read; Starlette reads any size, this bounds what one request holds.
inline constexpr std::size_t kMaxJsonBody = std::size_t{16} << 20;

// What the routes that need the meeting intelligence answer without it.
inline constexpr std::string_view kAiInsightsOff =
    "Turn on AI insights in an active meeting first.";
inline constexpr std::string_view kCloudConsentMissing =
    "Turn on AI insights once from the desktop app first; it asks for consent before meeting "
    "text is sent to the AI provider.";
// meeting/semantic_search.py.
inline constexpr std::string_view kSemanticDisabled =
    "Keyword results. Enable Semantic history search under Meeting settings → Fast "
    "judgments for meaning-based ranking.";
inline constexpr std::string_view kSemanticUnavailable =
    "Semantic ranking unavailable. Showing keyword results.";

// The query parameters of one route, read as FastAPI reads them: the last value of a repeated
// name, the declared default when absent, pydantic's lax int and bool parsing. Every error is
// kept, in declaration order, and finish() throws them as one 422.
class QueryReader {
public:
    explicit QueryReader(const net::Query& query) : query_(query) {}

    std::string text(std::string_view name, std::string_view fallback = "");
    std::int64_t integer(std::string_view name, std::int64_t fallback);
    std::optional<std::int64_t> optional_integer(std::string_view name);
    bool boolean(std::string_view name, bool fallback);
    // HttpError 422 {"detail": [{type, loc, msg, input}, ...]} after a failed read.
    void finish() const;

private:
    std::optional<std::string> last(std::string_view name) const;
    void fail(std::string_view name, std::string_view type, std::string_view message,
              const std::string& input);

    const net::Query& query_;
    Json errors_ = Json::array();
};

// The "{name}" segment of the matched route ("" when absent).
std::string path_param(const net::HttpRequest& request, std::string_view name);

// JSONResponse.render: json.dumps(ensure_ascii=False, allow_nan=False, separators=(",", ":")).
std::string render_json(const Json& value);

// Starlette's Response(content, media_type): text/* gains "; charset=utf-8".
std::string media_type_header(std::string_view media_type);

} // namespace fv::meeting_web::detail
