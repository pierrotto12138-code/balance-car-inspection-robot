#include "ui/panels/tune_panel.h"

#include "data/event_log.h"
#include "domain/commands.h"
#include "domain/units.h"
#include "io/link_manager.h"
#include "ui/theme/theme.h"
#include "ui/widgets/scope_plot.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

using data::ParamCheck;
using data::PidParamInfo;
using data::ParamStore;

namespace {

constexpr int kColId = 0;
constexpr int kColName = 1;
constexpr int kColVar = 2;
constexpr int kColValue = 3;
constexpr int kColRange = 4;
constexpr int kColDefault = 5;
constexpr int kColSource = 6;
constexpr int kColDesc = 7;

} // namespace

TunePanel::TunePanel(domain::VehicleState *state, io::LinkManager *mgr, data::ParamStore *params,
                     data::EventLog *log, QWidget *parent)
    : QWidget(parent), m_state(state), m_mgr(mgr), m_params(params), m_log(log)
{
    m_clock.start();
    buildUi();
    rebuildTable();

    connect(m_params, &ParamStore::valueUpdated, this, [this](quint8, double, bool) {
        rebuildTable();
    });
    connect(m_params, &ParamStore::mismatch, this, [this](quint8 id, double want, double actual) {
        appendLogRow(QStringLiteral("回读不一致：%1").arg(data::pidParamName(id)),
                     domain::units::number(want, 4), domain::units::number(actual, 4),
                     QStringLiteral("✘ 未生效（固件保持原值：越界？NaN？）"), false);
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &TunePanel::refresh);
    m_timer->start();
    refresh();
}

void TunePanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(8);

    // ---------------- 参数表 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("PID 参数（0x24 在线调参 · 范围依据协议 §3.8）"), this);
        auto *vl = new QVBoxLayout(g);
        vl->setSpacing(6);

        m_table = new QTableWidget(g);
        m_table->setColumnCount(8);
        m_table->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("参数"),
                                            QStringLiteral("固件变量"), QStringLiteral("当前值"),
                                            QStringLiteral("允许范围"), QStringLiteral("默认"),
                                            QStringLiteral("数据来源"), QStringLiteral("说明")});
        m_table->verticalHeader()->setVisible(false);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->horizontalHeader()->setSectionResizeMode(kColDesc, QHeaderView::Stretch);
        m_table->setColumnWidth(kColId, 44);
        m_table->setColumnWidth(kColName, 90);
        m_table->setColumnWidth(kColVar, 100);
        m_table->setColumnWidth(kColValue, 80);
        m_table->setColumnWidth(kColRange, 110);
        m_table->setColumnWidth(kColDefault, 60);
        m_table->setColumnWidth(kColSource, 90);
        m_table->setMinimumHeight(260); // 7 行参数要放得下（190 会把 0x06/0x07 截掉）
        vl->addWidget(m_table);

        auto *edit = new QHBoxLayout;
        edit->setSpacing(6);
        m_selectedName = new QLabel(QStringLiteral("（未选中）"), g);
        m_selectedName->setFont(theme::monoFont(10, true));
        m_valueEdit = new QDoubleSpinBox(g);
        m_valueEdit->setDecimals(4);
        m_valueEdit->setRange(-1e6, 1e6);
        m_valueEdit->setSingleStep(1.0);
        m_valueEdit->setMinimumWidth(120);
        m_btnSend = new QPushButton(QStringLiteral("下发该参数"), g);
        m_btnReadAll = new QPushButton(QStringLiteral("全量回读 (0xFF)"), g);
        m_btnSaveFlash = new QPushButton(QStringLiteral("保存到 Flash (0x00)"), g);
        m_btnLoadFlash = new QPushButton(QStringLiteral("从 Flash 载入 (0xFE)"), g);
        edit->addWidget(m_selectedName);
        edit->addWidget(m_valueEdit);
        edit->addWidget(m_btnSend);
        edit->addStretch(1);
        edit->addWidget(m_btnReadAll);
        edit->addWidget(m_btnSaveFlash);
        edit->addWidget(m_btnLoadFlash);
        vl->addLayout(edit);

        m_rangeHint = new QLabel(g);
        m_rangeHint->setProperty("role", "sub");
        m_rangeHint->setWordWrap(true);
        vl->addWidget(m_rangeHint);

        m_observe = new QCheckBox(
            QStringLiteral("观测模式 (0xFD)：有线链路以 100 Hz 上报 0x10+0x11（PID 整定用；"
                           "**临时状态，整定完请关掉**）"),
            g);
        m_observeHint = new QLabel(g);
        m_observeHint->setProperty("role", "sub");
        m_lastAction = new QLabel(g);
        m_lastAction->setProperty("role", "sub");
        m_lastAction->setWordWrap(true);
        vl->addWidget(m_observe);
        vl->addWidget(m_observeHint);
        vl->addWidget(m_lastAction);
        row1->addWidget(g, 4);
    }

    // ---------------- 下发日志 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("下发日志（下发值 ↔ 回读值配对）"), this);
        auto *vl = new QVBoxLayout(g);
        m_logTable = new QTableWidget(g);
        m_logTable->setColumnCount(5);
        m_logTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("动作"),
                                               QStringLiteral("下发"), QStringLiteral("回读"),
                                               QStringLiteral("结论")});
        m_logTable->verticalHeader()->setVisible(false);
        m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_logTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        m_logTable->setColumnWidth(0, 86);
        m_logTable->setColumnWidth(2, 70);
        m_logTable->setColumnWidth(3, 70);
        vl->addWidget(m_logTable);
        row1->addWidget(g, 3);
    }
    root->addLayout(row1);

    auto *row2 = new QHBoxLayout;
    row2->setSpacing(8);

    // ---------------- 0x23 巡检参数 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("巡检参数下发（0x23，掉电不保存）"), this);
        auto *grid = new QGridLayout(g);
        grid->setSpacing(6);
        m_periodMs = new QSpinBox(g);
        m_periodMs->setRange(100, 60000);
        m_periodMs->setSingleStep(100);
        m_periodMs->setSuffix(QStringLiteral(" ms"));
        m_mq2Limit = new QDoubleSpinBox(g);
        m_mq2Limit->setRange(0, 6000);
        m_mq2Limit->setDecimals(1);
        m_mq2Limit->setSuffix(QStringLiteral(" ppm"));
        m_mq135Limit = new QDoubleSpinBox(g);
        m_mq135Limit->setRange(0, 6000);
        m_mq135Limit->setDecimals(1);
        m_mq135Limit->setSuffix(QStringLiteral(" ppm"));
        m_adaptive = new QCheckBox(QStringLiteral("自适应采样"), g);
        m_compensation = new QCheckBox(QStringLiteral("温湿度补偿"), g);
        m_confirmFrames = new QSpinBox(g);
        m_confirmFrames->setRange(1, 20);
        m_confirmFrames->setSuffix(QStringLiteral(" 帧"));
        m_surveyHint = new QLabel(g);
        m_surveyHint->setProperty("role", "sub");
        m_surveyHint->setWordWrap(true);
        auto *btnSendSurvey = new QPushButton(QStringLiteral("下发巡检参数"), g);

        grid->addWidget(new QLabel(QStringLiteral("采样周期")), 0, 0);
        grid->addWidget(m_periodMs, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("MQ-2 阈值")), 0, 2);
        grid->addWidget(m_mq2Limit, 0, 3);
        grid->addWidget(new QLabel(QStringLiteral("MQ-135 阈值")), 1, 0);
        grid->addWidget(m_mq135Limit, 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("连续确认")), 1, 2);
        grid->addWidget(m_confirmFrames, 1, 3);
        grid->addWidget(m_adaptive, 2, 0);
        grid->addWidget(m_compensation, 2, 1);
        grid->addWidget(btnSendSurvey, 2, 2, 1, 2);
        grid->addWidget(m_surveyHint, 3, 0, 1, 4);
        row2->addWidget(g, 2);

        // ★ 默认值必须来自"账本里的巡检参数"（协议 §4.1 的默认：1000 ms / 30 / 50 / 连续 3 帧），
        //   而不是各 spinbox 的最小值 —— 否则界面一打开就显示 100 ms / 0 ppm / 1 帧，
        //   看着像"当前配置"，其实是控件的下限（这类"默认值即最小值"的展示会误导现场判断）。
        const data::ParamStore::SurveyCfg cfg = m_params->surveyCfg();
        m_periodMs->setValue(cfg.samplePeriodMs);
        m_mq2Limit->setValue(cfg.mq2Ppm);
        m_mq135Limit->setValue(cfg.mq135Ppm);
        m_adaptive->setChecked(cfg.adaptive);
        m_compensation->setChecked(cfg.compensation);
        m_confirmFrames->setValue(cfg.confirmFrames);
        m_surveyHint->setText(QStringLiteral("默认值来自协议 §4.1（1000 ms / 30 / 50 ppm / 连续 3 帧）。"
                                             "0x23 **掉电不保存**，重启回固件里的上电值。"));

        connect(btnSendSurvey, &QPushButton::clicked, this, &TunePanel::sendSurveyCfg);
    }

    // ---------------- 三环实况 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("三环实况（0x16，1 Hz）"), this);
        auto *vl = new QVBoxLayout(g);
        vl->setSpacing(4);
        m_loopPlot = new ScopePlot(QString(), g);
        m_loopPlot->setObjectName(QStringLiteral("loopPlot"));
        m_loopPlot->setUnitLabel(QStringLiteral("PWM 分量"));
        m_loopPlot->addChannel(QStringLiteral("Bal"), QColor("#7FA6C8"));
        m_loopPlot->addChannel(QStringLiteral("Vel"), QColor("#8FB98F"));
        m_loopPlot->addChannel(QStringLiteral("Turn"), QColor("#C8B47F"));
        m_loopPlot->setYRange(-1500, 1500);
        vl->addWidget(m_loopPlot, 1);

        m_execPlot = new ScopePlot(QString(), g);
        m_execPlot->setObjectName(QStringLiteral("execPlot"));
        m_execPlot->setUnitLabel(QStringLiteral("左轮：Moto 与 CCR"));
        m_execPlot->addChannel(QStringLiteral("Moto1"), QColor("#7FA6C8"));
        m_execPlot->addChannel(QStringLiteral("CCR_A"), QColor("#D9534F"));
        m_execPlot->setYRange(0, 2000);
        vl->addWidget(m_execPlot, 1);
        row2->addWidget(g, 3);
    }
    root->addLayout(row2, 1);

    // ---------------- 连线 ----------------
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &TunePanel::updateSelectedRow);
    connect(m_btnSend, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r < 0) {
            m_lastAction->setText(QStringLiteral("先在上面选中一行参数"));
            return;
        }
        const quint8 id = static_cast<quint8>(m_table->item(r, kColId)->text().mid(2).toInt(nullptr, 16));
        sendParam(id, m_valueEdit->value());
    });
    connect(m_btnReadAll, &QPushButton::clicked, this, &TunePanel::sendFullReadback);
    connect(m_btnSaveFlash, &QPushButton::clicked, this,
            [this]() { sendFlash(0x00, QStringLiteral("保存到 Flash")); });
    connect(m_btnLoadFlash, &QPushButton::clicked, this,
            [this]() { sendFlash(0xFE, QStringLiteral("从 Flash 载入")); });
    connect(m_observe, &QCheckBox::toggled, this, [this](bool on) {
        QByteArray payload;
        if (!domain::cmd::pidTunePayload(0xFD, on ? 1.0f : 0.0f, &payload)) {
            return;
        }
        const qint64 n = m_mgr->send(proto::down::kPidTune, payload);
        const bool ok = n > 0;
        m_params->noteRequest(0xFD, on ? 1.0 : 0.0);
        appendLogRow(QStringLiteral("观测模式 %1").arg(on ? QStringLiteral("开") : QStringLiteral("关")),
                     on ? QStringLiteral("1") : QStringLiteral("0"), QStringLiteral("待回执"),
                     ok ? QStringLiteral("已下发，等回执") : QStringLiteral("✘ 无可用链路"), ok);
        if (m_log != nullptr) {
            m_log->onOperatorAction(proto::down::kPidTune,
                                    QStringLiteral("观测模式(0xFD)=%1").arg(on ? 1 : 0), ok);
        }
        m_observeHint->setText(on ? QStringLiteral("⚠ 观测模式已请求开启：固件会以 100 Hz 上报，"
                                                   "整定完务必关掉并「保存到 Flash」")
                                  : QStringLiteral("观测模式已请求关闭"));
    });
}

void TunePanel::rebuildTable()
{
    const auto &list = data::pidParams();
    if (m_table->rowCount() != list.size()) {
        m_table->setRowCount(list.size());
    }
    for (int r = 0; r < list.size(); ++r) {
        const PidParamInfo &p = list.at(r);
        const bool have = m_params->readbackReceived(p.id);
        const double v = m_params->current(p.id);
        auto set = [this, r](int col, const QString &text, const QColor &color, bool mono) {
            auto *item = m_table->item(r, col);
            if (item == nullptr) {
                item = new QTableWidgetItem;
                item->setForeground(color);
                if (mono) {
                    item->setFont(theme::monoFont(9));
                }
                m_table->setItem(r, col, item);
            }
            item->setText(text);
            item->setForeground(color);
        };
        set(kColId, domain::hexByte(p.id), QColor(theme::kTextDim), true);
        set(kColName, QString::fromUtf8(p.name), QColor(theme::kText), false);
        set(kColVar, QString::fromUtf8(p.baseVar), QColor(theme::kTextDim), true);
        // 未回读时显示默认值但颜色变灰 —— "默认值"与"板子上的真值"必须能区分
        set(kColValue, domain::units::number(v, 4),
            have ? QColor(theme::kText) : QColor(theme::kOffline), true);
        set(kColRange, QStringLiteral("%1 ~ %2").arg(p.lo).arg(p.hi), QColor(theme::kTextDim), true);
        set(kColDefault, domain::units::number(p.defaultValue, 4), QColor(theme::kTextDim), true);
        const bool confirmed = m_params->lastRequestConfirmed(p.id);
        set(kColSource, have ? (confirmed ? QStringLiteral("已回读✔") : QStringLiteral("待回读"))
                             : QStringLiteral("默认值"),
            have && confirmed ? QColor(theme::kOk) : QColor(theme::kWarn), false);
        set(kColDesc, QString::fromUtf8(p.desc), QColor(theme::kTextDim), false);
        // 说明列会被列宽截断 —— 挂完整文本到 tooltip（截断的说明等于没有说明）
        if (auto *desc = m_table->item(r, kColDesc)) {
            desc->setToolTip(QString::fromUtf8(p.desc));
        }
        if (auto *var = m_table->item(r, kColVar)) {
            var->setToolTip(QStringLiteral("固件里的变量名：%1").arg(QString::fromUtf8(p.baseVar)));
        }
    }
    updateSelectedRow();
}

void TunePanel::updateSelectedRow()
{
    const int r = m_table->currentRow();
    if (r < 0) {
        m_selectedName->setText(QStringLiteral("（未选中）"));
        m_rangeHint->setText(QStringLiteral("选中一行参数后，在上面的输入框里改值再点「下发该参数」。"));
        m_btnSend->setEnabled(false);
        return;
    }
    const quint8 id = static_cast<quint8>(m_table->item(r, kColId)->text().mid(2).toInt(nullptr, 16));
    const PidParamInfo *p = data::pidParamInfo(id);
    if (p == nullptr) {
        return;
    }
    m_selectedName->setText(QStringLiteral("%1（%2）")
                                .arg(QString::fromUtf8(p->name), domain::hexByte(id)));
    m_valueEdit->setRange(p->lo, p->hi); // 输入框直接按协议范围限制，从源头堵住越界
    m_valueEdit->setValue(m_params->current(id));
    // Zhongzhi 是"平衡目标角（重心补偿）"，单位 °、步进 0.1（B 侧接口文档 §3.2）
    m_valueEdit->setSingleStep(id == 0x05 ? 0.1 : std::max(0.01, (p->hi - p->lo) / 100.0));
    m_btnSend->setEnabled(true);
    m_rangeHint->setText(QStringLiteral("允许范围 %1 ~ %2（输入框已按此限制；协议 §3.8 规定越界固件会"
                                        "拒绝并保持原值）· %3")
                             .arg(p->lo)
                             .arg(p->hi)
                             .arg(QString::fromUtf8(p->desc)));
}

bool TunePanel::sendParam(quint8 id, double value)
{
    QString why;
    const ParamCheck chk = data::checkPidValue(id, value, &why);
    if (chk != ParamCheck::Ok) {
        m_lastAction->setText(QStringLiteral("✘ 未下发：%1").arg(why));
        appendLogRow(QStringLiteral("下发 %1").arg(data::pidParamName(id)),
                     domain::units::number(value, 4), QString(), QStringLiteral("✘ 未通过校验"), false);
        return false;
    }
    QByteArray payload;
    if (!domain::cmd::pidTunePayload(id, static_cast<float>(value), &payload)) {
        m_lastAction->setText(QStringLiteral("✘ 值无法编码（NaN/Inf）"));
        return false;
    }
    const qint64 n = m_mgr->send(proto::down::kPidTune, payload);
    const bool ok = n > 0;
    m_params->noteRequest(id, value); // 先记账，再看回读（这样"回读不一致"有据可查）
    m_timeoutReported.remove(id);     // 新的一次下发 ⇒ 重新开始等回读超时
    appendLogRow(QStringLiteral("下发 %1").arg(data::pidParamName(id)),
                 domain::units::number(value, 4), QStringLiteral("待回读"),
                 ok ? QStringLiteral("已下发 %1 B").arg(n) : QStringLiteral("✘ 无可用链路"), ok);
    if (m_log != nullptr) {
        m_log->onOperatorAction(proto::down::kPidTune,
                                QStringLiteral("%1 = %2").arg(data::pidParamName(id),
                                                              domain::units::number(value, 4)),
                                ok);
    }
    m_lastAction->setText(ok ? QStringLiteral("已下发 %1 = %2，等待回读确认（回读值出现在日志里）")
                                   .arg(data::pidParamName(id), domain::units::number(value, 4))
                             : QStringLiteral("✘ 下发失败：没有可用链路（保活/连接是否正常？）"));
    return ok;
}

void TunePanel::sendFlash(quint8 extId, const QString &what)
{
    QByteArray payload;
    if (!domain::cmd::pidTunePayload(extId, 0.0f, &payload)) {
        return;
    }
    const qint64 n = m_mgr->send(proto::down::kPidTune, payload);
    const bool ok = n > 0;
    appendLogRow(what, QStringLiteral("—"), QStringLiteral("待回执"),
                 ok ? QStringLiteral("已下发（0.0 = 成功）") : QStringLiteral("✘ 无可用链路"), ok);
    if (m_log != nullptr) {
        m_log->onOperatorAction(proto::down::kPidTune, what, ok);
    }
    m_lastAction->setText(ok ? QStringLiteral("%1 已下发").arg(what)
                             : QStringLiteral("✘ %1 下发失败：没有可用链路").arg(what));
}

void TunePanel::sendFullReadback()
{
    QByteArray payload;
    if (!domain::cmd::pidTunePayload(0xFF, 0.0f, &payload)) {
        return;
    }
    const qint64 n = m_mgr->send(proto::down::kPidTune, payload);
    const bool ok = n > 0;
    appendLogRow(QStringLiteral("全量回读 (0xFF)"), QStringLiteral("—"), QStringLiteral("7 条回读"),
                 ok ? QStringLiteral("已下发") : QStringLiteral("✘ 无可用链路"), ok);
    if (m_log != nullptr) {
        m_log->onOperatorAction(proto::down::kPidTune, QStringLiteral("全量回读(0xFF)"), ok);
    }
    m_lastAction->setText(ok ? QStringLiteral("已请求全量回读：机器人会按 0x01~0x07 顺序回 7 条 0x24")
                             : QStringLiteral("✘ 全量回读下发失败：没有可用链路"));
}

void TunePanel::sendSurveyCfg()
{
    const QByteArray payload = domain::cmd::surveyCfgPayload(
        static_cast<quint16>(m_periodMs->value()), m_mq2Limit->value(), m_mq135Limit->value(),
        m_adaptive->isChecked(), m_compensation->isChecked(),
        static_cast<quint8>(m_confirmFrames->value()));
    const qint64 n = m_mgr->send(proto::down::kSurveyCfg, payload);
    const bool ok = n > 0;

    ParamStore::SurveyCfg cfg;
    cfg.samplePeriodMs = static_cast<quint16>(m_periodMs->value());
    cfg.mq2Ppm = m_mq2Limit->value();
    cfg.mq135Ppm = m_mq135Limit->value();
    cfg.adaptive = m_adaptive->isChecked();
    cfg.compensation = m_compensation->isChecked();
    cfg.confirmFrames = static_cast<quint8>(m_confirmFrames->value());
    m_params->setSurveyCfg(cfg);

    appendLogRow(QStringLiteral("巡检参数 (0x23)"),
                 QStringLiteral("周期 %1ms / %2 / %3 ppm")
                     .arg(cfg.samplePeriodMs)
                     .arg(cfg.mq2Ppm)
                     .arg(cfg.mq135Ppm),
                 QStringLiteral("回 0x12 确认"),
                 ok ? QStringLiteral("已下发 %1 B").arg(n) : QStringLiteral("✘ 无可用链路"), ok);
    if (m_log != nullptr) {
        m_log->onOperatorAction(proto::down::kSurveyCfg,
                                QStringLiteral("采样 %1 ms · MQ-2 %2 ppm · MQ-135 %3 ppm · "
                                               "连续确认 %4 帧 · 自适应 %5 · 补偿 %6")
                                    .arg(cfg.samplePeriodMs)
                                    .arg(cfg.mq2Ppm)
                                    .arg(cfg.mq135Ppm)
                                    .arg(cfg.confirmFrames)
                                    .arg(cfg.adaptive ? 1 : 0)
                                    .arg(cfg.compensation ? 1 : 0),
                                ok);
    }
    m_surveyHint->setText(QStringLiteral("已下发并记账（0x23 掉电不保存，重启回上电值；"
                                         "要长期生效请在固件默认值里改）。"));
}

void TunePanel::appendLogRow(const QString &action, const QString &sent, const QString &readback,
                             const QString &verdict, bool ok)
{
    const int row = m_logTable->rowCount();
    m_logTable->insertRow(row);
    auto set = [this, row](int col, const QString &text, const QColor &color, bool mono) {
        auto *item = new QTableWidgetItem(text);
        item->setForeground(color);
        if (mono) {
            item->setFont(theme::monoFont(9));
        }
        m_logTable->setItem(row, col, item);
    };
    set(0, QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
        QColor(theme::kTextDim), true);
    set(1, action, QColor(theme::kText), false);
    set(2, sent, QColor(theme::kTextDim), true);
    set(3, readback, QColor(theme::kTextDim), true);
    set(4, verdict, ok ? QColor(theme::kOk) : QColor(theme::kAlarm), false);
    m_logTable->scrollToBottom();
}

void TunePanel::onDataUpdated(quint8 cmd)
{
    if (cmd == proto::down::kPidTune) {
        // 0x24 的回显：真实增益回读 or 扩展 ID 回执
        const domain::PidTuneData &e = m_state->pidEcho();
        if (e.isAck()) {
            m_params->noteAck(e.paramId, e.value);
            QString what = data::pidParamName(e.paramId);
            if (e.paramId == 0xFD) {
                what += (e.value >= 0.5) ? QStringLiteral("：当前为开") : QStringLiteral("：当前为关");
            } else {
                what += (e.value == 0.0f) ? QStringLiteral("：成功") : QStringLiteral("：失败？");
            }
            appendLogRow(QStringLiteral("回执"), QStringLiteral("—"),
                         domain::units::number(e.value, 1), what, e.value == 0.0f || e.paramId == 0xFD);
        } else {
            m_params->noteReadback(e.paramId, double(e.value));
            appendLogRow(QStringLiteral("回读 %1").arg(data::pidParamName(e.paramId)),
                         QStringLiteral("—"), domain::units::number(e.value, 4),
                         m_params->lastRequestConfirmed(e.paramId)
                             ? QStringLiteral("✔ 与下发一致")
                             : QStringLiteral("✘ 与下发不一致（保持原值？）"),
                         m_params->lastRequestConfirmed(e.paramId));
        }
        rebuildTable();
        return;
    }
    if (cmd == proto::up::kLoop && m_state->received(proto::up::kLoop)) {
        // 横轴用面板自己的经过时间（0x16 是 1 Hz；曲线只关心相对形状与间隔）
        const double sec = m_clock.elapsed() / 1000.0;
        const domain::LoopData &l = m_state->loop();
        m_loopPlot->addSample(0, sec, l.balancePwm);
        m_loopPlot->addSample(1, sec, l.velocityPwm);
        m_loopPlot->addSample(2, sec, l.turnPwm);
        m_execPlot->addSample(0, sec, std::abs(l.moto1));
        m_execPlot->addSample(1, sec, l.ccrA);
    }
}

void TunePanel::refresh()
{
    if (!isVisible()) {
        return;
    }
    m_loopPlot->replot();
    m_execPlot->replot();
    // 表格里"待回读/已回读"会随回读更新，10 Hz 刷一次足够（不是每帧重绘）
    rebuildTable();

    // ★ 无回读超时：真机代码里**越界/NaN/未知 ID 时固件整帧不回复**（不是回旧值）——
    //   界面若只显示"待回读"，用户会一直等；正确结论是"这次下发被拒/该 ID 未实现"。
    for (const data::PidParamInfo &p : data::pidParams()) {
        if (m_params->requestTimedOut(p.id, m_clock.elapsed()) && !m_timeoutReported.contains(p.id)) {
            m_timeoutReported.insert(p.id);
            appendLogRow(QStringLiteral("回读超时：%1").arg(QString::fromUtf8(p.name)),
                         domain::units::number(m_params->lastRequestValue(p.id), 4),
                         QStringLiteral("无回读"),
                         QStringLiteral("✘ 固件整帧未回复（越界/NaN/未知 ID 都是这个表现）"), false);
        }
    }

    // 0x16 的三个"一眼定位故障"组合（B 侧接口文档 §2.1）—— 调参页把它当"判读结论"常显
    if (m_state->received(proto::up::kLoop)) {
        const domain::LoopData &l = m_state->loop();
        QStringList parts;
        parts << (l.differentialWithoutTurn()
                      ? QStringLiteral("⚠ Turn=0 却两轮命令不同（先核对固件极性开关再判故障）")
                      : QStringLiteral("✔ Turn=0 时两轮命令一致"));
        // "算了没执行"分级：未使能/锁存时 CCR=0 是**保护生效**（现场"没使能电机"就长这样）
        const domain::LoopData::Execution exec = l.executionState();
        parts << (exec == domain::LoopData::Execution::Mismatch
                      ? QStringLiteral("⚠ 算了没执行（未停车却 CCR=0）")
                  : exec == domain::LoopData::Execution::Protected
                      ? QStringLiteral("保护中，未驱动（正常）")
                      : QStringLiteral("✔ CCR 与 Moto 一致"));
        parts << (l.setPwmCalled() ? QStringLiteral("✔ 本拍调用了 Set_pwm")
                                   : QStringLiteral("⚠ 本拍未调用 Set_pwm（保护中？）"));
        parts << (l.flagStop() ? QStringLiteral("Flag_Stop=1（已停车）") : QStringLiteral("Flag_Stop=0"));
        parts << (l.motorDisabled() ? QStringLiteral("电机被禁（锁存）") : QStringLiteral("电机使能中"));
        m_lastAction->setText(QStringLiteral("固件 %1/%2 · %3")
                                  .arg(domain::hexByte(l.fwVersion),
                                       domain::firmwareVersionText(l.fwVersion),
                                       parts.join(QStringLiteral(" ｜ "))));
    }
}
