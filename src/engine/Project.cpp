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
    std::vector<std::unique_ptr<Layer>> old;
    {
        Lock lk(&m_mutex);
        old.swap(m_layers);
        m_projectPath.clear();
        m_binItems.clear();
        m_memories.clear();
        m_fades.clear();
        m_audio->setMasterVolume(1.0f);
        m_audio->setMuted(false);
        if (m_publish != PublishSettings()) {
            m_publish = PublishSettings();
            m_publishDirty = true;
        }
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
    emit memoriesChanged();
}

QString Engine::resolvePath(const QJsonObject &o, const QString &projectDir) const
{
    const QString abs = o.value("path").toString();
    if (!abs.isEmpty() && QFile::exists(abs)) return abs;
    if (!projectDir.isEmpty()) {
        const QString rel = o.value("relativePath").toString();
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
    o["name"] = l.name;
    o["visible"] = l.visible;
    o["locked"] = l.locked;
    o["opacity"] = l.opacity;
    o["blend"] = blendModeKey(l.blend);
    o["volume"] = l.volume;
    o["muted"] = l.muted;
    QJsonObject src;
    switch (l.type) {
    case SourceType::Video:
        src["type"] = "video";
        src["playMode"] = playModeKey(l.mode);
        src["in"] = l.inPoint;
        src["out"] = l.outPoint;
        src["speed"] = l.speed;
        src["playing"] = l.playing;
        break;
    case SourceType::Audio:
        src["type"] = "audio";
        src["playMode"] = playModeKey(l.mode);
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
    default:
        // Missing file: keep what was intended, so nothing is lost on save.
        if (l.missingType == SourceType::Video || l.missingType == SourceType::Audio) {
            src["type"] = l.missingType == SourceType::Video ? "video" : "audio";
            src["playMode"] = playModeKey(l.mode);
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
        if (!projectDir.isEmpty()) src["relativePath"] = QDir(projectDir).relativeFilePath(l.sourcePath);
    }
    const QRectF c = l.roi;
    src["roi"] = QJsonArray{c.left(), c.top(), c.right(), c.bottom()};
    o["source"] = src;
    auto rgb = [](const float v[3]) { return QJsonArray{v[0], v[1], v[2]}; };
    o["color"] = QJsonObject{{"temp", l.color.temp},        {"tint", l.color.tint},
                             {"add", rgb(l.color.add)},    {"remove", rgb(l.color.remove)},
                             {"enabled", l.color.enabled}, {"tempOn", l.color.tempOn},
                             {"tintOn", l.color.tintOn},   {"addOn", l.color.addOn},
                             {"removeOn", l.color.removeOn}};
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir));
    o["effects"] = fx;
    o["effectsEnabled"] = l.effectsEnabled;
    o["colorModels"] = l.colorModels;
    o["mapping"] = l.mapping.toJson();
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
        l->blend = blendModeFromKey(o.value("blend").toString());
        l->volume = float(std::clamp(o.value("volume").toDouble(1.0), 0.0, 2.0));
        l->muted = o.value("muted").toBool(false);
        l->locked = o.value("locked").toBool(false);
        l->isGroup = o.value("group").toBool(false);
        l->collapsed = o.value("collapsed").toBool(false);
        l->effectsEnabled = o.value("effectsEnabled").toBool(true);
        l->colorModels = o.value("colorModels").toInt(l->colorModels);
        // Saved id kept unless another layer already has it
        const quint64 id = o.value("id").toString().toULongLong();
        bool taken = false;
        for (const auto &other : m_layers) taken |= other.get() != l && other->id == id;
        if (id && !taken) {
            l->id = id;
            m_nextId = std::max(m_nextId, id + 1);
        }
        l->parent = o.value("parent").toString().toULongLong();
        l->roi = roiFromJson(o.value("source").toObject());
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
        // Saved mode ("loop": true / false in projects older than the play modes)
        const PlayMode fallback = src.value("loop").toBool(true) ? PlayMode::Loop : PlayMode::OneShot;
        setLayerPlayMode(index, playModeFromKey(src.value("playMode").toString(), fallback));
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
        if (gen) runGl([gen, params, projectDir] { gen->restoreParams(params, projectDir); });
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
            inst->enabled = e.value("enabled").toBool(true);
            inst->restoreParams(e.value("params").toObject(), projectDir);
        });
    }
    // The mapping is restored after the source (which would otherwise auto-adjust the aspect ratio).
    Lock lk(&m_mutex);
    layer(index)->mapping.fromJson(o.value("mapping").toObject());
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QString dir = QFileInfo(path).absolutePath();
    QJsonObject root;
    {
        Lock lk(&m_mutex);
        root["app"] = "Fulskrin";
        root["formatVersion"] = 1;
        root["composition"] = QJsonObject{{"width", m_compSize.width()}, {"height", m_compSize.height()}};
        QJsonArray layers;
        for (const auto &l : m_layers) layers.append(layerToJson(*l, dir));
        root["layers"] = layers;
        QJsonArray bin;
        for (const QString &p : m_binItems)
            bin.append(QJsonObject{{"path", p}, {"relativePath", QDir(dir).relativeFilePath(p)}});
        root["bin"] = bin;
        root["publish"] = m_publish.toJson();
        root["audio"] = QJsonObject{{"volume", double(m_audio->masterVolume())}, {"muted", m_audio->muted()}};
        QJsonArray mems;
        for (const Memory &m : m_memories) mems.append(memoryToJson(m, dir));
        root["memories"] = mems;
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
    newProject();
    const QJsonObject comp = root.value("composition").toObject();
    setCompositionSize(QSize(comp.value("width").toInt(1920), comp.value("height").toInt(1080)));
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
    fixLayerReferences(&warnings);
    QStringList bin;
    for (const QJsonValue &v : root.value("bin").toArray()) bin << resolvePath(v.toObject(), dir);
    addBinItems(bin);
    if (root.contains("publish")) setPublishSettings(PublishSettings::fromJson(root.value("publish").toObject()));
    const QJsonObject audio = root.value("audio").toObject();
    m_audio->setMasterVolume(float(std::clamp(audio.value("volume").toDouble(1.0), 0.0, 2.0)));
    m_audio->setMuted(audio.value("muted").toBool(false));
    {
        Lock lk(&m_mutex);
        for (const QJsonValue &v : root.value("memories").toArray()) m_memories.push_back(memoryFromJson(v.toObject(), dir));
    }
    emit memoriesChanged();
    if (uiState) *uiState = root.value("ui").toObject();
    setProjectPath(QFileInfo(path).absoluteFilePath());
    emit layersChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}
