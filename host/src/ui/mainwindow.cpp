#include "mainwindow.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "codec.h"
#include "crc16.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("仓储环境安全巡检机器人 · 上位机（骨架）"));
    resize(900, 600);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    auto *title = new QLabel(QStringLiteral("上位机骨架 — 待实现面板："), central);
    QFont f = title->font();
    f.setPointSize(12);
    f.setBold(true);
    title->setFont(f);

    auto *todo = new QLabel(
        QStringLiteral(
            "① 通信接口面板（串口选择 / 连接状态 / 丢帧率）\n"
            "② 状态监测面板（姿态三通道曲线、轮速、任务进度）\n"
            "③ 气体与环境面板（双路浓度曲线、阈值线、超限高亮）\n"
            "④ 远程控制面板（运动控制、任务启停、巡检参数、急停）\n"
            "⑤ 事件日志面板（告警滚动、历史回放、CSV 导出）"),
        central);

    m_statusLabel = new QLabel(central);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(title);
    layout->addWidget(todo);
    layout->addSpacing(12);
    layout->addWidget(m_statusLabel);
    layout->addStretch();
    setCentralWidget(central);

    m_statusLabel->setText(runProtocolSelfTest());
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

    QString s;
    s += QStringLiteral("协议层自检（与 docs/protocol 第 2.1 节一致）\n\n");
    s += QStringLiteral("① CRC-16 标准向量 \"123456789\" → 0x%1  %2\n")
             .arg(crc, 4, 16, QLatin1Char('0'))
             .arg(crcOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("② 组帧/解帧往返（0x14，16 字节）  %1\n")
             .arg(rtOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("③ 逐字节分片重组  %1\n")
             .arg(splitOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("④ 坏校验丢弃  %1\n")
             .arg(dropOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));

    const bool allOk = crcOk && rtOk && splitOk && dropOk;
    s += QStringLiteral("\n结论：%1")
             .arg(allOk ? QStringLiteral("协议层工作正常，可开始对接固件。")
                        : QStringLiteral("存在失败项，需排查。"));
    return s;
}
