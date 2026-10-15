// ============================================================================
//  P2 告警判定与事件流测试（不需要硬件、不需要界面）
//
//  覆盖的是"监控系统最容易做成装饰"的部分：
//    ① **连续确认**：单帧毛刺不许报警；
//    ② **只报一次**：同一段异常期间只报一次（状态跃变才报），但**解除后再超限必须重新报**
//       —— 若"再次进入异常不再提示"，安全功能就形同虚设；
//    ③ **迟滞**：回到阈值附近不许反复在"报警/解除"之间跳（事件流会被刷屏）；
//    ④ **失联判据**：必须先有过数据；没连过端口时的"没有帧"不是失联；
//    ⑤ **安全事件成对**：`0x0A` 急停 → `0x09` 解除要能算出**停车时长**（可量化证据）。
//
//  跑法：ctest -R alarm    或    .\test_alarm.exe
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h"
#include "data/event_log.h"
#include "domain/alarm.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"

#include "frame_factory.h"

using namespace domain;

class TestAlarm : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- 气体：连续确认
    void gas_requiresConsecutiveFrames()
    {
        VehicleState st;
        AlarmEngine alarm; // 默认 confirmFrames = 3
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);

        auto feed = [&](double ppm) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(ppm, 5.0));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
        };

        feed(420.0); // 第 1 帧
        QCOMPARE(raised.count(), 0);
        feed(430.0); // 第 2 帧
        QCOMPARE(raised.count(), 0);
        feed(440.0); // 第 3 帧 → 达到连续确认数
        QCOMPARE(raised.count(), 1);
        QVERIFY(alarm.anyActive());
        QVERIFY(alarm.record(AlarmEngine::kGasMq2).active);

        // 继续超限：**不许**重复报（同一段异常只提示一次）
        for (int i = 0; i < 5; ++i) {
            feed(500.0);
        }
        QCOMPARE(raised.count(), 1);
        QVERIFY2(alarm.record(AlarmEngine::kGasMq2).value == 500.0,
                 "活动期间应持续更新当前值（事件流/界面要显示最新值）");
    }

    void gas_singleFrameSpikeIsIgnored()
    {
        VehicleState st;
        AlarmEngine alarm;
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);

        // 一帧冒高、随后正常：确认计数必须清零，不许报
        st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(400.0, 5.0));
        alarm.onFrameDecoded(st, proto::up::kEnvGas);
        st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(10.0, 5.0));
        alarm.onFrameDecoded(st, proto::up::kEnvGas);
        QCOMPARE(raised.count(), 0);
        QVERIFY(!alarm.anyActive());
        QCOMPARE(alarm.record(AlarmEngine::kGasMq2).overCount, 0);
    }

    void gas_hysteresisAndRearm()
    {
        VehicleState st;
        AlarmEngine alarm; // 阈值 30 ppm，回差 0.9 ⇒ 解除线 27 ppm
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);
        QSignalSpy cleared(&alarm, &AlarmEngine::alarmCleared);

        auto feed = [&](double ppm) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(ppm, 5.0));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
        };

        for (int i = 0; i < 3; ++i) {
            feed(35.0);
        }
        QCOMPARE(raised.count(), 1);

        // 迟滞区间（27~30）：既不重复报，也不解除
        for (int i = 0; i < 10; ++i) {
            feed(28.0);
        }
        QCOMPARE(raised.count(), 1);
        QCOMPARE(cleared.count(), 0);
        QVERIFY(alarm.anyActive());

        // 回到回差以下，且要连续确认
        feed(20.0);
        QCOMPARE(cleared.count(), 0);
        feed(20.0);
        QCOMPARE(cleared.count(), 0);
        feed(20.0);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!alarm.anyActive());

        // ★ 解除之后**再次**超限：必须重新报（否则安全功能是装饰）
        for (int i = 0; i < 3; ++i) {
            feed(45.0);
        }
        QCOMPARE(raised.count(), 2);
    }

    void gas_notJudgedWhenSensorNotReady()
    {
        VehicleState st;
        AlarmEngine alarm;
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);
        // 就绪位为 0（未插传感器）：即使数值超限也不判 —— 否则"没插传感器"会被当成报警源
        for (int i = 0; i < 5; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(900.0, 900.0, 0x00));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
        }
        QCOMPARE(raised.count(), 0);
    }

    // ---------------------------------------------------------------- 低压：迟滞
    void battery_lowWithHysteresis()
    {
        VehicleState st;
        AlarmEngine alarm; // 11.10 报警线 / 11.40 解除线
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);
        QSignalSpy cleared(&alarm, &AlarmEngine::alarmCleared);

        auto feed = [&](double v) {
            st.onFrame(io::LinkKind::Wire, proto::up::kDiag, testframes::diagVolt(v));
            alarm.onFrameDecoded(st, proto::up::kDiag);
        };

        feed(11.05);
        feed(11.05);
        QCOMPARE(raised.count(), 0);
        feed(11.05);
        QCOMPARE(raised.count(), 1);
        QCOMPARE(int(alarm.severity()), int(Severity::Warn));

        // 回升到报警线与解除线之间：保持报警（迟滞）
        for (int i = 0; i < 5; ++i) {
            feed(11.25);
        }
        QCOMPARE(cleared.count(), 0);
        QVERIFY(alarm.anyActive());

        for (int i = 0; i < 3; ++i) {
            feed(11.60);
        }
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!alarm.anyActive());
    }

    // ---------------------------------------------------------------- 失联与锁存
    void linkLost_needsPriorDataAndClearsOnFrame()
    {
        VehicleState st;
        AlarmEngine alarm;
        AlarmThresholds th = alarm.thresholds();
        th.linkLostMs = 80; // 测试里把判据缩短，免得等 500 ms
        alarm.setThresholds(th);
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);
        QSignalSpy cleared(&alarm, &AlarmEngine::alarmCleared);

        // 从没收到过任何帧：**不是失联**（端口没打开而已）
        alarm.tick(st);
        QCOMPARE(raised.count(), 0);

        st.onFrame(io::LinkKind::Wire, proto::up::kHeartbeat, QByteArray());
        alarm.tick(st);
        QCOMPARE(raised.count(), 0);

        QTest::qWait(150); // 超过 80 ms 没有新帧
        alarm.tick(st);
        QCOMPARE(raised.count(), 1);
        QVERIFY(alarm.record(AlarmEngine::kLinkLost).active);

        st.onFrame(io::LinkKind::Wire, proto::up::kHeartbeat, QByteArray());
        alarm.tick(st);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!alarm.record(AlarmEngine::kLinkLost).active);
    }

    void safetyLatch_raiseAndRelease()
    {
        VehicleState st;
        AlarmEngine alarm;
        QSignalSpy raised(&alarm, &AlarmEngine::alarmRaised);
        QSignalSpy cleared(&alarm, &AlarmEngine::alarmCleared);

        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::EstopReceived));
        alarm.tick(st);
        QCOMPARE(raised.count(), 1);
        QVERIFY(alarm.record(AlarmEngine::kSafetyLatched).active);
        QCOMPARE(int(alarm.severity()), int(Severity::Alarm));

        alarm.tick(st); // 状态没变 → 不许重复报
        QCOMPARE(raised.count(), 1);

        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::SafetyReleased));
        alarm.tick(st);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!alarm.anyActive());
    }

    void activeSummary_mentionsAllActive()
    {
        VehicleState st;
        AlarmEngine alarm;
        for (int i = 0; i < 3; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(400.0, 600.0));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
        }
        QCOMPARE(alarm.activeCount(), 2); // MQ-2 与 MQ-135 都超
        const QString s = alarm.activeSummary();
        QVERIFY(s.contains(QStringLiteral("2 项报警")));
        QVERIFY(s.contains(QStringLiteral("MQ-2")));
        QCOMPARE(int(alarm.severity()), int(Severity::Alarm));
    }

    // ---------------------------------------------------------------- 事件流
    void eventLog_pairsSafetyStopWithRelease()
    {
        data::EventLog log;
        QSignalSpy added(&log, &data::EventLog::entryAdded);

        log.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                    testframes::event(proto::EventCode::EstopReceived, 1000));
        QCOMPARE(added.count(), 1);
        QVERIFY(log.at(0).safety);
        QCOMPARE(log.size(), 1);

        QTest::qWait(60);
        log.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                    testframes::event(proto::EventCode::SafetyReleased, 5000));
        QCOMPARE(log.size(), 2);
        const data::EventEntry &release = log.at(1);
        QVERIFY2(release.sinceSafetyStopMs >= 50,
                 qPrintable(QStringLiteral("停车时长应 ≥50 ms，实际 %1").arg(release.sinceSafetyStopMs)));
        QVERIFY(release.detail.contains(QStringLiteral("停车时长")));
        QVERIFY(log.shortestSafetyStopMs() >= 50);

        // 非事件帧不该进事件流（否则日志会被周期帧刷满）
        log.onFrame(io::LinkKind::Wire, proto::up::kPose, testframes::pose());
        log.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(1, 1));
        QCOMPARE(log.size(), 2);
    }

    void eventLog_recordsRobotEventsAndLocalAlarms()
    {
        data::EventLog log;
        log.onFrame(io::LinkKind::Bluetooth, proto::up::kEvent,
                    testframes::event(proto::EventCode::GasOverLimit, 777));
        QCOMPARE(log.size(), 1);
        QCOMPARE(log.at(0).title, QString::fromUtf8(domain::eventName(proto::EventCode::GasOverLimit)));
        QCOMPARE(log.at(0).robotTsMs, quint32(777));
        QCOMPARE(int(log.at(0).kind), int(io::LinkKind::Bluetooth));
        QVERIFY(!log.at(0).local);

        AlarmRecord r;
        r.key = AlarmEngine::kGasMq2;
        r.title = QStringLiteral("MQ-2 浓度超阈值");
        r.severity = Severity::Alarm;
        r.unit = QStringLiteral("ppm");
        r.threshold = 30.0;
        r.value = 420.5;
        r.peak = 430.0;
        r.onsetMs = 1000;
        r.clearMs = 4000;
        log.onAlarmRaised(r);
        log.onAlarmCleared(r);
        QCOMPARE(log.size(), 3);
        QVERIFY(log.at(1).local && log.at(1).raised);
        QVERIFY(log.at(1).title.contains(QStringLiteral("告警")));
        QVERIFY(log.at(1).detail.contains(QStringLiteral("420.5")));
        QVERIFY(log.at(2).detail.contains(QStringLiteral("3.0 s"))); // 持续 3 s

        log.clear();
        QCOMPARE(log.size(), 0);
        QCOMPARE(log.shortestSafetyStopMs(), qint64(-1));
    }

    void alarm_worksOffTheFakeSource()
    {
        // 端到端：假源 ×20 跑约 60 s（虚拟时间）应覆盖"气体冲高→急停锁存→解除→低压"
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        mgr.setReplayRate(20.0);

        VehicleState st;
        AlarmEngine alarm;
        data::EventLog log;
        QObject::connect(&mgr, &io::LinkManager::frameReady, &st, &VehicleState::onFrame);
        QObject::connect(&mgr, &io::LinkManager::frameReady, &log, &data::EventLog::onFrame);
        QObject::connect(&st, &VehicleState::dataUpdated, &alarm,
                         [&st, &alarm](quint8 cmd) { alarm.onFrameDecoded(st, cmd); });
        QObject::connect(&alarm, &AlarmEngine::alarmRaised, &log, &data::EventLog::onAlarmRaised);
        QObject::connect(&alarm, &AlarmEngine::alarmCleared, &log, &data::EventLog::onAlarmCleared);

        mgr.open(io::LinkKind::Wire);
        for (int i = 0; i < 320; ++i) { // 3.2 s 实时 ≈ 64 s 虚拟
            QTest::qWait(10);
            alarm.tick(st);
        }
        mgr.close(io::LinkKind::Wire);

        QVERIFY2(log.size() >= 4, qPrintable(QStringLiteral("事件流应有机器人事件+告警，实际 %1 条")
                                                 .arg(log.size())));
        QVERIFY2(log.shortestSafetyStopMs() > 0, "假源剧本里应有一次\"急停→解除\"配对");
        bool sawGasAlarm = false;
        for (const AlarmRecord &r : alarm.history()) {
            if (r.key == AlarmEngine::kGasMq2 && r.onsetMs >= 0) {
                sawGasAlarm = true;
            }
        }
        QVERIFY2(sawGasAlarm, "假源每 30 s 的气体冲高应触发 MQ-2 告警");
        QCOMPARE(st.malformedFrames(), quint64(0));
    }
};

QTEST_MAIN(TestAlarm)
#include "test_alarm.moc"
