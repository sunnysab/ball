// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/traffic_model.h"

#include <QSet>

#include <cmath>
#include <limits>

namespace ball::ui {
namespace {

bool addCounter(quint64& total, quint64 value) {
    if (value > std::numeric_limits<quint64>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

} // namespace

InterfaceTotals sumSelectedInterfaces(const QList<InterfaceSnapshot>& interfaces,
                                      const QStringList& selectedNames) {
    InterfaceTotals totals;
    const QSet<QString> selected(selectedNames.cbegin(), selectedNames.cend());
    totals.selectedCount = int(selected.size());
    QSet<QString> found;
    for (const InterfaceSnapshot& row : interfaces) {
        if (!selected.contains(row.name)) {
            continue;
        }
        found.insert(row.name);
        if (!addCounter(totals.today.rxBytes, row.today.rxBytes) ||
            !addCounter(totals.today.txBytes, row.today.txBytes) ||
            !addCounter(totals.session.rxBytes, row.session.rxBytes) ||
            !addCounter(totals.session.txBytes, row.session.txBytes)) {
            return {};
        }
        totals.rxBps += row.rxBps;
        totals.txBps += row.txBps;
        if (!std::isfinite(totals.rxBps) || !std::isfinite(totals.txBps)) {
            return {};
        }
    }
    totals.presentCount = int(found.size());
    totals.valid = !found.isEmpty();
    totals.complete = totals.valid && found.size() == selected.size();
    return totals;
}

QStringList defaultInterfaceSelection(const QList<InterfaceSnapshot>& interfaces) {
    QSet<QString> names;
    for (const InterfaceSnapshot& row : interfaces) {
        if (row.kind == InterfaceKind::Physical && row.up) {
            names.insert(row.name);
        }
    }
    QStringList result(names.cbegin(), names.cend());
    result.sort();
    return result;
}

} // namespace ball::ui
