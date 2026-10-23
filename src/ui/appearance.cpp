// ball: built-in color presets, independent of layout and desktop palette.
#include "ui/appearance.h"

#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QPalette>
#include <QTableView>
#include <QWidget>

namespace ball::ui {

ThemeColors themeColors(Theme theme) {
    switch (theme) {
    case Theme::Paper:
        return {QColor("#faf7f2"), QColor("#fffcf7"), QColor("#302c27"),
                QColor("#71675b"), QColor("#ddd5c8"), QColor("#27695f"),
                QColor("#27695f"), QColor("#79592f"), QColor("#8b5816")};
    case Theme::Graphite:
        return {QColor("#20252d"), QColor("#282f39"), QColor("#eef2f6"),
                QColor("#b0bac8"), QColor("#46515f"), QColor("#8dbaff"),
                QColor("#8dbaff"), QColor("#8ad4bb"), QColor("#f2c27b")};
    case Theme::Porcelain:
        return {QColor("#f8fafc"), QColor("#ffffff"), QColor("#172033"),
                QColor("#647084"), QColor("#d9e0e9"), QColor("#285fae"),
                QColor("#285fae"), QColor("#317568"), QColor("#8b5b16")};
    }
    Q_UNREACHABLE();
}

QString themeName(Theme theme) {
    switch (theme) {
    case Theme::Porcelain:
        return QStringLiteral("瓷白");
    case Theme::Paper:
        return QStringLiteral("暖纸");
    case Theme::Graphite:
        return QStringLiteral("石墨");
    }
    Q_UNREACHABLE();
}

void applyTheme(QWidget& widget, Theme theme) {
    const auto colors = themeColors(theme);
    // Remove the stylesheet palette cache before applying a different preset.
    widget.setStyleSheet({});
    QPalette palette = widget.palette();
    palette.setColor(QPalette::Window, colors.background);
    palette.setColor(QPalette::Base, colors.surface);
    palette.setColor(QPalette::AlternateBase, colors.background);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Button, colors.surface);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::PlaceholderText, colors.muted);
    palette.setColor(QPalette::ToolTipBase, colors.surface);
    palette.setColor(QPalette::ToolTipText, colors.text);
    palette.setColor(QPalette::Highlight, colors.accent);
    palette.setColor(QPalette::HighlightedText,
                     theme == Theme::Graphite ? colors.background : QColor(Qt::white));
    palette.setColor(QPalette::Disabled, QPalette::Text, colors.muted);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.muted);
    widget.setPalette(palette);
    widget.setStyleSheet(QStringLiteral(R"(
            QWidget { color: %9; }
            QLabel[role="muted"] { color: %1; }
            QLabel[role="download"] { color: %2; }
            QLabel[role="upload"] { color: %3; }
            QLabel[role="warning"] { color: %4; }
            QFrame[role="card"] { background: %5; border: 1px solid %6; border-radius: 10px; }
            QTableView { background: %5; alternate-background-color: %7;
                         border: 1px solid %6; border-radius: 6px; }
            QHeaderView { background: %7; }
            QHeaderView::section { background: %7; color: %1; border: 0;
                                   border-bottom: 1px solid %6; padding: 8px; }
            QTabWidget::pane { border: 0; }
            QTabBar::tab { padding: 8px 16px; margin-right: 4px; color: %1;
                           background: transparent; border-bottom: 2px solid transparent; }
            QTabBar::tab:selected { color: %8; border-bottom-color: %8; }
            QLineEdit, QComboBox { background: %5; border: 1px solid %6;
                          border-radius: 6px; padding: 7px; }
            QLineEdit:focus, QComboBox:focus { border-color: %8; }
            QComboBox { padding-right: 24px; }
            QComboBox QAbstractItemView { background: %5; color: %9; }
            QPlainTextEdit { background: %5; border: 1px solid %6; border-radius: 6px;
                             padding: 10px; }
            QSplitter::handle { background: %6; }
            QMenu { background: %5; color: %9; border: 1px solid %6; }
            QMenu::item { padding: 6px 22px; }
            QMenu::item:selected { background: %7; }
            QToolButton { background: %5; border: 1px solid %6; border-radius: 6px;
                          padding: 6px 10px; }
            QToolButton:hover, QToolButton:checked { background: %7; border-color: %8; }
        )")
                             .arg(colors.muted.name(), colors.download.name(), colors.upload.name(),
                                  colors.warning.name(), colors.surface.name(),
                                  colors.border.name(), colors.background.name(),
                                  colors.accent.name(), colors.text.name()));
}

void enableColumnMenu(QTableView& table) {
    auto* header = table.horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setToolTip(QStringLiteral("右键表头选择显示列；拖动分隔线调整列宽"));
    QObject::connect(
        header, &QWidget::customContextMenuRequested, &table,
        [&table, header](const QPoint& position) {
            QMenu menu(&table);
            for (int column = 1; column < table.model()->columnCount(); ++column) {
                auto* action =
                    menu.addAction(table.model()->headerData(column, Qt::Horizontal).toString());
                action->setCheckable(true);
                action->setChecked(!table.isColumnHidden(column));
                QObject::connect(action, &QAction::toggled, &table, [&table, column](bool checked) {
                    table.setColumnHidden(column, !checked);
                });
            }
            menu.exec(header->mapToGlobal(position));
        });
}

} // namespace ball::ui
