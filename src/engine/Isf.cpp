#include "Isf.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <cmath>

// ---------------------------------------------------------------------------
// Text utilities
// ---------------------------------------------------------------------------

static bool readText(const QString &path, QString *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    *out = QString::fromUtf8(f.readAll());
    return true;
}

// Splits the JSON header (first /* */ comment) from the GLSL body.
static bool splitIsf(const QString &src, QString *json, QString *body)
{
    int start = src.indexOf(QLatin1String("/*"));
    if (start < 0) return false;
    int end = src.indexOf(QLatin1String("*/"), start + 2);
    if (end < 0) return false;
    *json = src.mid(start + 2, end - start - 2);
    if (body) *body = src.mid(end + 2);
    return true;
}

static QJsonObject parseLooseJson(QString json, QString *err)
{
    // Many ISF files contain trailing commas: strip them.
    static const QRegularExpression trailing(QStringLiteral(",(\\s*[}\\]])"));
    json.replace(trailing, QStringLiteral("\\1"));
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QStringLiteral("Invalid JSON header: ") + pe.errorString();
        return {};
    }
    return doc.object();
}

static bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

// Rewrites IMG_PIXEL / IMG_NORM_PIXEL / IMG_THIS_PIXEL / IMG_THIS_NORM_PIXEL / IMG_SIZE
// into standard GLSL calls. Parses parentheses to support nested arguments.
static QString rewriteImgFuncs(const QString &s)
{
    static const QStringList names = {QStringLiteral("IMG_THIS_NORM_PIXEL"), QStringLiteral("IMG_THIS_PIXEL"),
                                      QStringLiteral("IMG_NORM_PIXEL"), QStringLiteral("IMG_PIXEL"),
                                      QStringLiteral("IMG_SIZE")};
    QString out;
    out.reserve(s.size());
    int i = 0;
    while (i < s.size()) {
        int j = s.indexOf(QLatin1String("IMG_"), i);
        if (j < 0) {
            out += s.mid(i);
            break;
        }
        if (j > 0 && isIdentChar(s[j - 1])) {
            out += s.mid(i, j - i + 4);
            i = j + 4;
            continue;
        }
        QString matched;
        for (const QString &n : names) {
            if (s.mid(j, n.size()) == n) {
                int k = j + n.size();
                if (k < s.size() && isIdentChar(s[k])) continue;
                matched = n;
                break;
            }
        }
        if (matched.isEmpty()) {
            out += s.mid(i, j - i + 4);
            i = j + 4;
            continue;
        }
        int k = j + matched.size();
        while (k < s.size() && s[k].isSpace()) ++k;
        if (k >= s.size() || s[k] != QLatin1Char('(')) {
            out += s.mid(i, k - i);
            i = k;
            continue;
        }
        int depth = 0, p = k, argStart = k + 1;
        QStringList args;
        for (; p < s.size(); ++p) {
            QChar c = s[p];
            if (c == QLatin1Char('(')) {
                ++depth;
            } else if (c == QLatin1Char(')')) {
                if (--depth == 0) {
                    args << s.mid(argStart, p - argStart);
                    break;
                }
            } else if (c == QLatin1Char(',') && depth == 1) {
                args << s.mid(argStart, p - argStart);
                argStart = p + 1;
            }
        }
        if (p >= s.size()) {
            out += s.mid(i);
            break;
        }
        for (QString &a : args) a = rewriteImgFuncs(a).trimmed();
        out += s.mid(i, j - i);
        const QString img = args.value(0);
        if (matched == QLatin1String("IMG_SIZE"))
            out += QStringLiteral("_%1_imgSize").arg(img);
        else if (matched.startsWith(QLatin1String("IMG_THIS")))
            out += QStringLiteral("texture(%1, isf_FragNormCoord)").arg(img);
        else if (matched == QLatin1String("IMG_NORM_PIXEL"))
            out += QStringLiteral("texture(%1, %2)").arg(img, args.value(1));
        else
            out += QStringLiteral("texture(%1, (%2) / _%1_imgSize)").arg(img, args.value(1));
        i = p + 1;
    }
    return out;
}

static QString cleanBody(QString body)
{
    static const QRegularExpression version(QStringLiteral("^\\s*#version[^\\n]*$"),
                                            QRegularExpression::MultilineOption);
    static const QRegularExpression normDecl(
        QStringLiteral("(varying|in|out)\\s+(highp\\s+|mediump\\s+)?vec2\\s+(isf|vv)_FragNormCoord\\s*;"));
    body.replace(version, QString());
    body.replace(normDecl, QString());
    return rewriteImgFuncs(body);
}

// ---------------------------------------------------------------------------
// Small expression evaluator for pass WIDTH/HEIGHT ("$WIDTH/2.0", "floor($HEIGHT*0.25)").
// ---------------------------------------------------------------------------
namespace {
struct ExprParser {
    QString s;
    int p = 0;
    const QMap<QString, double> *vars = nullptr;
    bool ok = true;

    void ws() { while (p < s.size() && s[p].isSpace()) ++p; }
    bool eat(QChar c)
    {
        ws();
        if (p < s.size() && s[p] == c) { ++p; return true; }
        return false;
    }
    double expr()
    {
        double v = term();
        for (;;) {
            if (eat('+')) v += term();
            else if (eat('-')) v -= term();
            else return v;
        }
    }
    double term()
    {
        double v = unary();
        for (;;) {
            if (eat('*')) v *= unary();
            else if (eat('/')) { double d = unary(); v = d != 0 ? v / d : 0; }
            else return v;
        }
    }
    double unary()
    {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return primary();
    }
    double primary()
    {
        ws();
        if (eat('(')) { double v = expr(); if (!eat(')')) ok = false; return v; }
        if (p < s.size() && (s[p].isDigit() || s[p] == '.')) {
            int st = p;
            while (p < s.size() && (s[p].isDigit() || s[p] == '.' || s[p] == 'e' || s[p] == 'E')) ++p;
            return s.mid(st, p - st).toDouble(&ok);
        }
        bool dollar = eat('$');
        int st = p;
        while (p < s.size() && isIdentChar(s[p])) ++p;
        QString id = s.mid(st, p - st);
        if (id.isEmpty()) { ok = false; return 0; }
        if (dollar) {
            if (vars->contains(id)) return vars->value(id);
            ok = false;
            return 0;
        }
        // function
        if (!eat('(')) { ok = false; return 0; }
        QVector<double> args;
        if (!eat(')')) {
            do { args << expr(); } while (eat(','));
            if (!eat(')')) ok = false;
        }
        auto a = [&](int i) { return i < args.size() ? args[i] : 0.0; };
        const QString f = id.toLower();
        if (f == "floor") return std::floor(a(0));
        if (f == "ceil") return std::ceil(a(0));
        if (f == "round") return std::round(a(0));
        if (f == "abs") return std::fabs(a(0));
        if (f == "sqrt") return std::sqrt(a(0));
        if (f == "pow") return std::pow(a(0), a(1));
        if (f == "min") return std::min(a(0), a(1));
        if (f == "max") return std::max(a(0), a(1));
        ok = false;
        return 0;
    }
};
} // namespace

double IsfInstance::evalSize(const QString &e, int w, int h, int fallback) const
{
    if (e.isEmpty() || e.trimmed().isEmpty()) return fallback;
    QMap<QString, double> vars;
    vars["WIDTH"] = w;
    vars["HEIGHT"] = h;
    for (const IsfInput &in : m_inputs) {
        switch (in.type) {
        case IsfInput::Float: vars[in.name] = in.fValue; break;
        case IsfInput::Long: vars[in.name] = in.lValue; break;
        case IsfInput::Bool:
        case IsfInput::Event: vars[in.name] = in.bValue ? 1 : 0; break;
        default: break;
        }
    }
    ExprParser ep;
    ep.s = e;
    ep.vars = &vars;
    double v = ep.expr();
    if (!ep.ok || !std::isfinite(v)) return fallback;
    return v;
}

// ---------------------------------------------------------------------------

QString IsfInput::typeName(Type t)
{
    switch (t) {
    case Event: return "event";
    case Bool: return "bool";
    case Long: return "long";
    case Float: return "float";
    case Point2D: return "point2D";
    case Color: return "color";
    case Image: return "image";
    case Audio: return "audio";
    case AudioFFT: return "audioFFT";
    default: return "unknown";
    }
}

static IsfInput::Type typeFromString(const QString &s)
{
    const QString t = s.toLower();
    if (t == "event") return IsfInput::Event;
    if (t == "bool") return IsfInput::Bool;
    if (t == "long") return IsfInput::Long;
    if (t == "float") return IsfInput::Float;
    if (t == "point2d") return IsfInput::Point2D;
    if (t == "color") return IsfInput::Color;
    if (t == "image") return IsfInput::Image;
    if (t == "audio") return IsfInput::Audio;
    if (t == "audiofft") return IsfInput::AudioFFT;
    return IsfInput::Unknown;
}

static QPointF jsonPoint(const QJsonValue &v, QPointF def = {})
{
    QJsonArray a = v.toArray();
    if (a.size() >= 2) return QPointF(a[0].toDouble(), a[1].toDouble());
    return def;
}

IsfInstance::Header IsfInstance::readHeader(const QString &path)
{
    Header h;
    QString src, json;
    if (!readText(path, &src) || !splitIsf(src, &json, nullptr)) return h;
    QJsonObject o = parseLooseJson(json, nullptr);
    if (o.isEmpty()) return h;
    h.ok = true;
    h.name = QFileInfo(path).completeBaseName();
    h.description = o.value("DESCRIPTION").toString();
    for (const QJsonValue &c : o.value("CATEGORIES").toArray()) h.categories << c.toString();
    bool start = false, end = false;
    for (const QJsonValue &v : o.value("INPUTS").toArray()) {
        QJsonObject in = v.toObject();
        const QString n = in.value("NAME").toString();
        if (in.value("TYPE").toString().toLower() != "image") continue;
        if (n == "inputImage") h.isFilter = true;
        if (n == "startImage") start = true;
        if (n == "endImage") end = true;
    }
    h.isTransition = start && end;
    return h;
}

IsfInput *IsfInstance::input(const QString &name)
{
    for (IsfInput &in : m_inputs)
        if (in.name == name) return &in;
    return nullptr;
}

void IsfInstance::setImageTexture(const QString &name, GLuint tex, int w, int h)
{
    if (IsfInput *in = input(name)) {
        in->extTex = tex;
        in->extW = w;
        in->extH = h;
    }
}

void IsfInstance::readState(const QJsonObject &o)
{
    enabled = o.value("enable").toBool(true);
    speed = std::clamp(o.value("speed").toDouble(1.0), 0.0, 10.0);
    maskLayer = o.value("mask").toString().toULongLong();
    maskInvert = o.value("mask_invert").toBool(false);
    maskPreFx = o.value("mask_tap").toString() == "prefx";
}

IsfInstance::~IsfInstance()
{
    // The engine calls releaseGl() with its context current before destruction.
}

void IsfInstance::releaseGl()
{
    if (!QOpenGLContext::currentContext()) return;
    if (m_program) gl()->glDeleteProgram(m_program);
    m_program = 0;
    for (IsfTarget &t : m_targets) { t.rt[0].destroy(); t.rt[1].destroy(); }
    for (IsfImported &im : m_imported) im.tex.destroy();
    for (IsfInput &in : m_inputs) in.imageTex.destroy();
}

bool IsfInstance::parse(const QString &src, QString *body)
{
    QString json;
    if (!splitIsf(src, &json, body)) {
        m_error = QStringLiteral("No ISF header (missing /* */ JSON comment).");
        return false;
    }
    QJsonObject o = parseLooseJson(json, &m_error);
    if (o.isEmpty()) return false;

    m_description = o.value("DESCRIPTION").toString();
    m_credit = o.value("CREDIT").toString();

    for (const QJsonValue &v : o.value("INPUTS").toArray()) {
        QJsonObject j = v.toObject();
        IsfInput in;
        in.name = j.value("NAME").toString();
        if (in.name.isEmpty()) continue;
        in.label = j.value("LABEL").toString(in.name);
        in.type = typeFromString(j.value("TYPE").toString());
        switch (in.type) {
        case IsfInput::Float:
            in.fMin = j.value("MIN").toDouble(0.0);
            in.fMax = j.value("MAX").toDouble(1.0);
            if (in.fMax <= in.fMin) in.fMax = in.fMin + 1.0;
            in.fDefault = j.value("DEFAULT").toDouble(in.fMin);
            in.fValue = in.fDefault;
            break;
        case IsfInput::Bool:
        case IsfInput::Event: {
            QJsonValue d = j.value("DEFAULT");
            in.bDefault = d.isBool() ? d.toBool() : d.toDouble() != 0.0;
            in.bValue = in.type == IsfInput::Event ? false : in.bDefault;
            break;
        }
        case IsfInput::Long: {
            for (const QJsonValue &x : j.value("VALUES").toArray()) in.lValues << x.toInt();
            for (const QJsonValue &x : j.value("LABELS").toArray()) in.lLabels << x.toVariant().toString();
            if (in.lValues.isEmpty()) {
                int mn = j.value("MIN").toInt(0), mx = j.value("MAX").toInt(mn + 10);
                for (int k = mn; k <= mx && in.lValues.size() < 512; ++k) in.lValues << k;
            }
            while (in.lLabels.size() < in.lValues.size()) in.lLabels << QString::number(in.lValues[in.lLabels.size()]);
            in.lDefault = j.value("DEFAULT").toInt(in.lValues.value(0));
            in.lValue = in.lDefault;
            break;
        }
        case IsfInput::Point2D:
            in.pDefault = jsonPoint(j.value("DEFAULT"));
            in.pValue = in.pDefault;
            if (j.contains("MIN") && j.contains("MAX")) {
                in.hasPointRange = true;
                in.pMin = jsonPoint(j.value("MIN"));
                in.pMax = jsonPoint(j.value("MAX"), QPointF(1, 1));
            }
            break;
        case IsfInput::Color: {
            QJsonArray a = j.value("DEFAULT").toArray();
            for (int k = 0; k < 4; ++k) in.cDefault[k] = k < a.size() ? float(a[k].toDouble()) : (k == 3 ? 1.f : 0.f);
            std::copy(in.cDefault, in.cDefault + 4, in.cValue);
            break;
        }
        case IsfInput::Image:
            in.isInputImage = in.name == QLatin1String("inputImage");
            if (in.isInputImage) m_isFilter = true;
            break;
        default: break;
        }
        m_inputs.push_back(std::move(in));
    }

    for (const QJsonValue &v : o.value("PASSES").toArray()) {
        QJsonObject j = v.toObject();
        IsfPass p;
        p.target = j.value("TARGET").toString();
        p.persistent = j.value("PERSISTENT").toVariant().toBool();
        p.isFloat = j.value("FLOAT").toVariant().toBool();
        auto expr = [](const QJsonValue &x) { return x.isDouble() ? QString::number(x.toDouble()) : x.toString(); };
        p.widthExpr = expr(j.value("WIDTH"));
        p.heightExpr = expr(j.value("HEIGHT"));
        m_passes.push_back(p);
        if (!p.target.isEmpty()) {
            auto it = std::find_if(m_targets.begin(), m_targets.end(), [&](const IsfTarget &t) { return t.name == p.target; });
            if (it == m_targets.end()) {
                IsfTarget t;
                t.name = p.target;
                t.persistent = p.persistent;
                t.isFloat = p.isFloat;
                m_targets.push_back(std::move(t));
            } else {
                it->persistent |= p.persistent;
                it->isFloat |= p.isFloat;
            }
        }
    }
    // ISF v1: "PERSISTENT_BUFFERS"
    QJsonValue pb = o.value("PERSISTENT_BUFFERS");
    QStringList persistentNames;
    if (pb.isArray()) for (const QJsonValue &x : pb.toArray()) persistentNames << x.toString();
    else if (pb.isObject()) persistentNames = pb.toObject().keys();
    for (IsfTarget &t : m_targets) if (persistentNames.contains(t.name)) t.persistent = true;

    // Imported images: object {name: {PATH}} or array [{NAME, PATH}]
    QJsonValue imp = o.value("IMPORTED");
    const QDir dir = QFileInfo(m_path).absoluteDir();
    auto addImport = [&](const QString &n, const QString &p) {
        if (n.isEmpty() || p.isEmpty()) return;
        IsfImported im;
        im.name = n;
        im.path = dir.absoluteFilePath(p);
        m_imported.push_back(std::move(im));
    };
    if (imp.isObject()) {
        QJsonObject io = imp.toObject();
        for (auto it = io.begin(); it != io.end(); ++it) addImport(it.key(), it.value().toObject().value("PATH").toString());
    } else if (imp.isArray()) {
        for (const QJsonValue &x : imp.toArray())
            addImport(x.toObject().value("NAME").toString(), x.toObject().value("PATH").toString());
    }
    return true;
}

QString IsfInstance::uniformBlock() const
{
    QString u;
    u += "uniform int PASSINDEX;\nuniform vec2 RENDERSIZE;\nuniform float TIME;\nuniform float TIMEDELTA;\n"
         "uniform vec4 DATE;\nuniform int FRAMEINDEX;\n";
    auto sampler = [&](const QString &n) {
        // _imgRect / _flip: VVISF internal variables that some shaders use directly
        u += QStringLiteral("uniform sampler2D %1;\nuniform vec2 _%1_imgSize;\nuniform vec4 _%1_imgRect;\n"
                            "uniform bool _%1_flip;\n").arg(n);
    };
    for (const IsfInput &in : m_inputs) {
        switch (in.type) {
        case IsfInput::Event:
        case IsfInput::Bool: u += QStringLiteral("uniform bool %1;\n").arg(in.name); break;
        case IsfInput::Long: u += QStringLiteral("uniform int %1;\n").arg(in.name); break;
        case IsfInput::Float: u += QStringLiteral("uniform float %1;\n").arg(in.name); break;
        case IsfInput::Point2D: u += QStringLiteral("uniform vec2 %1;\n").arg(in.name); break;
        case IsfInput::Color: u += QStringLiteral("uniform vec4 %1;\n").arg(in.name); break;
        case IsfInput::Image:
        case IsfInput::Audio:
        case IsfInput::AudioFFT: sampler(in.name); break;
        default: break;
        }
    }
    for (const IsfTarget &t : m_targets) sampler(t.name);
    for (const IsfImported &im : m_imported) sampler(im.name);
    return u;
}

bool IsfInstance::compile(const QString &fsBody, const QString &vsBody)
{
    const QString uniforms = uniformBlock();

    QString fs = QStringLiteral(
        "#version 330 core\n"
        "in vec2 isf_FragNormCoord;\n"
        "out vec4 isf_FragColor;\n"
        "#define gl_FragColor isf_FragColor\n"
        "#define texture2D texture\n"
        "#define texture2DRect texture\n"
        "#define varying in\n"
        "#define vv_FragNormCoord isf_FragNormCoord\n");
    fs += uniforms;
    fs += "#line 1\n";
    fs += fsBody;

    QString vs = QStringLiteral(
        "#version 330 core\n"
        "layout(location = 0) in vec2 isf_position;\n"
        "out vec2 isf_FragNormCoord;\n");
    vs += uniforms;
    vs += QStringLiteral(
        "void isf_vertShaderInit() {\n"
        "    gl_Position = vec4(isf_position, 0.0, 1.0);\n"
        "    isf_FragNormCoord = isf_position * 0.5 + 0.5;\n"
        "}\n"
        "#define vv_vertShaderInit isf_vertShaderInit\n"
        "#define vv_FragNormCoord isf_FragNormCoord\n"
        "#define varying out\n"
        "#define attribute in\n"
        "#define texture2D texture\n");
    if (vsBody.isEmpty()) {
        vs += "void main() { isf_vertShaderInit(); }\n";
    } else {
        vs += "#line 1\n";
        vs += vsBody;
    }

    QString log;
    m_program = compileProgram(vs, fs, &log);
    if (!m_program) {
        m_error = QStringLiteral("Compilation error:\n") + log;
        return false;
    }
    cacheLocations();
    return true;
}

void IsfInstance::cacheLocations()
{
    auto f = gl();
    auto loc = [&](const QString &n) { return f->glGetUniformLocation(m_program, n.toUtf8().constData()); };
    m_locPassIndex = loc("PASSINDEX");
    m_locRenderSize = loc("RENDERSIZE");
    m_locTime = loc("TIME");
    m_locTimeDelta = loc("TIMEDELTA");
    m_locDate = loc("DATE");
    m_locFrameIndex = loc("FRAMEINDEX");
    for (IsfInput &in : m_inputs) {
        in.loc = loc(in.name);
        in.sizeLoc = loc("_" + in.name + "_imgSize");
        in.rectLoc = loc("_" + in.name + "_imgRect");
    }
    for (IsfTarget &t : m_targets) {
        t.loc = loc(t.name);
        t.sizeLoc = loc("_" + t.name + "_imgSize");
        t.rectLoc = loc("_" + t.name + "_imgRect");
    }
    for (IsfImported &im : m_imported) {
        im.loc = loc(im.name);
        im.sizeLoc = loc("_" + im.name + "_imgSize");
        im.rectLoc = loc("_" + im.name + "_imgRect");
    }
}

static bool loadImageTexture(const QString &path, Texture2D &tex, QString *err)
{
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Unreadable image: ") + path;
        return false;
    }
    // OpenGL convention: first row = bottom of the image.
    img = img.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
    tex.upload(img.constBits(), img.width(), img.height());
    return true;
}

bool IsfInstance::load(const QString &path)
{
    releaseGl();
    m_inputs.clear();
    m_passes.clear();
    m_targets.clear();
    m_imported.clear();
    m_error.clear();
    m_isFilter = false;
    m_time = 0;
    m_frameIndex = 0;
    m_path = QFileInfo(path).absoluteFilePath();
    m_name = QFileInfo(path).completeBaseName();

    QString src;
    if (!readText(m_path, &src)) {
        m_error = QStringLiteral("File not found: ") + path;
        return false;
    }
    QString body;
    if (!parse(src, &body)) return false;

    QString vsBody;
    QFileInfo fi(m_path);
    for (const QString &ext : {QStringLiteral("vs"), QStringLiteral("vert")}) {
        QString vsPath = fi.absolutePath() + "/" + fi.completeBaseName() + "." + ext;
        if (QFile::exists(vsPath) && readText(vsPath, &vsBody)) {
            vsBody = cleanBody(vsBody);
            break;
        }
    }
    if (!compile(cleanBody(body), vsBody)) return false;

    for (IsfImported &im : m_imported) {
        QString e;
        if (!loadImageTexture(im.path, im.tex, &e)) m_error += e + '\n';
    }
    return true;
}

bool IsfInstance::setImageInput(int index, const QString &path, QString *err)
{
    if (index < 0 || index >= int(m_inputs.size())) return false;
    IsfInput &in = m_inputs[index];
    if (path.isEmpty()) {
        in.imageTex.destroy();
        in.imagePath.clear();
        return true;
    }
    if (!loadImageTexture(path, in.imageTex, err)) return false;
    in.imagePath = QFileInfo(path).absoluteFilePath();
    return true;
}

void IsfInstance::render(const IsfRenderContext &rc, GLuint inputTex, int inW, int inH, RenderTarget &out, int outW,
                         int outH)
{
    auto f = gl();
    out.ensure(outW, outH);
    if (!m_program) {
        // Broken shader: pass the image through (filter) or render black (generator).
        if (inputTex) rc.blit(inputTex, out);
        else out.clear();
        return;
    }

    const double dt = rc.dt * speed;
    m_time += dt;
    f->glUseProgram(m_program);
    f->glDisable(GL_BLEND);
    if (m_locTime >= 0) f->glUniform1f(m_locTime, float(m_time));
    if (m_locTimeDelta >= 0) f->glUniform1f(m_locTimeDelta, float(dt));
    if (m_locFrameIndex >= 0) f->glUniform1i(m_locFrameIndex, m_frameIndex);
    if (m_locDate >= 0) {
        QDateTime now = QDateTime::currentDateTime();
        f->glUniform4f(m_locDate, now.date().year(), now.date().month(), now.date().day(),
                       now.time().msecsSinceStartOfDay() / 1000.0f);
    }

    int unit = 0;
    auto bindSampler = [&](GLint loc, GLint sizeLoc, GLint rectLoc, GLuint tex, int w, int h) {
        if (rectLoc >= 0) f->glUniform4f(rectLoc, 0.f, 0.f, tex ? float(w) : 1.f, tex ? float(h) : 1.f);
        if (loc < 0 && sizeLoc < 0) return;
        f->glActiveTexture(GL_TEXTURE0 + unit);
        f->glBindTexture(GL_TEXTURE_2D, tex ? tex : rc.blackTex);
        if (loc >= 0) f->glUniform1i(loc, unit);
        if (sizeLoc >= 0) f->glUniform2f(sizeLoc, tex ? float(w) : 1.f, tex ? float(h) : 1.f);
        ++unit;
    };

    for (IsfInput &in : m_inputs) {
        switch (in.type) {
        case IsfInput::Float: if (in.loc >= 0) f->glUniform1f(in.loc, float(in.fValue)); break;
        case IsfInput::Bool: if (in.loc >= 0) f->glUniform1i(in.loc, in.bValue ? 1 : 0); break;
        case IsfInput::Event: if (in.loc >= 0) f->glUniform1i(in.loc, in.eventFired ? 1 : 0); break;
        case IsfInput::Long: if (in.loc >= 0) f->glUniform1i(in.loc, in.lValue); break;
        case IsfInput::Point2D: if (in.loc >= 0) f->glUniform2f(in.loc, float(in.pValue.x()), float(in.pValue.y())); break;
        case IsfInput::Color: if (in.loc >= 0) f->glUniform4fv(in.loc, 1, in.cValue); break;
        case IsfInput::Image:
            if (in.isInputImage) bindSampler(in.loc, in.sizeLoc, in.rectLoc, inputTex, inW, inH);
            else if (in.extTex) bindSampler(in.loc, in.sizeLoc, in.rectLoc, in.extTex, in.extW, in.extH);
            else bindSampler(in.loc, in.sizeLoc, in.rectLoc, in.imageTex.tex, in.imageTex.w, in.imageTex.h);
            break;
        case IsfInput::Audio:
        case IsfInput::AudioFFT: bindSampler(in.loc, in.sizeLoc, in.rectLoc, 0, 1, 1); break;
        default: break;
        }
    }
    for (IsfImported &im : m_imported) bindSampler(im.loc, im.sizeLoc, im.rectLoc, im.tex.tex, im.tex.w, im.tex.h);
    const int targetUnitBase = unit;

    static const std::vector<IsfPass> kSinglePass{IsfPass{}};
    const std::vector<IsfPass> &passes = m_passes.empty() ? kSinglePass : m_passes; // no copy per frame

    for (size_t i = 0; i < passes.size(); ++i) {
        const IsfPass &pass = passes[i];
        const bool last = i + 1 == passes.size();
        int pw = int(std::lround(evalSize(pass.widthExpr, outW, outH, outW)));
        int ph = int(std::lround(evalSize(pass.heightExpr, outW, outH, outH)));
        pw = qBound(1, pw, 16384);
        ph = qBound(1, ph, 16384);

        IsfTarget *target = nullptr;
        if (!pass.target.isEmpty())
            for (IsfTarget &t : m_targets) if (t.name == pass.target) target = &t;
        if (target) {
            target->rt[0].ensure(pw, ph, target->isFloat);
            target->rt[1].ensure(pw, ph, target->isFloat);
        }

        // Already-rendered targets are readable ("front" buffer).
        unit = targetUnitBase;
        for (IsfTarget &t : m_targets) {
            const RenderTarget &fr = t.rt[t.front];
            bindSampler(t.loc, t.sizeLoc, t.rectLoc, fr.tex, fr.w, fr.h);
        }

        if (m_locPassIndex >= 0) f->glUniform1i(m_locPassIndex, int(i));

        if (target) {
            // Double buffer: read "front", write "back", then swap.
            RenderTarget &back = target->rt[1 - target->front];
            back.bind();
            if (m_locRenderSize >= 0) f->glUniform2f(m_locRenderSize, pw, ph);
            rc.drawQuad();
            target->front = 1 - target->front;
            if (last) {
                rc.blit(target->rt[target->front].tex, out);
                f->glUseProgram(m_program);
            }
        } else {
            // Pass without target: renders directly into the output (output size).
            out.bind();
            if (m_locRenderSize >= 0) f->glUniform2f(m_locRenderSize, out.w, out.h);
            rc.drawQuad();
        }
    }

    for (IsfInput &in : m_inputs) in.eventFired = false;
    ++m_frameIndex;
    f->glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// Parameter saving
// ---------------------------------------------------------------------------

QJsonObject IsfInstance::save(const QString &projectDir) const
{
    QJsonObject o;
    o["path"] = m_path;
    if (!projectDir.isEmpty()) o["relative_path"] = QDir(projectDir).relativeFilePath(m_path);
    o["enable"] = enabled;
    o["speed"] = speed;
    if (maskLayer) o["mask"] = QString::number(maskLayer); // ids are strings: JSON numbers are doubles
    if (maskInvert) o["mask_invert"] = true;
    if (maskLayer) o["mask_tap"] = maskPreFx ? "prefx" : "postfx";
    QJsonObject params;
    for (const IsfInput &in : m_inputs) {
        switch (in.type) {
        case IsfInput::Float: params[in.name] = in.fValue; break;
        case IsfInput::Bool: params[in.name] = in.bValue; break;
        case IsfInput::Long: params[in.name] = in.lValue; break;
        case IsfInput::Point2D: params[in.name] = QJsonArray{in.pValue.x(), in.pValue.y()}; break;
        case IsfInput::Color: params[in.name] = QJsonArray{in.cValue[0], in.cValue[1], in.cValue[2], in.cValue[3]}; break;
        case IsfInput::Image:
            if (!in.isInputImage && !in.imagePath.isEmpty()) params[in.name] = in.imagePath;
            break;
        default: break;
        }
    }
    o["params"] = params;
    return o;
}

void IsfInstance::restoreParams(const QJsonObject &params, const QString &projectDir)
{
    for (int idx = 0; idx < int(m_inputs.size()); ++idx) {
        IsfInput &in = m_inputs[idx];
        if (!params.contains(in.name)) continue;
        QJsonValue v = params.value(in.name);
        switch (in.type) {
        case IsfInput::Float: in.fValue = v.toDouble(in.fDefault); break;
        case IsfInput::Bool: in.bValue = v.toBool(in.bDefault); break;
        case IsfInput::Long: in.lValue = v.toInt(in.lDefault); break;
        case IsfInput::Point2D: in.pValue = jsonPoint(v, in.pDefault); break;
        case IsfInput::Color: {
            QJsonArray a = v.toArray();
            for (int k = 0; k < 4 && k < a.size(); ++k) in.cValue[k] = float(a[k].toDouble());
            break;
        }
        case IsfInput::Image: {
            QString p = v.toString();
            if (!QFile::exists(p) && !projectDir.isEmpty()) {
                QString alt = QDir(projectDir).absoluteFilePath(QFileInfo(p).fileName());
                if (QFile::exists(alt)) p = alt;
            }
            if (QFile::exists(p)) setImageInput(idx, p, nullptr);
            else if (!p.isEmpty()) in.imagePath = p; // kept (missing) for the media bin and saving
            break;
        }
        default: break;
        }
    }
}

void IsfInstance::resetParams()
{
    for (IsfInput &in : m_inputs) {
        in.fValue = in.fDefault;
        in.bValue = in.bDefault;
        in.lValue = in.lDefault;
        in.pValue = in.pDefault;
        std::copy(in.cDefault, in.cDefault + 4, in.cValue);
    }
}
