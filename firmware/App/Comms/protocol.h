#ifndef APP_COMMS_PROTOCOL_H
#define APP_COMMS_PROTOCOL_H

/*
 * 自研通信协议 v1.0（机器人 ↔ 上位机）
 *   唯一依据：docs/protocol/communication_protocol.md
 *   交叉验证：docs/protocol/test_vectors.json / .tsv（决策 36）
 *   上位机对端：host/src/protocol/{frame.h,codec.cpp}（字段逐一对齐）
 *
 * ★ 只依赖 <stdint.h>，可在 PC 上原生编译做单元测试（见 firmware/tests/proto_test.c）。
 */

#include <stdint.h>

/* ---------- 帧结构（§二） ---------- */
#define PROTO_HEADER0        0xAAu
#define PROTO_HEADER1        0x55u
#define PROTO_OVERHEAD       7u      /* 帧头2 + 命令字1 + 长度2 + CRC2 */
#define PROTO_MAX_DATA_LEN   256u    /* 数据段上限（与上位机 kMaxDataLen 一致，防异常长度） */
#define PROTO_RX_TIMEOUT_MS  100u    /* 单帧接收超时（§2.2）：超时复位状态机 */

/* ---------- 上行命令字（机器人 → 监控，§三） ---------- */
#define PROTO_UP_POSE        0x10u   /* 姿态数据   N=12 */
#define PROTO_UP_MOTION      0x11u   /* 运动状态   N=9  */
#define PROTO_UP_TASK        0x12u   /* 任务状态   N=5  */
#define PROTO_UP_EVENT       0x13u   /* 事件告警   N=5  */
#define PROTO_UP_ENV_GAS     0x14u   /* 环境与气体 N=16 */
/* ★ v1.2 新增：诊断帧（仅供调试/联调，不属于任务书功能）。
 * 为什么要它：OLED 是现场唯一的可视化输出，而"屏幕画成什么样"在协议里原本**完全看不到** ——
 * 联调时只能靠人眼看屏、靠猜。有了这一帧，工具端可以客观判定：
 *   · 各任务还在不在跑（tick 计数在涨 = 任务存活）
 *   · OLED 到底有没有被写（写字节计数在涨 = 有在刷屏；不涨 = 根本没画）
 *   · 当前在哪一页（`Flag_Page`）、`Flag_Show`/`Way_Angle` 是什么
 *   · 电池电压（原本任何上行帧都没有它）、剩余堆
 * 它**不改动任何已有帧的长度与语义**，旧工具不认识 0x15 直接忽略即可（向后兼容）。*/
#define PROTO_UP_INTERNALS   0x16u   /* 三环实况（N=20，50 ms；调参专用，见 §3.5）*/
#define PROTO_UP_DIAG        0x15u   /* 诊断（N=16，1 Hz，见 §3.4） */
#define PROTO_UP_HEARTBEAT   0x1Fu   /* 心跳       N=0  */

/* ---------- 下行命令字（监控 → 机器人，§四） ---------- */
#define PROTO_DOWN_MOTION    0x20u   /* 运动控制 N=5 */
#define PROTO_DOWN_TASK      0x21u   /* 任务指示 N=2+2k */
#define PROTO_DOWN_MODE      0x22u   /* 控制模式 N=1 */
#define PROTO_DOWN_SURVEY    0x23u   /* 巡检参数 N=12 */
#define PROTO_DOWN_PID_TUNE  0x24u   /* PID 调参 N=6 */
#define PROTO_DOWN_ESTOP     0x2Fu   /* 紧急停机 N=0 */

/* ---------- 阶段枚举（0x12 第 2 字节，§3.2） ---------- */
#define PROTO_PHASE_INIT     0x00u
#define PROTO_PHASE_WARMUP   0x01u
#define PROTO_PHASE_IDLE     0x02u
#define PROTO_PHASE_PATROL   0x03u
#define PROTO_PHASE_ANALYSIS 0x04u
#define PROTO_PHASE_RETURN   0x05u
#define PROTO_PHASE_DONE     0x06u
#define PROTO_PHASE_FAULT    0x07u

/* ---------- 事件码（0x13 第 1 字节，§3.3） ---------- */
#define PROTO_EVT_SELFCHECK  0x01u
#define PROTO_EVT_TASK_START 0x02u
#define PROTO_EVT_TASK_DONE  0x03u
#define PROTO_EVT_TASK_FAIL  0x04u
#define PROTO_EVT_CRASH_LVBAT 0x05u
#define PROTO_EVT_GAS_LIMIT  0x06u
#define PROTO_EVT_WARMUP_FAIL 0x07u
/* ---- 安全通道事件（决策 57：0x08~0x0A 专用于"安全停车"，便于上位机做带时间戳的配对）----
 * 为什么单独给码：0x12 是 20 Hz 周期帧，"失联/急停"只体现在它的异常标志与阶段字节里，
 * 上位机必须**对比前后两帧**才能推断出"什么时候停的"。而 0x13 事件帧自带时间戳 ——
 * 这才是监控系统真正需要的记录。三种停车原因分开编码，日志里不会含糊。
 */
#define PROTO_EVT_LINK_LOST     0x08u  /* 通信失联（> linkTimeoutMs 无下行帧）→ 安全停车 */
#define PROTO_EVT_STOP_CLEARED  0x09u  /* 安全停车解除（收到显式使能：0x21 任务 / 0x20 使能）*/
#define PROTO_EVT_ESTOP         0x0Au  /* 收到急停指令（0x2F）→ 安全停车并锁存 */

/* ---------- 任务 ID（§4.3） ---------- */
#define PROTO_TASK_IDLE      0x00u
#define PROTO_TASK_PATROL    0x01u
#define PROTO_TASK_RETURN    0x02u

/* ---------- 采样档位（0x14 偏移 13，§3.1） ---------- */
#define PROTO_GEAR_WARMUP    0u
#define PROTO_GEAR_NORMAL    1u      /* 常态 1 Hz  */
#define PROTO_GEAR_FAST      2u      /* 异常 5 Hz  */

/* ---------- 就绪状态位（0x14 偏移 12，§3.1） ---------- */
#define PROTO_RDY_MQ2        0x01u
#define PROTO_RDY_MQ135      0x02u
#define PROTO_RDY_DHT11      0x04u
#define PROTO_RDY_COMP       0x08u

/* ---------- 控制模式（0x22） ---------- */
#define PROTO_MODE_BALANCE   0u
#define PROTO_MODE_REMOTE    1u
#define PROTO_MODE_AUTO      2u

/* ---------- PID 参数 ID（0x24，§4.2） ---------- */
#define PROTO_PID_BALANCE_KP 0x01u
#define PROTO_PID_BALANCE_KD 0x02u
#define PROTO_PID_VELOCITY_KP 0x03u
#define PROTO_PID_VELOCITY_KI 0x04u
#define PROTO_PID_ZHONGZHI   0x05u
#define PROTO_PID_TURN_KP    0x06u
#define PROTO_PID_TURN_KD    0x07u

/* ============================================================================
 * 小端读写辅助（§5.1：全部小端；float 仅用于 0x24 的参数字段）
 * ==========================================================================*/
void     Proto_PutU16(uint8_t *p, uint16_t v);
void     Proto_PutI16(uint8_t *p, int16_t v);
void     Proto_PutU32(uint8_t *p, uint32_t v);
void     Proto_PutF32(uint8_t *p, float v);
uint16_t Proto_GetU16(const uint8_t *p);
int16_t  Proto_GetI16(const uint8_t *p);
uint32_t Proto_GetU32(const uint8_t *p);
float    Proto_GetF32(const uint8_t *p);

/* ============================================================================
 * 组帧 / 解帧
 * ==========================================================================*/

/* 一帧解析结果：data 指向解码器内部缓冲，**回调返回后失效** */
typedef struct
{
	const uint8_t *data;
	uint16_t       len;
	uint8_t        cmd;
} ProtoFrame;

typedef void (*ProtoFrameCb)(const ProtoFrame *frame, void *user);

typedef enum
{
	PROTO_ST_HEADER0 = 0,
	PROTO_ST_HEADER1,
	PROTO_ST_CMD,
	PROTO_ST_LENLO,
	PROTO_ST_LENHI,
	PROTO_ST_DATA,
	PROTO_ST_CRCLO,
	PROTO_ST_CRCHI
} ProtoState;

typedef struct
{
	ProtoState state;
	uint8_t    cmd;
	uint16_t   need;                 /* 本帧数据段长度 */
	uint16_t   got;                  /* 已收数据段字节数 */
	uint16_t   crcGot;               /* 已收 CRC（低字节先到） */
	uint8_t    buf[PROTO_MAX_DATA_LEN];
	uint32_t   lastMs;               /* 上一字节到达时刻，用于 100 ms 超时 */
	uint32_t   framesOk;
	uint32_t   framesDropped;
} ProtoDecoder;

/* 组帧：out 容量须 ≥ PROTO_OVERHEAD+len；返回帧总长，0 表示参数非法 */
uint16_t Proto_Encode(uint8_t cmd, const uint8_t *data, uint16_t len,
                      uint8_t *out, uint16_t outCap);

void     Proto_Init(ProtoDecoder *dec);

/*
 * 喂入字节流，返回本次解析出的**有效帧个数**（每帧回调一次）。
 * nowMs：当前毫秒时基（由调用方提供 —— 这样超时逻辑可以在 PC 上被测到，
 *        固件里传 HAL 的毫秒计数即可）。传 0 表示不检查超时。
 */
uint16_t Proto_Feed(ProtoDecoder *dec, const uint8_t *data, uint16_t len,
                    uint32_t nowMs, ProtoFrameCb cb, void *user);

/* ============================================================================
 * 常用帧的打包 / 解包（字段顺序严格按协议文档）
 *   打包函数返回写入的字节数；解包返回 0=成功，非 0=长度不符
 * ==========================================================================*/

/* 0x10 姿态：俯仰/横滚/偏航 int16(0.01°) + 三轴角速度 int16(0.01 dps) */
typedef struct { int16_t pitch, roll, yaw; int16_t gx, gy, gz; } ProtoPose;
uint16_t Proto_Pack_Pose(const ProtoPose *s, uint8_t *out);
int      Proto_Unpack_Pose(const uint8_t *in, uint16_t len, ProtoPose *s);

/* 0x11 运动状态（N=9）：左右轮速 int16(mm/s)、PWM int16、超声 uint16(cm)、避障位 uint8 */
typedef struct { int16_t speedL, speedR, pwm; uint16_t distCm; uint8_t avoidBits; } ProtoMotion;
uint16_t Proto_Pack_Motion(const ProtoMotion *s, uint8_t *out);
int      Proto_Unpack_Motion(const uint8_t *in, uint16_t len, ProtoMotion *s);

/* 0x12 任务状态：任务 ID、阶段、完成度%、当前巡检点、异常标志 */
typedef struct { uint8_t taskId, phase, progress, waypoint, fault; } ProtoTaskState;
uint16_t Proto_Pack_TaskState(const ProtoTaskState *s, uint8_t *out);
int      Proto_Unpack_TaskState(const uint8_t *in, uint16_t len, ProtoTaskState *s);

/* 0x13 事件告警：事件码 uint8 + 时间戳 uint32(ms) */
uint16_t Proto_Pack_Event(uint8_t code, uint32_t timestampMs, uint8_t *out);
int      Proto_Unpack_Event(const uint8_t *in, uint16_t len, uint8_t *code, uint32_t *ts);

/* 0x14 环境与气体（N=16）：见协议 §3.1 偏移表 */
typedef struct
{
	uint16_t mq2Raw, mq135Raw;    /* ADC 原始值 0~4095 */
	uint16_t mq2PpmX10, mq135PpmX10; /* ppm×10 */
	int16_t  tempX10;             /* 0.1 ℃ */
	uint16_t humiX10;             /* 0.1 %RH */
	uint8_t  readyBits;           /* PROTO_RDY_* 位 */
	uint8_t  gear;                /* PROTO_GEAR_* */
} ProtoEnvGas;
uint16_t Proto_Pack_EnvGas(const ProtoEnvGas *s, uint8_t *out);
int      Proto_Unpack_EnvGas(const uint8_t *in, uint16_t len, ProtoEnvGas *s);

/* 0x15 诊断（N=16，v1.2 新增）：见 §3.4 偏移表。
   这是**联调专用**的一帧，用来把"看不见的东西"变成可测数据（屏幕内容、任务存活、电压、堆）。*/
typedef struct
{
	uint16_t voltageX100;   /* 电池电压，0.01 V（厂商 Voltage 原单位）*/
	uint8_t  flagPage;      /* 自研页号：0=厂商页，1=环境气体页，2=巡检通信页 */
	uint8_t  flagShow;      /* 厂商 Flag_Show：非 0 表示让位给 DataScope */
	uint8_t  wayAngle;      /* 姿态算法：1=四元数 2=卡尔曼 3=互补 */
	uint8_t  statusBits;    /* bit0 报警中 bit1 安全锁存 bit2 DHT11 有效 bit3 蓝牙链路开 */
	uint16_t heapFreeKb;    /* 剩余堆（KB）*/
	uint32_t oledWrites;    /* OLED 累计写入字节数（不涨 = 屏幕根本没在刷）*/
	uint8_t  ticks[4];      /* 四个任务 tick 的低 8 位（在涨 = 该任务存活）*/
} ProtoDiag;
/* 0x16 三环实况（v1.2 新增）：把"调平衡时真正需要看的量"一次给全。
   解决的问题：原先只能看到"轮子往哪转"这一个末端现象，而三个环各自的贡献、
   以及"算出来的值有没有真的写进寄存器"全都不可见 —— 于是速度环（权重是平衡环
   的 13 倍）一旦符号错，就把平衡环的表现完全掩盖，只能靠"翻一个试试"，必然绕圈。*/
typedef struct
{
	int16_t  balancePwm;    /* balance() 输出 */
	int16_t  velocityPwm;   /* velocity() 输出 */
	int16_t  turnPwm;       /* turn() 输出 */
	int16_t  moto1;         /* 送 Set_Pwm 前（含限幅）*/
	int16_t  moto2;
	uint16_t ccrA;          /* 真实寄存器值（TIM1->CCR4）*/
	uint16_t ccrB;          /* TIM1->CCR1 */
	int16_t  encLeft;       /* 编码器原始计数（不依赖轮径标定）*/
	int16_t  encRight;
	uint8_t  statusBits;    /* bit0 Flag_Stop / bit1 App_MotorDisable / bit2 本拍调了 Set_Pwm */
	uint8_t  reserved;
} ProtoInternals;
uint16_t Proto_Pack_Internals(const ProtoInternals *s, uint8_t *out);
int      Proto_Unpack_Internals(const uint8_t *in, uint16_t len, ProtoInternals *s);
uint16_t Proto_Pack_Diag(const ProtoDiag *s, uint8_t *out);
int      Proto_Unpack_Diag(const uint8_t *in, uint16_t len, ProtoDiag *s);

/* 0x20 运动控制（下行）：线速度 int16(mm/s)、角速度 int16(0.1 dps)、使能 uint8 */
typedef struct { int16_t linMmS, angDpsX10; uint8_t enable; } ProtoMotionCmd;
uint16_t Proto_Pack_MotionCmd(const ProtoMotionCmd *s, uint8_t *out);
int      Proto_Unpack_MotionCmd(const uint8_t *in, uint16_t len, ProtoMotionCmd *s);

/* 0x23 巡检参数（下行，N=12）：见协议 §4.1 偏移表 */
typedef struct
{
	uint16_t samplePeriodMs;      /* 默认 1000 */
	uint16_t mq2ThreshPpmX10;     /* ppm×10 */
	uint16_t mq135ThreshPpmX10;   /* ppm×10 */
	uint8_t  adaptiveSample;      /* 0/1 */
	uint8_t  tempCompEnable;      /* 0/1 */
	uint8_t  confirmFrames;       /* 默认 3 */
} ProtoSurveyCfg;
uint16_t Proto_Pack_SurveyCfg(const ProtoSurveyCfg *s, uint8_t *out);
int      Proto_Unpack_SurveyCfg(const uint8_t *in, uint16_t len, ProtoSurveyCfg *s);

/* 0x24 PID 调参（双向）：参数 ID uint8、保留 uint8、值 float32 */
typedef struct { uint8_t paramId, reserved; float value; } ProtoPidTune;
uint16_t Proto_Pack_PidTune(const ProtoPidTune *s, uint8_t *out);
int      Proto_Unpack_PidTune(const uint8_t *in, uint16_t len, ProtoPidTune *s);

#endif /* APP_COMMS_PROTOCOL_H */
