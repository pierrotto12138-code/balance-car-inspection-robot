#include "buzzer.h"
#include "sys.h"

/*
 * 蜂鸣器：PC5，**低电平响**（有源模块，决策 26）。
 * 上电时把引脚置为高（不响），避免上电瞬间乱叫。
 */

#define BUZZER_PORT  GPIOC
#define BUZZER_PIN   GPIO_Pin_5

void Buzzer_Init(void)
{
	GPIO_InitTypeDef gi;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
	gi.GPIO_Pin = BUZZER_PIN;
	gi.GPIO_Mode = GPIO_Mode_OUT;
	gi.GPIO_OType = GPIO_OType_PP;
	gi.GPIO_Speed = GPIO_Speed_50MHz;
	gi.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(BUZZER_PORT, &gi);

	Buzzer_Off();
}

void Buzzer_On(void)
{
	GPIO_ResetBits(BUZZER_PORT, BUZZER_PIN);     /* 低电平 = 响 */
}

void Buzzer_Off(void)
{
	GPIO_SetBits(BUZZER_PORT, BUZZER_PIN);
}

void Buzzer_Set(int on)
{
	if (on)
	{
		Buzzer_On();
	}
	else
	{
		Buzzer_Off();
	}
}
