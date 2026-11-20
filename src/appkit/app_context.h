#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "appkit/registries.h"
#include "base/clock.h"
#include "base/events.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"

namespace fv {

// Services that libraries outside appkit's closure implement; AppContext hands out references.
class SettingsStore;
class CredentialStore;
class Database;
class ShutdownPlan;
class CancelRouter;
class ActivityState;
namespace http {
class Client;
}
namespace net {
class Reactor;
}
namespace audio {
class Backend;
}
namespace speech {
class SpeechService;
}

// What Bootstrap has built. A null entry makes its accessor throw ServiceUnavailable, so a
// context can start with only what exists yet (M0 has no database or speech service).
// Nothing here is owned: Bootstrap keeps every service alive longer than the context.
struct AppServices {
    const AppIdentity* identity = nullptr;
    const Paths* paths = nullptr;
    Clock* clock = nullptr;
    SettingsStore* settings = nullptr;
    CredentialStore* credentials = nullptr;
    Database* db = nullptr;
    http::Client* http = nullptr;
    net::Reactor* net = nullptr;
    audio::Backend* audio = nullptr;
    speech::SpeechService* speech = nullptr;
    EventSink* events = nullptr;
    Executor* ui = nullptr;
    Executor* jobs = nullptr;
    Executor* persistence = nullptr;
    Executor* io = nullptr;
    ShutdownPlan* shutdown = nullptr;
    CancelRouter* cancel = nullptr;
    ActivityState* activity = nullptr;
};

// A service asked for before anything provided it: "<Service> is not available yet." for the
// services of AppServices, "<type> is not available: no feature provides it." for service<T>().
class ServiceUnavailable : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// Built in Bootstrap; outlives every window.
// Affinity: UI actor; the services it hands out keep their own affinity.
class AppContext {
public:
    explicit AppContext(AppServices);
    // Releases the feature-provided services newest first, so a service can use older ones
    // until it goes.
    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    const AppIdentity& identity();
    const Paths& paths();
    Clock& clock();
    SettingsStore& settings();
    CredentialStore& credentials();
    Database& db();
    http::Client& http();
    net::Reactor& net();
    audio::Backend& audio();
    speech::SpeechService& speech();
    EventSink& events();
    Executor& ui();
    Executor& jobs();
    Executor& persistence();
    Executor& io();
    ScreenRegistry& screens();
    MenuRegistry& menus();
    SettingsPageRegistry& settingsPages();
    ShutdownPlan& shutdown();
    CancelRouter& cancel();
    ActivityState& activity();

    // feature-provided; throws ServiceUnavailable naming T
    template <class T> T& service() {
        if (T* found = find<T>()) {
            return *found;
        }
        throw_unavailable(typeid(T));
    }
    // Null when nothing provided T.
    template <class T> T* find() const {
        const auto it = provided_.find(std::type_index(typeid(T)));
        return it == provided_.end() ? nullptr : static_cast<T*>(it->second.get());
    }
    template <class T> std::shared_ptr<T> find_shared() const {
        const auto it = provided_.find(std::type_index(typeid(T)));
        return it == provided_.end() ? nullptr : std::static_pointer_cast<T>(it->second);
    }

    // Throws std::invalid_argument for a null service or a type provided before.
    template <class T> void provide(std::shared_ptr<T> value) {
        add_provided(typeid(T), std::shared_ptr<void>(std::move(value)));
    }

private:
    [[noreturn]] static void throw_unavailable(const std::type_info&);
    void add_provided(const std::type_info&, std::shared_ptr<void>);

    AppServices services_;
    std::unique_ptr<ScreenRegistry> screens_;
    std::unique_ptr<MenuRegistry> menus_;
    std::unique_ptr<SettingsPageRegistry> settings_pages_;
    std::map<std::type_index, std::shared_ptr<void>> provided_;
    std::vector<std::shared_ptr<void>> provision_order_;
};

// typeid(T).name() without the compiler's decoration: "fv::Foo", not "class fv::Foo" (MSVC) or
// "N2fv3FooE" (GCC).
std::string type_display_name(const std::type_info&);

} // namespace fv
