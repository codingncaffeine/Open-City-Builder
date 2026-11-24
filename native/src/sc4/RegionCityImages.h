// The region screen's city picture preparation (SC4WinRegionScreen.cpp, Mac i386): the city thumbnail and its mask are
// resampled by the fraction of the city's map position (anonymous Resample 0x001a1dd8 / 0x001a1b3a, ResampleHorizontal
// 0x0019e800, ResampleVertical 0x0019faa8, a two-tap triangle kernel from GenerateKernel 0x006a62ac and
// ConvertKernelToInteger 0x006a632a), the mask's blue moves into the picture's alpha (SC4MoveMaskToAlpha 0x0019fe7a), the
// mask's three channels become span lists (SC4CreateSpanList 0x001a1fae) and the label anchor is weighed from the mask
// (cSC4WinRegionScreen::RecalculateImagesFromCitySnapshots 0x001a4cfe).
#pragma once
#include <cstdint>
#include <vector>

class cIGZBuffer;

namespace sc4 {

// A span list: per row, for every run of pixels whose channel byte is non-zero, a start word (x | y << 16) and an end word
// (exclusive x | y << 16), then, with weights, the channel byte of every pixel of the run.
void SC4CreateSpanList(cIGZBuffer* buffer, int shift, std::vector<std::uint32_t>& out, bool weights);

// The picture's colours keep, its alpha becomes the mask's lowest byte (blue), over the smaller of the two areas.
void SC4MoveMaskToAlpha(cIGZBuffer* image, cIGZBuffer* mask);

// A new ARGB8888 buffer two pixels wider and taller than `source`, shifted by (xFraction, yFraction) of a pixel with the
// triangle kernel; a counted reference, null without a source.
cIGZBuffer* RegionResample(cIGZBuffer* source, double xFraction, double yFraction);

struct RegionCityImages {
    cIGZBuffer* image = nullptr;      // +0x1c the resampled picture with the mask in its alpha (counted reference)
    cIGZBuffer* altImage = nullptr;   // +0x24 the second picture (transportation view), same treatment, or null
    std::vector<std::uint32_t> hitSpans;      // +0x44 blue, no weights (hit testing)
    std::vector<std::uint32_t> gridSpans;     // +0x50 red, weights (the city grid)
    std::vector<std::uint32_t> selectSpans;   // +0x5c green, weights (the selected city)
    int centerX = 0, centerY = 0;             // +0x68 / +0x6c the label anchor
};

// RecalculateImagesFromCitySnapshots on one city: `out` takes the results; the caller keeps its own references to the
// inputs. The alternative pair is used only when both buffers are given.
void SC4RecalculateRegionCityImages(cIGZBuffer* image, cIGZBuffer* mask, cIGZBuffer* altImage, cIGZBuffer* altMask,
                                    float mapX, float mapY, RegionCityImages& out);

}  // namespace sc4
