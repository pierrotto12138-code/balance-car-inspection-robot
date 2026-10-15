#pragma once

// ============================================================================
//  ParamStore —— PID 参数与巡检参数的"账本"（data 层，P4）
//
//  它管三件事，每一件都对应一次真机上真实浪费过的时间：
//    ① **参数定义**（ID / 名称 / 基底变量 / 默认值 / 允许范围 / 说明）——
//       范围的唯一依据是协议 §3.8（越界固件会拒绝并保持原值）；
//    ② **下发前就校验**：越界/NaN 在**上位机侧**就拦下来并说明原因。
//       若不拦，固件会"拒绝并回旧值"，现象是"改了没生效" —— 人会去查链路、查固件，
//       而真正的原因只是"填错了一个数"（协议 §3.8 已经写明范围）；
//    ③ **下发值 ↔ 回读值配对**：机器人收到 `0x24` 后会把**实际生效值**回显（协议 §4.2），
//       两者不一致就是"以为改了、其实没收到"的第一现场，必须当成一条**可见的失败**报出来，
//       而不是悄悄把界面上的值改成回读值（那会把证据抹掉）。
//
//  另外记录**快照历史**（谁在什么时候把哪个参数改成了多少），P4 的 SQLite 落盘也吃这个。
// ============================================================================

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

namespace data {

/// 一个 PID 参数的定义（范围依据：协议 §3.8）
struct PidParamInfo {
    quint8 id = 0;
    const char *name = "";     ///< 中文名
    const char *baseVar = "";  ///< 固件里的基底变量名（对 B 侧排障最有用的一列）
    double defaultValue = 0.0; ///< 协议 §4.2 的默认值
    double lo = 0.0;           ///< 允许下限
    double hi = 0.0;           ///< 允许上限
    const char *desc = "";     ///< 说明（为什么是这个范围/什么时候调）
};

/// 7 个真实增益参数（`0x00/0xFD/0xFE/0xFF` 是扩展 ID，不是增益，见协议 §4.2）
const QVector<PidParamInfo> &pidParams();
/// 按 ID 取定义；未知 ID 返回 nullptr
const PidParamInfo *pidParamInfo(quint8 id);
QString pidParamName(quint8 id);

/// 校验结果
enum class ParamCheck {
    Ok = 0,
    UnknownParam, ///< 不认识的 ID
    OutOfRange,   ///< 超出协议允许范围（固件会拒绝）
    NotANumber,   ///< NaN/Inf
};

/// 下发前校验（界面与工装都该用它，不要各自写一套判据）
ParamCheck checkPidValue(quint8 id, double value, QString *why = nullptr);

/// 参数快照（一条记录 = 一次下发或一次回读）
struct ParamSnapshot {
    qint64 atMs = 0;      ///< 本地时刻（相对账本创建）
    quint8 id = 0;
    double value = 0.0;
    QString source;       ///< "下发" / "回读" / "回读(全量)" / "回执"
    bool ok = true;       ///< 配对结果：false = 与最近一次下发不一致（仅回读记录有意义）
};

class ParamStore : public QObject
{
    Q_OBJECT
public:
    explicit ParamStore(QObject *parent = nullptr);

    /// 记录"我们要下发的值"（在真正发送**之前**调用：先记账，再看回读）
    void noteRequest(quint8 id, double value);
    /// 记录机器人回读/回执（来自 `0x24` 回显）
    void noteReadback(quint8 id, double value);
    /// 扩展 ID 的回执（`0x00` 保存成功 / `0xFE` 载入成功 / `0xFD` 观测模式）
    void noteAck(quint8 id, double value);

    bool has(quint8 id) const { return m_current.contains(id); }
    /// 当前值（未收到过回读时返回定义的默认值）
    double current(quint8 id) const;
    /// "是否已从机器人回读过"（与"有默认值"区分开：界面要能说清数据来源）
    bool readbackReceived(quint8 id) const;

    /// 最近一次下发是否已被回读确认（且一致）。没有下发过返回 true（不构成不一致）
    bool lastRequestConfirmed(quint8 id) const;
    /// 最近一次下发值（没下发过返回 NaN）
    double lastRequestValue(quint8 id) const;

    QVector<ParamSnapshot> history() const { return m_history; }
    /// 清空（新会话）
    void clear();

    /// 巡检参数（`0x23`）的当前值：阈值/采样周期/开关 —— 与 PID 分开存，
    /// 因为它们下行后**不会**被回显（协议只回 0x12 确认），所以这里只是"我们发过什么"。
    struct SurveyCfg {
        quint16 samplePeriodMs = 1000;
        double mq2Ppm = 30.0;
        double mq135Ppm = 50.0;
        bool adaptive = true;
        bool compensation = false;
        quint8 confirmFrames = 3;
    };
    SurveyCfg surveyCfg() const { return m_survey; }
    void setSurveyCfg(const SurveyCfg &cfg);

signals:
    /// 参数值更新（回读；界面据此刷新表格）
    void valueUpdated(quint8 id, double value, bool fromRobot);
    /// 下发与回读**不一致**（"以为改了、其实没收到"）
    void mismatch(quint8 id, double requested, double actual);
    void historyAppended(const data::ParamSnapshot &snap);
    void surveyCfgChanged(const data::ParamStore::SurveyCfg &cfg);

private:
    void append(quint8 id, double value, const QString &source, bool ok);
    qint64 nowMs() const { return m_clock.elapsed(); }

    QHash<quint8, double> m_current;   ///< 最近一次回读值
    QHash<quint8, double> m_requested; ///< 最近一次下发值
    QHash<quint8, bool> m_confirmed;   ///< 最近一次下发是否已被"一致的回读"确认
    QVector<ParamSnapshot> m_history;
    SurveyCfg m_survey;
    QElapsedTimer m_clock;
};

} // namespace data
