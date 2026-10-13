// ============================================================================
//  P0 链路层测试（不需要任何硬件 —— 全部跑在假源上）
//
//  覆盖四件事，每件都对应一个"以后一定会踩的坑"：
//    ① 合成假源能按协议周期产帧，且统计数自洽（收字节 / 解帧数 / 帧率）
//    ② 坏帧比例 100% 时**必须**计入丢帧、且解帧数为 0（证明丢帧统计不是摆设）
//    ③ 录制文件回放：按行读十六进制、一行多帧也要正确切开，顺序不乱、不丢
//    ④ 双链路管理：两条链路各自计数；主链路"有线优先"；0x24 下发能在假源上拿到回帧
//
//  跑法：ctest -R link    或    .\test_link.exe
// ============================================================================

#include <QtTest/QtTest>

#include "io/link_manager.h"
#include "io/replay_link.h"

#include <QSignalSpy>
#include <QTemporaryDir>

#include "codec.h"

using namespace io;

namespace {

/// 造一条可用于回放文件的完整帧（0x12 任务状态）
QByteArray makeTaskFrame(quint8 progress)
{
    std::vector<uint8_t> d;
    d.push_back(static_cast<uint8_t>(proto::TaskId::Patrol));
    d.push_back(static_cast<uint8_t>(proto::Phase::Patrol));
    d.push_back(progress);
    d.push_back(1);
    d.push_back(0);
    const auto v = proto::encodeFrame(proto::up::kTask, d);
    return QByteArray(reinterpret_cast<const char *>(v.data()), static_cast<int>(v.size()));
}

} // namespace

class TestLink : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- ① 合成假源
    void synthetic_producesFramesAndConsistentStats()
    {
        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        QSignalSpy frames(&mgr, &LinkManager::frameReady);

        mgr.open(LinkKind::Wire);
        QVERIFY(mgr.isOpen(LinkKind::Wire));
        QTest::qWait(600); // 600 ms：按协议周期应产出 0x10/0x11 各 ~12 帧、0x12/0x1F ~3 帧

        mgr.close(LinkKind::Wire);

        const LinkStats s = mgr.stats(LinkKind::Wire);
        QVERIFY2(s.bytesRx > 0, "应收到字节");
        QVERIFY2(s.framesOk > 0, "应解出帧");
        QVERIFY2(frames.count() == static_cast<int>(s.framesOk),
                 "帧信号条数必须与解帧统计一致（同一来源，不许两套账）");
        QCOMPARE(s.framesDropped, quint64(0));
        qInfo("合成假源：%llu 帧 / %llu 字节", static_cast<unsigned long long>(s.framesOk),
              static_cast<unsigned long long>(s.bytesRx));
    }

    // ---------------------------------------------------------------- ② 坏帧必须被计入
    void corruptFrames_countedAsDropped()
    {
        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        mgr.setReplayCorruptRatio(1.0); // 全部破坏 CRC
        QSignalSpy frames(&mgr, &LinkManager::frameReady);

        mgr.open(LinkKind::Wire);
        QTest::qWait(400);
        mgr.close(LinkKind::Wire);

        const LinkStats s = mgr.stats(LinkKind::Wire);
        QCOMPARE(frames.count(), 0);          // 全是坏帧 → 一帧都不该报出来
        QVERIFY2(s.framesDropped > 0, "坏帧必须计入丢帧");
        QCOMPARE(s.framesOk, quint64(0));
        QVERIFY(s.dropRate() > 0.99);
    }

    // ---------------------------------------------------------------- ③ 文件回放
    void captureFile_replaysInOrderWithoutLoss()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("capture.txt"));

        // 写 5 行：每行一帧；其中第 3 行**故意放两帧**（验证"一行多帧"能切开）
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        for (int i = 0; i < 5; ++i) {
            const QByteArray fr = makeTaskFrame(static_cast<quint8>(i * 10));
            if (i == 2) {
                const QByteArray fr2 = makeTaskFrame(99);
                out << fr.toHex(' ').toUpper() << ' ' << fr2.toHex(' ').toUpper() << '\n';
            } else {
                out << fr.toHex(' ').toUpper() << '\n';
            }
        }
        f.close();

        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        QCOMPARE(mgr.loadCapture(LinkKind::Wire, path), 6); // 5 行 + 多出 1 帧

        QVector<QByteArray> got;
        connect(&mgr, &LinkManager::frameReady, this,
                [&got](LinkKind, quint8 cmd, const QByteArray &d) {
                    if (cmd == proto::up::kTask) {
                        got.append(d);
                    }
                });

        mgr.open(LinkKind::Wire);
        QTest::qWait(400);
        mgr.close(LinkKind::Wire);

        QVERIFY2(got.size() >= 6, "至少应回放出 6 帧");
        // 顺序必须与文件一致（循环回放，取前 6 帧比对）
        QCOMPARE(int(got.at(0).at(2)), 0);   // 第 1 帧 progress=0
        QCOMPARE(int(got.at(1).at(2)), 10);  // 第 2 帧 =10
        QCOMPARE(int(got.at(2).at(2)), 20);  // 第 3 帧（同一行的第 1 帧）=20
        QCOMPARE(int(got.at(3).at(2)), 99);  // 同一行的第 2 帧 =99
        QCOMPARE(int(got.at(4).at(2)), 30);
    }

    // ---------------------------------------------------------------- ④ 双链路与主链路
    void manager_twoLinksAndPrimaryPreference()
    {
        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        mgr.setMode(LinkKind::Bluetooth, LinkManager::Mode::Replay);

        QSignalSpy wireFrames(&mgr, &LinkManager::frameReady);
        // 只开蓝牙：主链路应退到蓝牙
        mgr.open(LinkKind::Bluetooth);
        QTest::qWait(200);
        QCOMPARE(mgr.primaryLink(), LinkKind::Bluetooth);
        QVERIFY(mgr.stats(LinkKind::Bluetooth).framesOk > 0);
        QCOMPARE(mgr.stats(LinkKind::Wire).framesOk, quint64(0));

        // 再开有线：主链路应切到有线（有线优先）
        mgr.open(LinkKind::Wire);
        QTest::qWait(100);
        QCOMPARE(mgr.primaryLink(), LinkKind::Wire);

        const int bothOpen = wireFrames.count();
        QVERIFY2(bothOpen > 0, "两条链路都开时，帧都应上报");
        mgr.closeAll();
        QCOMPARE(mgr.primaryLink(), LinkKind::Count);
    }

    // ---------------------------------------------------------------- ⑤ 0x24 调参往返（假源）
    void pidTuneRoundTripOnReplay()
    {
        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        mgr.open(LinkKind::Wire);
        QTest::qWait(50);

        auto *rl = qobject_cast<ReplayLink *>(mgr.link(LinkKind::Wire));
        QVERIFY(rl != nullptr);
        QSignalSpy replies(rl, &ReplayLink::replyEmitted);

        // 下发 0x24：ID=0x06(Turn_Kp) + 保留 + float32(52.0f)
        QByteArray data;
        data.append(char(0x06));
        data.append(char(0x00));
        const float v = 52.0f;
        data.append(reinterpret_cast<const char *>(&v), 4);
        QCOMPARE(data.size(), 6);

        QVERIFY(mgr.send(proto::down::kPidTune, data) > 0);
        QTest::qWait(50);
        QCOMPARE(replies.count(), 1);
        const QList<QVariant> args = replies.takeFirst();
        QCOMPARE(args.at(0).toInt(), int(proto::down::kPidTune));
        QCOMPARE(args.at(1).toByteArray(), data);
        mgr.close(LinkKind::Wire);
    }

    // ---------------------------------------------------------------- ⑥ 压测：不丢帧
    void stress_tenTimesRate_noLoss()
    {
        LinkManager mgr;
        mgr.setMode(LinkKind::Wire, LinkManager::Mode::Replay);
        mgr.setReplayRate(10.0); // 10 倍速率
        QSignalSpy frames(&mgr, &LinkManager::frameReady);

        mgr.open(LinkKind::Wire);
        QTest::qWait(1000);
        mgr.close(LinkKind::Wire);

        const LinkStats s = mgr.stats(LinkKind::Wire);
        QCOMPARE(s.framesDropped, quint64(0)); // 压测下也不许丢
        QCOMPARE(frames.count(), static_cast<int>(s.framesOk));
        // 10 倍速率下 1 s 至少几百帧（合成源：0x10/0x11 各 200/s + 0x12/0x1F 各 50/s）
        QVERIFY2(s.framesOk >= 300, qPrintable(QStringLiteral("压测帧数偏低：%1").arg(s.framesOk)));
        qInfo("压测：1 s 内 %llu 帧，0 丢帧",
              static_cast<unsigned long long>(s.framesOk));
    }
};

QTEST_MAIN(TestLink)
#include "test_link.moc"
