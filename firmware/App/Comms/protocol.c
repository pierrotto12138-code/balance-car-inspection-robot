#include "protocol.h"
#include "crc16.h"

/*
 * 自研通信协议 v1.0 —— 组帧 / 解帧 / 字段打包解包
 *   与上位机 host/src/protocol/codec.cpp 行为**逐位对齐**：
 *     · 长度字段小端、CRC 覆盖「命令字 → 数据段末尾」（偏移 2 起）
 *     · 长度 > PROTO_MAX_DATA_LEN 时整帧作废并重新同步
 *     · 校验失败丢弃整帧，但**当前字节若为 0xAA 则作为下一帧帧头候选**（避免漏帧）
 *     · 帧头容错 `AA AA 55`
 *     · 单帧接收超过 PROTO_RX_TIMEOUT_MS 未完成 → 复位状态机
 */

/* ------------------------------------------------------------------ 小端读写 */

void Proto_PutU16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void Proto_PutI16(uint8_t *p, int16_t v)
{
	Proto_PutU16(p, (uint16_t)v);
}

void Proto_PutU32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* float32 按 IEEE-754 位型以小端存放（0x24 参数字段，协议 §5.1 的唯一例外） */
void Proto_PutF32(uint8_t *p, float v)
{
	union { float f; uint32_t u; } cvt;
	cvt.f = v;
	Proto_PutU32(p, cvt.u);
}

uint16_t Proto_GetU16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int16_t Proto_GetI16(const uint8_t *p)
{
	return (int16_t)Proto_GetU16(p);
}

uint32_t Proto_GetU32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

float Proto_GetF32(const uint8_t *p)
{
	union { float f; uint32_t u; } cvt;
	cvt.u = Proto_GetU32(p);
	return cvt.f;
}

/* ------------------------------------------------------------------ 组帧 */

uint16_t Proto_Encode(uint8_t cmd, const uint8_t *data, uint16_t len,
                      uint8_t *out, uint16_t outCap)
{
	uint16_t crc;
	uint16_t i;

	if (out == 0 || len > PROTO_MAX_DATA_LEN)
	{
		return 0;
	}
	if ((uint16_t)(PROTO_OVERHEAD + len) > outCap)
	{
		return 0;
	}
	if (len > 0 && data == 0)
	{
		return 0;
	}

	/*
	 * 帧布局（协议 §二）：[0]=0xAA [1]=0x55 [2]=cmd [3..4]=N(小端)
	 *                      [5..5+N-1]=数据段 [5+N..6+N]=CRC(小端)
	 * 总长 = 7 + N
	 */
	out[0] = PROTO_HEADER0;
	out[1] = PROTO_HEADER1;
	out[2] = cmd;
	Proto_PutU16(&out[3], len);
	for (i = 0; i < len; i++)
	{
		out[5 + i] = data[i];
	}
	/* CRC 覆盖 out[2] 起的 3+N 字节（命令字 + 长度 + 数据段） */
	crc = crc16_modbus(&out[2], (uint16_t)(3 + len));
	Proto_PutU16(&out[5 + len], crc);
	return (uint16_t)(PROTO_OVERHEAD + len);
}

/* ------------------------------------------------------------------ 解帧 */

void Proto_Init(ProtoDecoder *dec)
{
	if (dec == 0)
	{
		return;
	}
	dec->state = PROTO_ST_HEADER0;
	dec->cmd = 0;
	dec->need = 0;
	dec->got = 0;
	dec->crcGot = 0;
	dec->lastMs = 0;
	dec->framesOk = 0;
	dec->framesDropped = 0;
}

/* 复位到"找帧头"状态；不计入丢弃数（调用方按语义决定是否计数） */
static void proto_reset(ProtoDecoder *dec)
{
	dec->state = PROTO_ST_HEADER0;
	dec->cmd = 0;
	dec->need = 0;
	dec->got = 0;
	dec->crcGot = 0;
}

static void proto_deliver(ProtoDecoder *dec, ProtoFrameCb cb, void *user)
{
	if (cb != 0)
	{
		ProtoFrame f;
		f.cmd = dec->cmd;
		f.data = dec->buf;
		f.len = dec->need;
		cb(&f, user);
	}
	dec->framesOk++;
	dec->state = PROTO_ST_HEADER0;
	dec->got = 0;
}

uint16_t Proto_Feed(ProtoDecoder *dec, const uint8_t *data, uint16_t len,
                    uint32_t nowMs, ProtoFrameCb cb, void *user)
{
	uint16_t i;
	uint16_t produced = 0;

	if (dec == 0 || (data == 0 && len > 0))
	{
		return 0;
	}

	for (i = 0; i < len; i++)
	{
		uint8_t b = data[i];

		/* 单帧接收超时（§2.2）：只要不是在找帧头，超时就复位状态机 */
		if (nowMs != 0 && dec->lastMs != 0 && dec->state != PROTO_ST_HEADER0)
		{
			if ((uint32_t)(nowMs - dec->lastMs) > PROTO_RX_TIMEOUT_MS)
			{
				dec->framesDropped++;
				proto_reset(dec);
			}
		}
		if (nowMs != 0)
		{
			dec->lastMs = nowMs;
		}

		switch (dec->state)
		{
		case PROTO_ST_HEADER0:
			if (b == PROTO_HEADER0)
			{
				dec->state = PROTO_ST_HEADER1;
			}
			break;

		case PROTO_ST_HEADER1:
			if (b == PROTO_HEADER1)
			{
				dec->state = PROTO_ST_CMD;
			}
			else if (b == PROTO_HEADER0)
			{
				/* 连续 0xAA：兼容 AA AA 55 */
				dec->state = PROTO_ST_HEADER1;
			}
			else
			{
				dec->state = PROTO_ST_HEADER0;
			}
			break;

		case PROTO_ST_CMD:
			dec->cmd = b;
			dec->state = PROTO_ST_LENLO;
			break;

		case PROTO_ST_LENLO:
			dec->need = b;
			dec->state = PROTO_ST_LENHI;
			break;

		case PROTO_ST_LENHI:
			dec->need = (uint16_t)(dec->need | ((uint16_t)b << 8));
			dec->got = 0;
			if (dec->need > PROTO_MAX_DATA_LEN)
			{
				/* 长度异常：整帧作废，重新同步 */
				dec->framesDropped++;
				proto_reset(dec);
			}
			else if (dec->need == 0)
			{
				dec->state = PROTO_ST_CRCLO;
			}
			else
			{
				dec->state = PROTO_ST_DATA;
			}
			break;

		case PROTO_ST_DATA:
			dec->buf[dec->got++] = b;
			if (dec->got == dec->need)
			{
				dec->state = PROTO_ST_CRCLO;
			}
			break;

		case PROTO_ST_CRCLO:
			dec->crcGot = b;
			dec->state = PROTO_ST_CRCHI;
			break;

		case PROTO_ST_CRCHI:
		{
			uint16_t want;
			uint8_t head[3];

			dec->crcGot = (uint16_t)(dec->crcGot | ((uint16_t)b << 8));

			/* 重算：命令字 + 长度(2) + 数据段 —— 用**增量 CRC**，
			   不需要为拼接再开一块缓冲区（RAM 省下来了，且没有 static 状态）。 */
			head[0] = dec->cmd;
			head[1] = (uint8_t)(dec->need & 0xFF);
			head[2] = (uint8_t)((dec->need >> 8) & 0xFF);
			want = crc16_modbus_update(0xFFFF, head, 3);
			if (dec->need > 0)
			{
				want = crc16_modbus_update(want, dec->buf, dec->need);
			}

			if (dec->crcGot == want)
			{
				proto_deliver(dec, cb, user);
				produced++;
			}
			else
			{
				dec->framesDropped++;
				/* 该字节本身可能是下一帧帧头，避免漏帧 */
				dec->state = (b == PROTO_HEADER0) ? PROTO_ST_HEADER1 : PROTO_ST_HEADER0;
				dec->got = 0;
			}
			break;
		}

		default:
			proto_reset(dec);
			break;
		}
	}
	return produced;
}

/* ------------------------------------------------------------------ 0x10 姿态 */

uint16_t Proto_Pack_Pose(const ProtoPose *s, uint8_t *out)
{
	Proto_PutI16(&out[0], s->pitch);
	Proto_PutI16(&out[2], s->roll);
	Proto_PutI16(&out[4], s->yaw);
	Proto_PutI16(&out[6], s->gx);
	Proto_PutI16(&out[8], s->gy);
	Proto_PutI16(&out[10], s->gz);
	return 12;
}

int Proto_Unpack_Pose(const uint8_t *in, uint16_t len, ProtoPose *s)
{
	if (len != 12)
	{
		return -1;
	}
	s->pitch = Proto_GetI16(&in[0]);
	s->roll = Proto_GetI16(&in[2]);
	s->yaw = Proto_GetI16(&in[4]);
	s->gx = Proto_GetI16(&in[6]);
	s->gy = Proto_GetI16(&in[8]);
	s->gz = Proto_GetI16(&in[10]);
	return 0;
}

/* ------------------------------------------------------------------ 0x11 运动状态 */

uint16_t Proto_Pack_Motion(const ProtoMotion *s, uint8_t *out)
{
	Proto_PutI16(&out[0], s->speedL);
	Proto_PutI16(&out[2], s->speedR);
	Proto_PutI16(&out[4], s->pwm);
	Proto_PutU16(&out[6], s->distCm);
	out[8] = s->avoidBits;
	return 9;
}

int Proto_Unpack_Motion(const uint8_t *in, uint16_t len, ProtoMotion *s)
{
	if (len != 9)
	{
		return -1;
	}
	s->speedL = Proto_GetI16(&in[0]);
	s->speedR = Proto_GetI16(&in[2]);
	s->pwm = Proto_GetI16(&in[4]);
	s->distCm = Proto_GetU16(&in[6]);
	s->avoidBits = in[8];
	return 0;
}

/* ------------------------------------------------------------------ 0x12 任务状态 */

uint16_t Proto_Pack_TaskState(const ProtoTaskState *s, uint8_t *out)
{
	out[0] = s->taskId;
	out[1] = s->phase;
	out[2] = s->progress;
	out[3] = s->waypoint;
	out[4] = s->fault;
	return 5;
}

int Proto_Unpack_TaskState(const uint8_t *in, uint16_t len, ProtoTaskState *s)
{
	if (len != 5)
	{
		return -1;
	}
	s->taskId = in[0];
	s->phase = in[1];
	s->progress = in[2];
	s->waypoint = in[3];
	s->fault = in[4];
	return 0;
}

/* ------------------------------------------------------------------ 0x13 事件 */

uint16_t Proto_Pack_Event(uint8_t code, uint32_t timestampMs, uint8_t *out)
{
	out[0] = code;
	Proto_PutU32(&out[1], timestampMs);
	return 5;
}

int Proto_Unpack_Event(const uint8_t *in, uint16_t len, uint8_t *code, uint32_t *ts)
{
	if (len != 5)
	{
		return -1;
	}
	*code = in[0];
	*ts = Proto_GetU32(&in[1]);
	return 0;
}

/* ------------------------------------------------------------------ 0x14 环境与气体 */

uint16_t Proto_Pack_EnvGas(const ProtoEnvGas *s, uint8_t *out)
{
	Proto_PutU16(&out[0], s->mq2Raw);
	Proto_PutU16(&out[2], s->mq135Raw);
	Proto_PutU16(&out[4], s->mq2PpmX10);
	Proto_PutU16(&out[6], s->mq135PpmX10);
	Proto_PutI16(&out[8], s->tempX10);
	Proto_PutU16(&out[10], s->humiX10);
	out[12] = s->readyBits;
	out[13] = s->gear;
	out[14] = 0;   /* 保留 */
	out[15] = 0;   /* 保留 */
	return 16;
}

int Proto_Unpack_EnvGas(const uint8_t *in, uint16_t len, ProtoEnvGas *s)
{
	if (len != 16)
	{
		return -1;
	}
	s->mq2Raw = Proto_GetU16(&in[0]);
	s->mq135Raw = Proto_GetU16(&in[2]);
	s->mq2PpmX10 = Proto_GetU16(&in[4]);
	s->mq135PpmX10 = Proto_GetU16(&in[6]);
	s->tempX10 = Proto_GetI16(&in[8]);
	s->humiX10 = Proto_GetU16(&in[10]);
	s->readyBits = in[12];
	s->gear = in[13];
	return 0;
}

/* ------------------------------------------------------------------ 0x20 运动控制 */

uint16_t Proto_Pack_MotionCmd(const ProtoMotionCmd *s, uint8_t *out)
{
	Proto_PutI16(&out[0], s->linMmS);
	Proto_PutI16(&out[2], s->angDpsX10);
	out[4] = s->enable;
	return 5;
}

int Proto_Unpack_MotionCmd(const uint8_t *in, uint16_t len, ProtoMotionCmd *s)
{
	if (len != 5)
	{
		return -1;
	}
	s->linMmS = Proto_GetI16(&in[0]);
	s->angDpsX10 = Proto_GetI16(&in[2]);
	s->enable = in[4];
	return 0;
}

/* ------------------------------------------------------------------ 0x23 巡检参数 */

uint16_t Proto_Pack_SurveyCfg(const ProtoSurveyCfg *s, uint8_t *out)
{
	Proto_PutU16(&out[0], s->samplePeriodMs);
	Proto_PutU16(&out[2], s->mq2ThreshPpmX10);
	Proto_PutU16(&out[4], s->mq135ThreshPpmX10);
	out[6] = s->adaptiveSample;
	out[7] = s->tempCompEnable;
	out[8] = s->confirmFrames;
	out[9] = 0;
	out[10] = 0;
	out[11] = 0;
	return 12;
}

int Proto_Unpack_SurveyCfg(const uint8_t *in, uint16_t len, ProtoSurveyCfg *s)
{
	if (len != 12)
	{
		return -1;
	}
	s->samplePeriodMs = Proto_GetU16(&in[0]);
	s->mq2ThreshPpmX10 = Proto_GetU16(&in[2]);
	s->mq135ThreshPpmX10 = Proto_GetU16(&in[4]);
	s->adaptiveSample = in[6];
	s->tempCompEnable = in[7];
	s->confirmFrames = in[8];
	return 0;
}

/* ------------------------------------------------------------------ 0x24 PID 调参 */

uint16_t Proto_Pack_PidTune(const ProtoPidTune *s, uint8_t *out)
{
	out[0] = s->paramId;
	out[1] = s->reserved;
	Proto_PutF32(&out[2], s->value);
	return 6;
}

int Proto_Unpack_PidTune(const uint8_t *in, uint16_t len, ProtoPidTune *s)
{
	if (len != 6)
	{
		return -1;
	}
	s->paramId = in[0];
	s->reserved = in[1];
	s->value = Proto_GetF32(&in[2]);
	return 0;
}
