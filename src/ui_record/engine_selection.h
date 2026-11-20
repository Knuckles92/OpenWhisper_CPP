#pragma once

#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <QPointer>

#include "base/executor.h"
#include "base/subscription.h"

class QObject;

namespace fv {

class SettingsStore;
class TranscriptionTabBase;

// What the engine fields show. A save that changes any of these is heard: the selected backend,
// the model, device and language per local engine, the API server and the dictation languages.
std::span<const std::string_view> engineKeys();

// Tells every view of the dictation engine when the saved engine changes. Quick Record, Upload
// File and Settings each show the engine (its backend, model, device and language). They are
// views of the saved settings, so each registers here and reloads, whoever made the change:
// another view, the controller reverting a device, or a paired computer switching this one's
// model. A save made on the GUI thread reloads the views before it returns; a save from another
// thread reloads them on the GUI thread. It lives for the session, so the settings watch never
// holds a view: a deleted view drops out of the list.
// Affinity: GUI thread (a save from any thread is delivered there).
class EngineSelection {
public:
    EngineSelection(SettingsStore& settings, Executor& ui);

    // Adds a tab; it shows the saved engine (syncEngineSelection) until it is deleted.
    void add(TranscriptionTabBase* tab);
    // Adds any other view of the saved engine (a Settings page): reload runs after every save
    // that changes one of engineKeys(), until receiver is deleted.
    void follow(QObject* receiver, std::function<void()> reload);
    // The tabs still alive, in the order they were added.
    std::vector<TranscriptionTabBase*> views() const;

private:
    struct Follower {
        QPointer<QObject> receiver;
        std::function<void()> reload;
    };

    void changed();

    std::vector<Follower> followers_;
    Subscription subscription_;
};

} // namespace fv
