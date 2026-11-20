// tests/test_typesafe_service.py and the TypeSafe half of tests/test_network_reuse.py: answer
// validation, the failure policy, settings gating, key verification, the OpenRouter route, the
// following judge, and the keep-alive transport against a loopback server that counts
// connections. No request leaves 127.0.0.1: requests to the pinned endpoints go to fake
// connections, and the machine's proxy settings are never read.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>

// <windows.h>, which Asio brings in, defines these as empty macros; golden::near is a function.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "generation_test_support.h"
#include "golden/golden.h"
#include "http/proxy.h"
#include "http/testing/fake_http_client.h"
#include "llm/detail/keep_alive.h"
#include "llm/judge.h"
#include "llm/testing/fake_judge.h"
#include "llm/typesafe.h"
#include "llm_test_support.h"
#include "pyport/json.h"
#include "pyport/text.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

const Json kNoulQ = Json{{"q", Json{{"type", "noul"}, {"instructions", "Is it?"}}}};
const Json kChoiceQ = Json{{"q", Json{{"type", "choice"},
                                      {"instructions", "Which?"},
                                      {"criteria", Json{{"a", "A"}, {"b", "B"}}}}}};
const Json kScoreQ = Json{{"q", Json{{"type", "score"},
                                     {"instructions", "How?"},
                                     {"criteria", Json::array({"low", "mid", "high"})}}}};
const Json kGood = Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.9}}}}}};
const Json kGoodVerify = Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 1.0}}}}}};

std::string product() {
    return std::string(identity().product);
}

// RecordingTransport: answers one status and body (or fails with an exception class) and keeps
// every payload and budget it was given.
struct Recording {
    int status = 200;
    std::string body;
    std::optional<std::string> raises;
    mutable std::mutex mutex;
    std::vector<std::pair<Json, double>> calls;

    std::vector<std::pair<Json, double>> seen() const {
        std::lock_guard lock(mutex);
        return calls;
    }
};

std::shared_ptr<Recording> recording(int status, const Json& body) {
    auto made = std::make_shared<Recording>();
    made->status = status;
    made->body = body.is_string() ? body.get<std::string>() : py::dumps(body);
    return made;
}

std::shared_ptr<Recording> raising(std::string name) {
    auto made = std::make_shared<Recording>();
    made->raises = std::move(name);
    return made;
}

JudgeTransport transport_of(const std::shared_ptr<Recording>& recorder) {
    return [recorder](const Json& payload,
                      double timeout_s) -> std::expected<TransportReply, std::string> {
        std::lock_guard lock(recorder->mutex);
        recorder->calls.emplace_back(payload, timeout_s);
        if (recorder->raises) {
            return std::unexpected(*recorder->raises);
        }
        return TransportReply{recorder->status, recorder->body};
    };
}

// The machine's proxy settings are never read here.
std::optional<std::string> no_proxy(std::string_view) {
    return std::nullopt;
}

// Keys from a map environment, a fake HTTP client for the one-shot path, and a judge
// environment over them.
struct Fixture {
    explicit Fixture(JudgeHooks hooks = {}) {
        if (!hooks.proxy_for) {
            hooks.proxy_for = no_proxy;
        }
        env = std::make_unique<JudgeEnvironment>(http, keys.store, keys.env, keys.dotenv,
                                                 std::move(hooks));
    }

    TestKeys keys;
    std::shared_ptr<http::FakeHttpClient> http = std::make_shared<http::FakeHttpClient>();
    std::unique_ptr<JudgeEnvironment> env;
};

// Connections to the pinned endpoints that never leave the process: one fake client serves
// every connection the pool opens, and counts how many it was asked for.
struct FakeConnections {
    std::shared_ptr<http::FakeHttpClient> client = std::make_shared<http::FakeHttpClient>();
    std::shared_ptr<std::atomic<int>> made = std::make_shared<std::atomic<int>>(0);

    JudgeHooks hooks() const {
        JudgeHooks hooks;
        hooks.new_connection = [client = client, made = made]() -> std::shared_ptr<http::Client> {
            ++*made;
            return client;
        };
        return hooks;
    }
};

// A log sink that keeps each record's level and logger.
class LevelSink final : public log::Sink {
public:
    struct Line {
        log::Level level;
        std::string logger;
        std::string message;
    };
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        lines_.push_back({record.level, std::string(record.logger), std::string(record.message)});
    }
    std::vector<Line> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
};

struct LevelScope {
    LevelScope() : previous(log::level()) {
        log::set_level(log::Level::Debug);
        log::add_sink(sink);
    }
    ~LevelScope() {
        log::remove_sink(sink);
        log::set_level(previous);
    }
    LevelScope(const LevelScope&) = delete;
    LevelScope& operator=(const LevelScope&) = delete;

    std::size_t count(log::Level level, std::string_view logger) const {
        std::size_t n = 0;
        for (const auto& line : sink->lines()) {
            n += line.level == level && line.logger == logger ? 1 : 0;
        }
        return n;
    }

    log::Level previous;
    std::shared_ptr<LevelSink> sink = std::make_shared<LevelSink>();
};

// An HTTP/1.1 server on 127.0.0.1 that keeps connections open between requests, as TypeSafe
// does, numbers each connection's requests and lets a test decide how each is answered.
class KeepAliveServer {
public:
    struct Seen {
        unsigned short port = 0;
        std::optional<std::string> auth;
        std::string body;
    };

    class Connection {
    public:
        Connection(boost::asio::ip::tcp::socket& socket, KeepAliveServer& server)
            : socket_(socket), server_(server) {}
        // A JSON answer; with `close_after`, the connection then closes without saying so.
        void answer(int status = 200, const std::string& body = py::dumps(kGood),
                    bool close_after = false) {
            const std::string reason = status == 200 ? "OK" : "Error";
            const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                                     "\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\n\r\n";
            boost::system::error_code ignored;
            boost::asio::write(socket_, boost::asio::buffer(head + body), ignored);
            closed_ = close_after;
        }
        // Closes the connection without answering, as a server reaping it would.
        void drop() { closed_ = true; }
        // Sleeps, waking early when the server stops; false then.
        bool sleep(std::chrono::milliseconds duration) {
            std::unique_lock lock(server_.mutex_);
            return !server_.wake_.wait_for(lock, duration, [this] { return server_.stopping_; });
        }
        unsigned short port() const {
            boost::system::error_code ignored;
            return socket_.remote_endpoint(ignored).port();
        }
        bool closed() const { return closed_; }

    private:
        boost::asio::ip::tcp::socket& socket_;
        KeepAliveServer& server_;
        bool closed_ = false;
    };

    using Behavior = std::function<void(Connection&, int nth)>;

    KeepAliveServer()
        : acceptor_(io_,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        behavior_ = [](Connection& connection, int) {
            connection.answer();
        };
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~KeepAliveServer() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        try {
            boost::asio::io_context io;
            boost::asio::ip::tcp::socket socket(io);
            socket.connect(acceptor_.local_endpoint());
        } catch (...) {
        }
        accept_thread_.join();
        std::list<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            for (const auto& socket : sockets_) {
                boost::system::error_code ignored;
                socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            }
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }

    KeepAliveServer(const KeepAliveServer&) = delete;
    KeepAliveServer& operator=(const KeepAliveServer&) = delete;

    void set_behavior(Behavior behavior) {
        std::lock_guard lock(mutex_);
        behavior_ = std::move(behavior);
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1/systemone";
    }
    std::vector<Seen> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::size_t connections() const {
        std::set<unsigned short> ports;
        for (const auto& seen : requests()) {
            ports.insert(seen.port);
        }
        return ports.size();
    }

private:
    void accept_loop() {
        while (true) {
            auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
            boost::system::error_code error;
            acceptor_.accept(*socket, error);
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            if (error) {
                continue;
            }
            sockets_.push_back(socket);
            workers_.emplace_back([this, socket] { serve(*socket); });
        }
    }

    void serve(boost::asio::ip::tcp::socket& socket) {
        boost::asio::streambuf buffer;
        boost::system::error_code error;
        const unsigned short port = socket.remote_endpoint(error).port();
        while (true) {
            const std::size_t head_size =
                boost::asio::read_until(socket, buffer, "\r\n\r\n", error);
            if (error) {
                break;
            }
            const std::string head(boost::asio::buffers_begin(buffer.data()),
                                   boost::asio::buffers_begin(buffer.data()) +
                                       static_cast<std::ptrdiff_t>(head_size));
            buffer.consume(head_size);
            Seen seen;
            seen.port = port;
            std::size_t length = 0;
            std::size_t line_start = head.find("\r\n") + 2;
            while (line_start < head.size()) {
                const auto line_end = head.find("\r\n", line_start);
                const std::string line = head.substr(line_start, line_end - line_start);
                line_start = line_end + 2;
                const auto colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') {
                    value.erase(value.begin());
                }
                for (auto& c : name) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (name == "content-length") {
                    length = static_cast<std::size_t>(std::stoull(value));
                } else if (name == "authorization") {
                    seen.auth = value;
                }
            }
            if (buffer.size() < length) {
                boost::asio::read(socket, buffer,
                                  boost::asio::transfer_exactly(length - buffer.size()), error);
                if (error) {
                    break;
                }
            }
            seen.body.assign(boost::asio::buffers_begin(buffer.data()),
                             boost::asio::buffers_begin(buffer.data()) +
                                 static_cast<std::ptrdiff_t>(length));
            buffer.consume(length);
            int nth = 0;
            Behavior behavior;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(seen);
                for (const auto& earlier : requests_) {
                    nth += earlier.port == port ? 1 : 0;
                }
                behavior = behavior_;
            }
            Connection connection(socket, *this);
            behavior(connection, nth);
            if (connection.closed()) {
                break;
            }
        }
        boost::system::error_code ignored;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    unsigned short port_ = 0;
    std::thread accept_thread_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    Behavior behavior_;
    std::list<std::shared_ptr<boost::asio::ip::tcp::socket>> sockets_;
    std::list<std::thread> workers_;
    std::vector<Seen> requests_;
};

// test_network_reuse.py's post(): one bare judgment payload through the keep-alive POST.
std::expected<llm::detail::PostReply, http::Error>
post(JudgeEnvironment& env, const std::string& url, double timeout_s = 5.0) {
    const Json payload = Json{{"model", kTypeSafeModel},
                              {"state", Json{{"text", "ok"}}},
                              {"questions", Json{{"q", Json{{"type", "noul"}}}}}};
    return llm::detail::http_post(env.pool(), *env.http(), env.proxy_lookup(), payload, timeout_s,
                                  "k", url, {});
}

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

Json route_fields(const JudgeRoute& route) {
    Json headers = Json::object();
    for (const auto& [name, value] : route.headers) {
        headers[name] = value;
    }
    Json profile = nullptr;
    if (route.profile) {
        profile = Json{{"id", route.profile->id}, {"is_local", route.profile->is_local()}};
    }
    return Json{{"id", route.id},
                {"name", route.name},
                {"endpoint", route.endpoint},
                {"model", route.model},
                {"credential_env", route.credential_env},
                {"headers", headers},
                {"timeout_s", route.timeout_s},
                {"profile", profile},
                {"is_local", route.is_local()},
                {"host", route.host()},
                {"key_label", route.key_label()},
                {"key_phrase", route.key_phrase()}};
}

Json rebranded(const Json& value) {
    if (value.is_string()) {
        return golden::rebrand(value.get<std::string>());
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = rebranded(item);
        }
        return out;
    }
    return value;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Golden vectors

TEST_CASE("services.typesafe validate_answers", "[golden][test_typesafe_service]") {
    golden::for_each("services.typesafe", "validate_answers", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        Json actual;
        try {
            actual = Json{{"ok", validate_answers(c.arg<Json>(0), c.arg<Json>(1))}};
        } catch (const TypeSafeError& error) {
            actual = Json{{"error", error.what()}};
        }
        INFO(c.label());
        CHECK(golden::equal(expected, actual));
    });
}

TEST_CASE("services.typesafe _status_outcome", "[golden][test_typesafe_service]") {
    golden::for_each("services.typesafe", "_status_outcome", [](const golden::Case& c) {
        const std::string route_id = c.arg<std::string>(2);
        const JudgeRoute& route = route_id == "openrouter" ? openrouter_route() : typesafe_route();
        const auto outcome = status_outcome(c.arg<int>(0), c.arg<std::string>(1), route);
        const Json actual = outcome ? Json::array({outcome->ok, outcome->message}) : Json();
        INFO(c.label());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("services.typesafe route_from_settings", "[golden][test_typesafe_service]") {
    golden::for_each("services.typesafe", "route_from_settings", [](const golden::Case& c) {
        INFO(c.label());
        CHECK(golden::equal(rebranded(c.result<Json>()),
                            route_fields(route_from_settings(c.arg<Json>(0)))));
    });
}

TEST_CASE("services.typesafe JudgeRoute copy", "[golden][test_typesafe_service]") {
    golden::for_each("services.typesafe", "route_copy", [](const golden::Case& c) {
        JudgeRoute route;
        route.id = "x";
        route.name = c.arg<std::string>(0);
        route.endpoint = c.arg<std::string>(1);
        route.model = "m";
        const Json expected = c.result<Json>();
        INFO(c.label());
        if (expected.contains("error")) {
            // urlsplit refuses the URL: Python raises from the property; here the endpoint
            // itself names the place, as for any other URL without a host.
            CHECK(route.host() == route.endpoint);
            return;
        }
        const Json actual = Json{{"host", route.host()},
                                 {"key_label", route.key_label()},
                                 {"key_phrase", route.key_phrase()}};
        CHECK(golden::equal(expected, actual));
    });
}

TEST_CASE("services.typesafe verify_key", "[golden][test_typesafe_service]") {
    Fixture fixture;
    golden::for_each("services.typesafe", "verify_key", [&](const golden::Case& c) {
        const std::string route_id = c.arg<std::string>(0);
        const JudgeRoute& route = route_id == "openrouter" ? openrouter_route() : typesafe_route();
        auto recorder = recording(c.arg<int>(1), Json(c.arg<std::string>(2)));
        if (c.arg_count() > 3) {
            recorder->raises = c.arg<std::string>(3);
        }
        VerifyOptions options;
        options.transport = transport_of(recorder);
        auto result = verify_key(route, "k", *fixture.env, options);
        const auto at = result.message.find("answered in ");
        if (result.ok && at != std::string::npos && result.message.ends_with(" s.")) {
            result.message = result.message.substr(0, at) + "answered in <t> s.";
        }
        INFO(c.label());
        CHECK(golden::equal(c.result<Json>(), Json::array({result.ok, result.message})));
    });
}

// ---------------------------------------------------------------------------------------------
// TestValidateAnswers

TEST_CASE("accepts each primitive", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestValidateAnswers::test_accepts_each_primitive
    CHECK(validate_answers(Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.25}}}}}},
                           kNoulQ)["q"]["noul"] == 0.25);
    const Json choice = validate_answers(
        Json{{"answers", Json{{"q", Json{{"type", "choice"},
                                         {"choice", "b"},
                                         {"confidence", 0.7},
                                         {"probabilities", Json{{"a", 0.3}, {"b", 0.7}}}}}}}},
        kChoiceQ)["q"];
    CHECK(choice["choice"] == "b");
    CHECK(choice["probabilities"]["b"] == 0.7);
    const Json score = validate_answers(
        Json{
            {"answers", Json{{"q", Json{{"type", "score"}, {"score", 1.4}, {"confidence", 0.5}}}}}},
        kScoreQ)["q"];
    CHECK(score["score"] == 1.4);
}

TEST_CASE("rejects malformed", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestValidateAnswers::test_rejects_malformed
    const std::vector<Json> bodies{
        Json{{"answers", Json{{"other", Json{{"type", "noul"}, {"noul", 0.5}}}}}},
        Json{{"answers", Json{{"q", Json{{"type", "choice"}, {"choice", "a"}}}}}},
        Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 1.5}}}}}},
        Json{{"no_answers", true}},
    };
    for (const auto& body : bodies) {
        INFO(py::dumps(body));
        CHECK_THROWS_AS(validate_answers(body, kNoulQ), TypeSafeError);
    }
}

TEST_CASE("rejects unknown choice and bad score", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestValidateAnswers::test_rejects_unknown_choice_and_bad_score
    CHECK_THROWS_AS(
        validate_answers(
            Json{{"answers",
                  Json{{"q", Json{{"type", "choice"}, {"choice", "zzz"}, {"confidence", 0.9}}}}}},
            kChoiceQ),
        TypeSafeError);
    CHECK_THROWS_AS(
        validate_answers(Json{{"answers", Json{{"q", Json{{"type", "score"}, {"score", 7}}}}}},
                         kScoreQ),
        TypeSafeError);
}

// ---------------------------------------------------------------------------------------------
// TestJudge

TEST_CASE("requires key", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestJudge::test_requires_key
    Fixture fixture;
    CHECK_THROWS_AS(TypeSafeJudge("", *fixture.env), std::invalid_argument);
    try {
        TypeSafeJudge judge("", *fixture.env);
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "TypeSafe API key is required");
    }
}

TEST_CASE("ask returns validated answers and records usage", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestJudge::test_ask_returns_validated_answers_and_records_usage
    Fixture fixture;
    auto transport =
        recording(200, Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.9}}}}},
                            {"usage", Json{{"input_tokens", 321}}}});
    TypeSafeJudge judge("k", *fixture.env,
                        {.timeout_s = 2.5, .transport = transport_of(transport)});
    CHECK(judge.noul(Json{{"text", "hi"}}, "Is it?") == 0.9);
    const auto [payload, timeout] = transport->seen().at(0);
    CHECK(payload["model"] == std::string(kTypeSafeModel));
    CHECK(payload["state"] == Json{{"text", "hi"}});
    CHECK(timeout == 2.5);
    CHECK(judge.usage().requests == 1);
    CHECK(judge.usage().failures == 0);
    CHECK(judge.usage().input_tokens == 321);
}

TEST_CASE("choice convenience", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestJudge::test_choice_convenience
    Fixture fixture;
    auto transport = recording(
        200, Json{{"answers", Json{{"q", Json{{"type", "choice"},
                                              {"choice", "a"},
                                              {"confidence", 0.8},
                                              {"probabilities", Json{{"a", 0.9}, {"b", 0.1}}}}}}}});
    TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport)});
    const auto answer = judge.choice("state", "Which?", Json{{"a", "A"}, {"b", "B"}});
    REQUIRE(answer.has_value());
    CHECK(*answer == ChoiceAnswer{"a", 0.8, Json{{"a", 0.9}, {"b", 0.1}}});
}

TEST_CASE("failures return none and count", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestJudge::test_failures_return_none_and_count
    const std::vector<std::shared_ptr<Recording>> transports{
        recording(429, Json("")),
        recording(200, Json("not json")),
        recording(200, Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 3}}}}}}),
        raising("TimeoutError"),
    };
    for (const auto& transport : transports) {
        Fixture fixture;
        TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport)});
        CHECK_FALSE(judge.noul("x", "Is it?").has_value());
        CHECK(judge.usage().failures == 1);
    }
}

TEST_CASE("oversized state is refused before transport", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestJudge::test_oversized_state_is_refused_before_transport
    Fixture fixture;
    auto transport =
        recording(200, Json{{"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.1}}}}}});
    TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport)});
    CHECK_FALSE(judge.noul(std::string(kMaxStateChars + 1, 'x'), "Is it?").has_value());
    CHECK(transport->seen().empty());
}

TEST_CASE("empty questions short circuit", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestJudge::test_empty_questions_short_circuit
    Fixture fixture;
    auto transport = recording(200, Json{{"answers", Json::object()}});
    TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport)});
    CHECK_FALSE(judge.ask("x", Json::object(), std::nullopt, {}).has_value());
    CHECK(transport->seen().empty());
}

TEST_CASE("warning rate limited", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestJudge::test_warning_rate_limited
    Fixture fixture;
    FakeClock clock;
    auto transport = recording(500, Json(""));
    TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport), .clock = &clock});
    LevelScope logs;
    judge.noul("x", "a");
    clock.advance(1s);
    judge.noul("x", "a");
    CHECK(logs.count(log::Level::Warning, "services.typesafe") == 1);
    CHECK(logs.count(log::Level::Debug, "services.typesafe") == 1);
}

TEST_CASE("a state JSON cannot hold is skipped", "[test_typesafe_service]") {
    // typesafe.py: a non-JSON-serializable state is skipped without a request (A12-71).
    Fixture fixture;
    auto transport = recording(200, kGood);
    TypeSafeJudge judge("k", *fixture.env, {.transport = transport_of(transport)});
    LevelScope logs;
    CHECK_FALSE(judge.noul(Json::binary({1, 2, 3}), "Is it?").has_value());
    CHECK(transport->seen().empty());
    CHECK(logs.count(log::Level::Warning, "services.typesafe") == 1);
}

// ---------------------------------------------------------------------------------------------
// TestSettingsGating and TestKeyPresence

TEST_CASE("disabled setting yields no judge", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestSettingsGating::test_disabled_setting_yields_no_judge
    Fixture fixture;
    fixture.keys.env.set("TYPESAFE_API_KEY", "key");
    CHECK(judge_from_settings(Json{{"typesafe_enabled", false}}, *fixture.env) == nullptr);
    CHECK_FALSE(is_configured(Json{{"typesafe_enabled", false}}, fixture.keys.store,
                              fixture.keys.env, fixture.keys.dotenv));
}

TEST_CASE("enabled without key yields no judge", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestSettingsGating::test_enabled_without_key_yields_no_judge
    Fixture fixture;
    CHECK(judge_from_settings(Json{{"typesafe_enabled", true}}, *fixture.env) == nullptr);
    CHECK_FALSE(is_configured(Json{{"typesafe_enabled", true}}, fixture.keys.store,
                              fixture.keys.env, fixture.keys.dotenv));
}

TEST_CASE("enabled with key builds judge", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestSettingsGating::test_enabled_with_key_builds_judge
    Fixture fixture;
    fixture.keys.env.set(std::string(kTypeSafeCredentialEnv), "key");
    const auto built =
        judge_from_settings(Json{{"typesafe_enabled", true}}, *fixture.env, {.timeout_s = 1.0});
    const auto judge = std::dynamic_pointer_cast<TypeSafeJudge>(built);
    REQUIRE(judge != nullptr);
    CHECK(judge->timeout_s() == 1.0);
    CHECK(judge->route() == typesafe_route());
}

TEST_CASE("reports whether a key resolves", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestKeyPresence::test_reports_whether_a_key_resolves
    Fixture fixture;
    auto& keys = fixture.keys;
    CHECK_FALSE(key_present(Json::object(), keys.store, keys.env, keys.dotenv));
    keys.env.set("TYPESAFE_API_KEY", "key");
    CHECK(key_present(Json::object(), keys.store, keys.env, keys.dotenv));
}

TEST_CASE("is configured still needs the master switch", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestKeyPresence::test_is_configured_still_needs_the_master_switch
    Fixture fixture;
    auto& keys = fixture.keys;
    keys.env.set("TYPESAFE_API_KEY", "key");
    CHECK(key_present(Json::object(), keys.store, keys.env, keys.dotenv));
    CHECK_FALSE(
        is_configured(Json{{"typesafe_enabled", false}}, keys.store, keys.env, keys.dotenv));
}

// ---------------------------------------------------------------------------------------------
// TestVerifyKey

TEST_CASE("accepts a working key", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestVerifyKey::test_accepts_a_working_key
    Fixture fixture;
    auto transport = recording(200, kGoodVerify);
    const auto result =
        verify_key(typesafe_route(), "k", *fixture.env, {.transport = transport_of(transport)});
    CHECK(result.ok);
    CHECK(result.message.find(kTypeSafeModel) != std::string::npos);
    const Json payload = transport->seen().at(0).first;
    CHECK(payload["model"] == std::string(kTypeSafeModel));
    CHECK(payload["state"] == Json{{"text", "ok"}});
    CHECK(payload["questions"].size() == 1);
    CHECK(payload["questions"].contains("q"));
}

TEST_CASE("blank key is not sent", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestVerifyKey::test_blank_key_is_not_sent
    Fixture fixture;
    auto transport = recording(200, kGoodVerify);
    const auto result =
        verify_key(typesafe_route(), "   ", *fixture.env, {.transport = transport_of(transport)});
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Paste a key first.");
    CHECK(transport->seen().empty());
}

TEST_CASE("reports the status class", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestVerifyKey::test_reports_the_status_class
    const std::vector<std::pair<int, std::string>> cases{
        {401, "rejected the key"}, {403, "denied access"}, {500, "HTTP 500"}};
    for (const auto& [status, fragment] : cases) {
        Fixture fixture;
        const auto result =
            verify_key(typesafe_route(), "k", *fixture.env,
                       {.transport = transport_of(recording(status, Json::object()))});
        CHECK_FALSE(result.ok);
        CHECK(result.message.find(fragment) != std::string::npos);
    }
}

TEST_CASE("rate limiting still proves the key", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestVerifyKey::test_rate_limiting_still_proves_the_key
    Fixture fixture;
    const auto result = verify_key(typesafe_route(), "k", *fixture.env,
                                   {.transport = transport_of(recording(429, Json::object()))});
    CHECK(result.ok);
    CHECK(result.message.find("429") != std::string::npos);
}

TEST_CASE("rejects a 200 that does not answer", "[test_typesafe_service]") {
    // from tests/test_typesafe_service.py::TestVerifyKey::test_rejects_a_200_that_does_not_answer
    Fixture fixture;
    auto transport =
        recording(200, Json{{"answers", Json{{"other", Json{{"type", "noul"}, {"noul", 1.0}}}}}});
    const auto result =
        verify_key(typesafe_route(), "k", *fixture.env, {.transport = transport_of(transport)});
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("answered oddly") != std::string::npos);
}

TEST_CASE("network failure names the host not the key", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyKey::test_network_failure_names_the_host_not_the_key
    Fixture fixture;
    const auto result = verify_key(typesafe_route(), "secret-key", *fixture.env,
                                   {.transport = transport_of(raising("OSError"))});
    CHECK_FALSE(result.ok);
    CHECK(result.message.find(verify_host()) != std::string::npos);
    CHECK(result.message.find("secret-key") == std::string::npos);
}

TEST_CASE("uses a longer timeout than a live judgment", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyKey::test_uses_a_longer_timeout_than_a_live_judgment
    Fixture fixture;
    auto transport = recording(200, kGoodVerify);
    verify_key(typesafe_route(), "k", *fixture.env, {.transport = transport_of(transport)});
    CHECK(transport->seen().at(0).second == kVerifyTimeoutS);
    STATIC_CHECK(kVerifyTimeoutS > kJudgeDefaultTimeoutS);
}

// ---------------------------------------------------------------------------------------------
// TestOpenRouterRoute

TEST_CASE("route reuses the openrouter text profile", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestOpenRouterRoute::test_route_reuses_the_openrouter_text_profile
    const auto profile = *builtin_profile(kOpenRouterProfileId);
    const JudgeRoute& route = openrouter_route();
    CHECK(route.endpoint == py::rstrip(*profile.base_url, "/") + "/systemone");
    CHECK(route.credential_env == profile.api_key_env);
    CHECK(route.credential_env == "OPENROUTER_API_KEY");
    CHECK(route.headers == http::Headers{{"X-Title", product()}});
    CHECK(route.host() == "openrouter.ai");
}

TEST_CASE("model is openrouters id for the pinned release", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestOpenRouterRoute::test_model_is_openrouters_id_for_the_pinned_release
    CHECK(kTypeSafeModel == "jev-1.13.0");
    CHECK(openrouter_route().model == "typesafe/jev-1.13");
}

TEST_CASE("judge posts to openrouter with its key model and header", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestOpenRouterRoute::test_judge_posts_to_openrouter_with_its_key_model_and_header
    FakeConnections connections;
    connections.client->push(json_reply(
        200,
        Json{{"id", "gen-dec-1"},
             {"model", "typesafe/jev-1.13-20260917"},
             {"provider", "TypeSafe"},
             {"answers", Json{{"q", Json{{"type", "noul"}, {"noul", 0.98}}}}},
             {"usage", Json{{"input_tokens", 275}, {"output_tokens", 20}, {"cost", 0.00003}}}}));
    Fixture fixture(connections.hooks());
    TypeSafeJudge judge("or-key", *fixture.env, {.route = openrouter_route()});
    CHECK(judge.noul("I was charged twice.", "Is a refund requested?") == 0.98);
    const auto sent = connections.client->requests().at(0);
    CHECK(sent.url == openrouter_route().endpoint);
    CHECK(request_body(sent)["model"] == "typesafe/jev-1.13");
    CHECK(http::find_header(sent.headers, "Authorization") == "Bearer or-key");
    CHECK(http::find_header(sent.headers, "X-Title") == product());
    CHECK(judge.usage().input_tokens == 275);
}

TEST_CASE("extra headers never replace authorization", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestOpenRouterRoute::test_extra_headers_never_replace_authorization
    JudgeHooks hooks;
    hooks.proxy_for = [](std::string_view) {
        return std::optional<std::string>("http://proxy.invalid:3128");
    };
    Fixture fixture(std::move(hooks));
    fixture.http->push(json_reply(200, kGood));
    const auto reply = llm::detail::http_post(
        fixture.env->pool(), *fixture.env->http(), fixture.env->proxy_lookup(),
        Json{{"model", "m"}}, 1.0, "real", "https://example.test/x",
        {{"Authorization", "Bearer spoofed"}, {"X-Title", product()}});
    REQUIRE(reply.has_value());
    const auto sent = fixture.http->requests().at(0);
    CHECK(http::find_header(sent.headers, "Authorization") == "Bearer real");
    CHECK(http::find_header(sent.headers, "X-Title") == product());
}

// ---------------------------------------------------------------------------------------------
// TestRouteFromSettings

TEST_CASE("defaults to typesafe and ignores unknown values", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestRouteFromSettings::test_defaults_to_typesafe_and_ignores_unknown_values
    CHECK(route_from_settings(Json::object()) == typesafe_route());
    CHECK(route_from_settings(Json{{"typesafe_provider", "bogus"}}) == typesafe_route());
    CHECK(route_from_settings(Json{{"typesafe_provider", "openrouter"}}) == openrouter_route());
}

TEST_CASE("judge uses the chosen routes credential", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestRouteFromSettings::test_judge_uses_the_chosen_routes_credential
    Fixture fixture;
    fixture.keys.env.set("OPENROUTER_API_KEY", "or-key");
    const Json settings{{"typesafe_enabled", true}, {"typesafe_provider", "openrouter"}};
    const auto judge =
        std::dynamic_pointer_cast<TypeSafeJudge>(judge_from_settings(settings, *fixture.env));
    REQUIRE(judge != nullptr);
    CHECK(judge->route() == openrouter_route());
    CHECK(judge->endpoint() == openrouter_route().endpoint);
    // The TypeSafe route has no key here, so nothing would run there.
    CHECK(judge_from_settings(Json{{"typesafe_enabled", true}}, *fixture.env) == nullptr);
}

TEST_CASE("key presence follows the route", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestRouteFromSettings::test_key_presence_follows_the_route
    Fixture fixture;
    auto& keys = fixture.keys;
    keys.env.set("OPENROUTER_API_KEY", "k");
    CHECK(
        key_present(Json{{"typesafe_provider", "openrouter"}}, keys.store, keys.env, keys.dotenv));
    CHECK_FALSE(
        key_present(Json{{"typesafe_provider", "typesafe"}}, keys.store, keys.env, keys.dotenv));
    CHECK(key_present(openrouter_route(), keys.sources));
    CHECK(is_configured(Json{{"typesafe_enabled", true}, {"typesafe_provider", "openrouter"}},
                        keys.store, keys.env, keys.dotenv));
}

TEST_CASE("same connection compares destination and key", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestRouteFromSettings::test_same_connection_compares_destination_and_key
    Fixture fixture;
    const TypeSafeJudge direct("k", *fixture.env);
    CHECK(direct.same_connection(TypeSafeJudge("k", *fixture.env)));
    CHECK_FALSE(
        direct.same_connection(TypeSafeJudge("k", *fixture.env, {.route = openrouter_route()})));
    CHECK_FALSE(direct.same_connection(TypeSafeJudge("other", *fixture.env)));
    CHECK_FALSE(direct.same_connection(FakeJudge()));
}

// ---------------------------------------------------------------------------------------------
// TestFollowingJudge

TEST_CASE("each question goes to the judge provided at that moment", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestFollowingJudge::test_each_question_goes_to_the_judge_provided_at_that_moment
    Fixture fixture;
    auto first_calls = recording(200, kGoodVerify);
    auto second_calls = recording(200, kGoodVerify);
    auto first = std::make_shared<TypeSafeJudge>(
        "k", *fixture.env, TypeSafeJudgeOptions{.transport = transport_of(first_calls)});
    auto second = std::make_shared<TypeSafeJudge>(
        "k", *fixture.env,
        TypeSafeJudgeOptions{.route = openrouter_route(), .transport = transport_of(second_calls)});
    std::shared_ptr<Judge> current = first;
    FollowingJudge judge([&current] { return current; });
    CHECK(judge.noul("x", "Is it?") == 1.0);
    current = second;
    CHECK(judge.noul("x", "Is it?") == 1.0);
    CHECK(judge.model() == openrouter_route().model);
    current = nullptr;
    CHECK_FALSE(judge.noul("x", "Is it?").has_value());
    CHECK(judge.model().empty());
    CHECK(first_calls->seen().size() == 1);
    CHECK(second_calls->seen().size() == 1);
    CHECK(second_calls->seen().at(0).first["model"] == openrouter_route().model);
}

TEST_CASE("timeout is passed only when given and by keyword", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestFollowingJudge::test_timeout_is_passed_only_when_given_and_by_keyword
    auto duck = std::make_shared<FakeJudge>();
    duck->set_handler([](const Json&, const Json&) { return Json::object(); });
    FollowingJudge judge([duck] { return duck; });
    judge.ask("x", kNoulQ, std::nullopt, {});
    judge.ask("x", kNoulQ, 2.0, {});
    const auto calls = duck->calls();
    REQUIRE(calls.size() == 2);
    CHECK_FALSE(calls[0].timeout_s.has_value());
    CHECK(calls[1].timeout_s == 2.0);
}

// ---------------------------------------------------------------------------------------------
// TestVerifyOverOpenRouter

TEST_CASE("reports the openrouter host and model", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyOverOpenRouter::test_reports_the_openrouter_host_and_model
    Fixture fixture;
    auto transport = recording(200, kGoodVerify);
    const auto result =
        verify_key(openrouter_route(), "k", *fixture.env, {.transport = transport_of(transport)});
    CHECK(result.ok);
    CHECK(result.message.find("openrouter.ai") != std::string::npos);
    CHECK(result.message.find("typesafe/jev-1.13") != std::string::npos);
    CHECK(transport->seen().at(0).first["model"] == "typesafe/jev-1.13");
}

TEST_CASE("sends the route header and endpoint", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyOverOpenRouter::test_sends_the_route_header_and_endpoint
    FakeConnections connections;
    connections.client->push(json_reply(200, kGoodVerify));
    Fixture fixture(connections.hooks());
    const auto result = verify_key(openrouter_route(), "k", *fixture.env);
    CHECK(result.ok);
    const auto sent = connections.client->requests().at(0);
    CHECK(sent.url == openrouter_route().endpoint);
    CHECK(http::find_header(sent.headers, "X-Title") == product());
}

TEST_CASE("out of credits is not reported as a bad key", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyOverOpenRouter::test_out_of_credits_is_not_reported_as_a_bad_key
    Fixture fixture;
    const auto result = verify_key(openrouter_route(), "k", *fixture.env,
                                   {.transport = transport_of(recording(402, Json::object()))});
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("out of credits") != std::string::npos);
    CHECK(result.message.find("rejected") == std::string::npos);
}

TEST_CASE("no route points at the account settings", "[test_typesafe_service]") {
    // from
    // tests/test_typesafe_service.py::TestVerifyOverOpenRouter::test_no_route_points_at_the_account_settings
    Fixture fixture;
    auto result = verify_key(openrouter_route(), "k", *fixture.env,
                             {.transport = transport_of(recording(404, Json::object()))});
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("privacy settings") != std::string::npos);
    // TypeSafe's own 404 keeps the plain status wording.
    result = verify_key(typesafe_route(), "k", *fixture.env,
                        {.transport = transport_of(recording(404, Json::object()))});
    CHECK(result.message == "api.typesafe.ai answered HTTP 404.");
}

// ---------------------------------------------------------------------------------------------
// test_network_reuse.py: TestTypeSafeKeepAlive

TEST_CASE("sequential judgments share one connection", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_sequential_judgments_share_one_connection
    KeepAliveServer server;
    Fixture fixture;
    TypeSafeJudge judge("secret-key", *fixture.env, {.endpoint = server.url()});
    for (int i = 0; i < 5; ++i) {
        CHECK(judge.noul(Json{{"text", "hi"}}, "Is it?") == 0.9);
    }
    const auto requests = server.requests();
    REQUIRE(requests.size() == 5);
    CHECK(server.connections() == 1);
    CHECK(requests[0].auth == "Bearer secret-key");
    CHECK(py::loads(requests[0].body)["model"] == std::string(kTypeSafeModel));
}

TEST_CASE("concurrent judges never share a socket", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_concurrent_judges_never_share_a_socket
    KeepAliveServer server;
    Fixture fixture;
    TypeSafeJudge judge("k", *fixture.env, {.endpoint = server.url()});
    std::mutex mutex;
    std::vector<std::optional<double>> results;
    std::atomic<int> waiting{4};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            --waiting;
            while (waiting.load() > 0) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 5; ++i) {
                const auto answer = judge.noul(Json{{"text", "hi"}}, "Is it?");
                std::lock_guard lock(mutex);
                results.push_back(answer);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // Interleaved writes on one socket would corrupt answers.
    REQUIRE(results.size() == 20);
    for (const auto& result : results) {
        CHECK(result == 0.9);
    }
    CHECK(judge.usage().failures == 0);
    CHECK(server.connections() <= 4);
}

TEST_CASE("http error keeps only the status and the connection", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_http_error_keeps_only_the_status_and_the_connection
    KeepAliveServer server;
    Fixture fixture;
    server.set_behavior(
        [](KeepAliveServer::Connection& c, int) { c.answer(503, "secret-sentinel"); });
    const auto failed = post(*fixture.env, server.url());
    REQUIRE(failed.has_value());
    CHECK(failed->status == 503);
    CHECK(failed->text.empty());
    server.set_behavior([](KeepAliveServer::Connection& c, int) { c.answer(); });
    const auto good = post(*fixture.env, server.url());
    REQUIRE(good.has_value());
    CHECK(good->status == 200);
    CHECK(py::loads(good->text) == kGood);
    CHECK(server.connections() == 1);
}

TEST_CASE("connection the server closed while idle is replaced", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_connection_the_server_closed_while_idle_is_replaced
    KeepAliveServer server;
    Fixture fixture;
    // Answers, then closes without saying so in a header.
    server.set_behavior(
        [](KeepAliveServer::Connection& c, int) { c.answer(200, py::dumps(kGood), true); });
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    std::this_thread::sleep_for(200ms); // let the FIN arrive
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    CHECK(server.connections() == 2);
    // Caught before sending, so nothing needed a second attempt.
    CHECK(server.requests().size() == 2);
    CHECK(fixture.env->pool().discards() == 0);
}

TEST_CASE("request dropped on a reused connection is sent once more",
          "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_request_dropped_on_a_reused_connection_is_sent_once_more
    KeepAliveServer server;
    Fixture fixture;
    server.set_behavior([](KeepAliveServer::Connection& c, int nth) {
        if (nth == 2) {
            c.drop();
        } else {
            c.answer();
        }
    });
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    const auto reply = post(*fixture.env, server.url());
    REQUIRE(reply.has_value());
    CHECK(reply->status == 200);
    CHECK(py::loads(reply->text) == kGood);
    CHECK(server.requests().size() == 3); // two on the first connection, one on the next
    CHECK(server.connections() == 2);
}

TEST_CASE("failure on a fresh connection is not retried", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_failure_on_a_fresh_connection_is_not_retried
    KeepAliveServer server;
    Fixture fixture;
    server.set_behavior([](KeepAliveServer::Connection& c, int) { c.drop(); });
    TypeSafeJudge judge("k", *fixture.env, {.endpoint = server.url()});
    CHECK_FALSE(judge.noul("x", "Is it?").has_value());
    CHECK(judge.usage().failures == 1);
    CHECK(server.requests().size() == 1);
}

TEST_CASE("connection idle past the window is not reused", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_connection_idle_past_the_window_is_not_reused
    KeepAliveServer server;
    FakeClock clock;
    JudgeHooks hooks;
    hooks.pool_clock = &clock;
    hooks.pool_idle_s = 10.0;
    Fixture fixture(std::move(hooks));
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    clock.advance(9s);
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    CHECK(server.connections() == 1);
    clock.advance(11s);
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    CHECK(server.connections() == 2);
}

TEST_CASE("timeout never outlasts the requests budget", "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_timeout_never_outlasts_the_requests_budget
    KeepAliveServer server;
    Fixture fixture;
    REQUIRE(post(*fixture.env, server.url(), 5.0)->status == 200);
    server.set_behavior([](KeepAliveServer::Connection& c, int) {
        if (c.sleep(1s)) {
            c.answer();
        }
    });
    const auto started = std::chrono::steady_clock::now();
    const auto reply = post(*fixture.env, server.url(), 0.2);
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == http::ErrorKind::Timeout);
    CHECK(llm::detail::python_exception_name(reply.error()) == "TimeoutError");
    CHECK(seconds_since(started) < 0.9);
    // The reused socket used the whole budget, so nothing was left for a fresh attempt, and
    // the silent socket was not handed back.
    CHECK(server.requests().size() == 2);
    CHECK(server.connections() == 1);
    CHECK(fixture.env->pool().idle() == 0);
}

TEST_CASE("silent reused connection is retried fresh within the budget",
          "[test_network_reuse][platform]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_silent_reused_connection_is_retried_fresh_within_the_budget
    // A NAT that forgot an idle connection: the request goes out and nothing ever comes back.
    KeepAliveServer server;
    Fixture fixture;
    REQUIRE(post(*fixture.env, server.url(), 5.0)->status == 200);
    const unsigned short first_port = server.requests().at(0).port;
    server.set_behavior([first_port](KeepAliveServer::Connection& c, int) {
        if (c.port() == first_port && !c.sleep(2s)) {
            return;
        }
        c.answer();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto reply = post(*fixture.env, server.url(), 2.2);
    const double elapsed = seconds_since(started);
    REQUIRE(reply.has_value());
    CHECK(reply->status == 200);
    CHECK(py::loads(reply->text) == kGood);
    // Gave the reused socket half the budget (1.1 s), then a fresh one.
    CHECK(elapsed > 1.0);
    CHECK(elapsed < 2.0);
    CHECK(server.connections() == 2);
}

TEST_CASE("the pool reuses one connection to the TypeSafe endpoint and gives a reused socket "
          "half the budget, at least 1 s",
          "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestTypeSafeKeepAlive::test_https_connections_use_the_shared_verified_context
    // There is no shared Python ssl context to pin: the curl connection verifies against the
    // system store. What the test also pins, connection reuse and the reused budget, is here.
    FakeConnections connections;
    connections.client->set_handler([](const http::Request&) { return json_reply(200, kGood); });
    Fixture fixture(connections.hooks());
    REQUIRE(post(*fixture.env, std::string(kTypeSafeEndpoint), 3.0)->status == 200);
    REQUIRE(post(*fixture.env, std::string(kTypeSafeEndpoint), 2.0)->status == 200);
    CHECK(*connections.made == 1);
    const auto sent = connections.client->requests();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].method == "POST");
    CHECK(sent[0].url == "https://api.typesafe.ai/v1/systemone");
    CHECK(sent[0].idle_timeout == 3000ms);
    // A reused socket may stay silent for half the 2 s budget, at least 1 s.
    CHECK(sent[1].idle_timeout == 1000ms);
    CHECK(sent[1].bypass_proxy);
}

// ---------------------------------------------------------------------------------------------
// test_network_reuse.py: TestProxyFallback

TEST_CASE("proxied endpoint goes through urllib", "[test_network_reuse]") {
    // from
    // tests/test_network_reuse.py::TestProxyFallback::test_proxied_endpoint_goes_through_urllib
    FakeConnections connections;
    JudgeHooks hooks = connections.hooks();
    hooks.proxy_for = [](std::string_view) {
        return std::optional<std::string>("http://proxy.invalid:3128");
    };
    Fixture fixture(std::move(hooks));
    fixture.http->push(json_reply(200, kGood));
    const auto reply = post(*fixture.env, std::string(kTypeSafeEndpoint), 3.0);
    REQUIRE(reply.has_value());
    CHECK(reply->status == 200);
    CHECK(reply->text == py::dumps(kGood));
    const auto sent = fixture.http->requests().at(0);
    CHECK(sent.idle_timeout == 3000ms);
    CHECK_FALSE(sent.bypass_proxy);
    CHECK(http::find_header(sent.headers, "Connection") == "close");
    CHECK(*connections.made == 0);
    fixture.http->push(text_reply(401, "secret-sentinel"));
    const auto refused = post(*fixture.env, std::string(kTypeSafeEndpoint));
    REQUIRE(refused.has_value());
    CHECK(refused->status == 401);
    CHECK(refused->text.empty());
}

TEST_CASE("proxy detection", "[test_network_reuse]") {
    // from tests/test_network_reuse.py::TestProxyFallback::test_proxy_detection
    const std::string url(kTypeSafeEndpoint);
    CHECK_FALSE(
        llm::detail::proxied(url, [](std::string_view) { return std::optional<std::string>(); }));
    http::ProxySettings settings;
    settings.environment = {{"https", "http://proxy.invalid:3128"}, {"no", "api.typesafe.ai"}};
    const auto lookup = [&settings](std::string_view target) {
        return http::proxy_for_url(target, settings);
    };
    CHECK_FALSE(llm::detail::proxied(url, lookup));
    settings.environment.erase("no");
    CHECK(llm::detail::proxied(url, lookup));
    // When in doubt, let the one-shot request apply its own proxy rules.
    CHECK(llm::detail::proxied(url, [](std::string_view) -> std::optional<std::string> {
        throw std::runtime_error("registry unreadable");
    }));
}

// ---------------------------------------------------------------------------------------------
// Acceptance and the provider

TEST_CASE("the default endpoints are the pinned TypeSafe and OpenRouter URLs", "[typesafe]") {
    // Asserted without contacting them.
    CHECK(typesafe_route().endpoint == "https://api.typesafe.ai/v1/systemone");
    CHECK(openrouter_route().endpoint == "https://openrouter.ai/api/v1/systemone");
    CHECK(typesafe_route().credential_env == "TYPESAFE_API_KEY");
    CHECK(typesafe_route().timeout_s == 4.0);
    CHECK(verify_host() == "api.typesafe.ai");
    Fixture fixture;
    CHECK(TypeSafeJudge("k", *fixture.env).endpoint() == "https://api.typesafe.ai/v1/systemone");
    CHECK(jev_routes().size() == 2);
}

TEST_CASE("a judge asked with a 1.5 s budget against a server that answers in 3 s gives up "
          "within 1.6 s",
          "[typesafe][platform]") {
    KeepAliveServer server;
    server.set_behavior([](KeepAliveServer::Connection& c, int) {
        if (c.sleep(3s)) {
            c.answer();
        }
    });
    Fixture fixture;
    TypeSafeJudge judge("k", *fixture.env, {.endpoint = server.url()});
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(judge.noul("x", "Is it?", Json(), 1.5).has_value());
    const double elapsed = seconds_since(started);
    CHECK(elapsed >= 1.4);
    CHECK(elapsed < 1.6);
    CHECK(judge.usage().failures == 1);
}

TEST_CASE("a TypeSafe budget ends the wait for a blocked transport") {
    Fixture fixture;
    std::promise<void> release;
    auto gate = release.get_future().share();
    TypeSafeJudge judge("k", *fixture.env, {.transport = [gate](const Json&, double) {
                            gate.wait();
                            return TransportReply{
                                200, R"({"answers":{"q":{"type":"noul","noul":0.1}}})"};
                        }});
    auto result =
        std::async(std::launch::async, [&] { return judge.noul("x", "Is it?", Json(), 0.05); });
    const auto before_transport_unblocked = result.wait_for(500ms);
    release.set_value();

    CHECK(before_transport_unblocked == std::future_status::ready);
    CHECK_FALSE(result.get().has_value());
    CHECK(judge.usage().failures == 1);
}

TEST_CASE("a cancelled token ends the wait within 50 ms", "[typesafe][platform]") {
    KeepAliveServer server;
    server.set_behavior([](KeepAliveServer::Connection& c, int) {
        if (c.sleep(3s)) {
            c.answer();
        }
    });
    Fixture fixture;
    TypeSafeJudge judge("k", *fixture.env, {.endpoint = server.url()});
    CancelSource cancel;
    std::chrono::steady_clock::time_point cancelled_at;
    std::thread canceller([&] {
        std::this_thread::sleep_for(300ms);
        cancelled_at = std::chrono::steady_clock::now();
        cancel.cancel();
    });
    const auto answer = judge.noul("x", "Is it?", Json(), std::nullopt, cancel.token());
    const auto returned = std::chrono::steady_clock::now();
    canceller.join();
    CHECK_FALSE(answer.has_value());
    CHECK(returned - cancelled_at < 50ms);
    // An already cancelled token sends nothing.
    const auto before = server.requests().size();
    CHECK_FALSE(judge.noul("x", "Is it?", Json(), std::nullopt, cancel.token()).has_value());
    CHECK(server.requests().size() == before);
}

TEST_CASE("the judge provider follows the settings it is given", "[typesafe]") {
    Fixture fixture;
    const auto provider = make_judge_provider(*fixture.env);
    const Json settings{{"typesafe_enabled", true}, {"typesafe_provider", "openrouter"}};
    CHECK(provider->route(settings) == openrouter_route());
    CHECK_FALSE(provider->key_present(settings));
    CHECK(provider->judge(settings) == nullptr);
    fixture.keys.env.set("OPENROUTER_API_KEY", "or-key");
    CHECK(provider->key_present(settings));
    const auto judge = provider->judge(settings);
    REQUIRE(judge != nullptr);
    CHECK(judge->model() == "typesafe/jev-1.13");
    CHECK_FALSE(judge->is_local());
    CHECK(provider->judge(Json{{"typesafe_provider", "openrouter"}}) == nullptr);
}

TEST_CASE("the fake judge answers its script and records the questions", "[typesafe]") {
    FakeJudge judge;
    judge.push(FakeJudge::noul_answer(0.4));
    judge.push(std::nullopt);
    judge.push(FakeJudge::choice_answer("b", 0.6), 2s);
    CHECK(judge.noul("x", "Is it?") == 0.4);
    CHECK_FALSE(judge.noul("x", "Is it?").has_value());
    CancelSource cancel;
    cancel.cancel();
    CHECK_FALSE(judge.choice("x", "Which?", Json{{"a", "A"}, {"b", "B"}}, cancel.token()));
    CHECK(judge.calls().size() == 3);
    CHECK(judge.calls()[0].questions["q"]["type"] == "noul");

    FakeJudgeProvider provider;
    auto shared = std::make_shared<FakeJudge>();
    provider.set_judge(shared);
    CHECK(provider.judge(Json::object()) == shared);
    CHECK(provider.route(Json::object()) == typesafe_route());
    CHECK(provider.asked().size() == 2);
}

TEST_CASE("a judgment dropped on a reused connection and again on the fresh one goes out twice",
          "[test_network_reuse][platform][sweep]") {
    // Sweep B21: curl resends by itself on a fresh connection when a reused one died before
    // answering; the pool must not add a third send (_http_post sends at most twice).
    KeepAliveServer server;
    Fixture fixture;
    // `nth` counts per connection; this counts over all of them.
    auto seen = std::make_shared<std::atomic<int>>(0);
    server.set_behavior([seen](KeepAliveServer::Connection& c, int) {
        if (++*seen >= 2) {
            c.drop();
        } else {
            c.answer();
        }
    });
    REQUIRE(post(*fixture.env, server.url())->status == 200);
    CHECK_FALSE(post(*fixture.env, server.url()).has_value());
    CHECK(server.requests().size() == 3);
}
