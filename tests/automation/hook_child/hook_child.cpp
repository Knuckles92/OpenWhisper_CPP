// fv_automation_hook_child: the program the program hook and program filter tests start, in place
// of OpenWhisper's `python -c ...` snippets. Its arguments are commands run in order:
//
//   read                  stdin, all of it, kept as the input
//   record FILE NAME ...  appends {"stdin": input, "env": {NAME: value or null}} as one line to
//                         FILE (ends the commands)
//   append FILE TEXT      appends TEXT and a newline to FILE, with "{n}" replaced by the input's
//                         data.n and "{type}" by its type (the input is an event's JSON)
//   upper-text            writes {"text": the input's "text" in ASCII upper case}
//   request-text          writes {"text": the input without its last newline}
//   write TEXT            TEXT to stdout as it is
//   sleep S               S seconds (a decimal)
//   exit C                exit with code C
//
// Text is UTF-8 on every platform.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <shellapi.h>
#endif

namespace {

using Json = nlohmann::ordered_json;

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
#endif

std::optional<std::string> env(const std::string& name) {
#ifdef _WIN32
    const std::wstring wide = widen(name);
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(written);
    return narrow(value);
#else
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

std::filesystem::path path_of(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(widen(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

void append_line(const std::string& file, const std::string& line) {
    std::ofstream out(path_of(file), std::ios::binary | std::ios::app);
    out << line << '\n';
    out.flush();
}

void replace_all(std::string& text, std::string_view from, const std::string& to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

std::string value_text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

void write_out(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // The arguments as UTF-8, whatever the code page.
    int wide_count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    std::vector<std::string> args;
    for (int i = 1; i < wide_count; ++i) {
        args.push_back(narrow(wide[i]));
    }
    LocalFree(wide);
    (void)argc;
    (void)argv;
#else
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::string input;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& command = args[i];
        const auto next = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (command == "read") {
            input.assign(std::istreambuf_iterator<char>(std::cin),
                         std::istreambuf_iterator<char>());
        } else if (command == "record") {
            const std::string file = next();
            Json line = Json::object();
            line["stdin"] = input;
            Json variables = Json::object();
            for (++i; i < args.size(); ++i) {
                const auto value = env(args[i]);
                variables[args[i]] = value ? Json(*value) : Json(nullptr);
            }
            line["env"] = variables;
            append_line(file, line.dump());
            return 0;
        } else if (command == "append") {
            const std::string file = next();
            std::string text = next();
            const Json event = Json::parse(input, nullptr, false);
            if (event.is_object()) {
                if (event.contains("data") && event["data"].is_object() &&
                    event["data"].contains("n")) {
                    replace_all(text, "{n}", value_text(event["data"]["n"]));
                }
                if (event.contains("type")) {
                    replace_all(text, "{type}", value_text(event["type"]));
                }
            }
            append_line(file, text);
        } else if (command == "upper-text") {
            const Json request = Json::parse(input, nullptr, false);
            std::string text =
                request.is_object() && request.contains("text") && request["text"].is_string()
                    ? request["text"].get<std::string>()
                    : std::string();
            for (char& c : text) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            write_out(Json{{"text", text}}.dump());
        } else if (command == "request-text") {
            std::string text = input;
            if (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            write_out(Json{{"text", text}}.dump());
        } else if (command == "write") {
            write_out(next());
        } else if (command == "sleep") {
            const double seconds = std::stod(next());
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        } else if (command == "exit") {
            return std::stoi(next());
        } else {
            std::fprintf(stderr, "unknown command: %s\n", command.c_str());
            return 99;
        }
    }
    return 0;
}
