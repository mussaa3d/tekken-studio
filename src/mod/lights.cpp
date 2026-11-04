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
    void* Component = nullptr;
    uint32_t Index = 0;
    bool Spot = false;
};
std::array<Live, MaxLights> g_live;
bool g_warned = false;

struct LinearColor {
    float R, G, B, A;
};

// Adds a light component to a fighter through the engine's own function. Its parameter layout comes from reflection, so
// the struct below isn't assumed.
void* AddLight(void* owner, const char* className)
{
    static ue::FunctionRef addComponent { "Actor", "AddComponentByClass" };
    void* cls = ue::FindClass(className);
    void* actorClass = ue::FindClass("Actor");
    void* function = actorClass ? ue::FindFunction(actorClass, "AddComponentByClass") : nullptr;
    if (!cls || !function)
        return nullptr;
    const int classAt = ue::PropertyOffset(function, "Class");
    const int manualAt = ue::PropertyOffset(function, "bManualAttachment");
    const int transformAt = ue::PropertyOffset(function, "RelativeTransform");
    const int deferredAt = ue::PropertyOffset(function, "bDeferredFinish");
    const int returnAt = ue::PropertyOffset(function, "ReturnValue");
    if (classAt < 0 || transformAt < 0 || returnAt < 0) {
        if (!g_warned)
            mod::Log("Lights: AddComponentByClass has an unexpected layout");
        g_warned = true;
        return nullptr;
    }
    alignas(16) uint8_t params[0x200] = {};
    *reinterpret_cast<void**>(params + classAt) = cls;
    if (manualAt >= 0)
        params[manualAt] = 0;
    if (deferredAt >= 0)
        params[deferredAt] = 0;
    // Identity transform: rotation quaternion, translation, scale, four doubles each.
    auto* transform = reinterpret_cast<double*>(params + transformAt);
    transform[3] = 1.0;
    transform[8] = transform[9] = transform[10] = 1.0;
    ue::Call(owner, addComponent, params);
    return *reinterpret_cast<void**>(params + returnAt);
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
    if (live.Component && ue::StillAlive(live.Component, live.Index)) {
        static ue::FunctionRef destroy { "ActorComponent", "K2_DestroyComponent" };
        struct {
            void* Object;
        } params { live.Component };
        ue::Call(live.Component, destroy, &params);
    }
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
    void* owner = game::Fighter(0);
    for (int i = 0; i < MaxLights; ++i) {
        const Light& light = g_rig[i];
        Live& live = g_live[i];
        if (!light.On || !owner) {
            if (live.Component && owner)
                Destroy(live);
            continue;
        }
        if (live.Component && (!ue::StillAlive(live.Component, live.Index) || live.Spot != light.Spot))
            Destroy(live);
        if (!live.Component) {
            live.Component = AddLight(owner, light.Spot ? "SpotLightComponent" : "PointLightComponent");
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
