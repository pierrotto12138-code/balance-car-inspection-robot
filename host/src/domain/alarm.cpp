#include "domain/alarm.h"

#include <QStringList>

#include <algorithm>

namespace domain {

const QString AlarmEngine::kGasMq2 = QStringLiteral("gas.mq2");
const QString AlarmEngine::kGasMq135 = QStringLiteral("gas.mq135");
const QString AlarmEngine::kBatteryLow = QStringLiteral("battery.low");
const QString AlarmEngine::kLinkLost = QStringLiteral("link.lost");
const QString AlarmEngine::kSafetyLatched = QStringLiteral("safety.latched");

AlarmEngine::AlarmEngine(QObject *parent) : QObject(parent)
{
    m_clock.start();
    qRegisterMetaType<domain::AlarmRecord>("domain::AlarmRecord");
}

void AlarmEngine::setThresholds(const AlarmThresholds &t)
{
    m_th = t;
    // 阈值变了要按新阈值重判：活动的"气体超限"可能已经不超了 —— 这里不偷偷改状态，
    // 交给下一次 judgeThreshold（下一帧就到），避免"改阈值那一刻凭空产生一条解除事件"。
    emit thresholdsChanged(m_th);
}

AlarmRecord &AlarmEngine::slot(const QString &key, const QString &title, Severity sev,
                               const QString &unit, double threshold)
{
    auto it = m_records.find(key);
    if (it == m_records.end()) {
        AlarmRecord r;
        r.key = key;
        r.title = title;
        r.severity = sev;
        r.unit = unit;
        r.threshold = threshold;
        it = m_records.insert(key, r);
    } else {
        it->threshold = threshold; // 阈值可改，记录里的阈值跟着更新
    }
    return it.value();
}

void AlarmEngine::setActive(AlarmRecord &r, bool active)
{
    if (r.active == active) {
        return; // 状态跃变才报 —— 这是"同一段异常只提示一次"的实现处
    }
    r.active = active;
    if (active) {
        r.onsetMs = m_clock.elapsed();
        r.clearMs = -1;
        r.peak = r.value;
        emit alarmRaised(r);
    } else {
        r.clearMs = m_clock.elapsed();
        emit alarmCleared(r);
    }
}

void AlarmEngine::judgeThresholdFor(const QString &key, const QString &title, Severity sev,
                                   const QString &unit, double badAt, double clearAt, double value,
                                   bool lowIsBad)
{
    AlarmRecord &r = slot(key, title, sev, unit, badAt);
    judgeThreshold(r, value, badAt, clearAt, lowIsBad);
}

void AlarmEngine::judgeThreshold(AlarmRecord &r, double value, double badAt, double clearAt,
                                 bool lowIsBad)
{
    r.value = value;
    if (r.active) {
        r.peak = lowIsBad ? std::min(r.peak, value) : std::max(r.peak, value);
    }

    // "坏"与"回到正常"的比较方向由 lowIsBad 决定：
    //   高=坏（气体）：坏 ⇔ value ≥ badAt；好 ⇔ value ≤ clearAt
    //   低=坏（电压）：坏 ⇔ value ≤ badAt；好 ⇔ value ≥ clearAt
    const bool bad = lowIsBad ? (value <= badAt) : (value >= badAt);
    const bool good = lowIsBad ? (value >= clearAt) : (value <= clearAt);

    if (!r.active) {
        if (bad) {
            if (r.overCount < m_th.confirmFrames) {
                ++r.overCount;
            }
            r.underCount = 0;
            if (r.overCount >= m_th.confirmFrames) {
                r.value = value;
                setActive(r, true);
            }
        } else {
            r.overCount = 0; // 中途回到正常 ⇒ 确认计数清零（不累计"间歇性超限"）
        }
    } else {
        if (good) {
            if (r.underCount < m_th.clearFrames) {
                ++r.underCount;
            }
            if (r.underCount >= m_th.clearFrames) {
                r.value = value;
                setActive(r, false);
                r.overCount = 0;
                r.underCount = 0;
            }
        } else {
            r.underCount = 0; // 迟滞区间内：既不报新警，也不解除
        }
    }
}

void AlarmEngine::judgeState(AlarmRecord &r, bool active, double value)
{
    r.value = value;
    if (active) {
        setActive(r, true);
    } else if (r.active) {
        setActive(r, false);
    }
}

void AlarmEngine::onFrameDecoded(const VehicleState &st, quint8 cmd)
{
    switch (cmd) {
    case proto::up::kEnvGas: {
        const EnvData &e = st.env();
        // 传感器未就绪时不判气体告警：那时的浓度是 0/无意义，拿它判"正常"会掩盖
        // "其实没插传感器"；而界面上就绪位已经明确说了未就绪（各管各的信息）。
        if (e.mq2Ready()) {
            judgeThresholdFor(kGasMq2, QStringLiteral("MQ-2 浓度超阈值"), Severity::Alarm,
                              QStringLiteral("ppm"), m_th.mq2Ppm,
                              m_th.mq2Ppm * m_th.gasClearRatio, e.mq2Ppm);
        }
        if (e.mq135Ready()) {
            judgeThresholdFor(kGasMq135, QStringLiteral("MQ-135 浓度超阈值"), Severity::Alarm,
                              QStringLiteral("ppm"), m_th.mq135Ppm,
                              m_th.mq135Ppm * m_th.gasClearRatio, e.mq135Ppm);
        }
        break;
    }
    case proto::up::kDiag: {
        // ★ 先过"有没有电池"的门槛（6.00 V，决策 60；真机 USB 供电时 0x15 报 ≈4.3 V）：
        //   没电池时报"低压锁存"是假报警，而且会把车真的锁住（固件也按无电池处理）。
        if (st.batteryValid() && batteryPresent(st.diag().batteryV)) {
            judgeThresholdFor(kBatteryLow, QStringLiteral("电池低压"), Severity::Warn,
                              QStringLiteral("V"), m_th.lowBatteryV, m_th.lowBatteryClearV,
                              st.diag().batteryV, /*lowIsBad=*/true);
        } else if (st.batteryAbsent()) {
            // 曾经报过低压、现在电压掉到门槛以下（拔了电池）⇒ 按"无电池"处理，撤掉该告警
            auto it = m_records.find(kBatteryLow);
            if (it != m_records.end() && it->active) {
                setActive(it.value(), false);
            }
        }
        break;
    }
    default:
        break;
    }
}

void AlarmEngine::tick(const VehicleState &st)
{
    // ---- 失联：必须"先有过数据"才判 —— 端口没打开时的"没有帧"不是失联 ----
    const qint64 age = st.anyFrameAgeMs();
    AlarmRecord &link = slot(kLinkLost, QStringLiteral("通信失联"), Severity::Alarm,
                             QStringLiteral("ms"), double(m_th.linkLostMs));
    if (age >= 0) {
        judgeState(link, age > m_th.linkLostMs, double(age));
    }

    // ---- 安全锁存：来自事件（0x08/0x0A/0x09）与 0x15 bit1 的合并判定 ----
    AlarmRecord &latch = slot(kSafetyLatched, QStringLiteral("安全锁存（需重下任务解除）"),
                              Severity::Alarm, QString(), 0.0);
    judgeState(latch, st.safetyLatched(), 0.0);
}

void AlarmEngine::reset()
{
    m_records.clear();
    m_clock.restart();
}

bool AlarmEngine::anyActive() const
{
    for (const AlarmRecord &r : m_records) {
        if (r.active) {
            return true;
        }
    }
    return false;
}

int AlarmEngine::activeCount() const
{
    int n = 0;
    for (const AlarmRecord &r : m_records) {
        if (r.active) {
            ++n;
        }
    }
    return n;
}

QList<AlarmRecord> AlarmEngine::activeAlarms() const
{
    QList<AlarmRecord> out;
    for (const AlarmRecord &r : m_records) {
        if (r.active) {
            out.append(r);
        }
    }
    std::sort(out.begin(), out.end(), [](const AlarmRecord &a, const AlarmRecord &b) {
        if (a.severity != b.severity) {
            return static_cast<int>(a.severity) > static_cast<int>(b.severity);
        }
        return a.onsetMs < b.onsetMs;
    });
    return out;
}

Severity AlarmEngine::severity() const
{
    Severity s = Severity::Ok;
    for (const AlarmRecord &r : m_records) {
        if (r.active && static_cast<int>(r.severity) > static_cast<int>(s)) {
            s = r.severity;
        }
    }
    return s;
}

QString AlarmEngine::activeSummary() const
{
    const QList<AlarmRecord> act = activeAlarms();
    if (act.isEmpty()) {
        return QString();
    }
    auto one = [](const AlarmRecord &r) {
        if (r.unit.isEmpty()) {
            return r.title;
        }
        return QStringLiteral("%1 %2 %3（阈值 %4）")
            .arg(r.title, units::number(r.value, r.unit == QStringLiteral("V") ? 2 : 1), r.unit,
                 units::number(r.threshold, r.unit == QStringLiteral("V") ? 2 : 1));
    };
    if (act.size() == 1) {
        return one(act.first());
    }
    QStringList names;
    for (const AlarmRecord &r : act) {
        names << r.title;
    }
    return QStringLiteral("%1 项报警：%2").arg(act.size()).arg(names.join(QStringLiteral("、")));
}

QList<AlarmRecord> AlarmEngine::history() const
{
    QList<AlarmRecord> out = m_records.values();
    std::sort(out.begin(), out.end(), [](const AlarmRecord &a, const AlarmRecord &b) {
        return a.onsetMs < b.onsetMs;
    });
    return out;
}

AlarmRecord AlarmEngine::record(const QString &key) const
{
    return m_records.value(key);
}

} // namespace domain
