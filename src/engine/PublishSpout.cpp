// Spout publishing (Windows): shares the output texture with other applications.
// Compiled only on Windows (FULSKRIN_HAS_SPOUT), with the Spout2 SDK (BSD license).
// This file deliberately includes no Qt OpenGL header (Spout provides its own).

#include "Publish.h"

#include "SpoutSender.h"

class SpoutPublisher : public GpuPublisher
{
public:
    ~SpoutPublisher() override { m_sender.ReleaseSender(); }

    bool start(const QString &name, QString *) override
    {
        m_name = name.toUtf8();
        m_sender.SetSenderName(m_name.constData());
        return true;
    }

    void publish(unsigned int texture, int width, int height) override
    {
        // OpenGL texture (bottom-left origin): Spout flips it for DirectX receivers.
        m_sender.SendTexture(texture, GL_TEXTURE_2D, unsigned(width), unsigned(height), true, 0);
    }

private:
    SpoutSender m_sender;
    QByteArray m_name;
};

std::unique_ptr<GpuPublisher> createSpoutPublisher() { return std::make_unique<SpoutPublisher>(); }
