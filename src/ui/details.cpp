// ball: independent traffic details, fed by the HUD's existing subscription.
#include "ui/details.h"

#include "process.h"
#include "ui/format.h"
#include "ui/traffic_model.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
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
    QList<double> keys;
    bool idle = false;
    QString search;
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
        m_rows = std::move(rows);
        applyForcedSort();
        refilter();
    }

    void setFilter(const QString& needle, bool hideIdle) {
        const QString trimmed = needle.trimmed();
        if (m_needle == trimmed && m_hideIdle == hideIdle) {
            return;
        }
        m_needle = trimmed;
        m_hideIdle = hideIdle;
        refilter();
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(m_visible.size());
    }
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
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
            return {};
        }
        return m_headers.value(section);
    }
    // Only the value columns are sortable; the name and status columns keep
    // insertion order so a stable ranking is always available.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override {
        if (!m_numericColumns.contains(column)) {
            return;
        }
        m_sortColumn = column;
        m_sortOrder = order;
        applyForcedSort();
        refilter();
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
                             const double left = a.keys.value(column);
                             const double right = b.keys.value(column);
                             return order == Qt::AscendingOrder ? left < right : left > right;
                         });
    }

    void refilter() {
        beginResetModel();
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
        endResetModel();
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
    resize(1180, 620);
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
    layout->addLayout(heading);

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("details.selection"));
    m_selectionLabel->setProperty("role", "muted");
    m_selectionLabel->setWordWrap(true);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(m_selectionLabel);

    auto* totals = new QHBoxLayout;
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
        for (QLabel* value : {m_downLabels[column], m_upLabels[column]}) {
            value->setFont(numericFont());
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cardLayout->addWidget(value);
        }
        totals->addWidget(card, 1);
    }
    m_downLabels[0]->setObjectName(QStringLiteral("details.currentDown"));
    m_upLabels[0]->setObjectName(QStringLiteral("details.currentUp"));
    m_downLabels[1]->setObjectName(QStringLiteral("details.todayDown"));
    m_upLabels[1]->setObjectName(QStringLiteral("details.todayUp"));
    m_downLabels[2]->setObjectName(QStringLiteral("details.sessionDown"));
    m_upLabels[2]->setObjectName(QStringLiteral("details.sessionUp"));
    layout->addLayout(totals);

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
    layout->addWidget(m_ipLabel);

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

    auto* tools = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("details.search"));
    m_search->setPlaceholderText(QStringLiteral("搜索进程名或 PID"));
    m_search->setClearButtonEnabled(true);
    tools->addWidget(m_search, 1);
    m_showIdle = new QCheckBox(QStringLiteral("显示空闲"), this);
    m_showIdle->setObjectName(QStringLiteral("details.showIdle"));
    tools->addWidget(m_showIdle);
    layout->addLayout(tools);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("details.tabs"));
    m_processTable = new QTableView(m_tabs);
    m_processTable->setObjectName(QStringLiteral("details.processTable"));
    m_interfaceTable = new QTableView(m_tabs);
    m_interfaceTable->setObjectName(QStringLiteral("details.interfaceTable"));
    m_tabs->addTab(m_processTable, QStringLiteral("进程"));
    m_tabs->addTab(m_interfaceTable, QStringLiteral("网卡"));
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
    for (QTableView* table : {m_processTable, m_interfaceTable}) {
        table->setSortingEnabled(true);
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
        const int metricWidth =
            QFontMetrics(numericFont()).horizontalAdvance(QStringLiteral("1023.99 MiB/s")) + 24;
        for (int column = 3; column < 9; ++column) {
            table->setColumnWidth(column, metricWidth);
        }
        table->sortByColumn(3, Qt::DescendingOrder);
    }
    connect(m_search, &QLineEdit::textChanged, this, &Details::filterTables);
    connect(m_showIdle, &QCheckBox::toggled, this, &Details::filterTables);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        m_search->setPlaceholderText(index == 0 ? QStringLiteral("搜索进程名或 PID")
                                                : QStringLiteral("搜索网卡名"));
        m_search->clear();
    });

    m_startedLabel = new QLabel(this);
    m_startedLabel->setObjectName(QStringLiteral("details.started"));
    m_startedLabel->setProperty("role", "muted");
    m_startedLabel->setWordWrap(true);
    layout->addWidget(m_startedLabel);
    setSnapshot(std::nullopt, false, {});
}

void Details::setTheme(Theme theme) { applyTheme(*this, theme); }

void Details::setStatus(const QString& summary, const QStringList& reasons) {
    m_qualitySummary->setText(summary);
    m_qualityReasons->setText(reasons.join(QLatin1Char('\n')));
    m_quality->setVisible(!summary.isEmpty());
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
                           double(row.today.rxBytes),
                           double(row.today.txBytes),
                           double(row.session.rxBytes),
                           double(row.session.txBytes)};
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
    static_cast<TrafficTableModel*>(m_interfaceTable->model())->setRows(interfaces);
    filterTables();
}

void Details::filterTables() {
    for (QTableView* table : {m_processTable, m_interfaceTable}) {
        static_cast<TrafficTableModel*>(table->model())
            ->setFilter(m_search->text(), !m_showIdle->isChecked());
    }
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
