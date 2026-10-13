# AGENTS.md —— 给 AI 编码代理的工作说明

> 本文件是**给 AI 代理的入场须知**：项目在哪、怎么连 EDA、哪些坑踩过、什么能做什么不能做。
> **人类读者请看 [`README.md`](README.md)；接手工作请看 [`docs/handoff/交接说明.md`](docs/handoff/交接说明.md)。**

---

## 0. 一句话 & 当前状态

**项目**：基于平衡小车的仓储环境安全巡检机器人（课程：测控系统综合设计，第 1 组，4 人）。
**A1 的活**：把 MiniBalance V5.0 的**底板重做**（原理图 + PCB + 制板）。

| 阶段 | 状态 |
|---|---|
| 原理图（板「改」） | ✅ 完工 —— 104 元件 / 52 位号 / 65 网络 / 全有封装与立创料号 / DRC 致命 0 |
| PCB（板「改」） | ✅ 完工 —— 523 走线段 / 76 过孔 / **双面实心 GND 铺铜** / **DRC 零错误零未连接** |
| 打样与采购 | ✅ **全部到货，正在焊接**（2026-10-09 确认）|
| 固件 B / 上位机界面 C | 🔶 **B：P0~P5（离线部分）完成 + 复查/接口完善**（GCC 构建出 `.hex`；协议 117 + 传感器 1235 + 状态机 76 + 调参 29 条断言全过；四任务已跑起来、程序说明文档已出 → [`docs/design_firmware/B阶段开工准备.md`](docs/design_firmware/B阶段开工准备.md)）；C 待推进 |

**⚠️ 现在不要动 EDA 工程**，除非用户明确要求。板子已下单，改动会引入版本混乱。

---

## 1. 工作目录与关键路径

| 位置 | 说明 |
|---|---|
| **仓库根** | 克隆到哪就是哪（用 `pwd` 确认，**不要假定**）；下文一律写作 `<仓库根>` |
| **EDA 工程** | `%USERPROFILE%\Documents\LCEDA-Pro\projects\参考01_copy\`（**在仓库外**） |
| 独立副本 | `…\projects\26测控系统设计\`（板「改」的副本，交作业用） |
| 完工快照 | `hardware\硬件工程_完工版_20261008_最终\`（MD5 与工程一致） |
| EDA 桥接 | `Base\easyeda-bridge\easyeda-api-skill\` |
| 核查脚本 | `tools\`（见 [`tools/README.md`](tools/README.md)） |

**板「改」= Schematic2 + PCB2，是唯一有效版本**；「原」和「改_1」是参照/工作副本。

### 只读，不要修改

- `Base/`（学校原始资料，约 150 MB，已 gitignore）
- `docs/task/`（任务书原文）
- `docs/archive/`（历史归档——里面保留着当时的原始引用，**不要"修正"它**）

### 🚫 含他人个人信息的资料 —— 已移出本仓库

**本仓库是公开仓库（PUBLIC）**。下列含**他人个人信息**的资料已移到仓库外的独立目录，
**不要**把它们复制回本仓库：

| 位置（仓库外） | 内容 |
|---|---|
| `<仓库根>\..\260921Task_私有资料\` | 独立的本地 git 仓库（**与仓库同级、不进本仓库**，含：<br>· 全班分组统计表（**全班同学学号 + 姓名**）<br>· 本组任务书原件（含组员学号）<br>· 上届学生作业的原始工程与预览图<br>· 往届板解析与清理记录 |

**`.gitignore` 里已加了三道保险**，即使误放回原位也不会被提交：

```
_本地私有资料/
docs/task/*分组*统计*.xlsx
docs/task/*任务书_1组*
```

**⚠️ 新增资料前先自问：里面有没有别人的学号、姓名、联系方式、原始作业？**
有 → 放进**与仓库同级的** `260921Task_私有资料\` 目录，不要进本仓库。

### ✍️ 写作约定：不写真名，用角色代号

**本仓库公开，文档与代码里一律使用角色代号，不出现任何真实姓名：**

| 代号 | 角色 |
|---|---|
| **A1** | 电路（原理图 / PCB / 制板焊接） |
| **A2** | 结构（结构方案 / 装配图 / 效果图） |
| **B** | 程序（STM32 固件） |
| **C** | 系统（Qt 上位机 / 通信） |

**要指代"谁负责"时**：写 `A1（电路）`，**不要**写 `A1（某某某）`。
**要列分工时**：只写角色与职责，**不要**加"姓名"列。
**提交信息（commit message）里也不要写名字。**

> 检查命令（提交前跑一遍）：
> ```powershell
> Get-ChildItem -Recurse -File -Include *.md,*.txt,*.py,*.js,*.ps1,*.cpp,*.h |
>   Where-Object { $_.FullName -notmatch '\\Base\\|\\\.git\\' } |
>   Select-String -Pattern '<组员姓名 1>|<组员姓名 2>|...'
> ```
> （**这条命令本身不要写进公开仓库**，要用时临时敲。）

**⚠️ 血的教训**：本仓库曾误把全班分组表提交并推成公开仓库，
最后只能**【删库重建】**才清干净 —— `git push --force` **不会**删除 GitHub 上旧的不可达提交，
按 SHA 仍可访问。**含隐私的文件，第一次提交前就要挡住。**

---

## 2. 连 EDA：桥接怎么起

```powershell
# 1) 起桥（后台）
cd <仓库根>\Base\easyeda-bridge\easyeda-api-skill
node scripts/bridge-server.mjs        # 端口 49620

# 2) 健康检查（edaConnected 必须为 true）
curl.exe -s http://127.0.0.1:49620/health

# 3) 执行 JS（会被包成 async function(eda){...}，必须 return）
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_pcb_overview.js
```

**前置条件**：嘉立创EDA 专业版**已打开**，且装了 **`Run API Gateway`** 扩展并点过 `Reconnect`。
（`edaConnected: false` 时：让用户在 EDA 顶部菜单点 **`API Gateway → Reconnect`**，
**这一步只能人在 GUI 里做，AI 做不了**。）

---

## 3. ★★ 铁律（都是本项目**实际踩过**的坑）

### 3.1 数据安全

| # | 铁律 |
|---|---|
| **1** | **★ EDA 里改一段就 `Ctrl+S`。没落盘的改动会随关闭彻底丢失** —— 本项目丢过一次 **144 段走线**，任何文件级备份都救不回。**AI 每次读取数据前先调 `pcb_Document.save()`。** |
| **2** | **工程索引 `.eprj3` 会不同步** —— 复制/删除板子后，重开工程可能"少一块板"。**复制板之后要 `Ctrl+S`，再关掉重开确认。** |
| **3** | **做重要改动前先整目录快照**到 `hardware/`（`Copy-Item -Recurse`）。**改完校验 MD5。** |
| **4** | **不要在 EDA 打开时改它的文件** —— EDA 会覆写。要改文件先让用户关掉 EDA。 |

### 3.2 EDA API 的行为陷阱

| # | 陷阱 |
|---|---|
| **5** | **★ 不要相信 `pcb_Drc.check()` 的返回** —— 它读的是 **DRC 面板的缓存**，常常是旧的。判断连通性请用**几何算法**（见 `tools/eda_pcb_overview.js`：焊盘/走线端点/过孔做并查集）。**改完铺铜后它会刷新，那时才可信。** |
| **6** | **连接判断要考虑"宽线罩住两个脚"** —— 用户常用一根 40 mil 线居中罩住两个相邻焊盘，中线离焊盘中心有 10~16 mil。**判据要用 `焊盘半宽 + 线宽/2 的重叠`，不是"端点在焊盘中心 4 mil 内"。** |
| **7** | **T 型接头**：走线中段接到另一条线时，端点集合法会漏。几何算法要能处理。 |
| **8** | **`createNetLabel()` 成功时也返回 `undefined`** —— 不能用返回值判断成功，**建完必须回读 `sch_PrimitiveAttribute.getAll()` 确认条数**。盲目重试会建出一堆重复标签。 |
| **9** | **`sch_PrimitiveWire.modify(id, {net})` 不会生成网络标签** —— 网表看不见。要改网名必须用 `create(coords, net)` 或 `createNetLabel(x, y, net)`。 |
| **10** | **属性（ATTR）无法用 API 删除** —— `SCH_PrimitiveAttribute` 没有 `delete`。多余的标签只能让用户在界面里删。 |
| **11** | **`sch_PrimitiveComponent.create(deviceObject, x, y)` 可行，传 `{libraryUuid, uuid}` 会卡死 30 秒。** |
| **12** | **`createNetFlag()` 建的是 EDA 原生符号**（`Power-5V`/`Ground-GND`），与工程库符号（`power_+5V`/`power_GNDREF`）**风格不一致，且会被按前缀筛选的防短路脚本漏掉**。正确做法是复制现成的 `power_*` 符号。 |
| **13** | **铺铜多边形必须传"现成的 `IPCB_Polygon` 实例"**（普通对象会报"参数不正确"）。可行做法：取板框 polyline 的 `polygon` 对象，**改它内存里的坐标数组**（是副本，不会改到板框），再传给 `create()`。 |
| **14** | **`pourFillMethod` 必须传 `'solid'`** —— 传 `'90grid'`/`'45grid'` 会铺成**网格状**。 |
| **15** | **布线后必须 `rebuildCopperRegion()`** —— 否则铜皮不避让新走线 = **短路**。本项目在 `PCB2_1` 上出过 **246 处间距违规**。 |
| **16** | **自动布线要传 `ignoreNets: ['POWER_GNDREF']`** —— 否则它会拿 GND 把出线走廊占满，其他线过不去。 |
| **17** | **PCB 标签页开着时，原理图写入会失败或超时**（读正常）。操作原理图前先切过去。 |
| **18** | **批量 create 超过 ~10 个操作会 30 秒超时**，但 **EDA 侧通常仍会执行完**。请拆成 ≤10 个的小批次，并**回读确认**。 |
| **19** | **`openDocument(uuid)` 会切换 EDA 的当前文档**，可能打断用户操作；且**读到的可能是落盘状态**。做只读查询时优先用 `dmt_Pcb.getCurrentPcbInfo()` 确认在哪个文档。 |

### 3.3 领域知识陷阱

| # | 陷阱 |
|---|---|
| **20** | **EDA 的 `layer 12` = Multi-Layer（通孔焊盘），不是"底层"**。底层是 `layer 2`。别把 `{1: 106, 12: 115}` 读成"两面都有元件"。 |
| **21** | **改板板框的 y 上限是 `239.85`**（不是 `43`）。判"元件是否在框外"用 `x ∈ [-236.22, 3700.79]`、`y ∈ [-2319.20, 239.85]`。 |
| **22** | **座序必须对着实物丝印核，不能只信往届工程的网络表** —— 原板 `J3/J4` 的 pin2/pin5 与实物**是反的**，照它插电机会把编码器**反极烧毁**。 |
| **23** | **验证自己的改动要用独立方法** —— 不要拿自己写的解析器去验自己的改动（它可能共享同一个错误假设）。最可靠：**看文件里的原始记录**，或**数焊盘个数**。 |

### 3.4 固件 / RTOS 陷阱（B 阶段实际踩过，2026-10-09 补）

| # | 陷阱 |
|---|---|
| **24** | **★ 高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的中断里绝不能调 FreeRTOS API**（含 `...FromISR`）——本项目的 `EXTI15_10`（优先级 1）与 `USART1/2`（优先级 0）都在阈值(5)之上。**编译器不报错、上板才炸**。做法：中断里只"塞字节进环形缓冲"，解帧交给任务；用 `tools/fw_prep/check_isr_safety.py` 做静态核验。 |
| **25** | **★ 中断与任务不能共用同一个 ADC（外设）** —— 任务侧临界区**挡不住**高优先级中断：控制中断读电池时会重设 ADC1 通道，任务同时读就会互相打断（读到别的通道、EOC 标志被吃掉）。只能**物理分开**（本项目：ADC1 给中断算电池，ADC2 给任务测气体；PA5/PA6 恰好同时在 ADC12 上）。 |
| **26** | **★ 改板换了引脚，要连"所有用到该脚的地方"一起改** —— 超声波 TRIG 从 PC2 改到 PC4，若只改 `GPIO_Init` 而漏了发脉冲的 `PCout(2)`，现象是"编译没问题、测距恒为 0"。**改完要全仓库搜一遍旧引脚**（`PCout(2)`/`PAout(4)`…）。 |
| **27** | **★ 状态机与标定/初始化之间不要互相等待** —— "离开预热"要求"标定完成"，而"标定收尾"又写在"已离开预热"的分支里 = **死锁**（车永远进不了待机）。凡是"A 等 B、B 等 A"的流程，必须有一方是**单向收尾**（按次数或超时自行结束）。 |
| **28** | **解析外部文件/向量时对"分节变化与空列"要防御** —— 向量文件加了新分节，解析器把中文名当十六进制 `stoul` 直接抛异常；行尾空列还会被 `std::getline` 丢掉。**证据链本身崩了，比没有证据更危险**：解析器要按"列内容"判别而非按行位置。 |
| **29** | **★ "算出该停车"≠"真的停车"** —— 状态机的 `Patrol_MotorsDisabled()` 结果如果没人用，安全功能就是**装饰**：本项目复查时发现 500 ms 失联只把标志置位、**电机照转**。凡是"决策 → 执行机构"的链路，**两端都要各自验证**（一端单测逻辑、一端确认变量真的被中断/驱动读走）。同理还要排查**绕过安全逻辑的旁路** —— 厂商的"放下自动使能"与"按键启停"都曾能把安全锁存解除。 |

---

## 4. AI 的能力边界（本项目实测结论）

| 能做 ✅ | 不能做 ❌ |
|---|---|
| 原理图：放件、接线、改网名、填料号、改封装 | **PCB 摆放**（全局优化，做不好） |
| 原理图核查：逐脚解算、防短路、封装/料号覆盖率 | **PCB 布线**（要人做；自动布线只能辅助） |
| PCB 核查：线宽实测、间距/连通性、NetFlag 一致性 | **判断"布局好不好看"** |
| **铺铜**（建边界 + 重建，选 `solid`）| 器件选型的美学/结构判断 |
| 批量文件操作、文档整合、脚本编写 | 在 EDA GUI 里点菜单（必须让用户做） |
| 跑 DRC 并**判读**、导出数据 | 相信 DRC 的**缓存**返回 |

> **AI 的正确定位：检查员 + 文书 + 批量操作工，不是设计师。**

---

## 5. 常用命令

```powershell
# PCB 总览（改完 PCB 必跑）：走线/过孔/铺铜 + 线宽层分布 + 几何连通性 + DRC
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_pcb_overview.js

# 14 个连接器逐脚网络
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_connectors.js

# 原理图网表 vs PCB 网表
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_netdiff.js

# 防短路（原理图侧，42 个电源符号）
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_shortcheck.js

# 文件层独立核验（EDA 可关闭）
python hardware\sch_netlist.py "<...>\sch\Schematic2\P1.esch2"
python tools\sch_watch.py

# 渲染
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_render_pcb.js   # PCB
pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_render.js       # 原理图
```

**渲染取图**（返回 dataURL，需 base64 解码）：

```powershell
$out = pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_render_pcb.js
$s = ($out | Out-String).Trim() -replace '^data:image/png;base64,',''
[System.IO.File]::WriteAllBytes("shot.png", [Convert]::FromBase64String($s))
```

### 固件（B）—— 不需要 Keil

```powershell
cd <仓库根>\firmware
make                     # 默认带 FreeRTOS；产出 build/MiniBalance.hex（给 FlyMcu 烧）
make USE_RTOS=0          # 纯裸机对照（怀疑 RTOS 影响时序时二分定位）
make size                # Flash / RAM 占用
make strict              # ★ 严格告警自检（自研代码须 0 条；见 docs/design_firmware/编码规范.md）

# 离线核验（不需要板子）
python ..\tools\fw_prep\run_all_tests.py          # ★★ 一键跑完下面全部并汇总（提交前必跑）
python ..\tools\fw_prep\run_proto_test.py          # ★ 协议层原生单测（117 条断言，PC 上跑）
python ..\tools\fw_prep\run_sensor_test.py        # ★ 传感器算法原生单测（1235 条断言）
python ..\tools\fw_prep\run_patrol_test.py        # ★ 巡检状态机原生单测（76 条断言）
python ..\tools\fw_prep\run_param_test.py         # ★ PID 参数打包原生单测（29 条断言）
python ..\tools\fw_prep\tune.py --selftest          # ★ 有线调参工具自检（PC 侧独立实现，比对冻结向量）
python ..\tools\fw_prep\check_vector_table.py     # ★ 向量表 + 关键中断归属（读 .map）
python ..\tools\fw_prep\check_isr_safety.py       # ★ 中断安全：ISR 不得调用 FreeRTOS API（编译器不报，上板才炸）
python ..\tools\fw_prep\pin_check.py              # ★ 引脚基线：从 PCB 的 PAD_NET 逐脚比对
python ..\tools\fw_prep\gen_test_vectors.py       # 协议测试向量（B/C 共用，决策 36）
```

---

## 6. 文档约定

- **文档中文，工程目录英文。**
- **改动前先看对应文档**：硬件 → `docs/design_hardware/`；协议 → `docs/protocol/`。
- **决策要记进 [`docs/决策记录.md`](docs/决策记录.md)**（连续编号，现已到 52）。
- **固件（B）相关**：开工准备与核验记录在 [`docs/design_firmware/B阶段开工准备.md`](docs/design_firmware/B阶段开工准备.md)；
  构建 = `arm-none-eabi-gcc` + Makefile（**必须产出 `.hex`**，用 FlyMcu 串口 ISP 烧录），**不考虑 Keil 交付**（决策 34）。
- **新读者入口**：[`docs/文档索引.md`](docs/文档索引.md)；**接手**：[`docs/handoff/交接说明.md`](docs/handoff/交接说明.md)。
- **历史过程写进 `docs/archive/`**，正文只留结论。

---

## 7. 关键网络与引脚（速查）

**电机接口（★ 与实物丝印一致）**

| 座 | pin1 | pin2 | pin3 | pin4 | pin5 | pin6 |
|---|---|---|---|---|---|---|
| `CN1`（电机 A）| `MA_OUT1` | **GND** | `PB6` | `PB7` | **+5V** | `MA_OUT2` |
| `CN2`（电机 B）| `MB_OUT1` | **GND** | `PA0` | `PA1` | **+5V** | `MB_OUT2` |

**外挂模块**

| 模块 | 座 | 接法 |
|---|---|---|
| MQ-2 | `J13` | `MQ2_AO` / `PB3` / GND / +5V（AO 经 `R10`10k+`R11`15k 分压 → **PA5**，`C14` 去耦）|
| MQ-135 | `J10` | `MQ135_AO` / `PC3` / GND / +5V（经 `R12`+`R13` → **PA6**，`C15`）|
| DHT11 | `J9` | GND / +3.3V / **PA7**（`R14` 4.7k 上拉）|
| HC-SR04 | `J5` | +5V / **PC4**(TRIG) / **PB0**(ECHO) / GND |
| 蓝牙 | `J7` | NC / **PA2** / **PA3** / GND / +5V / NC |
| 蜂鸣器 | `J18` | +5V / **PC5** / GND（**有源模块，低电平响**）|
| 红外 L / R | `J11` / `J14` | +5V / GND / **PB12** · **PC1**（**DNP：留焊盘不留排针**）|

**电源树**：`J17`(12V) → `SW2` → `C13`/`C1` → `U2`(LM2596) → `L1` → `+5V` → `U3`(AMS1117) → `+3.3V`
**★ 电机走 `+12V`**（`U1` 的 `VM1/2/3`），不是 `+5V`。

**线宽约定**：电机/12V 主干 40~80 mil · +5V/+3.3V 25 mil · 模拟（`PA5`/`PA6`/`PB1`/`MQ*_AO`）12 mil · 一般信号 10 mil · **GND 靠铺铜**。

---

## 8. 提交前自检

1. `tools/eda_pcb_overview.js` → **间距违规 0 · 未连接 0**（GND 除外的网络）
2. **铺铜是 `solid`**，且**布线后已重建**
3. 文档链接无断链（全仓库扫一遍 `](...)`）
4. 重要节点已快照到 `hardware/`，**MD5 与工程一致**
5. `docs/决策记录.md` 已记下新决策
