// The game's camera. TTT2 keeps up to twelve camera records in a manager and marks one as the one in use. A record holds where the
// camera is, what it looks at and its lens:
//
//   [0x82E6B05C] -> the game system; +724 -> the camera object; +436 -> the manager; +316 = the slot in use;
//   [manager + 340 + slot * 4] -> the slot's block; +16 = the record: eye xyz (+0), target xyz (+16), field of view in degrees (+56)
//
// Read from the game's own getters (sub_82565678, sub_825468D0, sub_82542618, sub_825426C0, sub_825427C8), on BlueSpark release 23.
// The game writes the record for each fight frame it runs (sub_82541878), and the studio's view goes over it right after. The picture's
// view is made from the record only on a frame the fight runs: while BlueSpark holds the fight, a changed record changes nothing on
// screen (measured), which is why the studio lets the fight creep while its camera is moved during a pause (time.cpp).
#include "camera.h"

#include "studio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

#include <rex/ppc/context.h>

namespace studio::camera {

namespace {
constexpr uint32_t GameSystemHolder = 0x82E6B05C;
constexpr uint32_t CameraObject = 724, Manager = 436, SlotInUse = 316, Slots = 340, SlotCount = 12, RecordInBlock = 16;
constexpr float Pi = 3.14159265358979f;

Settings g_settings;
std::vector<Key> g_keys;
std::mutex g_mutex;      // the keys and the last view, which the editor and the game thread share
View g_last;             // what went to the picture last
bool g_haveLast = false;
std::atomic<uint64_t> g_writes { 0 };

float Radians(float degrees) { return degrees * Pi / 180.0f; }
float Degrees(float radians) { return radians * 180.0f / Pi; }

std::filesystem::path PathsDir()
{
    const std::filesystem::path dir = DataDir() / "paths";
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    return dir;
}

bool SafeName(const std::string& name)
{
    return !name.empty() && name.size() < 64 && name.find_first_of("\\/:*?\"<>|.") == std::string::npos;
}
} // namespace

uint32_t Record()
{
    if (!Readable(GameSystemHolder, 4))
        return 0;
    const uint32_t system = ReadU32(GameSystemHolder);
    if (!system || !Readable(system + CameraObject, 4))
        return 0;
    const uint32_t object = ReadU32(system + CameraObject);
    if (!object || !Readable(object + Manager, 4))
        return 0;
    const uint32_t manager = ReadU32(object + Manager);
    if (!manager || !Readable(manager + SlotInUse, 4))
        return 0;
    const uint32_t slot = ReadU32(manager + SlotInUse);
    if (slot >= SlotCount || !Readable(manager + Slots + slot * 4, 4))
        return 0;
    const uint32_t block = ReadU32(manager + Slots + slot * 4);
    if (!block || !Readable(block + RecordInBlock, 64))
        return 0;
    return block + RecordInBlock;
}

bool Read(View& out)
{
    const uint32_t record = Record();
    if (!record)
        return false;
    out.Eye = { ReadFloat(record + 0), ReadFloat(record + 4), ReadFloat(record + 8) };
    out.Target = { ReadFloat(record + 16), ReadFloat(record + 20), ReadFloat(record + 24) };
    out.Fov = ReadFloat(record + 56);
    // A record the game hasn't filled (a menu's) can hold anything.
    return std::isfinite(out.Eye.X) && std::isfinite(out.Eye.Y) && std::isfinite(out.Eye.Z) && std::isfinite(out.Target.X) && out.Fov > 1 && out.Fov < 179;
}

uint64_t Writes()
{
    return g_writes.load();
}

View ToView(const Pose& pose)
{
    const float yaw = Radians(pose.Yaw), pitch = Radians(std::clamp(pose.Pitch, -89.0f, 89.0f));
    const Vec3 direction { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
    View view;
    view.Eye = pose.Eye;
    // The game keeps the camera's distance to its target for effects, so the target sits where the fight's does: 4.6 m ahead.
    view.Target = { pose.Eye.X + direction.X * 4600, pose.Eye.Y + direction.Y * 4600, pose.Eye.Z + direction.Z * 4600 };
    view.Fov = std::clamp(pose.Fov, 5.0f, 150.0f);
    return view;
}

Pose ToPose(const View& view)
{
    const float dx = view.Target.X - view.Eye.X, dy = view.Target.Y - view.Eye.Y, dz = view.Target.Z - view.Eye.Z;
    const float flat = std::sqrt(dx * dx + dz * dz);
    Pose pose;
    pose.Eye = view.Eye;
    pose.Yaw = Degrees(std::atan2(dx, -dz));
    pose.Pitch = Degrees(std::atan2(dy, std::max(flat, 0.001f)));
    pose.Fov = view.Fov;
    return pose;
}

Settings& Config()
{
    return g_settings;
}

void SetMode(Mode mode)
{
    if (mode == Mode::Free && g_settings.Mode != Mode::Free) {
        // The free camera starts from where the picture is now.
        View now;
        if (Current(now))
            g_settings.Free = ToPose(now);
    }
    g_settings.Mode = mode;
}

std::atomic<float> g_gameFov { 0 };     // the game's own field of view, as it last wrote it
std::atomic<float> g_lastWritten { 0 }; // the field of view the studio last put in the record, 0 if none

// The game's function runs many times a frame and doesn't write the field of view every time, so what's in the record can be the
// studio's own value from a moment ago: only a value that isn't the studio's counts as the game's.
void WriteFov(uint32_t record, float fov)
{
    WriteFloat(record + 56, fov);
    g_lastWritten.store(fov, std::memory_order_relaxed);
}

bool Current(View& out)
{
    {
        std::lock_guard lock(g_mutex);
        if (g_haveLast && g_settings.Mode != Mode::Game) {
            out = g_last;
            return true;
        }
    }
    if (!Read(out))
        return false;
    // With a shape on, the record holds the changed field of view: the game's own is the one for 16:9.
    const float own = g_gameFov.load(std::memory_order_relaxed);
    if (g_settings.ShapeAspect > 0 && own > 1 && own < 179)
        out.Fov = own;
    return true;
}

bool Drawn(View& out)
{
    if (!Current(out))
        return false;
    out.Fov = ShapeFov(out.Fov);
    return true;
}

float ShapeFov(float fov)
{
    constexpr float Wide = 16.0f / 9.0f;
    const float aspect = g_settings.ShapeAspect;
    if (!(aspect > 0.1f) || std::fabs(aspect - Wide) < 0.005f || !(fov > 1 && fov < 179))
        return fov;
    // The field of view is across the width. Wider than 16:9: the cut keeps the full width and loses height, so the view widens
    // by the same share and the fighters keep their size against the cut's height. Narrower: the cut is the middle of the picture
    // as it is; Fit widens the view towards the whole 16:9 width.
    const float factor = aspect > Wide ? aspect / Wide : std::pow(Wide / aspect, std::clamp(g_settings.ShapeFit, 0.0f, 1.0f));
    const float half = std::tan(Radians(fov) * 0.5f) * factor;
    return std::clamp(std::atan(half) * 2.0f * 57.29577951f, 1.0f, 170.0f);
}

void ShapeRect(float width, float height, float& left, float& top, float& right, float& bottom)
{
    left = top = 0;
    right = width;
    bottom = height;
    const float aspect = g_settings.ShapeAspect;
    if (!(aspect > 0.1f) || width <= 0 || height <= 0)
        return;
    const float whole = width / height;
    if (aspect > whole) {
        const float keep = width / aspect;
        top = (height - keep) * 0.5f;
        bottom = top + keep;
    } else {
        const float keep = height * aspect;
        left = (width - keep) * 0.5f;
        right = left + keep;
    }
}

std::vector<Key>& Keys()
{
    return g_keys;
}

void AddKeyAtView(double time)
{
    View now;
    if (!Current(now))
        return;
    Key key { time, ToPose(now) };
    std::lock_guard lock(g_mutex);
    // Yaw is counted on from the key before, so a turn of more than half a circle goes the way it was made.
    const auto at = std::lower_bound(g_keys.begin(), g_keys.end(), time, [](const Key& k, double t) { return k.Time < t; });
    if (!g_keys.empty()) {
        const float neighbour = at != g_keys.begin() ? (at - 1)->Pose.Yaw : at->Pose.Yaw;
        float turn = std::fmod(key.Pose.Yaw - neighbour, 360.0f);
        if (turn > 180)
            turn -= 360;
        if (turn < -180)
            turn += 360;
        key.Pose.Yaw = neighbour + turn;
    }
    if (at != g_keys.end() && std::fabs(at->Time - time) < 0.001)
        *at = key;
    else
        g_keys.insert(at, key);
}

namespace {
// A time-weighted Catmull-Rom through the keys, as Tekken Studio's Smooth style for Tekken 8.
float Tangent(float p0, float p1, float p2, double t0, double t1, double t2)
{
    const double d0 = (p1 - p0) / std::max(t1 - t0, 1e-6), d1 = (p2 - p1) / std::max(t2 - t1, 1e-6);
    const double w0 = t2 - t1, w1 = t1 - t0;
    return static_cast<float>((w0 * d0 + w1 * d1) / std::max(w0 + w1, 1e-6));
}
float Hermite(float p0, float m0, float p1, float m1, double h, double u)
{
    const double u2 = u * u, u3 = u2 * u;
    return static_cast<float>((2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * h * m1);
}
} // namespace

Pose Sample(const std::vector<Key>& keys, double time, PathStyle style)
{
    if (keys.empty())
        return {};
    if (time <= keys.front().Time || keys.size() == 1)
        return keys.front().Pose;
    if (time >= keys.back().Time)
        return keys.back().Pose;
    size_t i = 1;
    while (i < keys.size() - 1 && keys[i].Time <= time)
        ++i;
    const Key& a = keys[i - 1];
    const Key& b = keys[i];
    const double h = std::max(b.Time - a.Time, 1e-6), u = (time - a.Time) / h;
    if (style == PathStyle::Linear) {
        const float t = static_cast<float>(u);
        auto mix = [t](float x, float y) { return x + (y - x) * t; };
        return { { mix(a.Pose.Eye.X, b.Pose.Eye.X), mix(a.Pose.Eye.Y, b.Pose.Eye.Y), mix(a.Pose.Eye.Z, b.Pose.Eye.Z) }, mix(a.Pose.Yaw, b.Pose.Yaw), mix(a.Pose.Pitch, b.Pose.Pitch),
                 mix(a.Pose.Fov, b.Pose.Fov) };
    }
    const Key& before = i >= 2 ? keys[i - 2] : a;
    const Key& after = i + 1 < keys.size() ? keys[i + 1] : b;
    auto channel = [&](float Pose::*field) {
        const float m0 = i >= 2 ? Tangent(before.Pose.*field, a.Pose.*field, b.Pose.*field, before.Time, a.Time, b.Time) : (b.Pose.*field - a.Pose.*field) / static_cast<float>(h);
        const float m1 = i + 1 < keys.size() ? Tangent(a.Pose.*field, b.Pose.*field, after.Pose.*field, a.Time, b.Time, after.Time) : (b.Pose.*field - a.Pose.*field) / static_cast<float>(h);
        return Hermite(a.Pose.*field, m0, b.Pose.*field, m1, h, u);
    };
    auto place = [&](float Vec3::*axis) {
        const float p0 = before.Pose.Eye.*axis, p1 = a.Pose.Eye.*axis, p2 = b.Pose.Eye.*axis, p3 = after.Pose.Eye.*axis;
        const float m0 = i >= 2 ? Tangent(p0, p1, p2, before.Time, a.Time, b.Time) : (p2 - p1) / static_cast<float>(h);
        const float m1 = i + 1 < keys.size() ? Tangent(p1, p2, p3, a.Time, b.Time, after.Time) : (p2 - p1) / static_cast<float>(h);
        return Hermite(p1, m0, p2, m1, h, u);
    };
    Pose out;
    out.Eye = { place(&Vec3::X), place(&Vec3::Y), place(&Vec3::Z) };
    out.Yaw = channel(&Pose::Yaw);
    out.Pitch = channel(&Pose::Pitch);
    out.Fov = channel(&Pose::Fov);
    return out;
}

bool Wanted(View& out)
{
    const Settings& s = g_settings;
    if (s.Mode == Mode::Game)
        return false;
    if (s.Mode == Mode::Free) {
        out = ToView(s.Free);
    } else {
        std::lock_guard lock(g_mutex);
        if (g_keys.empty())
            return false;
        out = ToView(Sample(g_keys, Clock(), s.Style));
    }
    std::lock_guard lock(g_mutex);
    g_last = out;
    g_haveLast = true;
    return true;
}

void Apply()
{
    const uint32_t record = Record();
    View game, v;
    // Only over a real camera: the fight's, a replay's. Never over a menu's empty record.
    if (!record || !Read(game))
        return;
    if (!Wanted(v)) {
        // The game's own camera: only its field of view, for a shape.
        const float own = g_gameFov.load(std::memory_order_relaxed);
        if (!(own > 1 && own < 179))
            return;
        if (g_settings.ShapeAspect > 0) {
            WriteFov(record, ShapeFov(own));
        } else if (g_lastWritten.load(std::memory_order_relaxed) > 0) {
            // The shape or a studio camera has just gone off: the game's own goes back, once.
            WriteFloat(record + 56, own);
            g_lastWritten.store(0, std::memory_order_relaxed);
        }
        return;
    }
    WriteFloat(record + 0, v.Eye.X);
    WriteFloat(record + 4, v.Eye.Y);
    WriteFloat(record + 8, v.Eye.Z);
    WriteFloat(record + 16, v.Target.X);
    WriteFloat(record + 20, v.Target.Y);
    WriteFloat(record + 24, v.Target.Z);
    WriteFov(record, ShapeFov(v.Fov));
}

// Right after the game has written its own camera: its field of view is kept before anything of the studio's goes over it.
void GameWrote()
{
    const uint32_t record = Record();
    if (!record)
        return;
    const float fov = ReadFloat(record + 56);
    const float ours = g_lastWritten.load(std::memory_order_relaxed);
    if (fov > 1 && fov < 179 && !(ours > 0 && std::fabs(fov - ours) < 0.0005f))
        g_gameFov.store(fov, std::memory_order_relaxed);
}

bool SavePath(const std::string& name)
{
    if (!SafeName(name))
        return false;
    std::ofstream file(PathsDir() / (name + ".ttt2path"), std::ios::trunc);
    if (!file)
        return false;
    file << "# time x y z yaw pitch fov\n";
    file.precision(9);
    std::lock_guard lock(g_mutex);
    for (const Key& key : g_keys)
        file << key.Time << ' ' << key.Pose.Eye.X << ' ' << key.Pose.Eye.Y << ' ' << key.Pose.Eye.Z << ' ' << key.Pose.Yaw << ' ' << key.Pose.Pitch << ' ' << key.Pose.Fov << '\n';
    return true;
}

bool LoadPath(const std::string& name)
{
    if (!SafeName(name))
        return false;
    std::ifstream file(PathsDir() / (name + ".ttt2path"));
    if (!file)
        return false;
    std::vector<Key> keys;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        Key key;
        if (row >> key.Time >> key.Pose.Eye.X >> key.Pose.Eye.Y >> key.Pose.Eye.Z >> key.Pose.Yaw >> key.Pose.Pitch >> key.Pose.Fov)
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.Time < b.Time; });
    std::lock_guard lock(g_mutex);
    g_keys = std::move(keys);
    return true;
}

std::vector<std::string> SavedPaths()
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(PathsDir(), ignored))
        if (entry.path().extension() == ".ttt2path")
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

bool Project(const View& view, float aspect, Vec3 point, float& x, float& y)
{
    // The camera's axes from eye and target, with the world's Y as up.
    Vec3 f { view.Target.X - view.Eye.X, view.Target.Y - view.Eye.Y, view.Target.Z - view.Eye.Z };
    const float length = std::sqrt(f.X * f.X + f.Y * f.Y + f.Z * f.Z);
    if (length < 0.001f)
        return false;
    f = { f.X / length, f.Y / length, f.Z / length };
    Vec3 r { -f.Z, 0, f.X }; // forward x up(0,1,0), so it points to the picture's right
    const float flat = std::sqrt(r.X * r.X + r.Z * r.Z);
    if (flat < 0.0001f)
        return false;
    r = { r.X / flat, 0, r.Z / flat };
    const Vec3 u { r.Y * f.Z - r.Z * f.Y, r.Z * f.X - r.X * f.Z, r.X * f.Y - r.Y * f.X };
    const Vec3 d { point.X - view.Eye.X, point.Y - view.Eye.Y, point.Z - view.Eye.Z };
    const float ahead = d.X * f.X + d.Y * f.Y + d.Z * f.Z;
    if (ahead < 1.0f)
        return false;
    const float side = d.X * r.X + d.Y * r.Y + d.Z * r.Z, up = d.X * u.X + d.Y * u.Y + d.Z * u.Z;
    // The game's field of view is across the picture's width (measured against the fighters' feet, see the mod log).
    const float half = std::tan(Radians(view.Fov) * 0.5f);
    x = 0.5f + 0.5f * side / (ahead * half);
    y = 0.5f - 0.5f * up * aspect / (ahead * half);
    return std::fabs(x) < 40 && std::fabs(y) < 40;
}

} // namespace studio::camera

// The game's function that writes the frame's camera into its record. BlueSpark doesn't hook it (checked against ttt2_ours.lib of
// release 23). The original runs first, then the studio's view goes over what it wrote.
extern "C" void __imp__sub_82541878(PPCContext& ctx, uint8_t* base);
namespace studio::camera {
std::atomic<int> g_stackWanted { 0 };
void LogStackOnce()
{
    g_stackWanted = 1;
}
} // namespace studio::camera

extern "C" void sub_82541878(PPCContext& ctx, uint8_t* base)
{
    // Research: the game's own call stack here, once. Each frame keeps the stack pointer of the one above at its start, and that
    // one's return address 8 bytes under it (the Xbox 360's rule).
    if (studio::camera::g_stackWanted.exchange(0)) {
        std::string line;
        char text[32];
        snprintf(text, sizeof(text), "%08X", static_cast<uint32_t>(ctx.lr));
        line = text;
        uint32_t sp = ctx.r1.u32;
        for (int depth = 0; depth < 40 && sp && studio::Readable(sp, 4); ++depth) {
            const uint32_t above = studio::ReadU32(sp);
            if (above <= sp || !studio::Readable(above - 8, 4))
                break;
            snprintf(text, sizeof(text), " %08X", studio::ReadU32(above - 8));
            line += text;
            sp = above;
        }
        studio::Log("stack at the camera write: %s", line.c_str());
    }
    __imp__sub_82541878(ctx, base);
    studio::camera::GameWrote();
    ++studio::camera::g_writes;
    studio::camera::Apply();
}
