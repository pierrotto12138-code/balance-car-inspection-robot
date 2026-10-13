#ifndef APP_TASK_APP_API_H
#define APP_TASK_APP_API_H

/*
 * **给上位机 / 后续开发用的只读接口**（B，2026-10-09）
 *
 * 为什么单独开一个 API 头：
 *   1. 上位机（C）要看的数据散落在各任务上下文里（姿态在中断、气体在 GasTask、
 *      状态在 TaskTask）。约定一个**唯一的读取入口**，比让调用方到处 extern 全局量安全得多；
 *   2. 后续要加"有线调参工具 / 数据记录 / 第二套上行帧"时，直接复用这些接口，
 *      不需要再动任务内部；
 *   3. 所有接口都是**拷贝出快照**（不返回内部指针），调用方拿到的数据自洽。
 *
 * ★ 线程安全约定：以下 getter 都自带短临界区，可**从任意任务**调用；
 *   **不要从高于 syscall 阈值的中断里调用**（会用到临界区/信号量）。
 *   中断里要用数据，请在中断里自己读写全局量（像控制中断那样）。
 *
 * ★ 单位约定（与协议 §5.1 一致，全部为整数定标值）：
 *     角度 0.01°、角速度 0.01 dps、轮速 1 mm/s、距离 1 cm、
 *     温度 0.1 ℃、湿度 0.1 %RH、浓度 ppm×10、电压 0.01 V
 */

#include <stdint.h>
#include "protocol.h"

/* ---- 姿态与运动（来自控制中断更新的全局量）---- */
typedef struct
{
	int16_t  pitchX100;      /* 俯仰 0.01° */
	int16_t  gyroX100;       /* 俯仰角速度 0.01 dps */
	int16_t  gyroTurnX100;   /* 转向角速度 0.01 dps */
	int32_t  encLeft;        /* 左编码器计数（10 ms 周期，原始脉冲）*/
	int32_t  encRight;       /* 右编码器计数 */
	int16_t  speedLmmS;      /* 左轮线速度 mm/s（按 CFG_* 标定换算）*/
	int16_t  speedRmmS;      /* 右轮线速度 mm/s */
	int16_t  pwmLeft;        /* 左电机 PWM（含符号）*/
	int16_t  pwmRight;       /* 右电机 PWM（含符号）*/
	uint16_t distCm;         /* 超声波距离 cm */
	int32_t  voltageX100;    /* 电池电压 0.01 V */
	uint8_t  stopped;        /* 1 = 电机当前被保护关闭（Flag_Stop）*/
} AppTelemetry;

/* 取一帧姿态/运动快照；返回 0 成功 */
int App_GetTelemetry(AppTelemetry *out);

/* ---- 气体与环境 ---- */
typedef struct
{
	uint16_t mq2Raw, mq135Raw;       /* ADC 原始值 0~4095（已 8 次平均）*/
	uint16_t mq2PpmX10, mq135PpmX10; /* 估计浓度 ppm×10（已饱和钳位）*/
	float    mq2Ratio, mq135Ratio;   /* Rs/R0（主链路量，未定标）*/
	float    mq2R0, mq135R0;         /* 标定得到的 R0（Ω）；0 表示未标定 */
	uint8_t  mq2Alarm, mq135Alarm;   /* 1 = 已确认超阈值 */
	int16_t  tempX10;                /* DHT11 温度 0.1 ℃ */
	uint16_t humiX10;                /* DHT11 湿度 0.1 %RH */
	uint8_t  dhtValid;               /* DHT 本周期读数是否有效 */
	uint8_t  readyBits;              /* 协议 0x14 的就绪位 */
	uint8_t  gear;                   /* 采样档位（PROTO_GEAR_*）*/
} AppGasSnapshot;

int App_GetGas(AppGasSnapshot *out);

/* ---- 任务状态机 ---- */
typedef struct
{
	uint8_t  phase;        /* PROTO_PHASE_* */
	uint8_t  taskId;       /* PROTO_TASK_* */
	uint8_t  progress;     /* 0~100 */
	uint8_t  waypoint;
	uint8_t  fault;        /* 异常标志位（协议 0x12 的第 5 字节）*/
	uint8_t  safetyStop;   /* 1 = 安全停车中 */
	uint8_t  stopReason;   /* PATROL_STOP_* 位组合 */
} AppPatrolStatus;

int App_GetPatrol(AppPatrolStatus *out);

/* ---- 通信与系统诊断（调参/排障很有用）---- */
typedef struct
{
	uint32_t rxBytesBt, rxBytesWire;       /* 各链路收到的字节数 */
	uint32_t rxDroppedBt, rxDroppedWire;   /* 环形缓冲丢弃 */
	uint32_t overrunsBt, overrunsWire;     /* 串口 ORE 次数 */
	uint32_t framesOk, framesDropped;      /* 协议解帧统计 */
	uint32_t taskTicks[4];                 /* 四个任务的心跳计数（确认任务在跑）*/
	uint32_t heapFreeBytes;                /* FreeRTOS 剩余堆（查内存泄漏）*/
	uint32_t minEverFreeHeap;              /* 历史最小剩余堆 */
	uint8_t  tuningMode;                   /* 1 = 调参观测模式（100 Hz 上行）*/
} AppDiag;

int App_GetDiag(AppDiag *out);

/* ---- PID 参数（读 / 写 / 存 / 载）----
 * 写参数会立即生效（对齐 float 写是原子的，控制中断下一拍就用新值）。
 * 典型调参流程见 docs/design_firmware/程序说明文档.md §5.5。*/
int App_SetPidParam(uint8_t paramId, float value);   /* 0=成功 */
int App_GetPidParam(uint8_t paramId, float *value);  /* 0=成功 */
int App_SavePidParams(void);                         /* 写 Flash（整扇区擦除，勿频繁调用）*/
int App_LoadPidParams(void);                         /* 从 Flash 读回 */

/* 调参观测模式：进入后 0x10/0x11 以 100 Hz 上报（PID 整定用）*/
void App_SetTuningMode(int on);
int  App_GetTuningMode(void);

#endif /* APP_TASK_APP_API_H */
