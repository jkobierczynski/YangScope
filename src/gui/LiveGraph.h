// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointF>
#include <QVector>
#include <QWidget>

namespace ys {

/// Small time-series plot for one streamed value (or its rate).
class LiveGraph : public QWidget {
    Q_OBJECT
public:
    explicit LiveGraph(QWidget *parent = nullptr);
    /// Points as (seconds, value); x need not start at 0.
    void setSeries(const QString &title, const QString &unit, const QVector<QPointF> &points);
    void clearSeries();
    QSize sizeHint() const override { return {400, 160}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_title;
    QString m_unit;
    QVector<QPointF> m_points;
};

/// Compact number: 1.23 k, 4.5 M, ... (SI); integers below 10000 as is.
QString compactNumber(double v);

} // namespace ys
