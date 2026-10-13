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
 * 字号 12 时每字符宽 6 像素，一行 21 字符（128 像素）
 * -------------------------------------------------------------------------*/
static void show_num(u8 x, u8 y, u32 v, u8 len)
{
	OLED_ShowNumber(x, y, v, len, 12);
}

/* 显示 x.y（1 位小数，值 = v10/10）*/
static void show_x1(u8 x, u8 y, u16 v10)
{
	show_num(x, y, (u32)(v10 / 10u), 2);
	OLED_ShowString((u8)(x + 12), y, S("."));
	show_num((u8)(x + 18), y, (u32)(v10 % 10u), 1);
}

/* 显示 xx.yy（2 位小数，值 = v100/100）—— 用于 Rs/R0 */
static void show_x2(u8 x, u8 y, u32 v100)
{
	show_num(x, y, v100 / 100u, 2);
	OLED_ShowString((u8)(x + 12), y, S("."));
	show_num((u8)(x + 18), y, v100 % 100u, 2);
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

	/* 采样档位：预热 / 常态 1 Hz / 异常 5 Hz */
	if (g.gear == PROTO_GEAR_WARMUP)
	{
		OLED_ShowString(84, 0, S("WARM"));
	}
	else if (g.gear == PROTO_GEAR_FAST)
	{
		OLED_ShowString(90, 0, S("FAST"));
	}
	else
	{
		OLED_ShowString(96, 0, S("1HZ"));
	}

	/* 第 2 行：MQ-2（估计浓度 ppm，报警时右侧提示）*/
	OLED_ShowString(0, 10, S("MQ2"));
	show_x1(24, 10, g.mq2PpmX10);
	OLED_ShowString(60, 10, S("ppm"));
	if (g.mq2Alarm)
	{
		OLED_ShowString(90, 10, S("ALM"));
	}

	/* 第 3 行：MQ-135 */
	OLED_ShowString(0, 20, S("MQ135"));
	show_x1(42, 20, g.mq135PpmX10);
	OLED_ShowString(78, 20, S("ppm"));
	if (g.mq135Alarm)
	{
		OLED_ShowString(108, 20, S("A"));
	}

	/* 第 4 行：温湿度（DHT11 无效时明确显示 N/A，不含糊）*/
	if (g.dhtValid)
	{
		OLED_ShowString(0, 30, S("T"));
		show_x1(12, 30, (u16)g.tempX10);
		OLED_ShowString(36, 30, S("C"));
		OLED_ShowString(48, 30, S("H"));
		show_x1(60, 30, g.humiX10);
		OLED_ShowString(84, 30, S("%"));
	}
	else
	{
		OLED_ShowString(0, 30, S("T/H  N/A"));
	}

	/* 第 5 行：Rs/R0（决策 18 的"主链路量"）—— 左 MQ-2、右 MQ-135 */
	OLED_ShowString(0, 40, S("Rs/R0"));
	show_x2(42, 40, (u32)(g.mq2Ratio * 100.0f + 0.5f));
	show_x2(78, 40, (u32)(g.mq135Ratio * 100.0f + 0.5f));

	/* 第 6 行：R0 是否已标定 + 温湿度补偿是否开启 */
	OLED_ShowString(0, 50, (g.mq2R0 > 0.0f) ? S("CAL OK") : S("CAL..."));
	OLED_ShowString(60, 50, (g.readyBits & PROTO_RDY_COMP) ? S("COMP ON") : S("COMP OFF"));
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
		OLED_ShowString(48, 0, S(phase_text(p.phase)));

		OLED_ShowString(0, 10, S("TASK"));
		show_num(30, 10, p.taskId, 2);
		OLED_ShowString(54, 10, S("PROG"));
		show_num(84, 10, p.progress, 3);
		OLED_ShowString(102, 10, S("%"));

		OLED_ShowString(0, 20, S("WP"));
		show_num(18, 20, p.waypoint, 1);
		OLED_ShowString(30, 20, S("STOP"));
		show_num(60, 20, p.stopReason, 2);
		if (p.safetyStop)
		{
			OLED_ShowString(90, 20, S("LOCK"));
		}
	}

	if (App_GetDiag(&d) == 0)
	{
		/* 两条链路各收到多少字节、丢了多少（诊断环形缓冲是否够用）*/
		OLED_ShowString(0, 30, S("BT rx"));
		show_num(36, 30, d.rxBytesBt, 5);
		OLED_ShowString(0, 40, S("WR rx"));
		show_num(36, 40, d.rxBytesWire, 5);
		OLED_ShowString(72, 40, S("dr"));
		show_num(90, 40, d.rxDroppedWire, 3);

		/* 解帧成功数 + 剩余堆（KB）—— 排障两件套 */
		OLED_ShowString(0, 50, S("FR"));
		show_num(18, 50, d.framesOk, 5);
		OLED_ShowString(54, 50, S("Hp"));
		show_num(72, 50, d.heapFreeBytes / 1024u, 4);
		OLED_ShowString(102, 50, S("K"));
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
