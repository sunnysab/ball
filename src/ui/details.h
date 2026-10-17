// ball: a non-modal, normally managed window for live traffic details.
#pragma once

#include "snapshot.h"
#include "ui/appearance.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTabWidget;
class QTableView;
class QToolButton;

namespace ball::ui {

class ConnectionsView;

class Details : public QWidget {
    Q_OBJECT

public:
    Details();
    void setSnapshot(const std::optional<Snapshot>& snapshot, bool fresh,
                     const QStringList& selectedInterfaces);
    void setStatus(const QString& summary, const QStringList& reasons);
    // Explanations only (process_status.info). Displayed as neutral plain text;
    // never drives the quality warning or the HUD gap marker.
    void setInfo(const QStringList& info);
    void setTheme(Theme theme);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuildTables();
    void filterTables();
    // Drills into the connection tab using the selected row's real identity,
    // never its position in the original (unsorted) list.
    void openConnectionsForCurrentRow();

    std::optional<Snapshot> m_snapshot;
    bool m_fresh = false;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_downLabels[3]{};
    QLabel* m_upLabels[3]{};
    QLabel* m_ipLabel = nullptr;
    QLabel* m_startedLabel = nullptr;
    // Explanations from process_status.info. Neutral, plain text, never a warning.
    QLabel* m_infoLabel = nullptr;
    QWidget* m_quality = nullptr;
    QLabel* m_qualitySummary = nullptr;
    QLabel* m_qualityReasons = nullptr;
    QWidget* m_tools = nullptr;
    QWidget* m_totals = nullptr;
    QLineEdit* m_search = nullptr;
    QCheckBox* m_showIdle = nullptr;
    QToolButton* m_viewConnections = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableView* m_processTable = nullptr;
    QTableView* m_interfaceTable = nullptr;
    ConnectionsView* m_connections = nullptr;
    int m_resetPid = 0;
    quint64 m_resetStartNs = 0;
};

} // namespace ball::ui
