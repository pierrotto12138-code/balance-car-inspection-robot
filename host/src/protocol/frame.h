#pragma once

#include <cstdint>

/// 协议常量定义。
/// 唯一依据：docs/protocol/communication_protocol.md（v1.0）。
/// 任何改动必须同步更新该文档并升级版本号。
namespace proto {

// ---- 帧结构常量 ----
constexpr uint8_t  kHeader0 = 0xAA;
constexpr uint8_t  kHeader1 = 0x55;
constexpr uint8_t  kHeaderLen = 2;   // 帧头
constexpr uint8_t  kCmdLen    = 1;   // 命令字
constexpr uint8_t  kLenLen    = 2;   // 长度字段
constexpr uint8_t  kCrcLen    = 2;   // 校验
constexpr uint8_t  kOverhead  = kHeaderLen + kCmdLen + kLenLen + kCrcLen; // 7
constexpr uint16_t kMaxDataLen = 256; // 数据段上限，防异常长度导致内存问题
constexpr int      kRxTimeoutMs = 100; // 单帧接收超时

// ---- 上行命令字（机器人 -> 监控）----
namespace up {
constexpr uint8_t kPose      = 0x10; // 姿态数据  N=12
constexpr uint8_t kMotion    = 0x11; // 运动状态  N=11
constexpr uint8_t kTask      = 0x12; // 任务状态  N=5
constexpr uint8_t kEvent     = 0x13; // 事件告警  N=5
constexpr uint8_t kEnvGas    = 0x14; // 环境与气体 N=16
constexpr uint8_t kHeartbeat = 0x1F; // 心跳      N=0
} // namespace up

// ---- 下行命令字（监控 -> 机器人）----
namespace down {
constexpr uint8_t kMotion    = 0x20; // 运动控制  N=5
constexpr uint8_t kTask      = 0x21; // 任务指示  N=2+2k
constexpr uint8_t kMode      = 0x22; // 控制模式  N=1
constexpr uint8_t kSurveyCfg = 0x23; // 巡检参数  N=12
constexpr uint8_t kPidTune   = 0x24; // PID 调参  N=6
constexpr uint8_t kEstop     = 0x2F; // 紧急停机  N=0
} // namespace down

// ---- 任务阶段枚举（0x12 第 2 字节）----
enum class Phase : uint8_t {
    Init     = 0x00,
    Warmup   = 0x01,
    Idle     = 0x02,
    Patrol   = 0x03,
    Analysis = 0x04,
    Return   = 0x05,
    Done     = 0x06,
    Fault    = 0x07,
};

// ---- 事件码（0x13 第 1 字节）----
enum class EventCode : uint8_t {
    SelfCheckDone = 0x01,
    TaskStart     = 0x02,
    TaskDone      = 0x03,
    TaskFail      = 0x04,
    CrashOrLowBat = 0x05,
    GasOverLimit  = 0x06,
    WarmupOrFail  = 0x07,
};

// ---- 任务 ID ----
enum class TaskId : uint8_t {
    Idle   = 0x00,
    Patrol = 0x01,
    Return = 0x02,
};

// ---- 采样档位（0x14 第 14 字节）----
enum class SampleGear : uint8_t {
    Warmup = 0,
    Normal = 1, // 1 Hz
    Fast   = 2, // 5 Hz
};

// ---- PID 参数 ID（0x24 第 1 字节）----
enum class PidParam : uint8_t {
    BalanceKp  = 0x01,
    BalanceKd  = 0x02,
    VelocityKp = 0x03,
    VelocityKi = 0x04,
    Zhongzhi   = 0x05,
    TurnKp     = 0x06,
    TurnKd     = 0x07,
};

} // namespace proto
