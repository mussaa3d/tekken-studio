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

// The keyed look when the depth buffer can't be read: with the stage hidden the background is nearly black. Everything that dark and
// connected to the edge of the picture is background and turns pure green or blue. Dark armour inside a fighter isn't connected to the edge, so it stays. Alpha bits are kept.
// `darkness` is on the 0 to 255 scale; a pixel counts as background when no channel is above it.
inline void PaintBackgroundKey(uint8_t* image, int width, int height, PixelFormat format, int darkness, bool blue, std::vector<int>& stack)
{
    if (format == PixelFormat::RgbaF16 || width <= 0 || height <= 0)
        return;
    auto* px = reinterpret_cast<uint32_t*>(image);
    const uint32_t limit10 = static_cast<uint32_t>(darkness) * 4 + 3;
    const uint32_t limit8 = static_cast<uint32_t>(darkness);
    // Green is the same bits in BGRA and RGBA, and the middle ten bits in the 10-bit format. Blue depends on the order.
    const uint32_t key = format == PixelFormat::X2Bgr10 ? (blue ? 1023u << 20 : 1023u << 10) : (!blue ? 0x0000FF00u : (format == PixelFormat::Bgra8 ? 0x000000FFu : 0x00FF0000u));
    const uint32_t colour = format == PixelFormat::X2Bgr10 ? 0x3FFFFFFFu : 0x00FFFFFFu;
    const auto isBackground = [&](uint32_t p) {
        if ((p & colour) == key)
            return false; // already done
        if (format == PixelFormat::X2Bgr10)
            return (p & 1023) <= limit10 && ((p >> 10) & 1023) <= limit10 && ((p >> 20) & 1023) <= limit10;
        return (p & 255) <= limit8 && ((p >> 8) & 255) <= limit8 && ((p >> 16) & 255) <= limit8;
    };
    stack.clear();
    const auto visit = [&](int index) {
        if (isBackground(px[index])) {
            px[index] = (px[index] & ~colour) | key;
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

} // namespace framemath
