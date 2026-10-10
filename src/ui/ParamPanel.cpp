#include "ParamPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Osc.h"
#include "ParamAnimPanel.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QAbstractItemView>
#include <QPointer>
#include <QUndoStack>
#include <cmath>

static void setSwatch(QPushButton *b, const float c[4])
{
    const QColor col = QColor::fromRgbF(c[0], c[1], c[2], c[3]);
    b->setText(col.name(QColor::HexArgb).toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid #555; padding:3px 8px; }")
                         .arg(col.name(QColor::HexRgb), col.lightnessF() > 0.55 ? "#000" : "#fff"));
}

void ParamPanel::setValue(int input, const QString &label, const std::function<void(IsfValue &)> &modify)
{
    IsfValue before;
    {
        Engine::Lock lk(&m_engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
        if (!inst || input >= int(inst->inputs().size())) return;
        before = inst->inputs()[size_t(input)].value();
    }
    IsfValue after = before;
    modify(after);
    if (after == before) return;
    m_undo->push(new cmd::SetParam(m_engine, m_layer, m_slot, input, before, after, label));
}

ParamPanel::ParamPanel(Engine *engine, QUndoStack *undo, int layer, int slot, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo), m_layer(layer), m_slot(slot)
{
    // Copy metadata and values under the lock: widgets are then built without blocking rendering.
    std::vector<IsfInput> inputs;
    QString description;
    {
        Engine::Lock lk(&engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(engine, layer, slot);
        if (inst) {
            inputs = inst->inputs();
            description = inst->description();
        }
    }

    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 4, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    if (!description.isEmpty()) {
        auto *d = new QLabel(description);
        d->setWordWrap(true);
        d->setStyleSheet("color:#999; font-size:11px;");
        form->addRow(d);
    }

    // Speed of TIME, for every shader (TIME goes on from where it is: no jump)
    {
        double speed = 1.0;
        {
            Engine::Lock lk(&engine->mutex());
            if (IsfInstance *inst = cmd::resolveIsf(engine, layer, slot)) speed = inst->speed;
        }
        m_speed = new SliderField;
        m_speed->setRange(0, 10);
        m_speed->setDecimals(2);
        m_speed->setSingleStep(0.05);
        m_speed->setTicks(10);
        m_speed->setSnaps({1});
        m_speed->setValue(speed);
        m_speed->setToolTip(QStringLiteral("Speed of the shader's time (1 = normal, 0 = stopped)"));
        connect(m_speed, &SliderField::valueEdited, this, [this](double v) {
            double before = 1.0;
            {
                Engine::Lock lk(&m_engine->mutex());
                IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
                if (!inst) return;
                before = inst->speed;
            }
            if (std::abs(before - v) > 1e-9) m_undo->push(new cmd::SetIsfSpeed(m_engine, m_layer, m_slot, before, v));
        });
        auto *name = new ResetLabel(QStringLiteral("Speed"), [this] {
            m_speed->setValue(1);
            emit m_speed->valueEdited(1);
        });
        form->addRow(name, m_speed);
        m_rows.push_back({QString(), {name, m_speed}});
    }

    for (int idx = 0; idx < int(inputs.size()); ++idx) {
        const IsfInput &in = inputs[size_t(idx)];
        if (in.type == IsfInput::Image && in.isInputImage) continue;
        const QString label = in.label;
        QWidget *field = nullptr;
        QWidget *pointX = nullptr, *pointY = nullptr;
        std::function<void()> reset; // an image: cleared by a click on its name

        switch (in.type) {
        case IsfInput::Float: {
            const double span = in.fMax - in.fMin;
            auto *bar = new SliderField;
            bar->setRange(in.fMin, in.fMax);
            bar->setDecimals(span >= 100 ? 1 : span >= 10 ? 2 : 3);
            bar->setSingleStep(span / 100.0);
            bar->setTicks(10);
            bar->setValue(in.fValue);
            connect(bar, &SliderField::valueEdited, this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &x) { x.f = v; }); });
            m_followers.push_back({idx, [bar = QPointer<SliderField>(bar)](const IsfValue &x) {
                                       if (bar && !bar->isDragging() && std::abs(bar->value() - x.f) > 1e-6) bar->setValue(x.f);
                                   }});
            field = bar;
            break;
        }
        case IsfInput::Bool: {
            auto *c = new FlagBox;
            c->setChecked(in.bValue);
            connect(c, &QCheckBox::toggled, this, [=](bool on) { setValue(idx, label, [on](IsfValue &x) { x.b = on; }); });
            m_followers.push_back({idx, [c = QPointer<QCheckBox>(c)](const IsfValue &x) {
                                       if (!c || c->isChecked() == x.b) return;
                                       QSignalBlocker blk(c);
                                       c->setChecked(x.b);
                                   }});
            field = c;
            break;
        }
        case IsfInput::Event: {
            auto *b = new QPushButton(QStringLiteral("Trigger"));
            connect(b, &QPushButton::clicked, this, [=] {
                Engine::Lock lk(&m_engine->mutex());
                if (IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot))
                    if (idx < int(inst->inputs().size())) inst->inputs()[size_t(idx)].eventFired = true;
            });
            field = b;
            break;
        }
        case IsfInput::Long: {
            auto *c = new QComboBox;
            for (int k = 0; k < in.lValues.size(); ++k) c->addItem(in.lLabels.value(k), in.lValues[k]);
            c->setCurrentIndex(qMax(0, in.lValues.indexOf(in.lValue)));
            connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int k) {
                const int v = c->itemData(k).toInt();
                setValue(idx, label, [v](IsfValue &x) { x.l = v; });
            });
            m_followers.push_back({idx, [c = QPointer<QComboBox>(c)](const IsfValue &x) {
                                       if (!c || c->view()->isVisible()) return; // its list is open
                                       const int k = c->findData(x.l);
                                       if (k < 0 || k == c->currentIndex()) return;
                                       QSignalBlocker blk(c);
                                       c->setCurrentIndex(k);
                                   }});
            field = c;
            break;
        }
        case IsfInput::Point2D: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *x = new NumberBox, *y = new NumberBox;
            const QPointF mn = in.hasPointRange ? in.pMin : QPointF(-100000, -100000);
            const QPointF mx = in.hasPointRange ? in.pMax : QPointF(100000, 100000);
            x->setRange(mn.x(), mx.x());
            y->setRange(mn.y(), mx.y());
            for (auto *s : {x, y}) {
                s->setDecimals(3);
                s->setSingleStep(in.hasPointRange ? (mx.x() - mn.x()) / 100.0 : 1.0);
                s->setKeyboardTracking(false);
            }
            x->setValue(in.pValue.x());
            y->setValue(in.pValue.y());
            x->setPrefix("x ");
            y->setPrefix("y ");
            pointX = x;
            pointY = y;
            h->addWidget(x);
            h->addWidget(y);
            connect(x, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setX(v); }); });
            connect(y, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setY(v); }); });
            m_followers.push_back({idx, [x = QPointer<QDoubleSpinBox>(x), y = QPointer<QDoubleSpinBox>(y)](const IsfValue &v) {
                                       for (auto [box, value] : {std::pair{x.data(), v.p.x()}, std::pair{y.data(), v.p.y()}}) {
                                           if (!box || box->hasFocus() || std::abs(box->value() - value) < 1e-6) continue;
                                           QSignalBlocker blk(box);
                                           box->setValue(value);
                                       }
                                   }});
            field = w;
            break;
        }
        case IsfInput::Color: {
            auto *b = new QPushButton;
            setSwatch(b, in.cValue);
            connect(b, &QPushButton::clicked, this, [=] {
                IsfValue cur;
                {
                    Engine::Lock lk(&m_engine->mutex());
                    IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
                    if (!inst || idx >= int(inst->inputs().size())) return;
                    cur = inst->inputs()[size_t(idx)].value();
                }
                const QColor start = QColor::fromRgbF(cur.c[0], cur.c[1], cur.c[2], cur.c[3]);
                // Non-modal dialog: the color is applied live while picking.
                auto *dlg = new QColorDialog(start, this);
                dlg->setOption(QColorDialog::ShowAlphaChannel);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                auto apply = [=](const QColor &c) {
                    float v[4] = {float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())};
                    setValue(idx, label, [v](IsfValue &x) { std::copy(v, v + 4, x.c); });
                    setSwatch(b, v);
                };
                connect(dlg, &QColorDialog::currentColorChanged, b, apply);
                connect(dlg, &QColorDialog::rejected, b, [=] { apply(start); });
                dlg->open();
            });
            m_followers.push_back({idx, [b = QPointer<QPushButton>(b)](const IsfValue &x) {
                                       if (!b) return;
                                       const QColor col = QColor::fromRgbF(x.c[0], x.c[1], x.c[2], x.c[3]);
                                       if (b->text() != col.name(QColor::HexArgb).toUpper()) setSwatch(b, x.c);
                                   }});
            field = b;
            break;
        }
        case IsfInput::Image: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *name = new QLabel(in.imagePath.isEmpty() ? QStringLiteral("(none)") : QFileInfo(in.imagePath).fileName());
            name->setStyleSheet("color:#bbb;");
            auto *pick = new QPushButton(QStringLiteral("Image…"));
            auto *clear = new QPushButton(QStringLiteral("×"));
            clear->setFixedWidth(28);
            h->addWidget(name, 1);
            h->addWidget(pick);
            h->addWidget(clear);
            auto instance = [this] {
                Engine::Lock lk(&m_engine->mutex());
                return cmd::resolveIsf(m_engine, m_layer, m_slot);
            };
            connect(pick, &QPushButton::clicked, this, [=] {
                QSettings s;
                const QString f = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Image for %1").arg(label), s.value("dirs/image").toString(),
                    QStringLiteral("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.gif *.webp *.tga)"));
                if (f.isEmpty()) return;
                s.setValue("dirs/image", QFileInfo(f).absolutePath());
                QString err;
                if (m_engine->setIsfImageInput(instance(), idx, f, &err)) name->setText(QFileInfo(f).fileName());
                else name->setText(err);
            });
            connect(clear, &QPushButton::clicked, this, [=] {
                m_engine->setIsfImageInput(instance(), idx, QString());
                name->setText(QStringLiteral("(none)"));
            });
            reset = [clear] { clear->click(); };
            field = w;
            break;
        }
        case IsfInput::Audio:
        case IsfInput::AudioFFT: {
            auto *l = new QLabel(QStringLiteral("audio input not supported (V1)"));
            l->setStyleSheet("color:#888;");
            field = l;
            break;
        }
        default: continue;
        }
        if (in.type == IsfInput::Event || in.type == IsfInput::Image || in.type == IsfInput::Audio ||
            in.type == IsfInput::AudioFFT) {
            if (reset) form->addRow(new ResetLabel(label, reset), field);
            else form->addRow(label, field); // a trigger, or nothing to set
            continue;
        }
        // A click on the parameter's name puts it back to its default value
        IsfValue def;
        def.f = in.fDefault;
        def.b = in.bDefault;
        def.l = in.lDefault;
        def.p = in.pDefault;
        std::copy(in.cDefault, in.cDefault + 4, def.c);
        auto *name = new ResetLabel(label, [this, idx, label, def] {
            setValue(idx, label, [def](IsfValue &x) { x = def; });
            emit rebuildRequested();
        });
        form->addRow(name, field);
        m_rows.push_back({in.name, {name, field}, pointX, pointY});
    }

    auto *reset = new QPushButton(QStringLiteral("Reset to Defaults"));
    reset->setFlat(true);
    reset->setStyleSheet("color:#aaa; text-align:left;");
    connect(reset, &QPushButton::clicked, this, [this] {
        // A single undo step for all parameters
        std::vector<std::pair<IsfValue, IsfValue>> changes;
        std::vector<QString> labels;
        {
            Engine::Lock lk(&m_engine->mutex());
            IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
            if (!inst) return;
            for (const IsfInput &in : inst->inputs()) {
                IsfValue def;
                def.f = in.fDefault;
                def.b = in.bDefault;
                def.l = in.lDefault;
                def.p = in.pDefault;
                std::copy(in.cDefault, in.cDefault + 4, def.c);
                changes.emplace_back(in.value(), def);
                labels.push_back(in.label);
            }
        }
        m_undo->beginMacro(QStringLiteral("Reset to Defaults"));
        for (size_t k = 0; k < changes.size(); ++k)
            if (changes[k].first != changes[k].second)
                m_undo->push(new cmd::SetParam(m_engine, m_layer, m_slot, int(k), changes[k].first, changes[k].second,
                                               labels[k]));
        m_undo->endMacro();
        emit rebuildRequested();
    });
    form->addRow(reset);
}

void ParamPanel::attachAnimate(AnimateMenu *menu)
{
    if (!menu) return;
    // The addresses of this shader's numbers in the layer: the generator's, or the effect's (its segment)
    QString base, speed;
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(m_layer);
        if (!l) return;
        if (m_slot < 0) {
            base = QStringLiteral("param/");
            speed = QStringLiteral("speed");
        } else {
            QStringList names;
            for (const auto &x : l->effects) names << x->name();
            const QStringList segs = osc::uniqueSegments(names);
            if (m_slot >= segs.size()) return;
            base = QStringLiteral("fx/%1/param/").arg(segs[m_slot]);
            speed = QStringLiteral("fx/%1/speed").arg(segs[m_slot]);
        }
    }
    for (const Row &r : m_rows) {
        if (r.input.isEmpty()) {
            for (QWidget *w : r.all) menu->attach(w, {speed});
            continue;
        }
        const QString p = base + r.input;
        // Every number this input has (only the ones declared animatable are offered)
        const QStringList all{p, p + "/x", p + "/y", p + "/r", p + "/g", p + "/b", p + "/a"};
        for (QWidget *w : r.all) menu->attach(w, all);
        menu->attach(r.x, {p + "/x"});
        menu->attach(r.y, {p + "/y"});
    }
}

void ParamPanel::refresh()
{
    std::vector<IsfValue> values;
    double speed = 1.0;
    {
        Engine::Lock lk(&m_engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
        if (!inst) return;
        speed = inst->speed;
        for (const IsfInput &in : inst->inputs()) values.push_back(in.value());
    }
    if (m_speed && !m_speed->isDragging() && std::abs(m_speed->value() - speed) > 1e-6) m_speed->setValue(speed);
    for (const auto &[input, show] : m_followers)
        if (input < int(values.size())) show(values[size_t(input)]);
}
