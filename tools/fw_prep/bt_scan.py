#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""蓝牙/串口链路自动识别 —— 找出"哪个 COM 口是我们的设备"。

为什么需要它（2026-10-13 真机联调）：
  Windows 的蓝牙 SPP 口**没有规律可循**：配对后端口号随机、名字可能只是 "COM5"、
  设备管理器里还常与"标准串行 over 蓝牙链接"混在一起；而我们的设备**同时有两条链路**
  （有线 CH340 + 蓝牙 SPP），靠肉眼猜很容易发错口、白折腾。
  ⇒ 判据不该是"端口叫什么"，而该是"**这个口上有没有我们的合法帧**"。
  本脚本就按这个判据扫一遍：逐个打开、显式驱动 DTR/RTS（否则 MCU 会被按在
  BootLoader 里，见 AGENTS 陷阱 33）、读几秒、数 AA 55 帧头与**通过 CRC 的整帧**。

用法：
    python tools\\fw_prep\\bt_scan.py                 # 只列端口（含 VID/PID/描述）
    python tools\\fw_prep\\bt_scan.py --probe 3       # 每个口读 3 秒，报"是不是我们的设备"
    python tools\\fw_prep\\bt_scan.py --probe 3 --baud 115200
"""
import argparse
import sys
import time

BAUD = 115200


def list_ports():
    try:
        import serial.tools.list_ports as lp
    except ImportError:
        print("未安装 pyserial：python -m pip install pyserial")
        raise SystemExit(2)
    return list(lp.comports())


def kind_of(p):
    """粗分端口类型：蓝牙 / USB 转串口 / 其它。判据以 HWID 与描述为准。"""
    s = f"{p.description or ''} {p.hwid or ''} {p.manufacturer or ''}".upper()
    if "BTHENUM" in s or "BLUETOOTH" in s or "蓝牙" in s:
        return "蓝牙"
    if any(k in s for k in ("CH340", "CP210", "FTDI", "USB-SERIAL", "USB VID")):
        return "USB串口"
    return "?"


def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def count_frames(buf):
    """数 AA 55 帧头，以及**长度+CRC 都对**的整帧（后者才证明是我们的设备）。
    帧格式（与 tune.py 的 encode() 一致，见协议文档 §2）：
        AA 55 | cmd | len(2, 小端) | data… | crc(2, 小端)，CRC 覆盖 cmd+len+data。
    ★ 这里曾写错过：把 byte2 当长度、把 AA 55 也算进 CRC → 结果"帧头一堆、合法帧 0"。
      教训：判据要照参考实现抄，不要凭印象手写（本项目已多次栽在"自写解析器"上）。"""
    heads = 0
    good = 0
    i = 0
    while i + 7 <= len(buf):
        if buf[i] == 0xAA and buf[i + 1] == 0x55:
            heads += 1
            n = buf[i + 3] | (buf[i + 4] << 8)
            end = i + 5 + n + 2
            if end <= len(buf):
                body = buf[i + 2:i + 5 + n]          # cmd + len(2) + data
                crc = buf[end - 2] | (buf[end - 1] << 8)
                if crc16_modbus(body) == crc:
                    good += 1
                    i = end
                    continue
        i += 1
    return heads, good


def probe(port, secs, baud):
    import serial
    try:
        s = serial.Serial(port, baud, timeout=0.1)
    except Exception as e:
        return None, f"打不开（{e}）"
    try:
        # ★ 显式驱动 DTR/RTS：本板 CH340 的自动下载电路把 DTR 接复位、RTS 接 BOOT0，
        #   不驱动就会把 MCU 按在 BootLoader 里 —— 现象是"一个字节都收不到"（AGENTS 陷阱 33）
        s.dtr = True
        s.rts = False
        time.sleep(0.2)
        s.reset_input_buffer()
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < secs:
            buf += s.read(4096)
        heads, good = count_frames(bytes(buf))
        return (len(buf), heads, good), None
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="找出哪个串口是我们的设备（按合法帧判定）")
    ap.add_argument("--probe", type=float, default=0, help="每个口读几秒（0 = 只列端口）")
    ap.add_argument("--baud", type=int, default=BAUD)
    args = ap.parse_args()

    ports = list_ports()
    if not ports:
        print("没有发现任何串口 —— 板子没插？蓝牙没配对？")
        return 1

    print(f"发现 {len(ports)} 个串口：\n")
    for p in ports:
        print(f"  {p.device:8} [{kind_of(p)}]  {p.description or ''}")
        if p.vid is not None:
            print(f"           VID:PID = {p.vid:04X}:{p.pid:04X}   {p.hwid or ''}")

    if args.probe <= 0:
        print("\n提示：加 --probe 3 让脚本逐个口读 3 秒，直接告诉你哪个口是我们的设备。")
        return 0

    print(f"\n逐个探测（每个口读 {args.probe:g} s @ {args.baud}，已显式驱动 DTR=高/RTS=低）：\n")
    hits = []
    for p in ports:
        r, err = probe(p.device, args.probe, args.baud)
        tag = kind_of(p)
        if err:
            print(f"  {p.device:8} [{tag}]  {err}（多半被别的程序占用：FlyMcu / 串口助手 / 上位机）")
            continue
        nbytes, heads, good = r
        verdict = "★ 是我们的设备（CRC 校验通过）" if good else ("有帧头但 CRC 没过" if heads else "无数据")
        print(f"  {p.device:8} [{tag}]  收到 {nbytes} B，AA55 帧头 {heads}，合法帧 {good}  → {verdict}")
        if good:
            hits.append((p.device, tag, good))

    print()
    if not hits:
        print("❌ 没有口收到合法帧。依次检查：① 固件在跑（蓝 LED 是否在闪）② DTR/RTS"
              "③ 蓝牙是否已配对 ④ 波特率是否 115200 ⑤ 端口是否被占用")
        return 1
    for dev, tag, good in hits:
        print(f"✅ {dev} [{tag}]：{good} 个合法帧 —— 用它即可（有线口与蓝牙口都会列出）")
    if len(hits) > 1:
        print("   两条链路都在：可同时用两个口，验证双链路并发与"
              "『回帧只回请求方』（协议 §5）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
