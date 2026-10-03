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
class QProgressBar;
class QDoubleSpinBox;
class SeekBar;
class CropEditor;
class ColorEditor;
class QComboBox;

// Inspector for the selected layer or group, in sub-tabs: Source (drop zone, transport, sound, generator parameters,
// crop), Color (added / removed), Spatial (mapping), Effects (ISF chain), Compositing (opacity, blend).
// All edits go through the undo stack. A locked layer shows its settings without allowing edits
// (the transport stays available). A click on a parameter's name resets it.
class LayerInspector : public QWidget
{
    Q_OBJECT
public:
    LayerInspector(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);

    void setLayer(int index);
    int layerIndex() const { return m_layer; }
    void rebuild();
    void refreshDynamic(); // playback position (called periodically)

signals:
    void layerChanged();   // name, visibility, source: the list must be refreshed
    void mappingChanged(); // the mapping view must be redrawn
    void fileDropped(const QString &path); // media or ISF dropped on the Source tab: load it into the layer
    void setInOutRequested(bool in);       // in / out point at the current position
    void projectEdited();                  // saved interface state of the layer changed (not undoable)

private:
    QWidget *buildSource(const LayerSnapshot &s);
    QWidget *buildCrop(const LayerSnapshot &s);
    QWidget *buildColor(const LayerSnapshot &s);
    QWidget *buildCompositing(const LayerSnapshot &s);
    QWidget *buildMapping(const LayerSnapshot &s);
    QWidget *buildEffects(const LayerSnapshot &s);
    void editMapping(const QString &text, const std::function<void(Mapping &)> &fn, bool merge = false);
    void refreshSpatial(); // position / scale fields follow the mapping (handles dragged in the preview)
    void editEffects(const QString &text, const std::function<void()> &op);
    void editSource(const QString &text, const std::function<void()> &op);
    void setProp(int prop, const QVariant &value);

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer = -1;
    int m_selectedEffect = 0;
    int m_subTab = 0; // Source / Color / Spatial / Effects / Compositing
    quint64 m_layerId = 0;
    bool m_locked = false;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;

    QPointer<SeekBar> m_seek;
    QPointer<CropEditor> m_crop;
    QPointer<ColorEditor> m_colorAdd, m_colorRemove;
    QPointer<QDoubleSpinBox> m_temp, m_tint;
    bool m_previewing = false;
    QPointer<QLabel> m_time, m_duration;
    QPointer<QPushButton> m_play;
    QPointer<QProgressBar> m_meter;
    QPointer<QDoubleSpinBox> m_posX, m_posY, m_scaleX, m_scaleY;
    bool m_scaleLinked = true;
};
