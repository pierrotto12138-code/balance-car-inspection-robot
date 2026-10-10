/*
 * 参数打包/解包**原生（PC）单元测试** —— 不需要板子。
 *
 * 为什么值得单独测：
 *   "调好的 PID 存了 Flash，重启却回到默认值（或全变成 0）"是极难查的问题，
 *   而 Flash 读写本身无法离线测；把**打包/解包**拆出来（param_pack.c）就能离线验证：
 *     · 往返一致（float 位型存储，逐位相同）
 *     · magic / 参数个数 不符 → 必须拒绝（退回默认值），不能把垃圾当参数用
 *     · 数值离谱（模拟 Flash 位翻转）→ 必须拒绝
 *     · 按 ID 取值/设值与协议 0x24 的 ID 对应关系正确
 *
 * 退出码 = 失败条数（0 = 全部通过）。
 */
#include <stdio.h>
#include <string.h>

#include "param_pack.h"

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

#define MAGIC 0x50494431u   /* 'PID1'，与 app_config.h 的 CFG_PARAM_MAGIC 一致 */

int main(void)
{
	ParamSet a, b;
	uint32_t words[PARAM_WORDS + 4];
	uint16_t n;

	printf("=== PID 参数打包/解包原生测试 ===\n\n");

	ParamPack_Default(&a);
	ok(a.balanceKp == 300.0f && a.balanceKd == 1.0f && a.velocityKp == 80.0f &&
	   a.velocityKi == 0.4f && a.zhongzhi == 1.0f && a.turnKp == 52.0f && a.turnKd == 0.5f,
	   "默认值应与厂商基线一致（300/1/80/0.4/1/52/0.5）");

	/* 往返一致（含小数与非整数增益）*/
	a.balanceKp = 321.75f;
	a.balanceKd = 0.85f;
	a.velocityKp = 77.5f;
	a.velocityKi = 0.37f;
	a.zhongzhi = -1.25f;
	a.turnKp = 48.125f;
	a.turnKd = 0.0625f;
	n = ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	ok(n == PARAM_WORDS, "Pack 应返回 PARAM_WORDS");
	ok(words[0] == MAGIC && words[1] == PARAM_COUNT, "头部应写入 magic 与参数个数");
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) == 0, "Unpack 应成功");
	ok(memcmp(&a, &b, sizeof(a)) == 0, "往返后应逐位一致（float 位型存储）");

	/* 极端值也不应丢精度 */
	a.balanceKp = 1.0e-6f;
	a.velocityKi = 12345.678f;
	(void)ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) == 0 &&
	   b.balanceKp == a.balanceKp && b.velocityKi == a.velocityKi,
	   "极端量级也应逐位一致");

	/* magic 不符 → 拒绝 */
	ParamPack_Default(&a);
	(void)ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	words[0] = 0xDEADBEEFu;
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) != 0, "magic 不符必须拒绝");

	/* 参数个数不符 → 拒绝 */
	(void)ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	words[1] = 99u;
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) != 0, "参数个数不符必须拒绝");

	/* 全 0xFF（Flash 未写过）→ 拒绝 */
	memset(words, 0xFF, sizeof(words));
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) != 0, "Flash 未写过（全 FF）必须拒绝");

	/* 位翻转成离谱值 → 拒绝（防止把车"调飞"）*/
	(void)ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	words[2] = 0x7F7FFFFFu;                 /* 约 3.4e38 */
	ok(ParamPack_Unpack(words, PARAM_WORDS + 4, MAGIC, &b) != 0, "离谱数值必须拒绝");

	/* 容量不足 → 拒绝（正好 PARAM_WORDS 是**够用**的，少一个字才该拒绝）*/
	(void)ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS + 4);
	ok(ParamPack_Unpack(words, PARAM_WORDS, MAGIC, &b) == 0,
	   "容量恰好等于 PARAM_WORDS 时应成功（这是合法的最小容量）");
	ok(ParamPack_Unpack(words, PARAM_WORDS - 1, MAGIC, &b) != 0, "容量不足必须拒绝");
	ok(ParamPack_Pack(&a, MAGIC, words, PARAM_WORDS - 1) == 0, "Pack 容量不足应返回 0");

	/* 按 ID 取值/设值：必须与协议 §4.2 的 ID 对应 */
	{
		ParamSet p;
		float v = 0.0f;
		ParamPack_Default(&p);
		ok(ParamPack_Get(&p, PARAM_BALANCE_KP, &v) == 0 && v == 300.0f, "ID 0x01 = Balance_Kp");
		ok(ParamPack_Set(&p, PARAM_BALANCE_KP, 333.0f) == 0 && p.balanceKp == 333.0f,
		   "写 0x01 应改到 Balance_Kp");
		ok(ParamPack_Get(&p, PARAM_BALANCE_KD, &v) == 0 && v == 1.0f, "ID 0x02 = Balance_Kd");
		ok(ParamPack_Get(&p, PARAM_VELOCITY_KP, &v) == 0 && v == 80.0f, "ID 0x03 = Velocity_Kp");
		ok(ParamPack_Get(&p, PARAM_VELOCITY_KI, &v) == 0 && v == 0.4f, "ID 0x04 = Velocity_Ki");
		ok(ParamPack_Get(&p, PARAM_ZHONGZHI, &v) == 0 && v == 1.0f, "ID 0x05 = Zhongzhi");
		ok(ParamPack_Get(&p, PARAM_TURN_KP, &v) == 0 && v == 52.0f, "ID 0x06 = Turn_Kp");
		ok(ParamPack_Get(&p, PARAM_TURN_KD, &v) == 0 && v == 0.5f, "ID 0x07 = Turn_Kd");
		ok(ParamPack_Get(&p, 0x42u, &v) != 0, "未知 ID 必须报错");
		ok(ParamPack_Set(&p, 0x42u, 1.0f) != 0, "未知 ID 写入必须报错");
	}

	/* 7 个参数互不串位：逐个改、逐个校验 */
	{
		ParamSet p;
		uint8_t ids[PARAM_COUNT] = {PARAM_BALANCE_KP, PARAM_BALANCE_KD, PARAM_VELOCITY_KP,
		                            PARAM_VELOCITY_KI, PARAM_ZHONGZHI, PARAM_TURN_KP,
		                            PARAM_TURN_KD};
		uint8_t i;
		int allOk = 1;
		ParamPack_Default(&p);
		for (i = 0; i < PARAM_COUNT; i++)
		{
			float v = 0.0f;
			(void)ParamPack_Set(&p, ids[i], (float)(i + 1) * 11.0f);
			if (ParamPack_Get(&p, ids[i], &v) != 0 || v != (float)(i + 1) * 11.0f)
			{
				allOk = 0;
			}
		}
		ok(allOk, "7 个参数逐个写入后应能逐个读回（无串位）");
	}

	printf("\n=== 结果：通过 %d 条，失败 %d 条 ===\n", g_pass, g_fail);
	return g_fail;
}
