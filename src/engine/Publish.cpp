#include "Publish.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLibrary>
#include <QMutex>
#include <QStringList>
#include <QtGlobal>
#include <climits>

QString publishKindName(PublishKind k)
{
    switch (k) {
    case PublishKind::Ndi: return QStringLiteral("NDI");
    case PublishKind::Omt: return QStringLiteral("OMT");
    case PublishKind::Syphon: return QStringLiteral("Syphon");
    case PublishKind::Spout: return QStringLiteral("Spout");
    }
    return {};
}

static const char *kKeys[kPublishKindCount] = {"ndi", "omt", "syphon", "spout"};

bool PublishSettings::operator==(const PublishSettings &o) const
{
    for (int i = 0; i < kPublishKindCount; ++i)
        if (targets[i] != o.targets[i]) return false;
    return omtQuality == o.omtQuality && libraryFolder == o.libraryFolder;
}

QJsonObject PublishSettings::toJson() const
{
    QJsonObject o;
    for (int i = 0; i < kPublishKindCount; ++i)
        o[kKeys[i]] = QJsonObject{{"enabled", targets[i].enabled}, {"name", targets[i].name}};
    o["omtQuality"] = omtQuality;
    if (!libraryFolder.isEmpty()) o["libraryFolder"] = libraryFolder;
    return o;
}

PublishSettings PublishSettings::fromJson(const QJsonObject &o)
{
    PublishSettings s;
    for (int i = 0; i < kPublishKindCount; ++i) {
        const QJsonObject t = o.value(kKeys[i]).toObject();
        s.targets[i].enabled = t.value("enabled").toBool(false);
        s.targets[i].name = t.value("name").toString(QStringLiteral("Lanterne"));
        if (s.targets[i].name.trimmed().isEmpty()) s.targets[i].name = QStringLiteral("Lanterne");
    }
    s.omtQuality = o.value("omtQuality").toInt(0);
    s.libraryFolder = o.value("libraryFolder").toString();
    return s;
}

bool publishCompiledIn(PublishKind k)
{
    switch (k) {
    case PublishKind::Ndi:
    case PublishKind::Omt: return true;
#ifdef LANTERNE_HAS_SYPHON
    case PublishKind::Syphon: return true;
#endif
#ifdef LANTERNE_HAS_SPOUT
    case PublishKind::Spout: return true;
#endif
    default: return false;
    }
}

#ifndef LANTERNE_HAS_SYPHON
std::unique_ptr<GpuPublisher> createSyphonPublisher() { return nullptr; }
#endif
#ifndef LANTERNE_HAS_SPOUT
std::unique_ptr<GpuPublisher> createSpoutPublisher() { return nullptr; }
#endif

// ---------------------------------------------------------------------------
// Fil d'envoi
// ---------------------------------------------------------------------------

CpuSendThread::CpuSendThread()
{
    for (CpuFrame &f : m_pool) m_free.push_back(&f);
    m_thread = std::thread(&CpuSendThread::run, this);
}

CpuSendThread::~CpuSendThread()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

CpuFrame *CpuSendThread::acquire()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_free.empty()) {
        CpuFrame *f = m_free.back();
        m_free.pop_back();
        return f;
    }
    // Le fil d'envoi est en retard : on remplace l'image en attente plutôt que d'attendre.
    CpuFrame *f = m_pending;
    m_pending = nullptr;
    return f;
}

void CpuSendThread::submit(CpuFrame *f)
{
    if (!f) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_pending) m_free.push_back(m_pending);
        m_pending = f;
    }
    m_cv.notify_all();
}

void CpuSendThread::setPublishers(std::vector<std::shared_ptr<CpuPublisher>> pubs)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_active = !pubs.empty();
        m_next = std::move(pubs);
        m_changed = true;
    }
    m_cv.notify_all();
}

void CpuSendThread::run()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    for (;;) {
        m_cv.wait(lk, [&] { return m_quit || m_changed || m_pending; });
        if (m_quit) break;
        if (m_changed) {
            // Les anciennes publications sont détruites ici, avant que leur dernier tampon ne soit réutilisé.
            auto old = std::move(m_pubs);
            m_pubs = std::move(m_next);
            m_next.clear();
            m_changed = false;
            lk.unlock();
            old.clear();
            lk.lock();
            if (m_inflight) {
                m_free.push_back(m_inflight);
                m_inflight = nullptr;
            }
            continue;
        }
        CpuFrame *f = m_pending;
        m_pending = nullptr;
        auto pubs = m_pubs;
        lk.unlock();
        for (auto &p : pubs) p->send(*f);
        ++m_sent;
        pubs.clear();
        lk.lock();
        if (m_inflight) m_free.push_back(m_inflight);
        m_inflight = f;
    }
    auto old = std::move(m_pubs);
    m_next.clear();
    lk.unlock();
    old.clear();
}

// ---------------------------------------------------------------------------
// Chargement des bibliothèques
// ---------------------------------------------------------------------------

static QStringList appFolders()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList f;
    if (app.isEmpty()) return f;
    f << app;
#ifdef Q_OS_MACOS
    f << QDir::cleanPath(app + "/../Frameworks") << QDir::cleanPath(app + "/../Resources");
#endif
    return f;
}

// Essaie une liste de chemins / noms ; retourne la bibliothèque chargée (jamais déchargée).
static QLibrary *loadFirst(const QStringList &candidates, QString *triedOut)
{
    QStringList tried;
    for (const QString &c : candidates) {
        if (c.isEmpty()) continue;
        if (QFileInfo(c).isAbsolute() && !QFileInfo::exists(c)) {
            tried << c;
            continue;
        }
        auto *lib = new QLibrary(c);
        lib->setLoadHints(QLibrary::ExportExternalSymbolsHint);
        if (lib->load()) return lib;
        tried << c + " (" + lib->errorString() + ")";
        delete lib;
    }
    if (triedOut) *triedOut = tried.join("\n");
    return nullptr;
}

static QStringList ndiCandidates(const QString &extra)
{
    QStringList c;
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("Processing.NDI.Lib.x64.dll");
#elif defined(Q_OS_MACOS)
    const QString name = QStringLiteral("libndi.dylib");
#else
    const QString name = QStringLiteral("libndi.so.6");
#endif
    if (!extra.isEmpty()) c << QDir(extra).filePath(name);
    for (const char *env : {"NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5"}) {
        const QString d = qEnvironmentVariable(env);
        if (!d.isEmpty()) c << QDir(d).filePath(name);
    }
    for (const QString &d : appFolders()) c << QDir(d).filePath(name);
#if defined(Q_OS_WIN)
    c << QStringLiteral("C:/Program Files/NDI/NDI 6 Runtime/v6/") + name << QStringLiteral("C:/Program Files/NDI/NDI 6 Tools/Runtime/") + name;
    c << name;
#elif defined(Q_OS_MACOS)
    c << QStringLiteral("/usr/local/lib/libndi.dylib") << QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib")
      << QStringLiteral("/opt/homebrew/lib/libndi.dylib");
#else
    c << name << QStringLiteral("libndi.so.5") << QStringLiteral("/usr/local/lib/libndi.so.6") << QStringLiteral("/usr/lib/libndi.so.6");
#endif
    return c;
}

static QStringList omtCandidates(const QString &extra, QString *vmxOut)
{
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("libomt.dll"), vmx = QStringLiteral("libvmx.dll");
#elif defined(Q_OS_MACOS)
    const QString name = QStringLiteral("libomt.dylib"), vmx = QStringLiteral("libvmx.dylib");
#else
    const QString name = QStringLiteral("libomt.so"), vmx = QStringLiteral("libvmx.so");
#endif
    QStringList dirs;
    if (!extra.isEmpty()) dirs << extra;
    const QString env = qEnvironmentVariable("OMT_LIBRARY_DIR");
    if (!env.isEmpty()) dirs << env;
    dirs << appFolders();
#if defined(Q_OS_MACOS)
    dirs << QStringLiteral("/usr/local/lib") << QStringLiteral("/opt/homebrew/lib");
#elif !defined(Q_OS_WIN)
    dirs << QStringLiteral("/usr/local/lib") << QStringLiteral("/usr/lib");
#endif
    QStringList c;
    for (const QString &d : dirs) c << QDir(d).filePath(name);
    c << name;
    if (vmxOut) *vmxOut = vmx;
    return c;
}

QString ndiLibraryPath(const QString &extra)
{
    for (const QString &c : ndiCandidates(extra))
        if (QFileInfo(c).isAbsolute() && QFileInfo::exists(c)) return c;
    return {};
}

QString omtLibraryPath(const QString &extra)
{
    for (const QString &c : omtCandidates(extra, nullptr))
        if (QFileInfo(c).isAbsolute() && QFileInfo::exists(c)) return c;
    return {};
}

// ---------------------------------------------------------------------------
// NDI (structures conformes à Processing.NDI.structs.h / Send.h, SDK v5 et v6)
// ---------------------------------------------------------------------------
namespace ndi {
struct SendCreate {
    const char *p_ndi_name;
    const char *p_groups;
    bool clock_video, clock_audio;
};
struct VideoFrameV2 {
    int xres, yres;
    uint32_t FourCC;
    int frame_rate_N, frame_rate_D;
    float picture_aspect_ratio;
    int frame_format_type;
    int64_t timecode;
    uint8_t *p_data;
    int line_stride_in_bytes;
    const char *p_metadata;
    int64_t timestamp;
};
constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t kBGRA = fourcc('B', 'G', 'R', 'A');
constexpr int kProgressive = 1;

struct Lib {
    bool ok = false;
    QString error, path;
    bool (*initialize)() = nullptr;
    void *(*send_create)(const SendCreate *) = nullptr;
    void (*send_destroy)(void *) = nullptr;
    void (*send_video_async_v2)(void *, const VideoFrameV2 *) = nullptr;
    void (*send_video_v2)(void *, const VideoFrameV2 *) = nullptr;
    int (*send_get_no_connections)(void *, uint32_t) = nullptr;
};

static Lib &lib(const QString &extra)
{
    static QMutex mutex;
    static Lib l;
    static bool tried = false;
    QMutexLocker lk(&mutex);
    if (tried && (l.ok || extra.isEmpty())) return l;
    tried = true;
    QString triedPaths;
    QLibrary *q = loadFirst(ndiCandidates(extra), &triedPaths);
    if (!q) {
        l.error = QStringLiteral("Bibliothèque NDI introuvable : installez « NDI Tools » ou le « NDI Runtime » (ndi.video).");
        return l;
    }
    l.path = q->fileName();
    l.initialize = reinterpret_cast<bool (*)()>(q->resolve("NDIlib_initialize"));
    l.send_create = reinterpret_cast<void *(*)(const SendCreate *)>(q->resolve("NDIlib_send_create"));
    l.send_destroy = reinterpret_cast<void (*)(void *)>(q->resolve("NDIlib_send_destroy"));
    l.send_video_async_v2 = reinterpret_cast<void (*)(void *, const VideoFrameV2 *)>(q->resolve("NDIlib_send_send_video_async_v2"));
    l.send_video_v2 = reinterpret_cast<void (*)(void *, const VideoFrameV2 *)>(q->resolve("NDIlib_send_send_video_v2"));
    l.send_get_no_connections = reinterpret_cast<int (*)(void *, uint32_t)>(q->resolve("NDIlib_send_get_no_connections"));
    if (!l.initialize || !l.send_create || !l.send_destroy || !l.send_video_async_v2) {
        l.error = QStringLiteral("Bibliothèque NDI incompatible : ") + l.path;
        return l;
    }
    if (!l.initialize()) {
        l.error = QStringLiteral("NDI ne peut pas s'initialiser sur ce processeur.");
        return l;
    }
    l.ok = true;
    l.error.clear();
    return l;
}
} // namespace ndi

class NdiPublisher : public CpuPublisher
{
public:
    ~NdiPublisher() override
    {
        if (m_send) m_lib->send_destroy(m_send); // synchronise aussi l'envoi asynchrone en cours
    }
    bool start(const QString &name, const PublishSettings &s, QString *err) override
    {
        m_lib = &ndi::lib(s.libraryFolder);
        if (!m_lib->ok) {
            if (err) *err = m_lib->error;
            return false;
        }
        m_name = name.toUtf8();
        ndi::SendCreate c{m_name.constData(), nullptr, false, false};
        m_send = m_lib->send_create(&c);
        if (!m_send) {
            if (err) *err = QStringLiteral("Création de la source NDI impossible.");
            return false;
        }
        return true;
    }
    void send(const CpuFrame &f) override
    {
        ndi::VideoFrameV2 v{};
        v.xres = f.width;
        v.yres = f.height;
        v.FourCC = ndi::kBGRA;
        v.frame_rate_N = f.fpsN;
        v.frame_rate_D = f.fpsD;
        v.picture_aspect_ratio = f.height ? float(f.width) / float(f.height) : 0.f;
        v.frame_format_type = ndi::kProgressive;
        v.timecode = INT64_MAX; // NDIlib_send_timecode_synthesize
        v.p_data = const_cast<uint8_t *>(f.bgra.data());
        v.line_stride_in_bytes = f.stride;
        // Le tampon reste valide jusqu'à l'envoi suivant (géré par CpuSendThread)
        m_lib->send_video_async_v2(m_send, &v);
        if (m_lib->send_get_no_connections && (++m_count % 30) == 1) m_receivers = m_lib->send_get_no_connections(m_send, 0);
    }

private:
    ndi::Lib *m_lib = nullptr;
    void *m_send = nullptr;
    QByteArray m_name;
    unsigned m_count = 0;
};

std::unique_ptr<CpuPublisher> createNdiPublisher() { return std::make_unique<NdiPublisher>(); }

// ---------------------------------------------------------------------------
// OMT (structures conformes à libomt.h, Open Media Transport, licence MIT)
// ---------------------------------------------------------------------------
namespace omt {
struct MediaFrame {
    int Type;
    int64_t Timestamp;
    int Codec;
    int Width, Height, Stride;
    int Flags;
    int FrameRateN, FrameRateD;
    float AspectRatio;
    int ColorSpace;
    int SampleRate, Channels, SamplesPerChannel;
    void *Data;
    int DataLength;
    void *CompressedData;
    int CompressedLength;
    void *FrameMetadata;
    int FrameMetadataLength;
};
constexpr int kVideo = 2;
constexpr int kBGRA = 0x41524742;
constexpr int kBT709 = 709;

struct Lib {
    bool ok = false;
    QString error, path;
    void *(*send_create)(const char *, int) = nullptr;
    void (*send_destroy)(void *) = nullptr;
    int (*send)(void *, MediaFrame *) = nullptr;
    int (*send_connections)(void *) = nullptr;
};

static Lib &lib(const QString &extra)
{
    static QMutex mutex;
    static Lib l;
    static bool tried = false;
    QMutexLocker lk(&mutex);
    if (tried && (l.ok || extra.isEmpty())) return l;
    tried = true;
    QString vmxName;
    const QStringList candidates = omtCandidates(extra, &vmxName);
    // libomt charge le codec libvmx : on le précharge depuis le même dossier.
    for (const QString &c : candidates) {
        if (!QFileInfo(c).isAbsolute() || !QFileInfo::exists(c)) continue;
        const QString vmx = QFileInfo(c).dir().filePath(vmxName);
        if (QFileInfo::exists(vmx)) {
            auto *v = new QLibrary(vmx);
            v->setLoadHints(QLibrary::ExportExternalSymbolsHint);
            if (!v->load()) delete v;
        }
        break;
    }
    QString triedPaths;
    QLibrary *q = loadFirst(candidates, &triedPaths);
    if (!q) {
        l.error = QStringLiteral("Bibliothèque OMT introuvable : placez libomt et libvmx (github.com/openmediatransport) "
                                 "à côté de l'application ou dans le dossier choisi.");
        return l;
    }
    l.path = q->fileName();
    l.send_create = reinterpret_cast<void *(*)(const char *, int)>(q->resolve("omt_send_create"));
    l.send_destroy = reinterpret_cast<void (*)(void *)>(q->resolve("omt_send_destroy"));
    l.send = reinterpret_cast<int (*)(void *, MediaFrame *)>(q->resolve("omt_send"));
    l.send_connections = reinterpret_cast<int (*)(void *)>(q->resolve("omt_send_connections"));
    if (!l.send_create || !l.send_destroy || !l.send) {
        l.error = QStringLiteral("Bibliothèque OMT incompatible : ") + l.path;
        return l;
    }
    l.ok = true;
    l.error.clear();
    return l;
}
} // namespace omt

class OmtPublisher : public CpuPublisher
{
public:
    ~OmtPublisher() override
    {
        if (m_send) m_lib->send_destroy(m_send);
    }
    bool start(const QString &name, const PublishSettings &s, QString *err) override
    {
#if defined(Q_OS_LINUX)
        // Sous Linux, libomt utilise Avahi pour la découverte et interrompt le programme s'il n'est pas lancé.
        if (!QFileInfo::exists(QStringLiteral("/run/avahi-daemon/socket"))
            && !QFileInfo::exists(QStringLiteral("/var/run/avahi-daemon/socket"))) {
            if (err) *err = QStringLiteral("OMT a besoin du service Avahi (avahi-daemon), qui n'est pas lancé.");
            return false;
        }
#endif
        m_lib = &omt::lib(s.libraryFolder);
        if (!m_lib->ok) {
            if (err) *err = m_lib->error;
            return false;
        }
        m_send = m_lib->send_create(name.toUtf8().constData(), s.omtQuality);
        if (!m_send) {
            if (err) *err = QStringLiteral("Création de la source OMT impossible.");
            return false;
        }
        return true;
    }
    void send(const CpuFrame &f) override
    {
        omt::MediaFrame m{};
        m.Type = omt::kVideo;
        m.Timestamp = f.timestamp100ns;
        m.Codec = omt::kBGRA;
        m.Width = f.width;
        m.Height = f.height;
        m.Stride = f.stride;
        m.FrameRateN = f.fpsN;
        m.FrameRateD = f.fpsD;
        m.AspectRatio = f.height ? float(f.width) / float(f.height) : 0.f;
        m.ColorSpace = omt::kBT709;
        m.Data = const_cast<uint8_t *>(f.bgra.data());
        m.DataLength = f.stride * f.height;
        m_lib->send(m_send, &m);
        if (m_lib->send_connections && (++m_count % 30) == 1) m_receivers = m_lib->send_connections(m_send);
    }

private:
    omt::Lib *m_lib = nullptr;
    void *m_send = nullptr;
    unsigned m_count = 0;
};

std::unique_ptr<CpuPublisher> createOmtPublisher() { return std::make_unique<OmtPublisher>(); }

// ---------------------------------------------------------------------------

class TapPublisher : public CpuPublisher
{
public:
    explicit TapPublisher(std::function<void(const CpuFrame &)> fn) : m_fn(std::move(fn)) {}
    bool start(const QString &, const PublishSettings &, QString *) override { return true; }
    void send(const CpuFrame &f) override { m_fn(f); }

private:
    std::function<void(const CpuFrame &)> m_fn;
};

std::unique_ptr<CpuPublisher> createTapPublisher(std::function<void(const CpuFrame &)> fn)
{
    return std::make_unique<TapPublisher>(std::move(fn));
}
