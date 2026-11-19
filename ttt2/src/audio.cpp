// The game's own sound, captured to a WAV file while a recording with sound runs: Windows' process loopback, so nothing of other
// programs is in it. Windows 10 2004 or newer. (The Tekken 8 mod's audio pass, without its timeline.)
// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "studio.h"

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

namespace studio::audio {

namespace {

constexpr int SampleRate = 48000;
constexpr int Channels = 2;

std::atomic<bool> g_running { false };
std::atomic<bool> g_stop { false };
std::atomic<uint64_t> g_samples { 0 };
std::atomic<bool> g_heard { false };
std::mutex g_textMutex;
std::string g_lastFile, g_lastError;

void SetError(const std::string& text)
{
    Log("Audio: %s", text.c_str());
    std::lock_guard lock(g_textMutex);
    g_lastError = text;
}

// Receives the client that ActivateAudioInterfaceAsync produces.
class Activation : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IAudioClient* Client = nullptr;
    HRESULT Result = E_FAIL;

    ~Activation() { CloseHandle(Done); }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_references; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --m_references;
        if (remaining == 0)
            delete this;
        return remaining;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        IUnknown* unknown = nullptr;
        HRESULT activateResult = E_FAIL;
        Result = operation->GetActivateResult(&activateResult, &unknown);
        if (SUCCEEDED(Result))
            Result = activateResult;
        if (SUCCEEDED(Result) && unknown)
            Result = unknown->QueryInterface(IID_PPV_ARGS(&Client));
        if (unknown)
            unknown->Release();
        SetEvent(Done);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_references { 1 };
};

void WriteWavHeader(std::ofstream& file, uint64_t samples)
{
    const uint32_t dataBytes = static_cast<uint32_t>(samples * Channels * 2);
    const uint32_t riffBytes = 36 + dataBytes;
    const uint16_t format = 1, channels = Channels, align = Channels * 2, bits = 16;
    const uint32_t rate = SampleRate, byteRate = SampleRate * Channels * 2, chunk = 16;
    file.seekp(0);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffBytes), 4);
    file.write("WAVEfmt ", 8);
    file.write(reinterpret_cast<const char*>(&chunk), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&align), 2);
    file.write(reinterpret_cast<const char*>(&bits), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataBytes), 4);
}

void Capture(std::filesystem::path path)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Process loopback: this game's sound only, so other apps don't end up in the track.
    AUDIOCLIENT_ACTIVATION_PARAMS activation = {};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    activation.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    PROPVARIANT variant = {};
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(activation);
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    auto* handler = new Activation();
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant, handler, &operation);
    IAudioClient* client = nullptr;
    if (SUCCEEDED(result)) {
        WaitForSingleObject(handler->Done, 5000);
        result = handler->Result;
        client = handler->Client;
    }
    if (operation)
        operation->Release();
    handler->Release();

    IAudioCaptureClient* capture = nullptr;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(result) && client) {
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = Channels;
        format.nSamplesPerSec = SampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = Channels * 2;
        format.nAvgBytesPerSec = SampleRate * Channels * 2;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, &format, nullptr);
        if (SUCCEEDED(result))
            result = client->SetEventHandle(ready);
        if (SUCCEEDED(result))
            result = client->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(result))
            result = client->Start();
    }
    if (FAILED(result) || !capture) {
        char text[96];
        snprintf(text, sizeof(text), "couldn't capture the game's sound (0x%08lX). This needs Windows 10 2004 or newer.", static_cast<unsigned long>(result));
        SetError(text);
        if (client)
            client->Release();
        CloseHandle(ready);
        CoUninitialize();
        g_running = false;
        return;
    }

    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    WriteWavHeader(file, 0);
    uint64_t samples = 0;
    const std::vector<int16_t> silence(SampleRate, 0);

    while (!g_stop) {
        WaitForSingleObject(ready, 100);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                for (UINT32 left = frames; left > 0;) {
                    const UINT32 chunk = std::min<UINT32>(left, static_cast<UINT32>(silence.size()) / Channels);
                    file.write(reinterpret_cast<const char*>(silence.data()), chunk * Channels * 2);
                    left -= chunk;
                }
            } else {
                file.write(reinterpret_cast<const char*>(data), frames * Channels * 2);
                const auto* pcm = reinterpret_cast<const int16_t*>(data);
                for (UINT32 i = 0; i < frames * Channels && !g_heard; ++i)
                    if (pcm[i] > 64 || pcm[i] < -64)
                        g_heard = true;
            }
            samples += frames;
            g_samples = samples;
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    WriteWavHeader(file, samples);
    file.close();
    capture->Release();
    client->Release();
    CloseHandle(ready);
    CoUninitialize();
    Log("Audio: wrote %.2f s to %s", samples / static_cast<double>(SampleRate), path.string().c_str());
    g_running = false;
}

} // namespace

bool Start(const std::filesystem::path& file)
{
    if (g_running)
        return false;
    g_stop = false;
    g_samples = 0;
    g_heard = false;
    g_running = true;
    {
        std::lock_guard lock(g_textMutex);
        g_lastFile = file.string();
        g_lastError.clear();
    }
    std::thread(Capture, file).detach();
    return true;
}

void Stop()
{
    g_stop = true;
}

bool Running()
{
    return g_running;
}

std::string LastError()
{
    std::lock_guard lock(g_textMutex);
    return g_lastError;
}

} // namespace studio::audio
