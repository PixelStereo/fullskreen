#pragma once
// Transport timeline of a video or a sound: maps a monotonic clock (seconds of playback, advancing with
// |speed|) to a position in the media, according to the play mode and the starting direction.
//
// The clock is cut into legs, each one played forwards (position increases) or backwards:
//  - the first leg starts at `origin` in direction `dir` and runs to the end (forwards) or the start (backwards);
//  - Once: nothing after the first leg (the position stays at its end);
//  - Loop: then whole legs in the same direction (0 → d, or d → 0);
//  - PingPong: then whole legs alternating direction.
// Video and audio decoders deliver their frames / samples stamped with this clock, so picture and sound
// follow the same timeline whatever the speed sign or the mode.

#include <algorithm>
#include <cmath>

struct Timeline {
    enum Mode { Once = 0, Loop = 1, PingPong = 2 };

    double duration = 0; // d (0: unknown, plain forward)
    int mode = Loop;
    double origin = 0;   // position at clock 0
    int dir = 1;         // +1 forwards, -1 backwards, at clock 0

    struct Leg {
        bool forward = true;
        double from = 0, to = 0;   // positions at the start and the end of the leg
        double clockStart = 0;     // clock at the start of the leg
        double length() const { return std::abs(to - from); }
        double clockEnd() const { return clockStart + length(); }
    };

    Leg firstLeg() const
    {
        Leg l;
        l.forward = dir >= 0;
        l.from = std::clamp(origin, 0.0, duration);
        l.to = l.forward ? duration : 0.0;
        return l;
    }

    // Leg containing clock c (for Once, the first leg, even beyond its end)
    Leg legAt(double c) const
    {
        Leg l = firstLeg();
        if (duration <= 0 || mode == Once || c < l.clockEnd()) return l;
        const double r = c - l.clockEnd();
        const double k = std::floor(r / duration);
        Leg n;
        n.clockStart = l.clockEnd() + k * duration;
        bool forward = dir >= 0;
        if (mode == PingPong && std::fmod(k, 2.0) == 0.0) forward = !forward; // the leg after the first turns back
        n.forward = forward;
        n.from = forward ? 0.0 : duration;
        n.to = forward ? duration : 0.0;
        return n;
    }

    // Leg that follows `l`
    Leg nextLeg(const Leg &l) const { return legAt(l.clockEnd() + 1e-9); }

    double position(double c) const
    {
        if (duration <= 0) return origin + c * (dir >= 0 ? 1 : -1);
        const Leg l = legAt(c);
        const double t = std::min(c - l.clockStart, l.length());
        return std::clamp(l.forward ? l.from + t : l.from - t, 0.0, duration);
    }

    // Once: true when the clock has gone past the end of the (only) leg
    bool ended(double c) const { return mode == Once && duration > 0 && c >= firstLeg().clockEnd(); }

    bool operator==(const Timeline &o) const
    {
        return duration == o.duration && mode == o.mode && origin == o.origin && dir == o.dir;
    }
};
