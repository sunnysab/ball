// ball: a network traffic HUD for KDE Wayland sessions.
// Frameless, translucent HUD with a compact traffic bar and an expandable panel.
#pragma once

#include "ui/preferences.h"
#include "ui/snapshot.h"
#include "ui/traffic_model.h"

#include <QIcon>
#include <QPoint>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QScreen;
class QSettings;
class QSystemTrayIcon;
class QTabWidget;
class QTableView;

namespace ball::ui {

class SnapshotClient;

// The desktop HUD. It owns the read-only snapshot subscription, keeps only the
// newest snapshot and never sends anything to the backend; closing it leaves
// the collector running. `settings` is injected by the caller so tests can use
// a temporary file and the real user preferences stay untouched.
class Hud : public QWidget {
    Q_OBJECT

public:
    Hud(const QString& socketPath, QSettings& settings, QWidget* parent = nullptr);
    ~Hud() override;

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);
    void toggleExpanded();

    // A locked HUD cannot be dragged, so its position stays put. Locking does
    // not affect clicking: the panel still opens and closes.
    bool isLocked() const { return m_locked; }
    void setLocked(bool locked);

    // A pinned HUD keeps its panel open when the window loses activation, for
    // users who want the table visible while working elsewhere.
    bool isPinned() const { return m_pinned; }
    void setPinned(bool pinned);

    // Background opacity in [0.2, 1.0]; text is never made translucent by it.
    double opacity() const { return m_opacity; }
    void setOpacity(double opacity);

    QStringList selectedInterfaces() const { return m_selectedInterfaces; }
    void setSelectedInterfaces(const QStringList& names);

    // Corner the surface is anchored to inside its screen, with margins. This is
    // the layer-shell view of the position the user dragged to.
    QPoint requestedPosition() const { return m_position; }
    QString currentScreenName() const;

    bool trayAvailable() const { return m_tray != nullptr; }
    bool isHiddenByUser() const { return m_hiddenByUser; }

    // Shows the shared context menu (also used by the tray icon). Kept public so
    // the menu can be opened from tests and future shortcuts.
    void openContextMenu(const QPoint& globalPos);

    // Small preview used by the tray icon; drawn in code, no asset files.
    static QIcon hudIcon();

Q_SIGNALS:
    // The user chose "退出 GUI"; the entry point owns the actual quit. The
    // backend is never asked to stop.
    void exitRequested();
    void expandedChanged(bool expanded);
    void lockedChanged(bool locked);
    void pinnedChanged(bool pinned);
    void opacityChanged(double opacity);
    void selectedInterfacesChanged(const QStringList& names);
    // Any change of the data shown, including freshness and error state.
    void snapshotChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

    // True when the compositor owns the surface position (layer-shell on
    // Wayland), so QWidget::pos() is a stale by-product and never the
    // requested position. Offscreen tests override this to exercise the
    // Wayland path without a compositor.
    virtual bool compositorOwnsPosition() const;

private:
    void buildUi();
    void buildContextMenu();
    // Light text on the dark translucent chrome. The palette is local to this
    // widget; the application palette is never touched.
    void applyPalette();
    void applySnapshot();
    void applyStatus();
    void rebuildProcessTable();
    void rebuildInterfaceTable();
    void ensureDefaultInterfaceSelection();
    void setBackgroundOpacity(double opacity);
    // Returns false when no layer shell is in play and the widget has to be
    // moved itself (offscreen tests, non-Wayland platforms).
    bool updateLayerShell();
    QScreen* currentScreen() const;
    void applyPosition(const QPoint& globalTopLeft, bool persist);
    void clampIntoVisibleArea();
    void setHiddenByUser(bool hidden);
    void savePreferences();

    SnapshotClient* m_client = nullptr;
    QSettings* m_settings = nullptr;
    Preferences m_preferences;
    QMenu* m_contextMenu = nullptr;
    QSystemTrayIcon* m_tray = nullptr;

    // Compact bar.
    QLabel* m_rxLabel = nullptr;
    QLabel* m_txLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    // Expanded panel.
    QWidget* m_panel = nullptr;
    QLabel* m_totalUpLabel = nullptr;
    QLabel* m_totalDownLabel = nullptr;
    QLabel* m_todayUpLabel = nullptr;
    QLabel* m_todayDownLabel = nullptr;
    QLabel* m_sessionUpLabel = nullptr;
    QLabel* m_sessionDownLabel = nullptr;
    QLabel* m_startedLabel = nullptr;
    QLabel* m_qualityLabel = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLineEdit* m_processSearch = nullptr;
    QCheckBox* m_showIdle = nullptr;
    QTableView* m_processTable = nullptr;
    QTableView* m_interfaceTable = nullptr;

    QPoint m_position;
    QString m_screenName;
    QStringList m_selectedInterfaces;
    QList<InterfaceSnapshot> m_knownInterfaces;
    double m_opacity = 0.9;

    // Press position in surface-local coordinates. Wayland clients never see a
    // reliable global pointer position for a layer-shell surface, so a drag is
    // derived from this anchor and the authoritative m_position instead.
    QPointF m_pressLocal;
    bool m_pressed = false;
    bool m_dragging = false;
    bool m_expanded = false;
    bool m_locked = false;
    bool m_pinned = false;
    bool m_hiddenByUser = false;
    bool m_layerShellApplied = false;
    bool m_layerShellActive = false;
};

} // namespace ball::ui
