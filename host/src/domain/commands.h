#pragma once

// ============================================================================
//  下行命令的数据段构造（domain 层）
//
//  与 `frames.h/.cpp`（上行帧解析）互为反面：**上面这里只造数据段**，
//  帧头/长度/CRC 由 `proto::encodeFrame()` 统一加（链路层发送时也会再加一次，
//  所以这里绝不能返回"整帧"——否则会出现"帧里套帧"这种极难查的错）。
//
//  为什么把下行的字段偏移也集中在一处：
//    与上行同理 —— 偏移错一位的现象是"车按错误的语义动"或"参数写飞"，
//    而且下行是**有副作用**的，比读错更危险。所有下行帧都从这里出去，
//    单元测试再做"造 → 组帧 → 解帧 → 逐字段比对"的往返验证。
//
//  依据：docs/protocol/communication_protocol.md §四 / §3.6 / §3.8 / §4.1 / §4.2
// ============================================================================

#include "frame.h"

#include <QByteArray>
#include <QList>

namespace domain {
namespace cmd {

/// 0x20 运动控制（N=5）：线速度 int16(mm/s)、角速度 int16(0.1 dps)、使能 uint8。
///
/// ★ 固件侧的语义（协议 §3.6）：连续量只做**方向判定** ——
///   `|linMmS| ≥ 50` 判前进/后退、`|angDpsX10| ≥ 100`(10 dps) 判左/右转、
///   `enable=0` 立即停。所以这里不必凑"精确速度"，但要让数值**明确越过判据**。
QByteArray motionPayload(int linMmS, int angDpsX10, bool enable);

/// 0x20 立即停止（enable=0）。**松手即停**用它。
QByteArray stopPayload();

/// 0x21 任务指示（N=2+2k）：任务 ID uint8、参数个数 uint8、参数 uint16×k
QByteArray taskPayload(proto::TaskId id, const QList<quint16> &params = QList<quint16>());

/// 0x22 控制模式（N=1）：0=平衡 1=遥控 2=自主
QByteArray modePayload(quint8 mode);

/// 0x23 巡检参数（N=12）：采样周期 ms、MQ-2 阈值(ppm)、MQ-135 阈值(ppm)、
/// 自适应采样使能、温湿度补偿使能、连续确认帧数、保留
QByteArray surveyCfgPayload(quint16 samplePeriodMs, double mq2Ppm, double mq135Ppm,
                            bool adaptive, bool compensation, quint8 confirmFrames);

/// 0x24 PID 调参（N=6）：参数 ID uint8、保留 uint8、参数值 **float32 小端**（协议 §七 约定 5）
///
/// ★ NaN/Inf 一律不发送（协议 §3.8 固件会拒绝，但**上位机不该把明显非法的值发出去**：
///   发出去只会换来一条"回读仍是旧值"，让人误以为"写不进去"）。
bool pidTunePayload(quint8 paramId, float value, QByteArray *out);

/// 0x2F 紧急停机（N=0）—— 机器人立即切断电机输出并锁存
QByteArray estopPayload();

/// 0x1F 心跳（N=0）—— **保活专用**：它只声明"上位机在线"，
/// **不会**解除安全锁存、也不会使能电机（协议 §七 约定 7）。
QByteArray heartbeatPayload();

/// 命令字的可读名（界面提示、事件流留档共用；避免各处硬编码）
const char *name(quint8 cmd);

/// 该命令字是否会**改变安全状态**（写这组函数是为了让界面能"显式提示后果"）：
/// `0x2F` 会锁存；`0x20(使能)/0x21` 会**解除**锁存；心跳与查询类不会。
enum class SafetyEffect {
    None = 0,     ///< 不影响安全状态
    Latches,      ///< 会置位安全锁存
    Releases,     ///< 可能解除安全锁存
};

/// 该动作的安全影响（界面按此决定是否二次确认/提示）
SafetyEffect safetyEffect(quint8 cmd);

} // namespace cmd
} // namespace domain
