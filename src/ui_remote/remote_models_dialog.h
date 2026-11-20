#pragma once

#include <functional>
#include <memory>
#include <string>

#include <QString>

class QWidget;

namespace fv {
class Executor;
namespace remote {
class RemoteEngineControl;
}

void openRemoteModelsDialog(QWidget* parent, const QString& host, std::string fingerprint,
                            std::shared_ptr<remote::RemoteEngineControl> service, Executor& io,
                            Executor& ui, std::function<void()> selected);

} // namespace fv
