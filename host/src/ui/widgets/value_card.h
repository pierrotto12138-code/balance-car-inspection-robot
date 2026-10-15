#pragma once

// ============================================================================
//  ValueCard —— 数值牌（标题 + 大号等宽数值 + 单位 + 副标题）
//
//  它承担三条界面纪律：
//    ① **同一量始终同一位置、同一量程、同一小数位**（ISA-101）：数值区宽度按
//       "最坏情况位数"预留，数字在 9.99↔10.00 之间跳时单位和副标题**不会跟着位移**；
//    ② **颜色只用于表达异常**：正常一律中性色，警示琥珀、报警红、无数据灰；
//    ③ 等宽数字右对齐（字体来自 theme::monoFont）。
//
//  副标题用来回答"这个值是谁给的、多久以前"——两条链路都会发周期帧（协议 §七 约定 6），
//  不说清来源就会出现"两条链路的数据互相打架、而看的人不知道信哪个"。
// ============================================================================

#include "domain/vehicle_state.h"

#include <QFrame>

class QLabel;

class ValueCard : public QFrame
{
    Q_OBJECT
public:
    explicit ValueCard(const QString &title, const QString &unit = QString(), int decimals = 2,
                       QWidget *parent = nullptr);

    /// 设定数值（按当前单位与小数位格式化）
    void setValue(double v);
    /// 直接给文本（用于枚举类"值"，如任务阶段；此时不参与数值格式化）
    void setText(const QString &text);
    /// 无数据（显示 "--"，灰色）——**不要**显示 0：0 是"测量到的 0"，不是"不知道"
    void setInvalid(const QString &hint = QStringLiteral("无数据"));

    void setUnit(const QString &unit) { m_unit = unit; }
    void setDecimals(int d);
    void setSeverity(domain::Severity s);
    void setSub(const QString &text);
    /// 数值区的字号（诊断类小卡片可以用小一号）
    void setValuePointSize(int pt);

private:
    void relayoutValue();

    QLabel *m_title = nullptr;
    QLabel *m_value = nullptr;
    QLabel *m_unitLabel = nullptr;
    QLabel *m_sub = nullptr;
    QString m_unit;
    int m_decimals = 2;
    int m_pointSize = 17;
    bool m_hasValue = false;
    double m_last = 0.0;
};
