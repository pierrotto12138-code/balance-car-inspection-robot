# 第三方：QCustomPlot 2.1.1（原样内置，未改动）

**为什么内置源码而不是用系统的曲线模块**（依据：`docs/design_host/C部分方案与计划.md` §2.4、决策 58）：

| 方案 | 结论 |
|---|---|
| **QCustomPlot**（本目录） | ✅ 选它：Qt Widgets 体系中实时曲线风险最低，自带缩放/十字定标/图例，**不依赖 QML** |
| Qt Charts | 🔶 备选：官方，但 Qt6 中已被 Qt Graphs 取代，社区资料新旧混杂 |
| Qt Graphs | ❌ 不选：依赖 QML/Quick 组件，Widgets 集成与文档仍新，踩坑成本高 |

## 来源与许可

| 项 | 内容 |
|---|---|
| 版本 | **2.1.1**（2022-11-06 发布） |
| 来源 | <https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz>（官方源包，取其中 `qcustomplot.h` / `qcustomplot.cpp`） |
| 许可 | **GPLv3**（见同目录 `GPL.txt`）—— 课程项目按其条款使用；**若将来要闭源分发，这里必须替换** |
| 内置文件 | `qcustomplot.h`、`qcustomplot.cpp`、`GPL.txt`、`changelog.txt`（均**未做任何修改**，便于日后比对升级） |

## 构建约定（见 `host/CMakeLists.txt` 的 `qcustomplot` 目标）

1. **`QT_NO_PRINTER`**：我们不做打印导出，定义它可以少一个 `Qt6::PrintSupport` 依赖（打包更小）；
2. **对第三方源码关闭告警**（`target_compile_options(... PRIVATE -w)`）：本项目对**自研代码**要求零告警
   （见 `firmware/Makefile` 的 `make strict` 思路），但第三方源码的告警不该混进我们的自检里 ——
   关掉它的同时**不改它的代码**，这样升级时能干净地整体替换；
3. 使用方（`hostui`）只需 `target_link_libraries(... qcustomplot)`，不用自己加包含路径。

## 我们只用它做什么

`host/src/ui/widgets/scope_plot.*` 是唯一的封装层：通道/环形缓冲/阈值线/事件标记/十字定标/缩放全在那里，
**其余界面代码不直接碰 QCustomPlot** —— 这样万一将来要换实现（或改用自绘），只动一个文件。
