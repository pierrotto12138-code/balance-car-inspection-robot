#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

#include "codec.h" // 事件码常量（自检里识别 0x13）
#include "io/link_manager.h"
#include "ui/mainwindow.h"

// ============================================================================
//  程序入口：只做"装配"这一件事（依赖由外向内注入）
//    LinkManager（io 层）→ MainWindow（ui 层）
//  io 层不知道 ui 的存在，ui 层通过信号槽接收解好的帧 —— 依赖单向。
//
//  支持一个无界面自检开关（便于在没有屏幕/无人值守时验证整机可跑）：
//      host.exe --selftest [秒数]
//    它会：两条链路都切到假源 → 打开 → 跑指定秒数 → 打印统计 → 退出
//    退出码 0 = 收到帧且无丢帧；非 0 = 有异常。
//    用法举例：host.exe --selftest 3 -platform offscreen
// ============================================================================
namespace {

int runSelfTest(double seconds)
{
    io::LinkManager mgr;
    mgr.setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
    mgr.setMode(io::LinkKind::Bluetooth, io::LinkManager::Mode::Replay);
    mgr.setReplayRate(1.0);

    quint64 frames = 0;
    quint64 events = 0;
    QObject::connect(&mgr, &io::LinkManager::frameReady,
                     [&frames, &events](io::LinkKind, quint8 cmd, const QByteArray &) {
                         ++frames;
                         if (cmd == proto::up::kEvent) {
                             ++events;
                         }
                     });

    mgr.openAll();

    QTextStream out(stdout);
    out << QStringLiteral("自检中：假源 ×1.0，跑 %1 s（协议周期 0x10/0x11 20 Hz…）\n")
               .arg(seconds, 0, 'f', 1);
    out.flush();

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
    out << QStringLiteral("  合计 %1 帧（其中事件帧 %2）\n").arg(frames).arg(events);
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

    io::registerLinkMetaTypes();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));
    parser.addHelpOption();
    QCommandLineOption selftest(QStringLiteral("selftest"),
                                QStringLiteral("无界面自检：假源跑 N 秒后打印统计并退出"),
                                QStringLiteral("秒"), QStringLiteral("3"));
    parser.addOption(selftest);
    parser.process(app);

    if (parser.isSet(selftest)) {
        return runSelfTest(parser.value(selftest).toDouble());
    }

    io::LinkManager manager;
    MainWindow w(&manager);
    w.show();
    return app.exec();
}
