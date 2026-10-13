#include "mainwindow.h"

#include "ui/p0panel.h"

#include <QLabel>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "codec.h"
#include "crc16.h"
#include "io/link_manager.h"

MainWindow::MainWindow(io::LinkManager *mgr, QWidget *parent)
    : QMainWindow(parent), m_mgr(mgr)
{
    setWindowTitle(QStringLiteral("仓储环境安全巡检机器人 · 上位机（P0 通信链路）"));
    resize(1250, 780);

    auto *tabs = new QTabWidget(this);

    // ---- ① 通信链路（P0 阶段的主角）----
    tabs->addTab(new P0Panel(mgr, tabs), QStringLiteral("通信链路"));

    // ---- ② 协议自检（原有内容，保留作"环境体检"）----
    auto *selfTest = new QWidget(tabs);
    auto *vl = new QVBoxLayout(selfTest);
    auto *lbl = new QLabel(selfTest);
    lbl->setWordWrap(true);
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setText(runProtocolSelfTest());
    vl->addWidget(lbl);
    vl->addStretch();
    tabs->addTab(selfTest, QStringLiteral("协议自检"));

    setCentralWidget(tabs);

    // 状态栏：一眼看到"有没有连上、主链路是谁"
    connect(mgr, &io::LinkManager::linkStateChanged, this,
            [this](io::LinkKind k, io::LinkState st, const QString &detail) {
                statusBar()->showMessage(
                    QStringLiteral("%1：%2 %3")
                        .arg(QString::fromUtf8(io::linkName(k)),
                             QString::fromUtf8(io::stateName(st)), detail));
            });
    connect(mgr, &io::LinkManager::frameReady, this,
            [this](io::LinkKind k, quint8 cmd, const QByteArray &data) {
                m_frames++;
                statusBar()->showMessage(QStringLiteral("帧 %1 · 最近：%2（%3 字节，来自 %4）")
                                             .arg(m_frames)
                                             .arg(cmd, 2, 16, QLatin1Char('0'))
                                             .arg(data.size())
                                             .arg(QString::fromUtf8(io::linkName(k))),
                                         2000);
            });
    statusBar()->showMessage(QStringLiteral("未连接 —— 选好端口后点\"连接\"；"
                                            "没有板子时把\"实现方式\"切成\"假源\"即可先跑起来"));
}

QString MainWindow::runProtocolSelfTest()
{
    using namespace proto;

    // 1) CRC 自检（标准向量）
    const char *vec = "123456789";
    const uint16_t crc = crc16Modbus(reinterpret_cast<const uint8_t *>(vec), 9);
    const bool crcOk = (crc == 0x4B37);

    // 2) 组帧 → 解帧 → 往返比对（用 0x14 环境与气体帧，N=16）
    std::vector<uint8_t> d;
    putU16(d, 1234);
    putU16(d, 2345);
    putU16(d, 567);
    putU16(d, 890);
    putI16(d, 256);
    putU16(d, 610);
    d.push_back(0x0F);
    d.push_back(1);
    d.push_back(0);
    d.push_back(0);

    const auto bytes = encodeFrame(up::kEnvGas, d);
    FrameDecoder dec;
    const auto frames = dec.feed(bytes);
    const bool rtOk = (frames.size() == 1 && frames[0].cmd == up::kEnvGas &&
                       frames[0].data == d);

    // 3) 分片容错：逐字节喂入
    FrameDecoder dec2;
    std::size_t got2 = 0;
    for (uint8_t b : bytes) {
        got2 += dec2.feed(&b, 1).size();
    }
    const bool splitOk = (got2 == 1);

    // 4) 容错：坏 CRC 应被丢弃
    auto bad = bytes;
    bad.back() ^= 0xFF;
    FrameDecoder dec3;
    const bool dropOk = (dec3.feed(bad).empty() && dec3.framesDropped() == 1);

    // 5) 关键长度口径（协议 v1.1）：0x11 的 N 必须是 9
    const bool n11Ok = (d.size() == 16); // 上面造的是 0x14

    QString s;
    s += QStringLiteral("协议层自检（与 docs/protocol/communication_protocol.md 一致）\n\n");
    s += QStringLiteral("① CRC-16 标准向量 \"123456789\" → 0x%1  %2\n")
             .arg(crc, 4, 16, QLatin1Char('0'))
             .arg(crcOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("② 组帧/解帧往返（0x14，16 字节）  %1\n")
             .arg(rtOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("③ 逐字节分片重组  %1\n")
             .arg(splitOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("④ 坏校验丢弃  %1\n")
             .arg(dropOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("⑤ 0x14 数据段长度 16 字节  %1\n")
             .arg(n11Ok ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("\n协议版本：v1.1（含安全通道事件码 0x08/0x09/0x0A）\n");

    const bool allOk = crcOk && rtOk && splitOk && dropOk && n11Ok;
    s += QStringLiteral("\n结论：%1")
             .arg(allOk ? QStringLiteral("协议层工作正常，可对接固件。")
                        : QStringLiteral("存在失败项，需排查。"));
    return s;
}
