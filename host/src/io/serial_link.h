#pragma once

// ============================================================================
//  真实串口链路（有线 / 蓝牙都一样，只是端口不同）
//
//  线程模型（关键，决定界面会不会卡）：
//    主线程                      工作线程 "link-<kind>"
//    ─────────                   ─────────────────────────
//    SerialLink（对外接口）  ──►  Reader（阻塞读 waitForReadyRead）
//      ▲                            │  读到的字节先喂 FrameDecoder 解帧
//      │  queued 信号（值类型）      │
//      └────────────────────────────┘  frameReady / rawBytes / stateChanged / statsChanged
//
//  为什么用**阻塞读 + 独立线程**而不是 readyRead 信号：
//    事件驱动在高速率下会"每几个字节触发一次信号"，抖动大且把解析带回主线程；
//    阻塞读线程天然把"等待"变成"睡眠"，代价是每条链路一个线程（共 2 条，可接受）。
//    参考 Qt 官方 Blocking Receiver 范式（qtserialport-blockingreceiver-example）。
// ============================================================================

#include "io/link.h"

#include <QString>
#include <QThread>

class QSerialPort;

namespace io {

/// 串口参数
struct SerialConfig {
    QString portName;              ///< 如 "COM3"
    int baudRate = 115200;         ///< 协议规定 115200（蓝牙模块也改到 115200）
    bool autoReconnect = true;     ///< 断开后自动重连（演示时很有用）
    int reconnectDelayMs = 1000;   ///< 重连间隔
};

class SerialLink : public LinkBase
{
    Q_OBJECT
public:
    explicit SerialLink(LinkKind kind, QObject *parent = nullptr);
    ~SerialLink() override;

    void setConfig(const SerialConfig &cfg);
    SerialConfig config() const { return m_cfg; }

    void open() override;
    void close() override;
    qint64 sendCommand(std::uint8_t cmd, const QByteArray &data) override;

    /// 可用串口列表（界面下拉框用；**不打开端口**，只枚举）
    static QStringList availablePorts();

private:
    class Reader; // 工作线程里的读取者（定义在 .cpp，避免把 QSerialPort 暴露到头文件）

    void startReader();
    void stopReader();
    void onStateFromReader(int state, const QString &detail);

    SerialConfig m_cfg;
    QThread *m_thread = nullptr;
    Reader *m_reader = nullptr;
};

} // namespace io
