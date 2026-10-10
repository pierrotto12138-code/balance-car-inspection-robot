/*
 * 协议模块**原生（PC）单元测试** —— 不需要板子、不需要编译器交叉工具链。
 *
 * 为什么这么做（AGENTS 铁律 23：独立方法验证）：
 *   协议是 B/C 两端的接口，错一个字节就是"联调三天查不出"的问题。
 *   这套测试同时用了三条独立路径：
 *     ① Python 独立实现生成的向量（docs/protocol/test_vectors.tsv）
 *     ② C 侧真实实现（host/src/protocol）复算 —— tools/fw_prep/host_crosscheck.cpp
 *     ③ **本文件**：固件侧实现，逐条复现向量 + 边界/超时/环形缓冲测试
 *
 * 编译运行（见 tools/fw_prep/run_proto_test.py）：
 *   gcc -std=c11 -Wall -Wextra -I firmware/App/Comms \
 *       firmware/tests/proto_test.c firmware/App/Comms/{protocol,crc16,ringbuf}.c \
 *       -o proto_test.exe && proto_test.exe docs/protocol/test_vectors.tsv
 *
 * 退出码 = 失败条数（0 = 全部通过）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "crc16.h"
#include "ringbuf.h"

static int g_fail = 0;
static int g_pass = 0;
static int g_crc_checked = 0;

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

/* ------------------------------------------------------------------ 工具 */

static int hex2bin(const char *hex, uint8_t *out, int outCap)
{
	int n = 0;
	while (hex[0] != '\0' && hex[1] != '\0')
	{
		unsigned v;
		if (sscanf(hex, "%2x", &v) != 1)
		{
			break;
		}
		if (n >= outCap)
		{
			return -1;
		}
		out[n++] = (uint8_t)v;
		hex += 2;
	}
	return n;
}

static void bin2hex(const uint8_t *b, int n, char *out)
{
	static const char *d = "0123456789abcdef";
	int i;
	for (i = 0; i < n; i++)
	{
		out[2 * i] = d[b[i] >> 4];
		out[2 * i + 1] = d[b[i] & 0x0F];
	}
	out[2 * n] = '\0';
}

/* 去掉行尾的 \r\n */
static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
	{
		s[--n] = '\0';
	}
}

/* 按分隔符切分（原地改串），返回段数 */
static int split(char *s, char sep, char **out, int outCap)
{
	int n = 0;
	char *p = s;
	if (*s == '\0')
	{
		return 0;
	}
	while (p != NULL && n < outCap)
	{
		char *q = strchr(p, sep);
		if (q != NULL)
		{
			*q = '\0';
		}
		out[n++] = p;
		p = (q != NULL) ? q + 1 : NULL;
	}
	return n;
}

/* ------------------------------------------------------------------ 回调收集 */

#define MAX_FRAMES 8
typedef struct
{
	uint8_t cmd[MAX_FRAMES];
	uint8_t data[MAX_FRAMES][PROTO_MAX_DATA_LEN];
	uint16_t len[MAX_FRAMES];
	int count;
} Collected;

static void collect_cb(const ProtoFrame *f, void *user)
{
	Collected *c = (Collected *)user;
	if (c->count < MAX_FRAMES)
	{
		c->cmd[c->count] = f->cmd;
		c->len[c->count] = f->len;
		if (f->len > 0)
		{
			memcpy(c->data[c->count], f->data, f->len);
		}
	}
	c->count++;
}

/* ------------------------------------------------------------------ 分节测试 */

static void test_crc_section(FILE *fp)
{
	char line[1024];
	printf("[1] CRC 用例（来自 test_vectors.tsv）\n");
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char *col[4];
		char buf[600];
		int n;
		chomp(line);
		if (line[0] == '#' || line[0] == '\0')
		{
			continue;
		}
		strncpy(buf, line, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		n = split(buf, '\t', col, 4);
		if (n < 3 || strcmp(col[0], "CRC") != 0)
		{
			continue;
		}
		{
			uint8_t data[64];
			int len = hex2bin(col[1], data, sizeof(data));
			uint16_t want = (uint16_t)strtoul(col[2], NULL, 16);
			uint16_t got = crc16_modbus(data, (uint16_t)len);
			char msg[160];
			sprintf(msg, "CRC(%s)：期望 %04x 实得 %04x", col[1], want, got);
			ok(got == want, msg);
			g_crc_checked++;
		}
	}
}

int main(int argc, char **argv)
{
	const char *tsv = (argc > 1) ? argv[1] : "docs/protocol/test_vectors.tsv";
	FILE *fp;
	char line[1024];
	int nFrameCases = 0, nStreamCases = 0;

	printf("=== 固件协议模块原生测试 ===\n");
	printf("向量文件：%s\n\n", tsv);

	fp = fopen(tsv, "r");
	if (fp == NULL)
	{
		printf("打不开向量文件，先跑 tools/fw_prep/gen_test_vectors.py\n");
		return 2;
	}

	/* ---------- 1) CRC ---------- */
	test_crc_section(fp);

	/* ---------- 2) 帧用例 ---------- */
	fclose(fp);
	fp = fopen(tsv, "r");
	printf("\n[2] 帧用例（组帧逐字节比对 + 解帧往返 + 结构体打包往返）\n");
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char *col[4];
		char buf[2048];
		int n;
		chomp(line);
		if (line[0] == '#' || line[0] == '\0')
		{
			continue;
		}
		strncpy(buf, line, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		n = split(buf, '\t', col, 4);
		if (n < 4 || strcmp(col[0], "CRC") == 0 || strcmp(col[0], "STREAM") == 0)
		{
			continue;
		}
		{
			uint8_t cmd = (uint8_t)strtoul(col[1], NULL, 16);
			uint8_t data[PROTO_MAX_DATA_LEN];
			uint8_t wantFrame[PROTO_OVERHEAD + PROTO_MAX_DATA_LEN];
			uint8_t gotFrame[PROTO_OVERHEAD + PROTO_MAX_DATA_LEN];
			char gotHex[2 * (PROTO_OVERHEAD + PROTO_MAX_DATA_LEN) + 1];
			int dataLen = hex2bin(col[2], data, sizeof(data));
			int wantLen = hex2bin(col[3], wantFrame, sizeof(wantFrame));
			uint16_t encLen;
			char msg[512];

			nFrameCases++;

			/* 组帧 */
			encLen = Proto_Encode(cmd, data, (uint16_t)dataLen, gotFrame, sizeof(gotFrame));
			bin2hex(gotFrame, encLen, gotHex);
			sprintf(msg, "%s：组帧得 %s，期望 %s", col[0], gotHex, col[3]);
			ok(encLen == (uint16_t)wantLen && memcmp(gotFrame, wantFrame, (size_t)wantLen) == 0, msg);

			/* 解帧（一次喂完） */
			{
				ProtoDecoder dec;
				Collected c;
				uint16_t produced;
				memset(&c, 0, sizeof(c));
				Proto_Init(&dec);
				produced = Proto_Feed(&dec, wantFrame, (uint16_t)wantLen, 1000, collect_cb, &c);
				sprintf(msg, "%s：解帧应得 1 帧，实得 %u", col[0], (unsigned)produced);
				ok(produced == 1 && c.count == 1 && c.cmd[0] == cmd &&
				       c.len[0] == (uint16_t)dataLen &&
				       (dataLen == 0 || memcmp(c.data[0], data, (size_t)dataLen) == 0),
				   msg);
				ok(dec.framesOk == 1 && dec.framesDropped == 0,
				   "解帧后统计应为 ok=1 dropped=0");
			}

			/* 逐字节喂入（模拟最恶劣分片） */
			{
				ProtoDecoder dec;
				Collected c;
				int i;
				uint16_t total = 0;
				memset(&c, 0, sizeof(c));
				Proto_Init(&dec);
				for (i = 0; i < wantLen; i++)
				{
					total += Proto_Feed(&dec, &wantFrame[i], 1, (uint32_t)(1000 + i), collect_cb, &c);
				}
				sprintf(msg, "%s：逐字节喂入应得 1 帧，实得 %u", col[0], (unsigned)total);
				ok(total == 1 && c.count == 1 && c.cmd[0] == cmd, msg);
			}

			/* 按命令字做结构体打包往返（验证字段偏移与类型没写错） */
			{
				uint8_t repack[PROTO_MAX_DATA_LEN];
				uint16_t rl = 0;
				int same = 1;
				if (cmd == PROTO_UP_POSE)
				{
					ProtoPose s;
					if (Proto_Unpack_Pose(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_Pose(&s, repack); }
				}
				else if (cmd == PROTO_UP_MOTION)
				{
					ProtoMotion s;
					if (Proto_Unpack_Motion(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_Motion(&s, repack); }
				}
				else if (cmd == PROTO_UP_TASK)
				{
					ProtoTaskState s;
					if (Proto_Unpack_TaskState(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_TaskState(&s, repack); }
				}
				else if (cmd == PROTO_UP_EVENT)
				{
					uint8_t code; uint32_t ts;
					if (Proto_Unpack_Event(data, (uint16_t)dataLen, &code, &ts) != 0) { same = 0; }
					else { rl = Proto_Pack_Event(code, ts, repack); }
				}
				else if (cmd == PROTO_UP_ENV_GAS)
				{
					ProtoEnvGas s;
					if (Proto_Unpack_EnvGas(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_EnvGas(&s, repack); }
				}
				else if (cmd == PROTO_DOWN_MOTION)
				{
					ProtoMotionCmd s;
					if (Proto_Unpack_MotionCmd(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_MotionCmd(&s, repack); }
				}
				else if (cmd == PROTO_DOWN_SURVEY)
				{
					ProtoSurveyCfg s;
					if (Proto_Unpack_SurveyCfg(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_SurveyCfg(&s, repack); }
				}
				else if (cmd == PROTO_DOWN_PID_TUNE)
				{
					ProtoPidTune s;
					if (Proto_Unpack_PidTune(data, (uint16_t)dataLen, &s) != 0) { same = 0; }
					else { rl = Proto_Pack_PidTune(&s, repack); }
				}
				else
				{
					continue;   /* 该命令字没有专用结构体（如心跳/急停） */
				}
				sprintf(msg, "%s：结构体打包往返应还原数据段", col[0]);
				ok(same && rl == (uint16_t)dataLen &&
				       (dataLen == 0 || memcmp(repack, data, (size_t)dataLen) == 0), msg);
			}
		}
	}
	fclose(fp);

	/* ---------- 3) 流式用例 ---------- */
	fp = fopen(tsv, "r");
	printf("\n[3] 流式解帧用例\n");
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char *col[5];
		char buf[2048];
		int n;
		chomp(line);
		if (line[0] == '#' || line[0] == '\0')
		{
			continue;
		}
		strncpy(buf, line, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		n = split(buf, '\t', col, 5);
		if (n < 4 || strcmp(col[0], "STREAM") != 0)
		{
			continue;
		}
		{
			char *reads[16];
			int nReads = split(col[2], ';', reads, 16);
			char *exp[16];
			int nExp = split(col[3], ',', exp, 16);
			ProtoDecoder dec;
			Collected c;
			int r;
			uint32_t now = 5000;
			char msg[256];

			nStreamCases++;
			memset(&c, 0, sizeof(c));
			Proto_Init(&dec);
			for (r = 0; r < nReads; r++)
			{
				char *parts[8];
				int nParts = split(reads[r], ',', parts, 8);
				int k;
				for (k = 0; k < nParts; k++)
				{
					uint8_t bytes[1024];
					int nb = hex2bin(parts[k], bytes, sizeof(bytes));
					if (nb > 0)
					{
						Proto_Feed(&dec, bytes, (uint16_t)nb, now, collect_cb, &c);
					}
					now += 5;   /* 同一次 read 内的分段间隔 5 ms */
				}
				now += 20;      /* 两次 read 之间间隔 20 ms（< 100 ms 超时）*/
			}
			sprintf(msg, "%s：应得 %d 帧，实得 %d", col[1], nExp, c.count);
			ok(c.count == nExp, msg);
			for (r = 0; r < nExp && r < c.count; r++)
			{
				uint8_t want = (uint8_t)strtoul(exp[r], NULL, 16);
				sprintf(msg, "%s：第 %d 帧命令字应为 0x%02X，实得 0x%02X",
				        col[1], r + 1, want, c.cmd[r]);
				ok(c.cmd[r] == want, msg);
			}
			if (nExp == 0)
			{
				sprintf(msg, "%s：不应有有效帧时，应有丢弃计数（实得 %u）",
				        col[1], (unsigned)dec.framesDropped);
				ok(dec.framesDropped >= 1, msg);
			}
		}
	}
	fclose(fp);

	/* ---------- 4) 超时与边界（不来自向量，属于模块自身语义） ---------- */
	printf("\n[4] 超时 / 边界 / 环形缓冲\n");
	{
		/* 100 ms 超时：半帧后停顿 150 ms，必须丢弃并重新同步 */
		ProtoDecoder dec;
		Collected c;
		uint8_t frame[64];
		uint16_t flen;
		uint8_t data[5] = {0x01, 0x03, 45, 2, 0x00};

		memset(&c, 0, sizeof(c));
		Proto_Init(&dec);
		flen = Proto_Encode(PROTO_UP_TASK, data, 5, frame, sizeof(frame));

		Proto_Feed(&dec, frame, 5, 1000, collect_cb, &c);      /* 只喂前半帧 */
		ok(c.count == 0, "半帧不应产生帧");
		Proto_Feed(&dec, &frame[5], (uint16_t)(flen - 5), 1150, collect_cb, &c); /* 150 ms 后补齐 */
		ok(c.count == 0, "超时后应重新同步，不应拼出这一帧");
		ok(dec.framesDropped >= 1, "超时应计入丢弃");

		/* 超时后紧接着发一整帧，必须正常收到 */
		Proto_Feed(&dec, frame, flen, 1200, collect_cb, &c);
		ok(c.count == 1 && c.cmd[0] == PROTO_UP_TASK, "超时后新帧应能正常解析");

		/* 正常逐字节（每字节 1 ms）不应被误判超时 */
		memset(&c, 0, sizeof(c));
		Proto_Init(&dec);
		{
			int i;
			for (i = 0; i < flen; i++)
			{
				Proto_Feed(&dec, &frame[i], 1, (uint32_t)(2000 + i), collect_cb, &c);
			}
		}
		ok(c.count == 1 && dec.framesDropped == 0, "每字节 1 ms 的正常流不应触发超时");

		/* 长度非法（32767）应整帧作废且不越界 */
		{
			uint8_t bad[5] = {0xAA, 0x55, 0x14, 0xFF, 0x7F};
			memset(&c, 0, sizeof(c));
			Proto_Init(&dec);
			Proto_Feed(&dec, bad, 5, 3000, collect_cb, &c);
			ok(c.count == 0 && dec.framesDropped == 1, "长度 32767 应整帧作废");
		}

		/* 坏 CRC：末字节改坏 → 丢弃 */
		{
			uint8_t f2[64];
			uint16_t l2 = Proto_Encode(PROTO_UP_EVENT, data, 5, f2, sizeof(f2));
			memset(&c, 0, sizeof(c));
			Proto_Init(&dec);
			f2[l2 - 1] ^= 0xFF;
			Proto_Feed(&dec, f2, l2, 4000, collect_cb, &c);
			ok(c.count == 0 && dec.framesDropped >= 1, "坏 CRC 应丢弃整帧");
		}

		/* 增量 CRC 必须等于一次性 CRC */
		{
			uint8_t head[3] = {0x14, 0x10, 0x00};
			uint16_t a = crc16_modbus(head, 3);
			a = crc16_modbus_update(a, data, 5);
			{
				uint8_t whole[8];
				memcpy(whole, head, 3);
				memcpy(whole + 3, data, 5);
				ok(a == crc16_modbus(whole, 8), "增量 CRC 应等于一次性 CRC");
			}
		}

		/* 组帧参数非法：容量不足 / len 超限 → 返回 0 */
		{
			uint8_t small[8];
			uint8_t big[PROTO_MAX_DATA_LEN + 1];
			ok(Proto_Encode(0x10, data, 5, small, 5) == 0, "输出缓冲不足应返回 0");
			ok(Proto_Encode(0x10, big, PROTO_MAX_DATA_LEN + 1, big, sizeof(big)) == 0,
			   "数据段超上限应返回 0");
			ok(Proto_Encode(0x10, NULL, 5, small, sizeof(small)) == 0, "data 为空但 len>0 应返回 0");
		}
	}

	/* 环形缓冲 */
	{
		static RingBuf rb;
		uint8_t out[600];
		uint16_t i;
		uint16_t n;

		RingBuf_Init(&rb);
		ok(RingBuf_Count(&rb) == 0, "初始应为空");
		ok(RingBuf_Free(&rb) == RINGBUF_CAPACITY - 1, "初始可用空间 = 容量-1");

		ok(RingBuf_Push(&rb, 0x5A) == 1, "push 1 字节应成功");
		{
			uint8_t b = 0;
			ok(RingBuf_Pop(&rb, &b) == 1 && b == 0x5A, "pop 应取回同一字节");
			ok(RingBuf_Pop(&rb, &b) == 0, "空缓冲 pop 应返回 0");
		}

		/* 写满 → 再写必须丢弃且计数 */
		RingBuf_Init(&rb);
		{
			uint16_t cap = RINGBUF_CAPACITY - 1;
			for (i = 0; i < cap; i++)
			{
				(void)RingBuf_Push(&rb, (uint8_t)i);
			}
			ok(RingBuf_Count(&rb) == cap, "写满后 count 应为 容量-1");
			ok(RingBuf_Push(&rb, 0xEE) == 0 && rb.dropped == 1, "满时 push 应丢弃并计数");
			/* 数据不应被破坏：首个字节仍是最早写入的 0 */
			(void)RingBuf_Pop(&rb, &out[0]);
			ok(out[0] == 0, "满时丢弃不应破坏已有数据");
		}

		/* 环绕：用"参考计数器"模型验证 —— 推入的第 k 个字节是 k&0xFF，
		   弹出的第 m 个字节必须是 m&0xFF；两者独立计数，环绕几圈都不能错 */
		{
			uint32_t pushed = 0, popped = 0;
			int seq_ok = 1;
			uint32_t step;
			RingBuf_Init(&rb);
			for (step = 0; step < 5000u; step++)
			{
				if ((step % 2u) == 0u && RingBuf_Free(&rb) > 0u)
				{
					if (!RingBuf_Push(&rb, (uint8_t)(pushed & 0xFFu)))
					{
						seq_ok = 0;
						break;
					}
					pushed++;
				}
				else if (RingBuf_Count(&rb) > 0u)
				{
					uint8_t b = 0;
					if (!RingBuf_Pop(&rb, &b) || b != (uint8_t)(popped & 0xFFu))
					{
						seq_ok = 0;
						break;
					}
					popped++;
				}
			}
			ok(seq_ok, "环形缓冲环绕后数据顺序应正确（参考计数器模型）");
			ok(rb.dropped == 0, "参考模型下不应发生丢弃");
		}

		/* PushBuf / PopBuf 计数 */
		{
			uint8_t src[300];
			RingBuf_Init(&rb);
			for (i = 0; i < 300; i++)
			{
				src[i] = (uint8_t)(i * 7);
			}
			n = RingBuf_PushBuf(&rb, src, 300);
			ok(n == 300, "PushBuf 应推入 300 字节");
			n = RingBuf_PopBuf(&rb, out, 600);
			ok(n == 300 && memcmp(out, src, 300) == 0, "PopBuf 应取回同样内容");
			ok(RingBuf_Count(&rb) == 0, "取空后 count 应为 0");
		}
	}

	printf("\n=== 结果：通过 %d 条，失败 %d 条（帧用例 %d，流式用例 %d，CRC %d）===\n",
	       g_pass, g_fail, nFrameCases, nStreamCases, g_crc_checked);
	return g_fail;
}
