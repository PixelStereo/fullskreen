#include "Commands.h"

#include <QDateTime>
#include <algorithm>

namespace cmd {

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static constexpr qint64 kMergeWindowMs = 1500; // continuous gestures merged into a single step

IsfInstance *resolveIsf(Engine *e, int layer, int slot)
{
    Layer *l = e->layer(layer);
    if (!l) return nullptr;
    if (slot < 0) return l->generator.get();
    return slot < int(l->effects.size()) ? l->effects[size_t(slot)].get() : nullptr;
}

// --- SetParam ---------------------------------------------------------------

SetParam::SetParam(Engine *e, int layer, int slot, int input, const IsfValue &before, const IsfValue &after,
                   const QString &label)
    : m_e(e), m_layer(layer), m_slot(slot), m_input(input), m_before(before), m_after(after), m_time(nowMs())
{
    setText(QStringLiteral("Change \"%1\"").arg(label));
}

void SetParam::apply(const IsfValue &v)
{
    Engine::Lock lk(&m_e->mutex());
    IsfInstance *inst = resolveIsf(m_e, m_layer, m_slot);
    if (!inst || m_input < 0 || m_input >= int(inst->inputs().size())) return;
    inst->inputs()[size_t(m_input)].setValue(v);
}

void SetParam::undo() { apply(m_before); }
void SetParam::redo() { apply(m_after); }

bool SetParam::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetParam *>(other);
    if (o->m_layer != m_layer || o->m_slot != m_slot || o->m_input != m_input) return false;
    if (o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

// --- SetLayerProp -----------------------------------------------------------

static QString propText(SetLayerProp::Prop p)
{
    switch (p) {
    case SetLayerProp::Name: return QStringLiteral("Rename Layer");
    case SetLayerProp::Visible: return QStringLiteral("Change Visibility");
    case SetLayerProp::Opacity: return QStringLiteral("Change Opacity");
    case SetLayerProp::Blend: return QStringLiteral("Change Blend Mode");
    case SetLayerProp::Speed: return QStringLiteral("Change Speed");
    case SetLayerProp::Loop: return QStringLiteral("Toggle Loop");
    case SetLayerProp::Volume: return QStringLiteral("Change Volume");
    case SetLayerProp::Muted: return QStringLiteral("Toggle Mute");
    }
    return {};
}

SetLayerProp::SetLayerProp(Engine *e, int layer, Prop prop, const QVariant &before, const QVariant &after)
    : m_e(e), m_layer(layer), m_prop(prop), m_before(before), m_after(after), m_time(nowMs())
{
    setText(propText(prop));
}

QVariant SetLayerProp::read(Engine *e, int layer, Prop prop)
{
    Engine::Lock lk(&e->mutex());
    Layer *l = e->layer(layer);
    if (!l) return {};
    switch (prop) {
    case Name: return l->name;
    case Visible: return l->visible;
    case Opacity: return double(l->opacity);
    case Blend: return int(l->blend);
    case Speed: return l->speed;
    case Loop: return l->loop;
    case Volume: return double(l->volume);
    case Muted: return l->muted;
    }
    return {};
}

void SetLayerProp::apply(const QVariant &v)
{
    if (m_prop == Loop) {
        m_e->setLayerLoop(m_layer, v.toBool());
        return;
    }
    Engine::Lock lk(&m_e->mutex());
    Layer *l = m_e->layer(m_layer);
    if (!l) return;
    switch (m_prop) {
    case Name: l->name = v.toString(); break;
    case Visible: l->visible = v.toBool(); break;
    case Opacity: l->opacity = float(v.toDouble()); break;
    case Blend: l->blend = BlendMode(v.toInt()); break;
    case Speed: l->speed = v.toDouble(); break;
    case Volume: l->volume = float(std::clamp(v.toDouble(), 0.0, 2.0)); break;
    case Muted: l->muted = v.toBool(); break;
    default: break;
    }
}

bool SetLayerProp::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetLayerProp *>(other);
    if (o->m_layer != m_layer || o->m_prop != m_prop) return false;
    if (m_prop != Name && m_prop != Opacity && m_prop != Speed && m_prop != Volume) return false;
    if (o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

// --- SetMapping -------------------------------------------------------------

SetMapping::SetMapping(Engine *e, int layer, const Mapping &before, const Mapping &after, const QString &text, bool nudge)
    : m_e(e), m_layer(layer), m_before(before), m_after(after), m_nudge(nudge), m_time(nowMs())
{
    setText(text);
}

Mapping SetMapping::read(Engine *e, int layer)
{
    Engine::Lock lk(&e->mutex());
    Layer *l = e->layer(layer);
    return l ? l->mapping : Mapping();
}

void SetMapping::apply(const Mapping &m)
{
    Engine::Lock lk(&m_e->mutex());
    Layer *l = m_e->layer(m_layer);
    if (!l) return;
    const unsigned rev = l->mapping.revision;
    l->mapping = m;
    l->mapping.revision = rev + 1;
}

bool SetMapping::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetMapping *>(other);
    if (!m_nudge || !o->m_nudge || o->m_layer != m_layer) return false;
    if (o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

// --- Layers -----------------------------------------------------------------

AddLayer::AddLayer(Engine *e, int index, const QString &text) : m_e(e), m_index(index)
{
    m_json = e->layerJson(index);
    setText(text);
}

void AddLayer::undo() { m_e->removeLayer(m_index); }

void AddLayer::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->insertLayerJson(m_index, m_json);
}

RemoveLayer::RemoveLayer(Engine *e, int index) : m_e(e), m_index(index)
{
    m_json = e->layerJson(index);
    setText(QStringLiteral("Delete \"%1\"").arg(m_json.value("name").toString()));
}

void RemoveLayer::undo() { m_e->insertLayerJson(m_index, m_json); }
void RemoveLayer::redo() { m_e->removeLayer(m_index); }

MoveLayer::MoveLayer(Engine *e, int from, int to) : m_e(e), m_from(from), m_to(to)
{
    setText(QStringLiteral("Reorder Layers"));
}

ReplaceLayer::ReplaceLayer(Engine *e, int index, const QJsonObject &before, const QString &text)
    : m_e(e), m_index(index), m_before(before)
{
    m_after = e->layerJson(index);
    setText(text);
}

void ReplaceLayer::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->replaceLayerJson(m_index, m_after);
}

SetEffects::SetEffects(Engine *e, int index, const QJsonArray &before, const QString &text)
    : m_e(e), m_index(index), m_before(before)
{
    m_after = e->effectsJson(index);
    setText(text);
}

void SetEffects::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->setEffectsJson(m_index, m_after);
}

} // namespace cmd
