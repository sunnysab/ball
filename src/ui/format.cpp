// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/format.h"

#include <QLocale>
#include <QRegularExpression>

#include <cmath>

namespace ball::ui {
namespace {

constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
constexpr int kNumberWidth = 7;
constexpr int kRateUnitWidth = 5;
constexpr int kByteUnitWidth = 3;

QString formatValue(double value, bool perSecond) {
    int unit = 0;
    while (value >= 1024.0 && unit < 6) {
        value /= 1024.0;
        ++unit;
    }
    const QString number = QLocale::c().toString(value, 'f', 2).rightJustified(kNumberWidth);
    QString suffix = QString::fromLatin1(kUnits[unit]);
    if (perSecond) {
        suffix += QStringLiteral("/s");
    }
    return number + QLatin1Char(' ') +
           suffix.leftJustified(perSecond ? kRateUnitWidth : kByteUnitWidth);
}

} // namespace

QFont numericFont() {
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QString formatRate(double bytesPerSecond) {
    return std::isfinite(bytesPerSecond) && bytesPerSecond >= 0.0
               ? formatValue(bytesPerSecond, true)
               : unavailableRate();
}

QString formatBytes(quint64 bytes) { return formatValue(static_cast<double>(bytes), false); }

QString unavailableRate() {
    return QStringLiteral("--").rightJustified(kNumberWidth) + QLatin1Char(' ') +
           QString(kRateUnitWidth, QLatin1Char(' '));
}

QString formatCommandLine(const QStringList& arguments) {
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_@%+=:,./-]"));
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (QString argument : arguments) {
        if (argument.isEmpty() || argument.contains(unsafe)) {
            argument.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            argument = QLatin1Char('\'') + argument + QLatin1Char('\'');
        }
        quoted.append(argument);
    }
    return quoted.join(QLatin1Char(' '));
}

} // namespace ball::ui
