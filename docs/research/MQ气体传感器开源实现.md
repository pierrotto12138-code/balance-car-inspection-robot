# MQ 系列气体传感器：开源实现调研

> 调研日期：2026-09-23
> 目的：为「双路 MQ 气体检测（MQ-2 + MQ-135）」找可参考的**算法、标定、补偿**实现。
> **本文件的算法参数均经我本人读源码核实**，标注了出处（文件/行号），并区分"代码实证"与"文档声明"。

---

## 一、结论速览

| 问题 | 答案 |
|---|---|
| **最权威的开源实现** | **`miguel5612/MQSensorsLib`**（Arduino 库名 `MQUnifiedsensor`）—— 223⭐，**MIT**，覆盖 13 个 MQ 型号 |
| **换算法** | 幂律拟合 **`ppm = a · (Rs/R0)^b`**；参数**来自 datasheet 曲线数字化**，不是实测标定 |
| **温湿度补偿** | 有公开做法（GeorgK 体系），但**普遍被忽略**，且**系数未经严格验证** |
| **多传感器交叉验证** | 有电子鼻项目，但**几乎都是 ML 分类，不做定量 ppm** |
| **"MQ + 上位机"齐活的项目** | 极少，且多数只做**阈值判定**不做 ppm 换算 |

---

## 二、首选参考：MQUnifiedsensor（MQSensorsLib）

**仓库**：https://github.com/miguel5612/MQSensorsLib ｜ 223⭐ ｜ **MIT** ｜ 更新 2025-06
**库名注意**：Arduino Library Manager 里叫 **`MQUnifiedsensor`**，GitHub 仓库名却是 `MQSensorsLib`。
**配套文档仓**：https://github.com/miguel5612/MQSensorsLib_Docs （含参数推导的原始数据，见第三节末）

**支持型号**（`library.properties`）：MQ2/3/4/5/6/7/8/9/131/135/136/303A/309A

**可移植性**：纯 C++，无 Arduino 硬依赖的算法部分可直接搬到 STM32 —— 只需把 `analogRead()` 换成 HAL_ADC 读取。

---

## 三、核心算法（★ 我亲自读源码核实）

### 3.1 Rs 计算（`src/MQUnifiedsensor.cpp:193`、`:221`）

```cpp
_RS_Calc = ((_VCC*_RL)/_sensor_volt)-_RL;   // RS = (VCC × RL) / Vout - RL
if(_RS_Calc < 0) _RS_Calc = 0;
```

即 **`Rs = (VCC · RL) / Vout − RL`**，与分压公式推导一致（源码注释里有完整推导）。

### 3.2 ADC → 电压（`MQUnifiedsensor.cpp:68`）

```cpp
_sensor_volt = value * _VOLT_RESOLUTION / (2^ADC_Bits - 1);
```

⚠️ **`_VOLT_RESOLUTION` 默认 5.0**（`MQUnifiedsensor.h:61`，注释写"if 3.3v use 3.3"）。
**我们用 STM32（ADC 参考 3.3V），这个值必须改成 3.3**，否则电压算错。

### 3.3 默认常量（`MQUnifiedsensor.h:61-63`）

| 常量 | 默认值 | 说明 |
|---|---|---|
| `_VOLT_RESOLUTION` | 5.0 V | ADC 参考电压 → **我们要改 3.3** |
| `_VCC` | 5.0 V | 传感器供电电压 |
| **`_RL`** | **10 kΩ** | 负载电阻，可用 `setRL()` 改 |

> ⚠️ **RL 是最容易被忽略、又最影响结果的参数**。见第五节的风险提醒。

### 3.4 ppm 换算公式（`MQUnifiedsensor.cpp:199-211`）

**回归模式 1（指数，默认）**：

```cpp
double logppm = log10((double)_a) + (double)_b * log10((double)_ratio);
ppm = pow(10, logppm);
```

即 **`log10(ppm) = log10(a) + b·log10(Rs/R0)`**，等价于

> ### **`ppm = a · (Rs / R0)^b`**

**回归模式 0（线性）**：`logppm = (log10(ratio) − b) / a`

### 3.5 参数表（★ 代码实证，写在示例的注释里）

**MQ-2**（`examples/MQ-2/MQ-2.ino:44` 用了 LPG 那条）：

| 气体 | a | b |
|---|---|---|
| H2 | 987.99 | -2.162 |
| **LPG** | **574.25** | **-2.222** |
| CO | 36974 | -3.109 |
| Alcohol | 3616.1 | -2.675 |
| Propane | 658.71 | -2.168 |

**MQ-135**（`examples/MQ-135/MQ-135.ino:45` 默认用 NH4）：

| 气体 | a | b |
|---|---|---|
| CO | 605.18 | -3.937 |
| Alcohol | 77.255 | -3.18 |
| **CO2** | **110.47** | **-2.862** |
| Toluene | 44.947 | -3.445 |
| **NH4** | **102.2** | **-2.473** |
| Acetone | 34.668 | -3.369 |

### 3.6 清洁空气标定系数（★ 代码实证）

**这是标定 R0 用的关键系数**，来自 `examples/Calibration/Calibration.ino:29-40`：

| 型号 | 清洁空气 RS/R0 | 型号 | 清洁空气 RS/R0 |
|---|---|---|---|
| **MQ-2** | **9.83** | MQ-7 | 27.5 |
| MQ-3 | 60 | MQ-8 | 70 |
| MQ-4 | 4.4 | MQ-9 | 9.6 |
| MQ-5 | 6.5 | MQ-131 | 15 |
| MQ-6 | 10 | **MQ-135** | **3.6** |

> **多个独立项目数值一致**（MQUnifiedsensor、ESPHome 移植版、MQGasKit），**可信度高**。

### 3.7 标定流程（`MQUnifiedsensor.cpp:243` 起 `calibrate()`）

```cpp
RS_air = ((_VCC*_RL)/_sensor_volt)-_RL;   // 清洁空气中测 Rs
R0 = RS_air / ratioInCleanAir;            // ratioInCleanAir = 上表的系数（MQ-2 用 9.83）
```

**取平均的做法**（`examples/MQ-2/MQ-2.ino:75` 附近）：循环 **10 次** `calibrate()` 累加后
`setR0(calcR0/10)`。注释建议"标定只在 setup 里做一次，之后从 EEPROM 载入 R0"。
另有移植版改成 **100 次**平均。

### 3.8 参数从哪来？（回答"datasheet 还是实测"）

**来自 datasheet 曲线数字化，不是实测标定。**

依据：
- 配套文档仓 `MQSensorsLib_Docs` 的 **`WPDigitalizer/<型号>/`** 目录下，
  每个型号有分气体 CSV（如 `MQ135/CO2.csv`）+ `*_Analisis.xlsx` + 芯片曲线图
  → 即用 **WebPlotDigitizer 从 HANWEI datasheet 的 log-log 曲线取点**，再做幂回归
- `examples/AlgorithmTester/AlgorithmTester.ino` 自述：
  "these values were taken from the same datasheet with which the library was made"，
  并用 ratio/ppm 对照点验证误差 < 5%

> **所以：抄这些参数，本质上抄的是"datasheet 曲线的拟合值"，而不是"某个人实测的结果"。**

---

## 四、温湿度补偿

### 4.1 成体系的公开实现（源出 GeorgK/MQ135）

**MQUnifiedsensor 自带 DHT11 示例**：
`examples/MQ-135-CorrectionFactorDHT11/MQ-135-CorrectionFactorDHT11.ino`

补偿系数（与 GeorgK 一致）：

```cpp
CORR = CORA*t² − CORB*t + CORC − (h−33)*CORD
// CORA = 0.00035,  CORB = 0.02718,  CORC = 1.39538,  CORD = 0.0018
```

**调用方式**：`MQ135.readSensor(false, correctionFactor)`
—— 库函数原生支持，内部是 `_ratio += correctionFactor`（`MQUnifiedsensor.cpp:196`）。

### 4.2 ⚠️ 三个必须知道的坑

| 坑 | 说明 |
|---|---|
| **两种数学处理不同** | MQUnifiedsensor 是**叠加式**（`ratio += fact`），GeorgK 是**除法式**（`Rs/fact`）。**别混用** |
| **作者自己都怀疑** | GeorgK 源码顶部写："*TODO: Review the correction factor calculation. This currently relies on the datasheet but the information there seems to be wrong.*" |
| **PR 无数据支撑** | 该补偿系数来自一个 PR（GeorgK/MQ135#6），**无讨论、未说明数据来源** |

> **结论：可用作起点，但必须自己实测验证，不能当定论。**

### 4.3 普遍被忽略的证据（代码实证）

| 项目 | 情况 |
|---|---|
| `akashhhh22/Gas-Leak-Severity-Monitor` | 有 DHT11，**但只上报，不参与换算** |
| `Bobbo117/MQ135-Air-Quality-Sensor` | README 明文"**makes no effort to correct**" |
| MQUnifiedsensor **主库** | 无补偿（只留了形参） |
| ESPHome 组件、`bairwa25`、STM32 项目 | 均无补偿 |

---

## 五、⚠️ 两个高风险提醒

### 5.1 参数体系互相矛盾，**绝不能混用**

MQ-135 网上流传着**两套**都真实存在、但标定条件不同的参数：

| 体系 | 公式 | 参数 | 出处 |
|---|---|---|---|
| **A（MQUnifiedsensor）** | `ppm = a·(Rs/R0)^b` | CO2: a=110.47, b=-2.862 | datasheet 曲线拟合 |
| **B（GeorgK）** | `ppm = PARA·(Rs/R0)^(−PARB)` | PARA=116.6020682, PARB=2.769034857, RZERO=76.63 | 独立标定，CO2 专用 |

**两者都是实证存在的真实代码，但不是同一标定条件——混用会出错。**

### 5.2 ★ RL（负载电阻）必须实测确认

**这是最实际的风险。** `RobotnikAutomation/mq_gas_sensors` 项目在源码里
**把 RL 从默认的 10kΩ 改成了 1kΩ**（`mq_ros.ino:39,45`：`setRL(1)`），
因为**该模块实际的负载电阻是 1kΩ**。

**若 RL 设错，Rs 会错，ppm 就整体错一个系数。**

> 国产 MQ 模块通常带一个**电位器**调节 RL（或直接固定）。**我们必须实测自己模块的 RL**，
> 否则算出来的 ppm 只能叫"相对指数"，不能叫浓度。

---

## 六、其他可参考项目

### 6.1 与我们的机器人场景最接近

| 项目 | 链接 | 借鉴点 |
|---|---|---|
| **RobotnikAutomation/mq_gas_sensors** | https://github.com/RobotnikAutomation/mq_gas_sensors | **同为 MQ-2 + MQ-135 组合**；把数据发成 ROS topic；README 要求**首次 12–24h burn-in** |
| **akashhhh22/Gas-Leak-Severity-Monitor** | https://github.com/akashhhh22/Gas-Leak-Severity-Monitor | ★ **工程化防抖逻辑**：8 点滑动平均 + **90% 迟滞** + 三级报警（MINOR/MODERATE/SEVERE）+ 静音超时 |
| **bairwa25/air_quality** | https://github.com/bairwa25/air_quality | 端到端链条最完整：ADC→电压→Rs→ratio→log 拟合→ppm→**上传云平台画曲线** |

### 6.2 单传感器（MQ-135）最经典

| 项目 | 链接 | 说明 |
|---|---|---|
| **GeorgK/MQ135** | https://github.com/GeorgK/MQ135 | 182⭐ / GPL-3.0；CO2 参数 `PARA/PARB` 源头；温湿度补偿源头 |
| **NuclearPhoenixx/MQ135** | https://github.com/NuclearPhoenixx/MQ135 | ★ **GeorgK 的修正版**（修正了 Rs 公式里多乘 5 的错误）；README 明确"户外 20°C/33%RH **预热 12–24h** 后取 RZero，多次平均抗噪声" |
| **Bobbo117/MQ135-Air-Quality-Sensor** | https://github.com/Bobbo117/MQ135-Air-Quality-Sensor | **48h 预热**；两步式 `CALIBRATE` 模式（先测再写回 RZERO）；明确声明不做补偿 |

### 6.3 多传感器交叉验证 / 电子鼻

**几乎都是 ML 分类，不做定量 ppm**：

| 项目 | 说明 |
|---|---|
| `kbhujbal/Artificial-Nose-ML-...` | 8 个 MOS 传感器阵列 + RandomForest 分类（PIC 单片机），IEEE 论文 2023 |
| `anemo-labs/SmellDB` | 电子鼻基准数据集/竞赛 |
| `TheHanys/Electronic-Nose-Graduation-Project` | 毕业设计 |
| `WuFanaa/electronic-nose-co-` | 中文，CO 浓度预测，实验室自采数据 |

**最轻量的"多气体交叉"其实是 MQUnifiedsensor 的 `MQ-135-ALL.ino` 示例**：
同一个 Rs/R0 值分别代入 6 组 a/b，算出 CO/Alcohol/CO2/Toluene/NH4/Acetone 六条浓度曲线。

---

## 七、对我们项目的建议

### 7.1 推荐组合（抄谁）

| 模块 | 抄哪里 | 拿到什么 |
|---|---|---|
| **算法核心** | `MQUnifiedsensor` | Rs 公式、ppm 幂律公式、13 型号的 a/b 表、清洁空气系数 |
| **标定流程** | `MQ-2.ino` + `Bobbo117` | 10–100 次平均；12–48h 预热；两步式 CALIBRATE 模式 |
| **防抖与报警** | `Gas-Leak-Severity-Monitor` | 滑动平均 + 迟滞 + 分级报警 |
| **温湿度补偿** | `MQ-135-CorrectionFactorDHT11.ino` | 现成公式（DHT11 已在我们清单里），**但要实测验证** |
| **上位机** | `bairwa25` 的数据链条 | ADC→ppm→曲线的完整链路 |

### 7.2 移植到 STM32 的注意点

1. `analogRead()` → **HAL_ADC 读取**（把 12 位值归一化到 0–1 再乘参考电压）
2. `_VOLT_RESOLUTION` **改成 3.3**（STM32 的 ADC 参考是 3.3V，不是 Arduino 的 5V）
3. **必须先实测模块的 RL**（见 5.2），再用 `setRL()` 设定
4. 标定的 R0 **存 Flash**（基底已有 `Flash_Write/Read`），避免每次上电重标
5. 我们只用 2 路（MQ-2 + MQ-135），不需要 13 型号的完整表

### 7.3 ★ 一个需要你定夺的设计问题

**我们要不要报"ppm"？**

MQ 传感器即使按上述方法标定，**绝对浓度的真实精度也很差**（datasheet 本身就标 ±几十%，
且受温湿度、老化、批次影响）。所以：

| 方案 | 优点 | 代价 |
|---|---|---|
| **报 ppm** | 报告好写、"浓度场"叙事强 | 精度站不住，若老师追问"你怎么保证这个 ppm 准"，难答 |
| **报相对指数 Rs/R0** | **诚实**——这正是传感器真正擅长测的量 | 叙事弱一些 |
| **两者都报**（推荐） | ppm 用于报告与阈值展示；**Rs/R0 用于跨通道比较与趋势分析** | 略多一点代码 |

> ★ **关键洞察**：我们的任务是"**泄漏巡检与定位**"，需要的其实是
> **灵敏度（有没有气）+ 相对比较（哪个通道高、梯度方向）**，**而不是绝对浓度精度**。
> 而**跨通道比较用 Rs/R0 比值比用 ppm 更可靠**（避免各自的标定误差叠加）。

**建议**：主链路用 **Rs/R0 相对量**做交叉验证与超阈值判定，
ppm 作为"估计值"展示（并注明是 datasheet 曲线估算）。
这样既好看，又经得起追问。

---

## 八、检索限制（如实说明）

| 平台 | 状态 |
|---|---|
| GitHub | ✅ 可用（`api.github.com` 匿名限流 60/h，多次耗尽后改用 `raw.githubusercontent.com` 直取文件） |
| **即使用 raw 直取，少数文件仍 404** | 已标注"未确认"，未编造 |
| Gitee | ❌ 匿名 API 搜索返回空数组 |
| oshwhub | ❌ 客户端渲染，未抓取 |

**本文件中所有算法参数均指向具体文件与行号，可自行复核。** 未标注出处的表述已注明是
"README 声明"或"未确认"。

---

## 九、已核实存在的仓库

以下仓库均为 **HTTP 200 实测确认**（2026-09-23），且部分源码已下载逐行核对：

```
miguel5612/MQSensorsLib                 200  ★ 源码已核实
GeorgK/MQ135                            200
NuclearPhoenixx/MQ135                   200
RobotnikAutomation/mq_gas_sensors       200
akashhhh22/Gas-Leak-Severity-Monitor    200
bairwa25/air_quality                    200
```

**已逐行核实的文件**：
- `MQSensorsLib/src/MQUnifiedsensor.cpp`（Rs 公式 :193/:221、ppm 公式 :199-211、correctionFactor :196）
- `MQSensorsLib/src/MQUnifiedsensor.h`（RL=10 :63、VCC=5 :62、VOLT_RESOLUTION=5 :61）
- `MQSensorsLib/examples/MQ-2/MQ-2.ino`（清洁空气 9.83 :32、LPG a/b :44、10 次平均 :75）
- `MQSensorsLib/examples/MQ-135/MQ-135.ino`（清洁空气 3.6 :33、NH4 a/b :45、6 气体参数表 :46-52）
- `MQSensorsLib/examples/Calibration/Calibration.ino`（12 型号清洁空气系数表 :29-40）
- `MQSensorsLib/examples/MQ-135-ALL/MQ-135-ALL.ino`（**CO2 +400 偏移 :109**，原因见 :102-106）

---

## 十、变更记录

| 日期 | 内容 |
|---|---|
| 2026-09-23 | 初版：检索 MQ 系列开源实现；核心算法与参数逐行核实；提出"报 ppm 还是相对指数"的设计问题 |
