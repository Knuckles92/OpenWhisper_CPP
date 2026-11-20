#pragma once

#include <string>

class QWidget;

namespace fv {
class Executor;
namespace remote {
class ClientHistorySource;
}
namespace remote_records {
class RecordSync;
}

void openClientHistory(QWidget&, remote::ClientHistorySource&, Executor& io, Executor& ui);
void openHeldMeetingHistory(QWidget&, remote_records::RecordSync&, std::string host,
                            std::string fingerprint, Executor& io, Executor& ui);
} // namespace fv
