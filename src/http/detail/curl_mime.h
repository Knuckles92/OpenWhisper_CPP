#pragma once

#include <curl/curl.h>

#include <expected>
#include <memory>
#include <string>

#include "http/client.h"

namespace fv::http::detail {

struct MimeDeleter {
    void operator()(curl_mime* mime) const { curl_mime_free(mime); }
};
using MimePtr = std::unique_ptr<curl_mime, MimeDeleter>;

// The curl form for `multipart`, its file parts read from disk while the body is sent (opened
// and sized here, so a missing file fails before connecting). The error is the message of an
// ErrorKind::Protocol failure.
std::expected<MimePtr, std::string> make_curl_mime(CURL* easy, const Multipart& multipart);

} // namespace fv::http::detail
