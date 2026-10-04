// Video pipeline tests: every pixel layout the GPU converts, compared with FFmpeg's own decoding (swscale);
// hidden layers; upload buffers.
// Called from engine_test.cpp (manual rendering mode, engine initialized).
#include "Engine.h"
#include "Gl.h"

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
// The tests
// ---------------------------------------------------------------------------
int runVideoTests(Engine &e, const QString &root, const QString &tmp)
{
    g_failures = 0;
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

    // --- A hidden layer is neither uploaded nor rendered; shown again, it shows its current frame
    {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        const int i = e.addLayer(QStringLiteral("v"));
        QString err;
        VCHECK(e.setLayerVideo(i, root + "/media/h264.mp4", &err));
        QElapsedTimer tm;
        tm.start();
        while (e.videoFramesShown(i) < 3 && tm.elapsed() < 5000) {
            e.renderFrame();
            QThread::msleep(20);
        }
        {
            Engine::Lock lk(&e.mutex());
            e.layer(i)->visible = false;
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
            e.layer(i)->visible = true;
        }
        e.renderFrame();
        VCHECK(e.videoFramesShown(i) == hidden + 1);
        Engine::Lock lk(&e.mutex());
        VCHECK(e.layer(i)->finalTex != 0);
    }
    e.newProject();
    return g_failures;
}
