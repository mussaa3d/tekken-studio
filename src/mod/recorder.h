#pragma once
// Frame-locked recording, the way HLAE and CoDMVM streams work: the game advances by a fixed amount per
// video frame, each frame is copied before the menu is drawn on it, and ffmpeg encodes it. Rendering can
// be slower than real time and the video still plays perfectly smooth. There is no game audio in the video;
// the audio pass records it separately.
//
// Streams: with more than one stream on, the game holds each moment still and renders it once per stream, each
// with its own look, and every stream goes to its own file. All the files line up frame for frame.
//
// Motion blur: several samples are rendered per video frame and averaged, so the blur is real movement over the
// shutter time and not a post effect.
#include <d3d12.h>
#include <dxgi1_4.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recorder {

struct Codec {
    const char* Label;
    const char* Extension;
    const char* Arguments;
};
const std::vector<Codec>& Codecs();

enum class Look { Picture, Greenscreen, StageOnly, FightersOnBlack, Depth };
const char* LookName(Look look);

// What the game should show for the frame being rendered. Depth renders twice, with white fog and then black fog:
// subtracting the two cancels the picture and leaves only how far away each pixel is.
enum class Pass { Picture, Greenscreen, StageOnly, FightersOnBlack, DepthWhiteFog, DepthBlackFog };

struct Stream {
    char Name[24] = "";
    Look Look = Look::Picture;
    bool Enabled = false;
};
constexpr int MaxStreams = 4;

struct Settings {
    int Fps = 60;
    int CodecIndex = 0;
    char FfmpegPath[260] = "ffmpeg";
    std::filesystem::path OutputDir;
    std::array<Stream, MaxStreams> Streams;
    int SettleFrames = 3;     // frames to wait after changing the look, so the new look is what gets captured
    int MotionSamples = 1;    // renders per video frame; 1 = no blur
    float ShutterDegrees = 180; // how much of each frame's time the shutter is open
    Look Preview = Look::Picture; // the look on screen while not recording
    int GreenDarkness = 16;   // how dark, out of 255, the empty background behind the fighters can be for the greenscreen
    bool UseRange = false;    // start and stop by themselves at the timeline's range
    double RangeIn = 0, RangeOut = 10; // seconds on the fight clock
};
Settings& Config();

void RequestStart();
void RequestStop();
void Toggle();
bool Active();        // frames are being captured, or are about to be
bool Finishing();     // stopped, and ffmpeg is still writing out the file
uint64_t Frames();    // video frames so far
std::string LastFile();
std::string LastError();

// More than one pass per sample: the game must only move when TakeAdvance says so.
bool HoldsTime();
bool TakeAdvance();
// Starting takes a few frames. The game waits, so a recording begins on the frame it was asked to begin on.
bool WaitingForFirstFrame();
Pass ActivePass();
int EnabledStreams();

// Records exactly this stretch of the timeline once, starting when the clock gets to the start. The caller takes the fight there.
void RecordRange(double in, double out);

// Game thread: starts and stops at the timeline range, when that's on.
void OnGameTick(double clockSeconds);

// Render thread, inside Present, before the menu is drawn.
void OnPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, DXGI_FORMAT format);

// For the test harness: writes the current back buffer as raw pixels. Returns the bytes per pixel.
int SaveRawFrame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, const std::filesystem::path& path);

void WaitUntilFinished();

} // namespace recorder
