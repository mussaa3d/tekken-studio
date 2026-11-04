#pragma once
// Pixel work for the recorder, with no graphics API involved so it can be tested on plain buffers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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

// Greenscreen fallback: pixels with no colour at all become pure green, keeping the alpha bits.
inline void PaintBlackGreen(uint8_t* image, size_t pixels, PixelFormat format)
{
    auto* px = reinterpret_cast<uint32_t*>(image);
    if (format == PixelFormat::X2Bgr10) {
        for (size_t i = 0; i < pixels; ++i)
            if ((px[i] & 0x3FFFFFFFu) == 0)
                px[i] |= 1023u << 10;
    } else if (format != PixelFormat::RgbaF16) {
        // Green is the second byte in both BGRA and RGBA.
        for (size_t i = 0; i < pixels; ++i)
            if ((px[i] & 0x00FFFFFFu) == 0)
                px[i] |= 0x0000FF00u;
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
