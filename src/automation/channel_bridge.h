#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "automation/channel_link.h"
#include "base/env.h"
#include "base/json.h"

// fiorivoce-cli --channel: a Claude Code channel fed by the running app
// (services/automation/channel_bridge.py). Claude Code starts it as a stdio MCP server
// (https://code.claude.com/docs/en/channels-reference). It declares the claude/channel
// capability, follows the app's channel messages (channel_link.h) and turns each into a
// notifications/claude/channel line; its reply tool shows Claude's answer near the pointer.
//
// The MCP side is written by hand, one JSON object per line. It answers Claude Code's
// server/discover probe (revision 2026-07-28) with "method not found", which keeps Claude Code on
// the initialize handshake (2025-11-25 and earlier): only that one carries channel
// notifications. Standard output carries protocol messages only; logs go to standard error and
// never include dictated text, replies or the token.
namespace fv::automation {

// Handshake revisions this server speaks, newest first.
inline constexpr std::array<std::string_view, 4> PROTOCOL_VERSIONS = {"2025-11-25", "2025-06-18",
                                                                      "2025-03-26", "2024-11-05"};

inline constexpr int PARSE_ERROR = -32700;
inline constexpr int INVALID_REQUEST = -32600;
inline constexpr int METHOD_NOT_FOUND = -32601;
inline constexpr int INVALID_PARAMS = -32602;

// The server's name (the .mcp.json key, and the tag's source in Claude Code): the product in
// lower case.
std::string channel_server_name();
// What initialize tells Claude about the messages it will receive (under 2048 characters).
std::string channel_instructions();
// The reply tool's definition, as tools/list sends it.
const Json& reply_tool();
// --channel --help, for `program` (the executable's name).
std::string channel_usage(std::string_view program);

// _reply_arguments: the stripped text and the title with its whitespace collapsed. Throws
// std::invalid_argument with the message Claude reads.
std::pair<std::string, std::string> reply_arguments(const Json& arguments);
// {"content": [{"type": "text", "text": text}], "isError": error}.
Json tool_result(std::string_view text, bool error = false);

// One protocol line at a time, as Python's readline() gives them: the line with its "\n", or the
// unterminated rest at the end; nullopt at the end of the stream. Throws std::runtime_error when
// the stream fails.
// Affinity: one reader thread.
class LineReader {
public:
    virtual ~LineReader() = default;
    virtual std::optional<std::string> readline() = 0;
};

// The protocol stream's writing end. write() sends every byte or throws std::runtime_error.
// Affinity: called under MessageWriter's lock only.
class LineWriter {
public:
    virtual ~LineWriter() = default;
    virtual void write(std::string_view bytes) = 0;
    virtual void close() = 0;
};

// One JSON-RPC message per line on the protocol stream; the only writer. ASCII-only JSON: every
// parser reads it whatever the pipe's encoding, and a lone surrogate in dictated text cannot fail
// the write. A failed write closes it.
// Affinity: thread-safe.
class MessageWriter {
public:
    explicit MessageWriter(LineWriter& stream) : stream_(stream) {}
    // False once closed.
    bool send(const Json& message);
    void close();
    bool closed() const;

private:
    LineWriter& stream_;
    mutable std::mutex mutex_;
    bool closed_ = false;
};

namespace detail {
class WorkerThreads;
}

// Affinity: value type.
struct ChannelServerOptions {
    std::string version; // serverInfo's version; "" sends "0"
    // The stream started after notifications/initialized; null: an EventStream on the link that
    // delivers to send_channel_message and steps aside for an update.
    std::function<std::unique_ptr<ChannelStream>()> stream_factory;
    // How the process ends when it steps aside for an update; null: std::_Exit.
    std::function<void(int)> exit_process;
    // The default stream's update check; null: never.
    std::function<bool()> update_check;
};

// The MCP server Claude Code talks to over the protocol streams. serve() reads requests on the
// calling thread; a reply runs on a thread of its own ("channel-reply") so that requests keep
// being read while the app is asked.
// Affinity: serve() on one thread; send_channel_message and close from any thread.
class ChannelServer {
public:
    ChannelServer(LineReader& reader, LineWriter& writer, AppLink& link,
                  ChannelServerOptions options = {});
    // close(), then cancels the link's requests (AppLink::cancel_requests) and waits for the
    // replies still running, which then end at once.
    ~ChannelServer();
    ChannelServer(const ChannelServer&) = delete;
    ChannelServer& operator=(const ChannelServer&) = delete;

    // Answers requests until the input ends; returns the exit code (0).
    int serve();
    // Stops the event stream.
    void close();

    void send_channel_message(const Json& params);
    // _initialize: the result of an initialize request.
    Json initialize(const Json& params) const;
    // The stream the server starts when no stream_factory was given.
    std::unique_ptr<EventStream> default_stream();
    // Closes the protocol stream and ends the process with 0: the reading thread waits on
    // standard input, which no other thread can interrupt.
    void exit_for_update();

private:
    void handle_line(std::string_view line);
    std::optional<Json> parse_message(std::string_view line);
    void request(const Json& id, const std::string& method, const Json& params);
    void notification(const std::string& method);
    void start_stream();
    void call_tool(const Json& id, const Json& params);
    void result(const Json& id, Json result);
    void error(const Json& id, int code, const std::string& message);

    LineReader& reader_;
    MessageWriter out_;
    AppLink& link_;
    ChannelServerOptions options_;
    std::mutex stream_mutex_;
    std::unique_ptr<ChannelStream> stream_;
    std::unique_ptr<detail::WorkerThreads> replies_;
};

// The protocol streams of this process: a reader on a duplicate of standard input (polling a
// pipe, so that no read is left pending on it) and a writer on a duplicate of standard output,
// both binary. Standard output (the descriptor and the OS handle) then points at standard error,
// so that nothing else that prints can corrupt the protocol. nullopt when the process has no
// usable standard input or output (a windowed start without pipes).
struct ProtocolStreams {
    std::unique_ptr<LineReader> reader;
    std::unique_ptr<LineWriter> writer;
};
std::optional<ProtocolStreams> protocol_streams();

// _configure_logging: every record at or above <env_prefix>LOG_LEVEL (INFO by default) goes to
// standard error as "<product in lower case> channel: <message>", and nowhere else.
void configure_channel_logging(const EnvReader& env);

} // namespace fv::automation
