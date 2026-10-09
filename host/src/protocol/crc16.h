#pragma once

#include <cstddef>
#include <cstdint>

namespace proto {

/// CRC-16/MODBUS：多项式 0xA001（反转），初值 0xFFFF，输入输出反射，结果不异或。
/// 与 docs/protocol/communication_protocol.md 第 2.1 节定义一致。
///
/// 标准自检：ASCII "123456789" -> 0x4B37
uint16_t crc16Modbus(const uint8_t *data, std::size_t len);

} // namespace proto
