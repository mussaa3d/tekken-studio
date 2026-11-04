#include "mod/testcmd.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/guard.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/postfx.h"
#include "mod/presets.h"
#include "mod/recorder.h"
#include "mod/replay.h"
#include "mod/savetrace.h"
#include "mod/ue.h"

namespace testcmd {

namespace {

bool g_enabled = false;

void WriteState()
{
    std::ofstream out(mod::DataDir() / "state.tmp", std::ios::trunc);
    const game::TimeControl& t = game::Time();
    const camera::Settings& c = camera::Config();
    const camera::View view = camera::LastView();
    const game::Rates rates = game::MeasuredRates();
    cine::Vec3 p1, p2;
    const bool have1 = game::FighterLocation(0, p1), have2 = game::FighterLocation(1, p2);
    out << "clock " << game::Clock() << "\n";
    out << "steps " << game::StepCount() << "\n";
    out << "ticks_per_second " << rates.TicksPerSecond << "\n";
    out << "fps " << rates.FramesPerSecond << "\n";
    out << "speed " << t.Speed << "\n";
    out << "paused " << t.Paused << "\n";
    out << "mode " << static_cast<int>(c.Mode) << "\n";
    out << "filter " << static_cast<int>(c.Filter) << " holding " << camera::InCinematicHold() << "\n";
    out << "view " << view.Location.X << " " << view.Location.Y << " " << view.Location.Z << " " << view.Rotation.Pitch << " " << view.Rotation.Yaw << " "
        << view.Rotation.Roll << " " << view.Fov << "\n";
    out << "fighters " << have1 << " " << have2 << " fights " << game::FightCount() << "\n";
    if (have1)
        out << "p1 " << p1.X << " " << p1.Y << " " << p1.Z << "\n";
    if (have2)
        out << "p2 " << p2.X << " " << p2.Y << " " << p2.Z << "\n";
    out << "keys " << camera::Keys().size() << " speedkeys " << game::SpeedKeys().size() << "\n";
    out << "bones " << camera::Bones(0).size() << " " << camera::Bones(1).size() << "\n";
    out << "menu " << overlay::MenuOpen() << "\n";
    out << "rec active " << recorder::Active() << " finishing " << recorder::Finishing() << " frames " << recorder::Frames() << "\n";
    out << "rec_file " << recorder::LastFile() << "\n";
    out << "rec_error " << recorder::LastError() << "\n";
    out << "audio running " << audio::Running() << " seconds " << audio::SecondsCaptured() << " heard " << audio::HeardSound() << "\n";
    out << "audio_file " << audio::LastFile() << "\n";
    out << "audio_error " << audio::LastError() << "\n";
    out << "steam " << guard::SteamState() << " connections " << guard::LastConnectionCount() << " tripped " << guard::Tripped() << " blocked " << guard::BlockedAttempts() << "\n";
    out << "postfx checked " << postfx::Checked() << " available " << postfx::Available() << "\n";
    out << "world " << ue::NameOf(ue::World()) << "\n";
    int round = -1;
    uint32_t frame = 0;
    const bool inFight = replay::Position(round, frame);
    out << "position " << inFight << " round " << round << " frame " << frame << " canjump " << replay::CanJump() << " seeking " << game::Seeking() << "\n";
    out.close();
    std::error_code ignored;
    std::filesystem::rename(mod::DataDir() / "state.tmp", mod::DataDir() / "state.txt", ignored);
}

std::string Rest(std::istringstream& in)
{
    std::string rest;
    std::getline(in, rest);
    const size_t begin = rest.find_first_not_of(' ');
    return begin == std::string::npos ? std::string() : rest.substr(begin);
}

void Run(const std::string& line)
{
    std::istringstream in(line);
    std::string command;
    in >> command;
    game::TimeControl& t = game::Time();
    camera::Settings& c = camera::Config();
    recorder::Settings& r = recorder::Config();
    effects::Settings& e = effects::Config();
    double a = 0, b = 0, d = 0;
    int n = 0, m = 0;

    if (command == "state") {
        WriteState();
    } else if (command == "menu") {
        if ((in >> n) && static_cast<bool>(n) != overlay::MenuOpen())
            overlay::ToggleMenu();
    } else if (command == "tab") {
        in >> n;
        menu::SetTab(n);
    } else if (command == "mode") {
        std::string name;
        in >> name;
        if (name == "free") {
            if (c.Mode != camera::Mode::Free)
                camera::ToggleFree();
        } else if (name == "dolly") {
            if (c.Mode != camera::Mode::Dolly)
                camera::ToggleDolly();
        } else if (name == "attach") {
            c.Mode = camera::Mode::Attach;
        } else {
            c.Mode = camera::Mode::Game;
        }
    } else if (command == "pause") {
        in >> n;
        t.Paused = n != 0;
    } else if (command == "step") {
        n = 1;
        in >> n;
        for (int i = 0; i < n; ++i)
            game::StepOneFrame();
    } else if (command == "speed") {
        in >> t.Speed;
    } else if (command == "ramp") {
        in >> n;
        t.UseSpeedRamp = n != 0;
    } else if (command == "blend") {
        in >> n;
        t.Interpolate = n != 0;
    } else if (command == "scalefx") {
        in >> n;
        t.ScaleEffects = n != 0;
    } else if (command == "fpscap") {
        in >> t.FpsCap;
    } else if (command == "filter") {
        in >> n;
        c.Filter = static_cast<camera::Filter>(n);
    } else if (command == "jump") {
        in >> c.JumpDistance >> c.JumpAngle >> c.SeparationZoom;
    } else if (command == "fov") {
        in >> c.Fov;
    } else if (command == "fovgame") {
        in >> n;
        c.FovOverride = n != 0;
    } else if (command == "pose") {
        double x, y, z, pitch, yaw, roll, fov;
        if (in >> x >> y >> z >> pitch >> yaw >> roll >> fov)
            camera::SetFreePose({ x, y, z }, { pitch, yaw, roll }, fov);
    } else if (command == "count") {
        std::string name;
        in >> name;
        void* cls = ue::FindClass(name);
        const auto objects = ue::AllObjectsOf(cls);
        mod::Log("[test] %s: class %p, %zu objects", name.c_str(), cls, objects.size());
        for (size_t i = 0; i < objects.size() && i < 4; ++i)
            mod::Log("[test]   %s (%s)", ue::NameOf(objects[i]).c_str(), ue::ClassNameOf(objects[i]).c_str());
    } else if (command == "prop") {
        // prop <Class> <Property>: logs a float property on every object of the class
        std::string name, property;
        in >> name >> property;
        void* cls = ue::FindClass(name);
        const int offset = cls ? ue::PropertyOffset(cls, property) : -1;
        mod::Log("[test] %s.%s at offset %d", name.c_str(), property.c_str(), offset);
        if (offset >= 0)
            for (void* object : ue::AllObjectsOf(cls))
                mod::Log("[test]   %s = %g", ue::NameOf(object).c_str(), ue::Field<float>(object, offset));
    } else if (command == "addkey") {
        camera::AddKeyAtCurrentView();
    } else if (command == "clearkeys") {
        camera::ClearKeys();
    } else if (command == "speedkey") {
        in >> a >> b;
        auto keys = game::SpeedKeys();
        keys.push_back({ a, b });
        game::SetSpeedKeys(keys);
    } else if (command == "clearspeed") {
        game::SetSpeedKeys({});
    } else if (command == "seek") {
        in >> a;
        game::SeekTo(a);
    } else if (command == "zero") {
        game::ZeroClock();
    } else if (command == "hud") {
        in >> n;
        e.HideHud = n != 0;
    } else if (command == "fx") {
        std::string name;
        in >> name >> n;
        bool* flag = name == "hit" ? &e.NoHitEffects : name == "drama" ? &e.NoDramaManager : name == "bloom" ? &e.NoBloom : name == "blur" ? &e.NoMotionBlur
            : name == "dof" ? &e.NoDepthOfField : name == "particles" ? &e.NoParticles : name == "vignette" ? &e.NoVignette : nullptr;
        if (flag) {
            *flag = n != 0;
            effects::RequestRefresh();
        }
    } else if (command == "pp") {
        std::string property;
        in >> property >> a;
        for (postfx::Control& control : postfx::All())
            if (property == control.Property) {
                control.On = true;
                control.Value = static_cast<float>(a);
            }
    } else if (command == "ppoff") {
        for (postfx::Control& control : postfx::All())
            control.On = false;
    } else if (command == "focus") {
        in >> n;
        postfx::FocusConfig().Mode = static_cast<postfx::Focus>(n);
    } else if (command == "dvar" || command == "dvarreset") {
        std::string label;
        in >> label;
        std::replace(label.begin(), label.end(), '_', ' ');
        in >> a;
        for (dvars::Slider& slider : dvars::All())
            if (slider.Label == label || slider.Group + " " + slider.Label == label) {
                if (command == "dvar") {
                    slider.Value = static_cast<float>(a);
                    dvars::Changed(slider);
                } else {
                    dvars::Reset(slider);
                }
            }
    } else if (command == "fogcolor" || command == "suncolor") {
        float* out = command == "fogcolor" ? dvars::Tint().Fog : dvars::Tint().Sun;
        in >> out[0] >> out[1] >> out[2];
        (command == "fogcolor" ? dvars::Tint().FogOverridden : dvars::Tint().SunOverridden) = true;
        dvars::TintChanged();
    } else if (command == "light") {
        lights::Light light;
        int on = 0, spot = 0, follow = 0;
        in >> n >> on >> spot >> follow >> light.Offset[0] >> light.Offset[1] >> light.Offset[2] >> light.Intensity >> light.Radius >> light.Color[0] >> light.Color[1] >> light.Color[2];
        light.On = on != 0;
        light.Spot = spot != 0;
        light.Follow = static_cast<lights::Follow>(follow);
        lights::Rig()[std::clamp(n, 0, lights::MaxLights - 1)] = light;
    } else if (command == "look") {
        in >> n;
        r.Preview = static_cast<recorder::Look>(n);
    } else if (command == "lightinfo") {
        void* cls = ue::FindClass("LightComponent");
        void* scene = ue::FindClass("SceneComponent");
        int count = 0;
        for (void* light : ue::AllObjectsOf(cls)) {
            const std::string name = ue::NameOf(light);
            ++count;
            if (name.rfind("PointLightComponent_", 0) != 0 && count > 3)
                continue;
            struct {
                cine::Vec3 ReturnValue;
            } where {};
            static ue::FunctionRef location { "SceneComponent", "K2_GetComponentLocation" };
            ue::Call(light, location, &where);
            int visibleByte = -1, affectsByte = -1, hiddenByte = -1, movableAt = ue::PropertyOffset(scene, "Mobility");
            uint8_t visibleMask = 0, affectsMask = 0, hiddenMask = 0;
            ue::BoolProperty(scene, "bVisible", visibleByte, visibleMask);
            ue::BoolProperty(scene, "bHiddenInGame", hiddenByte, hiddenMask);
            ue::BoolProperty(cls, "bAffectsWorld", affectsByte, affectsMask);
            const int intensityAt = ue::PropertyOffset(cls, "Intensity");
            const int colorAt = ue::PropertyOffset(cls, "LightColor");
            const int channelsAt = ue::PropertyOffset(cls, "LightingChannels");
            mod::Log("[test] %s (%s): at %.0f %.0f %.0f intensity %g visible %d hidden %d affects %d mobility %d color %08X channels %02X", name.c_str(), ue::ClassNameOf(light).c_str(),
                     where.ReturnValue.X, where.ReturnValue.Y, where.ReturnValue.Z, intensityAt >= 0 ? ue::Field<float>(light, intensityAt) : -1.0f,
                     visibleByte >= 0 ? (ue::Field<uint8_t>(light, visibleByte) & visibleMask) != 0 : -1, hiddenByte >= 0 ? (ue::Field<uint8_t>(light, hiddenByte) & hiddenMask) != 0 : -1,
                     affectsByte >= 0 ? (ue::Field<uint8_t>(light, affectsByte) & affectsMask) != 0 : -1, movableAt >= 0 ? ue::Field<uint8_t>(light, movableAt) : -1,
                     colorAt >= 0 ? ue::Field<uint32_t>(light, colorAt) : 0, channelsAt >= 0 ? ue::Field<uint8_t>(light, channelsAt) : 0);
        }
        mod::Log("[test] %d lights in all", count);
    } else if (command == "greendark") {
        in >> r.GreenDarkness;
    } else if (command == "fogcall") {
        // fogcall <Function> f <float> | c <r g b a> | b <0|1> | k <b g r a>: calls an engine function on every fog object
        std::string name, kind;
        in >> name >> kind;
        ue::FunctionRef ref { "ExponentialHeightFogComponent", name.c_str() };
        struct Params {
            float Values[4] = {};
        } params;
        if (kind == "f") {
            in >> params.Values[0];
        } else if (kind == "c") {
            in >> params.Values[0] >> params.Values[1] >> params.Values[2] >> params.Values[3];
        } else if (kind == "b") {
            in >> n;
            *reinterpret_cast<bool*>(params.Values) = n != 0;
        } else if (kind == "k") {
            int bytes[4] = {};
            in >> bytes[0] >> bytes[1] >> bytes[2] >> bytes[3];
            auto* out = reinterpret_cast<uint8_t*>(params.Values);
            for (int i = 0; i < 4; ++i)
                out[i] = static_cast<uint8_t>(bytes[i]);
        }
        void* cls = ue::FindClass("ExponentialHeightFogComponent");
        for (void* fog : ue::AllObjectsOf(cls))
            ue::Call(fog, ref, &params);
        mod::Log("[test] fogcall %s done", name.c_str());
    } else if (command == "cvar") {
        std::string name;
        in >> name;
        mod::Log("[test] %s = %d", name.c_str(), ue::CvarInt(std::wstring(name.begin(), name.end())));
    } else if (command == "depthrange") {
        in >> e.DepthDensity;
    } else if (command == "rec") {
        std::string what;
        in >> what;
        if (what == "start")
            recorder::RequestStart();
        else
            recorder::RequestStop();
    } else if (command == "recfps") {
        in >> r.Fps;
    } else if (command == "codec") {
        in >> r.CodecIndex;
    } else if (command == "stream") {
        in >> n >> m >> d;
        r.Streams[std::clamp(n, 0, recorder::MaxStreams - 1)].Enabled = m != 0;
        r.Streams[std::clamp(n, 0, recorder::MaxStreams - 1)].Look = static_cast<recorder::Look>(static_cast<int>(d));
    } else if (command == "settle") {
        in >> r.SettleFrames;
    } else if (command == "samples") {
        in >> r.MotionSamples;
    } else if (command == "shutter") {
        in >> r.ShutterDegrees;
    } else if (command == "range") {
        in >> r.RangeIn >> r.RangeOut >> n;
        r.UseRange = n != 0;
    } else if (command == "audio") {
        std::string what;
        in >> what;
        if (what == "start")
            audio::Start(r.OutputDir / "test_audio.wav");
        else
            audio::Stop();
    } else if (command == "attach") {
        camera::AttachSettings& at = camera::Attach();
        std::string bone;
        in >> at.Fighter >> n >> bone;
        at.Style = static_cast<camera::AttachStyle>(n);
        strncpy_s(at.Bone, bone == "-" ? "" : bone.c_str(), _TRUNCATE);
    } else if (command == "orbit") {
        camera::AttachSettings& at = camera::Attach();
        in >> at.OrbitDistance >> at.OrbitYaw >> at.OrbitPitch >> at.Smooth;
    } else if (command == "events") {
        in >> n;
        savetrace::TraceEvents(n != 0);
    } else if (command == "preview") {
        camera::PlayShot();
    } else if (command == "recordshot") {
        camera::PlayShot();
        recorder::RecordRange(camera::Keys().front().Time, camera::Keys().back().Time);
    } else if (command == "console") {
        effects::RunCommand(Rest(in));
    } else if (command == "dump") {
        effects::RequestDump();
    } else {
        mod::Log("[test] unknown command: %s", line.c_str());
    }
}

} // namespace

void Enable()
{
    g_enabled = true;
    mod::Log("Test command channel is on");
}

void OnGameTick()
{
    if (!g_enabled)
        return;
    static int frames = 0;
    if (++frames < 6)
        return;
    frames = 0;

    const std::filesystem::path file = mod::DataDir() / "commands.txt";
    std::error_code error;
    if (!std::filesystem::exists(file, error))
        return;
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty() && line.back() == '\r')
                line.pop_back(), lines.push_back(line);
            else if (!line.empty())
                lines.push_back(line);
    }
    std::filesystem::remove(file, error);
    if (error)
        return;
    for (const std::string& line : lines) {
        if (line.empty())
            continue;
        mod::Log("[test] %s", line.c_str());
        Run(line);
    }
}

} // namespace testcmd
