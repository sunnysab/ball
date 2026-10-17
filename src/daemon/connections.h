// ball: a network traffic HUD for KDE Wayland sessions.
// Bounded cache of connection rows: the GUI queries the latest snapshot here
// instead of the daemon forwarding an unbounded flow history. The cache is
// deliberately independent of the process and interface totals.
#pragma once

#include "snapshot.h"

#include <QHash>
#include <QList>

#include <list>
#include <map>

namespace ball {

// Keeps at most kMaxConnections rows and an 8 MiB serialization budget. Rows
// are replaced by ID, and rates only change on sample(). Non-copyable and
// non-movable: the lookup hash stores std::list iterators into `m_entries`.
class Connections {
public:
    static constexpr int kMaxConnections = 8192;

    Connections() = default;
    Connections(const Connections&) = delete;
    Connections& operator=(const Connections&) = delete;
    Connections(Connections&&) = delete;
    Connections& operator=(Connections&&) = delete;

    // Drops every row, baseline, budget use and truncation flag.
    void reset();

    // Inserts or replaces the row with the same ID. One snapshot carries
    // cumulative counters since the collector first saw the connection, so the
    // row replaces the previous one instead of being summed into it.
    void update(const ConnectionSnapshot& row);

    // Recomputes direction rates against the last sample. The first sample
    // after reset and the first sample of a new row only record a baseline.
    void sample(qint64 monotonicMs);

    QList<ConnectionSnapshot> rows() const;

    // True once the bounded cache had to reclaim or reject a row.
    bool truncated() const { return m_truncated; }

private:
    // One cached connection. `row` is the latest record the caller handed in,
    // with the direction rates recomputed by sample(); the baseline fields
    // describe the previous sample, so rates are counter deltas, never sums.
    struct Entry {
        ConnectionSnapshot row;
        quint64 lastRx = 0;
        quint64 lastTx = 0;
        qint64 lastSampleMs = 0;
        bool hasSample = false;
        bool finished = false;
    };

    using Entries = std::list<Entry>;

    bool evictOne(quint64 protectId);
    void eraseEntry(quint64 id);

    Entries m_entries;
    QHash<quint64, Entries::iterator> m_index;
    // Finished rows keyed by their end instant, so eviction can take the
    // earliest end without a full scan or sort. Duplicate instants are legal.
    std::multimap<qint64, quint64> m_finishedIndex;
    qint64 m_budgetUsed = 0;
    bool m_truncated = false;
};

} // namespace ball
