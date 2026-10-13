#include "odom.h"

/* 见 odom.h：纯整数里程计。
 *
 * 为什么用**定点整数**而不是浮点：
 *   Odom_AddCounts() 在控制中断（每 10 ms）里被调用，中断里应尽量少做事。
 *   这里把"每计数多少毫米"放大成整数微米（µm）累加，避免浮点与除法：
 *       1 count ≈ 0.135 mm = 135 µm（⌀67 mm 轮、1560 counts/圈，见 app_config.h）
 *   里程以 µm 累积，对外换算成 mm 只是除以 1000。
 *
 * ★ 本文件**只依赖 <stdint.h>**：标定系数由 Odom_SetScaleUmPerCount() 注入，
 *   所以它能在 PC 上被 patrol_test.c 直接编译测试（不拖 app_config.h / ST 头文件）。
 */

#define MM_PER_M          1000u
#define DEFAULT_UM_PER_COUNT  135u   /* 与 app_config.h 的默认标定一致（仅作兜底）*/

static volatile uint32_t s_legUm = 0;      /* 本段里程（µm）—— 中断写、任务读 */
static volatile uint32_t s_totalUm = 0;    /* 总里程（µm）*/
static uint32_t s_umPerCount = DEFAULT_UM_PER_COUNT;
static uint32_t s_spacingMm = 0;
static int      s_spacingSet = 0;

void Odom_SetScaleUmPerCount(uint32_t umPerCount)
{
	if (umPerCount > 0u)
	{
		s_umPerCount = umPerCount;
	}
}

void Odom_Init(uint32_t spacingMm)
{
	s_spacingMm = spacingMm;
	s_spacingSet = (spacingMm > 0u) ? 1 : 0;
	s_legUm = 0;
	s_totalUm = 0;
}

uint32_t Odom_CountsToMm(int left, int right)
{
	uint32_t l = (left < 0) ? (uint32_t)(-left) : (uint32_t)left;
	uint32_t r = (right < 0) ? (uint32_t)(-right) : (uint32_t)right;
	uint32_t avg = (l + r) / 2u;                    /* 取左右平均，抑制单轮打滑带来的偏差 */
	return (avg * s_umPerCount) / MM_PER_M;         /* µm → mm */
}

void Odom_AddCounts(int left, int right)
{
	uint32_t l = (left < 0) ? (uint32_t)(-left) : (uint32_t)left;
	uint32_t r = (right < 0) ? (uint32_t)(-right) : (uint32_t)right;
	uint32_t avgUm = ((l + r) / 2u) * s_umPerCount;

	s_legUm += avgUm;
	s_totalUm += avgUm;
}

int Odom_WaypointReached(void)
{
	if (!s_spacingSet)
	{
		return 0;                                /* 未配置间距 → 永不到位（拿到实测值再开）*/
	}
	if (s_legUm >= (uint32_t)s_spacingMm * MM_PER_M)
	{
		s_legUm = 0;                             /* 开始下一段（只触发一次）*/
		return 1;
	}
	return 0;
}

uint32_t Odom_LegMm(void)
{
	return s_legUm / MM_PER_M;
}

uint32_t Odom_TotalMm(void)
{
	return s_totalUm / MM_PER_M;
}

void Odom_Reset(void)
{
	s_legUm = 0;
	s_totalUm = 0;
}
