#include "Widgets.h"
#include <QSpinBox>
#include <QPointer>

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
#include <QRegularExpression>
#include <QPixmap>
#include <QWheelEvent>
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
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
// Accent color
// ---------------------------------------------------------------------------

namespace theme {
namespace {
QColor g_accent;
bool g_loaded = false;
} // namespace

QColor defaultAccent() { return QColor(214, 214, 219); }

QColor accent()
{
    if (!g_loaded) {
        const QString v = QSettings().value(QStringLiteral("ui/accent")).toString();
        const QColor c(v);
        g_accent = c.isValid() ? c : defaultAccent();
        g_loaded = true;
    }
    return g_accent;
}

void setAccent(const QColor &c)
{
    g_accent = c.isValid() ? c : defaultAccent();
    g_loaded = true;
    QSettings().setValue(QStringLiteral("ui/accent"), g_accent.name());
    emit notifier()->changed();
}

// Black or white, whichever stands out on the accent (sRGB luminance)
QColor onAccent()
{
    const QColor a = accent();
    const double l = 0.2126 * a.redF() + 0.7152 * a.greenF() + 0.0722 * a.blueF();
    return l > 0.55 ? QColor(26, 26, 29) : QColor(240, 240, 243);
}

QString css(int alpha)
{
    const QColor a = accent();
    if (alpha >= 255) return a.name();
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(a.red()).arg(a.green()).arg(a.blue()).arg(alpha);
}

Notifier *notifier()
{
    static Notifier n;
    return &n;
}

} // namespace theme

namespace magnet {
static bool g_enabled = true, g_enabledLoaded = false;
static int g_distance = -1;

Notifier *notifier()
{
    static Notifier n;
    return &n;
}
bool enabledAtStart() { return QSettings().value(QStringLiteral("ui/magnetism"), true).toBool(); }
void setEnabledAtStart(bool on) { QSettings().setValue(QStringLiteral("ui/magnetism"), on); }
bool enabled()
{
    if (!g_enabledLoaded) {
        g_enabled = enabledAtStart();
        g_enabledLoaded = true;
    }
    return g_enabled;
}
void setEnabled(bool on)
{
    g_enabledLoaded = true;
    if (g_enabled == on) return;
    g_enabled = on;
    emit notifier()->changed();
}
int distance()
{
    if (g_distance < 0) g_distance = std::clamp(QSettings().value(QStringLiteral("ui/magnetDistance"), 8).toInt(), 2, 40);
    return g_distance;
}
void setDistance(int px)
{
    g_distance = std::clamp(px, 2, 40);
    QSettings().setValue(QStringLiteral("ui/magnetDistance"), g_distance);
    emit notifier()->changed();
}
QIcon icon()
{
    QIcon icon;
    for (int on = 0; on < 2; ++on) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        // A horseshoe magnet: red when on, grey when off, its poles silver
        const QColor body = on ? QColor(225, 62, 56) : QColor(118, 118, 124), poles(215, 215, 220);
        QPainterPath shoe;
        shoe.moveTo(8, 9);
        shoe.lineTo(8, 16);
        shoe.arcTo(QRectF(8, 8.5, 16, 17), 180, 180);
        shoe.lineTo(24, 9);
        p.setPen(QPen(body, 6.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.drawPath(shoe);
        p.setPen(QPen(poles, 6.5, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(8, 3), QPointF(8, 9));
        p.drawLine(QPointF(24, 3), QPointF(24, 9));
        icon.addPixmap(pm, QIcon::Normal, on ? QIcon::On : QIcon::Off);
    }
    return icon;
}
} // namespace magnet

namespace theme {
// Dark interface, with the accent wherever a selection or an active control is shown
void applyToApplication(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const QColor base(30, 30, 33), window(40, 40, 44), text(225, 225, 228), a = accent();
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(52, 52, 57));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, a);
    p.setColor(QPalette::HighlightedText, onAccent());
    p.setColor(QPalette::Link, a);
    p.setColor(QPalette::PlaceholderText, QColor(130, 130, 135));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 125));
    app.setPalette(p);
    app.setStyleSheet(QStringLiteral("QGroupBox { font-weight: bold; border: 1px solid #4a4a50; border-radius: 4px; "
                                     "margin-top: 10px; padding-top: 8px; } "
                                     "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; "
                                     "color: %1; }")
                          .arg(css()));
}
} // namespace theme

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
    if (isEnabled()) setStyleSheet(QStringLiteral("color:%1; text-decoration:underline;").arg(theme::css()));
    QLabel::enterEvent(e);
}

void ResetLabel::leaveEvent(QEvent *e)
{
    setStyleSheet(QString());
    QLabel::leaveEvent(e);
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
    p.setPen(QPen(theme::accent(), isEnabled() ? 2 : 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(c);
    if (!isEnabled()) return;
    // Side handles
    p.setBrush(theme::accent());
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
    m_switch = new FlagBox;
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

// "stop:0 #3a7bff, stop:0.5 #888, stop:1 #ffd23a" — the gradients were written in stylesheet form
static QGradientStops parseStops(const QString &text)
{
    QGradientStops out;
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList f = part.trimmed().split(QRegularExpression(QStringLiteral("[: ]")), Qt::SkipEmptyParts);
        if (f.size() >= 3) out.append({f[1].toDouble(), QColor(f[2])});
    }
    return out;
}

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
    ch.bar = new SliderField;
    ch.bar->setRange(0, max);
    ch.bar->setDecimals(max >= 255 ? 0 : 1);
    ch.bar->setSuffix(suffix);
    ch.bar->setSingleStep(max / 100);
    ch.bar->setGradient(parseStops(gradient));
    grid->addWidget(label, row, 0);
    grid->addWidget(ch.bar, row, 1);
    grid->setColumnStretch(1, 1);
    m_channels.push_back(ch);
    const size_t k = m_channels.size() - 1;
    connect(ch.bar, &SliderField::valueEdited, this, [this, k](double v) {
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
        if (ch.bar->isDragging()) return; // being edited
    apply(c, false);
}

void ColorEditor::sync(const Channel *)
{
    m_syncing = true;
    for (Channel &ch : m_channels) {
        const double v = channelValue(ch.model, ch.index);
        if (!ch.bar->isDragging()) ch.bar->setValue(v);
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
    // Dark outline: readable on the selection too
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

namespace {
// Slides the value of a number field, on its text field
class NumberDrag : public QObject
{
public:
    NumberDrag(QAbstractSpinBox *spin, QLineEdit *edit) : QObject(spin), m_spin(spin), m_edit(edit)
    {
        edit->installEventFilter(this);
        spin->installEventFilter(this); // its focus (the text field's is given by it, past the filters)
        edit->setCursor(Qt::SizeHorCursor); // an I-beam only once editing
        // A press must not give the focus by itself (Qt does it before any filter sees the press): the release
        // decides, a click edits the text, a drag does not. Tab still reaches the field.
        spin->setFocusPolicy(Qt::TabFocus);
        edit->setFocusPolicy(Qt::TabFocus);
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == m_spin) {
            if (e->type() == QEvent::FocusIn) m_edit->setCursor(Qt::IBeamCursor);
            else if (e->type() == QEvent::FocusOut) m_edit->setCursor(Qt::SizeHorCursor);
            return false;
        }
        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: break;
        default: return false;
        }
        auto *me = static_cast<QMouseEvent *>(e);
        // Editing the text: the mouse places the cursor and selects, as usual
        if (!m_pressed && (m_spin->hasFocus() || !m_spin->isEnabled() || m_spin->isReadOnly())) return false;
        const QPointF pos = me->globalPosition();
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            if (me->button() != Qt::LeftButton) return false;
            m_pressed = true;
            m_dragging = false;
            m_start = pos;
            m_last = pos.x();
            m_carry = 0;
            return true; // neither focus nor text cursor yet
        }
        if (!m_pressed) return false;
        if (e->type() == QEvent::MouseMove) {
            if (!m_dragging) {
                if (std::abs(pos.x() - m_start.x()) < 3) return true;
                m_dragging = true;
                m_last = pos.x();
            }
            slide(pos.x() - m_last, me->modifiers());
            m_last = pos.x();
            return true;
        }
        // Release: a click without moving edits the text, all of it selected; a drag leaves the field as it was
        m_pressed = false;
        if (!m_dragging) {
            m_spin->setFocus(Qt::MouseFocusReason);
            m_spin->selectAll();
        }
        m_dragging = false;
        return true;
    }

private:
    void slide(double dx, Qt::KeyboardModifiers mods)
    {
        double factor = 1.0;
        if (mods & Qt::ShiftModifier) factor = 0.1;
        if (mods & Qt::ControlModifier) factor = 10.0;
        if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin)) {
            m_carry += dx * d->singleStep() * factor;
            const double unit = std::pow(10.0, -d->decimals());
            const double move = std::trunc(m_carry / unit) * unit; // whole units of what is shown
            if (move != 0) {
                m_carry -= move;
                d->setValue(d->value() + move);
            }
        } else if (auto *i = qobject_cast<QSpinBox *>(m_spin)) {
            m_carry += dx * i->singleStep() * factor;
            const int move = int(std::trunc(m_carry));
            if (move != 0) {
                m_carry -= move;
                i->setValue(i->value() + move);
            }
        }
    }

    QAbstractSpinBox *m_spin;
    QLineEdit *m_edit;
    QPointF m_start;
    double m_last = 0, m_carry = 0;
    bool m_pressed = false, m_dragging = false;
};
} // namespace

NumberBox::NumberBox(QWidget *parent) : QDoubleSpinBox(parent) { new NumberDrag(this, lineEdit()); }
IntBox::IntBox(QWidget *parent) : QSpinBox(parent) { new NumberDrag(this, lineEdit()); }

namespace {
QColor flagEdge(bool hover, bool enabled)
{
    QColor c = hover ? QColor(165, 165, 172) : QColor(105, 105, 112);
    if (!enabled) c.setAlphaF(0.45);
    return c;
}
} // namespace

FlagBox::FlagBox(QWidget *parent) : FlagBox(QString(), parent) {}
FlagBox::FlagBox(const QString &text, QWidget *parent) : QCheckBox(text, parent)
{
    setCursor(Qt::PointingHandCursor);
    connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

QSize FlagBox::sizeHint() const
{
    const QFontMetrics fm(font());
    const int tw = text().isEmpty() ? 0 : fm.horizontalAdvance(text()) + 8;
    return QSize(16 + tw + 2, std::max(20, fm.height() + 4));
}

void FlagBox::enterEvent(QEnterEvent *e)
{
    m_hover = true;
    update();
    QCheckBox::enterEvent(e);
}

void FlagBox::leaveEvent(QEvent *e)
{
    m_hover = false;
    update();
    QCheckBox::leaveEvent(e);
}

void FlagBox::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked(), en = isEnabled();
    const QRectF box(1.5, (height() - 14) / 2.0, 14, 14);
    if (on) {
        QColor a = theme::accent();
        if (!en) a.setAlphaF(0.4);
        p.setPen(Qt::NoPen);
        p.setBrush(a);
        p.drawRoundedRect(box, 3, 3);
        QColor tick = theme::onAccent();
        if (!en) tick.setAlphaF(0.6);
        p.setPen(QPen(tick, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(box.left() + 3.2, box.center().y() + 0.3);
        path.lineTo(box.left() + 5.8, box.bottom() - 3.6);
        path.lineTo(box.right() - 3, box.top() + 3.6);
        p.drawPath(path);
    } else {
        p.setPen(QPen(flagEdge(m_hover, en), 1.2));
        p.setBrush(QColor(24, 24, 27));
        p.drawRoundedRect(box, 3, 3);
    }
    if (!text().isEmpty()) {
        p.setPen(palette().color(en ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
        p.setFont(font());
        p.drawText(QRectF(box.right() + 8, 0, width() - box.right() - 8, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

ToggleButton::ToggleButton(const QString &text, QWidget *parent) : QToolButton(parent)
{
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void ToggleButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked(), en = isEnabled();
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor fill = on ? theme::accent() : QColor(52, 52, 57);
    if (!on && underMouse()) fill = QColor(66, 66, 72);
    if (on && isDown()) fill = fill.darker(115);
    if (!en) fill.setAlphaF(0.45);
    p.setPen(on ? Qt::NoPen : QPen(QColor(90, 90, 97), 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 4, 4);
    QColor fg = on ? theme::onAccent() : QColor(205, 205, 210);
    if (!en) fg.setAlphaF(0.5);
    p.setPen(fg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
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

// ---------------------------------------------------------------------------
// SliderField, RangeField
// ---------------------------------------------------------------------------

namespace {
constexpr int kFieldHeight = 24;
const QColor kTrack(28, 28, 31);
QColor fillColor(bool on) { QColor c = theme::accent(); c.setAlpha(on ? 210 : 80); return c; }
const QColor kTick(255, 255, 255, 38), kBorder(0, 0, 0, 90);

// Flat field at the end of a bar: the value, typed, with its unit and no spin buttons.
QDoubleSpinBox *valueField(QWidget *parent, Qt::Alignment align = Qt::AlignRight)
{
    auto *s = new NumberBox(parent);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(align | Qt::AlignVCenter);
    s->setKeyboardTracking(false);
    s->setFrame(false);
    s->setStyleSheet("QDoubleSpinBox { background: #232326; border: none; padding: 0 5px; }");
    s->setFixedHeight(kFieldHeight - 2);
    return s;
}

int fieldWidth(const QDoubleSpinBox *s)
{
    const QString longest = QStringLiteral("-%1%2").arg(std::max(std::abs(s->minimum()), std::abs(s->maximum())),
                                                        0, 'f', s->decimals())
                            + s->suffix();
    return s->fontMetrics().horizontalAdvance(longest) + 16;
}
} // namespace

SliderField::SliderField(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(kFieldHeight);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeHorCursor);
    m_spin = valueField(this);
    m_spin->setRange(0, 1);
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (std::abs(v - m_value) < 1e-12) return;
        m_value = v;
        update();
        emit valueEdited(v);
        emit editingFinished(v);
    });
}

void SliderField::setRange(double min, double max)
{
    m_min = min;
    m_max = std::max(max, min + 1e-9);
    m_spin->setRange(min, max);
    m_origin = std::clamp(m_origin, m_min, m_max);
    resizeEvent(nullptr);
    update();
}
void SliderField::setTypedRange(double min, double max)
{
    m_spin->setRange(min, max);
    resizeEvent(nullptr);
}
void SliderField::setDecimals(int d) { m_spin->setDecimals(d); resizeEvent(nullptr); }
void SliderField::setSuffix(const QString &s) { m_spin->setSuffix(s); resizeEvent(nullptr); }
void SliderField::setSingleStep(double s) { m_spin->setSingleStep(s); }
void SliderField::setTicks(int n) { m_ticks = n; update(); }
void SliderField::setSnaps(const std::vector<double> &v) { m_snaps = v; }
void SliderField::setGradient(const QGradientStops &stops) { m_gradient = stops; update(); }
void SliderField::setOrigin(double v) { m_origin = std::clamp(v, m_min, m_max); update(); }

void SliderField::setValue(double v)
{
    v = std::clamp(v, m_spin->minimum(), m_spin->maximum());
    if (std::abs(v - m_value) < 1e-12) return;
    m_value = v;
    QSignalBlocker b(m_spin);
    m_spin->setValue(v);
    update();
}

void SliderField::resizeEvent(QResizeEvent *)
{
    const int w = fieldWidth(m_spin);
    m_spin->setGeometry(width() - w, 1, w, kFieldHeight - 2);
    update();
}

QRectF SliderField::bar() const { return QRectF(0, 1, width() - m_spin->width() - 4, kFieldHeight - 2); }
double SliderField::valueAt(double x) const
{
    const QRectF r = bar();
    return m_min + std::clamp((x - r.left()) / std::max(1.0, r.width()), 0.0, 1.0) * (m_max - m_min);
}

void SliderField::apply(double v, bool finished)
{
    v = std::clamp(v, m_spin->minimum(), m_spin->maximum());
    const double q = std::pow(10, m_spin->decimals());
    v = std::round(v * q) / q;
    if (std::abs(v - m_value) > 1e-12) {
        m_value = v;
        QSignalBlocker b(m_spin);
        m_spin->setValue(v);
        update();
        emit valueEdited(v);
    }
    if (finished) emit editingFinished(m_value);
}

void SliderField::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = bar();
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(r, 3, 3);
    auto xOf = [&](double v) {
        return r.left() + (std::clamp(v, m_min, m_max) - m_min) / (m_max - m_min) * r.width();
    };
    const double x0 = xOf(m_origin), xv = xOf(m_value);
    if (!m_gradient.isEmpty()) {
        // Color bar: the whole track shows the scale, and the value is a marker on it
        QLinearGradient grad(r.topLeft(), r.topRight());
        grad.setStops(m_gradient);
        p.setBrush(grad);
        p.setOpacity(isEnabled() ? 1.0 : 0.4);
        p.drawRoundedRect(r, 3, 3);
        p.setOpacity(1.0);
    } else {
        p.setBrush(fillColor(isEnabled()));
        p.drawRoundedRect(QRectF(QPointF(std::min(x0, xv), r.top()), QPointF(std::max(x0, xv), r.bottom())), 3, 3);
    }
    if (m_ticks > 1) {
        p.setPen(QPen(kTick, 1));
        for (int i = 1; i < m_ticks; ++i) {
            const double x = r.left() + r.width() * i / m_ticks;
            p.drawLine(QPointF(x, r.top() + 3), QPointF(x, r.bottom() - 3));
        }
    }
    if (!m_gradient.isEmpty()) {
        p.setPen(QPen(QColor(20, 20, 22), 3));
        p.drawLine(QPointF(xv, r.top() + 1), QPointF(xv, r.bottom() - 1));
        p.setPen(QPen(Qt::white, 1.4));
        p.drawLine(QPointF(xv, r.top() + 1), QPointF(xv, r.bottom() - 1));
    }
    p.setPen(QPen(kBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (hasFocus()) {
        p.setPen(QPen(theme::css(140).isEmpty() ? QColor() : [] { QColor c = theme::accent(); c.setAlpha(140); return c; }(), 1));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
}

void SliderField::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !isEnabled() || e->position().x() > bar().right()) return;
    m_drag = true;
    setFocus(Qt::MouseFocusReason);
    apply(valueAt(e->position().x()), false);
}
void SliderField::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_drag) return;
    const double v = valueAt(e->position().x());
    // Shift: ten times finer, from where the value is
    if (e->modifiers() & Qt::ShiftModifier) return apply(m_value + (v - m_value) * 0.1, false);
    // Otherwise the notable values catch the cursor (magnetism), so they can be hit on a wide range
    const double tol = (m_max - m_min) * 0.012;
    double best = v;
    if (magnet::enabled())
        for (double snap : m_snaps)
            if (snap >= m_min && snap <= m_max && std::abs(v - snap) < tol) best = snap;
    apply(best, false);
}
void SliderField::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_drag || e->button() != Qt::LeftButton) return;
    m_drag = false;
    emit editingFinished(m_value);
}
void SliderField::wheelEvent(QWheelEvent *e)
{
    if (!isEnabled()) return;
    const double step = m_spin->singleStep() * (e->modifiers() & Qt::ShiftModifier ? 0.1 : 1.0);
    apply(m_value + (e->angleDelta().y() > 0 ? step : -step), true);
    e->accept();
}

// --- RangeField

RangeField::RangeField(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(kFieldHeight);
    setCursor(Qt::SizeHorCursor);
    m_loSpin = valueField(this, Qt::AlignLeft);
    m_hiSpin = valueField(this);
    connect(m_loSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (std::abs(v - m_lo) < 1e-12) return;
        m_lo = std::min(v, m_hi);
        update();
        emit edited(true, m_lo);
        emit editingFinished();
    });
    connect(m_hiSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (std::abs(v - m_hi) < 1e-12) return;
        m_hi = std::max(v, m_lo);
        update();
        emit edited(false, m_hi);
        emit editingFinished();
    });
}

void RangeField::setRange(double min, double max)
{
    m_min = min;
    m_max = std::max(max, min + 1e-9);
    for (auto *s : {m_loSpin, m_hiSpin}) s->setRange(min, m_max);
    resizeEvent(nullptr);
    update();
}
void RangeField::setDecimals(int d) { for (auto *s : {m_loSpin, m_hiSpin}) s->setDecimals(d); resizeEvent(nullptr); }
void RangeField::setSuffix(const QString &s) { for (auto *b : {m_loSpin, m_hiSpin}) b->setSuffix(s); resizeEvent(nullptr); }

void RangeField::setValues(double lo, double hi)
{
    m_lo = std::clamp(lo, m_min, m_max);
    m_hi = std::clamp(std::max(hi, m_lo), m_min, m_max);
    QSignalBlocker a(m_loSpin), b(m_hiSpin);
    m_loSpin->setValue(m_lo);
    m_hiSpin->setValue(m_hi);
    update();
}

void RangeField::resizeEvent(QResizeEvent *)
{
    const int wl = fieldWidth(m_loSpin), wh = fieldWidth(m_hiSpin);
    m_loSpin->setGeometry(0, 1, wl, kFieldHeight - 2);
    m_hiSpin->setGeometry(width() - wh, 1, wh, kFieldHeight - 2);
    update();
}

QRectF RangeField::bar() const
{
    return QRectF(QPointF(m_loSpin->width() + 4, 1), QPointF(width() - m_hiSpin->width() - 4, kFieldHeight - 1));
}
double RangeField::xOf(double v) const
{
    const QRectF r = bar();
    return r.left() + (v - m_min) / (m_max - m_min) * r.width();
}
double RangeField::valueAt(double x) const
{
    const QRectF r = bar();
    return m_min + std::clamp((x - r.left()) / std::max(1.0, r.width()), 0.0, 1.0) * (m_max - m_min);
}

void RangeField::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = bar();
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(r, 3, 3);
    { QColor c = theme::accent(); c.setAlpha(isEnabled() ? 110 : 45); p.setBrush(c); }
    p.drawRect(QRectF(QPointF(xOf(m_lo), r.top()), QPointF(xOf(m_hi), r.bottom())));
    p.setPen(QPen(kBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    // Handles: a triangle pointing into the range
    p.setPen(Qt::NoPen);
    p.setBrush(isEnabled() ? theme::accent().lighter(115) : QColor(150, 150, 155));
    for (int k = 0; k < 2; ++k) {
        const double x = k ? xOf(m_hi) : xOf(m_lo);
        const double d = k ? -5 : 5;
        QPainterPath path;
        path.moveTo(x, r.top());
        path.lineTo(x + d, r.top());
        path.lineTo(x, r.top() + 6);
        path.closeSubpath();
        p.drawPath(path);
        QPainterPath low;
        low.moveTo(x, r.bottom());
        low.lineTo(x + d, r.bottom());
        low.lineTo(x, r.bottom() - 6);
        low.closeSubpath();
        p.drawPath(low);
        p.fillRect(QRectF(x - 0.5, r.top(), 1, r.height()), p.brush());
    }
}

void RangeField::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !isEnabled()) return;
    const double x = e->position().x();
    if (x < bar().left() || x > bar().right()) return;
    m_drag = std::abs(x - xOf(m_lo)) <= std::abs(x - xOf(m_hi)) ? 1 : 2;
    mouseMoveEvent(e);
}
void RangeField::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_drag) return;
    const double v = valueAt(e->position().x());
    if (m_drag == 1) {
        m_lo = std::min(v, m_hi);
        QSignalBlocker b(m_loSpin);
        m_loSpin->setValue(m_lo);
        emit edited(true, m_lo);
    } else {
        m_hi = std::max(v, m_lo);
        QSignalBlocker b(m_hiSpin);
        m_hiSpin->setValue(m_hi);
        emit edited(false, m_hi);
    }
    update();
}
void RangeField::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_drag || e->button() != Qt::LeftButton) return;
    m_drag = 0;
    emit editingFinished();
}

// ---------------------------------------------------------------------------
// Transport and play-mode icons
// ---------------------------------------------------------------------------

namespace {
// 20x20 logical glyph, drawn so no symbol font is needed (as the padlock is).
// Light on the dark panel, and in the color that reads on the accent once the button is checked.
QIcon drawnIcon(const std::function<void(QPainter &, const QColor &)> &draw)
{
    QIcon icon;
    for (int px : {20, 40}) {
        for (QIcon::State st : {QIcon::Off, QIcon::On}) {
            QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(px / 20.0, px / 20.0);
            draw(p, st == QIcon::On ? theme::onAccent() : QColor(225, 225, 228));
            p.end();
            icon.addPixmap(QPixmap::fromImage(img), QIcon::Normal, st);
        }
    }
    return icon;
}

QPainterPath triangle(double x, double y, double w, double h, int dir) // dir +1 right, -1 left
{
    QPainterPath t;
    t.moveTo(x + (dir > 0 ? 0 : w), y);
    t.lineTo(x + (dir > 0 ? 0 : w), y + h);
    t.lineTo(x + (dir > 0 ? w : 0), y + h / 2);
    t.closeSubpath();
    return t;
}
} // namespace

QIcon transportIcon(TransportIcon kind)
{
    return drawnIcon([kind](QPainter &p, const QColor &c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        switch (kind) {
        case TransportIcon::PlayForward: p.drawPath(triangle(6, 4, 9, 12, +1)); break;
        case TransportIcon::PlayBack: p.drawPath(triangle(5, 4, 9, 12, -1)); break;
        case TransportIcon::Pause:
            p.drawRect(QRectF(6, 4, 3, 12));
            p.drawRect(QRectF(11.5, 4, 3, 12));
            break;
        case TransportIcon::ToStart:
            p.drawRect(QRectF(5, 4, 2, 12));
            p.drawPath(triangle(8, 4, 7, 12, -1));
            break;
        case TransportIcon::StepBack:
            p.drawRect(QRectF(5, 4, 2, 12));
            p.drawPath(triangle(8, 6, 6, 8, -1));
            break;
        case TransportIcon::StepForward:
            p.drawPath(triangle(6, 6, 6, 8, +1));
            p.drawRect(QRectF(13, 4, 2, 12));
            break;
        case TransportIcon::MarkIn:  // [
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(c, 2));
            p.drawPolyline(QPolygonF({QPointF(12, 4), QPointF(7, 4), QPointF(7, 16), QPointF(12, 16)}));
            break;
        case TransportIcon::MarkOut: // ]
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(c, 2));
            p.drawPolyline(QPolygonF({QPointF(8, 4), QPointF(13, 4), QPointF(13, 16), QPointF(8, 16)}));
            break;
        }
    });
}

// PlayMode: 0 one-shot (plays once, freezes), 1 loop, 2 ping-pong, 3 stop (black at the end)
QIcon playModeIcon(int mode)
{
    return drawnIcon([mode](QPainter &p, const QColor &c) {
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        auto arrowHead = [&](double x, double y, int dir) {
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPath(triangle(dir > 0 ? x - 4 : x, y - 3, 4, 6, dir));
            p.setPen(QPen(c, 1.6));
            p.setBrush(Qt::NoBrush);
        };
        switch (mode) {
        case 1: // loop: a circle closed by an arrow
            p.drawArc(QRectF(4, 4, 12, 12), 40 * 16, 290 * 16);
            arrowHead(15.5, 7.5, +1);
            break;
        case 2: // ping-pong: forwards above, backwards below
            p.drawLine(QPointF(4, 7), QPointF(14, 7));
            arrowHead(16, 7, +1);
            p.drawLine(QPointF(16, 13), QPointF(6, 13));
            arrowHead(4, 13, -1);
            break;
        case 3: // stop: the arrow runs into a black screen
            p.drawLine(QPointF(2, 10), QPointF(9, 10));
            arrowHead(11, 10, +1);
            p.setPen(QPen(c, 1.4));
            p.setBrush(QColor(20, 20, 22));
            p.drawRoundedRect(QRectF(12.5, 5, 6, 10), 1.5, 1.5);
            break;
        default: // one-shot: the arrow stops and stays there
            p.drawLine(QPointF(3, 10), QPointF(12, 10));
            arrowHead(14, 10, +1);
            p.setPen(QPen(c, 1.6, Qt::DotLine));
            p.drawLine(QPointF(16, 6), QPointF(16, 14));
            break;
        }
    });
}

