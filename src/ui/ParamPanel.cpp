#include "ParamPanel.h"
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
#include <cmath>

static void setSwatch(QPushButton *b, const float c[4])
{
    const QColor col = QColor::fromRgbF(c[0], c[1], c[2], c[3]);
    b->setText(col.name(QColor::HexArgb).toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid #555; padding:3px 8px; }")
                         .arg(col.name(QColor::HexRgb), col.lightnessF() > 0.55 ? "#000" : "#fff"));
}

ParamPanel::ParamPanel(Engine *engine, IsfInstance *inst, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_inst(inst)
{
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 4, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    if (!inst->description().isEmpty()) {
        auto *d = new QLabel(inst->description());
        d->setWordWrap(true);
        d->setStyleSheet("color:#999; font-size:11px;");
        form->addRow(d);
    }

    auto &inputs = inst->inputs();
    for (int idx = 0; idx < int(inputs.size()); ++idx) {
        IsfInput &in = inputs[idx];
        if (in.type == IsfInput::Image && in.isInputImage) continue;
        auto input = [this, idx]() -> IsfInput & { return m_inst->inputs()[size_t(idx)]; };
        QString label = in.label;
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
            const double span = in.fMax - in.fMin;
            spin->setDecimals(span >= 100 ? 1 : span >= 10 ? 2 : 3);
            spin->setSingleStep(span / 100.0);
            spin->setValue(in.fValue);
            spin->setKeyboardTracking(false);
            spin->setFixedWidth(80);
            slider->setValue(int(std::lround((in.fValue - in.fMin) / span * 1000)));
            h->addWidget(slider, 1);
            h->addWidget(spin);
            connect(slider, &QSlider::valueChanged, this, [=](int v) {
                IsfInput &i = input();
                i.fValue = i.fMin + (i.fMax - i.fMin) * v / 1000.0;
                QSignalBlocker b(spin);
                spin->setValue(i.fValue);
            });
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double v) {
                IsfInput &i = input();
                i.fValue = v;
                QSignalBlocker b(slider);
                slider->setValue(int(std::lround((v - i.fMin) / (i.fMax - i.fMin) * 1000)));
            });
            field = w;
            break;
        }
        case IsfInput::Bool: {
            auto *c = new QCheckBox;
            c->setChecked(in.bValue);
            connect(c, &QCheckBox::toggled, this, [=](bool on) { input().bValue = on; });
            field = c;
            break;
        }
        case IsfInput::Event: {
            auto *b = new QPushButton(QStringLiteral("Déclencher"));
            connect(b, &QPushButton::clicked, this, [=] { input().eventFired = true; });
            field = b;
            break;
        }
        case IsfInput::Long: {
            auto *c = new QComboBox;
            for (int k = 0; k < in.lValues.size(); ++k) c->addItem(in.lLabels.value(k), in.lValues[k]);
            c->setCurrentIndex(qMax(0, in.lValues.indexOf(in.lValue)));
            connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [=](int k) { input().lValue = c->itemData(k).toInt(); });
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
            connect(x, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double v) { input().pValue.setX(v); });
            connect(y, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double v) { input().pValue.setY(v); });
            field = w;
            break;
        }
        case IsfInput::Color: {
            auto *b = new QPushButton;
            setSwatch(b, in.cValue);
            connect(b, &QPushButton::clicked, this, [=] {
                IsfInput &i = input();
                const QColor start = QColor::fromRgbF(i.cValue[0], i.cValue[1], i.cValue[2], i.cValue[3]);
                // Dialogue non bloquant : la couleur est appliquée en direct pendant le choix.
                auto *dlg = new QColorDialog(start, this);
                dlg->setOption(QColorDialog::ShowAlphaChannel);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                auto apply = [=](const QColor &c) {
                    IsfInput &ii = input();
                    ii.cValue[0] = float(c.redF());
                    ii.cValue[1] = float(c.greenF());
                    ii.cValue[2] = float(c.blueF());
                    ii.cValue[3] = float(c.alphaF());
                    setSwatch(b, ii.cValue);
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
            auto *name = new QLabel(in.imagePath.isEmpty() ? QStringLiteral("(aucune)") : QFileInfo(in.imagePath).fileName());
            name->setStyleSheet("color:#bbb;");
            auto *pick = new QPushButton(QStringLiteral("Image…"));
            auto *clear = new QPushButton(QStringLiteral("×"));
            clear->setFixedWidth(28);
            h->addWidget(name, 1);
            h->addWidget(pick);
            h->addWidget(clear);
            connect(pick, &QPushButton::clicked, this, [=] {
                QSettings s;
                const QString f = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Image pour %1").arg(input().name), s.value("dirs/image").toString(),
                    QStringLiteral("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.gif *.webp *.tga)"));
                if (f.isEmpty()) return;
                s.setValue("dirs/image", QFileInfo(f).absolutePath());
                QString err;
                if (m_engine->setIsfImageInput(m_inst, idx, f, &err)) name->setText(QFileInfo(f).fileName());
                else name->setText(err);
            });
            connect(clear, &QPushButton::clicked, this, [=] {
                m_engine->setIsfImageInput(m_inst, idx, QString());
                name->setText(QStringLiteral("(aucune)"));
            });
            field = w;
            break;
        }
        case IsfInput::Audio:
        case IsfInput::AudioFFT: {
            auto *l = new QLabel(QStringLiteral("entrée audio non gérée (V1)"));
            l->setStyleSheet("color:#888;");
            field = l;
            break;
        }
        default: continue;
        }
        form->addRow(label, field);
    }

    auto *reset = new QPushButton(QStringLiteral("Valeurs par défaut"));
    reset->setFlat(true);
    reset->setStyleSheet("color:#aaa; text-align:left;");
    connect(reset, &QPushButton::clicked, this, [this] {
        m_inst->resetParams();
        emit rebuildRequested();
    });
    form->addRow(reset);
}
