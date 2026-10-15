#include "app_tasks.h"

#ifdef USE_FREERTOS

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"
#include "app_api.h"
#include "protocol.h"
#include "comm_link.h"
#include "gas.h"
#include "dht11.h"
#include "dht11_hw.h"
#include "sensor_adc.h"
#include "sensor_report.h"
#include "patrol.h"
#include "param_store.h"
#include "board_state.h"
#include "buzzer.h"
#include "pages.h"
#include "odom.h"
#include "show.h"
#include "oled.h"      /* g_oledWrites：0x15 诊断帧里上报"OLED 有没有在被写" */

/* ============================================================================
 * 厂商全局量（定义在 main.c / control.c）—— 只读，用于上报
 *
 * ★ 2026-10-13（P2 结构整改）：App 层**只允许通过 `board_state.h` 访问厂商全局量**，
 *   连厂商的类型与位带宏（`u8`、`LED`）也不再直接用 —— 本文件因此**不再 include `sys.h`**：
 *     姿态/运动/电压 → `Board_ReadRaw()`；停止标志 → `Board_WriteVendorStop()`；
 *     板载 LED     → `Board_LedSet()`；PID 增益同步在 `param_store.c`（同样走 board_state）。
 *   这样"同一个全局量/板级细节在 N 个文件里各自出现"的问题被彻底关进一个文件里。
 * ==========================================================================*/

/* ============================================================================
 * 运行标志与任务心跳
 * ==========================================================================*/
static volatile unsigned long s_ticks[APP_TASK_COUNT];
static volatile int s_msHookInstalled = 0;

/* ★ 安全停车"真正落到电机"的通道（决策 47）：TaskTask 写、控制中断读。
   中断据此强制 Flag_Stop=1 —— 这样"500 ms 失联 / 急停 / 硬件故障"才真的会停车。
   （此前 `Patrol_MotorsDisabled()` 的结果没有任何地方使用：锁存只停在状态机里，
     电机照转 —— 这是本轮复查抓到的最严重问题。）*/
volatile uint8_t App_MotorDisable = 0;

unsigned long App_TaskTicks(int idx)
{
	if (idx < 0 || idx >= APP_TASK_COUNT)
	{
		return 0;
	}
	return s_ticks[idx];
}

/* ----------------------------------------------------------------------------
 * 把"让出 CPU 的毫秒延时"注入 System 层（P2 分层整改，2026-10-13）
 *
 * 为什么要有这一层：`System/delay/delay.c` 原先 `extern int App_SchedulerRunning(void)`
 *   反向回调 App 层 —— 底层依赖上层（依赖倒挂）。现在改成本文件注册函数指针，
 *   `System/` 只认 `DelayMsHook` 这个类型，不认识 App（见 docs/design_firmware/编码规范.md §一）。
 * 为什么不需要再加"调度器是否运行"判断：本钩子**只在 `vTaskStartScheduler()` 之前一行**注册，
 *   所以"钩子非 0" ⟺ "调度器已被启动"；启动阶段的 `delay_ms()` 会自动走忙等（钩子仍为 0）。
 * --------------------------------------------------------------------------*/
static void app_delay_ms(uint16_t nms)
{
	vTaskDelay(pdMS_TO_TICKS(nms));
}

int App_DelayHookInstalled(void)
{
	return s_msHookInstalled;
}

/* ============================================================================
 * 姿态/运动快照（控制中断写、任务读）
 *
 * ★ P2 结构整改（2026-10-13）：原先这里自己定义 `RawState` 并直接读厂商全局量；
 *   现在统一走 `board_state.h` 的 `Board_ReadRaw()` ——
 *   **App 层碰厂商全局量只允许有这一个出口**（临界区也由那一层内部管理），
 *   以后换板/换厂商只改 `board_state.c`。
 * ==========================================================================*/

/* ============================================================================
 * 跨任务共享状态（都在任务上下文访问）
 * ==========================================================================*/
static GasChannel     s_mq2, s_mq135;
static Dht11Reading   s_dht;
static volatile uint8_t s_dhtValid = 0;
static volatile uint8_t s_calibDone = 0;     /* GasTask 写、TaskTask 读（单字节，良性竞争）*/
static PatrolState    s_patrol;
static ProtoDecoder   s_dec[COMM_LINK_COUNT];  /* 每条链路一个解帧器（状态互不干扰）*/
static ProtoSurveyCfg s_cfg;
static volatile uint8_t s_gear = PROTO_GEAR_WARMUP;
/* 低压"连续确认"计数器（2026-10-13 修正误触发；详见 state_task 里的说明）。
   为什么要连续确认：分压网络受电机噪声影响，单拍抖动不该直接触发安全停车。*/
static uint8_t        s_lowBatCount = 0;
static volatile uint8_t s_tuningMode = 0;
static uint32_t       s_lastDhtMs = 0;
static uint32_t       s_lastSampleMs = 0;
static uint32_t       s_calibStartMs = 0;
static uint32_t       s_lastGasTxMs = 0;
/* ★ 最后一次收到 0x20 运动指令的时刻（用于 500 ms 无指令自动停）*/
static uint32_t       s_lastMotionMs = 0;
static uint16_t       s_mq2RawAvg = 0, s_mq135RawAvg = 0;

/* ----------------------------------------------------------------------------
 * 统一发送辅助：组帧 + 发送
 * --------------------------------------------------------------------------*/
static void send_frame(CommLinkId link, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
	static uint8_t frame[PROTO_OVERHEAD + PROTO_MAX_DATA_LEN];
	uint16_t n = Proto_Encode(cmd, payload, len, frame, sizeof(frame));
	if (n > 0)
	{
		(void)CommLink_Send(link, frame, n);
	}
}

static void send_frame_all(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
	static uint8_t frame[PROTO_OVERHEAD + PROTO_MAX_DATA_LEN];
	uint16_t n = Proto_Encode(cmd, payload, len, frame, sizeof(frame));
	if (n > 0)
	{
		CommLink_SendAll(frame, n);
	}
}

/* ----------------------------------------------------------------------------
 * 回复一条 0x24（用于调参回读/确认）：参数 ID + 保留 + float 值
 * --------------------------------------------------------------------------*/
static void reply_pid(CommLinkId link, uint8_t paramId, float value)
{
	uint8_t payload[6];
	ProtoPidTune t;
	t.paramId = paramId;
	t.reserved = 0;
	t.value = value;
	Proto_Pack_PidTune(&t, payload);
	send_frame(link, PROTO_DOWN_PID_TUNE, payload, 6);
}

/* 把当前 7 个参数逐个回读（协议 §4.2 扩展：paramId=0xFF 请求全量回读）*/
static void dump_all_pid(CommLinkId link)
{
	static const uint8_t ids[PARAM_COUNT] = {
		PARAM_BALANCE_KP, PARAM_BALANCE_KD, PARAM_VELOCITY_KP, PARAM_VELOCITY_KI,
		PARAM_ZHONGZHI, PARAM_TURN_KP, PARAM_TURN_KD
	};
	uint8_t i;
	float v;
	for (i = 0; i < PARAM_COUNT; i++)
	{
		if (ParamStore_GetOne(ids[i], &v) == 0)
		{
			reply_pid(link, ids[i], v);
		}
	}
}

/* ============================================================================
 * 下行帧分派（已通过 CRC 校验）
 * ==========================================================================*/
static void on_frame(const ProtoFrame *f, void *user)
{
	CommLinkId link = (CommLinkId)(uintptr_t)user;
	uint32_t now = CommLink_NowMs();

	/* 任何通过 CRC 的下行帧都算"链路活着"（失联检测依据）*/
	Patrol_NoteDownlink(&s_patrol, now);

	switch (f->cmd)
	{
	case PROTO_DOWN_SURVEY:                  /* 0x23 巡检参数：写入并回 0x12 确认（协议 §4.1）*/
		if (Proto_Unpack_SurveyCfg(f->data, f->len, &s_cfg) == 0)
		{
			uint8_t payload[5];
			ProtoTaskState ts;
			Patrol_FillTaskState(&s_patrol, &ts);
			Proto_Pack_TaskState(&ts, payload);
			send_frame(link, PROTO_UP_TASK, payload, 5);
		}
		break;

	case PROTO_DOWN_PID_TUNE:                /* 0x24 PID 调参（含扩展 ID，协议 §4.2）*/
	{
		ProtoPidTune t;
		if (Proto_Unpack_PidTune(f->data, f->len, &t) != 0)
		{
			break;
		}
		if (t.paramId == PARAM_ID_DUMP_ALL)
		{
			dump_all_pid(link);              /* 全量回读 */
		}
		else if (t.paramId == PARAM_ID_SAVE)
		{
			int rc = App_SavePidParams();    /* 存 Flash */
			reply_pid(link, PARAM_ID_SAVE, (float)rc);
		}
		else if (t.paramId == PARAM_ID_LOAD)
		{
			int rc = App_LoadPidParams();    /* 从 Flash 载入 */
			reply_pid(link, PARAM_ID_LOAD, (float)rc);
		}
		else if (t.paramId == PARAM_ID_TUNE_MODE)
		{
			App_SetTuningMode(t.value != 0.0f);
			reply_pid(link, PARAM_ID_TUNE_MODE, (float)App_GetTuningMode());
		}
		else if (App_SetPidParam(t.paramId, t.value) == 0)
		{
			float eff = 0.0f;
			(void)App_GetPidParam(t.paramId, &eff);
			reply_pid(link, t.paramId, eff);  /* 回读**实际生效值**（协议 §4.2）*/
		}
		break;
	}

	case PROTO_DOWN_TASK:                    /* 0x21 任务指示 */
	case PROTO_DOWN_MODE:                    /* 0x22 控制模式 */
	case PROTO_DOWN_MOTION:                  /* 0x20 运动控制 */
	case PROTO_DOWN_ESTOP:                   /* 0x2F 紧急停机 */
	{
		int accepted = Patrol_Command(&s_patrol, f->cmd, f->data, f->len, now);

		/* ★ 2026-10-13（A1 运动链路）：0x20 的**连续量真正用起来**。
		   原先只用了 data[4] 的使能位、把 linMmS/angDpsX10 **直接丢弃**，
		   而 RTOS 下又没有任何人写 Flag_Qian/Hou/Left/Right ⇒ 上位机发指令车不会走。
		   数据段（N=5）：[0..1] linMmS(int16, mm/s) [2..3] angDpsX10(int16, 0.1dps) [4] enable。
		   实现按厂商原语义（方向标志），阈值以下当作"不走/不转"；enable=0 时立即停。*/
		if (f->cmd == PROTO_DOWN_MOTION && f->len >= 5)
		{
			int16_t lin = (int16_t)((uint16_t)f->data[0] | ((uint16_t)f->data[1] << 8));
			int16_t ang = (int16_t)((uint16_t)f->data[2] | ((uint16_t)f->data[3] << 8));
			uint8_t en  = f->data[4];
			uint8_t dir = 0;

			if (en && accepted)
			{
				if (lin >= (int16_t)CFG_MOTION_LIN_MIN_MMS)
				{
					dir |= 0x01u;                    /* 前进 */
				}
				else if (lin <= -(int16_t)CFG_MOTION_LIN_MIN_MMS)
				{
					dir |= 0x02u;                    /* 后退 */
				}
				if (ang >= (int16_t)CFG_MOTION_ANG_MIN_DPS10)
				{
					dir |= 0x04u;                    /* 左转 */
				}
				else if (ang <= -(int16_t)CFG_MOTION_ANG_MIN_DPS10)
				{
					dir |= 0x08u;                    /* 右转 */
				}
				s_lastMotionMs = now;
			}
			Board_WriteMotion(dir);                  /* en=0 / 未接受 ⇒ dir=0 ⇒ 立即停 */
		}

		/* ★ 决策 41/47：安全停车是**锁存**的 ——
				只有"显式使能"（0x21 任务=自主巡航，或 0x20 使能位=1）才能解除，并且
				解除时要**同时**把电机重新使能，否则车会"任务已恢复但电机还关着"。
				急停（0x2F）相反：立刻停车（不等 100 ms 的任务周期）。 */
		if (f->cmd == PROTO_DOWN_ESTOP)
		{
			App_MotorDisable = 1u;
			Board_WriteVendorStop(1);        /* 权威是 App_MotorDisable，Flag_Stop 只是执行器 */
		}
		else if (accepted && s_patrol.safetyStop == 0 &&
					((f->cmd == PROTO_DOWN_TASK && f->len >= 1 && f->data[0] == PROTO_TASK_PATROL) ||
					(f->cmd == PROTO_DOWN_MOTION && f->len >= 5 && f->data[4] != 0)))
		{
			Board_WriteVendorStop(0);        /* 显式使能：重新允许电机出力 */
		}
		break;
	}

	default:
		break;
	}
}

/* ============================================================================
 * 任务 1：CommTask —— 解帧下行 + 排空事件 + 定时上行
 * ==========================================================================*/
static void comm_task(void *arg)
{
	uint8_t  buf[128];
	uint8_t  payload[PROTO_MAX_DATA_LEN];
	PatrolEvent ev;
	uint32_t now;
	uint8_t  li;

	(void)arg;
	for (;;)
	{
		BoardRawState st;
		ProtoTaskState ts;
		ProtoPose pose;
		ProtoMotion mot;
		uint8_t i;

		now = CommLink_NowMs();
		s_ticks[APP_TASK_COMM]++;

		/* 1) 排空两条链路的环形缓冲 → 各自解帧 */
		for (li = 0; li < (uint8_t)COMM_LINK_COUNT; li++)
		{
			for (;;)
			{
				uint16_t n = CommLink_Read((CommLinkId)li, buf, sizeof(buf));
				if (n == 0)
				{
					break;
				}
				/* 忽略返回值是有意的：解帧统计（成功/丢弃）由解帧器自己维护，
					已在 App_GetDiag().framesOk/framesDropped 里对外暴露 */
				(void)Proto_Feed(&s_dec[li], buf, n, now, on_frame,
									(void *)(uintptr_t)li);
			}
		}

		/* 2) 排空待发事件（0x13，触发即发；两条链路都发，谁连上都能看到）*/
		while (Patrol_PopEvent(&s_patrol, &ev))
		{
			uint8_t data[5];
			Proto_Pack_Event(ev.code, ev.tsMs, data);
			send_frame_all(PROTO_UP_EVENT, data, 5);
		}

		Board_ReadRaw(&st);

		/* 3) 调参观测模式：有线链路额外以 100 Hz 上报姿态与运动（PID 整定用）*/
		if (s_tuningMode)
		{
			pose.pitch = (int16_t)(st.angle * 100.0f);
			pose.roll = 0;
			pose.yaw = 0;
			pose.gx = (int16_t)(st.gyro * 100.0f);
			pose.gy = 0;
			pose.gz = (int16_t)(st.gyroTurn * 100.0f);
			Proto_Pack_Pose(&pose, payload);
			send_frame(COMM_LINK_WIRE, PROTO_UP_POSE, payload, 12);

			mot.speedL = (int16_t)((float)st.encLeft * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
			mot.speedR = (int16_t)((float)st.encRight * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
			mot.pwm = (int16_t)((st.pwmLeft + st.pwmRight) / 2);
			mot.distCm = (uint16_t)st.distance;
			mot.avoidBits = 0;
			Proto_Pack_Motion(&mot, payload);
			send_frame(COMM_LINK_WIRE, PROTO_UP_MOTION, payload, 9);
		}

		/* 4) 常态上行：姿态/运动/任务 20 Hz；心跳 5 Hz（两条链路都发）*/
		{
			static uint32_t lastTelemetryMs = 0;
			static uint32_t lastHeartbeatMs = 0;
			uint32_t telemetryPeriod = s_tuningMode ? CFG_TX_TELEMETRY_TUNE_MS
													: CFG_TX_TELEMETRY_MS;

			if ((uint32_t)(now - lastTelemetryMs) >= telemetryPeriod)
			{
				lastTelemetryMs = now;

				pose.pitch = (int16_t)(st.angle * 100.0f);
				pose.roll = 0;
				pose.yaw = 0;
				pose.gx = (int16_t)(st.gyro * 100.0f);
				pose.gy = 0;
				pose.gz = (int16_t)(st.gyroTurn * 100.0f);
				Proto_Pack_Pose(&pose, payload);
				send_frame_all(PROTO_UP_POSE, payload, 12);

				mot.speedL = (int16_t)((float)st.encLeft * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
				mot.speedR = (int16_t)((float)st.encRight * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
				mot.pwm = (int16_t)((st.pwmLeft + st.pwmRight) / 2);
				mot.distCm = (uint16_t)st.distance;
				mot.avoidBits = 0;
				Proto_Pack_Motion(&mot, payload);
				send_frame_all(PROTO_UP_MOTION, payload, 9);

				Patrol_FillTaskState(&s_patrol, &ts);
				Proto_Pack_TaskState(&ts, payload);
				send_frame_all(PROTO_UP_TASK, payload, 5);
			}

			if ((uint32_t)(now - lastHeartbeatMs) >= CFG_TX_HEARTBEAT_MS)
			{
				lastHeartbeatMs = now;
				send_frame_all(PROTO_UP_HEARTBEAT, 0, 0);
			}
		}

		/* 5) 环境与气体（0x14）：按采样档位（1 Hz / 5 Hz）上报 */
		{
			uint32_t gasPeriod = (s_gear == PROTO_GEAR_FAST) ? CFG_TX_ENV_FAST_MS
								: (s_gear == PROTO_GEAR_WARMUP ? CFG_TX_ENV_WARMUP_MS : s_cfg.samplePeriodMs);
			if (gasPeriod < CFG_TX_ENV_MIN_MS)
			{
				gasPeriod = CFG_TX_ENV_MIN_MS;
			}
			if ((uint32_t)(now - s_lastGasTxMs) >= gasPeriod)
			{
				AppGasSnapshot g;
				ProtoEnvGas env;
				s_lastGasTxMs = now;
				if (App_GetGas(&g) == 0)
				{
					env.mq2Raw = g.mq2Raw;
					env.mq135Raw = g.mq135Raw;
					env.mq2PpmX10 = g.mq2PpmX10;
					env.mq135PpmX10 = g.mq135PpmX10;
					env.tempX10 = g.tempX10;
					env.humiX10 = g.humiX10;
					env.readyBits = g.readyBits;
					env.gear = g.gear;
					Proto_Pack_EnvGas(&env, payload);
					send_frame_all(PROTO_UP_ENV_GAS, payload, 16);
				}

				/* ★ 同一个 1 Hz 节拍发一帧诊断（0x15, v1.2）——
				   把"屏幕有没有在刷、任务还活着没有、电压多少"变成**可远程测量**的数据。
				   它是新增命令，**不改任何已有帧**，老工具忽略即可。*/
				{
					ProtoDiag dg;
					BoardRawState bs2;
					Board_ReadRaw(&bs2);
					dg.voltageX100 = (uint16_t)(bs2.voltage > 0 ? bs2.voltage : 0);
					dg.flagPage = (uint8_t)Flag_Page;
					dg.flagShow = bs2.showFlag;
					dg.wayAngle = (uint8_t)Way_Angle;
					dg.statusBits = (uint8_t)(((s_mq2.alarm || s_mq135.alarm) ? 0x01u : 0u) |
											  (s_patrol.safetyStop ? 0x02u : 0u) |
											  (s_dhtValid ? 0x04u : 0u) |
											  ((CommLink_RxBytes(COMM_LINK_BT) > 0) ? 0x08u : 0u));
					dg.heapFreeKb = (uint16_t)(xPortGetFreeHeapSize() / 1024u);
					dg.oledWrites = g_oledWrites;
					dg.ticks[0] = (uint8_t)(s_ticks[APP_TASK_COMM] & 0xFFu);
					dg.ticks[1] = (uint8_t)(s_ticks[APP_TASK_GAS] & 0xFFu);
					dg.ticks[2] = (uint8_t)(s_ticks[APP_TASK_STATE] & 0xFFu);
					dg.ticks[3] = (uint8_t)(s_ticks[APP_TASK_UI] & 0xFFu);
					Proto_Pack_Diag(&dg, payload);
					send_frame_all(PROTO_UP_DIAG, payload, 16);

					/* ★ 同一个 1 Hz 节拍再发一帧**三环实况**（0x16, v1.2 新增）。
					   调平衡时真正需要看的量：三个环各自的贡献、送 Set_Pwm 前后的值、
					   **真实 CCR**、编码器原始计数，以及"本拍到底调没调 Set_Pwm"。
					   —— 有了它，"往哪转"这类黑盒问题变成"一眼看到是谁在出力、输出有没有被挡"。*/
					{
						ProtoInternals it;
						BoardControlInternals ci;
						Board_ReadControlInternals(&ci);
						it.balancePwm  = ci.balancePwm;
						it.velocityPwm = ci.velocityPwm;
						it.turnPwm     = ci.turnPwm;
						it.moto1       = ci.moto1;
						it.moto2       = ci.moto2;
						it.ccrA        = ci.ccrA;
						it.ccrB        = ci.ccrB;
						it.encLeft     = ci.encLeft;
						it.encRight    = ci.encRight;
						it.statusBits  = (uint8_t)((ci.flagStop ? 0x01u : 0u) |
												   (ci.motorDisable ? 0x02u : 0u) |
												   (ci.pwmApplied ? 0x04u : 0u));
						it.reserved    = CFG_FW_BUILD_ID;   /* 固件版本字节（月<<4|日）*/
						Proto_Pack_Internals(&it, payload);
						send_frame_all(PROTO_UP_INTERNALS, payload, 20);
					}
				}
			}
		}

		(void)i;
		vTaskDelay(pdMS_TO_TICKS(s_tuningMode ? CFG_COMM_TICK_TUNE_MS : CFG_COMM_TICK_MS));
	}
}

/* ============================================================================
 * 任务 2：GasTask —— 双路气体 + 温湿度 + R0 标定（1 Hz 常态 / 5 Hz 异常）
 * ==========================================================================*/
static void gas_task(void *arg)
{
	(void)arg;
	for (;;)
	{
		uint32_t now = CommLink_NowMs();
		uint32_t period;

		s_ticks[APP_TASK_GAS]++;

		period = (s_gear == PROTO_GEAR_FAST) ? CFG_GAS_SAMPLE_FAST_MS
				: (s_gear == PROTO_GEAR_WARMUP ? CFG_GAS_SAMPLE_WARM_MS : s_cfg.samplePeriodMs);
		if (period < CFG_GAS_SAMPLE_MIN_MS)
		{
			period = CFG_GAS_SAMPLE_MIN_MS;
		}

		if ((uint32_t)(now - s_lastSampleMs) >= period)
		{
			float t = 25.0f, h = 50.0f;
			int   compOn = 0;

			s_lastSampleMs = now;

			/* ---- 温湿度：DHT11 最快 1 Hz，整次读取约 20 ms ---- */
			if ((uint32_t)(now - s_lastDhtMs) >= CFG_DHT_PERIOD_MS)
			{
				s_lastDhtMs = now;
				s_dhtValid = (Dht11_Hw_Read(&s_dht) == 0) ? 1 : 0;
				if (s_dhtValid && s_cfg.tempCompEnable)
				{
					t = Dht11_TempC(&s_dht);
					h = Dht11_HumiRh(&s_dht);
					compOn = 1;
				}
			}

			/* ---- 气体 ADC（ADC2，与中断里的 ADC1 分开）---- */
			s_mq2RawAvg = SensorAdc_ReadAvg(SENSOR_ADC_CH_MQ2, CFG_GAS_ADC_AVG);
			s_mq135RawAvg = SensorAdc_ReadAvg(SENSOR_ADC_CH_MQ135, CFG_GAS_ADC_AVG);

			/* ---- R0 标定（★ 修正：标定必须由 GasTask 自己收尾，否则状态机永远出不了预热）----
				收尾条件：累加够 CFG_GAS_CALIB_SAMPLES 次，或预热时长已到（拿现有样本收尾）。
				若一次样本都没有，r0 保持 0 → 通道报"未就绪"，但**不阻塞任务**。 */
			if (!s_calibDone)
			{
				if (s_calibStartMs == 0u)
				{
					s_calibStartMs = now;
				}
				if (s_patrol.phase == PROTO_PHASE_WARMUP)
				{
					Gas_CalibrateAdd(&s_mq2, s_mq2RawAvg);
					Gas_CalibrateAdd(&s_mq135, s_mq135RawAvg);
				}
				if (s_mq2.r0Count >= CFG_GAS_CALIB_SAMPLES ||
					(uint32_t)(now - s_calibStartMs) >= CFG_WARMUP_MS)
				{
					(void)Gas_CalibrateFinish(&s_mq2);
					(void)Gas_CalibrateFinish(&s_mq135);
					s_calibDone = 1;
				}
			}

			(void)Gas_Update(&s_mq2, s_mq2RawAvg, t, h, compOn);
			(void)Gas_Update(&s_mq135, s_mq135RawAvg, t, h, compOn);
		}

		vTaskDelay(pdMS_TO_TICKS(CFG_GAS_POLL_MS));
	}
}

/* ============================================================================
 * 任务 3：TaskTask —— 状态机推进 + 阈值研判 + 档位决策
 * ==========================================================================*/
static void state_task(void *arg)
{
	PatrolInput in;

	(void)arg;
	for (;;)
	{
		uint32_t now = CommLink_NowMs();
		BoardRawState bs;
		s_ticks[APP_TASK_STATE]++;

		Board_ReadRaw(&bs);                            /* 电压走 board_state 出口（P2 结构整改）*/

		memset(&in, 0, sizeof(in));
		in.nowMs = now;
		in.dhtValid = s_dhtValid;
		in.calibrating = (s_calibDone == 0) ? 1 : 0;   /* ★ 由 GasTask 的标定进度决定 */
		in.gasAlarm = (uint8_t)((s_mq2.alarm || s_mq135.alarm) ? 1 : 0);
		in.gasAnyOver = (uint8_t)((s_mq2.overCount > 0 || s_mq135.overCount > 0) ? 1 : 0);

		/* ★ 低压判据修正（2026-10-13，真机联调发现）：
		   原判据是 `voltage > 0 && voltage < 1110` —— 本意是"有电池才判低压"，
		   但**分压网络悬空时会拾到几毫伏的噪声**（读数为 20~300，即 0.2~3 V），
		   于是被当成"电池只有 0.x V，低于 11.1 V" → **误触发硬件故障 → 安全停车锁存** →
		   `Turn_Off()` 恒返回 1 → `Set_Pwm()` 永不被调用 → **电机永远没有输出，车根本不能平衡**。
		   真机上表现为：一上电就 stage=FAULT、fault=0x04，且怎么调都动不了。
		   修正：① 先要求电压达到"电池存在"的门槛（> 6.00 V）才进入低压判定；
		        ② 再要求**连续 N 拍**都低于阈值才确认（抗单次抖动）。
		   为什么门槛取 6.00 V：本车是 3S 锂电（标称 11.1 V、满电 12.6 V、下限 ~9 V），
		   任何"真实电池"都远高于 6 V；而悬空分压的读数远低于它 —— 6 V 是两者之间很宽的安全带。*/
		if (bs.voltage > CFG_BAT_PRESENT_MIN)
		{
			if (bs.voltage < CFG_BAT_LOW_LIMIT)
			{
				if (s_lowBatCount < CFG_BAT_LOW_CONFIRM)
				{
					s_lowBatCount++;
				}
			}
			else
			{
				s_lowBatCount = 0;
			}
		}
		else
		{
			s_lowBatCount = 0;                        /* 没接电池（读数无效）→ 不判低压 */
		}
		in.hwFault = (uint8_t)((s_lowBatCount >= CFG_BAT_LOW_CONFIRM) ? 1 : 0);

		/* 巡检点到位判据 = **编码器里程**（决策 49）。
			红外 J11/J14 是 DNP 未焊，所以不依赖红外；间距在 app_config.h 里配置
			（CFG_WAYPOINT_SPACING_MM = 0 表示暂不启用，此时恒为 0，车按指令推进）。 */
		in.waypointReached = (uint8_t)Odom_WaypointReached();

		(void)Patrol_Tick(&s_patrol, &in);

		/* ★ 决策 47：把"该不该关电机"告诉控制中断（安全停车/故障 → 中断强制 Flag_Stop=1）。
			这是"500 ms 失联安全停车"能否真正停车的唯一通路。 */
		App_MotorDisable = Patrol_MotorsDisabled(&s_patrol) ? 1u : 0u;

		/* ★ 运动指令看门狗（安全必需）：超过超时没收到新的 0x20 → 自动停。
		   没有它的话，"上位机崩了/线断了"时车会按最后一条指令一直跑下去。*/
		if (s_lastMotionMs != 0 && (uint32_t)(now - s_lastMotionMs) > CFG_MOTION_TIMEOUT_MS)
		{
			Board_StopMotion();
			s_lastMotionMs = 0;
		}

		/* 阈值研判（多帧确认 + 90% 迟滞）*/
		if (s_calibDone)
		{
			(void)Gas_UpdateAlarm(&s_mq2, s_cfg.mq2ThreshPpmX10, s_cfg.confirmFrames);
			(void)Gas_UpdateAlarm(&s_mq135, s_cfg.mq135ThreshPpmX10, s_cfg.confirmFrames);
		}

		/* 自适应采样档位（预热 → 常态 1 Hz ↔ 异常 5 Hz）*/
		s_gear = SensorReport_DecideGear(&s_mq2, &s_mq135,
											(s_patrol.phase != PROTO_PHASE_WARMUP) && s_calibDone,
											s_cfg.adaptiveSample);

		vTaskDelay(pdMS_TO_TICKS(CFG_STATE_TICK_MS));
	}
}

/* ============================================================================
 * 任务 4：UiTask —— OLED + 蜂鸣器报警
 * ==========================================================================*/
static void ui_task(void *arg)
{
	(void)arg;
	for (;;)
	{
		int beepOn;
		BoardRawState bs;

		s_ticks[APP_TASK_UI]++;

		/* 厂商 `Flag_Show`：0 = 用 OLED/APP 界面；非 0 = 让位给 DataScope 波形通道。
			★ 走 board_state 出口读，不直接碰厂商全局量（P2 结构整改）。 */
		Board_ReadRaw(&bs);
		if (bs.showFlag == 0)
		{
			/* ★ B 2026-10-13 修正（用户实测"有个第三页像两页重合"）：
			   厂商 `oled_show()` **只覆盖它自己那几个固定坐标的字段、从不整屏清**，
			   而自研页（1/2）用的 x/y 与它不同 → 从自研页切回厂商页时，
			   厂商页没写到的地方**留着上一页的残字**，看起来就是"两页重合"。
			   修法：**只在换页的那一拍清屏**（不是每拍清，否则 10 Hz 清屏会明显闪烁）。
			   清屏放在这里而不是 `Pages_Next()`：后者在**控制中断**里被调用，
			   整屏刷新要 1~2 ms，放中断里会拖坏 5 ms 控制节拍。*/
			static uint8_t s_lastPage = 0xFF;
			if (s_lastPage != Flag_Page)
			{
				OLED_Clear();
				s_lastPage = Flag_Page;
			}
			if (Flag_Page == PAGE_VENDOR)
			{
				/* ★ 2026-10-13 真机联调发现并移除：这里原本还调了厂商的 `APP_Show()`。
				   它会把 `{A..}$`/`{B..}$` 这类**厂商手机 APP 的文本协议**用 printf 发出去，
				   而 printf 的路由是 `Flag_Show==0 → USART2`（**正是我们的蓝牙协议链路**）——
				   于是每隔 100 ms 就往蓝牙链路上灌 ~30 字节 ASCII 文本，
				   把我们的二进制帧流**污染**成夹杂乱码的流（表现为蓝牙链路丢帧/解帧异常）。
				   我们的协议已经取代了厂商那套 APP 文本协议，所以这里只保留 OLED 绘制。*/
				oled_show();          /* 厂商原始界面（页 0）*/
			}
			else
			{
				Pages_Show();         /* 自研页 1/2（USER 按键双击切换）*/
			}
		}

		/* 报警：任一通道确认超阈值，或处于安全停车/故障 → 蜂鸣器响（PC5 低电平）*/
		beepOn = (s_mq2.alarm || s_mq135.alarm || s_patrol.phase == PROTO_PHASE_FAULT);
		Buzzer_Set(beepOn);

		vTaskDelay(pdMS_TO_TICKS(CFG_UI_TICK_MS));
	}
}

/* ============================================================================
 * 启动
 * ==========================================================================*/
void App_Start(void)
{
	PatrolCfg pcfg;
	TaskHandle_t h;
	uint8_t li;

	/* ---- 模块初始化（任务启动前，单线程，无需互斥）---- */
	CommLink_Init();
	for (li = 0; li < (uint8_t)COMM_LINK_COUNT; li++)
	{
		Proto_Init(&s_dec[li]);
	}
	SensorAdc_Init();
	Buzzer_Init();

	/* PID 参数：Flash 里有就载入，没有就保持默认（决策 46）*/
	ParamStore_InitDefaults();
	(void)ParamStore_Load();

	{
		GasChannelCfg cfg2, cfg135;
		Gas_DefaultCfg_MQ2(&cfg2);
		Gas_DefaultCfg_MQ135(&cfg135);
		Gas_InitChannel(&s_mq2, &cfg2);
		Gas_InitChannel(&s_mq135, &cfg135);
	}

	/* 协议 §4.1 的默认巡检参数（可被 0x23 覆盖）*/
	s_cfg.samplePeriodMs = CFG_DEF_SAMPLE_MS;
	s_cfg.mq2ThreshPpmX10 = CFG_DEF_MQ2_THRESH_X10;
	s_cfg.mq135ThreshPpmX10 = CFG_DEF_MQ135_THRESH_X10;
	s_cfg.adaptiveSample = CFG_DEF_ADAPTIVE;
	s_cfg.tempCompEnable = CFG_DEF_TEMP_COMP;
	s_cfg.confirmFrames = CFG_DEF_CONFIRM_FRAMES;

	pcfg.warmupMs = CFG_WARMUP_MS;
	pcfg.dwellMs = CFG_DWELL_MS;
	pcfg.linkTimeoutMs = CFG_LINK_TIMEOUT_MS;
	pcfg.taskTimeoutMs = CFG_TASK_TIMEOUT_MS;
	pcfg.totalWaypoints = CFG_TOTAL_WAYPOINTS;
	Patrol_Init(&s_patrol, &pcfg);

	/* 里程计（巡检点判据）：间距为 0 时等于"不启用"，车按指令推进 */
	Odom_SetScaleUmPerCount((uint32_t)(CFG_MM_PER_COUNT * 1000.0f + 0.5f));
	Odom_Init(CFG_WAYPOINT_SPACING_MM);

	/* ---- 建任务（栈以"字"为单位；1 字 = 4 B）---- */
	xTaskCreate(comm_task,  "Comm", CFG_STACK_COMM,  0, 3, &h);
	xTaskCreate(gas_task,   "Gas",  CFG_STACK_GAS,   0, 2, &h);
	xTaskCreate(state_task, "Task", CFG_STACK_STATE, 0, 3, &h);
	xTaskCreate(ui_task,    "Ui",   CFG_STACK_UI,    0, 1, &h);

	/* ★ P2 分层整改：把"让出 CPU 的延时"注入 System 层，紧接着启动调度器。
		钩子注册 ⟺ 调度器已启动；注册之前的 delay_ms() 仍走忙等，行为与原来一致。 */
	Delay_SetMsHook(app_delay_ms);
	s_msHookInstalled = 1;
	vTaskStartScheduler();

	/* 调度器启动失败（通常是堆不够）：LED 常亮 + 死循环，便于调试器定位 */
	for (;;)
	{
		Board_LedSet(1);
	}
}

/* ============================================================================
 * 只读 API（供上位机 / 调试工具使用）—— 见 app_api.h 的约定
 * ==========================================================================*/

int App_GetTelemetry(AppTelemetry *out)
{
	BoardRawState st;

	if (out == 0)
	{
		return -1;
	}
	Board_ReadRaw(&st);

	out->pitchX100 = (int16_t)(st.angle * 100.0f);
	out->gyroX100 = (int16_t)(st.gyro * 100.0f);
	out->gyroTurnX100 = (int16_t)(st.gyroTurn * 100.0f);
	out->encLeft = st.encLeft;
	out->encRight = st.encRight;
	out->speedLmmS = (int16_t)((float)st.encLeft * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
	out->speedRmmS = (int16_t)((float)st.encRight * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
	out->pwmLeft = (int16_t)st.pwmLeft;
	out->pwmRight = (int16_t)st.pwmRight;
	out->distCm = (uint16_t)st.distance;
	out->voltageX100 = st.voltage;
	out->stopped = st.vendorStop;
	return 0;
}

int App_GetGas(AppGasSnapshot *out)
{
	ProtoEnvGas env;                        /* 用 0x14 载荷组装器当**唯一实现**：
												就绪位与 ppm×10 的算法只有一处（见 sensor_report.c），
												避免"两套实现算同一件事"（2026-10-09 复查发现）*/
	float tempC = 0.0f, humiRh = 0.0f;
	uint8_t valid;
	uint8_t comp;

	if (out == 0)
	{
		return -1;
	}

	/* ★ 兑现 app_api.h 的承诺：getter 自带短临界区 ——
		本函数同时读 GasTask 写的气体通道与 TaskTask 写的档位，必须取一个一致的快照。 */
	taskENTER_CRITICAL();

	valid = s_dhtValid;
	comp = (uint8_t)(s_cfg.tempCompEnable ? 1 : 0);
	if (valid)
	{
		tempC = Dht11_TempC(&s_dht);
		humiRh = Dht11_HumiRh(&s_dht);
	}
	SensorReport_FillEnvGas(&s_mq2, &s_mq135, &s_dht, valid, comp, s_gear, &env);

	out->mq2Raw = s_mq2RawAvg;
	out->mq135Raw = s_mq135RawAvg;
	out->mq2PpmX10 = env.mq2PpmX10;
	out->mq135PpmX10 = env.mq135PpmX10;
	out->mq2Ratio = s_mq2.ratioAvg;
	out->mq135Ratio = s_mq135.ratioAvg;
	out->mq2R0 = s_mq2.r0;
	out->mq135R0 = s_mq135.r0;
	out->mq2Alarm = s_mq2.alarm;
	out->mq135Alarm = s_mq135.alarm;
	out->dhtValid = valid;
	out->tempX10 = valid ? (int16_t)(tempC * 10.0f + 0.5f) : 0;
	out->humiX10 = valid ? (uint16_t)(humiRh * 10.0f + 0.5f) : 0;
	out->readyBits = env.readyBits;
	out->gear = env.gear;

	taskEXIT_CRITICAL();
	return 0;
}

int App_GetPatrol(AppPatrolStatus *out)
{
	if (out == 0)
	{
		return -1;
	}
	/* ★ 短临界区：s_patrol 由 TaskTask 写，这里可能在别的任务里被读（见 app_api.h 的约定） */
	taskENTER_CRITICAL();
	out->phase = s_patrol.phase;
	out->taskId = s_patrol.taskId;
	out->progress = s_patrol.progress;
	out->waypoint = s_patrol.waypoint;
	out->fault = s_patrol.fault;
	out->safetyStop = s_patrol.safetyStop;
	out->stopReason = s_patrol.stopReason;
	taskEXIT_CRITICAL();
	return 0;
}

int App_GetDiag(AppDiag *out)
{
	uint8_t i;

	if (out == 0)
	{
		return -1;
	}
	/* ★ 短临界区：计数由中断在后台自增，任务心跳由各任务写 ——
		取一次一致快照，避免"任务心跳读了半拍"这类误判（排障时这两组数最容易被误信）。 */
	taskENTER_CRITICAL();
	out->rxBytesBt = CommLink_RxBytes(COMM_LINK_BT);
	out->rxBytesWire = CommLink_RxBytes(COMM_LINK_WIRE);
	out->rxDroppedBt = CommLink_RxDropped(COMM_LINK_BT);
	out->rxDroppedWire = CommLink_RxDropped(COMM_LINK_WIRE);
	out->overrunsBt = CommLink_Overruns(COMM_LINK_BT);
	out->overrunsWire = CommLink_Overruns(COMM_LINK_WIRE);
	out->framesOk = s_dec[COMM_LINK_BT].framesOk + s_dec[COMM_LINK_WIRE].framesOk;
	out->framesDropped = s_dec[COMM_LINK_BT].framesDropped + s_dec[COMM_LINK_WIRE].framesDropped;
	for (i = 0; i < APP_TASK_COUNT; i++)
	{
		out->taskTicks[i] = s_ticks[i];
	}
	out->tuningMode = s_tuningMode;
	taskEXIT_CRITICAL();

	/* 堆查询放在临界区外：`xPortGetFreeHeapSize()` 是普通函数调用，没必要在临界区里跑 */
	out->heapFreeBytes = (uint32_t)xPortGetFreeHeapSize();
	out->minEverFreeHeap = (uint32_t)xPortGetMinimumEverFreeHeapSize();
	return 0;
}

int App_SetPidParam(uint8_t paramId, float value)
{
	return ParamStore_SetOne(paramId, value);
}

int App_GetPidParam(uint8_t paramId, float *value)
{
	return ParamStore_GetOne(paramId, value);
}

int App_SavePidParams(void)
{
	return ParamStore_Save();
}

int App_LoadPidParams(void)
{
	return ParamStore_Load();
}

void App_SetTuningMode(int on)
{
	s_tuningMode = (on != 0) ? 1 : 0;
}

int App_GetTuningMode(void)
{
	return (int)s_tuningMode;
}

#endif /* USE_FREERTOS */
