#pragma once
// A parameter: one value of a layer, or of one of its shaders (its generator, an effect), declared once by the object
// that holds it — a Layer instantiates its own (Layer::parameters), an IsfInstance its own from its inputs
// (IsfInstance::parameters). Everything that works on parameters asks the composition for its layers, then each layer
// for its parameters: OSC (the address is the path), the timelines and the layers' animations (the animatable ones),
// the Animate menu, the inspector's limits and resets.
//
// A Parameter says what it is — its type, how it goes from one value to another in a fade (its ramp), its granularity,
// its access, its default, its range — and reads / writes the value in its holder (bound at construction).

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <functional>

// What a parameter is, copied out of its holder (the interface works on these: a layer's parameters may be made again
// when its source or its effects change)
struct ParamInfo {
    enum class Type {
        Float,   // a number
        Int,     // a whole number (or one among `values`)
        Bool,    // on / off
        Choice,  // one key among `choices`
        Text,    // a string
        Trigger, // an action without a value (a shader's event)
    };
    // How it goes from one value to another in a fade
    enum class Ramp {
        Cut,    // at once (switches, choices, texts)
        Linear, // through the values in between
        Angle,  // the shortest way round (degrees)
    };
    enum Access { Read = 1, Write = 2, ReadWrite = 3 };

    QString path;  // its address in its layer ("opacity", "spatial/rotation", "fx/<fx>/param/<name>/x"…)
    QString label; // "Spatial › Rotation (°)": the category, then its name
    Type type = Type::Float;
    Ramp ramp = Ramp::Linear;
    int access = ReadWrite;
    double step = 0.01;      // its granularity: a number moves by this (a whole number: 1)
    QVariant defaultValue;   // what a reset puts back
    double min = 0, max = 1; // the range shown (bars, lanes, OSC range)
    double lo = 0, hi = 1;   // the limits a value is kept within
    QList<int> values;       // Int: the values it can take (empty: any whole number within the limits)
    QStringList choices;     // Choice: its keys
    bool animatable = false; // a timeline or an animation can drive it (the numbers)

    bool isNumber() const { return type == Type::Float || type == Type::Int; }
    QString name() const; // its own name, without the category ("Rotation (°)")
};

class Parameter
{
public:
    using Type = ParamInfo::Type;
    using Ramp = ParamInfo::Ramp;
    Parameter(const QString &path, const QString &label, Type type);

    // Its declaration, set fluently where it is made
    Parameter &range(double min, double max);       // shown; also the limits unless limits() says otherwise
    Parameter &limits(double lo, double hi);
    Parameter &step(double s);
    Parameter &ramp(Ramp r);
    Parameter &readOnly();
    Parameter &writeOnly();
    Parameter &animatable(bool on = true);
    Parameter &values(const QList<int> &v);           // Int: among these
    Parameter &choices(const QStringList &keys);      // Choice
    Parameter &byDefault(const QVariant &v);
    Parameter &byDefault(std::function<QVariant()> f); // a default that depends on where the holder is (pixels…)
    // Where its value lives: read and written in its holder (the holder's lock is held by the caller)
    Parameter &bind(std::function<QVariant()> get, std::function<void(const QVariant &)> set);

    // The same parameter under its holder's holder: path and the address under it ("fx/blur/" + "speed")
    Parameter prefixed(const QString &prefix) const
    {
        Parameter p = *this;
        p.m_info.path = prefix + m_info.path;
        return p;
    }
    ParamInfo info() const; // its declaration, its default resolved now
    const QString &path() const { return m_info.path; }
    bool isNumber() const { return m_info.isNumber(); }
    bool animatable() const { return m_info.animatable; }

    QVariant value() const;
    double number() const { return value().toDouble(); }
    // Written within its limits and its granularity (an Int to the nearest of its values, a Choice only to one of
    // its keys); false if it cannot be written or the value is not one it takes
    bool setValue(const QVariant &v);
    bool setNumber(double v) { return setValue(v); }
    bool reset() { return setValue(info().defaultValue); }

private:
    ParamInfo m_info;
    std::function<QVariant()> m_default, m_get;
    std::function<void(const QVariant &)> m_set;
};
