#include "Parameter.h"

#include <algorithm>
#include <cmath>

QString ParamInfo::name() const
{
    const int cut = label.lastIndexOf(QStringLiteral(" › "));
    return cut < 0 ? label : label.mid(cut + 3);
}

Parameter::Parameter(const QString &path, const QString &label, Type type)
{
    m_info.path = path;
    m_info.label = label;
    m_info.type = type;
    switch (type) {
    case Type::Float: m_info.ramp = Ramp::Linear; break;
    case Type::Int:
        m_info.ramp = Ramp::Linear;
        m_info.step = 1;
        break;
    case Type::Trigger:
        m_info.access = ParamInfo::Write;
        m_info.ramp = Ramp::Cut;
        break;
    default: m_info.ramp = Ramp::Cut; break;
    }
    m_info.animatable = m_info.isNumber();
    if (type == Type::Bool) m_info.min = 0, m_info.max = 1, m_info.lo = 0, m_info.hi = 1;
}

Parameter &Parameter::range(double min, double max)
{
    m_info.min = m_info.lo = std::min(min, max);
    m_info.max = m_info.hi = std::max(min, max);
    return *this;
}

Parameter &Parameter::limits(double lo, double hi)
{
    m_info.lo = std::min(lo, hi);
    m_info.hi = std::max(lo, hi);
    return *this;
}

Parameter &Parameter::step(double s)
{
    m_info.step = std::max(0.0, s);
    return *this;
}

Parameter &Parameter::ramp(Ramp r)
{
    m_info.ramp = r;
    return *this;
}

Parameter &Parameter::readOnly()
{
    m_info.access = ParamInfo::Read;
    m_info.animatable = false;
    return *this;
}

Parameter &Parameter::writeOnly()
{
    m_info.access = ParamInfo::Write;
    m_info.animatable = false;
    return *this;
}

Parameter &Parameter::animatable(bool on)
{
    m_info.animatable = on && m_info.isNumber() && (m_info.access & ParamInfo::Write);
    return *this;
}

Parameter &Parameter::values(const QList<int> &v)
{
    m_info.values = v;
    if (!v.isEmpty()) {
        const auto [a, b] = std::minmax_element(v.begin(), v.end());
        range(*a, *b);
    }
    return *this;
}

Parameter &Parameter::choices(const QStringList &keys)
{
    m_info.choices = keys;
    return *this;
}

Parameter &Parameter::byDefault(const QVariant &v)
{
    m_default = [v] { return v; };
    return *this;
}

Parameter &Parameter::byDefault(std::function<QVariant()> f)
{
    m_default = std::move(f);
    return *this;
}

Parameter &Parameter::bind(std::function<QVariant()> get, std::function<void(const QVariant &)> set)
{
    m_get = std::move(get);
    m_set = std::move(set);
    return *this;
}

ParamInfo Parameter::info() const
{
    ParamInfo i = m_info;
    if (m_default) i.defaultValue = m_default();
    return i;
}

QVariant Parameter::value() const
{
    if (!(m_info.access & ParamInfo::Read) || !m_get) return {};
    return m_get();
}

bool Parameter::setValue(const QVariant &v)
{
    if (!(m_info.access & ParamInfo::Write) || !m_set) return false;
    switch (m_info.type) {
    case Type::Float: {
        bool ok = false;
        const double x = v.toDouble(&ok);
        if (!ok || !std::isfinite(x)) return false;
        m_set(std::clamp(x, m_info.lo, m_info.hi));
        return true;
    }
    case Type::Int: {
        bool ok = false;
        const double x = v.toDouble(&ok);
        if (!ok || !std::isfinite(x)) return false;
        int n = int(std::lround(std::clamp(x, m_info.lo, m_info.hi)));
        if (!m_info.values.isEmpty()) { // the nearest of its values
            int best = m_info.values.front();
            for (int c : m_info.values)
                if (std::abs(c - x) < std::abs(best - x)) best = c;
            n = best;
        }
        m_set(n);
        return true;
    }
    case Type::Bool: m_set(v.toBool()); return true;
    case Type::Choice: {
        const QString k = v.toString().toLower();
        if (!m_info.choices.contains(k)) return false;
        m_set(k);
        return true;
    }
    case Type::Text: m_set(v.toString()); return true;
    case Type::Trigger: m_set(QVariant()); return true;
    }
    return false;
}

bool Parameter::isStored() const
{
    return m_info.access == ParamInfo::ReadWrite && m_info.type != Type::Trigger && m_get && m_set;
}

QJsonValue Parameter::json() const
{
    const QVariant v = value();
    switch (m_info.type) {
    case Type::Float: return v.toDouble();
    case Type::Int: return v.toInt();
    case Type::Bool: return v.toBool();
    case Type::Choice:
    case Type::Text: return v.toString();
    case Type::Trigger: break;
    }
    return {};
}

bool Parameter::setJson(const QJsonValue &v)
{
    if (v.isUndefined() || v.isNull()) return false;
    return setValue(v.toVariant());
}
