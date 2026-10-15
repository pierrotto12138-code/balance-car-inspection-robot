#pragma once

// ============================================================================
//  上位机主窗口
//
//  导航（左侧 QListWidget → 中央 QStackedWidget）：
//    ① 总览          姿态/运动/电源环境/任务/诊断/状态位 + 安全横幅（P1）
//    ② 曲线与告警    四张通道独立的实时曲线 + 阈值线 + 告警标记（P2）
//    ③ 事件流        0x13 事件 + 本机告警，安全事件成对显示停车时长（P2）
//    ④ 通信链路      双链路连接控制、统计、HEX 报文日志（P0）
//    ⑤ 协议自检      协议层就绪的可视证据
//  状态条（常驻）：两条链路 · 电池 · 安全/告警 · 帧计数 · **假源水印**
//
//  ⚠️ 状态条**不显示"平滑后"的安全信息**（协议 §七 约定 10：安全相关的显示必须原样）：
//  它直接读 `domain::VehicleState` + `domain::AlarmEngine`，与总览横幅、曲线标记
//  是同一份判定结果，不会出现"两处口径打架"。
//
//  ★ 界面内部的连线（面板 ↔ 信号）在这里做；跨层的装配（io → domain → data）在 main.cpp。
// ============================================================================

#include <QMainWindow>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace data {
class EventLog;
}
namespace domain {
class AlarmEngine;
class VehicleState;
}
namespace io {
class LinkManager;
}

class CurvePanel;
class EventPanel;
class OverviewPanel;
class P0Panel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(io::LinkManager *mgr, domain::VehicleState *state, domain::AlarmEngine *alarm,
               data::EventLog *log, QWidget *parent = nullptr);

    /// 切换左侧导航页（0=总览 1=曲线与告警 2=事件流 3=通信链路 4=协议自检）
    /// —— `--shot` 自动截图与自检脚本都用它
    void showPage(int index);
    /// 当前导航页号（测试断言用）
    int currentPage() const;
    /// 左侧导航页标题（测试/文档用，避免文档里的页号与实际菜单脱节）
    QString pageTitle(int index) const;

private slots:
    /// 状态条刷新（5 Hz：状态条不需要 20 Hz，且它是常驻的）
    void refreshStatusBar();

private:
    QString runProtocolSelfTest();
    QWidget *buildSelfTestPage();
    void buildMenus();
    void buildStatusBar();
    /// F11 快速截图（直接服务"实验记录与验收材料"）
    void quickScreenshot();

    io::LinkManager *m_mgr = nullptr;
    domain::VehicleState *m_state = nullptr;
    domain::AlarmEngine *m_alarm = nullptr;
    data::EventLog *m_log = nullptr;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    OverviewPanel *m_overview = nullptr;
    CurvePanel *m_curves = nullptr;
    EventPanel *m_events = nullptr;
    P0Panel *m_p0 = nullptr;

    QLabel *m_stWire = nullptr;
    QLabel *m_stBt = nullptr;
    QLabel *m_stBattery = nullptr;
    QLabel *m_stSafety = nullptr;
    QLabel *m_stAlarm = nullptr;
    QLabel *m_stFrames = nullptr;
    QLabel *m_stDemo = nullptr;

    int m_shotCount = 0;
};
