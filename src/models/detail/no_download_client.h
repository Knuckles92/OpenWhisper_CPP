#pragma once

#include "http/client.h"

namespace fv::models::detail {

// A client for stores that only work with files already on this computer (adoption): every
// request fails as a refused connection, so nothing is ever fetched through it.
http::Client& no_download_client();

} // namespace fv::models::detail
