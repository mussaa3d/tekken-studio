#include "mod/testcmd.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mod/actors.h"
#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/gameui.h"
#include "mod/hudframe.h"
#include "mod/guard.h"
#include "mod/lights.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/postfx.h"
#include "mod/presets.h"
#include "mod/recorder.h"
#include "mod/replay.h"
#include "mod/replaydisplay.h"
#include "mod/reshadelink.h"
#include "mod/savetrace.h"
#include "mod/scan.h"
#include "mod/scene.h"
#include "mod/sharing.h"
#include "mod/sky.h"
#include "mod/spawner.h"
#include "mod/switcher.h"
#include "mod/ue.h"
#include "mod/window.h"

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
    out << "input keyboard_taken " << overlay::KeyboardTakenByUi() << " foreground " << overlay::IsGameForeground() << " over_view "
        << overlay::MouseOverGameView() << " w " << overlay::KeyDown('W') << "\n";
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
    out << "sky " << sky::Config().SunYaw << " " << sky::Config().SunHeight << " " << sky::Config().SkyRotation << " hdri " << sky::Config().HdriOn << " " << sky::Config().HdriBrightness << " status " << sky::Status() << "\n";
    out << "lookkeys " << presets::LookKeyTimes().size() << "\n";
    int pictureWidth = 0, pictureHeight = 0, videoWidth = 0, videoHeight = 0;
    window::PictureSize(pictureWidth, pictureHeight);
    recorder::VideoSize(pictureWidth, pictureHeight, videoWidth, videoHeight);
    out << "picture " << pictureWidth << " " << pictureHeight << " video " << videoWidth << " " << videoHeight << "\n";
    {
        float fovIn = 0, fovOut = 0;
        int constraint = -1;
        game::LastProjection(fovIn, fovOut, constraint);
        out << "projection " << fovIn << " " << fovOut << " constraint " << constraint << "\n";
    }
    out << "take recording " << actors::RecordingTake() << " takes " << actors::Takes().size() << " status " << actors::TakeStatus() << "\n";
    for (int i = 0; i < actors::MaxActors; ++i)
        if (actors::Cast()[i].On)
            out << "actor" << i << " take " << actors::Cast()[i].Take << " start " << actors::Cast()[i].Start << " at " << actors::Cast()[i].Location[0] << " " << actors::Cast()[i].Location[1]
                << " " << actors::Cast()[i].Location[2] << " yaw " << actors::Cast()[i].Rotation[1] << " status " << actors::ActorStatus(i) << "\n";
    for (int i = 0; i < spawner::MaxProps; ++i)
        if (spawner::Props()[i].On)
            out << "prop" << i << " " << spawner::Props()[i].Asset << " at " << spawner::Props()[i].Location[0] << " " << spawner::Props()[i].Location[1] << " "
                << spawner::Props()[i].Location[2] << " status " << spawner::PropStatus(i) << "\n";
    for (int i = 0; i < spawner::MaxEffects; ++i)
        if (spawner::Effects()[i].On)
            out << "vfx" << i << " " << spawner::Effects()[i].Asset << " start " << spawner::Effects()[i].Start << " status " << spawner::EffectStatus(i) << "\n";
    out << "reshade found " << reshadelink::Found() << " owns " << reshadelink::OwnsPresent() << " techniques " << reshadelink::Techniques().size() << " values "
        << reshadelink::Values().size() << "\n";
    out << "position " << inFight << " round " << round << " frame " << frame << " canjump " << replay::CanJump() << " seeking " << game::Seeking() << "\n";
    out.close();
    std::error_code ignored;
    std::filesystem::rename(mod::DataDir() / "state.tmp", mod::DataDir() / "state.txt", ignored);
}

// Test only: logs the game's own idea of its speed whenever it changes.
int g_speedTrace = 0;

double GameSpeedValue(const char* cls, const char* function)
{
    void* owner = ue::FindClass(cls);
    void* fn = owner ? ue::FindFunction(owner, function) : nullptr;
    if (!fn)
        return -1;
    const int worldAt = ue::PropertyOffset(fn, "WorldContextObject");
    const int returnAt = ue::PropertyOffset(fn, "ReturnValue");
    if (returnAt < 0)
        return -2;
    uint8_t params[128] = {};
    if (worldAt >= 0)
        *reinterpret_cast<void**>(params + worldAt) = ue::World();
    ue::FunctionRef ref { cls, function };
    if (!ue::Call(nullptr, ref, params))
        return -3;
    return *reinterpret_cast<float*>(params + returnAt);
}

void SpeedTrace()
{
    if (!g_speedTrace)
        return;
    if (g_speedTrace == 2) {
        static uint32_t lastFrame = 0;
        int stepRound = -1;
        uint32_t stepFrame = 0;
        replay::Position(stepRound, stepFrame);
        static void* fighterClass = ue::FindClass("PolarisCharacterActor");
        static const int motionField = fighterClass ? ue::PropertyOffset(fighterClass, "PlayerMotionFrame") : -1;
        void* one = game::Fighter(0);
        void* two = game::Fighter(1);
        if (stepFrame != lastFrame && one && two && motionField >= 0) {
            cine::Vec3 p1, p2;
            game::FighterLocation(0, p1);
            game::FighterLocation(1, p2);
            mod::Log("[step] %u motion %g %g p1 %.2f %.2f %.2f p2 %.2f %.2f %.2f dilation %g total %g", stepFrame, ue::Field<float>(one, motionField), ue::Field<float>(two, motionField), p1.X, p1.Y, p1.Z, p2.X, p2.Y, p2.Z,
                     GameSpeedValue("GameplayStatics", "GetGlobalTimeDilation"), GameSpeedValue("PolarisBlueprintLibrary", "GetGameSpeedTotal"));
            lastFrame = stepFrame;
        }
    }
    static double last[3] = { -9, -9, -9 };
    const double now[3] = { GameSpeedValue("GameplayStatics", "GetGlobalTimeDilation"), GameSpeedValue("PolarisBlueprintLibrary", "GetGameSpeedTotal"),
                            GameSpeedValue("PolarisBlueprintLibrary", "GetFrameSkip") };
    if (now[0] != last[0] || now[1] != last[1] || now[2] != last[2]) {
        int round = -1;
        uint32_t frame = 0;
        replay::Position(round, frame);
        mod::Log("[speed] frame %u: dilation %g, game speed %g, frame skip %g", frame, now[0], now[1], now[2]);
    }
    std::copy(now, now + 3, last);
}

std::string Rest(std::istringstream& in)
{
    std::string rest;
    std::getline(in, rest);
    const size_t begin = rest.find_first_not_of(' ');
    return begin == std::string::npos ? std::string() : rest.substr(begin);
}

// A parameter's type, from a line of ue::PropertyNames ("Name @0x0 IntProperty 4").
std::string ParamType(const std::string& line)
{
    std::istringstream words(line);
    std::string name, at, type;
    words >> name >> at >> type;
    return type;
}

// For finding the game's replay calls: a function's flags and parameters, a call that only takes plain numbers, and copies of replays in memory.
bool RunReplayCommand(const std::string& command, std::istringstream& in)
{
    if (command == "fnx") {
        // fnx <Class> <Function>: flags, parameter size, where its native code is, and each parameter with its type
        std::string name, function;
        in >> name >> function;
        void* fn = ue::FindFunction(ue::FindClass(name), function);
        if (!fn) {
            mod::Log("[fnx] %s.%s not found", name.c_str(), function.c_str());
            return true;
        }
        const uintptr_t native = ue::Field<uintptr_t>(fn, 0xD8);
        const scan::Module module = scan::MainModule();
        mod::Log("[fnx] %s.%s flags %08X params %u bytes, code %s+0x%llX", name.c_str(), function.c_str(), ue::Field<uint32_t>(fn, ue::off::FunctionFlags),
                 ue::Field<uint16_t>(fn, ue::off::FunctionParamsSize), native >= module.Base && native < module.Base + module.Size ? "game" : "elsewhere ",
                 static_cast<unsigned long long>(native - (native >= module.Base ? module.Base : 0)));
        for (const std::string& property : ue::PropertyNames(fn))
            mod::Log("[fnx]   %s", property.c_str());
        return true;
    }
    if (command == "callints") {
        // callints <Class> <Function> [int ...]: calls it on every live object of the class, the ints filling its parameters in order.
        // Refused unless every parameter is a plain number, since a wrong argument is a fatal error in this game.
        std::string name, function;
        in >> name >> function;
        void* cls = ue::FindClass(name);
        void* fn = ue::FindFunction(cls, function);
        if (!fn) {
            mod::Log("[callints] %s.%s not found", name.c_str(), function.c_str());
            return true;
        }
        for (const std::string& property : ue::PropertyNames(fn)) {
            const std::string type = ParamType(property);
            if (type != "IntProperty" && type != "BoolProperty" && type != "ByteProperty" && type != "EnumProperty" && type != "FloatProperty" && type != "UInt32Property") {
                mod::Log("[callints] refused: %s", property.c_str());
                return true;
            }
        }
        int32_t values[16] = {};
        int count = 0;
        while (count < 16 && (in >> values[count]))
            ++count;
        ue::FunctionRef ref { name.c_str(), function.c_str() };
        int called = 0;
        for (void* object : ue::AllObjectsOf(cls)) {
            if (ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            int32_t params[16] = {};
            std::copy(values, values + count, params);
            if (ue::Call(object, ref, params)) {
                ++called;
                mod::Log("[callints] %s on %p: after %d %d %d %d", function.c_str(), object, params[0], params[1], params[2], params[3]);
            }
        }
        mod::Log("[callints] %s.%s on %d objects", name.c_str(), function.c_str(), called);
        return true;
    }
    if (command == "uistate") {
        std::string buttons;
        for (const std::string& button : gameui::QuestionButtons())
            buttons += " [" + button + "]";
        mod::Log("[ui] list %d tab %d rows %d fills %d, details %d slot %d, question %d shown %d:%s, open slot %d", gameui::ListUp(), gameui::ListTab(),
                 gameui::ListRows(), gameui::ListFills(), gameui::DetailsUp(), gameui::DetailsSlot(), gameui::QuestionUp(), gameui::QuestionsShown(), buttons.c_str(),
                 gameui::OpenSlot());
        return true;
    }
    if (command == "uirow") {
        // uirow <slot>: the row of the game's list that shows it
        int slot = -1;
        in >> slot;
        mod::Log("[ui] slot %d is row %d", slot, gameui::RowOfSlot(slot));
        return true;
    }
    if (command == "replaycopies") {
        // Every replay record in the game's memory outside its block of slots.
        for (const sharing::Copy& copy : sharing::Copies())
            mod::Log("[copies] %p in a region of %zu MB at +0x%zX: played %llu, %s vs %s, matches slot %d", copy.Address, copy.RegionSize >> 20, copy.RegionOffset,
                     static_cast<unsigned long long>(copy.Played), copy.Player1.c_str(), copy.Player2.c_str(), copy.Slot);
        mod::Log("[copies] end");
        return true;
    }
    return false;
}

// The commands for what 2.0 added: placed lights, props, effects, actors and takes, the picture's size, and ReShade.
bool RunSceneCommand(const std::string& command, std::istringstream& in)
{
    recorder::Settings& r = recorder::Config();
    double a = 0, b = 0;
    int n = 0, m = 0;
    if (command == "lightaim") {
        // lightaim <index> <aim 0-3> <pitch> <yaw> <cone> <shadows 0|1>
        int aim = 0, shadows = 1;
        in >> n;
        lights::Light& light = lights::Rig()[std::clamp(n, 0, lights::MaxLights - 1)];
        in >> aim >> light.Direction[0] >> light.Direction[1] >> light.Cone >> shadows;
        light.Aim = static_cast<lights::Aim>(std::clamp(aim, 0, 3));
        light.Shadows = shadows != 0;
    } else if (command == "lightcam") {
        in >> n;
        lights::PlaceAtCamera(n);
    } else if (command == "assets") {
        // assets <StaticMesh|NiagaraSystem|SkeletalMesh> [part of a name]: logs what's loaded, up to 80 of them
        std::string className, part;
        in >> className >> part;
        const std::vector<std::string> names = ue::LoadedNames(className);
        int shown = 0;
        for (const std::string& name : names)
            if ((part.empty() || name.find(part) != std::string::npos) && shown++ < 80)
                mod::Log("[assets] %s", name.c_str());
        mod::Log("[assets] %zu %s loaded, %d shown", names.size(), className.c_str(), std::min(shown, 80));
    } else if (command == "assetpath") {
        // assetpath <Class> <name>: the full path of a loaded asset
        std::string className, name;
        in >> className >> name;
        void* object = ue::FindLoaded(className, name);
        mod::Log("[assets] %s %s = %p %s", className.c_str(), name.c_str(), object, ue::PathOf(object).c_str());
    } else if (command == "prop") {
        // prop <index> <on> <asset> [x y z] [pitch yaw roll] [scale]
        int on = 0;
        std::string asset;
        in >> n >> on >> asset;
        spawner::Prop& prop = spawner::Props()[std::clamp(n, 0, spawner::MaxProps - 1)];
        prop.On = on != 0;
        if (asset != "-")
            strncpy_s(prop.Asset, asset.c_str(), _TRUNCATE);
        float scale = 0;
        in >> prop.Location[0] >> prop.Location[1] >> prop.Location[2] >> prop.Rotation[0] >> prop.Rotation[1] >> prop.Rotation[2];
        if (in >> scale)
            prop.Scale[0] = prop.Scale[1] = prop.Scale[2] = scale;
    } else if (command == "propcam") {
        in >> n;
        spawner::PlacePropAtCamera(n);
    } else if (command == "vfx") {
        // vfx <index> <on> <asset> <timeline 0|1> <start> <length> <rate> [x y z] [scale]
        int on = 0, timeline = 1;
        std::string asset;
        in >> n >> on >> asset;
        spawner::Effect& effect = spawner::Effects()[std::clamp(n, 0, spawner::MaxEffects - 1)];
        effect.On = on != 0;
        if (asset != "-")
            strncpy_s(effect.Asset, asset.c_str(), _TRUNCATE);
        in >> timeline >> effect.Start >> effect.Length >> effect.Rate >> effect.Location[0] >> effect.Location[1] >> effect.Location[2] >> effect.Scale;
        effect.OnTimeline = timeline != 0;
    } else if (command == "vfxcam") {
        in >> n;
        spawner::PlaceEffectAtCamera(n);
    } else if (command == "vfxrestart") {
        in >> n;
        spawner::RestartEffect(n);
    } else if (command == "take") {
        // take <name> <side> <in> <out>
        std::string name, error;
        in >> name >> n >> a >> b;
        if (!actors::StartTake(name, n, a, b, error))
            mod::Log("[test] take: %s", error.c_str());
    } else if (command == "takecancel") {
        actors::CancelTake();
    } else if (command == "takes") {
        actors::RefreshTakes();
        for (const actors::TakeInfo& take : actors::Takes())
            mod::Log("[takes] %s: %d frames, P%d, %d meshes", take.Name.c_str(), take.Frames, take.Side + 1, take.Meshes);
        mod::Log("[takes] status: %s", actors::TakeStatus().c_str());
    } else if (command == "takedelete") {
        std::string name;
        in >> name;
        actors::DeleteTake(name);
    } else if (command == "actor") {
        // actor <index> <on> <take> [start] [speed] [end 0-2] [body -1..1]
        int on = 0;
        std::string take;
        in >> n >> on >> take;
        n = std::clamp(n, 0, actors::MaxActors - 1);
        actors::Actor& actor = actors::Cast()[n];
        if (take != "-")
            actors::SetTake(n, take);
        in >> actor.Start >> actor.Speed >> actor.End >> actor.Body;
        actor.On = on != 0;
    } else if (command == "actorplace") {
        // actorplace <index> <x> <y> <z> <yaw> [scale]
        in >> n;
        actors::Actor& actor = actors::Cast()[std::clamp(n, 0, actors::MaxActors - 1)];
        in >> actor.Location[0] >> actor.Location[1] >> actor.Location[2] >> actor.Rotation[1] >> actor.Scale;
    } else if (command == "actormove") {
        // actormove <index> <dx> <dy> <dz> <dyaw>: from where it is
        float dx = 0, dy = 0, dz = 0, dyaw = 0;
        in >> n >> dx >> dy >> dz >> dyaw;
        actors::Actor& actor = actors::Cast()[std::clamp(n, 0, actors::MaxActors - 1)];
        actor.Location[0] += dx;
        actor.Location[1] += dy;
        actor.Location[2] += dz;
        actor.Rotation[1] += dyaw;
    } else if (command == "actorcam") {
        in >> n;
        actors::PlaceAtCamera(n);
    } else if (command == "capsize") {
        in >> r.CaptureWidth >> r.CaptureHeight;
    } else if (command == "outsize") {
        in >> r.OutputWidth >> r.OutputHeight;
    } else if (command == "winsize") {
        // winsize <w> <h>: the game's picture size, with no recording. 0 0 gives the window back
        in >> n >> m;
        window::SetCaptureSize(n, m);
    } else if (command == "winmove") {
        // winmove <x> <y>: moves the game window, to put it on another monitor
        in >> n >> m;
        if (HWND window = overlay::GameWindow())
            SetWindowPos(window, nullptr, n, m, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (command == "shot") {
        // shot <file name>: the frame as the game drew it, with no editor on it, as raw pixels in the mod's folder
        std::string name;
        in >> name;
        overlay::RequestScreenshot(mod::DataDir() / name);
    } else if (command == "preset") {
        // preset save|load <name>, shotfile save|load <name>
        std::string what, name;
        in >> what >> name;
        mod::Log("[test] preset %s %s: %d", what.c_str(), name.c_str(), what == "save" ? presets::SavePreset(name) : presets::LoadPreset(name));
    } else if (command == "shotfile") {
        std::string what, name;
        in >> what >> name;
        mod::Log("[test] shot %s %s: %d", what.c_str(), name.c_str(), what == "save" ? presets::SaveShot(name) : presets::LoadShot(name));
    } else if (command == "reshade") {
        mod::Log("[reshade] %s", reshadelink::StatusText().c_str());
        for (const reshadelink::Technique& technique : reshadelink::Techniques())
            mod::Log("[reshade] technique %s / %s: %d", technique.Effect.c_str(), technique.Name.c_str(), technique.On);
        for (const reshadelink::Value& value : reshadelink::Values())
            mod::Log("[reshade] value %s / %s (%s): %g %g %g %g, %g to %g, kind %d count %d", value.Effect.c_str(), value.Name.c_str(), value.Label.c_str(), value.Numbers[0], value.Numbers[1],
                     value.Numbers[2], value.Numbers[3], value.Min, value.Max, static_cast<int>(value.Kind), value.Count);
    } else if (command == "reshadetech") {
        // reshadetech <effect file> <technique> <0|1>
        std::string effect, name;
        in >> effect >> name >> n;
        reshadelink::SetTechnique(effect, name, n != 0);
    } else if (command == "reshadevalue") {
        // reshadevalue <effect file> <value> <a> [b c d]
        std::string effect, name;
        float numbers[4] = {};
        in >> effect >> name >> numbers[0] >> numbers[1] >> numbers[2] >> numbers[3];
        reshadelink::SetValue(effect, name, numbers);
    } else {
        return false;
    }
    return true;
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

    if (RunSceneCommand(command, in) || RunReplayCommand(command, in))
        return;
    if (command == "state") {
        WriteState();
    } else if (command == "menu") {
        if ((in >> n) && static_cast<bool>(n) != overlay::MenuOpen())
            overlay::ToggleMenu();
    } else if (command == "tab") {
        in >> n;
        menu::SetTab(n);
    } else if (command == "groups") {
        in >> n;
        menu::OpenAllGroups(n != 0);
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
        // events <0|1> [words, comma separated, that pick the functions logged with their parameters]
        in >> n;
        savetrace::TraceEvents(n != 0, Rest(in));
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
        else if (what == "keep")
            sc.KeepFighters = a != 0;
        else if (what == "fit")
            sc.Fit = static_cast<float>(a);
        else if (what == "hud")
            sc.HudFrame = static_cast<int>(a);
    } else if (command == "subprop") {
        // subprop <Class> <Property> [float]: reads, and can write, a value on every object of a class
        std::string name, property;
        float value = 0;
        in >> name >> property;
        const bool write = static_cast<bool>(in >> value);
        void* cls = ue::FindClass(name);
        const int at = cls ? ue::PropertyOffset(cls, property) : -1;
        for (void* object : cls && at >= 0 ? ue::AllObjectsOf(cls) : std::vector<void*>()) {
            if (write && !(ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject))
                ue::Field<float>(object, at) = value;
            mod::Log("[prop] %s %s.%s = %g (int %d, byte %d)", ue::NameOf(object).c_str(), name.c_str(), property.c_str(), ue::Field<float>(object, at), ue::Field<int>(object, at), ue::Field<uint8_t>(object, at));
        }
    } else if (command == "fninfo") {
        // fninfo <Class> <Function>: the function's parameters
        std::string name, function;
        in >> name >> function;
        void* fn = ue::FindFunction(ue::FindClass(name), function);
        mod::Log("[fn] %s.%s %p", name.c_str(), function.c_str(), fn);
        if (fn)
            for (const std::string& property : ue::PropertyNames(fn))
                mod::Log("[fn]   %s", property.c_str());
    } else if (command == "sky") {
        // sky <sun yaw> <sun height> <sky rotation>
        sky::Settings& k = sky::Config();
        in >> k.SunYaw >> k.SunHeight >> k.SkyRotation;
    } else if (command == "hdri") {
        // hdri <0|1> [file] / hdribright <x> / hdrilight <0|1>
        sky::Settings& k = sky::Config();
        in >> n;
        k.HdriOn = n != 0;
        const std::string file = Rest(in);
        if (!file.empty())
            strncpy_s(k.HdriFile, file.c_str(), _TRUNCATE);
    } else if (command == "hdribright") {
        in >> sky::Config().HdriBrightness;
    } else if (command == "hdrilight") {
        in >> n;
        sky::Config().HdriLightsStage = n != 0;
    } else if (command == "hidefighter") {
        // hidefighter <side> <0|1>
        in >> n >> m;
        sky::Config().HideFighter[std::clamp(n, 0, 1)] = m != 0;
    } else if (command == "lookkey") {
        presets::AddLookKey();
        mod::Log("[test] look keys: %zu", presets::LookKeyTimes().size());
    } else if (command == "lookclear") {
        presets::ClearLookKeys();
    } else if (command == "speedtrace") {
        in >> g_speedTrace;
    } else if (command == "keepstatus") {
        in >> n;
        e.KeepBattleStatus = n != 0;
    } else if (command == "statusitem") {
        // statusitem <item> <0|1>
        in >> n >> m;
        replaydisplay::StatusShown(std::clamp(n, 0, replaydisplay::StatusItems - 1)) = m != 0;
        replaydisplay::Changed();
    } else if (command == "display") {
        // display <row> <-1|0|1>
        in >> n >> m;
        replaydisplay::Choice(std::clamp(n, 0, replaydisplay::Rows - 1)) = m;
        replaydisplay::Changed();
    } else if (command == "slowmo") {
        in >> n;
        t.NoGameSlowMotion = n != 0;
    } else if (command == "reset") {
        presets::ResetAll();
    } else if (command == "undo") {
        mod::Log("[test] undo: %d", presets::Undo() ? 1 : 0);
    } else if (command == "fogcam") {
        in >> n;
        dvars::Tint().FogFromCamera = n != 0;
    } else if (command == "thirds") {
        in >> n;
        c.RuleOfThirds = n != 0;
    } else if (command == "focusmode") {
        in >> n;
        postfx::FocusConfig().Mode = static_cast<postfx::Focus>(n);
    } else if (command == "start") {
        game::SeekToStart();
    } else if (command == "slotfind") {
        int slot = 0;
        unsigned value = 0;
        in >> slot >> value;
        sharing::FindInSlot(slot, value);
    } else if (command == "unlockold") {
        mod::Log("[test] unlocked %d older replays", sharing::UnlockOlderReplays());
    } else if (command == "hudpreset") {
        in >> n;
        effects::ApplyHudPreset(n);
    } else if (command == "gfxpreset") {
        in >> n;
        dvars::ApplyGraphicsPreset(n);
    } else if (command == "owners") {
        effects::LogVisibleOwners();
    } else if (command == "hudtree") {
        // hudtree [part of a class name]: the HUD widgets on screen, as trees
        hudframe::Dump(Rest(in));
    } else if (command == "widgetvis") {
        // widgetvis <part of a class name> <0|1>: hides or shows every widget of matching classes
        std::string part;
        in >> part >> n;
        int count = 0;
        for (void* widget : ue::AllObjectsOf(ue::FindClass("UserWidget")))
            if (!(ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject) && ue::ClassNameOf(widget).find(part) != std::string::npos) {
                ue::SetWidgetVisibility(widget, n ? ue::SlateVisibility::SelfHitTestInvisible : ue::SlateVisibility::Hidden);
                ++count;
            }
        mod::Log("[test] widgetvis %s %d: %d widgets", part.c_str(), n, count);
    } else if (command == "hudanchor") {
        // hudanchor <minX> <minY> <maxX> <maxY>: every widget on screen into that part of the window (0 0 1 1 is the whole window)
        float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        in >> x0 >> y0 >> x1 >> y1;
        hudframe::Anchor(x0, y0, x1, y1);
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
    SpeedTrace();
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
