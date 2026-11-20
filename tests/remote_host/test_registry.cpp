// DeviceRegistry (services/remote_asr/host.py:72-203): test_remote_registry.py's last_seen race,
// test_remote_rename.py's rename, then the registry's rules from host.py, and the registry kept
// in remote_host_devices (host_settings.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "pyport/errors.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/host_settings.h"
#include "remote_host/registry.h"
#include "remote_host/testing/memory_device_store.h"
#include "settings/testing/memory_store.h"

using fv::FakeClock;
using fv::Json;
using fv::remote_host::DeviceRegistry;
using fv::remote_host::testing::MemoryDeviceStore;
using namespace std::chrono_literals;

namespace {

// Set on the thread that changes the devices while a last_seen write is paused.
thread_local bool tl_mutation = false;

std::set<std::string> stored_ids(const MemoryDeviceStore& store) {
    std::set<std::string> ids;
    for (const auto& entry : store.devices()) {
        ids.insert(entry.at("id").get<std::string>());
    }
    return ids;
}

Json entry(std::string id, std::string digest, Json extra = Json::object()) {
    Json out = Json::object();
    out["id"] = std::move(id);
    out["name"] = "laptop";
    out["token_sha256"] = std::move(digest);
    out["paired_at"] = "2026-01-01T00:00:00+00:00";
    out["last_seen"] = "2026-01-01T00:00:00+00:00";
    out["via"] = "code";
    for (const auto& item : extra.items()) {
        out[item.key()] = item.value();
    }
    return out;
}

} // namespace

TEST_CASE("last seen write serializes with device changes", "[test_remote_registry]") {
    // from tests/test_remote_registry.py::test_last_seen_write_serializes_with_device_changes
    // (the service's shared registry is M9.12's; here both threads use the one registry)
    const std::string mutation = GENERATE("add", "remove");
    MemoryDeviceStore store;
    FakeClock clock;
    std::mutex mutex;
    std::condition_variable changed;
    bool snapshot_ready = false;
    bool release_snapshot = false;
    bool mutation_loaded = false;
    std::atomic<bool> pause_next_save{false};

    DeviceRegistry registry(
        [&] {
            if (tl_mutation) {
                std::lock_guard lock(mutex);
                mutation_loaded = true;
                changed.notify_all();
            }
            return store.load();
        },
        [&](const Json& devices) {
            if (pause_next_save.exchange(false)) {
                std::unique_lock lock(mutex);
                snapshot_ready = true;
                changed.notify_all();
                // Catch2's assertions are for the test's own thread: this one throws, and the
                // authenticate future hands the failure on.
                if (!changed.wait_for(lock, 5s, [&] { return release_snapshot; })) {
                    throw std::runtime_error("the last_seen write was not released");
                }
            }
            store.save(devices);
        },
        clock);
    const auto [device, token] = registry.add("Fixture laptop");

    pause_next_save = true;
    auto touched = std::async(std::launch::async, [&] { return registry.authenticate(token); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 5s, [&] { return snapshot_ready; }));
    }
    auto change = std::async(std::launch::async, [&]() -> std::string {
        // std::async may run later tasks on this pool thread: the flag ends with the task.
        struct Flag {
            Flag() { tl_mutation = true; }
            ~Flag() { tl_mutation = false; }
            Flag(const Flag&) = delete;
            Flag& operator=(const Flag&) = delete;
        } flag;
        if (mutation == "add") {
            return registry.add("Fixture desktop").first.id;
        }
        return registry.remove(device.id) ? "removed" : "kept";
    });
    bool contended = false;
    {
        // The device change waits for the registry's lock, held across the paused write, so it
        // reads nothing until that write is done.
        std::unique_lock lock(mutex);
        contended = !changed.wait_for(lock, 300ms, [&] { return mutation_loaded; });
        release_snapshot = true;
    }
    changed.notify_all();
    const auto authenticated = touched.get();
    REQUIRE(authenticated);
    CHECK(authenticated->id == device.id);
    const std::string result = change.get();

    if (mutation == "add") {
        CHECK(stored_ids(store) == std::set<std::string>{device.id, result});
    } else {
        CHECK(result == "removed");
        CHECK(stored_ids(store).empty());
    }
    CHECK(contended);
}

TEST_CASE("a paired computer can go by another name and back", "[test_remote_rename]") {
    // from tests/test_remote_rename.py::test_a_paired_computer_can_go_by_another_name_and_back
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const auto [device, token] = registry.add("DESKTOP-LAPTOP");
    const auto renamed = registry.rename(device.id, "  Kitchen laptop\n");
    REQUIRE(renamed);
    CHECK(renamed->name == "Kitchen laptop");
    CHECK(renamed->paired_name == "DESKTOP-LAPTOP");
    CHECK(registry.list() == std::vector{*renamed});
    // Renamed again, it still knows the name it paired with.
    CHECK(registry.rename(device.id, "Studio")->paired_name == "DESKTOP-LAPTOP");
    const auto back = registry.rename(device.id, "");
    REQUIRE(back);
    CHECK(back->name == "DESKTOP-LAPTOP");
    CHECK(back->paired_name.empty());
    CHECK_FALSE(store.devices()[0].contains("paired_name"));
    registry.rename(device.id, "Studio");
    CHECK(registry.rename(device.id, "DESKTOP-LAPTOP")->paired_name.empty());
    CHECK_FALSE(store.devices()[0].contains("paired_name"));
    CHECK(registry.rename(device.id, std::string(200, 'x'))->name ==
          std::string(fv::remote::MAX_NAME, 'x'));
    CHECK_FALSE(registry.rename("not-paired", "Studio"));
    const auto authenticated = registry.authenticate(token);
    REQUIRE(authenticated);
    CHECK(authenticated->id == device.id);
}

TEST_CASE("the registry stores the token's digest, never the token", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock(FakeClock::default_utc() + 3h + 4min + 5s + 600ms);
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const auto [device, token] = registry.add("  laptop\t", "approval");

    CHECK(token.size() == 43); // token_urlsafe(32)
    CHECK(device.id.size() == 32);
    CHECK(device.name == "laptop");
    CHECK(device.via == "approval");
    CHECK(device.paired_at == "2026-01-01T03:04:05+00:00");
    CHECK(device.last_seen == device.paired_at);
    const Json stored = store.devices();
    REQUIRE(stored.size() == 1);
    std::vector<std::string> keys;
    for (const auto& item : stored[0].items()) {
        keys.push_back(item.key());
    }
    CHECK(keys ==
          std::vector<std::string>{"id", "name", "token_sha256", "paired_at", "last_seen", "via"});
    CHECK(stored[0]["token_sha256"] == fv::remote::token_digest(token));
    CHECK(stored.dump().find(token) == std::string::npos);
    CHECK(registry.add("").first.name == "Unnamed computer");
    CHECK(registry.add("\x01\x02").first.name == "Unnamed computer");
    CHECK(registry.list().size() == 3);
}

TEST_CASE("only entries with a string id and digest count", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const std::string digest = fv::remote::token_digest("token");

    store.set_devices(Json::object());
    CHECK(registry.list().empty());
    const Json kept_d = Json{{"id", "d"}, {"token_sha256", digest}, {"name", 12}};
    store.set_devices(Json::array({"text", 5, nullptr, entry("a", digest),
                                   Json{{"id", 7}, {"token_sha256", digest}}, Json{{"id", "b"}},
                                   Json{{"id", "c"}, {"token_sha256", nullptr}}, kept_d}));
    const auto devices = registry.list();
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].id == "a");
    CHECK(devices[1].id == "d");
    // A field a hand edit made something other than text reads as "".
    CHECK(devices[1].name.empty());
    // A write saves the sanitized list.
    CHECK(registry.remove("a"));
    CHECK(store.devices() == Json::array({kept_d}));
}

TEST_CASE("record owners are sanitized on every read", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const std::string digest = fv::remote::token_digest("token");
    const Json owners = Json::array({"old_1", "old_1", "bad id", 5, "", std::string(65, 'x'),
                                     std::string(64, 'y'), "\xc3\xa9t\xc3\xa9", "ok-2"});
    store.set_devices(
        Json::array({entry("a", digest, {{"record_owner_ids", owners}}),
                     entry("b", digest, {{"record_owner_ids", "old_1"}}), entry("c", digest)}));

    const Json entries = registry.entries();
    CHECK(entries[0]["record_owner_ids"] == Json::array({"old_1", std::string(64, 'y'), "ok-2"}));
    CHECK(entries[1]["record_owner_ids"] == Json::array());
    CHECK_FALSE(entries[2].contains("record_owner_ids"));
    CHECK(registry.list()[0].record_owner_ids ==
          std::vector<std::string>{"old_1", std::string(64, 'y'), "ok-2"});
}

TEST_CASE("records of an old pairing go to one paired computer", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const auto laptop = registry.add("laptop").first;
    const auto desk = registry.add("desk").first;

    const auto message = [&](std::string_view id, std::string_view owner) {
        try {
            registry.attach_record_owner(id, owner);
        } catch (const fv::py::ValueError& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    CHECK(message(laptop.id, "bad owner") == "Invalid stored record owner.");
    CHECK(message(laptop.id, "") == "Invalid stored record owner.");
    CHECK(message("gone", "old_1") ==
          "Pair the receiving computer again before recovering its records.");
    CHECK(message(laptop.id, "old_1").empty());
    CHECK(registry.list()[0].record_owner_ids == std::vector<std::string>{"old_1"});
    CHECK(message(laptop.id, "old_2").empty());
    CHECK(registry.list()[0].record_owner_ids == std::vector<std::string>{"old_1", "old_2"});
    CHECK(message(desk.id, "old_1") == "These records already belong to a paired computer.");
    CHECK(message(desk.id, laptop.id) == "These records already belong to a paired computer.");
    std::vector<std::string> keys;
    const auto devices = store.devices();
    for (const auto& item : devices.at(0).items()) {
        keys.push_back(item.key());
    }
    REQUIRE_FALSE(keys.empty());
    CHECK(keys.back() == "record_owner_ids");
}

TEST_CASE("authenticate writes last_seen at most once a minute per device", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const std::string token = registry.add("laptop").second;
    const auto saves = store.saves();

    clock.advance(10s);
    const auto first = registry.authenticate(token);
    REQUIRE(first);
    // The entry comes back as it was read, before the write.
    CHECK(first->last_seen == "2026-01-01T00:00:00+00:00");
    CHECK(store.saves() == saves + 1);
    CHECK(store.devices()[0]["last_seen"] == "2026-01-01T00:00:10+00:00");

    clock.advance(59s);
    CHECK(registry.authenticate(token));
    CHECK(store.saves() == saves + 1);
    clock.advance(1s);
    CHECK(registry.authenticate(token)->last_seen == "2026-01-01T00:00:10+00:00");
    CHECK(store.saves() == saves + 2);
    CHECK(store.devices()[0]["last_seen"] == "2026-01-01T00:01:10+00:00");

    // A wall-clock jump does not move the throttle; steady time does.
    clock.set_utc(FakeClock::default_utc() + 24h);
    CHECK(registry.authenticate(token));
    CHECK(store.saves() == saves + 2);
}

TEST_CASE("authenticate refuses empty and unknown tokens", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    registry.add("laptop");
    const auto saves = store.saves();
    CHECK_FALSE(registry.authenticate(""));
    CHECK_FALSE(registry.authenticate("not a token"));
    CHECK(store.saves() == saves);
}

TEST_CASE("authenticate compares every entry and the last match wins", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const std::string digest = fv::remote::token_digest("shared");
    store.set_devices(
        Json::array({entry("first", digest), entry("other", "ab"), entry("second", digest)}));
    const auto device = registry.authenticate("shared");
    REQUIRE(device);
    CHECK(device->id == "second");

    // hmac.compare_digest refuses non-ASCII text.
    store.set_devices(Json::array({entry("odd", "\xc3\xa9")}));
    CHECK_THROWS_AS(registry.authenticate("shared"), fv::py::TypeError);
}

TEST_CASE("remove drops the device and says whether it was paired", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const auto [laptop, token] = registry.add("laptop");
    registry.add("desk");
    const auto saves = store.saves();
    CHECK_FALSE(registry.remove("gone"));
    CHECK(store.saves() == saves);
    CHECK(registry.remove(laptop.id));
    CHECK(registry.list().size() == 1);
    CHECK_FALSE(registry.authenticate(token));
}

TEST_CASE("rename reads a stored paired name as Python's or does", "[registry]") {
    MemoryDeviceStore store;
    FakeClock clock;
    DeviceRegistry registry(store.loader(), store.saver(), clock);
    const std::string digest = fv::remote::token_digest("t");
    // entry.pop("paired_name", None) or entry.get("name"): a falsy paired name falls back to the
    // name, and a number is cleaned through str().
    store.set_devices(Json::array({entry("a", digest, {{"paired_name", ""}}),
                                   entry("b", digest, {{"paired_name", 1234}}),
                                   entry("c", digest, {{"name", nullptr}})}));
    CHECK(registry.rename("a", "Studio")->paired_name == "laptop");
    CHECK(registry.rename("b", "Studio")->paired_name == "1234");
    const auto unnamed = registry.rename("c", "");
    REQUIRE(unnamed);
    CHECK(unnamed->name == "Unnamed computer");
    CHECK(unnamed->paired_name.empty());
}

TEST_CASE("now_iso and clean_device_name", "[registry]") {
    FakeClock clock(FakeClock::default_utc() + 999ms);
    CHECK(fv::remote_host::now_iso(clock) == "2026-01-01T00:00:00+00:00");
    CHECK(fv::remote_host::clean_device_name(" desk ") == "desk");
    CHECK(fv::remote_host::clean_device_name("") == "Unnamed computer");
    CHECK(fv::remote_host::clean_device_name(Json(nullptr)) == "Unnamed computer");
    CHECK(fv::remote_host::valid_record_owner("A-z_09"));
    CHECK_FALSE(fv::remote_host::valid_record_owner("a b"));
}

TEST_CASE("the host's registry lives in remote_host_devices", "[host_settings]") {
    fv::MemorySettingsStore settings;
    FakeClock clock;
    auto registry = fv::remote_host::settings_device_registry(settings, clock);
    const auto [device, token] = registry->add("laptop");
    const Json saved = settings.contents();
    REQUIRE(saved.contains("remote_host_devices"));
    REQUIRE(saved["remote_host_devices"].size() == 1);
    CHECK(saved["remote_host_devices"][0]["id"] == device.id);
    CHECK(saved["remote_host_devices"][0]["token_sha256"] == fv::remote::token_digest(token));
    CHECK(registry->authenticate(token)->id == device.id);
}

TEST_CASE("the host reads its settings from the store", "[host_settings]") {
    fv::MemorySettingsStore defaults;
    CHECK_FALSE(fv::remote_host::host_enabled(defaults));
    CHECK(fv::remote_host::host_port(defaults) == fv::remote::DEFAULT_PORT);
    CHECK(fv::remote_host::host_tailscale_trust(defaults));
    CHECK_FALSE(fv::remote_host::host_model_management(defaults));
    CHECK_FALSE(fv::remote_host::host_keeps_records(defaults));
    CHECK_FALSE(fv::remote_host::host_manages_mcp(defaults));

    fv::MemorySettingsStore saved(Json{{"remote_host_enabled", true},
                                       {"remote_host_port", 50000},
                                       {"remote_host_tailscale_trust", false},
                                       {"remote_host_model_management", true},
                                       {"remote_host_keep_records", true},
                                       {"remote_host_manage_mcp", true}});
    CHECK(fv::remote_host::host_enabled(saved));
    CHECK(fv::remote_host::host_port(saved) == 50000);
    CHECK_FALSE(fv::remote_host::host_tailscale_trust(saved));
    CHECK(fv::remote_host::host_model_management(saved));
    CHECK(fv::remote_host::host_keeps_records(saved));
    CHECK(fv::remote_host::host_manages_mcp(saved));
}
