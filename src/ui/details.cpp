// ball: independent traffic details, fed by the HUD's existing subscription.
#include "ui/details.h"

#include "process.h"
#include "ui/connections.h"
#include "ui/format.h"
#include "ui/traffic_chart.h"
#include "ui/traffic_model.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QTabWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace ball::ui {
namespace {

// Raw values live in an extra role so the model sorts on real numbers instead
// of the formatted text; one numeric key column is enough for that.
constexpr int kSortRole = Qt::UserRole + 1;

// The placeholder shown while a value is unavailable.
QString placeholder() { return QStringLiteral("—"); }

struct TableRow {
    QStringList texts;
    QList<QVariant> keys;
    bool idle = false;
    QString search;
    QString name;
    int pid = 0;
    quint64 startNs = 0;
};

// One table's rows plus its filter. Sorting reads the numeric keys, so
// "9.5 KiB/s" never sorts above "10 KiB/s"; filtering hides idle rows unless
// asked otherwise and matches process name or PID.
class TrafficTableModel : public QAbstractTableModel {
public:
    TrafficTableModel(QStringList headers, QSet<int> numericColumns, QObject* parent = nullptr)
        : QAbstractTableModel(parent), m_headers(std::move(headers)),
          m_numericColumns(std::move(numericColumns)) {}

    void setRows(QList<TableRow> rows) {
        beginResetModel();
        m_rows = std::move(rows);
        applyForcedSort();
        refilter();
        endResetModel();
    }

    void setFilter(const QString& needle, bool hideIdle) {
        const QString trimmed = needle.trimmed();
        if (m_needle == trimmed && m_hideIdle == hideIdle) {
            return;
        }
        beginResetModel();
        m_needle = trimmed;
        m_hideIdle = hideIdle;
        refilter();
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(m_visible.size());
    }
    // The row a view index points at after filtering and sorting. The exact
    // (pid, startNs) identity is kept as an integer here, so a caller never has
    // to read it back from a formatted cell or a pre-sort list position.
    const TableRow& visibleRow(int viewRow) const { return m_rows.at(m_visible.at(viewRow)); }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(m_headers.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= m_visible.size()) {
            return {};
        }
        const TableRow& row = m_rows.at(m_visible.at(index.row()));
        switch (role) {
        case Qt::DisplayRole:
            return row.texts.value(index.column());
        case Qt::TextAlignmentRole:
            return m_numericColumns.contains(index.column())
                       ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter))
                       : QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));
        case Qt::ToolTipRole:
            if (row.pid > 0) {
                const auto arguments = readProcessCommandLine({row.pid, row.startNs});
                const QString command =
                    arguments.isEmpty()
                        ? QStringLiteral(
                              "命令行不可用：进程已退出、无读取权限、身份已变化或超过 64 KiB。")
                        : formatCommandLine(arguments);
                return QStringLiteral("<qt><b>%1 · PID %2</b><br>%3</qt>")
                    .arg(
                        row.texts.first().toHtmlEscaped(), QString::number(row.pid),
                        command.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
            }
            return {};
        case Qt::FontRole:
            return m_numericColumns.contains(index.column()) ? QVariant(numericFont()) : QVariant();
        case kSortRole:
            return row.keys.value(index.column());
        default:
            return {};
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal) {
            return {};
        }
        if (role == Qt::TextAlignmentRole) {
            return int((m_numericColumns.contains(section) ? Qt::AlignRight : Qt::AlignLeft) |
                       Qt::AlignVCenter);
        }
        return role == Qt::DisplayRole ? QVariant(m_headers.value(section)) : QVariant();
    }
    // Only the value columns are sortable; the name and status columns keep
    // insertion order so a stable ranking is always available.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override {
        if (!m_numericColumns.contains(column)) {
            return;
        }
        beginResetModel();
        m_sortColumn = column;
        m_sortOrder = order;
        applyForcedSort();
        refilter();
        endResetModel();
    }

private:
    void applyForcedSort() {
        if (m_sortColumn < 0) {
            return;
        }
        const int column = m_sortColumn;
        const Qt::SortOrder order = m_sortOrder;
        std::stable_sort(m_rows.begin(), m_rows.end(),
                         [column, order](const TableRow& a, const TableRow& b) {
                             if (column >= 5) {
                                 const quint64 left = a.keys.value(column).toULongLong();
                                 const quint64 right = b.keys.value(column).toULongLong();
                                 return order == Qt::AscendingOrder ? left < right : left > right;
                             }
                             const double left = a.keys.value(column).toDouble();
                             const double right = b.keys.value(column).toDouble();
                             return order == Qt::AscendingOrder ? left < right : left > right;
                         });
    }

    void refilter() {
        m_visible.clear();
        for (int i = 0; i < m_rows.size(); ++i) {
            const TableRow& row = m_rows.at(i);
            if (m_hideIdle && row.idle) {
                continue;
            }
            if (!m_needle.isEmpty() && !row.search.contains(m_needle, Qt::CaseInsensitive)) {
                continue;
            }
            m_visible.append(i);
        }
    }

    QStringList m_headers;
    QSet<int> m_numericColumns;
    QList<TableRow> m_rows;
    QList<int> m_visible;
    QString m_needle;
    bool m_hideIdle = false;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
};

} // namespace

Details::Details() : QWidget(nullptr, Qt::Window) {
    setObjectName(QStringLiteral("details"));
    setWindowTitle(QStringLiteral("ball · 流量详情"));
    resize(1180, 780);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(14);

    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("网络流量"), this);
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    heading->addWidget(title);
    heading->addStretch();
    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("details.selection"));
    m_selectionLabel->setProperty("role", "muted");
    m_selectionLabel->setWordWrap(true);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    heading->addWidget(m_selectionLabel);
    layout->addLayout(heading);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("details.tabs"));
    auto* overview = new QWidget(m_tabs);
    overview->setObjectName(QStringLiteral("details.overview"));
    auto* overviewLayout = new QVBoxLayout(overview);
    overviewLayout->setContentsMargins(0, 14, 0, 0);
    overviewLayout->setSpacing(16);
    m_tabs->addTab(overview, QStringLiteral("概览"));

    m_totals = new QWidget(this);
    auto* totals = new QHBoxLayout(m_totals);
    totals->setContentsMargins(0, 0, 0, 0);
    const QStringList titles{QStringLiteral("当前速率"), QStringLiteral("今日累计"),
                             QStringLiteral("本次累计")};
    for (int column = 0; column < 3; ++column) {
        auto* card = new QFrame(this);
        card->setProperty("role", "card");
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);
        auto* caption = new QLabel(titles.at(column), card);
        caption->setProperty("role", "muted");
        cardLayout->addWidget(caption);
        m_downLabels[column] = new QLabel(card);
        m_upLabels[column] = new QLabel(card);
        m_downLabels[column]->setProperty("role", "download");
        m_upLabels[column]->setProperty("role", "upload");
        auto* values = column == 0 ? new QHBoxLayout : nullptr;
        for (QLabel* value : {m_downLabels[column], m_upLabels[column]}) {
            QFont valueFont = numericFont();
            if (column == 0) {
                valueFont.setPointSizeF(valueFont.pointSizeF() + 9);
                valueFont.setBold(true);
            }
            value->setFont(valueFont);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            if (column == 0) {
                values->addWidget(value, 1);
            } else {
                cardLayout->addWidget(value);
            }
        }
        if (values) {
            cardLayout->addLayout(values);
        }
        totals->addWidget(card, column == 0 ? 2 : 1);
    }
    m_downLabels[0]->setObjectName(QStringLiteral("details.currentDown"));
    m_upLabels[0]->setObjectName(QStringLiteral("details.currentUp"));
    m_downLabels[1]->setObjectName(QStringLiteral("details.todayDown"));
    m_upLabels[1]->setObjectName(QStringLiteral("details.todayUp"));
    m_downLabels[2]->setObjectName(QStringLiteral("details.sessionDown"));
    m_upLabels[2]->setObjectName(QStringLiteral("details.sessionUp"));
    overviewLayout->addWidget(m_totals);

    auto* chartCard = new QFrame(overview);
    chartCard->setProperty("role", "card");
    auto* chartLayout = new QVBoxLayout(chartCard);
    chartLayout->setContentsMargins(18, 14, 18, 12);
    auto* chartHeading = new QHBoxLayout;
    auto* chartTitle = new QLabel(QStringLiteral("网络活动"), chartCard);
    QFont sectionFont = font();
    sectionFont.setBold(true);
    chartTitle->setFont(sectionFont);
    chartHeading->addWidget(chartTitle);
    chartHeading->addStretch();
    auto* historyNote = new QLabel(QStringLiteral("最近 5 分钟 · 仅界面运行期间"), chartCard);
    historyNote->setProperty("role", "muted");
    chartHeading->addWidget(historyNote);
    chartLayout->addLayout(chartHeading);
    m_chart = new TrafficChart(chartCard);
    chartLayout->addWidget(m_chart, 1);
    overviewLayout->addWidget(chartCard, 3);

    auto* activityHeading = new QHBoxLayout;
    m_activityLabel = new QLabel(overview);
    m_activityLabel->setTextFormat(Qt::PlainText);
    m_activityLabel->setFont(sectionFont);
    activityHeading->addWidget(m_activityLabel);
    activityHeading->addStretch();
    auto* allProcesses = new QToolButton(overview);
    allProcesses->setText(QStringLiteral("查看全部进程 →"));
    allProcesses->setObjectName(QStringLiteral("details.allProcesses"));
    activityHeading->addWidget(allProcesses);
    overviewLayout->addLayout(activityHeading);
    m_overviewProcesses = new QTableView(overview);
    m_overviewProcesses->setObjectName(QStringLiteral("details.overviewProcesses"));
    m_overviewProcesses->setToolTip(QStringLiteral("按 socket 总速率排列，双击查看进程连接。"
                                                   "进程统计不随所选网卡变化，不与图表数值相加。"));
    overviewLayout->addWidget(m_overviewProcesses, 2);
    connect(allProcesses, &QToolButton::clicked, this, [this] { m_tabs->setCurrentIndex(1); });

    m_ipLabel = new QLabel(this);
    m_ipLabel->setObjectName(QStringLiteral("details.ipTotals"));
    m_ipLabel->setProperty("role", "muted");
    m_ipLabel->setTextFormat(Qt::PlainText);
    m_ipLabel->setWordWrap(true);
    m_ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_ipLabel->setToolTip(
        QStringLiteral("IP 报文字节是采集点观测到的 IP 报文大小，不是进程 socket I/O。"
                       "两者与网卡计数均不相等，也不互相相加；此处不随所选网卡变化。"
                       "累计从采集器启动开始，采集器重启后清零，不按今日或本次监测分段。"));
    overviewLayout->addWidget(m_ipLabel);

    m_quality = new QWidget(this);
    auto* qualityLayout = new QVBoxLayout(m_quality);
    qualityLayout->setContentsMargins(0, 0, 0, 0);
    auto* qualityHeading = new QHBoxLayout;
    m_qualitySummary = new QLabel(m_quality);
    m_qualitySummary->setObjectName(QStringLiteral("details.quality"));
    m_qualitySummary->setProperty("role", "warning");
    m_qualitySummary->setTextFormat(Qt::PlainText);
    m_qualitySummary->setWordWrap(true);
    qualityHeading->addWidget(m_qualitySummary, 1);
    auto* reasons = new QToolButton(m_quality);
    reasons->setObjectName(QStringLiteral("details.showReasons"));
    reasons->setText(QStringLiteral("查看原因"));
    reasons->setCheckable(true);
    qualityHeading->addWidget(reasons);
    qualityLayout->addLayout(qualityHeading);
    m_qualityReasons = new QLabel(m_quality);
    m_qualityReasons->setObjectName(QStringLiteral("details.reasons"));
    m_qualityReasons->setTextFormat(Qt::PlainText);
    m_qualityReasons->setWordWrap(true);
    m_qualityReasons->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_qualityReasons->setProperty("role", "muted");
    m_qualityReasons->hide();
    connect(reasons, &QToolButton::toggled, m_qualityReasons, &QLabel::setVisible);
    qualityLayout->addWidget(m_qualityReasons);
    layout->addWidget(m_quality);

    // Diagnostics from process_status.info. It is an explanation, not a quality
    // problem: it never makes the HUD show a gap marker and is plain text.
    m_infoLabel = new QLabel(this);
    m_infoLabel->setObjectName(QStringLiteral("details.info"));
    m_infoLabel->setProperty("role", "muted");
    m_infoLabel->setTextFormat(Qt::PlainText);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoLabel->setVisible(false);
    layout->addWidget(m_infoLabel);

    m_tools = new QWidget(this);
    auto* tools = new QHBoxLayout(m_tools);
    tools->setContentsMargins(0, 0, 0, 0);
    m_search = new QLineEdit(m_tools);
    m_search->setObjectName(QStringLiteral("details.search"));
    m_search->setPlaceholderText(QStringLiteral("搜索进程名或 PID"));
    m_search->setClearButtonEnabled(true);
    tools->addWidget(m_search, 1);
    m_showIdle = new QCheckBox(QStringLiteral("显示空闲"), m_tools);
    m_showIdle->setObjectName(QStringLiteral("details.showIdle"));
    tools->addWidget(m_showIdle);
    m_viewConnections = new QToolButton(m_tools);
    m_viewConnections->setObjectName(QStringLiteral("details.viewConnections"));
    m_viewConnections->setText(QStringLiteral("查看连接"));
    m_viewConnections->setToolTip(QStringLiteral("用当前选中的进程筛选连接页"));
    m_viewConnections->setEnabled(false);
    tools->addWidget(m_viewConnections);
    m_processTable = new QTableView(m_tabs);
    m_processTable->setObjectName(QStringLiteral("details.processTable"));
    m_interfaceTable = new QTableView(m_tabs);
    m_interfaceTable->setObjectName(QStringLiteral("details.interfaceTable"));
    m_connections = new ConnectionsView(m_tabs);
    m_connections->setObjectName(QStringLiteral("details.connections"));
    for (auto* table : {m_processTable, m_interfaceTable}) {
        auto* page = new QWidget(m_tabs);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 12, 0, 0);
        if (table == m_processTable) {
            pageLayout->addWidget(m_tools);
        }
        pageLayout->addWidget(table, 1);
        m_tabs->addTab(page,
                       table == m_processTable ? QStringLiteral("进程") : QStringLiteral("网卡"));
    }
    m_tabs->addTab(m_connections, QStringLiteral("连接"));
    layout->addWidget(m_tabs, 1);

    m_processTable->setModel(new TrafficTableModel(
        {QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("状态"),
         QStringLiteral("当前↓"), QStringLiteral("当前↑"), QStringLiteral("今日↓"),
         QStringLiteral("今日↑"), QStringLiteral("本次↓"), QStringLiteral("本次↑")},
        {3, 4, 5, 6, 7, 8}, this));
    m_interfaceTable->setModel(new TrafficTableModel(
        {QStringLiteral("网卡"), QStringLiteral("类型"), QStringLiteral("状态"),
         QStringLiteral("当前↓"), QStringLiteral("当前↑"), QStringLiteral("今日↓"),
         QStringLiteral("今日↑"), QStringLiteral("本次↓"), QStringLiteral("本次↑")},
        {3, 4, 5, 6, 7, 8}, this));
    m_overviewProcesses->setModel(new TrafficTableModel(
        {QStringLiteral("活跃进程"), QStringLiteral("PID"), QStringLiteral("状态"),
         QStringLiteral("下载 · socket I/O"), QStringLiteral("上传 · socket I/O"),
         QStringLiteral("今日↓"), QStringLiteral("今日↑"), QStringLiteral("本次↓"),
         QStringLiteral("本次↑")},
        {3, 4, 5, 6, 7, 8}, this));
    for (QTableView* table : {m_processTable, m_interfaceTable, m_overviewProcesses}) {
        table->setSortingEnabled(table != m_overviewProcesses);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setShowGrid(false);
        table->setWordWrap(false);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(36);
        auto* header = table->horizontalHeader();
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setMinimumSectionSize(40);
        table->setColumnWidth(0, 220);
        table->setColumnWidth(1, 80);
        table->setColumnWidth(2, 80);
        table->setColumnHidden(2, true);
        table->setColumnHidden(7, true);
        table->setColumnHidden(8, true);
        const int metricWidth =
            QFontMetrics(numericFont()).horizontalAdvance(QStringLiteral("1023.99 MiB/s")) + 24;
        for (int column = 3; column < 9; ++column) {
            table->setColumnWidth(column, metricWidth);
        }
        if (table != m_overviewProcesses) {
            table->sortByColumn(3, Qt::DescendingOrder);
            enableColumnMenu(*table);
        } else {
            table->setColumnHidden(1, true);
            table->setColumnHidden(5, true);
            table->setColumnHidden(6, true);
            header->setSectionResizeMode(0, QHeaderView::Stretch);
        }
    }
    auto* columnsButton = new QToolButton(m_tools);
    columnsButton->setText(QStringLiteral("显示列"));
    columnsButton->setObjectName(QStringLiteral("details.columns"));
    tools->addWidget(columnsButton);
    connect(columnsButton, &QToolButton::clicked, this, [this] {
        auto* header =
            (m_tabs->currentIndex() == 1 ? m_processTable : m_interfaceTable)->horizontalHeader();
        Q_EMIT header->customContextMenuRequested(QPoint(0, header->height()));
    });
    for (auto* table : {m_processTable, m_overviewProcesses}) {
        auto* processModel = static_cast<TrafficTableModel*>(table->model());
        connect(processModel, &QAbstractItemModel::modelAboutToBeReset, this,
                [this, processModel, table] {
                    m_resetPid = 0;
                    m_resetStartNs = 0;
                    const auto selected = table->selectionModel()->selectedRows();
                    if (!selected.isEmpty() && selected.first().row() < processModel->rowCount()) {
                        const TableRow& row = processModel->visibleRow(selected.first().row());
                        m_resetPid = row.pid;
                        m_resetStartNs = row.startNs;
                    }
                });
        connect(processModel, &QAbstractItemModel::modelReset, this, [this, processModel, table] {
            if (m_resetPid <= 0) {
                return;
            }
            for (int i = 0; i < processModel->rowCount(); ++i) {
                const TableRow& row = processModel->visibleRow(i);
                if (row.pid == m_resetPid && row.startNs == m_resetStartNs) {
                    table->selectRow(i);
                    break;
                }
            }
        });
    }
    connect(m_search, &QLineEdit::textChanged, this, &Details::filterTables);
    connect(m_showIdle, &QCheckBox::toggled, this, &Details::filterTables);
    connect(m_viewConnections, &QToolButton::clicked, this,
            [this] { openConnectionsForCurrentRow(m_processTable); });
    connect(m_processTable, &QTableView::doubleClicked, this,
            [this] { openConnectionsForCurrentRow(m_processTable); });
    connect(m_overviewProcesses, &QTableView::doubleClicked, this,
            [this] { openConnectionsForCurrentRow(m_overviewProcesses); });
    connect(m_processTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        const bool hasRow = !m_processTable->selectionModel()->selectedRows().isEmpty() ||
                            m_processTable->currentIndex().isValid();
        m_viewConnections->setEnabled(hasRow);
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        // The process/interface search and idle filter do not apply to the
        // connection page, which owns its own filters; hide the duplicates.
        if (index == 1 || index == 2) {
            static_cast<QVBoxLayout*>(m_tabs->widget(index)->layout())->insertWidget(0, m_tools);
        }
        m_tools->setVisible(index == 1 || index == 2);
        m_viewConnections->setVisible(index == 1);
        m_search->setPlaceholderText(index == 1 ? QStringLiteral("搜索进程名或 PID")
                                                : QStringLiteral("搜索网卡名"));
        if (index == 1 || index == 2) {
            m_search->clear();
        }
        // Switching tabs must not discard the connection page's own query state.
        m_connections->setSnapshot(m_snapshot, m_fresh);
    });

    m_tools->hide();
    m_startedLabel = new QLabel(this);
    m_startedLabel->setObjectName(QStringLiteral("details.started"));
    m_startedLabel->setProperty("role", "muted");
    m_startedLabel->setWordWrap(true);
    layout->addWidget(m_startedLabel);
    setSnapshot(std::nullopt, false, {});
}

void Details::setTheme(Theme theme) {
    applyTheme(*this, theme);
    m_chart->setTheme(theme);
}

void Details::setStatus(const QString& summary, const QStringList& reasons) {
    m_qualitySummary->setText(summary);
    m_qualityReasons->setText(reasons.join(QLatin1Char('\n')));
    m_quality->setVisible(!summary.isEmpty());
}

void Details::setInfo(const QStringList& info) {
    // Explanations only: shown as neutral plain text and never triggers the
    // quality warning, the HUD gap marker or an "incomplete" state.
    m_infoLabel->setText(info.join(QLatin1Char('\n')));
    m_infoLabel->setVisible(!info.isEmpty());
}

void Details::setSnapshot(const std::optional<Snapshot>& snapshot, bool fresh,
                          const QStringList& selectedInterfaces) {
    m_snapshot = snapshot;
    m_fresh = fresh;
    m_selectionLabel->setText(QStringLiteral("所选网卡：%1")
                                  .arg(selectedInterfaces.isEmpty()
                                           ? QStringLiteral("未选择")
                                           : selectedInterfaces.join(QStringLiteral("、"))));
    const auto totals = snapshot.has_value() && snapshot->interfacesAvailable
                            ? sumSelectedInterfaces(snapshot->interfaces, selectedInterfaces)
                            : InterfaceTotals{};
    const bool usable = fresh && totals.valid;
    m_chart->addSample(snapshot ? snapshot->at : QDateTime{},
                       snapshot ? snapshot->started : QDateTime{}, selectedInterfaces, totals.rxBps,
                       totals.txBps, usable && totals.complete);
    m_downLabels[0]->setText(
        QStringLiteral("↓ %1").arg(usable ? formatRate(totals.rxBps).trimmed() : placeholder()));
    m_upLabels[0]->setText(
        QStringLiteral("↑ %1").arg(usable ? formatRate(totals.txBps).trimmed() : placeholder()));
    const ByteCounters counters[]{totals.today, totals.session};
    for (int i = 0; i < 2; ++i) {
        m_downLabels[i + 1]->setText(QStringLiteral("↓ %1").arg(
            usable ? formatBytes(counters[i].rxBytes).trimmed() : placeholder()));
        m_upLabels[i + 1]->setText(QStringLiteral("↑ %1").arg(
            usable ? formatBytes(counters[i].txBytes).trimmed() : placeholder()));
    }
    const bool ipUsable = fresh && snapshot.has_value() && snapshot->processStatus.available &&
                          !snapshot->processStatus.stale;
    m_ipLabel->setText(QStringLiteral("IP 报文字节：%1 · 报文数：%2（采集器本次运行累计）")
                           .arg(ipUsable ? formatBytes(snapshot->ipBytes).trimmed() : placeholder(),
                                ipUsable ? QString::number(snapshot->ipPackets) : placeholder()));
    const QString started =
        snapshot.has_value() && snapshot->started.isValid()
            ? snapshot->started.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : placeholder();
    m_startedLabel->setText(
        QStringLiteral("本次监测开始于 %1 · 统计仅保存在后台内存，后台重启后清零").arg(started));
    // The connection page is fed by the same snapshot. A connection failure
    // therefore never touches the interface or process values above.
    m_connections->setSnapshot(snapshot, fresh);
    rebuildTables();
}

void Details::rebuildTables() {
    QList<TableRow> processes;
    QList<TableRow> interfaces;
    if (m_snapshot.has_value()) {
        const bool processUsable =
            m_fresh && m_snapshot->processStatus.available && !m_snapshot->processStatus.stale;
        const bool interfaceUsable = m_fresh && m_snapshot->interfacesAvailable;
        const auto addValues = [](TableRow& target, const auto& row, bool usable) {
            target.texts.append({usable ? formatRate(row.rxBps) : unavailableRate(),
                                 usable ? formatRate(row.txBps) : unavailableRate(),
                                 formatBytes(row.today.rxBytes), formatBytes(row.today.txBytes),
                                 formatBytes(row.session.rxBytes),
                                 formatBytes(row.session.txBytes)});
            target.keys = {0,
                           0,
                           0,
                           row.rxBps,
                           row.txBps,
                           QVariant::fromValue(row.today.rxBytes),
                           QVariant::fromValue(row.today.txBytes),
                           QVariant::fromValue(row.session.rxBytes),
                           QVariant::fromValue(row.session.txBytes)};
            target.idle = row.rxBps == 0.0 && row.txBps == 0.0;
        };
        for (const auto& row : m_snapshot->processes) {
            TableRow item;
            const QString name = readProcessName({row.pid, row.startNs});
            const QString displayName = name.isEmpty() ? row.name : name;
            item.texts = {displayName.isEmpty() ? QStringLiteral("(未知)") : displayName,
                          QString::number(row.pid),
                          row.present ? QStringLiteral("当前") : QStringLiteral("历史")};
            addValues(item, row, processUsable);
            if (!row.present) {
                item.idle = true;
            }
            item.search = displayName + QLatin1Char(' ') + QString::number(row.pid);
            item.name = displayName;
            item.pid = row.pid;
            item.startNs = row.startNs;
            processes.append(item);
        }
        for (const auto& row : m_snapshot->interfaces) {
            TableRow item;
            item.texts = {row.name, interfaceKindName(row.kind),
                          row.up ? QStringLiteral("启用") : QStringLiteral("停用")};
            addValues(item, row, interfaceUsable);
            item.search = row.name;
            interfaces.append(item);
        }
    }
    static_cast<TrafficTableModel*>(m_processTable->model())->setRows(processes);
    const bool processUsable = m_snapshot && m_fresh && m_snapshot->processStatus.available &&
                               !m_snapshot->processStatus.stale;
    processes.removeIf([processUsable](const TableRow& row) { return !processUsable || row.idle; });
    std::stable_sort(processes.begin(), processes.end(), [](const TableRow& a, const TableRow& b) {
        return a.keys[3].toDouble() + a.keys[4].toDouble() >
               b.keys[3].toDouble() + b.keys[4].toDouble();
    });
    m_activityLabel->setText(
        processUsable
            ? QStringLiteral("活跃进程 %1 · 按 socket 总速率排列（前 5）").arg(processes.size())
            : QStringLiteral("活跃进程 · 等待可用的进程数据"));
    processes = processes.first(std::min<qsizetype>(5, processes.size()));
    static_cast<TrafficTableModel*>(m_overviewProcesses->model())->setRows(processes);
    static_cast<TrafficTableModel*>(m_interfaceTable->model())->setRows(interfaces);
    filterTables();
}

void Details::filterTables() {
    for (QTableView* table : {m_processTable, m_interfaceTable}) {
        static_cast<TrafficTableModel*>(table->model())
            ->setFilter(m_search->text(), !m_showIdle->isChecked());
    }
}

void Details::openConnectionsForCurrentRow(QTableView* table) {
    auto* model = static_cast<TrafficTableModel*>(table->model());
    // A selected row is authoritative; fall back to the current index for a
    // keyboard-driven cursor. Either way the view index is resolved through the
    // model, so sorting or filtering cannot make the original list position
    // select a different process.
    QModelIndex index = table->currentIndex();
    const auto selected = table->selectionModel()->selectedRows();
    if (!selected.isEmpty()) {
        index = selected.first();
    }
    if (!index.isValid() || index.row() < 0 || index.row() >= model->rowCount() ||
        model->columnCount() == 0) {
        return;
    }
    const TableRow& row = model->visibleRow(index.row());
    m_connections->setProcessFilter(row.pid, row.startNs, row.name);
    m_tabs->setCurrentWidget(m_connections);
}

void Details::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace ball::ui
