#pragma once
// A decoded picture as the GPU takes it: its planes as they come out of the decoder (YUV, 8 or 16 bits,
// subsampled or not, RGB, grey). The GPU turns it into RGBA (VideoTexture): no conversion and no copy on the
// CPU beyond the one into the upload buffer.

#include <QString>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct AVFrame;

// How the planes of a frame are laid out and how the GPU makes RGBA of them. Shared by the frames of one
// stream (it changes only when the stream's format does).
struct VideoLayout {
    enum class Mode {
        Yuv,  // Y, U, V (and A) through a matrix
        Rgb,  // R, G, B (and A)
        Gray, // one channel (and A)
    };
    // Texel format of a plane
    enum class Texels { U8, U16 };

    struct Plane {
        int width = 0, height = 0; // in texels
        int channels = 1;          // 1 to 4
        Texels texels = Texels::U8;
        size_t offset = 0, size = 0; // in a frame's packed data
        size_t rowBytes = 0;         // packed row
    };

    Mode mode = Mode::Rgb;
    int width = 0, height = 0; // of the picture
    int planeCount = 0;
    Plane plane[4];
    size_t totalBytes = 0; // packed size of a frame

    // Where each output channel is read (Y U V A, or R G B A, or grey and A): plane and channel, -1: none
    int srcPlane[4] = {-1, -1, -1, -1};
    int srcChannel[4] = {0, 0, 0, 0};
    float scale[4] = {1, 1, 1, 1}; // the value read is multiplied by this (bits held in 16)
    bool alpha = false;
    // Yuv: Y' = (y - yOffset) * yScale, C' = (c - cOffset) * cScale, then RGB = matrix (rows) × (Y', Cb', Cr')
    float yOffset = 0, yScale = 1, cOffset = 0.5f, cScale = 1;
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float chromaShift[2] = {0, 0}; // chroma siting, in texels of the chroma planes
    QString description;           // "yuv420p · BT.709"…
};

// One frame of a stream. Its bytes are in one of three places:
//  - an upload buffer of the GPU (staging >= 0), packed at the layout's offsets;
//  - `bytes`, packed at the layout's offsets;
//  - the decoder's own frame (av), plane p at av->data[p] with av->linesize[p] bytes per row.
struct VideoFrame {
    double pts = 0;
    std::shared_ptr<const VideoLayout> layout;
    int staging = -1;
    std::vector<uint8_t> bytes;
    AVFrame *av = nullptr;

    VideoFrame() = default;
    ~VideoFrame();
    VideoFrame(VideoFrame &&o) noexcept;
    VideoFrame &operator=(VideoFrame &&o) noexcept;
    VideoFrame(const VideoFrame &) = delete;
    VideoFrame &operator=(const VideoFrame &) = delete;

    bool valid() const { return layout && (staging >= 0 || av || !bytes.empty()); }
    void releaseAv(); // drops the decoder's frame
};
