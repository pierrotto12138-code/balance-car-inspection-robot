#ifndef SYS_DELAY_DELAY_H
#define SYS_DELAY_DELAY_H
#include <sys.h>
//////////////////////////////////////////////////////////////////////////////////
void delay_init(u8 SYSCLK);
void delay_ms(u16 nms);
void delay_us(u32 nus);

/* ★ B（固件）2026-10-13（P2 分层整改）：毫秒级"让出 CPU"延时的**回调钩子**。
 *
 * 为什么需要它：`delay_ms()` 在调度器跑起来之后应让出 CPU（`vTaskDelay`），
 *   但 `System/` 层**不许依赖上层**（不 include FreeRTOS 头、更不许 `extern App_xxx`）。
 *   做法是：本层只持有一个函数指针，**由 App 层在启动时注入**，
 *   依赖方向因此保持单向 `App → System`（见 docs/design_firmware/编码规范.md §一）。
 *
 * 用法（App 层）：
 *     static void app_delay_ms(uint16_t nms) { vTaskDelay(pdMS_TO_TICKS(nms)); }
 *     Delay_SetMsHook(app_delay_ms);      // 紧接着 vTaskStartScheduler()
 *
 * 语义：钩子为 0（默认 / 纯裸机构建）→ `delay_ms()` 走 DWT 忙等；钩子非 0 → 调钩子。 */
typedef void (*DelayMsHook)(uint16_t nms);
void Delay_SetMsHook(DelayMsHook hook);

#endif
