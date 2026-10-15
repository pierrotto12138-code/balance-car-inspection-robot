#include "domain/commands.h"

#include "codec.h"

#include <cmath>
#include <cstring>

namespace domain {
namespace cmd {

namespace {

void putU8(std::vector<uint8_t> &v, quint8 b)
{
    v.push_back(b);
}

QByteArray toQ(const std::vector<uint8_t> &v)
{
    return QByteArray(reinterpret_cast<const char *>(v.data()), static_cast<int>(v.size()));
}

} // namespace

QByteArray motionPayload(int linMmS, int angDpsX10, bool enable)
{
    std::vector<uint8_t> d;
    // 钳位到 int16：把"超出范围的意图"截断在这里，而不是让它回绕成反向指令
    // （回绕会让"全速前进"变成"后退"，属于最危险的一类 bug）。
    const int lin = qBound(-32768, linMmS, 32767);
    const int ang = qBound(-32768, angDpsX10, 32767);
    proto::putI16(d, static_cast<int16_t>(lin));
    proto::putI16(d, static_cast<int16_t>(ang));
    putU8(d, enable ? 1 : 0);
    return toQ(d);
}

QByteArray stopPayload()
{
    return motionPayload(0, 0, false);
}

QByteArray taskPayload(proto::TaskId id, const QList<quint16> &params)
{
    std::vector<uint8_t> d;
    putU8(d, static_cast<quint8>(id));
    // 参数个数用 quint8：协议格式是"参数个数 uint8 + 参数 uint16×k"，
    // 超过 255 个参数在协议里就表达不了，宁可在构造处截断（并让调用方自己保证）。
    const int n = qMin(params.size(), 255);
    putU8(d, static_cast<quint8>(n));
    for (int i = 0; i < n; ++i) {
        proto::putU16(d, params.at(i));
    }
    return toQ(d);
}

QByteArray modePayload(quint8 mode)
{
    std::vector<uint8_t> d;
    putU8(d, mode);
    return toQ(d);
}

QByteArray surveyCfgPayload(quint16 samplePeriodMs, double mq2Ppm, double mq135Ppm, bool adaptive,
                            bool compensation, quint8 confirmFrames)
{
    std::vector<uint8_t> d;
    proto::putU16(d, samplePeriodMs);
    // 阈值单位是 ppm×10（与 0x14 的浓度字段同口径）；四舍五入后再钳到 uint16，
    // 避免"界面填 7000 ppm 却因为回绕变成 4 ppm"这种荒唐的下载值。
    auto toPpm10 = [](double ppm) {
        const double v = std::round(ppm * 10.0);
        return static_cast<quint16>(qBound(0.0, v, 65535.0));
    };
    proto::putU16(d, toPpm10(mq2Ppm));
    proto::putU16(d, toPpm10(mq135Ppm));
    putU8(d, adaptive ? 1 : 0);
    putU8(d, compensation ? 1 : 0);
    putU8(d, confirmFrames);
    putU8(d, 0); // 偏移 9
    putU8(d, 0); // 偏移 10
    putU8(d, 0); // 偏移 11（9~11 保留，置 0）
    return toQ(d);
}

bool pidTunePayload(quint8 paramId, float value, QByteArray *out)
{
    if (out == nullptr) {
        return false;
    }
    if (std::isnan(value) || std::isinf(value)) {
        return false; // 非法值不发（见头文件说明）
    }
    std::vector<uint8_t> d;
    putU8(d, paramId);
    putU8(d, 0); // 保留
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits)); // IEEE-754 位型
    proto::putU32(d, bits);                   // 显式小端（不依赖主机字节序）
    *out = toQ(d);
    return true;
}

QByteArray estopPayload()
{
    return QByteArray(); // N=0
}

QByteArray heartbeatPayload()
{
    return QByteArray(); // N=0
}

const char *name(quint8 cmd)
{
    switch (cmd) {
    case proto::down::kMotion:
        return "运动控制 0x20";
    case proto::down::kTask:
        return "任务指示 0x21";
    case proto::down::kMode:
        return "控制模式 0x22";
    case proto::down::kSurveyCfg:
        return "巡检参数 0x23";
    case proto::down::kPidTune:
        return "PID 调参 0x24";
    case proto::down::kEstop:
        return "紧急停机 0x2F";
    case proto::up::kHeartbeat:
        return "心跳 0x1F（保活）";
    default:
        return "未知命令";
    }
}

SafetyEffect safetyEffect(quint8 cmd)
{
    switch (cmd) {
    case proto::down::kEstop:
        return SafetyEffect::Latches;
    case proto::down::kTask:
    case proto::down::kMotion:
        // ★ 这两个**可能解除**安全锁存（协议 v1.1：收到显式使能 0x20 enable=1
        //   或 0x21 任务=自主巡航 即解除）。界面必须把这件事说清楚 ——
        //   否则"我随手点了一下方向键，锁存就没了"正是铁律 29 里那类旁路。
        return SafetyEffect::Releases;
    default:
        return SafetyEffect::None;
    }
}

} // namespace cmd
} // namespace domain
