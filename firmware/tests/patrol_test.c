/*
 * 巡检状态机**原生（PC）单元测试** —— 不需要板子。
 *
 * 状态机的验证方式和"算数值"不同：这里用**脚本化时间线**（喂一串输入 → 断言阶段、
 * 事件、电机禁用状态），把任务书要求的几条链路和三条安全通道逐条走一遍：
 *   ① 完整任务链：初始化 → 预热 → 待机 → 巡航 → 异常研判 → 巡航 → 返航 → 完成
 *   ② 失联安全停车（500 ms）+ **链路恢复不自动起步** + 指令解除
 *   ③ 急停 安全停车 + 锁存
 *   ④ 硬件故障（低电压/倾角）安全停车
 *   ⑤ 任务总超时 → 任务失败
 *   ⑥ 气体报警上升沿：只报一次事件、进入研判、5 Hz 档位
 *
 * 退出码 = 失败条数（0 = 全部通过）。
 */
#include <stdio.h>
#include <string.h>

#include "patrol.h"

static int g_pass = 0, g_fail = 0;

static void ok(int cond, const char *what)
{
	if (cond)
	{
		g_pass++;
	}
	else
	{
		g_fail++;
		printf("  [FAIL] %s\n", what);
	}
}

static const char *phase_name(uint8_t p)
{
	switch (p)
	{
	case PROTO_PHASE_INIT:     return "初始化";
	case PROTO_PHASE_WARMUP:   return "预热";
	case PROTO_PHASE_IDLE:     return "待机";
	case PROTO_PHASE_PATROL:   return "巡航";
	case PROTO_PHASE_ANALYSIS: return "研判";
	case PROTO_PHASE_RETURN:   return "返航";
	case PROTO_PHASE_DONE:     return "完成";
	case PROTO_PHASE_FAULT:    return "故障/停车";
	default:                   return "?";
	}
}

static void expect_phase(PatrolState *st, uint8_t want, const char *what)
{
	if (st->phase != want)
	{
		g_fail++;
		printf("  [FAIL] %s：期望阶段=%s(0x%02X)，实得=%s(0x%02X)\n",
		       what, phase_name(want), want, phase_name(st->phase), st->phase);
	}
	else
	{
		g_pass++;
	}
}

/* 默认输入模板：一切正常 */
static PatrolInput base_input(uint32_t now)
{
	PatrolInput in;
	memset(&in, 0, sizeof(in));
	in.nowMs = now;
	in.dhtValid = 1;
	in.calibrating = 0;
	in.gasAlarm = 0;
	in.gasAnyOver = 0;
	in.hwFault = 0;
	in.waypointReached = 0;
	return in;
}

static void default_cfg(PatrolCfg *c)
{
	c->warmupMs = 1000;         /* 测试里把预热缩到 1 s */
	c->dwellMs = 500;
	c->linkTimeoutMs = 500;     /* 协议规定值，测试也用真值 */
	c->taskTimeoutMs = 10000;
	c->totalWaypoints = 3;
}

/* 用"一切正常"的输入推进一拍（方便写时间线） */
static int tick_at(PatrolState *st, uint32_t now)
{
	PatrolInput in = base_input(now);
	return Patrol_Tick(st, &in);
}

/* 收集事件码，返回条数 */
static int drain_events(PatrolState *st, uint8_t *codes, int cap)
{
	PatrolEvent e;
	int n = 0;
	while (n < cap && Patrol_PopEvent(st, &e))
	{
		codes[n++] = e.code;
	}
	return n;
}

static int has_event(const uint8_t *codes, int n, uint8_t code)
{
	int i;
	for (i = 0; i < n; i++)
	{
		if (codes[i] == code)
		{
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ 场景 1：完整任务链 */
static void scenario_full_mission(void)
{
	PatrolState st;
	PatrolCfg cfg;
	PatrolInput in;
	uint8_t ev[PATROL_EVENT_QUEUE];
	int n;

	printf("[场景 1] 完整任务链：初始化→预热→待机→巡航→研判→巡航→返航→完成\n");
	default_cfg(&cfg);
	Patrol_Init(&st, &cfg);

	in = base_input(0);
	(void)Patrol_Tick(&st, &in);                 /* INIT 一拍后进预热 */
	expect_phase(&st, PROTO_PHASE_WARMUP, "INIT 后应进入预热");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_SELFCHECK), "应上报 0x01 上电自检完成");

	/* 预热时长未到 → 仍在预热 */
	in = base_input(500);
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_WARMUP, "预热时长未到应仍停在预热");

	/* 预热时长到 → 待机，并报 0x07 */
	in = base_input(1100);
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_IDLE, "预热完成后应进入待机");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_WARMUP_FAIL), "应上报 0x07 预热完成");

	/* 待机时电机不应被禁（安全停车未触发），但也不该有任务在跑 */
	ok(Patrol_MotorsDisabled(&st) == 0, "待机时不应处于安全停车");
	ok(st.taskId == PROTO_TASK_IDLE, "待机时任务 ID 应为 0");

	/* 下发 0x21 任务=自主巡航(0x01) */
	{
		uint8_t data[2] = {PROTO_TASK_PATROL, 0};
		ok(Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 1200) == 1, "0x21 应被处理");
	}
	expect_phase(&st, PROTO_PHASE_PATROL, "收到任务指令应进入巡航");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_TASK_START), "应上报 0x02 任务开始");

	/* 抵达巡检点 1、2 */
	in = base_input(2000);
	in.waypointReached = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_PATROL, "巡检点未走完应继续巡航");
	ok(st.doneWaypoints == 1, "已完成巡检点数应为 1");
	ok(st.progress == 33, "完成度应为 33%（1/3）");

	/* 异常：气体报警确认 → 进入研判 + 事件 0x06 + 请求 5 Hz */
	in = base_input(3000);
	in.gasAlarm = 1;
	in.waypointReached = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_ANALYSIS, "气体超阈值应进入异常研判");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_GAS_LIMIT), "应上报 0x06 气体超阈值");
	ok(Patrol_WantsFastSampling(&st) == 1, "研判期应请求 5 Hz 加密采样");
	ok((st.fault & PATROL_FLT_GAS) != 0, "异常标志应置气体位");

	/* 报警保持一段时间 → 仍在研判（持续 5 Hz）*/
	in = base_input(3300);
	in.gasAlarm = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_ANALYSIS, "报警未解除应继续驻留");

	/* 报警解除 + 驻留时长到 → 回到巡航，且不再请求 5 Hz */
	in = base_input(4000);
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_PATROL, "报警解除且驻留结束应回到巡航");
	ok(Patrol_WantsFastSampling(&st) == 0, "报警解除后应恢复常态 1 Hz");
	ok((st.fault & PATROL_FLT_GAS) == 0, "气体异常标志应清除");

	/* 走完剩下巡检点 → 返航 */
	in = base_input(5000);
	in.waypointReached = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_PATROL, "第 2 个点后仍应巡航");
	in = base_input(6000);
	in.waypointReached = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_RETURN, "巡检点走完应进入返航");
	ok(st.progress == 100, "返航时完成度应为 100%");

	/* 到家 → 完成 + 事件 0x03 */
	in = base_input(7000);
	in.waypointReached = 1;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_DONE, "到位后应进入完成");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_TASK_DONE), "应上报 0x03 任务完成");
}

/* ------------------------------------------------------------------ 场景 2：失联安全停车 */
static void scenario_link_loss(void)
{
	PatrolState st;
	PatrolCfg cfg;
	PatrolInput in;
	uint8_t ev[PATROL_EVENT_QUEUE];
	int n;
	uint8_t data[2] = {PROTO_TASK_PATROL, 0};

	printf("[场景 2] 失联安全停车：判据 500 ms + 锁存 + 指令解除\n");
	default_cfg(&cfg);
	Patrol_Init(&st, &cfg);

	/* 从未收到过下行帧时**不判失联**（否则上电即停车，没法用）*/
	in = base_input(100000);
	(void)Patrol_Tick(&st, &in);
	ok(st.safetyStop == 0, "从未收到下行帧时不应判失联");

	/* 先跑起来 */
	(void)tick_at(&st, 0);
	(void)tick_at(&st, 1100);
	Patrol_NoteDownlink(&st, 1100);            /* 收到过下行帧 */
	(void)Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 1150);
	expect_phase(&st, PROTO_PHASE_PATROL, "任务已启动");

	/* 1499 时还在 500 ms 内（1150→1499 = 349 ms）：不停车 */
	in = base_input(1499);
	(void)Patrol_Tick(&st, &in);
	ok(st.safetyStop == 0, "失联未到 500 ms 不应停车");

	/* 1651（1150→1651 = 501 ms）→ 必须停车 */
	in = base_input(1651);
	(void)Patrol_Tick(&st, &in);
	ok(st.safetyStop == 1, "失联超 500 ms 应安全停车");
	expect_phase(&st, PROTO_PHASE_FAULT, "安全停车后阶段应为故障");
	ok(st.stopReason == PATROL_STOP_LINKLOST, "停车原因应记为失联");
	ok(Patrol_MotorsDisabled(&st) == 1, "安全停车后电机必须被禁用");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_TASK_FAIL), "应上报 0x04 任务失败");

	/* 链路恢复：**不自动起步**（安全设计）*/
	Patrol_NoteDownlink(&st, 1700);
	in = base_input(2000);
	(void)Patrol_Tick(&st, &in);
	ok(st.safetyStop == 1, "链路恢复不应自动解除安全停车（必须人工/指令确认）");
	ok(Patrol_MotorsDisabled(&st) == 1, "恢复后电机仍应禁用");

	/* 显式指令（0x20 使能）才解除 */
	{
		uint8_t m[5] = {0, 0, 0, 0, 1};
		ok(Patrol_Command(&st, PROTO_DOWN_MOTION, m, 5, 2100) == 1, "0x20 应被处理");
	}
	ok(st.safetyStop == 0 && Patrol_MotorsDisabled(&st) == 0, "显式使能后应解除安全停车");

	/* 重新给任务也能解除并再起步 */
	in = base_input(2500);
	(void)Patrol_Tick(&st, &in);               /* 失联已解除，不会再次触发 */
	(void)Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 2600);
	expect_phase(&st, PROTO_PHASE_PATROL, "重新下发任务应再次进入巡航");
}

/* ------------------------------------------------------------------ 场景 3：急停 */
static void scenario_estop(void)
{
	PatrolState st;
	PatrolCfg cfg;
	uint8_t data[2] = {PROTO_TASK_PATROL, 0};
	uint8_t ev[PATROL_EVENT_QUEUE];
	int n;

	printf("[场景 3] 急停 0x2F：立即停车且锁存\n");
	default_cfg(&cfg);
	Patrol_Init(&st, &cfg);
	(void)tick_at(&st, 0);
	(void)tick_at(&st, 1100);
	Patrol_NoteDownlink(&st, 1100);
	(void)Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 1150);

	(void)Patrol_Command(&st, PROTO_DOWN_ESTOP, 0, 0, 1200);
	ok(st.safetyStop == 1, "急停应立即安全停车");
	ok(st.stopReason == PATROL_STOP_ESTOP, "停车原因应记为急停");
	expect_phase(&st, PROTO_PHASE_FAULT, "急停后阶段应为故障");

	/* 连发急停帧不应产生重复事件 */
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_TASK_FAIL), "首次急停应上报任务失败");
	(void)Patrol_Command(&st, PROTO_DOWN_ESTOP, 0, 0, 1300);
	(void)Patrol_Command(&st, PROTO_DOWN_ESTOP, 0, 0, 1400);
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(n == 0, "重复急停不应重复上报事件");

	/* 急停**不能**靠 0x20 使能解除（只有重下任务才能解除）*/
	{
		uint8_t m[5] = {0, 0, 0, 0, 1};
		(void)Patrol_Command(&st, PROTO_DOWN_MOTION, m, 5, 1500);
	}
	ok(st.safetyStop == 1, "急停锁存不应被 0x20 使能解除");
	(void)Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 1600);
	ok(st.safetyStop == 0, "重新下发任务可解除急停锁存（人工确认路径）");
}

/* ------------------------------------------------------------------ 场景 4：硬件故障与超时 */
static void scenario_faults(void)
{
	PatrolState st;
	PatrolCfg cfg;
	PatrolInput in;
	uint8_t ev[PATROL_EVENT_QUEUE];
	int n;
	uint8_t data[2] = {PROTO_TASK_PATROL, 0};

	printf("[场景 4] 硬件故障 / 任务超时 / 传感器失效不阻塞\n");
	default_cfg(&cfg);
	Patrol_Init(&st, &cfg);
	(void)tick_at(&st, 0);

	/* DHT 一直无效：预热时长到后**不阻塞**，进待机并置传感器失效标志 */
	in = base_input(1100);
	in.dhtValid = 0;
	(void)Patrol_Tick(&st, &in);
	expect_phase(&st, PROTO_PHASE_IDLE, "DHT 无效也不应阻塞进入待机");
	ok((st.fault & PATROL_FLT_SENSOR) != 0, "应置传感器失效标志");

	/* 低电压/倾角 → 安全停车 + 事件 0x05 */
	Patrol_NoteDownlink(&st, 1200);
	(void)Patrol_Command(&st, PROTO_DOWN_TASK, data, 2, 1250);
	in = base_input(1300);
	in.hwFault = 1;
	(void)Patrol_Tick(&st, &in);
	ok(st.safetyStop == 1 && st.stopReason == PATROL_STOP_HWFAULT, "硬件故障应安全停车");
	n = drain_events(&st, ev, PATROL_EVENT_QUEUE);
	ok(has_event(ev, n, PROTO_EVT_CRASH_LVBAT), "应上报 0x05 跌落/低电压告警");

	/* 任务总超时 → 任务失败 */
	{
		PatrolState st2;
		PatrolCfg c2;
		PatrolInput i2;
		default_cfg(&c2);
		c2.taskTimeoutMs = 2000;
		Patrol_Init(&st2, &c2);
		(void)tick_at(&st2, 0);
		(void)tick_at(&st2, 1100);
		Patrol_NoteDownlink(&st2, 1100);
		(void)Patrol_Command(&st2, PROTO_DOWN_TASK, data, 2, 1150);
		i2 = base_input(1200);
		(void)Patrol_Tick(&st2, &i2);
		i2 = base_input(3300);                 /* 1150+2000=3150，已超 */
		(void)Patrol_Tick(&st2, &i2);
		ok(st2.safetyStop == 1, "任务总超时应触发任务失败并停车");
		expect_phase(&st2, PROTO_PHASE_FAULT, "超时后阶段应为故障");
	}

	/* 失联检测：每 400 ms 收一次下行帧 → 永不误判 */
	{
		PatrolState st3;
		PatrolCfg c3;
		PatrolInput i3;
		uint32_t t;
		default_cfg(&c3);
		Patrol_Init(&st3, &c3);
		(void)tick_at(&st3, 0);
		(void)tick_at(&st3, 1100);
		for (t = 1100; t < 6000; t += 400)
		{
			Patrol_NoteDownlink(&st3, t);
			i3 = base_input(t);
			(void)Patrol_Tick(&st3, &i3);
		}
		ok(st3.safetyStop == 0, "每 400 ms 有下行帧时不应误判失联");
	}
}

/* ------------------------------------------------------------------ 场景 5：状态迁移表与上报 */
static void scenario_table(void)
{
	PatrolState st;
	PatrolCfg cfg;
	ProtoTaskState ts;
	uint8_t seen[8];
	int i, nSeen = 0;

	printf("[场景 5] 阶段可达性与 0x12 上报\n");

	/* 8 个阶段都应可达（用不同场景把它们凑齐）*/
	{
		uint8_t phases[8] = {PROTO_PHASE_INIT, PROTO_PHASE_WARMUP, PROTO_PHASE_IDLE,
		                     PROTO_PHASE_PATROL, PROTO_PHASE_ANALYSIS, PROTO_PHASE_RETURN,
		                     PROTO_PHASE_DONE, PROTO_PHASE_FAULT};
		for (i = 0; i < 8; i++)
		{
			ok(phases[i] == i, "阶段枚举应与协议 §3.2 一致（顺序 0..7）");
		}
	}
	(void)seen;
	(void)nSeen;

	default_cfg(&cfg);
	Patrol_Init(&st, &cfg);
	(void)tick_at(&st, 0);
	Patrol_FillTaskState(&st, &ts);
	ok(ts.phase == PROTO_PHASE_WARMUP && ts.taskId == PROTO_TASK_IDLE,
	   "0x12 应反映当前阶段与任务 ID");
	ok(ts.fault == PATROL_FLT_NONE, "初始异常标志应为 0");

	/* 电机禁用逻辑：完成态不禁用，故障态禁用 */
	(void)tick_at(&st, 1100);
	ok(Patrol_MotorsDisabled(&st) == 0, "待机态不应禁用电机");
	st.phase = PROTO_PHASE_DONE;
	ok(Patrol_MotorsDisabled(&st) == 0, "完成态不应禁用电机（车已停稳但允许遥控）");
	st.phase = PROTO_PHASE_FAULT;
	ok(Patrol_MotorsDisabled(&st) == 1, "故障态必须禁用电机");

	/* 事件队列容量：写满后不覆盖、不越界 */
	{
		PatrolState st4;
		PatrolEvent e;
		int k;
		default_cfg(&cfg);
		Patrol_Init(&st4, &cfg);
		(void)tick_at(&st4, 0);
		for (k = 0; k < 20; k++)
		{
			(void)Patrol_Command(&st4, PROTO_DOWN_ESTOP, 0, 0, (uint32_t)(100 + k));
		}
		k = 0;
		while (Patrol_PopEvent(&st4, &e))
		{
			k++;
		}
		ok(k <= PATROL_EVENT_QUEUE, "事件队列不得溢出（<= 容量）");
	}
}

int main(void)
{
	printf("=== 巡检状态机原生测试 ===\n\n");
	scenario_full_mission();
	printf("\n");
	scenario_link_loss();
	printf("\n");
	scenario_estop();
	printf("\n");
	scenario_faults();
	printf("\n");
	scenario_table();

	printf("\n=== 结果：通过 %d 条，失败 %d 条 ===\n", g_pass, g_fail);
	return g_fail;
}
