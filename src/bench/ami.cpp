#include "bench/ami.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <tuple>

#include <pugixml.hpp>

#include "base/utf8.h"
#include "bench/detail/nfkc.h"
#include "pyport/text.h"

namespace fv::bench {

namespace {

std::string_view local_name(std::string_view tag) {
    // ElementTree names a namespaced tag "{uri}w"; pugixml keeps the prefix ("nite:w").
    const auto colon = tag.rfind(':');
    return colon == std::string_view::npos ? tag : tag.substr(colon + 1);
}

// float(text), for the decimal forms the corpus writes; nullopt where Python raises.
std::optional<double> parse_float(std::string_view text) {
    const std::string stripped = py::strip(text);
    if (stripped.empty()) {
        return std::nullopt;
    }
    std::string_view digits = stripped;
    if (digits.front() == '+') {
        digits.remove_prefix(1);
    }
    double value = 0.0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc() || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

// ElementTree's node.text: the character data before the first child element.
std::string leading_text(const pugi::xml_node& node) {
    std::string text;
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) {
            break;
        }
        text += child.value();
    }
    return text;
}

} // namespace

std::string MeetingSpec::audio_filename() const {
    return meeting_id + ".Mix-Headset.wav";
}

const std::vector<MeetingSpec>& default_meetings() {
    // Naturally occurring, non-scenario research meetings rather than scripted or read speech:
    // strong accents, specialist vocabulary, interruptions and overlap.
    static const std::vector<MeetingSpec> meetings = {
        {"IN1001", "Web-based video browsing; strong French accents"},
        {"IN1002", "Posterior probability methods"},
        {"IN1005", "PLSA methods for indexing web pages"},
        {"IN1007", "Keyword spotting and spectral transforms"},
        {"IN1008", "Audio-based interactive tables; French accents"},
        {"IN1009", "Audio-based interactive tables; French accents"},
        {"IN1012", "Speech researchers discuss Interspeech 2005"},
        {"IN1013", "Approaches for coding spectral information"},
        {"IN1014", "Conference-room recording equipment"},
        {"IN1016", "Slide content for speech recognition"},
    };
    return meetings;
}

std::vector<MeetingSpec> select_meetings(const std::vector<std::string>& ids) {
    std::vector<std::string> requested;
    for (const std::string& id : ids) {
        const std::string stripped = py::strip(id);
        if (!stripped.empty()) {
            requested.push_back(py::upper(stripped));
        }
    }
    if (requested.empty()) {
        return default_meetings();
    }
    std::vector<MeetingSpec> selected;
    std::vector<std::string> unknown;
    for (const std::string& id : requested) {
        const auto& all = default_meetings();
        const auto found = std::find_if(
            all.begin(), all.end(), [&](const MeetingSpec& spec) { return spec.meeting_id == id; });
        if (found == all.end()) {
            unknown.push_back(id);
        } else {
            selected.push_back(*found);
        }
    }
    if (!unknown.empty()) {
        std::string names;
        for (const std::string& id : unknown) {
            names += (names.empty() ? "" : ", ") + id;
        }
        throw std::invalid_argument("Unknown curated AMI meeting id(s): " + names);
    }
    return selected;
}

std::vector<ReferenceWord> parse_reference_words(const std::filesystem::path& annotations_dir,
                                                 std::string_view meeting_id) {
    const std::filesystem::path word_dir = annotations_dir / "words";
    const std::string prefix = std::string(meeting_id) + ".";
    constexpr std::string_view suffix = ".words.xml";
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(word_dir, error)) {
        const std::string name = path_to_utf8(entry.path().filename());
        if (name.starts_with(prefix) && name.size() >= prefix.size() + suffix.size() &&
            name.ends_with(suffix)) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        throw std::runtime_error("No AMI word annotations found for " + std::string(meeting_id) +
                                 " in " + path_to_utf8(word_dir));
    }

    std::vector<std::pair<ReferenceWord, std::size_t>> words;
    std::size_t ordinal = 0;
    for (const auto& path : paths) {
        const std::string name = path_to_utf8(path.filename());
        const auto parts = py::split_on(name, ".");
        const std::string speaker = parts.size() >= 3 ? parts[1] : path_to_utf8(path.stem());
        pugi::xml_document document;
        const auto parsed = document.load_file(path.c_str());
        if (!parsed) {
            throw std::runtime_error("Could not parse " + path_to_utf8(path) + ": " +
                                     parsed.description());
        }
        std::vector<pugi::xml_node> stack{document.document_element()};
        // root.iter(): the element and its descendants in document order.
        std::vector<pugi::xml_node> order;
        while (!stack.empty()) {
            const pugi::xml_node node = stack.back();
            stack.pop_back();
            order.push_back(node);
            std::vector<pugi::xml_node> children;
            for (const pugi::xml_node child : node.children(/*element only*/)) {
                if (child.type() == pugi::node_element) {
                    children.push_back(child);
                }
            }
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
        for (const pugi::xml_node& node : order) {
            if (local_name(node.name()) != "w" ||
                std::string_view(node.attribute("punc").value()) == "true") {
                continue;
            }
            const std::string text = py::strip(detail::nfkc(leading_text(node)));
            if (text.empty()) {
                continue;
            }
            const pugi::xml_attribute start = node.attribute("starttime");
            const pugi::xml_attribute end = node.attribute("endtime");
            if (!start || !end) {
                continue;
            }
            const auto start_s = parse_float(start.value());
            const auto end_s = parse_float(end.value());
            if (!start_s || !end_s) {
                continue;
            }
            words.push_back({ReferenceWord{text, *start_s, *end_s, speaker}, ordinal++});
        }
    }
    // Timing ties happen for overlap and zero-length tokens; source order is the last key so
    // repeated runs compare byte for byte.
    std::sort(words.begin(), words.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first.start_s, a.first.end_s, a.second) <
               std::tie(b.first.start_s, b.first.end_s, b.second);
    });
    std::vector<ReferenceWord> result;
    result.reserve(words.size());
    for (auto& item : words) {
        result.push_back(std::move(item.first));
    }
    if (result.empty()) {
        throw std::invalid_argument("No usable AMI reference words found for " +
                                    std::string(meeting_id));
    }
    return result;
}

std::filesystem::path annotation_root(const std::filesystem::path& data_dir) {
    return data_dir / "annotations";
}

std::filesystem::path audio_path(const std::filesystem::path& data_dir,
                                 std::string_view meeting_id) {
    return data_dir / "audio" / path_from_utf8(std::string(meeting_id) + ".Mix-Headset.wav");
}

} // namespace fv::bench
