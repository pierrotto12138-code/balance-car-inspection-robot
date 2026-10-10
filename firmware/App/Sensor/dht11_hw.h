#ifndef __DHT11_HW_H
#define __DHT11_HW_H

/*
 * DHT11 单总线时序层（**STM32 硬件层**）
 *
 * 接法（底板 `J9`）：1=GND，2=+3.3V，3=数据 → **PA7**，数据线上有 R14 4.7k 上拉到 3.3V。
 *
 * 时序（DHT11 datasheet）：
 *   1. MCU 拉低 ≥18 ms 作为起始信号，然后释放（输出高/输入上拉）
 *   2. DHT 响应：低 80 µs + 高 80 µs
 *   3. 之后 40 bit，每位以 50 µs 低电平开始：
 *        高电平 26~28 µs = 0，高电平 70 µs = 1
 *   4. 采样点取在每位低电平结束后的 ~40 µs 处
 *
 * ★ 风险与对策（已在 firmware/README 的"已知问题"里登记）：
 *   · 整次读取约 20 ms，期间会被 5 ms/10 ms 的控制中断打断 → **校验失败就重试**，
 *     最多 DHT11_HW_RETRY 次；仍失败则本周期不上报温湿度（readyBits 位清 0），
 *     不用"猜"的值去补偿气体（决策 18 只允许在数据有效时才启用补偿）。
 *   · 必须在**任务上下文**里调用（P4 之后由 GasTask 调用），不要在中断里读。
 */

#include <stdint.h>
#include "dht11.h"

#define DHT11_HW_RETRY 3

/* 读取一次并解析。返回 dht11.h 里 Dht11_Parse() 的返回码（0=成功），
   -10 = 无响应（传感器未接/上电未稳定），-11 = 位采样超时 */
int Dht11_Hw_Read(Dht11Reading *out);

#endif /* __DHT11_HW_H */
