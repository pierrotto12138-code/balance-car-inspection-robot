#pragma once

// ============================================================================
//  测试用"按协议造帧"的公共工具（test_domain / test_ui 共用）
//
//  为什么要共用一份：
//    协议一改长度，若每个测试各造一份帧，就会变成"改一处、漏三处"，
//    而漏掉的那几处会以"测试仍然通过"的形式**静默失效**（它压根没造出合法帧）。
//    所以：**造帧只允许出现在这里**（另加 test_domain 里那几条直接抄自
//    docs/protocol/test_vectors.json 的冻结向量）。
//
//  每个函数返回的都是**完整数据段**（不含帧头/CRC），长度严格等于协议 §三 的规定。
// ============================================================================

#include "codec.h"
#include "domain/frames.h"

#include <QByteArray>

namespace testframes {

inline QByteArray toQ(const std::vector<uint8_t> &v)
{
    return QByteArray(reinterpret_cast<const char *>(v.data()), static_cast<int>(v.size()));
}

/// 0x10 姿态（N=12）：角度/角速度单位 0.01°、0.01 dps
inline QByteArray pose(qint16 pitch = 123, qint16 roll = -456, qint16 yaw = 7890,
                       qint16 gx = 12, qint16 gy = -34, qint16 gz = 56)
{
    std::vector<uint8_t> d;
    proto::putI16(d, pitch);
    proto::putI16(d, roll);
    proto::putI16(d, yaw);
    proto::putI16(d, gx);
    proto::putI16(d, gy);
    proto::putI16(d, gz);
    return toQ(d);
}

/// 0x11 运动（N=9）
inline QByteArray motion(qint16 left = 250, qint16 right = -250, qint16 pwm = 1200,
                         quint16 dist = 85, quint8 avoid = 0x03)
{
    std::vector<uint8_t> d;
    proto::putI16(d, left);
    proto::putI16(d, right);
    proto::putI16(d, pwm);
    proto::putU16(d, dist);
    d.push_back(avoid);
    return toQ(d);
}

/// 0x12 任务（N=5）
inline QByteArray task(quint8 phase = 0x03, quint8 progress = 45, quint8 checkpoint = 2,
                       quint8 fault = 0x00, quint8 taskId = 0x01)
{
    std::vector<uint8_t> d;
    d.push_back(taskId);
    d.push_back(phase);
    d.push_back(progress);
    d.push_back(checkpoint);
    d.push_back(fault);
    return toQ(d);
}

/// 0x13 事件（N=5）
inline QByteArray event(proto::EventCode code, quint32 tsMs = 123456)
{
    std::vector<uint8_t> d;
    d.push_back(static_cast<uint8_t>(code));
    proto::putU32(d, tsMs);
    return toQ(d);
}

/// 0x14 环境与气体（N=16）：数值取自冻结向量（MQ2 1234 / MQ135 2345 / 567,890 ppm×10 …）
inline QByteArray env(quint16 mq2Raw = 1234, quint16 mq135Raw = 2345, quint16 mq2Ppm10 = 567,
                      quint16 mq135Ppm10 = 890, qint16 temp10 = 256, quint16 rh10 = 610,
                      quint8 ready = 0x0F, quint8 gear = 0x01)
{
    std::vector<uint8_t> d;
    proto::putU16(d, mq2Raw);
    proto::putU16(d, mq135Raw);
    proto::putU16(d, mq2Ppm10);
    proto::putU16(d, mq135Ppm10);
    proto::putI16(d, temp10);
    proto::putU16(d, rh10);
    d.push_back(ready);
    d.push_back(gear);
    d.push_back(0x00); // 保留
    d.push_back(0x00); // 保留
    return toQ(d);
}

/// 0x15 诊断（N=16）：batteryRaw 单位 0.01 V；stateBits 见协议 §3.4
inline QByteArray diag(quint16 batteryRaw = 1180, quint8 stateBits = 0x04, quint16 heapKb = 18,
                       quint32 oledBytes = 1000, quint8 page = 0x02)
{
    std::vector<uint8_t> d;
    proto::putU16(d, batteryRaw);
    d.push_back(page);
    d.push_back(0x00); // Flag_Show
    d.push_back(0x01); // 姿态算法 = 四元数
    d.push_back(stateBits);
    proto::putU16(d, heapKb);
    proto::putU32(d, oledBytes);
    d.push_back(1);
    d.push_back(2);
    d.push_back(3);
    d.push_back(4);
    return toQ(d);
}

/// 0x16 三环实况（N=20）；偏移 19 = 固件版本字节（月<<4|日）
inline QByteArray loop(qint16 bal = 100, qint16 vel = -50, qint16 turn = 25, qint16 moto1 = 75,
                       qint16 moto2 = 125, quint16 ccrA = 75, quint16 ccrB = 125,
                       qint16 encL = -7, qint16 encR = 7, quint8 stateBits = 0x04,
                       quint8 fwVersion = 0xAF)
{
    std::vector<uint8_t> d;
    proto::putI16(d, bal);
    proto::putI16(d, vel);
    proto::putI16(d, turn);
    proto::putI16(d, moto1);
    proto::putI16(d, moto2);
    proto::putU16(d, ccrA);
    proto::putU16(d, ccrB);
    proto::putI16(d, encL);
    proto::putI16(d, encR);
    d.push_back(stateBits);
    d.push_back(fwVersion);
    return toQ(d);
}

/// 往 VehicleState 里喂一整套（每种上行帧各一条）—— UI 冒烟与端到端断言用
inline void feedAll(domain::VehicleState &st, io::LinkKind kind = io::LinkKind::Wire)
{
    st.onFrame(kind, proto::up::kPose, pose());
    st.onFrame(kind, proto::up::kMotion, motion());
    st.onFrame(kind, proto::up::kTask, task());
    st.onFrame(kind, proto::up::kEvent, event(proto::EventCode::SelfCheckDone));
    st.onFrame(kind, proto::up::kEnvGas, env());
    st.onFrame(kind, proto::up::kDiag, diag());
    st.onFrame(kind, proto::up::kLoop, loop());
    st.onFrame(kind, proto::up::kHeartbeat, QByteArray());
}

// ---- 按"物理量"造帧：写测试时不必自己算 ppm×10 / 0.01 V（算错会得到假结论）----

/// 0x14：直接给两路浓度（ppm），内部 ×10 成协议单位
inline QByteArray envPpm(double mq2ppm, double mq135ppm, quint8 ready = 0x0F)
{
    return env(1234, 2345, static_cast<quint16>(mq2ppm * 10.0 + 0.5),
               static_cast<quint16>(mq135ppm * 10.0 + 0.5), 256, 610, ready, 0x01);
}

/// 0x15：直接给电压（V），内部 ×100 成协议单位
inline QByteArray diagVolt(double volts, quint8 stateBits = 0x04)
{
    return diag(static_cast<quint16>(volts * 100.0 + 0.5), stateBits);
}

} // namespace testframes
