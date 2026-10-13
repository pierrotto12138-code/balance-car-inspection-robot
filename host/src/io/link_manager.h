#pragma once

// ============================================================================
//  链路管理器：把两条链路（有线 / 蓝牙）收成一个入口
//
//  它负责三件事，界面因此可以很简单：
//    ① 按"真实串口 / 假源回放"两种模式创建链路（**上层不用区分真假**）；
//    ② 转发解好的帧与统计（界面只连这一个对象）；
//    ③ 决定"命令发给谁"：有线优先，不可用时退到蓝牙 —— 与协议 §七 约定 6 一致
//       （周期帧两路都发是固件侧行为；PC 侧下发命令默认走主链路）。
//
//  为什么要有"模式"这一层：板子在焊接期间，C 侧必须能用假源把 P0~P2 全部做完；
//  演示现场硬件出问题时，也靠它把界面演完（应急预案）。
// ============================================================================

#include "io/link.h"
#include "io/serial_link.h"

#include <QObject>
#include <QString>

namespace io {

class ReplayLink;

class LinkManager : public QObject
{
    Q_OBJECT
public:
    /// 每条链路的实现方式
    enum class Mode : int {
        Serial = 0, ///< 真实串口
        Replay = 1, ///< 假源（合成数据或录制文件回放）
    };
    Q_ENUM(Mode)

    explicit LinkManager(QObject *parent = nullptr);
    ~LinkManager() override;

    /// 当前模式
    Mode mode(LinkKind kind) const;
    /// 切换实现方式（会先关掉旧链路）
    void setMode(LinkKind kind, Mode mode);

    SerialConfig serialConfig(LinkKind kind) const { return m_cfg[static_cast<int>(kind)]; }
    void setSerialConfig(LinkKind kind, const SerialConfig &cfg);

    /// 假源参数：速率倍数与坏帧比例（两条假源共用）
    void setReplayRate(double x);
    void setReplayCorruptRatio(double r);
    /// 给指定链路加载录制文件（十六进制逐行）；返回帧数，-1 表示失败
    int loadCapture(LinkKind kind, const QString &path);

    LinkBase *link(LinkKind kind) const;
    LinkStats stats(LinkKind kind) const;
    bool isOpen(LinkKind kind) const;

    void open(LinkKind kind);
    void openAll();
    void close(LinkKind kind);
    void closeAll();

    /// 向"主链路"发命令：有线优先，未连接则用蓝牙；两条都没开返回 -1
    qint64 send(std::uint8_t cmd, const QByteArray &data);
    /// 向指定链路发命令
    qint64 sendTo(LinkKind kind, std::uint8_t cmd, const QByteArray &data);

    /// 实际选中的主链路（未连接时返回 LinkKind::Count）
    LinkKind primaryLink() const;

    /// 串口枚举（界面下拉框）
    static QStringList availablePorts() { return SerialLink::availablePorts(); }

signals:
    void frameReady(io::LinkKind kind, quint8 cmd, const QByteArray &data);
    void rawBytes(io::LinkKind kind, const QByteArray &bytes);
    void linkStateChanged(io::LinkKind kind, io::LinkState state, const QString &detail);
    void linkStatsChanged(io::LinkKind kind, quint64 bytesRx, quint64 bytesTx, quint64 framesOk,
                          quint64 framesDropped);
    /// 主链路变化（界面据此高亮）
    void primaryChanged(io::LinkKind kind);

private:
    void createLink(LinkKind kind, Mode mode);
    void destroyLink(LinkKind kind);
    LinkKind primary() const;

    LinkBase *m_links[static_cast<int>(LinkKind::Count)] = {nullptr, nullptr};
    Mode m_modes[static_cast<int>(LinkKind::Count)] = {Mode::Serial, Mode::Serial};
    SerialConfig m_cfg[static_cast<int>(LinkKind::Count)];
    double m_rate = 1.0;
    double m_corrupt = 0.0;
};

} // namespace io
