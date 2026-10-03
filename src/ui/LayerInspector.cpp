#include "LayerInspector.h"
#include "Commands.h"
#include "Engine.h"
#include "ParamPanel.h"
#include "SettingsPanel.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTabWidget>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
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
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>
#include <cmath>

// Copy of a layer's state taken under the lock: widgets are built afterwards without blocking rendering.
struct LayerSnapshot {
    bool valid = false;
    quint64 id = 0;
    bool isGroup = false, locked = false, lockedByGroup = false;
    int members = 0;
    QRectF crop{0, 0, 1, 1};
    ColorAdjust color;
    bool effectsEnabled = true;
    double aspect = 16.0 / 9.0; // source picture (before crop)
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
        s.id = l->id;
        s.isGroup = l->isGroup;
        s.locked = l->locked;
        s.lockedByGroup = !l->locked && e->isLocked(index);
        s.members = e->groupMembers(index).size();
        s.crop = l->crop;
        s.color = l->color;
        s.effectsEnabled = l->effectsEnabled;
        const QSize comp = e->compositionSize();
        const int sw = l->isGroup ? comp.width() : l->sourceWidth(), sh = l->isGroup ? comp.height() : l->sourceHeight();
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
        setStyleSheet(on ? "QLabel { border:2px dashed #ffa028; border-radius:6px; background:rgba(255,160,40,40);"
                           " color:#ffd9a8; padding:8px; }"
                         : "QLabel { border:2px dashed #55555c; border-radius:6px; color:#9a9aa0; padding:8px; }");
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
    // Header: visibility, lock, name
    auto *head = new QHBoxLayout;
    auto *name = new QLineEdit(s.name);
    name->setStyleSheet("font-weight:bold; font-size:14px;");
    name->setToolTip(s.isGroup ? QStringLiteral("Group name") : QStringLiteral("Layer name"));
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
    tabs->addTab(page(buildSource(s)), QStringLiteral("Source"));
    tabs->addTab(page(buildColor(s)), QStringLiteral("Color"));
    tabs->addTab(page(buildMapping(s)), QStringLiteral("Spatial"));
    tabs->addTab(page(buildEffects(s)), QStringLiteral("Effects"));
    tabs->addTab(page(buildCompositing(s)), QStringLiteral("Compositing"));
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

    if (s.isGroup) {
        auto *info = new QLabel(QStringLiteral("<b>Group</b> of %1 layer(s)<br><span style='font-size:11px; color:#999'>"
                                               "Its picture is the composite of its layers. Drag layers onto it in the "
                                               "layer list to add them.</span>")
                                    .arg(s.members));
        info->setWordWrap(true);
        v->addWidget(info);
        v->addWidget(buildCrop(s));
        return g;
    }

    QString desc;
    switch (s.type) {
    case SourceType::Video: desc = QStringLiteral("Video — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Image: desc = QStringLiteral("Image — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    case SourceType::Isf: desc = QStringLiteral("ISF Generator — %1").arg(QFileInfo(s.sourcePath).completeBaseName()); break;
    case SourceType::Audio: desc = QStringLiteral("Audio — %1").arg(QFileInfo(s.sourcePath).fileName()); break;
    default: desc = QStringLiteral("No source"); break;
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

    if (!s.error.isEmpty()) v->addWidget(errorLabel(s.error));

    auto audioText = [](const AudioStream::Info &a) {
        const QString ch = a.channels == 1 ? QStringLiteral("mono") : a.channels == 2 ? QStringLiteral("stereo")
                                                                                        : QStringLiteral("%1 ch").arg(a.channels);
        return QStringLiteral("%1 · %2 kHz · %3").arg(a.codec).arg(a.sampleRate / 1000.0, 0, 'g', 3).arg(ch);
    };
    const bool media = (s.type == SourceType::Video && s.hasVideo) || (s.type == SourceType::Audio && s.hasAudio);
    if (media) {
        QString text;
        if (s.type == SourceType::Video) {
            text = QStringLiteral("%1 × %2 · %3 fps · %4 · %5")
                       .arg(s.videoW)
                       .arg(s.videoH)
                       .arg(s.fps, 0, 'f', 2)
                       .arg(s.codec)
                       .arg(fmtTime(s.duration));
            text += s.hasAudio ? QStringLiteral("\nSound: ") + audioText(s.audio) : QStringLiteral("\nNo sound");
        } else {
            text = audioText(s.audio) + QStringLiteral(" · ") + fmtTime(s.duration);
        }
        auto *info = new QLabel(text);
        info->setStyleSheet("color:#999; font-size:11px;");
        v->addWidget(info);

        auto *transport = new QHBoxLayout;
        m_play = new QPushButton(s.playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
        m_play->setProperty("allowLocked", true);
        auto *rewind = toolButton(QStringLiteral("⏮"), QStringLiteral("Back to Start"));
        rewind->setProperty("allowLocked", true);
        auto *speed = new QDoubleSpinBox;
        auto *speedLabel = new ResetLabel(QStringLiteral("Speed"), [speed] { speed->setValue(1.0); });
        speed->setRange(-8.0, 8.0); // negative: backwards
        speed->setSingleStep(0.05);
        speed->setValue(s.speed);
        speed->setSuffix(QStringLiteral(" ×"));
        speed->setToolTip(QStringLiteral("Playback speed — negative values play backwards"));
        transport->addWidget(m_play);
        transport->addWidget(rewind);
        transport->addStretch();
        transport->addWidget(speedLabel);
        transport->addWidget(speed);
        v->addLayout(transport);

        // Play mode: four exclusive buttons, one is always selected
        auto *modes = new QHBoxLayout;
        modes->setSpacing(2);
        auto *group = new QButtonGroup(g);
        group->setExclusive(true);
        const struct {
            PlayMode mode;
            const char *tip;
        } kModes[] = {
            {PlayMode::OneShot, "Plays once and freezes on the last frame"},
            {PlayMode::Loop, "Starts again from the beginning"},
            {PlayMode::PingPong, "Plays forwards, then backwards, and so on"},
            {PlayMode::Stop, "Plays once, then goes black (and silent)"},
        };
        for (const auto &m : kModes) {
            auto *b = new QToolButton;
            b->setText(playModeName(m.mode));
            b->setToolTip(QString::fromUtf8(m.tip));
            b->setCheckable(true);
            b->setChecked(s.mode == m.mode);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setStyleSheet("QToolButton:checked { background:#ffa028; color:#1b1b1d; font-weight:bold; }");
            group->addButton(b, int(m.mode));
            modes->addWidget(b);
        }
        v->addLayout(modes);
        connect(group, &QButtonGroup::idClicked, this, [this](int id) {
            setProp(cmd::SetLayerProp::Mode, id);
            emit layerChanged();
        });

        auto *seekRow = new QHBoxLayout;
        m_seek = new SeekBar;
        m_seek->setDuration(s.duration);
        m_seek->setInOut(s.inPoint, s.outPoint);
        m_seek->setProperty("allowLocked", true); // seeking is not an edit; the markers are disabled when locked
        m_seek->setMarkersEditable(!m_locked);
        m_time = new QLabel;
        m_time->setStyleSheet("font-family:monospace;");
        seekRow->addWidget(m_seek, 1);
        seekRow->addWidget(m_time);
        v->addLayout(seekRow);

        // In / out points: the played range (loops and ping-pong stay within it). Keys I / O.
        auto *range = new QHBoxLayout;
        range->setSpacing(4);
        auto timeBox = [&](double value) {
            auto *b = new QDoubleSpinBox;
            b->setRange(0, std::max(0.01, s.duration));
            b->setDecimals(2);
            b->setSingleStep(1.0 / std::max(1.0, s.fps > 0 ? s.fps : 25.0));
            b->setSuffix(QStringLiteral(" s"));
            b->setKeyboardTracking(false);
            b->setValue(value);
            return b;
        };
        auto *inBox = timeBox(s.inPoint);
        auto *outBox = timeBox(s.outPoint < 0 ? s.duration : s.outPoint);
        inBox->setToolTip(QStringLiteral("In point: playback starts here (I sets it at the current position)"));
        outBox->setToolTip(QStringLiteral("Out point: playback ends here (O sets it at the current position)"));
        auto *setIn = toolButton(QStringLiteral("Set"), QStringLiteral("In point at the current position (I)"));
        auto *setOut = toolButton(QStringLiteral("Set"), QStringLiteral("Out point at the current position (O)"));
        auto *resetRange = toolButton(QStringLiteral("↺"), QStringLiteral("Whole media (clear in / out points)"));
        range->addWidget(new ResetLabel(QStringLiteral("In"), [inBox] { inBox->setValue(0); }));
        range->addWidget(inBox, 1);
        range->addWidget(setIn);
        range->addSpacing(8);
        range->addWidget(new ResetLabel(QStringLiteral("Out"), [outBox] { outBox->setValue(outBox->maximum()); }));
        range->addWidget(outBox, 1);
        range->addWidget(setOut);
        range->addWidget(resetRange);
        v->addLayout(range);
        const double duration0 = s.duration;
        connect(inBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double t) { setProp(cmd::SetLayerProp::InPoint, t); });
        connect(outBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, duration0](double t) {
            setProp(cmd::SetLayerProp::OutPoint, t >= duration0 - 1e-6 ? -1.0 : t);
        });
        connect(setIn, &QToolButton::clicked, this, [this] { emit setInOutRequested(true); });
        connect(setOut, &QToolButton::clicked, this, [this] { emit setInOutRequested(false); });
        connect(resetRange, &QToolButton::clicked, this, [this] {
            m_undo->beginMacro(QStringLiteral("Clear In / Out Points"));
            setProp(cmd::SetLayerProp::InPoint, 0.0);
            setProp(cmd::SetLayerProp::OutPoint, -1.0);
            m_undo->endMacro();
            rebuild();
        });

        // Playback is not a project edit: no undo.
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
        connect(speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double sp) { setProp(cmd::SetLayerProp::Speed, sp); });
        connect(m_seek, &SeekBar::seekRequested, this, [this](double t) { m_engine->seekLayer(m_layer, t); });
        // Markers dragged on the bar: in / out points (successive moves merge into one undo step)
        connect(m_seek, &SeekBar::inOutEdited, this, [this, inBox, outBox](bool in, double t) {
            setProp(in ? cmd::SetLayerProp::InPoint : cmd::SetLayerProp::OutPoint, t);
            QSignalBlocker b1(inBox), b2(outBox);
            if (in) inBox->setValue(t);
            else outBox->setValue(t < 0 ? outBox->maximum() : t);
        });
    }

    if (media && s.hasAudio) {
        // Sound: layer volume (undoable), mute, level
        auto *row = new QHBoxLayout;
        auto *vol = new QSlider(Qt::Horizontal);
        auto *icon = new ResetLabel(QStringLiteral("Volume"), [vol] { vol->setValue(100); });
        vol->setRange(0, 200);
        vol->setValue(int(std::lround(s.volume * 100)));
        vol->setToolTip(QStringLiteral("Layer volume (100% = original level). Hiding the layer also silences it."));
        auto *volLabel = new QLabel(QStringLiteral("%1%").arg(vol->value()));
        volLabel->setMinimumWidth(40);
        volLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *mute = new QCheckBox(QStringLiteral("Mute"));
        mute->setChecked(s.muted);
        row->addWidget(icon);
        row->addWidget(vol, 1);
        row->addWidget(volLabel);
        row->addWidget(mute);
        v->addLayout(row);
        m_meter = new QProgressBar;
        m_meter->setRange(0, 600);
        m_meter->setTextVisible(false);
        m_meter->setFixedHeight(5);
        m_meter->setStyleSheet("QProgressBar { background:#1b1b1d; border:none; } QProgressBar::chunk { background:#3fae5a; }");
        v->addWidget(m_meter);
        connect(vol, &QSlider::valueChanged, this, [this, volLabel](int pct) {
            volLabel->setText(QStringLiteral("%1%").arg(pct));
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
        auto *w = new QSpinBox, *h = new QSpinBox;
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
        connect(params, &ParamPanel::rebuildRequested, this, &LayerInspector::rebuild, Qt::QueuedConnection);
        v->addWidget(params);
    }
    if (s.type == SourceType::Video || s.type == SourceType::Image || s.type == SourceType::Isf) v->addWidget(buildCrop(s));
    return g;
}

// Part of the source picture used: preview with a rectangle whose sides are dragged, numeric fields in %
QWidget *LayerInspector::buildCrop(const LayerSnapshot &s)
{
    auto *box = new QGroupBox;
    auto *v = new QVBoxLayout(box);
    auto *head = new QHBoxLayout;
    auto *title = new ResetLabel(QStringLiteral("<b>Crop</b>"), [this] {
        setProp(cmd::SetLayerProp::Crop, Layer::fullCrop());
        rebuild();
    });
    title->setToolTip(QStringLiteral("Part of the source picture used by the layer — click to use the whole picture"));
    head->addWidget(title);
    head->addStretch();
    v->addLayout(head);
    m_crop = new CropEditor;
    m_crop->setAspect(s.aspect);
    m_crop->setCrop(s.crop);
    v->addWidget(m_crop);
    auto *grid = new QGridLayout;
    static const char *kNames[] = {"Left", "Top", "Right", "Bottom"};
    QDoubleSpinBox *fields[4];
    const double values[4] = {s.crop.left(), s.crop.top(), s.crop.right(), s.crop.bottom()};
    for (int k = 0; k < 4; ++k) {
        auto *f = new QDoubleSpinBox;
        f->setRange(0, 100);
        f->setDecimals(1);
        f->setSuffix(QStringLiteral(" %"));
        f->setKeyboardTracking(false);
        f->setValue(values[k] * 100);
        fields[k] = f;
        const double def = k < 2 ? 0.0 : 100.0;
        grid->addWidget(new ResetLabel(QString::fromUtf8(kNames[k]), [f, def] { f->setValue(def); }), k / 2, (k % 2) * 2);
        grid->addWidget(f, k / 2, (k % 2) * 2 + 1);
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    v->addLayout(grid);
    auto apply = [this](const QRectF &r) {
        setProp(cmd::SetLayerProp::Crop, r);
        emit layerChanged();
    };
    QPointer<CropEditor> editor = m_crop;
    connect(m_crop, &CropEditor::cropEdited, this, [apply, fields](const QRectF &r) {
        const double vals[4] = {r.left(), r.top(), r.right(), r.bottom()};
        for (int k = 0; k < 4; ++k) {
            QSignalBlocker b(fields[k]);
            fields[k]->setValue(vals[k] * 100);
        }
        apply(r);
    });
    for (int k = 0; k < 4; ++k)
        connect(fields[k], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [apply, fields, editor] {
            double l = fields[0]->value() / 100, t = fields[1]->value() / 100, r = fields[2]->value() / 100,
                   b = fields[3]->value() / 100;
            r = std::max(r, l + 0.001);
            b = std::max(b, t + 0.001);
            const QRectF rect(QPointF(l, t), QPointF(std::min(r, 1.0), std::min(b, 1.0)));
            if (editor) editor->setCrop(rect);
            apply(rect);
        });
    return box;
}

QWidget *LayerInspector::buildColor(const LayerSnapshot &s)
{
    auto *g = new QWidget;
    auto *v = new QVBoxLayout(g);
    v->setContentsMargins(0, 0, 0, 0);
    // Balance first, as in DaVinci Resolve: temperature (blue / yellow) and tint (green / magenta)
    {
        auto *box = new QGroupBox;
        auto *grid = new QGridLayout(box);
        grid->addWidget(new QLabel(QStringLiteral("<b>Balance</b>")), 0, 0, 1, 3);
        struct Def {
            const char *name;
            double range, value;
            const char *gradient;
            int prop;
            QPointer<QDoubleSpinBox> *field;
        } defs[] = {{"Temp", ColorAdjust::kTempRange, s.color.temp, "stop:0 #3a7bff, stop:0.5 #888, stop:1 #ffd23a",
                     cmd::SetLayerProp::Temp, &m_temp},
                    {"Tint", ColorAdjust::kTintRange, s.color.tint, "stop:0 #2fd04a, stop:0.5 #888, stop:1 #e03ce0",
                     cmd::SetLayerProp::Tint, &m_tint}};
        int row = 1;
        for (const Def &d : defs) {
            auto *slider = new QSlider(Qt::Horizontal);
            slider->setRange(-1000, 1000);
            slider->setStyleSheet(QStringLiteral("QSlider::groove:horizontal { height:6px; border-radius:3px;"
                                                 " background:qlineargradient(x1:0,y1:0,x2:1,y2:0,%1); }"
                                                 "QSlider::handle:horizontal { background:#eee; width:10px; margin:-5px 0;"
                                                 " border-radius:5px; }")
                                      .arg(QString::fromUtf8(d.gradient)));
            auto *spin = new QDoubleSpinBox;
            spin->setRange(-d.range, d.range);
            spin->setDecimals(d.range >= 1000 ? 0 : 1);
            spin->setSingleStep(d.range / 100);
            spin->setKeyboardTracking(false);
            spin->setFixedWidth(76);
            spin->setValue(d.value);
            slider->setValue(int(std::lround(d.value / d.range * 1000)));
            *d.field = spin;
            const double range = d.range;
            const int prop = d.prop;
            grid->addWidget(new ResetLabel(QString::fromUtf8(d.name), [spin] { spin->setValue(0); }), row, 0);
            grid->addWidget(slider, row, 1);
            grid->addWidget(spin, row, 2);
            connect(slider, &QSlider::valueChanged, this, [this, spin, range, prop](int x) {
                QSignalBlocker b(spin);
                spin->setValue(x / 1000.0 * range);
                setProp(prop, spin->value());
            });
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, slider, range, prop](double x) {
                QSignalBlocker b(slider);
                slider->setValue(int(std::lround(x / range * 1000)));
                setProp(prop, x);
            });
            ++row;
        }
        grid->setColumnStretch(1, 1);
        v->addWidget(box);
    }

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
        ed->setModels(SettingsPanel::colorModels()); // chosen in the Settings tab
    }
    connect(m_colorAdd, &ColorEditor::colorEdited, this, [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorAdd, c); });
    connect(m_colorRemove, &ColorEditor::colorEdited, this,
            [this](const QColor &c) { setProp(cmd::SetLayerProp::ColorRemove, c); });
    return g;
}

QWidget *LayerInspector::buildCompositing(const LayerSnapshot &s)
{
    auto *g = new QWidget; // titled by its sub-tab
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
    form->addRow(new ResetLabel(QStringLiteral("Opacity"), [spin] { spin->setValue(100); }), row);
    connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    connect(slider, &QSlider::valueChanged, this, [this](int v) { setProp(cmd::SetLayerProp::Opacity, v / 100.0); });

    auto *blend = new QComboBox;
    for (BlendMode m : {BlendMode::Normal, BlendMode::Add, BlendMode::Screen, BlendMode::Multiply})
        blend->addItem(blendModeName(m), int(m));
    blend->setCurrentIndex(blend->findData(int(s.blend)));
    form->addRow(new ResetLabel(QStringLiteral("Blend"), [blend] { blend->setCurrentIndex(0); }), blend);
    connect(blend, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, blend](int i) { setProp(cmd::SetLayerProp::Blend, blend->itemData(i).toInt()); });
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
            auto *b = new QDoubleSpinBox;
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
        link->setStyleSheet("QToolButton:checked { background:#ffa028; color:#1b1b1d; }");
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

    auto *modeRow = new QHBoxLayout;
    auto *corners = new QRadioButton(QStringLiteral("Corners"));
    auto *mesh = new QRadioButton(QStringLiteral("Mesh"));
    (s.meshMode ? mesh : corners)->setChecked(true);
    auto *modes = new QButtonGroup(g);
    modes->addButton(corners, 0);
    modes->addButton(mesh, 1);
    auto *cols = new QSpinBox, *rows = new QSpinBox;
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
    auto *add = new QPushButton(QStringLiteral("Add Effect"));
    auto *menu = new QMenu(add);
    add->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        // Group by ISF category
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
                editEffects(QStringLiteral("Add Effect %1").arg(n), [this, p] {
                    QString err;
                    m_selectedEffect = m_engine->addEffect(m_layer, p, &err);
                });
                rebuild();
            });
        }
        if (menu->isEmpty()) menu->addAction(QStringLiteral("(no filters in the library)"))->setEnabled(false);
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
        auto *it = new QListWidgetItem(fx.name + (fx.valid ? QString() : QStringLiteral("  ⚠")));
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
        if (r >= 0 && r != m_selectedEffect) {
            m_selectedEffect = r;
            QMetaObject::invokeMethod(this, &LayerInspector::rebuild, Qt::QueuedConnection);
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
    refreshSpatial();
    // Crop preview: the source picture, read back by the render thread while the editor is shown
    const bool wantPreview = m_crop && m_crop->isVisible();
    if (wantPreview) {
        m_engine->requestSourcePreview(m_layerId, 480);
        quint64 id = 0;
        const QImage img = m_engine->sourcePreview(&id);
        if (id == m_layerId && !img.isNull()) m_crop->setImage(img);
    } else if (m_previewing) {
        m_engine->requestSourcePreview(0);
    }
    m_previewing = wantPreview;
    // Values changed elsewhere (OSC, undo) follow in the color and crop editors
    {
        QRectF crop;
        QColor add, remove;
        double temp = 0, tint = 0;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(m_layer);
            if (!l) return;
            crop = l->crop;
            add = QColor::fromRgbF(l->color.add[0], l->color.add[1], l->color.add[2]);
            remove = QColor::fromRgbF(l->color.remove[0], l->color.remove[1], l->color.remove[2]);
            temp = l->color.temp;
            tint = l->color.tint;
        }
        for (auto [field, value] : {std::pair{m_temp.data(), temp}, std::pair{m_tint.data(), tint}})
            if (field && !field->hasFocus() && std::abs(field->value() - value) > 1e-3) field->setValue(value); // its slider follows
        if (m_crop) m_crop->setCrop(crop);
        if (m_colorAdd) m_colorAdd->setColor(add);
        if (m_colorRemove) m_colorRemove->setColor(remove);
    }
    bool playing;
    double d, p, in, out;
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
        d = l->duration();
        p = l->position();
    }
    if (m_play) m_play->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (m_seek) {
        m_seek->setDuration(d);
        m_seek->setPosition(p);
        m_seek->setInOut(in, out);
    }
    if (m_time) m_time->setText(fmtTime(p) + " / " + fmtTime(d));
}
