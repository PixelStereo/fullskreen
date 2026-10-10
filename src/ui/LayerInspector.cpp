#include "LayerInspector.h"
#include "Commands.h"
#include "Engine.h"
#include "LayerTable.h"
#include "ParamAnimPanel.h"
#include "ParamPanel.h"
#include "SettingsPanel.h"
#include "ViewportOutput.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>
#include <cmath>
#include <memory>

// Copy of a layer's state taken under the lock: widgets are built afterwards without blocking rendering.
struct LayerValues {
    bool valid = false;
    quint64 id = 0;
    bool isGroup = false, isViewport = false, locked = false, lockedByGroup = false;
    int members = 0;
    QSize vpSize;
    // Routing: an item at the top of the list chooses its viewports; inside a group, its top group does
    struct Route {
        quint64 id;
        QString name;
        float opacity; // how much of it the viewport shows
    };
    std::vector<Route> routes;
    QString routedBy; // the top group of an item inside a group
    QRectF roi{0, 0, 1, 1};
    ColorAdjust color;
    bool effectsEnabled = true;
    int colorModels = 1;
    double aspect = 16.0 / 9.0; // source picture (before roi)
    QString name, sourcePath, error;
    bool enabled = true;
    float opacity = 1;
    BlendMode blend = BlendMode::Normal;
    SourceType type = SourceType::None;
    bool hasVideo = false, playing = false;
    PlayMode mode = PlayMode::Loop;
    int videoW = 0, videoH = 0;
    double fps = 0, duration = 0, speed = 1;
    double inPoint = 0, outPoint = -1;
    QString codec;
    bool hasAudio = false, muted = false;
    float volume = 1;
    AudioStream::Info audio;
    bool hasGenerator = false;
    int genW = 0, genH = 0;
    double genSpeed = 1;
    int srcW = 0, srcH = 0; // an image's
    // Another layer as the source, and the layers that could be chosen (id, name; cycles left out)
    quint64 sourceLayer = 0;
    LayerTap sourceTap = LayerTap::PostFx;
    QString transition; // when a snapshot changes the source (empty: the default)
    std::vector<std::pair<quint64, QString>> candidates;
    struct Fx {
        QString name, error;
        bool valid = false, enabled = true, masked = false;
    };
    std::vector<Fx> effects;
    bool meshMode = false;
    int cols = 4, rows = 4;
    TextSource text; // Text generator

    static LayerValues take(Engine *e, int index)
    {
        LayerValues s;
        Engine::Lock lk(&e->mutex());
        Layer *l = e->layer(index);
        if (!l) return s;
        s.valid = true;
        s.id = l->id;
        s.isGroup = l->isGroup;
        s.isViewport = l->isViewport;
        s.vpSize = l->viewportSize();
        s.locked = l->locked;
        s.lockedByGroup = !l->locked && e->isLocked(index);
        s.members = e->groupMembers(index).size();
        s.roi = l->roi;
        s.color = l->color;
        s.effectsEnabled = l->effectsEnabled;
        s.colorModels = l->colorModels;
        const QSize comp = e->compositionSize();
        const QSize own = l->isViewport ? l->viewportSize() : l->isGroup ? comp : QSize(l->sourceWidth(), l->sourceHeight());
        const int sw = own.width(), sh = own.height();
        if (sw > 0 && sh > 0) s.aspect = double(sw) / sh;
        s.name = l->name;
        s.sourcePath = l->sourcePath;
        s.error = l->error;
        s.enabled = l->enabled;
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
        if (l->audio) {
            s.hasAudio = true;
            s.audio = l->audio->info();
            s.duration = l->duration();
        }
        s.volume = l->volume;
        s.muted = l->muted;
        s.playing = l->playing;
        s.mode = l->mode;
        s.speed = l->speed;
        s.inPoint = l->inPoint;
        s.outPoint = l->outPoint;
        s.hasGenerator = l->generator != nullptr;
        s.genW = l->genWidth;
        s.genH = l->genHeight;
        s.genSpeed = l->generator ? l->generator->speed : 1.0;
        s.srcW = l->srcWidth;
        s.srcH = l->srcHeight;
        s.sourceLayer = l->sourceLayer;
        s.sourceTap = l->sourceTap;
        s.transition = l->transition;
        if (!l->isGroup && !l->isViewport)
            for (int k = 0; k < e->layerCount(); ++k) {
                const Layer *o = e->layer(k);
                // Not itself, not a viewport, and not a layer that already depends on this one (the picture would feed back)
                if (!o || o->isViewport || o->id == l->id || e->layerDependsOn(o->id, l->id)) continue;
                s.candidates.push_back({o->id, o->isGroup ? o->name + QStringLiteral(" (group)") : o->name});
            }
        for (const auto &fx : l->effects)
            s.effects.push_back({fx->name(), fx->error(), fx->isValid(), fx->enabled, fx->maskLayer != 0});
        s.meshMode = l->mapping.meshMode;
        s.cols = l->mapping.cols;
        s.rows = l->mapping.rows;
        s.text = l->text;
        if (!l->isViewport) {
            const Layer *top = l;
            while (top->parent) {
                const Layer *g = e->layer(e->indexOfId(top->parent));
                if (!g) break;
                top = g;
            }
            if (top != l) s.routedBy = top->name;
            for (int v : e->viewports())
                if (const Layer *vp = e->layer(v)) s.routes.push_back({vp->id, vp->name, top->opacityIn(vp->id)});
        }
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

// Thin rule between the blocks of a panel
static QWidget *separator()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("color:#3a3a3f;");
    line->setFixedHeight(1);
    return line;
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

namespace {
// Drop zone of the source (top of the inspector): a file from the Media Bin or the Finder is loaded into the layer.
class DropZone : public QLabel
{
public:
    std::function<void(const QString &)> onDrop; // a file
    std::function<void(int)> onLayer;           // a layer dragged from the list (its row)
    explicit DropZone(const QString &text) : QLabel(text)
    {
        setAcceptDrops(true);
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setMinimumHeight(64);
        setHover(false);
    }
    // Loaded: it stands out (a filled, framed card), empty: a dashed outline; missing: in red
    void setLoaded(bool on, bool missing = false)
    {
        m_loaded = on;
        m_missing = missing;
        setHover(false);
    }

protected:
    void setHover(bool on)
    {
        if (on)
            setStyleSheet(QStringLiteral("QLabel { border:2px dashed %1; border-radius:6px; background:%2;"
                                         " color:%3; padding:8px; }")
                              .arg(theme::css(), theme::css(40), theme::accent().lighter(130).name()));
        else if (m_missing)
            setStyleSheet(QStringLiteral("QLabel { border:2px solid #b4483c; border-radius:6px; background:#3a2422;"
                                         " color:#ffb4a8; padding:8px; }"));
        else if (m_loaded)
            setStyleSheet(QStringLiteral("QLabel { border:2px solid %1; border-radius:6px; background:%2;"
                                         " color:#f4f4f6; padding:8px; }")
                              .arg(theme::css(), theme::css(55)));
        else
            setStyleSheet(QStringLiteral("QLabel { border:2px dashed #55555c; border-radius:6px; "
                                         "color:#9a9aa0; padding:8px; }"));
    }
    bool m_loaded = false, m_missing = false;
    static int layerRow(const QMimeData *m)
    {
        if (!m->hasFormat(kLayerRowsMime)) return -1;
        bool ok = false;
        const int row = m->data(kLayerRowsMime).split(',').value(0).toInt(&ok);
        return ok ? row : -1;
    }
    static QString firstFile(const QMimeData *m)
    {
        for (const QUrl &u : m->urls())
            if (u.isLocalFile()) return u.toLocalFile();
        return {};
    }
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (firstFile(e->mimeData()).isEmpty() && (layerRow(e->mimeData()) < 0 || !onLayer)) return e->ignore();
        setHover(true);
        e->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { setHover(false); }
    void dropEvent(QDropEvent *e) override
    {
        setHover(false);
        const QString f = firstFile(e->mimeData());
        const int row = layerRow(e->mimeData());
        e->acceptProposedAction();
        if (!f.isEmpty() && onDrop) onDrop(f);
        else if (row >= 0 && onLayer) onLayer(row);
    }
};

QWidget *page(QWidget *content)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 6, 0, 0);
    v->addWidget(content);
    v->addStretch();
    return w;
}
} // namespace

LayerInspector::LayerInspector(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(8, 8, 8, 8);
    // A number animated or no longer: the tabs again (the ∿ by its name); an edit of an animation: its card follows
    connect(engine, &Engine::layerAnimsChanged, this, [this](quint64 layer) {
        if (layer != m_layerId) return;
        QStringList now;
        for (const Animation &a : m_engine->layerAnims(layer))
            if (!a.tracks.empty()) now << a.tracks.front().param;
        if (now != m_animated) QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
    });
    rebuild();
}

std::pair<double, double> LayerInspector::limitsOf(const QString &path, double lo, double hi) const
{
    ParamInfo p;
    if (!m_engine->parameterInfo(m_layerId, path, &p)) return {lo, hi};
    return {p.lo, p.hi};
}

void LayerInspector::showAnimation(const QString &param)
{
    QStringList now;
    for (const Animation &a : m_engine->layerAnims(m_layerId))
        if (!a.tracks.empty()) now << a.tracks.front().param;
    if (now != m_animated) { // just made: shown once the tabs are built again
        m_revealAnim = param;
        return;
    }
    if (m_tabs && m_animTab >= 0) m_tabs->setCurrentIndex(m_animTab);
    if (m_anims) m_anims->reveal(param);
}

void LayerInspector::setLayer(int index)
{
    if (index != m_layer) m_selectedEffect = 0;
    m_layer = index;
    rebuild();
}

// --- Undoable edits ----------------------------------------------------------

void LayerInspector::setProp(int prop, const QVariant &value)
{
    const auto p = cmd::SetLayerProp::Prop(prop);
    if (p != cmd::SetLayerProp::Enabled && p != cmd::SetLayerProp::Locked && m_engine->isLocked(m_layer)) return;
    const QVariant before = cmd::SetLayerProp::read(m_engine, m_layer, p);
    if (!before.isValid() || before == value) return;
    m_undo->push(new cmd::SetLayerProp(m_engine, m_layer, p, before, value));
}

void LayerInspector::editMapping(const QString &text, const std::function<void(Mapping &)> &fn, bool merge)
{
    if (m_engine->isLocked(m_layer)) return;
    const Mapping before = cmd::SetMapping::read(m_engine, m_layer);
    Mapping after = before;
    fn(after);
    // merge: successive changes of the same field (spin box arrows, typing) form one undo step
    m_undo->push(new cmd::SetMapping(m_engine, m_layer, before, after, text, merge));
    emit mappingChanged();
}

void LayerInspector::editEffects(const QString &text, const std::function<void()> &op)
{
    if (m_engine->isLocked(m_layer)) {
        QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection); // undo the click on a checkbox
        return;
    }
    const QJsonArray before = m_engine->effectsJson(m_layer);
    op();
    m_undo->push(new cmd::SetEffects(m_engine, m_layer, before, text));
}

void LayerInspector::editSource(const QString &text, const std::function<void()> &op)
{
    if (m_engine->isLocked(m_layer)) return;
    const QJsonObject before = m_engine->layerJson(m_layer);
    op();
    m_undo->push(new cmd::ReplaceLayer(m_engine, m_layer, before, text));
}

// --- Building ----------------------------------------------------------------

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

    const LayerValues s = LayerValues::take(m_engine, m_layer);
    if (!s.valid) {
        emit kindChanged(QStringLiteral("Layer"));
        auto *empty = new QLabel(QStringLiteral("No layer selected.\n\nCreate a layer with the + button,\n"
                                                "then drop a video, image, sound\nor ISF generator onto it."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet("color:#888;");
        v->addWidget(empty);
        v->addStretch();
        return;
    }

    m_layerId = s.id;
    m_locked = s.locked || s.lockedByGroup;
    m_animated.clear();
    for (const Animation &a : m_engine->layerAnims(s.id))
        if (!a.tracks.empty()) m_animated << a.tracks.front().param;
    m_animate = new AnimateMenu(m_engine, m_undo, s.id, [this](const QString &p) { showAnimation(p); }, m_content);
    const QString kind = s.isViewport ? QStringLiteral("Viewport") : s.isGroup ? QStringLiteral("Group") : QStringLiteral("Layer");
    emit kindChanged(kind);
    // Header: visibility, lock, name
    auto *head = new QHBoxLayout;
    auto *name = new QLineEdit(s.name);
    name->setStyleSheet("font-weight:bold; font-size:14px;");
    name->setToolTip(kind + QStringLiteral(" name"));
    auto *vis = new FlagBox(QStringLiteral("Enable"));
    vis->setChecked(s.enabled);
    vis->setProperty("allowLocked", true);
    auto *lock = new QToolButton;
    lock->setCheckable(true);
    lock->setChecked(s.locked);
    lock->setIcon(padlockIcon(s.locked, s.lockedByGroup));
    lock->setIconSize(QSize(18, 18));
    lock->setToolTip(s.lockedByGroup ? QStringLiteral("Locked by its group") : QStringLiteral("Lock: no edit allowed on this layer (Ctrl+L)"));
    lock->setStyleSheet("QToolButton:checked { background:#4a2422; }");
    lock->setProperty("allowLocked", true);
    lock->setEnabled(!s.lockedByGroup);
    head->addWidget(vis);
    head->addWidget(lock);
    head->addWidget(name, 1);
    v->addLayout(head);
    connect(lock, &QToolButton::toggled, this, [this](bool on) {
        setProp(cmd::SetLayerProp::Locked, on);
        emit layerChanged();
        QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
    });
    if (m_locked) {
        auto *banner = new QLabel(s.lockedByGroup ? QStringLiteral("Locked by its group — unlock the group to edit")
                                                  : QStringLiteral("Locked — unlock (padlock) to edit; the transport stays available"));
        banner->setStyleSheet("background:#3a2422; color:#ffb4a8; padding:4px 8px; border-radius:4px;");
        v->addWidget(banner);
    }
    connect(name, &QLineEdit::textEdited, this, [this](const QString &t) {
        setProp(cmd::SetLayerProp::Name, t);
        emit layerChanged();
    });
    connect(vis, &QCheckBox::toggled, this, [this](bool on) {
        setProp(cmd::SetLayerProp::Enabled, on);
        emit layerChanged();
    });

    // At the top, always shown: its source (media, transport, sound, generator) and how it is composited (opacity,
    // blend, the viewports it appears in)
    auto *top = new QWidget;
    {
        auto *tv = new QVBoxLayout(top);
        tv->setContentsMargins(0, 0, 0, 0);
        tv->setSpacing(8);
        tv->addWidget(buildSource(s));
        if (s.type != SourceType::Audio) { // a sound has no picture to composite
            tv->addWidget(separator());
            tv->addWidget(buildCompositing(s));
        }
        lockInputs(top, m_locked);
    }
    v->addWidget(top);

    // Below, in sub-tabs; the current one is kept from one layer to the next
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    // Tabs that read as tabs (as the ones above): the current one lighter, underlined with the accent
    tabs->setStyleSheet(QStringLiteral("QTabBar::tab { background:#26262a; color:#a8a8ae; padding:5px 7px; margin-right:1px;"
                                       " border:1px solid #3a3a40; border-bottom:none;"
                                       " border-top-left-radius:4px; border-top-right-radius:4px; }"
                                       "QTabBar::tab:selected { background:#45454c; color:#f4f4f6; border-color:#55555c;"
                                       " border-bottom:2px solid %1; }"
                                       "QTabBar::tab:hover:!selected { background:#313136; color:#d8d8dc; }"
                                       "QTabBar::tab:disabled { color:#55555a; }")
                            .arg(theme::css()));
    const bool hasSource = !s.isGroup && !s.isViewport;
    if (hasSource) tabs->addTab(page(buildSourceTab(s)), QStringLiteral("Source"));
    tabs->addTab(page(buildRoi(s)), QStringLiteral("ROI"));
    tabs->addTab(page(buildColor(s)), QStringLiteral("Color"));
    tabs->addTab(page(buildMapping(s)), QStringLiteral("Spatial"));
    tabs->addTab(page(buildEffects(s)), QStringLiteral("FX"));
    if (!s.isViewport) tabs->addTab(page(buildViewports(s)), QStringLiteral("Viewports"));
    m_anims = new ParamAnimPanel(m_engine, m_undo, s.id);
    connect(m_anims, &ParamAnimPanel::projectEdited, this, &LayerInspector::projectEdited);
    {
        auto *w = new QWidget; // no stretch below: the cards that are not pinned scroll in the room left
        auto *av = new QVBoxLayout(w);
        av->setContentsMargins(0, 6, 0, 0);
        av->addWidget(m_anims, 1);
        m_animTab = tabs->addTab(w, m_animated.isEmpty() ? QStringLiteral("Anim") : QStringLiteral("Anim (%1)").arg(m_animated.size()));
        tabs->setTabToolTip(m_animTab, QStringLiteral("The animations of this layer's numbers (right-click a parameter › Animate)"));
    }
    m_output = nullptr;
    if (s.isViewport) m_routeFields.clear();
    if (s.isViewport) { // its size, screen and publishing
        m_output = new ViewportOutputPanel(m_engine, s.id);
        connect(m_output, &ViewportOutputPanel::edited, this, &LayerInspector::projectEdited);
        connect(m_output, &ViewportOutputPanel::edited, this, &LayerInspector::layerChanged);
        tabs->addTab(page(m_output), QStringLiteral("Output"));
    }
    lockInputs(tabs, m_locked);
    name->setEnabled(!m_locked);
    if (s.type == SourceType::Audio) { // a sound has no picture: no ROI, color, mapping, effects or routing
        for (int t = 0; t < tabs->count(); ++t) {
            if (t == m_animTab || tabs->tabText(t) == QLatin1String("Source")) continue; // its transport, volume, speed
            tabs->setTabEnabled(t, false);
            tabs->setTabToolTip(t, QStringLiteral("An audio layer has no picture"));
        }
    }
    m_tabs = tabs;
    // The tab shown last, by its name (the tabs differ from one kind of layer to the next)
    int current = -1;
    for (int t = 0; t < tabs->count(); ++t)
        if (tabs->tabText(t).section(' ', 0, 0) == m_subTab && tabs->isTabEnabled(t)) current = t;
    if (!m_revealAnim.isEmpty() || current < 0) current = m_animTab;
    if (current == m_animTab && m_revealAnim.isEmpty() && m_subTab != QLatin1String("Anim"))
        for (int t = 0; t < tabs->count(); ++t) // its first tab that can be used
            if (tabs->isTabEnabled(t)) {
                current = t;
                break;
            }
    tabs->setCurrentIndex(current);
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int i) {
        if (tabs->isTabEnabled(i)) m_subTab = tabs->tabText(i).section(' ', 0, 0);
    });
    v->addWidget(tabs, 1);
    if (!m_revealAnim.isEmpty()) {
        m_anims->reveal(m_revealAnim);
        m_revealAnim.clear();
    }
    refreshDynamic();
}

// The picture of the layer at `row` as this one's source (dropped from the list onto the source)
void LayerInspector::useLayer(int row, const std::vector<std::pair<quint64, QString>> &candidates)
{
    const quint64 id = m_engine->layerId(row);
    if (!id || row == m_layer) return;
    const bool allowed = std::any_of(candidates.begin(), candidates.end(), [id](const auto &c) { return c.first == id; });
    if (!allowed) {
        QMessageBox::warning(this, QStringLiteral("Source"),
                             QStringLiteral("This layer cannot be used here: its picture would feed back on itself."));
        return;
    }
    QString err;
    bool ok = false;
    editSource(QStringLiteral("Use Layer as Source"),
               [this, id, &err, &ok] { ok = m_engine->setLayerSourceLayer(m_layer, id, LayerTap::PostFx, &err); });
    if (!ok && !err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Source"), err);
    emit layerChanged();
    rebuild();
}

QWidget *LayerInspector::buildSource(const LayerValues &s)
{
    auto *g = new QWidget;
    auto *v = new QVBoxLayout(g);
    v->setContentsMargins(0, 0, 0, 0);

    if (s.isViewport) {
        auto *info = new QLabel(QStringLiteral("<b>Viewport</b> %1 × %2<br><span style='font-size:11px; color:#999'>"
                                               "A window onto the composition: it shows the region set in Spatial, "
                                               "at its own size (Output). The layers at the top of the list choose "
                                               "the viewports they appear in (below their opacity).</span>")
                                    .arg(s.vpSize.width())
                                    .arg(s.vpSize.height()));
        info->setWordWrap(true);
        v->addWidget(info);
        return g;
    }
    if (s.isGroup) {
        auto *info = new QLabel(QStringLiteral("<b>Group</b> of %1 layer(s)<br><span style='font-size:11px; color:#999'>"
                                               "Its picture is the composite of its layers. Drag layers onto it in the "
                                               "layer list to add them.</span>")
                                    .arg(s.members));
        info->setWordWrap(true);
        v->addWidget(info);
        return g;
    }

    // What feeds it, in the drop zone: its kind, its name and what it is (resolution, rate, length, codec), or that it is
    // empty. A file dropped there (Media Bin, Finder) replaces it; a layer dragged from the list becomes its source.
    QString kind, name, details;
    auto audioText = [](const AudioStream::Info &a) {
        const QString ch = a.channels == 1 ? QStringLiteral("mono") : a.channels == 2 ? QStringLiteral("stereo")
                                                                                        : QStringLiteral("%1 ch").arg(a.channels);
        return QStringLiteral("%1 · %2 kHz · %3").arg(a.codec).arg(a.sampleRate / 1000.0, 0, 'g', 3).arg(ch);
    };
    QStringList d;
    switch (s.type) {
    case SourceType::Video:
        kind = QStringLiteral("Video");
        name = QFileInfo(s.sourcePath).fileName();
        d << QStringLiteral("%1 × %2").arg(s.videoW).arg(s.videoH) << QStringLiteral("%1 fps").arg(s.fps, 0, 'f', 2)
          << fmtTime(s.duration) << s.codec << (s.hasAudio ? QStringLiteral("sound: ") + audioText(s.audio) : QStringLiteral("no sound"));
        break;
    case SourceType::Audio:
        kind = QStringLiteral("Sound");
        name = QFileInfo(s.sourcePath).fileName();
        d << fmtTime(s.duration) << audioText(s.audio);
        break;
    case SourceType::Image:
        kind = QStringLiteral("Image");
        name = QFileInfo(s.sourcePath).fileName();
        d << QStringLiteral("%1 × %2").arg(s.srcW).arg(s.srcH);
        break;
    case SourceType::Isf:
        kind = QStringLiteral("ISF Generator");
        name = QFileInfo(s.sourcePath).completeBaseName();
        d << QStringLiteral("%1 × %2").arg(s.genW).arg(s.genH);
        break;
    case SourceType::Text:
        kind = QStringLiteral("Text");
        name = s.text.content.section('\n', 0, 0).left(40);
        d << QStringLiteral("%1 × %2").arg(s.text.width).arg(s.text.height);
        break;
    case SourceType::Layer: {
        kind = QStringLiteral("Layer");
        name = QStringLiteral("(gone)");
        for (const auto &c : s.candidates)
            if (c.first == s.sourceLayer) name = c.second;
        d << (s.sourceTap == LayerTap::PreFx ? QStringLiteral("before its FX") : QStringLiteral("after its FX"));
        break;
    }
    default:
        if (!s.sourcePath.isEmpty()) { // its file is missing
            kind = QStringLiteral("Missing");
            name = QFileInfo(s.sourcePath).fileName();
        }
        break;
    }
    d.removeAll(QString());
    details = d.join(QStringLiteral(" · "));
    const bool loaded = !kind.isEmpty();
    auto *zoneRow = new QHBoxLayout;
    auto *zone = new DropZone(loaded ? QStringLiteral("<span style='font-size:11px; letter-spacing:1px'>%1</span><br>"
                                                      "<b style='font-size:14px'>%2</b><br>"
                                                      "<span style='font-size:11px'>%3</span>")
                                           .arg(kind.toUpper().toHtmlEscaped(), name.toHtmlEscaped(), details.toHtmlEscaped())
                                     : QStringLiteral("<b>Empty layer</b><br><span style='font-size:11px'>Drop a video, image, "
                                                      "sound or ISF generator here (Media Bin, Finder), or a layer from the list</span>"));
    zone->setTextFormat(Qt::RichText);
    zone->setLoaded(loaded, s.type == SourceType::None && loaded); // a missing file: in red
    zone->setToolTip(s.sourcePath.isEmpty() ? QStringLiteral("Drop a file, or a layer from the list (its picture becomes this one's)")
                                            : s.sourcePath + QStringLiteral("\nDrop another media or a layer to replace it"));
    zone->onDrop = [this](const QString &f) { QTimer::singleShot(0, this, [this, f] { emit fileDropped(f); }); };
    zone->onLayer = [this, candidates = s.candidates](int row) {
        // After the drag is over (the zone is rebuilt with the inspector)
        QTimer::singleShot(0, this, [this, candidates, row] { useLayer(row, candidates); });
    };
    auto *bClear = toolButton(QStringLiteral("×"), QStringLiteral("Eject the source from the layer"));
    bClear->setEnabled(loaded || s.error.size());
    zoneRow->addWidget(zone, 1);
    zoneRow->addWidget(bClear, 0, Qt::AlignTop);
    v->addLayout(zoneRow);
    connect(bClear, &QToolButton::clicked, this, [this] {
        editSource(QStringLiteral("Eject Media"), [this] { m_engine->clearLayerSource(m_layer); });
        emit layerChanged();
        rebuild();
    });
    if (!s.error.isEmpty()) v->addWidget(errorLabel(s.error));

    // Transition when a snapshot gives this layer another source
    {
        auto *row = new QHBoxLayout;
        auto *label = new ResetLabel(QStringLiteral("Transition"), [this] {
            setProp(cmd::SetLayerProp::Transition, QString());
            rebuild();
        });
        auto *pick = new QComboBox;
        const QString def = m_engine->defaultTransition();
        pick->addItem(QStringLiteral("Default (%1)").arg(def.isEmpty() ? QStringLiteral("Crossfade")
                                                                      : QFileInfo(def).completeBaseName()),
                      QString());
        for (const IsfEntry &t : m_engine->library().transitions()) {
            pick->addItem(t.name, t.path);
            pick->setItemData(pick->count() - 1, t.description, Qt::ToolTipRole);
        }
        int k = pick->findData(s.transition);
        if (k < 0 && !s.transition.isEmpty()) { // not in the library (any more): kept, shown by its name
            pick->addItem(QFileInfo(s.transition).completeBaseName(), s.transition);
            k = pick->count() - 1;
        }
        pick->setCurrentIndex(std::max(0, k));
        pick->setToolTip(QStringLiteral("When a snapshot gives this layer another source, the outgoing one keeps playing and "
                                        "this ISF transition takes it to the new one, over the snapshot's fade "
                                        "(or the time the snapshot gives the source)"));
        row->addWidget(label);
        row->addWidget(pick, 1);
        v->addLayout(row);
        connect(pick, &QComboBox::activated, this,
                [this, pick](int i) { setProp(cmd::SetLayerProp::Transition, pick->itemData(i).toString()); });
    }


    // Its speed: the media's (below 0 backwards), or the pace of the generator's time; a click on its name puts it back
    if ((s.type == SourceType::Video && s.hasVideo) || (s.type == SourceType::Audio && s.hasAudio)) {
        auto *row = new QHBoxLayout;
        m_speed = new SliderField;
        m_speed->setRange(-200, 200);      // the bar: the speeds actually used
        m_speed->setTypedRange(-800, 800); // faster or more backwards: typed
        m_speed->setDecimals(0);
        m_speed->setSuffix(QStringLiteral(" %"));
        m_speed->setSingleStep(5);
        m_speed->setOrigin(0); // the fill grows either side of a standstill
        m_speed->setTicks(8);
        m_speed->setSnaps({-200, -100, 0, 100, 200});
        m_speed->setValue(s.speed * 100);
        m_speed->setToolTip(QStringLiteral("Playback speed — below 0 the media plays backwards, 100 % is its own rate"));
        auto *label = new ResetLabel(QStringLiteral("Speed"), [this] {
            setProp(cmd::SetLayerProp::Speed, 1.0);
            m_speed->setValue(100);
        });
        m_animate->attach({label, m_speed}, {QStringLiteral("speed")});
        row->addWidget(label);
        row->addWidget(m_speed, 1);
        v->addLayout(row);
        connect(m_speed, &SliderField::valueEdited, this, [this](double pct) {
            setProp(cmd::SetLayerProp::Speed, pct / 100.0);
            emit layerChanged();
        });
    } else if (s.type == SourceType::Isf && s.hasGenerator) {
        auto *row = new QHBoxLayout;
        m_genSpeed = new SliderField;
        m_genSpeed->setRange(0, 10);
        m_genSpeed->setDecimals(2);
        m_genSpeed->setSingleStep(0.05);
        m_genSpeed->setTicks(10);
        m_genSpeed->setSnaps({1});
        m_genSpeed->setValue(s.genSpeed);
        m_genSpeed->setToolTip(QStringLiteral("Speed of the shader's time (1 = normal, 0 = stopped)"));
        auto setSpeed = [this](double v) {
            double before = 1.0;
            {
                Engine::Lock lk(&m_engine->mutex());
                IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, -1);
                if (!inst) return;
                before = inst->speed;
            }
            if (std::abs(before - v) > 1e-9) m_undo->push(new cmd::SetIsfSpeed(m_engine, m_layer, -1, before, v));
        };
        auto *label = new ResetLabel(QStringLiteral("Speed"), [this, setSpeed] {
            m_genSpeed->setValue(1);
            setSpeed(1);
        });
        m_animate->attach({label, m_genSpeed}, {QStringLiteral("speed")});
        row->addWidget(label);
        row->addWidget(m_genSpeed, 1);
        v->addLayout(row);
        connect(m_genSpeed, &SliderField::valueEdited, this, setSpeed);
    }
    return g;
}

// The Source tab: what the source is made of and how it plays — the media's transport and sound, the generator's
// parameters and resolution, the text and its style, the tap of a layer used as the source
QWidget *LayerInspector::buildSourceTab(const LayerValues &s)
{
    auto *g = new QWidget;
    auto *v = new QVBoxLayout(g);
    v->setContentsMargins(0, 0, 0, 0);

    if (s.type == SourceType::Layer) {
        auto *row = new QHBoxLayout;
        auto *tap = new QComboBox;
        tap->addItem(QStringLiteral("Pre-FX"), int(LayerTap::PreFx));
        tap->addItem(QStringLiteral("Post-FX"), int(LayerTap::PostFx));
        tap->setCurrentIndex(s.sourceTap == LayerTap::PreFx ? 0 : 1);
        tap->setToolTip(QStringLiteral("Where the picture is taken in that layer:\n"
                                       "Pre-FX — after its ROI and color, before its FX\n"
                                       "Post-FX — after its FX"));
        auto setTap = [this](LayerTap t) {
            editSource(QStringLiteral("Change Source Tap"), [this, t] { m_engine->setLayerTap(m_layer, t); });
            emit layerChanged();
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        };
        row->addWidget(new ResetLabel(QStringLiteral("Tap"), [setTap] { setTap(LayerTap::PostFx); }));
        row->addWidget(tap, 1);
        v->addLayout(row);
        connect(tap, &QComboBox::activated, this, [tap, setTap](int i) { setTap(LayerTap(tap->itemData(i).toInt())); });
        auto *hint = new QLabel(QStringLiteral("To use another layer, drag it from the list onto the source above, or "
                                               "right-click it › Load into Layer."));
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#888; font-size:11px;");
        v->addWidget(hint);
        return g;
    }

    // Text generator: the text typed here, and how it is set
    if (s.type == SourceType::Text) {
        // Edits apply at once; the undo step is pushed when the editing pauses
        struct Pending { QJsonObject before; QString label; bool has = false; };
        auto pend = std::make_shared<Pending>();
        auto *commit = new QTimer(g);
        commit->setSingleShot(true);
        commit->setInterval(700);
        connect(commit, &QTimer::timeout, this, [this, pend] {
            if (!pend->has) return;
            pend->has = false;
            m_undo->push(new cmd::ReplaceLayer(m_engine, m_layer, pend->before, pend->label));
        });
        auto live = [this, pend, commit](const QString &label, const std::function<void()> &op) {
            if (m_engine->isLocked(m_layer)) return;
            if (!pend->has) {
                pend->has = true;
                pend->before = m_engine->layerJson(m_layer);
            }
            pend->label = label;
            op();
            commit->start();
        };
        auto style = [this, live](const QString &label, std::function<void(Layer &)> fn) {
            live(label, [this, fn] { m_engine->editLayerText(m_layer, fn); });
        };
        using ColorSet = std::function<void(Layer &, const QColor &)>;
        auto paint = [](QPushButton *b, const QColor &x) {
            b->setProperty("color", x);
            b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #666; border-radius: 2px;").arg(x.name()));
        };
        // A color of the text set from its button (picked, or put back by the name of its row)
        auto applyColor = [style, paint](QPushButton *b, const QColor &x, const QString &label, ColorSet set) {
            paint(b, x);
            style(label, [set, x](Layer &l) { set(l, x); });
        };
        auto colorButton = [this, paint, applyColor](const QColor &c, const QString &label, ColorSet set) {
            auto *b = new QPushButton;
            b->setFixedWidth(48);
            b->setEnabled(!m_locked);
            paint(b, c);
            connect(b, &QPushButton::clicked, this, [this, b, label, set, applyColor] {
                const QColor picked = QColorDialog::getColor(b->property("color").value<QColor>(), this, label,
                                                             QColorDialog::ShowAlphaChannel);
                if (picked.isValid()) applyColor(b, picked, label, set);
            });
            return b;
        };
        const TextSource def; // what the names of the rows put back
        auto spin = [this](double v, double lo, double hi, double step, int dec, const QString &suffix) {
            auto *x = new NumberBox;
            x->setRange(lo, hi);
            x->setSingleStep(step);
            x->setDecimals(dec);
            x->setSuffix(suffix);
            x->setValue(v);
            x->setEnabled(!m_locked);
            return x;
        };

        auto *editor = new QPlainTextEdit;
        editor->setPlainText(s.text.content);
        editor->setMinimumHeight(90);
        editor->setPlaceholderText(QStringLiteral("Type the text here"));
        editor->setToolTip(QStringLiteral("The text of this layer. A snapshot that holds another text types it (typewriter) over its fade."));
        editor->setReadOnly(m_locked);
        v->addWidget(editor);
        connect(editor, &QPlainTextEdit::textChanged, this, [this, editor, live] {
            live(QStringLiteral("Edit Text"), [this, editor] { m_engine->setLayerTextContent(m_layer, editor->toPlainText()); });
        });

        auto *fmt = new QFormLayout;
        fmt->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *fontBox = new QFontComboBox;
        fontBox->setCurrentFont(QFont(s.text.font));
        fontBox->setEnabled(!m_locked);
        fmt->addRow(new ResetLabel(QStringLiteral("Font"), [fontBox, def] { fontBox->setCurrentFont(QFont(def.font)); }), fontBox);
        connect(fontBox, &QFontComboBox::currentFontChanged, this, [style](const QFont &f) {
            const QString family = f.family();
            style(QStringLiteral("Text Font"), [family](Layer &l) { l.text.font = family; });
        });

        auto *size = new IntBox;
        {
            const auto [lo, hi] = limitsOf(QStringLiteral("text/size"), 1, 1000);
            size->setRange(int(lo), int(hi));
        }
        size->setSuffix(QStringLiteral(" px"));
        size->setValue(s.text.size);
        size->setEnabled(!m_locked);
        auto *colorRow = new QHBoxLayout;
        colorRow->addWidget(size);
        const ColorSet setTextColor = [](Layer &l, const QColor &c) { l.text.color = c; };
        QPushButton *textColor = colorButton(s.text.color, QStringLiteral("Text Color"), setTextColor);
        colorRow->addWidget(textColor);
        colorRow->addStretch();
        fmt->addRow(new ResetLabel(QStringLiteral("Size / color"), [size, textColor, def, applyColor, setTextColor] {
                        size->setValue(def.size);
                        applyColor(textColor, def.color, QStringLiteral("Text Color"), setTextColor);
                    }),
                    colorRow);
        connect(size, QOverload<int>::of(&QSpinBox::valueChanged), this, [style](int px) {
            style(QStringLiteral("Text Size"), [px](Layer &l) { l.text.size = px; });
        });

        // Bold, italic, underline, strikethrough
        auto *styleRow = new QHBoxLayout;
        QList<ToggleButton *> styleButtons;
        struct Toggle { const char *label, *tip; bool on; bool TextSource::*field; };
        const Toggle toggles[] = {{"B", "Bold", s.text.bold, &TextSource::bold},
                                  {"I", "Italic", s.text.italic, &TextSource::italic},
                                  {"U", "Underline", s.text.underline, &TextSource::underline},
                                  {"S", "Strikethrough", s.text.strike, &TextSource::strike}};
        for (const Toggle &t : toggles) {
            auto *b = new ToggleButton(QString::fromLatin1(t.label));
            b->setToolTip(QString::fromLatin1(t.tip));
            b->setChecked(t.on);
            b->setEnabled(!m_locked);
            QFont f = b->font();
            f.setBold(t.label[0] == 'B');
            f.setItalic(t.label[0] == 'I');
            f.setUnderline(t.label[0] == 'U');
            f.setStrikeOut(t.label[0] == 'S');
            b->setFont(f);
            styleRow->addWidget(b);
            styleButtons << b;
            auto field = t.field;
            connect(b, &QToolButton::toggled, this, [style, field, tip = QString::fromLatin1(t.tip)](bool on) {
                style(tip, [field, on](Layer &l) { l.text.*field = on; });
            });
        }
        styleRow->addStretch();
        fmt->addRow(new ResetLabel(QStringLiteral("Style"), [styleButtons] { // plain
                        for (ToggleButton *b : styleButtons) b->setChecked(false);
                    }),
                    styleRow);

        // Alignment: left, center, right, justified; top, middle, bottom
        auto *hAlign = new QComboBox;
        hAlign->addItem(QStringLiteral("Left"), int(Qt::AlignLeft));
        hAlign->addItem(QStringLiteral("Center"), int(Qt::AlignHCenter));
        hAlign->addItem(QStringLiteral("Right"), int(Qt::AlignRight));
        hAlign->addItem(QStringLiteral("Justified"), int(Qt::AlignJustify));
        auto *vAlign = new QComboBox;
        vAlign->addItem(QStringLiteral("Top"), int(Qt::AlignTop));
        vAlign->addItem(QStringLiteral("Middle"), int(Qt::AlignVCenter));
        vAlign->addItem(QStringLiteral("Bottom"), int(Qt::AlignBottom));
        const int h0 = hAlign->findData(int(s.text.align & Qt::AlignHorizontal_Mask));
        const int v0 = vAlign->findData(int(s.text.align & Qt::AlignVertical_Mask));
        hAlign->setCurrentIndex(h0 >= 0 ? h0 : 0);
        vAlign->setCurrentIndex(v0 >= 0 ? v0 : 0);
        hAlign->setEnabled(!m_locked);
        vAlign->setEnabled(!m_locked);
        auto *alignRow = new QHBoxLayout;
        alignRow->addWidget(hAlign);
        alignRow->addWidget(vAlign);
        auto setAlign = [style, hAlign, vAlign] {
            const int a = hAlign->currentData().toInt() | vAlign->currentData().toInt();
            style(QStringLiteral("Text Alignment"), [a](Layer &l) { l.text.align = Qt::Alignment(a); });
        };
        fmt->addRow(new ResetLabel(QStringLiteral("Align"), [hAlign, vAlign, def, setAlign] {
                        hAlign->setCurrentIndex(std::max(0, hAlign->findData(int(def.align & Qt::AlignHorizontal_Mask))));
                        vAlign->setCurrentIndex(std::max(0, vAlign->findData(int(def.align & Qt::AlignVertical_Mask))));
                        setAlign();
                    }),
                    alignRow);
        connect(hAlign, QOverload<int>::of(&QComboBox::activated), this, setAlign);
        connect(vAlign, QOverload<int>::of(&QComboBox::activated), this, setAlign);

        const auto lh = limitsOf(QStringLiteral("text/line_height"), 0.1, 10);
        auto *lineSp = spin(s.text.lineHeight, lh.first, lh.second, 0.05, 2, QStringLiteral(" ×"));
        lineSp->setToolTip(QStringLiteral("Space between the lines (1 = the font's own)"));
        fmt->addRow(new ResetLabel(QStringLiteral("Line spacing"), [lineSp, def] { lineSp->setValue(def.lineHeight); }), lineSp);
        connect(lineSp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Line Spacing"), [x](Layer &l) { l.text.lineHeight = float(x); });
        });
        const auto ls = limitsOf(QStringLiteral("text/letter_spacing"), -200, 500);
        auto *letterSp = spin(s.text.letterSpacing, ls.first, ls.second, 0.5, 1, QStringLiteral(" px"));
        letterSp->setToolTip(QStringLiteral("Space added between the letters"));
        fmt->addRow(new ResetLabel(QStringLiteral("Letter spacing"), [letterSp, def] { letterSp->setValue(def.letterSpacing); }),
                    letterSp);
        connect(letterSp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Letter Spacing"), [x](Layer &l) { l.text.letterSpacing = float(x); });
        });

        // Outline
        const auto ol = limitsOf(QStringLiteral("text/outline/width"), 0, 200);
        auto *outline = spin(s.text.outline, ol.first, ol.second, 0.5, 1, QStringLiteral(" px"));
        auto *outlineRow = new QHBoxLayout;
        outlineRow->addWidget(outline);
        const ColorSet setOutlineColor = [](Layer &l, const QColor &c) { l.text.outlineColor = c; };
        QPushButton *outlineColor = colorButton(s.text.outlineColor, QStringLiteral("Outline Color"), setOutlineColor);
        outlineRow->addWidget(outlineColor);
        outlineRow->addStretch();
        fmt->addRow(new ResetLabel(QStringLiteral("Outline"), [outline, outlineColor, def, applyColor, setOutlineColor] {
                        outline->setValue(def.outline);
                        applyColor(outlineColor, def.outlineColor, QStringLiteral("Outline Color"), setOutlineColor);
                    }),
                    outlineRow);
        connect(outline, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Outline"), [x](Layer &l) { l.text.outline = float(x); });
        });

        for (auto [field, path] : std::initializer_list<std::pair<QWidget *, const char *>>{
                 {size, "text/size"}, {lineSp, "text/line_height"},
                 {letterSp, "text/letter_spacing"}, {outline, "text/outline/width"}})
            m_animate->attach(field, {QString::fromLatin1(path)});
        m_animate->attach(fmt->labelForField(colorRow), {QStringLiteral("text/size")});
        m_animate->attach(fmt->labelForField(lineSp), {QStringLiteral("text/line_height")});
        m_animate->attach(fmt->labelForField(letterSp), {QStringLiteral("text/letter_spacing")});
        m_animate->attach(fmt->labelForField(outlineRow), {QStringLiteral("text/outline/width")});

        // Shadow
        auto *shadow = new FlagBox(QStringLiteral("Enable"));
        shadow->setChecked(s.text.shadow);
        shadow->setEnabled(!m_locked);
        auto *shadowRow = new QHBoxLayout;
        shadowRow->addWidget(shadow);
        const ColorSet setShadowColor = [](Layer &l, const QColor &c) { l.text.shadowColor = c; };
        QPushButton *shadowColor = colorButton(s.text.shadowColor, QStringLiteral("Shadow Color"), setShadowColor);
        shadowRow->addWidget(shadowColor);
        const auto sxl = limitsOf(QStringLiteral("text/shadow/x"), -2000, 2000);
        const auto syl = limitsOf(QStringLiteral("text/shadow/y"), -2000, 2000);
        auto *sx = spin(s.text.shadowX, sxl.first, sxl.second, 1, 0, QStringLiteral(" x"));
        auto *sy = spin(s.text.shadowY, syl.first, syl.second, 1, 0, QStringLiteral(" y"));
        shadowRow->addWidget(sx);
        shadowRow->addWidget(sy);
        fmt->addRow(new ResetLabel(QStringLiteral("Shadow"), [shadow, shadowColor, sx, sy, def, applyColor, setShadowColor] {
                        shadow->setChecked(def.shadow);
                        applyColor(shadowColor, def.shadowColor, QStringLiteral("Shadow Color"), setShadowColor);
                        sx->setValue(def.shadowX);
                        sy->setValue(def.shadowY);
                    }),
                    shadowRow);
        m_animate->attach(sx, {QStringLiteral("text/shadow/x")});
        m_animate->attach(sy, {QStringLiteral("text/shadow/y")});
        m_animate->attach(fmt->labelForField(shadowRow), {QStringLiteral("text/shadow/x"), QStringLiteral("text/shadow/y")});
        connect(shadow, &QCheckBox::toggled, this, [style](bool on) {
            style(QStringLiteral("Text Shadow"), [on](Layer &l) { l.text.shadow = on; });
        });
        connect(sx, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Shadow"), [x](Layer &l) { l.text.shadowX = float(x); });
        });
        connect(sy, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Shadow"), [x](Layer &l) { l.text.shadowY = float(x); });
        });

        v->addLayout(fmt);
        v->addStretch();
        return g;
    }

    const bool media = (s.type == SourceType::Video && s.hasVideo) || (s.type == SourceType::Audio && s.hasAudio);
    if (media) {
        if (s.type == SourceType::Video) { // how the decoded frames reach the GPU: known once they come
            auto *facts = new QFormLayout;
            facts->setContentsMargins(0, 2, 0, 2);
            facts->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
            auto *k = new QLabel(QStringLiteral("Picture"));
            k->setStyleSheet("color:#8a8a8e;");
            m_pictureFact = new QLabel(QStringLiteral("…"));
            m_pictureFact->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_pictureFact->setToolTip(QStringLiteral("How the decoded frames reach the GPU. A pixel layout (yuv420p, nv12, "
                                                     "p010…) or HAP textures are converted by the GPU; \"converted on "
                                                     "the CPU\" or \"decoded on the CPU\" costs processor time."));
            facts->addRow(k, m_pictureFact);
            v->addLayout(facts);
        }
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        auto rowLabel = [&](int row, const QString &text, std::function<void()> reset = {}) {
            QLabel *l = reset ? static_cast<QLabel *>(new ResetLabel(text, std::move(reset))) : new QLabel(text);
            l->setStyleSheet("color:#8a8a8e;");
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
        };
        auto iconButton = [&](const QIcon &icon, const QString &tip, bool checkable) {
            auto *b = new QToolButton;
            b->setIcon(icon);
            b->setIconSize(QSize(18, 18));
            b->setToolTip(tip);
            b->setCheckable(checkable);
            b->setAutoRaise(true);
            b->setStyleSheet(QStringLiteral("QToolButton { padding:3px 7px; border-radius:3px; }"
                                            "QToolButton:checked { background:%1; }"
                                            "QToolButton:hover { background:#3a3a3f; }")
                                 .arg(theme::css()));
            return b;
        };

        // Play: direction and pause, then go to start and step one frame
        rowLabel(0, QStringLiteral("Play"));
        auto *playRow = new QHBoxLayout;
        playRow->setSpacing(2);
        m_playButtons = new QButtonGroup(g);
        const struct { TransportIcon icon; const char *tip; } kPlay[] = {
            {TransportIcon::PlayBack, "Play backwards"},
            {TransportIcon::Pause, "Pause"},
            {TransportIcon::PlayForward, "Play"},
        };
        for (int i = 0; i < 3; ++i) {
            auto *b = iconButton(transportIcon(kPlay[i].icon), QString::fromUtf8(kPlay[i].tip), true);
            b->setProperty("allowLocked", true); // the transport stays available on a locked layer
            m_playButtons->addButton(b, i);
            playRow->addWidget(b);
        }
        m_playButtons->setExclusive(true);
        playRow->addSpacing(10);
        const double frame = 1.0 / std::max(1.0, s.fps > 0 ? s.fps : 25.0);
        const struct { TransportIcon icon; const char *tip; double step; } kStep[] = {
            {TransportIcon::ToStart, "Back to the in point", 0},
            {TransportIcon::StepBack, "One frame back", -1},
            {TransportIcon::StepForward, "One frame on", +1},
        };
        for (const auto &k : kStep) {
            auto *b = iconButton(transportIcon(k.icon), QString::fromUtf8(k.tip), false);
            b->setProperty("allowLocked", true);
            playRow->addWidget(b);
            const double step = k.step;
            connect(b, &QToolButton::clicked, this, [this, step, frame] {
                double to = 0;
                {
                    Engine::Lock lk(&m_engine->mutex());
                    Layer *l = m_engine->layer(m_layer);
                    if (!l) return;
                    to = step == 0 ? l->inPoint : l->position() + step * frame;
                }
                m_engine->seekLayer(m_layer, to);
                refreshDynamic();
            });
        }
        playRow->addStretch();
        grid->addLayout(playRow, 0, 1);

        // Mode: what happens at the end, then the in / out points at the playhead
        auto *modes = new QButtonGroup(g);
        rowLabel(1, QStringLiteral("Mode"), [this, modes] { // the one new media get (Settings)
            const int def = int(m_engine->defaultPlayMode());
            if (QAbstractButton *b = modes->button(def)) b->setChecked(true);
            setProp(cmd::SetLayerProp::Mode, def);
            emit layerChanged();
        });
        auto *modeRow = new QHBoxLayout;
        modeRow->setSpacing(2);
        modes->setExclusive(true);
        const struct { PlayMode mode; const char *tip; } kModes[] = {
            {PlayMode::Loop, "Loop: starts again from the beginning"},
            {PlayMode::OneShot, "One-shot: plays once and freezes on the last frame"},
            {PlayMode::PingPong, "Ping-pong: forwards, then backwards, and so on"},
            {PlayMode::Stop, "Stop: plays once, then goes black (and silent)"},
        };
        for (const auto &m : kModes) {
            auto *b = iconButton(playModeIcon(int(m.mode)), QString::fromUtf8(m.tip), true);
            b->setChecked(s.mode == m.mode);
            modes->addButton(b, int(m.mode));
            modeRow->addWidget(b);
        }
        modeRow->addSpacing(10);
        auto *markIn = iconButton(transportIcon(TransportIcon::MarkIn),
                                  QStringLiteral("In point at the playhead (I)"), false);
        auto *markOut = iconButton(transportIcon(TransportIcon::MarkOut),
                                   QStringLiteral("Out point at the playhead (O)"), false);
        modeRow->addWidget(markIn);
        modeRow->addWidget(markOut);
        modeRow->addStretch();
        grid->addLayout(modeRow, 1, 1);
        connect(modes, &QButtonGroup::idClicked, this, [this](int id) {
            setProp(cmd::SetLayerProp::Mode, id);
            emit layerChanged();
        });
        connect(markIn, &QToolButton::clicked, this, [this] { emit setInOutRequested(true); });
        connect(markOut, &QToolButton::clicked, this, [this] { emit setInOutRequested(false); });
        v->addLayout(grid);
        v->addWidget(separator());

        // Position and played range: one bar each, dragged or typed
        auto *bars = new QGridLayout;
        bars->setHorizontalSpacing(8);
        bars->setVerticalSpacing(4);
        bars->setColumnStretch(1, 1);
        auto barLabel = [&](int row, const QString &text, std::function<void()> reset) {
            auto *l = new ResetLabel(text, std::move(reset));
            l->setStyleSheet("color:#8a8a8e;");
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            bars->addWidget(l, row, 0);
            return l;
        };
        m_position = new SliderField;
        m_position->setRange(0, std::max(0.01, s.duration));
        m_position->setDecimals(2);
        m_position->setSuffix(QStringLiteral(" s"));
        m_position->setSingleStep(frame);
        m_position->setTicks(10);
        m_position->setSnaps({0});
        m_position->setProperty("allowLocked", true); // seeking is not an edit
        m_position->setToolTip(QStringLiteral("Playhead — drag the bar, type the time, or use the wheel"));
        barLabel(0, QStringLiteral("Position"), [this] { m_engine->seekLayer(m_layer, 0); });
        bars->addWidget(m_position, 0, 1);

        m_loop = new RangeField;
        m_loop->setRange(0, std::max(0.01, s.duration));
        m_loop->setDecimals(2);
        m_loop->setSuffix(QStringLiteral(" s"));
        m_loop->setValues(s.inPoint, s.outPoint < 0 ? s.duration : s.outPoint);
        m_loop->setToolTip(QStringLiteral("Played range: playback, loops and ping-pong stay between these two points"));
        const double duration0 = s.duration;
        barLabel(1, QStringLiteral("Loop"), [this] {
            m_undo->beginMacro(QStringLiteral("Clear In / Out Points"));
            setProp(cmd::SetLayerProp::InPoint, 0.0);
            setProp(cmd::SetLayerProp::OutPoint, -1.0);
            m_undo->endMacro();
            rebuild();
        });
        bars->addWidget(m_loop, 1, 1);
        v->addLayout(bars);

        // Playback is not a project edit: no undo on play, pause or seek.
        connect(m_playButtons, &QButtonGroup::idClicked, this, [this](int id) {
            double speed = 1;
            {
                Engine::Lock lk(&m_engine->mutex());
                Layer *l = m_engine->layer(m_layer);
                if (!l) return;
                speed = std::abs(l->speed) < 1e-6 ? 1.0 : std::abs(l->speed);
            }
            if (id == 1) {
                m_engine->setLayerPlaying(m_layer, false);
            } else {
                m_engine->setLayerSpeed(m_layer, id == 0 ? -speed : speed);
                m_engine->setLayerPlaying(m_layer, true);
            }
            refreshDynamic();
            emit layerChanged();
        });
        connect(m_position, &SliderField::valueEdited, this, [this](double t) { m_engine->seekLayer(m_layer, t); });
        connect(m_loop, &RangeField::edited, this, [this, duration0](bool low, double t) {
            setProp(low ? cmd::SetLayerProp::InPoint : cmd::SetLayerProp::OutPoint,
                    low ? t : (t >= duration0 - 1e-6 ? -1.0 : t));
        });
    }

    if (media && s.hasAudio) {
        // Sound: layer volume (undoable), mute, level
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *vol = new SliderField;
        vol->setRange(0, 200);
        vol->setDecimals(0);
        vol->setSuffix(QStringLiteral(" %"));
        vol->setSingleStep(5);
        vol->setTicks(8);
        vol->setSnaps({100});
        vol->setValue(s.volume * 100);
        m_volume = vol;
        vol->setToolTip(QStringLiteral("Layer volume (100% = original level). Hiding the layer also silences it."));
        auto *icon = new ResetLabel(QStringLiteral("Volume"), [vol] { vol->setValue(100); emit vol->valueEdited(100); });
        icon->setStyleSheet("color:#8a8a8e;");
        icon->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *mute = new FlagBox(QStringLiteral("Mute"));
        mute->setChecked(s.muted);
        row->addWidget(icon);
        row->addWidget(vol, 1);
        row->addWidget(mute);
        m_animate->attach({icon, vol}, {QStringLiteral("volume")});
        v->addLayout(row);
        m_meter = new QProgressBar;
        m_meter->setRange(0, 600);
        m_meter->setTextVisible(false);
        m_meter->setFixedHeight(5);
        m_meter->setStyleSheet("QProgressBar { background:#1b1b1d; border:none; } QProgressBar::chunk { background:#3fae5a; }");
        v->addWidget(m_meter);
        connect(vol, &SliderField::valueEdited, this, [this](double pct) {
            setProp(cmd::SetLayerProp::Volume, pct / 100.0);
            emit layerChanged();
        });
        connect(mute, &QCheckBox::toggled, this, [this](bool on) {
            setProp(cmd::SetLayerProp::Muted, on);
            emit layerChanged();
        });
    }

    if (s.type == SourceType::Isf && s.hasGenerator) {
        // Its inputs back to their defaults, at the top
        auto *reset = new QPushButton(QStringLiteral("Reset to Defaults"));
        reset->setToolTip(QStringLiteral("Every parameter of the shader back to its default value (one undo step)"));
        auto *resetRow = new QHBoxLayout;
        resetRow->addWidget(reset);
        resetRow->addStretch();
        v->addLayout(resetRow);
        auto *res = new QHBoxLayout;
        auto *w = new IntBox, *h = new IntBox;
        for (auto *sb : {w, h}) {
            sb->setRange(1, 16384);
            sb->setKeyboardTracking(false);
        }
        w->setValue(s.genW);
        h->setValue(s.genH);
        auto *fit = new QPushButton(QStringLiteral("= composition"));
        auto *reload = toolButton(QStringLiteral("⟳"), QStringLiteral("Reload shader from disk"));
        res->addWidget(new ResetLabel(QStringLiteral("Resolution"), [fit] { fit->click(); }));
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
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, -1, ParamPanel::NoSpeed | ParamPanel::NoReset);
        connect(reset, &QPushButton::clicked, params, &ParamPanel::resetToDefaults);
        m_generatorParams = params;
        params->attachAnimate(m_animate);
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    if (s.type == SourceType::None) {
        auto *hint = new QLabel(QStringLiteral("Nothing to set: the layer is empty."));
        hint->setStyleSheet("color:#888;");
        v->addWidget(hint);
    }
    return g;
}

// Part of the source picture used: preview with a rectangle whose sides are dragged, numeric fields in %
QWidget *LayerInspector::buildRoi(const LayerValues &s)
{
    auto *box = new QGroupBox;
    auto *v = new QVBoxLayout(box);
    auto *head = new QHBoxLayout;
    auto *title = new ResetLabel(QStringLiteral("<b>ROI</b>"), [this] {
        setProp(cmd::SetLayerProp::Roi, Layer::fullRoi());
        rebuild();
    });
    title->setToolTip(QStringLiteral("Part of the source picture used by the layer — click to use the whole picture"));
    head->addWidget(title);
    head->addStretch();
    v->addLayout(head);
    m_roi = new RoiEditor;
    m_roi->setAspect(s.aspect);
    m_roi->setRoi(s.roi);
    v->addWidget(m_roi);
    m_animate->attach({title, m_roi.data()}, {"roi/left", "roi/top", "roi/right", "roi/bottom"});
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);
    // Left / right and top / bottom are two ranges: one bar each, the bounds typed at either end
    RangeField *bars[2];
    const double bounds[2][2] = {{s.roi.left(), s.roi.right()}, {s.roi.top(), s.roi.bottom()}};
    static const char *kRows[] = {"Left · Right", "Top · Bottom"};
    for (int k = 0; k < 2; ++k) {
        auto *bar = new RangeField;
        bar->setRange(0, 100);
        bar->setDecimals(1);
        bar->setSuffix(QStringLiteral(" %"));
        bar->setValues(bounds[k][0] * 100, bounds[k][1] * 100);
        bars[k] = bar;
        auto *name = new ResetLabel(QString::fromUtf8(kRows[k]), [bar] {
            bar->setValues(0, 100);
            emit bar->editingFinished();
        });
        name->setStyleSheet("color:#8a8a8e;");
        name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(name, k, 0);
        grid->addWidget(bar, k, 1);
        m_animate->attach({name, bar}, k == 0 ? QStringList{"roi/left", "roi/right"} : QStringList{"roi/top", "roi/bottom"});
    }
    v->addLayout(grid);
    auto apply = [this](const QRectF &r) {
        setProp(cmd::SetLayerProp::Roi, r);
        emit layerChanged();
    };
    QPointer<RoiEditor> editor = m_roi;
    connect(m_roi, &RoiEditor::roiEdited, this, [apply, bars](const QRectF &r) {
        QSignalBlocker b0(bars[0]), b1(bars[1]);
        bars[0]->setValues(r.left() * 100, r.right() * 100);
        bars[1]->setValues(r.top() * 100, r.bottom() * 100);
        apply(r);
    });
    auto fromBars = [apply, bars, editor] {
        const double l = bars[0]->low() / 100, r = std::max(bars[0]->high() / 100, bars[0]->low() / 100 + 0.001);
        const double t = bars[1]->low() / 100, b = std::max(bars[1]->high() / 100, bars[1]->low() / 100 + 0.001);
        const QRectF rect(QPointF(l, t), QPointF(std::min(r, 1.0), std::min(b, 1.0)));
        if (editor) editor->setRoi(rect);
        apply(rect);
    };
    for (RangeField *bar : bars) {
        connect(bar, &RangeField::edited, this, [fromBars] { fromBars(); });
        connect(bar, &RangeField::editingFinished, this, [fromBars] { fromBars(); });
    }
    return box;
}

QWidget *LayerInspector::buildColor(const LayerValues &s)
{
    auto *g = new QWidget;
    auto *outer = new QVBoxLayout(g);
    outer->setContentsMargins(0, 0, 0, 0);
    // Switch of the whole section: the values are kept, they are simply not applied
    auto *colorOn = new FlagBox(QStringLiteral("Color"));
    colorOn->setChecked(s.color.enabled);
    colorOn->setStyleSheet("font-weight:bold;");
    colorOn->setToolTip(QStringLiteral("Apply the color of this layer.\nOff: every value is kept, the picture is left alone."));
    outer->addWidget(colorOn);
    auto *body = new QWidget; // everything the switch above turns off
    auto *v = new QVBoxLayout(body);
    v->setContentsMargins(0, 0, 0, 0);
    body->setEnabled(s.color.enabled);
    outer->addWidget(body);
    connect(colorOn, &QCheckBox::toggled, this, [this, body](bool on) {
        body->setEnabled(on);
        setProp(cmd::SetLayerProp::ColorOn, on);
    });

    // Mask: where the whole section applies, from another layer's picture stretched over this one
    {
        std::vector<std::pair<quint64, QString>> masks; // layers that can mask it (no viewport, no feedback)
        {
            Engine::Lock lk(&m_engine->mutex());
            const Layer *self = m_engine->layer(m_layer);
            for (int k = 0; self && k < m_engine->layerCount(); ++k) {
                const Layer *o = m_engine->layer(k);
                if (!o || o->isViewport || o->id == self->id || m_engine->layerDependsOn(o->id, self->id)) continue;
                masks.push_back({o->id, o->isGroup ? o->name + QStringLiteral(" (group)") : o->name});
            }
        }
        auto *row = new QHBoxLayout;
        auto *label = new ResetLabel(QStringLiteral("Mask"), [this] { setProp(cmd::SetLayerProp::ColorMask, QVariant(qulonglong(0))); });
        auto *pick = new QComboBox;
        pick->addItem(QStringLiteral("None — everywhere"), QVariant(qulonglong(0)));
        int current = 0;
        for (const auto &[id, n] : masks) {
            pick->addItem(n, QVariant(qulonglong(id)));
            if (id == s.color.maskLayer) current = pick->count() - 1;
        }
        if (s.color.maskLayer && current == 0) { // a mask that cannot be chosen any more (gone): shown, not chosen
            pick->addItem(QStringLiteral("(gone)"), QVariant(qulonglong(s.color.maskLayer)));
            current = pick->count() - 1;
        }
        pick->setCurrentIndex(current);
        pick->setToolTip(QStringLiteral("Where the color applies: fully where the chosen layer's picture is white, not at "
                                        "all where it is black or transparent, in proportion in between. That picture is "
                                        "stretched over this layer's, after its ROI. The layer can stay hidden."));
        auto *inv = new FlagBox(QStringLiteral("Invert"));
        inv->setChecked(s.color.maskInvert);
        inv->setEnabled(s.color.maskLayer != 0);
        row->addWidget(label);
        row->addWidget(pick, 1);
        row->addWidget(inv);
        v->addLayout(row);
        connect(pick, &QComboBox::activated, this, [this, pick, inv](int i) {
            const quint64 id = pick->itemData(i).toULongLong();
            QString err;
            const QVariant before = cmd::SetLayerProp::read(m_engine, m_layer, cmd::SetLayerProp::ColorMask);
            if (!m_engine->setColorMask(m_layer, id, inv->isChecked(), &err)) { // refused: feedback
                if (!err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Mask"), err);
            } else if (before.toULongLong() != id) {
                m_undo->push(new cmd::SetLayerProp(m_engine, m_layer, cmd::SetLayerProp::ColorMask, before, QVariant(qulonglong(id))));
            }
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        });
        connect(inv, &QCheckBox::toggled, this, [this](bool on) { setProp(cmd::SetLayerProp::ColorMaskInvert, on); });
    }

    // Balance first, as in DaVinci Resolve: temperature (blue / yellow) and tint (green / magenta)
    {
        auto *box = new QGroupBox;
        auto *grid = new QGridLayout(box);
        grid->addWidget(new QLabel(QStringLiteral("<b>Balance</b>")), 0, 0, 1, 4);
        struct Def {
            const char *name, *path;
            double range, value;
            QGradientStops gradient;
            int prop;
            QPointer<SliderField> *field;
            bool on;
            int onProp;
        } defs[] = {{"Temp", "temp", ColorAdjust::kTempRange, s.color.temp,
                     {{0.0, QColor("#3a7bff")}, {0.5, QColor("#888888")}, {1.0, QColor("#ffd23a")}},
                     cmd::SetLayerProp::Temp, &m_temp, s.color.tempOn, cmd::SetLayerProp::TempOn},
                    {"Tint", "tint", ColorAdjust::kTintRange, s.color.tint,
                     {{0.0, QColor("#2fd04a")}, {0.5, QColor("#888888")}, {1.0, QColor("#e03ce0")}},
                     cmd::SetLayerProp::Tint, &m_tint, s.color.tintOn, cmd::SetLayerProp::TintOn}};
        int row = 1;
        for (const Def &d : defs) {
            auto *bar = new SliderField;
            bar->setRange(-d.range, d.range);
            bar->setDecimals(d.range >= 1000 ? 0 : 1);
            bar->setSingleStep(d.range / 100);
            bar->setSnaps({0});
            bar->setGradient(d.gradient);
            bar->setValue(d.value);
            *d.field = bar;
            const int prop = d.prop;
            auto *on = new FlagBox;
            on->setChecked(d.on);
            on->setToolTip(QStringLiteral("Apply %1 (the value is kept either way)").arg(QString::fromUtf8(d.name)));
            const int onProp = d.onProp;
            connect(on, &QCheckBox::toggled, this, [this, onProp](bool v) { setProp(onProp, v); });
            grid->addWidget(on, row, 0);
            auto *name = new ResetLabel(QString::fromUtf8(d.name), [bar] {
                bar->setValue(0);
                emit bar->valueEdited(0);
            });
            grid->addWidget(name, row, 1);
            grid->addWidget(bar, row, 2, 1, 2);
            m_animate->attach({name, bar}, {QString::fromLatin1(d.path)});
            connect(bar, &SliderField::valueEdited, this, [this, prop](double x) { setProp(prop, x); });
            ++row;
        }
        grid->setColumnStretch(2, 1);
        v->addWidget(box);
    }

    // How this layer's colors are edited: kept by the layer (new layers take the choice of the Settings tab)
    auto *models = new QComboBox;
    models->addItem(QStringLiteral("RGB"), 1);
    models->addItem(QStringLiteral("HSL"), 2);
    models->addItem(QStringLiteral("Additive (R G B light)"), 4);
    models->addItem(QStringLiteral("Subtractive (C M Y filters)"), 8);
    models->addItem(QStringLiteral("All together"), 15);
    models->setCurrentIndex(std::max(0, models->findData(s.colorModels)));
    models->setProperty("allowLocked", true); // a way of showing, not an edit
    auto *modelRow = new QHBoxLayout;
    modelRow->addWidget(new QLabel(QStringLiteral("Edit in")));
    modelRow->addWidget(models, 1);
    v->addLayout(modelRow);
    connect(models, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, models] {
        const int m = models->currentData().toInt();
        {
            Engine::Lock lk(&m_engine->mutex());
            if (Layer *l = m_engine->layer(m_layer)) l->colorModels = m;
        }
        if (m_colorAdd) m_colorAdd->setModels(m);
        if (m_colorRemove) m_colorRemove->setModels(m);
        emit projectEdited();
    });

    auto toColor = [](const float c[3]) { return QColor::fromRgbF(c[0], c[1], c[2]); };
    m_colorAdd = new ColorEditor(QStringLiteral("Add"), Qt::black);
    m_colorAdd->setColor(toColor(s.color.add));
    m_colorAdd->setToolTip(QStringLiteral("Color added to the picture, as light (black: nothing added)"));
    m_colorRemove = new ColorEditor(QStringLiteral("Remove"), Qt::black);
    m_colorRemove->setColor(toColor(s.color.remove));
    m_colorRemove->setToolTip(QStringLiteral("Color removed from the picture, as a filter (black: nothing removed)"));
    for (ColorEditor *ed : {m_colorAdd.data(), m_colorRemove.data()}) {
        auto *frame = new QGroupBox;
        auto *fv = new QVBoxLayout(frame);
        fv->addWidget(ed);
        v->addWidget(frame);
        ed->setModels(s.colorModels);
    }
    m_animate->attach(m_colorAdd, {"add/r", "add/g", "add/b"});
    m_animate->attach(m_colorRemove, {"remove/r", "remove/g", "remove/b"});
    m_colorAdd->setSwitch(true, s.color.addOn, QStringLiteral("Apply the added color (it is kept either way)"));
    m_colorRemove->setSwitch(true, s.color.removeOn, QStringLiteral("Apply the removed color (it is kept either way)"));
    connect(m_colorAdd, &ColorEditor::colorEdited, this, [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorAdd, c); });
    connect(m_colorRemove, &ColorEditor::colorEdited, this,
            [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorRemove, c); });
    connect(m_colorAdd, &ColorEditor::switchToggled, this, [this](bool on) { setProp(cmd::SetLayerProp::AddOn, on); });
    connect(m_colorRemove, &ColorEditor::switchToggled, this, [this](bool on) { setProp(cmd::SetLayerProp::RemoveOn, on); });
    return g;
}

QWidget *LayerInspector::buildCompositing(const LayerValues &s)
{
    auto *g = new QWidget; // titled by its sub-tab
    auto *form = new QFormLayout(g);

    auto *opacity = new SliderField;
    opacity->setRange(0, 100);
    opacity->setDecimals(0);
    opacity->setSuffix(QStringLiteral(" %"));
    opacity->setSingleStep(1);
    opacity->setTicks(10);
    opacity->setSnaps({100});
    opacity->setValue(s.opacity * 100);
    m_opacity = opacity;
    auto *opacityLabel = new ResetLabel(QStringLiteral("Opacity"), [this, opacity] {
        opacity->setValue(100);
        setProp(cmd::SetLayerProp::Opacity, 1.0);
    });
    form->addRow(opacityLabel, opacity);
    m_animate->attach({opacityLabel, opacity}, {QStringLiteral("opacity")});
    connect(opacity, &SliderField::valueEdited, this, [this](double v) { setProp(cmd::SetLayerProp::Opacity, v / 100.0); });
    if (s.isViewport) return g; // drawn onto nothing: no blend, no routing

    auto *blend = new QComboBox;
    for (BlendMode m : kBlendModes) blend->addItem(blendModeName(m), int(m));
    blend->setCurrentIndex(blend->findData(int(s.blend)));
    form->addRow(new ResetLabel(QStringLiteral("Blend Mode"), [blend] { blend->setCurrentIndex(0); }), blend);
    connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, blend](int i) { setProp(cmd::SetLayerProp::Blend, blend->itemData(i).toInt()); });

    return g;
}

// The viewports it appears in: chosen at the top of the list (a group for everything inside it)
QWidget *LayerInspector::buildViewports(const LayerValues &s)
{
    // Viewports it appears in: chosen at the top of the list (a group for everything inside it)
    auto *routes = new QWidget;
    auto *rv = new QVBoxLayout(routes);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(4);
    auto *intro = new QLabel(QStringLiteral("How much of it each viewport shows (0 hides it). A click on a name shows it fully."));
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#888; font-size:11px;");
    rv->addWidget(intro);
    if (!s.routedBy.isEmpty()) {
        auto *note = new QLabel(QStringLiteral("Routed by its group “%1”.").arg(s.routedBy.toHtmlEscaped()));
        note->setStyleSheet("color:#999;");
        note->setWordWrap(true);
        rv->addWidget(note);
    }
    m_routeFields.clear();
    for (const auto &r : s.routes) {
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto *sl = new SliderField;
        auto *name = new ResetLabel(r.name, [sl] { // fully shown
            sl->setValue(100);
            emit sl->valueEdited(100);
        });
        name->setMinimumWidth(70);
        sl->setRange(0, 100);
        sl->setDecimals(0);
        sl->setSuffix(QStringLiteral(" %"));
        sl->setSingleStep(1);
        sl->setTicks(10);
        sl->setSnaps({0, 100});
        sl->setValue(r.opacity * 100);
        sl->setEnabled(s.routedBy.isEmpty());
        sl->setToolTip(QStringLiteral("How much of it the viewport “%1” shows (0 hides it)").arg(r.name));
        rl->addWidget(name);
        rl->addWidget(sl, 1);
        rv->addWidget(row);
        const quint64 vp = r.id;
        auto current = std::make_shared<float>(r.opacity);
        m_routeFields.push_back({r.id, sl, current});
        connect(sl, &SliderField::valueEdited, this, [this, vp, current, vname = r.name](double v) {
            if (m_engine->isLocked(m_layer)) return;
            const float after = float(v / 100.0);
            m_undo->push(new cmd::SetOpacityIn(m_engine, m_layerId, vp, *current, after,
                                               QStringLiteral("Opacity in %1").arg(vname)));
            *current = after;
        });
    }
    rv->addStretch();
    return routes;
}


QWidget *LayerInspector::buildMapping(const LayerValues &s)
{
    auto *g = new QWidget; // titled by its sub-tab
    auto *v = new QVBoxLayout(g);

    // Position (center, composition pixels) and scale (% of the composition) of the whole mapped layer
    {
        const QSize comp = m_engine->compositionSize();
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(6);
        auto spin = [](double lo, double hi, const QString &suffix, int decimals) {
            auto *b = new NumberBox;
            b->setRange(lo, hi);
            b->setDecimals(decimals);
            b->setSuffix(suffix);
            b->setKeyboardTracking(false);
            b->setAccelerated(true);
            return b;
        };
        // Their limits are the parameters' (Parameter.h)
        auto spinOf = [this, spin](const QString &path, const QString &suffix, int decimals) {
            const auto [lo, hi] = limitsOf(path, -100000, 100000);
            return spin(lo, hi, suffix, decimals);
        };
        m_posX = spinOf(QStringLiteral("position/x"), QStringLiteral(" px"), 1);
        m_posY = spinOf(QStringLiteral("position/y"), QStringLiteral(" px"), 1);
        m_sizePx = s.isViewport;
        m_scaleX = m_sizePx ? spinOf(QStringLiteral("width"), QStringLiteral(" px"), 1)
                            : spinOf(QStringLiteral("scale/x"), QStringLiteral(" %"), 2);
        m_scaleY = m_sizePx ? spinOf(QStringLiteral("height"), QStringLiteral(" px"), 1)
                            : spinOf(QStringLiteral("scale/y"), QStringLiteral(" %"), 2);
        m_rotation = spin(-360, 360, QStringLiteral("°"), 1);
        m_posX->setToolTip(QStringLiteral("Horizontal position of the layer's center, in composition pixels"));
        m_posY->setToolTip(QStringLiteral("Vertical position of the layer's center, in composition pixels"));
        m_scaleX->setToolTip(m_sizePx ? QStringLiteral("Width of the region this viewport shows, in composition pixels")
                                      : QStringLiteral("Width of the layer, in % of the composition width"));
        m_scaleY->setToolTip(m_sizePx ? QStringLiteral("Height of the region this viewport shows, in composition pixels")
                                      : QStringLiteral("Height of the layer, in % of the composition height"));
        m_rotation->setToolTip(QStringLiteral("Rotation of the layer, in degrees"));
        auto *link = new QToolButton;
        link->setCheckable(true);
        link->setChecked(m_scaleLinked);
        link->setText(QStringLiteral("⛓"));
        link->setToolTip(QStringLiteral("Linked: width and height keep the aspect ratio. Click to unlink them."));
        link->setStyleSheet(QStringLiteral("QToolButton:checked { background:%1; color:%2; }")
                                .arg(theme::css(), theme::onAccent().name()));
        auto *posLabel = new ResetLabel(QStringLiteral("Position"), [this, comp] {
            m_posX->setValue(comp.width() / 2.0); // centered
            m_posY->setValue(comp.height() / 2.0);
        });
        auto *scaleLabel = new ResetLabel(m_sizePx ? QStringLiteral("Size") : QStringLiteral("Scale"), [this, comp] {
            const bool linked = m_scaleLinked;
            m_scaleLinked = false;
            m_scaleX->setValue(m_sizePx ? comp.width() : 100.0); // the whole composition
            m_scaleY->setValue(m_sizePx ? comp.height() : 100.0);
            m_scaleLinked = linked;
        });
        grid->addWidget(posLabel, 0, 0);
        grid->addWidget(new QLabel(QStringLiteral("X")), 0, 1);
        grid->addWidget(m_posX, 0, 2);
        grid->addWidget(new QLabel(QStringLiteral("Y")), 0, 4);
        grid->addWidget(m_posY, 0, 5);
        grid->addWidget(scaleLabel, 1, 0);
        grid->addWidget(new QLabel(m_sizePx ? QStringLiteral("W") : QStringLiteral("X")), 1, 1);
        grid->addWidget(m_scaleX, 1, 2);
        grid->addWidget(link, 1, 3);
        grid->addWidget(new QLabel(m_sizePx ? QStringLiteral("H") : QStringLiteral("Y")), 1, 4);
        grid->addWidget(m_scaleY, 1, 5);
        {
            m_rotation->setWrapping(true);
            m_rotation->setRange(-180, 180);
            auto *rotLabel = new ResetLabel(QStringLiteral("Rotation"), [this] { m_rotation->setValue(0); });
            m_animate->attach(rotLabel, {"rotation"});
            grid->addWidget(rotLabel, 2, 0);
            grid->addWidget(m_rotation, 2, 2);
        }
        m_pivotX = spinOf(QStringLiteral("pivot/x"), QStringLiteral(" %"), 1);
        m_pivotY = spinOf(QStringLiteral("pivot/y"), QStringLiteral(" %"), 1);
        m_pivotX->setToolTip(QStringLiteral("Horizontal center of the rotation, in % of the layer (50: its middle)"));
        m_pivotY->setToolTip(QStringLiteral("Vertical center of the rotation, in % of the layer (50: its middle)"));
        auto *pivotLabel = new ResetLabel(QStringLiteral("Pivot"), [this] {
            editMapping(QStringLiteral("Pivot"), [](Mapping &m) {
                m.pivot = QPointF(0.5, 0.5); // the middle of the picture
                ++m.revision;
            });
            refreshSpatial();
        });
        grid->addWidget(pivotLabel, 3, 0);
        grid->addWidget(new QLabel(QStringLiteral("X")), 3, 1);
        grid->addWidget(m_pivotX, 3, 2);
        grid->addWidget(new QLabel(QStringLiteral("Y")), 3, 4);
        grid->addWidget(m_pivotY, 3, 5);
        {
            const QString sx = m_sizePx ? QStringLiteral("width") : QStringLiteral("scale/x");
            const QString sy = m_sizePx ? QStringLiteral("height") : QStringLiteral("scale/y");
            m_animate->attach(posLabel, {"position/x", "position/y"});
            m_animate->attach(m_posX, {"position/x"});
            m_animate->attach(m_posY, {"position/y"});
            m_animate->attach(scaleLabel, {sx, sy});
            m_animate->attach(m_scaleX, {sx});
            m_animate->attach(m_scaleY, {sy});
            m_animate->attach(m_rotation, {"rotation"});
            m_animate->attach(pivotLabel, {"pivot/x", "pivot/y"});
            m_animate->attach(m_pivotX, {"pivot/x"});
            m_animate->attach(m_pivotY, {"pivot/y"});
        }
        grid->setColumnStretch(2, 1);
        grid->setColumnStretch(5, 1);
        v->addLayout(grid);
        refreshSpatial();
        // The fields are the mapping's own values (as its parameters show them: position and pivot in composition
        // pixels, the size in % of the composition, or in pixels for a viewport, the rotation in degrees)
        if (m_rotation) {
            connect(m_rotation, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double deg) {
                editMapping(QStringLiteral("Rotation"), [&](Mapping &m) {
                    m.rotation = deg;
                    ++m.revision;
                }, true);
                refreshSpatial();
            });
        }

        connect(link, &QToolButton::toggled, this, [this](bool on) { m_scaleLinked = on; });
        auto setPivot = [this](bool x, double v) {
            editMapping(QStringLiteral("Pivot"), [&](Mapping &m) {
                (x ? m.pivot.rx() : m.pivot.ry()) = v / 100.0;
                ++m.revision;
            }, true);
        };
        connect(m_pivotX, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [setPivot](double v) { setPivot(true, v); });
        connect(m_pivotY, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [setPivot](double v) { setPivot(false, v); });
        auto setPosition = [this, comp](bool x, double v) {
            editMapping(QStringLiteral("Position"), [&](Mapping &m) {
                (x ? m.position.rx() : m.position.ry()) = v / (x ? comp.width() : comp.height());
                ++m.revision;
            }, true);
        };
        connect(m_posX, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [setPosition](double v) { setPosition(true, v); });
        connect(m_posY, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [setPosition](double v) { setPosition(false, v); });
        auto scale = [this, comp](double sx, double sy, bool fromX) {
            // in the composition's units: a fraction of its width / height
            const double unitX = m_sizePx ? comp.width() : 100.0, unitY = m_sizePx ? comp.height() : 100.0;
            editMapping(m_sizePx ? QStringLiteral("Size") : QStringLiteral("Scale"), [&](Mapping &m) {
                double w = sx / unitX, h = sy / unitY;
                const QSizeF was = m.size;
                if (m_scaleLinked) { // the other side follows, keeping the aspect ratio
                    if (fromX && std::abs(was.width()) > 1e-12) h = was.height() * w / was.width();
                    if (!fromX && std::abs(was.height()) > 1e-12) w = was.width() * h / was.height();
                } else if (fromX) {
                    h = was.height();
                } else {
                    w = was.width();
                }
                const double least = m_sizePx ? 1.0 / std::max(1, std::min(comp.width(), comp.height())) : 0.001;
                m.size = QSizeF(std::max(least, w), std::max(least, h));
                ++m.revision;
            }, true);
            refreshSpatial();
        };
        connect(m_scaleX, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [scale, this](double x) { scale(x, m_scaleY->value(), true); });
        connect(m_scaleY, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [scale, this](double y) { scale(m_scaleX->value(), y, false); });
    }

    if (s.isViewport) {
        // A viewport is a rectangle of the composition: no corners, no mesh (the projector's warp comes later)
        auto *actions = new QHBoxLayout;
        auto *pixels = new QPushButton(QStringLiteral("Pixel for Pixel"));
        pixels->setToolTip(QStringLiteral("One viewport pixel for one composition pixel: the region takes the "
                                          "viewport's size, around its center"));
        auto *whole = new QPushButton(QStringLiteral("Whole Composition"));
        actions->addWidget(pixels);
        actions->addWidget(whole);
        actions->addStretch();
        v->addLayout(actions);
        auto *hint = new QLabel(QStringLiteral("The region of the composition this viewport shows. "
                                               "In the preview, drag its frame to move it."));
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#888; font-size:11px;");
        v->addWidget(hint);
        const QSize vs = s.vpSize;
        connect(pixels, &QPushButton::clicked, this, [this, vs] {
            const QSize c = m_engine->compositionSize();
            editMapping(QStringLiteral("Pixel for Pixel"), [&](Mapping &m) {
                Mapping::Rect r = m.rect(c);
                r.w = vs.width();
                r.h = vs.height();
                m.setRect(r, c);
            });
            refreshSpatial();
        });
        connect(whole, &QPushButton::clicked, this, [this] {
            editMapping(QStringLiteral("Whole Composition"), [](Mapping &m) { m.resetCorners(); });
            refreshSpatial();
        });
        return g;
    }

    auto *modeRow = new QHBoxLayout;
    auto *corners = new QRadioButton(QStringLiteral("Corners"));
    auto *mesh = new QRadioButton(QStringLiteral("Mesh"));
    (s.meshMode ? mesh : corners)->setChecked(true);
    auto *modes = new QButtonGroup(g);
    modes->addButton(corners, 0);
    modes->addButton(mesh, 1);
    auto *cols = new IntBox, *rows = new IntBox;
    cols->setRange(2, 32);
    rows->setRange(2, 32);
    cols->setValue(s.cols);
    rows->setValue(s.rows);
    cols->setToolTip(QStringLiteral("Point columns"));
    rows->setToolTip(QStringLiteral("Point rows"));
    auto *apply = new QPushButton(QStringLiteral("Apply"));
    apply->setToolTip(QStringLiteral("Change the mesh density (resets the warp)"));
    modeRow->addWidget(corners);
    modeRow->addWidget(mesh);
    modeRow->addStretch();
    modeRow->addWidget(cols);
    modeRow->addWidget(new QLabel(QStringLiteral("×")));
    modeRow->addWidget(rows);
    modeRow->addWidget(apply);
    v->addLayout(modeRow);

    auto *actions = new QHBoxLayout;
    auto *full = new QPushButton(QStringLiteral("Full Frame"));
    auto *ratio = new QPushButton(QStringLiteral("Source Aspect"));
    auto *resetMesh = new QPushButton(QStringLiteral("Flatten Mesh"));
    actions->addWidget(full);
    actions->addWidget(ratio);
    actions->addWidget(resetMesh);
    v->addLayout(actions);

    auto *hint = new QLabel(QStringLiteral("Drag: move · Shift: fine · Arrows: 1 px (Shift: 10 px) · "
                                           "Tab: next handle · Esc: deselect\n"
                                           "Several points: Ctrl/⌘+click to add, Ctrl/⌘+drag a rectangle, "
                                           "Ctrl/⌘+A for all; they move together.\n"
                                           "Preview: mouse wheel or pinch to zoom (finer moves), middle button or "
                                           "Alt/⌥ + drag to pan, Fit to see everything."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#888; font-size:11px;");
    v->addWidget(hint);

    connect(modes, &QButtonGroup::idClicked, this, [this](int id) {
        editMapping(id == 1 ? QStringLiteral("Mesh Mode") : QStringLiteral("Corners Mode"),
                    [id](Mapping &m) { m.meshMode = id == 1; });
    });
    connect(apply, &QPushButton::clicked, this, [this, cols, rows] {
        bool deformed = false;
        for (const QPointF &o : cmd::SetMapping::read(m_engine, m_layer).offsets) deformed |= !o.isNull();
        if (deformed && QMessageBox::question(this, QStringLiteral("Mesh"),
                                              QStringLiteral("Changing the density clears the current warp "
                                                             "(undoable with Ctrl+Z). Continue?"))
                            != QMessageBox::Yes)
            return;
        const int c = cols->value(), r = rows->value();
        editMapping(QStringLiteral("Mesh Density"), [c, r](Mapping &m) {
            m.resetMesh(c, r);
            m.meshMode = true;
        });
        rebuild();
    });
    connect(full, &QPushButton::clicked, this,
            [this] { editMapping(QStringLiteral("Full Frame"), [](Mapping &m) { m.resetCorners(); }); });
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
        editMapping(QStringLiteral("Source Aspect"),
                    [=](Mapping &m) { m.fitAspect(double(sw) / sh, double(c.width()) / c.height()); });
    });
    connect(resetMesh, &QPushButton::clicked, this, [this] {
        editMapping(QStringLiteral("Flatten Mesh"), [](Mapping &m) { m.resetMesh(m.cols, m.rows); });
    });

    // Soft edge: the picture fades out towards each side (to blend overlapping projections)
    {
        const SoftEdge se = cmd::SetMapping::read(m_engine, m_layer).soft;
        auto *box = new QGroupBox(QStringLiteral("Soft Edge"));
        box->setCheckable(true);
        box->setChecked(se.enabled);
        m_softBox = box;
        box->setToolTip(QStringLiteral("Fades the picture to transparent towards each side. Width: how far the fade "
                                       "reaches, in % of the layer. Power: shape of the fade (1 linear, above 1 "
                                       "stays dark longer, below 1 brightens sooner)."));
        auto *grid = new QGridLayout(box);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        grid->addWidget(new QLabel(QStringLiteral("Width")), 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("Power")), 0, 3);
        static const char *names[4] = {"Left", "Right", "Top", "Bottom"};
        for (int side = 0; side < 4; ++side) {
            auto *width = new SliderField;
            width->setRange(0, 50);
            width->setDecimals(1);
            width->setSuffix(QStringLiteral(" %"));
            width->setSingleStep(0.5);
            width->setSnaps({0});
            width->setValue(se.width[side] * 100.0);
            auto *power = new SliderField;
            power->setRange(0.1, 4);
            power->setTypedRange(0.1, 8);
            power->setDecimals(2);
            power->setSuffix(QStringLiteral(" ×"));
            power->setSingleStep(0.05);
            power->setSnaps({1});
            power->setValue(se.power[side]);
            m_softWidth[side] = width;
            m_softPower[side] = power;
            auto *name = new ResetLabel(QString::fromLatin1(names[side]), [width, power] {
                width->setValue(10.0);
                power->setValue(1.0);
                emit width->valueEdited(10.0);
                emit power->valueEdited(1.0);
            });
            grid->addWidget(name, side + 1, 0);
            const QString base = QStringLiteral("soft_edge/%1/").arg(QString::fromLatin1(names[side]).toLower());
            m_animate->attach(name, {base + "width", base + "power"});
            m_animate->attach(width, {base + "width"});
            m_animate->attach(power, {base + "power"});
            grid->addWidget(width, side + 1, 1);
            grid->addWidget(power, side + 1, 3);
            connect(width, &SliderField::valueEdited, this, [this, side](double v) {
                editMapping(QStringLiteral("Soft Edge Width"), [&](Mapping &m) { m.soft.width[side] = std::clamp(v / 100.0, 0.0, 0.5); },
                            true);
            });
            connect(power, &SliderField::valueEdited, this, [this, side](double v) {
                editMapping(QStringLiteral("Soft Edge Power"), [&](Mapping &m) { m.soft.power[side] = std::clamp(v, 0.1, 8.0); },
                            true);
            });
        }
        connect(box, &QGroupBox::toggled, this, [this](bool on) {
            editMapping(on ? QStringLiteral("Soft Edge On") : QStringLiteral("Soft Edge Off"),
                        [on](Mapping &m) { m.soft.enabled = on; });
        });
        v->addWidget(box);
    }
    return g;
}

QWidget *LayerInspector::buildEffects(const LayerValues &s)
{
    auto *g = new QWidget; // titled by its sub-tab
    auto *v = new QVBoxLayout(g);

    // General switch of the chain
    auto *all = new FlagBox(QStringLiteral("FX Enable"));
    all->setChecked(s.effectsEnabled);
    all->setToolTip(QStringLiteral("Turns the whole FX chain on or off (each FX keeps its own switch)"));
    all->setStyleSheet("QCheckBox { font-weight:bold; }");
    v->addWidget(all);
    connect(all, &QCheckBox::toggled, this, [this](bool on) {
        setProp(cmd::SetLayerProp::EffectsEnabled, on);
        emit layerChanged();
    });

    auto *bar = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add FX…"));
    add->setToolTip(QStringLiteral("Search the ISF library and add an FX to the chain"));
    connect(add, &QPushButton::clicked, this, [this, add] {
        QList<SearchPicker::Item> items;
        for (const IsfEntry &e : m_engine->library().filters())
            items.append({e.name, e.categories.value(0), e.description, e.path});
        auto *picker = new SearchPicker(items, this);
        connect(picker, &SearchPicker::picked, this, [this](const QString &p) {
            editEffects(QStringLiteral("Add FX %1").arg(QFileInfo(p).completeBaseName()), [this, p] {
                QString err;
                m_selectedEffect = m_engine->addEffect(m_layer, p, &err);
            });
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        });
        picker->popup(add->mapToGlobal(QPoint(0, add->height())));
    });
    auto *rescan = toolButton(QStringLiteral("↻"), QStringLiteral("Rescan ISF Library (shaders added in the Finder)"));
    connect(rescan, &QToolButton::clicked, this, &LayerInspector::rescanLibraryRequested);
    auto *remove = toolButton(QStringLiteral("−"), QStringLiteral("Remove FX"));
    auto *up = toolButton(QStringLiteral("▲"), QStringLiteral("Move Up (applied earlier)"));
    auto *down = toolButton(QStringLiteral("▼"), QStringLiteral("Move Down (applied later)"));
    auto *reload = toolButton(QStringLiteral("⟳"), QStringLiteral("Reload shader from disk"));
    bar->addWidget(add, 1);
    bar->addWidget(rescan);
    bar->addWidget(remove);
    bar->addWidget(up);
    bar->addWidget(down);
    bar->addWidget(reload);
    v->addLayout(bar);

    if (s.effects.empty()) {
        auto *none = new QLabel(QStringLiteral("No FX. FX are applied in list order."));
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
        auto *it = new QListWidgetItem(fx.name + (fx.masked ? QStringLiteral("  ◐") : QString()) +
                                       (fx.valid ? QString() : QStringLiteral("  ⚠")));
        if (fx.masked) it->setToolTip(QStringLiteral("Through a mask"));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(fx.enabled ? Qt::Checked : Qt::Unchecked);
        list->addItem(it);
    }
    list->setCurrentRow(m_selectedEffect);
    list->setMaximumHeight(qMin(160, 26 * count + 8));
    if (!s.effectsEnabled) list->setStyleSheet("QListWidget { color:#777; }");
    list->setProperty("allowLocked", true); // selecting an effect to see its parameters is not an edit
    v->addWidget(list);

    connect(list, &QListWidget::itemChanged, this, [this, list](QListWidgetItem *it) {
        const int r = list->row(it);
        const bool on = it->checkState() == Qt::Checked;
        editEffects(on ? QStringLiteral("Enable FX") : QStringLiteral("Disable FX"), [this, r, on] {
            Engine::Lock lk(&m_engine->mutex());
            Layer *ly = m_engine->layer(m_layer);
            if (ly && r >= 0 && r < int(ly->effects.size())) ly->effects[size_t(r)]->enabled = on;
        });
    });
    connect(list, &QListWidget::currentRowChanged, this, [this](int r) {
        // Only the parameters below are swapped. Rebuilding the whole tab here would destroy the list
        // between the press and the release, and the click that picked the row would never reach its
        // check box — which is why enabling an effect used to take two clicks.
        if (r >= 0 && r != m_selectedEffect) {
            m_selectedEffect = r;
            showEffectParams();
        }
    });
    connect(remove, &QToolButton::clicked, this, [this] {
        editEffects(QStringLiteral("Remove FX"), [this] { m_engine->removeEffect(m_layer, m_selectedEffect); });
        rebuild();
    });
    connect(up, &QToolButton::clicked, this, [this] {
        if (m_selectedEffect <= 0) return;
        editEffects(QStringLiteral("Reorder FX"),
                    [this] { m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect - 1); });
        --m_selectedEffect;
        rebuild();
    });
    connect(down, &QToolButton::clicked, this, [this, count] {
        if (m_selectedEffect >= count - 1) return;
        editEffects(QStringLiteral("Reorder FX"),
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

    // Parameters of the selected effect: their own container, refilled in place
    m_fxDetail = new QWidget;
    auto *detail = new QVBoxLayout(m_fxDetail);
    detail->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_fxDetail);
    showEffectParams();
    return g;
}

// Name, error and parameters of the selected effect, replaced without touching the list above
void LayerInspector::showEffectParams()
{
    if (!m_fxDetail) return;
    auto *lay = static_cast<QVBoxLayout *>(m_fxDetail->layout());
    while (QLayoutItem *item = lay->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    QString name, error;
    bool valid = false;
    quint64 maskId = 0, selfId = 0;
    bool invert = false, maskPre = false;
    std::vector<std::pair<quint64, QString>> masks; // layers that can mask it (no viewport, no feedback)
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l || l->effects.empty()) return;
        m_selectedEffect = std::clamp(m_selectedEffect, 0, int(l->effects.size()) - 1);
        const IsfInstance *fx = l->effects[size_t(m_selectedEffect)].get();
        name = fx->name();
        error = fx->error();
        valid = fx->isValid();
        maskId = fx->maskLayer;
        invert = fx->maskInvert;
        maskPre = fx->maskPreFx;
        selfId = l->id;
        for (int k = 0; k < m_engine->layerCount(); ++k) {
            const Layer *o = m_engine->layer(k);
            if (!o || o->isViewport || o->id == selfId || m_engine->layerDependsOn(o->id, selfId)) continue;
            masks.push_back({o->id, o->isGroup ? o->name + QStringLiteral(" (group)") : o->name});
        }
    }
    lay->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped())));
    if (!error.isEmpty()) lay->addWidget(errorLabel(error));

    // Mask: where the effect applies, from another layer's picture stretched over this one
    {
        auto *row = new QHBoxLayout;
        auto *label = new ResetLabel(QStringLiteral("Mask"), [this] {
            const int k = m_selectedEffect;
            editEffects(QStringLiteral("Remove FX Mask"), [this, k] { m_engine->setEffectMask(m_layer, k, 0, false); });
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection); // the list shows ◐
        });
        auto *pick = new QComboBox;
        pick->addItem(QStringLiteral("None — everywhere"), QVariant(qulonglong(0)));
        int current = 0;
        for (const auto &[id, n] : masks) {
            pick->addItem(n, QVariant(qulonglong(id)));
            if (id == maskId) current = pick->count() - 1;
        }
        if (maskId && current == 0) { // a mask that cannot be chosen any more (gone): shown, not chosen
            pick->addItem(QStringLiteral("(gone)"), QVariant(qulonglong(maskId)));
            current = pick->count() - 1;
        }
        pick->setCurrentIndex(current);
        pick->setToolTip(QStringLiteral("Where this FX applies: fully where the chosen layer's picture is white, not at "
                                        "all where it is black or transparent, in proportion in between. That picture is "
                                        "stretched over this layer's, before the mapping. The layer can stay hidden."));
        auto *inv = new FlagBox(QStringLiteral("Invert"));
        inv->setChecked(invert);
        inv->setEnabled(maskId != 0);
        row->addWidget(label);
        row->addWidget(pick, 1);
        row->addWidget(inv);
        auto *tap = new QComboBox;
        tap->addItem(QStringLiteral("Pre-FX"), true);
        tap->addItem(QStringLiteral("Post-FX"), false);
        tap->setCurrentIndex(maskPre ? 0 : 1);
        tap->setEnabled(maskId != 0);
        tap->setToolTip(QStringLiteral("The mask's picture: before or after the mask layer's own FX"));
        row->addWidget(tap);
        lay->addLayout(row);
        connect(tap, &QComboBox::activated, this, [this, tap](int i) {
            const bool pre = tap->itemData(i).toBool();
            const int k = m_selectedEffect;
            editEffects(QStringLiteral("Set FX Mask Tap"), [&] { m_engine->setEffectMaskTap(m_layer, k, pre); });
        });
        connect(pick, &QComboBox::activated, this, [this, pick, inv](int i) {
            const quint64 id = pick->itemData(i).toULongLong();
            const int k = m_selectedEffect;
            QString err;
            bool ok = true;
            editEffects(id ? QStringLiteral("Set FX Mask") : QStringLiteral("Remove FX Mask"),
                        [&] { ok = m_engine->setEffectMask(m_layer, k, id, inv->isChecked(), &err); });
            if (!ok && !err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Mask"), err);
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        });
        connect(inv, &QCheckBox::toggled, this, [this, pick](bool on) {
            const quint64 id = pick->currentData().toULongLong();
            const int k = m_selectedEffect;
            editEffects(QStringLiteral("Invert FX Mask"), [&] { m_engine->setEffectMask(m_layer, k, id, on); });
        });
    }
    if (valid) {
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, m_selectedEffect);
        m_effectParams = params;
        params->attachAnimate(m_animate);
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        lay->addWidget(params);
    }
    lockInputs(m_fxDetail, m_locked);
}

void LayerInspector::refreshSpatial()
{
    if (!m_posX) return;
    Mapping m;
    const QSize comp = m_engine->compositionSize();
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l) return;
        m = l->mapping;
    }
    const double unitX = m_sizePx ? comp.width() : 100.0, unitY = m_sizePx ? comp.height() : 100.0;
    QDoubleSpinBox *boxes[7] = {m_posX, m_posY, m_scaleX, m_scaleY, m_pivotX, m_pivotY, m_rotation};
    const double all[7] = {m.position.x() * comp.width(), m.position.y() * comp.height(), m.size.width() * unitX,
                           m.size.height() * unitY, m.pivot.x() * 100.0, m.pivot.y() * 100.0, std::remainder(m.rotation, 360.0)};
    for (int k = 0; k < 7; ++k) {
        if (!boxes[k] || boxes[k]->hasFocus()) continue; // being edited
        if (std::abs(boxes[k]->value() - all[k]) < 1e-6) continue;
        QSignalBlocker blk(boxes[k]);
        boxes[k]->setValue(all[k]);
    }
}

void LayerInspector::refreshDynamic()
{
    // The fields show the values as they move (a snapshot's fade), unless Settings keeps them still until it is over
    const bool follow = SettingsPanel::followFades() || !m_engine->isFading();
    if (follow) refreshSpatial();
    if (m_output) m_output->refreshStatus(); // mode set by ⌘F or a closed window, publishing states
    // ROI preview: the source picture, read back by the render thread while the editor is shown
    const bool wantPreview = m_roi && m_roi->isVisible();
    if (wantPreview) {
        m_engine->requestSourcePreview(m_layerId, 480);
        quint64 id = 0;
        const QImage img = m_engine->sourcePreview(&id);
        if (id == m_layerId && !img.isNull()) m_roi->setImage(img);
    } else if (m_previewing) {
        m_engine->requestSourcePreview(0);
    }
    m_previewing = wantPreview;
    // Values changed elsewhere (OSC, undo, a snapshot) follow in the fields
    if (follow) {
        QRectF roi;
        QColor add, remove;
        double temp = 0, tint = 0;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(m_layer);
            if (!l) return;
            roi = l->roi;
            add = QColor::fromRgbF(l->color.add[0], l->color.add[1], l->color.add[2]);
            remove = QColor::fromRgbF(l->color.remove[0], l->color.remove[1], l->color.remove[2]);
            temp = l->color.temp;
            tint = l->color.tint;
        }
        for (auto [field, value] : {std::pair{m_temp.data(), temp}, std::pair{m_tint.data(), tint}})
            if (field && !field->isDragging() && std::abs(field->value() - value) > 1e-3) field->setValue(value);
        if (m_roi) m_roi->setRoi(roi);
        if (m_colorAdd) m_colorAdd->setColor(add);
        if (m_colorRemove) m_colorRemove->setColor(remove);
    }
    if (follow) {
        SoftEdge soft;
        float opacity = 1, volume = 1;
        std::vector<float> routes;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(m_layer);
            if (!l) return;
            soft = l->mapping.soft;
            opacity = l->opacity;
            volume = l->volume;
            for (const RouteField &r : m_routeFields) routes.push_back(l->opacityIn(r.viewport));
        }
        if (m_softBox && m_softBox->isChecked() != soft.enabled) {
            QSignalBlocker blk(m_softBox);
            m_softBox->setChecked(soft.enabled);
        }
        auto show = [](SliderField *f, double value) {
            if (f && !f->isDragging() && std::abs(f->value() - value) > 1e-3) f->setValue(value);
        };
        show(m_opacity, opacity * 100.0);
        show(m_volume, volume * 100.0);
        if (m_generatorParams) m_generatorParams->refresh();
        if (m_genSpeed) {
            double gs = 1;
            {
                Engine::Lock lk(&m_engine->mutex());
                Layer *l = m_engine->layer(m_layer);
                if (l && l->generator) gs = l->generator->speed;
            }
            show(m_genSpeed, gs);
        }
        if (m_effectParams) m_effectParams->refresh();
        for (int side = 0; side < 4; ++side) {
            show(m_softWidth[side], soft.width[side] * 100.0);
            show(m_softPower[side], soft.power[side]);
        }
        for (size_t k = 0; k < m_routeFields.size(); ++k) {
            const RouteField &r = m_routeFields[k];
            if (!r.field || r.field->isDragging()) continue;
            *r.current = routes[k];
            show(r.field, routes[k] * 100.0);
        }
    }
    bool playing;
    double d, p, in, out, speed;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l || (l->type != SourceType::Video && l->type != SourceType::Audio)) return;
        in = l->inPoint;
        out = l->outPoint;
        if (m_meter) {
            const float pk = l->audio ? l->audio->peak() : 0.0f;
            const double db = pk > 1e-6f ? 20.0 * std::log10(pk) : -60.0;
            m_meter->setValue(int(std::clamp(db + 60.0, 0.0, 60.0) * 10));
        }
        playing = l->playing;
        speed = l->speed;
        d = l->duration();
        p = l->position();
        if (l->video && m_pictureFact && l->frame.layout && m_pictureFact->text() != l->frame.layout->description)
            m_pictureFact->setText(l->frame.layout->description);
    }
    // The transport follows what the engine does (play from a key, OSC, the end of a one-shot)
    if (m_playButtons) {
        const int on = !playing ? 1 : (speed < 0 ? 0 : 2);
        if (QAbstractButton *b = m_playButtons->button(on); b && !b->isChecked()) b->setChecked(true);
    }
    if (m_position && !m_position->isDragging()) {
        m_position->setRange(0, std::max(0.01, d));
        m_position->setValue(p);
    }
    if (!follow) return;
    if (m_speed && !m_speed->isDragging() && std::abs(m_speed->value() - speed * 100) > 0.5)
        m_speed->setValue(speed * 100);
    if (m_loop && !m_loop->isDragging()) {
        m_loop->setRange(0, std::max(0.01, d));
        m_loop->setValues(in, out < 0 ? d : out);
    }
}
