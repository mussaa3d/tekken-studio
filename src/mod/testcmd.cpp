#include "mod/testcmd.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
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
#include "mod/scene.h"
#include "mod/sharing.h"
#include "mod/switcher.h"
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
    out << "drama " << effects::DramaPlaying() << "\n";
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
    } else if (command == "fogreset") {
        dvars::Tint().FogOverridden = false;
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
    } else if (command == "slots") {
        for (const sharing::Slot& slot : sharing::Slots())
            if (slot.Downloaded)
                mod::Log("[test] slot %d version %u downloaded", slot.Index, slot.GameVersion);
    } else if (command == "pokeu32") {
        // pokeu32 <hex address> <value>
        std::string address;
        in >> address >> n;
        *reinterpret_cast<int*>(std::stoull(address, nullptr, 16)) = n;
        mod::Log("[test] poked %s = %d", address.c_str(), n);
    } else if (command == "exportslot") {
        std::string name, error;
        in >> n >> name;
        mod::Log("[test] export slot %d: %d %s", n, sharing::Export(n, name, error), error.c_str());
    } else if (command == "importfile") {
        std::string name, error;
        in >> name;
        mod::Log("[test] import %s: %d %s", name.c_str(), sharing::Import(name, error), error.c_str());
    } else if (command == "lightall") {
        // lightall <Class> <intensity> <r> <g> <b>: sets every light of a class, to see whether that kind shows at all
        std::string name;
        float intensity = 0, red = 1, green = 1, blue = 1;
        in >> name >> intensity >> red >> green >> blue;
        void* cls = ue::FindClass(name);
        static ue::FunctionRef setIntensity { "LightComponent", "SetIntensity" };
        static ue::FunctionRef setColor { "LightComponent", "SetLightColor" };
        int count = 0;
        for (void* light : ue::AllObjectsOf(cls)) {
            struct {
                float Value;
            } value { intensity };
            ue::Call(light, setIntensity, &value);
            struct {
                float R, G, B, A;
                bool SRGB;
            } color { red, green, blue, 1, true };
            ue::Call(light, setColor, &color);
            ++count;
        }
        mod::Log("[test] lightall %s: %d lights", name.c_str(), count);
    } else if (command == "memdump") {
        // memdump <hex address> <bytes> <file name>: writes part of the game's memory to a file next to the mod's log
        std::string address, name;
        size_t size = 0;
        in >> address >> size >> name;
        const auto* begin = reinterpret_cast<const uint8_t*>(std::stoull(address, nullptr, 16));
        std::ofstream file(mod::DataDir() / name, std::ios::binary | std::ios::trunc);
        size_t written = 0;
        MEMORY_BASIC_INFORMATION info {};
        while (written < size && VirtualQuery(begin + written, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const size_t chunk = std::min(size - written, static_cast<size_t>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - (begin + written)));
            file.write(reinterpret_cast<const char*>(begin + written), static_cast<std::streamsize>(chunk));
            written += chunk;
        }
        mod::Log("[test] memdump %s: wrote %zu of %zu bytes", name.c_str(), written, size);
    } else if (command == "memfind") {
        // memfind <hex>: where a byte string is in the game's writable memory, with the size of the block around it
        std::string hex;
        in >> hex;
        std::vector<uint8_t> needle;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            needle.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        MEMORY_BASIC_INFORMATION info {};
        int hits = 0;
        for (uint8_t* address = nullptr; VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && hits < 40; address = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize) {
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) == 0 || (info.Protect & PAGE_GUARD) || info.RegionSize > (1ull << 32))
                continue;
            const auto* begin = static_cast<const uint8_t*>(info.BaseAddress);
            const auto* end = begin + info.RegionSize;
            for (const uint8_t* hit = std::search(begin, end, needle.begin(), needle.end()); hit != end && hits < 40; hit = std::search(hit + 1, end, needle.begin(), needle.end())) {
                ++hits;
                mod::Log("[test] memfind %p region %p size %zu offset %zu", hit, begin, static_cast<size_t>(info.RegionSize), static_cast<size_t>(hit - begin));
            }
        }
        mod::Log("[test] memfind: %d hits", hits);
    } else if (command == "lightinfo") {
        void* cls = ue::FindClass("LightComponent");
        void* scene = ue::FindClass("SceneComponent");
        int count = 0;
        for (void* light : ue::AllObjectsOf(cls)) {
            const std::string name = ue::NameOf(light);
            ++count;
            struct {
                void* ReturnValue;
            } owner { nullptr };
            static ue::FunctionRef getOwner { "ActorComponent", "GetOwner" };
            ue::Call(light, getOwner, &owner);
            const std::string ownerClass = owner.ReturnValue ? ue::ClassNameOf(owner.ReturnValue) : "";
            int ownerHiddenByte = -1;
            uint8_t ownerHiddenMask = 0;
            ue::BoolProperty(ue::FindClass("Actor"), "bHidden", ownerHiddenByte, ownerHiddenMask);
            if (ownerClass != "PointLight" && ownerClass != "SpotLight" && count > 3)
                continue;
            mod::Log("[test] owner %s hidden %d", ownerClass.c_str(), owner.ReturnValue && ownerHiddenByte >= 0 ? (ue::Field<uint8_t>(owner.ReturnValue, ownerHiddenByte) & ownerHiddenMask) != 0 : -1);
            struct {
                cine::Vec3 ReturnValue;
            } where {};
            static ue::FunctionRef location { "SceneComponent", "K2_GetComponentLocation" };
            ue::Call(light, location, &where);
            int visibleByte = -1, affectsByte = -1, hiddenByte = -1, movableAt = ue::PropertyOffset(scene, "Mobility");
            uint8_t visibleMask = 0, affectsMask = 0, hiddenMask = 0;
            ue::BoolProperty(scene, "bVisible", visibleByte, visibleMask);
            ue::BoolProperty(scene, "bHiddenInGame", hiddenByte, hiddenMask);
            ue::BoolProperty(ue::FindClass("LightComponentBase"), "bAffectsWorld", affectsByte, affectsMask);
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
        in >> r.DepthNear >> r.DepthFar >> r.DepthCurve;
    } else if (command == "round") {
        in >> n;
        for (const replay::SavedPoint& point : replay::SavedPoints())
            if (point.Type == replay::TypeRoundStart && point.Round == n)
                game::SeekToRound(point.Frame);
    } else if (command == "classinfo") {
        // classinfo <Class>: logs the functions and properties a class adds, and those of its parents up to Actor components
        std::string name;
        in >> name;
        for (const void* cls = ue::FindClass(name); cls; cls = ue::Field<void*>(cls, ue::off::SuperStruct)) {
            const std::string level = ue::NameOf(cls);
            if (level == "ActorComponent" || level == "Actor" || level == "Object" || level == "SceneComponent")
                break;
            mod::Log("[class] %s", level.c_str());
            for (void* field = ue::Field<void*>(cls, ue::off::Children); field; field = ue::Field<void*>(field, ue::off::FieldNext))
                mod::Log("[class]   fn %s", ue::NameOf(field).c_str());
            for (const std::string& property : ue::PropertyNames(cls))
                mod::Log("[class]   prop %s", property.c_str());
        }
    } else if (command == "cdoprop") {
        // cdoprop <Class> <Property> [value]: reads, and can set, a float on a class's defaults and all its objects
        std::string name, property;
        double value = 0;
        in >> name >> property;
        const bool write = static_cast<bool>(in >> value);
        void* cls = ue::FindClass(name);
        const int offset = cls ? ue::PropertyOffset(cls, property) : -1;
        if (offset >= 0) {
            void* defaults = ue::Field<void*>(cls, ue::off::ClassDefaultObject);
            mod::Log("[cdo] %s.%s default = %g", name.c_str(), property.c_str(), defaults ? ue::Field<float>(defaults, offset) : -1.0f);
            std::vector<void*> objects = ue::AllObjectsOf(cls);
            if (defaults)
                objects.push_back(defaults);
            for (void* object : objects) {
                if (write)
                    ue::Field<float>(object, offset) = static_cast<float>(value);
            }
            mod::Log("[cdo] %d objects", static_cast<int>(objects.size()));
        } else {
            mod::Log("[cdo] no %s.%s", name.c_str(), property.c_str());
        }
    } else if (command == "bonesall") {
        in >> n;
        game::Time().BlendAllMeshes = n != 0;
    } else if (command == "lookinput") {
        in >> n;
        camera::Config().LookInput = static_cast<camera::LookInput>(n);
    } else if (command == "clothfollow") {
        in >> n;
        game::Time().ClothFollowsFight = n != 0;
    } else if (command == "clothmin") {
        in >> game::Time().ClothMinSolve;
    } else if (command == "exactframes") {
        in >> n;
        game::Time().ExactFrames = n != 0;
    } else if (command == "switch") {
        in >> n;
        std::string error;
        if (!switcher::Start(n, error))
            mod::Log("[test] switch: %s", error.c_str());
    } else if (command == "clockslow") {
        in >> n;
        game::Time().SlowWorldClock = n != 0;
    } else if (command == "keycolour") {
        in >> n;
        r.BlueKey = n != 0;
    } else if (command == "pathstyle") {
        in >> n;
        camera::Config().PathStyle = static_cast<cine::PathStyle>(std::clamp(n, 0, 2));
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
    } else if (command == "pov") {
        // pov <hide 0|1> <level 0|1> <offset x y z> <turn pitch yaw roll>
        camera::AttachSettings& at = camera::Attach();
        int hide = 1, level = 0;
        in >> hide >> level >> at.PovOffset[0] >> at.PovOffset[1] >> at.PovOffset[2] >> at.PovTurn[0] >> at.PovTurn[1] >> at.PovTurn[2];
        at.PovHideHead = hide != 0;
        at.PovLevel = level != 0;
    } else if (command == "bones") {
        in >> n;
        std::string line2;
        for (const std::string& bone : camera::Bones(n))
            if (bone.find("ead") != std::string::npos || bone.find("eye") != std::string::npos || bone.find("Eye") != std::string::npos || bone.find("eck") != std::string::npos)
                line2 += bone + " ";
        mod::Log("[bones] %s", line2.c_str());
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
    } else if (command == "callall") {
        // callall <Class> <Function> n | b <0|1> | f <float> | c <r g b a> | l <object name substring> <0|1>: calls a function on every object of a class
        std::string name, function, kind;
        in >> name >> function >> kind;
        ue::FunctionRef ref { name.c_str(), function.c_str() };
        float values[4] = {};
        if (kind == "b") {
            in >> n;
            *reinterpret_cast<bool*>(values) = n != 0;
        } else if (kind == "i") {
            in >> n;
            *reinterpret_cast<int*>(values) = n;
        } else if (kind == "f") {
            in >> values[0];
        } else if (kind == "c") {
            in >> values[0] >> values[1] >> values[2] >> values[3];
        }
        int count = 0;
        for (void* object : ue::AllObjectsOf(ue::FindClass(name))) {
            if (ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            float params[16] = {};
            std::memcpy(params, values, sizeof(values));
            if (ue::Call(object, ref, params))
                ++count;
        }
        mod::Log("[test] called %s.%s on %d objects", name.c_str(), function.c_str(), count);
    } else if (command == "widgets") {
        // The game's widgets on screen with Replay, Dialog, Menu, Result or Select in their class name.
        static ue::FunctionRef visible { "Widget", "IsVisible" };
        std::map<std::string, int> found;
        for (void* widget : ue::AllObjectsOf(ue::FindClass("PolarisUserWidget"))) {
            if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            struct {
                bool ReturnValue;
            } params { false };
            ue::Call(widget, visible, &params);
            const std::string name = ue::ClassNameOf(widget);
            if (params.ReturnValue && (name.find("Replay") != std::string::npos || name.find("Dialog") != std::string::npos || name.find("Menu") != std::string::npos || name.find("Result") != std::string::npos || name.find("Select") != std::string::npos))
                ++found[name];
        }
        for (const auto& [name, count] : found)
            mod::Log("[widgets] %d x %s", count, name.c_str());
        mod::Log("[widgets] end");
    } else if (command == "lc") {
        // lc <property> [r g b a]: reads, or writes, a colour on every character light controller
        std::string property;
        in >> property;
        float value[4];
        const bool write = static_cast<bool>(in >> value[0] >> value[1] >> value[2] >> value[3]);
        void* cls = ue::FindClass("BP_CharacterLightController_C");
        const int at = cls ? ue::PropertyOffset(cls, property) : -1;
        for (void* controller : cls && at >= 0 ? ue::AllObjectsOf(cls) : std::vector<void*>()) {
            if (ue::Field<uint32_t>(controller, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            float* colour = &ue::Field<float>(controller, at);
            if (write)
                std::copy(value, value + 4, colour);
            mod::Log("[lc] %p %s = %g %g %g %g", controller, property.c_str(), colour[0], colour[1], colour[2], colour[3]);
        }
    } else if (command == "scene") {
        // scene <charlights|stagelights|tint|plain|shake|frame|aspect> <value>
        std::string what;
        in >> what >> a;
        scene::Settings& sc = scene::Config();
        if (what == "charlights")
            sc.NoCharacterLights = a != 0;
        else if (what == "stagelights")
            sc.NoStageLights = a != 0;
        else if (what == "tint")
            sc.TintCharacterLights = a != 0;
        else if (what == "plain")
            sc.PlainStage = a != 0;
        else if (what == "shake")
            sc.NoCameraShake = a != 0;
        else if (what == "frame")
            sc.Frame = static_cast<scene::Frame>(static_cast<int>(a));
        else if (what == "aspect")
            sc.AspectRatio = static_cast<float>(a);
    } else if (command == "hudlist") {
        effects::LogVisibleWidgets();
    } else if (command == "hitlabels") {
        // hitlabels <0|1> [class words, comma separated]
        in >> n;
        e.HideHitLabels = n != 0;
        const std::string words = Rest(in);
        if (!words.empty())
            strncpy_s(e.HitLabelWidgets, words.c_str(), _TRUNCATE);
    } else if (command == "census") {
        // What's on this stage that moves or tints the picture, grouped by who owns it.
        static ue::FunctionRef getOwner { "ActorComponent", "GetOwner" };
        const auto ownerOf = [](void* component) {
            struct {
                void* ReturnValue;
            } params { nullptr };
            ue::Call(component, getOwner, &params);
            return params.ReturnValue;
        };
        for (const char* className : { "SkeletalMeshComponent", "NiagaraComponent", "ParticleSystemComponent", "ExponentialHeightFogComponent", "SkyAtmosphereComponent", "SkyLightComponent",
                                       "VolumetricCloudComponent", "LevelSequencePlayer", "MediaPlayer", "ChaosClothAsset", "ClothingSimulationInteractor", "AnimInstance" }) {
            void* cls = ue::FindClass(className);
            if (!cls) {
                mod::Log("[census] %s: no such class", className);
                continue;
            }
            const auto objects = ue::AllObjectsOf(cls);
            mod::Log("[census] %s: %zu", className, objects.size());
            std::map<std::string, int> groups;
            const bool component = ue::IsChildOf(cls, ue::FindClass("ActorComponent"));
            const int scaleAt = ue::PropertyOffset(cls, "GlobalAnimRateScale");
            const int clothAt = ue::PropertyOffset(cls, "ClothingInteractor");
            for (void* object : objects) {
                if (ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                    continue;
                void* owner = component ? ownerOf(object) : nullptr;
                std::string key = ue::ClassNameOf(object);
                if (owner) {
                    key += " on " + ue::ClassNameOf(owner) + " custom " + std::to_string(ue::Field<float>(owner, ue::PropertyOffset(ue::FindClass("Actor"), "CustomTimeDilation")));
                    if (owner == game::Fighter(0) || owner == game::Fighter(1))
                        key += " FIGHTER";
                }
                if (scaleAt >= 0)
                    key += " rate " + std::to_string(ue::Field<float>(object, scaleAt));
                if (clothAt >= 0)
                    key += ue::Field<void*>(object, clothAt) ? " CLOTH" : "";
                ++groups[key];
            }
            for (const auto& [key, count] : groups)
                mod::Log("[census]   %d x %s", count, key.c_str());
        }
        void* fogClass = ue::FindClass("ExponentialHeightFogComponent");
        for (const std::string& property : ue::PropertyNames(fogClass))
            mod::Log("[census] fog property %s", property.c_str());
        void* meshClass = ue::FindClass("SkeletalMeshComponent");
        for (const char* function : { "SuspendClothingSimulation", "ResumeClothingSimulation", "IsClothingSimulationSuspended", "ResetClothTeleportMode", "ForceClothNextUpdateTeleport" })
            mod::Log("[census] mesh function %s: %p", function, ue::FindFunction(meshClass, function));
        for (const char* function : { "SetFogInscatteringColor", "SetDirectionalInscatteringColor", "SetInscatteringColorCubemap", "SetVolumetricFogDistance", "SetFogDensity" })
            mod::Log("[census] fog function %s: %p", function, ue::FindFunction(fogClass, function));
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
