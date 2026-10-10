#ifndef __SENSOR_REPORT_H
#define __SENSOR_REPORT_H

/*
 * 传感器 → 协议载荷 的适配层（**纯算法，可在 PC 上测**）
 *
 * 把 GasChannel / Dht11Reading 组装成协议 0x14「环境与气体」帧的 16 字节数据段。
 *
 * ★ 必须做**饱和钳位**：ppm = a·ratio^b 是幂律，ratio 很小时 ppm 会爆掉
 *   （例如 MQ-2 的 b=−2.222，ratio=0.01 时 ppm 可达千万级），
 *   而协议字段是 uint16（ppm×10，上限 6553.5 ppm）。不钳位就会回绕成小数字，
 *   上位机看到的将是"浓度很低"——正好把危险情况报反。
 */

#include <stdint.h>
#include "gas.h"
#include "dht11.h"
#include "protocol.h"

/* ppm×10 的协议上限（uint16） */
#define SENSOR_PPM_X10_MAX 65535u

/* 把浮点 ppm 转成协议用的 ppm×10，带饱和钳位 */
uint16_t SensorReport_PpmToX10(float ppm);

/*
 * 组装 0x14 数据段。
 *   mq2/mq135 ：两路通道（用其 rs / ppm / r0 判断就绪）
 *   dht       ：DHT11 读数（dhtValid=0 时忽略其内容）
 *   compEnable：温湿度补偿是否启用（写进就绪位 bit3）
 *   gear      ：采样档位（PROTO_GEAR_*）
 */
void SensorReport_FillEnvGas(const GasChannel *mq2, const GasChannel *mq135,
                             const Dht11Reading *dht, int dhtValid,
                             int compEnable, uint8_t gear,
                             ProtoEnvGas *out);

/*
 * 采样档位决策：
 *   warmupDone=0            → 预热中
 *   adaptive=0              → 恒为常态 1 Hz
 *   任一通道报警 / 连续超阈值 → 异常 5 Hz
 *   否则                    → 常态
 */
uint8_t SensorReport_DecideGear(const GasChannel *mq2, const GasChannel *mq135,
                                int warmupDone, int adaptive);

#endif /* __SENSOR_REPORT_H */
