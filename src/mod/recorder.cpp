#include "mod/recorder.h"

#include "mod/depth.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "common/framemath.h"
#include "mod/game.h"
#include "mod/log.h"

namespace recorder {

namespace {

constexpr size_t MaxQueuedFrames = 4;

Settings g_settings;
bool g_rangeFired = false;
bool g_rangeOnce = false;
int g_rangeStopWait = 0;
constexpr int PictureLag = 2;
std::atomic<bool> g_firstCaptureDone { true };
std::atomic<bool> g_startRequested { false };
std::atomic<bool> g_stopRequested { false };
std::atomic<bool> g_active { false };
std::atomic<int> g_writers { 0 };
std::atomic<uint64_t> g_frames { 0 };
std::mutex g_textMutex;
std::string g_lastFile, g_lastError;

// GPU objects for copying the back buffer into CPU-readable memory.
struct CopyPipeline {
    ID3D12Device* Device = nullptr;
    ID3D12CommandAllocator* Allocator = nullptr;
    ID3D12GraphicsCommandList* List = nullptr;
    ID3D12Fence* Fence = nullptr;
    HANDLE Event = nullptr;
    UINT64 FenceValue = 0;
    ID3D12Resource* Readback = nullptr;
    UINT64 ReadbackSize = 0;
} g_gpu;

// An ffmpeg process and the thread feeding its stdin.
struct Encoder {
    HANDLE Stdin = nullptr;
    PROCESS_INFORMATION Process {};
    std::mutex Mutex;
    std::condition_variable Changed;
    std::deque<std::vector<uint8_t>> Queue;
    std::vector<std::vector<uint8_t>> Spare;
    bool Closing = false;
    std::atomic<bool> Broken { false };
};

// One output file: its encoder and the running total of the current frame's samples.
struct Output {
    std::shared_ptr<Encoder> Encoder;
    std::vector<uint32_t> Sum;
    int Samples = 0;
};

// The passes that make up one sample, in render order.
struct PlannedPass {
    Pass Pass;
    int Output;
};

std::vector<Output> g_outputs;
std::vector<PlannedPass> g_plan;
std::atomic<int> g_passIndex { 0 };
std::atomic<Pass> g_activePass { Pass::Picture };
std::atomic<int> g_advanceTokens { 0 };
std::atomic<bool> g_holdsTime { false };
int g_settleCount = 0;
int g_sampleIndex = 0;
int g_samplesPerFrame = 1;
int g_blendedSamples = 1;
framemath::PixelFormat g_pixels = framemath::PixelFormat::Bgra8;

void SetError(const std::string& text)
{
    mod::Log("Recorder: %s", text.c_str());
    std::lock_guard lock(g_textMutex);
    g_lastError = text;
}

bool EnsurePipeline(ID3D12Device* device)
{
    if (g_gpu.Device == device && g_gpu.Allocator)
        return true;
    g_gpu.Device = device;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_gpu.Allocator)))
        || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_gpu.Allocator, nullptr, IID_PPV_ARGS(&g_gpu.List)))
        || FAILED(g_gpu.List->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gpu.Fence))))
        return false;
    g_gpu.Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_gpu.Event != nullptr;
}

// Copies the image into g_gpu.Readback and waits for the GPU. Returns the row layout.
bool CopyToReadback(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    if (!EnsurePipeline(device))
        return false;

    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    if (!g_gpu.Readback || g_gpu.ReadbackSize < totalBytes) {
        if (g_gpu.Readback)
            g_gpu.Readback->Release();
        g_gpu.Readback = nullptr;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalBytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&g_gpu.Readback))))
            return false;
        g_gpu.ReadbackSize = totalBytes;
    }

    g_gpu.Allocator->Reset();
    g_gpu.List->Reset(g_gpu.Allocator, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_gpu.List->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = g_gpu.Readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    g_gpu.List->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_gpu.List->ResourceBarrier(1, &barrier);
    g_gpu.List->Close();

    ID3D12CommandList* lists[] = { g_gpu.List };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_gpu.Fence, ++g_gpu.FenceValue);
    if (g_gpu.Fence->GetCompletedValue() < g_gpu.FenceValue) {
        g_gpu.Fence->SetEventOnCompletion(g_gpu.FenceValue, g_gpu.Event);
        if (WaitForSingleObject(g_gpu.Event, 5000) != WAIT_OBJECT_0)
            mod::Log("Recorder: the GPU took over 5 s on frame %llu (fence %llu, reached %llu, device status 0x%lX)",
                     static_cast<unsigned long long>(g_frames.load()), static_cast<unsigned long long>(g_gpu.FenceValue),
                     static_cast<unsigned long long>(g_gpu.Fence->GetCompletedValue()), device->GetDeviceRemovedReason());
    }
    return true;
}

const char* FfmpegPixelFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return "bgra";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "rgba";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "x2bgr10le";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "rgbaf16le";
    default:
        return nullptr;
    }
}

framemath::PixelFormat PixelFormatOf(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return framemath::PixelFormat::X2Bgr10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return framemath::PixelFormat::RgbaF16;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return framemath::PixelFormat::Rgba8;
    default:
        return framemath::PixelFormat::Bgra8;
    }
}

int BytesPerPixel(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}

std::string TimestampedName()
{
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(name, sizeof(name), "tekken8mvm_%Y%m%d_%H%M%S", &local);
    return name;
}

void WriterLoop(std::shared_ptr<Encoder> encoder)
{
    for (;;) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock lock(encoder->Mutex);
            encoder->Changed.wait(lock, [&] { return !encoder->Queue.empty() || encoder->Closing; });
            if (encoder->Queue.empty())
                break;
            frame = std::move(encoder->Queue.front());
            encoder->Queue.pop_front();
        }
        encoder->Changed.notify_all();

        const uint8_t* data = frame.data();
        size_t remaining = frame.size();
        while (remaining > 0 && !encoder->Broken) {
            DWORD written = 0;
            if (!WriteFile(encoder->Stdin, data, static_cast<DWORD>(std::min<size_t>(remaining, 1 << 24)), &written, nullptr)) {
                encoder->Broken = true;
                encoder->Changed.notify_all();
                SetError("ffmpeg stopped accepting frames; see tekken8mvm_ffmpeg.log");
                break;
            }
            data += written;
            remaining -= written;
        }

        std::lock_guard lock(encoder->Mutex);
        encoder->Spare.push_back(std::move(frame));
    }

    CloseHandle(encoder->Stdin);
    encoder->Stdin = nullptr;
    WaitForSingleObject(encoder->Process.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(encoder->Process.hProcess, &code);
    CloseHandle(encoder->Process.hProcess);
    CloseHandle(encoder->Process.hThread);
    mod::Log("Recorder: ffmpeg exited with code %lu", code);
    if (code != 0)
        SetError("ffmpeg failed; see tekken8mvm_ffmpeg.log");
    --g_writers;
}

std::shared_ptr<Encoder> StartEncoder(UINT width, UINT height, DXGI_FORMAT format, const std::filesystem::path& output, const Codec& codec, int index)
{
    const char* pixelFormat = FfmpegPixelFormat(format);
    if (!pixelFormat) {
        SetError("the game's back buffer format isn't supported: " + std::to_string(format));
        return nullptr;
    }

    char arguments[512];
    snprintf(arguments, sizeof(arguments), "-hide_banner -y -f rawvideo -pix_fmt %s -s %ux%u -r %d -i - %s ", pixelFormat, width, height,
             g_settings.Fps, codec.Arguments);
    const std::string command = std::string("\"") + g_settings.FfmpegPath + "\" " + arguments + "\"" + output.string() + "\"";

    SECURITY_ATTRIBUTES inherit { sizeof(inherit), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 1 << 20)) {
        SetError("couldn't create a pipe for ffmpeg");
        return nullptr;
    }
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
    const std::wstring logName = index == 0 ? L"tekken8mvm_ffmpeg.log" : L"tekken8mvm_ffmpeg" + std::to_wstring(index + 1) + L".log";
    const std::wstring logPath = (mod::DataDir() / logName).wstring();
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    auto encoder = std::make_shared<Encoder>();
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = readEnd;
    startup.hStdOutput = log;
    startup.hStdError = log;
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    const BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &encoder->Process);
    CloseHandle(readEnd);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (!created) {
        CloseHandle(writeEnd);
        SetError("couldn't start ffmpeg (" + std::string(g_settings.FfmpegPath) + ")");
        return nullptr;
    }

    mod::Log("Recorder: %s", command.c_str());
    encoder->Stdin = writeEnd;
    ++g_writers;
    std::thread(WriterLoop, encoder).detach(); // ffmpeg may need a while to finish, so nothing waits on this thread
    return encoder;
}

Pass PassForLook(Look look)
{
    switch (look) {
    case Look::Greenscreen:
        return Pass::Greenscreen;
    case Look::StageOnly:
        return Pass::StageOnly;
    case Look::FightersOnBlack:
        return Pass::FightersOnBlack;
    case Look::Depth:
        return Pass::Depth;
    case Look::EffectsOnBlack:
        return Pass::EffectsOnBlack;
    default:
        return Pass::Picture;
    }
}

std::vector<int> EnabledStreamIndices()
{
    std::vector<int> enabled;
    for (int i = 0; i < MaxStreams; ++i)
        if (g_settings.Streams[i].Enabled)
            enabled.push_back(i);
    return enabled;
}

bool StartEncoders(UINT width, UINT height, DXGI_FORMAT format)
{
    const Settings& s = g_settings;
    const Codec& codec = Codecs()[std::clamp(s.CodecIndex, 0, static_cast<int>(Codecs().size()) - 1)];
    std::error_code ignored;
    std::filesystem::create_directories(s.OutputDir, ignored);

    std::vector<int> enabled = EnabledStreamIndices();
    if (enabled.empty())
        enabled.push_back(0); // the first stream is the default picture

    const std::string stem = TimestampedName();
    g_outputs.clear();
    g_plan.clear();
    std::string firstFile;
    for (const int index : enabled) {
        const Stream& stream = s.Streams[index];
        std::string name = stem;
        if (enabled.size() > 1)
            name += std::string("_") + (stream.Name[0] ? stream.Name : LookName(stream.Look));
        std::filesystem::path path = s.OutputDir / (name + codec.Extension);
        if (std::string(codec.Extension) == ".png") {
            std::filesystem::create_directories(s.OutputDir / name, ignored);
            path = s.OutputDir / name / "frame_%06d.png";
        }
        Output output;
        output.Encoder = StartEncoder(width, height, format, path, codec, static_cast<int>(g_outputs.size()));
        if (!output.Encoder)
            return false;
        if (firstFile.empty())
            firstFile = path.string();
        const int outputIndex = static_cast<int>(g_outputs.size());
        g_outputs.push_back(std::move(output));
        g_plan.push_back({ PassForLook(stream.Look), outputIndex });
    }

    std::lock_guard lock(g_textMutex);
    g_lastFile = firstFile;
    g_lastError.clear();
    return true;
}

void CloseEncoders()
{
    for (Output& output : g_outputs) {
        {
            std::lock_guard lock(output.Encoder->Mutex);
            output.Encoder->Closing = true;
        }
        output.Encoder->Changed.notify_all();
    }
    g_outputs.clear();
}

void Enqueue(Encoder& encoder, std::vector<uint8_t>&& frame)
{
    {
        std::lock_guard lock(encoder.Mutex);
        encoder.Queue.push_back(std::move(frame));
    }
    encoder.Changed.notify_all();
}

// Back-pressure: if ffmpeg is behind, hold this frame. The game's clock is locked to frames, so waiting here costs
// time but never smoothness.
std::vector<uint8_t> TakeBuffer(Encoder& encoder, size_t bytes)
{
    std::vector<uint8_t> frame;
    std::unique_lock lock(encoder.Mutex);
    encoder.Changed.wait(lock, [&] { return encoder.Queue.size() < MaxQueuedFrames || encoder.Broken; });
    if (!encoder.Spare.empty()) {
        frame = std::move(encoder.Spare.back());
        encoder.Spare.pop_back();
    }
    lock.unlock();
    frame.resize(bytes);
    return frame;
}

// Reads the mapped image into a tightly packed buffer.
void CopyRows(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, int bytesPerPixel, uint8_t* destination)
{
    const UINT rowBytes = footprint.Footprint.Width * bytesPerPixel;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(footprint.Footprint.RowPitch) * footprint.Footprint.Height };
    void* mapped = nullptr;
    if (FAILED(g_gpu.Readback->Map(0, &range, &mapped)))
        return;
    const auto* source = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        memcpy(destination + static_cast<size_t>(y) * rowBytes, source + static_cast<size_t>(y) * footprint.Footprint.RowPitch, rowBytes);
    D3D12_RANGE nothing { 0, 0 };
    g_gpu.Readback->Unmap(0, &nothing);
}

// Handles one captured pass: blends it into its output, and emits the finished frame on the last blended sample.
void CapturePass(const PlannedPass& pass, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, DXGI_FORMAT format)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    if (!CopyToReadback(device, queue, backBuffer, footprint)) {
        SetError("couldn't copy the frame");
        g_stopRequested = true;
        return;
    }

    const int bytesPerPixel = BytesPerPixel(format);
    const size_t pixels = static_cast<size_t>(footprint.Footprint.Width) * footprint.Footprint.Height;
    const size_t bytes = pixels * bytesPerPixel;
    Output& output = g_outputs[pass.Output];

    std::vector<uint8_t> image = TakeBuffer(*output.Encoder, bytes);
    CopyRows(footprint, bytesPerPixel, image.data());

    // With the depth buffer the key is already on the frame, and exact.
    if (pass.Pass == Pass::Greenscreen && !depth::Working()) {
        static std::vector<int> stack;
        framemath::PaintBackgroundKey(image.data(), footprint.Footprint.Width, footprint.Footprint.Height, g_pixels, g_settings.GreenDarkness, g_settings.BlueKey, stack);
    }

    if (g_blendedSamples <= 1) {
        Enqueue(*output.Encoder, std::move(image));
        return;
    }

    if (output.Sum.size() != pixels * 4)
        output.Sum.assign(pixels * 4, 0);
    framemath::AddSample(image.data(), pixels, g_pixels, output.Sum.data());
    ++output.Samples;
    if (output.Samples >= g_blendedSamples) {
        framemath::Average(output.Sum.data(), pixels, g_pixels, output.Samples, image.data());
        std::fill(output.Sum.begin(), output.Sum.end(), 0);
        output.Samples = 0;
        Enqueue(*output.Encoder, std::move(image));
        return;
    }
    std::lock_guard lock(output.Encoder->Mutex);
    output.Encoder->Spare.push_back(std::move(image));
}

} // namespace

const std::vector<Codec>& Codecs()
{
#define TEKKEN8MVM_BT709 "-vf scale=out_color_matrix=bt709:out_range=tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 "
    static const std::vector<Codec> codecs = {
        { "H.264 (x264, high quality)", ".mp4", TEKKEN8MVM_BT709 "-c:v libx264 -preset medium -crf 14 -pix_fmt yuv420p" },
        { "H.264 (NVIDIA NVENC)", ".mp4", TEKKEN8MVM_BT709 "-c:v h264_nvenc -preset p6 -cq 14 -pix_fmt yuv420p" },
        { "H.265 (NVIDIA NVENC)", ".mp4", TEKKEN8MVM_BT709 "-c:v hevc_nvenc -preset p6 -cq 16 -pix_fmt yuv420p" },
        { "ProRes 422 HQ (for editing)", ".mov", TEKKEN8MVM_BT709 "-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le" },
        { "PNG image sequence", ".png", "-c:v png -pix_fmt rgb24" },
    };
#undef TEKKEN8MVM_BT709
    return codecs;
}

const char* LookName(Look look)
{
    switch (look) {
    case Look::Greenscreen:
        return "Greenscreen";
    case Look::StageOnly:
        return "Stage only";
    case Look::FightersOnBlack:
        return "Fighters on black";
    case Look::Depth:
        return "Depth";
    case Look::EffectsOnBlack:
        return "Effects on black";
    default:
        return "Picture";
    }
}

Settings& Config()
{
    static const bool initialized = [] {
        strcpy_s(g_settings.Streams[0].Name, "picture");
        g_settings.Streams[0].Enabled = true;
        strcpy_s(g_settings.Streams[1].Name, "green");
        g_settings.Streams[1].Look = Look::Greenscreen;
        strcpy_s(g_settings.Streams[2].Name, "stage");
        g_settings.Streams[2].Look = Look::StageOnly;
        strcpy_s(g_settings.Streams[3].Name, "depth");
        g_settings.Streams[3].Look = Look::Depth;
        strcpy_s(g_settings.Streams[4].Name, "effects");
        g_settings.Streams[4].Look = Look::EffectsOnBlack;
        return true;
    }();
    (void)initialized;
    return g_settings;
}

void RequestStart()
{
    if (g_active || g_writers > 0)
        return;

    g_holdsTime = EnabledStreamIndices().size() > 1;

    g_samplesPerFrame = std::max(g_settings.MotionSamples, 1);
    g_blendedSamples = framemath::BlendedSampleCount(g_samplesPerFrame, g_settings.ShutterDegrees);
    g_advanceTokens = 0;
    g_settleCount = 0;
    g_sampleIndex = 0;
    g_passIndex = 0;
    g_stopRequested = false;
    g_rangeStopWait = 0;
    g_firstCaptureDone = false;
    g_startRequested = true;
    game::SetRecordingClock(true, static_cast<double>(g_settings.Fps) * g_samplesPerFrame);
}

void RequestStop()
{
    if (g_active)
        g_stopRequested = true;
}

void Toggle()
{
    if (g_active || g_startRequested)
        RequestStop();
    else
        RequestStart();
}

bool Active()
{
    return g_active || g_startRequested;
}

bool Finishing()
{
    return g_writers > 0 && !g_active;
}

uint64_t Frames()
{
    return g_frames;
}

std::string LastFile()
{
    std::lock_guard lock(g_textMutex);
    return g_lastFile;
}

std::string LastError()
{
    std::lock_guard lock(g_textMutex);
    return g_lastError;
}

bool HoldsTime()
{
    return g_active && g_holdsTime;
}

bool WaitingForFirstFrame()
{
    return !g_firstCaptureDone;
}

bool TakeAdvance()
{
    int tokens = g_advanceTokens.load();
    while (tokens > 0)
        if (g_advanceTokens.compare_exchange_weak(tokens, tokens - 1))
            return true;
    return false;
}

Pass ActivePass()
{
    if (g_active || g_startRequested)
        return g_activePass;
    return PassForLook(g_settings.Preview);
}

int EnabledStreams()
{
    return static_cast<int>(EnabledStreamIndices().size());
}

void RecordRange(double in, double out)
{
    g_settings.RangeIn = in;
    g_settings.RangeOut = out;
    g_settings.UseRange = true;
    g_rangeFired = false;
    g_rangeOnce = true;
}

void OnGameTick(double clock)
{
    Settings& s = g_settings;
    if (!s.UseRange) {
        g_rangeFired = false;
        return;
    }
    // On the way somewhere, the clock passes through places that don't count.
    if (game::Seeking())
        return;
    if (clock < s.RangeIn)
        g_rangeFired = false;
    if (!g_rangeFired && !Active() && !Finishing() && clock >= s.RangeIn && clock < s.RangeOut) {
        g_rangeFired = true;
        RequestStart();
    } else if (g_active && clock >= s.RangeOut) {
        // What's on screen is a little behind the game, and with passes each frame takes several renders.
        const int passes = std::max(static_cast<int>(g_plan.size()), 1);
        const int wait = PictureLag + (g_holdsTime ? passes * (s.SettleFrames + 1) * g_samplesPerFrame : 0);
        if (++g_rangeStopWait <= wait)
            return;
        RequestStop();
        if (g_rangeOnce) {
            g_rangeOnce = false;
            s.UseRange = false;
        }
    }
}

void OnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, DXGI_FORMAT format)
{
    if (g_startRequested.exchange(false)) {
        const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
        g_frames = 0;
        g_pixels = PixelFormatOf(format);
        if (!StartEncoders(static_cast<UINT>(desc.Width), desc.Height, format)) {
            CloseEncoders();
            game::SetRecordingClock(false, g_settings.Fps);
            return;
        }
        if (g_samplesPerFrame > 1 && g_pixels == framemath::PixelFormat::RgbaF16) {
            mod::Log("Recorder: this pixel format can't be blended, so motion blur is off");
            g_blendedSamples = 1;
        }
        g_passIndex = 0;
        g_activePass = g_plan[0].Pass;
        g_settleCount = 0;
        g_active = true;
    }
    if (!g_active)
        return;

    bool broken = false;
    for (const Output& output : g_outputs)
        broken = broken || output.Encoder->Broken;

    // Only stop between video frames, so every stream ends with the same number of frames.
    if (broken || (g_stopRequested && g_sampleIndex == 0 && g_passIndex == 0)) {
        g_stopRequested = false;
        g_active = false;
        g_holdsTime = false;
        game::SetRecordingClock(false, g_settings.Fps);
        CloseEncoders();
        mod::Log("Recorder: stopped after %llu frames", static_cast<unsigned long long>(g_frames.load()));
        return;
    }

    // The game shows a new look a few frames after it's asked for. Without a change there's nothing to wait for.
    const bool settlingNeeded = g_holdsTime || g_frames == 0;
    if (settlingNeeded && g_settleCount < g_settings.SettleFrames) {
        // The picture is one frame behind the game, so time starts moving one frame before the first capture.
        if (++g_settleCount == g_settings.SettleFrames)
            g_firstCaptureDone = true;
        return;
    }

    const PlannedPass pass = g_plan[g_passIndex];
    if (g_sampleIndex < g_blendedSamples)
        CapturePass(pass, device, queue, backBuffer, format);
    g_firstCaptureDone = true;

    int next = g_passIndex + 1;
    if (next >= static_cast<int>(g_plan.size())) {
        next = 0;
        if (++g_sampleIndex >= g_samplesPerFrame) {
            g_sampleIndex = 0;
            ++g_frames;
        }
        if (g_holdsTime)
            ++g_advanceTokens;
    }
    g_passIndex = next;
    g_activePass = g_plan[next].Pass;
    if (g_holdsTime)
        g_settleCount = 0;
}

int SaveRawFrame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, const std::filesystem::path& path)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    if (!CopyToReadback(device, queue, backBuffer, footprint))
        return 0;
    const int bytes = BytesPerPixel(footprint.Footprint.Format);
    const UINT rowBytes = footprint.Footprint.Width * bytes;
    void* mapped = nullptr;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(footprint.Footprint.RowPitch) * footprint.Footprint.Height };
    if (FAILED(g_gpu.Readback->Map(0, &range, &mapped)))
        return 0;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto* source = static_cast<const char*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        file.write(source + static_cast<size_t>(y) * footprint.Footprint.RowPitch, rowBytes);
    D3D12_RANGE nothing { 0, 0 };
    g_gpu.Readback->Unmap(0, &nothing);
    return bytes;
}

void WaitUntilFinished()
{
    while (g_active || g_writers > 0)
        Sleep(10);
}

} // namespace recorder
