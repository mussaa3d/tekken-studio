#include "mod/lights.h"

#include <cstring>

#include "common/cinemath.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace lights {

namespace {

std::array<Light, MaxLights> g_rig;

struct Live {
    void* Actor = nullptr;
    void* Component = nullptr;
    uint32_t Index = 0;
    bool Spot = false;
};
std::array<Live, MaxLights> g_live;
bool g_warned = false;

struct LinearColor {
    float R, G, B, A;
};

// Identity transform: rotation quaternion, translation, scale, four doubles each.
void WriteIdentity(uint8_t* at)
{
    auto* transform = reinterpret_cast<double*>(at);
    transform[3] = 1.0;
    transform[8] = transform[9] = transform[10] = 1.0;
}

// Spawns a light actor through the engine's own functions, with their parameter layout from reflection. A light on a fighter never
// shows: the fighter's own actor is hidden, and that hides everything on it.
void* SpawnLight(const char* className)
{
    void* library = ue::FindClass("GameplayStatics");
    void* cls = ue::FindClass(className);
    void* begin = library ? ue::FindFunction(library, "BeginDeferredActorSpawnFromClass") : nullptr;
    void* finish = library ? ue::FindFunction(library, "FinishSpawningActor") : nullptr;
    if (!cls || !begin || !finish)
        return nullptr;
    const int worldAt = ue::PropertyOffset(begin, "WorldContextObject");
    const int classAt = ue::PropertyOffset(begin, "ActorClass");
    const int transformAt = ue::PropertyOffset(begin, "SpawnTransform");
    const int collisionAt = ue::PropertyOffset(begin, "CollisionHandlingOverride");
    const int returnAt = ue::PropertyOffset(begin, "ReturnValue");
    const int finishActorAt = ue::PropertyOffset(finish, "Actor");
    const int finishTransformAt = ue::PropertyOffset(finish, "SpawnTransform");
    if (worldAt < 0 || classAt < 0 || transformAt < 0 || returnAt < 0 || finishActorAt < 0 || finishTransformAt < 0) {
        if (!g_warned)
            mod::Log("Lights: the spawn functions have an unexpected layout");
        g_warned = true;
        return nullptr;
    }
    alignas(16) uint8_t params[0x200] = {};
    *reinterpret_cast<void**>(params + worldAt) = ue::World();
    *reinterpret_cast<void**>(params + classAt) = cls;
    WriteIdentity(params + transformAt);
    if (collisionAt >= 0)
        params[collisionAt] = 1; // always spawn
    static ue::FunctionRef beginRef { "GameplayStatics", "BeginDeferredActorSpawnFromClass" };
    ue::Call(nullptr, beginRef, params);
    void* actor = *reinterpret_cast<void**>(params + returnAt);
    if (!actor)
        return nullptr;

    alignas(16) uint8_t finishParams[0x200] = {};
    *reinterpret_cast<void**>(finishParams + finishActorAt) = actor;
    WriteIdentity(finishParams + finishTransformAt);
    static ue::FunctionRef finishRef { "GameplayStatics", "FinishSpawningActor" };
    ue::Call(nullptr, finishRef, finishParams);
    return actor;
}

void* LightOf(void* actor)
{
    void* base = ue::FindClass("Light");
    const int at = base ? ue::PropertyOffset(base, "LightComponent") : -1;
    if (at < 0)
        return nullptr;
    void* light = ue::Field<void*>(actor, at);
    if (!light)
        return nullptr;

    // A light has to be movable to follow anything, and fighters can sit on their own lighting channel.
    struct {
        uint8_t Mobility;
    } mobility { 2 };
    static ue::FunctionRef setMobility { "SceneComponent", "SetMobility" };
    ue::Call(light, setMobility, &mobility);
    struct {
        bool Channel0, Channel1, Channel2;
    } channels { true, true, true };
    static ue::FunctionRef setChannels { "LightComponent", "SetLightingChannels" };
    ue::Call(light, setChannels, &channels);
    struct {
        bool Visible, Propagate;
    } visible { true, false };
    static ue::FunctionRef setVisibility { "SceneComponent", "SetVisibility" };
    ue::Call(light, setVisibility, &visible);
    return light;
}

void SetWorldLocation(void* component, cine::Vec3 location)
{
    static ue::FunctionRef setLocation { "SceneComponent", "K2_SetWorldLocation" };
    alignas(16) uint8_t params[0x400] = {}; // room for the FHitResult output the function also takes
    *reinterpret_cast<cine::Vec3*>(params) = location;
    ue::Call(component, setLocation, params);
}

void Configure(void* component, const Light& light)
{
    struct {
        float Value;
    } intensity { light.Intensity }, radius { light.Radius };
    static ue::FunctionRef setIntensity { "LightComponent", "SetIntensity" };
    static ue::FunctionRef setRadius { "LocalLightComponent", "SetAttenuationRadius" };
    static ue::FunctionRef setColor { "LightComponent", "SetLightColor" };
    ue::Call(component, setIntensity, &intensity);
    ue::Call(component, setRadius, &radius);
    struct {
        LinearColor Value;
        bool SRGB;
    } color { { light.Color[0], light.Color[1], light.Color[2], 1 }, true };
    ue::Call(component, setColor, &color);
}

void Destroy(Live& live)
{
    if (live.Actor && ue::StillAlive(live.Component, live.Index))
        ue::DestroyActor(live.Actor);
    live = {};
}

} // namespace

std::array<Light, MaxLights>& Rig()
{
    return g_rig;
}

void OnFightFound()
{
    g_live = {}; // the old fight's actors went with their lights
}

void OnGameTick()
{
    const bool inFight = game::Fighter(0) != nullptr;
    for (int i = 0; i < MaxLights; ++i) {
        const Light& light = g_rig[i];
        Live& live = g_live[i];
        if (!light.On || !inFight) {
            if (live.Component && inFight)
                Destroy(live);
            continue;
        }
        if (live.Component && (!ue::StillAlive(live.Component, live.Index) || live.Spot != light.Spot))
            Destroy(live);
        if (!live.Component) {
            live.Actor = SpawnLight(light.Spot ? "SpotLight" : "PointLight");
            live.Component = live.Actor ? LightOf(live.Actor) : nullptr;
            if (!live.Component)
                continue;
            live.Index = ue::Field<uint32_t>(live.Component, ue::off::InternalIndex);
            live.Spot = light.Spot;
            mod::Log("Lights: added light %d", i + 1);
        }
        Configure(live.Component, light);

        cine::Vec3 anchor, p1, p2;
        const bool have1 = game::FighterLocation(0, p1), have2 = game::FighterLocation(1, p2);
        if (light.Follow == Follow::P1 && have1)
            anchor = p1;
        else if (light.Follow == Follow::P2 && have2)
            anchor = p2;
        else if (light.Follow == Follow::Between && have1 && have2)
            anchor = cine::Lerp(p1, p2, 0.5);
        SetWorldLocation(live.Component, anchor + cine::Vec3 { light.Offset[0], light.Offset[1], light.Offset[2] });
    }
}

} // namespace lights
