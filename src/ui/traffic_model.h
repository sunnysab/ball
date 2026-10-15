// ball: a network traffic HUD for KDE Wayland sessions.
// Aggregation of a snapshot into the values the HUD displays.
#pragma once

#include "ui/snapshot.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace ball::ui {

// Sums only the interfaces the user selected. VPN, veth and loopback keep their
// own rows and are never folded into the total unless selected by name.
struct InterfaceTotals {
    bool valid = false;    // at least one selected interface is present in the snapshot
    bool complete = false; // every selected interface was present
    ByteCounters today;
    ByteCounters session;
    double rxBps = 0.0;
    double txBps = 0.0;
    int selectedCount = 0;
    int presentCount = 0;
};

InterfaceTotals sumSelectedInterfaces(const QList<InterfaceSnapshot>& interfaces,
                                      const QStringList& selectedNames);

// First-run selection: the interfaces that are up and physical, by name. Virtual
// interfaces, VPNs and loopback are deliberately left out.
QStringList defaultInterfaceSelection(const QList<InterfaceSnapshot>& interfaces);

} // namespace ball::ui
