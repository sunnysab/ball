// ball: compact, translucent network traffic HUD for KDE Wayland.
#pragma once

#include "ui/preferences.h"
#include "ui/snapshot.h"

#include <QIcon>
#include <QPoint>
#include <QStringList>
#include <QWidget>

#include <memory>

class QGridLayout;
class QLabel;
class QMenu;
class QScreen;
class QSettings;
class QSystemTrayIcon;

namespace ball::ui {

class Details;
class SnapshotClient;

// Owns one read-only subscription and an independent details window. Closing
// either window never sends commands to, or stops, the backend.
class Hud : public QWidget {
    Q_OBJECT

public:
    Hud(const QString& socketPath, QSettings& settings, QWidget* parent = nullptr);
    ~Hud() override;

    void showDetails();
    Details* detailsWindow() const { return m_details.get(); }

    bool isLocked() const { return m_locked; }
    void setLocked(bool locked);

    Theme theme() const { return m_preferences.theme; }
    void setTheme(Theme theme);
    BarLayout barLayout() const { return m_preferences.barLayout; }
    void setBarLayout(BarLayout layout);

    // Background opacity in [0.2, 1.0]; text stays opaque.
    double opacity() const { return m_opacity; }
    void setOpacity(double opacity);

    QStringList selectedInterfaces() const { return m_selectedInterfaces; }
    void setSelectedInterfaces(const QStringList& names);

    // Layer-shell position is expressed as anchors and margins, not QWidget::pos().
    QPoint requestedPosition() const { return m_position; }
    QString currentScreenName() const;

    bool trayAvailable() const { return m_tray != nullptr; }
    bool isHiddenByUser() const { return m_hiddenByUser; }
    void openContextMenu(const QPoint& globalPos);
    static QIcon hudIcon();

Q_SIGNALS:
    void exitRequested();
    void lockedChanged(bool locked);
    void opacityChanged(double opacity);
    void selectedInterfacesChanged(const QStringList& names);
    void snapshotChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    virtual bool compositorOwnsPosition() const;

private:
    void buildUi();
    void updateBarLayout();
    void buildContextMenu();
    void applySnapshot();
    void applyStatus();
    void ensureDefaultInterfaceSelection();
    void setBackgroundOpacity(double opacity);
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
    std::unique_ptr<Details> m_details;

    QGridLayout* m_bar = nullptr;
    QWidget* m_rxRow = nullptr;
    QWidget* m_txRow = nullptr;
    QLabel* m_rxLabel = nullptr;
    QLabel* m_txLabel = nullptr;
    QLabel* m_rxUnit = nullptr;
    QLabel* m_txUnit = nullptr;
    QLabel* m_statusLabel = nullptr;

    QPoint m_position;
    QString m_screenName;
    QStringList m_selectedInterfaces;
    QList<InterfaceSnapshot> m_knownInterfaces;
    double m_opacity = 0.96;

    QPointF m_pressLocal;
    bool m_pressed = false;
    bool m_dragging = false;
    bool m_locked = false;
    bool m_hiddenByUser = false;
    bool m_layerShellApplied = false;
    bool m_layerShellActive = false;
};

} // namespace ball::ui
