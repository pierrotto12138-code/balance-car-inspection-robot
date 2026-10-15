#pragma once

// ============================================================================
//  EventLog —— 事件流（data 层）
//
//  它把两类东西按**同一根时间轴**记在一起：
//    ① 机器人发来的事件帧 `0x13`（带机器人侧 32 位时间戳）；
//    ② 上位机自己判出来的告警发生/解除（`domain::AlarmEngine`）。
//
//  为什么必须同轴：复盘时人问的是"**什么时候**开始超限、**停了多久**"。
//  只有把"事件给时刻、标志给状态"（协议 §七 约定 9）与本地判定放在一条线上，
//  才能直接回答"`0x0A` 急停 → `0x09` 解除"之间发生了什么。
//
//  ★ 安全事件成对显示：`0x08`/`0x0A` 记下时刻，后到的 `0x09` 带出**停车时长** ——
//    这是"急停→解除"这段时间的留档，也是任务书里"安全"一项最直接的证据。
//
//  本文件只依赖 Qt6::Core（不依赖界面），可脱离硬件与界面单测。
// ============================================================================

#include "domain/alarm.h"
#include "domain/vehicle_state.h"
#include "io/link.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>

namespace data {

/// 事件流里的一条记录
struct EventEntry {
    qint64 localMs = 0;   ///< 会话内相对时刻（本地时钟，用于排序与"多久以前"）
    QDateTime wall;       ///< 墙上时间（界面显示与 P4 落盘用）
    io::LinkKind kind = io::LinkKind::Count;
    quint8 cmd = 0;       ///< 0x13 = 机器人事件；0 = 上位机本地产生的条目
    quint8 code = 0;      ///< 事件码（仅 cmd=0x13 有意义）
    quint32 robotTsMs = 0; ///< 机器人侧时间戳（仅 cmd=0x13）
    bool local = false;   ///< true = 本机判定（告警发生/解除）
    bool raised = true;   ///< 告警条目：true=发生，false=解除
    bool safety = false;  ///< 是否安全通道事件（0x08/0x09/0x0A）
    QString title;        ///< 一句话内容（事件名 / 告警标题）
    QString detail;       ///< 补充（数值/阈值/停车时长…）
    qint64 sinceSafetyStopMs = -1; ///< `0x09` 条目：距上次停车的时长
};

class EventLog : public QObject
{
    Q_OBJECT
public:
    explicit EventLog(QObject *parent = nullptr);

    /// 内存里保留的最大条数（界面只显示最近若干条；P4 落盘不受此限）
    static constexpr int kMaxEntries = 5000;

    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    const EventEntry &at(int index) const { return m_entries.at(index); }
    QVector<EventEntry> entries() const { return m_entries; }
    /// 最短的安全停车时长（毫秒）；没有配对的"停车→解除"返回 -1。
    /// 用途：P3 的"安全"验收要有可量化的证据，而不是"看起来停过"。
    qint64 shortestSafetyStopMs() const;

public slots:
    /// 机器人事件帧（0x13）—— 由 `domain::VehicleState::eventReceived` 驱动。
    ///
    /// ★ 为什么不再直接接 `frameReady`：两条链路都会发同样的事件帧（协议 §七 约定 6），
    ///   直接接原始帧会让**每条事件在流里出现两遍**（复盘时会误以为急停发生了两次）。
    ///   现在去重统一在 `VehicleState` 做（它本来就要按命令字记录"最近一帧"），
    ///   这里只收"确实是一次新事件"的通知。
    void onEvent(const domain::EventData &e, io::LinkKind kind);
    /// 告警发生/解除（由 `domain::AlarmEngine` 驱动）
    void onAlarmRaised(const domain::AlarmRecord &rec);
    void onAlarmCleared(const domain::AlarmRecord &rec);
    void clear();

signals:
    void entryAdded(const data::EventEntry &e);
    void cleared();

private:
    void append(EventEntry e);
    QString alarmDetail(const domain::AlarmRecord &r, bool raised) const;

    QVector<EventEntry> m_entries;
    qint64 m_lastSafetyStopMs = -1; ///< 最近一次 0x08/0x0A 的时刻（用于 0x09 配对）
    qint64 m_shortestStopMs = -1;
    QElapsedTimer m_clock;
};

} // namespace data

Q_DECLARE_METATYPE(data::EventEntry)
