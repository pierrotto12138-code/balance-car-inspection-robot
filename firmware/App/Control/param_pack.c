#include "param_pack.h"
#include <string.h>

/*
 * 见 param_pack.h：纯打包/解包 + 参数表访问。
 * float 以**位型**存储（不用定点缩放），保证存进去与读出来逐位一致。
 *
 * ★ 本文件**只依赖 <string.h>**：不引用任何厂商全局量、不碰 Flash ——
 *   这样它能在 PC 上被 param_test.c 直接编译测试（"存了却读不回来"是极难查的问题）。
 *   "参数表 ↔ 厂商全局量"的同步在 param_store.c。
 */

void ParamPack_Default(ParamSet *p)
{
	/* 与厂商基线一致（见 App/Config/app_config.h 的 CFG_PID_*）*/
	p->balanceKp = 300.0f;
	p->balanceKd = 1.0f;
	p->velocityKp = 80.0f;
	p->velocityKi = 0.4f;
	p->zhongzhi = 1.0f;
	p->turnKp = 52.0f;
	p->turnKd = 0.5f;
}

int ParamPack_Get(const ParamSet *p, uint8_t id, float *value)
{
	switch (id)
	{
	case PARAM_BALANCE_KP:  *value = p->balanceKp;  return 0;
	case PARAM_BALANCE_KD:  *value = p->balanceKd;  return 0;
	case PARAM_VELOCITY_KP: *value = p->velocityKp; return 0;
	case PARAM_VELOCITY_KI: *value = p->velocityKi; return 0;
	case PARAM_ZHONGZHI:    *value = p->zhongzhi;   return 0;
	case PARAM_TURN_KP:     *value = p->turnKp;     return 0;
	case PARAM_TURN_KD:     *value = p->turnKd;     return 0;
	default:                return -1;
	}
}

int ParamPack_Set(ParamSet *p, uint8_t id, float value)
{
	switch (id)
	{
	case PARAM_BALANCE_KP:  p->balanceKp  = value; return 0;
	case PARAM_BALANCE_KD:  p->balanceKd  = value; return 0;
	case PARAM_VELOCITY_KP: p->velocityKp = value; return 0;
	case PARAM_VELOCITY_KI: p->velocityKi = value; return 0;
	case PARAM_ZHONGZHI:    p->zhongzhi   = value; return 0;
	case PARAM_TURN_KP:     p->turnKp     = value; return 0;
	case PARAM_TURN_KD:     p->turnKd     = value; return 0;
	default:                return -1;
	}
}

uint16_t ParamPack_Pack(const ParamSet *p, uint32_t magic, uint32_t *words, uint16_t capWords)
{
	float vals[PARAM_COUNT];
	uint16_t i;

	if (words == 0 || capWords < PARAM_WORDS)
	{
		return 0;
	}

	vals[0] = p->balanceKp;
	vals[1] = p->balanceKd;
	vals[2] = p->velocityKp;
	vals[3] = p->velocityKi;
	vals[4] = p->zhongzhi;
	vals[5] = p->turnKp;
	vals[6] = p->turnKd;

	words[0] = magic;
	words[1] = (uint32_t)PARAM_COUNT;
	for (i = 0; i < PARAM_COUNT; i++)
	{
		uint32_t bits;
		memcpy(&bits, &vals[i], sizeof(bits));   /* 位型存储：不损失精度、不依赖 FPU 舍入 */
		words[2 + i] = bits;
	}
	/* 余下字填 0xFFFFFFFF（与"Flash 未写"一致，便于人工核对）*/
	for (i = PARAM_WORDS; i < capWords; i++)
	{
		words[i] = 0xFFFFFFFFu;
	}
	return PARAM_WORDS;
}

int ParamPack_Unpack(const uint32_t *words, uint16_t capWords,
                     uint32_t magic, ParamSet *out)
{
	float vals[PARAM_COUNT];
	uint16_t i;

	if (words == 0 || out == 0 || capWords < PARAM_WORDS)
	{
		return -1;
	}
	if (words[0] != magic || words[1] != (uint32_t)PARAM_COUNT)
	{
		return -1;                              /* 未写过或格式不符 → 调用方退回默认值 */
	}
	for (i = 0; i < PARAM_COUNT; i++)
	{
		uint32_t bits = words[2 + i];
		memcpy(&vals[i], &bits, sizeof(bits));
		/* 合理性检查：防止 Flash 位翻转产生离谱参数把车"调飞" */
		if (!(vals[i] > -1.0e6f && vals[i] < 1.0e6f))
		{
			return -1;
		}
	}

	out->balanceKp = vals[0];
	out->balanceKd = vals[1];
	out->velocityKp = vals[2];
	out->velocityKi = vals[3];
	out->zhongzhi = vals[4];
	out->turnKp = vals[5];
	out->turnKd = vals[6];
	return 0;
}
