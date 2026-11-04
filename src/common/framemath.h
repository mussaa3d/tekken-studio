#pragma once
// Pixel work for the recorder, with no graphics API involved so it can be tested on plain buffers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace framemath {

enum class PixelFormat { Bgra8, Rgba8, X2Bgr10, RgbaF16 };

// How many of a frame's samples fall inside the shutter. A 180 degree shutter is open for half the frame.
inline int BlendedSampleCount(int samplesPerFrame, float shutterDegrees)
{
    const double open = std::clamp(static_cast<double>(shutterDegrees), 1.0, 360.0) / 360.0;
    return std::clamp(static_cast<int>(std::lround(samplesPerFrame * open)), 1, samplesPerFrame);
}

// Adds an image to a running total, four totals per pixel. Formats that can't be blended are left out, and the
// recorder turns blending off for those.
inline void AddSample(const uint8_t* image, size_t pixels, PixelFormat format, uint32_t* sum)
{
    if (format == PixelFormat::X2Bgr10) {
        const auto* source = reinterpret_cast<const uint32_t*>(image);
        for (size_t i = 0; i < pixels; ++i) {
            sum[i * 4] += source[i] & 1023;
            sum[i * 4 + 1] += (source[i] >> 10) & 1023;
            sum[i * 4 + 2] += (source[i] >> 20) & 1023;
            sum[i * 4 + 3] += source[i] >> 30;
        }
    } else if (format != PixelFormat::RgbaF16) {
        for (size_t i = 0; i < pixels * 4; ++i)
            sum[i] += image[i];
    }
}

// Writes the rounded average of the running total back as an image.
inline void Average(const uint32_t* sum, size_t pixels, PixelFormat format, int samples, uint8_t* image)
{
    const uint32_t half = static_cast<uint32_t>(samples) / 2;
    if (format == PixelFormat::X2Bgr10) {
        auto* out = reinterpret_cast<uint32_t*>(image);
        for (size_t i = 0; i < pixels; ++i) {
            const uint32_t a = (sum[i * 4] + half) / samples;
            const uint32_t b = (sum[i * 4 + 1] + half) / samples;
            const uint32_t c = (sum[i * 4 + 2] + half) / samples;
            const uint32_t d = (sum[i * 4 + 3] + half) / samples;
            out[i] = a | (b << 10) | (c << 20) | (d << 30);
        }
    } else if (format != PixelFormat::RgbaF16) {
        for (size_t i = 0; i < pixels * 4; ++i)
            image[i] = static_cast<uint8_t>((sum[i] + half) / samples);
    }
}

// Greenscreen: with the stage hidden the background is nearly black. Everything that dark and connected to the edge of the picture
// is background and turns pure green. Dark armour inside a fighter isn't connected to the edge, so it stays. Alpha bits are kept.
// `darkness` is on the 0 to 255 scale; a pixel counts as background when no channel is above it.
inline void PaintBackgroundGreen(uint8_t* image, int width, int height, PixelFormat format, int darkness, std::vector<int>& stack)
{
    if (format == PixelFormat::RgbaF16 || width <= 0 || height <= 0)
        return;
    auto* px = reinterpret_cast<uint32_t*>(image);
    const uint32_t limit10 = static_cast<uint32_t>(darkness) * 4 + 3;
    const uint32_t limit8 = static_cast<uint32_t>(darkness);
    // Green is the same bits in BGRA and RGBA, and the middle ten bits in the 10-bit format.
    const uint32_t green = format == PixelFormat::X2Bgr10 ? 1023u << 10 : 0x0000FF00u;
    const uint32_t colour = format == PixelFormat::X2Bgr10 ? 0x3FFFFFFFu : 0x00FFFFFFu;
    const auto isBackground = [&](uint32_t p) {
        if ((p & colour) == green)
            return false; // already done
        if (format == PixelFormat::X2Bgr10)
            return (p & 1023) <= limit10 && ((p >> 10) & 1023) <= limit10 && ((p >> 20) & 1023) <= limit10;
        return (p & 255) <= limit8 && ((p >> 8) & 255) <= limit8 && ((p >> 16) & 255) <= limit8;
    };
    stack.clear();
    const auto visit = [&](int index) {
        if (isBackground(px[index])) {
            px[index] = (px[index] & ~colour) | green;
            stack.push_back(index);
        }
    };
    for (int x = 0; x < width; ++x) {
        visit(x);
        visit((height - 1) * width + x);
    }
    for (int y = 0; y < height; ++y) {
        visit(y * width);
        visit(y * width + width - 1);
    }
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const int x = index % width;
        if (x > 0)
            visit(index - 1);
        if (x + 1 < width)
            visit(index + 1);
        if (index >= width)
            visit(index - width);
        if (index + width < width * height)
            visit(index + width);
    }
}

// Depth: a pixel renders as colour*(1-fog) + fogColour*fog, so with white fog minus black fog the colour cancels and
// only the fog amount, which grows with distance, is left. Writes that as grey into the black fog image.
inline void FogDifference(const uint8_t* whiteFog, uint8_t* blackFog, size_t pixels, PixelFormat format)
{
    if (format == PixelFormat::X2Bgr10) {
        const auto* white = reinterpret_cast<const uint32_t*>(whiteFog);
        auto* black = reinterpret_cast<uint32_t*>(blackFog);
        const auto channel = [](uint32_t w, uint32_t b, int shift) {
            const int high = static_cast<int>((w >> shift) & 1023), low = static_cast<int>((b >> shift) & 1023);
            return std::clamp(high - low, 0, 1023);
        };
        for (size_t i = 0; i < pixels; ++i) {
            const uint32_t grey = (channel(white[i], black[i], 0) + channel(white[i], black[i], 10) + channel(white[i], black[i], 20)) / 3;
            black[i] = grey | (grey << 10) | (grey << 20) | (3u << 30);
        }
    } else if (format != PixelFormat::RgbaF16) {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* w = whiteFog + i * 4;
            uint8_t* b = blackFog + i * 4;
            const int total = std::clamp(w[0] - b[0], 0, 255) + std::clamp(w[1] - b[1], 0, 255) + std::clamp(w[2] - b[2], 0, 255);
            const auto grey = static_cast<uint8_t>(total / 3);
            b[0] = b[1] = b[2] = grey;
            b[3] = 255;
        }
    }
}

} // namespace framemath
