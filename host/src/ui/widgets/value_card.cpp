#include "ui/widgets/value_card.h"

#include "domain/units.h"
#include "ui/theme/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace {

/// 严重程度 → 颜色（**唯一的一处映射**：界面别处不许再按自己的理解挑颜色）
const char *severityColor(domain::Severity s)
{
    switch (s) {
    case domain::Severity::Ok:
        return theme::kText;
    case domain::Severity::Warn:
        return theme::kWarn;
    case domain::Severity::Alarm:
        return theme::kAlarm;
    case domain::Severity::Unknown:
        break;
    }
    return theme::kOffline;
}

} // namespace

ValueCard::ValueCard(const QString &title, const QString &unit, int decimals, QWidget *parent)
    : QFrame(parent), m_unit(unit), m_decimals(decimals)
{
    setProperty("role", "card");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_title = new QLabel(title, this);
    m_title->setProperty("role", "cardTitle");

    m_value = new QLabel(QStringLiteral("--"), this);
    m_value->setFont(theme::monoFont(m_pointSize, true));
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_unitLabel = new QLabel(m_unit, this);
    m_unitLabel->setProperty("role", "sub");
    m_unitLabel->setFont(theme::monoFont(10));

    m_sub = new QLabel(QString(), this);
    m_sub->setProperty("role", "sub");

    auto *valueRow = new QHBoxLayout;
    valueRow->setContentsMargins(0, 0, 0, 0);
    valueRow->setSpacing(4);
    valueRow->addWidget(m_value);
    valueRow->addWidget(m_unitLabel);
    valueRow->addStretch(1);

    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(9, 6, 9, 6);
    vl->setSpacing(1);
    vl->addWidget(m_title);
    vl->addLayout(valueRow);
    vl->addWidget(m_sub);

    relayoutValue();
    setSeverity(domain::Severity::Unknown);
}

void ValueCard::relayoutValue()
{
    // 按"最坏情况位数"预留宽度：符号 + 4 位整数 + 小数点 + decimals 位
    const QString worst = QStringLiteral("-0000.%1").arg(QString(m_decimals, QLatin1Char('0')));
    const int w = m_value->fontMetrics().horizontalAdvance(worst);
    m_value->setFixedWidth(w + 2);
}

void ValueCard::setDecimals(int d)
{
    m_decimals = d;
    relayoutValue();
    if (m_hasValue) {
        setValue(m_last);
    }
}

void ValueCard::setValuePointSize(int pt)
{
    m_pointSize = pt;
    m_value->setFont(theme::monoFont(pt, true));
    relayoutValue();
}

void ValueCard::setValue(double v)
{
    m_last = v;
    m_hasValue = true;
    relayoutValue(); // 数字按"最坏位数"预留宽度，单位与副标题就不会跟着抖
    m_unitLabel->setText(m_unit);
    m_value->setText(domain::units::number(v, m_decimals));
    setSeverity(domain::Severity::Ok);
}

void ValueCard::setText(const QString &text)
{
    m_hasValue = false;
    m_unitLabel->setText(m_unit);
    // ★ 文本类取值（任务/阶段/档位/页号等枚举）**不能**按数字预留宽度，
    //   否则"自主巡航"会被裁成"自主巡" —— 数值牌既显示数字又显示枚举，
    //   两种模式的宽度策略必须分开（这个坑在 P1 截图上直接看出来了）。
    m_value->setMinimumWidth(0);
    m_value->setMaximumWidth(QWIDGETSIZE_MAX);
    m_value->setText(text);
    setSeverity(domain::Severity::Ok);
}

void ValueCard::setInvalid(const QString &hint)
{
    m_hasValue = false;
    m_value->setText(QStringLiteral("--"));
    relayoutValue(); // 回到"数字"模式：恢复预留宽度
    setSeverity(domain::Severity::Unknown);
    setSub(hint);
}

void ValueCard::setSeverity(domain::Severity s)
{
    m_value->setStyleSheet(QStringLiteral("color:%1;")
                               .arg(QLatin1String(severityColor(s))));
}

void ValueCard::setSub(const QString &text)
{
    m_sub->setText(text);
}
