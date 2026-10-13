#include "io/link.h"

#include <QMetaType>

namespace io {

const char *linkName(LinkKind kind)
{
    switch (kind) {
    case LinkKind::Wire:
        return "有线";
    case LinkKind::Bluetooth:
        return "蓝牙";
    case LinkKind::Count:
        break;
    }
    return "未知";
}

const char *stateName(LinkState state)
{
    switch (state) {
    case LinkState::Closed:
        return "未连接";
    case LinkState::Opening:
        return "连接中";
    case LinkState::Open:
        return "已连接";
    case LinkState::Error:
        return "错误";
    }
    return "未知";
}

LinkBase::LinkBase(LinkKind kind, QObject *parent)
    : QObject(parent), m_kind(kind)
{
}

LinkBase::~LinkBase() = default;

void LinkBase::setState(LinkState st, const QString &detail)
{
    if (m_state == st && detail.isEmpty()) {
        return;
    }
    m_state = st;
    emit stateChanged(m_kind, st, detail);
}

void LinkBase::emitStats()
{
    emit statsChanged(m_kind, m_stats.bytesRx, m_stats.bytesTx, m_stats.framesOk,
                      m_stats.framesDropped);
}

void registerLinkMetaTypes()
{
    qRegisterMetaType<io::LinkKind>("io::LinkKind");
    qRegisterMetaType<io::LinkState>("io::LinkState");
}

} // namespace io
