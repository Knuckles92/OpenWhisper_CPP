// Sweep B28: once standard input closes, ChannelServer's destructor does not wait out a reply
// still waiting on an app that never answers (Python's daemon reply threads let it exit at
// once). An in-process client stands in for the app; nothing is sent anywhere.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "pyport/json.h"
#include "support/channel_support.h"

using namespace fv;
using namespace fv::automation;
using namespace fv::automation::channel_test;
using namespace std::chrono_literals;

namespace {

// An app that takes the request and never answers: the call returns only when cancelled, or
// after half a minute.
class SilentApp final : public http::Client {
public:
    std::expected<http::Response, http::Error> send(const http::Request&, const CancelToken& cancel,
                                                    http::BodySink*) override {
        ++calls;
        if (cancel.wait_for(30s)) {
            return std::unexpected(http::Error{http::ErrorKind::Cancelled, "canceled"});
        }
        return std::unexpected(http::Error{http::ErrorKind::Timeout, "timed out"});
    }
    std::atomic<int> calls{0};
};

} // namespace

TEST_CASE("the channel server exits without waiting for a reply the app never answers",
          "[sweep][channel_bridge]") {
    auto app = std::make_shared<SilentApp>();
    AppLinkOptions options;
    options.url = "http://127.0.0.1:9";
    options.token = std::string(40, 't');
    options.read_settings = [] {
        return Json::object();
    };
    options.read_token = [] {
        return std::optional<std::string>();
    };
    options.client = app;
    AppLink link(options);
    Pipe in;
    Output out;
    auto server = std::make_unique<ChannelServer>(in, out, link);
    std::thread serving([&] { server->serve(); });

    in.put(py::loads(R"({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                         "params": {"name": "reply", "arguments": {"text": "Done."}}})"));
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (app->calls == 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(app->calls == 1);
    in.close();
    serving.join();

    const auto started = std::chrono::steady_clock::now();
    server.reset();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK(app->calls == 1);
}
