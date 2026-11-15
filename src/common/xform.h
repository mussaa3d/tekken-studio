#pragma once
// Transforms composed the way the engine composes them, for putting recorded motion somewhere else, and the arithmetic of playing a
// take back on the timeline. No game dependency, so it can be tested without Tekken 8.
#include <algorithm>
#include <cmath>

#include "common/cinemath.h"

namespace cine {

struct Xform {
    Quat Rotation;
    Vec3 Translation;
    Vec3 Scale { 1, 1, 1 };
};

inline Vec3 Rotate(Quat q, Vec3 v)
{
    // v + 2w(q x v) + 2 q x (q x v)
    const Vec3 u { q.X, q.Y, q.Z };
    const Vec3 a { u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X };
    const Vec3 b { u.Y * a.Z - u.Z * a.Y, u.Z * a.X - u.X * a.Z, u.X * a.Y - u.Y * a.X };
    return v + a * (2.0 * q.W) + b * 2.0;
}

inline Vec3 Scaled(Vec3 v, Vec3 s)
{
    return { v.X * s.X, v.Y * s.Y, v.Z * s.Z };
}

// A, then B: a point goes through A first. The same order as the engine's FTransform operator*.
inline Xform Compose(const Xform& a, const Xform& b)
{
    Xform out;
    out.Rotation = Normalized(Multiply(b.Rotation, a.Rotation));
    out.Scale = Scaled(a.Scale, b.Scale);
    out.Translation = Rotate(b.Rotation, Scaled(a.Translation, b.Scale)) + b.Translation;
    return out;
}

// Exact for a scale that's the same on every axis, which is all this is used with.
inline Xform Inverse(const Xform& t)
{
    const auto reciprocal = [](double s) { return std::fabs(s) < 1e-9 ? 0.0 : 1.0 / s; };
    Xform out;
    out.Rotation = Conjugate(t.Rotation);
    out.Scale = { reciprocal(t.Scale.X), reciprocal(t.Scale.Y), reciprocal(t.Scale.Z) };
    out.Translation = Scaled(Rotate(out.Rotation, t.Translation * -1.0), out.Scale);
    return out;
}

inline Vec3 Apply(const Xform& t, Vec3 point)
{
    return Rotate(t.Rotation, Scaled(point, t.Scale)) + t.Translation;
}

inline Xform BlendXform(const Xform& a, const Xform& b, double alpha)
{
    Xform out;
    out.Rotation = Slerp(a.Rotation, b.Rotation, alpha);
    out.Translation = Lerp(a.Translation, b.Translation, alpha);
    out.Scale = Lerp(a.Scale, b.Scale, alpha);
    return out;
}

// Where something recorded goes when its first frame is put at `placement`: the motion since the first frame, then the placement.
// With the placement equal to the first frame's own transform, this gives back `now`.
inline Xform Replaced(const Xform& first, const Xform& now, const Xform& placement)
{
    return Compose(Compose(now, Inverse(first)), placement);
}

// ----- Playing a take: which two recorded frames a moment falls between.
enum class TakeEnd { Hold, Loop, Hide };

struct TakeCursor {
    int A = 0, B = 0;   // frames to blend
    double Alpha = 0;   // how far from A to B
    bool Visible = true;
};

// `seconds` is the time since the actor's start on the timeline, `speed` how fast the take plays (1 = as recorded, at 60 frames a second).
inline TakeCursor SampleTake(int frames, double seconds, double speed, TakeEnd end)
{
    TakeCursor cursor;
    if (frames <= 0) {
        cursor.Visible = false;
        return cursor;
    }
    const int last = frames - 1;
    double at = seconds * 60.0 * speed;
    if (at < 0) {
        cursor.Visible = end != TakeEnd::Hide;
        return cursor; // waits on its first frame
    }
    if (last == 0)
        return cursor;
    if (at >= last) {
        if (end == TakeEnd::Loop) {
            at = std::fmod(at, static_cast<double>(last));
        } else {
            cursor.A = cursor.B = last;
            cursor.Visible = end != TakeEnd::Hide;
            return cursor;
        }
    }
    cursor.A = std::clamp(static_cast<int>(std::floor(at)), 0, last);
    cursor.B = std::min(cursor.A + 1, last);
    cursor.Alpha = std::clamp(at - cursor.A, 0.0, 1.0);
    return cursor;
}

} // namespace cine
