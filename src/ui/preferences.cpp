// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/preferences.h"

namespace ball::ui {
namespace {
constexpr auto kGroup = "hud";
constexpr auto kPosition = "hud/position";
constexpr auto kScreen = "hud/screen";
constexpr auto kOpacity = "hud/opacity";
constexpr auto kInterfaces = "hud/interfaces";

// Keys are stored relative to the "hud" group on purpose; the read path uses
// the full path because QSettings::value() is not inside a group there.
} // namespace

Preferences Preferences::load(QSettings& settings) {
    Preferences preferences;
    preferences.hasPosition = settings.contains(QLatin1String(kPosition));
    preferences.position = settings.value(QLatin1String(kPosition)).toPoint();
    preferences.screenName = settings.value(QLatin1String(kScreen)).toString();
    const double opacity = settings.value(QLatin1String(kOpacity), preferences.opacity).toDouble();
    // A corrupted or hand-edited value must not make the HUD invisible.
    if (opacity >= 0.2 && opacity <= 1.0) {
        preferences.opacity = opacity;
    }
    preferences.hasInterfaceSelection = settings.contains(QLatin1String(kInterfaces));
    preferences.interfaces = settings.value(QLatin1String(kInterfaces)).toStringList();
    return preferences;
}

void Preferences::save(QSettings& settings) const {
    settings.beginGroup(QLatin1String(kGroup));
    settings.setValue(QLatin1String("position"), position);
    settings.setValue(QLatin1String("screen"), screenName);
    settings.setValue(QLatin1String("opacity"), opacity);
    if (hasInterfaceSelection || !interfaces.isEmpty()) {
        settings.setValue(QLatin1String("interfaces"), interfaces);
    } else {
        settings.remove(QLatin1String("interfaces"));
    }
    settings.endGroup();
}

} // namespace ball::ui
