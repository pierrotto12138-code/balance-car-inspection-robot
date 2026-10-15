#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""蓝牙模块（HC-05/06 及其克隆）AT 配置工具。

为什么要有它：Windows 下的蓝牙模块配置**没有统一入口**——芯片型号不同、命令方言不同
（老式 HC-06 是 `AT+NAME<名字>`/`AT+PIN<密码>`/`AT+BAUD<码>`；新式是 `AT+NAME=…`/
`AT+PSWD=…`/`AT+UART=…`），而且**进 AT 模式的波特率与通信波特率往往不是一个**
（本模块实测：通信默认 9600，AT 也在 9600 应答）。靠串口助手手敲极易敲错。
本工具会自动探波特率、自动试两种方言、改完**回读确认**。

用法：
    python tools\\fw_prep\\bt_at.py --port COM3 --show
    python tools\\fw_prep\\bt_at.py --port COM3 --name PierrotBalance --pswd 6667
    python tools\\fw_prep\\bt_at.py --port COM3 --baud 115200     # 改通信波特率（最后做）

★ 注意：改完通信波特率后，AT 会话本身就是新波特率，需用新波特率再确认。
"""
import argparse
import sys
import time

PROBE_BAUDS = [9600, 38400, 115200, 57600, 19200, 4800]


def _open(port, baud):
    import serial
    s = serial.Serial(port, baud, timeout=0.3)
    time.sleep(0.35)
    # 有些模块用 DTR/RTS 兼作 KEY/复位，配置时都不拉更安全
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    return s


def at(s, cmd, wait=0.6):
    s.reset_input_buffer()
    s.write((cmd + "\r\n").encode("ascii", "ignore"))
    time.sleep(wait)
    return s.read(300).decode("ascii", "replace").replace("\r", "|").replace("\n", "|").strip()


def find_baud(port, log=print):
    """找一个能让模块回 OK 的波特率。"""
    import serial
    for b in PROBE_BAUDS:
        try:
            s = _open(port, b)
        except Exception as e:
            log(f"  {b:6} 打不开：{e}")
            continue
        r = at(s, "AT")
        log(f"  {b:6}  AT -> {r!r}")
        if "OK" in r.upper():
            return s, b
        s.close()
    return None, None


def try_cmds(s, cmds, log=print):
    """依次尝试同一语义的多种方言，返回 (成功与否, 最后一次响应)。"""
    last = ""
    for c in cmds:
        r = at(s, c)
        log(f"    {c:24} -> {r!r}")
        last = r
        if "OK" in r.upper():
            return True, r
        if "ERROR" in r.upper():
            continue
    return False, last


def main():
    ap = argparse.ArgumentParser(description="蓝牙模块 AT 配置")
    ap.add_argument("--port", required=True)
    ap.add_argument("--show", action="store_true", help="只读当前配置")
    ap.add_argument("--name", default=None)
    ap.add_argument("--pswd", default=None)
    ap.add_argument("--baud", type=int, default=None, help="通信波特率（最后改，改完要按新波特率重连）")
    a = ap.parse_args()

    if a.pswd is not None and not (a.pswd.isdigit() and len(a.pswd) == 4):
        print("密码必须是 4 位数字（手机/Windows 配对的要求）")
        return 2

    s, baud = find_baud(a.port)
    if s is None:
        print("\n❌ 没有任何波特率能得到 OK。检查：TX/RX 是否交叉、GND 是否共地、"
              "模块是否已进 AT 模式（有的需上电时拉高 KEY 脚/按住按键）")
        return 1
    print(f"\n✅ 找到 AT 通道：{a.port} @ {baud}\n")

    print("当前配置：")
    for c in ("AT+VERSION?", "AT+NAME?", "AT+PSWD?", "AT+UART?", "AT+ROLE?", "AT+ADDR?"):
        print(f"  {c:14} -> {at(s, c)!r}")

    if a.show:
        s.close()
        return 0

    if a.name:
        print(f"\n设置名字 → {a.name}")
        ok, _ = try_cmds(s, [f"AT+NAME{a.name}", f"AT+NAME={a.name}", f"AT+NAME={a.name},0"])
        print(f"  {'成功' if ok else '未确认成功'}（回读：{at(s, 'AT+NAME?')!r}）")

    if a.pswd:
        print(f"\n设置配对密码 → {a.pswd}")
        ok, _ = try_cmds(s, [f"AT+PSWD{a.pswd}", f"AT+PSWD={a.pswd}", f"AT+PIN{a.pswd}"])
        print(f"  {'成功' if ok else '未确认成功'}（回读：{at(s, 'AT+PSWD?')!r}）")

    if a.baud:
        print(f"\n设置通信波特率 → {a.baud}（★ 改完本会话就失效，需按新波特率重连）")
        code = {1200: 1, 2400: 2, 4800: 3, 9600: 4, 19200: 5, 38400: 6, 57600: 7, 115200: 8}.get(a.baud)
        cmds = [f"AT+UART={a.baud},0,0", f"AT+BAUD{code}"] if code else [f"AT+UART={a.baud},0,0"]
        ok, _ = try_cmds(s, cmds)
        print(f"  {'成功' if ok else '未确认成功'}")

    s.close()
    print("\n收尾验证：重新以 9600/新波特率各发一次 AT，确认模块仍应答。")
    for b in ([baud] if not a.baud else [a.baud, baud]):
        try:
            s2 = _open(a.port, b)
            print(f"  {b:6}  AT -> {at(s2, 'AT')!r}")
            s2.close()
        except Exception as e:
            print(f"  {b:6}  失败：{e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
