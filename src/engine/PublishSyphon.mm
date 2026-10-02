// Syphon publishing (macOS): shares the output texture with other applications, with no CPU copy.
// Compiled only on macOS (FULSKRIN_HAS_SYPHON), with the Syphon framework (BSD license).

#include "Publish.h"

#import <Foundation/Foundation.h>
#import <Syphon/SyphonOpenGLServer.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

class SyphonPublisher : public GpuPublisher
{
public:
    ~SyphonPublisher() override
    {
        @autoreleasepool {
            [m_server stop];
            m_server = nil;
        }
    }

    bool start(const QString &name, QString *err) override
    {
        @autoreleasepool {
            CGLContextObj ctx = CGLGetCurrentContext();
            if (!ctx) {
                if (err) *err = QStringLiteral("No active OpenGL context for Syphon.");
                return false;
            }
            NSString *n = [NSString stringWithUTF8String:name.toUtf8().constData()];
            m_server = [[SyphonOpenGLServer alloc] initWithName:n context:ctx options:nil];
            if (!m_server) {
                if (err) *err = QStringLiteral("The Syphon server failed to start.");
                return false;
            }
        }
        return true;
    }

    void publish(unsigned int texture, int width, int height) override
    {
        if (!m_server) return;
        @autoreleasepool {
            // Standard OpenGL texture (bottom-left origin): no flip.
            [m_server publishFrameTexture:texture
                            textureTarget:GL_TEXTURE_2D
                              imageRegion:NSMakeRect(0, 0, width, height)
                        textureDimensions:NSMakeSize(width, height)
                                  flipped:NO];
            m_clients = m_server.hasClients ? 1 : 0;
        }
    }

    int receivers() const override { return m_clients; }

private:
    SyphonOpenGLServer *m_server = nil;
    int m_clients = 0;
};

std::unique_ptr<GpuPublisher> createSyphonPublisher() { return std::make_unique<SyphonPublisher>(); }
