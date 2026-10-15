#include "domain/keepalive.h"

#include "domain/commands.h"

#include <QTimer>

namespace domain {

KeepaliveEngine::KeepaliveEngine(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &KeepaliveEngine::onTick);
    m_timer->start(m_intervalMs);
}

QByteArray KeepaliveEngine::payloadFor(KeepaliveMode mode)
{
    switch (mode) {
    case KeepaliveMode::EnableZeroSpeed:
        // 零速 + 使能：**会解除锁存**（见头文件警告）
        return cmd::motionPayload(0, 0, true);
    case KeepaliveMode::Heartbeat:
        break;
    }
    return cmd::heartbeatPayload();
}

quint8 KeepaliveEngine::commandFor(KeepaliveMode mode)
{
    return (mode == KeepaliveMode::EnableZeroSpeed) ? proto::down::kMotion : proto::up::kHeartbeat;
}

void KeepaliveEngine::setIntervalMs(int ms)
{
    const int v = qBound(50, ms, 2000);
    if (v == m_intervalMs) {
        return;
    }
    m_intervalMs = v;
    m_timer->start(m_intervalMs); // 重新起算，避免"改周期后立刻连发两条"
    emit stateChanged();
}

void KeepaliveEngine::setMode(KeepaliveMode mode)
{
    if (mode == m_mode) {
        return;
    }
    m_mode = mode;
    emit stateChanged();
}

void KeepaliveEngine::setEnabled(bool on)
{
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    if (on) {
        sendNow(); // ★ 立刻发一条：否则"打开后到第一条"的空隙可能就是 500 ms 超时的来源
    }
    emit stateChanged();
}

void KeepaliveEngine::setLinkReady(bool ready)
{
    if (ready == m_linkReady) {
        return;
    }
    m_linkReady = ready;
    if (ready && m_enabled) {
        sendNow(); // 链路刚恢复：立刻补一条，别等下一个周期
    }
    emit stateChanged();
}

bool KeepaliveEngine::sendNow()
{
    if (!m_enabled || !m_linkReady || !m_sink) {
        return false;
    }
    const quint8 cmd = commandFor(m_mode);
    const QByteArray data = payloadFor(m_mode);
    const qint64 n = m_sink(cmd, data);
    if (n < 0) {
        emit sendFailed(cmd);
        emit stateChanged();
        return false;
    }
    ++m_sentCount;
    m_lastSentMs = m_clock.elapsed();
    emit sent(cmd, n);
    emit stateChanged();
    return true;
}

qint64 KeepaliveEngine::sinceLastSendMs() const
{
    return (m_lastSentMs < 0) ? -1 : (m_clock.elapsed() - m_lastSentMs);
}

void KeepaliveEngine::onTick()
{
    if (!m_enabled) {
        return;
    }
    sendNow();
}

} // namespace domain
