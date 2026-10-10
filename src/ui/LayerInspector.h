#pragma once
#include <QPointer>
#include <QVariant>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class Engine;
class Mapping;
struct LayerValues;
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
class QGroupBox;
class ParamPanel;
class ParamAnimPanel;
class AnimateMenu;
class QTabWidget;
class ViewportOutputPanel;

// Inspector for the selected layer, group or viewport: at the top, always shown, its source (drop zone, transport,
// sound, generator parameters) and its compositing (opacity, blend, the viewports it appears in); below, in sub-tabs:
// ROI, Color (added / removed), Spatial (mapping), FX (ISF chain), Anim (the animations of its parameters), and for a
// viewport, Output (size, screen, publishing).
// All edits go through the undo stack. A locked layer shows its settings without allowing edits
// (the transport stays available). A click on a parameter's name resets it; a right-click animates it.
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
    void rescanLibraryRequested();         // re-read the ISF folders (shaders added in the Finder)
    void projectEdited();                  // saved interface state of the layer changed (not undoable)
    void kindChanged(const QString &kind); // "Layer", "Group" or "Viewport": the title of the tab

private:
    QWidget *buildSource(const LayerValues &s);
    QWidget *buildRoi(const LayerValues &s);
    QWidget *buildColor(const LayerValues &s);
    QWidget *buildCompositing(const LayerValues &s);
    QWidget *buildMapping(const LayerValues &s);
    QWidget *buildEffects(const LayerValues &s);
    void showEffectParams(); // parameters of the selected effect, swapped without rebuilding the list
    void editMapping(const QString &text, const std::function<void(Mapping &)> &fn, bool merge = false);
    void refreshSpatial(); // position / scale fields follow the mapping (handles dragged in the preview)
    void editEffects(const QString &text, const std::function<void()> &op);
    void editSource(const QString &text, const std::function<void()> &op);
    void setProp(int prop, const QVariant &value);
    void showAnimation(const QString &param); // the Anim tab, that number's card unfolded
    // The limits of the layer's number at `path`, as declared (Parameter.h); `lo`, `hi` if it has none
    std::pair<double, double> limitsOf(const QString &path, double lo, double hi) const;

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer = -1;
    int m_selectedEffect = 0;
    int m_subTab = 0; // ROI / Color / Spatial / FX / Anim / Output
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
    // Followed while a snapshot fades them (as temp / tint)
    QPointer<SliderField> m_opacity, m_volume;
    QPointer<ParamPanel> m_generatorParams, m_effectParams;
    QPointer<QGroupBox> m_softBox;
    QPointer<SliderField> m_softWidth[4], m_softPower[4];
    struct RouteField {
        quint64 viewport;
        QPointer<SliderField> field;
        std::shared_ptr<float> current; // the value an edit starts from (undo)
    };
    std::vector<RouteField> m_routeFields;
    bool m_previewing = false;
    QPointer<QProgressBar> m_meter;
    QPointer<QLabel> m_codecFact, m_pictureFact; // known once frames are decoded: kept up to date
    QPointer<ViewportOutputPanel> m_output;
    QPointer<QTabWidget> m_tabs;
    QPointer<AnimateMenu> m_animate; // right-click on a number: Animate
    QPointer<ParamAnimPanel> m_anims; // the Anim tab
    int m_animTab = -1;
    QStringList m_animated; // the numbers animated when the tabs were built (a ∿ by their names)
    QString m_revealAnim;   // the card to show once rebuilt
    QPointer<QDoubleSpinBox> m_posX, m_posY, m_scaleX, m_scaleY, m_rotation, m_pivotX, m_pivotY;
    bool m_scaleLinked = true;
    bool m_sizePx = false; // a viewport: width and height in composition pixels (a layer: scale in %)
};
