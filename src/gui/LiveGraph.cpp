// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/LiveGraph.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace ys {

QString compactNumber(double v)
{
    const double a = std::fabs(v);
    if (a < 10000 && v == std::floor(v))
        return QString::number(qint64(v));
    if (a < 1000)
        return QString::number(v, 'g', 4);
    static const char *suffix[] = {"k", "M", "G", "T", "P"};
    int i = -1;
    double x = v;
    while (std::fabs(x) >= 1000 && i < 4) {
        x /= 1000;
        ++i;
    }
    return QString::number(x, 'f', std::fabs(x) < 10 ? 2 : 1) + QLatin1Char(' ') + QLatin1String(suffix[i]);
}

LiveGraph::LiveGraph(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void LiveGraph::setSeries(const QString &title, const QString &unit, const QVector<QPointF> &points)
{
    m_title = title;
    m_unit = unit;
    m_points = points;
    update();
}

void LiveGraph::clearSeries()
{
    m_title.clear();
    m_points.clear();
    update();
}

void LiveGraph::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const QColor text = pal.color(QPalette::Text);
    QColor grid = pal.color(QPalette::Mid);
    grid.setAlpha(90);
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    const QColor line = dark ? QColor(0x60, 0xa5, 0xfa) : QColor(0x25, 0x63, 0xeb);

    const QFontMetrics fm(font());
    const int left = fm.horizontalAdvance(QStringLiteral("000.0 M")) + 10;
    const QRectF plot(left, fm.height() + 8, width() - left - 10, height() - 2 * fm.height() - 16);

    p.setPen(text);
    if (m_points.size() < 2) {
        p.drawText(rect(), Qt::AlignCenter,
                   m_title.isEmpty() ? tr("Select a numeric value in the table to graph it")
                                     : tr("%1: waiting for more samples").arg(m_title));
        return;
    }
    double ymin = m_points.first().y(), ymax = ymin;
    for (const QPointF &pt : m_points) {
        ymin = qMin(ymin, pt.y());
        ymax = qMax(ymax, pt.y());
    }
    if (ymin > 0 && ymin < (ymax - ymin))
        ymin = 0; // anchor rates at zero when it does not flatten the plot
    if (ymax == ymin) {
        ymax += 1;
        ymin = qMin(ymin, ymin - 1);
    }
    const double x0 = m_points.first().x();
    const double x1 = qMax(m_points.last().x(), x0 + 1e-9);
    auto map = [&](const QPointF &pt) {
        return QPointF(plot.left() + (pt.x() - x0) / (x1 - x0) * plot.width(),
                       plot.bottom() - (pt.y() - ymin) / (ymax - ymin) * plot.height());
    };

    // grid and labels
    for (int i = 0; i <= 3; ++i) {
        const double y = plot.bottom() - i * plot.height() / 3;
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRectF(0, y - fm.height() / 2.0, left - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   compactNumber(ymin + i * (ymax - ymin) / 3));
    }
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), fm.height()), Qt::AlignLeft,
               tr("-%1 s").arg(qRound(x1 - x0)));
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), fm.height()), Qt::AlignRight, tr("now"));

    QPainterPath path(map(m_points.first()));
    for (int i = 1; i < m_points.size(); ++i)
        path.lineTo(map(m_points[i]));
    p.setPen(QPen(line, 2));
    p.drawPath(path);
    const QPointF last = map(m_points.last());
    p.setBrush(line);
    p.drawEllipse(last, 3, 3);

    p.setPen(text);
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    const QString head = m_title + QStringLiteral("   ") + compactNumber(m_points.last().y())
                         + (m_unit.isEmpty() ? QString() : QLatin1Char(' ') + m_unit);
    p.drawText(QRectF(left, 2, width() - left - 10, fm.height() + 4), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(head, Qt::ElideMiddle, width() - left - 10));
}

} // namespace ys
