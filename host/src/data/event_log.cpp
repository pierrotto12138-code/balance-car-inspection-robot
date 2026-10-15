#include "data/event_log.h"

#include "domain/frames.h"

namespace data {

EventLog::EventLog(QObject *parent) : QObject(parent)
{
    m_clock.start();
    qRegisterMetaType<data::EventEntry>("data::EventEntry");
}

void EventLog::append(EventEntry e)
{
    e.localMs = m_clock.elapsed();
    e.wall = QDateTime::currentDateTime();
    m_entries.append(e);
    // 环形上限：只丢最老的（界面显示最近 N 条；P4 的落盘在 append 之前就写完了）
    while (m_entries.size() > kMaxEntries) {
        m_entries.removeFirst();
    }
    emit entryAdded(m_entries.last());
}

void EventLog::onEvent(const domain::EventData &e, io::LinkKind kind)
{
    EventEntry entry;
    entry.kind = kind;
    entry.cmd = proto::up::kEvent;
    entry.code = static_cast<quint8>(e.code);
    entry.robotTsMs = e.timestampMs;
    entry.title = QString::fromUtf8(domain::eventName(e.code));

    switch (e.code) {
    case proto::EventCode::LinkLostStop:
    case proto::EventCode::EstopReceived:
        entry.safety = true;
        m_lastSafetyStopMs = m_clock.elapsed();
        entry.detail = (e.code == proto::EventCode::EstopReceived)
                           ? QStringLiteral("车已锁存，需重下任务才能解除")
                           : QStringLiteral("500 ms 未收到下行帧触发（固件侧）");
        break;
    case proto::EventCode::SafetyReleased: {
        entry.safety = true;
        if (m_lastSafetyStopMs >= 0) {
            const qint64 dur = m_clock.elapsed() - m_lastSafetyStopMs;
            entry.sinceSafetyStopMs = dur;
            entry.detail = QStringLiteral("停车时长 %1 s").arg(dur / 1000.0, 0, 'f', 1);
            if (m_shortestStopMs < 0 || dur < m_shortestStopMs) {
                m_shortestStopMs = dur;
            }
            m_lastSafetyStopMs = -1;
        }
        break;
    }
    default:
        // 其余事件码：`0x06` 气体超阈值、`0x05` 碰撞/低压 等，属"注意"级
        entry.detail = QStringLiteral("机器人侧时间戳 %1 ms").arg(e.timestampMs);
        break;
    }
    append(entry);
}

QString EventLog::alarmDetail(const domain::AlarmRecord &r, bool raised) const
{
    QString v = domain::units::number(r.value, r.unit == QStringLiteral("V") ? 2 : 1);
    if (r.unit.isEmpty()) {
        return raised ? QStringLiteral("已置位") : QStringLiteral("已解除");
    }
    if (raised) {
        return QStringLiteral("%1 %2（阈值 %3）")
            .arg(v, r.unit, domain::units::number(r.threshold, r.unit == QStringLiteral("V") ? 2 : 1));
    }
    const qint64 dur = (r.clearMs >= 0 && r.onsetMs >= 0) ? (r.clearMs - r.onsetMs) : -1;
    // 低压是"越低越坏"，所以它该显示"最低"而不是"峰值"（措辞错了会被误读成"电压冲到多高"）
    const QString extreme = (r.key == domain::AlarmEngine::kBatteryLow) ? QStringLiteral("最低")
                                                                       : QStringLiteral("峰值");
    return QStringLiteral("%1 %2 %3 · 持续 %4 s")
        .arg(extreme, domain::units::number(r.peak, r.unit == QStringLiteral("V") ? 2 : 1), r.unit)
        .arg(dur < 0 ? 0.0 : dur / 1000.0, 0, 'f', 1);
}

void EventLog::onAlarmRaised(const domain::AlarmRecord &r)
{
    EventEntry e;
    e.local = true;
    e.raised = true;
    e.cmd = 0; // 本机条目（不是 0x13）
    e.title = QStringLiteral("告警：%1").arg(r.title);
    e.detail = alarmDetail(r, true);
    append(e);
}

void EventLog::onAlarmCleared(const domain::AlarmRecord &r)
{
    EventEntry e;
    e.local = true;
    e.raised = false;
    e.cmd = 0;
    e.title = QStringLiteral("解除：%1").arg(r.title);
    e.detail = alarmDetail(r, false);
    append(e);
}

void EventLog::clear()
{
    m_entries.clear();
    m_lastSafetyStopMs = -1;
    m_shortestStopMs = -1;
    m_clock.restart();
    emit cleared();
}

qint64 EventLog::shortestSafetyStopMs() const
{
    return m_shortestStopMs;
}

} // namespace data
