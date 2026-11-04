#include "mod/game.h"

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include "common/config.h"
#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/guard.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/recorder.h"
#include "mod/replay.h"
#include "mod/scan.h"
#include "mod/testcmd.h"
#include "mod/ue.h"

namespace game {

using cine::Rot;
using cine::Vec3;
using namespace ue;

float** g_fpsVariable = nullptr;

namespace {

// UE5 FTransform: rotation, translation and scale, four doubles each.
struct BoneTransform {
    double Rotation[4];
    double Translation[4];
    double Scale[4];
};
static_assert(sizeof(BoneTransform) == 0x60);

using TickFn = void (*)(double);
using GameModeTickFn = void (*)(void*, float);
using FightStartFn = void (*)(void*, void*, void*);
using CameraFn = void (*)(void*, float);
using ActorFn = bool (*)(void*, Vec3, Rot, bool, void*, uint8_t);
using BoneFn = void (*)(void*, void*, void*, bool, TArray<BoneTransform>*, TArray<BoneTransform>*, Vec3*, void*, void*);
using ProjectionFn = void (*)(void*, uint8_t, void*, void*);

TickFn oTick = nullptr;
GameModeTickFn oGameModeTick = nullptr;
FightStartFn oFightStart = nullptr;
CameraFn oCamera = nullptr;
ActorFn oActor = nullptr;
BoneFn oBones = nullptr;
ProjectionFn oProjection = nullptr;

HookStatus g_hooks;
TimeControl g_time;
std::atomic<int> g_stepRequests { 0 };
std::atomic<bool> g_recording { false };
double g_recordingFps = 60;

double g_accumulator = 0;
std::atomic<double> g_alpha { 0 };
std::atomic<uint64_t> g_steps { 0 };
std::atomic<int64_t> g_position { 0 };
std::atomic<bool> g_positionMoved { true };
std::atomic<double> g_clockZero { 0 };
std::atomic<double> g_seekTarget { -1.0 };
constexpr int SettleAfterSeek = 4;
std::atomic<int> g_settleFrames { 0 };
int g_seekWaits = 0;
bool g_seekJumped = false;
bool g_seekToRound = false;
double g_lastRealDt = StepSeconds;
double g_lastAdvance = StepSeconds;

std::mutex g_speedMutex;
std::vector<cine::SpeedKey> g_speedKeys;

std::atomic<uint64_t> g_renderedFrames { 0 };

std::mutex g_taskMutex;
std::deque<std::function<void()>> g_tasks;

struct Track {
    bool Valid = false;
    bool NewStep = false;
    Vec3 PreviousLocation, TargetLocation;
    Rot PreviousRotation, TargetRotation;
};

struct BoneTrack {
    std::mutex Lock;
    std::atomic<bool> NewStep { false };
    std::vector<BoneTransform> Previous, Target;
};

std::array<void*, 2> g_fighters {};
std::array<uint32_t, 2> g_fighterIndex {};
std::array<std::atomic<void*>, 2> g_meshes {};
std::array<Track, 2> g_actorTracks;
std::array<BoneTrack, 2> g_boneTracks;
std::atomic<bool> g_fightersValid { false };
int g_fightCount = 0;

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

void Shift(Track& track, const Vec3& location, const Rot& rotation)
{
    if (!track.Valid) {
        track.PreviousLocation = track.TargetLocation = location;
        track.PreviousRotation = track.TargetRotation = rotation;
        track.Valid = true;
    } else {
        track.PreviousLocation = track.TargetLocation;
        track.PreviousRotation = track.TargetRotation;
        track.TargetLocation = location;
        track.TargetRotation = rotation;
    }
    track.NewStep = false;
}

// FTransform::Blend: normalized fast-lerp for rotation, straight lerp for the rest.
void NormalizeQuat(double q[4])
{
    const double size = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (size < 1e-8) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    const double scale = 1.0 / std::sqrt(size);
    for (int i = 0; i < 4; ++i)
        q[i] *= scale;
}

void BlendBone(const BoneTransform& a, const BoneTransform& b, double alpha, BoneTransform& out)
{
    const double dot = a.Rotation[0] * b.Rotation[0] + a.Rotation[1] * b.Rotation[1] + a.Rotation[2] * b.Rotation[2]
        + a.Rotation[3] * b.Rotation[3];
    const double bias = dot >= 0 ? 1.0 : -1.0;
    for (int i = 0; i < 4; ++i)
        out.Rotation[i] = b.Rotation[i] * alpha + a.Rotation[i] * (bias * (1.0 - alpha));
    NormalizeQuat(out.Rotation);
    for (int i = 0; i < 3; ++i) {
        out.Translation[i] = a.Translation[i] + (b.Translation[i] - a.Translation[i]) * alpha;
        out.Scale[i] = a.Scale[i] + (b.Scale[i] - a.Scale[i]) * alpha;
    }
    out.Translation[3] = a.Translation[3];
    out.Scale[3] = a.Scale[3];
}

// The replay jumped, so there's nothing to blend from.
void ForgetMotion()
{
    camera::OnFightJump();
    for (Track& track : g_actorTracks)
        track = {};
    for (BoneTrack& track : g_boneTracks) {
        std::lock_guard lock(track.Lock);
        track.NewStep = false;
        track.Previous.clear();
        track.Target.clear();
    }
}

// The timeline is the game's own frame count for the match, so a moment in a replay is always at the same place on the timeline,
// whatever was watched before it, and goes back when the replay goes back.
void FollowFight()
{
    const int64_t before = g_position.load(std::memory_order_relaxed);
    int round = 0;
    uint32_t frame = 0;
    const int64_t now = replay::Position(round, frame) ? static_cast<int64_t>(frame) : before + 1;
    g_position.store(now, std::memory_order_relaxed);
    g_positionMoved.store(now != before, std::memory_order_relaxed);
    if (now < before || now > before + 1)
        ForgetMotion();
}

void RunStep()
{
    oTick(StepSeconds);
    g_steps.fetch_add(1, std::memory_order_relaxed);
    FollowFight();
    camera::MarkNewStep();
    for (Track& track : g_actorTracks)
        track.NewStep = true;
    for (BoneTrack& track : g_boneTracks)
        track.NewStep = true;
}

void EndSeek()
{
    g_seekTarget = -1.0;
    replay::CancelJump();
    g_settleFrames = SettleAfterSeek;
}

// Going to a moment from anywhere: one jump to the last saved point before it, then running the fight up to the exact frame.
void RunSeek(double target)
{
    g_accumulator = 0;
    g_alpha.store(0, std::memory_order_relaxed);
    g_lastAdvance = 32 * StepSeconds;

    const int64_t wanted = std::llround((target + g_clockZero.load(std::memory_order_relaxed)) / StepSeconds);
    if (g_position.load(std::memory_order_relaxed) == wanted) {
        EndSeek();
        return;
    }

    // Waiting for the replay's input code to take the jump. It runs inside the game's step.
    if (replay::JumpPending()) {
        if (++g_seekWaits > 120) {
            mod::Log("Seek: the replay isn't taking jumps");
            EndSeek();
            return;
        }
        RunStep();
        return;
    }
    // The game's transition after a jump runs inside its step too, and the fight doesn't move until it's over.
    if (g_seekJumped && replay::JumpSettling()) {
        for (int i = 0; i < 32 && replay::JumpSettling(); ++i)
            RunStep();
        return;
    }

    const int64_t position = g_position.load(std::memory_order_relaxed);
    // Always by way of a saved point, even for a short hop ahead: what's on screen at a moment then doesn't depend on where you came from.
    if (replay::CanJump() && !g_seekJumped && wanted >= 0) {
        g_seekJumped = true;
        g_seekWaits = 0;
        replay::RequestJump(static_cast<uint32_t>(wanted), g_seekToRound);
        RunStep();
        return;
    }

    if (position > wanted) {
        if (!g_seekToRound)
            mod::Log("Seek: can't go further back than %lld (wanted %lld)", static_cast<long long>(position), static_cast<long long>(wanted));
        EndSeek();
        return;
    }
    for (int i = 0; i < 32 && g_position.load(std::memory_order_relaxed) < wanted; ++i)
        RunStep();
    // The game's own pause, or the end of the fight: the position stops moving.
    if (g_position.load(std::memory_order_relaxed) == position) {
        if (++g_seekWaits > 8) {
            mod::Log("Seek: the fight isn't moving, stopping at %lld", static_cast<long long>(position));
            EndSeek();
        }
    } else {
        g_seekWaits = 0;
    }
}

// The game only ever advances in fixed 60 Hz steps, so slow motion, pausing and frame-locked recording
// all come down to how many steps we let it run per rendered frame.
void DetourTick(double dt)
{
    if (guard::Tripped()) {
        oTick(dt);
        return;
    }

    static double last = RealSeconds();
    const double now = RealSeconds();
    const double real = std::clamp(now - last, 0.0, 0.25);
    last = now;
    g_lastRealDt = real;

    if (g_time.UseSpeedRamp) {
        std::lock_guard lock(g_speedMutex);
        if (!g_speedKeys.empty())
            g_time.Speed = std::clamp(cine::SampleSpeed(g_speedKeys, Clock()), 0.01, 4.0);
    }

    // Seeking overrides everything.
    const double seekTarget = g_seekTarget.load(std::memory_order_relaxed);
    if (seekTarget >= 0) {
        RunSeek(seekTarget);
        return;
    }
    // The picture on screen lags the game by a frame or two. Holding still after a seek means whatever comes next, like the
    // first frame of a recording, shows the moment we landed on and not the way there.
    if (g_settleFrames > 0) {
        --g_settleFrames;
        g_lastAdvance = 0;
        return;
    }

    double advance = 0;
    if (!g_time.Paused) {
        if (g_recording) {
            if (!recorder::WaitingForFirstFrame() && (!recorder::HoldsTime() || recorder::TakeAdvance()))
                advance = g_time.Speed / g_recordingFps;
        } else {
            advance = real * g_time.Speed;
        }
    }
    g_lastAdvance = advance;
    g_accumulator += advance;

    int steps = 0;
    while (g_accumulator >= StepSeconds) {
        g_accumulator -= StepSeconds;
        if (++steps <= 8) // after a long stall, catch up a little rather than fast-forwarding
            RunStep();
    }
    if (g_stepRequests.load(std::memory_order_relaxed) > 0) {
        g_stepRequests.fetch_sub(1, std::memory_order_relaxed);
        RunStep();
    }
    g_alpha.store(g_accumulator / StepSeconds, std::memory_order_relaxed);
}

void FindFighters()
{
    g_fighters = {};
    g_fightersValid = false;
    for (auto& mesh : g_meshes)
        mesh = nullptr;
    for (Track& track : g_actorTracks)
        track = {};

    void* cls = FindClass("PolarisCharacterActor");
    if (!cls)
        return;
    int found = 0;
    for (void* actor : AllObjectsOf(cls)) {
        if (!scan::IsReadable(actor, off::CharacterSide + 4))
            continue;
        const int side = Field<int>(actor, off::CharacterSide);
        if (side != 0 && side != 1)
            continue;
        g_fighters[side] = actor;
        g_fighterIndex[side] = Field<uint32_t>(actor, off::InternalIndex);
        ++found;

        BoneTrack& track = g_boneTracks[side];
        std::lock_guard lock(track.Lock);
        track.NewStep = false;
        track.Previous.clear();
        track.Target.clear();
        g_meshes[side] = CharacterMesh(actor);
    }
    if (g_fighters[0] && g_fighters[1]) {
        g_fightersValid = true;
        ++g_fightCount;
        mod::Log("Fight found: P1 %p, P2 %p", g_fighters[0], g_fighters[1]);
        camera::OnFightFound();
        effects::OnFightFound();
        dvars::OnFightFound();
        lights::OnFightFound();
    }
}

bool FightersStillAlive()
{
    return StillAlive(g_fighters[0], g_fighterIndex[0]) && StillAlive(g_fighters[1], g_fighterIndex[1]);
}

// A cloth solver straightens the cloth out once per simulated frame, however small the time step, so in slow motion or while paused it
// keeps moving at full speed long after the fight has stopped. So when things are slowed, cloth only gets a frame when the fight took a
// step. With the switch off, cloth holds its last pose rather than dropping back to the bare mesh.
constexpr double ClothFollowsStepsBelow = 0.75;

std::vector<std::pair<void*, uint32_t>> g_fighterMeshes;
int g_fighterMeshesFight = -1;

void FindFighterMeshes()
{
    g_fighterMeshes.clear();
    g_fighterMeshesFight = g_fightCount;
    static FunctionRef getOwner { "ActorComponent", "GetOwner" };
    void* cls = FindClass("SkeletalMeshComponent");
    if (!cls)
        return;
    for (void* mesh : AllObjectsOf(cls)) {
        if (Field<uint32_t>(mesh, off::ObjectFlags) & off::RF_ClassDefaultObject)
            continue;
        struct {
            void* ReturnValue;
        } owner { nullptr };
        Call(mesh, getOwner, &owner);
        if (owner.ReturnValue && (owner.ReturnValue == g_fighters[0] || owner.ReturnValue == g_fighters[1]))
            g_fighterMeshes.emplace_back(mesh, Field<uint32_t>(mesh, off::InternalIndex));
    }
}

void KeepClothWithTheFight()
{
    static uint64_t seenSteps = 0;
    static bool clothOn = true, wasSeeking = false;

    const uint64_t steps = g_steps.load(std::memory_order_relaxed);
    const bool stepped = steps != seenSteps;
    seenSteps = steps;
    const bool seeking = Seeking();
    const bool slowed = g_time.ScaleEffects && g_fightersValid && (g_time.Paused || g_time.Speed < ClothFollowsStepsBelow || g_recording);

    const bool wanted = !slowed || stepped || seeking;
    if (wanted != clothOn) {
        clothOn = wanted;
        ConsoleCommand(wanted ? L"p.ClothPhysics 1" : L"p.ClothPhysics 0");
    }
    // After a jump the cloth is somewhere else entirely. Starting it again from the pose means a moment looks the same however you got there.
    if (wasSeeking && !seeking && g_fightersValid) {
        if (g_fighterMeshesFight != g_fightCount)
            FindFighterMeshes();
        static FunctionRef teleport { "SkeletalMeshComponent", "ForceClothNextUpdateTeleportAndReset" };
        for (const auto& [mesh, index] : g_fighterMeshes)
            if (StillAlive(mesh, index))
                Call(mesh, teleport, nullptr);
    }
    wasSeeking = seeking;
}

// Applies time dilation and the FPS cap on the game thread once per frame.
void ApplyEngineTime()
{
    static double applied = 1.0;
    static void* appliedWorld = nullptr;
    if (void* world = World(); world != appliedWorld) {
        appliedWorld = world; // a new level starts at normal speed
        applied = 1.0;
    }
    double wanted = 1.0;
    if (g_time.ScaleEffects) {
        if (g_time.Paused)
            wanted = 0.0001;
        else if (g_recording)
            wanted = std::clamp(g_lastAdvance / std::max(g_lastRealDt, 0.001), 0.0001, 20.0); // holds still between passes
        else
            wanted = g_time.Speed;
    }
    // Restoring a saved point puts the game's own time back to normal, so this can't only be done on a change.
    if (wanted != 1.0 || applied != 1.0) {
        SetGlobalTimeDilation(static_cast<float>(wanted));
        applied = wanted;
    }

    static bool capWritten = false;
    static float* capVariable = nullptr;
    void* engine = Engine().GEngine ? *Engine().GEngine : nullptr;
    if (g_fpsVariable)
        capVariable = *g_fpsVariable;
    const double cap = g_recording ? std::max(g_time.FpsCap, g_recordingFps) : g_time.FpsCap;
    if (cap > 0 && engine && capVariable) {
        Field<uint8_t>(engine, off::EngineFrameFlags) &= 0xBF;
        Field<float>(engine, off::EngineFixedFrameRate) = static_cast<float>(cap);
        *capVariable = static_cast<float>(cap);
        capWritten = true;
    } else if (capWritten && engine && capVariable) {
        Field<float>(engine, off::EngineFixedFrameRate) = 60.0f;
        *capVariable = 60.0f;
        capWritten = false;
    }
}

void DetourGameModeTick(void* gameMode, float dt)
{
    oGameModeTick(gameMode, dt);
    if (guard::Tripped())
        return;

    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard lock(g_taskMutex);
        tasks.swap(g_tasks);
    }
    for (auto& task : tasks)
        task();

    // Fights start and end through menus; keep a cheap watch on whether our fighters still exist.
    static int frames = 0;
    if (g_fightersValid && !FightersStillAlive()) {
        mod::Log("Fight ended");
        g_fightersValid = false;
        g_fighters = {};
        for (auto& mesh : g_meshes)
            mesh = nullptr;
    }
    if (!g_fightersValid && ++frames >= 60) {
        frames = 0;
        FindFighters();
    }

    ApplyEngineTime();
    KeepClothWithTheFight();
    effects::OnGameTick();
    dvars::OnGameTick();
    lights::OnGameTick();
    audio::OnGameTick(Clock());
    recorder::OnGameTick(Clock());
    overlay::OnGameTick();
    testcmd::OnGameTick();
}

void DetourFightStart(void* a, void* b, void* c)
{
    oFightStart(a, b, c);
    if (!guard::Tripped())
        RunOnGameThread([] {
            if (!g_fightersValid)
                FindFighters();
        });
}

void DetourCamera(void* manager, float dt)
{
    oCamera(manager, dt);
    if (!guard::Tripped())
        camera::OnCameraUpdate(manager);
}

void DetourProjection(void* view, uint8_t constraint, void* a, void* b)
{
    if (!guard::Tripped()) {
        float& fov = Field<float>(view, off::ViewInfoFov);
        fov = camera::ProjectionFov(fov);
    }
    oProjection(view, constraint, a, b);
}

bool DetourActor(void* actor, Vec3 location, Rot rotation, bool sweep, void* hit, uint8_t teleport)
{
    if (g_time.Interpolate && !guard::Tripped()) {
        for (int i = 0; i < 2; ++i) {
            if (actor != g_fighters[i])
                continue;
            Track& track = g_actorTracks[i];
            if (Seeking()) {
                track.PreviousLocation = track.TargetLocation = location;
                track.PreviousRotation = track.TargetRotation = rotation;
                track.Valid = true;
                track.NewStep = false;
                break;
            }
            if (track.NewStep || !track.Valid)
                Shift(track, location, rotation);
            const double alpha = g_alpha.load(std::memory_order_relaxed);
            location = cine::Lerp(track.PreviousLocation, track.TargetLocation, alpha);
            rotation = cine::RotLerp(track.PreviousRotation, track.TargetRotation, alpha);
        }
    }
    return oActor(actor, location, rotation, sweep, hit, teleport);
}

// Runs on animation worker threads after the engine evaluated a pose, so it must not call into
// ProcessEvent; the blend is done natively.
void DetourBones(void* component, void* mesh, void* anim, bool evaluate, TArray<BoneTransform>* spaceBases,
                 TArray<BoneTransform>* boneSpace, Vec3* root, void* curve, void* attributes)
{
    oBones(component, mesh, anim, evaluate, spaceBases, boneSpace, root, curve, attributes);
    if (!g_time.Interpolate || !component || !spaceBases || guard::Tripped() || !g_fightersValid)
        return;
    for (int i = 0; i < 2; ++i) {
        if (component != g_meshes[i].load(std::memory_order_relaxed))
            continue;
        const int count = spaceBases->Num;
        if (count <= 0 || !spaceBases->Data)
            return;
        BoneTrack& track = g_boneTracks[i];
        std::lock_guard lock(track.Lock);
        // A seek runs many steps per frame. Blending from the frame before would start the next slow step that many steps back,
        // and the fighter would visibly run through all of them within one tick.
        if (static_cast<int>(track.Target.size()) != count || Seeking()) {
            track.Previous.assign(spaceBases->Data, spaceBases->Data + count);
            track.Target = track.Previous;
            track.NewStep = false;
        } else if (track.NewStep.exchange(false)) {
            track.Previous.swap(track.Target);
            track.Target.assign(spaceBases->Data, spaceBases->Data + count);
        }
        const double alpha = g_alpha.load(std::memory_order_relaxed);
        for (int bone = 0; bone < count; ++bone)
            BlendBone(track.Previous[bone], track.Target[bone], alpha, spaceBases->Data[bone]);
        return;
    }
}

struct Addresses {
    uintptr_t GEngine = 0, FpsVariable = 0, GObjects = 0, AppendString = 0, GameModeTick = 0, TekkenTick = 0, FightStart = 0,
              DoUpdateCamera = 0, SetActorLocationAndRotation = 0, PerformAnimationProcessing = 0, CalculateProjection = 0, ReplayInput = 0, ReplayRewindSite = 0,
              BattleRoundRead = 0;
};

// Rip-relative statics carry the operand offset; functions match at their first byte. Any entry can be
// replaced from signatures.ini after a game patch without rebuilding.
struct Entry {
    const char* Name;
    const char* Pattern;
    int Offset;
    bool Rip;
    bool Required;
    uintptr_t Addresses::*Field;
};

const Entry kEntries[] = {
    { "GEngine", "48 89 05 ?? ?? ?? ?? 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8D 4D F8", 3, true, true, &Addresses::GEngine },
    { "FpsVariable", "48 8B 3D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 09 E8 ?? ?? ?? ?? 84 C0 74 02 33 DB F3 0F 10 04 1F", 3, true, false, &Addresses::FpsVariable },
    { "GObjects", "8B 02 4C 8B 0D", 5, true, true, &Addresses::GObjects },
    { "AppendString", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 80 3D ?? ?? ?? ?? ?? 48 8B F2", 0, false, true, &Addresses::AppendString },
    { "GameModeTick", "48 89 5C 24 10 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B 87", 0, false, true, &Addresses::GameModeTick },
    { "TekkenTick", "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0A 48 8B 01 0F 28 C8", 0, false, true, &Addresses::TekkenTick },
    { "FightStart", "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? ?? 8B ?? 48 89 54 24 ?? 4C 8B ?? 48 89 4C 24 ?? 45 33 ??", 0, false, false, &Addresses::FightStart },
    { "DoUpdateCamera", "48 8B C4 48 89 58 18 55 56 57 48 81 EC 50", 0, false, true, &Addresses::DoUpdateCamera },
    { "SetActorLocationAndRotation", "48 81 EC 68 01 00 00 48 8B 89 A0 01 00 00 48 85 C9 74 71", 0, false, false, &Addresses::SetActorLocationAndRotation },
    { "PerformAnimationProcessing", "48 85 D2 0F 84 49 07", 0, false, false, &Addresses::PerformAnimationProcessing },
    { "CalculateProjection", "48 8B C4 48 89 58 ?? 57 48 81 EC ?? ?? ?? ?? F6 41", 0, false, false, &Addresses::CalculateProjection },
    { "ReplayInput", "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B F9 48 8B 2D ?? ?? ?? ?? 48 85 ED 0F 84", 0, false, false, &Addresses::ReplayInput },
    { "ReplayRewindSite", "E8 ?? ?? ?? ?? 45 33 C0 33 D2 48 8B C8 E8 ?? ?? ?? ?? 8B D8 4C 8B 0F 45 33 C0 8B D0 48 8B CF 41 FF 91 ?? ?? ?? ?? 85 DB 74 2D", 0, false, false, &Addresses::ReplayRewindSite },
    { "BattleRoundRead", "E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 8B 88 ?? ?? ?? ?? 89 4D 24", 0, false, false, &Addresses::BattleRoundRead },
};

template <typename T>
bool Install(uintptr_t target, void* detour, T& original, const char* name)
{
    if (!target)
        return false;
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        mod::Log("Hook %s failed: %s", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

} // namespace

bool Initialize()
{
    const scan::Module module = scan::MainModule();
    mod::Log("Game build TimeDateStamp %u, image %zu MB", module.TimeDateStamp, module.Size / (1024 * 1024));

    Ini overrides;
    overrides.Load(mod::DataDir() / "signatures.ini");

    Addresses addresses;
    bool missingRequired = false;
    for (const Entry& entry : kEntries) {
        scan::Signature signature;
        signature.Name = entry.Name;
        signature.Text = overrides.Get(entry.Name, entry.Pattern);
        signature.Offset = entry.Offset;
        signature.RipRelative = entry.Rip;
        std::string error;
        const uintptr_t address = scan::Resolve(module, signature, error);
        if (address) {
            addresses.*entry.Field = address;
            mod::Log("  %-30s +0x%llX", entry.Name, static_cast<unsigned long long>(address - module.Base));
        } else {
            mod::Log("  %-30s %s (%s)", entry.Name, error.c_str(), entry.Required ? "required" : "optional");
            missingRequired = missingRequired || entry.Required;
        }
    }
    if (missingRequired) {
        mod::Log("A required pattern is missing, so the mod stays off for this build of the game.");
        return false;
    }

    Engine().GEngine = reinterpret_cast<void**>(addresses.GEngine);
    Engine().GObjects = reinterpret_cast<void*>(addresses.GObjects);
    Engine().AppendString = reinterpret_cast<void*>(addresses.AppendString);
    g_fpsVariable = reinterpret_cast<float**>(addresses.FpsVariable);

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        mod::Log("MinHook failed to start: %s", MH_StatusToString(status));
        return false;
    }
    replay::Code replayCode;
    replayCode.InputHandler = addresses.ReplayInput;
    replayCode.RewindSite = addresses.ReplayRewindSite;
    replayCode.RoundRead = addresses.BattleRoundRead;
    replay::Setup(replayCode);
    g_hooks.GameMode = Install(addresses.GameModeTick, reinterpret_cast<void*>(&DetourGameModeTick), oGameModeTick, "GameModeTick");
    g_hooks.Tick = Install(addresses.TekkenTick, reinterpret_cast<void*>(&DetourTick), oTick, "TekkenTick");
    g_hooks.FightStart = Install(addresses.FightStart, reinterpret_cast<void*>(&DetourFightStart), oFightStart, "FightStart");
    g_hooks.Camera = Install(addresses.DoUpdateCamera, reinterpret_cast<void*>(&DetourCamera), oCamera, "DoUpdateCamera");
    g_hooks.Actors = Install(addresses.SetActorLocationAndRotation, reinterpret_cast<void*>(&DetourActor), oActor, "SetActorLocationAndRotation");
    g_hooks.Bones = Install(addresses.PerformAnimationProcessing, reinterpret_cast<void*>(&DetourBones), oBones, "PerformAnimationProcessing");
    g_hooks.Fov = Install(addresses.CalculateProjection, reinterpret_cast<void*>(&DetourProjection), oProjection, "CalculateProjection");
    mod::Log("Hooks: tick %d, gamemode %d, camera %d, actors %d, bones %d, fight %d, fov %d", g_hooks.Tick, g_hooks.GameMode, g_hooks.Camera,
             g_hooks.Actors, g_hooks.Bones, g_hooks.FightStart, g_hooks.Fov);
    return g_hooks.Tick && g_hooks.GameMode && g_hooks.Camera;
}

void Shutdown()
{
    // The tripped flag already makes every detour pass straight through; this removes them too.
    MH_DisableHook(MH_ALL_HOOKS);
}

void RunOnGameThread(std::function<void()> task)
{
    std::lock_guard lock(g_taskMutex);
    g_tasks.push_back(std::move(task));
}

TimeControl& Time()
{
    return g_time;
}

void StepOneFrame()
{
    g_stepRequests.fetch_add(1, std::memory_order_relaxed);
}

double Clock()
{
    // When the game itself is paused its steps still run but go nowhere, so there's nothing to blend towards.
    const double alpha = g_positionMoved.load(std::memory_order_relaxed) ? g_alpha.load(std::memory_order_relaxed) : 0.0;
    return (static_cast<double>(g_position.load(std::memory_order_relaxed)) + alpha) * StepSeconds - g_clockZero.load(std::memory_order_relaxed);
}

void ZeroClock()
{
    g_clockZero.store((static_cast<double>(g_position.load(std::memory_order_relaxed)) + g_alpha.load(std::memory_order_relaxed)) * StepSeconds,
                      std::memory_order_relaxed);
}

double ClockZero()
{
    return g_clockZero.load(std::memory_order_relaxed);
}

uint64_t StepCount()
{
    return g_steps.load(std::memory_order_relaxed);
}

std::vector<cine::SpeedKey> SpeedKeys()
{
    std::lock_guard lock(g_speedMutex);
    return g_speedKeys;
}

void SetSpeedKeys(std::vector<cine::SpeedKey> keys)
{
    std::sort(keys.begin(), keys.end(), [](const cine::SpeedKey& a, const cine::SpeedKey& b) { return a.Time < b.Time; });
    std::lock_guard lock(g_speedMutex);
    g_speedKeys = std::move(keys);
}

void SeekTo(double seconds)
{
    g_seekWaits = 0;
    g_seekJumped = false;
    g_seekToRound = false;
    g_seekTarget = seconds;
}

void SeekToRound(uint32_t frame)
{
    g_seekWaits = 0;
    g_seekJumped = false;
    g_seekToRound = true;
    g_seekTarget = frame * StepSeconds - g_clockZero.load(std::memory_order_relaxed);
}

bool Seeking()
{
    return g_seekTarget.load(std::memory_order_relaxed) >= 0 || g_settleFrames.load(std::memory_order_relaxed) > 0;
}

void CountRenderedFrame()
{
    g_renderedFrames.fetch_add(1, std::memory_order_relaxed);
}

Rates MeasuredRates()
{
    static double windowStart = 0;
    static uint64_t startSteps = 0, startFrames = 0;
    static Rates rates;
    const double now = RealSeconds();
    if (now - windowStart >= 0.5) {
        const double elapsed = now - windowStart;
        const uint64_t steps = g_steps.load(std::memory_order_relaxed), frames = g_renderedFrames.load(std::memory_order_relaxed);
        rates.TicksPerSecond = (steps - startSteps) / elapsed;
        rates.FramesPerSecond = (frames - startFrames) / elapsed;
        windowStart = now;
        startSteps = steps;
        startFrames = frames;
    }
    return rates;
}

void SetRecordingClock(bool enabled, double samplesPerSecond)
{
    g_recordingFps = std::max(samplesPerSecond, 1.0);
    g_recording = enabled;
}

void* Fighter(int side)
{
    return g_fightersValid ? g_fighters[side] : nullptr;
}

bool FighterLocation(int side, Vec3& out)
{
    void* actor = Fighter(side);
    return actor && ActorLocation(actor, out);
}

int FightCount()
{
    return g_fightCount;
}

double StepAlpha()
{
    return g_alpha.load(std::memory_order_relaxed);
}

const HookStatus& Hooks()
{
    return g_hooks;
}

void PreviewFrame()
{
    if (!oTick) {
        oTick = [](double) {};
        oGameModeTick = [](void*, float) {};
    }
    // The engine would leave its own camera here each frame: off to the side of the stage, looking across it.
    alignas(16) static uint8_t cameraManager[0x3000] = {};
    Field<Vec3>(cameraManager, off::PovLocation) = { 0, 700, 150 };
    Field<Rot>(cameraManager, off::PovRotation) = { -5, -90, 0 };
    Field<float>(cameraManager, off::PovFov) = 45.0f;
    DetourTick(StepSeconds);
    DetourGameModeTick(nullptr, 0);
    camera::OnCameraUpdate(cameraManager);
}

} // namespace game
