#pragma once
// Implementation of the ISF format (Interactive Shader Format, Vidvox v2 spec):
// JSON header parsing, translation to GLSL 330 core, multi-pass rendering,
// persistent and float buffers, imported images.

#include "Gl.h"
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <functional>
#include <vector>

// Value of a parameter (any numeric type), for undo.
struct IsfValue {
    double f = 0;
    bool b = false;
    int l = 0;
    QPointF p;
    float c[4] = {0, 0, 0, 0};
    bool operator==(const IsfValue &o) const
    {
        return f == o.f && b == o.b && l == o.l && p == o.p && c[0] == o.c[0] && c[1] == o.c[1] && c[2] == o.c[2]
               && c[3] == o.c[3];
    }
    bool operator!=(const IsfValue &o) const { return !(*this == o); }
};

struct IsfInput {
    enum Type { Event, Bool, Long, Float, Point2D, Color, Image, Audio, AudioFFT, Unknown };
    QString name, label;
    Type type = Unknown;
    bool isInputImage = false; // the input image of a filter

    double fMin = 0, fMax = 1, fDefault = 0, fValue = 0;
    bool bValue = false, bDefault = false;
    int lValue = 0, lDefault = 0;
    QVector<int> lValues;
    QStringList lLabels;
    QPointF pValue, pDefault, pMin, pMax;
    bool hasPointRange = false;
    float cValue[4] = {1, 1, 1, 1}, cDefault[4] = {1, 1, 1, 1};
    bool eventFired = false;

    QString imagePath; // image chosen by the user for a secondary image input
    Texture2D imageTex;

    GLint loc = -1, sizeLoc = -1, rectLoc = -1;
    static QString typeName(Type t);

    IsfValue value() const
    {
        IsfValue v;
        v.f = fValue;
        v.b = bValue;
        v.l = lValue;
        v.p = pValue;
        std::copy(cValue, cValue + 4, v.c);
        return v;
    }
    void setValue(const IsfValue &v)
    {
        fValue = v.f;
        bValue = v.b;
        lValue = v.l;
        pValue = v.p;
        std::copy(v.c, v.c + 4, cValue);
    }
};

struct IsfPass {
    QString target, widthExpr, heightExpr;
    bool persistent = false, isFloat = false;
};

struct IsfTarget {
    QString name;
    bool persistent = false, isFloat = false;
    RenderTarget rt[2];
    int front = 0;
    GLint loc = -1, sizeLoc = -1, rectLoc = -1;
};

struct IsfImported {
    QString name, path;
    Texture2D tex;
    GLint loc = -1, sizeLoc = -1, rectLoc = -1;
};

// Shared resources provided by the engine at render time.
struct IsfRenderContext {
    double dt = 0;
    GLuint blackTex = 0;
    std::function<void()> drawQuad;                                // fullscreen quad (VAO bound)
    std::function<void(GLuint tex, const RenderTarget &)> blit;   // plain copy
};

class IsfInstance
{
public:
    IsfInstance() = default;
    ~IsfInstance();
    IsfInstance(const IsfInstance &) = delete;
    IsfInstance &operator=(const IsfInstance &) = delete;

    // Parses the .fs file (and optional .vs), then compiles. The GL context must be current.
    bool load(const QString &path);
    void releaseGl();

    // Renders into `out` (size outW x outH). inputTex may be 0 (generator).
    void render(const IsfRenderContext &rc, GLuint inputTex, int inW, int inH, RenderTarget &out, int outW, int outH);

    bool setImageInput(int index, const QString &path, QString *err); // GL context current

    QJsonObject save(const QString &projectDir) const;
    void restoreParams(const QJsonObject &params, const QString &projectDir);
    void resetParams();

    QString path() const { return m_path; }
    QString name() const { return m_name; }
    QString description() const { return m_description; }
    QString credit() const { return m_credit; }
    QString error() const { return m_error; }
    bool isValid() const { return m_program != 0; }
    bool isFilter() const { return m_isFilter; }
    bool enabled = true;

    std::vector<IsfInput> &inputs() { return m_inputs; }
    const std::vector<IsfInput> &inputs() const { return m_inputs; }

    // Lightweight header parsing (for the library, no GL).
    struct Header {
        QString name, description;
        QStringList categories;
        bool isFilter = false, isTransition = false, ok = false;
    };
    static Header readHeader(const QString &path);

private:
    bool parse(const QString &src, QString *body);
    bool compile(const QString &fsBody, const QString &vsBody);
    QString uniformBlock() const;
    void cacheLocations();
    double evalSize(const QString &expr, int w, int h, int fallback) const;

    QString m_path, m_name, m_description, m_credit, m_error;
    bool m_isFilter = false;
    std::vector<IsfInput> m_inputs;
    std::vector<IsfPass> m_passes;
    std::vector<IsfTarget> m_targets;
    std::vector<IsfImported> m_imported;

    GLuint m_program = 0;
    GLint m_locPassIndex = -1, m_locRenderSize = -1, m_locTime = -1, m_locTimeDelta = -1, m_locDate = -1,
          m_locFrameIndex = -1;
    double m_time = 0;
    int m_frameIndex = 0;
};
