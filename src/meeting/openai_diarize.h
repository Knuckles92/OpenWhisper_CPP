#pragma once

#include <memory>
#include <string>

#include "http/client.h"
#include "meeting/cloud_pass.h"

namespace fv::meeting {

// The production request is confined to OpenAI's fixed HTTPS endpoint, with no redirects.
DiarizeFn make_openai_diarize(std::shared_ptr<http::Client> client, std::string api_key);

} // namespace fv::meeting
