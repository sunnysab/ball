// ball: shared colors for the HUD and the independent details window.
#pragma once

#include <QColor>
#include <QString>

class QWidget;
class QTableView;

namespace ball::ui {

enum class Theme : unsigned char { Porcelain, Paper, Graphite };
enum class BarLayout : unsigned char { TwoLine, SingleLine };

struct ThemeColors {
    QColor background;
    QColor surface;
    QColor text;
    QColor muted;
    QColor border;
    QColor accent;
    QColor download;
    QColor upload;
    QColor warning;
};

ThemeColors themeColors(Theme theme);
QString themeName(Theme theme);
void applyTheme(QWidget& widget, Theme theme);
void enableColumnMenu(QTableView& table);

} // namespace ball::ui
