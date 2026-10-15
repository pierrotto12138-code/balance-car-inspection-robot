// ============================================================================
//  P1 界面冒烟测试（**不需要硬件，也不需要真实显示器**：ctest 里用 offscreen 平台）
//
//  它不做"像素级"断言（那种测试既脆又说明不了问题），只守住三条真正会出事的底线：
//    ① **没有数据时不许崩、不许瞎显示**：全部数据源为空时刷新一遍，界面必须活着，
//       并且明确说"等待数据"（绝不能显示 0 —— 0 会被读成"测到的是 0"）；
//    ② **有数据时状态要正确落到界面上**：安全横幅、低压提示、锁存提示都来自
//       domain::VehicleState 的同一处判定，界面不许自己另算一套；
//    ③ **主窗能建、能切页**：三个导航页都要能构造与切换（面板构造函数里的
//       connect/布局写错时，这类测试会第一时间炸出来）。
//
//  跑法：ctest -R ui    或    .\test_ui.exe -platform offscreen
// ============================================================================

#include <QtTest/QtTest>

#include "codec.h"
#include "domain/vehicle_state.h"
#include "io/link_manager.h"
#include "ui/mainwindow.h"
#include "ui/panels/overview_panel.h"

#include "frame_factory.h"

using namespace domain;

class TestUi : public QObject
{
    Q_OBJECT

private slots:
    void overview_survivesWithNoData()
    {
        io::LinkManager mgr;
        VehicleState st;
        OverviewPanel panel(&st, &mgr);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel) || panel.isVisible());

        panel.refresh();
        // 没有任何数据时必须说"等待数据"，而不是把 0 当成测量值显示出来
        QVERIFY2(panel.bannerText().contains(QStringLiteral("等待数据")),
                 qPrintable(QStringLiteral("空数据横幅异常：%1").arg(panel.bannerText())));
    }

    void overview_reflectsStateAndSafety()
    {
        io::LinkManager mgr;
        VehicleState st;
        OverviewPanel panel(&st, &mgr);
        panel.show();
        QTest::qWait(30);

        testframes::feedAll(st);
        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("正常")),
                 qPrintable(QStringLiteral("正常数据下横幅应为正常：%1").arg(panel.bannerText())));

        // 低压：横幅必须说出来，并给出报警线（不能只把数字染黄）
        st.onFrame(io::LinkKind::Wire, proto::up::kDiag, testframes::diag(1095, 0x00));
        panel.refresh();
        QVERIFY(panel.bannerText().contains(QStringLiteral("低压")));

        // 锁存：横幅必须给出"需重下任务解除"这种可操作信息
        st.onFrame(io::LinkKind::Wire, proto::up::kEvent,
                   testframes::event(proto::EventCode::EstopReceived));
        panel.refresh();
        QVERIFY2(panel.bannerText().contains(QStringLiteral("锁存")),
                 qPrintable(QStringLiteral("锁存横幅异常：%1").arg(panel.bannerText())));
    }

    void overview_survivesMalformedAndUnknownFrames()
    {
        io::LinkManager mgr;
        VehicleState st;
        OverviewPanel panel(&st, &mgr);
        panel.show();
        QTest::qWait(30);

        // 长度错的帧 + 协议外命令：界面必须活下来（它们只应体现在"格式错帧"计数上）
        st.onFrame(io::LinkKind::Wire, proto::up::kMotion, QByteArray(11, '\0'));
        st.onFrame(io::LinkKind::Wire, 0x7E, QByteArray(4, '\0'));
        panel.refresh();
        QCOMPARE(st.malformedFrames(), quint64(1));
        QVERIFY(panel.isVisible());
    }

    void mainWindow_buildsAndSwitchesPages()
    {
        io::LinkManager mgr;
        VehicleState st;
        MainWindow w(&mgr, &st);
        w.resize(1200, 800);
        w.show();
        QTest::qWait(50);

        for (int i = 0; i < 3; ++i) {
            w.showPage(i);
            QTest::qWait(20);
            QCOMPARE(w.currentPage(), i);
        }
        // 有数据时切回总览也不能崩
        testframes::feedAll(st);
        w.showPage(0);
        QTest::qWait(80); // 让 20 Hz 定时器真的跑几拍
        QVERIFY(w.isVisible());
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
