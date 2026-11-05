#include "mod/overlay.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "mod/camera.h"
#include "mod/depth.h"
#include "mod/effects.h"
#include "mod/game.h"
#include "mod/guard.h"
#include "mod/log.h"
#include "mod/recorder.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn oPresent = nullptr;
ResizeFn oResize = nullptr;
ExecuteFn oExecute = nullptr;

struct Frame {
    ID3D12CommandAllocator* Allocator = nullptr;
    ID3D12Resource* BackBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv {};
    UINT64 FenceValue = 0;
};

ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain3* g_swapChain = nullptr;
ID3D12Device* g_device = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
HANDLE g_fenceEvent = nullptr;
std::vector<Frame> g_frames;
DXGI_SWAP_CHAIN_DESC g_desc {};
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_ready = false;
bool g_failed = false;

std::atomic<bool> g_menuOpen { false };
std::atomic<int> g_wheel { 0 };
std::atomic<bool> g_gameViewHovered { false };
std::atomic<bool> g_keyboardTaken { false };
HarnessOptions g_harness;
uint64_t g_harnessFrame = 0;
std::atomic<bool> g_harnessUiDone { false };

// The game's picture, copied each frame so the editor can draw it scaled down in the game view.
ID3D12Resource* g_gameCopy = nullptr;
D3D12_GPU_DESCRIPTOR_HANDLE g_gameCopyGpu {};
std::atomic<float> g_gameAspect { 0 };

void WaitForFence(UINT64 value);

void CreateGameCopy(ID3D12Resource* backBuffer)
{
    const D3D12_RESOURCE_DESC source = backBuffer->GetDesc();
    if (g_gameCopy) {
        const D3D12_RESOURCE_DESC current = g_gameCopy->GetDesc();
        if (current.Width == source.Width && current.Height == source.Height && current.Format == source.Format)
            return;
        g_queue->Signal(g_fence, ++g_fenceValue);
        WaitForFence(g_fenceValue);
        g_gameCopy->Release();
        g_gameCopy = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = source;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_gameCopy)))) {
        mod::Log("Overlay: couldn't create the game view texture");
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = source.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    // The game's alpha channel means nothing here, and the menu's shader would treat it as transparency.
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    const UINT increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += increment;
    g_gameCopyGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    g_gameCopyGpu.ptr += increment;
    g_device->CreateShaderResourceView(g_gameCopy, &view, cpu);
    g_gameAspect = static_cast<float>(source.Width) / static_cast<float>(source.Height);
}

void WaitForFence(UINT64 value)
{
    if (g_fence->GetCompletedValue() < value) {
        g_fence->SetEventOnCompletion(value, g_fenceEvent);
        if (WaitForSingleObject(g_fenceEvent, 1000) != WAIT_OBJECT_0)
            mod::Log("Overlay: the GPU took over 1 s (fence %llu, reached %llu)", static_cast<unsigned long long>(value),
                     static_cast<unsigned long long>(g_fence->GetCompletedValue()));
    }
}

void CreateRenderTargets()
{
    const UINT increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_frames.size(); ++i) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].BackBuffer));
        g_device->CreateRenderTargetView(g_frames[i].BackBuffer, nullptr, handle);
        g_frames[i].Rtv = handle;
        handle.ptr += increment;
    }
}

void ReleaseRenderTargets()
{
    g_queue->Signal(g_fence, ++g_fenceValue);
    WaitForFence(g_fenceValue);
    for (Frame& frame : g_frames)
        if (frame.BackBuffer) {
            frame.BackBuffer->Release();
            frame.BackBuffer = nullptr;
        }
}

struct WindowMessage {
    HWND Window;
    UINT Message;
    WPARAM WParam;
    LPARAM LParam;
};
std::mutex g_messageMutex;
std::vector<WindowMessage> g_messages;

void HandOverMessages()
{
    std::vector<WindowMessage> messages;
    {
        std::lock_guard lock(g_messageMutex);
        messages.swap(g_messages);
    }
    for (const WindowMessage& m : messages)
        ImGui_ImplWin32_WndProcHandler(m.Window, m.Message, m.WParam, m.LParam);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (guard::Tripped())
        return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);

    // The engine tells Windows parts of its picture are title bar and window buttons, which would swallow clicks on the editor there
    // and bring up the Snap menu.
    if (message == WM_NCHITTEST && g_menuOpen)
        return HTCLIENT;

    // During the UI test only the scripted mouse counts, not whatever the real one is doing.
    const bool scripted = g_harness.UiTest && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    if (ImGui::GetCurrentContext() && !scripted) {
        // Messages arrive on the game's thread and the editor draws on the render thread, so they wait here for the next frame.
        std::lock_guard lock(g_messageMutex);
        if (g_messages.size() < 4096)
            g_messages.push_back({ window, message, wParam, lParam });
    }

    if (message == WM_MOUSEWHEEL && (!g_menuOpen || g_gameViewHovered))
        g_wheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;

    const bool isMouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    const bool isKey = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    if ((g_menuOpen && (isMouse || isKey || message == WM_INPUT)) || (camera::WantsKeyboard() && (isKey || isMouse)))
        return message == WM_INPUT ? DefWindowProcW(window, message, wParam, lParam) : 0;
    return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
}

// Player names come in any script. Loading whole Korean and Japanese fonts takes seconds and a lot of memory, so the editor font only
// gets the characters that have actually turned up.
float g_fontSize = 18.0f;
std::mutex g_glyphMutex;
std::string g_glyphText;
bool g_glyphsChanged = false;
ImVector<ImWchar> g_glyphRanges;

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig font;
    font.OversampleH = 2;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", g_fontSize, &font)) {
        io.Fonts->AddFontDefault();
        return;
    }
    std::string text;
    {
        std::lock_guard lock(g_glyphMutex);
        text = g_glyphText;
    }
    if (text.empty())
        return;
    ImFontGlyphRangesBuilder builder;
    builder.AddText(text.c_str());
    g_glyphRanges.clear();
    builder.BuildRanges(&g_glyphRanges);
    ImFontConfig merge;
    merge.MergeMode = true;
    for (const char* path : { "C:\\Windows\\Fonts\\tahoma.ttf", "C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\YuGothR.ttc", "C:\\Windows\\Fonts\\msyh.ttc" })
        if (std::filesystem::exists(path))
            io.Fonts->AddFontFromFileTTF(path, g_fontSize, &merge, g_glyphRanges.Data);
}

void RebuildFontIfAsked()
{
    {
        std::lock_guard lock(g_glyphMutex);
        if (!g_glyphsChanged)
            return;
        g_glyphsChanged = false;
    }
    g_queue->Signal(g_fence, ++g_fenceValue);
    WaitForFence(g_fenceValue);
    ImGui_ImplDX12_InvalidateDeviceObjects();
    LoadFonts();
    ImGui_ImplDX12_CreateDeviceObjects();
}

bool CreateResources(IDXGISwapChain3* swapChain)
{
    g_swapChain = swapChain;
    if (FAILED(swapChain->GetDesc(&g_desc)) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_window = g_desc.OutputWindow;

    g_frames.resize(g_desc.BufferCount);
    for (Frame& frame : g_frames)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.Allocator))))
            return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].Allocator, nullptr, IID_PPV_ARGS(&g_list)))
        || FAILED(g_list->Close()) || FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Slot 0 is the menu font, slot 1 the game view.
    D3D12_DESCRIPTOR_HEAP_DESC srv = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rtv = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_desc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srvHeap))) || FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap))))
        return false;
    CreateRenderTargets();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // The editor packs a lot in, so text scales with the window.
    g_fontSize = std::clamp(std::floor(g_desc.BufferDesc.Width / 106.0f), 13.0f, 42.0f);
    LoadFonts();
    menu::ApplyStyle();
    ImGui::GetStyle().ScaleAllSizes(g_fontSize / 18.0f);

    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX12_InitInfo init;
    init.Device = g_device;
    init.CommandQueue = g_queue;
    init.NumFramesInFlight = static_cast<int>(g_desc.BufferCount);
    init.RTVFormat = g_desc.BufferDesc.Format;
    init.SrvDescriptorHeap = g_srvHeap;
    init.LegacySingleSrvCpuDescriptor = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    init.LegacySingleSrvGpuDescriptor = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&init))
        return false;

    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    mod::Log("Overlay ready: %ux%u, format %d, %u buffers", g_desc.BufferDesc.Width, g_desc.BufferDesc.Height, g_desc.BufferDesc.Format,
             g_desc.BufferCount);
    return true;
}

void RenderMenu(Frame& frame)
{
    const bool editor = g_menuOpen.load();
    if (editor)
        CreateGameCopy(frame.BackBuffer);

    RebuildFontIfAsked();
    HandOverMessages();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (g_harness.UiTest && !g_harnessUiDone)
        g_harnessUiDone = menu::RunUiTest();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = editor;
    if (editor) {
        menu::Draw();
    } else {
        g_gameViewHovered = false;
        menu::DrawOsd();
    }
    ImGui::Render();
    g_keyboardTaken = ImGui::GetIO().WantCaptureKeyboard;

    // The game can render at a different size from its window, with Windows stretching the picture to fit. The mouse
    // works in window coordinates, so the same stretch on the menu keeps everything lined up and covers the whole frame.
    if (ImDrawData* data = ImGui::GetDrawData(); data && data->DisplaySize.x > 0 && data->DisplaySize.y > 0) {
        const D3D12_RESOURCE_DESC size = frame.BackBuffer->GetDesc();
        const ImVec2 scale(static_cast<float>(size.Width) / data->DisplaySize.x, static_cast<float>(size.Height) / data->DisplaySize.y);
        if (scale.x != 1.0f || scale.y != 1.0f) {
            for (ImDrawList* list : data->CmdLists)
                for (ImDrawVert& vertex : list->VtxBuffer) {
                    vertex.pos.x *= scale.x;
                    vertex.pos.y *= scale.y;
                }
            data->ScaleClipRects(scale);
            data->DisplaySize = ImVec2(static_cast<float>(size.Width), static_cast<float>(size.Height));
        }
    }

    WaitForFence(frame.FenceValue);
    frame.Allocator->Reset();
    g_list->Reset(frame.Allocator, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.BackBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Copy the game's picture first, because the editor is about to draw over all of it.
    if (editor && g_gameCopy) {
        D3D12_RESOURCE_BARRIER toCopy[2] = { barrier, barrier };
        toCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[1].Transition.pResource = g_gameCopy;
        toCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_list->ResourceBarrier(2, toCopy);
        g_list->CopyResource(g_gameCopy, frame.BackBuffer);
        toCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_list->ResourceBarrier(2, toCopy);
    } else {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &barrier);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->OMSetRenderTargets(1, &frame.Rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_list->ResourceBarrier(1, &barrier);
    g_list->Close();

    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    frame.FenceValue = g_fenceValue;
}

void OnPresent(IDXGISwapChain3* swapChain)
{
    if (g_failed || !g_queue)
        return;
    if (!g_ready) {
        if (!CreateResources(swapChain)) {
            mod::Log("The overlay couldn't start.");
            g_failed = true;
            return;
        }
        g_ready = true;
        if (g_harness.Enabled)
            g_menuOpen = true;
    }
    if (swapChain != g_swapChain)
        return;

    const UINT index = swapChain->GetCurrentBackBufferIndex();
    if (index >= g_frames.size() || !g_frames[index].BackBuffer)
        return;
    Frame& frame = g_frames[index];

    game::CountRenderedFrame();
    if (g_harness.Preview)
        game::PreviewFrame();

    // The looks painted from the depth buffer go on the frame first, so the editor's view and the video both get them.
    const recorder::Pass pass = recorder::ActivePass();
    if (pass == recorder::Pass::Depth || pass == recorder::Pass::Greenscreen || pass == recorder::Pass::FightersOnBlack) {
        const recorder::Settings& look = recorder::Config();
        depth::Options options;
        options.Kind = pass == recorder::Pass::Depth ? depth::Draw::Depth : depth::Draw::Key;
        options.Near = look.DepthNear;
        options.Far = look.DepthFar;
        options.Curve = look.DepthCurve;
        options.Key[0] = 0;
        options.Key[1] = pass == recorder::Pass::Greenscreen && !look.BlueKey ? 1.0f : 0.0f;
        options.Key[2] = pass == recorder::Pass::Greenscreen && look.BlueKey ? 1.0f : 0.0f;
        depth::Apply(g_device, g_queue, frame.BackBuffer, frame.Rtv, g_desc.BufferDesc.Format, options);
    }

    // Captured before the menu is drawn, so the video never contains it.
    recorder::OnPresent(g_device, g_queue, frame.BackBuffer, g_desc.BufferDesc.Format);

    int screenshotTab = -1;
    if (g_harness.Enabled) {
        ++g_harnessFrame;
        if (g_harness.RecordFrames > 0) {
            if (g_harnessFrame == 30)
                recorder::RequestStart();
            if (recorder::Frames() >= static_cast<uint64_t>(g_harness.RecordFrames))
                recorder::RequestStop();
        }
        // One picture of each tab, a few frames after switching to it.
        if (g_harness.Screenshots) {
            const int phase = static_cast<int>(g_harnessFrame) - 6;
            if (phase >= 0 && phase < menu::TabCount * 6) {
                menu::SetTab(phase / 6);
                if (phase % 6 == 5)
                    screenshotTab = phase / 6;
            }
        }
    }

    RenderMenu(frame);

    if (screenshotTab >= 0) {
        wchar_t path[300];
        swprintf_s(path, L"%ls_%d.raw", g_harness.ScreenshotBase, screenshotTab);
        recorder::SaveRawFrame(g_device, g_queue, frame.BackBuffer, path);
    }
}

HRESULT PresentThrough(PresentFn next, IDXGISwapChain3* swapChain, UINT sync, UINT flags)
{
    if (!guard::Tripped()) {
        OnPresent(swapChain);
        if (recorder::Active())
            sync = 0; // recording runs as fast as frames can be made, not at the monitor's rate
    }
    return next(swapChain, sync, flags);
}

// Steam's overlay also hooks Present and can get there before us, which would put its popups on the frame when we copy it. The game
// calls Present through the swap chain's own table, which comes before every hook, so a copy of that table with our function in it
// puts us first: the frame is captured clean and Steam still draws on top for the screen.
constexpr int SwapChainMethods = 41;
void* g_frontTable[SwapChainMethods];
IDXGISwapChain3* g_frontChain = nullptr;
PresentFn g_frontNext = nullptr;
thread_local bool g_inFront = false;

HRESULT STDMETHODCALLTYPE FrontPresent(IDXGISwapChain3* swapChain, UINT sync, UINT flags)
{
    g_inFront = true;
    const HRESULT result = PresentThrough(g_frontNext, swapChain, sync, flags);
    g_inFront = false;
    return result;
}

void GetInFront(IDXGISwapChain3* swapChain)
{
    void** table = *reinterpret_cast<void***>(swapChain);
    if (table == g_frontTable)
        return;
    std::memcpy(g_frontTable, table, sizeof(g_frontTable));
    g_frontNext = reinterpret_cast<PresentFn>(table[8]);
    g_frontTable[8] = reinterpret_cast<void*>(&FrontPresent);
    *reinterpret_cast<void***>(swapChain) = g_frontTable;
    g_frontChain = swapChain;
    mod::Log("Present: in front of other hooks");
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain3* swapChain, UINT sync, UINT flags)
{
    if (g_inFront)
        return oPresent(swapChain, sync, flags);
    if (!guard::Tripped() && !g_harness.Enabled && swapChain != g_frontChain)
        GetInFront(swapChain);
    return PresentThrough(oPresent, swapChain, sync, flags);
}

HRESULT STDMETHODCALLTYPE DetourResize(IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    const bool ours = g_ready && swapChain == g_swapChain;
    if (ours)
        ReleaseRenderTargets();
    const HRESULT result = oResize(swapChain, count, width, height, format, flags);
    if (ours && SUCCEEDED(result)) {
        swapChain->GetDesc(&g_desc);
        CreateRenderTargets();
    }
    return result;
}

void STDMETHODCALLTYPE DetourExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!g_queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        g_queue = queue;
    oExecute(queue, count, lists);
}

template <typename T>
bool Hook(void* target, void* detour, T& original)
{
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK)
        mod::Log("Overlay hook failed: %s", MH_StatusToString(status));
    return status == MH_OK;
}

} // namespace

// The swap chain methods live in vtables shared by every swap chain in the process, so a throwaway device
// and a hidden window are enough to find them.
bool Install()
{
    const MH_STATUS started = MH_Initialize();
    if (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"tekken8mvmProbe";
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, windowClass.hInstance, nullptr);

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    bool ok = false;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters(0, &adapter))
        && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            desc.BufferDesc.Width = desc.BufferDesc.Height = 64;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (SUCCEEDED(factory->CreateSwapChain(queue, &desc, &swapChain))) {
                void** queueTable = *reinterpret_cast<void***>(queue);
                void** swapTable = *reinterpret_cast<void***>(swapChain);
                ok = Hook(queueTable[10], reinterpret_cast<void*>(&DetourExecute), oExecute)
                    && Hook(swapTable[8], reinterpret_cast<void*>(&DetourPresent), oPresent)
                    && Hook(swapTable[13], reinterpret_cast<void*>(&DetourResize), oResize);
                depth::Install(device);
            }
        }
    }
    if (swapChain)
        swapChain->Release();
    if (queue)
        queue->Release();
    if (device)
        device->Release();
    if (adapter)
        adapter->Release();
    if (factory)
        factory->Release();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    mod::Log(ok ? "Overlay hooks installed" : "Couldn't find the D3D12 methods");
    return ok;
}

void AddFontCharacters(const std::string& text)
{
    std::lock_guard lock(g_glyphMutex);
    bool asciiOnly = true;
    for (const unsigned char c : text)
        asciiOnly = asciiOnly && c < 0x80;
    if (asciiOnly || g_glyphText.find(text) != std::string::npos)
        return;
    g_glyphText += text;
    g_glyphsChanged = true;
}

bool MenuOpen()
{
    return g_menuOpen;
}

void ToggleMenu()
{
    g_menuOpen = !g_menuOpen;
}

bool IsGameForeground()
{
    if (!g_window)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

bool KeyboardTakenByUi()
{
    return g_keyboardTaken;
}

void SetGameViewHovered(bool hovered)
{
    g_gameViewHovered = hovered;
}

bool MouseOverGameView()
{
    return g_gameViewHovered;
}

uint64_t GameTexture()
{
    return g_gameCopy ? g_gameCopyGpu.ptr : 0;
}

float GameAspect()
{
    return g_gameAspect;
}

int TakeMouseWheel()
{
    return g_wheel.exchange(0);
}

void OnGameTick()
{
    if (!IsGameForeground() || KeyboardTakenByUi())
        return;

    static bool previous[256] = {};
    const auto pressed = [](int key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = down && !previous[key];
        previous[key] = down;
        return edge;
    };

    if (pressed(VK_INSERT))
        ToggleMenu();
    if (pressed(VK_F4))
        camera::ToggleLookInput();
    if (pressed(VK_F5))
        camera::ToggleFree();
    if (pressed(VK_F6))
        camera::ToggleDolly();
    if (pressed(VK_F7))
        recorder::Toggle();
    if (pressed(VK_F8))
        game::Time().Paused = !game::Time().Paused;
    if (pressed(VK_F9))
        effects::Config().HideHud = !effects::Config().HideHud;
    if (pressed(VK_OEM_PERIOD))
        game::StepOneFrame();
    if (pressed(VK_OEM_4))
        game::Time().Speed = std::max(game::Time().Speed * 0.5, 0.01);
    if (pressed(VK_OEM_6))
        game::Time().Speed = std::min(game::Time().Speed * 2.0, 4.0);
    // K sits on the game's default attack keys, so it only adds a key while the free camera owns the keyboard.
    if (pressed('K') && camera::WantsKeyboard())
        camera::AddKeyAtCurrentView();
}

void SetHarness(const HarnessOptions& options)
{
    g_harness = options;
}

bool HarnessBusy()
{
    return g_harness.UiTest && !g_harnessUiDone;
}

} // namespace overlay
