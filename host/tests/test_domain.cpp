// ============================================================================
//  P1 domain 层测试（不需要板子、不需要界面）
//
//  覆盖四类，每类都针对一个"看现象看不出来"的错误：
//    ① 单位口径：ppm×10 / 0.01° / 0.01 V / 千分比 —— 少除一个 10 谁也不会怀疑
//    ② 帧解析：**用冻结向量**（docs/protocol/test_vectors.json 里的 data 段）逐字段比对，
//       不是自己造一份自己的期望值（那样只能证明"自己和自己一致"）
//    ③ 长度防御：长度不符时**必须**判为无效并计数，绝不能按偏移硬读
//    ④ 状态聚合与安全：双链路合并 / 来源与新鲜度 / 低压 / 锁存与解除；
//       并用假源做一次端到端（假源 → 解帧 → domain，格式错帧必须为 0）
//
//  跑法：ctest -R domain    或    .\test_domain.exe
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h" // 造帧用的 putI16/putU16/putU32（协议统一小端）
#include "domain/frames.h"
#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"

#include <QSet>
#include <QSignalSpy>

#include <cmath>

#include "frame_factory.h"

using namespace domain;

namespace {

/// 按协议长度造一帧 0x10 姿态（pitch/roll/yaw 单位 0.01°，角速度 0.01 dps）
QByteArray poseFrame(qint16 pitch, qint16 roll, qint16 yaw)
{
    std::vector<uint8_t> d;
    proto::putI16(d, pitch);
    proto::putI16(d, roll);
    proto::putI16(d, yaw);
    proto::putI16(d, 0);
    proto::putI16(d, 0);
    proto::putI16(d, 0);
    return QByteArray(reinterpret_cast<const char *>(d.data()), static_cast<int>(d.size()));
}

/// 按协议长度造一帧 0x15 诊断
QByteArray diagFrame(quint16 batteryRaw, quint8 stateBits, quint32 oledBytes = 1000)
{
    std::vector<uint8_t> d;
    proto::putU16(d, batteryRaw);
    d.push_back(0x02);       // 页
    d.push_back(0x00);       // Flag_Show
    d.push_back(0x01);       // 姿态算法
    d.push_back(stateBits);  // 状态位
    proto::putU16(d, 18);    // 剩余堆
    proto::putU32(d, oledBytes);
    d.push_back(1);
    d.push_back(2);
    d.push_back(3);
    d.push_back(4);
    return QByteArray(reinterpret_cast<const char *>(d.data()), static_cast<int>(d.size()));
}

/// 按协议长度造一帧 0x12 任务状态
QByteArray taskFrame(quint8 phase, quint8 progress, quint8 fault)
{
    std::vector<uint8_t> d;
    d.push_back(static_cast<uint8_t>(proto::TaskId::Patrol));
    d.push_back(phase);
    d.push_back(progress);
    d.push_back(2);
    d.push_back(fault);
    return QByteArray(reinterpret_cast<const char *>(d.data()), static_cast<int>(d.size()));
}

/// 按协议长度造一帧 0x13 事件
QByteArray eventFrame(proto::EventCode code, quint32 ts)
{
    std::vector<uint8_t> d;
    d.push_back(static_cast<uint8_t>(code));
    proto::putU32(d, ts);
    return QByteArray(reinterpret_cast<const char *>(d.data()), static_cast<int>(d.size()));
}

} // namespace

class TestDomain : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- ① 单位口径
    void units_protocolScales()
    {
        // 依据：协议 §七 约定 1~4
        QCOMPARE(units::ppm(1234), 123.4);
        QCOMPARE(units::deg(static_cast<int16_t>(-1234)), -12.34);
        QCOMPARE(units::dps(56), 0.56);
        QCOMPARE(units::celsius(256), 25.6);
        QCOMPARE(units::rh(610), 61.0);
        QCOMPARE(units::volt(1180), 11.80);
        QCOMPARE(units::pwmPercent(1200), 120.0);
        QCOMPARE(units::kLowBatteryV, 11.10);
    }

    void units_formatIsStable()
    {
        // 定小数位 + 消负零：数值跳动时不"抖列"，也不出现"漂了 -0.00"的假象
        QCOMPARE(units::number(9.999, 2), QStringLiteral("10.00"));
        QCOMPARE(units::number(-0.001, 2), QStringLiteral("0.00"));
        QCOMPARE(units::withUnit(1.234, 2, QStringLiteral("ppm")), QStringLiteral("1.23 ppm"));
        QCOMPARE(units::withUnit(0.0, 1, QString()), QStringLiteral("0.0"));
    }

    // ---------------------------------------------------------------- ② 冻结向量逐字段
    void decode_frozenVectorsUplink()
    {
        // ★ 这些 data 段直接抄自 docs/protocol/test_vectors.json（B/C 共用的冻结向量）
        PoseData pose;
        QVERIFY(decodePose(QByteArray::fromHex("7b0038fed21e0c00deff3800"), pose));
        QCOMPARE(pose.pitchDeg, 1.23);
        QCOMPARE(pose.rollDeg, -4.56);
        QCOMPARE(pose.yawDeg, 78.90);
        QCOMPARE(pose.gyroXDps, 0.12);
        QCOMPARE(pose.gyroYDps, -0.34);
        QCOMPARE(pose.gyroZDps, 0.56);

        MotionData mot;
        QVERIFY(decodeMotion(QByteArray::fromHex("fa0006ffb004550003"), mot));
        QCOMPARE(mot.leftMmS, 250);
        QCOMPARE(mot.rightMmS, -250);
        QCOMPARE(mot.pwmPercent, 120.0);
        QCOMPARE(mot.distanceMm, 85);   // ★ 字段单位是 **mm**（不是 cm）—— 见 MotionData 的说明
        QCOMPARE(domain::units::cmFromMm(mot.distanceMm), 8.5); // 界面按 cm 显示
        QCOMPARE(int(mot.avoidFlags), 3);

        TaskData task;
        QVERIFY(decodeTask(QByteArray::fromHex("01032d0200"), task));
        QCOMPARE(int(task.taskId), int(proto::TaskId::Patrol));
        QCOMPARE(int(task.phase), int(proto::Phase::Patrol));
        QCOMPARE(task.progressPct, 45);
        QCOMPARE(task.checkpoint, 2);
        QVERIFY(!task.anyFault());

        EventData ev;
        QVERIFY(decodeEvent(QByteArray::fromHex("0640e20100"), ev));
        QCOMPARE(int(ev.code), int(proto::EventCode::GasOverLimit));
        QCOMPARE(ev.timestampMs, quint32(123456));

        EnvData env;
        QVERIFY(decodeEnv(QByteArray::fromHex("d204290937027a03000162020f010000"), env));
        QCOMPARE(env.mq2Raw, 1234);
        QCOMPARE(env.mq135Raw, 2345);
        QCOMPARE(env.mq2Ppm, 56.7);
        QCOMPARE(env.mq135Ppm, 89.0);
        QCOMPARE(env.tempC, 25.6);
        QCOMPARE(env.humidityPct, 61.0);
        QVERIFY(env.mq2Ready());
        QVERIFY(env.mq135Ready());
        QVERIFY(env.dht11Valid());
        QVERIFY(env.compensationOn());
        QCOMPARE(int(env.gear), int(proto::SampleGear::Normal));
    }

    void decode_diagAndLoop()
    {
        // 0x15（协议 §3.4）：N=16
        DiagData diag;
        QVERIFY(decodeDiag(QByteArray::fromHex("9c040200010312000000010001020304"), diag));
        QCOMPARE(diag.batteryV, 11.80);
        QCOMPARE(int(diag.page), 2);
        QVERIFY(!diag.flagShow);
        QCOMPARE(int(diag.attitudeAlgo), 1);
        QVERIFY(diag.alarmActive());
        QVERIFY(diag.safetyLatched());
        QCOMPARE(diag.heapKb, 18);
        QCOMPARE(diag.oledBytes, quint32(65536));
        QCOMPARE(int(diag.tickComm), 1);
        QCOMPARE(int(diag.tickUi), 4);

        // 0x16（协议 §3.5）：N=20；偏移 19 是固件版本字节（协议 §3.7）
        LoopData loop;
        QVERIFY(decodeLoop(QByteArray::fromHex("6400ceff19004b007d004b007d00f9ff070004af"), loop));
        QCOMPARE(loop.balancePwm, 100);
        QCOMPARE(loop.velocityPwm, -50);
        QCOMPARE(loop.turnPwm, 25);
        QCOMPARE(loop.moto1, 75);
        QCOMPARE(loop.moto2, 125);
        QCOMPARE(loop.ccrA, 75);
        QCOMPARE(loop.ccrB, 125);
        QCOMPARE(loop.encLeft, -7);
        QCOMPARE(loop.encRight, 7);
        QVERIFY(loop.setPwmCalled());
        QVERIFY(!loop.computedButNotExecuted());
        QCOMPARE(firmwareVersionText(loop.fwVersion), QStringLiteral("10-15"));

        QCOMPARE(expectedUpLen(proto::up::kPose), 12);
        QCOMPARE(expectedUpLen(proto::up::kMotion), 9);
        QCOMPARE(expectedUpLen(proto::up::kDiag), 16);
        QCOMPARE(expectedUpLen(proto::up::kLoop), 20);
        QCOMPARE(expectedUpLen(0x7E), -1);
    }

    void loop_flagsComputedButNotExecuted()
    {
        // "算了没执行"的直接断言：Moto 非零而 CCR=0（协议 §3.5 判读要点）
        LoopData loop;
        QVERIFY(decodeLoop(QByteArray::fromHex("000000000000701700000000000000000000" "04af"), loop));
        QCOMPARE(loop.moto1, 6000); // 0x1770
        QCOMPARE(loop.ccrA, 0);
        QVERIFY(loop.computedButNotExecuted());
    }

    // ---------------------------------------------------------------- ③ 长度防御
    void decode_rejectsWrongLength()
    {
        // 0x11 是 9 字节：给 11 字节（v1.0 文档里写错的那个长度）必须判无效
        MotionData mot;
        QVERIFY(!decodeMotion(QByteArray(11, '\0'), mot));
        QVERIFY(decodeMotion(QByteArray(9, '\0'), mot));
        EnvData env;
        QVERIFY(!decodeEnv(QByteArray(14, '\0'), env)); // 只发 14 字节的 0x14（P0 踩过）
        QVERIFY(decodeEnv(QByteArray(16, '\0'), env));
        PoseData pose;
        QVERIFY(!decodePose(QByteArray(11, '\0'), pose));
    }

    // ---------------------------------------------------------------- ④ 状态聚合
    void state_mergesTwoLinksAndKeepsSource()
    {
        VehicleState st;
        st.onFrame(io::LinkKind::Wire, proto::up::kPose, poseFrame(123, -456, 7890));
        st.onFrame(io::LinkKind::Bluetooth, proto::up::kMotion, QByteArray::fromHex("fa0006ffb004550003"));

        QVERIFY(st.received(proto::up::kPose));
        QVERIFY(st.received(proto::up::kMotion));
        QCOMPARE(int(st.sourceOf(proto::up::kPose)), int(io::LinkKind::Wire));
        QCOMPARE(int(st.sourceOf(proto::up::kMotion)), int(io::LinkKind::Bluetooth));
        QCOMPARE(st.pose().pitchDeg, 1.23);
        QCOMPARE(st.pose().rollDeg, -4.56);
        QCOMPARE(st.motion().leftMmS, 250);
        QVERIFY(st.anyFrameAgeMs() >= 0);
        QCOMPARE(st.frameCount(io::LinkKind::Wire), quint64(1));
        QCOMPARE(st.frameCount(io::LinkKind::Bluetooth), quint64(1));
        QVERIFY(st.linkAlive());

        // 第二条链路的同一种帧后到：值取**最新**，来源跟着最新那条走
        st.onFrame(io::LinkKind::Bluetooth, proto::up::kPose, poseFrame(999, 0, 0));
        QCOMPARE(int(st.sourceOf(proto::up::kPose)), int(io::LinkKind::Bluetooth));
        QCOMPARE(st.pose().pitchDeg, 9.99);
    }

    void state_malformedFrameIsCountedNotApplied()
    {
        VehicleState st;
        // 只喂一帧长度错的 0x11：既不认作"收到过"，也不许按偏移硬读
        st.onFrame(io::LinkKind::Wire, proto::up::kMotion, QByteArray(11, '\0'));
        QCOMPARE(st.malformedFrames(), quint64(1));
        QVERIFY(!st.received(proto::up::kMotion));
        QCOMPARE(st.motion().leftMmS, 0);

        // 长度正确后才算收到，且值来自这一帧
        st.onFrame(io::LinkKind::Wire, proto::up::kMotion, QByteArray::fromHex("fa0006ffb004550003"));
        QVERIFY(st.received(proto::up::kMotion));
        QCOMPARE(st.motion().leftMmS, 250);
        QCOMPARE(st.malformedFrames(), quint64(1));

        st.onFrame(io::LinkKind::Wire, 0x7E, QByteArray(3, '\0')); // 协议外命令
        QCOMPARE(st.unknownFrames(), quint64(1));
        QCOMPARE(st.malformedFrames(), quint64(1));
    }

    void state_batteryAndSafetyLifecycle()
    {
        VehicleState st;
        QSignalSpy spy(&st, &VehicleState::eventReceived);

        // 电压 10.95 V：低于报警线 11.10 → 低压；但还没到 10.80 的"严重"级
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, diagFrame(1095, 0x00));
        QVERIFY(st.batteryValid());
        QVERIFY(st.batteryLow());
        QCOMPARE(int(st.batterySeverity()), int(Severity::Warn));
        QVERIFY(st.safetySummary().contains(QStringLiteral("低压")));

        // 0x0A 急停 → 锁存；此时即使 0x15 的状态位没置，也必须显示锁存（安全相关不做平滑）
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   eventFrame(proto::EventCode::EstopReceived, 1000));
        QVERIFY(st.safetyLatched());
        QCOMPARE(int(st.safetySeverity()), int(Severity::Alarm));
        QVERIFY(st.safetySummary().contains(QStringLiteral("锁存")));

        // 0x09 解除 → 不再锁存
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   eventFrame(proto::EventCode::SafetyReleased, 2000));
        QVERIFY(!st.safetyLatched());
        QVERIFY(st.lastSafetyEventText().contains(QStringLiteral("解除")));
        QCOMPARE(spy.count(), 2);

        // 0x15 的 bit1 也要能独立表达"现在锁存着"（事件可能丢，状态位不会）
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, diagFrame(1180, 0x02));
        QVERIFY(st.safetyLatched());
        // 诊断位的锁存解除（固件解除时会把这一位清 0）→ 不再锁存
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, diagFrame(1180, 0x00));
        QVERIFY(!st.safetyLatched());

        // 异常标志位 → 报警中
        st.onFrame(io::LinkKind::Bluetooth, proto::up::kTask, taskFrame(3, 45, 0x01));
        QVERIFY(st.alarmActive());
        QVERIFY(st.safetySummary().contains(QStringLiteral("气体")));
        QVERIFY(faultFlagsText(0x0B).contains(QStringLiteral("气体")));
        QVERIFY(faultFlagsText(0x0B).contains(QStringLiteral("传感器")));
        QCOMPARE(faultFlagsText(0), QStringLiteral("无"));
    }

    void state_dedupesSameFrameFromTwoLinks()
    {
        // 协议 §七 约定 6：两条链路都会发同样的周期帧。**同一次物理测量只能算一次** ——
        // 否则告警的"连续确认 3 帧"会被 1.5 次真实采样满足，曲线也会重复采样。
        VehicleState st;
        QSignalSpy updates(&st, &VehicleState::dataUpdated);

        const QByteArray f = testframes::envPpm(20.0, 30.0);
        st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, f);
        st.onFrame(io::LinkKind::Bluetooth, proto::up::kEnvGas, f); // 同一次上报的第二次投递
        QCOMPARE(updates.count(), 1);
        QCOMPARE(st.duplicateFrames(), quint64(1));
        // 链路统计仍各自计数（"每条链路收到多少帧"是链路质量，不能被去重抹掉）
        QCOMPARE(st.frameCount(io::LinkKind::Wire), quint64(1));
        QCOMPARE(st.frameCount(io::LinkKind::Bluetooth), quint64(1));
        QCOMPARE(int(st.sourceOf(proto::up::kEnvGas)), int(io::LinkKind::Bluetooth)); // 来源跟最新

        // 同一条链路连发两帧相同内容（20 Hz 下很常见）**不是**重复
        st.onFrame(io::LinkKind::Wire, proto::up::kPose, testframes::pose());
        st.onFrame(io::LinkKind::Wire, proto::up::kPose, testframes::pose());
        QCOMPARE(st.duplicateFrames(), quint64(1));

        // 内容不同 ⇒ 是另一次测量
        st.onFrame(io::LinkKind::Bluetooth, proto::up::kEnvGas, testframes::envPpm(21.0, 30.0));
        QCOMPARE(st.duplicateFrames(), quint64(1));
        // 4 次 dataUpdated = env(新) + pose(新) + pose(同链路重复**也算新**) + env(内容不同)
        QCOMPARE(updates.count(), 4);
    }

    void state_oledRateAndTicks()
    {
        VehicleState st;
        // 两次诊断帧：oledByte 从 1000 → 3000，间隔很短 → 速率应为正
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, diagFrame(1180, 0, 1000));
        QTest::qWait(60);
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, diagFrame(1180, 0, 3000));
        QVERIFY2(st.oledBytesPerSec() > 0.0, "OLED 写计数在涨 → 速率必须为正");
        // tick 在涨 → 四个任务都判为存活
        QVERIFY(st.taskTickAlive(0));
        QVERIFY(st.taskTickAlive(3));
    }

    // ---------------------------------------------------------------- ⑤ 端到端（假源）
    void fakeSource_coversEveryUpFrameAndDecodesClean()
    {
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        VehicleState st;
        QObject::connect(&mgr, &io::LinkManager::frameReady, &st, &VehicleState::onFrame);

        mgr.open(io::LinkKind::Wire);
        QTest::qWait(2300); // 0x14/0x15/0x16 都是 1 Hz，需要跨过 2 个周期
        mgr.close(io::LinkKind::Wire);

        // 假源必须覆盖界面要用到的每一种上行帧（少一种 = 那块界面离线永远空白）
        for (quint8 cmd : {proto::up::kPose, proto::up::kMotion, proto::up::kTask,
                           proto::up::kEnvGas, proto::up::kDiag, proto::up::kLoop,
                           proto::up::kHeartbeat}) {
            QVERIFY2(st.received(cmd), qPrintable(QStringLiteral("假源未覆盖命令 0x%1")
                                                     .arg(cmd, 2, 16, QLatin1Char('0'))));
        }
        // 端到端最重要的一条：**格式错帧必须为 0**（假源与 domain 的口径一致）
        QCOMPARE(st.malformedFrames(), quint64(0));
        QCOMPARE(st.unknownFrames(), quint64(0));
        QVERIFY(st.batteryValid());
        QVERIFY(std::fabs(st.pose().pitchDeg) <= 90.0); // 假源姿态在 ±12.5° 内摆动
        QVERIFY(st.env().mq2Ppm > 0.0);
        QVERIFY(st.received(proto::up::kEvent)); // 1 s 处的自检完成事件
    }

    void fakeSource_safetyDramaLatchThenRelease()
    {
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        mgr.setReplayRate(20.0); // 20 倍：3 s 实时 ≈ 60 s 虚拟（剧本：40 s 急停、48 s 解除）
        VehicleState st;
        QObject::connect(&mgr, &io::LinkManager::frameReady, &st, &VehicleState::onFrame);

        mgr.open(io::LinkKind::Wire);
        bool sawLatched = false;
        bool sawReleaseAfterLatch = false;
        bool sawBatteryLow = false;
        bool sawGasEvent = false;
        QObject::connect(&st, &VehicleState::eventReceived, [&sawGasEvent](const EventData &e, io::LinkKind) {
            if (e.code == proto::EventCode::GasOverLimit) {
                sawGasEvent = true;
            }
        });
        for (int i = 0; i < 300; ++i) {
            QTest::qWait(10);
            if (st.safetyLatched()) {
                sawLatched = true;
            } else if (sawLatched) {
                sawReleaseAfterLatch = true;
            }
            if (st.batteryLow()) {
                sawBatteryLow = true;
            }
        }
        mgr.close(io::LinkKind::Wire);

        QVERIFY2(sawLatched, "假源应在 40 s（虚拟）处给出急停锁存");
        QVERIFY2(sawReleaseAfterLatch, "48 s（虚拟）应给出解除（锁存→解除必须成对出现）");
        QVERIFY2(sawBatteryLow, "50~56 s（虚拟）应出现低压，用来验证低压显示");
        QVERIFY2(sawGasEvent, "气体冲高应报 0x06 事件");
        QCOMPARE(st.malformedFrames(), quint64(0));
    }
};

QTEST_MAIN(TestDomain)
#include "test_domain.moc"
