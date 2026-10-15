// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/hud.h"

#include "ui/autostart.h"
#include "ui/format.h"
#include "ui/placement.h"
#include "ui/snapshot_client.h"

#include <LayerShellQt/Window>

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ball::ui {
namespace {

constexpr int kBarWidth = 232;
constexpr int kBarHeight = 40;
constexpr int kPanelWidth = 760;
constexpr int kPanelHeight = 560;
constexpr int kCornerRadius = 12;
constexpr int kSnapDistance = 14;
constexpr int kDefaultMargin = 16;

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
        m_needle = needle.trimmed();
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

Hud::Hud(const QString& socketPath, QSettings& settings, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint), m_settings(&settings) {
    setObjectName(QStringLiteral("hud"));
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("ball 流量"));
    // A layer-shell surface never appears in the task bar or the Alt-Tab list;
    // Qt::Tool keeps the same promise on any other platform.
    m_preferences = Preferences::load(settings);
    m_screenName = m_preferences.screenName;
    m_selectedInterfaces = m_preferences.interfaces;
    m_opacity = m_preferences.opacity;
    m_position = m_preferences.position;
    if (!m_preferences.hasPosition) {
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QRect geometry = screen != nullptr ? screen->geometry() : QRect(0, 0, 800, 600);
        m_position = QPoint(geometry.left() + kDefaultMargin, geometry.top() + kDefaultMargin);
    }

    buildUi();
    applyPalette();
    buildContextMenu();
    setBackgroundOpacity(m_opacity);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(hudIcon(), this);
        m_tray->setToolTip(QStringLiteral("ball 流量"));
        m_tray->setContextMenu(m_contextMenu);
        connect(m_tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger) {
                        setHiddenByUser(!m_hiddenByUser);
                    }
                });
        m_tray->show();
    }

    m_client = new SnapshotClient(this);
    connect(m_client, &SnapshotClient::updated, this, [this] {
        applySnapshot();
        applyStatus();
        Q_EMIT snapshotChanged();
    });
    connect(qApp, &QGuiApplication::screenAdded, this,
            [this](QScreen*) { clampIntoVisibleArea(); });
    connect(qApp, &QGuiApplication::screenRemoved, this,
            [this](QScreen*) { clampIntoVisibleArea(); });

    applySnapshot();
    applyStatus();
    m_client->start(socketPath);
}

Hud::~Hud() {
    // Persist whatever the user last chose; the backend is not touched.
    savePreferences();
}

void Hud::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(4);
    QWidget* barWidget = new QWidget(this);
    barWidget->setObjectName(QStringLiteral("hud.bar"));
    auto* bar = new QHBoxLayout(barWidget);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(8);
    m_rxLabel = new QLabel(this);
    m_rxLabel->setObjectName(QStringLiteral("hud.rx"));
    m_txLabel = new QLabel(this);
    m_txLabel->setObjectName(QStringLiteral("hud.tx"));
    for (QLabel* label : {m_rxLabel, m_txLabel}) {
        label->setFont(numericFont());
        label->setFixedWidth(
            QFontMetrics(label->font()).horizontalAdvance(QStringLiteral("↓ 1023.99 MiB/s")));
        label->setTextInteractionFlags(Qt::NoTextInteraction);
    }
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("hud.status"));
    m_statusLabel->setFixedWidth(
        QFontMetrics(m_statusLabel->font()).horizontalAdvance(QStringLiteral("未选网卡")));
    bar->addWidget(m_rxLabel);
    bar->addWidget(m_txLabel);
    bar->addStretch(1);
    bar->addWidget(m_statusLabel);
    layout->addWidget(barWidget);

    m_panel = new QWidget(this);
    m_panel->setObjectName(QStringLiteral("hud.panel"));
    auto* panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(6);

    auto* totals = new QGridLayout;
    const auto addTotal = [this, totals](int row, int column, const QString& title, QLabel*& target,
                                         const char* name) {
        auto* caption = new QLabel(title, m_panel);
        const QString prefix = QString::fromLatin1(name);
        caption->setObjectName(prefix + QStringLiteral(".caption"));
        target = new QLabel(placeholder(), m_panel);
        target->setObjectName(prefix);
        target->setFont(numericFont());
        totals->addWidget(caption, row, column * 2);
        totals->addWidget(target, row, column * 2 + 1);
    };
    addTotal(0, 0, QStringLiteral("总下行"), m_totalDownLabel, "hud.totalDown");
    addTotal(0, 1, QStringLiteral("总上行"), m_totalUpLabel, "hud.totalUp");
    addTotal(1, 0, QStringLiteral("今日下行"), m_todayDownLabel, "hud.todayDown");
    addTotal(1, 1, QStringLiteral("今日上行"), m_todayUpLabel, "hud.todayUp");
    addTotal(2, 0, QStringLiteral("本次下行"), m_sessionDownLabel, "hud.sessionDown");
    addTotal(2, 1, QStringLiteral("本次上行"), m_sessionUpLabel, "hud.sessionUp");
    auto* startedCaption = new QLabel(QStringLiteral("开始时间"), m_panel);
    m_startedLabel = new QLabel(placeholder(), m_panel);
    m_startedLabel->setObjectName(QStringLiteral("hud.started"));
    totals->addWidget(startedCaption, 3, 0);
    totals->addWidget(m_startedLabel, 3, 1, 1, 3);
    panelLayout->addLayout(totals);

    m_qualityLabel = new QLabel(m_panel);
    m_qualityLabel->setObjectName(QStringLiteral("hud.quality"));
    m_qualityLabel->setWordWrap(true);
    panelLayout->addWidget(m_qualityLabel);

    m_tabs = new QTabWidget(m_panel);
    m_tabs->setObjectName(QStringLiteral("hud.tabs"));
    auto* processPage = new QWidget(m_tabs);
    processPage->setObjectName(QStringLiteral("hud.processPage"));
    auto* processLayout = new QVBoxLayout(processPage);
    processLayout->setContentsMargins(0, 0, 0, 0);
    auto* searchRow = new QHBoxLayout;
    m_processSearch = new QLineEdit(processPage);
    m_processSearch->setObjectName(QStringLiteral("hud.processSearch"));
    m_processSearch->setPlaceholderText(QStringLiteral("搜索进程名或 PID"));
    m_processSearch->setClearButtonEnabled(true);
    m_showIdle = new QCheckBox(QStringLiteral("显示空闲"), processPage);
    m_showIdle->setObjectName(QStringLiteral("hud.showIdle"));
    searchRow->addWidget(m_processSearch, 1);
    searchRow->addWidget(m_showIdle);
    processLayout->addLayout(searchRow);
    m_processTable = new QTableView(processPage);
    m_processTable->setObjectName(QStringLiteral("hud.processTable"));
    m_interfaceTable = new QTableView(m_panel);
    m_interfaceTable->setObjectName(QStringLiteral("hud.interfaceTable"));
    processLayout->addWidget(m_processTable, 1);
    m_tabs->addTab(processPage, QStringLiteral("进程"));
    m_tabs->addTab(m_interfaceTable, QStringLiteral("网卡"));
    panelLayout->addWidget(m_tabs, 1);
    layout->addWidget(m_panel, 1);

    const QSet<int> processNumeric{3, 4, 5, 6, 7, 8};
    auto* processModel = new TrafficTableModel(
        {QStringLiteral("进程"), QStringLiteral("PID"), QStringLiteral("状态"),
         QStringLiteral("当前↓"), QStringLiteral("当前↑"), QStringLiteral("今日↓"),
         QStringLiteral("今日↑"), QStringLiteral("本次↓"), QStringLiteral("本次↑")},
        processNumeric, this);
    processModel->setObjectName(QStringLiteral("hud.processModel"));
    m_processTable->setModel(processModel);
    connect(m_processSearch, &QLineEdit::textChanged, this,
            [this, processModel](const QString& text) {
                processModel->setFilter(text, !m_showIdle->isChecked());
            });
    connect(m_showIdle, &QCheckBox::toggled, this, [this, processModel](bool checked) {
        processModel->setFilter(m_processSearch->text(), !checked);
    });
    processModel->setFilter(QString(), true);

    const QSet<int> interfaceNumeric{3, 4, 5, 6, 7, 8};
    auto* interfaceModel = new TrafficTableModel(
        {QStringLiteral("网卡"), QStringLiteral("类型"), QStringLiteral("状态"),
         QStringLiteral("当前↓"), QStringLiteral("当前↑"), QStringLiteral("今日↓"),
         QStringLiteral("今日↑"), QStringLiteral("本次↓"), QStringLiteral("本次↑")},
        interfaceNumeric, this);
    interfaceModel->setObjectName(QStringLiteral("hud.interfaceModel"));
    m_interfaceTable->setModel(interfaceModel);
    interfaceModel->setFilter(QString(), true);

    for (QTableView* table : {m_processTable, m_interfaceTable}) {
        table->setSortingEnabled(true);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setShowGrid(false);
        table->setWordWrap(false);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(false);
        // The first (name) column takes the slack; the numeric columns size to
        // their fixed-width digits so nothing is clipped or scrolls sideways.
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    }
    m_processTable->sortByColumn(3, Qt::DescendingOrder);
    m_interfaceTable->sortByColumn(3, Qt::DescendingOrder);

    m_panel->hide();
    resize(kBarWidth, kBarHeight);
}

void Hud::buildContextMenu() {
    if (m_contextMenu == nullptr) {
        m_contextMenu = new QMenu(this);
        m_contextMenu->setObjectName(QStringLiteral("hud.menu"));
    }
    m_contextMenu->clear();

    auto* hideAction = m_contextMenu->addAction(m_hiddenByUser ? QStringLiteral("恢复显示")
                                                               : QStringLiteral("隐藏"));
    hideAction->setObjectName(QStringLiteral("hud.action.hide"));
    connect(hideAction, &QAction::triggered, this, [this] { setHiddenByUser(!m_hiddenByUser); });

    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    auto* autostartAction = m_contextMenu->addAction(QStringLiteral("登录时启动界面"));
    autostartAction->setObjectName(QStringLiteral("hud.action.autostart"));
    autostartAction->setCheckable(true);
    autostartAction->setChecked(autostartEnabled(configDirectory));
    autostartAction->setToolTip(QStringLiteral("登录后自动显示流量条；后台采集由独立服务管理"));
    connect(autostartAction, &QAction::triggered, this,
            [this, autostartAction, configDirectory](bool enabled) {
                QString error;
                if (!setAutostartEnabled(configDirectory, QCoreApplication::applicationFilePath(),
                                         enabled, error)) {
                    {
                        const QSignalBlocker blocker(autostartAction);
                        autostartAction->setChecked(autostartEnabled(configDirectory));
                    }
                    QMessageBox::warning(this, QStringLiteral("无法设置登录自启"),
                                         QStringLiteral("无法保存设置：%1").arg(error));
                }
            });

    auto* lockAction = m_contextMenu->addAction(QStringLiteral("锁定位置"));
    lockAction->setObjectName(QStringLiteral("hud.action.lock"));
    lockAction->setCheckable(true);
    lockAction->setChecked(m_locked);
    connect(lockAction, &QAction::toggled, this, &Hud::setLocked);

    auto* pinAction = m_contextMenu->addAction(QStringLiteral("固定展开"));
    pinAction->setObjectName(QStringLiteral("hud.action.pin"));
    pinAction->setCheckable(true);
    pinAction->setChecked(m_pinned);
    connect(pinAction, &QAction::toggled, this, &Hud::setPinned);

    auto* opacityMenu = m_contextMenu->addMenu(QStringLiteral("透明度"));
    opacityMenu->setObjectName(QStringLiteral("hud.action.opacity"));
    for (const int percent : {50, 70, 85, 100}) {
        auto* action = opacityMenu->addAction(QStringLiteral("%1%").arg(percent));
        action->setObjectName(QStringLiteral("hud.action.opacity.%1").arg(percent));
        action->setCheckable(true);
        action->setChecked(qRound(m_opacity * 100.0) == percent);
        connect(action, &QAction::triggered, this,
                [this, percent] { setOpacity(percent / 100.0); });
    }

    auto* interfaceMenu = m_contextMenu->addMenu(QStringLiteral("统计网卡"));
    interfaceMenu->setObjectName(QStringLiteral("hud.action.interfaces"));
    if (m_knownInterfaces.isEmpty()) {
        interfaceMenu->addAction(QStringLiteral("暂无网卡"))->setEnabled(false);
    }
    for (const InterfaceSnapshot& row : m_knownInterfaces) {
        const QString label = QStringLiteral("%1 (%2)").arg(row.name, interfaceKindName(row.kind));
        auto* action = interfaceMenu->addAction(label);
        action->setObjectName(QStringLiteral("hud.action.interface.%1").arg(row.name));
        action->setCheckable(true);
        action->setChecked(m_selectedInterfaces.contains(row.name));
        connect(action, &QAction::triggered, this, [this, name = row.name] {
            QStringList selection = m_selectedInterfaces;
            if (selection.removeAll(name) == 0) {
                selection.append(name);
            }
            setSelectedInterfaces(selection);
        });
    }

    m_contextMenu->addSeparator();
    auto* quitAction = m_contextMenu->addAction(QStringLiteral("退出 GUI"));
    quitAction->setObjectName(QStringLiteral("hud.action.quit"));
    connect(quitAction, &QAction::triggered, this, &Hud::exitRequested);
}

void Hud::openContextMenu(const QPoint& globalPos) {
    buildContextMenu();
    m_contextMenu->popup(globalPos);
}

void Hud::applySnapshot() {
    const std::optional<Snapshot>& snapshot = m_client->snapshot();
    const bool fresh = m_client->fresh();
    if (snapshot.has_value()) {
        m_knownInterfaces = snapshot->interfaces;
    }
    ensureDefaultInterfaceSelection();

    const InterfaceTotals totals =
        snapshot.has_value() && snapshot->interfacesAvailable
            ? sumSelectedInterfaces(snapshot->interfaces, m_selectedInterfaces)
            : InterfaceTotals{};

    const bool usable =
        snapshot.has_value() && fresh && snapshot->interfacesAvailable && totals.valid;
    m_rxLabel->setText(QStringLiteral("↓ ") +
                       (usable ? formatRate(totals.rxBps) : unavailableRate()));
    m_txLabel->setText(QStringLiteral("↑ ") +
                       (usable ? formatRate(totals.txBps) : unavailableRate()));
    m_totalDownLabel->setText(usable ? formatRate(totals.rxBps) : unavailableRate());
    m_totalUpLabel->setText(usable ? formatRate(totals.txBps) : unavailableRate());
    m_todayDownLabel->setText(usable ? formatBytes(totals.today.rxBytes) : placeholder());
    m_todayUpLabel->setText(usable ? formatBytes(totals.today.txBytes) : placeholder());
    m_sessionDownLabel->setText(usable ? formatBytes(totals.session.rxBytes) : placeholder());
    m_sessionUpLabel->setText(usable ? formatBytes(totals.session.txBytes) : placeholder());
    m_startedLabel->setText(
        snapshot.has_value() && snapshot->started.isValid()
            ? snapshot->started.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : placeholder());

    rebuildInterfaceTable();
    rebuildProcessTable();
}

void Hud::applyStatus() {
    QStringList problems;
    if (!m_client->error().isEmpty()) {
        problems.append(m_client->error());
    }
    const std::optional<Snapshot>& snapshot = m_client->snapshot();
    if (snapshot.has_value()) {
        if (!snapshot->interfacesAvailable && !snapshot->interfaceError.isEmpty()) {
            problems.append(QStringLiteral("网卡不可用：%1").arg(snapshot->interfaceError));
        }
        if (!snapshot->processStatus.available) {
            problems.append(QStringLiteral("进程统计不可用"));
        }
        if (snapshot->processStatus.stale) {
            problems.append(QStringLiteral("进程统计已过期"));
        }
        if (!snapshot->processStatus.error.isEmpty()) {
            problems.append(snapshot->processStatus.error);
        }
        for (const QString& note : snapshot->processStatus.notes) {
            problems.append(note);
        }
        if (m_client->incomplete()) {
            problems.append(QStringLiteral("数据不完整"));
        }
    } else {
        problems.append(QStringLiteral("等待数据"));
    }

    // The compact bar always carries a short marker so a stale HUD is never
    // mistaken for a live zero.
    QString shortState;
    if (!snapshot.has_value()) {
        shortState = QStringLiteral("无数据");
    } else if (!m_client->fresh()) {
        shortState = QStringLiteral("已过期");
    } else if (!snapshot->interfacesAvailable) {
        shortState = QStringLiteral("网卡异常");
    } else if (m_selectedInterfaces.isEmpty()) {
        shortState = QStringLiteral("未选网卡");
    } else if (snapshot->processStatus.incomplete || m_client->incomplete()) {
        shortState = QStringLiteral("不完整");
    } else {
        shortState = QStringLiteral("正常");
    }
    m_statusLabel->setText(shortState);
    m_qualityLabel->setText(problems.join(QStringLiteral("；")));
}

void Hud::rebuildInterfaceTable() {
    auto* model = static_cast<TrafficTableModel*>(m_interfaceTable->model());
    const std::optional<Snapshot>& snapshot = m_client->snapshot();
    const bool fresh = m_client->fresh();
    const bool usable = snapshot.has_value() && fresh && snapshot->interfacesAvailable;
    QList<TableRow> rows;
    if (snapshot.has_value()) {
        for (const InterfaceSnapshot& row : snapshot->interfaces) {
            InterfaceSnapshot effective = row;
            if (!usable) {
                effective.rxBps = 0.0;
                effective.txBps = 0.0;
            }
            TableRow tableRow;
            tableRow.texts = {row.name,
                              interfaceKindName(row.kind),
                              row.up ? QStringLiteral("启用") : QStringLiteral("停用"),
                              usable ? formatRate(effective.rxBps) : unavailableRate(),
                              usable ? formatRate(effective.txBps) : unavailableRate(),
                              formatBytes(row.today.rxBytes),
                              formatBytes(row.today.txBytes),
                              formatBytes(row.session.rxBytes),
                              formatBytes(row.session.txBytes)};
            tableRow.keys = {0,
                             0,
                             0,
                             effective.rxBps,
                             effective.txBps,
                             double(row.today.rxBytes),
                             double(row.today.txBytes),
                             double(row.session.rxBytes),
                             double(row.session.txBytes)};
            tableRow.search = row.name;
            tableRow.idle = !row.up;
            rows.append(tableRow);
        }
    }
    model->setRows(rows);
}

void Hud::rebuildProcessTable() {
    auto* model = static_cast<TrafficTableModel*>(m_processTable->model());
    const std::optional<Snapshot>& snapshot = m_client->snapshot();
    // Process rates are only live while the backend marks the table available
    // and fresh.
    const bool usable = snapshot.has_value() && m_client->fresh() &&
                        snapshot->processStatus.available && !snapshot->processStatus.stale;
    QList<TableRow> rows;
    if (snapshot.has_value()) {
        for (const ProcessSnapshot& row : snapshot->processes) {
            TableRow tableRow;
            tableRow.texts = {row.name.isEmpty() ? QStringLiteral("(未知)") : row.name,
                              QString::number(row.pid),
                              row.present ? QStringLiteral("当前") : QStringLiteral("历史"),
                              usable ? formatRate(row.rxBps) : unavailableRate(),
                              usable ? formatRate(row.txBps) : unavailableRate(),
                              formatBytes(row.today.rxBytes),
                              formatBytes(row.today.txBytes),
                              formatBytes(row.session.rxBytes),
                              formatBytes(row.session.txBytes)};
            tableRow.keys = {0,
                             double(row.pid),
                             0,
                             row.rxBps,
                             row.txBps,
                             double(row.today.rxBytes),
                             double(row.today.txBytes),
                             double(row.session.rxBytes),
                             double(row.session.txBytes)};
            tableRow.search = row.name + QLatin1Char(' ') + QString::number(row.pid);
            tableRow.idle = !row.present || (row.rxBps == 0.0 && row.txBps == 0.0);
            rows.append(tableRow);
        }
    }
    model->setRows(rows);
}

void Hud::ensureDefaultInterfaceSelection() {
    if (m_preferences.hasInterfaceSelection || !m_selectedInterfaces.isEmpty() ||
        m_knownInterfaces.isEmpty()) {
        return;
    }
    m_selectedInterfaces = defaultInterfaceSelection(m_knownInterfaces);
    m_preferences.hasInterfaceSelection = !m_selectedInterfaces.isEmpty();
    Q_EMIT selectedInterfacesChanged(m_selectedInterfaces);
}

void Hud::setBackgroundOpacity(double opacity) {
    m_opacity = qBound(0.2, opacity, 1.0);
    update();
}

void Hud::setOpacity(double opacity) {
    if (qFuzzyCompare(m_opacity, qBound(0.2, opacity, 1.0))) {
        return;
    }
    setBackgroundOpacity(opacity);
    savePreferences();
    Q_EMIT opacityChanged(m_opacity);
}

void Hud::setSelectedInterfaces(const QStringList& names) {
    QStringList selection = names;
    selection.removeDuplicates();
    selection.sort();
    if (m_selectedInterfaces == selection && m_preferences.hasInterfaceSelection) {
        return;
    }
    m_preferences.hasInterfaceSelection = true;
    m_selectedInterfaces = selection;
    savePreferences();
    applySnapshot();
    Q_EMIT selectedInterfacesChanged(m_selectedInterfaces);
}

void Hud::setExpanded(bool expanded) {
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    if (m_expanded) {
        m_panel->show();
        resize(kPanelWidth, kPanelHeight);
    } else {
        m_panel->hide();
        resize(kBarWidth, kBarHeight);
    }
    clampIntoVisibleArea();
    updateLayerShell();
    Q_EMIT expandedChanged(m_expanded);
}

void Hud::toggleExpanded() { setExpanded(!m_expanded); }

void Hud::setLocked(bool locked) {
    if (m_locked == locked) {
        return;
    }
    m_locked = locked;
    Q_EMIT lockedChanged(m_locked);
}

void Hud::setPinned(bool pinned) {
    if (m_pinned == pinned) {
        return;
    }
    m_pinned = pinned;
    // Pinning only keeps an already open panel from auto-collapsing; it does
    // not open it, so the user's last click still decides visibility.
    Q_EMIT pinnedChanged(m_pinned);
}

void Hud::applyPalette() {
    // The chrome is a dark translucent card, so the default (dark text on a
    // presumed light window) palette is unreadable. Set a local palette only;
    // QApplication's palette is shared with every other window.
    QPalette palette = this->palette();
    const QColor text(238, 240, 244);
    const QColor base(30, 33, 40);
    const QColor button(52, 57, 68);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::PlaceholderText, QColor(160, 171, 187));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Window, base);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, button);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, QColor(38, 132, 255));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(palette);
    // The tree/table headers and the tab bar follow the widget palette, but a
    // global stylesheet from the desktop can still override them; the explicit
    // palette keeps the app readable on both light and dark system themes.
}

void Hud::setHiddenByUser(bool hidden) {
    if (m_hiddenByUser == hidden) {
        return;
    }
    m_hiddenByUser = hidden;
    if (hidden) {
        hide();
    } else {
        show();
    }
}

void Hud::savePreferences() {
    if (m_settings == nullptr) {
        return;
    }
    m_preferences.position = m_position;
    m_preferences.screenName = currentScreenName();
    m_preferences.opacity = m_opacity;
    m_preferences.interfaces = m_selectedInterfaces;
    m_preferences.save(*m_settings);
}

QScreen* Hud::currentScreen() const {
    if (!m_screenName.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen->name() == m_screenName) {
                return screen;
            }
        }
    }
    if (QScreen* screen = QGuiApplication::screenAt(m_position)) {
        return screen;
    }
    return QGuiApplication::primaryScreen();
}

QString Hud::currentScreenName() const {
    const QScreen* screen = currentScreen();
    return screen != nullptr ? screen->name() : QString();
}

void Hud::applyPosition(const QPoint& globalTopLeft, bool persist) {
    const QScreen* screen = currentScreen();
    const QRect screenRect = screen != nullptr ? screen->geometry() : QRect(m_position, size());
    const QRect clamped = clampRectInto(QRect(globalTopLeft, size()), screenRect);
    m_position = clamped.topLeft();
    // A layer-shell surface refuses free moves: the position is expressed as an
    // anchor plus margins instead. On Wayland QWidget::pos() is a by-product of
    // those margins, so it is never treated as the requested position.
    if (compositorOwnsPosition()) {
        updateLayerShell();
        // Layer-shell placement is double-buffered until the next surface commit.
        update();
    } else if (!updateLayerShell()) {
        move(m_position);
    }
    if (persist) {
        savePreferences();
    }
}

void Hud::clampIntoVisibleArea() {
    applyPosition(m_position, false);
    savePreferences();
}

bool Hud::updateLayerShell() {
    QWindow* handle = windowHandle();
    if (handle == nullptr || QGuiApplication::platformName() != QLatin1String("wayland")) {
        return false;
    }
    LayerShellQt::Window* shell = LayerShellQt::Window::get(handle);
    if (shell == nullptr) {
        return false;
    }
    if (!m_layerShellApplied) {
        shell->setScope(QStringLiteral("ball"));
        shell->setLayer(LayerShellQt::Window::LayerTop);
        shell->setExclusiveZone(0);
        shell->setActivateOnShow(false);
        m_layerShellApplied = true;
    }
    const QScreen* screen = currentScreen();
    const QRect screenRect = screen != nullptr ? screen->geometry() : QRect(m_position, size());
    const AnchoredPlacement placement = anchoredPlacementFor(QRect(m_position, size()), screenRect);
    LayerShellQt::Window::Anchors anchors;
    anchors.setFlag(LayerShellQt::Window::AnchorTop, placement.vertical == VerticalEdge::Top);
    anchors.setFlag(LayerShellQt::Window::AnchorBottom, placement.vertical == VerticalEdge::Bottom);
    anchors.setFlag(LayerShellQt::Window::AnchorLeft, placement.horizontal == HorizontalEdge::Left);
    anchors.setFlag(LayerShellQt::Window::AnchorRight,
                    placement.horizontal == HorizontalEdge::Right);
    shell->setAnchors(anchors);
    shell->setMargins(placement.margins);
    shell->setDesiredSize(size());
    // The bar never takes keyboard focus; the expanded panel asks for it on
    // demand (a click makes it focused) and gives it back when closed.
    shell->setKeyboardInteractivity(m_expanded ? LayerShellQt::Window::KeyboardInteractivityOnDemand
                                               : LayerShellQt::Window::KeyboardInteractivityNone);
    if (screen != nullptr) {
        shell->setScreen(const_cast<QScreen*>(screen));
    }
    m_layerShellActive = true;
    return true;
}

bool Hud::compositorOwnsPosition() const { return m_layerShellActive; }

void Hud::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    clampIntoVisibleArea();
    updateLayerShell();
}

void Hud::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    // On a layer-shell surface the compositor places the window from the
    // anchors/margins we request, so QWidget::pos() is a layout by-product and
    // never the requested position. Adopting it here is what made a drag snap
    // back (or to the origin) on Wayland; m_position stays authoritative there.
    if (compositorOwnsPosition() || m_dragging) {
        return;
    }
    m_position = pos();
    savePreferences();
}

void Hud::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(rect(), kCornerRadius, kCornerRadius);
    // Only the background is translucent; child labels keep fully opaque text.
    QColor background(24, 26, 32);
    background.setAlphaF(static_cast<float>(m_opacity));
    painter.fillPath(path, background);
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
    painter.drawPath(path);
}

void Hud::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        openContextMenu(event->globalPosition().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Locking only stops the surface from moving; the click that toggles the
    // panel is still recorded.
    m_pressed = true;
    m_dragging = false;
    // Wayland exposes no trustworthy global pointer position for a layer-shell
    // surface, so a drag is anchored at the surface-local press point.
    m_pressLocal = event->position();
}

void Hud::mouseMoveEvent(QMouseEvent* event) {
    if (!m_pressed) {
        return;
    }
    const QPointF delta = event->position() - m_pressLocal;
    if (!m_dragging && delta.manhattanLength() < QApplication::startDragDistance()) {
        return; // still a click, not a drag
    }
    m_dragging = true;
    if (m_locked) {
        return; // a locked HUD keeps its position, but the gesture is a drag
    }
    // m_position is the authority. Adding the local pointer offset to it (not
    // to a press-time QWidget::pos()) keeps the surface under the cursor while
    // the compositor moves it, and self-corrects when the compositor lags.
    // shortcut: fractional-scale release can drift slightly; use relative motion if it persists.
    applyPosition((QPointF(m_position) + delta).toPoint(), false);
}

void Hud::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    if (m_dragging) {
        // A locked HUD must not move on release either, but a drag is still not
        // a click, so it never toggles the panel.
        if (!m_locked) {
            const QScreen* screen = currentScreen();
            const QRect screenRect =
                screen != nullptr ? screen->geometry() : QRect(m_position, size());
            applyPosition(snappedPosition(QRect(m_position, size()), screenRect, kSnapDistance),
                          true);
        }
    } else {
        toggleExpanded();
    }
    m_dragging = false;
}

void Hud::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        // Esc always closes the panel, whether or not the position is locked or
        // the panel pinned; those only govern the automatic collapse.
        if (m_expanded) {
            setExpanded(false);
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool Hud::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::WindowDeactivate:
        // Clicking elsewhere collapses the panel unless the user pinned it;
        // locking the position has nothing to do with this.
        if (m_expanded && !m_pinned) {
            setExpanded(false);
        }
        break;
    case QEvent::ScreenChangeInternal:
    case QEvent::Resize:
        clampIntoVisibleArea();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

QIcon Hud::hudIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(38, 132, 255));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRect(4, 4, 56, 56), 14, 14);
    painter.setBrush(Qt::white);
    painter.drawPolygon(QPolygonF({QPointF(22, 44), QPointF(32, 24), QPointF(42, 44)}));
    painter.setBrush(QColor(255, 255, 255, 180));
    painter.drawPolygon(QPolygonF({QPointF(22, 20), QPointF(32, 40), QPointF(42, 20)}));
    return QIcon(pixmap);
}

} // namespace ball::ui
