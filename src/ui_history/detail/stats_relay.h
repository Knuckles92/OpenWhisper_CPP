#pragma once

#include <cstdint>
#include <memory>

#include <QMetaType>
#include <QObject>
#include <QString>

#include "history/stats_summary.h"

namespace fv::stats_detail {

// A count's summary; null when the count failed.
struct Loaded {
    std::shared_ptr<const dictation_stats::StatsSummary> summary;
};

// stats_dialog._Relay: carries worker results to the Stats windows. It lives for the session
// (never deleted), so a worker never holds or emits on a window. Emitted on the GUI thread (an
// io executor run inline) its connections run at once, as Python's synchronous fixture does.
// Affinity: created on the GUI thread; emitted from any thread.
class StatsRelay : public QObject {
    Q_OBJECT
public:
    static StatsRelay& instance();
    // Tokens, unique across windows because they share the relay.
    static std::uint64_t nextToken();

Q_SIGNALS:
    // The count's token, its summary, its error ("" when it worked).
    void loaded(quint64 token, const fv::stats_detail::Loaded& loaded, const QString& error);
    // A dictation was counted; emitted on the thread that counted it.
    void recorded();

private:
    StatsRelay() = default;
};

} // namespace fv::stats_detail

Q_DECLARE_METATYPE(fv::stats_detail::Loaded)
