#include "ParamPanel.h"
#include "Commands.h"
#include "Engine.h"

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

    for (int idx = 0; idx < int(inputs.size()); ++idx) {
        const IsfInput &in = inputs[size_t(idx)];
        if (in.type == IsfInput::Image && in.isInputImage) continue;
        const QString label = in.label;
        QWidget *field = nullptr;

        switch (in.type) {
        case IsfInput::Float: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, 1000);
            auto *spin = new QDoubleSpinBox;
            spin->setRange(in.fMin, in.fMax);
            const double mn = in.fMin, span = in.fMax - in.fMin;
            spin->setDecimals(span >= 100 ? 1 : span >= 10 ? 2 : 3);
            spin->setSingleStep(span / 100.0);
            spin->setValue(in.fValue);
            spin->setKeyboardTracking(false);
            spin->setFixedWidth(80);
            slider->setValue(int(std::lround((in.fValue - mn) / span * 1000)));
            h->addWidget(slider, 1);
            h->addWidget(spin);
            connect(slider, &QSlider::valueChanged, this, [=](int v) {
                const double val = mn + span * v / 1000.0;
                {
                    QSignalBlocker b(spin);
                    spin->setValue(val);
                }
                setValue(idx, label, [val](IsfValue &x) { x.f = val; });
            });
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double v) {
                {
                    QSignalBlocker b(slider);
                    slider->setValue(int(std::lround((v - mn) / span * 1000)));
                }
                setValue(idx, label, [v](IsfValue &x) { x.f = v; });
            });
            field = w;
            break;
        }
        case IsfInput::Bool: {
            auto *c = new QCheckBox;
            c->setChecked(in.bValue);
            connect(c, &QCheckBox::toggled, this, [=](bool on) { setValue(idx, label, [on](IsfValue &x) { x.b = on; }); });
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
            field = c;
            break;
        }
        case IsfInput::Point2D: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *x = new QDoubleSpinBox, *y = new QDoubleSpinBox;
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
            h->addWidget(x);
            h->addWidget(y);
            connect(x, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setX(v); }); });
            connect(y, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setY(v); }); });
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
        form->addRow(label, field);
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
