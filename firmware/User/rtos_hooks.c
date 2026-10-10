/*
 * FreeRTOS 回调钩子（B 新增，2026-10-09）
 *
 * 为什么必须有：
 *   FreeRTOSConfig.h 里把 configCHECK_FOR_STACK_OVERFLOW=2 和
 *   configUSE_MALLOC_FAILED_HOOK=1 打开了 —— 一旦任务栈溢出或堆不够，
 *   内核会调用下面两个钩子。**如果不实现，链接就会报未定义符号**；
 *   而如果实现了却什么都不做，故障就变成"莫名死机"。
 *   这里让 LED 常亮 + 断点停住，配合调试器一眼看出问题。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "sys.h"

/* 任务栈溢出：把用户 LED 点亮并停住（不要再返回，状态已不可信） */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	LED = 1;					/* 厂商 LED 宏，PB13 */
	__disable_irq();
	for (;;)
	{
	}
}

/* 堆申请失败（heap_4 用尽）：同样点亮 LED 并停住 */
void vApplicationMallocFailedHook(void)
{
	LED = 1;
	__disable_irq();
	for (;;)
	{
	}
}

/* 空闲任务钩子未启用（configUSE_IDLE_HOOK=0），故不提供 vApplicationIdleHook */

/* 供后续任务查询：调度器是否已经跑起来（调试用，不影响逻辑） */
volatile unsigned int g_rtos_ready = 0;
