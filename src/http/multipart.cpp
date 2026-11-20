#include "http/multipart.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "base/utf8.h"
#include "http/detail/curl_mime.h"

namespace fv::http {

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// httpx's escaping of a Content-Disposition parameter (_format_form_param): '"' as %22, '\' as
// two of them, control characters but ESC as %XX. Applied before curl sees a name, so curl's
// own HTML5 escaping (of '"', CR and LF) finds nothing left to change.
std::string form_quoted(std::string_view value) {
    std::string out;
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "%22";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (byte <= 0x1F && byte != 0x1B) {
            out += std::format("%{:02X}", byte);
        } else {
            out += c;
        }
    }
    return out;
}

// A file part's source while curl sends it.
struct FileSource {
    std::ifstream stream;
};

std::size_t read_file(char* buffer, std::size_t size, std::size_t count, void* user) {
    auto& source = *static_cast<FileSource*>(user);
    source.stream.read(buffer, static_cast<std::streamsize>(size * count));
    if (source.stream.bad()) {
        return CURL_READFUNC_ABORT;
    }
    return static_cast<std::size_t>(source.stream.gcount());
}

// curl rewinds a part when it has to send the body again (a redirect it follows).
int seek_file(void* user, curl_off_t offset, int origin) {
    auto& source = *static_cast<FileSource*>(user);
    source.stream.clear();
    const auto direction = origin == SEEK_CUR   ? std::ios::cur
                           : origin == SEEK_END ? std::ios::end
                                                : std::ios::beg;
    source.stream.seekg(static_cast<std::streamoff>(offset), direction);
    return source.stream ? CURL_SEEKFUNC_OK : CURL_SEEKFUNC_FAIL;
}

void free_file(void* user) {
    delete static_cast<FileSource*>(user);
}

} // namespace

MultipartPart form_field(std::string name, std::string value) {
    return MultipartPart{std::move(name), {}, {}, std::move(value)};
}

MultipartPart file_part(std::string name, std::filesystem::path path, std::string content_type,
                        std::string filename) {
    if (filename.empty()) {
        filename = path_to_utf8(path.filename());
    }
    return MultipartPart{std::move(name), std::move(filename), std::move(content_type),
                         std::move(path)};
}

std::string content_type_for(const std::filesystem::path& file) {
    std::string extension = path_to_utf8(file.extension());
    std::ranges::transform(extension, extension.begin(), ascii_lower);
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kTypes{{
        {".wav", "audio/wav"},
        {".mp3", "audio/mpeg"},
        {".m4a", "audio/x-m4a"},
        {".ogg", "audio/ogg"},
        {".flac", "audio/x-flac"},
        {".wma", "audio/x-ms-wma"},
    }};
    for (const auto& [suffix, type] : kTypes) {
        if (suffix == extension) {
            return std::string(type);
        }
    }
    return "application/octet-stream";
}

std::string encode_multipart(const Multipart& multipart, std::string_view boundary) {
    std::string out;
    for (const MultipartPart& part : multipart.parts) {
        out += "--";
        out += boundary;
        out += "\r\nContent-Disposition: form-data; name=\"" + form_quoted(part.name) + "\"";
        if (!part.filename.empty()) {
            out += "; filename=\"" + form_quoted(part.filename) + "\"";
        }
        out += "\r\n";
        if (!part.content_type.empty()) {
            out += "Content-Type: " + part.content_type + "\r\n";
        }
        out += "\r\n";
        if (const auto* bytes = std::get_if<std::string>(&part.data)) {
            out += *bytes;
        } else {
            const auto& path = std::get<std::filesystem::path>(part.data);
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("the request body file could not be read");
            }
            out.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        out += "\r\n";
    }
    out += "--";
    out += boundary;
    out += "--\r\n";
    return out;
}

namespace detail {

std::expected<MimePtr, std::string> make_curl_mime(CURL* easy, const Multipart& multipart) {
    MimePtr mime(curl_mime_init(easy));
    if (mime == nullptr) {
        return std::unexpected("curl_mime_init failed");
    }
    for (const MultipartPart& part : multipart.parts) {
        curl_mimepart* added = curl_mime_addpart(mime.get());
        if (added == nullptr) {
            return std::unexpected("curl_mime_addpart failed");
        }
        curl_mime_name(added, form_quoted(part.name).c_str());
        if (const auto* bytes = std::get_if<std::string>(&part.data)) {
            curl_mime_data(added, bytes->data(), bytes->size());
        } else {
            const auto& path = std::get<std::filesystem::path>(part.data);
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            auto source = std::make_unique<FileSource>();
            // Opened by path, never by a narrow file name, so any Unicode path works.
            source->stream.open(path, std::ios::binary);
            if (error || !source->stream) {
                return std::unexpected("the request body file could not be read");
            }
            // curl owns the source from here and frees it with the form.
            curl_mime_data_cb(added, static_cast<curl_off_t>(size), &read_file, &seek_file,
                              &free_file, source.release());
        }
        if (!part.filename.empty()) {
            curl_mime_filename(added, form_quoted(part.filename).c_str());
        }
        if (!part.content_type.empty()) {
            curl_mime_type(added, part.content_type.c_str());
        }
    }
    return mime;
}

} // namespace detail

} // namespace fv::http
