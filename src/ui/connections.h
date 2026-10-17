// ball: a network traffic HUD for KDE Wayland sessions.
// Read-only connection query page for the collector's flow section
// (docs/connections.md). It filters the bounded cache carried by each snapshot,
// ranks it and shows the selected row's details. It never performs DNS, network
// requests or per-cell /proc reads.
#pragma once

#include "snapshot.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QShowEvent;
class QSpinBox;
class QTableWidget;
class QToolButton;

namespace ball::ui {

// The page keeps every filter and the frozen/live state. All connection rows
// come from the snapshot; the widget only decides which ones are visible.
class ConnectionsView : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionsView(QWidget* parent = nullptr);

    // Replaces the cache. When paused the displayed rows stay frozen while the
    // latest snapshot is kept for resume. `fresh` is false for an expired frame,
    // so rates are never shown as live even while rows are frozen.
    void setSnapshot(const std::optional<Snapshot>& snapshot, bool fresh);
    // Filters on the exact (pid, startNs) identity from any of client, server or
    // io. A recycled PID with a different startNs never matches.
    void setProcessFilter(int pid, quint64 startNs, const QString& name);
    void clearProcessFilter();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void scheduleRebuild();
    void rebuild();
    void applySnapshot();
    void updateSummary(int visibleCount, int matchedCount, int totalCount);
    void updateDetails();
    void copySelection();
    void refreshProtocolChoices(const QList<ConnectionSnapshot>& rows);
    bool matchesFilters(const ConnectionSnapshot& row) const;
    bool matchesText(const ConnectionSnapshot& row) const;
    bool matchesProcessFilter(const ConnectionSnapshot& row) const;
    quint64 selectedId() const;

    QLineEdit* m_search = nullptr;
    QComboBox* m_protocol = nullptr;
    QComboBox* m_state = nullptr;
    QComboBox* m_metric = nullptr;
    QSpinBox* m_limit = nullptr;
    QTableWidget* m_table = nullptr;
    QToolButton* m_pause = nullptr;
    QToolButton* m_copy = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_processFilter = nullptr;
    QToolButton* m_clearProcess = nullptr;
    QPlainTextEdit* m_details = nullptr;

    std::optional<Snapshot> m_applied;
    std::optional<Snapshot> m_latest;
    bool m_stale = false;
    bool m_paused = false;
    bool m_dirty = true;
    bool m_hasProcessFilter = false;
    int m_filterPid = 0;
    quint64 m_filterStartNs = 0;
};

} // namespace ball::ui
