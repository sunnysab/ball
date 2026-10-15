// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/placement.h"

namespace ball::ui {

// The two rects are genuinely (window, screen) everywhere; naming them in the
// header was not enough for the linter, so the order is fixed with a NOLINT.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QRect clampRectInto(const QRect& windowRect, const QRect& screen) {
    QRect rect = windowRect;
    // A window bigger than its screen is shrunk rather than pushed off-screen,
    // so the whole surface stays reachable.
    if (rect.width() > screen.width()) {
        rect.setWidth(screen.width());
    }
    if (rect.height() > screen.height()) {
        rect.setHeight(screen.height());
    }
    if (rect.right() > screen.right()) {
        rect.moveRight(screen.right());
    }
    if (rect.left() < screen.left()) {
        rect.moveLeft(screen.left());
    }
    if (rect.bottom() > screen.bottom()) {
        rect.moveBottom(screen.bottom());
    }
    if (rect.top() < screen.top()) {
        rect.moveTop(screen.top());
    }
    return rect;
}

AnchoredPlacement anchoredPlacementFor(const QRect& windowRect, const QRect& screen) {
    AnchoredPlacement placement;
    const int leftDistance = windowRect.left() - screen.left();
    const int rightDistance = screen.right() - windowRect.right();
    if (leftDistance <= rightDistance) {
        placement.horizontal = HorizontalEdge::Left;
        placement.margins.setLeft(qMax(0, leftDistance));
        placement.margins.setRight(qMax(0, rightDistance));
    } else {
        placement.horizontal = HorizontalEdge::Right;
        placement.margins.setRight(qMax(0, rightDistance));
        placement.margins.setLeft(qMax(0, leftDistance));
    }

    const int topDistance = windowRect.top() - screen.top();
    const int bottomDistance = screen.bottom() - windowRect.bottom();
    if (topDistance <= bottomDistance) {
        placement.vertical = VerticalEdge::Top;
        placement.margins.setTop(qMax(0, topDistance));
        placement.margins.setBottom(qMax(0, bottomDistance));
    } else {
        placement.vertical = VerticalEdge::Bottom;
        placement.margins.setBottom(qMax(0, bottomDistance));
        placement.margins.setTop(qMax(0, topDistance));
    }
    return placement;
}

QPoint snappedPosition(const QRect& windowRect, const QRect& screen, int snapDistance) {
    QRect rect = clampRectInto(windowRect, screen);
    if (rect.left() - screen.left() <= snapDistance) {
        rect.moveLeft(screen.left());
    } else if (screen.right() - rect.right() <= snapDistance) {
        rect.moveRight(screen.right());
    }
    if (rect.top() - screen.top() <= snapDistance) {
        rect.moveTop(screen.top());
    } else if (screen.bottom() - rect.bottom() <= snapDistance) {
        rect.moveBottom(screen.bottom());
    }
    return rect.topLeft();
}

} // namespace ball::ui
