// The numbers of a layer, declared once (see Params.h)
#include "Params.h"
#include "EngineInternal.h"
#include "Osc.h"

#include <QHash>
#include <cmath>
#include <functional>

namespace {

// A range in composition widths or heights (positions, pivots): its bounds are multiples of that size
enum class Unit { Plain, Width, Height };

struct Def {
    QString path, label;
    double min, max, lo, hi;
    Unit unit;
    bool animatable;
    bool (*applies)(const Layer &);
    std::function<double(const Layer &, QSize)> get;
    std::function<void(Layer &, double, QSize)> set; // the value is within lo..hi
};

bool picture(const Layer &l) { return l.hasPicture(); }
bool layerPicture(const Layer &l) { return l.hasPicture() && !l.isViewport; } // not a viewport (a rectangle in pixels)
bool viewport(const Layer &l) { return l.isViewport; }
bool media(const Layer &l) { return l.hasTransport(); }
bool sound(const Layer &l) { return bool(l.audio); }
bool shaderTime(const Layer &l) { return !l.hasTransport() && l.generator && l.generator->isValid(); }
bool text(const Layer &l) { return l.isText(); }

double wrapDegrees(double d)
{
    d = std::fmod(d, 360.0);
    if (d <= -180) d += 360;
    if (d > 180) d -= 360;
    return d;
}

double unitSize(Unit u, QSize comp) { return u == Unit::Width ? comp.width() : u == Unit::Height ? comp.height() : 1.0; }

// Where a layer is, in composition pixels: a viewport is a rectangle (turned or not), a layer its bounds
QPointF centerPx(const Layer &l, QSize c)
{
    if (l.isViewport) return l.mapping.rect(c).center;
    const QPointF p = l.mapping.bounds().center();
    return QPointF(p.x() * c.width(), p.y() * c.height());
}

void setCenterPx(Layer &l, QPointF p, QSize c)
{
    if (l.isViewport) {
        Mapping::Rect r = l.mapping.rect(c);
        r.center = p;
        l.mapping.setRect(r, c);
        return;
    }
    QRectF b = l.mapping.bounds();
    b.moveCenter(QPointF(p.x() / c.width(), p.y() / c.height()));
    l.mapping.setBounds(b);
}

const std::vector<Def> &table()
{
    static const std::vector<Def> defs = [] {
        std::vector<Def> d;
        auto add = [&d](const QString &path, const QString &label, double min, double max, double lo, double hi, Unit unit,
                        bool (*applies)(const Layer &), std::function<double(const Layer &, QSize)> get,
                        std::function<void(Layer &, double, QSize)> set) {
            d.push_back({path, label, min, max, lo, hi, unit, true, applies, std::move(get), std::move(set)});
        };
        add("opacity", "Opacity", 0, 1, 0, 1, Unit::Plain, picture, [](const Layer &l, QSize) { return double(l.opacity); },
            [](Layer &l, double v, QSize) { l.opacity = float(v); });

        // Spatial
        add("spatial/rotation", "Spatial › Rotation (°)", -180, 180, -1e9, 1e9, Unit::Plain, picture,
            [](const Layer &l, QSize c) { return l.mapping.angle(c); },
            [](Layer &l, double v, QSize c) { l.mapping.rotate(wrapDegrees(v - l.mapping.angle(c)), c); }); // the shortest way round
        for (int axis = 0; axis < 2; ++axis) {
            const bool x = axis == 0;
            const Unit u = x ? Unit::Width : Unit::Height;
            add(x ? "spatial/position/x" : "spatial/position/y", x ? "Spatial › Position X" : "Spatial › Position Y", -4, 5, -4, 5,
                u, picture, [x](const Layer &l, QSize c) { return x ? centerPx(l, c).x() : centerPx(l, c).y(); },
                [x](Layer &l, double v, QSize c) {
                    QPointF p = centerPx(l, c);
                    (x ? p.rx() : p.ry()) = v;
                    setCenterPx(l, p, c);
                });
        }
        for (int axis = 0; axis < 2; ++axis) { // the center of the rotation, fixed to the picture, in composition pixels
            const bool x = axis == 0;
            add(x ? "spatial/pivot/x" : "spatial/pivot/y", x ? "Spatial › Pivot X" : "Spatial › Pivot Y", -4, 5, -4, 5,
                x ? Unit::Width : Unit::Height, picture,
                [x](const Layer &l, QSize c) {
                    const QPointF p = l.mapping.pivotPoint();
                    return x ? p.x() * c.width() : p.y() * c.height();
                },
                [x](Layer &l, double v, QSize c) {
                    QPointF p = l.mapping.pivotPoint();
                    (x ? p.rx() : p.ry()) = v / (x ? c.width() : c.height());
                    l.mapping.setPivotPoint(p);
                });
        }
        for (int axis = 0; axis < 2; ++axis) { // a viewport: its rectangle's size, in pixels
            const bool x = axis == 0;
            add(x ? "spatial/width" : "spatial/height", x ? "Spatial › Width" : "Spatial › Height", 1, 10000, 1, 100000,
                Unit::Plain, viewport, [x](const Layer &l, QSize c) { return x ? l.mapping.rect(c).w : l.mapping.rect(c).h; },
                [x](Layer &l, double v, QSize c) {
                    Mapping::Rect r = l.mapping.rect(c);
                    (x ? r.w : r.h) = v;
                    l.mapping.setRect(r, c);
                });
        }
        for (int axis = 0; axis < 2; ++axis) { // a layer: its size, in % of the composition
            const bool x = axis == 0;
            add(x ? "spatial/scale/x" : "spatial/scale/y", x ? "Spatial › Scale X (%)" : "Spatial › Scale Y (%)", 0.1, 400, 0.1,
                2000, Unit::Plain, layerPicture,
                [x](const Layer &l, QSize) { return (x ? l.mapping.bounds().width() : l.mapping.bounds().height()) * 100.0; },
                [x](Layer &l, double v, QSize) {
                    QRectF b = l.mapping.bounds();
                    const QPointF center = b.center();
                    b.setSize(QSizeF(x ? v / 100.0 : b.width(), x ? b.height() : v / 100.0));
                    b.moveCenter(center);
                    l.mapping.setBounds(b);
                });
        }
        static const char *const kSides[] = {"left", "right", "top", "bottom"}; // the order of SoftEdge's arrays
        static const char *const kSideNames[] = {"Left", "Right", "Top", "Bottom"};
        for (int k = 0; k < 4; ++k) {
            const QString base = QStringLiteral("spatial/soft_edge/%1/").arg(QLatin1String(kSides[k]));
            const QString name = QStringLiteral("Spatial › Soft Edge %1 ").arg(QLatin1String(kSideNames[k]));
            add(base + "width", name + "Width", 0, 0.5, 0, 0.5, Unit::Plain, picture,
                [k](const Layer &l, QSize) { return l.mapping.soft.width[k]; }, [k](Layer &l, double v, QSize) { l.mapping.soft.width[k] = v; });
            add(base + "power", name + "Power", 0.1, 8, 0.1, 8, Unit::Plain, picture,
                [k](const Layer &l, QSize) { return l.mapping.soft.power[k]; }, [k](Layer &l, double v, QSize) { l.mapping.soft.power[k] = v; });
        }

        // Playback: the media's (backwards below 0: it turns round where it is), or the pace of the shader's TIME
        add("source/volume", "Source › Volume", 0, 2, 0, 2, Unit::Plain, sound, [](const Layer &l, QSize) { return double(l.volume); },
            [](Layer &l, double v, QSize) { l.volume = float(v); });
        add("source/speed", "Source › Speed", 0, 4, -8, 8, Unit::Plain, media, [](const Layer &l, QSize) { return l.speed; },
            [](Layer &l, double v, QSize) {
                const int dir = v < 0 ? -1 : v > 0 ? 1 : l.dir;
                const double pos = l.position();
                l.speed = v;
                if (dir != l.dir) reposition(l, pos, dir); // the other way from the same place
            });
        add("source/speed", "Source › Speed", 0, 10, 0, 10, Unit::Plain, shaderTime,
            [](const Layer &l, QSize) { return l.generator->speed; }, [](Layer &l, double v, QSize) { l.generator->speed = v; });

        // ROI: the part of the source used (normalized, origin top left); its sides never cross
        static const char *const kRoi[] = {"left", "top", "right", "bottom"};
        static const char *const kRoiNames[] = {"Left", "Top", "Right", "Bottom"};
        for (int side = 0; side < 4; ++side)
            add(QStringLiteral("roi/") + kRoi[side], QStringLiteral("ROI › ") + kRoiNames[side], 0, 1, 0, 1, Unit::Plain, picture,
                [side](const Layer &l, QSize) {
                    const double v[4] = {l.roi.left(), l.roi.top(), l.roi.right(), l.roi.bottom()};
                    return v[side];
                },
                [side](Layer &l, double x, QSize) {
                    double v[4] = {l.roi.left(), l.roi.top(), l.roi.right(), l.roi.bottom()};
                    v[side] = x;
                    const double minSize = 0.002;
                    if (side == 0) v[0] = std::min(v[0], v[2] - minSize);
                    if (side == 2) v[2] = std::max(v[2], v[0] + minSize);
                    if (side == 1) v[1] = std::min(v[1], v[3] - minSize);
                    if (side == 3) v[3] = std::max(v[3], v[1] + minSize);
                    l.roi = QRectF(QPointF(v[0], v[1]), QPointF(v[2], v[3])) & Layer::fullRoi();
                });

        // Color
        const double T = ColorAdjust::kTempRange, N = ColorAdjust::kTintRange;
        add("color/temp", "Color › Temperature", -T, T, -T, T, Unit::Plain, picture, [](const Layer &l, QSize) { return double(l.color.temp); },
            [](Layer &l, double v, QSize) { l.color.temp = float(v); });
        add("color/tint", "Color › Tint", -N, N, -N, N, Unit::Plain, picture, [](const Layer &l, QSize) { return double(l.color.tint); },
            [](Layer &l, double v, QSize) { l.color.tint = float(v); });
        for (int which = 0; which < 2; ++which)
            for (int c = 0; c < 3; ++c)
                add(QStringLiteral("color/%1/%2").arg(which ? "remove" : "add").arg(QChar("rgb"[c])),
                    QStringLiteral("Color › %1 %2").arg(which ? "Remove" : "Add").arg(QChar("RGB"[c])), 0, 1, 0, 1, Unit::Plain, picture,
                    [which, c](const Layer &l, QSize) { return double((which ? l.color.remove : l.color.add)[c]); },
                    [which, c](Layer &l, double v, QSize) { (which ? l.color.remove : l.color.add)[c] = float(v); });

        // Text generator
        struct TextNum {
            const char *path, *label;
            double min, max, lo, hi;
            float TextSource::*member; // or the size (an int)
        };
        const TextNum texts[] = {
            {"source/text/size", "Source › Text Size (px)", 1, 400, 1, 1000, nullptr},
            {"source/text/line_height", "Source › Text Line Height", 0.5, 3, 0.1, 10, &TextSource::lineHeight},
            {"source/text/letter_spacing", "Source › Text Letter Spacing (px)", -20, 100, -200, 500, &TextSource::letterSpacing},
            {"source/text/outline", "Source › Text Outline (px)", 0, 40, 0, 200, &TextSource::outline},
            {"source/text/shadow/x", "Source › Text Shadow X (px)", -100, 100, -2000, 2000, &TextSource::shadowX},
            {"source/text/shadow/y", "Source › Text Shadow Y (px)", -100, 100, -2000, 2000, &TextSource::shadowY}};
        for (const TextNum &x : texts) {
            float TextSource::*m = x.member;
            add(x.path, x.label, x.min, x.max, x.lo, x.hi, Unit::Plain, text,
                [m](const Layer &l, QSize) { return m ? double(l.text.*m) : double(l.text.size); },
                [m](Layer &l, double v, QSize) {
                    if (m) l.text.*m = float(v);
                    else l.text.size = int(std::lround(v));
                });
        }
        return d;
    }();
    return defs;
}

const QHash<QString, QList<int>> &tableIndex()
{
    static const QHash<QString, QList<int>> index = [] {
        QHash<QString, QList<int>> h;
        for (int i = 0; i < int(table().size()); ++i) h[table()[size_t(i)].path].append(i);
        return h;
    }();
    return index;
}

NumberParam paramOf(const Def &d, QSize comp)
{
    const double k = unitSize(d.unit, comp);
    return {d.path, d.label, d.min * k, d.max * k, d.lo * k, d.hi * k, d.animatable};
}

// The effects' segments in the addresses (their names, made unique)
QStringList fxSegments(const Layer &l)
{
    QStringList names;
    for (const auto &x : l.effects) names << x->name();
    return osc::uniqueSegments(names);
}

// A shader's numbers, from its inputs
void isfNumbers(const IsfInstance *inst, const QString &base, const QString &title, std::vector<NumberParam> &out)
{
    if (!inst) return;
    for (const IsfInput &in : inst->inputs()) {
        if (!in.isNumber()) continue;
        const QString label = QStringLiteral("%1 › %2").arg(title, in.label.isEmpty() ? in.name : in.label);
        const QString path = base + osc::safeName(in.name); // its address, as in OSC
        switch (in.type) {
        case IsfInput::Float: {
            const double lo = std::min(in.fMin, in.fMax), hi = std::max(in.fMin, in.fMax);
            out.push_back({path, label, lo, hi, lo, hi, true});
            break;
        }
        case IsfInput::Long: {
            const double lo = *std::min_element(in.lValues.begin(), in.lValues.end());
            const double hi = *std::max_element(in.lValues.begin(), in.lValues.end());
            out.push_back({path, label, lo, hi, lo, hi, true, QList<int>(in.lValues.begin(), in.lValues.end())});
            break;
        }
        case IsfInput::Point2D: {
            const QPointF lo = in.hasPointRange ? in.pMin : QPointF(0, 0);
            const QPointF hi = in.hasPointRange ? in.pMax : QPointF(1, 1);
            out.push_back({path + "/x", label + " X", lo.x(), hi.x(), -1e6, 1e6, true});
            out.push_back({path + "/y", label + " Y", lo.y(), hi.y(), -1e6, 1e6, true});
            break;
        }
        case IsfInput::Color:
            for (int c = 0; c < 4; ++c) out.push_back({path + "/" + QChar("rgba"[c]), label + " " + QChar("RGBA"[c]), 0, 1, 0, 1, true});
            break;
        default: break;
        }
    }
}

// A shader's number at path p (from p[at]: the input's name, then x, y, r, g, b or a)
bool isfNumber(IsfInstance *inst, const QStringList &p, int at, double *get, const double *set)
{
    if (!inst) return false;
    IsfInput *in = nullptr; // by its name in the address
    for (IsfInput &x : inst->inputs())
        if (osc::safeName(x.name) == p.value(at)) in = &x;
    if (!in || !in->isNumber()) return false;
    const QString part = p.value(at + 1);
    if (p.size() > at + 2) return false;
    IsfValue v = in->value();
    double value = 0, lo = -1e6, hi = 1e6;
    switch (in->type) {
    case IsfInput::Float:
        if (!part.isEmpty()) return false;
        value = v.f;
        lo = std::min(in->fMin, in->fMax);
        hi = std::max(in->fMin, in->fMax);
        break;
    case IsfInput::Long:
        if (!part.isEmpty()) return false;
        value = v.l;
        lo = *std::min_element(in->lValues.begin(), in->lValues.end());
        hi = *std::max_element(in->lValues.begin(), in->lValues.end());
        break;
    case IsfInput::Point2D:
        if (part != "x" && part != "y") return false;
        value = part == "x" ? v.p.x() : v.p.y();
        break;
    case IsfInput::Color: {
        const int k = QStringLiteral("rgba").indexOf(part);
        if (k < 0 || part.size() != 1) return false;
        value = v.c[k];
        lo = 0;
        hi = 1;
        break;
    }
    default: return false;
    }
    if (get) *get = value;
    if (set) {
        const double s = std::clamp(*set, lo, hi);
        switch (in->type) {
        case IsfInput::Float: v.f = s; break;
        case IsfInput::Long: { // the nearest of its values
            int best = in->lValues.isEmpty() ? int(std::lround(s)) : in->lValues.front();
            for (int x : in->lValues)
                if (std::abs(x - s) < std::abs(best - s)) best = x;
            v.l = best;
            break;
        }
        case IsfInput::Point2D: (part == "x" ? v.p.rx() : v.p.ry()) = s; break;
        case IsfInput::Color: v.c[QStringLiteral("rgba").indexOf(part)] = float(s); break;
        default: break;
        }
        in->setValue(v);
    }
    return true;
}

} // namespace

std::vector<NumberParam> layerNumbers(const Layer &l, QSize comp)
{
    std::vector<NumberParam> out;
    for (const Def &d : table())
        if (d.applies(l)) out.push_back(paramOf(d, comp));
    isfNumbers(l.generator.get(), QStringLiteral("source/param/"), QStringLiteral("Source"), out);
    const QStringList segs = fxSegments(l);
    for (size_t k = 0; k < l.effects.size(); ++k) {
        const QString name = l.effects[k]->name();
        out.push_back({QStringLiteral("fx/%1/speed").arg(segs[int(k)]), QStringLiteral("FX › %1 › Speed").arg(name), 0, 10, 0, 10, true});
        isfNumbers(l.effects[k].get(), QStringLiteral("fx/%1/param/").arg(segs[int(k)]), QStringLiteral("FX › %1").arg(name), out);
    }
    return out;
}

bool layerNumber(Layer &l, const QString &path, double *get, const double *set, QSize comp)
{
    const auto it = tableIndex().constFind(path);
    if (it != tableIndex().constEnd()) {
        for (int i : *it) {
            const Def &d = table()[size_t(i)];
            if (!d.applies(l)) continue;
            if (get) *get = d.get(l, comp);
            if (set) {
                const double k = unitSize(d.unit, comp);
                d.set(l, std::clamp(*set, d.lo * k, d.hi * k), comp);
            }
            return true;
        }
        return false;
    }
    // A shader's: source/param/<name>[/…], fx/<fx>/speed, fx/<fx>/param/<name>[/…] (fx: its segment in the address)
    const QStringList p = path.split(QLatin1Char('/'));
    if (p.value(0) == "source" && p.value(1) == "param") return isfNumber(l.generator.get(), p, 2, get, set);
    if (p.value(0) != "fx" || p.size() < 3) return false;
    const int k = fxSegments(l).indexOf(p[1]);
    if (k < 0) return false;
    IsfInstance *fx = l.effects[size_t(k)].get();
    if (p.size() == 3 && p[2] == "speed") {
        if (get) *get = fx->speed;
        if (set) fx->speed = std::clamp(*set, 0.0, 10.0);
        return true;
    }
    return p[2] == "param" && isfNumber(fx, p, 3, get, set);
}

bool layerNumberParam(const Layer &l, const QString &path, QSize comp, NumberParam *p)
{
    const auto it = tableIndex().constFind(path);
    if (it != tableIndex().constEnd()) {
        for (int i : *it)
            if (table()[size_t(i)].applies(l)) {
                if (p) *p = paramOf(table()[size_t(i)], comp);
                return true;
            }
        return false;
    }
    for (const NumberParam &n : layerNumbers(l, comp))
        if (n.path == path) {
            if (p) *p = n;
            return true;
        }
    return false;
}
