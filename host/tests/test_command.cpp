// ============================================================================
//  P3 下行命令与保活测试（不需要硬件、不需要界面）
//
//  为什么下行命令值得单独测：**下行是有副作用的** ——
//  偏移错一位、符号写反、超出范围回绕，现象都是"车按错误的语义动"。
//  这里做两件事：
//    ① **造 → 组帧 → 解帧 → 逐字段比对**（走的就是 App 真正发送时经过的那条路径：
//       数据段由 domain 造，帧头/长度/CRC 由协议层加，再从解帧器读回来）；
//    ② 保活引擎：默认必须发**心跳 0x1F**（不会解除锁存）；链路不通时**不许假装在保活**；
//       使能模式（会解除锁存）是可选项，且行为要能被断言。
//
//  跑法：ctest -R command    或    .\test_command.exe
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h"
#include "data/event_log.h"
#include "domain/commands.h"
#include "domain/keepalive.h"

#include <QSignalSpy>

using namespace domain;

namespace {

/// 把 domain 造的数据段组帧、再解回来（与真实发送路径一致：只有一处组帧实现）
bool roundTrip(quint8 cmd, const QByteArray &payload, QByteArray *back)
{
    const std::vector<uint8_t> d(payload.begin(), payload.end());
    const auto frame = proto::encodeFrame(cmd, d);
    proto::FrameDecoder dec;
    const auto frames = dec.feed(frame);
    if (frames.size() != 1 || frames[0].cmd != cmd) {
        return false;
    }
    if (back != nullptr) {
        *back = QByteArray(reinterpret_cast<const char *>(frames[0].data.data()),
                           static_cast<int>(frames[0].data.size()));
    }
    return true;
}

} // namespace

class TestCommand : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- 0x20 运动控制
    void motion_layoutAndClamping()
    {
        const QByteArray p = cmd::motionPayload(250, -300, true);
        QCOMPARE(p.size(), 5);
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(p.constData())), qint16(250));
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(p.constData()) + 2), qint16(-300));
        QCOMPARE(quint8(p.at(4)), quint8(1));

        // 超出 int16 的范围必须**钳位**而不是回绕：回绕会把"全速前进"变成"后退"
        const QByteArray big = cmd::motionPayload(40000, -40000, true);
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(big.constData())), qint16(32767));
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(big.constData()) + 2), qint16(-32768));

        const QByteArray stop = cmd::stopPayload();
        QCOMPARE(stop.size(), 5);
        QCOMPARE(quint8(stop.at(4)), quint8(0)); // enable=0 = 立即停
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(stop.constData())), qint16(0));
    }

    // ---------------------------------------------------------------- 0x21 任务指示
    void task_layoutAndParams()
    {
        const QByteArray p = cmd::taskPayload(proto::TaskId::Patrol);
        QCOMPARE(p.size(), 2); // N = 2 + 2k，k=0
        QCOMPARE(quint8(p.at(0)), quint8(0x01));
        QCOMPARE(quint8(p.at(1)), quint8(0));

        const QByteArray q = cmd::taskPayload(proto::TaskId::Return, {7, 300});
        QCOMPARE(q.size(), 6);
        QCOMPARE(quint8(q.at(1)), quint8(2));
        QCOMPARE(proto::getU16(reinterpret_cast<const uint8_t *>(q.constData()) + 2), quint16(7));
        QCOMPARE(proto::getU16(reinterpret_cast<const uint8_t *>(q.constData()) + 4), quint16(300));
    }

    // ---------------------------------------------------------------- 0x23 巡检参数
    void surveyCfg_layoutAndPpmScale()
    {
        // 阈值单位是 ppm×10（与 0x14 同口径）：30.0 ppm → 300
        const QByteArray p = cmd::surveyCfgPayload(1000, 30.0, 50.0, true, false, 3);
        QCOMPARE(p.size(), 12);
        const auto *b = reinterpret_cast<const uint8_t *>(p.constData());
        QCOMPARE(proto::getU16(b + 0), quint16(1000));
        QCOMPARE(proto::getU16(b + 2), quint16(300));
        QCOMPARE(proto::getU16(b + 4), quint16(500));
        QCOMPARE(quint8(p.at(6)), quint8(1)); // 自适应采样开
        QCOMPARE(quint8(p.at(7)), quint8(0)); // 温湿度补偿关
        QCOMPARE(quint8(p.at(8)), quint8(3)); // 连续确认帧数
        QCOMPARE(quint8(p.at(9)), quint8(0));
        QCOMPARE(quint8(p.at(11)), quint8(0));

        // 超大阈值也必须钳到 uint16 上限，而不是回绕成"4 ppm"
        const QByteArray big = cmd::surveyCfgPayload(1000, 70000.0, 50.0, false, false, 1);
        QCOMPARE(proto::getU16(reinterpret_cast<const uint8_t *>(big.constData()) + 2), quint16(65535));
    }

    // ---------------------------------------------------------------- 0x24 PID 调参
    void pidTune_float32LittleEndianAndRejectsNaN()
    {
        QByteArray p;
        QVERIFY(cmd::pidTunePayload(0x06, 52.0f, &p));
        QCOMPARE(p.size(), 6);
        QCOMPARE(quint8(p.at(0)), quint8(0x06));
        QCOMPARE(quint8(p.at(1)), quint8(0x00)); // 保留
        // 52.0f 的 IEEE-754 位型 = 0x42500000 → 小端字节 00 00 50 42
        // （52 = 1.625×2^5 ⇒ 阶码 132=0x84、尾数 0x500000；这个期望值一度被我算错成
        //   0x42480000，测试当场纠正过来 —— 正是"造帧也要有独立期望值"的意义）
        QCOMPARE(quint8(p.at(2)), quint8(0x00));
        QCOMPARE(quint8(p.at(3)), quint8(0x00));
        QCOMPARE(quint8(p.at(4)), quint8(0x50));
        QCOMPARE(quint8(p.at(5)), quint8(0x42));

        QByteArray bad;
        QVERIFY(!cmd::pidTunePayload(0x01, std::nanf(""), &bad)); // NaN 不发
        QVERIFY(!cmd::pidTunePayload(0x01, std::numeric_limits<float>::infinity(), &bad));
        QVERIFY(!cmd::pidTunePayload(0x01, 1.0f, nullptr));
    }

    // ---------------------------------------------------------------- 往返（与真实发送同路径）
    void allCommands_roundTripThroughCodec()
    {
        struct Case {
            quint8 cmd;
            QByteArray payload;
        };
        QList<Case> cases;
        cases << Case{proto::down::kMotion, cmd::motionPayload(250, 300, true)};
        cases << Case{proto::down::kMotion, cmd::stopPayload()};
        cases << Case{proto::down::kTask, cmd::taskPayload(proto::TaskId::Patrol, {1, 2, 3})};
        cases << Case{proto::down::kMode, cmd::modePayload(2)};
        cases << Case{proto::down::kSurveyCfg, cmd::surveyCfgPayload(500, 30.0, 50.0, true, true, 3)};
        cases << Case{proto::down::kEstop, cmd::estopPayload()};
        cases << Case{proto::up::kHeartbeat, cmd::heartbeatPayload()};
        QByteArray p;
        QVERIFY(cmd::pidTunePayload(0x03, 80.0f, &p));
        cases << Case{proto::down::kPidTune, p};

        for (const Case &c : cases) {
            QByteArray back;
            QVERIFY2(roundTrip(c.cmd, c.payload, &back),
                     qPrintable(QStringLiteral("命令 0x%1 往返失败")
                                    .arg(c.cmd, 2, 16, QLatin1Char('0'))));
            QCOMPARE(back, c.payload);
        }
    }

    void safetyEffect_isExplicit()
    {
        // 急停会锁存；任务/运动命令**可能解除**锁存（协议 v1.1）；心跳什么也不做
        QCOMPARE(int(cmd::safetyEffect(proto::down::kEstop)), int(cmd::SafetyEffect::Latches));
        QCOMPARE(int(cmd::safetyEffect(proto::down::kTask)), int(cmd::SafetyEffect::Releases));
        QCOMPARE(int(cmd::safetyEffect(proto::down::kMotion)), int(cmd::SafetyEffect::Releases));
        QCOMPARE(int(cmd::safetyEffect(proto::up::kHeartbeat)), int(cmd::SafetyEffect::None));
        QVERIFY(QString::fromUtf8(cmd::name(proto::down::kEstop)).contains(QStringLiteral("0x2F")));
    }

    // ---------------------------------------------------------------- 保活引擎
    void keepalive_disabledSendsNothing()
    {
        KeepaliveEngine ka;
        int sent = 0;
        ka.setSink([&sent](quint8, const QByteArray &) {
            ++sent;
            return 8;
        });
        ka.setLinkReady(true);
        ka.setEnabled(false);
        QTest::qWait(350); // 跨过至少一个周期
        QCOMPARE(sent, 0);
        QCOMPARE(ka.sentCount(), quint64(0));
        QVERIFY(!ka.sendNow());
    }

    void keepalive_sendsImmediatelyWhenEnabledAndLinkReady()
    {
        KeepaliveEngine ka;
        ka.setIntervalMs(200);
        QList<quint8> cmds;
        ka.setSink([&cmds](quint8 c, const QByteArray &) {
            cmds << c;
            return 7;
        });

        // 链路不通时：打开保活也**不发**（不能假装在保活）
        ka.setEnabled(true);
        QCOMPARE(cmds.size(), 0);
        QVERIFY(!ka.linkReady());

        // 链路就绪 → 立刻发第一条（而不是等一个周期）
        ka.setLinkReady(true);
        QCOMPARE(cmds.size(), 1);
        QCOMPARE(int(cmds.first()), int(proto::up::kHeartbeat));

        // 之后按周期发
        QTest::qWait(480);
        QVERIFY2(cmds.size() >= 3, qPrintable(QStringLiteral("480 ms 内应至少发 3 条，实际 %1")
                                                  .arg(cmds.size())));
        for (quint8 c : cmds) {
            QCOMPARE(int(c), int(proto::up::kHeartbeat)); // 默认必须是心跳
        }
        QVERIFY(ka.sinceLastSendMs() >= 0);
        QVERIFY(ka.sinceLastSendMs() < ka.intervalMs() + 100);
    }

    void keepalive_stopsWhenLinkGoesDown()
    {
        KeepaliveEngine ka;
        ka.setIntervalMs(100);
        int sent = 0;
        ka.setSink([&sent](quint8, const QByteArray &) {
            ++sent;
            return 8;
        });
        ka.setEnabled(true);
        ka.setLinkReady(true);
        QCOMPARE(sent, 1);
        ka.setLinkReady(false);
        const int snapshot = sent;
        QTest::qWait(250);
        QCOMPARE(sent, snapshot); // 链路断了以后不许继续发
    }

    void keepalive_enableModeIsExplicitAndDangerous()
    {
        KeepaliveEngine ka;
        QList<QByteArray> payloads;
        QList<quint8> cmds;
        ka.setSink([&payloads, &cmds](quint8 c, const QByteArray &d) {
            cmds << c;
            payloads << d;
            return 5;
        });
        ka.setMode(KeepaliveMode::EnableZeroSpeed);
        ka.setEnabled(true);
        ka.setLinkReady(true);
        QCOMPARE(cmds.size(), 1);
        QCOMPARE(int(cmds.first()), int(proto::down::kMotion));
        QCOMPARE(payloads.first().size(), 5);
        // 零速 + 使能=1：协议规定这会**解除安全锁存** —— 所以它只能是显式选择
        QCOMPARE(quint8(payloads.first().at(4)), quint8(1));
        QCOMPARE(proto::getI16(reinterpret_cast<const uint8_t *>(payloads.first().constData())),
                 qint16(0));
        QCOMPARE(int(KeepaliveEngine::commandFor(KeepaliveMode::EnableZeroSpeed)),
                 int(proto::down::kMotion));
        QCOMPARE(int(KeepaliveEngine::commandFor(KeepaliveMode::Heartbeat)),
                 int(proto::up::kHeartbeat));
    }

    void keepalive_reportsSendFailure()
    {
        KeepaliveEngine ka;
        ka.setSink([](quint8, const QByteArray &) { return qint64(-1); });
        QSignalSpy failed(&ka, &KeepaliveEngine::sendFailed);
        ka.setEnabled(true);
        ka.setLinkReady(true);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ka.sentCount(), quint64(0)); // 失败不算"已发"
    }

    void keepalive_intervalIsClamped()
    {
        KeepaliveEngine ka;
        ka.setIntervalMs(10); // 太小：会比固件判据还密，白白占链路
        QCOMPARE(ka.intervalMs(), 50);
        ka.setIntervalMs(99999);
        QCOMPARE(ka.intervalMs(), 2000);
        ka.setIntervalMs(200);
        QCOMPARE(ka.intervalMs(), 200);
    }

    // ---------------------------------------------------------------- 操作员动作留档
    void eventLog_recordsOperatorActions()
    {
        data::EventLog log;
        log.onOperatorAction(proto::down::kEstop, QStringLiteral("立即切断电机输出并锁存"), true);
        log.onOperatorAction(proto::down::kMotion, QStringLiteral("lin=250 使能=1"), false);
        QCOMPARE(log.size(), 2);
        QVERIFY(log.at(0).operatorAction);
        QVERIFY(log.at(0).safety); // 急停属安全类动作 → 事件流里要显眼
        QVERIFY(log.at(0).title.contains(QStringLiteral("紧急停机")));
        QVERIFY2(log.at(1).title.contains(QStringLiteral("失败")), "失败的下发必须留痕");
        // 遥控（0x20）不算"安全事件"：它可能解除锁存，但一次遥控几十条，
        // 全标成安全事件会把"只看安全事件"过滤器刷爆（后果写在巡检页提示里）
        QVERIFY(!log.at(1).safety);

        // 任务指示（0x21）是协议规定的解除锁存路径 → 算安全事件
        log.onOperatorAction(proto::down::kTask, QStringLiteral("任务=自主巡航"), true);
        QVERIFY(log.at(2).safety);
    }
};

QTEST_MAIN(TestCommand)
#include "test_command.moc"
