#pragma once
#include <QPointer>
#include <QWidget>

class Engine;
class QVBoxLayout;
class QSlider;
class QLabel;
class QPushButton;
class QListWidget;

// Inspecteur du calque sélectionné : source, transport vidéo, paramètres ISF,
// fusion, mapping et chaîne d'effets.
class LayerInspector : public QWidget
{
    Q_OBJECT
public:
    explicit LayerInspector(Engine *engine, QWidget *parent = nullptr);

    void setLayer(int index);
    int layerIndex() const { return m_layer; }
    void rebuild();
    void refreshDynamic(); // position de lecture (appelé périodiquement)

signals:
    void layerChanged();   // nom, visibilité, source : la liste doit être rafraîchie
    void mappingChanged(); // la vue de mapping doit être redessinée
    void addSourceRequested(const QString &kind); // "video", "image"

private:
    QWidget *buildSource();
    QWidget *buildCompositing();
    QWidget *buildMapping();
    QWidget *buildEffects();
    void chooseGenerator(const QString &path);

    Engine *m_engine;
    int m_layer = -1;
    int m_selectedEffect = 0;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;

    QPointer<QSlider> m_seek;
    QPointer<QLabel> m_time;
    QPointer<QPushButton> m_play;
};
