#pragma once

// ============================================================================
//  LedIndicator —— 状态灯（一个圆点 + 文字 + 可选补充说明）
//
//  用途：把协议里的**位域**（0x12 异常标志、0x15 状态位、0x16 状态位、0x14 就绪位）
//  一位一行地显示出来。为什么不用彩色文字代替：
//    "位"的本质是"是/否"，圆点的位置与颜色比读一串文字快得多；
//    而且**点不亮**本身就是信息（例如 MQ-2 未就绪时灯是灰的，一眼就能看出
//    "这不是 0 ppm，而是传感器没插"）。
//
//  颜色语义（与 theme.h 一致）：绿=该位成立且是好消息；琥珀=该位成立且需注意；
//  红=该位成立且是故障；灰=该位不成立（含"无数据"）。
// ============================================================================

#include <QString>
#include <QWidget>

class LedIndicator : public QWidget
{
    Q_OBJECT
public:
    enum class Level {
        Off = 0, ///< 不成立（灰）
        Ok,      ///< 成立且正常（绿）
        Warn,    ///< 成立但需注意（琥珀）
        Alarm,   ///< 成立且是故障（红）
    };

    explicit LedIndicator(const QString &text, QWidget *parent = nullptr);

    void setLevel(Level l, const QString &detail = QString());
    void setText(const QString &text);
    Level level() const { return m_level; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
    QString m_detail;
    Level m_level = Level::Off;
};
