#include "sensor_adc.h"
#include "app_config.h"
#include "adc.h"

/*
 * 见 sensor_adc.h。★ P4 关键改动（决策 43）：气体两路改用 **ADC2**。
 *
 * 为什么必须分开：
 *   厂商的电池采样 `Get_battery_volt()` 是在 **5 ms/10 ms 控制中断里**跑的，
 *   它每次都会重设 ADC1 的通道并软件触发（`Get_Adc()`）。如果 GasTask 也在任务里
 *   用 ADC1 读 PA5/PA6，两者会互相打断：中断改了通道 → 任务的转换结果属于另一个通道，
 *   而且 `Get_Adc()` 里"等 EOC"的循环也会被对方吃掉标志。
 *   任务侧的临界区**挡不住**控制中断（它的优先级高于 FreeRTOS 的屏蔽阈值），
 *   所以唯一干净的办法是**用另一个 ADC**：
 *       ADC1 → PB1（电池，控制中断专用，保持原样）
 *       ADC2 → PA5 / PA6（气体，任务专用）
 *   PA5/PA6 同时挂在 ADC1 和 ADC2 上（ADC12_IN5 / ADC12_IN6），所以硬件上完全可行。
 */

static void adc2_init(void)
{
	ADC_CommonInitTypeDef common;
	ADC_InitTypeDef init;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC2, ENABLE);
	RCC_APB2PeriphResetCmd(RCC_APB2Periph_ADC2, ENABLE);
	RCC_APB2PeriphResetCmd(RCC_APB2Periph_ADC2, DISABLE);

	/* 与厂商对 ADC1 的公共配置保持一致（独立模式 / 4 分频 → ADCCLK = 21 MHz） */
	common.ADC_Mode = ADC_Mode_Independent;
	common.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
	common.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;
	common.ADC_Prescaler = ADC_Prescaler_Div4;
	ADC_CommonInit(&common);

	init.ADC_Resolution = ADC_Resolution_12b;
	init.ADC_ScanConvMode = DISABLE;              /* 非扫描：逐通道手动切 */
	init.ADC_ContinuousConvMode = DISABLE;        /* 单次转换 */
	init.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
	init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_T1_CC1;
	init.ADC_DataAlign = ADC_DataAlign_Right;
	init.ADC_NbrOfConversion = 1;
	ADC_Init(ADC2, &init);
	ADC_Cmd(ADC2, ENABLE);
}

void SensorAdc_Init(void)
{
	GPIO_InitTypeDef gpio_common;

	/* 厂商原有：ADC1 + PB1（电池电压，控制中断里用） */
	Adc_Init();

	/* 新增：PA5 / PA6 模拟输入（MQ-2 / MQ-135 分压点） */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
	gpio_common.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6;
	gpio_common.GPIO_Mode = GPIO_Mode_AN;         /* 模拟输入 */
	gpio_common.GPIO_PuPd = GPIO_PuPd_NOPULL;     /* 模拟输入不要上下拉 */
	GPIO_Init(GPIOA, &gpio_common);

	/* 气体两路走 ADC2（与中断里的 ADC1 彻底分开） */
	adc2_init();
}

uint16_t SensorAdc_Read(uint8_t ch)
{
	if (ch == SENSOR_ADC_CH_BAT)
	{
		return Get_Adc(ch);          /* 电池：沿用厂商 ADC1 路径（中断也在用，一致） */
	}

	/* 气体：ADC2，采样时间保持 480 周期（源阻抗 ≈ 6 kΩ，见 sensor_adc.h） */
	ADC_RegularChannelConfig(ADC2, ch, 1, ADC_SampleTime_480Cycles);
	ADC_SoftwareStartConv(ADC2);
	{
		uint32_t guard = 0;
		while (ADC_GetFlagStatus(ADC2, ADC_FLAG_EOC) == RESET)
		{
			if (++guard > CFG_ADC_CONV_GUARD)      /* 指针异常时不死等 */
			{
				return 0;
			}
		}
	}
	return ADC_GetConversionValue(ADC2);
}

uint16_t SensorAdc_ReadAvg(uint8_t ch, uint8_t n)
{
	uint32_t sum = 0;
	uint8_t i;

	if (n == 0)
	{
		n = 1;
	}
	for (i = 0; i < n; i++)
	{
		sum += SensorAdc_Read(ch);
	}
	return (uint16_t)(sum / n);
}
