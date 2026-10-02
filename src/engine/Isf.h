#pragma once
// Implémentation du format ISF (Interactive Shader Format, spec Vidvox v2) :
// parsing de l'en-tête JSON, traduction GLSL 330 core, rendu multi-passes,
// buffers persistants et flottants, images importées.

#include "Gl.h"
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <functional>
#include <vector>

// Valeur d'un paramètre (tous types numériques confondus), pour l'annulation.
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
    bool isInputImage = false; // l'image d'entrée d'un filtre

    double fMin = 0, fMax = 1, fDefault = 0, fValue = 0;
    bool bValue = false, bDefault = false;
    int lValue = 0, lDefault = 0;
    QVector<int> lValues;
    QStringList lLabels;
    QPointF pValue, pDefault, pMin, pMax;
    bool hasPointRange = false;
    float cValue[4] = {1, 1, 1, 1}, cDefault[4] = {1, 1, 1, 1};
    bool eventFired = false;

    QString imagePath; // image choisie par l'utilisateur pour une entrée image secondaire
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

// Ressources partagées fournies par le moteur au moment du rendu.
struct IsfRenderContext {
    double dt = 0;
    GLuint blackTex = 0;
    std::function<void()> drawQuad;                                // quad plein écran (VAO lié)
    std::function<void(GLuint tex, const RenderTarget &)> blit;   // copie simple
};

class IsfInstance
{
public:
    IsfInstance() = default;
    ~IsfInstance();
    IsfInstance(const IsfInstance &) = delete;
    IsfInstance &operator=(const IsfInstance &) = delete;

    // Analyse le fichier .fs (et .vs éventuel) puis compile. Le contexte GL doit être courant.
    bool load(const QString &path);
    void releaseGl();

    // Rend dans `out` (taille outW x outH). inputTex peut être 0 (générateur).
    void render(const IsfRenderContext &rc, GLuint inputTex, int inW, int inH, RenderTarget &out, int outW, int outH);

    bool setImageInput(int index, const QString &path, QString *err); // contexte GL courant

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

    // Analyse légère de l'en-tête (pour la bibliothèque, sans GL).
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
