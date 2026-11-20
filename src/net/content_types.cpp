#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/utf8.h"
#include "net/detail/wire.h"
#include "net/http_server.h"
#include "pyport/number.h"

namespace fv::net {

namespace {

// mimetypes._types_map_default of Python 3.12 (strict), plus the two web font types it lacks.
constexpr std::pair<std::string_view, std::string_view> kTypes[] = {
    {".js", "text/javascript"},
    {".mjs", "text/javascript"},
    {".json", "application/json"},
    {".webmanifest", "application/manifest+json"},
    {".doc", "application/msword"},
    {".dot", "application/msword"},
    {".wiz", "application/msword"},
    {".nq", "application/n-quads"},
    {".nt", "application/n-triples"},
    {".bin", "application/octet-stream"},
    {".a", "application/octet-stream"},
    {".dll", "application/octet-stream"},
    {".exe", "application/octet-stream"},
    {".o", "application/octet-stream"},
    {".obj", "application/octet-stream"},
    {".so", "application/octet-stream"},
    {".oda", "application/oda"},
    {".pdf", "application/pdf"},
    {".p7c", "application/pkcs7-mime"},
    {".ps", "application/postscript"},
    {".ai", "application/postscript"},
    {".eps", "application/postscript"},
    {".trig", "application/trig"},
    {".m3u", "application/vnd.apple.mpegurl"},
    {".m3u8", "application/vnd.apple.mpegurl"},
    {".xls", "application/vnd.ms-excel"},
    {".xlb", "application/vnd.ms-excel"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pot", "application/vnd.ms-powerpoint"},
    {".ppa", "application/vnd.ms-powerpoint"},
    {".pps", "application/vnd.ms-powerpoint"},
    {".pwz", "application/vnd.ms-powerpoint"},
    {".wasm", "application/wasm"},
    {".bcpio", "application/x-bcpio"},
    {".cpio", "application/x-cpio"},
    {".csh", "application/x-csh"},
    {".dvi", "application/x-dvi"},
    {".gtar", "application/x-gtar"},
    {".hdf", "application/x-hdf"},
    {".h5", "application/x-hdf5"},
    {".latex", "application/x-latex"},
    {".mif", "application/x-mif"},
    {".cdf", "application/x-netcdf"},
    {".nc", "application/x-netcdf"},
    {".p12", "application/x-pkcs12"},
    {".pfx", "application/x-pkcs12"},
    {".ram", "application/x-pn-realaudio"},
    {".pyc", "application/x-python-code"},
    {".pyo", "application/x-python-code"},
    {".sh", "application/x-sh"},
    {".shar", "application/x-shar"},
    {".swf", "application/x-shockwave-flash"},
    {".sv4cpio", "application/x-sv4cpio"},
    {".sv4crc", "application/x-sv4crc"},
    {".tar", "application/x-tar"},
    {".tcl", "application/x-tcl"},
    {".tex", "application/x-tex"},
    {".texi", "application/x-texinfo"},
    {".texinfo", "application/x-texinfo"},
    {".roff", "application/x-troff"},
    {".t", "application/x-troff"},
    {".tr", "application/x-troff"},
    {".man", "application/x-troff-man"},
    {".me", "application/x-troff-me"},
    {".ms", "application/x-troff-ms"},
    {".ustar", "application/x-ustar"},
    {".src", "application/x-wais-source"},
    {".xsl", "application/xml"},
    {".rdf", "application/xml"},
    {".wsdl", "application/xml"},
    {".xpdl", "application/xml"},
    {".zip", "application/zip"},
    {".3gp", "audio/3gpp"},
    {".3gpp", "audio/3gpp"},
    {".3g2", "audio/3gpp2"},
    {".3gpp2", "audio/3gpp2"},
    {".aac", "audio/aac"},
    {".adts", "audio/aac"},
    {".loas", "audio/aac"},
    {".ass", "audio/aac"},
    {".au", "audio/basic"},
    {".snd", "audio/basic"},
    {".mp3", "audio/mpeg"},
    {".mp2", "audio/mpeg"},
    {".opus", "audio/opus"},
    {".aif", "audio/x-aiff"},
    {".aifc", "audio/x-aiff"},
    {".aiff", "audio/x-aiff"},
    {".ra", "audio/x-pn-realaudio"},
    {".wav", "audio/x-wav"},
    {".avif", "image/avif"},
    {".bmp", "image/bmp"},
    {".gif", "image/gif"},
    {".ief", "image/ief"},
    {".jpg", "image/jpeg"},
    {".jpe", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".heic", "image/heic"},
    {".heif", "image/heif"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".tiff", "image/tiff"},
    {".tif", "image/tiff"},
    {".ico", "image/vnd.microsoft.icon"},
    {".ras", "image/x-cmu-raster"},
    {".pnm", "image/x-portable-anymap"},
    {".pbm", "image/x-portable-bitmap"},
    {".pgm", "image/x-portable-graymap"},
    {".ppm", "image/x-portable-pixmap"},
    {".rgb", "image/x-rgb"},
    {".xbm", "image/x-xbitmap"},
    {".xpm", "image/x-xpixmap"},
    {".xwd", "image/x-xwindowdump"},
    {".eml", "message/rfc822"},
    {".mht", "message/rfc822"},
    {".mhtml", "message/rfc822"},
    {".nws", "message/rfc822"},
    {".css", "text/css"},
    {".csv", "text/csv"},
    {".html", "text/html"},
    {".htm", "text/html"},
    {".md", "text/markdown"},
    {".markdown", "text/markdown"},
    {".n3", "text/n3"},
    {".txt", "text/plain"},
    {".bat", "text/plain"},
    {".c", "text/plain"},
    {".h", "text/plain"},
    {".ksh", "text/plain"},
    {".pl", "text/plain"},
    {".srt", "text/plain"},
    {".rtx", "text/richtext"},
    {".tsv", "text/tab-separated-values"},
    {".vtt", "text/vtt"},
    {".py", "text/x-python"},
    {".rst", "text/x-rst"},
    {".etx", "text/x-setext"},
    {".sgm", "text/x-sgml"},
    {".sgml", "text/x-sgml"},
    {".vcf", "text/x-vcard"},
    {".xml", "text/xml"},
    {".mp4", "video/mp4"},
    {".mpeg", "video/mpeg"},
    {".m1v", "video/mpeg"},
    {".mpa", "video/mpeg"},
    {".mpe", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".mov", "video/quicktime"},
    {".qt", "video/quicktime"},
    {".webm", "video/webm"},
    {".avi", "video/x-msvideo"},
    {".movie", "video/x-sgi-movie"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
};

constexpr std::pair<std::string_view, std::string_view> kSuffixes[] = {
    {".svgz", ".svg.gz"}, {".tgz", ".tar.gz"},   {".taz", ".tar.gz"},
    {".tz", ".tar.gz"},   {".tbz2", ".tar.bz2"}, {".txz", ".tar.xz"},
};

// Compared with their case, as Python compares them.
constexpr std::array<std::string_view, 5> kEncodings = {".gz", ".Z", ".bz2", ".xz", ".br"};

// posixpath.splitext: the extension after the last dot that is not a leading one.
std::pair<std::string, std::string> splitext(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        return {name, {}};
    }
    if (name.find_first_not_of('.') >= dot) {
        return {name, {}};
    }
    return {name.substr(0, dot), name.substr(dot)};
}

std::string_view strip_spaces(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\n\r\v\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\n\r\v\f");
    return text.substr(first, last - first + 1);
}

} // namespace

std::string content_type_for(const std::filesystem::path& path) {
    auto [base, ext] = splitext(path_to_utf8(path.filename()));
    while (true) {
        const std::string lowered = detail::ascii_lower(ext);
        const auto* suffix =
            std::ranges::find(kSuffixes, std::string_view(lowered),
                              &std::pair<std::string_view, std::string_view>::first);
        if (suffix == std::end(kSuffixes)) {
            break;
        }
        std::tie(base, ext) = splitext(base + std::string(suffix->second));
    }
    if (std::ranges::find(kEncodings, std::string_view(ext)) != kEncodings.end()) {
        std::tie(base, ext) = splitext(base);
    }
    const std::string lowered = detail::ascii_lower(ext);
    for (const auto& [suffix, type] : kTypes) {
        if (suffix == lowered) {
            return std::string(type);
        }
    }
    return "application/octet-stream";
}

std::optional<ByteRange> parse_range(std::string_view header, std::uint64_t size) {
    (void)size;
    const auto equals = header.find('=');
    if (equals == std::string_view::npos ||
        detail::ascii_lower(strip_spaces(header.substr(0, equals))) != "bytes") {
        return std::nullopt;
    }
    std::optional<ByteRange> found;
    std::string_view rest = header.substr(equals + 1);
    while (true) {
        const auto comma = rest.find(',');
        const std::string_view part = strip_spaces(rest.substr(0, comma));
        if (!part.empty() && part != "-" && part.contains('-')) {
            const auto dash = part.find('-');
            const std::string_view first = strip_spaces(part.substr(0, dash));
            const std::string_view last = strip_spaces(part.substr(dash + 1));
            const auto number = [](std::string_view text) -> std::optional<std::uint64_t> {
                const auto value = py::py_int(text);
                if (!value || *value < 0) {
                    return std::nullopt;
                }
                return static_cast<std::uint64_t>(*value);
            };
            ByteRange range;
            bool valid = true;
            if (!first.empty()) {
                range.first = number(first);
                valid = range.first.has_value();
            }
            if (valid && !last.empty()) {
                range.last = number(last);
                valid = range.last.has_value();
            }
            if (valid) {
                if (found) {
                    return std::nullopt; // more than one range
                }
                found = range;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return found;
}

} // namespace fv::net