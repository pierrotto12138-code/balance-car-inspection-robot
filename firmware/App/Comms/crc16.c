#include "crc16.h"

/*
 * CRC-16/MODBUS —— 与上位机 host/src/protocol/crc16.cpp **逐位等价**，
 * 并与 docs/protocol/test_vectors.json 的 4 条用例对齐（由 proto_test.c 逐条自测）。
 */
uint16_t crc16_modbus_update(uint16_t crc, const uint8_t *buf, uint16_t len)
{
	uint16_t i;
	uint8_t bit;

	for (i = 0; i < len; i++)
	{
		crc ^= (uint16_t)buf[i];
		for (bit = 0; bit < 8; bit++)
		{
			if (crc & 0x0001)
			{
				crc = (uint16_t)((crc >> 1) ^ 0xA001);
			}
			else
			{
				crc = (uint16_t)(crc >> 1);
			}
		}
	}
	return crc;
}

uint16_t crc16_modbus(const uint8_t *buf, uint16_t len)
{
	return crc16_modbus_update(0xFFFF, buf, len);
}
