#pragma once
// FFmpeg video decoding on a dedicated thread.
// Frames are converted to RGBA, flipped vertically (OpenGL convention)
// and queued with a monotonic timestamp (loops accumulate).

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

    // File info without decoding it (media bin)
    struct Info {
        int width = 0, height = 0;
        double duration = 0, fps = 0;
        QString codec;
    };
    static bool probe(const QString &path, Info *info, QString *err = nullptr);
    void close();

    int width() const { return m_width; }
    int height() const { return m_height; }
    double duration() const { return m_duration; }
    double fps() const { return m_fps; }
    QString codecName() const { return m_codecName; }

    void setLoop(bool on) { m_loop = on; }
    bool loop() const { return m_loop; }

    // Requests a seek (asynchronous). After seek(t), the player clock must equal t.
    void seek(double t);

    // Fetches the most recent frame whose timestamp <= t. Returns true if a new frame was written to out.
    bool fetch(double t, std::vector<uint8_t> &out, int *w, int *h);

    // True when non-looping playback has reached the end and the queue is empty.
    bool finished();

private:
    struct Frame {
        double pts = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    void run();
    void doSeek(double t);
    int decodeNext(Frame &f); // 1 = frame, 0 = end, -1 = error
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

    // Decode thread state
    bool m_draining = false;
    double m_loopBase = 0, m_lastPts = 0, m_discardBefore = -1e9;
};
