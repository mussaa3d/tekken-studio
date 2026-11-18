#pragma once
// Camera maths with no game dependency, so it can be tested without Tekken 8.
// Unreal conventions: X forward, Y right, Z up, angles in degrees, FVector and FRotator hold doubles.
#include <algorithm>
#include <cmath>
#include <vector>

namespace cine {

constexpr double Pi = 3.14159265358979323846;

struct Vec3 {
    double X = 0, Y = 0, Z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.X + b.X, a.Y + b.Y, a.Z + b.Z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.X - b.X, a.Y - b.Y, a.Z - b.Z }; }
inline Vec3 operator*(Vec3 a, double s) { return { a.X * s, a.Y * s, a.Z * s }; }
inline double Dot(Vec3 a, Vec3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Lerp(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }

struct Rot {
    double Pitch = 0, Yaw = 0, Roll = 0;
};

struct Quat {
    double X = 0, Y = 0, Z = 0, W = 1;
};
inline double Dot(Quat a, Quat b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W; }

inline double WrapAngle(double angle)
{
    angle = std::fmod(angle, 360.0);
    if (angle < 0)
        angle += 360.0;
    return angle > 180.0 ? angle - 360.0 : angle;
}

// TRotator::Quaternion
inline Quat ToQuat(Rot r)
{
    const double half = Pi / 360.0;
    const double sp = std::sin(r.Pitch * half), cp = std::cos(r.Pitch * half);
    const double sy = std::sin(r.Yaw * half), cy = std::cos(r.Yaw * half);
    const double sr = std::sin(r.Roll * half), cr = std::cos(r.Roll * half);
    return { cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy,
             cr * cp * cy + sr * sp * sy };
}

// TQuat::Rotator
inline Rot ToRot(Quat q)
{
    const double toDeg = 180.0 / Pi;
    const double test = q.Z * q.X - q.W * q.Y;
    const double yawY = 2.0 * (q.W * q.Z + q.X * q.Y);
    const double yawX = 1.0 - 2.0 * (q.Y * q.Y + q.Z * q.Z);
    Rot r;
    if (test < -0.4999995) {
        r.Pitch = -90.0;
        r.Yaw = std::atan2(yawY, yawX) * toDeg;
        r.Roll = WrapAngle(-r.Yaw - 2.0 * std::atan2(q.X, q.W) * toDeg);
    } else if (test > 0.4999995) {
        r.Pitch = 90.0;
        r.Yaw = std::atan2(yawY, yawX) * toDeg;
        r.Roll = WrapAngle(r.Yaw - 2.0 * std::atan2(q.X, q.W) * toDeg);
    } else {
        r.Pitch = std::asin(std::clamp(2.0 * test, -1.0, 1.0)) * toDeg;
        r.Yaw = std::atan2(yawY, yawX) * toDeg;
        r.Roll = std::atan2(-2.0 * (q.W * q.X + q.Y * q.Z), 1.0 - 2.0 * (q.X * q.X + q.Y * q.Y)) * toDeg;
    }
    return r;
}

inline Quat Normalized(Quat q)
{
    const double size = std::sqrt(Dot(q, q));
    if (size < 1e-12)
        return {};
    return { q.X / size, q.Y / size, q.Z / size, q.W / size };
}

inline Quat Slerp(Quat a, Quat b, double t)
{
    double cosom = Dot(a, b);
    const double sign = cosom < 0 ? -1.0 : 1.0;
    cosom *= sign;
    double s0 = 1.0 - t, s1 = t;
    if (cosom < 0.9999) {
        const double omega = std::acos(cosom);
        const double inv = 1.0 / std::sin(omega);
        s0 = std::sin((1.0 - t) * omega) * inv;
        s1 = std::sin(t * omega) * inv;
    }
    s1 *= sign;
    return Normalized({ s0 * a.X + s1 * b.X, s0 * a.Y + s1 * b.Y, s0 * a.Z + s1 * b.Z, s0 * a.W + s1 * b.W });
}

inline Rot RotLerp(Rot a, Rot b, double t) { return ToRot(Slerp(ToQuat(a), ToQuat(b), t)); }

inline Quat Multiply(Quat a, Quat b)
{
    return { a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y, a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
             a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W, a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z };
}
inline Quat Conjugate(Quat q) { return { -q.X, -q.Y, -q.Z, q.W }; }

// TQuat::RotateVector
inline Vec3 RotateVector(Quat q, Vec3 v)
{
    const Vec3 axis { q.X, q.Y, q.Z };
    auto cross = [](Vec3 a, Vec3 b) { return Vec3 { a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
    const Vec3 t = cross(axis, v) * 2.0;
    return v + t * q.W + cross(axis, t);
}

// A point in the world as the camera sees it: X ahead, Y right, Z up.
inline Vec3 ToCameraSpace(Vec3 cameraLocation, Rot cameraRotation, Vec3 point)
{
    return RotateVector(Conjugate(ToQuat(cameraRotation)), point - cameraLocation);
}

// A point in the world on a picture of the given width / height, for a camera whose field of view is across the width, as Unreal does.
// x goes from -1 at the left edge to 1 at the right, y from -1 at the bottom to 1 at the top. False when the point is behind the camera.
inline bool ProjectPoint(Vec3 cameraLocation, Rot cameraRotation, double fov, double aspect, Vec3 point, double& x, double& y)
{
    const Vec3 local = ToCameraSpace(cameraLocation, cameraRotation, point);
    if (local.X < 1.0)
        return false;
    const double half = std::tan(std::clamp(fov, 1.0, 170.0) * Pi / 360.0);
    x = local.Y / (local.X * half);
    y = local.Z / (local.X * half) * aspect;
    return true;
}

struct CameraKey {
    double Time = 0; // seconds on the path's clock
    Vec3 Location;
    Rot Rotation;
    double Fov = 45;
};

struct CameraSample {
    Vec3 Location;
    Rot Rotation;
    double Fov = 45;
};

inline double HermiteTangent(double p0, double p1, double p2, double t0, double t1, double t2)
{
    const double d0 = (p1 - p0) / std::max(t1 - t0, 1e-6);
    const double d1 = (p2 - p1) / std::max(t2 - t1, 1e-6);
    // Weighted so unevenly spaced keys keep a steady speed through the key.
    const double w0 = t2 - t1, w1 = t1 - t0;
    return (w0 * d0 + w1 * d1) / std::max(w0 + w1, 1e-6);
}

inline double Hermite(double p0, double m0, double p1, double m1, double h, double u)
{
    const double u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * h * m1;
}

// Keys must be sorted by Time. Every channel uses a time-weighted Catmull-Rom spline, so the camera moves through every key with no
// kink. Yaw is counted past 180 instead of wrapping, so a turn of any size goes the way it was made. Outside the key range it holds the end key.
inline CameraSample SamplePath(const std::vector<CameraKey>& keys, double time)
{
    CameraSample out;
    if (keys.empty())
        return out;
    if (time <= keys.front().Time || keys.size() == 1)
        return { keys.front().Location, keys.front().Rotation, keys.front().Fov };
    if (time >= keys.back().Time)
        return { keys.back().Location, keys.back().Rotation, keys.back().Fov };

    size_t i = 0;
    while (i + 2 < keys.size() && time > keys[i + 1].Time)
        ++i;
    const CameraKey& k1 = keys[i];
    const CameraKey& k2 = keys[i + 1];
    const CameraKey& k0 = i > 0 ? keys[i - 1] : k1;
    const CameraKey& k3 = i + 2 < keys.size() ? keys[i + 2] : k2;
    const double h = std::max(k2.Time - k1.Time, 1e-6);
    const double u = std::clamp((time - k1.Time) / h, 0.0, 1.0);

    const auto channel = [&](double a, double b, double c, double d) {
        const double m1 = HermiteTangent(a, b, c, k0.Time, k1.Time, k2.Time);
        const double m2 = HermiteTangent(b, c, d, k1.Time, k2.Time, k3.Time);
        return Hermite(b, m1, c, m2, h, u);
    };
    out.Location = { channel(k0.Location.X, k1.Location.X, k2.Location.X, k3.Location.X),
                     channel(k0.Location.Y, k1.Location.Y, k2.Location.Y, k3.Location.Y),
                     channel(k0.Location.Z, k1.Location.Z, k2.Location.Z, k3.Location.Z) };
    out.Fov = channel(k0.Fov, k1.Fov, k2.Fov, k3.Fov);

    out.Rotation = { channel(k0.Rotation.Pitch, k1.Rotation.Pitch, k2.Rotation.Pitch, k3.Rotation.Pitch),
                     channel(k0.Rotation.Yaw, k1.Rotation.Yaw, k2.Rotation.Yaw, k3.Rotation.Yaw),
                     channel(k0.Rotation.Roll, k1.Rotation.Roll, k2.Rotation.Roll, k3.Rotation.Roll) };
    return out;
}

// A speed ramp: eases from one key's timescale to the next, working in log space so a ramp from 1x to 0.05x feels even.
struct SpeedKey {
    double Time = 0;
    double Speed = 1;
};

inline double SampleSpeed(const std::vector<SpeedKey>& keys, double time)
{
    if (keys.empty())
        return 1.0;
    if (time <= keys.front().Time)
        return keys.front().Speed;
    if (time >= keys.back().Time)
        return keys.back().Speed;
    size_t i = 0;
    while (i + 2 < keys.size() && time > keys[i + 1].Time)
        ++i;
    const double span = std::max(keys[i + 1].Time - keys[i].Time, 1e-6);
    const double u = std::clamp((time - keys[i].Time) / span, 0.0, 1.0);
    const double eased = u * u * (3 - 2 * u);
    const double a = std::log(std::max(keys[i].Speed, 1e-4)), b = std::log(std::max(keys[i + 1].Speed, 1e-4));
    return std::exp(a + (b - a) * eased);
}

// Plain straight-line version of the camera path, for the Linear interpolation option.
inline CameraSample SamplePathLinear(const std::vector<CameraKey>& keys, double time)
{
    CameraSample out;
    if (keys.empty())
        return out;
    if (time <= keys.front().Time || keys.size() == 1)
        return { keys.front().Location, keys.front().Rotation, keys.front().Fov };
    if (time >= keys.back().Time)
        return { keys.back().Location, keys.back().Rotation, keys.back().Fov };
    size_t i = 0;
    while (i + 2 < keys.size() && time > keys[i + 1].Time)
        ++i;
    const double u = std::clamp((time - keys[i].Time) / std::max(keys[i + 1].Time - keys[i].Time, 1e-6), 0.0, 1.0);
    out.Location = Lerp(keys[i].Location, keys[i + 1].Location, u);
    const Rot& a = keys[i].Rotation;
    const Rot& b = keys[i + 1].Rotation;
    out.Rotation = { a.Pitch + (b.Pitch - a.Pitch) * u, a.Yaw + (b.Yaw - a.Yaw) * u, a.Roll + (b.Roll - a.Roll) * u };
    out.Fov = keys[i].Fov + (keys[i + 1].Fov - keys[i].Fov) * u;
    return out;
}

// The CoD movie mods' kind of path: a plain Catmull-Rom through the keys that ignores their spacing in time, so each stretch between
// two keys takes its own speed.
inline CameraSample SamplePathCoD(const std::vector<CameraKey>& keys, double time)
{
    CameraSample out;
    if (keys.empty())
        return out;
    if (time <= keys.front().Time || keys.size() == 1)
        return { keys.front().Location, keys.front().Rotation, keys.front().Fov };
    if (time >= keys.back().Time)
        return { keys.back().Location, keys.back().Rotation, keys.back().Fov };
    size_t i = 0;
    while (i + 2 < keys.size() && time > keys[i + 1].Time)
        ++i;
    const CameraKey& k1 = keys[i];
    const CameraKey& k2 = keys[i + 1];
    const CameraKey& k0 = i > 0 ? keys[i - 1] : k1;
    const CameraKey& k3 = i + 2 < keys.size() ? keys[i + 2] : k2;
    const double u = std::clamp((time - k1.Time) / std::max(k2.Time - k1.Time, 1e-6), 0.0, 1.0);
    const auto channel = [&](double a, double b, double c, double d) { return Hermite(b, (c - a) * 0.5, c, (d - b) * 0.5, 1.0, u); };
    out.Location = { channel(k0.Location.X, k1.Location.X, k2.Location.X, k3.Location.X),
                     channel(k0.Location.Y, k1.Location.Y, k2.Location.Y, k3.Location.Y),
                     channel(k0.Location.Z, k1.Location.Z, k2.Location.Z, k3.Location.Z) };
    out.Rotation = { channel(k0.Rotation.Pitch, k1.Rotation.Pitch, k2.Rotation.Pitch, k3.Rotation.Pitch),
                     channel(k0.Rotation.Yaw, k1.Rotation.Yaw, k2.Rotation.Yaw, k3.Rotation.Yaw),
                     channel(k0.Rotation.Roll, k1.Rotation.Roll, k2.Rotation.Roll, k3.Rotation.Roll) };
    out.Fov = channel(k0.Fov, k1.Fov, k2.Fov, k3.Fov);
    return out;
}

enum class PathStyle { Smooth, Linear, CoD };

inline CameraSample SamplePathStyled(const std::vector<CameraKey>& keys, double time, PathStyle style)
{
    switch (style) {
    case PathStyle::Linear:
        return SamplePathLinear(keys, time);
    case PathStyle::CoD:
        return SamplePathCoD(keys, time);
    default:
        return SamplePath(keys, time);
    }
}

// Keys from a file that stored yaw as -180 to 180: each one takes the short way from the one before.
inline void CountYaw(std::vector<CameraKey>& keys)
{
    for (size_t i = 1; i < keys.size(); ++i)
        keys[i].Rotation.Yaw = keys[i - 1].Rotation.Yaw + WrapAngle(keys[i].Rotation.Yaw - keys[i - 1].Rotation.Yaw);
}

// The rotation that points a camera at a target.
inline Rot LookAt(Vec3 from, Vec3 target, double roll = 0)
{
    const Vec3 d = target - from;
    const double flat = std::sqrt(d.X * d.X + d.Y * d.Y);
    return { std::atan2(d.Z, flat) * 180.0 / Pi, std::atan2(d.Y, d.X) * 180.0 / Pi, roll };
}

// Flying camera axes. The roll is applied to the picture only, so W always flies where you look.
inline Vec3 ForwardOf(Rot r)
{
    const double p = r.Pitch * Pi / 180.0, y = r.Yaw * Pi / 180.0;
    return { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p) };
}
inline Vec3 RightOf(Rot r)
{
    const double y = r.Yaw * Pi / 180.0;
    return { -std::sin(y), std::cos(y), 0 };
}
inline Vec3 UpOf(Rot r)
{
    const double p = r.Pitch * Pi / 180.0, y = r.Yaw * Pi / 180.0;
    return { -std::sin(p) * std::cos(y), -std::sin(p) * std::sin(y), std::cos(p) };
}

// Where the camera sits relative to the two fighters: along the line between them, out to the side, and
// up. This is the same whichever way the pair has turned, so a normal gameplay camera holds it nearly
// constant and a cinematic cut shows up as a jump.
struct FighterFrame {
    Vec3 Mid;
    double LineYaw = 0;
    double Separation = 0;
};

inline FighterFrame MakeFighterFrame(Vec3 p1, Vec3 p2)
{
    FighterFrame frame;
    frame.Mid = Lerp(p1, p2, 0.5);
    const Vec3 line = p2 - p1;
    frame.LineYaw = std::atan2(line.Y, line.X) * 180.0 / Pi;
    frame.Separation = std::sqrt(line.X * line.X + line.Y * line.Y);
    return frame;
}

struct RelativeCamera {
    double Along = 0, Side = 0, Height = 0;
    double YawOffset = 0, Pitch = 0, Fov = 45;
};

inline RelativeCamera ToRelative(const FighterFrame& frame, Vec3 location, Rot rotation, double fov)
{
    const Vec3 offset = location - frame.Mid;
    const Rot flat { 0, frame.LineYaw, 0 };
    const Vec3 along = ForwardOf(flat), side = RightOf(flat);
    return { Dot(offset, along), Dot(offset, side), offset.Z, WrapAngle(rotation.Yaw - frame.LineYaw),
             rotation.Pitch, fov };
}

inline void FromRelative(const FighterFrame& frame, const RelativeCamera& rel, Vec3& location, Rot& rotation, double& fov)
{
    const Rot flat { 0, frame.LineYaw, 0 };
    const Vec3 along = ForwardOf(flat), side = RightOf(flat);
    location = frame.Mid + along * rel.Along + side * rel.Side;
    location.Z = frame.Mid.Z + rel.Height;
    rotation = { rel.Pitch, WrapAngle(frame.LineYaw + rel.YawOffset), 0 };
    fov = rel.Fov;
}

// True when the game's camera has left the normal fight framing, i.e. a rage art or heat smash cut.
inline bool IsCinematicJump(const RelativeCamera& normal, const RelativeCamera& now, double distanceLimit, double angleLimit)
{
    const double dx = now.Along - normal.Along, dy = now.Side - normal.Side, dz = now.Height - normal.Height;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > distanceLimit)
        return true;
    // The game also zooms for big hits with the camera nearly where it was.
    return std::fabs(WrapAngle(now.YawOffset - normal.YawOffset)) > angleLimit
        || std::fabs(now.Pitch - normal.Pitch) > angleLimit
        || std::fabs(now.Fov - normal.Fov) > 2.0;
}

// The horizontal field of view that shows a picture of another shape with the fighters where the game's 16:9 picture has them.
// fit 0 keeps the height of the 16:9 view, so the fighters are the same size and in the same place: a wider shape (21:9, 32:9)
// shows more at the sides, a portrait shape (9:16) is the middle of the 16:9 view. fit 1 keeps the 16:9 view's width, so a
// portrait picture still holds everything the 16:9 one did, smaller. In between goes smoothly from one to the other.
// Degrees in and out; the field of view is horizontal, as the game's is.
inline double FovForAspect(double fov16x9, double aspect, double fit = 0.0)
{
    constexpr double PiValue = 3.14159265358979323846, Wide = 16.0 / 9.0;
    if (!(fov16x9 > 0.0 && fov16x9 < 179.0) || !(aspect > 0.0))
        return fov16x9;
    fit = std::clamp(fit, 0.0, 1.0);
    const double factor = std::pow(aspect / Wide, 1.0 - fit);
    const double half = std::tan(fov16x9 * PiValue / 360.0) * factor;
    return std::clamp(std::atan(half) * 360.0 / PiValue, 0.5, 175.0);
}

// A game that widens its own view for wider screens: it multiplies the 16:9 view's width (in tangent) by the picture's aspect over
// 16:9, held between `lowest` and `highest`. This gives back the 16:9 view from the one such a game handed over for a picture of
// `pictureAspect`. Tekken 8 does this between 16:10 and 21:9 (measured).
inline double FovAt16x9(double fov, double pictureAspect, double lowest, double highest)
{
    constexpr double PiValue = 3.14159265358979323846, Wide = 16.0 / 9.0;
    if (!(fov > 0.0 && fov < 179.0) || !(pictureAspect > 0.0))
        return fov;
    const double factor = std::clamp(pictureAspect, lowest, highest) / Wide;
    return std::atan(std::tan(fov * PiValue / 360.0) / factor) * 360.0 / PiValue;
}

} // namespace cine
