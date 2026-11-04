#include "mod/presets.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "common/config.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/postfx.h"
#include "mod/recorder.h"

namespace presets {

namespace fs = std::filesystem;

namespace {

fs::path Folder(const char* name)
{
    fs::path dir = mod::DataDir() / name;
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    return dir;
}

std::vector<std::string> ListFiles(const fs::path& dir, const char* extension)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error))
        if (entry.path().extension() == extension)
            names.push_back(entry.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string Key(const char* prefix, int index, const char* field)
{
    return std::string(prefix) + "." + std::to_string(index) + "." + field;
}

void WriteLook(Ini& ini)
{
    const game::TimeControl& t = game::Time();
    ini.SetNumber("time.speed", t.Speed);
    ini.SetBool("time.effects", t.ScaleEffects);
    ini.SetBool("time.blend", t.Interpolate);
    ini.SetNumber("camera.path_style", static_cast<double>(static_cast<int>(camera::Config().PathStyle)));
    ini.SetBool("time.ramp", t.UseSpeedRamp);

    const camera::Settings& c = camera::Config();
    ini.SetNumber("camera.filter", static_cast<int>(c.Filter));
    ini.SetNumber("camera.jump_distance", c.JumpDistance);
    ini.SetNumber("camera.jump_angle", c.JumpAngle);
    ini.SetNumber("camera.zoom", c.SeparationZoom);

    const effects::Settings& e = effects::Config();
    ini.SetBool("fx.hide_hud", e.HideHud);
    ini.SetBool("fx.no_hit", e.NoHitEffects);
    ini.SetBool("fx.no_drama", e.NoDramaManager);
    ini.SetBool("fx.no_bloom", e.NoBloom);
    ini.SetBool("fx.no_blur", e.NoMotionBlur);
    ini.SetBool("fx.no_dof", e.NoDepthOfField);
    ini.SetBool("fx.no_particles", e.NoParticles);
    ini.SetBool("fx.no_vignette", e.NoVignette);

    for (const dvars::Slider& slider : dvars::All()) {
        if (slider.Overridden)
            ini.SetNumber("dvar." + slider.Label, slider.Value);
    }
    const dvars::Colors& tint = dvars::Tint();
    if (tint.FogOverridden)
        for (int i = 0; i < 3; ++i)
            ini.SetNumber(Key("fog_color", i, "v"), tint.Fog[i]);
    if (tint.SunOverridden)
        for (int i = 0; i < 3; ++i)
            ini.SetNumber(Key("sun_color", i, "v"), tint.Sun[i]);

    for (const postfx::Control& control : postfx::All()) {
        if (control.On)
            ini.SetNumber(std::string("pp.") + control.Property, control.Value);
    }
    ini.SetNumber("pp.focus_mode", static_cast<int>(postfx::FocusConfig().Mode));

    for (int i = 0; i < lights::MaxLights; ++i) {
        const lights::Light& light = lights::Rig()[i];
        if (!light.On)
            continue;
        ini.SetBool(Key("light", i, "on"), true);
        ini.SetBool(Key("light", i, "spot"), light.Spot);
        ini.SetNumber(Key("light", i, "follow"), static_cast<int>(light.Follow));
        ini.SetNumber(Key("light", i, "intensity"), light.Intensity);
        ini.SetNumber(Key("light", i, "radius"), light.Radius);
        for (int axis = 0; axis < 3; ++axis) {
            ini.SetNumber(Key("light", i, ("offset" + std::to_string(axis)).c_str()), light.Offset[axis]);
            ini.SetNumber(Key("light", i, ("color" + std::to_string(axis)).c_str()), light.Color[axis]);
        }
    }

    const recorder::Settings& r = recorder::Config();
    ini.SetNumber("rec.fps", r.Fps);
    ini.SetNumber("rec.codec", r.CodecIndex);
    ini.SetNumber("rec.samples", r.MotionSamples);
    ini.SetNumber("rec.shutter", r.ShutterDegrees);
    for (int i = 0; i < recorder::MaxStreams; ++i) {
        ini.SetBool(Key("stream", i, "on"), r.Streams[i].Enabled);
        ini.SetNumber(Key("stream", i, "look"), static_cast<int>(r.Streams[i].Look));
    }
}

void ReadLook(const Ini& ini)
{
    game::TimeControl& t = game::Time();
    t.Speed = std::clamp(ini.GetNumber("time.speed", t.Speed), 0.01, 4.0);
    t.ScaleEffects = ini.GetBool("time.effects", t.ScaleEffects);
    t.Interpolate = ini.GetBool("time.blend", t.Interpolate);
    camera::Config().PathStyle = static_cast<cine::PathStyle>(std::clamp(static_cast<int>(ini.GetNumber("camera.path_style", static_cast<int>(camera::Config().PathStyle))), 0, 2));
    t.UseSpeedRamp = ini.GetBool("time.ramp", t.UseSpeedRamp);

    camera::Settings& c = camera::Config();
    c.Filter = static_cast<camera::Filter>(static_cast<int>(ini.GetNumber("camera.filter", static_cast<int>(c.Filter))));
    c.JumpDistance = ini.GetNumber("camera.jump_distance", c.JumpDistance);
    c.JumpAngle = ini.GetNumber("camera.jump_angle", c.JumpAngle);
    c.SeparationZoom = ini.GetNumber("camera.zoom", c.SeparationZoom);

    effects::Settings& e = effects::Config();
    e.HideHud = ini.GetBool("fx.hide_hud", e.HideHud);
    e.NoHitEffects = ini.GetBool("fx.no_hit", e.NoHitEffects);
    e.NoDramaManager = ini.GetBool("fx.no_drama", e.NoDramaManager);
    e.NoBloom = ini.GetBool("fx.no_bloom", e.NoBloom);
    e.NoMotionBlur = ini.GetBool("fx.no_blur", e.NoMotionBlur);
    e.NoDepthOfField = ini.GetBool("fx.no_dof", e.NoDepthOfField);
    e.NoParticles = ini.GetBool("fx.no_particles", e.NoParticles);
    e.NoVignette = ini.GetBool("fx.no_vignette", e.NoVignette);
    effects::RequestRefresh();

    for (dvars::Slider& slider : dvars::All()) {
        const std::string key = "dvar." + slider.Label;
        if (ini.Values().count(key)) {
            slider.Value = static_cast<float>(ini.GetNumber(key, slider.Value));
            dvars::Changed(slider);
        } else if (slider.Overridden) {
            dvars::Reset(slider);
        }
    }
    dvars::Colors& tint = dvars::Tint();
    tint.FogOverridden = ini.Values().count(Key("fog_color", 0, "v")) != 0;
    tint.SunOverridden = ini.Values().count(Key("sun_color", 0, "v")) != 0;
    for (int i = 0; i < 3; ++i) {
        tint.Fog[i] = static_cast<float>(ini.GetNumber(Key("fog_color", i, "v"), tint.Fog[i]));
        tint.Sun[i] = static_cast<float>(ini.GetNumber(Key("sun_color", i, "v"), tint.Sun[i]));
    }
    dvars::TintChanged();

    for (postfx::Control& control : postfx::All()) {
        const std::string key = std::string("pp.") + control.Property;
        control.On = ini.Values().count(key) != 0;
        control.Value = static_cast<float>(ini.GetNumber(key, control.Default));
    }
    postfx::FocusConfig().Mode = static_cast<postfx::Focus>(static_cast<int>(ini.GetNumber("pp.focus_mode", 0)));

    for (int i = 0; i < lights::MaxLights; ++i) {
        lights::Light& light = lights::Rig()[i];
        light.On = ini.GetBool(Key("light", i, "on"), false);
        light.Spot = ini.GetBool(Key("light", i, "spot"), light.Spot);
        light.Follow = static_cast<lights::Follow>(static_cast<int>(ini.GetNumber(Key("light", i, "follow"), static_cast<int>(light.Follow))));
        light.Intensity = static_cast<float>(ini.GetNumber(Key("light", i, "intensity"), light.Intensity));
        light.Radius = static_cast<float>(ini.GetNumber(Key("light", i, "radius"), light.Radius));
        for (int axis = 0; axis < 3; ++axis) {
            light.Offset[axis] = static_cast<float>(ini.GetNumber(Key("light", i, ("offset" + std::to_string(axis)).c_str()), light.Offset[axis]));
            light.Color[axis] = static_cast<float>(ini.GetNumber(Key("light", i, ("color" + std::to_string(axis)).c_str()), light.Color[axis]));
        }
    }

    recorder::Settings& r = recorder::Config();
    r.Fps = static_cast<int>(ini.GetNumber("rec.fps", r.Fps));
    r.CodecIndex = static_cast<int>(ini.GetNumber("rec.codec", r.CodecIndex));
    r.MotionSamples = static_cast<int>(ini.GetNumber("rec.samples", r.MotionSamples));
    r.ShutterDegrees = static_cast<float>(ini.GetNumber("rec.shutter", r.ShutterDegrees));
    for (int i = 0; i < recorder::MaxStreams; ++i) {
        r.Streams[i].Enabled = ini.GetBool(Key("stream", i, "on"), r.Streams[i].Enabled);
        r.Streams[i].Look = static_cast<recorder::Look>(static_cast<int>(ini.GetNumber(Key("stream", i, "look"), static_cast<int>(r.Streams[i].Look))));
    }
}

} // namespace

bool SavePreset(const std::string& name)
{
    Ini ini;
    WriteLook(ini);
    return ini.Save(Folder("presets") / (name + ".t8preset"));
}

bool LoadPreset(const std::string& name)
{
    Ini ini;
    if (!ini.Load(Folder("presets") / (name + ".t8preset")))
        return false;
    ReadLook(ini);
    return true;
}

std::vector<std::string> SavedPresets()
{
    return ListFiles(Folder("presets"), ".t8preset");
}

bool SaveShot(const std::string& name)
{
    Ini ini;
    WriteLook(ini);
    const auto keys = camera::Keys();
    ini.SetNumber("path.count", static_cast<double>(keys.size()));
    ini.SetBool("path.counted_yaw", true);
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        char row[256];
        snprintf(row, sizeof(row), "%.6f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", keys[i].Time, keys[i].Location.X, keys[i].Location.Y, keys[i].Location.Z,
                 keys[i].Rotation.Pitch, keys[i].Rotation.Yaw, keys[i].Rotation.Roll, keys[i].Fov);
        ini.Set("path." + std::to_string(i), row);
    }
    const auto speed = game::SpeedKeys();
    ini.SetNumber("ramp.count", static_cast<double>(speed.size()));
    for (int i = 0; i < static_cast<int>(speed.size()); ++i) {
        char row[64];
        snprintf(row, sizeof(row), "%.6f %.6f", speed[i].Time, speed[i].Speed);
        ini.Set("ramp." + std::to_string(i), row);
    }
    return ini.Save(Folder("shots") / (name + ".t8shot"));
}

bool LoadShot(const std::string& name)
{
    Ini ini;
    if (!ini.Load(Folder("shots") / (name + ".t8shot")))
        return false;
    ReadLook(ini);

    std::vector<cine::CameraKey> keys;
    const int keyCount = static_cast<int>(ini.GetNumber("path.count", 0));
    for (int i = 0; i < keyCount; ++i) {
        cine::CameraKey key;
        if (sscanf_s(ini.Get("path." + std::to_string(i)).c_str(), "%lf %lf %lf %lf %lf %lf %lf %lf", &key.Time, &key.Location.X, &key.Location.Y,
                     &key.Location.Z, &key.Rotation.Pitch, &key.Rotation.Yaw, &key.Rotation.Roll, &key.Fov) == 8)
            keys.push_back(key);
    }
    if (!ini.GetBool("path.counted_yaw", false))
        cine::CountYaw(keys);
    camera::SetKeys(std::move(keys));

    std::vector<cine::SpeedKey> speed;
    const int speedCount = static_cast<int>(ini.GetNumber("ramp.count", 0));
    for (int i = 0; i < speedCount; ++i) {
        cine::SpeedKey key;
        if (sscanf_s(ini.Get("ramp." + std::to_string(i)).c_str(), "%lf %lf", &key.Time, &key.Speed) == 2)
            speed.push_back(key);
    }
    game::SetSpeedKeys(std::move(speed));
    return true;
}

std::vector<std::string> SavedShots()
{
    return ListFiles(Folder("shots"), ".t8shot");
}

} // namespace presets
