#include "VideoTexture.h"
#include "VideoDecoder.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <QOpenGLContext>
#include <algorithm>
#include <cstring>

#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#ifndef GL_RGB16
#define GL_RGB16 0x8054
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_NUM_COMPRESSED_TEXTURE_FORMATS
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#endif
#ifndef GL_COMPRESSED_TEXTURE_FORMATS
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3
#endif

static constexpr GLenum kDxt1 = 0x83F0;  // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
static constexpr GLenum kDxt5 = 0x83F3;  // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
static constexpr GLenum kRgtc1 = 0x8DBB; // GL_COMPRESSED_RED_RGTC1 (OpenGL 3.0)
static constexpr GLenum kBc7 = 0x8E8C;   // GL_COMPRESSED_RGBA_BPTC_UNORM
static constexpr GLenum kBc6s = 0x8E8E;  // GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT
static constexpr GLenum kBc6u = 0x8E8F;  // GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT

// ---------------------------------------------------------------------------
// Conversion program
// ---------------------------------------------------------------------------
static const char *kVs = "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                         "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n";

// Modes: 0 YUV, 1 RGB, 2 grey; HAP: 3 RGB(A), 4 scaled YCoCg, 5 scaled YCoCg + alpha, 6 alpha only (grey)
static const char *kFs = R"(#version 330 core
uniform sampler2D u_t0, u_t1, u_t2, u_t3;
uniform vec4 u_sel0, u_sel1, u_sel2, u_sel3; // which channel of the texel
uniform vec2 u_off0, u_off1, u_off2, u_off3; // chroma siting
uniform vec4 u_scale;                        // bit depth held in 16 bits
uniform int u_mode, u_alpha;
uniform mat3 u_matrix;
uniform vec4 u_range;                        // Y offset, Y scale, C offset, C scale
uniform vec2 u_crop, u_clamp;                // part of the textures holding the picture
in vec2 v_uv; out vec4 o;
void main() {
    vec2 uv = min(vec2(v_uv.x, 1.0 - v_uv.y) * u_crop, u_clamp); // stored top row first
    if (u_mode >= 3) {
        vec4 c = texture(u_t0, uv);
        if (u_mode == 3) { o = vec4(clamp(c.rgb, 0.0, 1.0), u_alpha != 0 ? c.a : 1.0); return; }
        if (u_mode == 6) { o = vec4(c.rrr, 1.0); return; }
        float s = c.z * (255.0 / 8.0) + 1.0;
        float co = (c.x - 128.0 / 255.0) / s, cg = (c.y - 128.0 / 255.0) / s;
        vec3 rgb = vec3(c.w + co - cg, c.w + cg, c.w - co - cg);
        o = vec4(clamp(rgb, 0.0, 1.0), u_mode == 5 ? texture(u_t1, uv).r : 1.0);
        return;
    }
    float a = dot(texture(u_t0, uv + u_off0), u_sel0) * u_scale.x;
    float b = dot(texture(u_t1, uv + u_off1), u_sel1) * u_scale.y;
    float c = dot(texture(u_t2, uv + u_off2), u_sel2) * u_scale.z;
    float alpha = u_alpha != 0 ? dot(texture(u_t3, uv + u_off3), u_sel3) * u_scale.w : 1.0;
    vec3 rgb = u_mode == 0 ? u_matrix * vec3((a - u_range.x) * u_range.y, (b - u_range.z) * u_range.w, (c - u_range.z) * u_range.w)
             : u_mode == 1 ? vec3(a, b, c) : vec3(a);
    o = vec4(clamp(rgb, 0.0, 1.0), clamp(alpha, 0.0, 1.0));
}
)";

bool VideoConverter::init(GLuint quadVao, QString *err)
{
    auto f = gl();
    QString log;
    m_program = compileProgram(QString::fromLatin1(kVs), QString::fromLatin1(kFs), &log);
    if (!m_program) {
        if (err) *err = QStringLiteral("Video conversion shader: ") + log;
        return false;
    }
    m_quadVao = quadVao;
    for (int i = 0; i < 4; ++i) {
        m_tex[i] = f->glGetUniformLocation(m_program, QStringLiteral("u_t%1").arg(i).toLatin1().constData());
        m_sel[i] = f->glGetUniformLocation(m_program, QStringLiteral("u_sel%1").arg(i).toLatin1().constData());
        m_off[i] = f->glGetUniformLocation(m_program, QStringLiteral("u_off%1").arg(i).toLatin1().constData());
    }
    m_scale = f->glGetUniformLocation(m_program, "u_scale");
    m_mode = f->glGetUniformLocation(m_program, "u_mode");
    m_alpha = f->glGetUniformLocation(m_program, "u_alpha");
    m_matrix = f->glGetUniformLocation(m_program, "u_matrix");
    m_range = f->glGetUniformLocation(m_program, "u_range");
    m_crop = f->glGetUniformLocation(m_program, "u_crop");
    m_clamp = f->glGetUniformLocation(m_program, "u_clamp");

    // Compressed formats the GPU samples: by extension, or listed by the driver
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    GLint n = 0;
    f->glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    std::vector<GLint> listed(size_t(std::max(0, n)));
    if (n > 0) f->glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, listed.data());
    while (f->glGetError() != GL_NO_ERROR) {}
    auto has = [&](GLenum e) { return std::find(listed.begin(), listed.end(), GLint(e)) != listed.end(); };
    const QSurfaceFormat fmt = ctx->format();
    const int version = fmt.majorVersion() * 10 + fmt.minorVersion();
    m_s3tc = ctx->hasExtension("GL_EXT_texture_compression_s3tc") || (has(kDxt1) && has(kDxt5));
    m_bptc = version >= 42 || ctx->hasExtension("GL_ARB_texture_compression_bptc") || ctx->hasExtension("GL_EXT_texture_compression_bptc")
             || (has(kBc7) && has(kBc6u));
    return true;
}

void VideoConverter::release()
{
    if (m_program) gl()->glDeleteProgram(m_program);
    m_program = 0;
}

// ---------------------------------------------------------------------------
// Upload buffers
// ---------------------------------------------------------------------------
void VideoTexture::createPbo(VideoDecoder &dec)
{
    auto f = gl();
    int id = -1;
    for (size_t i = 0; i < m_pbos.size(); ++i)
        if (!m_pbos[i].buffer) {
            id = int(i);
            break;
        }
    if (id < 0) {
        id = int(m_pbos.size());
        m_pbos.push_back({});
    }
    Pbo &b = m_pbos[size_t(id)];
    f->glGenBuffers(1, &b.buffer);
    f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b.buffer);
    f->glBufferData(GL_PIXEL_UNPACK_BUFFER, GLsizeiptr(m_pboSize), nullptr, GL_STREAM_DRAW);
    void *ptr = f->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, GLsizeiptr(m_pboSize), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    b.size = m_pboSize;
    if (!ptr) { // no mapping: frames go through the decoder's memory
        f->glDeleteBuffers(1, &b.buffer);
        b = Pbo{};
        return;
    }
    b.mapped = true;
    ++m_live;
    dec.addStaging(id, static_cast<uint8_t *>(ptr), m_pboSize);
}

void VideoTexture::destroyPbo(int id)
{
    if (id < 0 || id >= int(m_pbos.size()) || !m_pbos[size_t(id)].buffer) return;
    auto f = gl();
    Pbo &b = m_pbos[size_t(id)];
    if (b.mapped) {
        f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b.buffer);
        f->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    f->glDeleteBuffers(1, &b.buffer);
    b = Pbo{};
    --m_live;
}

void VideoTexture::feed(VideoDecoder &dec)
{
    const size_t want = dec.stagingSize();
    if (want != m_pboSize) { // another frame size: the free buffers go now, the others as they come back
        for (int id : dec.takeFreeStaging()) destroyPbo(id);
        m_pboSize = want;
    }
    if (!want) return;
    const int count = dec.stagingCount();
    for (int guard = 0; m_live < count && guard < count; ++guard) {
        const int before = m_live;
        createPbo(dec);
        if (m_live == before) break; // mapping refused
    }
}

// ---------------------------------------------------------------------------
// Planes
// ---------------------------------------------------------------------------
static bool compressedTexels(VideoLayout::Texels t) { return t != VideoLayout::Texels::U8 && t != VideoLayout::Texels::U16; }

static GLenum compressedFormat(VideoLayout::Texels t)
{
    switch (t) {
    case VideoLayout::Texels::Dxt1: return kDxt1;
    case VideoLayout::Texels::Dxt5: return kDxt5;
    case VideoLayout::Texels::Rgtc1: return kRgtc1;
    case VideoLayout::Texels::Bc7: return kBc7;
    case VideoLayout::Texels::Bc6u: return kBc6u;
    case VideoLayout::Texels::Bc6s: return kBc6s;
    default: return 0;
    }
}

static void plainFormat(const VideoLayout::Plane &p, GLenum *internal, GLenum *format, GLenum *type)
{
    const bool wide = p.texels == VideoLayout::Texels::U16;
    static const GLenum formats[4] = {GL_RED, GL_RG, GL_RGB, GL_RGBA};
    static const GLenum narrow[4] = {GL_R8, GL_RG8, GL_RGB8, GL_RGBA8};
    static const GLenum wideFmt[4] = {GL_R16, GL_RG16, GL_RGB16, GL_RGBA16};
    const int c = std::clamp(p.channels, 1, 4) - 1;
    *internal = wide ? wideFmt[c] : narrow[c];
    *format = formats[c];
    *type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
}

static bool sameShape(const VideoLayout::Plane &a, const VideoLayout::Plane &b)
{
    return a.width == b.width && a.height == b.height && a.channels == b.channels && a.texels == b.texels;
}

void VideoTexture::ensurePlanes(const VideoLayout &L)
{
    auto f = gl();
    for (int p = 0; p < 4; ++p) {
        if (p >= L.planeCount) continue; // kept: another layout may come back
        if (m_planes[p] && sameShape(m_planeShape[p], L.plane[p])) continue;
        if (!m_planes[p]) {
            f->glGenTextures(1, &m_planes[p]);
            f->glBindTexture(GL_TEXTURE_2D, m_planes[p]);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_planeShape[p] = L.plane[p];
        m_allocated[p] = false; // allocated by its first upload
    }
}

// rowPixels: texels per row in memory (0: packed)
void VideoTexture::uploadPlane(int p, const VideoLayout::Plane &pl, const void *data, int rowPixels)
{
    auto f = gl();
    f->glBindTexture(GL_TEXTURE_2D, m_planes[p]);
    if (compressedTexels(pl.texels)) {
        const GLenum fmt = compressedFormat(pl.texels);
        if (!m_allocated[p]) f->glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, pl.width, pl.height, 0, GLsizei(pl.size), data);
        else f->glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pl.width, pl.height, fmt, GLsizei(pl.size), data);
    } else {
        GLenum internal, format, type;
        plainFormat(pl, &internal, &format, &type);
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, rowPixels);
        if (!m_allocated[p]) f->glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), pl.width, pl.height, 0, format, type, data);
        else f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pl.width, pl.height, format, type, data);
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    m_allocated[p] = true;
}

void VideoTexture::upload(VideoFrame &fr, VideoDecoder &dec, const VideoConverter &conv)
{
    if (!fr.valid()) return;
    const VideoLayout &L = *fr.layout;
    auto f = gl();
    ensurePlanes(L);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bool ok = true;
    if (fr.staging >= 0 && fr.staging < int(m_pbos.size()) && m_pbos[size_t(fr.staging)].buffer) {
        // Written by the decoder: the GPU copies it from there
        const int id = fr.staging;
        Pbo &b = m_pbos[size_t(id)];
        f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b.buffer);
        ok = f->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE; // false: its content was lost (rare)
        b.mapped = false;
        if (ok) {
            for (int p = 0; p < L.planeCount; ++p)
                uploadPlane(p, L.plane[p], reinterpret_cast<const void *>(L.plane[p].offset), 0);
            ++m_staged;
        }
        // Back to the decoder, mapped again on new storage (orphaned: the GPU keeps the old one until its copy is
        // done, so mapping does not wait for it)
        if (b.size == m_pboSize && m_pboSize >= L.totalBytes) {
            f->glBufferData(GL_PIXEL_UNPACK_BUFFER, GLsizeiptr(b.size), nullptr, GL_STREAM_DRAW);
            void *ptr = f->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, GLsizeiptr(b.size),
                                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            if (ptr) {
                b.mapped = true;
                dec.addStaging(id, static_cast<uint8_t *>(ptr), b.size);
            } else {
                destroyPbo(id);
            }
        } else {
            f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            destroyPbo(id); // a size no longer used
        }
        fr.staging = -1;
    } else if (fr.av) {
        // Kept by the decoder (no free upload buffer): read in place, rows repacked if need be
        for (int p = 0; p < L.planeCount; ++p) {
            const VideoLayout::Plane &pl = L.plane[p];
            const int line = fr.av->linesize[p];
            const int texel = int(pl.rowBytes / size_t(std::max(1, pl.width)));
            if (line > 0 && texel > 0 && line % texel == 0) {
                uploadPlane(p, pl, fr.av->data[p], line / texel);
            } else {
                m_repack.resize(pl.size);
                for (int y = 0; y < pl.height; ++y)
                    std::memcpy(m_repack.data() + size_t(y) * pl.rowBytes, fr.av->data[p] + ptrdiff_t(y) * line, pl.rowBytes);
                uploadPlane(p, pl, m_repack.data(), 0);
            }
        }
    } else if (!fr.bytes.empty()) {
        for (int p = 0; p < L.planeCount; ++p) uploadPlane(p, L.plane[p], fr.bytes.data() + L.plane[p].offset, 0);
    } else {
        ok = false;
    }
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (!ok) return;
    convert(L, conv);
    ++m_uploads;
}

void VideoTexture::convert(const VideoLayout &L, const VideoConverter &conv)
{
    auto f = gl();
    m_out.ensure(L.width, L.height);
    m_out.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(conv.m_program);
    const bool hap = L.mode >= VideoLayout::Mode::HapRgb;
    int mode = 0;
    switch (L.mode) {
    case VideoLayout::Mode::Yuv: mode = 0; break;
    case VideoLayout::Mode::Rgb: mode = 1; break;
    case VideoLayout::Mode::Gray: mode = 2; break;
    case VideoLayout::Mode::HapRgb: mode = 3; break;
    case VideoLayout::Mode::HapYCoCg: mode = 4; break;
    case VideoLayout::Mode::HapYCoCgAlpha: mode = 5; break;
    case VideoLayout::Mode::HapAlphaOnly: mode = 6; break;
    }
    for (int c = 0; c < 4; ++c) {
        // HAP: units 0 and 1 are its textures; planes: unit c holds the plane of output channel c
        const int plane = hap ? std::min(c, L.planeCount - 1) : (L.srcPlane[c] >= 0 ? L.srcPlane[c] : 0);
        f->glActiveTexture(GL_TEXTURE0 + GLenum(c));
        f->glBindTexture(GL_TEXTURE_2D, m_planes[plane]);
        f->glUniform1i(conv.m_tex[c], c);
        float sel[4] = {0, 0, 0, 0};
        if (!hap && L.srcPlane[c] >= 0) sel[std::clamp(L.srcChannel[c], 0, 3)] = 1;
        f->glUniform4fv(conv.m_sel[c], 1, sel);
        float off[2] = {0, 0};
        if (L.mode == VideoLayout::Mode::Yuv && (c == 1 || c == 2) && L.srcPlane[c] >= 0) {
            const VideoLayout::Plane &pl = L.plane[L.srcPlane[c]];
            off[0] = L.chromaShift[0] / float(std::max(1, pl.width));
            off[1] = L.chromaShift[1] / float(std::max(1, pl.height));
        }
        f->glUniform2fv(conv.m_off[c], 1, off);
    }
    f->glUniform4fv(conv.m_scale, 1, L.scale);
    f->glUniform1i(conv.m_mode, mode);
    f->glUniform1i(conv.m_alpha, L.alpha ? 1 : 0);
    f->glUniformMatrix3fv(conv.m_matrix, 1, GL_TRUE, L.matrix);
    f->glUniform4f(conv.m_range, L.yOffset, L.yScale, L.cOffset, L.cScale);
    const VideoLayout::Plane &p0 = L.plane[0];
    const float cx = float(L.width) / float(std::max(1, p0.width)), cy = float(L.height) / float(std::max(1, p0.height));
    f->glUniform2f(conv.m_crop, cx, cy);
    // HAP's textures are rounded up to 4: never sampled beyond the picture's last texel center
    f->glUniform2f(conv.m_clamp, hap ? (float(L.width) - 0.5f) / float(std::max(1, p0.width)) : 1.0f,
                   hap ? (float(L.height) - 0.5f) / float(std::max(1, p0.height)) : 1.0f);
    f->glBindVertexArray(conv.m_quadVao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    f->glBindVertexArray(0);
    for (int c = 3; c >= 0; --c) {
        f->glActiveTexture(GL_TEXTURE0 + GLenum(c));
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void VideoTexture::destroy()
{
    auto f = gl();
    for (int id = 0; id < int(m_pbos.size()); ++id) destroyPbo(id);
    m_pbos.clear();
    m_live = 0;
    m_pboSize = 0;
    for (GLuint &t : m_planes)
        if (t) {
            f->glDeleteTextures(1, &t);
            t = 0;
        }
    for (bool &a : m_allocated) a = false;
    m_out.destroy();
}
