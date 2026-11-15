#include "mod/actors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "common/xform.h"
#include "mod/camera.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/recorder.h"
#include "mod/replay.h"
#include "mod/ue.h"

namespace actors {

namespace fs = std::filesystem;

namespace {

// The engine's FTransform: rotation, translation and scale, four doubles each.
struct EngineBone {
    double Rotation[4];
    double Translation[4];
    double Scale[4];
};

// A bone as a take keeps it. Floats are plenty for a pose, and a take is a third of the size.
struct PackedBone {
    float R[4];
    float T[3];
    float S[3];
};

struct Pose {
    cine::Vec3 Location;
    cine::Quat Rotation;
};

struct TakeMesh {
    std::string Asset;            // the mesh asset's name when it was recorded, for the log only: the game gives it a new one in every fight
    uint64_t Skeleton = 0;        // made from the names of its bones, which is how the same piece is found on a fighter later
    int Bones = 0;
    cine::Xform Rel;              // the mesh component against the fighter's actor
    std::vector<PackedBone> Poses; // frames x bones
};

struct Take {
    std::string Name;
    int Side = 0;
    int Frames = 0;
    cine::Vec3 ActorScale { 1, 1, 1 };
    std::vector<Pose> Actor; // where the fighter stood, per frame
    std::vector<TakeMesh> Meshes;
};

constexpr int MostFrames = 60 * 120; // two minutes
constexpr char Magic[8] = { 'T', '8', 'T', 'A', 'K', 'E', '0', '2' };

std::array<Actor, MaxActors> g_cast;

// ----- Takes, in memory and on disk
std::mutex g_takesMutex;
std::map<std::string, std::shared_ptr<const Take>> g_loaded;
std::vector<TakeInfo> g_infos;
bool g_infosListed = false;

fs::path Folder()
{
    fs::path dir = mod::DataDir() / "takes";
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    return dir;
}

template <typename T>
void Put(std::ofstream& file, const T& value)
{
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool Get(std::ifstream& file, T& value)
{
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool SaveTake(const Take& take)
{
    if (!mod::SafeFileName(take.Name))
        return false;
    std::ofstream file(Folder() / (take.Name + ".t8take"), std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(Magic, sizeof(Magic));
    Put(file, static_cast<uint32_t>(take.Side));
    Put(file, static_cast<uint32_t>(take.Frames));
    Put(file, static_cast<uint32_t>(take.Meshes.size()));
    Put(file, take.ActorScale);
    for (const TakeMesh& mesh : take.Meshes) {
        Put(file, static_cast<uint32_t>(mesh.Asset.size()));
        file.write(mesh.Asset.data(), static_cast<std::streamsize>(mesh.Asset.size()));
        Put(file, mesh.Skeleton);
        Put(file, static_cast<uint32_t>(mesh.Bones));
        Put(file, mesh.Rel);
    }
    file.write(reinterpret_cast<const char*>(take.Actor.data()), static_cast<std::streamsize>(take.Actor.size() * sizeof(Pose)));
    for (const TakeMesh& mesh : take.Meshes)
        file.write(reinterpret_cast<const char*>(mesh.Poses.data()), static_cast<std::streamsize>(mesh.Poses.size() * sizeof(PackedBone)));
    return static_cast<bool>(file);
}

// The header and the mesh list, and the motion itself when `whole` is set.
std::shared_ptr<Take> ReadTake(const fs::path& path, bool whole)
{
    std::ifstream file(path, std::ios::binary);
    char magic[8] = {};
    uint32_t side = 0, frames = 0, meshes = 0;
    auto take = std::make_shared<Take>();
    if (!file || !file.read(magic, sizeof(magic)) || std::memcmp(magic, Magic, sizeof(Magic)) != 0 || !Get(file, side) || !Get(file, frames) || !Get(file, meshes)
        || !Get(file, take->ActorScale) || frames == 0 || frames > MostFrames || meshes == 0 || meshes > 64)
        return nullptr;
    take->Name = path.stem().string();
    take->Side = side == 1 ? 1 : 0;
    take->Frames = static_cast<int>(frames);
    take->Meshes.resize(meshes);
    for (TakeMesh& mesh : take->Meshes) {
        uint32_t length = 0, bones = 0;
        if (!Get(file, length) || length > 512)
            return nullptr;
        mesh.Asset.resize(length);
        if (!file.read(mesh.Asset.data(), length) || !Get(file, mesh.Skeleton) || !Get(file, bones) || bones == 0 || bones > 4096 || !Get(file, mesh.Rel))
            return nullptr;
        mesh.Bones = static_cast<int>(bones);
    }
    if (!whole)
        return take;
    take->Actor.resize(frames);
    if (!file.read(reinterpret_cast<char*>(take->Actor.data()), static_cast<std::streamsize>(frames * sizeof(Pose))))
        return nullptr;
    for (TakeMesh& mesh : take->Meshes) {
        mesh.Poses.resize(static_cast<size_t>(frames) * mesh.Bones);
        if (!file.read(reinterpret_cast<char*>(mesh.Poses.data()), static_cast<std::streamsize>(mesh.Poses.size() * sizeof(PackedBone))))
            return nullptr;
    }
    return take;
}

void ListTakesLocked()
{
    g_infos.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(Folder(), error)) {
        if (entry.path().extension() != ".t8take")
            continue;
        if (const auto take = ReadTake(entry.path(), false))
            g_infos.push_back({ take->Name, take->Frames, take->Side, static_cast<int>(take->Meshes.size()) });
    }
    std::sort(g_infos.begin(), g_infos.end(), [](const TakeInfo& a, const TakeInfo& b) { return a.Name < b.Name; });
    g_infosListed = true;
}

std::shared_ptr<const Take> FindTake(const std::string& name)
{
    if (!mod::SafeFileName(name))
        return nullptr;
    std::lock_guard lock(g_takesMutex);
    if (const auto it = g_loaded.find(name); it != g_loaded.end())
        return it->second;
    std::shared_ptr<const Take> take = ReadTake(Folder() / (name + ".t8take"), true);
    if (take)
        g_loaded[name] = take;
    return take;
}

// ----- Recording a take
enum class State { Idle, Going, Running };
State g_state = State::Idle;

struct Request {
    bool Pending = false;
    bool Cancel = false;
    std::string Name;
    int Side = 0;
    double In = 0, Out = 0;
} g_request;
std::mutex g_requestMutex;
std::string g_takeStatus;
std::atomic<bool> g_recordingTake { false };

void SetTakeStatus(const std::string& text)
{
    mod::Log("Take: %s", text.c_str());
    std::lock_guard lock(g_requestMutex);
    g_takeStatus = text;
}

std::atomic<bool> g_capturing { false };
std::mutex g_captureMutex;
struct Capture {
    std::shared_ptr<Take> Take;
    std::vector<void*> Components;
    int64_t StartFrame = 0;
    std::vector<std::vector<uint8_t>> Filled; // per mesh, per frame
    std::vector<uint8_t> ActorFilled;
} g_capture;

game::TimeControl g_savedTime;
int64_t g_lastPosition = 0;
int g_stalled = 0;
double g_wantedLength = 0;

void* AssetOf(void* component)
{
    static void* cls = ue::FindClass("SkinnedMeshComponent");
    static const int at = [] {
        if (!cls)
            return -1;
        const int skinned = ue::PropertyOffset(cls, "SkinnedAsset");
        return skinned >= 0 ? skinned : ue::PropertyOffset(cls, "SkeletalMesh");
    }();
    return at >= 0 ? ue::Field<void*>(component, at) : nullptr;
}

// What tells one fighter's body from another's: the names of its bones, in order. The mesh itself can't be gone by, since the game
// puts each fighter's body together when the fight loads and gives it a new name every time. Asking for 500 names isn't free, so each
// mesh is asked once.
std::map<void*, std::pair<uint32_t, uint64_t>> g_skeletons;

uint64_t SkeletonOf(void* component)
{
    const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
    if (const auto it = g_skeletons.find(component); it != g_skeletons.end() && it->second.first == index)
        return it->second.second;
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    const std::vector<std::string> bones = ue::BoneNames(component);
    for (const std::string& bone : bones) {
        for (const char c : bone)
            hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
        hash = (hash ^ 0xFF) * 1099511628211ull;
    }
    if (bones.empty())
        return 0; // not loaded yet: asked again next time
    g_skeletons[component] = { index, hash };
    return hash;
}

bool ActorTransform(void* actor, cine::Xform& out)
{
    struct {
        double Rotation[4], Translation[4], Scale[4];
    } params {};
    params.Rotation[3] = 1;
    static ue::FunctionRef ref { "Actor", "GetTransform" };
    if (!ue::Call(actor, ref, &params))
        return false;
    out.Rotation = { params.Rotation[0], params.Rotation[1], params.Rotation[2], params.Rotation[3] };
    out.Translation = { params.Translation[0], params.Translation[1], params.Translation[2] };
    out.Scale = { params.Scale[0], params.Scale[1], params.Scale[2] };
    return true;
}

// The fighter has landed on the take's first frame: note which meshes to follow, and start.
bool BeginCapture(const Request& request)
{
    void* fighter = game::Fighter(request.Side);
    cine::Xform actorWorld;
    if (!fighter || !ActorTransform(fighter, actorWorld))
        return false;
    auto take = std::make_shared<Take>();
    take->Name = request.Name;
    take->Side = request.Side;
    take->Frames = std::clamp(static_cast<int>(std::lround(g_wantedLength * 60.0)) + 1, 2, MostFrames);
    take->ActorScale = actorWorld.Scale;
    take->Actor.resize(take->Frames);

    Capture capture;
    for (void* component : game::FighterMeshes(request.Side)) {
        void* asset = AssetOf(component);
        cine::Xform world;
        if (!asset || !ue::ComponentTransform(component, world))
            continue;
        TakeMesh mesh;
        mesh.Asset = ue::NameOf(asset);
        mesh.Skeleton = SkeletonOf(component);
        mesh.Rel = cine::Compose(world, cine::Inverse(actorWorld));
        take->Meshes.push_back(std::move(mesh));
        capture.Components.push_back(component);
    }
    if (take->Meshes.empty())
        return false;
    capture.Take = take;
    capture.StartFrame = game::Position();
    capture.Filled.assign(take->Meshes.size(), std::vector<uint8_t>(take->Frames, 0));
    capture.ActorFilled.assign(take->Frames, 0);
    {
        std::lock_guard lock(g_captureMutex);
        g_capture = std::move(capture);
    }
    g_capturing = true;
    return true;
}

// Closes the gaps (a frame the picture skipped), cuts the take to what was really recorded, and saves it.
void EndCapture(bool keep)
{
    g_capturing = false;
    Capture capture;
    {
        std::lock_guard lock(g_captureMutex);
        capture = std::move(g_capture);
        g_capture = {};
    }
    if (!keep || !capture.Take) {
        SetTakeStatus("Cancelled");
        return;
    }
    Take& take = *capture.Take;
    // Meshes that never showed up in the bone hook can't be played back.
    for (size_t m = take.Meshes.size(); m-- > 0;)
        if (take.Meshes[m].Bones == 0) {
            take.Meshes.erase(take.Meshes.begin() + m);
            capture.Filled.erase(capture.Filled.begin() + m);
        }
    int frames = 0;
    for (const auto& filled : capture.Filled)
        for (int f = take.Frames - 1; f >= frames; --f)
            if (filled[f]) {
                frames = f + 1;
                break;
            }
    if (take.Meshes.empty() || frames < 2) {
        SetTakeStatus("Nothing was recorded. Is the replay playing, with Blend motion between game steps on?");
        return;
    }
    take.Frames = frames;
    take.Actor.resize(frames);
    const auto fill = [frames](const std::vector<uint8_t>& filled, const auto& copy) {
        int first = 0;
        while (first < frames && !filled[first])
            ++first;
        for (int f = 0; f < frames; ++f)
            if (!filled[f])
                copy(f, f < first ? first : f - 1);
    };
    fill(capture.ActorFilled, [&](int to, int from) {
        if (from < frames)
            take.Actor[to] = take.Actor[from];
    });
    for (size_t m = 0; m < take.Meshes.size(); ++m) {
        TakeMesh& mesh = take.Meshes[m];
        mesh.Poses.resize(static_cast<size_t>(frames) * mesh.Bones);
        fill(capture.Filled[m], [&](int to, int from) {
            if (from < frames)
                std::memcpy(&mesh.Poses[static_cast<size_t>(to) * mesh.Bones], &mesh.Poses[static_cast<size_t>(from) * mesh.Bones], sizeof(PackedBone) * mesh.Bones);
        });
    }
    const bool saved = SaveTake(take);
    {
        std::lock_guard lock(g_takesMutex);
        g_loaded[take.Name] = capture.Take;
        ListTakesLocked();
    }
    char text[160];
    snprintf(text, sizeof(text), "Take \"%s\": %d frames (%.2f s)%s", take.Name.c_str(), frames, frames / 60.0, saved ? "" : ", but the file couldn't be written");
    SetTakeStatus(text);
}

void UpdateTakeRecorder()
{
    Request request;
    {
        std::lock_guard lock(g_requestMutex);
        request = g_request;
        if (g_state == State::Idle)
            g_request.Pending = false;
        g_request.Cancel = false;
    }
    game::TimeControl& t = game::Time();

    if (g_state == State::Idle) {
        if (!request.Pending)
            return;
        if (!game::Fighter(request.Side) || recorder::Active() || recorder::Finishing()) {
            SetTakeStatus(recorder::Active() ? "Stop the video recording first" : "A take needs a fight on screen");
            return;
        }
        g_savedTime = t;
        g_wantedLength = std::max(request.Out - request.In, 1.0 / 60.0);
        t.Interpolate = true; // the bone hook only follows the fighters with this on
        t.UseSpeedRamp = false;
        t.Paused = false;
        game::SeekTo(request.In);
        g_state = State::Going;
        g_recordingTake = true;
        SetTakeStatus("Going to the start of the take...");
        return;
    }

    const auto finish = [&](bool keep) {
        game::SetRecordingClock(false, recorder::Config().Fps);
        EndCapture(keep);
        const bool interpolate = g_savedTime.Interpolate, ramp = g_savedTime.UseSpeedRamp;
        const double speed = g_savedTime.Speed;
        t.Interpolate = interpolate;
        t.UseSpeedRamp = ramp;
        t.Speed = speed;
        t.Paused = true;
        g_state = State::Idle;
        g_recordingTake = false;
    };

    if (request.Cancel || !game::Fighter(request.Side)) {
        if (g_state == State::Going)
            game::StopSeek();
        finish(false);
        return;
    }

    if (g_state == State::Going) {
        if (game::Seeking())
            return;
        if (!BeginCapture(request)) {
            SetTakeStatus("Couldn't find the fighter's meshes");
            finish(false);
            return;
        }
        // One game step for every picture, so no step is missed however fast the PC is.
        t.Speed = 1.0;
        t.Paused = false;
        game::SetRecordingClock(true, 60.0);
        g_lastPosition = game::Position();
        g_stalled = 0;
        g_state = State::Running;
        SetTakeStatus("Recording the take...");
        return;
    }

    // Running: where the fighter stands this step, and whether the take is done.
    const int64_t position = game::Position();
    bool done = false;
    {
        std::lock_guard lock(g_captureMutex);
        if (!g_capture.Take) {
            done = true;
        } else {
            const int64_t frame = position - g_capture.StartFrame;
            cine::Vec3 location;
            cine::Rot rotation;
            if (frame >= 0 && frame < g_capture.Take->Frames && game::FighterPose(request.Side, location, rotation)) {
                g_capture.Take->Actor[frame] = { location, cine::ToQuat(rotation) };
                g_capture.ActorFilled[frame] = 1;
            }
            done = frame >= g_capture.Take->Frames - 1;
        }
    }
    // The end of the round or the match: the fight stops moving.
    g_stalled = position == g_lastPosition ? g_stalled + 1 : 0;
    g_lastPosition = position;
    if (done || g_stalled > 240)
        finish(true);
}

// ----- Actors on the stage
constexpr int MaxCloneMeshes = 16;

struct DriveSlot {
    std::atomic<void*> Component { nullptr };
    std::mutex Lock;
    std::shared_ptr<const Take> Take;
    int Mesh = 0;
    int A = 0, B = 0;
    double Alpha = 0;
};
std::array<DriveSlot, MaxActors * MaxCloneMeshes> g_drive;
std::atomic<int> g_driveCount { 0 };

struct CloneMesh {
    void* Actor = nullptr;
    void* Component = nullptr;
    uint32_t Index = 0;
    int TakeMesh = 0;
    DriveSlot* Slot = nullptr;
    cine::Vec3 Scale;
};

struct Clone {
    std::vector<CloneMesh> Meshes;
    std::string TakeName;
    int Body = -2;
    std::shared_ptr<const Take> Take;
    bool Hidden = false;
    std::string Failed; // a take that couldn't be put on this fight, so it isn't tried again every frame
    int FailedBody = -2;
    int Retry = 0;      // ticks until it's tried again anyway: a fighter's meshes can load a moment after the fight starts
};
std::array<Clone, MaxActors> g_clones;
std::mutex g_statusMutex;
std::array<std::string, MaxActors> g_status;

void SetStatus(int index, const char* text)
{
    std::lock_guard lock(g_statusMutex);
    if (g_status[index] != text)
        g_status[index] = text;
}

void DestroyClone(Clone& clone)
{
    for (CloneMesh& mesh : clone.Meshes) {
        if (mesh.Slot) {
            std::lock_guard lock(mesh.Slot->Lock);
            mesh.Slot->Component = nullptr;
            mesh.Slot->Take.reset();
            --g_driveCount;
        }
        if (mesh.Actor && ue::StillAlive(mesh.Component, mesh.Index))
            ue::DestroyActor(mesh.Actor);
    }
    clone.Meshes.clear();
    clone.Take.reset();
    clone.TakeName.clear();
    clone.Body = -2;
    clone.Hidden = false;
}

DriveSlot* FreeSlot()
{
    for (DriveSlot& slot : g_drive)
        if (!slot.Component.load(std::memory_order_relaxed))
            return &slot;
    return nullptr;
}

void SetMesh(void* component, void* asset)
{
    static void* skinned = ue::FindClass("SkinnedMeshComponent");
    static void* skeletal = ue::FindClass("SkeletalMeshComponent");
    struct {
        void* Mesh;
        bool ReinitPose;
    } params { asset, true };
    if (skinned && ue::FindFunction(skinned, "SetSkinnedAssetAndUpdate")) {
        static ue::FunctionRef ref { "SkinnedMeshComponent", "SetSkinnedAssetAndUpdate" };
        ue::Call(component, ref, &params);
    } else if (skeletal && ue::FindFunction(skeletal, "SetSkeletalMesh")) {
        static ue::FunctionRef ref { "SkeletalMeshComponent", "SetSkeletalMesh" };
        ue::Call(component, ref, &params);
    } else if (skeletal && ue::FindFunction(skeletal, "SetSkeletalMeshAsset")) {
        static ue::FunctionRef ref { "SkeletalMeshComponent", "SetSkeletalMeshAsset" };
        ue::Call(component, ref, &params);
    }
}

// A copy of one of the fighter's mesh components: the same mesh in the same materials, lit the same way, on an actor of its own.
bool CloneMeshFrom(void* source, CloneMesh& out)
{
    void* asset = AssetOf(source);
    void* actor = asset ? ue::SpawnActor("SkeletalMeshActor") : nullptr;
    static void* cls = ue::FindClass("SkeletalMeshActor");
    static const int componentAt = cls ? ue::PropertyOffset(cls, "SkeletalMeshComponent") : -1;
    void* component = actor && componentAt >= 0 ? ue::Field<void*>(actor, componentAt) : nullptr;
    if (!component) {
        if (actor)
            ue::DestroyActor(actor);
        return false;
    }
    ue::SetMovable(component);
    SetMesh(component, asset);

    struct {
        int32_t ReturnValue;
    } count { 0 };
    static ue::FunctionRef numMaterials { "PrimitiveComponent", "GetNumMaterials" };
    ue::Call(source, numMaterials, &count);
    static ue::FunctionRef getMaterial { "PrimitiveComponent", "GetMaterial" };
    static ue::FunctionRef setMaterial { "PrimitiveComponent", "SetMaterial" };
    for (int i = 0; i < count.ReturnValue && i < 64; ++i) {
        struct {
            int32_t Slot;
            void* Material;
        } material { i, nullptr };
        ue::Call(source, getMaterial, &material);
        if (material.Material)
            ue::Call(component, setMaterial, &material);
    }

    // Fighters sit on a lighting channel of their own on some stages.
    static void* primitive = ue::FindClass("PrimitiveComponent");
    static const int channelsAt = primitive ? ue::PropertyOffset(primitive, "LightingChannels") : -1;
    if (channelsAt >= 0) {
        const uint8_t bits = ue::Field<uint8_t>(source, channelsAt);
        struct {
            bool Channel0, Channel1, Channel2;
        } channels { (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0 };
        static ue::FunctionRef setChannels { "PrimitiveComponent", "SetLightingChannels" };
        ue::Call(component, setChannels, &channels);
    }
    // The pose is worked out every frame, seen or not, since that's where the take's pose is put in.
    static void* skinned = ue::FindClass("SkinnedMeshComponent");
    static const int tickAt = skinned ? ue::PropertyOffset(skinned, "VisibilityBasedAnimTickOption") : -1;
    if (tickAt >= 0)
        ue::Field<uint8_t>(component, tickAt) = 0;
    struct {
        bool Value;
    } collision { false };
    static ue::FunctionRef setCollision { "Actor", "SetActorEnableCollision" };
    ue::Call(actor, setCollision, &collision);

    out.Actor = actor;
    out.Component = component;
    out.Index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
    return true;
}

enum class Built { Yes, NotYet, WrongFighter };

Built BuildClone(int index, Clone& clone, const std::shared_ptr<const Take>& take, int body)
{
    const int side = body < 0 ? take->Side : body;
    if (!game::Fighter(side))
        return Built::NotYet;
    const std::vector<void*> sources = game::FighterMeshes(side);
    bool allKnown = !sources.empty();
    for (int m = 0; m < static_cast<int>(take->Meshes.size()) && static_cast<int>(clone.Meshes.size()) < MaxCloneMeshes; ++m) {
        void* source = nullptr;
        for (void* candidate : sources) {
            const uint64_t skeleton = AssetOf(candidate) ? SkeletonOf(candidate) : 0;
            allKnown = allKnown && skeleton != 0;
            if (skeleton != 0 && skeleton == take->Meshes[m].Skeleton) {
                source = candidate;
                break;
            }
        }
        DriveSlot* slot = source ? FreeSlot() : nullptr;
        CloneMesh mesh;
        if (!slot || !CloneMeshFrom(source, mesh))
            continue;
        mesh.TakeMesh = m;
        mesh.Slot = slot;
        {
            std::lock_guard lock(slot->Lock);
            slot->Take = take;
            slot->Mesh = m;
            slot->A = slot->B = 0;
            slot->Alpha = 0;
            slot->Component = mesh.Component;
        }
        ++g_driveCount;
        clone.Meshes.push_back(mesh);
    }
    // With nothing matched, either this fighter is someone else, or their meshes aren't all there yet to tell.
    if (clone.Meshes.empty())
        return allKnown ? Built::WrongFighter : Built::NotYet;
    clone.Take = take;
    clone.TakeName = take->Name;
    clone.Body = body;
    clone.Hidden = false;
    mod::Log("Actors: actor %d plays \"%s\" on %zu of its %zu meshes", index + 1, take->Name.c_str(), clone.Meshes.size(), take->Meshes.size());
    return Built::Yes;
}

void UpdateActor(int i, bool inFight)
{
    const Actor& actor = g_cast[i];
    Clone& clone = g_clones[i];
    const std::string takeName = actor.Take;
    if (!actor.On || takeName.empty() || !inFight) {
        if (!clone.Meshes.empty() && inFight)
            DestroyClone(clone);
        // Outside a fight the stage may be on its way out, so nothing is asked of the engine: the bone hook just stops looking for these.
        if (!inFight)
            for (CloneMesh& mesh : clone.Meshes)
                if (mesh.Slot) {
                    std::lock_guard lock(mesh.Slot->Lock);
                    mesh.Slot->Component = nullptr;
                    mesh.Slot->Take.reset();
                    mesh.Slot = nullptr;
                    --g_driveCount;
                }
        if (!actor.On || takeName.empty()) {
            clone.Failed.clear();
            SetStatus(i, "");
        }
        return;
    }
    bool alive = !clone.Meshes.empty();
    for (const CloneMesh& mesh : clone.Meshes)
        alive = alive && mesh.Slot && ue::StillAlive(mesh.Component, mesh.Index);
    if (!clone.Meshes.empty() && (!alive || clone.TakeName != takeName || clone.Body != actor.Body))
        DestroyClone(clone);
    if (clone.Meshes.empty()) {
        if (clone.Failed == takeName && clone.FailedBody == actor.Body && --clone.Retry > 0)
            return;
        clone.Failed = takeName;
        clone.FailedBody = actor.Body;
        clone.Retry = 120;
        const std::shared_ptr<const Take> take = FindTake(takeName);
        if (!take) {
            SetStatus(i, "Can't find this take");
            return;
        }
        const Built built = BuildClone(i, clone, take, actor.Body);
        if (built != Built::Yes) {
            SetStatus(i, built == Built::NotYet ? "Waiting for the fighter to load" : "This take was made with another character or outfit: it needs a fighter with the same body in this fight");
            return;
        }
        clone.Failed.clear();
        SetStatus(i, clone.Meshes.size() < clone.Take->Meshes.size() ? "Some of the take's meshes aren't on this fighter" : "");
    }

    const Take& take = *clone.Take;
    const cine::TakeCursor cursor = cine::SampleTake(take.Frames, game::Clock() - actor.Start, actor.Speed, static_cast<cine::TakeEnd>(std::clamp(actor.End, 0, 2)));
    if (cursor.Visible == clone.Hidden) {
        for (const CloneMesh& mesh : clone.Meshes)
            ue::SetActorHidden(mesh.Actor, !cursor.Visible);
        clone.Hidden = !cursor.Visible;
    }

    const Pose& a = take.Actor[cursor.A];
    const Pose& b = take.Actor[cursor.B];
    const cine::Xform first { take.Actor[0].Rotation, take.Actor[0].Location, take.ActorScale };
    const cine::Xform now { cine::Slerp(a.Rotation, b.Rotation, cursor.Alpha), cine::Lerp(a.Location, b.Location, cursor.Alpha), take.ActorScale };
    const cine::Xform placement { cine::ToQuat({ actor.Rotation[0], actor.Rotation[1], actor.Rotation[2] }), { actor.Location[0], actor.Location[1], actor.Location[2] },
                                  take.ActorScale * static_cast<double>(std::max(actor.Scale, 0.01f)) };
    const cine::Xform world = cine::Replaced(first, now, placement);

    for (CloneMesh& mesh : clone.Meshes) {
        const cine::Xform at = cine::Compose(take.Meshes[mesh.TakeMesh].Rel, world);
        ue::SetActorPose(mesh.Actor, at.Translation, cine::ToRot(at.Rotation));
        if (at.Scale.X != mesh.Scale.X || at.Scale.Y != mesh.Scale.Y || at.Scale.Z != mesh.Scale.Z) {
            ue::SetActorScale(mesh.Actor, at.Scale);
            mesh.Scale = at.Scale;
        }
        std::lock_guard lock(mesh.Slot->Lock);
        mesh.Slot->A = cursor.A;
        mesh.Slot->B = cursor.B;
        mesh.Slot->Alpha = cursor.Alpha;
    }
}

} // namespace

std::array<Actor, MaxActors>& Cast()
{
    return g_cast;
}

std::vector<TakeInfo> Takes()
{
    std::lock_guard lock(g_takesMutex);
    if (!g_infosListed)
        ListTakesLocked();
    return g_infos;
}

void RefreshTakes()
{
    std::lock_guard lock(g_takesMutex);
    ListTakesLocked();
}

bool StartTake(const std::string& name, int side, double in, double out, std::string& error)
{
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        error = "Give the take a name without \\ / : * ? \" < > |";
        return false;
    }
    if (out <= in) {
        error = "The range has to end after it starts";
        return false;
    }
    if ((out - in) * 60.0 > MostFrames) {
        error = "A take can be two minutes at most";
        return false;
    }
    if (in < game::Clock() && !replay::CanJump()) {
        error = "Going back needs a replay";
        return false;
    }
    std::lock_guard lock(g_requestMutex);
    if (g_request.Pending || g_recordingTake) {
        error = "A take is already being recorded";
        return false;
    }
    g_request = { true, false, name, side == 1 ? 1 : 0, in, out };
    return true;
}

void CancelTake()
{
    std::lock_guard lock(g_requestMutex);
    g_request.Cancel = true;
}

bool RecordingTake()
{
    return g_recordingTake;
}

std::string TakeStatus()
{
    std::lock_guard lock(g_requestMutex);
    return g_takeStatus;
}

void DeleteTake(const std::string& name)
{
    if (!mod::SafeFileName(name))
        return;
    std::lock_guard lock(g_takesMutex);
    std::error_code ignored;
    fs::remove(Folder() / (name + ".t8take"), ignored);
    g_loaded.erase(name);
    ListTakesLocked();
}

void SetTake(int index, const std::string& name)
{
    if (index < 0 || index >= MaxActors)
        return;
    Actor& actor = g_cast[index];
    strncpy_s(actor.Take, name.c_str(), _TRUNCATE);
    const std::shared_ptr<const Take> take = FindTake(name);
    if (!take || take->Actor.empty())
        return;
    const Pose& first = take->Actor[0];
    const cine::Rot rotation = cine::ToRot(first.Rotation);
    actor.Location[0] = static_cast<float>(first.Location.X);
    actor.Location[1] = static_cast<float>(first.Location.Y);
    actor.Location[2] = static_cast<float>(first.Location.Z);
    actor.Rotation[0] = static_cast<float>(rotation.Pitch);
    actor.Rotation[1] = static_cast<float>(rotation.Yaw);
    actor.Rotation[2] = static_cast<float>(rotation.Roll);
}

void PlaceAtCamera(int index)
{
    const camera::View view = camera::LastView();
    if (!view.Valid || index < 0 || index >= MaxActors)
        return;
    Actor& actor = g_cast[index];
    // On the ground it was recorded on, a few steps in front of the camera.
    cine::Vec3 forward = cine::ForwardOf({ 0, view.Rotation.Yaw, 0 });
    const cine::Vec3 at = view.Location + forward * 300.0;
    actor.Location[0] = static_cast<float>(at.X);
    actor.Location[1] = static_cast<float>(at.Y);
}

std::string ActorStatus(int index)
{
    std::lock_guard lock(g_statusMutex);
    return index >= 0 && index < MaxActors ? g_status[index] : std::string();
}

bool IsActorPart(const void* actor)
{
    if (!actor)
        return false;
    for (const Clone& clone : g_clones)
        for (const CloneMesh& mesh : clone.Meshes)
            if (mesh.Actor == actor)
                return true;
    return false;
}

bool DrivePose(void* component, void* transforms, int count)
{
    if (g_driveCount.load(std::memory_order_relaxed) <= 0)
        return false;
    for (DriveSlot& slot : g_drive) {
        if (slot.Component.load(std::memory_order_relaxed) != component)
            continue;
        std::lock_guard lock(slot.Lock);
        if (slot.Component.load(std::memory_order_relaxed) != component || !slot.Take)
            return true;
        const TakeMesh& mesh = slot.Take->Meshes[slot.Mesh];
        if (mesh.Bones != count || slot.A < 0 || slot.B < 0 || slot.A >= slot.Take->Frames || slot.B >= slot.Take->Frames)
            return true;
        const PackedBone* a = &mesh.Poses[static_cast<size_t>(slot.A) * mesh.Bones];
        const PackedBone* b = &mesh.Poses[static_cast<size_t>(slot.B) * mesh.Bones];
        auto* out = static_cast<EngineBone*>(transforms);
        const double alpha = slot.Alpha;
        for (int bone = 0; bone < count; ++bone) {
            const PackedBone& from = a[bone];
            const PackedBone& to = b[bone];
            const double dot = static_cast<double>(from.R[0]) * to.R[0] + static_cast<double>(from.R[1]) * to.R[1] + static_cast<double>(from.R[2]) * to.R[2]
                + static_cast<double>(from.R[3]) * to.R[3];
            const double bias = dot >= 0 ? 1.0 : -1.0;
            double q[4];
            double size = 0;
            for (int k = 0; k < 4; ++k) {
                q[k] = to.R[k] * alpha + from.R[k] * (bias * (1.0 - alpha));
                size += q[k] * q[k];
            }
            const double scale = size > 1e-12 ? 1.0 / std::sqrt(size) : 0.0;
            for (int k = 0; k < 4; ++k)
                out[bone].Rotation[k] = size > 1e-12 ? q[k] * scale : (k == 3 ? 1.0 : 0.0);
            for (int k = 0; k < 3; ++k) {
                out[bone].Translation[k] = from.T[k] + (static_cast<double>(to.T[k]) - from.T[k]) * alpha;
                out[bone].Scale[k] = from.S[k] + (static_cast<double>(to.S[k]) - from.S[k]) * alpha;
            }
        }
        return true;
    }
    return false;
}

void CaptureBones(void* component, const void* transforms, int count)
{
    if (!g_capturing.load(std::memory_order_relaxed) || count <= 0)
        return;
    std::lock_guard lock(g_captureMutex);
    if (!g_capture.Take)
        return;
    const auto it = std::find(g_capture.Components.begin(), g_capture.Components.end(), component);
    if (it == g_capture.Components.end())
        return;
    const size_t index = static_cast<size_t>(it - g_capture.Components.begin());
    Take& take = *g_capture.Take;
    const int64_t frame = game::Position() - g_capture.StartFrame;
    if (frame < 0 || frame >= take.Frames)
        return;
    TakeMesh& mesh = take.Meshes[index];
    if (mesh.Bones == 0) {
        mesh.Bones = count;
        mesh.Poses.resize(static_cast<size_t>(take.Frames) * count);
    }
    if (mesh.Bones != count)
        return;
    const auto* in = static_cast<const EngineBone*>(transforms);
    PackedBone* out = &mesh.Poses[static_cast<size_t>(frame) * count];
    for (int bone = 0; bone < count; ++bone) {
        for (int k = 0; k < 4; ++k)
            out[bone].R[k] = static_cast<float>(in[bone].Rotation[k]);
        for (int k = 0; k < 3; ++k) {
            out[bone].T[k] = static_cast<float>(in[bone].Translation[k]);
            out[bone].S[k] = static_cast<float>(in[bone].Scale[k]);
        }
    }
    g_capture.Filled[index][frame] = 1;
}

void OnFightFound()
{
    // If the stage was loaded again its actors went with it. If it wasn't, they're still there and have to go.
    for (Clone& clone : g_clones) {
        DestroyClone(clone);
        clone.Failed.clear();
    }
    g_skeletons.clear(); // the new fight's meshes are new objects
}

void OnGameTick()
{
    UpdateTakeRecorder();
    const bool inFight = game::Fighter(0) != nullptr;
    for (int i = 0; i < MaxActors; ++i)
        UpdateActor(i, inFight);
}

} // namespace actors
