#include "update/release_identity.h"

#include <stdexcept>

#include "base/identity.h"
#include "base/json.h"
#include "update/data/release_identity.json.h"

namespace fv::update {

namespace {

bool is_upper_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

// "{8-4-4-4-12}" in upper-case hex: the form Inno's AppId and the mutex names use.
bool is_braced_guid(std::string_view text) {
    static constexpr std::string_view kShape = "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}";
    if (text.size() != kShape.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char want = kShape[i];
        if (want == 'x' ? !is_upper_hex(text[i]) : text[i] != want) {
            return false;
        }
    }
    return true;
}

std::string text_field(const Json& document, std::string_view key, bool allow_empty) {
    const auto found = document.find(std::string(key));
    if (found == document.end() || !found->is_string() ||
        (!allow_empty && found->get_ref<const std::string&>().empty())) {
        throw std::invalid_argument("release_identity.json: \"" + std::string(key) +
                                    "\" must be a" + (allow_empty ? "" : " non-empty") + " string");
    }
    return found->get<std::string>();
}

UpdateSource source_field(const Json& document) {
    const auto found = document.find("source");
    if (found == document.end() || !found->is_object()) {
        throw std::invalid_argument("release_identity.json: \"source\" must be an object");
    }
    const auto kind = text_field(*found, "kind", false);
    if (kind == "none") {
        return {};
    }
    if (kind != "github") {
        throw std::invalid_argument(
            "release_identity.json: \"source.kind\" must be \"none\" or \"github\"");
    }
    UpdateSource source;
    source.kind = UpdateSource::Kind::GitHub;
    source.repo = text_field(*found, "repo", false);
    const auto slash = source.repo.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == source.repo.size() ||
        source.repo.find('/', slash + 1) != std::string::npos) {
        throw std::invalid_argument(
            "release_identity.json: \"source.repo\" must be \"<owner>/<name>\"");
    }
    return source;
}

} // namespace

ReleaseIdentity parse_release_identity(std::string_view json_text) {
    Json document;
    try {
        document = Json::parse(json_text);
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("release_identity.json: ") + error.what());
    }
    if (!document.is_object()) {
        throw std::invalid_argument("release_identity.json: the document must be an object");
    }
    ReleaseIdentity identity;
    identity.app_id = text_field(document, "app_id", false);
    if (!is_braced_guid(identity.app_id)) {
        throw std::invalid_argument(
            "release_identity.json: \"app_id\" must be a braced upper-case GUID");
    }
    identity.publisher = text_field(document, "publisher", false);
    identity.publisher_url = text_field(document, "publisher_url", true);
    identity.install_dir_name = text_field(document, "install_dir_name", false);
    identity.app_exe = text_field(document, "app_exe", false);
    identity.cli_exe = text_field(document, "cli_exe", false);
    identity.setup_prefix = text_field(document, "setup_prefix", false);
    identity.source = source_field(document);
    identity.timestamp_url = text_field(document, "timestamp_url", true);
    return identity;
}

const ReleaseIdentity& release_identity() {
    static const ReleaseIdentity identity =
        parse_release_identity(embedded::update_release_identity);
    return identity;
}

std::string ReleaseIdentity::app_id_bare() const {
    return app_id.size() >= 2 ? app_id.substr(1, app_id.size() - 2) : app_id;
}

std::string ReleaseIdentity::app_mutex() const {
    return std::string(identity().product) + "-App-" + app_id_bare();
}

std::string ReleaseIdentity::app_mutex_global() const {
    return "Global\\" + app_mutex();
}

std::string ReleaseIdentity::setup_mutex() const {
    return "Global\\" + std::string(identity().product) + "-Setup-" + app_id_bare();
}

std::vector<std::string> ReleaseIdentity::app_mutexes() const {
    return {app_mutex(), app_mutex_global()};
}

std::vector<std::string> ReleaseIdentity::update_mutexes() const {
    const auto local = std::string(identity().product) + "-Update-" + app_id_bare();
    return {local, "Global\\" + local};
}

std::string ReleaseIdentity::inno_uninstall_key() const {
    return app_id + "_is1";
}

std::string ReleaseIdentity::uninstall_key_path() const {
    return "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + inno_uninstall_key();
}

} // namespace fv::update
