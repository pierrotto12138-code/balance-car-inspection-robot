#pragma once

#include <QMainWindow>

namespace io {
class LinkManager;
}

/// 上位机主窗口。
///
/// P0 阶段：以"通信链路"面板为主（连接、实时统计、报文日志），
/// 另保留一个"协议自检"页作为环境体检。
/// 后续按 docs/design_host/C部分方案与计划.md 的 P1~P4 补齐状态/气体/巡检/调参面板。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(io::LinkManager *mgr, QWidget *parent = nullptr);

private:
    /// 启动时跑一次协议自检（组帧 → 解帧 → 比对），把结果显示出来，
    /// 用作"协议层已就绪"的可视证据。
    QString runProtocolSelfTest();

    io::LinkManager *m_mgr = nullptr;
    quint64 m_frames = 0;
};
