#include "domain/units.h"

#include <cmath>

namespace domain {
namespace units {

QString number(double v, int decimals)
{
    if (std::isnan(v) || std::isinf(v)) {
        return QStringLiteral("--");
    }
    const double scaled = v * std::pow(10.0, decimals);
    // -0.00 → 0.00：负零来自定点补码，不是真实测量方向
    if (std::fabs(scaled) < 0.5) {
        v = 0.0;
    }
    return QString::number(v, 'f', decimals);
}

QString withUnit(double v, int decimals, const QString &unit)
{
    const QString s = number(v, decimals);
    return unit.isEmpty() ? s : (s + QLatin1Char(' ') + unit);
}

} // namespace units
} // namespace domain
