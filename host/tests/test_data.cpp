// ============================================================================
//  P4 记录 / 回放 / 会话库 / 参数账本测试（不需要硬件、不需要界面）
//
//  守四条底线：
//    ① **CSV 列数与表头一致**（"表头 42 列、数据 41 列"这种错位只有 Excel 才会发现）；
//    ② **记录能原样回放**：CSV 里的"原始帧 HEX"列喂回解帧器，解出的命令与数据段必须与
//       记录时逐字节相同 —— 这是"回放能驱动界面"的机械保证；
//    ③ **参数账本要说真话**：下发值 ↔ 回读值配对，不一致必须报出来；越界/NaN 在下发前拒绝；
//    ④ **SQLite 会话/事件/参数能落库**（驱动不可用时降级为"不可用"而不是崩）。
//
//  跑法：ctest -R data    或    .\test_data.exe
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h"
#include "data/event_log.h"
#include "data/param_store.h"
#include "data/recorder.h"
#include "domain/alarm.h"
#include "domain/commands.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>

#include "frame_factory.h"

using namespace data;

namespace {

/// 读整个文件（保留 BOM 供断言检查）
QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

QStringList splitLines(const QByteArray &raw)
{
    return QString::fromUtf8(raw).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

/// 简易 CSV 行切分（本项目的字段里不会有内嵌逗号，除事件备注外；测试只按列数断言）
QStringList splitCsv(const QString &line)
{
    return line.split(QLatin1Char(','));
}

} // namespace

class TestData : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- CSV 记录
    void csvRecorder_headerAndColumnCountMatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("rec.csv"));

        CsvRecorder rec;
        QSignalSpy started(&rec, &CsvRecorder::started);
        QVERIFY(rec.start(path));
        QCOMPARE(started.count(), 1);
        QVERIFY(rec.isRecording());

        // 每种帧都写一行（走的是真实路径：组帧后的原始字节也记进去）
        rec.onFrame(io::LinkKind::Wire, proto::up::kPose, testframes::pose());
        rec.onFrame(io::LinkKind::Wire, proto::up::kMotion, testframes::motion());
        rec.onFrame(io::LinkKind::Wire, proto::up::kTask, testframes::task());
        rec.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(12.3, 45.6));
        rec.onFrame(io::LinkKind::Wire, proto::up::kDiag, testframes::diagVolt(11.85));
        rec.onFrame(io::LinkKind::Wire, proto::up::kLoop, testframes::loop());
        rec.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                    testframes::event(proto::EventCode::GasOverLimit, 1000));
        rec.onFrame(io::LinkKind::Wire, proto::up::kHeartbeat, QByteArray());
        rec.stop();
        QVERIFY(!rec.isRecording());
        QCOMPARE(rec.rowsWritten(), quint64(8));

        const QByteArray raw = readAll(path);
        QVERIFY(!raw.isEmpty());
        // ★ UTF-8 BOM：没有它，中文 Windows 的 Excel 会把表头读成乱码
        QVERIFY2(raw.startsWith("\xEF\xBB\xBF"), "CSV 必须以 UTF-8 BOM 开头（Excel 中文表头）");
        const QStringList lines = splitLines(raw);
        QVERIFY(lines.size() >= 9); // 1 表头 + 8 数据
        // 表头首字符可能是 BOM（视 Qt 是否剥离而定）——两种情形都要能正确处理，
        // 否则会得到"地时间ms"这种让人摸不着头脑的失败（本项目实际踩过一次）。
        QString headerLine = lines.first();
        if (!headerLine.isEmpty() && headerLine.at(0) == QChar(0xFEFF)) {
            headerLine.remove(0, 1);
        }
        const QStringList header = splitCsv(headerLine);
        QCOMPARE(header.size(), 42);
        QCOMPARE(header.first(), QStringLiteral("本地时间ms"));
        QCOMPARE(header.last(), QStringLiteral("原始帧HEX"));
        for (int i = 1; i < lines.size(); ++i) {
            const QStringList cells = splitCsv(lines.at(i));
            QVERIFY2(cells.size() == header.size(),
                     qPrintable(QStringLiteral("第 %1 行列数 %2 ≠ 表头 %3")
                                    .arg(i)
                                    .arg(cells.size())
                                    .arg(header.size())));
        }
        // 值也要对（不是只数了列数）：MQ-2 12.3 ppm 落在第 20 列（0 基 19）
        bool sawGas = false;
        for (int i = 1; i < lines.size(); ++i) {
            const QStringList c = splitCsv(lines.at(i));
            if (c.value(3) == QStringLiteral("0x14")) {
                QCOMPARE(c.value(19), QStringLiteral("12.3"));
                QCOMPARE(c.value(20), QStringLiteral("45.6"));
                sawGas = true;
            }
        }
        QVERIFY(sawGas);
    }

    void csvRecorder_recordsEventsOnSameTimeline()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("ev.csv"));
        CsvRecorder rec;
        QVERIFY(rec.start(path));

        data::EventLog log;
        QObject::connect(&log, &data::EventLog::entryAdded, &rec, &data::CsvRecorder::onEvent);
        domain::EventData stop;
        stop.code = proto::EventCode::EstopReceived;
        stop.timestampMs = 500;
        log.onEvent(stop, io::LinkKind::Wire);
        domain::AlarmRecord r;
        r.title = QStringLiteral("MQ-2 浓度超阈值");
        r.unit = QStringLiteral("ppm");
        r.value = 420.0;
        r.threshold = 30.0;
        log.onAlarmRaised(r);
        rec.stop();

        const QStringList lines = splitLines(readAll(path));
        QCOMPARE(lines.size(), 3); // 表头 + 2 条
        QVERIFY(lines.at(1).contains(QStringLiteral("收到急停指令")));
        QVERIFY(lines.at(2).contains(QStringLiteral("MQ-2 浓度超阈值")));
    }

    void csvRecorder_reportsUnwritablePath()
    {
        CsvRecorder rec;
        // 不存在的盘符 + 非法路径 ⇒ 必须**明确失败**（"以为在录、其实没录"是最危险的）
        QVERIFY(!rec.start(QStringLiteral("Z:/definitely/not/here/rec.csv")));
        QVERIFY(!rec.lastError().isEmpty());
        QVERIFY(!rec.isRecording());
    }

    // ---------------------------------------------------------------- 回放
    void recordedFramesReplayByteForByte()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("replay.csv"));
        CsvRecorder rec;
        QVERIFY(rec.start(path));

        // 记录一批帧（含最容易出错的两类：0x14 的 16 字节与 0x11 的 9 字节）
        const QList<QPair<quint8, QByteArray>> origin{
            {proto::up::kPose, testframes::pose(321, -654, 9876)},
            {proto::up::kMotion, testframes::motion(111, -222, 333, 44, 0x03)},
            {proto::up::kEnvGas, testframes::envPpm(12.3, 45.6)},
            {proto::up::kLoop, testframes::loop()},
        };
        for (const auto &p : origin) {
            rec.onFrame(io::LinkKind::Wire, p.first, p.second);
        }
        rec.stop();

        // ★ 回放：把记录喂给**假源链路**（与界面上"打开记录文件回放"同一条路径）
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        QCOMPARE(mgr.loadCapture(io::LinkKind::Wire, path), 4);

        QList<QPair<quint8, QByteArray>> got;
        QObject::connect(&mgr, &io::LinkManager::frameReady, this,
                         [&got](io::LinkKind, quint8 cmd, const QByteArray &d) {
                             got.append({cmd, d});
                         });
        mgr.open(io::LinkKind::Wire);
        QTest::qWait(400);
        mgr.close(io::LinkKind::Wire);

        QVERIFY2(got.size() >= 4, qPrintable(QStringLiteral("回放应至少产出 4 帧，实际 %1")
                                                 .arg(got.size())));
        for (int i = 0; i < origin.size(); ++i) {
            QCOMPARE(int(got.at(i).first), int(origin.at(i).first));
            QCOMPARE(got.at(i).second, origin.at(i).second); // 逐字节相同
        }
    }

    // ---------------------------------------------------------------- 参数账本
    void paramStore_definitionMatchesProtocol()
    {
        QCOMPARE(pidParams().size(), 7);
        QCOMPARE(pidParamInfo(0x07)->hi, 10.0);   // 转向环 D 上界 10
        QCOMPARE(pidParamInfo(0x04)->hi, 0.82);   // 速度环 I 上界 0.82
        QCOMPARE(pidParamInfo(0x01)->defaultValue, 300.0);
        QVERIFY(pidParamInfo(0x7E) == nullptr);
        QCOMPARE(pidParamName(0xFF), QStringLiteral("全量回读"));
        // B 侧接口文档 §3.2：Zhongzhi 是"平衡目标角（重心补偿）"，默认 0.5
        QCOMPARE(pidParamInfo(0x05)->defaultValue, 0.5);
        QVERIFY(QString::fromUtf8(pidParamInfo(0x05)->name).contains(QStringLiteral("重心")));
    }

    void paramStore_allowsNegativeValues()
    {
        // ★★ 回归守卫（B 侧接口文档 §3.1/§5 第 2 条点名的坑）：
        //   本机电机/编码器符号与厂商约定相反，**负增益是正确的出厂值** ——
        //   前端校验若只允许 ≥0，会把 -80 这种正确值在界面上直接挡掉，
        //   而现象是"调参界面上不让填"，极易被当成界面 bug 而不是"校验写错了"。
        QString why;
        QCOMPARE(int(checkPidValue(0x03, -80.0, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x04, -0.4, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x05, 0.5, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x06, -52.0, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x07, -0.5, &why)), int(ParamCheck::Ok));
        // 本机定稿的七个出厂值必须**全部通过**校验（否则界面一打开就把正确值判成非法）
        const double factory[7] = {300.0, 1.0, -80.0, -0.4, 0.5, 0.0, 0.0};
        for (const PidParamInfo &p : pidParams()) {
            QCOMPARE(int(checkPidValue(p.id, factory[p.id - 1], &why)), int(ParamCheck::Ok));
        }
        // 仍然要挡住真正越界的（允许负值 ≠ 不设界）
        QCOMPARE(int(checkPidValue(0x03, -501.0, &why)), int(ParamCheck::OutOfRange));
        QCOMPARE(int(checkPidValue(0x04, -1.0, &why)), int(ParamCheck::OutOfRange));
    }

    void paramStore_rejectsOutOfRangeAndNaN()
    {
        QString why;
        QCOMPARE(int(checkPidValue(0x01, 300.0, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x01, 1501.0, &why)), int(ParamCheck::OutOfRange));
        QVERIFY(why.contains(QStringLiteral("1501")));
        QCOMPARE(int(checkPidValue(0x05, -20.0, &why)), int(ParamCheck::Ok));
        QCOMPARE(int(checkPidValue(0x05, -21.0, &why)), int(ParamCheck::OutOfRange));
        QCOMPARE(int(checkPidValue(0x01, std::nan(""), &why)), int(ParamCheck::NotANumber));
        QCOMPARE(int(checkPidValue(0x7E, 1.0, &why)), int(ParamCheck::UnknownParam));
    }

    void paramStore_pairsRequestWithReadback()
    {
        ParamStore st;
        QSignalSpy updated(&st, &ParamStore::valueUpdated);
        QSignalSpy mismatch(&st, &ParamStore::mismatch);

        // 未回读前：显示定义里的默认值（★ 已按代码更正：固件 v0xAF 实际生效值就是 +80/+0.4，
        // 因为编译期定稿的 −80/−0.4 被 param_store 的范围校验拒绝，见决策 77）
        QCOMPARE(st.current(0x06), 0.0); // Turn_Kp 定稿 0
        QCOMPARE(st.current(0x03), 80.0); // ★ 板上实际生效值（代码事实）
        QVERIFY(!st.readbackReceived(0x06));
        QVERIFY(st.lastRequestConfirmed(0x06)); // 没下发过 ⇒ 不构成不一致

        st.noteRequest(0x06, 40.0);
        QVERIFY(!st.lastRequestConfirmed(0x06)); // 下发后先标"待回读"
        st.noteReadback(0x06, 40.0);
        QVERIFY(st.readbackReceived(0x06));
        QVERIFY(st.lastRequestConfirmed(0x06));
        QCOMPARE(mismatch.count(), 0);
        QCOMPARE(st.current(0x06), 40.0);

        // 下发 60 但回读仍是 40（固件拒绝/没收到）⇒ 必须报不一致，且**不能用回读值掩盖**
        st.noteRequest(0x06, 60.0);
        st.noteReadback(0x06, 40.0);
        QCOMPARE(mismatch.count(), 1);
        QVERIFY(!st.lastRequestConfirmed(0x06));
        const QList<QVariant> args = mismatch.takeFirst();
        QCOMPARE(args.at(0).toInt(), int(0x06));
        QCOMPARE(args.at(1).toDouble(), 60.0);
        QCOMPARE(args.at(2).toDouble(), 40.0);
        QVERIFY(updated.count() >= 2);

        // 快照历史：下发/回读都要留痕（顺序：下发40 / 回读40 / 下发60 / 回读40）
        QVERIFY(st.history().size() >= 4);
        QCOMPARE(st.history().at(0).source, QStringLiteral("下发"));
        QCOMPARE(st.history().at(1).source, QStringLiteral("回读"));
        QVERIFY(st.history().at(1).ok); // 第一次回读与下发一致
        QCOMPARE(st.history().at(2).source, QStringLiteral("下发"));
        QCOMPARE(st.history().at(3).source, QStringLiteral("回读"));
        QVERIFY(!st.history().at(3).ok); // 第二次回读与下发不一致 → 必须留痕为"失败"
    }

    void paramStore_noReplyTimeout()
    {
        // ★ 真机口径（代码事实）：越界 / NaN / 未知 ID 时固件**整帧不回复**
        //   （app_tasks.c:214 的分支不成立）⇒ 界面必须"超时判失败"，否则会一直等。
        ParamStore st;
        st.noteRequest(0x01, 350.0);
        QVERIFY(!st.requestTimedOut(0x01, 100));          // 刚发出去：不算超时
        QVERIFY(st.requestTimedOut(0x01, 900));           // 超过 800 ms 无回读 ⇒ 判失败
        st.noteReadback(0x01, 350.0);                     // 回读到了
        QVERIFY(st.lastRequestConfirmed(0x01));
        QVERIFY(!st.requestTimedOut(0x01, 5000));         // 已确认 ⇒ 永远不报超时
        QVERIFY(!st.requestTimedOut(0x02, 5000));         // 没下发过 ⇒ 不报
        st.noteRequest(0x02, 1.0);
        QVERIFY(st.requestTimedOut(0x02, st.clockMsForTest() + 1000));
    }

    void paramStore_acksAndSurveyCfg()    {
        ParamStore st;
        st.noteAck(0x00, 0.0); // 保存到 Flash：0.0 = 成功
        QVERIFY(st.history().last().source == QStringLiteral("回执"));
        QVERIFY(st.history().last().ok);

        ParamStore::SurveyCfg cfg;
        cfg.mq2Ppm = 25.0;
        cfg.confirmFrames = 5;
        QSignalSpy changed(&st, &ParamStore::surveyCfgChanged);
        st.setSurveyCfg(cfg);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(st.surveyCfg().mq2Ppm, 25.0);
    }

    // ---------------------------------------------------------------- SQLite 会话库
    void sessionStore_recordsSessionEventAndParam()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("sessions.db"));

        SessionStore store;
        if (!store.open(db)) {
            QSKIP(qPrintable(QStringLiteral("SQLite 不可用，跳过：%1").arg(store.lastError())));
        }
        QVERIFY(store.isAvailable());

        const qint64 sid = store.beginSession(QStringLiteral("单元测试会话"),
                                              QStringLiteral("dummy.csv"));
        QVERIFY(sid > 0);

        EventEntry e;
        e.wall = QDateTime::currentDateTime();
        e.title = QStringLiteral("测试事件");
        e.detail = QStringLiteral("细节");
        e.code = 0x06;
        QVERIFY(store.addEvent(e));
        QVERIFY(store.addParam(QDateTime::currentMSecsSinceEpoch(), 0x06, 40.0,
                               QStringLiteral("下发")));
        QCOMPARE(store.eventCount(), 1);
        QCOMPARE(store.paramCount(), 1);

        QCOMPARE(store.endSession(), sid);
        const auto sessions = store.recentSessions();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().id, sid);
        QVERIFY(sessions.first().note.contains(QStringLiteral("单元测试")));
        QVERIFY(!sessions.first().ended.isEmpty()); // 结束时间已写回

        // 关掉重开：数据要还在（"会话库能跨次运行复盘"）
        {
            SessionStore again;
            QVERIFY(again.open(db));
            QCOMPARE(again.recentSessions().size(), 1);
            QCOMPARE(again.eventCount(), 1);
            QCOMPARE(again.paramCount(), 1);
        }
    }

    void sessionStore_unavailablePathDoesNotCrash()
    {
        SessionStore store;
        // 非法路径：必须返回 false 且后续操作都是安全空操作（不许崩）
        QVERIFY(!store.open(QStringLiteral("Z:/nope/sessions.db")));
        QVERIFY(!store.isAvailable());
        QCOMPARE(store.beginSession(QStringLiteral("x"), QString()), qint64(-1));
        QCOMPARE(store.endSession(), qint64(-1));
        QCOMPARE(store.eventCount(), -1);
        QVERIFY(store.recentSessions().isEmpty());
    }
};

QTEST_MAIN(TestData)
#include "test_data.moc"
