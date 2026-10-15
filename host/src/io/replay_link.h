#pragma once

// ============================================================================
//  回放/假源链路（离线验证与演示兜底）
//
//  用途有两个，都很实在：
//    ① **没有板子也能开发与测试**：P0 的全部功能（解帧、统计、HEX 日志、调参往返）
//       都能在假源上跑通；板子在焊接期间 C 侧不必干等。
//    ② **演示应急预案**：硬件坏在现场时，用它跑一段真实录制的数据把界面演完
//       （见 docs/design_host/C部分方案与计划.md §八 风险对策）。
//
//  两种数据来源：
//    · 合成（默认）：按协议真实周期造 0x10/0x11/0x12/0x13/0x14/0x15/0x16/0x1F 帧，
//      数值是可信的假数据（气体每 30 s 冲高一次、每 60 s 演一次"急停→解除"、
//      电压会掉到报警线以下），并且会**故意掺入**可配置比例的坏帧，
//      用来验证"丢帧计数"确实在工作。
//    · 文件：逐行十六进制（`AA 55 ...` 或 `AA55...`），可用 `tune.py --observe` 录真实数据。
//
//  ★ 假源必须覆盖**界面要用到的每一种帧**：只要有一种帧假源不发，那一块界面
//    在离线开发时就是"永远空白"，而人很容易把它当成"界面写错了"去瞎改
//    （P0 的 0x14 少发两个保留字节就是这么被发现的）。加帧时请同步更新
//    tests/test_link.cpp 的 `syntheticFramesMatchProtocolLengths` 长度表。
//
//  ★ 它继承同一个 LinkBase，所以上层（LinkManager / 界面）**完全不用区分真假**。
// ============================================================================

#include "io/link.h"

#include "codec.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QVector>

class QTimer;

namespace io {

class ReplayLink : public LinkBase
{
    Q_OBJECT
public:
    explicit ReplayLink(LinkKind kind, QObject *parent = nullptr);
    ~ReplayLink() override;

    void open() override;
    void close() override;
    qint64 sendCommand(std::uint8_t cmd, const QByteArray &data) override;

    /// 速率倍数（1.0 = 按协议真实周期；10.0 = 10 倍压测）
    void setRateMultiplier(double x);
    /// 坏帧比例（0~1），用于验证丢帧统计
    void setCorruptRatio(double ratio);
    /// 从文件加载录制数据（十六进制逐行）；返回读到的帧数，失败返回 -1
    int loadCapture(const QString &path);
    /// 生成的帧总数（测试用）
    quint64 generated() const { return m_generated; }
    /// 是否处于合成模式（未加载文件）
    bool synthetic() const { return m_capture.isEmpty(); }

signals:
    void replyEmitted(quint8 cmd, const QByteArray &data); // 假源回帧（测试可断言）

private:
    void onTick();
    void emitRawFrame(const QByteArray &bytes);
    /// 到期判定：返回"本拍应补发的帧数"（0 = 未到期；高倍数下可 >1）
    int dueCount(quint64 &last, quint64 periodMs);
    QByteArray makePose(quint32 tMs);
    QByteArray makeMotion(quint32 tMs);
    QByteArray makeTask(quint32 tMs);
    QByteArray makeEnvGas(quint32 tMs);
    QByteArray makeEvent(proto::EventCode code, quint32 stampMs);
    QByteArray makeDiag(quint32 tMs);
    QByteArray makeLoop(quint32 tMs);

    /// 定时器基准节拍（毫秒）；虚拟时间按 kBaseTickMs × 倍数 推进
    static constexpr int kBaseTickMs = 5;
    /// 单拍最多补发帧数（防"倍数调得极大"时一拍跑飞）
    static constexpr int kMaxCatchUp = 512;

    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock; ///< 实测经过时间（虚拟时间的基准，不受定时器粒度影响）
    QVector<QByteArray> m_capture; ///< 文件模式下的帧序列（已含完整帧）
    int m_captureIdx = 0;
    int m_tickCount = 0;
    quint32 m_t0 = 0;
    double m_rate = 1.0;
    double m_corruptRatio = 0.0;
    quint64 m_generated = 0;

    // ---- 虚拟时间与各帧类型的上次发送时刻（决定真实产帧速率）----
    quint64 m_virtualMs = 0;
    quint64 m_lastPose = 0;
    quint64 m_lastMotion = 0;
    quint64 m_lastTask = 0;
    quint64 m_lastEnv = 0;
    quint64 m_lastDiag = 0;
    quint64 m_lastLoop = 0;
    quint64 m_lastHb = 0;

    // ---- 演练用：一次性事件是否已发 + 气体冲高的窗口序号 ----
    static constexpr int kOneShotCount = 6;
    bool m_eventFired[kOneShotCount] = {false, false, false, false, false, false};
    qint64 m_lastSpikeIdx = -1;

    proto::FrameDecoder m_decoder; ///< 假源也要真的解帧（与真串口同一套代码路径）
};

} // namespace io
