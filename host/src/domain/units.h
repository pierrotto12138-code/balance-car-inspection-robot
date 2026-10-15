#pragma once

// ============================================================================
//  单位换算与数值格式化（domain 层，唯一实现处）
//
//  ★ 为什么要单列一个文件，而不是在界面里随手 *0.1：
//    协议 §七「给上位机的显示约定」把口径钉死了（浓度 ppm×10、角度 0.01°、
//    电压 0.01 V、PWM 千分比）。这类"少除一个 10"的错误**看现象看不出来** ——
//    界面照样显示 123.4 而不是 1234，谁也不会怀疑。
//    因此约定：**任何 0.1 / 0.01 只允许出现在本文件**，界面里只准调用这里的函数。
//    （同一条纪律在固件侧是 docs/design_firmware/编码规范.md 的"单位只在驱动层换算"。）
//
//  本文件只依赖 QtCore（QString），不依赖任何界面类，可直接单测。
// ============================================================================

#include <QString>

#include <cstdint>

namespace domain {

namespace units {

// ---- 缩放因子（依据：docs/protocol/communication_protocol.md §三 / §七）----
constexpr double kDegPerRaw        = 0.01;  // 0x10 角度：0.01°
constexpr double kDpsPerRaw        = 0.01;  // 0x10 角速度：0.01 dps
constexpr double kPpmPerRaw        = 0.1;   // 0x14 浓度：ppm×10
constexpr double kCelsiusPerRaw    = 0.1;   // 0x14 温度：0.1 ℃
constexpr double kRhPerRaw         = 0.1;   // 0x14 湿度：0.1 %RH
constexpr double kVoltPerRaw       = 0.01;  // 0x15 电压：0.01 V
constexpr double kPercentPerPermille = 0.1; // 0x11 PWM：千分比 → %

/// 低压报警线（协议 §七 约定 2：11.10 V 为低位报警线）
constexpr double kLowBatteryV = 11.10;

/// 超声波距离：`0x11` 的字段是**毫米**（见 `frames.h` 的 `MotionData::distanceMm` 说明），
/// 界面按 **cm** 显示 ⇒ 换算只在这里做一次（决策 65：换算只允许出现在 units 层）。
constexpr double kMmPerCm = 10.0;
inline double cmFromMm(double mm) { return mm / kMmPerCm; }

inline double deg(int16_t raw) { return static_cast<double>(raw) * kDegPerRaw; }
inline double dps(int16_t raw) { return static_cast<double>(raw) * kDpsPerRaw; }
inline double ppm(uint16_t raw) { return static_cast<double>(raw) * kPpmPerRaw; }
inline double celsius(int16_t raw) { return static_cast<double>(raw) * kCelsiusPerRaw; }
inline double rh(uint16_t raw) { return static_cast<double>(raw) * kRhPerRaw; }
inline double volt(uint16_t raw) { return static_cast<double>(raw) * kVoltPerRaw; }
inline double pwmPercent(int16_t raw) { return static_cast<double>(raw) * kPercentPerPermille; }

/// 固定小数位格式化。
///
/// ★ 两点讲究：
///   ① **永远定小数位**：数值在 9.99↔10.00 之间跳时若位数变化，整列数字会横向抖动
///      （ISA-101 要求"同一量始终保持同一位置与量程"，这是可验收项）；
///   ② **消掉负零**：定点量在 0 附近会出现 `-0.00`，它只是补码符号位的产物，
///      显示出来会让人以为"漂了"，统一显示为 `0.00`。
QString number(double v, int decimals);

/// "数值 + 单位"；unit 为空时只输出数值（中间不加空格）
QString withUnit(double v, int decimals, const QString &unit);

} // namespace units
} // namespace domain
