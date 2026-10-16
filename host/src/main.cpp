#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QTextStream>
#include <QTimer>

#include "codec.h" // 事件码常量（自检里识别 0x13）
#include "data/event_log.h"
#include "data/param_store.h"
#include "data/recorder.h"
#include "domain/alarm.h"
#include "domain/commands.h"
#include "domain/keepalive.h"
#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"
#include "ui/mainwindow.h"
#include "ui/theme/theme.h"

#include <QStandardPaths>

// ============================================================================
//  程序入口：只做"装配"这一件事
//    LinkManager（io）→ VehicleState / AlarmEngine（domain）→ EventLog（data）→ MainWindow（ui）
//  依赖单向：io 不知道 domain，domain 不知道界面，界面不解析字节。
//
//  无界面自检（便于无人值守/CI 验证整机可跑，也便于把证据贴进文档）：
//      host.exe --selftest [秒数] [--rate N] -platform offscreen
//    两条链路都切假源 → 打开 → 跑 N 秒 → 打印链路统计、**状态聚合**、**告警与事件流**
//    → 退出码 0 = 收到帧、无丢帧、无格式错帧、必到帧全解析、且没有意外告警。
//
//  演示模式（板子不在手边 / 现场硬件故障时的应急预案）：
//      host.exe --demo [--rate 5]     # ×5 时约 12 s 就能看到"气体冲高→急停锁存→解除→低压"
// ============================================================================
namespace {

// 前向声明：describeState 定义在下面（自检与探测都要用它打印状态）
QString describeState(const domain::VehicleState &st, const io::LinkManager &mgr);

/// 把告警与事件流打成一段可读文本（自检输出，也是"P2 是否真的在工作"的证据）
QString describeAlarms(const domain::VehicleState &st, const domain::AlarmEngine &alarm,
                       const data::EventLog &log)
{
    QString s;
    auto add = [&s](const QString &line) { s += line + QLatin1Char('\n'); };

    add(QStringLiteral("  活动告警：%1 项%2")
            .arg(alarm.activeCount())
            .arg(alarm.anyActive() ? (QStringLiteral(" —— ") + alarm.activeSummary()) : QString()));
    if (alarm.activeCount() == 0 && !alarm.history().isEmpty()) {
        QStringList cleared;
        for (const domain::AlarmRecord &r : alarm.history()) {
            QString extra;
            if (r.unit == QStringLiteral("ms")) {
                extra = QStringLiteral("最大间隔 %1 s").arg(r.peak / 1000.0, 0, 'f', 1);
            } else if (!r.unit.isEmpty()) {
                const QString extreme = (r.key == domain::AlarmEngine::kBatteryLow)
                                            ? QStringLiteral("最低")
                                            : QStringLiteral("峰值");
                extra = QStringLiteral("%1 %2 %3")
                            .arg(extreme,
                                 domain::units::number(r.peak,
                                                       r.unit == QStringLiteral("V") ? 2 : 1),
                                 r.unit);
            }
            cleared << QStringLiteral("%1（%2%3）")
                           .arg(r.title, r.active ? QStringLiteral("仍活动") : QStringLiteral("已解除"),
                                extra.isEmpty() ? QString() : (QStringLiteral("，") + extra));
        }
        add(QStringLiteral("  告警记录：%1").arg(cleared.join(QStringLiteral("；"))));
    }
    add(QStringLiteral("  安全状态：%1").arg(st.safetySummary()));

    add(QStringLiteral("  事件流：%1 条").arg(log.size()));
    const int show = qMin(log.size(), 8);
    for (int i = 0; i < show; ++i) {
        const data::EventEntry &e = log.at(i);
        add(QStringLiteral("    T+%1 s  %2  %3%4")
                .arg(e.localMs / 1000.0, 0, 'f', 1)
                .arg(e.local ? QStringLiteral("本机") : QStringLiteral("机器人"))
                .arg(e.title)
                .arg(e.detail.isEmpty() ? QString() : (QStringLiteral(" —— ") + e.detail)));
    }
    if (log.size() > show) {
        add(QStringLiteral("    …（共 %1 条，界面上可看全部）").arg(log.size()));
    }
    const qint64 shortest = log.shortestSafetyStopMs();
    add(QStringLiteral("  最短安全停车时长：%1")
            .arg(shortest < 0 ? QStringLiteral("（本次未出现\"停车→解除\"配对）")
                              : QStringLiteral("%1 s").arg(shortest / 1000.0, 0, 'f', 1)));
    return s;
}

int runSelfTest(double seconds, double rate)
{
    io::LinkManager mgr;
    mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
    mgr.setMode(io::LinkKind::Bluetooth, io::LinkManager::Mode::Replay);
    mgr.setReplayRate(rate);

    // 与真实运行**同一条路径**：链路 → VehicleState/AlarmEngine（domain）→ EventLog（data）
    domain::VehicleState state;
    domain::AlarmEngine alarm;
    data::EventLog log;
    QObject::connect(&mgr, &io::LinkManager::frameReady, &state, &domain::VehicleState::onFrame);
    QObject::connect(&state, &domain::VehicleState::eventReceived, &log, &data::EventLog::onEvent);
    QObject::connect(&state, &domain::VehicleState::dataUpdated, &alarm,
                     [&state, &alarm](quint8 cmd) { alarm.onFrameDecoded(state, cmd); });
    QObject::connect(&alarm, &domain::AlarmEngine::alarmRaised, &log, &data::EventLog::onAlarmRaised);
    QObject::connect(&alarm, &domain::AlarmEngine::alarmCleared, &log, &data::EventLog::onAlarmCleared);

    // 告警里"随时间变化"的规则（失联、锁存）用固定节拍评估，而不是每帧评估
    QTimer alarmTick;
    alarmTick.setInterval(100);
    QObject::connect(&alarmTick, &QTimer::timeout, &alarm, [&state, &alarm]() { alarm.tick(state); });
    alarmTick.start();

    // 保活（P3）：**默认心跳 0x1F**（不解除锁存）；真实运行里由界面上的开关控制，
    // 自检里开着它，顺便验证"下行链路真的能发出去"（假源会把字节数记进 bytesTx）
    domain::KeepaliveEngine keepalive;
    keepalive.setSink([&mgr](quint8 cmd, const QByteArray &data) { return mgr.send(cmd, data); });
    QObject::connect(&mgr, &io::LinkManager::linkStateChanged, &keepalive, [&mgr, &keepalive]() {
        keepalive.setLinkReady(mgr.isOpen(io::LinkKind::Wire) || mgr.isOpen(io::LinkKind::Bluetooth));
    });

    QTextStream out(stdout);
    out << QStringLiteral("自检中：假源 ×%1，跑 %2 s（协议周期 0x10/0x11 20 Hz…）\n")
               .arg(rate, 0, 'f', 1)
               .arg(seconds, 0, 'f', 1);
    out.flush();

    mgr.openAll();
    keepalive.setEnabled(true); // 打开时链路已就绪 → 立刻发第一条，不等周期

    QEventLoop loop;
    QTimer::singleShot(static_cast<int>(seconds * 1000.0), &loop, &QEventLoop::quit);
    loop.exec();

    alarmTick.stop();
    alarm.tick(state); // 收尾再判一次（把"最后一帧之后"的状态也反映出来）
    mgr.closeAll();

    bool ok = true;
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        const auto kind = static_cast<io::LinkKind>(i);
        const io::LinkStats s = mgr.stats(kind);
        out << QStringLiteral("  %1：收 %2 B / 发 %3 B · 解帧 %4 · 丢帧 %5 · 重连 %6\n")
                   .arg(QString::fromUtf8(io::linkName(kind)))
                   .arg(s.bytesRx)
                   .arg(s.bytesTx)
                   .arg(s.framesOk)
                   .arg(s.framesDropped)
                   .arg(s.reconnects);
        if (s.framesOk == 0 || s.framesDropped != 0) {
            ok = false;
        }
    }

    out << QStringLiteral("\n【domain 状态聚合】假源 → 解帧 → 领域层（界面上看到的就是这些值）\n");
    out << describeState(state, mgr);
    out << QStringLiteral("\n【告警判定与事件流】domain/alarm + data/event_log\n");
    out << describeAlarms(state, alarm, log);
    out << QStringLiteral("\n【保活（P3）】下行链路是否真的在周期性发合法帧\n")
        << QStringLiteral("  方式：%1 · 周期 %2 ms · 已发 %3 条 · 最近一条 %4 ms 前\n")
               .arg(keepalive.mode() == domain::KeepaliveMode::Heartbeat
                        ? QStringLiteral("心跳 0x1F（不解除锁存）")
                        : QStringLiteral("使能 0x20（会解除锁存！）"))
               .arg(keepalive.intervalMs())
               .arg(keepalive.sentCount())
               .arg(keepalive.sinceLastSendMs() < 0 ? QStringLiteral("--")
                                                    : QString::number(keepalive.sinceLastSendMs()));
    out.flush();

    // 界面能不能看到数据，取决于这几条：必到的帧必须都解析成功，且没有格式错帧
    const bool dataOk = state.received(proto::up::kPose) && state.received(proto::up::kMotion) &&
                        state.received(proto::up::kTask) && state.received(proto::up::kEnvGas) &&
                        state.received(proto::up::kDiag) && state.received(proto::up::kLoop);
    if (!dataOk) {
        out << QStringLiteral("\n✘ 有命令字始终没有解析成功 —— 检查假源与 domain 的长度口径\n");
        ok = false;
    }
    if (state.malformedFrames() != 0) {
        out << QStringLiteral("\n✘ 出现格式错帧 %1 条 —— 说明长度口径与协议不一致\n")
                   .arg(state.malformedFrames());
        ok = false;
    }
    out << (ok ? QStringLiteral("自检通过 ✅\n") : QStringLiteral("自检失败 ❌\n"));
    out.flush();
    return ok ? 0 : 1;
}

/// 把 domain 层聚合出的状态打成一段可读文本（自检输出，也是"数据到没到界面"的证据）
QString describeState(const domain::VehicleState &st, const io::LinkManager &mgr)
{
    QString s;
    auto add = [&s](const QString &line) { s += line + QLatin1Char('\n'); };

    auto srcText = [&st](quint8 cmd) {
        const qint64 age = st.ageMs(cmd);
        if (age < 0) {
            return QStringLiteral("无数据");
        }
        return QStringLiteral("%1 · %2 ms")
            .arg(QString::fromUtf8(io::linkName(st.sourceOf(cmd))))
            .arg(age);
    };

    if (st.received(proto::up::kPose)) {
        const auto &p = st.pose();
        add(QStringLiteral("  姿态：俯仰 %1° / 横滚 %2° / 偏航 %3°  （来源 %4）")
                .arg(domain::units::number(p.pitchDeg, 2), domain::units::number(p.rollDeg, 2),
                     domain::units::number(p.yawDeg, 2), srcText(proto::up::kPose)));
    } else {
        add(QStringLiteral("  姿态：**无数据**（0x10 没到 —— 见上面链路统计）"));
    }

    if (st.received(proto::up::kMotion)) {
        const auto &m = st.motion();
        add(QStringLiteral("  运动：左 %1 mm/s / 右 %2 mm/s / PWM 占空(估) %3 %（带符号；"
                           "原始值 %4 ÷ 7179） / 超声 %5 cm / 避障位 0x%6")
                .arg(m.leftMmS)
                .arg(m.rightMmS)
                .arg(domain::units::number(domain::units::motorDutyPercent(m.motorOutAvg), 1))
                .arg(m.motorOutAvg)
                .arg(domain::units::number(domain::units::cmFromMm(m.distanceMm), 1))
                .arg(m.avoidFlags, 2, 16, QLatin1Char('0')));
    } else {
        add(QStringLiteral("  运动：**无数据**"));
    }

    if (st.batteryValid()) {
        add(QStringLiteral("  电源：%1 V（%2）  安全：%3")
                .arg(domain::units::number(st.diag().batteryV, 2),
                     st.batteryLow() ? QStringLiteral("低于报警线")
                                     : QStringLiteral("正常，报警线 11.10 V"),
                     st.safetySummary()));
    } else {
        add(QStringLiteral("  电源：**无数据**（只有 0x15 诊断帧带电压）  安全：%1")
                .arg(st.safetySummary()));
    }

    if (st.received(proto::up::kTask)) {
        const auto &t = st.task();
        add(QStringLiteral("  任务：%1 · 阶段 %2 · 完成度 %3% · 巡检点 %4 · 异常 %5")
                .arg(QString::fromUtf8(domain::taskName(t.taskId)),
                     QString::fromUtf8(domain::phaseName(t.phase)))
                .arg(t.progressPct)
                .arg(t.checkpoint)
                .arg(domain::faultFlagsText(t.faultFlags)));
    } else {
        add(QStringLiteral("  任务：**无数据**"));
    }

    if (st.received(proto::up::kEnvGas)) {
        const auto &e = st.env();
        add(QStringLiteral("  环境：MQ-2 %1 ppm（ADC 原始值 %2）/ MQ-135 %3 ppm（原始值 %4）"
                           "/ %5 ℃ / %6 %RH · 档位 %7")
                .arg(domain::units::number(e.mq2Ppm, 1))
                .arg(e.mq2Raw)
                .arg(domain::units::number(e.mq135Ppm, 1))
                .arg(e.mq135Raw)
                .arg(domain::units::number(e.tempC, 1), domain::units::number(e.humidityPct, 1),
                     QString::fromUtf8(domain::sampleGearName(e.gear))));
        add(QStringLiteral("    （★ 原始 ADC 是判断“传感器有没有响应”的铁证：ppm 是换算来的，"
                           "原始值不动就说明那一路根本没在测 —— 与 OLED 第 2/3 行同源）"));
        add(QStringLiteral("  就绪：MQ-2 %1 · MQ-135 %2 · DHT11 %3 · 补偿 %4")
                .arg(e.mq2Ready() ? QStringLiteral("✔") : QStringLiteral("✘"),
                     e.mq135Ready() ? QStringLiteral("✔") : QStringLiteral("✘"),
                     e.dht11Valid() ? QStringLiteral("✔") : QStringLiteral("✘"),
                     e.compensationOn() ? QStringLiteral("✔") : QStringLiteral("✘")));
    } else {
        add(QStringLiteral("  环境：**无数据**"));
    }

    if (st.received(proto::up::kDiag)) {
        add(QStringLiteral("  诊断：剩余堆 %1 KB · OLED 写速率 %2 B/s · 页 %3 · Flag_Show %4")
                .arg(st.diag().heapKb)
                .arg(domain::units::number(st.oledBytesPerSec(), 0),
                     QString::fromUtf8(domain::diagPageName(st.diag().page)))
                .arg(st.diag().flagShow ? QStringLiteral("1") : QStringLiteral("0")));
    } else {
        add(QStringLiteral("  诊断：**无数据**"));
    }

    if (st.received(proto::up::kLoop)) {
        const auto &l = st.loop();
        add(QStringLiteral("  三环：Bal %1 / Vel %2 / Turn %3 · 左 Moto %4→CCR %5 · "
                           "右 Moto %6→CCR %7 · 固件 %8")
                .arg(l.balancePwm)
                .arg(l.velocityPwm)
                .arg(l.turnPwm)
                .arg(l.moto1)
                .arg(l.ccrA)
                .arg(l.moto2)
                .arg(l.ccrB)
                .arg(domain::firmwareVersionText(l.fwVersion)));
    } else {
        add(QStringLiteral("  三环：**无数据**（0x16 未上报）"));
    }

    add(QStringLiteral("  链路：有线 帧 %1 · 最近 %2 ｜ 蓝牙 帧 %3 · 最近 %4")
            .arg(st.frameCount(io::LinkKind::Wire))
            .arg(st.linkAgeMs(io::LinkKind::Wire) < 0
                     ? QStringLiteral("--")
                     : QStringLiteral("%1 ms").arg(st.linkAgeMs(io::LinkKind::Wire)))
            .arg(st.frameCount(io::LinkKind::Bluetooth))
            .arg(st.linkAgeMs(io::LinkKind::Bluetooth) < 0
                     ? QStringLiteral("--")
                     : QStringLiteral("%1 ms").arg(st.linkAgeMs(io::LinkKind::Bluetooth))));
    add(QStringLiteral("  主链路：%1")
            .arg(mgr.primaryLink() == io::LinkKind::Count
                     ? QStringLiteral("无")
                     : QString::fromUtf8(io::linkName(mgr.primaryLink()))));
    add(QStringLiteral("  质量：格式错帧 %1 · 协议外命令 %2 · 重复帧 %3（两条链路都发，已去重）"
                       "（前两项都应为 0）")
            .arg(st.malformedFrames())
            .arg(st.unknownFrames())
            .arg(st.duplicateFrames()));
    if (st.lastEvent().timestampMs != 0 || st.received(proto::up::kEvent)) {
        add(QStringLiteral("  最近事件：%1（机器人时间戳 %2 ms）")
                .arg(QString::fromUtf8(domain::eventName(st.lastEvent().code)))
                .arg(st.lastEvent().timestampMs));
    }
    if (!st.lastSafetyEventText().isEmpty()) {
        add(QStringLiteral("  最近安全事件：%1").arg(st.lastSafetyEventText()));
    }
    return s;
}

/// 真机只读探测：打开真实串口，把收到的帧/解出的状态打印出来。
///
/// 为什么需要它（而不是"打开界面自己看"）：
///   ① 联调第一现场永远是"端口打开了吗、帧在不在涨、**哪些命令字真的在发**" ——
///      命令行版能把这些数一次打全，也便于贴进联调记录；
///   ② 它**默认不发任何下行帧**，所以是一个"只监听"的工具。
///      ⚠️ 但固件的失联保护是"500 ms 没收到任何下行帧 → 安全停车"（协议 §五），
///      所以纯监听会让正在跑的车停下来 —— 要边跑边看请加 `--probe-heartbeat`，
///      它每 200 ms 发一条 0x1F 心跳（协议 §七 约定 7 推荐的做法），车就不会停。
int runProbe(const QString &port, int baud, double seconds, bool heartbeat, bool readback)
{
    io::LinkManager mgr;
    mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Serial);
    io::SerialConfig cfg = mgr.serialConfig(io::LinkKind::Wire);
    cfg.portName = port;
    cfg.baudRate = baud;
    cfg.autoReconnect = false; // 探测工具不需要自动重连：连不上就要立刻知道
    mgr.setSerialConfig(io::LinkKind::Wire, cfg);

    domain::VehicleState state;
    QObject::connect(&mgr, &io::LinkManager::frameReady, &state, &domain::VehicleState::onFrame);

    QHash<quint8, quint64> perCmd;
    QObject::connect(&mgr, &io::LinkManager::frameReady,
                     [&perCmd](io::LinkKind, quint8 cmd, const QByteArray &) { ++perCmd[cmd]; });

    // 0x24 回显（"实际生效值"）：全量回读的结果就靠它收集
    QHash<quint8, float> echoes;
    QObject::connect(&mgr, &io::LinkManager::frameReady, &mgr,
                     [&echoes](io::LinkKind, quint8 cmd, const QByteArray &d) {
                         if (cmd != proto::down::kPidTune) {
                             return;
                         }
                         domain::PidTuneData e;
                         if (domain::decodePidTune(d, e)) {
                             echoes.insert(e.paramId, e.value);
                         }
                     });

    QTextStream out(stdout);
    // 链路的每一次状态变化都要打出来 —— "连不上"和"连上了但没数据"是两回事，
    // 而端口被占用（FlyMcu/别的工具开着）是联调时最常见的一种，必须当场看得见。
    QObject::connect(&mgr, &io::LinkManager::linkStateChanged, &mgr,
                     [&out](io::LinkKind k, io::LinkState st, const QString &detail) {
                         out << QStringLiteral("  [链路] %1：%2 %3\n")
                                    .arg(QString::fromUtf8(io::linkName(k)),
                                         QString::fromUtf8(io::stateName(st)), detail);
                         out.flush();
                     });

    out << QStringLiteral("只读探测 %1 @ %2 8N1，%3 s%4%5\n")
               .arg(port)
               .arg(baud)
               .arg(seconds, 0, 'f', 1)
               .arg(heartbeat || readback
                        ? QStringLiteral("（每 200 ms 发 0x1F 心跳，车不会因失联停车）")
                        : QStringLiteral("（**不发任何下行帧** —— 固件 500 ms 后会安全停车）"))
               .arg(readback ? QStringLiteral("；先发一条 0x24 全量回读(0xFF)：**只读命令**，"
                                              "不写参数、不动 Flash")
                             : QString());
    if (!heartbeat && !readback) {
        out << QStringLiteral("提示：若车正在跑，请改用 --probe-heartbeat，避免把它“探”停。\n");
    }
    out.flush();

    mgr.open(io::LinkKind::Wire);

    QEventLoop loop;
    QTimer hb;
    if (heartbeat || readback) {
        // 回读也要打心跳：否则"问完一句就走"之后 500 ms 车就进了失联锁存（下一个人接手时会莫名）
        hb.setInterval(200);
        QObject::connect(&hb, &QTimer::timeout, [&mgr]() {
            mgr.sendTo(io::LinkKind::Wire, proto::up::kHeartbeat, QByteArray());
        });
        hb.start();
    }
    if (readback) {
        // 等链路稳一点再发（刚打开就发容易与复位/清缓冲撞在一起）
        QTimer::singleShot(300, &mgr, [&mgr, &out]() {
            QByteArray payload;
            if (domain::cmd::pidTunePayload(0xFF, 0.0f, &payload)) {
                const qint64 n = mgr.sendTo(io::LinkKind::Wire, proto::down::kPidTune, payload);
                out << QStringLiteral("  [下行] 0x24 全量回读(0xFF) → 写出 %1 B\n").arg(n);
                out.flush();
            }
        });
    }
    QElapsedTimer wall;
    wall.start();
    QTimer::singleShot(static_cast<int>(seconds * 1000.0), &loop, &QEventLoop::quit);
    loop.exec();
    hb.stop();

    // ★ 在**关闭链路之前**先记两个数：
    //     · 实测时长 —— 定时器有没有按时结束；
    //     · 结束时"最近帧年龄" —— 数据流是一直在来，还是中途停了。
    //   这两个数把"没数据"分成两种完全不同的故障：流停了（固件/线的问题）
    //   还是收尾慢（我们自己的问题，例如关闭串口时阻塞）。
    //   2026-10-15 打真机时正是靠"年龄恒为 ~3000 ms"发现了关闭链路会阻塞的坑。
    const qint64 elapsedAtStop = wall.elapsed();
    const qint64 ageAtStop = state.anyFrameAgeMs();

    const io::LinkStats s = mgr.stats(io::LinkKind::Wire);
    const qint64 closeBegin = wall.elapsed();
    mgr.close(io::LinkKind::Wire);
    const qint64 closeCost = wall.elapsed() - closeBegin;

    out << QStringLiteral("\n探测时长：设定 %1 s · 实测 %2 ms ｜ 结束时最近帧年龄 %3 ms")
               .arg(seconds, 0, 'f', 1)
               .arg(elapsedAtStop)
               .arg(ageAtStop < 0 ? QStringLiteral("无（一帧都没来）") : QString::number(ageAtStop))
        << QLatin1Char('\n');
    out << QStringLiteral("关闭链路耗时：%1 ms%2\n")
               .arg(closeCost)
               .arg(closeCost > 500 ? QStringLiteral("（偏大！关闭串口不应阻塞这么久）") : QString())
        << QStringLiteral("链路：收 %1 B / 发 %2 B · 解帧 %3 · 丢帧 %4\n")
               .arg(s.bytesRx)
               .arg(s.bytesTx)
               .arg(s.framesOk)
               .arg(s.framesDropped);
    QString cover;
    for (quint8 cmd : {proto::up::kPose, proto::up::kMotion, proto::up::kTask, proto::up::kEvent,
                       proto::up::kEnvGas, proto::up::kDiag, proto::up::kLoop,
                       proto::up::kHeartbeat}) {
        cover += QStringLiteral("  0x%1: %2 条\n")
                     .arg(cmd, 2, 16, QLatin1Char('0'))
                     .arg(perCmd.value(cmd, 0));
    }
    out << QStringLiteral("各命令字实际收到条数（缺哪个 = 固件没发那个命令）：\n") << cover;
    out << QStringLiteral("\n【状态聚合】\n") << describeState(state, mgr);

    // ---- 0x24 全量回读：把"实际生效值"与参数定义表逐项对照 ----
    if (readback) {
        out << QStringLiteral("\n【0x24 全量回读】请求 ID=0xFF → 收到 %1 条回显\n").arg(echoes.size());
        // B 侧接口文档 §3.1 给出的本机定稿值（用于对照，不用于判对错）
        static const double kFactory[7] = {300.0, 1.0, -80.0, -0.4, 0.5, 0.0, 0.0};
        for (const data::PidParamInfo &p : data::pidParams()) {
            const bool have = echoes.contains(p.id);
            const double v = have ? double(echoes.value(p.id)) : 0.0;
            QString verdict;
            if (!have) {
                verdict = QStringLiteral("✘ 没回（固件未实现回读？长度不符？）");
            } else {
                QString why;
                const data::ParamCheck chk = data::checkPidValue(p.id, v, &why);
                const bool sameAsFactory = std::fabs(v - kFactory[p.id - 1]) < 1e-4;
                verdict = (chk == data::ParamCheck::Ok)
                              ? (sameAsFactory ? QStringLiteral("✔ 范围内，且等于 B 侧定稿值")
                                               : QStringLiteral("✔ 范围内（与 B 侧定稿值不同：%1）")
                                                     .arg(kFactory[p.id - 1]))
                              : QStringLiteral("⚠ %1").arg(why);
            }
            out << QStringLiteral("  0x%1 %2（%3）实际 %4  范围 %5~%6  %7\n")
                       .arg(p.id, 2, 16, QLatin1Char('0'))
                       .toUpper()
                       .replace(QStringLiteral("0X"), QStringLiteral("0x"))
                       .arg(QString::fromUtf8(p.name), QString::fromUtf8(p.baseVar))
                       .arg(have ? QString::number(v, 'f', 4) : QStringLiteral("--"))
                       .arg(p.lo)
                       .arg(p.hi)
                       .arg(verdict);
        }
        // ---- 0x16 的三个"一眼定位故障"组合（B 侧接口文档 §2.1 / §7 的现场判据）----
        if (state.received(proto::up::kLoop)) {
            const domain::LoopData &l = state.loop();
            const double ratio = (l.moto1 != 0) ? (double(l.ccrA) / std::abs(double(l.moto1))) : 0.0;
            out << QStringLiteral("\n【0x16 判读（B 侧口径）】\n")
                << QStringLiteral("  固件版本：%1/%2\n")
                       .arg(domain::hexByte(l.fwVersion), domain::firmwareVersionText(l.fwVersion))
                << QStringLiteral("  Turn=%1 时两轮命令 %2（★ 该组合受固件侧向极性开关影响，"
                                  "不同不一定是 bug）\n")
                       .arg(l.turnPwm)
                       .arg(l.moto1 == l.moto2 ? QStringLiteral("相同")
                                               : QStringLiteral("不同 ⚠"))
                << QStringLiteral("  Moto1=%1 CCR_A=%2 ⇒ CCR/|Moto| = %3（期望 ≈1.17）· %4\n")
                       .arg(l.moto1)
                       .arg(l.ccrA)
                       .arg(ratio, 0, 'f', 3)
                       .arg(l.executionState() == domain::LoopData::Execution::Mismatch
                                ? QStringLiteral("⚠ 算了没执行（未停车却 CCR=0）")
                            : l.executionState() == domain::LoopData::Execution::Protected
                                ? QStringLiteral("保护中，未驱动（Flag_Stop/锁存 ⇒ 正常）")
                                : QStringLiteral("✔ 已执行"))
                << QStringLiteral("  编码器本拍增量：左 %1 / 右 %2（★ 手推前进时应**同号**；"
                                  "静止时都≈0，此判据需在推车时看）\n")
                       .arg(l.encLeft)
                       .arg(l.encRight)
                << QStringLiteral("  状态位：bit0 Flag_Stop=%1 · bit1 电机被禁=%2 · bit2 本拍调 Set_pwm=%3"
                                  " · **bit3 新特性=%4**（0 = 板上是旧构建，请重烧！）\n")
                       .arg(l.flagStop() ? 1 : 0)
                       .arg(l.motorDisabled() ? 1 : 0)
                       .arg(l.setPwmCalled() ? 1 : 0)
                       .arg(l.featureBits() ? 1 : 0);
            // 倾角保护（≥40° 关电机；现场常见，不是故障）
            if (state.received(proto::up::kPose)) {
                const double zz = echoes.contains(0x05) ? double(echoes.value(0x05)) : 0.5;
                const bool tilt = domain::tiltProtectActive(state.pose().pitchDeg, zz);
                out << QStringLiteral("  倾角 %1° − Zhongzhi %2° ⇒ %3\n")
                           .arg(domain::units::number(state.pose().pitchDeg, 2))
                           .arg(domain::units::number(zz, 2))
                           .arg(tilt ? QStringLiteral("⚠ 超 ±40°，固件会关电机（现场常见，非故障）")
                                     : QStringLiteral("✔ 在保护范围外"));
            }
        }
    }
    const bool ok = s.framesOk > 0 && state.malformedFrames() == 0;
    out << (ok ? QStringLiteral("探测通过 ✅（真实串口→解帧→domain 全链路可用）\n")
               : QStringLiteral("探测未通过 ❌（没收到帧，或出现格式错帧）\n"));
    out.flush();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));
    app.setOrganizationName(QStringLiteral("MCS-2026-Group1"));

    // 暗色主题：QSS 变量全部来自 ui/theme/theme.h（颜色只表达异常，见该文件头说明）
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(theme::styleSheet());

    io::registerLinkMetaTypes();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));
    parser.addHelpOption();
    QCommandLineOption selftest(QStringLiteral("selftest"),
                                QStringLiteral("无界面自检：假源跑 N 秒后打印链路统计与状态聚合，"
                                               "退出码 0 = 正常"),
                                QStringLiteral("秒"), QStringLiteral("3"));
    parser.addOption(selftest);
    // 演示模式：两条链路都切假源并自动连接 —— 板子没到 / 演示现场硬件故障时用
    QCommandLineOption demo(QStringLiteral("demo"),
                            QStringLiteral("演示模式：两条链路用假源并自动连接（打开就有数据在跑）"));
    parser.addOption(demo);
    // 自动截图：把界面渲染成 PNG 后退出 —— 文档里的截图用它生成，
    // 免得"手工截图 → 界面改一点 → 图全过期"（配合 --demo 可在无板子时出图）
    QCommandLineOption shot(QStringLiteral("shot"),
                            QStringLiteral("把界面渲染为 PNG 后退出（配合 --demo 出文档截图）"),
                            QStringLiteral("路径"));
    parser.addOption(shot);
    QCommandLineOption shotDelay(QStringLiteral("shot-delay"),
                                 QStringLiteral("截图前等待的毫秒数（让假源先攒一点数据）"),
                                 QStringLiteral("毫秒"), QStringLiteral("2500"));
    parser.addOption(shotDelay);
    QCommandLineOption shotTab(QStringLiteral("shot-tab"),
                               QStringLiteral("截图对应的导航页：0=总览 1=巡检与控制 2=曲线与告警 "
                                              "3=事件流 4=通信链路 5=协议自检"),
                               QStringLiteral("页号"), QStringLiteral("0"));
    parser.addOption(shotTab);
    QCommandLineOption sizeOpt(QStringLiteral("size"),
                               QStringLiteral("强制窗口尺寸 WxH（★ 小屏验证用：默认按屏幕自适应；"
                                              "例如 --size 1366x768 复现笔记本屏）"),
                               QStringLiteral("WxH"));
    parser.addOption(sizeOpt);
    // 假源速率倍数：×5 时约 12 s 就能看到完整的安全剧本（气体冲高→急停锁存→解除→低压）
    QCommandLineOption rate(QStringLiteral("rate"),
                            QStringLiteral("假源速率倍数（1.0 = 按协议真实周期；5 = 五倍速演示）"),
                            QStringLiteral("倍数"), QStringLiteral("1.0"));
    parser.addOption(rate);
    // 记录与回放（P4）：记录让"现场发生的事"可复盘；回放让"没有硬件也能演示"
    QCommandLineOption record(QStringLiteral("record"),
                              QStringLiteral("启动即开始录制（CSV 逐帧 + 会话入库）"),
                              QStringLiteral("CSV路径"));
    parser.addOption(record);
    QCommandLineOption play(QStringLiteral("play"),
                            QStringLiteral("用记录文件回放驱动界面（走假源链路，同一套数据通路）"),
                            QStringLiteral("CSV路径"));
    parser.addOption(play);
    QCommandLineOption dbPath(QStringLiteral("db"),
                              QStringLiteral("会话库(SQLite)路径；默认放在 QStandardPaths 的 "
                                             "AppDataLocation。★ 打包/便携用法：指到 U 盘或记录目录，"
                                             "所有会话与参数快照跟着数据一起走"),
                              QStringLiteral("db路径"));
    parser.addOption(dbPath);
    // 真机只读探测（不需要界面，用来在联调第一现场把"链路/命令覆盖/状态"一次打全）
    QCommandLineOption probe(QStringLiteral("probe"),
                             QStringLiteral("真机只读探测：打开指定串口，打印链路统计、各命令字"
                                            "收到条数与状态聚合后退出"),
                             QStringLiteral("端口"));
    parser.addOption(probe);
    QCommandLineOption probeBaud(QStringLiteral("probe-baud"), QStringLiteral("探测波特率"),
                                 QStringLiteral("波特率"), QStringLiteral("115200"));
    parser.addOption(probeBaud);
    QCommandLineOption probeSeconds(QStringLiteral("probe-seconds"), QStringLiteral("探测时长（秒）"),
                                    QStringLiteral("秒"), QStringLiteral("5"));
    parser.addOption(probeSeconds);
    QCommandLineOption probeHb(QStringLiteral("probe-heartbeat"),
                               QStringLiteral("探测期间每 200 ms 发 0x1F 心跳（车不会因失联停车）"));
    parser.addOption(probeHb);
    QCommandLineOption probeReadback(
        QStringLiteral("probe-readback"),
        QStringLiteral("探测时发一条 0x24 全量回读(0xFF) 并把 7 个实际生效值与定义表逐项对照"
                       "（**只读命令**，不写参数、不动 Flash；会自动开启心跳）"));
    parser.addOption(probeReadback);
    parser.process(app);

    if (parser.isSet(selftest)) {
        return runSelfTest(parser.value(selftest).toDouble(), parser.value(rate).toDouble());
    }
    if (parser.isSet(probe)) {
        return runProbe(parser.value(probe), parser.value(probeBaud).toInt(),
                        parser.value(probeSeconds).toDouble(), parser.isSet(probeHb),
                        parser.isSet(probeReadback));
    }

    // ---------------- 装配（依赖由外向内注入）----------------
    io::LinkManager manager;
    domain::VehicleState state;
    domain::AlarmEngine alarm;
    domain::KeepaliveEngine keepalive;
    data::EventLog log;
    data::ParamStore params;
    data::CsvRecorder recorder;
    data::SessionStore store;

    // 会话库放在标准路径下（不写死绝对路径：打包换机后仍然可用，计划 §2.2 的打包坑）；
    // 也可用 --db 指到别处（便携/把记录放 U 盘时用）
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dbFile = parser.isSet(dbPath) ? parser.value(dbPath)
                                                : appData + QStringLiteral("/sessions.db");
    if (!store.open(dbFile)) {
        // SQLite 不可用不影响主流程（逐帧 CSV 仍可用）——但要在控制台留痕，别静默降级
        QTextStream(stdout) << QStringLiteral("提示：会话库不可用（%1）—— 逐帧 CSV 不受影响\n")
                                   .arg(store.lastError());
    }

    // ① io → domain/data：跨线程用队列连接传值类型（解析在工作线程完成，见 io/serial_link.h）
    QObject::connect(&manager, &io::LinkManager::frameReady, &state, &domain::VehicleState::onFrame);
    QObject::connect(&state, &domain::VehicleState::eventReceived, &log, &data::EventLog::onEvent);
    // ② domain 内部：每解出一帧就让告警引擎按该帧的语义判一次（阈值类规则）
    QObject::connect(&state, &domain::VehicleState::dataUpdated, &alarm,
                     [&state, &alarm](quint8 cmd) { alarm.onFrameDecoded(state, cmd); });
    // ③ domain → data：告警的发生/解除进事件流（与 0x13 事件同一根时间轴）
    QObject::connect(&alarm, &domain::AlarmEngine::alarmRaised, &log, &data::EventLog::onAlarmRaised);
    QObject::connect(&alarm, &domain::AlarmEngine::alarmCleared, &log,
                     &data::EventLog::onAlarmCleared);
    // ④ 记录与落盘（P4）：逐帧 CSV ← frameReady；事件/告警/操作员动作 ← 事件流；参数 ← 账本
    QObject::connect(&manager, &io::LinkManager::frameReady, &recorder, &data::CsvRecorder::onFrame);
    QObject::connect(&log, &data::EventLog::entryAdded, &recorder, &data::CsvRecorder::onEvent);
    QObject::connect(&log, &data::EventLog::entryAdded, &store, [&store](const data::EventEntry &e) {
        if (store.isAvailable()) {
            store.addEvent(e);
        }
    });
    QObject::connect(&params, &data::ParamStore::historyAppended, &store,
                     [&store](const data::ParamSnapshot &s) {
                         if (store.isAvailable()) {
                             store.addParam(QDateTime::currentMSecsSinceEpoch(), s.id, s.value,
                                            s.source);
                         }
                     });
    // ⑤ 随时间变化的告警规则（失联、锁存）用 10 Hz 固定节拍评估
    QTimer alarmTick;
    alarmTick.setInterval(100);
    QObject::connect(&alarmTick, &QTimer::timeout, &alarm, [&state, &alarm]() { alarm.tick(state); });
    alarmTick.start();

    // ⑥ 保活：把"发送"接到真实链路上（默认心跳 0x1F，不解除锁存），
    //    并跟踪"是否有链路可用"——链路没开时它不发（不会假装在保活）
    keepalive.setSink([&manager](quint8 cmd, const QByteArray &data) {
        return manager.send(cmd, data);
    });
    QObject::connect(&manager, &io::LinkManager::linkStateChanged, &keepalive, [&manager, &keepalive]() {
        keepalive.setLinkReady(manager.isOpen(io::LinkKind::Wire) ||
                               manager.isOpen(io::LinkKind::Bluetooth));
    });
    keepalive.setEnabled(true); // 默认开启（协议 §11.5 第 6 条：不保活板子会锁存）

    if (parser.isSet(demo)) {
        manager.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        manager.setMode(io::LinkKind::Bluetooth, io::LinkManager::Mode::Replay);
        manager.setReplayRate(parser.value(rate).toDouble());
    }
    if (parser.isSet(play)) {
        // 用记录文件驱动整个界面（现场硬件故障时的应急预案）
        manager.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        const int n = manager.loadCapture(io::LinkKind::Wire, parser.value(play));
        QTextStream(stdout) << QStringLiteral("回放：%1（%2 帧）\n").arg(parser.value(play)).arg(n);
    }

    MainWindow w(&manager, &state, &alarm, &keepalive, &log, &params, &recorder, &store);
    if (parser.isSet(sizeOpt)) {
        // 小屏验证/演示固定尺寸：`--size 1366x768`
        const QStringList wh = parser.value(sizeOpt).split(QLatin1Char('x'), Qt::SkipEmptyParts);
        if (wh.size() == 2) {
            w.resize(wh.at(0).toInt(), wh.at(1).toInt());
        }
    }
    w.show();
    if (parser.isSet(demo) || parser.isSet(play)) {
        manager.openAll(); // 窗口显示后再开链路，界面上能直接看到状态变化
    }
    if (parser.isSet(record)) {
        if (!recorder.start(parser.value(record))) {
            QTextStream(stdout) << QStringLiteral("✘ 录制未能开始：%1\n").arg(recorder.lastError());
        } else {
            if (store.isAvailable()) {
                store.beginSession(QStringLiteral("命令行 --record 会话"), parser.value(record));
            }
            QTextStream(stdout) << QStringLiteral("⏺ 录制中：%1\n").arg(parser.value(record));
        }
    }
    if (parser.isSet(shot)) {
        w.showPage(parser.value(shotTab).toInt());
        const int delay = parser.value(shotDelay).toInt();
        const QString path = parser.value(shot);
        QTimer::singleShot(delay, &app, [&w, &manager, path]() {
            // 先截图再关链路：关掉链路会让"链路/新鲜度"那几行变成未打开，
            // 图就失去了它要说明的东西。
            const bool ok = w.grab().save(path);
            QTextStream(stdout) << QStringLiteral("截图%1：%2\n")
                                       .arg(ok ? QStringLiteral("已保存") : QStringLiteral("保存失败"), path);
            manager.closeAll();
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
