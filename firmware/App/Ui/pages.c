#include "pages.h"

#ifdef USE_FREERTOS        /* 自研显示页只在 RTOS 模式下启用（裸机模式保留厂商界面语义） */

#include "oled.h"
#include "app_api.h"
#include "protocol.h"

volatile u8 Flag_Page = PAGE_VENDOR;

/* 厂商的 OLED_ShowString 形参是 `const u8 *`，直接传字符串字面量会产生
   -Wpointer-sign 告警（厂商代码里就有一堆）。我们的新代码保持零告警，故统一转换。 */
#define S(x)   ((const u8 *)(x))

void Pages_Next(void)
{
	Flag_Page++;
	if (Flag_Page >= PAGES_COUNT)
	{
		Flag_Page = PAGE_VENDOR;
	}
	/* 中断里只改这一个变量；清屏与绘制交给 UiTask（中断不做耗时操作） */
}

/* ---------------------------------------------------------------------------
 * 小工具：OLED 只能显示整数，这里手工排小数点
 *
 * ★ 字号 12 的**真实步距**（照 `oled.c` 源码，别猜）：
 *     · `OLED_ShowString()` 每字符 **8 px**（函数内 `x += 8`）→ 一行最多 16 个字符
 *     · `OLED_ShowNumber()` 每个数字 **size/2 = 6 px**（`x + (size/2)*t`）
 *   所以"数字 + 小数点 + 数字"必须按 6 / 8 / 6 排：
 *     x → 整数 2 位（12 px）→ x+12 是小数点（8 px）→ x+20 是小数位（6 px）
 *   （本注释原先按"每字符 6 px"写过，导致小数点与数字重叠 —— 2026-10-09 由
 *     《人机界面设计说明》的排布核对发现并修正。）
 * -------------------------------------------------------------------------*/
static void show_num(u8 x, u8 y, u32 v, u8 len)
{
	OLED_ShowNumber(x, y, v, len, 12);
}

/* 显示 x.y（1 位小数，值 = v10/10）：宽 = 12 + 8 + 6 = 26 px */
static void show_x1(u8 x, u8 y, u16 v10)
{
	show_num(x, y, (u32)(v10 / 10u), 2);
	OLED_ShowString((u8)(x + 12), y, S("."));
	show_num((u8)(x + 20), y, (u32)(v10 % 10u), 1);
}

/* 显示 xx.yy（2 位小数，值 = v100/100）：宽 = 12 + 8 + 12 = 32 px —— 用于 Rs/R0 */
static void show_x2(u8 x, u8 y, u32 v100)
{
	show_num(x, y, v100 / 100u, 2);
	OLED_ShowString((u8)(x + 12), y, S("."));
	show_num((u8)(x + 20), y, v100 % 100u, 2);
}

/* ---------------------------------------------------------------------------
 * 页 1：环境与气体
 *   GAS ENV                   1HZ
 *   MQ2  30.0 ppm          ALM
 *   MQ135 50.0 ppm
 *   T 25.3C  H 60.1%
 *   Rs/R0  9.83    3.60
 *   CAL OK        COMP OFF
 * -------------------------------------------------------------------------*/
static void show_gas_page(void)
{
	AppGasSnapshot g;

	OLED_ShowString(0, 0, S("GAS ENV"));

	if (App_GetGas(&g) != 0)
	{
		OLED_ShowString(0, 10, S("READ ERR"));
		return;
	}

	/* 采样档位：预热 / 常态 1 Hz / 异常 5 Hz
	   ★ 三个档位文本都要给页码（x=120）让位：WARM 84..119、FAST 88..119、1HZ 96..119
	     （`FAST` 原在 x=90 → 占 90..121，会压住页码 2 px；2026-10-09 按排布核算改为 x=88）*/
	if (g.gear == PROTO_GEAR_WARMUP)
	{
		OLED_ShowString(84, 0, S("WARM"));
	}
	else if (g.gear == PROTO_GEAR_FAST)
	{
		OLED_ShowString(88, 0, S("FAST"));
	}
	else
	{
		OLED_ShowString(96, 0, S("1HZ"));
	}

	/* 第 2 行：MQ-2（估计浓度 ppm，报警时右侧提示）
	   "MQ2" 3×8=24 → 值 26 px（24..49）→ "ppm" 3×8=24（60..83）→ "ALM"（90..113）*/
	OLED_ShowString(0, 10, S("MQ2"));
	show_x1(24, 10, g.mq2PpmX10);
	OLED_ShowString(60, 10, S("ppm"));
	if (g.mq2Alarm)
	{
		OLED_ShowString(90, 10, S("ALM"));
	}

	/* 第 3 行：MQ-135
	   "MQ135" 5×8=40 → 值 26 px（44..69）→ "ppm"（78..101）→ 报警只放 1 个字符 A（108..115）*/
	OLED_ShowString(0, 20, S("MQ135"));
	show_x1(44, 20, g.mq135PpmX10);
	OLED_ShowString(78, 20, S("ppm"));
	if (g.mq135Alarm)
	{
		OLED_ShowString(108, 20, S("A"));
	}

	/* 第 4 行：温湿度（DHT11 无效时明确显示 N/A，不含糊）
	   T(0..7) 值 26 px(12..37) C(44..51) H(56..63) 值(68..93) %(96..103) */
	if (g.dhtValid)
	{
		OLED_ShowString(0, 30, S("T"));
		show_x1(12, 30, (u16)g.tempX10);
		OLED_ShowString(44, 30, S("C"));
		OLED_ShowString(56, 30, S("H"));
		show_x1(68, 30, g.humiX10);
		OLED_ShowString(96, 30, S("%"));
	}
	else
	{
		OLED_ShowString(0, 30, S("T/H  N/A"));
	}

	/* 第 5 行：Rs/R0（决策 18 的"主链路量"）—— 左 MQ-2、右 MQ-135
	   "Rs/R0" 5×8=40 → 值 32 px（44..75）→ 值 32 px（80..111）*/
	OLED_ShowString(0, 40, S("Rs/R0"));
	show_x2(44, 40, (u32)(g.mq2Ratio * 100.0f + 0.5f));
	show_x2(80, 40, (u32)(g.mq135Ratio * 100.0f + 0.5f));

	/* 第 6 行：R0 是否已标定 + 温湿度补偿是否开启
	   "CAL OK" 6×8=48（0..47）→ "COMP OFF" 8×8=64（56..119）*/
	OLED_ShowString(0, 50, (g.mq2R0 > 0.0f) ? S("CAL OK") : S("CAL..."));
	OLED_ShowString(56, 50, (g.readyBits & PROTO_RDY_COMP) ? S("COMP ON") : S("COMP OFF"));
}

/* ---------------------------------------------------------------------------
 * 页 2：巡检与通信
 *   PATROL PATROL
 *   TASK 1 PROG 40%
 *   WP 2 STOP 0  LOCK
 *   BT rx 12345
 *   WR rx  678 dr  0
 *   FR 1234  Hp 21000K
 * -------------------------------------------------------------------------*/
static const char *phase_text(u8 phase)
{
	switch (phase)
	{
	case PROTO_PHASE_INIT:     return "INIT";
	case PROTO_PHASE_WARMUP:   return "WARMUP";
	case PROTO_PHASE_IDLE:     return "IDLE";
	case PROTO_PHASE_PATROL:   return "PATROL";
	case PROTO_PHASE_ANALYSIS: return "ANALY";
	case PROTO_PHASE_RETURN:   return "RETURN";
	case PROTO_PHASE_DONE:     return "DONE";
	case PROTO_PHASE_FAULT:    return "FAULT";
	default:                   return "?";
	}
}

static void show_patrol_page(void)
{
	AppPatrolStatus p;
	AppDiag d;

	OLED_ShowString(0, 0, S("PATROL"));

	if (App_GetPatrol(&p) == 0)
	{
		/* "PATROL" 6×8=48（0..47）→ 阶段文本（48..95，最长 6 字符）→ 页码（120..127）*/
		OLED_ShowString(48, 0, S(phase_text(p.phase)));

		/* TASK(0..31) 任务号(32..43) PROG(52..83) 进度 3 位(84..101) %(104..111) */
		OLED_ShowString(0, 10, S("TASK"));
		show_num(32, 10, p.taskId, 2);
		OLED_ShowString(52, 10, S("PROG"));
		show_num(84, 10, p.progress, 3);
		OLED_ShowString(104, 10, S("%"));

		/* WP(0..15) 点号(16..21) STOP(30..61) 原因 2 位(64..75) LOCK(88..119) */
		OLED_ShowString(0, 20, S("WP"));
		show_num(16, 20, p.waypoint, 1);
		OLED_ShowString(30, 20, S("STOP"));
		show_num(64, 20, p.stopReason, 2);
		if (p.safetyStop)
		{
			OLED_ShowString(88, 20, S("LOCK"));
		}
	}

	if (App_GetDiag(&d) == 0)
	{
		/* 两条链路各收到多少字节、丢了多少（诊断环形缓冲是否够用）
		   "BT rx" 5×8=40（0..39）→ 5 位数值 30 px（44..73）*/
		OLED_ShowString(0, 30, S("BT rx"));
		show_num(44, 30, d.rxBytesBt, 5);

		/* WR rx(0..39) 数值(44..73) dr(78..93) 丢弃 3 位(96..113) */
		OLED_ShowString(0, 40, S("WR rx"));
		show_num(44, 40, d.rxBytesWire, 5);
		OLED_ShowString(78, 40, S("dr"));
		show_num(96, 40, d.rxDroppedWire, 3);

		/* FR(0..15) 解帧数 5 位(18..47) Hp(54..69) 剩余堆 4 位(72..95) K(100..107) */
		OLED_ShowString(0, 50, S("FR"));
		show_num(18, 50, d.framesOk, 5);
		OLED_ShowString(54, 50, S("Hp"));
		show_num(72, 50, d.heapFreeBytes / 1024u, 4);
		OLED_ShowString(100, 50, S("K"));
	}
}

/* ---------------------------------------------------------------------------
 * 对外：绘制当前页（由 UiTask 调用）
 * -------------------------------------------------------------------------*/
void Pages_Show(void)
{
	if (Flag_Page == PAGE_VENDOR)
	{
		return;                     /* 厂商页由 UiTask 调 APP_Show()/oled_show() 绘制 */
	}

	OLED_Clear();                   /* 换页会留残影，先清屏 */
	if (Flag_Page == PAGE_GAS)
	{
		show_gas_page();
	}
	else
	{
		show_patrol_page();
	}

	/* 右下角页码，提示"双击可继续切页" */
	OLED_ShowString(120, 0, S((Flag_Page == PAGE_GAS) ? "1" : "2"));
	OLED_Refresh_Gram();
}

#endif /* USE_FREERTOS */
