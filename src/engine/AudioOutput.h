#pragma once
// Audio output: one device (miniaudio), stereo float at a fixed sample rate.
// The device callback mixes every registered AudioStream, applies the composition volume and feeds the meters.

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class AudioStream;
struct AudioOutputPrivate;

class AudioOutput
{
public:
    static constexpr int kSampleRate = 48000;

    AudioOutput();
    ~AudioOutput();

    // Playback devices of the system (names), the default one first.
    static QStringList deviceNames();

    // Opens the device named `device` (empty = system default). nullDevice: no sound card (tests),
    // the callback runs in real time anyway. Returns false with err if no device could be opened.
    bool start(const QString &device, QString *err, bool nullDevice = false);
    void stop();
    bool isRunning() const;
    QString deviceName() const;   // device actually opened
    double latency() const;       // seconds between the callback and the speakers (estimate)

    void addStream(const std::shared_ptr<AudioStream> &s);
    void removeStream(const AudioStream *s);

    void setVolume(float v) { m_volume = v; }
    float volume() const { return m_volume.load(); }
    // Blackout: the mix fades to `target` (0..1) in `seconds` seconds, ramped in the audio thread
    void fadeTo(float target, double seconds);
    float fadeLevel() const { return m_fadeLevel.load(); }

    // Peak levels of the mix (0..1+), with decay
    float peak(int channel) const { return m_peak[channel & 1].load(); }

    // Tests: receives every mixed buffer (audio thread)
    void setTap(std::function<void(const float *stereo, int frames)> fn);

private:
    friend struct AudioOutputPrivate;
    void render(float *out, int frames);

    std::unique_ptr<AudioOutputPrivate> d;
    std::mutex m_streamsMutex;
    std::vector<std::shared_ptr<AudioStream>> m_streams;
    std::function<void(const float *, int)> m_tap;
    std::atomic<float> m_volume{1.0f};
    float m_gain = 1.0f; // ramped composition gain (audio thread)
    std::atomic<float> m_fadeTarget{1.0f}, m_fadeStep{1.0f}; // blackout: target and step per sample
    std::atomic<float> m_fadeLevel{1.0f};
    std::atomic<float> m_peak[2] = {0.0f, 0.0f};
};
