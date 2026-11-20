#include "appkit/app_context.h"

#include <format>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace fv {

namespace {

template <class T> T& require(T* service, std::string_view name) {
    if (service == nullptr) {
        throw ServiceUnavailable(std::format("{} is not available yet.", name));
    }
    return *service;
}

#if !defined(__GNUC__) && !defined(__clang__)
void erase_all(std::string& text, std::string_view word) {
    for (auto at = text.find(word); at != std::string::npos; at = text.find(word, at)) {
        text.erase(at, word.size());
    }
}
#endif

} // namespace

std::string type_display_name(const std::type_info& type) {
#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    std::string name = status == 0 && demangled != nullptr ? demangled : type.name();
    std::free(demangled);
    return name;
#else
    std::string name = type.name();
    for (const std::string_view word : {"class ", "struct ", "enum ", "union "}) {
        erase_all(name, word);
    }
    return name;
#endif
}

AppContext::AppContext(AppServices services)
    : services_(services), screens_(std::make_unique<ScreenRegistry>()),
      menus_(std::make_unique<MenuRegistry>()),
      settings_pages_(std::make_unique<SettingsPageRegistry>()) {}

AppContext::~AppContext() {
    provided_.clear();
    while (!provision_order_.empty()) {
        provision_order_.pop_back();
    }
}

const AppIdentity& AppContext::identity() {
    return require(services_.identity, "The application identity");
}

const Paths& AppContext::paths() {
    return require(services_.paths, "Paths");
}

Clock& AppContext::clock() {
    return require(services_.clock, "The clock");
}

SettingsStore& AppContext::settings() {
    return require(services_.settings, "SettingsStore");
}

CredentialStore& AppContext::credentials() {
    return require(services_.credentials, "CredentialStore");
}

Database& AppContext::db() {
    return require(services_.db, "Database");
}

http::Client& AppContext::http() {
    return require(services_.http, "http::Client");
}

net::Reactor& AppContext::net() {
    return require(services_.net, "net::Reactor");
}

audio::Backend& AppContext::audio() {
    return require(services_.audio, "audio::Backend");
}

speech::SpeechService& AppContext::speech() {
    return require(services_.speech, "speech::SpeechService");
}

EventSink& AppContext::events() {
    return require(services_.events, "EventSink");
}

Executor& AppContext::ui() {
    return require(services_.ui, "The ui executor");
}

Executor& AppContext::jobs() {
    return require(services_.jobs, "The jobs executor");
}

Executor& AppContext::persistence() {
    return require(services_.persistence, "The persistence executor");
}

Executor& AppContext::io() {
    return require(services_.io, "The io executor");
}

ScreenRegistry& AppContext::screens() {
    return *screens_;
}

MenuRegistry& AppContext::menus() {
    return *menus_;
}

SettingsPageRegistry& AppContext::settingsPages() {
    return *settings_pages_;
}

ShutdownPlan& AppContext::shutdown() {
    return require(services_.shutdown, "ShutdownPlan");
}

CancelRouter& AppContext::cancel() {
    return require(services_.cancel, "CancelRouter");
}

ActivityState& AppContext::activity() {
    return require(services_.activity, "ActivityState");
}

void AppContext::throw_unavailable(const std::type_info& type) {
    throw ServiceUnavailable(
        std::format("{} is not available: no feature provides it.", type_display_name(type)));
}

void AppContext::add_provided(const std::type_info& type, std::shared_ptr<void> value) {
    const std::string name = type_display_name(type);
    if (!value) {
        throw std::invalid_argument(std::format("A feature provided no {}.", name));
    }
    if (provided_.contains(std::type_index(type))) {
        throw std::invalid_argument(std::format("{} is provided twice.", name));
    }
    provided_.emplace(std::type_index(type), value);
    provision_order_.push_back(std::move(value));
}

} // namespace fv
