#ifndef __SENSOR_ADC_H
#define __SENSOR_ADC_H

/*
 * 三通道 ADC 采集（**STM32 硬件层**）
 *
 * 沿用厂商 Drivers/BSP/ADC/adc.c 的 ADC1 配置（非扫描、软件触发、480 周期采样），
 * 只补上本项目新增两路模拟输入（PA5/PA6）的 GPIO 模拟输入配置：
 *
 *     通道            引脚      ADC1 通道    用途
 *     ----------------------------------------------------------
 *     PB1             PB1       IN9        电池电压（厂商原有，不动）
 *     MQ-2 分压点      PA5       IN5        经 R10/R11 分压后的 MQ-2 AO
 *     MQ-135 分压点    PA6       IN6        经 R12/R13 分压后的 MQ-135 AO
 *
 * ★ 为什么不用扫描 + DMA：只有 3 路、1 Hz 级的采样率，逐通道切换更简单也更好排错；
 *   而且厂商的 Get_Adc() 本来就是"每次重设通道 + 软件触发"，直接复用最不容易踩坑。
 * ★ 采样时间保持 **480 周期**（厂商原值）：MQ 分压源阻抗 ≈ 6 kΩ（10k∥15k），
 *   采样时间不够会让读数偏低（见 docs/handoff §7 B-5 的提醒）。
 * ★ 本层不参与 PC 原生测试（依赖 STM32 寄存器）；算法全在 gas.c 里。
 */

#include <stdint.h>
#include "sys.h"

#define SENSOR_ADC_CH_MQ2    5u     /* ADC1_IN5  = PA5 */
#define SENSOR_ADC_CH_MQ135  6u     /* ADC1_IN6  = PA6 */
#define SENSOR_ADC_CH_BAT    9u     /* ADC1_IN9  = PB1（厂商 Battery_Ch） */

/* 初始化：调用厂商 Adc_Init() 后，把 PA5/PA6 配成模拟输入 */
void SensorAdc_Init(void);

/* 读一次原始值（0~4095）。ch 用上面的 SENSOR_ADC_CH_* */
uint16_t SensorAdc_Read(uint8_t ch);

/* 读 n 次取平均（气体读数用；n 建议 8） */
uint16_t SensorAdc_ReadAvg(uint8_t ch, uint8_t n);

#endif /* __SENSOR_ADC_H */
