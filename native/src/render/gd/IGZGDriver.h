// cIGZGDriver — SC4's engine-level graphics driver interface (GZCOM class; implemented in the shipped game by
// nSGLDX7::cGDriver, vtable group 3 of 0x100ad53e0, 83 slots). It is GL-1.x shaped. Slot order below is the binary's, so
// "vtable offset = 8 * slot" matches what the decompilation shows (e.g. SetViewport(x,y,w,h) = slot 75 = +0x258).
// The native Metal driver (cMetalGDriver) implements exactly this; everything above it (SC4DrawContext, cSC43DRender, the UI
// draw context) is unchanged game code.
//
// Enumerations are carried as plain 32-bit values with named constants (the game passes them straight through tables).
#pragma once
#include <cstdint>

namespace gd {

using eGDPrimType = std::uint32_t;
namespace prim { enum : eGDPrimType { Triangles = 0, TriangleStrip = 1, TriangleFan = 2, Points = 3, Lines = 4, LineStrip = 5, Quads = 6, QuadStrip = 7 }; }

using eGDTestFunc = std::uint32_t;   // 0..7
namespace test { enum : eGDTestFunc { Never = 0, Less, Equal, LEqual, Greater, NotEqual, GEqual, Always }; }

using eGDBlend = std::uint32_t;      // 0..10
namespace blend { enum : eGDBlend { Zero = 0, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha, DstColor, InvDstColor, SrcAlphaSat }; }

using eGDDriverState = std::uint32_t;
namespace state { enum : eGDDriverState { AlphaTest = 0, DepthTest = 1, Stencil = 2, CullFace = 3, Blend = 4, Texturing = 5, Fog = 6, Unused7 = 7 }; }

using eGDMatrixTarget = std::uint32_t;   // 0 modelview, 1 projection (the UI context loads target 1)
using eGDVertexFormat = std::uint32_t;   // 1,2,3,10,11,0x20..0x26 predefined; negative (sign bit) = packed
namespace vf { enum : eGDVertexFormat { V3F_C4UB = 1, V3F_T2F = 2, V3F_C4UB_T2F = 10, V3F_C4UB_2T2F = 11 }; }

using eGDStencilOp = std::uint32_t;
using eGDShade = std::uint32_t;
using eGDTextureTarget = std::uint32_t;
using eGDInternalTextureFormat = std::uint32_t;
using eGDTextureFormat = std::uint32_t;
using eGDType = std::uint32_t;
using eGDParameter = std::uint32_t;
using eGDTextureEnvTarget = std::uint32_t;
using eGDTextureEnvParamType = std::uint32_t;
using eGDTextureEnvModeParam = std::uint32_t;
using eGDTextureParamType = std::uint32_t;
using eGDTextureParam = std::uint32_t;
using eGDFogParamType = std::uint32_t;
using eGDFogParam = std::uint32_t;
using eGDTexCoordSource = std::uint32_t;
using eGDTexMatFlags = std::uint32_t;
enum eGDTextureStageCombineParamType : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineModeParam : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineSourceParamType : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineSourceParam : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineOperandParamType : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineScaleParamType : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
enum eGDTextureStageCombineScaleParam : std::uint32_t {};   // distinct types: the four TexStageCombine overloads differ only by these
using eGDTextureHintFlags = std::uint32_t;
using eGDElementType = std::uint32_t;

struct sGDMode;      // video mode description (layout recovered with CountVideoModes / GetVideoModeInfo)
struct cGDCombiner;  // texture combiner description (layout recovered with SetCombiner)
class cIGZBuffer;

class cIGZGDriver {
public:
    virtual bool QueryInterface(std::uint32_t iid, void** out) = 0;                                                     // 0
    virtual void AddRef() = 0;                                                                                          // 1
    virtual std::int32_t Release() = 0;                                                                                 // 2
    virtual void DrawArrays(eGDPrimType mode, int first, int count) = 0;                                                // 3
    virtual void DrawElements(eGDPrimType mode, int count, eGDType indexType, const void* indices) = 0;                 // 4
    virtual void InterleavedArrays(eGDVertexFormat format, int stride, const void* data) = 0;                           // 5
    virtual eGDVertexFormat MakeVertexFormat(std::uint32_t count, eGDElementType* elements) = 0;                        // 6
    virtual eGDVertexFormat MakeVertexFormat(eGDVertexFormat format) = 0;                                               // 7
    virtual int VertexFormatStride(eGDVertexFormat format) const = 0;                                                   // 8
    virtual int VertexFormatElementOffset(eGDVertexFormat format, eGDElementType element, std::uint32_t index) const = 0;   // 9
    virtual int VertexFormatNumElements(eGDVertexFormat format, eGDElementType element) const = 0;                      // 10
    virtual void Clear(std::uint32_t mask) = 0;                                                                         // 11
    virtual void ClearColor(float r, float g, float b, float a) = 0;                                                    // 12
    virtual void ClearDepth(double depth) = 0;                                                                          // 13
    virtual void ClearStencil(int value) = 0;                                                                           // 14
    virtual void ColorMask(bool enable) = 0;                                                                            // 15
    virtual void DepthFunc(eGDTestFunc func) = 0;                                                                       // 16
    virtual void DepthMask(bool enable) = 0;                                                                            // 17
    virtual void StencilFunc(eGDTestFunc func, int ref, std::uint32_t mask) = 0;                                        // 18
    virtual void StencilMask(std::uint32_t mask) = 0;                                                                   // 19
    virtual void StencilOp(eGDStencilOp fail, eGDStencilOp zfail, eGDStencilOp zpass) = 0;                              // 20
    virtual void BlendFunc(eGDBlend src, eGDBlend dst) = 0;                                                             // 21
    virtual void AlphaFunc(eGDTestFunc func, float ref) = 0;                                                            // 22
    virtual void ShadeModel(eGDShade model) = 0;                                                                        // 23
    virtual void BindTexture(eGDTextureTarget target, std::uint32_t texture) = 0;                                       // 24
    virtual void TexImage2D(eGDTextureTarget target, int level, eGDInternalTextureFormat internalFormat, int width, int height, int border, eGDTextureFormat format, eGDType type, const void* pixels) = 0;   // 25
    virtual void PixelStore(eGDParameter pname, int value) = 0;                                                         // 26
    virtual void TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, eGDTextureEnvModeParam mode) = 0;     // 27
    virtual void TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, const float* values) = 0;             // 28
    virtual void TexParameter(eGDTextureTarget target, eGDTextureParamType param, eGDTextureParam value) = 0;           // 29
    virtual void Fog(eGDFogParamType param, eGDFogParam value) = 0;                                                     // 30
    virtual void Fog(eGDFogParamType param, const float* values) = 0;                                                   // 31
    virtual void ColorMultiplier(float r, float g, float b) = 0;                                                        // 32
    virtual void AlphaMultiplier(float a) = 0;                                                                          // 33
    virtual void EnableVertexColors(bool a, bool b) = 0;                                                                // 34
    virtual void GenTextures(int count, std::uint32_t* ids) = 0;                                                        // 35
    virtual void DeleteTextures(int count, const std::uint32_t* ids) = 0;                                               // 36
    virtual bool IsTexture(std::uint32_t texture) = 0;                                                                  // 37
    virtual void PrioritizeTextures(int count, const std::uint32_t* ids, const float* priorities) = 0;                  // 38
    virtual bool AreTexturesResident(int count, const std::uint32_t* ids, bool* residence) = 0;                         // 39
    virtual void MatrixMode(eGDMatrixTarget target) = 0;                                                                // 40
    virtual void LoadMatrix(const float* m) = 0;                                                                        // 41
    virtual void LoadIdentity() = 0;                                                                                    // 42
    virtual void Flush() = 0;                                                                                           // 43
    virtual void Enable(eGDDriverState state) = 0;                                                                      // 44
    virtual void Disable(eGDDriverState state) = 0;                                                                     // 45
    virtual bool IsEnabled(eGDDriverState state) = 0;                                                                   // 46
    virtual void GetBoolean(eGDParameter pname, bool* out) = 0;                                                         // 47
    virtual void GetInteger(eGDParameter pname, int* out) = 0;                                                          // 48
    virtual void GetFloat(eGDParameter pname, float* out) = 0;                                                          // 49
    virtual int GetError() = 0;                                                                                         // 50
    virtual void TexStage(std::uint32_t stage) = 0;                                                                     // 51
    virtual void TexStageCoord(eGDTexCoordSource source) = 0;                                                           // 52
    virtual void TexStageMatrix(const float* m, std::uint32_t a, std::uint32_t b, eGDTexMatFlags flags) = 0;            // 53
    virtual void TexStageCombine(eGDTextureStageCombineParamType param, eGDTextureStageCombineModeParam mode) = 0;      // 54
    virtual void TexStageCombine(eGDTextureStageCombineSourceParamType param, eGDTextureStageCombineSourceParam source) = 0;   // 55
    virtual void TexStageCombine(eGDTextureStageCombineOperandParamType param, eGDBlend operand) = 0;                   // 56
    virtual void TexStageCombine(eGDTextureStageCombineScaleParamType param, eGDTextureStageCombineScaleParam scale) = 0;   // 57
    virtual void SetTexture(std::uint32_t texture, std::uint32_t stage) = 0;                                            // 58
    virtual std::uint32_t GetTexture(std::uint32_t stage) const = 0;                                                    // 59
    virtual std::uint32_t CreateTexture(eGDInternalTextureFormat format, std::uint32_t a, std::uint32_t b, std::uint32_t c, eGDTextureHintFlags hint) = 0;   // 60
    virtual void LoadTextureLevel(std::uint32_t texture, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, eGDTextureFormat format, eGDType type, std::uint32_t rowLength, const void* pixels) = 0;   // 61
    virtual void SetCombiner(const cGDCombiner& combiner, std::uint32_t stage) = 0;                                     // 62
    virtual int CountVideoModes() = 0;                                                                                  // 63
    virtual bool GetVideoModeInfo(std::uint32_t index, sGDMode& mode) = 0;                                              // 64
    virtual bool GetVideoModeInfo(sGDMode& mode) = 0;                                                                   // 65
    virtual bool SetVideoMode(int mode, void* window, bool windowed, bool vsync) = 0;                                   // 66
    virtual void PolygonOffset(std::uint32_t units) = 0;                                                                // 67
    virtual void BitBlt(int x, int y, int w, int h, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra) = 0;   // 68
    virtual void StretchBlt(int x, int y, int w, int h, int dw, int dh, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra) = 0;   // 69
    virtual void BitBltAlpha(int x, int y, int w, int h, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra, std::uint32_t alpha) = 0;   // 70
    virtual void StretchBltAlpha(int x, int y, int w, int h, int dw, int dh, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra, std::uint32_t alpha) = 0;   // 71
    virtual void BitBltAlphaModulate(int x, int y, int w, int h, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra, std::uint32_t alpha) = 0;   // 72
    virtual void StretchBltAlphaModulate(int x, int y, int w, int h, int dw, int dh, eGDTextureFormat format, eGDType type, const void* pixels, bool flip, const void* extra, std::uint32_t alpha) = 0;   // 73
    virtual void SetViewport() = 0;                                                                                     // 74
    virtual void SetViewport(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) = 0;                   // 75
    virtual void GetViewport(std::uint32_t out[4]) = 0;                                                                 // 76  (x, y, x+w, y+h in the binary)
    virtual const void* GetDriverInfo() = 0;                                                                            // 77
    virtual std::uint32_t GetGZCLSID() const = 0;                                                                       // 78
    virtual bool Init() = 0;                                                                                            // 79
    virtual bool Shutdown() = 0;                                                                                        // 80
    virtual bool IsDeviceReady() = 0;                                                                                   // 81
    virtual bool Punt(std::uint32_t code, void* data) = 0;                                                              // 82
    virtual ~cIGZGDriver() = default;
};

}  // namespace gd
