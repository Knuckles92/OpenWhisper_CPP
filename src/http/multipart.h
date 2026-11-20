#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "http/client.h"

namespace fv::http {

// A plain form field, as httpx sends one: no filename, no Content-Type.
MultipartPart form_field(std::string name, std::string value);

// A file streamed from disk when the request is sent. `filename` empty: the path's base name,
// which is what httpx sends for an open file.
MultipartPart file_part(std::string name, std::filesystem::path path, std::string content_type,
                        std::string filename = {});

// The Content-Type httpx gives a file part from its name (mimetypes.guess_type as Windows
// registers the audio types), "application/octet-stream" for anything else.
std::string content_type_for(const std::filesystem::path& file);

// The whole body as RFC 7578 bytes with `boundary`, file parts read now, in part order: what the
// curl client streams, for fakes and tests. Throws std::runtime_error when a file part cannot be
// read.
std::string encode_multipart(const Multipart&, std::string_view boundary);

} // namespace fv::http
