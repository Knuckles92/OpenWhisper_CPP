#include "remote_wire/transcript.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "base/identity.h"
#include "net/tls.h"
#include "testkit/testkit.h"

namespace fv::testing::remote_wire {

namespace {

// The hash domain of the approval pairing's commitment (protocol.py pairing_commitment).
constexpr std::string_view kCommitDomain = "openwhisper-pair-commit-v1";
constexpr std::string_view kPrefix = "remote_";

struct Shape {
    std::string_view kind;
    const char* pattern;
    bool number; // written as a JSON number, not a string
    bool bound;  // one value per placeholder across frames
};

constexpr std::array<Shape, 9> kShapes{{
    {"token", "[A-Za-z0-9_-]{43}", false, true},
    {"device_id", "[0-9a-f]{32}", false, true},
    {"nonce", "[0-9a-f]{64}", false, true},
    {"commit", "[0-9a-f]{64}", false, true},
    {"fingerprint", "[0-9A-F]{64}", false, true},
    {"host_ms", "[0-9]+\\.[0-9]", true, false},
    {"iso_time", "[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?(\\+00:00|Z)?",
     false, false},
    {"uuid", "[0-9a-f]{32}", false, true},
    {"conn_id", "[0-9a-f]{8}", false, true},
}};

const Shape* shape_of(std::string_view kind) {
    for (const auto& shape : kShapes) {
        if (shape.kind == kind) {
            return &shape;
        }
    }
    return nullptr;
}

std::mt19937_64& random_engine() {
    thread_local std::mt19937_64 engine{[] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }()};
    return engine;
}

std::string random_from(std::string_view alphabet, std::size_t length) {
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out += alphabet[pick(random_engine())];
    }
    return out;
}

constexpr std::string_view kLowerHex = "0123456789abcdef";

std::string to_hex(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        const auto value = std::to_integer<unsigned>(b);
        out += kLowerHex[value >> 4];
        out += kLowerHex[value & 0xF];
    }
    return out;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::vector<std::byte> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd-length hex");
    }
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_digit(hex[i]);
        const int low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("not hex: " + std::string(hex.substr(i, 2)));
        }
        out.push_back(static_cast<std::byte>(high * 16 + low));
    }
    return out;
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
}

std::string sha256_hex(std::span<const std::byte> bytes) {
    return lower(net::certificate_sha256_from_der(bytes));
}

// Python's bytes.decode("utf-8"): no overlong forms, no surrogates, nothing past U+10FFFF.
bool valid_utf8(std::span<const std::byte> bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto c = std::to_integer<unsigned>(bytes[i]);
        std::size_t extra = 0;
        unsigned min = 0;
        unsigned code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1, min = 0x80, code = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2, min = 0x800, code = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3, min = 0x10000, code = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = std::to_integer<unsigned>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// A placeholder in a recorded frame: the JSON string "<kind#n>", quotes included.
struct Hole {
    std::size_t begin = 0, end = 0;
    std::string name; // <kind#n>
    const Shape* shape = nullptr;
};

std::vector<Hole> holes_in(std::string_view text) {
    std::vector<Hole> out;
    std::size_t at = 0;
    while ((at = text.find("\"<", at)) != std::string_view::npos) {
        std::size_t i = at + 2;
        while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') || text[i] == '_')) {
            ++i;
        }
        const std::string_view kind = text.substr(at + 2, i - at - 2);
        const std::size_t hash = i;
        if (hash < text.size() && text[hash] == '#') {
            ++i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                ++i;
            }
            const Shape* shape = shape_of(kind);
            if (shape != nullptr && i > hash + 1 && text.substr(i, 2) == ">\"") {
                out.push_back({at, i + 2, std::string(text.substr(at + 1, i - at)), shape});
                at = i + 2;
                continue;
            }
        }
        at += 1;
    }
    return out;
}

// One JSON scalar of `text` at `at`: a string (quotes included), a number, or a literal.
std::optional<std::string_view> scalar_at(std::string_view text, std::size_t at) {
    if (at >= text.size()) {
        return std::nullopt;
    }
    std::size_t i = at;
    if (text[i] == '"') {
        for (++i; i < text.size(); ++i) {
            if (text[i] == '\\') {
                ++i;
            } else if (text[i] == '"') {
                return text.substr(at, i + 1 - at);
            }
        }
        return std::nullopt;
    }
    const auto in_scalar = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '+' ||
               c == '.' || c == 'E';
    };
    while (i < text.size() && in_scalar(text[i])) {
        ++i;
    }
    if (i == at) {
        return std::nullopt;
    }
    return text.substr(at, i - at);
}

std::string excerpt(std::string_view text, std::size_t at) {
    const std::size_t from = at > 40 ? at - 40 : 0;
    std::string out = from > 0 ? "..." : "";
    out += text.substr(from, 100);
    if (from + 100 < text.size()) {
        out += "...";
    }
    return out;
}

std::string describe(const WireFrame& frame, std::size_t index) {
    std::ostringstream out;
    out << "frame " << index << " (step \"" << frame.step << "\", conn " << frame.conn << ", "
        << frame.dir << ' ' << frame.kind << ")";
    return out.str();
}

std::string describe_actual(const WireFrame& frame) {
    if (frame.kind == "close") {
        return "close " + std::to_string(frame.code) + " \"" + frame.reason + "\"";
    }
    if (frame.kind == "http") {
        return "HTTP " + std::to_string(frame.status);
    }
    if (frame.kind == "text") {
        return "text " + excerpt(frame.text, 0);
    }
    return frame.kind + " frame";
}

// Checks every commit/nonce pair the transcript names once both are bound.
std::string check_commits(const Transcript& transcript, const std::set<std::string>& touched,
                          const Json& bindings) {
    for (const auto& [commit, nonce] : transcript.commit_of) {
        if (!touched.contains(commit) && !touched.contains(nonce)) {
            continue;
        }
        if (!bindings.contains(commit) || !bindings.contains(nonce)) {
            continue;
        }
        const std::string expected = commitment_of(bindings[nonce].get<std::string>());
        if (bindings[commit].get<std::string>() != expected) {
            return commit + " " + bindings[commit].get<std::string>() +
                   " is not the commitment of " + nonce + " " + bindings[nonce].get<std::string>();
        }
    }
    return {};
}

// `actual` against `recorded` with its placeholders as holes; binds what they matched.
std::string match_template(const Transcript& transcript, std::string_view recorded,
                           std::string_view actual, Json& bindings) {
    const auto holes = holes_in(recorded);
    Json staged = bindings;
    std::set<std::string> touched;
    std::size_t r = 0;
    std::size_t a = 0;
    const auto literal_differs = [&](std::string_view literal) -> std::optional<std::string> {
        const std::string_view have = actual.substr(std::min(a, actual.size()), literal.size());
        if (have == literal) {
            a += literal.size();
            return std::nullopt;
        }
        std::size_t k = 0;
        while (k < have.size() && have[k] == literal[k]) {
            ++k;
        }
        return "differs at byte " + std::to_string(a + k) +
               "\n  expected: " + excerpt(std::string(recorded), r + k) +
               "\n  actual:   " + excerpt(actual, a + k);
    };
    for (const auto& hole : holes) {
        if (auto diff = literal_differs(recorded.substr(r, hole.begin - r))) {
            return *diff;
        }
        const auto token = scalar_at(actual, a);
        if (!token) {
            return "expected a value for " + hole.name + " at byte " + std::to_string(a) +
                   "\n  actual:   " + excerpt(actual, a);
        }
        const bool is_string = token->front() == '"';
        const std::string_view content = is_string ? token->substr(1, token->size() - 2) : *token;
        if (is_string == hole.shape->number || !has_shape(hole.shape->kind, content)) {
            return hole.name + " should be a " + std::string(hole.shape->kind) + ", not " +
                   std::string(*token);
        }
        if (hole.shape->bound) {
            if (staged.contains(hole.name)) {
                if (staged[hole.name].get<std::string>() != content) {
                    return hole.name + " was " + staged[hole.name].get<std::string>() +
                           " in an earlier frame, but is " + std::string(content) + " here";
                }
            } else {
                staged[hole.name] = std::string(content);
            }
            touched.insert(hole.name);
        }
        a += token->size();
        r = hole.end;
    }
    if (auto diff = literal_differs(recorded.substr(r))) {
        return *diff;
    }
    if (a != actual.size()) {
        return "has " + std::to_string(actual.size() - a) + " bytes more at byte " +
               std::to_string(a) + "\n  actual:   " + excerpt(actual, a);
    }
    if (auto bad = check_commits(transcript, touched, staged); !bad.empty()) {
        return bad;
    }
    bindings = std::move(staged);
    return {};
}

bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// "an " or "An " as a word right before `at`, not before `from`.
std::optional<std::size_t> article_before(std::string_view text, std::size_t at, std::size_t from) {
    if (at < from + 3) {
        return std::nullopt;
    }
    const std::string_view word = text.substr(at - 3, 3);
    if (word != "an " && word != "An ") {
        return std::nullopt;
    }
    if (at > 3 && is_letter(text[at - 4])) {
        return std::nullopt;
    }
    return at - 3;
}

// The recorded text as a FioriVoce peer writes it: product names in copy become `product`.
std::string expected_text(const Transcript& transcript, std::size_t index, std::string recorded,
                          std::string_view product) {
    const std::string prefix = std::to_string(index) + "/";
    for (const auto& entry : transcript.normalize) {
        if (entry.value("as", "") == "product" && entry.value("path", "").starts_with(prefix)) {
            return with_product(recorded, product);
        }
    }
    return recorded;
}

std::pair<std::size_t, std::string> split_path(const std::string& path) {
    const auto slash = path.find('/');
    if (slash == std::string::npos || slash == 0) {
        throw std::runtime_error("normalize path without a frame index: " + path);
    }
    return {static_cast<std::size_t>(std::stoul(path.substr(0, slash))), path.substr(slash)};
}

Transcript select(const Transcript& transcript, const std::function<bool(const WireFrame&)>& keep) {
    Transcript out;
    out.scenario = transcript.scenario;
    out.setup = transcript.setup;
    out.commit_of = transcript.commit_of;
    out.normalize = Json::array();
    std::map<std::size_t, std::size_t> moved;
    for (std::size_t i = 0; i < transcript.frames.size(); ++i) {
        if (keep(transcript.frames[i])) {
            moved[i] = out.frames.size();
            out.frames.push_back(transcript.frames[i]);
        }
    }
    for (const auto& entry : transcript.normalize) {
        const auto [index, pointer] = split_path(entry.at("path").get<std::string>());
        if (const auto it = moved.find(index); it != moved.end()) {
            Json copy = entry;
            copy["path"] = std::to_string(it->second) + pointer;
            out.normalize.push_back(std::move(copy));
        }
    }
    return out;
}

} // namespace

std::string with_product(std::string_view text, std::string_view product) {
    // The article before the name follows the name that replaces it: "The host is preparing an
    // OpenWhisper update" is "preparing a FioriVoce update" (lead decision 2).
    const bool vowel = !product.empty() && std::string_view("AEIOUaeiou").find(product.front()) !=
                                               std::string_view::npos;
    std::string out;
    std::size_t at = 0;
    std::size_t found = 0;
    while ((found = text.find(kRecordedProduct, at)) != std::string_view::npos) {
        std::size_t copy_to = found;
        std::string article;
        if (const auto start = article_before(text, found, at)) {
            copy_to = *start;
            article = text[*start] == 'A' ? (vowel ? "An " : "A ") : (vowel ? "an " : "a ");
        }
        out += text.substr(at, copy_to - at);
        out += article;
        out += product;
        at = found + kRecordedProduct.size();
    }
    out += text.substr(at);
    return out;
}

std::vector<std::string> placeholders_in(std::string_view text) {
    std::vector<std::string> out;
    for (auto& hole : holes_in(text)) {
        out.push_back(std::move(hole.name));
    }
    return out;
}

bool has_shape(std::string_view kind, std::string_view text) {
    const Shape* shape = shape_of(kind);
    if (shape == nullptr) {
        return false;
    }
    return std::regex_match(text.begin(), text.end(), std::regex(shape->pattern));
}

std::string fresh_value(std::string_view kind) {
    if (kind == "token") {
        return random_from("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 43);
    }
    if (kind == "device_id" || kind == "uuid") {
        return random_from(kLowerHex, 32);
    }
    if (kind == "nonce" || kind == "commit") {
        return random_from(kLowerHex, 64);
    }
    if (kind == "fingerprint") {
        return random_from("0123456789ABCDEF", 64);
    }
    if (kind == "conn_id") {
        return random_from(kLowerHex, 8);
    }
    if (kind == "host_ms") {
        return "0.5";
    }
    if (kind == "iso_time") {
        return "2026-01-01T00:00:00+00:00";
    }
    throw std::invalid_argument("no placeholder kind " + std::string(kind));
}

std::string commitment_of(std::string_view nonce_hex) {
    std::vector<std::byte> data;
    for (char c : kCommitDomain) {
        data.push_back(static_cast<std::byte>(c));
    }
    const auto nonce = from_hex(nonce_hex);
    data.insert(data.end(), nonce.begin(), nonce.end());
    return sha256_hex(data);
}

Transcript parse_transcript(const Json& document) {
    Transcript out;
    out.scenario = document.at("scenario").get<std::string>();
    out.setup = document.value("setup", Json::object());
    out.normalize = document.value("normalize", Json::array());
    for (const auto& raw : document.at("frames")) {
        WireFrame frame;
        frame.step = raw.value("step", "");
        frame.conn = raw.value("conn", -1);
        frame.dir = raw.at("dir").get<std::string>();
        frame.kind = raw.at("kind").get<std::string>();
        if (frame.dir != "c2s" && frame.dir != "s2c") {
            throw std::runtime_error("a frame's dir is " + frame.dir);
        }
        if (frame.kind == "text") {
            frame.text = raw.at("text").get<std::string>();
        } else if (frame.kind == "binary") {
            frame.has_header = raw.contains("header");
            frame.text = raw.value("header", "");
            if (raw.contains("payload_hex")) {
                frame.payload = from_hex(raw.at("payload_hex").get<std::string>());
                frame.payload_len = frame.payload.size();
            } else {
                frame.payload_sha256 = raw.at("payload_sha256").get<std::string>();
                frame.payload_len = raw.at("payload_len").get<std::size_t>();
                if (raw.contains("payload_fill")) {
                    const auto fill = from_hex(raw.at("payload_fill").get<std::string>());
                    if (fill.size() != 1) {
                        throw std::runtime_error("payload_fill is not one byte: " +
                                                 raw.at("payload_fill").dump());
                    }
                    frame.payload_fill = std::to_integer<int>(fill[0]);
                }
            }
        } else if (frame.kind == "close") {
            frame.code = raw.at("code").get<int>();
            frame.reason = raw.at("reason").get<std::string>();
            frame.echo = raw.value("echo", false);
        } else if (frame.kind == "http") {
            frame.text = raw.value("text", "");
            frame.status = raw.value("status", 0);
            frame.body = raw.value("body", "");
        } else {
            throw std::runtime_error("a frame's kind is " + frame.kind);
        }
        out.frames.push_back(std::move(frame));
    }
    // Every entry must point at a placeholder of its kind (or, for product, at copy holding the
    // recorded product name), so a transcript edited by hand cannot drift from its list.
    for (const auto& entry : out.normalize) {
        const std::string kind = entry.at("as").get<std::string>();
        const auto [index, pointer] = split_path(entry.at("path").get<std::string>());
        if (index >= out.frames.size()) {
            throw std::runtime_error("normalize path past the last frame: " + entry.dump());
        }
        const WireFrame& frame = out.frames[index];
        const Json value = Json::parse(frame.text).at(Json::json_pointer(pointer));
        if (kind == "product") {
            if (!value.is_string() ||
                value.get<std::string>().find(kRecordedProduct) == std::string::npos) {
                throw std::runtime_error("product entry without the product name: " + entry.dump());
            }
            continue;
        }
        if (shape_of(kind) == nullptr || !value.is_string() ||
            !value.get<std::string>().starts_with("<" + kind + "#")) {
            throw std::runtime_error("normalize entry " + entry.dump() + " finds " + value.dump());
        }
        if (kind == "commit" && entry.contains("of")) {
            out.commit_of[value.get<std::string>()] = entry.at("of").get<std::string>();
        }
    }
    return out;
}

Transcript load_transcript_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("no transcript " + path.generic_string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return parse_transcript(Json::parse(text.str()));
    } catch (const std::exception& error) {
        throw std::runtime_error(path.generic_string() + ": " + error.what());
    }
}

Transcript load_transcript(std::string_view scenario) {
    std::string name(scenario);
    if (!name.starts_with(kPrefix)) {
        name = std::string(kPrefix) + name;
    }
    return load_transcript_file(testkit::source_root() / "tests" / "fixtures" / "transcripts" /
                                (name + ".json"));
}

Transcript only_steps(const Transcript& transcript, std::span<const std::string> steps) {
    return select(transcript, [&](const WireFrame& frame) {
        return std::ranges::find(steps, frame.step) != steps.end();
    });
}

Transcript only_connections(const Transcript& transcript, std::span<const int> wanted) {
    return select(transcript, [&](const WireFrame& frame) {
        return std::ranges::find(wanted, frame.conn) != wanted.end();
    });
}

std::vector<int> connections(const Transcript& transcript) {
    std::vector<int> out;
    for (const auto& frame : transcript.frames) {
        if (std::ranges::find(out, frame.conn) == out.end()) {
            out.push_back(frame.conn);
        }
    }
    return out;
}

std::vector<std::size_t> frames_of(const Transcript& transcript, int connection) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < transcript.frames.size(); ++i) {
        if (transcript.frames[i].conn == connection) {
            out.push_back(i);
        }
    }
    return out;
}

Json step_setup(const Transcript& transcript, std::string_view step) {
    for (const auto& entry : transcript.setup.value("steps", Json::array())) {
        if (entry.value("name", "") == step) {
            return entry;
        }
    }
    return nullptr;
}

WireFrame text_frame(std::string dir, std::string text) {
    WireFrame frame;
    frame.dir = std::move(dir);
    frame.kind = "text";
    frame.text = std::move(text);
    return frame;
}

WireFrame binary_frame(std::string dir, std::span<const std::byte> bytes) {
    WireFrame frame;
    frame.dir = std::move(dir);
    frame.kind = "binary";
    if (bytes.size() >= 4) {
        const std::size_t length = (std::to_integer<std::size_t>(bytes[0]) << 24) |
                                   (std::to_integer<std::size_t>(bytes[1]) << 16) |
                                   (std::to_integer<std::size_t>(bytes[2]) << 8) |
                                   std::to_integer<std::size_t>(bytes[3]);
        if (length <= bytes.size() - 4 && valid_utf8(bytes.subspan(4, length))) {
            frame.has_header = true;
            const auto header = bytes.subspan(4, length);
            frame.text.assign(reinterpret_cast<const char*>(header.data()), header.size());
            bytes = bytes.subspan(4 + length);
        }
    }
    frame.payload.assign(bytes.begin(), bytes.end());
    frame.payload_len = frame.payload.size();
    return frame;
}

WireFrame close_frame(std::string dir, int code, std::string reason) {
    WireFrame frame;
    frame.dir = std::move(dir);
    frame.kind = "close";
    frame.code = code;
    frame.reason = std::move(reason);
    return frame;
}

WireFrame http_frame(std::string dir, int status, std::string body) {
    WireFrame frame;
    frame.dir = std::move(dir);
    frame.kind = "http";
    frame.status = status;
    frame.body = std::move(body);
    return frame;
}

std::string compare(const Transcript& transcript, std::size_t index, const WireFrame& actual,
                    Json& bindings) {
    return compare(transcript, index, actual, bindings, identity().product);
}

std::string compare(const Transcript& transcript, std::size_t index, const WireFrame& actual,
                    Json& bindings, std::string_view product) {
    if (index >= transcript.frames.size()) {
        return "frame " + std::to_string(index) + " is past the end of " + transcript.scenario +
               " (" + std::to_string(transcript.frames.size()) + " frames); got " +
               describe_actual(actual);
    }
    if (!bindings.is_object()) {
        bindings = Json::object();
    }
    const WireFrame& recorded = transcript.frames[index];
    const std::string where = transcript.scenario + " " + describe(recorded, index);
    if (!actual.dir.empty() && actual.dir != recorded.dir) {
        return where + ": got a " + actual.dir + " frame";
    }
    if (actual.kind != recorded.kind) {
        return where + ": got " + describe_actual(actual);
    }
    if (recorded.kind == "text") {
        const std::string expected = expected_text(transcript, index, recorded.text, product);
        if (auto diff = match_template(transcript, expected, actual.text, bindings);
            !diff.empty()) {
            return where + ": " + diff;
        }
        return {};
    }
    if (recorded.kind == "binary") {
        if (actual.has_header != recorded.has_header) {
            return where + (recorded.has_header ? ": expected a request header, got none"
                                                : ": expected no request header, got one");
        }
        if (recorded.has_header) {
            const std::string expected = expected_text(transcript, index, recorded.text, product);
            if (auto diff = match_template(transcript, expected, actual.text, bindings);
                !diff.empty()) {
                return where + " header: " + diff;
            }
        }
        if (!recorded.payload_sha256.empty()) {
            if (actual.payload.size() != recorded.payload_len ||
                sha256_hex(actual.payload) != recorded.payload_sha256) {
                return where + ": the payload (" + std::to_string(actual.payload.size()) +
                       " bytes) is not the recorded one (" + std::to_string(recorded.payload_len) +
                       " bytes)";
            }
        } else if (actual.payload != recorded.payload) {
            return where + ": the payload differs (" + std::to_string(actual.payload.size()) +
                   " bytes, recorded " + std::to_string(recorded.payload.size()) + ")";
        }
        return {};
    }
    if (recorded.kind == "close") {
        if (actual.code != recorded.code || actual.reason != recorded.reason) {
            return where + ": expected close " + std::to_string(recorded.code) + " \"" +
                   recorded.reason + "\", got close " + std::to_string(actual.code) + " \"" +
                   actual.reason + "\"";
        }
        return {};
    }
    if (actual.status != recorded.status) {
        return where + ": expected HTTP " + std::to_string(recorded.status) + ", got HTTP " +
               std::to_string(actual.status);
    }
    if (!actual.body.empty() && actual.body != recorded.body) {
        return where + ": expected the body \"" + recorded.body + "\", got \"" + actual.body + "\"";
    }
    if (recorded.dir == "c2s" && !actual.text.empty() && actual.text != recorded.text) {
        return where + ": expected the path " + recorded.text + ", got " + actual.text;
    }
    return {};
}

WireFrame render(const Transcript& transcript, std::size_t index, Json& bindings) {
    if (index >= transcript.frames.size()) {
        throw std::out_of_range("no frame " + std::to_string(index) + " in " + transcript.scenario);
    }
    if (!bindings.is_object()) {
        bindings = Json::object();
    }
    WireFrame frame = transcript.frames[index];
    const auto value_of = [&](const Hole& hole) -> std::string {
        if (!hole.shape->bound) {
            return fresh_value(hole.shape->kind);
        }
        if (!bindings.contains(hole.name)) {
            std::string value;
            if (const auto it = transcript.commit_of.find(hole.name);
                it != transcript.commit_of.end()) {
                if (!bindings.contains(it->second)) {
                    bindings[it->second] = fresh_value("nonce");
                }
                value = commitment_of(bindings[it->second].get<std::string>());
            } else {
                value = fresh_value(hole.shape->kind);
            }
            bindings[hole.name] = value;
        }
        return bindings[hole.name].get<std::string>();
    };
    const auto fill = [&](const std::string& text) {
        std::string out;
        std::size_t at = 0;
        for (const auto& hole : holes_in(text)) {
            out += text.substr(at, hole.begin - at);
            const std::string value = value_of(hole);
            out += hole.shape->number ? value : "\"" + value + "\"";
            at = hole.end;
        }
        out += text.substr(at);
        return out;
    };
    if (frame.kind == "text" || (frame.kind == "binary" && frame.has_header)) {
        frame.text = fill(frame.text);
    }
    if (frame.kind == "binary" && !frame.payload_sha256.empty()) {
        if (frame.payload_fill < 0) {
            throw std::runtime_error(transcript.scenario + " " + describe(frame, index) +
                                     ": the payload is known only by its digest");
        }
        frame.payload.assign(frame.payload_len, static_cast<std::byte>(frame.payload_fill));
        frame.payload_sha256.clear();
        frame.payload_fill = -1;
    }
    return frame;
}

std::vector<std::byte> wire_bytes(const WireFrame& frame) {
    std::vector<std::byte> out;
    if (frame.has_header) {
        const auto length = static_cast<std::uint32_t>(frame.text.size());
        out.push_back(static_cast<std::byte>(length >> 24));
        out.push_back(static_cast<std::byte>(length >> 16));
        out.push_back(static_cast<std::byte>(length >> 8));
        out.push_back(static_cast<std::byte>(length));
        for (char c : frame.text) {
            out.push_back(static_cast<std::byte>(c));
        }
    }
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

} // namespace fv::testing::remote_wire
