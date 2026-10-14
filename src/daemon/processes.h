// ball: a network traffic HUD for KDE Wayland sessions.
// In-memory per-process traffic accumulation; nothing is written to disk.
#pragma once

#include "daemon/traffic.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>

#include <utility>

namespace ball {

// One process row as delivered by the backend for a single sampling instant.
// `startNs` is the collector's `start_ns`, a nanosecond identity, not the raw
// /proc/<pid>/stat starttime tick; together with the PID it distinguishes a
// recycled PID from its previous owner. `rxBytes`/`txBytes` are cumulative for
// the collector's current session, so they are not guaranteed to cover the
// whole process lifetime.
struct ProcessCounter {
    int pid = 0;
    quint64 startNs = 0;
    QString name;
    quint64 rxBytes = 0; // cumulative within the backend session
    quint64 txBytes = 0;
};

// Accumulated traffic of one process incarnation. Rows are kept after the
// process disappears so its observed traffic stays visible; `present` says
// whether the last snapshot still listed it.
struct ProcessTraffic {
    int pid = 0;
    quint64 startNs = 0;
    QString name;
    bool present = false;
    TrafficCounters today;
    TrafficCounters session;
    RateSample rate;
};

// Folds backend process counter snapshots into per-process today/session
// totals. A PID is only an identity together with startNs, so a recycled PID
// starts a fresh row. Rates use the caller-supplied monotonic timestamp, and
// accumulation continues across a backend session restart.
class Processes {
public:
    // Feeds one snapshot of every process the backend currently knows.
    // `now` is wall-clock time (monitoring start and local date only),
    // `monotonicMs` a monotonic timestamp (e.g. QElapsedTimer::elapsed) used
    // for rates, `backendSessionChanged` true when the traffic backend itself
    // restarted. Returns false and leaves every total untouched if either time
    // input is invalid.
    bool update(const QList<ProcessCounter>& counters, const QDateTime& now, qint64 monotonicMs,
                bool backendSessionChanged = false);

    QList<ProcessTraffic> rows() const;
    ProcessTraffic row(int pid, quint64 startNs) const;
    QDateTime monitoringStart() const { return m_start; }

private:
    using Key = std::pair<int, quint64>;

    struct Baseline {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
    };

    QMap<Key, ProcessTraffic> m_rows;
    QMap<Key, Baseline> m_baseline;
    QDateTime m_start;
    qint64 m_previousMonotonicMs = -1;
    QDate m_todayDate;
};

} // namespace ball
