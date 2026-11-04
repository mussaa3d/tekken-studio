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

void ApplyFogFloat(ue::FunctionRef& setter, const char* property, float value)
{
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

void ApplyTint()
{
    static ue::FunctionRef fogColor { "ExponentialHeightFogComponent", "SetFogInscatteringColor" };
    static ue::FunctionRef sunColor { "LightComponent", "SetLightColor" };
    if (g_colors.FogOverridden) {
        struct {
            LinearColor Value;
        } params { { g_colors.Fog[0], g_colors.Fog[1], g_colors.Fog[2], 1 } };
        ForEachOf("ExponentialHeightFogComponent", [&](void* fog) { ue::Call(fog, fogColor, &params); });
    }
    if (g_colors.SunOverridden) {
        struct {
            LinearColor Value;
            bool SRGB;
        } params { { g_colors.Sun[0], g_colors.Sun[1], g_colors.Sun[2], 1 }, true };
        ForEachOf("DirectionalLightComponent", [&](void* light) { ue::Call(light, sunColor, &params); });
    }
}

// Colour and floats of a fog object from before greenscreen, read from memory.
struct GreenBackup {
    float Density, Falloff, Start, Opacity;
    LinearColor Color;
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
    static ue::FunctionRef color { "ExponentialHeightFogComponent", "SetFogInscatteringColor" };
    const int densityAt = ue::PropertyOffset(cls, "FogDensity");
    const int falloffAt = ue::PropertyOffset(cls, "FogHeightFalloff");
    const int startAt = ue::PropertyOffset(cls, "StartDistance");
    const int opacityAt = ue::PropertyOffset(cls, "FogMaxOpacity");
    int colorAt = ue::PropertyOffset(cls, "FogInscatteringLuminance");
    if (colorAt < 0)
        colorAt = ue::PropertyOffset(cls, "FogInscatteringColor");

    const auto setColor = [&](void* fog, LinearColor value) {
        struct {
            LinearColor Value;
        } params { value };
        ue::Call(fog, color, &params);
    };

    const int count = ForEachOf("ExponentialHeightFogComponent", [&](void* fog) {
        if (on) {
            if (!g_greenBackup.count(fog)) {
                GreenBackup backup {};
                backup.Valid = densityAt >= 0 && falloffAt >= 0 && startAt >= 0 && opacityAt >= 0 && colorAt >= 0;
                if (backup.Valid) {
                    backup.Density = ue::Field<float>(fog, densityAt);
                    backup.Falloff = ue::Field<float>(fog, falloffAt);
                    backup.Start = ue::Field<float>(fog, startAt);
                    backup.Opacity = ue::Field<float>(fog, opacityAt);
                    backup.Color = ue::Field<LinearColor>(fog, colorAt);
                }
                g_greenBackup[fog] = backup;
            }
            CallFloat(fog, density, densityValue);
            CallFloat(fog, falloff, 0.001f); // the same thickness at every height
            CallFloat(fog, start, startValue);
            CallFloat(fog, opacity, 1.0f);
            setColor(fog, { r, g, b, 1 });
        } else if (const auto it = g_greenBackup.find(fog); it != g_greenBackup.end() && it->second.Valid) {
            CallFloat(fog, density, it->second.Density);
            CallFloat(fog, falloff, it->second.Falloff);
            CallFloat(fog, start, it->second.Start);
            CallFloat(fog, opacity, it->second.Opacity);
            setColor(fog, it->second.Color);
        }
    });
    if (!on)
        g_greenBackup.clear();
    return count > 0;
}

} // namespace

} // namespace dvars
