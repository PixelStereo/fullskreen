#pragma once
// The animations of a layer's numbers (Layer::anims): made from a right-click on a parameter (Animate ▸ a wave, or
// keys), edited in the Anim tab of the inspector — one card per number, folded or not, pinned at the top or not — and,
// bigger, in a floating window that stays in front.

#include "AnimEditor.h"

#include <QPointer>
#include <QWidget>
#include <functional>
#include <initializer_list>

class QFrame;
class QLabel;
class QScrollArea;
class QToolButton;
class QUndoStack;
class QVBoxLayout;
class FlagBox;

// One animation of a layer's number, edited
class ParamAnimEditor : public AnimEditor
{
    Q_OBJECT
public:
    ParamAnimEditor(Engine *engine, QUndoStack *undo, quint64 layer, const QString &param, Layout layout, QWidget *parent = nullptr);
    quint64 layer() const { return m_layer; }
    const QString &param() const { return m_param; }

protected:
    bool fetch(Animation *a) const override;
    void store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey) override;
    void control(AnimAction action, double time) override;

private:
    quint64 m_layer;
    QString m_param;
};

namespace paramanim {
// The name of a layer's number ("Spatial › Rotation (°)"), or its address when the layer has no such number
QString label(Engine *e, quint64 layer, const QString &param);
// A new animation of the number (a wave: an AnimWave; keys: -1), as an undo step
void animate(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, int wave);
// The whole list replaced, as an undo step
void setAnims(Engine *e, QUndoStack *undo, quint64 layer, const std::vector<Animation> &anims, const QString &text);
void setOn(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, bool on);
void remove(Engine *e, QUndoStack *undo, quint64 layer, const QString &param);
// Its floating window (one per number: shown again if it is open)
void openWindow(Engine *e, QUndoStack *undo, quint64 layer, const QString &param, QWidget *anchor);
// What a right-click on a number offers: Animate ▸ the waves, keys; animated already: edit (`edit`), its window,
// on / off, remove
void fillMenu(QMenu *menu, Engine *e, QUndoStack *undo, quint64 layer, const QString &param, QWidget *anchor,
              const std::function<void(const QString &)> &edit);
} // namespace paramanim

// Right-click on the widgets of a layer's numbers: the Animate menu of the numbers they show. Only the numbers declared
// animatable on that layer (Params.h) are offered; a widget showing several (a position: x and y) offers each one.
class AnimateMenu : public QObject
{
public:
    AnimateMenu(Engine *e, QUndoStack *undo, quint64 layer, std::function<void(const QString &)> edit, QObject *parent);
    // `w` and the widgets in it show the numbers at `paths`; a label of them gets a ∿ when one is animated
    void attach(QWidget *w, const QStringList &paths);
    void attach(std::initializer_list<QWidget *> ws, const QStringList &paths);
    bool animated(const QString &path) const { return m_animated.contains(path); }

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    Engine *m_e;
    QUndoStack *m_undo;
    quint64 m_layer;
    std::function<void(const QString &)> m_edit;
    QStringList m_animatable, m_animated;
};

// The Anim tab: every animation of the layer, as cards. "+ Animate" animates another number; the pinned cards stay at
// the top, the others scroll below.
class ParamAnimPanel : public QWidget
{
    Q_OBJECT
public:
    ParamAnimPanel(Engine *engine, QUndoStack *undo, quint64 layer, QWidget *parent = nullptr);
    void reveal(const QString &param); // unfolded, scrolled to

signals:
    void projectEdited(); // a card pinned or folded: saved, not undoable

private:
    class Card;
    void rebuild();
    QString signature() const; // the numbers animated and the pinned ones: what rebuilds the cards
    Engine *m_engine;
    QUndoStack *m_undo;
    quint64 m_layer;
    QVBoxLayout *m_pinned, *m_list;
    QWidget *m_pinnedBox;
    QScrollArea *m_scroll;
    QLabel *m_empty;
    std::vector<Card *> m_cards;
    QString m_signature;
};
