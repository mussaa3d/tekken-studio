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
    const char* Filter;    // ffmpeg video filters this format needs, such as the colour matrix
    const char* Arguments;
};
const std::vector<Codec>& Codecs();

enum class Look { Picture, Greenscreen, StageOnly, FightersOnBlack, Depth, EffectsOnBlack };
const char* LookName(Look look);

// What the game should show for the frame being rendered.
enum class Pass { Picture, Greenscreen, StageOnly, FightersOnBlack, Depth, EffectsOnBlack };

struct Stream {
    char Name[24] = "";
    Look Look = Look::Picture;
    bool Enabled = false;
};
constexpr int MaxStreams = 5;

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
    bool BlueKey = false;     // the keyed look is blue instead of green
    // What a Greenscreen stream is saved as: 0 = the key colour, in the format chosen for the recording; 1 = ProRes 4444 with
    // transparency; 2 = PNG pictures with transparency. The last two have nothing to key, and the first of them is what you get
    // unless you ask for a colour.
    int KeyFile = 1;
    int GreenDarkness = 16;  // how dark, out of 255, the empty background can be for the keyed look when the depth buffer can't be read
    float DepthNear = 0, DepthFar = 2000, DepthCurve = 1.0f; // centimetres that are white and black, and the grey in between
    bool UseRange = false;    // start and stop by themselves at the timeline's range
    double RangeIn = 0, RangeOut = 10; // seconds on the fight clock
    // The size the game renders at while it records, whatever the monitor's size is. 0 = the game window as it is.
    int CaptureWidth = 0, CaptureHeight = 0;
    // The size of the video. 0 = as rendered. Smaller than the render size gives a cleaner picture than rendering at that size.
    int OutputWidth = 0, OutputHeight = 0;
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
// The recording under way saves its Greenscreen streams with transparency, so that pass wants an alpha and no key colour.
bool TransparentKey();

// The size the next recording's video will be, given the picture's size now. For the menu.
void VideoSize(int pictureWidth, int pictureHeight, int& width, int& height);

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
