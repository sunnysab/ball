// ball: a network traffic HUD for KDE Wayland sessions.
// Placement math for a layer-shell surface, which cannot be moved freely.
#pragma once

#include <QMargins>
#include <QPoint>
#include <QRect>

#include <cstdint>

namespace ball::ui {

enum class HorizontalEdge : std::uint8_t {
    Left,
    Right,
};

enum class VerticalEdge : std::uint8_t {
    Top,
    Bottom,
};

// A window rect expressed as the corner it is anchored to plus its distance
// from that corner; this is what the layer-shell compositor understands.
struct AnchoredPlacement {
    HorizontalEdge horizontal = HorizontalEdge::Left;
    VerticalEdge vertical = VerticalEdge::Top;
    QMargins margins;
};

// Keeps the window inside the screen. A window larger than the screen is pinned
// to the top-left corner rather than pushed off-screen.
QRect clampRectInto(const QRect& windowRect, const QRect& screen);

// Picks the nearest corner so the stored position survives a resize: a surface
// anchored to the right grows leftwards and stays visible.
AnchoredPlacement anchoredPlacementFor(const QRect& windowRect, const QRect& screen);

// Moves the window to the near screen edge when it is released within
// `snapDistance` of one; otherwise only clamps it into view.
QPoint snappedPosition(const QRect& windowRect, const QRect& screen, int snapDistance);

} // namespace ball::ui
