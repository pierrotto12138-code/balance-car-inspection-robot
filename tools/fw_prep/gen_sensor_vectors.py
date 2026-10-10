#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成**传感器算法测试向量**（Python 独立实现 → CSV），供固件侧逐条比对。

为什么要有它（AGENTS 铁律 23）：
    gas.c 里的换算是"分压 → Rs → Rs/R0 → 补偿 → ppm"多级串联，错一级数值就偏，
    而板上很难判断"到底偏在哪一级"。这里用**另一套独立实现**（不引用固件代码）
    把每一级的中间量都算出来，固件侧逐级比对，偏在哪一级一目了然。

输出：firmware/tests/sensor_vectors.csv
    GAS,adc_raw,temp_c,humi_rh,comp,vao_mv,rs_ohm,ratio,ratio_corr,ppm
    CORR,temp_c,humi_rh,factor
    PARSE,hex,expect_rc,humi_int,humi_dec,temp_int,temp_dec
    ALARM,ppm_series_csv,thresh_x10,confirm,expect_rising_flags

用法：
    python tools/fw_prep/gen_sensor_vectors.py
"""
import os
import random

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "firmware", "tests", "sensor_vectors.csv")

# ── 与固件 gas.c 的默认配置保持一致（MQ-2 / MQ-135）──────────────────────────
CH = {
    "mq2": dict(rl=10000.0, vcc=5000.0, vref=3300.0, rtop=10000.0, rbot=15000.0,
                clean=9.83, a=574.25, b=-2.222),
    "mq135": dict(rl=10000.0, vcc=5000.0, vref=3300.0, rtop=10000.0, rbot=15000.0,
                  clean=3.6, a=102.2, b=-2.473),
}

ADC_FS = 4095.0


def vao_mv(c, raw):
    """ADC 原始值 → 反分压后的 MQ AO 电压（mV）"""
    vadc = raw * c["vref"] / ADC_FS
    return vadc * (c["rtop"] + c["rbot"]) / c["rbot"]


def rs_ohm(c, vao):
    if vao <= 0:
        return 0.0
    rs = c["vcc"] * c["rl"] / vao - c["rl"]
    return max(rs, 0.0)


def r0_from_clean_air(c, raw):
    """清洁空气标定：R0 = Rs_air / (Rs/R0)_clean"""
    return rs_ohm(c, vao_mv(c, raw)) / c["clean"]


def corr_factor(t, h):
    return 0.00035 * t * t - 0.02718 * t + 1.39538 - (h - 33.0) * 0.0018


def main():
    random.seed(20261009)
    rows = []

    # ── 1) 气体换算：覆盖量程两端 + 随机点（两个通道都要） ────────────────
    adcs = [0, 1, 8, 64, 256, 600, 1024, 1500, 2048, 2400, 3000, 3500, 4000, 4095]
    adcs += [random.randint(1, 4094) for _ in range(14)]
    for name, c in CH.items():
        clean_adc = 2600                     # 假设清洁空气时 ADC=2600（到货后按实测改）
        r0 = r0_from_clean_air(c, clean_adc)
        # 标定用例：把同一批 ADC 喂给 Gas_CalibrateAdd，最后应得到同一个 R0
        calib_adcs = [clean_adc - 20, clean_adc, clean_adc + 20]
        calib_r0 = sum(r0_from_clean_air(c, a) for a in calib_adcs) / len(calib_adcs)
        rows.append(("CALIB", f"{name},{calib_r0:.6f}," +
                              ",".join(str(a) for a in calib_adcs)))
        for raw in adcs:
            for (t, h, comp) in ((25.0, 50.0, 1), (19.37, 33.0, 1), (5.0, 90.0, 1),
                                 (40.0, 10.0, 1), (25.0, 50.0, 0)):
                v = vao_mv(c, raw)
                r = rs_ohm(c, v)
                ratio = r / r0
                rc = ratio / corr_factor(t, h) if comp else ratio
                ppm = c["a"] * (rc ** c["b"]) if rc > 0 else 0.0
                rows.append(("GAS", f"{name},{raw},{t:g},{h:g},{comp},"
                                    f"{r0:.6f},{v:.6f},{r:.6f},{ratio:.9f},{rc:.9f},{ppm:.6f}"))

        # ── 1b) 滑动平均窗口（8 点）：逐步喂入，逐次校验平均值 ────────────
        seq = [1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200]
        avgs = []
        win = []
        for raw in seq:
            ratio = rs_ohm(c, vao_mv(c, raw)) / r0
            win.append(ratio)
            if len(win) > 8:
                win.pop(0)
            avgs.append(sum(win) / len(win))
        rows.append(("AVG", f"{name},{r0:.6f}," +
                            ",".join(str(a) for a in seq) + "," +
                            ",".join(f"{a:.9f}" for a in avgs)))

    # ── 2) 补偿系数单独列（含"参考点≈1.0"这个关键性质） ────────────────────
    for (t, h) in ((19.3714, 33.0), (20.0, 33.0), (25.0, 50.0), (0.0, 0.0),
                   (50.0, 90.0), (-10.0, 20.0), (35.5, 65.0)):
        rows.append(("CORR", f"{t:g},{h:g},{corr_factor(t, h):.9f}"))

    # ── 3) DHT11 解析：正常 / 校验错 / 全 0 / 全 FF / 超范围 ───────────────
    def dht(humi, temp, bad=False, dec=(0, 0)):
        s = (humi + dec[0] + temp + dec[1]) & 0xFF
        if bad:
            s ^= 0x5A
        return bytes([humi, dec[0], temp, dec[1], s])

    cases = [
        (dht(45, 25), 0, 45, 0, 25, 0),
        (dht(90, 50), 0, 90, 0, 50, 0),
        (dht(20, 0), 0, 20, 0, 0, 0),
        (dht(45, 25, bad=True), -1, 0, 0, 0, 0),
        (bytes([0, 0, 0, 0, 0]), -3, 0, 0, 0, 0),
        (bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF]), -3, 0, 0, 0, 0),
        (dht(101, 25), -2, 0, 0, 0, 0),          # 湿度超量程
        (dht(50, 61), -2, 0, 0, 0, 0),           # 温度超量程
        (dht(0, 30), 0, 0, 0, 30, 0),            # 湿度为 0 是合法读数（不是"全 0"故障）
    ]
    for raw, rc, hi, hd, ti, td in cases:
        rows.append(("PARSE", f"{raw.hex()},{rc},{hi},{hd},{ti},{td}"))

    # ── 4) 阈值研判：多帧确认 + 90% 迟滞 ──────────────────────────────────
    alarm_cases = [
        # (ppm 序列, 阈值×10, 确认帧数)
        ([1.0, 2.0, 30.0, 31.0, 32.0, 33.0, 34.0], 300, 3),   # 第 3 帧确认（=阈值不算超）
        ([1.0, 2.0, 30.1, 30.2, 30.3], 300, 3),
        ([40.0, 40.0, 40.0, 28.0, 26.0, 24.0, 40.0, 40.0, 40.0], 300, 3),  # 迟滞：28>27 不解除
        ([40.0, 40.0, 40.0, 20.0, 19.0, 40.0, 40.0, 40.0], 300, 3),        # 掉到 <27 解除后再报
        ([50.0], 300, 1),                                     # 单帧确认
    ]
    for series, thresh, confirm in alarm_cases:
        # 独立实现：超阈值计数 + 上升沿标志 + 90% 迟滞
        over, alarm, flags = 0, False, []
        for ppm in series:
            rising = 0
            if ppm > thresh / 10.0:
                over += 1
                if not alarm and over >= confirm:
                    alarm = True
                    rising = 1
            else:
                over = 0
                if alarm and ppm < (thresh / 10.0) * 0.9:
                    alarm = False
            flags.append(rising)
        # 注意：ppm 序列与 flags 内部用 ';' 分隔（',' 是 CSV 字段分隔符，不能混用）
        rows.append(("ALARM", f"{';'.join(f'{p:g}' for p in series)},{thresh},{confirm},"
                              f"{';'.join(str(f) for f in flags)}"))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# 传感器算法测试向量（由 tools/fw_prep/gen_sensor_vectors.py 的 Python 独立实现生成）\n")
        fh.write("# 固件侧由 firmware/tests/sensor_test.c 逐条比对（tools/fw_prep/run_sensor_test.py 驱动）\n")
        fh.write("# 格式：KIND,payload...\n")
        for kind, payload in rows:
            fh.write(f"{kind},{payload}\n")

    n = {k: sum(1 for r in rows if r[0] == k) for k in ("GAS", "CORR", "PARSE", "ALARM", "CALIB", "AVG")}
    print(f"已写出 {os.path.relpath(OUT, ROOT)}：GAS {n['GAS']}、CORR {n['CORR']}、PARSE {n['PARSE']}、"
          f"ALARM {n['ALARM']}、CALIB {n['CALIB']}、AVG {n['AVG']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
