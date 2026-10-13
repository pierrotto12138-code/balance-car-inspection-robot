#pragma once

// ============================================================================
//  链路抽象（上位机 io 层）
//
//  两条物理链路，协议相同（见 docs/protocol/communication_protocol.md §一）：
//    · Wire      = 有线，板上 MicroUSB → CH340G → USART1(PA9/PA10)
//    · Bluetooth = 蓝牙，USART2(PA2/PA3) 模块（串口透传 SPP）
//  两者在 PC 侧**都是串口**，区别只在用途：有线做主链路（调参/下载日志），
//  蓝牙做移动链路。协议约定"周期帧两路都发，应答回到请求所在链路"。
//
//  设计要点（为什么这么分）：
//    ① 解码在**工作线程**里做（SerialLink 自带一条线程），界面只收"已经解好的帧"；
//       这样即使以后速率提高，也不用改上层。
//    ② 跨线程只传**值类型**（QByteArray / 整数 / 已注册枚举），不共享缓冲。
//    ③ 统计数（收字节/丢帧/收发字节）由链路自己维护，界面直接取 ——
//       排障时最容易被误信的就是这几个数，所以让它们只有一个来源。
// ============================================================================

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

namespace io {

/// 链路种类（顺序即界面显示顺序）
enum class LinkKind : int {
    Wire = 0,      ///< 有线：CH340G/USART1
    Bluetooth = 1, ///< 蓝牙：USART2 透传模块
    Count = 2,
};

/// 链路状态
enum class LinkState : int {
    Closed = 0,  ///< 未打开
    Opening,     ///< 正在打开 / 正在重连
    Open,        ///< 已打开并可收发
    Error,       ///< 出错（错误文本见 stateChanged 的 detail）
};

/// 链路统计（界面诊断用；全部来自链路自身，单一来源）
struct LinkStats {
    quint64 bytesRx = 0;       ///< 累计收到字节
    quint64 bytesTx = 0;       ///< 累计发出字节
    quint64 framesOk = 0;      ///< 解帧成功数
    quint64 framesDropped = 0; ///< 校验失败/超时丢弃的帧数
    quint64 reconnects = 0;    ///< 重连次数（用于判断链路是否不稳定）

    /// 丢帧率（0~1）；无帧时为 0
    double dropRate() const
    {
        const quint64 total = framesOk + framesDropped;
        return total == 0 ? 0.0 : static_cast<double>(framesDropped) / static_cast<double>(total);
    }
};

/// 中文名（界面与日志统一用它，避免各处硬编码字符串）
const char *linkName(LinkKind kind);
const char *stateName(LinkState state);

/// 全部链路的抽象基类。
///
/// ★ 信号都带 `LinkKind`，因为界面上两条链路的卡片是同一个类画的；
///   也让 LinkManager 能原样转发而不必区分来源。
class LinkBase : public QObject
{
    Q_OBJECT
public:
    explicit LinkBase(LinkKind kind, QObject *parent = nullptr);
    ~LinkBase() override;

    LinkKind kind() const { return m_kind; }
    LinkState state() const { return m_state; }
    virtual LinkStats stats() const { return m_stats; }

    /// 打开链路（异步：立即返回，结果通过 stateChanged 通知）
    virtual void open() = 0;
    /// 关闭链路（同步）
    virtual void close() = 0;
    virtual bool isOpen() const { return m_state == LinkState::Open; }

    /// 发送一帧（返回实际写出的字节数；未打开返回 -1）
    virtual qint64 sendCommand(std::uint8_t cmd, const QByteArray &data) = 0;

    /// 供子类更新状态/统计并对外通知
signals:
    /// 解出一帧（cmd + 数据段）
    void frameReady(io::LinkKind kind, quint8 cmd, const QByteArray &data);
    /// 原始字节（给"报文日志"面板做 HEX 显示；可能被上层限流）
    void rawBytes(io::LinkKind kind, const QByteArray &bytes);
    /// 状态变化
    void stateChanged(io::LinkKind kind, io::LinkState state, const QString &detail);
    /// 统计变化（低频通知，界面按需刷新）
    void statsChanged(io::LinkKind kind, quint64 bytesRx, quint64 bytesTx, quint64 framesOk,
                      quint64 framesDropped);

protected:
    void setState(LinkState st, const QString &detail);
    void bumpRx(qint64 n) { m_stats.bytesRx += static_cast<quint64>(n); }
    void bumpTx(qint64 n) { m_stats.bytesTx += static_cast<quint64>(n); }
    void bumpFrame(bool ok)
    {
        if (ok) {
            ++m_stats.framesOk;
        } else {
            ++m_stats.framesDropped;
        }
    }
    void emitStats();

    LinkStats m_stats;

private:
    LinkKind m_kind;
    LinkState m_state = LinkState::Closed;
};

/// 注册跨线程信号要用的元类型（main / LinkManager 启动时调用一次即可）
void registerLinkMetaTypes();

} // namespace io

Q_DECLARE_METATYPE(io::LinkKind)
Q_DECLARE_METATYPE(io::LinkState)
