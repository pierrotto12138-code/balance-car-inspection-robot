#ifndef __PATROL_H
#define __PATROL_H

/*
 * 巡检任务状态机（**纯逻辑层**：无 ST 头文件、时间由外部注入 → 可在 PC 上跑）
 *
 * 阶段枚举直接沿用协议 0x12 的 PROTO_PHASE_*（见 docs/protocol/communication_protocol.md §3.2）：
 *     0x00 初始化/自检 → 0x01 预热 → 0x02 待机 → 0x03 自主巡航
 *     → 0x04 异常研判/驻留加密采样 → 0x05 返航 → 0x06 完成 / 0x07 故障·安全停车
 *
 * ★ 三条安全设计（决策 41）：
 *   1. **500 ms 未收到任何下行帧 → 安全停车**（协议 §5 的机器人侧失联保护）；
 *   2. 安全停车是**锁存**的：链路恢复**不会**自动重新起步，必须由指令（0x21 任务 / 0x20 使能）
 *      显式解除 —— 否则"信号一抖车就自己冲出去"，这在仓库里是不可接受的；
 *   3. 急停（0x2F）与硬件故障（低电压/倾角，由控制侧给出）同样走同一条安全停车通道。
 *
 * ★ 事件上报：本模块维护一个小的**待发事件环形队列**（0x13 帧），
 *   CommTask 定时排空即可；事件码见协议 §3.3。
 */

#include <stdint.h>
#include "protocol.h"

/* 待发事件队列容量（够用且省 RAM：一条事件 5 字节） */
#define PATROL_EVENT_QUEUE 8

/* 安全停车原因（位标志，供上报/调试） */
#define PATROL_STOP_NONE      0x00u
#define PATROL_STOP_LINKLOST  0x01u   /* 500 ms 失联 */
#define PATROL_STOP_ESTOP     0x02u   /* 收到 0x2F 急停 */
#define PATROL_STOP_HWFAULT   0x04u   /* 硬件故障（低电压/倾角/跌落）*/

/* 状态机内部错误标志（0x12 的"异常标志"字节） */
#define PATROL_FLT_NONE       0x00u
#define PATROL_FLT_GAS        0x01u   /* 气体超阈值 */
#define PATROL_FLT_LINK       0x02u   /* 通信失联 */
#define PATROL_FLT_HW         0x04u   /* 硬件异常 */
#define PATROL_FLT_SENSOR     0x08u   /* 传感器失效（DHT 多次读失败）*/

typedef struct
{
	uint32_t warmupMs;         /* 预热时长（默认 3 min；MQ 老化是"小时级"，那是硬件老化，不占用此计时）*/
	uint32_t dwellMs;          /* 异常研判驻留时长（默认 5 s，期间 5 Hz 加密采样）*/
	uint32_t linkTimeoutMs;    /* 失联判定（协议规定 500 ms）*/
	uint32_t taskTimeoutMs;    /* 任务总超时（默认 30 min，超时→任务失败）*/
	uint8_t  totalWaypoints;   /* 巡检点总数（默认 4）*/
} PatrolCfg;

typedef struct
{
	uint32_t nowMs;
	uint8_t  gasAlarm;         /* 任一气体通道"已确认超阈值"*/
	uint8_t  gasAnyOver;       /* 任一通道"连续超阈值中"（用于提前切 5 Hz）*/
	uint8_t  dhtValid;         /* 本周期 DHT 读数是否有效 */
	uint8_t  hwFault;          /* 低电压 / 倾角超限 / 被拿起等（由控制侧给）*/
	uint8_t  calibrating;      /* R0 标定尚未完成 */
	uint8_t  waypointReached;  /* 本周期是否抵达当前巡检点 */
} PatrolInput;

typedef struct
{
	uint8_t  code;
	uint32_t tsMs;
} PatrolEvent;

typedef struct
{
	PatrolCfg cfg;

	uint8_t  phase;            /* PROTO_PHASE_* */
	uint8_t  taskId;           /* PROTO_TASK_* */
	uint8_t  fault;            /* PATROL_FLT_* 位 */
	uint8_t  waypoint;         /* 当前巡检点序号（0 基）*/
	uint8_t  doneWaypoints;    /* 已完成巡检点数 */
	uint8_t  progress;         /* 0~100 */

	/* 计时 */
	uint32_t tEnterMs;         /* 进入当前阶段的时刻 */
	uint32_t tTaskStartMs;
	uint32_t tLastDownlinkMs;  /* 最近一次收到下行帧的时刻 */
	uint8_t  linkSeen;         /* 是否曾收到过下行帧（未收到过不判失联）*/

	/* 安全停车 */
	uint8_t  safetyStop;
	uint8_t  stopReason;
	uint8_t  stopLatched;      /* 1 = 已锁存，需显式解除 */

	/* 事件队列 */
	PatrolEvent queue[PATROL_EVENT_QUEUE];
	uint8_t  qHead, qTail, qCount;

	/* 上次的气体报警电平，用于取上升沿 */
	uint8_t  lastGasAlarm;
} PatrolState;

void Patrol_Init(PatrolState *st, const PatrolCfg *cfg);

/* 健康检查：任何一次"收到并通过 CRC 的下行帧"都要调它（失联检测的依据） */
void Patrol_NoteDownlink(PatrolState *st, uint32_t nowMs);

/* 处理下行指令（已解帧的数据段）。返回 1=已处理 */
int  Patrol_Command(PatrolState *st, uint8_t cmd, const uint8_t *data, uint16_t len,
                    uint32_t nowMs);

/* 周期调用（建议 100 ms）。返回本次新产生的阶段迁移/事件数（便于日志） */
int  Patrol_Tick(PatrolState *st, const PatrolInput *in);

/* 取一条待发事件（0x13）。返回 1=取到 */
int  Patrol_PopEvent(PatrolState *st, PatrolEvent *out);

/* 组 0x12 任务状态载荷 */
void Patrol_FillTaskState(const PatrolState *st, ProtoTaskState *out);

/* 电机是否应被强制关闭（安全停车 / 急停 / 故障） */
int  Patrol_MotorsDisabled(const PatrolState *st);

/* 供任务层查询：本轮应使用哪个采样档位（配合 SensorReport_DecideGear 使用） */
int  Patrol_WantsFastSampling(const PatrolState *st);

#endif /* __PATROL_H */
