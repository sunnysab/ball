// ball: a network traffic HUD for KDE Wayland sessions.
// In-memory per-interface traffic accumulation; nothing is written to disk.
#pragma once

#include "daemon/interfaces.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>

namespace ball {

struct TrafficCounters {
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
};

struct RateSample {
    double rxBytesPerSecond = 0.0;
    double txBytesPerSecond = 0.0;
};

// Accumulated traffic of one interface. `today` is cleared at the first sample
// after the local date changes; `session` only grows while the process lives.
struct InterfaceTraffic {
    quint32 index = 0; // identity across a rename
    QString name;
    InterfaceKind kind = InterfaceKind::Physical;
    bool up = false;
    TrafficCounters today;
    TrafficCounters session;
    RateSample rate;
};

// Folds kernel counter snapshots into per-interface today/session totals. The
// interfaces stay separate on purpose: loopback, veth and VPN tunnels must not
// be summed together, the GUI picks the physical uplink later.
class Traffic {
public:
    // Feeds one snapshot. `now` is wall-clock time, used only for the monitoring
    // start and the local date (today/session split). `monotonicMs` is a
    // caller-supplied monotonic timestamp (e.g. QElapsedTimer::elapsed) used for
    // rates, so a wall-clock step does not distort them. Returns false and
    // leaves every total untouched if either input is invalid.
    bool update(const QList<InterfaceInfo>& interfaces, const QDateTime& now, qint64 monotonicMs);

    QList<InterfaceTraffic> rows() const;
    InterfaceTraffic row(quint32 index) const;
    QDateTime monitoringStart() const { return m_start; }

private:
    struct Baseline {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
    };

    QMap<quint32, InterfaceTraffic> m_rows;
    QMap<quint32, Baseline> m_baseline;
    QDateTime m_start;
    qint64 m_previousMonotonicMs = -1;
    QDate m_todayDate;
};

} // namespace ball
