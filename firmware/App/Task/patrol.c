#include "patrol.h"

/*
 * 巡检状态机 —— 见 patrol.h 的设计说明。
 * 本文件是**纯逻辑**：时间来自 in->nowMs，所有硬件信息由调用方填进 PatrolInput，
 * 因此整条任务时间线（含失联、急停、故障、返航）都能在 PC 上离线跑。
 */

/* ------------------------------------------------------------------ 内部工具 */

static void q_push(PatrolState *st, uint8_t code, uint32_t ts)
{
	if (st->qCount >= PATROL_EVENT_QUEUE)
	{
		return;                  /* 队列满：宁可丢事件，也不覆盖/阻塞控制流 */
	}
	st->queue[st->qTail].code = code;
	st->queue[st->qTail].tsMs = ts;
	st->qTail = (uint8_t)((st->qTail + 1u) % PATROL_EVENT_QUEUE);
	st->qCount++;
}

int Patrol_PopEvent(PatrolState *st, PatrolEvent *out)
{
	if (st->qCount == 0)
	{
		return 0;
	}
	*out = st->queue[st->qHead];
	st->qHead = (uint8_t)((st->qHead + 1u) % PATROL_EVENT_QUEUE);
	st->qCount--;
	return 1;
}

static void enter_phase(PatrolState *st, uint8_t phase, uint32_t nowMs)
{
	st->phase = phase;
	st->tEnterMs = nowMs;
}

/* 触发安全停车（锁存）。reason 为 PATROL_STOP_* 位组合 */
static void safety_stop(PatrolState *st, uint8_t reason, uint32_t nowMs)
{
	if (st->safetyStop && (st->stopReason & reason))
	{
		return;                  /* 同一原因不重复报（急停帧可能连发）*/
	}
	st->safetyStop = 1;
	st->stopLatched = 1;
	st->stopReason |= reason;
	enter_phase(st, PROTO_PHASE_FAULT, nowMs);
	q_push(st, PROTO_EVT_TASK_FAIL, nowMs);      /* 事件 0x04：任务失败/超时 */
}

/* ------------------------------------------------------------------ 初始化 */

void Patrol_Init(PatrolState *st, const PatrolCfg *cfg)
{
	uint8_t i;

	st->cfg = *cfg;
	if (st->cfg.warmupMs == 0)
	{
		st->cfg.warmupMs = 180000u;      /* 3 min */
	}
	if (st->cfg.dwellMs == 0)
	{
		st->cfg.dwellMs = 5000u;
	}
	if (st->cfg.linkTimeoutMs == 0)
	{
		st->cfg.linkTimeoutMs = 500u;    /* 协议 §5 */
	}
	if (st->cfg.taskTimeoutMs == 0)
	{
		st->cfg.taskTimeoutMs = 1800000u;
	}
	if (st->cfg.totalWaypoints == 0)
	{
		st->cfg.totalWaypoints = 4;
	}

	st->phase = PROTO_PHASE_INIT;
	st->taskId = PROTO_TASK_IDLE;
	st->fault = PATROL_FLT_NONE;
	st->waypoint = 0;
	st->doneWaypoints = 0;
	st->progress = 0;
	st->tEnterMs = 0;
	st->tTaskStartMs = 0;
	st->tLastDownlinkMs = 0;
	st->linkSeen = 0;
	st->safetyStop = 0;
	st->stopReason = PATROL_STOP_NONE;
	st->stopLatched = 0;
	st->qHead = st->qTail = st->qCount = 0;
	st->lastGasAlarm = 0;
	for (i = 0; i < PATROL_EVENT_QUEUE; i++)
	{
		st->queue[i].code = 0;
		st->queue[i].tsMs = 0;
	}
}

void Patrol_NoteDownlink(PatrolState *st, uint32_t nowMs)
{
	st->tLastDownlinkMs = nowMs;
	st->linkSeen = 1;
}

/* ------------------------------------------------------------------ 下行指令 */

int Patrol_Command(PatrolState *st, uint8_t cmd, const uint8_t *data, uint16_t len,
                   uint32_t nowMs)
{
	switch (cmd)
	{
	case PROTO_DOWN_TASK:                 /* 0x21 任务指示：任务 ID + 参数个数 + 参数 */
		if (len < 2)
		{
			return 0;
		}
		st->taskId = data[0];
		if (st->taskId == PROTO_TASK_PATROL)
		{
			/* 显式指令可以解除锁存的安全停车（人工确认后重新起步） */
			st->safetyStop = 0;
			st->stopLatched = 0;
			st->stopReason = PATROL_STOP_NONE;
			st->fault &= (uint8_t)~PATROL_FLT_LINK;
			q_push(st, PROTO_EVT_STOP_CLEARED, nowMs);   /* 事件 0x09：与 0x08/0x0A 配对 */
			st->waypoint = 0;
			st->doneWaypoints = 0;
			st->progress = 0;
			st->tTaskStartMs = nowMs;
			enter_phase(st, PROTO_PHASE_PATROL, nowMs);
			q_push(st, PROTO_EVT_TASK_START, nowMs);
		}
		else if (st->taskId == PROTO_TASK_RETURN)
		{
			enter_phase(st, PROTO_PHASE_RETURN, nowMs);
		}
		else
		{
			enter_phase(st, PROTO_PHASE_IDLE, nowMs);
		}
		return 1;

	case PROTO_DOWN_MOTION:               /* 0x20 运动控制：使能位=1 也可解除锁存 */
		if (len >= 5 && data[4] != 0 && st->stopLatched &&
			st->stopReason == PATROL_STOP_LINKLOST)
		{
			st->safetyStop = 0;
			st->stopLatched = 0;
			st->stopReason = PATROL_STOP_NONE;
			st->fault &= (uint8_t)~PATROL_FLT_LINK;
			q_push(st, PROTO_EVT_STOP_CLEARED, nowMs);   /* 事件 0x09 */
			enter_phase(st, PROTO_PHASE_IDLE, nowMs);
		}
		return 1;

	case PROTO_DOWN_ESTOP:                /* 0x2F 急停：立即安全停车（且锁存） */
		/* ★ 只在"跃变"时上报事件（0x0A）：操作者连按急停不应刷屏事件日志，
		   这一点由 tests/patrol_test.c 场景 3 的"重复急停不重复上报"断言约束。*/
		if (st->safetyStop == 0 || st->stopReason != PATROL_STOP_ESTOP)
		{
			q_push(st, PROTO_EVT_ESTOP, nowMs);          /* 事件 0x0A：带时间戳的急停记录 */
		}
		safety_stop(st, PATROL_STOP_ESTOP, nowMs);
		return 1;

	case PROTO_DOWN_MODE:                 /* 0x22 控制模式：本状态机只记日志，不改安全策略 */
		return 1;

	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ 电机/采样查询 */

int Patrol_MotorsDisabled(const PatrolState *st)
{
	return (st->safetyStop != 0) || (st->phase == PROTO_PHASE_FAULT);
}

int Patrol_WantsFastSampling(const PatrolState *st)
{
	/* 异常研判驻留期，或处于报警态：5 Hz 加密采样（决策里的"异常驱动自适应采样"）*/
	return (st->phase == PROTO_PHASE_ANALYSIS) ||
			((st->fault & PATROL_FLT_GAS) != 0);
}

/* ------------------------------------------------------------------ 周期推进 */

int Patrol_Tick(PatrolState *st, const PatrolInput *in)
{
	uint32_t now = in->nowMs;
	int n = 0;

	/* ---- 0) 失联检测（协议 §5：500 ms 未收到任何下行帧 → 安全停车）---- */
	if (st->linkSeen && !st->safetyStop)
	{
		if ((uint32_t)(now - st->tLastDownlinkMs) > st->cfg.linkTimeoutMs)
		{
			st->fault |= PATROL_FLT_LINK;
			q_push(st, PROTO_EVT_LINK_LOST, now);    /* 事件 0x08：失联停车（只报一次：
			                                            safety_stop 置位后本分支不再进入）*/
			safety_stop(st, PATROL_STOP_LINKLOST, now);
			n++;
		}
	}

	/* ---- 1) 硬件故障（低电压/倾角/拿起）→ 安全停车 ---- */
	if (in->hwFault && !st->safetyStop)
	{
		st->fault |= PATROL_FLT_HW;
		q_push(st, PROTO_EVT_CRASH_LVBAT, now);    /* 事件 0x05 */
		safety_stop(st, PATROL_STOP_HWFAULT, now);
		n++;
	}

	/* ---- 2) 气体报警上升沿 → 事件 + 进入研判 ---- */
	if (in->gasAlarm && !st->lastGasAlarm)
	{
		st->fault |= PATROL_FLT_GAS;
		q_push(st, PROTO_EVT_GAS_LIMIT, now);      /* 事件 0x06 */
		if (st->phase == PROTO_PHASE_PATROL)
		{
			enter_phase(st, PROTO_PHASE_ANALYSIS, now);   /* 驻留加密采样 */
			n++;
		}
	}
	else if (!in->gasAlarm && !in->gasAnyOver)
	{
		/* 报警解除（去抖/迟滞由 gas 模块负责）：**当拍就清标志**，
			否则上位机会看到"巡航 + 气体异常"这种自相矛盾的一拍，
			采样档位也会多保持一个周期的 5 Hz。 */
		st->fault &= (uint8_t)~PATROL_FLT_GAS;
	}
	st->lastGasAlarm = in->gasAlarm;

	/* ---- 3) 按阶段推进 ---- */
	switch (st->phase)
	{
	case PROTO_PHASE_INIT:
		/* 自检完成 → 预热；自检完成事件只报一次 */
		q_push(st, PROTO_EVT_SELFCHECK, now);      /* 事件 0x01 */
		enter_phase(st, PROTO_PHASE_WARMUP, now);
		n++;
		break;

	case PROTO_PHASE_WARMUP:
		/* 预热完成条件：预热时长到 + R0 标定完成。
			★ DHT 一直读不到**不阻塞任务**：只置"传感器失效"标志并照常进待机
				（补偿会自动不启用，因为 sensor_report 只认有效的 DHT 数据）。*/
		if ((uint32_t)(now - st->tEnterMs) >= st->cfg.warmupMs)
		{
			if (in->calibrating)
			{
				break;                              /* R0 还没标定完，继续等 */
			}
			if (!in->dhtValid)
			{
				st->fault |= PATROL_FLT_SENSOR;
			}
			q_push(st, PROTO_EVT_WARMUP_FAIL, now); /* 事件 0x07：预热完成（或传感器失效）*/
			enter_phase(st, PROTO_PHASE_IDLE, now);
			n++;
		}
		break;

	case PROTO_PHASE_IDLE:
		/* 待机：等待任务指令（由 Patrol_Command 驱动）；任务总超时不计入待机 */
		break;

	case PROTO_PHASE_PATROL:
		/* 抵达巡检点 → 计进度；全部完成 → 返航 */
		if (in->waypointReached)
		{
			if (st->doneWaypoints < 0xFFu)
			{
				st->doneWaypoints++;
			}
			if (st->waypoint + 1u < st->cfg.totalWaypoints)
			{
				st->waypoint++;
			}
			st->progress = (uint8_t)((st->doneWaypoints * 100u) /
										(st->cfg.totalWaypoints ? st->cfg.totalWaypoints : 1u));
			if (st->progress > 100u)
			{
				st->progress = 100u;
			}
			if (st->doneWaypoints >= st->cfg.totalWaypoints)
			{
				enter_phase(st, PROTO_PHASE_RETURN, now);
				n++;
			}
		}
		/* 任务总超时 → 任务失败（安全停车）*/
		if ((uint32_t)(now - st->tTaskStartMs) > st->cfg.taskTimeoutMs)
		{
			st->fault |= PATROL_FLT_HW;
			safety_stop(st, PATROL_STOP_HWFAULT, now);
			n++;
		}
		break;

	case PROTO_PHASE_ANALYSIS:
		/* 驻留加密采样结束：报警已解除 → 回到巡航；否则继续驻留 */
		if (!in->gasAlarm && !in->gasAnyOver)
		{
			if ((uint32_t)(now - st->tEnterMs) >= st->cfg.dwellMs)
			{
				enter_phase(st, PROTO_PHASE_PATROL, now);
				n++;
			}
		}
		else
		{
			st->tEnterMs = now;                 /* 仍在报警：刷新驻留起点，保持 5 Hz */
		}
		break;

	case PROTO_PHASE_RETURN:
		/* 回到起点（用"抵达巡检点"标志表示到位）→ 完成 */
		if (in->waypointReached)
		{
			st->progress = 100;
			st->taskId = PROTO_TASK_IDLE;
			q_push(st, PROTO_EVT_TASK_DONE, now);   /* 事件 0x03 */
			enter_phase(st, PROTO_PHASE_DONE, now);
			n++;
		}
		break;

	case PROTO_PHASE_DONE:
	case PROTO_PHASE_FAULT:
	default:
		break;
	}

	return n;
}

/* ------------------------------------------------------------------ 上报 */

void Patrol_FillTaskState(const PatrolState *st, ProtoTaskState *out)
{
	out->taskId = st->taskId;
	out->phase = st->phase;
	out->progress = st->progress;
	out->waypoint = st->waypoint;
	out->fault = st->fault;
}
