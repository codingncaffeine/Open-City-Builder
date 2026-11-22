#include "render/gd/SGLDX7Driver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "platform/Unwired.h"

namespace nSGLDX7 {
namespace {

constexpr std::int32_t kDeviceLost = static_cast<std::int32_t>(0x887601c2u);   // D3DERR_DEVICELOST as the shim reports it
constexpr std::size_t kMaxStages = 16;                          // guard against a hostile device description
// gDriverInfoPrefix @ 0x100b10e68 -> 0x1009f9b88.
constexpr const char* kDriverInfoPrefix = "Maxis 3D GDriver\nDirectX\n2.0\n";

inline std::uint32_t PackUnitColor(float r, float g, float b, float a) {
    return gd::PackClearColor(r, g, b, a);
}

// FogStart/FogEnd/FogDensity are handed to SetRenderState as raw float bits by the shim.
inline std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

// The pixel-format table SurfaceFormatEnumerator matches non-FourCC formats against (@ 0x100a26a60, 9 entries,
// checked from index 8 down to 0). Fields: bit count, alpha mask, red mask, green mask, blue mask.
struct FormatRow {
    std::uint32_t bitsPerPixel, alphaMask, redMask, greenMask, blueMask;
};
constexpr FormatRow kFormatTable[9] = {
    {0x18, 0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff},   // 0: RGB888
    {0x20, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff},   // 1: ARGB8888
    {0x20, 0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff},   // 2: X8R8G8B8
    {0x10, 0x00000000, 0x0000f800, 0x000007e0, 0x0000001f},   // 3: RGB565
    {0x10, 0x00000000, 0x00007c00, 0x000003e0, 0x0000001f},   // 4: RGB555
    {0x10, 0x00008000, 0x00007c00, 0x000003e0, 0x0000001f},   // 5: ARGB1555
    {0x10, 0x00000000, 0x00000f00, 0x000000f0, 0x0000000f},   // 6: RGB444
    {0x10, 0x0000f000, 0x00000f00, 0x000000f0, 0x0000000f},   // 7: ARGB4444
    {0x10, 0x000000ff, 0x00000000, 0x00000000, 0x00000000},   // 8: A8
};

inline std::uint32_t PopCount(std::uint32_t v) {
    std::uint32_t n = 0;
    while (v) {
        ++n;
        v &= v - 1;
    }
    return n;
}

// ---- indexed triangle-list generation for strip/fan (the original's template instantiations, 0x1006048c4..) ----
template <class In>
void GenerateIndexedTriangleStrip(int count, const In* in, std::uint16_t* out, std::uint32_t* minIndex,
                                 std::uint32_t* maxIndex) {
    std::uint32_t a0 = in[0], a1 = in[1];
    std::uint32_t lo = a0 < a1 ? a0 : a1;
    std::uint32_t hi = a0 < a1 ? a1 : a0;
    if (count * 3 != 6) {
        const In* p = in + 2;
        std::uint32_t i = 1;
        do {
            const std::uint32_t c = *p++;
            out[i - 1] = static_cast<std::uint16_t>(a0);
            out[i] = static_cast<std::uint16_t>(a1);
            out[i + 1] = static_cast<std::uint16_t>(c);
            std::uint32_t cand = lo;
            if (c < lo) cand = c;
            if (hi < c) cand = lo;
            lo = cand;
            if (hi < c) hi = c;
            if (((i - 1) & 1) != 0) {
                a1 = c;   // the original keeps a0 on odd triangles
            } else {
                a0 = c;
            }
            const std::uint32_t next = i + 2;
            i += 3;
            if (next >= static_cast<std::uint32_t>(count * 3 - 6)) break;
        } while (true);
    }
    *minIndex = lo;
    *maxIndex = hi;
}

template <class In>
void GenerateIndexedTriangleFan(int count, const In* in, std::uint16_t* out, std::uint32_t* minIndex,
                                std::uint32_t* maxIndex) {
    const std::uint32_t first = in[0];
    std::uint32_t a1 = in[1];
    std::uint32_t lo = first < a1 ? first : a1;
    std::uint32_t hi = first < a1 ? a1 : first;
    if (count * 3 != 6) {
        const In* p = in + 2;
        std::uint32_t i = 1;
        do {
            const std::uint32_t c = *p++;
            out[i - 1] = static_cast<std::uint16_t>(first);
            out[i] = static_cast<std::uint16_t>(a1);
            out[i + 1] = static_cast<std::uint16_t>(c);
            std::uint32_t cand = lo;
            if (c < lo) cand = c;
            if (hi < c) cand = lo;
            lo = cand;
            a1 = c;
            if (hi < c) hi = c;
            const std::uint32_t next = i + 2;
            i += 3;
            if (next >= static_cast<std::uint32_t>(count * 3 - 6)) break;
        } while (true);
    }
    *minIndex = lo;
    *maxIndex = hi;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Free helpers

int SurfaceFormatEnumerator(sGDPixelFormat* format, void* context) {
    auto* ctx = static_cast<sGDTextureFormatContext*>(context);
    std::uint32_t flags = format->dwFlags;
    if ((flags & 4) != 0) {   // DDPF_FOURCC
        const std::uint32_t fourCC = format->dwFourCC;
        if ((fourCC & 0xffffff) != 0x545844 || (fourCC + 0xcf000000u) > 0x04ffffffu) return 1;
        ctx->compressed = 1;
        cDXTextureFormatInfo info;
        info.alphaMask = format->dwRGBAlphaBitMask;
        info.redMask = format->dwRBitMask;
        info.greenMask = format->dwGBitMask;
        info.blueMask = format->dwBBitMask;
        info.flags = flags;
        info.fourCC = fourCC;
        info.bitsPerPixel = format->dwRGBBitCount;
        info.colorBits = PopCount(info.redMask | info.greenMask | info.blueMask);
        info.alphaBits = PopCount(info.alphaMask);
        info.formatIndex = (fourCC >> 24) - 0x28;
        ctx->formats->push_back(info);
    }
    if ((flags & 0xebe7a) == 0x40 && format->dwRGBBitCount > 8) {   // DDPF_RGB
        for (int i = 8; i >= 0; --i) {
            const FormatRow& row = kFormatTable[i];
            if (format->dwRGBBitCount == row.bitsPerPixel && format->dwRGBAlphaBitMask == row.alphaMask &&
                format->dwRBitMask == row.redMask && format->dwGBitMask == row.greenMask &&
                format->dwBBitMask == row.blueMask) {
                cDXTextureFormatInfo info;
                info.alphaMask = format->dwRGBAlphaBitMask;
                info.redMask = format->dwRBitMask;
                info.greenMask = format->dwGBitMask;
                info.blueMask = format->dwBBitMask;
                info.flags = flags;
                info.fourCC = format->dwFourCC;
                info.bitsPerPixel = format->dwRGBBitCount;
                info.colorBits = PopCount(info.redMask | info.greenMask | info.blueMask);
                info.alphaBits = PopCount(info.alphaMask);
                info.formatIndex = static_cast<std::uint32_t>(i);
                ctx->formats->push_back(info);
                break;
            }
        }
    }
    return 1;
}

bool IsIdentityMatrix(const float* m) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) {
        // The original tests `v == expected && !NAN(v)`, i.e. exact equality plus a NaN guard.
        if (!(m[i] == kIdentity[i]) || std::isnan(m[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Texture-stage adapter: cTextureStageData translates the record through one original call per state change.

class cGDriver::TextureStageHost final : public ITextureStageHost {
public:
    explicit TextureStageHost(cGDriver& driver) : driver_(driver) {}
    bool IsTextureUsable(std::uint32_t textureId) override {
        return driver_.textureManager_.HasTexture(textureId);
    }
    void BindTexture(int stage, std::uint32_t textureId) override {
        if (ID3D7Device* device = driver_.environment_.Device()) device->SetTexture(stage, textureId);
    }
    void SetTextureStageState(int stage, int state, int value) override {
        if (ID3D7Device* device = driver_.environment_.Device())
            device->SetTextureStageState(static_cast<std::uint32_t>(stage), static_cast<std::uint32_t>(state),
                                          static_cast<std::uint32_t>(value));
    }

private:
    cGDriver& driver_;
};

// ---------------------------------------------------------------------------------------------------------------------
// Construction / lifetime

cGDriver::cGDriver(ID3D7Environment& environment, gd::IModeSource& modeSource, IPrimitiveManagerHost& primitiveManager,
                   ITextureManagerHost& textureManager, IDriverGL& gl)
    : environment_(environment),
      modes_(&modeSource),
      primitiveManager_(primitiveManager),
      textureManager_(textureManager),
      gl_(gl) {
    // +0x23c, +0x184, +0x1e8, +0x2d8, +0x2dc, +0x2eb, +0x300 and the +0x380 flag bytes, as the constructor sets them.
    currentMode = sGDMode{};
    currentMode.id = -1;
    reserved184 = 7;
    clearStencil = 0;
    currentBitsPerPixel = 0xffffffffu;
    vsync = true;
    windowed = false;
    clearColor = 0;
    clearDepth = 1.0f;
    currentStage = 0;
    stageCount = 0;
    stagesDirty = false;
    textureParamsValid = false;
    unpackRowLength = 0;
    matrixTarget = 0;
    lightingEnabled = false;
    clearOnBeginScene = false;
    vertexColorEnabled = true;
    vertexAlphaEnabled = true;
}

cGDriver::~cGDriver() {
    bufferRegions_.clear();
    stages.clear();
    lights.clear();
}

cGDriver* cGDriver::Create(ID3D7Environment& environment, gd::IModeSource& modeSource,
                           IPrimitiveManagerHost& primitiveManager, ITextureManagerHost& textureManager, IDriverGL& gl) {
    return new cGDriver(environment, modeSource, primitiveManager, textureManager, gl);
}

bool cGDriver::QueryInterface(std::uint32_t iid, void** out) {
    // PARTIAL: the original is a four-vtable COM object; here one C++ class plays all four roles, and the primitive-manager
    // query returns the seam (which has no AddRef of its own).
    if (out == nullptr) return false;
    // The IIDs the original answers (0x10060401e).
    switch (iid) {
        case 0x87e2b87d:   // lighting extension (object +0x10)
            *out = static_cast<IGDDriverLighting*>(this);
            break;
        case 0xe69bfe2a:   // colour-buffer extension (object +0x8)
            *out = static_cast<IGDDriverColorBuffer*>(this);
            break;
        case 1:            // cIGZGDriver (object +0x18)
        case 0xa4554849:
            *out = static_cast<gd::cIGZGDriver*>(this);
            break;
        case 0x669565fe:   // core object (object +0x0)
            *out = static_cast<IGDDriverBufferRegions*>(this);
            break;
        case 0x9cd86f9: {  // the primitive manager, returned as-is
            // The original returns the cPrimitiveManager and AddRefs it (its slot +8). The seam has no refcount, so the
            // pointer is returned without the AddRef.
            *out = &primitiveManager_;
            return true;
        }
        default:
            return false;
    }
    ++refCount_;
    return true;
}

void cGDriver::AddRef() { ++refCount_; }

std::int32_t cGDriver::Release() {
    if (--refCount_ == 0) return FinalRelease();
    return refCount_;
}

bool cGDriver::FinalRelease() {
    Shutdown();          // core-vtable slot 14 (+0x70)
    delete this;         // deleting destructor, core-vtable slot 12 (+0x60)
    return false;
}

bool cGDriver::Init() {
    // PARTIAL: the real work is InitViewport, which needs the cISGLDX7D3DX context-info seam.
    error = 0;   // +0x158
    if (initialized) return true;
    initialized = true;
    currentStage = 0;
    stageCount = 0;      // the original clears +0x15c (current stage) and +0x160 (stage count) together
    reserved184 = 7;
    modeIndex = -1;
    InitViewport();
    return true;
}

bool cGDriver::Shutdown() {
    // PARTIAL: tears down the viewport created by the partial InitViewport path.
    if (!initialized) return true;
    initialized = false;
    ShutdownViewport();
    return true;
}

bool cGDriver::IsDeviceReady() { return !deviceLost; }

// ---------------------------------------------------------------------------------------------------------------------
// Capability helpers

std::uint16_t cGDriver::CapU16(std::size_t offset) const {
    std::uint16_t v = 0;
    if (offset + 2 <= caps::kDeviceDescSize) std::memcpy(&v, deviceDescription + offset, 2);
    return v;
}

std::uint32_t cGDriver::CapU32(std::size_t offset) const {
    std::uint32_t v = 0;
    if (offset + 4 <= caps::kDeviceDescSize) std::memcpy(&v, deviceDescription + offset, 4);
    return v;
}

bool cGDriver::HasStencilBuffer() const {
    // The driver reads currentMode byte +0x21, which GetDeviceProperties fills from the device description.
    return reinterpret_cast<const std::uint8_t*>(&currentMode)[0x21] != 0;
}

bool cGDriver::HasFog() const { return (deviceDescription[caps::kFog] & 4) != 0; }

bool cGDriver::HasCombine() const {
    // currentMode byte +0x29, set to 1 by GetDeviceProperties.
    return reinterpret_cast<const std::uint8_t*>(&currentMode)[0x29] != 0;
}

std::uint32_t cGDriver::StageCountFromCaps() const { return CapU16(caps::kStageCount); }

void cGDriver::MarkStageDirty(std::uint32_t stage) {
    if (stage < stages.size()) stages[stage].dirty = 1;
    stagesDirty = true;
}

}  // namespace nSGLDX7

namespace nSGLDX7 {

// ---------------------------------------------------------------------------------------------------------------------
// Primitive submission (index generation + cPrimitiveManager queueing)

void cGDriver::DrawArrays(eGDPrimType mode, int first, int count) {
    if (!primitiveManager_.IsStarted()) return;
    if (stagesDirty) {
        primitiveManager_.Flush();
        TextureStageHost host(*this);
        for (std::uint32_t s = 0; s < stageCount && s < stages.size(); ++s) {
            if (!stages[s].dirty) continue;
            stages[s].ApplyState(static_cast<int>(s), host);
        }
        stagesDirty = false;
    }
    if (count < 1) return;
    if (mode > 7) return;
    const std::uint32_t format = vertexFormat;
    const std::uint32_t stride = vertexStride;
    const auto* data = static_cast<const std::uint8_t*>(vertexData) + static_cast<std::ptrdiff_t>(first) * stride;
    std::uint16_t* indices = nullptr;
    std::uint32_t indexCount = 0;
    std::uint32_t prim = lookup::kPrimitiveType[mode];
    switch (mode) {
        case 1:   // triangle strip
            indexCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            indices = primitiveManager_.GetIndices(static_cast<int>(indexCount));
            GenerateIndexedTriListForTriangleStrip(count, indices);
            prim = 4;
            break;
        case 2:   // triangle fan
            indexCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            indices = primitiveManager_.GetIndices(static_cast<int>(indexCount));
            GenerateIndexedTriListForTriangleFan(count, indices);
            prim = 4;
            break;
        case 6:   // quads
            indexCount = (static_cast<std::uint32_t>(count) >> 1) * 3;
            indices = primitiveManager_.GetIndices(static_cast<int>(indexCount & 0xffffu));
            indexCount &= 0xffffu;
            GenerateIndexedTriListForQuads(count, indices);
            prim = 4;
            break;
        case 7:   // quad strip
            indexCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            indices = primitiveManager_.GetIndices(static_cast<int>(indexCount));
            GenerateIndexedTriListForQuadStrip(count, indices);
            prim = 4;
            break;
        default:
            break;
    }
    const long result = primitiveManager_.Queue(format, prim, stride, 0, count, data, indexCount, indices);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::DrawElements(eGDPrimType mode, int count, eGDType indexType, const void* indicesIn) {
    if (!primitiveManager_.IsStarted()) return;
    if (stagesDirty) {
        primitiveManager_.Flush();
        TextureStageHost host(*this);
        for (std::uint32_t s = 0; s < stageCount && s < stages.size(); ++s) {
            if (!stages[s].dirty) continue;
            stages[s].ApplyState(static_cast<int>(s), host);
        }
        stagesDirty = false;
    }
    if (count < 1) return;
    if (mode > 7) return;
    const std::uint32_t format = vertexFormat;
    const std::uint32_t stride = vertexStride;
    const void* data = vertexData;
    std::uint16_t* out = nullptr;
    std::uint32_t outCount = 0;
    std::uint32_t lo = 0, hi = 0;
    std::uint32_t prim = lookup::kPrimitiveType[mode];
    switch (mode) {
        case 1: {   // indexed triangle strip
            outCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            out = primitiveManager_.GetIndices(static_cast<int>(outCount));
            switch (indexType) {
                case 1: GenerateIndexedTriangleStrip(count, static_cast<const std::uint8_t*>(indicesIn), out, &lo, &hi); break;
                case 5: GenerateIndexedTriangleStrip(count, static_cast<const std::uint32_t*>(indicesIn), out, &lo, &hi); break;
                default: GenerateIndexedTriangleStrip(count, static_cast<const std::uint16_t*>(indicesIn), out, &lo, &hi); break;
            }
            prim = 4;
            break;
        }
        case 2: {   // indexed triangle fan
            outCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            out = primitiveManager_.GetIndices(static_cast<int>(outCount));
            switch (indexType) {
                case 1: GenerateIndexedTriangleFan(count, static_cast<const std::uint8_t*>(indicesIn), out, &lo, &hi); break;
                case 5: GenerateIndexedTriangleFan(count, static_cast<const std::uint32_t*>(indicesIn), out, &lo, &hi); break;
                default: GenerateIndexedTriangleFan(count, static_cast<const std::uint16_t*>(indicesIn), out, &lo, &hi); break;
            }
            prim = 4;
            break;
        }
        case 6: {   // indexed quads
            const std::uint32_t n = (static_cast<std::uint32_t>(count) >> 1) * 3;
            outCount = n & 0xffffu;
            out = primitiveManager_.GetIndices(static_cast<int>(outCount));
            switch (indexType) {
                case 1: GenerateIndexedTriListForIndexedQuads8(count, static_cast<const std::uint8_t*>(indicesIn), out, &lo, &hi); break;
                case 5: GenerateIndexedTriListForIndexedQuads32(count, static_cast<const std::uint32_t*>(indicesIn), out, &lo, &hi); break;
                default: GenerateIndexedTriListForIndexedQuads16(count, static_cast<const std::uint16_t*>(indicesIn), out, &lo, &hi); break;
            }
            prim = 4;
            break;
        }
        case 7: {   // indexed quad strip
            outCount = static_cast<std::uint32_t>(count * 3 - 6) & 0xffffu;
            out = primitiveManager_.GetIndices(static_cast<int>(outCount));
            switch (indexType) {
                case 1: GenerateIndexedTriListForIndexedQuadStrip8(count, static_cast<const std::uint8_t*>(indicesIn), out, &lo, &hi); break;
                case 5: GenerateIndexedTriListForIndexedQuadStrip32(count, static_cast<const std::uint32_t*>(indicesIn), out, &lo, &hi); break;
                default: GenerateIndexedTriListForIndexedQuadStrip16(count, static_cast<const std::uint16_t*>(indicesIn), out, &lo, &hi); break;
            }
            prim = 4;
            break;
        }
        default: {
            out = primitiveManager_.GetIndices(count);
            outCount = static_cast<std::uint32_t>(count);
            switch (indexType) {
                case 1: ConvertUint8IndicesToUint16(count, indicesIn, out, &lo, &hi); break;
                case 5: ConvertUint32IndicesToUint16(count, indicesIn, out, &lo, &hi); break;
                default: ConvertUint16IndicesToUint16(count, indicesIn, out, &lo, &hi); break;
            }
            break;
        }
    }
    const std::uint32_t firstVertex = lo;
    const std::int32_t vertexCount = static_cast<std::int32_t>((1 - lo) + hi);
    const long result = primitiveManager_.Queue(format, prim, stride, firstVertex, vertexCount, data, outCount, out);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::InterleavedArrays(eGDVertexFormat format, int stride, const void* data) {
    if (!interleavedSet) interleavedSet = true;
    vertexStride = static_cast<std::uint32_t>(stride);
    standardStride = static_cast<std::uint32_t>(nRZSimGL::StandardVFStride(format));
    if (stride == 0) vertexStride = standardStride;
    vertexFormat = format;
    vertexData = data;
}

eGDVertexFormat cGDriver::MakeVertexFormat(std::uint32_t count, eGDElementType* elements) {
    return nRZSimGL::MakeStandardVertexFormat(count, elements);
}

eGDVertexFormat cGDriver::MakeVertexFormat(eGDVertexFormat format) {
    return static_cast<eGDVertexFormat>(nRZSimGL::PackStandardVertexFormat(static_cast<int>(format)));
}

int cGDriver::VertexFormatStride(eGDVertexFormat format) const {
    return nRZSimGL::StandardVFStride(format);
}

int cGDriver::VertexFormatElementOffset(eGDVertexFormat format, eGDElementType element, std::uint32_t index) const {
    return nRZSimGL::StandardVFElementOffset(format, element, static_cast<int>(index));
}

int cGDriver::VertexFormatNumElements(eGDVertexFormat format, eGDElementType element) const {
    return static_cast<int>(nRZSimGL::StandardVFNumElements(format, element));
}

// ---------------------------------------------------------------------------------------------------------------------
// Clear / fixed-function state

void cGDriver::Clear(std::uint32_t mask) {
    primitiveManager_.Flush();
    std::uint32_t flags = (mask >> 11) & 2;   // depth/back buffer bit
    if (HasStencilBuffer()) flags = (mask >> 11) & 6;
    flags |= (mask >> 14) & 1;                // colour bit comes from a different position in the shim's mask
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->Clear(nullptr, flags, clearColor, clearDepth, static_cast<std::uint32_t>(clearStencil));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::ClearColor(float r, float g, float b, float a) { clearColor = PackUnitColor(r, g, b, a); }

void cGDriver::ClearDepth(double depth) { clearDepth = static_cast<float>(depth); }

void cGDriver::ClearStencil(int value) { clearStencil = value; }

void cGDriver::ColorMask(bool enable) {
    // Quirk kept: the shipped driver only flushes here and never forwards the colour mask to the device.
    (void)enable;
    primitiveManager_.Flush();
}

void cGDriver::DepthFunc(eGDTestFunc func) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::ZFunc, lookup::kCompareFunction[func & 7]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::DepthMask(bool enable) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::ZWriteEnable, enable ? 1 : 0);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::StencilFunc(eGDTestFunc func, int ref, std::uint32_t mask) {
    if (!HasStencilBuffer()) {
        error = 3;
        return;
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    long result = device->SetRenderState(rs::StencilMask, mask);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
        return;
    }
    result = device->SetRenderState(rs::StencilFunc, lookup::kCompareFunction[func & 7]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
        return;
    }
    result = device->SetRenderState(rs::StencilRef, static_cast<std::uint32_t>(ref));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::StencilMask(std::uint32_t mask) {
    if (!HasStencilBuffer()) {
        error = 3;
        return;
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::StencilWriteMask, mask);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::StencilOp(eGDStencilOp fail, eGDStencilOp zfail, eGDStencilOp zpass) {
    if (!HasStencilBuffer()) {
        error = 3;
        return;
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    long result = device->SetRenderState(rs::StencilFail, lookup::kStencilOperation[fail % 6]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
        return;
    }
    result = device->SetRenderState(rs::StencilZFail, lookup::kStencilOperation[zfail % 6]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
        return;
    }
    result = device->SetRenderState(rs::StencilPass, lookup::kStencilOperation[zpass % 6]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::BlendFunc(eGDBlend src, eGDBlend dst) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    device->SetRenderState(rs::SrcBlend, lookup::kBlendMode[src % 11]);
    const long result = device->SetRenderState(rs::DestBlend, lookup::kBlendMode[dst % 11]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::AlphaFunc(eGDTestFunc func, float ref) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    device->SetRenderState(rs::AlphaFunc, lookup::kCompareFunction[func & 7]);
    const long result = device->SetRenderState(rs::AlphaRef, gd::AlphaRefByte(ref));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::ShadeModel(eGDShade model) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::ShadeMode, lookup::kShadeModel[model & 1]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::PolygonOffset(std::uint32_t units) {
    long result = primitiveManager_.Flush();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    result = device->SetRenderState(rs::ZBias, units);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

bool cGDriver::Punt(std::uint32_t code, void* data) {
    (void)code;
    (void)data;
    return false;   // the shipped driver does not implement Punt
}

// The blit entry points are stubs in the shipped driver: they only record "invalid call" (error 3).
void cGDriver::BitBlt(int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*) { error = 3; }
void cGDriver::StretchBlt(int, int, int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*) { error = 3; }
void cGDriver::BitBltAlpha(int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*, std::uint32_t) { error = 3; }
void cGDriver::StretchBltAlpha(int, int, int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*, std::uint32_t) { error = 3; }
void cGDriver::BitBltAlphaModulate(int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*, std::uint32_t) { error = 3; }
void cGDriver::StretchBltAlphaModulate(int, int, int, int, int, int, eGDTextureFormat, eGDType, const void*, bool, const void*, std::uint32_t) { error = 3; }

void cGDriver::GetBoolean(eGDParameter pname, bool* out) {
    (void)pname;
    (void)out;   // no-op in the shipped driver
}

void cGDriver::GetInteger(eGDParameter pname, int* out) {
    (void)pname;
    if (out) *out = static_cast<int>(unpackRowLength);
}

void cGDriver::GetFloat(eGDParameter pname, float* out) {
    (void)pname;
    (void)out;   // no-op in the shipped driver
}

int cGDriver::GetError() {
    const int value = error;
    error = 0;
    return value;
}

void cGDriver::MatrixMode(eGDMatrixTarget target) { matrixTarget = target; }

void cGDriver::LoadMatrix(const float* matrix) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    std::uint32_t state;
    if (matrixTarget == 1) {
        state = 3;   // projection
    } else if (matrixTarget == 0) {
        state = 1;   // view
    } else {
        return;
    }
    device->SetTransform(state, matrix);
}

void cGDriver::LoadIdentity() {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    LoadMatrix(kIdentity);
}

void cGDriver::Enable(eGDDriverState state) {
    switch (state) {
        case 3:   // cull face
            primitiveManager_.Flush();
            if (ID3D7Device* device = environment_.Device())
                device->SetRenderState(rs::CullMode, 2);
            return;
        case 5:   // texturing
            MarkStageDirty(currentStage);
            if (currentStage < stages.size()) stages[currentStage].textureEnabled = 1;
            return;
        case 6:   // fog
            if (!HasFog()) return;
            primitiveManager_.Flush();
            if (ID3D7Device* device = environment_.Device()) device->SetRenderState(rs::FogEnable, 1);
            return;
        case 7:
            return;
        default:
            primitiveManager_.Flush();
            if (ID3D7Device* device = environment_.Device())
                device->SetRenderState(lookup::kDriverMode[state % 6], 1);
            return;
    }
}

void cGDriver::Disable(eGDDriverState state) {
    switch (state) {
        case 3: {   // quirk: disabling culling selects clockwise culling, not "none"
            primitiveManager_.Flush();
            ID3D7Device* device = environment_.Device();
            if (device) {
                const long result = device->SetRenderState(rs::CullMode, 1);
                if (result == kDeviceLost) {
                    error = 0;
                } else if (result != 0) {
                    error = 1;
                }
            }
            return;
        }
        case 5:
            MarkStageDirty(currentStage);
            if (currentStage < stages.size()) stages[currentStage].textureEnabled = 0;
            return;
        case 6:
            if (!HasFog()) return;
            primitiveManager_.Flush();
            if (ID3D7Device* device = environment_.Device()) {
                const long result = device->SetRenderState(rs::FogEnable, 0);
                if (result == kDeviceLost) {
                    error = 0;
                } else if (result != 0) {
                    error = 1;
                }
            }
            return;
        case 7:
            return;
        default: {
            primitiveManager_.Flush();
            ID3D7Device* device = environment_.Device();
            if (!device) return;
            const long result = device->SetRenderState(lookup::kDriverMode[state % 6], 0);
            if (result == kDeviceLost) {
                error = 0;
            } else if (result != 0) {
                error = 1;
            }
            return;
        }
    }
}

bool cGDriver::IsEnabled(eGDDriverState state) {
    if (deviceLost) return false;
    if (state == 7) return true;
    if (state == 5) {
        return currentStage < stages.size() && stages[currentStage].textureEnabled != 0;
    }
    ID3D7Device* device = environment_.Device();
    if (!device) return false;
    std::uint32_t value = 0;
    const std::uint32_t which = (state == 3) ? rs::CullMode : lookup::kDriverMode[state % 6];
    device->GetRenderState(which, &value);
    return value == 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and texture stages

void cGDriver::BindTexture(eGDTextureTarget target, std::uint32_t texture) {
    if (target != 0) {
        error = 1;   // only 2D textures
        return;
    }
    textureParamsValid = true;
    sGDTextureParams params;
    if (!textureManager_.GetTextureParams(texture, params)) textureManager_.GetTextureParams(0, params);
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    stagesDirty = true;
    stage.dirty = 1;
    stage.textureEnabled = 1;
    stage.appliedEnvMode = 0xffffffffu;
    stage.magFilter = params.magFilter;
    stage.minFilter = params.minFilter;
    stage.wrapS = params.wrapS;
    stage.wrapT = params.wrapT;
    stage.textureId = texture;
    stage.textureHasAlpha = textureManager_.HasAlpha(texture) ? 1 : 0;
}

void cGDriver::SetTexture(std::uint32_t texture, std::uint32_t stageIndex) {
    if (stageIndex >= stages.size()) stages.resize(stageIndex + 1);
    cTextureStageData& stage = stages[stageIndex];
    if (stage.textureId == texture) return;
    textureParamsValid = false;
    stagesDirty = true;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    stage.textureId = texture;
    stage.textureHasAlpha = textureManager_.HasAlpha(texture) ? 1 : 0;
}

std::uint32_t cGDriver::GetTexture(std::uint32_t stageIndex) const {
    return stageIndex < stages.size() ? stages[stageIndex].textureId : 0;
}

std::uint32_t cGDriver::CreateTexture(eGDInternalTextureFormat format, std::uint32_t a, std::uint32_t b,
                                      std::uint32_t c, eGDTextureHintFlags hint) {
    const std::uint32_t handle = textureManager_.NewTexture();
    if (!textureManager_.CreateTexture(handle, hint, format, a, b, c)) error = 3;
    return handle;
}

void cGDriver::LoadTextureLevel(std::uint32_t texture, std::uint32_t level, std::uint32_t x, std::uint32_t y,
                                std::uint32_t width, std::uint32_t height, eGDTextureFormat format, eGDType type,
                                std::uint32_t rowLength, const void* pixels) {
    primitiveManager_.Flush();
    if (!textureManager_.LoadTextureLevel(texture, level, x, y, width, height, format, type, rowLength, pixels))
        error = 3;
}

void cGDriver::TexImage2D(eGDTextureTarget target, int level, eGDInternalTextureFormat internalFormat, int width,
                          int height, int border, eGDTextureFormat format, eGDType type, const void* pixels) {
    (void)target;
    (void)border;
    if (width <= 0 || height <= 0) {
        error = 2;
        return;
    }
    if (currentStage >= stages.size() || stages[currentStage].textureId == 0) {
        error = 2;
        return;
    }
    const std::uint32_t texture = stages[currentStage].textureId;
    std::uint32_t rowLength = unpackRowLength;
    if (rowLength == 0) rowLength = static_cast<std::uint32_t>(width);
    if (!textureManager_.HasTexture(texture)) {
        if (!textureManager_.CreateTexture(texture, textureManager_.TextureHint(texture), internalFormat,
                                           static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1)) {
            error = 2;
            return;
        }
    }
    if (!textureManager_.LoadTextureLevel(texture, static_cast<std::uint32_t>(level), 0, 0,
                                          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                          format, type, rowLength, pixels)) {
        error = 3;
    }
}

void cGDriver::PixelStore(eGDParameter pname, int value) {
    if (pname == 0) unpackRowLength = static_cast<std::uint32_t>(value);
}

void cGDriver::TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, eGDTextureEnvModeParam mode) {
    (void)target;
    (void)param;
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    stagesDirty = true;
    stages[currentStage].dirty = 1;
    stages[currentStage].envMode = mode;
}

void cGDriver::TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, const float* values) {
    (void)target;
    (void)param;
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::TextureFactor,
                                               PackUnitColor(values[0], values[1], values[2], values[3]));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::TexParameter(eGDTextureTarget target, eGDTextureParamType param, eGDTextureParam value) {
    (void)target;
    if (textureParamsValid) {
        const std::uint32_t texture = currentStage < stages.size() ? stages[currentStage].textureId : 0;
        if (!textureManager_.SetTextureParam(texture, param, value)) {
            error = 1;
            return;
        }
    }
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    std::uint32_t* dst = nullptr;
    switch (param) {
        case 0: dst = &stage.magFilter; break;
        case 1: dst = &stage.minFilter; break;
        case 2: dst = &stage.wrapS; break;
        case 3: dst = &stage.wrapT; break;
        default: break;
    }
    if (dst) {
        if (*dst == value) return;
        *dst = value;
    }
    stagesDirty = true;
    stage.dirty = 1;
}

void cGDriver::SetCombiner(const cGDCombiner& combiner, std::uint32_t stageIndex) {
    primitiveManager_.Flush();
    if (stageIndex >= stages.size()) stages.resize(stageIndex + 1);
    cTextureStageData& stage = stages[stageIndex];
    stage.envMode = 4;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    stagesDirty = true;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&combiner);
    if (raw[0] == 0xff) return;
    stage.rgbCombine = raw[0];
    stage.alphaCombine = raw[4];
    stage.rgbSource[0] = raw[1] & 0xf;
    stage.rgbSource[1] = raw[2] & 0xf;
    stage.rgbSource[2] = raw[3] & 0xf;
    stage.alphaSource[0] = raw[5] & 0xf;
    stage.alphaSource[1] = raw[6] & 0xf;
    stage.alphaSource[2] = raw[7] & 0xf;
    // The original indexes the 4-entry tables with the operand nibble (0..3; other values read adjacent constants).
    stage.rgbOperand[0] = lookup::kColorOperand[(raw[1] >> 4) & 3];
    stage.rgbOperand[1] = lookup::kColorOperand[(raw[2] >> 4) & 3];
    stage.rgbOperand[2] = lookup::kColorOperand[(raw[3] >> 4) & 3];
    stage.alphaOperand[0] = lookup::kAlphaOperand[(raw[5] >> 4) & 3];
    stage.alphaOperand[1] = lookup::kAlphaOperand[(raw[6] >> 4) & 3];
    stage.alphaOperand[2] = lookup::kAlphaOperand[(raw[7] >> 4) & 3];
    stage.rgbScale = raw[8];
    stage.alphaScale = raw[9];
}

void cGDriver::TexStage(std::uint32_t stageIndex) {
    if (stageIndex < stageCount) {
        currentStage = stageIndex;
    } else {
        error = 2;
    }
}

void cGDriver::TexStageCoord(eGDTexCoordSource source) {
    const std::uint32_t index = ((source & 8) << 28) | (source >> 4);
    if (index > 3) {
        error = 2;
        return;
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    // D3DTSS_TEXCOORDINDEX: source-space flag from the table, low 3 bits are the coordinate set.
    const std::uint32_t value = lookup::kTexCoordSource[index] | (source & 7);
    const long result = device->SetTextureStageState(currentStage, 0xb, value);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::TexStageMatrix(const float* matrix, std::uint32_t a, std::uint32_t b, eGDTexMatFlags flags) {
    std::int32_t result = static_cast<std::int32_t>(primitiveManager_.Flush());
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    if (matrix == nullptr || IsIdentityMatrix(matrix)) {
        device->SetTextureStageState(currentStage, 0x18, 0);
        return;
    }
    (void)a;
    device->SetTextureStageState(currentStage, 0x18, ((flags & 2) << 7) | b);
    device->SetTransform(currentStage + 0x10, matrix);
}

void cGDriver::TexStageCombine(eGDTextureStageCombineParamType param, eGDTextureStageCombineModeParam mode) {
    if (!HasCombine()) {
        error = 2;
        return;
    }
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    stagesDirty = true;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    switch (static_cast<std::uint32_t>(param)) {
        case 0: stage.rgbCombine = mode; break;
        case 1: stage.alphaCombine = mode; break;
        default: break;
    }
}

void cGDriver::TexStageCombine(eGDTextureStageCombineSourceParamType param,
                               eGDTextureStageCombineSourceParam source) {
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    stagesDirty = true;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    switch (static_cast<std::uint32_t>(param)) {
        case 0: stage.rgbSource[0] = source; break;
        case 1: stage.rgbSource[1] = source; break;
        case 2: stage.rgbSource[2] = source; break;
        case 4: stage.alphaSource[0] = source; break;
        case 5: stage.alphaSource[1] = source; break;
        case 6: stage.alphaSource[2] = source; break;
        default: break;
    }
}

void cGDriver::TexStageCombine(eGDTextureStageCombineOperandParamType param, eGDBlend operand) {
    if (!HasCombine()) {
        error = 2;
        return;
    }
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    stagesDirty = true;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    switch (static_cast<std::uint32_t>(param)) {
        case 0: stage.rgbOperand[0] = operand; break;
        case 1: stage.rgbOperand[1] = operand; break;
        case 2: stage.rgbOperand[2] = operand; break;
        case 4: stage.alphaOperand[0] = operand; break;
        case 5: stage.alphaOperand[1] = operand; break;
        case 6: stage.alphaOperand[2] = operand; break;
        default: break;
    }
}

void cGDriver::TexStageCombine(eGDTextureStageCombineScaleParamType param, eGDTextureStageCombineScaleParam scale) {
    if (!HasCombine()) {
        error = 2;
        return;
    }
    if (currentStage >= stages.size()) stages.resize(currentStage + 1);
    cTextureStageData& stage = stages[currentStage];
    stagesDirty = true;
    stage.dirty = 1;
    stage.appliedEnvMode = 0xffffffffu;
    switch (static_cast<std::uint32_t>(param)) {
        case 0: stage.rgbScale = scale; break;
        case 1: stage.alphaScale = scale; break;
        default: break;
    }
}

void cGDriver::EnableVertexColors(bool color, bool alpha) {
    primitiveManager_.Flush();
    vertexColorEnabled = color;
    vertexAlphaEnabled = alpha;
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    device->SetRenderState(rs::ColorVertex, 1);
    device->SetRenderState(rs::AmbientMaterialSource, color ? 1 : 0);
    device->SetRenderState(rs::DiffuseMaterialSource, alpha ? 1 : 0);
}

void cGDriver::GenTextures(int count, std::uint32_t* textures) {
    for (int i = 0; i < count; ++i) textures[i] = textureManager_.NewTexture();
}

void cGDriver::DeleteTextures(int count, const std::uint32_t* textures) {
    if (!texturesReady || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        if (textures[i] != 0) textureManager_.FreeTexture(textures[i]);
    }
}

bool cGDriver::IsTexture(std::uint32_t texture) { return textureManager_.IsValid(texture); }

void cGDriver::PrioritizeTextures(int count, const std::uint32_t* textures, const float* priorities) {
    for (int i = 0; i < count; ++i) textureManager_.SetPriority(textures[i], priorities[i]);
}

bool cGDriver::AreTexturesResident(int count, const std::uint32_t* textures, bool* residence) {
    // Quirk kept: the original advances the residence pointer before writing, so it skips residence[0].
    for (int i = 0; i < count; ++i) {
        residence = residence + 1;
        residence[0] = textureManager_.IsResident(textures[i]);
        textures = textures + 1;
    }
    return true;
}

void cGDriver::ApplyTextureState() {
    if (!stagesDirty) return;
    TextureStageHost host(*this);
    for (std::uint32_t s = 0; s < stageCount && s < stages.size(); ++s) {
        if (!stages[s].dirty) continue;
        stages[s].ApplyState(static_cast<int>(s), host);
    }
    stagesDirty = false;
}

}  // namespace nSGLDX7

namespace nSGLDX7 {
namespace {
// Device-reset result codes the shim distinguishes (see BeginScene/Flush).
constexpr std::int32_t kDeviceNotReset = static_cast<std::int32_t>(0x887602f8u);
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Scene and fog

bool cGDriver::BeginScene() {
    ID3D7Device* device = environment_.Device();
    if (inScene || !device) return false;
    error = 0;
    const long result = device->BeginScene();
    if (result == kDeviceLost) {
        error = 0;
        if (result > -1 || result == kDeviceNotReset) inScene = true;
    } else if (result != 0) {
        error = 1;
    } else {
        inScene = true;
    }
    // BeginScene resets every texture-stage record to its default-constructed value.
    std::fill(stages.begin(), stages.end(), cTextureStageData{});
    if (clearOnBeginScene) Clear(0x4000);
    return inScene;
}

void cGDriver::EndScene() {
    if (!inScene) return;
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->EndScene();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    inScene = false;
}

void cGDriver::Fog(eGDFogParamType param, eGDFogParam value) {
    primitiveManager_.Flush();
    if (!HasFog()) {
        error = 3;
        return;
    }
    if (static_cast<std::uint32_t>(param) - 1u < 4u) {
        error = 1;   // only the table mode (param 0) exists in the shim
        return;
    }
    if (param != 0) return;
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const std::uint32_t lookupIndex = value < 3 ? value : 0;
    const long result = device->SetRenderState(rs::FogTableMode, lookup::kFogMode[lookupIndex]);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::Fog(eGDFogParamType param, const float* values) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    long result;
    switch (param) {
        case 0:
            error = 1;
            return;
        case 1:
            result = device->SetRenderState(rs::FogColor,
                                            PackUnitColor(values[0], values[1], values[2], values[3]));
            break;
        case 2:
            result = device->SetRenderState(rs::FogDensity, FloatBits(values[0]));
            break;
        case 3:
            result = device->SetRenderState(rs::FogStart, FloatBits(values[0]));
            break;
        case 4:
            result = device->SetRenderState(rs::FogEnd, FloatBits(values[0]));
            break;
        default:
            return;
    }
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Material / lighting

void cGDriver::ColorMultiplier(float r, float g, float b) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    if (!lightingEnabled) {
        lightingEnabled = true;
        const long result = device->SetRenderState(rs::Lighting, 1);
        if (result == kDeviceLost) {
            error = 0;
        } else if (result != 0) {
            error = 1;
        }
    }
    material.ambient[0] = 1.0f;
    material.ambient[1] = 1.0f;
    material.ambient[2] = 1.0f;
    device->SetMaterial(material);
    const long result = device->SetRenderState(rs::Ambient, PackUnitColor(r, g, b, 1.0f));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::AlphaMultiplier(float alpha) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    if (!lightingEnabled) {
        lightingEnabled = true;
        device->SetRenderState(rs::Lighting, 1);
    }
    material.diffuse[3] = alpha;
    long result = device->SetMaterial(material);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    if (alpha >= 1.0f) {
        if (!vertexAlphaEnabled) return;
        result = device->SetRenderState(rs::DiffuseMaterialSource, 1);
    } else {
        result = device->SetRenderState(rs::DiffuseMaterialSource, 0);
    }
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::EnableLighting(bool enable) {
    if (lightingEnabled == enable) return;
    lightingEnabled = enable;
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::Lighting, enable ? 1 : 0);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::EnableLight(std::uint32_t index, bool enable) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->LightEnable(index, enable);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::LightModelAmbient(float r, float g, float b, float a) {
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetRenderState(rs::Ambient, PackUnitColor(r, g, b, a));
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::LightColor(std::uint32_t index, std::uint32_t component, const float* color) {
    if (lights.size() <= index) lights.resize(index + 1);
    sGDLight& light = lights[index];
    float* dst = nullptr;
    switch (component) {
        case 0: dst = light.ambient; break;
        case 1: dst = light.diffuse; break;
        case 2: dst = light.specular; break;
        default: break;
    }
    if (dst) {
        dst[0] = color[0];
        dst[1] = color[1];
        dst[2] = color[2];
        dst[3] = color[3];
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetLight(index, light);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::LightColor(std::uint32_t index, const float* ambient, const float* diffuse, const float* specular) {
    if (lights.size() <= index) lights.resize(index + 1);
    sGDLight& light = lights[index];
    if (ambient) std::memcpy(light.ambient, ambient, sizeof(light.ambient));
    if (diffuse) std::memcpy(light.diffuse, diffuse, sizeof(light.diffuse));
    if (specular) std::memcpy(light.specular, specular, sizeof(light.specular));
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetLight(index, light);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::LightPosition(std::uint32_t index, const float* position) {
    (void)index;
    (void)position;   // no-op in the shipped driver
}

void cGDriver::LightDirection(std::uint32_t index, const float* direction) {
    (void)index;
    (void)direction;   // no-op in the shipped driver
}

void cGDriver::MaterialColor(std::uint32_t component, const float* color) {
    switch (component) {
        case 0: std::memcpy(material.ambient, color, sizeof(material.ambient)); break;
        case 1: std::memcpy(material.diffuse, color, sizeof(material.diffuse)); break;
        case 2: std::memcpy(material.specular, color, sizeof(material.specular)); break;
        case 3: std::memcpy(material.emissive, color, sizeof(material.emissive)); break;
        case 4: material.power = color[0]; break;
        default: break;
    }
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetMaterial(material);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

void cGDriver::MaterialColor(const float* ambient, const float* diffuse, const float* specular, const float* emissive,
                             float power) {
    if (ambient) std::memcpy(material.ambient, ambient, sizeof(material.ambient));
    if (diffuse) std::memcpy(material.diffuse, diffuse, sizeof(material.diffuse));
    if (specular) std::memcpy(material.specular, specular, sizeof(material.specular));
    if (emissive) std::memcpy(material.emissive, emissive, sizeof(material.emissive));
    if (power >= 0.0f) material.power = power;
    primitiveManager_.Flush();
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    const long result = device->SetMaterial(material);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Viewport

void cGDriver::SetViewport() {
    const long result = primitiveManager_.Flush();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    gl_.Viewport(0, 0, deviceViewport[2], deviceViewport[3]);
    gl_.Disable(0xc11);   // GL_SCISSOR_TEST
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = deviceViewport[2];
    viewport[3] = deviceViewport[3];
}

void cGDriver::SetViewport(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
    const long result = primitiveManager_.Flush();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    gl_.Viewport(x, y, width, height);
    gl_.Scissor(x, y, width, height);
    gl_.Enable(0xc11);   // GL_SCISSOR_TEST
    viewport[0] = x;
    viewport[1] = y;
    viewport[2] = width;
    viewport[3] = height;
}

void cGDriver::GetViewport(std::uint32_t out[4]) {
    out[0] = viewport[0];
    out[1] = viewport[1];
    out[2] = viewport[0] + viewport[2];
    out[3] = viewport[1] + viewport[3];
}

// ---------------------------------------------------------------------------------------------------------------------
// Video modes

int cGDriver::CountVideoModes() { return modes_.CountVideoModes(); }

bool cGDriver::GetVideoModeInfo(std::uint32_t index, sGDMode& mode) {
    if (!modes_.GetVideoModeInfo(index, mode)) {
        error = 2;
        return false;
    }
    return true;
}

bool cGDriver::GetVideoModeInfo(sGDMode& mode) {
    mode = currentMode;
    return true;
}

bool cGDriver::SetVideoMode(int mode, void* window, bool windowed, bool vsync) {
    // PARTIAL: MacDisplay::SetupDisplay / GetMainWindow are the environment seam.
    EndScene();
    if (mode == -1) {
        if (modeIndex == -1) return true;
        if (this->window == nullptr) return true;
        DestroyViewport();
        this->window = nullptr;
        modeIndex = -1;
        return true;
    }
    this->windowed = windowed;
    this->vsync = vsync;
    this->window = window;
    if (mode < 0 || static_cast<std::size_t>(mode) >= modes_.Modes().size()) {
        error = 2;
        return false;
    }
    if (modeIndex != -1) SetVideoMode(-1, nullptr, false, false);
    GetVideoModeInfo(static_cast<std::uint32_t>(mode), currentMode);
    modeIndex = mode;
    // Windowed when the mode's flag byte 0 is clear (full-screen entries carry 0x10001).
    const auto* modeBytes = reinterpret_cast<const std::uint8_t*>(&currentMode);
    this->windowed = (modeBytes[0x20] ^ 1) != 0;
    if (!environment_.SetDisplayMode(this->windowed, static_cast<std::int32_t>(currentMode.width),
                                     static_cast<std::int32_t>(currentMode.height))) {
        if (environment_.RenderTarget() != nullptr) return true;
        error = 6;
        modeIndex = -1;
        return false;
    }
    if (SetupViewport()) return true;
    DestroyViewport();
    error = 6;
    modeIndex = -1;
    return false;
}

}  // namespace nSGLDX7

namespace nSGLDX7 {
// ---------------------------------------------------------------------------------------------------------------------
// Buffer regions (cIGZGBufferRegionExtension). The original creates one off-screen surface per region through
// DirectDraw and blits between it and the frame; here the surface comes from ID3D7Environment and the blits go through
// ID3D7Surface. The DDSURFACEDESC2 capability bits the original sets are not recoverable from the decompilation (Ghidra
// dropped the call arguments), so these functions are recorded as recovered-partial.

bool cGDriver::BufferRegionEnabled() { return true; }

bool cGDriver::CanDoPartialRegionWrites() const { return true; }

bool cGDriver::CanDoOffsetReads() const { return true; }

int cGDriver::NewBufferRegion(int type) {
    // PARTIAL: the region surface is created by ID3D7Environment; the DDSURFACEDESC2 capability bits are not recoverable.
    if (deviceLost) return 0;
    std::size_t index = bufferRegions_.size();
    for (std::size_t i = 0; i < bufferRegions_.size(); ++i) {
        if (bufferRegions_[i].index == -1) {
            index = i;
            break;
        }
    }
    if (index == bufferRegions_.size()) bufferRegions_.push_back(BufferRegionData{});
    BufferRegionData& region = bufferRegions_[index];
    region = BufferRegionData{};
    region.index = -1;
    region.type = static_cast<std::uint32_t>(type);
    if (type == 0 || type == 1 || type == 2) {
        region.surface = environment_.CreateRegionSurface(type, static_cast<std::int32_t>(deviceViewport[2]),
                                                          static_cast<std::int32_t>(deviceViewport[3]));
    }
    if (region.surface == nullptr) return 0;
    region.index = static_cast<std::int32_t>(index);
    region.width = static_cast<std::int32_t>(deviceViewport[2]);
    region.height = static_cast<std::int32_t>(deviceViewport[3]);
    return static_cast<int>(index) + 1;
}

bool cGDriver::DeleteBufferRegion(std::uint32_t id) {
    // PARTIAL: the region surface belongs to the environment seam, so its Release is not repeated here.
    const std::uint32_t index = id - 1;
    if (index < bufferRegions_.size() && bufferRegions_[index].index == static_cast<std::int32_t>(index)) {
        bufferRegions_[index] = BufferRegionData{};
        return true;
    }
    return false;
}

bool cGDriver::ReadBufferRegion(std::uint32_t id, int x, int y, int width, int height, int dx, int dy) {
    // PARTIAL: the IDirectDrawSurface7::Blt argument set was dropped by the decompiler; the copy goes through the seam.
    if (deviceLost) return false;
    if ((x | y) < 0) return false;
    if (deviceViewport[2] < static_cast<std::uint32_t>(x + width)) return false;
    if (deviceViewport[3] < static_cast<std::uint32_t>(y + height)) return false;
    long result = primitiveManager_.Flush();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    const std::uint32_t index = id - 1;
    if (index >= bufferRegions_.size()) return false;
    BufferRegionData& region = bufferRegions_[index];
    if (region.surface == nullptr) return false;
    ID3D7Surface* target = environment_.BufferRegionTarget(static_cast<std::int32_t>(region.type));
    if (target == nullptr) return false;
    struct Rect {
        std::int32_t left, top, right, bottom;
    } dstRect{dx, dy, dx + width, dy + height}, srcRect{x, y, x + width, y + height};
    // The region surface is the blit destination; the frame/back-buffer surface is the source (Blt slot +0x28).
    result = region.surface->Blt(&dstRect, target, &srcRect, 0x1000000, nullptr);
    return result >= 0;
}

bool cGDriver::DrawBufferRegion(std::uint32_t id, int x, int y, int width, int height, int dx, int dy) {
    // PARTIAL: region/frame surface ownership and the lost Blt arguments are handled by ID3D7Environment/ID3D7Surface.
    if (deviceLost) return false;
    if ((x | y) < 0) return false;
    const std::uint32_t right = static_cast<std::uint32_t>(x + width);
    if (deviceViewport[2] < right) return false;
    const std::uint32_t bottom = static_cast<std::uint32_t>(y + height);
    if (deviceViewport[3] < bottom) return false;
    const std::uint32_t index = id - 1;
    if (index >= bufferRegions_.size()) return false;
    BufferRegionData& region = bufferRegions_[index];
    if (region.surface == nullptr) return false;
    if (region.width < static_cast<std::int32_t>(right) || region.height < static_cast<std::int32_t>(bottom)) return false;
    if (environment_.BufferRegionTarget(static_cast<std::int32_t>(region.type)) == nullptr) return false;
    long result = primitiveManager_.Flush();
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    ID3D7Surface* destination = environment_.BufferRegionTarget(static_cast<std::int32_t>(region.type));
    if (destination == nullptr) return false;
    struct Rect {
        std::int32_t left, top, right, bottom;
    } dstRect{dx, dy, dx + width, dy + height}, srcRect{x, y, x + width, y + height};
    result = destination->Blt(&dstRect, region.surface, &srcRect, 0x1000000, nullptr);
    return result >= 0;
}

bool cGDriver::IsBufferRegion(std::uint32_t id) const {
    const std::uint32_t index = id - 1;
    // Quirk kept: the original compares the region count with `<` (not `<=`), so an id one past the end depends on the
    // stale memory after the vector. Here it reports false.
    if (bufferRegions_.size() < index) return false;
    if (index >= bufferRegions_.size()) return false;
    return bufferRegions_[index].index == static_cast<std::int32_t>(index);
}

bool cGDriver::DeleteAllBufferRegions() {
    // PARTIAL: the region surfaces belong to the environment seam, so their Release is not repeated here.
    bufferRegions_.clear();
    // Quirk kept: DeleteAllBufferRegions leaves one entry behind with index 0 (not -1), so IsBufferRegion(1) answers true
    // afterwards and NewBufferRegion appends rather than reusing it.
    BufferRegionData freeEntry;
    freeEntry.index = 0;
    bufferRegions_.push_back(freeEntry);
    return true;
}

void* cGDriver::CopyColorBuffer(int x, int y, int width, int height, void* destination) {
    // PARTIAL: the original locks the render surface, reads its pixel format and converts the pixels into a cIGZBuffer
    // (Create/Lock/Unlock, PixelCopy16_32_* inlined). cIGZBuffer is not recovered, so only the surface state machine and
    // the error bookkeeping are reproduced; the destination is returned unchanged.
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    if (deviceLost) return nullptr;
    primitiveManager_.Flush();
    ID3D7Surface* surface = environment_.RenderTarget();
    if (!surface) return nullptr;
    long result = surface->IsLost();
    if (result == -0x7789fde4) {   // DDERR_WASSTILLDRAWING: the original retries Lock in a loop
        do {
            result = surface->IsLost();
        } while (result == -0x7789fde4);
    }
    if (!copyColorChecked) {
        copyColorAvailable = result >= 0;
        copyColorChecked = true;
    }
    if (!copyColorAvailable) return nullptr;
    SC4_UNWIRED("cGDriver::CopyColorBuffer: the render surface's pixels are not converted into the destination buffer");
    return destination;
}

// ---------------------------------------------------------------------------------------------------------------------
// Device capabilities and default state

bool cGDriver::GetDeviceProperties(std::vector<cDXTextureFormatInfo>& formats) {
    ID3D7Device* device = environment_.Device();
    if (!device) return false;
    device->GetCaps(deviceDescription);
    capStageCount_ = StageCountFromCaps();
    if (capStageCount_ > kMaxStages) capStageCount_ = kMaxStages;
    stageCount = capStageCount_;
    stages.resize(stageCount);   // like the original's vector::resize, existing records keep their state

    ID3D7Surface* surface = environment_.RenderTarget();
    if (!surface) return false;
    sGDPixelFormat pixelFormat;
    long result = surface->GetPixelFormat(pixelFormat);
    if (result == kDeviceLost) {
        error = 0;
    } else if (result != 0) {
        error = 1;
    }
    if (result < 0) return false;

    sGDMode* mode = modes_.MutableMode(static_cast<std::size_t>(modeIndex));
    if (!mode) return false;
    auto* bytes = reinterpret_cast<std::uint8_t*>(mode);
    bytes[0x21] = CapU32(caps::kHasStencil) != 0 ? 1 : 0;
    const std::uint32_t maxTextureSize = CapU16(caps::kMaxTextureSize);
    std::memcpy(bytes + 0x24, &maxTextureSize, 4);
    bytes[0x28] = maxTextureSize > 1 ? 1 : 0;
    bytes[0x2b] = static_cast<std::uint8_t>(CapU32(caps::kRenderBitDepth) >> 7);
    std::uint16_t zero = 0;
    std::memcpy(bytes + 0x2d, &zero, 2);
    std::uint16_t one = 1;
    std::memcpy(bytes + 0x29, &one, 2);
    bytes[0x2f] = static_cast<std::uint8_t>(CapU32(caps::kZBufferBitDepth) & 1);
    bytes[0x30] = 1;
    std::memcpy(bytes + 0x10, &pixelFormat.dwRGBAlphaBitMask, 4);
    std::memcpy(bytes + 0x14, &pixelFormat.dwRBitMask, 4);
    std::memcpy(bytes + 0x18, &pixelFormat.dwGBitMask, 4);
    std::memcpy(bytes + 0x1c, &pixelFormat.dwBBitMask, 4);

    sGDTextureFormatContext context;
    context.formats = &formats;
    result = device->EnumTextureFormats(&SurfaceFormatEnumerator, &context);
    if (result < 0) {
        error = result != kDeviceLost ? 1 : 0;
        return false;
    }
    if (context.compressed) bytes[0x2c] = 1;
    return true;
}

void cGDriver::SetDefaultD3DDeviceState() {
    ID3D7Device* device = environment_.Device();
    if (!device) return;
    device->SetRenderState(rs::CullMode, 1);
    device->SetRenderState(rs::DitherEnable, 1);
    device->SetRenderState(0x29, 0);
    device->SetRenderState(rs::SpecularEnable, 0);
    device->SetRenderState(rs::AlphaBlendEnable, 0);
    device->SetRenderState(rs::TexturePerspective, 1);
    device->SetRenderState(rs::ZWriteEnable, 1);
    device->SetRenderState(rs::ZFunc, 4);
    device->SetRenderState(rs::AlphaTestEnable, 0);
    device->SetRenderState(rs::FogEnable, 0);
    device->SetRenderState(rs::FillMode, 3);
    device->SetRenderState(rs::StencilEnable, 0);
    device->SetRenderState(rs::StencilFail, 1);
    device->SetRenderState(rs::StencilZFail, 6);
    device->SetRenderState(rs::StencilPass, 1);
    device->SetRenderState(rs::StencilFunc, 8);
    device->SetRenderState(rs::StencilRef, 1);
    device->SetRenderState(rs::StencilMask, 1);
    device->SetRenderState(rs::StencilWriteMask, 1);
    device->SetRenderState(rs::TextureFactor, 0xffffffffu);
    device->SetRenderState(rs::Ambient, 0);
    device->SetRenderState(rs::Lighting, 0);
    device->SetRenderState(rs::AmbientMaterialSource, 1);
    device->SetRenderState(rs::DiffuseMaterialSource, 1);
    device->SetRenderState(rs::SpecularMaterialSource, 0);
    device->SetRenderState(rs::ColorVertex, 1);
    lightingEnabled = false;
    clearOnBeginScene = false;
    vertexColorEnabled = true;
    vertexAlphaEnabled = true;
    // Texture stage 0: select the texture, modulate with the current colour, point-sampled, repeat.
    device->SetTextureStageState(0, 1, 2);
    device->SetTextureStageState(0, 0xc, 1);
    device->SetTextureStageState(0, 0xb, 0);
    device->SetTextureStageState(0, 0x10, 1);
    device->SetTextureStageState(0, 0x11, 1);
    device->SetTextureStageState(0, 0x12, 1);
    device->SetTextureStageState(0, 2, 2);
    device->SetTextureStageState(0, 3, 0);
    device->SetTextureStageState(0, 4, 2);
    device->SetTextureStageState(0, 5, 2);
    device->SetTextureStageState(0, 6, 0);
    device->SetTextureStageState(1, 1, 2);
    device->SetTexture(0, 0);
    stagesDirty = true;
    for (std::size_t i = 0; i < stages.size(); ++i) stages[i].dirty = 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Viewport / display environment. These bring up the D3D7 device through cISGLDX7D3DX; the actual context creation is the
// environment seam (CreateSGLD3DXContext), so the functions are recovered-partial.

void cGDriver::InitViewport() {
    // PARTIAL: cISGLDX7D3DX::CreateContextInfoOnly + IDirectDraw7::GetDeviceIdentifier are modelled by the environment
    // seam; the two intermediate libc++ string temporaries of the original are not reproduced byte-for-byte.
    modes_.Clear();
    if (!environment_.CreateContextInfoOnly()) {
        error = 6;
        return;
    }
    sGDDriverInfo info;
    if (!environment_.GetDriverInfo(info)) return;
    driverInfo = kDriverInfoPrefix;
    driverInfo.push_back('\n');
    std::uint32_t guid1 = 0;
    std::uint16_t guid2 = 0, guid3 = 0;
    std::memcpy(&guid1, info.guid + 0, 4);
    std::memcpy(&guid2, info.guid + 4, 2);
    std::memcpy(&guid3, info.guid + 6, 2);
    char line[512];
    std::snprintf(line, sizeof line, "%u.%u.%u.%u, GUID: %X-%X-%X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
                  (info.versionLow >> 16) & 0xffff, info.versionLow & 0xffff, (info.versionHigh >> 16) & 0xffff,
                  info.versionHigh & 0xffff, guid1, static_cast<unsigned>(guid2), static_cast<unsigned>(guid3),
                  info.guid[8], info.guid[9], info.guid[10], info.guid[11], info.guid[12], info.guid[13],
                  info.guid[14], info.guid[15]);
    driverInfo += line;
    driverInfo += info.driverName;
    driverInfo.push_back('\n');
    std::snprintf(line, sizeof line, "Vendor: %04x, Device: %04x, Board: %04x, Chipset: %04x\n",
                  info.vendorId & 0xffff, info.deviceId & 0xffff, info.subSysId & 0xffff, info.revision & 0xffff);
    driverInfo += line;
}

bool cGDriver::SetupD3DX() {
    // PARTIAL: window/client rects and the current display mode come from the environment; the DDSURFACEDESC2 the
    // original builds for cISGLDX7D3DX::CreateContext is not recoverable from the decompilation.
    if (!environment_.CreateContext(nullptr, windowed, vsync, currentBitsPerPixel,
                                    static_cast<std::int32_t>(currentMode.width),
                                    static_cast<std::int32_t>(currentMode.height))) {
        error = 6;
        return false;
    }
    ID3D7Device* device = environment_.Device();
    if (!device) return false;
    device->GetCaps(deviceDescription);
    return true;
}

bool cGDriver::Initialize3DEnvironment() {
    // PARTIAL: uses the device/context created by SetupD3DX. The original also reads the window and client rectangles and
    // builds the orthographic projection from them; the seam supplies the viewport instead.
    if (!SetupD3DX()) {
        error = 6;
        return false;
    }
    ID3D7Device* device = environment_.Device();
    if (!device) return false;
    std::uint32_t viewportOut[4] = {0, 0, 0, 0};
    device->GetViewport(viewportOut);
    std::memcpy(deviceViewport, viewportOut, sizeof(deviceViewport));
    std::memcpy(viewport, viewportOut, sizeof(viewport));
    windowRect[0] = static_cast<std::int32_t>(viewportOut[0]);
    windowRect[1] = static_cast<std::int32_t>(viewportOut[1]);
    windowRect[2] = static_cast<std::int32_t>(viewportOut[0] + viewportOut[2]);
    windowRect[3] = static_cast<std::int32_t>(viewportOut[1] + viewportOut[3]);
    // Projection: 2 / width, -2 / height, -1..1 in z, as the original builds it.
    float projection[16] = {0};
    projection[0] = 2.0f / static_cast<float>(viewportOut[2] ? viewportOut[2] : 1);
    projection[5] = -2.0f / static_cast<float>(viewportOut[3] ? viewportOut[3] : 1);
    projection[10] = -1.0f;
    projection[12] = -1.0f;
    projection[13] = 1.0f;
    projection[15] = 1.0f;
    device->SetTransform(2, nullptr);   // the original flushes with SetTransform(2) before switching matrix targets
    MatrixMode(0);
    LoadIdentity();
    MatrixMode(1);
    LoadMatrix(projection);
    MatrixMode(0);
    Clear(0x7000);
    return true;
}

bool cGDriver::SetupViewport() {
    // PARTIAL: composes the partial Initialize3DEnvironment / InitDevice seam path.
    EndScene();
    if (!Initialize3DEnvironment()) return false;
    std::vector<cDXTextureFormatInfo> formats;
    if (!GetDeviceProperties(formats)) return false;
    if (modeIndex >= 0 && static_cast<std::size_t>(modeIndex) < modes_.Modes().size())
        currentMode = modes_.Modes()[static_cast<std::size_t>(modeIndex)];
    textureManager_.Init(this, environment_.Device(), formats);
    primitiveManager_.Init(this, environment_.Device());
    SetDefaultD3DDeviceState();
    environment_.GetWindowRect(windowRect);
    texturesReady = true;
    return true;
}

void cGDriver::DestroyViewport() {
    // PARTIAL: releases the D3D7 environment through the seam.
    EndScene();
    ID3D7Device* device = environment_.Device();
    if (device) {
        for (std::uint32_t s = 0; s < stageCount; ++s) device->SetTexture(s, 0);
    }
    stageCount = 0;
    stages.clear();
    textureManager_.Shutdown();
    primitiveManager_.Shutdown();
    Cleanup3DEnvironment();
    texturesReady = false;
    environment_.ReleaseDisplay();
}

void cGDriver::ShutdownViewport() {
    // PARTIAL: the device/context release is the environment seam.
    SetVideoMode(-1, nullptr, false, false);
    modes_.Clear();
    modeIndex = -1;
    environment_.DestroyContext();
}

void cGDriver::Cleanup3DEnvironment() {
    // PARTIAL: releases the environment through the seam.
    ID3D7Device* device = environment_.Device();
    if (device && stageCount != 0) {
        device->SetTexture(0, 0);
        for (std::uint32_t s = 1; s < stageCount; ++s) device->SetTexture(s, 0);
    }
    DeleteAllBufferRegions();
    environment_.DestroyContext();
}

const void* cGDriver::GetDriverInfo() { return driverInfo.c_str(); }

std::uint32_t cGDriver::GetGZCLSID() const { return 0xbadb6906; }

// ---------------------------------------------------------------------------------------------------------------------
// Flush / device-lost handling

void cGDriver::Flush() {
    // PARTIAL: the original also notifies the message server (cRZMessage2Standard with ids 0xaacb92a3 / 0x8acaa95f /
    // 0x6acb92a9) around a device reset; the message-server seam is not part of this class. The device state machine
    // itself is reproduced: cooperative-level test, scene flush, lost-device switch-over and default-state restore.
    const bool cooperative = environment_.TestCooperativeLevel();
    if (!cooperative) {
        if (deviceLost) return;
        deviceLost = true;
        textureManager_.ShutdownDevice();
        primitiveManager_.Stop();
        environment_.DestroyContext();
        return;
    }
    if (!deviceLost) {
        const long result = primitiveManager_.FlushScene();
        if (result < 0) error = result != kDeviceLost ? 1 : 0;
        EndScene();
        if (!primitiveManager_.IsStarted()) primitiveManager_.Restart(this, environment_.Device());
        BeginScene();
        return;
    }
    if (!environment_.RestoreDevice()) {
        EndScene();
        return;
    }
    deviceLost = false;
    SC4_UNWIRED("cGDriver::Flush: the device-reset messages (0xaacb92a3 / 0x8acaa95f / 0x6acb92a9) are not sent to the message server");
    SetDefaultD3DDeviceState();
    if (!primitiveManager_.IsStarted()) primitiveManager_.Restart(this, environment_.Device());
    textureManager_.InitDevice(environment_.Device());
}

}  // namespace nSGLDX7
