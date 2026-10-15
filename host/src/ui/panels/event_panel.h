#pragma once

// ============================================================================
//  EventPanel —— 事件流（P2）
//
//  按时间顺序显示两类条目（同一个 `data::EventLog`）：
//    · 机器人事件帧 `0x13`（含安全通道 `0x08` 失联 / `0x0A` 急停 / `0x09` 解除）；
//    · 上位机判定的告警发生/解除。
//
//  三条设计要点：
//    ① **安全事件成对显示**：`0x09` 那一行直接给出"停车时长 x s" —— 复盘时不必
//       自己在两行之间做减法（协议 §七 约定 9：事件给时刻、标志给状态）；
//    ② **本机条目与机器人条目分色**：谁说的话要能分清（机器人 = 中性色；本机告警 = 红/绿），
//       否则"车说没事、上位机说有事"这种矛盾会被表格混在一起看不出来；
//    ③ **过滤 + 统计常显**：安全事件条数、最短停车时长始终显示在底部 ——
//       这是任务书"安全"一项**可量化**的证据，答辩时可以直接报数。
// ============================================================================

#include "data/event_log.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

class EventPanel : public QWidget
{
    Q_OBJECT
public:
    explicit EventPanel(data::EventLog *log, QWidget *parent = nullptr);

public slots:
    void onEntryAdded(const data::EventEntry &e);
    void onCleared();

private:
    void appendRow(const data::EventEntry &e);
    void refreshFromLog();
    void updateSummary();
    /// 当前过滤条件下这一条要不要显示
    bool passesFilter(const data::EventEntry &e) const;

    data::EventLog *m_log = nullptr;
    QTableWidget *m_table = nullptr;
    QCheckBox *m_onlySafety = nullptr;
    QCheckBox *m_onlyProblems = nullptr;
    QCheckBox *m_autoScroll = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_clear = nullptr;
};
