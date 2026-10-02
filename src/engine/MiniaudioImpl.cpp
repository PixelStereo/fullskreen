// miniaudio implementation (single translation unit). Only playback devices are used:
// decoding is done with FFmpeg, so miniaudio's decoders, encoders and audio engine are left out.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
