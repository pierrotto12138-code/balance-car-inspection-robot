# 轮式机器人测控系统综合设计（第 1 组）

> 课程：测控系统综合设计 ｜ 3.0 学分 / 3 周 ｜ 控制科学与工程学院
> 项目：**基于平衡小车的仓储环境安全巡检机器人**
> **当前阶段：阶段三 —— 回归 MiniBalance 平台，重做底板（原理图 ✅ / PCB ✅ / 已焊好，真机联调中）**
>
> | 你是谁 | 先看哪份 |
> |---|---|
> | **👤 人（新读者）** | [`docs/handoff/交接说明.md`](docs/handoff/交接说明.md) —— 全部已确认事实、决策、资产位置、踩坑、下一步 |
> | **🤖 AI 编码代理** | [`AGENTS.md`](AGENTS.md) —— 路径、桥接、**36 条踩坑铁律**、能力边界 |
> | **📚 找文档** | [`docs/文档索引.md`](docs/文档索引.md) |
> | **🔧 板子到了要焊** | [`docs/design_hardware/焊接调试手册.md`](docs/design_hardware/焊接调试手册.md) |
> | **🔍 焊完逐项自检** | [`docs/design_hardware/上板自检清单.md`](docs/design_hardware/上板自检清单.md) |
> | **📦 元件清单（可下单）** | [`docs/design_hardware/BOM表.md`](docs/design_hardware/BOM表.md) |

---

## 一、项目概述

以两轮自平衡小车为移动平台，在仓储环境中沿预设巡检点自主行进，采集可燃气体、空气质量与
环境温湿度数据，就地完成超阈值研判与声光报警，并将姿态、位置与检测数据上传上位机；
操作员经监控面板远程下发运动控制与巡检参数指令，形成"状态上行—指令下行"的双向数据闭环。

**技术主线**：双通道气体传感 + 温湿度在线补偿 + 边缘研判 + 自适应采样。

---

## 二、硬件方案（阶段三：保留 + 重做）

**基于学校采购的平衡小车（MiniBalance V5.0 高配版）改版。**

| 部件 | 处置 | 说明 |
|---|---|---|
| **STM32F405RGT6 控制板** | **保留不动** | 板载 MPU6050、CH340G、AMS1117；与 F103RC pin-to-pin 兼容 |
| **★ TB6612 电机驱动** | **★ 集成到底板**（保留 **SSOP-24 实芯芯片**） | 课程要求"把 TB6612 集成到底板"；**不用模块座**（用座违背集成初衷） |
| **电压测量模块** | **改为集成到底板**（R2/R3 分压 + D3 指示） | 减少一个插接模块 |
| **电源变压模块** | **改为集成到底板**（U2 LM2596 + U3 AMS1117 + L1 + D1） | 减少一个插接模块 |
| **★ 底板（主板）** | **★ 重做** | 保留全部插座分布与 60 针控制板接口，集成电源/TB6612/电压采样 |
| **车架 / 电机 / 电池 / 车轮** | **沿用原车** | 组员实测：尺寸完全匹配 |

> **为什么重做底板**：底板是"全是插座与走线、最容易画对"的那块；
> 而控制板（含板载 IMU）与各拓展模块都是已验证的现成品。
> 这样 **A1 有真实的 PCB 设计工作，风险又可控**。

> **📌 板子与元器件已全部到货并焊好，已进入真机联调**（2026-10-15 第二轮）：
> 原理图完工：104 元件 / 52 位号 / 65 网络，**52 个位号全部有封装与立创料号**，**DRC 致命错误 0**。
> 详见 [`docs/design_hardware/原理图完善计划.md`](docs/design_hardware/原理图完善计划.md) §6.5。
>
> **📌 底板"改"板 PCB 已于 2026-10-08 完工**：523 走线段 / 76 过孔 / **顶层 + 底层实心 GND 铺铜**，
> **DRC 零错误、零未连接**。
> 最终渲染图：[`docs/design_hardware/图/PCB2_最终.png`](docs/design_hardware/图/PCB2_最终.png)。
> 另存了一份独立工程：`…\LCEDA-Pro\projects\26测控系统设计\`（板"改"的副本）。

### 2.1 外挂模块（本项目新增）

| 模块 | 用途 | 接口 |
|---|---|---|
| **MQ-2** | 可燃气体/烟雾 | 模拟输出 → ADC（**经 R10/R11 分压**）|
| **MQ-135** | 空气质量/有害气体 | 模拟输出 → ADC（**经 R12/R13 分压**）|
| **DHT11** | 环境温湿度（用于气体补偿） | 单总线 GPIO **PA7**（**R14 4.7k 上拉**）|
| **红外避障 ×2** | 近距避障 / 沿边循迹 | GPIO（L→**PB12**、R→PC1）|
| **蜂鸣器** | 超阈值声光报警 | **有源模块，GPIO 开关**（PC5 直连，低电平响）|

> **📌 台架现状（2026-10-15）**：上表是**板上的座定义**；真机台架上目前**只有 MQ-135，插在 `J13`**
> （`J10` 空、MQ-2 模块已烧毁待补）。`J10`/`J13` 四脚电气相同（`AO/DO/GND/+5V`），插错座不会损坏，
> 只是**名字对不上** —— 现在 MQ-135 的读数被当成 `MQ2` 上报（走 `PA5`），而 `MQ135` 字段（`PA6`/`J10`）是空输入。
> **在 MQ-2 装回 `J10` 之前，上位机上名为 `MQ2` 的那一路才是真在工作。**

### 2.2 引脚与接口

权威依据：`docs/design_hardware/pin_assignment.md`（**以源码为准，实物丝印校验**）。
原理图 PDF 在 `hardware/schematic/`，实物照片在 `hardware/photos/`。

---

## 三、角色分工（4 人）

| 角色 | 职责 | 提交物 |
|---|---|---|
| **A1** 电路 | **底板重做**（原理图 + PCB + 制板焊接调试） | 原理图、PCB、BOM、说明文档 |
| **A2** 结构 | 结构方案、外观与人机界面、装配图 | 结构图、装配图、效果图 |
| **B** 程序 | 固件：姿态、PID 平衡、任务状态机、通信 | 源代码工程、程序说明文档 |
| **C** 系统 | PC 上位机：双向通信、状态面板、远程控制、数据存储 | 源代码工程、可执行程序、用户手册 |

---

## 四、目录结构

```
.
├── README.md                 ← 你在这里（人类入口）
├── AGENTS.md                 ← 🤖 AI 编码代理入口（路径/桥接/36 条铁律/能力边界）
├── .gitignore
├── archive/                  # 阶段一/阶段二 的方案归档 zip
├── Base/                     # 学校提供的原始资料（只读，不进版本控制）
├── docs/
│   ├── task/                 # 课程任务书、分组统计（只读）
│   ├── proposal/             # 立项文档、项目计划书
│   ├── protocol/             # ★ 双向通信协议
│   ├── design_hardware/      # ★ 引脚表、车体解析、接口规划、PCB 渲染图、
│   │                         #   焊接调试手册、上板自检清单、BOM 表、人机界面说明
│   ├── design_firmware/      # 固件架构
│   ├── design_host/          # 上位机设计
│   ├── handoff/              # ★ 交接说明（新读者从这里开始）
│   ├── archive/              # 📦 历史归档（施工过程、往届清理记录）
│   ├── setup/                # 环境配置、准备清单
│   └── research/             # 调研记录（含 Agent 接入 EDA 的实测验证）
├── hardware/
│   ├── 硬件工程_完工版_20261008_最终/   # ★ 完工时点的 EDA 工程快照（本地保留）
│   ├── _参考存档/            # 厂商 / 开源方案原理图渲染图（本地保留）
│   ├── schematic/            # 原理图资料
│   ├── photos/               # 实物照片
│   └── *.py                  # EDA 工程文件辅助脚本
├── firmware/                 # B：STM32 固件工程
├── host/                     # C：Qt 上位机（P0 已完成：链路层 + 通信诊断面板 + 假源回放；ctest 2/2）
└── tools/                    # 辅助脚本（EDA API 桥接 + 核查）
```

> **源码的真身在 EDA 里**：工程 `参考01_copy`（`…\LCEDA-Pro\projects\`），
> 板 `改` = Schematic2 + PCB2 是**唯一有效版本**；`原`/`改_1` 为参照与工作副本。

---

## 五、关键技术决策

| 决策项 | 选择 | 理由 |
|---|---|---|
| 姿态算法 | **卡尔曼滤波** | 厂商默认，实车实测最佳 |
| 固件架构 | **混合式**：平衡控制保中断 + 其余进 FreeRTOS | 满足任务书 RTOS 要求，又不动已调好的平衡时序 |
| 通信协议 | **自研**：`0xAA55` + 命令字 + 长度 + CRC-16/MODBUS | 按巡检需求定制，可控且有校验 |
| 物理链路 | 经典蓝牙 SPP，115200 8N1 | 蓝牙座已板载 |
| 上位机 | **Qt 6 C++ + CMake** | 任务书允许；可命令行构建 |
| 气体量纲 | **主用 Rs/R0 相对量**，ppm 作估计值 | 绝对浓度精度不可靠；泄漏定位需要灵敏度与相对比较 |

---

## 六、当前进展

| 交付物 | 状态 |
|---|---|
| 任务书 / 项目计划书 | ✅ 已提交（**无需重交**） |
| 引脚分配表（源码+实拍互证） | ✅ 完成，待实物终验 |
| 双向通信协议 **v1.2** | ✅ 完成（v1.1 冻结 + `0x15` 诊断帧 / `0x16` 三环实况增量） |
| 上位机协议层 | ✅ **13 项单元测试全部通过**（链路层另有 8 项，`ctest` 2/2） |
| MQ 气体算法调研 | ✅ 完成（参数逐行核实） |
| 车体组成解析 | ✅ 完成 |
| **底板原理图（A1）** | ✅ **完工** —— 104 元件 / 52 位号 / 65 网络 / 全有封装与料号 / DRC 致命 0 |
| **底板 PCB（A1）** | ✅ **完工** —— 523 走线段 / 76 过孔 / 双面实心 GND 铺铜 / **DRC 零错误零未连接** |
| **PCB 打样 + 元器件下单** | ✅ **已到货并焊好，进入真机联调**（2026-10-15 第二轮） |
| 固件改造（B） | 🔶 **离线部分完成 + 两轮真机联调修正** —— `firmware/` 用 GCC 一键构建出 `.hex`；**离线核验 9 项全通过：协议 117 + 传感器 1235 + 状态机 80 + 调参 29 条断言**，另加 `tune.py --selftest`、交叉编译、向量表、中断安全、引脚基线；现占用 **Flash 约 66.5 KB / RAM 约 32.8 KB**；四任务已跑起来、程序说明文档已出（Python ↔ 上位机 ↔ 固件三方交叉验证），见 [`docs/design_firmware/B阶段开工准备.md`](docs/design_firmware/B阶段开工准备.md) |
| 上位机界面（C） | 🔶 **P0（通信链路）已完成**：`io/` 链路层 + 通信诊断面板 + 假源回放，`ctest` 2/2；`host.exe --selftest` 退出码 0。P1~P5 见 [`docs/design_host/C部分方案与计划.md`](docs/design_host/C部分方案与计划.md) |

> **下一步**：**B 的离线部分与两轮真机联调修正已完成**（见 `docs/design_firmware/程序说明文档.md`
> 与 [`docs/design_firmware/真机联调问题清单.md`](docs/design_firmware/真机联调问题清单.md)），剩余为
> **速度环复位后的 PID 整定**与**待修项**（见问题清单 §3 A 组）；
> C 的上位机界面待推进；A1 的上板自检见
> [`docs/design_hardware/上板自检清单.md`](docs/design_hardware/上板自检清单.md)。
>
> **📌 真机台架现状（2026-10-15）**：**只有 MQ-135，插在 `J13`**（`J10` 空、MQ-2 模块已烧毁待补）；
> DHT11 正常；电池 **11.8 V**；**OLED 正常显示**（页 1 / 页 2 / 厂商页）——
> 早期文档里"传感器全未就绪""OLED 乱码"的说法**均已解决**，不要再按旧现象排障。
>
> **📌 蓝牙链路结论（2026-10-15 实测）**：Windows 蓝牙 SPP **能配对但建不出 COM 口**；
> Python 的 `AF_BLUETOOTH` **在 Windows 上不可用**、`pybluez2` 在 Python 3.13 上编译不过；
> 本 Qt 套件**未安装 Qt Bluetooth 模块**。手头两颗模块：一颗 DX-Smart 克隆（老式 HC-06 方言、
> **纯从机**、`AT+ROLE`/`AT+ADDR` 不回），一颗 **JDY-33**（经典 SPP + BLE、可读 `AT+LADDR`、
> 已配名 `Pie` / 密码 `6667` / 115200，当前从机 `AT+TYPE0`）。
> 相关工具：[`tools/fw_prep/bt_scan.py`](tools/fw_prep/bt_scan.py)（按**合法帧**识别端口）、
> [`tools/fw_prep/bt_at.py`](tools/fw_prep/bt_at.py)（AT 配置，自动探波特率与方言）。
> **因此演示与联调以有线链路（板上 MicroUSB/USART1）为主链路。**

---

## 七、构建与开发环境

### 7.1 各部分的构建方式

| 部分 | 怎么构建 / 打开 | 说明 |
|---|---|---|
| **固件** | `cd firmware && make`（默认带 FreeRTOS，产出 `build/MiniBalance.hex`）；`make USE_RTOS=0` 出裸机对照版；`make size` 看占用；`make strict` 严格告警自检（**必须先 `make clean`**，否则增量编译下可能一条都不编、"0 条"是假绿） | **不用 Keil**（决策 34）：`arm-none-eabi-gcc` + Makefile。`make flash` 走 ST-Link；离线核验一键脚本 `python tools\fw_prep\run_all_tests.py`（**9 项**，GBK 控制台需 `set PYTHONIOENCODING=utf-8`）|
| **上位机** | Qt 6.11.2 + CMake + Ninja，详见 `host/README.md` | |
| **PCB 工程** | 嘉立创EDA 专业版（`…\LCEDA-Pro\projects\参考01_copy`）| 自动化核查脚本在 `tools/`（见 `tools/README.md`）|

> **📌 固件调试开关**（都在 `firmware/App/Config/app_config.h`，联调只改这一个文件）：
> `CFG_MOTOR_LEFT/RIGHT_INVERT`、`CFG_ENCODER_LEFT/RIGHT_INVERT`（**分侧、互不嵌套**）、
> `CFG_DEBUG_DISABLE_VELOCITY`（**当前 = 1**，调试期先摘掉速度环）、`CFG_FW_BUILD_ID`（固件版本字节）。
>
> ⚠️ **踩过的坑（AGENTS 铁律 35）**：`control.c` 要用 `#if CFG_*`，就**必须能看到 `app_config.h`**
> —— 否则预处理器按"未定义 = 0"处理，整段 `#if` 被静默跳过，**开关写什么值都无效且运行现象上看不出来**。
> 改完开关**必须用预处理输出验证**（`gcc -E`，命令取自 `compile_commands.json`），
> **不要**用"文件大小/哈希是否变化"判断（对齐会吸收 2~4 字节差异，给出假阴性）。

### 7.2 ★ 想"看代码 / 改代码"用什么

**结论：用 VS Code（+ STM32 官方扩展）；不要用已装的 Visual Studio 2022。**

| 候选 | 适不适合本项目 | 原因 |
|---|---|---|
| **VS Code + C/C++ + STM32 VS Code Extension** | ✅ **推荐** | 已经装了 `STM32CubeCLT`（含 arm-gcc / gdb / STM32_Programmer_CLI）与 `ST-LINK_gdbserver` —— ST 的扩展正是对接这套工具链：**跳转/补全（IntelliSense）+ 一键构建 + SWD 断点调试**。轻量、可脚本化、`.vscode/` 能随仓库共享 |
| CLion / STM32CubeIDE | 🔶 也可以 | 开箱即用的索引与调试，但体积大、和 `Makefile` 工程要额外配置，本项目没采用 |
| **Visual Studio 2022（本机已装的 C++ 工作负载）** | ❌ **不建议** | 它是 **MSVC** 工具链、面向 Windows/x64 程序；本项目是 **ARM 交叉编译 + Makefile + 自定义链接脚本**，VS 认不出这套构建，也没有 STM32 的烧录/调试集成。**用它读代码只会得到一堆红波浪线**（找不到 `stm32f4xx.h`、`FreeRTOS.h`）|
| Keil MDK | ❌ 不装 | 决策 34 已定：交付不用 Keil，实际用 GCC 构建 |

**为什么"能跳转、能补全"值得单独配一次**：本工程有 8 层 include 路径（`-IApp/...`、`-IDrivers/BSP/...`、
`-IMiddlewares/FreeRTOS/include`…）和一堆编译宏（`USE_FREERTOS`、`STM32F40_41xxx`）。
编辑器若不知道这些，`Gas_Update()` / `Patrol_Tick()` 这类符号就跳不过去 ——
解决办法是让它读一份 `compile_commands.json`（由 Makefile 的编译命令生成，**不需要装额外软件**）。

**动手三步**（VS Code，约 10 分钟）：

```powershell
winget install --id Microsoft.VisualStudioCode -e          # ① 装 VS Code
code --install-extension ms-vscode.cpptools                # ② C/C++（补全/跳转）
code --install-extension STMicroelectronics.stm32-vscode-extension   # ②' STM32 构建+调试（可选）
cd <仓库根>\firmware
make compile_commands                                        # ③ 生成编译数据库（见下方说明）
```

> 装完用 VS Code 打开 `<仓库根>` 文件夹即可：仓库里已带 [`.vscode/settings.json`](.vscode/settings.json)（指向 `firmware/compile_commands.json`）与 [`.vscode/extensions.json`](.vscode/extensions.json)（推荐扩展），
> 跳转/补全会按**真实的编译参数**解析（含所有 `-I` 与 `-D`），不会出现假报错。
> 调试需要 ST-Link 时，用 ST 扩展生成的 `launch.json`（本机 `ST-LINK_gdbserver` 已就绪）。

---

## 八、开发约定

1. **协议先行**：通信字段改动必须先更新 `docs/protocol/` 再改代码。
2. **引脚唯一来源**：`docs/design_hardware/pin_assignment.md`；**以源码为准**，原理图仅作参考。
3. **原始资料只读**：`Base/` 与 `docs/task/` 不修改。
4. **文档中文，工程目录英文**。
5. **交付前自检**：编译无警告、协议版本一致、中间产物已被 `.gitignore` 忽略。
6. **★ EDA 操作后立刻 `Ctrl+S`**：EDA 工程索引（`.eprj3`）会不同步，
   且**没保存的改动会随关闭丢失**（本项目踩过一次，丢了 144 段走线）。
   重要节点另存快照到 `hardware/硬件工程_完工版_<日期>/`。

---

## 九、历史方案归档

| 归档包 | 内容 |
|---|---|
| [`archive/归档_阶段一_MiniBalance方案_20260923.zip`](archive) | 初次以 MiniBalance 为基线的完整工作（10.3 MB） |
| [`archive/归档_阶段二_开源方案_20260923.zip`](archive) | 改用开源基底的探索（含 MQ 算法调研、上位机协议层，2.9 MB） |

> 两个归档中都包含**跨阶段仍适用的资产**（协议、MQ 算法、上位机代码、工程经验），
> 详见各归档内的《归档说明.md》。
