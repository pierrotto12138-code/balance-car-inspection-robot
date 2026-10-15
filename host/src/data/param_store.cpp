#include "data/param_store.h"

#include <cmath>

namespace data {

namespace {

/// 参数定义表（范围与默认值**唯一依据**：协议 §3.8 / §4.2）
const QVector<PidParamInfo> kParams = {
    {0x01, "直立环 P", "Balance_Kp", 300.0, 0.0, 1500.0,
     "车往哪边倒就往哪边加速的力度。太大 = 高频抖动/啸叫，太小 = 站不住"},
    {0x02, "直立环 D", "Balance_Kd", 1.0, 0.0, 50.0,
     "抑制角度变化速度（阻尼）。是平衡车里最容易调出抖动的参数"},
    {0x03, "速度环 P", "Velocity_Kp", 80.0, 0.0, 500.0,
     "把轮速拉回 0 的力度。注意：速度环权重是平衡环的十几倍，符号错会把平衡环表现完全盖住"},
    {0x04, "速度环 I", "Velocity_Ki", 0.4, 0.0, 0.82,
     "消除稳态漂移。上界 0.82 = 限幅 8200 ÷ 积分限幅 10000，再大积分一项就能把输出顶满"},
    {0x05, "机械中值", "Zhongzhi", 1.0, -20.0, 20.0,
     "平衡点对应的机械角度偏置。范围 ±20（±180 会让 ±40° 倾角保护失效）"},
    {0x06, "转向环 P", "Turn_Kp", 52.0, 0.0, 500.0,
     "转向跟随力度。验收时“在线改 Turn_Kp 当场生效并回读一致”用的就是它"},
    {0x07, "转向环 D", "Turn_Kd", 0.5, 0.0, 10.0,
     "转向阻尼，仅前后运动时生效"},
};

} // namespace

const QVector<PidParamInfo> &pidParams()
{
    return kParams;
}

const PidParamInfo *pidParamInfo(quint8 id)
{
    for (const PidParamInfo &p : kParams) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

QString pidParamName(quint8 id)
{
    if (const PidParamInfo *p = pidParamInfo(id)) {
        return QString::fromUtf8(p->name);
    }
    switch (id) {
    case 0x00:
        return QStringLiteral("保存到 Flash");
    case 0xFE:
        return QStringLiteral("从 Flash 重新载入");
    case 0xFD:
        return QStringLiteral("调参观测模式");
    case 0xFF:
        return QStringLiteral("全量回读");
    default:
        return QStringLiteral("未知参数 0x%1").arg(id, 2, 16, QLatin1Char('0')).toUpper();
    }
}

ParamCheck checkPidValue(quint8 id, double value, QString *why)
{
    const PidParamInfo *p = pidParamInfo(id);
    if (p == nullptr) {
        if (why != nullptr) {
            *why = QStringLiteral("参数 ID 0x%1 不在协议定义内").arg(id, 2, 16, QLatin1Char('0'));
        }
        return ParamCheck::UnknownParam;
    }
    if (std::isnan(value) || std::isinf(value)) {
        if (why != nullptr) {
            *why = QStringLiteral("值非法（NaN/Inf）—— 协议 §3.8 规定固件一律拒绝");
        }
        return ParamCheck::NotANumber;
    }
    if (value < p->lo || value > p->hi) {
        if (why != nullptr) {
            *why = QStringLiteral("%1 允许范围 %2 ~ %3，填的是 %4 —— 协议 §3.8 规定固件会"
                                  "**拒绝并保持原值**（现象是“改了没生效”，别去查链路）")
                       .arg(QString::fromUtf8(p->name))
                       .arg(p->lo)
                       .arg(p->hi)
                       .arg(value);
        }
        return ParamCheck::OutOfRange;
    }
    return ParamCheck::Ok;
}

ParamStore::ParamStore(QObject *parent) : QObject(parent)
{
    m_clock.start();
}

double ParamStore::current(quint8 id) const
{
    if (m_current.contains(id)) {
        return m_current.value(id);
    }
    if (const PidParamInfo *p = pidParamInfo(id)) {
        return p->defaultValue;
    }
    return std::nan("");
}

bool ParamStore::readbackReceived(quint8 id) const
{
    return m_current.contains(id);
}

double ParamStore::lastRequestValue(quint8 id) const
{
    return m_requested.contains(id) ? m_requested.value(id) : std::nan("");
}

bool ParamStore::lastRequestConfirmed(quint8 id) const
{
    return m_confirmed.value(id, true); // 没下发过 ⇒ 不构成"不一致"
}

void ParamStore::append(quint8 id, double value, const QString &source, bool ok)
{
    ParamSnapshot s;
    s.atMs = nowMs();
    s.id = id;
    s.value = value;
    s.source = source;
    s.ok = ok;
    m_history.append(s);
    emit historyAppended(s);
}

void ParamStore::noteRequest(quint8 id, double value)
{
    m_requested.insert(id, value);
    // 下发后先标"未确认"：哪怕回读马上就来，中间这段时间界面也应显示"待回读"
    m_confirmed.insert(id, false);
    append(id, value, QStringLiteral("下发"), true);
}

void ParamStore::noteReadback(quint8 id, double value)
{
    m_current.insert(id, value);
    bool ok = true;
    if (m_requested.contains(id)) {
        const double want = m_requested.value(id);
        // 浮点比较：按"相对 1e-4 或绝对 1e-6"判等（值与固件 float32 往返有关）
        const double diff = std::fabs(want - value);
        ok = (diff <= 1e-6) || (diff <= 1e-4 * std::fmax(1.0, std::fabs(want)));
        if (ok) {
            m_confirmed.insert(id, true);
        } else {
            emit mismatch(id, want, value);
        }
    }
    append(id, value, QStringLiteral("回读"), ok);
    emit valueUpdated(id, value, true);
}

void ParamStore::noteAck(quint8 id, double value)
{
    // 扩展 ID 的回执：0.0 表示成功（协议 §4.2）；0xFD 的 value 是当前观测模式状态
    append(id, value, QStringLiteral("回执"), true);
    emit valueUpdated(id, value, false);
}

void ParamStore::setSurveyCfg(const SurveyCfg &cfg)
{
    m_survey = cfg;
    emit surveyCfgChanged(m_survey);
}

void ParamStore::clear()
{
    m_current.clear();
    m_requested.clear();
    m_confirmed.clear();
    m_history.clear();
    m_clock.restart();
}

} // namespace data
