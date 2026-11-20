#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/http_server.h"
#include "net/types.h"

// Starlette 1.6's FileResponse and StaticFiles as plain functions: what a file response says
// for a request, before any byte of the file is read. No Boost here.
namespace fv::net::detail {

// What FileResponse._parse_range_header makes of a Range header.
struct RangeRequest {
    enum class Kind { Whole, Ranges, Malformed, Unsatisfiable };
    Kind kind = Kind::Whole;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges; // [start, end), merged
    std::string message;                                         // Malformed: the 400 body
};

RangeRequest parse_range_header(std::string_view header, std::uint64_t size);
// A range given by the caller, checked as the header's single range is.
RangeRequest check_byte_range(const ByteRange& range, std::uint64_t size);

// email.utils.parsedate(text): (year, month, day, hour, minute, second), the time zone dropped.
std::optional<std::array<std::int64_t, 6>> parsedate(std::string_view text);

// What os.stat gives FileResponse.
struct FileFacts {
    std::uint64_t size = 0;
    std::int64_t mtime_seconds = 0; // st_mtime split as CPython fills it
    std::int64_t mtime_nanoseconds = 0;
};

// The regular file at `path`, or nullopt (missing, a directory, unreadable).
std::optional<FileFacts> stat_file(const std::filesystem::path& path);

// '"' + md5(f"{st_mtime}-{st_size}") + '"', st_mtime as repr(float).
std::string file_etag(const FileFacts& facts);
// formatdate(st_mtime, usegmt=True)
std::string file_last_modified(const FileFacts& facts);

// One part of a response body: literal bytes, then `length` bytes of the file from `offset`.
struct BodyPiece {
    std::string literal;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct FilePlan {
    int status = 200;
    Headers headers;               // names in lower case, in Starlette's order
    std::string body;              // a plain answer (400, 416, 304, 404): no file is read
    std::vector<BodyPiece> pieces; // empty unless the file is sent
    std::uint64_t content_length = 0;
};

// The whole answer of FileResponse(path, media_type=content_type, headers, filename,
// content_disposition_type) to a request with `request_headers`, StaticFiles' 304 first when
// options.not_modified_check. `boundary` separates the parts of a multi-range answer.
FilePlan plan_file_response(const FileFacts& facts, std::string_view content_type,
                            const std::optional<ByteRange>& range, const FileOptions& options,
                            const Headers& request_headers, std::string_view boundary);

// FastAPI's 404 for a file that is not there.
FilePlan not_found_plan();

// Python's secrets.token_hex(13): 26 lower-case hex digits.
std::string random_boundary();

// A file body being sent: its pieces, read in chunks as the socket takes them.
// Affinity: the session's strand.
class FileBody {
public:
    FileBody(std::ifstream file, std::vector<BodyPiece> pieces)
        : file_(std::move(file)), pieces_(std::move(pieces)) {}

    // The next bytes to send (at most `chunk` of file data), "" when everything was read, or
    // nullopt when the file ended early or failed.
    std::optional<std::string> next(std::size_t chunk);
    bool done() const { return index_ >= pieces_.size(); }

private:
    std::ifstream file_;
    std::vector<BodyPiece> pieces_;
    std::size_t index_ = 0;
    bool literal_sent_ = false;
    std::uint64_t sent_ = 0; // of the current piece's file bytes
};

} // namespace fv::net::detail
