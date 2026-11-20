#pragma once

#include <functional>
#include <optional>

#include <QString>

class QWidget;

namespace fv {

struct ActivitySnapshot;

// The question Quit asks while work is running (ui_controller.py:2616-2639).
struct QuitQuestion {
    QString title; // "Quit OpenWhisper_CPP?"
    QString
        text; // "OpenWhisper_CPP is busy with {work}. Quitting now stops it before it finishes."
};

// The question for the work that quitting would stop first (busy_reasons), or nullopt when
// nothing is running.
std::optional<QuitQuestion> quitQuestion(const ActivitySnapshot& activity);

// Shows the question as a Yes/No box, No the default; true for Yes.
using QuitPrompt = std::function<bool(QWidget* parent, const QuitQuestion& question)>;
QuitPrompt questionBoxPrompt();

// _confirm_quit_while_busy: true when nothing is running or the user agrees to stop it.
bool confirmQuitWhileBusy(QWidget* parent, const ActivitySnapshot& activity,
                          const QuitPrompt& prompt);

} // namespace fv
