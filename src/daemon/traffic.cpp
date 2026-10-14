// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/traffic.h"

#include <QSet>

namespace ball {

bool Traffic::update(const QList<InterfaceInfo>& interfaces, const QDateTime& now,
                     qint64 monotonicMs) {
    if (!now.isValid() || monotonicMs < 0) {
        // A broken clock sample must not touch the accumulated totals.
        return false;
    }

    const bool hasPrevious = m_previousMonotonicMs >= 0;
    const qint64 elapsedMs = hasPrevious ? monotonicMs - m_previousMonotonicMs : 0;

    if (!m_start.isValid()) {
        m_start = now;
    }

    const QDate date = now.date();
    if (m_todayDate != date) {
        // An interval that spans midnight is credited to the new day, so today
        // can be off by at most one sampling interval.
        // shortcut: midnight boundary is approximate, refine if it ever shows.
        for (InterfaceTraffic& row : m_rows) {
            row.today = {};
        }
        m_todayDate = date;
    }

    for (InterfaceTraffic& row : m_rows) {
        row.up = false;
        row.rate = {};
    }

    QSet<quint32> present;
    for (const InterfaceInfo& info : interfaces) {
        if (info.index == 0) {
            continue;
        }
        present.insert(info.index);
        InterfaceTraffic& row = m_rows[info.index];
        row.index = info.index;
        row.name = info.name; // the index is the identity, so a rename keeps the totals
        row.kind = info.kind;
        row.up = info.up;

        const auto baseline = m_baseline.find(info.index);
        if (baseline == m_baseline.end()) {
            // First sight of this interface, or it came back after being gone:
            // start from its current counters instead of mixing old and new.
            m_baseline.insert(info.index, Baseline{info.rxBytes, info.txBytes});
            continue;
        }
        if (elapsedMs <= 0) {
            // Zero or backwards elapsed keeps the baseline, so the next real
            // interval still sees the counters that arrived in between.
            continue;
        }

        const quint64 rxDelta =
            info.rxBytes >= baseline->rxBytes ? info.rxBytes - baseline->rxBytes : 0;
        const quint64 txDelta =
            info.txBytes >= baseline->txBytes ? info.txBytes - baseline->txBytes : 0;
        // A counter reset (interface recreated, driver restart) rebases instead
        // of underflowing or spiking; the two directions are independent.
        baseline->rxBytes = info.rxBytes;
        baseline->txBytes = info.txBytes;

        for (TrafficCounters* counters : {&row.today, &row.session}) {
            counters->rxBytes += rxDelta;
            counters->txBytes += txDelta;
        }
        row.rate.rxBytesPerSecond =
            static_cast<double>(rxDelta) * 1000.0 / static_cast<double>(elapsedMs);
        row.rate.txBytesPerSecond =
            static_cast<double>(txDelta) * 1000.0 / static_cast<double>(elapsedMs);
    }

    for (auto it = m_baseline.begin(); it != m_baseline.end();) {
        if (present.contains(it.key())) {
            ++it;
        } else {
            // The row keeps the traffic observed so far; a returning interface
            // gets a fresh baseline.
            it = m_baseline.erase(it);
        }
    }

    if (!hasPrevious || elapsedMs > 0) {
        m_previousMonotonicMs = monotonicMs;
    }
    return true;
}

QList<InterfaceTraffic> Traffic::rows() const { return m_rows.values(); }

InterfaceTraffic Traffic::row(quint32 index) const { return m_rows.value(index); }

} // namespace ball
