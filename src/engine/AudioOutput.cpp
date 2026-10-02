#include "AudioOutput.h"
#include "AudioStream.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>

struct AudioOutputPrivate {
    ma_context context{};
    ma_device device{};
    bool contextOk = false, deviceOk = false;
    QString name;

    static void callback(ma_device *dev, void *output, const void *, ma_uint32 frames)
    {
        auto *self = static_cast<AudioOutput *>(dev->pUserData);
        self->render(static_cast<float *>(output), int(frames));
    }
};

AudioOutput::AudioOutput() : d(std::make_unique<AudioOutputPrivate>()) {}

AudioOutput::~AudioOutput() { stop(); }

QStringList AudioOutput::deviceNames()
{
    QStringList names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info *playback = nullptr, *capture = nullptr;
    ma_uint32 nPlayback = 0, nCapture = 0;
    if (ma_context_get_devices(&ctx, &playback, &nPlayback, &capture, &nCapture) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < nPlayback; ++i) {
            const QString n = QString::fromUtf8(playback[i].name);
            if (playback[i].isDefault) names.prepend(n);
            else names.append(n);
        }
    }
    ma_context_uninit(&ctx);
    names.removeDuplicates();
    return names;
}

bool AudioOutput::start(const QString &device, QString *err, bool nullDevice)
{
    stop();
    ma_backend nullBackend[] = {ma_backend_null};
    if (ma_context_init(nullDevice ? nullBackend : nullptr, nullDevice ? 1 : 0, nullptr, &d->context) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("No audio system available.");
        return false;
    }
    d->contextOk = true;

    ma_device_id id{};
    bool haveId = false;
    if (!device.isEmpty() && !nullDevice) {
        ma_device_info *playback = nullptr, *capture = nullptr;
        ma_uint32 nPlayback = 0, nCapture = 0;
        if (ma_context_get_devices(&d->context, &playback, &nPlayback, &capture, &nCapture) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < nPlayback; ++i)
                if (QString::fromUtf8(playback[i].name) == device) {
                    id = playback[i].id;
                    haveId = true;
                    break;
                }
        }
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.playback.pDeviceID = haveId ? &id : nullptr;
    cfg.sampleRate = kSampleRate;
    cfg.dataCallback = &AudioOutputPrivate::callback;
    cfg.pUserData = this;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(&d->context, &cfg, &d->device) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("Unable to open the audio output") + (device.isEmpty() ? QString() : " \"" + device + "\"") + ".";
        stop();
        return false;
    }
    d->deviceOk = true;
    d->name = QString::fromUtf8(d->device.playback.name);
    if (ma_device_start(&d->device) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("Unable to start the audio output \"%1\".").arg(d->name);
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop()
{
    if (d->deviceOk) ma_device_uninit(&d->device); // waits for the callback to return
    if (d->contextOk) ma_context_uninit(&d->context);
    d->deviceOk = d->contextOk = false;
    d->name.clear();
    m_peak[0] = m_peak[1] = 0.0f;
}

bool AudioOutput::isRunning() const { return d->deviceOk && ma_device_is_started(&d->device); }

QString AudioOutput::deviceName() const { return d->name; }

double AudioOutput::latency() const
{
    if (!d->deviceOk) return 0;
    const auto &p = d->device.playback;
    const double rate = p.internalSampleRate > 0 ? p.internalSampleRate : kSampleRate;
    return double(p.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, p.internalPeriods) / rate;
}

void AudioOutput::addStream(const std::shared_ptr<AudioStream> &s)
{
    if (!s) return;
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    if (std::find(m_streams.begin(), m_streams.end(), s) == m_streams.end()) m_streams.push_back(s);
}

void AudioOutput::removeStream(const AudioStream *s)
{
    // Once this returns, the callback no longer uses the stream (it mixes under the same lock).
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    m_streams.erase(std::remove_if(m_streams.begin(), m_streams.end(), [s](const auto &p) { return p.get() == s; }),
                    m_streams.end());
}

void AudioOutput::setTap(std::function<void(const float *, int)> fn)
{
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    m_tap = std::move(fn);
}

void AudioOutput::render(float *out, int frames)
{
    std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    const double lat = latency();
    for (const auto &s : m_streams) s->mix(out, frames, lat);

    const float goal = m_muted ? 0.0f : std::max(0.0f, m_masterVolume.load());
    const float ramp = 1.0f / (0.01f * kSampleRate);
    float pk[2] = {0, 0};
    for (int i = 0; i < frames; ++i) {
        m_gain = m_gain < goal ? std::min(goal, m_gain + ramp) : std::max(goal, m_gain - ramp);
        for (int c = 0; c < 2; ++c) {
            float &v = out[i * 2 + c];
            v = std::clamp(v * m_gain, -1.0f, 1.0f);
            pk[c] = std::max(pk[c], std::abs(v));
        }
    }
    for (int c = 0; c < 2; ++c) m_peak[c] = std::max(pk[c], m_peak[c].load() * 0.9f);
    if (m_tap) m_tap(out, frames);
}
