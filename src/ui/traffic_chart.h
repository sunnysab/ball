// ball: bounded NIC-rate history, owned by the GUI rather than the root daemon.
#pragma once

#include "ui/appearance.h"

#include <QDateTime>
#include <QList>
#include <QStringList>
#include <QWidget>

namespace ball::ui {

class TrafficChart : public QWidget {
    Q_OBJECT

public:
    struct Sample {
        QDateTime at;
        double rx = 0;
        double tx = 0;
        bool breakBefore = true;
    };

    explicit TrafficChart(QWidget* parent = nullptr);
    void addSample(const QDateTime& at, const QDateTime& started, QStringList interfaces, double rx,
                   double tx, bool available);
    void setTheme(Theme theme);
    const QList<Sample>& samples() const { return m_samples; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<Sample> m_samples;
    QDateTime m_started;
    QStringList m_interfaces;
    bool m_available = false;
    bool m_breakNext = true;
    Theme m_theme = Theme::Porcelain;
};

} // namespace ball::ui
