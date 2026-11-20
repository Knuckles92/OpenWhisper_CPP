#pragma once

#include <string>

class QString;
class QWidget;

namespace fv {
class Executor;
namespace remote_records {
class RecordSync;
}

void openShareMeetingDialog(QWidget&, remote_records::RecordSync&, const QString& name,
                            std::string fingerprint, Executor&, Executor&);
} // namespace fv
