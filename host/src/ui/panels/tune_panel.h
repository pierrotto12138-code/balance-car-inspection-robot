#pragma once

// ============================================================================
//  TunePanel —— 调参与记录（P4）
//
//  四块内容，围绕"改一个参数要多久能看到效果"这件事：
//    ① **参数表**：7 个 PID 参数的 ID/名称/基底变量/当前值/单位/允许范围/默认值/说明；
//       选中一行 → 在上方输入框里改 → 「下发」；也可以「全量回读」(0xFF)。
//       **下发前在上位机侧就校验范围**（协议 §3.8）：越界在界面上直接拒绝并说明原因，
//       否则固件"拒绝并回旧值"，现象是"改了没生效"，人会去查链路（查错方向）。
//    ② **Flash 操作**：保存(0x00) / 从 Flash 载入(0xFE) —— 掉电保存，避免每次重启回初值。
//    ③ **观测模式(0xFD)**：开启后固件以 100 Hz 在有线上报 0x10+0x11（用于 PID 整定），
//       界面上必须写明"这是临时状态、别忘了关"。
//    ④ **三环实况曲线(0x16)** + **下发日志**：把"下发值 ↔ 回读值"成对显示，
//       不一致就是"以为改了、其实没收到"的第一现场（这条日志比曲线更值钱）。
//
//  ★ 数据来源：`data::ParamStore`（账本）+ `domain::VehicleState`（回读）。
//    界面**不自己判断"改没改成功"**，只显示账本的结论（单一判据）。
// ============================================================================

#include "data/param_store.h"
#include "domain/vehicle_state.h"

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

class ScopePlot;

namespace data {
class EventLog;
class SessionStore;
}
namespace domain {
class KeepaliveEngine;
}
namespace io {
class LinkManager;
}

class TunePanel : public QWidget
{
    Q_OBJECT
public:
    TunePanel(domain::VehicleState *state, io::LinkManager *mgr, data::ParamStore *params,
              data::EventLog *log, QWidget *parent = nullptr);

public slots:
    /// 10 Hz：回读值刷新表格 + 曲线重绘
    void refresh();
    /// 每成功解出一帧（0x16 三环 → 曲线）
    void onDataUpdated(quint8 cmd);

private:
    void buildUi();
    void rebuildTable();
    void updateSelectedRow();
    /// 下发单个参数（返回是否真的发了）
    bool sendParam(quint8 id, double value);
    void sendFlash(quint8 extId, const QString &what);
    void sendFullReadback();
    void sendSurveyCfg();
    void appendLogRow(const QString &action, const QString &sent, const QString &readback,
                      const QString &verdict, bool ok);

    domain::VehicleState *m_state = nullptr;
    io::LinkManager *m_mgr = nullptr;
    data::ParamStore *m_params = nullptr;
    data::EventLog *m_log = nullptr;

    QTableWidget *m_table = nullptr;
    QDoubleSpinBox *m_valueEdit = nullptr;
    QLabel *m_rangeHint = nullptr;
    QLabel *m_selectedName = nullptr;
    QPushButton *m_btnSend = nullptr;
    QPushButton *m_btnReadAll = nullptr;
    QPushButton *m_btnSaveFlash = nullptr;
    QPushButton *m_btnLoadFlash = nullptr;
    QCheckBox *m_observe = nullptr;
    QLabel *m_observeHint = nullptr;
    QLabel *m_lastAction = nullptr;

    QTableWidget *m_logTable = nullptr;

    // 0x23 巡检参数
    QSpinBox *m_periodMs = nullptr;
    QDoubleSpinBox *m_mq2Limit = nullptr;
    QDoubleSpinBox *m_mq135Limit = nullptr;
    QCheckBox *m_adaptive = nullptr;
    QCheckBox *m_compensation = nullptr;
    QSpinBox *m_confirmFrames = nullptr;
    QLabel *m_surveyHint = nullptr;

    ScopePlot *m_loopPlot = nullptr;   ///< 三环分量：Bal / Vel / Turn
    ScopePlot *m_execPlot = nullptr;   ///< 执行链路：左 Moto vs CCR_A（"算了没执行"一眼可见）

    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock; ///< 曲线横轴的时间基（面板自己的经过时间）
};
