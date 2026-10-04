#pragma once
// Snappy decompression (the second stage of HAP frames), after Google's format description:
// https://github.com/google/snappy/blob/main/format_description.txt
// Only decompression, of raw (unframed) streams.

#include <cstddef>
#include <cstdint>

namespace snappy {

// Size of the data once decompressed, read from the stream's preamble. False if it is malformed.
bool uncompressedLength(const uint8_t *src, size_t srcSize, size_t *length);

// Decompresses a whole stream into dst, which must hold exactly the announced length.
// False on a malformed stream (dst then holds garbage, never more than dstSize bytes written).
bool decompress(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize);

} // namespace snappy
