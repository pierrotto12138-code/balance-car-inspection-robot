#include "ui/panels/patrol_panel.h"

#include "data/event_log.h"
#include "domain/alarm.h"
#include "domain/commands.h"
#include "domain/units.h"
#include "io/link_manager.h"
#include "ui/theme/theme.h"
#include "ui/widgets/value_card.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 遥控速度档（协议 §3.6：固件按 |lin| ≥ 50 mm/s、|ang| ≥ 10 dps 判定方向）
constexpr int kLinForward = 250;
constexpr int kAngTurn = 300; // 30.0 dps
/// 左转的符号（★ 待真机核：若实车"左转键却右转"只改这一处，别在按钮回调里各改一遍）
constexpr int kTurnLeftSign = +1;

QString phaseChipText(int phase)
{
    switch (static_cast<proto::Phase>(phase)) {
    case proto::Phase::Init:
        return QStringLiteral("① 初始化");
    case proto::Phase::Warmup:
        return QStringLiteral("② 预热");
    case proto::Phase::Idle:
        return QStringLiteral("③ 待机");
    case proto::Phase::Patrol:
        return QStringLiteral("④ 巡航");
    case proto::Phase::Analysis:
        return QStringLiteral("⑤ 研判");
    case proto::Phase::Return:
        return QStringLiteral("⑥ 返航");
    case proto::Phase::Done:
        return QStringLiteral("⑦ 完成");
    case proto::Phase::Fault:
        return QStringLiteral("故障");
    }
    return QStringLiteral("?");
}

QString chipStyle(bool current, bool fault)
{
    if (fault) {
        return QStringLiteral("color:%1; border:1px solid %1; border-radius:4px; padding:4px 8px;")
            .arg(QString::fromLatin1(theme::kAlarm));
    }
    if (current) {
        return QStringLiteral("color:%1; background:%2; border:1px solid %2; border-radius:4px;"
                              "padding:4px 8px; font-weight:bold;")
            .arg(QString::fromLatin1(theme::kText), QString::fromLatin1(theme::kLine));
    }
    return QStringLiteral("color:%1; border:1px solid %2; border-radius:4px; padding:4px 8px;")
        .arg(QString::fromLatin1(theme::kTextDim), QString::fromLatin1(theme::kLine));
}

} // namespace

PatrolPanel::PatrolPanel(domain::VehicleState *state, io::LinkManager *mgr,
                         domain::AlarmEngine *alarm, domain::KeepaliveEngine *keepalive,
                         data::EventLog *log, QWidget *parent)
    : QWidget(parent), m_state(state), m_mgr(mgr), m_alarm(alarm), m_keepalive(keepalive), m_log(log)
{
    setFocusPolicy(Qt::StrongFocus); // 键盘遥控需要焦点
    buildUi();

    m_timer = new QTimer(this);
    m_timer->setInterval(100); // 10 Hz
    connect(m_timer, &QTimer::timeout, this, &PatrolPanel::refresh);
    m_timer->start();

    if (m_keepalive != nullptr) {
        connect(m_keepalive, &domain::KeepaliveEngine::stateChanged, this,
                &PatrolPanel::updateKeepaliveUi);
        updateKeepaliveUi();
    }
    refresh();
}

void PatrolPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    // ---------------- 安全横幅（本页最显眼的一行）----------------
    m_banner = new QLabel(this);
    m_banner->setProperty("role", "banner");
    m_banner->setFont(theme::monoFont(11, true));
    m_banner->setWordWrap(true);
    root->addWidget(m_banner);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);

    // ---------------- 任务状态 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("任务状态 0x12 · 5 Hz"), this);
        auto *grid = new QGridLayout(g);
        grid->setSpacing(6);
        m_task = new ValueCard(QStringLiteral("任务"), QString(), 0, g);
        m_phase = new ValueCard(QStringLiteral("阶段"), QString(), 0, g);
        m_checkpoint = new ValueCard(QStringLiteral("当前巡检点"), QString(), 0, g);
        m_fault = new ValueCard(QStringLiteral("异常标志"), QString(), 0, g);
        m_task->setValuePointSize(14);
        m_phase->setValuePointSize(14);
        m_fault->setValuePointSize(14);
        grid->addWidget(m_task, 0, 0);
        grid->addWidget(m_phase, 0, 1);
        grid->addWidget(m_checkpoint, 1, 0);
        grid->addWidget(m_fault, 1, 1);

        m_progress = new QProgressBar(g);
        m_progress->setRange(0, 100);
        m_progress->setFormat(QStringLiteral("任务完成度 %p%"));
        grid->addWidget(m_progress, 2, 0, 1, 2);

        // 阶段时间轴
        auto *tl = new QHBoxLayout;
        tl->setSpacing(4);
        for (int p = 0; p <= static_cast<int>(proto::Phase::Done); ++p) {
            auto *chip = new QLabel(phaseChipText(p), g);
            chip->setAlignment(Qt::AlignCenter);
            chip->setStyleSheet(chipStyle(false, false));
            m_chips.append(chip);
            tl->addWidget(chip);
        }
        m_faultChip = new QLabel(phaseChipText(static_cast<int>(proto::Phase::Fault)), g);
        m_faultChip->setAlignment(Qt::AlignCenter);
        m_faultChip->setStyleSheet(chipStyle(false, true));
        m_faultChip->setVisible(false);
        tl->addWidget(m_faultChip);
        grid->addLayout(tl, 3, 0, 1, 2);
        row->addWidget(g, 3);
    }

    // ---------------- 任务控制 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("任务控制（0x21 任务指示 / 0x2F 紧急停机）"), this);
        auto *grid = new QGridLayout(g);
        grid->setSpacing(6);
        m_btnPatrol = new QPushButton(QStringLiteral("下发：自主巡航"), g);
        m_btnReturn = new QPushButton(QStringLiteral("返航"), g);
        m_btnIdle = new QPushButton(QStringLiteral("待机"), g);
        m_btnEstop = new QPushButton(QStringLiteral("⛔ 紧急停机"), g);
        m_btnEstop->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                                      .arg(QString::fromLatin1(theme::kAlarm)));
        m_btnRelease = new QPushButton(QStringLiteral("重下任务解除锁存"), g);
        m_btnRelease->setToolTip(QStringLiteral("协议 v1.1：收到 0x21 任务=自主巡航（或 0x20 使能位=1）"
                                                "即解除安全锁存。\n"
                                                "锁存期间硬件按键也解不开，所以这是唯一的解除路径。"));
        grid->addWidget(m_btnPatrol, 0, 0);
        grid->addWidget(m_btnReturn, 0, 1);
        grid->addWidget(m_btnIdle, 0, 2);
        grid->addWidget(m_btnEstop, 1, 0);
        grid->addWidget(m_btnRelease, 1, 1, 1, 2);
        m_hint = new QLabel(g);
        m_hint->setProperty("role", "sub");
        m_hint->setWordWrap(true);
        grid->addWidget(m_hint, 2, 0, 1, 3);
        row->addWidget(g, 3);
    }
    root->addLayout(row);

    auto *row2 = new QHBoxLayout;
    row2->setSpacing(8);

    // ---------------- 保活 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("保活（防固件 500 ms 判失联而锁存）"), this);
        auto *grid = new QGridLayout(g);
        grid->setSpacing(6);
        m_kaEnable = new QCheckBox(QStringLiteral("开启保活"), g);
        m_kaInterval = new QSpinBox(g);
        m_kaInterval->setRange(50, 2000);
        m_kaInterval->setSingleStep(50);
        m_kaInterval->setSuffix(QStringLiteral(" ms"));
        m_kaInterval->setToolTip(QStringLiteral("必须明显小于固件的 500 ms 判据（默认 200 ms 留 2.5 倍余量）"));
        m_kaMode = new QComboBox(g);
        m_kaMode->addItem(QStringLiteral("心跳 0x1F（安全，不解除锁存）"),
                          int(domain::KeepaliveMode::Heartbeat));
        m_kaMode->addItem(QStringLiteral("使能 0x20（会解除锁存！）"),
                          int(domain::KeepaliveMode::EnableZeroSpeed));
        m_kaState = new QLabel(g);
        m_kaState->setFont(theme::monoFont(9));
        m_kaWarn = new QLabel(g);
        m_kaWarn->setProperty("role", "sub");
        m_kaWarn->setWordWrap(true);

        grid->addWidget(m_kaEnable, 0, 0);
        grid->addWidget(m_kaInterval, 0, 1);
        grid->addWidget(m_kaMode, 0, 2);
        grid->addWidget(m_kaState, 1, 0, 1, 3);
        grid->addWidget(m_kaWarn, 2, 0, 1, 3);
        row2->addWidget(g, 3);
    }

    // ---------------- 遥控 ----------------
    {
        auto *g = new QGroupBox(QStringLiteral("遥控 0x20（按住走 · 松手停）"), this);
        auto *grid = new QGridLayout(g);
        grid->setSpacing(6);
        m_btnFront = new QPushButton(QStringLiteral("前 ↑ (W)"), g);
        m_btnLeft = new QPushButton(QStringLiteral("左 ← (A)"), g);
        m_btnStop = new QPushButton(QStringLiteral("停 ■ (空格)"), g);
        m_btnRight = new QPushButton(QStringLiteral("右 → (D)"), g);
        m_btnBack = new QPushButton(QStringLiteral("后 ↓ (S)"), g);
        grid->addWidget(m_btnFront, 0, 1);
        grid->addWidget(m_btnLeft, 1, 0);
        grid->addWidget(m_btnStop, 1, 1);
        grid->addWidget(m_btnRight, 1, 2);
        grid->addWidget(m_btnBack, 2, 1);
        m_lastSent = new QLabel(g);
        m_lastSent->setProperty("role", "sub");
        m_lastSent->setWordWrap(true);
        grid->addWidget(m_lastSent, 3, 0, 1, 3);
        row2->addWidget(g, 3);
    }
    root->addLayout(row2);
    root->addStretch(1);

    // ---------------- 连线 ----------------
    connect(m_btnPatrol, &QPushButton::clicked, this, [this]() { sendTask(proto::TaskId::Patrol); });
    connect(m_btnReturn, &QPushButton::clicked, this, [this]() { sendTask(proto::TaskId::Return); });
    connect(m_btnIdle, &QPushButton::clicked, this, [this]() { sendTask(proto::TaskId::Idle); });
    connect(m_btnEstop, &QPushButton::clicked, this, &PatrolPanel::sendEstop);
    // "重下任务解除锁存" = 0x21 任务=自主巡航（协议 v1.1 的解除路径）
    connect(m_btnRelease, &QPushButton::clicked, this, [this]() {
        sendTask(proto::TaskId::Patrol);
        m_hint->setText(QStringLiteral("已重下任务（0x21 自主巡航）—— 若锁存已解除，横幅会变回正常；"
                                       "若仍在锁存，检查下行链路（保活是否开启 / 端口是否通）"));
    });

    // 按住走、松手停：pressed/released 一对，语义就是"松手即停"
    connect(m_btnFront, &QPushButton::pressed, this, [this]() { sendMotion(kLinForward, 0); });
    connect(m_btnBack, &QPushButton::pressed, this, [this]() { sendMotion(-kLinForward, 0); });
    connect(m_btnLeft, &QPushButton::pressed, this,
            [this]() { sendMotion(0, kTurnLeftSign * kAngTurn); });
    connect(m_btnRight, &QPushButton::pressed, this,
            [this]() { sendMotion(0, -kTurnLeftSign * kAngTurn); });
    for (QPushButton *b : {m_btnFront, m_btnBack, m_btnLeft, m_btnRight}) {
        connect(b, &QPushButton::released, this, &PatrolPanel::sendStop);
    }
    connect(m_btnStop, &QPushButton::clicked, this, &PatrolPanel::sendStop);

    if (m_keepalive != nullptr) {
        m_kaEnable->setChecked(m_keepalive->enabled());
        m_kaInterval->setValue(m_keepalive->intervalMs());
        m_kaMode->setCurrentIndex(m_keepalive->mode() == domain::KeepaliveMode::Heartbeat ? 0 : 1);
        connect(m_kaEnable, &QCheckBox::toggled, this, [this](bool on) {
            m_keepalive->setEnabled(on);
            if (m_log != nullptr) {
                m_log->onOperatorAction(proto::up::kHeartbeat,
                                        on ? QStringLiteral("开启保活（周期 %1 ms）").arg(m_kaInterval->value())
                                           : QStringLiteral("关闭保活 —— 固件 500 ms 后会安全停车并锁存"),
                                        true);
            }
        });
        connect(m_kaInterval, qOverload<int>(&QSpinBox::valueChanged), this,
                [this](int ms) { m_keepalive->setIntervalMs(ms); });
        connect(m_kaMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            const auto mode = static_cast<domain::KeepaliveMode>(m_kaMode->currentData().toInt());
            m_keepalive->setMode(mode);
            if (m_log != nullptr) {
                m_log->onOperatorAction(
                    domain::KeepaliveEngine::commandFor(mode),
                    mode == domain::KeepaliveMode::Heartbeat
                        ? QStringLiteral("保活方式改为心跳（不会解除锁存）")
                        : QStringLiteral("保活方式改为使能 0x20 —— 会周期性解除安全锁存（危险）"),
                    true);
            }
        });
    }
}

bool PatrolPanel::send(quint8 cmd, const QByteArray &payload, const QString &detail)
{
    const qint64 n = m_mgr != nullptr ? m_mgr->send(cmd, payload) : -1;
    const bool ok = n > 0;
    const QString text = QStringLiteral("%1（%2 B）%3")
                             .arg(QString::fromUtf8(domain::cmd::name(cmd)))
                             .arg(n < 0 ? 0 : n)
                             .arg(ok ? QString() : QStringLiteral(" —— 失败：没有可用链路"));
    m_lastSent->setText(QStringLiteral("最近下发：%1%2").arg(text, detail.isEmpty() ? QString() : QStringLiteral(" · ") + detail));
    if (m_log != nullptr) {
        m_log->onOperatorAction(cmd, detail, ok);
    }
    return ok;
}

void PatrolPanel::sendTask(proto::TaskId id)
{
    const QString detail = QStringLiteral("任务=%1").arg(QString::fromUtf8(domain::taskName(id)));
    // ★ 0x21 可能解除安全锁存 —— 所以按钮上的文案与这里的留档都把后果写清楚
    send(proto::down::kTask, domain::cmd::taskPayload(id), detail);
    m_hint->setText(QStringLiteral("已下发 %1。注意：收到显式使能（0x21 巡航 / 0x20 使能位=1）"
                                   "会**解除**安全锁存，这是协议规定的解除路径。")
                        .arg(detail));
}

void PatrolPanel::sendMotion(int linMmS, int angDpsX10)
{
    if (m_state->safetyLatched()) {
        m_hint->setText(QStringLiteral("已锁存：按协议 §3.6 **禁止起步** —— 请先「重下任务解除锁存」"));
        return;
    }
    const bool enable = (linMmS != 0 || angDpsX10 != 0);
    send(proto::down::kMotion,
         domain::cmd::motionPayload(linMmS, angDpsX10, enable),
         QStringLiteral("lin=%1 mm/s ang=%2 使能=%3")
             .arg(linMmS)
             .arg(angDpsX10)
             .arg(enable ? 1 : 0));
}

void PatrolPanel::sendStop()
{
    send(proto::down::kMotion, domain::cmd::stopPayload(), QStringLiteral("enable=0（立即停）"));
}

void PatrolPanel::sendEstop()
{
    // 急停不做二次确认：它就是"来不及解释了"的那个按钮
    send(proto::down::kEstop, domain::cmd::estopPayload(), QStringLiteral("立即切断电机输出并锁存"));
    m_hint->setText(QStringLiteral("已发急停（0x2F）。锁存后**硬件按键也解不开**，"
                                   "需用「重下任务解除锁存」；锁存期间方向键与巡航/返航已禁用。"));
}

void PatrolPanel::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        QWidget::keyPressEvent(event); // 长按的重复事件不重复下发（一次按下只发一条）
        return;
    }
    switch (event->key()) {
    case Qt::Key_W:
        sendMotion(kLinForward, 0);
        break;
    case Qt::Key_S:
        sendMotion(-kLinForward, 0);
        break;
    case Qt::Key_A:
        sendMotion(0, kTurnLeftSign * kAngTurn);
        break;
    case Qt::Key_D:
        sendMotion(0, -kTurnLeftSign * kAngTurn);
        break;
    case Qt::Key_Space:
        sendStop();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void PatrolPanel::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_W:
    case Qt::Key_S:
    case Qt::Key_A:
    case Qt::Key_D:
        sendStop(); // 松手即停（与按钮同一条路径）
        event->accept();
        return;
    default:
        QWidget::keyReleaseEvent(event);
        return;
    }
}

void PatrolPanel::applyInterlocks(bool latched)
{
    // 锁存期间禁止起步：禁用一切"会让车动"的入口（方向键、巡航、返航）
    for (QPushButton *b : {m_btnFront, m_btnBack, m_btnLeft, m_btnRight, m_btnPatrol, m_btnReturn}) {
        b->setEnabled(!latched);
    }
    m_btnRelease->setEnabled(latched);
    m_btnRelease->setStyleSheet(
        latched ? QStringLiteral("color:%1; font-weight:bold; border:1px solid %1; border-radius:4px;"
                                 "padding:5px 12px;")
                      .arg(QString::fromLatin1(theme::kWarn))
                : QString());
    if (latched != m_lastLatched) {
        m_lastLatched = latched;
        if (!latched) {
            m_hint->setText(QStringLiteral("锁存已解除 —— 可重新下发巡航或使用方向键。"));
        }
    }
}

void PatrolPanel::updatePhaseChips(const domain::TaskData &task, bool valid)
{
    const int cur = valid ? static_cast<int>(task.phase) : -1;
    const bool fault = valid && task.phase == proto::Phase::Fault;
    for (int i = 0; i < m_chips.size(); ++i) {
        m_chips.at(i)->setStyleSheet(chipStyle(i == cur && !fault, false));
    }
    // 故障是"另一种状态"，不与 ①~⑦ 抢位置：它单独一张红牌
    m_faultChip->setVisible(fault);
    m_faultChip->setStyleSheet(chipStyle(false, fault));
}

void PatrolPanel::updateKeepaliveUi()
{
    if (m_keepalive == nullptr) {
        return;
    }
    const bool on = m_keepalive->enabled();
    const bool ready = m_keepalive->linkReady();
    const auto mode = m_keepalive->mode();
    const qint64 since = m_keepalive->sinceLastSendMs();

    QString state = QStringLiteral("已发 %1 条").arg(m_keepalive->sentCount());
    if (since >= 0) {
        state += QStringLiteral(" · 上次 %1 ms 前").arg(since);
    }
    if (!on) {
        state += QStringLiteral(" · 保活已关闭 ⚠（固件 500 ms 后会安全停车并锁存）");
    } else if (!ready) {
        state += QStringLiteral(" · 待链路（链路未打开，暂不发送）");
    } else {
        state += QStringLiteral(" · 周期 %1 ms").arg(m_keepalive->intervalMs());
    }
    m_kaState->setText(state);
    const bool risky = (mode == domain::KeepaliveMode::EnableZeroSpeed);
    m_kaState->setStyleSheet(QStringLiteral("color:%1;")
                                 .arg(QString::fromLatin1(!on || !ready ? theme::kWarn
                                                          : risky        ? theme::kAlarm
                                                                         : theme::kText)));
    m_kaWarn->setText(risky
                          ? QStringLiteral("⚠ 当前用「使能 0x20」保活：协议规定收到使能位=1 即**解除安全锁存**，"
                                           "等于每 %1 ms 自动绕过一次急停保护。除非确实要保持使能，"
                                           "否则请改回心跳。")
                                .arg(m_keepalive->intervalMs())
                          : QStringLiteral("心跳 0x1F 只声明\"上位机在线\"，不使能电机、不解除锁存 —— "
                                           "这是唯一安全的自动保活方式（协议 §七 约定 7）。"));
    m_kaWarn->setStyleSheet(QStringLiteral("color:%1;")
                                .arg(QString::fromLatin1(risky ? theme::kAlarm : theme::kTextDim)));
    if (m_kaEnable->isChecked() != on) {
        QSignalBlocker b(m_kaEnable);
        m_kaEnable->setChecked(on);
    }
}

void PatrolPanel::refresh()
{
    if (!isVisible()) {
        return;
    }
    const auto &st = *m_state;
    const bool latched = st.safetyLatched();

    // ---- 安全横幅（与总览页同一套判定，只是这里更大、更直白）----
    if (latched) {
        m_banner->setText(QStringLiteral("⛔ 已安全锁存 —— 车不会动，需「重下任务解除锁存」"
                                         "（硬件按键也解不开）"));
        m_banner->setStyleSheet(QStringLiteral("color:%1; border:1px solid %1; border-radius:6px;"
                                               "padding:6px 10px; background:%2; font-weight:bold;")
                                    .arg(QString::fromLatin1(theme::kAlarm),
                                         QString::fromLatin1(theme::kPanel)));
    } else if (m_alarm != nullptr && m_alarm->anyActive()) {
        m_banner->setText(QStringLiteral("⚠ 报警中：%1").arg(m_alarm->activeSummary()));
        m_banner->setStyleSheet(QStringLiteral("color:%1; border:1px solid %1; border-radius:6px;"
                                               "padding:6px 10px; background:%2;")
                                    .arg(QString::fromLatin1(theme::kWarn),
                                         QString::fromLatin1(theme::kPanel)));
    } else if (st.alarmActive()) {
        m_banner->setText(QStringLiteral("⚠ 固件异常标志：%1（上位机自身的阈值判定未报警 —— "
                                         "两者不一致本身就是线索：看阈值/传感器就绪位）")
                              .arg(domain::faultFlagsText(st.task().faultFlags)));
        m_banner->setStyleSheet(QStringLiteral("color:%1; border:1px solid %1; border-radius:6px;"
                                               "padding:6px 10px; background:%2;")
                                    .arg(QString::fromLatin1(theme::kWarn),
                                         QString::fromLatin1(theme::kPanel)));
    } else {
        m_banner->setText(QStringLiteral("正常（未锁存、无异常标志）"));
        m_banner->setStyleSheet(QStringLiteral("color:%1; border:1px solid %2; border-radius:6px;"
                                               "padding:6px 10px; background:%3;")
                                    .arg(QString::fromLatin1(theme::kText),
                                         QString::fromLatin1(theme::kLine),
                                         QString::fromLatin1(theme::kPanel)));
    }
    applyInterlocks(latched);

    // ---- 任务状态 ----
    const bool taskOk = st.received(proto::up::kTask) && st.ageMs(proto::up::kTask) <= 1000;
    if (taskOk) {
        const auto &t = st.task();
        m_task->setText(QString::fromUtf8(domain::taskName(t.taskId)));
        m_phase->setText(QString::fromUtf8(domain::phaseName(t.phase)));
        m_checkpoint->setValue(t.checkpoint);
        m_fault->setText(domain::faultFlagsText(t.faultFlags));
        m_fault->setSeverity(t.anyFault() ? domain::Severity::Alarm : domain::Severity::Ok);
        m_progress->setValue(t.progressPct);
        updatePhaseChips(t, true);
    } else {
        const QString hint = st.received(proto::up::kTask)
                                 ? QStringLiteral("已过期 %1 ms").arg(st.ageMs(proto::up::kTask))
                                 : QStringLiteral("无数据");
        for (ValueCard *c : {m_task, m_phase, m_checkpoint, m_fault}) {
            c->setInvalid(hint);
        }
        m_progress->setValue(0);
        updatePhaseChips(domain::TaskData{}, false);
    }

    updateKeepaliveUi();
}
