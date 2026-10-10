#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""独立复算《通信协议 v1.0》§2.1 的 CRC-16/MODBUS 用例。

刻意用 Python 从零实现（不看固件/上位机代码），作为"独立方法"交叉验证：
若固件侧、上位机侧与本文三处结果一致，CRC 才可冻结。
"""


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


CASES = [
    ("算法自检（标准）", b"123456789", 0x4B37),
    ("用例 1", bytes([0x10]), 0x8CBE),
    ("用例 2", bytes([0x10, 0x0C]), 0x750C),
    ("用例 3", bytes([0x10, 0x0C, 0x00]), 0x0575),
]

if __name__ == "__main__":
    all_ok = True
    print("--- CRC-16/MODBUS 独立复算（Python 从零实现）---")
    for name, data, expect in CASES:
        got = crc16_modbus(data)
        ok = got == expect
        all_ok &= ok
        print(f"  [{'OK      ' if ok else 'MISMATCH'}] {name:<16} "
              f"输入 {data.hex(' '):<28} 期望 0x{expect:04X}  实算 0x{got:04X}")
    print("结论：" + ("协议 §2.1 四条用例全部复现 ✅" if all_ok else "存在不一致 ❌"))
