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
class SliderField;
class RangeField;
class QButtonGroup;
class RoiEditor;
class ColorEditor;
class QComboBox;
class ViewportOutputPanel;

// Inspector for the selected layer, group or viewport, in sub-tabs: Source (drop zone, transport, sound, generator
// parameters, roi), Color (added / removed), Spatial (mapping), Effects (ISF chain), Compositing (opacity, blend,
// the viewports it appears in), and for a viewport, Output (size, screen, publishing).
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
    void kindChanged(const QString &kind); // "Layer", "Group" or "Viewport": the title of the tab

private:
    QWidget *buildSource(const LayerSnapshot &s);
    QWidget *buildRoi(const LayerSnapshot &s);
    QWidget *buildColor(const LayerSnapshot &s);
    QWidget *buildCompositing(const LayerSnapshot &s);
    QWidget *buildMapping(const LayerSnapshot &s);
    QWidget *buildEffects(const LayerSnapshot &s);
    void showEffectParams(); // parameters of the selected effect, swapped without rebuilding the list
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

    QPointer<QWidget> m_fxDetail;
    QPointer<SliderField> m_position, m_speed;
    QPointer<RangeField> m_loop;
    QPointer<QButtonGroup> m_playButtons;
    QPointer<RoiEditor> m_roi;
    QPointer<ColorEditor> m_colorAdd, m_colorRemove;
    QPointer<SliderField> m_temp, m_tint;
    bool m_previewing = false;
    QPointer<QProgressBar> m_meter;
    QPointer<ViewportOutputPanel> m_output;
    QPointer<QDoubleSpinBox> m_posX, m_posY, m_scaleX, m_scaleY;
    bool m_scaleLinked = true;
};
