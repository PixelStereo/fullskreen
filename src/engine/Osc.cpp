#include "Osc.h"
#include "Engine.h"
#include "Zeroconf.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUrl>
#include <QtEndian>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// OSC encoding / decoding
// ---------------------------------------------------------------------------

namespace osc {

static void pad(QByteArray &b)
{
    while (b.size() % 4) b.append('\0');
}

static void putString(QByteArray &b, const QString &s)
{
    b.append(s.toUtf8());
    b.append('\0');
    pad(b);
}

template <typename T> static void putBE(QByteArray &b, T v)
{
    char raw[sizeof(T)];
    qToBigEndian(v, raw);
    b.append(raw, int(sizeof(T)));
}

QByteArray encode(const Message &m)
{
    QByteArray b;
    putString(b, m.address);
    putString(b, QStringLiteral(",") + m.types);
    for (int k = 0; k < m.types.size(); ++k) {
        const QVariant v = m.args.value(k);
        switch (m.types[k].toLatin1()) {
        case 'i': putBE<qint32>(b, qint32(v.toInt())); break;
        case 'h': putBE<qint64>(b, v.toLongLong()); break;
        case 'f': {
            const float f = v.toFloat();
            quint32 u;
            std::memcpy(&u, &f, 4);
            putBE<quint32>(b, u);
            break;
        }
        case 'd': {
            const double d = v.toDouble();
            quint64 u;
            std::memcpy(&u, &d, 8);
            putBE<quint64>(b, u);
            break;
        }
        case 's':
        case 'S': putString(b, v.toString()); break;
        case 'b': {
            const QByteArray blob = v.toByteArray();
            putBE<qint32>(b, qint32(blob.size()));
            b.append(blob);
            pad(b);
            break;
        }
        default: break; // T F N I: no data
        }
    }
    return b;
}

namespace {
struct Reader {
    const QByteArray &b;
    int pos = 0;
    bool ok = true;
    bool need(int n)
    {
        if (pos + n > b.size()) ok = false;
        return ok;
    }
    QString string()
    {
        const int end = b.indexOf('\0', pos);
        if (end < 0) {
            ok = false;
            return {};
        }
        const QString s = QString::fromUtf8(b.constData() + pos, end - pos);
        pos = (end + 4) & ~3;
        if (pos > b.size()) ok = false;
        return s;
    }
    template <typename T> T be()
    {
        if (!need(int(sizeof(T)))) return T();
        const T v = qFromBigEndian<T>(b.constData() + pos);
        pos += int(sizeof(T));
        return v;
    }
};
} // namespace

static bool decodeInto(const QByteArray &packet, std::vector<Message> &out, int depth)
{
    if (packet.isEmpty() || depth > 8) return false;
    if (packet.startsWith(QByteArray("#bundle", 8))) {
        Reader r{packet};
        r.pos = 16; // "#bundle\0" + time tag
        if (packet.size() < 16) return false;
        while (r.ok && r.pos < packet.size()) {
            const qint32 size = r.be<qint32>();
            if (!r.ok || size < 0 || !r.need(size)) return false;
            if (!decodeInto(packet.mid(r.pos, size), out, depth + 1)) return false;
            r.pos += size;
        }
        return r.ok;
    }
    if (packet[0] != '/') return false;
    Reader r{packet};
    Message m;
    m.address = r.string();
    if (!r.ok) return false;
    if (r.pos < packet.size() && packet[r.pos] == ',') {
        const QString tags = r.string();
        if (!r.ok) return false;
        m.types = tags.mid(1);
        for (const QChar c : std::as_const(m.types)) {
            switch (c.toLatin1()) {
            case 'i': m.args << QVariant(int(r.be<qint32>())); break;
            case 'h': m.args << QVariant(qlonglong(r.be<qint64>())); break;
            case 'f': {
                const quint32 u = r.be<quint32>();
                float f;
                std::memcpy(&f, &u, 4);
                m.args << QVariant(double(f));
                break;
            }
            case 'd': {
                const quint64 u = r.be<quint64>();
                double d;
                std::memcpy(&d, &u, 8);
                m.args << QVariant(d);
                break;
            }
            case 's':
            case 'S': m.args << QVariant(r.string()); break;
            case 'b': {
                const qint32 n = r.be<qint32>();
                if (n < 0 || !r.need(n)) return false;
                m.args << QVariant(packet.mid(r.pos, n));
                r.pos = (r.pos + n + 3) & ~3;
                break;
            }
            case 'c': m.args << QVariant(QString(QChar(r.be<qint32>()))); break;
            case 'r':
            case 'm': m.args << QVariant(qlonglong(r.be<quint32>())); break;
            case 't': m.args << QVariant(qlonglong(r.be<qint64>())); break;
            case 'T': m.args << QVariant(true); break;
            case 'F': m.args << QVariant(false); break;
            default: m.args << QVariant(); break; // N, I, arrays: no value
            }
            if (!r.ok) return false;
        }
    }
    out.push_back(m);
    return true;
}

bool decode(const QByteArray &packet, std::vector<Message> &out) { return decodeInto(packet, out, 0); }

// One segment: * ? [set] {alt,alt}
static bool matchSeg(const QString &p, int pi, const QString &s, int si)
{
    while (pi < p.size()) {
        const QChar c = p[pi];
        if (c == '*') {
            for (int k = si; k <= s.size(); ++k)
                if (matchSeg(p, pi + 1, s, k)) return true;
            return false;
        }
        if (c == '?') {
            if (si >= s.size()) return false;
            ++pi;
            ++si;
            continue;
        }
        if (c == '[') {
            const int end = p.indexOf(']', pi);
            if (end < 0 || si >= s.size()) return false;
            QString set = p.mid(pi + 1, end - pi - 1);
            bool negate = set.startsWith('!');
            if (negate) set.remove(0, 1);
            bool in = false;
            for (int k = 0; k < set.size(); ++k) {
                if (k + 2 < set.size() && set[k + 1] == '-') {
                    in |= s[si] >= set[k] && s[si] <= set[k + 2];
                    k += 2;
                } else {
                    in |= s[si] == set[k];
                }
            }
            if (in == negate) return false;
            pi = end + 1;
            ++si;
            continue;
        }
        if (c == '{') {
            const int end = p.indexOf('}', pi);
            if (end < 0) return false;
            const QString rest = p.mid(end + 1);
            for (const QString &alt : p.mid(pi + 1, end - pi - 1).split(','))
                if (matchSeg(alt + rest, 0, s, si)) return true;
            return false;
        }
        if (si >= s.size() || s[si] != c) return false;
        ++pi;
        ++si;
    }
    return si == s.size();
}

bool match(const QString &pattern, const QString &address)
{
    const QStringList p = pattern.split('/'), a = address.split('/');
    if (p.size() != a.size()) return false;
    for (int k = 0; k < p.size(); ++k)
        if (!matchSeg(p[k], 0, a[k], 0)) return false;
    return true;
}

QString safeName(const QString &name)
{
    QString s = name.trimmed();
    for (QChar &c : s)
        if (c.isSpace() || QStringLiteral("#*,/?[]{}!").contains(c)) c = '_';
    return s.isEmpty() ? QStringLiteral("_") : s;
}

} // namespace osc

// ---------------------------------------------------------------------------
// Namespace
// ---------------------------------------------------------------------------

static double num(const QVariant &v)
{
    if (v.typeId() == QMetaType::Bool) return v.toBool() ? 1.0 : 0.0;
    return v.toDouble();
}

static bool truth(const QVariant &v)
{
    if (v.typeId() == QMetaType::QString) {
        const QString s = v.toString().toLower();
        return s == "true" || s == "on" || s == "yes" || s.toDouble() != 0.0;
    }
    if (!v.isValid()) return true; // a bare message (N / I) toggles on
    return num(v) != 0.0;
}

static QJsonObject minMax(double lo, double hi) { return QJsonObject{{"MIN", lo}, {"MAX", hi}}; }

static QJsonObject vals(const QStringList &v)
{
    QJsonArray a;
    for (const QString &s : v) a.append(s);
    return QJsonObject{{"VALS", a}};
}

static QString sourceTypeKey(const Layer &l)
{
    if (l.isGroup) return QStringLiteral("group");
    switch (l.type) {
    case SourceType::Video: return QStringLiteral("video");
    case SourceType::Image: return QStringLiteral("image");
    case SourceType::Isf: return QStringLiteral("isf");
    case SourceType::Audio: return QStringLiteral("audio");
    default: return QStringLiteral("none");
    }
}

// Layer of a given id, lock held
static Layer *find(Engine *e, quint64 id) { return e->layer(e->indexOfId(id)); }

static IsfInstance *findIsf(Engine *e, quint64 id, int slot)
{
    Layer *l = find(e, id);
    if (!l) return nullptr;
    if (slot < 0) return l->generator.get();
    return slot < int(l->effects.size()) ? l->effects[size_t(slot)].get() : nullptr;
}

QString OscNamespace::signature() const
{
    QString s;
    Engine::Lock lk(&m_e->mutex());
    for (int i = 0; i < m_e->layerCount(); ++i) {
        const Layer *l = m_e->layer(i);
        s += QStringLiteral("%1|%2|%3|%4|%5|%6%7|").arg(l->id).arg(l->parent).arg(l->name, sourceTypeKey(*l))
                 .arg(l->hasTransport()).arg(bool(l->audio)).arg(bool(l->generator));
        auto isf = [&](const IsfInstance *inst) {
            if (!inst) return;
            s += inst->name() + ':';
            for (const IsfInput &in : inst->inputs()) s += in.name + QString::number(int(in.type)) + ',';
        };
        isf(l->generator.get());
        for (const auto &fx : l->effects) isf(fx.get());
        s += '\n';
    }
    for (int i = 0; i < m_e->memoryCount(); ++i) s += QStringLiteral("memory:") + m_e->memory(i).name + '\n';
    return s;
}

void OscNamespace::refresh()
{
    const QString sig = signature();
    if (sig == m_signature && !m_nodes.empty()) return;
    m_signature = sig;
    build();
}

const OscNode *OscNamespace::node(const QString &path) const
{
    const auto it = m_nodes.find(path);
    return it == m_nodes.end() ? nullptr : &it->second;
}

QStringList OscNamespace::paths() const
{
    QStringList out;
    for (const auto &[p, n] : m_nodes)
        if (!n.type.isEmpty()) out << p;
    return out;
}

OscNode &OscNamespace::add(const QString &path, const QString &type, int access, const QString &description)
{
    // Containers up to the root
    QString parent = path.section('/', 0, -2);
    QString child = path.section('/', -1);
    QString cur = path;
    while (!cur.isEmpty() && cur != "/") {
        const QString par = cur.section('/', 0, -2).isEmpty() ? QStringLiteral("/") : cur.section('/', 0, -2);
        OscNode &p = m_nodes[par];
        p.path = par;
        const QString seg = cur.section('/', -1);
        if (!p.children.contains(seg)) p.children << seg;
        cur = par;
    }
    (void)parent;
    (void)child;
    OscNode &n = m_nodes[path];
    n.path = path;
    n.type = type;
    n.access = access;
    n.description = description;
    return n;
}

void OscNamespace::build()
{
    m_nodes.clear();
    m_nodes["/"].path = "/";
    Engine *e = m_e;

    // --- Master
    {
        OscNode &n = add("/master/level", "f", 3, "Level");
        n.range = {minMax(0, 1)};
        n.clip = "both";
        n.get = [e] { return QVariantList{e->masterTarget()}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->fadeMaster(std::clamp(num(a[0]), 0.0, 1.0), 0.05);
            return true;
        };
    }
    {
        OscNode &n = add("/master/blackout", "T", 3, "Blackout");
        n.get = [e] { return QVariantList{e->blackout()}; };
        n.set = [e](const QVariantList &a) {
            e->setBlackout(truth(a.value(0)));
            return true;
        };
    }
    {
        OscNode &n = add("/master/fade", "f", 3, "Fade Time");
        n.range = {minMax(0, 30)};
        n.clip = "both";
        n.get = [e] { return QVariantList{e->blackoutFade()}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->setBlackoutFade(std::clamp(num(a[0]), 0.0, 30.0));
            return true;
        };
    }
    {
        OscNode &n = add("/master/volume", "f", 3, "Volume");
        n.range = {minMax(0, 2)};
        n.clip = "both";
        n.get = [e] { return QVariantList{double(e->audioVolume())}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->setAudioVolume(float(std::clamp(num(a[0]), 0.0, 2.0)));
            return true;
        };
    }
    {
        OscNode &n = add("/master/mute", "T", 3, "Mute");
        n.get = [e] { return QVariantList{e->audioMuted()}; };
        n.set = [e](const QVariantList &a) {
            e->setAudioMuted(truth(a.value(0)));
            return true;
        };
    }
    {
        OscNode &n = add("/master/fps", "f", 1, "FPS");
        n.get = [e] { return QVariantList{e->fps()}; };
    }
    for (int axis = 0; axis < 2; ++axis) {
        OscNode &n = add(axis ? "/composition/height" : "/composition/width", "i", 3,
                         axis ? "Height" : "Width");
        n.range = {minMax(16, 16384)};
        n.clip = "both";
        n.get = [e, axis] {
            const QSize s = e->compositionSize();
            return QVariantList{axis ? s.height() : s.width()};
        };
        n.set = [e, axis](const QVariantList &a) {
            if (a.isEmpty()) return false;
            QSize s = e->compositionSize();
            const int v = int(std::lround(num(a[0])));
            if (axis) s.setHeight(v);
            else s.setWidth(v);
            e->setCompositionSize(s);
            return true;
        };
    }

    // --- Memories: recalled by number (1 = first) or from their own node
    {
        OscNode &n = add("/memories/recall", "i", 2, "Recall");
        n.range = {minMax(1, std::max(1, e->memoryCount()))};
        n.set = [e](const QVariantList &a) {
            const int i = int(std::lround(num(a.value(0)))) - 1;
            if (i < 0 || i >= e->memoryCount()) return false;
            e->recallMemory(i);
            return true;
        };
        OscNode &c = add("/memories/count", "i", 1, "Count");
        c.get = [e] { return QVariantList{e->memoryCount()}; };
        m_nodes["/memories"].description = "Memories";
        for (int i = 0; i < e->memoryCount(); ++i) {
            const QString base = QStringLiteral("/memories/%1").arg(i + 1);
            OscNode &r = add(base + "/recall", "N", 2, "Recall");
            r.set = [e, i](const QVariantList &) {
                if (i >= e->memoryCount()) return false;
                e->recallMemory(i);
                return true;
            };
            OscNode &nm = add(base + "/name", "s", 1, "Name");
            nm.get = [e, i] { return QVariantList{e->memory(i).name}; };
            m_nodes[base].description = e->memory(i).name.isEmpty() ? QString::number(i + 1) : e->memory(i).name;
        }
    }

    // --- Layers (top level, then the members of each group)
    struct Item {
        quint64 id, parent;
        QString name;
    };
    std::vector<Item> items;
    {
        Engine::Lock lk(&e->mutex());
        for (int i = 0; i < e->layerCount(); ++i) {
            const Layer *l = e->layer(i);
            items.push_back({l->id, l->parent, l->name});
        }
    }
    add("/layers", QString(), 0, "Layers");
    m_nodes["/master"].description = "Master";
    m_nodes["/composition"].description = "Composition";
    QHash<quint64, QString> prefixOf;
    QHash<QString, int> used;
    for (const Item &it : items) {
        const QString base = it.parent && prefixOf.contains(it.parent) ? prefixOf[it.parent] + "/layers" : QStringLiteral("/layers");
        QString seg = osc::safeName(it.name);
        const QString key = base + '/' + seg;
        if (int n = used.value(key)) seg += QStringLiteral("_%1").arg(n + 1);
        used[key] += 1;
        prefixOf[it.id] = base + '/' + seg;
        addLayer(base + '/' + seg, it.id);
        m_nodes[base + '/' + seg].description = it.name; // real name (spaces included) for the clients that show names
    }
}

namespace {
// Builds the nodes of one layer
struct LayerNodes {
    OscNamespace *ns;
    Engine *e;
    quint64 id;
    std::function<OscNode &(const QString &, const QString &, int, const QString &)> add;

    // Read under the lock; writes refused on a locked layer unless `always`
    OscNode &method(const QString &path, const QString &type, int access, const QString &desc,
                    std::function<QVariantList(Layer &)> get, std::function<bool(int, const QVariantList &)> set,
                    bool always = false)
    {
        OscNode &n = add(path, type, access, desc);
        Engine *eng = e;
        const quint64 lid = id;
        if (get)
            n.get = [eng, lid, get] {
                Engine::Lock lk(&eng->mutex());
                Layer *l = find(eng, lid);
                return l ? get(*l) : QVariantList();
            };
        if (set)
            n.set = [eng, lid, set, always](const QVariantList &a) {
                const int idx = eng->indexOfId(lid);
                if (idx < 0 || (!always && eng->isLocked(idx))) return false;
                return set(idx, a);
            };
        return n;
    }
    // Field written under the lock
    std::function<bool(int, const QVariantList &)> edit(std::function<bool(Layer &, const QVariantList &)> fn)
    {
        Engine *eng = e;
        return [eng, fn](int idx, const QVariantList &a) {
            Engine::Lock lk(&eng->mutex());
            Layer *l = eng->layer(idx);
            return l && fn(*l, a);
        };
    }

    void isfParams(const QString &prefix, int slot, const IsfInstance &inst)
    {
        for (int k = 0; k < int(inst.inputs().size()); ++k) {
            const IsfInput &in = inst.inputs()[size_t(k)];
            if (in.isInputImage) continue;
            QString type;
            QJsonArray range;
            switch (in.type) {
            case IsfInput::Float: type = "f"; range = {minMax(in.fMin, in.fMax)}; break;
            case IsfInput::Bool: type = "T"; break;
            case IsfInput::Long: {
                type = "i";
                QJsonArray v;
                for (int x : in.lValues) v.append(x);
                range = {QJsonObject{{"VALS", v}}};
                break;
            }
            case IsfInput::Point2D:
                type = "ff";
                if (in.hasPointRange) range = {minMax(in.pMin.x(), in.pMax.x()), minMax(in.pMin.y(), in.pMax.y())};
                break;
            case IsfInput::Color: type = "ffff"; range = {minMax(0, 1), minMax(0, 1), minMax(0, 1), minMax(0, 1)}; break;
            case IsfInput::Event: type = "N"; break;
            default: continue; // images and audio: not controllable
            }
            const QString name = in.name;
            Engine *eng = e;
            const quint64 lid = id;
            // The input is found again by name (the shader may have been reloaded)
            auto input = [eng, lid, slot, name]() -> IsfInput * {
                IsfInstance *i = findIsf(eng, lid, slot);
                if (!i) return nullptr;
                for (IsfInput &x : i->inputs())
                    if (x.name == name) return &x;
                return nullptr;
            };
            const IsfInput::Type t = in.type;
            OscNode &n = method(prefix + '/' + osc::safeName(name), type, t == IsfInput::Event ? 2 : 3,
                                in.label.isEmpty() ? name : in.label,
                                t == IsfInput::Event ? std::function<QVariantList(Layer &)>()
                                                     : [input, t](Layer &) -> QVariantList {
                                                           IsfInput *x = input();
                                                           if (!x) return {};
                                                           switch (t) {
                                                           case IsfInput::Float: return {x->fValue};
                                                           case IsfInput::Bool: return {x->bValue};
                                                           case IsfInput::Long: return {x->lValue};
                                                           case IsfInput::Point2D: return {x->pValue.x(), x->pValue.y()};
                                                           case IsfInput::Color:
                                                               return {double(x->cValue[0]), double(x->cValue[1]),
                                                                       double(x->cValue[2]), double(x->cValue[3])};
                                                           default: return {};
                                                           }
                                                       },
                                [eng, input, t](int, const QVariantList &a) {
                                    Engine::Lock lk(&eng->mutex());
                                    IsfInput *x = input();
                                    if (!x) return false;
                                    switch (t) {
                                    case IsfInput::Float:
                                        if (a.isEmpty()) return false;
                                        x->fValue = std::clamp(num(a[0]), std::min(x->fMin, x->fMax), std::max(x->fMin, x->fMax));
                                        return true;
                                    case IsfInput::Bool: x->bValue = truth(a.value(0)); return true;
                                    case IsfInput::Long:
                                        if (a.isEmpty()) return false;
                                        x->lValue = int(std::lround(num(a[0])));
                                        return true;
                                    case IsfInput::Point2D:
                                        if (a.size() < 2) return false;
                                        x->pValue = QPointF(num(a[0]), num(a[1]));
                                        return true;
                                    case IsfInput::Color:
                                        if (a.size() < 3) return false;
                                        for (int c = 0; c < 4 && c < a.size(); ++c) x->cValue[c] = float(std::clamp(num(a[c]), 0.0, 1.0));
                                        return true;
                                    case IsfInput::Event: x->eventFired = true; return true;
                                    default: return false;
                                    }
                                });
            n.range = range;
            if (!range.isEmpty() && t == IsfInput::Float) n.clip = "both";
        }
    }
};
} // namespace

void OscNamespace::addLayer(const QString &P, quint64 id)
{
    Engine *e = m_e;
    LayerNodes L{this, e, id, [this](const QString &p, const QString &t, int a, const QString &d) -> OscNode & {
                     return add(p, t, a, d);
                 }};
    bool isGroup, transport, sound, picture;
    QStringList fxNames;
    std::vector<std::pair<int, QString>> effects;
    bool generator;
    {
        Engine::Lock lk(&e->mutex());
        Layer *l = find(e, id);
        if (!l) return;
        isGroup = l->isGroup;
        transport = l->hasTransport();
        sound = bool(l->audio);
        picture = l->hasPicture();
        generator = l->generator && l->generator->isValid();
        // Generator parameters
        if (generator) L.isfParams(P + "/source/params", -1, *l->generator);
        QHash<QString, int> used;
        for (int k = 0; k < int(l->effects.size()); ++k) {
            QString seg = osc::safeName(l->effects[size_t(k)]->name());
            if (int n = used.value(seg)) seg += QStringLiteral("_%1").arg(n + 1);
            used[osc::safeName(l->effects[size_t(k)]->name())] += 1;
            effects.emplace_back(k, seg);
            if (l->effects[size_t(k)]->isValid()) L.isfParams(P + "/effects/" + seg, k, *l->effects[size_t(k)]);
        }
    }

    L.method(P + "/name", "s", 3, "Name", [](Layer &l) { return QVariantList{l.name}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 const QString n = a.value(0).toString().trimmed();
                 if (n.isEmpty()) return false;
                 l.name = n;
                 return true;
             }));
    L.method(P + "/type", "s", 1, "Type", [](Layer &l) { return QVariantList{sourceTypeKey(l)}; }, nullptr).range =
        {vals({"none", "video", "image", "isf", "audio", "group"})};
    L.method(P + "/visible", "T", 3, "Visible", [](Layer &l) { return QVariantList{l.visible}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 l.visible = truth(a.value(0));
                 return true;
             }),
             true);
    L.method(P + "/locked", "T", 3, "Locked", [](Layer &l) { return QVariantList{l.locked}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 l.locked = truth(a.value(0));
                 return true;
             }),
             true);
    if (picture) {
        OscNode &op = L.method(P + "/opacity", "f", 3, "Opacity", [](Layer &l) { return QVariantList{double(l.opacity)}; },
                               L.edit([](Layer &l, const QVariantList &a) {
                                   if (a.isEmpty()) return false;
                                   l.opacity = float(std::clamp(num(a[0]), 0.0, 1.0));
                                   return true;
                               }));
        op.range = {minMax(0, 1)};
        op.clip = "both";
        L.method(P + "/blend", "s", 3, "Blend", [](Layer &l) { return QVariantList{blendModeKey(l.blend)}; },
                 L.edit([](Layer &l, const QVariantList &a) {
                     const QString k = a.value(0).toString().toLower();
                     if (!QStringList{"normal", "add", "screen", "multiply"}.contains(k)) return false;
                     l.blend = blendModeFromKey(k);
                     return true;
                 }))
            .range = {vals({"normal", "add", "screen", "multiply"})};

        // Crop: part of the source used (normalized, origin top left)
        static const char *kSides[] = {"left", "top", "right", "bottom"};
        static const char *kSideNames[] = {"Left", "Top", "Right", "Bottom"};
        for (int side = 0; side < 4; ++side) {
            OscNode &n = L.method(P + "/source/crop/" + kSides[side], "f", 3, kSideNames[side],
                                  [side](Layer &l) {
                                      const QRectF c = l.crop;
                                      const double v[4] = {c.left(), c.top(), c.right(), c.bottom()};
                                      return QVariantList{v[side]};
                                  },
                                  L.edit([side](Layer &l, const QVariantList &a) {
                                      if (a.isEmpty()) return false;
                                      double v[4] = {l.crop.left(), l.crop.top(), l.crop.right(), l.crop.bottom()};
                                      v[side] = std::clamp(num(a[0]), 0.0, 1.0);
                                      const double minSize = 0.002;
                                      if (side == 0) v[0] = std::min(v[0], v[2] - minSize);
                                      if (side == 2) v[2] = std::max(v[2], v[0] + minSize);
                                      if (side == 1) v[1] = std::min(v[1], v[3] - minSize);
                                      if (side == 3) v[3] = std::max(v[3], v[1] + minSize);
                                      l.crop = QRectF(QPointF(v[0], v[1]), QPointF(v[2], v[3])) & Layer::fullCrop();
                                      return true;
                                  }));
            n.range = {minMax(0, 1)};
            n.clip = "both";
        }

        // Color: balance (temperature, tint), then removed and added
        for (int which = 0; which < 2; ++which) {
            const double range = which ? ColorAdjust::kTintRange : ColorAdjust::kTempRange;
            OscNode &n = L.method(P + (which ? "/color/tint" : "/color/temp"), "f", 3, which ? "Tint" : "Temperature",
                                  [which](Layer &l) { return QVariantList{double(which ? l.color.tint : l.color.temp)}; },
                                  L.edit([which, range](Layer &l, const QVariantList &a) {
                                      if (a.isEmpty()) return false;
                                      (which ? l.color.tint : l.color.temp) = float(std::clamp(num(a[0]), -range, range));
                                      return true;
                                  }));
            n.range = {minMax(-range, range)};
            n.clip = "both";
        }
        // Color: added and removed
        for (int which = 0; which < 2; ++which) {
            OscNode &n = L.method(P + (which ? "/color/remove" : "/color/add"), "fff", 3,
                                  which ? "Remove" : "Add",
                                  [which](Layer &l) {
                                      const float *c = which ? l.color.remove : l.color.add;
                                      return QVariantList{double(c[0]), double(c[1]), double(c[2])};
                                  },
                                  L.edit([which](Layer &l, const QVariantList &a) {
                                      if (a.size() < 3) return false;
                                      float *c = which ? l.color.remove : l.color.add;
                                      for (int k = 0; k < 3; ++k) c[k] = float(std::clamp(num(a[k]), 0.0, 1.0));
                                      return true;
                                  }));
            n.range = {minMax(0, 1), minMax(0, 1), minMax(0, 1)};
            n.clip = "both";
        }

        // Spatial: position (center, composition pixels), scale (% of the composition), corners (normalized)
        L.method(P + "/spatial/position", "ff", 3, "Position",
                 [e](Layer &l) {
                     const QSize c = e->compositionSize();
                     const QPointF p = l.mapping.bounds().center();
                     return QVariantList{p.x() * c.width(), p.y() * c.height()};
                 },
                 L.edit([e](Layer &l, const QVariantList &a) {
                     if (a.size() < 2) return false;
                     const QSize c = e->compositionSize();
                     QRectF b = l.mapping.bounds();
                     b.moveCenter(QPointF(num(a[0]) / c.width(), num(a[1]) / c.height()));
                     l.mapping.setBounds(b);
                     return true;
                 }));
        L.method(P + "/spatial/scale", "ff", 3, "Scale",
                 [](Layer &l) {
                     const QRectF b = l.mapping.bounds();
                     return QVariantList{b.width() * 100.0, b.height() * 100.0};
                 },
                 L.edit([](Layer &l, const QVariantList &a) {
                     if (a.size() < 2) return false;
                     QRectF b = l.mapping.bounds();
                     const QPointF center = b.center();
                     b.setSize(QSizeF(std::max(0.001, num(a[0]) / 100.0), std::max(0.001, num(a[1]) / 100.0)));
                     b.moveCenter(center);
                     l.mapping.setBounds(b);
                     return true;
                 }));
        static const char *kCorners[] = {"tl", "tr", "br", "bl"};
        static const char *kCornerNames[] = {"Top Left", "Top Right", "Bottom Right", "Bottom Left"};
        for (int k = 0; k < 4; ++k)
            L.method(P + "/spatial/corners/" + kCorners[k], "ff", 3, kCornerNames[k],
                     [k](Layer &l) { return QVariantList{l.mapping.corners[k].x(), l.mapping.corners[k].y()}; },
                     L.edit([k](Layer &l, const QVariantList &a) {
                         if (a.size() < 2) return false;
                         l.mapping.setCorner(k, QPointF(num(a[0]), num(a[1])));
                         return true;
                     }));

        // Effects
        L.method(P + "/effects/enabled", "T", 3, "Enabled",
                 [](Layer &l) { return QVariantList{l.effectsEnabled}; }, L.edit([](Layer &l, const QVariantList &a) {
                     l.effectsEnabled = truth(a.value(0));
                     return true;
                 }));
        for (const auto &[k, seg] : effects) {
            const int slot = k;
            L.method(P + "/effects/" + seg + "/enabled", "T", 3, "Enabled",
                     [slot](Layer &l) {
                         return slot < int(l.effects.size()) ? QVariantList{l.effects[size_t(slot)]->enabled} : QVariantList();
                     },
                     L.edit([slot](Layer &l, const QVariantList &a) {
                         if (slot >= int(l.effects.size())) return false;
                         l.effects[size_t(slot)]->enabled = truth(a.value(0));
                         return true;
                     }));
        }
    }

    if (!isGroup) {
        L.method(P + "/source/file", "s", 3, "File",
                 [](Layer &l) { return QVariantList{l.sourcePath}; },
                 [e](int idx, const QVariantList &a) {
                     const QString path = a.value(0).toString();
                     if (path.isEmpty()) {
                         e->clearLayerSource(idx);
                         return true;
                     }
                     const QString ext = QFileInfo(path).suffix().toLower();
                     if (ext == "fs" || ext == "frag") return e->setLayerIsf(idx, path);
                     return e->setLayerFile(idx, path);
                 });
    }
    if (transport) {
        L.method(P + "/source/play", "T", 3, "Play", [](Layer &l) { return QVariantList{l.playing}; },
                 [e](int idx, const QVariantList &a) {
                     e->setLayerPlaying(idx, truth(a.value(0)));
                     return true;
                 },
                 true);
        L.method(P + "/source/restart", "N", 2, "Restart", nullptr,
                 [e](int idx, const QVariantList &) {
                     double from;
                     {
                         Engine::Lock lk(&e->mutex());
                         Layer *l = e->layer(idx);
                         if (!l) return false;
                         const Timeline t = l->timeline();
                         from = l->speed < 0 ? t.hi() : t.lo();
                     }
                     e->seekLayer(idx, from);
                     e->setLayerPlaying(idx, true);
                     return true;
                 },
                 true);
        L.method(P + "/source/position", "f", 3, "Position", [](Layer &l) { return QVariantList{l.position()}; },
                 [e](int idx, const QVariantList &a) {
                     if (a.isEmpty()) return false;
                     e->seekLayer(idx, num(a[0]));
                     return true;
                 },
                 true);
        L.method(P + "/source/duration", "f", 1, "Duration",
                 [](Layer &l) { return QVariantList{l.duration()}; }, nullptr);
        OscNode &sp = L.method(P + "/source/speed", "f", 3, "Speed",
                               [](Layer &l) { return QVariantList{l.speed}; }, [e](int idx, const QVariantList &a) {
                                   if (a.isEmpty()) return false;
                                   e->setLayerSpeed(idx, std::clamp(num(a[0]), -8.0, 8.0));
                                   return true;
                               });
        sp.range = {minMax(-8, 8)};
        sp.clip = "both";
        L.method(P + "/source/mode", "s", 3, "Play Mode", [](Layer &l) { return QVariantList{playModeKey(l.mode)}; },
                 [e](int idx, const QVariantList &a) {
                     const QString k = a.value(0).toString().toLower();
                     if (!QStringList{"oneshot", "loop", "pingpong", "stop"}.contains(k)) return false;
                     e->setLayerPlayMode(idx, playModeFromKey(k));
                     return true;
                 })
            .range = {vals({"oneshot", "loop", "pingpong", "stop"})};
        for (int which = 0; which < 2; ++which)
            L.method(P + (which ? "/source/out" : "/source/in"), "f", 3,
                     which ? "Out" : "In",
                     [which](Layer &l) {
                         return QVariantList{which ? (l.outPoint < 0 ? l.duration() : l.outPoint) : l.inPoint};
                     },
                     [e, which](int idx, const QVariantList &a) {
                         if (a.isEmpty()) return false;
                         double in, out;
                         {
                             Engine::Lock lk(&e->mutex());
                             Layer *l = e->layer(idx);
                             if (!l) return false;
                             in = l->inPoint;
                             out = l->outPoint;
                         }
                         (which ? out : in) = num(a[0]);
                         e->setLayerInOut(idx, in, out);
                         return true;
                     });
    }
    if (sound) {
        OscNode &v = L.method(P + "/source/volume", "f", 3, "Volume",
                              [](Layer &l) { return QVariantList{double(l.volume)}; }, [e](int idx, const QVariantList &a) {
                                  if (a.isEmpty()) return false;
                                  e->setLayerVolume(idx, float(num(a[0])));
                                  return true;
                              });
        v.range = {minMax(0, 2)};
        v.clip = "both";
        L.method(P + "/source/mute", "T", 3, "Mute", [](Layer &l) { return QVariantList{l.muted}; },
                 [e](int idx, const QVariantList &a) {
                     e->setLayerMuted(idx, truth(a.value(0)));
                     return true;
                 });
    }
    if (isGroup) add(P + "/layers", QString(), 0, "Layers");
    // Readable names of the containers
    static const std::pair<const char *, const char *> kNames[] = {
        {"/source", "Source"}, {"/source/crop", "Crop"}, {"/source/params", "Parameters"}, {"/color", "Color"},
        {"/spatial", "Spatial"}, {"/spatial/corners", "Corners"}, {"/effects", "Effects"}};
    for (const auto &[suffix, name] : kNames) {
        auto it = m_nodes.find(P + suffix);
        if (it != m_nodes.end()) it->second.description = name;
    }
    for (const auto &[k, seg] : effects) {
        auto it = m_nodes.find(P + "/effects/" + seg);
        if (it != m_nodes.end() && it->second.description.isEmpty()) it->second.description = seg;
    }
}

QJsonValue OscNamespace::jsonValue(const OscNode &n, const QVariant &v, int k)
{
    const QChar t = k < n.type.size() ? n.type[k] : QChar();
    if (t == 'T' || t == 'F') return v.toBool();
    if (t == 'i' || t == 'h') return v.toLongLong();
    if (t == 's') return v.toString();
    if (t == 'N' || t == 'I') return QJsonValue();
    const double d = v.toDouble();
    return std::isfinite(d) ? d : 0.0;
}

QJsonObject OscNamespace::toJson(const QString &path) const
{
    const OscNode *n = node(path);
    if (!n) return {};
    QJsonObject o;
    o["FULL_PATH"] = n->path;
    o["ACCESS"] = n->access;
    if (!n->description.isEmpty()) o["DESCRIPTION"] = n->description;
    if (!n->type.isEmpty()) {
        o["TYPE"] = n->type;
        if ((n->access & 1) && n->get) {
            const QVariantList v = n->get();
            QJsonArray a;
            for (int k = 0; k < v.size(); ++k) a.append(jsonValue(*n, v[k], k));
            o["VALUE"] = a;
        }
        if (!n->range.isEmpty()) o["RANGE"] = n->range;
        if (!n->clip.isEmpty()) {
            QJsonArray c;
            for (int k = 0; k < n->type.size(); ++k) c.append(n->clip);
            o["CLIPMODE"] = c;
        }
    }
    if (!n->children.isEmpty() || n->type.isEmpty()) { // a container always has CONTENTS (even empty)
        QJsonObject contents;
        for (const QString &c : n->children) contents[c] = toJson(path == "/" ? "/" + c : path + "/" + c);
        o["CONTENTS"] = contents;
    }
    return o;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

OscServer::OscServer(Engine *engine, QObject *parent) : QObject(parent), m_e(engine), m_ns(engine), m_listenTimer(this)
{
    m_listenTimer.setInterval(40);
    connect(&m_listenTimer, &QTimer::timeout, this, &OscServer::pushListened);
}

OscServer::~OscServer() { stop(); }

bool OscServer::start(quint16 oscPort, quint16 queryPort, const QString &name, bool announce)
{
    stop();
    m_name = name;
    m_udp = new QUdpSocket(this);
    if (!m_udp->bind(QHostAddress::AnyIPv4, oscPort)) {
        m_status = QStringLiteral("OSC: UDP port %1 unavailable (%2)").arg(oscPort).arg(m_udp->errorString());
        stop();
        return false;
    }
    connect(m_udp, &QUdpSocket::readyRead, this, &OscServer::onUdp);
    m_http = new QTcpServer(this);
    if (!m_http->listen(QHostAddress::Any, queryPort)) {
        m_status = QStringLiteral("OSCQuery: TCP port %1 unavailable (%2)").arg(queryPort).arg(m_http->errorString());
        stop();
        return false;
    }
    connect(m_http, &QTcpServer::newConnection, this, &OscServer::onConnection);
    m_listenTimer.start();
    m_status = QStringLiteral("OSC on UDP %1 · OSCQuery on http://<this machine>:%2").arg(this->oscPort()).arg(this->queryPort());
    if (announce) {
        m_zeroconf = new Zeroconf(this);
        const QString instance = QStringLiteral("%1 (%2)").arg(name, QHostInfo::localHostName().section('.', 0, 0));
        QString err;
        if (m_zeroconf->start(instance, {{"_oscjson._tcp", this->queryPort(), {{"txtvers", "1"}}}, {"_osc._udp", this->oscPort(), {{"txtvers", "1"}}}}, &err))
            m_status += QStringLiteral(" · announced as \"%1\"").arg(instance);
        else
            m_status += QStringLiteral(" · zeroconf: ") + err;
    }
    return true;
}

void OscServer::stop()
{
    delete m_zeroconf; // goodbye sent
    m_zeroconf = nullptr;
    m_listenTimer.stop();
    for (auto &c : m_clients) c.socket->deleteLater();
    m_clients.clear();
    delete m_udp;
    m_udp = nullptr;
    delete m_http;
    m_http = nullptr;
    m_status = QStringLiteral("OSC off");
}

bool OscServer::isRunning() const { return m_udp && m_http; }
quint16 OscServer::oscPort() const { return m_udp ? m_udp->localPort() : 0; }
quint16 OscServer::queryPort() const { return m_http ? m_http->serverPort() : 0; }

void OscServer::onUdp()
{
    bool any = false;
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_udp->receiveDatagram();
        std::vector<osc::Message> msgs;
        if (!osc::decode(d.data(), msgs)) continue;
        for (const osc::Message &m : msgs) any |= handleMessage(m);
    }
    if (any) emit edited();
}

int OscServer::handlePacket(const QByteArray &packet)
{
    std::vector<osc::Message> msgs;
    if (!osc::decode(packet, msgs)) return 0;
    int n = 0;
    for (const osc::Message &m : msgs) n += handleMessage(m);
    if (n) emit edited();
    return n;
}

bool OscServer::handleMessage(const osc::Message &m)
{
    m_ns.refresh();
    static const QString kPattern = QStringLiteral("*?[]{}");
    bool pattern = false;
    for (QChar c : m.address) pattern |= kPattern.contains(c);
    QStringList targets;
    if (pattern) {
        for (const QString &p : m_ns.paths())
            if (osc::match(m.address, p)) targets << p;
    } else {
        targets << m.address;
    }
    bool any = false;
    for (const QString &p : targets) {
        const OscNode *n = m_ns.node(p);
        if (!n || !(n->access & 2) || !n->set) continue;
        any |= n->set(m.args);
    }
    return any;
}

QJsonObject OscServer::hostInfo() const
{
    return QJsonObject{
        {"NAME", m_name},
        {"OSC_PORT", int(oscPort())},
        {"OSC_TRANSPORT", "UDP"},
        {"WS_PORT", int(queryPort())},
        {"EXTENSIONS", QJsonObject{{"ACCESS", true}, {"VALUE", true}, {"RANGE", true}, {"DESCRIPTION", true},
                                   {"TAGS", false}, {"EXTENDED_TYPE", false}, {"UNIT", false}, {"CRITICAL", false},
                                   {"CLIPMODE", true}, {"LISTEN", true}, {"PATH_CHANGED", true}}},
    };
}

QByteArray OscServer::httpGet(const QString &target, int *status)
{
    m_ns.refresh();
    QString path = QUrl::fromPercentEncoding(target.section('?', 0, 0).toUtf8());
    const QString query = target.section('?', 1).section('&', 0, 0).toUpper();
    if (path.isEmpty()) path = "/";
    while (path.size() > 1 && path.endsWith('/')) path.chop(1);
    *status = 200;
    if (query == "HOST_INFO") return QJsonDocument(hostInfo()).toJson(QJsonDocument::Compact);
    const OscNode *n = m_ns.node(path);
    if (!n) {
        *status = 404;
        return {};
    }
    const QJsonObject o = m_ns.toJson(path);
    if (query.isEmpty()) return QJsonDocument(o).toJson(QJsonDocument::Compact);
    static const QStringList known = {"FULL_PATH", "CONTENTS", "TYPE", "VALUE", "RANGE", "ACCESS", "DESCRIPTION", "CLIPMODE"};
    if (!known.contains(query)) {
        *status = 400;
        return {};
    }
    if (!o.contains(query)) {
        *status = 204;
        return {};
    }
    return QJsonDocument(QJsonObject{{query, o.value(query)}}).toJson(QJsonDocument::Compact);
}

void OscServer::onConnection()
{
    while (QTcpSocket *s = m_http->nextPendingConnection()) {
        Client c;
        c.socket = s;
        m_clients.insert(s, c);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_clients.remove(s);
            s->deleteLater();
        });
    }
}

static QByteArray httpResponse(int status, const QByteArray &body, const QByteArray &type = "application/json")
{
    static const QHash<int, QByteArray> text = {{200, "OK"}, {204, "No Content"}, {400, "Bad Request"}, {404, "Not Found"}};
    QByteArray r = "HTTP/1.1 " + QByteArray::number(status) + ' ' + text.value(status, "Error") + "\r\n";
    if (status == 200) r += "Content-Type: " + type + "\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    return r + body;
}

void OscServer::onData(QTcpSocket *s)
{
    auto it = m_clients.find(s);
    if (it == m_clients.end()) return;
    Client &c = *it;
    c.buffer += s->readAll();
    if (!c.websocket) {
        const int end = c.buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (c.buffer.size() > 65536) s->disconnectFromHost();
            return;
        }
        const QList<QByteArray> lines = c.buffer.left(end).split('\n');
        c.buffer.remove(0, end + 4);
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        QHash<QByteArray, QByteArray> headers;
        for (int k = 1; k < lines.size(); ++k) {
            const int colon = lines[k].indexOf(':');
            if (colon > 0) headers.insert(lines[k].left(colon).trimmed().toLower(), lines[k].mid(colon + 1).trimmed());
        }
        if (request.value(0) != "GET") {
            s->write(httpResponse(400, {}));
            s->disconnectFromHost();
            return;
        }
        if (headers.value("upgrade").toLower() == "websocket") {
            const QByteArray accept = QCryptographicHash::hash(headers.value("sec-websocket-key") +
                                                                   "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                                               QCryptographicHash::Sha1)
                                          .toBase64();
            s->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
            c.websocket = true;
            m_treeSignature = m_ns.signature(); // the client has just read the tree
        } else {
            int status = 200;
            const QByteArray body = httpGet(QString::fromUtf8(request.value(1)), &status);
            s->write(httpResponse(status, body));
            s->disconnectFromHost();
            return;
        }
    }
    // WebSocket frames (client frames are masked)
    while (c.websocket) {
        const QByteArray &b = c.buffer;
        if (b.size() < 2) return;
        const int opcode = b[0] & 0x0F;
        const bool masked = b[1] & 0x80;
        quint64 len = quint8(b[1]) & 0x7F;
        int pos = 2;
        if (len == 126) {
            if (b.size() < 4) return;
            len = qFromBigEndian<quint16>(b.constData() + 2);
            pos = 4;
        } else if (len == 127) {
            if (b.size() < 10) return;
            len = qFromBigEndian<quint64>(b.constData() + 2);
            pos = 10;
        }
        if (len > 16 * 1024 * 1024) {
            s->disconnectFromHost();
            return;
        }
        const int maskPos = pos;
        if (masked) pos += 4;
        if (quint64(b.size()) < pos + len) return;
        QByteArray payload = b.mid(pos, int(len));
        if (masked)
            for (int k = 0; k < payload.size(); ++k) payload[k] = char(payload[k] ^ b[maskPos + (k & 3)]);
        c.buffer.remove(0, pos + int(len));
        onWsFrame(c, opcode, payload);
        if (!m_clients.contains(s)) return;
    }
}

void OscServer::sendWs(QTcpSocket *s, int opcode, const QByteArray &payload)
{
    QByteArray f;
    f.append(char(0x80 | opcode));
    if (payload.size() < 126) {
        f.append(char(payload.size()));
    } else if (payload.size() < 65536) {
        f.append(char(126));
        char n[2];
        qToBigEndian(quint16(payload.size()), n);
        f.append(n, 2);
    } else {
        f.append(char(127));
        char n[8];
        qToBigEndian(quint64(payload.size()), n);
        f.append(n, 8);
    }
    s->write(f + payload);
}

void OscServer::onWsFrame(Client &c, int opcode, const QByteArray &payload)
{
    switch (opcode) {
    case 1: { // text: LISTEN / IGNORE
        const QJsonObject o = QJsonDocument::fromJson(payload).object();
        const QString cmd = o.value("COMMAND").toString(), path = o.value("DATA").toString();
        if (cmd == "LISTEN") {
            c.listening.insert(path);
            c.sent.remove(path);
        } else if (cmd == "IGNORE") {
            c.listening.remove(path);
            c.sent.remove(path);
        }
        break;
    }
    case 2: handlePacket(payload); break; // binary: OSC
    case 8:
        sendWs(c.socket, 8, {});
        c.socket->disconnectFromHost();
        break;
    case 9: sendWs(c.socket, 10, payload); break; // ping -> pong
    default: break;
    }
}

void OscServer::pushListened()
{
    // The tree changed (layer added, renamed, source or effect changed): clients fetch it again
    bool websockets = false;
    for (const Client &c : std::as_const(m_clients)) websockets |= c.websocket;
    if (websockets) {
        const QString sig = m_ns.signature();
        if (sig != m_treeSignature) {
            m_treeSignature = sig;
            const QByteArray msg = QJsonDocument(QJsonObject{{"COMMAND", "PATH_CHANGED"}, {"DATA", "/"}}).toJson(QJsonDocument::Compact);
            for (const Client &c : std::as_const(m_clients))
                if (c.websocket) sendWs(c.socket, 1, msg);
        }
    }
    bool anyone = false;
    for (const Client &c : std::as_const(m_clients)) anyone |= c.websocket && !c.listening.isEmpty();
    if (!anyone) return;
    m_ns.refresh();
    QHash<QString, QVariantList> values; // read once per path
    for (Client &c : m_clients) {
        if (!c.websocket) continue;
        for (const QString &p : std::as_const(c.listening)) {
            const OscNode *n = m_ns.node(p);
            if (!n || !n->get) continue;
            if (!values.contains(p)) values.insert(p, n->get());
            const QVariantList v = values.value(p);
            if (v.isEmpty() || c.sent.value(p) == v) continue;
            c.sent.insert(p, v);
            osc::Message m;
            m.address = p;
            for (int k = 0; k < n->type.size(); ++k) {
                const QChar t = n->type[k];
                m.types += (t == 'T' || t == 'F') ? (v.value(k).toBool() ? QChar('T') : QChar('F')) : t;
            }
            m.args = v;
            sendWs(c.socket, 2, osc::encode(m));
        }
    }
}
