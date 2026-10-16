#include "ui/widgets/attitude_indicator.h"

#include "domain/units.h"
#include "ui/theme/theme.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

AttitudeIndicator::AttitudeIndicator(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(140, 100); // 小屏友好（原来 180×130 太占地方）
}

void AttitudeIndicator::setAttitude(double rollDeg, double pitchDeg, bool valid)
{
    m_roll = rollDeg;
    m_pitch = pitchDeg;
    m_valid = valid;
    update();
}

void AttitudeIndicator::setRange(double deg)
{
    m_range = (deg > 1.0) ? deg : 30.0;
    update();
}

QSize AttitudeIndicator::sizeHint() const
{
    return QSize(220, 150);
}

QSize AttitudeIndicator::minimumSizeHint() const
{
    return QSize(180, 130);
}

void AttitudeIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = rect().adjusted(1, 1, -1, -1);
    QPainterPath clip;
    clip.addRoundedRect(box, 6, 6);
    p.setClipPath(clip);

    // 底色 = "天"
    p.fillRect(box, QColor(theme::kBg));

    const QPointF c = box.center();
    const double pxPerDeg = box.height() / (2.0 * m_range);
    const bool over = (std::abs(m_pitch) > m_range);

    p.save();
    p.translate(c);
    // 横滚：车体右倾 → 地平线在画面里向左倾斜（kRollSign 见头文件）
    p.rotate(-m_roll * kRollSign);
    // 俯仰：机头上仰 → 地平线在画面里下移（Qt 的 y 向下为正）
    const double dy = m_pitch * pxPerDeg;
    const double span = box.width() + box.height(); // 旋转后仍能铺满

    // 地面（地平线以下）
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme::kGrid));
    p.drawRect(QRectF(-span, dy, 2 * span, span));

    // 地平线
    p.setPen(QPen(QColor(theme::kTextDim), 1.5));
    p.drawLine(QPointF(-span, dy), QPointF(span, dy));

    // 俯仰刻度（每 5°，带小标签）
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() - 1.5);
    p.setFont(small);
    for (int d = -60; d <= 60; d += 5) {
        if (d == 0 || std::abs(d) > m_range) {
            continue;
        }
        const double y = d * pxPerDeg;
        const double half = (d % 10 == 0) ? 26.0 : 14.0;
        p.setPen(QPen(QColor(theme::kLine), 1.0));
        p.drawLine(QPointF(-half, y), QPointF(half, y));
        if (d % 10 == 0) {
            p.setPen(QColor(theme::kTextDim));
            p.drawText(QRectF(half + 3, y - 7, 30, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(d));
        }
    }
    p.restore();

    // 机头符号（固定的两横一竖）
    p.setPen(QPen(QColor(theme::kText), 2.0));
    p.drawLine(QPointF(c.x() - 30, c.y()), QPointF(c.x() - 8, c.y()));
    p.drawLine(QPointF(c.x() + 8, c.y()), QPointF(c.x() + 30, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));

    // 角标：横滚/俯仰读数（数字用等宽，便于扫读）
    QFont mono = theme::monoFont(10, true);
    p.setFont(mono);
    if (!m_valid) {
        p.setPen(QColor(theme::kOffline));
        p.drawText(box.adjusted(6, 4, -6, -4), Qt::AlignCenter, QStringLiteral("无姿态数据"));
    } else {
        p.setPen(QColor(theme::kTextDim));
        p.drawText(box.adjusted(6, 4, -6, 0), Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("R %1°").arg(domain::units::number(m_roll, 1)));
        p.drawText(box.adjusted(6, 4, -6, 0), Qt::AlignTop | Qt::AlignRight,
                   QStringLiteral("P %1°").arg(domain::units::number(m_pitch, 1)));
        if (over) {
            p.setPen(QColor(theme::kWarn));
            p.drawText(box.adjusted(6, 0, -6, -4), Qt::AlignBottom | Qt::AlignRight,
                       QStringLiteral("超量程 ±%1°").arg(domain::units::number(m_range, 0)));
        }
    }

    // 边框
    p.setClipping(false);
    p.setPen(QPen(QColor(theme::kLine), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box, 6, 6);
}
