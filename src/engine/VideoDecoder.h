#pragma once
// Décodage vidéo FFmpeg dans un thread dédié.
// Les images sont converties en RGBA, retournées verticalement (convention OpenGL)
// et mises en file avec un horodatage monotone (les boucles s'additionnent).

#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open(const QString &path, QString *err);
    void close();

    int width() const { return m_width; }
    int height() const { return m_height; }
    double duration() const { return m_duration; }
    double fps() const { return m_fps; }
    QString codecName() const { return m_codecName; }

    void setLoop(bool on) { m_loop = on; }
    bool loop() const { return m_loop; }

    // Demande un positionnement (asynchrone). Après seek(t), l'horloge du lecteur doit valoir t.
    void seek(double t);

    // Récupère l'image la plus récente dont l'horodatage <= t. Retourne true si une nouvelle image a été écrite dans out.
    bool fetch(double t, std::vector<uint8_t> &out, int *w, int *h);

    // Vrai quand la lecture sans boucle est arrivée au bout et que la file est vide.
    bool finished();

private:
    struct Frame {
        double pts = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    void run();
    void doSeek(double t);
    int decodeNext(Frame &f); // 1 = image, 0 = fin, -1 = erreur
    void recycle(std::vector<uint8_t> &&buf);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    int m_stream = -1;
    double m_timeBase = 0, m_startTime = 0;

    int m_width = 0, m_height = 0;
    double m_duration = 0, m_fps = 25;
    QString m_codecName;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Frame> m_queue;
    std::vector<std::vector<uint8_t>> m_pool;
    bool m_quit = false, m_seekPending = false, m_eof = false, m_needFirst = true;
    double m_seekTarget = 0;
    uint64_t m_generation = 0;
    std::atomic<bool> m_loop{true};

    // État du thread de décodage
    bool m_draining = false;
    double m_loopBase = 0, m_lastPts = 0, m_discardBefore = -1e9;
};
