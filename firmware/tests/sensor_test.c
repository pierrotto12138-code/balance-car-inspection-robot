/*
 * 传感器算法**原生（PC）单元测试** —— 不需要板子。
 *
 * 做法与协议测试同源（AGENTS 铁律 23）：
 *   tools/fw_prep/gen_sensor_vectors.py 用**独立的 Python 实现**算出每一级中间量
 *   （反分压电压 → Rs → Rs/R0 → 温湿度补偿 → ppm），写成 CSV；
 *   本文件让**固件实现**逐级复算并比对 —— 哪一级错了立刻能定位。
 *
 * 覆盖：GAS（换算）/ CALIB（R0 标定）/ AVG（滑动平均窗口）/ CORR（补偿系数）/
 *       PARSE（DHT11 解析与校验）/ ALARM（多帧确认 + 迟滞）
 *
 * 退出码 = 失败条数（0 = 全部通过）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gas.h"
#include "dht11.h"
#include "sensor_report.h"

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

/* 相对误差比较（Python 用 double、C 用 float，容差按量级给） */
static int close_rel(double got, double want, double rel)
{
	double diff = fabs(got - want);
	double scale = fabs(want) > 1e-9 ? fabs(want) : 1e-9;
	return diff <= rel * scale || diff <= 1e-6;
}

static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
	{
		s[--n] = '\0';
	}
}

static int split(char *s, char sep, char **out, int cap)
{
	int n = 0;
	char *p = s;
	if (*s == '\0')
	{
		return 0;
	}
	while (p != NULL && n < cap)
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

static void cfg_for(const char *chan, GasChannelCfg *c)
{
	if (strcmp(chan, "mq2") == 0)
	{
		Gas_DefaultCfg_MQ2(c);
	}
	else
	{
		Gas_DefaultCfg_MQ135(c);
	}
}

int main(int argc, char **argv)
{
	const char *csv = (argc > 1) ? argv[1] : "firmware/tests/sensor_vectors.csv";
	FILE *fp;
	char line[4096];
	int nGas = 0, nCorr = 0, nParse = 0, nAlarm = 0, nCalib = 0, nAvg = 0;

	printf("=== 传感器算法原生测试 ===\n向量文件：%s\n\n", csv);
	fp = fopen(csv, "r");
	if (fp == NULL)
	{
		printf("打不开向量文件，先跑 tools/fw_prep/gen_sensor_vectors.py\n");
		return 2;
	}

	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char buf[4096];
		char *col[40];
		int n;
		chomp(line);
		if (line[0] == '#' || line[0] == '\0')
		{
			continue;
		}
		strncpy(buf, line, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		n = split(buf, ',', col, 40);
		if (n < 2)
		{
			continue;
		}

		/* ---------------- GAS：整条链路逐级比对 ---------------- */
		if (strcmp(col[0], "GAS") == 0 && n >= 12)
		{
			GasChannelCfg cfg;
			GasChannel ch;
			uint16_t raw = (uint16_t)atoi(col[2]);
			float t = (float)atof(col[3]);
			float h = (float)atof(col[4]);
			int comp = atoi(col[5]);
			float r0 = (float)atof(col[6]);
			double wantVao = atof(col[7]);
			double wantRs = atof(col[8]);
			double wantRatioC = atof(col[10]);
			double wantPpm = atof(col[11]);
			double gotVao, gotRatio;
			char msg[320];

			cfg_for(col[1], &cfg);
			Gas_InitChannel(&ch, &cfg);
			ch.r0 = r0;                     /* 用向量给定的 R0，隔离"标定"这一级 */
			gotVao = Gas_AdcToVaoMv(&cfg, raw);
			gotRatio = Gas_Update(&ch, raw, t, h, comp);
			nGas++;

			sprintf(msg, "GAS %s adc=%u：AO 电压 期望 %.3f 实得 %.3f",
			        col[1], raw, wantVao, gotVao);
			ok(close_rel(gotVao, wantVao, 1e-4), msg);

			sprintf(msg, "GAS %s adc=%u：Rs 期望 %.3f 实得 %.3f",
			        col[1], raw, wantRs, (double)ch.rs);
			ok(close_rel(ch.rs, wantRs, 1e-4), msg);

			sprintf(msg, "GAS %s adc=%u comp=%d：ratio 期望 %.9f 实得 %.9f",
			        col[1], raw, comp, wantRatioC, gotRatio);
			ok(close_rel(gotRatio, wantRatioC, 1e-4), msg);

			sprintf(msg, "GAS %s adc=%u comp=%d：ppm 期望 %.6f 实得 %.6f",
			        col[1], raw, comp, wantPpm, (double)ch.ppm);
			ok(close_rel(ch.ppm, wantPpm, 1e-3), msg);
			continue;
		}

		/* ---------------- CALIB：R0 标定 ---------------- */
		if (strcmp(col[0], "CALIB") == 0 && n >= 5)
		{
			GasChannelCfg cfg;
			GasChannel ch;
			double wantR0 = atof(col[2]);
			int i;
			char msg[200];

			cfg_for(col[1], &cfg);
			Gas_InitChannel(&ch, &cfg);
			for (i = 3; i < n; i++)
			{
				Gas_CalibrateAdd(&ch, (uint16_t)atoi(col[i]));
			}
			{
				float gotR0 = Gas_CalibrateFinish(&ch);
				nCalib++;
				sprintf(msg, "CALIB %s：R0 期望 %.3f 实得 %.3f", col[1], wantR0, (double)gotR0);
				ok(close_rel(gotR0, wantR0, 1e-4), msg);
				/* 标定后 r0 必须写入通道 */
				ok(close_rel(ch.r0, wantR0, 1e-4), "CALIB：标定结果应写入 ch.r0");
			}
			continue;
		}

		/* ---------------- AVG：滑动平均窗口 ---------------- */
		if (strcmp(col[0], "AVG") == 0 && n >= 4)
		{
			GasChannelCfg cfg;
			GasChannel ch;
			float r0 = (float)atof(col[2]);
			int i;
			int nSeq = (n - 3) / 2;      /* 行 = AVG,chan,r0,<nSeq 个 adc>,<nSeq 个期望均值> */

			cfg_for(col[1], &cfg);
			Gas_InitChannel(&ch, &cfg);
			ch.r0 = r0;
			for (i = 0; i < nSeq; i++)
			{
				uint16_t raw = (uint16_t)atoi(col[3 + i]);
				double want = atof(col[3 + nSeq + i]);
				double got = Gas_Update(&ch, raw, 25.0f, 50.0f, 0);
				char msg[200];
				nAvg++;
				sprintf(msg, "AVG %s 第 %d 次：均值 期望 %.9f 实得 %.9f",
				        col[1], i + 1, want, got);
				ok(close_rel(got, want, 1e-4), msg);
			}
			continue;
		}

		/* ---------------- CORR：补偿系数 ---------------- */
		if (strcmp(col[0], "CORR") == 0 && n >= 4)
		{
			double t = atof(col[1]);
			double h = atof(col[2]);
			double want = atof(col[3]);
			double got = Gas_CorrectionFactor((float)t, (float)h);
			char msg[200];
			nCorr++;
			sprintf(msg, "CORR t=%.4f h=%.1f：期望 %.9f 实得 %.9f", t, h, want, got);
			ok(close_rel(got, want, 1e-4), msg);
			continue;
		}

		/* ---------------- PARSE：DHT11 解析 ---------------- */
		if (strcmp(col[0], "PARSE") == 0 && n >= 7)
		{
			uint8_t raw[5];
			Dht11Reading r;
			int wantRc = atoi(col[2]);
			int gotRc, i;
			char msg[200];

			if (strlen(col[1]) != 10)
			{
				ok(0, "PARSE：向量里的 hex 长度应为 10");
				continue;
			}
			for (i = 0; i < 5; i++)
			{
				unsigned v;
				sscanf(col[1] + i * 2, "%2x", &v);
				raw[i] = (uint8_t)v;
			}
			gotRc = Dht11_Parse(raw, &r);
			nParse++;
			sprintf(msg, "PARSE %s：返回码 期望 %d 实得 %d", col[1], wantRc, gotRc);
			ok(gotRc == wantRc, msg);
			if (wantRc == 0)
			{
				sprintf(msg, "PARSE %s：湿度 期望 %d 实得 %d", col[1], atoi(col[3]), r.humiInt);
				ok(r.humiInt == atoi(col[3]) && r.humiDec == atoi(col[4]) &&
				       r.tempInt == atoi(col[5]) && r.tempDec == atoi(col[6]), msg);
				/* 浮点换算也要对 */
				ok(close_rel(Dht11_TempC(&r), atoi(col[5]) + atoi(col[6]) * 0.1, 1e-6),
				   "PARSE：温度换算");
				ok(close_rel(Dht11_HumiRh(&r), atoi(col[3]) + atoi(col[4]) * 0.1, 1e-6),
				   "PARSE：湿度换算");
			}
			continue;
		}

		/* ---------------- ALARM：多帧确认 + 迟滞 ---------------- */
		if (strcmp(col[0], "ALARM") == 0 && n >= 5)
		{
			char *ppms[40];
			char *flags[40];
			int nPpm = split(col[1], ';', ppms, 40);
			int nFlag = split(col[4], ';', flags, 40);
			uint16_t thresh = (uint16_t)atoi(col[2]);
			uint8_t confirm = (uint8_t)atoi(col[3]);
			GasChannelCfg cfg;
			GasChannel ch;
			int i;
			char msg[240];

			(void)nFlag;
			cfg_for("mq2", &cfg);
			Gas_InitChannel(&ch, &cfg);
			ch.r0 = 1000.0f;
			nAlarm++;
			for (i = 0; i < nPpm; i++)
			{
				int want = atoi(flags[i]);
				int got;
				ch.ppm = (float)atof(ppms[i]);
				got = Gas_UpdateAlarm(&ch, thresh, confirm);
				sprintf(msg, "ALARM 第 %d 帧(ppm=%s,阈值=%u,确认=%u)：上升沿 期望 %d 实得 %d",
				        i + 1, ppms[i], thresh, confirm, want, got);
				ok(got == want, msg);
			}
			continue;
		}
	}
	fclose(fp);

	/* ---------------- 额外不变量（不来自向量） ---------------- */
	printf("\n[不变量] 数学性质与边界\n");
	{
		/* 补偿系数在参考点附近应≈1.0（这是"用除法而不是叠加"的关键理由） */
		double f19 = Gas_CorrectionFactor(19.3714f, 33.0f);
		ok(fabs(f19 - 1.0) < 0.005, "补偿系数在参考点(≈19.4℃/33%RH)应≈1.0");
		/* 湿度升高 → 系数**变小**（公式里是 −(h−33)·D）；
		   除法修正后 ratio 变大，正好抵消"湿度升高使 MQ 的 Rs/R0 下降"这一漂移。
		   （注意：方向别记反 —— 我们最初就写反过一次，被测试抓出来） */
		ok(Gas_CorrectionFactor(25.0f, 80.0f) < Gas_CorrectionFactor(25.0f, 30.0f),
		   "湿度越高，补偿系数应越小（公式含 −(h−33)·D）");
		/* Vao 为 0（ADC=0）时 Rs 应钳到 0，且不产生除零 */
		{
			GasChannelCfg cfg;
			GasChannel ch;
			Gas_DefaultCfg_MQ2(&cfg);
			Gas_InitChannel(&ch, &cfg);
			ch.r0 = 1000.0f;
			ok(Gas_VaoToRs(&cfg, 0.0f) == 0.0f, "Vao=0 时 Rs 应为 0（不除零）");
			ok(Gas_Update(&ch, 0, 25.0f, 50.0f, 0) == 0.0f, "ADC=0 时 ratio 应为 0");
		}
		/* 未标定（r0=0）时不应给出 ratio/ppm */
		{
			GasChannelCfg cfg;
			GasChannel ch;
			Gas_DefaultCfg_MQ2(&cfg);
			Gas_InitChannel(&ch, &cfg);
			ok(Gas_Update(&ch, 2000, 25.0f, 50.0f, 0) == 0.0f && ch.ppm == 0.0f,
			   "未标定时 ratio/ppm 应为 0（只保留 Rs）");
		}
		/* 补偿关闭 vs 打开：打开后（湿度 80% > 33%）ratio 应变小 */
		{
			GasChannelCfg cfg;
			GasChannel a, b;
			Gas_DefaultCfg_MQ2(&cfg);
			Gas_InitChannel(&a, &cfg);
			Gas_InitChannel(&b, &cfg);
			a.r0 = 1000.0f;
			b.r0 = 1000.0f;
			(void)Gas_Update(&a, 2000, 25.0f, 80.0f, 0);
			(void)Gas_Update(&b, 2000, 25.0f, 80.0f, 1);
			ok(b.ratioAvg > a.ratioAvg, "湿度 80% 时，开启补偿应使 ratio 变大（抵消湿度漂移）");
			/* 补偿的复原性：b.ratioAvg × factor == a.ratioAvg */
			{
				double f = Gas_CorrectionFactor(25.0f, 80.0f);
				ok(close_rel(b.ratioAvg * f, a.ratioAvg, 1e-4), "ratio/factor 应能还原未补偿值");
			}
		}
		/* 迟滞：确认后掉到 90% 以上不解除，掉到 90% 以下才解除 */
		{
			GasChannelCfg cfg;
			GasChannel ch;
			Gas_InitChannel(&ch, &cfg);
			ch.ppm = 40.0f;
			(void)Gas_UpdateAlarm(&ch, 300, 3);
			(void)Gas_UpdateAlarm(&ch, 300, 3);
			ok(Gas_UpdateAlarm(&ch, 300, 3) == 1 && ch.alarm == 1, "连续 3 帧超阈值应确认报警");
			ch.ppm = 28.0f;                          /* >90% 阈值(27) */
			(void)Gas_UpdateAlarm(&ch, 300, 3);
			ok(ch.alarm == 1, "掉到 90% 阈值以上不应解除（迟滞）");
			ch.ppm = 20.0f;                          /* <90% */
			(void)Gas_UpdateAlarm(&ch, 300, 3);
			ok(ch.alarm == 0, "掉到 90% 阈值以下应解除");
		}
	}

	printf("\n[REPORT] 组包与饱和钳位（0x14 数据段）\n");
	{
		GasChannelCfg cfg2, cfg135;
		GasChannel mq2, mq135;
		Dht11Reading dht;
		ProtoEnvGas env;

		Gas_DefaultCfg_MQ2(&cfg2);
		Gas_DefaultCfg_MQ135(&cfg135);
		Gas_InitChannel(&mq2, &cfg2);
		Gas_InitChannel(&mq135, &cfg135);

		/* ppm → ppm×10：四舍五入 + 饱和钳位（幂律 ppm 会爆到千万级，必须钳） */
		ok(SensorReport_PpmToX10(0.0f) == 0, "ppm 0 → 0");
		ok(SensorReport_PpmToX10(-5.0f) == 0, "负 ppm → 0");
		ok(SensorReport_PpmToX10(12.34f) == 123, "ppm 12.34 → 123（四舍五入）");
		ok(SensorReport_PpmToX10(6553.5f) == 65535, "ppm 6553.5 → 65535（正好上限）");
		ok(SensorReport_PpmToX10(1.0e6f) == 65535, "ppm 1e6 → 饱和到 65535（不回绕）");
		ok(SensorReport_PpmToX10(1.0e30f) == 65535, "超大 ppm → 仍饱和到 65535");

		/* 组包：两路已标定 + DHT 有效 + 补偿开 → 就绪位应为 0x0F */
		mq2.r0 = 1000.0f;
		mq135.r0 = 2000.0f;
		mq2.lastRaw = 1234;
		mq135.lastRaw = 2345;
		mq2.ppm = 56.7f;
		mq135.ppm = 89.0f;
		dht.humiInt = 61; dht.humiDec = 0;
		dht.tempInt = 25; dht.tempDec = 6;
		SensorReport_FillEnvGas(&mq2, &mq135, &dht, 1, 1, PROTO_GEAR_NORMAL, &env);
		ok(env.mq2Raw == 1234 && env.mq135Raw == 2345, "原始 ADC 值应来自各通道的 lastRaw");
		ok(env.mq2PpmX10 == 567 && env.mq135PpmX10 == 890, "ppm×10 换算");
		ok(env.tempX10 == 256 && env.humiX10 == 610, "温度/湿度 ×10 换算");
		ok(env.readyBits == 0x0F, "两路已标定 + DHT 有效 + 补偿开 → 0x0F");
		ok(env.gear == PROTO_GEAR_NORMAL, "档位应原样透传");

		/* DHT 无效时：温湿度置 0、DHT 位清 0，**补偿位也不能置**（没数据谈不上补偿） */
		SensorReport_FillEnvGas(&mq2, &mq135, 0, 0, 1, PROTO_GEAR_NORMAL, &env);
		ok(env.tempX10 == 0 && env.humiX10 == 0, "DHT 无效时温湿度应为 0");
		ok((env.readyBits & PROTO_RDY_DHT11) == 0, "DHT 无效时 DHT 就绪位应为 0");
		ok((env.readyBits & PROTO_RDY_COMP) == 0, "DHT 无效时补偿位不应置 1");
		ok(env.readyBits == (PROTO_RDY_MQ2 | PROTO_RDY_MQ135), "只应剩两路 MQ 就绪位");

		/* 未标定的通道不算"就绪" */
		mq135.r0 = 0.0f;
		SensorReport_FillEnvGas(&mq2, &mq135, &dht, 1, 0, PROTO_GEAR_NORMAL, &env);
		ok((env.readyBits & PROTO_RDY_MQ135) == 0, "未标定的 MQ-135 不应算就绪");

		/* 档位决策 */
		ok(SensorReport_DecideGear(&mq2, &mq135, 0, 1) == PROTO_GEAR_WARMUP, "预热未完成 → 预热档");
		ok(SensorReport_DecideGear(&mq2, &mq135, 1, 0) == PROTO_GEAR_NORMAL, "关闭自适应 → 恒常态");
		ok(SensorReport_DecideGear(&mq2, &mq135, 1, 1) == PROTO_GEAR_NORMAL, "正常 → 常态 1 Hz");
		mq2.alarm = 1;
		ok(SensorReport_DecideGear(&mq2, &mq135, 1, 1) == PROTO_GEAR_FAST, "报警 → 异常 5 Hz");
		mq2.alarm = 0;
		mq2.overCount = 1;
		ok(SensorReport_DecideGear(&mq2, &mq135, 1, 1) == PROTO_GEAR_FAST, "连续超阈值中 → 5 Hz");
	}

	printf("\n=== 结果：通过 %d 条，失败 %d 条"
	       "（GAS %d / CALIB %d / AVG %d / CORR %d / PARSE %d / ALARM %d）===\n",
	       g_pass, g_fail, nGas, nCalib, nAvg, nCorr, nParse, nAlarm);
	return g_fail;
}
