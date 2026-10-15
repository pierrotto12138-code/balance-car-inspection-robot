#pragma once

// ============================================================================
//  AttitudeIndicator —— 人工地平仪（小型姿态指示）
//
//  为什么值得自绘一个：三个角度数字（俯仰/横滚/偏航）读起来要"换算成想象"，
//  而地平仪把**横滚**直接画成地平线倾斜、把**俯仰**画成地平线上下平移 ——
//  一眼就能判断"车是往前趴还是往右倒"，这正是平衡车调试与演示时最常问的问题。
//
//  观感纪律：**不用蓝/棕这类装饰色**（ISA-101：颜色只用于表达异常），
//  天地用两级中性灰区分，唯一彩色的地方是"超量程"时角标变琥珀。
//
//  ⚠️ 符号约定：内部按"横滚为正 = 车右侧下沉"处理（平衡车常用约定）。
//  若固件上报的符号与之相反，现象是"车身往右倒、地平线却往左斜"，
//  改这里一处的 `kRollSign` 即可（不要在每个面板里各改一遍）。
// ============================================================================

#include <QWidget>

class AttitudeIndicator : public QWidget
{
    Q_OBJECT
public:
    explicit AttitudeIndicator(QWidget *parent = nullptr);

    /// 设定姿态（单位：度）；valid=false 时显示"无数据"底纹
    void setAttitude(double rollDeg, double pitchDeg, bool valid = true);
    /// 量程（度）：超出后指针钳位并在角标提示"超量程"
    void setRange(double deg);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static constexpr double kRollSign = 1.0; ///< 见文件头符号约定

    double m_roll = 0.0;
    double m_pitch = 0.0;
    double m_range = 30.0;
    bool m_valid = false;
};
