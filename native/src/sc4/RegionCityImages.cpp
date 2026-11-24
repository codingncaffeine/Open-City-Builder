#include "sc4/RegionCityImages.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gz/Buffer.h"

namespace sc4 {
namespace {

// TriangleFilter (i386 0x0019e7c4): 1 - |t| inside one pixel, 0 outside.
double TriangleFilter(double t) {
    const double a = std::fabs(t);
    return 1.0 <= a ? 0.0 : 1.0 - a;
}

// GenerateKernel<float*> (i386 0x006a62ac): `taps` samples of the filter from `start` in steps of `step`, scaled to sum to
// `total` (all in float, as the original).
void GenerateKernel(float start, float step, float* first, float* last, double (*filter)(double), float total) {
    float sum = 0.0f;
    for (float* p = first; p != last; ++p) {
        *p = static_cast<float>(filter(static_cast<double>(start)));
        sum += *p;
        start += step;
    }
    for (float* p = first; p != last; ++p) *p = (total / sum) * *p;
}

// ConvertKernelToInteger<float*, long*, long> (i386 0x006a632a): the truncated taps, with the difference between their sum
// and `total` spread over them. The original adds (sum - total) rather than subtracting it (disassembly 0x006a634c: SUB
// EBX,[EBP+0x14] then each tap += share); kept.
void ConvertKernelToInteger(const float* first, const float* last, std::int32_t* out, std::int32_t total) {
    std::int32_t sum = 0, count = 0;
    for (const float* p = first; p != last; ++p, ++count) sum += static_cast<std::int32_t>(*p);
    std::int32_t rest = sum - total;
    for (const float* p = first; p != last; ++p, --count) {
        const std::int32_t rounded = rest < 0 ? rest - count + 1 : rest + count - 1;
        const std::int32_t share = rounded / count;
        *out++ = share + static_cast<std::int32_t>(*p);
        rest -= share;
    }
}

// One channel sum back to a byte: rounded by 0x2000 >> 14; above 0xff -> 0 when the sum went negative, else 0xff.
std::uint32_t ToByte(std::int32_t sum) {
    const std::int32_t rounded = sum + 0x2000;
    std::uint32_t v = static_cast<std::uint32_t>(rounded >> 14);
    if (v > 0xff) v = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(rounded >> 31));
    return v;
}

std::uint32_t Combine(const std::uint32_t a, const std::uint32_t b, const std::int32_t* k) {
    std::int32_t s[4];
    for (int c = 0; c < 4; ++c) {
        const int shift = 24 - 8 * c;
        s[c] = static_cast<std::int32_t>((a >> shift) & 0xff) * k[0] + static_cast<std::int32_t>((b >> shift) & 0xff) * k[1];
    }
    return ToByte(s[3]) + ToByte(s[0]) * 0x1000000u + ToByte(s[1]) * 0x10000u + ToByte(s[2]) * 0x100u;
}

// ResampleHorizontal (i386 0x0019e800): output pixel x = k0 * source[x - 1] + k1 * source[x], zero outside the row.
void ResampleHorizontal(std::uint32_t* out, const std::uint32_t* row, int outWidth, int width, const std::int32_t* kernel) {
    for (int x = 0; x < outWidth; ++x) {
        const int i = x - 1;
        const std::uint32_t a = (i < 0 || i >= width) ? 0u : row[i];
        const std::uint32_t b = (i + 1 < 0 || i + 1 >= width) ? 0u : row[i + 1];
        out[x] = Combine(a, b, kernel);
    }
}

// The pointer-level Resample (i386 0x001a1b3a): rows resampled horizontally into a two-row ring, each output row
// k0 * ring[y - 1] + k1 * ring[y] (ResampleVertical 0x0019faa8), rows outside the source zero.
void Resample(std::uint32_t* out, int outStride, const std::uint8_t* source, int sourceStride, int outWidth, int outHeight,
              int width, int height, double xFraction, double yFraction) {
    float taps[2];
    std::int32_t horizontal[2], vertical[2];
    GenerateKernel(-static_cast<float>(xFraction), 1.0f, taps, taps + 2, TriangleFilter, 16384.0f);
    ConvertKernelToInteger(taps, taps + 2, horizontal, 0x4000);
    GenerateKernel(-static_cast<float>(yFraction), 1.0f, taps, taps + 2, TriangleFilter, 16384.0f);
    ConvertKernelToInteger(taps, taps + 2, vertical, 0x4000);
    std::vector<std::uint32_t> older, newer;
    int next = -4;   // the next source row the ring loads
    for (int y = 0; y < outHeight; ++y) {
        const int wanted = y + 1;
        int load = std::min(wanted - next, 2);
        next = wanted - load;
        for (; load > 0; --load) {
            older.swap(newer);
            newer.assign(static_cast<std::size_t>(outWidth), 0u);
            if (next >= 0 && next <= height - 1)
                ResampleHorizontal(newer.data(), reinterpret_cast<const std::uint32_t*>(source + std::size_t(next) * sourceStride),
                                   outWidth, width, horizontal);
            ++next;
        }
        if (older.size() < static_cast<std::size_t>(outWidth)) older.assign(static_cast<std::size_t>(outWidth), 0u);
        auto* row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(out) + std::size_t(y) * outStride);
        for (int x = 0; x < outWidth; ++x) row[x] = Combine(older[x], newer[x], vertical);
    }
}

int AreaWidth(cIGZBuffer* b) { const cRZRect* r = b->GetBufferArea(); return r->right - r->left; }
int AreaHeight(cIGZBuffer* b) { const cRZRect* r = b->GetBufferArea(); return r->bottom - r->top; }

}  // namespace

cIGZBuffer* RegionResample(cIGZBuffer* source, double xFraction, double yFraction) {   // i386 0x001a1dd8
    if (!source) return nullptr;
    auto* out = new cGZBuffer();
    out->AddRef();
    const int width = AreaWidth(source), height = AreaHeight(source);
    if (!out->Init(static_cast<std::uint32_t>(width + 2), static_cast<std::uint32_t>(height + 2), cGZBufferColorType{9, 0x20})) {
        out->Release();
        return nullptr;
    }
    if (source->Lock(cIGZBuffer::kLockRead)) {
        if (out->Lock(cIGZBuffer::kLockWrite)) {
            Resample(static_cast<std::uint32_t*>(out->GetColorSurfaceBits()), static_cast<int>(out->GetColorSurfaceStride()),
                     static_cast<const std::uint8_t*>(source->GetColorSurfaceBits()), static_cast<int>(source->GetColorSurfaceStride()),
                     width + 2, height + 2, width, height, xFraction, yFraction);
            out->Unlock(cIGZBuffer::kLockWrite);
        }
        source->Unlock(cIGZBuffer::kLockRead);
    }
    return out;
}

void SC4MoveMaskToAlpha(cIGZBuffer* image, cIGZBuffer* mask) {   // i386 0x0019fe7a
    if (!image || !mask) return;
    if (!image->Lock(cIGZBuffer::kLockWrite)) return;
    if (mask->Lock(cIGZBuffer::kLockRead)) {
        const int width = std::min(AreaWidth(mask), AreaWidth(image));
        const int height = std::min(AreaHeight(mask), AreaHeight(image));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const std::uint32_t pixel = image->GetPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                const std::uint32_t m = mask->GetPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                image->SetPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), (pixel & 0xffffffu) + m * 0x1000000u);
            }
        mask->Unlock(cIGZBuffer::kLockRead);
    }
    image->Unlock(cIGZBuffer::kLockWrite);
}

void SC4CreateSpanList(cIGZBuffer* buffer, int shift, std::vector<std::uint32_t>& out, bool weights) {   // i386 0x001a1fae
    out.clear();
    if (!buffer || !buffer->Lock(cIGZBuffer::kLockRead)) return;
    const int width = AreaWidth(buffer), height = AreaHeight(buffer);
    const auto* bits = static_cast<const std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::size_t stride = buffer->GetColorSurfaceStride();
    std::uint32_t rowWord = 0;
    for (int y = 0; y < height; ++y, rowWord += 0x10000) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(bits + std::size_t(y) * stride);
        bool inside = false;
        int start = 0, x = 0;
        auto close = [&](int end) {
            out.push_back(static_cast<std::uint32_t>(end) + rowWord);
            if (weights)
                for (int i = start; i < end; ++i) out.push_back((row[i] >> shift) & 0xff);
        };
        for (; x < width; ++x) {
            if (static_cast<std::uint8_t>(row[x] >> shift) == 0) {
                if (inside) close(x);
                inside = false;
            } else {
                if (!inside) {
                    out.push_back(static_cast<std::uint32_t>(x) + rowWord);
                    start = x;
                }
                inside = true;
            }
        }
        if (inside) close(x);
    }
    buffer->Unlock(cIGZBuffer::kLockRead);
}

void SC4RecalculateRegionCityImages(cIGZBuffer* image, cIGZBuffer* mask, cIGZBuffer* altImage, cIGZBuffer* altMask,
                                    float mapX, float mapY, RegionCityImages& out) {   // i386 0x001a4cfe
    const double xFraction = 1.0 - (static_cast<double>(mapX) - std::floor(static_cast<double>(mapX)));
    const double yFraction = 1.0 - (static_cast<double>(mapY) - std::floor(static_cast<double>(mapY)));
    cIGZBuffer* newMask = RegionResample(mask, xFraction, yFraction);
    out.image = RegionResample(image, xFraction, yFraction);
    cIGZBuffer* newAltMask = nullptr;
    if (altImage && altMask) {
        out.altImage = RegionResample(altImage, xFraction, yFraction);
        newAltMask = RegionResample(altMask, xFraction, yFraction);
    }
    SC4MoveMaskToAlpha(out.image, newMask);
    SC4CreateSpanList(newMask, 0, out.hitSpans, false);
    SC4CreateSpanList(newMask, 8, out.selectSpans, true);
    SC4CreateSpanList(newMask, 16, out.gridSpans, true);
    if (out.altImage && newAltMask) SC4MoveMaskToAlpha(out.altImage, newAltMask);
    // The label anchor. The original weighs byte 1 + 2 * column of each mask row for column < width (so the green and alpha
    // bytes of the row's first half), summed in 64 bits and rounded half up; the area's middle when nothing weighs.
    bool done = false;
    if (newMask) {
        const cRZRect* area = newMask->GetBufferArea();
        const int width = area->right, height = area->bottom;
        if (newMask->Lock(cIGZBuffer::kLockRead)) {
            const auto* bits = static_cast<const std::uint8_t*>(newMask->GetColorSurfaceBits());
            const std::size_t stride = newMask->GetColorSurfaceStride();
            std::int64_t sumX = 0, sumY = 0, total = 0;
            for (int y = 0; y < height; ++y) {
                std::uint32_t rowX = 0, rowY = 0, rowTotal = 0;   // 32-bit per row, as the original
                for (int x = 0; x < width; ++x) {
                    const std::uint32_t b = bits[std::size_t(y) * stride + 1 + std::size_t(x) * 2];
                    if (b != 0) {
                        rowX += static_cast<std::uint32_t>(x) * b;
                        rowY += static_cast<std::uint32_t>(y) * b;
                        rowTotal += b;
                    }
                }
                sumX += static_cast<std::int32_t>(rowX);
                sumY += static_cast<std::int32_t>(rowY);
                total += static_cast<std::int32_t>(rowTotal);
            }
            newMask->Unlock(cIGZBuffer::kLockRead);
            if (total > 0) {
                const std::int64_t half = total / 2;
                out.centerX = static_cast<int>((sumX + half) / total);
                out.centerY = static_cast<int>((sumY + half) / total);
                done = true;
            }
        }
        if (!done) {
            out.centerX = width / 2;
            out.centerY = height / 2;
            done = true;
        }
    }
    if (newMask) newMask->Release();
    if (newAltMask) newAltMask->Release();
}

}  // namespace sc4
