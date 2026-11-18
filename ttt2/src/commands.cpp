// The test channel, as in the Tekken 8 mod: a thread reads studio\commands.txt (one command a line), runs each, deletes the file, and
// "state" writes studio\state.txt. Only for driving the studio from scripts.
#include "studio.h"

#include "camera.h"

#include <windows.h>

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
        out << "studio 1\n";
        camera::View view;
        const uint32_t record = camera::Record();
        char text[200];
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
    } else if (command == "poke") {
        uint32_t address = 0, value = 0;
        in >> std::hex >> address >> value;
        if (Readable(address, 4))
            WriteU32(address, value);
    } else if (command == "cam") {
        // cam <0|1> [eye x y z, target x y z, fov]: the studio's camera on or off, and where it is
        int on = 0;
        in >> on;
        camera::View& v = camera::Studio();
        if (!(in >> v.Eye.X >> v.Eye.Y >> v.Eye.Z >> v.Target.X >> v.Target.Y >> v.Target.Z >> v.Fov) && on)
            camera::Read(v); // no numbers: start from the game's own view
        camera::Override() = on != 0;
    } else if (command == "watch") {
        // watch <hex address> <bytes>: count the code that writes there
        // watch <hex address> <bytes> [r]: with r, reads are counted too
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
