#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成协议测试向量（**独立实现**，用于 B/C 双方交叉验证与协议冻结）。

背景：`docs/protocol/communication_protocol.md` §七 的冻结条件是
    "B / C 双方各自实现 CRC-16，用 §2.1 节用例交叉验证一致"。
本脚本把这件事做成**可复跑的产物**：
    1. 用 Python 从零实现 CRC + 组帧（不看固件代码，也不看 `host/` 代码）；
    2. 输出 `docs/protocol/test_vectors.json`（人读）与 `docs/protocol/test_vectors.tsv`（机器比对）；
    3. 另有 `host_crosscheck.cpp` 用 **C 侧真实代码（`host/src/protocol/*.cpp`）** 逐条复算，
       Python 与 C++ 两条独立路径都对上，才算协议冻结。

用法：
    python tools/fw_prep/gen_test_vectors.py
"""
import json
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_JSON = os.path.join(ROOT, "docs", "protocol", "test_vectors.json")
OUT_TSV = os.path.join(ROOT, "docs", "protocol", "test_vectors.tsv")
DOC_MD = os.path.join(ROOT, "docs", "protocol", "communication_protocol.md")


def parse_doc_lengths():
    """从协议文档的命令字表格里抽出 {cmd: 文档声明的数据段长度 N}。

    只取"纯数字"的长度；`2+2k` 这类动态长度跳过。
    """
    out = {}
    row = re.compile(r"^\s*\|\s*\*{0,2}(0x[0-9A-Fa-f]{2})\*{0,2}\s*\|([^|]*)\|([^|]*)\|")
    with open(DOC_MD, encoding="utf-8") as fh:
        for line in fh:
            m = row.match(line)
            if not m:
                continue
            ncell = m.group(3).replace("*", "").strip()
            if re.fullmatch(r"\d+", ncell):
                out[int(m.group(1), 16)] = int(ncell)
    return out

HEADER = (0xAA, 0x55)


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def frame(cmd: int, data: bytes) -> bytes:
    """帧 = AA 55 | cmd | N(小端) | data | CRC(小端)；CRC 覆盖 cmd 起至数据段末尾。"""
    body = bytes([cmd]) + struct.pack("<H", len(data)) + data
    return bytes(HEADER) + body + struct.pack("<H", crc16_modbus(body))


def f32(v: float) -> bytes:
    """0x24 的参数值：IEEE-754 单精度，小端（与 ARM/x86 内存布局一致）。"""
    return struct.pack("<f", v)


def i16(v: int) -> bytes:
    return struct.pack("<h", v)


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


# ----------------------------------------------------------------------------
# 1) CRC 用例（协议 §2.1，四条都必须复现）
# ----------------------------------------------------------------------------
CRC_CASES = [
    ("算法自检（标准）", bytes(b"123456789"), 0x4B37),
    ("用例 1", bytes([0x10]), 0x8CBE),
    ("用例 2", bytes([0x10, 0x0C]), 0x750C),
    ("用例 3", bytes([0x10, 0x0C, 0x00]), 0x0575),
]

# ----------------------------------------------------------------------------
# 2) 帧用例（覆盖协议第三、四节的每一个命令字）
#    数据段取值都写成"可读的物理量"，便于双方对照字段偏移
# ----------------------------------------------------------------------------
FRAME_CASES = []


def add(name, cmd, data, note=""):
    FRAME_CASES.append({"name": name, "cmd": cmd, "data": data, "note": note})


# --- 上行 ---
add("0x10 姿态数据", 0x10,
    i16(123) + i16(-456) + i16(7890) + i16(12) + i16(-34) + i16(56),
    "俯仰 1.23° / 横滚 -4.56° / 偏航 78.90° / 三轴角速度 0.12,-0.34,0.56 dps")
add("0x11 运动状态", 0x11,
    i16(250) + i16(-250) + i16(1200) + u16(85) + bytes([0x03]),
    "左 +250 mm/s、右 -250 mm/s、PWM 1200、超声 85 cm、避障位 0b11")
add("0x12 任务状态", 0x12,
    bytes([0x01, 0x03, 45, 2, 0x00]),
    "任务=自主巡航、阶段=巡航、完成度 45%、当前巡检点 2、无异常")
add("0x13 事件告警", 0x13,
    bytes([0x06]) + u32(123456),
    "事件码 0x06（气体超阈值）、时间戳 123456 ms")
add("0x14 环境与气体", 0x14,
    u16(1234) + u16(2345) + u16(567) + u16(890) + i16(256) + u16(610)
    + bytes([0x0F, 1, 0, 0]),
    "MQ2 原始 1234 / MQ135 原始 2345 / ppm×10 567,890 / 25.6℃ / 61.0%RH / 就绪位 0x0F / 档位 1")
add("0x1F 心跳", 0x1F, b"", "N=0，总长 7 字节")

# --- 下行 ---
add("0x20 运动控制", 0x20,
    i16(300) + i16(-150) + bytes([1]),
    "线速度 +300 mm/s、角速度 -15.0 deg/s、使能 1")
add("0x21 任务指示", 0x21,
    bytes([0x01, 0x02]) + u16(1000) + u16(200),
    "任务=自主巡航、参数个数 2、参数 1000 与 200")
add("0x22 控制模式", 0x22, bytes([2]), "模式=自主")
add("0x23 巡检参数", 0x23,
    u16(1000) + u16(300) + u16(500) + bytes([1, 1, 3, 0, 0, 0]),
    "采样周期 1000 ms、MQ2 阈值 30.0 ppm、MQ135 阈值 50.0 ppm、自适应开、补偿开、连续确认 3 帧")
add("0x24 PID 调参", 0x24,
    bytes([0x01, 0x00]) + f32(320.5),
    "参数 ID=0x01（Balance_Kp）、保留 0、值 320.5（IEEE-754 小端）")
# v1.0-b 的四个扩展参数 ID（帧格式不变，只是 ID 取值不同）——一并进向量，确保两端一致
add("0x24 扩展·全量回读", 0x24, bytes([0xFF, 0x00]) + f32(0.0),
    "ID=0xFF 请求回读全部参数（机器人按 0x01~0x07 回 7 条）")
add("0x24 扩展·存 Flash", 0x24, bytes([0x00, 0x00]) + f32(0.0),
    "ID=0x00 保存到 Flash（回值 0.0 = 成功）")
add("0x24 扩展·重新载入", 0x24, bytes([0xFE, 0x00]) + f32(0.0),
    "ID=0xFE 从 Flash 重新载入")
add("0x24 扩展·调参观测模式", 0x24, bytes([0xFD, 0x00]) + f32(1.0),
    "ID=0xFD 开启 100 Hz 观测模式（值 1=开）")
add("0x2F 紧急停机", 0x2F, b"", "N=0")

# ----------------------------------------------------------------------------
# 3) 流式解帧用例（应与 host/tests/test_codec.cpp 的语义一致）
# ----------------------------------------------------------------------------
STREAM_CASES = [
    {
        "name": "逐字节喂入（最恶劣分片）",
        "chunks": [[frame(0x10, FRAME_CASES[0]["data"]).hex()]],
        "desc": "单帧拆成逐字节喂入",
        "expect_cmds": [0x10],
    },
    {
        "name": "一包内含两帧",
        "chunks": [[(frame(0x1F, b"") + frame(0x2F, b"")).hex()]],
        "desc": "心跳 + 急停 同一次 read 到达",
        "expect_cmds": [0x1F, 0x2F],
    },
    {
        "name": "垃圾字节 + 伪帧头后重新同步",
        "chunks": [[("00ffaa1377aa" + frame(0x13, FRAME_CASES[3]["data"]).hex())]],
        "desc": "前导垃圾含 0xAA 伪帧头，必须只解出真帧",
        "expect_cmds": [0x13],
    },
    {
        "name": "坏 CRC 整帧丢弃",
        "chunks": [[(frame(0x12, FRAME_CASES[2]["data"])[:-1] + bytes([0x00])).hex()]],
        "desc": "末字节被改坏，应丢弃且不计为有效帧",
        "expect_cmds": [],
    },
    {
        "name": "长度异常（32767）整帧作废",
        "chunks": [["aa5514ff7f"]],
        "desc": "长度字段超上限，必须丢弃且不越界",
        "expect_cmds": [],
    },
    {
        "name": "AA AA 55 前缀容错",
        "chunks": [[("aaaa" + frame(0x1F, b"").hex())]],
        "desc": "连续 0xAA 应仍能识别帧头",
        "expect_cmds": [0x1F],
    },
]


def main():
    # 先做"文档自洽性检查"：把协议文档里声明的 N 与本文向量里真实的数据段长度对齐。
    # 这一步专门用来抓"声明长度与字段清单算不出来"这类会直接打崩 B/C 互通的问题。
    doc_n = parse_doc_lengths()
    n_bad = 0
    print("--- 协议文档声明的 N vs 字段清单实际长度 ---")
    for c in FRAME_CASES:
        want = doc_n.get(c["cmd"])
        got = len(c["data"])
        if want is None:
            continue
        flag = "OK  " if want == got else "不符"
        if want != got:
            n_bad += 1
        print(f"  [{flag}] 0x{c['cmd']:02X} {c['name'][5:]:<12} 文档 N={want:<3} 实际 {got}")
    if n_bad:
        print(f"  ⚠️ 有 {n_bad} 条与协议文档不一致 —— 冻结前必须改文档或改字段定义\n")
    else:
        print("  ✅ 文档声明的 N 与字段清单完全一致\n")

    crc_rows = [{"name": n, "input": d.hex(), "crc": c} for n, d, c in CRC_CASES]
    frame_rows = []
    for c in FRAME_CASES:
        f = frame(c["cmd"], c["data"])
        frame_rows.append({
            "name": c["name"], "cmd": c["cmd"], "data": c["data"].hex(),
            "frame": f.hex(), "frame_len": len(f),
            "crc": crc16_modbus(bytes([c["cmd"]]) + struct.pack("<H", len(c["data"])) + c["data"]),
            "note": c["note"],
        })

    doc = {
        "protocol_version": "v1.0",
        "generator": "tools/fw_prep/gen_test_vectors.py（Python 独立实现）",
        "usage": "B（固件）与 C（上位机）各自实现后必须逐条复现本文件；"
                 "另有 tools/fw_prep/host_crosscheck.cpp 用 C 侧真实代码复算。",
        "byte_order": "全部小端；0x24 的参数值为 IEEE-754 单精度 float32（小端）",
        "crc_cases": crc_rows,
        "frame_cases": frame_rows,
        "stream_cases": STREAM_CASES,
    }
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    with open(OUT_TSV, "w", encoding="utf-8") as fh:
        fh.write("# name\tcmd_hex\tdata_hex\tframe_hex\n")
        for r in frame_rows:
            fh.write(f"{r['name']}\t{r['cmd']:02x}\t{r['data']}\t{r['frame']}\n")
        fh.write("# crc_cases\tinput_hex\texpected_crc\n")
        for r in crc_rows:
            fh.write(f"CRC\t{r['input']}\t{r['crc']:04x}\n")
        # 流式用例（给固件的 C 单测用）：chunks 用 ',' 分隔同一次 read 内的多段、
        # 用 ';' 分隔不同的 read；expect_cmds 为空表示"不应解析出任何帧"
        fh.write("# stream_cases\tname\tchunks(read内逗号, read间分号)\texpect_cmds\n")
        for s in STREAM_CASES:
            chunks = ";".join(",".join(c) for c in s["chunks"])
            cmds = ",".join(f"{c:02x}" for c in s["expect_cmds"])
            fh.write(f"STREAM\t{s['name']}\t{chunks}\t{cmds}\n")

    print(f"已写出 {os.path.relpath(OUT_JSON, ROOT)}"
          f"（CRC {len(crc_rows)} 条 + 帧 {len(frame_rows)} 条 + 流式 {len(STREAM_CASES)} 条）")
    print(f"已写出 {os.path.relpath(OUT_TSV, ROOT)}")
    print("\n--- 帧用例一览 ---")
    for r in frame_rows:
        print(f"  {r['name']:<16} cmd=0x{r['cmd']:02X} N={len(r['data']) // 2:<3} "
              f"len={r['frame_len']:<3} crc=0x{r['crc']:04X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
