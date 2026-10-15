#include "ui/panels/curve_panel.h"

#include "domain/units.h"
#include "io/link_manager.h"
#include "ui/theme/theme.h"
#include "ui/widgets/scope_plot.h"
#include "ui/widgets/value_card.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using domain::Severity;

namespace {

/// 曲线颜色：与主题一致 —— **只有异常才用彩色**，正常通道用中性灰蓝系
const char *kGas2Color = "#7FA6C8";
const char *kGas135Color = "#8FB98F";
const char *kVoltColor = "#C8B47F";
const char *kTempColor = "#C88F7F";
const char *kHumColor = "#7FC8C0";

} // namespace

CurvePanel::CurvePanel(domain::VehicleState *state, io::LinkManager *mgr, domain::AlarmEngine *alarm,
                       QWidget *parent)
    : QWidget(parent), m_state(state), m_mgr(mgr), m_alarm(alarm)
{
    m_clock.start();
    buildUi();

    m_timer = new QTimer(this);
    m_timer->setInterval(100); // 10 Hz：重绘节奏（与采样节奏分离）
    connect(m_timer, &QTimer::timeout, this, &CurvePanel::replotAll);
    m_timer->start();

    applyThresholdsToUi();
    replotAll();
}

void CurvePanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    // ---------------- 顶部工具条 ----------------
    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);
    bar->addWidget(new QLabel(QStringLiteral("MQ-2 阈值")));
    m_mq2Limit = new QDoubleSpinBox(this);
    m_mq2Limit->setRange(1.0, 6000.0);
    m_mq2Limit->setDecimals(1);
    m_mq2Limit->setSuffix(QStringLiteral(" ppm"));
    m_mq2Limit->setToolTip(QStringLiteral("与固件 0x23 巡检参数的默认值一致（30 ppm）。\n"
                                          "改这里只改上位机的判定，**下发到板子要等 P4 的 0x23 面板**。"));
    bar->addWidget(m_mq2Limit);
    bar->addWidget(new QLabel(QStringLiteral("MQ-135 阈值")));
    m_mq135Limit = new QDoubleSpinBox(this);
    m_mq135Limit->setRange(1.0, 6000.0);
    m_mq135Limit->setDecimals(1);
    m_mq135Limit->setSuffix(QStringLiteral(" ppm"));
    bar->addWidget(m_mq135Limit);
    connect(m_mq2Limit, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &CurvePanel::onThresholdEdited);
    connect(m_mq135Limit, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &CurvePanel::onThresholdEdited);

    bar->addSpacing(12);
    bar->addWidget(new QLabel(QStringLiteral("时间窗")));
    m_windowBox = new QComboBox(this);
    m_windowBox->addItem(QStringLiteral("30 s"), 30.0);
    m_windowBox->addItem(QStringLiteral("2 min"), 120.0);
    m_windowBox->addItem(QStringLiteral("10 min"), 600.0);
    m_windowBox->setCurrentIndex(1);
    connect(m_windowBox, &QComboBox::currentIndexChanged, this, &CurvePanel::onWindowChanged);
    bar->addWidget(m_windowBox);

    m_autoYBox = new QCheckBox(QStringLiteral("Y 自动量程"), this);
    m_autoYBox->setToolTip(QStringLiteral("ISA-101 建议固定量程（靠位置读值）；\n"
                                          "只有看趋势时才打开自动量程。"));
    bar->addWidget(m_autoYBox);
    m_endValueBox = new QCheckBox(QStringLiteral("曲线末端标数值"), this);
    m_endValueBox->setChecked(true);
    bar->addWidget(m_endValueBox);

    m_resetView = new QPushButton(QStringLiteral("撤销缩放"), this);
    m_resetView->setToolTip(QStringLiteral("回到默认视图（也可以直接双击图内任意位置）"));
    bar->addWidget(m_resetView);
    m_pauseBtn = new QPushButton(QStringLiteral("暂停更新"), this);
    m_pauseBtn->setCheckable(true);
    m_pauseBtn->setToolTip(QStringLiteral("冻结画面读波形（暂停期间不再采样，恢复后继续）"));
    bar->addWidget(m_pauseBtn);
    bar->addStretch(1);
    root->addLayout(bar);

    // ---------------- 四张图（通道独立） ----------------
    auto *grid = new QGridLayout;
    grid->setSpacing(8);

    auto makeCell = [this](ScopePlot *&plot, ValueCard *&card, const QString &title,
                           const QString &unit, const char *color, int decimals) {
        auto *box = new QGroupBox(title, this);
        auto *vl = new QVBoxLayout(box);
        vl->setContentsMargins(6, 6, 6, 6);
        vl->setSpacing(4);
        card = new ValueCard(title, unit, decimals, box);
        card->setValuePointSize(15);
        vl->addWidget(card);
        plot = new ScopePlot(QString(), box);
        plot->setUnitLabel(unit);
        plot->addChannel(title, QColor(color));
        vl->addWidget(plot, 1);
        return box;
    };

    grid->addWidget(makeCell(m_mq2Plot, m_mq2Card, QStringLiteral("MQ-2 浓度"),
                             QStringLiteral("ppm"), kGas2Color, 1),
                    0, 0);
    grid->addWidget(makeCell(m_mq135Plot, m_mq135Card, QStringLiteral("MQ-135 浓度"),
                             QStringLiteral("ppm"), kGas135Color, 1),
                    0, 1);
    grid->addWidget(makeCell(m_voltPlot, m_voltCard, QStringLiteral("电池电压"),
                             QStringLiteral("V"), kVoltColor, 2),
                    1, 0);

    // 温湿度：一张图两条通道（这两个量本来就该一起看）
    {
        auto *box = new QGroupBox(QStringLiteral("温度 / 湿度"), this);
        auto *vl = new QVBoxLayout(box);
        vl->setContentsMargins(6, 6, 6, 6);
        vl->setSpacing(4);
        auto *cards = new QHBoxLayout;
        cards->setSpacing(6);
        m_thCard = new ValueCard(QStringLiteral("温度"), QStringLiteral("℃"), 1, box);
        auto *humCard = new ValueCard(QStringLiteral("湿度"), QStringLiteral("%RH"), 1, box);
        m_thCard->setValuePointSize(15);
        humCard->setValuePointSize(15);
        humCard->setObjectName(QStringLiteral("humCard"));
        cards->addWidget(m_thCard);
        cards->addWidget(humCard);
        vl->addLayout(cards);
        m_thPlot = new ScopePlot(QString(), box);
        m_thPlot->setUnitLabel(QStringLiteral("℃ / %RH"));
        m_thPlot->addChannel(QStringLiteral("温度"), QColor(kTempColor));
        m_thPlot->addChannel(QStringLiteral("湿度"), QColor(kHumColor));
        m_thPlot->setYRange(0.0, 100.0);
        vl->addWidget(m_thPlot, 1);
        grid->addWidget(box, 1, 1);
    }
    root->addLayout(grid, 1);

    // 电压图固定量程 9~13 V（低压线附近要能看清）
    m_voltPlot->setYRange(9.0, 13.0);
    m_mq2Plot->setYRange(0.0, 100.0);
    m_mq135Plot->setYRange(0.0, 100.0);

    // ---------------- 底部：十字定标读数 + 提示 ----------------
    auto *foot = new QHBoxLayout;
    m_cursor = new QLabel(this);
    m_cursor->setFont(theme::monoFont(9));
    m_cursor->setProperty("role", "sub");
    m_hint = new QLabel(this);
    m_hint->setProperty("role", "sub");
    foot->addWidget(m_cursor);
    foot->addStretch(1);
    foot->addWidget(m_hint);
    root->addLayout(foot);

    for (ScopePlot *p : {m_mq2Plot, m_mq135Plot, m_voltPlot, m_thPlot}) {
        connect(p, &ScopePlot::cursorMoved, this, [this](const QString &text) {
            if (!text.isEmpty()) {
                m_cursor->setText(text);
            }
        });
        connect(p, &ScopePlot::viewModified, this, [this](bool modified) {
            m_resetView->setEnabled(modified);
            m_hint->setText(modified
                                ? QStringLiteral("已缩放/平移 —— 点“撤销缩放”或双击图内返回")
                                : QStringLiteral("滚轮缩放 · 拖动平移 · 双击复位 · 鼠标移动即十字定标"));
        });
    }
    m_resetView->setEnabled(false);
    connect(m_resetView, &QPushButton::clicked, this, [this]() {
        for (ScopePlot *p : {m_mq2Plot, m_mq135Plot, m_voltPlot, m_thPlot}) {
            p->resetView();
        }
    });
    connect(m_pauseBtn, &QPushButton::toggled, this, &CurvePanel::setPaused);
}

void CurvePanel::applyThresholdsToUi()
{
    const domain::AlarmThresholds th = m_alarm->thresholds();
    if (m_mq2Limit != nullptr) {
        QSignalBlocker b1(m_mq2Limit);
        QSignalBlocker b2(m_mq135Limit);
        m_mq2Limit->setValue(th.mq2Ppm);
        m_mq135Limit->setValue(th.mq135Ppm);
    }
    m_mq2Plot->setThresholdLine(th.mq2Ppm,
                                QStringLiteral("阈值 %1 ppm").arg(domain::units::number(th.mq2Ppm, 1)),
                                QColor(theme::kWarn));
    m_mq135Plot->setThresholdLine(
        th.mq135Ppm, QStringLiteral("阈值 %1 ppm").arg(domain::units::number(th.mq135Ppm, 1)),
        QColor(theme::kWarn));
    m_voltPlot->setThresholdLine(
        th.lowBatteryV, QStringLiteral("低压线 %1 V").arg(domain::units::number(th.lowBatteryV, 2)),
        QColor(theme::kWarn));

    // 阈值线要落在可视量程内，否则"参考线"等于没有
    m_mq2Plot->setYRange(0.0, std::max(100.0, th.mq2Ppm * 2.0));
    m_mq135Plot->setYRange(0.0, std::max(100.0, th.mq135Ppm * 2.0));
}

void CurvePanel::onThresholdEdited()
{
    domain::AlarmThresholds th = m_alarm->thresholds();
    th.mq2Ppm = m_mq2Limit->value();
    th.mq135Ppm = m_mq135Limit->value();
    m_alarm->setThresholds(th);
    applyThresholdsToUi();
}

void CurvePanel::onWindowChanged()
{
    const double sec = m_windowBox->currentData().toDouble();
    for (ScopePlot *p : {m_mq2Plot, m_mq135Plot, m_voltPlot, m_thPlot}) {
        p->setWindowSeconds(sec);
        p->resetView();
    }
}

void CurvePanel::setPaused(bool on)
{
    m_paused = on;
    if (m_pauseBtn->isChecked() != on) {
        QSignalBlocker b(m_pauseBtn);
        m_pauseBtn->setChecked(on);
    }
    m_pauseBtn->setText(on ? QStringLiteral("继续更新") : QStringLiteral("暂停更新"));
    if (m_windowBox != nullptr) {
        m_windowBox->setEnabled(!on);
    }
    if (!on) {
        // 恢复时**明确标出暂停区间**：暂停期间没有采样，若不在图上留痕，
        // 恢复后那条"从暂停前直连到恢复后"的线段会被误读成"中间一直在测"。
        const double t = nowSec();
        for (ScopePlot *p : {m_mq2Plot, m_mq135Plot, m_voltPlot, m_thPlot}) {
            p->addMarker(t, QStringLiteral("暂停结束"), QColor(theme::kTextDim));
        }
        replotAll();
    }
}

void CurvePanel::onDataUpdated(quint8 cmd)
{
    if (m_paused) {
        return; // 暂停期间不采样：恢复后曲线从当前时刻继续（不补暂停期的空洞）
    }
    const double t = nowSec();
    switch (cmd) {
    case proto::up::kEnvGas: {
        const domain::EnvData &e = m_state->env();
        m_mq2Plot->addSample(0, t, e.mq2Ppm);
        m_mq135Plot->addSample(0, t, e.mq135Ppm);
        m_thPlot->addSample(0, t, e.tempC);
        m_thPlot->addSample(1, t, e.humidityPct);
        break;
    }
    case proto::up::kDiag: {
        if (m_state->batteryValid()) {
            m_voltPlot->addSample(0, t, m_state->diag().batteryV);
        }
        break;
    }
    default:
        break;
    }
}

void CurvePanel::onAlarmRaised(const domain::AlarmRecord &r)
{
    const double t = nowSec();
    const QColor color(r.severity == Severity::Alarm ? theme::kAlarm : theme::kWarn);
    const QString text = r.title;
    if (r.key == domain::AlarmEngine::kGasMq2) {
        m_mq2Plot->addMarker(t, text, color);
    } else if (r.key == domain::AlarmEngine::kGasMq135) {
        m_mq135Plot->addMarker(t, text, color);
    } else if (r.key == domain::AlarmEngine::kBatteryLow) {
        m_voltPlot->addMarker(t, text, color);
    }
}

void CurvePanel::onAlarmCleared(const domain::AlarmRecord &r)
{
    const double t = nowSec();
    const QColor color(theme::kOk);
    if (r.key == domain::AlarmEngine::kGasMq2) {
        m_mq2Plot->addMarker(t, QStringLiteral("解除"), color);
    } else if (r.key == domain::AlarmEngine::kGasMq135) {
        m_mq135Plot->addMarker(t, QStringLiteral("解除"), color);
    } else if (r.key == domain::AlarmEngine::kBatteryLow) {
        m_voltPlot->addMarker(t, QStringLiteral("解除"), color);
    }
}

void CurvePanel::updateCards()
{
    const auto &st = *m_state;

    auto setCard = [this, &st](ValueCard *card, double value, quint8 cmd, bool valid, Severity sev) {
        if (!valid) {
            card->setInvalid(st.received(cmd) ? QStringLiteral("已过期") : QStringLiteral("无数据"));
            return;
        }
        card->setValue(value);
        card->setSeverity(sev);
        const qint64 age = st.ageMs(cmd);
        card->setSub(age < 0 ? QStringLiteral("--") : QStringLiteral("%1 ms 前").arg(age));
    };

    const bool envOk = st.received(proto::up::kEnvGas) && st.ageMs(proto::up::kEnvGas) <= 3000;
    const auto &e = st.env();
    setCard(m_mq2Card, e.mq2Ppm, proto::up::kEnvGas, envOk && e.mq2Ready(),
            m_alarm->record(domain::AlarmEngine::kGasMq2).active ? Severity::Alarm : Severity::Ok);
    setCard(m_mq135Card, e.mq135Ppm, proto::up::kEnvGas, envOk && e.mq135Ready(),
            m_alarm->record(domain::AlarmEngine::kGasMq135).active ? Severity::Alarm : Severity::Ok);
    if (envOk && e.dht11Valid()) {
        m_thCard->setValue(e.tempC);
        m_thCard->setSub(QStringLiteral("湿度 %1 %RH")
                             .arg(domain::units::number(e.humidityPct, 1)));
    } else {
        m_thCard->setInvalid(envOk ? QStringLiteral("DHT11 无效") : QStringLiteral("无数据"));
    }
    // 温度/湿度图的第二张卡片（按 objectName 找，避免再加一个成员）
    if (auto *hum = m_thCard->parentWidget()->findChild<ValueCard *>(QStringLiteral("humCard"))) {
        if (envOk && e.dht11Valid()) {
            hum->setValue(e.humidityPct);
        } else {
            hum->setInvalid(QStringLiteral("无数据"));
        }
    }

    if (st.batteryValid()) {
        m_voltCard->setValue(st.diag().batteryV);
        m_voltCard->setSeverity(st.batterySeverity());
        m_voltCard->setSub(QStringLiteral("低压线 %1 V")
                               .arg(domain::units::number(domain::units::kLowBatteryV, 2)));
    } else {
        m_voltCard->setInvalid(QStringLiteral("无数据（0x15 才有电压）"));
    }
}

void CurvePanel::replotAll()
{
    if (!isVisible()) {
        return;
    }
    for (ScopePlot *p : {m_mq2Plot, m_mq135Plot, m_voltPlot, m_thPlot}) {
        p->setEndValueVisible(m_endValueBox->isChecked());
        p->setAutoY(m_autoYBox->isChecked());
        p->replot();
    }
    updateCards();
}
