// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/hud.h"

#include "ui/autostart.h"
#include "ui/details.h"
#include "ui/format.h"
#include "ui/placement.h"
#include "ui/snapshot_client.h"
#include "ui/traffic_model.h"

#include <LayerShellQt/Window>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QWindow>

#include <utility>

namespace ball::ui {
namespace {

constexpr int kCornerRadius = 12;
constexpr int kSnapDistance = 14;
constexpr int kDefaultMargin = 16;

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

    m_details = std::make_unique<Details>();
    m_details->setWindowIcon(hudIcon());
    if (!m_preferences.detailsGeometry.isEmpty()) {
        m_details->restoreGeometry(m_preferences.detailsGeometry);
    }
    buildUi();
    applyTheme(*this, theme());
    m_details->setTheme(theme());
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

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        savePreferences();
        m_settings->sync();
    });
    applySnapshot();
    applyStatus();
    m_client->start(socketPath);
}

Hud::~Hud() {
    // Persist whatever the user last chose; the backend is not touched.
    savePreferences();
    m_settings->sync();
}

void Hud::buildUi() {
    m_bar = new QGridLayout(this);
    m_bar->setContentsMargins(12, 9, 12, 9);
    m_bar->setHorizontalSpacing(12);
    m_bar->setVerticalSpacing(3);
    const auto makeRow = [this](bool download, QLabel*& value, QLabel*& unit) {
        auto* row = new QWidget(this);
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto* direction = new QLabel(download ? QStringLiteral("↓") : QStringLiteral("↑"), row);
        direction->setProperty("role", download ? "download" : "upload");
        value = new QLabel(QStringLiteral("—"), row);
        value->setObjectName(download ? QStringLiteral("hud.rx") : QStringLiteral("hud.tx"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setAccessibleName(download ? QStringLiteral("下行速率")
                                          : QStringLiteral("上行速率"));
        unit = new QLabel(QStringLiteral("KiB/s"), row);
        unit->setProperty("role", "muted");
        layout->addWidget(direction);
        layout->addWidget(value);
        layout->addWidget(unit);
        return row;
    };
    m_rxRow = makeRow(true, m_rxLabel, m_rxUnit);
    m_txRow = makeRow(false, m_txLabel, m_txUnit);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("hud.status"));
    m_statusLabel->setProperty("role", "warning");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    updateBarLayout();
}

void Hud::updateBarLayout() {
    QFont digits = numericFont();
    digits.setPointSizeF(font().pointSizeF() + 1);
    for (auto* value : {m_rxLabel, m_txLabel}) {
        value->setFont(digits);
        value->setFixedWidth(QFontMetrics(digits).horizontalAdvance(QStringLiteral("1023.99")));
    }
    for (auto* unit : {m_rxUnit, m_txUnit}) {
        int width = 0;
        for (const auto* text : {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s", "PiB/s", "EiB/s"}) {
            width = qMax(width, QFontMetrics(unit->font()).horizontalAdvance(QLatin1String(text)));
        }
        unit->setFixedWidth(width);
    }
    m_statusLabel->setFixedWidth(QFontMetrics(font()).horizontalAdvance(QStringLiteral("过期")) +
                                 4);
    m_bar->removeWidget(m_rxRow);
    m_bar->removeWidget(m_txRow);
    m_bar->removeWidget(m_statusLabel);
    if (barLayout() == BarLayout::TwoLine) {
        m_bar->addWidget(m_rxRow, 0, 0);
        m_bar->addWidget(m_txRow, 1, 0);
        m_bar->addWidget(m_statusLabel, 0, 1, 2, 1);
    } else {
        m_bar->addWidget(m_rxRow, 0, 0);
        m_bar->addWidget(m_txRow, 0, 1);
        m_bar->addWidget(m_statusLabel, 0, 2);
    }
    // The bar never inherits the details window's minimum size or geometry.
    m_bar->invalidate();
    setFixedSize(m_bar->sizeHint());
    m_bar->activate();
}

void Hud::setTheme(Theme theme) {
    if (m_preferences.theme == theme) {
        return;
    }
    m_preferences.theme = theme;
    applyTheme(*this, theme);
    m_details->setTheme(theme);
    updateBarLayout();
    update();
    savePreferences();
}

void Hud::setBarLayout(BarLayout layout) {
    if (m_preferences.barLayout == layout) {
        return;
    }
    m_preferences.barLayout = layout;
    updateBarLayout();
    savePreferences();
}

void Hud::showDetails() {
    if (m_details->isMinimized()) {
        m_details->showNormal();
    } else {
        m_details->show();
    }
    m_details->raise();
    m_details->activateWindow();
}

void Hud::buildContextMenu() {
    if (m_contextMenu == nullptr) {
        m_contextMenu = new QMenu(this);
        m_contextMenu->setObjectName(QStringLiteral("hud.menu"));
    }
    // QMenu::clear() removes actions, but does not own the child menus' lifetimes.
    qDeleteAll(m_contextMenu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
    m_contextMenu->clear();
    auto* detailsAction = m_contextMenu->addAction(QStringLiteral("打开流量详情"));
    detailsAction->setObjectName(QStringLiteral("hud.action.details"));
    connect(detailsAction, &QAction::triggered, this, &Hud::showDetails);
    m_contextMenu->addSeparator();

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

    auto* lockAction = m_contextMenu->addAction(QStringLiteral("锁定流量条位置"));
    lockAction->setObjectName(QStringLiteral("hud.action.lock"));
    lockAction->setCheckable(true);
    lockAction->setChecked(m_locked);
    connect(lockAction, &QAction::toggled, this, &Hud::setLocked);

    auto* appearance = m_contextMenu->addMenu(QStringLiteral("外观"));
    auto* themes = appearance->addMenu(QStringLiteral("皮肤"));
    auto* themeGroup = new QActionGroup(themes);
    for (const Theme preset : {Theme::Porcelain, Theme::Paper, Theme::Graphite}) {
        auto* action = themes->addAction(themeName(preset));
        action->setObjectName(QStringLiteral("hud.action.theme.%1").arg(int(preset)));
        action->setCheckable(true);
        action->setChecked(theme() == preset);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, preset] { setTheme(preset); });
    }
    auto* layouts = appearance->addMenu(QStringLiteral("布局"));
    auto* layoutGroup = new QActionGroup(layouts);
    for (const BarLayout preset : {BarLayout::TwoLine, BarLayout::SingleLine}) {
        auto* action = layouts->addAction(preset == BarLayout::TwoLine ? QStringLiteral("双行")
                                                                       : QStringLiteral("单行"));
        action->setObjectName(QStringLiteral("hud.action.layout.%1").arg(int(preset)));
        action->setCheckable(true);
        action->setChecked(barLayout() == preset);
        layoutGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, preset] { setBarLayout(preset); });
    }
    auto* opacityMenu = appearance->addMenu(QStringLiteral("背景不透明度"));
    opacityMenu->setObjectName(QStringLiteral("hud.action.opacity"));
    auto* opacityGroup = new QActionGroup(opacityMenu);
    for (const int percent : {50, 70, 85, 96, 100}) {
        auto* action = opacityMenu->addAction(QStringLiteral("%1%").arg(percent));
        action->setObjectName(QStringLiteral("hud.action.opacity.%1").arg(percent));
        action->setCheckable(true);
        action->setChecked(qRound(m_opacity * 100.0) == percent);
        opacityGroup->addAction(action);
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
    auto* quitAction = m_contextMenu->addAction(QStringLiteral("退出界面"));
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
    const auto setRate = [usable](QLabel* value, QLabel* unit, double rate) {
        const QString formatted = formatRate(rate).simplified();
        value->setText(usable ? formatted.section(QLatin1Char(' '), 0, 0) : QStringLiteral("—"));
        unit->setText(usable ? formatted.section(QLatin1Char(' '), 1, 1) : QStringLiteral("KiB/s"));
    };
    setRate(m_rxLabel, m_rxUnit, totals.rxBps);
    setRate(m_txLabel, m_txUnit, totals.txBps);
    m_details->setSnapshot(snapshot, fresh, m_selectedInterfaces);
}

void Hud::applyStatus() {
    const auto& snapshot = m_client->snapshot();
    const auto totals = snapshot.has_value() && snapshot->interfacesAvailable
                            ? sumSelectedInterfaces(snapshot->interfaces, m_selectedInterfaces)
                            : InterfaceTotals{};
    QString summary;
    QString marker;
    QStringList reasons;
    if (!snapshot.has_value()) {
        marker = QStringLiteral("等待");
        summary = QStringLiteral("等待后台采集数据");
    } else if (!m_client->fresh()) {
        marker = QStringLiteral("过期");
        summary = QStringLiteral("采集连接中断或数据已过期；当前速率不可用");
    } else if (!snapshot->interfacesAvailable) {
        marker = QStringLiteral("异常");
        summary = QStringLiteral("网卡采集不可用");
    } else if (m_selectedInterfaces.isEmpty()) {
        marker = QStringLiteral("未选");
        summary = QStringLiteral("未选择统计网卡");
    } else if (!totals.valid) {
        marker = QStringLiteral("异常");
        summary = QStringLiteral("所选网卡统计不可用");
    } else if (!totals.complete) {
        marker = QStringLiteral("!");
        summary = QStringLiteral("部分所选网卡缺失；当前仅汇总可用网卡");
    }
    if (!m_client->error().isEmpty()) {
        reasons.append(m_client->error() == QStringLiteral("backend connection closed")
                           ? QStringLiteral("后台连接已关闭，界面正在自动重连")
                           : m_client->error());
    }
    if (snapshot.has_value()) {
        if (!snapshot->interfaceError.isEmpty()) {
            reasons.append(QStringLiteral("网卡采集：%1").arg(snapshot->interfaceError));
        }
        const auto& process = snapshot->processStatus;
        QString processSummary;
        if (!m_client->fresh() || !process.available || process.stale || !process.error.isEmpty()) {
            processSummary = QStringLiteral("进程采集不可用或已过期；不代表网卡速率不可用");
        } else if (process.incomplete || !process.notes.isEmpty()) {
            processSummary = process.notes.isEmpty()
                                 ? QStringLiteral("进程当前采集正常，但累计存在历史缺口")
                                 : QStringLiteral("进程统计存在丢失或未归属的数据");
        }
        if (!processSummary.isEmpty()) {
            if (summary.isEmpty()) {
                summary = std::move(processSummary);
                marker = QStringLiteral("!");
            } else {
                reasons.append(processSummary);
            }
            reasons.append(QStringLiteral("进程统计与网卡统计口径不同；缺失的累计数据无法补回。"));
        }
        if (!process.error.isEmpty()) {
            reasons.append(
                process.error == QStringLiteral("collector connection closed") ||
                        process.error.contains(QStringLiteral("Remote closed"))
                    ? QStringLiteral("采集器关闭了订阅连接，后台正在重连；进程速率暂不可用")
                    : QStringLiteral("进程采集：%1").arg(process.error));
        }
        reasons.append(process.notes);
    }
    if (!summary.isEmpty() && reasons.isEmpty()) {
        reasons.append(summary);
    }
    m_statusLabel->setText(marker);
    m_statusLabel->setAccessibleName(summary.isEmpty() ? QStringLiteral("采集正常") : summary);
    m_details->setStatus(summary, reasons);

    QStringList tip{QStringLiteral("所选网卡：%1")
                        .arg(m_selectedInterfaces.isEmpty()
                                 ? QStringLiteral("未选择")
                                 : m_selectedInterfaces.join(QStringLiteral("、")))};
    if (m_client->fresh() && totals.valid) {
        tip.append(QStringLiteral("今日：↓ %1 · ↑ %2")
                       .arg(formatBytes(totals.today.rxBytes).trimmed(),
                            formatBytes(totals.today.txBytes).trimmed()));
    }
    if (!summary.isEmpty()) {
        tip.append(summary);
    }
    tip.append(QStringLiteral("单击打开流量详情"));
    setToolTip(tip.join(QLatin1Char('\n'))
                   .toHtmlEscaped()
                   .replace(QLatin1Char('\n'), QStringLiteral("<br>")));
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
    applyStatus();
    Q_EMIT selectedInterfacesChanged(m_selectedInterfaces);
}

void Hud::setLocked(bool locked) {
    if (m_locked == locked) {
        return;
    }
    m_locked = locked;
    Q_EMIT lockedChanged(m_locked);
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
    if (m_details != nullptr) {
        m_preferences.detailsGeometry = m_details->saveGeometry();
    }
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
    const QScreen* screen = QGuiApplication::screenAt(globalTopLeft);
    if (screen == nullptr) {
        screen = currentScreen();
    }
    if (screen != nullptr) {
        m_screenName = screen->name();
    }
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
    // Details use an ordinary toplevel; the HUD never takes keyboard focus.
    shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    if (screen != nullptr) {
        shell->setScreen(const_cast<QScreen*>(screen));
    }
    return true;
}

bool Hud::compositorOwnsPosition() const {
    // Wayland positions are never authoritative, including before the first
    // layer-shell configure; adopting that startup move overwrites saved state.
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

void Hud::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    clampIntoVisibleArea();
    updateLayerShell();
    m_surfaceShown = true;
}

void Hud::closeEvent(QCloseEvent* event) {
    savePreferences();
    m_settings->sync();
    QWidget::closeEvent(event);
}

void Hud::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    // On a layer-shell surface the compositor places the window from the
    // anchors/margins we request, so QWidget::pos() is a layout by-product and
    // never the requested position. Adopting it here is what made a drag snap
    // back (or to the origin) on Wayland; m_position stays authoritative there.
    if (!m_surfaceShown || compositorOwnsPosition() || m_dragging) {
        return;
    }
    m_position = pos();
    savePreferences();
}

void Hud::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kCornerRadius,
                        kCornerRadius);
    // Only the background is translucent; child labels keep fully opaque text.
    const auto colors = themeColors(theme());
    QColor background = colors.background;
    background.setAlphaF(static_cast<float>(m_opacity));
    painter.fillPath(path, background);
    painter.setPen(QPen(colors.border, 1));
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
    // Locking prevents movement, not opening details.
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
        // a click, so it never opens details.
        if (!m_locked) {
            const QScreen* screen = currentScreen();
            const QRect screenRect =
                screen != nullptr ? screen->geometry() : QRect(m_position, size());
            applyPosition(snappedPosition(QRect(m_position, size()), screenRect, kSnapDistance),
                          true);
        }
    } else {
        showDetails();
    }
    m_dragging = false;
}

bool Hud::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Hide:
        m_surfaceShown = false;
        break;
    case QEvent::FontChange:
        if (m_statusLabel != nullptr) {
            updateBarLayout();
        }
        break;
    case QEvent::ScreenChangeInternal:
    case QEvent::Resize:
        if (isVisible() && m_bar != nullptr) {
            clampIntoVisibleArea();
        }
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
