// Publication Spout (Windows) : partage de la texture de sortie avec d'autres applications.
// Compilé uniquement sur Windows (LANTERNE_HAS_SPOUT), avec le SDK Spout2 (licence BSD).
// Ce fichier n'inclut volontairement aucun en-tête OpenGL de Qt (Spout fournit les siens).

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
        // Texture OpenGL (origine en bas) : Spout la retourne pour les récepteurs DirectX.
        m_sender.SendTexture(texture, GL_TEXTURE_2D, unsigned(width), unsigned(height), true, 0);
    }

private:
    SpoutSender m_sender;
    QByteArray m_name;
};

std::unique_ptr<GpuPublisher> createSpoutPublisher() { return std::make_unique<SpoutPublisher>(); }
