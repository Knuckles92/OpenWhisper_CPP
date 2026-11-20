#include "models/detail/no_download_client.h"

namespace fv::models::detail {

namespace {

class NoDownloadClient final : public http::Client {
public:
    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken&,
                                                    http::BodySink*) override {
        return std::unexpected(
            http::Error{http::ErrorKind::Connect, "downloads are not used here"});
    }
};

} // namespace

http::Client& no_download_client() {
    static NoDownloadClient client;
    return client;
}

} // namespace fv::models::detail
