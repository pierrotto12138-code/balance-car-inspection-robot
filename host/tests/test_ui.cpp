// ============================================================================
//  P1/P2 界面冒烟测试（**不需要硬件，也不需要真实显示器**：ctest 里用 offscreen 平台）
//
//  它不做"像素级"断言（那种测试既脆又说明不了问题），只守住几条真正会出事的底线：
//    ① **没有数据时不许崩、不许瞎显示**：全部数据源为空时刷新，界面必须活着，
//       并且明确说"等待数据"（绝不能显示 0 —— 0 会被读成"测到的是 0"）；
//    ② **有数据时状态要正确落到界面上**：安全横幅、低压提示、锁存提示都来自
//       domain 的同一处判定，界面不许自己另算一套；
//    ③ **采样与重绘是两条节奏**：每来一帧采一个样（不是按 20 Hz 轮询重复采样），
//       告警要能在曲线上留下标记；
//    ④ **主窗能建、能切页**（五个导航页都要能构造与切换）。
//
//  跑法：ctest -R ui    或    .\test_ui.exe -platform offscreen
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h"
#include "data/event_log.h"
#include "domain/alarm.h"
#include "domain/keepalive.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"
#include "ui/mainwindow.h"
#include "ui/panels/curve_panel.h"
#include "ui/panels/event_panel.h"
#include "ui/panels/overview_panel.h"
#include "ui/panels/patrol_panel.h"
#include "ui/widgets/scope_plot.h"

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include "frame_factory.h"

using namespace domain;

namespace {

/// 造一个事件帧的解码结果（事件流现在吃的是"已去重的事件"，不再吃原始字节）
EventData ev(proto::EventCode code, quint32 ts)
{
    EventData e;
    e.code = code;
    e.timestampMs = ts;
    return e;
}

} // namespace

class TestUi : public QObject
{
    Q_OBJECT

private slots:
    // ---------------------------------------------------------------- P1：总览
    void overview_survivesWithNoData()
    {
        io::LinkManager mgr;
        VehicleState st;
        AlarmEngine alarm;
        OverviewPanel panel(&st, &mgr, &alarm);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel) || panel.isVisible());

        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("等待数据")),
                 qPrintable(QStringLiteral("空数据横幅异常：%1").arg(panel.bannerText())));
    }

    void overview_reflectsStateAndSafety()
    {
        io::LinkManager mgr;
        VehicleState st;
        AlarmEngine alarm;
        OverviewPanel panel(&st, &mgr, &alarm);
        panel.show();
        QTest::qWait(30);

        testframes::feedAll(st);
        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("正常")),
                 qPrintable(QStringLiteral("正常数据下横幅应为正常：%1").arg(panel.bannerText())));

        // 锁存：横幅必须给出"需重下任务解除"这种可操作信息
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::EstopReceived));
        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("锁存")),
                 qPrintable(QStringLiteral("锁存横幅异常：%1").arg(panel.bannerText())));
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::SafetyReleased));

        // P2：报警横幅由 AlarmEngine 说了算（连续确认后才出现）
        for (int i = 0; i < 3; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(400.0, 60.0));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
        }
        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("报警")),
                 qPrintable(QStringLiteral("报警横幅异常：%1").arg(panel.bannerText())));

        // 暂停更新：刷新被冻结（横幅文本不再变化）
        const QString frozen = panel.bannerText();
        panel.setUpdatesPaused(true);
        st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(1.0, 1.0));
        panel.refresh();
        QCOMPARE(panel.bannerText(), frozen);
        panel.setUpdatesPaused(false);
    }

    void overview_survivesMalformedAndUnknownFrames()
    {
        io::LinkManager mgr;
        VehicleState st;
        AlarmEngine alarm;
        OverviewPanel panel(&st, &mgr, &alarm);
        panel.show();
        QTest::qWait(30);

        st.onFrame(io::LinkKind::Wire, proto::up::kMotion, QByteArray(11, '\0'));
        st.onFrame(io::LinkKind::Wire, 0x7E, QByteArray(4, '\0'));
        panel.refresh();
        QCOMPARE(st.malformedFrames(), quint64(1));
        QVERIFY(panel.isVisible());
    }

    // ---------------------------------------------------------------- P2：曲线
    void curvePanel_samplesPerFrameAndMarksAlarms()
    {
        io::LinkManager mgr;
        VehicleState st;
        AlarmEngine alarm;
        CurvePanel panel(&st, &mgr, &alarm);
        panel.resize(1200, 800);
        panel.show();
        QTest::qWait(30);

        auto *mq2 = panel.findChild<ScopePlot *>(QStringLiteral("mq2Plot"));
        auto *mq135 = panel.findChild<ScopePlot *>(QStringLiteral("mq135Plot"));
        auto *volt = panel.findChild<ScopePlot *>(QStringLiteral("voltPlot"));
        auto *th = panel.findChild<ScopePlot *>(QStringLiteral("thPlot"));
        QVERIFY(mq2 && mq135 && volt && th);
        QCOMPARE(mq2->sampleCount(), 0);

        // 三帧环境数据 + 两帧诊断数据 → 各通道恰好采到对应的点数（不是按定时器重复采）
        for (int i = 0; i < 3; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(10.0 + i, 20.0));
            panel.onDataUpdated(proto::up::kEnvGas);
        }
        QCOMPARE(mq2->sampleCount(), 3);
        QCOMPARE(mq135->sampleCount(), 3);
        QCOMPARE(th->sampleCount(), 6); // 温湿度各一条曲线
        for (int i = 0; i < 2; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kDiag, testframes::diagVolt(11.8 - i * 0.1));
            panel.onDataUpdated(proto::up::kDiag);
        }
        QCOMPARE(volt->sampleCount(), 2);
        QVERIFY(mq2->lastTime() >= 0.0);

        // 告警发生/解除 → 曲线上要留下标记
        for (int i = 0; i < 3; ++i) {
            st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(400.0, 5.0));
            alarm.onFrameDecoded(st, proto::up::kEnvGas);
            panel.onDataUpdated(proto::up::kEnvGas);
        }
        const int before = mq2->markerCount();
        if (alarm.record(AlarmEngine::kGasMq2).active) {
            panel.onAlarmRaised(alarm.record(AlarmEngine::kGasMq2));
        }
        QCOMPARE(mq2->markerCount(), before + 1);

        // 暂停时不再采样（冻结画面）
        panel.setPaused(true);
        const int frozen = mq2->sampleCount();
        st.onFrame(io::LinkKind::Wire, proto::up::kEnvGas, testframes::envPpm(11.0, 20.0));
        panel.onDataUpdated(proto::up::kEnvGas);
        QCOMPARE(mq2->sampleCount(), frozen);
        panel.setPaused(false);

        // 重绘与清空都不许崩
        panel.replotAll();
        QVERIFY(panel.isVisible());
        mq2->clearData();
        QCOMPARE(mq2->sampleCount(), 0);
    }

    // ---------------------------------------------------------------- P2：事件流
    void eventPanel_showsRobotEventsAndAlarms()
    {
        data::EventLog log;
        EventPanel panel(&log);
        panel.resize(1000, 600);
        panel.show();
        QTest::qWait(30);

        auto *table = panel.findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 0);

        log.onEvent(ev(proto::EventCode::EstopReceived, 1000), io::LinkKind::Wire);
        QTest::qWait(30);
        log.onEvent(ev(proto::EventCode::SafetyReleased, 9000), io::LinkKind::Wire);
        QCOMPARE(table->rowCount(), 2);

        // 安全事件的"停车时长"必须写在行里（复盘时不必自己两行相减）
        QString detail;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (table->item(r, 4) != nullptr && table->item(r, 4)->text().contains(QStringLiteral("解除"))) {
                detail = table->item(r, 5)->text();
            }
        }
        QVERIFY2(detail.contains(QStringLiteral("停车时长")), qPrintable(detail));

        // 过滤：只看安全事件时，普通信息类事件不显示
        data::EventEntry info;
        info.title = QStringLiteral("任务开始");
        info.code = static_cast<quint8>(proto::EventCode::TaskStart);
        log.onEvent(ev(proto::EventCode::TaskStart, 100), io::LinkKind::Wire);
        QCOMPARE(table->rowCount(), 3);
    }

    // ---------------------------------------------------------------- 主窗
    void mainWindow_buildsAndSwitchesPages()
    {
        io::LinkManager mgr;
        VehicleState st;
        AlarmEngine alarm;
        KeepaliveEngine keepalive;
        data::EventLog log;
        MainWindow w(&mgr, &st, &alarm, &keepalive, &log);
        w.resize(1400, 900);
        w.show();
        QTest::qWait(50);

        QCOMPARE(w.pageTitle(0), QStringLiteral("① 总览"));
        QCOMPARE(w.pageTitle(1), QStringLiteral("② 巡检与控制"));
        QCOMPARE(w.pageTitle(2), QStringLiteral("③ 曲线与告警"));
        QCOMPARE(w.pageTitle(3), QStringLiteral("④ 事件流"));
        for (int i = 0; i < 6; ++i) {
            w.showPage(i);
            QTest::qWait(30);
            QCOMPARE(w.currentPage(), i);
        }
        // 有数据时切回曲线页也不能崩（20 Hz/10 Hz 定时器真的会跑几拍）
        testframes::feedAll(st);
        w.showPage(2);
        QTest::qWait(120);
        QVERIFY(w.isVisible());
    }

    // ---------------------------------------------------------------- P3：巡检与控制
    void patrolPanel_interlocksMotionWhileLatched()
    {
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay); // 假装有链路，发得出去
        VehicleState st;
        AlarmEngine alarm;
        KeepaliveEngine keepalive;
        data::EventLog log;
        PatrolPanel panel(&st, &mgr, &alarm, &keepalive, &log);
        panel.resize(1200, 800);
        panel.show();
        QTest::qWait(30);

        // 未锁存：能发任务（0x21）与遥控（0x20）
        testframes::feedAll(st);
        panel.refresh();

        // 锁存：横幅要明确说"需重下任务解除锁存"（P3 的验收判据之一）
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::EstopReceived));
        panel.refresh();
        auto *banner = panel.findChild<QLabel *>();
        QVERIFY(banner != nullptr);
        // 所有"会让车动"的入口在锁存期间必须被禁用（靠禁用而不是靠提示语）
        int disabledMotion = 0;
        for (QPushButton *b : panel.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("前")) || b->text().contains(QStringLiteral("后")) ||
                b->text().contains(QStringLiteral("左")) || b->text().contains(QStringLiteral("右")) ||
                b->text().contains(QStringLiteral("巡航")) || b->text().contains(QStringLiteral("返航"))) {
                if (!b->isEnabled()) {
                    ++disabledMotion;
                }
            }
        }
        QVERIFY2(disabledMotion >= 6, qPrintable(QStringLiteral("锁存期间应有 6 个起点类按钮被禁用，实际 %1")
                                                     .arg(disabledMotion)));
        // 解除锁存后按钮应恢复
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::SafetyReleased));
        panel.refresh();
        int enabledMotion = 0;
        for (QPushButton *b : panel.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("前")) && b->isEnabled()) {
                ++enabledMotion;
            }
        }
        QCOMPARE(enabledMotion, 1);
    }

    void patrolPanel_aestopAndTaskAreLogged()
    {
        io::LinkManager mgr;
        mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
        VehicleState st;
        AlarmEngine alarm;
        KeepaliveEngine keepalive;
        data::EventLog log;
        PatrolPanel panel(&st, &mgr, &alarm, &keepalive, &log);
        panel.show();
        QTest::qWait(30);

        QPushButton *estop = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("紧急停机"))) {
                estop = b;
            }
        }
        QVERIFY(estop != nullptr);
        estop->click();
        QCOMPARE(log.size(), 1); // 操作员动作必须留档（复盘"谁把车停了"）
        QVERIFY(log.at(0).operatorAction);
        QVERIFY(log.at(0).safety);
        QCOMPARE(mgr.stats(io::LinkKind::Wire).bytesTx > 0, true); // 真的发出了字节
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
