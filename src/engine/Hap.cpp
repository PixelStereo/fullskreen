#include "Hap.h"
#include "Snappy.h"
#include "WorkerPool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"

namespace hap {

static constexpr uint32_t tag(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

int blockBytes(Format f)
{
    switch (f) {
    case Format::RgbDxt1:
    case Format::AlphaRgtc1: return 8;
    case Format::RgbaDxt5:
    case Format::YCoCgDxt5:
    case Format::RgbaBc7:
    case Format::RgbBc6u:
    case Format::RgbBc6s: return 16;
    default: return 0;
    }
}

size_t textureBytes(Format f, int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    return size_t((width + 3) / 4) * size_t((height + 3) / 4) * size_t(blockBytes(f));
}

QString formatName(Format f)
{
    switch (f) {
    case Format::RgbDxt1: return QStringLiteral("DXT1");
    case Format::RgbaDxt5: return QStringLiteral("DXT5");
    case Format::YCoCgDxt5: return QStringLiteral("YCoCg DXT5");
    case Format::RgbaBc7: return QStringLiteral("BC7");
    case Format::AlphaRgtc1: return QStringLiteral("RGTC1");
    case Format::RgbBc6u: return QStringLiteral("BC6U");
    case Format::RgbBc6s: return QStringLiteral("BC6S");
    default: return QString();
    }
}

bool isHapTag(uint32_t t)
{
    return t == tag('H', 'a', 'p', '1') || t == tag('H', 'a', 'p', '5') || t == tag('H', 'a', 'p', 'Y')
           || t == tag('H', 'a', 'p', 'M') || t == tag('H', 'a', 'p', 'A') || t == tag('H', 'a', 'p', '7')
           || t == tag('H', 'a', 'p', 'H');
}

QString variantName(uint32_t t)
{
    if (t == tag('H', 'a', 'p', '1')) return QStringLiteral("Hap");
    if (t == tag('H', 'a', 'p', '5')) return QStringLiteral("Hap Alpha");
    if (t == tag('H', 'a', 'p', 'Y')) return QStringLiteral("Hap Q");
    if (t == tag('H', 'a', 'p', 'M')) return QStringLiteral("Hap Q Alpha");
    if (t == tag('H', 'a', 'p', 'A')) return QStringLiteral("Hap Alpha-Only");
    if (t == tag('H', 'a', 'p', '7')) return QStringLiteral("Hap R");
    if (t == tag('H', 'a', 'p', 'H')) return QStringLiteral("Hap HDR");
    return QStringLiteral("Hap");
}

static uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// A section: its type, its data, and where the next one starts
struct Section {
    uint8_t type = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t total = 0; // header and data
};

static bool readSection(const uint8_t *p, size_t n, Section *s)
{
    if (n < 4) return false;
    size_t size = size_t(p[0]) | size_t(p[1]) << 8 | size_t(p[2]) << 16;
    size_t header = 4;
    if (size == 0) {
        if (n < 8) return false;
        size = le32(p + 4);
        header = 8;
    }
    if (size > n - header) return false;
    s->type = p[3];
    s->data = p + header;
    s->size = size;
    s->total = header + size;
    return true;
}

static bool knownFormat(uint8_t f)
{
    return f == 0xB || f == 0xE || f == 0xF || f == 0xC || f == 0x1 || f == 0x2 || f == 0x3;
}

static bool fail(QString *err, const char *what)
{
    if (err) *err = QString::fromLatin1(what);
    return false;
}

// The decode instructions of a chunked texture, then its chunks
static bool parseChunks(const Section &top, Texture *t, QString *err)
{
    Section ins;
    if (!readSection(top.data, top.size, &ins) || ins.type != 0x01) return fail(err, "HAP: no decode instructions");
    const uint8_t *frameData = top.data + ins.total;
    const size_t frameSize = top.size - ins.total;
    const uint8_t *comp = nullptr, *sizes = nullptr, *offsets = nullptr;
    size_t nComp = 0, nSizes = 0, nOffsets = 0;
    for (size_t at = 0; at < ins.size;) {
        Section s;
        if (!readSection(ins.data + at, ins.size - at, &s)) return fail(err, "HAP: malformed decode instructions");
        if (s.type == 0x02) {
            comp = s.data;
            nComp = s.size;
        } else if (s.type == 0x03) {
            sizes = s.data;
            nSizes = s.size / 4;
        } else if (s.type == 0x04) {
            offsets = s.data;
            nOffsets = s.size / 4;
        } // unknown sections: skipped
        at += s.total;
    }
    if (!comp || !sizes || nComp == 0 || nSizes != nComp || (offsets && nOffsets != nComp))
        return fail(err, "HAP: inconsistent chunk tables");
    t->chunks.resize(nComp);
    size_t running = 0, out = 0;
    for (size_t i = 0; i < nComp; ++i) {
        Chunk &c = t->chunks[i];
        c.compressor = comp[i];
        c.size = le32(sizes + 4 * i);
        const size_t off = offsets ? le32(offsets + 4 * i) : running;
        running = off + c.size;
        if (off > frameSize || c.size > frameSize - off) return fail(err, "HAP: chunk out of the frame");
        c.data = frameData + off;
        if (c.compressor == 0x0A) {
            c.outSize = c.size;
        } else if (c.compressor == 0x0B) {
            if (!snappy::uncompressedLength(c.data, c.size, &c.outSize)) return fail(err, "HAP: malformed Snappy chunk");
        } else {
            return fail(err, "HAP: unknown chunk compressor");
        }
        c.outOffset = out;
        out += c.outSize;
    }
    t->outSize = out;
    return true;
}

static bool parseTexture(const Section &s, Texture *t, QString *err)
{
    const uint8_t fmt = s.type & 0x0F, comp = s.type >> 4;
    if (!knownFormat(fmt)) return fail(err, "HAP: unknown texture format");
    t->format = Format(fmt);
    t->compressor = uint8_t(comp);
    t->data = s.data;
    t->size = s.size;
    t->chunks.clear();
    switch (comp) {
    case 0x0A: t->outSize = s.size; return true;
    case 0x0B:
        if (!snappy::uncompressedLength(s.data, s.size, &t->outSize)) return fail(err, "HAP: malformed Snappy data");
        return true;
    case 0x0C: return parseChunks(s, t, err);
    default: return fail(err, "HAP: unknown second-stage compressor");
    }
}

bool parse(const uint8_t *data, size_t size, Frame *out, QString *err)
{
    out->count = 0;
    Section top;
    if (!readSection(data, size, &top)) return fail(err, "HAP: truncated frame");
    if (top.type != 0x0D) {
        if (!parseTexture(top, &out->tex[0], err)) return false;
        out->count = 1;
        return true;
    }
    // Multiple images: the color (YCoCg) first, the alpha second, whatever their order in the frame
    Texture found[2];
    int n = 0;
    for (size_t at = 0; at < top.size && n < 2;) {
        Section s;
        if (!readSection(top.data + at, top.size - at, &s)) return fail(err, "HAP: malformed image section");
        at += s.total;
        if (!knownFormat(s.type & 0x0F)) continue; // unknown section: skipped
        if (!parseTexture(s, &found[n], err)) return false;
        ++n;
    }
    if (n == 0) return fail(err, "HAP: no image");
    if (n == 2 && found[0].format == Format::AlphaRgtc1) std::swap(found[0], found[1]);
    for (int i = 0; i < n; ++i) out->tex[i] = std::move(found[i]);
    out->count = n;
    return true;
}

bool decompress(const Texture &t, uint8_t *dst, size_t dstSize)
{
    if (dstSize != t.outSize) return false;
    switch (t.compressor) {
    case 0x0A: std::memcpy(dst, t.data, t.size); return true;
    case 0x0B: return snappy::decompress(t.data, t.size, dst, dstSize);
    case 0x0C: {
        std::atomic<bool> ok{true};
        workers::parallelFor(int(t.chunks.size()), [&](int i) {
            const Chunk &c = t.chunks[size_t(i)];
            if (c.compressor == 0x0A) std::memcpy(dst + c.outOffset, c.data, c.size);
            else if (!snappy::decompress(c.data, c.size, dst + c.outOffset, c.outSize)) ok = false;
        });
        return ok;
    }
    default: return false;
    }
}

int decodedChannels(Format f) { return f == Format::AlphaRgtc1 ? 1 : 4; }

static inline uint8_t unitToByte(float v)
{
    if (!(v > 0.0f)) return 0; // NaN too
    if (v >= 1.0f) return 255;
    return uint8_t(v * 255.0f + 0.5f);
}

void decodeBlocks(Format f, const uint8_t *blocks, int width, int height, uint8_t *dst, size_t stride)
{
    const int bw = (width + 3) / 4, bh = (height + 3) / 4, bb = blockBytes(f), ch = decodedChannels(f);
    if (!bb) return;
    const int rowsPerTask = 8;
    const int tasks = (bh + rowsPerTask - 1) / rowsPerTask;
    const int pitch = int(stride);
    workers::parallelFor(tasks, [&](int task) {
        float hdr[16 * 3];
        const int y1 = std::min(bh, (task + 1) * rowsPerTask);
        for (int by = task * rowsPerTask; by < y1; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t *src = blocks + (size_t(by) * bw + bx) * bb;
                uint8_t *out = dst + size_t(by) * 4 * stride + size_t(bx) * 4 * ch;
                switch (f) {
                case Format::RgbDxt1: bcdec_bc1(src, out, pitch); break;
                case Format::RgbaDxt5:
                case Format::YCoCgDxt5: bcdec_bc3(src, out, pitch); break;
                case Format::RgbaBc7: bcdec_bc7(src, out, pitch); break;
                case Format::AlphaRgtc1: bcdec_bc4(src, out, pitch); break;
                case Format::RgbBc6u:
                case Format::RgbBc6s:
                    bcdec_bc6h_float(src, hdr, 4 * 3, f == Format::RgbBc6s ? 1 : 0);
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const float *p = hdr + (y * 4 + x) * 3;
                            uint8_t *o = out + size_t(y) * stride + size_t(x) * 4;
                            o[0] = unitToByte(p[0]);
                            o[1] = unitToByte(p[1]);
                            o[2] = unitToByte(p[2]);
                            o[3] = 255;
                        }
                    break;
                default: break;
                }
            }
    });
}

} // namespace hap
