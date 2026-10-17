// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/connections.h"

#include "ui/format.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <vector>

namespace ball::ui {
namespace {

// Columns in display order. The header text is user facing, so it stays here.
enum Column {
    ColName = 0,
    ColProtocol,
    ColSource,
    ColTarget,
    ColProcess,
    ColState,
    ColRxRate,
    ColTxRate,
    ColIpBytes,
    ColRtt,
    ColRetransmits,
    ColumnCount,
};

enum Metric {
    MetricTotalRate = 0,
    MetricIpBytes,
    MetricRxRate,
    MetricTxRate,
    MetricRetransmits,
};

// Raw numeric value behind a formatted cell, so a test can check that the
// ranking uses the source value and not the display string.
constexpr int RawValueRole = Qt::UserRole + 1;

// Exact 65-bit sum of two uint64 counters. A double would lose the low bits of
// a large total, and a plain uint64 addition would wrap.
struct WideValue {
    quint64 high = 0;
    quint64 low = 0;

    friend bool operator<(const WideValue& a, const WideValue& b) {
        return a.high != b.high ? a.high < b.high : a.low < b.low;
    }
};

WideValue addWide(quint64 a, quint64 b) {
    const quint64 low = a + b;
    return WideValue{low < a ? 1U : 0U, low};
}

QString formatRtt(qint64 microseconds) {
    if (microseconds <= 0) {
        return QStringLiteral("--");
    }
    return QLocale::c().toString(double(microseconds) / 1000.0, 'f', 2) + QStringLiteral(" ms");
}

QString formatTime(const QDateTime& time) {
    return time.isValid()
               ? QLocale::c().toString(time.toLocalTime(), QStringLiteral("yyyy-MM-dd HH:mm:ss"))
               : QStringLiteral("--");
}

QString joinProcessNames(const ConnectionSnapshot& row) {
    QStringList names;
    const auto add = [&names](const ConnectionProcess& process) {
        if (!process.name.isEmpty() && !names.contains(process.name)) {
            names.append(process.name);
        }
    };
    add(row.client);
    add(row.server);
    for (const ConnectionProcess& process : row.io) {
        add(process);
    }
    return names.isEmpty() ? QStringLiteral("未知") : names.join(QStringLiteral(", "));
}

// One process identity in the detail text, with its socket byte counters.
QString describeProcess(const ConnectionProcess& process) {
    if (process.pid == 0 && process.name.isEmpty()) {
        return QStringLiteral("  未知");
    }
    QString text = QStringLiteral("  %1 (PID %2, start_ns %3)")
                       .arg(process.name.isEmpty() ? QStringLiteral("未知") : process.name)
                       .arg(process.pid)
                       .arg(process.startNs);
    if (process.pid < 0) {
        text = QStringLiteral("  %1 (PID 歧义)")
                   .arg(process.name.isEmpty() ? QStringLiteral("未知") : process.name);
    }
    text += QStringLiteral("\n    socket RX %1 / TX %2")
                .arg(formatBytes(process.rxBytes), formatBytes(process.txBytes));
    return text;
}

} // namespace

ConnectionsView::ConnectionsView(QWidget* parent) : QWidget(parent) { buildUi(); }

void ConnectionsView::buildUi() {
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("connections.search"));
    m_search->setPlaceholderText(QStringLiteral("搜索端点、域名、应用、进程或 PID"));
    m_search->setClearButtonEnabled(true);

    m_protocol = new QComboBox(this);
    m_protocol->setObjectName(QStringLiteral("connections.protocol"));
    m_protocol->addItem(QStringLiteral("全部协议"), QString());

    m_state = new QComboBox(this);
    m_state->setObjectName(QStringLiteral("connections.state"));
    m_state->addItem(QStringLiteral("活动"), 0);
    m_state->addItem(QStringLiteral("已结束"), 1);
    m_state->addItem(QStringLiteral("全部"), 2);
    m_state->setCurrentIndex(2);

    m_metric = new QComboBox(this);
    m_metric->setObjectName(QStringLiteral("connections.metric"));
    m_metric->addItem(QStringLiteral("总速率"), MetricTotalRate);
    m_metric->addItem(QStringLiteral("累计 IP 字节"), MetricIpBytes);
    m_metric->addItem(QStringLiteral("RX 速率"), MetricRxRate);
    m_metric->addItem(QStringLiteral("TX 速率"), MetricTxRate);
    m_metric->addItem(QStringLiteral("重传"), MetricRetransmits);

    m_limit = new QSpinBox(this);
    m_limit->setObjectName(QStringLiteral("connections.limit"));
    m_limit->setRange(0, connection::kMaxRows);
    m_limit->setValue(20);
    m_limit->setSpecialValueText(QStringLiteral("全部"));
    m_limit->setToolTip(QStringLiteral("0 显示全部，最多 %1 条").arg(connection::kMaxRows));

    m_pause = new QToolButton(this);
    m_pause->setObjectName(QStringLiteral("connections.pause"));
    m_pause->setText(QStringLiteral("暂停"));
    m_pause->setCheckable(true);

    m_copy = new QToolButton(this);
    m_copy->setObjectName(QStringLiteral("connections.copy"));
    m_copy->setText(QStringLiteral("复制"));
    m_copy->setEnabled(false);

    m_processFilter = new QLabel(this);
    m_processFilter->setObjectName(QStringLiteral("connections.processFilter"));
    m_processFilter->setTextFormat(Qt::PlainText);
    m_processFilter->setVisible(false);

    m_clearProcess = new QToolButton(this);
    m_clearProcess->setObjectName(QStringLiteral("connections.clearProcess"));
    m_clearProcess->setText(QStringLiteral("清除进程筛选"));
    m_clearProcess->setVisible(false);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("connections.summary"));
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("connections.table"));
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("名称"), QStringLiteral("协议"), QStringLiteral("来源"),
         QStringLiteral("目标"), QStringLiteral("进程"), QStringLiteral("状态"),
         QStringLiteral("采集 RX 速率"), QStringLiteral("采集 TX 速率"),
         QStringLiteral("累计 IP 字节"), QStringLiteral("RTT"), QStringLiteral("重传")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(ColName, 180);
    m_table->setColumnWidth(ColProtocol, 80);
    m_table->setColumnWidth(ColSource, 200);
    m_table->setColumnWidth(ColTarget, 200);
    m_table->setColumnWidth(ColProcess, 140);
    m_table->setColumnWidth(ColState, 80);
    m_table->setColumnWidth(ColRxRate, 110);
    m_table->setColumnWidth(ColTxRate, 110);
    m_table->setColumnWidth(ColIpBytes, 110);
    m_table->setColumnWidth(ColRtt, 80);
    m_table->setColumnWidth(ColRetransmits, 70);

    m_details = new QPlainTextEdit(this);
    m_details->setObjectName(QStringLiteral("connections.details"));
    m_details->setReadOnly(true);
    m_details->setPlaceholderText(QStringLiteral("选择一条连接查看详情"));

    auto* filters = new QHBoxLayout;
    filters->addWidget(m_search, 1);
    filters->addWidget(m_protocol);
    filters->addWidget(m_state);
    filters->addWidget(m_metric);
    filters->addWidget(new QLabel(QStringLiteral("Top"), this));
    filters->addWidget(m_limit);
    filters->addWidget(m_pause);
    filters->addWidget(m_copy);

    auto* processRow = new QHBoxLayout;
    processRow->addWidget(m_processFilter);
    processRow->addWidget(m_clearProcess);
    processRow->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(filters);
    layout->addLayout(processRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_table, 2);
    layout->addWidget(m_details, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this] { scheduleRebuild(); });
    connect(m_protocol, &QComboBox::currentIndexChanged, this, [this] { scheduleRebuild(); });
    connect(m_state, &QComboBox::currentIndexChanged, this, [this] { scheduleRebuild(); });
    connect(m_metric, &QComboBox::currentIndexChanged, this, [this] { scheduleRebuild(); });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
        int metric = -1;
        switch (column) {
        case ColRxRate:
            metric = MetricRxRate;
            break;
        case ColTxRate:
            metric = MetricTxRate;
            break;
        case ColIpBytes:
            metric = MetricIpBytes;
            break;
        case ColRetransmits:
            metric = MetricRetransmits;
            break;
        default:
            return;
        }
        m_metric->setCurrentIndex(m_metric->findData(metric));
        m_table->horizontalHeader()->setSortIndicator(column, Qt::DescendingOrder);
        m_table->horizontalHeader()->setSortIndicatorShown(true);
    });
    connect(m_limit, &QSpinBox::valueChanged, this, [this] { scheduleRebuild(); });
    connect(m_clearProcess, &QToolButton::clicked, this, &ConnectionsView::clearProcessFilter);
    connect(m_copy, &QToolButton::clicked, this, &ConnectionsView::copySelection);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &ConnectionsView::updateDetails);
    connect(m_pause, &QToolButton::toggled, this, [this](bool paused) {
        m_paused = paused;
        m_pause->setText(paused ? QStringLiteral("恢复") : QStringLiteral("暂停"));
        if (!paused) {
            applySnapshot();
        } else {
            rebuild();
        }
    });

    auto* copyShortcut = new QShortcut(QKeySequence::Copy, this);
    connect(copyShortcut, &QShortcut::activated, this, &ConnectionsView::copySelection);
}

void ConnectionsView::setSnapshot(const std::optional<Snapshot>& snapshot, bool fresh) {
    m_latest = snapshot;
    if (m_paused) {
        // The frozen rows stay untouched; only the note about staleness changes.
        m_stale = !fresh || !snapshot || !snapshot->connectionStatus.available ||
                  snapshot->connectionStatus.stale;
        scheduleRebuild();
        return;
    }
    m_stale = !fresh || !snapshot || !snapshot->connectionStatus.available ||
              snapshot->connectionStatus.stale;
    applySnapshot();
}

void ConnectionsView::applySnapshot() {
    m_applied = m_latest;
    m_dirty = true;
    if (isVisible()) {
        rebuild();
    }
}

void ConnectionsView::scheduleRebuild() {
    m_dirty = true;
    // Hidden pages rebuild on show, so a burst of filter edits on an unmounted
    // tab costs nothing.
    if (isVisible()) {
        rebuild();
    }
}

void ConnectionsView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_dirty) {
        rebuild();
    }
}

void ConnectionsView::refreshProtocolChoices(const QList<ConnectionSnapshot>& rows) {
    const QString current = m_protocol->currentData().toString();
    QStringList protocols;
    for (const ConnectionSnapshot& row : rows) {
        if (!row.protocol.isEmpty() && !protocols.contains(row.protocol)) {
            protocols.append(row.protocol);
        }
    }
    if (!current.isEmpty() && !protocols.contains(current)) {
        protocols.append(current);
    }
    protocols.sort();
    m_protocol->blockSignals(true);
    m_protocol->clear();
    m_protocol->addItem(QStringLiteral("全部协议"), QString());
    for (const QString& protocol : protocols) {
        m_protocol->addItem(protocol, protocol);
    }
    const int index = m_protocol->findData(current);
    m_protocol->setCurrentIndex(index < 0 ? 0 : index);
    m_protocol->blockSignals(false);
}

bool ConnectionsView::matchesText(const ConnectionSnapshot& row) const {
    const QString needle = m_search->text().trimmed();
    if (needle.isEmpty()) {
        return true;
    }
    const auto contains = [&needle](const QString& value) {
        return !value.isEmpty() && value.contains(needle, Qt::CaseInsensitive);
    };
    const auto processContains = [&](const ConnectionProcess& process) {
        return contains(process.name) || QString::number(process.pid).contains(needle);
    };
    if (contains(row.source) || contains(row.target) || contains(row.name) || contains(row.app) ||
        processContains(row.client) || processContains(row.server)) {
        return true;
    }
    for (const ConnectionProcess& process : row.io) {
        if (processContains(process)) {
            return true;
        }
    }
    return false;
}

bool ConnectionsView::matchesProcessFilter(const ConnectionSnapshot& row) const {
    if (!m_hasProcessFilter) {
        return true;
    }
    const auto matches = [this](const ConnectionProcess& process) {
        return process.pid == m_filterPid && process.startNs == m_filterStartNs;
    };
    if (matches(row.client) || matches(row.server)) {
        return true;
    }
    for (const ConnectionProcess& process : row.io) {
        if (matches(process)) {
            return true;
        }
    }
    return false;
}

bool ConnectionsView::matchesFilters(const ConnectionSnapshot& row) const {
    const QString protocol = m_protocol->currentData().toString();
    if (!protocol.isEmpty() && row.protocol != protocol) {
        return false;
    }
    switch (m_state->currentData().toInt()) {
    case 0:
        if (!row.end.isEmpty()) {
            return false;
        }
        break;
    case 1:
        if (row.end.isEmpty()) {
            return false;
        }
        break;
    default:
        break;
    }
    return matchesText(row) && matchesProcessFilter(row);
}

void ConnectionsView::rebuild() {
    m_dirty = false;
    m_table->blockSignals(true);
    const quint64 previousSelection = selectedId();

    if (!m_applied) {
        m_table->setRowCount(0);
        m_table->blockSignals(false);
        m_details->clear();
        m_copy->setEnabled(false);
        m_summary->setText(QStringLiteral("未收到连接数据"));
        return;
    }

    const Snapshot& snapshot = *m_applied;
    const QList<ConnectionSnapshot>& all = snapshot.connections;
    refreshProtocolChoices(all);

    std::vector<const ConnectionSnapshot*> filtered;
    filtered.reserve(size_t(all.size()));
    for (const ConnectionSnapshot& row : all) {
        if (matchesFilters(row)) {
            filtered.push_back(&row);
        }
    }

    const int metric = m_metric->currentData().toInt();
    const int sortedColumn = metric == MetricIpBytes       ? ColIpBytes
                             : metric == MetricRxRate      ? ColRxRate
                             : metric == MetricTxRate      ? ColTxRate
                             : metric == MetricRetransmits ? ColRetransmits
                                                           : -1;
    m_table->horizontalHeader()->setSortIndicatorShown(sortedColumn >= 0);
    if (sortedColumn >= 0) {
        m_table->horizontalHeader()->setSortIndicator(sortedColumn, Qt::DescendingOrder);
    }
    const auto metricLess = [metric](const ConnectionSnapshot* a, const ConnectionSnapshot* b) {
        switch (metric) {
        case MetricIpBytes: {
            const WideValue sumA = addWide(a->rxBytes, a->txBytes);
            const WideValue sumB = addWide(b->rxBytes, b->txBytes);
            if (sumA < sumB) {
                return false;
            }
            if (sumB < sumA) {
                return true;
            }
            break;
        }
        case MetricRxRate:
            if (a->rxBps != b->rxBps) {
                return a->rxBps > b->rxBps;
            }
            break;
        case MetricTxRate:
            if (a->txBps != b->txBps) {
                return a->txBps > b->txBps;
            }
            break;
        case MetricRetransmits:
            if (a->retransmits != b->retransmits) {
                return a->retransmits > b->retransmits;
            }
            break;
        default: {
            const double rateA = a->rxBps + a->txBps;
            const double rateB = b->rxBps + b->txBps;
            if (rateA != rateB) {
                return rateA > rateB;
            }
            break;
        }
        }
        // A stable, deterministic ordering for equal metrics: by ID ascending.
        return a->id < b->id;
    };
    std::stable_sort(filtered.begin(), filtered.end(), metricLess);

    const int limit = m_limit->value();
    const int shown =
        limit == 0 ? int(filtered.size()) : std::min<int>(limit, int(filtered.size()));

    m_table->setRowCount(shown);
    const QFont numbers = numericFont();
    int selectedRow = -1;
    for (int i = 0; i < shown; ++i) {
        const ConnectionSnapshot& row = *filtered[size_t(i)];
        if (row.id == previousSelection) {
            selectedRow = i;
        }
        const auto setCell = [this, i](int column, const QString& text) {
            m_table->setItem(i, column, new QTableWidgetItem(text));
        };
        setCell(ColName, row.name.isEmpty() ? row.app : row.name);
        // The ID is not a visible column, so the name cell carries it.
        m_table->item(i, ColName)->setData(Qt::UserRole, QVariant::fromValue(row.id));
        setCell(ColProtocol, row.protocol);
        setCell(ColSource, row.source);
        setCell(ColTarget, row.target);
        setCell(ColProcess, joinProcessNames(row));
        setCell(ColState, row.end.isEmpty()
                              ? (row.state.isEmpty() ? QStringLiteral("活动") : row.state)
                              : row.end);

        // A frozen or expired row must never show a stale number as a live rate.
        const bool liveRates = !m_stale && !m_paused;
        QTableWidgetItem* rx =
            new QTableWidgetItem(liveRates ? formatRate(row.rxBps) : unavailableRate());
        rx->setFont(numbers);
        rx->setData(RawValueRole, row.rxBps);
        m_table->setItem(i, ColRxRate, rx);

        QTableWidgetItem* tx =
            new QTableWidgetItem(liveRates ? formatRate(row.txBps) : unavailableRate());
        tx->setFont(numbers);
        tx->setData(RawValueRole, row.txBps);
        m_table->setItem(i, ColTxRate, tx);

        const WideValue total = addWide(row.rxBytes, row.txBytes);
        const QString totalText =
            total.high == 0 ? formatBytes(total.low)
                            : QStringLiteral("%1 + %2").arg(formatBytes(row.rxBytes).trimmed(),
                                                            formatBytes(row.txBytes).trimmed());
        QTableWidgetItem* bytes = new QTableWidgetItem(totalText);
        bytes->setFont(numbers);
        bytes->setData(RawValueRole, QVariantList{QVariant::fromValue(total.high),
                                                  QVariant::fromValue(total.low)});
        m_table->setItem(i, ColIpBytes, bytes);

        QTableWidgetItem* rtt = new QTableWidgetItem(formatRtt(row.rttUs));
        rtt->setFont(numbers);
        rtt->setData(RawValueRole, QVariant::fromValue(row.rttUs));
        m_table->setItem(i, ColRtt, rtt);

        QTableWidgetItem* retransmits = new QTableWidgetItem(QString::number(row.retransmits));
        retransmits->setFont(numbers);
        retransmits->setData(RawValueRole, QVariant::fromValue(row.retransmits));
        m_table->setItem(i, ColRetransmits, retransmits);
    }
    if (selectedRow >= 0) {
        m_table->selectRow(selectedRow);
    } else {
        m_table->clearSelection();
    }
    m_table->blockSignals(false);

    updateSummary(shown, int(filtered.size()), int(all.size()));
    updateDetails();
}

quint64 ConnectionsView::selectedId() const {
    const auto selected = m_table->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return 0;
    }
    const QTableWidgetItem* item = m_table->item(selected.first().row(), ColName);
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

void ConnectionsView::updateSummary(int visibleCount, int matchedCount, int totalCount) {
    QStringList parts;
    if (!m_applied) {
        parts.append(QStringLiteral("未收到连接数据"));
    } else if (totalCount == 0) {
        parts.append(
            m_applied->connectionStatus.available
                ? QStringLiteral("暂无连接")
                : QStringLiteral("未收到 flow 数据；采集器需启用 --socket-kinds stats,flow"));
    } else if (visibleCount == 0) {
        parts.append(QStringLiteral("无匹配连接"));
    } else {
        parts.append(QStringLiteral("显示 %1 条 · 匹配 %2 条 · 缓存 %3 条")
                         .arg(visibleCount)
                         .arg(matchedCount)
                         .arg(totalCount));
    }
    if (m_applied && m_applied->at.isValid()) {
        parts.append(QStringLiteral("数据时刻 %1").arg(formatTime(m_applied->at)));
    }
    if (m_paused) {
        parts.append(QStringLiteral("已暂停"));
    }
    if (m_stale) {
        parts.append(QStringLiteral("数据已过期"));
    }
    if (m_applied && m_applied->connectionStatus.truncated) {
        parts.append(QStringLiteral("缓存已截断，非全量历史"));
    }
    m_summary->setText(parts.join(QStringLiteral(" · ")));
}

void ConnectionsView::updateDetails() {
    const quint64 id = selectedId();
    if (id == 0 || !m_applied) {
        m_copy->setEnabled(false);
        m_details->clear();
        return;
    }
    const ConnectionSnapshot* found = nullptr;
    for (const ConnectionSnapshot& row : m_applied->connections) {
        if (row.id == id) {
            found = &row;
            break;
        }
    }
    if (!found) {
        m_copy->setEnabled(false);
        m_details->clear();
        return;
    }
    const ConnectionSnapshot& row = *found;
    m_copy->setEnabled(true);
    QStringList lines;
    lines << QStringLiteral("ID: %1").arg(row.id);
    lines << QStringLiteral("应用: %1").arg(row.app.isEmpty() ? QStringLiteral("未知") : row.app);
    if (!row.name.isEmpty()) {
        lines << QStringLiteral("域名/名称: %1").arg(row.name);
    }
    lines << QStringLiteral("协议: %1").arg(row.protocol);
    lines << QStringLiteral("状态: %1")
                 .arg(row.end.isEmpty() ? (row.state.isEmpty() ? QStringLiteral("活动") : row.state)
                                        : row.end);
    lines << QStringLiteral("方向: %1").arg(row.direction);
    lines << QStringLiteral("RTT: %1 (%2)").arg(formatRtt(row.rttUs), row.rttSource);
    lines << QStringLiteral("报文: %1 · 重传: %2").arg(row.packets).arg(row.retransmits);
    lines << QStringLiteral("来源: %1").arg(row.source);
    lines << QStringLiteral("目标: %1").arg(row.target);
    lines << QStringLiteral("接口: %1")
                 .arg(row.interfaces.isEmpty() ? QStringLiteral("未知")
                                               : row.interfaces.join(QStringLiteral(", ")));
    lines << QStringLiteral("采集方向字节: RX %1 / TX %2")
                 .arg(formatBytes(row.rxBytes), formatBytes(row.txBytes));
    lines << QStringLiteral("开始: %1").arg(formatTime(row.firstSeen));
    lines << QStringLiteral("最近: %1").arg(formatTime(row.lastSeen));
    if (!row.detail.isEmpty()) {
        lines << QStringLiteral("说明: %1").arg(row.detail);
    }
    lines << QStringLiteral("客户端进程:") << describeProcess(row.client);
    lines << QStringLiteral("服务端进程:") << describeProcess(row.server);
    lines << QStringLiteral("I/O 进程:");
    if (row.io.isEmpty()) {
        lines << QStringLiteral("  未知");
    } else {
        for (const ConnectionProcess& process : row.io) {
            lines << describeProcess(process);
        }
    }
    lines
        << QStringLiteral("结束原因: %1").arg(row.end.isEmpty() ? QStringLiteral("活动") : row.end);
    m_details->setPlainText(lines.join(QLatin1Char('\n')));
}

void ConnectionsView::copySelection() {
    if (!m_details->toPlainText().isEmpty()) {
        QApplication::clipboard()->setText(m_details->toPlainText());
    }
}

void ConnectionsView::setProcessFilter(int pid, quint64 startNs, const QString& name) {
    m_hasProcessFilter = true;
    m_filterPid = pid;
    m_filterStartNs = startNs;
    m_processFilter->setText(QStringLiteral("进程筛选: %1 (PID %2)")
                                 .arg(name.isEmpty() ? QStringLiteral("未知") : name)
                                 .arg(pid));
    m_processFilter->setVisible(true);
    m_clearProcess->setVisible(true);
    scheduleRebuild();
}

void ConnectionsView::clearProcessFilter() {
    m_hasProcessFilter = false;
    m_filterPid = 0;
    m_filterStartNs = 0;
    m_processFilter->clear();
    m_processFilter->setVisible(false);
    m_clearProcess->setVisible(false);
    scheduleRebuild();
}

} // namespace ball::ui
