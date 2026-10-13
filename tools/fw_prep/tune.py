#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**有线调参工具**（PC 端，走板上 MicroUSB / CH340G → USART1，同协议 115200 8N1）

用途：板子到货后在这里整定 PID、观察 100 Hz 波形、把参数写进 Flash —— 不必重编译重烧录。
      也是上位机（角色 C）实现"调参界面"的参考实现。

设计要点（为什么不直接抄固件的代码）：
  · 本工具**自带一份独立实现**的 CRC-16/MODBUS 与帧编解码，并用 `docs/protocol/test_vectors.json`
    的冻结向量自检（`--selftest`）—— 三方交叉验证的"PC 侧"就是它；
  · 采集/解析与串口解耦：`--replay <文件>` 可以离线喂数据（无板子也能验证解析与统计逻辑）；
  · 只依赖 `pyserial`（可选）；没有它时除实时模式外的一切（自检 / 回放）仍可用。

常用命令（详细见 --help）：
    python tools/fw_prep/tune.py --selftest                      # 离线自检（不需要板子）
    python tools/fw_prep/tune.py -p COM5 --dump                   # 回读全部 PID 参数
    python tools/fw_prep/tune.py -p COM5 --set 3=85 --set 4=0.5   # 在线改（立即生效，回显实际值）
    python tools/fw_prep/tune.py -p COM5 --observe 20 --csv run1.csv   # 100 Hz 观测 + 统计 + CSV
    python tools/fw_prep/tune.py -p COM5 --save                   # 存 Flash；--load 从 Flash 载入
    python tools/fw_prep/tune.py -p COM5 --monitor                # 常规监视（0x10/0x11/0x12/0x14）
    python tools/fw_prep/tune.py -p COM5 --estop                  # 紧急停机（0x2F）
    python tools/fw_prep/tune.py --replay log.txt                 # 离线回放（hex 帧或原始字节）
"""
import argparse
import json
import os
import re
import statistics
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VECTORS = os.path.join(ROOT, "docs", "protocol", "test_vectors.json")

HEAD0, HEAD1 = 0xAA, 0x55

CMD_POSE, CMD_MOTION, CMD_TASK, CMD_EVENT = 0x10, 0x11, 0x12, 0x13
CMD_ENV, CMD_HEARTBEAT = 0x14, 0x1F
CMD_MOTION_DOWN, CMD_TASK_DOWN, CMD_MODE_DOWN, CMD_SURVEY, CMD_PID, CMD_ESTOP = \
    0x20, 0x21, 0x22, 0x23, 0x24, 0x2F

PID_NAMES = {
    0x01: "Balance_Kp", 0x02: "Balance_Kd", 0x03: "Velocity_Kp", 0x04: "Velocity_Ki",
    0x05: "Zhongzhi", 0x06: "Turn_Kp", 0x07: "Turn_Kd",
}
ID_DUMP_ALL, ID_SAVE, ID_LOAD, ID_TUNE_MODE = 0xFF, 0x00, 0xFE, 0xFD
GEAR_NAMES = {0: "WARMUP", 1: "NORMAL(1Hz)", 2: "FAST(5Hz)"}
PHASE_NAMES = {0: "INIT", 1: "WARMUP", 2: "IDLE", 3: "PATROL", 4: "ANALYSIS",
               5: "RETURN", 6: "DONE", 7: "FAULT"}


# --------------------------------------------------------------------------- 协议
def crc16_modbus(data, crc=0xFFFF):
    """CRC-16/MODBUS（多项式 0xA001，反射）——与固件 crc16.c 一致。"""
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def encode(cmd, payload=b""):
    """组帧：AA 55 cmd len(2, LE) data crc(2, LE)；CRC 覆盖 cmd+len+data。"""
    n = len(payload)
    body = bytes([cmd, n & 0xFF, (n >> 8) & 0xFF]) + payload
    c = crc16_modbus(body)
    return bytes([HEAD0, HEAD1]) + body + bytes([c & 0xFF, (c >> 8) & 0xFF])


def f32(v):
    return struct.pack("<f", v)


class Parser:
    """流式解帧状态机（与固件 protocol.c / host codec.cpp 语义一致：100 ms 单帧超时）。"""

    def __init__(self, timeout_ms=100):
        self.buf = bytearray()
        self.timeout_ms = timeout_ms
        self.frames_ok = 0
        self.frames_bad = 0
        self.bytes_in = 0
        self._last = None

    def feed_bytes(self, data, now=None):
        now = time.monotonic() * 1000.0 if now is None else now
        out = []
        for b in data:
            self.bytes_in += 1
            out.extend(self._feed_one(b, now))
            if out:
                now = time.monotonic() * 1000.0   # 简单处理：一帧出栈后重新计时
        return out

    def _feed_one(self, b, now):
        if self._last is not None and (now - self._last) > self.timeout_ms:
            self.buf.clear()                       # 单帧接收超时 → 复位
        self._last = now
        self.buf.append(b)
        while True:
            # 帧头对齐
            while len(self.buf) >= 1 and self.buf[0] != HEAD0:
                self.buf.pop(0)
            if len(self.buf) >= 2 and self.buf[1] != HEAD1:
                if self.buf[1] == HEAD0:
                    self.buf.pop(0)                # AA AA 55 … 兼容
                else:
                    self.buf.pop(0)
                continue
            if len(self.buf) < 5:
                return []
            cmd = self.buf[2]
            n = self.buf[3] | (self.buf[4] << 8)
            if n > 256:
                self.buf.clear()
                self.frames_bad += 1
                return []
            if len(self.buf) < 7 + n:
                return []
            body = bytes(self.buf[2:5 + n])
            crc_rx = self.buf[5 + n] | (self.buf[6 + n] << 8)
            if crc16_modbus(body) != crc_rx:
                self.frames_bad += 1
                if self.buf[5] == HEAD0 or self.buf[2] == HEAD0:
                    del self.buf[0:5]              # 丢整帧但保住可能的下一帧头
                else:
                    del self.buf[0:7 + n]
                continue
            frame = {"cmd": cmd, "len": n, "data": bytes(self.buf[5:5 + n])}
            del self.buf[0:7 + n]
            self.frames_ok += 1
            return [frame]
        return []


def u16(d, o):
    return d[o] | (d[o + 1] << 8)


def s16(d, o):
    v = u16(d, o)
    return v - 65536 if v >= 32768 else v


def f32_at(d, o):
    return struct.unpack_from("<f", d, o)[0]


def describe(f):
    """把上行帧变成便于阅读的 dict。"""
    c, d = f["cmd"], f["data"]
    if c == CMD_POSE and len(d) >= 12:
        return {"kind": "pose", "pitch": s16(d, 0) / 100.0, "roll": s16(d, 2) / 100.0,
                "yaw": s16(d, 4) / 100.0, "gx": s16(d, 6) / 100.0,
                "gy": s16(d, 8) / 100.0, "gz": s16(d, 10) / 100.0}
    if c == CMD_MOTION and len(d) >= 9:
        return {"kind": "motion", "speedL": s16(d, 0), "speedR": s16(d, 2),
                "pwm": s16(d, 4), "distCm": u16(d, 6), "avoid": d[8]}
    if c == CMD_TASK and len(d) >= 5:
        return {"kind": "task", "task": d[0], "phase": d[1], "progress": d[2],
                "waypoint": d[3], "fault": d[4],
                "phaseName": PHASE_NAMES.get(d[1], "?")}
    if c == CMD_EVENT and len(d) >= 5:
        return {"kind": "event", "code": d[0], "tsMs": struct.unpack_from("<I", d, 1)[0]}
    if c == CMD_ENV and len(d) >= 16:
        return {"kind": "env", "mq2Raw": u16(d, 0), "mq135Raw": u16(d, 2),
                "mq2PpmX10": u16(d, 4), "mq135PpmX10": u16(d, 6),
                "tempX10": s16(d, 8), "humiX10": u16(d, 10),
                "ready": d[12], "gear": d[13],
                "gearName": GEAR_NAMES.get(d[13], "?")}
    if c == CMD_HEARTBEAT:
        return {"kind": "heartbeat"}
    if c == CMD_PID and len(d) >= 6:
        return {"kind": "pid", "id": d[0], "value": f32_at(d, 2),
                "name": PID_NAMES.get(d[0], "0x%02X" % d[0])}
    return {"kind": "cmd", "cmd": c, "len": f["len"], "raw": d.hex()}


# --------------------------------------------------------------------------- 离线自检
def selftest():
    """用冻结的协议向量校验本工具的编解码（不需要板子、不需要串口）。"""
    if not os.path.exists(VECTORS):
        print("缺少向量文件:", VECTORS)
        return 2
    v = json.load(open(VECTORS, encoding="utf-8"))
    crcs = v.get("crc_cases", [])
    frames = v.get("frame_cases", [])
    streams = v.get("stream_cases", [])
    if not (crcs and frames and streams):
        print("向量文件结构不符（期望 crc_cases / frame_cases / stream_cases）")
        return 2
    bad = 0

    for row in crcs:
        got = crc16_modbus(bytes.fromhex(row["input"]))
        want = row["crc"] if isinstance(row["crc"], int) else int(row["crc"], 16)
        if got != want:
            bad += 1
            print(f"  [FAIL] CRC {row['input']} 得 {got:04X} 期望 {want:04X}")
    print(f"  CRC 用例 {len(crcs)} 条，不符 {bad}")

    fbad = 0
    for row in frames:
        payload = bytes.fromhex(row.get("data", "") or "")
        cmd = row["cmd"]
        cmd = int(cmd, 16) if isinstance(cmd, str) else cmd
        got = encode(cmd, payload).hex()
        want = (row.get("frame", "") or "").lower()
        if got != want:
            fbad += 1
            print(f"  [FAIL] 帧 {row.get('name')}: 得 {got} 期望 {want}")
    print(f"  帧用例 {len(frames)} 条，不符 {fbad}")

    sbad = 0
    for row in streams:
        p = Parser()
        got = []
        for chunk in row.get("chunks", []):
            for piece in (chunk if isinstance(chunk, list) else [chunk]):
                blob = bytes.fromhex(piece) if isinstance(piece, str) else bytes(piece)
                for fr in p.feed_bytes(blob):
                    got.append(fr["cmd"])
        want = [int(x, 16) if isinstance(x, str) else x for x in row.get("expect_cmds", [])]
        if got != want:
            sbad += 1
            print(f"  [FAIL] 流式 {row.get('name')}: 得 {got} 期望 {want}")
    print(f"  流式用例 {len(streams)} 条，不符 {sbad}")

    total_bad = bad + fbad + sbad
    print("结论：" + ("本工具的编解码与冻结向量完全一致 ✅" if total_bad == 0
                     else f"共 {total_bad} 处不一致 ❌"))
    return 0 if total_bad == 0 else 1


# --------------------------------------------------------------------------- 串口
def open_port(port, baud, timeout=0.05):
    try:
        import serial                                    # noqa: F401
    except ImportError:
        print("未安装 pyserial。装一个即可（离线自检/回放不需要它）：\n"
              "    python -m pip install pyserial")
        raise SystemExit(2)
    import serial
    ser = serial.Serial(port, baud, timeout=timeout)
    ser.reset_input_buffer()
    return ser


class Link:
    """带解析与"等指定命令"的小封装（pyserial / 回放通用）。"""

    def __init__(self, ser=None):
        self.ser = ser
        self.p = Parser()
        self.log = []

    def send(self, cmd, payload=b""):
        if self.ser:
            self.ser.write(encode(cmd, payload))
            self.ser.flush()

    def pump(self, seconds=0.05):
        """读一段数据并返回解码后的帧列表（dict 形式）。"""
        out = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.ser:
                n = self.ser.in_waiting
                chunk = self.ser.read(n if n else 1)
            else:
                chunk = b""
            if chunk:
                out.extend(describe(f) for f in self.p.feed_bytes(chunk))
        return out


def cmd_dump(link, wait=0.5):
    link.send(CMD_PID, bytes([ID_DUMP_ALL, 0]) + f32(0.0))
    got = {}
    end = time.monotonic() + wait
    while time.monotonic() < end and len(got) < len(PID_NAMES):
        for d in link.pump(0.05):
            if d["kind"] == "pid" and d["id"] in PID_NAMES:
                got[d["id"]] = d["value"]
    print("当前 PID 参数（实际生效值）：")
    for pid in sorted(PID_NAMES):
        if pid in got:
            print(f"  0x{pid:02X} {PID_NAMES[pid]:<12} = {got[pid]:g}")
    if len(got) < len(PID_NAMES):
        print(f"  ⚠️ 只收到 {len(got)}/{len(PID_NAMES)} 条，检查链路/波特率")
    return got


def cmd_set(link, assigns):
    for a in assigns:
        pid_s, val_s = a.split("=", 1)
        pid = int(pid_s, 0)
        val = float(val_s)
        link.send(CMD_PID, bytes([pid & 0xFF, 0]) + f32(val))
        echo = None
        end = time.monotonic() + 0.4
        while time.monotonic() < end and echo is None:
            for d in link.pump(0.03):
                if d["kind"] == "pid" and d["id"] == (pid & 0xFF):
                    echo = d
        name = PID_NAMES.get(pid & 0xFF, "0x%02X" % (pid & 0xFF))
        if echo:
            print(f"  {name}: 请求 {val:g} → 实际 {echo['value']:g} ✅")
        else:
            print(f"  {name}: 请求 {val:g} → 无回显（链路问题？）")


def cmd_simple(link, cmd, payload=b"", expect=CMD_PID, wait=0.6, label=""):
    link.send(cmd, payload)
    got = None
    end = time.monotonic() + wait
    while time.monotonic() < end and got is None:
        for d in link.pump(0.05):
            if d["kind"] == "pid" and expect == CMD_PID:
                got = d
    if got:
        ok = (got["value"] == 0.0)
        print(f"  {label}: 返回 {got['value']:g} " + ("（成功 ✅）" if ok else "（失败 ❌）"))
    else:
        print(f"  {label}: 无回复")


def cmd_observe(link, seconds, csv_path, quiet=False):
    """调参观测模式：进 100 Hz、采样、统计、退模式。"""
    print("进入调参观测模式（有线链路 100 Hz）…")
    link.send(CMD_PID, bytes([ID_TUNE_MODE, 0]) + f32(1.0))
    link.pump(0.3)

    rows, t0 = [], time.monotonic()
    csv_f = open(csv_path, "w", encoding="utf-8") if csv_path else None
    if csv_f:
        csv_f.write("t_s,pitch_deg,gx_dps,gz_dps,speedL_mms,speedR_mms,pwm,dist_cm\n")
    pitch = []
    last_print = 0.0
    try:
        while time.monotonic() - t0 < seconds:
            for d in link.pump(0.05):
                t = time.monotonic() - t0
                if d["kind"] == "pose":
                    pose = d
                elif d["kind"] == "motion":
                    row = (t, pose.get("pitch", 0.0) if 'pose' in dir() else 0.0,
                           pose.get("gx", 0.0) if 'pose' in dir() else 0.0,
                           pose.get("gz", 0.0) if 'pose' in dir() else 0.0,
                           d["speedL"], d["speedR"], d["pwm"], d["distCm"])
                    rows.append(row)
                    pitch.append(row[1])
                    if csv_f:
                        csv_f.write("%.4f,%.2f,%.2f,%.2f,%d,%d,%d,%d\n" % row)
                    if not quiet and t - last_print >= 0.5:
                        last_print = t
                        print(f"  t={t:5.2f}s pitch={row[1]:7.2f}° gyro={row[2]:8.2f} "
                              f"spdL={row[4]:5d} spdR={row[5]:5d} pwm={row[6]:5d} d={row[7]:4d}cm")
                    if len(rows) >= 60000:
                        break
            if len(rows) >= 60000:
                break
    except KeyboardInterrupt:
        print("\n（用户中断）")
    finally:
        if csv_f:
            csv_f.close()
        link.send(CMD_PID, bytes([ID_TUNE_MODE, 0]) + f32(0.0))
        link.pump(0.2)
        print("已退出调参观测模式")

    dur = max(time.monotonic() - t0, 1e-6)
    print(f"\n采集 {len(rows)} 帧 / {dur:.2f}s ≈ {len(rows)/dur:.1f} Hz"
          + (f"，已写 {csv_path}" if csv_path else ""))
    if pitch:
        # 这几个统计量就是"调 PID 时该看的数"：抖动幅度看 std，振荡看零穿越率
        zc = sum(1 for i in range(1, len(pitch)) if (pitch[i-1] - pitch[i]) * (pitch[i] - pitch[i+1]) < 0)
        print("姿态统计（用于判断超调/振荡）：")
        print(f"  pitch  均值 {statistics.fmean(pitch):7.2f}°  "
              f"最小 {min(pitch):7.2f}°  最大 {max(pitch):7.2f}°  "
              f"标准差 {statistics.pstdev(pitch):5.2f}°")
        print(f"  峰峰值 {max(pitch)-min(pitch):6.2f}°   零穿越 {zc} 次 "
              f"（≈{zc/dur:.1f} Hz 振荡频率，越大越可能在抖）")


def replay(path, csv_path=None, limit=None):
    """离线回放：文件里可以是每行一个 hex 帧、或每行一段原始字节 hex、或原始二进制。"""
    raw = open(path, "rb").read()
    text = None
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        pass

    p = Parser()
    frames = []
    if text is not None and re.search(r"[0-9a-fA-F]{6,}", text):
        for line in text.splitlines():
            line = line.split("#")[0].strip().replace(" ", "")
            if not line:
                continue
            try:
                blob = bytes.fromhex(line)
            except ValueError:
                continue
            frames.extend(describe(f) for f in p.feed_bytes(blob))
            if limit and len(frames) >= limit:
                break
    else:
        frames = [describe(f) for f in p.feed_bytes(raw)]

    print(f"回放 {os.path.relpath(path, ROOT)}：解析出 {len(frames)} 帧"
          f"（坏帧 {p.frames_bad}）")
    if csv_path:
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write("idx,kind,pitch,gx,gz,speedL,speedR,pwm,dist\n")
            for i, d in enumerate(frames):
                if d["kind"] == "motion":
                    f.write(f"{i},motion,,,{d['speedL']},{d['speedR']},{d['pwm']},{d['distCm']}\n")
        print("已写", csv_path)
    for d in frames[:10]:
        print("  ", d)
    if len(frames) > 10:
        print(f"  … 其余 {len(frames)-10} 帧省略")
    return 0


def monitor(link, seconds):
    t0 = time.monotonic()
    last = {}
    print("监视中（Ctrl+C 结束）…")
    try:
        while time.monotonic() - t0 < (seconds or 1e9):
            for d in link.pump(0.1):
                k = d["kind"]
                if k == "pose":
                    last["pose"] = d
                elif k == "motion":
                    last["motion"] = d
                elif k == "env":
                    print(f"  ENV  MQ2 {d['mq2PpmX10']/10:6.1f}ppm  MQ135 {d['mq135PpmX10']/10:6.1f}ppm"
                          f"  T {d['tempX10']/10:5.1f}C  H {d['humiX10']/10:5.1f}%  {d['gearName']}")
                elif k == "task":
                    print(f"  TASK stage={d['phaseName']} task={d['task']} prog={d['progress']}% "
                          f"wp={d['waypoint']} fault=0x{d['fault']:02X}")
                elif k == "event":
                    print(f"  ⚠️ EVENT code=0x{d['code']:02X} ts={d['tsMs']}ms")
                elif k == "heartbeat":
                    last["hb"] = time.monotonic()
            if "pose" in last and "motion" in last:
                po, mo = last["pose"], last["motion"]
                print(f"  pose pitch={po['pitch']:7.2f}° gx={po['gx']:8.2f} gz={po['gz']:8.2f} | "
                      f"spd {mo['speedL']:5d}/{mo['speedR']:5d} pwm={mo['pwm']:5d} d={mo['distCm']}cm")
    except KeyboardInterrupt:
        print("\n结束")


def main():
    ap = argparse.ArgumentParser(description="有线调参工具（USART1，同协议 115200）",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("-p", "--port", help="串口，例如 COM5（Windows）或 /dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--selftest", action="store_true", help="离线自检（用冻结向量校验编解码）")
    ap.add_argument("--replay", metavar="FILE", help="离线回放日志（不需要板子）")
    ap.add_argument("--csv", metavar="FILE", help="把观测/回放数据写成 CSV")
    ap.add_argument("--dump", action="store_true", help="回读全部 PID 参数")
    ap.add_argument("--set", action="append", default=[], metavar="ID=VALUE",
                    help="在线改参数（可重复），ID 见协议 §4.2（0x01~0x07）")
    ap.add_argument("--save", action="store_true", help="把当前参数存入 Flash")
    ap.add_argument("--load", action="store_true", help="从 Flash 重新载入参数")
    ap.add_argument("--observe", type=float, metavar="SECONDS", help="100 Hz 观测（并统计）")
    ap.add_argument("--monitor", type=float, nargs="?", const=0, metavar="SECONDS",
                    help="常规监视 0x10/0x11/0x12/0x14")
    ap.add_argument("--estop", action="store_true", help="紧急停机（0x2F）")
    ap.add_argument("--start", action="store_true", help="下发 0x21 任务=自主巡航（解除安全停车）")
    ap.add_argument("--stop", action="store_true", help="下发 0x20 使能=0（停止运动）")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.replay:
        return replay(args.replay, args.csv)

    if not args.port:
        ap.print_help()
        print("\n提示：--selftest / --replay 不需要板子；实时模式需要 -p COMx")
        return 2

    link = Link(open_port(args.port, args.baud))
    print(f"已连接 {args.port} @ {args.baud}（有线链路，协议同蓝牙）")
    time.sleep(0.2)

    did = False
    if args.estop:
        link.send(CMD_ESTOP)
        print("已下发紧急停机 0x2F（安全停车锁存，需 0x21 任务才能解除）")
        did = True
    if args.start:
        link.send(CMD_TASK_DOWN, bytes([0x01, 0x00]))
        print("已下发 0x21 任务=自主巡航")
        did = True
    if args.stop:
        link.send(CMD_MOTION_DOWN, struct.pack("<hhB", 0, 0, 0))
        print("已下发 0x20 使能=0")
        did = True
    if args.set:
        cmd_set(link, args.set)
        did = True
    if args.save:
        cmd_simple(link, CMD_PID, bytes([ID_SAVE, 0]) + f32(0.0), label="存 Flash")
        did = True
    if args.load:
        cmd_simple(link, CMD_PID, bytes([ID_LOAD, 0]) + f32(0.0), label="从 Flash 载入")
        did = True
    if args.dump:
        cmd_dump(link)
        did = True
    if args.observe:
        cmd_observe(link, args.observe, args.csv)
        did = True
    if args.monitor is not None:
        monitor(link, args.monitor)
        did = True

    if not did:
        cmd_dump(link)
    return 0


if __name__ == "__main__":
    sys.exit(main())
