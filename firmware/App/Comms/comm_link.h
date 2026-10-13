#ifndef APP_COMMS_COMM_LINK_H
#define APP_COMMS_COMM_LINK_H

/*
 * 通信链路胶水层（**STM32 侧**）：两条串口 + 环形缓冲 + 发送互斥
 *
 *   COMM_LINK_BT   = USART2（PA2/PA3，蓝牙 SPP）—— 协议**主链路**，演示/验收用
 *   COMM_LINK_WIRE = USART1（PA9/PA10，USB-TTL）—— **有线调试链路**，同一套协议，
 *                    用于实验室调试与 PID 在线调参（不受蓝牙配对与干扰影响）
 *
 * 两条链路**对等**：收到帧就处理，回复原路返回（见 app_tasks.c 的 on_frame）。
 *
 * 分工（决策 42）：
 *   中断（USART2 / USART1，抢占优先级均为 0）：
 *        **只做一件事** —— 把字节塞进对应环形缓冲，处理 ORE。
 *        ★ 绝不调用任何 FreeRTOS API：它们的优先级高于
 *          configMAX_SYSCALL_INTERRUPT_PRIORITY(5)，在里面调 RTOS API 会破坏内核。
 *   任务（CommTask）：取字节 → 喂协议解帧器 → 处理指令 / 排空事件 / 定时上行。
 *
 * 为什么用环形缓冲而不是"中断里直接解帧"：解帧要跨多次中断（一帧 7~23 字节），
 * 在中断里做状态机会拉长中断、加大控制节拍（每 5 ms）的抖动 —— 那是平衡车的命脉。
 */

#include <stdint.h>

typedef enum
{
	COMM_LINK_BT = 0,        /* USART2（蓝牙）*/
	COMM_LINK_WIRE = 1,      /* USART1（有线调试）*/
	COMM_LINK_COUNT = 2
} CommLinkId;

void     CommLink_Init(void);

/* ---- 中断上下文（不得调用 RTOS API）---- */
void     CommLink_OnRxByte(CommLinkId link, uint8_t b);
void     CommLink_NoteOverrun(CommLinkId link);

/* ---- 任务上下文 ---- */
uint16_t CommLink_Read(CommLinkId link, uint8_t *out, uint16_t maxLen);
uint16_t CommLink_Send(CommLinkId link, const uint8_t *data, uint16_t len);

/* 供 printf 重定向使用（System/usart/usart.c 的 _write 会调它们）；
   ★ 两条链路共用一个 TX 互斥量，避免同一端口上协议帧与 printf 交错成乱码 */
void     CommLink_LockTx(void);
void     CommLink_UnlockTx(void);

/* 同一帧同时发到两条链路（事件告警这类"两边都该看到"的用）*/
void     CommLink_SendAll(const uint8_t *data, uint16_t len);

/* ---- 诊断计数（上位机/调试可读）---- */
uint32_t CommLink_RxBytes(CommLinkId link);
uint32_t CommLink_RxDropped(CommLinkId link);   /* 环形缓冲满而丢弃的字节数 */
uint32_t CommLink_Overruns(CommLinkId link);    /* 串口硬件 ORE 次数 */
uint32_t CommLink_NowMs(void);                  /* 毫秒时基（RTOS 下为内核节拍）*/

#endif /* APP_COMMS_COMM_LINK_H */
