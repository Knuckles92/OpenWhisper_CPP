// fv_fake_agent: a stand-in for the coding agents' command-line programs in the [platform] tests
// of tests/agents. A test copies it into a temporary folder as claude.exe, codex.exe or
// opencode.exe; it reads its own file name and answers as that agent:
//
//   --version                       the agent's version line
//   --help                          (claude) FV_FAKE_AGENT_HELP, else a help listing every flag an
//                                   ask uses (--tools, --no-session-persistence, ...)
//   exec --help, features list      (codex) FV_FAKE_AGENT_EXEC_HELP, FV_FAKE_AGENT_FEATURES, else
//                                   a current listing
//   auth status, login status       signed in, or signed out in mode "signed-out"
//   -p ...                          (claude) a headless ask: reads stdin to its end
//   exec --json ...                 (codex) a headless ask: reads stdin to its end
//   acp                             (opencode) a JSON-RPC 2.0 server on stdio
//   --fake-sleep S                  sleeps S seconds (the helper "hang" starts)
//
// FV_FAKE_AGENT_MODE picks the behaviour of an ask:
//   claude: ok (a result object; FV_FAKE_AGENT_REPLY replaces it verbatim), signed-out, empty,
//           long (more than the 4 MiB of stdout an ask keeps, in small writes; see claude_long)
//   codex:  ok (an error, then the answer and turn.completed), fail (turn.failed)
//   both:   crash (a stderr line, exit 2), hang (starts a helper, writes its process id to
//           FV_FAKE_AGENT_HELPER_PID, sleeps 60 s)
//   acp:    ok, auth (the prompt fails with an auth error), permission (asks for a permission
//           before answering), hang (never answers the prompt)
// A headless ask writes {"argv", "prompt", "cwd", "files"} to FV_FAKE_AGENT_RECORD, where files
// holds the text of the files --mcp-config, --settings, --system-prompt-file and
// model_instructions_file name. The ACP server appends one JSON line per message received to
// FV_FAKE_AGENT_ACP_LOG, after a first line with the OpenCode configuration variables.
//
// Passes (tests/fakes/fake_agent_cli.py and fake_acp_agent.py of OpenWhisper): the agent calls the
// app's loopback MCP server back over HTTP (initialize, notifications/initialized, tools/list,
// then the tools/call FV_FAKE_AGENT_TOOL_CALL holds as {"name", "arguments"}, if set) and prints
// what that CLI prints:
//   -p --output-format stream-json ...   (claude) a pass: the init event, the tool call, an
//                                        assistant tool_use and a result with usage. Modes: ok,
//                                        signed-out (a signed-out result), linger (starts a helper
//                                        and keeps running after its result), hang (the init
//                                        event, then as the ask's hang)
//   exec --json ... -c mcp_servers.<s>.url=...   (codex) a pass: thread and turn events, the
//                                        tool call, the reply and turn.completed with usage.
//                                        --image takes several values, as Codex's parser does: a
//                                        "-" it takes as a picture leaves no prompt, an error.
//                                        The record gets "images", each picture's size.
//                                        Modes: ok, fail, hang
//   [--disable F ...] mcp list --json    (codex) FV_FAKE_AGENT_MCP_LIST, else []
//   acp, with an overlay naming an MCP server   (opencode) a pass server: sessions ses_1, ses_2,
//                                        ... with model, effort and mode options; the first
//                                        session connects to the tools; a prompt makes the tool
//                                        call with the session id in _meta and answers "Done.".
//                                        FV_FAKE_AGENT_NO_IMAGES says it takes no pictures.
//
// Text is UTF-8 on every platform.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view CLAUDE_HELP =
    "--tools <tools...> --no-session-persistence --disable-slash-commands --model "
    "--strict-mcp-config --settings --setting-sources --system-prompt-file --effort";
constexpr std::string_view CODEX_EXEC_HELP =
    "--json --ephemeral --ignore-user-config --skip-git-repo-check";
constexpr std::string_view CODEX_FEATURES = "shell_tool    stable   true\n"
                                            "apps          stable   false\n"
                                            "code_mode_host stable  true\n"
                                            "js_repl       removed  false\n"
                                            "fast_mode     stable   true\n";

#ifdef _WIN32
std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::filesystem::path path_of(std::string_view utf8) {
    return std::filesystem::path(widen(utf8));
}

std::string path_text(const std::filesystem::path& path) {
    return narrow(path.native());
}

std::optional<std::string> env(const char* name) {
    const auto wide = widen(name);
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD got = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(got);
    return narrow(value);
}

bool write_to(DWORD which, std::string_view bytes) {
    HANDLE handle = GetStdHandle(which);
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

bool write_out(std::string_view bytes) {
    return write_to(STD_OUTPUT_HANDLE, bytes);
}

bool write_err(std::string_view bytes) {
    return write_to(STD_ERROR_HANDLE, bytes);
}

// Some bytes of stdin; empty at its end.
std::string read_some() {
    char buffer[65536];
    DWORD got = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &got, nullptr)) {
        return {};
    }
    return std::string(buffer, got);
}

std::string self_path() {
    wchar_t self[32768];
    const DWORD size = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return narrow(std::wstring_view(self, size));
}

// Starts this program with --fake-sleep, in this one's job; its process id, or 0.
unsigned long start_helper() {
    std::wstring line = L"\"" + widen(self_path()) + L"\" --fake-sleep 60";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info)) {
        return 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
}
#else
std::filesystem::path path_of(std::string_view utf8) {
    return std::filesystem::path(std::string(utf8));
}

std::string path_text(const std::filesystem::path& path) {
    return path.string();
}

std::optional<std::string> env(const char* name) {
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
}

bool write_fd(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(fd, bytes.data(), bytes.size());
        if (written <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

bool write_out(std::string_view bytes) {
    return write_fd(1, bytes);
}

bool write_err(std::string_view bytes) {
    return write_fd(2, bytes);
}

std::string read_some() {
    char buffer[65536];
    const auto got = ::read(0, buffer, sizeof(buffer));
    return got <= 0 ? std::string() : std::string(buffer, static_cast<std::size_t>(got));
}

std::string self_path() {
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).string();
}

unsigned long start_helper() {
    const auto self = self_path();
    std::string flag = "--fake-sleep";
    std::string seconds = "60";
    std::vector<char*> args{const_cast<char*>(self.c_str()), flag.data(), seconds.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, self.c_str(), nullptr, nullptr, args.data(), environ) != 0) {
        return 0;
    }
    return static_cast<unsigned long>(pid);
}
#endif

std::string read_all() {
    std::string data;
    for (auto chunk = read_some(); !chunk.empty(); chunk = read_some()) {
        data += chunk;
    }
    return data;
}

std::string mode() {
    return env("FV_FAKE_AGENT_MODE").value_or("ok");
}

void sleep_s(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::string& file, std::string_view text, bool append = false) {
    std::ofstream out(path_of(file), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    out << text;
}

void out_json(const Json& value) {
    write_out(value.dump() + "\n");
}

// The files an ask's command line names, read while they exist.
Json named_files(const std::vector<std::string>& args) {
    Json files = Json::object();
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--mcp-config" || args[i] == "--settings" ||
            args[i] == "--system-prompt-file") {
            files[args[i]] = read_file(path_of(args[i + 1]));
        } else if (args[i] == "-c" && args[i + 1].starts_with("model_instructions_file=")) {
            const auto file =
                args[i + 1].substr(std::string_view("model_instructions_file=").size());
            files["model_instructions_file"] = read_file(path_of(file));
        }
    }
    return files;
}

std::string record_ask(const std::vector<std::string>& args, const Json& extra = Json::object()) {
    const auto prompt = read_all();
    if (const auto record = env("FV_FAKE_AGENT_RECORD")) {
        Json entry{{"argv", args},
                   {"prompt", prompt},
                   {"cwd", path_text(std::filesystem::current_path())},
                   {"files", named_files(args)}};
        for (const auto& [key, value] : extra.items()) {
            entry[key] = value;
        }
        write_file(*record, entry.dump());
    }
    return prompt;
}

// ---- loopback HTTP, as urllib.request.urlopen posts ----

struct HttpReply {
    int status = 0;
    std::string body;
};

#ifdef _WIN32
using Socket = SOCKET;
using IoSize = int; // send and recv take an int length here, a size_t elsewhere
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) {
    closesocket(s);
}
bool sockets_ready() {
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
#else
using Socket = int;
using IoSize = std::size_t;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s) {
    ::close(s);
}
bool sockets_ready() {
    return true;
}
#endif

// POSTs `body` to an http://127.0.0.1:<port>/<path> URL with the bearer token and reads the
// whole answer (the connection closes after it). Throws std::runtime_error when it cannot.
HttpReply post_http(const std::string& url, const std::string& token, const std::string& body) {
    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme) || !sockets_ready()) {
        throw std::runtime_error("bad url " + url);
    }
    const auto rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    const auto authority = rest.substr(0, slash);
    const auto path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    const auto colon = authority.rfind(':');
    const auto host = authority.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(authority.substr(colon + 1)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("bad host " + host);
    }
    const Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        throw std::runtime_error("no socket");
    }
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(s);
        throw std::runtime_error("connection refused");
    }
    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + authority +
                          "\r\nContent-Type: application/json\r\nAuthorization: Bearer " + token +
                          "\r\nContent-Length: " + std::to_string(body.size()) +
                          "\r\nConnection: close\r\n\r\n" + body;
    std::string_view pending(request);
    while (!pending.empty()) {
        const auto sent = ::send(s, pending.data(), static_cast<IoSize>(pending.size()), 0);
        if (sent <= 0) {
            close_socket(s);
            throw std::runtime_error("send failed");
        }
        pending.remove_prefix(static_cast<std::size_t>(sent));
    }
    std::string answer;
    char buffer[65536];
    for (;;) {
        const auto got = ::recv(s, buffer, static_cast<IoSize>(sizeof(buffer)), 0);
        if (got <= 0) {
            break;
        }
        answer.append(buffer, static_cast<std::size_t>(got));
    }
    close_socket(s);
    const auto head_end = answer.find("\r\n\r\n");
    const auto space = answer.find(' ');
    if (head_end == std::string::npos || space == std::string::npos) {
        throw std::runtime_error("no answer");
    }
    return {std::stoi(answer.substr(space + 1, 3)), answer.substr(head_end + 4)};
}

// post(): the reply's JSON, null when the body is empty; an HTTP error status throws, as
// urlopen's HTTPError does.
Json post_json(const std::string& url, const std::string& token, const Json& message) {
    const auto reply = post_http(url, token, message.dump());
    if (reply.status >= 400) {
        throw std::runtime_error("HTTP Error " + std::to_string(reply.status));
    }
    return reply.body.empty() ? Json(nullptr) : Json::parse(reply.body);
}

// call_tool(): the MCP handshake, then FV_FAKE_AGENT_TOOL_CALL's call with `meta` as its _meta
// when that is an object; the call's reply, or null without a call.
Json call_tool(const std::string& url, const std::string& token, const Json& meta = nullptr) {
    post_json(url, token,
              {{"jsonrpc", "2.0"},
               {"id", 1},
               {"method", "initialize"},
               {"params", {{"protocolVersion", "2025-06-18"}}}});
    post_json(url, token, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    post_json(url, token, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    Json call = Json::parse(env("FV_FAKE_AGENT_TOOL_CALL").value_or("null"));
    if (!call.is_object()) {
        return nullptr;
    }
    if (meta.is_object()) {
        call["_meta"] = meta;
    }
    return post_json(url, token,
                     {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", call}});
}

int hang() {
    const auto helper = start_helper();
    if (const auto file = env("FV_FAKE_AGENT_HELPER_PID")) {
        write_file(*file, std::to_string(helper));
    }
    sleep_s(60);
    return 0;
}

int crash() {
    write_err("error: unknown option '--frobnicate'\n");
    return 2;
}

// Mode "long": stdout up to and past the 4 MiB an ask keeps, in 4 KiB writes. A line that is not
// JSON fills all but 10 bytes of the limit; the result line after it (a 1 MiB answer of "a") is
// still kept; a later result line ("late") is past the limit and must be dropped.
int claude_long() {
    constexpr std::size_t limit = 4 * 1024 * 1024;
    static constexpr std::size_t piece = 4096;
    const auto write_in_pieces = [](std::string_view text) {
        while (!text.empty()) {
            const auto size = std::min(piece, text.size());
            if (!write_out(text.substr(0, size))) {
                return false;
            }
            text.remove_prefix(size);
        }
        return true;
    };
    const Json answer{{"type", "result"},
                      {"subtype", "success"},
                      {"is_error", false},
                      {"result", std::string(1024 * 1024, 'a')}};
    const Json late{
        {"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", "late"}};
    const bool written = write_in_pieces(std::string(limit - 11, 'x') + "\n") &&
                         write_in_pieces(answer.dump() + "\n") &&
                         write_in_pieces(late.dump() + "\n");
    return written ? 0 : 1;
}

int claude_ask(const std::vector<std::string>& args) {
    const auto prompt = record_ask(args);
    const auto how = mode();
    if (how == "ok") {
        if (const auto reply = env("FV_FAKE_AGENT_REPLY")) {
            write_out(*reply + "\n");
            return 0;
        }
        out_json({{"type", "result"},
                  {"subtype", "success"},
                  {"is_error", false},
                  {"result", "Answer: " + prompt}});
        return 0;
    }
    if (how == "signed-out") {
        out_json({{"type", "result"},
                  {"subtype", "success"},
                  {"is_error", true},
                  {"result", "Not logged in \xC2\xB7 Please run /login"}});
        return 1;
    }
    if (how == "empty") {
        out_json(
            {{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", "  "}});
        return 0;
    }
    if (how == "long") {
        return claude_long();
    }
    if (how == "crash") {
        return crash();
    }
    if (how == "hang") {
        return hang();
    }
    return 3;
}

int codex_ask(const std::vector<std::string>& args) {
    const auto prompt = record_ask(args);
    const auto how = mode();
    if (how == "ok") {
        out_json({{"type", "thread.started"}, {"thread_id", "t1"}});
        out_json({{"type", "turn.started"}});
        out_json({{"type", "error"}, {"message", "Reconnecting... 1/5"}});
        out_json({{"type", "item.completed"},
                  {"item", {{"type", "agent_message"}, {"text", "Answer: " + prompt}}}});
        out_json(
            {{"type", "turn.completed"}, {"usage", {{"input_tokens", 9}, {"output_tokens", 3}}}});
        return 0;
    }
    if (how == "fail") {
        out_json({{"type", "thread.started"}, {"thread_id", "t1"}});
        const Json inner{{"error", {{"message", "The 'x' model is not supported."}}}};
        out_json({{"type", "turn.failed"}, {"error", {{"message", inner.dump()}}}});
        return 1;
    }
    if (how == "crash") {
        return crash();
    }
    if (how == "hang") {
        return hang();
    }
    return 3;
}

// ---- passes ----

// The MCP server a --mcp-config file names: (name, url, token).
struct McpServer {
    std::string name;
    std::string url;
    std::string token;
};

std::optional<McpServer> claude_server(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "--mcp-config") {
            continue;
        }
        const auto config = Json::parse(read_file(path_of(args[i + 1])), nullptr, false);
        if (!config.is_object() || !config.contains("mcpServers")) {
            return std::nullopt;
        }
        for (const auto& [name, server] : config.at("mcpServers").items()) {
            const auto auth = server.at("headers").at("Authorization").get<std::string>();
            return McpServer{name, server.at("url").get<std::string>(),
                             auth.substr(auth.find(' ') + 1)};
        }
    }
    return std::nullopt;
}

Json claude_init(const std::string& server) {
    return {{"type", "system"},
            {"subtype", "init"},
            {"tools", Json::array()},
            {"mcp_servers", Json::array({{{"name", server}, {"status", "connected"}}})}};
}

int claude_pass(const std::vector<std::string>& args) {
    record_ask(args);
    const auto how = mode();
    const auto server = claude_server(args);
    const auto name = server ? server->name : std::string("none");
    if (how == "crash") {
        return crash();
    }
    if (how == "hang") {
        out_json(claude_init(name));
        return hang();
    }
    if (!server) {
        write_err("fake agent: no --mcp-config\n");
        return 2;
    }
    out_json(claude_init(name));
    if (how == "signed-out") {
        out_json({{"type", "result"},
                  {"subtype", "success"},
                  {"is_error", true},
                  {"result", "Not logged in \xC2\xB7 Please run /login"}});
        return 0;
    }
    const auto reply = call_tool(server->url, server->token);
    out_json({{"type", "assistant"},
              {"message",
               {{"content", Json::array({{{"type", "tool_use"},
                                          {"name", "mcp__" + name + "__patch_state"},
                                          {"input", Json::object()}}})}}}});
    const auto text = env("FV_FAKE_AGENT_RESULT").value_or("Done. " + reply.dump().substr(0, 80));
    out_json({{"type", "result"},
              {"subtype", "success"},
              {"is_error", false},
              {"result", text},
              {"num_turns", 2},
              {"total_cost_usd", 0.001},
              {"usage",
               {{"input_tokens", 100}, {"cache_read_input_tokens", 50}, {"output_tokens", 20}}}});
    if (how == "linger") {
        // A helper holding stdout open after the result.
        return hang();
    }
    return 0;
}

// Codex's -c overrides, name=value split at the first "=".
std::vector<std::pair<std::string, std::string>>
codex_overrides(const std::vector<std::string>& args) {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "-c") {
            const auto& value = args[i + 1];
            const auto equals = value.find('=');
            out.emplace_back(value.substr(0, equals),
                             equals == std::string::npos ? "" : value.substr(equals + 1));
        }
    }
    return out;
}

// The server a pass's overrides point at: mcp_servers.<name>.url.
std::optional<std::string> codex_pass_server(const std::vector<std::string>& args) {
    for (const auto& [key, value] : codex_overrides(args)) {
        if (key.starts_with("mcp_servers.") && key.ends_with(".url")) {
            return key.substr(12, key.size() - 12 - 4);
        }
    }
    return std::nullopt;
}

int codex_pass(const std::vector<std::string>& args, const std::string& server) {
    // --image <FILE>... takes values until the next option; a lone "-" is a value too.
    Json images = Json::array();
    Json sizes = Json::array();
    bool prompt_on_stdin = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--image") {
            while (i + 1 < args.size() && (args[i + 1] == "-" || !args[i + 1].starts_with("-"))) {
                const auto& image = args[++i];
                images.push_back(image);
                std::error_code error;
                const auto size = std::filesystem::file_size(path_of(image), error);
                sizes.push_back(error ? Json(-1) : Json(size));
            }
        } else if (args[i] == "-" && i + 1 == args.size()) {
            prompt_on_stdin = true;
        }
    }
    record_ask(args, {{"images", images}, {"image_sizes", sizes}});
    if (!prompt_on_stdin) {
        write_err("error: no prompt (a picture took the \"-\")\n");
        return 2;
    }
    const auto how = mode();
    if (how == "crash") {
        return crash();
    }
    out_json({{"type", "thread.started"}, {"thread_id", "t1"}});
    if (how == "hang") {
        return hang();
    }
    if (how == "fail") {
        const Json inner{{"error", {{"message", "The 'x' model is not supported."}}}};
        out_json({{"type", "turn.failed"}, {"error", {{"message", inner.dump()}}}});
        return 1;
    }
    std::string url;
    std::string token_variable;
    for (const auto& [key, value] : codex_overrides(args)) {
        if (key == "mcp_servers." + server + ".url") {
            url = value;
        } else if (key == "mcp_servers." + server + ".bearer_token_env_var") {
            token_variable = value;
        }
    }
    const auto token = env(token_variable.c_str()).value_or("");
    out_json({{"type", "turn.started"}});
    call_tool(url, token);
    out_json(
        {{"type", "item.completed"},
         {"item", {{"type", "mcp_tool_call"}, {"tool", "patch_state"}, {"status", "completed"}}}});
    out_json(
        {{"type", "item.completed"},
         {"item",
          {{"type", "agent_message"}, {"text", env("FV_FAKE_AGENT_RESULT").value_or("Done.")}}}});
    out_json(
        {{"type", "turn.completed"},
         {"usage", {{"input_tokens", 300}, {"cached_input_tokens", 200}, {"output_tokens", 30}}}});
    return 0;
}

// ---- ACP ----

void acp_log(const Json& entry) {
    if (const auto file = env("FV_FAKE_AGENT_ACP_LOG")) {
        write_file(*file, entry.dump() + "\n", true);
    }
}

void send(const Json& message) {
    write_out(message.dump() + "\n");
}

void acp_answer(const Json& id, const std::string& text) {
    for (const auto& chunk : {std::string("Answer: "), text}) {
        send({{"jsonrpc", "2.0"},
              {"method", "session/update"},
              {"params",
               {{"sessionId", "s1"},
                {"update",
                 {{"sessionUpdate", "agent_message_chunk"},
                  {"content", {{"type", "text"}, {"text", chunk}}}}}}}});
    }
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"stopReason", "end_turn"}}}});
}

Json env_or_null(const char* name) {
    const auto value = env(name);
    return value ? Json(*value) : Json(nullptr);
}

// One line of stdin, without its line ending; nullopt at the end.
std::optional<std::string> read_line(std::string& buffer) {
    for (;;) {
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            auto line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        const auto chunk = read_some();
        if (chunk.empty()) {
            return std::nullopt;
        }
        buffer += chunk;
    }
}

// fake_acp_agent.py: the pass server for an overlay that names the app's MCP server.
int acp_pass(const Json& overlay) {
    const auto& servers = overlay.at("mcp").at("servers");
    const auto name = servers.begin().key();
    const auto& server = servers.begin().value();
    const auto url = server.at("url").get<std::string>();
    const auto auth = server.at("headers").at("Authorization").get<std::string>();
    const auto token = auth.substr(auth.find(' ') + 1);
    const Json models = Json::array({{{"value", "prov/fast"}, {"name", "prov/Fast"}},
                                     {{"value", "prov/slow"}, {"name", "prov/Slow"}}});
    std::map<std::string, std::map<std::string, std::string>> sessions;
    bool connected = false;
    int next = 1;
    const auto options = [&](const std::string& id) {
        auto& current = sessions[id];
        return Json::array(
            {{{"id", "model"},
              {"type", "select"},
              {"currentValue", current["model"]},
              {"options", models}},
             {{"id", "effort"},
              {"type", "select"},
              {"currentValue", "default"},
              {"options", Json::array({{{"value", "default"}}, {{"value", "low"}}})}},
             {{"id", "mode"},
              {"type", "select"},
              {"currentValue", current["mode"]},
              {"options", Json::array({{{"value", "build"}}, {{"value", "plan"}}})}}});
    };
    const auto update = [](const std::string& id, Json body) {
        send({{"jsonrpc", "2.0"},
              {"method", "session/update"},
              {"params", {{"sessionId", id}, {"update", std::move(body)}}}});
    };
    std::string buffer;
    while (const auto line = read_line(buffer)) {
        const auto message = Json::parse(*line, nullptr, false);
        if (!message.is_object() || !message.contains("method")) {
            continue;
        }
        const auto method = message.at("method").get<std::string>();
        Json params = message.value("params", Json::object());
        if (!params.is_object()) {
            params = Json::object();
        }
        acp_log({{"method", method}, {"params", params}});
        const bool has_id = message.contains("id");
        const Json id = has_id ? message.at("id") : Json(nullptr);
        Json result;
        if (method == "initialize") {
            result = {{"protocolVersion", 1},
                      {"agentInfo", {{"name", "Fake"}, {"version", "2.0.0"}}},
                      {"agentCapabilities",
                       {{"sessionCapabilities", {{"delete", Json::object()}}},
                        {"promptCapabilities", {{"image", !env("FV_FAKE_AGENT_NO_IMAGES")}}}}}};
        } else if (method == "session/new") {
            const auto session = "ses_" + std::to_string(next++);
            sessions[session] = {{"model", "prov/slow"}, {"mode", "build"}, {"effort", "default"}};
            if (!connected) {
                post_json(url, token,
                          {{"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {"params", {{"protocolVersion", "2025-06-18"}}}});
                post_json(url, token, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
                connected = true;
                update(session, {{"sessionUpdate", "available_commands_update"},
                                 {"availableCommands", Json::array()}});
            }
            result = {{"sessionId", session}, {"configOptions", options(session)}};
        } else if (method == "session/set_config_option") {
            const auto session = params.at("sessionId").get<std::string>();
            sessions[session][params.at("configId").get<std::string>()] =
                params.at("value").get<std::string>();
            result = {{"configOptions", options(session)}};
        } else if (method == "session/prompt") {
            const auto session = params.at("sessionId").get<std::string>();
            Json call = Json::parse(env("FV_FAKE_AGENT_TOOL_CALL").value_or("null"));
            if (call.is_object()) {
                call["_meta"] = {{"ai.opencode/sessionID", session}};
                update(session, {{"sessionUpdate", "tool_call"},
                                 {"title", name + "_" + call.at("name").get<std::string>()}});
                post_json(
                    url, token,
                    {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", call}});
            }
            update(session, {{"sessionUpdate", "agent_message_chunk"},
                             {"content", {{"type", "text"}, {"text", "Done."}}}});
            result = {{"stopReason", "end_turn"},
                      {"usage", {{"inputTokens", 40}, {"outputTokens", 5}, {"totalTokens", 45}}}};
        } else if (method == "session/delete") {
            sessions.erase(params.value("sessionId", std::string()));
            result = Json::object();
        } else {
            if (has_id) {
                send({{"jsonrpc", "2.0"},
                      {"id", id},
                      {"error", {{"code", -32601}, {"message", "unknown '" + method + "'"}}}});
            }
            continue;
        }
        if (has_id) {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
    }
    return 0;
}

int acp() {
    const auto how = mode();
    Json overlay = nullptr;
    if (const auto content = env("OPENCODE_CONFIG_CONTENT")) {
        overlay = Json::parse(*content, nullptr, false);
    }
    acp_log({{"overlay", overlay},
             {"project_config_off", env_or_null("OPENCODE_DISABLE_PROJECT_CONFIG")},
             {"autoupdate_off", env_or_null("OPENCODE_DISABLE_AUTOUPDATE")},
             {"filewatcher_off", env_or_null("OPENCODE_DISABLE_FILEWATCHER")}});
    if (overlay.is_object() && overlay.contains("mcp")) {
        return acp_pass(overlay);
    }
    std::string model = "prov/fast";
    std::optional<std::pair<Json, std::string>> waiting;
    std::string buffer;
    for (;;) {
        const auto newline = buffer.find('\n');
        if (newline == std::string::npos) {
            const auto chunk = read_some();
            if (chunk.empty()) {
                return 0;
            }
            buffer += chunk;
            continue;
        }
        auto line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto message = Json::parse(line, nullptr, false);
        if (!message.is_object()) {
            continue;
        }
        const Json method = message.contains("method") ? message.at("method") : Json(nullptr);
        Json params = message.contains("params") ? message.at("params") : Json::object();
        if (!params.is_object() || params.empty()) {
            params = Json::object();
        }
        const Json id = message.contains("id") ? message.at("id") : Json(nullptr);
        acp_log({{"method", method},
                 {"params", params},
                 {"result", message.contains("result") ? message.at("result") : Json(nullptr)}});
        if (method.is_null()) {
            if (waiting && id == 900) {
                acp_answer(waiting->first, waiting->second);
                waiting.reset();
            }
            continue;
        }
        const auto name = method.get<std::string>();
        if (name == "initialize") {
            send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"protocolVersion", 1},
                    {"agentCapabilities",
                     {{"sessionCapabilities", {{"delete", Json::object()}}}}}}}});
        } else if (name == "session/new") {
            send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"sessionId", "s1"},
                    {"configOptions",
                     Json::array({{{"id", "model"},
                                   {"type", "select"},
                                   {"currentValue", model},
                                   {"options", Json::array({{{"value", "prov/fast"}},
                                                            {{"value", "prov/slow"}}})}}})}}}});
        } else if (name == "session/set_config_option") {
            model = params.value("value", model);
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"configOptions", Json::array()}}}});
        } else if (name == "session/prompt") {
            const auto text = params["prompt"][0]["text"].get<std::string>();
            if (how == "hang") {
                continue;
            }
            if (how == "auth") {
                send({{"jsonrpc", "2.0"},
                      {"id", id},
                      {"error", {{"code", -32000}, {"message", "Provider auth required"}}}});
            } else if (how == "permission") {
                waiting = std::make_pair(id, text);
                send({{"jsonrpc", "2.0"},
                      {"id", 900},
                      {"method", "session/request_permission"},
                      {"params",
                       {{"sessionId", "s1"},
                        {"toolCall", {{"title", "bash"}}},
                        {"options",
                         Json::array({{{"optionId", "yes"}, {"kind", "allow_once"}},
                                      {{"optionId", "no"}, {"kind", "reject_once"}}})}}}});
            } else {
                acp_answer(id, text);
            }
        } else if (name == "session/delete") {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}});
        } else if (!id.is_null()) {
            send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"error", {{"code", -32601}, {"message", "nope"}}}});
        }
    }
}

// ---- commands ----

int run(const std::vector<std::string>& argv) {
    if (argv.size() >= 3 && argv[1] == "--fake-sleep") {
        sleep_s(std::stod(argv[2]));
        return 0;
    }
    // The file this process runs from, not argv[0], which the starter may have written another
    // way.
    auto agent = path_text(path_of(self_path()).stem());
    for (auto& c : agent) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const std::vector<std::string> args(argv.begin() + (argv.empty() ? 0 : 1), argv.end());
    const auto first = args.empty() ? std::string() : args[0];
    const auto second = args.size() > 1 ? args[1] : std::string();
    const bool signed_out = mode() == "signed-out";

    if (first == "--version") {
        if (agent == "claude") {
            write_out("2.1.296 (Claude Code)\n");
        } else if (agent == "codex") {
            write_out("codex-cli 0.162.1\n");
        } else {
            write_out("2.0.18\n");
        }
        return 0;
    }
    if (agent == "claude") {
        if (first == "--help") {
            write_out(env("FV_FAKE_AGENT_HELP").value_or(std::string(CLAUDE_HELP)) + "\n");
            return 0;
        }
        if (first == "auth" && second == "status") {
            write_out(signed_out ? R"({"loggedIn": false})"
                                 : R"({"loggedIn": true, "authMethod": "claude.ai", )"
                                   R"("subscriptionType": "pro"})");
            return signed_out ? 1 : 0;
        }
        if (first == "-p") {
            if (std::find(args.begin(), args.end(), "stream-json") != args.end()) {
                return claude_pass(args);
            }
            return claude_ask(args);
        }
    } else if (agent == "codex") {
        if (first == "exec" && second == "--help") {
            write_out(env("FV_FAKE_AGENT_EXEC_HELP").value_or(std::string(CODEX_EXEC_HELP)) + "\n");
            return 0;
        }
        if (first == "features" && second == "list") {
            write_out(env("FV_FAKE_AGENT_FEATURES").value_or(std::string(CODEX_FEATURES)));
            return 0;
        }
        if (first == "login" && second == "status") {
            write_out(signed_out ? "Not logged in\n" : "Logged in using ChatGPT\n");
            return signed_out ? 1 : 0;
        }
        if (first == "exec" && second == "--json") {
            if (const auto server = codex_pass_server(args)) {
                return codex_pass(args, *server);
            }
            return codex_ask(args);
        }
        if (args.size() >= 3 && args[args.size() - 3] == "mcp" && args[args.size() - 2] == "list" &&
            args.back() == "--json") {
            write_out(env("FV_FAKE_AGENT_MCP_LIST").value_or("[]") + "\n");
            return 0;
        }
    } else if (agent == "opencode") {
        if (first == "acp") {
            return acp();
        }
    }
    write_err("fake agent: unknown command\n");
    return 64;
}

// A failure (a refused HTTP call, a malformed file) ends the program with its message, as an
// uncaught Python exception does.
int guarded(const std::vector<std::string>& argv) {
    try {
        return run(argv);
    } catch (const std::exception& error) {
        write_err(std::string("fake agent: ") + error.what() + "\n");
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(narrow(argv[i]));
    }
    return guarded(args);
}
#else
int main(int argc, char** argv) {
    return guarded(std::vector<std::string>(argv, argv + argc));
}
#endif
