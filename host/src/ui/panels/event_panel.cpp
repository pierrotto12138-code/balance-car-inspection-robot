#include "ui/panels/event_panel.h"

#include "domain/frames.h"
#include "io/link.h"
#include "ui/theme/theme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

using data::EventEntry;

EventPanel::EventPanel(data::EventLog *log, QWidget *parent) : QWidget(parent), m_log(log)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    // ---------------- 工具条 ----------------
    auto *bar = new QHBoxLayout;
    bar->setSpacing(10);
    m_onlySafety = new QCheckBox(QStringLiteral("只看安全事件"), this);
    m_onlyProblems = new QCheckBox(QStringLiteral("只看告警/故障"), this);
    m_autoScroll = new QCheckBox(QStringLiteral("自动滚到最新"), this);
    m_autoScroll->setChecked(true);
    m_clear = new QPushButton(QStringLiteral("清空"), this);
    bar->addWidget(m_onlySafety);
    bar->addWidget(m_onlyProblems);
    bar->addWidget(m_autoScroll);
    bar->addStretch(1);
    bar->addWidget(m_clear);
    root->addLayout(bar);

    // ---------------- 表格 ----------------
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("会话内"),
                                        QStringLiteral("来源"), QStringLiteral("事件码"),
                                        QStringLiteral("内容"), QStringLiteral("备注")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(false);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 90);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 60);
    m_table->setColumnWidth(3, 70);
    root->addWidget(m_table, 1);

    // ---------------- 底部统计（可量化的"安全"证据） ----------------
    m_summary = new QLabel(this);
    m_summary->setProperty("role", "sub");
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_summary);

    connect(m_onlySafety, &QCheckBox::toggled, this, &EventPanel::refreshFromLog);
    connect(m_onlyProblems, &QCheckBox::toggled, this, &EventPanel::refreshFromLog);
    connect(m_clear, &QPushButton::clicked, this, [this]() { m_log->clear(); });
    connect(m_log, &data::EventLog::entryAdded, this, &EventPanel::onEntryAdded);
    connect(m_log, &data::EventLog::cleared, this, &EventPanel::onCleared);

    refreshFromLog();
}

bool EventPanel::passesFilter(const EventEntry &e) const
{
    if (m_onlySafety != nullptr && m_onlySafety->isChecked() && !e.safety) {
        return false;
    }
    if (m_onlyProblems != nullptr && m_onlyProblems->isChecked() && !(e.local || e.safety) &&
        e.code != static_cast<quint8>(proto::EventCode::GasOverLimit) &&
        e.code != static_cast<quint8>(proto::EventCode::CrashOrLowBat) &&
        e.code != static_cast<quint8>(proto::EventCode::TaskFail)) {
        return false;
    }
    return true;
}

void EventPanel::appendRow(const EventEntry &e)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto setCell = [this, row](int col, const QString &text, const QColor &color, bool mono) {
        auto *item = new QTableWidgetItem(text);
        item->setForeground(color);
        if (mono) {
            item->setFont(theme::monoFont(9));
        }
        m_table->setItem(row, col, item);
    };

    // 颜色语义：安全/告警=红；解除=绿；其余（信息类）=中性
    QColor color(theme::kText);
    if (e.local) {
        color = e.raised ? QColor(theme::kAlarm) : QColor(theme::kOk);
    } else if (e.safety) {
        color = (e.code == static_cast<quint8>(proto::EventCode::SafetyReleased))
                    ? QColor(theme::kOk)
                    : QColor(theme::kAlarm);
    } else if (e.code == static_cast<quint8>(proto::EventCode::GasOverLimit) ||
               e.code == static_cast<quint8>(proto::EventCode::CrashOrLowBat)) {
        color = QColor(theme::kWarn);
    } else {
        color = QColor(theme::kTextDim);
    }

    setCell(0, e.wall.toString(QStringLiteral("HH:mm:ss.zzz")), QColor(theme::kTextDim), true);
    setCell(1, QStringLiteral("T+%1 s").arg(e.localMs / 1000.0, 0, 'f', 1),
            QColor(theme::kTextDim), true);
    setCell(2, e.local ? QStringLiteral("本机")
                       : QString::fromUtf8(io::linkName(e.kind)),
            QColor(theme::kTextDim), false);
    setCell(3, e.local ? QStringLiteral("—") : domain::hexByte(e.code),
            QColor(theme::kTextDim), true);
    setCell(4, e.title, color, false);
    setCell(5, e.detail, QColor(theme::kTextDim), false);

    if (m_autoScroll->isChecked()) {
        m_table->scrollToBottom();
    }
}

void EventPanel::onEntryAdded(const EventEntry &e)
{
    if (passesFilter(e)) {
        appendRow(e);
    }
    updateSummary();
}

void EventPanel::onCleared()
{
    m_table->setRowCount(0);
    updateSummary();
}

void EventPanel::refreshFromLog()
{
    m_table->setRowCount(0);
    for (int i = 0; i < m_log->size(); ++i) {
        const EventEntry &e = m_log->at(i);
        if (passesFilter(e)) {
            appendRow(e);
        }
    }
    updateSummary();
}

void EventPanel::updateSummary()
{
    int safety = 0;
    int alarms = 0;
    for (int i = 0; i < m_log->size(); ++i) {
        const EventEntry &e = m_log->at(i);
        if (e.safety) {
            ++safety;
        }
        if (e.local && e.raised) {
            ++alarms;
        }
    }
    const qint64 shortest = m_log->shortestSafetyStopMs();
    QString s = QStringLiteral("事件 %1 条 · 安全事件 %2 条 · 告警 %3 次")
                    .arg(m_log->size())
                    .arg(safety)
                    .arg(alarms);
    s += QStringLiteral(" · 最短安全停车时长 %1")
             .arg(shortest < 0 ? QStringLiteral("（尚未出现\"停车→解除\"配对）")
                               : QStringLiteral("%1 s").arg(shortest / 1000.0, 0, 'f', 1));
    m_summary->setText(s);
}
