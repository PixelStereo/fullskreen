#pragma once
// The numbers of a layer, declared once: each one's address — its OSC address under the layer, the path of a timeline's
// track or of an animation of the Anim tab, the timing key of a snapshot — its name in the interface, its range, and
// whether timelines and animations can drive it (an attribute of the number, not a list kept elsewhere). The OSC
// namespace, the timelines, the layers' animations and the Animate menu all read them from here.
//
// The fixed numbers (opacity, ROI, color, spatial, soft edge, playback, text) are a table; the shaders' (the
// generator's parameters, each effect's speed and parameters) come from their inputs (IsfInput::isNumber), named in the
// addresses as in OSC (osc::safeName of the input's name; an effect by its segment, osc::uniqueSegments).

#include "Layer.h"

#include <QSize>
#include <QString>
#include <vector>

struct NumberParam {
    QString path;  // "opacity", "spatial/rotation", "fx/<fx>/param/<name>/x"… (inside the layer)
    QString label; // "Spatial › Rotation (°)": the category, then the number
    double min = 0, max = 1; // the range shown (bars, lanes, OSC range); a value is kept within lo..hi
    double lo = 0, hi = 1;
    bool animatable = true; // a timeline or an animation can drive it
    QList<int> values;      // the values it can take, when it is a choice (a shader's LONG input); empty: any
};

// The numbers of layer l, in the order of the interface (comp: the composition's size, for the pixels)
std::vector<NumberParam> layerNumbers(const Layer &l, QSize comp);
// The number at `path` of layer l: read into *get, or written from *set (kept within its limits). False: no such number
// on this layer.
bool layerNumber(Layer &l, const QString &path, double *get, const double *set, QSize comp);
// Its declaration (false: no such number on this layer)
bool layerNumberParam(const Layer &l, const QString &path, QSize comp, NumberParam *p);
