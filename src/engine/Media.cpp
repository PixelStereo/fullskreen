// Engine: the media used by the project — the file types read, what each layer uses, the Media Bin,
// and relinking a file that moved.
#include "EngineInternal.h"

#include <QDir>
#include <QFileInfo>

QStringList Engine::videoExtensions()
{
    static const QStringList e = {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mxf", "mpg", "mpeg", "wmv", "flv", "ts", "hap", "mts", "m2ts"};
    return e;
}

QStringList Engine::imageExtensions()
{
    static const QStringList e = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "tga", "exr", "psd"};
    return e;
}

QStringList Engine::audioExtensions()
{
    static const QStringList e = {"wav", "aif", "aiff", "aifc", "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wma", "caf", "w64"};
    return e;
}

bool Engine::isAudioFile(const QString &path) { return audioExtensions().contains(QFileInfo(path).suffix().toLower()); }

bool Engine::isVideoFile(const QString &path) { return videoExtensions().contains(QFileInfo(path).suffix().toLower()); }

bool Engine::isImageFile(const QString &path) { return imageExtensions().contains(QFileInfo(path).suffix().toLower()); }

bool Engine::isIsfFile(const QString &path)
{
    const QString e = QFileInfo(path).suffix().toLower();
    return e == "fs" || e == "frag";
}

std::vector<Engine::MediaRef> Engine::mediaUsage() const
{
    std::vector<MediaRef> out;
    enum Kind { ImageKind, VideoKind, AudioKind };
    auto kindOf = [](const QString &p) { return isVideoFile(p) ? VideoKind : isAudioFile(p) ? AudioKind : ImageKind; };
    auto add = [&](const QString &path, Kind kind, const QString &user, bool imported) {
        if (path.isEmpty()) return;
        for (MediaRef &r : out) {
            if (r.path == path) {
                if (!user.isEmpty() && !r.users.contains(user)) r.users << user;
                r.imported |= imported;
                return;
            }
        }
        MediaRef r;
        r.path = path;
        r.video = kind == VideoKind;
        r.audio = kind == AudioKind;
        r.imported = imported;
        if (!user.isEmpty()) r.users << user;
        out.push_back(r);
    };
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers) {
            const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
            if (t == SourceType::Video || t == SourceType::Image || t == SourceType::Audio)
                add(l->sourcePath, t == SourceType::Video ? VideoKind : t == SourceType::Audio ? AudioKind : ImageKind,
                    l->name, false);
            auto scan = [&](const IsfInstance *inst, const QString &user) {
                if (!inst) return;
                for (const IsfInput &in : inst->inputs())
                    if (in.type == IsfInput::Image && !in.isInputImage && !in.imagePath.isEmpty())
                        add(in.imagePath, isVideoFile(in.imagePath) ? VideoKind : ImageKind, user, false);
            };
            scan(l->generator.get(), l->name);
            for (const auto &fx : l->effects) scan(fx.get(), l->name + QStringLiteral(" › ") + fx->name());
        }
        for (const QString &p : m_binItems)
            if (!isIsfFile(p)) add(p, kindOf(p), QString(), true); // shaders imported to the bin are in ISF > Generators
    }
    for (MediaRef &r : out) r.missing = !QFileInfo::exists(r.path);
    return out;
}

QStringList Engine::binItems() const
{
    Lock lk(&m_mutex);
    return m_binItems;
}

void Engine::addBinItems(const QStringList &paths)
{
    Lock lk(&m_mutex);
    for (const QString &p : paths) {
        if (p.isEmpty()) continue;
        const QString abs = QFileInfo(p).isAbsolute() ? QDir::cleanPath(p) : QFileInfo(p).absoluteFilePath();
        if (!m_binItems.contains(abs)) m_binItems << abs;
    }
}

void Engine::removeBinItem(const QString &path)
{
    Lock lk(&m_mutex);
    m_binItems.removeAll(path);
}

void Engine::relinkBinItem(const QString &from, const QString &to)
{
    Lock lk(&m_mutex);
    const int i = m_binItems.indexOf(from);
    if (i >= 0) {
        if (m_binItems.contains(to)) m_binItems.removeAt(i);
        else m_binItems[i] = to;
    }
}

static bool isfUsesImage(const IsfInstance *inst, const QString &path)
{
    if (!inst) return false;
    for (const IsfInput &in : inst->inputs())
        if (in.type == IsfInput::Image && !in.isInputImage && in.imagePath == path) return true;
    return false;
}

QList<int> Engine::layersUsingMedia(const QString &path) const
{
    QList<int> out;
    Lock lk(&m_mutex);
    for (int i = 0; i < int(m_layers.size()); ++i) {
        const Layer &l = *m_layers[size_t(i)];
        bool uses = l.sourcePath == path && (l.type == SourceType::Video || l.type == SourceType::Image ||
                                             l.type == SourceType::Audio || l.missingType != SourceType::None);
        uses |= isfUsesImage(l.generator.get(), path);
        for (const auto &fx : l.effects) uses |= isfUsesImage(fx.get(), path);
        if (uses) out << i;
    }
    return out;
}

bool Engine::relinkLayerMedia(int i, const QString &from, const QString &to, QString *err)
{
    SourceType kind = SourceType::None;
    Mapping mapping;
    PlayMode mode = PlayMode::Loop;
    double in = 0, out = -1;
    std::vector<std::pair<IsfInstance *, int>> inputs;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        const SourceType t = l->type != SourceType::None ? l->type : l->missingType;
        if (l->sourcePath == from && (t == SourceType::Video || t == SourceType::Image || t == SourceType::Audio)) kind = t;
        mapping = l->mapping;
        mode = l->mode;
        in = l->inPoint;
        out = l->outPoint;
        auto collect = [&](IsfInstance *inst) {
            if (!inst) return;
            for (int k = 0; k < int(inst->inputs().size()); ++k) {
                const IsfInput &in = inst->inputs()[size_t(k)];
                if (in.type == IsfInput::Image && !in.isInputImage && in.imagePath == from) inputs.emplace_back(inst, k);
            }
        };
        collect(l->generator.get());
        for (auto &fx : l->effects) collect(fx.get());
    }
    bool changed = false, ok = true;
    if (kind != SourceType::None) {
        // The new file may be of another type (video replaced by an image, an audio file…)
        SourceType target = kind;
        if (isImageFile(to)) target = SourceType::Image;
        else if (isVideoFile(to)) target = SourceType::Video;
        else if (isAudioFile(to)) target = SourceType::Audio;
        ok = target == SourceType::Video   ? setLayerVideo(i, to, err)
             : target == SourceType::Audio ? setLayerAudio(i, to, err)
                                           : setLayerImage(i, to, err);
        if (ok) {
            setLayerPlayMode(i, mode); // the layer keeps its play mode and its in / out points
            setLayerInOut(i, in, out);
            Lock lk(&m_mutex);
            if (Layer *l = layer(i)) {
                const unsigned rev = l->mapping.revision;
                const double aspect = l->mapping.aspect;
                l->mapping = mapping; // the aligned mapping stays intact
                l->mapping.aspect = aspect;
                l->mapping.revision = rev + 1;
            }
            changed = true;
        }
    }
    for (auto &[inst, k] : inputs) {
        QString e;
        if (setIsfImageInput(inst, k, to, &e)) changed = true;
        else if (err && err->isEmpty()) *err = e;
    }
    return changed && ok;
}
