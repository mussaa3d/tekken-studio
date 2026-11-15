#include "mod/camera.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "mod/effects.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/postfx.h"
#include "mod/scan.h"
#include "mod/ue.h"

namespace camera {

using cine::CameraKey;
using cine::Rot;
using cine::Vec3;

namespace {

Settings g_settings;

Vec3 g_flyLocation;
Rot g_flyRotation;
std::atomic<float> g_gameFov { 45.0f };
std::atomic<float> g_wantedFov { -1.0f };
View g_view;
std::mutex g_viewMutex;

// Smoothing of the game's own camera between 60 Hz steps.
struct {
    bool Valid = false, NewStep = false;
    Vec3 PreviousLocation, TargetLocation;
    Rot PreviousRotation, TargetRotation;
} g_gameCamera;

// The "normal fight framing" learned while no cinematic is on screen.
struct {
    bool HaveNormal = false;
    bool Holding = false;
    int QuietFrames = 0;
    cine::RelativeCamera Normal;
    double NormalSeparation = 0;
    double LineYaw = 0;
    bool HaveLine = false;
} g_filter;

std::mutex g_keysMutex;
std::vector<CameraKey> g_keys;
double g_pathRealStart = 0;

double RealSeconds()
{
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart / frequency;
}

bool Down(int key)
{
    return overlay::KeyDown(key);
}

bool GameHasFocus()
{
    return overlay::IsGameForeground();
}

// With the editor open, the mouse only flies the camera when the press started over the game view.
bool LookButtonDown()
{
    static bool held = false;
    const bool down = Down(VK_RBUTTON);
    if (!down)
        held = false;
    else if (!held && (!overlay::MenuOpen() || overlay::MouseOverGameView()))
        held = true;
    return held;
}

// Free camera controls. They run off real time so flying stays usable in slow motion and while paused.
void FlyInput()
{
    static double last = RealSeconds();
    const double now = RealSeconds();
    const double dt = std::clamp(now - last, 0.0, 0.1);
    last = now;
    // The camera flies only from the editor. With it closed, the keyboard and mouse belong to the game and the camera stays put.
    if (!GameHasFocus() || overlay::KeyboardTakenByUi() || !overlay::MenuOpen()) {
        overlay::TakeMouseWheel();
        return;
    }

    Settings& s = g_settings;
    double speed = s.FlySpeed;
    if (Down(VK_SHIFT))
        speed *= 4;
    if (Down(VK_CONTROL))
        speed *= 0.2;

    Vec3 move;
    const Vec3 forward = cine::ForwardOf(g_flyRotation), right = cine::RightOf(g_flyRotation);
    if (Down('W'))
        move = move + forward;
    if (Down('S'))
        move = move - forward;
    if (Down('D'))
        move = move + right;
    if (Down('A'))
        move = move - right;
    if (Down('E'))
        move = move + Vec3 { 0, 0, 1 };
    if (Down('Q'))
        move = move - Vec3 { 0, 0, 1 };
    g_flyLocation = g_flyLocation + move * (speed * dt);

    if (Down('Z') && !Down(VK_CONTROL)) // Ctrl+Z is undo
        s.Roll -= 40 * dt;
    if (Down('C'))
        s.Roll += 40 * dt;
    if (Down('R'))
        s.Roll = 0;
    if (Down(VK_OEM_PLUS))
        s.Fov = std::max(s.Fov - 20 * dt, 5.0);
    if (Down(VK_OEM_MINUS))
        s.Fov = std::min(s.Fov + 20 * dt, 130.0);

    // Hold the right mouse button to look around. Sensitivity follows the zoom.
    static bool looking = false;
    static POINT anchor {};
    if (s.LookInput == LookInput::Keyboard) {
        looking = false;
        double keySpeed = s.KeyLookSpeed * (s.Fov / 45.0) * dt;
        if (Down(VK_SHIFT))
            keySpeed *= 3;
        if (Down(VK_CONTROL))
            keySpeed *= 0.25;
        if (Down(VK_RIGHT))
            g_flyRotation.Yaw += keySpeed;
        if (Down(VK_LEFT))
            g_flyRotation.Yaw -= keySpeed;
        if (Down(VK_UP))
            g_flyRotation.Pitch = std::min(g_flyRotation.Pitch + keySpeed, 89.0);
        if (Down(VK_DOWN))
            g_flyRotation.Pitch = std::max(g_flyRotation.Pitch - keySpeed, -89.0);
    } else if (!overlay::MenuOpen() || LookButtonDown()) {
        POINT cursor;
        GetCursorPos(&cursor);
        if (looking) {
            const double scale = s.LookSpeed * (s.Fov / 45.0);
            // Keeps counting past 180, so a key can remember which way round and how far the camera turned.
            g_flyRotation.Yaw += (cursor.x - anchor.x) * scale;
            g_flyRotation.Pitch = std::clamp(g_flyRotation.Pitch - (cursor.y - anchor.y) * scale, -89.0, 89.0);
            SetCursorPos(anchor.x, anchor.y);
        } else {
            anchor = cursor;
            looking = true;
        }
    } else {
        looking = false;
    }

    // With the editor open, scrolling over its panels and timeline is theirs.
    const int wheel = overlay::TakeMouseWheel();
    if (wheel != 0 && (!overlay::MenuOpen() || overlay::MouseOverGameView())) {
        if (Down(VK_CONTROL))
            s.Fov = std::clamp(s.Fov - wheel * 2.0, 5.0, 130.0);
        else if (Down(VK_SHIFT))
            s.FlySpeed = std::clamp(s.FlySpeed * std::pow(1.15, wheel), 10.0, 5000.0);
        else
            s.Roll += wheel * 2.0;
    }
}

double g_shotEnd = 0;
bool g_playingShot = false;

AttachSettings g_attach;
std::vector<std::string> g_boneNames[2];
std::mutex g_bonesMutex;

struct CachedSocket {
    ue::FName Name;
    std::string Text;
    bool Valid = false;
} g_socket;

// The chosen bone's place in the world. The name is converted once, since the engine call isn't free.
bool BoneTransform(Vec3& location, Rot& rotation)
{
    void* fighter = game::Fighter(g_attach.Fighter);
    void* mesh = fighter ? ue::CharacterMesh(fighter) : nullptr;
    if (!mesh)
        return false;
    if (!g_attach.Bone[0]) {
        location = {};
        return ue::ActorLocation(fighter, location);
    }
    if (!g_socket.Valid || g_socket.Text != g_attach.Bone) {
        g_socket.Text = g_attach.Bone;
        g_socket.Name = ue::MakeName(std::wstring(g_socket.Text.begin(), g_socket.Text.end()));
        g_socket.Valid = true;
    }
    return ue::SocketTransform(mesh, g_socket.Name, location, rotation);
}

// Input for the orbit style: right mouse turns, wheel sets the distance.
void OrbitInput()
{
    static bool looking = false;
    static POINT anchor {};
    if (GameHasFocus() && LookButtonDown()) {
        POINT cursor;
        GetCursorPos(&cursor);
        if (looking) {
            // Counted past 180, like the free camera's yaw, so two look keys can hold a turn of any size around the bone.
            g_attach.OrbitYaw = static_cast<float>(g_attach.OrbitYaw + (cursor.x - anchor.x) * 0.2);
            g_attach.OrbitPitch = std::clamp(g_attach.OrbitPitch + (cursor.y - anchor.y) * 0.2f, -85.0f, 85.0f);
            SetCursorPos(anchor.x, anchor.y);
        } else {
            anchor = cursor;
            looking = true;
        }
    } else {
        looking = false;
    }
    const int wheel = overlay::TakeMouseWheel();
    if (wheel != 0)
        g_attach.OrbitDistance = std::clamp(g_attach.OrbitDistance * static_cast<float>(std::pow(0.9, wheel)), 10.0f, 3000.0f);
}

// The bone that holds the fighter's eyes. Names differ between rigs, so this goes by the shortest name with "head" in it.
std::string HeadBone(int fighter)
{
    std::lock_guard lock(g_bonesMutex);
    std::string best;
    for (const std::string& bone : g_boneNames[std::clamp(fighter, 0, 1)]) {
        std::string lower = bone;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("head") != std::string::npos && lower.find("end") == std::string::npos && (best.empty() || bone.size() < best.size()))
            best = bone;
    }
    return best;
}

// Hiding goes through every mesh the fighter owns, since hair and masks are meshes of their own.
struct HiddenHead {
    void* Fighter = nullptr;
    ue::FName Bone;
    std::vector<void*> Meshes;
} g_hiddenHead;

void ShowHead()
{
    static ue::FunctionRef unhide { "SkinnedMeshComponent", "UnHideBoneByName" };
    if (g_hiddenHead.Fighter && game::Fighter(0) != g_hiddenHead.Fighter && game::Fighter(1) != g_hiddenHead.Fighter)
        g_hiddenHead = {}; // that fight is over, and its meshes with it
    for (void* mesh : g_hiddenHead.Meshes)
        ue::Call(mesh, unhide, &g_hiddenHead.Bone);
    g_hiddenHead = {};
}

void HideHead(void* fighter, const std::string& bone)
{
    if (g_hiddenHead.Fighter == fighter)
        return;
    ShowHead();
    static ue::FunctionRef getOwner { "ActorComponent", "GetOwner" };
    static ue::FunctionRef hide { "SkinnedMeshComponent", "HideBoneByName" };
    g_hiddenHead.Fighter = fighter;
    g_hiddenHead.Bone = ue::MakeName(std::wstring(bone.begin(), bone.end()));
    struct {
        ue::FName Bone;
        uint8_t PhysicsOption;
    } params { g_hiddenHead.Bone, 0 };
    for (void* mesh : ue::AllObjectsOf(ue::FindClass("SkinnedMeshComponent"))) {
        struct {
            void* ReturnValue;
        } owner { nullptr };
        ue::Call(mesh, getOwner, &owner);
        if (owner.ReturnValue == fighter) {
            ue::Call(mesh, hide, &params);
            g_hiddenHead.Meshes.push_back(mesh);
        }
    }
    mod::Log("POV: hid %s on %zu meshes", bone.c_str(), g_hiddenHead.Meshes.size());
}

Vec3 RotateByQuat(cine::Quat q, Vec3 v)
{
    const cine::Quat r = cine::Multiply(cine::Multiply(q, { v.X, v.Y, v.Z, 0 }), cine::Conjugate(q));
    return { r.X, r.Y, r.Z };
}

void ApplyAttach(Vec3& location, Rot& rotation, float& fov)
{
    const bool pov = g_attach.Style == AttachStyle::Pov;
    void* povFighter = game::Fighter(g_attach.Fighter);
    if (pov && povFighter) {
        const std::string head = HeadBone(g_attach.Fighter);
        strncpy_s(g_attach.Bone, head.c_str(), _TRUNCATE);
        if (g_attach.PovHideHead && !head.empty())
            HideHead(povFighter, head);
        else
            ShowHead();
    } else {
        ShowHead();
    }

    Vec3 boneLocation;
    Rot boneRotation;
    if (!BoneTransform(boneLocation, boneRotation))
        return;

    Vec3 targetLocation = location;
    Rot targetRotation = rotation;
    switch (g_attach.Style) {
    case AttachStyle::Fixed: {
        const Vec3 offset = cine::ForwardOf(boneRotation) * g_attach.Offset[0] + cine::RightOf(boneRotation) * g_attach.Offset[1]
            + cine::UpOf(boneRotation) * g_attach.Offset[2];
        targetLocation = boneLocation + offset;
        const cine::Quat turn = cine::ToQuat({ g_attach.Turn[0], g_attach.Turn[1], g_attach.Turn[2] });
        targetRotation = cine::ToRot(cine::Normalized(cine::Multiply(cine::ToQuat(boneRotation), turn)));
        break;
    }
    case AttachStyle::Orbit: {
        OrbitInput();
        const Rot around { g_attach.OrbitPitch, g_attach.OrbitYaw, 0 };
        targetLocation = boneLocation - cine::ForwardOf(around) * g_attach.OrbitDistance;
        targetRotation = cine::LookAt(targetLocation, boneLocation, g_settings.Roll);
        break;
    }
    case AttachStyle::Watch:
        FlyInput();
        targetLocation = g_flyLocation;
        targetRotation = cine::LookAt(targetLocation, boneLocation, g_settings.Roll);
        break;
    case AttachStyle::Pov: {
        const cine::Quat head = cine::Normalized(cine::Multiply(cine::ToQuat(boneRotation), cine::ToQuat({ g_attach.PovTurn[0], g_attach.PovTurn[1], g_attach.PovTurn[2] })));
        targetLocation = boneLocation + RotateByQuat(head, { g_attach.PovOffset[0], g_attach.PovOffset[1], g_attach.PovOffset[2] });
        targetRotation = cine::ToRot(head);
        if (g_attach.PovLevel)
            targetRotation.Roll = 0;
        break;
    }
    }

    // Bones shake on hits; a little smoothing takes the edge off.
    static Vec3 smoothLocation;
    static Rot smoothRotation;
    static bool haveSmooth = false;
    const double keep = haveSmooth && g_attach.Style != AttachStyle::Watch ? std::clamp<double>(g_attach.Smooth, 0.0, 0.98) : 0.0;
    smoothLocation = cine::Lerp(targetLocation, smoothLocation, keep);
    smoothRotation = cine::RotLerp(targetRotation, smoothRotation, keep);
    haveSmooth = true;
    location = smoothLocation;
    rotation = smoothRotation;
    fov = pov ? g_attach.PovFov : static_cast<float>(g_settings.Fov);
}

// Picks the line direction closest to last frame's, so the camera stays on its side when the fighters
// swap places.
cine::FighterFrame StableFrame(Vec3 p1, Vec3 p2, Vec3 cameraLocation)
{
    cine::FighterFrame frame = cine::MakeFighterFrame(p1, p2);
    if (frame.Separation < 1.0 && g_filter.HaveLine) {
        frame.LineYaw = g_filter.LineYaw;
        return frame;
    }
    if (!g_filter.HaveLine) {
        // First sight of this fight: orient the frame so the camera sits on its positive side.
        const cine::RelativeCamera rel = cine::ToRelative(frame, cameraLocation, {}, 0);
        if (rel.Side < 0)
            frame.LineYaw = cine::WrapAngle(frame.LineYaw + 180.0);
    } else if (std::fabs(cine::WrapAngle(frame.LineYaw - g_filter.LineYaw)) > 90.0) {
        frame.LineYaw = cine::WrapAngle(frame.LineYaw + 180.0);
    }
    g_filter.LineYaw = frame.LineYaw;
    g_filter.HaveLine = true;
    return frame;
}

void BlendRelative(cine::RelativeCamera& a, const cine::RelativeCamera& b, double t)
{
    a.Along += (b.Along - a.Along) * t;
    a.Side += (b.Side - a.Side) * t;
    a.Height += (b.Height - a.Height) * t;
    a.YawOffset = cine::WrapAngle(a.YawOffset + cine::WrapAngle(b.YawOffset - a.YawOffset) * t);
    a.Pitch += (b.Pitch - a.Pitch) * t;
    a.Fov += (b.Fov - a.Fov) * t;
}

void ApplyFilter(Vec3& location, Rot& rotation, float& fov)
{
    const Settings& s = g_settings;
    Vec3 p1, p2;
    if (s.Filter == Filter::Off || !game::FighterLocation(0, p1) || !game::FighterLocation(1, p2)) {
        g_filter.Holding = false;
        return;
    }

    const cine::FighterFrame frame = StableFrame(p1, p2, location);
    cine::RelativeCamera now = cine::ToRelative(frame, location, rotation, fov);
    // Compare at equal separation: the normal camera backs off as the fighters part.
    cine::RelativeCamera expected = g_filter.Normal;
    expected.Side += (frame.Separation - g_filter.NormalSeparation) * s.SeparationZoom;

    // The normal framing has to be learned from the normal camera. Starting from a zoom or a cinematic would mean holding that instead.
    if (!g_filter.HaveNormal) {
        const bool normalLens = std::fabs(now.Fov - s.NormalFov) < 1.0;
        if (!normalLens || effects::DramaPlaying() || !game::Hooks().Tick)
            return;
        g_filter.Normal = now;
        g_filter.NormalSeparation = frame.Separation;
        g_filter.HaveNormal = true;
        mod::Log("Camera filter: learned the fight framing at %.2f", game::Clock());
        return;
    }

    // The game saying a cinematic is on is the sure sign; the jump catches camera moves it does outside one.
    const bool jump = effects::DramaPlaying() || cine::IsCinematicJump(expected, now, s.JumpDistance, s.JumpAngle);
    if (!jump) {
        // Stay "held" for a moment after the camera returns, so a cut back and forth doesn't flicker.
        g_filter.QuietFrames = g_filter.Holding ? g_filter.QuietFrames + 1 : 30;
        if (g_filter.QuietFrames >= 30) {
            if (g_filter.Holding)
                mod::Log("Camera filter: back to the game's camera at %.2f", game::Clock());
            g_filter.Holding = false;
            // Learn slowly: a cinematic that eases in must not drag the baseline along with it.
            BlendRelative(g_filter.Normal, now, 0.005);
            g_filter.NormalSeparation += (frame.Separation - g_filter.NormalSeparation) * 0.005;
        }
    } else {
        if (!g_filter.Holding)
            mod::Log("Camera filter: holding the fight framing from %.2f", game::Clock());
        g_filter.Holding = true;
        g_filter.QuietFrames = 0;
    }

    if (g_filter.Holding || s.Filter == Filter::Classic) {
        double outFov = fov;
        cine::FromRelative(frame, expected, location, rotation, outFov);
        fov = static_cast<float>(outFov);
    }
}

} // namespace

Settings& Config()
{
    return g_settings;
}

void MarkNewStep()
{
    g_gameCamera.NewStep = true;
}

float ProjectionFov(float gameFov)
{
    if (gameFov > 1.0f && gameFov < 170.0f)
        g_gameFov.store(gameFov, std::memory_order_relaxed);
    const float wanted = g_wantedFov.load(std::memory_order_relaxed);
    return wanted > 0 ? wanted : gameFov;
}

void OnFightJump()
{
    g_gameCamera = {};
    g_filter.Holding = false;
    g_filter.QuietFrames = 30;
}

void OnFightFound()
{
    g_gameCamera = {};
    g_filter = {};
    g_socket = {};
    std::lock_guard lock(g_bonesMutex);
    for (int side = 0; side < 2; ++side) {
        void* fighter = game::Fighter(side);
        g_boneNames[side] = fighter ? ue::BoneNames(ue::CharacterMesh(fighter)) : std::vector<std::string> {};
        mod::Log("Fighter %d has %zu bones", side + 1, g_boneNames[side].size());
    }
}

AttachSettings& Attach()
{
    return g_attach;
}

std::vector<std::string> Bones(int fighter)
{
    std::lock_guard lock(g_bonesMutex);
    return g_boneNames[std::clamp(fighter, 0, 1)];
}

void OnCameraUpdate(void* manager)
{
    if (!scan::IsReadable(manager, ue::off::PovFov + 8))
        return;
    Vec3& location = ue::Field<Vec3>(manager, ue::off::PovLocation);
    Rot& rotation = ue::Field<Rot>(manager, ue::off::PovRotation);
    float fov = g_gameFov.load(std::memory_order_relaxed);

    // A fighter's mesh can load after the fight is found.
    static int boneRetry = 0;
    if (++boneRetry >= 60) {
        boneRetry = 0;
        std::lock_guard lock(g_bonesMutex);
        for (int side = 0; side < 2; ++side) {
            void* fighter = game::Fighter(side);
            if (fighter && g_boneNames[side].empty())
                g_boneNames[side] = ue::BoneNames(ue::CharacterMesh(fighter));
        }
    }

    if (game::Time().Interpolate && !ue::IsGamePaused()) {
        if (g_gameCamera.NewStep || !g_gameCamera.Valid || game::Seeking()) {
            if (!g_gameCamera.Valid || game::Seeking()) {
                g_gameCamera.PreviousLocation = location;
                g_gameCamera.PreviousRotation = rotation;
            } else {
                g_gameCamera.PreviousLocation = g_gameCamera.TargetLocation;
                g_gameCamera.PreviousRotation = g_gameCamera.TargetRotation;
            }
            g_gameCamera.TargetLocation = location;
            g_gameCamera.TargetRotation = rotation;
            g_gameCamera.Valid = true;
            g_gameCamera.NewStep = false;
        }
        const double alpha = game::StepAlpha();
        location = cine::Lerp(g_gameCamera.PreviousLocation, g_gameCamera.TargetLocation, alpha);
        rotation = cine::RotLerp(g_gameCamera.PreviousRotation, g_gameCamera.TargetRotation, alpha);
    }

    ApplyFilter(location, rotation, fov);

    if (g_playingShot && !game::Seeking() && game::Clock() >= g_shotEnd) {
        g_playingShot = false;
        game::Time().Paused = true;
    }

    Settings& s = g_settings;
    if (s.Mode == Mode::Free) {
        FlyInput();
        location = g_flyLocation;
        rotation = { g_flyRotation.Pitch, g_flyRotation.Yaw, s.Roll };
        fov = static_cast<float>(s.Fov);
    } else if (s.Mode == Mode::Dolly) {
        std::lock_guard lock(g_keysMutex);
        if (!g_keys.empty()) {
            const cine::CameraSample sample = cine::SamplePathStyled(g_keys, PathClock(), s.PathStyle);
            location = sample.Location;
            rotation = sample.Rotation;
            fov = static_cast<float>(sample.Fov);
        }
    } else if (s.Mode == Mode::Attach) {
        ApplyAttach(location, rotation, fov);
    } else if (s.FovOverride) {
        fov = static_cast<float>(s.Fov);
    }
    if (s.Mode != Mode::Attach)
        ShowHead();

    postfx::Apply(manager, location);

    const bool ours = s.Mode != Mode::Game || s.FovOverride || g_filter.Holding || s.Filter == Filter::Classic;
    g_wantedFov.store(ours ? fov : -1.0f, std::memory_order_relaxed);

    {
        std::lock_guard lock(g_viewMutex);
        g_view = { location, rotation, fov, true };
    }
    // The view keeps the full count of turns for new keys. The engine only needs to know where the camera points.
    rotation.Yaw = cine::WrapAngle(rotation.Yaw);
}

void ToggleLookInput()
{
    Settings& s = g_settings;
    s.LookInput = s.LookInput == LookInput::Mouse ? LookInput::Keyboard : LookInput::Mouse;
    mod::Log("Free camera looks with the %s", s.LookInput == LookInput::Mouse ? "mouse" : "keyboard");
}

void ToggleFree()
{
    Settings& s = g_settings;
    if (s.Mode == Mode::Free) {
        s.Mode = Mode::Game;
        return;
    }
    const View view = LastView();
    if (view.Valid) {
        g_flyLocation = view.Location;
        g_flyRotation = { view.Rotation.Pitch, g_flyRotation.Yaw + cine::WrapAngle(view.Rotation.Yaw - g_flyRotation.Yaw), 0 };
        s.Roll = view.Rotation.Roll;
        s.Fov = view.Fov;
    }
    s.Mode = Mode::Free;
}

void SetFreePose(Vec3 location, Rot rotation, double fov)
{
    g_flyLocation = location;
    g_flyRotation = { rotation.Pitch, rotation.Yaw, 0 };
    g_settings.Roll = rotation.Roll;
    g_settings.Fov = fov;
    g_settings.Mode = Mode::Free;
}

void ToggleDolly()
{
    Settings& s = g_settings;
    if (s.Mode == Mode::Dolly) {
        s.Mode = Mode::Game;
        return;
    }
    RestartPathClock();
    s.Mode = Mode::Dolly;
}

bool WantsKeyboard()
{
    return g_settings.Mode == Mode::Free;
}

View LastView()
{
    std::lock_guard lock(g_viewMutex);
    return g_view;
}

bool InCinematicHold()
{
    return g_filter.Holding;
}

std::vector<CameraKey> Keys()
{
    std::lock_guard lock(g_keysMutex);
    return g_keys;
}

void AddKeyAtCurrentView()
{
    const View view = LastView();
    if (!view.Valid)
        return;
    CameraKey key { PathClock(), view.Location, view.Rotation, view.Fov };
    std::lock_guard lock(g_keysMutex);
    const auto at = std::lower_bound(g_keys.begin(), g_keys.end(), key.Time, [](const CameraKey& k, double t) { return k.Time < t; });
    // The free camera's yaw is already counted. Any other camera's is the game's, so it takes the short way from its neighbour.
    if (g_settings.Mode != Mode::Free && !g_keys.empty()) {
        const double neighbour = at != g_keys.begin() ? (at - 1)->Rotation.Yaw : at->Rotation.Yaw;
        key.Rotation.Yaw = neighbour + cine::WrapAngle(key.Rotation.Yaw - neighbour);
    }
    if (at != g_keys.end() && std::fabs(at->Time - key.Time) < 0.001)
        *at = key;
    else
        g_keys.insert(at, key);
}

void RemoveKey(size_t index)
{
    std::lock_guard lock(g_keysMutex);
    if (index < g_keys.size())
        g_keys.erase(g_keys.begin() + index);
}

void SetKeys(std::vector<CameraKey> keys)
{
    std::sort(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.Time < b.Time; });
    std::lock_guard lock(g_keysMutex);
    g_keys = std::move(keys);
}

size_t MoveKey(size_t index, double time)
{
    std::lock_guard lock(g_keysMutex);
    if (index >= g_keys.size())
        return index;
    CameraKey key = g_keys[index];
    key.Time = time;
    g_keys.erase(g_keys.begin() + index);
    const auto at = std::lower_bound(g_keys.begin(), g_keys.end(), time, [](const CameraKey& k, double t) { return k.Time < t; });
    const size_t position = static_cast<size_t>(at - g_keys.begin());
    g_keys.insert(at, key);
    return position;
}

size_t SetKey(size_t index, const CameraKey& key)
{
    std::lock_guard lock(g_keysMutex);
    if (index >= g_keys.size())
        return index;
    g_keys.erase(g_keys.begin() + index);
    const auto at = std::lower_bound(g_keys.begin(), g_keys.end(), key.Time, [](const CameraKey& k, double t) { return k.Time < t; });
    const size_t position = static_cast<size_t>(at - g_keys.begin());
    g_keys.insert(at, key);
    return position;
}

void KeyFromView(size_t index)
{
    const View view = LastView();
    if (!view.Valid)
        return;
    std::lock_guard lock(g_keysMutex);
    if (index >= g_keys.size())
        return;
    CameraKey& key = g_keys[index];
    key.Location = view.Location;
    // The yaw is counted, so take the short way from what the key had.
    key.Rotation = { view.Rotation.Pitch, key.Rotation.Yaw + cine::WrapAngle(view.Rotation.Yaw - key.Rotation.Yaw), view.Rotation.Roll };
    key.Fov = view.Fov;
}

void ClearKeys()
{
    std::lock_guard lock(g_keysMutex);
    g_keys.clear();
}

void GoToKey(size_t index)
{
    std::lock_guard lock(g_keysMutex);
    if (index >= g_keys.size())
        return;
    g_flyLocation = g_keys[index].Location;
    g_flyRotation = { g_keys[index].Rotation.Pitch, g_keys[index].Rotation.Yaw, 0 };
    g_settings.Roll = g_keys[index].Rotation.Roll;
    g_settings.Fov = g_keys[index].Fov;
    g_settings.Mode = Mode::Free;
}

namespace {
std::filesystem::path PathsDir()
{
    std::filesystem::path dir = mod::DataDir() / "paths";
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    return dir;
}
} // namespace

bool SavePath(const std::string& name)
{
    if (!mod::SafeFileName(name))
        return false;
    std::ofstream file(PathsDir() / (name + ".t8path"), std::ios::trunc);
    if (!file)
        return false;
    file << "# time x y z pitch counted-yaw roll fov\n";
    file.precision(10);
    for (const CameraKey& key : Keys())
        file << key.Time << ' ' << key.Location.X << ' ' << key.Location.Y << ' ' << key.Location.Z << ' ' << key.Rotation.Pitch
             << ' ' << key.Rotation.Yaw << ' ' << key.Rotation.Roll << ' ' << key.Fov << '\n';
    return true;
}

bool LoadPath(const std::string& name)
{
    if (!mod::SafeFileName(name))
        return false;
    std::ifstream file(PathsDir() / (name + ".t8path"));
    if (!file)
        return false;
    std::vector<CameraKey> keys;
    std::string line;
    bool counted = false;
    while (std::getline(file, line)) {
        if (line.find("counted-yaw") != std::string::npos)
            counted = true;
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        CameraKey key;
        if (row >> key.Time >> key.Location.X >> key.Location.Y >> key.Location.Z >> key.Rotation.Pitch >> key.Rotation.Yaw
            >> key.Rotation.Roll >> key.Fov)
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.Time < b.Time; });
    if (!counted)
        cine::CountYaw(keys);
    std::lock_guard lock(g_keysMutex);
    g_keys = std::move(keys);
    return true;
}

std::vector<std::string> SavedPaths()
{
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(PathsDir(), ignored))
        if (entry.path().extension() == ".t8path")
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

double PathClock()
{
    if (g_settings.PathOnRealTime)
        return RealSeconds() - g_pathRealStart;
    return game::Clock();
}

void RestartPathClock()
{
    g_pathRealStart = RealSeconds();
}

void PlayShot()
{
    const std::vector<CameraKey> keys = Keys();
    if (keys.empty())
        return;
    RestartPathClock();
    g_settings.Mode = Mode::Dolly;
    game::SeekTo(keys.front().Time);
    game::Time().Paused = false;
    g_shotEnd = keys.back().Time;
    g_playingShot = true;
}

bool PlayingShot()
{
    return g_playingShot;
}

bool ExportCamIo(const std::string& name, double fps)
{
    const std::vector<CameraKey> keys = Keys();
    if (keys.size() < 2 || fps <= 0)
        return false;
    std::ofstream file(PathsDir() / (name + ".cam"), std::ios::trunc);
    if (!file)
        return false;
    file << "advancedfx Cam\nversion 1\nscaleFov none\nchannels time xPosition yPosition zPosition xRotation yRotation zRotation fov\nDATA\n";
    file.precision(8);
    const double start = keys.front().Time, end = keys.back().Time;
    for (double t = start; t <= end; t += 1.0 / fps) {
        const cine::CameraSample sample = cine::SamplePathStyled(keys, t, g_settings.PathStyle);
        // CamIO: xRotation = roll, yRotation = pitch, zRotation = yaw (Source convention, Y flipped from Unreal).
        file << (t - start) << ' ' << sample.Location.X << ' ' << -sample.Location.Y << ' ' << sample.Location.Z << ' '
             << sample.Rotation.Roll << ' ' << -sample.Rotation.Pitch << ' ' << -sample.Rotation.Yaw << ' ' << sample.Fov << '\n';
    }
    return true;
}

} // namespace camera
