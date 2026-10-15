#pragma once

// ============================================================================
//  AlarmEngine —— 告警判定与迟滞（domain 层，纯逻辑可单测）
//
//  它和 `VehicleState` 的分工：
//    · `VehicleState` 只回答"**现在是什么**"（原始值 + 来源 + 安全锁存/报警位）；
//    · `AlarmEngine` 回答"**该不该报警、什么时候报的、什么时候解除的**"。
//  为什么必须分开：告警是**有状态、有时间**的判断（连续确认、迟滞、只报一次），
//  把它塞进"读一眼当前值"的地方，就会出现"每帧都报一次"或"抖一下就报"。
//
//  三条规则（每一条都对应真机上会遇到的边界）：
//    ① **连续确认**：超限要连续 N 帧（默认 3，同 `0x23` 的"连续确认帧数"）才算数 ——
//       单帧毛刺（ADC 抖动、气体传感器噪声）不该报警；
//    ② **迟滞**：进入用阈值本身，退出要回到阈值的 90%（低压 11.10 → 11.40 V）——
//       否则数值贴着阈值时会在"报警/解除"之间来回跳，事件流被刷屏；
//    ③ **状态跃变才报**：同一段异常只报一次（on）；回到正常且确认后才报解除（off）；
//       解除之后**再次**超限会**重新**报 —— 这一条很关键：如果"再次进入异常不再提示"，
//       安全功能就成了装饰（AGENTS 铁律 29 的同一类错误）。
//
//  本文件只依赖 Qt6::Core（不依赖界面、不解析字节），因此可脱离硬件与界面单测。
// ============================================================================

#include "domain/vehicle_state.h"

#include "domain/units.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace domain {

/// 告警阈值与判定参数（默认值来自协议 §4.1 的 `0x23` 默认：MQ-2 30 ppm / MQ-135 50 ppm）
struct AlarmThresholds {
    double mq2Ppm = 30.0;   ///< MQ-2 报警阈值（ppm）
    double mq135Ppm = 50.0; ///< MQ-135 报警阈值（ppm）
    double lowBatteryV = units::kLowBatteryV;          ///< 低压报警线 11.10 V
    double lowBatteryClearV = units::kLowBatteryV + 0.3; ///< 低压解除线 11.40 V（迟滞）
    int confirmFrames = 3; ///< 连续确认帧数（超限）
    int clearFrames = 3;   ///< 连续正常帧数（解除）
    qint64 linkLostMs = 500; ///< 失联判据（与固件安全停车一致，协议 §五）
    double gasClearRatio = 0.9; ///< 解除时的回差比例（阈值 × 0.9）
};

/// 一条告警的完整生命周期（事件流留档用）
struct AlarmRecord {
    QString key;         ///< 唯一键：gas.mq2 / gas.mq135 / battery.low / link.lost / safety.latched
    QString title;       ///< 中文名
    Severity severity = Severity::Warn;
    QString unit;
    double threshold = 0.0;
    double value = 0.0;     ///< 最近一次触发时的值
    double peak = 0.0;      ///< 活动期间的最大值（事件流显示"最高到 x"）
    int overCount = 0;
    int underCount = 0;
    bool active = false;
    qint64 onsetMs = -1;    ///< 进入告警的本地时刻（相对引擎创建）
    qint64 clearMs = -1;    ///< 解除时刻
};

class AlarmEngine : public QObject
{
    Q_OBJECT
public:
    explicit AlarmEngine(QObject *parent = nullptr);

    AlarmThresholds thresholds() const { return m_th; }
    /// 改阈值/参数（界面上的阈值输入、P4 的设置项都走这里）
    void setThresholds(const AlarmThresholds &t);

    /// 每成功解出一帧调用一次（接 `VehicleState::dataUpdated`）——阈值类规则在这里判
    void onFrameDecoded(const VehicleState &st, quint8 cmd);
    /// 固定节拍（建议 10 Hz）调用 —— **随时间变化**的规则（失联、锁存）在这里判
    void tick(const VehicleState &st);
    /// 清空全部告警状态（新会话/新连接）
    void reset();

    bool anyActive() const;
    int activeCount() const;
    /// 活动告警的一句话摘要（界面横幅/状态条用；无活动告警返回空串）
    QString activeSummary() const;
    /// 活动告警里最严重的级别（无则 Ok）
    Severity severity() const;
    /// 活动的告警列表（按严重程度、再按触发时刻排序）
    QList<AlarmRecord> activeAlarms() const;
    /// 全部告警记录（含已解除）—— 测试与"告警一览"用
    QList<AlarmRecord> history() const;
    /// 该键最近一次的记录（不存在返回默认构造）
    AlarmRecord record(const QString &key) const;

    // ---- 规则键（界面与事件流按同一套键引用，避免各处硬编码字符串）----
    static const QString kGasMq2;
    static const QString kGasMq135;
    static const QString kBatteryLow;
    static const QString kLinkLost;
    static const QString kSafetyLatched;

signals:
    void alarmRaised(const domain::AlarmRecord &rec);
    void alarmCleared(const domain::AlarmRecord &rec);
    void thresholdsChanged(const domain::AlarmThresholds &th);

private:
    /// 取（或创建）某个规则的记录
    AlarmRecord &slot(const QString &key, const QString &title, Severity sev, const QString &unit,
                      double threshold);
    /// 阈值类规则一步到位：查记录 → 判定（超限连续确认→报；回到回差以下连续确认→解除）
    ///
    /// ★ 两个方向都要支持：气体是"高了才坏"，**电压是"低了才坏"** ——
    ///   把方向写死在"高=坏"里，低压保护就会变成"电压越高越报警"（反向），
    ///   而现象上只是"低压从来不报"，很难往这里想。
    ///
    /// ★ 为什么不让调用方先拿引用再判：`slot()` 在插入新键时可能触发 QHash 重哈希，
    ///   先前拿到的 `AlarmRecord&` 会失效（悬垂引用）—— 把"查+判"封在一个函数里，
    ///   这个坑就不存在了。
    void judgeThresholdFor(const QString &key, const QString &title, Severity sev,
                           const QString &unit, double badAt, double clearAt, double value,
                           bool lowIsBad = false);
    /// 内部的判定核心（只在 judgeThresholdFor 里用，引用不会跨调用存活）
    void judgeThreshold(AlarmRecord &r, double value, double badAt, double clearAt, bool lowIsBad);
    /// 状态类规则：直接按布尔量报/解除（锁存、失联）
    void judgeState(AlarmRecord &r, bool active, double value);
    /// 把一条记录从"不活动"翻到"活动"（或反向），并发出信号
    void setActive(AlarmRecord &r, bool active);

    AlarmThresholds m_th;
    QHash<QString, AlarmRecord> m_records;
    QElapsedTimer m_clock;
};

} // namespace domain

Q_DECLARE_METATYPE(domain::AlarmRecord)
Q_DECLARE_METATYPE(domain::AlarmThresholds)
