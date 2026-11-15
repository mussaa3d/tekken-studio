#include "mod/depth.h"

#include <windows.h>

#include <MinHook.h>
#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "mod/guard.h"
#include "mod/log.h"

namespace depth {

namespace {

using CreateViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using BindFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using CompileFn = decltype(&D3DCompile);

// Places in the COM tables.
constexpr int DeviceCreateDepthStencilView = 21;
constexpr int ListOMSetRenderTargets = 46;

// The engine's near plane. Its depth is reversed and has no far plane, so a pixel's distance is this over its depth value.
constexpr float NearPlane = 10.0f;

CreateViewFn oCreateView = nullptr;
BindFn oBind = nullptr;

struct View {
    ID3D12Resource* Resource = nullptr;
    int Binds = 0;
};
std::mutex g_viewsMutex;
std::unordered_map<SIZE_T, View> g_views;
std::atomic<bool> g_counting { false };

void STDMETHODCALLTYPE DetourCreateView(ID3D12Device* device, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    oCreateView(device, resource, desc, handle);
    if (guard::Tripped())
        return;
    std::lock_guard lock(g_viewsMutex);
    g_views[handle.ptr] = { resource, 0 };
}

void STDMETHODCALLTYPE DetourBind(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    if (depth && g_counting.load(std::memory_order_relaxed) && !guard::Tripped()) {
        std::lock_guard lock(g_viewsMutex);
        const auto found = g_views.find(depth->ptr);
        if (found != g_views.end())
            ++found->second.Binds;
    }
    oBind(list, count, targets, single, depth);
}

// Up to this many frames can still be on the GPU when a slot comes round again.
constexpr int Slots = 8;

struct Slot {
    ID3D12CommandAllocator* Allocator = nullptr;
    ID3D12Resource* Held = nullptr;
    UINT64 FenceValue = 0;
};

struct Pipeline {
    ID3D12RootSignature* Root = nullptr;
    ID3D12PipelineState* DepthState = nullptr;
    ID3D12PipelineState* KeyState = nullptr;
    ID3D12DescriptorHeap* Heap = nullptr;
    ID3D12GraphicsCommandList* List = nullptr;
    ID3D12Fence* Fence = nullptr;
    HANDLE Event = nullptr;
    UINT64 FenceValue = 0;
    UINT Increment = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    std::array<Slot, Slots> Slots;
    int Next = 0;
    bool Failed = false;
} g_pipe;

std::atomic<bool> g_working { false };
std::atomic<int> g_width { 0 }, g_height { 0 };

const char kShader[] = R"(
Texture2D<float> DepthBuffer : register(t0);
SamplerState Linear : register(s0);
cbuffer Constants : register(b0) {
    float NearCm, FarCm, Curve, NearPlane;
    float KeyR, KeyG, KeyB, Unused;
    float PixelU, PixelV;
};
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float Read(float4 position) { return DepthBuffer.SampleLevel(Linear, position.xy * float2(PixelU, PixelV), 0); }
float4 DepthPS(float4 position : SV_Position) : SV_Target {
    float value = Read(position);
    float grey = 0;
    if (value > 0) {
        float distance = NearPlane / value;
        grey = pow(saturate(1 - (distance - NearCm) / max(FarCm - NearCm, 1)), Curve);
    }
    return float4(grey, grey, grey, 1);
}
float4 KeyPS(float4 position : SV_Position) : SV_Target {
    if (Read(position) > 0)
        discard;
    return float4(KeyR, KeyG, KeyB, 1);
}
)";

ID3DBlob* Compile(CompileFn compile, const char* entry, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "t8mvm_depth", nullptr, nullptr, entry, target, 0, 0, &code, &errors))) {
        mod::Log("Depth: shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        code = nullptr;
    }
    if (errors)
        errors->Release();
    return code;
}

bool CreatePipeline(ID3D12Device* device, DXGI_FORMAT format)
{
    HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // Windows' own, not one dropped next to the game
    const CompileFn compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        mod::Log("Depth: no shader compiler");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 10;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = parameters;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    ID3DBlob* serialized = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_pipe.Root))))
        return false;
    serialized->Release();

    ID3DBlob* vs = Compile(compile, "VS", "vs_5_0");
    ID3DBlob* depthPs = Compile(compile, "DepthPS", "ps_5_0");
    ID3DBlob* keyPs = Compile(compile, "KeyPS", "ps_5_0");
    if (!vs || !depthPs || !keyPs)
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
    state.pRootSignature = g_pipe.Root;
    state.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    state.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    state.SampleMask = UINT_MAX;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = format;
    state.SampleDesc.Count = 1;
    state.PS = { depthPs->GetBufferPointer(), depthPs->GetBufferSize() };
    if (FAILED(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&g_pipe.DepthState))))
        return false;
    state.PS = { keyPs->GetBufferPointer(), keyPs->GetBufferSize() };
    if (FAILED(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&g_pipe.KeyState))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Slots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_pipe.Heap))))
        return false;
    g_pipe.Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (Slot& slot : g_pipe.Slots)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.Allocator))))
            return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pipe.Slots[0].Allocator, nullptr, IID_PPV_ARGS(&g_pipe.List))) || FAILED(g_pipe.List->Close())
        || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pipe.Fence))))
        return false;
    g_pipe.Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_pipe.Format = format;
    return true;
}

DXGI_FORMAT ReadableFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// The scene's depth buffer is the one the whole picture is the size of that the frame kept coming back to: shadow maps are other sizes,
// and the custom depth pass only binds its target once or twice. The buffer comes back with a reference held.
ID3D12Resource* TakeBusiestBuffer(UINT screenWidth, UINT screenHeight)
{
    ID3D12Resource* best = nullptr;
    int bestBinds = 0;
    std::lock_guard lock(g_viewsMutex);
    for (auto& [handle, view] : g_views) {
        const int binds = view.Binds;
        view.Binds = 0;
        if (binds <= bestBinds)
            continue;
        const D3D12_RESOURCE_DESC desc = view.Resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            continue;
        if (desc.Width * 3 < screenWidth || desc.Height * 3 < screenHeight || ReadableFormat(desc.Format) == DXGI_FORMAT_UNKNOWN)
            continue;
        best = view.Resource;
        bestBinds = binds;
    }
    if (best)
        best->AddRef();
    return best;
}

} // namespace

bool Install(ID3D12Device* device)
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    bool ok = false;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
        void* createView = (*reinterpret_cast<void***>(device))[DeviceCreateDepthStencilView];
        void* bind = (*reinterpret_cast<void***>(list))[ListOMSetRenderTargets];
        ok = MH_CreateHook(createView, reinterpret_cast<void*>(&DetourCreateView), reinterpret_cast<void**>(&oCreateView)) == MH_OK && MH_EnableHook(createView) == MH_OK
            && MH_CreateHook(bind, reinterpret_cast<void*>(&DetourBind), reinterpret_cast<void**>(&oBind)) == MH_OK && MH_EnableHook(bind) == MH_OK;
    }
    if (list)
        list->Release();
    if (allocator)
        allocator->Release();
    mod::Log(ok ? "Depth: hooks installed" : "Depth: couldn't hook the depth methods, so the depth look is off");
    return ok;
}

bool Apply(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format, const Options& options)
{
    g_working = false;
    if (!oBind || g_pipe.Failed)
        return false;
    g_counting = true;
    if (!g_pipe.Root || g_pipe.Format != format) {
        if (g_pipe.Root || !CreatePipeline(device, format)) {
            mod::Log("Depth: couldn't set up drawing");
            g_pipe.Failed = true;
            return false;
        }
    }

    const D3D12_RESOURCE_DESC screen = backBuffer->GetDesc();
    ID3D12Resource* buffer = TakeBusiestBuffer(static_cast<UINT>(screen.Width), screen.Height);
    if (!buffer)
        return false;
    const D3D12_RESOURCE_DESC bufferDesc = buffer->GetDesc();
    // The engine hands out a few of these in turn, all the same size.
    if (static_cast<int>(bufferDesc.Width) != g_width.load() || static_cast<int>(bufferDesc.Height) != g_height.load())
        mod::Log("Depth: reading a %llux%u buffer, format %d, for a %llux%u picture", static_cast<unsigned long long>(bufferDesc.Width), bufferDesc.Height, bufferDesc.Format,
                 static_cast<unsigned long long>(screen.Width), screen.Height);

    const int index = g_pipe.Next;
    g_pipe.Next = (index + 1) % Slots;
    Slot& slot = g_pipe.Slots[index];
    if (g_pipe.Fence->GetCompletedValue() < slot.FenceValue) {
        g_pipe.Fence->SetEventOnCompletion(slot.FenceValue, g_pipe.Event);
        WaitForSingleObject(g_pipe.Event, 1000);
    }
    if (slot.Held)
        slot.Held->Release();
    slot.Held = buffer;

    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = ReadableFormat(bufferDesc.Format);
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pipe.Heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * g_pipe.Increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_pipe.Heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * g_pipe.Increment;
    device->CreateShaderResourceView(buffer, &view, cpu);

    slot.Allocator->Reset();
    ID3D12GraphicsCommandList* list = g_pipe.List;
    list->Reset(slot.Allocator, options.Kind == Draw::Depth ? g_pipe.DepthState : g_pipe.KeyState);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);

    const float constants[10] = { options.Near, options.Far, options.Curve, NearPlane, options.Key[0], options.Key[1], options.Key[2], 0,
                                  1.0f / static_cast<float>(screen.Width), 1.0f / static_cast<float>(screen.Height) };
    list->SetGraphicsRootSignature(g_pipe.Root);
    list->SetDescriptorHeaps(1, &g_pipe.Heap);
    list->SetGraphicsRootDescriptorTable(0, gpu);
    list->SetGraphicsRoot32BitConstants(1, 10, constants, 0);
    const D3D12_VIEWPORT viewport = { 0, 0, static_cast<float>(screen.Width), static_cast<float>(screen.Height), 0, 1 };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(screen.Width), static_cast<LONG>(screen.Height) };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    list->Close();

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_pipe.Fence, ++g_pipe.FenceValue);
    slot.FenceValue = g_pipe.FenceValue;

    g_width = static_cast<int>(bufferDesc.Width);
    g_height = static_cast<int>(bufferDesc.Height);
    g_working = true;
    return true;
}

bool Working()
{
    return g_working.load();
}

void BufferSize(int& width, int& height)
{
    width = g_width.load();
    height = g_height.load();
}

} // namespace depth
