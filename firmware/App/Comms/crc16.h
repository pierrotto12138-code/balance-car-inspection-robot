#ifndef APP_COMMS_CRC16_H
#define APP_COMMS_CRC16_H

/*
 * CRC-16/MODBUS（协议 §2.1）
 *
 * ★ 本文件**只依赖 <stdint.h>**，不包含任何 ST 头文件 ——
 *   这样同一份代码既能在 STM32 上编译，也能在 PC 上用 gcc 编译做单元测试
 *   （见 firmware/tests/proto_test.c 与 tools/fw_prep/run_proto_test.py）。
 *
 * 参数：多项式 0xA001（0x8005 的反转形式）、初值 0xFFFF、输入/输出反射、结果不异或。
 * 协议 §2.1 用例：ASCII "123456789" → 0x4B37。
 */

#include <stdint.h>

/* 一次性计算（初值 0xFFFF） */
uint16_t crc16_modbus(const uint8_t *buf, uint16_t len);

/*
 * 增量计算：把 crc 当作"上一段的余数"继续算下一段。
 * 用于解帧时**不额外占缓冲区**地校验「命令字 + 长度 + 数据段」：
 *     crc = crc16_modbus_update(0xFFFF, head, 3);
 *     crc = crc16_modbus_update(crc,    data, need);
 * 与一次性计算的等价性由 proto_test.c 断言。
 */
uint16_t crc16_modbus_update(uint16_t crc, const uint8_t *buf, uint16_t len);

#endif /* APP_COMMS_CRC16_H */
