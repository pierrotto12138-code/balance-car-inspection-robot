#ifndef __PAGES_H
#define __PAGES_H

/*
 * 自研 OLED 显示页（B，2026-10-09）
 *
 * 交互：**USER 按键双击**切页（仅 FreeRTOS 模式）——
 *   单击 = 启停、长按 = 普通/避障（**厂商原语义，保持不变**），
 *   双击原本是切 `Flag_Show`（APP 界面 ↔ 上位机 DataScope），
 *   但 **RTOS 模式下不用 DataScope**（那条串口已改成有线协议链路），
 *   所以把双击改成切页，既不新增手势也不与厂商功能冲突。
 *
 * 页序：
 *   0 = 厂商原始界面（`APP_Show()` + `oled_show()`）
 *   1 = 环境与气体（双路 ppm / Rs-R0 / 温湿度 / 报警）
 *   2 = 巡检与通信（阶段 / 任务 / 巡检点 / 两条链路统计 / 帧统计 / 剩余堆）
 *
 * ★ 中断安全：`Pages_Next()` 会被控制中断调用，它**只自增一个 volatile 变量**，
 *   不调用任何 RTOS API、不碰 OLED。
 */

#include "sys.h"

#define PAGES_COUNT   3
#define PAGE_VENDOR   0
#define PAGE_GAS      1
#define PAGE_PATROL   2

extern volatile u8 Flag_Page;

/* 由控制中断（USER 按键双击）调用 */
void Pages_Next(void);

/* 由 UiTask 调用：绘制当前页并刷新 */
void Pages_Show(void);

#endif /* __PAGES_H */
