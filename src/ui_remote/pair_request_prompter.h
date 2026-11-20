#pragma once

class QWidget;

namespace fv {
class Executor;

namespace remote {
class RemoteEngineControl;
}

void installPairRequestPrompter(remote::RemoteEngineControl&, Executor& io, Executor& ui,
                                QWidget& window);
} // namespace fv
