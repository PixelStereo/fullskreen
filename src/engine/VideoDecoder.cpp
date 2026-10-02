#include "VideoDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>
#include <cmath>

static constexpr size_t kMaxQueue = 6;

VideoDecoder::~VideoDecoder() { close(); }

static QString avErr(int code)
{
    char buf[256] = {0};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

bool VideoDecoder::open(const QString &path, QString *err)
{
    close();
    QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&m_fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = QStringLiteral("Unable to open video: ") + avErr(r);
        m_fmt = nullptr;
        return false;
    }
    avformat_find_stream_info(m_fmt, nullptr);
    const AVCodec *dec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_stream < 0 || !dec) {
        if (err) *err = QStringLiteral("No decodable video stream.");
        close();
        return false;
    }
    AVStream *st = m_fmt->streams[m_stream];
    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, st->codecpar);
    m_codec->thread_count = 0; // auto
    m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    r = avcodec_open2(m_codec, dec, nullptr);
    if (r < 0) {
        if (err) *err = QStringLiteral("Decoder unavailable: ") + avErr(r);
        close();
        return false;
    }

    m_codecName = QString::fromUtf8(dec->name);
    m_width = m_codec->width;
    m_height = m_codec->height;
    m_timeBase = av_q2d(st->time_base);
    m_startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time * m_timeBase : 0.0;
    AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    m_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    if (m_fmt->duration > 0) m_duration = double(m_fmt->duration) / AV_TIME_BASE;
    else if (st->duration > 0) m_duration = st->duration * m_timeBase;
    else m_duration = 0;

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    m_quit = false;
    m_eof = false;
    m_seekPending = false;
    m_needFirst = true;
    m_draining = false;
    m_loopBase = 0;
    m_lastPts = 0;
    m_discardBefore = -1e9;
    m_backward = false;
    m_backStack.clear();
    m_thread = std::thread(&VideoDecoder::run, this);
    return true;
}

bool VideoDecoder::probe(const QString &path, Info *info, QString *err)
{
    AVFormatContext *fmt = nullptr;
    QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = avErr(r);
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s < 0) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("No video stream");
        return false;
    }
    AVStream *st = fmt->streams[s];
    if (info) {
        info->width = st->codecpar->width;
        info->height = st->codecpar->height;
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        info->codec = d ? QString::fromUtf8(d->name) : QString();
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        info->fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        if (fmt->duration > 0) info->duration = double(fmt->duration) / AV_TIME_BASE;
        else if (st->duration > 0) info->duration = st->duration * av_q2d(st->time_base);
    }
    avformat_close_input(&fmt);
    return true;
}

void VideoDecoder::close()
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
    if (m_sws) sws_freeContext(m_sws);
    m_sws = nullptr;
    if (m_frame) av_frame_free(&m_frame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt) avformat_close_input(&m_fmt);
    m_stream = -1;
    m_width = m_height = 0;
}

void VideoDecoder::seek(double t)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_generation;
        m_seekTarget = std::max(0.0, t);
        m_seekPending = true;
        m_needFirst = true;
        m_eof = false;
        for (Frame &f : m_queue) m_pool.push_back(std::move(f.rgba));
        m_queue.clear();
    }
    m_cv.notify_all();
}

bool VideoDecoder::fetch(double t, std::vector<uint8_t> &out, int *w, int *h)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_queue.empty()) return false;
    int pick = -1;
    for (size_t i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].pts <= t + 1e-4) pick = int(i);
        else break;
    }
    if (pick < 0) {
        if (!m_needFirst) return false;
        pick = 0;
    }
    for (int i = 0; i < pick; ++i) {
        m_pool.push_back(std::move(m_queue.front().rgba));
        m_queue.pop_front();
    }
    std::swap(out, m_queue.front().rgba);
    if (w) *w = m_queue.front().w;
    if (h) *h = m_queue.front().h;
    m_pool.push_back(std::move(m_queue.front().rgba));
    m_queue.pop_front();
    m_needFirst = false;
    m_cv.notify_all();
    return true;
}

bool VideoDecoder::finished()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_eof && m_queue.empty();
}

void VideoDecoder::recycle(std::vector<uint8_t> &&buf)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_pool.push_back(std::move(buf));
}

void VideoDecoder::doSeek(double t)
{
    AVStream *st = m_fmt->streams[m_stream];
    int64_t ts = int64_t((t + m_startTime) / m_timeBase);
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(m_fmt, -1, int64_t(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    (void)st;
    avcodec_flush_buffers(m_codec);
    m_draining = false;
}

int VideoDecoder::decodeNext(Frame &f, double skipBefore)
{
    for (;;) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) {
            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
            double local = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_lastPts + 1.0 / m_fps;
            m_lastPts = local;
            f.pts = local + m_loopBase;
            if (local < skipBefore - 0.25 / m_fps) { // not needed: no conversion
                av_frame_unref(m_frame);
                return 2;
            }

            const int w = m_frame->width, h = m_frame->height;
            m_sws = sws_getCachedContext(m_sws, w, h, AVPixelFormat(m_frame->format), w, h, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (!m_pool.empty()) {
                    f.rgba = std::move(m_pool.back());
                    m_pool.pop_back();
                }
            }
            f.rgba.resize(size_t(w) * h * 4);
            f.w = w;
            f.h = h;
            // Flipped write: start from the last row with a negative stride.
            uint8_t *dst[4] = {f.rgba.data() + size_t(h - 1) * w * 4, nullptr, nullptr, nullptr};
            int dstStride[4] = {-w * 4, 0, 0, 0};
            if (m_sws) sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, h, dst, dstStride);
            av_frame_unref(m_frame);
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return -1;
        if (m_draining) return 0;

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

// Ping-pong: delivers the frames of [a, m_backEnd) in reverse order, a = one short window earlier.
// Monotonic time of a frame at local time L on the backward leg: legBase + span - L - frame duration.
void VideoDecoder::produceBackwardWindow()
{
    const double dt = 1.0 / m_fps;
    const double window = std::max(4.0, std::min(12.0, m_fps * 0.3)) * dt; // a few frames: bounded memory
    const double a = std::max(0.0, m_backEnd - window);
    doSeek(a);
    std::vector<Frame> got;
    for (;;) {
        Frame f;
        const int r = decodeNext(f, a);
        if (r == 2) continue;
        if (r != 1) break;
        const double local = f.pts - m_loopBase;
        if (local >= m_backEnd - 0.25 * dt) {
            recycle(std::move(f.rgba));
            break;
        }
        f.pts = m_legBase + span() - local - dt;
        got.push_back(std::move(f));
    }
    // The stack is delivered from its end: the latest local time first
    m_backStack = std::move(got);
    m_backEnd = a;
    if (a <= 1e-9) m_backEnd = -1; // leg finished once the stack is delivered
}

void VideoDecoder::run()
{
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        gen = m_generation;
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] { return m_quit || m_seekPending || (!m_eof && m_queue.size() < kMaxQueue); });
            if (m_quit) return;
            if (m_seekPending) {
                m_seekPending = false;
                const double t = m_seekTarget;
                gen = m_generation;
                lk.unlock();
                for (Frame &f : m_backStack) recycle(std::move(f.rgba));
                m_backStack.clear();
                m_backward = false;
                doSeek(t);
                m_loopBase = 0;
                m_discardBefore = t - 0.5 / m_fps;
                continue;
            }
        }

        if (m_backward) {
            if (!m_backStack.empty()) {
                Frame f = std::move(m_backStack.back());
                m_backStack.pop_back();
                std::lock_guard<std::mutex> lk(m_mutex);
                if (gen != m_generation) m_pool.push_back(std::move(f.rgba));
                else m_queue.push_back(std::move(f));
                continue;
            }
            if (m_backEnd > 0 && m_mode == PingPong) {
                produceBackwardWindow();
                continue;
            }
            // Back at the start: forward again
            m_backward = false;
            m_loopBase = m_legBase + span();
            doSeek(0);
            m_discardBefore = -1e9;
            continue;
        }

        Frame f;
        int r = decodeNext(f);
        if (r == 1) {
            if (f.pts - m_loopBase < m_discardBefore) {
                recycle(std::move(f.rgba));
                continue;
            }
            if (m_mode != Once && m_duration > 0 && f.pts - m_loopBase >= m_duration) { // beyond the layer's length
                recycle(std::move(f.rgba));
                r = 0;
            } else {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (gen != m_generation) {
                    m_pool.push_back(std::move(f.rgba));
                    continue;
                }
                m_queue.push_back(std::move(f));
                continue;
            }
        }
        // End of file (or error)
        if (r == 0 && m_mode == Loop) {
            m_loopBase += span();
            doSeek(0);
            m_discardBefore = -1e9;
        } else if (r == 0 && m_mode == PingPong) {
            m_backward = true;
            m_legBase = m_loopBase + span();
            m_backEnd = span();
        } else {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_eof = true;
        }
    }
}
