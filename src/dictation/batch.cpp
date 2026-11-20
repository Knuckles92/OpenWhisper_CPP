#include "dictation/batch.h"

#include <cmath>
#include <format>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::dictation {

namespace {

bool is_separator(char c) {
    return c == '\\' || c == '/';
}

// ntpath.splitroot's drive: "C:", "\\server\share", "\\?\UNC\server\share", "\\.\device".
// Returns how many bytes of `path` it takes.
std::size_t drive_length(std::string_view path) {
    if (!path.empty() && is_separator(path[0])) {
        if (path.size() < 2 || !is_separator(path[1])) {
            return 0;
        }
        std::size_t start = 2;
        if (path.size() >= 8) {
            std::string prefix(path.substr(0, 8));
            for (char& c : prefix) {
                if (c == '/') {
                    c = '\\';
                } else if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            if (prefix == "\\\\?\\UNC\\") {
                start = 8;
            }
        }
        std::size_t index = start;
        while (index < path.size() && !is_separator(path[index])) {
            ++index;
        }
        if (index >= path.size()) {
            return path.size();
        }
        std::size_t index2 = index + 1;
        while (index2 < path.size() && !is_separator(path[index2])) {
            ++index2;
        }
        return index2;
    }
    // A drive letter is the first code point, whatever its UTF-8 length.
    std::size_t second = 0;
    if (!path.empty()) {
        py::next_code_point(path, second);
    }
    if (second < path.size() && path[second] == ':') {
        return second + 1;
    }
    return 0;
}

std::string ntpath_basename(std::string_view path) {
    const std::string_view rest = path.substr(drive_length(path));
    std::size_t i = rest.size();
    while (i > 0 && !is_separator(rest[i - 1])) {
        --i;
    }
    return std::string(rest.substr(i));
}

// str.format with the named fields {count}, {names} and {name}, in one pass so that a file name
// holding braces is never substituted again.
std::string format_named(std::string_view text, std::size_t count, std::string_view names,
                         std::string_view name = {}) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            const std::string_view field = text.substr(i + 1, close - i - 1);
            if (field == "count") {
                out += std::to_string(count);
            } else if (field == "names") {
                out += names;
            } else {
                out += name;
            }
            i = close + 1;
        } else {
            out += text[i++];
        }
    }
    return out;
}

std::string join(std::span<const std::string> parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

// sum() of floats in Python 3.12: Neumaier's compensated sum, the compensation added at the end
// only when it is non-zero and finite.
class PythonSum {
public:
    void add(double value) {
        const double t = total_ + value;
        if (std::fabs(total_) >= std::fabs(value)) {
            compensation_ += (total_ - t) + value;
        } else {
            compensation_ += (value - t) + total_;
        }
        total_ = t;
    }
    double result() const {
        if (compensation_ != 0.0 && std::isfinite(compensation_)) {
            return total_ + compensation_;
        }
        return total_;
    }

private:
    double total_ = 0.0;
    double compensation_ = 0.0;
};

} // namespace

std::string_view to_string(BatchRelation relation) {
    switch (relation) {
    case BatchRelation::Separate:
        return "separate";
    case BatchRelation::Sequential:
        return "sequential";
    case BatchRelation::Custom:
        return "custom";
    }
    return "separate";
}

std::optional<BatchRelation> batch_relation_from(std::string_view value) {
    if (value == "separate") {
        return BatchRelation::Separate;
    }
    if (value == "sequential") {
        return BatchRelation::Sequential;
    }
    if (value == "custom") {
        return BatchRelation::Custom;
    }
    return std::nullopt;
}

std::string BatchItem::source_name() const {
    return ntpath_basename(audio_path);
}

bool BatchUploadRequest::combine() const {
    if (relation == BatchRelation::Sequential) {
        return true;
    }
    if (relation == BatchRelation::Custom) {
        return custom_combine;
    }
    return false;
}

std::vector<std::string> BatchUploadRequest::source_names() const {
    std::vector<std::string> names;
    names.reserve(items.size());
    for (const BatchItem& item : items) {
        names.push_back(item.source_name());
    }
    return names;
}

std::optional<std::string> BatchUploadRequest::batch_context(const BatchItem* item) const {
    const std::size_t count = items.size();
    const std::string names = join(source_names(), ", ");
    if (relation == BatchRelation::Sequential) {
        return format_named(TRANSCRIPT_BATCH_PRESET_SEQUENTIAL, count, names);
    }
    if (relation != BatchRelation::Custom) {
        return std::nullopt;
    }
    const std::string text = py::strip(custom_instructions);
    if (text.empty()) {
        return std::nullopt;
    }
    std::string note;
    if (combine()) {
        note = format_named(TRANSCRIPT_BATCH_CUSTOM_COMBINED_NOTE, count, names);
    } else {
        const std::string name = item != nullptr ? item->source_name() : "one file";
        note = format_named(TRANSCRIPT_BATCH_CUSTOM_SEPARATE_NOTE, count, names, name);
    }
    return note + "\n" + text;
}

double BatchResult::total_duration_seconds() const {
    PythonSum sum;
    for (const BatchItemResult& result : items) {
        // `duration_seconds or 0.0`.
        sum.add(result.item.duration_seconds.value_or(0.0));
    }
    return sum.result();
}

std::int64_t BatchResult::total_file_size() const {
    std::int64_t total = 0;
    for (const BatchItemResult& result : items) {
        total += result.file_size.value_or(0);
    }
    return total;
}

double BatchResult::effective_transcription_time_s() const {
    if (transcription_time_s > 0) {
        return transcription_time_s;
    }
    PythonSum sum;
    for (const BatchItemResult& result : items) {
        sum.add(result.elapsed_s);
    }
    const double summed = sum.result();
    if (summed > 0) {
        return summed;
    }
    return total_elapsed_s;
}

double BatchResult::effective_cleanup_time_s() const {
    if (cleanup_time_s > 0) {
        return cleanup_time_s;
    }
    PythonSum sum;
    for (const BatchItemResult& result : items) {
        sum.add(result.cleanup_elapsed_s);
    }
    return sum.result();
}

std::string compose_batch_cleanup_prompt(std::string_view base_prompt,
                                         std::string_view batch_context) {
    return std::format("{}\n\n{}\n{}\n\n{}", base_prompt, TRANSCRIPT_BATCH_CONTEXT_HEADER,
                       batch_context, TRANSCRIPT_BATCH_CONTEXT_GUARD);
}

std::string join_raw_parts(std::span<const std::string> parts) {
    std::vector<std::string> kept;
    for (const std::string& part : parts) {
        std::string stripped = py::strip(part);
        if (!stripped.empty()) {
            kept.push_back(std::move(stripped));
        }
    }
    return join(kept, "\n\n");
}

std::string format_batch_transcript(std::span<const std::pair<std::string, std::string>> sections) {
    std::vector<std::string> rendered;
    rendered.reserve(sections.size());
    for (const auto& [name, body] : sections) {
        rendered.push_back(std::format("## {}\n\n{}", name, py::strip(body)));
    }
    return join(rendered, "\n\n");
}

std::string batch_source_name(std::span<const std::string> names, int limit) {
    const std::string prefix = std::format("{} files: ", names.size());
    const std::size_t prefix_length = py::len(prefix);
    std::size_t shown = 0;
    std::size_t shown_length = 0; // code points of the shown names joined with ", "
    for (std::size_t index = 0; index < names.size(); ++index) {
        const std::size_t candidate = shown + 1;
        const std::size_t candidate_length =
            shown_length + (shown != 0 ? 2 : 0) + py::len(names[index]);
        const std::size_t remaining = names.size() - candidate;
        const std::size_t suffix_length =
            remaining != 0 ? py::len(std::format(", +{} more", remaining)) : 0;
        if (shown != 0 &&
            static_cast<std::int64_t>(prefix_length + candidate_length + suffix_length) > limit) {
            break;
        }
        shown = candidate;
        shown_length = candidate_length;
    }
    const std::size_t remaining = names.size() - shown;
    std::string label = prefix + join(names.first(shown), ", ");
    if (remaining != 0) {
        label += std::format(", +{} more", remaining);
    }
    return label;
}

} // namespace fv::dictation
