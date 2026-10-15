#pragma once

// ============================================================================
//  上位机主窗口
//
//  结构（与计划 §6.1 一致）：
//    左侧导航（QListWidget）→ 中央 QStackedWidget
//      ① 总览（P1：姿态/运动/电源环境/任务/诊断/状态位）
//      ② 通信链路（P0：双链路连接控制、统计、HEX 报文日志）
//      ③ 协议自检（协议层就绪的可视证据）
//    状态栏（常驻）：两条链路状态 · 电池电压 · 安全状态 · 帧计数
//
//  ⚠️ 状态栏**不显示"平滑后"的安全信息**（协议 §七 约定 10：安全相关的显示必须原样）。
//  它直接读 domain::VehicleState，与总览横幅同一个来源 —— 两处口径不会打架。
// ============================================================================

#include <QMainWindow>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace domain {
class VehicleState;
}
namespace io {
class LinkManager;
}

class OverviewPanel;
class P0Panel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(io::LinkManager *mgr, domain::VehicleState *state, QWidget *parent = nullptr);

    /// 切换左侧导航页（0=总览 1=通信链路 2=协议自检）——供 `--shot` 自动截图使用
    void showPage(int index);

private slots:
    /// 状态栏刷新（5 Hz：状态栏不需要 20 Hz，且它是常驻的）
    void refreshStatusBar();

private:
    /// 启动时跑一次协议自检（组帧 → 解帧 → 比对），把结果显示出来，
    /// 用作"协议层已就绪"的可视证据。
    QString runProtocolSelfTest();
    QWidget *buildSelfTestPage();
    void buildMenus();

    io::LinkManager *m_mgr = nullptr;
    domain::VehicleState *m_state = nullptr;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    OverviewPanel *m_overview = nullptr;
    P0Panel *m_p0 = nullptr;

    QLabel *m_stWire = nullptr;
    QLabel *m_stBt = nullptr;
    QLabel *m_stBattery = nullptr;
    QLabel *m_stSafety = nullptr;
    QLabel *m_stFrames = nullptr;
};
