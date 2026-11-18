#include "mod/scene.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/cinemath.h"
#include "mod/effects.h"
#include "mod/hudframe.h"
#include "mod/window.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace scene {

namespace {

Settings g_settings;
Counts g_counts;

struct LinearColor {
    float R, G, B, A;
};

// ----- Lights. The fighters have spot lights of their own, on a lighting channel the stage doesn't use (or owned by the fighter).
struct LightState {
    uint32_t Index = 0;
    bool Character = false;
    bool WasVisible = true;
    bool Hidden = false;
};
std::unordered_map<void*, LightState> g_lights;
int g_lightRescan = 0;

void SetVisible(void* component, bool visible)
{
    struct {
        bool Visible, Propagate;
    } params { visible, false };
    static ue::FunctionRef ref { "SceneComponent", "SetVisibility" };
    ue::Call(component, ref, &params);
}

bool IsCharacterLight(void* light, void* owner)
{
    if (owner && effects::IsFighterPart(owner))
        return true;
    // The fighters' key and back lights hang off this controller; some of them sit on the stage's channel too.
    if (owner && ue::ClassNameOf(owner).find("CharacterLight") != std::string::npos)
        return true;
    static void* base = ue::FindClass("LightComponent"); // LightingChannels is on LightComponent, not its base
    static const int channelsAt = base ? ue::PropertyOffset(base, "LightingChannels") : -1;
    static void* channelsStruct = ue::FindStruct("LightingChannels");
    static int bytes[3] = { -1, -1, -1 };
    static uint8_t masks[3] = {};
    static const bool found = [] {
        bool all = channelsStruct != nullptr;
        for (int i = 0; i < 3 && all; ++i)
            all = ue::BoolProperty(channelsStruct, "bChannel" + std::to_string(i), bytes[i], masks[i]);
        return all;
    }();
    if (channelsAt < 0 || !found)
        return false;
    bool on[3];
    for (int i = 0; i < 3; ++i)
        on[i] = (ue::Field<uint8_t>(light, channelsAt + bytes[i]) & masks[i]) != 0;
    return !on[0] && (on[1] || on[2]);
}

void UpdateLights(bool rescan)
{
    void* cls = ue::FindClass("LightComponent");
    if (!cls)
        return;
    static void* scene = ue::FindClass("SceneComponent");
    static int visibleByte = -1;
    static uint8_t visibleMask = 0;
    if (visibleByte < 0 && scene)
        ue::BoolProperty(scene, "bVisible", visibleByte, visibleMask);

    if (rescan) {
        int characters = 0, stage = 0;
        std::map<std::string, int> owners;
        for (void* light : ue::AllObjectsOf(cls)) {
            if (ue::Field<uint32_t>(light, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            if (lights::IsRigLight(light))
                continue;
            const uint32_t index = ue::Field<uint32_t>(light, ue::off::InternalIndex);
            auto it = g_lights.find(light);
            if (it != g_lights.end() && it->second.Index != index)
                g_lights.erase(it), it = g_lights.end();
            if (it == g_lights.end()) {
                LightState state;
                state.Index = index;
                state.Character = IsCharacterLight(light, effects::OwnerOf(light));
                state.WasVisible = visibleByte < 0 || (ue::Field<uint8_t>(light, visibleByte) & visibleMask);
                it = g_lights.emplace(light, state).first;
            }
            ++(it->second.Character ? characters : stage);
            void* owner = effects::OwnerOf(light);
            ++owners[(it->second.Character ? "char " : "stage ") + ue::ClassNameOf(light) + " on " + (owner ? ue::ClassNameOf(owner) : std::string("nothing"))];
        }
        for (auto it = g_lights.begin(); it != g_lights.end();)
            it = ue::StillAlive(it->first, it->second.Index) ? std::next(it) : g_lights.erase(it);
        if (characters != g_counts.CharacterLights || stage != g_counts.StageLights) {
            mod::Log("Scene: %d character lights, %d stage lights", characters, stage);
            for (const auto& [what, count] : owners)
                mod::Log("Scene:   %d x %s", count, what.c_str());
        }
        g_counts.CharacterLights = characters;
        g_counts.StageLights = stage;
    }

    for (auto& [light, state] : g_lights) {
        const bool hide = state.Character ? g_settings.NoCharacterLights : g_settings.NoStageLights;
        if (hide != state.Hidden && state.WasVisible) {
            SetVisible(light, !hide);
            state.Hidden = hide;
        }
    }
}

// ----- The fighters' own light. Most of it isn't a light at all: the stage's BP_CharacterLightController sets a colour scale on the
// fighters' materials. Measured on Jin: writing its CharacterMainLightColorScaleExt and running its own two update functions tints the
// fighters and nothing else (red minus green on his jacket went from 17 to 48).
struct ControllerState {
    uint32_t Index = 0;
    LinearColor Original { 1, 1, 1, 1 };
};
std::unordered_map<void*, ControllerState> g_controllers;
bool g_controllerApplied = false;
int g_controllerRefresh = 0;

void RunController(void* controller)
{
    static ue::FunctionRef scale { "BP_CharacterLightController_C", "UpdateCharactermainLightColorScale" };
    static ue::FunctionRef material { "BP_CharacterLightController_C", "UpdateCharacterMaterialMPC" };
    uint8_t params[64] = {};
    ue::Call(controller, scale, params);
    ue::Call(controller, material, params);
}

void UpdateController()
{
    const bool wanted = g_settings.TintCharacterLights || g_settings.NoCharacterLights;
    if (!wanted && !g_controllerApplied)
        return;
    void* cls = ue::FindClass("BP_CharacterLightController_C");
    const int at = cls ? ue::PropertyOffset(cls, "CharacterMainLightColorScaleExt") : -1;
    if (at < 0)
        return;
    if (!wanted) {
        for (const auto& [controller, state] : g_controllers)
            if (ue::StillAlive(controller, state.Index)) {
                ue::Field<LinearColor>(controller, at) = state.Original;
                RunController(controller);
            }
        g_controllers.clear();
        g_controllerApplied = false;
        return;
    }
    // Each change, and now and then anyway, since the controller reruns when the battle or a drama light starts.
    static float last[4] = { -1, -1, -1, -1 };
    const float* tint = g_settings.CharacterLightColor;
    const LinearColor colour = g_settings.NoCharacterLights ? LinearColor { 0, 0, 0, 1 } : LinearColor { tint[0], tint[1], tint[2], 1 };
    const bool changed = colour.R != last[0] || colour.G != last[1] || colour.B != last[2] || !g_controllerApplied;
    last[0] = colour.R;
    last[1] = colour.G;
    last[2] = colour.B;
    if (!changed && --g_controllerRefresh > 0)
        return;
    g_controllerRefresh = 30;
    for (void* controller : ue::AllObjectsOf(cls)) {
        if (ue::Field<uint32_t>(controller, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        const uint32_t index = ue::Field<uint32_t>(controller, ue::off::InternalIndex);
        auto it = g_controllers.find(controller);
        if (it == g_controllers.end() || it->second.Index != index)
            g_controllers[controller] = { index, ue::Field<LinearColor>(controller, at) };
        ue::Field<LinearColor>(controller, at) = colour;
        RunController(controller);
    }
    g_controllerApplied = true;
}

// ----- The plain stage: every stage mesh drawn in the engine's default material, as TekkenOverlay's simplified stages do.
struct PlainMesh {
    uint32_t Index = 0;
    std::vector<void*> Materials; // what it had, to put back
};
std::unordered_map<void*, PlainMesh> g_plain;
void* g_plainMaterial = nullptr;
uint32_t g_plainMaterialIndex = 0;
uint32_t g_meshCursor = 0;
bool g_plainApplied = false;

// Materials held alive while they're swapped out, each once, however many meshes use it.
std::unordered_map<void*, uint32_t> g_held;

void AddHold(void* material)
{
    if (!material || g_held.count(material))
        return;
    g_held[material] = ue::Field<uint32_t>(material, ue::off::InternalIndex);
    ue::KeepAlive(material, true);
}

void ReleaseHolds()
{
    for (const auto& [material, index] : g_held)
        if (ue::StillAlive(material, index))
            ue::KeepAlive(material, false);
    g_held.clear();
}

void* PlainMaterial()
{
    if (g_plainMaterial && ue::StillAlive(g_plainMaterial, g_plainMaterialIndex))
        return g_plainMaterial;
    g_plainMaterial = nullptr;
    void* cls = ue::FindClass("Material");
    if (!cls)
        return nullptr;
    for (const char* name : { "WorldGridMaterial", "DefaultMaterial", "DefaultLitMaterial" }) {
        for (void* material : ue::AllObjectsOf(cls))
            if (ue::NameOf(material) == name) {
                g_plainMaterial = material;
                g_plainMaterialIndex = ue::Field<uint32_t>(material, ue::off::InternalIndex);
                mod::Log("Scene: plain stage uses %s", name);
                return material;
            }
    }
    mod::Log("Scene: no plain material found");
    return nullptr;
}

int NumMaterials(void* mesh)
{
    struct {
        int32_t ReturnValue;
    } params { 0 };
    static ue::FunctionRef ref { "PrimitiveComponent", "GetNumMaterials" };
    ue::Call(mesh, ref, &params);
    return params.ReturnValue;
}

void* GetMaterial(void* mesh, int slot)
{
    struct {
        int32_t Slot;
        void* ReturnValue;
    } params { slot, nullptr };
    static ue::FunctionRef ref { "PrimitiveComponent", "GetMaterial" };
    ue::Call(mesh, ref, &params);
    return params.ReturnValue;
}

void SetMaterial(void* mesh, int slot, void* material)
{
    struct {
        int32_t Slot;
        void* Material;
    } params { slot, material };
    static ue::FunctionRef ref { "PrimitiveComponent", "SetMaterial" };
    ue::Call(mesh, ref, &params);
}

void RestorePlain()
{
    for (auto& [mesh, plain] : g_plain)
        if (ue::StillAlive(mesh, plain.Index))
            for (size_t slot = 0; slot < plain.Materials.size(); ++slot)
                SetMaterial(mesh, static_cast<int>(slot), plain.Materials[slot]);
    g_plain.clear();
    ReleaseHolds();
    g_counts.PlainMeshes = 0;
    g_plainApplied = false;
}

void UpdatePlain()
{
    if (!g_settings.PlainStage) {
        if (g_plainApplied)
            RestorePlain();
        return;
    }
    void* material = PlainMaterial();
    void* cls = ue::FindClass("MeshComponent");
    if (!material || !cls)
        return;
    g_plainApplied = true;
    // A slice of the object array per tick: there are thousands of stage meshes, and new ones appear as the stage breaks.
    const uint32_t count = ue::ObjectCount();
    for (uint32_t n = 0; n < 8192 && count > 0; ++n) {
        if (g_meshCursor >= count)
            g_meshCursor = 0;
        const uint32_t index = g_meshCursor++;
        void* mesh = ue::ObjectAt(index);
        if (!mesh || !ue::ClassChainIsSound(mesh) || (ue::Field<uint32_t>(mesh, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject) || !ue::IsA(mesh, cls))
            continue;
        if (g_plain.count(mesh) && g_plain[mesh].Index == index)
            continue;
        const std::string className = ue::ClassNameOf(mesh);
        if (className.find("Widget") != std::string::npos)
            continue;
        void* owner = effects::OwnerOf(mesh);
        if (!owner || effects::IsFighterPart(owner))
            continue;
        PlainMesh plain;
        plain.Index = index;
        const int slots = NumMaterials(mesh);
        for (int slot = 0; slot < slots && slot < 64; ++slot) {
            void* original = GetMaterial(mesh, slot);
            // Once swapped out, a material only the stage used would be garbage collected, and putting it back would crash the game.
            // Held in the root set until it's back.
            AddHold(original);
            plain.Materials.push_back(original);
            SetMaterial(mesh, slot, material);
        }
        g_plain[mesh] = std::move(plain);
    }
    g_counts.PlainMeshes = static_cast<int>(g_plain.size());
}

// ----- The shape of the picture.
int g_aspectAt = -2, g_constrainByte = -1;
uint8_t g_constrainMask = 0;

} // namespace

Settings& Config()
{
    return g_settings;
}

Counts LastCounts()
{
    return g_counts;
}

void OnFightFound()
{
    // A new stage: its lights and meshes are new objects, and whatever was changed went with the old ones.
    g_lights.clear();
    g_plain.clear();
    ReleaseHolds();
    g_controllers.clear();
    g_controllerApplied = false;
    g_plainApplied = false;
    g_lightRescan = 0;
}

void OnGameTick()
{
    const bool lightsWanted = g_settings.NoCharacterLights || g_settings.NoStageLights;
    static bool lightsWereWanted = false;
    if (lightsWanted || lightsWereWanted) {
        UpdateLights(--g_lightRescan <= 0);
        if (g_lightRescan <= 0)
            g_lightRescan = 60;
    }
    lightsWereWanted = lightsWanted;
    UpdateController();
    UpdatePlain();

    // The HUD with the picture's shape.
    int width = 0, height = 0;
    window::PictureSize(width, height);
    const float aspect = g_settings.Frame == Frame::Fixed ? std::max(g_settings.AspectRatio, 0.2f)
                         : g_settings.Frame == Frame::Fill && height > 0 ? static_cast<float>(width) / height
                                                                          : 0.0f;
    hudframe::Apply(g_settings.Frame == Frame::Game ? hudframe::Mode::Game : static_cast<hudframe::Mode>(std::clamp(g_settings.HudFrame, 0, 2)), aspect, width, height);
}

void BeforeCameraUpdate(void* cameraManager)
{
    if (!g_settings.NoCameraShake || !cameraManager)
        return;
    struct {
        bool Immediately;
    } params { true };
    static ue::FunctionRef ref { "PlayerCameraManager", "StopAllCameraShakes" };
    ue::Call(cameraManager, ref, &params);
}

void AdjustView(void* view, float& fov, bool fromGame)
{
    if (g_settings.Frame == Frame::Game || !view)
        return;
    if (g_aspectAt == -2) {
        void* info = ue::FindStruct("MinimalViewInfo");
        g_aspectAt = info ? ue::PropertyOffset(info, "AspectRatio") : -1;
        if (!info || !ue::BoolProperty(info, "bConstrainAspectRatio", g_constrainByte, g_constrainMask))
            g_constrainByte = -1;
        mod::Log("Scene: aspect ratio at +0x%X, constrain at +0x%X", g_aspectAt, g_constrainByte);
    }
    if (g_aspectAt < 0 || g_constrainByte < 0)
        return;
    uint8_t& constrain = ue::Field<uint8_t>(view, g_constrainByte);
    if (g_settings.Frame == Frame::Fill) {
        constrain &= static_cast<uint8_t>(~g_constrainMask);
    } else {
        constrain |= g_constrainMask;
        ue::Field<float>(view, g_aspectAt) = std::max(g_settings.AspectRatio, 0.2f);
        // With the shape held, the engine takes the field of view across the width. The game widens its own camera's view for the
        // window's shape, between 16:10 and 21:9 (measured), so that comes off first, to give the view it has at 16:9.
        if (g_settings.KeepFighters) {
            double base = fov;
            if (fromGame) {
                int width = 0, height = 0;
                window::PictureSize(width, height);
                if (width > 0 && height > 0)
                    base = cine::FovAt16x9(fov, static_cast<double>(width) / height, 1.6, 7.0 / 3.0);
            }
            fov = static_cast<float>(cine::FovForAspect(base, std::max(g_settings.AspectRatio, 0.2f), g_settings.Fit));
        }
    }
}

} // namespace scene
