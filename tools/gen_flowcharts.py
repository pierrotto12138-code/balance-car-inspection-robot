# -*- coding: utf-8 -*-
"""生成 B（固件）程序设计说明文档用的流程图/框图 —— 纯 PIL 绘制，只用宋体/黑体，不依赖 graphviz。

产物：docs/design_firmware/图/流程图_*.png（同时可拷进提交包）

用法：python tools/gen_flowcharts.py [--out <目录>]
"""
import os, sys, argparse
from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_SONG = r"C:\Windows\Fonts\simsun.ttc"
FONT_HEI = r"C:\Windows\Fonts\simhei.ttf"
SS = 2                      # 超采样倍数（画完再缩，线条更干净）


def F(size, bold=False):
    path = FONT_HEI if bold else FONT_SONG
    return ImageFont.truetype(path, size * SS, index=0)


class Dia:
    """极简流程图绘制器：盒子 + 菱形 + 箭头（自动从边中点出发）。"""

    def __init__(self, title, w=1180, h=820):
        self.w, self.h = w, h
        self.im = Image.new("RGB", (w * SS, h * SS), "white")
        self.d = ImageDraw.Draw(self.im)
        self.nodes = {}
        self.title = title

    # ---------- 基本图元
    def text_center(self, box, text, size=19, bold=False, color="#111111", wrap=None):
        x0, y0, x1, y1 = box
        f = F(size, bold)
        lines = []
        for raw in text.split("\n"):
            if wrap and len(raw) > wrap:
                cur = ""
                for ch in raw:
                    cur += ch
                    if len(cur) >= wrap:
                        lines.append(cur); cur = ""
                if cur: lines.append(cur)
            else:
                lines.append(raw)
        lh = (f.getbbox("汉")[3] - f.getbbox("汉")[1]) * 1.35
        total = lh * len(lines)
        y = (y0 + y1) / 2 * SS - total / 2
        for ln in lines:
            bb = self.d.textbbox((0, 0), ln, font=f)
            x = (x0 + x1) / 2 * SS - (bb[2] - bb[0]) / 2
            self.d.text((x, y), ln, font=f, fill=color)
            y += lh

    def box(self, name, x, y, w, h, text, fill="#EAF2FB", edge="#2F6FB2", size=19, bold=False, wrap=None):
        box = (x, y, x + w, y + h)
        self.d.rectangle([c * SS for c in box], fill=fill, outline=edge, width=2 * SS)
        self.text_center(box, text, size, bold, wrap=wrap)
        self.nodes[name] = box
        return box

    def round_box(self, name, x, y, w, h, text, fill="#E8F5E9", edge="#2E7D32", size=19, bold=False, wrap=None):
        box = (x, y, x + w, y + h)
        self.d.rounded_rectangle([c * SS for c in box], radius=14 * SS, fill=fill, outline=edge, width=2 * SS)
        self.text_center(box, text, size, bold, wrap=wrap)
        self.nodes[name] = box
        return box

    def diamond(self, name, cx, cy, w, h, text, fill="#FFF6E5", edge="#B26A00", size=19, wrap=None):
        box = (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
        pts = [((cx) * SS, (cy - h / 2) * SS), ((cx + w / 2) * SS, cy * SS),
               (cx * SS, (cy + h / 2) * SS), ((cx - w / 2) * SS, cy * SS)]
        self.d.polygon(pts, fill=fill, outline=edge, width=2 * SS)
        self.text_center(box, text, size, wrap=wrap)
        self.nodes[name] = box
        return box

    # ---------- 连线
    def _arrowhead(self, p, direction):
        x, y = p[0] * SS, p[1] * SS
        s = 7 * SS
        if direction == "down":  pts = [(x, y), (x - s / 1.6, y - s), (x + s / 1.6, y - s)]
        elif direction == "up":  pts = [(x, y), (x - s / 1.6, y + s), (x + s / 1.6, y + s)]
        elif direction == "right": pts = [(x, y), (x - s, y - s / 1.6), (x - s, y + s / 1.6)]
        else: pts = [(x, y), (x + s, y - s / 1.6), (x + s, y + s / 1.6)]
        self.d.polygon(pts, fill="#333333")

    def link(self, a, b, label="", color="#333333", style="v", label_side="right", width=2):
        ax0, ay0, ax1, ay1 = self.nodes[a]
        bx0, by0, bx1, by1 = self.nodes[b]
        w = width * SS
        if style == "v":                     # 从 a 底部到 b 顶部
            p1 = ((ax0 + ax1) / 2, ay1); p2 = ((bx0 + bx1) / 2, by0)
            self.d.line([p1[0] * SS, p1[1] * SS, p2[0] * SS, p2[1] * SS], fill=color, width=w)
            self._arrowhead(p2, "down")
            lx, ly = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2 - 10
        elif style == "elbow":               # a 底 → 下折 → 横 → b 顶
            mid_y = (ay1 + by0) / 2
            pts = [((ax0 + ax1) / 2, ay1), ((ax0 + ax1) / 2, mid_y),
                   ((bx0 + bx1) / 2, mid_y), ((bx0 + bx1) / 2, by0)]
            self.d.line([(p[0] * SS, p[1] * SS) for p in pts], fill=color, width=w)
            self._arrowhead(pts[-1], "down")
            lx, ly = (pts[0][0] + pts[-1][0]) / 2, mid_y - 12
        elif style == "h":                   # 从 a 右侧到 b 左侧
            p1 = (ax1, (ay0 + ay1) / 2); p2 = (bx0, (by0 + by1) / 2)
            self.d.line([p1[0] * SS, p1[1] * SS, p2[0] * SS, p2[1] * SS], fill=color, width=w)
            self._arrowhead(p2, "right")
            lx, ly = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2 - 12
        elif style == "hu":                  # 从 a 右侧折上/折到 b 左侧（同一行也行）
            p1 = (ax1, (ay0 + ay1) / 2); p2 = (bx0, (by0 + by1) / 2)
            self.d.line([p1[0] * SS, p1[1] * SS, p2[0] * SS, p2[1] * SS], fill=color, width=w)
            self._arrowhead(p2, "right")
            lx, ly = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2 - 12
        else:                                # 折线：a 底 → 中间横线 → b 顶
            mid_y = (ay1 + by0) / 2
            pts = [(ax0 + (ax1 - ax0) / 2, ay1), (ax0 + (ax1 - ax0) / 2, mid_y),
                   (bx0 + (bx1 - bx0) / 2, mid_y), (bx0 + (bx1 - bx0) / 2, by0)]
            self.d.line([(p[0] * SS, p[1] * SS) for p in pts], fill=color, width=w)
            self._arrowhead(pts[-1], "down")
            lx, ly = (pts[0][0] + pts[-1][0]) / 2, mid_y - 12
        if label:
            f = F(16)
            bb = self.d.textbbox((0, 0), label, font=f)
            lx = lx + (6 if label_side == "right" else -(bb[2] - bb[0]) * SS - 6)
            self.d.rectangle([lx * SS - 2 * SS, ly * SS - 2 * SS,
                              lx * SS + (bb[2] - bb[0]) + 2 * SS, ly * SS + (bb[3] - bb[1]) + 6 * SS], fill="white")
            self.d.text((lx * SS, ly * SS), label, font=f, fill="#333333")

    def side_link(self, a, b, label=""):
        self.link(a, b, label, style="h")

    def save(self, path):
        if self.title:
            f = F(24, bold=True)
            bb = self.d.textbbox((0, 0), self.title, font=f)
            self.d.text(((self.w * SS - (bb[2] - bb[0])) / 2, 12 * SS), self.title, font=f, fill="#000000")
        self.im.resize((self.w, self.h), Image.LANCZOS).save(path)
        print("  ✓", os.path.relpath(path, REPO) if path.startswith(REPO) else path)


# ============================================================ 各图
def fig_arch(out):
    d = Dia("图 1　软件分层与任务划分", 1180, 760)
    d.round_box("app", 40, 70, 1100, 96,
                "应用层（4 个 FreeRTOS 任务）\nDataScope 上报任务 · SensorReport 采集任务 · Patrol 巡检任务链 · Ui 显示与交互任务",
                fill="#E8F5E9", edge="#2E7D32")
    d.round_box("svc", 40, 210, 1100, 92,
                "服务层\n协议编解码 protocol · 环形缓冲 ringbuf · 参数打包 param_pack · 里程 odom · 事件与状态位",
                fill="#EAF2FB", edge="#2F6FB2")
    d.round_box("bsp", 40, 348, 1100, 92,
                "驱动 / BSP 层\nMPU6050+DMP · TB6612 电机 · 编码器 · ADC(电池/气体) · USART1/2 · OLED · DHT11 · 蜂鸣器",
                fill="#FFF6E5", edge="#B26A00")
    d.round_box("ven", 40, 486, 1100, 92,
                "厂商基线（未改动）\nEXTI15_10 控制中断（5 ms / 10 ms 节拍）· 卡尔曼姿态 · 三环 PID 与限幅",
                fill="#F3E8FD", edge="#6A3FB5")
    d.round_box("hw", 40, 624, 1100, 76, "硬件：STM32F405RGT6 主控板 + 重做底板（传感器与外设接口）",
                fill="#F5F5F5", edge="#666666")
    for a, b in (("app", "svc"), ("svc", "bsp"), ("bsp", "ven"), ("ven", "hw")):
        d.link(a, b, style="v")
    d.save(os.path.join(out, "流程图_1_软件分层与任务划分.png"))


def fig_boot(out):
    d = Dia("图 2　上电初始化流程", 1180, 980)
    d.round_box("start", 440, 60, 300, 56, "上电复位", fill="#E8F5E9", edge="#2E7D32")
    d.box("a", 330, 150, 520, 74, "main()：delay_init(168) → uart_init(128000) → NVIC_PriorityGroup_4")
    d.box("b", 330, 250, 520, 74, "外设初始化：LED / KEY / OLED / 超声波捕获 / 编码器 / uart2(115200 蓝牙)")
    d.box("c", 330, 350, 520, 74, "IIC_Init → MPU6050_initialize → DMP_Init（姿态解算就绪）")
    d.box("d", 330, 450, 520, 74, "Adc_Init(电池) · CAN1_Mode_Init · MiniBalance_PWM_Init(8400)")
    d.box("e", 330, 550, 520, 74, "MiniBalance_EXTI_Init()　★ 控制中断从此按 5 ms / 10 ms 运行", fill="#FFF6E5", edge="#B26A00")
    d.box("f", 330, 650, 520, 74, "App_Start()：CommLink / Proto / SensorAdc / Buzzer / Gas / Patrol 初始化")
    d.box("g", 330, 750, 520, 74, "xTaskCreate ×4 · 注入毫秒延时钩子 Delay_SetMsHook(app_delay_ms)")
    d.round_box("h", 400, 860, 380, 62, "vTaskStartScheduler()（不返回）", fill="#F3E8FD", edge="#6A3FB5")
    for a, b in (("start", "a"), ("a", "b"), ("b", "c"), ("c", "d"), ("d", "e"), ("e", "f"), ("f", "g"), ("g", "h")):
        d.link(a, b, style="v")
    d.save(os.path.join(out, "流程图_2_上电初始化.png"))


def fig_isr(out):
    d = Dia("图 3　控制中断流程（EXTI15_10，厂商原始逻辑未改动）", 1320, 980)
    d.round_box("irq", 470, 60, 380, 54, "MPU6050 INT 下降沿", fill="#E8F5E9", edge="#2E7D32")
    d.box("clr", 430, 140, 460, 60, "清 EXTI 标志 · Flag_Target 翻转（交替 5 ms / 10 ms）")
    d.diamond("dm", 660, 280, 300, 100, "本次是哪个节拍？")
    d.box("m5", 70, 380, 380, 180,
          "5 ms 分支\n· Get_Angle（卡尔曼姿态）\n· Get_MC6（航模遥控）\n· Flash 参数条件回读\n· 电池电压采样 → 100 次平均\n→ 直接 return",
          fill="#EAF2FB", edge="#2F6FB2", wrap=16)
    d.box("m10", 560, 380, 400, 180,
          "10 ms 分支\n· 读编码器（TIM2 / TIM4）\n· Get_Angle · Read_Distane（超声波）\n· Balance_Pwm（直立 PD）· Velocity_Pwm（速度 PI）\n· Turn_Pwm（转向 PD）· 合成后限幅 ±8200",
          fill="#EAF2FB", edge="#2F6FB2", wrap=20)
    d.diamond("safe", 760, 690, 320, 120, "Turn_Off 判定：倾角 ±40° 或 电压 <11.10 V？", wrap=14)
    d.box("stop", 90, 850, 300, 80, "Set_Pwm 停车（安全保护）", fill="#FDECEA", edge="#C0392B", wrap=14)
    d.box("run", 560, 850, 300, 80, "继续：Led_Flash · 按键扫描\nPick_Up / Put_Down → 中断返回", fill="#E8F5E9", edge="#2E7D32", wrap=18)
    d.link("irq", "clr", style="v")
    d.link("clr", "dm", style="v")
    d.link("dm", "m5", "5 ms 节拍", style="v")
    d.link("dm", "m10", "10 ms 节拍", style="v")
    d.link("m10", "safe", style="v")
    d.link("safe", "stop", "是", style="elbow")
    d.link("safe", "run", "否", style="v")
    d.save(os.path.join(out, "流程图_3_控制中断.png"))


def fig_comm(out):
    d = Dia("图 4　通信收发流程（上行打包 / 下行解帧，两条独立路径）", 1280, 820)
    d.round_box("t1", 40, 70, 560, 62, "SensorReport / DataScope 任务", fill="#E8F5E9", edge="#2E7D32")
    d.box("t2", 40, 160, 560, 66, "按 20 Hz / 1 Hz 组装帧：AA 55 + 命令字 + 长度 + 数据 + CRC16")
    d.box("t3", 40, 250, 560, 66, "压入环形缓冲 ringbuf（生产端）")
    d.box("t4", 40, 340, 560, 66, "USART2 中断取数据发送（消费端）→ 蓝牙 / 有线")
    d.box("t5", 40, 450, 560, 90, "收 9401 B · 发 426 B（真机实测 2026-10-16）\n解帧 611 帧 · 丢帧 0 · 格式错帧 0", fill="#F5F5F5", edge="#666666")
    d.round_box("r1", 680, 70, 560, 62, "USART1 / USART2 接收中断", fill="#E8F5E9", edge="#2E7D32")
    d.box("r2", 680, 160, 560, 66, "字节入环形缓冲，解帧交给任务（中断内不调 RTOS API）")
    d.box("r3", 680, 250, 560, 66, "解帧状态机：找帧头 → 读命令字/长度 → 收数据 → CRC16 校验")
    d.diamond("ok", 960, 400, 300, 120, "CRC 正确？")
    d.box("bad", 680, 500, 270, 70, "丢帧计数 + 重新同步\n（100 ms 单帧超时）", fill="#FDECEA", edge="#C0392B", wrap=14)
    d.box("good", 990, 500, 250, 70, "交业务处理：\n命令解析 / 参数读写", fill="#E8F5E9", edge="#2E7D32", wrap=12)
    for a, b in (("t1", "t2"), ("t2", "t3"), ("t3", "t4"), ("t4", "t5"), ("r1", "r2"), ("r2", "r3"), ("r3", "ok")):
        d.link(a, b, style="v")
    d.link("ok", "bad", "否", style="v")
    d.link("ok", "good", "是", style="v")
    d.save(os.path.join(out, "流程图_4_通信收发.png"))


def fig_gas(out):
    d = Dia("图 5　气体采集与研判流程", 1180, 820)
    d.round_box("s", 420, 60, 340, 56, "SensorReport 任务周期到", fill="#E8F5E9", edge="#2E7D32")
    d.box("a", 330, 150, 520, 70, "ADC2 读 PA5（J13，MQ-135 角色）与 PA6（J10，MQ-2 角色）原始码")
    d.box("b", 330, 250, 520, 70, "换算 Rs = (VCC × RL) / Vao − RL，与洁净空气基准比得 Rs / R0")
    d.box("c", 330, 350, 520, 70, "查表得浓度 ppm；未标定/未就绪的通道置“未就绪 + 0”")
    d.diamond("d", 590, 490, 320, 120, "浓度超阈值？\n（连续 3 帧确认）", wrap=12)
    d.box("e", 330, 610, 240, 80, "蜂鸣器提示\n上报事件 0x13", fill="#FDECEA", edge="#C0392B", wrap=12)
    d.box("f", 700, 610, 240, 80, "正常：随 0x14 周期帧上行", fill="#E8F5E9", edge="#2E7D32", wrap=14)
    d.box("g", 330, 730, 610, 60, "★ 主判据用 Rs / R0 相对量（与 RL 取值无关）；ppm 绝对值依赖 R0 标定", fill="#F5F5F5", edge="#666666", wrap=42)
    for a, b in (("s", "a"), ("a", "b"), ("b", "c"), ("c", "d")):
        d.link(a, b, style="v")
    d.link("d", "e", "是", style="v")
    d.link("d", "f", "否", style="v")
    d.link("c", "g", style="v")
    d.save(os.path.join(out, "流程图_5_气体采集与研判.png"))


def fig_patrol(out):
    d = Dia("图 6　巡检状态机迁移", 1280, 820)
    d.round_box("warm", 60, 200, 200, 70, "预热 WARMUP", fill="#E8F5E9", edge="#2E7D32")
    d.round_box("idle", 60, 330, 200, 70, "待机 IDLE")
    d.round_box("patrol", 60, 460, 200, 70, "巡检 PATROL", fill="#EAF2FB", edge="#2F6FB2")
    d.round_box("back", 60, 590, 200, 70, "返航 RETURN")
    d.round_box("done", 60, 720, 200, 70, "完成 DONE", fill="#E8F5E9", edge="#2E7D32")
    d.box("sig", 60, 70, 300, 80, "启动 / 恢复：0x21 任务=自主巡航\n（解除安全锁存）", fill="#FFF6E5", edge="#B26A00", wrap=18)
    d.box("cal", 380, 330, 280, 70, "标定：传感器预热与基准", wrap=14)
    d.box("wp", 380, 460, 280, 70, "到点采样：气体 / 温湿度 / 里程", wrap=16)
    d.box("odom", 380, 590, 280, 70, "按编码器里程判定到点\n（不依赖红外）", wrap=16)
    d.box("fault", 800, 590, 340, 100, "安全停车 + 锁存 FAULT\n失联 > 500 ms · 急停 0x2F\n倾角过大 · 低压", fill="#FDECEA", edge="#C0392B", wrap=20)
    d.link("sig", "idle", "解除锁存", style="v")
    d.link("warm", "idle", style="v")
    d.link("idle", "patrol", "收到 0x21", style="v")
    d.link("patrol", "back", "巡检点走完", style="v")
    d.link("back", "done", "回到起点", style="v")
    d.link("idle", "cal", style="h")
    d.link("patrol", "wp", style="h")
    d.link("back", "odom", style="h")
    d.link("wp", "fault", "异常 / 失联", style="h")
    d.save(os.path.join(out, "流程图_6_巡检状态机迁移.png"))


def fig_decode(out):
    d = Dia("图 7　解帧状态机（含容错）", 1180, 800)
    d.round_box("s", 460, 60, 260, 54, "等待帧头", fill="#E8F5E9", edge="#2E7D32")
    d.diamond("a", 590, 180, 250, 96, "收到 0xAA？", wrap=10)
    d.diamond("b", 590, 320, 250, 96, "下一字节 0x55？", wrap=12)
    d.box("c", 430, 410, 320, 64, "读命令字 + 长度（小端 2 B）")
    d.box("d", 430, 500, 320, 64, "收满 data 段（粘包/分包由环形缓冲兜住）")
    d.diamond("e", 590, 640, 260, 100, "CRC16 校验通过？", wrap=12)
    d.box("ok", 850, 620, 260, 70, "交业务层处理\n（命令解析/参数/任务）", fill="#E8F5E9", edge="#2E7D32", wrap=14)
    d.box("bad", 120, 620, 260, 76, "丢帧计数 + 丢弃\n重新进入“等待帧头”", fill="#FDECEA", edge="#C0392B", wrap=14)
    for a, b in (("s", "a"), ("a", "b"), ("b", "c"), ("c", "d"), ("d", "e")):
        d.link(a, b, style="v")
    d.link("e", "ok", "是", style="h")
    d.link("e", "bad", "否", style="h")
    d.link("bad", "s", "100 ms 单帧超时", style="v")
    d.save(os.path.join(out, "流程图_7_解帧状态机.png"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "design_firmware", "图"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    print(f"输出目录：{args.out}")
    for fn in (fig_arch, fig_boot, fig_isr, fig_comm, fig_gas, fig_patrol, fig_decode):
        fn(args.out)
    print("完成，共 7 张。")


if __name__ == "__main__":
    main()
