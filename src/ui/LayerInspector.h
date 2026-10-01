#pragma once
#include <QPointer>
#include <QVariant>
#include <QWidget>
#include <functional>

class Engine;
class Mapping;
struct LayerSnapshot;
class QUndoStack;
class QVBoxLayout;
class QSlider;
class QLabel;
class QPushButton;
class QListWidget;

// Inspecteur du calque sélectionné : source, transport vidéo, paramètres ISF,
// fusion, mapping et chaîne d'effets. Les modifications passent par la pile d'annulation.
class LayerInspector : public QWidget
{
    Q_OBJECT
public:
    LayerInspector(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);

    void setLayer(int index);
    int layerIndex() const { return m_layer; }
    void rebuild();
    void refreshDynamic(); // position de lecture (appelé périodiquement)

signals:
    void layerChanged();   // nom, visibilité, source : la liste doit être rafraîchie
    void mappingChanged(); // la vue de mapping doit être redessinée
    void addSourceRequested(const QString &kind); // "video", "image"

private:
    QWidget *buildSource(const LayerSnapshot &s);
    QWidget *buildCompositing(const LayerSnapshot &s);
    QWidget *buildMapping(const LayerSnapshot &s);
    QWidget *buildEffects(const LayerSnapshot &s);
    void chooseGenerator(const QString &path);
    void editMapping(const QString &text, const std::function<void(Mapping &)> &fn);
    void editEffects(const QString &text, const std::function<void()> &op);
    void editSource(const QString &text, const std::function<void()> &op);
    void setProp(int prop, const QVariant &value);

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer = -1;
    int m_selectedEffect = 0;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;

    QPointer<QSlider> m_seek;
    QPointer<QLabel> m_time;
    QPointer<QPushButton> m_play;
};
