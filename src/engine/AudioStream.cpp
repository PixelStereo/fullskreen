#include "AudioStream.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
constexpr double kBufferSeconds = 0.5;   // decoded audio kept ahead of the mixer
constexpr double kJumpThreshold = 0.04;  // gap beyond which the stream jumps (fade out, realign, fade in)
constexpr double kSeekThreshold = 0.5;   // realigning further than this needs a seek in the file
constexpr double kMaxRateCorrection = 0.005;
constexpr double kFadeSeconds = 0.008;

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString avErr(int code)
{
    char buf[256] = {0};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

double containerDuration(AVFormatContext *fmt, AVStream *st)
{
    if (fmt->duration > 0) return double(fmt->duration) / AV_TIME_BASE;
    if (st->duration > 0) return st->duration * av_q2d(st->time_base);
    return 0;
}
} // namespace

AudioStream::~AudioStream() { close(); }

bool AudioStream::probe(const QString &path, Info *info, QString *err)
{
    AVFormatContext *fmt = nullptr;
    const QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = avErr(r);
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s < 0) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("No audio track");
        return false;
    }
    AVStream *st = fmt->streams[s];
    if (info) {
        info->sampleRate = st->codecpar->sample_rate;
        info->channels = st->codecpar->ch_layout.nb_channels;
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        info->codec = d ? QString::fromUtf8(d->name) : QString();
        info->duration = containerDuration(fmt, st);
    }
    avformat_close_input(&fmt);
    return true;
}

bool AudioStream::open(const QString &path, int outputRate, QString *err)
{
    close();
    m_rate = outputRate > 0 ? outputRate : 48000;
    const QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&m_fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = QStringLiteral("Unable to open audio: ") + avErr(r);
        m_fmt = nullptr;
        return false;
    }
    avformat_find_stream_info(m_fmt, nullptr);
    const AVCodec *dec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (m_stream < 0 || !dec) {
        if (err) *err = QStringLiteral("No decodable audio track.");
        close();
        return false;
    }
    AVStream *st = m_fmt->streams[m_stream];
    // Only the audio track is read: the other streams are skipped by the demuxer.
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (int(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;
    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, st->codecpar);
    r = avcodec_open2(m_codec, dec, nullptr);
    if (r < 0) {
        if (err) *err = QStringLiteral("Audio decoder unavailable: ") + avErr(r);
        close();
        return false;
    }

    AVChannelLayout in;
    if (m_codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || m_codec->ch_layout.nb_channels <= 0)
        av_channel_layout_default(&in, std::max(1, m_codec->ch_layout.nb_channels));
    else
        av_channel_layout_copy(&in, &m_codec->ch_layout);
    AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
    r = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_FLT, m_rate, &in, m_codec->sample_fmt, m_codec->sample_rate, 0,
                            nullptr);
    av_channel_layout_uninit(&in);
    if (r < 0 || !m_swr || swr_init(m_swr) < 0) {
        if (err) *err = QStringLiteral("Audio resampler unavailable.");
        close();
        return false;
    }

    m_info.sampleRate = m_codec->sample_rate;
    m_info.channels = m_codec->ch_layout.nb_channels;
    m_info.codec = QString::fromUtf8(dec->name);
    m_info.duration = containerDuration(m_fmt, st);
    m_timeBase = av_q2d(st->time_base);
    m_startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time * m_timeBase : 0.0;

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    // Mixer buffers allocated once: the audio callback never allocates.
    m_buf.assign(size_t(m_rate * 2.0) * 2, 0.0f);
    m_bufFrames = 0;
    m_bufPos = 0;
    m_bufPts = 0;
    m_pool.reserve(256);
    m_aligning = true;
    m_fadingForSeek = false;
    m_gain = 0;

    m_quit = false;
    m_eof = false;
    m_seekPending = false;
    m_generation = m_mixGeneration = 0;
    m_queuedFrames = 0;
    m_draining = false;
    m_nextPts = 0;
    m_timeline = m_seekTimeline = Timeline{};
    m_timeline.duration = m_seekTimeline.duration = m_info.duration;
    m_leg = m_timeline.firstLeg();
    m_mixTimelineId = ~0ull;
    m_timelineChanged = false;
    m_thread = std::thread(&AudioStream::run, this);
    return true;
}

void AudioStream::close()
{
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    m_queue.clear();
    m_pool.clear();
    m_queuedFrames = 0;
    if (m_swr) swr_free(&m_swr);
    if (m_frame) av_frame_free(&m_frame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt) avformat_close_input(&m_fmt);
    m_stream = -1;
}

int64_t AudioStream::clockNs() { return nowNs(); }

void AudioStream::setTransport(double clock, bool playing, double speed, const Timeline &timeline,
                               uint64_t timelineId, float gain, int64_t stampNs)
{
    std::lock_guard<std::mutex> lk(m_transportMutex);
    m_transport.clock = clock;
    m_transport.playing = playing;
    m_transport.speed = speed;
    m_transport.timeline = timeline;
    m_transport.timeline.duration = m_info.duration;
    m_transport.timelineId = timelineId;
    m_transport.gain = gain;
    m_transport.stampNs = stampNs ? stampNs : nowNs();
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

void AudioStream::doSeek(double t)
{
    const int64_t ts = int64_t((t + m_startTime) / m_timeBase);
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(m_fmt, -1, int64_t(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    swr_close(m_swr);
    swr_init(m_swr);
    m_draining = false;
    m_nextPts = t;
}

void AudioStream::pushSamples(const float *s, int frames, double pts)
{
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_seekPending) return; // obsolete: a seek is waiting
    Chunk c;
    c.pts = pts;
    c.generation = m_generation;
    if (!m_pool.empty()) {
        c.samples = std::move(m_pool.back());
        m_pool.pop_back();
    }
    c.samples.assign(s, s + size_t(frames) * 2);
    m_queuedFrames += size_t(frames);
    m_queue.push_back(std::move(c));
}

void AudioStream::pushSilence(int frames, double pts)
{
    m_convert.assign(size_t(frames) * 2, 0.0f);
    pushSamples(m_convert.data(), frames, pts);
}

bool AudioStream::decodeRaw(const float **samples, int *frames, double *local)
{
    for (;;) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) {
            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
            double t = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_nextPts;
            const int maxOut = swr_get_out_samples(m_swr, m_frame->nb_samples) + 32;
            if (m_convert.size() < size_t(maxOut) * 2) m_convert.resize(size_t(maxOut) * 2);
            uint8_t *outp = reinterpret_cast<uint8_t *>(m_convert.data());
            int n = swr_convert(m_swr, &outp, maxOut, const_cast<const uint8_t **>(m_frame->extended_data),
                                m_frame->nb_samples);
            av_frame_unref(m_frame);
            if (n <= 0) continue;
            const float *s = m_convert.data();
            if (t < 0) { // before the start of the file (encoder priming)
                const int skip = std::min(n, int(std::ceil(-t * m_rate)));
                s += size_t(skip) * 2;
                n -= skip;
                t = 0;
            }
            if (n <= 0) continue;
            *samples = s;
            *frames = n;
            *local = t;
            return true;
        }
        if (r == AVERROR_EOF) return false;
        if (r != AVERROR(EAGAIN)) return false;
        if (m_draining) return false;
        r = av_read_frame(m_fmt, m_packet);
        if (r < 0) {
            avcodec_send_packet(m_codec, nullptr); // flush
            m_draining = true;
            continue;
        }
        if (m_packet->stream_index == m_stream) avcodec_send_packet(m_codec, m_packet);
        av_packet_unref(m_packet);
    }
}

double AudioStream::legClock(double position) const { return m_leg.clockStart + position - m_leg.from; }

// Forward leg: next decoded samples. False at the end of the leg (end of the file, or the layer's length
// reached when the mode repeats: the cycle is exactly as long as the layer).
bool AudioStream::decodeFrame()
{
    const double d = m_info.duration;
    const bool bounded = d > 0; // the leg ends at the out point (or the end of the media)
    const float *s = nullptr;
    int n = 0;
    double local = 0;
    for (;;) {
        if (!decodeRaw(&s, &n, &local)) return false;
        if (local < m_trimBefore) { // before the start of the leg (the seek lands on an earlier packet)
            const int skip = std::min(n, int(std::ceil((m_trimBefore - local) * m_rate)));
            s += size_t(skip) * 2;
            n -= skip;
            local += double(skip) / m_rate;
            if (n <= 0) continue;
        }
        if (bounded) {
            if (local >= m_leg.to) return false;
            n = std::min(n, int(std::ceil((m_leg.to - local) * m_rate)));
        }
        if (n <= 0) continue;
        m_nextPts = local + double(n) / m_rate;
        pushSamples(s, n, legClock(local));
        return true;
    }
}

void AudioStream::startLeg(const Timeline::Leg &leg, double position)
{
    m_leg = leg;
    if (leg.forward) {
        doSeek(position);
        m_trimBefore = position;
    } else {
        m_backEnd = position;
    }
}

// Backward leg: samples of [a, m_backEnd) reversed, a = 0.25 s earlier. Silence where the track has none.
// Clock of position L on that leg: clockStart + from - L.
void AudioStream::produceBackwardWindow()
{
    const double a = std::max(m_leg.to, m_backEnd - 0.25);
    const int n = int(std::lround((m_backEnd - a) * m_rate));
    m_window.assign(size_t(std::max(n, 0)) * 2, 0.0f);
    doSeek(a);
    const float *s = nullptr;
    int frames = 0;
    double local = 0;
    while (n > 0 && decodeRaw(&s, &frames, &local)) {
        const long first = std::lround((local - a) * m_rate);
        if (first >= n) break;
        for (int k = 0; k < frames; ++k) {
            const long idx = first + k;
            if (idx < 0) continue;
            if (idx >= n) break;
            m_window[size_t(idx) * 2] = s[k * 2];
            m_window[size_t(idx) * 2 + 1] = s[k * 2 + 1];
        }
    }
    for (int k = 0; k < n / 2; ++k) { // reverse the frame order (channels stay in place)
        std::swap(m_window[size_t(k) * 2], m_window[size_t(n - 1 - k) * 2]);
        std::swap(m_window[size_t(k) * 2 + 1], m_window[size_t(n - 1 - k) * 2 + 1]);
    }
    if (n > 0) pushSamples(m_window.data(), n, m_leg.clockStart + m_leg.from - m_backEnd);
    m_backEnd = a;
}

void AudioStream::run()
{
    const size_t maxFrames = size_t(m_rate * kBufferSeconds);
    bool atEnd = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] { return m_quit || m_seekPending || (!m_eof && m_queuedFrames < maxFrames); });
            if (m_quit) return;
            if (m_seekPending) {
                const double c = m_seekTarget;
                m_timeline = m_seekTimeline;
                lk.unlock();
                startLeg(m_timeline.legAt(c), m_timeline.position(c));
                atEnd = false;
                lk.lock();
                m_seekPending = false; // chunks pushed from now on belong to the new generation
                continue;
            }
        }
        const double d = m_info.duration;
        if (!m_leg.forward) {
            if (m_backEnd > m_leg.to + 1e-9) {
                produceBackwardWindow();
                continue;
            }
        } else {
            if (!atEnd) {
                if (decodeFrame()) continue;
                atEnd = true;
            }
            if (m_timeline.mode != Timeline::Once && d > 0 && m_nextPts < m_leg.to - 0.5 / m_rate) {
                // Audio track shorter than the layer: silence until the end of the leg, in small chunks.
                const int frames = std::max(1, std::min(int(m_rate * 0.1), int(std::ceil((m_leg.to - m_nextPts) * m_rate))));
                pushSilence(frames, legClock(m_nextPts));
                m_nextPts += double(frames) / m_rate;
                continue;
            }
        }
        // End of the leg: the next one, or the end of playback
        if (m_timeline.mode == Timeline::Once || d <= 0) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_eof = true;
            continue;
        }
        const Timeline::Leg next = m_timeline.nextLeg(m_leg);
        startLeg(next, next.from);
        atEnd = false;
    }
}

// ---------------------------------------------------------------------------
// Audio callback
// ---------------------------------------------------------------------------

void AudioStream::requestSeek(double t)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_generation;
        m_mixGeneration = m_generation;
        m_seekTarget = std::max(0.0, t);
        m_seekTimeline = m_snapshot.timeline;
        m_seekPending = true;
        m_eof = false;
        for (Chunk &c : m_queue) m_pool.push_back(std::move(c.samples));
        m_queue.clear();
        m_queuedFrames = 0;
    }
    m_cv.notify_all();
    m_bufFrames = 0;
    m_bufPos = 0;
    m_aligning = true;
    m_fadingForSeek = false;
    m_timelineChanged = false;
    ++m_resyncs;
}

void AudioStream::mix(float *out, int frames, double latency)
{
    if (m_transportMutex.try_lock()) { // never wait in the audio callback: keep the previous state if busy
        m_snapshot = m_transport;
        m_transportMutex.unlock();
    }
    const Transport &t = m_snapshot;
    const double d = m_info.duration;
    const bool running = t.playing && t.speed > 0;

    // Layer clock at the moment these samples will be heard
    double target = t.clock;
    if (running) target += (double(nowNs() - t.stampNs) / 1e9 + latency) * t.speed;
    const bool once = t.timeline.mode == Timeline::Once && d > 0;
    if (once) target = std::min(target, t.timeline.firstLeg().clockEnd());
    auto gap = [&](double readPts) { return target - readPts; }; // > 0: the stream is behind the layer

    // The engine repositioned the layer (seek, direction, mode): what is buffered belongs to the old timeline
    if (t.timelineId != m_mixTimelineId) {
        m_mixTimelineId = t.timelineId;
        if (m_gain > 0 && !m_aligning) {
            m_fadingForSeek = true; // short fade out, then seek
            m_timelineChanged = true;
        } else {
            requestSeek(target);
        }
    }

    // 1. Pull decoded chunks into the mixer buffer
    const size_t capacity = m_buf.size() / 2;
    const size_t want = size_t(frames * std::max(1.0, t.speed) * 1.02) + 4;
    bool pulled = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        while (!m_queue.empty()) {
            const size_t avail = m_bufFrames - std::min(m_bufFrames, size_t(m_bufPos));
            if (avail >= want) break;
            Chunk &c = m_queue.front();
            const size_t n = c.samples.size() / 2;
            if (c.generation == m_mixGeneration) {
                // Aligning: a chunk that ends before the target position is useless
                const bool behind = m_aligning && m_bufFrames == 0 && gap(c.pts) >= double(n) / m_rate;
                if (!behind) {
                    if (m_bufFrames + n > capacity) break;
                    if (m_bufFrames == 0) {
                        m_bufPts = c.pts;
                        m_bufPos = 0;
                    }
                    std::memcpy(m_buf.data() + m_bufFrames * 2, c.samples.data(), n * 2 * sizeof(float));
                    m_bufFrames += n;
                }
            }
            m_queuedFrames -= std::min(m_queuedFrames, n);
            m_pool.push_back(std::move(c.samples));
            m_queue.pop_front();
            pulled = true;
        }
    }
    if (pulled) m_cv.notify_one();

    // 2. After open or seek: skip to the target position
    if (m_aligning && m_bufFrames > 0) {
        double g = gap(m_bufPts + m_bufPos / m_rate);
        if (g > 0) {
            m_bufPos = std::min(double(m_bufFrames), m_bufPos + g * m_rate);
            g = gap(m_bufPts + m_bufPos / m_rate);
        }
        if (m_bufPos + 1 < m_bufFrames && std::abs(g) <= 2.0 / m_rate) {
            m_aligning = false;
            m_drift = 0;
            m_gain = 0; // short fade in
        } else if (g < -kSeekThreshold || g > kSeekThreshold) {
            requestSeek(target); // too far from the buffered data
        }
    }

    // 3. Drift: small gaps are absorbed by the playback rate, large ones trigger a resync
    double drift = 0;
    if (!m_aligning && m_bufFrames > 0) {
        const double raw = gap(m_bufPts + m_bufPos / m_rate);
        const bool ended = once && target >= t.timeline.firstLeg().clockEnd();
        if (std::abs(raw) > kJumpThreshold && !ended) m_fadingForSeek = true; // seek, audio dropout, stall
        // The measure jitters with callback scheduling: correct on a smoothed value
        m_drift += (raw - m_drift) * 0.05;
        drift = m_drift;
        m_syncError = -drift;
    }
    const double speed = std::max(0.0, t.speed);
    const double step = speed * (1.0 + std::clamp(drift * 0.5, -kMaxRateCorrection, kMaxRateCorrection));
    const float goal = (running && !m_aligning && !m_fadingForSeek) ? t.gain : 0.0f;
    const float ramp = float(1.0 / (kFadeSeconds * m_rate));

    // 4. Mix (linear interpolation: the rate may differ slightly from 1)
    float pk = 0;
    for (int i = 0; i < frames; ++i) {
        m_gain = m_gain < goal ? std::min(goal, m_gain + ramp) : std::max(goal, m_gain - ramp);
        if (m_aligning) continue;
        const bool advance = running || m_gain > 0; // a pause fades out while still playing
        if (!advance) break;
        const size_t i0 = size_t(m_bufPos);
        if (i0 + 1 >= m_bufFrames) break; // underrun: silence
        const float f = float(m_bufPos - double(i0));
        const float *a = m_buf.data() + i0 * 2;
        const float l = (a[0] + (a[2] - a[0]) * f) * m_gain;
        const float r = (a[1] + (a[3] - a[1]) * f) * m_gain;
        out[i * 2] += l;
        out[i * 2 + 1] += r;
        pk = std::max(pk, std::max(std::abs(l), std::abs(r)));
        m_bufPos += step > 0 ? step : 1.0;
    }
    if (m_fadingForSeek && m_gain <= 0) {
        // Faded out: realign on the target. Close by, within the decoded data (no seek); far, by seeking.
        m_fadingForSeek = false;
        const double g = m_bufFrames > 0 ? gap(m_bufPts + m_bufPos / m_rate) : kSeekThreshold + 1;
        if (std::abs(g) < kSeekThreshold && !m_timelineChanged) {
            m_aligning = true;
            ++m_resyncs;
        } else {
            requestSeek(target);
        }
    }

    // 5. Drop consumed frames
    const size_t consumed = std::min(m_bufFrames, size_t(m_bufPos));
    if (consumed > 0) {
        std::memmove(m_buf.data(), m_buf.data() + consumed * 2, (m_bufFrames - consumed) * 2 * sizeof(float));
        m_bufFrames -= consumed;
        m_bufPos -= double(consumed);
        m_bufPts += double(consumed) / m_rate;
    }
    m_peak = std::max(pk, m_peak.load() * 0.8f);
}
