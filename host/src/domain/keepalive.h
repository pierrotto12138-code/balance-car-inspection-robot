#pragma once

// ============================================================================
//  KeepaliveEngine —— 心跳保活（P3，安全相关）
//
//  为什么必须有它（协议 §11.5 第 6 条 / §五）：
//    固件侧规定"**500 ms 没收到任何下行帧** ⇒ 安全停车并**锁存**"，
//    而锁存之后**硬件按键也解不开**（必须上位机重下任务或显式使能）。
//    也就是说：上位机只要"安静"半秒，车就会停并锁住 —— 演示时看起来像"程序崩了"。
//    所以上位机必须周期性发**合法下行帧**（本引擎默认 200 ms，留 2.5 倍余量）。
//
//  ★★ 保活**只能用 0x1F 心跳**，不能用 `0x20 enable=1`：
//    协议 §3.6/v1.1 明确"收到显式使能（`0x20` 使能位=1 或 `0x21` 任务=自主巡航）即解除安全锁存"。
//    若把 `0x20 enable=1` 当保活周期发，就等于**每 200 ms 自动解除一次安全锁存** ——
//    急停/失联的保护会被自己的保活悄悄绕过（AGENTS 铁律 29 的"旁路"典型）。
//    因此本引擎的默认且唯一的自动保活方式是心跳；"使能保活"只能由人显式选择，
//    并且界面必须写明后果（见 KeepaliveMode 注释与 ui/panels/patrol_panel 的提示）。
//
//  设计上把"发送"做成注入的 Sink（`std::function`）：
//    这样它能脱离串口与界面单测（测试里塞一个假 sink，断言"发了什么、发了几条、
//    链路不通时是不是**不发**"），而真实实现就是 `io::LinkManager::send`。
// ============================================================================

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

#include <functional>

class QTimer;

namespace domain {

/// 保活方式
enum class KeepaliveMode {
    /// 0x1F 心跳：只声明"上位机在线"，**不解除锁存、不使能电机**（默认，唯一安全的自动方式）
    Heartbeat = 0,
    /// 0x20 enable=1 零速：会**解除安全锁存**、并让电机保持使能。
    /// 只用于"确实要让车保持使能"的场合（例如正在遥控），**不能**当保活默认值。
    EnableZeroSpeed = 1,
};

class KeepaliveEngine : public QObject
{
    Q_OBJECT
public:
    /// 发送函数：参数是"命令字 + 数据段"，返回实际写出的字节数（<0 表示失败）。
    /// 真实实现：`io::LinkManager::send`；测试里换成一个记录用的假 sink。
    using Sink = std::function<qint64(quint8 cmd, const QByteArray &data)>;

    explicit KeepaliveEngine(QObject *parent = nullptr);

    void setSink(Sink sink) { m_sink = std::move(sink); }

    /// 保活周期（毫秒）。默认 200；必须明显小于固件的 500 ms 判据。
    void setIntervalMs(int ms);
    int intervalMs() const { return m_intervalMs; }

    void setMode(KeepaliveMode mode);
    KeepaliveMode mode() const { return m_mode; }

    /// 开关保活。打开时**立刻发一条**（不等第一个周期）——
    /// "打开后还要再等 200 ms 才发第一条"正是最容易踩出 500 ms 超时的空隙。
    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }

    /// 链路是否可用。不可用时定时器继续跑但**不发**（也就不会假装"在保活"）——
    /// 界面据此显示"待链路"，而不是"保活中"这种会误导人的状态。
    void setLinkReady(bool ready);
    bool linkReady() const { return m_linkReady; }

    quint64 sentCount() const { return m_sentCount; }
    /// 距上次发送多少毫秒（<0 = 还没发过）
    qint64 sinceLastSendMs() const;
    /// 立即发一条（界面"立即保活"按钮）；返回是否真的发出去了
    bool sendNow();

    /// 该命令字对应的数据段（供测试与界面显示"发的是什么"）
    static QByteArray payloadFor(KeepaliveMode mode);
    /// 该命令字（0x1F 或 0x20）
    static quint8 commandFor(KeepaliveMode mode);

signals:
    void sent(quint8 cmd, qint64 bytes);
    void sendFailed(quint8 cmd);
    /// 任何会影响界面显示的状态变化（开关/模式/周期/链路可用性/发送计数）
    void stateChanged();

private:
    void onTick();

    Sink m_sink;
    QTimer *m_timer = nullptr;
    KeepaliveMode m_mode = KeepaliveMode::Heartbeat;
    int m_intervalMs = 200;
    bool m_enabled = false;
    bool m_linkReady = false;
    quint64 m_sentCount = 0;
    QElapsedTimer m_clock;
    qint64 m_lastSentMs = -1;
};

} // namespace domain
