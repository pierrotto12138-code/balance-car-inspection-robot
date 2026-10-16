#include "domain/frames.h"

#include "codec.h" // 只用它的字节序辅助（getU16/getI16/getU32），不组帧/解帧
#include "domain/units.h"

#include <QStringList>

#include <cstring>

namespace domain {

namespace {

/// 把 QByteArray 当只读字节指针用（协议统一小端，见 codec.h 的 getU16/getI16/getU32）
inline const uint8_t *bytes(const QByteArray &d)
{
    return reinterpret_cast<const uint8_t *>(d.constData());
}

} // namespace

bool decodePose(const QByteArray &d, PoseData &out)
{
    if (d.size() != 12) {
        return false;
    }
    const uint8_t *p = bytes(d);
    PoseData v;
    v.pitchDeg = units::deg(proto::getI16(p + 0));
    v.rollDeg = units::deg(proto::getI16(p + 2));
    v.yawDeg = units::deg(proto::getI16(p + 4));
    v.gyroXDps = units::dps(proto::getI16(p + 6));
    v.gyroYDps = units::dps(proto::getI16(p + 8));
    v.gyroZDps = units::dps(proto::getI16(p + 10));
    out = v;
    return true;
}

bool decodeMotion(const QByteArray &d, MotionData &out)
{
    if (d.size() != 9) { // ★ 是 9 不是 11（协议 v1.1 已修正）
        return false;
    }
    const uint8_t *p = bytes(d);
    MotionData v;
    v.leftMmS = proto::getI16(p + 0);
    v.rightMmS = proto::getI16(p + 2);
    v.motorOutAvg = proto::getI16(p + 4); // 带符号原始值（±8200）——不是占空比千分比，见 frames.h 说明
    v.distanceMm = proto::getU16(p + 6);
    v.avoidFlags = p[8];
    out = v;
    return true;
}

bool decodeTask(const QByteArray &d, TaskData &out)
{
    if (d.size() != 5) {
        return false;
    }
    const uint8_t *p = bytes(d);
    TaskData v;
    v.taskId = static_cast<proto::TaskId>(p[0]);
    v.phase = static_cast<proto::Phase>(p[1]);
    v.progressPct = p[2];
    v.checkpoint = p[3];
    v.faultFlags = p[4];
    out = v;
    return true;
}

bool decodeEvent(const QByteArray &d, EventData &out)
{
    if (d.size() != 5) {
        return false;
    }
    const uint8_t *p = bytes(d);
    EventData v;
    v.code = static_cast<proto::EventCode>(p[0]);
    v.timestampMs = proto::getU32(p + 1);
    out = v;
    return true;
}

bool decodeEnv(const QByteArray &d, EnvData &out)
{
    if (d.size() != 16) {
        return false;
    }
    const uint8_t *p = bytes(d);
    EnvData v;
    v.mq2Raw = proto::getU16(p + 0);
    v.mq135Raw = proto::getU16(p + 2);
    v.mq2Ppm = units::ppm(proto::getU16(p + 4));
    v.mq135Ppm = units::ppm(proto::getU16(p + 6));
    v.tempC = units::celsius(proto::getI16(p + 8));
    v.humidityPct = units::rh(proto::getU16(p + 10));
    v.readyFlags = p[12];
    v.gear = static_cast<proto::SampleGear>(p[13]);
    // 偏移 14/15 为保留字节（固件写 0），这里**不校验**其内容：保留位的语义将来可能启用，
    // 现在对它们做断言等于把"未来兼容"堵死。
    out = v;
    return true;
}

bool decodeDiag(const QByteArray &d, DiagData &out)
{
    if (d.size() != 16) {
        return false;
    }
    const uint8_t *p = bytes(d);
    DiagData v;
    v.batteryV = units::volt(proto::getU16(p + 0));
    v.page = p[2];
    v.flagShow = (p[3] != 0);
    v.attitudeAlgo = p[4];
    v.stateBits = p[5];
    v.heapKb = proto::getU16(p + 6);
    v.oledBytes = proto::getU32(p + 8);
    v.tickComm = p[12];
    v.tickGas = p[13];
    v.tickState = p[14];
    v.tickUi = p[15];
    out = v;
    return true;
}

bool decodeLoop(const QByteArray &d, LoopData &out)
{
    if (d.size() != 20) {
        return false;
    }
    const uint8_t *p = bytes(d);
    LoopData v;
    v.balancePwm = proto::getI16(p + 0);
    v.velocityPwm = proto::getI16(p + 2);
    v.turnPwm = proto::getI16(p + 4);
    v.moto1 = proto::getI16(p + 6);
    v.moto2 = proto::getI16(p + 8);
    v.ccrA = proto::getU16(p + 10);
    v.ccrB = proto::getU16(p + 12);
    v.encLeft = proto::getI16(p + 14);
    v.encRight = proto::getI16(p + 16);
    v.stateBits = p[18];
    v.fwVersion = p[19]; // 协议 §3.7：保留字节用作固件版本 (月<<4)|日
    out = v;
    return true;
}

bool decodePidTune(const QByteArray &d, PidTuneData &out)
{
    if (d.size() != 6) {
        return false;
    }
    const uint8_t *p = bytes(d);
    PidTuneData v;
    v.paramId = p[0];
    v.reserved = p[1];
    const quint32 bits = proto::getU32(p + 2);
    static_assert(sizeof(float) == 4, "协议假定 float32");
    std::memcpy(&v.value, &bits, sizeof(float));
    out = v;
    return true;
}

int expectedUpLen(quint8 cmd)
{
    switch (cmd) {
    case proto::up::kPose:
        return 12;
    case proto::up::kMotion:
        return 9;
    case proto::up::kTask:
        return 5;
    case proto::up::kEvent:
        return 5;
    case proto::up::kEnvGas:
        return 16;
    case proto::up::kDiag:
        return 16;
    case proto::up::kLoop:
        return 20;
    case proto::up::kHeartbeat:
        return 0;
    case proto::down::kPidTune: // 0x24 的回显（下行命令也会上行回来，见 PidTuneData 注释）
        return 6;
    default:
        return -1;
    }
}

const char *phaseName(proto::Phase p)
{
    switch (p) {
    case proto::Phase::Init:
        return "初始化/自检";
    case proto::Phase::Warmup:
        return "预热中";
    case proto::Phase::Idle:
        return "待机";
    case proto::Phase::Patrol:
        return "自主巡航";
    case proto::Phase::Analysis:
        return "异常研判";
    case proto::Phase::Return:
        return "返航";
    case proto::Phase::Done:
        return "完成";
    case proto::Phase::Fault:
        return "故障/安全停车";
    }
    return "未知";
}

const char *taskName(proto::TaskId t)
{
    switch (t) {
    case proto::TaskId::Idle:
        return "待机";
    case proto::TaskId::Patrol:
        return "自主巡航";
    case proto::TaskId::Return:
        return "返航";
    }
    return "未知";
}

const char *eventName(proto::EventCode c)
{
    switch (c) {
    case proto::EventCode::SelfCheckDone:
        return "上电自检完成";
    case proto::EventCode::TaskStart:
        return "任务开始";
    case proto::EventCode::TaskDone:
        return "任务完成";
    case proto::EventCode::TaskFail:
        return "任务失败/超时";
    case proto::EventCode::CrashOrLowBat:
        return "碰撞/跌落/低电压告警";
    case proto::EventCode::GasOverLimit:
        return "气体浓度超阈值";
    case proto::EventCode::WarmupOrFail:
        return "预热完成/传感器失效";
    case proto::EventCode::LinkLostStop:
        return "通信失联 → 安全停车";
    case proto::EventCode::SafetyReleased:
        return "安全停车解除";
    case proto::EventCode::EstopReceived:
        return "收到急停指令 → 安全停车并锁存";
    }
    return "未知事件";
}

const char *sampleGearName(proto::SampleGear g)
{
    switch (g) {
    case proto::SampleGear::Warmup:
        return "预热中";
    case proto::SampleGear::Normal:
        return "常态 1 Hz";
    case proto::SampleGear::Fast:
        return "异常 5 Hz";
    }
    return "未知档位";
}

const char *attitudeAlgoName(uint8_t algo)
{
    switch (algo) {
    case 1:
        return "四元数";
    case 2:
        return "卡尔曼";
    case 3:
        return "互补滤波";
    default:
        return "未知";
    }
}

const char *diagPageName(uint8_t page)
{
    switch (page) {
    case 0:
        return "厂商页";
    case 1:
        return "环境气体页";
    case 2:
        return "巡检通信页";
    default:
        return "未知页";
    }
}

QString faultFlagsText(uint8_t flags)
{
    if (flags == 0) {
        return QStringLiteral("无");
    }
    QStringList parts;
    if (flags & 0x01) {
        parts << QStringLiteral("气体");
    }
    if (flags & 0x02) {
        parts << QStringLiteral("失联");
    }
    if (flags & 0x04) {
        parts << QStringLiteral("硬件");
    }
    if (flags & 0x08) {
        parts << QStringLiteral("传感器");
    }
    if (flags & 0xF0) { // 未定义的位也要显示出来（协议 §七 约定 10：安全相关不"平滑"）
        parts << QStringLiteral("其他(0x%1)").arg(flags & 0xF0, 2, 16, QLatin1Char('0'));
    }
    return parts.join(QLatin1Char('|'));
}

QString hexByte(quint8 v)
{
    return QStringLiteral("0x%1").arg(v, 2, 16, QLatin1Char('0')).toUpper().replace(
        QStringLiteral("0X"), QStringLiteral("0x"));
}

QString firmwareVersionText(uint8_t v)
{
    if (v == 0) {
        return QStringLiteral("未上报");
    }
    const int month = (v >> 4) & 0x0F;
    const int day = v & 0x0F;
    return QStringLiteral("%1-%2").arg(month, 2, 10, QLatin1Char('0')).arg(day, 2, 10, QLatin1Char('0'));
}

} // namespace domain
