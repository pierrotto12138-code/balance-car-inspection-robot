#pragma once

// ============================================================================
//  VehicleState —— 机器人当前状态的**唯一聚合处**（domain 层）
//
//  它回答四个界面必须回答的问题：
//    ① 现在是什么状态？（姿态/运动/任务/环境/诊断，逐字段结构体）
//    ② 这个值是谁给的、多久以前？（两条链路都会发周期帧 —— 协议 §七 约定 6；
//       界面必须能说清"最后一次有效值来自哪条链路、新鲜度多少"，
//       否则两条链路的数据会"互相打架"且没人看得出来）
//    ③ 有没有异常？锁没锁？（P1 给出最小集：低压、报警位、安全锁存；
//       P2 再扩成带迟滞的 alarm 模块）
//    ④ 数据本身可信吗？（格式错帧数、未知命令数、每链路帧计数、链路超时）
//
//  ★ 为什么不让界面自己解析：
//    界面按帧重绘必然卡顿（计划 §五），所以 UI 用 20 Hz 定时器**拉取**这里的状态；
//    拉取要求"状态是值的集合"，而不是"一堆回调"。解析与合并只做一次，就放在这里。
//
//  ★ 为什么事件（0x13）单独发信号：
//    "事件给时刻、标志给状态"（协议 §七 约定 9）—— 状态会被下一帧覆盖，
//    事件流却要留档（P2 的事件面板 / P4 的 SQLite），所以两条路都要有。
//
//  本文件只依赖 QtCore + io 的链路枚举（值类型，不依赖任何链路实现），可直接单测。
// ============================================================================

#include "domain/frames.h"
#include "io/link.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

namespace domain {

/// 严重程度（界面据此着色；ISA-101：颜色只用于表达异常）
enum class Severity {
    Unknown = 0, ///< 无数据（灰）
    Ok,          ///< 正常（中性色/绿只用于状态灯）
    Warn,        ///< 警示（琥珀）
    Alarm,       ///< 报警/危险（红）
};

class VehicleState : public QObject
{
    Q_OBJECT
public:
    explicit VehicleState(QObject *parent = nullptr);

    // ---- 协议 §五：上位机 200 ms 未收到任何帧 → 判定断开 ----
    static constexpr qint64 kLinkTimeoutMs = 200;
    /// 姿态/运动/任务帧的"过期"判定（周期分别是 50/50/200 ms，给 5 倍余量）
    static constexpr qint64 kPoseStaleMs = 250;
    static constexpr qint64 kMotionStaleMs = 250;
    static constexpr qint64 kTaskStaleMs = 1000;

    // ---- 各类数据（未收到过时保持默认值，用 received(cmd) 判断有效性）----
    const PoseData &pose() const { return m_pose; }
    const MotionData &motion() const { return m_motion; }
    const TaskData &task() const { return m_task; }
    const EnvData &env() const { return m_env; }
    const DiagData &diag() const { return m_diag; }
    const LoopData &loop() const { return m_loop; }
    const PidTuneData &pidEcho() const { return m_pidEcho; } ///< 最近一条 0x24 回读/回执
    const EventData &lastEvent() const { return m_lastEvent; }

    // ---- 来源与新鲜度（"谁给的、多久以前"）----
    bool received(quint8 cmd) const { return m_rec.contains(cmd); }
    qint64 ageMs(quint8 cmd) const;          ///< <0 = 从未收到
    io::LinkKind sourceOf(quint8 cmd) const; ///< Count = 未知
    /// 最近一次收到任意合法帧（含两条链路的合并结果）
    qint64 anyFrameAgeMs() const;
    io::LinkKind anyFrameSource() const { return m_lastAnyKind; }
    /// 链路是否"活着"（协议 §五 的 200 ms 判据，这里是上位机侧自己的判据）
    bool linkAlive() const;

    quint64 frameCount(io::LinkKind kind) const;
    /// 该链路最近一次收到帧距今多久（<0 = 该链路从未收到过帧）
    qint64 linkAgeMs(io::LinkKind kind) const;
    /// 被判定为"同一次测量的重复投递"而合并掉的帧数（两条链路都发同样的帧，见 onFrame 注释）
    quint64 duplicateFrames() const { return m_duplicates; }
    /// 格式错帧数（长度不符）：**必须能被看见** —— 它是"固件与协议不一致"的第一证据
    quint64 malformedFrames() const { return m_malformed; }
    /// 协议外命令数（可能是将来新增的合法命令，不作为错误）
    quint64 unknownFrames() const { return m_unknown; }

    // ---- 电源与环境判定 ----
    bool batteryValid() const { return received(proto::up::kDiag); }
    bool batteryLow() const;
    Severity batterySeverity() const;

    // ---- 安全状态（P1 最小集：报警位 + 安全锁存 + 低压 + 失联）----
    bool alarmActive() const;
    bool safetyLatched() const;
    Severity safetySeverity() const;
    /// 一句话说明（状态条与总览横幅共用同一来源，避免两处口径不一致）
    QString safetySummary() const;
    /// 最近一次安全事件文本（含相对时刻），无则空
    QString lastSafetyEventText() const;

    // ---- 诊断增量（0x15 的 OLED 写计数按秒差分；协议 §3.4）----
    double oledBytesPerSec() const { return m_oledRate; }
    bool taskTickAlive(int index) const; ///< 0=通信 1=气体 2=状态 3=界面

public slots:
    /// 帧入口：接到 `io::LinkManager::frameReady`（跨线程队列连接，值语义安全）
    void onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data);
    /// 清空全部状态（新会话/新连接时调用）
    void reset();

signals:
    /// 任意状态更新（界面**不要**直接连它重绘：UI 以 20 Hz 定时拉取，见计划 §五）
    void changed();
    /// 某个命令字**成功解出一帧**（每收到一帧一次）。
    ///
    /// 为什么要有它：曲线要在"每来一帧"时采一个样（不能按 20 Hz 轮询去重复采样），
    /// 而告警要在"每来一帧"时累计连续确认次数 —— 两者都需要"帧到了"这个事件，
    /// 但**都不需要自己再解析一遍字节**（那会让字段偏移出现第二个实现）。
    void dataUpdated(quint8 cmd);
    /// 事件帧（0x13）：事件流面板与落盘用它（事件给时刻、标志给状态）
    void eventReceived(const domain::EventData &e, io::LinkKind kind);

private:
    struct Record {
        io::LinkKind kind = io::LinkKind::Count;
        qint64 atMs = -1;
    };

    /// 每个命令字最近一次收到的原始数据（用于"两条链路发同一帧"的去重）
    struct LastFrame {
        QByteArray data;
        io::LinkKind kind = io::LinkKind::Count;
        qint64 atMs = -1;
        bool valid = false;
    };

    /// 跨链路去重的时间窗：两条链路是"同一次上报的两次投递"，间隔通常 <10 ms；
    /// 取 150 ms 足够宽（远小于最慢周期帧 1 s），又不会把两次真实测量合并。
    static constexpr qint64 kDuplicateWindowMs = 150;

    qint64 nowMs() const { return m_clock.elapsed(); }
    void handleEvent(const EventData &e, io::LinkKind kind);
    void updateOledRate(const DiagData &d);
    QString faultText() const;

    QElapsedTimer m_clock;

    PoseData m_pose;
    MotionData m_motion;
    TaskData m_task;
    EnvData m_env;
    DiagData m_diag;
    LoopData m_loop;
    PidTuneData m_pidEcho;
    EventData m_lastEvent;

    QHash<quint8, Record> m_rec; ///< 命令字 → 最近一次收到的来源与时刻
    QHash<quint8, LastFrame> m_lastFrame; ///< 命令字 → 最近一次收到的原始数据（去重用）

    quint64 m_linkFrames[static_cast<int>(io::LinkKind::Count)] = {0, 0};
    qint64 m_linkLastMs[static_cast<int>(io::LinkKind::Count)] = {-1, -1};
    qint64 m_lastAnyMs = -1;
    io::LinkKind m_lastAnyKind = io::LinkKind::Count;

    quint64 m_malformed = 0;
    quint64 m_unknown = 0;
    quint64 m_duplicates = 0;

    // 安全：事件给"锁存/解除的时刻"，0x15 的位给"现在的状态"，两者相与
    bool m_eventLatched = false;
    QString m_latchReason;
    QString m_warnReason;
    qint64 m_lastSafetyEventMs = -1;
    QString m_lastSafetyEventText;

    // OLED 写速率（按秒差分 + 平滑）
    quint32 m_lastOledBytes = 0;
    qint64 m_lastOledMs = -1;
    double m_oledRate = 0.0;

    // 任务 tick 存活判定（tick 不涨 = 该任务卡死）
    uint8_t m_lastTicks[4] = {0, 0, 0, 0};
    int m_tickStale[4] = {99, 99, 99, 99};
    qint64 m_lastTickSampleMs = -1;
};

} // namespace domain

Q_DECLARE_METATYPE(domain::EventData)
Q_DECLARE_METATYPE(domain::Severity)
