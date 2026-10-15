#include "io/serial_link.h"

#include "codec.h"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>
#include <QVector>

#include <atomic>

namespace io {

// ---------------------------------------------------------------------------
//  工作线程里的读取者
// ---------------------------------------------------------------------------
class SerialLink::Reader : public QObject
{
    Q_OBJECT
public:
    explicit Reader(SerialConfig cfg) : m_cfg(std::move(cfg)) {}

    /// 请求停止（线程安全：只置一个原子标志，读取循环 ≤10 ms 内会看到）
    void requestStop() { m_stop.store(true); }

    /// 排入一帧待发字节（线程安全）。
    ///
    /// ★ 为什么用"写队列"而不是 `QMetaObject::invokeMethod`：
    ///   本线程的 `run()` 里是**阻塞读循环**，事件循环根本不转 ——
    ///   排队调用永远送不进来。所以发送侧走互斥锁保护的队列，
    ///   由读取循环每轮取走并写出（≤10 ms 延迟）。
    ///   这与固件侧"单条 TX 互斥"是同一条思路：**同一时刻只有一个写者**。
    void queueWrite(const QByteArray &bytes)
    {
        QMutexLocker lock(&m_txMutex);
        m_txQueue.push_back(bytes);
    }

    void setStopForRestart() { m_stop.store(false); }

public slots:
    /// 在工作线程里执行：建端口 → 阻塞读 → 解帧 → 出错则按策略重连
    void run();

signals:
    void chunkReady(const QByteArray &bytes);
    void frameReadyRaw(quint8 cmd, const QByteArray &data);
    void stateRaw(int state, const QString &detail);
    void statsRaw(quint64 bytesRx, quint64 bytesTx, quint64 framesOk, quint64 framesDropped,
                  quint64 reconnects);
    void finished();

private:
    bool openPort(QSerialPort &port, QString *err);
    void sleepMs(int ms); // 可被 requestStop 打断的睡眠

    SerialConfig m_cfg;
    std::atomic<bool> m_stop{false};
    QMutex m_txMutex;
    QVector<QByteArray> m_txQueue;
    quint64 m_bytesRx = 0;
    quint64 m_bytesTx = 0;
    quint64 m_reconnects = 0;
    proto::FrameDecoder m_decoder;
};

void SerialLink::Reader::sleepMs(int ms)
{
    // 分片睡眠：stop 请求最多等 25 ms 就能生效
    for (int waited = 0; waited < ms && !m_stop.load(); waited += 25) {
        QThread::msleep(static_cast<unsigned long>(qMin(25, ms - waited)));
    }
}

bool SerialLink::Reader::openPort(QSerialPort &port, QString *err)
{
    port.setPortName(m_cfg.portName);
    port.setBaudRate(m_cfg.baudRate);
    port.setDataBits(QSerialPort::Data8);
    port.setParity(QSerialPort::NoParity);
    port.setStopBits(QSerialPort::OneStop);
    port.setFlowControl(QSerialPort::NoFlowControl);

    if (!port.open(QIODevice::ReadWrite)) {
        if (err) {
            *err = QStringLiteral("无法打开 %1（%2）—— 端口被占用？请先关闭 FlyMcu / 串口助手")
                       .arg(m_cfg.portName, port.errorString());
        }
        return false;
    }

    // ★ 必须显式驱动 DTR/RTS（2026-10-13 真机联调踩到，见 AGENTS 陷阱 33）：
    //   本板 CH340G 的自动下载电路把 **DTR 接复位、RTS 接 BOOT0**
    //   （厂商 ISP 工具原文：『DTR 的低电平复位，RTS 高电平进 BootLoader』）。
    //   串口一被打开，驱动默认把两者都拉高 → **RTS 高 = MCU 停在 BootLoader** →
    //   现象是"端口能开、但一个字节都收不到"，极容易误判为"没烧进去 / 线接错"。
    //   运行态：DTR = 高（释放复位）、RTS = 低（不进 BootLoader）。
    //   实测：DTR 必须为高，否则 MCU 一直处于复位。
    port.setDataTerminalReady(true);
    port.setRequestToSend(false);
    QThread::msleep(120);   // 等复位释放、应用起来再开始读
    port.clear(QSerialPort::AllDirections);
    return true;
}

void SerialLink::Reader::run()
{
    QSerialPort port;

    while (!m_stop.load()) {
        QString err;
        emit stateRaw(static_cast<int>(LinkState::Opening),
                      QStringLiteral("打开 %1 @ %2").arg(m_cfg.portName).arg(m_cfg.baudRate));
        if (!openPort(port, &err)) {
            emit stateRaw(static_cast<int>(LinkState::Error), err);
            if (!m_cfg.autoReconnect) {
                break;
            }
            sleepMs(m_cfg.reconnectDelayMs);
            continue;
        }

        ++m_reconnects;
        err.clear();
        emit stateRaw(static_cast<int>(LinkState::Open),
                      QStringLiteral("%1 @ %2").arg(m_cfg.portName).arg(m_cfg.baudRate));
        m_decoder.reset(); // 重连后重新找帧边界

        // ---- 阻塞读循环 ----
        while (!m_stop.load() && port.isOpen()) {
            // 1) 先把待发队列清空（写与读在同一线程，天然串行）
            {
                QVector<QByteArray> pending;
                {
                    QMutexLocker lock(&m_txMutex);
                    pending.swap(m_txQueue);
                }
                for (const QByteArray &b : pending) {
                    const qint64 n = port.write(b);
                    if (n > 0) {
                        m_bytesTx += static_cast<quint64>(n);
                    }
                }
            }

            // 2) 读（10 ms 超时：既能让上面的发送及时出去，又不空转烧 CPU）
            if (!port.waitForReadyRead(10)) {
                if (port.error() != QSerialPort::NoError &&
                    port.error() != QSerialPort::TimeoutError) {
                    err = QStringLiteral("读取错误：%1").arg(port.errorString());
                    break;
                }
                continue;
            }

            const QByteArray chunk = port.readAll();
            if (chunk.isEmpty()) {
                continue;
            }
            m_bytesRx += static_cast<quint64>(chunk.size());
            emit chunkReady(chunk); // 给"报文日志"面板

            // 3) 解帧（在工作线程里做，界面只收成品帧）
            const auto frames = m_decoder.feed(reinterpret_cast<const uint8_t *>(chunk.constData()),
                                               static_cast<std::size_t>(chunk.size()));
            for (const proto::Frame &f : frames) {
                emit frameReadyRaw(static_cast<quint8>(f.cmd),
                                   QByteArray(reinterpret_cast<const char *>(f.data.data()),
                                              static_cast<int>(f.data.size())));
            }
            emit statsRaw(m_bytesRx, m_bytesTx, m_decoder.framesOk(), m_decoder.framesDropped(),
                          m_reconnects);
        }

        port.close();
        if (!err.isEmpty()) {
            emit stateRaw(static_cast<int>(LinkState::Error), err);
        }
        if (!m_stop.load() && m_cfg.autoReconnect) {
            sleepMs(m_cfg.reconnectDelayMs);
            continue;
        }
        break;
    }

    emit stateRaw(static_cast<int>(LinkState::Closed), QString());
    emit statsRaw(m_bytesRx, m_bytesTx, m_decoder.framesOk(), m_decoder.framesDropped(),
                  m_reconnects);
    m_decoder.reset();
    emit finished();
}

// ---------------------------------------------------------------------------
//  SerialLink（主线程侧）
// ---------------------------------------------------------------------------
SerialLink::SerialLink(LinkKind kind, QObject *parent) : LinkBase(kind, parent) {}

SerialLink::~SerialLink()
{
    stopReader();
}

void SerialLink::setConfig(const SerialConfig &cfg)
{
    m_cfg = cfg;
}

QStringList SerialLink::availablePorts()
{
    QStringList out;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &i : infos) {
        QString desc = i.description();
        if (!desc.isEmpty()) {
            out << QStringLiteral("%1 — %2").arg(i.portName(), desc);
        } else {
            out << i.portName();
        }
    }
    return out;
}

void SerialLink::open()
{
    if (m_reader != nullptr) {
        return; // 已在运行
    }
    if (m_cfg.portName.isEmpty()) {
        setState(LinkState::Error, QStringLiteral("未选择串口"));
        return;
    }
    startReader();
}

void SerialLink::close()
{
    stopReader();
    setState(LinkState::Closed, QString());
}

qint64 SerialLink::sendCommand(std::uint8_t cmd, const QByteArray &data)
{
    if (m_reader == nullptr || !isOpen() || m_cfg.portName.isEmpty()) {
        return -1;
    }
    const std::vector<uint8_t> payload(data.begin(), data.end());
    const std::vector<uint8_t> bytes = proto::encodeFrame(cmd, payload);
    const QByteArray out(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<int>(bytes.size()));
    m_reader->queueWrite(out);
    return out.size();
}

void SerialLink::startReader()
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("link-%1").arg(linkName(kind())));

    m_reader = new Reader(m_cfg);
    m_reader->moveToThread(m_thread);

    // Reader 的信号 → 本类信号（跨线程自动 queued；这里在 lambda 里补上 LinkKind）
    connect(m_reader, &Reader::chunkReady, this,
            [this](const QByteArray &b) { emit rawBytes(kind(), b); });
    connect(m_reader, &Reader::frameReadyRaw, this,
            [this](quint8 c, const QByteArray &d) { emit frameReady(kind(), c, d); });
    connect(m_reader, &Reader::stateRaw, this,
            [this](int st, const QString &d) {
                setState(static_cast<LinkState>(st), d);
                if (static_cast<LinkState>(st) == LinkState::Error) {
                    ++m_stats.reconnects; // 出错即计一次（含自动重连）
                }
            });
    connect(m_reader, &Reader::statsRaw, this,
            [this](quint64 rx, quint64 tx, quint64 ok, quint64 drop, quint64 re) {
                m_stats.bytesRx = rx;
                m_stats.bytesTx = tx;
                m_stats.framesOk = ok;
                m_stats.framesDropped = drop;
                m_stats.reconnects = qMax(m_stats.reconnects, re);
                emitStats();
            });
    connect(m_thread, &QThread::started, m_reader, &Reader::run);
    // Reader::run() 自己就是循环体：m_stop 置位后它会 return，线程随之结束。
    // 这里的 quit 用 **DirectConnection**（见 stopReader 的注释：跨线程排队到主线程
    // 会失效）；即使不连这条也不会出错，留着是为了"万一将来 run() 改成 exec() 循环"。
    connect(m_reader, &Reader::finished, m_thread, &QThread::quit, Qt::DirectConnection);

    setState(LinkState::Opening, QStringLiteral("启动读取线程…"));
    m_thread->start();
}

void SerialLink::stopReader()
{
    if (m_thread == nullptr) {
        return;
    }
    if (m_reader != nullptr) {
        m_reader->requestStop(); // 原子标志：读循环在 ≤10 ms 内退出
    }
    // ★ 直接调 quit()：QThread::quit() 是线程安全的，可以跨线程调用。
    //   这里**不能**只依赖 `Reader::finished → QThread::quit` 那条信号连接 ——
    //   它是 AutoConnection：Reader 在工作线程、QThread 对象的归属线程是主线程，
    //   于是 quit 会被**排队**到主线程的事件循环里执行。如果调用 close() 时主线程
    //   并不在跑事件循环（命令行工具、程序退出路径就是这种情形），这条排队信号
    //   永远不被投递 → wait(3000) 必然超时 → **关一次串口界面冻 3 秒**，
    //   而且 thread/reader 引用被放弃（泄漏）。
    //   这个坑是 2026-10-15 用 `--probe` 打真机时，从"状态年龄总是 ~3000 ms"反推出来的。
    m_thread->quit();
    if (!m_thread->wait(2000)) {
        // 极端情况（驱动卡死）：不强杀线程，仅放弃引用，避免崩溃 —— 但要让它在日志里可见
        qWarning("SerialLink: 读取线程 2 s 内未退出（驱动可能卡死），放弃引用；串口资源可能未释放");
        m_thread->setParent(nullptr);
        m_thread = nullptr;
        m_reader = nullptr;
        return;
    }
    delete m_reader;
    m_reader = nullptr;
    m_thread->deleteLater();
    m_thread = nullptr;
}

} // namespace io

// ★ 这个 .moc 必须保留：SerialLink::Reader 是**定义在本 .cpp 里**的 QObject 子类
//   （故意不放进头文件，避免把 QSerialPort 暴露出去），AutoMoc 因此需要本文件自己包含 moc。
//   删掉它会直接编译失败（AutoMoc error: contains Q_OBJECT but does not include *.moc）。
#include "serial_link.moc"
