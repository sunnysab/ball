// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/processes.h"

#include "process.h"

#include <QSet>

namespace ball {

bool Processes::update(const QList<ProcessCounter>& counters, const QDateTime& now,
                       qint64 monotonicMs, bool backendSessionChanged) {
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
        for (ProcessTraffic& row : m_rows) {
            row.today = {};
        }
        m_todayDate = date;
    }

    for (ProcessTraffic& row : m_rows) {
        row.present = false;
        row.rate = {};
    }

    if (backendSessionChanged) {
        // The collector restarts its per-process counters, so the old baselines
        // are meaningless. Keep every already-observed total and rebaseline this
        // sample without counting anything (rate stays zero).
        m_baseline.clear();
    }
    const bool baselineOnly = !hasPrevious || backendSessionChanged;

    QSet<Key> present;
    for (const ProcessCounter& counter : counters) {
        if (counter.pid <= 0) {
            continue;
        }
        const Key key{counter.pid, counter.startNs};
        present.insert(key);
        ProcessTraffic& row = m_rows[key];
        row.pid = counter.pid;
        row.startNs = counter.startNs;
        // Collector comm can be a worker thread's name even when PID is TGID.
        const QString name = readProcessName({counter.pid, counter.startNs}, m_procRoot);
        if (!name.isEmpty()) {
            row.name = name;
        } else if (row.name.isEmpty()) {
            row.name = counter.name;
        }
        row.present = true;

        if (baselineOnly) {
            // The first snapshot only establishes the reference point: traffic
            // observed before monitoring started counts as zero. A backend
            // restart does the same, because its counters cannot be compared to
            // the previous session.
            m_baseline.insert(key, Baseline{counter.rxBytes, counter.txBytes});
            continue;
        }
        if (elapsedMs <= 0) {
            // Zero or backwards elapsed keeps every baseline, so the next real
            // interval still sees the bytes that arrived in between.
            continue;
        }

        const auto baseline = m_baseline.find(key);
        quint64 rxDelta = 0;
        quint64 txDelta = 0;
        if (baseline == m_baseline.end()) {
            // A process seen for the first time (started after monitoring began)
            // or one that came back after being absent. Both start a new segment
            // in the collector, so its current counters are new bytes.
            m_baseline.insert(key, Baseline{counter.rxBytes, counter.txBytes});
            rxDelta = counter.rxBytes;
            txDelta = counter.txBytes;
        } else {
            // A counter that went backwards means the collector restarted that
            // direction; its current value is the new segment. The directions
            // are independent.
            rxDelta = counter.rxBytes >= baseline->rxBytes ? counter.rxBytes - baseline->rxBytes
                                                           : counter.rxBytes;
            txDelta = counter.txBytes >= baseline->txBytes ? counter.txBytes - baseline->txBytes
                                                           : counter.txBytes;
            baseline->rxBytes = counter.rxBytes;
            baseline->txBytes = counter.txBytes;
        }

        for (TrafficCounters* totals : {&row.today, &row.session}) {
            totals->rxBytes += rxDelta;
            totals->txBytes += txDelta;
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
            // The row keeps the traffic observed so far; a returning process
            // starts a fresh segment.
            it = m_baseline.erase(it);
        }
    }

    if (!hasPrevious || elapsedMs > 0) {
        m_previousMonotonicMs = monotonicMs;
    }
    return true;
}

QList<ProcessTraffic> Processes::rows() const { return m_rows.values(); }

ProcessTraffic Processes::row(int pid, quint64 startNs) const {
    return m_rows.value({pid, startNs});
}

} // namespace ball
