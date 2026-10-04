#include "VideoDecoder.h"
#include "Hap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

static constexpr size_t kMaxQueue = 6;
static constexpr size_t kStagingBudget = size_t(128) << 20; // upload buffers of one stream, at most
static constexpr size_t kBytePool = 8;

static std::atomic<bool> s_hardware{true}, s_s3tc{true}, s_bptc{false};

void VideoDecoder::setHardwareDecoding(bool on) { s_hardware = on; }
bool VideoDecoder::hardwareDecoding() { return s_hardware; }
void VideoDecoder::setGpuFormats(bool s3tc, bool bptc)
{
    s_s3tc = s3tc;
    s_bptc = bptc;
}

// ---------------------------------------------------------------------------
// VideoFrame
// ---------------------------------------------------------------------------
VideoFrame::~VideoFrame() { releaseAv(); }

VideoFrame::VideoFrame(VideoFrame &&o) noexcept
    : pts(o.pts), layout(std::move(o.layout)), staging(o.staging), bytes(std::move(o.bytes)), av(o.av)
{
    o.staging = -1;
    o.av = nullptr;
    o.bytes = {};
}

VideoFrame &VideoFrame::operator=(VideoFrame &&o) noexcept
{
    if (this != &o) {
        releaseAv();
        pts = o.pts;
        layout = std::move(o.layout);
        staging = o.staging;
        bytes = std::move(o.bytes);
        av = o.av;
        o.staging = -1;
        o.av = nullptr;
        o.bytes = {};
    }
    return *this;
}

void VideoFrame::releaseAv()
{
    if (av) av_frame_free(&av);
}

// ---------------------------------------------------------------------------
// Opening, closing
// ---------------------------------------------------------------------------
VideoDecoder::~VideoDecoder() { close(); }

static QString avErr(int code)
{
    char buf[256] = {0};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// get_format: the hardware's format when offered, the first software one otherwise (the hardware refused)
struct HwFormat {
    static AVPixelFormat get(AVCodecContext *ctx, const AVPixelFormat *fmts)
    {
        const auto *self = static_cast<const VideoDecoder *>(ctx->opaque);
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (int(*p) == self->m_hwPixFmt) return *p;
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return fmts[0];
    }
};

bool VideoDecoder::setupHardware(const AVCodec *dec)
{
    static const AVHWDeviceType kTypes[] = {
#if defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,
#else
        AV_HWDEVICE_TYPE_VAAPI,
#endif
    };
    // A device type that could not be opened once is not tried again (no hardware, no driver)
    static std::mutex failedMutex;
    static std::set<int> failed;
    for (AVHWDeviceType type : kTypes) {
        {
            std::lock_guard<std::mutex> lk(failedMutex);
            if (failed.count(int(type))) continue;
        }
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) || cfg->device_type != type) continue;
            AVBufferRef *dev = nullptr;
            if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) {
                std::lock_guard<std::mutex> lk(failedMutex);
                failed.insert(int(type));
                break;
            }
            m_hwDevice = dev;
            m_codec->hw_device_ctx = av_buffer_ref(dev);
            m_hwPixFmt = int(cfg->pix_fmt);
            m_hwName = QString::fromUtf8(av_hwdevice_get_type_name(type));
            m_codec->opaque = this;
            m_codec->get_format = &HwFormat::get;
            return true;
        }
    }
    return false;
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
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_stream < 0) {
        if (err) *err = QStringLiteral("No decodable video stream.");
        close();
        return false;
    }
    // Only the picture is read here (the sound has its own reader)
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (int(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream *st = m_fmt->streams[m_stream];
    const AVCodecParameters *par = st->codecpar;

    if (hap::isHapTag(par->codec_tag) || par->codec_id == AV_CODEC_ID_HAP) {
        // HAP: the packets are read here, the GPU samples their textures
        m_hapTag = hap::isHapTag(par->codec_tag) ? par->codec_tag : MKTAG('H', 'a', 'p', '1');
        m_codecName = hap::variantName(m_hapTag);
        m_width = par->width;
        m_height = par->height;
        if (m_width <= 0 || m_height <= 0) {
            if (err) *err = QStringLiteral("HAP video without a size.");
            close();
            return false;
        }
    } else {
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        if (!dec) {
            const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
            if (err) *err = QStringLiteral("No decoder for this video (%1).").arg(d ? QString::fromUtf8(d->name) : QStringLiteral("unknown codec"));
            close();
            return false;
        }
        m_codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(m_codec, par);
        m_codec->pkt_timebase = st->time_base;
        const bool hw = s_hardware && setupHardware(dec);
        m_codec->thread_count = hw ? 4 : 0; // 0: as many as the machine has
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
    }

    m_timeBase = av_q2d(st->time_base);
    m_startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time * m_timeBase : 0.0;
    AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    m_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    if (m_fmt->duration > 0) m_duration = double(m_fmt->duration) / AV_TIME_BASE;
    else if (st->duration > 0) m_duration = st->duration * m_timeBase;
    else m_duration = 0;

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    m_quit = false;
    m_eof = false;
    m_seekPending = false;
    m_needFirst = true;
    m_draining = false;
    m_lastPts = 0;
    m_discardBefore = -1e9;
    m_backStack.clear();
    m_timeline.duration = m_pendingTimeline.duration = m_duration;
    m_leg = m_timeline.firstLeg();
    m_legDone = false;
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
        info->codec = hap::isHapTag(st->codecpar->codec_tag) ? hap::variantName(st->codecpar->codec_tag)
                      : d ? QString::fromUtf8(d->name) : QString();
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
    // Upload buffers belong to the render thread: they are only forgotten here
    m_queue.clear();
    m_backStack.clear();
    m_bytePool.clear();
    m_freeStaging.clear();
    m_stagingById.clear();
    m_stagingSize = 0;
    m_layouts.clear();
    m_rgbaLayout.reset();
    m_hapScratch = {};
    if (m_sws) sws_freeContext(m_sws);
    m_sws = nullptr;
    if (m_frame) av_frame_free(&m_frame);
    if (m_swFrame) av_frame_free(&m_swFrame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_hwDevice) av_buffer_unref(&m_hwDevice);
    if (m_fmt) avformat_close_input(&m_fmt);
    m_hwPixFmt = -1;
    m_hwName.clear();
    m_hwActive = false;
    m_hapTag = 0;
    m_stream = -1;
    m_width = m_height = 0;
}

QString VideoDecoder::codecName() const
{
    return m_hwActive && !m_hwName.isEmpty() ? QStringLiteral("%1 (%2)").arg(m_codecName, m_hwName) : m_codecName;
}

void VideoDecoder::setTimeline(const Timeline &t)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_pendingTimeline = t;
    m_pendingTimeline.duration = m_duration;
}

void VideoDecoder::seek(double c)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_generation;
        m_seekTarget = std::max(0.0, c);
        m_seekPending = true;
        m_needFirst = true;
        m_eof = false;
        for (VideoFrame &f : m_queue) recycleLocked(std::move(f));
        m_queue.clear();
    }
    m_cv.notify_all();
}

// ---------------------------------------------------------------------------
// Frames: delivery, recycling, upload buffers
// ---------------------------------------------------------------------------
bool VideoDecoder::fetch(double t, VideoFrame &out)
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
        recycleLocked(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    recycleLocked(std::move(out));
    out = std::move(m_queue.front());
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

void VideoDecoder::recycle(VideoFrame &&f)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    recycleLocked(std::move(f));
}

void VideoDecoder::recycleLocked(VideoFrame &&f)
{
    if (f.staging >= 0) {
        const auto it = m_stagingById.find(f.staging);
        if (it != m_stagingById.end()) m_freeStaging.push_back(it->second);
        f.staging = -1;
    }
    if (f.bytes.capacity() && m_bytePool.size() < kBytePool) m_bytePool.push_back(std::move(f.bytes));
    f.bytes = {};
    f.releaseAv();
    f.layout.reset();
}

int VideoDecoder::stagingCount() const
{
    const size_t size = m_stagingSize.load();
    if (!size) return 0;
    return int(std::clamp<size_t>(kStagingBudget / size, 2, kMaxQueue + 2));
}

void VideoDecoder::addStaging(int id, uint8_t *ptr, size_t size)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const Staging s{id, ptr, size};
    m_stagingById[id] = s;
    m_freeStaging.push_back(s);
}

std::vector<int> VideoDecoder::takeFreeStaging()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<int> ids;
    for (const Staging &s : m_freeStaging) {
        ids.push_back(s.id);
        m_stagingById.erase(s.id);
    }
    m_freeStaging.clear();
    return ids;
}

bool VideoDecoder::takeStaging(size_t size, int *id, uint8_t **ptr)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_freeStaging.size(); ++i)
        if (m_freeStaging[i].size >= size) {
            *id = m_freeStaging[i].id;
            *ptr = m_freeStaging[i].ptr;
            m_freeStaging.erase(m_freeStaging.begin() + long(i));
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// Layouts: how the GPU reads a frame
// ---------------------------------------------------------------------------
static void yuvMatrix(AVColorSpace space, int height, float m[9], QString *name)
{
    double kr, kb;
    switch (space) {
    case AVCOL_SPC_BT709: kr = 0.2126, kb = 0.0722, *name = QStringLiteral("BT.709"); break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: kr = 0.299, kb = 0.114, *name = QStringLiteral("BT.601"); break;
    case AVCOL_SPC_FCC: kr = 0.30, kb = 0.11, *name = QStringLiteral("FCC"); break;
    case AVCOL_SPC_SMPTE240M: kr = 0.212, kb = 0.087, *name = QStringLiteral("SMPTE 240M"); break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: kr = 0.2627, kb = 0.0593, *name = QStringLiteral("BT.2020"); break;
    default: // unspecified: HD and above are BT.709, smaller pictures BT.601
        if (height >= 720) kr = 0.2126, kb = 0.0722, *name = QStringLiteral("BT.709");
        else kr = 0.299, kb = 0.114, *name = QStringLiteral("BT.601");
        break;
    }
    const double kg = 1.0 - kr - kb;
    const double r[9] = {1, 0, 2 * (1 - kr), 1, -2 * kb * (1 - kb) / kg, -2 * kr * (1 - kr) / kg, 1, 2 * (1 - kb), 0};
    for (int i = 0; i < 9; ++i) m[i] = float(r[i]);
}

std::shared_ptr<const VideoLayout> VideoDecoder::layoutFor(const AVFrame *fr)
{
    const LayoutKey key{fr->format, fr->width, fr->height, int(fr->colorspace), int(fr->color_range), int(fr->chroma_location)};
    const auto found = m_layouts.find(key);
    if (found != m_layouts.end()) return found->second;
    return m_layouts[key] = layoutOf(fr);
}

std::shared_ptr<const VideoLayout> VideoDecoder::layoutOf(const AVFrame *fr)
{
    std::shared_ptr<VideoLayout> L;

    const AVPixelFormat fmt = AVPixelFormat(fr->format);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    const uint64_t refused = AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL
                             | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT;
    if (!d || (d->flags & refused) || fmt == AV_PIX_FMT_XYZ12LE || fmt == AV_PIX_FMT_XYZ12BE || d->nb_components < 1
        || fr->width <= 0 || fr->height <= 0)
        return nullptr;
    const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
    const bool alpha = d->flags & AV_PIX_FMT_FLAG_ALPHA;
    const int nc = d->nb_components;
    const bool gray = !rgb && nc <= 2;

    struct PlaneInfo {
        bool used = false, chroma = false;
        int step = 0, bytes = 0;
    } pi[4];
    int planes = 0;
    for (int c = 0; c < nc; ++c) {
        const AVComponentDescriptor &cd = d->comp[c];
        const int bits = cd.depth + cd.shift;
        if (cd.plane < 0 || cd.plane > 3 || cd.depth <= 0 || bits > 16) return nullptr;
        const int bytes = bits > 8 ? 2 : 1;
        PlaneInfo &p = pi[cd.plane];
        if (p.used && (p.step != cd.step || p.bytes != bytes)) return nullptr; // packed 4:2:2 and the like
        if (cd.step <= 0 || cd.step % bytes || cd.offset % bytes || cd.step / bytes > 4) return nullptr;
        for (int o = 0; o < c; ++o) // two components in the same bits (RGB565…)
            if (d->comp[o].plane == cd.plane && d->comp[o].offset == cd.offset) return nullptr;
        p.used = true;
        p.step = cd.step;
        p.bytes = bytes;
        if (!rgb && !gray && (c == 1 || c == 2)) p.chroma = true;
        planes = std::max(planes, cd.plane + 1);
    }

    L = std::make_shared<VideoLayout>();
    L->width = fr->width;
    L->height = fr->height;
    L->planeCount = planes;
    L->alpha = alpha;
    L->mode = rgb ? VideoLayout::Mode::Rgb : gray ? VideoLayout::Mode::Gray : VideoLayout::Mode::Yuv;
    size_t offset = 0;
    for (int p = 0; p < planes; ++p) {
        if (!pi[p].used) return nullptr;
        VideoLayout::Plane &pl = L->plane[p];
        pl.width = pi[p].chroma ? AV_CEIL_RSHIFT(fr->width, d->log2_chroma_w) : fr->width;
        pl.height = pi[p].chroma ? AV_CEIL_RSHIFT(fr->height, d->log2_chroma_h) : fr->height;
        pl.channels = pi[p].step / pi[p].bytes;
        pl.texels = pi[p].bytes == 2 ? VideoLayout::Texels::U16 : VideoLayout::Texels::U8;
        pl.rowBytes = size_t(pl.width) * size_t(pi[p].step);
        pl.offset = offset;
        pl.size = pl.rowBytes * size_t(pl.height);
        offset += (pl.size + 63) & ~size_t(63);
    }
    L->totalBytes = offset;

    // Output channels: Y U V A / R G B A / grey and A
    auto use = [&](int out, int comp) {
        const AVComponentDescriptor &cd = d->comp[comp];
        const int bytes = pi[cd.plane].bytes;
        L->srcPlane[out] = cd.plane;
        L->srcChannel[out] = cd.offset / bytes;
        const double full = bytes == 2 ? 65535.0 : 255.0;
        L->scale[out] = float(full / double(((1 << cd.depth) - 1) << cd.shift));
    };
    if (gray) {
        use(0, 0);
        if (alpha && nc == 2) use(3, 1);
    } else {
        for (int c = 0; c < 3; ++c) use(c, c);
        if (alpha && nc >= 4) use(3, 3);
    }

    QString space;
    if (L->mode == VideoLayout::Mode::Yuv) {
        yuvMatrix(fr->colorspace, fr->height, L->matrix, &space);
        const int depth = d->comp[0].depth;
        if (depth < 8) return nullptr;
        const double max = double((1 << depth) - 1), k = double(1 << (depth - 8));
        const bool full = fr->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P
                          || fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P || fmt == AV_PIX_FMT_YUVJ411P;
        L->cOffset = float(128.0 * k / max);
        if (full) {
            L->yOffset = 0;
            L->yScale = 1;
            L->cScale = 1;
        } else {
            L->yOffset = float(16.0 * k / max);
            L->yScale = float(max / (219.0 * k));
            L->cScale = float(max / (224.0 * k));
        }
        // Chroma siting: MPEG-2 and later put it on the left (co-sited) unless the stream says otherwise
        const AVChromaLocation loc = fr->chroma_location;
        const double sx = d->log2_chroma_w ? (double(1 << d->log2_chroma_w) - 1) / double(2 << d->log2_chroma_w) : 0;
        const double sy = d->log2_chroma_h ? (double(1 << d->log2_chroma_h) - 1) / double(2 << d->log2_chroma_h) : 0;
        const bool left = loc == AVCHROMA_LOC_LEFT || loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_BOTTOMLEFT
                          || loc == AVCHROMA_LOC_UNSPECIFIED;
        L->chromaShift[0] = left ? float(sx) : 0.0f;
        L->chromaShift[1] = loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_TOP           ? float(sy)
                            : loc == AVCHROMA_LOC_BOTTOMLEFT || loc == AVCHROMA_LOC_BOTTOM ? -float(sy)
                                                                                           : 0.0f;
        if (full) space += QStringLiteral(" full range");
    }
    L->description = QString::fromUtf8(d->name) + (space.isEmpty() ? QString() : QStringLiteral(" · ") + space);
    return L;
}

std::shared_ptr<const VideoLayout> VideoDecoder::hapLayout(int count, int f0, int f1)
{
    const LayoutKey key{-1, m_width, m_height, count, f0, f1};
    const auto found = m_layouts.find(key);
    if (found != m_layouts.end()) return found->second;

    auto L = std::make_shared<VideoLayout>();
    L->width = m_width;
    L->height = m_height;
    L->planeCount = count;
    const int w4 = (m_width + 3) & ~3, h4 = (m_height + 3) & ~3;
    bool cpu = false;
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        const hap::Format f = hap::Format(i == 0 ? f0 : f1);
        VideoLayout::Plane &p = L->plane[i];
        p.width = w4;
        p.height = h4;
        bool gpu = true;
        switch (f) {
        case hap::Format::RgbDxt1: p.texels = VideoLayout::Texels::Dxt1, gpu = s_s3tc; break;
        case hap::Format::RgbaDxt5:
        case hap::Format::YCoCgDxt5: p.texels = VideoLayout::Texels::Dxt5, gpu = s_s3tc; break;
        case hap::Format::AlphaRgtc1: p.texels = VideoLayout::Texels::Rgtc1; break; // OpenGL 3.0
        case hap::Format::RgbaBc7: p.texels = VideoLayout::Texels::Bc7, gpu = s_bptc; break;
        case hap::Format::RgbBc6u: p.texels = VideoLayout::Texels::Bc6u, gpu = s_bptc; break;
        case hap::Format::RgbBc6s: p.texels = VideoLayout::Texels::Bc6s, gpu = s_bptc; break;
        default: m_layouts[key] = nullptr; return nullptr;
        }
        if (gpu) {
            p.channels = f == hap::Format::AlphaRgtc1 ? 1 : 4;
            p.size = hap::textureBytes(f, m_width, m_height);
        } else { // decoded on the CPU into plain texels
            cpu = true;
            p.texels = VideoLayout::Texels::U8;
            p.channels = hap::decodedChannels(f);
            p.rowBytes = size_t(w4) * size_t(p.channels);
            p.size = p.rowBytes * size_t(h4);
        }
        p.offset = offset;
        offset += (p.size + 63) & ~size_t(63);
    }
    L->totalBytes = offset;
    const hap::Format first = hap::Format(f0);
    if (first == hap::Format::YCoCgDxt5) L->mode = count > 1 ? VideoLayout::Mode::HapYCoCgAlpha : VideoLayout::Mode::HapYCoCg;
    else if (first == hap::Format::AlphaRgtc1) L->mode = VideoLayout::Mode::HapAlphaOnly;
    else L->mode = VideoLayout::Mode::HapRgb;
    L->alpha = L->mode == VideoLayout::Mode::HapYCoCgAlpha || first == hap::Format::RgbaDxt5 || first == hap::Format::RgbaBc7;
    QStringList fmts;
    for (int i = 0; i < count; ++i) fmts << hap::formatName(hap::Format(i == 0 ? f0 : f1));
    L->description = hap::variantName(m_hapTag) + QStringLiteral(" · ") + fmts.join(QStringLiteral(" + "))
                     + (cpu ? QStringLiteral(" · decoded on the CPU") : QString());
    m_layouts[key] = L;
    return L;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
void VideoDecoder::doSeek(double t)
{
    int64_t ts = int64_t((t + m_startTime) / m_timeBase);
    if (av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(m_fmt, -1, int64_t(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    if (m_codec) avcodec_flush_buffers(m_codec);
    m_draining = false;
}

// Packed planes, at the layout's offsets
static void packPlanes(const VideoLayout &L, const AVFrame *src, uint8_t *dst)
{
    for (int p = 0; p < L.planeCount; ++p) {
        const VideoLayout::Plane &pl = L.plane[p];
        uint8_t *o = dst + pl.offset;
        if (src->linesize[p] == int(pl.rowBytes)) {
            std::memcpy(o, src->data[p], pl.size);
            continue;
        }
        for (int y = 0; y < pl.height; ++y)
            std::memcpy(o + size_t(y) * pl.rowBytes, src->data[p] + ptrdiff_t(y) * src->linesize[p], pl.rowBytes);
    }
}

// mapped: src is the hardware's memory seen in place (m_frame being the hardware frame)
bool VideoDecoder::deliver(AVFrame *src, VideoFrame &f, bool mapped)
{
    if (auto L = layoutFor(src)) {
        m_stagingSize = L->totalBytes;
        f.layout = L;
        int id;
        uint8_t *ptr;
        if (takeStaging(L->totalBytes, &id, &ptr)) {
            packPlanes(*L, src, ptr);
            f.staging = id;
            return true;
        }
        // No free upload buffer: the frame is kept as it is (copied out of the hardware's memory)
        AVFrame *keep = av_frame_alloc();
        if (mapped) {
            if (av_hwframe_transfer_data(keep, m_frame, 0) < 0) {
                av_frame_free(&keep);
                f.layout.reset();
                return false;
            }
            av_frame_copy_props(keep, src);
        } else {
            av_frame_move_ref(keep, src);
        }
        f.av = keep;
        return true;
    }

    // A format the GPU path does not take: RGBA on the CPU
    const int w = src->width, h = src->height;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(src->format));
    const bool alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
    if (!m_rgbaLayout || m_rgbaLayout->width != w || m_rgbaLayout->height != h || m_rgbaLayout->alpha != alpha) {
        auto L = std::make_shared<VideoLayout>();
        L->mode = VideoLayout::Mode::Rgb;
        L->width = w;
        L->height = h;
        L->planeCount = 1;
        L->plane[0].width = w;
        L->plane[0].height = h;
        L->plane[0].channels = 4;
        L->plane[0].rowBytes = size_t(w) * 4;
        L->plane[0].size = L->plane[0].rowBytes * size_t(h);
        L->totalBytes = L->plane[0].size;
        for (int c = 0; c < 4; ++c) {
            L->srcPlane[c] = 0;
            L->srcChannel[c] = c;
        }
        L->alpha = alpha;
        L->description = QString::fromUtf8(d ? d->name : "?") + QStringLiteral(" · converted on the CPU");
        m_rgbaLayout = L;
    }
    m_stagingSize = m_rgbaLayout->totalBytes;
    m_sws = sws_getCachedContext(m_sws, w, h, AVPixelFormat(src->format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
    if (!m_sws) return false;
    {
        // Same matrix as the GPU path would take
        float unused[9];
        QString name;
        yuvMatrix(src->colorspace, h, unused, &name);
        const int cs = name == QLatin1String("BT.709")      ? SWS_CS_ITU709
                       : name == QLatin1String("BT.2020")   ? SWS_CS_BT2020
                       : name == QLatin1String("FCC")       ? SWS_CS_FCC
                       : name == QLatin1String("SMPTE 240M") ? SWS_CS_SMPTE240M
                                                            : SWS_CS_ITU601;
        int *inv, *tbl, srcRange, dstRange, bri, con, sat;
        if (sws_getColorspaceDetails(m_sws, &inv, &srcRange, &tbl, &dstRange, &bri, &con, &sat) >= 0)
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), src->color_range == AVCOL_RANGE_JPEG ? 1 : srcRange,
                                     tbl, dstRange, bri, con, sat);
    }
    f.layout = m_rgbaLayout;
    int id;
    uint8_t *ptr = nullptr;
    if (takeStaging(m_rgbaLayout->totalBytes, &id, &ptr)) {
        f.staging = id;
    } else {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_bytePool.empty()) {
                f.bytes = std::move(m_bytePool.back());
                m_bytePool.pop_back();
            }
        }
        f.bytes.resize(m_rgbaLayout->totalBytes);
        ptr = f.bytes.data();
    }
    uint8_t *dst[4] = {ptr, nullptr, nullptr, nullptr};
    int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
    return true;
}

int VideoDecoder::decodeHap(VideoFrame &f, double skipBefore)
{
    for (;;) {
        const int r = av_read_frame(m_fmt, m_packet);
        if (r < 0) return 0;
        if (m_packet->stream_index != m_stream) {
            av_packet_unref(m_packet);
            continue;
        }
        int64_t ts = m_packet->pts != AV_NOPTS_VALUE ? m_packet->pts : m_packet->dts;
        const double local = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_lastPts + 1.0 / m_fps;
        m_lastPts = local;
        f.pts = local;
        if (local < skipBefore - 0.25 / m_fps) { // not needed: not even decompressed
            av_packet_unref(m_packet);
            return 2;
        }
        hap::Frame hf;
        QString err;
        bool ok = hap::parse(m_packet->data, size_t(m_packet->size), &hf, &err);
        std::shared_ptr<const VideoLayout> L;
        if (ok) L = hapLayout(hf.count, int(hf.tex[0].format), hf.count > 1 ? int(hf.tex[1].format) : 0);
        if (ok && L)
            for (int i = 0; i < hf.count; ++i)
                if (hf.tex[i].outSize != hap::textureBytes(hf.tex[i].format, m_width, m_height)) ok = false;
        if (!ok || !L) {
            av_packet_unref(m_packet);
            return 2; // a damaged frame: skipped
        }
        m_stagingSize = L->totalBytes;
        f.layout = L;
        int id;
        uint8_t *dst = nullptr;
        if (takeStaging(L->totalBytes, &id, &dst)) {
            f.staging = id;
        } else {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (!m_bytePool.empty()) {
                    f.bytes = std::move(m_bytePool.back());
                    m_bytePool.pop_back();
                }
            }
            f.bytes.resize(L->totalBytes);
            dst = f.bytes.data();
        }
        for (int i = 0; i < hf.count && ok; ++i) {
            const VideoLayout::Plane &p = L->plane[i];
            if (p.texels != VideoLayout::Texels::U8) {
                ok = hap::decompress(hf.tex[i], dst + p.offset, p.size);
            } else { // the GPU does not sample this format: its blocks decoded here
                m_hapScratch.resize(hf.tex[i].outSize);
                ok = hap::decompress(hf.tex[i], m_hapScratch.data(), m_hapScratch.size());
                if (ok) hap::decodeBlocks(hf.tex[i].format, m_hapScratch.data(), m_width, m_height, dst + p.offset, p.rowBytes);
            }
        }
        av_packet_unref(m_packet);
        if (!ok) {
            recycle(std::move(f));
            return 2;
        }
        return 1;
    }
}

int VideoDecoder::decodeNext(VideoFrame &f, double skipBefore)
{
    if (m_hapTag) return decodeHap(f, skipBefore);
    for (;;) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) {
            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
            double local = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_lastPts + 1.0 / m_fps;
            m_lastPts = local;
            f.pts = local; // position; stamped with the clock by the caller
            if (local < skipBefore - 0.25 / m_fps) { // not needed: not copied
                av_frame_unref(m_frame);
                return 2;
            }
            AVFrame *src = m_frame;
            bool mapped = false;
            if (m_hwPixFmt >= 0 && m_frame->format == m_hwPixFmt) {
                // The hardware's memory: seen in place, or copied back when it cannot be
                av_frame_unref(m_swFrame);
                int mr = av_hwframe_map(m_swFrame, m_frame, AV_HWFRAME_MAP_READ);
                mapped = mr >= 0;
                if (mr < 0) {
                    av_frame_unref(m_swFrame);
                    mr = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
                }
                if (mr < 0) {
                    av_frame_unref(m_frame);
                    return 2;
                }
                av_frame_copy_props(m_swFrame, m_frame);
                m_hwActive = true;
                src = m_swFrame;
            }
            const bool ok = deliver(src, f, mapped);
            av_frame_unref(m_swFrame);
            av_frame_unref(m_frame);
            if (!ok) {
                recycle(std::move(f));
                return 2;
            }
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

// Starts producing a leg at a position within it.
void VideoDecoder::startLeg(const Timeline::Leg &leg, double position)
{
    for (VideoFrame &f : m_backStack) recycle(std::move(f));
    m_backStack.clear();
    m_leg = leg;
    m_legDone = false;
    if (leg.forward) {
        doSeek(position);
        m_discardBefore = position - 0.5 / m_fps;
    } else {
        m_backEnd = position;
    }
}

// End of the current leg: the next one (Loop, PingPong), or the end of playback (Once).
void VideoDecoder::finishLeg()
{
    if (m_timeline.mode == Timeline::Once || m_duration <= 0) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_eof = true;
        return;
    }
    const Timeline::Leg next = m_timeline.nextLeg(m_leg);
    startLeg(next, next.from); // frames before the in point (from the keyframe) are discarded
}

// Backward leg: the frames of [a, m_backEnd) in reverse order, a = one short window earlier.
// Clock of a frame at position L: clockStart + from - L - frame duration.
void VideoDecoder::produceBackwardWindow()
{
    const double dt = 1.0 / m_fps;
    const double window = std::max(4.0, std::min(12.0, m_fps * 0.3)) * dt; // a few frames: bounded memory
    const double a = std::max(m_leg.to, m_backEnd - window);
    doSeek(a);
    std::vector<VideoFrame> got;
    for (;;) {
        VideoFrame f;
        const int r = decodeNext(f, a);
        if (r == 2) continue;
        if (r != 1) break;
        const double local = f.pts;
        if (local >= m_backEnd - 0.25 * dt) {
            recycle(std::move(f));
            break;
        }
        f.pts = m_leg.clockStart + m_leg.from - local - dt;
        got.push_back(std::move(f));
    }
    m_backStack = std::move(got); // delivered from its end: the latest position first
    m_backEnd = a;
    if (a <= m_leg.to + 1e-9) m_legDone = true; // finished once the stack is delivered
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
                const double c = m_seekTarget;
                m_timeline = m_pendingTimeline;
                gen = m_generation;
                lk.unlock();
                startLeg(m_timeline.legAt(c), m_timeline.position(c));
                continue;
            }
        }

        if (!m_leg.forward) {
            if (!m_backStack.empty()) {
                VideoFrame f = std::move(m_backStack.back());
                m_backStack.pop_back();
                std::lock_guard<std::mutex> lk(m_mutex);
                if (gen != m_generation) recycleLocked(std::move(f));
                else m_queue.push_back(std::move(f));
                continue;
            }
            if (!m_legDone) produceBackwardWindow();
            else finishLeg();
            continue;
        }

        VideoFrame f;
        int r = decodeNext(f);
        if (r == 1) {
            const double local = f.pts;
            if (local < m_discardBefore) {
                recycle(std::move(f));
                continue;
            }
            if (m_duration > 0 && local >= m_leg.to) { // end of the leg (out point, or the end of the media)
                recycle(std::move(f));
                finishLeg();
                continue;
            }
            f.pts = m_leg.clockStart + local - m_leg.from;
            std::lock_guard<std::mutex> lk(m_mutex);
            if (gen != m_generation) recycleLocked(std::move(f));
            else m_queue.push_back(std::move(f));
            continue;
        }
        if (r == 2) continue;
        finishLeg(); // end of file (or error)
    }
}

// ---------------------------------------------------------------------------
// CPU conversion (tests, tools)
// ---------------------------------------------------------------------------
bool VideoDecoder::fetchRgba(double t, std::vector<uint8_t> &out, int *w, int *h)
{
    VideoFrame f;
    if (!fetch(t, f)) return false;
    const bool ok = toRgba(f, out, true);
    if (ok) {
        if (w) *w = f.layout->width;
        if (h) *h = f.layout->height;
    }
    recycle(std::move(f));
    return ok;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

bool VideoDecoder::toRgba(const VideoFrame &f, std::vector<uint8_t> &out, bool bottomUp)
{
    if (!f.layout || f.staging >= 0) return false;
    const VideoLayout &L = *f.layout;
    const int W = L.width, H = L.height;
    out.assign(size_t(W) * H * 4, 0);
    const uint8_t *base[4] = {};
    size_t stride[4] = {};
    for (int p = 0; p < L.planeCount; ++p) {
        if (f.av) {
            base[p] = f.av->data[p];
            stride[p] = size_t(f.av->linesize[p]);
        } else {
            base[p] = f.bytes.data() + L.plane[p].offset;
            stride[p] = L.plane[p].rowBytes;
        }
    }
    // Compressed textures: their blocks decoded on the CPU first
    std::vector<uint8_t> decoded[4];
    int channels[4] = {};
    bool wide[4] = {};
    for (int p = 0; p < L.planeCount; ++p) {
        const VideoLayout::Plane &pl = L.plane[p];
        channels[p] = pl.channels;
        wide[p] = pl.texels == VideoLayout::Texels::U16;
        if (pl.texels == VideoLayout::Texels::U8 || pl.texels == VideoLayout::Texels::U16) continue;
        const hap::Format hf = pl.texels == VideoLayout::Texels::Dxt1    ? hap::Format::RgbDxt1
                               : pl.texels == VideoLayout::Texels::Dxt5  ? hap::Format::RgbaDxt5
                               : pl.texels == VideoLayout::Texels::Rgtc1 ? hap::Format::AlphaRgtc1
                               : pl.texels == VideoLayout::Texels::Bc7   ? hap::Format::RgbaBc7
                               : pl.texels == VideoLayout::Texels::Bc6u  ? hap::Format::RgbBc6u
                                                                         : hap::Format::RgbBc6s;
        channels[p] = hap::decodedChannels(hf);
        decoded[p].resize(size_t(pl.width) * pl.height * channels[p]);
        hap::decodeBlocks(hf, base[p], W, H, decoded[p].data(), size_t(pl.width) * channels[p]);
        base[p] = decoded[p].data();
        stride[p] = size_t(pl.width) * channels[p];
    }
    // A texel of a plane at picture pixel (x, y): nearest (subsampled planes stretched)
    auto texel = [&](int p, int x, int y, int ch) -> float {
        const VideoLayout::Plane &pl = L.plane[p];
        const bool hapPlane = L.mode >= VideoLayout::Mode::HapRgb; // padded, not stretched
        const int px = hapPlane ? x : std::min(pl.width - 1, int(int64_t(x) * pl.width / W));
        const int py = hapPlane ? y : std::min(pl.height - 1, int(int64_t(y) * pl.height / H));
        if (wide[p]) {
            uint16_t v;
            std::memcpy(&v, base[p] + size_t(py) * stride[p] + (size_t(px) * channels[p] + ch) * 2, 2);
            return v / 65535.0f;
        }
        return base[p][size_t(py) * stride[p] + size_t(px) * channels[p] + ch] / 255.0f;
    };
    for (int y = 0; y < H; ++y) {
        uint8_t *row = out.data() + size_t(bottomUp ? H - 1 - y : y) * W * 4;
        for (int x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0, a = 1;
            auto chan = [&](int c) { return L.srcPlane[c] < 0 ? 0.0f : texel(L.srcPlane[c], x, y, L.srcChannel[c]) * L.scale[c]; };
            switch (L.mode) {
            case VideoLayout::Mode::Yuv: {
                const float Y = (chan(0) - L.yOffset) * L.yScale, U = (chan(1) - L.cOffset) * L.cScale,
                            V = (chan(2) - L.cOffset) * L.cScale;
                r = L.matrix[0] * Y + L.matrix[1] * U + L.matrix[2] * V;
                g = L.matrix[3] * Y + L.matrix[4] * U + L.matrix[5] * V;
                b = L.matrix[6] * Y + L.matrix[7] * U + L.matrix[8] * V;
                if (L.alpha) a = chan(3);
                break;
            }
            case VideoLayout::Mode::Rgb:
                r = chan(0), g = chan(1), b = chan(2);
                if (L.alpha) a = chan(3);
                break;
            case VideoLayout::Mode::Gray:
                r = g = b = chan(0);
                if (L.alpha) a = chan(3);
                break;
            case VideoLayout::Mode::HapRgb:
                r = texel(0, x, y, 0), g = texel(0, x, y, 1), b = texel(0, x, y, 2);
                if (L.alpha) a = texel(0, x, y, 3);
                break;
            case VideoLayout::Mode::HapYCoCg:
            case VideoLayout::Mode::HapYCoCgAlpha: {
                const float k = 128.0f / 255.0f;
                const float s = texel(0, x, y, 2) * (255.0f / 8.0f) + 1.0f;
                const float co = (texel(0, x, y, 0) - k) / s, cg = (texel(0, x, y, 1) - k) / s, Y = texel(0, x, y, 3);
                r = Y + co - cg, g = Y + cg, b = Y - co - cg;
                if (L.mode == VideoLayout::Mode::HapYCoCgAlpha) a = texel(1, x, y, 0);
                break;
            }
            case VideoLayout::Mode::HapAlphaOnly: r = g = b = texel(0, x, y, 0); break;
            }
            uint8_t *o = row + size_t(x) * 4;
            o[0] = uint8_t(clamp01(r) * 255.0f + 0.5f);
            o[1] = uint8_t(clamp01(g) * 255.0f + 0.5f);
            o[2] = uint8_t(clamp01(b) * 255.0f + 0.5f);
            o[3] = uint8_t(clamp01(a) * 255.0f + 0.5f);
        }
    }
    return true;
}
