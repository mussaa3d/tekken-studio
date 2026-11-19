// The test channel, as in the Tekken 8 mod: a thread reads studio\commands.txt (one command a line), runs each, deletes the file, and
// "state" writes studio\state.txt. Only for driving the studio from scripts.
#include "studio.h"

#include "bluespark.h"
#include "camera.h"

#include <windows.h>

#include <rex/cvar.h>

#include <fstream>
#include <sstream>
#include <thread>

namespace studio {

bool Readable(uint32_t address, uint32_t size)
{
    uint8_t* base = GuestBase();
    if (!base || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION info {};
    const uint8_t* at = base + address;
    const uint8_t* end = at + size;
    while (at < end) {
        if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || info.Protect == 0)
            return false;
        at = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

namespace {

void Run(const std::string& line, std::ostringstream& out)
{
    std::istringstream in(line);
    std::string command;
    in >> command;
    if (command == "state") {
        char text[256];
        camera::View view;
        const uint32_t record = camera::Record();
        out << "studio 1 editor " << static_cast<int>(Editor()) << "\n";
        out << "camera_writes " << camera::Writes() << " mode " << static_cast<int>(camera::Config().Mode) << " keys " << camera::Keys().size() << "\n";
        snprintf(text, sizeof(text), "clock %.3f paused %d speed %.2f holding %d canhold %d replay %d replay_frame %llu bluespark %d\n", Clock(), Time().Paused ? 1 : 0,
                 Time().Speed, Holding() ? 1 : 0, CanHold() ? 1 : 0, ReplayPlaying() ? 1 : 0, static_cast<unsigned long long>(Ttt2ReplayFrameNow()), Ttt2BlueSparkOn() ? 1 : 0);
        out << text;
        snprintf(text, sizeof(text), "counters engine %llu fightsteps %llu testframe %llu inplay %d playingnow %d\n", static_cast<unsigned long long>(Ttt2EngineFrameNow()),
                 static_cast<unsigned long long>(Ttt2TestFightSteps()), static_cast<unsigned long long>(Ttt2TestFrame()), record && Ttt2FightInPlay() ? 1 : 0, record && Ttt2FightPlayingNow() ? 1 : 0);
        out << text;
        out << "skip" << SkipCounts() << "\n";
        out << "recorder " << (RecorderActive() ? 1 : 0) << " frames " << RecorderFrames() << " " << RecorderMessage() << "\n";
        {
            uint64_t a = 0, b = 0;
            Ttt2ReplayForwardFrames(&a, &b);
            out << "forward " << a << " " << b << "\n";
        }
        out << "replay_holding " << (Ttt2ReplayHolding() ? 1 : 0) << " hold_holding " << (GuestBase() && record && Ttt2TrainingHoldHolding(GuestBase()) ? 1 : 0) << "\n";
        if (record && camera::Read(view)) {
            snprintf(text, sizeof(text), "camera %08X eye %.1f %.1f %.1f target %.1f %.1f %.1f fov %.2f\n", record, view.Eye.X, view.Eye.Y, view.Eye.Z, view.Target.X,
                     view.Target.Y, view.Target.Z, view.Fov);
            out << text;
        } else {
            out << "camera none\n";
        }
    } else if (command == "peek") {
        // peek <hex address> <count>: 32-bit words, as hex and as floats
        uint32_t address = 0, count = 16;
        in >> std::hex >> address >> std::dec >> count;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t at = address + i * 4;
            if (!Readable(at, 4)) {
                out << std::hex << at << " unreadable\n";
                break;
            }
            char text[96];
            snprintf(text, sizeof(text), "%08X %08X %g\n", at, ReadU32(at), ReadFloat(at));
            out << text;
        }
    } else if (command == "pokef") {
        uint32_t address = 0;
        float value = 0;
        in >> std::hex >> address >> std::dec >> value;
        if (Readable(address, 4))
            WriteFloat(address, value);
    } else if (command == "editor") {
        // editor <0 hidden|1 simple|2 studio>
        int mode = 0;
        in >> mode;
        Editor() = static_cast<EditorMode>(mode);
    } else if (command == "holdraw") {
        // holdraw <practice|offline> <0|1>: BlueSpark's hold asked for directly (research)
        std::string kind;
        int on = 0;
        in >> kind >> on;
        if (kind == "practice")
            Ttt2TrainingHoldRequest("tekken studio test", on != 0);
        else
            Ttt2TrainingHoldRequestOffline("tekken studio test", on != 0);
        out << "asked\n";
    } else if (command == "flag") {
        // flag <name> <value>: one of the game's own switches, set while it runs
        std::string name, value;
        in >> name >> value;
        out << "flag " << name << (rex::cvar::SetFlagByName(name, value) ? " set " : " refused ") << rex::cvar::GetFlagByName(name) << "\n";
    } else if (command == "skip") {
        // skip <which 0-5, -1 none> <0|1>: research, one of the game's frame functions left out while 1
        int which = -1, hold = 0;
        in >> which >> hold;
        SkipTest(which, hold != 0);
    } else if (command == "rec") {
        // rec <start|stop>
        std::string what;
        in >> what;
        if (what == "start")
            RecorderStart();
        else
            RecorderStop();
    } else if (command == "imgui") {
        DumpFrameOnce();
    } else if (command == "stack") {
        camera::LogStackOnce();
    } else if (command == "markers") {
        int on = 0;
        in >> on;
        Markers() = on != 0;
    } else if (command == "mode") {
        // mode <game|free|dolly>
        std::string name;
        in >> name;
        camera::SetMode(name == "free" ? camera::Mode::Free : name == "dolly" ? camera::Mode::Dolly : camera::Mode::Game);
    } else if (command == "pose") {
        // pose x y z yaw pitch fov: the free camera there
        camera::Pose& p = camera::Config().Free;
        in >> p.Eye.X >> p.Eye.Y >> p.Eye.Z >> p.Yaw >> p.Pitch >> p.Fov;
        camera::Config().Mode = camera::Mode::Free;
    } else if (command == "addkey") {
        camera::AddKeyAtView(Clock());
    } else if (command == "clearkeys") {
        camera::Keys().clear();
    } else if (command == "pause") {
        int on = 0;
        in >> on;
        Time().Paused = on != 0;
    } else if (command == "speed") {
        in >> Time().Speed;
    } else if (command == "step") {
        StepOneFrame();
    } else if (command == "replay") {
        // replay <file name in the replays folder>: BlueSpark's own way of starting one
        std::string name;
        std::getline(in >> std::ws, name);
        const std::filesystem::path file = Ttt2ReplaysFolder() / name;
        out << "replay " << (GuestBase() && Ttt2ReplayChoose(GuestBase(), file.string()) ? "chosen" : "refused") << "\n";
    } else if (command == "replays") {
        std::error_code ignored;
        const std::filesystem::path folder = Ttt2ReplaysFolder();
        out << "folder " << folder.string() << "\n";
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ignored))
            if (entry.is_regular_file())
                out << "file " << std::filesystem::relative(entry.path(), folder, ignored).string() << " " << entry.file_size() << "\n";
    } else if (command == "watch") {
        // watch <hex address> <bytes> [r]: count the code that writes there; with r, reads too
        uint32_t address = 0, size = 4;
        std::string mode;
        in >> std::hex >> address >> std::dec >> size >> mode;
        WatchStart(address, size, mode == "r");
        out << "watching\n";
    } else if (command == "watchdump") {
        out << WatchDump();
    } else if (command == "watchstop") {
        WatchStop();
        out << WatchDump();
    } else if (!command.empty()) {
        out << "unknown " << command << "\n";
    }
}

void Loop()
{
    const std::filesystem::path commands = DataDir() / "commands.txt", state = DataDir() / "state.txt";
    // A command left from a run before would be run before the game has set itself up.
    {
        std::error_code ignored;
        std::filesystem::remove(commands, ignored);
    }
    for (;;) {
        Sleep(30);
        std::ifstream file(commands);
        if (!file)
            continue;
        std::ostringstream out;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            Run(line, out);
        }
        file.close();
        std::error_code ignored;
        std::filesystem::remove(commands, ignored);
        if (!out.str().empty()) {
            std::ofstream result(state, std::ios::trunc);
            result << out.str();
        }
    }
}

} // namespace

void StartCommands()
{
    std::thread(Loop).detach();
}

} // namespace studio
