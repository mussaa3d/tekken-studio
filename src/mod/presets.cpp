#include "mod/presets.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#include "common/config.h"
#include "common/lookmath.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/postfx.h"
#include "mod/recorder.h"
#include "mod/replaydisplay.h"
#include "mod/scene.h"
#include "mod/sky.h"

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
    ini.SetBool("time.no_game_slowmo", t.NoGameSlowMotion);
    for (int row = 0; row < replaydisplay::Rows; ++row)
        ini.SetNumber("replay_display." + std::to_string(row), replaydisplay::Choice(row));
    ini.SetBool("camera.thirds", camera::Config().RuleOfThirds);

    const camera::Settings& c = camera::Config();
    ini.SetNumber("camera.filter", static_cast<int>(c.Filter));
    ini.SetNumber("camera.jump_distance", c.JumpDistance);
    ini.SetNumber("camera.jump_angle", c.JumpAngle);
    ini.SetNumber("camera.zoom", c.SeparationZoom);

    const effects::Settings& e = effects::Config();
    ini.SetBool("fx.hide_hud", e.HideHud);
    ini.SetBool("fx.keep_battle_status", e.KeepBattleStatus);
    for (int item = 0; item < replaydisplay::StatusItems; ++item)
        ini.SetBool("replay_status." + std::to_string(item), replaydisplay::StatusShown(item));
    ini.SetBool("fx.hide_hit_labels", e.HideHitLabels);
    const scene::Settings& sc = scene::Config();
    ini.SetBool("scene.no_character_lights", sc.NoCharacterLights);
    ini.SetBool("scene.no_stage_lights", sc.NoStageLights);
    ini.SetBool("scene.tint_character_lights", sc.TintCharacterLights);
    for (int i = 0; i < 3; ++i)
        ini.SetNumber("scene.character_light_" + std::to_string(i), sc.CharacterLightColor[i]);
    ini.SetBool("scene.plain_stage", sc.PlainStage);
    ini.SetBool("fog.from_camera", dvars::Tint().FogFromCamera);
    ini.SetBool("scene.no_camera_shake", sc.NoCameraShake);
    ini.SetNumber("scene.frame", static_cast<int>(sc.Frame));
    ini.SetNumber("scene.aspect_ratio", sc.AspectRatio);
    ini.SetBool("fx.no_hit", e.NoHitEffects);
    ini.SetBool("fx.fog_hides_mist", e.FogHidesMist);
    ini.SetBool("fx.no_drama", e.NoDramaManager);
    ini.SetBool("fx.no_bloom", e.NoBloom);
    ini.SetBool("fx.no_blur", e.NoMotionBlur);
    ini.SetBool("fx.no_dof", e.NoDepthOfField);
    ini.SetBool("fx.no_particles", e.NoParticles);
    ini.SetBool("fx.no_vignette", e.NoVignette);

    const sky::Settings& k = sky::Config();
    ini.SetNumber("sky.sun_yaw", k.SunYaw);
    ini.SetNumber("sky.sun_height", k.SunHeight);
    ini.SetNumber("sky.rotation", k.SkyRotation);
    ini.SetBool("sky.hdri_on", k.HdriOn);
    ini.Set("sky.hdri_file", k.HdriFile);
    ini.SetNumber("sky.hdri_brightness", k.HdriBrightness);
    ini.SetBool("sky.hdri_lights_stage", k.HdriLightsStage);
    ini.SetBool("fighter.hide_0", k.HideFighter[0]);
    ini.SetBool("fighter.hide_1", k.HideFighter[1]);

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
    t.NoGameSlowMotion = ini.GetBool("time.no_game_slowmo", t.NoGameSlowMotion);
    // Only what's different is applied, so the look keys can run this every frame.
    bool displayChanged = false;
    for (int row = 0; row < replaydisplay::Rows; ++row) {
        const int choice = static_cast<int>(ini.GetNumber("replay_display." + std::to_string(row), replaydisplay::Choice(row)));
        displayChanged = displayChanged || choice != replaydisplay::Choice(row);
        replaydisplay::Choice(row) = choice;
    }
    camera::Config().RuleOfThirds = ini.GetBool("camera.thirds", camera::Config().RuleOfThirds);
    const dvars::Colors tintBefore = dvars::Tint();

    camera::Settings& c = camera::Config();
    c.Filter = static_cast<camera::Filter>(static_cast<int>(ini.GetNumber("camera.filter", static_cast<int>(c.Filter))));
    c.JumpDistance = ini.GetNumber("camera.jump_distance", c.JumpDistance);
    c.JumpAngle = ini.GetNumber("camera.jump_angle", c.JumpAngle);
    c.SeparationZoom = ini.GetNumber("camera.zoom", c.SeparationZoom);

    effects::Settings& e = effects::Config();
    const effects::Settings effectsBefore = e;
    e.HideHud = ini.GetBool("fx.hide_hud", e.HideHud);
    e.KeepBattleStatus = ini.GetBool("fx.keep_battle_status", e.KeepBattleStatus);
    for (int item = 0; item < replaydisplay::StatusItems; ++item) {
        const bool shown = ini.GetBool("replay_status." + std::to_string(item), replaydisplay::StatusShown(item));
        displayChanged = displayChanged || shown != replaydisplay::StatusShown(item);
        replaydisplay::StatusShown(item) = shown;
    }
    if (displayChanged)
        replaydisplay::Changed();
    e.HideHitLabels = ini.GetBool("fx.hide_hit_labels", e.HideHitLabels);
    scene::Settings& sc = scene::Config();
    sc.NoCharacterLights = ini.GetBool("scene.no_character_lights", sc.NoCharacterLights);
    sc.NoStageLights = ini.GetBool("scene.no_stage_lights", sc.NoStageLights);
    sc.TintCharacterLights = ini.GetBool("scene.tint_character_lights", sc.TintCharacterLights);
    for (int i = 0; i < 3; ++i)
        sc.CharacterLightColor[i] = static_cast<float>(ini.GetNumber("scene.character_light_" + std::to_string(i), sc.CharacterLightColor[i]));
    sc.PlainStage = ini.GetBool("scene.plain_stage", sc.PlainStage);
    dvars::Tint().FogFromCamera = ini.GetBool("fog.from_camera", dvars::Tint().FogFromCamera);
    sc.NoCameraShake = ini.GetBool("scene.no_camera_shake", sc.NoCameraShake);
    sc.Frame = static_cast<scene::Frame>(static_cast<int>(ini.GetNumber("scene.frame", static_cast<int>(sc.Frame))));
    sc.AspectRatio = static_cast<float>(ini.GetNumber("scene.aspect_ratio", sc.AspectRatio));
    e.NoHitEffects = ini.GetBool("fx.no_hit", e.NoHitEffects);
    e.FogHidesMist = ini.GetBool("fx.fog_hides_mist", e.FogHidesMist);
    e.NoDramaManager = ini.GetBool("fx.no_drama", e.NoDramaManager);
    e.NoBloom = ini.GetBool("fx.no_bloom", e.NoBloom);
    e.NoMotionBlur = ini.GetBool("fx.no_blur", e.NoMotionBlur);
    e.NoDepthOfField = ini.GetBool("fx.no_dof", e.NoDepthOfField);
    e.NoParticles = ini.GetBool("fx.no_particles", e.NoParticles);
    e.NoVignette = ini.GetBool("fx.no_vignette", e.NoVignette);
    if (std::memcmp(&effectsBefore, &e, sizeof(e)) != 0)
        effects::RequestRefresh();

    sky::Settings& k = sky::Config();
    k.SunYaw = static_cast<float>(ini.GetNumber("sky.sun_yaw", k.SunYaw));
    k.SunHeight = static_cast<float>(ini.GetNumber("sky.sun_height", k.SunHeight));
    k.SkyRotation = static_cast<float>(ini.GetNumber("sky.rotation", k.SkyRotation));
    k.HdriOn = ini.GetBool("sky.hdri_on", k.HdriOn);
    if (ini.Values().count("sky.hdri_file"))
        strncpy_s(k.HdriFile, ini.Get("sky.hdri_file").c_str(), _TRUNCATE);
    k.HdriBrightness = static_cast<float>(ini.GetNumber("sky.hdri_brightness", k.HdriBrightness));
    k.HdriLightsStage = ini.GetBool("sky.hdri_lights_stage", k.HdriLightsStage);
    k.HideFighter[0] = ini.GetBool("fighter.hide_0", k.HideFighter[0]);
    k.HideFighter[1] = ini.GetBool("fighter.hide_1", k.HideFighter[1]);

    for (dvars::Slider& slider : dvars::All()) {
        const std::string key = "dvar." + slider.Label;
        if (ini.Values().count(key)) {
            float value = static_cast<float>(ini.GetNumber(key, slider.Value));
            if (slider.Whole)
                value = std::round(value);
            if (!slider.Overridden || value != slider.Value) {
                slider.Value = value;
                dvars::Changed(slider);
            }
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
    if (std::memcmp(&tintBefore, &tint, sizeof(tint)) != 0)
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

// ----- Look keys
namespace {
struct LookKey {
    double Time = 0;
    cine::LookValues Values;
};
std::mutex g_lookMutex;
std::vector<LookKey> g_lookKeys;
std::atomic<uint64_t> g_lookVersion { 0 };
std::atomic<int> g_lookDrove { 0 };

// What a look key leaves alone: the playback speed has a lane of its own, and the recording setup and camera options aren't a look.
bool InLookKey(const std::string& key)
{
    for (const char* prefix : { "time.", "rec.", "stream.", "camera." })
        if (key.rfind(prefix, 0) == 0)
            return false;
    return true;
}

cine::LookValues CaptureLook()
{
    Ini ini;
    WriteLook(ini);
    cine::LookValues values;
    for (const auto& [key, value] : ini.Values())
        if (InLookKey(key))
            values[key] = value;
    return values;
}

// Sliders and colours move between keys. Everything else is a switch or a choice, and changes at the key.
bool MovesSmoothly(const std::string& key)
{
    if (key.rfind("dvar.", 0) == 0) {
        for (const dvars::Slider& slider : dvars::All())
            if (key.compare(5, std::string::npos, slider.Label) == 0)
                return slider.Choices.empty();
        return false;
    }
    if (key.rfind("light.", 0) == 0)
        return key.find(".intensity") != std::string::npos || key.find(".radius") != std::string::npos || key.find(".offset") != std::string::npos || key.find(".color") != std::string::npos;
    if (key == "pp.focus_mode")
        return false;
    for (const char* prefix : { "pp.", "fog_color.", "sun_color.", "scene.character_light_" })
        if (key.rfind(prefix, 0) == 0)
            return true;
    return key == "scene.aspect_ratio" || key == "sky.sun_yaw" || key == "sky.sun_height" || key == "sky.rotation" || key == "sky.hdri_brightness";
}
} // namespace

std::vector<double> LookKeyTimes()
{
    std::lock_guard lock(g_lookMutex);
    std::vector<double> times;
    for (const LookKey& key : g_lookKeys)
        times.push_back(key.Time);
    return times;
}

void AddLookKey()
{
    LookKey key { game::Clock(), CaptureLook() };
    std::lock_guard lock(g_lookMutex);
    const auto same = std::find_if(g_lookKeys.begin(), g_lookKeys.end(), [&](const LookKey& k) { return std::fabs(k.Time - key.Time) < game::StepSeconds * 0.5; });
    if (same != g_lookKeys.end()) {
        *same = std::move(key);
    } else {
        g_lookKeys.push_back(std::move(key));
        std::stable_sort(g_lookKeys.begin(), g_lookKeys.end(), [](const LookKey& a, const LookKey& b) { return a.Time < b.Time; });
    }
    ++g_lookVersion;
}

void RemoveLookKey(size_t index)
{
    std::lock_guard lock(g_lookMutex);
    if (index < g_lookKeys.size())
        g_lookKeys.erase(g_lookKeys.begin() + index);
    ++g_lookVersion;
}

size_t MoveLookKey(size_t index, double time)
{
    std::lock_guard lock(g_lookMutex);
    if (index >= g_lookKeys.size())
        return index;
    LookKey moved = std::move(g_lookKeys[index]);
    moved.Time = time;
    g_lookKeys.erase(g_lookKeys.begin() + index);
    const auto at = std::upper_bound(g_lookKeys.begin(), g_lookKeys.end(), time, [](double t, const LookKey& k) { return t < k.Time; });
    const size_t position = static_cast<size_t>(at - g_lookKeys.begin());
    g_lookKeys.insert(at, std::move(moved));
    ++g_lookVersion;
    return position;
}

void ClearLookKeys()
{
    std::lock_guard lock(g_lookMutex);
    g_lookKeys.clear();
    ++g_lookVersion;
}

void OnGameTick()
{
    static uint64_t seenVersion = ~0ull;
    static double seenClock = -1e300;
    static cine::LookValues applied;
    const double clock = game::Clock();
    const uint64_t version = g_lookVersion.load(std::memory_order_relaxed);
    if (version == seenVersion && clock == seenClock)
        return;
    seenVersion = version;
    seenClock = clock;

    cine::LookValues look;
    {
        std::lock_guard lock(g_lookMutex);
        if (g_lookKeys.size() < 2) {
            applied.clear();
            return;
        }
        const auto next = std::upper_bound(g_lookKeys.begin(), g_lookKeys.end(), clock, [](double t, const LookKey& k) { return t < k.Time; });
        if (next == g_lookKeys.begin()) {
            look = g_lookKeys.front().Values;
        } else if (next == g_lookKeys.end()) {
            look = g_lookKeys.back().Values;
        } else {
            const LookKey& from = *std::prev(next);
            look = cine::BlendLook(from.Values, next->Values, (clock - from.Time) / (next->Time - from.Time), MovesSmoothly);
        }
    }
    // The same look as last time means the clock is between the same values, or stopped: what's being edited by hand is left alone.
    if (look == applied)
        return;
    Ini ini;
    for (const auto& [key, value] : look)
        ini.Set(key, value);
    applied = std::move(look);
    ReadLook(ini);
    g_lookDrove.store(12, std::memory_order_relaxed);
}

void WriteShot(Ini& ini)
{
    WriteLook(ini);
    {
        std::lock_guard lock(g_lookMutex);
        ini.SetNumber("lookkey.count", static_cast<double>(g_lookKeys.size()));
        for (size_t i = 0; i < g_lookKeys.size(); ++i) {
            const std::string prefix = "lookkey." + std::to_string(i);
            char time[32];
            snprintf(time, sizeof(time), "%.6f", g_lookKeys[i].Time);
            ini.Set(prefix + ".time", time);
            for (const auto& [key, value] : g_lookKeys[i].Values)
                ini.Set(prefix + "/" + key, value);
        }
    }
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
}

void ReadShot(const Ini& ini)
{
    ReadLook(ini);

    std::vector<LookKey> lookKeys(static_cast<size_t>(std::clamp(static_cast<int>(ini.GetNumber("lookkey.count", 0)), 0, 512)));
    for (size_t i = 0; i < lookKeys.size(); ++i)
        lookKeys[i].Time = ini.GetNumber("lookkey." + std::to_string(i) + ".time", 0);
    for (const auto& [key, value] : ini.Values()) {
        const size_t slash = key.find('/');
        if (key.rfind("lookkey.", 0) != 0 || slash == std::string::npos)
            continue;
        const int index = std::atoi(key.c_str() + 8);
        if (index >= 0 && index < static_cast<int>(lookKeys.size()))
            lookKeys[index].Values[key.substr(slash + 1)] = value;
    }
    std::stable_sort(lookKeys.begin(), lookKeys.end(), [](const LookKey& a, const LookKey& b) { return a.Time < b.Time; });
    {
        std::lock_guard lock(g_lookMutex);
        g_lookKeys = std::move(lookKeys);
        ++g_lookVersion;
    }

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
}

bool SaveShot(const std::string& name)
{
    Ini ini;
    WriteShot(ini);
    return ini.Save(Folder("shots") / (name + ".t8shot"));
}

bool LoadShot(const std::string& name)
{
    Ini ini;
    if (!ini.Load(Folder("shots") / (name + ".t8shot")))
        return false;
    ReadShot(ini);
    return true;
}

// ----- Undo. The editor's state is written down the same way a shot is, and a change shows up as a difference from the last copy.
namespace {
std::mutex g_undoMutex;
std::vector<Ini> g_undo, g_redo;
Ini g_undoCurrent;
bool g_undoReady = false;
constexpr size_t MaxUndo = 64;

Ini CaptureForUndo()
{
    Ini ini;
    WriteShot(ini);
    if (game::Time().UseSpeedRamp)
        ini.Set("time.speed", ""); // the ramp moves it every frame
    return ini;
}
} // namespace

void UndoWatch(bool settled)
{
    // What the look keys change while the replay plays isn't something to undo.
    const int drove = g_lookDrove.load(std::memory_order_relaxed);
    if (drove > 0) {
        g_lookDrove.store(drove - 1, std::memory_order_relaxed);
        if (drove == 1) {
            Ini now = CaptureForUndo();
            std::lock_guard lock(g_undoMutex);
            g_undoCurrent = std::move(now);
            g_undoReady = true;
        }
        return;
    }
    // Not while something is being dragged, so one drag is one step.
    static int wait = 0;
    if (!settled || --wait > 0)
        return;
    wait = 6;
    Ini now = CaptureForUndo();
    std::lock_guard lock(g_undoMutex);
    if (!g_undoReady) {
        g_undoCurrent = std::move(now);
        g_undoReady = true;
        return;
    }
    if (now.Values() == g_undoCurrent.Values())
        return;
    g_undo.push_back(std::move(g_undoCurrent));
    if (g_undo.size() > MaxUndo)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_undoCurrent = std::move(now);
}

static bool StepUndo(std::vector<Ini>& from, std::vector<Ini>& to)
{
    std::lock_guard lock(g_undoMutex);
    if (from.empty())
        return false;
    to.push_back(std::move(g_undoCurrent));
    g_undoCurrent = std::move(from.back());
    from.pop_back();
    Ini wanted = g_undoCurrent;
    if (wanted.Get("time.speed").empty())
        wanted.SetNumber("time.speed", game::Time().Speed);
    ReadShot(wanted);
    return true;
}

bool Undo()
{
    return StepUndo(g_undo, g_redo);
}

bool Redo()
{
    return StepUndo(g_redo, g_undo);
}

void ResetAll()
{
    camera::Config() = {};
    camera::ClearKeys();
    game::SetSpeedKeys({});
    ClearLookKeys();
    sky::Config() = {};
    game::Time() = {};
    replaydisplay::ClearChoices();
    effects::Config() = {};
    effects::RequestRefresh();
    scene::Config() = {};
    lights::Rig() = {};
    postfx::FocusConfig() = {};
    for (postfx::Control& control : postfx::All()) {
        control.On = false;
        control.Value = control.Default;
    }
    for (dvars::Slider& slider : dvars::All())
        if (slider.Overridden)
            dvars::Reset(slider);
    dvars::Tint() = {};
    dvars::TintChanged();

    // The recording setup, but not where it saves or which ffmpeg it uses.
    recorder::Settings& r = recorder::Config();
    const recorder::Settings fresh;
    r.Fps = fresh.Fps;
    r.CodecIndex = fresh.CodecIndex;
    r.SettleFrames = fresh.SettleFrames;
    r.MotionSamples = fresh.MotionSamples;
    r.ShutterDegrees = fresh.ShutterDegrees;
    r.Preview = fresh.Preview;
    r.BlueKey = fresh.BlueKey;
    r.GreenDarkness = fresh.GreenDarkness;
    r.DepthNear = fresh.DepthNear;
    r.DepthFar = fresh.DepthFar;
    r.DepthCurve = fresh.DepthCurve;
    r.UseRange = false;
    for (int i = 0; i < recorder::MaxStreams; ++i)
        r.Streams[i].Enabled = i == 0;
    mod::Log("Reset: everything back to how it starts");
}

std::vector<std::string> SavedShots()
{
    return ListFiles(Folder("shots"), ".t8shot");
}

} // namespace presets
