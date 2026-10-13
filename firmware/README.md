# firmware/ —— B（固件）工程

> **角色 B 的交付物**：STM32F405RGT6 固件的完整源代码工程。
> 基于学校平衡小车（MiniBalance V5.0 高配版）的**厂商卡尔曼滤波版裸机源码**改造，
> 按 [`../docs/design_firmware/architecture.md`](../docs/design_firmware/architecture.md) 的
> **混合式架构**推进：平衡控制留在 MPU6050 中断里，其余功能进 FreeRTOS。
>
> 开工准备与核验记录：[`../docs/design_firmware/B阶段开工准备.md`](../docs/design_firmware/B阶段开工准备.md)

---

## 一、怎么编译（PC 端，不需要 Keil）

前置：`arm-none-eabi-gcc` 在 `PATH` 上（**装在哪个目录都可以**）。若不在 `PATH` 上，任选一种：

```powershell
make PREFIX=/path/to/GNU-tools-for-STM32/bin/arm-none-eabi-   # 直接给前缀
$env:FW_ARM_BIN = "C:\path\to\GNU-tools-for-STM32\bin"        # 或用环境变量
```

`make` 的 `PREFIX` 用 `?=` 定义，因此可以被命令行/环境覆盖；各核验脚本会自动探测
（顺序：`FW_ARM_BIN` / `STM32CUBECLT` 环境变量 → `PATH` → 常见安装根），
详见 [`../tools/fw_prep/toolchain.py`](../tools/fw_prep/toolchain.py)。

```powershell
cd <仓库根>/firmware
make                 # 产出 build/MiniBalance.elf + .hex + .bin + .map
make size            # 看 Flash / RAM 占用
make clean
```

**默认产出 `.hex`**（决策 34）——现场用 **FlyMcu 串口 ISP** 烧录：
控制板 MicroUSB 接电脑（板载 CH340G，带 DTR/RTS 自动 ISP）→ 选 CH340 串口 → 载入
`firmware\build\MiniBalance.hex` → 开始编程。

有 ST-Link 时也可以：`make flash`（`STM32_Programmer_CLI -c port=SWD -d ... -v -rst`）。

### 两种构建模式

| 命令 | 含义 | 用途 |
|---|---|---|
| `make`（默认 `USE_RTOS=1`） | **带 FreeRTOS**（混合式架构，决策 8） | 正式构建 |
| `make USE_RTOS=0` | **纯裸机**（与厂商基线行为一致） | 二分定位：怀疑 RTOS 影响平衡/时序时用来对照 |

### 离线核验（不需要板子）

```powershell
# ① 协议层原生单元测试（PC 上编译运行，117 条断言）
python ..\tools\fw_prep\run_proto_test.py

# ② 传感器算法原生单元测试（1235 条断言：换算五级 / 标定 / 滑动平均 / 补偿 / DHT11 / 迟滞 / 钳位）
python ..\tools\fw_prep\run_sensor_test.py

# ③ 巡检状态机原生单元测试（67 条断言：任务链 / 失联 / 急停 / 故障超时）
python ..\tools\fw_prep\run_patrol_test.py

# ③b PID 参数打包（存 Flash 往返 + 垃圾数据拒绝）
python ..\tools\fw_prep\run_param_test.py

# ④ 一键跑完以上全部 + 交叉编译 + 向量表 + 引脚基线
python ..\tools\fw_prep\run_all_tests.py

# ② 镜像向量表 + 关键中断归属（读 .map）
python ..\tools\fw_prep\check_vector_table.py

# ③ 引脚基线：从完工 PCB 的 PAD_NET 逐脚比对
python ..\tools\fw_prep\pin_check.py

# ④ 协议测试向量生成 + 协议文档自洽检查（B/C 共用）
python ..\tools\fw_prep\gen_test_vectors.py

# ⑤ 工具链与厂商基线可编译性体检（只读 Base/）
python ..\tools\fw_prep\smoke.py
```

---

## 二、目录结构

```
firmware/
├── App/                      ← 应用层（业务）
│   ├── Control/              control.c：★ MPU6050 中断里的三环 PID（5ms/10ms 节拍）
│   ├── Filter/               filter.c：卡尔曼 / 互补滤波
│   ├── Sensor/MPU6050/       姿态传感器
│   ├── Sensor/DMP/           DMP 库（main 里仍调用 DMP_Init）
│   ├── Ui/                   show.c：OLED / APP 显示
│   ├── Comms/DataScope/      厂商波形协议（调试通道，保留）
│   ├── Comms/                ★ 自研协议（P1 完成）：
│   │                           crc16.c       CRC-16/MODBUS（含增量版）
│   │                           protocol.c    组帧 + 流式解帧状态机 + 字段打包解包
│   │                           ringbuf.c     ★ SPSC 环形缓冲（中断→任务，无需关中断）
│   ├── Sensor/               ★ 传感器（P2 完成）：
│   │                           gas.c         MQ Rs/R0 + 温湿度补偿 + R0 标定 + 迟滞研判（纯算法）
│   │                           dht11.c       DHT11 帧解析与可信度判定（纯算法）
│   │                           dht11_hw.c    DHT11 单总线时序（PA7，含超时与重试）
│   │                           sensor_adc.c  ADC 三通道（PB1/PA5/PA6，复用厂商接口）
│   │                           sensor_report.c 组装 0x14 载荷 + ppm 饱和钳位 + 档位决策（纯算法）
│   ├── Task/                 ★ 任务与状态机（P3/P4）：patrol.c 状态机（纯逻辑）+ app_tasks.c 四个 FreeRTOS 任务
│   │                           app_api.h    ★ 给上位机/工具的只读接口（遥测/气体/状态/诊断/调参）
│   ├── Config/               ★ app_config.h 集中配置（轮径/线数/阈值/周期/栈/默认 PID/Flash 地址）
│   ├── Control/              ★ 三环 PID（厂商，时序未改）+ param_pack/param_store（调参持久化）
│   └── Ui/                   ★ buzzer.c 蜂鸣器（PC5 低电平响）
├── Drivers/
│   ├── CMSIS/                CMSIS 内核头 + **GCC 启动文件（生成）+ 链接脚本**
│   │   └── keil_original/    厂商原版 Keil(ARMCC) 启动汇编，**不参与构建**，仅留档对照
│   ├── FWLIB/                ST 标准外设库（StdPeriph）
│   └── BSP/                  adc / can / encoder / exti / iic / key / led / motor /
│                             oled / spi / stmflash / timer / usartx / NRF24L01
├── System/
│   ├── sys/                  ★ 位带宏、时钟（已移植为 GCC 版）
│   ├── delay/                ★ 延时（**已改 DWT CYCCNT**，SysTick 让给 FreeRTOS —— 决策 42）
│   ├── usart/                ★ printf 重定向（已移植为 GCC/newlib 版）
│   └── retarget/             newlib 系统调用桩（消掉链接警告）
├── Middlewares/FreeRTOS/     官方 FreeRTOS-Kernel **V11.1.0**（MIT，见 VERSION.txt）
├── User/
│   ├── main.c                入口（**仍是基线的超级循环**，P4 才搬进任务）
│   ├── stm32f4xx_it.c        中断向量实现（SVC/PendSV/SysTick 已按 USE_FREERTOS 隔离）
│   ├── system_stm32f4xx.c    时钟树
│   ├── FreeRTOSConfig.h      ★ FreeRTOS 配置（中断优先级策略写在里面）
│   └── rtos_hooks.c          栈溢出 / 堆失败钩子
├── Makefile
├── tests/                    ★ 原生（PC）单元测试：proto_test.c（协议/CRC/环形缓冲）
└── build/                    产物（已 gitignore）
```

**与厂商基线的对应关系**：`Base/…（卡尔曼滤波版）/CORE → Drivers/CMSIS`、
`FWLIB → Drivers/FWLIB`、`SYSTEM → System`、`MiniBalance → App`、
`MiniBalance_HARDWARE → Drivers/BSP`、`USER → User`。
映射脚本：[`../tools/fw_prep/vendor_baseline.py`](../tools/fw_prep/vendor_baseline.py)（可复跑）。
文件同时做了 **GBK → UTF-8** 转码，并把 `LED.C/LED.H/IOI2C.c/IOI2C.h/DataScope_DP.C`
改成小写文件名（Windows 不区分大小写，但 `Makefile` 的 `*.c` 会重复命中）。

---

## 三、移植记录（相对厂商 ARMCC 基线改了什么）

| # | 位置 | 改动 | 原因 |
|---|---|---|---|
| 1 | `System/sys/sys.c` | `__asm void WFI_SET/INTX_DISABLE/INTX_ENABLE/MSR_MSP` → CMSIS 内联函数 + GCC 内联汇编 | ARMCC 专有语法，GCC 不认 |
| 2 | `System/usart/usart.c` | 删 `#pragma import(__use_no_semihosting)`、`_sys_exit`、`fputc`；改 `_write()` 重定向 | newlib 的 printf 走 `_write`；半主机 pragma 是 ARMCC 专有 |
| 3 | `User/stm32f4xx_it.c` | SVC / PendSV / SysTick 三个 handler 用 `#if !defined(USE_FREERTOS)` 包起来 | 启用 FreeRTOS 时这三个由 `port.c` 提供，否则**重复定义** |
| 4 | `Drivers/CMSIS/startup_stm32f405.S` | **新写**（由 `tools/fw_prep/gen_startup.py` 从 `stm32f4xx.h` 的 IRQn 生成，98 个向量） | 厂商启动文件是 Keil 语法 |
| 5 | `Drivers/CMSIS/stm32f405rg_flash.ld` | **新写**链接脚本（Flash 1 MB / RAM 128 KB / CCM 64 KB，起始 `0x08000000`） | Keil 用分散加载文件，GCC 要 `.ld` |
| 6 | `System/retarget/syscalls.c` | 新增 `_close/_fstat/_isatty/_lseek/_read/...` 空实现 | 消掉 `nosys.specs` 的链接警告 |
| 7 | `User/FreeRTOSConfig.h`、`User/rtos_hooks.c` | 新增 | FreeRTOS 配置与故障钩子 |
| 8 | `Makefile` | 新增（`USE_RTOS` 可切换、`PREFIX` 可覆盖） | 不使用 Keil，用 GCC 构建 |

**未改动的部分（刻意）**：`App/Control/control.c` 里的 `EXTI15_10_IRQHandler`、
`App/Filter`、`Drivers/BSP/*`、`App/Ui/show.c` 等**逻辑一行没动** —— 目的是先做到
"GCC 能编出与厂商等价的镜像"，再在此基础上加功能，出问题时好二分定位。

### 编译告警现状（诚实记录）

- **链接警告 0**；
- 编译告警 **25 条，全部在厂商代码里**：22 条 `-Wpointer-sign`（`OLED_ShowString` 形参是
  `const u8*`，厂商自己传 `char*` 字面量）、1 条 `-Wmisleading-indentation`
  （ST 的 `system_stm32f4xx.c`）、1 条 `-Wunused-but-set-variable`；
- **我们新增/移植的代码（`sys.c`、`usart.c`、`startup`、`syscalls.c`、`rtos_hooks.c`、
  `Makefile`）在 `-Wall -Wextra` 下零告警**。

> 处理策略：**不为了"看着干净"去改厂商驱动**（改了就没法跟原版对照）。
> 后续新增代码保持零告警即可。

---

## 四、当前状态与下一步

| 步骤 | 状态 |
|---|---|
| **P0 骨架** | ✅ **完成**（2026-10-09）：GCC 构建跑通、FreeRTOS V11.1.0 入库并接入、向量表核验通过、`.hex` 就绪 |
| **P1 协议层** | ✅ **完成**（2026-10-09）：`crc16` / `protocol`（帧编解码 + 解帧状态机 + 100 ms 超时）/ `ringbuf`；**原生测试 117 条断言全过**，与 `host/` 三方交叉验证一致（帧 16 条） |
| **P2 传感器** | ✅ **完成**（2026-10-09）：`gas`（Rs/R0 + 补偿 + 标定 + 迟滞）/ `dht11`+`dht11_hw`（PA7）/ `sensor_adc`（PA5/PA6）/ `sensor_report`（0x14 载荷 + 钳位 + 档位）；**原生断言 1235 条全过**，五级中间量与 Python 独立实现逐条一致 |
| **P3 状态机** | ✅ **完成**（2026-10-09）：`App/Task/patrol.c`（阶段迁移 + 事件队列 + **500 ms 失联安全停车锁存**）；**脚本化时间线测试 67 条全过** |
| **P4 任务化** | ✅ **完成**（2026-10-09）：四个 FreeRTOS 任务 + **USART2/USART1 双链路**接环形缓冲 + `delay` 改 **DWT** + `Group_4`；**内核与四任务确认进镜像**；中断安全静态核验通过 |
| **P5 文档** | ✅ **完成**（2026-10-09）：[`../docs/design_firmware/程序说明文档.md`](../docs/design_firmware/程序说明文档.md)（任务书 B 交付物） |
| P5 联调 | ⬜ 待硬件（板子等货）：见 §五 已知问题 + 开工准备 §14.6 的 5 项实测 |

### 构建实测（2026-10-09）

| 模式 | Flash | RAM |
|---|---|---|
| `USE_RTOS=1`（默认） | **62,968 B / 1 MB（6.01%）** | **32,792 B / 128 KB（25.02%）** |
| `USE_RTOS=0`（裸机对照） | 49,188 B（4.69%） | 5,176 B（3.95%） |

`.hex` 数据区 `0x08000000 ~ 0x0800C3A4`（48.9 KB），首行 `:020000040800F2`、末行 `:00000001FF`
（标准 Intel HEX，可直接给 FlyMcu）。

---

## 五、两条链路与 PID 调参（2026-10-09 新增）

| 链路 | 端口 | 速率 | 用途 |
|---|---|---|---|
| 蓝牙 | USART2（PA2/PA3） | 115200 8N1 | 主链路：演示/验收/上位机正常连接 |
| **有线** | **USART1（PA9/PA10，USB-TTL）** | 115200 8N1 | 实验室调试与 **PID 整定**（不受蓝牙配对与干扰影响）|

两条链路**同一套协议、对等**：收到帧就处理、回复原路返回；周期上报与事件告警**两条都发**。

**有线调参流程**（USB-TTL 接 PA9/PA10）：

```text
0x24{ID=0xFD, 值=1}  → 进调参观测模式：有线链路开始 100 Hz 上报 0x10/0x11
0x24{ID=0xFF}        → 全量回读 7 个参数（确认当前值）
0x24{ID=0x03, 值=85} → 改 Velocity_Kp（立即生效），机器人回该参数**实际生效值**
0x24{ID=0x00}        → 存入 Flash（回值 0=成功）；0xFE 为从 Flash 重新载入
0x24{ID=0xFD, 值=0}  → 退出观测模式（恢复 20 Hz）
```

参数掉电保存：**float 位型**存于扇区 6（`0x08040000`，不复用厂商的 `0x08020000`），
带 magic + 数值合理性校验 + **写入后当场回读校验**；打包逻辑有原生单测（`run_param_test.py`）。
> 待 C 会签：0xFF/0x00/0xFE/0xFD 这四个扩展参数 ID（**帧格式不变**，见协议文档 v1.0-b）。

## 六、★ 已知问题 / 待办

| # | 问题 | 状态 / 何时处理 | 说明 |
|---|---|---|---|
| 1 | ~~`delay_us/xms` 直接操作 SysTick~~ | ✅ **已改 DWT CYCCNT** | SysTick 完全交给 FreeRTOS（决策 42）|
| 2 | ~~`NVIC_PriorityGroup_2`~~ | ✅ **已改 Group_4** | 控制中断优先级 1、串口 0，都高于 syscall 阈值 → 不会被调度器延迟，也绝不调 RTOS API（有静态核验脚本）|
| 3 | ~~调度器未启动~~ | ✅ **已启动** | `App_Start()` 建 4 任务并 `vTaskStartScheduler()`；`nm` 可见内核与任务符号 |
| 4 | ~~超声波 TRIG 是 PC2~~ | ✅ **已改 PC4** | 初始化与发脉冲两处都改了（`timer.c`）；照源码不改则新板上测距恒为 0 |
| 5 | ~~`printf` 与协议抢串口~~ | ✅ **已有 TX 互斥** | 两条链路共用一个发送互斥量 |
| 6 | **蓝牙波特率 9600 → 115200** | ⬜ 到货实测 | 若模块改不了，协议波特率要回退并同步 C |
| 7 | **MQ 的 `RL` 与 `R0` 未实测** | ⬜ 到货实测 | 先量模块负载电阻，再标定 `R0` 并存 Flash |
| 8 | **轮径与编码器线数未实测** | ⬜ 到货实测 | `app_config.h` 里暂定 ⌀67 mm / 1560 counts/rev；不实测则"轮速 mm/s"只是按假设换算 |
| 9 | **`waypointReached` 恒为 0** | ⬜ 到货后接里程判据 | 状态机的"到位"输入还没接编码器里程（已知待接线项）|
| 10 | DHT11 在任务里可能被控制中断打断 | ⬜ 到货实测 | 已内置重试 3 次；若仍不稳，改由中断捕获边沿 |

---

## 六、变更记录

| 日期 | 内容 |
|---|---|
| 2026-10-09 | **P0 完成**：从厂商 ARMCC 裸机基线移植到 GCC（arm-none-eabi 14.3）+ Makefile；FreeRTOS V11.1.0 入库并接入（`USE_RTOS` 可切）；新写 GCC 启动文件与链接脚本；`.hex/.bin/.map` 产出并核验（起始 0x08000000）；向量表逐条核验通过 |
| 2026-10-09 | **第二轮复查 + 接口完善**：修 3 个逻辑缺陷（R0 标定死锁 / 0x11 不是 mm/s 与 PWM / 0x23 未回 0x12 确认）+ 超声波 TRIG PC2→PC4；新增**有线调试链路（USART1）**、**0x24 调参扩展与 Flash 持久化**、**给上位机的只读 API（app_api.h）**、**集中配置（app_config.h）**；原生单测增至 4 套 |
| 2026-10-09 | **P1 完成**：`App/Comms/{crc16,protocol,ringbuf}` + `tests/proto_test.c`；协议模块只依赖 `<stdint.h>`，可在 PC 原生编译测试（`python ..\tools\fw_prep\run_proto_test.py` → 97 条断言全过）；与上位机、Python 三方交叉验证一致 |
