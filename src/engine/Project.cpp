// Engine: the project file — a layer to and from JSON, saving and opening a .fulskrin.
#include "EngineInternal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

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
        m_paused = false;
        m_compositionSpeed = 1.0;
        updateTimeScale();
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

// A layer in a file: what it is (its source, its effects, its mesh, where it is routed…) and the values of its
// parameters under "params", each at its address (the same as its OSC address, its timelines' and its snapshots'
// timing keys)
QJsonObject Engine::layerToJson(Layer &l, const QString &projectDir) const
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
        o["output"] = QJsonObject{{"width", l.vpWidth}, {"height", l.vpHeight}, {"screen", l.vpScreen}, {"mode", l.vpMode}};
        o["publish"] = l.vpPublish.toJson();
    }
    // How much of it each viewport shows (always there, so that a snapshot sets it)
    QJsonObject vo;
    for (const auto &[v, a] : l.viewportOpacity) vo[QString::number(v)] = double(a);
    o["viewports"] = vo;
    o["name"] = l.name;
    o["enable"] = l.enabled;
    o["locked"] = l.locked;
    o["color_models"] = l.colorModels;
    if (l.color.maskLayer) o["mask"] = QString::number(l.color.maskLayer);

    QJsonObject src;
    const SourceType type = l.type != SourceType::None ? l.type : l.missingType;
    switch (type) {
    case SourceType::Video: src["type"] = "video"; break;
    case SourceType::Audio: src["type"] = "audio"; break;
    case SourceType::Image: src["type"] = "image"; break;
    case SourceType::Layer:
        src["type"] = "layer";
        src["layer"] = QString::number(l.sourceLayer);
        src["tap"] = layerTapKey(l.sourceTap);
        break;
    case SourceType::Isf:
        if (l.generator) src = l.generator->save(projectDir, false);
        src["type"] = "isf";
        src["width"] = l.genWidth;
        src["height"] = l.genHeight;
        break;
    case SourceType::Text: src["type"] = "text"; break;
    default: src["type"] = "none"; break;
    }
    if (type == SourceType::Video || type == SourceType::Audio) o["play"] = l.playing;
    if (type == SourceType::Video || type == SourceType::Audio || type == SourceType::Image || type == SourceType::Isf) {
        src["path"] = l.sourcePath;
        if (!projectDir.isEmpty()) src["relative_path"] = QDir(projectDir).relativeFilePath(l.sourcePath);
    }
    if (!l.transition.isEmpty()) src["transition"] = l.transition;
    o["source"] = src;
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir, false));
    o["fx"] = fx;
    if (!l.isViewport) o["mesh"] = l.mapping.meshJson();

    QJsonObject params = parametersToJson(l.parameters());
    for (auto it = l.extraParams.constBegin(); it != l.extraParams.constEnd(); ++it)
        if (!params.contains(it.key())) params.insert(it.key(), it.value());
    o["params"] = params;
    if (!l.anims.empty()) {
        QJsonArray anims;
        for (const Animation &a : l.anims) anims.append(layerAnimToJson(a));
        o["anims"] = anims;
    }
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
        l->enabled = o.value("enable").toBool(true);
        l->locked = o.value("locked").toBool(false);
        l->isGroup = o.value("group").toBool(false);
        l->collapsed = o.value("collapsed").toBool(false);
        l->isViewport = o.value("viewport").toBool(false);
        if (l->isViewport) {
            const QJsonObject out = o.value("output").toObject();
            l->vpWidth = std::clamp(out.value("width").toInt(1920), 1, 16384);
            l->vpHeight = std::clamp(out.value("height").toInt(1080), 1, 16384);
            l->vpScreen = out.value("screen").toString();
            l->vpMode = std::clamp(out.value("mode").toInt(0), 0, 2);
            l->vpPublish = PublishSettings::fromJson(o.value("publish").toObject());
        }
        l->viewportOpacity.clear();
        const QJsonObject vo = o.value("viewports").toObject();
        for (auto it = vo.begin(); it != vo.end(); ++it)
            l->viewportOpacity[it.key().toULongLong()] = float(std::clamp(it.value().toDouble(1.0), 0.0, 1.0));
        {
            const QString t = o.value("source").toObject().value("transition").toString();
            l->transition = t.isEmpty() ? QString() : resolvePath(QJsonObject{{"path", t}}, projectDir);
        }
        l->colorModels = o.value("color_models").toInt(l->colorModels);
        l->color.maskLayer = o.value("mask").toString().toULongLong();
        // Saved id kept unless another layer already has it
        const quint64 id = o.value("id").toString().toULongLong();
        bool taken = false;
        for (const auto &other : m_layers) taken |= other.get() != l && other->id == id;
        if (id && !taken) {
            l->id = id;
            m_nextId = std::max(m_nextId, id + 1);
        }
        l->parent = o.value("parent").toString().toULongLong();
        name = l->name;
    }

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video" || type == "audio") {
        const bool video = type == "video";
        const bool ok = video ? setLayerVideo(index, path, &err) : setLayerAudio(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        if (!ok) {
            Lock lk(&m_mutex);
            markMissing(*layer(index), video ? SourceType::Video : SourceType::Audio, path, err);
        }
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
        if (gen) runGl([gen, src, projectDir] { gen->restore(src, projectDir); });
    } else if (type == "text") {
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->type = SourceType::Text;
        l->text = TextSource{};
    }

    for (const QJsonValue &v : o.value("fx").toArray()) {
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
        runGl([inst, e, projectDir] { inst->restore(e, projectDir); });
    }
    // Its values once it is all there (the source would otherwise fit the mapping to its aspect ratio), in the order
    // of its parameters; the mesh's points; then its animations, the ones that are on playing from the start
    Lock lk(&m_mutex);
    Layer *l = layer(index);
    if (o.contains("mesh")) l->mapping.setMeshJson(o.value("mesh").toObject());
    parametersFromJson(l->parameters(), o.value("params").toObject(), &l->extraParams);
    if (l->hasTransport()) {
        l->playing = o.value("play").toBool(true);
        startMedia(*l); // its speed: backwards starts from the end
    }
    l->anims.clear();
    for (const QJsonValue &v : o.value("anims").toArray()) {
        const Animation a = layerAnimFromJson(v.toObject(), l->id);
        const QString &param = a.tracks.front().param;
        const bool twice = std::any_of(l->anims.begin(), l->anims.end(), [&](const Animation &x) { return x.tracks.front().param == param; });
        if (!param.isEmpty() && !twice) l->anims.push_back(a);
    }
    for (Animation &a : l->anims)
        if (a.tracks.front().enabled) startLayerAnim(a);
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
                                    {"fps", renderSettings().frameRate}, {"speed", m_compositionSpeed.load()}};
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
    setCompositionSpeed(comp.value("speed").toDouble(1.0));
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
