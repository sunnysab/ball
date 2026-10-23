#include "ui/traffic_chart.h"

#include "ui/format.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ball::ui {
namespace {
// shortcut: history lives only in GUI memory; extend the daemon if durable history is needed.
constexpr qint64 kWindowMs = 5 * 60 * 1000;
constexpr qint64 kMaxGapMs = 2500;
} // namespace

TrafficChart::TrafficChart(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("details.trafficChart"));
    setMinimumSize(300, 170);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("所选网卡最近五分钟的下载和上传速率"));
    setToolTip(QStringLiteral("只记录界面运行期间的所选网卡速率，不保存到磁盘。\n"
                              "切换网卡或后台重启后重新记录；采集中断留空，不补成零。"));
}

void TrafficChart::addSample(const QDateTime& at, const QDateTime& started, QStringList interfaces,
                             double rx, double tx, bool available) {
    interfaces.removeDuplicates();
    interfaces.sort();
    if (started != m_started || interfaces != m_interfaces ||
        (!m_samples.isEmpty() && at.isValid() && at < m_samples.last().at)) {
        m_samples.clear();
        m_breakNext = true;
        m_started = started;
        m_interfaces = std::move(interfaces);
    }
    m_available = available && at.isValid() && started.isValid() && !m_interfaces.isEmpty() &&
                  std::isfinite(rx) && std::isfinite(tx) && rx >= 0 && tx >= 0;
    if (!m_available) {
        m_breakNext = true;
    } else if (!m_samples.isEmpty() &&
               at.toMSecsSinceEpoch() / 1000 == m_samples.last().at.toMSecsSinceEpoch() / 1000) {
        // UI-only refreshes must not add points or reconnect a broken segment.
        m_samples.last().rx = rx;
        m_samples.last().tx = tx;
    } else {
        const bool disconnected =
            m_breakNext || m_samples.isEmpty() || m_samples.last().at.msecsTo(at) > kMaxGapMs;
        m_samples.append({at, rx, tx, disconnected});
        m_breakNext = false;
        while (m_samples.size() > 301 || m_samples.first().at.msecsTo(at) > kWindowMs) {
            m_samples.removeFirst();
        }
    }
    setAccessibleDescription(
        m_samples.isEmpty()
            ? QStringLiteral("等待可用网卡数据")
            : QStringLiteral("%1；最近采样 %2，下载 %3，上传 %4")
                  .arg(m_available ? QStringLiteral("实时采集") : QStringLiteral("采集已中断"),
                       m_samples.last().at.toLocalTime().toString(QStringLiteral("HH:mm:ss")),
                       formatRate(m_samples.last().rx).trimmed(),
                       formatRate(m_samples.last().tx).trimmed()));
    update();
}

void TrafficChart::setTheme(Theme theme) {
    m_theme = theme;
    update();
}

void TrafficChart::paintEvent(QPaintEvent*) {
    const auto colors = themeColors(m_theme);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot(92, 28, std::max(1, width() - 108), std::max(1, height() - 62));

    double maximum = 1;
    for (const auto& sample : m_samples) {
        maximum = std::max(maximum, std::max(sample.rx, sample.tx));
    }
    if (maximum < std::numeric_limits<double>::max() / 1.15) {
        maximum *= 1.15;
    }
    for (int i = 0; i <= 2; ++i) {
        const double y = plot.top() + plot.height() * i / 2;
        painter.setPen(QPen(colors.border, 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(colors.muted);
        painter.drawText(QRectF(0, y - 10, 80, 20), Qt::AlignRight | Qt::AlignVCenter,
                         formatRate(maximum * ((2 - i) / 2.0)).trimmed());
    }
    painter.drawText(QRectF(plot.left(), plot.bottom() + 10, 100, 22), Qt::AlignLeft,
                     QStringLiteral("−5 分钟"));
    painter.drawText(QRectF(plot.center().x() - 50, plot.bottom() + 10, 100, 22), Qt::AlignHCenter,
                     QStringLiteral("−2 分 30 秒"));
    painter.drawText(QRectF(plot.right() - 120, plot.bottom() + 10, 120, 22), Qt::AlignRight,
                     m_available ? QStringLiteral("最新") : QStringLiteral("采集已中断"));
    if (!m_samples.isEmpty()) {
        painter.setPen(colors.muted);
        painter.drawText(
            QRectF(plot.left(), 0, plot.width() - 205, 22), Qt::AlignLeft,
            QStringLiteral("最近采样 %1")
                .arg(m_samples.last().at.toLocalTime().toString(QStringLiteral("HH:mm:ss"))));
    }
    painter.setPen(colors.download);
    painter.drawText(QRectF(plot.right() - 190, 0, 90, 22), Qt::AlignRight,
                     QStringLiteral("━ ↓ 下载"));
    painter.setPen(colors.upload);
    painter.drawText(QRectF(plot.right() - 90, 0, 90, 22), Qt::AlignRight,
                     QStringLiteral("┄ ↑ 上传"));
    if (m_samples.isEmpty()) {
        painter.setPen(colors.muted);
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待可用网卡数据"));
        return;
    }

    const qint64 end = m_samples.last().at.toMSecsSinceEpoch();
    painter.save();
    painter.setClipRect(plot.adjusted(-2, -2, 2, 2));
    for (const bool upload : {false, true}) {
        const QColor color = upload ? colors.upload : colors.download;
        QPainterPath segment;
        QPointF first;
        QPointF last;
        bool hasPoint = false;
        const auto drawSegment = [&] {
            if (!hasPoint) {
                return;
            }
            QPainterPath area = segment;
            area.lineTo(last.x(), plot.bottom());
            area.lineTo(first.x(), plot.bottom());
            area.closeSubpath();
            QColor fill = color;
            fill.setAlpha(upload ? 20 : 35);
            painter.fillPath(area, fill);
            painter.setPen(QPen(color, 1.8, upload ? Qt::DashLine : Qt::SolidLine));
            painter.drawPath(segment);
            painter.drawEllipse(last, 2, 2);
        };
        for (const auto& sample : m_samples) {
            const QPointF point(plot.right() - double(end - sample.at.toMSecsSinceEpoch()) /
                                                   kWindowMs * plot.width(),
                                plot.bottom() -
                                    (upload ? sample.tx : sample.rx) / maximum * plot.height());
            if (sample.breakBefore || !hasPoint) {
                drawSegment();
                segment = QPainterPath(point);
                first = point;
                hasPoint = true;
            } else {
                segment.lineTo(point);
            }
            last = point;
        }
        drawSegment();
    }
    painter.restore();
}

} // namespace ball::ui
