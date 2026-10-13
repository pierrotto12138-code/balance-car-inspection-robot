#ifndef __ODOM_H
#define __ODOM_H

/*
 * 里程计（纯逻辑：只有加减法，可在 PC 上测）
 *
 * 作用：给巡检状态机提供"**走到下一个巡检点了**"这个输入。
 *
 * 为什么不用红外：底板 `J11`/`J14` 两路红外是 **DNP（留焊盘不留排针）**，
 * 实物上没有；而编码器每 10 ms 都会更新，用它做里程**不需要额外器件**。
 * （若以后补焊红外，可在 `App_Task` 里把 `waypointReached` 换成红外判据，
 *   状态机不用改 —— 它只认这一个布尔输入。）
 *
 * 数据流：
 *   控制中断（10 ms 分支）→ Odom_AddCounts(左, 右)   ← 纯加法，中断安全
 *   任务（TaskTask，100 ms）→ Odom_WaypointReached()  → PatrolInput.waypointReached
 *
 * 单位：内部按 **mm** 累积（由调用方给出每计数多少毫米，见 app_config.h 的 CFG_MM_PER_COUNT）。
 */

#include <stdint.h>

/* 初始化：spacingMm = 相邻巡检点的距离（mm）*/
void Odom_Init(uint32_t spacingMm);

/* 设定标定系数：每 1 个编码器计数对应多少**微米**（µm）。
 * 由调用方从 app_config.h 的 CFG_MM_PER_COUNT 换算后传入 ——
 * 这样本模块**不依赖任何配置头/硬件头**，可以在 PC 上直接编译测试。*/
void Odom_SetScaleUmPerCount(uint32_t umPerCount);

/* 中断里调用：把这一拍（10 ms）的左右编码器计数累进里程。
   ★ 只做整数加减，不调用任何 RTOS API、不做除法（Cortex-M4 无硬件除法也要避免在中断里做）。*/
void Odom_AddCounts(int left, int right);

/* 任务里调用：本段里程是否已达到一个巡检点间距。
   返回 1 表示"到位"，并**自动开始下一段**（只返回一次 1，避免重复触发）。*/
int  Odom_WaypointReached(void);

/* 当前这一段已走的里程（mm）与总共走过的里程（mm）*/
uint32_t Odom_LegMm(void);
uint32_t Odom_TotalMm(void);

/* 清零（重新开始一次巡检任务时调用）*/
void Odom_Reset(void);

/* 把计数换算成毫米累加量：内部用，暴露出来便于单测。
   返回本拍前进的毫米数（左右取绝对值平均）。*/
uint32_t Odom_CountsToMm(int left, int right);

#endif /* __ODOM_H */
