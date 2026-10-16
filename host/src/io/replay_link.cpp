#include "io/replay_link.h"

#include "codec.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

#include <cmath>

namespace io {

namespace {

/// 合成数据用的时间基准（毫秒）
quint32 nowMs()
{
    return static_cast<quint32>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFFull);
}

QByteArray toQ(const std::vector<uint8_t> &v)
{
    return QByteArray(reinterpret_cast<const char *>(v.data()), static_cast<int>(v.size()));
}

// ---- 演练时刻表（虚拟时间 ms）----
// 为什么假源要主动造事件与安全状态：P1/P2/P3 要显示的"报警、锁存、解除"在真实硬件上
// 需要人去制造（拔线、按急停），离线开发时根本没法复现"锁存前后界面差别"。
// 这里把它变成**可重复的剧本**：每 60 s 一轮（急停在 40 s、解除在 48 s、低压在 50~56 s）。
struct ScheduledEvent {
    quint64 atMs;
    proto::EventCode code;
};

constexpr ScheduledEvent kOneShots[] = {
    {1000, proto::EventCode::SelfCheckDone},
    {1200, proto::EventCode::WarmupOrFail},
    {2000, proto::EventCode::TaskStart},
    {40000, proto::EventCode::EstopReceived},
    {48000, proto::EventCode::SafetyReleased},
    {58000, proto::EventCode::TaskDone},
};

/// 气体冲高窗口：每 30 s 里的前 4 s（与 makeEnvGas 用同一判据，必须一起改）
constexpr quint64 kSpikePeriodMs = 30000;
constexpr quint64 kSpikeLenMs = 4000;
/// 安全锁存窗口与低压窗口（每 60 s 一轮）
constexpr quint64 kSafetyPeriodMs = 60000;
constexpr quint64 kEstopAtMs = 40000;
constexpr quint64 kEstopReleaseMs = 48000;
constexpr quint64 kLowBatAtMs = 50000;
constexpr quint64 kLowBatEndMs = 56000;

bool inWindow(quint64 t, quint64 period, quint64 from, quint64 to)
{
    const quint64 p = t % period;
    return p >= from && p < to;
}

bool inSpike(quint64 t)
{
    // 前 30 s（第 0 个窗口）**不冲高**：否则假源一开机就"气体超限"，
    // 既不符合真实起步过程，也会让人误以为界面在乱报。
    return t >= kSpikePeriodMs && inWindow(t, kSpikePeriodMs, 0, kSpikeLenMs);
}

bool inEstopLatch(quint64 t)
{
    return inWindow(t, kSafetyPeriodMs, kEstopAtMs, kEstopReleaseMs);
}

bool inLowBattery(quint64 t)
{
    return inWindow(t, kSafetyPeriodMs, kLowBatAtMs, kLowBatEndMs);
}

} // namespace

ReplayLink::ReplayLink(LinkKind kind, QObject *parent) : LinkBase(kind, parent)
{
    m_timer = new QTimer(this);
    // 20 ms 基准节拍：协议里最快的周期帧是 50 ms（20 Hz），
    // 取 20 ms 是为了让"按周期拆分发送"足够细；倍数放大时再缩短。
    connect(m_timer, &QTimer::timeout, this, &ReplayLink::onTick);
}

ReplayLink::~ReplayLink() = default;

void ReplayLink::setRateMultiplier(double x)
{
    m_rate = (x > 0.0) ? x : 1.0;
}

void ReplayLink::setCorruptRatio(double ratio)
{
    m_corruptRatio = (ratio < 0.0) ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
}

void ReplayLink::open()
{
    m_t0 = nowMs();
    m_tickCount = 0;
    m_captureIdx = 0;
    m_virtualMs = 0;
    m_lastPose = m_lastMotion = m_lastTask = m_lastEnv = m_lastHb = 0;
    m_lastDiag = m_lastLoop = 0;
    for (int i = 0; i < kOneShotCount; ++i) {
        m_eventFired[i] = false;
    }
    m_lastSpikeIdx = -1;
    m_decoder.reset();
    m_clock.start();
    // 节拍只决定"多久检查一次"，**不决定速率**：速率由实测经过时间 × 倍数算出来。
    // ★ 这一点很关键：Windows 默认定时器粒度≈15.6 ms，5 ms 的 QTimer 实际约 15 ms 才触发；
    //   若按"节拍 × 倍数"累计虚拟时间，产帧率会掉到标称值的 1/3（实测 600 ms 只有 10 帧）。
    //   用 QElapsedTimer 量真实经过时间后，产帧率与系统抖动无关。
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->start(kBaseTickMs);
    setState(LinkState::Open,
             synthetic() ? QStringLiteral("合成数据源 ×%1").arg(m_rate, 0, 'f', 1)
                         : QStringLiteral("回放 %1 帧").arg(m_capture.size()));
}

void ReplayLink::close()
{
    m_timer->stop();
    setState(LinkState::Closed, QString());
}

qint64 ReplayLink::sendCommand(std::uint8_t cmd, const QByteArray &data)
{
    if (!isOpen()) {
        return -1;
    }
    const std::vector<uint8_t> payload(data.begin(), data.end());
    const QByteArray out = toQ(proto::encodeFrame(cmd, payload));
    bumpTx(out.size());

    // 假源也要能验证"调参往返"：对 0x24 立刻原样回一条（与固件"回读实际生效值"同形）——
    // 真实固件的回读值来自 Flash/全局量，这里只是把值原样送回，够 P0 用。
    if (cmd == proto::down::kPidTune && data.size() >= 6) {
        m_generated++;
        emitRawFrame(out);
        emit replyEmitted(cmd, data);
    }
    emitStats();
    return out.size();
}

int ReplayLink::loadCapture(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return -1;
    }
    QTextStream in(&f);
    m_capture.clear();
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        // 支持 "AA 55 ..." 与 "AA55..." 两种写法；忽略行内其它非十六进制字符
        QString hex;
        for (const QChar &ch : line) {
            if (ch.isLetterOrNumber()) {
                hex.append(ch);
            }
        }
        if (hex.size() < 2) {
            continue;
        }
        const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        // 一行可能含多帧：按帧头切开，逐个存入
        int start = 0;
        while (true) {
            const int p = bytes.indexOf(QByteArray("\xAA\x55", 2), start);
            if (p < 0) {
                break;
            }
            const int next = bytes.indexOf(QByteArray("\xAA\x55", 2), p + 2);
            const int end = (next < 0) ? bytes.size() : next;
            m_capture.append(bytes.mid(p, end - p));
            start = end;
        }
    }
    return m_capture.size();
}

QByteArray ReplayLink::makePose(quint32 tMs)
{
    std::vector<uint8_t> d;
    const double ang = std::sin(tMs / 1500.0) * 12.5; // ±12.5°：看起来很"真实"
    proto::putI16(d, static_cast<int16_t>(ang * 100));            // 俯仰 0.01°
    proto::putI16(d, static_cast<int16_t>(std::sin(tMs / 900.0) * 300)); // 横滚
    proto::putI16(d, 0);                                          // 偏航
    proto::putI16(d, static_cast<int16_t>(std::cos(tMs / 1500.0) * 200));
    proto::putI16(d, 0);
    proto::putI16(d, 15);
    return toQ(proto::encodeFrame(proto::up::kPose, d));
}

QByteArray ReplayLink::makeMotion(quint32 tMs)
{
    std::vector<uint8_t> d;
    const int spd = static_cast<int>(350 * std::sin(tMs / 2000.0));
    proto::putI16(d, static_cast<int16_t>(spd));      // 左轮速 mm/s
    proto::putI16(d, static_cast<int16_t>(spd));      // 右轮速
    proto::putI16(d, static_cast<int16_t>(spd / 2));  // PWM 千分比
    proto::putU16(d, 1200);                           // 距离 mm（≈120 cm；★ 字段是毫米，见 MotionData 说明）
    d.push_back(0x00);                                // 避障状态位
    return toQ(proto::encodeFrame(proto::up::kMotion, d));
}

QByteArray ReplayLink::makeTask(quint32 tMs)
{
    const quint64 t = tMs;
    std::vector<uint8_t> d;
    int phase = static_cast<int>((t / 5000) % 8);
    uint8_t fault = 0x00;
    // 与气体冲高、急停锁存**保持一致**：状态帧与事件帧互相矛盾的话，
    // 界面上就会出现"事件说锁存、状态说正常"，看的人第一反应是"上位机坏了"。
    if (inSpike(t)) {
        fault |= 0x01; // bit0 气体
        phase = static_cast<int>(proto::Phase::Analysis);
    }
    if (inEstopLatch(t)) {
        phase = static_cast<int>(proto::Phase::Fault);
    }
    d.push_back(static_cast<uint8_t>(proto::TaskId::Patrol));
    d.push_back(static_cast<uint8_t>(phase));
    d.push_back(static_cast<uint8_t>((t / 100) % 101)); // 完成度 %
    d.push_back(static_cast<uint8_t>((t / 8000) % 5));  // 巡检点
    d.push_back(fault);                                 // 异常标志
    return toQ(proto::encodeFrame(proto::up::kTask, d));
}

QByteArray ReplayLink::makeEnvGas(quint32 tMs)
{
    std::vector<uint8_t> d;
    proto::putU16(d, 1234);
    proto::putU16(d, 2345);
    // 每 30 s 来一次"气体冲高"，方便看报警与曲线
    const bool spike = inSpike(tMs);
    // 常态值必须**明显低于**协议默认阈值（30 / 50 ppm），否则假源会在"没有异常"的时候
    // 一直报警 —— 那样演示时就分不清"报警是冲高引起的"还是"门槛设得不对"。
    // 取值贴近真机台架的本底（决策 49：清洁空气本底 3.6 / 4.3 ppm）。
    const double mq2 = spike ? 420.0 : 9.0 + 4.0 * std::sin(tMs / 3000.0);
    const double mq135 = spike ? 610.0 : 14.0 + 5.0 * std::cos(tMs / 2500.0);
    proto::putU16(d, static_cast<uint16_t>(mq2 * 10));
    proto::putU16(d, static_cast<uint16_t>(mq135 * 10));
    proto::putI16(d, 256);   // 25.6 ℃
    proto::putU16(d, 610);   // 61.0 %RH
    d.push_back(0x0F);       // 就绪位：bit0 MQ-2 | bit1 MQ-135 | bit2 DHT11 | bit3 补偿
    d.push_back(spike ? static_cast<uint8_t>(proto::SampleGear::Fast)
                      : static_cast<uint8_t>(proto::SampleGear::Normal));
    // ★ 0x14 的数据段是 **16** 字节（协议 §3.1）：偏移 14/15 是保留字节，固件里写 0。
    //   这里原先漏了它们（只发 14 字节）→ 任何按"长度 ≥16"判断的实现（我们自己的
    //   Parser、以及 P1 要写的 0x14 解析）都会把它当**未知命令**，现象是
    //   "假源在跑、但环境数据一直是空的"，极难查。测试里已加长度断言防复发。
    d.push_back(0x00);
    d.push_back(0x00);
    return toQ(proto::encodeFrame(proto::up::kEnvGas, d));
}

QByteArray ReplayLink::makeEvent(proto::EventCode code, quint32 stampMs)
{
    std::vector<uint8_t> d;
    d.push_back(static_cast<uint8_t>(code));
    proto::putU32(d, stampMs); // 机器人侧时间戳（这里用虚拟时间，与产帧时刻自洽）
    return toQ(proto::encodeFrame(proto::up::kEvent, d));
}

QByteArray ReplayLink::makeDiag(quint32 tMs)
{
    const quint64 t = tMs;
    std::vector<uint8_t> d;
    double v = 11.85 + 0.15 * std::sin(static_cast<double>(t) / 7000.0); // 常态 11.7~12.0 V
    if (inLowBattery(t)) {
        v = 10.95 - 0.10 * std::sin(static_cast<double>(t) / 2000.0); // 掉到报警线 11.10 以下
    }
    proto::putU16(d, static_cast<uint16_t>(v * 100)); // 0 电池电压 0.01 V（★ 只有这一帧带电压）
    d.push_back(0x02);                                // 2 当前显示页 = 巡检通信页
    d.push_back(0x00);                                // 3 Flag_Show（0 = 屏幕归我们）
    d.push_back(0x02);                                // 4 姿态算法 = 卡尔曼（与真机 Way_Angle=2 一致）

    uint8_t bits = 0x04; // bit2 DHT11 有效
    if (inSpike(t)) {
        bits |= 0x01; // bit0 报警中
    }
    if (inEstopLatch(t)) {
        bits |= 0x02; // bit1 已安全锁存（与 0x0A/0x09 事件一致）
    }
    if (kind() == LinkKind::Bluetooth && m_stats.bytesRx > 0) {
        bits |= 0x08; // bit3 本链路收过数据（有线链路不该置这一位）
    }
    d.push_back(bits); // 5 状态位

    proto::putU16(d, static_cast<uint16_t>(18 - (t / 1000) % 3)); // 6 剩余堆 KB
    // 8 OLED 累计写入字节：约 1.05 kB/s 稳定增长（"不涨 = 屏幕没在刷"）
    proto::putU32(d, static_cast<uint32_t>(t * 105 / 100));
    const uint8_t tick = static_cast<uint8_t>((t / 1000) & 0xFF);
    d.push_back(tick);                                     // 12 通信
    d.push_back(tick);                                     // 13 气体
    d.push_back(static_cast<uint8_t>(tick - 1));           // 14 状态
    d.push_back(static_cast<uint8_t>(tick - 2));           // 15 界面
    return toQ(proto::encodeFrame(proto::up::kDiag, d));
}

QByteArray ReplayLink::makeLoop(quint32 tMs)
{
    const double t = static_cast<double>(tMs);
    std::vector<uint8_t> d;
    const int bal = static_cast<int>(std::sin(t / 800.0) * 300.0);
    const int vel = static_cast<int>(std::sin(t / 2500.0) * 120.0);
    const int turn = static_cast<int>(std::sin(t / 1500.0) * 40.0);
    const int moto1 = bal + vel + turn;
    const int moto2 = bal + vel - turn;
    const int a1 = moto1 < 0 ? -moto1 : moto1;
    const int a2 = moto2 < 0 ? -moto2 : moto2;
    proto::putI16(d, static_cast<int16_t>(bal));
    proto::putI16(d, static_cast<int16_t>(vel));
    proto::putI16(d, static_cast<int16_t>(turn));
    proto::putI16(d, static_cast<int16_t>(moto1));
    proto::putI16(d, static_cast<int16_t>(moto2));
    // "算了没执行"（Moto 非零而 CCR=0）在假源里**不该出现**，否则离线看到的
    // 就是假故障。这里 CCR 就是 |Moto|（钳到 7200 内）。
    proto::putU16(d, static_cast<uint16_t>(a1 > 7200 ? 7200 : a1));
    proto::putU16(d, static_cast<uint16_t>(a2 > 7200 ? 7200 : a2));
    proto::putI16(d, static_cast<int16_t>(std::sin(t / 900.0) * 120.0)); // 编码器原始计数
    proto::putI16(d, static_cast<int16_t>(std::sin(t / 950.0) * 120.0));
    // ★ bit3 = 固件特性位（2026-10-15）：假源也要置上，否则 demo 里状态条会显示
    //   "⚠旧构建"，与真机（已烧新固件）不一致 —— 假源应模拟**当前**固件的行为。
    uint8_t bits = 0x04 | 0x08; // bit2：本拍真的调用了 Set_Pwm；bit3：新特性
    if (inEstopLatch(static_cast<quint64>(tMs))) {
        bits |= 0x01 | 0x02; // Flag_Stop | App_MotorDisable
    }
    d.push_back(bits);
    d.push_back(0xAF); // 固件版本字节 (月<<4)|日 = 10 月 15 日（协议 §3.7）
    return toQ(proto::encodeFrame(proto::up::kLoop, d));
}

void ReplayLink::emitRawFrame(const QByteArray &bytes)
{
    // 故意掺坏帧（翻转最后一个字节 = 破坏 CRC），用来验证丢帧统计
    QByteArray out = bytes;
    if (m_corruptRatio > 0.0 &&
        QRandomGenerator::global()->generateDouble() < m_corruptRatio && !out.isEmpty()) {
        out[out.size() - 1] = static_cast<char>(out[out.size() - 1] ^ 0xFF);
    }
    m_stats.bytesRx += static_cast<quint64>(out.size());
    emit rawBytes(kind(), out);

    // ★ 必须自己解帧：真串口是在读线程里解的（SerialLink::Reader），
    //   假源如果只发原始字节，上层就永远收不到 frameReady —— 这曾经让 P0 的
    //   五个测试全部拿到 0 帧。解帧统计同样以 FrameDecoder 为唯一来源。
    const auto frames = m_decoder.feed(reinterpret_cast<const uint8_t *>(out.constData()),
                                       static_cast<std::size_t>(out.size()));
    for (const proto::Frame &f : frames) {
        emit frameReady(kind(), static_cast<quint8>(f.cmd),
                        QByteArray(reinterpret_cast<const char *>(f.data.data()),
                                   static_cast<int>(f.data.size())));
    }
    m_stats.framesOk = m_decoder.framesOk();
    m_stats.framesDropped = m_decoder.framesDropped();
    ++m_generated;
}

int ReplayLink::dueCount(quint64 &last, quint64 periodMs)
{
    int n = 0;
    // 虚拟时间一次跨过多个周期时**补发**（而不是只发一条），否则高倍数下会系统性少发
    while (m_virtualMs - last >= periodMs && n < kMaxCatchUp) {
        last += periodMs;
        ++n;
    }
    return n;
}

void ReplayLink::onTick()
{
    if (!isOpen()) {
        return;
    }
    ++m_tickCount;

    // 虚拟时间 = 实测经过时间 × 倍数（与定时器粒度无关）
    m_virtualMs = static_cast<quint64>(static_cast<double>(m_clock.elapsed()) * m_rate);

    if (!m_capture.isEmpty()) {
        // ---- 文件回放：按倍数每拍发 N 帧 ----
        const int n = qBound(1, static_cast<int>(std::lround(m_rate)), kMaxCatchUp);
        for (int k = 0; k < n; ++k) {
            emitRawFrame(m_capture.at(m_captureIdx));
            m_captureIdx = (m_captureIdx + 1) % m_capture.size();
        }
        emitStats();
        return;
    }

    // ---- 合成：按协议真实周期产帧（周期见 communication_protocol.md §三）----
    const quint32 vms = static_cast<quint32>(m_virtualMs);
    for (int i = dueCount(m_lastPose, 50); i > 0; --i) { // 0x10 姿态 20 Hz
        emitRawFrame(makePose(vms));
    }
    for (int i = dueCount(m_lastMotion, 50); i > 0; --i) { // 0x11 运动 20 Hz
        emitRawFrame(makeMotion(vms));
    }
    for (int i = dueCount(m_lastTask, 200); i > 0; --i) { // 0x12 任务 5 Hz
        emitRawFrame(makeTask(vms));
    }
    for (int i = dueCount(m_lastEnv, 1000); i > 0; --i) { // 0x14 环境 1 Hz
        emitRawFrame(makeEnvGas(vms));
    }
    for (int i = dueCount(m_lastDiag, 1000); i > 0; --i) { // 0x15 诊断 1 Hz
        emitRawFrame(makeDiag(vms));
    }
    for (int i = dueCount(m_lastLoop, 1000); i > 0; --i) { // 0x16 三环实况 1 Hz
        emitRawFrame(makeLoop(vms));
    }
    for (int i = dueCount(m_lastHb, 200); i > 0; --i) { // 0x1F 心跳 5 Hz
        emitRawFrame(toQ(proto::encodeFrame(proto::up::kHeartbeat, {})));
    }

    // ---- 事件演练（0x13）：一次性剧本 + 气体冲高沿 ----
    for (int i = 0; i < kOneShotCount; ++i) {
        if (!m_eventFired[i] && m_virtualMs >= kOneShots[i].atMs) {
            m_eventFired[i] = true;
            emitRawFrame(makeEvent(kOneShots[i].code, static_cast<quint32>(kOneShots[i].atMs)));
        }
    }
    const qint64 spikeIdx = static_cast<qint64>(m_virtualMs / kSpikePeriodMs);
    if (inSpike(m_virtualMs) && spikeIdx != m_lastSpikeIdx) {
        m_lastSpikeIdx = spikeIdx; // 只在"进入冲高窗口"的那一沿报一次（重复帧不重复上报）
        // ★ 时间戳用**窗口起点**（spikeIdx × 周期），而不是本次 tick 的时刻：
        //   两条假源链路的 tick 相差几毫秒，若各用各的时刻，同一次冲高会被造出
        //   **两个不同的帧**（内容不同 ⇒ 上位机的跨链路去重认不出来）→ 事件流出现两条
        //   "气体超阈值"。真实固件是同一帧发两路，所以假源也必须造出**逐字节相同**的帧。
        emitRawFrame(makeEvent(proto::EventCode::GasOverLimit,
                               static_cast<quint32>(spikeIdx * kSpikePeriodMs)));
    }

    emitStats();
}

} // namespace io
// 注意：Q_OBJECT 声明在 replay_link.h（已列进 CMake 目标），moc 由 AUTOMOC 统一生成。
// 这里**不要**再 `#include "replay_link.moc"` —— 否则 AutoMoc 会报
// "includes the moc file ... but does not contain Q_OBJECT"（噪音告警会掩盖真问题）。
