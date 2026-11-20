// AppContext: the services Bootstrap hands over, the registries it owns, and the services
// features provide to each other.

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include "appkit/app_context.h"
#include "base/testing/manual_executor.h"

namespace fv::test_services {
struct MeetingRuntime {
    int sessions = 0;
};
} // namespace fv::test_services

namespace {

// Records its destruction, to check the order services go in.
struct Tracked {
    Tracked(std::vector<std::string>& log, std::string name) : log(log), name(std::move(name)) {}
    ~Tracked() { log.push_back(name); }
    std::vector<std::string>& log;
    std::string name;
};
struct First : Tracked {
    using Tracked::Tracked;
};
struct Second : Tracked {
    using Tracked::Tracked;
};

} // namespace

TEST_CASE("AppContext::service<T>() throws naming T", "[widget]") {
    fv::AppContext context(fv::AppServices{});
    CHECK(context.find<fv::test_services::MeetingRuntime>() == nullptr);
    CHECK_THROWS_MATCHES(context.service<fv::test_services::MeetingRuntime>(),
                         fv::ServiceUnavailable,
                         Catch::Matchers::Message("fv::test_services::MeetingRuntime is not "
                                                  "available: no feature provides it."));

    auto runtime = std::make_shared<fv::test_services::MeetingRuntime>();
    context.provide(runtime);
    CHECK(&context.service<fv::test_services::MeetingRuntime>() == runtime.get());
    CHECK(context.find<fv::test_services::MeetingRuntime>() == runtime.get());
    CHECK_THROWS_MATCHES(
        context.provide(std::make_shared<fv::test_services::MeetingRuntime>()),
        std::invalid_argument,
        Catch::Matchers::Message("fv::test_services::MeetingRuntime is provided twice."));
    CHECK_THROWS_AS(context.provide(std::shared_ptr<First>()), std::invalid_argument);
}

TEST_CASE("AppContext accessors throw until Bootstrap has the service", "[widget]") {
    fv::ManualExecutor ui;
    fv::AppServices services;
    services.ui = &ui;
    services.events = &fv::null_event_sink();
    fv::AppContext context(services);
    CHECK(&context.ui() == &ui);
    CHECK(&context.events() == &fv::null_event_sink());
    CHECK_THROWS_MATCHES(context.settings(), fv::ServiceUnavailable,
                         Catch::Matchers::Message("SettingsStore is not available yet."));
    CHECK_THROWS_MATCHES(context.db(), fv::ServiceUnavailable,
                         Catch::Matchers::Message("Database is not available yet."));
    CHECK_THROWS_MATCHES(context.jobs(), fv::ServiceUnavailable,
                         Catch::Matchers::Message("The jobs executor is not available yet."));
    CHECK_THROWS_MATCHES(context.shutdown(), fv::ServiceUnavailable,
                         Catch::Matchers::Message("ShutdownPlan is not available yet."));
    CHECK_THROWS_AS(context.identity(), fv::ServiceUnavailable);
    CHECK_THROWS_AS(context.speech(), fv::ServiceUnavailable);

    // The registries belong to the context and always exist.
    CHECK(&context.screens() == &context.screens());
    CHECK(context.menus().items(fv::Menu::File).empty());
    CHECK(context.settingsPages().pages().empty());
}

TEST_CASE("AppContext releases provided services newest first", "[widget]") {
    std::vector<std::string> log;
    {
        fv::AppContext context(fv::AppServices{});
        context.provide(std::make_shared<First>(log, "first"));
        context.provide(std::make_shared<Second>(log, "second"));
    }
    CHECK(log == std::vector<std::string>{"second", "first"});
}

TEST_CASE("type_display_name drops the compiler's decoration") {
    CHECK(fv::type_display_name(typeid(fv::test_services::MeetingRuntime)) ==
          "fv::test_services::MeetingRuntime");
    CHECK(fv::type_display_name(typeid(int)) == "int");
}
