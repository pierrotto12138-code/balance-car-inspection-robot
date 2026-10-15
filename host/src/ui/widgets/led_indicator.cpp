#include "ui/widgets/led_indicator.h"

#include "ui/theme/theme.h"

#include <QFontMetrics>
#include <QPainter>

LedIndicator::LedIndicator(const QString &text, QWidget *parent) : QWidget(parent), m_text(text)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(20);
}

void LedIndicator::setLevel(Level l, const QString &detail)
{
    m_level = l;
    m_detail = detail;
    update();
}

void LedIndicator::setText(const QString &text)
{
    m_text = text;
    update();
}

QSize LedIndicator::sizeHint() const
{
    const QFontMetrics fm(font());
    const int w = 16 + fm.horizontalAdvance(m_text) + (m_detail.isEmpty() ? 0 : 6 + fm.horizontalAdvance(m_detail));
    return QSize(w, 20);
}

void LedIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor dot(theme::kOffline);
    switch (m_level) {
    case Level::Ok:
        dot = QColor(theme::kOk);
        break;
    case Level::Warn:
        dot = QColor(theme::kWarn);
        break;
    case Level::Alarm:
        dot = QColor(theme::kAlarm);
        break;
    case Level::Off:
        break;
    }

    const int cy = height() / 2;
    const int r = 4;
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(QPoint(6, cy), r, r);

    // 不成立的位用暗色描边，避免与"成立但为灰"混淆
    if (m_level == Level::Off) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(theme::kLine), 1));
        p.drawEllipse(QPoint(6, cy), r, r);
    }

    const QFontMetrics fm(font());
    int x = 16;
    p.setPen(QColor(m_level == Level::Off ? theme::kTextDim : theme::kText));
    p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text);
    x += fm.horizontalAdvance(m_text) + 6;

    if (!m_detail.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() - 1.0);
        p.setFont(f);
        p.setPen(QColor(theme::kTextDim));
        p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, m_detail);
    }
}
