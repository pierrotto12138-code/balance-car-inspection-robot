/*
 * FreeRTOS 回调钩子（B 新增，2026-10-09）
 *
 * 为什么必须有：
 *   FreeRTOSConfig.h 里把 configCHECK_FOR_STACK_OVERFLOW=2 和
 *   configUSE_MALLOC_FAILED_HOOK=1 打开了 —— 一旦任务栈溢出或堆不够，
 *   内核会调用下面两个钩子。**如果不实现，链接就会报未定义符号**；
 *   而如果实现了却什么都不做，故障就变成"莫名死机"。
 *   这里让 LED **常亮**并停住，配合调试器一眼看出问题。
 *
 * ★ 2026-10-13 修正（上板联调发现的真错误）：原来写的是 `LED = 1`，注释却说"点亮" ——
 *   但本板用户 LED 是 **低电平点亮**（`led.c` 里 `LED=0` 才是亮，见 `Board_LedSet()`），
 *   所以 `LED = 1` 实际把灯**熄掉**了：真出故障时反而什么指示都没有，
 *   与"故障要看得见"的初衷正好相反。
 *   现在改为：**LED = 0（常亮）**，并且__disable_irq() 后死循环 ——
 *   现场现象 = "LED 一直亮着不闪"（正常时是 1 Hz 闪烁），一眼可分。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "sys.h"

/* 用户 LED（PB13）低电平点亮 —— 这里直接写厂商宏，因为钩子可能在
   调度器启动前后任意时刻被调用，不能依赖其他模块已初始化。*/
#define FAULT_LED_ON()   (LED = 0)

/* 任务栈溢出：点亮 LED 并停住（不要再返回，状态已不可信） */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	FAULT_LED_ON();				/* 低电平点亮：现场看到"LED 常亮不闪"即此故障 */
	__disable_irq();
	for (;;)
	{
	}
}

/* 堆申请失败（heap_4 用尽）：同样点亮 LED 并停住 */
void vApplicationMallocFailedHook(void)
{
	FAULT_LED_ON();
	__disable_irq();
	for (;;)
	{
	}
}

/* 空闲任务钩子未启用（configUSE_IDLE_HOOK=0），故不提供 vApplicationIdleHook */

/* 供后续任务查询：调度器是否已经跑起来（调试用，不影响逻辑） */
volatile unsigned int g_rtos_ready = 0;
