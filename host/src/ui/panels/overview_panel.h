#pragma once

// ============================================================================
//  OverviewPanel —— 总览（P1 的主角面板）
//
//  它要在一屏之内回答四个问题（计划 §6.1 的"信息密度"要求）：
//    ① 车在哪、什么姿态、动不动？（姿态/运动）
//    ② 环境与电够不够？（气体/温湿度/电压）
//    ③ 任务走到哪一步？（任务阶段与进度）
//    ④ **现在有没有异常、锁没锁、数据可不可信？**（安全横幅 + 状态位 + 链路与来源）
//
//  三条实现纪律：
//    · UI **不解析任何字节** —— 只读 domain::VehicleState 的结构体；
//    · UI **不每帧重绘** —— 20 Hz 定时器拉取（计划 §五），且不可见时不刷新；
//    · **过期数据要看得出来** —— 数值超过协议周期数倍未更新时置灰并标"已过期"，
//      否则"画面冻住"会被当成"车停住了"（监控系统最危险的一类错误）。
// ============================================================================

#include "domain/vehicle_state.h"

#include <QWidget>

class QGroupBox;
class QLabel;
class QProgressBar;
class QTimer;

class AttitudeIndicator;
class LedIndicator;
class ValueCard;

namespace io {
class LinkManager;
}

class OverviewPanel : public QWidget
{
    Q_OBJECT
public:
    OverviewPanel(domain::VehicleState *state, io::LinkManager *mgr, QWidget *parent = nullptr);

public slots:
    /// 20 Hz 拉取刷新（计划 §五：UI 以 20 Hz 定时刷新，而不是每来一帧就重绘）
    void refresh();

public:
    /// 顶部安全横幅当前文本（测试断言用：横幅是"一眼看出异常"的唯一出口）
    QString bannerText() const;

private:
    QGroupBox *buildAttitudeGroup();
    QGroupBox *buildMotionGroup();
    QGroupBox *buildPowerEnvGroup();
    QGroupBox *buildTaskGroup();
    QGroupBox *buildDiagGroup();
    QGroupBox *buildLinkGroup();
    QGroupBox *buildBitsGroup();

    void setBanner(domain::Severity s, const QString &text);
    /// 某命令字的"来源 + 新鲜度"文本（两条链路都会发周期帧，来源必须能看见）
    QString sourceText(quint8 cmd) const;
    /// 数据是否新鲜（否则置灰 —— 冻住的画面比没有画面更危险）
    bool fresh(quint8 cmd, qint64 staleMs) const;

    domain::VehicleState *m_state = nullptr;
    io::LinkManager *m_mgr = nullptr;
    QTimer *m_timer = nullptr;

    QLabel *m_banner = nullptr;
    AttitudeIndicator *m_horizon = nullptr;

    // 姿态 / 运动
    ValueCard *m_pitch = nullptr;
    ValueCard *m_roll = nullptr;
    ValueCard *m_yaw = nullptr;
    ValueCard *m_gyroX = nullptr;
    ValueCard *m_gyroY = nullptr;
    ValueCard *m_gyroZ = nullptr;
    ValueCard *m_left = nullptr;
    ValueCard *m_right = nullptr;
    ValueCard *m_pwm = nullptr;
    ValueCard *m_dist = nullptr;
    QLabel *m_avoid = nullptr;

    // 电源与环境
    ValueCard *m_volt = nullptr;
    ValueCard *m_temp = nullptr;
    ValueCard *m_hum = nullptr;
    ValueCard *m_mq2 = nullptr;
    ValueCard *m_mq135 = nullptr;
    ValueCard *m_gear = nullptr;

    // 任务
    ValueCard *m_task = nullptr;
    ValueCard *m_phase = nullptr;
    ValueCard *m_checkpoint = nullptr;
    ValueCard *m_fault = nullptr;
    QProgressBar *m_progress = nullptr;

    // 诊断（0x15 / 0x16）
    ValueCard *m_heap = nullptr;
    ValueCard *m_oled = nullptr;
    ValueCard *m_fw = nullptr;
    ValueCard *m_page = nullptr;
    QLabel *m_algo = nullptr;
    QLabel *m_flagShow = nullptr;
    ValueCard *m_balPwm = nullptr;
    ValueCard *m_velPwm = nullptr;
    ValueCard *m_turnPwm = nullptr;

    // 链路与来源
    QLabel *m_wireState = nullptr;
    QLabel *m_wireFrames = nullptr;
    QLabel *m_wireBytes = nullptr;
    QLabel *m_wireAge = nullptr;
    QLabel *m_btState = nullptr;
    QLabel *m_btFrames = nullptr;
    QLabel *m_btBytes = nullptr;
    QLabel *m_btAge = nullptr;
    QLabel *m_primary = nullptr;
    QLabel *m_counters = nullptr;
    QLabel *m_sources = nullptr;

    // 状态位（LED）
    LedIndicator *m_ledAlarm = nullptr;
    LedIndicator *m_ledLatch = nullptr;
    LedIndicator *m_ledDht = nullptr;
    LedIndicator *m_ledMq2 = nullptr;
    LedIndicator *m_ledMq135 = nullptr;
    LedIndicator *m_ledComp = nullptr;
    LedIndicator *m_ledBtSeen = nullptr;
    LedIndicator *m_ledHeartbeat = nullptr;
    LedIndicator *m_ledSetPwm = nullptr;
    LedIndicator *m_ledNotExec = nullptr;
    LedIndicator *m_ledFlagStop = nullptr;
    LedIndicator *m_ledMotorDis = nullptr;
    LedIndicator *m_ledTick[4] = {nullptr, nullptr, nullptr, nullptr};
};
