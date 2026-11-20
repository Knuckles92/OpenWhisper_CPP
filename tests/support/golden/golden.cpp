#include "golden/golden.h"

#include "testkit/testkit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

namespace fv::golden {
namespace detail {

[[noreturn]] void fail(std::string_view expected, const Json& value) {
    throw DecodeError("golden vector: expected " + std::string(expected) + ", got " + value.dump());
}

double decode_double(const Json& value) {
    if (value.is_number() && !value.is_boolean()) {
        return value.get<double>();
    }
    if (value.is_object() && value.size() == 1 && value.contains("$float") &&
        value["$float"].is_string()) {
        const auto tag = value["$float"].get<std::string>();
        if (tag == "nan") {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (tag == "inf") {
            return std::numeric_limits<double>::infinity();
        }
        if (tag == "-inf") {
            return -std::numeric_limits<double>::infinity();
        }
    }
    fail("a number", value);
}

std::int64_t decode_integer(const Json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value <= static_cast<std::uint64_t>(INT64_MAX)) {
            return static_cast<std::int64_t>(unsigned_value);
        }
    }
    fail("an integer", value);
}

} // namespace detail

namespace {

const Json& empty_object() {
    static const Json object = Json::object();
    return object;
}

Json read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("golden: cannot read " + path.string());
    }
    std::ostringstream text;
    text << stream.rdbuf();
    // Python writes NaN as a tagged object, so a plain parse is exact.
    return Json::parse(text.str());
}

double file_tolerance(const Json& document) {
    const auto it = document.find("float_tolerance");
    return it != document.end() && it->is_number() ? it->get<double>() : 0.0;
}

std::optional<double> as_double(const Json& value) {
    try {
        return detail::decode_double(value);
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

bool compare(const Json& expected, const Json& actual, double tolerance, bool approximate) {
    const bool expected_number = (expected.is_number() && !expected.is_boolean()) ||
                                 (expected.is_object() && expected.contains("$float"));
    const bool actual_number = (actual.is_number() && !actual.is_boolean()) ||
                               (actual.is_object() && actual.contains("$float"));
    if (expected_number && actual_number) {
        const auto a = as_double(expected);
        const auto b = as_double(actual);
        if (!a || !b) {
            return false;
        }
        if (expected.is_number_integer() && actual.is_number_integer()) {
            return expected.get<std::int64_t>() == actual.get<std::int64_t>();
        }
        return approximate ? near(*a, *b, tolerance) : near(*a, *b, 0.0);
    }
    if (expected.type() != actual.type()) {
        return false;
    }
    if (expected.is_array()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (!compare(expected[i], actual[i], tolerance, approximate)) {
                return false;
            }
        }
        return true;
    }
    if (expected.is_object()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (const auto& [key, value] : expected.items()) {
            const auto it = actual.find(key);
            if (it == actual.end() || !compare(value, *it, tolerance, approximate)) {
                return false;
            }
        }
        return true;
    }
    return expected == actual;
}

} // namespace

Case::Case(std::string function, std::size_t index, const Json& raw, double float_tolerance)
    : function_(std::move(function)), index_(index), raw_(raw), tolerance_(float_tolerance) {
    if (!raw_.is_object() || !raw_.contains("args") || !raw_["args"].is_array() ||
        (raw_.contains("result") == raw_.contains("raises"))) {
        throw DecodeError(label() + ": a case needs \"args\" and one of \"result\", \"raises\"");
    }
}

std::size_t Case::arg_count() const {
    return args().size();
}

const Json& Case::args() const {
    return raw_.at("args");
}

const Json& Case::kwargs() const {
    const auto it = raw_.find("kwargs");
    return it != raw_.end() ? *it : empty_object();
}

bool Case::raises() const {
    return raw_.contains("raises");
}

std::string_view Case::raises_type() const {
    const auto it = raw_.find("raises");
    if (it == raw_.end() || !it->is_string()) {
        return {};
    }
    return it->get_ref<const std::string&>();
}

const Json& Case::raw() const {
    return raw_;
}

bool Case::traced() const {
    const auto it = raw_.find("traced");
    return it != raw_.end() && it->is_boolean() && it->get<bool>();
}

double Case::float_tolerance() const {
    return tolerance_;
}

std::string Case::label() const {
    std::string text = function_ + " #" + std::to_string(index_);
    if (raw_.is_object() && raw_.contains("args")) {
        text += " " + raw_["args"].dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    return text;
}

std::filesystem::path vector_file(std::string_view module) {
    return testkit::golden_path(module);
}

const Json& load(std::string_view module) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<Json>, std::less<>> cache;
    const std::lock_guard lock(mutex);
    if (const auto it = cache.find(module); it != cache.end()) {
        return *it->second;
    }
    auto document = std::make_unique<Json>(read_file(vector_file(module)));
    const auto declared = document->value("module", std::string());
    if (declared != module) {
        throw std::runtime_error("golden: " + vector_file(module).string() + " declares module \"" +
                                 declared + "\"");
    }
    return *cache.emplace(std::string(module), std::move(document)).first->second;
}

void for_each_in(const Json& document, std::string_view function,
                 const std::function<void(const Case&)>& body) {
    const std::string name(function);
    const auto functions = document.find("functions");
    if (functions == document.end() || !functions->contains(name)) {
        FAIL("no golden vectors for " << document.value("module", std::string("?")) << "." << name);
    }
    const Json& cases = (*functions)[name];
    if (!cases.is_array() || cases.empty()) {
        FAIL("the golden vectors for " << name << " hold no cases");
    }
    const double tolerance = file_tolerance(document);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case one(name, i, cases[i], tolerance);
        DYNAMIC_SECTION(one.label()) {
            body(one);
        }
    }
}

void for_each(std::string_view module, std::string_view function,
              const std::function<void(const Case&)>& body) {
    for_each_in(load(module), function, body);
}

std::string rebrand(std::string_view text) {
    // Every name AppIdentity holds, OpenWhisper's spelling first (architecture 3.1). At each
    // position the longest name wins, so ".openwhisper-data.lock" is not read as a product name.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kNames{{
        {"openwhisper_settings.json", "fiorivoce_settings.json"},
        {".openwhisper-data.lock", ".fiorivoce-data.lock"},
        {".openwhisper-restore", ".fiorivoce-restore"},
        {"openwhisper-backup", "fiorivoce-backup"},
        {"openwhisper.log", "fiorivoce.log"},
        {"openwhisper.db", "fiorivoce.db"},
        {"openwhisper-ui", "fiorivoce-ui"},
        {"OPENWHISPER_", "FIORIVOCE_"},
        {"OpenWhisper", "FioriVoce"},
        {".owbackup", ".fvbackup"},
    }};
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::pair<std::string_view, std::string_view>* best = nullptr;
        for (const auto& name : kNames) {
            if (text.substr(i).starts_with(name.first) &&
                (best == nullptr || name.first.size() > best->first.size())) {
                best = &name;
            }
        }
        if (best != nullptr) {
            out += best->second;
            i += best->first.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

bool equal(const Json& expected, const Json& actual) {
    return compare(expected, actual, 0.0, false);
}

bool near(double expected, double actual, double tolerance) {
    if (std::isnan(expected) || std::isnan(actual)) {
        return std::isnan(expected) && std::isnan(actual);
    }
    if (std::isinf(expected) || std::isinf(actual)) {
        return expected == actual;
    }
    return std::fabs(expected - actual) <= tolerance;
}

bool matches(const Json& expected, const Json& actual, double tolerance) {
    return compare(expected, actual, tolerance, true);
}

bool equal_rebranded(std::string_view expected, std::string_view actual) {
    return rebrand(expected) == actual;
}

} // namespace fv::golden
