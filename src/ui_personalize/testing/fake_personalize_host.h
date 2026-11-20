#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>

#include "base/json.h"
#include "pyport/errors.h"
#include "ui_personalize/personalize_host.h"

namespace fv {

// A PersonalizeHost that keeps the saved settings in memory and records every call, in order, as
// "name(arguments)" in `calls`. persist and persistMany refuse while `loading` is set, as the
// Settings window does; mutate runs the change over a copy of the saved settings and keeps the
// result only when it did not throw.
// Affinity: GUI thread.
class FakePersonalizeHost final : public PersonalizeHost {
public:
    // What every call has been, oldest first.
    std::vector<std::string> calls;
    // The saved settings the page reads and persist and mutate write.
    Json saved = Json::object();
    bool loading = false;
    // Make persist, persistMany and mutate fail: persist and persistMany answer false, mutate
    // false with `mutateError`.
    bool failSaves = false;
    QString mutateError;
    // The text of the last showMessage.
    QString message;
    // What setStandardHotkey answers ("" is saved), and the shortcuts currentHotkeys answers.
    QString standardHotkeyAnswer;
    Json hotkeys = Json::object();
    // The controls control(name) finds.
    std::map<std::string, QWidget*, std::less<>> controls;

    Json settings() override {
        calls.push_back("settings()");
        return saved;
    }

    bool persist(std::string_view key, const Json& value) override {
        calls.push_back("persist(" + std::string(key) + ", " + value.dump() + ")");
        if (loading || failSaves) {
            return false;
        }
        saved[std::string(key)] = value;
        return true;
    }

    bool persistMany(const Json& updates, std::vector<std::string> drops = {}) override {
        std::string dropped;
        for (const std::string& key : drops) {
            dropped += (dropped.empty() ? "" : ",") + key;
        }
        calls.push_back("persistMany(" + updates.dump() + ", [" + dropped + "])");
        if (loading || failSaves) {
            return false;
        }
        for (const auto& [key, value] : updates.items()) {
            saved[key] = value;
        }
        for (const std::string& key : drops) {
            saved.erase(key);
        }
        return true;
    }

    bool mutate(const std::function<void(Json&)>& change, QString* error = nullptr) override {
        calls.push_back("mutate()");
        if (error != nullptr) {
            error->clear();
        }
        if (failSaves) {
            if (error != nullptr) {
                *error = mutateError;
            }
            return false;
        }
        Json copy = saved;
        try {
            change(copy);
        } catch (const py::ValueError& refused) {
            if (error != nullptr) {
                *error = QString::fromUtf8(refused.what());
            }
            return false;
        }
        saved = std::move(copy);
        return true;
    }

    bool isLoading() const override { return loading; }

    void notifyChanged(const QString& kind) override {
        calls.push_back("notifyChanged(" + kind.toStdString() + ")");
    }

    void selectDestination(const QString& key) override {
        calls.push_back("selectDestination(" + key.toStdString() + ")");
    }

    void reveal(QWidget* target) override {
        calls.push_back(target != nullptr ? "reveal(" + target->objectName().toStdString() + ")"
                                          : std::string("reveal(null)"));
    }

    void showMessage(const QString& text) override {
        message = text;
        calls.push_back("showMessage(" + text.toStdString() + ")");
    }

    QString setStandardHotkey(const QString& action, const QString& hotkey) override {
        calls.push_back("setStandardHotkey(" + action.toStdString() + ", " + hotkey.toStdString() +
                        ")");
        return standardHotkeyAnswer;
    }

    void setHotkeyCaptureSuspended(bool suspended) override {
        calls.push_back(std::string("setHotkeyCaptureSuspended(") + (suspended ? "true" : "false") +
                        ")");
    }

    Json currentHotkeys() override {
        calls.push_back("currentHotkeys()");
        return hotkeys;
    }

    QWidget* control(std::string_view name) override {
        calls.push_back("control(" + std::string(name) + ")");
        const auto found = controls.find(name);
        return found == controls.end() ? nullptr : found->second;
    }

    // The calls whose text starts with `name`, oldest first.
    std::vector<std::string> callsTo(std::string_view name) const {
        std::vector<std::string> found;
        for (const std::string& call : calls) {
            if (call.starts_with(name)) {
                found.push_back(call);
            }
        }
        return found;
    }
};

} // namespace fv
