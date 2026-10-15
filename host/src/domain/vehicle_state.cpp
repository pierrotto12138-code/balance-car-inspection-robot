#include "domain/vehicle_state.h"

#include "domain/units.h"

#include <QStringList>

namespace domain {

namespace {

int linkIndex(io::LinkKind k)
{
    const int i = static_cast<int>(k);
    return (i >= 0 && i < static_cast<int>(io::LinkKind::Count)) ? i : 0;
}

QString secondsAgoText(qint64 ageMs)
{
    if (ageMs < 0) {
        return QStringLiteral("从未");
    }
    return QStringLiteral("%1 s 前").arg(ageMs / 1000.0, 0, 'f', 1);
}

} // namespace

VehicleState::VehicleState(QObject *parent) : QObject(parent)
{
    m_clock.start();
    qRegisterMetaType<domain::EventData>("domain::EventData");
}

// ---------------------------------------------------------------- 来源与新鲜度

qint64 VehicleState::ageMs(quint8 cmd) const
{
    const auto it = m_rec.constFind(cmd);
    return (it == m_rec.constEnd()) ? -1 : (nowMs() - it->atMs);
}

io::LinkKind VehicleState::sourceOf(quint8 cmd) const
{
    const auto it = m_rec.constFind(cmd);
    return (it == m_rec.constEnd()) ? io::LinkKind::Count : it->kind;
}

qint64 VehicleState::anyFrameAgeMs() const
{
    return (m_lastAnyMs < 0) ? -1 : (nowMs() - m_lastAnyMs);
}

bool VehicleState::linkAlive() const
{
    const qint64 age = anyFrameAgeMs();
    return age >= 0 && age <= kLinkTimeoutMs;
}

quint64 VehicleState::frameCount(io::LinkKind kind) const
{
    return m_linkFrames[linkIndex(kind)];
}

qint64 VehicleState::linkAgeMs(io::LinkKind kind) const
{
    const qint64 t = m_linkLastMs[linkIndex(kind)];
    return (t < 0) ? -1 : (nowMs() - t);
}

bool VehicleState::taskTickAlive(int index) const
{
    if (index < 0 || index > 3) {
        return false;
    }
    return m_tickStale[index] < 5; // 连续 5 帧（约 5 s）tick 不涨 = 该任务卡死
}

// ---------------------------------------------------------------- 电源与安全

bool VehicleState::batteryLow() const
{
    // ★ 必须先过"有没有电池"的门槛（决策 60）：USB 供电/分压悬空时 0x15 只报 ≈4.3 V，
    //   固件按"无电池"处理；上位机若直接判 <11.1 V，界面会报红而车只是没接电池。
    return batteryValid() && batteryPresent(m_diag.batteryV) &&
           m_diag.batteryV <= units::kLowBatteryV;
}

bool VehicleState::batteryAbsent() const
{
    return batteryValid() && !batteryPresent(m_diag.batteryV);
}

Severity VehicleState::batterySeverity() const
{
    if (!batteryValid() || batteryAbsent()) {
        return Severity::Unknown;
    }
    if (m_diag.batteryV <= 10.80) {
        return Severity::Alarm;
    }
    return batteryLow() ? Severity::Warn : Severity::Ok;
}

bool VehicleState::alarmActive() const
{
    const bool diagAlarm = received(proto::up::kDiag) && m_diag.alarmActive();
    const bool taskFault = received(proto::up::kTask) && m_task.anyFault();
    return diagAlarm || taskFault;
}

bool VehicleState::safetyLatched() const
{
    // 事件码 0x08/0x0A 置位、0x09 解除；0x15 的 bit1 给"现在的状态"。
    // 两者取或：漏报锁存比误报危险得多（协议 §七 约定 10：安全相关不做平滑/滤波）。
    return m_eventLatched || (received(proto::up::kDiag) && m_diag.safetyLatched());
}

QString VehicleState::faultText() const
{
    QStringList parts;
    if (received(proto::up::kTask) && m_task.anyFault()) {
        parts << faultFlagsText(m_task.faultFlags);
    }
    if (received(proto::up::kDiag) && m_diag.alarmActive() && parts.isEmpty()) {
        parts << QStringLiteral("固件报警位");
    }
    return parts.join(QLatin1Char('|'));
}

Severity VehicleState::safetySeverity() const
{
    if (safetyLatched() || alarmActive()) {
        return Severity::Alarm;
    }
    if (batteryLow()) {
        return batterySeverity();
    }
    if (!received(proto::up::kPose)) {
        return Severity::Unknown;
    }
    if (!linkAlive()) {
        return Severity::Warn; // 有数据但已超时（协议 §五 的 200 ms 判据）
    }
    return Severity::Ok;
}

QString VehicleState::safetySummary() const
{
    if (safetyLatched()) {
        const QString why = m_latchReason.isEmpty() ? QStringLiteral("安全锁存") : m_latchReason;
        return QStringLiteral("⛔ 已安全锁存（%1）—— 车不会动，需重下任务解除").arg(why);
    }
    if (alarmActive()) {
        return QStringLiteral("⚠ 报警中：%1").arg(faultText());
    }
    if (batteryAbsent()) {
        return QStringLiteral("无电池（%1 V < %2 V 门槛 —— 固件按无电池处理，不是低压报警）")
            .arg(units::number(m_diag.batteryV, 2), units::number(kBatteryPresenceV, 2));
    }
    if (batteryLow()) {
        return QStringLiteral("⚠ 电池低压 %1 V（报警线 %2 V）")
            .arg(units::number(m_diag.batteryV, 2), units::number(units::kLowBatteryV, 2));
    }
    if (!received(proto::up::kPose)) {
        return QStringLiteral("等待数据 —— 选好端口后点\"连接\"，或用 --demo 跑假源");
    }
    if (!linkAlive()) {
        return QStringLiteral("链路超时：已有 %1 未收到任何帧（判据 %2 ms）")
            .arg(secondsAgoText(anyFrameAgeMs()))
            .arg(kLinkTimeoutMs);
    }
    if (!m_warnReason.isEmpty()) {
        return QStringLiteral("⚠ %1").arg(m_warnReason);
    }
    return QStringLiteral("正常");
}

QString VehicleState::lastSafetyEventText() const
{
    if (m_lastSafetyEventMs < 0) {
        return QString();
    }
    return QStringLiteral("%1 · %2").arg(secondsAgoText(nowMs() - m_lastSafetyEventMs),
                                         m_lastSafetyEventText);
}

// ---------------------------------------------------------------- 帧入口

void VehicleState::onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data)
{
    const qint64 t = nowMs();
    const int li = linkIndex(kind);
    ++m_linkFrames[li];
    m_linkLastMs[li] = t;
    m_lastAnyMs = t;
    m_lastAnyKind = kind;

    bool ok = false;
    // 0x13 的解码结果要等"去重判定"之后再交给 handleEvent（见函数末尾）
    EventData evData;
    bool haveEvent = false;

    switch (cmd) {
    case proto::up::kPose: {
        PoseData v;
        ok = decodePose(data, v);
        if (ok) {
            m_pose = v;
        }
        break;
    }
    case proto::up::kMotion: {
        MotionData v;
        ok = decodeMotion(data, v);
        if (ok) {
            m_motion = v;
        }
        break;
    }
    case proto::up::kTask: {
        TaskData v;
        ok = decodeTask(data, v);
        if (ok) {
            m_task = v;
        }
        break;
    }
    case proto::up::kEnvGas: {
        EnvData v;
        ok = decodeEnv(data, v);
        if (ok) {
            m_env = v;
        }
        break;
    }
    case proto::up::kDiag: {
        DiagData v;
        ok = decodeDiag(data, v);
        if (ok) {
            updateOledRate(v);
            const uint8_t ticks[4] = {v.tickComm, v.tickGas, v.tickState, v.tickUi};
            for (int i = 0; i < 4; ++i) {
                if (ticks[i] != m_lastTicks[i]) {
                    m_lastTicks[i] = ticks[i];
                    m_tickStale[i] = 0;
                } else if (m_tickStale[i] < 99) {
                    ++m_tickStale[i];
                }
            }
            m_lastTickSampleMs = t;
            m_diag = v;
        }
        break;
    }
    case proto::up::kLoop: {
        LoopData v;
        ok = decodeLoop(data, v);
        if (ok) {
            m_loop = v;
        }
        break;
    }
    case proto::down::kPidTune: {
        // 0x24 是我们自己下发的命令，但机器人会把**实际生效值**回显上来（协议 §4.2），
        // 所以它同时是一条合法上行帧 —— 回读确认就走这条路。
        PidTuneData v;
        ok = decodePidTune(data, v);
        if (ok) {
            m_pidEcho = v;
        }
        break;
    }
    case proto::up::kEvent: {
        ok = decodeEvent(data, evData);
        haveEvent = ok;
        break;
    }
    case proto::up::kHeartbeat:
        ok = data.isEmpty(); // N=0
        break;
    default:
        // 协议外命令：不算错误（可能是将来新增的合法命令），但**要计数**，
        // 否则"固件发了新帧、上位机没实现"会表现为"界面莫名少一块数据"。
        ++m_unknown;
        emit changed();
        return;
    }

    if (!ok) {
        // 长度不符：保留旧值 + 计数。**绝不**按偏移硬读 —— 错位读出来的数看着也"像真的"。
        ++m_malformed;
        emit changed();
        return;
    }

    // ---- 跨链路去重（★ 必须在聚合层做一次）----
    // 协议 §七 约定 6：**两条链路都会发同样的周期帧与事件**。同一次物理测量若被当成两次，
    // 下游会集体出错，而且现象都很隐蔽：
    //   · 告警的"连续确认 3 帧"会被 1.5 次真实采样满足（提前报警）；
    //   · 曲线同一时刻采两个样（看上去只是"线更粗"）；
    //   · 事件流每条事件出现两遍（复盘时以为发生了两次急停）。
    // 判据：同命令字 + 内容完全相同 + **来自不同链路** + 间隔 ≤150 ms。
    // 注意"不同链路"这一条：同一条链路连发两帧相同内容（20 Hz 下很常见）**不是**重复。
    bool duplicate = false;
    {
        LastFrame &lf = m_lastFrame[cmd];
        if (lf.valid && lf.kind != kind && lf.data == data && (t - lf.atMs) <= kDuplicateWindowMs) {
            duplicate = true;
            ++m_duplicates;
        }
        lf.data = data;
        lf.kind = kind;
        lf.atMs = t;
        lf.valid = true;
    }

    m_rec.insert(cmd, Record{kind, t});
    if (!duplicate) {
        // dataUpdated 是"**这是一次新的测量**"的信号：曲线采样与告警连续确认都接它
        emit dataUpdated(cmd);
        if (haveEvent) {
            handleEvent(evData, kind);
        }
    }
    emit changed();
}

void VehicleState::handleEvent(const EventData &e, io::LinkKind kind)
{
    m_lastEvent = e;
    switch (e.code) {
    case proto::EventCode::LinkLostStop:
        m_eventLatched = true;
        m_latchReason = QStringLiteral("通信失联");
        break;
    case proto::EventCode::EstopReceived:
        m_eventLatched = true;
        m_latchReason = QStringLiteral("收到急停指令");
        break;
    case proto::EventCode::SafetyReleased:
        m_eventLatched = false;
        m_latchReason.clear();
        break;
    case proto::EventCode::CrashOrLowBat:
        m_warnReason = QStringLiteral("碰撞 / 跌落 / 低电压告警");
        break;
    case proto::EventCode::GasOverLimit:
        m_warnReason = QStringLiteral("气体浓度超阈值");
        break;
    case proto::EventCode::WarmupOrFail:
        m_warnReason.clear();
        break;
    default:
        break;
    }

    // 安全通道事件（0x08/0x09/0x0A）单独记一份文本：事件流面板与状态条都要"时刻"
    switch (e.code) {
    case proto::EventCode::LinkLostStop:
    case proto::EventCode::EstopReceived:
    case proto::EventCode::SafetyReleased:
        m_lastSafetyEventMs = nowMs();
        m_lastSafetyEventText = QString::fromUtf8(eventName(e.code));
        break;
    default:
        break;
    }

    emit eventReceived(e, kind);
    emit changed();
}

void VehicleState::updateOledRate(const DiagData &d)
{
    const qint64 t = nowMs();
    if (m_lastOledMs >= 0) {
        const qint64 dt = t - m_lastOledMs;
        if (dt > 0) {
            const quint32 delta = d.oledBytes - m_lastOledBytes; // 无符号回绕自然成立
            const double rate = static_cast<double>(delta) * 1000.0 / static_cast<double>(dt);
            m_oledRate = (m_oledRate <= 0.0) ? rate : (0.7 * m_oledRate + 0.3 * rate);
        }
    }
    m_lastOledBytes = d.oledBytes;
    m_lastOledMs = t;
}

void VehicleState::reset()
{
    m_pose = PoseData{};
    m_motion = MotionData{};
    m_task = TaskData{};
    m_env = EnvData{};
    m_diag = DiagData{};
    m_loop = LoopData{};
    m_pidEcho = PidTuneData{};
    m_lastEvent = EventData{};
    m_rec.clear();
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        m_linkFrames[i] = 0;
        m_linkLastMs[i] = -1;
    }
    m_lastAnyMs = -1;
    m_lastAnyKind = io::LinkKind::Count;
    m_malformed = 0;
    m_unknown = 0;
    m_duplicates = 0;
    m_lastFrame.clear();
    m_eventLatched = false;
    m_latchReason.clear();
    m_warnReason.clear();
    m_lastSafetyEventMs = -1;
    m_lastSafetyEventText.clear();
    m_lastOledBytes = 0;
    m_lastOledMs = -1;
    m_oledRate = 0.0;
    for (int i = 0; i < 4; ++i) {
        m_lastTicks[i] = 0;
        m_tickStale[i] = 99;
    }
    m_clock.restart();
    emit changed();
}

} // namespace domain
