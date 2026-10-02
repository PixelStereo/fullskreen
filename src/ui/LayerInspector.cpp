#include "LayerInspector.h"
#include "Commands.h"
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
#include <QUndoStack>
#include <QVBoxLayout>
#include <cmath>

// Copie de l'état d'un calque prise sous verrou : les widgets sont construits ensuite sans bloquer le rendu.
struct LayerSnapshot {
    bool valid = false;
    QString name, sourcePath, error;
    bool visible = true;
    float opacity = 1;
    BlendMode blend = BlendMode::Normal;
    SourceType type = SourceType::None;
    bool hasVideo = false, playing = false, loop = true;
    int videoW = 0, videoH = 0;
    double fps = 0, duration = 0, speed = 1;
    QString codec;
    bool hasGenerator = false;
    int genW = 0, genH = 0;
    struct Fx {
        QString name, error;
        bool valid = false, enabled = true;
    };
    std::vector<Fx> effects;
    bool meshMode = false;
    int cols = 4, rows = 4;

    static LayerSnapshot take(Engine *e, int index)
    {
        LayerSnapshot s;
        Engine::Lock lk(&e->mutex());
        Layer *l = e->layer(index);
        if (!l) return s;
        s.valid = true;
        s.name = l->name;
        s.sourcePath = l->sourcePath;
        s.error = l->error;
        s.visible = l->visible;
        s.opacity = l->opacity;
        s.blend = l->blend;
        s.type = l->type;
        if (l->video) {
            s.hasVideo = true;
            s.videoW = l->video->width();
            s.videoH = l->video->height();
            s.fps = l->video->fps();
            s.codec = l->video->codecName();
            s.duration = l->duration();
        }
        s.playing = l->playing;
        s.loop = l->loop;
        s.speed = l->speed;
        s.hasGenerator = l->generator != nullptr;
        s.genW = l->genWidth;
        s.genH = l->genHeight;
        for (const auto &fx : l->effects) s.effects.push_back({fx->name(), fx->error(), fx->isValid(), fx->enabled});
        s.meshMode = l->mapping.meshMode;
        s.cols = l->mapping.cols;
        s.rows = l->mapping.rows;
        return s;
    }
};

static QString fmtTime(double s)
{
    if (s < 0) s = 0;
    const int m = int(s) / 60;
    const double r = s - m * 60;
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(r, 5, 'f', 2, QLatin1Char('0'));
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

LayerInspector::LayerInspector(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo)
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

// --- Modifications annulables ------------------------------------------------

void LayerInspector::setProp(int prop, const QVariant &value)
{
    const auto p = cmd::SetLayerProp::Prop(prop);
    const QVariant before = cmd::SetLayerProp::read(m_engine, m_layer, p);
    if (!before.isValid() || before == value) return;
    m_undo->push(new cmd::SetLayerProp(m_engine, m_layer, p, before, value));
}

void LayerInspector::editMapping(const QString &text, const std::function<void(Mapping &)> &fn)
{
    const Mapping before = cmd::SetMapping::read(m_engine, m_layer);
    Mapping after = before;
    fn(after);
    m_undo->push(new cmd::SetMapping(m_engine, m_layer, before, after, text));
    emit mappingChanged();
}

void LayerInspector::editEffects(const QString &text, const std::function<void()> &op)
{
    const QJsonArray before = m_engine->effectsJson(m_layer);
    op();
    m_undo->push(new cmd::SetEffects(m_engine, m_layer, before, text));
}

void LayerInspector::editSource(const QString &text, const std::function<void()> &op)
{
    const QJsonObject before = m_engine->layerJson(m_layer);
    op();
    m_undo->push(new cmd::ReplaceLayer(m_engine, m_layer, before, text));
}

// --- Construction ------------------------------------------------------------

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

    const LayerSnapshot s = LayerSnapshot::take(m_engine, m_layer);
    if (!s.valid) {
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
    auto *name = new QLineEdit(s.name);
    name->setStyleSheet("font-weight:bold; font-size:14px;");
    auto *vis = new QCheckBox(QStringLiteral("Visible"));
    vis->setChecked(s.visible);
    head->addWidget(name, 1);
    head->addWidget(vis);
    v->addLayout(head);
    connect(name, &QLineEdit::textEdited, this, [this](const QString &t) {
        setProp(cmd::SetLayerProp::Name, t);
        emit layerChanged();
    });
    connect(vis, &QCheckBox::toggled, this, [this](bool on) {
        setProp(cmd::SetLayerProp::Visible, on);
        emit layerChanged();
    });

    v->addWidget(buildSource(s));
    v->addWidget(buildCompositing(s));
    v->addWidget(buildMapping(s));
    v->addWidget(buildEffects(s));
    v->addStretch();
    refreshDynamic();
}

QWidget *LayerInspector::buildSource(const LayerSnapshot &s)
{
    auto *g = new QGroupBox(QStringLiteral("Source"));
    auto *v = new QVBoxLayout(g);

    QString desc;
    switch (s.type) {
    case SourceType::Video: desc = QStringLiteral("Vidéo — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Image: desc = QStringLiteral("Image — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Isf: desc = QStringLiteral("Générateur ISF — %1").arg(QFileInfo(s.sourcePath).completeBaseName()); break;
    default: desc = QStringLiteral("Aucune source"); break;
    }
    auto *title = new QLabel(desc);
    title->setWordWrap(true);
    title->setToolTip(s.sourcePath);
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
        editSource(QStringLiteral("Retirer la source"), [this] { m_engine->clearLayerSource(m_layer); });
        emit layerChanged();
        rebuild();
    });

    if (!s.error.isEmpty()) v->addWidget(errorLabel(s.error));

    if (s.type == SourceType::Video && s.hasVideo) {
        auto *info = new QLabel(QStringLiteral("%1 × %2 · %3 i/s · %4 · %5")
                                    .arg(s.videoW)
                                    .arg(s.videoH)
                                    .arg(s.fps, 0, 'f', 2)
                                    .arg(s.codec)
                                    .arg(fmtTime(s.duration)));
        info->setStyleSheet("color:#999; font-size:11px;");
        v->addWidget(info);

        auto *transport = new QHBoxLayout;
        m_play = new QPushButton(s.playing ? QStringLiteral("Pause") : QStringLiteral("Lecture"));
        auto *rewind = toolButton(QStringLiteral("⏮"), QStringLiteral("Retour au début"));
        auto *loop = new QCheckBox(QStringLiteral("Boucle"));
        loop->setChecked(s.loop);
        auto *speed = new QDoubleSpinBox;
        speed->setRange(0.05, 8.0);
        speed->setSingleStep(0.05);
        speed->setValue(s.speed);
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

        // La lecture n'est pas une modification du projet : pas d'annulation.
        connect(m_play, &QPushButton::clicked, this, [this] {
            bool playing;
            {
                Engine::Lock lk(&m_engine->mutex());
                Layer *ly = m_engine->layer(m_layer);
                if (!ly) return;
                playing = ly->playing;
            }
            m_engine->setLayerPlaying(m_layer, !playing);
            refreshDynamic();
        });
        connect(rewind, &QToolButton::clicked, this, [this] { m_engine->seekLayer(m_layer, 0); });
        connect(loop, &QCheckBox::toggled, this, [this](bool on) { setProp(cmd::SetLayerProp::Loop, on); });
        connect(speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double sp) { setProp(cmd::SetLayerProp::Speed, sp); });
        const double duration = s.duration;
        auto seekTo = [this, duration](int pos) {
            if (duration > 0) m_engine->seekLayer(m_layer, duration * pos / 10000.0);
        };
        connect(m_seek, &QSlider::sliderMoved, this, seekTo);
        connect(m_seek, &QSlider::actionTriggered, this, [this, seekTo](int action) {
            if (action != QAbstractSlider::SliderMove) seekTo(m_seek->sliderPosition());
        });
    }

    if (s.type == SourceType::Isf && s.hasGenerator) {
        auto *res = new QHBoxLayout;
        auto *w = new QSpinBox, *h = new QSpinBox;
        for (auto *sb : {w, h}) {
            sb->setRange(1, 16384);
            sb->setKeyboardTracking(false);
        }
        w->setValue(s.genW);
        h->setValue(s.genH);
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
            IsfInstance *gen;
            {
                Engine::Lock lk(&m_engine->mutex());
                Layer *ly = m_engine->layer(m_layer);
                gen = ly ? ly->generator.get() : nullptr;
            }
            m_engine->reloadIsf(gen);
            rebuild();
        });
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, -1);
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    return g;
}

void LayerInspector::chooseGenerator(const QString &path)
{
    editSource(QStringLiteral("Générateur %1").arg(QFileInfo(path).completeBaseName()), [this, path] {
        QString err;
        m_engine->setLayerIsf(m_layer, path, &err);
        Engine::Lock lk(&m_engine->mutex());
        if (Layer *l = m_engine->layer(m_layer))
            if (l->name.startsWith(QStringLiteral("Calque "))) l->name = QFileInfo(path).completeBaseName();
    });
    emit layerChanged();
    rebuild();
}

QWidget *LayerInspector::buildCompositing(const LayerSnapshot &s)
{
    auto *g = new QGroupBox(QStringLiteral("Composition"));
    auto *form = new QFormLayout(g);

    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(int(std::lround(s.opacity * 100)));
    auto *spin = new QSpinBox;
    spin->setRange(0, 100);
    spin->setSuffix(" %");
    spin->setValue(slider->value());
    h->addWidget(slider, 1);
    h->addWidget(spin);
    form->addRow(QStringLiteral("Opacité"), row);
    connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    connect(slider, &QSlider::valueChanged, this, [this](int v) { setProp(cmd::SetLayerProp::Opacity, v / 100.0); });

    auto *blend = new QComboBox;
    for (BlendMode m : {BlendMode::Normal, BlendMode::Add, BlendMode::Screen, BlendMode::Multiply})
        blend->addItem(blendModeName(m), int(m));
    blend->setCurrentIndex(blend->findData(int(s.blend)));
    form->addRow(QStringLiteral("Fusion"), blend);
    connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, blend](int i) { setProp(cmd::SetLayerProp::Blend, blend->itemData(i).toInt()); });
    return g;
}

QWidget *LayerInspector::buildMapping(const LayerSnapshot &s)
{
    auto *g = new QGroupBox(QStringLiteral("Mapping"));
    auto *v = new QVBoxLayout(g);

    auto *modeRow = new QHBoxLayout;
    auto *corners = new QRadioButton(QStringLiteral("Coins"));
    auto *mesh = new QRadioButton(QStringLiteral("Grille"));
    (s.meshMode ? mesh : corners)->setChecked(true);
    auto *modes = new QButtonGroup(g);
    modes->addButton(corners, 0);
    modes->addButton(mesh, 1);
    auto *cols = new QSpinBox, *rows = new QSpinBox;
    cols->setRange(2, 32);
    rows->setRange(2, 32);
    cols->setValue(s.cols);
    rows->setValue(s.rows);
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
        editMapping(id == 1 ? QStringLiteral("Mode grille") : QStringLiteral("Mode coins"),
                    [id](Mapping &m) { m.meshMode = id == 1; });
    });
    connect(apply, &QPushButton::clicked, this, [this, cols, rows] {
        bool deformed = false;
        for (const QPointF &o : cmd::SetMapping::read(m_engine, m_layer).offsets) deformed |= !o.isNull();
        if (deformed && QMessageBox::question(this, QStringLiteral("Grille"),
                                              QStringLiteral("Changer la densité efface la déformation actuelle "
                                                             "(annulable avec Ctrl+Z). Continuer ?"))
                            != QMessageBox::Yes)
            return;
        const int c = cols->value(), r = rows->value();
        editMapping(QStringLiteral("Densité de la grille"), [c, r](Mapping &m) {
            m.resetMesh(c, r);
            m.meshMode = true;
        });
        rebuild();
    });
    connect(full, &QPushButton::clicked, this,
            [this] { editMapping(QStringLiteral("Plein cadre"), [](Mapping &m) { m.resetCorners(); }); });
    connect(ratio, &QPushButton::clicked, this, [this] {
        int sw, sh;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *ly = m_engine->layer(m_layer);
            if (!ly) return;
            sw = ly->sourceWidth();
            sh = ly->sourceHeight();
        }
        if (sh <= 0) return;
        const QSize c = m_engine->compositionSize();
        editMapping(QStringLiteral("Ratio source"),
                    [=](Mapping &m) { m.fitAspect(double(sw) / sh, double(c.width()) / c.height()); });
    });
    connect(resetMesh, &QPushButton::clicked, this, [this] {
        editMapping(QStringLiteral("Aplanir la grille"), [](Mapping &m) { m.resetMesh(m.cols, m.rows); });
    });
    return g;
}

QWidget *LayerInspector::buildEffects(const LayerSnapshot &s)
{
    auto *g = new QGroupBox(QStringLiteral("Effets ISF"));
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
            connect(a, &QAction::triggered, this, [this, p = e.path, n = e.name] {
                editEffects(QStringLiteral("Ajouter l'effet %1").arg(n), [this, p] {
                    QString err;
                    m_selectedEffect = m_engine->addEffect(m_layer, p, &err);
                });
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

    if (s.effects.empty()) {
        auto *none = new QLabel(QStringLiteral("Aucun effet. Les effets s'appliquent dans l'ordre de la liste."));
        none->setWordWrap(true);
        none->setStyleSheet("color:#888;");
        v->addWidget(none);
        for (auto *b : {remove, up, down, reload}) b->setEnabled(false);
        return g;
    }

    const int count = int(s.effects.size());
    m_selectedEffect = std::clamp(m_selectedEffect, 0, count - 1);
    auto *list = new QListWidget;
    for (const auto &fx : s.effects) {
        auto *it = new QListWidgetItem(fx.name + (fx.valid ? QString() : QStringLiteral("  ⚠")));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(fx.enabled ? Qt::Checked : Qt::Unchecked);
        list->addItem(it);
    }
    list->setCurrentRow(m_selectedEffect);
    list->setMaximumHeight(qMin(160, 26 * count + 8));
    v->addWidget(list);

    connect(list, &QListWidget::itemChanged, this, [this, list](QListWidgetItem *it) {
        const int r = list->row(it);
        const bool on = it->checkState() == Qt::Checked;
        editEffects(on ? QStringLiteral("Activer l'effet") : QStringLiteral("Désactiver l'effet"), [this, r, on] {
            Engine::Lock lk(&m_engine->mutex());
            Layer *ly = m_engine->layer(m_layer);
            if (ly && r >= 0 && r < int(ly->effects.size())) ly->effects[size_t(r)]->enabled = on;
        });
    });
    connect(list, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0 && r != m_selectedEffect) {
            m_selectedEffect = r;
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        }
    });
    connect(remove, &QToolButton::clicked, this, [this] {
        editEffects(QStringLiteral("Retirer l'effet"), [this] { m_engine->removeEffect(m_layer, m_selectedEffect); });
        rebuild();
    });
    connect(up, &QToolButton::clicked, this, [this] {
        if (m_selectedEffect <= 0) return;
        editEffects(QStringLiteral("Ordre des effets"),
                    [this] { m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect - 1); });
        --m_selectedEffect;
        rebuild();
    });
    connect(down, &QToolButton::clicked, this, [this, count] {
        if (m_selectedEffect >= count - 1) return;
        editEffects(QStringLiteral("Ordre des effets"),
                    [this] { m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect + 1); });
        ++m_selectedEffect;
        rebuild();
    });
    connect(reload, &QToolButton::clicked, this, [this] {
        IsfInstance *fx;
        {
            Engine::Lock lk(&m_engine->mutex());
            fx = cmd::resolveIsf(m_engine, m_layer, m_selectedEffect);
        }
        m_engine->reloadIsf(fx);
        rebuild();
    });

    const auto &fx = s.effects[size_t(m_selectedEffect)];
    v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(fx.name.toHtmlEscaped())));
    if (!fx.error.isEmpty()) v->addWidget(errorLabel(fx.error));
    if (fx.valid) {
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, m_selectedEffect);
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    return g;
}

void LayerInspector::refreshDynamic()
{
    bool playing;
    double d, p;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l || l->type != SourceType::Video) return;
        playing = l->playing;
        d = l->duration();
        p = l->position();
    }
    if (m_play) m_play->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Lecture"));
    if (m_seek && !m_seek->isSliderDown() && d > 0) {
        QSignalBlocker b(m_seek);
        m_seek->setValue(int(std::lround(p / d * 10000)));
    }
    if (m_time) m_time->setText(fmtTime(p) + " / " + fmtTime(d));
}
