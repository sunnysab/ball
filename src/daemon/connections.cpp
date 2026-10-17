// ball: a network traffic HUD for KDE Wayland sessions.
// Bounded connection cache: see connections.h for the contract.
#include "daemon/connections.h"

namespace ball {
namespace {

// Conservative serialization budget for the cached connection rows. The wire
// frame of one row is estimated from its UTF-8 text plus a fixed per-field and
// per-object allowance; the exact MessagePack size is never computed here.
constexpr qint64 kBudgetBytes = 8 * 1024 * 1024;

// One string field: MessagePack map key, type byte, length prefix and the
// text itself.
qint64 textCost(const QString& text) { return text.toUtf8().size() + 16; }

// One ConnectionProcess object: its name, the identity fields and the
// fixed-size counters.
qint64 processCost(const ConnectionProcess& process) { return textCost(process.name) + 64; }

// Estimated serialized width of one row. Deliberately generous: the budget
// only has to keep the cache well under the 16 MiB frame limit.
qint64 rowCost(const ConnectionSnapshot& row) {
    qint64 cost = 256; // id, timestamps, counters, rates and the map keys
    cost += textCost(row.protocol);
    cost += textCost(row.app);
    cost += textCost(row.state);
    cost += textCost(row.direction);
    cost += textCost(row.source);
    cost += textCost(row.target);
    cost += textCost(row.name);
    cost += textCost(row.detail);
    cost += textCost(row.end);
    cost += textCost(row.rttSource);
    for (const QString& iface : row.interfaces) {
        cost += textCost(iface);
    }
    cost += processCost(row.client);
    cost += processCost(row.server);
    for (const ConnectionProcess& process : row.io) {
        cost += processCost(process);
    }
    return cost;
}

// Finished rows are ordered by their last observation, which is the end of a
// finished connection; rows without a time sort first as the earliest.
qint64 finishKeyMs(const ConnectionSnapshot& row) {
    return row.lastSeen.isValid() ? row.lastSeen.toMSecsSinceEpoch() : 0;
}

// Removes exactly one (end instant, ID) entry, leaving other rows that share
// the instant alone.
void eraseFinished(std::multimap<qint64, quint64>& index, const ConnectionSnapshot& row) {
    auto range = index.equal_range(finishKeyMs(row));
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == row.id) {
            index.erase(it);
            return;
        }
    }
}

} // namespace

void Connections::reset() {
    m_entries.clear();
    m_index.clear();
    m_finishedIndex.clear();
    m_budgetUsed = 0;
    m_truncated = false;
}

void Connections::update(const ConnectionSnapshot& row) {
    const qint64 cost = rowCost(row);
    if (cost > kBudgetBytes) {
        // A single row that cannot fit is refused whole, never trimmed.
        m_truncated = true;
        return;
    }

    auto found = m_index.find(row.id);
    if (found != m_index.end()) {
        // Copy the list iterator out: evictOne() below may invalidate `found`.
        const Entries::iterator listIt = found.value();
        Entry& entry = *listIt;
        if (entry.finished) {
            eraseFinished(m_finishedIndex, entry.row);
        }
        m_budgetUsed -= rowCost(entry.row);
        while (m_budgetUsed + cost > kBudgetBytes) {
            if (!evictOne(row.id)) {
                break;
            }
        }
        entry.row = row;
        entry.finished = !row.end.isEmpty();
        m_budgetUsed += cost;
        if (entry.finished) {
            m_finishedIndex.insert({finishKeyMs(entry.row), entry.row.id});
        }
        // Most recently updated rows move to the front; splice keeps every
        // stored list iterator valid.
        m_entries.splice(m_entries.begin(), m_entries, listIt);
        return;
    }

    if (static_cast<int>(m_entries.size()) >= kMaxConnections) {
        evictOne(0);
    }
    while (m_budgetUsed + cost > kBudgetBytes) {
        if (!evictOne(0)) {
            break;
        }
    }
    if (m_budgetUsed + cost > kBudgetBytes) {
        m_truncated = true;
        return;
    }

    m_entries.push_front(Entry{});
    Entry& entry = m_entries.front();
    entry.row = row;
    entry.finished = !row.end.isEmpty();
    m_index.insert(row.id, m_entries.begin());
    m_budgetUsed += cost;
    if (entry.finished) {
        m_finishedIndex.insert({finishKeyMs(entry.row), entry.row.id});
    }
}

void Connections::sample(qint64 monotonicMs) {
    for (Entry& entry : m_entries) {
        ConnectionSnapshot& row = entry.row;
        if (entry.finished || !row.end.isEmpty()) {
            row.rxBps = 0.0;
            row.txBps = 0.0;
            continue;
        }
        if (!entry.hasSample) {
            // First sighting of this row: only record the baseline.
            entry.lastRx = row.rxBytes;
            entry.lastTx = row.txBytes;
            entry.lastSampleMs = monotonicMs;
            entry.hasSample = true;
            row.rxBps = 0.0;
            row.txBps = 0.0;
            continue;
        }
        if (monotonicMs <= entry.lastSampleMs) {
            // Repeated or backward time: no measurable interval, so the
            // baseline stays where it was.
            row.rxBps = 0.0;
            row.txBps = 0.0;
            continue;
        }

        const qint64 deltaMs = monotonicMs - entry.lastSampleMs;
        if (row.rxBytes >= entry.lastRx) {
            row.rxBps = static_cast<double>(row.rxBytes - entry.lastRx) * 1000.0 /
                        static_cast<double>(deltaMs);
            entry.lastRx = row.rxBytes;
        } else {
            // The collector restarted its counter: re-baseline this direction
            // instead of reporting a spike.
            entry.lastRx = row.rxBytes;
            row.rxBps = 0.0;
        }
        if (row.txBytes >= entry.lastTx) {
            row.txBps = static_cast<double>(row.txBytes - entry.lastTx) * 1000.0 /
                        static_cast<double>(deltaMs);
            entry.lastTx = row.txBytes;
        } else {
            entry.lastTx = row.txBytes;
            row.txBps = 0.0;
        }
        entry.lastSampleMs = monotonicMs;
    }
}

QList<ConnectionSnapshot> Connections::rows() const {
    QList<ConnectionSnapshot> result;
    result.reserve(static_cast<qsizetype>(m_entries.size()));
    for (const Entry& entry : m_entries) {
        result.append(entry.row);
    }
    return result;
}

bool Connections::evictOne(quint64 protectId) {
    quint64 victim = 0;
    bool found = false;
    for (const auto& key : m_finishedIndex) {
        if (key.second != protectId) {
            victim = key.second;
            found = true;
            break;
        }
    }
    if (!found) {
        // Least recently updated: the tail of the list, skipping the row being
        // replaced when there is one.
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (it->row.id != protectId) {
                victim = it->row.id;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return false;
    }
    m_truncated = true;
    eraseEntry(victim);
    return true;
}

void Connections::eraseEntry(quint64 id) {
    auto found = m_index.find(id);
    if (found == m_index.end()) {
        return;
    }
    Entry& entry = *found.value();
    if (entry.finished) {
        eraseFinished(m_finishedIndex, entry.row);
    }
    m_budgetUsed -= rowCost(entry.row);
    m_entries.erase(found.value());
    m_index.erase(found);
}

} // namespace ball
