#pragma once

// ============================================================================
//  P0 面板：通信链路诊断（连接、实时统计、报文日志）
//
//  它是 P0 阶段的验收界面，也是后续所有面板的"体检入口"：
//    · 两条链路各自：实现方式（真实串口 / 假源）、端口、波特率、连接状态
//    · 实时统计：收/发字节、解帧数、丢帧数、丢帧率、重连次数、帧率（帧/s）
//    · 报文日志：十六进制收发热流水（可暂停、可清空、有行数上限）
//
//  为什么先把这块做扎实：排查"收不到数据"时，第一现场永远是"链路通不通、
//  帧在不在涨、丢帧是不是在涨" —— 这三个数对了，才轮到怀疑解析和界面。
// ============================================================================

#include "io/link.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTimer;

namespace io {
class LinkManager;
}

class P0Panel : public QWidget
{
    Q_OBJECT
public:
    explicit P0Panel(io::LinkManager *mgr, QWidget *parent = nullptr);

    /// 帧率统计（最近 1 s）
    double frameRate(io::LinkKind kind) const;

private slots:
    void refreshStats();
    void onLinkState(io::LinkKind kind, io::LinkState state, const QString &detail);
    void onRawBytes(io::LinkKind kind, const QByteArray &bytes);
    void onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data);
    void onModeChanged(io::LinkKind kind);
    void onConnectClicked(io::LinkKind kind);
    void onResetCounters();
    void onClearLog();

private:
    struct LinkUi {
        QComboBox *mode = nullptr;     // 真实串口 / 假源
        QComboBox *port = nullptr;     // 端口
        QComboBox *baud = nullptr;     // 波特率
        QPushButton *connect = nullptr;
        QLabel *state = nullptr;
        QLabel *stats = nullptr;
    };

    QWidget *buildLinkGroup(io::LinkKind kind);
    void appendLog(const QString &line);
    void refreshPorts(io::LinkKind kind);

    io::LinkManager *m_mgr = nullptr;
    LinkUi m_ui[static_cast<int>(io::LinkKind::Count)];
    QPlainTextEdit *m_log = nullptr;
    QCheckBox *m_pauseLog = nullptr;
    QPushButton *m_clearLog = nullptr;
    QPushButton *m_resetCounters = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
    QDoubleSpinBox *m_corrupt = nullptr;
    QLabel *m_primary = nullptr;

    QTimer *m_refresh = nullptr;
    quint64 m_lastFrames[static_cast<int>(io::LinkKind::Count)] = {0, 0};
    double m_rateFps[static_cast<int>(io::LinkKind::Count)] = {0.0, 0.0};
};
