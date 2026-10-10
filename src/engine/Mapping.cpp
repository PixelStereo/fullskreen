#include "Mapping.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

Homography Homography::squareToQuad(const QPointF q[4])
{
    // Heckbert, "Fundamentals of Texture Mapping": unit square -> quadrilateral.
    Homography H;
    const double x0 = q[0].x(), y0 = q[0].y(), x1 = q[1].x(), y1 = q[1].y();
    const double x2 = q[2].x(), y2 = q[2].y(), x3 = q[3].x(), y3 = q[3].y();
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    if (std::fabs(sx) < 1e-12 && std::fabs(sy) < 1e-12) {
        H.a = x1 - x0; H.b = x3 - x0; H.c = x0;
        H.d = y1 - y0; H.e = y3 - y0; H.f = y0;
        H.g = H.h = 0;
        return H;
    }
    const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    double den = dx1 * dy2 - dx2 * dy1;
    if (std::fabs(den) < 1e-12) den = den < 0 ? -1e-12 : 1e-12;
    H.g = (sx * dy2 - dx2 * sy) / den;
    H.h = (dx1 * sy - sx * dy1) / den;
    H.a = x1 - x0 + H.g * x1;
    H.b = x3 - x0 + H.h * x3;
    H.c = x0;
    H.d = y1 - y0 + H.g * y1;
    H.e = y3 - y0 + H.h * y3;
    H.f = y0;
    return H;
}

QPointF Homography::map(double u, double v) const
{
    double w = g * u + h * v + 1.0;
    if (std::fabs(w) < 1e-9) w = 1e-9;
    return QPointF((a * u + b * v + c) / w, (d * u + e * v + f) / w);
}

static constexpr double kPi = 3.14159265358979323846;
static const QPointF kUnit[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

void Mapping::resetMesh(int c, int r)
{
    cols = std::clamp(c, 2, 32);
    rows = std::clamp(r, 2, 32);
    offsets.assign(size_t(cols) * rows, QPointF(0, 0));
    ++revision;
}

void Mapping::resetCorners()
{
    position = {0.5, 0.5};
    size = {1, 1};
    rotation = 0;
    pivot = {0.5, 0.5};
    for (QPointF &p : pins) p = QPointF();
    ++revision;
}

QPointF Mapping::toComposition(QPointF q) const
{
    const double A = aspect > 0 ? aspect : 1.0, a = rotation * kPi / 180, c = std::cos(a), s = std::sin(a);
    // In units of the composition's height: its pixels, to turn it
    const double dx = (q.x() - 0.5) * size.width() * A, dy = (q.y() - 0.5) * size.height();
    const double px = (pivot.x() - 0.5) * size.width() * A, py = (pivot.y() - 0.5) * size.height();
    const double rx = px + c * (dx - px) - s * (dy - py), ry = py + s * (dx - px) + c * (dy - py);
    return position + QPointF(rx / A, ry);
}

QPointF Mapping::fromComposition(QPointF p) const
{
    const double A = aspect > 0 ? aspect : 1.0, a = rotation * kPi / 180, c = std::cos(a), s = std::sin(a);
    const double rx = (p.x() - position.x()) * A, ry = p.y() - position.y();
    const double px = (pivot.x() - 0.5) * size.width() * A, py = (pivot.y() - 0.5) * size.height();
    const double dx = px + c * (rx - px) + s * (ry - py), dy = py - s * (rx - px) + c * (ry - py);
    const double w = std::abs(size.width()) > 1e-12 ? size.width() * A : 1e-12, h = std::abs(size.height()) > 1e-12 ? size.height() : 1e-12;
    return QPointF(dx / w + 0.5, dy / h + 0.5);
}

Homography Mapping::local() const
{
    const QPointF q[4] = {kUnit[0] + pins[0], kUnit[1] + pins[1], kUnit[2] + pins[2], kUnit[3] + pins[3]};
    return Homography::squareToQuad(q);
}

QPointF Mapping::corner(int k) const { return toComposition(kUnit[k & 3] + pins[k & 3]); }

void Mapping::setCorner(int i, QPointF p)
{
    if (i < 0 || i > 3) return;
    pins[i] = fromComposition(p) - kUnit[i];
    ++revision;
}

void Mapping::translate(QPointF delta)
{
    position += delta;
    ++revision;
}

QRectF Mapping::bounds() const
{
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    auto add = [&](QPointF p) {
        x0 = std::min(x0, p.x());
        y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x());
        y1 = std::max(y1, p.y());
    };
    for (int k = 0; k < 4; ++k) add(corner(k));
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) add(controlPoint(i, j));
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

void Mapping::setBounds(const QRectF &to)
{
    const QRectF from = bounds();
    if (from.width() > 1e-9) size.setWidth(size.width() * to.width() / from.width());
    if (from.height() > 1e-9) size.setHeight(size.height() * to.height() / from.height());
    position += to.center() - bounds().center();
    ++revision;
}

static double catmull(double p0, double p1, double p2, double p3, double t)
{
    const double t2 = t * t, t3 = t2 * t;
    return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

QPointF Mapping::offsetAt(double u, double v) const
{
    const double gx = std::clamp(u, 0.0, 1.0) * (cols - 1);
    const double gy = std::clamp(v, 0.0, 1.0) * (rows - 1);
    int i = std::min(int(std::floor(gx)), cols - 2);
    int j = std::min(int(std::floor(gy)), rows - 2);
    const double tx = gx - i, ty = gy - j;
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, cols - 1);
        y = std::clamp(y, 0, rows - 1);
        return offsets[size_t(y) * cols + x];
    };
    QPointF rowv[4];
    for (int k = 0; k < 4; ++k) {
        const int y = j - 1 + k;
        QPointF p0 = at(i - 1, y), p1 = at(i, y), p2 = at(i + 1, y), p3 = at(i + 2, y);
        rowv[k] = QPointF(catmull(p0.x(), p1.x(), p2.x(), p3.x(), tx), catmull(p0.y(), p1.y(), p2.y(), p3.y(), tx));
    }
    return QPointF(catmull(rowv[0].x(), rowv[1].x(), rowv[2].x(), rowv[3].x(), ty),
                   catmull(rowv[0].y(), rowv[1].y(), rowv[2].y(), rowv[3].y(), ty));
}

QPointF Mapping::map(double u, double v) const { return toComposition(local().map(u, v) + offsetAt(u, v)); }

QPointF Mapping::unmap(QPointF p, QPointF guess) const
{
    QPointF uv = guess;
    for (int it = 0; it < 30; ++it) {
        const QPointF f = map(uv.x(), uv.y()) - p;
        if (std::hypot(f.x(), f.y()) < 1e-10) break;
        const double h = 1e-5;
        const QPointF du = (map(uv.x() + h, uv.y()) - map(uv.x() - h, uv.y())) / (2 * h);
        const QPointF dv = (map(uv.x(), uv.y() + h) - map(uv.x(), uv.y() - h)) / (2 * h);
        const double det = du.x() * dv.y() - dv.x() * du.y();
        if (std::fabs(det) < 1e-14) break;
        uv -= QPointF((dv.y() * f.x() - dv.x() * f.y()) / det, (-du.y() * f.x() + du.x() * f.y()) / det);
    }
    return uv;
}

bool Mapping::isIdentity() const
{
    auto near = [](QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) <= 1e-12; };
    if (!near(position, QPointF(0.5, 0.5)) || std::abs(size.width() - 1) > 1e-12 || std::abs(size.height() - 1) > 1e-12 ||
        std::abs(rotation) > 1e-12)
        return false;
    for (const QPointF &p : pins)
        if (!p.isNull()) return false;
    for (const QPointF &o : offsets)
        if (!o.isNull()) return false;
    return true;
}

QPointF Mapping::controlUV(int i, int j) const
{
    return QPointF(double(i) / (cols - 1), double(j) / (rows - 1));
}

QPointF Mapping::controlPoint(int i, int j) const
{
    const QPointF uv = controlUV(i, j);
    return toComposition(local().map(uv.x(), uv.y()) + offsets[size_t(j) * cols + i]);
}

void Mapping::setControlPoint(int i, int j, QPointF p)
{
    const QPointF uv = controlUV(i, j);
    offsets[size_t(j) * cols + i] = fromComposition(p) - local().map(uv.x(), uv.y());
    ++revision;
}

void Mapping::buildVertices(int n, std::vector<float> &out) const
{
    const Homography H = local();
    out.resize(size_t(n + 1) * (n + 1) * 4);
    size_t k = 0;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            const double u = double(x) / n, v = double(y) / n;
            const QPointF p = toComposition(H.map(u, v) + offsetAt(u, v));
            out[k++] = float(p.x() * 2.0 - 1.0);
            out[k++] = float(1.0 - p.y() * 2.0);
            out[k++] = float(u);
            out[k++] = float(1.0 - v); // textures in OpenGL convention
        }
    }
}

void Mapping::fitAspect(double srcAspect, double compAspect)
{
    if (srcAspect <= 0 || compAspect <= 0) return;
    double w = 1, h = 1;
    if (srcAspect > compAspect) h = compAspect / srcAspect;
    else w = srcAspect / compAspect;
    aspect = compAspect;
    size = QSizeF(w, h);
    position = QPointF(0.5, 0.5);
    ++revision;
}

QJsonObject SoftEdge::toJson() const
{
    QJsonObject o;
    o["enable"] = enabled;
    QJsonArray w, p;
    for (int i = 0; i < 4; ++i) {
        w.append(width[i]);
        p.append(power[i]);
    }
    o["width"] = w;
    o["power"] = p;
    return o;
}

void SoftEdge::fromJson(const QJsonObject &o)
{
    *this = SoftEdge();
    enabled = o.value("enable").toBool(false);
    const QJsonArray w = o.value("width").toArray(), p = o.value("power").toArray();
    for (int i = 0; i < 4; ++i) {
        if (i < w.size()) width[i] = std::clamp(w[i].toDouble(), 0.0, 0.5);
        if (i < p.size()) power[i] = std::clamp(p[i].toDouble(), 0.1, 8.0);
    }
}

bool SoftEdge::operator==(const SoftEdge &o) const
{
    if (enabled != o.enabled) return false;
    for (int i = 0; i < 4; ++i)
        if (width[i] != o.width[i] || power[i] != o.power[i]) return false;
    return true;
}

QJsonObject Mapping::toJson() const
{
    auto pt = [](QPointF p) { return QJsonArray{p.x(), p.y()}; };
    QJsonObject o;
    o["position"] = pt(position);
    o["size"] = QJsonArray{size.width(), size.height()};
    o["rotation"] = rotation;
    o["pivot"] = pt(pivot);
    QJsonArray pin;
    for (const QPointF &p : pins) pin.append(pt(p));
    o["pins"] = pin;
    o["cols"] = cols;
    o["rows"] = rows;
    QJsonArray off;
    for (const QPointF &p : offsets) off.append(pt(p));
    o["offsets"] = off;
    o["mesh_mode"] = meshMode;
    // Always save soft edge (even if disabled) so snapshots preserve crop feathering settings
    o["soft_edge"] = soft.toJson();
    return o;
}

void Mapping::fromJson(const QJsonObject &o)
{
    auto pt = [](const QJsonValue &v, QPointF def) {
        const QJsonArray a = v.toArray();
        return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : def;
    };
    position = pt(o.value("position"), QPointF(0.5, 0.5));
    const QPointF sz = pt(o.value("size"), QPointF(1, 1));
    size = QSizeF(sz.x(), sz.y());
    rotation = o.value("rotation").toDouble(0);
    pivot = pt(o.value("pivot"), QPointF(0.5, 0.5));
    const QJsonArray pin = o.value("pins").toArray();
    for (int i = 0; i < 4; ++i) pins[i] = pt(pin.at(i), QPointF());
    resetMesh(o.value("cols").toInt(4), o.value("rows").toInt(4));
    const QJsonArray off = o.value("offsets").toArray();
    for (int i = 0; i < int(offsets.size()) && i < off.size(); ++i) offsets[size_t(i)] = pt(off[i], QPointF());
    meshMode = o.value("mesh_mode").toBool(false);
    soft.fromJson(o.value("soft_edge").toObject());
    ++revision;
}

void Mapping::rotate(double degrees, QSize)
{
    if (std::abs(degrees) < 1e-12) return;
    double r = std::fmod(rotation + degrees, 360.0);
    if (r <= -180) r += 360;
    if (r > 180) r -= 360;
    rotation = r;
    ++revision;
}

Mapping::Rect Mapping::rect(QSize comp) const
{
    const double W = std::max(1, comp.width()), H = std::max(1, comp.height());
    Rect r;
    const QPointF c = toComposition(QPointF(0.5, 0.5));
    r.center = QPointF(c.x() * W, c.y() * H);
    r.w = size.width() * W;
    r.h = size.height() * H;
    r.angle = rotation;
    return r;
}

void Mapping::setRect(const Rect &r, QSize comp)
{
    const double W = std::max(1, comp.width()), H = std::max(1, comp.height());
    size = QSizeF(r.w / W, r.h / H);
    rotation = r.angle;
    for (QPointF &p : pins) p = QPointF();
    for (QPointF &o : offsets) o = QPointF();
    position += QPointF(r.center.x() / W, r.center.y() / H) - toComposition(QPointF(0.5, 0.5));
    ++revision;
}

// The pivot is fixed to the picture
QPointF Mapping::pivotPoint() const { return toComposition(pivot); }

void Mapping::setPivotPoint(QPointF p)
{
    const QPointF before = corner(0);
    pivot = fromComposition(p);
    position += before - corner(0); // the picture stays where it is
    ++revision;
}
