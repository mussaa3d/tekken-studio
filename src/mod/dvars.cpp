#include "mod/dvars.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "mod/log.h"
#include "mod/ue.h"

namespace dvars {

namespace {

struct LinearColor {
    float R, G, B, A;
};

Colors g_colors;
std::mutex g_mutex;
std::vector<int> g_pending;
bool g_tintPending = false;
bool g_refreshAll = false;

std::vector<Slider> MakeSliders()
{
    const auto cvar = [](const char* group, const char* label, const wchar_t* name, float min, float max, float value, bool whole = false) {
        Slider s;
        s.Group = group;
        s.Label = label;
        s.Cvar = name;
        s.Min = min;
        s.Max = max;
        s.Default = s.Value = value;
        s.Whole = whole;
        return s;
    };
    const auto stage = [](const char* group, const char* label, Kind kind, float min, float max, float value, bool logarithmic = false) {
        Slider s;
        s.Group = group;
        s.Label = label;
        s.Kind = kind;
        s.Min = min;
        s.Max = max;
        s.Default = s.Value = value;
        s.Logarithmic = logarithmic;
        return s;
    };
    return {
        stage("Fog", "Density", Kind::FogDensity, 0.0f, 0.5f, 0.02f, true),
        stage("Fog", "Height falloff", Kind::FogFalloff, 0.001f, 2.0f, 0.2f, true),
        stage("Fog", "Start distance", Kind::FogStartDistance, 0.0f, 10000.0f, 0.0f),
        stage("Fog", "Max opacity", Kind::FogMaxOpacity, 0.0f, 1.0f, 1.0f),
        stage("Light", "Sun brightness", Kind::SunIntensity, 0.0f, 50.0f, 5.0f),
        stage("Light", "Sky brightness", Kind::SkyIntensity, 0.0f, 10.0f, 1.0f),
        cvar("Engine", "Bloom quality", L"r.BloomQuality", 0, 5, 5, true),
        cvar("Engine", "Motion blur quality", L"r.MotionBlurQuality", 0, 4, 4, true),
        cvar("Engine", "Depth of field quality", L"r.DepthOfFieldQuality", 0, 4, 2, true),
        cvar("Engine", "Lens fringe", L"r.SceneColorFringeQuality", 0, 1, 1, true),
        cvar("Engine", "Sharpen", L"r.Tonemapper.Sharpen", 0, 2, 0),
        cvar("Engine", "Resolution scale", L"r.ScreenPercentage", 50, 200, 100, true),
    };
}

template <typename Apply>
int ForEachOf(const char* className, Apply apply)
{
    void* cls = ue::FindClass(className);
    if (!cls)
        return 0;
    int count = 0;
    for (void* object : ue::AllObjectsOf(cls)) {
        apply(object);
        ++count;
    }
    return count;
}

void CallFloat(void* object, ue::FunctionRef& ref, float value)
{
    struct {
        float Value;
    } params { value };
    ue::Call(object, ref, &params);
}

// The value of each stage object before we first changed it, to put back on Reset.
std::map<std::pair<void*, std::string>, float> g_originals;

void Remember(void* object, void* cls, const char* property)
{
    const auto key = std::make_pair(object, std::string(property));
    if (g_originals.count(key))
        return;
    const int offset = ue::PropertyOffset(cls, property);
    if (offset >= 0)
        g_originals[key] = ue::Field<float>(object, offset);
}

// The game ships with volumetric fog turned off and leaves its plain fog barely visible, so nothing a fog object is told shows until
// volumetric fog is on. It's lit by the stage, which is where its colour comes from; with no albedo only its own glow is left, which
// makes it a colour we can choose.
constexpr float FogGlowScale = 200000.0f; // a glow this many times the density looks like its full colour

bool g_fogOn = false;
bool g_fogTinted = false;
int g_gameVolumetricFog = 0;

void TurnFogOn()
{
    static ue::FunctionRef enable { "ExponentialHeightFogComponent", "SetVolumetricFog" };
    if (!g_fogOn) {
        g_fogOn = true;
        g_gameVolumetricFog = ue::CvarInt(L"r.VolumetricFog");
        ue::ConsoleCommand(L"r.VolumetricFog 1");
    }
    struct {
        bool Enable;
    } params { true };
    ForEachOf("ExponentialHeightFogComponent", [&](void* fog) { ue::Call(fog, enable, &params); });
}

void TurnFogOff()
{
    if (!g_fogOn)
        return;
    g_fogOn = false;
    ue::ConsoleCommand(L"r.VolumetricFog " + std::to_wstring(g_gameVolumetricFog));
}

void SetFogGlow(void* fog, LinearColor glow)
{
    static ue::FunctionRef albedo { "ExponentialHeightFogComponent", "SetVolumetricFogAlbedo" };
    static ue::FunctionRef emissive { "ExponentialHeightFogComponent", "SetVolumetricFogEmissive" };
    const uint32_t black = 0xFF000000u;
    ue::Call(fog, albedo, const_cast<uint32_t*>(&black));
    struct {
        LinearColor Value;
    } params { glow };
    ue::Call(fog, emissive, &params);
}

void ApplyFogFloat(ue::FunctionRef& setter, const char* property, float value)
{
    TurnFogOn();
    void* cls = ue::FindClass("ExponentialHeightFogComponent");
    const int count = ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
        Remember(fog, cls, property);
        CallFloat(fog, setter, value);
    });
    if (count == 0)
        mod::Log("Dvars: this stage has no fog object");
}

void ApplyStage(const Slider& slider, float value)
{
    static ue::FunctionRef density { "ExponentialHeightFogComponent", "SetFogDensity" };
    static ue::FunctionRef falloff { "ExponentialHeightFogComponent", "SetFogHeightFalloff" };
    static ue::FunctionRef start { "ExponentialHeightFogComponent", "SetStartDistance" };
    static ue::FunctionRef opacity { "ExponentialHeightFogComponent", "SetFogMaxOpacity" };
    static ue::FunctionRef lightIntensity { "LightComponent", "SetIntensity" };
    static ue::FunctionRef skyIntensity { "SkyLightComponent", "SetIntensity" };

    switch (slider.Kind) {
    case Kind::FogDensity:
        ApplyFogFloat(density, "FogDensity", value);
        break;
    case Kind::FogFalloff:
        ApplyFogFloat(falloff, "FogHeightFalloff", value);
        break;
    case Kind::FogStartDistance:
        ApplyFogFloat(start, "StartDistance", value);
        break;
    case Kind::FogMaxOpacity:
        ApplyFogFloat(opacity, "FogMaxOpacity", value);
        break;
    case Kind::SunIntensity: {
        void* cls = ue::FindClass("DirectionalLightComponent");
        ForEachOf("DirectionalLightComponent", [&](void* light) {
            Remember(light, cls, "Intensity");
            CallFloat(light, lightIntensity, value);
        });
        break;
    }
    case Kind::SkyIntensity: {
        void* cls = ue::FindClass("SkyLightComponent");
        ForEachOf("SkyLightComponent", [&](void* light) {
            Remember(light, cls, "Intensity");
            CallFloat(light, skyIntensity, value);
        });
        break;
    }
    case Kind::Cvar:
        break;
    }
}

void Apply(const Slider& slider)
{
    if (slider.Kind == Kind::Cvar) {
        wchar_t command[160];
        swprintf_s(command, L"%ls %g", slider.Cvar.c_str(), slider.Value);
        ue::ConsoleCommand(command);
    } else {
        ApplyStage(slider, slider.Value);
    }
}

// Puts the stage's own values back. Stage objects are found by property name, so this covers whichever were changed.
void RestoreStage(const Slider& slider)
{
    const char* property = nullptr;
    const char* className = nullptr;
    static ue::FunctionRef density { "ExponentialHeightFogComponent", "SetFogDensity" };
    static ue::FunctionRef falloff { "ExponentialHeightFogComponent", "SetFogHeightFalloff" };
    static ue::FunctionRef start { "ExponentialHeightFogComponent", "SetStartDistance" };
    static ue::FunctionRef opacity { "ExponentialHeightFogComponent", "SetFogMaxOpacity" };
    static ue::FunctionRef lightIntensity { "LightComponent", "SetIntensity" };
    static ue::FunctionRef skyIntensity { "SkyLightComponent", "SetIntensity" };
    ue::FunctionRef* setter = nullptr;
    switch (slider.Kind) {
    case Kind::FogDensity:
        property = "FogDensity", className = "ExponentialHeightFogComponent", setter = &density;
        break;
    case Kind::FogFalloff:
        property = "FogHeightFalloff", className = "ExponentialHeightFogComponent", setter = &falloff;
        break;
    case Kind::FogStartDistance:
        property = "StartDistance", className = "ExponentialHeightFogComponent", setter = &start;
        break;
    case Kind::FogMaxOpacity:
        property = "FogMaxOpacity", className = "ExponentialHeightFogComponent", setter = &opacity;
        break;
    case Kind::SunIntensity:
        property = "Intensity", className = "DirectionalLightComponent", setter = &lightIntensity;
        break;
    case Kind::SkyIntensity:
        property = "Intensity", className = "SkyLightComponent", setter = &skyIntensity;
        break;
    case Kind::Cvar:
        return;
    }
    ForEachOf(className, [&](void* object) {
        const auto it = g_originals.find(std::make_pair(object, std::string(property)));
        if (it != g_originals.end())
            CallFloat(object, *setter, it->second);
    });
}

// The plain fog's own colours, and how much of the sky it takes in, which is where the map's blue haze comes from. Volumetric fog stops
// at 60 m, so past that these are the only fog there is. No setters exist for all of them, so they're written and then a setter
// makes the engine read them.
struct PlainFogColours {
    LinearColor Colour, Luminance, Directional, DirectionalLuminance, SkyScale;
    void* Cubemap;
};
std::map<void*, PlainFogColours> g_plainFog;

struct PlainFogFields {
    int Colour, Luminance, Directional, DirectionalLuminance, SkyScale, Cubemap, Density;
    bool Valid() const { return Colour >= 0 && Luminance >= 0 && Directional >= 0 && DirectionalLuminance >= 0 && SkyScale >= 0 && Cubemap >= 0 && Density >= 0; }
};

PlainFogFields FindPlainFogFields()
{
    void* cls = ue::FindClass("ExponentialHeightFogComponent");
    if (!cls)
        return { -1, -1, -1, -1, -1, -1, -1 };
    return { ue::PropertyOffset(cls, "FogInscatteringColor"), ue::PropertyOffset(cls, "FogInscatteringLuminance"), ue::PropertyOffset(cls, "DirectionalInscatteringColor"),
             ue::PropertyOffset(cls, "DirectionalInscatteringLuminance"), ue::PropertyOffset(cls, "SkyAtmosphereAmbientContributionColorScale"), ue::PropertyOffset(cls, "InscatteringColorCubemap"),
             ue::PropertyOffset(cls, "FogDensity") };
}

void RefreshFog(void* fog, const PlainFogFields& fields)
{
    static ue::FunctionRef density { "ExponentialHeightFogComponent", "SetFogDensity" };
    // The engine ignores a setter that changes nothing, so this goes by way of another value.
    const float value = ue::Field<float>(fog, fields.Density);
    CallFloat(fog, density, value + 0.0001f);
    CallFloat(fog, density, value);
}

void SetPlainFogColour(const float* colour)
{
    const PlainFogFields fields = FindPlainFogFields();
    if (!fields.Valid())
        return;
    const LinearColor chosen { colour[0], colour[1], colour[2], 1 };
    const LinearColor none { 0, 0, 0, 1 };
    ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
        if (!g_plainFog.count(fog))
            g_plainFog[fog] = { ue::Field<LinearColor>(fog, fields.Colour), ue::Field<LinearColor>(fog, fields.Luminance), ue::Field<LinearColor>(fog, fields.Directional),
                                ue::Field<LinearColor>(fog, fields.DirectionalLuminance), ue::Field<LinearColor>(fog, fields.SkyScale), ue::Field<void*>(fog, fields.Cubemap) };
        ue::Field<LinearColor>(fog, fields.Colour) = chosen;
        ue::Field<LinearColor>(fog, fields.Luminance) = chosen;
        ue::Field<LinearColor>(fog, fields.Directional) = none;
        ue::Field<LinearColor>(fog, fields.DirectionalLuminance) = none;
        ue::Field<LinearColor>(fog, fields.SkyScale) = none;
        ue::Field<void*>(fog, fields.Cubemap) = nullptr;
        RefreshFog(fog, fields);
    });
}

void RestorePlainFogColour()
{
    const PlainFogFields fields = FindPlainFogFields();
    if (fields.Valid())
        ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
            const auto it = g_plainFog.find(fog);
            if (it == g_plainFog.end())
                return;
            ue::Field<LinearColor>(fog, fields.Colour) = it->second.Colour;
            ue::Field<LinearColor>(fog, fields.Luminance) = it->second.Luminance;
            ue::Field<LinearColor>(fog, fields.Directional) = it->second.Directional;
            ue::Field<LinearColor>(fog, fields.DirectionalLuminance) = it->second.DirectionalLuminance;
            ue::Field<LinearColor>(fog, fields.SkyScale) = it->second.SkyScale;
            ue::Field<void*>(fog, fields.Cubemap) = it->second.Cubemap;
            RefreshFog(fog, fields);
        });
    g_plainFog.clear();
}

void ApplyTint()
{
    static ue::FunctionRef sunColor { "LightComponent", "SetLightColor" };
    if (g_colors.FogOverridden) {
        SetPlainFogColour(g_colors.Fog);
        TurnFogOn();
        float density = 0.02f;
        for (const Slider& slider : All())
            if (slider.Kind == Kind::FogDensity)
                density = slider.Value;
        const float glow = density * FogGlowScale;
        ForEachOf("ExponentialHeightFogComponent", [&](void* fog) { SetFogGlow(fog, { g_colors.Fog[0] * glow, g_colors.Fog[1] * glow, g_colors.Fog[2] * glow, 1 }); });
        g_fogTinted = true;
    } else if (g_fogTinted) {
        // Back to fog lit by the stage.
        g_fogTinted = false;
        RestorePlainFogColour();
        static ue::FunctionRef albedo { "ExponentialHeightFogComponent", "SetVolumetricFogAlbedo" };
        static ue::FunctionRef emissive { "ExponentialHeightFogComponent", "SetVolumetricFogEmissive" };
        uint32_t white = 0xFFFFFFFFu;
        struct {
            LinearColor Value;
        } none { { 0, 0, 0, 1 } };
        ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
            ue::Call(fog, albedo, &white);
            ue::Call(fog, emissive, &none);
        });
    }
    if (g_colors.SunOverridden) {
        struct {
            LinearColor Value;
            bool SRGB;
        } params { { g_colors.Sun[0], g_colors.Sun[1], g_colors.Sun[2], 1 }, true };
        ForEachOf("DirectionalLightComponent", [&](void* light) { ue::Call(light, sunColor, &params); });
    }
}

// A fog object from before a depth pass, read from memory.
struct GreenBackup {
    float Density, Falloff, Start, Opacity;
    uint32_t Albedo;
    LinearColor Glow;
    bool Valid;
};
std::map<void*, GreenBackup> g_greenBackup;

} // namespace

std::vector<Slider>& All()
{
    static std::vector<Slider> sliders = MakeSliders();
    return sliders;
}

void LoadExtra()
{
    std::ifstream file(mod::DataDir() / "dvars.ini");
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        std::istringstream row(line);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(row, part, '|')) {
            const size_t begin = part.find_first_not_of(" \t\r");
            const size_t end = part.find_last_not_of(" \t\r");
            parts.push_back(begin == std::string::npos ? std::string() : part.substr(begin, end - begin + 1));
        }
        if (parts.size() != 5 || parts[0].empty() || parts[1].empty())
            continue;
        Slider slider;
        slider.Group = "Your own";
        slider.Label = parts[0];
        slider.Cvar = std::wstring(parts[1].begin(), parts[1].end());
        slider.Min = static_cast<float>(std::atof(parts[2].c_str()));
        slider.Max = static_cast<float>(std::atof(parts[3].c_str()));
        slider.Default = slider.Value = static_cast<float>(std::atof(parts[4].c_str()));
        All().push_back(std::move(slider));
    }
}

void Changed(Slider& slider)
{
    slider.Overridden = true;
    std::lock_guard lock(g_mutex);
    g_pending.push_back(static_cast<int>(&slider - All().data()));
}

void Reset(Slider& slider)
{
    slider.Overridden = false;
    slider.Value = slider.Default;
    std::lock_guard lock(g_mutex);
    g_pending.push_back(static_cast<int>(&slider - All().data()));
}

Colors& Tint()
{
    return g_colors;
}

void TintChanged()
{
    std::lock_guard lock(g_mutex);
    g_tintPending = true;
}

void OnFightFound()
{
    g_originals.clear();
    g_greenBackup.clear();
    std::lock_guard lock(g_mutex);
    g_refreshAll = true;
}

void OnGameTick()
{
    std::vector<int> pending;
    bool tint = false, refresh = false;
    {
        std::lock_guard lock(g_mutex);
        pending.swap(g_pending);
        std::swap(tint, g_tintPending);
        std::swap(refresh, g_refreshAll);
    }
    for (const int index : pending) {
        Slider& slider = All()[index];
        if (slider.Overridden)
            Apply(slider);
        else if (slider.Kind == Kind::Cvar)
            Apply(slider); // the default is the value in the table
        else
            RestoreStage(slider);
    }
    if (refresh)
        for (const Slider& slider : All())
            if (slider.Overridden)
                Apply(slider);
    if (tint || refresh)
        ApplyTint();

    // Once nothing asks for fog any more, the game goes back to how it was.
    if (!pending.empty() && g_fogOn && !g_colors.FogOverridden && g_greenBackup.empty()) {
        bool wanted = false;
        for (const Slider& slider : All())
            wanted = wanted || (slider.Overridden && (slider.Kind == Kind::FogDensity || slider.Kind == Kind::FogFalloff || slider.Kind == Kind::FogStartDistance || slider.Kind == Kind::FogMaxOpacity));
        if (!wanted)
            TurnFogOff();
    }
}

namespace {
bool FogOverride(bool on, float r, float g, float b, float densityValue, float startValue);
}

bool SetFogOverride(float r, float g, float b, float density, float startDistance)
{
    return FogOverride(true, r, g, b, density, startDistance);
}

void ClearFogOverride()
{
    FogOverride(false, 0, 0, 0, 0, 0);
}

namespace {

bool FogOverride(bool on, float r, float g, float b, float densityValue, float startValue)
{
    void* cls = ue::FindClass("ExponentialHeightFogComponent");
    if (!cls)
        return false;
    static ue::FunctionRef density { "ExponentialHeightFogComponent", "SetFogDensity" };
    static ue::FunctionRef falloff { "ExponentialHeightFogComponent", "SetFogHeightFalloff" };
    static ue::FunctionRef start { "ExponentialHeightFogComponent", "SetStartDistance" };
    static ue::FunctionRef opacity { "ExponentialHeightFogComponent", "SetFogMaxOpacity" };
    const int densityAt = ue::PropertyOffset(cls, "FogDensity");
    const int falloffAt = ue::PropertyOffset(cls, "FogHeightFalloff");
    const int startAt = ue::PropertyOffset(cls, "StartDistance");
    const int opacityAt = ue::PropertyOffset(cls, "FogMaxOpacity");
    const int albedoAt = ue::PropertyOffset(cls, "VolumetricFogAlbedo");
    const int glowAt = ue::PropertyOffset(cls, "VolumetricFogEmissive");

    if (on) {
        TurnFogOn();
        // The fog's history would smear each pass into the next.
        ue::ConsoleCommand(L"r.VolumetricFog.TemporalReprojection 0");
    } else if (!g_greenBackup.empty()) {
        ue::ConsoleCommand(L"r.VolumetricFog.TemporalReprojection 1");
    }
    const float glow = densityValue * FogGlowScale;
    const int count = ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
        if (on) {
            if (!g_greenBackup.count(fog)) {
                GreenBackup backup {};
                backup.Valid = densityAt >= 0 && falloffAt >= 0 && startAt >= 0 && opacityAt >= 0 && albedoAt >= 0 && glowAt >= 0;
                if (backup.Valid) {
                    backup.Density = ue::Field<float>(fog, densityAt);
                    backup.Falloff = ue::Field<float>(fog, falloffAt);
                    backup.Start = ue::Field<float>(fog, startAt);
                    backup.Opacity = ue::Field<float>(fog, opacityAt);
                    backup.Albedo = ue::Field<uint32_t>(fog, albedoAt);
                    backup.Glow = ue::Field<LinearColor>(fog, glowAt);
                }
                g_greenBackup[fog] = backup;
            }
            CallFloat(fog, density, densityValue);
            CallFloat(fog, falloff, 0.001f); // the same thickness at every height
            CallFloat(fog, start, startValue);
            CallFloat(fog, opacity, 1.0f);
            SetFogGlow(fog, { r * glow, g * glow, b * glow, 1 });
        } else if (const auto it = g_greenBackup.find(fog); it != g_greenBackup.end() && it->second.Valid) {
            CallFloat(fog, density, it->second.Density);
            CallFloat(fog, falloff, it->second.Falloff);
            CallFloat(fog, start, it->second.Start);
            CallFloat(fog, opacity, it->second.Opacity);
            static ue::FunctionRef albedo { "ExponentialHeightFogComponent", "SetVolumetricFogAlbedo" };
            static ue::FunctionRef emissive { "ExponentialHeightFogComponent", "SetVolumetricFogEmissive" };
            ue::Call(fog, albedo, &it->second.Albedo);
            struct {
                LinearColor Value;
            } params { it->second.Glow };
            ue::Call(fog, emissive, &params);
        }
    });
    if (!on && !g_greenBackup.empty()) {
        g_greenBackup.clear();
        bool wanted = g_colors.FogOverridden;
        for (const Slider& slider : All())
            wanted = wanted || (slider.Overridden && (slider.Kind == Kind::FogDensity || slider.Kind == Kind::FogFalloff || slider.Kind == Kind::FogStartDistance || slider.Kind == Kind::FogMaxOpacity));
        if (!wanted)
            TurnFogOff();
    }
    return count > 0;
}

} // namespace

} // namespace dvars
