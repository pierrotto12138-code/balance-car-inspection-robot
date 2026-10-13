#include "ui/p0panel.h"

#include "io/link_manager.h"

#include "codec.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 十六进制串（每字节两字符，空格分隔）
QString toHex(const QByteArray &b, int maxBytes = 64)
{
    const int n = qMin(b.size(), maxBytes);
    QString s;
    for (int i = 0; i < n; ++i) {
        s += QStringLiteral("%1 ").arg(static_cast<quint8>(b.at(i)), 2, 16, QLatin1Char('0')).toUpper();
    }
    if (b.size() > n) {
        s += QStringLiteral("… (+%1 字节)").arg(b.size() - n);
    }
    return s.trimmed();
}

const char *cmdName(quint8 cmd)
{
    switch (cmd) {
    case proto::up::kPose:
        return "0x10 姿态";
    case proto::up::kMotion:
        return "0x11 运动";
    case proto::up::kTask:
        return "0x12 任务";
    case proto::up::kEvent:
        return "0x13 事件";
    case proto::up::kEnvGas:
        return "0x14 环境气体";
    case proto::up::kHeartbeat:
        return "0x1F 心跳";
    default:
        return "保留/未知";
    }
}

QColor stateColor(io::LinkState st)
{
    switch (st) {
    case io::LinkState::Open:
        return QColor(0x3F, 0xA4, 0x5B); // 正常运行绿
    case io::LinkState::Opening:
        return QColor(0xE0, 0xA8, 0x00); // 过渡态琥珀
    case io::LinkState::Error:
        return QColor(0xD9, 0x53, 0x4F); // 错误红
    case io::LinkState::Closed:
        break;
    }
    return QColor(0x9A, 0xA3, 0xAD); // 中性灰（未连接/未知）
}

} // namespace

P0Panel::P0Panel(io::LinkManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    auto *root = new QVBoxLayout(this);

    // ---------------- 两条链路的卡片 ----------------
    auto *cards = new QHBoxLayout;
    cards->addWidget(buildLinkGroup(io::LinkKind::Wire), 1);
    cards->addWidget(buildLinkGroup(io::LinkKind::Bluetooth), 1);
    root->addLayout(cards);

    // ---------------- 全局控制 ----------------
    auto *ctrl = new QGroupBox(QStringLiteral("假源 / 全局"), this);
    auto *cl = new QHBoxLayout(ctrl);

    m_primary = new QLabel(ctrl);
    cl->addWidget(m_primary);
    cl->addSpacing(12);

    cl->addWidget(new QLabel(QStringLiteral("速率倍数"), ctrl));
    m_rate = new QDoubleSpinBox(ctrl);
    m_rate->setRange(0.1, 100.0);
    m_rate->setSingleStep(0.5);
    m_rate->setValue(1.0);
    m_rate->setToolTip(QStringLiteral("1.0 = 按协议真实周期；10.0 用于压测界面是否卡顿"));
    cl->addWidget(m_rate);

    cl->addWidget(new QLabel(QStringLiteral("坏帧比例"), ctrl));
    m_corrupt = new QDoubleSpinBox(ctrl);
    m_corrupt->setRange(0.0, 1.0);
    m_corrupt->setSingleStep(0.05);
    m_corrupt->setDecimals(2);
    m_corrupt->setValue(0.0);
    m_corrupt->setToolTip(QStringLiteral("故意破坏 CRC 的比例，用来验证\"丢帧计数\"确实在工作"));
    cl->addWidget(m_corrupt);

    m_resetCounters = new QPushButton(QStringLiteral("重置计数"), ctrl);
    cl->addWidget(m_resetCounters);

    m_pauseLog = new QCheckBox(QStringLiteral("暂停日志"), ctrl);
    cl->addWidget(m_pauseLog);

    m_clearLog = new QPushButton(QStringLiteral("清空日志"), ctrl);
    cl->addWidget(m_clearLog);
    cl->addStretch();
    root->addWidget(ctrl);

    // ---------------- 报文日志（HEX 流水）----------------
    auto *logBox = new QGroupBox(QStringLiteral("报文日志（收 / 发 十六进制流水）"), this);
    auto *ll = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000); // 有上限，长时间运行不会吃内存
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    m_log->setFont(mono);
    ll->addWidget(m_log);
    root->addWidget(logBox, 1);

    // ---------------- 信号 ----------------
    connect(mgr, &io::LinkManager::linkStateChanged, this, &P0Panel::onLinkState);
    connect(mgr, &io::LinkManager::rawBytes, this, &P0Panel::onRawBytes);
    connect(mgr, &io::LinkManager::frameReady, this, &P0Panel::onFrame);
    connect(mgr, &io::LinkManager::primaryChanged, this, [this](io::LinkKind k) {
        m_primary->setText(k == io::LinkKind::Count
                               ? QStringLiteral("主链路：无（未连接）")
                               : QStringLiteral("主链路：%1").arg(QString::fromUtf8(io::linkName(k))));
    });
    connect(mgr, &io::LinkManager::linkStatsChanged, this, [this] { refreshStats(); });

    connect(m_resetCounters, &QPushButton::clicked, this, &P0Panel::onResetCounters);
    connect(m_clearLog, &QPushButton::clicked, this, &P0Panel::onClearLog);
    connect(m_rate, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { m_mgr->setReplayRate(v); });
    connect(m_corrupt, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { m_mgr->setReplayCorruptRatio(v); });

    // 每秒刷新一次帧率（用差分算，比"每帧刷新"稳）
    m_refresh = new QTimer(this);
    m_refresh->setInterval(1000);
    connect(m_refresh, &QTimer::timeout, this, [this] {
        for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
            const quint64 now = m_mgr->stats(static_cast<io::LinkKind>(i)).framesOk;
            m_rateFps[i] = static_cast<double>(now - m_lastFrames[i]);
            m_lastFrames[i] = now;
        }
        refreshStats();
    });
    m_refresh->start();

    // 初始状态
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        onLinkState(static_cast<io::LinkKind>(i), io::LinkState::Closed, QString());
    }
    refreshStats();
}

QWidget *P0Panel::buildLinkGroup(io::LinkKind kind)
{
    const int i = static_cast<int>(kind);
    auto *box = new QGroupBox(QString::fromUtf8(io::linkName(kind)), this);
    auto *form = new QFormLayout(box);

    m_ui[i].mode = new QComboBox(box);
    m_ui[i].mode->addItem(QStringLiteral("真实串口"), static_cast<int>(io::LinkManager::Mode::Serial));
    m_ui[i].mode->addItem(QStringLiteral("假源（离线/演示）"),
                          static_cast<int>(io::LinkManager::Mode::Replay));
    form->addRow(QStringLiteral("实现方式"), m_ui[i].mode);

    m_ui[i].port = new QComboBox(box);
    m_ui[i].port->setEditable(true);
    form->addRow(QStringLiteral("端口"), m_ui[i].port);

    m_ui[i].baud = new QComboBox(box);
    for (int b : {9600, 19200, 38400, 57600, 115200, 230400}) {
        m_ui[i].baud->addItem(QString::number(b), b);
    }
    m_ui[i].baud->setCurrentText(QStringLiteral("115200"));
    form->addRow(QStringLiteral("波特率"), m_ui[i].baud);

    auto *row = new QHBoxLayout;
    m_ui[i].connect = new QPushButton(QStringLiteral("连接"), box);
    m_ui[i].state = new QLabel(box);
    row->addWidget(m_ui[i].connect);
    row->addWidget(m_ui[i].state, 1);
    form->addRow(row);

    m_ui[i].stats = new QLabel(box);
    m_ui[i].stats->setTextFormat(Qt::RichText);
    m_ui[i].stats->setWordWrap(true);
    form->addRow(QStringLiteral("统计"), m_ui[i].stats);

    connect(m_ui[i].mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, kind] { onModeChanged(kind); });
    connect(m_ui[i].connect, &QPushButton::clicked, this, [this, kind] { onConnectClicked(kind); });

    refreshPorts(kind);
    onModeChanged(kind);
    return box;
}

void P0Panel::refreshPorts(io::LinkKind kind)
{
    const int i = static_cast<int>(kind);
    QComboBox *cb = m_ui[i].port;
    const QString keep = cb->currentText();
    cb->clear();
    const QStringList ports = io::LinkManager::availablePorts();
    if (ports.isEmpty()) {
        cb->addItem(QStringLiteral("（未检测到串口）"));
    } else {
        cb->addItems(ports);
    }
    if (!keep.isEmpty()) {
        cb->setCurrentText(keep);
    }
}

void P0Panel::onModeChanged(io::LinkKind kind)
{
    const int i = static_cast<int>(kind);
    const auto mode = static_cast<io::LinkManager::Mode>(m_ui[i].mode->currentData().toInt());
    m_ui[i].port->setEnabled(mode == io::LinkManager::Mode::Serial);
    m_ui[i].baud->setEnabled(mode == io::LinkManager::Mode::Serial);
    m_mgr->setMode(kind, mode);
    refreshPorts(kind);
}

void P0Panel::onConnectClicked(io::LinkKind kind)
{
    const int i = static_cast<int>(kind);
    if (m_mgr->isOpen(kind)) {
        m_mgr->close(kind);
        m_ui[i].connect->setText(QStringLiteral("连接"));
        return;
    }

    const auto mode = static_cast<io::LinkManager::Mode>(m_ui[i].mode->currentData().toInt());
    if (mode == io::LinkManager::Mode::Serial) {
        io::SerialConfig cfg = m_mgr->serialConfig(kind);
        cfg.portName = m_ui[i].port->currentText().section(QLatin1Char(' '), 0, 0);
        cfg.baudRate = m_ui[i].baud->currentData().toInt();
        m_mgr->setSerialConfig(kind, cfg);
        if (cfg.portName.isEmpty() || cfg.portName.startsWith(QStringLiteral("（"))) {
            appendLog(QStringLiteral("[%1] %2：未选择串口").arg(
                QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                QString::fromUtf8(io::linkName(kind))));
            return;
        }
    }
    m_mgr->open(kind);
    m_ui[i].connect->setText(QStringLiteral("断开"));
}

void P0Panel::onLinkState(io::LinkKind kind, io::LinkState state, const QString &detail)
{
    const int i = static_cast<int>(kind);
    m_ui[i].state->setText(QStringLiteral("<span style='color:%1'>● %2</span> %3")
                               .arg(stateColor(state).name(),
                                    QString::fromUtf8(io::stateName(state)), detail.toHtmlEscaped()));
    m_ui[i].connect->setText(m_mgr->isOpen(kind) ? QStringLiteral("断开")
                                                 : QStringLiteral("连接"));
}

void P0Panel::onRawBytes(io::LinkKind kind, const QByteArray &bytes)
{
    appendLog(QStringLiteral("[%1] %2 RX  %3")
                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                       QString::fromUtf8(io::linkName(kind)), toHex(bytes)));
}

void P0Panel::onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data)
{
    // 帧级日志只记摘要（高频帧不刷屏）；完整字节在 rawBytes 里已经能看到
    static QHash<int, qint64> lastLog;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastLog.value(cmd) < 1000) {
        return;
    }
    lastLog[cmd] = now;
    appendLog(QStringLiteral("[%1] %2 帧 %3  N=%4  %5")
                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                       QString::fromUtf8(io::linkName(kind)),
                       QString::fromUtf8(cmdName(cmd)))
                  .arg(data.size())
                  .arg(toHex(data, 16)));
}

void P0Panel::appendLog(const QString &line)
{
    if (m_pauseLog->isChecked()) {
        return;
    }
    m_log->appendPlainText(line);
}

void P0Panel::refreshStats()
{
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        const auto kind = static_cast<io::LinkKind>(i);
        const io::LinkStats s = m_mgr->stats(kind);
        m_ui[i].stats->setText(
            QStringLiteral("收 <b>%1</b> B · 发 <b>%2</b> B<br/>解帧 <b>%3</b> · 丢帧 <b>%4</b>"
                           "（%5%）· 重连 %6<br/>帧率 <b>%7</b> 帧/s")
                .arg(s.bytesRx)
                .arg(s.bytesTx)
                .arg(s.framesOk)
                .arg(s.framesDropped)
                .arg(QString::number(s.dropRate() * 100.0, 'f', 2))
                .arg(s.reconnects)
                .arg(QString::number(m_rateFps[i], 'f', 1)));
    }
}

void P0Panel::onResetCounters()
{
    // 重新建链路即可清零（计数由链路自身持有，单一来源）
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        const auto kind = static_cast<io::LinkKind>(i);
        const bool wasOpen = m_mgr->isOpen(kind);
        m_mgr->close(kind);
        m_lastFrames[i] = 0;
        m_rateFps[i] = 0.0;
        if (wasOpen) {
            m_mgr->open(kind);
        }
    }
    appendLog(QStringLiteral("[%1] 计数已重置").arg(
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
    refreshStats();
}

void P0Panel::onClearLog()
{
    m_log->clear();
}

double P0Panel::frameRate(io::LinkKind kind) const
{
    return m_rateFps[static_cast<int>(kind)];
}
