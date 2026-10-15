#include "ui/widgets/scope_plot.h"

#include "domain/units.h"
#include "ui/theme/theme.h"

#include "qcustomplot.h"

#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

ScopePlot::ScopePlot(const QString &title, QWidget *parent) : QWidget(parent)
{
    m_plot = new QCustomPlot(this);
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->addWidget(m_plot);

    m_plot->setBackground(QBrush(QColor(theme::kBg)));
    m_plot->setNoAntialiasingOnDrag(true); // 拖动时关抗锯齿：交互更顺，松手后自动重绘
    styleAxes();
    m_plot->plotLayout()->insertRow(0);
    auto *titleItem = new QCPTextElement(m_plot, title, QFont(font().family(), 9));
    titleItem->setTextColor(QColor(theme::kTextDim));
    m_plot->plotLayout()->addElement(0, 0, titleItem);

    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    m_plot->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);

    // 十字定标：两条无限直线 + 一个读数标签（读数同时发给面板显示在工具条上）
    m_crossV = new QCPItemStraightLine(m_plot);
    m_crossV->setPen(QPen(QColor(theme::kTextDim), 1, Qt::DashLine));
    m_crossV->setVisible(false);
    m_crossLabel = new QCPItemText(m_plot);
    m_crossLabel->setColor(QColor(theme::kText));
    m_crossLabel->setFont(theme::monoFont(8));
    m_crossLabel->setBrush(QBrush(QColor(theme::kPanel)));
    m_crossLabel->setPadding(QMargins(4, 2, 4, 2));
    m_crossLabel->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_crossLabel->setVisible(false);

    m_thresholdLine = new QCPItemStraightLine(m_plot);
    m_thresholdLine->setPen(QPen(QColor(theme::kWarn), 1, Qt::DashLine));
    m_thresholdLine->setVisible(false);
    m_thresholdLabel = new QCPItemText(m_plot);
    m_thresholdLabel->setColor(QColor(theme::kWarn));
    m_thresholdLabel->setFont(theme::monoFont(8));
    m_thresholdLabel->setPositionAlignment(Qt::AlignRight | Qt::AlignBottom);
    m_thresholdLabel->setVisible(false);

    connect(m_plot, &QCustomPlot::mouseMove, this, [this](QMouseEvent *e) {
        const double key = m_plot->xAxis->pixelToCoord(e->pos().x());
        const double val = m_plot->yAxis->pixelToCoord(e->pos().y());
        updateCrosshair(key, val);
    });

    applyDefaultRanges();
}

ScopePlot::~ScopePlot() = default;

void ScopePlot::styleAxes()
{
    const QColor text(theme::kTextDim);
    const QColor grid(theme::kGrid);
    for (QCPAxis *ax : {m_plot->xAxis, m_plot->yAxis}) {
        ax->setBasePen(QPen(QColor(theme::kLine)));
        ax->setTickPen(QPen(QColor(theme::kLine)));
        ax->setSubTickPen(QPen(QColor(theme::kLine)));
        ax->setTickLabelColor(text);
        ax->setLabelColor(text);
        ax->grid()->setPen(QPen(grid, 1, Qt::DotLine));
        ax->grid()->setSubGridVisible(false);
        ax->grid()->setZeroLinePen(QPen(QColor(theme::kLine)));
        ax->setTickLabelFont(theme::monoFont(8));
    }
    m_plot->xAxis->setLabel(QStringLiteral("时间 (s)"));
}

int ScopePlot::addChannel(const QString &name, const QColor &color)
{
    Channel c;
    c.name = name;
    c.color = color;
    c.graph = m_plot->addGraph();
    c.graph->setPen(QPen(color, 2));
    c.graph->setName(name);
    c.graph->setLineStyle(QCPGraph::lsLine);
    c.graph->setScatterStyle(QCPScatterStyle::ssNone);

    c.tracer = new QCPItemTracer(m_plot);
    c.tracer->setGraph(c.graph);
    c.tracer->setInterpolating(false);
    c.tracer->setStyle(QCPItemTracer::tsNone); // 只要它的位置，不要画圆圈
    c.tracer->setSize(0);
    c.tracer->setVisible(false);

    c.endLabel = new QCPItemText(m_plot);
    c.endLabel->setColor(color);
    c.endLabel->setFont(theme::monoFont(8, true));
    c.endLabel->setPositionAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    c.endLabel->setPadding(QMargins(3, 1, 3, 1));
    c.endLabel->setBrush(QBrush(QColor(theme::kPanel)));
    c.endLabel->setVisible(false);
    c.endLabel->position->setTypeX(QCPItemPosition::ptPlotCoords);
    c.endLabel->position->setTypeY(QCPItemPosition::ptPlotCoords);

    m_channels.append(c);
    return m_channels.size() - 1;
}

void ScopePlot::addSample(int channel, double tSec, double y)
{
    if (channel < 0 || channel >= m_channels.size()) {
        return;
    }
    Channel &c = m_channels[channel];
    c.t.append(tSec);
    c.y.append(y);
    m_lastT = std::max(m_lastT, tSec);
    trim();
}

void ScopePlot::trim()
{
    // 只保留"窗口的 2 倍"以内的点：既够向左回看，又不会无限增长
    const double keepFrom = m_lastT - m_windowSec * 2.0;
    for (Channel &c : m_channels) {
        int drop = 0;
        while (drop < c.t.size() && c.t.at(drop) < keepFrom) {
            ++drop;
        }
        if (drop > 0) {
            c.t.remove(0, drop);
            c.y.remove(0, drop);
        }
        const int excess = c.t.size() - kMaxPoints;
        if (excess > 0) {
            c.t.remove(0, excess);
            c.y.remove(0, excess);
        }
    }
}

void ScopePlot::setThresholdLine(double y, const QString &label, const QColor &color)
{
    m_thresholdLine->point1->setCoords(0, y);
    m_thresholdLine->point2->setCoords(1, y);
    m_thresholdLine->setPen(QPen(color, 1, Qt::DashLine));
    m_thresholdLine->setVisible(true);
    m_thresholdLabel->setText(label);
    m_thresholdLabel->setColor(color);
    m_thresholdLabel->position->setTypeX(QCPItemPosition::ptAxisRectRatio);
    m_thresholdLabel->position->setTypeY(QCPItemPosition::ptPlotCoords);
    m_thresholdLabel->position->setCoords(1.0, y);
    m_thresholdLabel->setVisible(true);
}

void ScopePlot::addMarker(double tSec, const QString &text, const QColor &color)
{
    auto *line = new QCPItemLine(m_plot);
    line->setPen(QPen(color, 1, Qt::DashLine));
    line->start->setCoords(tSec, 0);
    line->end->setCoords(tSec, 1);
    line->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
    line->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
    line->start->setCoords(tSec, 0.0);
    line->end->setCoords(tSec, 1.0);

    auto *label = new QCPItemText(m_plot);
    label->setText(text);
    label->setColor(color);
    label->setFont(theme::monoFont(7));
    label->setRotation(90);
    label->setPositionAlignment(Qt::AlignLeft | Qt::AlignBottom);
    label->position->setTypeX(QCPItemPosition::ptPlotCoords);
    label->position->setTypeY(QCPItemPosition::ptAxisRectRatio);
    label->position->setCoords(tSec, 0.02);

    m_markers.append(line);
    m_markerLabels.append(label);

    // 只留最近 40 个标记：避免长时间运行时"标记堆满整张图"
    while (m_markers.size() > 40) {
        QCPItemLine *l = m_markers.takeFirst();
        QCPItemText *t = m_markerLabels.takeFirst();
        m_plot->removeItem(l);
        m_plot->removeItem(t);
        delete l;
        delete t;
    }
}

void ScopePlot::clearMarkers()
{
    for (QCPItemLine *l : m_markers) {
        m_plot->removeItem(l);
        delete l;
    }
    for (QCPItemText *t : m_markerLabels) {
        m_plot->removeItem(t);
        delete t;
    }
    m_markers.clear();
    m_markerLabels.clear();
}

void ScopePlot::clearData()
{
    for (Channel &c : m_channels) {
        c.t.clear();
        c.y.clear();
        if (c.graph != nullptr) {
            c.graph->data()->clear();
        }
        if (c.endLabel != nullptr) {
            c.endLabel->setVisible(false);
        }
    }
    clearMarkers();
    m_lastT = -1.0;
    applyDefaultRanges();
    replot();
}

void ScopePlot::setUnitLabel(const QString &unit)
{
    m_unit = unit;
    m_plot->yAxis->setLabel(unit);
}

void ScopePlot::setYRange(double lo, double hi)
{
    m_yLo = lo;
    m_yHi = hi;
    m_autoY = false;
    applyDefaultRanges();
}

void ScopePlot::setAutoY(bool on)
{
    m_autoY = on;
}

void ScopePlot::setWindowSeconds(double sec)
{
    m_windowSec = (sec > 1.0) ? sec : 1.0;
    applyDefaultRanges();
}

void ScopePlot::setEndValueVisible(bool on)
{
    m_endLabels = on;
    for (Channel &c : m_channels) {
        c.endLabel->setVisible(on && !c.t.isEmpty());
    }
}

void ScopePlot::applyDefaultRanges()
{
    const double right = (m_lastT >= 0) ? m_lastT : m_windowSec;
    m_plot->xAxis->setRange(std::max(0.0, right - m_windowSec), right);
    m_plot->yAxis->setRange(m_yLo, m_yHi);
    m_viewModified = false;
    emit viewModified(false);
}

void ScopePlot::resetView()
{
    applyDefaultRanges();
    replot();
}

void ScopePlot::markViewModified()
{
    if (!m_viewModified) {
        m_viewModified = true;
        emit viewModified(true);
    }
}

void ScopePlot::updateCrosshair(double key, double value)
{
    m_crossV->point1->setCoords(key, 0);
    m_crossV->point2->setCoords(key, 1);
    m_crossV->point1->setTypeY(QCPItemPosition::ptAxisRectRatio);
    m_crossV->point2->setTypeY(QCPItemPosition::ptAxisRectRatio);
    m_crossV->setVisible(true);

    m_readout = QStringLiteral("t %1 s · %2%3")
                    .arg(domain::units::number(key, 1),
                         domain::units::number(value, m_unit == QStringLiteral("V") ? 2 : 1),
                         m_unit.isEmpty() ? QString() : (QStringLiteral(" ") + m_unit));
    m_crossLabel->setText(m_readout);
    m_crossLabel->position->setTypeX(QCPItemPosition::ptAxisRectRatio);
    m_crossLabel->position->setTypeY(QCPItemPosition::ptAxisRectRatio);
    m_crossLabel->position->setCoords(0.01, 0.98);
    m_crossLabel->setVisible(true);
    emit cursorMoved(m_readout);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::mouseMoveEvent(QMouseEvent *event)
{
    QWidget::mouseMoveEvent(event);
    if (!m_plot->geometry().contains(event->pos())) {
        return;
    }
    const QPoint p = m_plot->mapFrom(this, event->pos());
    updateCrosshair(m_plot->xAxis->pixelToCoord(p.x()), m_plot->yAxis->pixelToCoord(p.y()));
    if (m_plot->axisRect()->rect().contains(p)) {
        markViewModified();
    }
}

void ScopePlot::leaveEvent(QEvent *event)
{
    m_crossV->setVisible(false);
    m_crossLabel->setVisible(false);
    m_readout.clear();
    emit cursorMoved(QString());
    m_plot->replot(QCustomPlot::rpQueuedReplot);
    QWidget::leaveEvent(event);
}

void ScopePlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    resetView();
    QWidget::mouseDoubleClickEvent(event);
}

void ScopePlot::wheelEvent(QWheelEvent *event)
{
    // 滚轮缩放交给 QCustomPlot（iRangeZoom）；这里只负责把"已缩放"标出来
    markViewModified();
    QWidget::wheelEvent(event);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ScopePlot::updateEndLabels()
{
    for (Channel &c : m_channels) {
        const bool has = !c.t.isEmpty();
        if (c.tracer != nullptr) {
            c.tracer->setGraphKey(m_lastT);
            c.tracer->setVisible(has);
        }
        if (c.endLabel != nullptr) {
            c.endLabel->setVisible(m_endLabels && has);
            if (has) {
                c.endLabel->position->setCoords(m_lastT, c.y.last());
                c.endLabel->setText(QStringLiteral("%1%2")
                                        .arg(domain::units::number(c.y.last(),
                                                                   m_unit == QStringLiteral("V") ? 2 : 1),
                                             m_unit.isEmpty() ? QString()
                                                              : (QStringLiteral(" ") + m_unit)));
            }
        }
    }
}

void ScopePlot::replot()
{
    for (const Channel &c : m_channels) {
        if (c.graph != nullptr) {
            c.graph->setData(c.t, c.y, true);
        }
    }
    if (m_autoY && m_lastT >= 0) {
        m_plot->yAxis->rescale(true);
        m_plot->yAxis->scaleRange(1.15, m_plot->yAxis->range().center());
    }
    // X 轴：未缩放时始终跟随最新数据（"滑动窗口"）；用户缩放后不再自动跟随
    if (!m_viewModified) {
        const double right = (m_lastT >= 0) ? m_lastT : m_windowSec;
        m_plot->xAxis->setRange(std::max(0.0, right - m_windowSec), right);
        if (!m_autoY) {
            m_plot->yAxis->setRange(m_yLo, m_yHi);
        }
    }
    updateEndLabels();
    m_plot->replot();
}
