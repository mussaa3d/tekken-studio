// Stands in for the game: a D3D12 window with the real mod DLL loaded.
//
// With no arguments this is tekken8mvm_preview: the editor running on a top-down fake scene, for trying the tool with no game.
// With arguments it's the automated test: tekken8mvm_harness <path to dinput8.dll> <frames>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {

struct PreviewState {
    double Clock, X, Y, Yaw;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool preview = argc < 3;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path dllPath = preview ? std::filesystem::path(self).parent_path() / "mod" / "dinput8.dll" : std::filesystem::path(argv[1]);

    SetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS", L"1");
    if (preview)
        SetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_PREVIEW", L"1");
    const bool livePicture = GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_PREVIEW", nullptr, 0) > 0;

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        MessageBoxW(nullptr, (L"Couldn't load " + dllPath.wstring()).c_str(), L"tekken8mvm preview", MB_OK | MB_ICONERROR);
        return 3;
    }

    // A steady tone for the audio pass test to capture, when it's asked for.
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_AUDIO", nullptr, 0) > 0) {
        static std::vector<char> wav;
        const int rate = 48000, seconds = 8;
        const uint32_t dataBytes = rate * seconds * 2;
        const auto put = [&](const void* data, size_t size) { wav.insert(wav.end(), static_cast<const char*>(data), static_cast<const char*>(data) + size); };
        const uint32_t riff = 36 + dataBytes, chunk = 16, byteRate = rate * 2, sampleRate = rate;
        const uint16_t format = 1, channels = 1, align = 2, bits = 16;
        put("RIFF", 4); put(&riff, 4); put("WAVEfmt ", 8); put(&chunk, 4); put(&format, 2); put(&channels, 2);
        put(&sampleRate, 4); put(&byteRate, 4); put(&align, 2); put(&bits, 2); put("data", 4); put(&dataBytes, 4);
        for (int i = 0; i < rate * seconds; ++i) {
            const auto sample = static_cast<int16_t>(4000 * sin(2 * 3.14159265358979 * 440.0 * i / rate));
            put(&sample, 2);
        }
        PlaySoundA(wav.data(), nullptr, SND_MEMORY | SND_ASYNC);
    }

    // The overlay installs its hooks on its own thread, and the frame counts below assume they are in.
    using ReadyFn = bool (*)();
    const auto ready = reinterpret_cast<ReadyFn>(GetProcAddress(mod, "tekken8mvmHarnessReady"));
    for (int wait = 0; ready && !ready() && wait < 1000; ++wait)
        Sleep(10);
    const int frames = preview ? 0 : _wtoi(argv[2]);

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"tekken8mvmHarness";
    RegisterClassW(&windowClass);
    const UINT width = preview ? 1600 : 1920, height = preview ? 900 : 1080;
    const DWORD style = preview ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    RECT outer { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    if (preview)
        AdjustWindowRect(&outer, style, FALSE);
    HWND window = CreateWindowW(windowClass.lpszClassName, preview ? L"tekken8mvm preview (no game: fake scene from above)" : L"tekken8mvm harness", style, 60, 40,
                                outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr, windowClass.hInstance, nullptr);

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        MessageBoxW(nullptr, L"This PC has no D3D12 device.", L"tekken8mvm preview", MB_OK | MB_ICONERROR);
        return 4;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* baseSwapChain = nullptr;
    factory->CreateSwapChain(queue, &desc, &baseSwapChain);
    IDXGISwapChain3* swapChain = nullptr;
    baseSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain));

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::vector<ID3D12Resource*> buffers(desc.BufferCount);
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs(desc.BufferCount);
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        rtvs[i] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvs[i].ptr += static_cast<SIZE_T>(i) * increment;
        device->CreateRenderTargetView(buffers[i], nullptr, rtvs[i]);
    }

    ID3D12CommandAllocator* allocator = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;

    // Stopping a recording happens inside Present, so frames keep coming until it's done.
    using BusyFn = bool (*)();
    const auto busy = reinterpret_cast<BusyFn>(GetProcAddress(mod, "tekken8mvmHarnessBusy"));
    using StateFn = void (*)(PreviewState*);
    const auto readState = reinterpret_cast<StateFn>(GetProcAddress(mod, "tekken8mvmHarnessState"));
    constexpr int HardLimit = 20000;
    bool running = true;
    for (int frame = 0; running && (preview || (frame < HardLimit && (frame < frames || (busy && busy())))); ++frame) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                running = false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
            break;

        const UINT index = swapChain->GetCurrentBackBufferIndex();
        allocator->Reset();
        list->Reset(allocator, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);

        const auto box = [&](LONG x, LONG y, LONG w, LONG h, float r, float g, float b) {
            const FLOAT colour[4] = { r, g, b, 1 };
            const D3D12_RECT rect = { x, y, x + w, y + h };
            list->ClearRenderTargetView(rtvs[index], colour, 1, &rect);
        };

        const FLOAT background[4] = { 0.10f, 0.25f, 0.45f, 1.0f };
        list->ClearRenderTargetView(rtvs[index], background, 0, nullptr);
        if (livePicture && readState) {
            // A stage seen from above. The white bar crosses it at a rate tied to the game clock, so it follows pause and
            // timescale. The red dot is the camera and the line shows where it looks. 1 pixel is 2 cm.
            PreviewState state {};
            readState(&state);
            const LONG cx = width / 2, cy = height / 2;
            for (LONG x = 0; x < static_cast<LONG>(width); x += 100)
                box(x, 0, 1, height, 0.16f, 0.32f, 0.52f);
            for (LONG y = 0; y < static_cast<LONG>(height); y += 100)
                box(0, y, width, 1, 0.16f, 0.32f, 0.52f);
            box(cx - 100, cy - 30, 40, 60, 0.95f, 0.75f, 0.25f); // the two fighters
            box(cx + 60, cy - 30, 40, 60, 0.30f, 0.80f, 0.95f);
            const LONG bar = static_cast<LONG>(std::fmod(state.Clock * 240.0, static_cast<double>(width)));
            box(bar, static_cast<LONG>(height) - 120, 24, 60, 1, 1, 1);
            const double px = cx + state.X / 2.0, py = cy + state.Y / 2.0;
            const double yaw = state.Yaw * 3.14159265358979 / 180.0;
            for (int step = 0; step < 14; ++step)
                box(static_cast<LONG>(px + std::cos(yaw) * step * 5), static_cast<LONG>(py + std::sin(yaw) * step * 5), 4, 4, 1, 0.5f, 0.5f);
            box(static_cast<LONG>(px) - 9, static_cast<LONG>(py) - 9, 18, 18, 0.95f, 0.2f, 0.25f);
        } else {
            // A white bar that moves one pixel per frame, so dropped frames would show.
            const LONG x = frame % static_cast<int>(width);
            box(x, static_cast<LONG>(height) / 2 - 60, 40, 120, 1, 1, 1);
        }

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        swapChain->Present(1, 0);

        queue->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, event);
            WaitForSingleObject(event, 2000);
        }
    }

    using WaitFn = void (*)();
    if (auto wait = reinterpret_cast<WaitFn>(GetProcAddress(mod, "tekken8mvmHarnessWait")))
        wait();
    ExitProcess(0);
}
