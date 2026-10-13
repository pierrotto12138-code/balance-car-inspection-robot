#include "io/link_manager.h"

#include "io/replay_link.h"

namespace io {

namespace {
int idx(LinkKind k)
{
    return static_cast<int>(k);
}
} // namespace

LinkManager::LinkManager(QObject *parent) : QObject(parent)
{
    registerLinkMetaTypes();

    // 默认串口配置：与协议一致（115200），端口留空由界面选
    m_cfg[idx(LinkKind::Wire)].baudRate = 115200;
    m_cfg[idx(LinkKind::Wire)].autoReconnect = true;
    m_cfg[idx(LinkKind::Bluetooth)].baudRate = 115200;
    m_cfg[idx(LinkKind::Bluetooth)].autoReconnect = true;

    createLink(LinkKind::Wire, Mode::Serial);
    createLink(LinkKind::Bluetooth, Mode::Serial);
}

LinkManager::~LinkManager()
{
    destroyLink(LinkKind::Wire);
    destroyLink(LinkKind::Bluetooth);
}

LinkManager::Mode LinkManager::mode(LinkKind kind) const
{
    return m_modes[idx(kind)];
}

void LinkManager::setMode(LinkKind kind, Mode mode)
{
    if (m_modes[idx(kind)] == mode && m_links[idx(kind)] != nullptr) {
        return;
    }
    destroyLink(kind);
    m_modes[idx(kind)] = mode;
    createLink(kind, mode);
}

void LinkManager::setSerialConfig(LinkKind kind, const SerialConfig &cfg)
{
    m_cfg[idx(kind)] = cfg;
    if (auto *sl = qobject_cast<SerialLink *>(m_links[idx(kind)])) {
        sl->setConfig(cfg);
    }
}

void LinkManager::setReplayRate(double x)
{
    m_rate = x;
    for (int i = 0; i < idx(LinkKind::Count); ++i) {
        if (auto *rl = qobject_cast<ReplayLink *>(m_links[i])) {
            rl->setRateMultiplier(x);
        }
    }
}

void LinkManager::setReplayCorruptRatio(double r)
{
    m_corrupt = r;
    for (int i = 0; i < idx(LinkKind::Count); ++i) {
        if (auto *rl = qobject_cast<ReplayLink *>(m_links[i])) {
            rl->setCorruptRatio(r);
        }
    }
}

int LinkManager::loadCapture(LinkKind kind, const QString &path)
{
    if (auto *rl = qobject_cast<ReplayLink *>(m_links[idx(kind)])) {
        return rl->loadCapture(path);
    }
    return -1;
}

void LinkManager::createLink(LinkKind kind, Mode mode)
{
    LinkBase *l = nullptr;
    if (mode == Mode::Serial) {
        auto *sl = new SerialLink(kind, this);
        sl->setConfig(m_cfg[idx(kind)]);
        l = sl;
    } else {
        auto *rl = new ReplayLink(kind, this);
        rl->setRateMultiplier(m_rate);
        rl->setCorruptRatio(m_corrupt);
        l = rl;
    }
    m_links[idx(kind)] = l;

    connect(l, &LinkBase::frameReady, this, &LinkManager::frameReady);
    connect(l, &LinkBase::rawBytes, this, &LinkManager::rawBytes);
    connect(l, &LinkBase::stateChanged, this,
            [this](LinkKind k, LinkState st, const QString &d) {
                emit linkStateChanged(k, st, d);
                emit primaryChanged(primary());
            });
    connect(l, &LinkBase::statsChanged, this, &LinkManager::linkStatsChanged);
}

void LinkManager::destroyLink(LinkKind kind)
{
    if (m_links[idx(kind)] == nullptr) {
        return;
    }
    LinkBase *l = m_links[idx(kind)];
    l->close();
    l->deleteLater();
    m_links[idx(kind)] = nullptr;
}

LinkBase *LinkManager::link(LinkKind kind) const
{
    return m_links[idx(kind)];
}

LinkStats LinkManager::stats(LinkKind kind) const
{
    LinkBase *l = m_links[idx(kind)];
    return l != nullptr ? l->stats() : LinkStats{};
}

bool LinkManager::isOpen(LinkKind kind) const
{
    LinkBase *l = m_links[idx(kind)];
    return l != nullptr && l->isOpen();
}

void LinkManager::open(LinkKind kind)
{
    if (LinkBase *l = m_links[idx(kind)]) {
        l->open();
    }
}

void LinkManager::openAll()
{
    open(LinkKind::Wire);
    open(LinkKind::Bluetooth);
}

void LinkManager::close(LinkKind kind)
{
    if (LinkBase *l = m_links[idx(kind)]) {
        l->close();
    }
    emit primaryChanged(primary());
}

void LinkManager::closeAll()
{
    close(LinkKind::Wire);
    close(LinkKind::Bluetooth);
}

LinkKind LinkManager::primary() const
{
    if (isOpen(LinkKind::Wire)) {
        return LinkKind::Wire;
    }
    if (isOpen(LinkKind::Bluetooth)) {
        return LinkKind::Bluetooth;
    }
    return LinkKind::Count;
}

LinkKind LinkManager::primaryLink() const
{
    return primary();
}

qint64 LinkManager::send(std::uint8_t cmd, const QByteArray &data)
{
    const LinkKind k = primary();
    return k == LinkKind::Count ? -1 : sendTo(k, cmd, data);
}

qint64 LinkManager::sendTo(LinkKind kind, std::uint8_t cmd, const QByteArray &data)
{
    LinkBase *l = m_links[idx(kind)];
    if (l == nullptr || !l->isOpen()) {
        return -1;
    }
    return l->sendCommand(cmd, data);
}

} // namespace io
