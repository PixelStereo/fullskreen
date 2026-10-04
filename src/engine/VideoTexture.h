#pragma once
// A video layer's picture on the GPU (render thread).
// The decoder's frames arrive as they were decoded — YUV planes, RGB, grey — in upload buffers (pixel buffer
// objects) the decoder wrote them into on its own thread. Their planes become textures, and one draw turns them
// into the RGBA picture the layer samples: YUV matrix and range, bit depth, chroma siting, alpha, and the
// vertical flip (frames are stored top row first).

#include "Gl.h"
#include "VideoFrame.h"

#include <memory>
#include <vector>

class VideoDecoder;

// The conversion program, shared by every video layer
class VideoConverter
{
public:
    bool init(GLuint quadVao, QString *err); // context current
    void release();

private:
    friend class VideoTexture;
    GLuint m_program = 0, m_quadVao = 0;
    GLint m_tex[4] = {-1, -1, -1, -1}, m_sel[4] = {-1, -1, -1, -1}, m_off[4] = {-1, -1, -1, -1};
    GLint m_scale = -1, m_mode = -1, m_alpha = -1, m_matrix = -1, m_range = -1;
};

class VideoTexture
{
public:
    VideoTexture() = default;
    ~VideoTexture() = default; // destroy() releases the GL objects, in the render thread
    VideoTexture(const VideoTexture &) = delete;
    VideoTexture &operator=(const VideoTexture &) = delete;

    // Each frame: the decoder gets as many upload buffers as it can use, of the size its frames need
    void feed(VideoDecoder &dec);
    // A frame fetched from the decoder becomes the picture. Its upload buffer goes back to the decoder.
    void upload(VideoFrame &f, VideoDecoder &dec, const VideoConverter &conv);

    GLuint texture() const { return m_out.tex; }
    int width() const { return m_out.w; }
    int height() const { return m_out.h; }
    uint64_t uploads() const { return m_uploads; }             // frames shown so far
    uint64_t stagedUploads() const { return m_staged; }        // of which through an upload buffer

    // Releases the textures and the upload buffers (render thread; the decoder must be closed already)
    void destroy();

private:
    struct Pbo {
        GLuint buffer = 0;
        size_t size = 0;
        bool mapped = false;
    };
    void createPbo(VideoDecoder &dec);
    void destroyPbo(int id);
    void ensurePlanes(const VideoLayout &L);
    void uploadPlane(int p, const VideoLayout::Plane &pl, const void *data, int rowPixels);
    void convert(const VideoLayout &L, const VideoConverter &conv);

    GLuint m_planes[4] = {0, 0, 0, 0};
    VideoLayout::Plane m_planeShape[4]; // what each texture holds
    bool m_allocated[4] = {false, false, false, false};
    RenderTarget m_out;
    std::vector<Pbo> m_pbos; // by id; buffer 0: a free slot
    size_t m_pboSize = 0;
    int m_live = 0;
    uint64_t m_uploads = 0, m_staged = 0;
    std::vector<uint8_t> m_repack; // rows of a plane the GPU cannot read in place
};
