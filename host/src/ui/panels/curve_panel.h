#pragma once

// ============================================================================
//  CurvePanel —— 曲线与告警（P2 主角）
//
//  四张**通道独立**的图（厂商 PC 工具"通道独立"做法的取舍：通道多了叠在一起看不清）：
//    MQ-2 浓度 / MQ-135 浓度 / 电池电压 / 温湿度
//  每张图上方一个数值牌（当前值，异常时按告警级别着色），右上角显示十字定标读数。
//
//  三条实现纪律：
//    ① **采样≠重绘**：采样由 `VehicleState::dataUpdated`（每帧一次）驱动，
//       重绘由 10 Hz 定时器驱动 —— 绝不"来一帧重绘一次"；
//    ② **阈值线是"参考"，判定在 domain**：这里的线只画出来，报不报警由
//       `domain::AlarmEngine` 说了算（否则界面上会出现两套判据）；
//    ③ **告警要在曲线上留痕**：发生/解除都在当时的时间点打竖线 + 标签，
//       这样"什么时候开始超限、持续多久"一眼可见（答辩与复盘都要这个）。
// ============================================================================

#include "domain/alarm.h"
#include "domain/vehicle_state.h"

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

class ScopePlot;
class ValueCard;

namespace io {
class LinkManager;
}

class CurvePanel : public QWidget
{
    Q_OBJECT
public:
    CurvePanel(domain::VehicleState *state, io::LinkManager *mgr, domain::AlarmEngine *alarm,
               QWidget *parent = nullptr);

public slots:
    /// 每成功解出一帧调用一次：在这里**采一个样**（0x14 环境 / 0x15 诊断）
    void onDataUpdated(quint8 cmd);
    /// 10 Hz：重绘 + 刷新当前值卡片
    void replotAll();
    /// 告警发生/解除：在对应曲线上打标记
    void onAlarmRaised(const domain::AlarmRecord &r);
    void onAlarmCleared(const domain::AlarmRecord &r);
    /// 暂停更新（冻结画面读波形；恢复时把暂停期间的点补进来）
    void setPaused(bool on);
    bool isPaused() const { return m_paused; }

private:
    double nowSec() const { return static_cast<double>(m_clock.elapsed()) / 1000.0; }
    void buildUi();
    void applyThresholdsToUi();
    void onThresholdEdited();
    void onWindowChanged();
    void updateCards();

    domain::VehicleState *m_state = nullptr;
    io::LinkManager *m_mgr = nullptr;
    domain::AlarmEngine *m_alarm = nullptr;

    QElapsedTimer m_clock;
    QTimer *m_timer = nullptr;
    bool m_paused = false;

    ScopePlot *m_mq2Plot = nullptr;
    ScopePlot *m_mq135Plot = nullptr;
    ScopePlot *m_voltPlot = nullptr;
    ScopePlot *m_thPlot = nullptr;

    ValueCard *m_mq2Card = nullptr;
    ValueCard *m_mq135Card = nullptr;
    ValueCard *m_voltCard = nullptr;
    ValueCard *m_thCard = nullptr;

    QDoubleSpinBox *m_mq2Limit = nullptr;
    QDoubleSpinBox *m_mq135Limit = nullptr;
    QComboBox *m_windowBox = nullptr;
    QCheckBox *m_autoYBox = nullptr;
    QCheckBox *m_endValueBox = nullptr;
    QLabel *m_cursor = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_resetView = nullptr;
    QPushButton *m_pauseBtn = nullptr;
};
