#include "LayerInspector.h"
#include "Engine.h"
#include "ParamPanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static QString fmtTime(double s)
{
    if (s < 0) s = 0;
    const int m = int(s) / 60;
    const double r = s - m * 60;
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(r, 5, 'f', 2, QLatin1Char('0'));
}

static QGroupBox *group(const QString &title)
{
    auto *g = new QGroupBox(title);
    return g;
}

static QLabel *errorLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet("color:#ff6b5b; font-family:monospace; font-size:11px;");
    return l;
}

static QToolButton *toolButton(const QString &text, const QString &tip)
{
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setMinimumWidth(28);
    return b;
}

LayerInspector::LayerInspector(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(8, 8, 8, 8);
    rebuild();
}

void LayerInspector::setLayer(int index)
{
    if (index != m_layer) m_selectedEffect = 0;
    m_layer = index;
    rebuild();
}

void LayerInspector::rebuild()
{
    if (m_content) {
        m_content->hide();
        m_content->deleteLater();
    }
    m_content = new QWidget;
    auto *v = new QVBoxLayout(m_content);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    m_layout->addWidget(m_content);

    Layer *l = m_engine->layer(m_layer);
    if (!l) {
        auto *empty = new QLabel(QStringLiteral("Aucun calque sélectionné.\n\nAjoutez un calque avec le bouton +\n"
                                                "ou glissez des vidéos, images ou\nshaders ISF dans la fenêtre."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet("color:#888;");
        v->addWidget(empty);
        v->addStretch();
        return;
    }

    // En-tête : nom + visibilité
    auto *head = new QHBoxLayout;
    auto *name = new QLineEdit(l->name);
    name->setStyleSheet("font-weight:bold; font-size:14px;");
    auto *vis = new QCheckBox(QStringLiteral("Visible"));
    vis->setChecked(l->visible);
    head->addWidget(name, 1);
    head->addWidget(vis);
    v->addLayout(head);
    connect(name, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (Layer *ly = m_engine->layer(m_layer)) ly->name = t;
        emit layerChanged();
    });
    connect(vis, &QCheckBox::toggled, this, [this](bool on) {
        if (Layer *ly = m_engine->layer(m_layer)) ly->visible = on;
        emit layerChanged();
    });

    v->addWidget(buildSource());
    v->addWidget(buildCompositing());
    v->addWidget(buildMapping());
    v->addWidget(buildEffects());
    v->addStretch();
    refreshDynamic();
}

QWidget *LayerInspector::buildSource()
{
    Layer *l = m_engine->layer(m_layer);
    auto *g = group(QStringLiteral("Source"));
    auto *v = new QVBoxLayout(g);

    QString desc;
    switch (l->type) {
    case SourceType::Video:
        desc = QStringLiteral("Vidéo — %1").arg(QFileInfo(l->sourcePath).fileName());
        break;
    case SourceType::Image: desc = QStringLiteral("Image — %1").arg(QFileInfo(l->sourcePath).fileName()); break;
    case SourceType::Isf: desc = QStringLiteral("Générateur ISF — %1").arg(QFileInfo(l->sourcePath).completeBaseName()); break;
    default: desc = QStringLiteral("Aucune source"); break;
    }
    auto *title = new QLabel(desc);
    title->setWordWrap(true);
    title->setToolTip(l->sourcePath);
    v->addWidget(title);

    auto *buttons = new QHBoxLayout;
    auto *bVideo = new QPushButton(QStringLiteral("Vidéo…"));
    auto *bImage = new QPushButton(QStringLiteral("Image…"));
    auto *bGen = new QPushButton(QStringLiteral("Générateur"));
    auto *genMenu = new QMenu(bGen);
    for (const IsfEntry &e : m_engine->library().generators()) {
        QAction *a = genMenu->addAction(e.name);
        a->setToolTip(e.description);
        connect(a, &QAction::triggered, this, [this, p = e.path] { chooseGenerator(p); });
    }
    if (genMenu->isEmpty()) genMenu->addAction(QStringLiteral("(bibliothèque vide)"))->setEnabled(false);
    bGen->setMenu(genMenu);
    auto *bClear = toolButton(QStringLiteral("×"), QStringLiteral("Retirer la source"));
    buttons->addWidget(bVideo);
    buttons->addWidget(bImage);
    buttons->addWidget(bGen);
    buttons->addWidget(bClear);
    v->addLayout(buttons);
    connect(bVideo, &QPushButton::clicked, this, [this] { emit addSourceRequested("video"); });
    connect(bImage, &QPushButton::clicked, this, [this] { emit addSourceRequested("image"); });
    connect(bClear, &QToolButton::clicked, this, [this] {
        m_engine->clearLayerSource(m_layer);
        emit layerChanged();
        rebuild();
    });

    if (!l->error.isEmpty()) v->addWidget(errorLabel(l->error));

    if (l->type == SourceType::Video && l->video) {
        auto *info = new QLabel(QStringLiteral("%1 × %2 · %3 i/s · %4 · %5")
                                    .arg(l->video->width())
                                    .arg(l->video->height())
                                    .arg(l->video->fps(), 0, 'f', 2)
                                    .arg(l->video->codecName())
                                    .arg(fmtTime(l->duration())));
        info->setStyleSheet("color:#999; font-size:11px;");
        v->addWidget(info);

        auto *transport = new QHBoxLayout;
        m_play = new QPushButton(l->playing ? QStringLiteral("Pause") : QStringLiteral("Lecture"));
        auto *rewind = toolButton(QStringLiteral("⏮"), QStringLiteral("Retour au début"));
        auto *loop = new QCheckBox(QStringLiteral("Boucle"));
        loop->setChecked(l->loop);
        auto *speed = new QDoubleSpinBox;
        speed->setRange(0.05, 8.0);
        speed->setSingleStep(0.05);
        speed->setValue(l->speed);
        speed->setSuffix(QStringLiteral(" ×"));
        speed->setToolTip(QStringLiteral("Vitesse de lecture"));
        transport->addWidget(m_play);
        transport->addWidget(rewind);
        transport->addWidget(loop);
        transport->addStretch();
        transport->addWidget(speed);
        v->addLayout(transport);

        auto *seekRow = new QHBoxLayout;
        m_seek = new QSlider(Qt::Horizontal);
        m_seek->setRange(0, 10000);
        m_time = new QLabel;
        m_time->setStyleSheet("font-family:monospace;");
        seekRow->addWidget(m_seek, 1);
        seekRow->addWidget(m_time);
        v->addLayout(seekRow);

        connect(m_play, &QPushButton::clicked, this, [this] {
            Layer *ly = m_engine->layer(m_layer);
            if (!ly) return;
            m_engine->setLayerPlaying(m_layer, !ly->playing);
            refreshDynamic();
        });
        connect(rewind, &QToolButton::clicked, this, [this] { m_engine->seekLayer(m_layer, 0); });
        connect(loop, &QCheckBox::toggled, this, [this](bool on) { m_engine->setLayerLoop(m_layer, on); });
        connect(speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double s) {
            if (Layer *ly = m_engine->layer(m_layer)) ly->speed = s;
        });
        auto seekTo = [this](int v) {
            Layer *ly = m_engine->layer(m_layer);
            if (ly && ly->duration() > 0) m_engine->seekLayer(m_layer, ly->duration() * v / 10000.0);
        };
        connect(m_seek, &QSlider::sliderMoved, this, seekTo);
        connect(m_seek, &QSlider::actionTriggered, this, [this, seekTo](int action) {
            if (action != QAbstractSlider::SliderMove) seekTo(m_seek->sliderPosition());
        });
    }

    if (l->type == SourceType::Isf && l->generator) {
        auto *res = new QHBoxLayout;
        auto *w = new QSpinBox, *h = new QSpinBox;
        for (auto *s : {w, h}) {
            s->setRange(1, 16384);
            s->setKeyboardTracking(false);
        }
        w->setValue(l->genWidth);
        h->setValue(l->genHeight);
        auto *fit = new QPushButton(QStringLiteral("= composition"));
        auto *reload = toolButton(QStringLiteral("⟳"), QStringLiteral("Recharger le shader depuis le disque"));
        res->addWidget(new QLabel(QStringLiteral("Résolution")));
        res->addWidget(w);
        res->addWidget(new QLabel(QStringLiteral("×")));
        res->addWidget(h);
        res->addWidget(fit);
        res->addWidget(reload);
        v->addLayout(res);
        auto apply = [this, w, h] { m_engine->setGeneratorSize(m_layer, w->value(), h->value()); };
        connect(w, qOverload<int>(&QSpinBox::valueChanged), this, apply);
        connect(h, qOverload<int>(&QSpinBox::valueChanged), this, apply);
        connect(fit, &QPushButton::clicked, this, [this, w, h] {
            const QSize c = m_engine->compositionSize();
            w->setValue(c.width());
            h->setValue(c.height());
        });
        connect(reload, &QToolButton::clicked, this, [this] {
            if (Layer *ly = m_engine->layer(m_layer)) m_engine->reloadIsf(ly->generator.get());
            rebuild();
        });
        auto *params = new ParamPanel(m_engine, l->generator.get());
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    return g;
}

void LayerInspector::chooseGenerator(const QString &path)
{
    QString err;
    m_engine->setLayerIsf(m_layer, path, &err);
    if (Layer *l = m_engine->layer(m_layer)) {
        if (l->name.startsWith(QStringLiteral("Calque "))) l->name = QFileInfo(path).completeBaseName();
    }
    emit layerChanged();
    rebuild();
}

QWidget *LayerInspector::buildCompositing()
{
    Layer *l = m_engine->layer(m_layer);
    auto *g = group(QStringLiteral("Composition"));
    auto *form = new QFormLayout(g);

    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(int(std::lround(l->opacity * 100)));
    auto *spin = new QSpinBox;
    spin->setRange(0, 100);
    spin->setSuffix(" %");
    spin->setValue(slider->value());
    h->addWidget(slider, 1);
    h->addWidget(spin);
    form->addRow(QStringLiteral("Opacité"), row);
    connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    connect(slider, &QSlider::valueChanged, this, [this](int v) {
        if (Layer *ly = m_engine->layer(m_layer)) ly->opacity = v / 100.0f;
    });

    auto *blend = new QComboBox;
    for (BlendMode m : {BlendMode::Normal, BlendMode::Add, BlendMode::Screen, BlendMode::Multiply})
        blend->addItem(blendModeName(m), int(m));
    blend->setCurrentIndex(blend->findData(int(l->blend)));
    form->addRow(QStringLiteral("Fusion"), blend);
    connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, blend](int i) {
        if (Layer *ly = m_engine->layer(m_layer)) ly->blend = BlendMode(blend->itemData(i).toInt());
    });
    return g;
}

QWidget *LayerInspector::buildMapping()
{
    Layer *l = m_engine->layer(m_layer);
    auto *g = group(QStringLiteral("Mapping"));
    auto *v = new QVBoxLayout(g);

    auto *modeRow = new QHBoxLayout;
    auto *corners = new QRadioButton(QStringLiteral("Coins"));
    auto *mesh = new QRadioButton(QStringLiteral("Grille"));
    (l->mapping.meshMode ? mesh : corners)->setChecked(true);
    auto *modes = new QButtonGroup(g);
    modes->addButton(corners, 0);
    modes->addButton(mesh, 1);
    auto *cols = new QSpinBox, *rows = new QSpinBox;
    cols->setRange(2, 32);
    rows->setRange(2, 32);
    cols->setValue(l->mapping.cols);
    rows->setValue(l->mapping.rows);
    cols->setToolTip(QStringLiteral("Colonnes de points"));
    rows->setToolTip(QStringLiteral("Lignes de points"));
    auto *apply = new QPushButton(QStringLiteral("Appliquer"));
    apply->setToolTip(QStringLiteral("Change la densité de la grille (réinitialise la déformation)"));
    modeRow->addWidget(corners);
    modeRow->addWidget(mesh);
    modeRow->addStretch();
    modeRow->addWidget(cols);
    modeRow->addWidget(new QLabel(QStringLiteral("×")));
    modeRow->addWidget(rows);
    modeRow->addWidget(apply);
    v->addLayout(modeRow);

    auto *actions = new QHBoxLayout;
    auto *full = new QPushButton(QStringLiteral("Plein cadre"));
    auto *ratio = new QPushButton(QStringLiteral("Ratio source"));
    auto *resetMesh = new QPushButton(QStringLiteral("Aplanir la grille"));
    actions->addWidget(full);
    actions->addWidget(ratio);
    actions->addWidget(resetMesh);
    v->addLayout(actions);

    auto *hint = new QLabel(QStringLiteral("Glisser : déplacer · Maj : précision · Flèches : 1 px (Maj : 10 px) · "
                                           "Tab : poignée suivante · Échap : désélection"));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#888; font-size:11px;");
    v->addWidget(hint);

    connect(modes, &QButtonGroup::idClicked, this, [this](int id) {
        if (Layer *ly = m_engine->layer(m_layer)) ly->mapping.meshMode = id == 1;
        emit mappingChanged();
    });
    connect(apply, &QPushButton::clicked, this, [this, cols, rows] {
        Layer *ly = m_engine->layer(m_layer);
        if (!ly) return;
        bool deformed = false;
        for (const QPointF &o : ly->mapping.offsets) deformed |= !o.isNull();
        if (deformed && QMessageBox::question(this, QStringLiteral("Grille"),
                                              QStringLiteral("Changer la densité efface la déformation actuelle. Continuer ?"))
                            != QMessageBox::Yes)
            return;
        ly->mapping.resetMesh(cols->value(), rows->value());
        ly->mapping.meshMode = true;
        emit mappingChanged();
        rebuild();
    });
    connect(full, &QPushButton::clicked, this, [this] {
        if (Layer *ly = m_engine->layer(m_layer)) ly->mapping.resetCorners();
        emit mappingChanged();
    });
    connect(ratio, &QPushButton::clicked, this, [this] {
        Layer *ly = m_engine->layer(m_layer);
        if (!ly || ly->sourceHeight() <= 0) return;
        const QSize c = m_engine->compositionSize();
        ly->mapping.fitAspect(double(ly->sourceWidth()) / ly->sourceHeight(), double(c.width()) / c.height());
        emit mappingChanged();
    });
    connect(resetMesh, &QPushButton::clicked, this, [this] {
        if (Layer *ly = m_engine->layer(m_layer)) ly->mapping.resetMesh(ly->mapping.cols, ly->mapping.rows);
        emit mappingChanged();
    });
    return g;
}

QWidget *LayerInspector::buildEffects()
{
    Layer *l = m_engine->layer(m_layer);
    auto *g = group(QStringLiteral("Effets ISF"));
    auto *v = new QVBoxLayout(g);

    auto *bar = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Ajouter un effet"));
    auto *menu = new QMenu(add);
    add->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        // Regroupe par catégorie ISF
        QMap<QString, QMenu *> sub;
        for (const IsfEntry &e : m_engine->library().filters()) {
            const QString cat = e.categories.value(0);
            QMenu *target = menu;
            if (!cat.isEmpty()) {
                if (!sub.contains(cat)) sub[cat] = menu->addMenu(cat);
                target = sub[cat];
            }
            QAction *a = target->addAction(e.name);
            a->setToolTip(e.description);
            connect(a, &QAction::triggered, this, [this, p = e.path] {
                QString err;
                m_selectedEffect = m_engine->addEffect(m_layer, p, &err);
                rebuild();
            });
        }
        if (menu->isEmpty()) menu->addAction(QStringLiteral("(aucun filtre dans la bibliothèque)"))->setEnabled(false);
    });
    auto *remove = toolButton(QStringLiteral("−"), QStringLiteral("Retirer l'effet"));
    auto *up = toolButton(QStringLiteral("▲"), QStringLiteral("Monter (appliqué plus tôt)"));
    auto *down = toolButton(QStringLiteral("▼"), QStringLiteral("Descendre (appliqué plus tard)"));
    auto *reload = toolButton(QStringLiteral("⟳"), QStringLiteral("Recharger le shader depuis le disque"));
    bar->addWidget(add, 1);
    bar->addWidget(remove);
    bar->addWidget(up);
    bar->addWidget(down);
    bar->addWidget(reload);
    v->addLayout(bar);

    if (l->effects.empty()) {
        auto *none = new QLabel(QStringLiteral("Aucun effet. Les effets s'appliquent dans l'ordre de la liste."));
        none->setWordWrap(true);
        none->setStyleSheet("color:#888;");
        v->addWidget(none);
        remove->setEnabled(false);
        up->setEnabled(false);
        down->setEnabled(false);
        reload->setEnabled(false);
        return g;
    }

    m_selectedEffect = std::clamp(m_selectedEffect, 0, int(l->effects.size()) - 1);
    auto *list = new QListWidget;
    for (const auto &fx : l->effects) {
        auto *it = new QListWidgetItem(fx->name() + (fx->isValid() ? QString() : QStringLiteral("  ⚠")));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(fx->enabled ? Qt::Checked : Qt::Unchecked);
        list->addItem(it);
    }
    list->setCurrentRow(m_selectedEffect);
    list->setMaximumHeight(qMin(160, 26 * int(l->effects.size()) + 8));
    v->addWidget(list);

    connect(list, &QListWidget::itemChanged, this, [this, list](QListWidgetItem *it) {
        Layer *ly = m_engine->layer(m_layer);
        const int r = list->row(it);
        if (ly && r >= 0 && r < int(ly->effects.size())) ly->effects[size_t(r)]->enabled = it->checkState() == Qt::Checked;
    });
    connect(list, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0 && r != m_selectedEffect) {
            m_selectedEffect = r;
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        }
    });
    connect(remove, &QToolButton::clicked, this, [this] {
        m_engine->removeEffect(m_layer, m_selectedEffect);
        rebuild();
    });
    connect(up, &QToolButton::clicked, this, [this] {
        m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect - 1);
        m_selectedEffect = qMax(0, m_selectedEffect - 1);
        rebuild();
    });
    connect(down, &QToolButton::clicked, this, [this] {
        Layer *ly = m_engine->layer(m_layer);
        m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect + 1);
        if (ly) m_selectedEffect = qMin(int(ly->effects.size()) - 1, m_selectedEffect + 1);
        rebuild();
    });
    connect(reload, &QToolButton::clicked, this, [this] {
        Layer *ly = m_engine->layer(m_layer);
        if (ly && m_selectedEffect < int(ly->effects.size())) m_engine->reloadIsf(ly->effects[size_t(m_selectedEffect)].get());
        rebuild();
    });

    IsfInstance *fx = l->effects[size_t(m_selectedEffect)].get();
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(fx->name().toHtmlEscaped()));
    v->addWidget(title);
    if (!fx->error().isEmpty()) v->addWidget(errorLabel(fx->error()));
    if (fx->isValid()) {
        auto *params = new ParamPanel(m_engine, fx);
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    return g;
}

void LayerInspector::refreshDynamic()
{
    Layer *l = m_engine->layer(m_layer);
    if (!l || l->type != SourceType::Video) return;
    if (m_play) m_play->setText(l->playing ? QStringLiteral("Pause") : QStringLiteral("Lecture"));
    const double d = l->duration();
    const double p = l->position();
    if (m_seek && !m_seek->isSliderDown() && d > 0) {
        QSignalBlocker b(m_seek);
        m_seek->setValue(int(std::lround(p / d * 10000)));
    }
    if (m_time) m_time->setText(fmtTime(p) + " / " + fmtTime(d));
}
