#pragma once
// Publication de la sortie vers d'autres logiciels / machines :
//  - Syphon (macOS) et Spout (Windows) : partage de texture GPU, sans copie ;
//  - NDI et OMT (Open Media Transport) : réseau. L'image est relue depuis le GPU (asynchrone)
//    puis envoyée par un fil dédié pour ne jamais ralentir le rendu.
//
// NDI et OMT sont chargés à l'exécution : Lanterne se compile et fonctionne sans eux,
// la publication devient disponible dès que la bibliothèque est installée sur la machine.
//
// Ce fichier ne dépend d'aucun en-tête OpenGL (les implémentations Syphon / Spout ont les leurs).

#include <QJsonObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

enum class PublishKind { Ndi = 0, Omt = 1, Syphon = 2, Spout = 3 };
constexpr int kPublishKindCount = 4;
QString publishKindName(PublishKind k);

struct PublishTarget {
    bool enabled = false;
    QString name = QStringLiteral("Lanterne");
    bool operator==(const PublishTarget &o) const { return enabled == o.enabled && name == o.name; }
    bool operator!=(const PublishTarget &o) const { return !(*this == o); }
};

struct PublishSettings {
    PublishTarget targets[kPublishKindCount];
    int omtQuality = 0;    // 0 = automatique, 1 = basse, 50 = moyenne, 100 = haute
    QString libraryFolder; // dossier supplémentaire où chercher les bibliothèques NDI / OMT

    PublishTarget &operator[](PublishKind k) { return targets[int(k)]; }
    const PublishTarget &operator[](PublishKind k) const { return targets[int(k)]; }
    bool operator==(const PublishSettings &o) const;
    bool operator!=(const PublishSettings &o) const { return !(*this == o); }
    QJsonObject toJson() const;
    static PublishSettings fromJson(const QJsonObject &o);
};

struct PublishState {
    enum Level { Off, Ok, Error, Unavailable };
    Level level = Off;
    QString text;
    int receivers = -1; // -1 = inconnu
};

// Disponible à la compilation (Syphon / Spout) ou toujours (NDI / OMT, chargés à l'exécution)
bool publishCompiledIn(PublishKind k);

// --- Publication GPU (dans le fil de rendu, contexte OpenGL courant) -------------------------
class GpuPublisher
{
public:
    virtual ~GpuPublisher() = default;
    virtual bool start(const QString &name, QString *err) = 0;
    virtual void publish(unsigned int texture, int width, int height) = 0; // texture GL_TEXTURE_2D, origine en bas
    virtual int receivers() const { return -1; }
};
std::unique_ptr<GpuPublisher> createSyphonPublisher(); // nullptr si non compilé
std::unique_ptr<GpuPublisher> createSpoutPublisher();  // nullptr si non compilé

// --- Publication CPU (dans le fil d'envoi) ----------------------------------------------------
struct CpuFrame {
    std::vector<uint8_t> bgra; // lignes de haut en bas
    int width = 0, height = 0, stride = 0;
    int64_t timestamp100ns = 0;
    int fpsN = 60, fpsD = 1;
};

class CpuPublisher
{
public:
    virtual ~CpuPublisher() = default;
    virtual bool start(const QString &name, const PublishSettings &s, QString *err) = 0;
    virtual void send(const CpuFrame &f) = 0;
    int receivers() const { return m_receivers.load(); }

protected:
    std::atomic<int> m_receivers{-1};
};
std::unique_ptr<CpuPublisher> createNdiPublisher();
std::unique_ptr<CpuPublisher> createOmtPublisher();
// Pour les tests : reçoit chaque image envoyée
std::unique_ptr<CpuPublisher> createTapPublisher(std::function<void(const CpuFrame &)> fn);

// Fil d'envoi : reçoit les images relues, les distribue aux publications CPU.
// Trois tampons tournants : l'envoi asynchrone NDI garde une image jusqu'à l'envoi suivant.
class CpuSendThread
{
public:
    CpuSendThread();
    ~CpuSendThread();
    CpuFrame *acquire();          // tampon libre (ou l'image en attente, remplacée), jamais bloquant
    void submit(CpuFrame *f);
    void setPublishers(std::vector<std::shared_ptr<CpuPublisher>> pubs); // détruits dans le fil d'envoi
    bool hasPublishers() const { return m_active.load(); }
    uint64_t sentFrames() const { return m_sent.load(); }

private:
    void run();
    std::mutex m_mutex;
    std::condition_variable m_cv;
    CpuFrame m_pool[3];
    std::vector<CpuFrame *> m_free;
    CpuFrame *m_pending = nullptr, *m_inflight = nullptr;
    std::vector<std::shared_ptr<CpuPublisher>> m_pubs, m_next;
    bool m_changed = false, m_quit = false;
    std::atomic<bool> m_active{false};
    std::atomic<uint64_t> m_sent{0};
    std::thread m_thread;
};

// Chemin de la bibliothèque trouvée (vide si absente) — pour l'affichage
QString ndiLibraryPath(const QString &extraFolder);
QString omtLibraryPath(const QString &extraFolder);
