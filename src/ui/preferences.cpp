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
    const QString theme = settings.value(QStringLiteral("hud/theme")).toString();
    if (theme == QStringLiteral("paper")) {
        preferences.theme = Theme::Paper;
    } else if (theme == QStringLiteral("graphite")) {
        preferences.theme = Theme::Graphite;
    }
    if (settings.value(QStringLiteral("hud/layout")).toString() == QStringLiteral("single-line")) {
        preferences.barLayout = BarLayout::SingleLine;
    }
    preferences.detailsGeometry =
        settings.value(QStringLiteral("hud/detailsGeometry")).toByteArray();
    preferences.hasInterfaceSelection = settings.contains(QLatin1String(kInterfaces));
    preferences.interfaces = settings.value(QLatin1String(kInterfaces)).toStringList();
    return preferences;
}

void Preferences::save(QSettings& settings) const {
    settings.beginGroup(QLatin1String(kGroup));
    settings.setValue(QLatin1String("position"), position);
    settings.setValue(QLatin1String("screen"), screenName);
    settings.setValue(QLatin1String("opacity"), opacity);
    const char* themeId = "porcelain";
    if (theme == Theme::Paper) {
        themeId = "paper";
    } else if (theme == Theme::Graphite) {
        themeId = "graphite";
    }
    settings.setValue(QStringLiteral("theme"), QLatin1String(themeId));
    settings.setValue(QStringLiteral("layout"), barLayout == BarLayout::SingleLine
                                                    ? QStringLiteral("single-line")
                                                    : QStringLiteral("two-line"));
    settings.setValue(QStringLiteral("detailsGeometry"), detailsGeometry);
    if (hasInterfaceSelection || !interfaces.isEmpty()) {
        settings.setValue(QLatin1String("interfaces"), interfaces);
    } else {
        settings.remove(QLatin1String("interfaces"));
    }
    settings.endGroup();
}

} // namespace ball::ui
