#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "automation/action_copy.h"
#include "automation/action_model.h"
#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "automation/hooks_model.h"
#include "automation/permissions.h"
#include "automation/signing.h"
#include "base/base64.h"
#include "golden_support.h"
#include "pyport/errors.h"
#include "settings/keys.h"

// Replays tests/golden/services.automation.events.json, services.automation.catalog.json (the
// builders fv_automation adds; the job builders are tests/dictation/test_event_payloads.cpp's),
// services.automation.permissions.json, services.automation.signing.json,
// services.automation.models.json, services.actions.model.json (the functions M5.3 adds) and
// ui_qt.widgets.action_copy.json. tools/parity/cases/*.py say how each case is built.

using namespace fv::automation;
using namespace fv::automation::test;
using fv::Json;
namespace golden = fv::golden;
namespace py = fv::py;

namespace {

constexpr std::string_view kEvents = "services.automation.events";
constexpr std::string_view kCatalog = "services.automation.catalog";
constexpr std::string_view kPermissions = "services.automation.permissions";
constexpr std::string_view kSigning = "services.automation.signing";
constexpr std::string_view kModels = "services.automation.models";
constexpr std::string_view kActions = "services.actions.model";
constexpr std::string_view kCopy = "ui_qt.widgets.action_copy";

std::optional<std::vector<std::string>> types_of(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return value.get<std::vector<std::string>>();
}

Json seqs_json(const std::vector<Event>& events) {
    Json list = Json::array();
    for (const Event& event : events) {
        list.push_back(event.seq);
    }
    return list;
}

// The data the case file's publish_lone builds, with each lone surrogate in the three bytes
// json.loads gives it.
Json lone(std::string_view where) {
    if (where == "value") {
        return Json{{"text", "hi \xED\xA0\xBD"}};
    }
    if (where == "key") {
        return Json{{"\xED\xB0\x80", "key"}};
    }
    return Json{{"list", Json::array({"ok", "\xED\xB0\x80"})}};
}

void check_event_shape(const Event& event) {
    CHECK(event.id.starts_with("evt_"));
    CHECK(event.id.size() == 28);
    CHECK(event.time.size() == 24);
    CHECK(event.time.ends_with("Z"));
}

Json run_script(std::int64_t buffer_size, std::int64_t seq_base, const Json& steps) {
    EventBus bus(static_cast<std::size_t>(std::max<std::int64_t>(0, buffer_size)), seq_base);
    Json out = Json::array();
    for (const Json& step : steps) {
        const std::string op = step.at(0).get<std::string>();
        if (op == "publish") {
            try {
                const Event event = bus.publish_event(step.at(1).get<std::string>(), step.at(2),
                                                      step.at(3).get<bool>());
                check_event_shape(event);
                const Json data = event.to_dict();
                out.push_back(Json::array({data["seq"], data["type"], data["v"], data["data"]}));
            } catch (const py::TypeError&) {
                out.push_back("TypeError");
            }
        } else if (op == "publish_lone") {
            try {
                bus.publish_event(step.at(1).get<std::string>(),
                                  lone(step.at(2).get<std::string>()));
                out.push_back("published");
            } catch (const py::TypeError&) {
                out.push_back("TypeError");
            }
        } else if (op == "publish_many") {
            Json last;
            for (int n = 0; n < step.at(2).get<int>(); ++n) {
                last = bus.publish_event(step.at(1).get<std::string>(), Json{{"n", n}}).seq;
            }
            out.push_back(last);
        } else if (op == "clear") {
            bus.clear();
            out.push_back(nullptr);
        } else if (op == "since") {
            const auto result = bus.since(step.at(1).get<std::int64_t>(), types_of(step.at(2)));
            out.push_back(Json{{"events", seqs_json(result.events)},
                               {"gap", result.gap ? Json(*result.gap) : Json(nullptr)}});
        } else if (op == "recent") {
            out.push_back(
                seqs_json(bus.recent(step.at(1).get<std::int64_t>(), types_of(step.at(2)))));
        } else if (op == "last_seq") {
            out.push_back(bus.last_seq());
        } else {
            FAIL("unknown step " << op);
        }
    }
    bus.close();
    return out;
}

std::vector<std::string> strings(const Json& value) {
    return value.is_null() ? std::vector<std::string>{} : value.get<std::vector<std::string>>();
}

hooks::Webhook webhook_from(const Json& spec) {
    return {text(spec.value("id", Json())), text(spec.value("name", Json())),
            text(spec.value("url", Json())), strings(spec.value("events", Json())),
            spec.value("enabled", true)};
}

hooks::Program program_from(const Json& spec) {
    return {text(spec.value("id", Json())),
            text(spec.value("name", Json())),
            text(spec.value("program", Json())),
            strings(spec.value("args", Json())),
            strings(spec.value("events", Json())),
            spec.value("enabled", true),
            spec.value("timeout_s", hooks::PROGRAM_TIMEOUT_DEFAULT_S)};
}

hooks::TextFilter filter_from(const Json& spec) {
    return {text(spec.value("id", Json())),
            text(spec.value("name", Json())),
            text(spec.value("kind", Json())),
            text(spec.value("stage", Json())),
            text(spec.value("url", Json())),
            text(spec.value("program", Json())),
            strings(spec.value("args", Json())),
            spec.value("timeout_ms", hooks::FILTER_TIMEOUT_DEFAULT_MS),
            spec.value("enabled", true)};
}

template <class Fn> Json message_of(Fn&& fn) {
    try {
        fn();
    } catch (const py::ValueError& error) {
        return error.what();
    }
    return nullptr;
}

template <class Fn> Json saved(Json settings, Fn&& fn) {
    try {
        fn(settings);
    } catch (const py::ValueError& error) {
        return Json::array({"ValueError", error.what()});
    }
    return settings;
}

template <class Items> Json dicts(const Items& items) {
    Json list = Json::array();
    for (const auto& item : items) {
        list.push_back(item.to_dict());
    }
    return list;
}

template <class Item> Json dict_or_null(const std::optional<Item>& item) {
    return item ? item->to_dict() : Json(nullptr);
}

Json labels_json(const action_copy::Labels& labels) {
    Json list = Json::array();
    for (const auto& [key, value] : labels) {
        list.push_back(Json::array({std::string(key), value}));
    }
    return list;
}

} // namespace

// ---- services.automation.events ----

TEST_CASE("golden services.automation.events run", "[golden]") {
    golden::for_each(kEvents, "run", [](const golden::Case& c) {
        const Json actual =
            run_script(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1), c.arg<Json>(2));
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

// ---- services.automation.catalog ----

TEST_CASE("golden services.automation.catalog known_types", "[golden]") {
    golden::for_each(kCatalog, "known_types", [](const golden::Case& c) {
        CHECK(catalog::known_types(c.arg<std::vector<std::string>>(0)) ==
              c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden services.automation.catalog action_ran", "[golden]") {
    golden::for_each(kCatalog, "action_ran", [](const golden::Case& c) {
        fv::dictation::DictationJob storage;
        const auto* job = job_from(c.arg<Json>(0), storage);
        const Action action = action_from(c.arg<Json>(1));
        const Json actual =
            catalog::action_ran(job, action, c.arg<bool>(2), text(c.arg<Json>(3)),
                                text(c.arg<Json>(4)), c.arg<std::optional<std::string>>(5));
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden services.automation.catalog channel_message", "[golden]") {
    golden::for_each(kCatalog, "channel_message", [](const golden::Case& c) {
        fv::dictation::DictationJob storage;
        const auto* job = job_from(c.arg<Json>(0), storage);
        const Json actual = catalog::channel_message(job, action_from(c.arg<Json>(1)),
                                                     text(c.arg<Json>(2)), c.arg<Json>(3));
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden services.automation.catalog route_decided", "[golden]") {
    golden::for_each(kCatalog, "route_decided", [](const golden::Case& c) {
        fv::dictation::DictationJob storage;
        const auto* job = job_from(c.arg<Json>(0), storage);
        const Json actual = catalog::route_decided(job, text(c.arg<Json>(1)),
                                                   c.arg<std::optional<double>>(2), c.arg<bool>(3));
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden services.automation.catalog small builders", "[golden]") {
    golden::for_each(kCatalog, "filter_failed", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            catalog::filter_failed(text(c.arg<Json>(0)), text(c.arg<Json>(1)),
                                                   text(c.arg<Json>(2)), text(c.arg<Json>(3)))));
    });
    golden::for_each(kCatalog, "meeting_started", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            catalog::meeting_started(text(c.arg<Json>(0)), text(c.arg<Json>(1)))));
    });
    golden::for_each(kCatalog, "meeting_ended", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            catalog::meeting_ended(text(c.arg<Json>(0)), text(c.arg<Json>(1)))));
    });
    golden::for_each(kCatalog, "meeting_segments", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            catalog::meeting_segments(text(c.arg<Json>(0)), c.arg<Json>(1))));
    });
    golden::for_each(kCatalog, "meeting_voice_command", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), catalog::meeting_voice_command(
                                                  text(c.arg<Json>(0)), text(c.arg<Json>(1)))));
    });
    golden::for_each(kCatalog, "events_gap", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            catalog::events_gap(c.arg<std::int64_t>(0), text(c.arg<Json>(1)))));
    });
}

TEST_CASE("golden services.automation.catalog tables", "[golden]") {
    golden::for_each(kCatalog, "event_types", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const auto& [type, description] : catalog::EVENT_TYPES()) {
            actual.push_back(Json::array({std::string(type), std::string(description)}));
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kCatalog, "public_types", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const auto type : catalog::PUBLIC_TYPES()) {
            actual.push_back(std::string(type));
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kCatalog, "constants", [](const golden::Case& c) {
        const auto list = [](const auto& values) {
            std::vector<std::string> items;
            for (const auto value : values) {
                items.emplace_back(value);
            }
            return items;
        };
        auto user_sources = list(catalog::USER_SOURCES);
        auto failures = list(catalog::FAILURES);
        std::ranges::sort(user_sources);
        std::ranges::sort(failures);
        const Json actual = {
            {"SOURCES", list(catalog::SOURCES)},
            {"USER_SOURCES", user_sources},
            {"FAILURES", failures},
            {"ORIGINS", {catalog::ORIGIN_DICTATED, catalog::ORIGIN_PROGRAM}},
            {"FILE_MODE", catalog::FILE_MODE},
            {"GAPS", {catalog::GAP_EVICTED, catalog::GAP_RESTARTED, catalog::GAP_SLOW}},
        };
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

// ---- services.automation.permissions ----

TEST_CASE("golden services.automation.permissions", "[golden]") {
    golden::for_each(kPermissions, "granted", [](const golden::Case& c) {
        CHECK(permissions::granted(c.arg<Json>(0), c.arg<std::string>(1)) == c.result<bool>());
    });
    golden::for_each(kPermissions, "require", [](const golden::Case& c) {
        Json actual;
        try {
            permissions::require(c.arg<Json>(0), c.arg<std::string>(1));
        } catch (const permissions::PermissionDenied& denied) {
            actual = Json::array({denied.permission(), denied.message()});
        }
        CHECK(golden::equal(rebranded(c.result<Json>()), actual));
    });
    golden::for_each(kPermissions, "describe", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), permissions::describe(c.arg<Json>(0))));
    });
    golden::for_each(kPermissions, "tables", [](const golden::Case& c) {
        Json all = Json::array();
        Json names = Json::object();
        for (const auto key : permissions::ALL) {
            all.push_back(std::string(key));
            names[std::string(key)] =
                std::string(permissions::public_name(*permissions::from_key(key)));
        }
        CHECK(golden::equal(c.result<Json>(), Json{{"ALL", all}, {"PUBLIC_NAMES", names}}));
    });
}

// ---- services.automation.signing ----

TEST_CASE("golden services.automation.signing", "[golden]") {
    golden::for_each(kSigning, "signature", [](const golden::Case& c) {
        const auto call = [&] {
            return signing::signature(c.arg<std::string>(0), c.arg<std::string>(1),
                                      c.arg<std::int64_t>(2), c.arg<std::string>(3));
        };
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(call(), py::ValueError);
        } else {
            CHECK(call() == c.result<std::string>());
        }
    });
    golden::for_each(kSigning, "headers", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const auto& [name, value] :
             signing::headers(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<std::string>(2),
                              c.arg<std::int64_t>(3))) {
            actual.push_back(Json::array({name, value}));
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kSigning, "verify", [](const golden::Case& c) {
        signing::Received received;
        const auto environment = c.arg<Json>(1);
        for (const auto& [name, value] : environment.items()) {
            received.emplace_back(name, value.get<std::string>());
        }
        CHECK(signing::verify(c.arg<std::string>(0), received, c.arg<std::string>(2),
                              c.arg<std::int64_t>(3), c.arg<std::int64_t>(4)) == c.result<bool>());
    });
    // _key: the bytes behind "whsec_", strict base64, not empty.
    golden::for_each(kSigning, "key_bytes", [](const golden::Case& c) {
        const std::string secret = c.arg<std::string>(0);
        Json actual = "ValueError";
        if (secret.starts_with(signing::PREFIX)) {
            const auto key = fv::b64decode(std::string_view(secret).substr(6));
            if (key && !key->empty()) {
                actual = Json::array();
                for (const std::byte b : *key) {
                    actual.push_back(std::to_integer<int>(b));
                }
            }
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

// ---- services.automation.models ----

TEST_CASE("golden services.automation.models loaders", "[golden]") {
    golden::for_each(kModels, "load_webhooks", [](const golden::Case& c) {
        const Json settings = {{"automation_webhooks", c.arg<Json>(0)}};
        CHECK(golden::equal(c.result<Json>(), dicts(hooks::load_webhooks(settings))));
    });
    golden::for_each(kModels, "load_programs", [](const golden::Case& c) {
        const Json settings = {{"automation_programs", c.arg<Json>(0)}};
        CHECK(golden::equal(c.result<Json>(), dicts(hooks::load_programs(settings))));
    });
    golden::for_each(kModels, "load_filters", [](const golden::Case& c) {
        const Json settings = {{"automation_filters", c.arg<Json>(0)}};
        CHECK(golden::equal(c.result<Json>(), dicts(hooks::load_filters(settings))));
    });
    golden::for_each(kModels, "find_webhook", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), dict_or_null(hooks::find_webhook(
                                                  c.arg<Json>(0), c.arg<std::string>(1)))));
    });
    golden::for_each(kModels, "find_program", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), dict_or_null(hooks::find_program(
                                                  c.arg<Json>(0), c.arg<std::string>(1)))));
    });
    golden::for_each(kModels, "find_filter", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), dict_or_null(hooks::find_filter(
                                                  c.arg<Json>(0), c.arg<std::string>(1)))));
    });
    golden::for_each(kModels, "secret_name", [](const golden::Case& c) {
        CHECK(golden::equal_rebranded(c.result<std::string>(),
                                      hooks::webhook_secret_name(c.arg<std::string>(0))));
    });
}

TEST_CASE("golden services.automation.models urls", "[golden]") {
    golden::for_each(kModels, "validate_url", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            message_of([&] { hooks::validate_url(c.arg<std::string>(0)); })));
    });
    golden::for_each(kModels, "url_is_private", [](const golden::Case& c) {
        CHECK(hooks::url_is_private(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("golden services.automation.models webhooks", "[golden]") {
    golden::for_each(kModels, "validate_webhook", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), message_of([&] {
                                hooks::validate_webhook(webhook_from(c.arg<Json>(0)),
                                                        c.arg<Json>(1));
                            })));
    });
    golden::for_each(kModels, "save_webhook", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::save_webhook(settings, webhook_from(c.arg<Json>(1)));
                            })));
    });
    golden::for_each(kModels, "delete_webhook", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::delete_webhook(settings, c.arg<std::string>(1));
                            })));
    });
}

TEST_CASE("golden services.automation.models programs and filters", "[golden]") {
    // Program paths are the vectors' platform's (os.path.isabs).
    if (!same_platform(kModels)) {
        SKIP("program and filter vectors were made on another platform");
    }
    golden::for_each(kModels, "validate_program", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), message_of([&] {
                                hooks::validate_program(program_from(c.arg<Json>(0)),
                                                        c.arg<Json>(1));
                            })));
    });
    golden::for_each(kModels, "validate_filter", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), message_of([&] {
                                hooks::validate_filter(filter_from(c.arg<Json>(0)), c.arg<Json>(1));
                            })));
    });
    golden::for_each(kModels, "save_program", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::save_program(settings, program_from(c.arg<Json>(1)));
                            })));
    });
    golden::for_each(kModels, "save_filter", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::save_filter(settings, filter_from(c.arg<Json>(1)));
                            })));
    });
    golden::for_each(kModels, "delete_program", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::delete_program(settings, c.arg<std::string>(1));
                            })));
    });
    golden::for_each(kModels, "delete_filter", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                hooks::delete_filter(settings, c.arg<std::string>(1));
                            })));
    });
}

// ---- services.actions.model ----

TEST_CASE("golden services.actions.model links and views", "[golden]") {
    golden::for_each(kActions, "link_scheme", [](const golden::Case& c) {
        CHECK(link_scheme(c.arg<std::string>(0)) == c.result<std::string>());
    });
    golden::for_each(kActions, "expand_link", [](const golden::Case& c) {
        Json actual;
        try {
            actual = expand_link(c.arg<std::string>(0), text(c.arg<Json>(1)));
        } catch (const py::ValueError& error) {
            actual = Json::array({"ValueError", error.what()});
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kActions, "public_dict", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), action_from(c.arg<Json>(0)).public_dict()));
    });
    golden::for_each(kActions, "param", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            action_from(c.arg<Json>(0)).param(c.arg<std::string>(1))));
    });
    golden::for_each(kActions, "shaping_ids", [](const golden::Case& c) {
        const Action action = action_from(c.arg<Json>(0));
        CHECK(golden::equal(c.result<Json>(),
                            Json::array({action.profile_id(), action.transform_id()})));
    });
    golden::for_each(kActions, "secret_name", [](const golden::Case& c) {
        CHECK(golden::equal_rebranded(c.result<std::string>(),
                                      action_secret_name(c.arg<std::string>(0))));
    });
}

// A19-01: the automation loader itself (backup's own reader replays the same vectors). The
// vectors were made on Windows, so shortcuts are read in the Windows vocabulary on any platform.
TEST_CASE("golden services.actions.model load_actions", "[golden]") {
    golden::for_each(kActions, "load_actions", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const Action& action :
             load_actions(Json{{"actions", c.arg<Json>(0)}}, fv::hotkeys::Vocabulary::Windows)) {
            actual.push_back(action.to_dict());
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden services.actions.model presets and tables", "[golden]") {
    golden::for_each(kActions, "presets", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const Preset& preset : PRESETS()) {
            actual.push_back(Json{{"id", preset.id},
                                  {"name", preset.name},
                                  {"kind", preset.kind},
                                  {"params", preset.params},
                                  {"description", preset.description},
                                  {"icon", preset.icon},
                                  {"shaping", preset.shaping},
                                  {"note", preset.note}});
        }
        CHECK(golden::equal(rebranded(c.result<Json>()), actual));
    });
    golden::for_each(kActions, "tables", [](const golden::Case& c) {
        const auto list = [](const auto& values) {
            Json items = Json::array();
            for (const auto value : values) {
                items.push_back(std::string(value));
            }
            return items;
        };
        Json names = Json::object();
        for (const auto& [id, name] : AGENT_NAMES()) {
            names[std::string(id)] = std::string(name);
        }
        Json params = Json::object();
        for (const auto kind : ActionKind::ALL) {
            params[std::string(kind)] = PARAMS(kind);
        }
        std::vector<std::string> refused(REFUSED_SCHEMES.begin(), REFUSED_SCHEMES.end());
        std::ranges::sort(refused);
        const Json actual = {
            {"ActionKind", list(ActionKind::ALL)},
            {"Output", list(Output::ALL)},
            {"AgentMode", list(AgentMode::ALL)},
            {"AgentSurface", list(AgentSurface::ALL)},
            {"AGENTS", list(AGENTS)},
            {"AGENT_NAMES", names},
            {"PARAMS", params},
            {"REFUSED_SCHEMES", refused},
            {"limits",
             {MAX_ACTIONS, NAME_MAX_CHARS, DESCRIPTION_MAX_CHARS, MAX_SPOKEN_NAMES,
              SPOKEN_NAME_MAX_CHARS, TEMPLATE_MAX_CHARS}},
        };
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden services.actions.model validation and saving", "[golden]") {
    // Shortcut names and paths are the vectors' platform's.
    if (!same_platform(kActions)) {
        SKIP("action vectors were made on another platform");
    }
    golden::for_each(kActions, "validate_action", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), message_of([&] {
                                validate_action(action_from(c.arg<Json>(0)), c.arg<Json>(1));
                            })));
    });
    golden::for_each(kActions, "save_action", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                save_action(settings, action_from(c.arg<Json>(1)));
                            })));
    });
    golden::for_each(kActions, "delete_action", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), saved(c.arg<Json>(0), [&](Json& settings) {
                                delete_action(settings, c.arg<std::string>(1));
                            })));
    });
    golden::for_each(kActions, "find_action", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            dict_or_null(find_action(c.arg<Json>(0), c.arg<std::string>(1)))));
    });
    golden::for_each(kActions, "hotkey_conflict", [](const golden::Case& c) {
        CHECK(action_hotkey_conflict(c.arg<std::string>(0), c.arg<Json>(1),
                                     c.arg<std::string>(2)) == c.result<std::string>());
    });
}

// ---- ui_qt.widgets.action_copy ----

TEST_CASE("golden ui_qt.widgets.action_copy summary", "[golden]") {
    const bool here = same_platform(kCopy);
    golden::for_each(kCopy, "summary", [&](const golden::Case& c) {
        const Action action = action_from(c.arg<Json>(0));
        // A program's file name is os.path.basename's on the vectors' platform.
        if (action.kind == ActionKind::PROGRAM && !here) {
            return;
        }
        CHECK(golden::equal_rebranded(c.result<std::string>(), action_copy::summary(action)));
    });
}

TEST_CASE("golden ui_qt.widgets.action_copy text helpers", "[golden]") {
    golden::for_each(kCopy, "link_destination", [](const golden::Case& c) {
        CHECK(action_copy::link_destination(c.arg<std::string>(0)) == c.result<std::string>());
    });
    golden::for_each(kCopy, "shortened", [](const golden::Case& c) {
        CHECK(action_copy::shortened(text(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(kCopy, "try_outcome", [](const golden::Case& c) {
        const Json spec = c.arg<Json>(0);
        const ActionResult result{spec.value("ok", false), text(spec.value("message", Json())),
                                  text(spec.value("output", Json())), spec.value("stopped", false)};
        CHECK(action_copy::try_outcome(result) == c.result<std::string>());
    });
    golden::for_each(kCopy, "spoken_hint", [](const golden::Case& c) {
        CHECK(action_copy::spoken_hint(action_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(kCopy, "parse_spoken_names", [](const golden::Case& c) {
        CHECK(action_copy::parse_spoken_names(text(c.arg<Json>(0))) ==
              c.result<std::vector<std::string>>());
    });
    golden::for_each(kCopy, "parse_arguments", [](const golden::Case& c) {
        CHECK(action_copy::parse_arguments(text(c.arg<Json>(0))) ==
              c.result<std::vector<std::string>>());
    });
    golden::for_each(kCopy, "agent_name", [](const golden::Case& c) {
        CHECK(action_copy::agent_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.widgets.action_copy tables", "[golden]") {
    golden::for_each(kCopy, "tables", [](const golden::Case& c) {
        const Json actual = {
            {"SAMPLE_WORDS", action_copy::SAMPLE_WORDS},
            {"TRY_OUTPUT_LINES", action_copy::TRY_OUTPUT_LINES},
            {"TRY_OUTPUT_CHARS", action_copy::TRY_OUTPUT_CHARS},
            {"KIND_ICONS", labels_json(action_copy::KIND_ICONS())},
            {"KIND_BLURBS", labels_json(action_copy::KIND_BLURBS())},
            {"OUTPUT_LABELS", labels_json(action_copy::OUTPUT_LABELS())},
            {"MODE_LABELS", labels_json(action_copy::MODE_LABELS())},
            {"SURFACE_LABELS", labels_json(action_copy::SURFACE_LABELS())},
            {"SURFACE_HINTS", labels_json(action_copy::SURFACE_HINTS())},
            {"NO_APP_HINT", action_copy::no_app_hint("OpenCode")},
        };
        CHECK(golden::equal(rebranded(c.result<Json>()), actual));
    });
}
