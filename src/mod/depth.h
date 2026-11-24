// The game's own depth buffer, drawn over the finished picture for the depth and keyed looks.
#pragma once

#include <d3d12.h>

namespace depth {

// Alpha is the key with no colour: nothing rendered turns black and see-through, and everything else is made fully there.
enum class Draw { Depth, Key, Alpha };

struct Options {
    Draw Kind = Draw::Depth;
    float Near = 100, Far = 2000, Curve = 1; // Depth: centimetres from the camera that are white and black, and how the grey falls between them
    float Key[3] = { 0, 1, 0 };              // Key: the colour of everything with nothing rendered in it
};

// Finds the methods the game binds its depth targets through. Takes any device, since these tables are shared.
bool Install(ID3D12Device* device);

// Draws onto the back buffer, which must be in the present state, and leaves it there. False if the depth can't be read yet.
bool Apply(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format, const Options& options);

// Whether the last Apply drew anything, and the size of the buffer it read.
bool Working();
void BufferSize(int& width, int& height);

} // namespace depth
