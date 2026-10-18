# 参考仓库调研记录

> 记录本项目为寻找可复用硬件/软件基底所做的检索结论，避免重复劳动。
> 更新日期：2026-09-22

---

## 一、检索目标

我方需为「基于平衡小车的仓储环境安全巡检机器人」寻找：
1. 可复用的**开源平衡车基底**（固件 + 硬件设计）
2. 小车上两块拓展板的**可编辑工程源文件**（立创EDA / Altium / KiCad）

---

## 二、结论速览

| 目标 | 结论 |
|---|---|
| 平衡小车之家 MiniBalance **完整开源仓库（含 EDA）** | ❌ **未找到**。公开流传的只有**固件 + PDF 原理图**，且平台是 **F103**，非 F405 高配版 |
| **F405RGT6 控制板**原工程（丝印 `Wang_HonSan 2013-11-27`） | ❌ **未找到** |
| **MPU6050 模块**原工程（SPX3819 + L2N7002 电平转换） | ❌ **未找到** |
| 功能等价的**替代设计** | ✅ **很多，可直接使用** |

---

## 三、关键检索发现

### 3.1 官方渠道已失效

- `balancecarhome.com` 与 `www.balancecarhome.com` → **NXDOMAIN**（域名不存在）
- 固件源码中留的淘宝店铺号 `114407458` → GitHub 与 Bing 搜索**均无有效命中**

### 3.2 名为 MiniBalance 的仓库，全部只有固件

| 仓库 | 内容 | 有无 EDA |
|---|---|---|
| `shuai132/BalanceCar` (236★) | **唯一带完整 Altium 工程的**（`cannonrobot.PrjPCB/.SchDoc/.PcbDoc` + TB6612 库 + SolidWorks） | ✅ 有，但硬件是 **STM32F401 + LSM6DS3 + BlueNRG**，与 F405+MPU6050 **不符** |
| `whitedeer996/BalanceCar-StudyMaterials` (5★) | 平衡小车之家资料：开发笔记 PDF、`代码对应原理图.pdf`、调试指南、固件 zip | ❌ 仅 PDF + F103 固件 |
| `xiesicong/Balance_car` (10★) | `平衡小车.zip` = F103 Keil 固件 | ❌ |
| `zhumj1991/miniBalance` (2★) | Keil + FreeRTOS 固件（含 `PacketProtocol.xlsx`，疑为通讯协议表） | ❌ |
| `HUSTEPIC/MiniBalanceStm32F407` (1★) | 移植到 F407 的固件（含 `DataScope_DP`） | ❌ |
| `Rickenbacker620/MiniBalanceReborn` (1★) | F103 固件重构（含 `DataScope_DP` 原始文件） | ❌ |
| `wudan133/MiniBalanceCar`、`zycczy/minibalance`、`TheLionOfMyHeart/MiniBalance`、`WaterFishJ/micropython_minibalance` | 均为固件 | ❌ |

> ⚠️ 注意：`SchildCode/MiniBalance` 是**暖通空调风量平衡**的电子表格 App，与本车无关（易被名字误导）。

### 3.3 F405 相关仓库均为「同类板」，无一吻合目标特征

目标的特征组合是：`J-LINK HDR1X4` + `P3/P4 双 2×12` + `P2 2×4 LED` + `SS34` + `AMS1117-3.3` + `8MHz/32.768kHz 双晶振` + **无 USB** + 49×49mm。

| 仓库 | 为何不匹配 |
|---|---|
| `WeActStudio/WeActStudio.STM32F4_64Pin_CoreBoard` (94★) | 核心板形态，**含 USB**、无 4 脚 J-LINK 头、无 2×4 LED 头；且 `Hardware/` 下**只有 PDF + STEP，无可编辑源** |
| `JordanAceto/STM32F405RGT6_breakout_board` (3★) | 命中 AMS1117、10µH，但为通用 breakout，无 J-LINK/2×12/LED 排针 |
| `MURPHYENGINEERING/STM32F405RGT6-Breakout` (1★) | 通用 breakout |
| `PhilCarPi/STM32F4_Breakout_Board` (2★) | F405 + **板载 MPU6050 一体板**（KiCad，4 层）——结构不同 |
| `angst7/stm32-breakout-mk2` (6★) | 通用 64 脚 breakout |
| `EchelonForce/stm32f405_template_bob` (0★) | F405RG 模板板 |

### 3.4 MPU6050 模块：常见模组均为「无电平转换」版本

| 仓库 | 与目标的差异 |
|---|---|
| `pms67/MPU6050_Breakout` (24★，**Altium 源文件**) | 最接近，但**无 L2N7002 双向电平转换、无 SPX3819、非 8 脚 JP1** |
| `RealHaakon/MPU6050-Breakout`、`Harihara0309/MPU6050_breakout-board`、`atakaldirak/...` | 同上，均为裸 breakout |

> 目标的 `L2N7002` 双向 I²C 电平转换是**标准电路**（两颗 N-MOS + 上拉），
> 可用 `pms67/MPU6050_Breakout` 为底自行补上，无需等原版。

---

## 四、可直接使用的替代参考

若需要重新设计，以下仓库含**可编辑源文件**：

| 用途 | 仓库 | 格式 | 匹配度 |
|---|---|---|---|
| **F405RGT6 + 板载 IMU 一体板** | `PhilCarPi/STM32F4_Breakout_Board` | KiCad（含 `MPU_6050.sch`）+ gerber | 功能最接近「F405+IMU」组合 |
| **F405RGT6 通用 breakout** | `MURPHYENGINEERING/STM32F405RGT6-Breakout` | KiCad | 形态最接近排针引出式 |
| 同上 | `JordanAceto/STM32F405RGT6_breakout_board` | KiCad + gerber | — |
| **MPU6050 模块** | `pms67/MPU6050_Breakout` | **Altium**（PrjPcb + PcbDoc）+ JLC BOM/gerber | 需自行补电平转换 |
| **F405RGT6 最小系统（作 A1 参考）** | 我方已有的 `SCH_STM32F405RGT6_2026-07-24.pdf` | 立创EDA 导出 PDF | **★ 最省事：直接照抄** |

---

## 五、检索工具的限制（如实说明）

以下平台在此环境下**无法有效检索**，故结论存在盲区：

| 平台 | 状态 |
|---|---|
| **oshwhub（立创开源）** | 页面 200 但结果**纯客户端渲染**（连 MPU6050 都显示 0 条）；API 返回 `104001`/`401`；`sitemap.xml` 返回 **418**；CloudWAF 拦截 → **实际无法检索** |
| Gitee | 匿名 API 恒返回 0；网页端客户端渲染 |
| GitHub 代码搜索 | `401 Requires authentication`（需登录） |
| Sourcegraph / grep.app / searchcode | 分别返回防火墙拦截 / Vercel 验证 / 404 |
| DDG lite / Ecosia / Brave / Startpage | 被拦截或空响应 |
| CSDN 下载 | 需登录/积分，未取到实际文件 |

**可用**：GitHub REST API（仓库级）、cn.bing.com（但精确短语与 `site:` 筛选基本失效）

> ⚠️ **存在「曾上传到 oshwhub 但未公开/已下架」的可能，无法排除。**
> 建议人工在浏览器登录立创开源广场，用「平衡小车之家 / MiniBalance / F405 平衡车 / Wang_HonSan」检索确认。

---

## 六、对本项目的实际结论

1. **原版 EDA 拿不到**，但**不影响推进**——因为：
   - 我方**已有这两块板的完整原理图 + PCB + BOM**（立创EDA 导出 PDF），足以在立创EDA 中重画；
   - 这两块板都是**2013 年风格的通用设计**（F405 最小系统 + MPU6050 模块），社区等价替代极多。
2. **A1 最省力的路线**：直接依据手上的 PDF 在立创EDA 专业版中重绘，而不是去别处找开源工程来改。
3. **仍应设法获取 `.epro` 源文件**：这些 PDF 是立创EDA 导出的，说明源工程存在，
   优先向资料来源方（老师/学长/实验室）索取，比重新画快得多。

---

## 七、变更记录

| 日期 | 内容 |
|---|---|
| 2026-09-22 | 初版：记录对 MiniBalance 基底、F405 控制板、MPU6050 模块的检索结论与替代方案 |

---

> **项目归属**：中国石油大学（华东） 专业生产实习课程项目<br>
> **范围说明**：本项目不涉及 MiniBalance 控制板（STM32F405RGT6 版本）的硬件设计与改版；控制板直接沿用，A1 仅重做主底板。<br>
> **许可**：原创代码与文档采用 MIT License；第三方组件遵循各自原始许可证。