#include "Snappy.h"

#include <cstring>

namespace snappy {

static bool readVarint(const uint8_t *&p, const uint8_t *end, size_t *value)
{
    size_t v = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= size_t(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            if (v > 0xffffffffull) return false;
            *value = v;
            return true;
        }
    }
    return false;
}

bool uncompressedLength(const uint8_t *src, size_t srcSize, size_t *length)
{
    const uint8_t *p = src;
    return readVarint(p, src + srcSize, length);
}

static inline uint32_t readLe(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}

bool decompress(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize)
{
    const uint8_t *ip = src, *const iend = src + srcSize;
    size_t announced = 0;
    if (!readVarint(ip, iend, &announced) || announced != dstSize) return false;
    uint8_t *op = dst, *const oend = dst + dstSize;

    while (ip < iend) {
        const uint8_t tag = *ip++;
        size_t len, offset;
        switch (tag & 3) {
        case 0: { // literal
            len = (tag >> 2) + 1;
            if (len > 60) {
                const int extra = int(len - 60); // 1 to 4 bytes of length
                if (iend - ip < extra) return false;
                len = size_t(readLe(ip, extra)) + 1;
                ip += extra;
            }
            if (size_t(iend - ip) < len || size_t(oend - op) < len) return false;
            std::memcpy(op, ip, len);
            ip += len;
            op += len;
            continue;
        }
        case 1: // copy, 1-byte offset
            if (ip >= iend) return false;
            len = 4 + ((tag >> 2) & 7);
            offset = (size_t(tag >> 5) << 8) | *ip++;
            break;
        case 2: // copy, 2-byte offset
            if (iend - ip < 2) return false;
            len = 1 + (tag >> 2);
            offset = readLe(ip, 2);
            ip += 2;
            break;
        default: // copy, 4-byte offset
            if (iend - ip < 4) return false;
            len = 1 + (tag >> 2);
            offset = readLe(ip, 4);
            ip += 4;
            break;
        }
        if (offset == 0 || offset > size_t(op - dst) || size_t(oend - op) < len) return false;
        const uint8_t *from = op - offset;
        if (offset >= len) {
            std::memcpy(op, from, len);
            op += len;
        } else if (offset >= 8) { // overlapping, but by whole 8-byte steps
            while (len >= 8) {
                std::memcpy(op, from, 8);
                op += 8;
                from += 8;
                len -= 8;
            }
            while (len--) *op++ = *from++;
        } else { // a short pattern repeated
            while (len--) *op++ = *from++;
        }
    }
    return op == oend;
}

} // namespace snappy
