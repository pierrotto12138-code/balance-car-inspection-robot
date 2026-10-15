#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

#include "codec.h" // 事件码常量（自检里识别 0x13）
#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"
#include "ui/mainwindow.h"
#include "ui/theme/theme.h"

// ============================================================================
//  程序入口：只做"装配"这一件事
//     LinkManager（io 层）→ VehicleState（domain 层）→ MainWindow（ui 层）
//  依赖单向：io 不知道 domain，domain 不知道界面，界面不解析字节。
//
//  无界面自检（便于无人值守/CI 验证整机可跑，也便于把证据贴进文档）：
//      host.exe --selftest [秒数] -platform offscreen
//    两条链路都切假源 → 打开 → 跑 N 秒 → 打印链路统计 **与 domain 聚合出的状态**
//    → 退出码 0 = 收到帧、无丢帧、无格式错帧、且姿态/环境/诊断三类数据都解析成功。
//
//  演示模式（板子不在手边 / 现场硬件故障时的应急预案）：
//      host.exe --demo
// ============================================================================
namespace {

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
        add(QStringLiteral("  运动：左 %1 mm/s / 右 %2 mm/s / PWM %3% / 超声 %4 cm / 避障位 0x%5")
                .arg(m.leftMmS)
                .arg(m.rightMmS)
                .arg(domain::units::number(m.pwmPercent, 1))
                .arg(m.distanceCm)
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
        add(QStringLiteral("  环境：MQ-2 %1 ppm / MQ-135 %2 ppm / %3 ℃ / %4 %RH · 档位 %5")
                .arg(domain::units::number(e.mq2Ppm, 1), domain::units::number(e.mq135Ppm, 1),
                     domain::units::number(e.tempC, 1), domain::units::number(e.humidityPct, 1),
                     QString::fromUtf8(domain::sampleGearName(e.gear))));
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
    add(QStringLiteral("  质量：格式错帧 %1 · 协议外命令 %2（都应为 0）")
            .arg(st.malformedFrames())
            .arg(st.unknownFrames()));
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

int runSelfTest(double seconds)
{
    io::LinkManager mgr;
    mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
    mgr.setMode(io::LinkKind::Bluetooth, io::LinkManager::Mode::Replay);
    mgr.setReplayRate(1.0);

    // 与真实运行**同一条路径**：链路 → VehicleState（domain）→ 界面读它
    domain::VehicleState state;
    QObject::connect(&mgr, &io::LinkManager::frameReady, &state, &domain::VehicleState::onFrame);

    QTextStream out(stdout);
    out << QStringLiteral("自检中：假源 ×1.0，跑 %1 s（协议周期 0x10/0x11 20 Hz…）\n")
               .arg(seconds, 0, 'f', 1);
    out.flush();

    mgr.openAll();

    QEventLoop loop;
    QTimer::singleShot(static_cast<int>(seconds * 1000.0), &loop, &QEventLoop::quit);
    loop.exec();

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
    parser.process(app);

    if (parser.isSet(selftest)) {
        return runSelfTest(parser.value(selftest).toDouble());
    }

    io::LinkManager manager;
    domain::VehicleState state;
    // io → domain：跨线程用队列连接传值类型（解析在工作线程完成，见 io/serial_link.h）
    QObject::connect(&manager, &io::LinkManager::frameReady, &state, &domain::VehicleState::onFrame);

    if (parser.isSet(demo)) {
        manager.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        manager.setMode(io::LinkKind::Bluetooth, io::LinkManager::Mode::Replay);
    }

    MainWindow w(&manager, &state);
    w.show();
    if (parser.isSet(demo)) {
        manager.openAll(); // 窗口显示后再开链路，界面上能直接看到状态变化
    }
    return app.exec();
}
