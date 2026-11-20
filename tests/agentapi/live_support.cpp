#include "live_support.h"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <thread>

#include "pyport/json.h"
#include "support.h"

namespace fv::agentapi::test {

// --- FakeSse -----------------------------------------------------------------------------------

bool FakeSse::write(std::string frame) {
    std::lock_guard lock(mutex_);
    if (gone_ || closed_) {
        return false;
    }
    if (!frame.empty()) {
        frames_.push_back(std::move(frame));
        changed_.notify_all();
    }
    return true;
}

void FakeSse::close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    changed_.notify_all();
}

std::size_t FakeSse::queued_frames() const {
    std::lock_guard lock(mutex_);
    return queued_;
}

std::vector<std::string> FakeSse::frames() const {
    std::lock_guard lock(mutex_);
    return frames_;
}

std::string FakeSse::text() const {
    std::string out;
    for (const std::string& frame : frames()) {
        out += frame;
    }
    return out;
}

bool FakeSse::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

void FakeSse::set_queued(std::size_t frames) {
    std::lock_guard lock(mutex_);
    queued_ = frames;
}

void FakeSse::set_gone(bool gone) {
    std::lock_guard lock(mutex_);
    gone_ = gone;
}

bool FakeSse::wait_for(const std::function<bool(const std::vector<std::string>&)>& predicate,
                       std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return predicate(frames_); });
}

// --- CaptureResponder --------------------------------------------------------------------------

void CaptureResponder::send(int status_code, net::Headers sent, std::string sent_body) {
    ++answers;
    status = status_code;
    headers = std::move(sent);
    body = std::move(sent_body);
}

void CaptureResponder::send_file(std::filesystem::path, std::string,
                                 std::optional<net::ByteRange>) {
    ++answers;
    FAIL("The live API never answers with a file.");
}

std::shared_ptr<net::EventStream> CaptureResponder::start_sse(net::Headers sent) {
    ++answers;
    status = 200;
    headers = std::move(sent);
    stream = std::make_shared<FakeSse>();
    return stream;
}

std::optional<std::string> Served::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

Json Served::json() const {
    return py::loads(body);
}

Json Served::error() const {
    return json().at("error");
}

Served serve(McpApp& app, const net::HttpRequest& request) {
    auto responder = std::make_shared<CaptureResponder>();
    app.serve(request, responder);
    REQUIRE(responder->answers == 1);
    Served out;
    out.status = responder->status;
    out.headers = responder->headers;
    out.stream = responder->stream;
    out.body = responder->stream ? responder->stream->text() : responder->body;
    return out;
}

net::HttpRequest
live_request(std::string method, std::string path, const std::string& token,
             std::optional<std::string> body,
             std::vector<std::pair<std::string, std::optional<std::string>>> headers) {
    std::vector<std::pair<std::string, std::optional<std::string>>> all = {
        {"host", "127.0.0.1"}, {"authorization", "Bearer " + token}};
    if (body) {
        all.emplace_back("content-type", "application/json");
        all.emplace_back("content-length", std::to_string(body->size()));
    }
    for (auto& [name, value] : headers) {
        std::string lower = name;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        bool replaced = false;
        for (auto& existing : all) {
            if (existing.first == lower) {
                existing.second = value;
                replaced = true;
            }
        }
        if (!replaced) {
            all.emplace_back(lower, value);
        }
    }
    net::HttpRequest request = fv::agentapi::test::request(std::move(path), {}, {}, method);
    request.headers.clear();
    for (auto& [name, value] : all) {
        if (value) {
            request.headers.emplace_back(name, *value);
        }
    }
    request.body = body.value_or("");
    return request;
}

// --- RouteControl ------------------------------------------------------------------------------

void RouteControl::maybe_fail(bool read) const {
    std::function<void()> fail;
    {
        std::lock_guard lock(mutex_);
        if (read && !reads_fail) {
            return;
        }
        fail = fail_;
    }
    if (fail) {
        fail();
    }
}

automation::LiveState RouteControl::state() {
    maybe_fail(true);
    std::lock_guard lock(mutex_);
    return live;
}

Json RouteControl::list_actions() {
    maybe_fail(true);
    std::lock_guard lock(mutex_);
    return actions;
}

std::string RouteControl::run_action(std::string_view action_id, std::string_view text,
                                     std::string_view source) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"run", std::string(action_id), std::string(text), std::string(source)});
    }
    maybe_fail(false);
    return run_job;
}

std::string RouteControl::start_recording(std::optional<std::string> action_id,
                                          std::string_view source) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"start", action_id.value_or(""), std::string(source)});
    }
    maybe_fail(false);
    return start_job;
}

void RouteControl::stop_recording() {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"stop"});
    }
    maybe_fail(false);
}

void RouteControl::cancel() {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"cancel"});
    }
    maybe_fail(false);
}

void RouteControl::notify(std::string_view text, std::string_view title) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({"notify", std::string(text), std::string(title)});
    }
    maybe_fail(false);
}

void RouteControl::fail_with(std::function<void()> thrower) {
    std::lock_guard lock(mutex_);
    fail_ = std::move(thrower);
}

std::vector<std::vector<std::string>> RouteControl::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

ScopedAppControl::ScopedAppControl(std::shared_ptr<automation::AppControl> control)
    : previous_(automation::app_control()) {
    automation::set_app_control(std::move(control));
}

ScopedAppControl::~ScopedAppControl() {
    automation::set_app_control(previous_);
}

// --- FakeHost ----------------------------------------------------------------------------------

void FakeHost::record(std::string name, std::vector<std::string> args) {
    std::lock_guard lock(mutex_);
    calls_.push_back({std::move(name), std::move(args), std::this_thread::get_id()});
}

automation::LiveState FakeHost::state() {
    std::lock_guard lock(mutex_);
    automation::LiveState out;
    out.recording = recording;
    out.busy = busy;
    out.meeting = meeting;
    out.version = version;
    // The job as it is now: the one being worked on once busy, else the slot's.
    const std::optional<Job>& current = busy ? active_job : job;
    if ((out.recording || out.busy) && current) {
        out.mode = current->mode;
        out.action_id = current->action_id;
        out.job_id = current->id;
    }
    return out;
}

std::optional<std::string> FakeHost::start_recording(std::optional<std::string> action_id,
                                                     std::string source) {
    if (action_id) {
        record("start_action", {*action_id, source});
        recording = true;
        std::lock_guard lock(mutex_);
        job = Job{"job-action", "action", *action_id};
        return "job-action";
    }
    record("start", {source});
    if (!refusal.empty()) {
        status_update.emit(refusal);
        return std::nullopt;
    }
    status_update.emit("Recording...");
    recording = true;
    std::lock_guard lock(mutex_);
    job = Job{"job-new", "dictation", ""};
    return "job-new";
}

bool FakeHost::stop_recording() {
    if (!recording) {
        return false;
    }
    record("stop", {});
    return true;
}

bool FakeHost::cancel() {
    const bool cancelable = (busy && !saving) || recording;
    if (!cancelable) {
        return false;
    }
    record("cancel", {});
    return true;
}

std::string FakeHost::run_action_text(std::string action_id, std::string text, std::string source) {
    record("run", {action_id, text, source});
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
    }
    if (on_run) {
        on_run();
    }
    if (action_id == "gone") {
        throw automation::ControlError("not_found", "No enabled action has that id.");
    }
    return "job-run";
}

void FakeHost::notify(std::string line) {
    record("notify", {std::move(line)});
}

std::vector<FakeHost::Call> FakeHost::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

std::vector<std::string> FakeHost::call_names() const {
    std::vector<std::string> out;
    for (const Call& call : calls()) {
        out.push_back(call.name);
    }
    return out;
}

// --- ScopedLogCapture --------------------------------------------------------------------------

class ScopedLogCapture::Sink final : public log::Sink {
public:
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex);
        text += std::string(record.logger) + " " + std::string(record.message) + " " +
                std::string(record.exception_type) + "\n";
    }
    std::mutex mutex;
    std::string text;
};

ScopedLogCapture::ScopedLogCapture() : sink_(std::make_shared<Sink>()), previous_(log::level()) {
    log::set_level(log::Level::Debug);
    log::add_sink(sink_);
}

ScopedLogCapture::~ScopedLogCapture() {
    log::remove_sink(sink_);
    log::set_level(previous_);
}

std::string ScopedLogCapture::text() const {
    std::lock_guard lock(sink_->mutex);
    return sink_->text;
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace fv::agentapi::test
