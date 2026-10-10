#pragma once
// Numbers drawn over time, played on their own clock. Shared by the timelines of the show (Engine::Animation in the
// Engine's list) and by the animations of a layer's numbers (Layer::anims: one track each, on that layer).
//
// An animation has a duration (one pass), a loop mode and a speed, and tracks, each driving one number: a curve
// (keys over the duration, each eased towards the next one — the pattern repeats when it loops) or an oscillator
// (a wave of its own period, on the time played so that it goes on without a jump across the loops). Values are
// absolute.

#include <QString>
#include <QtGlobal>
#include <cmath>
#include <limits>
#include <vector>

// Random: a new value each period, held (sample and hold); SmoothRandom: from one random value to the next over a
// period. Both are drawn from the track's seed: the same at the same time, so they can be shown and sought.
enum class AnimWave { Sine = 0, Triangle = 1, Saw = 2, Square = 3, Random = 4, SmoothRandom = 5 };
inline constexpr int kAnimWaveCount = 6;
enum class AnimLoop { Once = 0, Loop = 1, PingPong = 2 };
enum class AnimState { Stopped = 0, Playing = 1, Paused = 2 };
enum class AnimAction { Play = 0, Pause = 1, Stop = 2, Rewind = 3, Seek = 4, LoopMode = 5, Speed = 6 };
inline constexpr int kAnimHold = 100; // a key's curve: holds its value until the next key
inline constexpr int kAnimBezier = 6; // a key's curve: a cubic Bézier, shaped by its out handle and the next key's in handle

struct AnimKey {
    double t = 0, v = 0; // seconds into the pass, value
    int curve = 0;       // towards the next key: an easing (0 linear, 1 in, 2 out, 3 in-out, 4 in cubic,
                         // 5 out cubic), kAnimBezier or kAnimHold
    bool isCurrentValue = false; // the first key only: starts from the value the number has when the animation
                                 // starts (v is then only a placeholder), so that nothing jumps
    // Bézier handles, from the key (seconds, value): `out` shapes the curve towards the next key (when this key's
    // curve is kAnimBezier), `in` the one from the previous key (when that key's curve is). (0, 0): automatic —
    // flat, a third of the way.
    double inDt = 0, inDv = 0, outDt = 0, outDv = 0;
    bool hasIn() const { return inDt != 0 || inDv != 0; }
    bool hasOut() const { return outDt != 0 || outDv != 0; }
};

struct AnimTrack {
    quint64 layer = 0; // the layer driven (its id; 0: the composition)
    QString param;     // the number (see Engine::animatableParams): "opacity", "spatial/rotation", "fx/<fx>/param/<name>"…
    bool enabled = true;
    bool oscillator = false;
    std::vector<AnimKey> keys; // curve: sorted by time
    AnimWave wave = AnimWave::Sine;
    double period = 1, center = 0.5, amplitude = 0.5, phase = 0; // oscillator (phase: 0..1 of a period)
    quint32 seed = 1;                                          // Random waves
    double valueAt(double position, double played) const; // nan: no value (a curve without keys)
    double keyValue(size_t k) const; // a key's value (the one captured for a "current value" key)
    // The two handles of the segment from key k to key k + 1, as absolute points (time, value), the automatic ones
    // resolved and both kept within the segment's time
    void bezierHandles(size_t k, double *t1, double *v1, double *t2, double *v2) const;
    // Where it is (not saved): the value read when the animation started, for a first key "current value"
    double captured = std::numeric_limits<double>::quiet_NaN();
};

struct Animation {
    quint64 id = 0; // a timeline: stable (sequences refer to it), given by Engine::addAnimation
    QString name;
    double duration = 4; // seconds of one pass
    AnimLoop loop = AnimLoop::Loop;
    int repeat = 0; // passes of Loop / PingPong (0: endless)
    double speed = 1.0; // playback speed (0 to 10x; 0: frozen where it is, still playing)
    std::vector<AnimTrack> tracks;
    // A layer's number: how its card is shown in the Anim tab (saved)
    bool pinned = false, folded = false;
    // Where it is (not saved)
    AnimState state = AnimState::Stopped;
    double clock = 0;      // seconds played since its start (or since its loop mode last changed)
    bool reversed = false; // the first pass goes backwards (a loop mode changed during a ping-pong's way back)
    double length() const; // the time it plays from the start (infinity: endless)
    double position(double clock) const; // within the pass: 0..duration
    double position() const { return position(clock); }
    bool backwardsAt(double clock) const; // the pass at that clock goes from the end to the start
    double clockAt(double position) const; // the clock giving `position` within the pass it is in now
    // A new loop mode / repeat while it runs: it goes on from where it is, the way it goes, and the passes
    // count from the one it is in (Once: it ends this pass, then stops — it does not jump to the end)
    void setLoop(AnimLoop loop, int repeat);
    // One step of its clock while it plays; false once it is over (Once, or its passes done): stopped, its last values
    bool advance(double dt);
};
