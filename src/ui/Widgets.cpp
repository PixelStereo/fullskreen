#include "Widgets.h"

#include <QCheckBox>

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSlider>
#include <QCoreApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMap>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// ResetLabel
// ---------------------------------------------------------------------------

ResetLabel::ResetLabel(const QString &text, std::function<void()> reset, QWidget *parent)
    : QLabel(text, parent), m_reset(std::move(reset))
{
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Click to reset \"%1\" to its default value").arg(text));
    setProperty("resetLabel", true);
}

void ResetLabel::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled() && m_reset) {
        m_reset();
        e->accept();
        return;
    }
    QLabel::mousePressEvent(e);
}

void ResetLabel::enterEvent(QEnterEvent *e)
{
    if (isEnabled()) setStyleSheet("color:#ffa028; text-decoration:underline;");
    QLabel::enterEvent(e);
}

void ResetLabel::leaveEvent(QEvent *e)
{
    setStyleSheet(QString());
    QLabel::leaveEvent(e);
}

// ---------------------------------------------------------------------------
// SeekBar
// ---------------------------------------------------------------------------

SeekBar::SeekBar(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("Click or drag to seek · drag the [ ] markers to move the in / out points"));
}

void SeekBar::setDuration(double d)
{
    if (d == m_duration) return;
    m_duration = d;
    update();
}

void SeekBar::setPosition(double t)
{
    if (m_drag == Head || std::abs(t - m_pos) < 1e-6) return;
    m_pos = t;
    update();
}

void SeekBar::setInOut(double in, double out)
{
    if (m_drag == In || m_drag == Out) return;
    if (in == m_in && out == m_out) return;
    m_in = in;
    m_out = out;
    update();
}

QRectF SeekBar::track() const { return QRectF(7, 11, width() - 14, 8); }

double SeekBar::xOf(double t) const
{
    const QRectF r = track();
    return m_duration > 0 ? r.left() + std::clamp(t / m_duration, 0.0, 1.0) * r.width() : r.left();
}

double SeekBar::tOf(double x) const
{
    const QRectF r = track();
    return m_duration > 0 ? std::clamp((x - r.left()) / r.width(), 0.0, 1.0) * m_duration : 0.0;
}

void SeekBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = track();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(28, 28, 31));
    p.drawRoundedRect(r, 3, 3);
    const double out = m_out < 0 ? m_duration : m_out;
    const double xi = xOf(m_in), xo = xOf(out);
    // Played range
    p.setBrush(QColor(255, 160, 40, isEnabled() ? 110 : 50));
    p.drawRect(QRectF(QPointF(xi, r.top()), QPointF(xo, r.bottom())));
    // Progress inside the range
    p.setBrush(QColor(255, 160, 40, isEnabled() ? 220 : 90));
    p.drawRect(QRectF(QPointF(xi, r.top()), QPointF(std::clamp(xOf(m_pos), xi, xo), r.bottom())));
    // In / out markers: brackets above and below the track
    const QColor marker(255, 200, 120);
    p.setPen(QPen(marker, 2));
    p.setBrush(Qt::NoBrush);
    auto bracket = [&](double x, int dir) {
        QPainterPath path;
        path.moveTo(x + 5 * dir, r.top() - 6);
        path.lineTo(x, r.top() - 6);
        path.lineTo(x, r.bottom() + 6);
        path.lineTo(x + 5 * dir, r.bottom() + 6);
        p.drawPath(path);
    };
    bracket(xi, 1);
    bracket(xo, -1);
    // Playhead
    const double xp = xOf(m_pos);
    p.setPen(QPen(Qt::white, 2));
    p.drawLine(QPointF(xp, r.top() - 4), QPointF(xp, r.bottom() + 4));
}

void SeekBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || m_duration <= 0) return;
    const double x = e->position().x();
    const double out = m_out < 0 ? m_duration : m_out;
    const double di = std::abs(x - xOf(m_in)), dout = std::abs(x - xOf(out));
    const bool nearMarkerZone = e->position().y() < track().top() || e->position().y() > track().bottom();
    // A marker is grabbed above / below the track, or right on it when the playhead is not closer
    if (m_markersEditable && std::min(di, dout) < 6 && (nearMarkerZone || std::abs(x - xOf(m_pos)) > 4)) {
        m_drag = di <= dout ? In : Out;
    } else {
        m_drag = Head;
        m_pos = tOf(x);
        emit seekRequested(m_pos);
    }
    update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *e)
{
    const double x = e->position().x();
    if (m_drag == None) {
        const double out = m_out < 0 ? m_duration : m_out;
        const bool marker = m_markersEditable && std::min(std::abs(x - xOf(m_in)), std::abs(x - xOf(out))) < 6;
        setCursor(marker ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    const double t = tOf(x);
    if (m_drag == Head) {
        m_pos = t;
        emit seekRequested(t);
    } else if (m_drag == In) {
        m_in = std::min(t, (m_out < 0 ? m_duration : m_out) - 0.01);
        emit inOutEdited(true, std::max(0.0, m_in));
    } else {
        m_out = std::max(t, m_in + 0.01);
        emit inOutEdited(false, m_out >= m_duration - 1e-6 ? -1.0 : m_out);
    }
    update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = None;
    update();
}

// ---------------------------------------------------------------------------
// RoiEditor
// ---------------------------------------------------------------------------

RoiEditor::RoiEditor(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(QStringLiteral("Drag a side of the rectangle to keep only part of the picture; drag inside to move it"));
}

void RoiEditor::setImage(const QImage &img)
{
    const bool resize = img.isNull() != m_image.isNull() ||
                        (!img.isNull() && std::abs(double(img.width()) / img.height() - m_aspect) > 1e-3);
    m_image = img;
    if (!img.isNull()) m_aspect = double(img.width()) / std::max(1, img.height());
    if (resize) updateGeometry();
    update();
}

void RoiEditor::setAspect(double aspect)
{
    if (aspect <= 0 || !m_image.isNull()) return;
    m_aspect = aspect;
    updateGeometry();
    update();
}

void RoiEditor::setRoi(const QRectF &r)
{
    if (m_drag >= 0 || r == m_roi) return;
    m_roi = r;
    update();
}

int RoiEditor::heightForWidth(int w) const { return std::clamp(int(std::lround((w - 16) / m_aspect)) + 16, 80, 400); }

QRectF RoiEditor::picture() const
{
    const QRectF area = QRectF(rect()).adjusted(8, 8, -8, -8);
    double w = area.width(), h = w / m_aspect;
    if (h > area.height()) {
        h = area.height();
        w = h * m_aspect;
    }
    return QRectF(area.center().x() - w / 2, area.center().y() - h / 2, w, h);
}

void RoiEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(24, 24, 27));
    const QRectF pic = picture();
    if (m_image.isNull()) {
        p.fillRect(pic, QColor(45, 45, 50));
        p.setPen(QColor(130, 130, 135));
        p.drawText(pic, Qt::AlignCenter, QStringLiteral("no picture yet"));
    } else {
        p.drawImage(pic, m_image);
    }
    const QRectF c(pic.left() + m_roi.left() * pic.width(), pic.top() + m_roi.top() * pic.height(),
                   m_roi.width() * pic.width(), m_roi.height() * pic.height());
    // Unused part dimmed
    QPainterPath outside;
    outside.addRect(pic);
    outside.addRect(c);
    outside.setFillRule(Qt::OddEvenFill);
    p.fillPath(outside, QColor(0, 0, 0, 150));
    p.setPen(QPen(QColor(255, 160, 40), isEnabled() ? 2 : 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(c);
    if (!isEnabled()) return;
    // Side handles
    p.setBrush(QColor(255, 160, 40));
    p.setPen(Qt::NoPen);
    const QPointF mids[4] = {{c.left(), c.center().y()}, {c.center().x(), c.top()}, {c.right(), c.center().y()},
                             {c.center().x(), c.bottom()}};
    for (int k = 0; k < 4; ++k) {
        const QSizeF s = (k % 2 == 0) ? QSizeF(6, 18) : QSizeF(18, 6);
        p.drawRoundedRect(QRectF(mids[k].x() - s.width() / 2, mids[k].y() - s.height() / 2, s.width(), s.height()), 2, 2);
    }
}

int RoiEditor::hit(QPointF p) const
{
    const QRectF pic = picture();
    const QRectF c(pic.left() + m_roi.left() * pic.width(), pic.top() + m_roi.top() * pic.height(),
                   m_roi.width() * pic.width(), m_roi.height() * pic.height());
    const double tol = 7;
    const bool inY = p.y() > c.top() - tol && p.y() < c.bottom() + tol;
    const bool inX = p.x() > c.left() - tol && p.x() < c.right() + tol;
    double best = tol;
    int side = -1;
    const double d[4] = {std::abs(p.x() - c.left()), std::abs(p.y() - c.top()), std::abs(p.x() - c.right()),
                         std::abs(p.y() - c.bottom())};
    for (int k = 0; k < 4; ++k) {
        if ((k % 2 == 0 && !inY) || (k % 2 == 1 && !inX)) continue;
        if (d[k] < best) {
            best = d[k];
            side = k;
        }
    }
    if (side < 0 && c.contains(p)) side = 4;
    return side;
}

void RoiEditor::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    m_drag = hit(e->position());
    m_dragStart = m_roi;
    const QRectF pic = picture();
    m_pressNorm = QPointF((e->position().x() - pic.left()) / pic.width(), (e->position().y() - pic.top()) / pic.height());
}

void RoiEditor::mouseMoveEvent(QMouseEvent *e)
{
    const QRectF pic = picture();
    const QPointF n((e->position().x() - pic.left()) / pic.width(), (e->position().y() - pic.top()) / pic.height());
    if (m_drag < 0) {
        const int h = hit(e->position());
        setCursor(h == 0 || h == 2 ? Qt::SizeHorCursor : h == 1 || h == 3 ? Qt::SizeVerCursor : h == 4 ? Qt::SizeAllCursor
                                                                                                  : Qt::ArrowCursor);
        return;
    }
    const double minSize = 0.01;
    double l = m_dragStart.left(), t = m_dragStart.top(), r = m_dragStart.right(), b = m_dragStart.bottom();
    const double x = std::clamp(n.x(), 0.0, 1.0), y = std::clamp(n.y(), 0.0, 1.0);
    switch (m_drag) {
    case 0: l = std::min(x, r - minSize); break;
    case 1: t = std::min(y, b - minSize); break;
    case 2: r = std::max(x, l + minSize); break;
    case 3: b = std::max(y, t + minSize); break;
    case 4: {
        const double dx = std::clamp(n.x() - m_pressNorm.x(), -l, 1.0 - r);
        const double dy = std::clamp(n.y() - m_pressNorm.y(), -t, 1.0 - b);
        l += dx;
        r += dx;
        t += dy;
        b += dy;
        break;
    }
    default: break;
    }
    const QRectF nc(QPointF(l, t), QPointF(r, b));
    if (nc != m_roi) {
        m_roi = nc;
        update();
        emit roiEdited(m_roi);
    }
}

void RoiEditor::mouseReleaseEvent(QMouseEvent *) { m_drag = -1; }

// ---------------------------------------------------------------------------
// ColorEditor
// ---------------------------------------------------------------------------

ColorEditor::ColorEditor(const QString &title, const QColor &defaultColor, QWidget *parent)
    : QWidget(parent), m_color(defaultColor), m_default(defaultColor)
{
    m_color.getHslF(&m_h, &m_s, &m_l);
    m_h = std::max(0.0f, m_h);
    build(title);
    sync();
}

void ColorEditor::build(const QString &title)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto *head = new QHBoxLayout;
    m_switch = new QCheckBox;
    m_switch->setChecked(true);
    m_switch->hide();
    head->addWidget(m_switch);
    connect(m_switch, &QCheckBox::toggled, this, &ColorEditor::switchToggled);
    auto *name = new ResetLabel(QStringLiteral("<b>%1</b>").arg(title), [this] { apply(m_default, true); });
    name->setToolTip(QStringLiteral("Click to reset the color"));
    m_swatch = new QLabel;
    m_swatch->setFixedSize(46, 18);
    m_hex = new QLineEdit;
    m_hex->setMaximumWidth(80);
    m_hex->setToolTip(QStringLiteral("Hexadecimal value (#RRGGBB)"));
    head->addWidget(name);
    head->addStretch();
    head->addWidget(m_swatch);
    head->addWidget(m_hex);
    v->addLayout(head);
    connect(m_hex, &QLineEdit::editingFinished, this, [this] {
        const QColor c(m_hex->text().trimmed());
        if (c.isValid()) apply(c, true);
        else sync();
    });

    static const char *kGroups[] = {"RGB", "HSL", "Additive (light)", "Subtractive (filters)"};
    for (int g = 0; g < 4; ++g) {
        auto *box = new QWidget;
        auto *grid = new QGridLayout(box);
        grid->setContentsMargins(0, 2, 0, 2);
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(2);
        auto *cap = new QLabel(QString::fromUtf8(kGroups[g]));
        cap->setStyleSheet("color:#888; font-size:11px;");
        grid->addWidget(cap, 0, 0, 1, 3);
        m_groups[g] = box;
        v->addWidget(box);
    }
    const QString red = "stop:0 #000, stop:1 #f33", green = "stop:0 #000, stop:1 #3d3", blue = "stop:0 #000, stop:1 #47f";
    addChannel(Rgb, 0, "R", 255, QString(), red);
    addChannel(Rgb, 1, "G", 255, QString(), green);
    addChannel(Rgb, 2, "B", 255, QString(), blue);
    addChannel(Hsl, 0, QStringLiteral("Hue"), 360, QStringLiteral("°"),
               "stop:0 #f00, stop:0.17 #ff0, stop:0.33 #0f0, stop:0.5 #0ff, stop:0.67 #00f, stop:0.83 #f0f, stop:1 #f00");
    addChannel(Hsl, 1, QStringLiteral("Saturation"), 100, QStringLiteral(" %"), "stop:0 #777, stop:1 #f80");
    addChannel(Hsl, 2, QStringLiteral("Lightness"), 100, QStringLiteral(" %"), "stop:0 #000, stop:0.5 #888, stop:1 #fff");
    addChannel(Additive, 0, QStringLiteral("Red"), 100, QStringLiteral(" %"), red);
    addChannel(Additive, 1, QStringLiteral("Green"), 100, QStringLiteral(" %"), green);
    addChannel(Additive, 2, QStringLiteral("Blue"), 100, QStringLiteral(" %"), blue);
    addChannel(Subtractive, 0, QStringLiteral("Cyan"), 100, QStringLiteral(" %"), "stop:0 #fff, stop:1 #0cf");
    addChannel(Subtractive, 1, QStringLiteral("Magenta"), 100, QStringLiteral(" %"), "stop:0 #fff, stop:1 #f0d");
    addChannel(Subtractive, 2, QStringLiteral("Yellow"), 100, QStringLiteral(" %"), "stop:0 #fff, stop:1 #fe0");
}

static int groupIndex(int model) { return model == ColorEditor::Rgb ? 0 : model == ColorEditor::Hsl ? 1 : model == ColorEditor::Additive ? 2 : 3; }

void ColorEditor::addChannel(int model, int index, const QString &name, double max, const QString &suffix,
                             const QString &gradient)
{
    auto *grid = static_cast<QGridLayout *>(m_groups[groupIndex(model)]->layout());
    const int row = index + 1;
    Channel ch;
    ch.model = model;
    ch.index = index;
    ch.max = max;
    auto *label = new ResetLabel(name, [this, model, index] {
        // Default value of this channel: the one of the default color in this model
        const QColor keep = m_color;
        const float hk = m_h, sk = m_s, lk = m_l;
        m_color = m_default;
        m_default.getHslF(&m_h, &m_s, &m_l);
        m_h = std::max(0.0f, m_h);
        const double def = channelValue(model, index);
        m_color = keep;
        m_h = hk;
        m_s = sk;
        m_l = lk;
        setChannel(model, index, def);
    });
    label->setMinimumWidth(64);
    ch.slider = new QSlider(Qt::Horizontal);
    ch.slider->setRange(0, 1000);
    ch.slider->setStyleSheet(QStringLiteral("QSlider::groove:horizontal { height:6px; border-radius:3px;"
                                            " background:qlineargradient(x1:0,y1:0,x2:1,y2:0,%1); }"
                                            "QSlider::handle:horizontal { background:#eee; width:10px; margin:-5px 0;"
                                            " border-radius:5px; }")
                                 .arg(gradient));
    ch.spin = new QDoubleSpinBox;
    ch.spin->setRange(0, max);
    ch.spin->setDecimals(max >= 255 ? 0 : 1);
    ch.spin->setSuffix(suffix);
    ch.spin->setKeyboardTracking(false);
    ch.spin->setFixedWidth(76);
    grid->addWidget(label, row, 0);
    grid->addWidget(ch.slider, row, 1);
    grid->addWidget(ch.spin, row, 2);
    grid->setColumnStretch(1, 1);
    m_channels.push_back(ch);
    const size_t k = m_channels.size() - 1;
    connect(ch.slider, &QSlider::valueChanged, this, [this, k](int v) {
        if (m_syncing) return;
        const Channel &c = m_channels[k];
        setChannel(c.model, c.index, v / 1000.0 * c.max);
    });
    connect(ch.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, k](double v) {
        if (m_syncing) return;
        const Channel &c = m_channels[k];
        setChannel(c.model, c.index, v);
    });
}

double ColorEditor::channelValue(int model, int index) const
{
    const double rgb[3] = {m_color.redF(), m_color.greenF(), m_color.blueF()};
    switch (model) {
    case Rgb: return rgb[index] * 255.0;
    case Hsl: return index == 0 ? m_h * 360.0 : index == 1 ? m_s * 100.0 : m_l * 100.0;
    case Additive: return rgb[index] * 100.0;
    case Subtractive: return (1.0 - rgb[index]) * 100.0;
    }
    return 0;
}

void ColorEditor::setChannel(int model, int index, double v)
{
    double rgb[3] = {m_color.redF(), m_color.greenF(), m_color.blueF()};
    QColor c;
    switch (model) {
    case Rgb: rgb[index] = v / 255.0; break;
    case Additive: rgb[index] = v / 100.0; break;
    case Subtractive: rgb[index] = 1.0 - v / 100.0; break;
    case Hsl: {
        (index == 0 ? m_h : index == 1 ? m_s : m_l) = float(std::clamp(index == 0 ? v / 360.0 : v / 100.0, 0.0, 1.0));
        c = QColor::fromHslF(std::min(m_h, 0.9999f), m_s, m_l);
        break;
    }
    }
    if (model != Hsl) {
        for (double &x : rgb) x = std::clamp(x, 0.0, 1.0);
        c = QColor::fromRgbF(rgb[0], rgb[1], rgb[2]);
        float h, s, l;
        c.getHslF(&h, &s, &l);
        if (h >= 0 && s > 0) m_h = h; // gray: keep the hue
        if (l > 0 && l < 1) m_s = s;
        m_l = l;
    }
    m_color = c;
    sync();
    emit colorEdited(m_color);
}

void ColorEditor::apply(const QColor &c, bool emitSignal)
{
    m_color = c.toRgb();
    float h, s, l;
    m_color.getHslF(&h, &s, &l);
    if (h >= 0 && s > 0) m_h = h;
    m_s = s;
    m_l = l;
    sync();
    if (emitSignal) emit colorEdited(m_color);
}

void ColorEditor::setColor(const QColor &c)
{
    if (c.rgba64() == m_color.rgba64()) return;
    for (const Channel &ch : m_channels)
        if (ch.slider->isSliderDown() || ch.spin->hasFocus()) return; // being edited
    apply(c, false);
}

void ColorEditor::sync(const Channel *)
{
    m_syncing = true;
    for (Channel &ch : m_channels) {
        const double v = channelValue(ch.model, ch.index);
        if (!ch.slider->isSliderDown()) ch.slider->setValue(int(std::lround(v / ch.max * 1000)));
        if (!ch.spin->hasFocus()) ch.spin->setValue(v);
    }
    m_swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #555; border-radius:3px;").arg(m_color.name()));
    if (!m_hex->hasFocus()) m_hex->setText(m_color.name().toUpper());
    m_syncing = false;
}

void ColorEditor::setSwitch(bool shown, bool on, const QString &tip)
{
    m_switch->setVisible(shown);
    QSignalBlocker b(m_switch);
    m_switch->setChecked(on);
    if (!tip.isEmpty()) m_switch->setToolTip(tip);
}

void ColorEditor::setModels(int models)
{
    const int bits[4] = {Rgb, Hsl, Additive, Subtractive};
    for (int g = 0; g < 4; ++g) m_groups[g]->setVisible(models & bits[g]);
}

// ---------------------------------------------------------------------------

QIcon padlockIcon(bool locked, bool inherited)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = locked ? QColor(235, 70, 60) : inherited ? QColor(170, 90, 80) : QColor(110, 110, 116);
    // Dark outline: readable on the orange selection too
    for (int pass = 0; pass < 2; ++pass) {
        const QColor col = pass == 0 ? QColor(20, 20, 22, locked || inherited ? 200 : 0) : c;
        const double grow = pass == 0 ? 1.6 : 0.0;
        p.setPen(QPen(col, 3.2 + grow * 2, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        QPainterPath shackle;
        const double x0 = 10, x1 = 22;
        const bool closed = locked || inherited;
        shackle.moveTo(x0, 16);
        shackle.lineTo(x0, closed ? 11 : 9);
        shackle.arcTo(QRectF(x0, closed ? 5 : 3, x1 - x0, 12), 180, -180);
        shackle.lineTo(x1, closed ? 16 : 11);
        p.drawPath(shackle);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(QRectF(7 - grow, 15 - grow, 18 + 2 * grow, 13 + 2 * grow), 3, 3);
    }
    return QIcon(pm);
}

void lockInputs(QWidget *root, bool locked)
{
    if (!locked) return;
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *w : all) {
        if (w->property("allowLocked").toBool()) continue;
        bool parentAllowed = false;
        for (QWidget *p = w->parentWidget(); p && p != root; p = p->parentWidget()) parentAllowed |= p->property("allowLocked").toBool();
        if (parentAllowed) continue;
        if (qobject_cast<QAbstractButton *>(w) || qobject_cast<QAbstractSlider *>(w) || qobject_cast<QAbstractSpinBox *>(w) ||
            qobject_cast<QComboBox *>(w) || qobject_cast<QLineEdit *>(w) || qobject_cast<RoiEditor *>(w) ||
            qobject_cast<ResetLabel *>(w) || w->property("lockable").toBool())
            w->setEnabled(false);
    }
}

// ---------------------------------------------------------------------------
// SearchPicker
// ---------------------------------------------------------------------------

SearchPicker::SearchPicker(const QList<Item> &items, QWidget *parent) : QWidget(parent, Qt::Popup)
{
    setAttribute(Qt::WA_DeleteOnClose);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search…"));
    m_search->setClearButtonEnabled(true);
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(12);
    v->addWidget(m_search);
    v->addWidget(m_tree, 1);
    QMap<QString, QTreeWidgetItem *> groups;
    for (const Item &it : items) {
        QTreeWidgetItem *parentItem = nullptr;
        if (!it.group.isEmpty()) {
            parentItem = groups.value(it.group);
            if (!parentItem) {
                parentItem = new QTreeWidgetItem(m_tree, {it.group});
                parentItem->setFlags(Qt::ItemIsEnabled);
                QFont f = parentItem->font(0);
                f.setItalic(true);
                parentItem->setFont(0, f);
                groups.insert(it.group, parentItem);
            }
        }
        auto *leaf = parentItem ? new QTreeWidgetItem(parentItem, {it.text}) : new QTreeWidgetItem(m_tree, {it.text});
        leaf->setData(0, Qt::UserRole, it.data);
        leaf->setToolTip(0, it.tip);
    }
    m_tree->sortItems(0, Qt::AscendingOrder);
    if (items.isEmpty()) new QTreeWidgetItem(m_tree, {QStringLiteral("(nothing in the library)")});
    connect(m_search, &QLineEdit::textChanged, this, &SearchPicker::filter);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const QString d = it->data(0, Qt::UserRole).toString();
        if (d.isEmpty()) return;
        emit picked(d);
        close();
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        if (it->childCount()) it->setExpanded(!it->isExpanded());
        else emit m_tree->itemActivated(it, 0);
    });
    m_search->installEventFilter(this);
    resize(300, 380);
}

void SearchPicker::popup(const QPoint &globalPos)
{
    move(globalPos);
    show();
    m_search->setFocus();
}

void SearchPicker::filter(const QString &text)
{
    const QString f = text.trimmed();
    QTreeWidgetItem *first = nullptr;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *top = m_tree->topLevelItem(g);
        auto match = [&](QTreeWidgetItem *it) {
            return f.isEmpty() || it->text(0).contains(f, Qt::CaseInsensitive) || it->toolTip(0).contains(f, Qt::CaseInsensitive);
        };
        if (top->childCount() == 0) {
            top->setHidden(!match(top));
            if (!top->isHidden() && !first) first = top;
            continue;
        }
        bool any = false;
        for (int c = 0; c < top->childCount(); ++c) {
            QTreeWidgetItem *it = top->child(c);
            const bool m = match(it) || (!f.isEmpty() && top->text(0).contains(f, Qt::CaseInsensitive));
            it->setHidden(!m);
            any |= m;
            if (m && !first) first = it;
        }
        top->setHidden(!any);
        top->setExpanded(!f.isEmpty() && any);
    }
    if (first && !f.isEmpty()) m_tree->setCurrentItem(first);
}

bool SearchPicker::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) { // the list, while typing
            QCoreApplication::sendEvent(m_tree, e);
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (QTreeWidgetItem *it = m_tree->currentItem())
                if (!it->isHidden() && it->childCount() == 0) emit m_tree->itemActivated(it, 0);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}
