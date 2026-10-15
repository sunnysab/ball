// ball: a non-modal, normally managed window for live traffic details.
#pragma once

#include "ui/appearance.h"
#include "ui/snapshot.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTabWidget;
class QTableView;

namespace ball::ui {

class Details : public QWidget {
    Q_OBJECT

public:
    Details();
    void setSnapshot(const std::optional<Snapshot>& snapshot, bool fresh,
                     const QStringList& selectedInterfaces);
    void setStatus(const QString& summary, const QStringList& reasons);
    void setTheme(Theme theme);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuildTables();
    void filterTables();

    std::optional<Snapshot> m_snapshot;
    bool m_fresh = false;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_downLabels[3]{};
    QLabel* m_upLabels[3]{};
    QLabel* m_startedLabel = nullptr;
    QWidget* m_quality = nullptr;
    QLabel* m_qualitySummary = nullptr;
    QLabel* m_qualityReasons = nullptr;
    QLineEdit* m_search = nullptr;
    QCheckBox* m_showIdle = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableView* m_processTable = nullptr;
    QTableView* m_interfaceTable = nullptr;
};

} // namespace ball::ui
