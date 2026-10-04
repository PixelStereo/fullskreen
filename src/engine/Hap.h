#pragma once
// HAP frames (Vidvox), every variant: Hap (DXT1), Hap Alpha (DXT5), Hap Q (scaled YCoCg DXT5),
// Hap Q Alpha (YCoCg DXT5 + RGTC1 alpha), Hap Alpha-Only (RGTC1), Hap R (BC7), Hap HDR (BC6U / BC6S).
// Specification: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
//
// A frame holds one or two textures, compressed in blocks of 4×4 pixels that the GPU samples as they are.
// Only their second stage (none, Snappy, or chunks of either) is undone here, on the CPU; the chunks of one
// frame are decompressed side by side. When the GPU cannot sample a texture format (BC7 and BC6 on macOS),
// its blocks are decoded here too (decodeBlocks), into plain 8-bit pixels.

#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hap {

// Texture formats: the low four bits of a section type
enum class Format : uint8_t {
    None = 0,
    RgbDxt1 = 0xB,    // BC1
    RgbaDxt5 = 0xE,   // BC3
    YCoCgDxt5 = 0xF,  // BC3 holding scaled YCoCg
    RgbaBc7 = 0xC,    // BPTC unorm
    AlphaRgtc1 = 0x1, // BC4
    RgbBc6u = 0x2,    // BPTC unsigned float
    RgbBc6s = 0x3,    // BPTC signed float
};

int blockBytes(Format f);                          // 8 or 16 (0: unknown)
size_t textureBytes(Format f, int width, int height); // blocks of the picture, its size rounded up to 4
QString formatName(Format f);

// Four-character codes of the HAP variants in a container (Hap1, Hap5, HapY, HapM, HapA, Hap7, HapH)
bool isHapTag(uint32_t fourcc);
QString variantName(uint32_t fourcc); // "Hap Q Alpha"…

struct Chunk {
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint8_t compressor = 0x0A; // 0x0A none, 0x0B Snappy
    size_t outOffset = 0, outSize = 0;
};

struct Texture {
    Format format = Format::None;
    uint8_t compressor = 0x0A;     // 0x0A none, 0x0B Snappy, 0x0C chunks
    const uint8_t *data = nullptr; // the section's data (points into the packet)
    size_t size = 0;
    std::vector<Chunk> chunks;     // compressor 0x0C
    size_t outSize = 0;            // once decompressed
};

// The textures of one frame: one, or two for Hap Q Alpha (the color first, the alpha second)
struct Frame {
    int count = 0;
    Texture tex[2];
};

// Reads the sections of a frame. False (and err) if it is not a well-formed HAP frame.
bool parse(const uint8_t *data, size_t size, Frame *out, QString *err = nullptr);

// Undoes the second stage of a texture into dst (exactly t.outSize bytes)
bool decompress(const Texture &t, uint8_t *dst, size_t dstSize);

// Decodes blocks on the CPU, top row first: RGBA 8 bits (or one channel for RGTC1), the picture's size
// rounded up to 4, `stride` bytes per row. BC6 is clamped to [0, 1]. Rows of blocks spread over the workers.
void decodeBlocks(Format f, const uint8_t *blocks, int width, int height, uint8_t *dst, size_t stride);
int decodedChannels(Format f); // 1 for RGTC1, 4 otherwise

} // namespace hap
