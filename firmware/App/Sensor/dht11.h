#ifndef __DHT11_H
#define __DHT11_H

/*
 * DHT11 单总线温湿度传感器 —— **纯解析层**（无 ST 头文件，可在 PC 上测）
 *
 * DHT11 一次测量返回 40 bit（5 字节）：
 *     [0] 湿度整数  [1] 湿度小数  [2] 温度整数  [3] 温度小数  [4] 校验和
 *   校验和 = (前 4 字节之和) & 0xFF
 *
 * 硬件时序（拉低 18 ms、等待响应、逐位读）在 dht11_hw.c 里；本文件只负责
 * "把 5 个字节变成有意义的读数，并判断是否可信"，这样校验逻辑可以离线回归。
 *
 * ★ 本工程用途：给 MQ 气体传感器的温湿度补偿提供输入（决策 13/18）。
 */

#include <stdint.h>

typedef struct
{
	uint8_t  humiInt;    /* %RH 整数部分 */
	uint8_t  humiDec;    /* %RH 小数部分（DHT11 恒为 0） */
	uint8_t  tempInt;    /* ℃ 整数部分 */
	uint8_t  tempDec;    /* ℃ 小数部分（DHT11 恒为 0） */
} Dht11Reading;

/*
 * 解析 40 bit 原始数据。
 *   返回 0  成功
 *   返回 -1 校验和错误（数据不可信，**必须丢弃**）
 *   返回 -2 物理量超范围（湿度 >100 %RH 或温度 >60 ℃，视为异常）
 *   返回 -3 全 0 / 全 0xFF（典型的总线未接或上电未稳定）
 */
int Dht11_Parse(const uint8_t raw[5], Dht11Reading *out);

/* 便捷：温度（℃）、湿度（%RH）转成浮点 */
float Dht11_TempC(const Dht11Reading *r);
float Dht11_HumiRh(const Dht11Reading *r);

#endif /* __DHT11_H */
