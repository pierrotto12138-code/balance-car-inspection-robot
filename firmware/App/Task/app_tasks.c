#include "app_tasks.h"

#ifdef USE_FREERTOS

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"
#include "app_api.h"
#include "sys.h"
#include "protocol.h"
#include "crc16.h"
#include "comm_link.h"
#include "gas.h"
#include "dht11.h"
#include "dht11_hw.h"
#include "sensor_adc.h"
#include "sensor_report.h"
#include "patrol.h"
#include "param_store.h"
#include "buzzer.h"
#include "show.h"

/* ============================================================================
 * 厂商全局量（定义在 main.c / control.c）—— 只读，用于上报
 * ★ 这里只 **声明**，不定义；控制逻辑不受影响，也不改控制时序。
 * ==========================================================================*/
extern float Angle_Balance, Gyro_Balance, Gyro_Turn;
extern int   Encoder_Left, Encoder_Right;
extern int   Moto1, Moto2;
extern int   Voltage;
extern u32   Distance;
extern u8    Flag_Show, Flag_Stop;
extern float Balance_Kp, Balance_Kd, Velocity_Kp, Velocity_Ki, Zhongzhi;

/* ============================================================================
 * 运行标志与任务心跳
 * ==========================================================================*/
static volatile unsigned long s_ticks[APP_TASK_COUNT];
static volatile int s_schedRunning = 0;

unsigned long App_TaskTicks(int idx)
{
	if (idx < 0 || idx >= APP_TASK_COUNT)
	{
		return 0;
	}
	return s_ticks[idx];
}

int App_SchedulerRunning(void)
{
	return s_schedRunning;
}

void App_DelayMs(unsigned short nms)
{
	if (s_schedRunning)
	{
		vTaskDelay(pdMS_TO_TICKS(nms));
	}
}

/* ============================================================================
 * 姿态/运动快照（控制中断写、任务读）
 *   说明：对齐的 32 位读写本身是原子的；临界区只是让"一组相关量"尽量取自同一拍。
 *   这些数据**只用于上报/显示**，不参与控制，所以不需要更强的同步。
 * ==========================================================================*/
typedef struct
{
	float    angle, gyro, gyroTurn;
	int32_t  encL, encR;
	int32_t  pwm1, pwm2;
	int32_t  voltage;
	uint32_t dist;
	uint8_t  stopped;
} RawState;

static void state_read(RawState *out)
{
	taskENTER_CRITICAL();
	out->angle    = Angle_Balance;
	out->gyro     = Gyro_Balance;
	out->gyroTurn = Gyro_Turn;
	out->encL     = Encoder_Left;
	out->encR     = Encoder_Right;
	out->pwm1     = Moto1;
	out->pwm2     = Moto2;
	out->voltage  = Voltage;
	out->dist     = Distance;
	out->stopped  = Flag_Stop;
	taskEXIT_CRITICAL();
}

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
static volatile uint8_t s_tuningMode = 0;
static uint32_t       s_lastDhtMs = 0;
static uint32_t       s_lastSampleMs = 0;
static uint32_t       s_calibStartMs = 0;
static uint32_t       s_lastGasTxMs = 0;
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
		(void)Patrol_Command(&s_patrol, f->cmd, f->data, f->len, now);
		break;

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
		RawState st;
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

		state_read(&st);

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

			mot.speedL = (int16_t)((float)st.encL * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
			mot.speedR = (int16_t)((float)st.encR * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
			mot.pwm = (int16_t)((st.pwm1 + st.pwm2) / 2);
			mot.distCm = (uint16_t)st.dist;
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

				mot.speedL = (int16_t)((float)st.encL * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
				mot.speedR = (int16_t)((float)st.encR * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
				mot.pwm = (int16_t)((st.pwm1 + st.pwm2) / 2);
				mot.distCm = (uint16_t)st.dist;
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
			uint32_t gasPeriod = (s_gear == PROTO_GEAR_FAST) ? 200u
			                   : (s_gear == PROTO_GEAR_WARMUP ? 1000u : s_cfg.samplePeriodMs);
			if (gasPeriod < 100u)
			{
				gasPeriod = 100u;
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

		period = (s_gear == PROTO_GEAR_FAST) ? 200u
		       : (s_gear == PROTO_GEAR_WARMUP ? 500u : s_cfg.samplePeriodMs);
		if (period < 50u)
		{
			period = 50u;
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
		s_ticks[APP_TASK_STATE]++;

		memset(&in, 0, sizeof(in));
		in.nowMs = now;
		in.dhtValid = s_dhtValid;
		in.calibrating = (s_calibDone == 0) ? 1 : 0;   /* ★ 由 GasTask 的标定进度决定 */
		in.gasAlarm = (uint8_t)((s_mq2.alarm || s_mq135.alarm) ? 1 : 0);
		in.gasAnyOver = (uint8_t)((s_mq2.overCount > 0 || s_mq135.overCount > 0) ? 1 : 0);
		in.hwFault = (uint8_t)((Voltage > 0 && Voltage < 1110) ? 1 : 0);
		in.waypointReached = 0;      /* 里程到位判据：待实测标定后接入（见 README §五）*/

		(void)Patrol_Tick(&s_patrol, &in);

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

		s_ticks[APP_TASK_UI]++;

		if (Flag_Show == 0)
		{
			APP_Show();
			oled_show();
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

	/* ---- 建任务（栈以"字"为单位；1 字 = 4 B）---- */
	xTaskCreate(comm_task,  "Comm", CFG_STACK_COMM,  0, 3, &h);
	xTaskCreate(gas_task,   "Gas",  CFG_STACK_GAS,   0, 2, &h);
	xTaskCreate(state_task, "Task", CFG_STACK_STATE, 0, 3, &h);
	xTaskCreate(ui_task,    "Ui",   CFG_STACK_UI,    0, 1, &h);

	s_schedRunning = 1;
	vTaskStartScheduler();

	/* 调度器启动失败（通常是堆不够）：LED 常亮，便于调试器定位 */
	for (;;)
	{
		LED = 1;
	}
}

/* ============================================================================
 * 只读 API（供上位机 / 调试工具使用）—— 见 app_api.h 的约定
 * ==========================================================================*/

int App_GetTelemetry(AppTelemetry *out)
{
	RawState st;

	if (out == 0)
	{
		return -1;
	}
	state_read(&st);

	out->pitchX100 = (int16_t)(st.angle * 100.0f);
	out->gyroX100 = (int16_t)(st.gyro * 100.0f);
	out->gyroTurnX100 = (int16_t)(st.gyroTurn * 100.0f);
	out->encLeft = st.encL;
	out->encRight = st.encR;
	out->speedLmmS = (int16_t)((float)st.encL * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
	out->speedRmmS = (int16_t)((float)st.encR * CFG_MM_PER_COUNT / CFG_MS_PER_ENCODER_PERIOD);
	out->pwmLeft = (int16_t)st.pwm1;
	out->pwmRight = (int16_t)st.pwm2;
	out->distCm = (uint16_t)st.dist;
	out->voltageX100 = st.voltage;
	out->stopped = st.stopped;
	return 0;
}

int App_GetGas(AppGasSnapshot *out)
{
	uint8_t ready = 0;

	if (out == 0)
	{
		return -1;
	}
	out->mq2Raw = s_mq2RawAvg;
	out->mq135Raw = s_mq135RawAvg;
	out->mq2PpmX10 = SensorReport_PpmToX10(s_mq2.ppm);
	out->mq135PpmX10 = SensorReport_PpmToX10(s_mq135.ppm);
	out->mq2Ratio = s_mq2.ratioAvg;
	out->mq135Ratio = s_mq135.ratioAvg;
	out->mq2R0 = s_mq2.r0;
	out->mq135R0 = s_mq135.r0;
	out->mq2Alarm = s_mq2.alarm;
	out->mq135Alarm = s_mq135.alarm;
	out->dhtValid = s_dhtValid;
	if (s_dhtValid)
	{
		out->tempX10 = (int16_t)(Dht11_TempC(&s_dht) * 10.0f + 0.5f);
		out->humiX10 = (uint16_t)(Dht11_HumiRh(&s_dht) * 10.0f + 0.5f);
		ready |= PROTO_RDY_DHT11;
		if (s_cfg.tempCompEnable)
		{
			ready |= PROTO_RDY_COMP;
		}
	}
	else
	{
		out->tempX10 = 0;
		out->humiX10 = 0;
	}
	if (s_mq2.r0 > 0.0f)
	{
		ready |= PROTO_RDY_MQ2;
	}
	if (s_mq135.r0 > 0.0f)
	{
		ready |= PROTO_RDY_MQ135;
	}
	out->readyBits = ready;
	out->gear = s_gear;
	return 0;
}

int App_GetPatrol(AppPatrolStatus *out)
{
	if (out == 0)
	{
		return -1;
	}
	out->phase = s_patrol.phase;
	out->taskId = s_patrol.taskId;
	out->progress = s_patrol.progress;
	out->waypoint = s_patrol.waypoint;
	out->fault = s_patrol.fault;
	out->safetyStop = s_patrol.safetyStop;
	out->stopReason = s_patrol.stopReason;
	return 0;
}

int App_GetDiag(AppDiag *out)
{
	uint8_t i;

	if (out == 0)
	{
		return -1;
	}
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
	out->heapFreeBytes = (uint32_t)xPortGetFreeHeapSize();
	out->minEverFreeHeap = (uint32_t)xPortGetMinimumEverFreeHeapSize();
	out->tuningMode = s_tuningMode;
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
