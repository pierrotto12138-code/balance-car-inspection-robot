#ifndef __APP_TASKS_H
#define __APP_TASKS_H

/*
 * 应用任务层（混合式架构的落地，决策 8/42）
 *
 *   ┌──────────────── 中断上下文（时序一字不改）────────────────┐
 *   │ EXTI15_10_IRQHandler  5 ms/10 ms：姿态 + 三环 PID + PWM   │
 *   │ USART2_IRQHandler     只把字节推进环形缓冲                │
 *   └──────────────────────────────────────────────────────────┘
 *   ┌──────────────── FreeRTOS 任务 ────────────────────────────┐
 *   │ CommTask  20 ms：解帧下行指令 / 排空事件 / 定时上行（心跳、姿态、气体）│
 *   │ GasTask   1 Hz 常态 / 5 Hz 异常：ADC(PA5/PA6) + DHT11(PA7) → 研判    │
 *   │ TaskTask 100 ms：巡检状态机推进（含 500 ms 失联安全停车）           │
 *   │ UiTask   100 ms：OLED 刷新 + 蜂鸣器报警                            │
 *   └──────────────────────────────────────────────────────────┘
 *
 * ★ 中断与任务之间**只有两条通道**，都是"无锁可证明"的：
 *     1. SPSC 环形缓冲（串口字节，见 comm_link/ringbuf）
 *     2. 对厂商全局量的**短临界区读取**（姿态/编码器/电压）—— 见 app_tasks.c 的说明：
 *        对齐的 32 位读写本身是原子的，临界区只是为了让"一组相关量"尽量来自同一拍；
 *        读到的数据只用于上报/显示，**不参与控制**，所以不需要更强的同步。
 *
 * ★ 所有任务都不调用控制相关的写操作（不改 PID 参数以外的任何控制量；
 *   PID 调参通过 0x24 写入的是全局 float，控制中断直接读它们 —— float 对齐写是原子的）。
 */

/* 创建任务并进入调度（不返回）。SUCCESS 时永不返回 */
void App_Start(void);

/* 输出 0/1：调度器是否已在运行（供 delay_ms 等判断）*/
int  App_SchedulerRunning(void);

/* 供 delay_ms 使用：把毫秒延时转成 vTaskDelay */
void App_DelayMs(unsigned short nms);

/* 各任务的"心跳"计数（便于用调试器确认任务真的在跑）*/
unsigned long App_TaskTicks(int idx);

/* 任务索引 */
#define APP_TASK_COMM  0
#define APP_TASK_GAS   1
#define APP_TASK_STATE 2
#define APP_TASK_UI    3
#define APP_TASK_COUNT 4

#endif /* __APP_TASKS_H */
