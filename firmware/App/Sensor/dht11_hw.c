#include "dht11_hw.h"
#include "sys.h"
#include "delay.h"

/*
 * DHT11 时序层 —— 数据引脚 PA7（底板 J9.3，R14 4.7k 上拉到 3.3V）
 * 见 dht11_hw.h 的时序说明。
 *
 * 说明：这里用厂商 delay.c 的 delay_us()/delay_ms()。
 * ⚠️ P4 起 delay_us 会改用 DWT 计数（因为 SysTick 要交给 FreeRTOS），
 *    届时本文件无需改动 —— 只要 delay_us 的语义仍是"忙等 N 微秒"。
 */

#define DHT11_PORT   GPIOA
#define DHT11_PIN    GPIO_Pin_7

/* 每个等待阶段的超时上限（微秒），防止传感器不在时死等 */
#define DHT11_WAIT_US 200u

static void dht11_pin_out(void)
{
	GPIO_InitTypeDef gi;
	gi.GPIO_Pin = DHT11_PIN;
	gi.GPIO_Mode = GPIO_Mode_OUT;
	gi.GPIO_OType = GPIO_OType_PP;
	gi.GPIO_Speed = GPIO_Speed_50MHz;
	gi.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(DHT11_PORT, &gi);
}

static void dht11_pin_in(void)
{
	GPIO_InitTypeDef gi;
	gi.GPIO_Pin = DHT11_PIN;
	gi.GPIO_Mode = GPIO_Mode_IN;
	gi.GPIO_OType = GPIO_OType_PP;
	gi.GPIO_Speed = GPIO_Speed_50MHz;
	gi.GPIO_PuPd = GPIO_PuPd_UP;    /* 上拉（外部已有 4.7k，这里再开内部上拉更稳） */
	GPIO_Init(DHT11_PORT, &gi);
}

/* ---------------------------------------------------------------------------
 * 时序临界段：响应握手 + 40 位数据
 * ★ B 2026-10-13 新增（审计发现"会错"级缺陷后的修复）：
 *   DHT11 用微秒级脉宽编码，而本项目的控制中断（EXTI15_10，优先级 1）每 5 ms 抢占一次，
 *   落在位窗口里就必然把数据打错 → 校验失败 → 反复重试上百毫秒，`T/H` 长期 N/A。
 *   这里把**这一段**放进关中断区，并且**所有出口（含超时/错误）都恢复中断**——
 *   之所以单独做成一个函数：原来用 `goto` 跳出去，很容易漏掉恢复那一步（漏了就是死机）。
 *   注意：**只包这一段**。外层的 20 ms 起始脉冲与 30 ms 重试间隔必须留在开中断状态，
 *   因为调度器启动后 `delay_ms()` 走的是 `vTaskDelay()`，在关中断时调用它是非法用法。
 * -------------------------------------------------------------------------*/
static int dht11_wait_and_read(uint8_t raw[5])
{
	uint8_t i, b;
	int rc = 0;
	uint32_t prim = __get_PRIMASK();

	__disable_irq();

	/* ---- 等响应：低 80 µs + 高 80 µs ---- */
	{
		uint16_t t = 0;
		while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
		{
			if (++t > DHT11_WAIT_US) { rc = -10; goto done; }
			delay_us(1);
		}
		t = 0;
		while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == 0)
		{
			if (++t > DHT11_WAIT_US) { rc = -10; goto done; }
			delay_us(1);
		}
		t = 0;
		while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
		{
			if (++t > DHT11_WAIT_US) { rc = -10; goto done; }
			delay_us(1);
		}
	}

	/* ---- 读 40 bit：每位先低 50 µs，再按高电平长度判 0/1 ---- */
	for (i = 0; i < 5; i++)
	{
		raw[i] = 0;
	}
	for (b = 0; b < 40; b++)
	{
		uint16_t t = 0;

		/* 等这位的低电平结束 */
		while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == 0)
		{
			if (++t > DHT11_WAIT_US) { rc = -11; goto done; }
			delay_us(1);
		}
		/* 低电平结束后 ~40 µs 采样：仍为高 → 1 */
		delay_us(40);
		if (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
		{
			raw[b / 8] |= (uint8_t)(0x80u >> (b % 8));
			/* 等这位的高电平结束，准备下一位 */
			t = 0;
			while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
			{
				if (++t > DHT11_WAIT_US) { rc = -11; goto done; }
				delay_us(1);
			}
		}
	}

done:
	if (prim == 0)
	{
		__enable_irq();          /* 只在"进函数前中断本来是开的"时才恢复，避免破坏嵌套语义 */
	}
	return rc;
}

int Dht11_Hw_Read(Dht11Reading *out)
{
	uint8_t raw[5];
	int retry;
	int rc = -10;

	/* 数据引脚时钟（PA 组；与 IIC/OLED 等同组，重复使能无副作用） */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

	/* ★ B 2026-10-13 修正（上板联调发现的真缺陷）：DHT11 用**微秒级脉宽**编码，
	 * 而本项目的 5 ms/10 ms 控制中断（EXTI15_10，优先级 1）每 5 ms 抢占一次 ——
	 * 一旦落在位采样窗口里，时序立刻错乱 → 校验失败 → 在"3 次重试 + 20/30 ms 等待"里
	 * 空转上百毫秒，`T/H` 长期显示 N/A（正是首次上板看到的现象）。
	 *
	 * 修法（**关键是"屏蔽的范围与时机"，不能简单整段关中断**）：
	 *   ① 调用方（GasTask）保证**只在车静止时**才读 DHT（见 app_tasks.c 的闸门）——
	 *      这样屏蔽控制中断不会影响平衡控制；
	 *   ② 只在 **40 位数据段（约 3~4 ms）** 关全局中断：响应的 20 ms 起始脉冲与失败后的
	 *      30 ms 重试间隔**必须在开中断状态**下进行，因为 `delay_ms()` 在调度器启动后
	 *      会走 `vTaskDelay()` —— 在关中断的情况下调用它是 FreeRTOS 的非法用法；
	 *   ③ 重试次数有上限，最坏阻塞时间有界。
	 *
	 * 代价与权衡（写清楚，免得以后被当成新 bug）：这 3~4 ms 内 USART 中断也被屏蔽，
	 * 若恰好有字节到达可能丢 1~2 个字节 → 该帧被 CRC 拒收（协议本来就是 20 Hz 重发，
	 * 丢一帧无影响）；换来的是一条**能正常工作的 DHT11 通路**。*/
	for (retry = 0; retry < DHT11_HW_RETRY; retry++)
	{
		/* ---- 起始信号：拉低 ≥18 ms（开中断，delay_ms 走 vTaskDelay）---- */
		dht11_pin_out();
		GPIO_ResetBits(DHT11_PORT, DHT11_PIN);
		delay_ms(20);
		GPIO_SetBits(DHT11_PORT, DHT11_PIN);
		dht11_pin_in();

		/* ---- 响应握手 + 40 位数据：整段放入关中断的临界段（见上面的说明）---- */
		rc = dht11_wait_and_read(raw);
		if (rc != 0)
		{
			goto next;               /* 超时/时序错 → 重试（重试间隔在开中断状态下等） */
		}

		rc = Dht11_Parse(raw, out);
		if (rc == 0)
		{
			return 0;               /* 成功 */
		}
		/* 校验/范围失败 → 重试（DHT11 允许 ≥1 s 间隔，重试间隔 30 ms 足够） */
next:
		delay_ms(30);
	}

	dht11_pin_out();
	GPIO_SetBits(DHT11_PORT, DHT11_PIN);   /* 失败也把线释放为高，别把总线拉住 */
	return rc;
}
