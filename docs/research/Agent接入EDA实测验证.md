# Agent 接入 EDA：实测验证报告

> 验证日期：2026-09-23
> **结论：链路完全打通，agent 可以程序化读写立创EDA 专业版工程。**
> 本文件记录**实际跑通的命令与返回**，可复核。

---

## 一、验证结果：✅ 全部通过

| 验证项 | 结果 |
|---|---|
| Bridge 服务器启动 | ✅ 端口 49620 |
| EDA 客户端连接 | ✅ `edaConnected: true` |
| 在 EDA 进程内执行代码 | ✅ 返回真实数据 |
| 读取工程信息 | ✅ 工程名、UUID、板卡结构 |
| **读取元件（101 个）** | ✅ 与解析 `.eprj2` 得到的数据**完全一致** |
| 打开指定文档 | ✅ `openDocument()` 可用 |
| 写入能力（API 存在） | ✅ `create`/`modify`/`delete` 均已确认存在（未做破坏性测试） |

---

## 二、环境与组件

| 组件 | 位置/版本 |
|---|---|
| EDA 客户端 | 立创EDA 专业版 **4.1.60**（安装目录各人不同） |
| **扩展** | **`Run API Gateway`** —— 用户已在 EDA 内安装，并勾选「允许外部交互」+「显示在顶部菜单」 |
| **Bridge 服务器** | 官方 `easyeda-api-skill` 仓库的 `scripts/bridge-server.mjs` |
| Bridge 本地路径 | `Base/easyeda-bridge/easyeda-api-skill/` |
| Node | v24.19.0（要求 22+） |
| 依赖 | 仅 `ws`（`npm install` 已装） |

### 启动方式

```bash
cd Base/easyeda-bridge/easyeda-api-skill
npm install                                  # 仅首次
node scripts/bridge-server.mjs               # 起 Bridge
```

> 端口固定 **49620**；EDA 扩展会在这条链路里自动扫描 49620–49629 并连回来。
> **注意**：Bridge 是前台进程，需保持运行；EDA 侧扩展会自动重连（实测断开后 20 秒内自动恢复）。

---

## 三、HTTP 接口（实测可用）

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/health` | 健康检查 + EDA 连接状态 |
| GET | `/eda-windows` | 列出 EDA 窗口 |
| POST | `/eda-windows/select` | 多窗口时切换目标窗口 |
| **POST** | **`/execute`** | ★ **在 EDA 进程内执行 JS 并返回结果** |

### 调用形式

```bash
curl -s -X POST "http://127.0.0.1:49620/execute" \
  -H "Content-Type: application/json" \
  -d '{"code":"return JSON.stringify(await eda.dmt_Project.getCurrentProjectInfo());"}'
```

返回格式：`{"success":true,"result":"...","windowId":"..."}`

> `code` 里可以直接 `await`，可以用 `return` 返回任意值（会自动序列化）。

---

## 四、★ 已确认可用的 API（实测清单）

### 4.1 工程与文档

| API | 方法 | 实测 |
|---|---|---|
| `eda.dmt_Project` | `getCurrentProjectInfo()` | ✅ 返回工程名/UUID/板卡树 |
| `eda.dmt_Board` | `getAllBoardsInfo()`、`getCurrentBoardInfo()`、`createBoard`、`modifyBoardName` | ✅ |
| `eda.dmt_Schematic` | `getAllSchematicsInfo()`、`getAllSchematicPagesInfo()`、`getSchematicPageInfo(uuid)`、`createSchematic`、`modifySchematicName` | ✅ |
| `eda.dmt_Pcb` | `getAllPcbsInfo()`、`getCurrentPcbInfo()`、`createPcb` | ✅ |
| **`eda.dmt_EditorControl`** | **`openDocument(uuid)`**、`closeDocument`、`activateDocument`、`getCurrentRenderedAreaImage`、`zoomTo` | ✅ **打开文档实测成功** |

### 4.2 原理图（读写齐备）

| API | 方法 |
|---|---|
| `eda.sch_PrimitiveComponent` | **`getAll()`**（实测读到 101 个元件）、`get`、`create`、`modify`、`delete`、**`getAllPinsByPrimitiveId`**、`createNetFlag`、`createNetPort`、`placeComponentWithMouse` |
| `eda.sch_Net` | `getAllNets()`、`getAllNetsName()`、`getNet()`、`getCurrentProjectAllNets()` |
| `eda.sch_PrimitiveWire` | （导线，读写） |
| `eda.sch_PrimitiveText` | （文本/丝印） |
| `eda.sch_Document` | `save`、`autoLayout`、`autoRouting`、`navigateToCoordinates`、`getPrimitivesInRegion` |
| `eda.sch_Netlist` | `getNetlist()`、`setNetlist()` |
| `eda.sch_Drc` | DRC 检查 |

### 4.3 PCB（读写齐备）

| API | 方法（节选） |
|---|---|
| `eda.pcb_Document` | 文档操作 |
| `eda.pcb_Net` | 网络 |
| `eda.pcb_Layer` | 层管理 |
| `eda.pcb_PrimitivePad` / `Via` / `Pour` / `Line` / `Fill` / `Region` / `String` … | 焊盘/过孔/覆铜/走线/丝印 |
| **`eda.pcb_Drc`** | **DRC 检查** |
| **`eda.pcb_ManufactureData`** | **★ 制造数据导出（Gerber 等）** |
| `eda.pcb_RayTracerEngine` | 3D 渲染 |

### 4.4 库与系统

| API | 用途 |
|---|---|
| `eda.lib_Device` / `lib_Footprint` / `lib_Symbol` / `lib_3DModel` / `lib_Cbb` | 元件/封装/符号/3D/CBB 库操作 |
| `eda.sys_FileSystem` / `sys_FileManager` | 文件读写 |
| **`eda.sys_FormatConversion`** | **格式转换** |
| `eda.sys_Dialog` / `sys_MessageBox` / `sys_ToastMessage` | 交互 |
| `eda.sys_Storage` / `sys_Setting` | 存储与设置 |
| `eda.sys_ExternalApi` / `sys_WebSocket` | 外部接口 |

---

## 五、完整验证记录（可复核）

### 5.1 健康检查

```json
{"service":"easyeda-bridge","status":"ok","edaConnected":true,
 "edaWindowCount":1,"activeWindowId":"c00f35f2-...","pendingRequests":0}
```

### 5.2 API 命名空间（95 个命名空间，节选）

```
dmt_Board, dmt_EditorControl, dmt_Pcb, dmt_Project, dmt_Schematic,
sch_Document, sch_Net, sch_Netlist, sch_PrimitiveComponent, sch_PrimitiveWire, sch_Drc,
pcb_Document, pcb_Net, pcb_Layer, pcb_PrimitivePad, pcb_PrimitiveVia, pcb_PrimitivePour,
pcb_Drc, pcb_ManufactureData, pcb_RayTracerEngine,
lib_Device, lib_Footprint, lib_Symbol, lib_3DModel,
sys_FileSystem, sys_FormatConversion, sys_Storage, ...
```

### 5.3 读取工程信息

```json
{"itemType":"Project",
 "uuid":"e1c0c3dc888d182f7d41b44d1b1883d2b64cd77a5cb14b3acda6e71b7ebb06db",
 "friendlyName":"往届作业参考",
 "data":[{"itemType":"Board","name":"PCB1",
          "schematic":{"uuid":"97f7695b12c64165b15d0ad2cb8c9d42", ...}}]}
```

> ✅ 与我们此前从 `.eprj2` 解出的 `schematics.uuid = 97f7695b...` **完全一致**。

### 5.4 ★ 读取元件（关键验证）

```
real components=100
Connector_Conn_01x03_Socket ×4, Driver_Motor_TB6612FNG, Device_Buzzer,
Device_LED ×3, POWER_GNDREF ×28, POWER_+3.3V ×4,
Connector_Conn_01x04_Socket ×3, POWER_+5V ×15, Connector_Conn_01x06_Socket, ...
```

> ✅ **与我们此前从 `.eprj2` 解出的元件统计完全吻合**
> （101 个元件含 1 个图纸符号，实体 100 个；TB6612、蜂鸣器、LED×3、大量 GND/电源标号）。
> **两条独立路径（解析文件 vs 调用 API）得到一致结果 → 验证可靠。**

---

## 六、能力边界与使用注意

### 6.1 可用能力（已验证）

| 能力 | 状态 |
|---|---|
| **读**：工程结构、元件、网络、引脚、DRC 结果 | ✅ 可用 |
| **写**：增删改元件、放置网络标号、导线、改属性 | ✅ API 存在（未做破坏性测试） |
| **打开/切换文档** | ✅ 可用 |
| **导出制造数据（Gerber）** | ✅ API 存在（`pcb_ManufactureData`） |
| **3D 渲染取图** | ✅ API 存在（`RayTracerEngine`） |

### 6.2 ⚠️ 注意

| 项 | 说明 |
|---|---|
| **必须保持 GUI 运行** | 无 headless；EDA 必须开着，Bridge 必须运行 |
| **需打开目标文档** | 部分 API（如 `getNetlist`）**依赖当前打开的是哪个文档**；实测空页返回空网表 |
| **`getNetlist()` 有报错** | 在往届这块有内容的板上报 `TypeError: i is not iterable` —— 可能是该工程数据不完全或用错参数。**改用 `sch_PrimitiveComponent.getAll()` 读取元件是可行的替代** |
| **多窗口** | 支持（`/eda-windows` + `/eda-windows/select`）；实测 EDA 重开后会生成新 windowId |
| **Bridge 生命周期** | 前台进程；**EDA 会自动重连**（实测断后约 20s 恢复） |

---

## 七、对 A1 的实际意义

### 7.1 现在能做到的事

| 场景 | 做法 |
|---|---|
| **查设计** | agent 读工程 → 列元件/网络/引脚 → 核对是否与我们的需求一致 |
| **改设计** | agent 调用 `modify`/`create` 改属性、加元件、放网络标号 |
| **批量改丝印** | 遍历元件改 `name`/属性（如加组名） |
| **跑检查** | `sch_Drc` / `pcb_Drc` 检查，读结果 |
| **出 Gerber** | `pcb_ManufactureData` 导出 |
| **看渲染** | `getCurrentRenderedAreaImage` 取当前画布图像 |

### 7.2 建议的用法

> **agent 做"读写 + 批量 + 检查"，人做"布局决策与审美"**。

因为：
- 布局布线涉及**空间判断与工程经验**，agent 不擅长
- 但**批量改属性、核对网表、跑 DRC、导 Gerber** 这类机械操作，agent 又快又不会漏

---

## 八、可复用脚本位置

```
Base/easyeda-bridge/                 ← Bridge 与官方 skill
└── easyeda-api-skill/
    ├── scripts/bridge-server.mjs    ← Bridge 服务器
    ├── SKILL.md                     ← 官方 skill 说明（32 KB）
    ├── references/                  ← API 参考文档
    ├── format/                      ← 源文件格式规格
    └── guide/ 、user-guide/          ← 指南
```

> 官方仓库：https://github.com/easyeda/easyeda-api-skill
> 扩展下载：https://ext.lceda.cn/item/oshwhub/run-api-gateway

---

## 九、★ 补充实测（2026-09-29）：能力边界 —— "能改线，不能放件"

**背景**：改板阶段需要往"改"板补 6 个传感器座 + 若干电阻电容。
故在 `参考01_copy` 工程上做了一次**干跑**（建临时废板 → 试写 → 清理），
逐项验证写入能力。**结论推翻了"API 可以自动放件"的预期。**

### 9.1 实测结果（可复核）

| 能力 | 调用 | 结果 |
|---|---|---|
| 读工程/板卡/原理图/PCB 结构 | `dmt_Project/Board/Schematic/Pcb.getAll*` | ✅ 与解析 `.eprj3` **完全一致** |
| 读元件全量信息 | `sch_PrimitiveComponent.getAll()` | ✅ 101 个（原板） |
| **读元件的库链接** | `getState_Component()` / `_Symbol()` / `_Footprint()` | ✅ 返回 `{name, uuid, libraryUuid}`。**uuid 与我解析 `.esch2` 得到的器件 id 逐个吻合** |
| 读引脚与坐标 | `await component.getAllPins()` | ✅（**必须 await**，同步调用返回空数组） |
| 读导线与网名 | `sch_PrimitiveWire.getAll()` → `.net` | ✅ 114 根线、73 根带网名 |
| **建导线 + 直接指定网名** | `sch_PrimitiveWire.create([x1,y1,x2,y2], 'NETNAME')` | ✅ **约 200 ms 完成** |
| **改导线网名** | `sch_PrimitiveWire.modify(wire, {net:'X'})` | ✅ |
| 建板 / 建原理图 / 建 PCB | `dmt_Board.createBoard()` 等 | ✅ |
| 删元件 / 导线 / 板卡 / 原理图 / PCB | 各 `delete*` | ✅（注意：`deleteBoard` **不会**连带删除其 Schematic/PCB，需另删） |
| **★ 从库放置元件** | `sch_PrimitiveComponent.create(comp, x, y)` | ❌ **30 秒超时**（bridge 的 `REQUEST_TIMEOUT_MS`） |
| **★ 解析工程库器件** | `lib_Device.get(devUuid[, libUuid])` | ❌ 返回 `null` |
| 枚举网络名 | `sch_Net.getAllNetsName()` | ❌ 返回空数组（**不可靠**）；`getAllNets()` 返回 0 |
| 工程库 uuid | `lib_LibrariesList.getProjectLibraryUuid()` | ✅ 返回字符串 **`"project"`** |
| 系统库 uuid | `lib_LibrariesList.getSystemLibraryUuid()` | ✅ `0819f05c4eef4c71ace90d822a990e87` |

### 9.2 ★ `create()` 的四种参数写法全部超时

按官方签名，`create()` 的第一个参数可以是 `{libraryType?, libraryUuid, uuid}`。实测：

| 写法 | 结果 |
|---|---|
| `{uuid: DEV}` | ⏱ 30 s 超时 |
| `{libraryUuid: '', uuid: DEV}` | ⏱ 超时 |
| `{libraryType: '3', libraryUuid: '', uuid: DEV}` | ⏱ 超时 |
| `{libraryUuid: 'project', uuid: DEV}` | ⏱ 超时 |

> 枚举值注意：`ELIB_LibraryType.DEVICE = '3'` 是**字符串**，不是数字。
>
> 推测卡死原因：`create()` 内部要按 uuid 去解析库器件，而 `lib_Device.get()` 对
> **工程自带（内嵌）元件**返回 `null` —— 解析不到就一直等。

### 9.3 由此得到的**唯一可行分工**

| 谁 | 做什么 | 为什么 |
|---|---|---|
| **人（EDA 界面）** | **放置元件**（复制粘贴 / 器件面板） | API 放不了件；而界面原生复制粘贴**已被证明有效**：改板上手工贴入的 J9，其 `devUuid` 与原板 J9 **完全相同** |
| **Agent（API）** | **改导线与网名、批量核对、DRC、导出** | 这些是机械且易漏的工作，API 已验证可用且快 |

> **一句话**：~~**人放件，机器接线。**~~
>
> ## ⚠️ **2026-09-30 已被推翻：机器也能放件了** —— 见 §十一

### 9.4 干跑纪律（沉淀为流程）

1. 先**备份工程**（复制整个工程目录即可，`.eprj3` 是纯文本文件夹格式）
2. 在**临时废板**上试写（`createBoard()`），**不动真板**
3. 试完**清理**：删测试图元 → `deleteBoard()` → **再手动删 `deleteBoard` 漏下的 Schematic/PCB**
4. 用**两条独立路径核验**（API 读 + 解析 `.esch2` 文件）确认零残留

> 本次干跑后核验：板卡 2 / 原理图 2 / PCB 2，改板仍为 35 位号 / 58 网络 —— **零残留**。

### 9.5 可复用脚本（本次新增）

| 脚本 | 用途 |
|---|---|
| `tools/eda_exec.ps1` | 通过 bridge 执行 JS。**用 `curl.exe` 发请求**，否则 PowerShell 会吞掉 500 的错误体（拿不到 "timed out" 这类关键信息） |
| `tools/sch_watch.py` | 解析 `.esch2`，列出"改"板位号并与本计划对照 |
| `hardware/sch_netlist.py` | 解析 `.esch2`，列出位号/器件/值/网络名，支持两图 `--diff` |

---

## 十一、★ 重大修订（2026-09-30）：**放件也能用 API 做了**

§九 的"人放件、机器接线"**已被推翻**。

### 11.1 突破点：传"完整器件对象"而不是"引用"

| 写法 | 结果 |
|---|---|
| `create({libraryUuid, uuid}, x, y)` | ❌ 卡死 30 s 超时 |
| `create({libraryType:'3', libraryUuid:''}, x, y)` | ❌ 超时 |
| `create({libraryUuid:'project', uuid}, x, y)` | ❌ 超时 |
| **`create(<`lib_Device.search()` 返回的完整器件对象>, x, y)`** | ✅ **成功，且 `DeviceName` + `Footprint` 一并齐全** |

根因：`lib_Device.get(uuid)` 对**工程内嵌器件**返回 `null`，所以只传引用时 EDA 无法解析器件；
而传完整对象时字段都在手上，直接可用。

### 11.2 由此解锁的能力（2026-09-30 ~ 10-05 实测）

| 能力 | 结论 |
|---|---|
| **放元件** | ✅ 可行（还自带封装与料号）|
| **接线 / 改网名** | ✅ 可行 |
| **写立创料号** | ✅ `modify(id, {supplier:'LCSC', supplierId:'C1234'})`，**不换器件、不弄断导线** |
| **改位号** | ✅ `modify(id, {designator:'R1'})` |
| **改封装** | ❌ **不行** —— 只有 `getState_Footprint()`，**没有 setter**；用别的器件原位替换会因**引脚几何不同而弄断导线**（0603 电阻垂直 30 / 0805 水平 40）。只能在界面里改 |
| **运行 DRC 并读明细** | 🔶 `sch_Drc.check(strict, ui, verbose)` 可用，但 `verbose` 只返回**类型+条数**，拿不到逐条明细 |
| **PCB 自动布线** | ❌ 仍不可行（见 `决策记录.md` §六之补）|

### 11.3 新踩的坑（已沉淀到 `tools/README.md`）

| # | 坑 | 正确做法 |
|---|---|---|
| 1 | **删导线会误伤** | 一条"导线"可能已被 EDA **合并**成多分支折线；**必须要求"所有顶点都落在指定区域内"才删** |
| 2 | **桥有 30 s 超时** | 一批别塞太多 create；**超时不等于没执行**，先回读再重试 |
| 3 | **`PCB1` 标签打开时原理图写不进去** | 表现为 `create failed!` 或挂起，**而读仍正常** |
| 4 | **建导线先删旧线** | 否则新线会继承落在其坐标上的既有网名 |
| 5 | **多条新线不要共享端点** | 否则被合并后可能继承错误网名 |
| 6 | **挪动元件会弄断接线** | 手工挪完**必须重跑逐脚核验**（实测挪 R11/R13/R14 后出现 `PA5` 短接 GND、`R14` 两脚短路）|

---

## 十、变更记录

| 日期 | 内容 |
|---|---|
| 2026-09-23 | 初版：实测验证 agent 接入立创EDA 的完整链路；确认读写 API 可用；记录验证数据与边界 |
| 2026-09-29 | **补充实测：能力边界**。干跑验证发现 **`create()` 放件 4 种写法全部 30 s 超时**、`lib_Device.get()` 对工程内嵌元件返回 `null`；而**建/改导线与网名完全可用**。据此确立"**人放件、机器接线**"的分工；沉淀干跑纪律与可复用脚本 |
| 2026-09-30 | **★ 推翻上述结论**：传 `lib_Device.search()` 的**完整器件对象**即可成功放件（含封装）。新增 §十一。据此，"改"板原理图**全自动完成**（12 只新件 + 约 40 条连线 + 料号 + 位号）|
| 2026-10-05 | 原理图终态核验通过：104 元件 / 52 位号 / **52 个封装齐全 / 52 个料号齐全 / DRC 致命错误 0** |
