// Video pipeline tests: every pixel layout the GPU converts and every HAP variant, compared with FFmpeg's own
// decoding (swscale) or with the CPU's decoding of the same blocks; Snappy; hidden layers; upload buffers.
// Called from engine_test.cpp (manual rendering mode, engine initialized).
#include "Engine.h"
#include "Gl.h"
#include "Hap.h"
#include "Snappy.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

static int g_failures = 0;
#define VCHECK(cond)                                                                                    \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                \
            ++g_failures;                                                                               \
        } else                                                                                          \
            std::printf("ok    %s\n", #cond);                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

// The first frame as FFmpeg decodes it, converted by swscale with the matrix and range the GPU path takes,
// chroma interpolated. Top row first, RGBA.
static QImage ffmpegReference(const QString &path)
{
    AVFormatContext *fmt = nullptr;
    QImage out;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return out;
    avformat_find_stream_info(fmt, nullptr);
    const AVCodec *dec = nullptr;
    const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (s < 0 || !dec) {
        avformat_close_input(&fmt);
        return out;
    }
    AVCodecContext *c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fmt->streams[s]->codecpar);
    avcodec_open2(c, dec, nullptr);
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == s && avcodec_send_packet(c, pkt) >= 0) got = avcodec_receive_frame(c, fr) == 0;
        av_packet_unref(pkt);
    }
    if (!got) {
        avcodec_send_packet(c, nullptr);
        got = avcodec_receive_frame(c, fr) == 0;
    }
    if (got) {
        const int w = fr->width, h = fr->height;
        SwsContext *sws = sws_getContext(w, h, AVPixelFormat(fr->format), w, h, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP, nullptr,
                                         nullptr, nullptr);
        int cs = SWS_CS_ITU601;
        if (fr->colorspace == AVCOL_SPC_BT709 || (fr->colorspace == AVCOL_SPC_UNSPECIFIED && h >= 720)) cs = SWS_CS_ITU709;
        if (fr->colorspace == AVCOL_SPC_BT2020_NCL) cs = SWS_CS_BT2020;
        const AVPixelFormat pf = AVPixelFormat(fr->format);
        const bool full = fr->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P || pf == AV_PIX_FMT_YUVJ422P
                          || pf == AV_PIX_FMT_YUVJ444P;
        int *inv, *tbl, srcRange, dstRange, bri, con, sat;
        sws_getColorspaceDetails(sws, &inv, &srcRange, &tbl, &dstRange, &bri, &con, &sat);
        sws_setColorspaceDetails(sws, sws_getCoefficients(cs), full ? 1 : srcRange, tbl, 1, bri, con, sat);
        out = QImage(w, h, QImage::Format_RGBA8888);
        uint8_t *dst[4] = {out.bits(), nullptr, nullptr, nullptr};
        int stride[4] = {int(out.bytesPerLine()), 0, 0, 0};
        sws_scale(sws, fr->data, fr->linesize, 0, h, dst, stride);
        sws_freeContext(sws);
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    return out;
}

// The first frame as the decoder's CPU conversion makes it (the same blocks decoded by bcdec). Top row first.
static QImage cpuReference(const QString &path)
{
    VideoDecoder d;
    QString err;
    if (!d.open(path, &err)) return {};
    Timeline t;
    t.mode = Timeline::Loop;
    d.setTimeline(t);
    d.seek(0);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    QElapsedTimer tm;
    tm.start();
    while (!d.fetchRgba(0.001, rgba, &w, &h) && tm.elapsed() < 3000) QThread::msleep(1);
    if (!w) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), rgba.data() + size_t(h - 1 - y) * w * 4, size_t(w) * 4);
    return img;
}

struct Diff {
    int max = 0;
    double mean = 0;
    bool sameSize = false;
};

static Diff compare(const QImage &a, const QImage &b, bool alpha)
{
    Diff d;
    if (a.size() != b.size() || a.isNull()) return d;
    d.sameSize = true;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0;
    size_t n = 0;
    for (int r = 0; r < x.height(); ++r) {
        const uint8_t *p = x.constScanLine(r), *q = y.constScanLine(r);
        for (int c = 0; c < x.width() * 4; ++c) {
            if (!alpha && c % 4 == 3) continue;
            const int v = std::abs(int(p[c]) - int(q[c]));
            d.max = std::max(d.max, v);
            sum += v;
            ++n;
        }
    }
    d.mean = n ? sum / double(n) : 0;
    return d;
}

// ---------------------------------------------------------------------------
// Writing HAP files (the variants FFmpeg does not encode)
// ---------------------------------------------------------------------------
static QByteArray section(uint8_t type, const QByteArray &data, bool longHeader = false)
{
    QByteArray h;
    const uint32_t n = uint32_t(data.size());
    if (longHeader || n > 0xFFFFFF) {
        h.append(char(0)).append(char(0)).append(char(0)).append(char(type));
        for (int i = 0; i < 4; ++i) h.append(char((n >> (8 * i)) & 0xFF));
    } else {
        for (int i = 0; i < 3; ++i) h.append(char((n >> (8 * i)) & 0xFF));
        h.append(char(type));
    }
    return h + data;
}

static void putLiteral(QByteArray &o, const char *p, int len)
{
    while (len > 0) {
        const int n = std::min(len, 65536);
        if (n <= 60) {
            o.append(char((n - 1) << 2));
        } else if (n <= 256) {
            o.append(char(60 << 2)).append(char(n - 1));
        } else {
            o.append(char(61 << 2)).append(char((n - 1) & 0xFF)).append(char((n - 1) >> 8));
        }
        o.append(p, n);
        p += n;
        len -= n;
    }
}

// A small Snappy writer: a unit equal to the previous one becomes a copy (overlapping runs), the rest literals
static QByteArray snappyCompress(const QByteArray &in, int unit)
{
    QByteArray o;
    uint32_t n = uint32_t(in.size());
    do {
        uint8_t b = n & 0x7F;
        n >>= 7;
        o.append(char(n ? (b | 0x80) : b));
    } while (n);
    int i = 0, litStart = 0;
    const int size = int(in.size());
    while (i < size) {
        int run = 0;
        if (i >= unit)
            while (i + run < size && in[i + run] == in[i + run - unit]) ++run;
        if (run >= 8) {
            putLiteral(o, in.constData() + litStart, i - litStart);
            while (run > 0) {
                const int len = std::min(run, 64);
                o.append(char(2 | ((len - 1) << 2))).append(char(unit & 0xFF)).append(char(unit >> 8)); // 2-byte offset
                i += len;
                run -= len;
            }
            litStart = i;
        } else {
            ++i;
        }
    }
    putLiteral(o, in.constData() + litStart, size - litStart);
    return o;
}

static bool writeHapMov(const QString &path, uint32_t tag, int w, int h, const QList<QByteArray> &frames)
{
    AVFormatContext *oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mov", path.toUtf8().constData()) < 0) return false;
    oc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL; // Hap7 and HapH are not in FFmpeg's table
    AVStream *st = avformat_new_stream(oc, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_HAP;
    st->codecpar->codec_tag = tag;
    st->codecpar->width = w;
    st->codecpar->height = h;
    st->time_base = AVRational{1, 25};
    bool ok = avio_open(&oc->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(oc, nullptr) >= 0;
    for (int i = 0; ok && i < frames.size(); ++i) {
        AVPacket *p = av_packet_alloc();
        av_new_packet(p, int(frames[i].size()));
        std::memcpy(p->data, frames[i].constData(), size_t(frames[i].size()));
        p->pts = p->dts = av_rescale_q(i, AVRational{1, 25}, st->time_base);
        p->duration = av_rescale_q(1, AVRational{1, 25}, st->time_base);
        p->flags |= AV_PKT_FLAG_KEY;
        ok = av_interleaved_write_frame(oc, p) >= 0;
        av_packet_free(&p);
    }
    if (ok) av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    return ok;
}

struct BitWriter {
    uint8_t b[16] = {};
    int pos = 0;
    void put(uint32_t v, int n)
    {
        for (int i = 0; i < n; ++i, ++pos)
            if ((v >> i) & 1) b[pos >> 3] |= uint8_t(1 << (pos & 7));
    }
    QByteArray bytes() const { return QByteArray(reinterpret_cast<const char *>(b), 16); }
};

// Blocks of a texture whose quadrants (in blocks) hold the four given blocks
static QByteArray quadrantTexture(int w, int h, const QByteArray q[4])
{
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    QByteArray t;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) t += q[(by < bh / 2 ? 0 : 2) + (bx < bw / 2 ? 0 : 1)];
    return t;
}

static QByteArray rgtc1Block(uint8_t v) { return QByteArray(1, char(v)) + QByteArray(1, char(v)) + QByteArray(6, 0); }

static QByteArray bc7Mode6Block(uint8_t r, uint8_t g, uint8_t b, uint8_t a) // even values: p-bit 0
{
    BitWriter w;
    w.put(1 << 6, 7);
    for (uint8_t c : {r, r, g, g, b, b, a, a}) w.put(c >> 1, 7);
    w.put(0, 1);
    w.put(0, 1);
    w.put(0, 63);
    return w.bytes();
}

static QByteArray bc6Mode11Block(int r, int g, int b) // 10-bit endpoints, one color
{
    BitWriter w;
    w.put(0x03, 5);
    for (int c : {r, g, b}) w.put(uint32_t(c), 10);
    for (int c : {r, g, b}) w.put(uint32_t(c), 10);
    w.put(0, 63);
    return w.bytes();
}

static uint16_t rgb565(int r, int g, int b) { return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static QByteArray ycocgBlock(int co, int cg, int y) // DXT5: Y in the alpha, Co Cg in red green, scale 1
{
    QByteArray b;
    b.append(char(y)).append(char(y)).append(QByteArray(6, 0));
    const uint16_t c = rgb565(co, cg, 0);
    b.append(char(c & 0xFF)).append(char(c >> 8)).append(char(c & 0xFF)).append(char(c >> 8)).append(QByteArray(4, 0));
    return b;
}

// Chunked second stage: chunks of the texture, alternately raw and Snappy, stored in reverse order
// (the offset table finds them)
static QByteArray chunked(uint8_t format, const QByteArray &tex, int chunks, int unit)
{
    QList<QByteArray> parts;
    QByteArray comp, sizes, offsets;
    const int per = ((int(tex.size()) / chunks) / unit) * unit;
    for (int i = 0; i < chunks; ++i) {
        const QByteArray raw = tex.mid(i * per, i + 1 == chunks ? -1 : per);
        const bool sn = i % 2 == 1;
        parts << (sn ? snappyCompress(raw, unit) : raw);
        comp.append(char(sn ? 0x0B : 0x0A));
    }
    QByteArray data;
    QList<int> at(chunks);
    for (int i = chunks - 1; i >= 0; --i) {
        at[i] = int(data.size());
        data += parts[i];
    }
    for (int i = 0; i < chunks; ++i)
        for (int k = 0; k < 4; ++k) {
            sizes.append(char((uint32_t(parts[i].size()) >> (8 * k)) & 0xFF));
            offsets.append(char((uint32_t(at[i]) >> (8 * k)) & 0xFF));
        }
    const QByteArray ins = section(0x01, section(0x02, comp) + section(0x03, sizes) + section(0x04, offsets));
    return section(uint8_t(0xC0 | format), ins + data);
}

static constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------
int runVideoTests(Engine &e, const QString &root, const QString &tmp)
{
    g_failures = 0;
    // --- Snappy: literals, the three kinds of copy (overlapping too), malformed streams
    {
        const uint8_t s1[] = {7, 0x08, 'x', 'a', 'b', 0x01, 0x02}; // "xab" + copy(offset 2, length 4)
        uint8_t out[16];
        VCHECK(snappy::decompress(s1, sizeof s1, out, 7) && std::memcmp(out, "xababab", 7) == 0);
        const uint8_t s2[] = {10, 0x0C, 'a', 'b', 'c', 'd', 0x16, 0x04, 0x00}; // "abcd" + copy2(offset 4, length 6)
        VCHECK(snappy::decompress(s2, sizeof s2, out, 10) && std::memcmp(out, "abcdabcdab", 10) == 0);
        const uint8_t s3[] = {6, 0x04, 'h', 'i', 0x0F, 0x02, 0x00, 0x00, 0x00}; // "hi" + copy4(offset 2, length 4)
        VCHECK(snappy::decompress(s3, sizeof s3, out, 6) && std::memcmp(out, "hihihi", 6) == 0);
        const uint8_t bad1[] = {4, 0x01, 0x05}; // a copy before any data
        VCHECK(!snappy::decompress(bad1, sizeof bad1, out, 4));
        const uint8_t bad2[] = {9, 0x08, 'x', 'y'}; // truncated literal
        VCHECK(!snappy::decompress(bad2, sizeof bad2, out, 9));
        const uint8_t bad3[] = {3, 0x08, 'x', 'y', 'z'}; // announced length differs from the buffer
        VCHECK(!snappy::decompress(bad3, sizeof bad3, out, 4));
        QByteArray big;
        for (int i = 0; i < 70000; ++i) big.append(char((i / 37) % 251));
        const QByteArray z = snappyCompress(big, 16);
        QByteArray back(big.size(), 0);
        size_t len = 0;
        VCHECK(snappy::uncompressedLength(reinterpret_cast<const uint8_t *>(z.constData()), size_t(z.size()), &len)
               && len == size_t(big.size()));
        VCHECK(snappy::decompress(reinterpret_cast<const uint8_t *>(z.constData()), size_t(z.size()),
                                  reinterpret_cast<uint8_t *>(back.data()), size_t(back.size())));
        VCHECK(back == big);
        std::printf("      Snappy: %lld bytes -> %lld\n", (long long)big.size(), (long long)z.size());
    }

    // Loads a file into a fresh layer, lets frames reach the GPU until some came through an upload buffer
    // (the first ones, decoded before the buffers were handed over, come from the decoder's memory), pauses,
    // and returns the layer's source picture (top row first)
    auto playAndGrab = [&](const QString &path, QString *description, quint64 *staged) -> QImage {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        const int i = e.addLayer(QStringLiteral("v"));
        QString err;
        if (!e.setLayerVideo(i, path, &err)) {
            std::printf("      %s: %s\n", qPrintable(path), qPrintable(err));
            return {};
        }
        QElapsedTimer tm;
        tm.start();
        auto stagedNow = [&] {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(i);
            return l->videoTex ? l->videoTex->stagedUploads() : 0;
        };
        while ((e.videoFramesShown(i) < 3 || stagedNow() < 1) && tm.elapsed() < 5000) {
            e.renderFrame();
            QThread::msleep(15);
        }
        e.setLayerPlaying(i, false);
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(i);
            if (description) *description = l->frame.layout ? l->frame.layout->description : QString();
            if (staged) *staged = l->videoTex ? l->videoTex->stagedUploads() : 0;
        }
        return e.grabLayerSource(i);
    };

    // --- Pixel layouts: the GPU's conversion against swscale's (smooth pictures, static)
    struct Case {
        const char *file;
        const char *layout; // start of the description
        bool alpha;
    };
    const Case cases[] = {
        {"fmt_yuv420p.mp4", "yuv420p", false},       {"fmt_yuv420p_odd.nut", "yuv420p", false},
        {"fmt_yuvj420p.mov", "yuvj420p", false},     {"fmt_yuv422p10.mov", "yuv422p10le", false},
        {"fmt_yuva444p10.mov", "yuva444p12le", true}, {"fmt_nv12.nut", "nv12", false},
        {"fmt_yuv444p.nut", "yuv444p", false},
        {"fmt_gbrp10le.nut", "gbrp10le", false},     {"fmt_gray.nut", "gray", false},
        {"fmt_gray16le.nut", "gray16le", false},     {"fmt_bgr0.nut", "bgr0", false},
        {"fmt_rgb48le.nut", "rgb48le", false},       {"fmt_rgba.mov", "rgba", true},
        {"fmt_yuyv422.nut", "yuyv422 · converted on the CPU", false},
        {"fmt_rgb565le.nut", "rgb565le · converted on the CPU", false},
    };
    for (const Case &c : cases) {
        const QString path = root + "/media/" + c.file;
        QString desc;
        quint64 staged = 0;
        const QImage gpu = playAndGrab(path, &desc, &staged);
        const QImage ref = ffmpegReference(path);
        const Diff d = compare(gpu, ref, c.alpha);
        std::printf("      %-22s %-34s max %3d  mean %.2f  staged %llu\n", c.file, qPrintable(desc), d.max, d.mean,
                    (unsigned long long)staged);
        VCHECK(d.sameSize && desc.startsWith(QString::fromUtf8(c.layout)));
        VCHECK(d.max <= 8 && d.mean <= 1.5);
        VCHECK(staged >= 1); // frames came through upload buffers
    }

    // --- Every layout swscale writes: the GPU's conversion of the same frame against swscale's own, through the
    // decoder's memory and through packed bytes (the upload buffers' layout). Formats the GPU path refuses go
    // to the CPU conversion instead.
    {
        const int w = 322, h = 182;
        QImage src(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                src.setPixelColor(x, y, QColor(40 + 180 * x / w, 30 + 200 * y / h, 220 - 150 * (x + y) / (w + h), 60 + 190 * x / w));
        struct Fmt {
            const char *name;
            bool gpu;
        };
        const Fmt fmts[] = {
            {"yuv420p", true},    {"yuv422p", true},     {"yuv444p", true},     {"yuv410p", true},    {"yuv411p", true},
            {"yuv440p", true},    {"yuvj420p", true},    {"yuvj444p", true},    {"yuv420p10le", true}, {"yuv422p10le", true},
            {"yuv444p12le", true}, {"yuv420p16le", true}, {"yuva420p", true},    {"yuva444p10le", true}, {"nv12", true},
            {"nv21", true},       {"nv16", true},        {"nv24", true},        {"nv42", true},       {"p010le", true},
            {"p016le", true},     {"p210le", true},      {"p410le", true},      {"ayuv64le", true},   {"vuya", true},
            {"vuyx", true},       {"gray", true},        {"gray10le", true},    {"gray16le", true},   {"ya8", true},
            {"ya16le", true},     {"gbrp", true},        {"gbrp10le", true},    {"gbrap", true},      {"gbrap16le", true},
            {"rgba", true},       {"bgra", true},        {"argb", true},        {"abgr", true},       {"rgb24", true},
            {"bgr24", true},      {"rgb0", true},        {"0bgr", true},        {"rgb48le", true},    {"rgba64le", true},
            {"bgra64le", true},   {"x2rgb10le", false},  {"yuyv422", false},    {"uyvy422", false},   {"rgb565le", false},
            {"pal8", false},      {"y210le", false},     {"monob", false},      {"yuv420p10be", false},
        };
        e.runGl([&] {
            auto f = gl();
            GLuint vao = 0, vbo = 0, fbo = 0;
            const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
            f->glGenVertexArrays(1, &vao);
            f->glBindVertexArray(vao);
            f->glGenBuffers(1, &vbo);
            f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
            f->glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
            f->glEnableVertexAttribArray(0);
            f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            f->glBindVertexArray(0);
            f->glGenFramebuffers(1, &fbo);
            VideoConverter conv;
            QString err;
            VCHECK(conv.init(vao, &err));
            VideoDecoder none; // upload buffers are not used here
            for (const Fmt &fm : fmts) {
                const AVPixelFormat pf = av_get_pix_fmt(fm.name);
                if (pf == AV_PIX_FMT_NONE || !sws_isSupportedOutput(pf) || !sws_isSupportedInput(pf)) {
                    std::printf("      %-12s (not in this FFmpeg's swscale)\n", fm.name);
                    continue;
                }
                const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(pf);
                const bool yuv = !(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3;
                const bool full = !yuv || QByteArray(fm.name).startsWith("yuvj");
                AVFrame *fr = av_frame_alloc();
                fr->format = pf;
                fr->width = w;
                fr->height = h;
                av_frame_get_buffer(fr, 0);
                fr->colorspace = AVCOL_SPC_SMPTE170M;
                fr->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                fr->chroma_location = AVCHROMA_LOC_CENTER;
                const int flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
                auto details = [&](SwsContext *c, int srcFull, int dstFull) {
                    int *inv, *tbl, sr, dr, bri, con, sat;
                    sws_getColorspaceDetails(c, &inv, &sr, &tbl, &dr, &bri, &con, &sat);
                    sws_setColorspaceDetails(c, sws_getCoefficients(SWS_CS_ITU601), srcFull, sws_getCoefficients(SWS_CS_ITU601),
                                             dstFull, bri, con, sat);
                };
                SwsContext *to = sws_getContext(w, h, AV_PIX_FMT_RGBA, w, h, pf, flags, nullptr, nullptr, nullptr);
                SwsContext *back = sws_getContext(w, h, pf, w, h, AV_PIX_FMT_RGBA, flags, nullptr, nullptr, nullptr);
                QImage ref(w, h, QImage::Format_RGBA8888);
                if (to && back) {
                    details(to, 1, full ? 1 : 0);
                    details(back, full ? 1 : 0, 1);
                    const uint8_t *in[4] = {src.constBits(), nullptr, nullptr, nullptr};
                    const int inStride[4] = {int(src.bytesPerLine()), 0, 0, 0};
                    sws_scale(to, in, inStride, 0, h, fr->data, fr->linesize);
                    uint8_t *out[4] = {ref.bits(), nullptr, nullptr, nullptr};
                    const int outStride[4] = {int(ref.bytesPerLine()), 0, 0, 0};
                    sws_scale(back, fr->data, fr->linesize, 0, h, out, outStride);
                }
                sws_freeContext(to);
                sws_freeContext(back);
                const auto L = VideoDecoder::layoutOf(fr);
                if (!L || !fm.gpu) {
                    std::printf("      %-12s %s\n", fm.name, L ? "taken by the GPU" : "refused: converted on the CPU");
                    VCHECK(!L == !fm.gpu);
                    av_frame_free(&fr);
                    continue;
                }
                const bool alpha = d->flags & AV_PIX_FMT_FLAG_ALPHA;
                for (int path = 0; path < 2; ++path) {
                    VideoFrame vf;
                    vf.layout = L;
                    if (path == 0) {
                        vf.av = av_frame_clone(fr);
                    } else { // packed rows, at the layout's offsets
                        vf.bytes.assign(L->totalBytes, 0);
                        for (int p = 0; p < L->planeCount; ++p)
                            for (int y = 0; y < L->plane[p].height; ++y)
                                std::memcpy(vf.bytes.data() + L->plane[p].offset + size_t(y) * L->plane[p].rowBytes,
                                            fr->data[p] + ptrdiff_t(y) * fr->linesize[p], L->plane[p].rowBytes);
                    }
                    VideoTexture vt;
                    vt.upload(vf, none, conv);
                    QImage gpu(w, h, QImage::Format_RGBA8888);
                    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vt.texture(), 0);
                    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gpu.bits());
                    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    vt.destroy();
                    const Diff df = compare(gpu.mirrored(false, true), ref, alpha);
                    if (path == 0)
                        std::printf("      %-12s %-30s max %3d  mean %.2f\n", fm.name, qPrintable(L->description), df.max, df.mean);
                    // Chroma subsampled by 4 (4:1:0, 4:1:1): interpolations differ a little more
                    const bool coarse = d->log2_chroma_w >= 2 || d->log2_chroma_h >= 2;
                    VCHECK(df.sameSize && df.max <= (coarse ? 5 : 3) && df.mean <= (coarse ? 1.2 : 0.8));
                }
                av_frame_free(&fr);
            }
            conv.release();
            f->glDeleteFramebuffers(1, &fbo);
            f->glDeleteBuffers(1, &vbo);
            f->glDeleteVertexArrays(1, &vao);
        });
    }

    // --- A known color: BT.709 limited range, 8 and 10 bits (independent of swscale)
    {
        const QString p8 = tmp + "/color709.mp4";
        const QString cmd = QStringLiteral("ffmpeg -loglevel error -y -f lavfi -i color=c=0x3080C0:s=1280x720:d=0.2 "
                                           "-vf scale=out_color_matrix=bt709:out_range=tv,format=yuv420p -colorspace bt709 -color_range tv "
                                           "-c:v libx264 -qp 0 %1")
                                .arg(p8);
        if (std::system(cmd.toUtf8().constData()) == 0) {
            const QImage g = playAndGrab(p8, nullptr, nullptr);
            const QRgb px = g.pixel(640, 360);
            std::printf("      0x3080C0 -> %02x %02x %02x\n", qRed(px), qGreen(px), qBlue(px));
            VCHECK(std::abs(qRed(px) - 0x30) <= 2 && std::abs(qGreen(px) - 0x80) <= 2 && std::abs(qBlue(px) - 0xC0) <= 2);
        }
    }

    // --- HAP as FFmpeg encodes it, against FFmpeg's own decoding (on the CPU)
    struct HapCase {
        const char *file, *layout;
        bool alpha;
    };
    const HapCase haps[] = {{"hap1.mov", "Hap · DXT1", false},
                            {"hap5.mov", "Hap Alpha · DXT5", true},
                            {"hapy.mov", "Hap Q · YCoCg DXT5", false},
                            {"hapy_raw.mov", "Hap Q · YCoCg DXT5", false}};
    for (const HapCase &c : haps) {
        const QString path = root + "/media/" + c.file;
        QString desc;
        quint64 staged = 0;
        const QImage gpu = playAndGrab(path, &desc, &staged);
        const Diff d = compare(gpu, ffmpegReference(path), c.alpha);
        std::printf("      %-12s %-30s max %3d  mean %.2f  staged %llu\n", c.file, qPrintable(desc), d.max, d.mean,
                    (unsigned long long)staged);
        VCHECK(d.sameSize && desc == QString::fromUtf8(c.layout));
        VCHECK(d.max <= 8 && d.mean <= 1.0);
        VCHECK(staged >= 1);
    }

    // --- HAP variants FFmpeg does not encode, written here: 250×142 (rounded up to 4 inside), every second
    // stage (none, Snappy, chunks with an offset table), both header sizes, two images in either order
    const int W = 250, H = 142;
    {
        const QByteArray alphaQ[4] = {rgtc1Block(0x20), rgtc1Block(0x60), rgtc1Block(0xA0), rgtc1Block(0xE0)};
        const QByteArray alphaTex = quadrantTexture(W, H, alphaQ);
        const QByteArray ycocgQ[4] = {ycocgBlock(128, 128, 200), ycocgBlock(160, 120, 128), ycocgBlock(100, 150, 90),
                                      ycocgBlock(128, 100, 40)};
        const QByteArray ycocgTex = quadrantTexture(W, H, ycocgQ);
        const QByteArray bc7Q[4] = {bc7Mode6Block(200, 100, 40, 254), bc7Mode6Block(20, 220, 120, 128),
                                    bc7Mode6Block(80, 80, 240, 64), bc7Mode6Block(254, 254, 254, 254)};
        const QByteArray bc7Tex = quadrantTexture(W, H, bc7Q);
        // Half floats: 10-bit endpoints around 450..495 (unsigned) and 200..247 (signed) are within 0..1
        const QByteArray bc6Q[4] = {bc6Mode11Block(450, 470, 490), bc6Mode11Block(490, 400, 470), bc6Mode11Block(0, 480, 0),
                                    bc6Mode11Block(480, 480, 480)};
        const QByteArray bc6Tex = quadrantTexture(W, H, bc6Q);
        const QByteArray bc6sQ[4] = {bc6Mode11Block(220, 230, 240), bc6Mode11Block(240, 200, 230), bc6Mode11Block(0, 240, 0),
                                     bc6Mode11Block(230, 230, 230)};
        const QByteArray bc6sTex = quadrantTexture(W, H, bc6sQ);
        VCHECK(size_t(alphaTex.size()) == hap::textureBytes(hap::Format::AlphaRgtc1, W, H));
        VCHECK(size_t(bc7Tex.size()) == hap::textureBytes(hap::Format::RgbaBc7, W, H));

        struct Made {
            QString file;
            uint32_t tag;
            QList<QByteArray> frames;
            const char *layout;
            bool alpha;
        };
        const QList<Made> made = {
            {"hapA.mov", fourcc('H', 'a', 'p', 'A'),
             {section(0xB1, snappyCompress(alphaTex, 8)), section(0xA1, alphaTex, true)},
             "Hap Alpha-Only · RGTC1", false},
            {"hapM.mov", fourcc('H', 'a', 'p', 'M'),
             {section(0x0D, chunked(0x1, alphaTex, 3, 8) + section(0xAF, ycocgTex)),
              section(0x0D, section(0xBF, snappyCompress(ycocgTex, 16)) + section(0xB1, snappyCompress(alphaTex, 8)))},
             "Hap Q Alpha · YCoCg DXT5 + RGTC1", true},
            {"hap7.mov", fourcc('H', 'a', 'p', '7'),
             {chunked(0xC, bc7Tex, 4, 16), section(0xBC, snappyCompress(bc7Tex, 16))}, "Hap R · BC7", true},
            {"hapH.mov", fourcc('H', 'a', 'p', 'H'), {section(0xA2, bc6Tex), section(0xB2, snappyCompress(bc6Tex, 16))},
             "Hap HDR · BC6U", false},
            {"hapHs.mov", fourcc('H', 'a', 'p', 'H'), {section(0xA3, bc6sTex), section(0xB3, snappyCompress(bc6sTex, 16))}, "Hap HDR · BC6S", false},
        };
        for (const Made &m : made) {
            const QString path = tmp + "/" + m.file;
            QFile::remove(path);
            VCHECK(writeHapMov(path, m.tag, W, H, m.frames));
            // The textures as they come out of the decoder: parsed and decompressed
            {
                hap::Frame hf;
                QString err;
                for (const QByteArray &fr : m.frames) {
                    const bool parsed = hap::parse(reinterpret_cast<const uint8_t *>(fr.constData()), size_t(fr.size()), &hf, &err);
                    bool sizes = parsed;
                    for (int t = 0; parsed && t < hf.count; ++t)
                        sizes = sizes && hf.tex[t].outSize == hap::textureBytes(hf.tex[t].format, W, H);
                    VCHECK(parsed && sizes);
                }
            }
            const QImage cpu = cpuReference(path); // bcdec
            QString desc;
            quint64 staged = 0;
            const QImage gpu = playAndGrab(path, &desc, &staged);
            const Diff d = compare(gpu, cpu, m.alpha);
            std::printf("      %-10s %-36s max %3d  mean %.2f  staged %llu\n", qPrintable(m.file), qPrintable(desc), d.max,
                        d.mean, (unsigned long long)staged);
            const bool cpuDecoded = desc.endsWith(QLatin1String("decoded on the CPU"));
            VCHECK(d.sameSize && desc.startsWith(QString::fromUtf8(m.layout)));
            VCHECK(d.max <= 2);
            VCHECK(staged >= 1);
            (void)cpuDecoded;
        }
        // Known values: the alpha-only quadrants are grey levels, BC7's are its endpoints
        {
            const QImage a = playAndGrab(tmp + "/hapA.mov", nullptr, nullptr);
            VCHECK(!a.isNull() && qRed(a.pixel(10, 10)) == 0x20 && qRed(a.pixel(W - 10, 10)) == 0x60
                   && qRed(a.pixel(10, H - 10)) == 0xA0 && qRed(a.pixel(W - 10, H - 10)) == 0xE0
                   && qAlpha(a.pixel(W - 10, H - 10)) == 255);
            const QImage r = playAndGrab(tmp + "/hap7.mov", nullptr, nullptr);
            const QColor q0 = QColor::fromRgba(r.pixel(10, 10)), q1 = QColor::fromRgba(r.pixel(W - 10, 10));
            VCHECK(q0.red() == 200 && q0.green() == 100 && q0.blue() == 40 && q0.alpha() == 254);
            VCHECK(q1.red() == 20 && q1.green() == 220 && q1.blue() == 120 && q1.alpha() == 128);
            // Half floats 0.409 0.622 0.924 (unsigned), 0.338 0.489 0.781 (signed)
            const QColor h = QColor::fromRgba(playAndGrab(tmp + "/hapH.mov", nullptr, nullptr).pixel(10, 10));
            const QColor hs = QColor::fromRgba(playAndGrab(tmp + "/hapHs.mov", nullptr, nullptr).pixel(10, 10));
            std::printf("      BC6U %d %d %d  BC6S %d %d %d\n", h.red(), h.green(), h.blue(), hs.red(), hs.green(), hs.blue());
            VCHECK(std::abs(h.red() - 104) <= 2 && std::abs(h.green() - 159) <= 2 && std::abs(h.blue() - 236) <= 2);
            VCHECK(std::abs(hs.red() - 86) <= 2 && std::abs(hs.green() - 125) <= 2 && std::abs(hs.blue() - 199) <= 2);
        }
        // The same HAP R and HDR when the GPU cannot sample BPTC (macOS): blocks decoded by the decoder
        {
            const bool dxt = e.gpuSamplesDxt(), bptc = e.gpuSamplesBptc();
            VideoDecoder::setGpuFormats(false, false);
            for (const char *f : {"hap7.mov", "hapH.mov", "hapM.mov"}) {
                QString desc;
                const QImage g = playAndGrab(tmp + "/" + f, &desc, nullptr);
                const Diff d = compare(g, cpuReference(tmp + "/" + f), true);
                std::printf("      %-10s %-50s max %3d\n", f, qPrintable(desc), d.max);
                VCHECK(desc.endsWith(QLatin1String("decoded on the CPU")) && d.sameSize && d.max <= 2);
            }
            VideoDecoder::setGpuFormats(dxt, bptc);
        }
        // A damaged frame is skipped, the next one shown
        {
            QByteArray broken = section(0xB1, snappyCompress(alphaTex, 8));
            broken.truncate(broken.size() / 2);
            const QString path = tmp + "/hapBroken.mov";
            QFile::remove(path);
            VCHECK(writeHapMov(path, fourcc('H', 'a', 'p', 'A'), W, H, {broken, section(0xA1, alphaTex)}));
            const QImage g = playAndGrab(path, nullptr, nullptr);
            VCHECK(!g.isNull() && qRed(g.pixel(10, 10)) == 0x20);
        }
    }

    // --- A hidden layer is neither uploaded nor rendered; shown again, it shows its current frame
    {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        const int i = e.addLayer(QStringLiteral("v"));
        QString err;
        VCHECK(e.setLayerVideo(i, root + "/media/hap.mov", &err));
        QElapsedTimer tm;
        tm.start();
        while (e.videoFramesShown(i) < 3 && tm.elapsed() < 5000) {
            e.renderFrame();
            QThread::msleep(20);
        }
        {
            Engine::Lock lk(&e.mutex());
            e.layer(i)->enabled = false;
        }
        e.renderFrame();
        const quint64 hidden = e.videoFramesShown(i);
        for (int k = 0; k < 10; ++k) {
            e.renderFrame();
            QThread::msleep(20);
        }
        VCHECK(e.videoFramesShown(i) == hidden);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(i)->enabled = true;
        }
        e.renderFrame();
        VCHECK(e.videoFramesShown(i) == hidden + 1);
        Engine::Lock lk(&e.mutex());
        VCHECK(e.layer(i)->finalTex != 0);
    }
    e.newProject();
    return g_failures;
}
