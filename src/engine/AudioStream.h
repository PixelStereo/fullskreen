#pragma once
// Audio track of a file (audio file, or sound of a video file), decoded with FFmpeg on a dedicated thread,
// resampled to the output format (stereo float, output sample rate) and mixed by AudioOutput.
//
// Synchronization: the stream follows the layer's transport (position, play / pause, speed, loop),
// which the engine updates every frame. In the audio callback, the stream extrapolates the layer position
// to the moment the samples will actually be heard (output latency), corrects small drifts by
// adjusting its playback rate very slightly (at most 0.5 %, inaudible), and resynchronizes (short fade,
// then seek) when the gap is too large: seek, speed change, render stall.

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
struct SwrContext;

class AudioStream
{
public:
    AudioStream() = default;
    ~AudioStream();
    AudioStream(const AudioStream &) = delete;
    AudioStream &operator=(const AudioStream &) = delete;

    struct Info {
        double duration = 0;
        int sampleRate = 0, channels = 0;
        QString codec;
    };
    // Audio track info without decoding it. Returns false if the file has no audio track.
    static bool probe(const QString &path, Info *info, QString *err = nullptr);

    // Opens the best audio track of the file. Returns false (with err) if there is none.
    bool open(const QString &path, int outputRate, QString *err);
    void close();
    const Info &info() const { return m_info; }
    double duration() const { return m_info.duration; }

    // Engine side (any thread), every frame: layer transport and gain (volume, mute, layer on/off).
    // `stampNs`: steady_clock time at which `position` was exact (0 = now).
    void setTransport(double position, bool playing, double speed, bool loop, float gain, int64_t stampNs = 0);
    static int64_t clockNs(); // steady_clock, nanoseconds

    // Audio callback: adds `frames` stereo interleaved samples into `out`.
    // `latency` = time between this call and the moment the samples are heard (seconds).
    void mix(float *out, int frames, double latency);

    // Meters and diagnostics (any thread)
    float peak() const { return m_peak.load(); }
    double syncError() const { return m_syncError.load(); } // heard position - expected position (s)
    int resyncCount() const { return m_resyncs.load(); }

private:
    struct Chunk {
        double pts = 0;            // monotonic (loops accumulate), seconds
        uint64_t generation = 0;
        std::vector<float> samples; // stereo interleaved
    };
    struct Transport {
        double position = 0, speed = 1;
        bool playing = false, loop = true;
        float gain = 1;
        int64_t stampNs = 0;
    };

    void run();
    void doSeek(double t);
    bool decodeFrame();           // false = end of file or error
    void pushSamples(const float *s, int frames, double pts);
    void pushSilence(int frames, double pts);
    void requestSeek(double t);   // audio callback
    double wrap(double t) const;  // position in [0, duration) when looping

    // Decoder (decode thread)
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwrContext *m_swr = nullptr;
    int m_stream = -1;
    double m_timeBase = 0, m_startTime = 0;
    int m_rate = 48000;
    Info m_info;
    bool m_draining = false;
    double m_loopBase = 0, m_nextPts = 0; // m_nextPts: local time of the next decoded sample
    std::vector<float> m_convert;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Chunk> m_queue;
    std::vector<std::vector<float>> m_pool; // recycled buffers (no allocation in the audio callback)
    size_t m_queuedFrames = 0;
    bool m_quit = false, m_seekPending = false, m_eof = false;
    double m_seekTarget = 0;
    uint64_t m_generation = 0;
    std::atomic<bool> m_loop{true};

    // Transport (written by the engine, read by the audio callback)
    std::mutex m_transportMutex;
    Transport m_transport;

    // Mixer state (audio callback only)
    Transport m_snapshot;
    std::vector<float> m_buf; // stereo interleaved, m_bufFrames frames
    size_t m_bufFrames = 0;
    double m_bufPos = 0;      // fractional read index in m_buf
    double m_bufPts = 0;      // monotonic time of m_buf frame 0
    uint64_t m_mixGeneration = 0;
    bool m_aligning = true;   // waiting for data at the target position (after open or seek)
    bool m_fadingForSeek = false;
    float m_gain = 0;         // current gain (ramped)
    double m_drift = 0;       // filtered gap between the layer and the stream (s)

    std::atomic<float> m_peak{0};
    std::atomic<double> m_syncError{0};
    std::atomic<int> m_resyncs{0};
};
