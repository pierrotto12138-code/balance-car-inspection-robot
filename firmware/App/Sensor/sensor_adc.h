#ifndef APP_SENSOR_SENSOR_ADC_H
#define APP_SENSOR_SENSOR_ADC_H

/*
 * 三通道 ADC 采集（**STM32 硬件层**）
 *
 * 沿用厂商 Drivers/BSP/ADC/adc.c 的 ADC1 配置（非扫描、软件触发、480 周期采样），
 * 只补上本项目新增两路模拟输入（PA5/PA6）的 GPIO 模拟输入配置：
 *
 *     座（丝印）        引脚   ADC 通道   网络名        用途
 *     --------------------------------------------------------------
 *     J13（"MQ-2"）     PA5    IN5        MQ2_AO       经 R10/R11 分压后的 AO
 *     J10（"MQ-135"）   PA6    IN6        MQ135_AO     经 R12/R13 分压后的 AO
 *     （电池 PB1 走 IN9，由控制中断的 ADC1 单独读，不在这里）
 *
 * ★★ 必须分清"**硬件事实**"与"**软件指派**"（2026-10-15 真机台架修正）：
 *     · **硬件事实（不可改）**：J13 座接 PA5、J10 座接 PA6；两个座的分压网络**完全相同**
 *       （10k 串联 + 15k 对地 ⇒ ×0.600，5 V 满量程降到 3.00 V）⇒ 两路在电气上可以互换使用，
 *       软件把哪一路当 MQ-2、哪一路当 MQ-135 **不需要任何标度补偿**。
 *     · **软件指派（唯一要改的地方）**：见下面的 `SENSOR_ADC_CH_MQ2/MQ135`。
 *       当前台架**只有 MQ-135 模块，且它插在 J13**（板上丝印写的是 "MQ-2" 座），J10 是空座 ——
 *       真机实测：J13 那路 ADC 原始值 ≈131（有响应、原标定 ✔），J10 那路 ≈4（空座）。
 *       于是把 **MQ-135 角色指给 J13**，让"上位机/OLED 上的 MQ-135"真的是 MQ-135 的数据；
 *       MQ-2 角色指给空着的 J10 ⇒ 它必然报"未就绪 + 0 ppm"（固件 `gas.c` 的无效样本守卫
 *       保证空座不会编出假 ppm），而不是继续把两个角色张冠李戴。
 *     · 换座位 / 补上 MQ-2 模块后：**只改下面那两行**（其余代码、曲线、阈值都不用动）。
 *
 * ★ 为什么不用扫描 + DMA：只有 3 路、1 Hz 级的采样率，逐通道切换更简单也更好排错；
 *   而且厂商的 Get_Adc() 本来就是"每次重设通道 + 软件触发"，直接复用最不容易踩坑。
 * ★ 采样时间保持 **480 周期**（厂商原值）：MQ 分压源阻抗 ≈ 6 kΩ（10k∥15k），
 *   采样时间不够会让读数偏低（见 docs/handoff §7 B-5 的提醒）。
 * ★ 本层不参与 PC 原生测试（依赖 STM32 寄存器）；算法全在 gas.c 里。
 */

#include <stdint.h>
#include "sys.h"

/* ---- 硬件事实：座 ↔ ADC 通道（由 PCB 走线决定，**永远不要改这两行**）---- */
#define SENSOR_ADC_CH_J13   5u     /* J13 座（丝印 "MQ-2"）= PA5 = ADC_IN5 */
#define SENSOR_ADC_CH_J10   6u     /* J10 座（丝印 "MQ-135"）= PA6 = ADC_IN6 */
#define SENSOR_ADC_CH_BAT   9u     /* ADC1_IN9 = PB1（厂商 Battery_Ch） */

/* ---- 软件指派：气体角色 ↔ 座（**本台架现状**：只有 MQ-135，插在 J13）----
   2026-10-15 真机实测依据：J13 路 ADC≈131 有响应、标定就绪；J10 路 ADC≈4 为空座。
   ⇒ 让 MQ-135 走 J13，MQ-2 走空着的 J10（后者会如实报"未就绪"）。 */
#define SENSOR_ADC_CH_MQ135  SENSOR_ADC_CH_J13   /* ★ 本机 MQ-135 插在 J13 */
#define SENSOR_ADC_CH_MQ2    SENSOR_ADC_CH_J10   /* ★ 空座，等 MQ-2 补件后插这里 */

/* 初始化：调用厂商 Adc_Init() 后，把 PA5/PA6 配成模拟输入 */
void SensorAdc_Init(void);

/* 读一次原始值（0~4095）。ch 用上面的 SENSOR_ADC_CH_* */
uint16_t SensorAdc_Read(uint8_t ch);

/* 读 n 次取平均（气体读数用；n 建议 8） */
uint16_t SensorAdc_ReadAvg(uint8_t ch, uint8_t n);

#endif /* APP_SENSOR_SENSOR_ADC_H */
