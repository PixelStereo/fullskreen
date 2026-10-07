// Engine: the project file — a layer to and from JSON, saving and opening a .fulskrin.
#include "EngineInternal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

// The Text generator in a layer state: colors as [r, g, b, a] (0..1, so that a snapshot's panel edits and fades them
// like the shaders' colors), alignment as words.
static QJsonArray colorJson(const QColor &c) { return QJsonArray{c.redF(), c.greenF(), c.blueF(), c.alphaF()}; }
static QColor colorFromJson(const QJsonValue &v, const QColor &fallback)
{
    if (v.isString()) {
        const QColor c(v.toString());
        return c.isValid() ? c : fallback;
    }
    const QJsonArray a = v.toArray();
    if (a.size() < 3) return fallback;
    auto ch = [&](int k, double d) { return std::clamp(k < a.size() ? a[k].toDouble(d) : d, 0.0, 1.0); };
    return QColor::fromRgbF(float(ch(0, 0)), float(ch(1, 0)), float(ch(2, 0)), float(ch(3, 1)));
}
static const char *const kHAlign[] = {"left", "center", "right", "justify"};
static const Qt::AlignmentFlag kHFlag[] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight, Qt::AlignJustify};
static const char *const kVAlign[] = {"top", "middle", "bottom"};
static const Qt::AlignmentFlag kVFlag[] = {Qt::AlignTop, Qt::AlignVCenter, Qt::AlignBottom};

QJsonObject textJson(const TextSource &t)
{
    QString h = QStringLiteral("left"), v = QStringLiteral("top");
    for (int k = 0; k < 4; ++k)
        if (t.align & kHFlag[k]) h = QString::fromLatin1(kHAlign[k]);
    for (int k = 0; k < 3; ++k)
        if (t.align & kVFlag[k]) v = QString::fromLatin1(kVAlign[k]);
    return QJsonObject{{"content", t.content},       {"font", t.font},
                       {"size", t.size},             {"color", colorJson(t.color)},
                       {"h_align", h},                {"v_align", v},
                       {"line_height", t.lineHeight}, {"letter_spacing", t.letterSpacing},
                       {"bold", t.bold},             {"italic", t.italic},
                       {"underline", t.underline},   {"strike", t.strike},
                       {"outline", t.outline},       {"outline_color", colorJson(t.outlineColor)},
                       {"shadow", t.shadow},         {"shadow_color", colorJson(t.shadowColor)},
                       {"shadow_x", t.shadowX},       {"shadow_y", t.shadowY},
                       {"width", t.width},           {"height", t.height}};
}

// Onto the current values: what the state does not hold stays as it is
void readTextJson(TextSource &t, const QJsonObject &o)
{
    t.content = o.value("content").toString(t.content);
    t.font = o.value("font").toString(t.font);
    t.size = o.value("size").toInt(t.size);
    t.color = colorFromJson(o.value("color"), t.color);
    if (o.contains("h_align") || o.contains("v_align")) {
        int h = Qt::AlignLeft, v = Qt::AlignTop;
        for (int k = 0; k < 4; ++k)
            if (o.value("h_align").toString() == QLatin1String(kHAlign[k])) h = kHFlag[k];
        for (int k = 0; k < 3; ++k)
            if (o.value("v_align").toString() == QLatin1String(kVAlign[k])) v = kVFlag[k];
        t.align = Qt::Alignment(h | v);
    } else if (o.contains("align")) {
        t.align = Qt::Alignment(o.value("align").toInt(int(t.align)));
    }
    t.lineHeight = float(o.value("line_height").toDouble(t.lineHeight));
    t.letterSpacing = float(o.value("letter_spacing").toDouble(t.letterSpacing));
    t.bold = o.value("bold").toBool(t.bold);
    t.italic = o.value("italic").toBool(t.italic);
    t.underline = o.value("underline").toBool(t.underline);
    t.strike = o.value("strike").toBool(t.strike);
    t.outline = float(o.value("outline").toDouble(t.outline));
    t.outlineColor = colorFromJson(o.value("outline_color"), t.outlineColor);
    t.shadow = o.value("shadow").toBool(t.shadow);
    t.shadowColor = colorFromJson(o.value("shadow_color"), t.shadowColor);
    t.shadowX = float(o.value("shadow_x").toDouble(t.shadowX));
    t.shadowY = float(o.value("shadow_y").toDouble(t.shadowY));
    t.width = o.value("width").toInt(t.width);
    t.height = o.value("height").toInt(t.height);
    t.sanitize();
}

QString Engine::projectPath() const
{
    Lock lk(&m_mutex);
    return m_projectPath;
}

void Engine::setProjectPath(const QString &p)
{
    Lock lk(&m_mutex);
    m_projectPath = p;
}

static QString missingMessage(const QString &path, const QString &err)
{
    return QFileInfo::exists(path) ? err : QStringLiteral("File not found: ") + path;
}

static void markMissing(Layer &l, SourceType type, const QString &path, const QString &err)
{
    l.missingType = type;
    l.sourcePath = path;
    l.error = missingMessage(path, err);
}

void Engine::newProject()
{
    clearProject();
    ensureViewport(); // a new project shows its composition through one viewport
    emit layersChanged();
    emit snapshotsChanged();
    emit animationsChanged();
    emit sequencesChanged();
    emit sequencePositionChanged();
}

// Empties everything (layers, viewports, snapshots, media bin)
void Engine::clearProject()
{
    std::vector<std::unique_ptr<Layer>> old;
    {
        Lock lk(&m_mutex);
        old.swap(m_layers);
        m_projectPath.clear();
        m_binItems.clear();
        m_snapshots.clear();
        m_nextSnapshotId = 1;
        m_render = RenderSettings(); // the machine's defaults
        m_sequences.clear();
        m_currentSequence = m_sequencePosition = -1;
        m_runs.clear();
        m_animations.clear();
        m_nextAnimationId = 1;
        m_fades.clear();
        m_recalledSnapshot = 0;
        m_recallTotal = 0;
        for (auto &[id, t] : m_transitions) retireTransition(std::move(t));
        m_transitions.clear();
        m_audio->setVolume(1.0f);
        m_publishDirty = true; // the publishers of the viewports that went are stopped
    }
    for (auto &l : old) {
        if (l->video) l->video->close();
        releaseAudio(*m_audio, l->audio);
        auto g = std::make_shared<Garbage>();
        g->layer = std::move(l);
        runGl([this, g] { releaseLayer(*g->layer); }, false);
    }
    setCompositionSize(QSize(1920, 1080));
    emit layersChanged();
    emit snapshotsChanged();
    emit animationsChanged();
}

QString Engine::resolvePath(const QJsonObject &o, const QString &projectDir) const
{
    const QString abs = o.value("path").toString();
    if (!abs.isEmpty() && QFile::exists(abs)) return abs;
    if (!projectDir.isEmpty()) {
        const QString rel = o.value("relative_path").toString();
        if (!rel.isEmpty()) {
            const QString p = QDir(projectDir).absoluteFilePath(rel);
            if (QFile::exists(p)) return QDir::cleanPath(p);
        }
        const QString same = QDir(projectDir).absoluteFilePath(QFileInfo(abs).fileName());
        if (QFile::exists(same)) return same;
    }
    const QString lib = m_library.findByFileName(QFileInfo(abs).fileName());
    if (!lib.isEmpty()) return lib;
    return abs;
}

QJsonObject Engine::layerToJson(const Layer &l, const QString &projectDir) const
{
    QJsonObject o;
    // Ids are strings: JSON numbers are doubles
    o["id"] = QString::number(l.id);
    if (l.parent) o["parent"] = QString::number(l.parent);
    if (l.isGroup) {
        o["group"] = true;
        o["collapsed"] = l.collapsed;
    }
    if (l.isViewport) {
        o["viewport"] = true;
        o["width"] = l.vpWidth;
        o["height"] = l.vpHeight;
        o["screen"] = l.vpScreen;
        o["output_mode"] = l.vpMode;
        o["publish"] = l.vpPublish.toJson();
    }
    // Always save viewport opacity (even if empty) so snapshots preserve per-viewport visibility settings
    QJsonObject vo;
    for (const auto &[v, a] : l.viewportOpacity) vo[QString::number(v)] = double(a);
    o["viewports"] = vo;
    o["name"] = l.name;
    o["visible"] = l.visible;
    o["locked"] = l.locked;
    o["opacity"] = l.opacity;
    o["blend_mode"] = blendModeKey(l.blend);
    o["volume"] = l.volume;
    o["muted"] = l.muted;
    QJsonObject src;
    switch (l.type) {
    case SourceType::Video:
        src["type"] = "video";
        src["play_mode"] = playModeKey(l.mode);
        src["in"] = l.inPoint;
        src["out"] = l.outPoint;
        src["speed"] = l.speed;
        src["playing"] = l.playing;
        break;
    case SourceType::Audio:
        src["type"] = "audio";
        src["play_mode"] = playModeKey(l.mode);
        src["in"] = l.inPoint;
        src["out"] = l.outPoint;
        src["speed"] = l.speed;
        src["playing"] = l.playing;
        break;
    case SourceType::Image: src["type"] = "image"; break;
    case SourceType::Layer:
        src["type"] = "layer";
        src["layer"] = QString::number(l.sourceLayer); // ids are strings: JSON numbers are doubles
        src["tap"] = layerTapKey(l.sourceTap);
        break;
    case SourceType::Isf:
        src = l.generator ? l.generator->save(projectDir) : QJsonObject();
        src["type"] = "isf";
        src["width"] = l.genWidth;
        src["height"] = l.genHeight;
        break;
    case SourceType::Text: {
        src["type"] = "text";
        const QJsonObject tj = textJson(l.text);
        for (auto it = tj.constBegin(); it != tj.constEnd(); ++it) src[it.key()] = it.value();
        break;
    }
    default:
        // Missing file: keep what was intended, so nothing is lost on save.
        if (l.missingType == SourceType::Video || l.missingType == SourceType::Audio) {
            src["type"] = l.missingType == SourceType::Video ? "video" : "audio";
            src["play_mode"] = playModeKey(l.mode);
            src["in"] = l.inPoint;
            src["out"] = l.outPoint;
            src["speed"] = l.speed;
            src["playing"] = l.playing;
        } else if (l.missingType == SourceType::Image) {
            src["type"] = "image";
        } else {
            src["type"] = "none";
        }
        break;
    }
    if ((l.type != SourceType::None && l.type != SourceType::Layer) || l.missingType != SourceType::None) {
        src["path"] = l.sourcePath;
        if (!projectDir.isEmpty()) src["relative_path"] = QDir(projectDir).relativeFilePath(l.sourcePath);
    }
    const QRectF c = l.roi;
    if (!l.transition.isEmpty()) src["transition"] = l.transition;
    o["source"] = src;
    o["roi"] = QJsonArray{c.left(), c.top(), c.right(), c.bottom()};
    auto rgb = [](const float v[3]) { return QJsonArray{v[0], v[1], v[2]}; };
    o["color"] = QJsonObject{{"temp", l.color.temp},        {"tint", l.color.tint},
                             {"add", rgb(l.color.add)},    {"remove", rgb(l.color.remove)},
                             {"enable", l.color.enabled}, {"temp_enable", l.color.tempOn},
                             {"tint_enable", l.color.tintOn},   {"add_enable", l.color.addOn},
                             {"remove_enable", l.color.removeOn}};
    if (l.color.maskLayer) {
        QJsonObject col = o["color"].toObject();
        col["mask"] = QString::number(l.color.maskLayer);
        if (l.color.maskInvert) col["mask_invert"] = true;
        o["color"] = col;
    }
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir));
    o["effects"] = fx;
    o["effects_enable"] = l.effectsEnabled;
    o["color_models"] = l.colorModels;
    o["spatial"] = l.mapping.toJson();
    return o;
}

void Engine::layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings)
{
    QString name;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        if (!l) return;
        l->name = o.value("name").toString(l->name);
        l->visible = o.value("visible").toBool(true);
        l->opacity = float(o.value("opacity").toDouble(1.0));
        l->blend = blendModeFromKey(o.value("blend_mode").toString());
        l->volume = float(std::clamp(o.value("volume").toDouble(1.0), 0.0, 2.0));
        l->muted = o.value("muted").toBool(false);
        l->locked = o.value("locked").toBool(false);
        l->isGroup = o.value("group").toBool(false);
        l->collapsed = o.value("collapsed").toBool(false);
        l->isViewport = o.value("viewport").toBool(false);
        if (l->isViewport) {
            l->vpWidth = std::clamp(o.value("width").toInt(1920), 1, 16384);
            l->vpHeight = std::clamp(o.value("height").toInt(1080), 1, 16384);
            l->vpScreen = o.value("screen").toString();
            l->vpMode = std::clamp(o.value("output_mode").toInt(0), 0, 2);
            l->vpPublish = PublishSettings::fromJson(o.value("publish").toObject());
        }
        l->viewportOpacity.clear();
        const QJsonObject vo = o.value("viewports").toObject();
        for (auto it = vo.begin(); it != vo.end(); ++it)
            l->viewportOpacity[it.key().toULongLong()] = float(std::clamp(it.value().toDouble(1.0), 0.0, 1.0));
        l->effectsEnabled = o.value("effects_enable").toBool(true);
        {
            const QString t = o.value("source").toObject().value("transition").toString();
            l->transition = t.isEmpty() ? QString() : resolvePath(QJsonObject{{"path", t}}, projectDir);
        }
        l->colorModels = o.value("color_models").toInt(l->colorModels);
        // Saved id kept unless another layer already has it
        const quint64 id = o.value("id").toString().toULongLong();
        bool taken = false;
        for (const auto &other : m_layers) taken |= other.get() != l && other->id == id;
        if (id && !taken) {
            l->id = id;
            m_nextId = std::max(m_nextId, id + 1);
        }
        l->parent = o.value("parent").toString().toULongLong();
        l->roi = roiFromJson(o);
        colorFromJson(l->color, o.value("color").toObject());
        name = l->name;
    }

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video" || type == "audio") {
        const bool video = type == "video";
        {
            Lock lk(&m_mutex);
            layer(index)->speed = src.value("speed").toDouble(1.0);
        }
        const bool ok = video ? setLayerVideo(index, path, &err) : setLayerAudio(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        setLayerPlayMode(index, playModeFromKey(src.value("play_mode").toString()));
        setLayerInOut(index, src.value("in").toDouble(0), src.value("out").toDouble(-1));
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->playing = src.value("playing").toBool(true);
        if (l->hasTransport()) startMedia(*l); // saved speed: backwards starts from the end
        if (!ok) markMissing(*l, video ? SourceType::Video : SourceType::Audio, path, err);
    } else if (type == "image") {
        const bool ok = setLayerImage(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        if (!ok) {
            Lock lk(&m_mutex);
            markMissing(*layer(index), SourceType::Image, path, err);
        }
    } else if (type == "layer") {
        // The referenced layer may not be loaded yet: the id is kept as it is and checked once the project is read
        // (fixLayerReferences); an unknown id simply shows nothing.
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->type = SourceType::Layer;
        l->sourceLayer = src.value("layer").toString().toULongLong();
        l->sourceTap = layerTapFromKey(src.value("tap").toString());
    } else if (type == "isf") {
        {
            Lock lk(&m_mutex);
            layer(index)->genWidth = src.value("width").toInt(m_compSize.width());
            layer(index)->genHeight = src.value("height").toInt(m_compSize.height());
        }
        if (!setLayerIsf(index, path, &err) && warnings) *warnings << name + ": " + err;
        IsfInstance *gen;
        {
            Lock lk(&m_mutex);
            gen = layer(index)->generator.get();
        }
        const QJsonObject params = src.value("params").toObject();
        const double speed = std::clamp(src.value("speed").toDouble(1.0), 0.0, 10.0);
        if (gen) runGl([gen, params, projectDir, speed] {
            gen->restoreParams(params, projectDir);
            gen->speed = speed;
        });
    } else if (type == "text") {
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->type = SourceType::Text;
        l->text = TextSource{};
        readTextJson(l->text, src);
    }

    for (const QJsonValue &v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        const QString p = resolvePath(e, projectDir);
        const int fi = addEffect(index, p, &err);
        if (fi < 0) continue;
        IsfInstance *inst;
        {
            Lock lk(&m_mutex);
            inst = layer(index)->effects[size_t(fi)].get();
        }
        if (!inst->isValid() && warnings) *warnings << name + " / " + QFileInfo(p).fileName() + ": " + inst->error();
        runGl([inst, e, projectDir] {
            inst->readState(e);
            inst->restoreParams(e.value("params").toObject(), projectDir);
        });
    }
    // The mapping is restored after the source (which would otherwise auto-adjust the aspect ratio).
    Lock lk(&m_mutex);
    layer(index)->mapping.fromJson(o.value("spatial").toObject());
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QString dir = QFileInfo(path).absolutePath();
    QJsonObject root;
    {
        Lock lk(&m_mutex);
        root["app"] = "Fulskrin";
        root["format_version"] = 1;
        root["composition"] = QJsonObject{{"width", m_compSize.width()}, {"height", m_compSize.height()},
                                    {"fps", renderSettings().frameRate}};
        QJsonArray layers;
        for (const auto &l : m_layers) layers.append(layerToJson(*l, dir));
        root["layers"] = layers;
        QJsonArray bin;
        for (const QString &p : m_binItems)
            bin.append(QJsonObject{{"path", p}, {"relative_path", QDir(dir).relativeFilePath(p)}});
        root["bin"] = bin;
        root["audio"] = QJsonObject{{"volume", double(m_audio->volume())}};
        QJsonArray mems;
        for (const Snapshot &m : m_snapshots) mems.append(snapshotToJson(m, dir));
        root["snapshots"] = mems;
        root["timelines"] = animationsToJson();
        root["sequences"] = sequencesToJson();
        root["current_sequence"] = m_currentSequence;
    }
    root["ui"] = uiState;
    // Atomic write: a crash during save does not corrupt the existing file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

bool Engine::loadProject(const QString &path, QJsonObject *uiState, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("Unreadable project: ") + pe.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    clearProject();
    const QJsonObject comp = root.value("composition").toObject();
    setCompositionSize(QSize(comp.value("width").toInt(1920), comp.value("height").toInt(1080)));
    {
        RenderSettings rs;
        rs.frameRate = comp.value("fps").toDouble(-1); // -1: the machine's default
        setRenderSettings(rs);
    }
    const QString dir = QFileInfo(path).absolutePath();
    QStringList warnings;
    const QJsonArray layers = root.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        int idx = addLayer(QString(), layerCount());
        layerFromJson(idx, layers[i].toObject(), dir, &warnings);
    }
    {
        Lock lk(&m_mutex);
        normalizeLocked();
    }
    ensureViewport();
    fixLayerReferences(&warnings);
    {
        Lock lk(&m_mutex);
        m_publishDirty = true;
    }
    QStringList bin;
    for (const QJsonValue &v : root.value("bin").toArray()) bin << resolvePath(v.toObject(), dir);
    addBinItems(bin);
    const QJsonObject audio = root.value("audio").toObject();
    m_audio->setVolume(float(std::clamp(audio.value("volume").toDouble(1.0), 0.0, 2.0)));
    {
        Lock lk(&m_mutex);
        for (const QJsonValue &v : root.value("snapshots").toArray()) {
            Snapshot m = snapshotFromJson(v.toObject(), dir);
            bool taken = !m.id;
            for (const Snapshot &o : m_snapshots) taken = taken || o.id == m.id;
            if (taken) m.id = 0; // given below, after the saved ones
            m_snapshots.push_back(m);
        }
        for (const Snapshot &m : m_snapshots) m_nextSnapshotId = std::max(m_nextSnapshotId, m.id + 1);
        for (Snapshot &m : m_snapshots)
            if (!m.id) m.id = m_nextSnapshotId++;
        animationsFromJson(root.value("timelines").toArray());
        sequencesFromJson(root.value("sequences").toArray(), root.value("current_sequence").toInt(0));
    }
    emit snapshotsChanged();
    emit animationsChanged();
    emit sequencesChanged();
    emit sequencePositionChanged();
    if (uiState) *uiState = root.value("ui").toObject();
    setProjectPath(QFileInfo(path).absoluteFilePath());
    emit layersChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}
