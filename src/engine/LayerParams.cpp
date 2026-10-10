// The parameters a layer instantiates (Layer::parameters)
#include "EngineInternal.h"
#include "Osc.h"

#include <QHash>
#include <cmath>

namespace {

QStringList blendKeys()
{
    QStringList k;
    for (BlendMode m : kBlendModes) k << blendModeKey(m);
    return k;
}

// What the parameters are made from: when it changes, they are made again
std::vector<quint64> keyOf(const Layer &l)
{
    const QSize c = l.compSize ? *l.compSize : QSize(1920, 1080);
    std::vector<quint64> k{quint64(l.type),       quint64(l.hasTransport()), quint64(bool(l.audio)), quint64(l.isViewport),
                           quint64(l.isGroup),    quint64(c.width()),        quint64(c.height()),
                           quint64(quintptr(l.generator.get())), quint64(l.effects.size())};
    auto inputs = [&k](const IsfInstance *i) {
        if (!i) return;
        k.push_back(quint64(i->isValid()));
        for (const IsfInput &in : i->inputs()) k.push_back(qHash(in.name) ^ (quint64(in.type) << 40));
    };
    inputs(l.generator.get());
    for (const auto &fx : l.effects) {
        k.push_back(quint64(quintptr(fx.get())));
        k.push_back(qHash(fx->name()));
        inputs(fx.get());
    }
    return k;
}

} // namespace

const std::vector<Parameter *> &Layer::parameters()
{
    std::vector<quint64> key = keyOf(*this);
    if (key == paramKey) return paramList;
    paramKey = std::move(key);
    paramStore.clear();
    paramList.clear();
    using T = Parameter::Type;
    Layer *self = this;
    const QSize *comp = compSize;
    auto C = [comp] { return comp ? *comp : QSize(1920, 1080); };
    auto add = [this](const QString &path, const QString &label, T type) -> Parameter & {
        paramStore.push_back(std::make_unique<Parameter>(path, label, type));
        return *paramStore.back();
    };
    auto num = [&](const QString &path, const QString &label, double lo, double hi, double def, std::function<double()> get,
                   std::function<void(double)> set) -> Parameter & {
        return add(path, label, T::Float)
            .range(lo, hi)
            .byDefault(def)
            .bind([get] { return QVariant(get()); }, [set](const QVariant &v) { set(v.toDouble()); });
    };
    auto flag = [&](const QString &path, const QString &label, bool def, bool *field) -> Parameter & {
        return add(path, label, T::Bool).byDefault(def).bind([field] { return QVariant(*field); },
                                                             [field](const QVariant &v) { *field = v.toBool(); });
    };
    const bool picture = hasPicture();

    if (picture) {
        num("opacity", "Opacity", 0, 1, 1.0, [self] { return double(self->opacity); },
            [self](double v) { self->opacity = float(v); });
        if (!isViewport)
            add("blend_mode", "Blend Mode", T::Choice)
                .choices(blendKeys())
                .byDefault(blendModeKey(BlendMode::Normal))
                .bind([self] { return QVariant(blendModeKey(self->blend)); },
                      [self](const QVariant &v) { self->blend = blendModeFromKey(v.toString()); });

        // Where it is (Mapping.h): the center of its rectangle, its size, its rotation about its pivot — each one a
        // value of its own; a viewport's size is in pixels, a layer's in % of the composition
        Mapping *m = &mapping;
        num("rotation", "Spatial › Rotation (°)", -180, 180, 0.0, [m] { return m->rotation; },
            [m](double v) {
                m->rotation = v;
                ++m->revision;
            })
            .limits(-1e9, 1e9)
            .step(0.1)
            .ramp(Parameter::Ramp::Angle);
        for (int axis = 0; axis < 2; ++axis) {
            const bool x = axis == 0;
            const double size = x ? C().width() : C().height();
            num(x ? "position/x" : "position/y", x ? "Spatial › Position X" : "Spatial › Position Y", -4 * size, 5 * size, size / 2,
                [m, C, x] { return x ? m->position.x() * C().width() : m->position.y() * C().height(); },
                [m, C, x](double v) {
                    (x ? m->position.rx() : m->position.ry()) = v / (x ? C().width() : C().height());
                    ++m->revision;
                })
                .step(1);
        }
        for (int axis = 0; axis < 2; ++axis) { // the center of the rotation, fixed to the picture, in composition pixels
            const bool x = axis == 0;
            const double size = x ? C().width() : C().height();
            num(x ? "pivot/x" : "pivot/y", x ? "Spatial › Pivot X" : "Spatial › Pivot Y", -4 * size, 5 * size, size / 2,
                [m, C, x] {
                    const QPointF p = m->pivotPoint();
                    return x ? p.x() * C().width() : p.y() * C().height();
                },
                [m, C, x](double v) { // the picture stays where it is
                    QPointF p = m->pivotPoint();
                    (x ? p.rx() : p.ry()) = v / (x ? C().width() : C().height());
                    m->setPivotPoint(p);
                })
                .step(1)
                .byDefault(std::function<QVariant()>([m, C, x] { // the middle of the picture
                    const QPointF c = m->toComposition(QPointF(0.5, 0.5));
                    return QVariant(x ? c.x() * C().width() : c.y() * C().height());
                }));
        }
        for (int axis = 0; axis < 2; ++axis) {
            const bool x = axis == 0;
            auto get = [m, x] { return x ? m->size.width() : m->size.height(); };
            auto put = [m, x](double v) {
                if (x) m->size.setWidth(v);
                else m->size.setHeight(v);
                ++m->revision;
            };
            if (isViewport) // its rectangle, in pixels
                num(x ? "width" : "height", x ? "Spatial › Width" : "Spatial › Height", 1, 10000, x ? C().width() : C().height(),
                    [get, C, x] { return get() * (x ? C().width() : C().height()); },
                    [put, C, x](double v) { put(v / (x ? C().width() : C().height())); })
                    .limits(1, 100000)
                    .step(1);
            else // its size, in % of the composition
                num(x ? "scale/x" : "scale/y", x ? "Spatial › Scale X (%)" : "Spatial › Scale Y (%)", 0.1, 400, 100.0,
                    [get] { return get() * 100.0; }, [put](double v) { put(v / 100.0); })
                    .limits(0.1, 2000)
                    .step(0.1);
        }
        // Soft edge: the picture fades out towards each side
        flag("soft_edge/enable", "Spatial › Soft Edge", false, &mapping.soft.enabled);
        static const char *const kSides[] = {"left", "right", "top", "bottom"}; // the order of SoftEdge's arrays
        static const char *const kSideNames[] = {"Left", "Right", "Top", "Bottom"};
        for (int k = 0; k < 4; ++k) {
            const QString base = QStringLiteral("soft_edge/%1/").arg(QLatin1String(kSides[k]));
            const QString name = QStringLiteral("Spatial › Soft Edge %1 ").arg(QLatin1String(kSideNames[k]));
            num(base + "width", name + "Width", 0, 0.5, 0.0, [self, k] { return self->mapping.soft.width[k]; },
                [self, k](double v) { self->mapping.soft.width[k] = v; })
                .step(0.001);
            num(base + "power", name + "Power", 0.1, 8, 1.0, [self, k] { return self->mapping.soft.power[k]; },
                [self, k](double v) { self->mapping.soft.power[k] = v; });
        }

        // ROI: the part of the source used (normalized, origin top left); its sides never cross
        static const char *const kRoi[] = {"left", "top", "right", "bottom"};
        static const char *const kRoiNames[] = {"Left", "Top", "Right", "Bottom"};
        for (int side = 0; side < 4; ++side)
            num(QStringLiteral("roi/") + kRoi[side], QStringLiteral("ROI › ") + kRoiNames[side], 0, 1, side < 2 ? 0.0 : 1.0,
                [self, side] {
                    const double v[4] = {self->roi.left(), self->roi.top(), self->roi.right(), self->roi.bottom()};
                    return v[side];
                },
                [self, side](double x) {
                    double v[4] = {self->roi.left(), self->roi.top(), self->roi.right(), self->roi.bottom()};
                    v[side] = x;
                    const double minSize = 0.002;
                    if (side == 0) v[0] = std::min(v[0], v[2] - minSize);
                    if (side == 2) v[2] = std::max(v[2], v[0] + minSize);
                    if (side == 1) v[1] = std::min(v[1], v[3] - minSize);
                    if (side == 3) v[3] = std::max(v[3], v[1] + minSize);
                    self->roi = QRectF(QPointF(v[0], v[1]), QPointF(v[2], v[3])) & Layer::fullRoi();
                })
                .step(0.001);

        // Color: the section's switch, the balance, the colors removed and added (each with its switch)
        flag("color/enable", "Color › Enable", true, &color.enabled);
        const double T_ = ColorAdjust::kTempRange, N = ColorAdjust::kTintRange;
        num("temp", "Color › Temperature", -T_, T_, 0.0, [self] { return double(self->color.temp); },
            [self](double v) { self->color.temp = float(v); })
            .step(1);
        flag("temp/enable", "Color › Temperature Enable", true, &color.tempOn);
        num("tint", "Color › Tint", -N, N, 0.0, [self] { return double(self->color.tint); },
            [self](double v) { self->color.tint = float(v); })
            .step(0.1);
        flag("tint/enable", "Color › Tint Enable", true, &color.tintOn);
        for (int which = 0; which < 2; ++which) {
            const QString key = which ? QStringLiteral("remove") : QStringLiteral("add");
            const QString name = which ? QStringLiteral("Remove") : QStringLiteral("Add");
            for (int c = 0; c < 3; ++c)
                num(key + "/" + QChar("rgb"[c]), QStringLiteral("Color › %1 %2").arg(name).arg(QChar("RGB"[c])), 0, 1, 0.0,
                    [self, which, c] { return double((which ? self->color.remove : self->color.add)[c]); },
                    [self, which, c](double v) { (which ? self->color.remove : self->color.add)[c] = float(v); });
            flag(key + "/enable", QStringLiteral("Color › %1 Enable").arg(name), true, which ? &color.removeOn : &color.addOn);
        }
        flag("mask/invert", "Color › Invert Mask", false, &color.maskInvert);
        flag("fx/enable", "FX › Enable", true, &effectsEnabled);
    }

    // Playback: the media's (backwards below 0: it turns round where it is), or the pace of the shader's TIME
    if (audio) {
        num("volume", "Source › Volume", 0, 2, 1.0, [self] { return double(self->volume); },
            [self](double v) { self->volume = float(v); });
        flag("mute", "Source › Mute", false, &muted);
    }
    if (hasTransport())
        num("speed", "Source › Speed", 0, 4, 1.0, [self] { return self->speed; },
            [self](double v) {
                const int dir = v < 0 ? -1 : v > 0 ? 1 : self->dir;
                const double pos = self->position();
                self->speed = v;
                if (dir != self->dir) reposition(*self, pos, dir); // the other way from the same place
            })
            .limits(-8, 8);

    // Text generator
    if (isText()) {
        TextSource *t = &text;
        auto textNum = [&](const QString &path, const QString &label, double lo, double hi, double llo, double lhi, double def,
                           float TextSource::*m) -> Parameter & {
            return num(path, label, lo, hi, def, [t, m] { return m ? double(t->*m) : double(t->size); },
                       [t, m](double v) {
                           if (m) t->*m = float(v);
                           else t->size = int(std::lround(v));
                       })
                .limits(llo, lhi);
        };
        const TextSource def;
        add("text/content", "Text › Content", T::Text).byDefault(QString()).bind([t] { return QVariant(t->content); },
                                                                                 [t](const QVariant &v) { t->content = v.toString(); });
        add("text/font", "Text › Font", T::Text).byDefault(def.font).bind([t] { return QVariant(t->font); }, [t](const QVariant &v) {
            t->font = v.toString().isEmpty() ? QStringLiteral("Arial") : v.toString();
        });
        textNum("text/size", "Text › Size (px)", 1, 400, 1, 1000, def.size, nullptr).step(1);
        textNum("text/line_height", "Text › Line Height", 0.5, 3, 0.1, 10, def.lineHeight, &TextSource::lineHeight);
        textNum("text/letter_spacing", "Text › Letter Spacing (px)", -20, 100, -200, 500, def.letterSpacing, &TextSource::letterSpacing)
            .step(0.5);
        textNum("text/outline", "Text › Outline (px)", 0, 40, 0, 200, def.outline, &TextSource::outline).step(0.5);
        flag("text/shadow/enable", "Text › Shadow", def.shadow, &text.shadow);
        textNum("text/shadow/x", "Text › Shadow X (px)", -100, 100, -2000, 2000, def.shadowX, &TextSource::shadowX).step(1);
        textNum("text/shadow/y", "Text › Shadow Y (px)", -100, 100, -2000, 2000, def.shadowY, &TextSource::shadowY).step(1);
        struct Col {
            const char *path, *label;
            QColor TextSource::*member;
            QColor def;
        };
        for (const Col &c : {Col{"text/color", "Text › Color", &TextSource::color, def.color},
                             Col{"text/outline/color", "Text › Outline Color", &TextSource::outlineColor, def.outlineColor},
                             Col{"text/shadow/color", "Text › Shadow Color", &TextSource::shadowColor, def.shadowColor}})
            for (int k = 0; k < 4; ++k) {
                QColor TextSource::*m = c.member;
                const double d = k == 0 ? c.def.redF() : k == 1 ? c.def.greenF() : k == 2 ? c.def.blueF() : c.def.alphaF();
                num(QString::fromLatin1(c.path) + "/" + QChar("rgba"[k]), QString::fromLatin1(c.label) + " " + QChar("RGBA"[k]), 0, 1, d,
                    [t, m, k] {
                        const QColor &q = t->*m;
                        return k == 0 ? q.redF() : k == 1 ? q.greenF() : k == 2 ? q.blueF() : q.alphaF();
                    },
                    [t, m, k](double v) {
                        QColor &q = t->*m;
                        if (k == 0) q.setRedF(float(v));
                        else if (k == 1) q.setGreenF(float(v));
                        else if (k == 2) q.setBlueF(float(v));
                        else q.setAlphaF(float(v));
                    });
            }
    }

    // The shaders: the generator's at the layer's top (its speed: the pace of its TIME), each effect under fx/<fx>/
    auto shader = [&](const IsfInstance &inst, const QString &prefix, const QString &title, bool effect) {
        for (Parameter p : inst.parameters(title)) {
            if (!effect && (p.path() == "enable" || p.path() == "mask/invert")) continue; // an effect's
            if (!effect && p.path() == "speed" && hasTransport()) continue;              // the media's speed instead
            paramStore.push_back(std::make_unique<Parameter>(p.prefixed(prefix)));
        }
    };
    if (generator && generator->isValid() && !hasTransport()) shader(*generator, QString(), QStringLiteral("Source"), false);
    QStringList names;
    for (const auto &x : effects) names << x->name();
    const QStringList segs = osc::uniqueSegments(names);
    for (size_t k = 0; k < effects.size(); ++k)
        shader(*effects[k], QStringLiteral("fx/%1/").arg(segs[int(k)]), QStringLiteral("FX › %1").arg(effects[k]->name()), true);

    for (auto &p : paramStore) paramList.push_back(p.get());
    return paramList;
}

Parameter *Layer::parameter(const QString &path)
{
    for (Parameter *p : parameters())
        if (p->path() == path) return p;
    return nullptr;
}
