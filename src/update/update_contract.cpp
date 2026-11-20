#include "update/update_contract.h"

#include "base/identity.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "update/release_identity.h"

namespace fv::update {

namespace {

std::string lower_ascii(std::string text) {
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

} // namespace

std::string_view apply_mode_name(ApplyMode mode) {
    switch (mode) {
    case ApplyMode::Native:
        return "native";
    case ApplyMode::Setup:
        return "setup";
    case ApplyMode::NotifyOnly:
        break;
    }
    return "notify_only";
}

std::string manifest_name() {
    return "." + lower_ascii(std::string(identity().product)) + "-update.json";
}

std::string sentinel_name() {
    return "." + lower_ascii(std::string(identity().product)) + "-update-complete";
}

std::string runonce_value_name() {
    return "!" + std::string(identity().product) + "UpdateRecover";
}

std::string normalize_version(std::string_view raw) {
    auto text = py::strip(raw);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.erase(0, 1);
    }
    return py::strip(text);
}

std::array<long long, 3> parse_strict_version(std::string_view raw) {
    // Python's \d: any Unicode decimal digit, which int() then reads.
    static const py::Regex kVersion(R"(\d+\.\d+\.\d+\Z)");
    const auto text = normalize_version(raw);
    const auto fail = [&] {
        return py::ValueError("Not a strict major.minor.patch version: " + py::repr(raw));
    };
    if (!kVersion.fullmatch(text)) {
        throw fail();
    }
    const auto parts = py::split_on(text, ".");
    std::array<long long, 3> numbers{};
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        const auto value = py::py_int(parts.at(i));
        if (!value) {
            throw fail();
        }
        numbers[i] = *value;
    }
    return numbers;
}

bool is_newer_version(std::string_view current, std::string_view candidate) {
    return parse_strict_version(candidate) > parse_strict_version(current);
}

bool is_setup_bridge_release(std::string_view version) {
    return parse_strict_version(version) <= parse_strict_version(kMinimumUpdaterVersion);
}

std::string setup_asset_name(std::string_view version) {
    return release_identity().setup_prefix + normalize_version(version) + ".exe";
}

std::string archive_asset_name(std::string_view version) {
    return std::string(identity().product) + "-" + normalize_version(version) + "-win64.zip";
}

std::string validate_transaction_id(std::string_view transaction_id) {
    static const py::Regex kTransactionId(R"([0-9a-f]{32}\Z)");
    auto token = py::lower(py::strip(transaction_id));
    if (!kTransactionId.fullmatch(token)) {
        throw py::ValueError("Invalid update transaction id.");
    }
    return token;
}

std::string encode_native_result(std::string_view transaction_id) {
    return std::string(kNativeResultPrefix) + validate_transaction_id(transaction_id);
}

std::optional<std::string> decode_native_result(std::string_view raw) {
    if (!raw.starts_with(kNativeResultPrefix)) {
        return std::nullopt;
    }
    try {
        return validate_transaction_id(raw.substr(kNativeResultPrefix.size()));
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
}

} // namespace fv::update
