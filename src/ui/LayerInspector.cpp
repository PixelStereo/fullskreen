#include "LayerInspector.h"
#include "Commands.h"
#include "Engine.h"
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
struct LayerSnapshot {
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
    bool visible = true;
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
    // Another layer as the source, and the layers that could be chosen (id, name; cycles left out)
    quint64 sourceLayer = 0;
    LayerTap sourceTap = LayerTap::PostFx;
    QString transition; // when a memory changes the source (empty: the default)
    std::vector<std::pair<quint64, QString>> candidates;
    struct Fx {
        QString name, error;
        bool valid = false, enabled = true, masked = false;
    };
    std::vector<Fx> effects;
    bool meshMode = false;
    int cols = 4, rows = 4;
    // Text layer
    QString textContent;
    QString textFont = "Arial";
    int textSize = 48;
    QColor textColor = QColor(255, 255, 255);
    Qt::Alignment textAlign = Qt::AlignCenter;
    float textLineHeight = 1.2f;
    float textLetterSpacing = 0.0f;
    bool textBold = false, textItalic = false, textUnderline = false, textStrike = false;
    float textOutline = 0.0f;
    QColor textOutlineColor = QColor(0, 0, 0);
    bool textShadow = false;
    QColor textShadowColor = QColor(0, 0, 0, 160);
    float textShadowX = 4.0f, textShadowY = 4.0f;

    static LayerSnapshot take(Engine *e, int index)
    {
        LayerSnapshot s;
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
        // Text layer properties
        s.textContent = l->textContent;
        s.textFont = l->textFont;
        s.textSize = l->textSize;
        s.textColor = l->textColor;
        s.textAlign = l->textAlign;
        s.textLineHeight = l->textLineHeight;
        s.textLetterSpacing = l->textLetterSpacing;
        s.textBold = l->textBold;
        s.textItalic = l->textItalic;
        s.textUnderline = l->textUnderline;
        s.textStrike = l->textStrike;
        s.textOutline = l->textOutline;
        s.textOutlineColor = l->textOutlineColor;
        s.textShadow = l->textShadow;
        s.textShadowColor = l->textShadowColor;
        s.textShadowX = l->textShadowX;
        s.textShadowY = l->textShadowY;
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
// Drop zone of the Source tab: a file from the Media Bin or the Finder is loaded into the layer.
class DropZone : public QLabel
{
public:
    std::function<void(const QString &)> onDrop;
    explicit DropZone(const QString &text) : QLabel(text)
    {
        setAcceptDrops(true);
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setMinimumHeight(64);
        setHover(false);
    }

protected:
    void setHover(bool on)
    {
        setStyleSheet(on ? QStringLiteral("QLabel { border:2px dashed %1; border-radius:6px; background:%2;"
                                          " color:%3; padding:8px; }")
                               .arg(theme::css(), theme::css(40), theme::accent().lighter(130).name())
                         : QStringLiteral("QLabel { border:2px dashed #55555c; border-radius:6px; "
                                          "color:#9a9aa0; padding:8px; }"));
    }
    static QString firstFile(const QMimeData *m)
    {
        for (const QUrl &u : m->urls())
            if (u.isLocalFile()) return u.toLocalFile();
        return {};
    }
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (firstFile(e->mimeData()).isEmpty()) return e->ignore();
        setHover(true);
        e->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { setHover(false); }
    void dropEvent(QDropEvent *e) override
    {
        setHover(false);
        const QString f = firstFile(e->mimeData());
        e->acceptProposedAction();
        if (!f.isEmpty() && onDrop) onDrop(f);
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
    rebuild();
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
    if (p != cmd::SetLayerProp::Visible && p != cmd::SetLayerProp::Locked && m_engine->isLocked(m_layer)) return;
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

    const LayerSnapshot s = LayerSnapshot::take(m_engine, m_layer);
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
    const QString kind = s.isViewport ? QStringLiteral("Viewport") : s.isGroup ? QStringLiteral("Group") : QStringLiteral("Layer");
    emit kindChanged(kind);
    // Header: visibility, lock, name
    auto *head = new QHBoxLayout;
    auto *name = new QLineEdit(s.name);
    name->setStyleSheet("font-weight:bold; font-size:14px;");
    name->setToolTip(kind + QStringLiteral(" name"));
    auto *vis = new QCheckBox(QStringLiteral("Visible"));
    vis->setChecked(s.visible);
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
        setProp(cmd::SetLayerProp::Visible, on);
        emit layerChanged();
    });

    // Sub-tabs; the current one is kept from one layer to the next
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
    tabs->addTab(page(buildSource(s)), QStringLiteral("Source"));
    tabs->addTab(page(buildColor(s)), QStringLiteral("Color"));
    tabs->addTab(page(buildMapping(s)), QStringLiteral("Spatial"));
    tabs->addTab(page(buildEffects(s)), QStringLiteral("Effects"));
    tabs->addTab(page(buildCompositing(s)), QStringLiteral("Compositing"));
    m_output = nullptr;
    if (s.isViewport) { // its size, screen and publishing
        m_output = new ViewportOutputPanel(m_engine, s.id);
        connect(m_output, &ViewportOutputPanel::edited, this, &LayerInspector::projectEdited);
        connect(m_output, &ViewportOutputPanel::edited, this, &LayerInspector::layerChanged);
        tabs->addTab(page(m_output), QStringLiteral("Output"));
    }
    lockInputs(tabs, m_locked);
    name->setEnabled(!m_locked);
    if (s.type == SourceType::Audio) { // a sound has no picture: no color, mapping, effects or compositing
        for (int t = 1; t < tabs->count(); ++t) {
            tabs->setTabEnabled(t, false);
            tabs->setTabToolTip(t, QStringLiteral("An audio layer has no picture"));
        }
    }
    tabs->setCurrentIndex(tabs->isTabEnabled(m_subTab) ? m_subTab : 0);
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int i) {
        if (tabs->isTabEnabled(i)) m_subTab = i;
    });
    v->addWidget(tabs, 1);
    refreshDynamic();
}

QWidget *LayerInspector::buildSource(const LayerSnapshot &s)
{
    auto *g = new QWidget;
    auto *v = new QVBoxLayout(g);
    v->setContentsMargins(0, 0, 0, 0);

    if (s.isViewport) {
        auto *info = new QLabel(QStringLiteral("<b>Viewport</b> %1 × %2<br><span style='font-size:11px; color:#999'>"
                                               "A window onto the composition: it shows the region set in Spatial, "
                                               "at its own size (Output). The layers at the top of the list choose "
                                               "the viewports they appear in (Compositing).</span>")
                                    .arg(s.vpSize.width())
                                    .arg(s.vpSize.height()));
        info->setWordWrap(true);
        v->addWidget(info);
        v->addWidget(buildRoi(s));
        return g;
    }
    if (s.isGroup) {
        auto *info = new QLabel(QStringLiteral("<b>Group</b> of %1 layer(s)<br><span style='font-size:11px; color:#999'>"
                                               "Its picture is the composite of its layers. Drag layers onto it in the "
                                               "layer list to add them.</span>")
                                    .arg(s.members));
        info->setWordWrap(true);
        v->addWidget(info);
        v->addWidget(buildRoi(s));
        return g;
    }

    QString desc;
    switch (s.type) {
    case SourceType::Video: desc = QStringLiteral("Video — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Image: desc = QStringLiteral("Image — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Isf: desc = QStringLiteral("ISF Generator — %1").arg(QFileInfo(s.sourcePath).completeBaseName()); break;
    case SourceType::Audio: desc = QStringLiteral("Audio — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Text: desc = QStringLiteral("Text"); break;
    case SourceType::Layer: {
        QString from = QStringLiteral("(gone)");
        for (const auto &c : s.candidates)
            if (c.first == s.sourceLayer) from = c.second;
        desc = QStringLiteral("Layer — %1, %2").arg(from, s.sourceTap == LayerTap::PreFx ? QStringLiteral("pre-FX") : QStringLiteral("post-FX"));
        break;
    }
    default: desc = QStringLiteral("No source"); break;
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
        auto colorButton = [this, style](const QColor &c, const QString &label, std::function<void(Layer &, const QColor &)> set) {
            auto *b = new QPushButton;
            b->setFixedWidth(48);
            b->setEnabled(!m_locked);
            auto paint = [b](const QColor &x) {
                b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #666; border-radius: 2px;").arg(x.name()));
            };
            paint(c);
            connect(b, &QPushButton::clicked, this, [this, b, c, label, set, style, paint] {
                const QColor cur = b->property("color").value<QColor>().isValid() ? b->property("color").value<QColor>() : c;
                const QColor picked = QColorDialog::getColor(cur, this, label, QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                b->setProperty("color", picked);
                paint(picked);
                style(label, [set, picked](Layer &l) { set(l, picked); });
            });
            return b;
        };
        auto spin = [this](double v, double lo, double hi, double step, int dec, const QString &suffix) {
            auto *x = new QDoubleSpinBox;
            x->setRange(lo, hi);
            x->setSingleStep(step);
            x->setDecimals(dec);
            x->setSuffix(suffix);
            x->setValue(v);
            x->setEnabled(!m_locked);
            return x;
        };

        auto *editor = new QPlainTextEdit;
        editor->setPlainText(s.textContent);
        editor->setMinimumHeight(90);
        editor->setPlaceholderText(QStringLiteral("Type the text here"));
        editor->setToolTip(QStringLiteral("The text of this layer. A memory that holds another text types it (typewriter) over its fade."));
        editor->setReadOnly(m_locked);
        v->addWidget(editor);
        connect(editor, &QPlainTextEdit::textChanged, this, [this, editor, live] {
            live(QStringLiteral("Edit Text"), [this, editor] { m_engine->setLayerTextContent(m_layer, editor->toPlainText()); });
        });

        auto *fmt = new QFormLayout;
        fmt->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *fontBox = new QFontComboBox;
        fontBox->setCurrentFont(QFont(s.textFont));
        fontBox->setEnabled(!m_locked);
        fmt->addRow(QStringLiteral("Font"), fontBox);
        connect(fontBox, &QFontComboBox::currentFontChanged, this, [style](const QFont &f) {
            const QString family = f.family();
            style(QStringLiteral("Text Font"), [family](Layer &l) { l.textFont = family; });
        });

        auto *size = new QSpinBox;
        size->setRange(1, 1000);
        size->setSuffix(QStringLiteral(" px"));
        size->setValue(s.textSize);
        size->setEnabled(!m_locked);
        auto *colorRow = new QHBoxLayout;
        colorRow->addWidget(size);
        colorRow->addWidget(colorButton(s.textColor, QStringLiteral("Text Color"), [](Layer &l, const QColor &c) { l.textColor = c; }));
        colorRow->addStretch();
        fmt->addRow(QStringLiteral("Size / color"), colorRow);
        connect(size, QOverload<int>::of(&QSpinBox::valueChanged), this, [style](int px) {
            style(QStringLiteral("Text Size"), [px](Layer &l) { l.textSize = px; });
        });

        // Bold, italic, underline, strikethrough
        auto *styleRow = new QHBoxLayout;
        struct Toggle { const char *label, *tip; bool on; bool Layer::*field; };
        const Toggle toggles[] = {{"B", "Bold", s.textBold, &Layer::textBold},
                                  {"I", "Italic", s.textItalic, &Layer::textItalic},
                                  {"U", "Underline", s.textUnderline, &Layer::textUnderline},
                                  {"S", "Strikethrough", s.textStrike, &Layer::textStrike}};
        for (const Toggle &t : toggles) {
            auto *b = new QToolButton;
            b->setText(QString::fromLatin1(t.label));
            b->setToolTip(QString::fromLatin1(t.tip));
            b->setCheckable(true);
            b->setChecked(t.on);
            b->setEnabled(!m_locked);
            QFont f = b->font();
            f.setBold(t.label[0] == 'B');
            f.setItalic(t.label[0] == 'I');
            f.setUnderline(t.label[0] == 'U');
            f.setStrikeOut(t.label[0] == 'S');
            b->setFont(f);
            styleRow->addWidget(b);
            auto field = t.field;
            connect(b, &QToolButton::toggled, this, [style, field, tip = QString::fromLatin1(t.tip)](bool on) {
                style(tip, [field, on](Layer &l) { l.*field = on; });
            });
        }
        styleRow->addStretch();
        fmt->addRow(QStringLiteral("Style"), styleRow);

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
        const int h0 = hAlign->findData(int(s.textAlign & Qt::AlignHorizontal_Mask));
        const int v0 = vAlign->findData(int(s.textAlign & Qt::AlignVertical_Mask));
        hAlign->setCurrentIndex(h0 >= 0 ? h0 : 0);
        vAlign->setCurrentIndex(v0 >= 0 ? v0 : 0);
        hAlign->setEnabled(!m_locked);
        vAlign->setEnabled(!m_locked);
        auto *alignRow = new QHBoxLayout;
        alignRow->addWidget(hAlign);
        alignRow->addWidget(vAlign);
        fmt->addRow(QStringLiteral("Align"), alignRow);
        auto setAlign = [style, hAlign, vAlign] {
            const int a = hAlign->currentData().toInt() | vAlign->currentData().toInt();
            style(QStringLiteral("Text Alignment"), [a](Layer &l) { l.textAlign = Qt::Alignment(a); });
        };
        connect(hAlign, QOverload<int>::of(&QComboBox::activated), this, setAlign);
        connect(vAlign, QOverload<int>::of(&QComboBox::activated), this, setAlign);

        auto *lineSp = spin(s.textLineHeight, 0.2, 5.0, 0.05, 2, QStringLiteral(" ×"));
        lineSp->setToolTip(QStringLiteral("Space between the lines (1 = the font's own)"));
        fmt->addRow(QStringLiteral("Line spacing"), lineSp);
        connect(lineSp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Line Spacing"), [x](Layer &l) { l.textLineHeight = float(x); });
        });
        auto *letterSp = spin(s.textLetterSpacing, -50, 200, 0.5, 1, QStringLiteral(" px"));
        letterSp->setToolTip(QStringLiteral("Space added between the letters"));
        fmt->addRow(QStringLiteral("Letter spacing"), letterSp);
        connect(letterSp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Letter Spacing"), [x](Layer &l) { l.textLetterSpacing = float(x); });
        });

        // Outline
        auto *outline = spin(s.textOutline, 0, 100, 0.5, 1, QStringLiteral(" px"));
        auto *outlineRow = new QHBoxLayout;
        outlineRow->addWidget(outline);
        outlineRow->addWidget(colorButton(s.textOutlineColor, QStringLiteral("Outline Color"),
                                          [](Layer &l, const QColor &c) { l.textOutlineColor = c; }));
        outlineRow->addStretch();
        fmt->addRow(QStringLiteral("Outline"), outlineRow);
        connect(outline, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Outline"), [x](Layer &l) { l.textOutline = float(x); });
        });

        // Shadow
        auto *shadow = new QCheckBox(QStringLiteral("On"));
        shadow->setChecked(s.textShadow);
        shadow->setEnabled(!m_locked);
        auto *shadowRow = new QHBoxLayout;
        shadowRow->addWidget(shadow);
        shadowRow->addWidget(colorButton(s.textShadowColor, QStringLiteral("Shadow Color"),
                                         [](Layer &l, const QColor &c) { l.textShadowColor = c; }));
        auto *sx = spin(s.textShadowX, -200, 200, 1, 0, QStringLiteral(" x"));
        auto *sy = spin(s.textShadowY, -200, 200, 1, 0, QStringLiteral(" y"));
        shadowRow->addWidget(sx);
        shadowRow->addWidget(sy);
        fmt->addRow(QStringLiteral("Shadow"), shadowRow);
        connect(shadow, &QCheckBox::toggled, this, [style](bool on) {
            style(QStringLiteral("Text Shadow"), [on](Layer &l) { l.textShadow = on; });
        });
        connect(sx, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Shadow"), [x](Layer &l) { l.textShadowX = float(x); });
        });
        connect(sy, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [style](double x) {
            style(QStringLiteral("Text Shadow"), [x](Layer &l) { l.textShadowY = float(x); });
        });

        v->addLayout(fmt);
        auto *eject = new QPushButton(QStringLiteral("Eject Text"));
        eject->setToolTip(QStringLiteral("Empty the layer (the text generator stays in the Media Bin)"));
        eject->setEnabled(!m_locked);
        v->addWidget(eject);
        connect(eject, &QPushButton::clicked, this, [this] {
            editSource(QStringLiteral("Eject Text"), [this] { m_engine->clearLayerSource(m_layer); });
            emit layerChanged();
            rebuild();
        });
        v->addStretch();
        return g;
    }

    // Drop zone: the current media, replaced by whatever is dropped (Media Bin, Finder). × ejects it.
    const bool loaded = s.type != SourceType::None;
    auto *zoneRow = new QHBoxLayout;
    auto *zone = new DropZone(loaded ? QStringLiteral("<b>%1</b><br><span style='font-size:11px'>Drop another media here to replace it</span>")
                                           .arg(desc.toHtmlEscaped())
                                     : QStringLiteral("Drop a video, image, sound or ISF generator here<br>"
                                                      "<span style='font-size:11px'>from the Media Bin or the Finder</span>"));
    zone->setTextFormat(Qt::RichText);
    zone->setToolTip(s.sourcePath.isEmpty() ? QStringLiteral("Video, image, audio file or ISF generator") : s.sourcePath);
    zone->onDrop = [this](const QString &f) { emit fileDropped(f); };
    auto *bClear = toolButton(QStringLiteral("×"), QStringLiteral("Eject the media from the layer"));
    bClear->setEnabled(loaded || s.error.size());
    zoneRow->addWidget(zone, 1);
    zoneRow->addWidget(bClear, 0, Qt::AlignTop);
    v->addLayout(zoneRow);
    connect(bClear, &QToolButton::clicked, this, [this] {
        editSource(QStringLiteral("Eject Media"), [this] { m_engine->clearLayerSource(m_layer); });
        emit layerChanged();
        rebuild();
    });

    // Or the picture of another layer, tapped before or after its effect chain
    {
        auto *row = new QHBoxLayout;
        auto *pick = new QComboBox;
        pick->setToolTip(QStringLiteral("Use the picture of another layer as this layer's source"));
        pick->addItem(QStringLiteral("Use a layer…"), QVariant(qulonglong(0)));
        int current = 0;
        for (const auto &c : s.candidates) {
            pick->addItem(c.second, QVariant(qulonglong(c.first)));
            if (s.type == SourceType::Layer && c.first == s.sourceLayer) current = pick->count() - 1;
        }
        pick->setCurrentIndex(current);
        auto *tap = new QComboBox;
        tap->addItem(QStringLiteral("Pre-FX"), int(LayerTap::PreFx));
        tap->addItem(QStringLiteral("Post-FX"), int(LayerTap::PostFx));
        tap->setCurrentIndex(s.sourceTap == LayerTap::PreFx ? 0 : 1);
        tap->setToolTip(QStringLiteral("Where the picture is taken in that layer:\n"
                                       "Pre-FX — after its ROI and color, before its effects\n"
                                       "Post-FX — after its effects"));
        tap->setEnabled(s.type == SourceType::Layer);
        row->addWidget(pick, 1);
        row->addWidget(tap, 0);
        v->addLayout(row);
        connect(pick, &QComboBox::activated, this, [this, pick, tap](int i) {
            const quint64 id = pick->itemData(i).toULongLong();
            if (!id) return;
            const LayerTap t = LayerTap(tap->currentData().toInt());
            QString err;
            bool ok = false;
            editSource(QStringLiteral("Use Layer as Source"),
                       [this, id, t, &err, &ok] { ok = m_engine->setLayerSourceLayer(m_layer, id, t, &err); });
            if (!ok && !err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Source"), err);
            emit layerChanged();
            rebuild();
        });
        connect(tap, &QComboBox::activated, this, [this, tap](int i) {
            editSource(QStringLiteral("Change Source Tap"),
                       [this, tap, i] { m_engine->setLayerTap(m_layer, LayerTap(tap->itemData(i).toInt())); });
            emit layerChanged();
            rebuild();
        });
        if (s.candidates.empty()) {
            pick->setEnabled(false);
            pick->setToolTip(QStringLiteral("No other layer can be used here without the picture feeding back on itself"));
        }
    }

    // Transition when a memory gives this layer another source
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
        pick->setToolTip(QStringLiteral("When a memory gives this layer another source, the outgoing one keeps playing and "
                                        "this ISF transition takes it to the new one, over the memory's fade "
                                        "(or the time the memory gives the source)"));
        row->addWidget(label);
        row->addWidget(pick, 1);
        v->addLayout(row);
        connect(pick, &QComboBox::activated, this,
                [this, pick](int i) { setProp(cmd::SetLayerProp::Transition, pick->itemData(i).toString()); });
    }

    if (!s.error.isEmpty()) v->addWidget(errorLabel(s.error));

    auto audioText = [](const AudioStream::Info &a) {
        const QString ch = a.channels == 1 ? QStringLiteral("mono") : a.channels == 2 ? QStringLiteral("stereo")
                                                                                        : QStringLiteral("%1 ch").arg(a.channels);
        return QStringLiteral("%1 · %2 kHz · %3").arg(a.codec).arg(a.sampleRate / 1000.0, 0, 'g', 3).arg(ch);
    };
    const bool media = (s.type == SourceType::Video && s.hasVideo) || (s.type == SourceType::Audio && s.hasAudio);
    if (media) {
        // What the media is
        auto *facts = new QFormLayout;
        facts->setContentsMargins(0, 2, 0, 2);
        facts->setHorizontalSpacing(10);
        facts->setVerticalSpacing(2);
        facts->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto fact = [&](const QString &name, const QString &value) {
            auto *k = new QLabel(name);
            k->setStyleSheet("color:#8a8a8e;");
            auto *val = new QLabel(value);
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            facts->addRow(k, val);
            return val;
        };
        fact(QStringLiteral("Name"), QFileInfo(s.sourcePath).fileName());
        if (s.type == SourceType::Video) {
            fact(QStringLiteral("Resolution"), QStringLiteral("%1 × %2").arg(s.videoW).arg(s.videoH));
            fact(QStringLiteral("FPS"), QString::number(s.fps, 'f', 2));
        }
        fact(QStringLiteral("Duration"), fmtTime(s.duration));
        m_codecFact = fact(QStringLiteral("Codec"), s.type == SourceType::Video ? s.codec : s.audio.codec);
        if (s.type == SourceType::Video) {
            // How the frames reach the GPU: their layout, or HAP's textures, or a conversion on the CPU
            m_pictureFact = fact(QStringLiteral("Picture"), QStringLiteral("…"));
            m_pictureFact->setToolTip(QStringLiteral("How the decoded frames reach the GPU. A pixel layout (yuv420p, nv12, "
                                                     "p010…) or HAP textures are converted by the GPU; \"converted on "
                                                     "the CPU\" or \"decoded on the CPU\" costs processor time."));
        } else {
            m_codecFact = nullptr;
        }
        fact(QStringLiteral("Sound"), s.hasAudio ? audioText(s.audio) : QStringLiteral("none"));
        v->addLayout(facts);
        v->addWidget(separator());

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
        rowLabel(1, QStringLiteral("Mode"));
        auto *modeRow = new QHBoxLayout;
        modeRow->setSpacing(2);
        auto *modes = new QButtonGroup(g);
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

        // Position, speed and played range: one bar each, dragged or typed
        auto *bars = new QGridLayout;
        bars->setHorizontalSpacing(8);
        bars->setVerticalSpacing(4);
        bars->setColumnStretch(1, 1);
        auto barLabel = [&](int row, const QString &text, std::function<void()> reset) {
            auto *l = new ResetLabel(text, std::move(reset));
            l->setStyleSheet("color:#8a8a8e;");
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            bars->addWidget(l, row, 0);
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

        m_speed = new SliderField;
        m_speed->setRange(-200, 200);        // the bar: the speeds actually used
        m_speed->setTypedRange(-800, 800); // faster or more backwards: typed
        m_speed->setDecimals(0);
        m_speed->setSuffix(QStringLiteral(" %"));
        m_speed->setSingleStep(5);
        m_speed->setOrigin(0); // the fill grows either side of a standstill
        m_speed->setTicks(8);
        m_speed->setSnaps({-200, -100, 0, 100, 200});
        m_speed->setValue(s.speed * 100);
        m_speed->setToolTip(QStringLiteral("Playback speed — below 0 the media plays backwards, 100 % is its own rate"));
        barLabel(1, QStringLiteral("Speed"), [this] { setProp(cmd::SetLayerProp::Speed, 1.0); m_speed->setValue(100); });
        bars->addWidget(m_speed, 1, 1);

        m_loop = new RangeField;
        m_loop->setRange(0, std::max(0.01, s.duration));
        m_loop->setDecimals(2);
        m_loop->setSuffix(QStringLiteral(" s"));
        m_loop->setValues(s.inPoint, s.outPoint < 0 ? s.duration : s.outPoint);
        m_loop->setToolTip(QStringLiteral("Played range: playback, loops and ping-pong stay between these two points"));
        const double duration0 = s.duration;
        barLabel(2, QStringLiteral("Loop"), [this] {
            m_undo->beginMacro(QStringLiteral("Clear In / Out Points"));
            setProp(cmd::SetLayerProp::InPoint, 0.0);
            setProp(cmd::SetLayerProp::OutPoint, -1.0);
            m_undo->endMacro();
            rebuild();
        });
        bars->addWidget(m_loop, 2, 1);
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
        connect(m_speed, &SliderField::valueEdited, this, [this](double pct) {
            setProp(cmd::SetLayerProp::Speed, pct / 100.0);
            emit layerChanged();
        });
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
        auto *mute = new QCheckBox(QStringLiteral("Mute"));
        mute->setChecked(s.muted);
        row->addWidget(icon);
        row->addWidget(vol, 1);
        row->addWidget(mute);
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
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, -1);
        m_generatorParams = params;
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    if (s.type == SourceType::Video || s.type == SourceType::Image || s.type == SourceType::Isf || s.type == SourceType::Text) v->addWidget(buildRoi(s));
    return g;
}

// Part of the source picture used: preview with a rectangle whose sides are dragged, numeric fields in %
QWidget *LayerInspector::buildRoi(const LayerSnapshot &s)
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

QWidget *LayerInspector::buildColor(const LayerSnapshot &s)
{
    auto *g = new QWidget;
    auto *outer = new QVBoxLayout(g);
    outer->setContentsMargins(0, 0, 0, 0);
    // Switch of the whole section: the values are kept, they are simply not applied
    auto *master = new QCheckBox(QStringLiteral("Color"));
    master->setChecked(s.color.enabled);
    master->setStyleSheet("font-weight:bold;");
    master->setToolTip(QStringLiteral("Apply the color of this layer.\nOff: every value is kept, the picture is left alone."));
    outer->addWidget(master);
    auto *body = new QWidget; // everything the switch above turns off
    auto *v = new QVBoxLayout(body);
    v->setContentsMargins(0, 0, 0, 0);
    body->setEnabled(s.color.enabled);
    outer->addWidget(body);
    connect(master, &QCheckBox::toggled, this, [this, body](bool on) {
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
        auto *inv = new QCheckBox(QStringLiteral("Invert"));
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
            const char *name;
            double range, value;
            QGradientStops gradient;
            int prop;
            QPointer<SliderField> *field;
            bool on;
            int onProp;
        } defs[] = {{"Temp", ColorAdjust::kTempRange, s.color.temp,
                     {{0.0, QColor("#3a7bff")}, {0.5, QColor("#888888")}, {1.0, QColor("#ffd23a")}},
                     cmd::SetLayerProp::Temp, &m_temp, s.color.tempOn, cmd::SetLayerProp::TempOn},
                    {"Tint", ColorAdjust::kTintRange, s.color.tint,
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
            auto *on = new QCheckBox;
            on->setChecked(d.on);
            on->setToolTip(QStringLiteral("Apply %1 (the value is kept either way)").arg(QString::fromUtf8(d.name)));
            const int onProp = d.onProp;
            connect(on, &QCheckBox::toggled, this, [this, onProp](bool v) { setProp(onProp, v); });
            grid->addWidget(on, row, 0);
            grid->addWidget(new ResetLabel(QString::fromUtf8(d.name), [bar] {
                                bar->setValue(0);
                                emit bar->valueEdited(0);
                            }),
                            row, 1);
            grid->addWidget(bar, row, 2, 1, 2);
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
    m_colorAdd->setSwitch(true, s.color.addOn, QStringLiteral("Apply the added color (it is kept either way)"));
    m_colorRemove->setSwitch(true, s.color.removeOn, QStringLiteral("Apply the removed color (it is kept either way)"));
    connect(m_colorAdd, &ColorEditor::colorEdited, this, [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorAdd, c); });
    connect(m_colorRemove, &ColorEditor::colorEdited, this,
            [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorRemove, c); });
    connect(m_colorAdd, &ColorEditor::switchToggled, this, [this](bool on) { setProp(cmd::SetLayerProp::AddOn, on); });
    connect(m_colorRemove, &ColorEditor::switchToggled, this, [this](bool on) { setProp(cmd::SetLayerProp::RemoveOn, on); });
    return g;
}

QWidget *LayerInspector::buildCompositing(const LayerSnapshot &s)
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
    form->addRow(new ResetLabel(QStringLiteral("Opacity"), [this, opacity] {
                     opacity->setValue(100);
                     setProp(cmd::SetLayerProp::Opacity, 1.0);
                 }),
                 opacity);
    connect(opacity, &SliderField::valueEdited, this, [this](double v) { setProp(cmd::SetLayerProp::Opacity, v / 100.0); });
    if (s.isViewport) return g; // drawn onto nothing: no blend, no routing

    auto *blend = new QComboBox;
    for (BlendMode m : {BlendMode::Normal, BlendMode::Add, BlendMode::Screen, BlendMode::Multiply, BlendMode::Subtract,
                        BlendMode::Difference})
        blend->addItem(blendModeName(m), int(m));
    blend->setCurrentIndex(blend->findData(int(s.blend)));
    form->addRow(new ResetLabel(QStringLiteral("Blend"), [blend] { blend->setCurrentIndex(0); }), blend);
    connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, blend](int i) { setProp(cmd::SetLayerProp::Blend, blend->itemData(i).toInt()); });

    // Viewports it appears in: chosen at the top of the list (a group for everything inside it)
    auto *routes = new QWidget;
    auto *rv = new QVBoxLayout(routes);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(2);
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
        auto *name = new QLabel(r.name);
        name->setMinimumWidth(70);
        auto *sl = new SliderField;
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
    form->addRow(new QLabel(QStringLiteral("Viewports")), routes);
    return g;
}

QWidget *LayerInspector::buildMapping(const LayerSnapshot &s)
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
        m_posX = spin(-100000, 100000, QStringLiteral(" px"), 1);
        m_posY = spin(-100000, 100000, QStringLiteral(" px"), 1);
        m_scaleX = spin(0.1, 10000, QStringLiteral(" %"), 2);
        m_scaleY = spin(0.1, 10000, QStringLiteral(" %"), 2);
        m_posX->setToolTip(QStringLiteral("Horizontal position of the layer's center, in composition pixels"));
        m_posY->setToolTip(QStringLiteral("Vertical position of the layer's center, in composition pixels"));
        m_scaleX->setToolTip(QStringLiteral("Width of the layer, in % of the composition width"));
        m_scaleY->setToolTip(QStringLiteral("Height of the layer, in % of the composition height"));
        auto *link = new QToolButton;
        link->setCheckable(true);
        link->setChecked(m_scaleLinked);
        link->setText(QStringLiteral("⛓"));
        link->setToolTip(QStringLiteral("Link width and height (keep the aspect ratio)"));
        link->setStyleSheet(QStringLiteral("QToolButton:checked { background:%1; color:%2; }")
                                .arg(theme::css(), theme::onAccent().name()));
        auto *posLabel = new ResetLabel(QStringLiteral("Position"), [this, comp] {
            m_posX->setValue(comp.width() / 2.0); // centered
            m_posY->setValue(comp.height() / 2.0);
        });
        auto *scaleLabel = new ResetLabel(QStringLiteral("Scale"), [this] {
            const bool linked = m_scaleLinked;
            m_scaleLinked = false;
            m_scaleX->setValue(100.0);
            m_scaleY->setValue(100.0);
            m_scaleLinked = linked;
        });
        grid->addWidget(posLabel, 0, 0);
        grid->addWidget(new QLabel(QStringLiteral("X")), 0, 1);
        grid->addWidget(m_posX, 0, 2);
        grid->addWidget(new QLabel(QStringLiteral("Y")), 0, 4);
        grid->addWidget(m_posY, 0, 5);
        grid->addWidget(scaleLabel, 1, 0);
        grid->addWidget(new QLabel(QStringLiteral("X")), 1, 1);
        grid->addWidget(m_scaleX, 1, 2);
        grid->addWidget(link, 1, 3);
        grid->addWidget(new QLabel(QStringLiteral("Y")), 1, 4);
        grid->addWidget(m_scaleY, 1, 5);
        grid->setColumnStretch(2, 1);
        grid->setColumnStretch(5, 1);
        v->addLayout(grid);
        refreshSpatial();

        connect(link, &QToolButton::toggled, this, [this](bool on) { m_scaleLinked = on; });
        auto applyBounds = [this, comp](const QString &text, const std::function<QRectF(QRectF)> &fn) {
            editMapping(text, [&](Mapping &m) {
                QRectF b = m.bounds();
                // in composition pixels
                b = QRectF(b.left() * comp.width(), b.top() * comp.height(), b.width() * comp.width(), b.height() * comp.height());
                b = fn(b);
                m.setBounds(QRectF(b.left() / comp.width(), b.top() / comp.height(), b.width() / comp.width(),
                                   b.height() / comp.height()));
            }, true);
        };
        connect(m_posX, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applyBounds](double x) {
            applyBounds(QStringLiteral("Position"), [x](QRectF b) { b.moveCenter(QPointF(x, b.center().y())); return b; });
        });
        connect(m_posY, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [applyBounds](double y) {
            applyBounds(QStringLiteral("Position"), [y](QRectF b) { b.moveCenter(QPointF(b.center().x(), y)); return b; });
        });
        auto scale = [this, applyBounds, comp](double sx, double sy, bool fromX) {
            applyBounds(QStringLiteral("Scale"), [&](QRectF b) {
                const QPointF c = b.center();
                double w = sx / 100.0 * comp.width(), h = sy / 100.0 * comp.height();
                if (m_scaleLinked) { // the other axis follows, keeping the aspect ratio
                    if (fromX && b.width() > 1e-9) h = b.height() * w / b.width();
                    if (!fromX && b.height() > 1e-9) w = b.width() * h / b.height();
                }
                if (fromX) h = m_scaleLinked ? h : b.height();
                else w = m_scaleLinked ? w : b.width();
                b.setSize(QSizeF(w, h));
                b.moveCenter(c);
                return b;
            });
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
                QRectF b = m.bounds();
                const QPointF center = b.center();
                b.setSize(QSizeF(double(vs.width()) / c.width(), double(vs.height()) / c.height()));
                b.moveCenter(center);
                m.setBounds(b);
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
        auto *box = new QGroupBox(QStringLiteral("Soft edge"));
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
            grid->addWidget(new ResetLabel(QString::fromLatin1(names[side]), [width, power] {
                                width->setValue(10.0);
                                power->setValue(1.0);
                                emit width->valueEdited(10.0);
                                emit power->valueEdited(1.0);
                            }),
                            side + 1, 0);
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

QWidget *LayerInspector::buildEffects(const LayerSnapshot &s)
{
    auto *g = new QWidget; // titled by its sub-tab
    auto *v = new QVBoxLayout(g);

    // General switch of the chain
    auto *all = new QCheckBox(QStringLiteral("Effects enabled"));
    all->setChecked(s.effectsEnabled);
    all->setToolTip(QStringLiteral("Turns the whole effect chain on or off (each effect keeps its own switch)"));
    all->setStyleSheet("QCheckBox { font-weight:bold; }");
    v->addWidget(all);
    connect(all, &QCheckBox::toggled, this, [this](bool on) {
        setProp(cmd::SetLayerProp::EffectsEnabled, on);
        emit layerChanged();
    });

    auto *bar = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add Effect…"));
    add->setToolTip(QStringLiteral("Search the ISF library and add an effect to the chain"));
    connect(add, &QPushButton::clicked, this, [this, add] {
        QList<SearchPicker::Item> items;
        for (const IsfEntry &e : m_engine->library().filters())
            items.append({e.name, e.categories.value(0), e.description, e.path});
        auto *picker = new SearchPicker(items, this);
        connect(picker, &SearchPicker::picked, this, [this](const QString &p) {
            editEffects(QStringLiteral("Add Effect %1").arg(QFileInfo(p).completeBaseName()), [this, p] {
                QString err;
                m_selectedEffect = m_engine->addEffect(m_layer, p, &err);
            });
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        });
        picker->popup(add->mapToGlobal(QPoint(0, add->height())));
    });
    auto *remove = toolButton(QStringLiteral("−"), QStringLiteral("Remove Effect"));
    auto *up = toolButton(QStringLiteral("▲"), QStringLiteral("Move Up (applied earlier)"));
    auto *down = toolButton(QStringLiteral("▼"), QStringLiteral("Move Down (applied later)"));
    auto *reload = toolButton(QStringLiteral("⟳"), QStringLiteral("Reload shader from disk"));
    bar->addWidget(add, 1);
    bar->addWidget(remove);
    bar->addWidget(up);
    bar->addWidget(down);
    bar->addWidget(reload);
    v->addLayout(bar);

    if (s.effects.empty()) {
        auto *none = new QLabel(QStringLiteral("No effects. Effects are applied in list order."));
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
        editEffects(on ? QStringLiteral("Enable Effect") : QStringLiteral("Disable Effect"), [this, r, on] {
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
        editEffects(QStringLiteral("Remove Effect"), [this] { m_engine->removeEffect(m_layer, m_selectedEffect); });
        rebuild();
    });
    connect(up, &QToolButton::clicked, this, [this] {
        if (m_selectedEffect <= 0) return;
        editEffects(QStringLiteral("Reorder Effects"),
                    [this] { m_engine->moveEffect(m_layer, m_selectedEffect, m_selectedEffect - 1); });
        --m_selectedEffect;
        rebuild();
    });
    connect(down, &QToolButton::clicked, this, [this, count] {
        if (m_selectedEffect >= count - 1) return;
        editEffects(QStringLiteral("Reorder Effects"),
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
    bool invert = false;
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
            editEffects(QStringLiteral("Remove Effect Mask"), [this, k] { m_engine->setEffectMask(m_layer, k, 0, false); });
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
        pick->setToolTip(QStringLiteral("Where this effect applies: fully where the chosen layer's picture is white, not at "
                                        "all where it is black or transparent, in proportion in between. That picture is "
                                        "stretched over this layer's, before the mapping. The layer can stay hidden."));
        auto *inv = new QCheckBox(QStringLiteral("Invert"));
        inv->setChecked(invert);
        inv->setEnabled(maskId != 0);
        row->addWidget(label);
        row->addWidget(pick, 1);
        row->addWidget(inv);
        lay->addLayout(row);
        connect(pick, &QComboBox::activated, this, [this, pick, inv](int i) {
            const quint64 id = pick->itemData(i).toULongLong();
            const int k = m_selectedEffect;
            QString err;
            bool ok = true;
            editEffects(id ? QStringLiteral("Set Effect Mask") : QStringLiteral("Remove Effect Mask"),
                        [&] { ok = m_engine->setEffectMask(m_layer, k, id, inv->isChecked(), &err); });
            if (!ok && !err.isEmpty()) QMessageBox::warning(this, QStringLiteral("Mask"), err);
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
        });
        connect(inv, &QCheckBox::toggled, this, [this, pick](bool on) {
            const quint64 id = pick->currentData().toULongLong();
            const int k = m_selectedEffect;
            editEffects(QStringLiteral("Invert Effect Mask"), [&] { m_engine->setEffectMask(m_layer, k, id, on); });
        });
    }
    if (valid) {
        auto *params = new ParamPanel(m_engine, m_undo, m_layer, m_selectedEffect);
        m_effectParams = params;
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        lay->addWidget(params);
    }
    lockInputs(m_fxDetail, m_locked);
}

void LayerInspector::refreshSpatial()
{
    if (!m_posX) return;
    QRectF b;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l) return;
        b = l->mapping.bounds();
    }
    const QSize comp = m_engine->compositionSize();
    const double values[4] = {b.center().x() * comp.width(), b.center().y() * comp.height(), b.width() * 100.0,
                              b.height() * 100.0};
    QDoubleSpinBox *boxes[4] = {m_posX, m_posY, m_scaleX, m_scaleY};
    for (int k = 0; k < 4; ++k) {
        if (boxes[k]->hasFocus()) continue; // being edited
        if (std::abs(boxes[k]->value() - values[k]) < 1e-6) continue;
        QSignalBlocker blk(boxes[k]);
        boxes[k]->setValue(values[k]);
    }
}

void LayerInspector::refreshDynamic()
{
    // The fields show the values as they move (a memory's fade), unless Settings keeps them still until it is over
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
    // Values changed elsewhere (OSC, undo, a memory) follow in the fields
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
        if (l->video && m_codecFact) {
            const QString codec = l->video->codecName();
            if (m_codecFact->text() != codec) m_codecFact->setText(codec);
        }
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
