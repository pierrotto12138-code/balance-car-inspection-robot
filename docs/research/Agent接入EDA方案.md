# Agent 接入 EDA：可行方案调研

> 调研日期：2026-09-23
> 目的：寻找**让 AI agent 程序化编辑 EDA 工程**的成熟方案，以降低 A1 画底板的负担。
> **本文件的关键链接与包名均经 HTTP/registry 实测核实**（见第七节）。

---

## 一、结论速览

| 问题 | 答案 |
|---|---|
| **立创EDA 有官方 API 吗？** | ✅ **有**，且是一等公民（官方 JS/TS 扩展 API，全局对象 `eda`） |
| **有现成的"agent 接入"方案吗？** | ✅ **有官方方案**：`easyeda-api-skill` + `eext-run-api-gateway`（WebSocket 桥） |
| **有官方 MCP server 吗？** | ❌ 没有。社区有（`hyl64/jlcmcp` 最全），非官方 |
| **能不用 GUI（headless）吗？** | ❌ **不能**。必须开着立创EDA 客户端，手动打开工程 |
| **最推荐路线** | ★ **官方 WebSocket 桥 + `easyeda-api-skill`**，agent 直接 POST JS 到客户端执行 |

---

## 二、★ 推荐路线：官方 API 桥（能力最全、写入最安全）

### 2.1 三个组件

| 组件 | 来源 | 作用 |
|---|---|---|
| **`run-api-gateway.eext`** | 官方扩展，从 https://jlcext.com/item/oshwhub-official/run-api-gateway 下载 | 装在立创EDA 客户端里，**扫描 49620–49629 端口**自动连回 Bridge |
| **`easyeda-api-skill`** | https://github.com/easyeda/easyeda-api-skill | 官方 skill 包：结构化 API 文档 + **WebSocket Bridge（Node.js）** + 源格式规格 + `SKILL.md` |
| **HTTP 接口** | Bridge 服务器本地端口 | agent 通过 `POST /execute` 把 JS 送进 EDA 进程执行 |

### 2.2 工作方式（实测自官方 README）

```bash
npm install
npm run build:docs    # 生成结构化 API 文档
npm run server        # 起 Bridge，自动选 49620-49629 中的空闲端口
```

然后在**立创EDA 扩展管理器**里导入 `run-api-gateway.eext`，它会自动扫描端口并握手。

**调用示例**：

```http
GET  http://localhost:49620/health
POST http://localhost:49620/execute
     {"code":"return await eda.dmt_Project.getCurrentProjectInfo();"}
```

→ **在 EDA 进程内执行任意 JS 并取回结果**。

### 2.3 能力范围（官方 API 命名空间）

| 域 | 命名空间 | 能做什么 |
|---|---|---|
| 系统 | `SYS_FileSystem` / `SYS_Dialog` / `SYS_Message` / `SYS_Storage` / `SYS_FormatConversion` | 文件、对话框、存储、格式转换 |
| 工程/文档 | `DMT_Project` / `DMT_Board` / `DMT_Schematic` / `DMT_Pcb` / `DMT_SelectControl` | 工程与文档管理 |
| **原理图** | `SCH_Document` / `SCH_Net` / **`SCH_Netlist`** / `ISCH_PrimitiveComponent` / `ISCH_PrimitiveWire` / `ISCH_PrimitivePin` | 器件、导线、引脚、网络标签、**网表** |
| **PCB** | `PCB_Document` / `PCB_Net` / **`PCB_Drc`** / **`PCB_ManufactureData`** / `IPCB_PrimitiveVia` / `IPCB_PrimitivePad` | 层、网络、焊盘、过孔、覆铜、走线、**DRC**、**制造数据导出（Gerber）** |
| 库 | `LIB_Device` / `LIB_Footprint` / `LIB_Symbol` / `LIB_3DModel` | 器件/封装/符号/3D 模型 |

> **即：增删元件、改网络、加接口、改丝印、跑 DRC、导出 Gerber 都能做**，
> 且因为走的是编辑器内核，**ID 与网表一致性由编辑器维护**（比直接改文件安全）。

### 2.4 ⚠️ 代价与风险

| 项 | 说明 |
|---|---|
| **必须开 GUI** | 无 headless；必须手动打开工程、并开启「**允许外部交互**」安全开关 |
| **端口依赖** | 占用 49620–49629，需防火墙放行本地回环 |
| **等价任意代码执行** | `/execute` 能在 EDA 进程里跑任意 JS —— 安全边界要注意（本地使用风险可控） |
| **版本兼容** | 我们的客户端是 **4.1.60**，需实测与 skill 版本是否匹配 |
| **流程非规范** | 这是"官方支持的调试通道"，不是设计给纯自动化的 CI 流程 |

---

## 三、备选路线

### 3.1 社区 MCP（非官方，但功能全）

| 项目 | 链接 | 能力 | 风险 |
|---|---|---|---|
| **`hyl64/jlcmcp`** | https://github.com/hyl64/jlcmcp | **59 个工具**：状态查询、元件操作、布线/过孔、铺铜、丝印、约束、原理图（`sch_get_state`/`sch_get_netlist`/`sch_run_drc`）、BOM 导出、DRC 自修复、**`sch_generate_from_netlist`（从网表生成原理图）** | MIT；**agent tool 需 Anthropic key**；自动布线仅基础 L 型；LCSC 查询受限 |
| **`oaslananka/easyeda-mcp-pro`** | https://github.com/oaslananka/easyeda-mcp-pro | 最多约 115 工具：网表/元件/导线编辑、BOM、DRC/ERC、层叠、**导出 Gerber/贴片坐标/PDF/网表** | 许可为 **PolyForm Noncommercial**（商用需授权）；原理图写 API 依赖 beta 功能 |
| `@yuanzihao/schemcp` | https://github.com/yuanzihao/SchemCP | **只读**解析原理图 | 不能改，只适合让 agent"看懂" |

> **共同点**：它们底层**都走官方的 WebSocket 桥**（不是解析文件）。
> 所以**直接用官方 skill 最干净**，社区 MCP 只是封装。

### 3.2 迁移到 `.eprj3` 文件夹格式（headless 方向）

- 官方 `easyeda-eprj3-skill`（名字是 `easyeda-eprj3-skill`，**不是** `eprj3-skill`）：
  https://github.com/easyeda/easyeda-eprj3-skill
  用脚本链生成/编辑 `.eprj3`（`init.js` → `add-symbol.js` → `add-wire.js` → `add-footprint.js` → `add-track.js` → `validate.js`），产出可在 **EasyEDA Pro V4.1+ 打开**（**我们 4.1.60 符合**）
- `.eprj3` = **文件夹 + JSON/纯文本**，Git 友好、可直接访问
- **代价**：需先把现有工程转过去；skill 生态较年轻

### 3.3 转 KiCad（自动化天花板，但成本最高）

| 事实 | 说明 |
|---|---|
| KiCad 有官方 **`kicad-cli`**（headless）：DRC/ERC/Gerber/STEP/网表/BOM 全支持 | 这是立创**没有**的能力 |
| KiCad 文件是 **S-expression 纯文本**，易于程序生成 | — |
| KiCad 有成熟 **MCP 生态**（`mixelpixx/KiCAD-MCP-Server` 约 2.4k★，可写） | — |
| **立创 → KiCad 整工程转换：未找到成熟工具** | 只有反向的 `kicad-to-easyeda-eprj3` |

> **代价**：等于换工具链，重学、重画，且打板流程要改（虽然嘉立创接受标准 Gerber）。
> 对 3 周课程项目**不划算**。

### 3.4 代码化硬件设计（不解决"改现有工程"）

`atopile`（3.9k★）/ `SKiDL` / `circuit-synth` / `tscircuit` 等用代码描述电路，
但**没有一个能做布局布线**，都止步于"生成待布线的电路/PCB 文件"。
→ 适合从零设计，**不适合"改我们现有的底板"**。

---

## 四、官方文件格式文档（重要收获）

官方已**公开格式文档**（GitHub `easyeda` 组织下）：

| 仓库 | 内容 |
|---|---|
| `easyeda-pro-file-format-v2` | **V1/V2 格式**（即我们的 `.epro`）——含中英文档 + 3 份 PDF（通用/原理图/PCB） |
| `easyeda-pro-file-format` | **V3 格式**（`.epro2`） |
| `easyeda-pro-eprj3-format` | `.eprj3` 新离线格式（文件夹式） |
| `easyeda-viewer` | `epro2`/`eprj3` **只读**解析与渲染器（可作参考实现） |

**格式代际对照**：

| 扩展名 | 含义 | 存储 |
|---|---|---|
| `.epro` | V1/V2 工程归档 | **ZIP**（我们手上的开源工程就是这个） |
| `.eprj` / `.eprj2` | 旧离线工程 | **SQLite 单文件**（官方评价：难版本管理） |
| `.epro2` | V3+ 工程归档 | ZIP + `type\|\|data` 行 |
| `.eprj3` | 新离线工程 | **文件夹 + 纯文本（Git 友好）** |

> ⚠️ **注意版本差异**：我们手上的 `.epro` 是 **V2**（`DOCTYPE SCH 1.1` / `PCB 1.8`），
> 而最新文档描述的是 **V3**。同是 ZIP 但内部行结构不同，**解析器不能通用**。

---

## 五、对 A1 的实际含义

### 5.1 三条可选路线

| 路线 | 做法 | 适合 | 代价 |
|---|---|---|---|
| **A. 官方桥 + agent 辅助** | 装 `run-api-gateway.eext` + 起 Bridge；agent 通过 `/execute` 帮改丝印、加接口、查网表、跑 DRC、导 Gerber | **想在现有工程上快速改版** | 必须开着 GUI；安全开关；版本需实测 |
| **B. 纯人工 + agent 查资料** | A1 在立创EDA 里手动改；agent 只提供"改哪里、怎么接"的清单 | **最稳，不折腾** | 全手工 |
| **C. 迁 `.eprj3`** | 用官方 skill 生成新格式工程 | 想要 Git 友好 / headless | 需先迁移；生态年轻 |

### 5.2 ★ 我的建议

> **推荐 "B 为主 + A 为辅"**：
>
> 1. **主线走 B**：A1 在立创EDA 里手动改往届底板工程（专业版已确认能打开往届 `.eprj2`）。
>    改版内容不复杂（加 2 路 MQ 接口 + 分压、裁剪不用接口、改丝印），手工可控。
> 2. **用 A 做辅助**：等改版进行到需要**批量操作**（如"改 20 处丝印"、"检查所有网络"、
>    "导出 Gerber"）时，再装官方桥，让 agent 直接执行 —— **省力但不阻塞主线**。
>
> **理由**：`/execute` 虽然强，但**必须开 GUI + 开安全开关 + 版本要匹配**，
> 这些前提没验证前，**不应把它当作关键路径**。而 A1 的改版工作量本身不大。

### 5.3 立即可做的一件事

**装官方桥并做一次连通性验证**（约 20 分钟）：
1. 从 https://jlcext.com/item/oshwhub-official/run-api-gateway 下扩展
2. `git clone https://github.com/easyeda/easyeda-api-skill`，`npm install && npm run server`
3. 立创EDA 里导入扩展、开「允许外部交互」
4. `curl http://localhost:49620/health` 看是否通
5. 通了就 `POST /execute` 试一句 `return await eda.dmt_Project.getCurrentProjectInfo();`

> **若这一步跑通，后续 agent 就能直接操作工程**；跑不通就安心走路线 B，不影响进度。

---

## 六、明确"未找到"的项（不编造）

- ❌ **官方 MCP server** —— 立创EDA **没有**厂商官方 MCP，现有全是社区项目
- ❌ **官方 headless CLI** —— 无 `kicad-cli` 的对应物；扩展必须跑在 GUI 进程内
- ❌ **`@jlceda/pro-api` 这个 npm 包** —— **不存在**（registry 返回 404）。
  真实存在的是 **`@jlceda/pro-api-types`**（类型定义）和 **`@jlceda/mcli`**（命令框架）
- ❌ **立创"导入网表"功能** —— 官方文档未找到该能力，**不要把"生成网表再导入"当可靠方案**
- ❌ **立创 → KiCad 整工程转换工具** —— 只有反向（KiCad → `.eprj3`）
- ❌ **`.epro`(V2) / `.eprj2` 的开源读写库** —— 未找到；官方有格式 PDF 但无现成库

---

## 七、核实记录（本次实测）

### 7.1 仓库/文档可达性（HTTP 实测）

```
github.com/easyeda/easyeda-api-skill                    200  ★ 官方 skill
github.com/easyeda/eext-run-api-gateway                 200  ★ 官方桥扩展
github.com/easyeda/easyeda-eprj3-skill                  200  （注意：带 easyeda- 前缀）
github.com/hyl64/jlcmcp                                 200  社区 MCP（59 工具）
github.com/oaslananka/easyeda-mcp-pro                   200  社区 MCP（115 工具）
github.com/salitronic/eda-agent                         200  Altium/KiCad/立创三后端
prodocs.lceda.cn/cn/api/reference/pro-api.html          200  官方 API 文档
```

### 7.2 npm 包核实（registry 实测）

```
@jlceda/pro-api-types    200  ✅ 存在（类型定义）
@jlceda/pro-api          404  ❌ 不存在（常见误写）
@jlceda/mcli             200  ✅ 存在（命令框架）
@yuanzihao/schemcp       200  ✅ 存在（只读原理图 MCP）
```

### 7.3 本地环境实测

| 项 | 值 |
|---|---|
| 立创EDA 专业版 | `D:\0_APP\lceda-pro`，Electron，版本 **4.1.60**，productName `JLCEDA_Pro` |
| 用户目录 | `%USERPROFILE%\Documents\LCEDA-Pro\`（含 `projects`、`example-projects`、`libraries`） |
| 本地示例工程格式 | 全部是 **`.eprj2`**（SQLite） |
| 客户端认识的格式 | `app.js` 内出现 `eprj3`×24、`eprj2`×10、`epro`×4 → **新旧两代都认** |
| 我们的 `.epro` 实际格式 | ZIP，157 条目；`DOCTYPE SCH 1.1` / `PCB 1.8` → **V2 行式 JSON** |

---

## 八、变更记录

| 日期 | 内容 |
|---|---|
| 2026-09-23 | 初版：调研 agent 接入 EDA 的方案，确认官方 API 桥路线；核实所有关键链接与包名 |
