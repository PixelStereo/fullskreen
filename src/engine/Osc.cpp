#include "Osc.h"
#include "EngineInternal.h"
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

QStringList uniqueSegments(const QStringList &names)
{
    QStringList out;
    QHash<QString, int> used;
    for (const QString &n : names) {
        const QString base = safeName(n);
        QString seg = base;
        if (int k = used.value(base)) seg += QStringLiteral("_%1").arg(k + 1);
        used[base] += 1;
        out << seg;
    }
    return out;
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
    if (l.isViewport) return QStringLiteral("viewport");
    if (l.isGroup) return QStringLiteral("group");
    switch (l.type) {
    case SourceType::Video: return QStringLiteral("video");
    case SourceType::Image: return QStringLiteral("image");
    case SourceType::Isf: return QStringLiteral("isf");
    case SourceType::Audio: return QStringLiteral("audio");
    case SourceType::Layer: return QStringLiteral("layer");
    case SourceType::Text: return QStringLiteral("text");
    default: return QStringLiteral("none");
    }
}

// Layer of a given id, lock held
static Layer *find(Engine *e, quint64 id) { return e->layer(e->indexOfId(id)); }

static QStringList blendKeys() // every blend mode, by its key
{
    QStringList k;
    for (BlendMode m : kBlendModes) k << blendModeKey(m);
    return k;
}

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
        for (const Animation &a : l->anims)
            if (!a.tracks.empty()) s += QStringLiteral("anim:") + a.tracks.front().param + ',';
        s += '\n';
    }
    for (int i = 0; i < m_e->snapshotCount(); ++i) s += QStringLiteral("snapshot:") + m_e->snapshot(i).name + '\n';
    for (int i = 0; i < m_e->animationCount(); ++i) s += QStringLiteral("timeline:") + m_e->animation(i).name + '\n';
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

    // --- Composition: opacity, sound volume, blackout (picture and sound), size, frame rate
    {
        OscNode &n = add("/composition/opacity", "f", 3, "Opacity");
        n.range = {minMax(0, 1)};
        n.clip = "both";
        n.get = [e] { return QVariantList{e->compositionOpacityTarget()}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->fadeCompositionOpacity(std::clamp(num(a[0]), 0.0, 1.0), 0.05);
            return true;
        };
    }
    {
        OscNode &n = add("/composition/volume", "f", 3, "Volume");
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
        // Speed of all time (media, shaders, timelines, fades, sequences), and the pause that holds it at 0
        OscNode &n = add("/composition/speed", "f", 3, "Speed (0 = stopped, 1 = normal)");
        n.range = {minMax(0, 10)};
        n.clip = "both";
        n.get = [e] { return QVariantList{e->compositionSpeed()}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->setCompositionSpeed(num(a[0]));
            return true;
        };
        OscNode &p = add("/composition/pause", "T", 3, "Pause (speed 0 until released)");
        p.get = [e] { return QVariantList{e->paused()}; };
        p.set = [e](const QVariantList &a) {
            e->setPaused(truth(a.value(0)));
            return true;
        };
    }
    {
        OscNode &n = add("/composition/blackout", "T", 3, "Blackout (picture and sound)");
        n.get = [e] { return QVariantList{e->blackout()}; };
        n.set = [e](const QVariantList &a) {
            e->setBlackout(truth(a.value(0)));
            return true;
        };
        OscNode &f = add("/composition/blackout/fade", "f", 3, "Fade Time (s)");
        f.range = {minMax(0, 30)};
        f.clip = "both";
        f.get = [e] { return QVariantList{e->blackoutFade()}; };
        f.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            e->setBlackoutFade(std::clamp(num(a[0]), 0.0, 30.0));
            return true;
        };
    }
    {
        // Frame rate of the render (0: the refresh rate of the screen); what is measured is under /fps/measured
        OscNode &n = add("/composition/fps", "f", 3, "Frame Rate (0 = screen refresh)");
        n.range = {minMax(0, 240)};
        n.clip = "both";
        n.get = [e] { return QVariantList{e->effectiveRender().frameRate}; };
        n.set = [e](const QVariantList &a) {
            if (a.isEmpty()) return false;
            Engine::RenderSettings r = e->renderSettings();
            r.frameRate = std::clamp(num(a[0]), 0.0, 240.0);
            e->setRenderSettings(r);
            return true;
        };
        add("/composition/fps/measured", "f", 1, "Measured").get = [e] { return QVariantList{e->fps()}; };
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

    // --- Snapshots: recalled by number (1 = first) or from their own node
    {
        OscNode &n = add("/snapshot/recall", "i", 2, "Recall");
        n.range = {minMax(1, std::max(1, e->snapshotCount()))};
        n.set = [e](const QVariantList &a) {
            const int i = int(std::lround(num(a.value(0)))) - 1;
            if (i < 0 || i >= e->snapshotCount()) return false;
            e->recallSnapshot(i);
            return true;
        };
        OscNode &c = add("/snapshot/count", "i", 1, "Count");
        c.get = [e] { return QVariantList{e->snapshotCount()}; };
        m_nodes["/snapshot"].description = "Snapshots";
        for (int i = 0; i < e->snapshotCount(); ++i) {
            const QString base = QStringLiteral("/snapshot/%1").arg(i + 1);
            OscNode &r = add(base + "/recall", "N", 2, "Recall");
            r.set = [e, i](const QVariantList &) {
                if (i >= e->snapshotCount()) return false;
                e->recallSnapshot(i);
                return true;
            };
            OscNode &nm = add(base + "/name", "s", 1, "Name");
            nm.get = [e, i] { return QVariantList{e->snapshot(i).name}; };
            m_nodes[base].description = e->snapshot(i).name.isEmpty() ? QString::number(i + 1) : e->snapshot(i).name;
        }
    }

    // --- Sequence: GO, GO BACK, a step of the current sequence (1 = first), where it is
    {
        add("/sequence/go", "N", 2, "GO").set = [e](const QVariantList &) { return e->sequenceGo(); };
        add("/sequence/back", "N", 2, "GO Back").set = [e](const QVariantList &) { return e->sequenceBack(); };
        OscNode &st = add("/sequence/step", "i", 3, "Step");
        st.get = [e] { return QVariantList{e->sequencePosition() + 1}; };
        st.set = [e](const QVariantList &a) { return e->sequenceGoTo(int(std::lround(num(a.value(0)))) - 1); };
        OscNode &cur = add("/sequence/current", "i", 3, "Current Sequence");
        cur.get = [e] { return QVariantList{e->currentSequence() + 1}; };
        cur.set = [e](const QVariantList &a) {
            const int i = int(std::lround(num(a.value(0)))) - 1;
            if (i < 0 || i >= e->sequenceCount()) return false;
            e->setCurrentSequence(i);
            return true;
        };
        m_nodes["/sequence"].description = "Sequence";
    }

    // --- Timelines (1 = first): their transport
    {
        m_nodes["/timeline"].description = "Timelines";
        using A = Engine::AnimAction;
        for (int i = 0; i < e->animationCount(); ++i) {
            const Engine::Animation an = e->animation(i);
            const QString base = QStringLiteral("/timeline/%1").arg(i + 1);
            const quint64 id = an.id;
            struct Cmd {
                const char *key;
                A act;
                const char *label;
            };
            for (const Cmd &c : {Cmd{"play", A::Play, "Play"}, Cmd{"pause", A::Pause, "Pause"}, Cmd{"stop", A::Stop, "Stop"},
                                 Cmd{"rewind", A::Rewind, "Rewind"}})
                add(base + "/" + c.key, "N", 2, c.label).set = [e, id, act = c.act](const QVariantList &) {
                    if (e->indexOfAnimation(id) < 0) return false;
                    e->controlAnimation(id, act);
                    return true;
                };
            OscNode &sk = add(base + "/seek", "f", 3, "Seek (s)");
            sk.get = [e, id] { return QVariantList{e->animation(e->indexOfAnimation(id)).clock}; };
            sk.set = [e, id](const QVariantList &a) {
                if (e->indexOfAnimation(id) < 0) return false;
                e->controlAnimation(id, A::Seek, num(a.value(0)));
                return true;
            };
            OscNode &sp = add(base + "/speed", "f", 3, "Speed (1 = normal)");
            sp.get = [e, id] { return QVariantList{e->animation(e->indexOfAnimation(id)).speed}; };
            sp.set = [e, id](const QVariantList &a) {
                if (e->indexOfAnimation(id) < 0) return false;
                e->controlAnimation(id, A::Speed, num(a.value(0)));
                return true;
            };
            OscNode &pl = add(base + "/playing", "T", 1, "Playing");
            pl.get = [e, id] { return QVariantList{e->animation(e->indexOfAnimation(id)).state == Engine::AnimState::Playing}; };
            m_nodes[base].description = an.name.isEmpty() ? QString::number(i + 1) : an.name;
        }
    }

    // --- Viewports, then the layers (top level, then the contents of each group, at any depth)
    struct Item {
        quint64 id, parent;
        QString name;
        bool viewport;
    };
    std::vector<Item> items;
    {
        Engine::Lock lk(&e->mutex());
        for (int i = 0; i < e->layerCount(); ++i) {
            const Layer *l = e->layer(i);
            items.push_back({l->id, l->parent, l->name, l->isViewport});
        }
    }
    add("/layer", QString(), 0, "Layers");
    add("/viewport", QString(), 0, "Viewports");
    m_nodes["/composition"].description = "Composition";
    QHash<quint64, QString> prefixOf;
    QHash<QString, int> used;
    for (const Item &it : items) {
        const QString base = it.viewport ? QStringLiteral("/viewport")
                             : it.parent && prefixOf.contains(it.parent) ? prefixOf[it.parent] + "/layer"
                                                                         : QStringLiteral("/layer");
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

    // The layer's parameters (Parameter.h), each at its address; and the numbers that go together (x y; r g b;
    // r g b a) also at their common address, all at once
    void parameters(const QString &P, Layer &layer)
    {
        auto typeOf = [](const ParamInfo &i) -> QString {
            switch (i.type) {
            case ParamInfo::Type::Float: return QStringLiteral("f");
            case ParamInfo::Type::Int: return QStringLiteral("i");
            case ParamInfo::Type::Bool: return QStringLiteral("T");
            case ParamInfo::Type::Choice:
            case ParamInfo::Type::Text: return QStringLiteral("s");
            case ParamInfo::Type::Trigger: return QStringLiteral("N");
            }
            return {};
        };
        auto rangeOf = [](const ParamInfo &i) {
            if (!i.values.isEmpty()) {
                QJsonArray v;
                for (int x : i.values) v.append(x);
                return QJsonObject{{"VALS", v}};
            }
            if (i.type == ParamInfo::Type::Choice) return vals(i.choices);
            return minMax(i.min, i.max);
        };
        // Read and written through the layer's parameter, found again by its address
        auto read = [](const QString &path) {
            return [path](Layer &l) -> QVariantList {
                const Parameter *p = l.parameter(path);
                const QVariant v = p ? p->value() : QVariant();
                return v.isValid() ? QVariantList{v} : QVariantList();
            };
        };
        QHash<QString, QStringList> parts; // a common address → the last segments of its numbers
        QHash<QString, ParamInfo> byPath;
        for (const Parameter *p : layer.parameters()) {
            const ParamInfo i = p->info();
            byPath[i.path] = i;
            const QString path = i.path;
            const bool number = i.isNumber();
            OscNode &node = method(P + '/' + path, typeOf(i), i.access, i.name(),
                                   (i.access & ParamInfo::Read) ? read(path) : std::function<QVariantList(Layer &)>(),
                                   edit([path, number](Layer &l, const QVariantList &a) {
                                       Parameter *p = l.parameter(path);
                                       if (!p) return false;
                                       if (number) return !a.isEmpty() && p->setNumber(num(a[0]));
                                       if (p->info().type == ParamInfo::Type::Bool) return p->setValue(truth(a.value(0)));
                                       return p->setValue(a.value(0));
                                   }));
            if (i.type != ParamInfo::Type::Bool && i.type != ParamInfo::Type::Text && i.type != ParamInfo::Type::Trigger)
                node.range = {rangeOf(i)};
            if (number && i.min == i.lo && i.max == i.hi) node.clip = "both";
            const int cut = path.lastIndexOf('/');
            if (number && cut > 0) parts[path.left(cut)] << path.mid(cut + 1);
        }
        for (auto it = parts.cbegin(); it != parts.cend(); ++it) {
            const QStringList &p = it.value();
            if (p != QStringList{"x", "y"} && p != QStringList{"r", "g", "b"} && p != QStringList{"r", "g", "b", "a"}) continue;
            QStringList paths;
            QJsonArray range;
            for (const QString &x : p) {
                paths << it.key() + '/' + x;
                range.append(rangeOf(byPath[paths.back()]));
            }
            // Its name: what the names of its numbers share ("Position X", "Position Y": "Position")
            QString name = byPath[paths.front()].name();
            for (const QString &path : paths) {
                const QString other = byPath[path].name();
                int k = 0;
                while (k < name.size() && k < other.size() && name[k] == other[k]) ++k;
                name.truncate(k);
            }
            OscNode &node = method(P + '/' + it.key(), QString(p.size(), QLatin1Char('f')), 3, name.trimmed(),
                                   [paths](Layer &l) {
                                       QVariantList out;
                                       for (const QString &path : paths) {
                                           const Parameter *x = l.parameter(path);
                                           if (!x) return QVariantList();
                                           out << x->number();
                                       }
                                       return out;
                                   },
                                   edit([paths](Layer &l, const QVariantList &a) {
                                       if (a.size() < paths.size() - (paths.size() == 4 ? 1 : 0)) return false; // alpha may be left out
                                       for (int k = 0; k < paths.size() && k < a.size(); ++k) {
                                           Parameter *x = l.parameter(paths[k]);
                                           if (!x || !x->setNumber(num(a[k]))) return false;
                                       }
                                       return true;
                                   }));
            node.range = range;
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
    bool isGroup, isViewport, topLevel, transport;
    std::vector<std::pair<quint64, QString>> viewports; // routing of a top-level item
    std::vector<std::pair<int, QString>> effects;
    QStringList animated; // the parameters with an animation
    {
        Engine::Lock lk(&e->mutex());
        Layer *l = find(e, id);
        if (!l) return;
        isViewport = l->isViewport;
        isGroup = l->isGroup || isViewport; // no source of its own
        topLevel = !l->parent && !isViewport;
        if (topLevel) {
            QHash<QString, int> seen;
            for (int i = 0; i < e->layerCount(); ++i) {
                const Layer *v = e->layer(i);
                if (!v->isViewport) continue;
                QString seg = osc::safeName(v->name);
                if (int n = seen.value(seg)) seg += QStringLiteral("_%1").arg(n + 1);
                seen[osc::safeName(v->name)] += 1;
                viewports.push_back({v->id, seg});
            }
        }
        transport = l->hasTransport();
        L.parameters(P, *l); // its parameters, declared by it
        QStringList fxNames;
        for (const auto &x : l->effects) fxNames << x->name();
        const QStringList segs = osc::uniqueSegments(fxNames);
        for (int k = 0; k < int(l->effects.size()); ++k) effects.emplace_back(k, segs[k]);
        for (const Animation &a : l->anims)
            if (!a.tracks.empty()) animated << a.tracks.front().param;
    }

    // What is not a value of the layer but an action of the engine on it: its name, its kind, its visibility and lock
    // (allowed on a locked layer), its source, its transport, the layers chosen as masks, its viewports
    if (isViewport) {
        // Its picture's size in pixels and where it is shown (0 hidden, 1 windowed, 2 fullscreen)
        for (int axis = 0; axis < 2; ++axis) {
            OscNode &n = L.method(P + (axis ? "/output/height" : "/output/width"), "i", 3, axis ? "Height" : "Width",
                                  [axis](Layer &l) { return QVariantList{axis ? l.vpHeight : l.vpWidth}; },
                                  L.edit([axis](Layer &l, const QVariantList &a) {
                                      if (a.isEmpty()) return false;
                                      (axis ? l.vpHeight : l.vpWidth) = std::clamp(int(std::lround(num(a[0]))), 1, 16384);
                                      return true;
                                  }));
            n.range = {minMax(1, 16384)};
        }
        L.method(P + "/output/mode", "i", 3, "Output Mode", [](Layer &l) { return QVariantList{l.vpMode}; },
                 L.edit([](Layer &l, const QVariantList &a) {
                     if (a.isEmpty()) return false;
                     l.vpMode = std::clamp(int(std::lround(num(a[0]))), 0, 2);
                     return true;
                 }))
            .range = {minMax(0, 2)};
    }
    // How much of a top-level item each viewport shows (0 hides it)
    for (const auto &[vid, seg] : viewports) {
        const quint64 v = vid;
        OscNode &vo = L.method(P + "/viewport/" + seg + "/opacity", "f", 3, "Opacity in this viewport",
                               [v](Layer &l) { return QVariantList{double(l.opacityIn(v))}; },
                               L.edit([v](Layer &l, const QVariantList &a) {
                                   if (a.isEmpty()) return false;
                                   const float o = float(std::clamp(num(a[0]), 0.0, 1.0));
                                   if (o >= 1.0f) l.viewportOpacity.erase(v);
                                   else l.viewportOpacity[v] = o;
                                   return true;
                               }));
        vo.range = {minMax(0, 1)};
        vo.clip = "both";
    }
    L.method(P + "/name", "s", 3, "Name", [](Layer &l) { return QVariantList{l.name}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 const QString n = a.value(0).toString().trimmed();
                 if (n.isEmpty()) return false;
                 l.name = n;
                 return true;
             }));
    L.method(P + "/type", "s", 1, "Type", [](Layer &l) { return QVariantList{sourceTypeKey(l)}; }, nullptr).range =
        {vals({"none", "video", "image", "isf", "audio", "layer", "group", "viewport"})};
    L.method(P + "/enable", "T", 3, "Enable", [](Layer &l) { return QVariantList{l.enabled}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 l.enabled = truth(a.value(0));
                 return true;
             }),
             true);
    L.method(P + "/locked", "T", 3, "Locked", [](Layer &l) { return QVariantList{l.locked}; },
             L.edit([](Layer &l, const QVariantList &a) {
                 l.locked = truth(a.value(0));
                 return true;
             }),
             true);
    // A mask: a layer by its name ("" for none)
    auto byName = [e](const QString &name, quint64 *id) {
        Engine::Lock lk(&e->mutex());
        for (int k = 0; k < e->layerCount() && !name.isEmpty(); ++k)
            if (const Layer *o = e->layer(k); o && o->name == name && !o->isViewport) {
                *id = o->id;
                return true;
            }
        return name.isEmpty();
    };
    if (!isViewport) {
        L.method(P + "/mask", "s", 3, "Color Mask",
                 [e](Layer &l) {
                     const Layer *m = l.color.maskLayer ? e->layer(e->indexOfId(l.color.maskLayer)) : nullptr;
                     return QVariantList{m ? m->name : QString()};
                 },
                 [e, byName](int idx, const QVariantList &a) {
                     quint64 id = 0;
                     if (!byName(a.value(0).toString().trimmed(), &id)) return false;
                     bool invert = false;
                     {
                         Engine::Lock lk(&e->mutex());
                         if (const Layer *l = e->layer(idx)) invert = l->color.maskInvert;
                     }
                     return e->setColorMask(idx, id, invert);
                 });
    }
    for (const auto &[k, seg] : effects) {
        const int slot = k;
        L.method(P + "/fx/" + seg + "/mask", "s", 3, "Mask",
                 [e, slot](Layer &l) {
                     const Layer *m = slot < int(l.effects.size()) && l.effects[size_t(slot)]->maskLayer
                                          ? e->layer(e->indexOfId(l.effects[size_t(slot)]->maskLayer))
                                          : nullptr;
                     return QVariantList{m ? m->name : QString()};
                 },
                 [e, slot, byName](int idx, const QVariantList &a) {
                     quint64 id = 0;
                     if (!byName(a.value(0).toString().trimmed(), &id)) return false;
                     bool invert = false;
                     {
                         Engine::Lock lk(&e->mutex());
                         const Layer *l = e->layer(idx);
                         if (!l || slot >= int(l->effects.size())) return false;
                         invert = l->effects[size_t(slot)]->maskInvert;
                     }
                     return e->setEffectMask(idx, slot, id, invert);
                 });
        L.method(P + "/fx/" + seg + "/mask/tap", "s", 3, "Mask Tap",
                 [slot](Layer &l) {
                     return slot < int(l.effects.size()) ? QVariantList{l.effects[size_t(slot)]->maskPreFx ? "prefx" : "postfx"}
                                                         : QVariantList();
                 },
                 [e, slot](int idx, const QVariantList &a) {
                     const QString k = a.value(0).toString().toLower();
                     if (k != "prefx" && k != "postfx") return false;
                     return e->setEffectMaskTap(idx, slot, k == "prefx");
                 })
            .range = {vals({"prefx", "postfx"})};
    }

    if (!isGroup) {
        L.method(P + "/file", "s", 3, "File",
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
        // Another layer as the source, by name (empty: no source), and where its picture is taken
        L.method(P + "/source_layer", "s", 3, "Source Layer",
                 [e](Layer &l) {
                     const Layer *s = l.type == SourceType::Layer ? e->layer(e->indexOfId(l.sourceLayer)) : nullptr;
                     return QVariantList{s ? s->name : QString()};
                 },
                 [e](int idx, const QVariantList &a) {
                     const QString name = a.value(0).toString().trimmed();
                     if (name.isEmpty()) {
                         e->clearLayerSource(idx);
                         return true;
                     }
                     quint64 id = 0;
                     LayerTap tap = LayerTap::PostFx;
                     {
                         Engine::Lock lk(&e->mutex());
                         if (Layer *l = e->layer(idx)) tap = l->sourceTap;
                         for (int k = 0; k < e->layerCount(); ++k)
                             if (const Layer *o = e->layer(k); o && o->name == name) {
                                 id = o->id;
                                 break;
                             }
                     }
                     return id && e->setLayerSourceLayer(idx, id, tap);
                 });
        L.method(P + "/source_tap", "s", 3, "Source Tap",
                 [](Layer &l) { return QVariantList{layerTapKey(l.sourceTap)}; },
                 [e](int idx, const QVariantList &a) {
                     const QString k = a.value(0).toString().toLower();
                     if (k != "prefx" && k != "postfx") return false;
                     return e->setLayerTap(idx, layerTapFromKey(k));
                 })
            .range = {vals({"prefx", "postfx"})};
    }
    if (transport) {
        L.method(P + "/play", "T", 3, "Play", [](Layer &l) { return QVariantList{l.playing}; },
                 [e](int idx, const QVariantList &a) {
                     e->setLayerPlaying(idx, truth(a.value(0)));
                     return true;
                 },
                 true);
        L.method(P + "/restart", "N", 2, "Restart", nullptr,
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
        L.method(P + "/time", "f", 3, "Time (s)", [](Layer &l) { return QVariantList{l.position()}; },
                 [e](int idx, const QVariantList &a) {
                     if (a.isEmpty()) return false;
                     e->seekLayer(idx, num(a[0]));
                     return true;
                 },
                 true);
        L.method(P + "/duration", "f", 1, "Duration",
                 [](Layer &l) { return QVariantList{l.duration()}; }, nullptr);
    }

    // The animation of a parameter, under that parameter: <param>/anim/play, …/duration, …/loop_mode…
    for (const QString &param : animated) {
        const QString A = P + '/' + param + "/anim";
        auto anim = [param](Layer &l) -> const Animation * {
            for (const Animation &a : l.anims)
                if (!a.tracks.empty() && a.tracks.front().param == param) return &a;
            return nullptr;
        };
        // Its transport: no value, an action
        struct Cmd {
            const char *key, *label;
            AnimAction act;
        };
        for (const Cmd &c : {Cmd{"play", "Play", AnimAction::Play}, Cmd{"pause", "Pause", AnimAction::Pause},
                             Cmd{"stop", "Stop", AnimAction::Stop}, Cmd{"rewind", "Rewind", AnimAction::Rewind}})
            L.method(A + '/' + c.key, "N", 2, c.label, nullptr, [e, id, param, act = c.act](int, const QVariantList &) {
                if (!e->layerAnim(id, param, nullptr)) return false;
                e->controlLayerAnim(id, param, act);
                return true;
            });
        // Its settings: read, and written as an edit of the animation (it goes on from where it is)
        auto setting = [&](const char *key, const QString &type, const QString &label, std::function<QVariant(const Animation &)> get,
                           std::function<bool(Animation &, const QVariant &)> set) -> OscNode & {
            return L.method(A + '/' + key, type, 3, label,
                            [anim, get](Layer &l) {
                                const Animation *a = anim(l);
                                return a ? QVariantList{get(*a)} : QVariantList();
                            },
                            [e, id, param, set](int, const QVariantList &v) {
                                Animation a;
                                if (v.isEmpty() || !e->layerAnim(id, param, &a) || !set(a, v[0])) return false;
                                e->setLayerAnim(id, param, &a);
                                return true;
                            });
        };
        setting("enable", "T", "On", [](const Animation &a) { return QVariant(a.tracks.front().enabled); },
                [](Animation &a, const QVariant &v) {
                    a.tracks.front().enabled = truth(v);
                    return true;
                });
        setting("duration", "f", "Duration (s)", [](const Animation &a) { return QVariant(a.duration); },
                [](Animation &a, const QVariant &v) {
                    a.duration = std::clamp(num(v), 0.05, 36000.0);
                    for (AnimKey &k : a.tracks.front().keys) k.t = std::min(k.t, a.duration);
                    return true;
                })
            .range = {minMax(0.05, 600)};
        setting("speed", "f", "Speed (1 = normal)", [](const Animation &a) { return QVariant(a.speed); },
                [](Animation &a, const QVariant &v) {
                    a.speed = std::clamp(num(v), 0.0, 10.0);
                    return true;
                })
            .range = {minMax(0, 10)};
        setting("loop_mode", "s", "Loop Mode", [](const Animation &a) { return QVariant(animLoopKey(a.loop)); },
                [](Animation &a, const QVariant &v) {
                    const QString k = v.toString().toLower().remove('-');
                    if (!QStringList{"once", "loop", "pingpong"}.contains(k)) return false;
                    a.loop = animLoopFromKey(k);
                    return true;
                })
            .range = {vals({"once", "loop", "pingpong"})};
        setting("repeat", "i", "Passes (0: endless)", [](const Animation &a) { return QVariant(a.repeat); },
                [](Animation &a, const QVariant &v) {
                    a.repeat = std::clamp(int(std::lround(num(v))), 0, 100000);
                    return true;
                })
            .range = {minMax(0, 1000)};
        // Its wave (a wave keeps these while it draws keys instead)
        setting("wave", "s", "Wave", [](const Animation &a) { return QVariant(a.tracks.front().oscillator ? animWaveKey(a.tracks.front().wave) : QStringLiteral("keys")); },
                [](Animation &a, const QVariant &v) {
                    const QString k = v.toString().toLower();
                    if (k == "keys") {
                        a.tracks.front().oscillator = false;
                        return true;
                    }
                    const int w = animWaveKeys().indexOf(k);
                    if (w < 0) return false;
                    a.tracks.front().oscillator = true;
                    a.tracks.front().wave = AnimWave(w);
                    return true;
                })
            .range = {vals(animWaveKeys() << QStringLiteral("keys"))};
        struct Num {
            const char *key, *label;
            double AnimTrack::*field;
            double lo, hi;
        };
        for (const Num &n : {Num{"period", "Period (s)", &AnimTrack::period, 0.01, 36000},
                             Num{"center", "Center", &AnimTrack::center, -1e9, 1e9},
                             Num{"amplitude", "Amplitude", &AnimTrack::amplitude, -1e9, 1e9},
                             Num{"phase", "Phase (0..1)", &AnimTrack::phase, 0, 1}}) {
            double AnimTrack::*f = n.field;
            const double lo = n.lo, hi = n.hi;
            setting(n.key, "f", n.label, [f](const Animation &a) { return QVariant(a.tracks.front().*f); },
                    [f, lo, hi](Animation &a, const QVariant &v) {
                        a.tracks.front().*f = std::clamp(num(v), lo, hi);
                        return true;
                    });
        }
        m_nodes[A].description = QStringLiteral("Animation");
    }
    if (isGroup) add(P + "/layer", QString(), 0, "Layers");
    // Readable names of the containers
    static const std::pair<const char *, const char *> kNames[] = {
        {"/roi", "ROI"},        {"/param", "Parameters"}, {"/corner", "Corners"}, {"/soft_edge", "Soft Edge"},
        {"/text", "Text"},      {"/fx", "FX"},            {"/output", "Output"},  {"/position", "Position"},
        {"/pivot", "Pivot"},    {"/scale", "Scale"},      {"/add", "Add"},        {"/remove", "Remove"}};
    for (const auto &[suffix, name] : kNames) {
        auto it = m_nodes.find(P + suffix);
        if (it != m_nodes.end() && it->second.description.isEmpty()) it->second.description = name;
    }
    for (const auto &[k, seg] : effects) {
        auto it = m_nodes.find(P + "/fx/" + seg);
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
