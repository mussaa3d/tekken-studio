#include "mod/postfx.h"

#include "mod/game.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace postfx {

namespace {

FocusSettings g_focus;
bool g_resolved = false;
bool g_available = false;
int g_blendWeightOffset = -1; // inside the camera's view info
int g_settingsOffset = -1;

std::vector<Control> MakeControls()
{
    const auto one = [](const char* group, const char* label, const char* property, float min, float max, float value) {
        Control c { group, label, property };
        c.Min = min;
        c.Max = max;
        c.Default = c.Value = value;
        return c;
    };
    const auto all = [](const char* group, const char* label, const char* property, float min, float max, float value) {
        Control c { group, label, property, Type::AllChannels };
        c.Min = min;
        c.Max = max;
        c.Default = c.Value = value;
        return c;
    };
    return {
        all("Filmtweaks", "Brightness", "ColorGain", 0.0f, 3.0f, 1.0f),
        all("Filmtweaks", "Contrast", "ColorContrast", 0.0f, 2.0f, 1.0f),
        all("Filmtweaks", "Saturation", "ColorSaturation", 0.0f, 2.0f, 1.0f),
        all("Filmtweaks", "Gamma", "ColorGamma", 0.2f, 2.5f, 1.0f),
        one("Filmtweaks", "Temperature", "WhiteTemp", 1500.0f, 15000.0f, 6500.0f),
        one("Filmtweaks", "Tint", "WhiteTint", -1.0f, 1.0f, 0.0f),
        one("Filmtweaks", "Exposure", "AutoExposureBias", -6.0f, 6.0f, 0.0f),
        one("Lens", "Bloom", "BloomIntensity", 0.0f, 8.0f, 0.675f),
        one("Lens", "Vignette", "VignetteIntensity", 0.0f, 1.0f, 0.4f),
        one("Lens", "Film grain", "FilmGrainIntensity", 0.0f, 1.0f, 0.0f),
        one("Lens", "Colour fringe", "SceneFringeIntensity", 0.0f, 5.0f, 0.0f),
        one("Lens", "Engine motion blur", "MotionBlurAmount", 0.0f, 1.0f, 0.5f),
        one("Depth of field", "Focus distance", "DepthOfFieldFocalDistance", 1.0f, 5000.0f, 400.0f),
        one("Depth of field", "Aperture (f-stop)", "DepthOfFieldFstop", 0.1f, 32.0f, 2.8f),
        one("Depth of field", "Sensor width", "DepthOfFieldSensorWidth", 1.0f, 100.0f, 24.0f),
    };
}

void Resolve()
{
    g_resolved = true;
    void* view = ue::FindStruct("MinimalViewInfo");
    void* settings = ue::FindStruct("PostProcessSettings");
    if (!view || !settings) {
        mod::Log("Post-process: MinimalViewInfo %p, PostProcessSettings %p, so these controls stay off", view, settings);
        return;
    }
    g_blendWeightOffset = ue::PropertyOffset(view, "PostProcessBlendWeight");
    g_settingsOffset = ue::PropertyOffset(view, "PostProcessSettings");
    int found = 0;
    for (Control& control : All()) {
        control.ValueOffset = ue::PropertyOffset(settings, control.Property);
        int byteOffset = -1;
        unsigned char mask = 0;
        if (ue::BoolProperty(settings, std::string("bOverride_") + control.Property, byteOffset, mask)) {
            control.OverrideByte = byteOffset;
            control.OverrideMask = mask;
        }
        if (control.ValueOffset >= 0 && control.OverrideByte >= 0)
            ++found;
        else
            mod::Log("Post-process: %s not found", control.Property);
    }
    g_available = g_blendWeightOffset >= 0 && g_settingsOffset >= 0 && found > 0;
    mod::Log("Post-process: weight at +0x%X, settings at +0x%X, %d of %zu controls found", g_blendWeightOffset, g_settingsOffset, found, All().size());
}

} // namespace

std::vector<Control>& All()
{
    static std::vector<Control> controls = MakeControls();
    return controls;
}

FocusSettings& FocusConfig()
{
    return g_focus;
}

bool Available()
{
    return g_available;
}

bool Checked()
{
    return g_resolved;
}

void Apply(void* manager, cine::Vec3 cameraLocation)
{
    // Looked up the first time a control is used, so nobody pays for this until then.
    bool any = false;
    for (const Control& control : All())
        any = any || control.On;
    if (!any && g_focus.Mode == Focus::Manual)
        return;
    if (!g_resolved)
        Resolve();
    if (!g_available)
        return;

    auto* view = reinterpret_cast<uint8_t*>(manager) + ue::off::PovLocation;
    auto* settings = view + g_settingsOffset;
    *reinterpret_cast<float*>(view + g_blendWeightOffset) = 1.0f;

    for (Control& control : All()) {
        float value = control.Value;
        bool on = control.On;
        // Autofocus keeps the focus distance on the chosen fighter.
        if (g_focus.Mode != Focus::Manual && std::string(control.Property) == "DepthOfFieldFocalDistance") {
            cine::Vec3 p1, p2;
            const bool have1 = game::FighterLocation(0, p1), have2 = game::FighterLocation(1, p2);
            if (g_focus.Mode == Focus::P1 && have1)
                value = static_cast<float>(cine::Length(p1 - cameraLocation));
            else if (g_focus.Mode == Focus::P2 && have2)
                value = static_cast<float>(cine::Length(p2 - cameraLocation));
            else if (g_focus.Mode == Focus::BetweenFighters && have1 && have2)
                value = static_cast<float>(cine::Length(cine::Lerp(p1, p2, 0.5) - cameraLocation));
            else if (g_focus.Mode == Focus::Camera)
                value = 1.0f; // sharp at the lens, softer the further from it
            control.Value = value;
            on = true;
        }
        if (!on || control.ValueOffset < 0 || control.OverrideByte < 0)
            continue;
        settings[control.OverrideByte] |= control.OverrideMask;
        if (control.Type == Type::Float) {
            *reinterpret_cast<float*>(settings + control.ValueOffset) = value;
        } else {
            auto* channels = reinterpret_cast<double*>(settings + control.ValueOffset);
            channels[0] = channels[1] = channels[2] = value;
            channels[3] = 1.0;
        }
    }
}

} // namespace postfx
