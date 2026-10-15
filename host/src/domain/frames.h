#pragma once

// ============================================================================
//  上行帧的字段结构体与解析（domain 层）
//
//  为什么把解析从界面里拿出来单列一层：
//    · 字段偏移与单位口径**只能有一处实现**（协议 §七 约定 1~4；界面里出现
//      `data.at(6)` 就是 bug 温床 —— 偏移写错一个字节，现象是"数值乱跳"，无从查起）；
//    · 解析是纯函数，在 PC 上就能单测（不需要板子、不需要界面）；
//    · 界面只读结构体，不再碰字节流。
//
//  ★ 长度校验用**精确相等**，不用 "≥"：协议把每个命令的 N 钉死了
//    （0x11 曾经写成 11、v1.1 修正为 9）。按 ≥ 判会把"旧格式/异构固件"静默当新格式用，
//    读出来的值全是错位的 —— 宁可判为无效并计数（VehicleState 会暴露"格式错帧数"）。
//
//  依据：docs/protocol/communication_protocol.md v1.1（含 §3.4 0x15、§3.5 0x16、§3.7 版本字节）
// ============================================================================

#include "frame.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace domain {

/// 0x10 姿态数据（N=12）
struct PoseData {
    double pitchDeg = 0.0; ///< 俯仰（0.01° → °）
    double rollDeg = 0.0;  ///< 横滚
    double yawDeg = 0.0;   ///< 偏航
    double gyroXDps = 0.0; ///< 三轴角速度（0.01 dps → °/s）
    double gyroYDps = 0.0;
    double gyroZDps = 0.0;
};

/// 0x11 运动状态（N=9）
struct MotionData {
    int leftMmS = 0;      ///< 左轮速 mm/s（有符号：负 = 反转）
    int rightMmS = 0;     ///< 右轮速
    double pwmPercent = 0.0; ///< PWM 占空比（千分比 → %，协议 §七 约定 4）
    int distanceCm = 0;   ///< 超声波距离 cm
    uint8_t avoidFlags = 0; ///< 避障状态位（协议未细化位定义 → 只原样显示，不做语义假设）
};

/// 0x12 任务状态（N=5）
struct TaskData {
    proto::TaskId taskId = proto::TaskId::Idle;
    proto::Phase phase = proto::Phase::Init;
    int progressPct = 0;   ///< 完成度 %
    int checkpoint = 0;    ///< 当前巡检点
    uint8_t faultFlags = 0; ///< 异常标志位：bit0 气体 / bit1 失联 / bit2 硬件 / bit3 传感器

    bool gasFault() const { return (faultFlags & 0x01) != 0; }
    bool linkFault() const { return (faultFlags & 0x02) != 0; }
    bool hardwareFault() const { return (faultFlags & 0x04) != 0; }
    bool sensorFault() const { return (faultFlags & 0x08) != 0; }
    bool anyFault() const { return faultFlags != 0; }
};

/// 0x13 事件告警（N=5）
struct EventData {
    proto::EventCode code = proto::EventCode::SelfCheckDone;
    uint32_t timestampMs = 0; ///< 机器人侧时间戳（ms）；事件给"时刻"，状态位给"现状"
};

/// 0x14 环境与气体（N=16）
struct EnvData {
    int mq2Raw = 0;      ///< MQ-2 ADC 原始值（0–4095）
    int mq135Raw = 0;    ///< MQ-135 ADC 原始值
    double mq2Ppm = 0.0;   ///< 补偿后浓度（ppm×10 → ppm）
    double mq135Ppm = 0.0;
    double tempC = 0.0;    ///< 0.1 ℃ → ℃
    double humidityPct = 0.0; ///< 0.1 %RH → %RH
    uint8_t readyFlags = 0; ///< bit0 MQ-2 / bit1 MQ-135 / bit2 DHT11 有效 / bit3 补偿已启用
    proto::SampleGear gear = proto::SampleGear::Normal;

    bool mq2Ready() const { return (readyFlags & 0x01) != 0; }
    bool mq135Ready() const { return (readyFlags & 0x02) != 0; }
    bool dht11Valid() const { return (readyFlags & 0x04) != 0; }
    bool compensationOn() const { return (readyFlags & 0x08) != 0; }
};

/// 0x15 诊断（N=16，协议 §3.4）—— 联调专用：把"看不见的东西"变成可测量的数
struct DiagData {
    double batteryV = 0.0;  ///< 0.01 V → V（★ 全部上行帧里只有这一帧带电压）
    uint8_t page = 0;       ///< 0=厂商页 1=环境气体页 2=巡检通信页
    bool flagShow = false;  ///< 非 0 = 屏幕让位给 DataScope 波形通道
    uint8_t attitudeAlgo = 1; ///< 1=四元数 2=卡尔曼 3=互补滤波
    uint8_t stateBits = 0;  ///< bit0 报警中 / bit1 已安全锁存 / bit2 DHT11 有效 / bit3 蓝牙收过数据
    int heapKb = 0;         ///< 剩余堆 KB
    uint32_t oledBytes = 0; ///< OLED 累计写入字节（按秒差分 = 写入速率；不涨 = 屏幕没在刷）
    uint8_t tickComm = 0;   ///< 四个任务 tick 低 8 位：通信 / 气体 / 状态 / 界面
    uint8_t tickGas = 0;
    uint8_t tickState = 0;
    uint8_t tickUi = 0;

    bool alarmActive() const { return (stateBits & 0x01) != 0; }
    bool safetyLatched() const { return (stateBits & 0x02) != 0; }
    bool dht11Valid() const { return (stateBits & 0x04) != 0; }
    bool btLinkSeen() const { return (stateBits & 0x08) != 0; }
};

/// 0x16 三环实况（N=20，协议 §3.5）—— 调参核心：把中间量全部摊开
struct LoopData {
    int balancePwm = 0;  ///< 平衡环输出（P+D）
    int velocityPwm = 0; ///< 速度环输出（PI）
    int turnPwm = 0;     ///< 转向环输出（PD）
    int moto1 = 0;       ///< 送 Set_Pwm 前的左轮值（含限幅）
    int moto2 = 0;       ///< 右轮
    int ccrA = 0;        ///< 真实写进 TIM1->CCR4 的占空（左轮）
    int ccrB = 0;        ///< 右轮（TIM1->CCR1）
    int encLeft = 0;     ///< 编码器原始计数（每 10 ms）
    int encRight = 0;
    uint8_t stateBits = 0; ///< bit0 Flag_Stop / bit1 App_MotorDisable / bit2 本拍真的调用了 Set_Pwm
    uint8_t fwVersion = 0; ///< 固件版本字节 (月<<4)|日（协议 §3.7）

    bool flagStop() const { return (stateBits & 0x01) != 0; }
    bool motorDisabled() const { return (stateBits & 0x02) != 0; }
    bool setPwmCalled() const { return (stateBits & 0x04) != 0; }

    /// "算了没执行"的直接断言：算出的轮值非零、但写进寄存器的占空为 0
    bool computedButNotExecuted() const
    {
        return (moto1 != 0 && ccrA == 0) || (moto2 != 0 && ccrB == 0);
    }
};

/// 0x24 PID 调参回读（N=6，协议 §4.2）—— **下行命令的上行回显**：
/// 机器人收到 0x24 后把**实际生效值**按同样格式回发一次，上位机据此确认写入成功
/// （避免"以为改了、其实没收到"）。扩展 ID 也用它回执：
/// `0x00`=保存成功 / `0xFE`=从 Flash 载入成功 / `0xFD`=观测模式当前状态（值 1 或 0）。
struct PidTuneData {
    quint8 paramId = 0;
    quint8 reserved = 0;
    float value = 0.0f; ///< IEEE-754 小端（协议 §七 约定 5）

    /// 是否是扩展 ID 的回执（不是真实增益）
    bool isAck() const { return paramId == 0x00 || paramId == 0xFE || paramId == 0xFD; }
};

// ---- 解析（返回 false = 长度不符/未识别，调用方应计数并保留旧值）----
bool decodePose(const QByteArray &d, PoseData &out);
bool decodeMotion(const QByteArray &d, MotionData &out);
bool decodeTask(const QByteArray &d, TaskData &out);
bool decodeEvent(const QByteArray &d, EventData &out);
bool decodeEnv(const QByteArray &d, EnvData &out);
bool decodeDiag(const QByteArray &d, DiagData &out);
bool decodeLoop(const QByteArray &d, LoopData &out);
bool decodePidTune(const QByteArray &d, PidTuneData &out);

/// 协议规定的上行数据段长度（-1 = 本版本协议未定义该命令）
int expectedUpLen(quint8 cmd);

// ---- 名称表（界面与日志共用，避免各处硬编码中文字符串）----
const char *phaseName(proto::Phase p);
const char *taskName(proto::TaskId t);
const char *eventName(proto::EventCode c);
const char *sampleGearName(proto::SampleGear g);
const char *attitudeAlgoName(uint8_t algo);
const char *diagPageName(uint8_t page);

/// 异常标志位 → 可读文本（"气体|失联"；无异常时 "无"）
QString faultFlagsText(uint8_t flags);

/// 固件版本字节 → "(月<<4)|日" 的可读形式，如 0xAF → "10-15"
QString firmwareVersionText(uint8_t v);

} // namespace domain
