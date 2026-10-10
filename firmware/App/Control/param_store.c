#include "param_store.h"
#include "app_config.h"
#include "stmflash.h"
#include "sys.h"

/* 见 param_store.h：Flash 持久化（整扇区擦除 → 只在保存时调用）。
 *
 * ★ "参数表 ↔ 厂商全局量"的同步放在本文件（STM32 侧），
 *   这样 param_pack.c 保持纯逻辑、可在 PC 上单测。 */

/* 厂商全局量（定义在 main.c / control.c）*/
extern float Balance_Kp, Balance_Kd, Velocity_Kp, Velocity_Ki, Zhongzhi;
extern float Turn_Kp, Turn_Kd;

static ParamSet s_params;

/* 参数表 → 厂商全局量（控制中断下一拍即生效）*/
static void params_to_globals(const ParamSet *p)
{
	Balance_Kp = p->balanceKp;
	Balance_Kd = p->balanceKd;
	Velocity_Kp = p->velocityKp;
	Velocity_Ki = p->velocityKi;
	Zhongzhi = p->zhongzhi;
	Turn_Kp = p->turnKp;
	Turn_Kd = p->turnKd;
}

/* 厂商全局量 → 参数表（保存时以"实际生效值"为准）*/
static void params_from_globals(ParamSet *p)
{
	p->balanceKp = Balance_Kp;
	p->balanceKd = Balance_Kd;
	p->velocityKp = Velocity_Kp;
	p->velocityKi = Velocity_Ki;
	p->zhongzhi = Zhongzhi;
	p->turnKp = Turn_Kp;
	p->turnKd = Turn_Kd;
}

void ParamStore_InitDefaults(void)
{
	ParamPack_Default(&s_params);
	params_to_globals(&s_params);
}

int ParamStore_Load(void)
{
	static uint32_t words[PARAM_WORDS + 4];
	ParamSet loaded;

	/* 读多几个字，便于将来扩展时保持兼容 */
	STMFLASH_Read(CFG_PARAM_FLASH_ADDR, words, PARAM_WORDS + 4);
	if (ParamPack_Unpack(words, PARAM_WORDS + 4, CFG_PARAM_MAGIC, &loaded) != 0)
	{
		return -1;                          /* 未写过或无效：保持默认值 */
	}
	s_params = loaded;
	params_to_globals(&s_params);
	return 0;
}

int ParamStore_Save(void)
{
	static uint32_t words[PARAM_WORDS + 4];

	params_from_globals(&s_params);         /* 以"实际生效值"为准保存 */
	if (ParamPack_Pack(&s_params, CFG_PARAM_MAGIC, words, PARAM_WORDS + 4) == 0)
	{
		return -1;
	}
	STMFLASH_Write(CFG_PARAM_FLASH_ADDR, words, PARAM_WORDS + 4);

	/* 回读校验：写 Flash 有失败可能，这里当场确认，避免"以为存了" */
	{
		static uint32_t check[PARAM_WORDS + 4];
		ParamSet verify;
		STMFLASH_Read(CFG_PARAM_FLASH_ADDR, check, PARAM_WORDS + 4);
		if (ParamPack_Unpack(check, PARAM_WORDS + 4, CFG_PARAM_MAGIC, &verify) != 0)
		{
			return -1;
		}
	}
	return 0;
}

const ParamSet *ParamStore_Current(void)
{
	return &s_params;
}

int ParamStore_SetOne(uint8_t id, float value)
{
	if (ParamPack_Set(&s_params, id, value) != 0)
	{
		return -1;
	}
	/* 立即同步到厂商全局量：控制中断下一拍就会用到新值（对齐 float 写是原子的）*/
	params_to_globals(&s_params);
	return 0;
}

int ParamStore_GetOne(uint8_t id, float *value)
{
	return ParamPack_Get(&s_params, id, value);
}
