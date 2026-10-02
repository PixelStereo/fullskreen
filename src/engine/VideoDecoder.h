#pragma once
// FFmpeg video decoding on a dedicated thread.
// Frames are converted to RGBA, flipped vertically (OpenGL convention) and queued, stamped with the
// clock of the layer's Timeline (monotonic whatever the direction or the play mode).
// Backward legs (negative speed, ping-pong) are produced in short windows: seek to the keyframe, decode
// forward, deliver the frames in reverse order — bounded memory whatever the GOP length.

#include "Timeline.h"

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

    // Timeline followed by the next seek (mode, origin, direction). Its duration is set by the decoder.
    void setTimeline(const Timeline &t);

    // Repositions on the clock `c` of the timeline (asynchronous): frames are then stamped from there.
    void seek(double c);

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
    int decodeNext(Frame &f, double skipBefore = -1e9); // 1 = frame, 2 = skipped (before skipBefore), 0 = end, -1 = error
    void produceBackwardWindow(); // decode thread, backward leg
    void startLeg(const Timeline::Leg &leg, double position); // decode thread
    void finishLeg();                                          // decode thread: next leg, or end
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
    Timeline m_timeline, m_pendingTimeline; // pending: applied at the next seek

    // Decode thread state
    bool m_draining = false;
    double m_lastPts = 0, m_discardBefore = -1e9;
    Timeline::Leg m_leg;            // leg being produced
    std::vector<Frame> m_backStack; // backward leg: frames still to deliver (reverse order)
    double m_backEnd = 0;           // backward leg: end (position) of the next window
    bool m_legDone = false;
};
