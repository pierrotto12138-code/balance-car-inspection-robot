#pragma once

// ============================================================================
//  PatrolPanel —— 巡检可视化与任务控制（P3）
//
//  它把"操作"和"安全"放在同一个页面上，因为它们本来就是一件事：
//    · 任务状态与**阶段时间轴**（初始化→预热→待机→巡航→研判→返航→完成，故障单列红牌）
//    · 任务控制：`0x21` 下发（巡航/返航/待机）、`0x2F` 急停、**重下任务解除锁存**
//    · 保活（`domain::KeepaliveEngine`）：默认 200 ms 心跳，防止固件 500 ms 判失联而锁存
//    · 遥控（`0x20`）：按住走、松手停（`enable=0` 立即停）
//
//  ★ 两条安全纪律（都有依据，且都在界面上写明白）：
//    ① **锁存期间禁止起步**（协议 §3.6）：方向键/巡航/返航在锁存时**禁用**，
//       并提示"请用重下任务解除"—— 靠禁用而不是靠提示语，因为人不会读提示语；
//    ② **保活只能用 0x1F 心跳**：使能类保活（`0x20 enable=1`）会每 200 ms 自动解除一次
//       安全锁存，等于把急停保护绕过（AGENTS 铁律 29）。这一项由人显式选择，且旁边写明后果。
// ============================================================================

#include "domain/keepalive.h"
#include "domain/vehicle_state.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

class ValueCard;

namespace data {
class EventLog;
}
namespace domain {
class AlarmEngine;
}
namespace io {
class LinkManager;
}

class PatrolPanel : public QWidget
{
    Q_OBJECT
public:
    PatrolPanel(domain::VehicleState *state, io::LinkManager *mgr, domain::AlarmEngine *alarm,
                domain::KeepaliveEngine *keepalive, data::EventLog *log, QWidget *parent = nullptr);

protected:
    /// 键盘遥控（W/A/S/D 按住走、松手停；空格停）—— 必须重写 keyPress/keyRelease，
    /// 因为"按住"这件事只有按键事件能表达（QShortcut 只有"激活"没有"松开"）
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

public slots:
    /// 10 Hz 刷新（状态卡、阶段时间轴、按钮可用性、保活状态）
    void refresh();

private:
    void buildUi();
    /// 下发一条命令并留档（返回是否成功）
    bool send(quint8 cmd, const QByteArray &payload, const QString &detail);
    void sendTask(proto::TaskId id);
    void sendMotion(int linMmS, int angDpsX10);
    void sendStop();
    /// 按下方向（按钮或键盘）：立刻发一条，并启动 10 Hz 重发
    void startMotion(int linMmS, int angDpsX10);
    /// 松手/停止：停掉重发并立刻发 enable=0
    void stopMotion();
    void sendEstop();
    /// 锁存时禁用"会解除锁存"的起点类操作
    void applyInterlocks(bool latched);
    void updatePhaseChips(const domain::TaskData &task, bool valid);
    void updateKeepaliveUi();

    domain::VehicleState *m_state = nullptr;
    io::LinkManager *m_mgr = nullptr;
    domain::AlarmEngine *m_alarm = nullptr;
    domain::KeepaliveEngine *m_keepalive = nullptr;
    data::EventLog *m_log = nullptr;

    QLabel *m_banner = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_lastSent = nullptr;

    ValueCard *m_task = nullptr;
    ValueCard *m_phase = nullptr;
    ValueCard *m_checkpoint = nullptr;
    ValueCard *m_fault = nullptr;
    QProgressBar *m_progress = nullptr;

    QVector<QLabel *> m_chips; ///< 阶段时间轴（顺序 = proto::Phase 0..6）
    QLabel *m_faultChip = nullptr;

    QPushButton *m_btnPatrol = nullptr;
    QPushButton *m_btnReturn = nullptr;
    QPushButton *m_btnIdle = nullptr;
    QPushButton *m_btnEstop = nullptr;
    QPushButton *m_btnRelease = nullptr;
    QPushButton *m_btnFront = nullptr;
    QPushButton *m_btnBack = nullptr;
    QPushButton *m_btnLeft = nullptr;
    QPushButton *m_btnRight = nullptr;
    QPushButton *m_btnStop = nullptr;

    QCheckBox *m_kaEnable = nullptr;
    QSpinBox *m_kaInterval = nullptr;
    QComboBox *m_kaMode = nullptr;
    QLabel *m_kaState = nullptr;
    QLabel *m_kaWarn = nullptr;

    QTimer *m_timer = nullptr;
    /// 按住期间的重发定时器（10 Hz）与当前方向 —— 固件对 0x20 有 **500 ms 看门狗**，
    /// 只发一条会变成"按一下只动半秒"（本项目 2026-10-15 真机踩到，用户报"遥控不行"）
    QTimer *m_holdTimer = nullptr;
    bool m_holdActive = false;
    int m_holdLin = 0;
    int m_holdAng = 0;
    bool m_lastLatched = false;
};
