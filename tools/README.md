# tools/ —— 辅助脚本

本目录是**核查与桥接工具**，只读为主，不修改 EDA 工程（除明确标注者）。

## 1. 通过 EDA API 桥接执行 JS

| 文件 | 用途 |
|---|---|
| `eda_exec.ps1` | **执行器**。通过 easyeda-bridge 在 EDA 进程内跑 JS。<br>用法：`pwsh -File tools/eda_exec.ps1 -CodeFile tools/xxx.js`<br>⚠️ 内部用 `curl.exe` 发请求 —— PowerShell 的 `Invoke-RestMethod` 会吞掉 500 的响应体，拿不到 `"timed out"` 这类关键信息 |

**桥的启动**（EDA 需已打开、且装了 `Run API Gateway` 扩展）：

```powershell
cd <仓库根>/Base/easyeda-bridge/easyeda-api-skill
node scripts/bridge-server.mjs        # 端口 49620
```

## 2. 核查脚本

| 文件 | 用途 | 何时用 |
|---|---|---|
| `bom_export.py` | **★ 离线导出 BOM**：解析完工快照的 `.epcb2`（位号 ↔ `COMPONENT` ↔ `META` 器件库），输出 数量/位号/型号/封装/**立创料号**/厂商；`--md`/`--csv` 直接产出交付文档与下单表格。**不需要开 EDA** | 交作业前导出 BOM、或 EDA 改动后重新导出 |
| `sch_watch.py` | 解析 `.esch2`，列出"改"板位号并**对照完善计划**（缺哪些、多哪些、关键网络是否存在） | **EDA 关闭时**跑，做文件层独立核验 |
| `hardware/sch_netlist.py` | 列出任意 `.esch2` 的位号/器件/值/网络名，支持两图 `--diff` | 同上 |
| `check_doc_links.py` | **★ 文档链接断链检查**：扫全仓库 `.md` 的 `](...)` 相对链接，报告不存在的目标（跳过 http/#、`Base/`、`archive/`、构建产物与**带时间戳的快照目录**——快照里的相对链接本来就指向快照外）。有断链则退出码 1 | **文档整合/挪动图片后必跑**（本项目踩过：图挪了目录、链接没跟着改，而 Markdown 断链是**静默**的） |
| `mech_export.py` | **★ 离线导出「机械接口数据」**：解析完工快照 `PCB2.epcb2`，输出**板框 100.00×65.00 / 圆角 R5.0 / 3×⌀3.2 安装孔**与**全部 52 个器件的坐标**（原点 = **板左下角**，mm），供 A2 画外壳/装配图用。**不需要开 EDA** | A2 开工，或板框/孔位/座位置需要复核时 |
| `eda_verify_all.js` | **逐脚解算** 8 个座 + 新增阻容 + J18 的网络，与期望表比对 | 改动接线后必跑 |
| `eda_shortcheck.js` | **全局防短路**：从每个电源符号 BFS，检查 `+5V`/`GND` 等电源网之间是否串通 | 改动电源接线后必跑 |
| `eda_verify_clean.js` | 元件普查（有位号/电源符号/无名/远处残留） | 怀疑有测试残留时 |
| `eda_render.js` | **把当前原理图渲染成 PNG**（返回 dataURL，需 base64 解码存盘） | 想"看一眼"图纸时 |
| `eda_resolve_pins.js` | 打印各座"引脚 → 网络"明细（带电源符号） | 排查单个座 |
| `eda_parts_stats.js` | 统计**封装**与**立创料号**覆盖率（判断能否随板贴片） | 采购/下单前 |
| `eda_no_fp.js` | 列出**没有封装**的元件 | 下单前必跑 |
| `eda_order_audit.js` | 下单前总审核（封装/料号/数量） | 下单前必跑 |
| `eda_set_lcsc.js` | 批量写立创料号 | 换件后 |
| `eda_read_annot.js` | 读"原"板 15 条模块丝印标注，并找每条最近的在座 | 核对插座用途 |
| `eda_search_parts.js` | 在**立创商城库**里按"阻值/容值 + 封装"搜器件（含 C 编号） | 选新增元件时 |
| `eda_repair4.js` | **参考实现**：区域清线 + 重建插座区 + 修电源符号错位 | 需要同类修复时照抄 |
| `eda_fix_rrr2.js` | **参考实现**：删局部乱线 + 按目标网重建（含区域保护） | 同上 |
| `extract_taskbook.py` | 从任务书 DOCX 抽纯文本 | 需要检索任务书条款时 |

### 固件（B 阶段）脚本 —— `fw_prep/`

> 除 `vendor_baseline.py --apply`（写 `firmware/`）外，其余**只读**；输出物在 `fw_prep/out/` 或 `firmware/build/`（均已 gitignore）。

> **🔧 换机器也能跑（不写死安装路径）**：固件相关的脚本通过 `fw_prep/toolchain.py` 自动找工具链。
> 找不到时它会打印**可直接照抄**的指引，任选一种即可：
> ```powershell
> # PC 端 C 编译器（跑原生单测）
> set FW_HOST_CC=C:\path\to\mingw64\bin\gcc.exe
> # 交叉工具链（构建/核验固件镜像）
> set FW_ARM_BIN=C:\path\to\GNU-tools-for-STM32\bin
> ```
> 或把对应 `bin` 目录直接加进 `PATH`。`make` 侧也可 `make PREFIX=/path/to/arm-none-eabi-`。
> 自检：`python tools/fw_prep/toolchain.py`（打印当前找到的两个工具链）。

| 文件 | 用途 | 何时用 |
|---|---|---|
| `fw_prep/gen_compile_commands.py` | **生成 `compile_commands.json`**（编辑器跳转/补全用）：抓 `make -n -B all` 里真实的编译命令，不真编译、不依赖 bear/compiledb。用法 `cd firmware && make compile_commands` | 想用 VS Code/clangd 读代码时（一次即可，产物不入库）|
| `fw_prep/normalize_whitespace.py` | **纯空白规范化**（LF + 去行尾空白 + 混合缩进折 Tab）：**白名单限定作用域**（厂商目录一律不碰），且带"非空白字符零变化"安全阀 | 整合排版时（幂等，可反复跑）|
| `fw_prep/audit_refactor.py` | **重构安全审计**：逐文件比较两个提交，把改动分成"纯排版"与"有真实内容"，并断言**控制中断/厂商库/内核**里没有真实改动 | 任何一次大范围排版/结构调整之后 |
| `fw_prep/toolchain.py` | **★ 工具链探测（通用化的关键）**：不写死任何安装路径 —— PC 端 C 编译器按 `FW_HOST_CC` → `PATH`(gcc/cc/clang) → `QTDIR` 下的 MinGW 查找；交叉工具链按 `FW_ARM_BIN` / `STM32CUBECLT` → `PATH` → 常见安装根查找。**装在哪都行**，并负责把编译器目录带进子进程 `PATH`（MinGW 的 `cc1` 需要同目录 DLL） | 其他脚本自动引用；换机器/报"找不到编译器"时先跑它自检 |
| `fw_prep/vendor_baseline.py` | 把厂商基线源码 vendor 进 `firmware/`：**GBK→UTF-8** + 目录映射 + 报告转码失败 | 重建/升级 `firmware/` 时（`--apply` 才写盘） |
| `fw_prep/gen_startup.py` | **生成 GCC 启动文件**：从 `User/stm32f4xx.h` 的 **IRQn 枚举**取名，98 个向量，杜绝手抄中断名 | 换芯片/改 CMSIS 头后 |
| `fw_prep/smoke.py` | 固件开工体检：① arm-gcc 构建最小 F405 镜像并出 hex；② 逐文件编译基线源码，列出"需改造"清单。基线目录取 `<仓库根>/Base/…`，可用 `FW_VENDOR_SRC` 覆盖 | 换机器 / 换工具链后 |
| `fw_prep/check_vector_table.py` | **★ 镜像核验**：栈顶/入口/`.isr_vector` 地址 + 关键中断（控制 ISR、USART2、TIM3/TIM8、SVC/PendSV/SysTick）逐条查**归属哪个 `.o`**（读 `.map`） | **每次 `make` 后** |
| `fw_prep/check_isr_safety.py` | **★ 中断安全静态检查**：从 `.map` 取真正参与链接的 ISR → 定位源码函数体 → 搜索 FreeRTOS API 名单；并列出各中断抢占优先级与 syscall 阈值的关系 | **每次动中断或 RTOS 配置后**（这类错误编译器不报） |
| `fw_prep/gen_test_vectors.py` | 生成协议**测试向量**（`docs/protocol/test_vectors.json/.tsv`：CRC 4 + 帧 12 + 流式 6），并检查协议文档声明的 N 与字段清单是否自洽 | 协议冻结前 / 改协议后 |
| `fw_prep/run_proto_test.py` | **★ 固件协议层原生单元测试**：用 PC 端 gcc（自动探测）编译 `firmware/App/Comms/*` + `firmware/tests/proto_test.c` 并在 PC 上跑（117 条断言：向量复现 + 超时 + 边界 + 环形缓冲） | **每次改协议/解帧代码后** |
| `fw_prep/gen_sensor_vectors.py` | 生成**传感器算法测试向量**（Python 独立实现 → `firmware/tests/sensor_vectors.csv`：GAS 280 / CALIB 2 / AVG 22 / CORR 7 / PARSE 9 / ALARM 5） | 改气体换算/补偿/标定/DHT11 后 |
| `fw_prep/run_sensor_test.py` | **★ 传感器算法原生单元测试**：编译 `firmware/App/Sensor/{gas,dht11,sensor_report}.c` + `firmware/tests/sensor_test.c` 并跑（**1235 条断言**，含五级中间量比对、迟滞、饱和钳位） | **每次改传感器算法后** |
| `fw_prep/run_param_test.py` | **★ PID 参数打包原生单测**：编译 `firmware/App/Control/param_pack.c` + `tests/param_test.c`（**29 条断言**：float 位型往返一致 / magic 与个数校验 / 离谱值拒绝 / 7 个 ID 无串位） | 改调参或持久化后 |
| `fw_prep/tune.py` | **★ 有线调参工具（PC 端）**：`--selftest` 用冻结向量自检自己的编解码；`--dump` 回读 7 个参数、`--set ID=VALUE` 在线改、`--save/--load` 存载 Flash、`--observe N --csv` **100 Hz 观测 + 姿态统计（标准差 / 过均值线次数→摆动频率）**、`--monitor` 常规监视、`--estop/--start/--stop`；**已支持 `0x15` 诊断帧与 `0x16` 三环实况解码，并打印固件版本字节**（`0x16` 偏移 19，`(月<<4)|日`）；`--replay` 可离线回放日志<br>★ **观测运行中的车必须加 `--enable`**（观测期间每 100 ms 发 `0x20 使能=1` 零速度：保活**并解除安全锁存**）；只看停着的车加 `--keepalive`（每 200 ms 发 `0x1F` 心跳，保活不解锁）。**两个都不加 ⇒ 固件 500 ms 后锁存停车，采到的是"电机已停"的无效数据**（2026-10-16 真机踩过，决策 84）| 板子到货后整定 PID；也是上位机调参界面的参考实现 |
| **`gen_flowcharts.py`** | **★ 生成 B（固件）报告的流程图/框图**：纯 PIL 绘制（方框 / 圆角框 / 菱形判断 / 折线箭头），**只用黑体+宋体**，不依赖 graphviz/Graphviz；产物 7 张 PNG 落在 `docs/design_firmware/图/`，被 `程序说明文档.md` §4 与 B 的提交报告引用。用法 `python tools/gen_flowcharts.py [--out 目录]` | 报告/答辩要换图或加图时 |
| `fw_prep/run_patrol_test.py` | **★ 巡检状态机原生单元测试**：编译 `firmware/App/Task/patrol.c` + `firmware/tests/patrol_test.c` 并跑（**80 条断言**，脚本化时间线：完整任务链 / 失联 / 急停 / 故障超时 / 迁移表 / 里程计判据） | **每次改状态机或安全策略后** |
| **`fw_prep/run_all_tests.py`** | **★★ 一键跑完 B 阶段全部离线核验（9 项）**：4 个原生单测 + `tune.py --selftest` + 交叉编译 + 向量表核验 + 中断安全 + 引脚基线核验，输出汇总表（`--no-build` 可跳过编译）<br>⚠️ **GBK 控制台需先 `set PYTHONIOENCODING=utf-8`**，否则汇总表里的中文/`✓` 会抛编码错误 | **提交前 / 每轮收尾** |
| `fw_prep/bt_scan.py` | **★ 蓝牙/串口端口自动识别**：把候选串口逐个按当前波特率试读，按**CRC 通过的整帧数**排序推荐（Windows 蓝牙 SPP 口号随机、描述不可信，"按名字猜口"很容易发错口）；识别时**显式驱动 DTR=高 / RTS=低**，避免 MCU 停在 BootLoader（AGENTS 铁律 33） | 找不到板子在哪条串口时（决策 61） |
| `fw_prep/bt_at.py` | **★ 蓝牙模块 AT 配置**：**自动探波特率与方言**后读写 AT 指令（改名/改波特率/查询地址等）；对**纯从机、老式 HC-06 方言**的克隆模块（`AT+ROLE`/`AT+ADDR` 不回）会明确报告"不支持"，不误判成功 | 配蓝牙模块（如 JDY-33：名 `Pie` / 密码 `6667` / 115200 / `AT+TYPE0`）时 |
| `fw_prep/host_crosscheck.cpp` | 用 **C 侧真实代码**（`host/src/protocol/*.cpp`）复算测试向量（CRC/帧/流式），与 Python 实现交叉验证 | 协议冻结前 |
| `fw_prep/check_crc.py` | CRC-16/MODBUS 四条用例的独立复算（Python 从零实现） | 协议冻结前 |
| `fw_prep/pin_check.py` | **★ 引脚基线文件层核验**：从完工快照的 `PCB2.epcb2` 读 `PAD_NET`，与期望表逐脚比对（13 个位号） | 改板/改插座后 |
| `fw_prep/FreeRTOSConfig_min.h`<br>`fw_prep/startup_min.S`<br>`fw_prep/stm32f405rg_flash.ld` | 冒烟测试用的最小配置 / 启动 / 链接（**不是**工程用的那份，工程用的在 `firmware/`） | 仅供 `smoke.py` |

### PCB 阶段专用

| 文件 | 用途 | 何时用 |
|---|---|---|
| **`eda_pcb_overview.js`** | **★ PCB 总览**：元件/走线/过孔/铺铜 + 线宽层分布 + 几何连通性 + DRC | **每次改完 PCB 必跑** |
| `eda_pcb_status.js` | 板框内外元件 + 关键件中心距（电源区/模拟区） | 检查摆放 |
| `eda_connectors.js` | **逐脚解算 14 个连接器**的引脚→网络 | 核对座序 |
| `eda_netdiff.js` | **原理图网表 vs PCB 网表**逐网比对（找 Netlist Error） | 同步后 |
| `eda_recheck.js` | PCB 走线/过孔/铺铜/DRC 速查 | 快速看一眼 |
| `eda_drc.js` | 跑 DRC 并输出分类统计 | 收尾 |
| `eda_render_pcb.js` | 把 PCB 渲染成 PNG | 出报告/看图 |
| `eda_copy_board.js` | 复制整块板（做沙盒/备份用） | 试验前 |
| `eda_new_project.js` | 调 API 新建工程 | 建新工程 |
| `new_project_folder.py` | **建"文件夹格式"新工程**并把指定板复制进去 | 交作业分工程 |
| `eda_uid_map.js` | 列出原理图 ↔ PCB 的 `UniqueId` 对应关系 | 位号乱了要还原时 |
| `eda_pcb_place.js` | 按坐标批量摆放 PCB 元件 | 需要精确定位时 |
| `eda_pcb_restore_des.js` | 按 `UniqueId` **还原被改乱的位号** | 位号被手工改坏时 |

### 渲染图纸的用法

```powershell
$out = pwsh -NoProfile -File tools\eda_exec.ps1 -CodeFile tools\eda_render.js
$s = ($out | Out-String).Trim() -replace '^data:image/png;base64,',''
[System.IO.File]::WriteAllBytes("shot.png", [Convert]::FromBase64String($s))
```

## 3. 使用须知（踩过的坑）

1. **改原理图前先关掉 PCB 标签** —— `PCB1` 打开时，原理图的 `create` 会全部失败或挂 30 秒，而**读仍正常**。
2. **桥有 30 秒超时** —— 一次请求别塞太多操作（建议 ≤10 个 create）。超时不代表没执行：EDA 侧可能已经做完，**要先回读确认再重试**。
3. **删导线要带"区域保护"** —— 只删"所有顶点都落在指定区域内"的线。EDA 会把共享端点的线**合并**成一条多分支折线，按端点匹配删除会误伤别的插座。
4. **放元件必须传完整器件对象**（`lib_Device.search()` 的返回值），传 `{libraryUuid, uuid}` 引用会卡死。
5. **建导线先删旧线**，否则新线会继承落在其坐标上的既有图元的网名。
6. **多条新线不要共享端点**，否则被合并后可能继承错误网名。
7. **挪动元件会弄断接线** —— 手工挪完元件后**必须重跑 `eda_verify_all.js`**。实测挪动 R11/R13/R14 后出现 `PA5 短接 GND`、`R13 接错到 PA5`、`R14 两脚短路`。
8. **改不了封装** —— 原理图与 PCB 的组件接口都只有 `getState_Footprint()`，**没有 setter**；用别的器件原位替换也不行（0603 电阻垂直间距 30 / 0805 水平间距 40，替换会弄断导线）。**只能在界面里改。**

## 4. 补充：料号字段在哪

读料号要看 **getState_SupplierId()**（值形如 C1234）。
属性里的 LCSC Part Name / Supplier Part 存的是**中文描述**（如"100nF ±10% 50V"），
**不是**商品编号 —— 只读这两个字段会误判成"没有料号"。

写入料号可用 modify(id, { supplier: 'LCSC', supplierId: 'C1234' })，
**不会替换器件、不会改变引脚几何、因此不会弄断导线**。

## 5. 补充：`createNetFlag()` 会建"原生符号"，与工程库符号不一致

`sch_PrimitiveComponent.createNetFlag('Power'|'Ground', net, x, y)` **能建出电源符号**，
但它生成的是 EDA **原生符号**（器件名 `Power-5V` / `Ground-GND`），
而本工程用的是**库符号** `power_+5V` / `power_GNDREF`。

**后果**：图样不一致，原理图上会混着两种风格的电源符号（A1 评价："太丑"）。

**正确做法**：在 EDA 界面里**复制现成的 `power_*` 符号**再改位置/网名，
**不要用 `createNetFlag()`**。

> 另一个副作用：本项目 `tools/eda_shortcheck.js` 是按器件名前缀 `power_` 筛选电源符号的，
> 用 `createNetFlag()` 建出来的符号**不会被它检查到** —— 防短路检查会出现盲区。

## 6. 补充：改网络名的正确 API —— 以及一个反直觉的返回值

### `modify(id, {net})` **不会**生成"网络标签"

原理图的网表是**按「网络标签 ATTR」**生成的，不是按导线的 `net` 字段。

| 做法 | 会不会被网表认到 |
|---|---|
| `sch_PrimitiveWire.create(coords, net)` | ✅ **会** —— 自动生成 NET 标签 |
| `sch_PrimitiveAttribute.createNetLabel(x, y, net)` | ✅ **会** —— 显式生成 NET 标签 |
| **`sch_PrimitiveWire.modify(id, {net})`** | ❌ **不会** —— 只改属性，网表看不见 |

> **典型问题（2026-10-07 实例）**：给分压中点补 `PB1` 时用了 `modify`，
> 于是**原理图显示 PB1、但网表里那个节点仍是无名网**，
> 同步到 PCB 后 `PB1` 只有 1 个焊盘（应为 4 个）→ **电池电压采样在板上是断的**。

### ⚠️ `createNetLabel()` 成功时也返回 `undefined`

**不能用返回值判断成功** —— 必须回读确认：

```js
await eda.sch_PrimitiveAttribute.createNetLabel(x, y, 'PB1');   // 返回 undefined
var all = await eda.sch_PrimitiveAttribute.getAll();             // 用它确认
```

> 否则会像我一样**误判失败并重试**，建出一堆重复标签。

### 属性（ATTR）**无法用 API 删除**

`SCH_PrimitiveAttribute` 只有 `createNetLabel / get / getAll / modify`，**没有 `delete`**；
`SCH_Primitive` 和 `SCH_SelectControl` 也没有。**多余的标签只能在 EDA 界面里手工删。**

### 验证网络名要"独立"验

**不要用自己写的解析器去验自己的改动** —— 它可能和改动共享同一个错误假设。
可靠做法：**看文件里那条记录**（`{"key":"NET","value":"PB1","parentId":"<导线id>"}`），
或者**同步到 PCB 后数焊盘个数**。

## 7. A2 结构工具（离线几何内核，不用装 CAD）

> 全部走 **OpenCASCADE（`cadquery-ocp`）+ trimesh**：STEP/STL 真几何，能出图、能量、能布尔。
> 交付件在仓库外 `…\260921Task_私有资料\正式作业提交\_A2审阅\`；参数与口径见
> [`docs/design_mechanical/A2_结构进展与待办.md`](../docs/design_mechanical/A2_结构进展与待办.md)。

| 文件 | 用途 | 何时用 |
|---|---|---|
| `mech_export.py` | 离线导出**机械接口数据**（板框/孔位/52 器件坐标），见 §2 | A2 开工 |
| `mesh_tools.py` | **★ 通用三维工具**：`info` 量化（尺寸/体积/水密/竖孔）/ `render` 四视图 / `holes` 孔检 / `check` 壁厚干涉 / `drawing` 零件图（三视图 + 尺寸 + 孔表 + 图框 + 仿宋）| 任何"量一下/看一眼/出一张图" |
| `a2_shell.py` | **★ 外壳生成**：下壳 + 上壳 + 装配体 STEP/STL（参数全在文件顶部：`TOP_H` / `MARGIN` / `WALL` / 开孔表）| 改结构后重出外壳 |
| `a2_print_coupon.py` | **★ 孔位试印件**：只打"下壳底板段 + 3 安装柱 + 4 外凸耳"（≈20 cm³），30 min 打完就能验孔位/铜柱/M3 自攻 → **不拿整壳去赌** | **打整壳之前必打** |
| `a2_car_assembly.py` | 整车装配（网格）：底盘 + 两轮 + 电机 + 电池 + 铜柱 | 出整车视图/爆炸图 |
| `a2_car_step.py` | 整车导出 STEP：默认复合体 / `--light` 轻量 / `--xcaf` 带零件名与颜色的**装配树**（云 CAD / SolidWorks 可看树）| 交给评审/在 CAD 里打开 |
| `step_assembly.py` | **★ 写"真装配"STEP**：复合体 → `AddShape(…, makeAssembly=True)` → 逐组件 `GetReferredShape` 命名，才会写出 `NEXT_ASSEMBLY_USAGE_OCCURRENCE`。自带 `selftest`（写两盒子 → 按文本数 PRODUCT/NAUO/BREP 断言）| 任何要出装配树的地方；**别再用 `AddShape(shape, False)`**（那是自由件，NAUO=0，见 §8）|
| `a2_explode.py` | 爆炸图 + 明细栏（含序号引出） | 交付图纸 |
| `step_intake.py` | **外来 STEP/IGES 一条命令体检**：件数/尺寸/水密/孔位 + 与"我们的口径"对照 | 拿到厂商或往届模型时先跑它 |
| `ref_parts_scan.py` | 参考件批量体检（先缓存 STL 再分析，避免反复解析大 STEP）| 拿来一堆外来件时 |
| `iges_read.py` | IGES/STEP 实体清单 + 细分导出（含面朝向修正）| 只想拆出某几件 |
| `sw_com.ps1` | **SolidWorks COM 封装**（`info`/`open`/`mass`/`export`/`close`）：附加到已开的实例、一律不保存、单位 mm-g | 想用真 SW 量质量属性/转格式，但不想拼 COM |

### ⚠️ 跑这些脚本的两个本机坑

1. **PATH 上的 `python` 缺依赖**（没有 numpy/trimesh/OSCP）⇒ 用 **DSH 运行时自带的解释器**：
   `C:\Users\<你>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`
2. **控制台是 GBK**，脚本里有 `⇒`/`×` 会 `UnicodeEncodeError` ⇒ 先设
   `$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'`。

## 8. 两个"名不副实"的坑（A2，2026-10-17 实测）

### 8.1 "装配" STEP 其实没有装配树

用 OCC 写装配时，`XCAFDoc_ShapeTool.AddShape(shape, False)` 只是把件加成**自由件**：
文件里 N 个 `PRODUCT` 各自独立、`NEXT_ASSEMBLY_USAGE_OCCURRENCE`(NAUO) = **0**
⇒ SolidWorks / 云 CAD 打开**看不到装配树**，也没法逐件选。

另一种直觉写法（`NewShape()` 空装配 + `AddComponent`）在这版 OCC 上更糟：
`PRODUCT=1 / NAUO=0 / MANIFOLD_SOLID_BREP=0`，**几何根本没写出去**。

**可行**：复合体 → `AddShape(comp, makeAssembly=True)` → 对每个组件 `GetReferredShape`
拿到被引用件标签再命名（名字要设在**被引用件**上，设在组件标签上不生效）。见 `tools/step_assembly.py`。

**顺带**：`A2-00_外壳装配.step` 旧版是 `fuse(下壳, 上壳)` —— 布尔并成**一个实体**，
"2 实体"是错的（文件里 1 个 `MANIFOLD_SOLID_BREP`）。

### 8.2 判据：数文本，不看"导出成功"

`STEPControl_Writer.Write()` 返回 `RetDone` **只说明文件写完了**，不说明结构对。
要按 STEP 文本数 `PRODUCT(` / `NEXT_ASSEMBLY_USAGE_OCCURRENCE` / `MANIFOLD_SOLID_BREP`，
并断言"`PRODUCT` = 件数+1、`NAUO` = 件数"。`step_assembly.describe()` 就是干这个的。

### 8.3 `OpenDoc6` 打不开外来格式（`2097152`），但 GUI 双击正常

**别被这个错误码骗了**（本项目 2026-10-17 一整轮排查的结论）。

- **自动化调用**：`ISldWorks::OpenDoc6(step/igs/stl/x_t/dxf, …)` 一律返回
  `2097152 = swFileRequiresRepairError`（本机 `swconst.tlb` 解出来的），
  连 40×30×10 的长方体、**SolidWorks 自己导出的** STEP 也一样；换 `swDocIMPORTED_PART`、
  `Silent|ReadOnly`、`CommandInProgress`、关 3D Interconnect、重置导入设置、改
  `Stringent Check and Repair` —— 全都不变。
- **GUI**：同一个文件**双击就开**（`文件→打开` 同理），打开后是 `xxx.step.SLDASM/.SLDPRT`
  的 **3D Interconnect 链接**文档。实测 `A2-00_外壳装配.step` → 2 组件、
  `A2-10_整车_装配_轻量.step` → 14 个命名件，装配树完全正确。
- 原因：SW 2026 对外来格式走 3D Interconnect 的内部路径，`OpenDoc6` 这条 API 对链接文档
  只回"需要修复"。

**结论**：① 判"文件能不能用"**必须用 GUI 双击验**，别用 `OpenDoc6` 的返回值；
② 要自动化开 STEP，别指望 `OpenDoc6`（这版 `ISldWorks` 也没有 `LoadFile4`）；
③ 链接文档要编辑/交付，先**根节点右键 → 断开链接**。

---

> **项目归属**：中国石油大学（华东） 专业生产实习课程项目<br>
> **范围说明**：本项目不涉及 MiniBalance 控制板（STM32F405RGT6 版本）的硬件设计与改版；控制板直接沿用，A1 仅重做主底板。<br>
> **许可**：原创代码与文档采用 MIT License；第三方组件遵循各自原始许可证。