// Test helper: prints each audio session's process and peak level, to see whether a game is making sound. With a process ID and a
// volume, sets that process's slider in the Volume Mixer first.
// Build: cl /EHsc /O1 tools\audio_peek.cpp ole32.lib /Fe:build\audio_peek.exe
#include <windows.h>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    const DWORD setPid = argc > 2 ? static_cast<DWORD>(atoi(argv[1])) : 0;
    const float setVolume = argc > 2 ? static_cast<float>(atof(argv[2])) : 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    for (int flow = 0; flow < 1; ++flow) {
        IMMDeviceCollection* devices = nullptr;
        enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT d = 0; d < count; ++d) {
            IMMDevice* device = nullptr;
            devices->Item(d, &device);
            IAudioSessionManager2* manager = nullptr;
            device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager));
            IAudioEndpointVolume* volume = nullptr;
            device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&volume));
            float master = 0;
            BOOL muted = FALSE;
            volume->GetMasterVolumeLevelScalar(&master);
            volume->GetMute(&muted);
            printf("device %u: volume %.2f muted %d\n", d, master, muted);
            IAudioSessionEnumerator* sessions = nullptr;
            manager->GetSessionEnumerator(&sessions);
            int sessionCount = 0;
            sessions->GetCount(&sessionCount);
            for (int i = 0; i < sessionCount; ++i) {
                IAudioSessionControl* control = nullptr;
                sessions->GetSession(i, &control);
                IAudioSessionControl2* control2 = nullptr;
                control->QueryInterface(IID_PPV_ARGS(&control2));
                IAudioMeterInformation* meter = nullptr;
                control->QueryInterface(IID_PPV_ARGS(&meter));
                ISimpleAudioVolume* simple = nullptr;
                control->QueryInterface(IID_PPV_ARGS(&simple));
                DWORD pid = 0;
                control2->GetProcessId(&pid);
                if (setPid != 0) {
                    if (pid == setPid) {
                        simple->SetMasterVolume(setVolume, nullptr);
                        printf("set pid %lu to %.3f\n", pid, setVolume);
                    }
                    continue;
                }
                AudioSessionState state = AudioSessionStateInactive;
                control->GetState(&state);
                float peak = 0, sessionVolume = 0;
                for (int t = 0; t < 30; ++t) {
                    float now = 0;
                    meter->GetPeakValue(&now);
                    peak = peak > now ? peak : now;
                    Sleep(50);
                }
                BOOL sessionMuted = FALSE;
                simple->GetMasterVolume(&sessionVolume);
                simple->GetMute(&sessionMuted);
                printf("  pid %lu state %d peak %.4f volume %.2f muted %d\n", pid, static_cast<int>(state), peak, sessionVolume, sessionMuted);
            }
        }
    }
    return 0;
}
