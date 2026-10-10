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

int Dht11_Hw_Read(Dht11Reading *out)
{
	uint8_t raw[5];
	uint8_t i, b;
	int retry;
	int rc = -10;

	/* 数据引脚时钟（PA 组；与 IIC/OLED 等同组，重复使能无副作用） */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

	for (retry = 0; retry < DHT11_HW_RETRY; retry++)
	{
		/* ---- 起始信号：拉低 ≥18 ms ---- */
		dht11_pin_out();
		GPIO_ResetBits(DHT11_PORT, DHT11_PIN);
		delay_ms(20);
		GPIO_SetBits(DHT11_PORT, DHT11_PIN);
		dht11_pin_in();

		/* ---- 等响应：低 80 µs + 高 80 µs ---- */
		{
			uint16_t t = 0;
			while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
			{
				if (++t > DHT11_WAIT_US) { rc = -10; goto next; }
				delay_us(1);
			}
			t = 0;
			while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == 0)
			{
				if (++t > DHT11_WAIT_US) { rc = -10; goto next; }
				delay_us(1);
			}
			t = 0;
			while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) != 0)
			{
				if (++t > DHT11_WAIT_US) { rc = -10; goto next; }
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
				if (++t > DHT11_WAIT_US) { rc = -11; goto next; }
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
					if (++t > DHT11_WAIT_US) { rc = -11; goto next; }
					delay_us(1);
				}
			}
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
