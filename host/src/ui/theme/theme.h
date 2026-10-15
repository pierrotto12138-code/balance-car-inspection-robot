#pragma once

// ============================================================================
//  主题：配色变量 + 全局 QSS（ISA-101 工业人机界面原则的落地）
//
//  为什么把颜色写成"变量"而不是到处 #RRGGBB：
//    ISA-101 的核心是**颜色只用来表达异常**（正常状态一律中性灰），
//    所以"哪个颜色什么含义"必须只有一处定义 —— 否则界面上很快就会出现
//    "用绿色表示在线、用蓝色表示正常、用黄色表示普通"这种装饰性用色，
//    一眼看过去全是颜色，等于没有颜色。
//
//  配色约定（每个颜色在界面里**有且只有一个含义**）：
//    中性灰   → 一切正常状态与普通信息
//    琥珀     → 警示（低压、接近阈值、链路超时）
//    红       → 报警/危险（超阈值、故障、安全锁存）
//    灰(offline) → 离线/未知/无数据
//    绿       → 仅用于"状态灯"表达"这一位是好的"（不作装饰）
//
//  另外两条可验收的规矩：不用渐变/阴影/3D，圆角 ≤ 6 px；
//  数字用等宽字体 + 固定小数位（数值跳动时不抖列）。
// ============================================================================

#include <QFont>
#include <QString>

namespace theme {

constexpr const char *kBg      = "#1E2227"; ///< 窗口底色（低饱和中性灰）
constexpr const char *kPanel   = "#262B31"; ///< 面板/卡片
constexpr const char *kPanel2  = "#2B3138"; ///< 次级面板（表头、进度条槽）
constexpr const char *kLine    = "#3A4048"; ///< 分隔线/边框
constexpr const char *kText    = "#E6E9ED"; ///< 主文字（与底色对比度 ≥ 4.5:1）
constexpr const char *kTextDim = "#9AA3AD"; ///< 次要文字
constexpr const char *kOk      = "#3FA45B"; ///< 运行正常（仅状态灯）
constexpr const char *kWarn    = "#E0A800"; ///< 警示：琥珀
constexpr const char *kAlarm   = "#D9534F"; ///< 报警/危险：红
constexpr const char *kOffline = "#6C757D"; ///< 离线/未知：灰
constexpr const char *kGrid    = "#2E343B"; ///< 图表网格/地平线以下

/// 全局样式表（在 main 里 qApp->setStyleSheet(theme::styleSheet()) 应用一次）
QString styleSheet();

/// 等宽数字字体（Consolas 缺失时退化为系统等宽，保证列对齐）
QFont monoFont(int pointSize, bool bold = false);

} // namespace theme
