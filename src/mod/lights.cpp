#include "mod/lights.h"

#include <algorithm>
#include <cstring>

#include "common/cinemath.h"
#include "mod/camera.h"
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
    Light Applied;
    bool HaveApplied = false;
};
std::array<Live, MaxLights> g_live;

struct LinearColor {
    float R, G, B, A;
};

// A light on a fighter never shows: the fighter's own actor is hidden, and that hides everything on it. So each light is an actor of its own.
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
    ue::SetMovable(light);
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

// Only what changed is sent to the engine, since look keys can move these every frame.
void Configure(Live& live, const Light& light)
{
    void* component = live.Component;
    const Light& was = live.Applied;
    const bool all = !live.HaveApplied;
    struct {
        float Value;
    } value {};
    if (all || was.Intensity != light.Intensity) {
        value.Value = light.Intensity;
        static ue::FunctionRef setIntensity { "LightComponent", "SetIntensity" };
        ue::Call(component, setIntensity, &value);
    }
    if (all || was.Radius != light.Radius) {
        value.Value = light.Radius;
        static ue::FunctionRef setRadius { "LocalLightComponent", "SetAttenuationRadius" };
        ue::Call(component, setRadius, &value);
    }
    if (all || std::memcmp(was.Color, light.Color, sizeof(light.Color)) != 0) {
        struct {
            LinearColor Value;
            bool SRGB;
        } color { { light.Color[0], light.Color[1], light.Color[2], 1 }, true };
        static ue::FunctionRef setColor { "LightComponent", "SetLightColor" };
        ue::Call(component, setColor, &color);
    }
    if (all || was.Shadows != light.Shadows) {
        struct {
            bool Value;
        } shadows { light.Shadows };
        static ue::FunctionRef setShadows { "LightComponent", "SetCastShadows" };
        ue::Call(component, setShadows, &shadows);
    }
    if (light.Spot && (all || was.Cone != light.Cone)) {
        value.Value = std::clamp(light.Cone, 1.0f, 80.0f);
        static ue::FunctionRef setOuter { "SpotLightComponent", "SetOuterConeAngle" };
        ue::Call(component, setOuter, &value);
        value.Value *= 0.6f; // a soft edge
        static ue::FunctionRef setInner { "SpotLightComponent", "SetInnerConeAngle" };
        ue::Call(component, setInner, &value);
    }
    live.Applied = light;
    live.HaveApplied = true;
}

void Destroy(Live& live)
{
    if (live.Actor && ue::StillAlive(live.Component, live.Index))
        ue::DestroyActor(live.Actor);
    live = {};
}

} // namespace

bool IsRigLight(const void* component)
{
    for (const Live& live : g_live)
        if (live.Component && live.Component == component)
            return true;
    return false;
}

std::array<Light, MaxLights>& Rig()
{
    return g_rig;
}

void PlaceAtCamera(int index)
{
    const camera::View view = camera::LastView();
    if (!view.Valid || index < 0 || index >= MaxLights)
        return;
    Light& light = g_rig[index];
    light.On = true;
    light.Follow = Follow::World;
    light.Aim = Aim::Direction;
    light.Offset[0] = static_cast<float>(view.Location.X);
    light.Offset[1] = static_cast<float>(view.Location.Y);
    light.Offset[2] = static_cast<float>(view.Location.Z);
    light.Direction[0] = static_cast<float>(view.Rotation.Pitch);
    light.Direction[1] = static_cast<float>(cine::WrapAngle(view.Rotation.Yaw));
}

void OnFightFound()
{
    // If the stage was loaded again its lights went with it. If it wasn't, they're still there and have to go before they're made again.
    for (Live& live : g_live)
        Destroy(live);
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
            live.Actor = ue::SpawnActor(light.Spot ? "SpotLight" : "PointLight");
            live.Component = live.Actor ? LightOf(live.Actor) : nullptr;
            if (!live.Component)
                continue;
            live.Index = ue::Field<uint32_t>(live.Component, ue::off::InternalIndex);
            live.Spot = light.Spot;
            mod::Log("Lights: added light %d", i + 1);
        }
        Configure(live, light);

        cine::Vec3 anchor, p1, p2;
        const bool have1 = game::FighterLocation(0, p1), have2 = game::FighterLocation(1, p2);
        if (light.Follow == Follow::P1 && have1)
            anchor = p1;
        else if (light.Follow == Follow::P2 && have2)
            anchor = p2;
        else if (light.Follow == Follow::Between && have1 && have2)
            anchor = cine::Lerp(p1, p2, 0.5);
        const cine::Vec3 location = anchor + cine::Vec3 { light.Offset[0], light.Offset[1], light.Offset[2] };

        cine::Rot rotation { light.Direction[0], light.Direction[1], 0 };
        if (light.Spot && light.Aim != Aim::Direction) {
            // A fighter's location is at their feet, so the light aims at about chest height.
            cine::Vec3 target = light.Aim == Aim::P1 ? p1 : (light.Aim == Aim::P2 ? p2 : cine::Lerp(p1, p2, 0.5));
            target.Z += 100;
            if ((light.Aim == Aim::P1 && have1) || (light.Aim == Aim::P2 && have2) || (light.Aim == Aim::Between && have1 && have2))
                rotation = cine::LookAt(location, target);
        }
        ue::SetComponentPose(live.Component, location, rotation);
    }
}

} // namespace lights
