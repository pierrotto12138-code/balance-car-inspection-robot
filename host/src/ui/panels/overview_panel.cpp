#include "ui/panels/overview_panel.h"

#include "domain/units.h"
#include "io/link_manager.h"
#include "ui/theme/theme.h"
#include "ui/widgets/attitude_indicator.h"
#include "ui/widgets/led_indicator.h"
#include "ui/widgets/value_card.h"

#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 次要文字标签（"键：值"里的键）
QLabel *dimLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "sub");
    return l;
}

/// 等宽数值标签（表格式的短值，用等宽字以免列宽跳动）
QLabel *monoLabel(const QString &text = QStringLiteral("--"))
{
    auto *l = new QLabel(text);
    l->setFont(theme::monoFont(10));
    return l;
}

QString linkCn(io::LinkKind kind)
{
    return QString::fromUtf8(io::linkName(kind));
}

} // namespace

OverviewPanel::OverviewPanel(domain::VehicleState *state, io::LinkManager *mgr, QWidget *parent)
    : QWidget(parent), m_state(state), m_mgr(mgr)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    // ---- 安全横幅：一屏之内最显眼的一行（异常时才有颜色）----
    m_banner = new QLabel(this);
    m_banner->setProperty("role", "banner");
    m_banner->setWordWrap(true);
    root->addWidget(m_banner);

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(8);
    row1->addWidget(buildAttitudeGroup(), 3);
    row1->addWidget(buildMotionGroup(), 2);
    row1->addWidget(buildPowerEnvGroup(), 3);
    root->addLayout(row1);

    auto *row2 = new QHBoxLayout;
    row2->setSpacing(8);
    row2->addWidget(buildTaskGroup(), 3);
    row2->addWidget(buildDiagGroup(), 3);
    row2->addWidget(buildLinkGroup(), 3);
    root->addLayout(row2);

    root->addWidget(buildBitsGroup());
    root->addStretch(1);

    // 20 Hz 拉取（计划 §五）。不可见时不刷新 —— 省 CPU，也避免"看不见的界面在偷偷刷新"。
    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, &OverviewPanel::refresh);
    m_timer->start();
    refresh();
}

QString OverviewPanel::bannerText() const
{
    return m_banner->text();
}

QGroupBox *OverviewPanel::buildAttitudeGroup()
{
    auto *g = new QGroupBox(QStringLiteral("姿态 0x10 · 20 Hz"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    m_horizon = new AttitudeIndicator(g);
    grid->addWidget(m_horizon, 0, 0, 3, 1);

    m_pitch = new ValueCard(QStringLiteral("俯仰 P"), QStringLiteral("°"), 2, g);
    m_roll = new ValueCard(QStringLiteral("横滚 R"), QStringLiteral("°"), 2, g);
    m_yaw = new ValueCard(QStringLiteral("偏航 Y"), QStringLiteral("°"), 2, g);

    m_gyroX = new ValueCard(QStringLiteral("角速度 X"), QStringLiteral("°/s"), 2, g);
    m_gyroY = new ValueCard(QStringLiteral("角速度 Y"), QStringLiteral("°/s"), 2, g);
    m_gyroZ = new ValueCard(QStringLiteral("角速度 Z"), QStringLiteral("°/s"), 2, g);

    grid->addWidget(m_pitch, 0, 1);
    grid->addWidget(m_roll, 0, 2);
    grid->addWidget(m_yaw, 0, 3);
    grid->addWidget(m_gyroX, 1, 1);
    grid->addWidget(m_gyroY, 1, 2);
    grid->addWidget(m_gyroZ, 1, 3);
    return g;
}

QGroupBox *OverviewPanel::buildMotionGroup()
{
    auto *g = new QGroupBox(QStringLiteral("运动 0x11 · 20 Hz"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    m_left = new ValueCard(QStringLiteral("左轮速"), QStringLiteral("mm/s"), 0, g);
    m_right = new ValueCard(QStringLiteral("右轮速"), QStringLiteral("mm/s"), 0, g);
    m_pwm = new ValueCard(QStringLiteral("PWM 占空"), QStringLiteral("%"), 1, g);
    m_dist = new ValueCard(QStringLiteral("超声波距离"), QStringLiteral("cm"), 0, g);

    grid->addWidget(m_left, 0, 0);
    grid->addWidget(m_right, 0, 1);
    grid->addWidget(m_pwm, 1, 0);
    grid->addWidget(m_dist, 1, 1);

    m_avoid = dimLabel(QStringLiteral("避障状态位：--"));
    grid->addWidget(m_avoid, 2, 0, 1, 2);
    return g;
}

QGroupBox *OverviewPanel::buildPowerEnvGroup()
{
    auto *g = new QGroupBox(QStringLiteral("电源与环境（0x15 电压 / 0x14 环境）"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    m_volt = new ValueCard(QStringLiteral("电池电压"), QStringLiteral("V"), 2, g);
    m_temp = new ValueCard(QStringLiteral("温度"), QStringLiteral("℃"), 1, g);
    m_hum = new ValueCard(QStringLiteral("湿度"), QStringLiteral("%RH"), 1, g);
    m_mq2 = new ValueCard(QStringLiteral("MQ-2 浓度"), QStringLiteral("ppm"), 1, g);
    m_mq135 = new ValueCard(QStringLiteral("MQ-135 浓度"), QStringLiteral("ppm"), 1, g);
    m_gear = new ValueCard(QStringLiteral("采样档位"), QString(), 0, g);

    grid->addWidget(m_volt, 0, 0);
    grid->addWidget(m_gear, 0, 1);
    grid->addWidget(m_temp, 1, 0);
    grid->addWidget(m_hum, 1, 1);
    grid->addWidget(m_mq2, 2, 0);
    grid->addWidget(m_mq135, 2, 1);
    return g;
}

QGroupBox *OverviewPanel::buildTaskGroup()
{
    auto *g = new QGroupBox(QStringLiteral("任务与巡检 0x12 · 5 Hz"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    m_task = new ValueCard(QStringLiteral("任务"), QString(), 0, g);
    m_phase = new ValueCard(QStringLiteral("阶段"), QString(), 0, g);
    m_checkpoint = new ValueCard(QStringLiteral("当前巡检点"), QString(), 0, g);
    m_fault = new ValueCard(QStringLiteral("异常标志"), QString(), 0, g);
    m_task->setValuePointSize(13);
    m_phase->setValuePointSize(13);
    m_fault->setValuePointSize(13);

    grid->addWidget(m_task, 0, 0);
    grid->addWidget(m_phase, 0, 1);
    grid->addWidget(m_checkpoint, 1, 0);
    grid->addWidget(m_fault, 1, 1);

    m_progress = new QProgressBar(g);
    m_progress->setRange(0, 100);
    m_progress->setFormat(QStringLiteral("完成度 %p%"));
    grid->addWidget(m_progress, 2, 0, 1, 2);
    return g;
}

QGroupBox *OverviewPanel::buildDiagGroup()
{
    auto *g = new QGroupBox(QStringLiteral("诊断 0x15 / 0x16 · 1 Hz（联调专用）"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    m_heap = new ValueCard(QStringLiteral("剩余堆"), QStringLiteral("KB"), 0, g);
    m_oled = new ValueCard(QStringLiteral("OLED 写入速率"), QStringLiteral("B/s"), 0, g);
    m_fw = new ValueCard(QStringLiteral("固件版本"), QString(), 0, g);
    m_page = new ValueCard(QStringLiteral("OLED 当前页"), QString(), 0, g);
    m_heap->setValuePointSize(13);
    m_oled->setValuePointSize(13);

    grid->addWidget(m_heap, 0, 0);
    grid->addWidget(m_oled, 0, 1);
    grid->addWidget(m_fw, 1, 0);
    grid->addWidget(m_page, 1, 1);

    m_algo = dimLabel(QStringLiteral("姿态算法：--"));
    m_flagShow = dimLabel(QStringLiteral("Flag_Show：--"));
    grid->addWidget(m_algo, 2, 0, 1, 2);
    grid->addWidget(m_flagShow, 3, 0, 1, 2);

    m_balPwm = new ValueCard(QStringLiteral("平衡环 Bal"), QString(), 0, g);
    m_velPwm = new ValueCard(QStringLiteral("速度环 Vel"), QString(), 0, g);
    m_turnPwm = new ValueCard(QStringLiteral("转向环 Turn"), QString(), 0, g);
    grid->addWidget(m_balPwm, 4, 0);
    grid->addWidget(m_velPwm, 4, 1);
    grid->addWidget(m_turnPwm, 5, 0);
    return g;
}

QGroupBox *OverviewPanel::buildLinkGroup()
{
    auto *g = new QGroupBox(QStringLiteral("链路与数据来源"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(6);

    const QStringList head{QStringLiteral("链路"), QStringLiteral("状态"), QStringLiteral("解帧"),
                           QStringLiteral("收字节"), QStringLiteral("最近帧")};
    for (int c = 0; c < head.size(); ++c) {
        grid->addWidget(dimLabel(head.at(c)), 0, c);
    }

    auto addRow = [&](int row, io::LinkKind kind, QLabel *&st, QLabel *&fr, QLabel *&by, QLabel *&age) {
        grid->addWidget(new QLabel(linkCn(kind)), row, 0);
        st = monoLabel();
        fr = monoLabel();
        by = monoLabel();
        age = monoLabel();
        grid->addWidget(st, row, 1);
        grid->addWidget(fr, row, 2);
        grid->addWidget(by, row, 3);
        grid->addWidget(age, row, 4);
    };
    addRow(1, io::LinkKind::Wire, m_wireState, m_wireFrames, m_wireBytes, m_wireAge);
    addRow(2, io::LinkKind::Bluetooth, m_btState, m_btFrames, m_btBytes, m_btAge);

    m_primary = dimLabel(QStringLiteral("主链路：--"));
    m_counters = dimLabel(QStringLiteral("本机统计：--"));
    m_sources = dimLabel(QStringLiteral("数据来源：--"));
    m_sources->setWordWrap(true);
    grid->addWidget(m_primary, 3, 0, 1, 5);
    grid->addWidget(m_counters, 4, 0, 1, 5);
    grid->addWidget(m_sources, 5, 0, 1, 5);
    return g;
}

QGroupBox *OverviewPanel::buildBitsGroup()
{
    auto *g = new QGroupBox(QStringLiteral("状态位（0x12 异常标志 · 0x14 就绪位 · 0x15 状态位 · 0x16 执行位）"));
    auto *grid = new QGridLayout(g);
    grid->setSpacing(4);
    grid->setHorizontalSpacing(18);

    m_ledAlarm = new LedIndicator(QStringLiteral("报警中"), g);
    m_ledLatch = new LedIndicator(QStringLiteral("已安全锁存"), g);
    m_ledDht = new LedIndicator(QStringLiteral("DHT11 有效"), g);
    m_ledMq2 = new LedIndicator(QStringLiteral("MQ-2 就绪"), g);
    m_ledMq135 = new LedIndicator(QStringLiteral("MQ-135 就绪"), g);
    m_ledComp = new LedIndicator(QStringLiteral("温湿度补偿"), g);
    m_ledBtSeen = new LedIndicator(QStringLiteral("蓝牙收过数据"), g);
    m_ledHeartbeat = new LedIndicator(QStringLiteral("心跳 0x1F"), g);
    m_ledSetPwm = new LedIndicator(QStringLiteral("Set_Pwm 已调用"), g);
    m_ledNotExec = new LedIndicator(QStringLiteral("算了没执行"), g);
    m_ledFlagStop = new LedIndicator(QStringLiteral("Flag_Stop"), g);
    m_ledMotorDis = new LedIndicator(QStringLiteral("电机被禁"), g);

    LedIndicator *leds[] = {m_ledAlarm, m_ledLatch, m_ledDht, m_ledMq2, m_ledMq135, m_ledComp,
                            m_ledBtSeen, m_ledHeartbeat, m_ledSetPwm, m_ledNotExec, m_ledFlagStop,
                            m_ledMotorDis};
    const int cols = 4;
    for (int i = 0; i < 12; ++i) {
        grid->addWidget(leds[i], i / cols, i % cols);
    }

    static const char *tickNames[4] = {"tick 通信", "tick 气体", "tick 状态", "tick 界面"};
    for (int i = 0; i < 4; ++i) {
        m_ledTick[i] = new LedIndicator(QString::fromUtf8(tickNames[i]), g);
        grid->addWidget(m_ledTick[i], 3, i);
    }
    return g;
}

// ---------------------------------------------------------------- 刷新

QString OverviewPanel::sourceText(quint8 cmd) const
{
    const qint64 age = m_state->ageMs(cmd);
    if (age < 0) {
        return QStringLiteral("无数据");
    }
    const QString ageText = (age < 1000)
                                ? QStringLiteral("%1 ms").arg(age)
                                : QStringLiteral("%1 s").arg(domain::units::number(age / 1000.0, 1));
    return QStringLiteral("%1 · %2").arg(linkCn(m_state->sourceOf(cmd)), ageText);
}

bool OverviewPanel::fresh(quint8 cmd, qint64 staleMs) const
{
    const qint64 age = m_state->ageMs(cmd);
    return age >= 0 && age <= staleMs;
}

void OverviewPanel::setBanner(domain::Severity s, const QString &text)
{
    const char *color = theme::kTextDim;
    switch (s) {
    case domain::Severity::Ok:
        color = theme::kText;
        break;
    case domain::Severity::Warn:
        color = theme::kWarn;
        break;
    case domain::Severity::Alarm:
        color = theme::kAlarm;
        break;
    case domain::Severity::Unknown:
        break;
    }
    m_banner->setText(text);
    m_banner->setStyleSheet(QStringLiteral("color:%1; border:1px solid %2; border-radius:6px;"
                                           "padding:6px 10px; background:%3;")
                                .arg(QString::fromLatin1(color),
                                     QString::fromLatin1(s == domain::Severity::Ok ? theme::kLine
                                                                                   : color),
                                     QString::fromLatin1(theme::kPanel)));
}

void OverviewPanel::refresh()
{
    if (!isVisible()) {
        return; // 不可见时不刷新（计划 §五：UI 定时刷新，但没必要为看不到的界面烧 CPU）
    }
    const auto &st = *m_state;

    // ---- ① 安全横幅（最显眼的一行；正常时中性色）----
    setBanner(st.safetySeverity(), st.safetySummary());

    // ---- ② 姿态 ----
    const bool poseOk = fresh(proto::up::kPose, domain::VehicleState::kPoseStaleMs);
    if (poseOk) {
        const auto &p = st.pose();
        m_horizon->setAttitude(p.rollDeg, p.pitchDeg, true);
        m_pitch->setValue(p.pitchDeg);
        m_roll->setValue(p.rollDeg);
        m_yaw->setValue(p.yawDeg);
        m_gyroX->setValue(p.gyroXDps);
        m_gyroY->setValue(p.gyroYDps);
        m_gyroZ->setValue(p.gyroZDps);
        const QString src = sourceText(proto::up::kPose);
        for (ValueCard *c : {m_pitch, m_roll, m_yaw, m_gyroX, m_gyroY, m_gyroZ}) {
            c->setSub(src);
        }
    } else if (st.received(proto::up::kPose)) {
        // 收到过但已过期：**置灰 + 明说"已过期"**，否则冻住的画面会被当成"车停住了"
        const QString hint = QStringLiteral("已过期 %1 ms").arg(st.ageMs(proto::up::kPose));
        m_horizon->setAttitude(st.pose().rollDeg, st.pose().pitchDeg, false);
        for (ValueCard *c : {m_pitch, m_roll, m_yaw, m_gyroX, m_gyroY, m_gyroZ}) {
            c->setInvalid(hint);
        }
    } else {
        m_horizon->setAttitude(0, 0, false);
        for (ValueCard *c : {m_pitch, m_roll, m_yaw, m_gyroX, m_gyroY, m_gyroZ}) {
            c->setInvalid(QStringLiteral("无数据"));
        }
    }

    // ---- ③ 运动 ----
    const bool motionOk = fresh(proto::up::kMotion, domain::VehicleState::kMotionStaleMs);
    if (motionOk) {
        const auto &m = st.motion();
        m_left->setValue(m.leftMmS);
        m_right->setValue(m.rightMmS);
        m_pwm->setValue(m.pwmPercent);
        m_dist->setValue(m.distanceCm);
        const QString src = sourceText(proto::up::kMotion);
        m_left->setSub(src);
        m_right->setSub(src);
        m_pwm->setSub(src);
        m_dist->setSub(src);
        m_avoid->setText(QStringLiteral("避障状态位：0b%1（协议未细化位定义，此处只原样显示）")
                             .arg(m.avoidFlags, 4, 2, QLatin1Char('0')));
    } else {
        const QString hint = st.received(proto::up::kMotion)
                                 ? QStringLiteral("已过期 %1 ms").arg(st.ageMs(proto::up::kMotion))
                                 : QStringLiteral("无数据");
        for (ValueCard *c : {m_left, m_right, m_pwm, m_dist}) {
            c->setInvalid(hint);
        }
        m_avoid->setText(QStringLiteral("避障状态位：--"));
    }

    // ---- ④ 电源与环境 ----
    if (st.batteryValid()) {
        m_volt->setValue(st.diag().batteryV);
        m_volt->setSeverity(st.batterySeverity());
        m_volt->setSub(st.batteryLow()
                           ? QStringLiteral("低于报警线 %1 V")
                                 .arg(domain::units::number(domain::units::kLowBatteryV, 2))
                           : QStringLiteral("正常（报警线 %1 V）")
                                 .arg(domain::units::number(domain::units::kLowBatteryV, 2)));
    } else {
        m_volt->setInvalid(QStringLiteral("无数据（0x15 才有电压）"));
    }

    const bool envOk = st.received(proto::up::kEnvGas);
    if (envOk) {
        const auto &e = st.env();
        m_temp->setValue(e.tempC);
        m_hum->setValue(e.humidityPct);
        m_mq2->setValue(e.mq2Ppm);
        m_mq135->setValue(e.mq135Ppm);
        m_gear->setText(QString::fromUtf8(domain::sampleGearName(e.gear)));
        const QString src = sourceText(proto::up::kEnvGas);
        for (ValueCard *c : {m_temp, m_hum, m_mq2, m_mq135, m_gear}) {
            c->setSub(src);
        }
        // 就绪位为 0 时**不能显示"0 ppm"让人以为空气干净** —— 置灰并写明"未就绪"
        if (!e.mq2Ready()) {
            m_mq2->setInvalid(QStringLiteral("MQ-2 未就绪（未插/未预热）"));
        }
        if (!e.mq135Ready()) {
            m_mq135->setInvalid(QStringLiteral("MQ-135 未就绪（未插/未预热）"));
        }
        if (!e.dht11Valid()) {
            m_temp->setInvalid(QStringLiteral("DHT11 无效"));
            m_hum->setInvalid(QStringLiteral("DHT11 无效"));
        }
        // 超阈值（由固件给的异常标志）→ 红；P2 会换成带迟滞的阈值判定
        if (st.received(proto::up::kTask) && st.task().gasFault()) {
            m_mq2->setSeverity(domain::Severity::Alarm);
            m_mq135->setSeverity(domain::Severity::Alarm);
        }
    } else {
        for (ValueCard *c : {m_temp, m_hum, m_mq2, m_mq135, m_gear}) {
            c->setInvalid(QStringLiteral("无数据"));
        }
    }

    // ---- ⑤ 任务 ----
    const bool taskOk = fresh(proto::up::kTask, domain::VehicleState::kTaskStaleMs);
    if (taskOk) {
        const auto &t = st.task();
        m_task->setText(QString::fromUtf8(domain::taskName(t.taskId)));
        m_phase->setText(QString::fromUtf8(domain::phaseName(t.phase)));
        m_checkpoint->setValue(t.checkpoint);
        m_fault->setText(domain::faultFlagsText(t.faultFlags));
        m_fault->setSeverity(t.anyFault() ? domain::Severity::Alarm : domain::Severity::Ok);
        m_progress->setValue(t.progressPct);
        const QString src = sourceText(proto::up::kTask);
        m_task->setSub(src);
        m_phase->setSub(src);
        m_checkpoint->setSub(src);
        m_fault->setSub(src);
        if (t.phase == proto::Phase::Fault) {
            m_phase->setSeverity(domain::Severity::Alarm);
        }
    } else {
        const QString hint = st.received(proto::up::kTask)
                                 ? QStringLiteral("已过期 %1 ms").arg(st.ageMs(proto::up::kTask))
                                 : QStringLiteral("无数据");
        for (ValueCard *c : {m_task, m_phase, m_checkpoint, m_fault}) {
            c->setInvalid(hint);
        }
        m_progress->setValue(0);
    }

    // ---- ⑥ 诊断 ----
    if (st.received(proto::up::kDiag)) {
        const auto &d = st.diag();
        m_heap->setValue(d.heapKb);
        m_oled->setValue(st.oledBytesPerSec());
        m_fw->setText(QStringLiteral("--"));
        m_page->setText(QString::fromUtf8(domain::diagPageName(d.page)));
        m_algo->setText(QStringLiteral("姿态算法：%1 · 剩余堆 %2 KB")
                            .arg(QString::fromUtf8(domain::attitudeAlgoName(d.attitudeAlgo)))
                            .arg(d.heapKb));
        m_flagShow->setText(QStringLiteral("Flag_Show：%1（%2）")
                                .arg(d.flagShow ? QStringLiteral("1") : QStringLiteral("0"),
                                     d.flagShow ? QStringLiteral("屏幕让位给 DataScope 波形")
                                                : QStringLiteral("屏幕归自研页")));
        m_heap->setSub(sourceText(proto::up::kDiag));
        m_oled->setSub(QStringLiteral("不涨 = 屏幕没在刷"));
        m_page->setSub(sourceText(proto::up::kDiag));
        // OLED 写速率异常：诊断帧在来、但计数不涨 → 屏幕确实没被刷
        if (st.oledBytesPerSec() <= 0.0) {
            m_oled->setSeverity(domain::Severity::Warn);
        }
    } else {
        m_heap->setInvalid(QStringLiteral("无数据"));
        m_oled->setInvalid(QStringLiteral("无数据"));
        m_page->setInvalid(QStringLiteral("无数据"));
        m_fw->setText(QStringLiteral("--"));
        m_algo->setText(QStringLiteral("姿态算法：--"));
        m_flagShow->setText(QStringLiteral("Flag_Show：--"));
    }
    if (st.received(proto::up::kLoop)) {
        const auto &l = st.loop();
        m_balPwm->setValue(l.balancePwm);
        m_velPwm->setValue(l.velocityPwm);
        m_turnPwm->setValue(l.turnPwm);
        m_fw->setText(domain::firmwareVersionText(l.fwVersion));
        m_fw->setSub(sourceText(proto::up::kLoop));
        m_balPwm->setSub(QStringLiteral("三环实况 0x16"));
        m_velPwm->setSub(QStringLiteral("权重是平衡环的 13 倍"));
        m_turnPwm->setSub(QStringLiteral("仅前后运动时生效"));
    } else {
        m_balPwm->setInvalid(QStringLiteral("无数据"));
        m_velPwm->setInvalid(QStringLiteral("无数据"));
        m_turnPwm->setInvalid(QStringLiteral("无数据"));
        m_fw->setSub(QStringLiteral("0x16 未上报（旧固件？）"));
    }

    // ---- ⑦ 链路与来源 ----
    QLabel *stateLbl[2] = {m_wireState, m_btState};
    QLabel *frameLbl[2] = {m_wireFrames, m_btFrames};
    QLabel *byteLbl[2] = {m_wireBytes, m_btBytes};
    QLabel *ageLbl[2] = {m_wireAge, m_btAge};
    for (int i = 0; i < 2; ++i) {
        const auto kind = static_cast<io::LinkKind>(i);
        const auto stats = m_mgr->stats(kind);
        const bool open = m_mgr->isOpen(kind);
        stateLbl[i]->setText(QString::fromUtf8(io::stateName(open ? io::LinkState::Open
                                                                 : io::LinkState::Closed)));
        stateLbl[i]->setStyleSheet(QStringLiteral("color:%1;")
                                       .arg(QString::fromLatin1(open ? theme::kOk : theme::kOffline)));
        frameLbl[i]->setText(QStringLiteral("%1 (丢 %2)").arg(stats.framesOk).arg(stats.framesDropped));
        byteLbl[i]->setText(QStringLiteral("%1 B").arg(stats.bytesRx));
        const qint64 age = st.linkAgeMs(kind);
        ageLbl[i]->setText(age < 0 ? QStringLiteral("--") : QStringLiteral("%1 ms").arg(age));
    }
    const io::LinkKind primary = m_mgr->primaryLink();
    m_primary->setText(primary == io::LinkKind::Count
                           ? QStringLiteral("主链路：无（两条都未连接）")
                           : QStringLiteral("主链路：%1（有线优先，协议 §七 约定 6）").arg(linkCn(primary)));
    m_counters->setText(QStringLiteral("本机统计：格式错帧 %1 · 协议外命令 %2 · 已收到命令 "
                                       "0x10/0x11/0x12/0x14/0x15/0x16 = %3/%4/%5/%6/%7/%8")
                            .arg(st.malformedFrames())
                            .arg(st.unknownFrames())
                            .arg(st.received(proto::up::kPose) ? QStringLiteral("有") : QStringLiteral("无"),
                                 st.received(proto::up::kMotion) ? QStringLiteral("有") : QStringLiteral("无"),
                                 st.received(proto::up::kTask) ? QStringLiteral("有") : QStringLiteral("无"),
                                 st.received(proto::up::kEnvGas) ? QStringLiteral("有") : QStringLiteral("无"),
                                 st.received(proto::up::kDiag) ? QStringLiteral("有") : QStringLiteral("无"),
                                 st.received(proto::up::kLoop) ? QStringLiteral("有") : QStringLiteral("无")));
    m_sources->setText(QStringLiteral("数据来源：姿态 %1 ｜ 运动 %2 ｜ 任务 %3 ｜ 环境 %4 ｜ 诊断 %5")
                           .arg(sourceText(proto::up::kPose), sourceText(proto::up::kMotion),
                                sourceText(proto::up::kTask), sourceText(proto::up::kEnvGas),
                                sourceText(proto::up::kDiag)));
    // 两条链路都会发周期帧（协议 §七 约定 6），这里显示的是**最后一次有效值**的来源；
    // 把这句话放进 tooltip，避免"为什么全是蓝牙"这种疑问没法当场回答。
    m_sources->setToolTip(QStringLiteral("两条链路都会周期性上报同样的帧（协议 §七 约定 6）。\n"
                                         "这里显示的是最后一次**有效值**来自哪条链路、距今多久；\n"
                                         "下面链路表的\"解帧/最近帧\"才是各链路自己的统计。"));

    // ---- ⑧ 状态位 ----
    const auto &d = st.diag();
    const auto &e = st.env();
    const auto &t = st.task();
    const auto &l = st.loop();
    const bool diagOk = st.received(proto::up::kDiag);
    const bool envSeen = st.received(proto::up::kEnvGas);
    const bool loopOk = st.received(proto::up::kLoop);

    m_ledAlarm->setLevel((diagOk && d.alarmActive()) || (taskOk && t.anyFault())
                             ? LedIndicator::Level::Alarm
                             : LedIndicator::Level::Off,
                         (taskOk && t.anyFault()) ? domain::faultFlagsText(t.faultFlags) : QString());
    m_ledLatch->setLevel(st.safetyLatched() ? LedIndicator::Level::Alarm : LedIndicator::Level::Off,
                         st.safetyLatched() ? QStringLiteral("需重下任务解除") : QString());
    m_ledDht->setLevel((diagOk && d.dht11Valid()) || (envSeen && e.dht11Valid())
                           ? LedIndicator::Level::Ok
                           : LedIndicator::Level::Off);
    m_ledMq2->setLevel(envSeen && e.mq2Ready() ? LedIndicator::Level::Ok : LedIndicator::Level::Off,
                       envSeen && !e.mq2Ready() ? QStringLiteral("未就绪") : QString());
    m_ledMq135->setLevel(envSeen && e.mq135Ready() ? LedIndicator::Level::Ok : LedIndicator::Level::Off,
                         envSeen && !e.mq135Ready() ? QStringLiteral("未就绪") : QString());
    m_ledComp->setLevel(envSeen && e.compensationOn() ? LedIndicator::Level::Ok : LedIndicator::Level::Off);
    m_ledBtSeen->setLevel(diagOk && d.btLinkSeen() ? LedIndicator::Level::Ok : LedIndicator::Level::Off);
    const qint64 hbAge = st.ageMs(proto::up::kHeartbeat);
    m_ledHeartbeat->setLevel(hbAge >= 0 && hbAge <= 500 ? LedIndicator::Level::Ok : LedIndicator::Level::Off,
                             hbAge >= 0 ? QStringLiteral("%1 ms").arg(hbAge) : QString());
    m_ledSetPwm->setLevel(loopOk && l.setPwmCalled() ? LedIndicator::Level::Ok : LedIndicator::Level::Off);
    m_ledNotExec->setLevel(loopOk && l.computedButNotExecuted() ? LedIndicator::Level::Alarm
                                                               : LedIndicator::Level::Off,
                           loopOk && l.computedButNotExecuted() ? QStringLiteral("算了没执行！")
                                                                : QString());
    m_ledFlagStop->setLevel(loopOk && l.flagStop() ? LedIndicator::Level::Warn : LedIndicator::Level::Off);
    m_ledMotorDis->setLevel(loopOk && l.motorDisabled() ? LedIndicator::Level::Warn
                                                        : LedIndicator::Level::Off);
    for (int i = 0; i < 4; ++i) {
        m_ledTick[i]->setLevel(st.taskTickAlive(i) && diagOk ? LedIndicator::Level::Ok
                                                             : LedIndicator::Level::Off,
                               st.taskTickAlive(i) && diagOk ? QString() : QStringLiteral("不涨=卡死"));
    }
}
