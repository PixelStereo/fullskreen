#include "ParamAnimPanel.h"
#include "Commands.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <optional>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

static const QColor kPlaying(76, 217, 100);

// The number's own name, without its category ("Rotation (°)")
static QString shortLabel(const QString &label)
{
    const int cut = label.lastIndexOf(QStringLiteral(" › "));
    return cut < 0 ? label : label.mid(cut + 3);
}

static const Animation *findAnim(const std::vector<Animation> &list, const QString &param)
{
    for (const Animation &a : list)
        if (!a.tracks.empty() && a.tracks.front().param == param) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------
// ParamAnimEditor

ParamAnimEditor::ParamAnimEditor(Engine *engine, QUndoStack *undo, quint64 layer, const QString &param, Layout layout,
                                 QWidget *parent)
    : AnimEditor(engine, undo, layout, false, parent), m_layer(layer), m_param(param)
{
    setLaneHeight(layout == Layout::Stacked ? 110 : 160);
    connect(engine, &Engine::layerAnimsChanged, this, [this](quint64 l) {
        if (l == m_layer && !m_committing) reload();
    });
    reload();
}

bool ParamAnimEditor::fetch(Animation *a) const { return m_engine->layerAnim(m_layer, m_param, a); }

void ParamAnimEditor::store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey)
{
    if (m_engine->isLocked(m_engine->indexOfId(m_layer))) { // locked meanwhile: the edit is not made
        QTimer::singleShot(0, this, [this] {
            forgetView();
            reload();
        });
        return;
    }
    if (m_undo) m_undo->push(new cmd::SetLayerAnim(m_engine, m_layer, m_param, before, after, text, mergeKey));
    else m_engine->setLayerAnim(m_layer, m_param, &after);
}

void ParamAnimEditor::loaded()
{
    // A locked layer: its animations are shown, not edited (they play on)
    setEditorEnabled(m_has && !m_engine->isLocked(m_engine->indexOfId(m_layer)));
}

void ParamAnimEditor::control(AnimAction action, double time) { m_engine->controlLayerAnim(m_layer, m_param, action, time); }

// ---------------------------------------------------------------------------
// Making, switching, removing

namespace {
// A layer's animation in a window of its own, in front, bigger than in the Anim tab
class ParamAnimWindow : public QWidget
{
public:
    ParamAnimWindow(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, QWidget *parent)
        : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_e(e), m_layer(layer), m_param(param)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        resize(980, 400);
        auto *v = new QVBoxLayout(this);
        auto *head = new QHBoxLayout;
        m_on = new FlagBox(QStringLiteral("On"));
        m_on->setToolTip(QStringLiteral("On: it plays and drives its number; off: the number stays where it is"));
        m_title = new QLabel;
        m_title->setStyleSheet("font-weight:bold;");
        head->addWidget(m_on);
        head->addWidget(m_title, 1);
        v->addLayout(head);
        m_editor = new ParamAnimEditor(e, undo, layer, param, AnimEditor::Layout::Side, this);
        v->addWidget(m_editor, 1);
        auto *how = new QLabel(QStringLiteral("Lane: click: a key · drag a key: move it (Shift: one axis) · drag elsewhere: draw · "
                                              "double-click a key: delete · right-click: curve (Bézier…), value · "
                                              "⌘/Ctrl+wheel: zoom time · Alt+wheel: zoom values"));
        how->setStyleSheet("color:#8a8a90; font-size:11px;");
        v->addWidget(how);
        if (undo) { // undo / redo from this window too
            QAction *u = undo->createUndoAction(this);
            u->setShortcut(QKeySequence::Undo);
            QAction *r = undo->createRedoAction(this);
            r->setShortcut(QKeySequence::Redo);
            addActions({u, r});
        }
        connect(m_on, &QCheckBox::toggled, this, [this, undo](bool on) {
            if (!m_filling) paramanim::setOn(m_e, undo, m_layer, m_param, on);
        });
        auto check = [this] { // gone (the layer, or its animation): closed
            if (!m_e->layerAnim(m_layer, m_param, nullptr)) close();
            else fill();
        };
        connect(e, &Engine::layerAnimsChanged, this, [check, this](quint64 l) {
            if (l == m_layer) check();
        });
        connect(e, &Engine::layersChanged, this, [check, this] {
            check();
            if (m_editor && !m_editor->isCommitting()) m_editor->reload(); // locked or unlocked meanwhile
        });
        fill();
    }

private:
    void fill()
    {
        Animation a;
        if (!m_e->layerAnim(m_layer, m_param, &a)) return;
        QString layerName;
        {
            Engine::Lock lk(&m_e->mutex());
            if (const Layer *l = m_e->layer(m_e->indexOfId(m_layer))) layerName = l->name;
        }
        const QString label = paramanim::label(m_e, m_layer, m_param);
        setWindowTitle(QStringLiteral("%1 — %2").arg(layerName, label));
        m_title->setText(QStringLiteral("%1 › %2").arg(layerName.toHtmlEscaped(), label.toHtmlEscaped()));
        m_filling = true;
        m_on->setChecked(a.tracks.front().enabled);
        m_filling = false;
        m_on->setEnabled(!m_e->isLocked(m_e->indexOfId(m_layer))); // a locked layer: shown, not edited
    }
    Engine *m_e;
    quint64 m_layer;
    QString m_param;
    FlagBox *m_on;
    QLabel *m_title;
    ParamAnimEditor *m_editor;
    bool m_filling = false;
};

QHash<QString, QPointer<ParamAnimWindow>> &windows()
{
    static QHash<QString, QPointer<ParamAnimWindow>> w;
    return w;
}
} // namespace

namespace paramanim {

QString label(Engine *e, quint64 layer, const QString &param)
{
    for (const Engine::AnimParam &p : e->animatableParams(layer))
        if (p.path == param) return p.label;
    return param;
}

// One animation replaced, added or removed (absent), as an undo step; nothing on a locked layer
static void setAnim(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, std::optional<Animation> before,
                    std::optional<Animation> after, const QString &text)
{
    if (e->isLocked(e->indexOfId(layer))) return;
    if (undo) undo->push(new cmd::SetLayerAnim(e, layer, param, std::move(before), std::move(after), text));
    else e->setLayerAnim(layer, param, after ? &*after : nullptr);
}

void animate(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, int wave)
{
    if (e->layerAnim(layer, param, nullptr)) return; // one animation per number
    setAnim(e, undo, layer, param, std::nullopt, e->makeLayerAnim(layer, param, wave),
            QStringLiteral("Animate %1").arg(shortLabel(label(e, layer, param))));
}

void setOn(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, bool on)
{
    Animation a;
    if (!e->layerAnim(layer, param, &a) || a.tracks.front().enabled == on) return;
    const Animation before = a;
    a.tracks.front().enabled = on;
    setAnim(e, undo, layer, param, before, a, on ? QStringLiteral("Animation On") : QStringLiteral("Animation Off"));
}

void remove(Engine *e, QUndoStack *undo, quint64 layer, const QString &param)
{
    Animation a;
    if (!e->layerAnim(layer, param, &a)) return;
    setAnim(e, undo, layer, param, a, std::nullopt, QStringLiteral("Remove Animation"));
}

void openWindow(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, QWidget *anchor)
{
    const QString key = QStringLiteral("%1/%2").arg(layer).arg(param);
    QPointer<ParamAnimWindow> &w = windows()[key];
    QWidget *top = anchor ? anchor->window() : QApplication::activeWindow();
    if (!w) w = new ParamAnimWindow(e, undo, layer, param, top);
    w->show();
    w->raise();
    w->activateWindow();
}

void fillMenu(QMenu *menu, Engine *e, QUndoStack *undo, quint64 layer, const QString &param, QWidget *widget,
              const std::function<void(const QString &)> &edit)
{
    const QPointer<QWidget> anchor = widget; // the menu may outlive it (the inspector rebuilt meanwhile)
    Animation a;
    if (e->layerAnim(layer, param, &a)) {
        menu->addAction(QStringLiteral("Edit Animation"), menu, [edit, param] {
            if (edit) edit(param);
        });
        menu->addAction(QStringLiteral("Open Animation in a Window"), menu,
                        [e, undo, layer, param, anchor] { openWindow(e, undo, layer, param, anchor.data()); });
        QAction *on = menu->addAction(QStringLiteral("Animation On"), menu,
                                      [e, undo, layer, param](bool checked) { setOn(e, undo, layer, param, checked); });
        on->setCheckable(true);
        on->setChecked(a.tracks.front().enabled);
        menu->addAction(QStringLiteral("Remove Animation"), menu, [e, undo, layer, param] { remove(e, undo, layer, param); });
        return;
    }
    QMenu *sub = menu->addMenu(QStringLiteral("Animate"));
    auto make = [=](int wave) {
        return [=] {
            animate(e, undo, layer, param, wave);
            if (edit) edit(param);
        };
    };
    for (int w = 0; w < kAnimWaveCount; ++w) sub->addAction(animWaveName(AnimWave(w)), menu, make(w));
    sub->addSeparator();
    sub->addAction(QStringLiteral("Keys (draw the curve)"), menu, make(-1));
}

} // namespace paramanim

// ---------------------------------------------------------------------------
// AnimateMenu

AnimateMenu::AnimateMenu(Engine *e, QUndoStack *undo, quint64 layer, std::function<void(const QString &)> edit, QObject *parent)
    : QObject(parent), m_e(e), m_undo(undo), m_layer(layer), m_edit(std::move(edit))
{
    for (const Engine::AnimParam &p : e->animatableParams(layer)) m_animatable << p.path;
    for (const Animation &a : e->layerAnims(layer))
        if (!a.tracks.empty()) m_animated << a.tracks.front().param;
}

void AnimateMenu::attach(QWidget *w, const QStringList &paths)
{
    if (!w) return;
    QStringList offered;
    for (const QString &p : paths)
        if (m_animatable.contains(p)) offered << p; // declared animatable on this layer
    if (offered.isEmpty()) return;
    w->setProperty("animPaths", offered);
    w->installEventFilter(this);
    for (QWidget *c : w->findChildren<QWidget *>()) c->installEventFilter(this);
    auto *label = qobject_cast<QLabel *>(w);
    if (!label) return;
    QString tip = label->toolTip();
    for (const QString &p : offered)
        if (m_animated.contains(p)) {
            label->setText(label->text() + QStringLiteral(" <span style='color:%1'>∿</span>").arg(theme::css()));
            tip += QStringLiteral("\nAnimated (Anim tab)");
            break;
        }
    label->setToolTip(tip + QStringLiteral("\nRight-click: animate"));
}

void AnimateMenu::attach(std::initializer_list<QWidget *> ws, const QStringList &paths)
{
    for (QWidget *w : ws) attach(w, paths);
}

bool AnimateMenu::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() != QEvent::ContextMenu) return false;
    auto *w = qobject_cast<QWidget *>(o);
    while (w && !w->property("animPaths").isValid()) w = w->parentWidget();
    if (!w || !w->isEnabled()) return false;
    const QStringList paths = w->property("animPaths").toStringList();
    // Shown without waiting here (no event loop inside this event): the menu goes with the widget, should the
    // inspector be rebuilt meanwhile
    auto *menu = new QMenu(w);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (paths.size() == 1) {
        paramanim::fillMenu(menu, m_e, m_undo, m_layer, paths.front(), w, m_edit);
    } else { // a sub-menu for each number (X, Y; R, G, B…)
        for (const QString &p : paths) {
            QMenu *sub = menu->addMenu(shortLabel(paramanim::label(m_e, m_layer, p)) +
                                       (m_animated.contains(p) ? QStringLiteral("  ∿") : QString()));
            paramanim::fillMenu(sub, m_e, m_undo, m_layer, p, w, m_edit);
        }
    }
    menu->popup(static_cast<QContextMenuEvent *>(e)->globalPos());
    return true;
}

// ---------------------------------------------------------------------------
// ParamAnimPanel::Card

class ParamAnimPanel::Card : public QFrame
{
public:
    Card(ParamAnimPanel *panel, const Animation &a) : m_panel(panel), m_param(a.tracks.front().param), m_pinned(a.pinned)
    {
        Engine *e = panel->m_engine;
        QUndoStack *undo = panel->m_undo;
        const quint64 layer = panel->m_layer;
        setObjectName("animCard");
        setStyleSheet("#animCard { background:#232327; border:1px solid #38383e; border-radius:4px; }");
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(6, 4, 6, 6);
        v->setSpacing(4);
        auto *head = new QHBoxLayout;
        head->setSpacing(4);
        m_fold = new QToolButton;
        m_fold->setAutoRaise(true);
        m_fold->setToolTip(QStringLiteral("Fold / unfold"));
        m_on = new FlagBox;
        m_on->setToolTip(QStringLiteral("On: it plays and drives its number; off: the number stays where it is"));
        m_title = new QLabel(paramanim::label(e, layer, m_param));
        m_title->setStyleSheet("font-weight:bold;");
        m_title->setToolTip(m_param);
        m_title->setCursor(Qt::PointingHandCursor);
        m_title->installEventFilter(this);
        m_summary = new QLabel;
        m_summary->setStyleSheet("color:#9a9aa0; font-size:11px;");
        auto *pin = new QToolButton;
        pin->setText(QStringLiteral("Pin"));
        pin->setCheckable(true);
        pin->setChecked(m_pinned);
        pin->setToolTip(QStringLiteral("Pinned: stays at the top of the tab, above the ones that scroll"));
        pin->setStyleSheet(QStringLiteral("QToolButton:checked { background:%1; color:%2; }").arg(theme::css(), theme::onAccent().name()));
        auto *window = new QToolButton;
        window->setText(QStringLiteral("↗"));
        window->setToolTip(QStringLiteral("Open in a window (bigger, in front)"));
        auto *del = new QToolButton;
        del->setText(QStringLiteral("✕"));
        del->setToolTip(QStringLiteral("Remove the animation (the number stays where it is)"));
        head->addWidget(m_fold);
        head->addWidget(m_on);
        head->addWidget(m_title, 1);
        head->addWidget(m_summary);
        head->addWidget(pin);
        head->addWidget(window);
        head->addWidget(del);
        v->addLayout(head);
        editor = new ParamAnimEditor(e, undo, layer, m_param, AnimEditor::Layout::Stacked, this);
        v->addWidget(editor);
        setFolded(a.folded, false);
        refresh(a);

        connect(m_fold, &QToolButton::clicked, this, [this] { setFolded(!m_folded, true); });
        connect(m_on, &QCheckBox::toggled, this, [this, e, undo, layer](bool on) {
            if (!m_filling) paramanim::setOn(e, undo, layer, m_param, on);
        });
        connect(pin, &QToolButton::toggled, this, [this, e, layer](bool on) {
            m_pinned = on;
            const QString param = m_param;
            const bool folded = m_folded;
            QTimer::singleShot(0, m_panel, [p = m_panel, e, layer, param, on, folded] { // the card moves: not from inside it
                e->setLayerAnimView(layer, param, on, folded);
                emit p->projectEdited();
            });
        });
        connect(window, &QToolButton::clicked, this, [this, e, undo, layer] { paramanim::openWindow(e, undo, layer, m_param, this); });
        connect(del, &QToolButton::clicked, this, [this, e, undo, layer] {
            const QString param = m_param;
            QTimer::singleShot(0, m_panel, [e, undo, layer, param] { paramanim::remove(e, undo, layer, param); });
        });
    }
    const QString &param() const { return m_param; }
    bool folded() const { return m_folded; }
    void setFolded(bool f, bool save)
    {
        m_folded = f;
        m_fold->setText(f ? QStringLiteral("▸") : QStringLiteral("▾"));
        editor->setVisible(!f);
        if (!save) return;
        m_panel->m_engine->setLayerAnimView(m_panel->m_layer, m_param, m_pinned, f);
        emit m_panel->projectEdited();
    }
    // The header from the animation: on, what drives it, whether it plays
    void refresh(const Animation &a)
    {
        const AnimTrack &t = a.tracks.front();
        m_filling = true;
        m_on->setChecked(t.enabled);
        m_filling = false;
        const QString what = t.oscillator ? QStringLiteral("%1 · %2 s").arg(animWaveName(t.wave)).arg(t.period, 0, 'g', 3)
                                          : QStringLiteral("Keys · %1 s").arg(a.duration, 0, 'g', 3);
        const bool playing = a.state == AnimState::Playing && t.enabled;
        m_summary->setText(QStringLiteral("%1 <span style='color:%2'>●</span>").arg(what, playing ? kPlaying.name() : QStringLiteral("#55555c")));
        m_summary->setToolTip(playing ? QStringLiteral("Playing") : QStringLiteral("Not playing"));
    }
    ParamAnimEditor *editor;

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == m_title && e->type() == QEvent::MouseButtonRelease) {
            setFolded(!m_folded, true);
            return true;
        }
        return QFrame::eventFilter(o, e);
    }

private:
    ParamAnimPanel *m_panel;
    QString m_param;
    bool m_pinned, m_folded = false, m_filling = false;
    QToolButton *m_fold;
    FlagBox *m_on;
    QLabel *m_title, *m_summary;
};

// ---------------------------------------------------------------------------
// ParamAnimPanel

ParamAnimPanel::ParamAnimPanel(Engine *engine, QUndoStack *undo, quint64 layer, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo), m_layer(layer)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    auto *top = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("+ Animate"));
    add->setToolTip(QStringLiteral("Animate a number of this layer (also: right-click a parameter in the other tabs)"));
    auto *menu = new QMenu(add);
    add->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly)); // the sub-menus of the last time
        menu->clear();
        addParamMenu(menu, m_engine->animatableParams(m_layer), [this](QMenu *into, const Engine::AnimParam &p, const QString &text) {
            QMenu *sub = into->addMenu(text);
            paramanim::fillMenu(sub, m_engine, m_undo, m_layer, p.path, this, [this](const QString &param) { reveal(param); });
        });
    });
    auto *foldAll = new QPushButton(QStringLiteral("Fold All"));
    auto *unfoldAll = new QPushButton(QStringLiteral("Unfold All"));
    foldAll->setToolTip(QStringLiteral("Every card folded: the list of the animations"));
    top->addWidget(add);
    top->addStretch();
    top->addWidget(foldAll);
    top->addWidget(unfoldAll);
    v->addLayout(top);
    m_empty = new QLabel(QStringLiteral("No animation yet. Right-click a parameter (Opacity, Rotation, a shader's "
                                        "parameter…) › Animate, and choose a wave or keys — or use + Animate."));
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet("color:#8a8a90;");
    v->addWidget(m_empty);
    // The pinned cards above, the others below: each part scrolls, the line between them is dragged
    auto scrolled = [](QVBoxLayout **list) {
        auto *area = new QScrollArea;
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *host = new QWidget;
        *list = new QVBoxLayout(host);
        (*list)->setContentsMargins(0, 0, 0, 0);
        (*list)->setSpacing(4);
        (*list)->addStretch();
        area->setWidget(host);
        return area;
    };
    m_pinnedArea = scrolled(&m_pinned);
    m_scroll = scrolled(&m_list);
    m_split = new QSplitter(Qt::Vertical);
    m_split->setChildrenCollapsible(false);
    m_split->addWidget(m_pinnedArea);
    m_split->addWidget(m_scroll);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);
    v->addWidget(m_split, 1);

    auto foldEvery = [this](bool f) {
        for (Card *c : m_cards)
            if (c->folded() != f) c->setFolded(f, true);
    };
    connect(foldAll, &QPushButton::clicked, this, [foldEvery] { foldEvery(true); });
    connect(unfoldAll, &QPushButton::clicked, this, [foldEvery] { foldEvery(false); });
    connect(engine, &Engine::layerAnimsChanged, this, [this](quint64 l) {
        if (l != m_layer) return;
        if (signature() != m_signature) {
            QTimer::singleShot(0, this, [this] { // not from inside a card that is going
                if (signature() != m_signature) rebuild();
            });
            return;
        }
        const std::vector<Animation> list = m_engine->layerAnims(m_layer);
        for (Card *c : m_cards)
            if (const Animation *a = findAnim(list, c->param())) c->refresh(*a);
    });
    auto *timer = new QTimer(this); // whether each one plays
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!isVisible()) return;
        const std::vector<Animation> list = m_engine->layerAnims(m_layer);
        for (Card *c : m_cards)
            if (const Animation *a = findAnim(list, c->param())) c->refresh(*a);
    });
    timer->start();
    rebuild();
}

QString ParamAnimPanel::signature() const
{
    QString s;
    for (const Animation &a : m_engine->layerAnims(m_layer))
        if (!a.tracks.empty()) s += a.tracks.front().param + (a.pinned ? QStringLiteral("*") : QString()) + QLatin1Char('\n');
    return s;
}

void ParamAnimPanel::rebuild()
{
    for (Card *c : m_cards) {
        c->hide();
        c->deleteLater();
    }
    m_cards.clear();
    m_signature = signature();
    int pinned = 0;
    const std::vector<Animation> list = m_engine->layerAnims(m_layer);
    for (const Animation &a : list) {
        if (a.tracks.empty()) continue;
        auto *c = new Card(this, a);
        QVBoxLayout *into = a.pinned ? m_pinned : m_list;
        into->insertWidget(into->count() - 1, c); // before the stretch
        pinned += a.pinned ? 1 : 0;
        m_cards.push_back(c);
    }
    m_pinnedArea->setVisible(pinned > 0);
    m_empty->setVisible(m_cards.empty());
    m_scroll->setVisible(int(m_cards.size()) > pinned);
    m_split->setVisible(!m_cards.empty());
}

void ParamAnimPanel::reveal(const QString &param)
{
    if (signature() != m_signature) rebuild(); // just made
    for (Card *c : m_cards)
        if (c->param() == param) {
            if (c->folded()) c->setFolded(false, true);
            QTimer::singleShot(0, this, [this, card = QPointer<Card>(c)] {
                if (!card) return;
                for (QScrollArea *area : {m_pinnedArea, m_scroll})
                    if (card->parentWidget() && card->parentWidget()->parentWidget() == area->viewport()) area->ensureWidgetVisible(card);
            });
        }
}
