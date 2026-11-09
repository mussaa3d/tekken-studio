#include "mod/sky.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "mod/effects.h"
#include "mod/game.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace sky {

namespace fs = std::filesystem;

namespace {

Settings g_settings;
std::mutex g_statusMutex;
std::string g_status;

void SetStatus(std::string text)
{
    std::lock_guard lock(g_statusMutex);
    g_status = std::move(text);
}

struct Rotator {
    double Pitch = 0, Yaw = 0, Roll = 0;
};

struct LinearColor {
    float R, G, B, A;
};

bool Live(void* object)
{
    return !(ue::Field<uint32_t>(object, ue::off::ObjectFlags) & 0x30); // not a class default or a template
}

uint32_t IndexOf(void* object)
{
    return ue::Field<uint32_t>(object, ue::off::InternalIndex);
}

Rotator RotationOf(void* component)
{
    Rotator rotation;
    static ue::FunctionRef ref { "SceneComponent", "K2_GetComponentRotation" };
    ue::Call(component, ref, &rotation);
    return rotation;
}

// The stage's sun and sky dome are placed as things that never move, and the engine refuses to turn those.
void MakeMovable(void* component)
{
    static void* scene = ue::FindClass("SceneComponent");
    static const int at = scene ? ue::PropertyOffset(scene, "Mobility") : -1;
    if (at >= 0 && ue::Field<uint8_t>(component, at) == 2)
        return;
    uint8_t movable = 2;
    static ue::FunctionRef ref { "SceneComponent", "SetMobility" };
    ue::Call(component, ref, &movable);
}

void SetRotation(void* component, const Rotator& rotation)
{
    static void* scene = ue::FindClass("SceneComponent");
    static void* function = scene ? ue::FindFunction(scene, "K2_SetWorldRotation") : nullptr;
    static const int rotationAt = function ? ue::PropertyOffset(function, "NewRotation") : -1;
    static const int teleportAt = function ? ue::PropertyOffset(function, "bTeleport") : -1;
    if (rotationAt < 0 || teleportAt < 0 || teleportAt > 0x3F0)
        return;
    alignas(16) uint8_t params[0x400] = {}; // the function also takes a hit result to fill in
    *reinterpret_cast<Rotator*>(params + rotationAt) = rotation;
    params[teleportAt] = 1;
    static ue::FunctionRef ref { "SceneComponent", "K2_SetWorldRotation" };
    MakeMovable(component);
    ue::Call(component, ref, params);
}

// ----- The sun: every directional light of the stage, turned by the same amount from where the stage put it.
struct Sun {
    uint32_t Index = 0;
    Rotator Original;
    float Yaw = 0, Height = 0;
};
std::unordered_map<void*, Sun> g_suns;

void ScanSuns()
{
    void* cls = ue::FindClass("DirectionalLightComponent");
    if (!cls)
        return;
    for (void* light : ue::AllObjectsOf(cls)) {
        if (!Live(light) || lights::IsRigLight(light))
            continue;
        const uint32_t index = IndexOf(light);
        auto it = g_suns.find(light);
        if (it != g_suns.end() && it->second.Index != index)
            g_suns.erase(it), it = g_suns.end();
        if (it == g_suns.end())
            g_suns[light] = { index, RotationOf(light) };
    }
    std::erase_if(g_suns, [](const auto& entry) { return !ue::StillAlive(entry.first, entry.second.Index); });
}

void UpdateSuns()
{
    for (auto& [light, sun] : g_suns) {
        if (sun.Yaw == g_settings.SunYaw && sun.Height == g_settings.SunHeight)
            continue;
        if (!ue::StillAlive(light, sun.Index))
            continue;
        sun.Yaw = g_settings.SunYaw;
        sun.Height = g_settings.SunHeight;
        // A light's pitch is how far it points down, so a higher sun is a lower number. Past straight down it would flip over.
        const double pitch = sun.Height == 0 ? sun.Original.Pitch : std::clamp(sun.Original.Pitch - sun.Height, -89.9, 20.0);
        SetRotation(light, { pitch, sun.Original.Yaw + sun.Yaw, sun.Original.Roll });
    }
}

// ----- The sky dome. The stages draw their sky with one material, M_ENV_HDRI_Sky, which reads a panorama from its HDR_Map texture. A
// dynamic instance of the dome's own material with our texture in it shows the picture, with the stage's own tuning of it taken out.
constexpr const char* SkyMaterial = "M_ENV_HDRI_Sky";

struct Dome {
    uint32_t Index = 0;
    Rotator Original;
    float Turn = 0;
    bool TakesPicture = false;
    void* OriginalMaterial = nullptr;
    void* Instance = nullptr;
    uint32_t InstanceIndex = 0;
    void* Texture = nullptr;
    float Brightness = -1;
    float OriginalIntensity = 16000;
};
std::unordered_map<void*, Dome> g_domes;
std::unordered_map<void*, uint32_t> g_examined;
int g_pictureDomes = 0;

void* MaterialOf(void* mesh)
{
    struct {
        int32_t Slot;
        void* ReturnValue;
    } params { 0, nullptr };
    static ue::FunctionRef ref { "PrimitiveComponent", "GetMaterial" };
    ue::Call(mesh, ref, &params);
    return params.ReturnValue;
}

void SetMaterial(void* mesh, void* material)
{
    struct {
        int32_t Slot;
        void* Material;
    } params { 0, material };
    static ue::FunctionRef ref { "PrimitiveComponent", "SetMaterial" };
    ue::Call(mesh, ref, &params);
}

// Whether a material comes down from the sky material, through however many instances.
bool IsSkyMaterial(void* material)
{
    static std::unordered_map<void*, bool> known;
    const auto it = known.find(material);
    if (it != known.end())
        return it->second;
    static void* instanceClass = ue::FindClass("MaterialInstance");
    static const int parentAt = instanceClass ? ue::PropertyOffset(instanceClass, "Parent") : -1;
    bool sky = false;
    int depth = 0;
    for (void* m = material; m && depth < 8; ++depth) {
        if (!instanceClass || !ue::IsA(m, instanceClass)) {
            sky = ue::NameOf(m) == SkyMaterial;
            break;
        }
        m = parentAt >= 0 ? ue::Field<void*>(m, parentAt) : nullptr;
    }
    if (known.size() > 4096)
        known.clear();
    known[material] = sky;
    return sky;
}

void ScanDomes()
{
    void* cls = ue::FindClass("StaticMeshComponent");
    if (!cls)
        return;
    static const int assetAt = ue::PropertyOffset(cls, "StaticMesh");
    for (void* mesh : ue::AllObjectsOf(cls)) {
        if (!Live(mesh))
            continue;
        const uint32_t index = IndexOf(mesh);
        auto it = g_domes.find(mesh);
        if (it != g_domes.end() && it->second.Index == index)
            continue;
        if (it != g_domes.end())
            g_domes.erase(it);
        // A stage has thousands of meshes, and asking each for its material isn't free, so each is only looked at once.
        const auto seen = g_examined.find(mesh);
        if (seen != g_examined.end() && seen->second == index)
            continue;
        g_examined[mesh] = index;
        void* material = MaterialOf(mesh);
        const bool takesPicture = material && IsSkyMaterial(material);
        void* asset = assetAt >= 0 ? ue::Field<void*>(mesh, assetAt) : nullptr;
        if (!takesPicture && !(asset && ue::NameOf(asset).rfind("SM_Sky", 0) == 0))
            continue;
        Dome dome;
        dome.Index = index;
        dome.Original = RotationOf(mesh);
        dome.TakesPicture = takesPicture;
        g_domes[mesh] = dome;
        mod::Log("Sky: dome %s%s", ue::NameOf(effects::OwnerOf(mesh)).c_str(), takesPicture ? ", takes a picture" : "");
    }
    std::erase_if(g_domes, [](const auto& entry) { return !ue::StillAlive(entry.first, entry.second.Index); });
    g_pictureDomes = static_cast<int>(std::count_if(g_domes.begin(), g_domes.end(), [](const auto& entry) { return entry.second.TakesPicture; }));
}

void SetScalar(void* instance, const wchar_t* name, float value)
{
    struct {
        ue::FName Name;
        float Value;
    } params { ue::MakeName(name), value };
    static ue::FunctionRef ref { "MaterialInstanceDynamic", "SetScalarParameterValue" };
    ue::Call(instance, ref, &params);
}

float GetScalar(void* instance, const wchar_t* name, float fallback)
{
    struct {
        ue::FName Name;
        float ReturnValue;
    } params { ue::MakeName(name), fallback };
    static ue::FunctionRef ref { "MaterialInstanceDynamic", "K2_GetScalarParameterValue" };
    return ue::Call(instance, ref, &params) ? params.ReturnValue : fallback;
}

void* MakeInstance(void* mesh, void* material)
{
    static void* primitive = ue::FindClass("PrimitiveComponent");
    static void* function = primitive ? ue::FindFunction(primitive, "CreateDynamicMaterialInstance") : nullptr;
    static const int indexAt = function ? ue::PropertyOffset(function, "ElementIndex") : -1;
    static const int sourceAt = function ? ue::PropertyOffset(function, "SourceMaterial") : -1;
    static const int returnAt = function ? ue::PropertyOffset(function, "ReturnValue") : -1;
    if (indexAt < 0 || sourceAt < 0 || returnAt < 0 || returnAt > 56)
        return nullptr;
    alignas(8) uint8_t params[64] = {};
    *reinterpret_cast<int32_t*>(params + indexAt) = 0;
    *reinterpret_cast<void**>(params + sourceAt) = material;
    static ue::FunctionRef ref { "PrimitiveComponent", "CreateDynamicMaterialInstance" };
    ue::Call(mesh, ref, params);
    return *reinterpret_cast<void**>(params + returnAt);
}

// ----- The picture itself. The engine reads .hdr, .exr, .png, .jpg and the rest of what its image readers know.
void* g_texture = nullptr;
uint32_t g_textureIndex = 0;
std::string g_loadedFile;
bool g_fileTried = false;

void UpdateTexture()
{
    const bool wanted = g_settings.HdriOn && g_settings.HdriFile[0];
    const std::string file = wanted ? g_settings.HdriFile : "";
    if (g_fileTried && file == g_loadedFile)
        return;
    void* world = ue::World();
    if (wanted && !world)
        return; // not in a stage yet
    g_fileTried = true;
    g_loadedFile = file;
    void* old = g_texture;
    const uint32_t oldIndex = g_textureIndex;
    g_texture = nullptr;
    SetStatus("");
    if (wanted) {
        fs::path path(std::u8string(file.begin(), file.end()));
        if (!path.is_absolute())
            path = Folder() / path;
        std::error_code ignored;
        if (!fs::exists(path, ignored)) {
            SetStatus("Can't find " + file);
        } else {
            const std::wstring wide = path.wstring();
            struct {
                void* World;
                ue::FString Filename;
                void* ReturnValue;
            } params { world, ue::FString(wide), nullptr };
            static ue::FunctionRef import { "KismetRenderingLibrary", "ImportFileAsTexture2D" };
            ue::Call(nullptr, import, &params);
            if (!params.ReturnValue) {
                SetStatus("The game couldn't read " + file);
                mod::Log("Sky: couldn't read %s", file.c_str());
            } else {
                g_texture = params.ReturnValue;
                g_textureIndex = IndexOf(g_texture);
                ue::KeepAlive(g_texture, true);
                struct {
                    int32_t ReturnValue;
                } x { 0 }, y { 0 };
                static ue::FunctionRef sizeX { "Texture2D", "Blueprint_GetSizeX" };
                static ue::FunctionRef sizeY { "Texture2D", "Blueprint_GetSizeY" };
                ue::Call(g_texture, sizeX, &x);
                ue::Call(g_texture, sizeY, &y);
                mod::Log("Sky: picture %s, %d x %d", file.c_str(), x.ReturnValue, y.ReturnValue);
                SetStatus(std::to_string(x.ReturnValue) + " x " + std::to_string(y.ReturnValue));
            }
        }
    }
    if (old && ue::StillAlive(old, oldIndex))
        ue::KeepAlive(old, false);
}

bool g_lightDirty = false;

void UpdateDomes()
{
    void* texture = g_texture && ue::StillAlive(g_texture, g_textureIndex) ? g_texture : nullptr;
    for (auto& [mesh, dome] : g_domes) {
        if (!ue::StillAlive(mesh, dome.Index))
            continue;
        if (dome.Turn != g_settings.SkyRotation) {
            dome.Turn = g_settings.SkyRotation;
            SetRotation(mesh, { dome.Original.Pitch, dome.Original.Yaw + dome.Turn, dome.Original.Roll });
            g_lightDirty = true;
        }
        if (!dome.TakesPicture)
            continue;
        if (!texture) {
            if (dome.Instance) {
                if (dome.OriginalMaterial)
                    SetMaterial(mesh, dome.OriginalMaterial);
                dome.Instance = nullptr;
                dome.Texture = nullptr;
                g_lightDirty = true;
            }
            continue;
        }
        if (!dome.Instance || !ue::StillAlive(dome.Instance, dome.InstanceIndex)) {
            if (!dome.OriginalMaterial)
                dome.OriginalMaterial = MaterialOf(mesh);
            dome.Instance = dome.OriginalMaterial ? MakeInstance(mesh, dome.OriginalMaterial) : nullptr;
            if (!dome.Instance)
                continue;
            dome.InstanceIndex = IndexOf(dome.Instance);
            dome.OriginalIntensity = GetScalar(dome.Instance, L"Intensity", 16000.0f);
            // One whole panorama over the dome, as it is: none of the stage's own grading, haze, drifting or clouds on top.
            SetScalar(dome.Instance, L"SkyTile_U", 1.0f);
            SetScalar(dome.Instance, L"SkyTile_V", 1.0f);
            SetScalar(dome.Instance, L"Contrast", 1.0f);
            SetScalar(dome.Instance, L"Saturation", 0.0f);
            SetScalar(dome.Instance, L"Atomosphere", 0.0f);
            SetScalar(dome.Instance, L"UVnoiseMult", 0.0f);
            SetScalar(dome.Instance, L"CloudIntensity", 0.0f);
            struct {
                ue::FName Name;
                LinearColor Value;
            } tint { ue::MakeName(L"SkyTint"), { 1, 1, 1, 1 } };
            static ue::FunctionRef setVector { "MaterialInstanceDynamic", "SetVectorParameterValue" };
            ue::Call(dome.Instance, setVector, &tint);
            dome.Texture = nullptr;
            dome.Brightness = -1;
        }
        if (dome.Texture != texture) {
            struct {
                ue::FName Name;
                void* Value;
            } params { ue::MakeName(L"HDR_Map"), texture };
            static ue::FunctionRef setTexture { "MaterialInstanceDynamic", "SetTextureParameterValue" };
            ue::Call(dome.Instance, setTexture, &params);
            dome.Texture = texture;
            g_lightDirty = true;
        }
        if (dome.Brightness != g_settings.HdriBrightness) {
            dome.Brightness = g_settings.HdriBrightness;
            SetScalar(dome.Instance, L"Intensity", dome.OriginalIntensity * dome.Brightness);
            g_lightDirty = true;
        }
    }
}

// ----- The stage's sky light. It's lit from a cubemap made for the stage's own sky. Told to take its light from the scene instead, it
// captures the dome with the new picture on it.
struct SkyLight {
    uint32_t Index = 0;
    uint8_t OriginalSource = 1;
    float OriginalAngle = 0;
    float Turn = 0;
    bool Captured = false;
};
std::unordered_map<void*, SkyLight> g_skyLights;
ULONGLONG g_nextCapture = 0;

void ScanSkyLights()
{
    void* cls = ue::FindClass("SkyLightComponent");
    if (!cls)
        return;
    static const int sourceAt = ue::PropertyOffset(cls, "SourceType"), angleAt = ue::PropertyOffset(cls, "SourceCubemapAngle");
    if (sourceAt < 0 || angleAt < 0)
        return;
    for (void* light : ue::AllObjectsOf(cls)) {
        if (!Live(light))
            continue;
        const uint32_t index = IndexOf(light);
        auto it = g_skyLights.find(light);
        if (it != g_skyLights.end() && it->second.Index == index)
            continue;
        g_skyLights[light] = { index, ue::Field<uint8_t>(light, sourceAt), ue::Field<float>(light, angleAt) };
    }
    std::erase_if(g_skyLights, [](const auto& entry) { return !ue::StillAlive(entry.first, entry.second.Index); });
}

void UpdateSkyLights()
{
    void* cls = ue::FindClass("SkyLightComponent");
    if (!cls)
        return;
    static const int sourceAt = ue::PropertyOffset(cls, "SourceType");
    static ue::FunctionRef setAngle { "SkyLightComponent", "SetSourceCubemapAngle" };
    static ue::FunctionRef recapture { "SkyLightComponent", "RecaptureSky" };
    const bool capture = g_settings.HdriOn && g_settings.HdriLightsStage && g_texture && g_pictureDomes > 0;
    for (auto& [light, state] : g_skyLights) {
        if (!ue::StillAlive(light, state.Index))
            continue;
        if (state.Turn != g_settings.SkyRotation) {
            state.Turn = g_settings.SkyRotation;
            float angle = std::fmod(state.OriginalAngle + state.Turn, 360.0f);
            if (angle < 0)
                angle += 360.0f;
            ue::Call(light, setAngle, &angle);
        }
        if (capture != state.Captured && sourceAt >= 0) {
            state.Captured = capture;
            ue::Field<uint8_t>(light, sourceAt) = capture ? 0 : state.OriginalSource; // 0 = captured scene
            g_lightDirty = true;
        }
    }
    // Capturing isn't free, so a slider being dragged or a keyframe turning the sky gets a few captures a second, not one a frame.
    const ULONGLONG now = GetTickCount64();
    if (!g_lightDirty || now < g_nextCapture)
        return;
    g_lightDirty = false;
    g_nextCapture = now + 200;
    for (auto& [light, state] : g_skyLights)
        if (ue::StillAlive(light, state.Index))
            ue::Call(light, recapture, nullptr);
}

// ----- Hiding a fighter: the whole actor, with what it carries and its shadow.
void UpdateFighters()
{
    static void* seen[2] = {};
    static bool hidden[2] = {};
    static int tick = 0;
    ++tick;
    for (int side = 0; side < 2; ++side) {
        void* fighter = game::Fighter(side);
        if (fighter != seen[side]) {
            seen[side] = fighter;
            hidden[side] = false;
        }
        if (!fighter)
            continue;
        const bool wanted = g_settings.HideFighter[side];
        // Now and then again while hidden, in case the game shows the fighter for a new round or a cinematic.
        if (wanted == hidden[side] && !(wanted && tick % 20 == 0))
            continue;
        struct {
            bool Hidden;
        } params { wanted };
        static ue::FunctionRef ref { "Actor", "SetActorHiddenInGame" };
        ue::Call(fighter, ref, &params);
        hidden[side] = wanted;
    }
}

bool g_used = false;
ULONGLONG g_nextScan = 0;

} // namespace

Settings& Config()
{
    return g_settings;
}

fs::path Folder()
{
    fs::path dir = mod::DataDir() / "hdri";
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    return dir;
}

std::vector<std::string> Files()
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(Folder(), error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* known : { ".hdr", ".exr", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff" })
            if (extension == known) {
                const std::u8string name = entry.path().filename().u8string();
                names.emplace_back(name.begin(), name.end());
            }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string Status()
{
    std::lock_guard lock(g_statusMutex);
    if (g_settings.HdriOn && g_texture && g_pictureDomes == 0 && g_used)
        return "This stage's sky can't take a picture";
    return g_status;
}

void OnFightFound()
{
    g_nextScan = 0;
    g_examined.clear(); // game thread, as the scans are
}

void OnGameTick()
{
    UpdateFighters();
    const bool wanted = g_settings.SunYaw != 0 || g_settings.SunHeight != 0 || g_settings.SkyRotation != 0 || g_settings.HdriOn;
    if (!wanted && !g_used)
        return;
    if (!game::Fighter(0))
        return;
    g_used = true;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextScan) {
        g_nextScan = now + 4000;
        ScanSuns();
        ScanDomes();
        ScanSkyLights();
    }
    UpdateTexture();
    UpdateSuns();
    UpdateDomes();
    UpdateSkyLights();
}

} // namespace sky
