#pragma once
// Output publishing to other software / machines:
//  - Syphon (macOS) and Spout (Windows): GPU texture sharing, no copy;
//  - NDI and OMT (Open Media Transport): network. The image is read back from the GPU (asynchronously)
//    then sent by a dedicated thread so it never slows down rendering.
//
// NDI and OMT are loaded at runtime: Fulskrin builds and runs without them,
// publishing becomes available as soon as the library is installed on the machine.
//
// This file depends on no OpenGL header (the Syphon / Spout implementations have their own).

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
    QString name = QStringLiteral("Fulskrin");
    bool operator==(const PublishTarget &o) const { return enabled == o.enabled && name == o.name; }
    bool operator!=(const PublishTarget &o) const { return !(*this == o); }
};

struct PublishSettings {
    PublishTarget targets[kPublishKindCount];
    int omtQuality = 0;    // 0 = automatic, 1 = low, 50 = medium, 100 = high
    QString libraryFolder; // additional folder in which to look for the NDI / OMT libraries

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
    int receivers = -1; // -1 = unknown
};

// Available at compile time (Syphon / Spout) or always (NDI / OMT, loaded at runtime)
bool publishCompiledIn(PublishKind k);

// --- GPU publishing (in the render thread, OpenGL context current) ---------------------------
class GpuPublisher
{
public:
    virtual ~GpuPublisher() = default;
    virtual bool start(const QString &name, QString *err) = 0;
    virtual void publish(unsigned int texture, int width, int height) = 0; // GL_TEXTURE_2D texture, bottom-left origin
    virtual int receivers() const { return -1; }
};
std::unique_ptr<GpuPublisher> createSyphonPublisher(); // nullptr if not compiled in
std::unique_ptr<GpuPublisher> createSpoutPublisher();  // nullptr if not compiled in

// --- CPU publishing (in the send thread) -------------------------------------------------------
struct CpuFrame {
    std::vector<uint8_t> bgra; // rows top to bottom
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
// For tests: receives every frame sent
std::unique_ptr<CpuPublisher> createTapPublisher(std::function<void(const CpuFrame &)> fn);

// Send thread: receives read-back frames and dispatches them to CPU publishers.
// Three rotating buffers: NDI asynchronous send holds a frame until the next send.
class CpuSendThread
{
public:
    CpuSendThread();
    ~CpuSendThread();
    CpuFrame *acquire();          // free buffer (or the pending frame, replaced), never blocks
    void submit(CpuFrame *f);
    void setPublishers(std::vector<std::shared_ptr<CpuPublisher>> pubs); // destroyed in the send thread
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

// Path of the library found (empty if missing) — for display
QString ndiLibraryPath(const QString &extraFolder);
QString omtLibraryPath(const QString &extraFolder);
