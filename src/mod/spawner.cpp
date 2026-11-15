#include "mod/spawner.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "common/cinemath.h"
#include "mod/camera.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace spawner {

namespace {

std::array<Prop, MaxProps> g_props;
std::array<Effect, MaxEffects> g_effects;

std::mutex g_textMutex;
std::array<std::string, MaxProps> g_propStatus;
std::array<std::string, MaxEffects> g_effectStatus;
std::vector<std::string> g_meshNames, g_effectNames;
std::atomic<bool> g_listsWanted { false };
std::atomic<int> g_restartEffect { -1 };

void SetStatus(std::string& slot, const char* text)
{
    std::lock_guard lock(g_textMutex);
    if (slot != text)
        slot = text;
}

// A stage is still loading its meshes and effects in the first seconds of a fight, so something that isn't found is looked for again a
// few times, a couple of seconds apart, before it's given up on. Looking means going through everything the game has loaded.
constexpr int MostTries = 8, TicksBetweenTries = 120;

struct LiveProp {
    void* Actor = nullptr;
    void* Component = nullptr;
    uint32_t Index = 0;
    std::string Asset;
    std::string Failed; // an asset that couldn't be found, so it isn't looked for again every frame
    int Tries = 0, Retry = 0;
    cine::Vec3 Location, Scale;
    cine::Rot Rotation;
    bool Shadows = true;
    bool Placed = false;
};
std::array<LiveProp, MaxProps> g_liveProps;

struct LiveEffect {
    void* Component = nullptr;
    uint32_t Index = 0;
    std::string Asset;
    std::string Failed;
    int Tries = 0, Retry = 0;
    cine::Vec3 Location;
    cine::Rot Rotation;
    float Scale = 1;
    bool OnTimeline = false;
    bool Visible = true;
    bool Placed = false;
};
std::array<LiveEffect, MaxEffects> g_liveEffects;

// The asset by name among what's loaded, or by path from the game's files.
void* FindAsset(const char* className, const std::string& asset)
{
    if (void* loaded = ue::FindLoaded(className, asset))
        return loaded;
    if (asset.find('/') == std::string::npos)
        return nullptr;
    void* object = ue::LoadAsset(asset);
    void* cls = ue::FindClass(className);
    if (object && cls && ue::IsA(object, cls)) {
        mod::Log("Spawner: loaded %s", asset.c_str());
        return object;
    }
    return nullptr;
}

cine::Vec3 Anchor(int follow)
{
    cine::Vec3 anchor, p1, p2;
    const bool have1 = game::FighterLocation(0, p1), have2 = game::FighterLocation(1, p2);
    const Follow what = static_cast<Follow>(follow);
    if (what == Follow::P1 && have1)
        anchor = p1;
    else if (what == Follow::P2 && have2)
        anchor = p2;
    else if (what == Follow::Between && have1 && have2)
        anchor = cine::Lerp(p1, p2, 0.5);
    return anchor;
}

bool Same(cine::Vec3 a, cine::Vec3 b)
{
    return a.X == b.X && a.Y == b.Y && a.Z == b.Z;
}

bool Same(cine::Rot a, cine::Rot b)
{
    return a.Pitch == b.Pitch && a.Yaw == b.Yaw && a.Roll == b.Roll;
}

void DestroyProp(LiveProp& live)
{
    if (live.Actor && ue::StillAlive(live.Component, live.Index))
        ue::DestroyActor(live.Actor);
    const std::string failed = live.Failed;
    live = {};
    live.Failed = failed;
}

void UpdateProp(int i, bool inFight)
{
    const Prop& prop = g_props[i];
    LiveProp& live = g_liveProps[i];
    const std::string asset = prop.Asset;
    if (!prop.On || asset.empty() || !inFight) {
        if (live.Component && inFight)
            DestroyProp(live);
        if (!prop.On || asset.empty()) {
            live.Failed.clear();
            live.Tries = 0;
            SetStatus(g_propStatus[i], "");
        }
        return;
    }
    if (live.Component && (!ue::StillAlive(live.Component, live.Index) || live.Asset != asset))
        DestroyProp(live);
    if (!live.Component) {
        if (live.Failed != asset)
            live.Tries = 0;
        else if (live.Tries >= MostTries || --live.Retry > 0)
            return;
        void* mesh = FindAsset("StaticMesh", asset);
        if (!mesh) {
            live.Failed = asset;
            ++live.Tries;
            live.Retry = TicksBetweenTries;
            SetStatus(g_propStatus[i], asset.find('/') == std::string::npos ? "This mesh isn't loaded on this stage" : "The game couldn't load this mesh");
            return;
        }
        void* actor = ue::SpawnActor("StaticMeshActor");
        void* cls = ue::FindClass("StaticMeshActor");
        const int at = cls ? ue::PropertyOffset(cls, "StaticMeshComponent") : -1;
        void* component = actor && at >= 0 ? ue::Field<void*>(actor, at) : nullptr;
        if (!component) {
            live.Failed = asset;
            SetStatus(g_propStatus[i], "The game wouldn't make a prop");
            if (actor)
                ue::DestroyActor(actor);
            return;
        }
        ue::SetMovable(component);
        struct {
            void* Mesh;
            bool ReturnValue;
        } set { mesh, false };
        static ue::FunctionRef setMesh { "StaticMeshComponent", "SetStaticMesh" };
        ue::Call(component, setMesh, &set);
        // A prop is there to be looked at: nothing should bump into it.
        struct {
            bool Value;
        } collision { false };
        static ue::FunctionRef setCollision { "Actor", "SetActorEnableCollision" };
        ue::Call(actor, setCollision, &collision);
        live.Actor = actor;
        live.Component = component;
        live.Index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        live.Asset = asset;
        live.Failed.clear();
        live.Placed = false;
        SetStatus(g_propStatus[i], "");
        mod::Log("Spawner: prop %d is %s", i + 1, asset.c_str());
    }

    const cine::Vec3 location = Anchor(prop.Follow) + cine::Vec3 { prop.Location[0], prop.Location[1], prop.Location[2] };
    const cine::Rot rotation { prop.Rotation[0], prop.Rotation[1], prop.Rotation[2] };
    const cine::Vec3 scale { prop.Scale[0], prop.Scale[1], prop.Scale[2] };
    if (!live.Placed || !Same(location, live.Location) || !Same(rotation, live.Rotation)) {
        ue::SetActorPose(live.Actor, location, rotation);
        live.Location = location;
        live.Rotation = rotation;
    }
    if (!live.Placed || !Same(scale, live.Scale)) {
        ue::SetActorScale(live.Actor, scale);
        live.Scale = scale;
    }
    if (!live.Placed || live.Shadows != prop.Shadows) {
        struct {
            bool Value;
        } shadows { prop.Shadows };
        static ue::FunctionRef setShadow { "PrimitiveComponent", "SetCastShadow" };
        ue::Call(live.Component, setShadow, &shadows);
        live.Shadows = prop.Shadows;
    }
    live.Placed = true;
}

void* SpawnSystem(void* system)
{
    void* library = ue::FindClass("NiagaraFunctionLibrary");
    void* function = library ? ue::FindFunction(library, "SpawnSystemAtLocation") : nullptr;
    if (!function)
        return nullptr;
    const int worldAt = ue::PropertyOffset(function, "WorldContextObject"), systemAt = ue::PropertyOffset(function, "SystemTemplate");
    const int scaleAt = ue::PropertyOffset(function, "Scale"), destroyAt = ue::PropertyOffset(function, "bAutoDestroy");
    const int activateAt = ue::PropertyOffset(function, "bAutoActivate"), returnAt = ue::PropertyOffset(function, "ReturnValue");
    if (worldAt < 0 || systemAt < 0 || scaleAt < 0 || destroyAt < 0 || activateAt < 0 || returnAt < 0 || returnAt > 0xF0)
        return nullptr;
    alignas(16) uint8_t params[0x100] = {};
    *reinterpret_cast<void**>(params + worldAt) = ue::World();
    *reinterpret_cast<void**>(params + systemAt) = system;
    *reinterpret_cast<cine::Vec3*>(params + scaleAt) = { 1, 1, 1 };
    params[destroyAt] = 0; // it stays, so it can be played again
    params[activateAt] = 1;
    static ue::FunctionRef ref { "NiagaraFunctionLibrary", "SpawnSystemAtLocation" };
    ue::Call(nullptr, ref, params);
    return *reinterpret_cast<void**>(params + returnAt);
}

void DestroyEffect(LiveEffect& live)
{
    if (live.Component && ue::StillAlive(live.Component, live.Index)) {
        static ue::FunctionRef destroy { "ActorComponent", "K2_DestroyComponent" };
        struct {
            void* Object;
        } params { live.Component };
        ue::Call(live.Component, destroy, &params);
    }
    const std::string failed = live.Failed;
    live = {};
    live.Failed = failed;
}

void SetVisible(void* component, bool visible)
{
    struct {
        bool Visible, Propagate;
    } params { visible, true };
    static ue::FunctionRef ref { "SceneComponent", "SetVisibility" };
    ue::Call(component, ref, &params);
}

void UpdateEffect(int i, bool inFight)
{
    const Effect& effect = g_effects[i];
    LiveEffect& live = g_liveEffects[i];
    const std::string asset = effect.Asset;
    if (!effect.On || asset.empty() || !inFight) {
        if (live.Component && inFight)
            DestroyEffect(live);
        if (!effect.On || asset.empty()) {
            live.Failed.clear();
            live.Tries = 0;
            SetStatus(g_effectStatus[i], "");
        }
        return;
    }
    if (live.Component && (!ue::StillAlive(live.Component, live.Index) || live.Asset != asset))
        DestroyEffect(live);
    if (!live.Component) {
        if (live.Failed != asset)
            live.Tries = 0;
        else if (live.Tries >= MostTries || --live.Retry > 0)
            return;
        void* system = FindAsset("NiagaraSystem", asset);
        void* component = system ? SpawnSystem(system) : nullptr;
        if (!component) {
            live.Failed = asset;
            ++live.Tries;
            live.Retry = TicksBetweenTries;
            SetStatus(g_effectStatus[i], !system ? (asset.find('/') == std::string::npos ? "This effect isn't loaded in this fight" : "The game couldn't load this effect")
                                                 : "The game wouldn't start this effect");
            return;
        }
        live.Component = component;
        live.Index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        live.Asset = asset;
        live.Failed.clear();
        live.Placed = false;
        live.OnTimeline = false;
        live.Visible = true;
        // While it's catching up to a moment it keeps drawing, and it catches up in game steps.
        struct {
            bool Value;
        } render { true };
        static ue::FunctionRef canRender { "NiagaraComponent", "SetCanRenderWhileSeeking" };
        ue::Call(component, canRender, &render);
        struct {
            float Value;
        } delta { static_cast<float>(game::StepSeconds) };
        static ue::FunctionRef seekDelta { "NiagaraComponent", "SetSeekDelta" };
        ue::Call(component, seekDelta, &delta);
        SetStatus(g_effectStatus[i], "");
        mod::Log("Spawner: effect %d is %s", i + 1, asset.c_str());
    }

    const cine::Vec3 location = Anchor(effect.Follow) + cine::Vec3 { effect.Location[0], effect.Location[1], effect.Location[2] };
    const cine::Rot rotation { effect.Rotation[0], effect.Rotation[1], effect.Rotation[2] };
    if (!live.Placed || !Same(location, live.Location) || !Same(rotation, live.Rotation)) {
        ue::SetComponentPose(live.Component, location, rotation);
        live.Location = location;
        live.Rotation = rotation;
    }
    if (!live.Placed || live.Scale != effect.Scale) {
        ue::SetComponentScale(live.Component, { effect.Scale, effect.Scale, effect.Scale });
        live.Scale = effect.Scale;
    }

    if (!live.Placed || live.OnTimeline != effect.OnTimeline) {
        struct {
            uint8_t Mode;
        } mode { static_cast<uint8_t>(effect.OnTimeline ? 1 : 0) }; // 1 = the age it's told, 0 = the engine's own time
        static ue::FunctionRef setMode { "NiagaraComponent", "SetAgeUpdateMode" };
        ue::Call(live.Component, setMode, &mode);
        live.OnTimeline = effect.OnTimeline;
    }
    bool visible = true;
    if (effect.OnTimeline) {
        const double age = (game::Clock() - effect.Start) * effect.Rate;
        visible = age >= 0 && (effect.Length <= 0 || age <= effect.Length);
        struct {
            float Value;
        } desired { static_cast<float>(std::max(age, 0.0)) };
        static ue::FunctionRef setAge { "NiagaraComponent", "SetDesiredAge" };
        ue::Call(live.Component, setAge, &desired);
    }
    if (!live.Placed || visible != live.Visible) {
        SetVisible(live.Component, visible);
        live.Visible = visible;
    }
    live.Placed = true;

    if (g_restartEffect.load() == i) {
        g_restartEffect = -1;
        static ue::FunctionRef reinitialize { "NiagaraComponent", "ReinitializeSystem" };
        ue::Call(live.Component, reinitialize, nullptr);
    }
}

void InFrontOfCamera(float location[3], float rotation[3])
{
    const camera::View view = camera::LastView();
    if (!view.Valid)
        return;
    const cine::Vec3 at = view.Location + cine::ForwardOf(view.Rotation) * 300.0;
    location[0] = static_cast<float>(at.X);
    location[1] = static_cast<float>(at.Y);
    location[2] = static_cast<float>(at.Z);
    rotation[0] = 0;
    rotation[1] = static_cast<float>(cine::WrapAngle(view.Rotation.Yaw + 180.0)); // facing the camera
    rotation[2] = 0;
}

} // namespace

std::array<Prop, MaxProps>& Props()
{
    return g_props;
}

std::array<Effect, MaxEffects>& Effects()
{
    return g_effects;
}

void PlacePropAtCamera(int index)
{
    if (index < 0 || index >= MaxProps)
        return;
    Prop& prop = g_props[index];
    prop.Follow = 0;
    InFrontOfCamera(prop.Location, prop.Rotation);
    prop.On = true;
}

void PlaceEffectAtCamera(int index)
{
    if (index < 0 || index >= MaxEffects)
        return;
    Effect& effect = g_effects[index];
    effect.Follow = 0;
    InFrontOfCamera(effect.Location, effect.Rotation);
    effect.Start = game::Clock();
    effect.On = true;
}

void RestartEffect(int index)
{
    g_restartEffect = index;
}

void AskForLists()
{
    g_listsWanted = true;
}

std::vector<std::string> MeshNames()
{
    std::lock_guard lock(g_textMutex);
    return g_meshNames;
}

std::vector<std::string> EffectNames()
{
    std::lock_guard lock(g_textMutex);
    return g_effectNames;
}

std::string PropStatus(int index)
{
    std::lock_guard lock(g_textMutex);
    return index >= 0 && index < MaxProps ? g_propStatus[index] : std::string();
}

std::string EffectStatus(int index)
{
    std::lock_guard lock(g_textMutex);
    return index >= 0 && index < MaxEffects ? g_effectStatus[index] : std::string();
}

bool IsPlacedEffect(const void* component)
{
    for (const LiveEffect& live : g_liveEffects)
        if (live.Component && live.Component == component)
            return true;
    return false;
}

void OnFightFound()
{
    // If the stage was loaded again, what was on it went with it. If it wasn't, it's still there and has to go before it's made again.
    for (LiveProp& live : g_liveProps) {
        DestroyProp(live);
        live = {};
    }
    for (LiveEffect& live : g_liveEffects) {
        DestroyEffect(live);
        live = {};
    }
}

void OnGameTick()
{
    const bool inFight = game::Fighter(0) != nullptr;
    // Walking the object list is only safe while a fight is on.
    if (inFight && g_listsWanted.exchange(false)) {
        std::vector<std::string> meshes = ue::LoadedNames("StaticMesh"), effects = ue::LoadedNames("NiagaraSystem");
        std::lock_guard lock(g_textMutex);
        g_meshNames = std::move(meshes);
        g_effectNames = std::move(effects);
    }
    for (int i = 0; i < MaxProps; ++i)
        UpdateProp(i, inFight);
    for (int i = 0; i < MaxEffects; ++i)
        UpdateEffect(i, inFight);
}

} // namespace spawner
