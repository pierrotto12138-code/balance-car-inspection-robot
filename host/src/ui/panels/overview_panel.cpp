#include "ui/panels/overview_panel.h"

#include "domain/alarm.h"
#include "domain/units.h"
#include "data/param_store.h"
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

OverviewPanel::OverviewPanel(domain::VehicleState *state, io::LinkManager *mgr,
                             domain::AlarmEngine *alarm, data::ParamStore *params, QWidget *parent)
    : QWidget(parent), m_state(state), m_mgr(mgr), m_alarm(alarm), m_params(params)
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

void OverviewPanel::setUpdatesPaused(bool on)
{
    if (on) {
        m_timer->stop();
    } else {
        m_timer->start();
        refresh();
    }
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
    // ★ 显示成**带符号的估算占空比**（−100%…+100%）：负值 = 反向，**不是异常**。
    //   换算式来自固件：`Set_Pwm` 里 `CCR = |Moto|×1.17`、CCR 满量程 8400
    //   ⇒ duty% = Moto ÷ 7179.5 × 100（见 units.h 的说明）。真正的异常是**顶到满量程**
    //   （CCR 被截断、控制失去分辨率），所以只有饱和时才给琥珀提示。
    m_pwm = new ValueCard(QStringLiteral("PWM 占空（估）"), QStringLiteral("%"), 1, g);
    m_pwm->setToolTip(QStringLiteral(
        "由 0x11 的「带符号电机输出均值」估算：duty% = Moto ÷ 7179.5 × 100\n"
        "（固件 `Set_Pwm`：CCR = |Moto| × 1.17；CCR 满量程 8400）\n"
        "★ 负值表示反向，属正常；|Moto| > 7179 时 CCR 顶到 8400 ⇒ 输出饱和（会提示）"));
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
    auto *g = new QGroupBox(QStringLiteral("任务与巡检 0x12 · 与 0x10/0x11 同频（20 Hz）"));
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
    m_ledNotExec = new LedIndicator(QStringLiteral("未驱动"), g);
    m_ledTurnDiff = new LedIndicator(QStringLiteral("Turn=0 两轮不同"), g);
    m_ledTiltProt = new LedIndicator(QStringLiteral("倾角保护 >40°"), g);
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
    // 0x16 的"一眼定位故障"组合（B 侧接口文档 §2.1）单独放一行：它们是**判读结论**，
    // 不是原始位，和上面的位域混在一起会让人分不清"这是位还是结论"。
    grid->addWidget(m_ledTurnDiff, 4, 0, 1, 2);
    grid->addWidget(m_ledTiltProt, 4, 2, 1, 2);
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

QString OverviewPanel::sourceAgeText(quint8 cmd) const
{
    // 只回"多久之前"（数字 + 单位），用于把"数据来源"那一行压缩到一行放得下；
    // 具体是哪条链路看下面的链路表（每页状态条也写着）。
    const qint64 age = m_state->ageMs(cmd);
    if (age < 0) {
        return QStringLiteral("--");
    }
    return (age < 1000) ? QString::number(age)
                        : QStringLiteral("%1s").arg(domain::units::number(age / 1000.0, 0));
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
    // 优先级：安全锁存（车不会动）> 活动告警 > 低压 > 无数据/链路超时 > 正常。
    // 注意：报警的判定**只认 AlarmEngine**（阈值+迟滞+连续确认），
    // 这里不再自己按数值比一遍 —— 两套判据一定会打架。
    if (st.safetyLatched()) {
        setBanner(domain::Severity::Alarm, st.safetySummary());
    } else if (m_alarm != nullptr && m_alarm->anyActive()) {
        setBanner(m_alarm->severity(),
                  QStringLiteral("⚠ 报警中：%1").arg(m_alarm->activeSummary()));
    } else {
        setBanner(st.safetySeverity(), st.safetySummary());
    }

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
        // ★ 偏航角特殊说明：厂商 DMP 只算了 Pitch/Roll，**固件没有计算偏航** ⇒ 该字段恒 0。
        //   如实标注，免得让人以为"车真的一直没转过"（若将来固件补上，这里会自动恢复显示来源）。
        if (p.yawDeg == 0.0) {
            m_yaw->setSub(QStringLiteral("固件未计算偏航（该字段恒 0）"));
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
        m_pwm->setValue(domain::units::motorDutyPercent(m.motorOutAvg)); // 带符号估算占空比
        m_dist->setValue(domain::units::cmFromMm(m.distanceMm)); // 字段是 mm，界面按 cm 显示
        const QString src = sourceText(proto::up::kMotion);
        m_left->setSub(src);
        m_right->setSub(src);
        m_dist->setSub(src);
        // 负值不报警（反向是正常工作）；只有**饱和**才提示（CCR 被 8400 截断 ⇒ 控制失去分辨率）
        if (domain::units::motorDutySaturated(m.motorOutAvg)) {
            m_pwm->setSeverity(domain::Severity::Warn);
            m_pwm->setSub(QStringLiteral("已顶到满量程（|Moto|=%1 > %2 ⇒ CCR 被 8400 截断，"
                                         "控制失去分辨率；原始值 %3）")
                              .arg(std::abs(m.motorOutAvg))
                              .arg(domain::units::number(domain::units::kMotoFullScale, 0))
                              .arg(m.motorOutAvg));
        } else {
            m_pwm->setSub(QStringLiteral("%1 · 带符号（负=反向）· 原始值 %2/±8200")
                              .arg(src)
                              .arg(m.motorOutAvg));
        }
        m_avoid->setText(QStringLiteral("避障位 0b%1").arg(m.avoidFlags, 4, 2, QLatin1Char('0')));
        m_avoid->setToolTip(QStringLiteral("0x11 避障状态位；协议未定义位含义 ⇒ 原样显示，不作语义推断。"));
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
        if (st.batteryAbsent()) {
            m_volt->setSub(QStringLiteral("低于“有电池”门槛 %1 V —— 按无电池处理（USB 供电？）")
                               .arg(domain::units::number(domain::kBatteryPresenceV, 2)));
        } else {
            m_volt->setSub(st.batteryLow()
                               ? QStringLiteral("低于报警线 %1 V —— 控制侧 ≥%2 V 恢复；"
                                                "★ App 侧锁存不会自动清，需「重下任务解除锁存」")
                                     .arg(domain::units::number(domain::units::kLowBatteryV, 2))
                                     .arg(domain::units::number(domain::kLowBatteryReleaseV, 2))
                               : QStringLiteral("正常（报警线 %1 V）")
                                     .arg(domain::units::number(domain::units::kLowBatteryV, 2)));
        }
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
        // 就绪位为 0 时**不能显示"0 ppm"让人以为空气干净** —— 置灰并写明"未就绪"。
        // ★ 2026-10-15 补充：当前台架只有 MQ-135 模块且插在 J13（板上丝印是 "MQ-2" 座），
        //   固件已把 **MQ-135 角色指派给 J13**（`sensor_adc.h`）⇒ 正常现象是
        //   **MQ-135 有数、MQ-2 一直是"未就绪 + 0"（空座）**，不要当成故障。
        if (!e.mq2Ready()) {
            m_mq2->setInvalid(QStringLiteral("未就绪"));
            m_mq2->setToolTip(QStringLiteral("该座未插模块 / 未预热 / 未完成标定 —— "
                                             "0 ppm 不代表空气干净（固件对无效样本不计入标定，故不报假 ppm）"));
        }
        if (!e.mq135Ready()) {
            m_mq135->setInvalid(QStringLiteral("未就绪"));
            m_mq135->setToolTip(QStringLiteral("该座未插模块 / 未预热 / 未完成标定 —— "
                                               "0 ppm 不代表空气干净（固件对无效样本不计入标定，故不报假 ppm）"));
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
        frameLbl[i]->setText(QStringLiteral("%1 帧（丢 %2）").arg(stats.framesOk).arg(stats.framesDropped));
        byteLbl[i]->setText(QStringLiteral("%1 B").arg(stats.bytesRx));
        const qint64 age = st.linkAgeMs(kind);
        ageLbl[i]->setText(age < 0 ? QStringLiteral("--") : QStringLiteral("%1 ms").arg(age));
    }
    const io::LinkKind primary = m_mgr->primaryLink();
    // ★ 文案精简（用户反馈"这些字很占地方"）：细节移到 tooltip，界面上只留结论 ——
    //   一行放得下 ⇒ 1366×768 全屏不必再拉滚动条。
    m_primary->setText(primary == io::LinkKind::Count
                           ? QStringLiteral("主链路：无")
                           : QStringLiteral("主链路：%1").arg(linkCn(primary)));
    m_primary->setToolTip(QStringLiteral("判定规则：有线优先（协议 §七 约定 6）。"));
    m_counters->setText(QStringLiteral("%1·%2·%3·%4 | %5")
                            .arg(m_alarm != nullptr ? m_alarm->activeCount() : 0)
                            .arg(st.malformedFrames())
                            .arg(st.unknownFrames())
                            .arg(st.duplicateFrames())
                            .arg(st.received(proto::up::kPose) && st.received(proto::up::kMotion) &&
                                         st.received(proto::up::kTask) && st.received(proto::up::kEnvGas) &&
                                         st.received(proto::up::kDiag) && st.received(proto::up::kLoop)
                                     ? QStringLiteral("6/6")
                                     : QStringLiteral("%1/6")
                                           .arg(int(st.received(proto::up::kPose)) +
                                                int(st.received(proto::up::kMotion)) +
                                                int(st.received(proto::up::kTask)) +
                                                int(st.received(proto::up::kEnvGas)) +
                                                int(st.received(proto::up::kDiag)) +
                                                int(st.received(proto::up::kLoop)))));
    // ★ 用户反馈"这些字很占地方" ⇒ 界面上只留数字，含义全部搬进 tooltip。
    m_counters->setToolTip(QStringLiteral(
        "四项计数（依次）：活动告警 · 格式错帧 · 协议外命令 · 跨链路重复帧（已去重）。\n"
        "前两项应为 0。末项 = 已覆盖的命令字数 / 6（0x10/0x11/0x12/0x14/0x15/0x16）。"));
    m_sources->setText(QStringLiteral("最近帧来源 / 年龄(ms)：%1 %2 %3 %4 %5")
                           .arg(sourceAgeText(proto::up::kPose), sourceAgeText(proto::up::kMotion),
                                sourceAgeText(proto::up::kTask), sourceAgeText(proto::up::kEnvGas),
                                sourceAgeText(proto::up::kDiag)));
    // 两条链路都会发周期帧（协议 §七 约定 6），这里显示的是**最后一次有效值**的来源；
    // 把这句话放进 tooltip，避免"为什么全是蓝牙"这种疑问没法当场回答。
    m_sources->setToolTip(QStringLiteral("两条链路都会周期性上报同样的帧（协议 §七 约定 6）。\n"
                                         "此处显示最后一次有效值来自哪条链路、距今多久；\n"
                                         "下面链路表的\"解帧/最近帧\"才是各链路自己的统计。"));

    // ---- ⑧ 状态位 ----
    const auto &d = st.diag();
    const auto &e = st.env();
    const auto &t = st.task();
    const auto &l = st.loop();
    const bool diagOk = st.received(proto::up::kDiag);
    const bool envSeen = st.received(proto::up::kEnvGas);
    const bool loopOk = st.received(proto::up::kLoop);

    m_ledAlarm->setLevel((diagOk && d.alarmActive()) || (taskOk && t.anyFault()) ||
                                 (m_alarm != nullptr && m_alarm->anyActive())
                             ? LedIndicator::Level::Alarm
                             : LedIndicator::Level::Off,
                         (m_alarm != nullptr && m_alarm->anyActive())
                             ? QStringLiteral("%1 项").arg(m_alarm->activeCount())
                             : ((taskOk && t.anyFault()) ? domain::faultFlagsText(t.faultFlags)
                                                         : QString()));
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
    // "算了没执行"要**分级**（B 侧接口文档 §2.1/§4）：未使能/锁存时 CCR=0 是保护生效，属正常
    const domain::LoopData::Execution exec =
        loopOk ? l.executionState() : domain::LoopData::Execution::Ok;
    m_ledNotExec->setLevel(exec == domain::LoopData::Execution::Mismatch ? LedIndicator::Level::Alarm
                             : exec == domain::LoopData::Execution::Protected
                                 ? LedIndicator::Level::Warn
                                 : LedIndicator::Level::Off,
                           exec == domain::LoopData::Execution::Mismatch
                               ? QStringLiteral("未驱动（未停车但 CCR=0）")
                           : exec == domain::LoopData::Execution::Protected
                               ? QStringLiteral("保护中，未驱动（Flag_Stop/锁存）")
                               : QString());
    // 0x16 判读组合（B 侧接口文档 §2.1）：差速/极性 bug 的**线索**。
    // ★ 2026-10-15 复查降级为琥珀：`moto1/moto2` 是**经过侧向极性补偿之后**的全局量，
    //   而固件里"哪一侧翻向"的注释与宏值自相矛盾（`app_config.h:138-151` 注释说唯一解是
    //   (1,0)、宏却是 (1,1)）⇒ 该组合**可能是正常现象**，不能当故障报红。
    m_ledTurnDiff->setLevel(loopOk && l.differentialWithoutTurn() ? LedIndicator::Level::Warn
                                                                 : LedIndicator::Level::Off,
                            loopOk && l.differentialWithoutTurn()
                                ? QStringLiteral("Turn=0 而两轮命令不同。先核对固件极性开关"
                                                 "（CFG_MOTOR_*_INVERT）再判断是否为差速 bug")
                                : QString());
    // 倾角保护：现场把车手动倾过去也会触发 ⇒ **不是故障**，用琥珀而不是红。
    // Zhongzhi（平衡目标角）的当前值从参数账本取；账本没有时用 0.5（本机定稿值）
    const double zhongzhi = (m_params != nullptr) ? m_params->current(0x05) : 0.5;
    const bool tiltTrig = st.received(proto::up::kPose) &&
                          domain::tiltProtectActive(st.pose().pitchDeg, zhongzhi);
    m_ledTiltProt->setLevel(tiltTrig ? LedIndicator::Level::Warn : LedIndicator::Level::Off,
                            tiltTrig ? QStringLiteral("|倾角−%1°| > %2°：固件会关电机（现场常见）")
                                           .arg(domain::units::number(zhongzhi, 1))
                                           .arg(domain::kTiltProtectDeg)
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
