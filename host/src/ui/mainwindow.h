#pragma once

#include <QMainWindow>

class QLabel;

/// 上位机主窗口。
///
/// 当前为骨架：验证 Qt 构建链与协议层可用。后续按设计文档补齐
/// 状态监测面板、气体与环境面板、远程控制面板、事件日志面板。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    /// 启动时跑一次协议自检（组帧 → 解帧 → 比对），把结果显示出来，
    /// 用作"协议层已就绪"的可视证据。
    QString runProtocolSelfTest();

    QLabel *m_statusLabel = nullptr;
};
